#pragma once
#include <vector>
#include <cstdint>
#include <memory>
#include "CaptureManager.h" // for MonitorDesc

struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDXGIOutputDuplication;
struct ID3D11Texture2D;

class DxgiScreenCapture {
public:
    struct CapturedFrame {
        std::vector<uint8_t> data;//字节数组，按照BGRA格式存储逐行扫描的像素数据  每个像素占B,G,R,A四个字节
        uint32_t width = 0;
        uint32_t height = 0;
        int64_t timestampMs = 0;
    };

    DxgiScreenCapture();
    ~DxgiScreenCapture();

    bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);
    void Shutdown();

    bool AcquireFrame(CapturedFrame& outFrame);
    bool AcquireFrameGpu(CapturedFrameGpu& outFrame);
    void ReleaseGpuFrame(); // no-op: frame released inside AcquireFrameGpu after safe copy
    void SetTargetFps(uint32_t fps) { m_targetFps = fps; }
    std::vector<MonitorDesc> GetMonitors() const;

private:
    bool InitDuplicationForOutput(int outputIndex);
    bool AcquireFromMonitor(int index, CapturedFrame& outFrame);
    bool AcquireFromMonitorGpu(int index, CapturedFrameGpu& outFrame);

    static constexpr int kSafePoolSize = 12;

    struct MonitorCapture {
        IDXGIOutputDuplication* duplication = nullptr;
        MonitorDesc desc;
        ID3D11Texture2D* stagingTex = nullptr;
        ID3D11Texture2D* sampleStagingTex = nullptr; // small staging for pixel differencing
        std::vector<uint8_t> prevSample;             // previous frame's sample data
        int duplicateCount = 0;                      // consecutive identical frames
        uint32_t frameCounter = 0;                   // frames since init, drives periodic pixel diff
        bool valid = false;

        // Pre-allocated texture pool to avoid per-frame CreateTexture2D (~16MB)
        // overhead that causes 100-600ms GPU allocation stalls.
        ID3D11Texture2D* safePool[kSafePoolSize] = {};
        uint32_t poolWidth = 0;
        uint32_t poolHeight = 0;
        int poolIndex = 0;
    };

    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;
    std::vector<MonitorCapture> m_monitors;
    int m_currentMonitor = 0;
    int64_t m_lastFrameTime = 0;
    uint32_t m_targetFps = 60;
};

// Helper to save BGRA frame to BMP file (for debugging)
void SaveFrameToBmp(const uint8_t* data, uint32_t width, uint32_t height, const char* filename);
