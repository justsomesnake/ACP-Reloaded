// Render-thread side of the overlay: ImGui init in the first valid Present, toggle, frame, teardown.
#include "overlay.h"

#include <d3d11.h>
#include <dxgi.h>
#include <xinput.h>

#include <algorithm>
#include <atomic>

#include "hooks.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "platform.h"
#include "portraits.h"
#include "frame_pacer.h"
#include "game_bridge.h"
#include "postfx.h"
#include "theme.h"

namespace acp::hook {
namespace {

std::atomic<OverlayRuntime*> g_runtime{nullptr};

// Used only under renderMutex.
struct Backend {
    IDXGISwapChain* swapChain = nullptr;  // no reference held, for comparison only
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    HWND hwnd = nullptr;
    bool wndProcInstalled = false;
} g_backend;
std::uint64_t g_toastUntil = 0;  // renderMutex
struct QuickHold {  // hold trigger of the quick teleport (renderMutex)
    std::uint64_t since = 0;    // held since (0 = not held)
    bool startedPaused = false; // the game was paused when pressed (map open)
    bool fired = false;         // already fired during this hold
};
QuickHold g_padHold, g_mouseHold;
int g_lastGameState = 0;         // renderMutex

DXGI_FORMAT RtvFormat(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default: return f;
    }
}

void ShutdownImGuiLocked() {
    ReleasePostFx();
    ui::portraits::Release();
    if (!G().imguiReady.load()) return;
    std::lock_guard ctx(G().ctxMutex);
    G().imguiReady = false;
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (g_backend.context) g_backend.context->Release();
    if (g_backend.device) g_backend.device->Release();
    g_backend.context = nullptr;
    g_backend.device = nullptr;
    g_backend.swapChain = nullptr;
}

bool InitLocked(IDXGISwapChain* sc, OverlayRuntime& rt) {
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(sc->GetDesc(&desc))) return false;
    const HWND game = G().gameWindow.load();
    if (!desc.OutputWindow || desc.OutputWindow != game) return false;  // foreign swap chain (e.g. another overlay)

    ID3D11Device* dev = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev))) || !dev) return false;
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);

    // Scaling: window DPI or image height (couch distance), whichever is larger.
    const float dpi = ImGui_ImplWin32_GetDpiScaleForHwnd(desc.OutputWindow);
    const float byHeight = static_cast<float>(desc.BufferDesc.Height) / 1080.0f;
    const float scale = std::clamp(std::max(dpi, byHeight), 1.0f, 3.0f);

    {
        std::lock_guard lock(G().ctxMutex);
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;  // otherwise imgui.ini would end up in the game folder
        io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        ui::LoadUiFont(&rt.fontData, 17.0f);
        ImGui::GetStyle().FontScaleDpi = scale;
        ui::ApplyTheme(ui::ThemeId::UplayClassic, scale);  // the panel applies the chosen theme afterwards
        ImGui_ImplWin32_Init(desc.OutputWindow);
        ImGui_ImplDX11_Init(dev, ctx);
    }
    g_backend.swapChain = sc;
    g_backend.device = dev;
    g_backend.context = ctx;
    g_backend.hwnd = desc.OutputWindow;
    rt.env.baseScale = scale;
    rt.env.customFilterAvailable = true;
    rt.env.portrait = [](int n) { return ui::portraits::Get(g_backend.device, n); };
    rt.env.gameAction = [](ui::GameAction a, int value, float hours, float xpMultiplierPct) {
        game::Request(static_cast<game::Action>(static_cast<int>(a)), value, hours, xpMultiplierPct);
    };
    rt.env.holdTime = [](bool on, float hours) { game::SetHold(on, hours); };
    rt.env.teleportRequest = [](int kind) { game::TeleportRequest(kind == 1 ? game::TpTarget::Quest : game::TpTarget::Marker); };
    G().imguiReady = true;

    if (!g_backend.wndProcInstalled) g_backend.wndProcInstalled = InstallWndProc(desc.OutputWindow);
    platform::log::Write("Overlay initialized (" + std::to_string(desc.BufferDesc.Width) + "x" +
                         std::to_string(desc.BufferDesc.Height) + ", scale " + std::to_string(scale) + ")");
    return true;
}

