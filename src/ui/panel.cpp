#include "panel.h"

#include <windows.h>  // GetTickCount64

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>

#include "icons.h"
#include "imgui.h"
#include "widgets.h"

namespace acp::ui {
namespace {

constexpr const char* kCategoryNames[] = {"Presets",      "Hero General", "Hero Fighting", "NPC General",
                                          "NPC Fighting", "World",        "Settings"};
constexpr int kCategoryCount = 7;
constexpr int kSettingsPage = 6;

Category ToCategory(int idx) { return static_cast<Category>(idx - 1); }

// Display order of the groups (names from docs/param_map.csv via the generator).
constexpr std::string_view kGroupOrder[] = {
    "Vitality", "Movement", "Water & Heat", "Progression", "Equipment", "Character & Companions",
    "Damage", "Assassination", "Adrenaline", "Desynchronization",
    "Level Scaling", "Perception", "Combat", "Relations to Player", "Faction Relations",
    "Time of Day", "Sandstorm", "World Map", "Vehicles & Siege", "Environment & Loot",
    "Game Look (Post-Processing)", "Custom Filter (ACP Reloaded)"};
constexpr std::string_view kGroupTime = "Time of Day";
constexpr std::string_view kGroupSandstorm = "Sandstorm";
constexpr std::string_view kGroupCustomFx = "Custom Filter (ACP Reloaded)";
constexpr std::string_view kGroupProgression = "Progression";

int GroupRank(std::string_view g) {
    for (int i = 0; i < static_cast<int>(std::size(kGroupOrder)); ++i)
        if (kGroupOrder[i] == g) return i;
    return 1000;
}

float Em() { return ImGui::GetFontSize(); }

const char* SliderFormat(const ParamDesc& d) {
    if (d.type == ParamType::Float) return d.unit == Unit::Seconds ? "%.2f s" : "%.2f";
    switch (d.unit) {
        case Unit::Percent: return "%d %%";
        case Unit::Seconds: return "%d s";
        case Unit::Dps: return "%d DPS";
        case Unit::Level: return d.uiMin.i < 0 ? "%+d" : "%d";
        default: return "%d";
    }
}

std::string EnumLabel(const ParamDesc& d, int n) {
    const std::string_view key = d.key;
    if (key.size() > 9 && key.substr(key.size() - 9) == "Relations") {
        static constexpr const char* kRel[] = {"Default", "Ally", "Neutral", "Hostile"};
        return n >= 0 && n <= 3 ? kRel[n] : std::to_string(n);
    }
    if (d.id == ParamId::PostProcessFilterSettings) return "Look " + std::to_string(n) + (n == d.def.i ? " (default)" : "");
    if (d.id == ParamId::PlayableCharacterIndex) {
        // Order from the ACP DLC game data ("ACP Playable Characters", 21 entries; analysis round 9).
        // 20 = no ACP outfit, i.e. Bayek. 10 (female assassin): name from the old panel; 18 as in the game data ("Roman General", old panel probably "Roman Scout")
        // by elimination; the in-game images match.
        static constexpr const char* kChars[] = {
            "Bandit Warrior", "Theodoros", "Aya",         "Duelist", "Abstergo Trooper", "Hepzefa",      "Hotephres",
            "Greek Soldier",  "Jeska",     "Layla",       "Kawit",   "Khaliset",         "Rai",          "Roman Soldier",
            "Kensa",          "Bandit Archer", "Tahira",  "Nubian Soldier", "Roman General", "Son of Ra", "Bayek"};
        if (n >= 0 && n < static_cast<int>(std::size(kChars)))
            return std::string(kChars[n]) + (n == d.def.i ? " (default)" : "");
        return "Character " + std::to_string(n);
    }
    return std::to_string(n) + (n == d.def.i ? " (default)" : "");
}

// Display name: on hero/NPC pages without the prefix the page already implies ("Player ...", "NPC ...").
std::string_view DisplayLabel(const ParamDesc& d) {
    std::string_view l = d.label;
    const bool hero = d.category == Category::HeroGeneral || d.category == Category::HeroFighting;
    const bool npc = d.category == Category::NpcGeneral || d.category == Category::NpcFighting;
    for (std::string_view pre : {std::string_view("Player "), std::string_view("NPC "), std::string_view("NPCs ")}) {
        if (((hero && pre == "Player ") || (npc && pre != "Player ")) && l.size() > pre.size() + 3 && l.substr(0, pre.size()) == pre) {
            l.remove_prefix(pre.size());
            break;
        }
    }
    return l;
}

std::string ValueText(const ParamDesc& d, const ParamValue& v) {
    char buf[48];
    switch (d.type) {
        case ParamType::Bool: return v.b ? "On" : "Off";
        case ParamType::Int:
            if (d.unit == Unit::Enum) return EnumLabel(d, v.i);
            std::snprintf(buf, sizeof(buf), SliderFormat(d), v.i);
            return buf;
        case ParamType::Float: std::snprintf(buf, sizeof(buf), SliderFormat(d), v.f); return buf;
    }
    return {};
}

bool ContainsNoCase(std::string_view hay, std::string_view needle) {
    if (needle.empty()) return true;
    auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        std::size_t j = 0;
        while (j < needle.size() && lower(hay[i + j]) == lower(needle[j])) ++j;
        if (j == needle.size()) return true;
    }
    return false;
}

// Value from a number in the matching type, clamped to the range.
ParamValue MakeValue(const ParamDesc& d, double x) {
    switch (d.type) {
        case ParamType::Bool: return ParamValue::Bool(x != 0.0);
        case ParamType::Int:
            return ParamValue::Int(std::clamp(static_cast<int>(std::lround(x)), d.uiMin.i, d.uiMax.i));
        case ParamType::Float: return ParamValue::Float(std::clamp(static_cast<float>(x), d.uiMin.f, d.uiMax.f));
    }
    return d.def;
}

struct StatusView {
    const char* text;
    ImVec4 color;
};

StatusView ViewOf(SyncStatus s) {
    const Palette& p = CurrentPalette();
    switch (s) {
        case SyncStatus::Starting: return {"Starting...", p.textDim};
        case SyncStatus::NotFound: return {"ACP.ini not found", colors::Error};
        case SyncStatus::Ok: return {"Synced", colors::Ok};
        case SyncStatus::Pending: return {"Saving...", p.accent};
        case SyncStatus::Locked: return {"File locked - retrying", colors::Warn};
        case SyncStatus::ReadOnly: return {"ACP.ini is read-only", colors::Warn};
        case SyncStatus::Error: return {"Error", colors::Error};
    }
    return {"?", p.textDim};
}

void SectionTitle(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, CurrentPalette().accent);
    ImGui::SeparatorText(text);
    ImGui::PopStyleColor();
}

// Parameters of a group in file order, dependent values directly below their toggle.
std::vector<const ParamDesc*> OrderedGroup(Category cat, std::string_view group) {
    std::vector<const ParamDesc*> items;
    for (const ParamDesc& d : ParamTable())
        if (d.category == cat && d.group == group && !d.readOnly) items.push_back(&d);
    auto controllerInGroup = [&](const ParamDesc& d) -> const ParamDesc* {
        for (const auto& w : d.disabledWhen) {
            if (!w.Active()) continue;
            for (const ParamDesc* o : items)
                if (o->id == w.param) return o;
        }
        return nullptr;
    };
    std::vector<const ParamDesc*> out;
    std::vector<bool> done(items.size(), false);
    auto emit = [&](auto&& self, const ParamDesc* d) -> void {
        out.push_back(d);
        for (std::size_t j = 0; j < items.size(); ++j) {
            if (!done[j] && controllerInGroup(*items[j]) == d) {
                done[j] = true;
                self(self, items[j]);
            }
        }
    };
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (done[i] || controllerInGroup(*items[i])) continue;
        done[i] = true;
        emit(emit, items[i]);
    }
    for (std::size_t i = 0; i < items.size(); ++i)
        if (!done[i]) out.push_back(items[i]);
    return out;
}

std::vector<std::string_view> GroupsOf(Category cat) {
    std::vector<std::string_view> groups;
    for (const ParamDesc& d : ParamTable()) {
        if (d.category == cat && !d.readOnly && std::find(groups.begin(), groups.end(), d.group) == groups.end())
            groups.push_back(d.group);
    }
    std::stable_sort(groups.begin(), groups.end(),
                     [](std::string_view a, std::string_view b) { return GroupRank(a) < GroupRank(b); });
    return groups;
}

void LabelCell(const char* text, const char* tooltip) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(text);
    if (tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
}

// Origins background like the game menu: soft light in the centre, fine golden Animus geometry, slowly
// rising gold particles. All very subtle (low opacity), drawn behind everything else.
void DrawOriginsBackdrop(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    const Palette& p = CurrentPalette();
    const float w = b.x - a.x, h = b.y - a.y;
    dl->PushClipRect(a, b, true);
    // glow
    const ImVec2 glow(a.x + w * 0.62f, a.y + h * 0.45f);
    for (int i = 0; i < 6; ++i)
        dl->AddCircleFilled(glow, h * (0.75f - 0.1f * static_cast<float>(i)), ImGui::GetColorU32(ImVec4(0.20f, 0.42f, 0.42f, 0.022f)), 64);
    // geometry: concentric circles, diagonals and a large triangle
    const ImU32 line = ImGui::GetColorU32(ImVec4(p.accent.x, p.accent.y, p.accent.z, 0.06f));
    for (float r : {0.30f, 0.46f, 0.62f}) dl->AddCircle(glow, h * r, line, 96, 1.0f);
    dl->AddLine(ImVec2(glow.x - h * 0.9f, glow.y - h * 0.9f), ImVec2(glow.x + h * 0.9f, glow.y + h * 0.9f), line, 1.0f);
    dl->AddLine(ImVec2(glow.x - h * 0.9f, glow.y + h * 0.9f), ImVec2(glow.x + h * 0.9f, glow.y - h * 0.9f), line, 1.0f);
    const ImVec2 tri[3] = {ImVec2(glow.x, glow.y - h * 0.62f), ImVec2(glow.x + h * 0.54f, glow.y + h * 0.31f),
                           ImVec2(glow.x - h * 0.54f, glow.y + h * 0.31f)};
    dl->AddPolyline(tri, 3, line, 1.0f, ImDrawFlags_Closed);
    // particles (deterministic placement, rise slowly, flicker slightly)
    const float t = static_cast<float>(ImGui::GetTime());
    for (int i = 0; i < 42; ++i) {
        const auto hash = [](std::uint32_t x) {
            x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
            return static_cast<float>(x & 0xFFFF) / 65535.0f;
        };
        const float px = hash(static_cast<std::uint32_t>(i) * 3u + 1u), py = hash(static_cast<std::uint32_t>(i) * 3u + 2u);
        const float sp = 0.006f + 0.012f * hash(static_cast<std::uint32_t>(i) * 3u + 3u);
        float y = py - t * sp;
        y -= std::floor(y);
        const float alpha = 0.25f + 0.35f * (0.5f + 0.5f * std::sin(t * (0.8f + px) + static_cast<float>(i)));
        dl->AddCircleFilled(ImVec2(a.x + px * w, a.y + y * h), 1.0f + 0.8f * px,
                            ImGui::GetColorU32(ImVec4(p.accent.x, p.accent.y, p.accent.z, alpha)), 8);
    }
    dl->PopClipRect();
}

// Toggle with label on the right (settings page, outside tables).
bool LabeledToggle(const char* label, bool* v) {
    ImGui::PushID(label);
    const bool changed = ToggleSwitch("##t", v, false);
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::PopID();
    return changed;
}

// --- Console-style controller: left/right changes the focused row's value ---
PadActions g_pad;          // pad actions of the current frame (render thread)
bool g_rowFocused = false; // a value row had focus this frame

// Step size: about 40 steps across the range, on "round" values; LT = fine, RT/hold = coarse.
double PadStep(double range, bool integer) {
    double s = range / 40.0;
    if (integer) {
        for (int n : {1, 2, 5, 10, 25, 50, 100, 250, 500, 1000})
            if (n >= s) {
                s = n;
                break;
            }
    }
    if (g_pad.fine) s = integer ? 1.0 : range / 400.0;
    else if (g_pad.coarse || g_pad.adjustFast) s *= 5.0;
    return s;
}
// Next grid value in direction dir (stays on the step grid).
double PadStepValue(double v, double lo, double hi, int dir, bool integer) {
    const double s = PadStep(hi - lo, integer);
    const double k = v / s;
    double n = dir > 0 ? (std::floor(k + 1e-6) + 1.0) * s : (std::ceil(k - 1e-6) - 1.0) * s;
    if (integer) n = std::round(n);
    return std::clamp(n, lo, hi);
}
// Highlight the focused row (whole table row) and report it as a value row.
bool PadRowFocus() {
    if (!ImGui::IsItemFocused() || ImGui::IsItemActive()) return false;
    g_rowFocused = true;
    const Palette& p = CurrentPalette();
    if (ImGui::TableGetColumnCount() == 0) {
        // Outside a table (settings page): highlight only the item
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        const float pad = ImGui::GetStyle().FramePadding.y * 0.6f;
        ImGui::GetWindowDrawList()->AddRect(ImVec2(a.x - pad, a.y - pad), ImVec2(b.x + pad, b.y + pad), ImGui::GetColorU32(p.accent),
                                            0.0f, 1.5f);
        return true;
    }
    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(p.highlight));
    if (CurrentTheme() == ThemeId::Origins) {
        // Like the focused button in the game menu: gold frame around the whole row
        const ImVec2 wp = ImGui::GetWindowPos();
        const float padX = ImGui::GetStyle().WindowPadding.x * 0.45f, padY = ImGui::GetStyle().CellPadding.y;
        ImGui::GetWindowDrawList()->AddRect(ImVec2(wp.x + padX, ImGui::GetItemRectMin().y - padY),
                                            ImVec2(wp.x + ImGui::GetWindowWidth() - padX, ImGui::GetItemRectMax().y + padY),
                                            ImGui::GetColorU32(p.accent), 0.0f, 1.5f);
    }
    return true;
}
// Slider via pad: focused + left/right = one step. Returns changed (save afterwards).
bool PadAdjustInt(int* v, int lo, int hi) {
    if (!PadRowFocus() || g_pad.adjust == 0) return false;
    const int n = static_cast<int>(PadStepValue(*v, lo, hi, g_pad.adjust, true));
    if (n == *v) return false;
    *v = n;
    return true;
}

// Toggle via pad: left = off, right = on. Returns changed.
bool PadToggle(bool* v) {
    if (!PadRowFocus() || g_pad.adjust == 0) return false;
    const bool nv = g_pad.adjust > 0;
    if (nv == *v) return false;
    *v = nv;
    return true;
}

// Slider with value on the right for extra rows; reserve = width kept free on the right
// (default: room for the reset icon so values line up with the parameter rows).
struct SliderState {
    bool changed = false, committed = false, released = false;
};
SliderState IntSliderRow(const char* id, int* v, int lo, int hi, const char* text, float reserve = -1.0f) {
    const float sp = ImGui::GetStyle().ItemInnerSpacing.x, valueW = Em() * 3.3f;
    if (reserve < 0.0f) reserve = ImGui::GetFrameHeight();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - valueW - reserve - sp * 2.0f);
    SliderState st;
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));  // the row is highlighted instead
    st.changed = ValueSlider(id, ImGuiDataType_S32, v, &lo, &hi, "%d", ImGuiSliderFlags_AlwaysClamp);
    ImGui::PopStyleColor();
    st.committed = ImGui::IsItemDeactivatedAfterEdit();
    st.released = ImGui::IsItemDeactivated();
    if (PadRowFocus() && g_pad.adjust != 0) {
        const int nv = static_cast<int>(PadStepValue(*v, lo, hi, g_pad.adjust, true));
        if (nv != *v) {
            *v = nv;
            st.changed = st.committed = true;
        }
    }
    ImGui::SameLine(0, sp);
    const float x = ImGui::GetCursorPosX();
    ImGui::SetCursorPosX(x + valueW - ImGui::CalcTextSize(text).x);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(text);
    return st;
}

