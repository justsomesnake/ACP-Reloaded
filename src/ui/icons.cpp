#include "icons.h"

#include <cmath>

#include "theme.h"

namespace acp::ui {
namespace {

constexpr float kPi = 3.14159265f;

ImVec2 P(ImVec2 c, float s, float x, float y) { return ImVec2(c.x + x * s, c.y + y * s); }

// Arrowhead at tip, direction (dx, dy) normalised.
void ArrowHead(ImDrawList* dl, ImVec2 tip, float dx, float dy, float a, ImU32 col) {
    const ImVec2 back(tip.x - dx * a, tip.y - dy * a);
    const ImVec2 n(-dy * a * 0.62f, dx * a * 0.62f);
    dl->AddTriangleFilled(tip, ImVec2(back.x + n.x, back.y + n.y), ImVec2(back.x - n.x, back.y - n.y), col);
}

// Arc with an arrowhead at the end (angles in radians, screen coordinates: y down).
void ArcArrow(ImDrawList* dl, ImVec2 c, float r, float a0, float a1, float t, ImU32 col) {
    dl->PathArcTo(c, r, a0, a1, 24);
    dl->PathStroke(col, t);
    const ImVec2 tip(c.x + std::cos(a1) * r, c.y + std::sin(a1) * r);
    const float dir = a1 > a0 ? 1.0f : -1.0f;  // tangent in direction of travel
    const float dx = -std::sin(a1) * dir, dy = std::cos(a1) * dir;
    ArrowHead(dl, ImVec2(tip.x + dx * t * 1.6f, tip.y + dy * t * 1.6f), dx, dy, t * 3.2f, col);
}

void Person(ImDrawList* dl, ImVec2 c, float s, float t, ImU32 col) {
    dl->AddCircle(P(c, s, 0, -0.20f), s * 0.15f, col, 20, t);
    dl->PathArcTo(P(c, s, 0, 0.38f), s * 0.30f, kPi * 1.05f, kPi * 1.95f, 20);
    dl->PathStroke(col, t);
}

}  // namespace

void DrawIcon(ImDrawList* dl, Icon icon, ImVec2 c, float s, ImU32 col) {
    const float t = s * 0.085f < 1.3f ? 1.3f : s * 0.085f;
    switch (icon) {
        case Icon::Logo: {
            // diamond with a filled inner diamond
            const ImVec2 o[4] = {P(c, s, 0, -0.46f), P(c, s, 0.46f, 0), P(c, s, 0, 0.46f), P(c, s, -0.46f, 0)};
            dl->AddPolyline(o, 4, col, t, ImDrawFlags_Closed);
            dl->AddQuadFilled(P(c, s, 0, -0.2f), P(c, s, 0.2f, 0), P(c, s, 0, 0.2f), P(c, s, -0.2f, 0), col);
            break;
        }
        case Icon::HeroGeneral: Person(dl, c, s, t, col); break;
        case Icon::HeroFighting: {
            // diagonal sword: blade, crossguard, pommel
            dl->AddLine(P(c, s, -0.20f, 0.20f), P(c, s, 0.36f, -0.36f), col, t * 1.2f);
            dl->AddLine(P(c, s, -0.34f, 0.06f), P(c, s, -0.06f, 0.34f), col, t);
            dl->AddLine(P(c, s, -0.20f, 0.20f), P(c, s, -0.34f, 0.34f), col, t * 1.2f);
            dl->AddCircleFilled(P(c, s, -0.38f, 0.38f), t * 1.1f, col);
            break;
        }
        case Icon::NpcGeneral: {
            Person(dl, P(c, s, 0.16f, -0.04f), s * 0.80f, t, col);
            Person(dl, P(c, s, -0.18f, 0.04f), s * 0.92f, t, col);
            break;
        }
        case Icon::NpcFighting: {
            const ImVec2 sh[6] = {P(c, s, -0.32f, -0.34f), P(c, s, 0.32f, -0.34f), P(c, s, 0.32f, 0.02f),
                                  P(c, s, 0.0f, 0.40f),    P(c, s, -0.32f, 0.02f), P(c, s, -0.32f, -0.34f)};
            dl->AddPolyline(sh, 6, col, t, ImDrawFlags_Closed);
            dl->AddLine(P(c, s, 0, -0.22f), P(c, s, 0, 0.22f), col, t);
            break;
        }
        case Icon::World: {
            // pyramid with shaded edge and horizon
            const ImVec2 tri[3] = {P(c, s, 0, -0.36f), P(c, s, 0.42f, 0.30f), P(c, s, -0.42f, 0.30f)};
            dl->AddPolyline(tri, 3, col, t, ImDrawFlags_Closed);
            dl->AddTriangleFilled(P(c, s, 0, -0.36f), P(c, s, 0.42f, 0.30f), P(c, s, 0.10f, 0.30f), col);
            dl->AddLine(P(c, s, -0.48f, 0.40f), P(c, s, 0.48f, 0.40f), col, t);
            break;
        }        case Icon::Settings: {
            dl->AddCircle(c, s * 0.12f, col, 16, t);
            dl->AddCircle(c, s * 0.27f, col, 24, t);
            for (int i = 0; i < 8; ++i) {
                const float a = kPi * 0.25f * static_cast<float>(i) + kPi / 8.0f;
                const float ca = std::cos(a), sa = std::sin(a);
                dl->AddLine(P(c, s, ca * 0.27f, sa * 0.27f), P(c, s, ca * 0.42f, sa * 0.42f), col, t * 1.6f);
            }
            break;
        }
        case Icon::Reset: ArcArrow(dl, c, s * 0.30f, kPi * 0.25f, kPi * -1.25f, t, col); break;
        case Icon::Reload: {
            // globe: circle, equator, meridian ellipse
            dl->AddCircle(c, s * 0.38f, col, 28, t);
            dl->AddLine(P(c, s, -0.38f, 0), P(c, s, 0.38f, 0), col, t * 0.8f);
            dl->AddEllipse(c, ImVec2(s * 0.16f, s * 0.38f), col, 0.0f, 24, t * 0.8f);
            break;
        }
        case Icon::ChevronDown: {
            const ImVec2 v[3] = {P(c, s, -0.30f, -0.12f), P(c, s, 0, 0.18f), P(c, s, 0.30f, -0.12f)};
            dl->AddPolyline(v, 3, col, t, ImDrawFlags_None);
            break;
        }
        case Icon::Undo: {
            dl->PathArcTo(P(c, s, 0.04f, 0.04f), s * 0.28f, kPi * -0.5f, kPi * 0.5f, 16);
            dl->PathLineTo(P(c, s, -0.22f, 0.32f));
            dl->PathStroke(col, t);
            dl->AddLine(P(c, s, 0.04f, -0.24f), P(c, s, -0.22f, -0.24f), col, t);
            ArrowHead(dl, P(c, s, -0.38f, -0.24f), -1.0f, 0.0f, t * 3.0f, col);
            break;
        }
        case Icon::Warning: {
            const ImVec2 tri[3] = {P(c, s, 0, -0.40f), P(c, s, 0.44f, 0.36f), P(c, s, -0.44f, 0.36f)};
            dl->AddPolyline(tri, 3, col, t, ImDrawFlags_Closed);
            dl->AddLine(P(c, s, 0, -0.14f), P(c, s, 0, 0.12f), col, t * 1.2f);
            dl->AddCircleFilled(P(c, s, 0, 0.24f), t * 0.85f, col);
            break;
        }
        case Icon::Save: {
            // tray with downward arrow
            const ImVec2 tray[4] = {P(c, s, -0.36f, 0.06f), P(c, s, -0.36f, 0.36f), P(c, s, 0.36f, 0.36f), P(c, s, 0.36f, 0.06f)};
            dl->AddPolyline(tray, 4, col, t, ImDrawFlags_None);
            dl->AddLine(P(c, s, 0, -0.38f), P(c, s, 0, 0.06f), col, t);
            ArrowHead(dl, P(c, s, 0, 0.20f), 0.0f, 1.0f, t * 3.0f, col);
            break;
        }
        case Icon::Trash: {
            dl->AddLine(P(c, s, -0.36f, -0.24f), P(c, s, 0.36f, -0.24f), col, t);
            dl->AddLine(P(c, s, -0.10f, -0.36f), P(c, s, 0.10f, -0.36f), col, t);
            const ImVec2 bin[4] = {P(c, s, -0.26f, -0.24f), P(c, s, -0.20f, 0.38f), P(c, s, 0.20f, 0.38f), P(c, s, 0.26f, -0.24f)};
            dl->AddPolyline(bin, 4, col, t, ImDrawFlags_None);
            dl->AddLine(P(c, s, -0.07f, -0.10f), P(c, s, -0.06f, 0.24f), col, t * 0.8f);
            dl->AddLine(P(c, s, 0.07f, -0.10f), P(c, s, 0.06f, 0.24f), col, t * 0.8f);
            break;
        }
        case Icon::Search: {
            dl->AddCircle(P(c, s, -0.07f, -0.07f), s * 0.24f, col, 20, t);
            dl->AddLine(P(c, s, 0.11f, 0.11f), P(c, s, 0.36f, 0.36f), col, t * 1.4f);
            break;
        }
    }
}

bool IconButton(const char* id, Icon icon, const char* tooltip) {
    const Palette& p = CurrentPalette();
    const float h = ImGui::GetFrameHeight();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(p.accent.x, p.accent.y, p.accent.z, 0.16f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(p.accent.x, p.accent.y, p.accent.z, 0.30f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    const bool pressed = ImGui::Button(id, ImVec2(h, h));
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    DrawIcon(ImGui::GetWindowDrawList(), icon, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), h * 0.62f,
             ImGui::GetColorU32(hovered ? p.accent : p.text));
    if (tooltip && hovered) ImGui::SetTooltip("%s", tooltip);
    return pressed;
}

void InlineIcon(Icon icon, const ImVec4& color, bool frameLine) {
    const float h = ImGui::GetTextLineHeight();
    const float lineH = frameLine ? ImGui::GetFrameHeight() : h;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    DrawIcon(ImGui::GetWindowDrawList(), icon, ImVec2(pos.x + h * 0.5f, pos.y + lineH * 0.5f), h * 0.92f,
             ImGui::GetColorU32(color));
    ImGui::Dummy(ImVec2(h, lineH));
}

}  // namespace acp::ui
