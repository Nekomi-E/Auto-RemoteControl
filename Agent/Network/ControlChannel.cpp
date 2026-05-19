#include "ControlChannel.h"

bool ControlChannel::SendMessage(SOCKET sock, const Protocol::ControlMessage& msg) {
    if (sock == INVALID_SOCKET) return false;
    auto wireData = msg.serialize();
    int sent = send(sock, (const char*)wireData.data(), (int)wireData.size(), 0);
    return sent == static_cast<int>(wireData.size());
}

std::optional<Protocol::ControlMessage> ControlChannel::ReceiveMessage(SOCKET sock, int timeoutMs) {
    if (sock == INVALID_SOCKET) return std::nullopt;

    DWORD tv = static_cast<DWORD>(timeoutMs);
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

    uint8_t lenBuf[4];
    int received = recv(sock, (char*)lenBuf, 4, MSG_WAITALL);
    if (received != 4) return std::nullopt;

    uint32_t msgLen = (static_cast<uint32_t>(lenBuf[0]) << 24)
                    | (static_cast<uint32_t>(lenBuf[1]) << 16)
                    | (static_cast<uint32_t>(lenBuf[2]) << 8)
                    | lenBuf[3];

    if (msgLen > 1024 * 1024) return std::nullopt;

    std::vector<uint8_t> buf(msgLen);
    received = recv(sock, (char*)buf.data(), msgLen, MSG_WAITALL);
    if (received != static_cast<int>(msgLen)) return std::nullopt;

    return Protocol::ControlMessage::deserialize(buf.data(), buf.size());
}
