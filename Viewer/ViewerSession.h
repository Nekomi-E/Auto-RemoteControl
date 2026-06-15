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
    void SetInputActive(bool active) { m_inputActive = active; }

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

    ThreadSafeQueue<VideoPacket> m_videoQueue{96};
    ThreadSafeQueue<AudioPacket> m_audioQueue{128};
    ThreadSafeQueue<Protocol::InputEvent> m_inputSendQueue{64};

    // Latest decoded frame for rendering
    std::mutex m_frameMutex;
    DecodedFrame m_latestFrame;

    // Condition variable signalled by the decode thread when a new frame is
    // ready.  The render thread blocks on this CV instead of busy-spinning
    // at the target FPS when no new decoded frames are available.
    std::condition_variable m_frameCv;

    // Frame freshness tracking — the decode thread bumps m_decodedFrameId
    // on each successful decode and notifies m_frameCv; the render thread
    // only presents when m_decodedFrameId > m_lastRenderedFrameId, avoiding
    // redundant GPU work and breaking the EWMA→queue-buildup→drop feedback loop.
    std::atomic<uint32_t> m_decodedFrameId{0};
    uint32_t m_lastRenderedFrameId = 0;

    // Decoded FPS for overlay display (informational only, not used for pacing)
    std::atomic<uint32_t> m_decodedFrameCount{0};
    float m_displayFps = 0.0f;
    float m_targetRenderFps = 60.0f;
    bool m_variableFrameRate = false;   // VFR: render at decode rate, no fixed pacing

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
    bool m_timerResolutionSet = false;
};
