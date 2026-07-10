#ifndef D3D11RENDERER_H
#define D3D11RENDERER_H

#include "videorendererbase.h"

#include <dxgiformat.h>
#include <QUrl>

#include <atomic>
#include <functional>
#include <mutex>

class D3D11Context;
class D3D11RenderSurface;
class QWidget;
class QString;

struct ID3D11Texture2D;
struct ID3D11RenderTargetView;
struct ID3D11VertexShader;
struct ID3D11PixelShader;
struct ID3D11SamplerState;
struct ID3D11Buffer;
struct ID3D11ShaderResourceView;
struct IDXGISwapChain1;

class D3D11Renderer : public VideoRendererBase
{
public:
    D3D11Renderer(D3D11Context* context, QWidget* parent = nullptr);
    ~D3D11Renderer() override;

    void renderFrame(AVFrame* frame) override;
    void clear() override;
    QWidget* asWidget() override;
    int preferredPixelFormat() const override;
    bool isReady() const { return initialized_.load(); }

    using UnsupportedFormatCallback = std::function<void(unsigned long long sessionId,
                                                          const QUrl& url,
                                                          const QString& reason)>;
    void setUnsupportedFormatCallback(UnsupportedFormatCallback callback);
    void setPlaybackSession(unsigned long long sessionId, const QUrl& url);

    void resizeSwapChain(int width, int height);
    void present();

private:
    bool initializeSwapChain();
    bool initializePipeline();
    bool createRenderTarget();
    bool ensureVideoTexture(int width, int height, DXGI_FORMAT sourceFormat);
    void reportUnsupportedFormat(unsigned long long sessionId,
                                 const QUrl& url,
                                 DXGI_FORMAT sourceFormat);
    void reportFallback(unsigned long long sessionId,
                        const QUrl& url,
                        const QString& reason);
    void releaseSizeDependentResources();
    void releasePipelineResources();
    void clearBackBuffer();
    void queuePresentLocked(bool* shouldQueue);
    void requestQueuedPresent();

    D3D11Context* context_ = nullptr; // MainWindow keeps this alive longer than the renderer.
    D3D11RenderSurface* surface_ = nullptr;

    std::mutex pendingMutex_;
    AVFrame* pendingFrame_ = nullptr;
    unsigned long long activeSessionId_ = 0;
    QUrl activeSessionUrl_;
    unsigned long long pendingSessionId_ = 0;
    QUrl pendingSessionUrl_;
    bool clearRequested_ = false;
    bool presentQueued_ = false;

    IDXGISwapChain1* swapChain_ = nullptr;
    ID3D11RenderTargetView* renderTargetView_ = nullptr;
    ID3D11Texture2D* sampledYuvTexture_ = nullptr;
    ID3D11ShaderResourceView* lumaSrv_ = nullptr;
    ID3D11ShaderResourceView* chromaSrv_ = nullptr;
    ID3D11VertexShader* vertexShader_ = nullptr;
    ID3D11PixelShader* pixelShader_ = nullptr;
    ID3D11SamplerState* sampler_ = nullptr;
    ID3D11Buffer* colorConstants_ = nullptr;
    int textureWidth_ = 0;
    int textureHeight_ = 0;
    DXGI_FORMAT textureFormat_ = DXGI_FORMAT_UNKNOWN;
    UnsupportedFormatCallback unsupportedFormatCallback_;
    std::atomic<unsigned long long> fallbackReportedSessionId_{0};
    std::atomic_bool initialized_{false};
};

#endif // D3D11RENDERER_H
