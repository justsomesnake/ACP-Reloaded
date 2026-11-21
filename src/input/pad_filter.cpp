#include "pad_filter.h"

namespace acp::input {

PadFilterOut PadFilter::Step(std::uint64_t now, bool connected, const PadRaw& raw, PadGate gate,
                             const PadFilterConfig& cfg) noexcept {
    PadFilterOut out;

    if (!connected) {
        chord_ = Chord::Idle;
        latchButtons_ = 0;
        latchLt_ = latchRt_ = false;
        prevGate_ = gate;
        if (lastConnected_) ++packet_;
        lastConnected_ = false;
        lastVisible_ = {};
        out.packet = packet_;
        return out;
    }

    // --- Chord detection and masking ---
    const std::uint16_t mask = cfg.chordEnabled ? cfg.chordMask : 0;
    const std::uint16_t held = raw.buttons & mask;
    std::uint16_t hide = 0;
    std::uint16_t add = 0;

    switch (chord_) {
        case Chord::Idle:
            if (mask && held == mask) {
                chord_ = Chord::Held;
                t0_ = now;
                fired_ = false;
                hide = mask;
            } else if (held) {
                chord_ = Chord::Deferring;
                deferredBits_ = held;
                t0_ = now;
                hide = held;
            }
            break;
        case Chord::Deferring:
            if (held == mask) {
                chord_ = Chord::Held;
                t0_ = now;
                fired_ = false;
                hide = mask;
            } else if (held == 0) {
                // Short tap: replay it so the game sees it.
                // This step counts as the first of at least two visible polls.
                chord_ = Chord::Replay;
                replayUntil_ = now + cfg.replayMs;
                replaySteps_ = 1;
                add = deferredBits_;
            } else if (now - t0_ >= cfg.deferMs) {
                chord_ = Chord::PassThrough;  // visible normally from now on
            } else {
                deferredBits_ |= held;
                hide = held;
            }
            break;
        case Chord::Replay:
            if (now >= replayUntil_ && replaySteps_ <= 0) {
                chord_ = Chord::Idle;  // tap shown long enough; normal from the next step
            } else {
                add = deferredBits_;
                if (replaySteps_ > 0) --replaySteps_;
            }
            break;
        case Chord::Held:
            hide = mask;
            if (!fired_ && now - t0_ >= cfg.holdMs) {
                fired_ = true;
                out.toggleFired = true;
            }
            if (held == 0) chord_ = Chord::Idle;
            break;
        case Chord::PassThrough:
            if (held == 0) chord_ = Chord::Idle;
            break;
    }

    PadRaw visible = raw;
    visible.buttons = static_cast<std::uint16_t>((raw.buttons & ~hide) | add);

    // --- Gate ---
    if (gate == PadGate::Block) {
        visible = PadRaw{};
    } else {
        if (prevGate_ == PadGate::Block) {
            // Buttons/triggers held while closing stay hidden until released.
            latchButtons_ = raw.buttons;
            latchLt_ = raw.lt > pad::TriggerThreshold;
            latchRt_ = raw.rt > pad::TriggerThreshold;
        }
        latchButtons_ &= raw.buttons;
        visible.buttons = static_cast<std::uint16_t>(visible.buttons & ~latchButtons_);
        if (latchLt_) {
            if (raw.lt <= pad::TriggerThreshold) latchLt_ = false;
            else visible.lt = 0;
        }
        if (latchRt_) {
            if (raw.rt <= pad::TriggerThreshold) latchRt_ = false;
            else visible.rt = 0;
        }
    }
    prevGate_ = gate;

    if (!lastConnected_ || !(visible == lastVisible_)) ++packet_;
    lastConnected_ = true;
    lastVisible_ = visible;
    out.visible = visible;
    out.packet = packet_;
    return out;
}

}  // namespace acp::input
