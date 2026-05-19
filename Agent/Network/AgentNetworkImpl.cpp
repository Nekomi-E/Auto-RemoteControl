#include "AgentNetworkImpl.h"
#include "ControlChannel.h"
#include "DataChannel.h"
#include "Common/Utils/Logger.h"
#include "Common/Utils/Timer.h"
#include <winsock2.h>
#include <ws2tcpip.h>

AgentNetworkImpl::AgentNetworkImpl() {}

AgentNetworkImpl::~AgentNetworkImpl() { Shutdown(); }

bool AgentNetworkImpl::Initialize(uint16_t port, const std::string& password, bool enableEncryption) {
    m_port = port;
    m_password = password;
    m_enableEncryption = enableEncryption;

    // Create TCP listen socket
    m_tcpListen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_tcpListen == INVALID_SOCKET) {
        LOG_ERROR("Failed to create TCP listen socket: %d", WSAGetLastError());
        return false;
    }

    // Set socket options
    int reuse = 1;
    setsockopt(m_tcpListen, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    // Disable Nagle's algorithm for low latency
    int nodelay = 1;
    setsockopt(m_tcpListen, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));

    // Bind
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(m_tcpListen, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        LOG_ERROR("TCP bind failed: %d", WSAGetLastError());
        closesocket(m_tcpListen);
        m_tcpListen = INVALID_SOCKET;
        return false;
    }

    if (listen(m_tcpListen, 1) == SOCKET_ERROR) {
        LOG_ERROR("TCP listen failed: %d", WSAGetLastError());
        closesocket(m_tcpListen);
        m_tcpListen = INVALID_SOCKET;
        return false;
    }

    // Create UDP socket
    m_udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_udpSocket == INVALID_SOCKET) {
        LOG_ERROR("Failed to create UDP socket: %d", WSAGetLastError());
        closesocket(m_tcpListen);
        m_tcpListen = INVALID_SOCKET;
        return false;
    }

    // Bind UDP to same port
    sockaddr_in udpAddr = {};
    udpAddr.sin_family = AF_INET;
    udpAddr.sin_addr.s_addr = INADDR_ANY;
    udpAddr.sin_port = htons(port);

    if (bind(m_udpSocket, (sockaddr*)&udpAddr, sizeof(udpAddr)) == SOCKET_ERROR) {
        LOG_ERROR("UDP bind failed: %d", WSAGetLastError());
        closesocket(m_tcpListen);
        closesocket(m_udpSocket);
        m_tcpListen = INVALID_SOCKET;
        m_udpSocket = INVALID_SOCKET;
        return false;
    }

    // Increase UDP buffer size
    int bufSize = 8 * 1024 * 1024; // 8 MB
    setsockopt(m_udpSocket, SOL_SOCKET, SO_RCVBUF, (const char*)&bufSize, sizeof(bufSize));
    setsockopt(m_udpSocket, SOL_SOCKET, SO_SNDBUF, (const char*)&bufSize, sizeof(bufSize));

    // Create security objects (init during handshake)
    m_authenticator = std::make_unique<Authenticator>(
        Authenticator::Role::Agent, m_password);

    LOG_INFO("Network initialized on port %u (TCP+UDP)", port);
    return true;
}

void AgentNetworkImpl::Shutdown() {
    m_connected = false;
    m_encrypted = false;

    if (m_tcpClient != INVALID_SOCKET) {
        closesocket(m_tcpClient);
        m_tcpClient = INVALID_SOCKET;
    }
    if (m_tcpListen != INVALID_SOCKET) {
        closesocket(m_tcpListen);
        m_tcpListen = INVALID_SOCKET;
    }
    if (m_udpSocket != INVALID_SOCKET) {
        closesocket(m_udpSocket);
        m_udpSocket = INVALID_SOCKET;
    }
}

