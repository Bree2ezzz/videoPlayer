#include "d3d11renderer.h"

#include "app_logger.h"
#include "d3d11context.h"

#include <QMetaObject>
#include <QPaintEngine>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QString>
#include <QWidget>

#include <d3dcompiler.h>
#include <dxgi1_2.h>

extern "C" {
#include <libavutil/pixfmt.h>
}

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>

namespace {

template <typename T>
void releaseCom(T*& object)
{
    if (object) {
        object->Release();
        object = nullptr;
    }
}

class DeviceContextLock
{
public:
    explicit DeviceContextLock(D3D11Context* context)
        : context_(context)
    {
        if (context_) {
            context_->lock();
        }
    }

    ~DeviceContextLock()
    {
        if (context_) {
            context_->unlock();
        }
    }

private:
    D3D11Context* context_ = nullptr;
};

struct ColorConstants {
    float offset[4];
    float row0[4];
    float row1[4];
    float row2[4];
};

constexpr char kShaderSource[] = R"(
struct VsOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VsOutput VSMain(uint vertexId : SV_VertexID) {
    const float2 positions[4] = {
        float2(-1.0, -1.0), float2(-1.0, 1.0),
        float2(1.0, -1.0), float2(1.0, 1.0)
    };
    const float2 uvs[4] = {
        float2(0.0, 1.0), float2(0.0, 0.0),
        float2(1.0, 1.0), float2(1.0, 0.0)
    };
    VsOutput output;
    output.position = float4(positions[vertexId], 0.0, 1.0);
    output.uv = uvs[vertexId];
    return output;
}

Texture2D texY : register(t0);
Texture2D texUV : register(t1);
SamplerState linearSampler : register(s0);
cbuffer ColorTransform : register(b0) {
    float4 offset;
    float4 row0;
    float4 row1;
    float4 row2;
};

float4 PSMain(VsOutput input) : SV_Target {
    const float y = texY.Sample(linearSampler, input.uv).r + offset.x;
    const float u = texUV.Sample(linearSampler, input.uv).r + offset.y;
    const float v = texUV.Sample(linearSampler, input.uv).g + offset.z;
    const float3 yuv = float3(y, u, v);
    return float4(dot(row0.xyz, yuv), dot(row1.xyz, yuv), dot(row2.xyz, yuv), 1.0);
}
)";

bool compileShader(const char* entryPoint, const char* target, ID3DBlob** blob)
{
    ID3DBlob* errors = nullptr;
    const HRESULT hr = D3DCompile(kShaderSource,
                                  sizeof(kShaderSource) - 1,
                                  nullptr,
                                  nullptr,
                                  nullptr,
                                  entryPoint,
                                  target,
                                  0,
                                  0,
                                  blob,
                                  &errors);
    if (FAILED(hr)) {
        const char* message = errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown error";
        VP_ERROR("D3DCompile entry={} target={} hr=0x{:08x} message={}",
                 entryPoint, target, static_cast<unsigned long>(hr), message);
        releaseCom(errors);
        return false;
    }
    releaseCom(errors);
    return true;
}
bool isSupportedVideoTextureFormat(DXGI_FORMAT format)
{
    return format == DXGI_FORMAT_NV12 || format == DXGI_FORMAT_P010;
}

