// dinput8 proxy test: loads the proxy DLL by path and creates a real IDirectInput8W.
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>

#include <string>

#include "test_main.h"

TEST(ProxyForwardsDirectInput8Create) {
    HMODULE proxy = LoadLibraryW(ACP_PROXY_DLL);
    CHECK(proxy != nullptr);
    if (!proxy) return;
    using Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    auto create = reinterpret_cast<Fn>(GetProcAddress(proxy, "DirectInput8Create"));
    CHECK(create != nullptr);
    CHECK(GetProcAddress(proxy, "GetdfDIJoystick") != nullptr);
    if (create) {
        IDirectInput8W* di = nullptr;
        const HRESULT hr = create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W,
                                  reinterpret_cast<void**>(&di), nullptr);
        CHECK(SUCCEEDED(hr));
        CHECK(di != nullptr);
        if (di) di->Release();
    }
    // The real system DLL must be loaded (forwarding), not a second proxy.
    wchar_t sys[MAX_PATH]{};
    GetSystemDirectoryW(sys, MAX_PATH);
    bool realLoaded = false;
    wchar_t path[MAX_PATH]{};
    HMODULE real = nullptr;
    std::wstring sysDll = std::wstring(sys) + L"\\dinput8.dll";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, sysDll.c_str(), &real) && real != proxy) {
        GetModuleFileNameW(real, path, MAX_PATH);
        realLoaded = _wcsicmp(path, sysDll.c_str()) == 0;
    }
    CHECK(realLoaded);
}

TEST(ProxyLoadsChainedDll) {
    // Put the proxy and a "foreign" DLL (copy of System32\version.dll) as dinput8_chain.dll into a temp folder.
    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring dir = std::wstring(tmp) + L"acp_chain_" + std::to_wstring(GetCurrentProcessId()) + L"\\";
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring proxyCopy = dir + L"dinput8.dll";
    const std::wstring chain = dir + L"dinput8_chain.dll";
    wchar_t sys[MAX_PATH]{};
    GetSystemDirectoryW(sys, MAX_PATH);
    CHECK(CopyFileW(ACP_PROXY_DLL, proxyCopy.c_str(), FALSE));
    CHECK(CopyFileW((std::wstring(sys) + L"\\version.dll").c_str(), chain.c_str(), FALSE));
    HMODULE proxy = LoadLibraryW(proxyCopy.c_str());
    CHECK(proxy != nullptr);
    bool loaded = false;
    for (int i = 0; i < 100 && !loaded; ++i) {  // the proxy loads on its own thread
        HMODULE m = nullptr;
        loaded = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, chain.c_str(), &m) && m != nullptr;
        if (!loaded) Sleep(20);
    }
    CHECK(loaded);
}