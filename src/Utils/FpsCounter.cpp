#include "FpsCounter.h"
#include <chrono>

using namespace wallpaper;
using namespace std::chrono;

FpsCounter::FpsCounter(): m_fps(0), m_frameCount(0), m_startTime(steady_clock::now()) {};

// Republish the rolling average every ~half second so consumers (SceneScript
// `engine.fps`) see updates quickly when the user toggles a heavy effect on
// or off, while still averaging across enough frames to smooth out hitches.
constexpr milliseconds kPublishWindow { 500 };

void FpsCounter::RegisterFrame() {
    auto now  = steady_clock::now();
    auto diff = now - m_startTime;

    m_frameCount++;
    if (diff > kPublishWindow) {
        m_fps.store((u32)(m_frameCount / duration<double>(diff).count()),
                    std::memory_order_relaxed);
        m_frameCount = 0;
        m_startTime  = now;
    }
}
