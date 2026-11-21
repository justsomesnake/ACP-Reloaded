// ACPReloaded.dll - DllMain only starts the lifecycle thread (the loader lock is held).
#include "hooks.h"

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        acp::hook::G().module = module;
        if (HANDLE t = CreateThread(nullptr, 0, &acp::hook::LifecycleThread, nullptr, 0, nullptr)) CloseHandle(t);
    }
    // DLL_PROCESS_DETACH: deliberately nothing (at process exit other threads are already gone).
    return TRUE;
}
