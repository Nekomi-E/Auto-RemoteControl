#pragma once
#include <vector>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <memory>

// Forward declarations
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;  // for CapturedFrameGpu

namespace Protocol { struct InputEvent; }

struct MonitorDesc {
    uint32_t width = 0;
    uint32_t height = 0;
    int32_t x = 0;
    int32_t y = 0;
    bool isPrimary = false;
};

// GPU captured frame: BGRA texture on GPU, no CPU readback.
// caller must call ReleaseGpuFrame() after processing is done,
// then Release() on the texture.
struct CapturedFrameGpu {
    ID3D11Texture2D* texture = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    int64_t timestampMs = 0;
};

class CaptureManager {
public:
    struct Stats {
        float captureFps = 0;
        float audioFps = 0;
        uint32_t capturedFrames = 0;
    };

    CaptureManager();
    ~CaptureManager();

    bool Initialize(uint32_t targetFps);
    void Stop();

    bool AcquireFrame(std::vector<uint8_t>& outData, uint32_t& width, uint32_t& height);
    bool AcquireAudio(std::vector<uint8_t>& outData);

    // GPU path: returns the captured texture directly (no CPU readback).
    bool AcquireFrameGpu(CapturedFrameGpu& outFrame);
    void ReleaseGpuFrame();

    std::vector<MonitorDesc> GetMonitorDescs() const;
    void SetTargetFps(uint32_t fps);

    Stats GetStats() const;

    // Access to shared D3D11 device for encoder interop
    ID3D11Device* GetD3DDevice() const;
    ID3D11DeviceContext* GetD3DContext() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
