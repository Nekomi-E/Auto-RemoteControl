#include "ViewerSession.h"
#include "Network/ViewerNetworkImpl.h"
#include "Decoder/MfVideoDecoder.h"
#include "Decoder/MfAudioDecoder.h"
#include "Render/D3d11Renderer.h"
#include "Render/D2dOverlay.h"
#include "Render/WasapiAudioRenderer.h"
#include "Input/RawInputCapture.h"
#include "Common/Utils/Logger.h"
#include "Common/Utils/Timer.h"
#include "Common/Utils/DebugScreenshot.h"
#include <d3d11.h>
#include <thread>

ViewerSession::ViewerSession() {}

ViewerSession::~ViewerSession() { Stop(); }

bool ViewerSession::Initialize(const ViewerConfig& config) {
    m_config = config;

    // Create subsystems
    m_network = std::make_unique<ViewerNetworkImpl>();
    m_videoDecoder = std::make_unique<MfVideoDecoder>();
    m_audioDecoder = std::make_unique<MfAudioDecoder>();
    m_renderer = std::make_unique<D3d11Renderer>();
    m_overlay = std::make_unique<D2dOverlay>();
    m_audioRenderer = std::make_unique<WasapiAudioRenderer>();

    // Connect to Agent
    if (!m_network->Connect(m_config.host, m_config.port, m_config.password,
                             m_config.enableEncryption)) {
        LOG_ERROR("Failed to connect to %s:%u", m_config.host.c_str(), m_config.port);
        return false;
    }

    // Get remote screen info from session start
    m_remoteWidth = m_network->GetRemoteWidth();
    m_remoteHeight = m_network->GetRemoteHeight();
    uint32_t codecType = m_network->GetCodecType();

    // Initialize decoders
    if (!m_videoDecoder->Initialize(codecType, m_remoteWidth, m_remoteHeight)) {
        LOG_ERROR("Failed to initialize video decoder (codec: %s)",
                  codecType == 1 ? "HEVC" : "H.264");
        return false;
    }

    if (m_config.enableAudio) {
        if (!m_audioDecoder->Initialize()) {
            LOG_WARNING("Audio decoder init failed, continuing without audio");
            m_audioDecoder.reset();
            m_config.enableAudio = false;
        }
    }

    // Initialize audio renderer
    if (m_config.enableAudio) {
        if (!m_audioRenderer->Initialize(48000, 2)) {
            LOG_WARNING("Audio renderer init failed");
            m_config.enableAudio = false;
        }
    }

    LOG_INFO("Viewer session initialized, remote screen: %ux%u codec: %s",
             m_remoteWidth, m_remoteHeight,
             codecType == 1 ? "HEVC" : "H.264");
    return true;
}

bool ViewerSession::SetRenderWindow(HWND hwnd) {
    m_hwnd = hwnd;
    RECT rect;
    GetClientRect(hwnd, &rect);
    m_windowWidth = rect.right - rect.left;
    m_windowHeight = rect.bottom - rect.top;

    // Initialize D3D11 renderer with the window
    if (!m_renderer->Initialize(hwnd, m_remoteWidth, m_remoteHeight)) {
        LOG_ERROR("Failed to initialize D3D11 renderer");
        return false;
    }
    m_overlay->Initialize(hwnd, m_renderer->GetD2DDeviceContext());

    // Enable GPU decode path if the decoder can use the renderer's D3D11 device
    LOG_INFO("Checking GPU decode path: decoder=%p rendererDevice=%p rendererContext=%p",
             (void*)m_videoDecoder.get(),
             (void*)m_renderer->GetDevice(),
             (void*)m_renderer->GetContext());
    if (m_videoDecoder && m_renderer->GetDevice()) {
        m_videoDecoder->InitializeWithD3D11(
            m_network->GetCodecType(), m_remoteWidth, m_remoteHeight,
            m_renderer->GetDevice(), m_renderer->GetContext());
    } else {
        LOG_INFO("GPU decode path skipped: decoder=%d device=%p",
                 m_videoDecoder ? 1 : 0, (void*)m_renderer->GetDevice());
    }

    return true;
}

