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
#include <algorithm>
#include <thread>
#include <timeapi.h>

#pragma comment(lib, "winmm.lib")

// Precision sleep with spin-finish — see Agent/Capture/DxgiScreenCapture.cpp
// for the rationale.  Used by the render thread instead of raw Sleep().
static void PrecisionSleepMs(int64_t targetMs) {
    if (targetMs <= 0) return;
    if (targetMs > 2) {
        Sleep(static_cast<DWORD>(targetMs - 2));
    }
    LARGE_INTEGER freq, start;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    int64_t targetTicks = freq.QuadPart * targetMs / 1000;
    for (;;) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (now.QuadPart - start.QuadPart >= targetTicks) break;
        YieldProcessor();
    }
}

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

    // Clamp and store the render FPS from config
    m_targetRenderFps = static_cast<float>(
        (std::max)(10u, (std::min)(m_config.targetFps, 120u)));
    m_variableFrameRate = m_config.variableFrameRate;

    // Set global timer resolution for precision pacing
    if (timeBeginPeriod(1) == TIMERR_NOERROR) {
        m_timerResolutionSet = true;
        LOG_INFO("Viewer timer resolution set to 1 ms");
    }

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

    // Share the renderer's D3D11 device with the hardware decoder so decoded
    // NV12 textures are directly accessible for VP conversion + rendering.
    // This puts decode + VP + draw on the same GPU queue; at 2560×1600 the
    // VP NV12→BGRA conversion takes ~10-15 ms on the laptop RTX 4080, which
    // limits new-frame throughput to ~60-100 fps depending on content.
    if (m_videoDecoder && m_renderer->GetDevice()) {
        m_videoDecoder->InitializeWithD3D11(
            m_network->GetCodecType(), m_remoteWidth, m_remoteHeight,
            m_renderer->GetDevice(), m_renderer->GetContext());
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
    m_frameCv.notify_all();  // wake render thread from CV wait

    if (m_network) m_network->Disconnect();

    for (auto& t : m_threads) {
        if (t.joinable()) t.join();
    }

    if (m_timerResolutionSet) {
        timeEndPeriod(1);
        m_timerResolutionSet = false;
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
            nv12Tex->AddRef();//AddRef函数增加引用计数，确保在RenderFrame结束前纹理不会被释放
            nv12W = m_latestFrame.width;
            nv12H = m_latestFrame.height;
        } else if (m_latestFrame.width > 0 && !m_latestFrame.data.empty()) {
            cpuFrame = m_latestFrame.data;
            nv12W = m_latestFrame.width;
            nv12H = m_latestFrame.height;
        }
    }

    if (nv12Tex) {
        // VP path: NV12→BGRA via D3D11 Video Processor (hardware color conversion).
        // The NV12 pixel-shader path is faster but GPU-dependent — the UV channel
        // mapping (R=U/G=V vs R=V/G=U) varies across GPU vendors and driver versions.
        // The VP path handles colors correctly on all hardware.
        m_renderer->RenderFrameNv12(nv12Tex, nv12W, nv12H);
        nv12Tex->Release();
    } else if (!cpuFrame.empty()) {
        m_renderer->RenderFrame(cpuFrame.data(), nv12W, nv12H);
    }

    // Render overlay — show actual decoded video FPS, not network packet rate
    m_overlay->Draw(m_displayFps,
                    m_network ? m_network->IsConnected() : false,
                    m_inputActive.load());

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

    static int keyCount = 0;
    if (++keyCount <= 3) {
        LOG_INFO("[Input] Key event: vk=0x%02X %s (active=%d)", ke.vkCode,
                 isDown ? "down" : "up", m_inputActive.load());
    }

    m_inputSendQueue.tryPush(ev);
}

