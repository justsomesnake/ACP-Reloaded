// Uplay options channel (analysis round 3): hooks on exports of uplay_r1_loader64.dll.
//  - UPLAY_OPTIONS_SetInGameState: pass-through; remembers whether the game accepts ACP (1 = Hacked Animus + in game)
//  - UPLAY_OPTIONS_Enumerate:      pass-through; capture only
//  - UPLAY_GetNextEvent:           pass-through; when ACP is accepted, delivers pending values as event 80000
// Detours: no allocation, no file I/O, no logger (ring buffer only), never blocking.
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <cmath>
#include <optional>
#include <utility>
#include <vector>

#include "MinHook.h"
#include "ini_document.h"
#include "hooks.h"
#include "game_bridge.h"
#include "platform.h"

namespace acp::hook {
namespace {

// --- Structures from static analysis (private notes, not included) ---
struct UplayEvent {
    std::uint32_t type;
    std::uint32_t pad;
    void* data;
};
struct KeyValue {
    const char* key;
    const char* value;
};
struct KeyValueList {
    std::uint32_t count;
    std::uint32_t pad;
    KeyValue** items;
};
struct OptionsEvent {
    std::uint64_t unknown0;  // not read by the game
    KeyValueList* list;
};
constexpr std::uint32_t kEventOptions = 80000;

using GetNextEvent_t = bool (*)(UplayEvent*);
using SetInGameState_t = int (*)(std::uint32_t);
using Enumerate_t = int (*)(std::uint32_t, void**, void*);
using Set_t = int (*)(KeyValueList*, const char*, const char*);
using Apply_t = int (*)(std::uint32_t, KeyValueList*, void*);
using Release_t = int (*)(KeyValueList*);
using HasOverlapped_t = bool (*)(void*);

GetNextEvent_t g_oGetNextEvent = nullptr;
SetInGameState_t g_oSetInGameState = nullptr;
Enumerate_t g_oEnumerate = nullptr;
Set_t g_oSet = nullptr;
Apply_t g_oApply = nullptr;
Release_t g_oRelease = nullptr;
HasOverlapped_t g_hasOverlapped = nullptr;  // not hooked, only for the self-test

// --- Reverse direction (stage c): the game reports its save-game values via Enumerate/Set/Apply ---
std::atomic<bool> g_emulate{false};     // Set/Apply/Release hooks active
std::atomic<bool> g_ourListOut{false};  // our list is currently with the game
KeyValueList g_ourList{};               // placeholder list (the game does not read it)
std::mutex g_captureMutex;              // leaf mutex: work and result buffers
std::array<std::optional<ParamValue>, kParamCount> g_working{};
std::array<std::optional<ParamValue>, kParamCount> g_captured{};
std::atomic<bool> g_captureReady{false};

std::atomic<UplayMode> g_mode{UplayMode::Off};
std::atomic<bool> g_hooked{false};
std::atomic<int> g_gameState{-1};  // -1 unknown, 0 not accepting, 1 ACP active

// --- Pending values (leaf mutex; detour uses try_lock only) ---
std::mutex g_pendingMutex;
std::array<std::optional<ParamValue>, kParamCount> g_pending{};
std::atomic<bool> g_hasPending{false};

// Under g_pendingMutex: last seen values (basis for "send only changes") and values reported by the
// game that have not yet arrived in the snapshot (do not send old values back).
std::array<ParamValue, kParamCount> g_lastValues{};
bool g_baselineSet = false;
std::array<std::optional<ParamValue>, kParamCount> g_fromGame{};
std::uint64_t g_fromGameUntil = 0;

// --- Event packets: fixed, rotated, never freed (the game copies them immediately) ---
struct Packet {
    OptionsEvent ev;
    KeyValueList list;
    std::array<KeyValue, kParamCount> kv;
    std::array<KeyValue*, kParamCount> ptrs;
    std::array<std::array<char, 32>, kParamCount> values;
};
std::array<Packet, 4> g_packets{};
std::atomic<unsigned> g_packetIdx{0};

// --- Capture ring (detours write, lifecycle thread reads) ---
enum class Rec : std::uint32_t { EventHit = 1, SetInGameState, Enumerate, Injected, EnumEmulated, ApplyCaptured };
struct LogRec {
    std::atomic<std::uint64_t> seq{0};
    Rec kind{};
    std::uint32_t tid = 0;
    std::uint64_t a = 0, b = 0, tick = 0;
};
constexpr std::size_t kRing = 512;
std::array<LogRec, kRing> g_ring;
std::atomic<std::uint64_t> g_head{0};
std::uint64_t g_tail = 0;  // lifecycle thread only

std::atomic<std::uint64_t> g_calls{0}, g_hits{0}, g_injected{0};

void Record(Rec kind, std::uint64_t a, std::uint64_t b) noexcept {
    const std::uint64_t idx = g_head.fetch_add(1, std::memory_order_acq_rel);
    LogRec& r = g_ring[idx % kRing];
    r.kind = kind;
    r.tid = GetCurrentThreadId();
    r.a = a;
    r.b = b;
    r.tick = GetTickCount64();
    r.seq.store(idx + 1, std::memory_order_release);
}

// Value in the game's format without allocation (strings, "true"/"false", dot as decimal separator).
void FormatInto(const ParamValue& v, std::array<char, 32>& out) noexcept {
    char* b = out.data();
    char* e = b + out.size() - 1;
    char* p = b;
    switch (v.type) {
        case ParamType::Bool: {
            const char* s = v.b ? "true" : "false";
            const std::size_t n = std::strlen(s);
            std::memcpy(b, s, n);
            p = b + n;
            break;
        }
        case ParamType::Int: p = std::to_chars(b, e, v.i).ptr; break;
        case ParamType::Float: {
            p = std::to_chars(b, e, v.f == 0.0f ? 0.0f : v.f, std::chars_format::fixed, 4).ptr;
            char* dot = std::find(b, p, '.');
            if (dot != p) {
                while (p > dot && p[-1] == '0') --p;
                if (p == dot + 1) p = dot;
            }
            break;
        }
    }
    *p = '\0';
}

bool HkGetNextEvent(UplayEvent* ev) {
    InFlight guard(G().inFlightUplay);
    const bool r = g_oGetNextEvent(ev);
    if (!ev || G().phase.load(std::memory_order_acquire) != Phase::Running) return r;
    g_calls.fetch_add(1, std::memory_order_relaxed);
    if (r) {
        g_hits.fetch_add(1, std::memory_order_relaxed);
        Record(Rec::EventHit, ev->type, reinterpret_cast<std::uint64_t>(ev->data));
        return r;
    }
    game::OnPreStep(g_gameState.load(std::memory_order_acquire) == 1);  // game task PreStep: the only place for game access
    if (g_mode.load(std::memory_order_relaxed) != UplayMode::Inject || g_gameState.load(std::memory_order_acquire) != 1 ||
        !g_hasPending.load(std::memory_order_acquire))
        return r;

    std::unique_lock lock(g_pendingMutex, std::try_to_lock);
    if (!lock.owns_lock()) return r;
    Packet& pk = g_packets[g_packetIdx.fetch_add(1, std::memory_order_relaxed) % g_packets.size()];
    std::uint32_t n = 0;
    std::uint64_t firstKey = 0;
    for (std::size_t i = 0; i < kParamCount; ++i) {
        if (!g_pending[i]) continue;
        const ParamDesc& d = ParamTable()[i];
        if (!d.readOnly && !d.local) {
            if (n == 0) firstKey = i;
            FormatInto(*g_pending[i], pk.values[n]);
            pk.kv[n].key = d.key.data();  // literal from the table, null-terminated
            pk.kv[n].value = pk.values[n].data();
            pk.ptrs[n] = &pk.kv[n];
            ++n;
        }
        g_pending[i].reset();
    }
    g_hasPending.store(false, std::memory_order_release);
    lock.unlock();
    if (n == 0) return r;

    pk.list.count = n;
    pk.list.pad = 0;
    pk.list.items = pk.ptrs.data();
    pk.ev.unknown0 = 0;
    pk.ev.list = &pk.list;
    ev->type = kEventOptions;
    ev->pad = 0;
    ev->data = &pk.ev;
    g_injected.fetch_add(1, std::memory_order_relaxed);
    Record(Rec::Injected, n, firstKey);
    return true;
}

int HkSetInGameState(std::uint32_t flags) {
    InFlight guard(G().inFlightUplay);
    g_gameState.store(flags ? 1 : 0, std::memory_order_release);
    Record(Rec::SetInGameState, flags, 0);
    return g_oSetInGameState(flags);
}

// Mark the overlapped as completed (layout: +4 bytes "done", +8 result) and cross-check it with the real
// client function so that the game's state machine never gets stuck.
bool CompleteOverlapped(void* ovl) noexcept {
    auto* b = static_cast<unsigned char*>(ovl);
    b[4] = 1;
    *reinterpret_cast<std::uint32_t*>(b + 8) = 0;
    return g_hasOverlapped ? g_hasOverlapped(ovl) : false;
}

int HkEnumerate(std::uint32_t handle, void** outList, void* overlapped) {
    InFlight guard(G().inFlightUplay);
    const int r = g_oEnumerate(handle, outList, overlapped);
    Record(Rec::Enumerate, handle, static_cast<std::uint64_t>(static_cast<std::uint32_t>(r)));
    // Only if the client does not deliver (anymore): emulate the reverse direction.
    if (r != 0 || !outList || !overlapped || !g_emulate.load() || G().phase.load() != Phase::Running ||
        g_mode.load() != UplayMode::Inject)
        return r;
    if (g_ourListOut.exchange(true)) return r;  // a list is already out
    {
        std::lock_guard lock(g_captureMutex);
        g_working = {};
    }
    g_ourList = {};
    if (!CompleteOverlapped(overlapped)) {
        g_ourListOut = false;
        return r;  // self-test failed: behave as without the hook
    }
    *outList = &g_ourList;
    Record(Rec::EnumEmulated, handle, 0);
    return 1;
}

int HkSet(KeyValueList* list, const char* key, const char* value) {
    InFlight guard(G().inFlightUplay);
    if (list != &g_ourList || !g_ourListOut.load()) return g_oSet(list, key, value);
    if (key && value) {
        if (const auto id = FindParam(key)) {
            if (const auto v = IniDocument::ParseValue(Desc(*id).type, value)) {
                std::lock_guard lock(g_captureMutex);
                g_working[static_cast<std::size_t>(*id)] = *v;
            }
        }
    }
    return 1;
}

int HkApply(std::uint32_t handle, KeyValueList* list, void* overlapped) {
    InFlight guard(G().inFlightUplay);
    if (list != &g_ourList || !g_ourListOut.load()) return g_oApply(handle, list, overlapped);
    std::uint64_t n = 0;
    {
        std::lock_guard lock(g_captureMutex);
        g_captured = g_working;
        for (const auto& v : g_captured) n += v.has_value();
    }
    g_captureReady.store(true, std::memory_order_release);
    if (overlapped) CompleteOverlapped(overlapped);
    Record(Rec::ApplyCaptured, n, 0);
    return 1;
}

int HkRelease(KeyValueList* list) {
    InFlight guard(G().inFlightUplay);
    if (list == &g_ourList) {
        g_ourListOut = false;
        return 1;
    }
    return g_oRelease(list);
}

template <std::size_t N>
bool PrologueIs(void* fn, const std::array<unsigned char, N>& bytes) {
    return std::memcmp(fn, bytes.data(), N) == 0;
}

template <std::size_t N>
bool HookExport(HMODULE m, const char* name, void* detour, void** original, const std::array<unsigned char, N>& prologue,
                std::string& info) {
    void* fn = reinterpret_cast<void*>(GetProcAddress(m, name));
    if (!fn) {
        info += std::string(name) + " missing; ";
        return false;
    }
    if (!PrologueIs(fn, prologue)) {
        info += std::string(name) + ": unexpected prologue (already hooked or different build); ";
        return false;
    }
    if (MH_CreateHook(fn, detour, original) != MH_OK) {
        info += std::string(name) + ": MH_CreateHook failed; ";
        return false;
    }
    MH_QueueEnableHook(fn);
    RegisterHookTarget(fn);
    return true;
}

}  // namespace

bool CreateUplayHooks(UplayMode mode, bool anyHost, std::string& info) {
    g_mode = mode;
    if (mode == UplayMode::Off) {
        info = "Uplay channel: off";
        return false;
    }
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(0, L"uplay_r1_loader64.dll", &m) || !m) {
        info = "Uplay channel: uplay_r1_loader64.dll not loaded";
        return false;
    }
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(m, path, MAX_PATH);
    const auto st = platform::StatFile(path);
    if (!anyHost && (!st || st->size != 416768ull)) {
        info = "Uplay channel: unknown uplay_r1_loader64.dll (size " + std::to_string(st ? st->size : 0) + ") - no hook";
        return false;
    }
    std::string err;
    static constexpr std::array<unsigned char, 6> kPrologA{0x40, 0x53, 0x48, 0x83, 0xEC, 0x20};
    static constexpr std::array<unsigned char, 5> kPrologB{0x48, 0x89, 0x5C, 0x24, 0x08};
    const bool a = HookExport(m, "UPLAY_OPTIONS_SetInGameState", reinterpret_cast<void*>(&HkSetInGameState),
                              reinterpret_cast<void**>(&g_oSetInGameState), kPrologA, err);
    const bool b = HookExport(m, "UPLAY_GetNextEvent", reinterpret_cast<void*>(&HkGetNextEvent),
                              reinterpret_cast<void**>(&g_oGetNextEvent), kPrologA, err);
    HookExport(m, "UPLAY_OPTIONS_Enumerate", reinterpret_cast<void*>(&HkEnumerate), reinterpret_cast<void**>(&g_oEnumerate),
               kPrologB, err);
    g_hasOverlapped = reinterpret_cast<HasOverlapped_t>(GetProcAddress(m, "UPLAY_HasOverlappedOperationCompleted"));
    const bool c = HookExport(m, "UPLAY_OPTIONS_Set", reinterpret_cast<void*>(&HkSet), reinterpret_cast<void**>(&g_oSet),
                              kPrologB, err);
    const bool d = HookExport(m, "UPLAY_OPTIONS_Apply", reinterpret_cast<void*>(&HkApply),
                              reinterpret_cast<void**>(&g_oApply), kPrologB, err);
    const bool e = HookExport(m, "UPLAY_OPTIONS_ReleaseKeyValueList", reinterpret_cast<void*>(&HkRelease),
                              reinterpret_cast<void**>(&g_oRelease), kPrologA, err);
    g_emulate = c && d && e && g_oEnumerate && g_hasOverlapped;
    g_hooked = a && b;
    info = std::string("Uplay channel: ") + (g_hooked ? (mode == UplayMode::Inject ? (g_emulate ? "hooks active (injection + save-game values)" : "hooks active (injection on)") : "hooks active (capture only)")
                                                    : "not available") +
           (err.empty() ? "" : " - " + err);
    return g_hooked;
}