void ViewerSession::Start() {
    m_running = true;

    m_threads.emplace_back(&ViewerSession::RenderThread, this);
    m_threads.emplace_back(&ViewerSession::NetworkReceiveThread, this);
    m_threads.emplace_back(&ViewerSession::VideoDecodeThread, this);
    m_threads.emplace_back(&ViewerSession::InputSendThread, this);

    if (m_config.enableAudio) {
        m_threads.emplace_back(&ViewerSession::AudioDecodeThread, this);
    }

    LOG_INFO("Viewer session started");
}

void ViewerSession::Stop() {
    m_running = false;
    m_videoQueue.close();
    m_audioQueue.close();
    m_inputSendQueue.close();

    if (m_network) m_network->Disconnect();

    for (auto& t : m_threads) {
        if (t.joinable()) t.join();
    }

    if (m_latestFrame.nv12Texture) {
        m_latestFrame.nv12Texture->Release();
        m_latestFrame.nv12Texture = nullptr;
    }

    if (m_renderer) m_renderer->Shutdown();
    if (m_overlay) m_overlay->Shutdown();
    if (m_audioRenderer) m_audioRenderer->Shutdown();
}

void ViewerSession::RenderFrame() {
    if (!m_running) return;

    // Snapshot the latest frame under the mutex, then release it so the decode
    // thread can keep producing frames while we render on the GPU.
    ID3D11Texture2D* nv12Tex = nullptr;
    uint32_t nv12W = 0, nv12H = 0;
    std::vector<uint8_t> cpuFrame;

    {
        std::lock_guard lock(m_frameMutex);
        if (m_latestFrame.nv12Texture) {
            nv12Tex = m_latestFrame.nv12Texture;
            nv12Tex->AddRef();
            nv12W = m_latestFrame.width;
            nv12H = m_latestFrame.height;
        } else if (m_latestFrame.width > 0 && !m_latestFrame.data.empty()) {
            cpuFrame = m_latestFrame.data;
            nv12W = m_latestFrame.width;
            nv12H = m_latestFrame.height;
        }
    }

    if (nv12Tex) {
        m_renderer->RenderFrameNv12(nv12Tex, nv12W, nv12H);
        nv12Tex->Release();
    } else if (!cpuFrame.empty()) {
        m_renderer->RenderFrame(cpuFrame.data(), nv12W, nv12H);
    }

    // Render overlay — show actual decoded video FPS, not network packet rate
    m_overlay->Draw(m_decodedFps,
                    m_network ? m_network->IsConnected() : false);

    // Present
    m_renderer->Present();
}

void ViewerSession::OnResize(uint32_t width, uint32_t height) {
    m_windowWidth = width;
    m_windowHeight = height;
    // Defer the actual D3D11 resize to the render thread — calling ResizeBuffers
    // from the main thread races with Present on the render thread.
    {
        std::lock_guard lock(m_renderMutex);
        m_pendingResize = true;
        m_pendingWidth = width;
        m_pendingHeight = height;
    }
}

void ViewerSession::OnKeyEvent(UINT msg, WPARAM wParam, LPARAM lParam) {
    if (!m_inputActive || !m_network) return;

    Protocol::InputEvent ev;
    ev.timestamp = Timer::NowMs();

    bool isDown = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
    ev.type = isDown ? Protocol::InputType::KEY_DOWN : Protocol::InputType::KEY_UP;

    Protocol::KeyEvent ke;
    ke.vkCode = static_cast<uint16_t>(wParam);
    ke.extended = (HIWORD(lParam) & KF_EXTENDED) != 0;
    ev.key = ke;

    m_inputSendQueue.tryPush(ev);
}

