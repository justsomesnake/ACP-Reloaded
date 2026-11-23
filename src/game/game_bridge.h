#pragma once

// Bridge to the game's own functions (based on private static analysis notes, not included).
// Stage 7a: read (time of day, reason time is blocked, level, XP, ability points).
// Stage 7b/7c: set/hold time of day, grant XP/level/ability points - only through the game's own functions.
// All game access happens exclusively in OnPreStep() (called from the UPLAY_GetNextEvent detour, engine task "PreStep"),
// and only if: feature enabled, checksums ok, game reports ACP active, the game's own gate is true, vtbl checks ok.

#include <cstdint>
#include <string>

namespace acp::game {

enum class BridgeState : std::uint8_t {
    Disabled,      // option off
    WaitingForAcp, // waiting for "Hacked Animus" in the game
    Ready,         // checksums ok, reading
    Unsupported,   // different game build / checksum mismatch
    Failed,        // exception during an access - off for this session
};

enum class TimeStop : std::uint8_t { None, QuestPause, CameraStill, WorldPaused };

// --- Teleport (analysis round 6), stage 8a: read only (marker, distance, blocking reason) ---
enum class TpBlock : std::uint8_t {
    None, Unavailable, NotLocalPlayer, NoMarker, OtherWorld, Loading, WorldNotLoaded, Paused, FastTravelLocked,
    QuestBlocks, InCombat, EagleControl, Mounted, TooFar, TooClose, MarkerUnreadable,
    NoQuestTracked, NoQuestTarget, QuestOtherRegion,
};
enum class TpTarget : std::uint8_t { Marker, Quest };
struct TeleportInfo {
    bool valid = false;           // measurement available (teleport checksums ok, ACP active)
    TpBlock block = TpBlock::Unavailable;
    int markerSource = 0;         // 0 none, 1 UIAfterlifeManager block, 2 CustomMarkerModule only (no position)
    bool blockFlagM6A = false;    // diagnostics: ready flag M+6A
    bool moduleSet = false;       // diagnostics: module reports a marker is set
    int posMode = 0;              // diagnostics, variant 2: 0 none, 1 final position, 2 target position, 3 unreadable
    int playerBits = 0;           // player diagnostics: 1 pointer, 2 world back-link, 4 control object, 8 "local" per game
    bool mounted = false;         // attached/mounted (mount, boat)
    int combatDiag = 0;           // combat detection diagnostics (0..3)
    bool markerEntity = false;    // marker is attached to a character/object (land next to it)
    float target[3] = {0, 0, 0};
    float player[3] = {0, 0, 0};
    float distance = 0;           // 3D in meters
    float distanceXY = 0;
    // Analysis round 8: quest target (nearest active map icon of the tracked quest)
    bool questAvailable = false;  // round 8 functions verified
    TpBlock questBlock = TpBlock::Unavailable;
    float questTarget[3] = {0, 0, 0};
    float questDistance = 0;
    int questIcons = 0;           // diagnostics: quest icons / how many of them are usable
    int questUsable = 0;
    bool questEntity = false;     // target is attached to a character/object (land next to it)
};
const char* TpBlockText(TpBlock b);

// Stage 8b: jump to the marker. The render thread requests, OnPreStep executes (state machine, at most one
// writing game call per PreStep). Test limit: kTpStageMaxDistance.
// Range (render thread, from settings): 300 m, 2 km or the whole map (30 km, the game's limit).
void SetTeleportMaxDistance(float meters);
// Experimental exceptions (render thread, from settings): allow in combat or while mounted.
void SetTeleportOptions(bool allowCombat, bool allowMounted);
// waitForMap: quick teleport via hotkey - if the game is currently paused (map open), wait up to 20 s and
// jump as soon as the map is closed.
void TeleportRequest(TpTarget target = TpTarget::Marker, bool waitForMap = false);
// true while a jump is in progress (jumping, holding, observing, jumping back) - unloading waits for it.
bool TeleportBusy();
// Progress for the on-screen notice: text and whether it should be shown now (running or recently finished).
bool TeleportStatus(char* buf, std::size_t n);

struct GameInfo {
    bool valid = false;        // values come from the current ACP state
    float hours = 0;           // 0..24
    bool displayOverride = false;  // a script displays a different time of day
    TimeStop stop = TimeStop::None;
    bool acpTimeBlocked = false;   // a script makes the game ignore the ACP day lengths (default speed)
    bool holding = false;          // "Hold time of day" active
    bool ownClock = false;         // our own time-lapse clock is currently driving the time
    // Weather (analysis round 7): intensity 0 = clear .. 1 = storm
    bool weatherValid = false;
    float weather = 0;
    bool weatherHeld = false;      // we are holding the weather (randomness paused)
    bool weatherScripted = false;  // a script/mission currently controls the weather
    bool climateOff = false;       // climate system off in this world (weather not controllable)
    bool hasPlayer = false;
    std::uint32_t level = 0;
    std::uint32_t xp = 0;
    std::uint32_t abilityPoints = 0;
    TeleportInfo teleport;
    std::uint64_t tick = 0;    // GetTickCount64 of the measurement
};

// Render thread: apply the option from the settings.
void SetEnabled(bool enabled);
// Render thread: teleport measurement on/off (opt-in, in addition to the game functions).
void SetTeleportEnabled(bool enabled);
// Lifecycle thread: verify the game functions once enabled and ACP is active; log state changes.
void LifecycleTick(bool acpActive, bool anyHost);
// UPLAY_GetNextEvent detour (game task PreStep): the only place with game access. noexcept, no allocation, no log.
void OnPreStep(bool acpActive) noexcept;

BridgeState State();
const char* StateText(BridgeState s);
const char* TimeStopText(TimeStop s);
// Render thread: latest measurement (false if no valid one exists).
bool Read(GameInfo& out);

// --- Stage 7b/7c: actions (render thread queues, OnPreStep executes; at most one per game update) ---
enum class Action : std::uint8_t {
    GrantXp,        // value = XP (before multiplier; compensated with xpMultiplierPct)
    LevelAtLeast,   // value = target level (upwards only, via XP)
    GrantPoints,    // value = ability points
    SetTime,        // hours = time of day 0..24
};
// Queues an action (replaces one not yet executed). Expires after 5 s.
void Request(Action a, int value, float hours, float xpMultiplierPct);
// Hold the time of day (sets it again as soon as it drifts by more than one game minute).
void SetHold(bool on, float hours);
// Own time-lapse clock: only while a script blocks the ACP day lengths, the overlay advances the clock itself
// at the speed of the ACP values. minutes[i] = real-time minutes per phase (morning, noon, afternoon, night),
// already adjusted for "Length of the day".
void SetOwnClock(bool on, const float minutes[4]);
// Weather: hold = keep the weather at intensity (0..1) (transition over seconds), false = hand it back to the game.
// Reconciled in OnPreStep (set again after the world reloads). Not stored in the save game.
void SetWeather(bool hold, float intensity, float seconds);
// true if the game is currently paused (map/menu open); measured in PreStep, at most 0.5 s old.
bool GamePausedNow();
// Analysis round 8 (experimental): time scale only for the player character while swimming on the surface, or only
// for the player's own mount (1.0 = off, 1.0..2.0). OnPreStep sets and resets it (dismount, surfacing, turning off).
void SetSpeedBoost(float swimFactor, float mountFactor);
// true while any factor other than 1 is set (unloading resets it first).
bool SpeedBoostActive();
// Round 8 functions verified and ready (for display).
bool SpeedBoostAvailable();
// Hide the game's ACP watermark text (colour 0); false restores it. Applied in OnPreStep while the game functions run.
void SetHideWatermark(bool hide);
// Unloading: have all factors reset to 1 and accept no new requests.
void ReleaseSpeedBoost();
// Result of the last action (empty text = none yet). seq counts up.
std::string LastResult(std::uint32_t* seq = nullptr, bool* isTime = nullptr);

}  // namespace acp::game
