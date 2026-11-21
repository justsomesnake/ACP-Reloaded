// Full-screen pass: copy the back buffer, write it back through the filter shader. All of the game's
// affected pipeline state is saved and restored.
#include "postfx.h"

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>

#include <cstring>

namespace acp::hook {
namespace {

constexpr char kShader[] = R"(
cbuffer P : register(b0) {
    float grain; float vignette; float brightness; float temperature;
    float tint; float saturation; float time; float sharpen;
};
Texture2D src : register(t0);
SamplerState smp : register(s0);
struct VSO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSO vs(uint id : SV_VertexID) {
    VSO o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float hash(float2 p) { return frac(sin(dot(p, float2(12.9898, 78.233))) * 43758.5453); }
float3 px(int2 p, int2 maxp) { return src.Load(int3(clamp(p, int2(0, 0), maxp), 0)).rgb; }
float4 ps(VSO i) : SV_Target {
    uint w, h;
    src.GetDimensions(w, h);
    const int2 maxp = int2(w, h) - 1;
    const int2 p = int2(i.pos.xy);
    float3 c = px(p, maxp);
    if (sharpen > 0.0) {
        // Contrast-adaptive sharpening (after AMD FidelityFX CAS): strong on soft edges, weak on hard ones (no halos).
        float3 n = px(p + int2(0, -1), maxp), s = px(p + int2(0, 1), maxp);
        float3 e = px(p + int2(1, 0), maxp), wv = px(p + int2(-1, 0), maxp);
        float3 mn = min(c, min(min(n, s), min(e, wv)));
        float3 mx = max(c, max(max(n, s), max(e, wv)));
        float3 amp = sqrt(saturate(min(mn, 1.0 - mx) / max(mx, 1e-4)));
        float3 wt = amp * (-1.0 / lerp(8.0, 4.5, sharpen));
        c = max((c + (n + s + e + wv) * wt) / (1.0 + 4.0 * wt), 0.0);
    }
    c *= brightness;
    float t = temperature / 100.0;
    c.r *= 1.0 + 0.18 * t;
    c.b *= 1.0 - 0.18 * t;
    float g = tint / 100.0;
    c.g *= 1.0 + 0.12 * g;
    c.rb *= 1.0 - 0.06 * g;
    float l = dot(c, float3(0.2126, 0.7152, 0.0722));
    c = lerp(l.xxx, c, max(0.0, 1.0 + saturation / 100.0));
    float2 d = i.uv - 0.5;
    c *= saturate(1.0 - dot(d, d) * vignette * 2.2);
    float n = hash(i.uv * 1024.0 + time);
    c += (n - 0.5) * grain * 0.18;
    return float4(c, 1.0);
}
)";

struct Resources {
    ID3D11Device* device = nullptr;  // no reference held, for comparison only
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11Buffer* cb = nullptr;
    ID3D11SamplerState* sampler = nullptr;
    ID3D11RasterizerState* rs = nullptr;
    ID3D11BlendState* blend = nullptr;
    ID3D11DepthStencilState* ds = nullptr;
    ID3D11Texture2D* copy = nullptr;
    ID3D11ShaderResourceView* copySrv = nullptr;
    D3D11_TEXTURE2D_DESC copyDesc{};
    bool failed = false;
} g;

template <typename T>
void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

DXGI_FORMAT TypedFormat(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default: return f;
    }
}

bool CreateStatic(ID3D11Device* dev) {
    ID3DBlob* vsb = nullptr;
    ID3DBlob* psb = nullptr;
    ID3DBlob* err = nullptr;
    bool ok = SUCCEEDED(D3DCompile(kShader, sizeof(kShader) - 1, "acp_postfx", nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vsb, &err));
    SafeRelease(err);
    ok = ok && SUCCEEDED(D3DCompile(kShader, sizeof(kShader) - 1, "acp_postfx", nullptr, nullptr, "ps", "ps_4_0", 0, 0, &psb, &err));
    SafeRelease(err);
    ok = ok && SUCCEEDED(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g.vs));
    ok = ok && SUCCEEDED(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g.ps));
    SafeRelease(vsb);
    SafeRelease(psb);

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 32;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ok = ok && SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &g.cb));

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ok = ok && SUCCEEDED(dev->CreateSamplerState(&sd, &g.sampler));

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    ok = ok && SUCCEEDED(dev->CreateRasterizerState(&rd, &g.rs));

    D3D11_BLEND_DESC bld{};
    bld.RenderTarget[0].BlendEnable = FALSE;
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN |
                                                D3D11_COLOR_WRITE_ENABLE_BLUE;
    ok = ok && SUCCEEDED(dev->CreateBlendState(&bld, &g.blend));

    D3D11_DEPTH_STENCIL_DESC dsd{};
    dsd.DepthEnable = FALSE;
    dsd.StencilEnable = FALSE;
    ok = ok && SUCCEEDED(dev->CreateDepthStencilState(&dsd, &g.ds));
    return ok;
}

