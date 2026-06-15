#include "Logger.h"
#include <cstdarg>
#include <ctime>
#include <windows.h>

#include <array>

// 静态映射表（constexpr，编译期完成）
constexpr std::array<const char*, static_cast<size_t>(ConsoleColor::Count)> kAnsiColorCodes = {//std::array<数据类型, 数组长度>
    "",                 // Default
    "\033[32m",         // Green
    "\033[31m",         // Red
    "\033[33m",         // Yellow
    "\033[34m",         // Blue
    "\033[35m",         // Magenta
    "\033[36m",         // Cyan
    "\033[37m",         // White
    "\033[92m",         // BrightGreen
};
constexpr const char* GetAnsiCode(ConsoleColor color) {//constexpr函数允许在编译期计算结果
    return kAnsiColorCodes[static_cast<size_t>(color)];
}


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

void Logger::Info(const char* file, int line, ConsoleColor color, const char* format, ...) {
    if (m_level > LogLevel::Info) return;
    va_list args;
    va_start(args, format);
    char buf[4096];
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    Write(LogLevel::Info, file, line, buf, color);
}

void Logger::Warning(const char* file, int line, const char* format, ...) {
    if (m_level > LogLevel::Warning) return;
    va_list args;
    va_start(args, format);
    char buf[4096];
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    //Write(LogLevel::Warning, file, line, buf);
    Write(LogLevel::Warning, file, line, buf, ConsoleColor::Yellow);//默认输出为黄色
}

void Logger::Error(const char* file, int line, const char* format, ...) {
    va_list args;
    va_start(args, format);
    char buf[4096];
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    //Write(LogLevel::Error, file, line, buf);
    Write(LogLevel::Error, file, line, buf, ConsoleColor::Red);//默认输出为红色
}

void Logger::Write(LogLevel level, const char* file, int line, const std::string& msg, ConsoleColor color) {
    const char* levelStr[] = { "DEBUG", "INFO ", "WARN ", "ERROR" };

    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    char timeBuf[32];
    strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", localtime(&time));

    // Extract filename from path
    const char* filename = file;
    const char* lastSlash = strrchr(file, '\\');
    if (lastSlash) filename = lastSlash + 1;
    
    //构建不带颜色的文本输出
    char plainOut[4608];
    int n = snprintf(plainOut, sizeof(plainOut), "[%s] [%s] %s:%d %s\n",
        timeBuf, levelStr[static_cast<int>(level)], filename, line, msg.c_str());

    // 1. 输出到 VS 调试器（纯文本）
    OutputDebugStringA(plainOut);

    std::lock_guard lock(m_mutex);

    // 2, 输出到文件（纯文本）
    if (m_file) {
        fwrite(plainOut, 1, n > 0 ? n : sizeof(plainOut) - 1, m_file);
        fflush(m_file);
    }

    // 3, 输出到控制台（带颜色）
    const char* colorCode = GetAnsiCode(color);
    if (color != ConsoleColor::Default && colorCode[0] != '/0') {
        //添加颜色标签， [color]message["\033[0m"] 重置码恢复默认颜色
        fprintf(stdout, "%s%s\033[0m", colorCode, plainOut);
    }
    else {
        fwrite(plainOut, 1, n > 0 ? n : sizeof(plainOut) - 1, stdout);  //去掉空字符，避免输出乱码
    }
    fflush(stdout);//立即输出到控制台

}