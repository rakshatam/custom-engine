#pragma once
#include <windows.h>

namespace Engine {

class Timer {
public:
    Timer() {
        QueryPerformanceFrequency(&m_frequency);
        QueryPerformanceCounter(&m_startTime);
        m_prevTime = m_startTime;
    }

    void reset() {
        QueryPerformanceCounter(&m_startTime);
        m_prevTime = m_startTime;
    }

    float tick() {
        LARGE_INTEGER currentTime;
        QueryPerformanceCounter(&currentTime);
        float dt = static_cast<float>(currentTime.QuadPart - m_prevTime.QuadPart) / static_cast<float>(m_frequency.QuadPart);
        m_prevTime = currentTime;
        return dt;
    }

    float totalTime() const {
        LARGE_INTEGER currentTime;
        QueryPerformanceCounter(&currentTime);
        return static_cast<float>(currentTime.QuadPart - m_startTime.QuadPart) / static_cast<float>(m_frequency.QuadPart);
    }

private:
    LARGE_INTEGER m_frequency;
    LARGE_INTEGER m_startTime;
    LARGE_INTEGER m_prevTime;
};

} // namespace Engine
