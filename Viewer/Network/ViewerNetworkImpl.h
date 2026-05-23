#pragma once
#include "Common/Common.h"
#include "Common/Protocol/FrameHeader.h"
#include "Common/Protocol/ControlMessage.h"
#include "Common/Security/SecureChannel.h"
#include <string>
#include <cstdint>
#include <atomic>
#include <memory>
#include <optional>
#include <mutex>
#include <unordered_map>
#include <vector>

class ViewerNetworkImpl {
public:
    struct DataPacket {
        std::vector<uint8_t> data;
        Protocol::FrameType type;
        uint32_t timestampMs = 0;
    };

    ViewerNetworkImpl();
    ~ViewerNetworkImpl();

    bool Connect(const std::string& host, uint16_t port,
                 const std::string& password, bool enableEncryption);
    void Disconnect();
    bool IsConnected() const { return m_connected; }

    bool SendControlMessage(const Protocol::ControlMessage& msg);
    std::optional<Protocol::ControlMessage> ReceiveControlMessage(int timeoutMs);
    bool ReceiveDataFrame(DataPacket& outPacket, int timeoutMs);

    uint32_t GetRemoteWidth() const { return m_remoteWidth; }
    uint32_t GetRemoteHeight() const { return m_remoteHeight; }
    uint32_t GetCodecType() const { return m_codecType; }
    float GetFps() const { return m_fps; }

private:
    bool PerformHandshake();

    SOCKET m_tcpSocket = INVALID_SOCKET;
    SOCKET m_udpSocket = INVALID_SOCKET;
    std::string m_host;
    uint16_t m_port = 0;
    std::string m_password;
    bool m_enableEncryption = true;

    std::unique_ptr<SecureChannel> m_secureChannel;

    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_encrypted{false};
    std::mutex m_sendMutex;

    uint32_t m_remoteWidth = 1920;
    uint32_t m_remoteHeight = 1080;
    uint32_t m_codecType = 0;  // 0=H.264, 1=HEVC
    float m_fps = 0;

    // Fragment reassembly
    struct FragBuf {
        uint16_t totalFrags = 0;
        uint32_t receivedMask = 0;  // bitmask for up to 32 fragments
        std::vector<std::vector<uint8_t>> chunks;
        int64_t firstSeen = 0;
        uint32_t totalSize = 0;
    };
    std::unordered_map<uint16_t, FragBuf> m_fragments;
    int64_t m_lastFragCleanup = 0;
    int m_lastTcpTimeout = -1;  // cache to avoid setsockopt per call
};