bool EnsureCopy(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC& bb) {
    if (g.copy && g.copyDesc.Width == bb.Width && g.copyDesc.Height == bb.Height && g.copyDesc.Format == bb.Format) return true;
    SafeRelease(g.copySrv);
    SafeRelease(g.copy);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = bb.Width;
    td.Height = bb.Height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = bb.Format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &g.copy))) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = TypedFormat(bb.Format);
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    if (FAILED(dev->CreateShaderResourceView(g.copy, &sv, &g.copySrv))) {
        SafeRelease(g.copy);
        return false;
    }
    g.copyDesc = td;
    return true;
}

// Backup of the pipeline state that the pass changes.
struct StateBackup {
    UINT viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    ID3D11RasterizerState* rs = nullptr;
    ID3D11BlendState* blend = nullptr;
    FLOAT blendFactor[4]{};
    UINT sampleMask = 0;
    ID3D11DepthStencilState* ds = nullptr;
    UINT stencilRef = 0;
    ID3D11ShaderResourceView* psSrv = nullptr;
    ID3D11SamplerState* psSampler = nullptr;
    ID3D11Buffer* psCb = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11ClassInstance* psInst[256]{};
    UINT psInstCount = 256;
    ID3D11VertexShader* vs = nullptr;
    ID3D11ClassInstance* vsInst[256]{};
    UINT vsInstCount = 256;
    ID3D11GeometryShader* gs = nullptr;
    ID3D11HullShader* hs = nullptr;
    ID3D11DomainShader* dsh = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY topology{};
    ID3D11InputLayout* layout = nullptr;
    ID3D11Buffer* vb = nullptr;
    UINT vbStride = 0, vbOffset = 0;
    ID3D11Buffer* ib = nullptr;
    DXGI_FORMAT ibFormat{};
    UINT ibOffset = 0;
    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView* dsv = nullptr;

    void Save(ID3D11DeviceContext* c) {
        c->RSGetViewports(&viewportCount, viewports);
        c->RSGetState(&rs);
        c->OMGetBlendState(&blend, blendFactor, &sampleMask);
        c->OMGetDepthStencilState(&ds, &stencilRef);
        c->PSGetShaderResources(0, 1, &psSrv);
        c->PSGetSamplers(0, 1, &psSampler);
        c->PSGetConstantBuffers(0, 1, &psCb);
        c->PSGetShader(&ps, psInst, &psInstCount);
        c->VSGetShader(&vs, vsInst, &vsInstCount);
        c->GSGetShader(&gs, nullptr, nullptr);
        c->HSGetShader(&hs, nullptr, nullptr);
        c->DSGetShader(&dsh, nullptr, nullptr);
        c->IAGetPrimitiveTopology(&topology);
        c->IAGetInputLayout(&layout);
        c->IAGetVertexBuffers(0, 1, &vb, &vbStride, &vbOffset);
        c->IAGetIndexBuffer(&ib, &ibFormat, &ibOffset);
        c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
    }