// --- Hotkey keys: ImGui key <-> Windows VK ---
struct KeyMap {
    ImGuiKey key;
    std::uint32_t vk;
    const char* name;
};
const std::vector<KeyMap>& HotkeyKeys() {
    static const std::vector<KeyMap> keys = [] {
        std::vector<KeyMap> k;
        static const char* const kF[] = {"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12"};
        for (int i = 0; i < 12; ++i) k.push_back({static_cast<ImGuiKey>(ImGuiKey_F1 + i), static_cast<std::uint32_t>(VK_F1 + i), kF[i]});
        k.push_back({ImGuiKey_Insert, VK_INSERT, "Insert"});
        k.push_back({ImGuiKey_Delete, VK_DELETE, "Delete"});
        k.push_back({ImGuiKey_Home, VK_HOME, "Home"});
        k.push_back({ImGuiKey_PageUp, VK_PRIOR, "Page Up"});
        k.push_back({ImGuiKey_PageDown, VK_NEXT, "Page Down"});
        k.push_back({ImGuiKey_Pause, VK_PAUSE, "Pause"});
        k.push_back({ImGuiKey_ScrollLock, VK_SCROLL, "Scroll Lock"});
        k.push_back({ImGuiKey_GraveAccent, VK_OEM_3, "^"});
        static const char* const kDigits[] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
        static const char* const kNum[] = {"Num 0", "Num 1", "Num 2", "Num 3", "Num 4",
                                           "Num 5", "Num 6", "Num 7", "Num 8", "Num 9"};
        for (int i = 0; i < 10; ++i) {
            k.push_back({static_cast<ImGuiKey>(ImGuiKey_0 + i), static_cast<std::uint32_t>('0' + i), kDigits[i]});
            k.push_back({static_cast<ImGuiKey>(ImGuiKey_Keypad0 + i), static_cast<std::uint32_t>(VK_NUMPAD0 + i), kNum[i]});
        }
        static const char* const kLetters[] = {"A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
                                               "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z"};
        for (int i = 0; i < 26; ++i) k.push_back({static_cast<ImGuiKey>(ImGuiKey_A + i), static_cast<std::uint32_t>('A' + i), kLetters[i]});
        k.push_back({ImGuiKey_KeypadAdd, VK_ADD, "Num +"});
        k.push_back({ImGuiKey_KeypadSubtract, VK_SUBTRACT, "Num -"});
        k.push_back({ImGuiKey_KeypadMultiply, VK_MULTIPLY, "Num *"});
        k.push_back({ImGuiKey_KeypadDivide, VK_DIVIDE, "Num /"});
        return k;
    }();
    return keys;
}

// Parts of a combination for display ("Shift", "F2").
std::vector<std::string> HotkeyParts(std::uint32_t hk) {
    std::vector<std::string> parts;
    if (hk & (1u << 17)) parts.emplace_back("Ctrl");
    if (hk & (1u << 18)) parts.emplace_back("Alt");
    if (hk & (1u << 16)) parts.emplace_back("Shift");
    const std::uint32_t vk = hk & 0xFFFF;
    std::string name;
    for (const KeyMap& m : HotkeyKeys())
        if (m.vk == vk) name = m.name;
    if (name.empty()) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "Key %u", vk);
        name = buf;
    }
    parts.push_back(name);
    return parts;
}

// --- Key and controller glyphs for hint lines ---
// Keycap: rounded frame with label, height of a text line.
void KeyCap(const char* text, float yOff = 0.0f) {
    const Palette& p = CurrentPalette();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float h = ImGui::GetTextLineHeight();
    const float padX = h * 0.3f;
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const ImVec2 cur = ImGui::GetCursorScreenPos();
    const ImVec2 pos(cur.x, cur.y + yOff);
    const ImVec2 size(std::max(ts.x + padX * 2.0f, h), h);
    const ImU32 border = ImGui::GetColorU32(p.textDim);
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), ImGui::GetColorU32(ImVec4(1, 1, 1, 0.06f)), h * 0.2f);
    dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), border, h * 0.2f, 1.0f);
    dl->AddText(ImVec2(pos.x + (size.x - ts.x) * 0.5f, pos.y + (h - ts.y) * 0.5f), ImGui::GetColorU32(p.text), text);
    ImGui::Dummy(ImVec2(size.x, size.y + yOff));
}

enum class Pad { A, B, X, Y, LB, RB, LT, RT, View, Menu, LStick, RStick, DPad };

// Controller glyph, monochrome (like current Xbox glyphs): light shape, symbol cut out in background colour.
void PadIcon(Pad b) {
    const Palette& p = CurrentPalette();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float h = ImGui::GetTextLineHeight();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImU32 fill = ImGui::GetColorU32(ImVec4(p.text.x, p.text.y, p.text.z, 0.90f));
    const ImU32 cut = ImGui::GetColorU32(ImVec4(p.background.x, p.background.y, p.background.z, 1.0f));
    const float t = std::max(1.2f, h * 0.08f);
    // Label optically centred: explicit size, vertically aligned on cap height instead of line height.
    ImFont* font = ImGui::GetFont();
    auto centered = [&](const char* s, float w, float scale) {
        // Centre on the visible ink of the letters (glyph bounds), not on the line box: the line box includes
        // descender space and would push capitals below the middle of the button.
        const float size = ImGui::GetFontSize() * scale;
        ImFontBaked* baked = font->GetFontBaked(size);
        float x0 = FLT_MAX, x1 = -FLT_MAX, y0 = FLT_MAX, y1 = -FLT_MAX, adv = 0.0f;
        for (const char* q = s; *q; ++q) {
            const ImFontGlyph* g = baked->FindGlyph(static_cast<ImWchar>(*q));
            if (!g) continue;
            x0 = std::min(x0, adv + g->X0);
            x1 = std::max(x1, adv + g->X1);
            y0 = std::min(y0, g->Y0);
            y1 = std::max(y1, g->Y1);
            adv += g->AdvanceX;
        }
        if (x0 > x1 || y0 > y1) return;
        const float tx = pos.x + w * 0.5f - (x0 + x1) * 0.5f;
        const float ty = pos.y + h * 0.5f - (y0 + y1) * 0.5f;
        dl->AddText(font, size, ImVec2(std::round(tx), std::round(ty)), cut, s);
    };
    float w = h;
    const ImVec2 c(pos.x + h * 0.5f, pos.y + h * 0.5f);
    const float r = h * 0.48f;
    switch (b) {
        case Pad::A: case Pad::B: case Pad::X: case Pad::Y: {
            static const char* const kTxt[] = {"A", "B", "X", "Y"};
            dl->AddCircleFilled(c, r, fill, 28);
            centered(kTxt[static_cast<int>(b)], h, 0.80f);
            break;
        }
        case Pad::LB: case Pad::RB: case Pad::LT: case Pad::RT: {
            static const char* const kTxt[] = {"LB", "RB", "LT", "RT"};
            const char* s = kTxt[static_cast<int>(b) - static_cast<int>(Pad::LB)];
            w = font->CalcTextSizeA(ImGui::GetFontSize() * 0.74f, FLT_MAX, 0.0f, s).x + h * 0.75f;
            const bool trigger = b == Pad::LT || b == Pad::RT;
            // bumper: flat with rounded top corners; trigger: tall, strongly rounded top
            dl->AddRectFilled(ImVec2(pos.x, pos.y + (trigger ? 0.0f : h * 0.12f)), ImVec2(pos.x + w, pos.y + h - (trigger ? 0.0f : h * 0.12f)),
                              fill, trigger ? h * 0.45f : h * 0.30f, trigger ? (ImDrawFlags_RoundTopLeft | ImDrawFlags_RoundTopRight) : ImDrawFlags_RoundAll);
            centered(s, w, 0.74f);
            break;
        }
        case Pad::View: {
            dl->AddCircleFilled(c, r, fill, 28);
            const float s = h * 0.15f;
            dl->AddRect(ImVec2(c.x - s * 1.3f, c.y - s * 1.1f), ImVec2(c.x + s * 0.5f, c.y + s * 0.5f), cut, 0.0f, t);
            dl->AddRectFilled(ImVec2(c.x - s * 0.5f, c.y - s * 0.5f), ImVec2(c.x + s * 1.3f, c.y + s * 1.1f), cut);
            break;
        }
        case Pad::Menu: {
            dl->AddCircleFilled(c, r, fill, 28);
            const float s = h * 0.2f;
            for (int i = -1; i <= 1; ++i) dl->AddLine(ImVec2(c.x - s, c.y + i * s * 0.75f), ImVec2(c.x + s, c.y + i * s * 0.75f), cut, t);
            break;
        }
        case Pad::LStick: case Pad::RStick: {
            // stick from above: ring with filled cap and direction marks
            dl->AddCircle(c, r * 0.66f, fill, 28, t);
            dl->AddCircleFilled(c, r * 0.34f, fill, 20);
            const float m = r * 0.66f + t * 0.8f, k = h * 0.065f;
            for (int i = 0; i < 4; ++i) {
                const float dx = i == 0 ? 1.0f : i == 1 ? -1.0f : 0.0f, dy = i == 2 ? 1.0f : i == 3 ? -1.0f : 0.0f;
                const ImVec2 tip(c.x + dx * (m + k * 1.4f), c.y + dy * (m + k * 1.4f));
                dl->AddTriangleFilled(tip, ImVec2(c.x + dx * m - dy * k, c.y + dy * m + dx * k),
                                      ImVec2(c.x + dx * m + dy * k, c.y + dy * m - dx * k), fill);
            }
            break;
        }
        case Pad::DPad: {  // D-pad as a filled cross
            const float a = h * 0.46f, k = h * 0.16f;
            dl->AddRectFilled(ImVec2(c.x - k, c.y - a), ImVec2(c.x + k, c.y + a), fill, h * 0.06f);
            dl->AddRectFilled(ImVec2(c.x - a, c.y - k), ImVec2(c.x + a, c.y + k), fill, h * 0.06f);
            break;
        }
    }
    ImGui::Dummy(ImVec2(w, h));
}
// Glyphs and text on one line: parts in sequence with small gaps.
void Gap(float em = 0.25f) { ImGui::SameLine(0.0f, Em() * em); }
void HintText(const char* t) {
    Gap();
    ImGui::TextDisabled("%s", t);
    Gap(1.1f);
}
void HotkeyCaps(std::uint32_t hk, float yOff = 0.0f) {
    const auto parts = HotkeyParts(hk);
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) {
            Gap(0.15f);
            ImGui::TextDisabled("+");
            Gap(0.15f);
        }
        KeyCap(parts[i].c_str(), yOff);
    }
}

}  // namespace

std::string HotkeyName(std::uint32_t hk) {
    std::string s;
    for (const std::string& part : HotkeyParts(hk)) s += (s.empty() ? "" : "+") + part;
    return s;
}

// ---------------------------------------------------------------------------
// Settings

std::string UiSettings::Serialize() const {
    char buf[1024];
    std::snprintf(buf, sizeof(buf),
                  "theme=%d\r\nscale=%.2f\r\nexpert=%d\r\ncustomFilter=%d\r\ngameFunctions=%d\r\nownClock=%d\r\ntoggleKey=%u\r\n"
                  "onlyInHackedSave=%d\r\nfpsLimit=%d\r\nbackgroundLimit=%d\r\nshowFps=%d\r\nshowFrameGraph=%d\r\nstatsCorner=%d\r\n"
                  "sharpen=%d\r\nfpsCustom=%d\r\nteleport=%d\r\ntpRange=%d\r\ntpAllowCombat=%d\r\ntpAllowMounted=%d\r\n"
                  "swimBoost=%d\r\nmountBoost=%d\r\ntpQuick=%d\r\ntpQuickHotkey=%u\r\nhideWatermark=%d\r\n",
                  static_cast<int>(theme), scale, expert ? 1 : 0, customFilter ? 1 : 0, gameFunctions ? 1 : 0, ownClock ? 1 : 0,
                  static_cast<unsigned>(toggleKey), onlyInHackedSave ? 1 : 0, fpsLimit, backgroundLimit ? 1 : 0, showFps ? 1 : 0,
                  showFrameGraph ? 1 : 0, statsCorner, sharpen, fpsCustom ? 1 : 0, teleport ? 1 : 0, tpRange,
                  tpAllowCombat ? 1 : 0, tpAllowMounted ? 1 : 0, swimBoost, mountBoost, tpQuick ? 1 : 0,
                  static_cast<unsigned>(tpQuickKey), hideWatermark ? 1 : 0);
    return buf;
}

UiSettings UiSettings::Parse(std::string_view text) {
    UiSettings s;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t end = text.find_first_of("\r\n", pos);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view line = text.substr(pos, end - pos);
        pos = end + 1;
        const auto eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        const std::string_view k = line.substr(0, eq);
        const std::string v(line.substr(eq + 1));
        if (k == "theme") s.theme = std::atoi(v.c_str()) == 1 ? ThemeId::Origins : ThemeId::UplayClassic;
        else if (k == "scale") s.scale = std::clamp(static_cast<float>(std::atof(v.c_str())), 0.75f, 1.5f);
        else if (k == "expert") s.expert = v == "1";
        else if (k == "customFilter") s.customFilter = v == "1";
        else if (k == "gameFunctions") s.gameFunctions = v == "1";
        else if (k == "ownClock") s.ownClock = v == "1";
        else if (k == "onlyInHackedSave") s.onlyInHackedSave = v == "1";
        else if (k == "fpsLimit") s.fpsLimit = std::clamp(std::atoi(v.c_str()), 0, 1000);
        else if (k == "backgroundLimit") s.backgroundLimit = v == "1";
        else if (k == "showFps") s.showFps = v == "1";
        else if (k == "showFrameGraph") s.showFrameGraph = v == "1";
        else if (k == "statsCorner") s.statsCorner = std::clamp(std::atoi(v.c_str()), 0, 3);
        else if (k == "sharpen") s.sharpen = std::clamp(std::atoi(v.c_str()), 0, 100);
        else if (k == "fpsCustom") s.fpsCustom = v == "1";
        else if (k == "teleport") s.teleport = v == "1";
        else if (k == "tpRange") s.tpRange = std::clamp(std::atoi(v.c_str()), 0, 2);
        else if (k == "tpAllowCombat") s.tpAllowCombat = v == "1";
        else if (k == "tpAllowMounted") s.tpAllowMounted = v == "1";
        else if (k == "swimBoost") s.swimBoost = std::clamp(std::atoi(v.c_str()), 100, 400);
        else if (k == "mountBoost") s.mountBoost = std::clamp(std::atoi(v.c_str()), 100, 400);
        else if (k == "tpQuick") s.tpQuick = v == "1";
        else if (k == "hideWatermark") s.hideWatermark = v == "1";
        else if (k == "tpQuickHotkey") {
            const unsigned long hk = std::strtoul(v.c_str(), nullptr, 10);
            if (hk <= 0x7FFFF) s.tpQuickKey = static_cast<std::uint32_t>(hk);  // 0 = controller only
        }
        else if (k == "toggleKey") {
            const unsigned long hk = std::strtoul(std::string(v).c_str(), nullptr, 10);
            if ((hk & 0xFFFF) != 0 && hk <= 0x7FFFF) s.toggleKey = static_cast<std::uint32_t>(hk);
        }
    }
    return s;
}

void Panel::SaveSettings() {
    sync_.SaveSettings(settings_.Serialize());
    themeDirty_ = true;
}

void Panel::ApplySettingsIfNeeded(const PanelEnv& env) {
    const float scale = env.baseScale * settings_.scale;
    if (!themeDirty_ && appliedScale_ == scale) return;
    ImGui::GetStyle().FontScaleMain = settings_.scale;
    ApplyTheme(settings_.theme, scale);
    appliedScale_ = scale;
    themeDirty_ = false;
}

// ---------------------------------------------------------------------------
// State

void Panel::Update() {
    if (sync_.TryConsumeSnapshot(snap_)) {
        haveSnap_ = true;
        snapDeferred_ = true;
        if (!settingsLoaded_) {
            settings_ = UiSettings::Parse(snap_.settingsText);
            scaleEdit_ = settings_.scale;
            settingsLoaded_ = true;
            themeDirty_ = true;
        }
    }
    if (!snapDeferred_ || ImGui::IsAnyItemActive()) return;  // don't change values while the user is editing
    const std::uint64_t now = GetTickCount64();
    for (std::size_t i = 0; i < kParamCount; ++i) {
        if (staged_[i]) continue;  // keep staged change until "Apply"
        if (localEdit_[i]) {
            if (snap_.values[i] == *localEdit_[i] || now - localEditTick_[i] > 3000) localEdit_[i].reset();
            else continue;
        }
        work_[i] = snap_.values[i];
        committed_[i] = snap_.values[i];
    }
    snapDeferred_ = false;
}

void Panel::OnOpened() {
    focusContent_ = true;
    capturingKey_ = false;
    ImGui::SetNavCursorVisible(lastInputPad_);
}

void Panel::Submit(ParamId id, ParamValue v, bool record) {
    const auto i = static_cast<std::size_t>(id);
    if (record) Record(id, v);
    committed_[i] = v;
    work_[i] = v;
    localEdit_[i] = v;
    localEditTick_[i] = GetTickCount64();
    sync_.SubmitEdit(id, v);
}

void Panel::Commit(const ParamDesc& d, ParamValue v, const PanelEnv& env) {
    const auto i = static_cast<std::size_t>(d.id);
    if (d.reloadsWorld && env.overlay) {
        Record(d.id, v);
        committed_[i] = v;
        work_[i] = v;
        staged_[i] = v;  // send only on "Apply": the game reloads the world for it
        return;
    }
    Submit(d.id, v);
}

