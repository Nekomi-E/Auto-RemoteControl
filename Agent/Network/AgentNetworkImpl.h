#pragma once
#include "Common/Common.h"
#include "Common/Protocol/FrameHeader.h"
#include "Common/Protocol/ControlMessage.h"
#include "Common/Security/SecureChannel.h"
#include "Common/Security/Authenticator.h"
#include "Common/Security/DiffieHellman.h"
#include <string>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <memory>

class AgentNetworkImpl {
public:
    AgentNetworkImpl();
    ~AgentNetworkImpl();

    bool Initialize(uint16_t port, const std::string& password, bool enableEncryption);
    void Shutdown();

    bool AcceptConnection(int timeoutMs);
    bool IsConnected() const { return m_connected; }

    // Screen info for SESSION_START
    void SetScreenInfo(uint32_t width, uint32_t height) {
        m_screenWidth = width;
        m_screenHeight = height;
    }
    void SetCodecType(uint32_t codecType) { m_codecType = codecType; }

    // Control messages (TCP)
    bool SendControlMessage(const Protocol::ControlMessage& msg);
    std::optional<Protocol::ControlMessage> ReceiveControlMessage(int timeoutMs);

    // Data frames (UDP)
    bool SendDataFrame(Protocol::FrameType type, uint16_t seq, uint32_t timestampMs,
                       const uint8_t* payload, uint32_t payloadSize);

private:
    bool PerformHandshake();

    uint16_t m_port = 0;
    std::string m_password;
    bool m_enableEncryption = true;

    SOCKET m_tcpListen = INVALID_SOCKET;//监听套接字，接受新的TCP连接
    SOCKET m_tcpClient = INVALID_SOCKET;//已连接的TCP套接字，实际上是accept函数返回的套接字，与客户端通信
    SOCKET m_udpSocket = INVALID_SOCKET;//UDP无连接套接字，发送数据帧到客户端
    sockaddr_in m_udpDestAddr = {};

    std::unique_ptr<SecureChannel> m_secureChannel;
    std::unique_ptr<Authenticator> m_authenticator;
    std::unique_ptr<DiffieHellman> m_dh;

    std::atomic<bool> m_connected{ false };//连接状态
	std::atomic<bool> m_encrypted{ false };//是否启用加密
    std::mutex m_sendMutex; // TCP send serialization

    // Pre-allocated buffers to avoid per-frame heap allocations
    std::vector<uint8_t> m_sendBuffer;     // UDP packet assembly buffer
    std::vector<uint8_t> m_encryptBuffer;  // EncryptFrameOut output buffer

    std::atomic<uint32_t> m_screenWidth{1920};
    std::atomic<uint32_t> m_screenHeight{1080};
    std::atomic<uint32_t> m_codecType{0};  // 0=H.264, 1=HEVC
};
