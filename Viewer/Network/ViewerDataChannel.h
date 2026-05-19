#pragma once
#include "Common/Protocol/FrameHeader.h"
#include "Common/Security/SecureChannel.h"
#include <winsock2.h>
#include <vector>
#include <cstdint>
#include <optional>

class ViewerDataChannel {
public:
    struct ReceivedFrame {
        std::vector<uint8_t> data;
        Protocol::FrameType type;
        uint32_t timestampMs = 0;
    };

    ViewerDataChannel() = default;

    void SetSecureChannel(SecureChannel* sc) { m_secureChannel = sc; }
    void SetEncrypted(bool enc) { m_encrypted = enc; }

    std::optional<ReceivedFrame> ReceiveFrame(SOCKET sock, int timeoutMs);

private:
    SecureChannel* m_secureChannel = nullptr;
    bool m_encrypted = false;
};
