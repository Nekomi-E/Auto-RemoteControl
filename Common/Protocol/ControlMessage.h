#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <optional>

namespace Protocol {

enum class MessageType : uint8_t {
    AUTH_REQUEST = 0x01,
    AUTH_RESPONSE = 0x02,
    KEY_EXCHANGE = 0x03,
    SESSION_START = 0x04,
    SESSION_STOP = 0x05,
    INPUT_EVENT = 0x06,
    QUALITY_ADJUST = 0x07,
    CLIPBOARD_SYNC = 0x08,
    PING = 0x09,
    PONG = 0x0A,
    ERROR_MSG = 0x7F
};

enum class InputType : uint8_t {
    MOUSE_MOVE = 0x01,
    MOUSE_BUTTON_DOWN = 0x02,
    MOUSE_BUTTON_UP = 0x03,
    MOUSE_WHEEL = 0x04,
    KEY_DOWN = 0x05,
    KEY_UP = 0x06,
    INPUT_FOCUS_ENTER = 0x07,
    INPUT_FOCUS_LEAVE = 0x08
};

struct MouseMoveEvent {
    int16_t dx = 0;
    int16_t dy = 0;
};

struct MouseButtonEvent {
    uint8_t button = 0;  // 0=left, 1=right, 2=middle
};

struct MouseWheelEvent {
    int16_t delta = 0;
};

struct KeyEvent {
    uint16_t vkCode = 0;
    bool extended = false;
};

struct InputEvent {
    InputType type;
    uint64_t timestamp = 0;
    std::optional<MouseMoveEvent> mouseMove;
    std::optional<MouseButtonEvent> mouseButton;
    std::optional<MouseWheelEvent> mouseWheel;
    std::optional<KeyEvent> key;
};

class ControlMessage {
public:
    MessageType type = MessageType::ERROR_MSG;
    uint32_t seq = 0;

    // Auth
    std::optional<std::string> password;
    std::optional<std::string> challenge;
    std::optional<std::string> response; // HMAC-SHA256(challenge, password)

    // Key exchange
    std::optional<std::string> publicKey; // base64 ECDH P-256 public key

    // Input
    std::vector<InputEvent> inputEvents;

    // Quality
    std::optional<uint32_t> targetBitrate;
    std::optional<uint32_t> targetFps;

    // Error
    std::optional<std::string> errorMessage;

    std::string toJson() const;
    static std::optional<ControlMessage> fromJson(const std::string& json);

    // Wire format: [4-byte big-endian length][JSON payload]
    std::vector<uint8_t> serialize() const;
    static std::optional<ControlMessage> deserialize(const uint8_t* data, size_t len);
};

} // namespace Protocol
