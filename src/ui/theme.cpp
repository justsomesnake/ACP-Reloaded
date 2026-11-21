#include "theme.h"

namespace acp::ui {
namespace {

#include "font_cinzel.inc"  // kCinzelTtf (SIL Open Font License 1.1, third_party/fonts/Cinzel_OFL.txt)

ImFont* g_heading = nullptr;

// Uplay look: charcoal, Animus blue, soft rounding.
constexpr Palette kUplay{
    {0.08f, 0.09f, 0.11f, 0.97f},  // background
    {0.05f, 0.06f, 0.08f, 0.98f},  // sidebar
    {0.13f, 0.15f, 0.19f, 1.00f},  // frame
    {0.18f, 0.22f, 0.28f, 0.80f},  // border
    {0.105f, 0.118f, 0.145f, 1.00f},  // card
    {0.00f, 0.62f, 0.98f, 0.13f},     // highlight
    {0.00f, 0.62f, 0.98f, 1.00f},  // accent
    {0.00f, 0.42f, 0.68f, 1.00f},  // accentDim
    {1.00f, 1.00f, 1.00f, 1.00f},  // accentText
    {0.86f, 0.89f, 0.93f, 1.00f},  // text
    {0.52f, 0.56f, 0.62f, 1.00f},  // textDim
    1.0f,
    "ANIMUS CONTROL PANEL",
    "Reloaded",
};

// Origins look based on the game's main menu: dark slate teal, thin light borders, white sans-serif text,
// gold only for focus, selection and fill levels (like the focused button in the game menu: gold frame + teal).
constexpr Palette kOrigins{
    {0.040f, 0.068f, 0.074f, 0.95f},  // background
    {0.030f, 0.056f, 0.061f, 0.70f},  // sidebar (slightly translucent, same tone)
    {0.070f, 0.110f, 0.116f, 1.00f},  // frame
    {0.86f, 0.88f, 0.86f, 0.20f},     // border (thin light line)
    {0.050f, 0.088f, 0.094f, 0.80f},  // card
    {0.16f, 0.37f, 0.36f, 0.55f},     // highlight (teal)
    {0.88f, 0.72f, 0.42f, 1.00f},     // accent (gold)
    {0.52f, 0.43f, 0.26f, 1.00f},     // accentDim
    {0.05f, 0.07f, 0.07f, 1.00f},     // accentText
    {0.95f, 0.94f, 0.91f, 1.00f},     // text
    {0.60f, 0.65f, 0.63f, 1.00f},     // textDim
    0.0f,
    "ANIMUS",
    "Control Panel",
};
ThemeId g_theme = ThemeId::UplayClassic;

ImVec4 Mix(const ImVec4& a, const ImVec4& b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}
ImVec4 WithAlpha(ImVec4 c, float a) {
    c.w = a;
    return c;
}

}  // namespace

const Palette& CurrentPalette() { return g_theme == ThemeId::Origins ? kOrigins : kUplay; }
ThemeId CurrentTheme() { return g_theme; }

void ApplyTheme(ThemeId id, float scale) {
    g_theme = id;
    const Palette& p = CurrentPalette();
    ImGuiStyle& s = ImGui::GetStyle();
    // Keep font values (owned by the host: DPI/resolution scaling).
    const float fontBase = s.FontSizeBase, fontDpi = s.FontScaleDpi, fontMain = s.FontScaleMain;
    s = ImGuiStyle();
    ImGui::StyleColorsDark(&s);
    s.FontSizeBase = fontBase;
    s.FontScaleDpi = fontDpi;
    s.FontScaleMain = fontMain;

    const bool origins = id == ThemeId::Origins;
    s.WindowRounding = origins ? 0.0f : 4.0f;
    s.ChildRounding = origins ? 0.0f : 3.0f;
    s.FrameRounding = origins ? 1.0f : 3.0f;
    s.GrabRounding = origins ? 0.0f : 3.0f;
    s.PopupRounding = origins ? 0.0f : 3.0f;
    s.ScrollbarRounding = origins ? 0.0f : 3.0f;
    s.TabRounding = origins ? 0.0f : 3.0f;
    s.WindowBorderSize = 1.0f;
    s.ChildBorderSize = 0.0f;  // borders only where wanted (cards in the Origins theme)
    s.FrameBorderSize = origins ? 1.0f : 0.0f;
    s.WindowPadding = origins ? ImVec2(18, 16) : ImVec2(14, 12);
    s.FramePadding = origins ? ImVec2(10, 7) : ImVec2(10, 6);
    s.ItemSpacing = origins ? ImVec2(12, 9) : ImVec2(10, 8);
    s.ItemInnerSpacing = ImVec2(8, 6);
    s.CellPadding = origins ? ImVec2(10, 6) : ImVec2(8, 5);
    s.ScrollbarSize = origins ? 10.0f : 14.0f;
    s.GrabMinSize = origins ? 6.0f : 14.0f;
    s.SeparatorTextBorderSize = origins ? 1.0f : 2.0f;
    s.SeparatorTextPadding = ImVec2(0, 4);
    s.DisabledAlpha = 0.38f;
    s.ScaleAllSizes(scale);

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = p.text;
    c[ImGuiCol_TextDisabled] = p.textDim;
    c[ImGuiCol_WindowBg] = p.background;
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = WithAlpha(p.sidebar, 0.98f);
    c[ImGuiCol_Border] = p.border;
    c[ImGuiCol_FrameBg] = p.frame;
    c[ImGuiCol_FrameBgHovered] = Mix(p.frame, p.accentDim, 0.30f);
    c[ImGuiCol_FrameBgActive] = Mix(p.frame, p.accentDim, 0.45f);
    c[ImGuiCol_TitleBg] = p.sidebar;
    c[ImGuiCol_TitleBgActive] = p.sidebar;
    c[ImGuiCol_TitleBgCollapsed] = p.sidebar;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0.2f);
    c[ImGuiCol_ScrollbarGrab] = Mix(p.frame, p.text, 0.15f);
    c[ImGuiCol_ScrollbarGrabHovered] = p.accentDim;
    c[ImGuiCol_ScrollbarGrabActive] = p.accent;
    c[ImGuiCol_CheckMark] = p.accent;
    c[ImGuiCol_SliderGrab] = p.accent;
    c[ImGuiCol_SliderGrabActive] = Mix(p.accent, ImVec4(1, 1, 1, 1), 0.3f);
    c[ImGuiCol_Button] = Mix(p.frame, p.accentDim, 0.15f);
    c[ImGuiCol_ButtonHovered] = p.accentDim;
    c[ImGuiCol_ButtonActive] = p.accent;
    c[ImGuiCol_Header] = WithAlpha(p.accentDim, 0.55f);
    c[ImGuiCol_HeaderHovered] = WithAlpha(p.accentDim, 0.75f);
    c[ImGuiCol_HeaderActive] = p.accent;
    c[ImGuiCol_Separator] = p.border;
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, origins ? 0.018f : 0.025f);
    c[ImGuiCol_TableBorderLight] = WithAlpha(p.border, 0.5f);
    c[ImGuiCol_NavCursor] = p.accent;
    c[ImGuiCol_TextSelectedBg] = WithAlpha(p.accent, 0.35f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.55f);
    if (origins) {
        // Like the game menu buttons: semi-transparent with a light line, focus/hover in teal
        c[ImGuiCol_Button] = ImVec4(p.frame.x, p.frame.y, p.frame.z, 0.55f);
        c[ImGuiCol_ButtonHovered] = p.highlight;
        c[ImGuiCol_ButtonActive] = WithAlpha(p.highlight, 0.85f);
        c[ImGuiCol_FrameBg] = ImVec4(p.frame.x, p.frame.y, p.frame.z, 0.70f);
        c[ImGuiCol_FrameBgHovered] = p.highlight;
        c[ImGuiCol_FrameBgActive] = WithAlpha(p.highlight, 0.85f);
        c[ImGuiCol_Header] = p.highlight;
        c[ImGuiCol_HeaderHovered] = WithAlpha(p.highlight, 0.75f);
        c[ImGuiCol_HeaderActive] = WithAlpha(p.highlight, 0.95f);
        c[ImGuiCol_PopupBg] = ImVec4(0.035f, 0.060f, 0.066f, 0.98f);
        c[ImGuiCol_Separator] = p.border;
    }
}

ImFont* HeadingFont() { return g_heading; }

namespace {
void AddHeadingFont(float sizePixels) {
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;  // static data
    g_heading = ImGui::GetIO().Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(kCinzelTtf),
                                                            static_cast<int>(sizeof(kCinzelTtf)), sizePixels, &cfg);
}
}  // namespace

bool LoadUiFont(const std::string* ttfData, float sizePixels) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    g_heading = nullptr;
    if (ttfData && !ttfData->empty()) {
        ImFontConfig cfg;
        cfg.FontDataOwnedByAtlas = false;  // data owned by the caller
        if (io.Fonts->AddFontFromMemoryTTF(const_cast<char*>(ttfData->data()), static_cast<int>(ttfData->size()),
                                           sizePixels, &cfg)) {
            ImGui::GetStyle().FontSizeBase = sizePixels;
            AddHeadingFont(sizePixels);
            return true;
        }
    }
    ImFontConfig cfg;
    cfg.SizePixels = sizePixels;
    io.Fonts->AddFontDefault(&cfg);
    ImGui::GetStyle().FontSizeBase = sizePixels;
    AddHeadingFont(sizePixels);
    return false;
}

}  // namespace acp::ui
