#pragma once
#include <cstdint>
#include <vector>
#include <optional>

namespace Protocol {

enum class FrameType : uint8_t {
    VIDEO_KEYFRAME  = 0x01,
    VIDEO_DELTA     = 0x02,
    AUDIO_FRAME     = 0x03,
    CURSOR_SHAPE    = 0x04,
    CURSOR_POSITION = 0x05,
    KEEPALIVE       = 0xFF
};

enum FrameFlags : uint8_t {
    FLAG_ENCRYPTED   = 0x01,
    FLAG_RETRANSMIT  = 0x02,
};

#pragma pack(push, 1)
struct FrameHeader {
    uint8_t  type = 0;
    uint8_t  flags = 0;
    uint16_t sequenceNumber = 0;
    uint32_t timestampMs = 0;
    uint16_t payloadSize = 0;

    static constexpr size_t WireSize = 12;

    std::vector<uint8_t> serialize() const;
    static std::optional<FrameHeader> deserialize(const uint8_t* data, size_t len);

    bool isEncrypted()  const { return flags & FLAG_ENCRYPTED; }
    bool isRetransmit() const { return flags & FLAG_RETRANSMIT; }
    void setEncrypted(bool v);
    void setRetransmit(bool v);
};
#pragma pack(pop)

} // namespace Protocol