void ViewerSession::OnMouseEvent(UINT msg, WPARAM wParam, LPARAM lParam) {
    if (!m_inputActive || !m_network) return;

    Protocol::InputEvent ev;
    ev.timestamp = Timer::NowMs();

    switch (msg) {
    case WM_MOUSEMOVE:
        ev.type = Protocol::InputType::MOUSE_MOVE;
        ev.mouseMove = Protocol::MouseMoveEvent{
            static_cast<int16_t>(LOWORD(lParam)),
            static_cast<int16_t>(HIWORD(lParam))
        };
        break;
    case WM_LBUTTONDOWN:
        ev.type = Protocol::InputType::MOUSE_BUTTON_DOWN;
        ev.mouseButton = Protocol::MouseButtonEvent{0};
        break;
    case WM_LBUTTONUP:
        ev.type = Protocol::InputType::MOUSE_BUTTON_UP;
        ev.mouseButton = Protocol::MouseButtonEvent{0};
        break;
    case WM_RBUTTONDOWN:
        ev.type = Protocol::InputType::MOUSE_BUTTON_DOWN;
        ev.mouseButton = Protocol::MouseButtonEvent{1};
        break;
    case WM_RBUTTONUP:
        ev.type = Protocol::InputType::MOUSE_BUTTON_UP;
        ev.mouseButton = Protocol::MouseButtonEvent{1};
        break;
    case WM_MBUTTONDOWN:
        ev.type = Protocol::InputType::MOUSE_BUTTON_DOWN;
        ev.mouseButton = Protocol::MouseButtonEvent{2};
        break;
    case WM_MBUTTONUP:
        ev.type = Protocol::InputType::MOUSE_BUTTON_UP;
        ev.mouseButton = Protocol::MouseButtonEvent{2};
        break;
    case WM_MOUSEWHEEL:
        ev.type = Protocol::InputType::MOUSE_WHEEL;
        ev.mouseWheel = Protocol::MouseWheelEvent{
            static_cast<int16_t>(GET_WHEEL_DELTA_WPARAM(wParam))
        };
        break;
    default:
        return;
    }
    m_inputSendQueue.tryPush(ev);
}