// ---------------------------------------------------------------------------
// Undo

void Panel::Record(ParamId id, const ParamValue& newValue) {
    const ParamValue old = committed_[static_cast<std::size_t>(id)];
    if (old == newValue) return;
    if (undoGroup_) {
        for (const auto& e : undoGroup_->old)
            if (e.first == id) return;  // first (oldest) value of the group counts
        undoGroup_->old.emplace_back(id, old);
        return;
    }
    // Same parameter several times in a row (e.g. pad slider steps): one undo step
    const std::uint64_t now = GetTickCount64();
    if (!undo_.empty() && undo_.back().old.size() == 1 && undo_.back().old[0].first == id && now - lastRecordTick_ < 1500) {
        lastRecordTick_ = now;
        return;
    }
    undo_.push_back({std::string(DisplayLabel(Desc(id))), {{id, old}}});
    if (undo_.size() > 50) undo_.erase(undo_.begin());
    lastRecordTick_ = now;
}

void Panel::BeginUndoGroup(std::string label) { undoGroup_ = UndoEntry{std::move(label), {}}; }

void Panel::EndUndoGroup() {
    if (undoGroup_ && !undoGroup_->old.empty()) {
        undo_.push_back(std::move(*undoGroup_));
        if (undo_.size() > 50) undo_.erase(undo_.begin());
    }
    undoGroup_.reset();
    lastRecordTick_ = 0;
}

void Panel::RecordAll(std::string label) {
    UndoEntry e{std::move(label), {}};
    for (const ParamDesc& d : ParamTable())
        if (!d.readOnly) e.old.emplace_back(d.id, committed_[static_cast<std::size_t>(d.id)]);
    undo_.push_back(std::move(e));
    if (undo_.size() > 50) undo_.erase(undo_.begin());
    lastRecordTick_ = 0;
}

void Panel::Undo(const PanelEnv& env) {
    if (undo_.empty() || undoGroup_ || activeEdit_) return;
    const UndoEntry e = std::move(undo_.back());
    undo_.pop_back();
    for (const auto& [id, old] : e.old) {
        const auto i = static_cast<std::size_t>(id);
        if (work_[i] == old && !staged_[i]) continue;
        const ParamDesc& d = Desc(id);
        if (d.reloadsWorld && env.overlay) {
            committed_[i] = old;
            work_[i] = old;
            if (haveSnap_ && snap_.values[i] == old) staged_[i].reset();  // matches the game again: nothing to send
            else staged_[i] = old;
        } else {
            staged_[i].reset();
            Submit(id, old, false);
        }
    }
    undoToast_ = "Undone: " + e.label;
    undoToastUntil_ = GetTickCount64() + 2500;
    lastRecordTick_ = 0;
}

void Panel::FlushActiveEdit() {
    if (activeEdit_) {
        const ParamDesc& d = Desc(*activeEdit_);
        const ParamValue v = work_[static_cast<std::size_t>(*activeEdit_)];
        if (d.reloadsWorld) staged_[static_cast<std::size_t>(d.id)] = v;
        else Submit(d.id, v);
        activeEdit_.reset();
    }
}

bool Panel::IsDisabled(const ParamDesc& d) const {
    if (d.local && !settings_.customFilter) return true;
    for (const auto& w : d.disabledWhen) {
        if (w.Active() && work_[static_cast<std::size_t>(w.param)].b == w.whenValue) return true;
    }
    return false;
}

bool Panel::Visible(const ParamDesc& d) const {
    if (d.readOnly) return false;
    if (changedOnly_ && work_[static_cast<std::size_t>(d.id)] == d.def) return false;
    return ContainsNoCase(d.label, search_) || ContainsNoCase(d.key, search_);
}

// ---------------------------------------------------------------------------
// Drawing

void Panel::Draw(const PanelEnv& env, const PadActions& pad, bool* closeRequested) {
    if (closeRequested) *closeRequested = false;
    portraitFocused_ = false;  // set again by DrawCharacterPreview this frame
    const bool capturingAtStart = capturingKey_;
    g_pad = pad;
    g_rowFocused = false;  // Esc then only ends key recording
    Update();
    ApplySettingsIfNeeded(env);
    ImGuiIO& io = ImGui::GetIO();
    if (pad.padUsed) lastInputPad_ = true;
    else if (io.MouseDelta.x != 0 || io.MouseDelta.y != 0 || ImGui::IsAnyMouseDown()) lastInputPad_ = false;

    // Pages 1..6 (presets live in the header, no longer a page of their own)
    if (category_ < 1) category_ = 1;
    if (pad.prevCategory) {
        category_ = category_ <= 1 ? kSettingsPage : category_ - 1;
        focusContent_ = true;
    }
    if (pad.nextCategory) {
        category_ = category_ >= kSettingsPage ? 1 : category_ + 1;
        focusContent_ = true;
    }

    // Undo: Ctrl+Z (not in text fields) or Y on the controller (when nothing is being edited)
    if (!capturingKey_ && !ImGui::IsAnyItemActive() && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup) &&
        ((!io.WantTextInput && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z)) ||
         ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false)))
        Undo(env);

    const float em = Em();
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoTitleBar;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    if (env.fillViewport) {
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        flags |= ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
    } else {
        const ImVec2 size(std::min(em * 72.0f, vp->WorkSize.x * 0.9f), std::min(em * 44.0f, vp->WorkSize.y * 0.9f));
        ImGui::SetNextWindowSize(size, ImGuiCond_Once);
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                                ImGuiCond_Once, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(em * 38, em * 25), ImVec2(FLT_MAX, FLT_MAX));
    }

    if (ImGui::Begin("Animus Control Panel##acp", nullptr, flags)) {
        if (CurrentTheme() == ThemeId::Origins) {
            const ImVec2 wp = ImGui::GetWindowPos();
            DrawOriginsBackdrop(ImGui::GetWindowDrawList(), wp, ImVec2(wp.x + ImGui::GetWindowWidth(), wp.y + ImGui::GetWindowHeight()));
        }
        const float footerH = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y * 2;
        DrawSidebar(env);
        ImGui::SameLine(0, em * 1.0f);

        ImGui::BeginGroup();
        DrawHeader(env);
        const bool paramPage = category_ >= 1 && category_ <= 5;
        if (paramPage) DrawToolbar(env);
        DrawMissingIniBar();
        DrawStagedBar();
        char childId[32];
        std::snprintf(childId, sizeof(childId), "content_%d", category_);
        if (focusContent_) ImGui::SetNextWindowFocus();
        if (ImGui::BeginChild(childId, ImVec2(0, -footerH), ImGuiChildFlags_NavFlattened)) {
            if (std::fabs(pad.scroll) > 0.0f) ImGui::SetScrollY(ImGui::GetScrollY() + pad.scroll * em);
            if (debugScroll_ > 0.0f) ImGui::SetScrollY(debugScroll_ * em);  // editor screenshots only
            if (category_ == kSettingsPage) DrawSettings(env);
            else if (search_[0]) DrawSearchResults(env);
            else DrawCategory(ToCategory(category_), env);
        }
        ImGui::EndChild();
        ImGui::EndGroup();
        focusContent_ = false;
        DrawFooter(env);

        const std::uint64_t nowMs = GetTickCount64();
        if (nowMs < undoToastUntil_) {
            const float remaining = static_cast<float>(undoToastUntil_ - nowMs) / 1000.0f;
            const float alpha = std::min(1.0f, remaining / 0.4f);  // fade out over the last 0.4 s
            const Palette& pal = CurrentPalette();
            const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
            const ImVec2 ts = ImGui::CalcTextSize(undoToast_.c_str());
            const float padX = em * 1.0f, padY = em * 0.5f;
            const ImVec2 a(wp.x + (ws.x - ts.x) * 0.5f - padX, wp.y + ws.y - footerH - ts.y - padY * 2.0f - em * 1.2f);
            const ImVec2 b(a.x + ts.x + padX * 2.0f, a.y + ts.y + padY * 2.0f);
            ImDrawList* fg = ImGui::GetForegroundDrawList();
            const ImVec4 bg(pal.sidebar.x, pal.sidebar.y, pal.sidebar.z, 0.96f * alpha);
            fg->AddRectFilled(a, b, ImGui::GetColorU32(bg), (b.y - a.y) * 0.5f * pal.toggleRounding);
            fg->AddRect(a, b, ImGui::GetColorU32(ImVec4(pal.accent.x, pal.accent.y, pal.accent.z, alpha)),
                        (b.y - a.y) * 0.5f * pal.toggleRounding, 1.0f);
            fg->AddText(ImVec2(a.x + padX, a.y + padY), ImGui::GetColorU32(ImVec4(pal.text.x, pal.text.y, pal.text.z, alpha)),
                        undoToast_.c_str());
        }

        if (requestClose_ && closeRequested) {
            *closeRequested = true;
            requestClose_ = false;
        }
        // B / Esc closes the overlay - only if nothing was active and no popup was open in the previous frame.
        if (env.overlay && closeRequested && !capturingAtStart && !wasAnyItemActive_ && !popupWasOpen_ &&
            !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup) &&
            (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
            *closeRequested = true;
        }
    }
    ImGui::End();
    wasAnyItemActive_ = ImGui::IsAnyItemActive();
    // ImGui closes a popup on B/Esc before our check runs, so remember whether one was open: that key press
    // then only closes the popup (e.g. the character overview), not the whole panel.
    popupWasOpen_ = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup);
    wantsHorizontal_ = g_rowFocused && lastInputPad_ && !wasAnyItemActive_ && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup);
}

// Content header: large page title centred (left-aligned when space is short), live and save status on the right.
void Panel::DrawHeader(const PanelEnv& env) {
    const Palette& p = CurrentPalette();
    const float em = Em();
    const float rightEdge = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;

    // Status block width (needed to keep the centred title clear of it)
    const StatusView sv = haveSnap_ ? ViewOf(snap_.status) : ViewOf(SyncStatus::Starting);
    const char* live = env.overlay ? (env.liveActive ? "Live" : "Not live") : "Editor";
    const float pillPadX = em * 0.6f;
    const float dotR = em * 0.22f;
    const float pillW = ImGui::CalcTextSize(live).x + pillPadX * 2.0f + dotR * 2.0f + em * 0.35f;
    const float syncW = dotR * 2.0f + em * 0.35f + ImGui::CalcTextSize(sv.text).x;
    {
        // Centre the title group ([LB] title [RB] on the controller) over the content area
        const char* title = kCategoryNames[category_];
        float groupW = HeadingWidth(title, 1.45f);
        if (lastInputPad_) {
            const float bumperW = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize() * 0.74f, FLT_MAX, 0.0f, "LB").x +
                                  ImGui::GetTextLineHeight() * 0.75f;
            groupW += 2.0f * (bumperW + em * 0.6f);
        }
        const float startX = ImGui::GetCursorPosX();
        const float centred = startX + (rightEdge - startX - groupW) * 0.5f;
        const float statusLeft = rightEdge - pillW - syncW - em * 1.2f;
        if (centred + groupW < statusLeft - em) ImGui::SetCursorPosX(centred);
    }

    if (lastInputPad_) {
        // Like console tabs: [LB] Title [RB]
        const float lineH = ImGui::GetTextLineHeight() * 1.45f;
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (lineH - ImGui::GetTextLineHeight()) * 0.5f);
        PadIcon(Pad::LB);
        ImGui::SameLine(0, em * 0.6f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - (lineH - ImGui::GetTextLineHeight()) * 0.5f);
    }
    HeadingText(kCategoryNames[category_], 1.45f, CurrentTheme() == ThemeId::Origins ? p.accent : p.text);
    if (lastInputPad_) {
        const float lineH = ImGui::GetTextLineHeight() * 1.45f;
        ImGui::SameLine(0, em * 0.6f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (lineH - ImGui::GetTextLineHeight()) * 0.5f);
        PadIcon(Pad::RB);
    }

    // Right: live pill, then save status as dot + short text
    ImGui::SameLine();
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), rightEdge - pillW - syncW - em * 1.2f));
    {
        // Pill: live (accent) or not live (dimmed); tooltip explains the state
        const float h = ImGui::GetFrameHeight();
        const ImVec2 pos(ImGui::GetCursorScreenPos().x, ImGui::GetCursorScreenPos().y + (ImGui::GetTextLineHeight() * 1.45f - h) * 0.5f);
        const ImVec4 col = env.liveActive || !env.overlay ? p.accent : p.textDim;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(pos, ImVec2(pos.x + pillW, pos.y + h), ImGui::GetColorU32(ImVec4(col.x, col.y, col.z, 0.14f)),
                         h * 0.5f * p.toggleRounding);
        if (CurrentTheme() == ThemeId::Origins)
            dl->AddRect(pos, ImVec2(pos.x + pillW, pos.y + h), ImGui::GetColorU32(ImVec4(col.x, col.y, col.z, 0.45f)));
        dl->AddCircleFilled(ImVec2(pos.x + pillPadX + dotR, pos.y + h * 0.5f), dotR, ImGui::GetColorU32(col), 16);
        dl->AddText(ImVec2(pos.x + pillPadX + dotR * 2.0f + em * 0.35f, pos.y + (h - ImGui::GetTextLineHeight()) * 0.5f),
                    ImGui::GetColorU32(col), live);
        ImGui::Dummy(ImVec2(pillW, ImGui::GetTextLineHeight() * 1.45f));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", env.liveHint.empty() ? "Changes are saved to ACP.ini." : env.liveHint.c_str());
    }
    ImGui::SameLine(0, em * 1.0f);
    {
        const float lineH = ImGui::GetTextLineHeight() * 1.45f;
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddCircleFilled(ImVec2(pos.x + dotR, pos.y + lineH * 0.5f), dotR, ImGui::GetColorU32(sv.color), 16);
        dl->AddText(ImVec2(pos.x + dotR * 2.0f + em * 0.35f, pos.y + (lineH - ImGui::GetTextLineHeight()) * 0.5f),
                    ImGui::GetColorU32(p.textDim), sv.text);
        ImGui::Dummy(ImVec2(syncW, lineH));
        if (ImGui::IsItemHovered() && haveSnap_)
            ImGui::SetTooltip("Changes are saved to ACP.ini immediately.\n%s\n%s",
                              snap_.path.empty() ? "(no file)" : snap_.path.string().c_str(), snap_.message.c_str());
    }
    if (haveSnap_ && snap_.multipleCandidates)
        ImGui::TextColored(colors::Warn, "Two ACP.ini files found - using %s", snap_.path.string().c_str());
    ImGui::Dummy(ImVec2(0, em * 0.15f));
}

