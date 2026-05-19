#include "AgentSession.h"
#include "Common/Common.h"
#include "Common/Utils/Logger.h"
#include "Common/Utils/Timer.h"

AgentSession::AgentSession() {}

AgentSession::~AgentSession() { Stop(); }

bool AgentSession::Initialize(const AgentConfig& config) {
    m_config = config;

    // Init COM for Media Foundation
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        LOG_ERROR("CoInitializeEx failed: 0x%08X", hr);
        return false;
    }

    // Init Winsock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        LOG_ERROR("WSAStartup failed");
        return false;
    }

    // Create subsystems
    m_captureMgr = std::make_unique<CaptureManager>();
    m_encoderMgr = std::make_unique<EncoderManager>();
    m_network = std::make_unique<AgentNetworkImpl>();
    m_inputInjector = std::make_unique<InputInjectorImpl>();

    // Initialize network (starts listening)
    if (!m_network->Initialize(m_config.port, m_config.password,
                               m_config.enableEncryption)) {
        LOG_ERROR("Failed to initialize network");
        return false;
    }

    // Start accept thread early so Viewer can connect while we initialize
    m_running = true;
    m_threads.emplace_back(&AgentSession::AcceptThread, this);

    // Initialize capture
    if (!m_captureMgr->Initialize(m_config.targetFps)) {
        LOG_ERROR("Failed to initialize capture");
        m_running = false;
        m_network->Shutdown();
        if (!m_threads.empty()) { m_threads[0].join(); m_threads.clear(); }
        return false;
    }

    // Initialize encoder (deferred until we know the screen resolution)
    auto monitors = m_captureMgr->GetMonitorDescs();
    if (monitors.empty()) {
        LOG_ERROR("No monitors detected");
        m_running = false;
        m_network->Shutdown();
        if (!m_threads.empty()) { m_threads[0].join(); m_threads.clear(); }
        return false;
    }
    // Use primary monitor's dimensions for encoder init
    uint32_t encWidth = monitors[0].width;
    uint32_t encHeight = monitors[0].height;
    if (!m_encoderMgr->Initialize(encWidth, encHeight,
                                   m_config.videoBitrate, m_config.targetFps,
                                   m_config.enableAudio)) {
        LOG_ERROR("Failed to initialize encoders");
        m_running = false;
        m_network->Shutdown();
        if (!m_threads.empty()) { m_threads[0].join(); m_threads.clear(); }
        return false;
    }

    LOG_INFO("Agent initialized successfully");
    LOG_INFO("  Monitor: %ux%u, Bitrate: %u bps, FPS: %u",
             encWidth, encHeight, m_config.videoBitrate, m_config.targetFps);
    return true;
}

void AgentSession::Run() {
    m_running = true;
    m_sessionStartMs = Timer::NowMs();

    LOG_INFO("Agent running, waiting for viewer connection on port %u...", m_config.port);
    LOG_INFO("Use Scroll Lock key on Viewer to toggle input control.");

    // Accept thread was started in Initialize() already
    m_threads.emplace_back(&AgentSession::CaptureThread, this);
    m_threads.emplace_back(&AgentSession::VideoEncodeThread, this);
    m_threads.emplace_back(&AgentSession::NetworkSendThread, this);
    m_threads.emplace_back(&AgentSession::InputInjectThread, this);
    m_threads.emplace_back(&AgentSession::StatsThread, this);

    if (m_config.enableAudio) {
        m_threads.emplace_back(&AgentSession::AudioCaptureThread, this);
        m_threads.emplace_back(&AgentSession::AudioEncodeThread, this);
    }

    // Main thread: wait for stop signal
    LOG_INFO("Agent running. Press Ctrl+C in console to stop.");
    while (m_running) {
        Sleep(500);
    }

    // Shutdown
    for (auto& t : m_threads) {
        if (t.joinable()) t.join();
    }
}

void AgentSession::Stop() {
    m_running = false;
    m_videoSendQueue.close();
    m_audioSendQueue.close();
    m_inputQueue.close();
    if (m_network) m_network->Shutdown();
    if (m_captureMgr) m_captureMgr->Stop();
    if (m_encoderMgr) m_encoderMgr->Stop();
}

void AgentSession::AcceptThread() {
    LOG_INFO("[Accept] Thread started");
    while (m_running) {
        if (m_network->AcceptConnection(1000)) {
            m_clientConnected = true;
            LOG_INFO("[Accept] Viewer connected, starting session");

            // Process control messages while connected
            while (m_running && m_network->IsConnected()) {
                auto msg = m_network->ReceiveControlMessage(100);
                if (msg) {
                    if (msg->type == Protocol::MessageType::INPUT_EVENT) {
                        for (auto& ev : msg->inputEvents) {
                            m_inputQueue.tryPush(std::move(ev));
                        }
                    } else if (msg->type == Protocol::MessageType::SESSION_STOP) {
                        LOG_INFO("[Accept] Viewer requested session stop");
                        break;
                    } else if (msg->type == Protocol::MessageType::QUALITY_ADJUST) {
                        if (msg->targetBitrate) {
                            m_encoderMgr->AdjustBitrate(*msg->targetBitrate);
                        }
                        if (msg->targetFps) {
                            m_captureMgr->SetTargetFps(*msg->targetFps);
                        }
                    }
                }
            }
            m_clientConnected = false;
            LOG_INFO("[Accept] Viewer disconnected");
        }
    }
    LOG_INFO("[Accept] Thread stopped");
}

