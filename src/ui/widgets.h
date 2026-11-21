#pragma once

#include "imgui.h"

namespace acp::ui {

// Uplay-style toggle switch. Navigable (A/Enter/click). Returns true when toggled.
// showText: show "On"/"Off" to its right.
bool ToggleSwitch(const char* id, bool* value, bool showText = true);

// Panel-style slider: thin track, filled part in accent colour, round grab; the value is not shown inside the
// slider (it sits next to it). Ctrl+click still opens exact input. Returns like ImGui::SliderScalar.
bool ValueSlider(const char* id, ImGuiDataType type, void* value, const void* min, const void* max, const char* format,
                 ImGuiSliderFlags flags);

// Combo with a slim chevron instead of an arrow button (otherwise like ImGui::BeginCombo; EndCombo only if true).
bool ComboBox(const char* id, const char* preview);

// Heading: in the Origins theme uses the serif font, uppercase and letter-spaced; otherwise the normal font.
// scale: size relative to the base font.
void HeadingText(const char* text, float scale, const ImVec4& color);
// Width HeadingText() will take for this text and scale.
float HeadingWidth(const char* text, float scale);

// Small filled circle at the current cursor position (no layout advance beyond SameLine).
void StatusDot(const ImVec4& color, float radius);

}  // namespace acp::ui