// Sidebar: brand at the top, categories with icons, settings at the bottom.
// In narrow windows icons only (name as tooltip) so the content keeps its space.
void Panel::DrawSidebar(const PanelEnv& env) {
    (void)env;
    const Palette& p = CurrentPalette();
    const float em = Em();
    const float footerH = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y * 2;
    const bool compact = ImGui::GetContentRegionAvail().x < em * 58.0f;
    const bool origins = CurrentTheme() == ThemeId::Origins;
    static constexpr Icon kIcons[] = {Icon::Logo,        Icon::HeroGeneral, Icon::HeroFighting, Icon::NpcGeneral,
                                      Icon::NpcFighting, Icon::World,       Icon::Settings};
    ImGui::PushStyleColor(ImGuiCol_ChildBg, p.sidebar);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, em * 0.8f));
    if (ImGui::BeginChild("sidebar", ImVec2(compact ? em * 3.8f : em * 12.5f, -footerH), ImGuiChildFlags_AlwaysUseWindowPadding)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float width = ImGui::GetContentRegionAvail().x;
        // Brand
        {
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const float s = em * 1.6f;
            const float lx = compact ? pos.x + (width - s) * 0.5f : pos.x + em * 1.0f;
            DrawIcon(dl, Icon::Logo, ImVec2(lx + s * 0.5f, pos.y + s * 0.5f), s, ImGui::GetColorU32(p.accent));
            if (!compact) {
                const ImVec2 tp(lx + s + em * 0.6f, pos.y);
                if (origins && HeadingFont())
                    dl->AddText(HeadingFont(), ImGui::GetFontSize() * 1.15f, ImVec2(tp.x, tp.y - em * 0.1f), ImGui::GetColorU32(p.accent), p.title);
                else
                    dl->AddText(tp, ImGui::GetColorU32(p.text), p.title);
                dl->AddText(ImVec2(tp.x, tp.y + ImGui::GetTextLineHeight()), ImGui::GetColorU32(p.textDim), p.subtitle);
            }
            ImGui::Dummy(ImVec2(0, std::max(s, compact ? s : ImGui::GetTextLineHeight() * 2.0f) + em * 1.0f));
        }
        const float rowH = ImGui::GetFrameHeight() * 1.25f;
        auto item = [&](int i) {
            const bool selected = i == category_;
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            ImGui::PushID(i);
            ImGui::PushStyleColor(ImGuiCol_Header, origins ? ImVec4(0, 0, 0, 0) : ImVec4(p.accent.x, p.accent.y, p.accent.z, 0.12f));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(p.text.x, p.text.y, p.text.z, 0.06f));
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(p.accent.x, p.accent.y, p.accent.z, 0.20f));
            // Not a nav target: the controller switches categories with LB/RB, up/down stays in the content.
            ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
            if (ImGui::Selectable("##cat", selected, 0, ImVec2(width, rowH)) && category_ != i) {
                category_ = i;
                focusContent_ = true;
            }
            ImGui::PopItemFlag();
            if (compact && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kCategoryNames[i]);
            ImGui::PopStyleColor(3);
            ImGui::PopID();
            const ImU32 col = ImGui::GetColorU32(selected && !origins ? p.accent : p.text);
            const float inset = compact ? em * 0.35f : em * 0.5f;
            if (selected) {
                if (origins) {
                    const ImVec2 a(pos.x + inset, pos.y + 1.0f), b(pos.x + width - inset, pos.y + rowH - 1.0f);
                    dl->AddRectFilled(a, b, ImGui::GetColorU32(p.highlight));
                    dl->AddRect(a, b, ImGui::GetColorU32(p.accent), 0.0f, 1.5f);
                } else {
                    dl->AddRectFilled(pos, ImVec2(pos.x + 3.0f, pos.y + rowH), ImGui::GetColorU32(p.accent));
                }
            }
            const float iconS = em * 1.15f;
            const float ix = compact ? pos.x + width * 0.5f : pos.x + em * 1.0f + iconS * 0.5f;
            DrawIcon(dl, kIcons[i], ImVec2(ix, pos.y + rowH * 0.5f), iconS, col);
            if (!compact)
                dl->AddText(ImVec2(pos.x + em * 1.0f + iconS + em * 0.7f, pos.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f), col,
                            kCategoryNames[i]);
        };
        for (int i = 1; i <= 5; ++i) item(i);
        // Settings at the bottom
        const float bottom = ImGui::GetWindowHeight() - ImGui::GetStyle().WindowPadding.y - rowH;
        if (ImGui::GetCursorPosY() < bottom) ImGui::SetCursorPosY(bottom);
        item(kSettingsPage);
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}
// Presets: selecting applies after confirmation; save and delete as icon buttons.
void Panel::DrawPresetControls(float comboW) {
    const auto& presets = snap_.presets;
    const float em = Em();
    bool openApply = false, openSave = false, openDelete = false;

    ImGui::SetNextItemWidth(comboW);
    if (ComboBox("##preset", activePreset_.empty() ? "Choose a preset..." : activePreset_.c_str())) {
        if (presets.empty()) ImGui::TextDisabled("No presets yet - save your current values first.");
        for (int i = 0; i < static_cast<int>(presets.size()); ++i) {
            const std::string label = presets[i].name + (presets[i].readOnly ? "  (bundled)" : "");
            if (ImGui::Selectable(label.c_str(), presets[i].name == activePreset_)) {
                pendingPreset_ = i;
                openApply = true;
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Presets: apply a saved set of values.");
    ImGui::SameLine(0, em * 0.2f);
    if (IconButton("##savepreset", Icon::Save, "Save current values as a preset")) openSave = true;
    int activeIdx = -1;
    for (int i = 0; i < static_cast<int>(presets.size()); ++i)
        if (presets[i].name == activePreset_) activeIdx = i;
    ImGui::SameLine(0, 0);
    ImGui::BeginDisabled(activeIdx < 0 || presets[activeIdx].readOnly);
    if (IconButton("##delpreset", Icon::Trash, "Delete the selected preset")) openDelete = true;
    ImGui::EndDisabled();

    if (openApply) ImGui::OpenPopup("Apply preset?");
    if (openSave) {
        int n = 1;
        for (;;) {
            char name[64];
            std::snprintf(name, sizeof(name), "Custom Preset %d", n);
            if (std::none_of(presets.begin(), presets.end(), [&](const PresetInfo& pi) { return pi.name == name; })) {
                std::snprintf(presetName_, sizeof(presetName_), "%s", name);
                break;
            }
            ++n;
        }
        ImGui::OpenPopup("Save preset");
    }
    if (openDelete) ImGui::OpenPopup("Delete preset?");

    if (ImGui::BeginPopupModal("Apply preset?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const bool ok = pendingPreset_ >= 0 && pendingPreset_ < static_cast<int>(presets.size());
        ImGui::Text("Apply '%s'?", ok ? presets[pendingPreset_].name.c_str() : "");
        ImGui::TextDisabled("Your current values will be replaced.");
        if (ImGui::Button("Apply") && ok) {
            RecordAll("Preset '" + presets[pendingPreset_].name + "'");
            sync_.ApplyPreset(presets[pendingPreset_].name);
            activePreset_ = presets[pendingPreset_].name;
            snapDeferred_ = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::SetItemDefaultFocus();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("Save preset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Name");
        ImGui::SetNextItemWidth(em * 16.0f);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##name", presetName_, sizeof(presetName_), ImGuiInputTextFlags_EnterReturnsTrue);
        if ((ImGui::Button("Save") || enter) && presetName_[0]) {
            sync_.SavePreset(presetName_);
            activePreset_ = presetName_;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("Delete preset?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Delete '%s'?", activePreset_.c_str());
        if (ImGui::Button("Delete") && activeIdx >= 0) {
            sync_.DeletePreset(activePreset_);
            activePreset_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::SetItemDefaultFocus();
        ImGui::EndPopup();
    }
}

// Toolbar of the parameter pages: presets, search, "changed only", undo, reset page.
// Adapts to the width: preset combo and search shrink to a minimum width, then search and
// "Changed only" wrap to a second line. The buttons on the right stay pinned to the right edge.
void Panel::DrawToolbar(const PanelEnv& env) {
    const Palette& p = CurrentPalette();
    const float em = Em();
    const float fh = ImGui::GetFrameHeight();
    const float avail = ImGui::GetContentRegionAvail().x;
    const float startX = ImGui::GetCursorPosX();
    const float rightW = fh * 2.0f;                                     // undo + reset page
    const float changedW = fh * 1.9f + ImGui::GetStyle().ItemSpacing.x + ImGui::CalcTextSize("Changed only").x;
    const float comboW = std::clamp(avail * 0.28f, em * 8.5f, em * 13.0f);
    const float presetW = comboW + em * 0.2f + fh * 2.0f;
    const float gapL = em * 1.2f, gapS = em * 1.0f;
    float searchW = avail - presetW - gapL - gapS - changedW - gapS - rightW;
    const bool wrap = searchW < em * 7.0f;
    searchW = wrap ? 0.0f : std::min(searchW, em * 14.0f);

    auto rightButtons = [&]() {
        ImGui::SameLine();
        ImGui::SetCursorPosX(startX + avail - rightW);
        ImGui::BeginDisabled(undo_.empty());
        const std::string tip = undo_.empty() ? std::string("Nothing to undo")
                                              : "Undo: " + undo_.back().label + "\nCtrl+Z  /  controller: Y";
        if (IconButton("##undo", Icon::Undo, tip.c_str())) Undo(env);
        ImGui::EndDisabled();
        ImGui::SameLine(0, 0);
        if (IconButton("##resetpage", Icon::Reset, "Reset this page to the original values")) ImGui::OpenPopup("Reset this page?");
    };
    auto searchAndFilter = [&](float w) {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(em * 1.9f, ImGui::GetStyle().FramePadding.y));
        ImGui::SetNextItemWidth(w);
        ImGui::InputTextWithHint("##search", "Search", search_, sizeof(search_));
        ImGui::PopStyleVar();
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        DrawIcon(ImGui::GetWindowDrawList(), Icon::Search, ImVec2(a.x + em * 0.95f, (a.y + b.y) * 0.5f), em * 0.95f,
                 ImGui::GetColorU32(p.textDim));
        ImGui::SameLine(0, gapS);
        ToggleSwitch("##changed", &changedOnly_, false);
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Changed only");
    };

    DrawPresetControls(comboW);
    if (!wrap) {
        ImGui::SameLine(0, gapL);
        searchAndFilter(searchW);
        rightButtons();
    } else {
        rightButtons();
        searchAndFilter(std::max(em * 7.0f, avail - changedW - gapS));
    }
    if (ImGui::BeginPopupModal("Reset this page?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Set all values on '%s' back to their original values?", kCategoryNames[category_]);
        if (ImGui::Button("Reset")) {
            BeginUndoGroup(std::string("Reset ") + kCategoryNames[category_]);
            for (const ParamDesc& d : ParamTable()) {
                const auto i = static_cast<std::size_t>(d.id);
                if (d.category == ToCategory(category_) && !d.readOnly && !(work_[i] == d.def)) {
                    staged_[i].reset();
                    Submit(d.id, d.def);  // page reset applies immediately on purpose (including reload values)
                }
            }
            EndUndoGroup();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::SetItemDefaultFocus();
        ImGui::EndPopup();
    }
    ImGui::Dummy(ImVec2(0, em * 0.2f));
}

// ACP.ini missing (deleted or never created): notice with a button to create it with original values.
// Only on an explicit click - the panel never creates the file on its own.
void Panel::DrawMissingIniBar() {
    if (!haveSnap_ || snap_.status != SyncStatus::NotFound) return;
    const Palette& p = CurrentPalette();
    const float em = Em();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, p.card);
    ImGui::PushStyleColor(ImGuiCol_Border, colors::Warn);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em * 1.0f, em * 0.7f));
    const bool open = ImGui::BeginChild("missingini", ImVec2(0, 0),
                                        ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders |
                                            ImGuiChildFlags_NavFlattened);
    ImGui::PopStyleColor();  // warning border for the card only, not for the button inside
    if (open) {
        InlineIcon(Icon::Warning, colors::Warn);
        ImGui::SameLine();
        ImGui::TextUnformatted("ACP.ini not found");
        ImGui::TextWrapped("The game creates this file the first time it starts. If it was deleted or never created, "
                           "ACP Reloaded can create it with the original values. Your saves are not affected.");
        if (ImGui::Button("Create ACP.ini with original values")) sync_.CreateMissingIni();
        if (!snap_.message.empty() && snap_.message.rfind("ACP.ini not found", 0) != 0) {
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", snap_.message.c_str());
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, em * 0.2f));
}

void Panel::DrawStagedBar() {
    int n = 0;
    for (const auto& s : staged_) n += s.has_value();
    if (n == 0) return;
    const Palette& p = CurrentPalette();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(p.accentDim.x, p.accentDim.y, p.accentDim.z, 0.25f));
    if (ImGui::BeginChild("staged", ImVec2(0, ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().WindowPadding.y * 2),
                          ImGuiChildFlags_AlwaysUseWindowPadding)) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(colors::Warn, "%d change(s) waiting - the game reloads the world when they are applied.", n);
        ImGui::SameLine();
        if (ImGui::Button("Apply now")) {
            for (std::size_t i = 0; i < kParamCount; ++i) {
                if (staged_[i]) {
                    Submit(static_cast<ParamId>(i), *staged_[i], false);
                    staged_[i].reset();
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard")) {
            for (std::size_t i = 0; i < kParamCount; ++i) {
                if (staged_[i]) {
                    staged_[i].reset();
                    if (haveSnap_) work_[i] = committed_[i] = snap_.values[i];
                }
            }
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// Approximate row count of a group (for the two-column layout); 0 = nothing to show.
int Panel::GroupWeight(Category cat, std::string_view group, const PanelEnv& env) const {
    int n = 0;
    for (const ParamDesc* d : OrderedGroup(cat, group))
        if (Visible(*d)) ++n;
    const bool unfiltered = !search_[0] && !changedOnly_;
    if (!unfiltered) return n ? n + 1 : 0;
    const bool game = env.game.supported && settings_.gameFunctions;
    if (group == kGroupTime) n += 2 + (game ? 4 : 0);
    else if (group == kGroupSandstorm) n += 3 + (game ? 3 : 0);
    else if (group == kGroupCustomFx) n += 1;
    else if (group == "World Map" && game && settings_.teleport) n += 12;
    else if (group == "Movement" && game) n += 4;
    else if (group == "Character & Companions" && env.portrait) n += 7;  // portrait of the selected character
    else if (group == kGroupProgression && game) n += 5;
    return n ? n + 1 : 0;  // + card header
}

// Groups as cards: two columns when wide enough (left column first, order preserved).
void Panel::DrawGroupColumns(Category cat, const std::vector<std::string_view>& groups, const PanelEnv& env, bool& first) {
    std::vector<std::pair<std::string_view, int>> shown;
    int total = 0;
    for (std::string_view g : groups) {
        const int w = GroupWeight(cat, g, env);
        if (w > 0) {
            shown.emplace_back(g, w);
            total += w;
        }
    }
    if (shown.empty()) return;
    // With a controller: one column (like console menus: up/down = row, left/right = value)
    const bool twoCols = !lastInputPad_ && ImGui::GetContentRegionAvail().x >= Em() * 46.0f && shown.size() > 1;
    if (!twoCols) {
        for (const auto& [g, w] : shown) DrawGroup(cat, g, env, first);
        return;
    }
    // Distribute by height: each card (in order) goes into the currently shorter column so both end about level.
    (void)total;
    std::vector<std::size_t> colIdx[2];
    int colH[2] = {0, 0};
    for (std::size_t i = 0; i < shown.size(); ++i) {
        const int c = colH[1] < colH[0] ? 1 : 0;
        colIdx[c].push_back(i);
        colH[c] += shown[i].second + 1;  // + gap between cards
    }
    char id[48];
    std::snprintf(id, sizeof(id), "cols_%d", static_cast<int>(cat));
    if (ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoPadOuterX)) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        for (std::size_t i : colIdx[0]) DrawGroup(cat, shown[i].first, env, first);
        ImGui::TableSetColumnIndex(1);
        for (std::size_t i : colIdx[1]) DrawGroup(cat, shown[i].first, env, first);
        ImGui::EndTable();
    }
}

void Panel::DrawCategory(Category cat, const PanelEnv& env) {
    bool first = true;
    DrawGroupColumns(cat, GroupsOf(cat), env, first);
    if (first) ImGui::TextDisabled(changedOnly_ ? "No changed values on this page." : "Nothing to show.");
}

void Panel::DrawSearchResults(const PanelEnv& env) {
    bool first = true;
    for (int c = 1; c <= 5; ++c) {
        const Category cat = ToCategory(c);
        std::vector<std::string_view> groups;
        for (std::string_view g : GroupsOf(cat))
            if (GroupWeight(cat, g, env) > 0) groups.push_back(g);
        if (groups.empty()) continue;
        ImGui::TextDisabled("%s", kCategoryNames[c]);
        DrawGroupColumns(cat, groups, env, first);
    }
    if (first) ImGui::TextDisabled("No parameter matches '%s'.", search_);
}

// Card frame: title, then a two-column table (label left, control right).
// warn: border in warning colour (for permanent actions). Returns whether the table is open (then EndCard(true)).
bool Panel::BeginCard(const std::string& id, const char* title, bool warn, const char* note) {
    const Palette& p = CurrentPalette();
    const float em = Em();
    const bool origins = CurrentTheme() == ThemeId::Origins;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, p.card);
    ImGui::PushStyleColor(ImGuiCol_Border, warn ? ImVec4(colors::Warn.x, colors::Warn.y, colors::Warn.z, 0.55f) : p.border);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, origins ? 0.0f : em * 0.4f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, origins || warn ? 1.0f : 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em * 1.0f, em * 0.75f));
    ImGuiChildFlags cflags = ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened;
    if (origins || warn) cflags |= ImGuiChildFlags_Borders;
    const bool open = ImGui::BeginChild(id.c_str(), ImVec2(0, 0), cflags, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();  // border colour for the card only, not for buttons inside
    if (!open) return false;
    if (warn) {
        InlineIcon(Icon::Warning, colors::Warn);
        ImGui::SameLine(0, em * 0.4f);
    }
    if (origins) {
        // White title followed by a thin light line to the card edge (like the lines on the title screen)
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.08f);
        ImGui::TextUnformatted(title);
        ImGui::PopFont();
        const ImVec2 a = ImGui::GetItemRectMax();
        const float y = (ImGui::GetItemRectMin().y + a.y) * 0.5f;
        const float x1 = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
        if (x1 > a.x + em)
            ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x + em * 0.7f, y), ImVec2(x1, y), ImGui::GetColorU32(p.border), 1.0f);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, warn ? colors::Warn : p.accent);
        ImGui::TextUnformatted(title);
        ImGui::PopStyleColor();
    }
    if (note) {
        ImGui::PushStyleColor(ImGuiCol_Text, warn ? colors::Warn : p.textDim);
        ImGui::TextWrapped("%s", note);
        ImGui::PopStyleColor();
    }
    ImGui::Dummy(ImVec2(0, em * 0.1f));
    if (!ImGui::BeginTable(title, 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoPadOuterX)) return false;
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 0.50f);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.50f);
    return true;
}