ColorConstants colorConstantsForFrame(const AVFrame* frame, DXGI_FORMAT textureFormat)
{
    const bool tenBit = textureFormat == DXGI_FORMAT_P010;
    const float containerMax = tenBit ? 65535.0f : 255.0f;
    const float storageScale = tenBit ? 64.0f : 1.0f;
    const float signalMax = tenBit ? 1023.0f : 255.0f;
    const float lumaBlack = tenBit ? 64.0f : 16.0f;
    const float lumaSpan = tenBit ? 876.0f : 219.0f;
    const float chromaCenter = tenBit ? 512.0f : 128.0f;
    const float chromaSpan = tenBit ? 896.0f : 224.0f;
    const bool fullRange = frame && frame->color_range == AVCOL_RANGE_JPEG;

    // P010 stores the ten meaningful bits in the upper bits of a 16-bit UNORM sample.
    const float lumaScale = fullRange
                                ? containerMax / (signalMax * storageScale)
                                : containerMax / (lumaSpan * storageScale);
    const float chromaScale = fullRange
                                  ? containerMax / (signalMax * storageScale)
                                  : containerMax / (chromaSpan * storageScale);
    const float lumaOffset = fullRange ? 0.0f : -(lumaBlack * storageScale) / containerMax;
    const float chromaOffset = -(chromaCenter * storageScale) / containerMax;

    float redU = 0.0f;
    float redV = 1.402f;
    float greenU = -0.344136f;
    float greenV = -0.714136f;
    float blueU = 1.772f;
    float blueV = 0.0f;

    const AVColorSpace colorspace = frame ? frame->colorspace : AVCOL_SPC_UNSPECIFIED;
    const bool unspecifiedHd = colorspace == AVCOL_SPC_UNSPECIFIED && frame &&
                               (frame->width >= 1280 || frame->height > 576);
    if (colorspace == AVCOL_SPC_BT2020_NCL) {
        redV = 1.474600f;
        greenU = -0.164553f;
        greenV = -0.571353f;
        blueU = 1.881400f;
    } else if (colorspace == AVCOL_SPC_BT709 || unspecifiedHd) {
        redV = 1.574800f;
        greenU = -0.187324f;
        greenV = -0.468124f;
        blueU = 1.855600f;
    }

    ColorConstants constants = {};
    constants.offset[0] = lumaOffset;
    constants.offset[1] = chromaOffset;
    constants.offset[2] = chromaOffset;
    constants.row0[0] = lumaScale;
    constants.row0[1] = redU * chromaScale;
    constants.row0[2] = redV * chromaScale;
    constants.row1[0] = lumaScale;
    constants.row1[1] = greenU * chromaScale;
    constants.row1[2] = greenV * chromaScale;
    constants.row2[0] = lumaScale;
    constants.row2[1] = blueU * chromaScale;
    constants.row2[2] = blueV * chromaScale;
    return constants;
}
} // namespace

class D3D11RenderSurface final : public QWidget
{
public:
    D3D11RenderSurface(D3D11Renderer* renderer, QWidget* parent)
        : QWidget(parent), renderer_(renderer)
    {
        setAttribute(Qt::WA_NativeWindow);
        setAttribute(Qt::WA_PaintOnScreen);
        setAttribute(Qt::WA_OpaquePaintEvent);
        setAutoFillBackground(false);
    }

    void detachRenderer() { renderer_ = nullptr; }

protected:
    QPaintEngine* paintEngine() const override { return nullptr; }

    void paintEvent(QPaintEvent* event) override
    {
        Q_UNUSED(event);
    }

    void resizeEvent(QResizeEvent* event) override
    {
        QWidget::resizeEvent(event);
        if (renderer_) {
            renderer_->resizeSwapChain(event->size().width(), event->size().height());
        }
    }

private:
    D3D11Renderer* renderer_ = nullptr;
};

D3D11Renderer::D3D11Renderer(D3D11Context* context, QWidget* parent)
    : context_(context),
      surface_(new D3D11RenderSurface(this, parent)),
      pendingFrame_(av_frame_alloc())
{
    if (!context_ || !context_->device() || !context_->immediateContext() || !pendingFrame_) {
        VP_ERROR("D3D11Renderer cannot initialize context={} device={} frame={}",
                 static_cast<const void*>(context_),
                 context_ ? static_cast<const void*>(context_->device()) : nullptr,
                 static_cast<const void*>(pendingFrame_));
        return;
    }

    surface_->winId();
    if (!initializeSwapChain() || !initializePipeline()) {
        return;
    }

    initialized_.store(true);
    VP_INFO("D3D11Renderer created renderer={} surface={} device={}",
            static_cast<const void*>(this),
            static_cast<const void*>(surface_),
            static_cast<const void*>(context_->device()));
}

D3D11Renderer::~D3D11Renderer()
{
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        if (pendingFrame_) {
            av_frame_free(&pendingFrame_);
        }
        presentQueued_ = false;
    }

    if (context_) {
        DeviceContextLock lock(context_);
        releaseSizeDependentResources();
        releasePipelineResources();
        releaseCom(swapChain_);
    }

    if (surface_) {
        surface_->detachRenderer();
        delete surface_;
        surface_ = nullptr;
    }
    VP_INFO("D3D11Renderer destroyed renderer={}", static_cast<const void*>(this));
}

