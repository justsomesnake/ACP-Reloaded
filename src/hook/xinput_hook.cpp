// Hook on XInputGetState (xinput9_1_0.dll, static import of the game):
// combo detection + masking + blocking while the overlay is open. Detour without locks/allocation/logging.
#include <windows.h>
#include <xinput.h>

#include <array>
#include <atomic>

#include "MinHook.h"
#include "hooks.h"

namespace acp::hook {
namespace {

using XInputGetState_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

XInputGetState_t g_original = nullptr;  // trampoline (when hooked) or the function itself (fallback)
void* g_target = nullptr;
bool g_hooked = false;

std::atomic<input::PadGate> g_gate{input::PadGate::Pass};
std::atomic<std::uint32_t> g_toggles{0};
std::atomic<std::uint64_t> g_lastSampleMs{0};
input::PadFilterConfig g_cfg;  // immutable after init

struct Slot {
    std::atomic_flag busy;  // try-lock for PadFilter::Step only
    input::PadFilter filter;
    std::atomic<std::uint32_t> seq{0};  // seqlock for the raw state
    std::atomic<std::uint64_t> w0{0}, w1{0};
    std::atomic<std::uint64_t> lastMs{0};
    std::atomic<bool> connected{false};
};
std::array<Slot, XUSER_MAX_COUNT> g_slots;

// Fallback polling in Present (render thread only)
DWORD g_fallbackIndex = 0;
std::uint64_t g_fallbackNextScan = 0;

input::PadRaw FromX(const XINPUT_GAMEPAD& g) {
    return {g.wButtons, g.bLeftTrigger, g.bRightTrigger, g.sThumbLX, g.sThumbLY, g.sThumbRX, g.sThumbRY};
}
void ToX(const input::PadRaw& p, XINPUT_GAMEPAD& g) {
    g.wButtons = p.buttons;
    g.bLeftTrigger = p.lt;
    g.bRightTrigger = p.rt;
    g.sThumbLX = p.lx;
    g.sThumbLY = p.ly;
    g.sThumbRX = p.rx;
    g.sThumbRY = p.ry;
}

std::uint64_t Pack0(const input::PadRaw& p) {
    return static_cast<std::uint64_t>(p.buttons) | (static_cast<std::uint64_t>(p.lt) << 16) |
           (static_cast<std::uint64_t>(p.rt) << 24) | (static_cast<std::uint64_t>(static_cast<std::uint16_t>(p.lx)) << 32) |
           (static_cast<std::uint64_t>(static_cast<std::uint16_t>(p.ly)) << 48);
}
std::uint64_t Pack1(const input::PadRaw& p) {
    return static_cast<std::uint64_t>(static_cast<std::uint16_t>(p.rx)) |
           (static_cast<std::uint64_t>(static_cast<std::uint16_t>(p.ry)) << 16);
}
input::PadRaw Unpack(std::uint64_t a, std::uint64_t b) {
    input::PadRaw p;
    p.buttons = static_cast<std::uint16_t>(a);
    p.lt = static_cast<std::uint8_t>(a >> 16);
    p.rt = static_cast<std::uint8_t>(a >> 24);
    p.lx = static_cast<std::int16_t>(static_cast<std::uint16_t>(a >> 32));
    p.ly = static_cast<std::int16_t>(static_cast<std::uint16_t>(a >> 48));
    p.rx = static_cast<std::int16_t>(static_cast<std::uint16_t>(b));
    p.ry = static_cast<std::int16_t>(static_cast<std::uint16_t>(b >> 16));
    return p;
}

DWORD WINAPI HkXInputGetState(DWORD index, XINPUT_STATE* state) {
    InFlight guard(G().inFlightXInput);
    const DWORD r = g_original(index, state);
    if (index >= XUSER_MAX_COUNT || !state || G().phase.load(std::memory_order_acquire) != Phase::Running) return r;

    Slot& s = g_slots[index];
    const bool connected = r == ERROR_SUCCESS;
    const std::uint64_t now = GetTickCount64();
    const input::PadGate gate = g_gate.load(std::memory_order_acquire);

    if (s.busy.test_and_set(std::memory_order_acquire)) {
        // Second concurrent caller: do not wait, only apply the block.
        if (connected && gate == input::PadGate::Block) ToX(input::PadRaw{}, state->Gamepad);
        return r;
    }
    const input::PadRaw raw = connected ? FromX(state->Gamepad) : input::PadRaw{};
    const input::PadFilterOut out = s.filter.Step(now, connected, raw, gate, g_cfg);

    // Publish the raw state for the render thread (seqlock).
    s.seq.fetch_add(1, std::memory_order_acq_rel);
    s.w0.store(Pack0(raw), std::memory_order_relaxed);
    s.w1.store(Pack1(raw), std::memory_order_relaxed);
    s.connected.store(connected, std::memory_order_relaxed);
    s.lastMs.store(now, std::memory_order_relaxed);
    s.seq.fetch_add(1, std::memory_order_release);
    s.busy.clear(std::memory_order_release);

    g_lastSampleMs.store(now, std::memory_order_relaxed);
    if (out.toggleFired) g_toggles.fetch_add(1, std::memory_order_acq_rel);
    if (connected) {
        ToX(out.visible, state->Gamepad);
        state->dwPacketNumber = out.packet;
    }
    return r;
}

}  // namespace

bool CreateXInputHook(bool anyHost, std::string& info) {
    const wchar_t* modules[] = {L"xinput9_1_0.dll", anyHost ? L"xinput1_4.dll" : nullptr};
    for (const wchar_t* name : modules) {
        if (!name) continue;
        HMODULE m = nullptr;
        // Hold our own reference so the module is not unloaded under the hook.
        if (!GetModuleHandleExW(0, name, &m) || !m) continue;
        auto fn = reinterpret_cast<XInputGetState_t>(GetProcAddress(m, "XInputGetState"));
        if (!fn) continue;
        g_target = reinterpret_cast<void*>(fn);
        const MH_STATUS st = MH_CreateHook(g_target, reinterpret_cast<void*>(&HkXInputGetState),
                                           reinterpret_cast<void**>(&g_original));
        if (st != MH_OK) {
            g_original = fn;  // fallback: navigation only, no blocking
            info = "XInput hook failed: " + std::string(MH_StatusToString(st));
            return false;
        }
        MH_QueueEnableHook(g_target);
        RegisterHookTarget(g_target);
        g_hooked = true;
        info = "XInput hook on " + std::string(name == modules[0] ? "xinput9_1_0.dll" : "xinput1_4.dll");
        return true;
    }
    // No XInput loaded by the game: fall back to direct polling.
    if (HMODULE m = LoadLibraryW(L"xinput1_4.dll")) g_original = reinterpret_cast<XInputGetState_t>(GetProcAddress(m, "XInputGetState"));
    info = "No XInput module in the process; falling back to direct polling";
    return false;
}

bool XInputHookActive() {
    if (!g_hooked) return false;
    const std::uint64_t last = g_lastSampleMs.load(std::memory_order_relaxed);
    return last != 0 && GetTickCount64() - last < 2000;
}

std::uint32_t ConsumePadToggles() {
    const std::uint32_t t = g_toggles.exchange(0, std::memory_order_acq_rel);
    // Without an active hook the pad toggle is off (otherwise the combo buttons would be visible to the game).
    return XInputHookActive() ? t : 0;
}

void SetPadGate(input::PadGate gate) { g_gate.store(gate, std::memory_order_release); }

bool ReadPad(input::PadRaw& raw) {
    raw = {};
    if (XInputHookActive()) {
        // Most recently active connected index
        int best = -1;
        std::uint64_t bestMs = 0;
        for (int i = 0; i < XUSER_MAX_COUNT; ++i) {
            const Slot& s = g_slots[i];
            if (s.connected.load(std::memory_order_relaxed) && s.lastMs.load(std::memory_order_relaxed) >= bestMs) {
                bestMs = s.lastMs.load(std::memory_order_relaxed);
                best = i;
            }
        }
        if (best < 0) return false;
        const Slot& s = g_slots[best];
        for (int tries = 0; tries < 8; ++tries) {
            const std::uint32_t a = s.seq.load(std::memory_order_acquire);
            if (a & 1u) continue;
            const std::uint64_t w0 = s.w0.load(std::memory_order_relaxed);
            const std::uint64_t w1 = s.w1.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (s.seq.load(std::memory_order_relaxed) == a) {
                raw = Unpack(w0, w1);
                return true;
            }
        }
        return false;
    }
    // Fallback: direct polling (trampoline or original function, not the detour).
    if (!g_original) return false;
    const std::uint64_t now = GetTickCount64();
    XINPUT_STATE st{};
    if (g_original(g_fallbackIndex, &st) == ERROR_SUCCESS) {
        raw = FromX(st.Gamepad);
        return true;
    }
    if (now >= g_fallbackNextScan) {
        g_fallbackNextScan = now + 2000;
        for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
            if (g_original(i, &st) == ERROR_SUCCESS) {
                g_fallbackIndex = i;
                raw = FromX(st.Gamepad);
                return true;
            }
        }
    }
    return false;
}

}  // namespace acp::hook
