#include "ControlMessage.h"
#include <sstream>
#include <cstring>

// Minimal JSON serializer — avoids dependency on a full JSON library.
// Control messages are small and simple enough for hand-rolled JSON.

namespace Protocol {

static std::string escapeJson(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                out += buf;
            } else {
                out += c;
            }
        }
    }
    return out;
}

static std::string unescapeJson(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            switch (s[i + 1]) {
            case '"':  out += '"';  ++i; break;
            case '\\': out += '\\'; ++i; break;
            case 'n':  out += '\n'; ++i; break;
            case 'r':  out += '\r'; ++i; break;
            case 't':  out += '\t'; ++i; break;
            default: out += s[i]; break;
            }
        } else {
            out += s[i];
        }
    }
    return out;
}

// Extremely minimal JSON parser — only handles the exact structures we emit.
// Format: {"type":"...","seq":N,...}
static std::string extractString(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos += search.size();
    size_t end = json.find('"', pos);
    if (end == std::string::npos) return "";
    return unescapeJson(json.substr(pos, end - pos));
}

static std::optional<uint32_t> extractUint(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return std::nullopt;
    pos += search.size();
    size_t end = pos;
    while (end < json.size() && (isdigit(json[end]) || json[end] == '-')) ++end;
    if (end == pos) return std::nullopt;
    return static_cast<uint32_t>(strtoul(json.substr(pos, end - pos).c_str(), nullptr, 10));
}

static std::optional<int32_t> extractInt(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return std::nullopt;
    pos += search.size();
    size_t end = pos;
    while (end < json.size() && (isdigit(json[end]) || json[end] == '-')) ++end;
    if (end == pos) return std::nullopt;
    return static_cast<int32_t>(strtol(json.substr(pos, end - pos).c_str(), nullptr, 10));
}

static uint64_t extractUint64(const std::string& json, const std::string& key) {
    auto v = extractUint(json, key);
    return v.value_or(0);
}

std::string ControlMessage::toJson() const {
    std::ostringstream ss;
    ss << "{";

    // type
    ss << "\"type\":\"" << static_cast<int>(type) << "\"";

    // seq
    ss << ",\"seq\":" << seq;

    if (password)   ss << ",\"password\":\"" << escapeJson(*password) << "\"";
    if (challenge)  ss << ",\"challenge\":\"" << escapeJson(*challenge) << "\"";
    if (response)   ss << ",\"response\":\"" << escapeJson(*response) << "\"";
    if (publicKey)  ss << ",\"publicKey\":\"" << escapeJson(*publicKey) << "\"";
    if (udpPort)    ss << ",\"udpPort\":" << *udpPort;
    if (errorMessage) ss << ",\"errorMessage\":\"" << escapeJson(*errorMessage) << "\"";
    if (screenWidth)  ss << ",\"screenWidth\":" << *screenWidth;
    if (screenHeight) ss << ",\"screenHeight\":" << *screenHeight;
    if (targetBitrate) ss << ",\"targetBitrate\":" << *targetBitrate;
    if (targetFps)  ss << ",\"targetFps\":" << *targetFps;

    if (!inputEvents.empty()) {
        ss << ",\"inputEvents\":[";
        for (size_t i = 0; i < inputEvents.size(); ++i) {
            if (i > 0) ss << ",";
            const auto& ev = inputEvents[i];
            ss << "{\"type\":" << static_cast<int>(ev.type)
               << ",\"ts\":" << ev.timestamp;
            if (ev.mouseMove) {
                ss << ",\"dx\":" << ev.mouseMove->dx
                   << ",\"dy\":" << ev.mouseMove->dy;
            }
            if (ev.mouseButton) {
                ss << ",\"btn\":" << static_cast<int>(ev.mouseButton->button);
            }
            if (ev.mouseWheel) {
                ss << ",\"delta\":" << ev.mouseWheel->delta;
            }
            if (ev.key) {
                ss << ",\"vk\":" << ev.key->vkCode
                   << ",\"ext\":" << (ev.key->extended ? 1 : 0);
            }
            ss << "}";
        }
        ss << "]";
    }

    ss << "}";
    return ss.str();
}

