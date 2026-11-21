#pragma once

// Shared state and interfaces of the overlay DLL.

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <array>
#include <string>
#include <utility>
#include <vector>

#include "pad_filter.h"
#include "param_table.h"

struct IDXGISwapChain;

namespace acp::hook {

enum class Phase : std::uint8_t { Loading, Running, ShuttingDown, Inert };

struct Globals {
    std::atomic<Phase> phase{Phase::Loading};
    HMODULE module = nullptr;
    HANDLE unloadEvent = nullptr;
    std::atomic<HWND> gameWindow{nullptr};
    std::atomic<bool> overlayOpen{false};
    std::atomic<std::uint32_t> keyToggles{0};  // counted by the window thread, consumed in Present
    std::atomic<std::uint32_t> toggleKey{0x10071};  // VK | Shift<<16 | Ctrl<<17 | Alt<<18 (default Shift+F2)
    std::atomic<std::uint32_t> tpKey{0};            // quick teleport to the marker (0 = off), same format as toggleKey
    std::atomic<std::uint32_t> keyTeleports{0};     // counted by the window thread, consumed in Present
    std::atomic<bool> keyCapture{false};            // panel is capturing a new key: hotkey suspended
    std::atomic<bool> faulted{false};         // exception in Present: overlay permanently off

    std::atomic<int> inFlightPresent{0};
    std::atomic<int> inFlightResize{0};
    std::atomic<int> inFlightWndProc{0};
    std::atomic<int> inFlightXInput{0};
    std::atomic<int> inFlightUplay{0};

    std::mutex renderMutex;           // init, frame, resize, teardown
    std::recursive_mutex ctxMutex;    // every ImGui access (render and window thread)
    std::atomic<bool> imguiReady{false};
    std::atomic<bool> teardownRequested{false};
    std::atomic<bool> teardownDone{false};
};
Globals& G();

// Only sets a flag + event; the lifecycle thread does the work. Safe from any context.
void RequestUnload();

struct InFlight {
    explicit InFlight(std::atomic<int>& c) noexcept : c_(c) { c_.fetch_add(1, std::memory_order_acq_rel); }
    ~InFlight() { c_.fetch_sub(1, std::memory_order_acq_rel); }
    InFlight(const InFlight&) = delete;
    InFlight& operator=(const InFlight&) = delete;
    std::atomic<int>& c_;
};

// --- vtable_probe.cpp ---
struct SwapChainVtbl {
    void* present = nullptr;
    void* resizeBuffers = nullptr;
    void* present1 = nullptr;
};
bool ProbeSwapChainVtable(SwapChainVtbl& out, std::string& error);

// --- dx11_hooks.cpp ---
bool CreateDxgiHooks(const SwapChainVtbl& vt, std::string& error);  // MH_CreateHook + MH_QueueEnableHook

// --- overlay.cpp (render thread) ---
void OnPresent(IDXGISwapChain* sc);  // in the Present detour during phase Running
void TeardownRender();               // release ImGui + D3D objects (render thread or lifecycle)

// --- wndproc_hook.cpp ---
bool InstallWndProc(HWND hwnd);
bool RestoreWndProc();  // false if a foreign subclass sits on top of ours

// --- xinput_hook.cpp ---
bool CreateXInputHook(bool anyHost, std::string& info);  // MH_CreateHook + queue; false = no hook (fallback)
bool XInputHookActive();                                 // the game calls the hook regularly
std::uint32_t ConsumePadToggles();
bool ReadPad(input::PadRaw& raw);  // render thread: true if a pad is connected
void SetPadGate(input::PadGate gate);

// --- uplay_hook.cpp (Uplay options channel, live values in a "Hacked Animus" save) ---
enum class UplayMode : std::uint8_t { Off = 0, Log = 1, Inject = 2 };
bool CreateUplayHooks(UplayMode mode, bool anyHost, std::string& info);  // MH_CreateHook + Queue
void UplayOnValues(const std::array<ParamValue, kParamCount>& values);   // I/O thread: queue changed values
void UplayPushAll(const std::array<ParamValue, kParamCount>& values);    // queue all values
// Lifecycle thread: save-game values reported by the game (empty if nothing new)
std::vector<std::pair<ParamId, ParamValue>> UplayTakeGameValues();
bool UplayHooked();
int UplayGameState();  // -1 unknown, 0 not accepting, 1 ACP active
void UplayDrainLog();  // lifecycle thread: write the capture to the log

// --- lifecycle.cpp ---
DWORD WINAPI LifecycleThread(LPVOID);

// Hook targets for the chain check on unload
void RegisterHookTarget(void* target);

}  // namespace acp::hook
