#include "ViewerNetworkImpl.h"
#include "Common/Security/Authenticator.h"
#include "Common/Security/DiffieHellman.h"
#include "Common/Utils/Logger.h"
#include "Common/Utils/Timer.h"
#include <winsock2.h>
#include <ws2tcpip.h>

ViewerNetworkImpl::ViewerNetworkImpl() {}

ViewerNetworkImpl::~ViewerNetworkImpl() { Disconnect(); }

bool ViewerNetworkImpl::Connect(const std::string& host, uint16_t port,
                                  const std::string& password, bool enableEncryption) {
    m_host = host;
    m_port = port;
    m_password = password;
    m_enableEncryption = enableEncryption;

    // Resolve hostname
    struct addrinfo hints = {}, *result = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    char portStr[16];
    snprintf(portStr, sizeof(portStr), "%u", port);

    if (getaddrinfo(host.c_str(), portStr, &hints, &result) != 0) {
        LOG_ERROR("Failed to resolve host: %s", host.c_str());
        return false;
    }

    // Create TCP socket and connect
    m_tcpSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_tcpSocket == INVALID_SOCKET) {
        freeaddrinfo(result);
        return false;
    }

    int nodelay = 1;
    setsockopt(m_tcpSocket, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));

    if (connect(m_tcpSocket, result->ai_addr, (int)result->ai_addrlen) == SOCKET_ERROR) {
        LOG_ERROR("TCP connect failed: %d", WSAGetLastError());
        freeaddrinfo(result);
        closesocket(m_tcpSocket);
        m_tcpSocket = INVALID_SOCKET;
        return false;
    }
    freeaddrinfo(result);

    LOG_INFO("TCP connected to %s:%u", host.c_str(), port);

    // Create UDP socket
    m_udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_udpSocket == INVALID_SOCKET) {
        LOG_ERROR("Failed to create UDP socket");
        closesocket(m_tcpSocket);
        m_tcpSocket = INVALID_SOCKET;
        return false;
    }

    // Bind UDP to any available port
    sockaddr_in udpAddr = {};
    udpAddr.sin_family = AF_INET;
    udpAddr.sin_addr.s_addr = INADDR_ANY;
    udpAddr.sin_port = 0; // any port
    bind(m_udpSocket, (sockaddr*)&udpAddr, sizeof(udpAddr));

    int bufSize = 8 * 1024 * 1024;
    setsockopt(m_udpSocket, SOL_SOCKET, SO_RCVBUF, (const char*)&bufSize, sizeof(bufSize));
    setsockopt(m_udpSocket, SOL_SOCKET, SO_SNDBUF, (const char*)&bufSize, sizeof(bufSize));

    // Perform handshake
    if (!PerformHandshake()) {
        LOG_ERROR("Handshake failed");
        Disconnect();
        return false;
    }

    m_connected = true;
    return true;
}

void ViewerNetworkImpl::Disconnect() {
    m_connected = false;

    if (m_tcpSocket != INVALID_SOCKET) {
        closesocket(m_tcpSocket);
        m_tcpSocket = INVALID_SOCKET;
    }
    if (m_udpSocket != INVALID_SOCKET) {
        closesocket(m_udpSocket);
        m_udpSocket = INVALID_SOCKET;
    }
}

bool ViewerNetworkImpl::PerformHandshake() {
    Authenticator auth(Authenticator::Role::Viewer, m_password);

    // Step 1: Receive authentication challenge
    uint8_t lenBuf[4];
    int received = recv(m_tcpSocket, (char*)lenBuf, 4, MSG_WAITALL);
    if (received != 4) return false;

    uint32_t msgLen = (static_cast<uint32_t>(lenBuf[0]) << 24)
                    | (static_cast<uint32_t>(lenBuf[1]) << 16)
                    | (static_cast<uint32_t>(lenBuf[2]) << 8)
                    | lenBuf[3];
    if (msgLen > 1024 * 1024) return false;

    std::vector<uint8_t> msgBuf(msgLen);
    received = recv(m_tcpSocket, (char*)msgBuf.data(), msgLen, MSG_WAITALL);
    if (received != static_cast<int>(msgLen)) return false;

    auto challengeMsg = Protocol::ControlMessage::deserialize(msgBuf.data(), msgBuf.size());
    if (!challengeMsg || challengeMsg->type != Protocol::MessageType::AUTH_REQUEST) {
        LOG_ERROR("Expected AUTH_REQUEST");
        return false;
    }

    if (!challengeMsg->challenge) return false;
    std::string challenge = *challengeMsg->challenge;

    // Step 2: Send authentication response
    std::string response = auth.ComputeResponse(challenge);

    Protocol::ControlMessage respMsg;
    respMsg.type = Protocol::MessageType::AUTH_RESPONSE;
    respMsg.response = response;

    auto wireData = respMsg.serialize();
    send(m_tcpSocket, (const char*)wireData.data(), (int)wireData.size(), 0);

    // Step 3: Key exchange (if encryption enabled)
    if (m_enableEncryption) {
        // Receive Agent's public key
        received = recv(m_tcpSocket, (char*)lenBuf, 4, MSG_WAITALL);
        if (received != 4) return false;

        msgLen = (static_cast<uint32_t>(lenBuf[0]) << 24)
               | (static_cast<uint32_t>(lenBuf[1]) << 16)
               | (static_cast<uint32_t>(lenBuf[2]) << 8)
               | lenBuf[3];
        msgBuf.resize(msgLen);
        received = recv(m_tcpSocket, (char*)msgBuf.data(), msgLen, MSG_WAITALL);
        if (received != static_cast<int>(msgLen)) return false;

        auto keyMsg = Protocol::ControlMessage::deserialize(msgBuf.data(), msgBuf.size());
        if (!keyMsg || keyMsg->type != Protocol::MessageType::KEY_EXCHANGE || !keyMsg->publicKey) {
            LOG_ERROR("Expected KEY_EXCHANGE");
            return false;
        }

        auto agentPubKey = Authenticator::HexToBytes(*keyMsg->publicKey);

        // Generate our key pair and compute shared secret
        DiffieHellman dh;
        if (!dh.GenerateKeyPair()) return false;

        auto ourPubKey = dh.GetPublicKey();
        auto sharedSecret = dh.ComputeSharedSecret(agentPubKey);
        if (sharedSecret.empty()) return false;

        auto aesKey = dh.GetAesKey(sharedSecret);

        // Send our public key
        Protocol::ControlMessage ourKeyMsg;
        ourKeyMsg.type = Protocol::MessageType::KEY_EXCHANGE;
        ourKeyMsg.publicKey = Authenticator::BytesToHex(ourPubKey);

        wireData = ourKeyMsg.serialize();
        send(m_tcpSocket, (const char*)wireData.data(), (int)wireData.size(), 0);

        // Initialize secure channel
        m_secureChannel = std::make_unique<SecureChannel>();
        if (!m_secureChannel->Initialize(aesKey)) return false;

        m_encrypted = true;
        LOG_INFO("Secure channel established");
    }

    // Step 4: Receive session start
    received = recv(m_tcpSocket, (char*)lenBuf, 4, MSG_WAITALL);
    if (received != 4) return false;

    msgLen = (static_cast<uint32_t>(lenBuf[0]) << 24)
           | (static_cast<uint32_t>(lenBuf[1]) << 16)
           | (static_cast<uint32_t>(lenBuf[2]) << 8)
           | lenBuf[3];
    msgBuf.resize(msgLen);
    received = recv(m_tcpSocket, (char*)msgBuf.data(), msgLen, MSG_WAITALL);

    auto startMsg = Protocol::ControlMessage::deserialize(msgBuf.data(), msgBuf.size());
    if (!startMsg || startMsg->type != Protocol::MessageType::SESSION_START) {
        LOG_ERROR("Expected SESSION_START");
        return false;
    }

    LOG_INFO("Session handshake complete");
    return true;
}