void UplayOnValues(const std::array<ParamValue, kParamCount>& values) {
    std::lock_guard lock(g_pendingMutex);
    if (!g_baselineSet) {
        g_lastValues = values;
        g_baselineSet = true;
        return;
    }
    const bool fromGameActive = GetTickCount64() < g_fromGameUntil;
    bool any = false;
    for (std::size_t i = 0; i < kParamCount; ++i) {
        if (g_fromGame[i]) {
            // Value reported by the game: wait until it arrives in the snapshot; do not send old values back.
            if (values[i] == *g_fromGame[i] || !fromGameActive) g_fromGame[i].reset();
            else continue;
        }
        if (!(values[i] == g_lastValues[i])) {
            g_pending[i] = values[i];
            any = true;
        }
        g_lastValues[i] = values[i];
    }
    if (any) g_hasPending.store(true, std::memory_order_release);
}

std::vector<std::pair<ParamId, ParamValue>> UplayTakeGameValues() {
    std::vector<std::pair<ParamId, ParamValue>> out;
    if (!g_captureReady.exchange(false, std::memory_order_acq_rel)) return out;
    std::array<std::optional<ParamValue>, kParamCount> cap;
    {
        std::lock_guard lock(g_captureMutex);
        cap = g_captured;
    }
    std::lock_guard lock(g_pendingMutex);
    g_fromGameUntil = GetTickCount64() + 3000;
    for (std::size_t i = 0; i < kParamCount; ++i) {
        if (!cap[i] || ParamTable()[i].readOnly) continue;
        ParamValue v = *cap[i];
        // The game reports decimals truncated ("%d"): keep our own value if it matches.
        if (v.type == ParamType::Float && g_baselineSet && std::trunc(g_lastValues[i].f) == v.f) v = g_lastValues[i];
        g_pending[i].reset();
        g_fromGame[i] = v;
        g_lastValues[i] = v;
        out.emplace_back(static_cast<ParamId>(i), v);
    }
    return out;
}