void D3D11Renderer::renderFrame(AVFrame* frame)
{
    if (!frame || frame->format != AV_PIX_FMT_D3D11 || !initialized_.load()) {
        if (frame) {
            VP_WARN("D3D11Renderer rejected frame format={} initialized={}",
                    frame->format, initialized_.load());
        }
        return;
    }

    bool shouldQueue = false;
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        av_frame_unref(pendingFrame_);
        if (av_frame_ref(pendingFrame_, frame) < 0) {
            VP_ERROR("D3D11Renderer av_frame_ref failed");
            return;
        }
        pendingSessionId_ = activeSessionId_;
        pendingSessionUrl_ = activeSessionUrl_;
        clearRequested_ = false;
        queuePresentLocked(&shouldQueue);
    }
    if (shouldQueue) {
        requestQueuedPresent();
    }
}

void D3D11Renderer::clear()
{
    bool shouldQueue = false;
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        av_frame_unref(pendingFrame_);
        clearRequested_ = true;
        queuePresentLocked(&shouldQueue);
    }
    if (shouldQueue) {
        requestQueuedPresent();
    }
}

QWidget* D3D11Renderer::asWidget()
{
    return surface_;
}

int D3D11Renderer::preferredPixelFormat() const
{
    return AV_PIX_FMT_D3D11;
}

void D3D11Renderer::setUnsupportedFormatCallback(UnsupportedFormatCallback callback)
{
    unsupportedFormatCallback_ = std::move(callback);
}

void D3D11Renderer::setPlaybackSession(unsigned long long sessionId, const QUrl& url)
{
    std::lock_guard<std::mutex> lock(pendingMutex_);
    activeSessionId_ = sessionId;
    activeSessionUrl_ = url;
}

void D3D11Renderer::resizeSwapChain(int width, int height)
{
    if (!initialized_.load() || !swapChain_ || width <= 0 || height <= 0) {
        return;
    }

    DeviceContextLock lock(context_);
    ID3D11DeviceContext* immediate = context_->immediateContext();
    immediate->OMSetRenderTargets(0, nullptr, nullptr);
    releaseCom(renderTargetView_);
    const HRESULT hr = swapChain_->ResizeBuffers(0,
                                                  static_cast<UINT>(width),
                                                  static_cast<UINT>(height),
                                                  DXGI_FORMAT_UNKNOWN,
                                                  0);
    if (FAILED(hr)) {
        VP_ERROR("D3D11 ResizeBuffers failed {}x{} hr=0x{:08x}",
                 width, height, static_cast<unsigned long>(hr));
        return;
    }
    if (!createRenderTarget()) {
        VP_ERROR("D3D11Renderer failed to recreate render target after resize");
        return;
    }
    clearBackBuffer();
    swapChain_->Present(1, 0);
}

