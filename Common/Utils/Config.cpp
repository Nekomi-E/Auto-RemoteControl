#include "Config.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>

static bool parseUint16(const char* s, uint16_t& out) {
    char* end;
    long val = strtol(s, &end, 10);
    if (end == s || *end != '\0' || val < 0 || val > 65535) return false;
    out = static_cast<uint16_t>(val);
    return true;
}

static bool parseUint32(const char* s, uint32_t& out) {
    char* end;
    long long val = strtoll(s, &end, 10);
    if (end == s || *end != '\0' || val < 0) return false;
    out = static_cast<uint32_t>(val);
    return true;
}

AgentConfig Config::ParseAgentArgs(int argc, char* argv[]) {
    AgentConfig cfg;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            parseUint16(argv[++i], cfg.port);
        } else if (strcmp(argv[i], "--password") == 0 && i + 1 < argc) {
            cfg.password = argv[++i];
        } else if (strcmp(argv[i], "--bitrate") == 0 && i + 1 < argc) {
            parseUint32(argv[++i], cfg.videoBitrate);
        } else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
            parseUint32(argv[++i], cfg.targetFps);
        } else if (strcmp(argv[i], "--no-audio") == 0) {
            cfg.enableAudio = false;
        } else if (strcmp(argv[i], "--no-encrypt") == 0) {
            cfg.enableEncryption = false;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            PrintAgentHelp();
            exit(0);
        }
    }
    return cfg;
}

ViewerConfig Config::ParseViewerArgs(int argc, char* argv[]) {
    ViewerConfig cfg;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            cfg.host = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            parseUint16(argv[++i], cfg.port);
        } else if (strcmp(argv[i], "--password") == 0 && i + 1 < argc) {
            cfg.password = argv[++i];
        } else if (strcmp(argv[i], "--no-audio") == 0) {
            cfg.enableAudio = false;
        } else if (strcmp(argv[i], "--no-encrypt") == 0) {
            cfg.enableEncryption = false;
        } else if (strcmp(argv[i], "--fullscreen") == 0) {
            cfg.fullscreen = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            PrintViewerHelp();
            exit(0);
        }
    }
    return cfg;
}

void Config::PrintAgentHelp() {
    printf("RemoteControl Agent (被控端) v1.0\n\n");
    printf("Usage: Agent.exe [options]\n\n");
    printf("Options:\n");
    printf("  --port N        TCP/UDP port to listen on (default: 27015)\n");
    printf("  --password STR  Authentication password (required)\n");
    printf("  --bitrate N     Video bitrate in bps (default: 4000000)\n");
    printf("  --fps N         Target capture framerate (default: 30)\n");
    printf("  --no-audio      Disable audio capture\n");
    printf("  --no-encrypt    Disable encryption (not recommended)\n");
    printf("  --help, -h      Show this help\n");
}

void Config::PrintViewerHelp() {
    printf("RemoteControl Viewer (主控端) v1.0\n\n");
    printf("Usage: Viewer.exe [options]\n\n");
    printf("Options:\n");
    printf("  --host IP       Agent IP address to connect to (default: 127.0.0.1)\n");
    printf("  --port N        TCP/UDP port (default: 27015)\n");
    printf("  --password STR  Authentication password (required)\n");
    printf("  --no-audio      Disable audio playback\n");
    printf("  --no-encrypt    Disable encryption (not recommended)\n");
    printf("  --fullscreen    Start in fullscreen mode\n");
    printf("  --help, -h      Show this help\n");
}