bool ViewerNetworkImpl::SendControlMessage(const Protocol::ControlMessage& msg) {
    if (m_tcpSocket == INVALID_SOCKET) return false;

    std::lock_guard lock(m_sendMutex);
    auto wireData = msg.serialize();
    int sent = send(m_tcpSocket, (const char*)wireData.data(), (int)wireData.size(), 0);
    return sent == static_cast<int>(wireData.size());
}

std::optional<Protocol::ControlMessage> ViewerNetworkImpl::ReceiveControlMessage(int timeoutMs) {
    if (m_tcpSocket == INVALID_SOCKET) return std::nullopt;

    DWORD tv = static_cast<DWORD>(timeoutMs);
    setsockopt(m_tcpSocket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

    uint8_t lenBuf[4];
    int received = recv(m_tcpSocket, (char*)lenBuf, 4, MSG_WAITALL);
    if (received != 4) return std::nullopt;

    uint32_t msgLen = (static_cast<uint32_t>(lenBuf[0]) << 24)
                    | (static_cast<uint32_t>(lenBuf[1]) << 16)
                    | (static_cast<uint32_t>(lenBuf[2]) << 8)
                    | lenBuf[3];
    if (msgLen > 1024 * 1024) return std::nullopt;

    std::vector<uint8_t> buf(msgLen);
    received = recv(m_tcpSocket, (char*)buf.data(), msgLen, MSG_WAITALL);
    if (received != static_cast<int>(msgLen)) return std::nullopt;

    return Protocol::ControlMessage::deserialize(buf.data(), buf.size());
}

bool ViewerNetworkImpl::ReceiveDataFrame(DataPacket& outPacket, int timeoutMs) {
    if (m_udpSocket == INVALID_SOCKET) return false;

    // Set timeout via select
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(m_udpSocket, &readSet);

    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;

    if (select(0, &readSet, nullptr, nullptr, &tv) <= 0) return false;

    uint8_t buf[65536];
    sockaddr_in fromAddr = {};
    int fromLen = sizeof(fromAddr);
    int received = recvfrom(m_udpSocket, (char*)buf, sizeof(buf), 0,
                             (sockaddr*)&fromAddr, &fromLen);
    if (received < (int)Protocol::FrameHeader::WireSize) return false;

    // Parse header
    auto header = Protocol::FrameHeader::deserialize(buf, received);
    if (!header) return false;

    size_t headerSize = Protocol::FrameHeader::WireSize;
    const uint8_t* payload = buf + headerSize;
    size_t payloadSize = received - headerSize;

    // Decrypt if needed
    std::vector<uint8_t> plaintext;
    if (header->isEncrypted() && m_secureChannel && m_secureChannel->IsReady()) {
        if (!m_secureChannel->DecryptFrame(payload, payloadSize, *header, plaintext)) {
            // Auth failure or corrupted packet
            return false;
        }
        payload = plaintext.data();
        payloadSize = plaintext.size();
    }

    outPacket.type = static_cast<Protocol::FrameType>(header->type);
    outPacket.timestampMs = header->timestampMs;
    outPacket.data.assign(payload, payload + payloadSize);

    // Update FPS estimate
    static int frameCount = 0;
    static int64_t lastFpsTime = Timer::NowMs();
    frameCount++;
    auto now = Timer::NowMs();
    if (now - lastFpsTime >= 1000) {
        m_fps = frameCount * 1000.0f / (now - lastFpsTime);
        frameCount = 0;
        lastFpsTime = now;
    }

    return true;
}
