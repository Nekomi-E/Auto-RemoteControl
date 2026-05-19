#pragma once
#include "Common/Protocol/FrameHeader.h"
#include "Common/Security/SecureChannel.h"
#include <winsock2.h>
#include <vector>
#include <cstdint>

class DataChannel {
public:
    DataChannel() = default;

    void SetSecureChannel(SecureChannel* sc) { m_secureChannel = sc; }
    void SetEncrypted(bool enc) { m_encrypted = enc; }

    bool SendFrame(SOCKET sock, const sockaddr_in& dest,
                   Protocol::FrameType type, uint16_t seq, uint32_t timestampMs,
                   const uint8_t* payload, uint32_t payloadSize);

private:
    SecureChannel* m_secureChannel = nullptr;
    bool m_encrypted = false;
};
