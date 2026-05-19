#pragma once
#include "Common/Protocol/ControlMessage.h"
#include <winsock2.h>
#include <optional>

// Thin wrapper for TCP control message operations.
// The heavy lifting is in AgentNetworkImpl; this provides reusable helpers.
class ControlChannel {
public:
    static bool SendMessage(SOCKET sock, const Protocol::ControlMessage& msg);
    static std::optional<Protocol::ControlMessage> ReceiveMessage(SOCKET sock, int timeoutMs);
};
