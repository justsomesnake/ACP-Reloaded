#pragma once

#include <string>

#include "imgui.h"

namespace acp::ui {

enum class ThemeId : int { UplayClassic = 0, Origins = 1 };

// Colours of the active theme (for custom widgets and accents).
struct Palette {
    ImVec4 background, sidebar, frame, border;
    ImVec4 card;                           // group cards in the content area
    ImVec4 highlight;                      // fill of the focused row / selected category
    ImVec4 accent, accentDim, accentText;  // accentText: text on accent fill
    ImVec4 text, textDim;
    float toggleRounding;  // 0..1 relative to height (1 = pill)
    const char* title;
    const char* subtitle;
};

namespace colors {
inline constexpr ImVec4 Ok{0.30f, 0.80f, 0.45f, 1.00f};
inline constexpr ImVec4 Warn{0.98f, 0.66f, 0.18f, 1.00f};
inline constexpr ImVec4 Error{0.95f, 0.30f, 0.28f, 1.00f};
}  // namespace colors

const Palette& CurrentPalette();
ThemeId CurrentTheme();

// Applies theme, colours and (scaled) spacing to the current ImGui context.
// Font size and font scaling (FontSizeBase/FontScaleDpi) are left unchanged.
void ApplyTheme(ThemeId id, float scale);

// Loads the UI font from already-read TTF data (data must outlive the context).
// Without data: ImGui default font. Returns true if the TTF font is used.
bool LoadUiFont(const std::string* ttfData, float sizePixels);

// Heading font (Cinzel, embedded); nullptr if it could not be loaded.
ImFont* HeadingFont();

}  // namespace acp::ui
