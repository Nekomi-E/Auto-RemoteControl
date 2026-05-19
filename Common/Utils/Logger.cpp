#include "Logger.h"
#include <cstdarg>
#include <ctime>

Logger& Logger::Instance() {
    static Logger logger;
    return logger;
}

void Logger::SetLevel(LogLevel level) {
    std::lock_guard lock(m_mutex);
    m_level = level;
}

void Logger::SetFile(const std::string& filePath) {
    std::lock_guard lock(m_mutex);
    if (m_file) fclose(m_file);
    m_file = fopen(filePath.c_str(), "a");
}

void Logger::Log(LogLevel level, const char* file, int line, const char* format, ...) {
    if (level < m_level) return;

    va_list args;
    va_start(args, format);
    char buf[4096];
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);

    Write(level, file, line, buf);
}

void Logger::Debug(const char* file, int line, const char* format, ...) {
    if (m_level > LogLevel::Debug) return;
    va_list args;
    va_start(args, format);
    char buf[4096];
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    Write(LogLevel::Debug, file, line, buf);
}

void Logger::Info(const char* file, int line, const char* format, ...) {
    if (m_level > LogLevel::Info) return;
    va_list args;
    va_start(args, format);
    char buf[4096];
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    Write(LogLevel::Info, file, line, buf);
}

void Logger::Warning(const char* file, int line, const char* format, ...) {
    if (m_level > LogLevel::Warning) return;
    va_list args;
    va_start(args, format);
    char buf[4096];
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    Write(LogLevel::Warning, file, line, buf);
}

void Logger::Error(const char* file, int line, const char* format, ...) {
    va_list args;
    va_start(args, format);
    char buf[4096];
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    Write(LogLevel::Error, file, line, buf);
}

void Logger::Write(LogLevel level, const char* file, int line, const std::string& msg) {
    const char* levelStr[] = {"DEBUG", "INFO ", "WARN ", "ERROR"};

    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    char timeBuf[32];
    strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", localtime(&time));

    // Extract filename from path
    const char* filename = file;
    const char* lastSlash = strrchr(file, '\\');
    if (lastSlash) filename = lastSlash + 1;

    char out[4608];
    int n = snprintf(out, sizeof(out), "[%s] [%s] %s:%d %s\n",
                     timeBuf, levelStr[static_cast<int>(level)], filename, line, msg.c_str());

    std::lock_guard lock(m_mutex);
    fwrite(out, 1, n > 0 ? n : sizeof(out) - 1, stdout);
    fflush(stdout);
    if (m_file) {
        fwrite(out, 1, n > 0 ? n : sizeof(out) - 1, m_file);
        fflush(m_file);
    }
}
