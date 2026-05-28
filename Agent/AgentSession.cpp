#include "AgentSession.h"
#include "Common/Common.h"
#include "Common/Utils/Logger.h"
#include "Common/Utils/Timer.h"
#include "Common/Utils/DebugScreenshot.h"
#include <d3d11.h>
#include <thread>

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
	m_threads.emplace_back(&AgentSession::AcceptThread, this);//将工作线程添加到线程池中，保证其生命周期与AgentSession对象一致

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
                                   m_config.enableAudio, m_config.videoQuality,
                                   m_captureMgr->GetD3DDevice(),
                                   m_captureMgr->GetD3DContext())) {
        LOG_ERROR("Failed to initialize encoders");
        m_running = false;
        m_network->Shutdown();
        if (!m_threads.empty()) { m_threads[0].join(); m_threads.clear(); }
        return false;
    }

    // Notify network of actual encoder resolution (may differ from monitor due to HW limits)
    uint32_t actualEncWidth = m_encoderMgr->GetEncoderWidth();
    uint32_t actualEncHeight = m_encoderMgr->GetEncoderHeight();
    uint32_t codecType = m_encoderMgr->GetCodecType();
    m_network->SetScreenInfo(actualEncWidth, actualEncHeight);
    m_network->SetCodecType(codecType);

    LOG_INFO("  Encoder resolution: %ux%u (monitor: %ux%u)",
             actualEncWidth, actualEncHeight, encWidth, encHeight);

    LOG_INFO("Agent initialized successfully");
    LOG_INFO("  Encoder: %ux%u, FPS: %u, Codec: %s",
             encWidth, encHeight, m_config.targetFps,
             codecType == 1 ? "HEVC" : "H.264");
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
    // Stop encoder BEFORE capture manager — the encoder's internal encode
    // thread uses the D3D11 context that CaptureManager owns. Joining the
    // encode thread first ensures it has finished all GPU operations before
    // CaptureManager releases the D3D11 device/context.
    if (m_encoderMgr) m_encoderMgr->Stop();
    if (m_captureMgr) m_captureMgr->Stop();
}

