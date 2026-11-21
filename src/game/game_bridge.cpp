#include "game_bridge.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "platform.h"

namespace acp::game {
namespace {

// --- Table per build (RVA, length, masked byte positions, FNV-1a-64) ---
struct GameFn {
    const char* name;
    std::uint32_t rva;
    std::uint32_t len;
    std::uint64_t hash;
    std::vector<std::uint16_t> masked;
};
const std::vector<GameFn>& Table() {
    static const std::vector<GameFn> t = {
#include "game_table_16965472.inc"
    };
    return t;
}
const std::vector<GameFn>& TpTable() {
    static const std::vector<GameFn> t = {
#include "game_table_tp_16965472.inc"
    };
    return t;
}
// Analysis round 8 (water, quest target, time scale): separate group with fixed call targets
struct CallCheck {
    const char* name;
    std::uint32_t at;
    std::uint32_t target;
};
const std::vector<GameFn>& ExtTable() {
    static const std::vector<GameFn> t = {
#include "game_table_ext_16965472.inc"
    };
    return t;
}
const std::vector<CallCheck>& ExtCalls() {
    static const std::vector<CallCheck> t = {
#include "game_calls_ext_16965472.inc"
    };
    return t;
}
constexpr std::uint64_t kExeSize = 243515408ull;

// Global and vtbl RVAs (from private static analysis notes, not included)
constexpr std::uintptr_t kAcpMgrGlobal = 0x4C23AF8;
constexpr std::uintptr_t kProgressRootGlobal = 0x4C5AAD8;
constexpr std::uintptr_t kSkillMgrGlobal = 0x4C4F6E8;
constexpr std::uintptr_t kVtblTod = 0x3669688;
constexpr std::uintptr_t kVtblAcp = 0x37BBCE8;
constexpr std::uintptr_t kVtblProgress = 0x3790ED8;
constexpr std::uintptr_t kVtblXpMgr = 0x3790B78;

// Resolved functions (immutable after verification)
struct Fns {
    void* (*getWorld)() = nullptr;
    void* (*getTodMgr)(void*) = nullptr;
    float (*todGetTime)(void*) = nullptr;
    bool (*todIsPaused)(void*) = nullptr;
    bool (*acpGate1)() = nullptr;
    std::uint32_t (*getLocalLevel)(void*) = nullptr;
    std::uint32_t (*getLocalXp)() = nullptr;
    std::uint32_t (*getAvailPts)(void*) = nullptr;
    void (*todSetTime)(void*, float) = nullptr;
    void (*addXpLocal)(void*, std::uint32_t) = nullptr;
    std::uint32_t (*levelForXp)(void*, std::uint32_t) = nullptr;
    void** (*getLocalHandle)(void*) = nullptr;
    int (*addAbilityPts)(void*, int, void*, void*, bool) = nullptr;
    float (*wxGetCurrent)(void*) = nullptr;
    void (*wxSetTarget)(void*, float, float) = nullptr;
    bool (*wxIsRandPaused)(void*) = nullptr;
    void (*wxSetRandPaused)(void*, bool) = nullptr;
} g_fn;

constexpr std::uintptr_t kWeatherState = 0x42ED870;

// --- Teleport (analysis round 6), stage 8a: read-only calls ---
struct alignas(16) Vec4 {
    float x, y, z, w;
};
struct TpFns {
    void* (*markerModuleGet)() = nullptr;
    bool (*markerIsSet)(void*) = nullptr;
    bool (*markerSameWorld)(void*) = nullptr;
    bool (*isLocalPlayer)(void*) = nullptr;
    Vec4* (*charGetWorldPos)(Vec4*, void*) = nullptr;
    void* (*entParentLink)(void*) = nullptr;
    void* (*linkGetParent)(void*) = nullptr;
    void* (*charStateObj)(void*) = nullptr;
    bool (*charStateIsActive)(void*, int) = nullptr;
    void* (*conflictMgrOfEnt)(void*) = nullptr;
    void* (*conflictFind)(void*, void*) = nullptr;
    float (*conflictDuration)(void*) = nullptr;
    bool (*questBlocksFt)(void*) = nullptr;
    bool (*lscBusy)(void*) = nullptr;
    bool (*worldIsLoaded)(void*) = nullptr;
    // 8b: Entity*, const Vec4* pos, const Vec4* quat, bool, bool (5th argument on the stack)
    void (*charTeleportQuat)(void*, const Vec4*, const Vec4*, bool, bool) = nullptr;
    bool (*iconLookup)(void*, const std::int32_t*, void**) = nullptr;  // (IconMgr+40h, &id, &handleOut)
    // Move a mount together with its rider: (interface, player, const Vec4* pos); 1st argument is unused
    void (*ifaceTeleportMount)(void*, void*, const Vec4*) = nullptr;
    // Pointer to the mount's handle: (&rider+1F8h, true)
    void** (*riderMountHandle)(void*, bool) = nullptr;
} g_tp;

// Analysis round 8
struct U32Array {  // the game's Array<u32>: data, packed (bits 0-14 capacity, bit 16 external buffer, bits 17+ count)
    std::uint32_t* data;
    std::uint32_t packed;
};
struct ExtFns {
    bool (*waterDepthFast)(const Vec4*, void*, float*, float*) = nullptr;  // (pos, World, &level, &depth)
    bool (*waterLevelGet)(const Vec4*, void*, float*, bool) = nullptr;     // (pos, World, &level, loadedOnly)
    void** (*qmGetTracked)(void*, void**) = nullptr;                        // (UIQuestManager, &handle) +1 reference
    void (*iconsOfLinked)(void*, void**, U32Array*) = nullptr;              // (IconMgr, &handle, &array)
    void* (*iconObjective)(void*, const std::int32_t*) = nullptr;           // (IconMgr, &id) -> UIObjective*
    void (*handleRelease)(void*) = nullptr;                                 // reference -1
    void (*entSetTimeScale)(void*, float) = nullptr;                        // (Entity, factor > 0)
    void* (*animalCtrlOfEnt)(void*) = nullptr;                              // Entity -> animal controller or 0
} g_ext;
std::atomic<int> g_extState{0};  // 0 unchecked, 1 ready, 2 unavailable
constexpr std::uintptr_t kVtblQuestMgr = 0x37E51C0, kVtblIconMgrDelegate = 0x3824FB0;
constexpr std::uintptr_t kVtblIconCtrlEntity = 0x3822B00, kVtblIconCtrlDynLoc = 0x3822DD0;
constexpr std::uintptr_t kVoGetPosDynamic = 0x1D82FC0, kVoGetPosStatic = 0x1D84270;
constexpr std::uintptr_t kGamePlaySettings = 0x4C59408, kVtblGamePlaySettings = 0x3765678, kVtblPlayerSettings = 0x3764FE8;
constexpr std::uintptr_t kVtblAnimalCtrl = 0x385E9C8;
// Time scale: request (render thread) and state (PreStep only)
std::atomic<float> g_swimScale{1.0f};
std::atomic<float> g_mountScale{1.0f};
std::atomic<bool> g_tsActive{false};
std::atomic<bool> g_tsShutdown{false};  // unloading: accept no more requests
std::uintptr_t g_tsWorld = 0, g_tsPlayer = 0, g_tsMount = 0;
float g_tsPlayerSet = 1.0f, g_tsMountSet = 1.0f;
std::uint64_t g_tsNext = 0;
constexpr std::uintptr_t kAssassinIface = 0x4C23DF0;
std::atomic<bool> g_tpAllowCombat{false};
std::atomic<bool> g_tpAllowMounted{false};
constexpr std::uintptr_t kIconMgr = 0x4C5EC80;
std::atomic<float> g_tpMaxDistance{30000.0f};
constexpr std::uintptr_t kMarkerMgr = 0x4C5E678, kVtblMarkerMgr = 0x37E5860, kVtblMarkerModule = 0x37FAC80;
constexpr std::uintptr_t kVtblMarkerIconCtrl = 0x3822CE0;  // CustomMarkerUIIconsController
constexpr std::uintptr_t kIconPosInvalid = 0x4870910;      // "no position" vector (compared like 22F6290 does)
constexpr std::uintptr_t kVtblPosIconCtrl = 0x3822BF0;     // PositionUIIconsController: actual position at +180h
constexpr std::uintptr_t kFastTravel = 0x4C59418, kVtblFastTravel = 0x3765EC0;
constexpr std::uintptr_t kQuestMgr = 0x4C5E668, kLsc = 0x4C25B68, kLscDep = 0x4C24E90, kLocalCtl = 0x4C5D650;
constexpr float kTpMaxDistance = 30000.0f;  // the game discards markers > 30 km
std::atomic<bool> g_tpEnabled{false};
std::atomic<int> g_tpState{0};  // 0 unchecked, 1 ready, 2 unavailable  // global state block S = [tod+240h] (analysis round 7)

// Weather: request from the render thread, reconciled in OnPreStep
std::atomic<bool> g_wxWantHold{false};
std::atomic<float> g_wxWantValue{0.0f};
std::atomic<float> g_wxSeconds{8.0f};
// PreStep only (serialized via the read interval)
bool g_wxWeHold = false;
float g_wxLastTarget = -1.0f;

constexpr std::uintptr_t kEmptyHandle = 0x4871468;  // the game's own empty handle (AddXpLocal compares against it)

// --- Actions (render thread -> OnPreStep) ---
struct PendingAction {
    bool set = false;
    Action action = Action::GrantXp;
    int value = 0;
    float hours = 0;
    float xpMultiplierPct = 100;
    std::uint64_t expires = 0;
};
std::mutex g_actMutex;  // Render: lock, Detour: try_lock
PendingAction g_pending;
std::atomic<bool> g_holdOn{false};
std::atomic<float> g_holdHours{12.0f};

// Own time-lapse clock
std::atomic<bool> g_clockOn{false};
std::atomic<float> g_clockMin[4] = {6.0f, 12.0f, 6.0f, 12.0f};
std::atomic<bool> g_clockActive{false};  // for display: clock is currently running
// detour only (serialized via g_clockMutex with try_lock)
std::mutex g_clockMutex;
std::uint64_t g_clockLastTick = 0;
float g_clockTime = 0;
bool g_clockSynced = false;

std::mutex g_resultMutex;
char g_resultText[192] = {};
std::uint32_t g_resultSeq = 0;
bool g_resultIsTime = false;  // last request was "set time of day" (shown in the right place)
// detour thread(s) only, under g_actMutex: result not yet published
char g_unpublished[192] = {};
bool g_hasUnpublished = false;

std::uintptr_t g_base = 0;
std::atomic<bool> g_enabled{false};
std::atomic<BridgeState> g_state{BridgeState::Disabled};
std::atomic<bool> g_faulted{false};
BridgeState g_loggedState = BridgeState::Disabled;  // lifecycle only

std::mutex g_infoMutex;  // leaf mutex: detour writes via try_lock, render reads
GameInfo g_info;
std::atomic<std::uint64_t> g_nextRead{0};

std::uint64_t Fnv(const unsigned char* p, const GameFn& f) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    std::size_t m = 0;
    for (std::uint32_t i = 0; i < f.len; ++i) {
        unsigned char b = p[i];
        if (m < f.masked.size() && f.masked[m] == i) {
            b = 0;
            ++m;
        }
        h ^= b;
        h *= 0x100000001b3ull;
    }
    return h;
}

// SEH-protected read of our own process's code (no C++ objects inside the __try frame).
bool SehHash(const unsigned char* p, const GameFn* f, std::uint64_t* out) {
    __try {
        *out = Fnv(p, *f);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool Readable(const void* p, std::size_t n) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return static_cast<const char*>(p) + n <= static_cast<const char*>(mbi.BaseAddress) + mbi.RegionSize;
}

// Verifies all table entries in memory. Returns the number of mismatches and fills g_fn.
int VerifyAndResolve(std::string& detail) {
    int bad = 0;
    for (const GameFn& f : Table()) {
        const auto* p = reinterpret_cast<const unsigned char*>(g_base + f.rva);
        std::uint64_t h = 0;
        if (!Readable(p, f.len) || !SehHash(p, &f, &h) || h != f.hash) {
            ++bad;
            detail += std::string(f.name) + " ";
        }
    }
    if (bad) return bad;
    auto at = [](std::uint32_t rva) { return reinterpret_cast<void*>(g_base + rva); };
    auto find = [&](const char* n) -> void* {
        for (const GameFn& f : Table())
            if (std::string(f.name) == n) return at(f.rva);
        return nullptr;
    };
    g_fn.getWorld = reinterpret_cast<void* (*)()>(find("GetWorld"));
    g_fn.getTodMgr = reinterpret_cast<void* (*)(void*)>(find("GetTodMgr"));
    g_fn.todGetTime = reinterpret_cast<float (*)(void*)>(find("TodGetTime"));
    g_fn.todIsPaused = reinterpret_cast<bool (*)(void*)>(find("TodIsPaused"));
    g_fn.acpGate1 = reinterpret_cast<bool (*)()>(find("AcpGate1"));
    g_fn.getLocalLevel = reinterpret_cast<std::uint32_t (*)(void*)>(find("GetLocalLevel"));
    g_fn.getLocalXp = reinterpret_cast<std::uint32_t (*)()>(find("GetLocalXp"));
    g_fn.getAvailPts = reinterpret_cast<std::uint32_t (*)(void*)>(find("GetAvailPts"));
    g_fn.todSetTime = reinterpret_cast<void (*)(void*, float)>(find("TodSetTime"));
    g_fn.addXpLocal = reinterpret_cast<void (*)(void*, std::uint32_t)>(find("AddXpLocal"));
    g_fn.levelForXp = reinterpret_cast<std::uint32_t (*)(void*, std::uint32_t)>(find("LevelForXp"));
    g_fn.getLocalHandle = reinterpret_cast<void** (*)(void*)>(find("GetLocalHandle"));
    g_fn.addAbilityPts = reinterpret_cast<int (*)(void*, int, void*, void*, bool)>(find("AddAbilityPts"));
    g_fn.wxGetCurrent = reinterpret_cast<float (*)(void*)>(find("WxGetCurrent"));
    g_fn.wxSetTarget = reinterpret_cast<void (*)(void*, float, float)>(find("WxSetTarget"));
    g_fn.wxIsRandPaused = reinterpret_cast<bool (*)(void*)>(find("WxIsRandPaused"));
    g_fn.wxSetRandPaused = reinterpret_cast<void (*)(void*, bool)>(find("WxSetRandPaused"));
    if (!g_fn.getWorld || !g_fn.getTodMgr || !g_fn.todGetTime || !g_fn.acpGate1 || !g_fn.getLocalLevel ||
        !g_fn.getLocalXp || !g_fn.getAvailPts || !g_fn.todSetTime || !g_fn.addXpLocal || !g_fn.levelForXp ||
        !g_fn.getLocalHandle || !g_fn.addAbilityPts || !g_fn.wxGetCurrent || !g_fn.wxSetTarget || !g_fn.wxIsRandPaused ||
        !g_fn.wxSetRandPaused) {
        detail += "(table incomplete) ";
        return 1;
    }
    return 0;
}

// Verify and resolve the teleport group (separate table: a mismatch disables only the teleport).
int VerifyAndResolveTp(std::string& detail) {
    int bad = 0;
    for (const GameFn& f : TpTable()) {
        const auto* p = reinterpret_cast<const unsigned char*>(g_base + f.rva);
        std::uint64_t h = 0;
        if (!Readable(p, f.len) || !SehHash(p, &f, &h) || h != f.hash) {
            ++bad;
            detail += std::string(f.name) + " ";
        }
    }
    if (bad) return bad;
    auto find = [&](const char* n) -> void* {
        for (const GameFn& f : TpTable())
            if (std::string(f.name) == n) return reinterpret_cast<void*>(g_base + f.rva);
        return nullptr;
    };
    g_tp.markerModuleGet = reinterpret_cast<void* (*)()>(find("MarkerModuleGet"));
    g_tp.markerIsSet = reinterpret_cast<bool (*)(void*)>(find("MarkerIsSet"));
    g_tp.markerSameWorld = reinterpret_cast<bool (*)(void*)>(find("MarkerMgrSameWorld"));
    g_tp.isLocalPlayer = reinterpret_cast<bool (*)(void*)>(find("IsLocalPlayerEnt"));
    g_tp.charGetWorldPos = reinterpret_cast<Vec4* (*)(Vec4*, void*)>(find("CharGetWorldPos"));
    g_tp.entParentLink = reinterpret_cast<void* (*)(void*)>(find("EntParentLink"));
    g_tp.linkGetParent = reinterpret_cast<void* (*)(void*)>(find("LinkGetParent"));
    g_tp.charStateObj = reinterpret_cast<void* (*)(void*)>(find("CharStateObj"));
    g_tp.charStateIsActive = reinterpret_cast<bool (*)(void*, int)>(find("CharStateIsActive"));
    g_tp.conflictMgrOfEnt = reinterpret_cast<void* (*)(void*)>(find("ConflictMgrOfEnt"));
    g_tp.conflictFind = reinterpret_cast<void* (*)(void*, void*)>(find("ConflictFind"));
    g_tp.conflictDuration = reinterpret_cast<float (*)(void*)>(find("ConflictDuration"));
    g_tp.questBlocksFt = reinterpret_cast<bool (*)(void*)>(find("QuestBlocksFt"));
    g_tp.lscBusy = reinterpret_cast<bool (*)(void*)>(find("LscBusy"));
    g_tp.worldIsLoaded = reinterpret_cast<bool (*)(void*)>(find("WorldIsLoaded"));
    g_tp.charTeleportQuat = reinterpret_cast<void (*)(void*, const Vec4*, const Vec4*, bool, bool)>(find("CharTeleportQuat"));
    g_tp.iconLookup = reinterpret_cast<bool (*)(void*, const std::int32_t*, void**)>(find("IconLookup"));
    g_tp.ifaceTeleportMount = reinterpret_cast<void (*)(void*, void*, const Vec4*)>(find("IfaceTeleportMount"));
    g_tp.riderMountHandle = reinterpret_cast<void** (*)(void*, bool)>(find("RiderMountHandle"));
    // Call targets that the mask hides but that must be fixed (jump -> matrix variant -> SetTransform)
    auto callTarget = [&](const char* fn, std::uint32_t at) -> std::uintptr_t {
        const auto a = reinterpret_cast<std::uintptr_t>(find(fn));
        return a ? a + at + 4 + static_cast<std::intptr_t>(*reinterpret_cast<const std::int32_t*>(a + at)) : 0;
    };
    if (callTarget("CharTeleportQuat", 0x131) != g_base + 0x1F4DE40 || callTarget("CharTeleportMtx", 0x1B) != g_base + 0x8E7420 ||
        callTarget("IfaceTeleportMount", 0x47) != g_base + 0x1DE2F10 || callTarget("IfaceTeleportMount", 0xA3) != g_base + 0x1DFF920 ||
        callTarget("RiderMountHandle", 0x0A) != g_base + 0x1D846F0 || callTarget("RiderMountHandle", 0x17) != g_base + 0x1D82390) {
        detail += "(call targets) ";
        return 1;
    }
    return 0;
}

// Verify and resolve the round 8 group (a mismatch disables only water, quest target and time scale).
int VerifyAndResolveExt(std::string& detail) {
    int bad = 0;
    for (const GameFn& f : ExtTable()) {
        const auto* p = reinterpret_cast<const unsigned char*>(g_base + f.rva);
        std::uint64_t h = 0;
        if (!Readable(p, f.len) || !SehHash(p, &f, &h) || h != f.hash) {
            ++bad;
            detail += std::string(f.name) + " ";
        }
    }
    if (bad) return bad;
    auto find = [&](const char* n) -> std::uintptr_t {
        for (const GameFn& f : ExtTable())
            if (std::string(f.name) == n) return g_base + f.rva;
        return 0;
    };
    for (const CallCheck& c : ExtCalls()) {
        const std::uintptr_t a = find(c.name);
        if (!a || a + c.at + 4 + static_cast<std::intptr_t>(*reinterpret_cast<const std::int32_t*>(a + c.at)) != g_base + c.target) {
            detail += std::string("(call target ") + c.name + ") ";
            return 1;
        }
    }
    g_ext.waterDepthFast = reinterpret_cast<bool (*)(const Vec4*, void*, float*, float*)>(find("WaterLevelDepthFast"));
    g_ext.waterLevelGet = reinterpret_cast<bool (*)(const Vec4*, void*, float*, bool)>(find("WaterLevelGet"));
    g_ext.qmGetTracked = reinterpret_cast<void** (*)(void*, void**)>(find("QmGetTracked"));
    g_ext.iconsOfLinked = reinterpret_cast<void (*)(void*, void**, U32Array*)>(find("IconsOfLinked"));
    g_ext.iconObjective = reinterpret_cast<void* (*)(void*, const std::int32_t*)>(find("IconObjective"));
    g_ext.handleRelease = reinterpret_cast<void (*)(void*)>(find("HandleRelease"));
    g_ext.entSetTimeScale = reinterpret_cast<void (*)(void*, float)>(find("EntSetTimeScale"));
    g_ext.animalCtrlOfEnt = reinterpret_cast<void* (*)(void*)>(find("AnimalCtrlOfEnt"));
    return 0;
}

template <typename T>
T Load(std::uintptr_t addr) {
    return *reinterpret_cast<const T*>(addr);
}

// The only place with game access. Plain types only (SEH frame).
bool SehReadGame(GameInfo* out) {
    __try {
        GameInfo r{};
        if (!g_fn.acpGate1()) return false;  // the game's own gate: ACP save, in game, no locked context

        // Time of day
        const auto world = reinterpret_cast<std::uintptr_t>(g_fn.getWorld());
        if (world) {
            const auto tod = reinterpret_cast<std::uintptr_t>(g_fn.getTodMgr(reinterpret_cast<void*>(world)));
            if (tod && Load<std::uintptr_t>(tod) == g_base + kVtblTod) {
                r.hours = g_fn.todGetTime(reinterpret_cast<void*>(tod));
                const auto s = Load<std::uintptr_t>(tod + 0x240);
                const int questPause = s ? Load<int>(s + 4) : 0;
                const bool camStill = s ? Load<unsigned char>(s + 9) != 0 : false;
                const int worldPause = Load<int>(world + 0x1458);
                r.displayOverride = Load<unsigned char>(tod + 0x381) != 0;
                const auto mgr = Load<std::uintptr_t>(g_base + kAcpMgrGlobal);
                if (mgr && Load<std::uintptr_t>(mgr) == g_base + kVtblAcp)
                    r.acpTimeBlocked = Load<unsigned char>(mgr + 0xD) != 0;
                r.stop = questPause > 0 ? TimeStop::QuestPause
                         : worldPause > 0 ? TimeStop::WorldPaused
                         : camStill       ? TimeStop::CameraStill
                                          : TimeStop::None;
            }
        }

        // Progression
        const auto g = Load<std::uintptr_t>(g_base + kProgressRootGlobal);
        if (g &&Load<std::uintptr_t>(g) == g_base + kVtblProgress && Load<std::uintptr_t>(g + 0x100) == g_base + kVtblXpMgr) {
            r.level = g_fn.getLocalLevel(reinterpret_cast<void*>(g + 0x100));
            r.hasPlayer = r.level != 0;
            if (r.hasPlayer) r.xp = g_fn.getLocalXp();
            const auto sm = Load<std::uintptr_t>(g_base + kSkillMgrGlobal);
            if (sm && r.hasPlayer) r.abilityPoints = g_fn.getAvailPts(reinterpret_cast<void*>(sm));
        }
        r.valid = true;
        *out = r;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faulted.store(true);
        return false;
    }
}

// Is the handle object valid? (same check as the game's callers: bit 31 of [h+8] and object with flag +90h & 2)
bool HandleAlive(std::uintptr_t hobj) {
    if (!hobj || hobj == g_base + kEmptyHandle) return false;
    if ((Load<std::uint32_t>(hobj + 8) & 0x80000000u) == 0) return false;
    const auto obj = Load<std::uintptr_t>(hobj);
    return obj && (Load<unsigned char>(obj + 0x90) & 2) != 0;
}

// Time-of-day object of the current world (0 = none).
std::uintptr_t CurrentTod() {
    const auto world = reinterpret_cast<std::uintptr_t>(g_fn.getWorld());
    if (!world) return 0;
    const auto tod = reinterpret_cast<std::uintptr_t>(g_fn.getTodMgr(reinterpret_cast<void*>(world)));
    return tod && Load<std::uintptr_t>(tod) == g_base + kVtblTod ? tod : 0;
}

// Progression root with XP manager (0 = none or no player).
std::uintptr_t ProgressRoot() {
    const auto g = Load<std::uintptr_t>(g_base + kProgressRootGlobal);
    if (!g || Load<std::uintptr_t>(g) != g_base + kVtblProgress || Load<std::uintptr_t>(g + 0x100) != g_base + kVtblXpMgr)
        return 0;
    if (g_fn.getLocalLevel(reinterpret_cast<void*>(g + 0x100)) == 0) return 0;
    return g;
}

enum class ActResult : int { Ok, NotReady, NoPlayer, MultiplierZero, AlreadyThere, NoTod, Fault };

struct ActOut {
    ActResult r = ActResult::NotReady;
    std::uint32_t before = 0, after = 0;  // level/XP/points before/after (depending on the action)
    std::uint32_t sent = 0;               // amount passed to the game
};

std::uint32_t CompensatedXp(std::uint32_t want, float pct) {
    // The game computes ceil(n * multiplier); we pass n = ceil(want / multiplier).
    const double m = static_cast<double>(pct) / 100.0;
    double n = static_cast<double>(want) / m;
    n = n - static_cast<double>(static_cast<std::uint64_t>(n)) > 1e-9 ? static_cast<double>(static_cast<std::uint64_t>(n)) + 1.0 : n;
    return n >= 2147483647.0 ? 2147483647u : static_cast<std::uint32_t>(n);
}

// The only place with writing game calls. Plain types only (SEH frame).
bool SehAct(const PendingAction* a, ActOut* out) {
    __try {
        if (!g_fn.acpGate1()) {
            out->r = ActResult::NotReady;
            return true;
        }
        if (a->action == Action::SetTime) {
            const std::uintptr_t tod = CurrentTod();
            if (!tod) {
                out->r = ActResult::NoTod;
                return true;
            }
            g_fn.todSetTime(reinterpret_cast<void*>(tod), a->hours);
            out->r = ActResult::Ok;
            return true;
        }
        const std::uintptr_t g = ProgressRoot();
        if (!g) {
            out->r = ActResult::NoPlayer;
            return true;
        }
        void* xpMgr = reinterpret_cast<void*>(g + 0x100);
        if (a->action == Action::GrantPoints) {
            const auto sm = Load<std::uintptr_t>(g_base + kSkillMgrGlobal);
            if (!sm || !HandleAlive(Load<std::uintptr_t>(g + 0xA8))) {
                out->r = ActResult::NoPlayer;
                return true;
            }
            out->before = g_fn.getAvailPts(reinterpret_cast<void*>(sm));
            out->sent = static_cast<std::uint32_t>(g_fn.addAbilityPts(reinterpret_cast<void*>(sm), a->value,
                                                                       reinterpret_cast<void*>(g + 0xA8),
                                                                       reinterpret_cast<void*>(g + 0x120), false));
            out->after = g_fn.getAvailPts(reinterpret_cast<void*>(sm));
            out->r = ActResult::Ok;
            return true;
        }
        // XP actions
        if (a->xpMultiplierPct <= 0.0f) {
            out->r = ActResult::MultiplierZero;
            return true;
        }
        void** hp = g_fn.getLocalHandle(reinterpret_cast<void*>(g));
        if (!hp || !HandleAlive(reinterpret_cast<std::uintptr_t>(*hp))) {
            out->r = ActResult::NoPlayer;
            return true;
        }
        const std::uint32_t xp = g_fn.getLocalXp();
        std::uint32_t want = 0;
        if (a->action == Action::GrantXp) {
            want = static_cast<std::uint32_t>(a->value);
        } else {  // LevelAtLeast: smallest XP value with level >= target (LevelForXp is a pure table lookup)
            const auto target = static_cast<std::uint32_t>(a->value);
            if (g_fn.levelForXp(xpMgr, xp) >= target) {
                out->r = ActResult::AlreadyThere;
                out->before = out->after = g_fn.getLocalLevel(xpMgr);
                return true;
            }
            std::uint32_t lo = xp, hi = 0x7FFFFFFFu;
            if (g_fn.levelForXp(xpMgr, hi) < target) {
                // target beyond the end of the table: use the highest reachable level
                const std::uint32_t maxLevel = g_fn.levelForXp(xpMgr, hi);
                while (lo < hi) {
                    const std::uint32_t mid = lo + (hi - lo) / 2;
                    if (g_fn.levelForXp(xpMgr, mid) >= maxLevel) hi = mid;
                    else lo = mid + 1;
                }
            } else {
                while (lo < hi) {
                    const std::uint32_t mid = lo + (hi - lo) / 2;
                    if (g_fn.levelForXp(xpMgr, mid) >= target) hi = mid;
                    else lo = mid + 1;
                }
            }
            want = lo > xp ? lo - xp : 0;
            if (want == 0) {
                out->r = ActResult::AlreadyThere;
                out->before = out->after = g_fn.getLocalLevel(xpMgr);
                return true;
            }
        }
        out->before = a->action == Action::GrantXp ? xp : g_fn.getLocalLevel(xpMgr);
        out->sent = CompensatedXp(want, a->xpMultiplierPct);
        g_fn.addXpLocal(xpMgr, out->sent);
        out->after = a->action == Action::GrantXp ? g_fn.getLocalXp() : g_fn.getLocalLevel(xpMgr);
        out->r = ActResult::Ok;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faulted.store(true);
        out->r = ActResult::Fault;
        return false;
    }
}

// Hold time of day: set it again when it drifts by more than one game minute.
void SehHold(float target, float current) {
    __try {
        float d = current - target;
        if (d > 12.0f) d -= 24.0f;
        if (d < -12.0f) d += 24.0f;
        if (d > -1.0f / 60.0f && d < 1.0f / 60.0f) return;
        if (!g_fn.acpGate1()) return;
        if (const std::uintptr_t tod = CurrentTod()) g_fn.todSetTime(reinterpret_cast<void*>(tod), target);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faulted.store(true);
    }
}

// Own time-lapse clock, one step. Returns whether the clock drove the time.
// Speed as in the game: phase i (morning 4-8, noon 8-16, afternoon 16-20, night 20-4) lasts minutes[i] real-time minutes.
bool SehClockStep(float dtSec, const float* minutes) {
    __try {
        if (!g_fn.acpGate1()) return false;
        const auto mgr = Load<std::uintptr_t>(g_base + kAcpMgrGlobal);
        if (!mgr || Load<std::uintptr_t>(mgr) != g_base + kVtblAcp || Load<unsigned char>(mgr + 0xD) == 0) {
            g_clockSynced = false;  // not blocked: the game uses the ACP values itself
            return false;
        }
        const auto world = reinterpret_cast<std::uintptr_t>(g_fn.getWorld());
        if (!world) return false;
        const std::uintptr_t tod = CurrentTod();
        if (!tod) return false;
        const float t = g_fn.todGetTime(reinterpret_cast<void*>(tod));
        if (!g_clockSynced) {
            g_clockTime = t;
            g_clockSynced = true;
        } else {
            float d = t - g_clockTime;
            if (d > 12.0f) d -= 24.0f;
            if (d < -12.0f) d += 24.0f;
            if (d > 0.25f || d < -0.25f) g_clockTime = t;  // a script/cutscene set the time: adopt it
        }
        const auto s = Load<std::uintptr_t>(tod + 0x240);
        const int questPause = s ? Load<int>(s + 4) : 0;
        if (questPause > 0 || Load<int>(world + 0x1458) > 0) return true;  // time is stopped in the game: do not advance

        const float h = g_clockTime;
        const int phase = (h >= 4.0f && h < 8.0f) ? 0 : (h >= 8.0f && h < 16.0f) ? 1 : (h >= 16.0f && h < 20.0f) ? 2 : 3;
        constexpr float kHours[4] = {4.0f, 8.0f, 4.0f, 8.0f};
        float m = minutes[phase];
        if (m < 0.01f) m = 0.01f;
        float nt = h + kHours[phase] / (m * 60.0f) * dtSec;
        while (nt >= 24.0f) nt -= 24.0f;
        g_clockTime = nt;
        g_fn.todSetTime(reinterpret_cast<void*>(tod), nt);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faulted.store(true);
        return false;
    }
}

void ClockTick(std::uint64_t now) {
    std::unique_lock lock(g_clockMutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    if (!g_clockOn.load(std::memory_order_relaxed) || g_holdOn.load(std::memory_order_relaxed)) {
        g_clockSynced = false;
        g_clockLastTick = 0;
        g_clockActive.store(false, std::memory_order_relaxed);
        return;
    }
    if (g_clockLastTick && now - g_clockLastTick < 33) return;  // at most ~30 steps per second
    float dt = g_clockLastTick ? static_cast<float>(now - g_clockLastTick) / 1000.0f : 0.0f;
    if (dt > 0.25f) dt = 0.25f;  // do not jump after a loading screen/menu
    g_clockLastTick = now;
    float minutes[4];
    for (int i = 0; i < 4; ++i) minutes[i] = g_clockMin[i].load(std::memory_order_relaxed);
    g_clockActive.store(SehClockStep(dt, minutes), std::memory_order_relaxed);
}

// Read the position of a map icon controller (like 22F6290, without the computing branch): final position,
// valid target position or - for PositionUIIconsController - its actual position at +180h. 0 = unreadable.
int ReadIconCtrlPos(std::uintptr_t ctrl, float* out) {
    const unsigned char flags = Load<unsigned char>(ctrl + 0x4C);
    std::uintptr_t src = 0;
    int mode = 0;
    if (flags & 1) {
        src = ctrl + 0x10;
        mode = 1;
    } else if (flags & 2) {
        bool invalid = true;
        for (int i = 0; i < 4; ++i) {
            const float a = Load<float>(ctrl + 0x20 + 4 * i), b = Load<float>(g_base + kIconPosInvalid + 4 * i);
            invalid = invalid && std::fabs(a - b) <= 1e-5f;
        }
        if (!invalid) {
            src = ctrl + 0x20;
            mode = 2;
        }
    }
    if (!src && Load<std::uintptr_t>(ctrl) == g_base + kVtblPosIconCtrl) {
        src = ctrl + 0x180;
        mode = 4;
    }
    if (!src) return 0;
    out[0] = Load<float>(src);
    out[1] = Load<float>(src + 4);
    out[2] = Load<float>(src + 8);
    return mode;
}

std::uintptr_t DerefHandle(std::uintptr_t h) { return h && (Load<std::uint32_t>(h + 8) & 0x80000000u) ? Load<std::uintptr_t>(h) : 0; }

bool UsablePos(const float* p) {
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(p[i]) || std::fabs(p[i]) > 1e9f) return false;
    return p[0] != 0.0f || p[1] != 0.0f || p[2] != 0.0f;
}

// Analysis round 8: read the raw position of any icon controller (read-only equivalent of vtbl+D0h, without the
// smoothing 22F5830). entity = position of a character/object. false = no position.
bool ReadAnyIconPos(std::uintptr_t ctrl, std::uintptr_t world, float* out, bool* entity) {
    *entity = false;
    if (ReadIconCtrlPos(ctrl, out)) return UsablePos(out);
    const auto vt = Load<std::uintptr_t>(ctrl);
    if (vt == g_base + kVtblMarkerIconCtrl) {
        const auto inner = DerefHandle(Load<std::uintptr_t>(ctrl + 0x180));
        return inner && Load<std::uintptr_t>(inner) == g_base + kVtblPosIconCtrl && ReadIconCtrlPos(inner, out) && UsablePos(out);
    }
    if (vt != g_base + kVtblIconCtrlEntity && vt != g_base + kVtblIconCtrlDynLoc) return false;
    const auto e = DerefHandle(Load<std::uintptr_t>(ctrl + 0x180));
    if (e && Load<std::uintptr_t>(e + 0x60) == world) {  // loaded entity: world position as for the player
        Vec4 pos{};
        g_tp.charGetWorldPos(&pos, reinterpret_cast<void*>(e));
        out[0] = pos.x;
        out[1] = pos.y;
        out[2] = pos.z;
        *entity = true;
        return UsablePos(out);
    }
    // not loaded: virtual MetaAI target (known classes only, read-only)
    const auto vo = DerefHandle(Load<std::uintptr_t>(ctrl + 0x188));
    if (!vo) return false;
    const auto slot = Load<std::uintptr_t>(Load<std::uintptr_t>(vo) + 0x1A8);
    std::uintptr_t src = 0;
    if (slot == g_base + kVoGetPosDynamic) src = vo + 0x10;
    else if (slot == g_base + kVoGetPosStatic) {
        const auto g60 = Load<std::uintptr_t>(vo + 0x60);
        const auto g = g60 ? Load<std::uintptr_t>(g60 + 0x40) : 0;
        src = g && Load<std::uintptr_t>(g + 0x38) ? g + 0x10 : vo + 0x10;
    }
    if (!src) return false;
    out[0] = Load<float>(src);
    out[1] = Load<float>(src + 4);
    out[2] = Load<float>(src + 8);
    *entity = true;
    return UsablePos(out);
}

// Analysis round 8: target of the tracked quest = nearest icon whose objective is active or that has none (like the
// HUD distance 21EBAD0). The game's chain: QmGetTracked -> IconsOfLinked -> IconObjective -> IconLookup.
// The calls only take and release their own locks/references. Check beforehand the objects they use unchecked.
TpBlock SehQuestTarget(std::uintptr_t world, const float* player, TeleportInfo* r) {
    alignas(16) static std::uint32_t buf[64];
    static U32Array arr;
    void* h = nullptr;
    __try {
        const auto mgr = Load<std::uintptr_t>(g_base + kQuestMgr);
        const auto icons = Load<std::uintptr_t>(g_base + kIconMgr);
        if (!mgr || Load<std::uintptr_t>(mgr) != g_base + kVtblQuestMgr || !icons ||
            Load<std::uintptr_t>(icons + 0x28) != g_base + kVtblIconMgrDelegate)
            return TpBlock::Unavailable;
        g_ext.qmGetTracked(reinterpret_cast<void*>(mgr), &h);
        if (!h) return TpBlock::NoQuestTracked;
        TpBlock res = TpBlock::NoQuestTracked;
        if (reinterpret_cast<std::uintptr_t>(h) != g_base + kEmptyHandle) {
            res = TpBlock::NoQuestTarget;
            arr.data = buf;
            arr.packed = 0x10000u | 64u;
            g_ext.iconsOfLinked(reinterpret_cast<void*>(icons), &h, &arr);
            // bit 16 cleared = the game grew the array (its own heap): discard the result
            const std::uint32_t n = (arr.packed & 0x10000u) && arr.data == buf ? (arr.packed >> 17) : 0;
            r->questIcons = static_cast<int>(n);
            float best = 1e30f;
            bool otherWorld = false;
            for (std::uint32_t i = 0; i < n && i < 64; ++i) {
                const std::int32_t id = static_cast<std::int32_t>(buf[i]);
                if (static_cast<std::uint32_t>(id) == 0x80000000u) continue;
                const auto obj = reinterpret_cast<std::uintptr_t>(g_ext.iconObjective(reinterpret_cast<void*>(icons), &id));
                if (obj && Load<int>(obj + 0x174) != 1) continue;  // linked objective not active
                void* slot = nullptr;
                if (!g_tp.iconLookup(reinterpret_cast<void*>(icons + 0x40), &id, &slot) || !slot) continue;
                const auto ctrl = DerefHandle(Load<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(slot)));
                if (!ctrl) continue;
                if (DerefHandle(Load<std::uintptr_t>(ctrl + 0x30)) != world) {
                    otherWorld = true;
                    continue;
                }
                float p[3];
                bool entity = false;
                if (!ReadAnyIconPos(ctrl, world, p, &entity)) continue;
                ++r->questUsable;
                const float dx = p[0] - player[0], dy = p[1] - player[1], dz = p[2] - player[2];
                const float d = dx * dx + dy * dy + dz * dz;
                if (d < best) {
                    best = d;
                    r->questTarget[0] = p[0];
                    r->questTarget[1] = p[1];
                    r->questTarget[2] = p[2];
                    r->questEntity = entity;
                }
            }
            if (best < 1e30f) {
                res = TpBlock::None;
                r->questDistance = std::sqrt(best);
            } else if (otherWorld) {
                res = TpBlock::OtherWorld;
            }
        }
        g_ext.handleRelease(h);
        h = nullptr;
        // the game's transition icon (target lies in another region): do not jump
        const auto m = Load<std::uintptr_t>(g_base + kMarkerMgr);
        if (res == TpBlock::None && m && Load<std::uintptr_t>(m) == g_base + kVtblMarkerMgr &&
            Load<std::uint32_t>(m + 0x1D8) != 0x80000000u)
            res = TpBlock::QuestOtherRegion;
        if (res == TpBlock::None && r->questDistance > kTpMaxDistance) res = TpBlock::TooFar;
        if (res == TpBlock::None && r->questDistance < 3.0f) res = TpBlock::TooClose;
        return res;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_extState.store(2);  // disable only the round 8 functions
        return TpBlock::Unavailable;
    }
}

// Analysis round 8: water at the target (pure grid query, also for unloaded areas). loadedOnly: exact query after loading.
// Returns true = deep water, *level = water height.
bool SehWaterAt(std::uintptr_t world, const float* p, bool loadedOnly, float* level) {
    alignas(16) static Vec4 pos;
    pos = Vec4{p[0], p[1], p[2], 0.0f};
    __try {
        float lv = -9999.0f, depth = 9999.0f;
        if (loadedOnly) {
            if (!g_ext.waterLevelGet(&pos, reinterpret_cast<void*>(world), &lv, true) || lv <= -9000.0f) return false;
            *level = lv;
            return true;
        }
        if (!g_ext.waterDepthFast(&pos, reinterpret_cast<void*>(world), &lv, &depth) || lv <= -9000.0f || depth >= 9000.0f) return false;
        // threshold as in the game: swimming from PlayerSettings+340h (otherwise 1.2 m)
        float thr = 1.2f;
        const auto gps = Load<std::uintptr_t>(g_base + kGamePlaySettings);
        if (gps && Load<std::uintptr_t>(gps) == g_base + kVtblGamePlaySettings) {
            const auto ps = Load<std::uintptr_t>(gps + 0x1638);
            if (ps && Load<std::uintptr_t>(ps) == g_base + kVtblPlayerSettings) {
                const float v = Load<float>(ps + 0x340);
                if (v > 0.2f && v < 5.0f) thr = v;
            }
        }
        if (depth <= thr) return false;
        *level = lv;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_extState.store(2);
        return false;
    }
}

// Teleport 8a: read marker, player position and blocking reason. No writing game calls. Before each call, check the
// objects that the game function itself dereferences unchecked (architecture review, analysis round 6).
void SehTeleport(TeleportInfo* t) {
    __try {
        TeleportInfo r{};
        r.valid = true;
        r.block = TpBlock::Unavailable;
        if (!g_fn.acpGate1()) {
            *t = r;
            return;
        }
        const auto world = reinterpret_cast<std::uintptr_t>(g_fn.getWorld());
        if (!world) {
            *t = r;
            return;
        }
        // Player
        const auto player = Load<std::uintptr_t>(world + 0x98);
        const auto ctl = Load<std::uintptr_t>(g_base + kLocalCtl);
        const bool ctlOk = ctl && Load<std::uintptr_t>(ctl + 0x2A0);
        r.playerBits = (player ? 1 : 0) | (player && Load<std::uintptr_t>(player + 0x60) == world ? 2 : 0) | (ctlOk ? 4 : 0) |
                       (player && ctlOk && g_tp.isLocalPlayer(reinterpret_cast<void*>(player)) ? 8 : 0);
        // Like the game's marker arrival check: player = [World+98h]; the other checks are diagnostics only
        const bool local = player != 0;
        if (local) {
            Vec4 pos{};
            g_tp.charGetWorldPos(&pos, reinterpret_cast<void*>(player));
            r.player[0] = pos.x;
            r.player[1] = pos.y;
            r.player[2] = pos.z;
        }
        // Marker - variant 1: UIAfterlifeManager block "applied" (+2E0), resolved position M+310
        const auto mgr = Load<std::uintptr_t>(g_base + kMarkerMgr);
        bool set1 = false, same1 = false;
        if (mgr && Load<std::uintptr_t>(mgr) == g_base + kVtblMarkerMgr) {
            r.blockFlagM6A = Load<unsigned char>(mgr + 0x6A) != 0;
            set1 = g_tp.markerIsSet(reinterpret_cast<void*>(mgr + 0x2E0));
            same1 = set1 && g_tp.markerSameWorld(reinterpret_cast<void*>(mgr + 0x2E0));
            if (set1) {
                r.target[0] = Load<float>(mgr + 0x310);
                r.target[1] = Load<float>(mgr + 0x314);
                r.target[2] = Load<float>(mgr + 0x318);
            }
        }
        // Variant 2: CustomMarkerModule (only "set" and "same world", position not used in 8a)
        const auto module = reinterpret_cast<std::uintptr_t>(g_tp.markerModuleGet());
        bool same2 = false;
        if (module && Load<std::uintptr_t>(module) == g_base + kVtblMarkerModule) {
            r.moduleSet = Load<std::uint32_t>(module + 0xE8) != 0x80000000u;
            same2 = r.moduleSet && Load<std::uintptr_t>(module + 0x28) == world;
            if (same2 && !same1) {
                r.posMode = 3;
                const std::int32_t iconId = Load<std::int32_t>(module + 0xEC);
                const auto iconMgr = Load<std::uintptr_t>(g_base + kIconMgr);
                if (iconId != static_cast<std::int32_t>(0x80000000u) && iconMgr) {
                    // Marker on a map icon (blacksmith, merchant, ...): position = icon position
                    void* handle = nullptr;
                    if (g_tp.iconLookup(reinterpret_cast<void*>(iconMgr + 0x40), &iconId, &handle) && handle) {
                        const auto ctrl = DerefHandle(Load<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(handle)));
                        if (ctrl) {
                            const int m = ReadIconCtrlPos(ctrl, r.target);
                            if (m) r.posMode = 4 + m;  // 5, 6, 8 = icon marker
                            else {  // icon on a character/object (side quest <!>, trial): entity or virtual target
                                bool entity = false;
                                if (ReadAnyIconPos(ctrl, world, r.target, &entity)) {
                                    r.posMode = entity ? 9 : 10;
                                    r.markerEntity = entity;
                                }
                            }
                        }
                    }
                } else {
                    // Free marker: handle at module+F0 -> marker icon
                    const auto ctrl = DerefHandle(Load<std::uintptr_t>(module + 0xF0));
                    if (ctrl && Load<std::uintptr_t>(ctrl) == g_base + kVtblMarkerIconCtrl) {
                        int m = ReadIconCtrlPos(ctrl, r.target);
                        if (!m) {  // actual position: inner PositionUIIconsController object (handle at +180h)
                            const auto inner = DerefHandle(Load<std::uintptr_t>(ctrl + 0x180));
                            if (inner && Load<std::uintptr_t>(inner) == g_base + kVtblPosIconCtrl) m = ReadIconCtrlPos(inner, r.target) ? 4 : 0;
                        }
                        if (m) r.posMode = m;
                    }
                }
            }
        }        r.markerSource = same1 ? 1 : (r.moduleSet ? 2 : 0);
        const bool havePos = same1 || (r.posMode != 0 && r.posMode != 3);
        if (havePos && local) {
            const float dx = r.target[0] - r.player[0], dy = r.target[1] - r.player[1], dz = r.target[2] - r.player[2];
            r.distanceXY = std::sqrt(dx * dx + dy * dy);
            r.distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        }

        // Blocks in fixed order (the first one gives the reason)
        const auto lsc = Load<std::uintptr_t>(g_base + kLsc);
        const bool loading = lsc && ((Load<std::uintptr_t>(g_base + kLscDep) && g_tp.lscBusy(reinterpret_cast<void*>(lsc))) ||
                                     Load<std::uint32_t>(lsc + 0x44) != 0);
        const bool loaded = Load<std::uintptr_t>(world + 0xE8) && g_tp.worldIsLoaded(reinterpret_cast<void*>(world));
        const auto ft = Load<std::uintptr_t>(g_base + kFastTravel);
        const bool ftLocked = !ft || Load<std::uintptr_t>(ft) != g_base + kVtblFastTravel || Load<int>(ft + 0xAC) != 0 ||
                              Load<std::uintptr_t>(ft + 0xD8) != 0;
        const auto quest = Load<std::uintptr_t>(g_base + kQuestMgr);
        const auto questObj = quest ? Load<std::uintptr_t>(quest + 0x18) : 0;
        const bool questBlocks = questObj && Load<std::uintptr_t>(questObj) && g_tp.questBlocksFt(reinterpret_cast<void*>(quest));
        bool combat = false, eagle = false, mounted = false;
        if (local) {
            // Diagnostics: 0 no fight director, 1 no conflict entry, 2 entry without duration, 3 in combat
            r.combatDiag = 0;
            if (void* fd = g_tp.conflictMgrOfEnt(reinterpret_cast<void*>(player))) {
                r.combatDiag = 1;
                if (void* f = g_tp.conflictFind(fd, reinterpret_cast<void*>(player + 0x1F8))) {
                    combat = g_tp.conflictDuration(f) > 0.0f;
                    r.combatDiag = combat ? 3 : 2;
                }
            }
            if (void* st = g_tp.charStateObj(reinterpret_cast<void*>(player))) eagle = g_tp.charStateIsActive(st, 0x21);
            if (void* link = g_tp.entParentLink(reinterpret_cast<void*>(player))) mounted = g_tp.linkGetParent(link) != nullptr;
        }
        r.mounted = mounted;
        // Blocks independent of the target (apply to marker and quest target)
        const TpBlock common = !local                                ? TpBlock::NotLocalPlayer
                               : loading                             ? TpBlock::Loading
                               : !loaded                             ? TpBlock::WorldNotLoaded
                               : Load<int>(world + 0x1458) > 0       ? TpBlock::Paused
                               : ftLocked && !g_tpAllowCombat.load() ? TpBlock::FastTravelLocked  // option: bypass combat and travel locks
                               : questBlocks                         ? TpBlock::QuestBlocks
                               : combat && !g_tpAllowCombat.load()   ? TpBlock::InCombat
                               : eagle                               ? TpBlock::EagleControl
                               : mounted && !g_tpAllowMounted.load() ? TpBlock::Mounted
                                                                     : TpBlock::None;
        r.block = common != TpBlock::None    ? common
                  : !set1 && !r.moduleSet    ? TpBlock::NoMarker
                  : !same1 && !same2         ? TpBlock::OtherWorld
                  : !havePos                 ? TpBlock::MarkerUnreadable
                  : r.distance > kTpMaxDistance ? TpBlock::TooFar
                  : r.distance < 3.0f        ? TpBlock::TooClose
                                             : TpBlock::None;
        *t = r;
        // Quest target (separate function, own SEH frame; only with a verified round 8 group)
        if (local && g_extState.load() == 1) {
            t->questAvailable = true;
            const TpBlock q = SehQuestTarget(world, r.player, t);
            t->questBlock = common != TpBlock::None ? common : q;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_tpState.store(2);  // disable only the teleport
        t->valid = false;
    }
}

// --- Stage 8b: jump sequence (PreStep only) ---
enum class TpPhase : int { Idle, Hold, Watch, ReturnHold, Done, Failed, Aborted };
struct TpRun {
    TpPhase phase = TpPhase::Idle;
    bool returning = false;
    std::uint64_t start = 0, lastStep = 0, loadedSince = 0, lastPlace = 0, watchStart = 0, finished = 0;
    int places = 0;
    float target[3] = {}, origin[3] = {};
    float distance = 0;
    bool mounted = false;  // move the mount together with its rider
    bool water = false;    // target is in deep water: place on the surface
    bool refined = false;  // water height re-checked once after loading
    float zOff = 1.0f;     // height added above the target
    std::uintptr_t world = 0, player = 0;
} g_run;
std::atomic<bool> g_tpRequest{false};
std::atomic<int> g_tpRequestKind{0};  // TpTarget
std::atomic<std::uint64_t> g_tpWaitUntil{0};
std::atomic<bool> g_paused{false};             // game paused (map/menu), from PreStep
std::atomic<std::uint64_t> g_pausedTick{0};  // quick teleport: wait until then for the map to close
std::atomic<int> g_tpPhase{0};
std::mutex g_tpTextMutex;  // leaf mutex: PreStep try_lock, render lock
char g_tpText[160] = {};
std::uint64_t g_tpTextUntil = 0;

void TpSetText(const char* text, std::uint64_t until) {
    std::unique_lock lock(g_tpTextMutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    std::snprintf(g_tpText, sizeof(g_tpText), "%s", text);
    g_tpTextUntil = until;
}

// Player position (read only). false on error.
bool SehTpPos(std::uintptr_t player, float* out) {
    __try {
        Vec4 pos{};
        g_tp.charGetWorldPos(&pos, reinterpret_cast<void*>(player));
        out[0] = pos.x;
        out[1] = pos.y;
        out[2] = pos.z;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The only writing teleport call: same arguments as the game's TeleportEvent handler for the local character
// (position + identity quaternion, both 16-byte aligned). false = exception (game functions off).
bool SehTpPlace(std::uintptr_t player, float x, float y, float z, bool mounted = false) {
    alignas(16) static Vec4 pos;
    alignas(16) static Vec4 quat;
    pos = Vec4{x, y, z, 0.0f};
    quat = Vec4{0.0f, 0.0f, 0.0f, 1.0f};
    if ((reinterpret_cast<std::uintptr_t>(&pos) & 15) != 0 || (reinterpret_cast<std::uintptr_t>(&quat) & 15) != 0) return true;
    __try {
        if (mounted) g_tp.ifaceTeleportMount(reinterpret_cast<void*>(Load<std::uintptr_t>(g_base + kAssassinIface)),
                                             reinterpret_cast<void*>(player), &pos);
        else g_tp.charTeleportQuat(reinterpret_cast<void*>(player), &pos, &quat, false, false);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The world's pause counter (map, menus). Read only.
bool SehWorldPaused() {
    __try {
        const auto world = reinterpret_cast<std::uintptr_t>(g_fn.getWorld());
        return world && Load<int>(world + 0x1458) > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SehWorldLoaded(std::uintptr_t world) {
    __try {
        return Load<std::uintptr_t>(world + 0xE8) && g_tp.worldIsLoaded(reinterpret_cast<void*>(world));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// World and player unchanged? (otherwise save switch/reload: abort, never jump back)
bool SehSameIdentity() {
    __try {
        const auto world = reinterpret_cast<std::uintptr_t>(g_fn.getWorld());
        return world && world == g_run.world && Load<std::uintptr_t>(world + 0x98) == g_run.player && g_fn.acpGate1();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void TpFinish(TpPhase phase, const char* text, std::uint64_t now) {
    g_run.phase = phase;
    g_run.finished = now;
    g_tpPhase.store(static_cast<int>(phase));
    TpSetText(text, now + 5000);
}

// One step of the jump sequence (every 100 ms). Returns whether this call wrote to the game.
bool TeleportStep(std::uint64_t now) {
    // Accept a request
    if (g_tpRequest.exchange(false)) {
        if (g_run.phase == TpPhase::Hold || g_run.phase == TpPhase::Watch || g_run.phase == TpPhase::ReturnHold) return false;
        TeleportInfo t{};
        SehTeleport(&t);
        char buf[160];
        if (g_tpState.load() != 1 || !t.valid) {
            TpFinish(TpPhase::Failed, "Teleport not available.", now);
            return false;
        }
        const bool quest = g_tpRequestKind.load() == static_cast<int>(TpTarget::Quest);
        if (quest && !t.questAvailable) {
            TpFinish(TpPhase::Failed, "Quest teleport not available for this game version.", now);
            return false;
        }
        const TpBlock block = quest ? t.questBlock : t.block;
        if ((block == TpBlock::Paused || block == TpBlock::Unavailable) && now < g_tpWaitUntil.load()) {  // map/menu open: keep the request
            g_tpRequest.store(true);
            TpSetText("Teleport ready - close the map to jump.", now + 60000);
            return false;
        }
        if (block != TpBlock::None) {
            std::snprintf(buf, sizeof(buf), "Teleport not possible: %s", TpBlockText(block));
            TpFinish(TpPhase::Failed, buf, now);
            return false;
        }
        const float dist = quest ? t.questDistance : t.distance;
        const float maxDist = g_tpMaxDistance.load(std::memory_order_relaxed);
        if (dist > maxDist) {
            std::snprintf(buf, sizeof(buf), "%s is %.0f m away - the range limit is %.0f m (see the teleport card).",
                          quest ? "Quest target" : "Marker", dist, maxDist);
            TpFinish(TpPhase::Failed, buf, now);
            return false;
        }
        g_run = TpRun{};
        g_run.world = reinterpret_cast<std::uintptr_t>(g_fn.getWorld());
        g_run.player = Load<std::uintptr_t>(g_run.world + 0x98);
        for (int i = 0; i < 3; ++i) {
            g_run.target[i] = quest ? t.questTarget[i] : t.target[i];
            g_run.origin[i] = t.player[i];
        }
        if (quest ? t.questEntity : t.markerEntity) {  // target is a character/object: land 2 m in front of it, not inside
            const float dx = t.player[0] - g_run.target[0], dy = t.player[1] - g_run.target[1];
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d > 0.5f) {
                g_run.target[0] += dx / d * 2.0f;
                g_run.target[1] += dy / d * 2.0f;
            }
        }
        // Deep water at the target: place on the water surface instead of 1 m above the marker Z
        float level = 0.0f;
        if (g_extState.load() == 1 && SehWaterAt(g_run.world, g_run.target, false, &level)) {
            g_run.water = true;
            g_run.target[2] = level;
            g_run.zOff = 0.0f;
        }
        g_run.distance = dist;
        g_run.mounted = t.mounted;
        g_run.start = g_run.lastPlace = g_run.lastStep = now;
        g_run.places = 1;
        if (!SehTpPlace(g_run.player, g_run.target[0], g_run.target[1], g_run.target[2] + g_run.zOff, g_run.mounted)) {
            g_faulted.store(true);
            TpFinish(TpPhase::Failed, "Error during the teleport - game functions disabled. Restart the game without saving.", now);
            return true;
        }
        g_run.phase = TpPhase::Hold;
        g_tpPhase.store(static_cast<int>(TpPhase::Hold));
        TpSetText("Teleporting...", now + 60000);
        return true;
    }

    const TpPhase ph = g_run.phase;
    if (ph != TpPhase::Hold && ph != TpPhase::Watch && ph != TpPhase::ReturnHold) return false;
    if (now - g_run.lastStep < 100) return false;
    g_run.lastStep = now;
    if (!SehSameIdentity()) {
        TpFinish(TpPhase::Aborted, "Teleport stopped (loading or save changed).", now);
        return false;
    }
    float pos[3];
    if (!SehTpPos(g_run.player, pos)) {
        TpFinish(TpPhase::Aborted, "Teleport stopped (player not readable).", now);
        return false;
    }
    const float* goal = ph == TpPhase::ReturnHold ? g_run.origin : g_run.target;
    const float zOff = ph == TpPhase::ReturnHold ? 0.3f : g_run.zOff;
    const float dz = pos[2] - goal[2];
    const float dx = pos[0] - goal[0], dy = pos[1] - goal[1];
    const float dxy = std::sqrt(dx * dx + dy * dy);

    if (ph == TpPhase::Hold || ph == TpPhase::ReturnHold) {
        // Hold until the world at the target is loaded: at least 1 s after the jump and 0.5 s continuously "loaded"
        const bool loaded = SehWorldLoaded(g_run.world);
        if (!loaded) g_run.loadedSince = 0;
        else if (g_run.loadedSince == 0) g_run.loadedSince = now;
        if (now - g_run.start >= 1000 && g_run.loadedSince && now - g_run.loadedSince >= 500) {
            // Water target: after loading, check the exact water height and re-place once (coarse grid near shores)
            if (ph == TpPhase::Hold && g_run.water && !g_run.refined) {
                g_run.refined = true;
                float level = 0.0f;
                if (g_extState.load() == 1 && SehWaterAt(g_run.world, g_run.target, true, &level) &&
                    std::fabs(level - g_run.target[2]) > 0.3f) {
                    g_run.target[2] = level;
                    g_run.lastPlace = now;
                    if (!SehTpPlace(g_run.player, g_run.target[0], g_run.target[1], level, g_run.mounted)) {
                        g_faulted.store(true);
                        TpFinish(TpPhase::Failed, "Error during the teleport - game functions disabled. Restart the game without saving.", now);
                    }
                    return true;
                }
            }
            g_run.phase = TpPhase::Watch;
            g_tpPhase.store(static_cast<int>(TpPhase::Watch));
            if (!g_run.returning) {
                char landed[96];
                std::snprintf(landed, sizeof(landed), g_run.water ? "Teleported %.0f m (water surface)." : "Teleported %.0f m.",
                              g_run.distance);
                TpSetText(landed, now + 5000);
            }
            g_run.watchStart = now;
            return false;
        }
        if (now - g_run.start > 20000 || g_run.places >= 60) {
            if (ph == TpPhase::ReturnHold) {
                TpFinish(TpPhase::Failed, "Teleport failed - could not settle at the start position.", now);
                return false;
            }
            g_run.returning = true;
            g_run.phase = TpPhase::ReturnHold;
        g_tpPhase.store(static_cast<int>(TpPhase::ReturnHold));
            g_run.start = g_run.lastPlace = now;
            g_run.places = 1;
            g_run.loadedSince = 0;
            if (!SehTpPlace(g_run.player, g_run.origin[0], g_run.origin[1], g_run.origin[2] + 0.3f, g_run.mounted)) {
                g_faulted.store(true);
                TpFinish(TpPhase::Failed, "Error during the teleport - game functions disabled. Restart the game without saving.", now);
            }
            TpSetText("Teleport did not settle - returning to the start position...", now + 60000);
            return true;
        }
        // If the character sinks well below the target (terrain not there yet): place it on the target again
        if (dz < -3.0f && now - g_run.lastPlace >= 250) {
            g_run.lastPlace = now;
            ++g_run.places;
            if (!SehTpPlace(g_run.player, goal[0], goal[1], goal[2] + zOff, g_run.mounted)) {
                g_faulted.store(true);
                TpFinish(TpPhase::Failed, "Error during the teleport - game functions disabled. Restart the game without saving.", now);
            }
            return true;
        }
        return false;
    }

    // Watch: observe for 3 s. Long fall -> go back; the user walks away -> done.
    if (dz < -40.0f) {
        if (g_run.returning) {
            TpFinish(TpPhase::Failed, "Teleport failed - still falling at the start position.", now);
            return false;
        }
        g_run.returning = true;
        g_run.phase = TpPhase::ReturnHold;
        g_tpPhase.store(static_cast<int>(TpPhase::ReturnHold));
        g_run.start = g_run.lastPlace = now;
        g_run.places = 1;
        g_run.loadedSince = 0;
        if (!SehTpPlace(g_run.player, g_run.origin[0], g_run.origin[1], g_run.origin[2] + 0.3f, g_run.mounted)) {
            g_faulted.store(true);
            TpFinish(TpPhase::Failed, "Error during the teleport - game functions disabled. Restart the game without saving.", now);
        }
        TpSetText("No ground at the marker - returning to the start position...", now + 60000);
        return true;
    }
    if (now - g_run.watchStart >= 3000 || dxy > 25.0f) {
        if (g_run.returning) {
            TpFinish(TpPhase::Failed, "Teleport failed - you are back at the start position.", now);
        } else {  // success message is already shown since landing; just finish the state
            g_run.phase = TpPhase::Done;
            g_run.finished = now;
            g_tpPhase.store(static_cast<int>(TpPhase::Done));
        }
    }
    return false;
}

// --- Analysis round 8: time scale of an entity (experimental) ---
// The entity's own factor a (time info at +238h, a at +0); 1.0 without time info. Read only.
float TimeScaleOf(std::uintptr_t e) {
    const auto ti = Load<std::uintptr_t>(e + 0x238);
    return ti ? Load<float>(ti) : 1.0f;
}

// Is the mount valid? (world, the game's animal controller with type check, back-link)
bool MountValid(std::uintptr_t m, std::uintptr_t world) {
    if (!m || Load<std::uintptr_t>(m + 0x60) != world) return false;
    const auto ctrl = reinterpret_cast<std::uintptr_t>(g_ext.animalCtrlOfEnt(reinterpret_cast<void*>(m)));
    return ctrl && Load<std::uintptr_t>(ctrl) == g_base + kVtblAnimalCtrl && Load<std::uintptr_t>(ctrl + 0x850) == m;
}

// One step: at most one set per call. Only factors > 0. A factor set by someone else (script) is never
// overwritten. off = reset everything. Returns whether it wrote to the game.
bool SehSpeedStep(bool off) {
    __try {
        if (!g_fn.acpGate1()) return false;
        const auto world = reinterpret_cast<std::uintptr_t>(g_fn.getWorld());
        if (!world) return false;
        if (world != g_tsWorld) {  // new world / reloaded: the old entities no longer exist
            g_tsWorld = world;
            g_tsPlayer = g_tsMount = 0;
            g_tsPlayerSet = g_tsMountSet = 1.0f;
        }
        const auto player = Load<std::uintptr_t>(world + 0x98);
        if (!player || Load<std::uintptr_t>(player + 0x60) != world) return false;
        // The player's mount
        std::uintptr_t mount = 0;
        if (void** hp = g_tp.riderMountHandle(reinterpret_cast<void*>(player + 0x1F8), true)) {
            const auto m = DerefHandle(reinterpret_cast<std::uintptr_t>(*hp));
            if (MountValid(m, world)) mount = m;
        }
        // Requested values
        const float mountWant = off ? 1.0f : g_mountScale.load(std::memory_order_relaxed);
        float playerWant = 1.0f;
        if (!off && !mount) {
            const float f = g_swimScale.load(std::memory_order_relaxed);
            if (f != 1.0f)
                if (void* st = g_tp.charStateObj(reinterpret_cast<void*>(player)); st && g_tp.charStateIsActive(st, 0x11)) playerWant = f;
        }
        // 1. reset the old mount (dismounted / switched)
        if (g_tsMount && g_tsMount != mount) {
            const auto old = g_tsMount;
            g_tsMount = 0;
            if (MountValid(old, world) && TimeScaleOf(old) == g_tsMountSet) {
                g_tsMountSet = 1.0f;
                g_ext.entSetTimeScale(reinterpret_cast<void*>(old), 1.0f);
                return true;
            }
            g_tsMountSet = 1.0f;
        }
        // 2. mount
        if (mount) {
            const float cur = TimeScaleOf(mount);
            const bool ours = g_tsMount == mount && cur == g_tsMountSet;
            if (cur != mountWant && (ours || cur == 1.0f) && mountWant > 0.0f) {
                g_ext.entSetTimeScale(reinterpret_cast<void*>(mount), mountWant);
                g_tsMount = mountWant != 1.0f ? mount : 0;
                g_tsMountSet = mountWant;
                return true;
            }
        }
        // 3. player (only while swimming on the surface)
        if (g_tsPlayer && g_tsPlayer != player) g_tsPlayer = 0;  // different player: do not touch the old entity
        const float cur = TimeScaleOf(player);
        const bool ours = g_tsPlayer == player && cur == g_tsPlayerSet;
        if (cur != playerWant && (ours || cur == 1.0f) && playerWant > 0.0f) {
            g_ext.entSetTimeScale(reinterpret_cast<void*>(player), playerWant);
            g_tsPlayer = playerWant != 1.0f ? player : 0;
            g_tsPlayerSet = playerWant;
            return true;
        }
        if (!ours && g_tsPlayer == player) g_tsPlayer = 0;  // a script took over
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_extState.store(2);
        g_tsPlayer = g_tsMount = 0;
        return false;
    }
}

// Read the weather and reconcile it with the user's request (analysis round 7): pause randomness + set target, or release.
// Mirrors the game's own script action (target + pause, undone at the end). Never count below 0.
void SehWeather(GameInfo* r) {
    __try {
        if (!g_fn.acpGate1()) return;
        const auto world = reinterpret_cast<std::uintptr_t>(g_fn.getWorld());
        const std::uintptr_t tod = CurrentTod();
        if (!world || !tod || Load<std::uintptr_t>(tod + 0x240) != g_base + kWeatherState) return;
        void* mgr = reinterpret_cast<void*>(tod);
        const auto climate = Load<std::uintptr_t>(world + 0x108);
        r->climateOff = climate == 0 || Load<unsigned char>(climate + 0xA4) == 0;
        const bool paused = g_fn.wxIsRandPaused(mgr);
        const bool wantHold = g_wxWantHold.load(std::memory_order_relaxed);

        if (wantHold && !r->climateOff) {
            if (!g_wxWeHold) {
                if (!paused) {  // take over as long as no script holds the weather
                    g_fn.wxSetRandPaused(mgr, true);
                    g_wxWeHold = true;
                    g_wxLastTarget = -1.0f;
                }
            } else if (!paused) {  // world rebuilt (counter reset): take over again
                g_fn.wxSetRandPaused(mgr, true);
                g_wxLastTarget = -1.0f;
            }
            const float want = g_wxWantValue.load(std::memory_order_relaxed);
            if (g_wxWeHold && want != g_wxLastTarget) {
                g_fn.wxSetTarget(mgr, want, g_wxSeconds.load(std::memory_order_relaxed));
                g_wxLastTarget = want;
            }
        } else if (g_wxWeHold) {
            if (g_fn.wxIsRandPaused(mgr)) g_fn.wxSetRandPaused(mgr, false);  // hand back to the game
            g_wxWeHold = false;
            g_wxLastTarget = -1.0f;
        }
        r->weatherHeld = g_wxWeHold;
        r->weatherScripted = !g_wxWeHold && g_fn.wxIsRandPaused(mgr);
        r->weather = g_fn.wxGetCurrent(mgr);
        r->weatherValid = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faulted.store(true);
    }
}

void FormatResult(const PendingAction& a, const ActOut& o, char* buf, std::size_t n) {
    switch (o.r) {
        case ActResult::NotReady: std::snprintf(buf, n, "Not possible right now (menu, loading or cutscene) - try again in the game world."); return;
        case ActResult::NoPlayer: std::snprintf(buf, n, "No player loaded."); return;
        case ActResult::MultiplierZero: std::snprintf(buf, n, "XP gain multiplier is 0 %% - the game would grant nothing."); return;
        case ActResult::NoTod: std::snprintf(buf, n, "No time of day in this area."); return;
        case ActResult::Fault: std::snprintf(buf, n, "Error during the game call - game functions disabled for this session."); return;
        case ActResult::AlreadyThere: std::snprintf(buf, n, "Already level %u or higher.", o.before); return;
        case ActResult::Ok: break;
    }
    switch (a.action) {
        case Action::SetTime: {
            const int h = static_cast<int>(a.hours) % 24;
            const int m = static_cast<int>((a.hours - static_cast<float>(static_cast<int>(a.hours))) * 60.0f + 0.5f) % 60;
            std::snprintf(buf, n, "Time set to %02d:%02d.", h, m);
            return;
        }
        case Action::GrantXp:
            if (o.after == o.before) std::snprintf(buf, n, "The game granted no XP (maximum level reached or level cap active?).");
            else std::snprintf(buf, n, "+%u XP (now %u XP).", o.after - o.before, o.after);
            return;
        case Action::LevelAtLeast:
            if (o.after == o.before) std::snprintf(buf, n, "Level unchanged (%u) - level cap active?", o.after);
            else std::snprintf(buf, n, "Level %u -> %u (ability points for each level were granted by the game).", o.before, o.after);
            return;
        case Action::GrantPoints:
            std::snprintf(buf, n, "+%u ability point(s) (now %u available).", o.sent, o.after);
            return;
    }
}

void Publish(const char* text) {
    // The detour holds g_actMutex. Publish without blocking; otherwise next time.
    std::unique_lock lock(g_resultMutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        if (text != g_unpublished) std::snprintf(g_unpublished, sizeof(g_unpublished), "%s", text);
        g_hasUnpublished = true;
        return;
    }
    std::snprintf(g_resultText, sizeof(g_resultText), "%s", text);
    ++g_resultSeq;
    g_hasUnpublished = false;
}

// Executes a pending action. true = something in the game was changed (read again afterwards).
bool RunPendingAction(std::uint64_t now) {
    std::unique_lock lock(g_actMutex, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    if (g_hasUnpublished) Publish(g_unpublished);
    if (!g_pending.set) return false;
    const PendingAction a = g_pending;
    g_pending.set = false;
    char buf[192];
    if (now > a.expires) {
        std::snprintf(buf, sizeof(buf), "Not executed - not possible right now (menu, loading or cutscene?). Try again in the game world.");
        Publish(buf);
        return false;
    }
    ActOut o;
    SehAct(&a, &o);
    if (o.r == ActResult::NotReady) {
        g_pending = a;  // gate currently closed (menu, loading screen): retry until it expires
        return false;
    }
    FormatResult(a, o, buf, sizeof(buf));
    Publish(buf);
    return o.r == ActResult::Ok;
}

}  // namespace

void SetEnabled(bool enabled) { g_enabled.store(enabled, std::memory_order_release); }

BridgeState State() { return g_state.load(); }

const char* StateText(BridgeState s) {
    switch (s) {
        case BridgeState::Disabled: return "off";
        case BridgeState::WaitingForAcp: return "waiting for a 'Hacked Animus' save";
        case BridgeState::Ready: return "active";
        case BridgeState::Unsupported: return "not available for this game version";
        case BridgeState::Failed: return "disabled after an error (see log)";
    }
    return "?";
}

const char* TimeStopText(TimeStop s) {
    switch (s) {
        case TimeStop::None: return "running";
        case TimeStop::QuestPause: return "stopped by the current quest";
        case TimeStop::CameraStill: return "paused while the camera is still (up to 15 s)";
        case TimeStop::WorldPaused: return "game paused";
    }
    return "?";
}

void LifecycleTick(bool acpActive, bool anyHost) {
    BridgeState s = g_state.load();
    if (s == BridgeState::Unsupported || s == BridgeState::Failed) {
        // final for this session
    } else if (g_faulted.load()) {
        s = BridgeState::Failed;
    } else if (!g_enabled.load()) {
        s = BridgeState::Disabled;
    } else if (s != BridgeState::Ready) {
        if (anyHost) {
            s = BridgeState::Unsupported;  // the self-test host is not the game
        } else if (acpActive) {
            // Strict reading: verify only once the game reports ACP active.
            wchar_t exe[MAX_PATH]{};
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            const auto st = platform::StatFile(exe);
            g_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            std::string detail;
            if (!st || st->size != kExeSize) {
                s = BridgeState::Unsupported;
                platform::log::Write("Game functions: unknown game build - off");
            } else if (const int bad = VerifyAndResolve(detail); bad != 0) {
                s = BridgeState::Unsupported;
                platform::log::Write("Game functions: " + std::to_string(bad) + " checksum(s) mismatched: " + detail + "- off");
            } else {
                s = BridgeState::Ready;
                platform::log::Write("Game functions: " + std::to_string(Table().size()) + "/" +
                                     std::to_string(Table().size()) + " checksums ok");
                std::string tpDetail;
                if (const int tpBad = VerifyAndResolveTp(tpDetail); tpBad == 0) {
                    g_tpState.store(1);
                    platform::log::Write("Teleport (8a): " + std::to_string(TpTable().size()) + "/" +
                                         std::to_string(TpTable().size()) + " checksums ok");
                } else {
                    g_tpState.store(2);
                    platform::log::Write("Teleport: " + std::to_string(tpBad) + " checksum(s) mismatched: " + tpDetail + "- teleport off");
                }
                std::string extDetail;
                if (const int extBad = VerifyAndResolveExt(extDetail); extBad == 0) {
                    g_extState.store(1);
                    platform::log::Write("Round 8 (water, quest target, time scale): " + std::to_string(ExtTable().size()) + "/" +
                                         std::to_string(ExtTable().size()) + " checksums ok, " + std::to_string(ExtCalls().size()) +
                                         " call targets ok");
                } else {
                    g_extState.store(2);
                    platform::log::Write("Round 8: " + std::to_string(extBad) + " checksum(s) mismatched: " + extDetail + "- off");
                }
            }
        } else {
            s = BridgeState::WaitingForAcp;
        }
    }
    g_state.store(s);
    if (s != g_loggedState) {
        if (s == BridgeState::Failed) platform::log::Write("Game functions: exception during access - off for this session");
        g_loggedState = s;
    }
}

void OnPreStep(bool acpActive) noexcept {
    if (g_state.load(std::memory_order_acquire) != BridgeState::Ready || g_faulted.load(std::memory_order_relaxed)) return;
    const std::uint64_t now = GetTickCount64();
    const bool speedOk = acpActive && g_extState.load() == 1 && g_tpState.load() == 1;
    if (!g_enabled.load(std::memory_order_relaxed)) {
        // Game functions turned off: only reset time scales that are still set
        if (speedOk && g_tsActive.load() && now >= g_tsNext) {
            g_tsNext = now + 100;
            SehSpeedStep(true);
            g_tsActive.store(g_tsPlayer != 0 || g_tsMount != 0);
        }
        return;
    }
    if (acpActive && g_tpEnabled.load(std::memory_order_relaxed) && g_tpState.load() == 1) {
        g_paused.store(SehWorldPaused());
        g_pausedTick.store(now);
    }
    const bool tpWrote = acpActive && g_tpEnabled.load(std::memory_order_relaxed) && g_tpState.load() == 1 && TeleportStep(now);
    bool wrote = tpWrote;
    if (!tpWrote && acpActive && RunPendingAction(now)) {
        g_nextRead.store(0, std::memory_order_relaxed);  // show the result immediately
        wrote = true;
    }
    if (!wrote && speedOk && now >= g_tsNext) {  // at most one writing game call per PreStep
        g_tsNext = now + 100;
        SehSpeedStep(false);
        g_tsActive.store(g_tsPlayer != 0 || g_tsMount != 0);
    }
    if (acpActive) ClockTick(now);
    if (g_faulted.load(std::memory_order_relaxed)) return;
    if (now < g_nextRead.load(std::memory_order_relaxed)) return;
    g_nextRead.store(now + 200, std::memory_order_relaxed);

    GameInfo r{};
    const bool ok = acpActive && SehReadGame(&r);
    const bool hold = g_holdOn.load(std::memory_order_relaxed);
    if (ok && hold) SehHold(g_holdHours.load(std::memory_order_relaxed), r.hours);
    r.holding = hold;
    if (ok) SehWeather(&r);
    if (ok && g_tpEnabled.load(std::memory_order_relaxed) && g_tpState.load() == 1) SehTeleport(&r.teleport);
    r.ownClock = !hold && g_clockActive.load(std::memory_order_relaxed);
    std::unique_lock lock(g_infoMutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    if (ok) {
        r.tick = now;
        g_info = r;
    } else {
        g_info.valid = false;
    }
}

bool Read(GameInfo& out) {
    std::lock_guard lock(g_infoMutex);
    out = g_info;
    return out.valid && GetTickCount64() - out.tick < 2000 && g_state.load() == BridgeState::Ready;
}

void Request(Action a, int value, float hours, float xpMultiplierPct) {
    {
        std::lock_guard lock(g_resultMutex);
        g_resultIsTime = a == Action::SetTime;
    }
    if (g_state.load() != BridgeState::Ready || !g_enabled.load()) {
        std::lock_guard lock(g_resultMutex);
        std::snprintf(g_resultText, sizeof(g_resultText), "Game functions are not active (%s).", StateText(g_state.load()));
        ++g_resultSeq;
        return;
    }
    std::lock_guard lock(g_actMutex);
    g_pending.set = true;
    g_pending.action = a;
    g_pending.value = value;
    g_pending.hours = hours < 0.0f ? 0.0f : hours >= 24.0f ? 23.99f : hours;
    g_pending.xpMultiplierPct = xpMultiplierPct;
    g_pending.expires = GetTickCount64() + 5000;
}

void SetHold(bool on, float hours) {
    g_holdHours.store(hours < 0.0f ? 0.0f : hours >= 24.0f ? 23.99f : hours, std::memory_order_relaxed);
    g_holdOn.store(on, std::memory_order_relaxed);
}

void SetTeleportEnabled(bool enabled) { g_tpEnabled.store(enabled, std::memory_order_relaxed); }

void TeleportRequest(TpTarget target, bool waitForMap) {
    g_tpRequestKind.store(static_cast<int>(target));
    g_tpWaitUntil.store(waitForMap ? GetTickCount64() + 20000 : 0);
    TpSetText("Teleporting...", GetTickCount64() + 60000);  // visible immediately, not only at the next game step
    g_tpRequest.store(true);
}

void SetTeleportMaxDistance(float meters) { g_tpMaxDistance.store(meters, std::memory_order_relaxed); }

void SetTeleportOptions(bool allowCombat, bool allowMounted) {
    g_tpAllowCombat.store(allowCombat, std::memory_order_relaxed);
    g_tpAllowMounted.store(allowMounted, std::memory_order_relaxed);
}

bool TeleportBusy() {
    const auto ph = static_cast<TpPhase>(g_tpPhase.load());
    return g_tpRequest.load() || ph == TpPhase::Hold || ph == TpPhase::Watch || ph == TpPhase::ReturnHold;
}

bool TeleportStatus(char* buf, std::size_t n) {
    std::lock_guard lock(g_tpTextMutex);
    if (!g_tpText[0] || GetTickCount64() > g_tpTextUntil) return false;
    std::snprintf(buf, n, "%s", g_tpText);
    return true;
}

const char* TpBlockText(TpBlock b) {
    switch (b) {
        case TpBlock::None: return "ready";
        case TpBlock::Unavailable: return "not available right now";
        case TpBlock::NotLocalPlayer: return "player not found";
        case TpBlock::NoMarker: return "no custom marker set on the map";
        case TpBlock::MarkerUnreadable: return "marker found, position not readable";
        case TpBlock::OtherWorld: return "marker is on another map";
        case TpBlock::Loading: return "loading";
        case TpBlock::WorldNotLoaded: return "world still loading";
        case TpBlock::Paused: return "game paused";
        case TpBlock::FastTravelLocked: return "fast travel is blocked here (mission, arena, ...)";
        case TpBlock::QuestBlocks: return "the current quest forbids travel";
        case TpBlock::InCombat: return "in combat";
        case TpBlock::EagleControl: return "controlling Senu";
        case TpBlock::Mounted: return "mounted / on a boat - dismount first";
        case TpBlock::TooFar: return "target too far away (over 30 km)";
        case TpBlock::TooClose: return "already at the target";
        case TpBlock::NoQuestTracked: return "no quest is tracked";
        case TpBlock::NoQuestTarget: return "the tracked quest shows no target on the map";
        case TpBlock::QuestOtherRegion: return "the quest target is in another region";
    }
    return "?";
}

void SetWeather(bool hold, float intensity, float seconds) {
    g_wxWantValue.store(intensity < 0.0f ? 0.0f : intensity > 1.0f ? 1.0f : intensity, std::memory_order_relaxed);
    g_wxSeconds.store(seconds < 0.0f ? 0.0f : seconds, std::memory_order_relaxed);
    g_wxWantHold.store(hold, std::memory_order_relaxed);
}

void SetOwnClock(bool on, const float minutes[4]) {
    for (int i = 0; i < 4; ++i) g_clockMin[i].store(minutes[i], std::memory_order_relaxed);
    g_clockOn.store(on, std::memory_order_relaxed);
}

void SetSpeedBoost(float swimFactor, float mountFactor) {
    if (g_tsShutdown.load()) return;
    auto clamp = [](float f) { return f < 1.0f ? 1.0f : f > 4.0f ? 4.0f : f; };
    g_swimScale.store(clamp(swimFactor), std::memory_order_relaxed);
    g_mountScale.store(clamp(mountFactor), std::memory_order_relaxed);
}

bool GamePausedNow() { return g_paused.load() && GetTickCount64() - g_pausedTick.load() < 500; }

bool SpeedBoostActive() { return g_tsActive.load(); }

bool SpeedBoostAvailable() { return g_state.load() == BridgeState::Ready && g_extState.load() == 1 && g_tpState.load() == 1; }

void ReleaseSpeedBoost() {
    g_tsShutdown.store(true);
    g_swimScale.store(1.0f);
    g_mountScale.store(1.0f);
}

std::string LastResult(std::uint32_t* seq, bool* isTime) {
    std::lock_guard lock(g_resultMutex);
    if (seq) *seq = g_resultSeq;
    if (isTime) *isTime = g_resultIsTime;
    return g_resultText;
}

}  // namespace acp::game