    void Restore(ID3D11DeviceContext* c) {
        c->RSSetViewports(viewportCount, viewports);
        c->RSSetState(rs);
        c->OMSetBlendState(blend, blendFactor, sampleMask);
        c->OMSetDepthStencilState(ds, stencilRef);
        c->PSSetShaderResources(0, 1, &psSrv);
        c->PSSetSamplers(0, 1, &psSampler);
        c->PSSetConstantBuffers(0, 1, &psCb);
        c->PSSetShader(ps, psInst, psInstCount);
        c->VSSetShader(vs, vsInst, vsInstCount);
        c->GSSetShader(gs, nullptr, 0);
        c->HSSetShader(hs, nullptr, 0);
        c->DSSetShader(dsh, nullptr, 0);
        c->IASetPrimitiveTopology(topology);
        c->IASetInputLayout(layout);
        c->IASetVertexBuffers(0, 1, &vb, &vbStride, &vbOffset);
        c->IASetIndexBuffer(ib, ibFormat, ibOffset);
        c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
        SafeRelease(rs);
        SafeRelease(blend);
        SafeRelease(ds);
        SafeRelease(psSrv);
        SafeRelease(psSampler);
        SafeRelease(psCb);
        SafeRelease(ps);
        for (UINT k = 0; k < psInstCount; ++k) SafeRelease(psInst[k]);
        SafeRelease(vs);
        for (UINT k = 0; k < vsInstCount; ++k) SafeRelease(vsInst[k]);
        SafeRelease(gs);
        SafeRelease(hs);
        SafeRelease(dsh);
        SafeRelease(layout);
        SafeRelease(vb);
        SafeRelease(ib);
        for (auto*& r : rtvs) SafeRelease(r);
        SafeRelease(dsv);
    }
};

}  // namespace

bool ApplyPostFx(IDXGISwapChain* sc, ID3D11Device* dev, ID3D11DeviceContext* ctx, const PostFxParams& p) {
    if (g.failed) return false;
    if (g.device != dev) {
        ReleasePostFx();
        g.device = dev;
        if (!CreateStatic(dev)) {
            ReleasePostFx();
            g.failed = true;  // e.g. shader compiler unavailable: filter permanently off
            return false;
        }
    }
    ID3D11Texture2D* back = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) || !back) return false;
    D3D11_TEXTURE2D_DESC bd{};
    back->GetDesc(&bd);
    bool ok = bd.SampleDesc.Count == 1 && EnsureCopy(dev, bd);
    ID3D11RenderTargetView* rtv = nullptr;
    if (ok) {
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = TypedFormat(bd.Format);
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        ok = SUCCEEDED(dev->CreateRenderTargetView(back, &rd, &rtv));
    }
    if (ok) {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(g.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            const float data[8] = {p.grain, p.vignette, p.brightness, p.temperature, p.tint, p.saturation,
                                   static_cast<float>(GetTickCount64() % 100000) * 0.001f, p.sharpen};
            std::memcpy(m.pData, data, sizeof(data));
            ctx->Unmap(g.cb, 0);
        }
        StateBackup st;
        st.Save(ctx);

        ctx->CopyResource(g.copy, back);
        D3D11_VIEWPORT vp{0, 0, static_cast<float>(bd.Width), static_cast<float>(bd.Height), 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->RSSetState(g.rs);
        const float bf[4] = {0, 0, 0, 0};
        ctx->OMSetBlendState(g.blend, bf, 0xffffffff);
        ctx->OMSetDepthStencilState(g.ds, 0);
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetInputLayout(nullptr);
        ID3D11Buffer* noVb = nullptr;
        const UINT zero = 0;
        ctx->IASetVertexBuffers(0, 1, &noVb, &zero, &zero);
        ctx->VSSetShader(g.vs, nullptr, 0);
        ctx->GSSetShader(nullptr, nullptr, 0);
        ctx->HSSetShader(nullptr, nullptr, 0);
        ctx->DSSetShader(nullptr, nullptr, 0);
        ctx->PSSetShader(g.ps, nullptr, 0);
        ctx->PSSetConstantBuffers(0, 1, &g.cb);
        ctx->PSSetShaderResources(0, 1, &g.copySrv);
        ctx->PSSetSamplers(0, 1, &g.sampler);
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView* noSrv = nullptr;
        ctx->PSSetShaderResources(0, 1, &noSrv);

        st.Restore(ctx);
    }
    if (rtv) rtv->Release();
    back->Release();  // no back buffer reference beyond the frame
    return ok;
}

void ReleasePostFx() {
    SafeRelease(g.copySrv);
    SafeRelease(g.copy);
    SafeRelease(g.vs);
    SafeRelease(g.ps);
    SafeRelease(g.cb);
    SafeRelease(g.sampler);
    SafeRelease(g.rs);
    SafeRelease(g.blend);
    SafeRelease(g.ds);
    g.device = nullptr;
    g.copyDesc = {};
}

}  // namespace acp::hook
