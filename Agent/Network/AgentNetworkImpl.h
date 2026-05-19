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

    SOCKET m_tcpListen = INVALID_SOCKET;
    SOCKET m_tcpClient = INVALID_SOCKET;
    SOCKET m_udpSocket = INVALID_SOCKET;
    sockaddr_in m_udpDestAddr = {};

    std::unique_ptr<SecureChannel> m_secureChannel;
    std::unique_ptr<Authenticator> m_authenticator;
    std::unique_ptr<DiffieHellman> m_dh;

    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_encrypted{false};
    std::mutex m_sendMutex; // TCP send serialization
};