void Panel::EndCard(bool tableOpen) {
    if (tableOpen) ImGui::EndTable();
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
}

// One group as a card. For "Progression" with game functions, a separate card for the permanent actions comes first
// (XP, level, ability points): clearly separated from the sliders, which can be reset at any time.
void Panel::DrawGroup(Category cat, std::string_view group, const PanelEnv& env, bool& first) {
    std::vector<const ParamDesc*> items;
    for (const ParamDesc* d : OrderedGroup(cat, group))
        if (Visible(*d)) items.push_back(d);
    const bool unfiltered = !search_[0] && !changedOnly_;
    const bool hasMacro = unfiltered && (group == kGroupTime || group == kGroupSandstorm || group == kGroupCustomFx);
    const bool game = env.game.supported && settings_.gameFunctions;
    const bool hasProgress = unfiltered && group == kGroupProgression && game;
    if (items.empty() && !hasMacro && !hasProgress) return;

    const std::string title(group);
    const std::string cardId = "card_" + std::to_string(static_cast<int>(cat)) + "_" + title;
    if (unfiltered && group == "World Map" && game && settings_.teleport) DrawTeleportCard(env, first);
    if (unfiltered && group == "Movement" && game) DrawSpeedCard(env);
    if (hasProgress) {
        const bool t = BeginCard(cardId + "_actions", "Progress actions", true,
                                 "Permanent in this save - not covered by Undo. Back up your saves first.");
        if (t) {
            DrawProgressRow(env);
            DrawGrantRows(env);
        }
        EndCard(t);
        if (items.empty()) return;
    }
    const bool t = BeginCard(cardId, group == kGroupSandstorm && unfiltered ? "Weather & Sandstorm" : title.c_str(), false);
    if (t) {
        if (hasMacro) {
            if (group == kGroupTime) {
                if (game) {
                    DrawGameTimeRow(env);
                    DrawSetTimeRows(env);
                }
                DrawDayLengthRow();
                DrawTimeLockRow();
            } else if (group == kGroupSandstorm) {
                DrawWeatherRows(env);
                DrawSandstormQuickRow();
            } else {
                DrawCustomFilterRow(env);
            }
            if (first) {
                ImGui::SetItemDefaultFocus();
                first = false;
            }
        }
        for (const ParamDesc* d : items) {
            DrawParam(*d, env);
            if (first) {
                ImGui::SetItemDefaultFocus();
                first = false;
            }
        }
    }
    EndCard(t);
}
// One parameter row. Right side: control, value, reset icon (only when changed).
void Panel::DrawParam(const ParamDesc& d, const PanelEnv& env) {
    const Palette& p = CurrentPalette();
    const float em = Em();
    const auto i = static_cast<std::size_t>(d.id);
    ParamValue& v = work_[i];
    const bool disabled = IsDisabled(d);
    const bool differs = !(v == d.def);
    const bool outOfRange = !InUiRange(d, v);
    const bool staged = staged_[i].has_value();

    ImGui::PushID(static_cast<int>(i));
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    {
        // Marker at the left card edge: changed (accent) or waiting for "Apply now" (warning colour)
        if (differs || staged) {
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const float x = pos.x - em * 0.55f;
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(x, pos.y + 2.0f), ImVec2(x + 3.0f, pos.y + ImGui::GetFrameHeight() - 2.0f),
                                                      ImGui::GetColorU32(staged ? colors::Warn : p.accent), 1.5f);
        }
    }
    ImGui::AlignTextToFramePadding();
    if (outOfRange) {
        InlineIcon(Icon::Warning, colors::Warn, true);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Value is outside the game's range.");
        ImGui::SameLine(0, em * 0.35f);
    }
    const ImVec4 labelColor = disabled ? p.textDim : p.text;
    const std::string_view label = DisplayLabel(d);
    {
        // Truncate long labels with "..." (full name in the tooltip)
        const float reserve = d.reloadsWorld ? ImGui::GetTextLineHeight() + em * 0.4f : 0.0f;
        const float maxW = ImGui::GetContentRegionAvail().x - reserve - em * 0.3f;
        std::string shown(label);
        if (ImGui::CalcTextSize(shown.c_str()).x > maxW) {
            std::size_t lo = 0, hi = shown.size();
            while (lo < hi) {  // longest prefix that fits with "..."
                const std::size_t mid = (lo + hi + 1) / 2;
                if (ImGui::CalcTextSize((std::string(label.substr(0, mid)) + "...").c_str()).x <= maxW) lo = mid;
                else hi = mid - 1;
            }
            shown = std::string(label.substr(0, lo));
            while (!shown.empty() && shown.back() == ' ') shown.pop_back();
            shown += "...";
        }
        ImGui::TextColored(labelColor, "%s", shown.c_str());
    }
    if (ImGui::IsItemHovered()) {
        std::string tip;
        tip += std::string(d.label) + "\n\n";
        if (!d.hint.empty()) tip += std::string(d.hint) + "\n\n";
        tip += std::string(d.key) + "   (default: " + ValueText(d, d.def) + ")";
        if (outOfRange) tip += "\nValue is outside the game's range.";
        if (haveSnap_ && !snap_.present[i] && snap_.status != SyncStatus::NotFound) tip += "\nNot present in ACP.ini.";
        if (disabled) tip += d.local && !settings_.customFilter ? "\nEnable the custom filter first." : "\nDisabled by a related switch.";
        ImGui::SetTooltip("%s", tip.c_str());
    }
    if (d.reloadsWorld) {
        ImGui::SameLine(0, em * 0.4f);
        InlineIcon(Icon::Reload, p.textDim, true);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The game reloads the world to apply this value.\nChanges wait for 'Apply now'.");
    }

    ImGui::TableSetColumnIndex(1);
    const float cellX = ImGui::GetCursorPosX();
    const float avail = ImGui::GetContentRegionAvail().x;
    const float resetW = ImGui::GetFrameHeight();
    const float sp = ImGui::GetStyle().ItemInnerSpacing.x;
    const float valueW = em * 3.3f;
    const ImVec4 valueColor = differs ? p.accent : p.textDim;
    bool controlFocused = false;

    ImGui::BeginDisabled(disabled);
    const ImGuiSliderFlags sflags = settings_.expert ? ImGuiSliderFlags_None : ImGuiSliderFlags_AlwaysClamp;
    auto trackSlider = [&]() {
        if (ImGui::IsItemActive()) activeEdit_ = d.id;
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            Commit(d, v, env);
            activeEdit_.reset();
        } else if (ImGui::IsItemDeactivated() && activeEdit_ == d.id) {
            activeEdit_.reset();
        }
    };
    auto valueText = [&](const std::string& text) {
        ImGui::SameLine(0, sp);
        const float x = ImGui::GetCursorPosX();
        ImGui::SetCursorPosX(x + valueW - ImGui::CalcTextSize(text.c_str()).x);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(valueColor, "%s", text.c_str());
    };
    switch (d.type) {
        case ParamType::Bool: {
            bool b = v.b;
            ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
            bool toggled = ToggleSwitch("##v", &b, false);
            ImGui::PopStyleColor();
            controlFocused = ImGui::IsItemFocused();
            toggled |= PadToggle(&b);
            if (toggled) Commit(d, ParamValue::Bool(b), env);
            ImGui::SameLine(0, sp * 1.5f);
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(valueColor, "%s", v.b ? "On" : "Off");
            break;
        }
        case ParamType::Int: {
            if (d.unit == Unit::Enum) {
                ImGui::SetNextItemWidth(avail - resetW - sp);
                ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
                const bool open = ComboBox("##v", EnumLabel(d, v.i).c_str());
                ImGui::PopStyleColor();
                if (open) {
                    for (int n = d.uiMin.i; n <= d.uiMax.i; ++n) {
                        if (ImGui::Selectable(EnumLabel(d, n).c_str(), n == v.i)) Commit(d, ParamValue::Int(n), env);
                        if (n == v.i) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                } else {
                    controlFocused = ImGui::IsItemFocused();
                    if (PadRowFocus() && g_pad.adjust != 0) {  // left/right: previous/next option
                        const int n = std::clamp(v.i + g_pad.adjust, d.uiMin.i, d.uiMax.i);
                        if (n != v.i) Commit(d, ParamValue::Int(n), env);
                    }
                }
            } else {
                int x = v.i;
                ImGui::SetNextItemWidth(avail - valueW - resetW - sp * 2.0f);
                ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
                if (ValueSlider("##v", ImGuiDataType_S32, &x, &d.uiMin.i, &d.uiMax.i, SliderFormat(d), sflags))
                    v = ParamValue::Int(x);
                ImGui::PopStyleColor();
                trackSlider();
                controlFocused = ImGui::IsItemFocused() && !ImGui::IsItemActive();
                if (PadRowFocus() && g_pad.adjust != 0) {
                    const int n = static_cast<int>(PadStepValue(v.i, d.uiMin.i, d.uiMax.i, g_pad.adjust, true));
                    if (n != v.i) Commit(d, ParamValue::Int(n), env);
                }
                valueText(ValueText(d, v));
            }
            break;
        }
        case ParamType::Float: {
            float x = v.f;
            ImGui::SetNextItemWidth(avail - valueW - resetW - sp * 2.0f);
            ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
            if (ValueSlider("##v", ImGuiDataType_Float, &x, &d.uiMin.f, &d.uiMax.f, SliderFormat(d), sflags))
                v = ParamValue::Float(x);
            ImGui::PopStyleColor();
            trackSlider();
            controlFocused = ImGui::IsItemFocused() && !ImGui::IsItemActive();
            if (PadRowFocus() && g_pad.adjust != 0) {
                const float n = static_cast<float>(PadStepValue(v.f, d.uiMin.f, d.uiMax.f, g_pad.adjust, false));
                if (n != v.f) Commit(d, ParamValue::Float(n), env);
            }
            valueText(ValueText(d, v));
            break;
        }
    }
    ImGui::EndDisabled();

    // Reset: icon on the right (mouse), or X on the controller / Delete on the focused item
    const bool canReset = differs && !disabled;
    if (canReset && controlFocused &&
        (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false) || ImGui::IsKeyPressed(ImGuiKey_Delete, false)))
        Commit(d, d.def, env);
    ImGui::SameLine(0, 0);
    ImGui::SetCursorPosX(cellX + avail - resetW);
    if (canReset) {
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
        std::string tip = "Reset to default (" + ValueText(d, d.def) + ")\nController: X on the value";
        if (IconButton("##reset", Icon::Reset, tip.c_str())) Commit(d, d.def, env);
        ImGui::PopItemFlag();
    } else {
        ImGui::Dummy(ImVec2(resetW, ImGui::GetFrameHeight()));
    }
    if (d.id == ParamId::PlayableCharacterIndex && env.portrait) DrawCharacterPreview(d, env);
    ImGui::PopID();
}

namespace {
// Portrait framed in the theme's style: image, soft fade into the card colour, frame (Origins: thin gold line with
// diamond and corner brackets; Uplay: outline with accent line at the bottom). selected/hot: selected / hovered.
void DrawPortrait(ImDrawList* dl, ImTextureID tex, ImVec2 a, ImVec2 b, bool selected, bool hot, float fade,
                  ImTextureID backdrop = ImTextureID{}) {
    const Palette& p = CurrentPalette();
    const bool origins = CurrentTheme() == ThemeId::Origins;
    const float r = origins ? 0.0f : 4.0f;
    const ImU32 card = ImGui::GetColorU32(p.card);
    const ImVec4 c = p.card;
    const ImU32 clear = ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, 0.0f));
    const float h = b.y - a.y, w = b.x - a.x;
    // Theme-styled backdrop (characters are cut out): dark base, accent-coloured glow behind the
    // character, Origins: fine diamonds; Uplay: fine grid lines; floor line and soft shadow under the feet.
    const ImVec4 ac = p.accent;
    const ImVec4 top = origins ? ImVec4(0.055f, 0.092f, 0.098f, 1.0f) : ImVec4(0.125f, 0.140f, 0.170f, 1.0f);
    const ImVec4 bot = origins ? ImVec4(0.028f, 0.050f, 0.054f, 1.0f) : ImVec4(0.070f, 0.080f, 0.098f, 1.0f);
    dl->AddRectFilledMultiColor(a, b, ImGui::GetColorU32(top), ImGui::GetColorU32(top), ImGui::GetColorU32(bot),
                                ImGui::GetColorU32(bot));
    dl->PushClipRect(a, b, true);
    const ImVec2 gc((a.x + b.x) * 0.5f, a.y + h * 0.42f);
    const float glow = (selected || hot) ? 1.35f : 1.0f;
    for (int i = 0; i < 14 && !(origins && backdrop); ++i) {  // soft glow: many low-opacity circles
        const float t = 1.0f - static_cast<float>(i) / 14.0f;
        const ImVec4 gcol = origins ? ImVec4(ac.x, ac.y, ac.z, 0.022f * glow) : ImVec4(0.75f, 0.85f, 1.0f, 0.010f * glow);
        dl->AddCircleFilled(gc, w * (0.12f + 0.55f * t), ImGui::GetColorU32(gcol), 48);
    }
    if (origins && backdrop) {  // banner-style backdrop (hieroglyph wall, gold dust) as an image
        dl->AddImage(backdrop, a, b, ImVec2(0, 0), ImVec2(1, 1), selected || hot ? IM_COL32_WHITE : IM_COL32(215, 215, 215, 255));
    } else if (origins) {
        for (int k = 0; k < 2; ++k) {
            const float rr = w * (k == 0 ? 0.40f : 0.32f);
            const ImU32 lc = ImGui::GetColorU32(ImVec4(ac.x, ac.y, ac.z, k == 0 ? 0.20f : 0.10f));
            dl->AddQuad(ImVec2(gc.x, gc.y - rr * 1.25f), ImVec2(gc.x + rr, gc.y), ImVec2(gc.x, gc.y + rr * 1.25f),
                        ImVec2(gc.x - rr, gc.y), lc, 1.2f);
        }
    }
    const float floorY = a.y + h * 0.955f;
    dl->AddLine(ImVec2(a.x + w * 0.12f, floorY), ImVec2(b.x - w * 0.12f, floorY),
                ImGui::GetColorU32(ImVec4(ac.x, ac.y, ac.z, origins ? 0.30f : 0.0f)), 1.0f);
    for (int i = 0; i < 4; ++i)  // shadow under the feet
        dl->AddEllipseFilled(ImVec2(gc.x, floorY - h * 0.01f), ImVec2(w * (0.30f - 0.05f * i), h * (0.030f - 0.005f * i)),
                             ImGui::GetColorU32(ImVec4(0, 0, 0, 0.16f)));
    if (tex) dl->AddImage(tex, a, b, ImVec2(0, 0), ImVec2(1, 1), selected || hot ? IM_COL32_WHITE : IM_COL32(232, 232, 232, 255));
    dl->PopClipRect();
    (void)fade;
    (void)card;
    (void)clear;
    if (origins) {
        const ImVec4 lineCol = selected ? p.accent : hot ? p.text : ImVec4(p.accent.x, p.accent.y, p.accent.z, 0.35f);
        dl->AddRect(a, b, ImGui::GetColorU32(lineCol), 0.0f, selected ? 1.6f : 1.0f);
        const float k = std::min(w, h) * 0.08f;  // corner bracket
        const ImU32 gold = ImGui::GetColorU32(ImVec4(p.accent.x, p.accent.y, p.accent.z, selected || hot ? 1.0f : 0.75f));
        const ImVec2 cs[4] = {a, ImVec2(b.x, a.y), b, ImVec2(a.x, b.y)};
        const float sx[4] = {1, -1, -1, 1}, sy[4] = {1, 1, -1, -1};
        for (int i = 0; i < 4; ++i) {
            dl->AddLine(ImVec2(cs[i].x, cs[i].y), ImVec2(cs[i].x + sx[i] * k, cs[i].y), gold, 2.0f);
            dl->AddLine(ImVec2(cs[i].x, cs[i].y), ImVec2(cs[i].x, cs[i].y + sy[i] * k), gold, 2.0f);
        }
        const float dm = k * 0.45f;  // diamond at top centre
        const ImVec2 m((a.x + b.x) * 0.5f, a.y);
        dl->AddQuadFilled(ImVec2(m.x, m.y - dm), ImVec2(m.x + dm, m.y), ImVec2(m.x, m.y + dm), ImVec2(m.x - dm, m.y), card);
        dl->AddQuad(ImVec2(m.x, m.y - dm), ImVec2(m.x + dm, m.y), ImVec2(m.x, m.y + dm), ImVec2(m.x - dm, m.y), gold, 1.4f);
    } else {
        dl->AddRect(a, b, ImGui::GetColorU32(selected ? p.accent : hot ? p.text : p.border), r, selected ? 2.0f : 1.0f);
        if (selected) dl->AddLine(ImVec2(a.x + r, b.y - 1.0f), ImVec2(b.x - r, b.y - 1.0f), ImGui::GetColorU32(p.accent), 3.0f);
    }
}

