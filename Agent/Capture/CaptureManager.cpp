#include "CaptureManager.h"
#include "DxgiScreenCapture.h"
#include "WasapiAudioCapture.h"
#include "Common/Utils/Logger.h"
#include "Common/Utils/Timer.h"
#include <d3d11.h>
#include <d3d10.h>       // ID3D10Multithread
#include <dxgi1_2.h>

#pragma comment(lib, "dxguid.lib")  // IID_ID3D10Multithread

struct CaptureManager::Impl {
    std::unique_ptr<DxgiScreenCapture> screenCapture;
    std::unique_ptr<WasapiAudioCapture> audioCapture;
    std::vector<MonitorDesc> monitors;

    uint32_t targetFps = 60;
    std::atomic<bool> running{false};

    // Stats
    std::atomic<uint32_t> capturedFrames{0};
    std::atomic<uint32_t> audioFrames{0};
    int64_t lastStatsTime = 0;
    uint32_t framesSinceLastStats = 0;
    float captureFps = 0;

    ID3D11Device* d3dDevice = nullptr;
    ID3D11DeviceContext* d3dContext = nullptr;
};

CaptureManager::CaptureManager() : m_impl(std::make_unique<Impl>()) {}

CaptureManager::~CaptureManager() { Stop(); }

bool CaptureManager::Initialize(uint32_t targetFps) {
    m_impl->targetFps = targetFps;

    // Create D3D11 device shared between capture and encoder.
    // D3D11_CREATE_DEVICE_VIDEO_SUPPORT is required for Intel QuickSync and
    // other hardware MFTs to accept the device via MFT_MESSAGE_SET_D3D_MANAGER.
    // On multi-GPU systems (e.g. Optimus laptops) the default adapter may be
    // the dGPU; we need the adapter that owns the display outputs so DXGI
    // Desktop Duplication and the hardware encoder share the same GPU.
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT
               | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0
    };

    // Find the adapter with display outputs for DXGI Desktop Duplication.
    // On Optimus laptops the NVIDIA dGPU typically owns the display outputs.
    IDXGIAdapter* chosenAdapter = nullptr;
    {
        IDXGIFactory1* factory = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) {
            IDXGIAdapter1* adap = nullptr;
            for (UINT i = 0; factory->EnumAdapters1(i, &adap) != DXGI_ERROR_NOT_FOUND; ++i) {
                IDXGIOutput* out = nullptr;
                bool hasOutputs = (adap->EnumOutputs(0, &out) != DXGI_ERROR_NOT_FOUND);
                if (out) out->Release();

                DXGI_ADAPTER_DESC1 desc;
                adap->GetDesc1(&desc);
                LOG_INFO("  Adapter %u: %S (vendor=0x%04X device=0x%04X flags=0x%X)%s",
                         i, desc.Description, desc.VendorId, desc.DeviceId,
                         desc.Flags, hasOutputs ? " [has outputs]" : "");

                if (hasOutputs && !chosenAdapter) {
                    chosenAdapter = adap;
                    chosenAdapter->AddRef();
                }
                adap->Release();
            }
            factory->Release();
        }
    }

    HRESULT hr = D3D11CreateDevice(chosenAdapter,
                                    chosenAdapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                                    nullptr, flags,
                                    featureLevels, ARRAYSIZE(featureLevels),
                                    D3D11_SDK_VERSION,
                                    &m_impl->d3dDevice, nullptr, &m_impl->d3dContext);
    if (chosenAdapter) chosenAdapter->Release();

    if (FAILED(hr)) {
        LOG_ERROR("D3D11CreateDevice failed: 0x%08X", hr);
        return false;
    }

    // Log which adapter the device landed on
    {
        IDXGIDevice* dxgiDev = nullptr;
        if (SUCCEEDED(m_impl->d3dDevice->QueryInterface(
                          __uuidof(IDXGIDevice), (void**)&dxgiDev))) {
            IDXGIAdapter* adap = nullptr;
            if (SUCCEEDED(dxgiDev->GetAdapter(&adap))) {
                DXGI_ADAPTER_DESC desc;
                if (SUCCEEDED(adap->GetDesc(&desc))) {
                    LOG_INFO("D3D11 device created on: %S", desc.Description);
                }
                adap->Release();
            }
            dxgiDev->Release();
        }
    }

    // Enable multithread protection — the same D3D11 immediate context is used
    // from the encoder thread (CopyResource, VideoProcessorBlt, Flush) and the
    // capture thread (CPU-fallback staging), so the driver must serialize access.
    {
        ID3D10Multithread* mt = nullptr;
        if (SUCCEEDED(m_impl->d3dDevice->QueryInterface(IID_ID3D10Multithread, (void**)&mt))) {
            mt->SetMultithreadProtected(TRUE);
            mt->Release();
            LOG_INFO("D3D11 multithread protection enabled (Agent)");
        }
    }

    // Init screen capture
    m_impl->screenCapture = std::make_unique<DxgiScreenCapture>();
    if (!m_impl->screenCapture->Initialize(m_impl->d3dDevice, m_impl->d3dContext)) {
        LOG_ERROR("Failed to initialize DXGI screen capture");
        return false;
    }
    m_impl->screenCapture->SetTargetFps(m_impl->targetFps);

    m_impl->monitors = m_impl->screenCapture->GetMonitors();
    LOG_INFO("Detected %zu monitor(s)", m_impl->monitors.size());
    for (auto& m : m_impl->monitors) {
        LOG_INFO("  Monitor: %ux%u at (%d, %d)%s",
                 m.width, m.height, m.x, m.y, m.isPrimary ? " [primary]" : "");
    }

    // Try to init audio capture (non-fatal if fails)
    m_impl->audioCapture = std::make_unique<WasapiAudioCapture>();
    if (!m_impl->audioCapture->Initialize(48000, 2)) {
        LOG_WARNING("Audio capture not available");
        m_impl->audioCapture.reset();
    }

    m_impl->lastStatsTime = Timer::NowMs();
    m_impl->running = true;
    return true;
}

