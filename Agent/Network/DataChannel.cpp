#include "DataChannel.h"
#include "Common/Utils/Logger.h"
#include <cstring>

bool DataChannel::SendFrame(SOCKET sock, const sockaddr_in& dest,
                              Protocol::FrameType type, uint16_t seq,
                              uint32_t timestampMs,
                              const uint8_t* payload, uint32_t payloadSize) {
    if (sock == INVALID_SOCKET) return false;

    Protocol::FrameHeader header;
    header.type = static_cast<uint8_t>(type);
    header.flags = 0;
    header.sequenceNumber = seq;
    header.timestampMs = timestampMs;
    header.payloadSize = static_cast<uint16_t>(payloadSize);

    std::vector<uint8_t> wirePayload;
    const uint8_t* finalPayload = payload;
    size_t finalPayloadSize = payloadSize;

    if (m_encrypted && m_secureChannel && m_secureChannel->IsReady()) {
        header.setEncrypted(true);
        wirePayload = m_secureChannel->EncryptFrame(header, payload, payloadSize);
        if (wirePayload.empty()) return false;
        finalPayload = wirePayload.data();
        finalPayloadSize = wirePayload.size();
    }

    auto headerBytes = header.serialize();
    std::vector<uint8_t> packet;
    packet.reserve(headerBytes.size() + finalPayloadSize);
    packet.insert(packet.end(), headerBytes.begin(), headerBytes.end());
    packet.insert(packet.end(), finalPayload, finalPayload + finalPayloadSize);

    int sent = sendto(sock, (const char*)packet.data(), (int)packet.size(), 0,
                      (const sockaddr*)&dest, sizeof(dest));
    return sent == static_cast<int>(packet.size());
}
