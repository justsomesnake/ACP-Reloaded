// Lifecycle thread: init (outside the loader lock), install hooks, wait for VK_END, orderly teardown.
#include <array>
#include <cstring>
#include <string>
#include <vector>

#include "MinHook.h"
#include "hooks.h"
#include "game_bridge.h"
#include "overlay.h"
#include "platform.h"
#include "portraits.h"

namespace acp::hook {

Globals& G() {
    static Globals g;
    return g;
}

void RequestUnload() {
    if (HANDLE e = G().unloadEvent) SetEvent(e);
}

namespace {

using platform::log::Write;

struct HookTarget {
    void* target;
    std::array<unsigned char, 16> prologue;
};
std::vector<HookTarget> g_targets;  // lifecycle thread only

std::wstring ExePath() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n < buf.size()) {
            buf.resize(n);
            return buf;
        }
        buf.resize(buf.size() * 2);
    }
}

bool EnvFlag(const wchar_t* name) {
    wchar_t v[8]{};
    return GetEnvironmentVariableW(name, v, 8) > 0 && v[0] == L'1';
}

struct FindCtx {
    DWORD pid;
    bool anyHost;
    HWND found;
    long long bestArea;
};

BOOL CALLBACK EnumProc(HWND h, LPARAM lp) {
    auto& c = *reinterpret_cast<FindCtx*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != c.pid || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    wchar_t cls[128]{};
    GetClassNameW(h, cls, 128);
    if (wcscmp(cls, L"ACPReloadedProbeWindow") == 0) return TRUE;
    if (!c.anyHost) {
        if (wcscmp(cls, L"ScimitarEngineWindowClass") == 0) {
            c.found = h;
            return FALSE;
        }
        return TRUE;
    }
    RECT r{};
    GetClientRect(h, &r);
    const long long area = static_cast<long long>(r.right - r.left) * (r.bottom - r.top);
    if (area > c.bestArea) {
        c.bestArea = area;
        c.found = h;
    }
    return TRUE;
}

HWND FindGameWindow(bool anyHost) {
    FindCtx c{GetCurrentProcessId(), anyHost, nullptr, 0};
    EnumWindows(&EnumProc, reinterpret_cast<LPARAM>(&c));
    return c.found;
}

// Waits in 250 ms steps; false if unloading was requested meanwhile.
template <typename Pred>
bool WaitFor(Pred pred) {
    while (!pred()) {
        if (WaitForSingleObject(G().unloadEvent, 250) == WAIT_OBJECT_0) return false;
    }
    return true;
}

bool WaitInFlightZero(DWORD timeoutMs) {
    const ULONGLONG end = GetTickCount64() + timeoutMs;
    for (;;) {
        if (G().inFlightPresent.load() == 0 && G().inFlightResize.load() == 0 && G().inFlightWndProc.load() == 0 &&
            G().inFlightXInput.load() == 0 && G().inFlightUplay.load() == 0)
            return true;
        if (GetTickCount64() > end) return false;
        Sleep(5);
    }
}

