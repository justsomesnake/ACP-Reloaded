#pragma once

// Animus Control Panel: pure presentation layer over param_table + SyncService.
// Call only on the render thread with an active ImGui context.

#include <cstdio>
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pad_feed.h"
#include "param_table.h"
#include "sync_service.h"
#include "theme.h"

namespace acp::ui {

struct UiSettings {
    ThemeId theme = ThemeId::UplayClassic;
    float scale = 1.0f;          // user factor on top of host scaling
    bool expert = false;         // allow out-of-range values via Ctrl+click
    bool customFilter = false;   // custom image filter (overlay only)
    bool gameFunctions = false;  // game functions (phase 7, opt-in)
    bool ownClock = true;        // own time-lapse clock when a script locks the day lengths
    bool onlyInHackedSave = true;  // open the overlay only in a running "Hacked Animus" save
    int fpsLimit = 0;            // own frame rate limiter (0 = off)
    bool backgroundLimit = false;  // 30 FPS while the game is not the active window
    bool showFps = false;        // FPS counter
    bool showFrameGraph = false; // frame time graph
    int statsCorner = 0;         // 0 top left, 1 top right, 2 bottom left, 3 bottom right
    int sharpen = 0;             // sharpening 0..100 % (own filter pass)
    bool fpsCustom = false;      // FPS limit set freely via slider (even if the value matches a preset step)
    bool teleport = false;       // teleport to map marker (experimental, opt-in)
    int tpRange = 2;             // range: 0 = 300 m, 1 = 2 km, 2 = whole map (30 km, default)
    bool tpAllowCombat = false;  // experimental: also in combat
    bool tpAllowMounted = false; // experimental: mounted (mount and rider)
    bool tpQuick = true;         // quick teleport on the map: hold right mouse button / X (only with teleport enabled)
    std::uint32_t tpQuickKey = 0;        // optional key (same format as toggleKey), 0 = off (F3 and L3+R3 = game's photo mode)
    int swimBoost = 100;         // experimental: time scale of the player character while surface swimming (100..200 %)
    int mountBoost = 100;        // experimental: time scale of the player's mount (100..200 %)
    float TeleportRange() const { return tpRange == 2 ? 30000.0f : tpRange == 1 ? 2000.0f : 300.0f; }  // range in metres for the current setting
    std::uint32_t toggleKey = kDefaultToggleKey;  // VK | Shift<<16 | Ctrl<<17 | Alt<<18
    static constexpr std::uint32_t kDefaultToggleKey = 0x10071;  // Shift+F2 (like the Uplay overlay)
    std::string Serialize() const;
    static UiSettings Parse(std::string_view text);
};

// Game function display state (filled by the overlay; no dependency on the game module).
struct GameView {
    bool supported = false;  // overlay only
    const char* stateText = "";
    bool valid = false;
    float hours = 0;
    const char* timeText = "";
    bool timeRunning = true;
    bool displayOverride = false;
    bool acpTimeBlocked = false;  // script: ACP day lengths are currently ignored
    bool holding = false;         // "Hold time" active
    bool ownClock = false;        // own time-lapse clock is currently driving time
    bool weatherValid = false;    // weather readable
    float weather = 0;            // 0 = clear .. 1 = storm
    bool weatherHeld = false;
    bool weatherScripted = false; // mission/script controls the weather
    bool climateOff = false;
    // Teleport 8a (read only)
    bool tpValid = false;
    bool tpReady = false;          // not blocked
    const char* tpText = "";       // block reason or "ready"
    int tpSource = 0;              // marker source (diagnostics)
    bool tpM6A = false, tpModuleSet = false;
    int tpPosMode = 0, tpPlayerBits = 0, tpCombatDiag = 0;
    float tpDistance = 0, tpDistanceXY = 0;
    float tpMaxDistance = 0;       // test limit for the jump (0 = jump unavailable)
    bool questAvailable = false;   // quest target readable (round 8)
    bool questReady = false;
    const char* questText = "";
    float questDistance = 0;
    int questIcons = 0, questUsable = 0;
    bool speedAvailable = false;   // time scale (round 8) available
    bool hasPlayer = false;
    std::uint32_t level = 0, xp = 0, abilityPoints = 0;
    std::string resultText;       // result of the last action
    bool resultIsTime = false;    // result belongs to "Set time" (otherwise to the progression actions)
};

// Game function actions (values as in acp::game::Action).
enum class GameAction : int { GrantXp = 0, LevelAtLeast = 1, GrantPoints = 2, SetTime = 3 };

struct PanelEnv {
    bool overlay = false;        // in game (B/Esc closes) rather than standalone
    bool fillViewport = false;   // window fills the whole viewport (standalone)
    float baseScale = 1.0f;      // host scaling (DPI/resolution)
    std::string liveHint;        // effect hint for the banner
    bool padBlockActive = false; // overlay: game sees no pad input while the panel is open
    bool liveActive = false;     // overlay: game accepts live values (Hacked Animus, in game)
    bool customFilterAvailable = false;  // overlay can draw the custom image filter
    GameView game;
    // Overlay: send all current values to the game (empty = unavailable)
    std::function<void(const std::array<ParamValue, kParamCount>&)> pushAllToGame;
    // Overlay: queue a game action (value, hours, XP multiplier in %) and hold time
    std::function<void(GameAction, int, float, float)> gameAction;
    std::function<void(bool, float)> holdTime;
    std::function<void(int)> teleportRequest;  // overlay: request a jump (0 = marker, 1 = quest target)
    // Host: portrait of playable character n (0..20, -1 = backdrop) as a texture, 0 = none
    std::function<ImTextureID(int)> portrait;
};

class Panel {
public:
    explicit Panel(SyncService& sync) : sync_(sync) {}

