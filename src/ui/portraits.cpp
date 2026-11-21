#include "portraits.h"

#include <windows.h>
#include <wincodec.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

#include "portraits_ids.h"

namespace acp::ui::portraits {
namespace {

struct Image {
    UINT w = 0, h = 0;
    std::vector<std::uint8_t> rgba;
};

constexpr int kSlots = kCount + 1;  // characters + backdrop (last slot)
std::array<Image, kSlots> g_images;
std::atomic<bool> g_decoded{false};
std::array<ID3D11ShaderResourceView*, kSlots> g_srv{};  // render thread only
std::array<bool, kSlots> g_failed{};                     // render thread only: creation failed, don't retry

int Slot(int n) { return n == kBackdrop ? kCount : n; }

bool DecodeOne(IWICImagingFactory* f, HMODULE module, int resId, Image& out) {
    HRSRC res = FindResourceW(module, MAKEINTRESOURCEW(resId), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    if (!res) return false;
    HGLOBAL h = LoadResource(module, res);
    const DWORD size = SizeofResource(module, res);
    const void* data = h ? LockResource(h) : nullptr;
    if (!data || !size) return false;
    bool ok = false;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    if (SUCCEEDED(f->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromMemory(static_cast<BYTE*>(const_cast<void*>(data)), size)) &&
        SUCCEEDED(f->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(f->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(conv->GetSize(&out.w, &out.h)) && out.w && out.h && out.w <= 4096 && out.h <= 4096) {
        out.rgba.resize(static_cast<std::size_t>(out.w) * out.h * 4);
        ok = SUCCEEDED(conv->CopyPixels(nullptr, out.w * 4, static_cast<UINT>(out.rgba.size()), out.rgba.data()));
    }
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    if (stream) stream->Release();
    if (!ok) out = Image{};
    return ok;
}

}  // namespace

int Decode(HMODULE module) {
    if (g_decoded.load(std::memory_order_acquire)) {
        int n = 0;
        for (const Image& i : g_images) n += i.rgba.empty() ? 0 : 1;
        return n;
    }
    IWICImagingFactory* f = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return 0;
    int n = 0;
    for (int i = 0; i < kCount; ++i) n += DecodeOne(f, module, IDR_PORTRAIT_FIRST + i, g_images[i]) ? 1 : 0;
    n += DecodeOne(f, module, IDR_PORTRAIT_BACKDROP, g_images[kCount]) ? 1 : 0;
    f->Release();
    g_decoded.store(true, std::memory_order_release);
    return n;
}

ImTextureID Get(ID3D11Device* device, int n) {
    if (!device || (n != kBackdrop && (n < 0 || n >= kCount)) || !g_decoded.load(std::memory_order_acquire)) return ImTextureID{};
    const int s = Slot(n);
    if (!g_srv[s] && !g_failed[s]) {
        const Image& img = g_images[s];
        g_failed[s] = true;
        if (!img.rgba.empty()) {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = img.w;
            td.Height = img.h;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_IMMUTABLE;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            const D3D11_SUBRESOURCE_DATA sd{img.rgba.data(), img.w * 4, 0};
            ID3D11Texture2D* tex = nullptr;
            if (SUCCEEDED(device->CreateTexture2D(&td, &sd, &tex)) && tex) {
                if (SUCCEEDED(device->CreateShaderResourceView(tex, nullptr, &g_srv[s]))) g_failed[s] = false;
                tex->Release();
            }
        }
    }
    return reinterpret_cast<ImTextureID>(g_srv[s]);
}

void Release() {
    for (auto& v : g_srv) {
        if (v) v->Release();
        v = nullptr;
    }
    g_failed.fill(false);
}

}  // namespace acp::ui::portraits