// Arrow button (chevron) over the portrait; no controller focus (the pad browses via the row above).
bool ChevronButton(const char* id, ImVec2 center, float size, bool left) {
    const Palette& p = CurrentPalette();
    ImGui::SetCursorScreenPos(ImVec2(center.x - size, center.y - size));
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(size * 2, size * 2));
    ImGui::PopItemFlag();
    const bool hot = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddCircleFilled(center, size, ImGui::GetColorU32(ImVec4(0, 0, 0, hot ? 0.65f : 0.40f)));
    dl->AddCircle(center, size, ImGui::GetColorU32(hot ? p.accent : ImVec4(p.text.x, p.text.y, p.text.z, 0.5f)), 0, 1.2f);
    const float s = size * 0.38f, dir = left ? -1.0f : 1.0f;
    const ImVec2 pts[3] = {ImVec2(center.x - dir * s * 0.5f, center.y - s), ImVec2(center.x + dir * s * 0.6f, center.y),
                           ImVec2(center.x - dir * s * 0.5f, center.y + s)};
    dl->AddPolyline(pts, 3, ImGui::GetColorU32(hot ? p.accent : p.text), 2.0f, 0);
    return pressed;
}
}  // namespace

// Character picker: portrait of the selected character with arrows, name and position on the left, and "Show all" (tile overview).
void Panel::DrawCharacterPreview(const ParamDesc& d, const PanelEnv& env) {
    const Palette& p = CurrentPalette();
    const float em = Em();
    const int cur = work_[static_cast<std::size_t>(d.id)].i;
    const int lo = d.uiMin.i, hi = d.uiMax.i, count = hi - lo + 1;
    auto plainName = [&](int n) {
        std::string s = EnumLabel(d, n);
        if (const auto pos = s.find(" (default)"); pos != std::string::npos) s.erase(pos);
        return s;
    };
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::Dummy(ImVec2(0, em * 0.6f));
    ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.35f);
    ImGui::TextColored(p.accent, "%s", plainName(cur).c_str());
    ImGui::PopFont();
    ImGui::TextDisabled("Character %d of %d", cur - lo + 1, count);
    ImGui::Dummy(ImVec2(0, em * 0.4f));
    ImGui::PushStyleColor(ImGuiCol_Text, p.textDim);
    ImGui::TextWrapped("%s", lastInputPad_ ? "Controller: move down to the picture, then press left/right to browse "
                                             "and A to see all characters."
                                           : "Browse with the arrows, or click the picture to see all characters.");
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, em * 0.4f));
    if (ImGui::Button("Show all characters") || openCharGrid_) ImGui::OpenPopup("##chargrid");
    openCharGrid_ = false;

    ImGui::TableSetColumnIndex(1);
    const float avail = ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemInnerSpacing.x;
    const float w = std::min(avail, em * 12.0f), h = w * 4.0f / 3.0f;
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 a(start.x + (avail - w) * 0.5f, start.y + em * 0.6f);
    const ImVec2 b(a.x + w, a.y + h);
    // Portrait as its own item: controller focus (left/right browses, A opens the overview), click opens the overview
    ImGui::SetCursorScreenPos(a);
    ImGui::SetNextItemAllowOverlap();
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));  // own frame instead of the nav frame
    const bool openAll = ImGui::InvisibleButton("##portrait", ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    ImGui::PopStyleColor();
    const bool focused = ImGui::IsItemFocused();
    const bool hovered = ImGui::IsItemHovered();
    portraitFocused_ = focused;
    int next = cur;
    if (focused) {
        g_rowFocused = true;  // left/right belong to the portrait, not navigation
        if (g_pad.adjust < 0) next = cur > lo ? cur - 1 : hi;
        if (g_pad.adjust > 0) next = cur < hi ? cur + 1 : lo;
    }
    if (openAll) ImGui::OpenPopup("##chargrid");
    DrawPortrait(ImGui::GetWindowDrawList(), env.portrait(cur), a, b, focused, hovered, 0.30f, env.portrait(-1));
    // arrows inside the image (not clipped in narrow windows)
    const float arrow = std::min(em * 0.95f, w * 0.11f);
    const float inset = arrow + em * 0.25f;
    if (ChevronButton("##prev", ImVec2(a.x + inset, (a.y + b.y) * 0.5f), arrow, true)) next = cur > lo ? cur - 1 : hi;
    if (ChevronButton("##next", ImVec2(b.x - inset, (a.y + b.y) * 0.5f), arrow, false)) next = cur < hi ? cur + 1 : lo;
    ImGui::SetCursorScreenPos(ImVec2(start.x, b.y + em * 0.25f));
    {
        // control hint below the image: controller glyphs for pad users, plain text for mouse users
        ImGui::SetCursorScreenPos(ImVec2(a.x, b.y + em * 0.25f));
        if (lastInputPad_) {
            ImGui::PushStyleColor(ImGuiCol_TextDisabled, focused ? p.accent : p.textDim);
            PadIcon(Pad::DPad);
            Gap();
            ImGui::TextDisabled("Browse");
            Gap(0.9f);
            PadIcon(Pad::A);
            Gap();
            ImGui::TextDisabled("All characters");
            ImGui::PopStyleColor();
        } else {
            ImGui::TextColored(p.textDim, "Click the picture for all characters");
        }
    }
    ImGui::Dummy(ImVec2(1.0f, em * 0.3f));
    if (next != cur) Commit(d, ParamValue::Int(next), env);

    // centred tile overview (mouse; controller: D-pad + A, B closes)
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em * 1.2f, em * 1.0f));
    const bool gridOpen = ImGui::BeginPopup("##chargrid");
    ImGui::PopStyleVar();
    if (gridOpen) {
        ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.25f);
        ImGui::TextColored(p.accent, "Choose a character");
        ImGui::PopFont();
        ImGui::TextDisabled("Click a portrait or press A. The world reloads after 'Apply now'.");
        ImGui::Dummy(ImVec2(0, em * 0.3f));
        const int cols = 7;
        const float tw = em * 5.6f, th = tw * 4.0f / 3.0f, lh = ImGui::GetTextLineHeight();
        for (int n = lo; n <= hi; ++n) {
            if ((n - lo) % cols) ImGui::SameLine(0, em * 0.5f);
            ImGui::PushID(n);
            const ImVec2 q = ImGui::GetCursorScreenPos();
            const bool clicked = ImGui::InvisibleButton("##tile", ImVec2(tw, th + lh + em * 0.3f), ImGuiButtonFlags_EnableNav);
            const bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
            if (n == cur) ImGui::SetItemDefaultFocus();
            ImDrawList* gl = ImGui::GetWindowDrawList();
            DrawPortrait(gl, env.portrait(n), q, ImVec2(q.x + tw, q.y + th), n == cur, hot, 0.22f, env.portrait(-1));
            std::string tn = plainName(n);
            while (ImGui::CalcTextSize(tn.c_str()).x > tw && tn.size() > 4) tn = tn.substr(0, tn.size() - 4) + "...";
            const float tx = q.x + (tw - ImGui::CalcTextSize(tn.c_str()).x) * 0.5f;
            gl->AddText(ImVec2(tx, q.y + th + em * 0.15f), ImGui::GetColorU32(n == cur ? p.accent : hot ? p.text : p.textDim),
                        tn.c_str());
            if (clicked) {
                if (n != cur) Commit(d, ParamValue::Int(n), env);
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopID();
        }
        ImGui::EndPopup();
    }
}

// Time-of-day quick preset: the chosen phase gets 91 % of the day, the others 3 % each (like DayNightTime).
void Panel::DrawTimeLockRow() {
    constexpr ParamId kDur[] = {ParamId::DayPeriodsMorningDuration, ParamId::DayPeriodsMiddayDuration,
                                ParamId::DayPeriodsAfternoonDuration, ParamId::DayPeriodsNightDuration};
    constexpr const char* kNames[] = {"Off (normal day)", "Morning", "Midday", "Afternoon", "Night"};
    float d[4], total = 0;
    for (int k = 0; k < 4; ++k) total += (d[k] = work_[static_cast<std::size_t>(kDur[k])].f);
    int current = -1;  // -1 = custom values
    bool defaults = true;
    for (int k = 0; k < 4; ++k) defaults &= d[k] == Desc(kDur[k]).def.f;
    if (defaults) current = 0;
    for (int k = 0; k < 4 && current < 0; ++k) {
        bool match = total > 0 && std::fabs(d[k] - total * 0.91f) < total * 0.02f;
        for (int o = 0; o < 4 && match; ++o)
            if (o != k) match = std::fabs(d[o] - total * 0.03f) < total * 0.02f;
        if (match) current = k + 1;
    }
    ImGui::PushID("timelock");
    LabelCell("Lock time of day",
              "Stretches the chosen period to ~91% of the day (the other three get 3% each).\n"
              "Combine with a long day ('Length of the day' = 400%) for an almost fixed time.");
    auto apply = [&](int n) {
        const float sum = 36.0f;  // sum of the default durations (6 + 12 + 6 + 12)
        BeginUndoGroup("Lock time of day");
        for (int k = 0; k < 4; ++k) {
            const ParamDesc& pd = Desc(kDur[k]);
            const float val = n == 0 ? pd.def.f : (k == n - 1 ? sum * 0.91f : sum * 0.03f);
            Submit(pd.id, MakeValue(pd, val));
        }
        EndUndoGroup();
    };
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
    const bool open = ComboBox("##lock", current < 0 ? "Custom durations" : kNames[current]);
    ImGui::PopStyleColor();
    if (open) {
        for (int n = 0; n < 5; ++n)
            if (ImGui::Selectable(kNames[n], n == current)) apply(n);
        ImGui::EndCombo();
    } else if (PadRowFocus() && g_pad.adjust != 0) {
        const int n = std::clamp(current + g_pad.adjust, 0, 4);
        if (n != current) apply(n);
    }
    ImGui::PopID();
}

// Weather (round 7): set/hold intensity via game functions; sandstorm off/soon via the ACP values.
// None of this is permanent: the game doesn't save weather, and ACP values can be reset at any time.
void Panel::DrawWeatherRows(const PanelEnv& env) {
    const GameView& g = env.game;
    ImGui::PushID("weather");
    if (env.game.supported && settings_.gameFunctions) {
        LabelCell("Current weather", "Weather intensity read from the game: 0 % = clear, 100 % = stormy.\n"
                                     "Rain only appears in regions that have rain (e.g. the Nile delta).");
        ImGui::AlignTextToFramePadding();
        if (!g.weatherValid) ImGui::TextDisabled("%s", g.valid ? "not available here" : g.stateText);
        else {
            const int pct = static_cast<int>(g.weather * 100.0f + 0.5f);
            ImGui::Text("%d %%", pct);
            ImGui::SameLine();
            if (g.climateOff) ImGui::TextColored(colors::Warn, "- weather is fixed in this area");
            else if (g.weatherScripted) ImGui::TextColored(colors::Warn, "- controlled by the current mission");
            else if (g.weatherHeld) ImGui::TextColored(CurrentPalette().accent, "- held");
            else ImGui::TextDisabled("- natural");
        }

        LabelCell("Set weather", "Clear (0 %) to stormy (100 %). Changes smoothly over a few seconds and is held\n"
                                 "until you switch 'Hold weather' off. Not saved - loading a save restores natural weather.");
        // While not held: slider follows the current weather (starting point for the user's setting)
        if (!weatherHold_ && g.weatherValid && !ImGui::IsAnyItemActive()) weatherValue_ = static_cast<int>(g.weather * 100.0f + 0.5f);
        const char* mood = weatherValue_ < 20 ? "Clear" : weatherValue_ < 50 ? "Cloudy" : weatherValue_ < 80 ? "Overcast" : "Stormy";
        char text[24];
        std::snprintf(text, sizeof(text), "%s", mood);
        ImGui::BeginDisabled(!g.weatherValid || g.climateOff);
        const SliderState st = IntSliderRow("##wx", &weatherValue_, 0, 100, text);
        if (st.committed) weatherHold_ = true;  // setting = holding

        LabelCell("Hold weather", "On: the weather stays at the value above.\nOff: the game picks its own weather again.");
        bool hold = weatherHold_;
        bool changed = ToggleSwitch("##wxhold", &hold, false);
        changed |= PadToggle(&hold);
        if (changed) weatherHold_ = hold;
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", weatherHold_ ? "On" : "Off");
        ImGui::EndDisabled();
    }

    // Sandstorm via the ACP values (works without game functions too)
    const ParamDesc& prob = Desc(ParamId::SandstormBaseProbability);
    const ParamDesc& cool = Desc(ParamId::SandstormCooldownFactor);
    const float p = work_[static_cast<std::size_t>(prob.id)].f;
    LabelCell("No sandstorms", "Sets the sandstorm probability to its minimum. A storm that is already running ends normally.");
    bool none = p <= prob.uiMin.f;
    bool noneChanged = ToggleSwitch("##nosand", &none, false);
    noneChanged |= PadToggle(&none);
    if (noneChanged) Submit(prob.id, MakeValue(prob, none ? prob.uiMin.f : prob.def.f));
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", none ? "On" : "Off");

    LabelCell("Sandstorm soon", "Maximum probability and shortest cooldown: the game starts the next sandstorm by itself\n"
                                "after the current cooldown (desert regions only). Use 'Reset' on the values to go back.");
    if (ImGui::Button("Make sandstorms likely", ImVec2(-FLT_MIN, 0))) {
        BeginUndoGroup("Sandstorm soon");
        Submit(prob.id, MakeValue(prob, prob.uiMax.f));
        Submit(cool.id, MakeValue(cool, cool.uiMin.i));
        EndUndoGroup();
    }
    ImGui::PopID();
}

