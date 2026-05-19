#include "Common/Common.h"
#include "Common/Utils/Logger.h"
#include "Common/Utils/Config.h"
#include "AgentSession.h"
#include <timeapi.h>

int main(int argc, char* argv[]) {
    timeBeginPeriod(1);  // 1ms timer resolution for precise Sleep at 60fps
    Logger::Instance().SetLevel(LogLevel::Debug);

    auto config = Config::ParseAgentArgs(argc, argv);

    if (config.password.empty()) {
        LOG_ERROR("Password is required. Use --password <password> to set one.");
        Config::PrintAgentHelp();
        printf("\nPress Enter to exit...");
        getchar();
        return 1;
    }

    LOG_INFO("RemoteControl Agent starting on port %u", config.port);

    AgentSession session;
    if (!session.Initialize(config)) {
        LOG_ERROR("Failed to initialize Agent session");
        printf("\nPress Enter to exit...");
        getchar();
        return 1;
    }

    session.Run();

    LOG_INFO("Agent stopped.");
    timeEndPeriod(1);
    return 0;
}
