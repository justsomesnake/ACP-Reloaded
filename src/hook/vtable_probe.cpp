// Determines the addresses of IDXGISwapChain::Present/ResizeBuffers/Present1 via a
// dummy swap chain (replaces Kiero). Runs on the lifecycle thread.
#include <d3d11.h>
#include <dxgi1_2.h>

#include "hooks.h"

namespace acp::hook {

bool ProbeSwapChainVtable(SwapChainVtbl& out, std::string& error) {
    const HINSTANCE inst = G().module;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = inst;
    wc.lpszClassName = L"ACPReloadedProbeWindow";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 16, 16, nullptr, nullptr, inst, nullptr);
    if (!hwnd) {
        error = "Probe window: " + std::to_string(GetLastError());
        UnregisterClassW(wc.lpszClassName, inst);
        return false;
    }

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 16;
    sd.BufferDesc.Height = 16;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                               &sd, &sc, &dev, nullptr, &ctx);
    if (FAILED(hr)) {
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd,
                                           &sc, &dev, nullptr, &ctx);
    }
    if (SUCCEEDED(hr) && sc) {
        void** vtbl = *reinterpret_cast<void***>(sc);
        out.present = vtbl[8];         // IDXGISwapChain::Present
        out.resizeBuffers = vtbl[13];  // IDXGISwapChain::ResizeBuffers
        IDXGISwapChain1* sc1 = nullptr;
        if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&sc1))) && sc1) {
            out.present1 = (*reinterpret_cast<void***>(sc1))[22];  // IDXGISwapChain1::Present1
            sc1->Release();
        }
    } else {
        error = "D3D11CreateDeviceAndSwapChain: 0x" + std::to_string(static_cast<unsigned long>(hr));
    }
    if (sc) sc->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, inst);
    return out.present && out.resizeBuffers;
}

}  // namespace acp::hook
