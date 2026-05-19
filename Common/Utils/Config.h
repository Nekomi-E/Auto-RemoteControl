#pragma once
#include <string>
#include <cstdint>

struct AgentConfig {
    uint16_t port = 27015;
    std::string password;
    uint32_t videoBitrate = 4000000;   // 4 Mbps
    uint32_t targetFps = 30;
    bool enableAudio = true;
    uint32_t audioBitrate = 64000;    // 64 kbps AAC
    bool enableEncryption = true;
};

struct ViewerConfig {
    std::string host = "127.0.0.1";
    uint16_t port = 27015;
    std::string password;
    bool enableAudio = true;
    bool enableEncryption = true;
    bool fullscreen = false;
};

class Config {
public:
    static AgentConfig ParseAgentArgs(int argc, char* argv[]);
    static ViewerConfig ParseViewerArgs(int argc, char* argv[]);
    static void PrintAgentHelp();
    static void PrintViewerHelp();
};
