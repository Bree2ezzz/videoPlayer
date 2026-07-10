#include "d3d11context.h"

#include "app_logger.h"

#include <array>

D3D11Context::~D3D11Context()
{
    std::lock_guard<std::mutex> lock(contextMutex_);
    releaseDeviceObjects();
}

bool D3D11Context::initialize()
{
    std::lock_guard<std::mutex> lock(contextMutex_);
    if (device_ && immediateContext_) {
        return true;
    }
    releaseDeviceObjects();

    const std::array<D3D_FEATURE_LEVEL, 4> requestedFeatureLevels = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT |
                       D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    HRESULT hr = D3D11CreateDevice(nullptr,
                                   D3D_DRIVER_TYPE_HARDWARE,
                                   nullptr,
                                   flags,
                                   requestedFeatureLevels.data(),
                                   static_cast<UINT>(requestedFeatureLevels.size()),
                                   D3D11_SDK_VERSION,
                                   &device_,
                                   &featureLevel_,
                                   &immediateContext_);
    if (hr == E_INVALIDARG) {
        // Windows 7-era runtimes do not accept 11.1 in the requested list.
        releaseDeviceObjects();
        hr = D3D11CreateDevice(nullptr,
                               D3D_DRIVER_TYPE_HARDWARE,
                               nullptr,
                               flags,
                               requestedFeatureLevels.data() + 1,
                               static_cast<UINT>(requestedFeatureLevels.size() - 1),
                               D3D11_SDK_VERSION,
                               &device_,
                               &featureLevel_,
                               &immediateContext_);
    }
    if (FAILED(hr)) {
        VP_ERROR("D3D11CreateDevice failed hr=0x{:08x}", static_cast<unsigned long>(hr));
        releaseDeviceObjects();
        return false;
    }

    ID3D11Multithread* multithread = nullptr;
    hr = immediateContext_->QueryInterface(IID_PPV_ARGS(&multithread));
    if (FAILED(hr) || !multithread) {
        VP_ERROR("ID3D11Multithread unavailable hr=0x{:08x}", static_cast<unsigned long>(hr));
        releaseDeviceObjects();
        return false;
    }
    multithread->SetMultithreadProtected(TRUE);
    multithread->Release();

    VP_INFO("D3D11 device initialized device={} context={} feature_level=0x{:x}",
            static_cast<const void*>(device_),
            static_cast<const void*>(immediateContext_),
            static_cast<unsigned int>(featureLevel_));
    return true;
}

void D3D11Context::lock()
{
    contextMutex_.lock();
}

void D3D11Context::unlock()
{
    contextMutex_.unlock();
}

void D3D11Context::releaseDeviceObjects()
{
    if (immediateContext_) {
        immediateContext_->Release();
        immediateContext_ = nullptr;
    }
    if (device_) {
        device_->Release();
        device_ = nullptr;
    }
}
