#include "widgets.h"

#include <algorithm>

#include "imgui_internal.h"  // TempInputIsActive
#include "icons.h"
#include "theme.h"

namespace acp::ui {

bool ToggleSwitch(const char* id, bool* value, bool showText) {
    const Palette& p = CurrentPalette();
    const float h = ImGui::GetFrameHeight();
    const float w = h * 1.9f;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    // EnableNav: since ImGui 1.91 InvisibleButton is otherwise not a nav target (controller/keyboard would skip the switch)
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    if (pressed) *value = !*value;

    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float alpha = ImGui::GetStyle().Alpha;  // BeginDisabled() has already reduced alpha
    if (CurrentTheme() == ThemeId::Origins) {
        // On: teal with gold frame and gold grab; off: dark with a light line
        const ImVec2 b(pos.x + w, pos.y + h);
        dl->AddRectFilled(pos, b, ImGui::GetColorU32(*value ? p.highlight : (hovered ? p.highlight : p.frame)));
        dl->AddRect(pos, b, ImGui::GetColorU32(*value ? p.accent : p.border), 0.0f, 1.0f);
        const float pad = h * 0.26f, knob = h - pad * 2;
        const float kx = *value ? pos.x + w - pad - knob : pos.x + pad;
        const ImVec4 off(p.textDim.x, p.textDim.y, p.textDim.z, 0.75f);
        dl->AddRectFilled(ImVec2(kx, pos.y + pad), ImVec2(kx + knob, pos.y + pad + knob), ImGui::GetColorU32(*value ? p.accent : off));
        if (showText) {
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", *value ? "On" : "Off");
        }
        return pressed;
    }
    ImVec4 bg = *value ? p.accent : p.frame;
    if (hovered) bg = *value ? ImVec4(p.accent.x * 1.1f, p.accent.y * 1.1f, p.accent.z * 1.1f, 1.0f) : p.accentDim;
    bg.w *= alpha;
    const float r = h * 0.5f * p.toggleRounding;
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), ImGui::GetColorU32(bg), r);
    if (!*value) dl->AddRect(pos, ImVec2(pos.x + w, pos.y + h), ImGui::GetColorU32(ImVec4(p.border.x, p.border.y, p.border.z, alpha)), r);
    const float pad = 3.0f;
    const float knob = h - pad * 2;
    const float kx = *value ? pos.x + w - pad - knob : pos.x + pad;
    const ImVec4 knobColor = *value ? ImVec4(p.accentText.x, p.accentText.y, p.accentText.z, alpha)
                                    : ImVec4(p.text.x, p.text.y, p.text.z, alpha * 0.85f);
    dl->AddRectFilled(ImVec2(kx, pos.y + pad), ImVec2(kx + knob, pos.y + pad + knob), ImGui::GetColorU32(knobColor),
                      knob * 0.5f * p.toggleRounding);

    if (showText) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", *value ? "On" : "Off");
    }
    return pressed;
}

