#pragma once
// ImGui configuration for ACP Reloaded (included via IMGUI_USER_CONFIG).

// IME calls send messages to the window thread - deadlock risk while holding the mutex.
#define IMGUI_DISABLE_WIN32_DEFAULT_IME_FUNCTIONS
// We feed pad data ourselves (acp_input); the Win32 backend does not read XInput.
#define IMGUI_IMPL_WIN32_DISABLE_GAMEPAD
#define IMGUI_DISABLE_OBSOLETE_FUNCTIONS
