// Subclassing of the game window: hotkeys (configurable, default Shift+F2; End = unload) and input blocking while the overlay is open.
#include <atomic>
#include <cstdint>
#include <mutex>

#include "hooks.h"
#include "imgui.h"
#include "imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace acp::hook {
namespace {

std::atomic<WNDPROC> g_orig{nullptr};
std::atomic<HWND> g_hwnd{nullptr};
std::atomic<bool> g_unicode{false};

LRESULT CallOrig(HWND h, UINT msg, WPARAM w, LPARAM l) {
    WNDPROC orig = g_orig.load(std::memory_order_acquire);
    if (!orig) return g_unicode ? DefWindowProcW(h, msg, w, l) : DefWindowProcA(h, msg, w, l);
    return g_unicode ? CallWindowProcW(orig, h, msg, w, l) : CallWindowProcA(orig, h, msg, w, l);
}

LRESULT CallDef(HWND h, UINT msg, WPARAM w, LPARAM l) {
    return g_unicode ? DefWindowProcW(h, msg, w, l) : DefWindowProcA(h, msg, w, l);
}

std::uint32_t CurrentMods() {
    std::uint32_t m = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000) m |= 1u << 16;
    if (GetKeyState(VK_CONTROL) & 0x8000) m |= 1u << 17;
    if (GetKeyState(VK_MENU) & 0x8000) m |= 1u << 18;
    return m;
}

// Window thread: last swallowed hotkey (its release is swallowed as well).
WPARAM g_swallowedVk = 0;

bool IsKeyboardOrMouse(UINT msg) {
    return (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST);
}

// Releases always go to the game so that no key gets stuck there.
bool IsRelease(UINT msg) {
    switch (msg) {
        case WM_KEYUP: case WM_SYSKEYUP: case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: case WM_XBUTTONUP:
            return true;
        default:
            return false;
    }
}

// true if a raw input packet reports only a release (may go to the game).
bool RawInputIsReleaseOnly(LPARAM l) {
    alignas(8) BYTE buf[sizeof(RAWINPUT) + 64];
    UINT size = sizeof(buf);
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(l), RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1))
        return false;
    const auto* ri = reinterpret_cast<const RAWINPUT*>(buf);
    if (ri->header.dwType == RIM_TYPEKEYBOARD) return (ri->data.keyboard.Flags & RI_KEY_BREAK) != 0;
    if (ri->header.dwType == RIM_TYPEMOUSE) {
        const USHORT f = ri->data.mouse.usButtonFlags;
        constexpr USHORT downs = RI_MOUSE_BUTTON_1_DOWN | RI_MOUSE_BUTTON_2_DOWN | RI_MOUSE_BUTTON_3_DOWN |
                                 RI_MOUSE_BUTTON_4_DOWN | RI_MOUSE_BUTTON_5_DOWN | RI_MOUSE_WHEEL | RI_MOUSE_HWHEEL;
        constexpr USHORT ups = RI_MOUSE_BUTTON_1_UP | RI_MOUSE_BUTTON_2_UP | RI_MOUSE_BUTTON_3_UP | RI_MOUSE_BUTTON_4_UP |
                               RI_MOUSE_BUTTON_5_UP;
        return (f & ups) && !(f & downs) && ri->data.mouse.lLastX == 0 && ri->data.mouse.lLastY == 0;
    }
    return false;
}

