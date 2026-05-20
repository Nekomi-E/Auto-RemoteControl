#pragma once
#include "Common/Utils/Config.h"
#include "Common/Utils/ThreadSafeQueue.h"
#include "Common/Protocol/ControlMessage.h"
#include <memory>
#include <atomic>
#include <thread>
#include <vector>
#include <windows.h>

struct ID3D11Texture2D;

class ViewerSession {
public:
    ViewerSession();
    ~ViewerSession();

    bool Initialize(const ViewerConfig& config);
    void Start();
    void Stop();

    // Called by render thread only
    void RenderFrame();
    void OnResize(uint32_t width, uint32_t height);
    void OnKeyEvent(UINT msg, WPARAM wParam, LPARAM lParam);
    void OnMouseEvent(UINT msg, WPARAM wParam, LPARAM lParam);
    void OnRawInput(HRAWINPUT hRawInput);

    bool SetRenderWindow(HWND hwnd);
    uint32_t GetRemoteWidth() const { return m_remoteWidth; }
    uint32_t GetRemoteHeight() const { return m_remoteHeight; }

private:
    void RenderThread();
    void NetworkReceiveThread();
    void VideoDecodeThread();
    void AudioDecodeThread();
    void InputSendThread();

    ViewerConfig m_config;
    std::atomic<bool> m_running{false};

    // Network
    std::unique_ptr<class ViewerNetworkImpl> m_network;

    // Decoders
    std::unique_ptr<class MfVideoDecoder> m_videoDecoder;
    std::unique_ptr<class MfAudioDecoder> m_audioDecoder;

    // Renderers
    std::unique_ptr<class D3d11Renderer> m_renderer;
    std::unique_ptr<class WasapiAudioRenderer> m_audioRenderer;
    std::unique_ptr<class D2dOverlay> m_overlay;

    // Queues
    struct VideoPacket {
        std::vector<uint8_t> data;
        bool isKeyFrame = false;
        uint32_t timestampMs = 0;
    };
    struct AudioPacket {
        std::vector<uint8_t> data;
        uint32_t timestampMs = 0;
    };
    struct DecodedFrame {
        std::vector<uint8_t> data;          // RGBA for CPU renderer
        ID3D11Texture2D* nv12Texture = nullptr; // NV12 GPU texture (GPU path)
        uint32_t width = 0;
        uint32_t height = 0;
    };

    ThreadSafeQueue<VideoPacket> m_videoQueue{64};
    ThreadSafeQueue<AudioPacket> m_audioQueue{128};
    ThreadSafeQueue<Protocol::InputEvent> m_inputSendQueue{64};

    // Latest decoded frame for rendering
    std::mutex m_frameMutex;
    DecodedFrame m_latestFrame;

    // Decoded frame rate tracking (actual video FPS, not render/packet rate)
    std::atomic<uint32_t> m_decodedFrameCount{0};
    float m_decodedFps = 0.0f;
    float m_renderTargetFps = 60.0f;  // smoothed, updated each second

    // Resize synchronization (main thread → render thread)
    std::mutex m_renderMutex;
    bool m_pendingResize = false;
    uint32_t m_pendingWidth = 0;
    uint32_t m_pendingHeight = 0;

    // Threads
    std::vector<std::thread> m_threads;

    // Remote info (set after handshake)
    uint32_t m_remoteWidth = 1920;
    uint32_t m_remoteHeight = 1080;
    HWND m_hwnd = nullptr;
    uint32_t m_windowWidth = 0;
    uint32_t m_windowHeight = 0;
    std::atomic<bool> m_inputActive{false};
};
