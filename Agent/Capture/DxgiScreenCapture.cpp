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

    mc.duplicateCount = 0;
    mc.prevSample.clear();

    hr = output1->DuplicateOutput(m_device, &mc.duplication);
    output1->Release();

    if (FAILED(hr)) {
        LOG_WARNING("DuplicateOutput failed for monitor %d: 0x%08X", outputIndex, hr);
        return false;
    }

    // Pre-allocate texture pool for safe copies.
    // Per-frame CreateTexture2D at 2560x1600 (~16MB) causes 100-600ms GPU
    // allocation stalls. A fixed pool eliminates that overhead entirely.
    {
        uint32_t pw = mc.desc.width;
        uint32_t ph = mc.desc.height;
        bool needRealloc = (mc.poolWidth != pw || mc.poolHeight != ph);

        // Release old pool if dimensions changed (e.g. monitor resolution change)
        if (needRealloc) {
            for (int i = 0; i < kSafePoolSize; ++i) {
                if (mc.safePool[i]) {
                    mc.safePool[i]->Release();
                    mc.safePool[i] = nullptr;
                }
            }
        }

        if (needRealloc || !mc.safePool[0]) {
            D3D11_TEXTURE2D_DESC poolDesc = {};
            poolDesc.Width = pw;
            poolDesc.Height = ph;
            poolDesc.MipLevels = 1;
            poolDesc.ArraySize = 1;
            poolDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            poolDesc.SampleDesc.Count = 1;
            poolDesc.Usage = D3D11_USAGE_DEFAULT;
            poolDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

            for (int i = 0; i < kSafePoolSize; ++i) {
                if (!mc.safePool[i]) {
                    HRESULT poolHr = m_device->CreateTexture2D(&poolDesc, nullptr, &mc.safePool[i]);
                    if (FAILED(poolHr)) {
                        LOG_WARNING("Safe pool tex[%d] creation failed: 0x%08X", i, poolHr);
                    }
                }
            }
            mc.poolWidth = pw;
            mc.poolHeight = ph;
            mc.poolIndex = 0;
            LOG_INFO("Safe texture pool allocated: %ux%u x%d (%.1f MB each)",
                     pw, ph, kSafePoolSize, (pw * ph * 4.0) / (1024 * 1024));
        }
    }

    mc.valid = true;
    return true;
}

void DxgiScreenCapture::Shutdown() {
    for (auto& mc : m_monitors) {
        if (mc.duplication) mc.duplication->Release();
        if (mc.stagingTex) mc.stagingTex->Release();
        if (mc.sampleStagingTex) mc.sampleStagingTex->Release();
        for (int i = 0; i < kSafePoolSize; ++i) {
            if (mc.safePool[i]) mc.safePool[i]->Release();
        }
    }
    m_monitors.clear();
}

bool DxgiScreenCapture::AcquireFrame(CapturedFrame& outFrame) {
    if (m_monitors.empty()) return false;

    // Pace to target framerate — ensures consistent inter-frame spacing.
    auto now = Timer::NowMs();
    int64_t frameInterval = 1000 / m_targetFps;
    int64_t elapsed = now - m_lastFrameTime;
    if (elapsed < frameInterval) {
        Sleep(static_cast<DWORD>(frameInterval - elapsed));
    }

    if (AcquireFromMonitor(0, outFrame)) {
        m_lastFrameTime = Timer::NowMs();
        outFrame.timestampMs = m_lastFrameTime;
        return true;
    }
    m_lastFrameTime = Timer::NowMs();
    return false;
}

bool DxgiScreenCapture::AcquireFrameGpu(CapturedFrameGpu& outFrame) {
    if (m_monitors.empty()) return false;

    // Pace to target framerate — ensures consistent inter-frame spacing so the
    // encoder always has its full frame budget (16.67ms at 60fps). Without this,
    // frames can arrive in bursts that overflow the encoder queue.
    auto now = Timer::NowMs();
    int64_t frameInterval = 1000 / m_targetFps;
    int64_t elapsed = now - m_lastFrameTime;
    if (elapsed < frameInterval) {
        Sleep(static_cast<DWORD>(frameInterval - elapsed));
    }

    if (AcquireFromMonitorGpu(0, outFrame)) {
        m_lastFrameTime = Timer::NowMs();
        outFrame.timestampMs = m_lastFrameTime;
        return true;
    }
    // Update timer on miss to avoid tight-looping after static periods.
    // Without this, elapsed grows unbounded, pacing is skipped, and the
    // capture thread burns CPU in a polling loop.
    m_lastFrameTime = Timer::NowMs();
    return false;
}

