#include "EncoderManager.h"
#include "MfVideoEncoder.h"
#include "MfAudioEncoder.h"
#include "Common/Utils/Logger.h"
#include <d3d11.h>
#include "Common/Utils/Timer.h"
#include "Common/Utils/RingBuffer.h"
#include "Common/Utils/ThreadSafeQueue.h"
#include <thread>
#include <mutex>

struct EncoderManager::Impl {
    std::unique_ptr<MfVideoEncoder> videoEncoder;
    std::unique_ptr<MfAudioEncoder> audioEncoder;
    bool audioEnabled = false;

    // Raw frame input queues
    struct RawVideoFrame {
        std::vector<uint8_t> data;
        uint32_t width = 0;
        uint32_t height = 0;
        int64_t timestampMs = 0;
    };
    struct RawAudioFrame {
        std::vector<uint8_t> data;
        int64_t timestampMs = 0;
    };

    ThreadSafeQueue<RawVideoFrame> videoInputQueue{16};
    ThreadSafeQueue<RawAudioFrame> audioInputQueue{16};

    // Encoded output queues
    ThreadSafeQueue<EncodedFrame> videoOutputQueue{32};
    ThreadSafeQueue<EncodedFrame> audioOutputQueue{32};

    // Worker threads
    std::thread videoEncodeThread;
    std::thread audioEncodeThread;
    std::atomic<bool> running{false};

    // Stats
    std::atomic<uint32_t> encodedFrames{0};
    std::atomic<uint32_t> keyFrames{0};
    std::atomic<uint32_t> encodedAudioFrames{0};
    int64_t lastStatsTime = 0;
    uint32_t framesSinceLastStats = 0;
    float encodeFps = 0;
    uint32_t currentBitrate = 0;
    uint32_t accumulatedBytes = 0;

    uint32_t frameWidth = 1920;
    uint32_t frameHeight = 1080;
    uint32_t targetBitrate = 0;       // 0 = auto
    uint32_t targetFps = 60;
};

EncoderManager::EncoderManager() : m_impl(std::make_unique<Impl>()) {}

EncoderManager::~EncoderManager() { Stop(); }

bool EncoderManager::Initialize(uint32_t width, uint32_t height, uint32_t bitrate,
                                 uint32_t fps, bool enableAudio,
                                 ID3D11Device* d3dDevice, ID3D11DeviceContext* d3dContext) {
    m_impl->frameWidth = width;
    m_impl->frameHeight = height;
    m_impl->targetBitrate = bitrate;
    m_impl->targetFps = fps;
    m_impl->audioEnabled = enableAudio;

    // Initialize video encoder
    m_impl->videoEncoder = std::make_unique<MfVideoEncoder>();
    if (!m_impl->videoEncoder->Initialize(width, height, bitrate, fps,
                                           d3dDevice, d3dContext)) {
        LOG_ERROR("Failed to initialize video encoder");
        return false;
    }
    LOG_INFO("Video encoder initialized");

    // Initialize audio encoder if enabled
    if (enableAudio) {
        m_impl->audioEncoder = std::make_unique<MfAudioEncoder>();
        if (m_impl->audioEncoder->Initialize(48000, 2, 64000)) {
            LOG_INFO("Audio encoder initialized: 48000Hz 2ch 64kbps AAC");
        } else {
            LOG_WARNING("Failed to initialize audio encoder, continuing without audio");
            m_impl->audioEncoder.reset();
            m_impl->audioEnabled = false;
        }
    }

    m_impl->running = true;
    m_impl->lastStatsTime = Timer::NowMs();

    // Start encode workers
    m_impl->videoEncodeThread = std::thread([this]() {
        while (m_impl->running) {
            auto frame = m_impl->videoInputQueue.tryPop(33);
            if (frame) {
                std::vector<uint8_t> bitstream;
                bool isKeyFrame = false;
                if (m_impl->videoEncoder->EncodeFrame(
                        frame->data.data(), frame->width, frame->height,
                        bitstream, isKeyFrame)) {
                    EncodedFrame ef;
                    ef.data = std::move(bitstream);
                    ef.isKeyFrame = isKeyFrame;
                    ef.width = frame->width;
                    ef.height = frame->height;
                    ef.timestampMs = frame->timestampMs;
                    m_impl->videoOutputQueue.tryPush(std::move(ef));

                    m_impl->encodedFrames++;
                    if (isKeyFrame) m_impl->keyFrames++;
                    m_impl->framesSinceLastStats++;
                    m_impl->accumulatedBytes += static_cast<uint32_t>(ef.data.size());

                    // Update stats every second
                    auto now = Timer::NowMs();
                    auto elapsed = now - m_impl->lastStatsTime;
                    if (elapsed >= 1000) {
                        m_impl->encodeFps = m_impl->framesSinceLastStats * 1000.0f / elapsed;
                        m_impl->currentBitrate = static_cast<uint32_t>(
                            m_impl->accumulatedBytes * 8000.0f / elapsed);
                        m_impl->framesSinceLastStats = 0;
                        m_impl->accumulatedBytes = 0;
                        m_impl->lastStatsTime = now;
                    }
                }
            }
        }
    });

    if (m_impl->audioEnabled) {
        m_impl->audioEncodeThread = std::thread([this]() {
            while (m_impl->running) {
                auto frame = m_impl->audioInputQueue.tryPop(20);
                if (frame && m_impl->audioEncoder) {
                    std::vector<uint8_t> encoded;
                    if (m_impl->audioEncoder->EncodeFrame(frame->data, encoded)) {
                        EncodedFrame ef;
                        ef.data = std::move(encoded);
                        ef.timestampMs = frame->timestampMs;
                        m_impl->audioOutputQueue.tryPush(std::move(ef));
                        m_impl->encodedAudioFrames++;
                    }
                }
            }
        });
    }

    return true;
}

