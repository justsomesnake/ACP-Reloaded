#pragma once

// Character portraits (playable ACP characters 0..20) and the Origins theme backdrop, embedded as RCDATA resources
// (assets/portraits, see portraits.rc). Images: our own cut-out screenshots of the characters; see THIRD_PARTY.md.
//
// Flow: Decode() on a separate thread with COM (WIC to RGBA), then Get() creates textures on demand on the
// render thread. Call Release() before the D3D device is torn down (render thread).

#include <d3d11.h>

#include "imgui.h"

namespace acp::ui::portraits {

inline constexpr int kCount = 21;
inline constexpr int kBackdrop = -1;  // backdrop behind the characters (Origins)

// Decodes all images embedded in the module. COM must be initialised on the calling thread.
// Returns the number of images decoded successfully. Safe to call more than once.
int Decode(HMODULE module);
// Render thread: texture for character n (0..20) or kBackdrop; 0 if not available (yet).
ImTextureID Get(ID3D11Device* device, int n);
// Render thread: release all textures (decoded data is kept for a new device).
void Release();

}  // namespace acp::ui::portraits