LRESULT CALLBACK HkWndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    InFlight guard(G().inFlightWndProc);
    if (msg == WM_NCDESTROY) {
        RestoreWndProc();
        return CallOrig(h, msg, w, l);
    }
    if (G().phase.load(std::memory_order_acquire) != Phase::Running) return CallOrig(h, msg, w, l);

    // Hotkeys: edge-triggered (bit 30 = autorepeat); the press and its matching release are swallowed.
    // Open/close: configurable combination (default Shift+F2). Unload: End.
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && !G().keyCapture.load(std::memory_order_acquire)) {
        const std::uint32_t hk = G().toggleKey.load(std::memory_order_relaxed);
        const bool isToggle = hk != 0 && w == (hk & 0xFFFF) && CurrentMods() == (hk & 0x70000);
        const bool isUnload = w == VK_END && CurrentMods() == 0 && hk != VK_END;
        const std::uint32_t tk = G().tpKey.load(std::memory_order_relaxed);
        const bool isTp = !isToggle && tk != 0 && w == (tk & 0xFFFF) && CurrentMods() == (tk & 0x70000);
        if (isToggle || isUnload || isTp) {
            if (!(l & (1 << 30))) {
                if (isToggle) G().keyToggles.fetch_add(1, std::memory_order_acq_rel);
                else if (isTp) G().keyTeleports.fetch_add(1, std::memory_order_acq_rel);
                else RequestUnload();
            }
            g_swallowedVk = w;
            return 0;
        }
    }
    if ((msg == WM_KEYUP || msg == WM_SYSKEYUP) && g_swallowedVk != 0 && w == g_swallowedVk) {
        g_swallowedVk = 0;
        return 0;
    }

    if (!G().overlayOpen.load(std::memory_order_acquire)) return CallOrig(h, msg, w, l);

    // Overlay open: input goes to ImGui.
    {
        std::lock_guard lock(G().ctxMutex);
        if (G().imguiReady.load() && ImGui::GetCurrentContext()) ImGui_ImplWin32_WndProcHandler(h, msg, w, l);
    }

    if (msg == WM_INPUT) {
        if (RawInputIsReleaseOnly(l)) return CallOrig(h, msg, w, l);
        return CallDef(h, msg, w, l);  // mandatory cleanup, the game does not see the packet
    }
    if (IsKeyboardOrMouse(msg)) {
        if (IsRelease(msg)) return CallOrig(h, msg, w, l);
        // Alt combinations (Alt+Tab, Alt+F4, Alt+Enter) stay with the game.
        if ((msg == WM_SYSKEYDOWN || msg == WM_SYSCHAR) && (l & (1 << 29))) return CallOrig(h, msg, w, l);
        return 0;
    }
    if (msg == WM_SETCURSOR && LOWORD(l) == HTCLIENT) {
        SetCursor(nullptr);  // ImGui draws the cursor itself
        return TRUE;
    }
    return CallOrig(h, msg, w, l);
}

}  // namespace

bool InstallWndProc(HWND hwnd) {
    const bool unicode = IsWindowUnicode(hwnd) != FALSE;
    g_unicode = unicode;
    g_hwnd = hwnd;
    const LONG_PTR cur = unicode ? GetWindowLongPtrW(hwnd, GWLP_WNDPROC) : GetWindowLongPtrA(hwnd, GWLP_WNDPROC);
    g_orig.store(reinterpret_cast<WNDPROC>(cur), std::memory_order_release);
    const LONG_PTR hook = reinterpret_cast<LONG_PTR>(&HkWndProc);
    SetLastError(0);
    const LONG_PTR prev = unicode ? SetWindowLongPtrW(hwnd, GWLP_WNDPROC, hook) : SetWindowLongPtrA(hwnd, GWLP_WNDPROC, hook);
    if (prev == 0 && GetLastError() != 0) {
        g_orig = nullptr;
        return false;
    }
    if (prev != cur) g_orig.store(reinterpret_cast<WNDPROC>(prev), std::memory_order_release);
    return true;
}

bool RestoreWndProc() {
    const HWND hwnd = g_hwnd.exchange(nullptr);
    if (!hwnd) return true;
    const bool unicode = g_unicode.load();
    const LONG_PTR cur = unicode ? GetWindowLongPtrW(hwnd, GWLP_WNDPROC) : GetWindowLongPtrA(hwnd, GWLP_WNDPROC);
    if (cur != reinterpret_cast<LONG_PTR>(&HkWndProc)) {
        g_hwnd = hwnd;  // foreign subclass on top of us: leave ours in place as a pass-through
        return false;
    }
    const LONG_PTR orig = reinterpret_cast<LONG_PTR>(g_orig.load());
    if (unicode) SetWindowLongPtrW(hwnd, GWLP_WNDPROC, orig);
    else SetWindowLongPtrA(hwnd, GWLP_WNDPROC, orig);
    return true;
}

}  // namespace acp::hook