bool AgentNetworkImpl::AcceptConnection(int timeoutMs) {
    if (m_tcpListen == INVALID_SOCKET) return false;

    // Set timeout on listen socket
    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;

    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(m_tcpListen, &readSet);

    int result = select(0, &readSet, nullptr, nullptr, &tv);
    if (result <= 0) return false;

    sockaddr_in clientAddr = {};
    int addrLen = sizeof(clientAddr);
    m_tcpClient = accept(m_tcpListen, (sockaddr*)&clientAddr, &addrLen);
    if (m_tcpClient == INVALID_SOCKET) return false;

    // Set TCP_NODELAY on client socket
    int nodelay = 1;
    setsockopt(m_tcpClient, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));

    // Set up UDP destination address (same IP as TCP client)
    m_udpDestAddr = clientAddr;
    // Port is the same — the Viewer uses the same port for UDP

    // Store client address for logging
    char ipStr[64];
    inet_ntop(AF_INET, &clientAddr.sin_addr, ipStr, sizeof(ipStr));
    LOG_INFO("Connection accepted from %s:%d", ipStr, ntohs(clientAddr.sin_port));

    // Perform handshake
    if (!PerformHandshake()) {
        LOG_ERROR("Handshake failed");
        closesocket(m_tcpClient);
        m_tcpClient = INVALID_SOCKET;
        return false;
    }

    m_connected = true;
    return true;
}

bool AgentNetworkImpl::PerformHandshake() {
    // Step 1: Agent sends challenge
    std::string challenge = m_authenticator->GenerateChallenge();

    Protocol::ControlMessage challengeMsg;
    challengeMsg.type = Protocol::MessageType::AUTH_REQUEST;
    challengeMsg.challenge = challenge;
    if (!SendControlMessage(challengeMsg)) {
        LOG_ERROR("Failed to send AUTH_REQUEST");
        return false;
    }

    // Step 2: Receive authentication response
    auto authResp = ReceiveControlMessage(10000);
    if (!authResp || authResp->type != Protocol::MessageType::AUTH_RESPONSE) {
        LOG_ERROR("Failed to receive AUTH_RESPONSE");
        return false;
    }

    if (!authResp->response ||
        !m_authenticator->VerifyResponse(challenge, *authResp->response)) {
        LOG_ERROR("Authentication failed");

        Protocol::ControlMessage errorMsg;
        errorMsg.type = Protocol::MessageType::ERROR_MSG;
        errorMsg.errorMessage = "Authentication failed";
        SendControlMessage(errorMsg);
        return false;
    }
    LOG_INFO("Authentication successful");

    // Step 3: Key exchange (if encryption enabled)
    if (m_enableEncryption) {
        m_dh = std::make_unique<DiffieHellman>();
        if (!m_dh->GenerateKeyPair()) {
            LOG_ERROR("Failed to generate DH key pair");
            return false;
        }

        // Send our public key
        auto pubKey = m_dh->GetPublicKey();
        Protocol::ControlMessage keyMsg;
        keyMsg.type = Protocol::MessageType::KEY_EXCHANGE;
        keyMsg.publicKey = Authenticator::BytesToHex(pubKey);
        if (!SendControlMessage(keyMsg)) {
            LOG_ERROR("Failed to send KEY_EXCHANGE");
            return false;
        }

        // Receive peer's public key
        auto peerKeyMsg = ReceiveControlMessage(10000);
        if (!peerKeyMsg || peerKeyMsg->type != Protocol::MessageType::KEY_EXCHANGE) {
            LOG_ERROR("Failed to receive peer KEY_EXCHANGE");
            return false;
        }

        if (!peerKeyMsg->publicKey) {
            LOG_ERROR("No public key in KEY_EXCHANGE response");
            return false;
        }

        auto peerPubKey = Authenticator::HexToBytes(*peerKeyMsg->publicKey);
        auto sharedSecret = m_dh->ComputeSharedSecret(peerPubKey);
        if (sharedSecret.empty()) {
            LOG_ERROR("Failed to compute shared secret");
            return false;
        }

        auto aesKey = m_dh->GetAesKey(sharedSecret);
        m_secureChannel = std::make_unique<SecureChannel>();
        if (!m_secureChannel->Initialize(aesKey)) {
            LOG_ERROR("Failed to initialize SecureChannel");
            return false;
        }

        m_encrypted = true;
        LOG_INFO("Secure channel established (AES-256-GCM)");
    }

    // Step 4: Session start
    Protocol::ControlMessage startMsg;
    startMsg.type = Protocol::MessageType::SESSION_START;
    SendControlMessage(startMsg);

    LOG_INFO("Session handshake complete");
    return true;
}

