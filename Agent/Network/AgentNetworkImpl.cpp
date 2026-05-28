#include "AgentNetworkImpl.h"
#include "ControlChannel.h"
#include "DataChannel.h"
#include "Common/Utils/Logger.h"
#include "Common/Utils/Timer.h"
#include "Common/Security/AesGcm.h"
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
    setsockopt(m_tcpListen, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));//允许重用本地地址和端口，快速重启服务器时避免等待TIME_WAIT状态结束

    // Disable Nagle's algorithm for low latency
    int nodelay = 1;
    setsockopt(m_tcpListen, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));//Nagle算法会将小数据包合并成一个大包发送，虽然可能减少网络开销，但会增加延迟。

    // Bind
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(m_tcpListen, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {//绑定套接字到指定的IP地址和端口号
        LOG_ERROR("TCP bind failed: %d", WSAGetLastError());
        closesocket(m_tcpListen);
        m_tcpListen = INVALID_SOCKET;
        return false;
    }

    if (listen(m_tcpListen, 1) == SOCKET_ERROR) {//只接受一个连接。包含了tcp三次握手的过程。accept函数会阻塞直到有一个客户端连接成功完成三次握手，或者发送出错。
		//服务器套接字在调用listen函数后就变成了监听套接字，表示它准备好接受客户端连接了。这个套接字本身不能用于发送或接收数据，只能用于接受连接请求。
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

    if (bind(m_udpSocket, (sockaddr*)&udpAddr, sizeof(udpAddr)) == SOCKET_ERROR) {//（可选）UDP套接字绑定到同一端口。bind(udpSocket,nullptr,0)让系统自动分配一个可用端口
        LOG_ERROR("UDP bind failed: %d", WSAGetLastError());
        closesocket(m_tcpListen);
        closesocket(m_udpSocket);
        m_tcpListen = INVALID_SOCKET;
        m_udpSocket = INVALID_SOCKET;
        return false;
    }

    // Increase UDP buffer size
    int bufSize = 8 * 1024 * 1024; // 8 MB
    setsockopt(m_udpSocket, SOL_SOCKET, SO_RCVBUF, (const char*)&bufSize, sizeof(bufSize));//分配的缓冲区空间由系统管理，无需手动释放
    setsockopt(m_udpSocket, SOL_SOCKET, SO_SNDBUF, (const char*)&bufSize, sizeof(bufSize));

    // Create security objects (init during handshake)
    m_authenticator = std::make_unique<Authenticator>(
        Authenticator::Role::Agent, m_password);

    LOG_INFO("Network initialized on port %u (TCP+UDP)", port);
    return true;
}

void AgentNetworkImpl::Shutdown() {
    if (m_secureChannel) {
        LOG_INFO("[AgentNet] Encryption stats: %llu encrypted",
                 m_secureChannel->GetEncryptedCount());
    }

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

    // Use WSAPoll (Vista+) — more reliable than select() on Windows
    WSAPOLLFD pfd;
    pfd.fd = m_tcpListen;
    pfd.events = POLLRDNORM;
    pfd.revents = 0;

    int result = WSAPoll(&pfd, 1, timeoutMs);//WSAPoll监视套接字的状态，等待期间当前线程也处于可提醒状态，可以同时监视多个套接字，但是不会处理连接请求，只是通知哪个套接字有事件发生。只有当监听套接字上有可读事件（POLLRDNORM）时，才表示有新的连接请求到达，此时才调用accept函数接受连接。
    if (result <= 0) {
        if (result == SOCKET_ERROR) {
            LOG_ERROR("[Accept] WSAPoll error: %d", WSAGetLastError());
        }
        return false;
    }

    if (!(pfd.revents & POLLRDNORM)) return false;

    sockaddr_in clientAddr = {};
    int addrLen = sizeof(clientAddr);
    m_tcpClient = accept(m_tcpListen, (sockaddr*)&clientAddr, &addrLen);//accept等待期间当前线程处于可提醒状态，允许处理其他事件（定时器、信号等）。 直到有一个客户端连接成功并完成三次握手，或者发生错误（网络异常、套接字被关闭等）
    if (m_tcpClient == INVALID_SOCKET) {
        LOG_ERROR("[Accept] accept() failed: %d", WSAGetLastError());
        return false;
    }

    // Set TCP_NODELAY on client socket
    int nodelay = 1;
    setsockopt(m_tcpClient, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));//针对已连接的TCP套接字单独设置TCP_NODELAY选项，禁用Nagle算法以减少延迟

    // Set up UDP destination address (same IP as TCP client)
    m_udpDestAddr = clientAddr;
    // Port is the same — the Viewer uses the same port for UDP

    // Store client address for logging
    char ipStr[64];
    inet_ntop(AF_INET, &clientAddr.sin_addr, ipStr, sizeof(ipStr));//inet_ntop将网络地址结构转换为点分十进制字符串表示形式
    LOG_INFO("Connection accepted from %s:%d", ipStr, ntohs(clientAddr.sin_port));//ntohs将网络字节序的端口号转换为主机字节序，正确显示端口号
	//网络字节序：网络通信中使用的一种字节序，通常是大端序（big-endian）。在大端序中，数据的高位字节存储在低地址处，低位字节存储在高地址处。
    //主机字节序：计算机系统内部使用的字节序，可以是大端序或小端序（little-endian）。小端序中，数据的低位字节存储在低地址处，高位字节存储在高地址处。由于不同系统可能使用不同的主机字节序，在网络通信中需要将主机字节序转换为网络字节序，以确保数据在不同系统之间正确传输和解析。

    // Perform handshake
	if (!PerformHandshake()) {//accept函数返回的套接字已经完成了TCP三次握手，建立了与客户端的连接。PerformHandshake函数在这个基础上进行协议层面的握手，包括认证和密钥交换等，以确保双方能够安全地通信。如果握手失败，说明协议层面的验证没有通过，可能是因为认证失败、密钥交换失败或者其他协议错误等原因。
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

        // Update UDP destination port (Viewer's UDP socket is on a different port than TCP)
        if (peerKeyMsg->udpPort) {
            m_udpDestAddr.sin_port = htons(*peerKeyMsg->udpPort);
            LOG_INFO("UDP destination port: %u", *peerKeyMsg->udpPort);
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
    startMsg.screenWidth = m_screenWidth.load();
    startMsg.screenHeight = m_screenHeight.load();
    startMsg.codecType = m_codecType.load();
    SendControlMessage(startMsg);

    LOG_INFO("Session handshake complete");
    return true;
}

bool AgentNetworkImpl::SendControlMessage(const Protocol::ControlMessage& msg) {//const XX& msg表示msg是一个常量引用，函数内部不能修改msg的内容。使用引用避免了传值带来的性能开销，特别是当ControlMessage对象较大时。通过const修饰，确保函数不会意外修改传入的消息对象，提高代码的安全性和可读性。
    if (m_tcpClient == INVALID_SOCKET) return false;

    std::lock_guard lock(m_sendMutex);//同一时间只有一个线程可以执行tcp发送操作
    auto wireData = msg.serialize();
    int sent = send(m_tcpClient, (const char*)wireData.data(), (int)wireData.size(), 0);
    return sent == static_cast<int>(wireData.size());
}

std::optional<Protocol::ControlMessage> AgentNetworkImpl::ReceiveControlMessage(int timeoutMs) {
    if (m_tcpClient == INVALID_SOCKET) return std::nullopt;//std::nullopt表示std::optional对象不包含值

    // Set receive timeout
    DWORD tv = static_cast<DWORD>(timeoutMs);
    setsockopt(m_tcpClient, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

    // Read 4-byte length prefix
    uint8_t lenBuf[4];
    int received = recv(m_tcpClient, (char*)lenBuf, 4, MSG_WAITALL);//接收前4字节数据，表示后续消息体的长度。
    if (received != 4) return std::nullopt;

    uint32_t msgLen = (static_cast<uint32_t>(lenBuf[0]) << 24)
                    | (static_cast<uint32_t>(lenBuf[1]) << 16)
                    | (static_cast<uint32_t>(lenBuf[2]) << 8)
                    | lenBuf[3];

    if (msgLen > 1024 * 1024) return std::nullopt; // 1MB limit For Control Messages

    std::vector<uint8_t> buf(msgLen);
    received = recv(m_tcpClient, (char*)buf.data(), msgLen, MSG_WAITALL);//接收消息体数据  recv从TCP套接字的接收缓冲区中读取数据
    if (received != static_cast<int>(msgLen)) return std::nullopt;

    return Protocol::ControlMessage::deserialize(buf.data(), buf.size());
}

bool AgentNetworkImpl::SendDataFrame(Protocol::FrameType type, uint16_t seq,
                                      uint32_t timestampMs,
                                      const uint8_t* payload, uint32_t payloadSize) {
    if (m_udpSocket == INVALID_SOCKET) return false;

    static constexpr size_t MAX_UDP_PAYLOAD = 65507;//UDP协议规定的最大数据负载字节数
    // Per-fragment overhead: FrameHeader + FragmentHeader + encryption (IV + GCM tag)
    static constexpr size_t FRAG_OVERHEAD = Protocol::FrameHeader::WireSize
                                          + Protocol::FragmentHeader::WireSize
                                          + AesGcm::IV_SIZE + AesGcm::TAG_SIZE;
    static constexpr size_t MAX_FRAG_DATA = MAX_UDP_PAYLOAD - FRAG_OVERHEAD;//每个UDP数据包中实际可用来承载分片数据的最大字节数

    if (payloadSize <= MAX_FRAG_DATA) {
        // Single packet — encrypt payload directly
        Protocol::FrameHeader header;
        header.type = static_cast<uint8_t>(type);
        header.flags = 0;
        header.sequenceNumber = seq;
        header.timestampMs = timestampMs;
        header.payloadSize = static_cast<uint16_t>(payloadSize);

        const uint8_t* sendPayload = payload;
        size_t sendPayloadSize = payloadSize;

        if (m_encrypted && m_secureChannel) {//如果启用了加密并且安全通道已经建立，则对整个负载进行加密，并将加密后的数据作为发送负载
            header.setEncrypted(true);
            if (!m_secureChannel->EncryptFrameOut(header, payload, payloadSize, m_encryptBuffer))
                return false;
            sendPayload = m_encryptBuffer.data();
            sendPayloadSize = m_encryptBuffer.size();
        }

        auto headerBytes = header.serialize();
        m_sendBuffer.clear();
        m_sendBuffer.reserve(headerBytes.size() + sendPayloadSize);//预分配UDP数据包缓冲区空间
        m_sendBuffer.insert(m_sendBuffer.end(), headerBytes.begin(), headerBytes.end());
        m_sendBuffer.insert(m_sendBuffer.end(), sendPayload, sendPayload + sendPayloadSize);

        int sent = sendto(m_udpSocket, (const char*)m_sendBuffer.data(), (int)m_sendBuffer.size(), 0,
                          (const sockaddr*)&m_udpDestAddr, sizeof(m_udpDestAddr));
        if (sent != static_cast<int>(m_sendBuffer.size())) {
            LOG_ERROR("[SendDataFrame] sendto failed: sent=%d expected=%zu err=%d",
                      sent, m_sendBuffer.size(), WSAGetLastError());
            return false;
        }
        return true;
    }

    // Fragment large payload: split first, then encrypt each fragment
    uint16_t totalFrags = static_cast<uint16_t>((payloadSize + MAX_FRAG_DATA - 1) / MAX_FRAG_DATA);
    uint16_t fragId = seq;

    static bool loggedOnce = false;
    if (!loggedOnce) {
        LOG_INFO("[SendDataFrame] Fragmenting %u bytes into %u fragments", payloadSize, totalFrags);
        loggedOnce = true;
    }

    // Pre-allocate fragment prefix buffer for reuse
    std::vector<uint8_t> fragPrefix;
    fragPrefix.reserve(Protocol::FragmentHeader::WireSize + MAX_FRAG_DATA);

    for (uint16_t i = 0; i < totalFrags; i++) {
        size_t offset = static_cast<size_t>(i) * MAX_FRAG_DATA;
        size_t chunkSize = (offset + MAX_FRAG_DATA <= payloadSize) ? MAX_FRAG_DATA : (payloadSize - offset);

        // Build fragment prefix: FragmentHeader (6 bytes) + chunk data
        fragPrefix.resize(Protocol::FragmentHeader::WireSize + chunkSize);
        fragPrefix[0] = static_cast<uint8_t>((fragId >> 8) & 0xFF);
        fragPrefix[1] = static_cast<uint8_t>(fragId & 0xFF);
        fragPrefix[2] = static_cast<uint8_t>((i >> 8) & 0xFF);
        fragPrefix[3] = static_cast<uint8_t>(i & 0xFF);
        fragPrefix[4] = static_cast<uint8_t>((totalFrags >> 8) & 0xFF);
        fragPrefix[5] = static_cast<uint8_t>(totalFrags & 0xFF);
        memcpy(fragPrefix.data() + Protocol::FragmentHeader::WireSize, payload + offset, chunkSize);

        // Create FrameHeader for this fragment
        Protocol::FrameHeader fragFrameHdr;
        fragFrameHdr.type = static_cast<uint8_t>(type);
        fragFrameHdr.flags = 0;
        fragFrameHdr.setFragment(true);
        fragFrameHdr.sequenceNumber = seq;
        fragFrameHdr.timestampMs = timestampMs;
        fragFrameHdr.payloadSize = static_cast<uint16_t>(fragPrefix.size());

        // Encrypt the FragmentHeader + chunk as one unit
        const uint8_t* sendPayload = fragPrefix.data();
        size_t sendPayloadSize = fragPrefix.size();

        if (m_encrypted && m_secureChannel) {
            fragFrameHdr.setEncrypted(true);
            if (!m_secureChannel->EncryptFrameOut(fragFrameHdr, fragPrefix.data(), fragPrefix.size(),
                                                    m_encryptBuffer))
                return false;
            sendPayload = m_encryptBuffer.data();
            sendPayloadSize = m_encryptBuffer.size();
        }

        // Serialize FrameHeader + encrypted(FragmentHeader + chunk)
        auto fragHdrBytes = fragFrameHdr.serialize();
        m_sendBuffer.clear();
        m_sendBuffer.reserve(fragHdrBytes.size() + sendPayloadSize);
        m_sendBuffer.insert(m_sendBuffer.end(), fragHdrBytes.begin(), fragHdrBytes.end());
        m_sendBuffer.insert(m_sendBuffer.end(), sendPayload, sendPayload + sendPayloadSize);

        int sent = sendto(m_udpSocket, (const char*)m_sendBuffer.data(), (int)m_sendBuffer.size(), 0,
                          (const sockaddr*)&m_udpDestAddr, sizeof(m_udpDestAddr));
        if (sent != static_cast<int>(m_sendBuffer.size())) {
            LOG_ERROR("[SendDataFrame] Fragment %u/%u sendto failed: sent=%d expected=%zu err=%d",
                      i + 1, totalFrags, sent, m_sendBuffer.size(), WSAGetLastError());
            return false;
        }
    }
    return true;
}