void AgentSession::CaptureThread() {
    LOG_INFO("[Capture] Thread started");
    uint32_t frameCount = 0;

    while (m_running) {
        if (!m_clientConnected) {
            Sleep(50);
            continue;
        }

        uint32_t width, height;
        std::vector<uint8_t> frameData;
        if (m_captureMgr->AcquireFrame(frameData, width, height)) {
            m_encoderMgr->SubmitVideoFrame(frameData, width, height, Timer::NowMs());
            frameCount++;
        }
    }
    LOG_INFO("[Capture] Thread stopped, %u frames captured", frameCount);
}

void AgentSession::VideoEncodeThread() {
    LOG_INFO("[VideoEncode] Thread started");

    while (m_running) {
        EncoderManager::EncodedFrame encFrame;
        if (m_encoderMgr->GetEncodedVideoFrame(encFrame, 50)) {
            VideoFrame vf;
            vf.data = std::move(encFrame.data);
            vf.isKeyFrame = encFrame.isKeyFrame;
            vf.width = encFrame.width;
            vf.height = encFrame.height;
            vf.timestampMs = encFrame.timestampMs;
            m_videoSendQueue.tryPush(std::move(vf));
        }
    }
    LOG_INFO("[VideoEncode] Thread stopped");
}

void AgentSession::AudioCaptureThread() {
    LOG_INFO("[AudioCapture] Thread started");

    while (m_running) {
        if (!m_clientConnected) {
            Sleep(50);
            continue;
        }

        std::vector<uint8_t> audioData;
        if (m_captureMgr->AcquireAudio(audioData)) {
            m_encoderMgr->SubmitAudioFrame(audioData, Timer::NowMs());
        }
    }
    LOG_INFO("[AudioCapture] Thread stopped");
}

void AgentSession::AudioEncodeThread() {
    LOG_INFO("[AudioEncode] Thread started");

    while (m_running) {
        EncoderManager::EncodedFrame encFrame;
        if (m_encoderMgr->GetEncodedAudioFrame(encFrame, 50)) {
            AudioPacket ap;
            ap.data = std::move(encFrame.data);
            ap.timestampMs = encFrame.timestampMs;
            m_audioSendQueue.tryPush(std::move(ap));
        }
    }
    LOG_INFO("[AudioEncode] Thread stopped");
}

void AgentSession::NetworkSendThread() {
    LOG_INFO("[NetworkSend] Thread started");
    uint32_t videoSent = 0, audioSent = 0;

    while (m_running) {
        // Send video frames (higher priority)
        auto vf = m_videoSendQueue.tryPop(2);
        if (vf) {
            Protocol::FrameType type = vf->isKeyFrame
                ? Protocol::FrameType::VIDEO_KEYFRAME
                : Protocol::FrameType::VIDEO_DELTA;
            uint16_t seq = m_videoSeq.fetch_add(1);
            if (m_network->SendDataFrame(type, seq, vf->timestampMs,
                                         vf->data.data(), vf->data.size())) {
                videoSent++;
            }
        }

        // Send audio frames
        auto af = m_audioSendQueue.tryPop(1);
        if (af) {
            uint16_t seq = m_audioSeq.fetch_add(1);
            if (m_network->SendDataFrame(Protocol::FrameType::AUDIO_FRAME, seq,
                                         af->timestampMs,
                                         af->data.data(), af->data.size())) {
                audioSent++;
            }
        }
    }
    LOG_INFO("[NetworkSend] Thread stopped, video=%u audio=%u", videoSent, audioSent);
}

void AgentSession::InputInjectThread() {
    LOG_INFO("[InputInject] Thread started");

    while (m_running) {
        auto ev = m_inputQueue.tryPop(50);
        if (ev) {
            m_inputInjector->Inject(*ev);
        }
    }
    LOG_INFO("[InputInject] Thread stopped");
}

void AgentSession::StatsThread() {
    LOG_INFO("[Stats] Thread started");

    while (m_running) {
        Sleep(5000); // Log stats every 5 seconds

        auto capStats = m_captureMgr ? m_captureMgr->GetStats() : CaptureManager::Stats{};
        auto encStats = m_encoderMgr ? m_encoderMgr->GetStats() : EncoderManager::Stats{};

        LOG_INFO("[Stats] Capture: %.1f fps, Encode: %.1f fps, "
                 "Bitrate: %.1f Mbps, Queue: video=%zu audio=%zu",
                 capStats.captureFps, encStats.encodeFps,
                 encStats.currentBitrate / 1000000.0,
                 m_videoSendQueue.size(), m_audioSendQueue.size());
    }
}
