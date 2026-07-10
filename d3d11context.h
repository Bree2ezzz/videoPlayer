#ifndef D3D11CONTEXT_H
#define D3D11CONTEXT_H

#include <d3d11.h>
#include <d3d11_4.h>

#include <mutex>

class D3D11Context
{
public:
    D3D11Context() = default;
    ~D3D11Context();

    D3D11Context(const D3D11Context&) = delete;
    D3D11Context& operator=(const D3D11Context&) = delete;

    bool initialize();

    ID3D11Device* device() const { return device_; }
    ID3D11DeviceContext* immediateContext() const { return immediateContext_; }
    D3D_FEATURE_LEVEL featureLevel() const { return featureLevel_; }

    // Used by FFmpeg lock callbacks and by every renderer access to the immediate context.
    void lock();
    void unlock();

private:
    void releaseDeviceObjects();

    mutable std::mutex contextMutex_;
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* immediateContext_ = nullptr;
    D3D_FEATURE_LEVEL featureLevel_ = D3D_FEATURE_LEVEL_9_1;
};

#endif // D3D11CONTEXT_H
