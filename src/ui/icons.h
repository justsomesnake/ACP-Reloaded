#pragma once

// Panel vector icons: lines, circles and polygons only (no glyphs, no image files).

#include "imgui.h"

namespace acp::ui {

enum class Icon {
    Logo,          // Animus diamond
    HeroGeneral,   // person
    HeroFighting,  // sword
    NpcGeneral,    // two people
    NpcFighting,   // shield
    World,         // pyramid
    Settings,      // gear
    Reset,         // counter-clockwise arrow
    Reload,        // reload world (globe)
    ChevronDown,   // dropdown
    Warning,       // triangle with exclamation mark
    Save,          // save (arrow into tray)
    Trash,         // delete
    Search,        // magnifier
    Undo,          // undo
};

// Draws an icon centred on center inside a square of side length size.
void DrawIcon(ImDrawList* dl, Icon icon, ImVec2 center, float size, ImU32 color);

// Square icon button (height of a control row). Navigable, tooltip on hover.
bool IconButton(const char* id, Icon icon, const char* tooltip);

// Inline icon (height of a text line), e.g. before a label.
// frameLine: the line uses AlignTextToFramePadding (align the icon with the text).
void InlineIcon(Icon icon, const ImVec4& color, bool frameLine = false);

}  // namespace acp::ui