    // Every frame (even with the panel closed): take over the new state.
    void Update();
    // Call when the overlay opens (resets focus/navigation).
    void OnOpened();
    // Draws the panel. closeRequested is set when B/Esc should close the overlay.
    void Draw(const PanelEnv& env, const PadActions& pad, bool* closeRequested);
    // Commit an in-progress slider edit immediately (on close).
    void FlushActiveEdit();
    // Set the start page (0 = presets, 1..5 = categories, 6 = settings).
    void SetPage(int page) { category_ = page >= 0 && page <= 6 ? page : 1; }
    // Test/editor: render as after controller input (footer, single column, visible focus).
    void DebugOpenCharacterGrid() { openCharGrid_ = true; }
    void DebugScroll(float em) { debugScroll_ = em; }  // editor: scroll the page (in em) for screenshots
    void DebugSetSearch(const char* text) { std::snprintf(search_, sizeof(search_), "%s", text); }  // editor: screenshots  // editor: open the tile overview for screenshots
    void ForcePadLayout() { lastInputPad_ = true; }

    const std::array<ParamValue, kParamCount>& Values() const { return work_; }
    const UiSettings& Settings() const { return settings_; }
    // Currently recording a new key combination (overlay: hotkey is suspended meanwhile).
    bool CapturingKey() const { return capturingKey_; }
    // Requested weather for this session (not saved; the game doesn't save weather either)
    bool WeatherHold() const { return weatherHold_; }
    float WeatherTarget() const { return static_cast<float>(weatherValue_) / 100.0f; }
    // Controller: a value row has focus, left/right change the value (for PadFeeder::SetHorizontalCapture).
    bool WantsPadHorizontal() const { return wantsHorizontal_; }

private:
    void ApplySettingsIfNeeded(const PanelEnv& env);
    void SaveSettings();
    void DrawHeader(const PanelEnv& env);
    void DrawSidebar(const PanelEnv& env);
    void DrawToolbar(const PanelEnv& env);
    void DrawPresetControls(float comboW);
    void DrawGroupColumns(Category cat, const std::vector<std::string_view>& groups, const PanelEnv& env, bool& first);
    int GroupWeight(Category cat, std::string_view group, const PanelEnv& env) const;
    void DrawStagedBar();
    void DrawMissingIniBar();
    void DrawCategory(Category cat, const PanelEnv& env);
    void DrawSearchResults(const PanelEnv& env);
    void DrawGroup(Category cat, std::string_view group, const PanelEnv& env, bool& first);
    void DrawParam(const ParamDesc& d, const PanelEnv& env);
    void DrawCharacterPreview(const ParamDesc& d, const PanelEnv& env);
    bool BeginCard(const std::string& id, const char* title, bool warn, const char* note = nullptr);
    void EndCard(bool tableOpen);
    void DrawTimeLockRow();
    void DrawSandstormQuickRow();
    void DrawWeatherRows(const PanelEnv& env);
    void DrawTeleportCard(const PanelEnv& env, bool& first);
    void DrawSpeedCard(const PanelEnv& env);
    void DrawCustomFilterRow(const PanelEnv& env);
    void DrawDayLengthRow();
    void DrawGameTimeRow(const PanelEnv& env);
    void DrawProgressRow(const PanelEnv& env);
    void DrawSetTimeRows(const PanelEnv& env);
    void DrawGrantRows(const PanelEnv& env);
    void DrawDataSection(const PanelEnv& env);
    void DrawPerformanceSection(const PanelEnv& env);
    void DrawSettings(const PanelEnv& env);
    void DrawFooter(const PanelEnv& env);
    void Commit(const ParamDesc& d, ParamValue v, const PanelEnv& env);  // immediately or staged (reload)
    void Submit(ParamId id, ParamValue v, bool record = true);
    // Undo: changes are recorded with the previously committed value (groups = one step).
    void Record(ParamId id, const ParamValue& newValue);
    void BeginUndoGroup(std::string label);
    void EndUndoGroup();
    void RecordAll(std::string label);  // before applying a preset / original values: record all values
    void Undo(const PanelEnv& env);
    bool IsDisabled(const ParamDesc& d) const;
    bool Visible(const ParamDesc& d) const;

