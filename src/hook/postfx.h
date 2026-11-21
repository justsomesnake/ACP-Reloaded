#pragma once

// Own image filter (replaces the PostProcessCustom* values, which have no effect in the game).
// Use only on the render thread under renderMutex.

struct IDXGISwapChain;
struct ID3D11Device;
struct ID3D11DeviceContext;

namespace acp::hook {

struct PostFxParams {
    float grain = 0;        // 0..1
    float vignette = 0;     // 0..1
    float brightness = 1;   // 0.25..4
    float temperature = 0;  // -100..100
    float tint = 0;         // -100..100
    float saturation = 0;   // -100..100
    float sharpen = 0;      // 0..1 (contrast-adaptive sharpening)
    bool IsNeutral() const {
        return grain == 0 && vignette == 0 && brightness == 1 && temperature == 0 && tint == 0 && saturation == 0 && sharpen == 0;
    }
};

// Applies the filter to the current back buffer. false on error (the image then stays unchanged).
bool ApplyPostFx(IDXGISwapChain* sc, ID3D11Device* dev, ID3D11DeviceContext* ctx, const PostFxParams& p);
// Releases all of our D3D objects (teardown/device change).
void ReleasePostFx();

}  // namespace acp::hook
