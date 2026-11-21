// Detours for IDXGISwapChain::Present, Present1 and ResizeBuffers.
#include <dxgi1_2.h>

#include "MinHook.h"
#include "frame_pacer.h"
#include "hooks.h"

namespace acp::hook {
namespace {

using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ResizeBuffers_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

Present_t g_oPresent = nullptr;
Present1_t g_oPresent1 = nullptr;
ResizeBuffers_t g_oResizeBuffers = nullptr;

thread_local int t_presentDepth = 0;

// SEH protection without C++ objects in the same frame.
bool SehOnPresent(IDXGISwapChain* sc) {
    __try {
        OnPresent(sc);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void MaybeDoOverlayWork(IDXGISwapChain* sc, UINT flags) {
    if (flags & (DXGI_PRESENT_TEST | DXGI_PRESENT_DO_NOT_SEQUENCE)) return;
    if (t_presentDepth > 0) return;  // nested call (Present1 -> Present or similar)
    const Phase phase = G().phase.load(std::memory_order_acquire);
    ++t_presentDepth;
    if (phase == Phase::Running && !G().faulted.load(std::memory_order_relaxed)) {
        if (!SehOnPresent(sc)) {
            // After an SEH exception locks may still be held: overlay permanently off, lock nothing anymore.
            G().faulted = true;
            G().overlayOpen = false;
        }
    } else if (phase == Phase::ShuttingDown && G().teardownRequested.load() && !G().teardownDone.load()) {
        TeardownRender();
    }
    --t_presentDepth;
}

// Frame rate limiter: only on the outermost, real Present (not on test/nested calls).
void MaybePace(bool outer) {
    if (!outer || G().phase.load(std::memory_order_acquire) != Phase::Running || G().faulted.load(std::memory_order_relaxed)) return;
    const HWND game = G().gameWindow.load(std::memory_order_relaxed);
    pacing::BeforePresent(game == nullptr || GetForegroundWindow() == game);
}

bool IsOuter(UINT flags) { return t_presentDepth == 0 && !(flags & (DXGI_PRESENT_TEST | DXGI_PRESENT_DO_NOT_SEQUENCE)); }

HRESULT STDMETHODCALLTYPE HkPresent(IDXGISwapChain* sc, UINT syncInterval, UINT flags) {
    InFlight guard(G().inFlightPresent);
    const bool outer = IsOuter(flags);
    MaybeDoOverlayWork(sc, flags);
    MaybePace(outer);
    return g_oPresent(sc, syncInterval, flags);
}

HRESULT STDMETHODCALLTYPE HkPresent1(IDXGISwapChain1* sc, UINT syncInterval, UINT flags,
                                      const DXGI_PRESENT_PARAMETERS* params) {
    InFlight guard(G().inFlightPresent);
    const bool outer = IsOuter(flags);
    MaybeDoOverlayWork(sc, flags);
    MaybePace(outer);
    return g_oPresent1(sc, syncInterval, flags, params);
}

HRESULT STDMETHODCALLTYPE HkResizeBuffers(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
    InFlight guard(G().inFlightResize);
    if (!G().faulted.load(std::memory_order_relaxed)) {
        // A running overlay frame briefly holds the back buffer; afterwards no reference of ours remains.
        std::lock_guard lock(G().renderMutex);
    }
    return g_oResizeBuffers(sc, count, w, h, fmt, flags);
}

bool Create(void* target, void* detour, void** original, const char* name, std::string& error) {
    const MH_STATUS st = MH_CreateHook(target, detour, original);
    if (st != MH_OK) {
        error += std::string(name) + ": " + MH_StatusToString(st) + " ";
        return false;
    }
    MH_QueueEnableHook(target);
    RegisterHookTarget(target);
    return true;
}

}  // namespace

bool CreateDxgiHooks(const SwapChainVtbl& vt, std::string& error) {
    bool ok = Create(vt.present, reinterpret_cast<void*>(&HkPresent), reinterpret_cast<void**>(&g_oPresent), "Present", error);
    ok = Create(vt.resizeBuffers, reinterpret_cast<void*>(&HkResizeBuffers), reinterpret_cast<void**>(&g_oResizeBuffers),
                "ResizeBuffers", error) && ok;
    if (vt.present1 && vt.present1 != vt.present) {
        Create(vt.present1, reinterpret_cast<void*>(&HkPresent1), reinterpret_cast<void**>(&g_oPresent1), "Present1", error);
    }
    return ok;
}

}  // namespace acp::hook
