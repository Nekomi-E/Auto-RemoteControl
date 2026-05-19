#include "Timer.h"
#include <windows.h>

Timer::Timer() : m_start(NowMs()) {}

void Timer::Reset() {
    m_start = NowMs();
}

int64_t Timer::ElapsedMs() const {
    return NowMs() - m_start;
}

int64_t Timer::ElapsedUs() const {
    return NowUs() - m_start * 1000;
}

int64_t Timer::NowMs() {
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return counter.QuadPart * 1000 / Frequency();
}

int64_t Timer::NowUs() {
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return counter.QuadPart * 1000000 / Frequency();
}

int64_t Timer::Frequency() {
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    return freq.QuadPart;
}
