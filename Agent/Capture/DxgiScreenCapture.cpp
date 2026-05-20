#include "DxgiScreenCapture.h"
#include "Common/Utils/Logger.h"
#include "Common/Utils/Timer.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi.h>
#include <cstdio>
#include <cstring>

DxgiScreenCapture::DxgiScreenCapture() {}

DxgiScreenCapture::~DxgiScreenCapture() { Shutdown(); }

bool DxgiScreenCapture::Initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
    m_device = device;
    m_context = context;
    if (!m_device || !m_context) return false;

    // Get DXGI device from D3D11 device
    IDXGIDevice* dxgiDevice = nullptr;
    HRESULT hr = m_device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
    if (FAILED(hr)) {
        LOG_ERROR("Failed to get IDXGIDevice: 0x%08X", hr);
        return false;
    }

    IDXGIAdapter* adapter = nullptr;
    hr = dxgiDevice->GetAdapter(&adapter);
    dxgiDevice->Release();
    if (FAILED(hr)) {
        LOG_ERROR("Failed to get IDXGIAdapter: 0x%08X", hr);
        return false;
    }

    // Enumerate outputs
    IDXGIOutput* output = nullptr;
    int outputIndex = 0;
    while (adapter->EnumOutputs(outputIndex, &output) != DXGI_ERROR_NOT_FOUND) {
        DXGI_OUTPUT_DESC outputDesc;
        output->GetDesc(&outputDesc);

        MonitorDesc desc;
        desc.width  = outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left;
        desc.height = outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top;
        desc.x = outputDesc.DesktopCoordinates.left;
        desc.y = outputDesc.DesktopCoordinates.top;
        desc.isPrimary = (outputIndex == 0);

        MonitorCapture mc;
        mc.desc = desc;
        mc.valid = false;
        m_monitors.push_back(mc);

        output->Release();
        outputIndex++;
    }
    adapter->Release();

    if (m_monitors.empty()) {
        LOG_ERROR("No monitors found");
        return false;
    }

    // Try to init duplication for each monitor
    for (size_t i = 0; i < m_monitors.size(); ++i) {
        InitDuplicationForOutput(static_cast<int>(i));
    }

    LOG_INFO("DXGI screen capture initialized, %zu monitor(s)", m_monitors.size());
    return true;
}

bool DxgiScreenCapture::InitDuplicationForOutput(int outputIndex) {
    if (outputIndex < 0 || outputIndex >= static_cast<int>(m_monitors.size())) return false;

    // Re-get adapter and output
    IDXGIDevice* dxgiDevice = nullptr;
    m_device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
    if (!dxgiDevice) return false;

    IDXGIAdapter* adapter = nullptr;
    dxgiDevice->GetAdapter(&adapter);
    dxgiDevice->Release();

    IDXGIOutput* output = nullptr;
    HRESULT hr = adapter->EnumOutputs(outputIndex, &output);
    adapter->Release();
    if (FAILED(hr)) return false;

    // Get IDXGIOutput1 for DuplicateOutput
    IDXGIOutput1* output1 = nullptr;
    hr = output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1);
    output->Release();
    if (FAILED(hr)) {
        LOG_WARNING("Monitor %d does not support IDXGIOutput1", outputIndex);
        return false;
    }

    auto& mc = m_monitors[outputIndex];
    if (mc.duplication) {
        mc.duplication->Release();
        mc.duplication = nullptr;
    }

    hr = output1->DuplicateOutput(m_device, &mc.duplication);
    output1->Release();

    if (FAILED(hr)) {
        LOG_WARNING("DuplicateOutput failed for monitor %d: 0x%08X", outputIndex, hr);
        return false;
    }

    mc.valid = true;
    return true;
}

void DxgiScreenCapture::Shutdown() {
    for (auto& mc : m_monitors) {
        if (mc.duplication) mc.duplication->Release();
        if (mc.stagingTex) mc.stagingTex->Release();
    }
    m_monitors.clear();
}