void DxgiScreenCapture::ReleaseGpuFrame() {
    // DXGI frame is already released inside AcquireFromMonitorGpu
    // after the safe copy. This is a no-op kept for API compatibility.
}

bool DxgiScreenCapture::AcquireFromMonitor(int index, CapturedFrame& outFrame) {
    auto& mc = m_monitors[index];
    if (!mc.valid || !mc.duplication) return false;

    IDXGIResource* frameResource = nullptr;
    DXGI_OUTDUPL_FRAME_INFO frameInfo;

    HRESULT hr = mc.duplication->AcquireNextFrame(50, &frameInfo, &frameResource);

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        LOG_WARNING("DXGI access lost on monitor %d, reinitializing", index);
        InitDuplicationForOutput(index);
        return false;
    }
    if (FAILED(hr)) return false;

    // Skip duplicate frames: AccumulatedFrames==0 means the desktop hasn't
    // presented since our last AcquireNextFrame (same physical frame).
    if (frameInfo.AccumulatedFrames == 0) {
        frameResource->Release();
        mc.duplication->ReleaseFrame();
        return false;
    }

    // Fast path: zero metadata means no dirty/move rects reported by the OS.
    // The frame is pixel-identical — skip the expensive GPU readback entirely.
    if (frameInfo.TotalMetadataBufferSize == 0) {
        frameResource->Release();
        mc.duplication->ReleaseFrame();
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

    // Copy from GPU texture to staging texture.
    // Use CopySubresourceRegion (not CopyResource) because the DXGI capture
    // texture may have different MipLevels/ArraySize than our staging texture.
    {
        D3D11_BOX box;
        box.left = 0; box.top = 0; box.front = 0;
        box.right = width; box.bottom = height; box.back = 1;
        m_context->CopySubresourceRegion(mc.stagingTex, 0, 0, 0, 0,
                                         srcTexture, 0, &box);
    }

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

    // Block up to 50ms for the next desktop frame. This replaces the old
    // AcquireNextFrame(0) + 3×Sleep(1) retry loop, eliminating the tight
    // polling that burned CPU and caused burst-gap capture patterns.
    // A 50ms timeout gives ~20 checks/sec for m_running during static
    // periods while naturally pacing to the DWM's VSync rate (60Hz =
    // 16.67ms) when content is changing.
    HRESULT hr = mc.duplication->AcquireNextFrame(50, &frameInfo, &frameResource);

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        static uint32_t timeoutLogCount = 0;
        if ((timeoutLogCount++ & 0x3FF) == 0) {
            LOG_INFO("[CaptureGpu] AcquireNextFrame timeout (count=%u)", timeoutLogCount);
        }
        return false;
    }
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        LOG_WARNING("DXGI access lost on monitor %d, reinitializing", index);
        InitDuplicationForOutput(index);
        return false;
    }
    if (FAILED(hr)) {
        static uint32_t failLogCount = 0;
        if ((failLogCount++ & 0x3F) == 0) {
            LOG_WARNING("[CaptureGpu] AcquireNextFrame failed: 0x%08X (count=%u)", hr, failLogCount);
        }
        return false;
    }

    // Skip duplicate frames: AccumulatedFrames==0 means the desktop hasn't
    // presented since our last AcquireNextFrame (same physical frame).
    if (frameInfo.AccumulatedFrames == 0) {
        static uint32_t dupLogCount = 0;
        if ((dupLogCount++ & 0x3FF) == 0) {
            LOG_INFO("[CaptureGpu] AccumulatedFrames==0 (count=%u)", dupLogCount);
        }
        frameResource->Release();
        mc.duplication->ReleaseFrame();
        return false;
    }

    ID3D11Texture2D* srcTexture = nullptr;
    hr = frameResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&srcTexture);
    frameResource->Release();

    if (FAILED(hr)) {
        mc.duplication->ReleaseFrame();
        return false;
    }

    // Fast path: zero metadata means no dirty/move rects reported by the OS.
    // The frame is pixel-identical — skip the expensive GPU readback entirely.
    // On modern drivers (RTX 4080+, WDDM 3.x) this is reliable. For older or
    // quirky drivers that report TotalMetadataBufferSize==0 on changed frames,
    // the periodic pixel-diff below acts as a safety net.
    if (frameInfo.TotalMetadataBufferSize == 0) {
        static uint32_t tmbZeroLogCount = 0;
        if ((tmbZeroLogCount++ & 0x3FF) == 0) {
            LOG_INFO("[CaptureGpu] TotalMetadataBufferSize==0 (count=%u)", tmbZeroLogCount);
        }
        srcTexture->Release();
        mc.duplication->ReleaseFrame();
        return false;
    }

    // Periodic pixel differencing: every 8th frame, sample a 64x64 block from
    // the screen center. This catches false-positive TotalMetadataBufferSize
    // reports on quirky drivers without incurring a GPU→CPU Map stall on every
    // captured frame. Map stalls serialize the capture and encode pipelines on
    // the shared immediate context, halving effective throughput.
    mc.frameCounter++;
    if ((mc.frameCounter & 7) == 0) {
        D3D11_TEXTURE2D_DESC srcDesc;
        srcTexture->GetDesc(&srcDesc);

        UINT sampleW = 64, sampleH = 64;
        if (!mc.sampleStagingTex) {
            D3D11_TEXTURE2D_DESC stagingDesc = {};
            stagingDesc.Width = sampleW;
            stagingDesc.Height = sampleH;
            stagingDesc.MipLevels = 1;
            stagingDesc.ArraySize = 1;
            stagingDesc.Format = srcDesc.Format;
            stagingDesc.SampleDesc.Count = 1;
            stagingDesc.Usage = D3D11_USAGE_STAGING;
            stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            stagingDesc.BindFlags = 0;
            if (FAILED(m_device->CreateTexture2D(&stagingDesc, nullptr, &mc.sampleStagingTex))) {
                mc.sampleStagingTex = nullptr;
            }
        }

        if (mc.sampleStagingTex) {
            UINT srcW = mc.desc.width, srcH = mc.desc.height;
            D3D11_BOX box;
            box.left   = (srcW > sampleW) ? (srcW - sampleW) / 2 : 0;
            box.top    = (srcH > sampleH) ? (srcH - sampleH) / 2 : 0;
            box.front  = 0;
            box.right  = box.left + sampleW;
            box.bottom = box.top + sampleH;
            box.back   = 1;

            m_context->CopySubresourceRegion(mc.sampleStagingTex, 0, 0, 0, 0,
                                             srcTexture, 0, &box);

            D3D11_MAPPED_SUBRESOURCE mapped;
            if (SUCCEEDED(m_context->Map(mc.sampleStagingTex, 0, D3D11_MAP_READ, 0, &mapped))) {
                size_t sampleSize = static_cast<size_t>(sampleW) * sampleH * 4;
                bool identical = (mc.prevSample.size() == sampleSize);

                if (identical) {
                    for (UINT row = 0; row < sampleH && identical; ++row) {
                        if (std::memcmp(static_cast<const uint8_t*>(mapped.pData) + row * mapped.RowPitch,
                                       mc.prevSample.data() + row * sampleW * 4,
                                       sampleW * 4) != 0) {
                            identical = false;
                        }
                    }
                }

                if (!identical) {
                    mc.prevSample.resize(sampleSize);
                    for (UINT row = 0; row < sampleH; ++row) {
                        std::memcpy(mc.prevSample.data() + row * sampleW * 4,
                                   static_cast<const uint8_t*>(mapped.pData) + row * mapped.RowPitch,
                                   sampleW * 4);
                    }
                    mc.duplicateCount = 0;
                } else {
                    mc.duplicateCount++;
                }
                m_context->Unmap(mc.sampleStagingTex, 0);

                if (identical) {
                    static uint32_t identLogCount = 0;
                    if ((identLogCount++ & 0x3FF) == 0) {
                        LOG_INFO("[CaptureGpu] pixel diff identical (count=%u dupCount=%u)",
                                 identLogCount, mc.duplicateCount);
                    }
                    srcTexture->Release();
                    mc.duplication->ReleaseFrame();
                    return false;
                }
            }
        }
    }

    // Copy the captured frame into a pre-allocated pool texture BEFORE
    // calling ReleaseFrame. On some drivers ReleaseFrame may recycle the
    // underlying surface; the pool texture guarantees the encoder always
    // sees valid data regardless of when the encode thread processes it.
    {
        ID3D11Texture2D* safeTex = mc.safePool[mc.poolIndex];
        if (safeTex) {
            D3D11_BOX box;
            box.left = 0; box.top = 0; box.front = 0;
            box.right = mc.desc.width; box.bottom = mc.desc.height; box.back = 1;
            m_context->CopySubresourceRegion(safeTex, 0, 0, 0, 0,
                                             srcTexture, 0, &box);
            safeTex->AddRef(); // caller takes ownership of this reference
            outFrame.texture = safeTex;
            mc.poolIndex = (mc.poolIndex + 1) % kSafePoolSize;
        } else {
            outFrame.texture = nullptr;
        }
    }

    // Release the DXGI frame immediately so the next AcquireNextFrame can proceed.
    srcTexture->Release();
    mc.duplication->ReleaseFrame();

    outFrame.width  = mc.desc.width;
    outFrame.height = mc.desc.height;
    return outFrame.texture != nullptr;
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