void EncoderManager::Stop() {
    m_impl->running = false;
    m_impl->videoInputQueue.close();
    m_impl->audioInputQueue.close();
    m_impl->videoOutputQueue.close();
    m_impl->audioOutputQueue.close();

    if (m_impl->videoEncodeThread.joinable()) m_impl->videoEncodeThread.join();
    if (m_impl->audioEncodeThread.joinable()) m_impl->audioEncodeThread.join();

    if (m_impl->videoEncoder) m_impl->videoEncoder->Shutdown();
    if (m_impl->audioEncoder) m_impl->audioEncoder->Shutdown();
}

void EncoderManager::SubmitVideoFrame(std::vector<uint8_t> rawData,
                                       uint32_t width, uint32_t height,
                                       int64_t timestampMs) {
    Impl::RawVideoFrame rf;
    rf.data = std::move(rawData);
    rf.width = width;
    rf.height = height;
    rf.timestampMs = timestampMs;
    m_impl->videoInputQueue.tryPush(std::move(rf));
}

void EncoderManager::SubmitAudioFrame(const std::vector<uint8_t>& rawData,
                                       int64_t timestampMs) {
    if (!m_impl->audioEnabled) return;
    Impl::RawAudioFrame af;
    af.data = rawData;
    af.timestampMs = timestampMs;
    m_impl->audioInputQueue.tryPush(std::move(af));
}

bool EncoderManager::GetEncodedVideoFrame(EncodedFrame& outFrame, int timeoutMs) {
    auto frame = m_impl->videoOutputQueue.tryPop(timeoutMs);
    if (frame) {
        outFrame = std::move(*frame);
        return true;
    }
    return false;
}

bool EncoderManager::GetEncodedAudioFrame(EncodedFrame& outFrame, int timeoutMs) {
    auto frame = m_impl->audioOutputQueue.tryPop(timeoutMs);
    if (frame) {
        outFrame = std::move(*frame);
        return true;
    }
    return false;
}

void EncoderManager::AdjustBitrate(uint32_t bitrate) {
    m_impl->targetBitrate = bitrate;
    if (m_impl->videoEncoder) {
        m_impl->videoEncoder->SetBitrate(bitrate);
    }
}

uint32_t EncoderManager::GetEncoderWidth() const {
    return m_impl->videoEncoder ? m_impl->videoEncoder->GetWidth() : 1920;
}

uint32_t EncoderManager::GetEncoderHeight() const {
    return m_impl->videoEncoder ? m_impl->videoEncoder->GetHeight() : 1080;
}

uint32_t EncoderManager::GetCodecType() const {
    return m_impl->videoEncoder ? m_impl->videoEncoder->GetCodecType() : 0;
}

void EncoderManager::RequestKeyFrame() {
    if (m_impl->videoEncoder) {
        m_impl->videoEncoder->RequestKeyFrame();
    }
}

EncoderManager::Stats EncoderManager::GetStats() const {
    Stats s;
    s.encodeFps = m_impl->encodeFps;
    s.currentBitrate = static_cast<float>(m_impl->currentBitrate);
    s.encodedFrames = m_impl->encodedFrames;
    s.keyFrames = m_impl->keyFrames;
    return s;
}