bool DxgiScreenCapture::AcquireFrame(CapturedFrame& outFrame) {
    if (m_monitors.empty()) return false;

    // Pace to target framerate — sleep only the remaining budget so capture
    // runs as close to the target rate as possible without busy-waiting.
    auto now = Timer::NowMs();
    int64_t frameInterval = 1000 / m_targetFps;
    int64_t elapsed = now - m_lastFrameTime;
    if (elapsed < frameInterval) {
        Sleep(static_cast<DWORD>(frameInterval - elapsed));
        now = Timer::NowMs();
    }

    // Capture primary monitor only (monitor 0)
    // Multi-monitor composition would require stitching frames before encoding
    if (AcquireFromMonitor(0, outFrame)) {
        m_lastFrameTime = now;
        outFrame.timestampMs = now;
        return true;
    }
    return false;
}

bool DxgiScreenCapture::AcquireFrameGpu(CapturedFrameGpu& outFrame) {
    if (m_monitors.empty()) return false;

    auto now = Timer::NowMs();
    int64_t frameInterval = 1000 / m_targetFps;
    int64_t elapsed = now - m_lastFrameTime;
    if (elapsed < frameInterval) {
        Sleep(static_cast<DWORD>(frameInterval - elapsed));
        now = Timer::NowMs();
    }

    if (AcquireFromMonitorGpu(0, outFrame)) {
        m_lastFrameTime = now;
        outFrame.timestampMs = now;
        return true;
    }
    return false;
}

void DxgiScreenCapture::ReleaseGpuFrame() {
    if (!m_monitors.empty() && m_monitors[0].duplication) {
        m_monitors[0].duplication->ReleaseFrame();
    }
}

bool DxgiScreenCapture::AcquireFromMonitor(int index, CapturedFrame& outFrame) {
    auto& mc = m_monitors[index];
    if (!mc.valid || !mc.duplication) return false;

    IDXGIResource* frameResource = nullptr;
    DXGI_OUTDUPL_FRAME_INFO frameInfo;
    HRESULT hr = DXGI_ERROR_WAIT_TIMEOUT;

    // Retry a few times with a 1ms wait — at 60fps (16.67ms interval)
    // the desktop may not have a fresh frame exactly when we ask.
    for (int retry = 0; retry < 3; ++retry) {
        hr = mc.duplication->AcquireNextFrame(0, &frameInfo, &frameResource);
        if (hr != DXGI_ERROR_WAIT_TIMEOUT) break;
        if (retry < 2) Sleep(1);
    }

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        return false; // No new frame after retries, normal
    }
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        LOG_WARNING("DXGI access lost on monitor %d, reinitializing", index);
        InitDuplicationForOutput(index);
        return false;
    }
    if (FAILED(hr)) {
        return false;
    }

    // Get the D3D11 texture from the frame resource
    ID3D11Texture2D* srcTexture = nullptr;
    hr = frameResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&srcTexture);
    frameResource->Release();

    if (FAILED(hr)) {
        mc.duplication->ReleaseFrame();
        return false;
    }

    D3D11_TEXTURE2D_DESC texDesc;
    srcTexture->GetDesc(&texDesc);

    uint32_t width  = mc.desc.width;
    uint32_t height = mc.desc.height;

    // Create or resize staging texture if needed
    if (!mc.stagingTex) {
        D3D11_TEXTURE2D_DESC stagingDesc = {};
        stagingDesc.Width = width;
        stagingDesc.Height = height;
        stagingDesc.MipLevels = 1;
        stagingDesc.ArraySize = 1;
        stagingDesc.Format = texDesc.Format; // Usually DXGI_FORMAT_B8G8R8A8_UNORM
        stagingDesc.SampleDesc.Count = 1;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingDesc.BindFlags = 0;

        hr = m_device->CreateTexture2D(&stagingDesc, nullptr, &mc.stagingTex);
        if (FAILED(hr)) {
            srcTexture->Release();
            mc.duplication->ReleaseFrame();
            return false;
        }
    }

    // Copy from GPU texture to staging texture
    m_context->CopyResource(mc.stagingTex, srcTexture);

    // Map staging texture and read pixels
    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = m_context->Map(mc.stagingTex, 0, D3D11_MAP_READ, 0, &mapped);
    if (SUCCEEDED(hr)) {
        // For BGRA format, copy raw pixels
        size_t dataSize = width * height * 4;
        outFrame.data.resize(dataSize);

        if (mapped.RowPitch == width * 4) {
            // Tightly packed, can copy directly
            memcpy(outFrame.data.data(), mapped.pData, dataSize);
        } else {
            // Handle row pitch mismatch
            for (uint32_t row = 0; row < height; ++row) {
                memcpy(outFrame.data.data() + row * width * 4,
                       static_cast<const uint8_t*>(mapped.pData) + row * mapped.RowPitch,
                       width * 4);
            }
        }
        m_context->Unmap(mc.stagingTex, 0);
    }

    outFrame.width = width;
    outFrame.height = height;

    srcTexture->Release();
    mc.duplication->ReleaseFrame();

    return SUCCEEDED(hr);
}

