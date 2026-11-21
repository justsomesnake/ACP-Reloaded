#include "pad_feed.h"

#include <cmath>

namespace acp::ui {
namespace {

constexpr float kStickDeadzone = 7849.0f;  // XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE

float Axis(std::int16_t v) {
    const float f = static_cast<float>(v);
    if (std::fabs(f) < kStickDeadzone) return 0.0f;
    const float sign = f < 0 ? -1.0f : 1.0f;
    float n = (std::fabs(f) - kStickDeadzone) / (32767.0f - kStickDeadzone);
    if (n > 1.0f) n = 1.0f;
    return sign * n;
}

void Digital(ImGuiIO& io, ImGuiKey key, bool down) { io.AddKeyEvent(key, down); }

void Analog(ImGuiIO& io, ImGuiKey key, float v) { io.AddKeyAnalogEvent(key, v > 0.10f, v > 0.0f ? v : 0.0f); }

}  // namespace

void PadFeeder::Feed(ImGuiIO& io, bool connected, const input::PadRaw& raw, PadActions& out) {
    using namespace input::pad;
    out = {};
    if (connected) io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    else io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;

    const input::PadRaw p = connected ? raw : input::PadRaw{};
    const std::uint16_t b = p.buttons;

    // Left stick acts like the D-pad (digital, same repeat): even navigation instead of ImGui's analog
    // stick navigation. The dominant axis wins so diagonal input doesn't jump.
    const float lx = Axis(p.lx), ly = Axis(p.ly);
    const bool vertical = std::fabs(ly) >= std::fabs(lx);
    const bool up = (b & DpadUp) || (vertical && ly > 0.45f), down = (b & DpadDown) || (vertical && ly < -0.45f);
    const bool left = (b & DpadLeft) || (!vertical && lx < -0.45f), right = (b & DpadRight) || (!vertical && lx > 0.45f);

    Digital(io, ImGuiKey_GamepadDpadUp, up);
    Digital(io, ImGuiKey_GamepadDpadDown, down);
    // Left/right: either ImGui navigation or a value change by the panel (console style)
    Digital(io, ImGuiKey_GamepadDpadLeft, !horizontal_ && left);
    Digital(io, ImGuiKey_GamepadDpadRight, !horizontal_ && right);
    Digital(io, ImGuiKey_GamepadFaceDown, b & A);   // activate / confirm
    Digital(io, ImGuiKey_GamepadFaceRight, b & B);  // cancel / back
    Digital(io, ImGuiKey_GamepadFaceLeft, b & X);   // reset value
    Digital(io, ImGuiKey_GamepadFaceUp, b & Y);     // undo
    // LT/RT = finer/coarser (ImGui uses L1/R1 for this); LB/RB stay reserved for categories.
    Digital(io, ImGuiKey_GamepadL1, p.lt > TriggerThreshold);
    Digital(io, ImGuiKey_GamepadR1, p.rt > TriggerThreshold);

    // Don't also feed the stick to ImGui as analog (would cause doubled, uneven movement)
    Analog(io, ImGuiKey_GamepadLStickLeft, 0.0f);
    Analog(io, ImGuiKey_GamepadLStickRight, 0.0f);
    Analog(io, ImGuiKey_GamepadLStickUp, 0.0f);
    Analog(io, ImGuiKey_GamepadLStickDown, 0.0f);

    // Value change with repeat: first step immediately, after 350 ms every 110 ms, after 1.2 s every 50 ms (bigger).
    const int dir = left == right ? 0 : (right ? 1 : -1);
    const double now = ImGui::GetTime();
    if (dir == 0 || !horizontal_) {
        holdDir_ = 0;
    } else if (dir != holdDir_) {
        holdDir_ = dir;
        holdStart_ = now;
        nextRepeat_ = now + 0.35;
        out.adjust = dir;
    } else if (now >= nextRepeat_) {
        const bool fast = now - holdStart_ > 1.2;
        nextRepeat_ = now + (fast ? 0.05 : 0.11);
        out.adjust = dir;
        out.adjustFast = fast;
    }
    out.fine = p.lt > TriggerThreshold;
    out.coarse = p.rt > TriggerThreshold;

    const bool lb = b & LeftShoulder, rb = b & RightShoulder;
    const bool prevLb = prev_.buttons & LeftShoulder, prevRb = prev_.buttons & RightShoulder;
    out.prevCategory = lb && !prevLb;
    out.nextCategory = rb && !prevRb;
    out.scroll = -Axis(p.ry);
    out.padUsed = connected && (b != 0 || lx != 0.0f || ly != 0.0f || out.scroll != 0.0f || p.lt > TriggerThreshold ||
                                p.rt > TriggerThreshold);

    prev_ = p;
    prevConnected_ = connected;
}

void PadFeeder::Release(ImGuiIO& io) {
    PadActions ignored;
    horizontal_ = false;
    holdDir_ = 0;
    Feed(io, prevConnected_, input::PadRaw{}, ignored);
}

}  // namespace acp::ui