void AgentSession::AcceptThread() {
    LOG_INFO("[Accept] Thread started, listening on port %u", m_config.port);
    while (m_running) {
        if (m_network->AcceptConnection(1000)) {
            m_clientConnected = true;
            LOG_INFO("[Accept] Viewer connected, starting session");

            // Process control messages while connected
            while (m_running && m_network->IsConnected()) {
				auto msg = m_network->ReceiveControlMessage(100);//100ms 线程在没有消息时也能及时响应停止信号，同时避免长时间阻塞在ReceiveControlMessage中无法处理其他事件（如网络 disconnect）。如果改成完全阻塞等待，可能会导致在Viewer断开连接或Agent停止时，AcceptThread无法及时退出。不过100ms的等待确实可能引入一些输入响应延迟，可以考虑更短的等待时间或者使用事件驱动的方式来通知线程有新消息。
				//TODO 利用事件驱动的方式通知线程有新消息，避免轮询等待带来的输入响应延迟
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
    uint32_t dropCount = 0;
    // Per-interval diagnostics
    uint32_t intervalFrames = 0, intervalDrops = 0;
    int64_t lastDiagTime = Timer::NowMs();
    int64_t lastFrameMs = 0; // for inter-frame gap measurement

    while (m_running) {
        if (!m_clientConnected) {
            Sleep(50);
            continue;
        }

        CapturedFrameGpu gpuFrame;
        if (m_captureMgr->AcquireFrameGpu(gpuFrame)) {
            frameCount++;
            intervalFrames++;
            if (frameCount == 1) {
                LOG_INFO("[Capture] First frame (GPU): %ux%u", gpuFrame.width, gpuFrame.height);
            }
            auto now = Timer::NowMs();

            // Log inter-frame gap if unusually large (>33ms = <30fps)
            if (lastFrameMs > 0) {
                int64_t gap = now - lastFrameMs;
                static uint32_t gapLogCount = 0;
                if (gap > 33 && (gapLogCount++ & 0x1F) == 0) {
                    LOG_WARNING("[Capture] inter-frame gap %lldms (%.1f fps equiv)", gap, 1000.0/gap);
                }
            }
            lastFrameMs = now;

            bool submitted = m_encoderMgr->SubmitVideoFrameGpu(gpuFrame.texture,
                                               gpuFrame.width, gpuFrame.height, now);
            m_captureMgr->ReleaseGpuFrame();
            if (!submitted) {
                gpuFrame.texture->Release();
                dropCount++;
                intervalDrops++;
                static uint32_t dropLogCount = 0;
                if ((dropLogCount++ & 0x3F) == 0) {
                    LOG_WARNING("[Capture] encoder queue full, dropping GPU frame (total drops=%u)", dropCount);
                }
            }
            continue;
        }

        // No new/different frame available. Yield briefly.
        Sleep(1);

        // Per-interval diagnostics every 5 seconds
        auto now = Timer::NowMs();
        if (now - lastDiagTime >= 5000) {
            LOG_INFO("[Capture] interval: %u captured, %u dropped, %u total",
                     intervalFrames, intervalDrops, frameCount);
            intervalFrames = 0;
            intervalDrops = 0;
            lastDiagTime = now;
        }
    }
    LOG_INFO("[Capture] Thread stopped, %u frames, %u drops", frameCount, dropCount);
}

void AgentSession::VideoEncodeThread() {
    LOG_INFO("[VideoEncode] Thread started");
    uint32_t frameCount = 0;
    uint32_t debugSaveCount = 0;

    while (m_running) {
        EncoderManager::EncodedFrame encFrame;
        if (m_encoderMgr->GetEncodedVideoFrame(encFrame, 50)) {
            size_t encSize = encFrame.data.size();
            auto encWidth = encFrame.width;
            auto encHeight = encFrame.height;
            auto encKey = encFrame.isKeyFrame;

            // DEBUG: Save first 5 encoded H.264 bitstreams (before move)
            if (debugSaveCount < 5 && encSize > 0) {
                SaveRawData("agent_encoded", encFrame.data.data(), encSize);
                debugSaveCount++;
            }

            VideoFrame vf;
            vf.data = std::move(encFrame.data);
            vf.isKeyFrame = encKey;
            vf.width = encWidth;
            vf.height = encHeight;
            vf.timestampMs = encFrame.timestampMs;
            m_videoSendQueue.tryPush(std::move(vf));
            frameCount++;
            if (frameCount == 1) {
                LOG_INFO("[VideoEncode] First encoded frame: %ux%u key=%d size=%zu",
                         encWidth, encHeight, encKey, encSize);
            }
        }
    }
    LOG_INFO("[VideoEncode] Thread stopped, %u frames", frameCount);
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
    uint32_t videoSent = 0, audioSent = 0, videoFail = 0;
    int64_t lastStatsTime = Timer::NowMs();

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
                if (videoSent == 1) {
                    LOG_INFO("[NetworkSend] First frame sent: key=%d size=%zu",
                             vf->isKeyFrame, vf->data.size());
                }
            } else {
                videoFail++;
                if (videoFail <= 3) {
                    LOG_WARNING("[NetworkSend] Send failed: key=%d size=%zu seq=%u",
                                vf->isKeyFrame, vf->data.size(), seq);
                }
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

        // Periodic stats
        auto now = Timer::NowMs();
        if (now - lastStatsTime >= 5000) {
            m_videoSent.store(videoSent);
            m_audioSent.store(audioSent);
            LOG_INFO("[NetworkSend] Sent: video=%u (fail=%u) audio=%u queue=%zu",
                     videoSent, videoFail, audioSent, m_videoSendQueue.size());
            lastStatsTime = now;
        }
    }
    LOG_INFO("[NetworkSend] Thread stopped, video=%u audio=%u fail=%u", videoSent, audioSent, videoFail);
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
    uint32_t lastVideoSent = 0, lastAudioSent = 0;

    while (m_running) {
        Sleep(5000); // Log stats every 5 seconds

        auto capStats = m_captureMgr ? m_captureMgr->GetStats() : CaptureManager::Stats{};
        auto encStats = m_encoderMgr ? m_encoderMgr->GetStats() : EncoderManager::Stats{};

        uint32_t videoSentNow = m_videoSent.load();
        uint32_t audioSentNow = m_audioSent.load();
        float sendFps = (videoSentNow - lastVideoSent) / 5.0f;

        LOG_INFO("[Stats] Capture: %.1f fps | Encode: %.1f fps | Send: %.1f fps | "
                 "Bitrate: %.1f Mbps | Queue: vq=%zu aq=%zu | "
                 "Total: cap=%u enc=%u sent=%u",
                 capStats.captureFps, encStats.encodeFps, sendFps,
                 encStats.currentBitrate / 1000000.0,
                 m_videoSendQueue.size(), m_audioSendQueue.size(),
                 capStats.capturedFrames, encStats.encodedFrames, videoSentNow);

        lastVideoSent = videoSentNow;
        lastAudioSent = audioSentNow;
    }
}