std::optional<ControlMessage> ControlMessage::fromJson(const std::string& json) {
    ControlMessage msg;

    auto typeStr = extractString(json, "type");
    if (typeStr.empty()) return std::nullopt;
    int typeVal = strtol(typeStr.c_str(), nullptr, 10);
    msg.type = static_cast<MessageType>(typeVal);
    msg.seq = extractUint(json, "seq").value_or(0);

    msg.password = json.find("\"password\"") != std::string::npos
        ? std::make_optional(extractString(json, "password")) : std::nullopt;
    msg.challenge = json.find("\"challenge\"") != std::string::npos
        ? std::make_optional(extractString(json, "challenge")) : std::nullopt;
    msg.response = json.find("\"response\"") != std::string::npos
        ? std::make_optional(extractString(json, "response")) : std::nullopt;
    msg.publicKey = json.find("\"publicKey\"") != std::string::npos
        ? std::make_optional(extractString(json, "publicKey")) : std::nullopt;
    msg.errorMessage = json.find("\"errorMessage\"") != std::string::npos
        ? std::make_optional(extractString(json, "errorMessage")) : std::nullopt;

    msg.udpPort = json.find("\"udpPort\"") != std::string::npos
        ? std::make_optional(static_cast<uint16_t>(extractUint(json, "udpPort").value_or(0))) : std::nullopt;
    msg.screenWidth = extractUint(json, "screenWidth");
    msg.screenHeight = extractUint(json, "screenHeight");
    msg.targetBitrate = extractUint(json, "targetBitrate");
    msg.targetFps = extractUint(json, "targetFps");

    // Parse inputEvents array (simplified: extract individual events)
    size_t arrStart = json.find("\"inputEvents\":[");
    if (arrStart != std::string::npos) {
        arrStart += 15; // past "inputEvents":[
        size_t pos = arrStart;
        while (pos < json.size()) {
            if (json[pos] == ']') break;
            if (json[pos] == '{') {
                InputEvent ev;
                ev.type = static_cast<InputType>(extractUint(json.substr(pos), "type").value_or(0));
                ev.timestamp = extractUint64(json.substr(pos), "ts");

                if (json.substr(pos).find("\"dx\"") < json.substr(pos).find("}")) {
                    MouseMoveEvent mme;
                    mme.dx = static_cast<int16_t>(extractInt(json.substr(pos), "dx").value_or(0));
                    mme.dy = static_cast<int16_t>(extractInt(json.substr(pos), "dy").value_or(0));
                    ev.mouseMove = mme;
                }
                if (json.substr(pos).find("\"btn\"") < json.substr(pos).find("}")) {
                    MouseButtonEvent mbe;
                    mbe.button = static_cast<uint8_t>(extractUint(json.substr(pos), "btn").value_or(0));
                    ev.mouseButton = mbe;
                }
                if (json.substr(pos).find("\"delta\"") < json.substr(pos).find("}")) {
                    MouseWheelEvent mwe;
                    mwe.delta = static_cast<int16_t>(extractInt(json.substr(pos), "delta").value_or(0));
                    ev.mouseWheel = mwe;
                }
                if (json.substr(pos).find("\"vk\"") < json.substr(pos).find("}")) {
                    KeyEvent ke;
                    ke.vkCode = static_cast<uint16_t>(extractUint(json.substr(pos), "vk").value_or(0));
                    ke.extended = extractUint(json.substr(pos), "ext").value_or(0) != 0;
                    ev.key = ke;
                }
                msg.inputEvents.push_back(ev);

                size_t close = json.find('}', pos);
                if (close == std::string::npos) break;
                pos = close + 1;
            } else {
                ++pos;
            }
        }
    }

    return msg;
}

std::vector<uint8_t> ControlMessage::serialize() const {
    std::string json = toJson();
    std::vector<uint8_t> buf(4 + json.size());
    uint32_t len = static_cast<uint32_t>(json.size());
    buf[0] = static_cast<uint8_t>((len >> 24) & 0xFF);
    buf[1] = static_cast<uint8_t>((len >> 16) & 0xFF);
    buf[2] = static_cast<uint8_t>((len >> 8) & 0xFF);
    buf[3] = static_cast<uint8_t>(len & 0xFF);
    memcpy(buf.data() + 4, json.data(), json.size());
    return buf;
}

std::optional<ControlMessage> ControlMessage::deserialize(const uint8_t* data, size_t len) {
    if (len == 0) return std::nullopt;
    std::string json(reinterpret_cast<const char*>(data), len);
    return fromJson(json);
}

} // namespace Protocol
