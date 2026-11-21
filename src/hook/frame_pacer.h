#pragma once

// Frame rate limiter and frame time statistics in the Present detour (like RTSS/Special K: wait before the real Present).
// No intervention in the game: only waiting on the render thread. All functions except SetLimits() run only on the
// thread that calls Present.

#include <cstdint>

namespace acp::hook::pacing {

// Any thread: target FPS (0 = off) in the foreground, and when the game is not the active window.
void SetLimits(int foregroundFps, int backgroundFps);

// Present thread, outermost Present call, right before the real Present: waits until the next frame deadline and
// measures the frame time. foreground: game window is active.
void BeforePresent(bool foreground);

constexpr int kHistory = 240;
struct Stats {
    float fps = 0;        // average over approx. 0.5 s
    float frameMs = 0;    // last frame time
    float avgMs = 0;      // average over the history
    float low1Fps = 0;    // 1 % low over the history
    int limit = 0;        // active target (0 = off)
    int count = 0;        // valid entries in history
    int head = 0;         // index of the oldest entry
    float history[kHistory] = {};
};
// Present thread (the overlay draws on the same thread): current state.
void GetStats(Stats& out);

// Pure helper (testable): 1 % low in FPS from frame times in ms.
float OnePercentLowFps(const float* ms, int n);

}  // namespace acp::hook::pacing
