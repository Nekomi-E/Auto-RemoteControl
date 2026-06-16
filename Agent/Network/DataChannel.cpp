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
    size_t totalSize = headerBytes.size() + finalPayloadSize;

    // Pre-allocated send buffer — amortises the per-frame heap allocation.
    // At 120 fps with ~100 KB frames this saves ~120 allocations/s and the
    // associated malloc/free overhead.  The buffer only resizes when a
    // larger-than-ever frame arrives (first frame, resolution change).
    // Per Sunshine: S/G I/O eliminates intermediate copies entirely;
    // this is the simpler equivalent for sendto-based UDP.
    if (m_sendBuffer.size() < totalSize) {
        m_sendBuffer.resize(totalSize);
    }
    memcpy(m_sendBuffer.data(), headerBytes.data(), headerBytes.size());
    memcpy(m_sendBuffer.data() + headerBytes.size(), finalPayload, finalPayloadSize);

    int sent = sendto(sock, (const char*)m_sendBuffer.data(), (int)totalSize, 0,
                      (const sockaddr*)&dest, sizeof(dest));
    return sent == static_cast<int>(totalSize);
}
