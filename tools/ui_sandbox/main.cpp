// ACPReloaded_Editor.exe - standalone ACP.ini editor with the same UI as the overlay.
// Options:   --ini <path>     edit a different ACP.ini (tests)
//            --data <folder>  different user data folder (presets/backups)
//            --frames <n>     exit after n frames (self-test)
//            --load-overlay   load ACPReloaded.dll into this process (hook self-test)
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <xinput.h>
#include <shellapi.h>

#include <cstdlib>
#include <string>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "pad_feed.h"
#include "panel.h"
#include "platform.h"
#include "sync_service.h"
#include "portraits.h"
#include "theme.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

ID3D11Device* g_device = nullptr;

ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swapChain = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
UINT g_resizeW = 0, g_resizeH = 0;

void CreateRtv() {
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back)))) {
        g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

void ReleaseRtv() {
    if (g_rtv) {
        g_rtv->Release();
        g_rtv = nullptr;
    }
}

bool CreateDevice(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                               D3D11_SDK_VERSION, &sd, &g_swapChain, &g_device, &got, &g_context);
    if (hr == DXGI_ERROR_UNSUPPORTED)
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd,
                                           &g_swapChain, &g_device, &got, &g_context);
    if (FAILED(hr)) return false;
    CreateRtv();
    return true;
}

void DestroyDevice() {
    ReleaseRtv();
    if (g_swapChain) g_swapChain->Release();
    if (g_context) g_context->Release();
    if (g_device) g_device->Release();
    g_swapChain = nullptr;
    g_context = nullptr;
    g_device = nullptr;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) return TRUE;
    switch (msg) {
        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED) {
                g_resizeW = LOWORD(lParam);
                g_resizeH = HIWORD(lParam);
            }
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Self-test: save our own frame (before Present) as a 32-bit BMP - not a screen capture.
void SaveBackbufferBmp(const std::wstring& path) {
    ID3D11Texture2D* back = nullptr;
    if (FAILED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back)))) return;
    D3D11_TEXTURE2D_DESC td{};
    back->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    if (SUCCEEDED(g_device->CreateTexture2D(&td, nullptr, &staging))) {
        g_context->CopyResource(staging, back);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(g_context->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
            const UINT w = td.Width, h = td.Height;
            std::string data(static_cast<std::size_t>(w) * h * 4, char(0));
            for (UINT y = 0; y < h; ++y) {
                const auto* src = static_cast<const unsigned char*>(m.pData) + static_cast<std::size_t>(y) * m.RowPitch;
                auto* dst = reinterpret_cast<unsigned char*>(data.data()) + static_cast<std::size_t>(h - 1 - y) * w * 4;
                for (UINT x = 0; x < w; ++x) {  // RGBA -> BGRA
                    dst[x * 4 + 0] = src[x * 4 + 2];
                    dst[x * 4 + 1] = src[x * 4 + 1];
                    dst[x * 4 + 2] = src[x * 4 + 0];
                    dst[x * 4 + 3] = 255;
                }
            }
            g_context->Unmap(staging, 0);
            BITMAPFILEHEADER fh{};
            BITMAPINFOHEADER ih{};
            ih.biSize = sizeof(ih);
            ih.biWidth = static_cast<LONG>(w);
            ih.biHeight = static_cast<LONG>(h);
            ih.biPlanes = 1;
            ih.biBitCount = 32;
            ih.biCompression = BI_RGB;
            fh.bfType = 0x4D42;
            fh.bfOffBits = sizeof(fh) + sizeof(ih);
            fh.bfSize = fh.bfOffBits + static_cast<DWORD>(data.size());
            std::string file(reinterpret_cast<const char*>(&fh), sizeof(fh));
            file.append(reinterpret_cast<const char*>(&ih), sizeof(ih));
            file += data;
            acp::platform::WriteFileAtomic(path, file, acp::platform::WriteStrategy::AtomicReplace, true);
        }
        staging->Release();
    }
    back->Release();
}