bool ValueSlider(const char* id, ImGuiDataType type, void* value, const void* min, const void* max, const char* format,
                 ImGuiSliderFlags flags) {
    const Palette& p = CurrentPalette();
    const bool textInput = ImGui::TempInputIsActive(ImGui::GetID(id));  // Ctrl+click: show a normal input field
    const float h = ImGui::GetFrameHeight();
    const float r = h * 0.24f;
    if (!textInput) {
        const ImVec4 none(0, 0, 0, 0);
        for (ImGuiCol c : {ImGuiCol_FrameBg, ImGuiCol_FrameBgHovered, ImGuiCol_FrameBgActive, ImGuiCol_SliderGrab,
                           ImGuiCol_SliderGrabActive, ImGuiCol_Text})
            ImGui::PushStyleColor(c, none);
    }
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, r * 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, textInput ? ImGui::GetStyle().FrameBorderSize : 0.0f);
    const bool changed = ImGui::SliderScalar(id, type, value, min, max, format, flags);
    ImGui::PopStyleVar(2);
    // Console style: release sliders activated by controller right away (left/right changes the value directly;
    // A must not open a hidden edit mode that has to be left with A/B).
    if (ImGui::IsItemActive() && GImGui->ActiveIdSource == ImGuiInputSource_Gamepad) ImGui::ClearActiveID();
    if (textInput) return changed;
    ImGui::PopStyleColor(6);

    double t = 0.0;
    if (type == ImGuiDataType_S32) {
        const int v = *static_cast<const int*>(value), lo = *static_cast<const int*>(min), hi = *static_cast<const int*>(max);
        t = hi > lo ? static_cast<double>(v - lo) / static_cast<double>(hi - lo) : 0.0;
    } else if (type == ImGuiDataType_Float) {
        const float v = *static_cast<const float*>(value), lo = *static_cast<const float*>(min), hi = *static_cast<const float*>(max);
        t = hi > lo ? static_cast<double>(v - lo) / static_cast<double>(hi - lo) : 0.0;
    }
    t = std::clamp(t, 0.0, 1.0);

    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
    const float pad = 2.0f;  // as in ImGui (grab inset from edge) so mouse and grab line up
    const float x0 = a.x + pad + r, x1 = b.x - pad - r;
    const float kx = x0 + static_cast<float>(t) * (x1 - x0);
    const float cy = (a.y + b.y) * 0.5f;
    const float th = std::max(2.0f, h * 0.09f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec4 track(p.text.x, p.text.y, p.text.z, 0.16f);
    dl->AddRectFilled(ImVec2(x0, cy - th * 0.5f), ImVec2(x1, cy + th * 0.5f), ImGui::GetColorU32(track), th * 0.5f);
    dl->AddRectFilled(ImVec2(x0, cy - th * 0.5f), ImVec2(kx, cy + th * 0.5f), ImGui::GetColorU32(p.accent), th * 0.5f);
    const float kr = hot ? r * 1.12f : r;
    if (CurrentTheme() == ThemeId::Origins) {
        // slim white grab, gold ring when focused (like focus in the game menu)
        dl->AddCircleFilled(ImVec2(kx, cy), kr * 0.82f, ImGui::GetColorU32(p.text), 24);
        if (hot) dl->AddCircle(ImVec2(kx, cy), kr * 1.25f, ImGui::GetColorU32(p.accent), 24, 1.5f);
    } else {
        dl->AddCircleFilled(ImVec2(kx, cy), kr, ImGui::GetColorU32(hot ? p.accent : p.text), 24);
    }
    return changed;
}
bool ComboBox(const char* id, const char* preview) {
    const Palette& p = CurrentPalette();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float w = ImGui::CalcItemWidth();
    const float h = ImGui::GetFrameHeight();
    ImDrawList* dl = ImGui::GetWindowDrawList();  // before BeginCombo: afterwards the popup may be current
    const bool open = ImGui::BeginCombo(id, preview, ImGuiComboFlags_NoArrowButton);
    DrawIcon(dl, Icon::ChevronDown, ImVec2(pos.x + w - h * 0.55f, pos.y + h * 0.5f), h * 0.5f,
             ImGui::GetColorU32(open ? p.accent : p.textDim));
    return open;
}
float HeadingWidth(const char* text, float scale) {
    const bool origins = CurrentTheme() == ThemeId::Origins && HeadingFont();
    const float size = ImGui::GetStyle().FontSizeBase * scale;
    if (!origins) {
        ImGui::PushFont(nullptr, size);
        const float w = ImGui::CalcTextSize(text).x;
        ImGui::PopFont();
        return w;
    }
    ImGui::PushFont(HeadingFont(), size * 1.05f);
    const float tracking = ImGui::GetFontSize() * 0.12f;
    float w = 0.0f;
    char ch[2] = {0, 0};
    for (const char* c = text; *c; ++c) {
        ch[0] = (*c >= 'a' && *c <= 'z') ? static_cast<char>(*c - 'a' + 'A') : *c;
        w += ImGui::CalcTextSize(ch).x + tracking;
    }
    ImGui::PopFont();
    return w > 0.0f ? w - tracking : 0.0f;
}

void HeadingText(const char* text, float scale, const ImVec4& color) {
    const bool origins = CurrentTheme() == ThemeId::Origins && HeadingFont();
    const float size = ImGui::GetStyle().FontSizeBase * scale;
    if (!origins) {
        ImGui::PushFont(nullptr, size);
        ImGui::TextColored(color, "%s", text);
        ImGui::PopFont();
        return;
    }
    // letter-spaced uppercase (drawn letter by letter)
    ImGui::PushFont(HeadingFont(), size * 1.05f);
    const float tracking = ImGui::GetFontSize() * 0.12f;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 col = ImGui::GetColorU32(color);
    float x = pos.x;
    char ch[2] = {0, 0};
    for (const char* c = text; *c; ++c) {
        ch[0] = (*c >= 'a' && *c <= 'z') ? static_cast<char>(*c - 'a' + 'A') : *c;
        dl->AddText(ImVec2(x, pos.y), col, ch);
        x += ImGui::CalcTextSize(ch).x + tracking;
    }
    const float h = ImGui::GetTextLineHeight();
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(x - pos.x - tracking, h));
}
void StatusDot(const ImVec4& color, float radius) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float y = p.y + ImGui::GetTextLineHeight() * 0.5f;
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + radius, y), radius, ImGui::GetColorU32(color));
    ImGui::Dummy(ImVec2(radius * 2.0f, ImGui::GetTextLineHeight()));
}

}  // namespace acp::ui
