#pragma once
#include <string>
#include <cstdint>

struct AgentConfig {
    uint16_t port = 27015;
    std::string password;
    uint32_t videoBitrate = 0;          // 0 = auto (scaled to resolution + quality)
    uint32_t targetFps = 60;
    bool enableAudio = true;
    uint32_t audioBitrate = 64000;    // 64 kbps AAC
    bool enableEncryption = true;
    uint32_t videoQuality = 0;          // 0=auto, 1=balanced, 2=lossless ("原画")
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