void Shutdown(OverlayRuntime* rt, bool hooksCreated) {
    Write("Unload requested");
    // If a teleport is running (holding/jumping back), wait for it to finish first - at most 30 s, hooks stay active
    for (const ULONGLONG end = GetTickCount64() + 30000; game::TeleportBusy() && GetTickCount64() < end;) Sleep(50);
    // Have any set time scales (swimming/mount) reset in the next game step - at most 3 s
    game::ReleaseSpeedBoost();
    for (const ULONGLONG end = GetTickCount64() + 3000; game::SpeedBoostActive() && GetTickCount64() < end;) Sleep(50);
    G().phase = Phase::ShuttingDown;  // from now on all hooks just pass through
    SetPadGate(input::PadGate::Pass);

    // 1. ImGui/D3D teardown: in the next Present, otherwise here after 2 s.
    bool cleanTeardown = !G().faulted.load();
    if (cleanTeardown && G().imguiReady.load()) {
        G().teardownRequested = true;
        const ULONGLONG end = GetTickCount64() + 2000;
        while (!G().teardownDone.load() && GetTickCount64() < end) Sleep(10);
        if (!G().teardownDone.load()) TeardownRender();
    }

    // 2. Restore WndProc (only if we are on top).
    const bool wndOk = RestoreWndProc();
    if (!wndOk) Write("Foreign subclass on top of our WndProc - ours stays as a pass-through");

    // 3. Disable hooks (only if the prologue bytes are unchanged).
    bool hooksOk = true;
    if (hooksCreated) {
        for (const HookTarget& t : g_targets) {
            if (std::memcmp(t.target, t.prologue.data(), t.prologue.size()) == 0) {
                MH_DisableHook(t.target);
            } else {
                hooksOk = false;
                Write("Foreign hook on top of ours - ours stays active as a pass-through");
            }
        }
    }

    // 4. Drain
    const bool drained = WaitInFlightZero(2000);
    if (!drained) Write("Warning: hook calls still running after 2 s");

    // 5. Stop the I/O thread (finish pending saves).
    if (rt) rt->sync.Stop(true);
    Write("Soft unload complete");
    platform::log::Flush();

#ifdef ACP_DEV_BUILD
    if (hooksCreated && wndOk && hooksOk && drained && cleanTeardown) {
        MH_Uninitialize();
        Sleep(250);
        Write("Hard unload");
        platform::log::Flush();
        G().phase = Phase::Inert;
        FreeLibraryAndExitThread(G().module, 0);
    }
#else
    (void)hooksOk;
#endif
    G().phase = Phase::Inert;  // module stays loaded and inactive
}

}  // namespace

void RegisterHookTarget(void* target) {
    HookTarget t{target, {}};
    g_targets.push_back(t);
}