void ViewerSession::OnRawInput(HRAWINPUT hRawInput) {
    if (!m_inputActive || !m_network) return;

    UINT size = 0;
    GetRawInputData(hRawInput, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
    if (size == 0) return;

    std::vector<BYTE> buf(size);
    GetRawInputData(hRawInput, RID_INPUT, buf.data(), &size, sizeof(RAWINPUTHEADER));

    RAWINPUT* raw = reinterpret_cast<RAWINPUT*>(buf.data());

    if (raw->header.dwType == RIM_TYPEMOUSE) {
        Protocol::InputEvent ev;
        ev.timestamp = Timer::NowMs();

        if (raw->data.mouse.usFlags & MOUSE_MOVE_RELATIVE) {
            ev.type = Protocol::InputType::MOUSE_MOVE;
            ev.mouseMove = Protocol::MouseMoveEvent{
                static_cast<int16_t>(raw->data.mouse.lLastX),
                static_cast<int16_t>(raw->data.mouse.lLastY)
            };
            m_inputSendQueue.tryPush(ev);
        }
    }
}

void ViewerSession::RenderThread() {
    LOG_INFO("[Render] Thread started");
    int64_t lastFrameTime = Timer::NowMs();
    uint32_t frameCount = 0;
    int64_t lastStatsTime = lastFrameTime;
    int64_t lastFpsUpdateTime = lastFrameTime;
    uint32_t lastDecodedCount = 0;

    while (m_running) {
        // Check for pending resize from the main thread
        {
            std::lock_guard lock(m_renderMutex);
            if (m_pendingResize) {
                if (m_renderer) {
                    m_renderer->Resize(m_pendingWidth, m_pendingHeight);
                }
                m_pendingResize = false;
            }
        }

        RenderFrame();
        frameCount++;

        auto now = Timer::NowMs();

        // Update decoded FPS every second for smooth overlay display
        if (now - lastFpsUpdateTime >= 1000) {
            uint32_t decodedNow = m_decodedFrameCount.load();
            float instantFps = static_cast<float>(
                (decodedNow - lastDecodedCount) * 1000.0f / (now - lastFpsUpdateTime));
            m_decodedFps = instantFps;
            lastDecodedCount = decodedNow;
            lastFpsUpdateTime = now;

            // Smooth the render target to avoid oscillation when decode rate varies
            if (m_renderTargetFps == 60.0f && instantFps > 0.0f) {
                m_renderTargetFps = instantFps;
            } else if (instantFps > 0.0f) {
                m_renderTargetFps = m_renderTargetFps * 0.7f + instantFps * 0.3f;
            }
            if (m_renderTargetFps < 10.0f) m_renderTargetFps = 10.0f;
            if (m_renderTargetFps > 60.0f) m_renderTargetFps = 60.0f;
        }

        // Pace to actual decode rate so each new frame gets one present,
        // avoiding irregular frame duplication that causes visible stutter.
        int64_t targetInterval = static_cast<int64_t>(1000.0f / m_renderTargetFps);
        int64_t elapsed = now - lastFrameTime;
        if (elapsed < targetInterval) {
            Sleep(static_cast<DWORD>(targetInterval - elapsed));
        }
        lastFrameTime = Timer::NowMs();

        // Periodic stats (5s interval) — more detailed log
        if (now - lastStatsTime >= 5000) {
            double renderFps = frameCount * 1000.0 / (now - lastStatsTime);
            LOG_INFO("[Render] render=%.1f fps  video=%.1f fps  frames=%u",
                     renderFps, m_decodedFps, frameCount);
            frameCount = 0;
            lastStatsTime = now;
        }
    }
    LOG_INFO("[Render] Thread stopped");
}

void ViewerSession::NetworkReceiveThread() {
    LOG_INFO("[NetRecv] Thread started");
    uint32_t totalPackets = 0, videoPackets = 0;
    int64_t lastStatsTime = Timer::NowMs();
    uint32_t debugSaveCount = 0;

    while (m_running) {
        // Receive data frame
        ViewerNetworkImpl::DataPacket packet;
        if (m_network->ReceiveDataFrame(packet, 10)) {
            totalPackets++;
            if (packet.type == Protocol::FrameType::VIDEO_KEYFRAME ||
                packet.type == Protocol::FrameType::VIDEO_DELTA) {
                videoPackets++;

                // DEBUG: Save first 5 received H.264 bitstreams
                if (debugSaveCount < 5 && !packet.data.empty()) {
                    SaveRawData("viewer_received", packet.data.data(), packet.data.size());
                    debugSaveCount++;
                }

                VideoPacket vp;
                vp.data = std::move(packet.data);
                vp.isKeyFrame = (packet.type == Protocol::FrameType::VIDEO_KEYFRAME);
                vp.timestampMs = packet.timestampMs;
                // If the queue is full, drop the incoming frame. Log periodically
                // so we can detect sustained backpressure without spamming logs.
                if (!m_videoQueue.tryPush(std::move(vp))) {
                    static uint32_t dropLogCount = 0;
                    if ((dropLogCount++ & 0x3F) == 0) {
                        LOG_WARNING("[NetRecv] video queue full, dropping frame");
                    }
                }
            } else if (packet.type == Protocol::FrameType::AUDIO_FRAME) {
                AudioPacket ap;
                ap.data = std::move(packet.data);
                ap.timestampMs = packet.timestampMs;
                m_audioQueue.tryPush(std::move(ap));
            }
        }

        // Periodic stats
        auto now = Timer::NowMs();
        if (now - lastStatsTime >= 5000) {
            LOG_INFO("[NetRecv] Packets: total=%u video=%u queue=%zu",
                     totalPackets, videoPackets, m_videoQueue.size());
            lastStatsTime = now;
        }

        // Check for incoming control messages
        auto ctrlMsg = m_network->ReceiveControlMessage(1);
        if (ctrlMsg && ctrlMsg->type == Protocol::MessageType::SESSION_STOP) {
            LOG_INFO("[NetRecv] Remote session ended");
            m_running = false;
        }
    }
    LOG_INFO("[NetRecv] Thread stopped, total=%u video=%u", totalPackets, videoPackets);
}

void ViewerSession::VideoDecodeThread() {
    LOG_INFO("[VideoDecode] Thread started");
    uint32_t frames = 0, fails = 0, gpuFrames = 0;
    int64_t lastDebugSaveMs = 0;
    uint32_t debugSaveCount = 0;

    while (m_running) {
        auto vp = m_videoQueue.tryPop(50);
        if (!vp) continue;

        // Log occasional debug info when packets are being consumed to help
        // diagnose stalls (coarse sampling to avoid log spam).
        static uint32_t consumeLogCount = 0;
        if ((consumeLogCount++ & 0x7F) == 0) {
            LOG_INFO("[VideoDecode] Consuming packet input size=%zu key=%d queue=%zu",
                     vp->data.size(), vp->isKeyFrame ? 1 : 0, m_videoQueue.size());
        }

        if (m_videoDecoder->HasGpuPath()) {
            // GPU path: DecodeFrameGpu feeds the data and drains ALL output.
            // DrainDecoderOutputGpu handles both GPU-backed and system-memory
            // buffers internally (CPU→GPU upload fallback). The MFT consumes
            // the input in ProcessInput — do NOT call DecodeFrame afterwards
            // (that would double-feed the same bitstream, causing the 50/50
            // GPU/CPU decode split when the MFT surface pool alternates).
            ID3D11Texture2D* nv12Tex = nullptr;
            uint32_t width = 0, height = 0;
            if (m_videoDecoder->DecodeFrameGpu(vp->data.data(), vp->data.size(),
                                                nv12Tex, width, height)) {
                std::lock_guard lock(m_frameMutex);
                if (m_latestFrame.nv12Texture) m_latestFrame.nv12Texture->Release();
                m_latestFrame.nv12Texture = nv12Tex;
                m_latestFrame.data.clear();
                m_latestFrame.width = width;
                m_latestFrame.height = height;
                frames++;
                gpuFrames++;
                m_decodedFrameCount++;
                if (frames == 1) {
                    LOG_INFO("[VideoDecode] First frame decoded (GPU): %ux%u (key=%d input=%zu bytes)",
                             width, height, vp->isKeyFrame, vp->data.size());
                }
            }
            // No fallback — data already consumed by ProcessInput
        } else {
            // Pure CPU path: no GPU decode available at all
            std::vector<uint8_t> rgba;
            uint32_t width = 0, height = 0;
            if (m_videoDecoder->DecodeFrame(vp->data.data(), vp->data.size(),
                                             rgba, width, height)) {
                std::lock_guard lock(m_frameMutex);
                if (m_latestFrame.nv12Texture) {
                    m_latestFrame.nv12Texture->Release();
                    m_latestFrame.nv12Texture = nullptr;
                }
                m_latestFrame.data = std::move(rgba);
                m_latestFrame.width = width;
                m_latestFrame.height = height;
                frames++;
                m_decodedFrameCount++;
                if (frames == 1) {
                    LOG_INFO("[VideoDecode] First frame decoded (CPU): %ux%u (key=%d input=%zu bytes)",
                             width, height, vp->isKeyFrame, vp->data.size());
                }

                // DEBUG: Save decoded RGBA frame to BMP every ~5 seconds (up to 10)
                auto now = Timer::NowMs();
                if (debugSaveCount < 10 && (debugSaveCount == 0 || now - lastDebugSaveMs >= 5000)) {
                    lastDebugSaveMs = now;
                    debugSaveCount++;
                    std::vector<uint8_t> copyForSave = m_latestFrame.data;
                    std::thread([](std::vector<uint8_t> data, uint32_t w, uint32_t h, uint32_t cnt) {
                        char prefix[64];
                        snprintf(prefix, sizeof(prefix), "viewer_decoded_%u", cnt);
                        SaveBmp(prefix, data.data(), w, h, false);
                    }, std::move(copyForSave), m_latestFrame.width, m_latestFrame.height, debugSaveCount).detach();
                }
            } else {
                fails++;
                if ((fails & 0x1F) == 0) {
                    LOG_WARNING("[VideoDecode] consecutive decode failures=%u", fails);
                }
            }
        }
    }
    LOG_INFO("[VideoDecode] Thread stopped, %u frames (%u GPU, %u CPU), %u fails",
             frames, gpuFrames, frames - gpuFrames, fails);
}

void ViewerSession::AudioDecodeThread() {
    LOG_INFO("[AudioDecode] Thread started");

    while (m_running) {
        auto ap = m_audioQueue.tryPop(50);
        if (!ap) continue;

        std::vector<uint8_t> pcm;
        if (m_audioDecoder->DecodeFrame(ap->data, pcm)) {
            m_audioRenderer->Play(pcm);
        }
    }
    LOG_INFO("[AudioDecode] Thread stopped");
}

void ViewerSession::InputSendThread() {
    LOG_INFO("[InputSend] Thread started");

    while (m_running) {
        auto ev = m_inputSendQueue.tryPop(20);
        if (ev && m_network) {
            Protocol::ControlMessage msg;
            msg.type = Protocol::MessageType::INPUT_EVENT;
            msg.inputEvents.push_back(*ev);
            m_network->SendControlMessage(msg);
        }
    }
    LOG_INFO("[InputSend] Thread stopped");
}
