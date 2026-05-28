#pragma once
#include "Common/Utils/Config.h"
#include "Common/Security/SecureChannel.h"
#include "Common/Utils/ThreadSafeQueue.h"
#include "Capture/CaptureManager.h"
#include "Encoder/EncoderManager.h"
#include "Network/AgentNetworkImpl.h"
#include "Input/InputInjectorImpl.h"
#include <atomic>
#include <thread>
#include <memory>

class AgentSession {
public:
    AgentSession();
    ~AgentSession();

    bool Initialize(const AgentConfig& config);
    void Run();
    void Stop();

private:
    void AcceptThread();
    void CaptureThread();
    void VideoEncodeThread();
    void AudioCaptureThread();
    void AudioEncodeThread();
    void NetworkSendThread();
    void InputInjectThread();
    void StatsThread();

    AgentConfig m_config;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_clientConnected{false};

    // Subsystems
    std::unique_ptr<CaptureManager> m_captureMgr;
    std::unique_ptr<EncoderManager> m_encoderMgr;
    std::unique_ptr<AgentNetworkImpl> m_network;
    std::unique_ptr<InputInjectorImpl> m_inputInjector;

    // Queues
    struct VideoFrame {
		std::vector<uint8_t> data;// Encoded video data (e.g. H.264 NAL units)
        bool isKeyFrame = false;
        uint32_t width = 0;
        uint32_t height = 0;
        int64_t timestampMs = 0;
    };
    struct AudioPacket {
		std::vector<uint8_t> data;// Encoded audio data (e.g. AAC frames)
        int64_t timestampMs = 0;
    };

    ThreadSafeQueue<VideoFrame> m_videoSendQueue{32};
    ThreadSafeQueue<AudioPacket> m_audioSendQueue{64};
    ThreadSafeQueue<Protocol::InputEvent> m_inputQueue{32};

    // Workers
    std::vector<std::thread> m_threads;

    // Sequence numbers
    std::atomic<uint16_t> m_videoSeq{0};
    std::atomic<uint16_t> m_audioSeq{0};
    std::atomic<uint32_t> m_videoSent{0};
    std::atomic<uint32_t> m_audioSent{0};
    int64_t m_sessionStartMs = 0;
};