void D3D11Renderer::present()
{
    AVFrame* frame = av_frame_alloc();
    unsigned long long frameSessionId = 0;
    QUrl frameSessionUrl;
    bool clearOnly = false;
    bool shouldQueueAgain = false;
    if (!frame) {
        VP_ERROR("D3D11Renderer could not allocate presentation frame");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        clearOnly = clearRequested_;
        clearRequested_ = false;
        av_frame_move_ref(frame, pendingFrame_);
        frameSessionId = pendingSessionId_;
        frameSessionUrl = pendingSessionUrl_;
        presentQueued_ = false;
    }

    if (initialized_.load()) {
        DeviceContextLock lock(context_);
        if (clearOnly || !frame->data[0]) {
            clearBackBuffer();
            if (swapChain_) {
                swapChain_->Present(1, 0);
            }
        } else {
            ID3D11Texture2D* sourceTexture = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
            const UINT sourceSubresource = static_cast<UINT>(reinterpret_cast<uintptr_t>(frame->data[1]));
            D3D11_TEXTURE2D_DESC sourceDesc = {};
            if (sourceTexture) {
                sourceTexture->GetDesc(&sourceDesc);
            }
            if (frame->colorspace == AVCOL_SPC_BT2020_CL) {
                reportFallback(frameSessionId,
                               frameSessionUrl,
                               QStringLiteral("BT.2020 constant-luminance video requires Software rendering"));
            } else if (!sourceTexture || !isSupportedVideoTextureFormat(sourceDesc.Format)) {
                reportUnsupportedFormat(frameSessionId, frameSessionUrl, sourceDesc.Format);
            } else if (frame->width <= 0 || frame->height <= 0 ||
                       static_cast<UINT>(frame->width) > sourceDesc.Width ||
                       static_cast<UINT>(frame->height) > sourceDesc.Height ||
                       !ensureVideoTexture(frame->width, frame->height, sourceDesc.Format)) {
                VP_ERROR("D3D11Renderer cannot prepare texture format={} source={} size={}x{} source_size={}x{}",
                         static_cast<unsigned int>(sourceDesc.Format),
                         static_cast<const void*>(sourceTexture),
                         frame->width,
                         frame->height,
                         sourceDesc.Width,
                         sourceDesc.Height);
                reportUnsupportedFormat(frameSessionId, frameSessionUrl, sourceDesc.Format);
            } else {
                ID3D11DeviceContext* immediate = context_->immediateContext();
                D3D11_BOX sourceBox = {};
                sourceBox.right = static_cast<UINT>(frame->width);
                sourceBox.bottom = static_cast<UINT>(frame->height);
                sourceBox.back = 1;
                immediate->CopySubresourceRegion(sampledYuvTexture_,
                                                 0,
                                                 0,
                                                 0,
                                                 0,
                                                 sourceTexture,
                                                 sourceSubresource,
                                                 &sourceBox);

                D3D11_VIEWPORT viewport = {};
                const float outputWidth = static_cast<float>(std::max(surface_->width(), 1));
                const float outputHeight = static_cast<float>(std::max(surface_->height(), 1));
                const float sourceAspect = static_cast<float>(frame->width) / static_cast<float>(frame->height);
                const float outputAspect = outputWidth / outputHeight;
                viewport.Width = outputWidth;
                viewport.Height = outputHeight;
                if (sourceAspect > outputAspect) {
                    viewport.Height = outputWidth / sourceAspect;
                    viewport.TopLeftY = (outputHeight - viewport.Height) * 0.5f;
                } else {
                    viewport.Width = outputHeight * sourceAspect;
                    viewport.TopLeftX = (outputWidth - viewport.Width) * 0.5f;
                }
                viewport.MinDepth = 0.0f;
                viewport.MaxDepth = 1.0f;

                clearBackBuffer();
                const ColorConstants constants = colorConstantsForFrame(frame, sourceDesc.Format);
                immediate->UpdateSubresource(colorConstants_, 0, nullptr, &constants, 0, 0);
                ID3D11ShaderResourceView* shaderResources[] = {lumaSrv_, chromaSrv_};
                ID3D11Buffer* constantBuffers[] = {colorConstants_};
                immediate->OMSetRenderTargets(1, &renderTargetView_, nullptr);
                immediate->RSSetViewports(1, &viewport);
                immediate->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
                immediate->IASetInputLayout(nullptr);
                immediate->VSSetShader(vertexShader_, nullptr, 0);
                immediate->PSSetShader(pixelShader_, nullptr, 0);
                immediate->PSSetSamplers(0, 1, &sampler_);
                immediate->PSSetShaderResources(0, 2, shaderResources);
                immediate->PSSetConstantBuffers(0, 1, constantBuffers);
                immediate->Draw(4, 0);

                ID3D11ShaderResourceView* nullResources[] = {nullptr, nullptr};
                immediate->PSSetShaderResources(0, 2, nullResources);
                swapChain_->Present(1, 0);
            }
        }
    }
    av_frame_free(&frame);

    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        if (pendingFrame_->data[0] || clearRequested_) {
            queuePresentLocked(&shouldQueueAgain);
        }
    }
    if (shouldQueueAgain) {
        requestQueuedPresent();
    }
}

