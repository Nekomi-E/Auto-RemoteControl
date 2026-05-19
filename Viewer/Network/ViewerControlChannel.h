#pragma once
#include "Common/Protocol/ControlMessage.h"
#include <winsock2.h>
#include <optional>

class ViewerControlChannel {
public:
    static bool SendMessage(SOCKET sock, const Protocol::ControlMessage& msg);
    static std::optional<Protocol::ControlMessage> ReceiveMessage(SOCKET sock, int timeoutMs);
};
