#pragma once
#include <string>
#include <mutex>
#include <cstdio>
#include <chrono>

enum class LogLevel { Debug = 0, Info = 1, Warning = 2, Error = 3 };

class Logger {
public:
    static Logger& Instance();

    void SetLevel(LogLevel level);
    void SetFile(const std::string& filePath);
    void Log(LogLevel level, const char* file, int line, const char* format, ...);

    void Debug(const char* file, int line, const char* format, ...);
    void Info(const char* file, int line, const char* format, ...);
    void Warning(const char* file, int line, const char* format, ...);
    void Error(const char* file, int line, const char* format, ...);

private:
    Logger() = default;
    void Write(LogLevel level, const char* file, int line, const std::string& msg);

    std::mutex m_mutex;
    LogLevel m_level = LogLevel::Debug;
    FILE* m_file = nullptr;
};

#define LOG_DEBUG(fmt, ...)   Logger::Instance().Debug(__FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)    Logger::Instance().Info(__FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_WARNING(fmt, ...) Logger::Instance().Warning(__FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...)   Logger::Instance().Error(__FILE__, __LINE__, fmt, ##__VA_ARGS__)