DWORD WINAPI LifecycleThread(LPVOID) {
    G().unloadEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const bool anyHost = EnvFlag(L"ACP_RELOADED_ANY_HOST");

    // Check the host
    const std::filesystem::path exe = ExePath();
    const std::wstring exeName = exe.filename().wstring();
    if (!anyHost && _wcsicmp(exeName.c_str(), L"ACOrigins.exe") != 0) {
        G().phase = Phase::Inert;
        return 0;
    }
    // Only one instance per process
    const std::wstring mutexName = L"Local\\ACPReloaded_" + std::to_wstring(GetCurrentProcessId());
    HANDLE instance = CreateMutexW(nullptr, FALSE, mutexName.c_str());
    if (!instance || GetLastError() == ERROR_ALREADY_EXISTS) {
        G().phase = Phase::Inert;
        return 0;
    }

    // Log, paths, sync
    auto* rt = new OverlayRuntime();  // lives until process exit (soft unload)
    SyncConfig cfg;
    if (auto d = platform::UserDataDir()) cfg.userDataDir = *d;
    cfg.bundledPresetsDir = platform::ThisModuleDir() / L"ACP_Presets";
    platform::log::SetFile(cfg.userDataDir / L"ACPReloaded.log");
    Write(std::string("==== ACP Reloaded ") + ACP_VERSION_LABEL + " ====");
    Write("Host: " + platform::Narrow(exe.wstring()));
    if (auto st = platform::StatFile(exe)) {
        Write("EXE size " + std::to_string(st->size) + ", timestamp " + std::to_string(st->writeTime));
        if (!anyHost && st->size != 243515408ull) Write("Note: untested build of the EXE");
    }
    wchar_t winDir[MAX_PATH]{};
    GetWindowsDirectoryW(winDir, MAX_PATH);
    if (auto f = platform::ReadFileBytes(std::filesystem::path(winDir) / L"Fonts" / L"segoeui.ttf")) rt->fontData = *f;
    rt->panel = std::make_unique<ui::Panel>(rt->sync);
    rt->env.overlay = true;
    rt->env.liveHint = "Live channel starting...";
    rt->env.pushAllToGame = [](const std::array<ParamValue, kParamCount>& v) { UplayPushAll(v); };
    cfg.onValues = [](const std::array<ParamValue, kParamCount>& v) { UplayOnValues(v); };
    rt->sync.Start(cfg);
    // Decode the character portraits once (this thread with its own COM; textures are created later on the render thread)
    if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
        Write("Portraits: " + std::to_string(ui::portraits::Decode(G().module)) + "/22 decoded");
        CoUninitialize();
    }
    SetRuntime(rt);

    // Wait for the game window and DXGI
    HWND game = nullptr;
    if (!WaitFor([&] { return (game = FindGameWindow(anyHost)) != nullptr; }) ||
        !WaitFor([] { return GetModuleHandleW(L"dxgi.dll") != nullptr; })) {
        Shutdown(rt, false);
        return 0;
    }
    G().gameWindow = game;
    Write("Game window found");

    SwapChainVtbl vt;
    std::string err;
    if (!ProbeSwapChainVtable(vt, err)) {
        Write("VTable probe failed: " + err);
        Shutdown(rt, false);
        return 0;
    }
    if (MH_Initialize() != MH_OK) {
        Write("MH_Initialize failed");
        Shutdown(rt, false);
        return 0;
    }
    if (!CreateDxgiHooks(vt, err)) {
        Write("DXGI hooks failed: " + err);
        Shutdown(rt, true);
        return 0;
    }
    std::string xinfo;
    CreateXInputHook(anyHost, xinfo);
    Write(xinfo);
    // Uplay options channel: ACP_RELOADED_UPLAY=0 (off), 1 (capture only), 2 (default: capture + injection)
    UplayMode uplayMode = UplayMode::Inject;
    wchar_t um[8]{};
    if (GetEnvironmentVariableW(L"ACP_RELOADED_UPLAY", um, 8) > 0) {
        uplayMode = um[0] == L'0' ? UplayMode::Off : um[0] == L'1' ? UplayMode::Log : UplayMode::Inject;
    }
    std::string uinfo;
    CreateUplayHooks(uplayMode, anyHost, uinfo);
    Write(uinfo);
    if (const MH_STATUS st = MH_ApplyQueued(); st != MH_OK) {
        Write(std::string("MH_ApplyQueued: ") + MH_StatusToString(st));
        Shutdown(rt, true);
        return 0;
    }
    for (HookTarget& t : g_targets) std::memcpy(t.prologue.data(), t.target, t.prologue.size());

    G().phase = Phase::Running;
    Write("Hooks active (Present" + std::string(vt.present1 ? ", Present1" : "") +
          ", ResizeBuffers). Shift+F2 (configurable) or View+Menu (hold) opens the panel, End unloads.");
    platform::log::Flush();

    if (EnvFlag(L"ACP_RELOADED_AUTO_OPEN")) G().keyToggles.fetch_add(1);

    // Self-test: ACP_RELOADED_TEST_UNLOAD_MS unloads after the given time, like VK_END.
    DWORD waitMs = INFINITE;
    wchar_t ms[16]{};
    if (GetEnvironmentVariableW(L"ACP_RELOADED_TEST_UNLOAD_MS", ms, 16) > 0) waitMs = static_cast<DWORD>(_wtoi(ms));
    const ULONGLONG unloadAt = waitMs == INFINITE ? ~0ull : GetTickCount64() + waitMs;
    while (WaitForSingleObject(G().unloadEvent, 250) == WAIT_TIMEOUT) {
        UplayDrainLog();
        game::LifecycleTick(UplayGameState() == 1, anyHost);
        const auto gameValues = UplayTakeGameValues();
        if (!gameValues.empty()) {
            for (const auto& [id, v] : gameValues) rt->sync.SubmitEdit(id, v);
            Write("Values taken over from the save game (" + std::to_string(gameValues.size()) + ")");
        }
        if (GetTickCount64() >= unloadAt) break;
    }
    UplayDrainLog();
    Shutdown(rt, true);
    return 0;
}

}  // namespace acp::hook