void SetOpen(bool open, OverlayRuntime& rt) {
    if (G().overlayOpen.load() == open) return;
    std::lock_guard lock(G().ctxMutex);
    ImGuiIO& io = ImGui::GetIO();
    if (open) {
        io.ClearInputKeys();
        io.ClearInputMouse();
        io.MouseDrawCursor = true;
        SetPadGate(input::PadGate::Block);
        rt.panel->OnOpened();
    } else {
        rt.panel->FlushActiveEdit();
        rt.feeder.Release(io);
        io.ClearInputKeys();
        io.ClearInputMouse();
        io.MouseDrawCursor = false;
        SetPadGate(input::PadGate::Pass);
    }
    G().overlayOpen = open;
}

// Draws the current ImGui data onto the back buffer (own RTV, previous state is restored).
void RenderToBackbuffer(IDXGISwapChain* sc) {
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) && back) {
        D3D11_TEXTURE2D_DESC td{};
        back->GetDesc(&td);
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = RtvFormat(td.Format);
        rd.ViewDimension = td.SampleDesc.Count > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
        ID3D11RenderTargetView* rtv = nullptr;
        if (SUCCEEDED(g_backend.device->CreateRenderTargetView(back, &rd, &rtv)) && rtv) {
            ID3D11RenderTargetView* oldRtv = nullptr;
            ID3D11DepthStencilView* oldDsv = nullptr;
            g_backend.context->OMGetRenderTargets(1, &oldRtv, &oldDsv);
            g_backend.context->OMSetRenderTargets(1, &rtv, nullptr);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            g_backend.context->OMSetRenderTargets(1, &oldRtv, oldDsv);
            if (oldRtv) oldRtv->Release();
            if (oldDsv) oldDsv->Release();
            rtv->Release();
        }
        back->Release();  // no back buffer reference beyond the frame
    }
}