bool AgentNetworkImpl::SendControlMessage(const Protocol::ControlMessage& msg) {
    if (m_tcpClient == INVALID_SOCKET) return false;

    std::lock_guard lock(m_sendMutex);
    auto wireData = msg.serialize();
    int sent = send(m_tcpClient, (const char*)wireData.data(), (int)wireData.size(), 0);
    return sent == static_cast<int>(wireData.size());
}

std::optional<Protocol::ControlMessage> AgentNetworkImpl::ReceiveControlMessage(int timeoutMs) {
    if (m_tcpClient == INVALID_SOCKET) return std::nullopt;

    // Set receive timeout
    DWORD tv = static_cast<DWORD>(timeoutMs);
    setsockopt(m_tcpClient, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

    // Read 4-byte length prefix
    uint8_t lenBuf[4];
    int received = recv(m_tcpClient, (char*)lenBuf, 4, MSG_WAITALL);
    if (received != 4) return std::nullopt;

    uint32_t msgLen = (static_cast<uint32_t>(lenBuf[0]) << 24)
                    | (static_cast<uint32_t>(lenBuf[1]) << 16)
                    | (static_cast<uint32_t>(lenBuf[2]) << 8)
                    | lenBuf[3];

    if (msgLen > 1024 * 1024) return std::nullopt; // 1MB limit

    std::vector<uint8_t> buf(msgLen);
    received = recv(m_tcpClient, (char*)buf.data(), msgLen, MSG_WAITALL);
    if (received != static_cast<int>(msgLen)) return std::nullopt;

    return Protocol::ControlMessage::deserialize(buf.data(), buf.size());
}

bool AgentNetworkImpl::SendDataFrame(Protocol::FrameType type, uint16_t seq,
                                      uint32_t timestampMs,
                                      const uint8_t* payload, uint32_t payloadSize) {
    if (m_udpSocket == INVALID_SOCKET) return false;

    Protocol::FrameHeader header;
    header.type = static_cast<uint8_t>(type);
    header.flags = 0;
    header.sequenceNumber = seq;
    header.timestampMs = timestampMs;
    header.payloadSize = static_cast<uint16_t>(payloadSize);

    std::vector<uint8_t> wirePayload;
    const uint8_t* finalPayload = payload;
    size_t finalPayloadSize = payloadSize;

    if (m_encrypted && m_secureChannel) {
        header.setEncrypted(true);
        wirePayload = m_secureChannel->EncryptFrame(header, payload, payloadSize);
        if (wirePayload.empty()) return false;
        finalPayload = wirePayload.data();
        finalPayloadSize = wirePayload.size();
    }

    // Serialize header + payload and send via UDP
    auto headerBytes = header.serialize();
    std::vector<uint8_t> packet;
    packet.reserve(headerBytes.size() + finalPayloadSize);
    packet.insert(packet.end(), headerBytes.begin(), headerBytes.end());
    packet.insert(packet.end(), finalPayload, finalPayload + finalPayloadSize);

    int sent = sendto(m_udpSocket, (const char*)packet.data(), (int)packet.size(), 0,
                      (const sockaddr*)&m_udpDestAddr, sizeof(m_udpDestAddr));
    return sent == static_cast<int>(packet.size());
}