    SyncService& sync_;
    Snapshot snap_;
    bool haveSnap_ = false;
    bool snapDeferred_ = false;
    std::array<ParamValue, kParamCount> work_{};
    std::array<std::optional<ParamValue>, kParamCount> localEdit_{};
    std::array<std::uint64_t, kParamCount> localEditTick_{};
    std::array<std::optional<ParamValue>, kParamCount> staged_{};  // waiting for "Apply" (world reload)
    std::optional<ParamId> activeEdit_;

    UiSettings settings_;
    bool settingsLoaded_ = false;
    bool themeDirty_ = true;
    float appliedScale_ = 0.0f;

    int category_ = 1;  // 0 = presets, 1..5 = categories, 6 = settings
    bool focusContent_ = true;
    bool wasAnyItemActive_ = false;
    bool lastInputPad_ = false;
    char search_[64] = {};
    bool changedOnly_ = false;
    int sandQuick_ = -1;  // in-progress edit of the sandstorm quick slider
    int selectedPreset_ = -1;
    char presetName_[64] = {};
    bool presetNameInit_ = false;
    float scaleEdit_ = 1.0f;
    int setTimeQuarter_ = 48;  // time-of-day slider in quarter hours (0..95)
    bool holdTime_ = false;
    int levelTarget_ = 40;
    bool capturingKey_ = false;
    bool wantsHorizontal_ = false;
    bool weatherHold_ = false;
    std::uint64_t tpArmedUntil_ = 0;  // teleport: first press arms for 3 s (second press confirms)
    bool openCharGrid_ = false;
    float debugScroll_ = 0.0f;
    bool popupWasOpen_ = false;     // a popup was open at the end of the previous frame
    bool portraitFocused_ = false;  // character picture has controller focus (footer hint)
    int tpArmedKind_ = 0;             // armed button: 0 marker, 1 quest target
    bool requestClose_ = false;       // close the panel after a teleport starts
    int weatherValue_ = 0;   // last frame: focused value row (console style)
    std::string activePreset_;   // last applied preset (shown in the header)
    int pendingPreset_ = -1;     // selection awaiting confirmation

    struct UndoEntry {
        std::string label;
        std::vector<std::pair<ParamId, ParamValue>> old;  // values before the change
    };
    std::vector<UndoEntry> undo_;
    std::optional<UndoEntry> undoGroup_;
    std::array<ParamValue, kParamCount> committed_{};  // last committed value per parameter
    std::string undoToast_;
    std::uint64_t undoToastUntil_ = 0;
    std::uint64_t lastRecordTick_ = 0;
};

// Name of a hotkey combination ("Shift+F2"); parts separately for keycap icons.
std::string HotkeyName(std::uint32_t hk);

}  // namespace acp::ui
