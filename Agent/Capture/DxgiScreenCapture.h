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
        std::vector<uint8_t> data;
        uint32_t width = 0;
        uint32_t height = 0;
        int64_t timestampMs = 0;
    };

    DxgiScreenCapture();
    ~DxgiScreenCapture();

    bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);
    void Shutdown();

    bool AcquireFrame(CapturedFrame& outFrame);
    std::vector<MonitorDesc> GetMonitors() const;

private:
    bool InitDuplicationForOutput(int outputIndex);
    bool AcquireFromMonitor(int index, CapturedFrame& outFrame);

    struct MonitorCapture {
        IDXGIOutputDuplication* duplication = nullptr;
        MonitorDesc desc;
        ID3D11Texture2D* stagingTex = nullptr;
        bool valid = false;
    };

    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;
    std::vector<MonitorCapture> m_monitors;
    int m_currentMonitor = 0;
    int64_t m_lastFrameTime = 0;
};

// Helper to save BGRA frame to BMP file (for debugging)
void SaveFrameToBmp(const uint8_t* data, uint32_t width, uint32_t height, const char* filename);
