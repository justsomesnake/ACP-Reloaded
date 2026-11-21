#pragma once

// Feeds a controller state into ImGui and returns panel actions (category, scrolling, value changes).
// Runs on the render thread under the ImGui context. Same path in overlay and sandbox.

#include "imgui.h"
#include "pad_filter.h"

namespace acp::ui {

struct PadActions {
    bool prevCategory = false;  // LB
    bool nextCategory = false;  // RB
    float scroll = 0.0f;        // right stick Y, -1..1 (positive = down)
    bool padUsed = false;       // pad input this frame
    // Console style: left/right changes the selected row's value (only when the panel asks for it).
    int adjust = 0;             // -1 / +1 per step (repeats while held)
    bool adjustFast = false;    // held longer: bigger steps
    bool fine = false;          // LT held
    bool coarse = false;        // RT held
};

class PadFeeder {
public:
    // Panel handles left/right itself (a value row has focus): don't pass it to ImGui navigation.
    void SetHorizontalCapture(bool on) { horizontal_ = on; }
    void Feed(ImGuiIO& io, bool connected, const input::PadRaw& raw, PadActions& out);
    void Release(ImGuiIO& io);  // release all pad buttons (on close)

private:
    input::PadRaw prev_{};
    bool prevConnected_ = false;
    bool horizontal_ = false;
    int holdDir_ = 0;           // held direction left/right
    double holdStart_ = 0.0;    // ImGui::GetTime()
    double nextRepeat_ = 0.0;
};

}  // namespace acp::ui