// Teleport 8a: shows marker, distance and whether a jump would be allowed (block reason). No jump yet.
void Panel::DrawTeleportCard(const PanelEnv& env, bool& first) {
    (void)first;
    const GameView& g = env.game;
    const bool t = BeginCard("card_teleport", "Teleport (experimental)", true,
                             "Experimental: back up your saves first. Set your own marker on the map (also on map icons) "
                             "or track a quest, then press the button twice. Deep water: you land on the surface.");
    if (t) {
        LabelCell("Status", "Why a teleport is or is not possible right now.");
        ImGui::AlignTextToFramePadding();
        if (!g.tpValid) ImGui::TextDisabled("%s", g.valid ? "not available (see log)" : g.stateText);
        else if (g.tpReady) ImGui::TextColored(colors::Ok, "Ready");
        else {
            ImGui::PushStyleColor(ImGuiCol_Text, colors::Warn);
            ImGui::TextWrapped("%s", g.tpText);
            ImGui::PopStyleColor();
        }

        LabelCell("Marker", "Set a custom marker on the world map (the one you place yourself).");
        ImGui::AlignTextToFramePadding();
        if (!g.tpValid || g.tpSource == 0) ImGui::TextDisabled("none");
        else if (g.tpSource == 1 || (g.tpPosMode != 0 && g.tpPosMode != 3))
            ImGui::TextWrapped("%.0f m away (map: %.0f m)", g.tpDistance, g.tpDistanceXY);
        else {
            ImGui::PushStyleColor(ImGuiCol_Text, colors::Warn);
            ImGui::TextWrapped("set, position not readable yet");
            ImGui::PopStyleColor();
        }

        LabelCell("Range limit", "Increase step by step while testing. Whole map allows up to 30 km (the game's own marker limit).");
        {
            static constexpr const char* kRanges[] = {"300 m", "2 km", "Whole map (experimental)"};
            ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
            const bool open = ComboBox("##tprange", kRanges[settings_.tpRange]);
            ImGui::PopStyleColor();
            if (open) {
                for (int i = 0; i < 3; ++i)
                    if (ImGui::Selectable(kRanges[i], i == settings_.tpRange)) {
                        settings_.tpRange = i;
                        SaveSettings();
                    }
                ImGui::EndCombo();
            } else if (PadRowFocus() && g_pad.adjust != 0) {
                const int n = std::clamp(settings_.tpRange + g_pad.adjust, 0, 2);
                if (n != settings_.tpRange) {
                    settings_.tpRange = n;
                    SaveSettings();
                }
            }
        }

        auto optionRow = [&](const char* label, const char* tip, bool* value, const char* id) {
            LabelCell(label, tip);
            bool v = *value;
            bool changed = ToggleSwitch(id, &v, false);
            changed |= PadToggle(&v);
            if (changed) {
                *value = v;
                SaveSettings();
            }
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", *value ? "On" : "Off");
        };
        optionRow("Ignore combat & travel locks", "Experimental: allows teleporting while in combat and where the game blocks fast travel\n(missions, arenas, special areas). Quest restrictions still apply. Enemies may keep searching\nfor you, and leaving a scripted area early can break that sequence - back up your saves.",
                  &settings_.tpAllowCombat, "##tpcombat");
        optionRow("Allow while mounted", "Experimental: moves your horse or camel together with you, using the game's own mount teleport.\n"
                                         "On a boat you are moved alone - the boat stays where it was.",
                  &settings_.tpAllowMounted, "##tpmounted");
        {
            std::string tip = "On the world map: set your marker, then hold the right mouse button or X (controller) for about 1 s.~"
                              "The jump follows as soon as you close the map. Only counts while the map is open, so aiming and other~"
                              "hold actions in the world are not affected. Same locks as the button, no second press.";
            if (settings_.tpQuickKey) tip += "~Extra key (anywhere): " + HotkeyName(settings_.tpQuickKey) + ".";
            for (char& c : tip)
                if (c == '~') c = '\n';
            optionRow("Quick teleport on the map", tip.c_str(), &settings_.tpQuick, "##tpquick");
        }

        // Jump buttons: first press arms (3 s), second starts; the panel closes (progress shown as a toast)
        const std::uint64_t now = GetTickCount64();
        auto jumpButton = [&](int kind, const char* idle, bool ready, float dist) {
            LabelCell("", nullptr);
            const bool inRange = g.tpMaxDistance > 0 && dist <= g.tpMaxDistance;
            const bool can = g.tpValid && ready && inRange && env.teleportRequest;
            const bool armed = now < tpArmedUntil_ && tpArmedKind_ == kind;
            char label[80];
            if (g.tpValid && ready && !inRange) std::snprintf(label, sizeof(label), "Beyond the range limit (%.0f m)##tp%d", g.tpMaxDistance, kind);
            else std::snprintf(label, sizeof(label), "%s##tp%d", armed ? "Press again to teleport" : idle, kind);
            ImGui::BeginDisabled(!can);
            if (armed) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(colors::Warn.x, colors::Warn.y, colors::Warn.z, 0.35f));
            const bool pressed = ImGui::Button(label, ImVec2(-FLT_MIN, 0));
            if (armed) ImGui::PopStyleColor();
            ImGui::EndDisabled();
            if (pressed && can) {
                if (armed) {
                    tpArmedUntil_ = 0;
                    env.teleportRequest(kind);
                    requestClose_ = true;
                } else {
                    tpArmedUntil_ = now + 3000;
                    tpArmedKind_ = kind;
                }
            }
        };
        jumpButton(0, "Teleport to marker", g.tpReady, g.tpDistance);

        if (g.questAvailable) {
            LabelCell("Quest target", "The nearest active objective of the quest you track (like the distance shown in the HUD).\n"
                                      "You land right at the objective - also inside tombs or next to a moving person.\n"
                                      "Skipping the way there can skip scripted events on that way.");
            ImGui::AlignTextToFramePadding();
            if (g.questReady) ImGui::TextWrapped("%.0f m away", g.questDistance);
            else {
                ImGui::PushStyleColor(ImGuiCol_Text, colors::Warn);
                ImGui::TextWrapped("%s", g.questText);
                ImGui::PopStyleColor();
            }
            jumpButton(1, "Teleport to quest target", g.questReady, g.questDistance);
        }

        LabelCell("Diagnostics", "For the test report: which internal source the marker came from.");
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("src %d / flag %d / mod %d / pos %d / pl %d / cb %d / q %d/%d", g.tpSource, g.tpM6A ? 1 : 0,
                            g.tpModuleSet ? 1 : 0, g.tpPosMode, g.tpPlayerBits, g.tpCombatDiag, g.questUsable, g.questIcons);
    }
    EndCard(t);
}

// Round 8 (experimental): time scale of the player character while surface swimming and of the player's mount.
// Only while the overlay runs; reset on dismount/leaving the water/disabling, never saved.
void Panel::DrawSpeedCard(const PanelEnv& env) {
    const GameView& g = env.game;
    const bool t = BeginCard("card_speed", "Extra speed (experimental)", true,
                             "Plays the animations of only your character (surface swimming) or only your own horse/camel faster. "
                             "It visibly looks fast-forwarded. Not stored in the save; 100 % = off.");
    if (t) {
        auto row = [&](const char* label, const char* tip, const char* id, int* v) {
            LabelCell(label, tip);
            char text[16];
            if (*v == 100) std::snprintf(text, sizeof(text), "Off");
            else std::snprintf(text, sizeof(text), "%d %%", *v);
            ImGui::BeginDisabled(!g.speedAvailable);
            // Expert mode: up to 400 % (our own slider, not limited by the game); otherwise up to 200 %
            const int hi = settings_.expert ? 400 : 200;
            if (*v > hi) *v = hi;
            const SliderState st = IntSliderRow(id, v, 100, hi, text);
            ImGui::EndDisabled();
            if (st.committed) SaveSettings();
        };
        row("Surface swimming", "Speeds up your character only while swimming at the surface\n"
                                "(the ACP multipliers only work on land and under water).",
            "##swimboost", &settings_.swimBoost);
        row("Own horse / camel", "Speeds up only the animal you are riding, in all gaits.\n"
                                 "Reset to normal as soon as you dismount.",
            "##mountboost", &settings_.mountBoost);
        if (!g.speedAvailable) {
            LabelCell("", nullptr);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", g.valid ? "not available for this game version (see log)" : g.stateText);
        }
    }
    EndCard(t);
}

// Sandstorm damage quick slider: sets player and NPC damage to v % of their maximum (like DamageInSandstorm).
void Panel::DrawSandstormQuickRow() {
    constexpr ParamId kEnabled[] = {ParamId::PlayerDamageInSandstormEnabled, ParamId::NPCDamageInSandstormEnabled};
    constexpr ParamId kValues[] = {ParamId::PlayerDamageInSandstormInterval, ParamId::PlayerDamageInSandstormLightDPS,
                                   ParamId::PlayerDamageInSandstormHeavyDPS, ParamId::PlayerDamageInSandstormInsideDPS,
                                   ParamId::NPCDamageInSandstormInterval,    ParamId::NPCDamageInSandstormLightDPS,
                                   ParamId::NPCDamageInSandstormHeavyDPS,    ParamId::NPCDamageInSandstormInsideDPS};
    const bool on = work_[static_cast<std::size_t>(kEnabled[0])].b || work_[static_cast<std::size_t>(kEnabled[1])].b;
    const ParamDesc& heavy = Desc(ParamId::PlayerDamageInSandstormHeavyDPS);
    int shown = sandQuick_ >= 0
                    ? sandQuick_
                    : (on ? static_cast<int>(std::lround(work_[static_cast<std::size_t>(heavy.id)].i * 100.0 / heavy.uiMax.i)) : 0);
    ImGui::PushID("sandquick");
    LabelCell("Sandstorm damage (quick)",
              "0 = no sandstorm damage. Otherwise player and NPC damage are enabled and all eight\n"
              "sandstorm damage values are set to this percentage of their maximum.");
    char sqText[16];
    if (shown == 0) std::snprintf(sqText, sizeof(sqText), "Off");
    else std::snprintf(sqText, sizeof(sqText), "%d %%", shown);
    const SliderState sq = IntSliderRow("##sq", &shown, 0, 100, sqText);
    if (sq.changed) sandQuick_ = shown;
    if (sq.committed) {
        const double f = shown / 100.0;
        BeginUndoGroup("Sandstorm damage (quick)");
        for (ParamId e : kEnabled) Submit(e, ParamValue::Bool(shown > 0));
        if (shown > 0) {
            for (ParamId id : kValues) {
                const ParamDesc& pd = Desc(id);
                const double max = pd.type == ParamType::Float ? pd.uiMax.f : pd.uiMax.i;
                Submit(id, MakeValue(pd, max * f));
            }
        }
        EndUndoGroup();
        sandQuick_ = -1;
    } else if (sq.released) {
        sandQuick_ = -1;
    }
    ImGui::PopID();
}

// Computed day length: phase i lasts D[i] * M/100 real-time minutes (higher modifier = slower).
void Panel::DrawDayLengthRow() {
    const float m = static_cast<float>(work_[static_cast<std::size_t>(ParamId::DayPeriodTimeModifier)].i) / 100.0f;
    float minutes = 0;
    for (ParamId id : {ParamId::DayPeriodsMorningDuration, ParamId::DayPeriodsMiddayDuration,
                       ParamId::DayPeriodsAfternoonDuration, ParamId::DayPeriodsNightDuration})
        minutes += work_[static_cast<std::size_t>(id)].f * m;
    ImGui::PushID("daylen");
    LabelCell("Resulting day length",
              "Computed from the values below: each period lasts 'duration x length of the day %' real-time minutes.\n"
              "Morning 4-8 h, Midday 8-16 h, Afternoon 16-20 h, Night 20-4 h. Default: 36 minutes per game day.\n"
              "The game pauses time while the camera is still (up to 15 s) and during some quests.");
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%.1f min / day", minutes);
    ImGui::AlignTextToFramePadding();
    if (minutes < 2.0f) ImGui::TextColored(colors::Warn, "%s", buf);
    else ImGui::TextUnformatted(buf);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("1 game hour = %.0f s real time", minutes * 60.0f / 24.0f);
    ImGui::PopID();
}

void Panel::DrawGameTimeRow(const PanelEnv& env) {
    const GameView& g = env.game;
    ImGui::PushID("gametime");
    LabelCell("Game time", "Read live from the game (game functions).");
    ImGui::AlignTextToFramePadding();
    if (!g.valid) {
        ImGui::TextDisabled("%s", g.stateText);
    } else {
        const int h = static_cast<int>(g.hours) % 24;
        const int min = static_cast<int>((g.hours - static_cast<float>(static_cast<int>(g.hours))) * 60.0f);
        ImGui::Text("%02d:%02d", h, min);
        ImGui::SameLine();
        // State next to the time: wrap instead of clipping at the column edge
        const ImVec4 col = g.holding || g.ownClock ? CurrentPalette().accent
                           : g.timeRunning         ? ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled)
                                                   : colors::Warn;
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        if (g.holding) ImGui::TextWrapped("- held");
        else if (g.ownClock) ImGui::TextWrapped("- driven by ACP Reloaded (your day length)");
        else ImGui::TextWrapped("- %s", g.timeText);
        ImGui::PopStyleColor();
        if (g.acpTimeBlocked && !g.holding && !g.ownClock) {
            ImGui::PushStyleColor(ImGuiCol_Text, colors::Warn);  // wrap instead of clipping at the window edge
            ImGui::TextWrapped("Normal speed: the current quest/script ignores your day length values.");
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The game uses its default day length while this script runs and returns to your\n"
                                  "values when it ends. Switch on 'Own time-lapse clock' below to use your values anyway.");
        }
        if (g.displayOverride) ImGui::TextDisabled("(a script overrides the shown time)");
    }
    ImGui::PopID();
}

namespace {
void FormatQuarter(int q, char* buf, std::size_t n) { std::snprintf(buf, n, "%02d:%02d", q / 4, (q % 4) * 15); }
}  // namespace

// Stage 7b: set the exact time of day (quarter-hour grid) and optionally hold it.
void Panel::DrawSetTimeRows(const PanelEnv& env) {
    if (!env.gameAction) return;
    const bool ready = env.game.valid;
    ImGui::PushID("settime");
    LabelCell("Set time of day",
              "Sets the game clock directly (game function). Works in every area, also while a quest\n"
              "ignores your day length values. Applied when you release the slider.");
    char fmt[16];
    FormatQuarter(setTimeQuarter_, fmt, sizeof(fmt));
    ImGui::BeginDisabled(!ready);
    const SliderState ts = IntSliderRow("##t", &setTimeQuarter_, 0, 95, fmt);
    FormatQuarter(setTimeQuarter_, fmt, sizeof(fmt));
    const float hours = static_cast<float>(setTimeQuarter_) / 4.0f;
    if (ts.committed) {
        env.gameAction(GameAction::SetTime, 0, hours, 100.0f);
        if (holdTime_ && env.holdTime) env.holdTime(true, hours);
    }
    ImGui::EndDisabled();

    LabelCell("Hold this time",
              "Keeps the clock at the chosen time (re-applied when it drifts by more than one game minute).\n"
              "Lasts until you switch it off or leave the game.");
    ImGui::BeginDisabled(!ready && !holdTime_);
    bool holdChanged = ToggleSwitch("##hold", &holdTime_);
    holdChanged |= PadToggle(&holdTime_);
    if (holdChanged) {
        if (holdTime_) env.gameAction(GameAction::SetTime, 0, hours, 100.0f);
        if (env.holdTime) env.holdTime(holdTime_, hours);
    }
    ImGui::EndDisabled();

    LabelCell("Own time-lapse clock",
              "Some story scripts make the game ignore your day length values (it then runs at normal speed).\n"
              "While that happens, ACP Reloaded advances the clock itself at the speed of your values below.\n"
              "Without such a script the game handles the time on its own.");
    bool clockChanged = ToggleSwitch("##clock", &settings_.ownClock);
    clockChanged |= PadToggle(&settings_.ownClock);
    if (clockChanged) SaveSettings();
    if (!env.game.resultText.empty() && env.game.resultIsTime) {
        LabelCell("", nullptr);
        ImGui::TextDisabled("%s", env.game.resultText.c_str());
    }
    ImGui::PopID();
}

// Stage 7c: grant XP, levels, ability points (via the game's own functions).
void Panel::DrawGrantRows(const PanelEnv& env) {
    if (!env.gameAction) return;
    const GameView& g = env.game;
    const bool ready = g.valid && g.hasPlayer;
    const float mult = static_cast<float>(work_[static_cast<std::size_t>(ParamId::PlayerExperienceGainingMultiplier)].i);
    const bool capOn = work_[static_cast<std::size_t>(ParamId::PlayerMaxLevelEnabled)].b;
    const int cap = work_[static_cast<std::size_t>(ParamId::PlayerMaxLevel)].i;
    ImGui::PushID("grant");
    ImGui::BeginDisabled(!ready);

    // Buttons in a row share the width evenly (never clipped, even in narrow windows).
    auto buttonRow = [&](const char* const* labels, int n) {
        const float sp = ImGui::GetStyle().ItemSpacing.x;
        const float w = std::max(Em() * 2.5f, (ImGui::GetContentRegionAvail().x - sp * static_cast<float>(n - 1)) / static_cast<float>(n));
        int clicked = -1;
        for (int i = 0; i < n; ++i) {
            if (i) ImGui::SameLine();
            if (ImGui::Button(labels[i], ImVec2(w, 0))) clicked = i;
        }
        return clicked;
    };

    LabelCell("Back up saves first", "Copies your game saves to ACP_Reloaded\\SaveBackups before you change your progress.");
    {
        const char* const labels[] = {"Back up now"};
        if (buttonRow(labels, 1) == 0) sync_.BackupGameSaves();
        if (haveSnap_ && snap_.message.rfind("Game saves", 0) == 0) {
            LabelCell("", nullptr);
            ImGui::TextDisabled("%s", snap_.message.c_str());
        }
    }

    LabelCell("Grant XP",
              "Adds experience through the game's own XP function (level-ups and their ability points included).\n"
              "Your 'XP gain multiplier' is compensated, so you get exactly the shown amount.");
    {
        const char* const labels[] = {"+1k", "+10k", "+100k"};
        static constexpr int kXp[] = {1000, 10000, 100000};
        const int c = buttonRow(labels, 3);
        if (c >= 0) env.gameAction(GameAction::GrantXp, kXp[c], 0.0f, mult);
    }

    LabelCell("Raise level to",
              "Choose the level (left/right on the controller), then confirm below.\n"
              "Grants exactly the XP needed (levels can only go up); the game hands out the ability points itself.");
    char lvlText[8];
    std::snprintf(lvlText, sizeof(lvlText), "%d", levelTarget_);
    IntSliderRow("##lvl", &levelTarget_, 1, 40, lvlText);
    // Separate button on the next line: reachable with "down" on the controller (left/right changes the level)
    LabelCell("", nullptr);
    {
        char label[32];
        std::snprintf(label, sizeof(label), "Raise to level %d", levelTarget_);
        const char* const labels[] = {label};
        if (buttonRow(labels, 1) == 0) env.gameAction(GameAction::LevelAtLeast, levelTarget_, 0.0f, mult);
    }

    LabelCell("Grant ability points", "Adds free ability points (game function). Spend them in the game's ability menu.");
    {
        const char* const labels[] = {"+1", "+5", "+10"};
        static constexpr int kPts[] = {1, 5, 10};
        const int c = buttonRow(labels, 3);
        if (c >= 0) env.gameAction(GameAction::GrantPoints, kPts[c], 0.0f, mult);
    }    ImGui::EndDisabled();

    if (ready && (mult <= 0.0f || (capOn && g.level >= static_cast<std::uint32_t>(cap)))) {
        LabelCell("", nullptr);
        if (mult <= 0.0f) ImGui::TextColored(colors::Warn, "XP gain multiplier is 0 %% - XP and levels cannot be granted.");
        else ImGui::TextColored(colors::Warn, "Max level cap (%d) reached - the game grants no more XP.", cap);
    }
    if (!g.resultText.empty() && !g.resultIsTime) {
        LabelCell("", nullptr);
        ImGui::TextWrapped("%s", g.resultText.c_str());
    }
    ImGui::PopID();
}

