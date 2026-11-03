#pragma once
#include <algorithm>
#include <chrono>
#include <cmath>

namespace Engine {

class Timer {
public:
    using Clock = std::chrono::steady_clock;

    Timer() { reset(); }

    void reset() {
        m_startTime = Clock::now();
        m_prevTime = m_startTime;
    }

    float tick() {
        const Clock::time_point currentTime = Clock::now();
        const float dt = std::chrono::duration<float>(currentTime - m_prevTime).count();
        m_prevTime = currentTime;
        return std::isfinite(dt) ? std::max(dt, 0.0f) : 0.0f;
    }

    float totalTime() const {
        const float elapsed = std::chrono::duration<float>(Clock::now() - m_startTime).count();
        return std::isfinite(elapsed) ? std::max(elapsed, 0.0f) : 0.0f;
    }

private:
    Clock::time_point m_startTime{};
    Clock::time_point m_prevTime{};
};

} // namespace Engine
