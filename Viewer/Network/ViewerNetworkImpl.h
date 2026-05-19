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
    float m_fps = 0;
};