void Panel::DrawProgressRow(const PanelEnv& env) {
    const GameView& g = env.game;
    ImGui::PushID("progress");
    LabelCell("Current progress", "Read live from the game (game functions).");
    ImGui::AlignTextToFramePadding();
    if (!g.valid) ImGui::TextDisabled("%s", g.stateText);
    else if (!g.hasPlayer) ImGui::TextDisabled("no player loaded");
    else ImGui::TextWrapped("Level %u, %u XP, %u ability point%s", g.level, g.xp, g.abilityPoints, g.abilityPoints == 1 ? "" : "s");
    ImGui::PopID();
}

void Panel::DrawCustomFilterRow(const PanelEnv& env) {
    ImGui::PushID("customfx");
    LabelCell("Enable custom filter",
              "These six values are ignored by the current game version.\n"
              "ACP Reloaded renders them itself on top of the game image (in-game overlay only).");
    bool on = settings_.customFilter;
    if (ToggleSwitch("##cf", &on)) {
        settings_.customFilter = on;
        SaveSettings();
    }
    if (!env.customFilterAvailable) {
        ImGui::SameLine();
        ImGui::TextDisabled("(in-game only)");
    }
    ImGui::PopID();
}

// Settings: frame rate limit, FPS counter and frame time graph (in-game overlay only).
void Panel::DrawPerformanceSection(const PanelEnv& env) {
    const Palette& p = CurrentPalette();
    const float em = Em();
    SectionTitle("Performance & image");
    if (!env.overlay) ImGui::TextDisabled("These options take effect in the in-game overlay.");

    // Sharpening (applies to the game image, not the panel)
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Sharpening");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Contrast-adaptive sharpening against the soft look of the game's anti-aliasing.\n"
                          "Strong on soft edges, gentle on hard ones (no halos). 0 = off.");
    ImGui::SameLine(em * 11.0f);
    ImGui::SetNextItemWidth(em * 11.0f);
    {
        const int lo = 0, hi = 100;
        ValueSlider("##sharpen", ImGuiDataType_S32, &settings_.sharpen, &lo, &hi, "%d", ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemDeactivatedAfterEdit() | PadAdjustInt(&settings_.sharpen, lo, hi)) SaveSettings();
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        if (settings_.sharpen == 0) ImGui::TextDisabled("Off");
        else ImGui::Text("%d %%", settings_.sharpen);
    }

    static constexpr int kSteps[] = {0, 30, 40, 45, 50, 60, 72, 75, 90, 100, 120, 144, 165, 180, 240};
    const bool isStep = !settings_.fpsCustom && std::find(std::begin(kSteps), std::end(kSteps), settings_.fpsLimit) != std::end(kSteps);
    char preview[32];
    if (settings_.fpsLimit == 0) std::snprintf(preview, sizeof(preview), "Off");
    else if (isStep) std::snprintf(preview, sizeof(preview), "%d FPS", settings_.fpsLimit);
    else std::snprintf(preview, sizeof(preview), "Custom (%d FPS)", settings_.fpsLimit);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Frame rate limit");
    ImGui::SameLine(em * 11.0f);
    ImGui::SetNextItemWidth(em * 11.0f);
    if (ComboBox("##fps", preview)) {
        for (int v : kSteps) {
            char label[24];
            if (v == 0) std::snprintf(label, sizeof(label), "Off");
            else std::snprintf(label, sizeof(label), "%d FPS", v);
            if (ImGui::Selectable(label, isStep && v == settings_.fpsLimit)) {
                settings_.fpsLimit = v;
                settings_.fpsCustom = false;
                SaveSettings();
            }
            if (isStep && v == settings_.fpsLimit) ImGui::SetItemDefaultFocus();
        }
        if (ImGui::Selectable("Custom...", !isStep)) {
            if (settings_.fpsLimit == 0) settings_.fpsLimit = 110;
            settings_.fpsCustom = true;
            SaveSettings();
        }
        ImGui::EndCombo();
    }
    if (!isStep) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(em * 10.0f);
        const int lo = 20, hi = 360;
        ValueSlider("##fpscustom", ImGuiDataType_S32, &settings_.fpsLimit, &lo, &hi, "%d", ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemDeactivatedAfterEdit() | PadAdjustInt(&settings_.fpsLimit, lo, hi)) SaveSettings();
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%d FPS", settings_.fpsLimit);
    }
    ImGui::PushStyleColor(ImGuiCol_Text, p.textDim);
    ImGui::TextWrapped("Higher than the game's own limit only works if the in-game frame rate limit is off and VSync is "
                       "disabled (game options, Display).");
    ImGui::PopStyleColor();
    if (LabeledToggle("Limit to 30 FPS while the game is in the background", &settings_.backgroundLimit)) SaveSettings();
    if (LabeledToggle("Show FPS", &settings_.showFps)) SaveSettings();
    if (LabeledToggle("Show frame time graph", &settings_.showFrameGraph)) SaveSettings();
    if (settings_.showFps || settings_.showFrameGraph) {
        static constexpr const char* kCorners[] = {"Top left", "Top right", "Bottom left", "Bottom right"};
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Position");
        ImGui::SameLine(em * 11.0f);
        ImGui::SetNextItemWidth(em * 11.0f);
        if (ComboBox("##corner", kCorners[settings_.statsCorner])) {
            for (int i = 0; i < 4; ++i)
                if (ImGui::Selectable(kCorners[i], i == settings_.statsCorner)) {
                    settings_.statsCorner = i;
                    SaveSettings();
                }
            ImGui::EndCombo();
        }
    }
}

// Settings: send values to the game, restore original values.
void Panel::DrawDataSection(const PanelEnv& env) {
    SectionTitle("Game saves");
    if (ImGui::Button("Back up game saves now")) sync_.BackupGameSaves();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Copies your Assassin's Creed Origins saves (Ubisoft Connect folder) to\n"
                          "Documents\\Assassin's Creed Origins\\ACP_Reloaded\\SaveBackups (last 10 are kept).\n"
                          "Recommended before granting XP, levels or ability points.");
    ImGui::SameLine();
    if (ImGui::Button("Open backup folder")) sync_.OpenSaveBackupFolder();
    if (haveSnap_ && snap_.message.rfind("Game saves", 0) == 0) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", snap_.message.c_str());
    }
    ImGui::PushStyleColor(ImGuiCol_Text, CurrentPalette().textDim);
    ImGui::TextWrapped("To restore: quit the game, then copy the files of a backup back into the save folder.");
    ImGui::PopStyleColor();
    if (env.pushAllToGame) {
        SectionTitle("Game");
        ImGui::BeginDisabled(!env.liveActive);
        if (ImGui::Button("Send all values to the game")) env.pushAllToGame(work_);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("%s", env.liveActive ? "Overwrites the values stored in this 'Hacked Animus' save."
                                                 : "Only available in a 'Hacked Animus' save.");
    }

    SectionTitle("Original values");
    if (ImGui::Button("Restore original values")) ImGui::OpenPopup("Restore original values?");
    if (ImGui::BeginPopupModal("Restore original values?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("All parameters will be set to their original values.");
        if (ImGui::Button("Restore")) {
            staged_ = {};
            RecordAll("Restore original values");
            sync_.ResetToDefaults();
            snapDeferred_ = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::SetItemDefaultFocus();
        ImGui::EndPopup();
    }
}

void Panel::DrawSettings(const PanelEnv& env) {
    const Palette& p = CurrentPalette();
    SectionTitle("Design");
    struct Opt {
        ThemeId id;
        const char* name;
        const char* desc;
    };
    const Opt opts[] = {{ThemeId::UplayClassic, "Uplay Classic", "The original Animus Control Panel look: anthracite and Animus blue."},
                        {ThemeId::Origins, "Origins", "Warm, clean and modern - gold accents in the style of the game."}};
    for (const Opt& o : opts) {
        const bool sel = settings_.theme == o.id;
        if (ImGui::RadioButton(o.name, sel) && !sel) {
            settings_.theme = o.id;
            SaveSettings();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", o.desc);
    }

    SectionTitle("Size");
    ImGui::SetNextItemWidth(Em() * 14);
    const float scaleMin = 0.75f, scaleMax = 1.5f;
    ValueSlider("##uiscale", ImGuiDataType_Float, &scaleEdit_, &scaleMin, &scaleMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    bool scaleDone = ImGui::IsItemDeactivatedAfterEdit();
    if (PadRowFocus() && g_pad.adjust != 0) {  // 5 % steps
        scaleEdit_ = std::clamp(std::round((scaleEdit_ + 0.05f * static_cast<float>(g_pad.adjust)) * 20.0f) / 20.0f, scaleMin, scaleMax);
        scaleDone = true;
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%.0f %%", scaleEdit_ * 100.0f);
    if (scaleDone) {
        settings_.scale = scaleEdit_;
        SaveSettings();
    }

    SectionTitle("Behaviour");
    if (LabeledToggle("Expert mode (wider ranges)", &settings_.expert)) SaveSettings();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Ctrl+click lets you type values outside the usual ranges. Note: the game itself caps every ACP value\n"
                          "at its own limits (e.g. movement speed at 400 %%) - higher values are stored but have no extra effect.\n"
                          "Extra speed (surface swimming, own mount) goes up to 400 %% in expert mode.");
    if (env.customFilterAvailable) {
        if (LabeledToggle("Custom filter (rendered by ACP Reloaded)", &settings_.customFilter)) SaveSettings();
    }
    if (env.overlay) {
        if (LabeledToggle("Open the panel only in a running 'Hacked Animus' save", &settings_.onlyInHackedSave)) SaveSettings();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Outside such a save (main menu, normal saves) the shortcut only shows a short hint.\n"
                              "The panel closes when you leave the save. The standalone editor is not affected.");
    }
    if (env.game.supported) {
        if (LabeledToggle("Game functions (experimental)", &settings_.gameFunctions)) SaveSettings();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Reads the game time and your progress directly from the game - only in a 'Hacked Animus' save.\n"
                              "Also sets the time of day and grants XP, levels and ability points.");
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", settings_.gameFunctions ? env.game.stateText : "off");
        if (settings_.gameFunctions) {
            if (LabeledToggle("Hide the ACP watermark (for screenshots)", &settings_.hideWatermark)) SaveSettings();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Hides the faint ACP text the game shows in a 'Hacked Animus' save - in play and in photo mode.\n"
                                  "Only changes how that text is drawn: achievements, saves and ACP values are not affected.\n"
                                  "Turning it off (or unloading the overlay) brings the text back.");
            if (LabeledToggle("Teleport (experimental): to map marker or quest target", &settings_.teleport)) SaveSettings();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Shows your custom map marker, its distance and whether a teleport would be allowed.\n"
                                  "This test stage does not move the player yet.");
        }
    }

    SectionTitle("Controls");
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Open/close (keyboard):");
    Gap(0.6f);
    if (capturingKey_) {
        ImGui::TextColored(p.accent, "Press the new key combination...  (Esc = cancel)");
        // Record a new combination: first non-modifier key plus held Shift/Ctrl/Alt.
        const ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            capturingKey_ = false;
        } else {
            for (const KeyMap& m : HotkeyKeys()) {
                if (!ImGui::IsKeyPressed(m.key, false)) continue;
                std::uint32_t hk = m.vk;
                if (io.KeyShift) hk |= 1u << 16;
                if (io.KeyCtrl) hk |= 1u << 17;
                if (io.KeyAlt) hk |= 1u << 18;
                settings_.toggleKey = hk;
                capturingKey_ = false;
                SaveSettings();
                break;
            }
        }
    } else {
        HotkeyCaps(settings_.toggleKey, ImGui::GetStyle().FramePadding.y);  // centred on the button row
        Gap(1.0f);
        if (ImGui::Button("Change")) capturingKey_ = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Press any key, optionally with Shift, Ctrl or Alt.\n"
                              "Avoid keys the game uses. Shift+F2 is the original Uplay overlay shortcut.");
        if (settings_.toggleKey != UiSettings::kDefaultToggleKey) {
            ImGui::SameLine();
            if (ImGui::Button("Default")) {
                settings_.toggleKey = UiSettings::kDefaultToggleKey;
                SaveSettings();
            }
        }
        const std::uint32_t vk = settings_.toggleKey & 0xFFFF;
        if ((settings_.toggleKey & 0x70000) == 0 && ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')))
            ImGui::TextColored(colors::Warn, "A single letter or digit is probably also used by the game - consider adding Shift or Ctrl.");
    }

    ImGui::TextDisabled("Open/close (controller):");
    Gap(0.6f);
    ImGui::TextDisabled("hold");
    Gap();
    PadIcon(Pad::View);
    Gap(0.15f);
    ImGui::TextDisabled("+");
    Gap(0.15f);
    PadIcon(Pad::Menu);

    ImGui::TextDisabled("Navigate:");
    Gap(0.6f);
    PadIcon(Pad::DPad);
    Gap(0.15f);
    PadIcon(Pad::LStick);
    HintText("Move");
    PadIcon(Pad::A);
    HintText("Select");
    PadIcon(Pad::B);
    HintText("Back / close");
    PadIcon(Pad::LB);
    Gap(0.15f);
    PadIcon(Pad::RB);
    HintText("Category");
    PadIcon(Pad::LT);
    Gap(0.15f);
    PadIcon(Pad::RT);
    Gap();
    ImGui::TextDisabled("Fine / coarse");

    ImGui::TextDisabled("Unload overlay:");
    Gap(0.6f);
    KeyCap("End");

    DrawPerformanceSection(env);
    DrawDataSection(env);

    SectionTitle("Status");
    ImGui::TextDisabled("Version:");
    ImGui::SameLine();
    ImGui::TextUnformatted(ACP_VERSION_LABEL);
    if (haveSnap_) {
        ImGui::TextDisabled("ACP.ini:");
        ImGui::SameLine();
        ImGui::TextUnformatted(snap_.path.empty() ? "(not found)" : snap_.path.string().c_str());
        ImGui::TextDisabled("Saves this session: %llu   External changes: %llu", static_cast<unsigned long long>(snap_.writes),
                            static_cast<unsigned long long>(snap_.externalChanges));
        if (!snap_.message.empty()) ImGui::TextDisabled("%s", snap_.message.c_str());
    }
    if (env.overlay) {
        ImGui::TextDisabled("Live channel:");
        ImGui::SameLine();
        ImGui::TextColored(env.liveActive ? p.accent : p.textDim, "%s", env.liveHint.c_str());
        ImGui::TextDisabled("Controller blocking for the game: %s", env.padBlockActive ? "active" : "not available");
    }
}

void Panel::DrawFooter(const PanelEnv& env) {
    ImGui::Separator();
    if (lastInputPad_) {
        if (portraitFocused_) {  // character picture selected: A opens the overview
            PadIcon(Pad::A);
            HintText("All characters");
        }
        PadIcon(Pad::LStick);
        Gap(0.15f);
        PadIcon(Pad::DPad);
        HintText("Choose / change value");
        PadIcon(Pad::B);
        HintText(env.overlay ? "Close" : "Back");
        PadIcon(Pad::X);
        HintText("Reset value");
        PadIcon(Pad::Y);
        HintText("Undo");
        PadIcon(Pad::LB);
        Gap(0.15f);
        PadIcon(Pad::RB);
        HintText("Category");
        PadIcon(Pad::LT);
        Gap(0.15f);
        PadIcon(Pad::RT);
        HintText("Fine / coarse");
        if (env.overlay) {
            PadIcon(Pad::View);
            Gap(0.15f);
            ImGui::TextDisabled("+");
            Gap(0.15f);
            PadIcon(Pad::Menu);
            Gap();
            ImGui::TextDisabled("hold: close");
        }
    } else {
        if (env.overlay) {
            HotkeyCaps(settings_.toggleKey);
            HintText("Open / close");
            KeyCap("Esc");
            HintText("Close");
            KeyCap("End");
            HintText("Unload");
        }
        KeyCap("Ctrl");
        Gap(0.15f);
        ImGui::TextDisabled("+");
        Gap(0.15f);
        KeyCap("Z");
        HintText("Undo");
        InlineIcon(Icon::Reset, CurrentPalette().textDim);
        HintText("Reset value");
        InlineIcon(Icon::Reload, CurrentPalette().textDim);
        Gap();
        ImGui::TextDisabled("Game reloads the world");
    }
}

}  // namespace acp::ui