std::wstring ArgValue(int argc, wchar_t** argv, const wchar_t* name) {
    for (int i = 1; i + 1 < argc; ++i)
        if (wcscmp(argv[i], name) == 0) return argv[i + 1];
    return {};
}
bool HasArg(int argc, wchar_t** argv, const wchar_t* name) {
    for (int i = 1; i < argc; ++i)
        if (wcscmp(argv[i], name) == 0) return true;
    return false;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const std::wstring iniArg = ArgValue(argc, argv, L"--ini");
    const std::wstring dataArg = ArgValue(argc, argv, L"--data");
    const std::wstring framesArg = ArgValue(argc, argv, L"--frames");
    const long maxFrames = framesArg.empty() ? -1 : std::wcstol(framesArg.c_str(), nullptr, 10);
    const bool loadOverlay = HasArg(argc, argv, L"--load-overlay");
    const std::wstring shotArg = ArgValue(argc, argv, L"--screenshot");

    ImGui_ImplWin32_EnableDpiAwareness();
    const float dpi = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));

    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, WndProc, 0, 0, hInst, nullptr, LoadCursorW(nullptr, IDC_ARROW), nullptr, nullptr,
                   L"ACPReloadedEditor", nullptr};
    RegisterClassExW(&wc);
    // --size WxH: window size in pixels (tests the layout in small/large windows)
    int winW = static_cast<int>(1100 * dpi), winH = static_cast<int>(760 * dpi);
    if (const std::wstring sz = ArgValue(argc, argv, L"--size"); !sz.empty()) swscanf_s(sz.c_str(), L"%dx%d", &winW, &winH);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"ACP Reloaded - Animus Control Panel Editor", WS_OVERLAPPEDWINDOW, 100,
                              100, winW, winH, nullptr, nullptr, hInst,
                              nullptr);
    if (!CreateDevice(hwnd)) {
        MessageBoxW(nullptr, L"Direct3D 11 could not be initialized.", L"ACP Reloaded", MB_ICONERROR);
        return 1;
    }
    ShowWindow(hwnd, maxFrames > 0 ? SW_SHOWNOACTIVATE : SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    // Logging + Sync
    acp::SyncConfig cfg;
    if (!iniArg.empty()) cfg.candidates = {std::filesystem::path(iniArg)};
    if (!dataArg.empty()) cfg.userDataDir = dataArg;
    else if (auto d = acp::platform::UserDataDir()) cfg.userDataDir = *d;
    cfg.bundledPresetsDir = acp::platform::ThisModuleDir() / L"ACP_Presets";
    acp::platform::log::SetFile(cfg.userDataDir / L"ACPReloaded_Editor.log");
    acp::platform::log::Write("Editor started");
    acp::SyncService sync;
    sync.Start(cfg);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    wchar_t winDir[MAX_PATH]{};
    GetWindowsDirectoryW(winDir, MAX_PATH);
    static std::string fontData;
    if (auto f = acp::platform::ReadFileBytes(std::filesystem::path(winDir) / L"Fonts" / L"segoeui.ttf")) fontData = *f;
    acp::ui::LoadUiFont(&fontData, 17.0f);
    ImGui::GetStyle().FontScaleDpi = dpi;
    acp::ui::ApplyTheme(acp::ui::ThemeId::UplayClassic, dpi);
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    if (loadOverlay) {
        // --overlay-file <name>: different file name in the editor folder (e.g. ACPReloaded.asi for the ASI loader test)
        const std::wstring overlayFile = ArgValue(argc, argv, L"--overlay-file");
        const auto dll = acp::platform::ThisModuleDir() / (overlayFile.empty() ? std::wstring(L"ACPReloaded.dll") : overlayFile);
        SetEnvironmentVariableW(L"ACP_RELOADED_ANY_HOST", L"1");
        if (!LoadLibraryW(dll.c_str())) acp::platform::log::Write("ACPReloaded.dll could not be loaded");
    }

    acp::ui::Panel panel(sync);
    if (const std::wstring page = ArgValue(argc, argv, L"--page"); !page.empty()) panel.SetPage(_wtoi(page.c_str()));
    if (HasArg(argc, argv, L"--pad")) panel.ForcePadLayout();
    if (HasArg(argc, argv, L"--char-grid")) panel.DebugOpenCharacterGrid();
    if (const std::wstring sc = ArgValue(argc, argv, L"--scroll"); !sc.empty()) panel.DebugScroll(static_cast<float>(_wtof(sc.c_str())));
    if (const std::wstring q = ArgValue(argc, argv, L"--search"); !q.empty()) panel.DebugSetSearch(acp::platform::Narrow(q).c_str());
    acp::ui::PadFeeder feeder;
    acp::ui::PanelEnv env;
    env.fillViewport = true;
    env.baseScale = dpi;
    env.liveHint = "Changes are saved to ACP.ini immediately";
    // Character portraits (embedded resources of this program)
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    acp::platform::log::Write("Portraits: " + std::to_string(acp::ui::portraits::Decode(GetModuleHandleW(nullptr))) + "/22");
    env.portrait = [](int n) { return acp::ui::portraits::Get(g_device, n); };
    if (HasArg(argc, argv, L"--fake-game")) {
        // Test the game-function rows without the game: sample values, actions have no effect
        env.game.supported = true;
        env.game.valid = true;
        env.game.stateText = "active (editor sample)";
        env.game.hours = 14.5f;
        env.game.timeText = "running";
        env.game.acpTimeBlocked = HasArg(argc, argv, L"--time-blocked");  // show the "Normal speed" hint
        if (env.game.acpTimeBlocked) {
            env.game.resultText = "Time set to 07:30.";
            env.game.resultIsTime = true;
        }
        env.game.hasPlayer = true;
        env.game.level = 23;
        env.game.xp = 48250;
        env.game.abilityPoints = 3;
        env.game.weatherValid = true;
        env.game.weather = 0.35f;
        env.game.tpValid = true;
        env.game.tpText = "ready";
        env.game.tpReady = true;
        env.game.tpSource = 1;
        env.game.tpM6A = true;
        env.game.tpModuleSet = true;
        env.game.tpDistance = 1834.0f;
        env.game.tpDistanceXY = 1829.0f;
        env.game.tpMaxDistance = 30000.0f;
        env.game.questAvailable = true;
        env.game.questReady = true;
        env.game.questText = "ready";
        env.game.questDistance = 742.0f;
        env.game.questIcons = 2;
        env.game.questUsable = 1;
        env.game.speedAvailable = true;
        env.teleportRequest = [](int) {};
        env.gameAction = [](acp::ui::GameAction, int, float, float) {};
        env.holdTime = [](bool, float) {};
    }

    long frame = 0;
    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;
        if (g_resizeW != 0 && g_resizeH != 0) {
            ReleaseRtv();
            g_swapChain->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
            g_resizeW = g_resizeH = 0;
            CreateRtv();
        }

        // Controller: first connected pad, only while the window is in the foreground.
        acp::input::PadRaw raw{};
        bool connected = false;
        if (GetForegroundWindow() == hwnd) {
            for (DWORD i = 0; i < XUSER_MAX_COUNT && !connected; ++i) {
                XINPUT_STATE st{};
                if (XInputGetState(i, &st) == ERROR_SUCCESS) {
                    connected = true;
                    raw = {st.Gamepad.wButtons, st.Gamepad.bLeftTrigger, st.Gamepad.bRightTrigger, st.Gamepad.sThumbLX,
                           st.Gamepad.sThumbLY, st.Gamepad.sThumbRX, st.Gamepad.sThumbRY};
                }
            }
        }
        acp::ui::PadActions actions;
        feeder.SetHorizontalCapture(panel.WantsPadHorizontal());
        feeder.Feed(io, connected, raw, actions);

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        panel.Draw(env, actions, nullptr);
        ImGui::Render();

        const float clear[4] = {0.03f, 0.035f, 0.045f, 1.0f};
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        if (!shotArg.empty() && maxFrames > 0 && frame + 1 == maxFrames) SaveBackbufferBmp(shotArg);
        g_swapChain->Present(1, 0);

        if (maxFrames > 0 && ++frame >= maxFrames) done = true;
    }

    panel.FlushActiveEdit();
    acp::ui::portraits::Release();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    sync.Stop(true);
    acp::platform::log::Write("Editor stopped");
    acp::platform::log::Flush();
    DestroyDevice();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, hInst);
    LocalFree(argv);
    return 0;
}
