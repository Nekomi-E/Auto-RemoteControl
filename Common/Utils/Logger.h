#pragma once
#include <string>
#include <mutex>
#include <cstdio>
#include <chrono>

enum class ConsoleColor : uint8_t {
    Default = 0,
    Green,
    Red,
    Yellow,
    Blue,
    Magenta,
    Cyan,
    White,
    BrightGreen,
    // ... 按需添加，必须保持从0连续
    Count      // 用于数组大小
};

enum class LogLevel { Debug = 0, Info = 1, Warning = 2, Error = 3 };

class Logger {
public:
    static Logger& Instance();//单例模式，全局唯一的Logger实例

    void SetLevel(LogLevel level);
    void SetFile(const std::string& filePath);
    void Log(LogLevel level, const char* file, int line, const char* format, ...);

    void Debug(const char* file, int line, const char* format, ...);
    void Info(const char* file, int line, const char* format, ...);
    void Warning(const char* file, int line, const char* format, ...);
    void Error(const char* file, int line, const char* format, ...);
    void Info(const char* file, int line, ConsoleColor color, const char* format, ...);//增加带颜色的系列函数重载


private:
    Logger() = default;
    //void Write(LogLevel level, const char* file, int line, const std::string& msg);
    void Write(LogLevel level, const char* file, int line, const std::string& msg, ConsoleColor = ConsoleColor::Default);

    std::mutex m_mutex;
    LogLevel m_level = LogLevel::Debug;
    FILE* m_file = nullptr;
};

#define LOG_DEBUG(fmt, ...)   Logger::Instance().Debug(__FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)    Logger::Instance().Info(__FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_WARNING(fmt, ...) Logger::Instance().Warning(__FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...)   Logger::Instance().Error(__FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define LOG_INFO_COLOR(color, fmt, ...) Logger::Instance().Info(__FILE__, __LINE__, color, fmt, ##__VA_ARGS__)
#define LOG_INFO_GREEN(fmt, ...) LOG_INFO_COLOR(ConsoleColor::Green, fmt, ##__VA_ARGS__)
#define LOG_INFO_YELLOW(fmt, ...) LOG_INFO_COLOR(ConsoleColor::Yellow, fmt, ##__VA_ARGS__)