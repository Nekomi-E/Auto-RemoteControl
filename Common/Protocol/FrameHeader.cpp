#include "FrameHeader.h"
#include <cstring>

namespace Protocol {

std::vector<uint8_t> FrameHeader::serialize() const {
    std::vector<uint8_t> buf(WireSize);
    buf[0] = type;
    buf[1] = flags;
    buf[2] = static_cast<uint8_t>((sequenceNumber >> 8) & 0xFF);
    buf[3] = static_cast<uint8_t>(sequenceNumber & 0xFF);
    buf[4] = static_cast<uint8_t>((timestampMs >> 24) & 0xFF);
    buf[5] = static_cast<uint8_t>((timestampMs >> 16) & 0xFF);
    buf[6] = static_cast<uint8_t>((timestampMs >> 8) & 0xFF);
    buf[7] = static_cast<uint8_t>(timestampMs & 0xFF);
    buf[8] = static_cast<uint8_t>((payloadSize >> 8) & 0xFF);
    buf[9] = static_cast<uint8_t>(payloadSize & 0xFF);
    return buf;
}

std::optional<FrameHeader> FrameHeader::deserialize(const uint8_t* data, size_t len) {
    if (len < WireSize) return std::nullopt;
    FrameHeader hdr;
    hdr.type = data[0];
    hdr.flags = data[1];
    hdr.sequenceNumber = (static_cast<uint16_t>(data[2]) << 8) | data[3];
    hdr.timestampMs    = (static_cast<uint32_t>(data[4]) << 24)
                       | (static_cast<uint32_t>(data[5]) << 16)
                       | (static_cast<uint32_t>(data[6]) << 8)
                       | data[7];
    hdr.payloadSize    = (static_cast<uint16_t>(data[8]) << 8) | data[9];
    return hdr;
}

void FrameHeader::setEncrypted(bool v) {
    if (v) flags |= FLAG_ENCRYPTED;
    else   flags &= ~FLAG_ENCRYPTED;
}

void FrameHeader::setRetransmit(bool v) {
    if (v) flags |= FLAG_RETRANSMIT;
    else   flags &= ~FLAG_RETRANSMIT;
}

} // namespace Protocol