// FPS display and frame time graph in the chosen corner (no input, in the panel theme).
void DrawStatsHud(const ui::UiSettings& s) {
    if (!s.showFps && !s.showFrameGraph) return;
    pacing::Stats st;
    pacing::GetStats(st);
    const ui::Palette& p = ui::CurrentPalette();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float em = ImGui::GetFontSize();
    const bool right = s.statsCorner == 1 || s.statsCorner == 3, bottom = s.statsCorner >= 2;
    const float m = em * 0.8f;
    ImGui::SetNextWindowPos(ImVec2(right ? vp->WorkPos.x + vp->WorkSize.x - m : vp->WorkPos.x + m,
                                   bottom ? vp->WorkPos.y + vp->WorkSize.y - m : vp->WorkPos.y + m),
                            ImGuiCond_Always, ImVec2(right ? 1.0f : 0.0f, bottom ? 1.0f : 0.0f));
    ImGui::SetNextWindowBgAlpha(0.72f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em * 0.6f, em * 0.4f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(em * 0.4f, em * 0.15f));
    if (ImGui::Begin("##acp_stats", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
        if (s.showFps) {
            ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.25f);
            ImGui::Text("%.0f", st.fps);
            ImGui::PopFont();
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("FPS");
            ImGui::TextDisabled("%.1f ms   1%% low %.0f", st.avgMs, st.low1Fps);
            if (st.limit > 0) {
                ImGui::SameLine();
                ImGui::TextDisabled("  limit %d", st.limit);
            }
        }
        if (s.showFrameGraph && st.count > 1) {
            const ImVec2 size(em * 12.0f, em * 2.6f);
            const ImVec2 a = ImGui::GetCursorScreenPos();
            ImGui::Dummy(size);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            float maxMs = st.limit > 0 ? 1500.0f / static_cast<float>(st.limit) : 0.0f;
            for (int i = 0; i < st.count; ++i) maxMs = std::max(maxMs, st.history[(st.head + i) % pacing::kHistory]);
            maxMs = std::max(maxMs, std::max(st.avgMs * 2.0f, 8.0f));
            dl->AddRectFilled(a, ImVec2(a.x + size.x, a.y + size.y), ImGui::GetColorU32(ImVec4(0, 0, 0, 0.25f)));
            if (st.limit > 0) {  // target frame time
                const float y = a.y + size.y - size.y * (1000.0f / static_cast<float>(st.limit)) / maxMs;
                dl->AddLine(ImVec2(a.x, y), ImVec2(a.x + size.x, y), ImGui::GetColorU32(ImVec4(p.accent.x, p.accent.y, p.accent.z, 0.35f)));
            }
            const int n = st.count;
            ImVec2 prev;
            for (int i = 0; i < n; ++i) {
                const float v = st.history[(st.head + i) % pacing::kHistory];
                const ImVec2 pt(a.x + size.x * static_cast<float>(i) / static_cast<float>(pacing::kHistory - 1),
                                a.y + size.y - size.y * std::min(v / maxMs, 1.0f));
                if (i > 0) {
                    const bool spike = v > st.avgMs * 2.0f && v > 20.0f;  // noticeable stutter
                    dl->AddLine(prev, pt, ImGui::GetColorU32(spike ? ui::colors::Warn : p.accent), 1.4f);
                }
                prev = pt;
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

void DrawFrameLocked(IDXGISwapChain* sc, OverlayRuntime& rt) {
    bool close = false;
    {
        std::lock_guard lock(G().ctxMutex);
        ImGuiIO& io = ImGui::GetIO();
        input::PadRaw raw{};
        const bool connected = ReadPad(raw);
        ui::PadActions actions;
        rt.feeder.SetHorizontalCapture(rt.panel->WantsPadHorizontal());
        rt.feeder.Feed(io, connected, raw, actions);
        rt.env.padBlockActive = XInputHookActive();
        {
            game::GameInfo gi;
            ui::GameView& gv = rt.env.game;
            gv.supported = true;
            gv.stateText = game::StateText(game::State());
            gv.valid = game::Read(gi);
            gv.hours = gi.hours;
            gv.timeText = game::TimeStopText(gi.stop);
            gv.timeRunning = gi.stop == game::TimeStop::None;
            gv.displayOverride = gi.displayOverride;
            gv.acpTimeBlocked = gi.acpTimeBlocked;
            gv.holding = gi.holding;
            gv.ownClock = gi.ownClock;
            gv.weatherValid = gi.weatherValid;
            gv.weather = gi.weather;
            gv.weatherHeld = gi.weatherHeld;
            gv.weatherScripted = gi.weatherScripted;
            gv.climateOff = gi.climateOff;
            gv.tpValid = gi.teleport.valid && gv.valid;
            gv.tpReady = gi.teleport.block == game::TpBlock::None;
            gv.tpText = game::TpBlockText(gi.teleport.block);
            gv.tpSource = gi.teleport.markerSource;
            gv.tpM6A = gi.teleport.blockFlagM6A;
            gv.tpModuleSet = gi.teleport.moduleSet;
            gv.tpPosMode = gi.teleport.posMode;
            gv.tpPlayerBits = gi.teleport.playerBits;
            gv.tpCombatDiag = gi.teleport.combatDiag;
            gv.tpMaxDistance = rt.panel->Settings().TeleportRange();
            gv.tpDistance = gi.teleport.distance;
            gv.tpDistanceXY = gi.teleport.distanceXY;
            gv.questAvailable = gv.tpValid && gi.teleport.questAvailable;
            gv.questReady = gi.teleport.questBlock == game::TpBlock::None;
            gv.questText = game::TpBlockText(gi.teleport.questBlock);
            gv.questDistance = gi.teleport.questDistance;
            gv.questIcons = gi.teleport.questIcons;
            gv.questUsable = gi.teleport.questUsable;
            gv.speedAvailable = game::SpeedBoostAvailable();
            gv.resultText = game::LastResult(nullptr, &gv.resultIsTime);
            gv.hasPlayer = gi.hasPlayer;
            gv.level = gi.level;
            gv.xp = gi.xp;
            gv.abilityPoints = gi.abilityPoints;
        }
        const int gs = UplayGameState();
        rt.env.liveActive = UplayHooked() && gs == 1;
        rt.env.liveHint = !UplayHooked() ? "Live channel unavailable - see log"
                          : gs == 1      ? "ACP live: changes apply instantly"
                                         : "Load a 'Hacked Animus' save for live changes";

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        rt.panel->Draw(rt.env, actions, &close);
        DrawStatsHud(rt.panel->Settings());
        ImGui::Render();
        RenderToBackbuffer(sc);
    }
    if (close) SetOpen(false, rt);
}

// Short notice while the panel is closed (no input) and/or FPS display.
void DrawClosedLocked(IDXGISwapChain* sc, const char* text, const ui::UiSettings& settings) {
    std::lock_guard lock(G().ctxMutex);
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    DrawStatsHud(settings);
    if (!text) {
        ImGui::Render();
        RenderToBackbuffer(sc);
        return;
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.12f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.88f);
    if (ImGui::Begin("##acp_toast", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
        ImGui::TextColored(ui::CurrentPalette().accent, "ANIMUS CONTROL PANEL");
        ImGui::TextUnformatted(text);
    }
    ImGui::End();
    ImGui::Render();
    RenderToBackbuffer(sc);
}


}  // namespace

OverlayRuntime* Runtime() { return g_runtime.load(std::memory_order_acquire); }
void SetRuntime(OverlayRuntime* rt) { g_runtime.store(rt, std::memory_order_release); }

void OnPresent(IDXGISwapChain* sc) {
    OverlayRuntime* rt = Runtime();
    if (!rt) return;

    std::unique_lock render(G().renderMutex, std::try_to_lock);
    if (!render.owns_lock()) return;

    if (!G().imguiReady.load()) {
        if (!InitLocked(sc, *rt)) return;
    } else if (sc != g_backend.swapChain) {
        DXGI_SWAP_CHAIN_DESC desc{};
        if (FAILED(sc->GetDesc(&desc)) || desc.OutputWindow != G().gameWindow.load()) return;
        // New swap chain for the game window (mode/device change): rebuild the backend.
        const bool wasOpen = G().overlayOpen.load();
        G().overlayOpen = false;
        ShutdownImGuiLocked();
        if (!InitLocked(sc, *rt)) return;
        if (wasOpen) SetOpen(true, *rt);
        platform::log::Write("Backend rebuilt for new swap chain");
    }

    // Toggle: keyboard (window thread) and controller combo (XInput detour).
    // Option "only in Hacked Animus": outside it only a short notice; close when leaving the save.
    // Applies only while the Uplay channel is running (otherwise the panel could never be reached).
    std::uint32_t toggles = G().keyToggles.exchange(0) + ConsumePadToggles();
    const std::uint64_t now = GetTickCount64();
    const bool gated = rt->panel->Settings().onlyInHackedSave && UplayHooked();
    const int gs = UplayGameState();
    if (toggles & 1u) {
        if (!G().overlayOpen.load() && gated && gs != 1) g_toastUntil = now + 3500;
        else SetOpen(!G().overlayOpen.load(), *rt);
    }
    if (G().overlayOpen.load() && gated && g_lastGameState == 1 && gs != 1) SetOpen(false, *rt);
    g_lastGameState = gs;

    // Quick teleport to the map marker: hold the right mouse button or X for about 1 s. Counts only if the game
    // was already paused when pressed (map open) - so it never fires while aiming a bow or during other hold actions.
    // The jump follows as soon as the map is closed. Optionally also a dedicated key (off by default).
    {
        const ui::UiSettings& st = rt->panel->Settings();
        const bool allowed = st.gameFunctions && st.teleport && st.tpQuick && !G().overlayOpen.load();
        const bool paused = game::GamePausedNow();
        bool fire = G().keyTeleports.exchange(0) != 0;
        auto holdTrigger = [&](bool held, QuickHold& h) {
            if (!held) {
                h = QuickHold{};
                return;
            }
            if (h.since == 0) {
                h.since = now;
                h.startedPaused = paused;
            } else if (!h.fired && h.startedPaused && paused && now - h.since >= 800) {
                h.fired = true;
                fire = true;
            }
        };
        input::PadRaw pad;
        holdTrigger(ReadPad(pad) && (pad.buttons & XINPUT_GAMEPAD_X) != 0, g_padHold);  // hold X (user request)
        const HWND gw = G().gameWindow.load();
        holdTrigger(gw && GetForegroundWindow() == gw && (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0, g_mouseHold);
        if (fire && allowed) game::TeleportRequest(game::TpTarget::Marker, true);
    }

    // Take over the current state (also while the panel is closed: for our own image filter).
    bool filter = false;
    int sharpen = 0;
    PostFxParams fx;
    {
        std::lock_guard lock(G().ctxMutex);
        rt->panel->Update();
        game::SetEnabled(rt->panel->Settings().gameFunctions);
        game::SetHideWatermark(rt->panel->Settings().gameFunctions && rt->panel->Settings().hideWatermark);
        game::SetTeleportEnabled(rt->panel->Settings().gameFunctions && rt->panel->Settings().teleport);
        game::SetTeleportMaxDistance(rt->panel->Settings().TeleportRange());
        game::SetTeleportOptions(rt->panel->Settings().tpAllowCombat, rt->panel->Settings().tpAllowMounted);
        game::SetSpeedBoost(rt->panel->Settings().gameFunctions ? rt->panel->Settings().swimBoost / 100.0f : 1.0f,
                            rt->panel->Settings().gameFunctions ? rt->panel->Settings().mountBoost / 100.0f : 1.0f);
        G().toggleKey.store(rt->panel->Settings().toggleKey, std::memory_order_relaxed);
        G().tpKey.store(rt->panel->Settings().gameFunctions && rt->panel->Settings().teleport && rt->panel->Settings().tpQuick
                            ? rt->panel->Settings().tpQuickKey : 0u,
                        std::memory_order_relaxed);
        G().keyCapture.store(G().overlayOpen.load() && rt->panel->CapturingKey(), std::memory_order_release);
        {
            // Own time-lapse clock: real-time minutes per phase = duration x "Length of the day" / 100
            const auto& v = rt->panel->Values();
            const float mod = static_cast<float>(v[static_cast<std::size_t>(ParamId::DayPeriodTimeModifier)].i) / 100.0f;
            const float minutes[4] = {v[static_cast<std::size_t>(ParamId::DayPeriodsMorningDuration)].f * mod,
                                      v[static_cast<std::size_t>(ParamId::DayPeriodsMiddayDuration)].f * mod,
                                      v[static_cast<std::size_t>(ParamId::DayPeriodsAfternoonDuration)].f * mod,
                                      v[static_cast<std::size_t>(ParamId::DayPeriodsNightDuration)].f * mod};
            game::SetOwnClock(rt->panel->Settings().ownClock, minutes);
            game::SetWeather(rt->panel->WeatherHold(), rt->panel->WeatherTarget(), 8.0f);
            pacing::SetLimits(rt->panel->Settings().fpsLimit, rt->panel->Settings().backgroundLimit ? 30 : 0);
        }
        filter = rt->panel->Settings().customFilter;
        sharpen = rt->panel->Settings().sharpen;
        if (filter) {
            const auto& v = rt->panel->Values();
            auto num = [&](ParamId id) {
                const ParamValue& x = v[static_cast<std::size_t>(id)];
                return x.type == ParamType::Float ? x.f : static_cast<float>(x.i);
            };
            fx.grain = num(ParamId::PostProcessCustomFilmGrain);
            fx.vignette = num(ParamId::PostProcessCustomVignette);
            fx.brightness = num(ParamId::PostProcessCustomBrightness);
            fx.temperature = num(ParamId::PostProcessCustomTemperature);
            fx.tint = num(ParamId::PostProcessCustomTint);
            fx.saturation = num(ParamId::PostProcessCustomSaturation);
        }
    }
    if (sharpen > 0) fx.sharpen = static_cast<float>(sharpen) / 100.0f;  // sharpening: independent of our own filter
    if ((filter || sharpen > 0) && !fx.IsNeutral()) ApplyPostFx(sc, g_backend.device, g_backend.context, fx);

    if (!G().overlayOpen.load()) {
        const ui::UiSettings& st = rt->panel->Settings();
        const bool toast = now < g_toastUntil;
        char tpText[160];
        const bool tpToast = game::TeleportStatus(tpText, sizeof(tpText));
        if (toast || tpToast || st.showFps || st.showFrameGraph)
            DrawClosedLocked(sc, toast ? "Available in a 'Hacked Animus' save - load one to open the panel." : tpToast ? tpText : nullptr, st);
        return;  // closed: no further D3D work
    }
    DrawFrameLocked(sc, *rt);
}

void TeardownRender() {
    std::lock_guard render(G().renderMutex);
    if (G().overlayOpen.load()) {
        if (OverlayRuntime* rt = Runtime()) rt->panel->FlushActiveEdit();
        G().overlayOpen = false;
    }
    ShutdownImGuiLocked();
    G().teardownDone = true;
}

}  // namespace acp::hook
