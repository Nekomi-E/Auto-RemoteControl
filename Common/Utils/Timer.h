#pragma once
#include <cstdint>

class Timer {
public:
    Timer();

    void Reset();
    int64_t ElapsedMs() const;
    int64_t ElapsedUs() const;

    static int64_t NowMs();
    static int64_t NowUs();

private:
    int64_t m_start;
    static int64_t Frequency();
};