void UplayPushAll(const std::array<ParamValue, kParamCount>& values) {
    std::lock_guard lock(g_pendingMutex);
    for (std::size_t i = 0; i < kParamCount; ++i) g_pending[i] = values[i];
    g_hasPending.store(true, std::memory_order_release);
}

bool UplayHooked() { return g_hooked.load(); }
int UplayGameState() { return g_gameState.load(); }

void UplayDrainLog() {
    static std::uint64_t nextStats = GetTickCount64() + 10000;
    for (;;) {
        const std::uint64_t head = g_head.load(std::memory_order_acquire);
        if (g_tail >= head) break;
        if (head - g_tail > kRing) g_tail = head - kRing;  // overrun: discard older entries
        LogRec& r = g_ring[g_tail % kRing];
        if (r.seq.load(std::memory_order_acquire) != g_tail + 1) break;  // not completely written yet
        char line[160];
        switch (r.kind) {
            case Rec::EventHit:
                std::snprintf(line, sizeof(line), "UPLAY tid=%u GetNextEvent -> type=%llu data=%p", r.tid,
                              static_cast<unsigned long long>(r.a), reinterpret_cast<void*>(r.b));
                break;
            case Rec::SetInGameState:
                std::snprintf(line, sizeof(line), "UPLAY tid=%u SetInGameState(%llu)%s", r.tid,
                              static_cast<unsigned long long>(r.a), r.a ? "  -> ACP active (Hacked Animus, in game)" : "");
                break;
            case Rec::Enumerate:
                std::snprintf(line, sizeof(line), "UPLAY tid=%u Enumerate(handle=%llu) ret=%llu", r.tid,
                              static_cast<unsigned long long>(r.a), static_cast<unsigned long long>(r.b));
                break;
            case Rec::Injected:
                std::snprintf(line, sizeof(line), "UPLAY tid=%u event 80000 injected: %llu value(s), first %.*s", r.tid,
                              static_cast<unsigned long long>(r.a), static_cast<int>(ParamTable()[r.b].key.size()),
                              ParamTable()[r.b].key.data());
                break;
            case Rec::EnumEmulated:
                std::snprintf(line, sizeof(line), "UPLAY tid=%u Enumerate emulated (game reports its save-game values)", r.tid);
                break;
            case Rec::ApplyCaptured:
                std::snprintf(line, sizeof(line), "UPLAY tid=%u Apply: %llu values received from the game", r.tid,
                              static_cast<unsigned long long>(r.a));
                break;
            default: std::snprintf(line, sizeof(line), "UPLAY ?"); break;
        }
        platform::log::Write(line);
        ++g_tail;
    }
    const std::uint64_t now = GetTickCount64();
    if (g_hooked && now >= nextStats) {
        nextStats = now + 60000;
        char line[160];
        std::snprintf(line, sizeof(line), "UPLAY stats: GetNextEvent %llu calls, %llu events, %llu injected, state %d",
                      static_cast<unsigned long long>(g_calls.load()), static_cast<unsigned long long>(g_hits.load()),
                      static_cast<unsigned long long>(g_injected.load()), g_gameState.load());
        platform::log::Write(line);
    }
}

}  // namespace acp::hook
