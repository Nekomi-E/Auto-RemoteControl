#pragma once
#include <cstdint>

namespace Protocol {

constexpr uint16_t DEFAULT_PORT = 27015;
constexpr uint16_t PROTOCOL_VERSION = 1;
constexpr uint32_t MAX_FRAME_PAYLOAD = 65500;
constexpr uint32_t FRAME_HEADER_SIZE = 12;
constexpr uint32_t ENCRYPTION_OVERHEAD = 28;  // 12 IV + 16 GCM tag
constexpr uint32_t MAX_WIRE_SIZE = FRAME_HEADER_SIZE + MAX_FRAME_PAYLOAD + ENCRYPTION_OVERHEAD;
constexpr uint32_t REORDER_WINDOW = 32;       // frames
constexpr uint32_t MAX_KEYFRAME_INTERVAL = 120; // frames (~2 seconds at 60fps)

} // namespace Protocol
