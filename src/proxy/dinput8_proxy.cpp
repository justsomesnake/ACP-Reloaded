// dinput8.dll proxy: forwards DirectInput8Create to System32\dinput8.dll and loads
// ACPReloaded.dll from its own folder (if missing, the proxy just forwards).
// Chaining for other mods that also ship a dinput8.dll: rename their DLL to dinput8_chain.dll
// and place it alongside - it is loaded before the overlay.
#include <windows.h>

#include <string>

namespace {

HMODULE g_self = nullptr;
HMODULE g_real = nullptr;
INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;

BOOL CALLBACK LoadReal(PINIT_ONCE, PVOID, PVOID*) {
    wchar_t sys[MAX_PATH]{};
    const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return TRUE;
    std::wstring path(sys, n);
    path += L"\\dinput8.dll";
    g_real = LoadLibraryExW(path.c_str(), nullptr, 0);
    return TRUE;
}

FARPROC Real(const char* name) {
    InitOnceExecuteOnce(&g_once, &LoadReal, nullptr, nullptr);
    return g_real ? GetProcAddress(g_real, name) : nullptr;
}

DWORD WINAPI LoaderThread(LPVOID) {
    std::wstring buf(MAX_PATH, L'\0');
    const DWORD n = GetModuleFileNameW(g_self, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= buf.size()) return 0;
    buf.resize(n);
    const auto slash = buf.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return 0;
    const std::wstring dir = buf.substr(0, slash + 1);
    const std::wstring chain = dir + L"dinput8_chain.dll";
    if (GetFileAttributesW(chain.c_str()) != INVALID_FILE_ATTRIBUTES) LoadLibraryW(chain.c_str());
    const std::wstring overlay = dir + L"ACPReloaded.dll";
    if (GetFileAttributesW(overlay.c_str()) != INVALID_FILE_ATTRIBUTES) LoadLibraryW(overlay.c_str());
    return 0;
}

}  // namespace

// Exports (names via dinput8.def). Normal functions with their own prologue: the Steam and Ubisoft
// overlays place their own inline hooks here.
extern "C" HRESULT WINAPI ACP_DirectInput8Create(HINSTANCE inst, DWORD version, REFIID riid, LPVOID* out, void* outer) {
    using Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, void*);
    if (auto fn = reinterpret_cast<Fn>(Real("DirectInput8Create"))) return fn(inst, version, riid, out, outer);
    if (out) *out = nullptr;
    return E_FAIL;
}

extern "C" HRESULT WINAPI ACP_DllCanUnloadNow() {
    using Fn = HRESULT(WINAPI*)();
    if (auto fn = reinterpret_cast<Fn>(Real("DllCanUnloadNow"))) return fn();
    return S_FALSE;
}

extern "C" HRESULT WINAPI ACP_DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* out) {
    using Fn = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
    if (auto fn = reinterpret_cast<Fn>(Real("DllGetClassObject"))) return fn(clsid, riid, out);
    if (out) *out = nullptr;
    return CLASS_E_CLASSNOTAVAILABLE;
}

extern "C" HRESULT WINAPI ACP_DllRegisterServer() {
    using Fn = HRESULT(WINAPI*)();
    if (auto fn = reinterpret_cast<Fn>(Real("DllRegisterServer"))) return fn();
    return E_FAIL;
}

extern "C" HRESULT WINAPI ACP_DllUnregisterServer() {
    using Fn = HRESULT(WINAPI*)();
    if (auto fn = reinterpret_cast<Fn>(Real("DllUnregisterServer"))) return fn();
    return E_FAIL;
}

extern "C" const void* WINAPI ACP_GetdfDIJoystick() {
    using Fn = const void*(WINAPI*)();
    if (auto fn = reinterpret_cast<Fn>(Real("GetdfDIJoystick"))) return fn();
    return nullptr;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = module;
        DisableThreadLibraryCalls(module);
        if (HANDLE t = CreateThread(nullptr, 0, &LoaderThread, nullptr, 0, nullptr)) CloseHandle(t);
    }
    return TRUE;
}