bool D3D11Renderer::initializeSwapChain()
{
    DeviceContextLock lock(context_);
    IDXGIDevice* dxgiDevice = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory2* factory = nullptr;
    HRESULT hr = context_->device()->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    if (SUCCEEDED(hr)) {
        hr = dxgiDevice->GetAdapter(&adapter);
    }
    if (SUCCEEDED(hr)) {
        hr = adapter->GetParent(IID_PPV_ARGS(&factory));
    }
    if (FAILED(hr)) {
        VP_ERROR("D3D11Renderer could not obtain DXGI factory hr=0x{:08x}", static_cast<unsigned long>(hr));
        releaseCom(factory);
        releaseCom(adapter);
        releaseCom(dxgiDevice);
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = static_cast<UINT>(std::max(surface_->width(), 1));
    desc.Height = static_cast<UINT>(std::max(surface_->height(), 1));
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Scaling = DXGI_SCALING_STRETCH;

    const HWND hwnd = reinterpret_cast<HWND>(surface_->winId());
    hr = factory->CreateSwapChainForHwnd(context_->device(), hwnd, &desc, nullptr, nullptr, &swapChain_);
    if (SUCCEEDED(hr)) {
        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    }
    releaseCom(factory);
    releaseCom(adapter);
    releaseCom(dxgiDevice);
    if (FAILED(hr)) {
        VP_ERROR("CreateSwapChainForHwnd failed hr=0x{:08x}", static_cast<unsigned long>(hr));
        return false;
    }
    return createRenderTarget();
}
bool D3D11Renderer::initializePipeline()
{
    DeviceContextLock lock(context_);
    ID3DBlob* vertexBlob = nullptr;
    ID3DBlob* pixelBlob = nullptr;
    if (!compileShader("VSMain", "vs_4_0", &vertexBlob) ||
        !compileShader("PSMain", "ps_4_0", &pixelBlob)) {
        releaseCom(pixelBlob);
        releaseCom(vertexBlob);
        return false;
    }

    HRESULT hr = context_->device()->CreateVertexShader(vertexBlob->GetBufferPointer(),
                                                         vertexBlob->GetBufferSize(),
                                                         nullptr,
                                                         &vertexShader_);
    if (SUCCEEDED(hr)) {
        hr = context_->device()->CreatePixelShader(pixelBlob->GetBufferPointer(),
                                                    pixelBlob->GetBufferSize(),
                                                    nullptr,
                                                    &pixelShader_);
    }
    releaseCom(pixelBlob);
    releaseCom(vertexBlob);
    if (FAILED(hr)) {
        VP_ERROR("D3D11Renderer shader creation failed hr=0x{:08x}", static_cast<unsigned long>(hr));
        return false;
    }

    D3D11_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    hr = context_->device()->CreateSamplerState(&samplerDesc, &sampler_);
    if (SUCCEEDED(hr)) {
        D3D11_BUFFER_DESC constantsDesc = {};
        constantsDesc.ByteWidth = sizeof(ColorConstants);
        constantsDesc.Usage = D3D11_USAGE_DEFAULT;
        constantsDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        hr = context_->device()->CreateBuffer(&constantsDesc, nullptr, &colorConstants_);
    }
    if (FAILED(hr)) {
        VP_ERROR("D3D11Renderer pipeline state creation failed hr=0x{:08x}", static_cast<unsigned long>(hr));
        return false;
    }
    return true;
}
bool D3D11Renderer::createRenderTarget()
{
    ID3D11Texture2D* backBuffer = nullptr;
    HRESULT hr = swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (SUCCEEDED(hr)) {
        hr = context_->device()->CreateRenderTargetView(backBuffer, nullptr, &renderTargetView_);
    }
    releaseCom(backBuffer);
    if (FAILED(hr)) {
        VP_ERROR("D3D11Renderer CreateRenderTargetView failed hr=0x{:08x}", static_cast<unsigned long>(hr));
        return false;
    }
    return true;
}
bool D3D11Renderer::ensureVideoTexture(int width, int height, DXGI_FORMAT sourceFormat)
{
    if (width <= 0 || height <= 0 || (width & 1) || (height & 1) ||
        !isSupportedVideoTextureFormat(sourceFormat)) {
        return false;
    }
    if (sampledYuvTexture_ && textureWidth_ == width && textureHeight_ == height &&
        textureFormat_ == sourceFormat) {
        return true;
    }

    releaseCom(chromaSrv_);
    releaseCom(lumaSrv_);
    releaseCom(sampledYuvTexture_);
    textureWidth_ = 0;
    textureHeight_ = 0;
    textureFormat_ = DXGI_FORMAT_UNKNOWN;

    D3D11_TEXTURE2D_DESC textureDesc = {};
    textureDesc.Width = static_cast<UINT>(width);
    textureDesc.Height = static_cast<UINT>(height);
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = sourceFormat;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;
    textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = context_->device()->CreateTexture2D(&textureDesc, nullptr, &sampledYuvTexture_);
    if (FAILED(hr)) {
        VP_ERROR("CreateTexture2D format={} failed {}x{} hr=0x{:08x}",
                 static_cast<unsigned int>(sourceFormat), width, height, static_cast<unsigned long>(hr));
        return false;
    }

    const DXGI_FORMAT lumaFormat = sourceFormat == DXGI_FORMAT_P010
                                       ? DXGI_FORMAT_R16_UNORM
                                       : DXGI_FORMAT_R8_UNORM;
    const DXGI_FORMAT chromaFormat = sourceFormat == DXGI_FORMAT_P010
                                         ? DXGI_FORMAT_R16G16_UNORM
                                         : DXGI_FORMAT_R8G8_UNORM;
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Format = lumaFormat;
    hr = context_->device()->CreateShaderResourceView(sampledYuvTexture_, &srvDesc, &lumaSrv_);
    if (SUCCEEDED(hr)) {
        srvDesc.Format = chromaFormat;
        hr = context_->device()->CreateShaderResourceView(sampledYuvTexture_, &srvDesc, &chromaSrv_);
    }
    if (FAILED(hr)) {
        VP_ERROR("CreateShaderResourceView format={} failed hr=0x{:08x}",
                 static_cast<unsigned int>(sourceFormat), static_cast<unsigned long>(hr));
        releaseCom(chromaSrv_);
        releaseCom(lumaSrv_);
        releaseCom(sampledYuvTexture_);
        return false;
    }

    textureWidth_ = width;
    textureHeight_ = height;
    textureFormat_ = sourceFormat;
    return true;
}

void D3D11Renderer::reportUnsupportedFormat(unsigned long long sessionId,
                                            const QUrl& url,
                                            DXGI_FORMAT sourceFormat)
{
    reportFallback(sessionId,
                   url,
                   QStringLiteral("Unsupported D3D11 decoder texture format: %1")
                       .arg(static_cast<unsigned int>(sourceFormat)));
}

void D3D11Renderer::reportFallback(unsigned long long sessionId,
                                   const QUrl& url,
                                   const QString& reason)
{
    if (fallbackReportedSessionId_.exchange(sessionId) == sessionId) {
        return;
    }

    VP_ERROR("{}", reason.toStdString());
    if (unsupportedFormatCallback_) {
        unsupportedFormatCallback_(sessionId, url, reason);
    }
}
void D3D11Renderer::releaseSizeDependentResources()
{
    releaseCom(chromaSrv_);
    releaseCom(lumaSrv_);
    releaseCom(sampledYuvTexture_);
    releaseCom(renderTargetView_);
    textureWidth_ = 0;
    textureHeight_ = 0;
    textureFormat_ = DXGI_FORMAT_UNKNOWN;
}

void D3D11Renderer::releasePipelineResources()
{
    releaseCom(colorConstants_);
    releaseCom(sampler_);
    releaseCom(pixelShader_);
    releaseCom(vertexShader_);
}

void D3D11Renderer::clearBackBuffer()
{
    if (!renderTargetView_) {
        return;
    }
    constexpr std::array<float, 4> background = {0.0f, 0.0f, 0.0f, 1.0f};
    ID3D11DeviceContext* immediate = context_->immediateContext();
    immediate->OMSetRenderTargets(1, &renderTargetView_, nullptr);
    immediate->ClearRenderTargetView(renderTargetView_, background.data());
}

void D3D11Renderer::queuePresentLocked(bool* shouldQueue)
{
    if (!shouldQueue || presentQueued_ || !surface_) {
        return;
    }
    presentQueued_ = true;
    *shouldQueue = true;
}

void D3D11Renderer::requestQueuedPresent()
{
    if (!surface_) {
        return;
    }
    QMetaObject::invokeMethod(surface_, [this] { present(); }, Qt::QueuedConnection);
}
