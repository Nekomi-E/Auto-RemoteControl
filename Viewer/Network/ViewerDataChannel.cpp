#include "ViewerDataChannel.h"
#include "Common/Utils/Timer.h"
#include <cstring>

std::optional<ViewerDataChannel::ReceivedFrame> ViewerDataChannel::ReceiveFrame(SOCKET sock, int timeoutMs) {
    if (sock == INVALID_SOCKET) return std::nullopt;

    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(sock, &readSet);

    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;

    if (select(0, &readSet, nullptr, nullptr, &tv) <= 0) return std::nullopt;

    uint8_t buf[65536];
    sockaddr_in fromAddr = {};
    int fromLen = sizeof(fromAddr);
    int received = recvfrom(sock, (char*)buf, sizeof(buf), 0,
                             (sockaddr*)&fromAddr, &fromLen);
    if (received < (int)Protocol::FrameHeader::WireSize) return std::nullopt;

    auto header = Protocol::FrameHeader::deserialize(buf, received);
    if (!header) return std::nullopt;

    size_t headerSize = Protocol::FrameHeader::WireSize;
    const uint8_t* payload = buf + headerSize;
    size_t payloadSize = received - headerSize;

    std::vector<uint8_t> plaintext;
    if (header->isEncrypted() && m_secureChannel && m_secureChannel->IsReady()) {
        if (!m_secureChannel->DecryptFrame(payload, payloadSize, *header, plaintext)) {
            return std::nullopt;
        }
        payload = plaintext.data();
        payloadSize = plaintext.size();
    }

    ReceivedFrame frame;
    frame.type = static_cast<Protocol::FrameType>(header->type);
    frame.timestampMs = header->timestampMs;
    frame.data.assign(payload, payload + payloadSize);
    return frame;
}
