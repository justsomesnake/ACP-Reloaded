#pragma once

// PadFilter: decides, per XInput poll, what the game sees from the controller.
// Pure (no Win32), unit-tested. One instance per pad index; Step() is not reentrant.

#include <cstdint>

namespace acp::input {

// Bit masks matching XINPUT_GAMEPAD_* (without depending on xinput.h).
namespace pad {
inline constexpr std::uint16_t DpadUp = 0x0001, DpadDown = 0x0002, DpadLeft = 0x0004, DpadRight = 0x0008;
inline constexpr std::uint16_t Start = 0x0010, Back = 0x0020, LeftThumb = 0x0040, RightThumb = 0x0080;
inline constexpr std::uint16_t LeftShoulder = 0x0100, RightShoulder = 0x0200;
inline constexpr std::uint16_t A = 0x1000, B = 0x2000, X = 0x4000, Y = 0x8000;
inline constexpr std::uint8_t TriggerThreshold = 30;  // XINPUT_GAMEPAD_TRIGGER_THRESHOLD
}  // namespace pad

struct PadRaw {
    std::uint16_t buttons = 0;
    std::uint8_t lt = 0, rt = 0;
    std::int16_t lx = 0, ly = 0, rx = 0, ry = 0;
    friend bool operator==(const PadRaw&, const PadRaw&) = default;
};

enum class PadGate : std::uint8_t {
    Pass,   // game sees everything (except chord masking and the release latch)
    Block,  // overlay open: game sees a neutral controller
};

struct PadFilterConfig {
    std::uint16_t chordMask = pad::Back | pad::Start;  // View + Menu
    std::uint16_t holdMs = 500;    // hold time until toggling
    std::uint16_t deferMs = 120;   // how long a single chord button is held back
    std::uint16_t replayMs = 60;   // minimum replay time for a short tap
    bool chordEnabled = true;
};

struct PadFilterOut {
    PadRaw visible;           // what the game should see
    std::uint32_t packet = 0; // own packet number: counts changes of `visible`
    bool toggleFired = false; // chord held long enough (once per press)
};

class PadFilter {
public:
    PadFilterOut Step(std::uint64_t nowMs, bool connected, const PadRaw& raw, PadGate gate,
                      const PadFilterConfig& cfg) noexcept;

private:
    enum class Chord : std::uint8_t { Idle, Deferring, Replay, Held, PassThrough };

    Chord chord_ = Chord::Idle;
    std::uint16_t deferredBits_ = 0;
    std::uint64_t t0_ = 0;
    std::uint64_t replayUntil_ = 0;
    int replaySteps_ = 0;
    bool fired_ = false;

    PadGate prevGate_ = PadGate::Pass;
    std::uint16_t latchButtons_ = 0;
    bool latchLt_ = false, latchRt_ = false;

    PadRaw lastVisible_{};
    bool lastConnected_ = false;
    std::uint32_t packet_ = 0;
};

}  // namespace acp::input