void ViewerSession::OnMouseEvent(UINT msg, WPARAM wParam, LPARAM lParam) {
    if (!m_inputActive || !m_network) return;

    static int mouseCount = 0;

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
    if (++mouseCount <= 3) {
        LOG_INFO("[Input] Mouse event: type=%d (active=%d)", (int)msg, m_inputActive.load());
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
            static int rawMoveCount = 0;
            if (++rawMoveCount <= 3) {
                LOG_INFO("[Input] RawInput mouse move: dx=%d dy=%d",
                         (int)raw->data.mouse.lLastX, (int)raw->data.mouse.lLastY);
            }
            m_inputSendQueue.tryPush(ev);
        }
    }
}

void ViewerSession::RenderThread() {
    LOG_INFO("[Render] Thread started, target=%.1f fps", m_targetRenderFps);
    int64_t lastFrameTime = Timer::NowMs();
    int64_t lastFpsUpdateTime = lastFrameTime;
    uint32_t lastDecodedCount = 0;
    uint32_t renderCount = 0;
    uint32_t skipCount = 0;
    int64_t lastStatsTime = lastFrameTime;

    // Fixed render pacing: always aims for m_targetRenderFps (from config),
    // independent of instantaneous decode rate.  The render thread polls for
    // a new frame (m_decodedFrameId > m_lastRenderedFrameId) and only issues
    // GPU work when a fresh frame is available.
    //
    // This breaks the old EWMA negative-feedback loop:
    //   decode slows → EWMA drops m_renderTargetFps → render slows →
    //   decode queue fills → network drops → decode slows further.
    // With fixed pacing, the decode/network path is never throttled by the
    // render consumer; the queue drains freely and the network never sees
    // artificial backpressure.
    int64_t frameIntervalMs = static_cast<int64_t>(1000.0 / m_targetRenderFps);
    bool vfr = m_variableFrameRate;
    LOG_INFO("[Render] Thread started, target=%.1f fps  mode=%s",
             m_targetRenderFps, vfr ? "VFR" : "CFR");

    // CFR: always present at target FPS even when content is static.
    //   New frame → render new.  No new frame → re-render last.
    // VFR: only present when a new decoded frame arrives.
    //   New frame → render new.  No new frame → CV wait (no GPU work).

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

        uint32_t currentDecodedId = m_decodedFrameId.load();
        bool hasNewFrame = (currentDecodedId != m_lastRenderedFrameId);

        if (hasNewFrame) {
            // New decoded frame available — render it.
            RenderFrame();
            m_lastRenderedFrameId = currentDecodedId;
            renderCount++;
        } else if (!vfr) {
            // CFR mode, no new frame — re-render the last frame to maintain
            // the target display rate even when the remote desktop is static.
            RenderFrame();
            skipCount++;
        } else {
            // VFR mode, no new frame — block until one arrives.
            skipCount++;
            std::unique_lock cvLock(m_frameMutex);
            m_frameCv.wait(cvLock, [this] {
                return !m_running || m_decodedFrameId.load() != m_lastRenderedFrameId;
            });
        }

        auto now = Timer::NowMs();

        // Update decoded FPS display once per second
        if (now - lastFpsUpdateTime >= 1000) {
            uint32_t decodedNow = m_decodedFrameCount.load();
            m_displayFps = static_cast<float>(
                (decodedNow - lastDecodedCount) * 1000.0f / (now - lastFpsUpdateTime));
            lastDecodedCount = decodedNow;
            lastFpsUpdateTime = now;
        }

        // CFR only: fixed-interval pacing
        if (!vfr) {
            int64_t elapsed = now - lastFrameTime;
            if (elapsed < frameIntervalMs) {
                PrecisionSleepMs(frameIntervalMs - elapsed);
            }
            lastFrameTime = Timer::NowMs();
        }

        // Periodic stats (5s interval)
        if (now - lastStatsTime >= 5000) {
            double renderFps = (renderCount + skipCount) * 1000.0 / (now - lastStatsTime);
            LOG_INFO("[Render] render=%.1f fps  video=%.1f fps  renders=%u skips=%u  mode=%s",
                     renderFps, m_displayFps, renderCount, skipCount, vfr ? "VFR" : "CFR");
            renderCount = 0;
            skipCount = 0;
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
        // Receive data frame — 5ms timeout matches 120fps frame interval (~8ms)
        // to ensure timely reception without excessive CPU polling.
        ViewerNetworkImpl::DataPacket packet;
        if (m_network->ReceiveDataFrame(packet, 5)) {
            totalPackets++;
            if (packet.type == Protocol::FrameType::VIDEO_KEYFRAME ||
                packet.type == Protocol::FrameType::VIDEO_DELTA) {
                videoPackets++;

                // DEBUG: Save first 5 received H.264 bitstreams
                if (debugSaveCount < 5 && !packet.data.empty()) {
                    SaveRawData("viewer_received", packet.data.data(), packet.data.size());
                    debugSaveCount++;
                }

                VideoPacket vp;//vp接管packet.data的所有权，延长生命周期直到视频解码线程处理完毕
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
        auto ctrlMsg = m_network->ReceiveControlMessage(1);//目前控制消息很少，若后续增多可以改为专门的线程处理
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
        // 5ms timeout — at 120 fps a new frame arrives every ~8.3 ms.
        // A 5 ms poll ensures the decode thread starts work within one
        // frame interval.  (Was 50 ms, tuned for ≤60 fps.)
        auto vp = m_videoQueue.tryPop(5);
        if (!vp) continue;

        // Log occasional debug info when packets are being consumed to help
        // diagnose stalls (coarse sampling to avoid log spam).
        static uint32_t consumeLogCount = 0;
        if ((consumeLogCount++ & 0x7F) == 0) {
            LOG_INFO("[VideoDecode] Consuming packet input size=%zu key=%d queue=%zu",
                     vp->data.size(), vp->isKeyFrame ? 1 : 0, m_videoQueue.size());
        }

        if (m_videoDecoder->HasGpuPath()) {
            // GPU decode: MFT outputs NV12 GPU texture on the shared device.
            // The render thread does VP NV12→BGRA conversion and draw.
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
                m_decodedFrameId.fetch_add(1);
                m_frameCv.notify_one();
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
                m_decodedFrameId.fetch_add(1);
                m_frameCv.notify_one();  // wake render thread
                if (frames == 1) {
                    LOG_INFO("[VideoDecode] First frame decoded (CPU): %ux%u (key=%d input=%zu bytes)",
                             width, height, vp->isKeyFrame, vp->data.size());
                }

                // DEBUG: Save decoded RGBA frame to BMP every ~5 seconds (up to 10).
                // Copy under mutex to avoid data race with the render thread.
                auto now = Timer::NowMs();
                if (debugSaveCount < 10 && (debugSaveCount == 0 || now - lastDebugSaveMs >= 5000)) {
                    lastDebugSaveMs = now;
                    debugSaveCount++;
                    // Capture a copy inside the lock to avoid racing with RenderFrame
                    std::vector<uint8_t> copyForSave;
                    {
                        std::lock_guard lock2(m_frameMutex);
                        copyForSave = m_latestFrame.data;
                    }
                    SaveBmp("viewer_decoded", copyForSave.data(),
                            m_latestFrame.width, m_latestFrame.height, false);
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

    uint32_t sentCount = 0;
    while (m_running) {
        auto ev = m_inputSendQueue.tryPop(20);
        if (ev && m_network) {
            Protocol::ControlMessage msg;
            msg.type = Protocol::MessageType::INPUT_EVENT;
            msg.inputEvents.push_back(*ev);
            if (m_network->SendControlMessage(msg)) {
                sentCount++;
                if (sentCount <= 3) {
                    LOG_INFO("[InputSend] Sent input event type=%d", (int)ev->type);
                }
            }
        }
    }
    LOG_INFO("[InputSend] Thread stopped, %u sent", sentCount);
}