bool DxgiScreenCapture::AcquireFromMonitorGpu(int index, CapturedFrameGpu& outFrame) {
    auto& mc = m_monitors[index];
    if (!mc.valid || !mc.duplication) return false;

    IDXGIResource* frameResource = nullptr;
    DXGI_OUTDUPL_FRAME_INFO frameInfo;
    HRESULT hr = DXGI_ERROR_WAIT_TIMEOUT;

    for (int retry = 0; retry < 3; ++retry) {
        hr = mc.duplication->AcquireNextFrame(0, &frameInfo, &frameResource);
        if (hr != DXGI_ERROR_WAIT_TIMEOUT) break;
        if (retry < 2) Sleep(1);
    }

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        LOG_WARNING("DXGI access lost on monitor %d, reinitializing", index);
        InitDuplicationForOutput(index);
        return false;
    }
    if (FAILED(hr)) return false;

    ID3D11Texture2D* srcTexture = nullptr;
    hr = frameResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&srcTexture);
    frameResource->Release();

    if (FAILED(hr)) {
        mc.duplication->ReleaseFrame();
        return false;
    }

    // Return the GPU texture directly — caller MUST call ReleaseGpuFrame()
    // after processing is complete, then Release() the texture.
    outFrame.texture = srcTexture; // already AddRef'd by QueryInterface
    outFrame.width  = mc.desc.width;
    outFrame.height = mc.desc.height;
    return true;
}

std::vector<MonitorDesc> DxgiScreenCapture::GetMonitors() const {
    std::vector<MonitorDesc> result;
    for (auto& mc : m_monitors) {
        result.push_back(mc.desc);
    }
    return result;
}

void SaveFrameToBmp(const uint8_t* data, uint32_t width, uint32_t height, const char* filename) {
    // BMP file header
    uint32_t rowSize = ((width * 24 + 31) / 32) * 4; // DWORD-aligned
    uint32_t imageSize = rowSize * height;
    uint32_t fileSize = 54 + imageSize;

    uint8_t bmpHeader[54] = {};
    bmpHeader[0] = 'B'; bmpHeader[1] = 'M';
    memcpy(bmpHeader + 2, &fileSize, 4);
    bmpHeader[10] = 54; // data offset
    bmpHeader[14] = 40; // DIB header size
    memcpy(bmpHeader + 18, &width, 4);
    memcpy(bmpHeader + 22, &height, 4);
    bmpHeader[26] = 1;  // planes
    bmpHeader[28] = 24; // bits per pixel
    memcpy(bmpHeader + 34, &imageSize, 4);

    FILE* f = fopen(filename, "wb");
    if (!f) return;
    fwrite(bmpHeader, 1, 54, f);

    // BMP expects BGR (we have BGRA from DXGI), just strip alpha and flip
    std::vector<uint8_t> row(rowSize, 0);
    for (int32_t y = static_cast<int32_t>(height) - 1; y >= 0; --y) {
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t* src = data + (y * width + x) * 4;
            uint8_t* dst = row.data() + x * 3;
            dst[0] = src[0]; // B
            dst[1] = src[1]; // G
            dst[2] = src[2]; // R
        }
        fwrite(row.data(), 1, rowSize, f);
    }
    fclose(f);
}