void CaptureManager::Stop() {
    m_impl->running = false;
    if (m_impl->screenCapture) m_impl->screenCapture->Shutdown();
    if (m_impl->audioCapture) m_impl->audioCapture->Stop();
    if (m_impl->d3dContext) m_impl->d3dContext->Release();
    if (m_impl->d3dDevice) m_impl->d3dDevice->Release();
}

bool CaptureManager::AcquireFrame(std::vector<uint8_t>& outData, uint32_t& width, uint32_t& height) {
    if (!m_impl->running || !m_impl->screenCapture) return false;

    DxgiScreenCapture::CapturedFrame frame;
    if (!m_impl->screenCapture->AcquireFrame(frame)) return false;

    width = frame.width;
    height = frame.height;
    outData = std::move(frame.data);

    m_impl->capturedFrames++;
    m_impl->framesSinceLastStats++;

    // Update FPS stats
    auto now = Timer::NowMs();
    auto elapsed = now - m_impl->lastStatsTime;
    if (elapsed >= 1000) {
        m_impl->captureFps = m_impl->framesSinceLastStats * 1000.0f / elapsed;
        m_impl->framesSinceLastStats = 0;
        m_impl->lastStatsTime = now;
    }

    return true;
}

bool CaptureManager::AcquireFrameGpu(CapturedFrameGpu& outFrame) {
    if (!m_impl->running || !m_impl->screenCapture) return false;

    if (!m_impl->screenCapture->AcquireFrameGpu(outFrame))
        return false;

    m_impl->capturedFrames++;
    m_impl->framesSinceLastStats++;

    auto now = Timer::NowMs();
    auto elapsed = now - m_impl->lastStatsTime;
    if (elapsed >= 1000) {
        m_impl->captureFps = m_impl->framesSinceLastStats * 1000.0f / elapsed;
        m_impl->framesSinceLastStats = 0;
        m_impl->lastStatsTime = now;
    }

    return true;
}

void CaptureManager::ReleaseGpuFrame() {
    if (m_impl->screenCapture) m_impl->screenCapture->ReleaseGpuFrame();
}

bool CaptureManager::AcquireAudio(std::vector<uint8_t>& outData) {
    if (!m_impl->audioCapture) return false;
    if (m_impl->audioCapture->GetBuffer(outData)) {
        m_impl->audioFrames++;
        return true;
    }
    return false;
}

std::vector<MonitorDesc> CaptureManager::GetMonitorDescs() const {
    return m_impl->monitors;
}

void CaptureManager::SetTargetFps(uint32_t fps) {
    m_impl->targetFps = fps;
}

CaptureManager::Stats CaptureManager::GetStats() const {
    Stats s;
    s.captureFps = m_impl->captureFps;
    s.capturedFrames = m_impl->capturedFrames;
    return s;
}

ID3D11Device* CaptureManager::GetD3DDevice() const {
    return m_impl->d3dDevice;
}

ID3D11DeviceContext* CaptureManager::GetD3DContext() const {
    return m_impl->d3dContext;
}
