// Tests for the frame-rate limiter (math part and a rough timing check).
#include <windows.h>

#include <chrono>

#include "frame_pacer.h"
#include "test_main.h"

using namespace acp::hook::pacing;

TEST(PacingOnePercentLow) {
    float ms[200];
    for (float& v : ms) v = 10.0f;  // 100 FPS
    ms[17] = 50.0f;                  // two stutters of 50 ms each
    ms[123] = 50.0f;
    CHECK(OnePercentLowFps(ms, 200) > 19.9f && OnePercentLowFps(ms, 200) < 20.1f);  // 1 % of 200 = 2 frames
    CHECK(OnePercentLowFps(ms, 0) == 0.0f);
}

TEST(PacingHoldsTargetRate) {
    SetLimits(100, 0);
    BeforePresent(true);  // first call only syncs the clock
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 30; ++i) BeforePresent(true);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    CHECK(ms >= 280 && ms <= 360);  // 30 frames of 10 ms each (generous for busy test machines)
    Stats st;
    GetStats(st);
    CHECK(st.limit == 100);
    CHECK(st.fps > 90.0f && st.fps < 110.0f);
    SetLimits(0, 0);
}
