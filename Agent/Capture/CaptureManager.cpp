#include "CaptureManager.h"
#include "DxgiScreenCapture.h"
#include "WasapiAudioCapture.h"
#include "Common/Utils/Logger.h"
#include "Common/Utils/Timer.h"
#include <d3d11.h>

struct CaptureManager::Impl {
    std::unique_ptr<DxgiScreenCapture> screenCapture;
    std::unique_ptr<WasapiAudioCapture> audioCapture;
    std::vector<MonitorDesc> monitors;

    uint32_t targetFps = 30;
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

    // Create D3D11 device shared between capture and encoder
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0
    };

    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                    featureLevels, ARRAYSIZE(featureLevels),
                                    D3D11_SDK_VERSION,
                                    &m_impl->d3dDevice, nullptr, &m_impl->d3dContext);
    if (FAILED(hr)) {
        LOG_ERROR("D3D11CreateDevice failed: 0x%08X", hr);
        return false;
    }

    // Init screen capture
    m_impl->screenCapture = std::make_unique<DxgiScreenCapture>();
    if (!m_impl->screenCapture->Initialize(m_impl->d3dDevice, m_impl->d3dContext)) {
        LOG_ERROR("Failed to initialize DXGI screen capture");
        return false;
    }

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
