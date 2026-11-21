// Tests for PadFilter (unit-testable part).
#include "pad_filter.h"
#include "test_main.h"

using namespace acp::input;

namespace {

struct Sim {
    PadFilter f;
    PadFilterConfig cfg;
    std::uint64_t t = 1000;
    int toggles = 0;
    PadFilterOut last;
    // One poll step every `dt` ms
    PadFilterOut Step(std::uint16_t buttons, PadGate gate = PadGate::Pass, std::uint64_t dt = 8, std::uint8_t lt = 0) {
        t += dt;
        PadRaw r;
        r.buttons = buttons;
        r.lt = lt;
        last = f.Step(t, true, r, gate, cfg);
        toggles += last.toggleFired;
        return last;
    }
};

}  // namespace

TEST(PadChordTogglesOnceAndIsNeverVisible) {
    Sim s;
    bool sawChordButton = false;
    s.Step(pad::Back);  // View first
    sawChordButton |= (s.last.visible.buttons & (pad::Back | pad::Start)) != 0;
    for (int i = 0; i < 100; ++i) {  // both held for 800 ms
        s.Step(pad::Back | pad::Start);
        sawChordButton |= (s.last.visible.buttons & (pad::Back | pad::Start)) != 0;
    }
    s.Step(pad::Start);  // release one button first
    sawChordButton |= (s.last.visible.buttons & (pad::Back | pad::Start)) != 0;
    s.Step(0);
    CHECK(s.toggles == 1);
    CHECK(!sawChordButton);
}

TEST(PadShortTapIsReplayedAtLeastTwoPollsAnd60ms) {
    for (int tapMs : {30, 60, 100}) {
        Sim s;
        int visiblePolls = 0;
        std::uint64_t firstVisible = 0, lastVisible = 0;
        const int pollsHeld = tapMs / 8;
        for (int i = 0; i < pollsHeld; ++i) {
            s.Step(pad::Start);
            if (s.last.visible.buttons & pad::Start) {
                if (!visiblePolls) firstVisible = s.t;
                lastVisible = s.t;
                ++visiblePolls;
            }
        }
        for (int i = 0; i < 30; ++i) {
            s.Step(0);
            if (s.last.visible.buttons & pad::Start) {
                if (!visiblePolls) firstVisible = s.t;
                lastVisible = s.t;
                ++visiblePolls;
            }
        }
        CHECK(visiblePolls >= 2);
        CHECK(lastVisible - firstVisible + 8 >= 60);
        CHECK(s.toggles == 0);
    }
}

TEST(PadLongSingleHoldPassesThroughAfterDefer) {
    Sim s;
    bool visibleLate = false, visibleEarly = false;
    for (int i = 0; i < 40; ++i) {  // 320 ms View only
        s.Step(pad::Back);
        const bool vis = s.last.visible.buttons & pad::Back;
        if (s.t - 1000 < 120) visibleEarly |= vis;
        else visibleLate |= vis;
    }
    CHECK(!visibleEarly);
    CHECK(visibleLate);
    CHECK(s.toggles == 0);
}

TEST(PadBlockZeroesEverythingAndPacketCountsVisibleChanges) {
    Sim s;
    const auto a = s.Step(pad::A);
    CHECK(a.visible.buttons == pad::A);
    const auto b = s.Step(pad::A, PadGate::Block);
    CHECK(b.visible.buttons == 0 && b.visible.lt == 0);
    const auto c = s.Step(pad::A | pad::X, PadGate::Block, 8, 200);
    CHECK(c.visible == PadRaw{});
    CHECK(c.packet == b.packet);  // visible state unchanged
    CHECK(b.packet != a.packet);
}

TEST(PadReleaseLatchHidesHeldButtonsAfterClose) {
    Sim s;
    s.Step(pad::B, PadGate::Block, 8, 255);
    const auto closed = s.Step(pad::B, PadGate::Pass, 8, 255);  // closed, B and RT/LT still held
    CHECK((closed.visible.buttons & pad::B) == 0);
    CHECK(closed.visible.lt == 0);
    const auto other = s.Step(pad::B | pad::A, PadGate::Pass, 8, 255);
    CHECK((other.visible.buttons & pad::A) != 0);  // newly pressed button is visible
    CHECK((other.visible.buttons & pad::B) == 0);
    s.Step(pad::A);                                  // B released, triggers released
    const auto again = s.Step(pad::B | pad::A, PadGate::Pass, 8, 255);
    CHECK((again.visible.buttons & pad::B) != 0);  // normal again after release
    CHECK(again.visible.lt == 255);
}

TEST(PadDisconnectResets) {
    Sim s;
    s.Step(pad::Back);
    PadRaw none;
    const auto d = s.f.Step(s.t + 8, false, none, PadGate::Pass, s.cfg);
    CHECK(d.visible == PadRaw{});
    const auto r = s.Step(pad::A);
    CHECK(r.visible.buttons == pad::A);
}

TEST(PadChordDisabledPassesEverything) {
    Sim s;
    s.cfg.chordEnabled = false;
    const auto o = s.Step(pad::Back | pad::Start);
    CHECK(o.visible.buttons == (pad::Back | pad::Start));
    for (int i = 0; i < 100; ++i) s.Step(pad::Back | pad::Start);
    CHECK(s.toggles == 0);
}
