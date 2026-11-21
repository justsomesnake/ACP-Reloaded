#include "frame_pacer.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <vector>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace acp::hook::pacing {
namespace {

std::atomic<int> g_fgLimit{0};
std::atomic<int> g_bgLimit{0};

// Present thread only
struct State {
    std::int64_t freq = 0;
    std::int64_t next = 0;       // next frame deadline (QPC)
    std::int64_t lastFrame = 0;  // QPC of the previous Present
    HANDLE timer = nullptr;
    bool timerTried = false;
    Stats stats;
    float window[64] = {};       // for the FPS average
} g;

std::int64_t Qpc() {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}

// Waits until target (QPC): coarsely via a high-resolution timer, the last ~1.5 ms busy-waiting (precise, no stutter).
void WaitUntil(std::int64_t target) {
    if (!g.timerTried) {
        g.timerTried = true;
        g.timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!g.timer) g.timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);  // older Windows versions
    }
    const std::int64_t spin = g.freq * 15 / 10000;  // 1.5 ms
    std::int64_t now = Qpc();
    if (target - now > spin && g.timer) {
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>((target - now - spin) * 10000000 / g.freq);  // 100 ns units, relative
        if (SetWaitableTimerEx(g.timer, &due, 0, nullptr, nullptr, nullptr, 0)) WaitForSingleObject(g.timer, 100);
    }
    while ((now = Qpc()) < target) YieldProcessor();
}

void Record(std::int64_t now) {
    if (g.lastFrame != 0) {
        const float ms = static_cast<float>(static_cast<double>(now - g.lastFrame) * 1000.0 / static_cast<double>(g.freq));
        Stats& s = g.stats;
        const int idx = (s.head + s.count) % kHistory;
        s.history[idx] = ms;
        if (s.count < kHistory) ++s.count;
        else s.head = (s.head + 1) % kHistory;
        s.frameMs = ms;
    }
    g.lastFrame = now;
}

}  // namespace

void SetLimits(int foregroundFps, int backgroundFps) {
    g_fgLimit.store(std::clamp(foregroundFps, 0, 1000), std::memory_order_relaxed);
    g_bgLimit.store(std::clamp(backgroundFps, 0, 1000), std::memory_order_relaxed);
}

void BeforePresent(bool foreground) {
    if (g.freq == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g.freq = f.QuadPart;
    }
    int limit = foreground ? g_fgLimit.load(std::memory_order_relaxed) : g_bgLimit.load(std::memory_order_relaxed);
    if (!foreground && limit == 0) limit = g_fgLimit.load(std::memory_order_relaxed);
    g.stats.limit = limit;

    if (limit > 0) {
        const std::int64_t period = g.freq / limit;
        const std::int64_t now = Qpc();
        if (g.next == 0 || now - g.next > period * 4 || g.next - now > period * 2) {
            g.next = now + period;  // start, after loading pauses or a limit change: restart the cadence instead of catching up
        } else {
            WaitUntil(g.next);
            const std::int64_t after = Qpc();
            // Stay on cadence; if the frame was late anyway, count from now (no frame bursts to catch up).
            g.next = after - g.next > period ? after + period : g.next + period;
        }
    } else {
        g.next = 0;
    }
    Record(Qpc());
}

void GetStats(Stats& out) {
    Stats& s = g.stats;
    // FPS over the most recent ~0.5 s, average and 1 % low over the whole history
    float sum = 0, recent = 0;
    int recentN = 0;
    std::vector<float> ms;
    ms.reserve(static_cast<std::size_t>(s.count));
    for (int i = s.count - 1; i >= 0; --i) {
        const float v = s.history[(s.head + i) % kHistory];
        sum += v;
        ms.push_back(v);
        if (recent < 500.0f) {
            recent += v;
            ++recentN;
        }
    }
    s.fps = recent > 0 ? 1000.0f * static_cast<float>(recentN) / recent : 0.0f;
    s.avgMs = s.count ? sum / static_cast<float>(s.count) : 0.0f;
    s.low1Fps = OnePercentLowFps(ms.data(), static_cast<int>(ms.size()));
    out = s;
}

float OnePercentLowFps(const float* ms, int n) {
    if (n <= 0) return 0.0f;
    std::vector<float> v(ms, ms + n);
    std::sort(v.begin(), v.end(), [](float a, float b) { return a > b; });  // longest frame times first
    const int k = std::max(1, n / 100);
    float sum = 0;
    for (int i = 0; i < k; ++i) sum += v[static_cast<std::size_t>(i)];
    const float avg = sum / static_cast<float>(k);
    return avg > 0 ? 1000.0f / avg : 0.0f;
}

}  // namespace acp::hook::pacing
