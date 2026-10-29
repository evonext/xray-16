#include "stdafx.h"
#pragma hdrstop

#if defined(XR_DLSS)

#include "dx11DLSS.h"
#include "Layers/xrRender/xrRender_console.h"

#include <algorithm>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_d3d.h>

namespace xray::render::RENDER_NAMESPACE
{
namespace dlss
{
namespace
{
// Unique id for this integration (NGX requires some GUID-like project id for custom engines).
constexpr const char* NGX_PROJECT_ID = "6b1f6f0e-2a63-4d8e-9d2b-0f9e5a1c7d42";

bool g_init_tried = false;
bool g_available = false;
NVSDK_NGX_Parameter* g_params = nullptr;
NVSDK_NGX_Handle* g_feature = nullptr;
u32 g_feat_w = 0, g_feat_h = 0, g_feat_ow = 0, g_feat_oh = 0;
int g_feat_preset = -1;
int g_feat_mode = -1;

u32 g_saved_w = 0, g_saved_h = 0;
bool g_render_res = false;

NVSDK_NGX_PerfQuality_Value mode_to_pq(int mode)
{
    switch (mode)
    {
    case 1: return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    case 2: return NVSDK_NGX_PerfQuality_Value_Balanced;
    case 3: return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    case 4: return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
    default: return NVSDK_NGX_PerfQuality_Value_DLAA;
    }
}

constexpr const char* MODE_NAMES[] = { "DLAA", "Quality", "Balanced", "Performance", "Ultra Performance" };

ID3D11Texture2D* g_mv = nullptr; // R16G16_FLOAT motion vectors (pixels, current -> previous)
ID3D11UnorderedAccessView* g_mv_uav = nullptr;
ID3D11Texture2D* g_zout = nullptr; // R32_FLOAT depth copy
ID3D11UnorderedAccessView* g_zout_uav = nullptr;
ID3D11Texture2D* g_out = nullptr; // DLSS output, copied back into scene color
DXGI_FORMAT g_out_fmt = DXGI_FORMAT_UNKNOWN;
u32 g_tex_w = 0, g_tex_h = 0;

ID3D11Texture2D* g_depth_src = nullptr; // not owned
ID3D11ShaderResourceView* g_depth_srv = nullptr;

ID3D11ComputeShader* g_cs = nullptr;
ID3D11Buffer* g_cb = nullptr;
ID3DDeviceContextState* g_state = nullptr;

Fmatrix g_prev_vp;
Fvector g_prev_cam;
bool g_have_prev = false;
u32 g_jitter_index = 0;
bool g_last_reset = true;

struct alignas(16) MVConstants
{
    Fmatrix inv_view;
    Fmatrix cur_vp;
    Fmatrix prev_vp;
    float size_p[4]; // w, h, P11, P22
    float proj[4]; // P31, P32 (as rendered, jittered), P33, P43
};

constexpr const char* MV_CS = R"(
cbuffer C : register(b0)
{
    row_major float4x4 inv_view;
    row_major float4x4 cur_vp;
    row_major float4x4 prev_vp;
    float4 size_p;
    float4 proj;
};
Texture2D<float> t_depth : register(t0);
RWTexture2D<float2> u_mv : register(u0);
RWTexture2D<float> u_depth : register(u1);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)size_p.x || id.y >= (uint)size_p.y)
        return;
    float d = t_depth.Load(int3(id.xy, 0));
    float2 uv = (float2(id.xy) + 0.5) / size_p.xy;
    float2 ndc = uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
    float2 mv = 0;
    // HUD (weapon, hands) is drawn with viewport depth range [0, 0.02] and moves with the camera
    if (d >= 0.0199)
    {
        float4 vp;
        if (d >= 0.99999)
        {
            // sky / far plane: direction only (rotation-only motion)
            vp = float4((ndc.x - proj.x) / size_p.z, (ndc.y - proj.y) / size_p.w, 1.0, 0.0);
        }
        else
        {
            float z = proj.w / (d - proj.z);
            vp = float4((ndc.x - proj.x) * z / size_p.z, (ndc.y - proj.y) * z / size_p.w, z, 1.0);
        }
        float4 w = mul(vp, inv_view);
        float4 c = mul(w, cur_vp);
        float4 p = mul(w, prev_vp);
        float2 uc = c.xy / c.w * float2(0.5, -0.5) + 0.5;
        float2 up = p.xy / p.w * float2(0.5, -0.5) + 0.5;
        mv = (up - uc) * size_p.xy;
    }
    u_mv[id.xy] = mv;
    u_depth[id.xy] = d;
}
)";

template <typename T>
void safe_release(T*& p)
{
    if (p)
    {
        p->Release();
        p = nullptr;
    }
}

float halton(u32 i, u32 b)
{
    float f = 1.f, r = 0.f;
    while (i > 0)
    {
        f /= float(b);
        r += f * float(i % b);
        i /= b;
    }
    return r;
}

std::wstring logs_dir()
{
    string_path path;
    FS.update_path(path, "$logs$", "");
    const int n = MultiByteToWideChar(CP_ACP, 0, path, -1, nullptr, 0);
    std::wstring w(n > 0 ? n : 1, L'\0');
    if (n > 0)
        MultiByteToWideChar(CP_ACP, 0, path, -1, w.data(), n);
    w.resize(wcslen(w.c_str()));
    return w;
}

bool try_init(ID3D11Device* device)
{
    if (g_init_tried)
        return g_available;
    g_init_tried = true;

    const std::wstring logs = logs_dir();
    NVSDK_NGX_Result r = NVSDK_NGX_D3D11_Init_with_ProjectID(
        NGX_PROJECT_ID, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "OpenXRay", logs.c_str(), device);
    if (NVSDK_NGX_FAILED(r))
    {
        Msg("! [DLSS] NGX init failed: 0x%08x (not an NVIDIA RTX GPU or old driver?)", r);
        return false;
    }

    r = NVSDK_NGX_D3D11_GetCapabilityParameters(&g_params);
    if (NVSDK_NGX_FAILED(r) || !g_params)
    {
        Msg("! [DLSS] GetCapabilityParameters failed: 0x%08x", r);
        NVSDK_NGX_D3D11_Shutdown1(device);
        return false;
    }

    int available = 0, needs_update = 0;
    unsigned int min_major = 0, min_minor = 0;
    NVSDK_NGX_Parameter_GetI(g_params, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    NVSDK_NGX_Parameter_GetI(g_params, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_update);
    NVSDK_NGX_Parameter_GetUI(g_params, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &min_major);
    NVSDK_NGX_Parameter_GetUI(g_params, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &min_minor);
    if (!available)
    {
        int init_result = 0;
        NVSDK_NGX_Parameter_GetI(g_params, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &init_result);
        Msg("! [DLSS] not available (init result 0x%08x, needs driver update: %d, min driver %u.%u). "
            "Is nvngx_dlss.dll next to xrEngine.exe?", init_result, needs_update, min_major, min_minor);
        NVSDK_NGX_D3D11_DestroyParameters(g_params);
        g_params = nullptr;
        NVSDK_NGX_D3D11_Shutdown1(device);
        return false;
    }

    // Compute shader for motion vectors
    ID3DBlob* blob = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = D3DCompile(MV_CS, xr_strlen(MV_CS), "dlss_mv", nullptr, nullptr, "main", "cs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors);
    if (FAILED(hr))
    {
        Msg("! [DLSS] motion vector shader failed to compile: %s",
            errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        safe_release(errors);
        return false;
    }
    safe_release(errors);
    hr = device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &g_cs);
    safe_release(blob);
    if (FAILED(hr))
        return false;

    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = sizeof(MVConstants);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&cbd, nullptr, &g_cb)))
        return false;

    // Separate pipeline state: NGX and our compute pass must not disturb the engine's cached D3D state
    ID3D11Device1* device1 = nullptr;
    if (FAILED(device->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&device1))))
    {
        Msg("! [DLSS] ID3D11Device1 not available");
        return false;
    }
    const D3D_FEATURE_LEVEL fl = device->GetFeatureLevel();
    hr = device1->CreateDeviceContextState(0, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), nullptr, &g_state);
    device1->Release();
    if (FAILED(hr))
    {
        Msg("! [DLSS] CreateDeviceContextState failed: 0x%08x", hr);
        return false;
    }

    Msg("* [DLSS] NGX initialized, DLSS available (driver min %u.%u)", min_major, min_minor);
    g_available = true;
    return true;
}

void release_textures()
{
    safe_release(g_mv_uav);
    safe_release(g_mv);
    safe_release(g_zout_uav);
    safe_release(g_zout);
    safe_release(g_out);
    safe_release(g_depth_srv);
    g_depth_src = nullptr;
    g_tex_w = g_tex_h = 0;
    g_out_fmt = DXGI_FORMAT_UNKNOWN;
}

void release_feature()
{
    if (g_feature)
    {
        NVSDK_NGX_D3D11_ReleaseFeature(g_feature);
        g_feature = nullptr;
    }
    g_feat_w = g_feat_h = g_feat_ow = g_feat_oh = 0;
    g_feat_preset = -1;
    g_feat_mode = -1;
}

bool ensure_textures(ID3D11Device* device, const FrameInputs& in)
{
    D3D11_TEXTURE2D_DESC cd;
    in.color->GetDesc(&cd);

    const bool need_out = in.output == nullptr;
    if (g_tex_w != in.width || g_tex_h != in.height || g_out_fmt != cd.Format || need_out != (g_out != nullptr))
    {
        release_textures();

        D3D11_TEXTURE2D_DESC d{};
        d.Width = in.width;
        d.Height = in.height;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

        d.Format = DXGI_FORMAT_R16G16_FLOAT;
        if (FAILED(device->CreateTexture2D(&d, nullptr, &g_mv)) ||
            FAILED(device->CreateUnorderedAccessView(g_mv, nullptr, &g_mv_uav)))
            return false;

        d.Format = DXGI_FORMAT_R32_FLOAT;
        if (FAILED(device->CreateTexture2D(&d, nullptr, &g_zout)) ||
            FAILED(device->CreateUnorderedAccessView(g_zout, nullptr, &g_zout_uav)))
            return false;

        if (need_out)
        {
            d.Format = cd.Format;
            if (FAILED(device->CreateTexture2D(&d, nullptr, &g_out)))
            {
                Msg("! [DLSS] cannot create UAV output texture of format %u", cd.Format);
                return false;
            }
        }

        g_tex_w = in.width;
        g_tex_h = in.height;
        g_out_fmt = cd.Format;
        g_have_prev = false;
        Msg("* [DLSS] buffers %ux%u, color format %u", in.width, in.height, cd.Format);
    }

    if (g_depth_src != in.depth)
    {
        safe_release(g_depth_srv);
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        if (FAILED(device->CreateShaderResourceView(in.depth, &sd, &g_depth_srv)))
        {
            Msg("! [DLSS] cannot create depth SRV");
            return false;
        }
        g_depth_src = in.depth;
    }
    return true;
}

bool ensure_feature(ID3D11DeviceContext* ctx, const FrameInputs& in)
{
    const int preset = ps_r__dlss_preset;
    const int mode = (in.outWidth == in.width && in.outHeight == in.height) ? 0 : int(ps_r__dlss_mode);
    if (g_feature && g_feat_w == in.width && g_feat_h == in.height && g_feat_ow == in.outWidth &&
        g_feat_oh == in.outHeight && g_feat_preset == preset && g_feat_mode == mode)
        return true;

    release_feature();

    NVSDK_NGX_Parameter_SetUI(g_params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, unsigned(preset));
    NVSDK_NGX_Parameter_SetUI(g_params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, unsigned(preset));
    NVSDK_NGX_Parameter_SetUI(g_params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, unsigned(preset));
    NVSDK_NGX_Parameter_SetUI(g_params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, unsigned(preset));
    NVSDK_NGX_Parameter_SetUI(g_params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, unsigned(preset));

    NVSDK_NGX_DLSS_Create_Params cp{};
    cp.Feature.InWidth = in.width;
    cp.Feature.InHeight = in.height;
    cp.Feature.InTargetWidth = in.outWidth;
    cp.Feature.InTargetHeight = in.outHeight;
    cp.Feature.InPerfQualityValue = mode_to_pq(mode);
    cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes; // LDR input, MVs not jittered, depth not inverted

    const NVSDK_NGX_Result r = NGX_D3D11_CREATE_DLSS_EXT(ctx, &g_feature, g_params, &cp);
    if (NVSDK_NGX_FAILED(r))
    {
        Msg("! [DLSS] CreateFeature failed: 0x%08x", r);
        g_feature = nullptr;
        return false;
    }
    g_feat_w = in.width;
    g_feat_h = in.height;
    g_feat_ow = in.outWidth;
    g_feat_oh = in.outHeight;
    g_feat_preset = preset;
    g_feat_mode = mode;
    g_have_prev = false;
    Msg("* [DLSS] %s feature created: render %ux%u -> output %ux%u, preset %d", MODE_NAMES[mode], in.width,
        in.height, in.outWidth, in.outHeight, preset);
    return true;
}
} // namespace

bool Enabled(ID3D11Device* device)
{
    if (!ps_r__dlss)
        return false;
    return try_init(device);
}

void NextJitter(u32 width, u32 height, u32 outWidth, Fvector2& outPx, Fvector2& outNdc)
{
    if (!ps_r__dlss || !g_available || !ps_r__dlss_jitter || !width || !height)
    {
        outPx.set(0.f, 0.f);
        outNdc.set(0.f, 0.f);
        return;
    }
    // NVIDIA recommendation: 8 * (output / render)^2 phases, at least 16 for DLAA
    const float ratio = width ? float(outWidth) / float(width) : 1.f;
    const u32 phases = std::clamp(u32(8.f * ratio * ratio + 0.5f), 16u, 72u);
    g_jitter_index = (g_jitter_index + 1) % phases;
    outPx.x = halton(g_jitter_index + 1, 2) - 0.5f;
    outPx.y = halton(g_jitter_index + 1, 3) - 0.5f;
    // D3D: NDC y goes up, pixel y goes down
    outNdc.x = 2.f * outPx.x / float(width);
    outNdc.y = -2.f * outPx.y / float(height);
}

bool Evaluate(ID3D11Device* device, ID3D11DeviceContext1* ctx1, const FrameInputs& in,
    const std::function<void()>& beforeUpscale)
{
    if (!g_available || !ctx1 || !in.color || !in.depth || !in.width || !in.height || !in.outWidth || !in.outHeight)
        return false;
    if (!ensure_textures(device, in))
    {
        release_textures();
        return false;
    }

    // Unjittered view-projection for motion vectors
    Fmatrix cur_vp;
    cur_vp.mul(in.projUnjittered, in.view);

    bool reset = !g_have_prev;
    if (g_have_prev && g_prev_cam.distance_to(Device.vCameraPosition) > 5.f) // teleport, level change, cutscene cut
        reset = true;
    if (reset)
        g_prev_vp = cur_vp;
    g_last_reset = reset;

    MVConstants c;
    c.inv_view.invert(in.view);
    c.cur_vp = cur_vp;
    c.prev_vp = g_prev_vp;
    c.size_p[0] = float(in.width);
    c.size_p[1] = float(in.height);
    c.size_p[2] = in.projUnjittered._11;
    c.size_p[3] = in.projUnjittered._22;
    c.proj[0] = in.projUnjittered._31 + in.jitterNdc.x;
    c.proj[1] = in.projUnjittered._32 + in.jitterNdc.y;
    c.proj[2] = in.projUnjittered._33;
    c.proj[3] = in.projUnjittered._43;

    // Switch to our own clean pipeline state; engine state is restored untouched afterwards
    ID3DDeviceContextState* engine_state = nullptr;
    ctx1->SwapDeviceContextState(g_state, &engine_state);

    bool ok = false;
    do
    {
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(ctx1->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
            break;
        CopyMemory(m.pData, &c, sizeof(c));
        ctx1->Unmap(g_cb, 0);

        ID3D11UnorderedAccessView* uavs[2] = { g_mv_uav, g_zout_uav };
        ctx1->CSSetShader(g_cs, nullptr, 0);
        ctx1->CSSetConstantBuffers(0, 1, &g_cb);
        ctx1->CSSetShaderResources(0, 1, &g_depth_srv);
        ctx1->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
        ctx1->Dispatch((in.width + 7) / 8, (in.height + 7) / 8, 1);
        ctx1->ClearState();

        if (beforeUpscale)
        {
            // guides are ready: hand the context back to the engine state for the callback
            g_last_reset = reset;
            ctx1->SwapDeviceContextState(engine_state, nullptr);
            beforeUpscale();
            ctx1->SwapDeviceContextState(g_state, nullptr);
        }

        if (!ensure_feature(ctx1, in))
            break;

        const float sx = (ps_r__dlss_jsign & 1) ? -1.f : 1.f;
        const float sy = (ps_r__dlss_jsign & 2) ? -1.f : 1.f;

        NVSDK_NGX_D3D11_DLSS_Eval_Params ep{};
        ep.Feature.pInColor = in.color;
        ep.Feature.pInOutput = in.output ? in.output : g_out;
        ep.pInDepth = g_zout;
        ep.pInMotionVectors = g_mv;
        ep.InJitterOffsetX = sx * in.jitterPx.x;
        ep.InJitterOffsetY = sy * in.jitterPx.y;
        ep.InRenderSubrectDimensions.Width = in.width;
        ep.InRenderSubrectDimensions.Height = in.height;
        ep.InReset = reset ? 1 : 0;
        ep.InMVScaleX = 1.f;
        ep.InMVScaleY = 1.f;
        ep.InFrameTimeDeltaInMsec = Device.fTimeDelta * 1000.f;

        const NVSDK_NGX_Result r = NGX_D3D11_EVALUATE_DLSS_EXT(ctx1, g_feature, g_params, &ep);
        ctx1->ClearState();
        if (NVSDK_NGX_FAILED(r))
        {
            static u32 errors = 0;
            if (errors++ < 5)
                Msg("! [DLSS] Evaluate failed: 0x%08x", r);
            break;
        }

        if (!in.output)
            ctx1->CopyResource(in.color, g_out);
        ok = true;
    } while (false);

    ctx1->SwapDeviceContextState(engine_state, nullptr);
    safe_release(engine_state);

    g_prev_vp = cur_vp;
    g_prev_cam = Device.vCameraPosition;
    g_have_prev = true;
    return ok;
}

bool GetRenderSize(ID3D11Device* device, u32 outW, u32 outH, u32& rw, u32& rh)
{
    rw = outW;
    rh = outH;
    const int mode = int(ps_r__dlss_mode);
    if (!ps_r__dlss || mode <= 0 || mode > 4 || !try_init(device))
        return false;

    unsigned int ow = 0, oh = 0, maxw = 0, maxh = 0, minw = 0, minh = 0;
    float sharpness = 0.f;
    const NVSDK_NGX_Result r = NGX_DLSS_GET_OPTIMAL_SETTINGS(
        g_params, outW, outH, mode_to_pq(mode), &ow, &oh, &maxw, &maxh, &minw, &minh, &sharpness);
    if (NVSDK_NGX_FAILED(r) || !ow || !oh)
    {
        // fallback ratios
        constexpr float scales[] = { 1.f, 0.667f, 0.58f, 0.5f, 0.333f };
        ow = u32(float(outW) * scales[mode]);
        oh = u32(float(outH) * scales[mode]);
        Msg("! [DLSS] GetOptimalSettings failed (0x%08x), using %ux%u", r, ow, oh);
    }
    rw = ow;
    rh = oh;
    Msg("* [DLSS] %s: render %ux%u -> output %ux%u", MODE_NAMES[mode], rw, rh, outW, outH);
    return rw != outW || rh != outH;
}

void BeginRenderRes(u32 rw, u32 rh)
{
    if (g_render_res)
        return;
    g_saved_w = Device.dwWidth;
    g_saved_h = Device.dwHeight;
    Device.dwWidth = rw;
    Device.dwHeight = rh;
    g_render_res = true;
}

void EndRenderRes()
{
    if (!g_render_res)
        return;
    Device.dwWidth = g_saved_w;
    Device.dwHeight = g_saved_h;
    g_render_res = false;
}

bool InRenderRes() { return g_render_res; }

ID3D11Texture2D* LastMotionVectors() { return g_mv; }
ID3D11Texture2D* LastDepth() { return g_zout; }
bool LastWasReset() { return g_last_reset; }

void Shutdown(ID3D11Device* device)
{
    ShutdownNR();
    release_feature();
    release_textures();
    safe_release(g_cs);
    safe_release(g_cb);
    safe_release(g_state);
    if (g_params)
    {
        NVSDK_NGX_D3D11_DestroyParameters(g_params);
        g_params = nullptr;
    }
    if (g_available && device)
        NVSDK_NGX_D3D11_Shutdown1(device);
    g_available = false;
    g_init_tried = false;
    g_have_prev = false;
}
} // namespace dlss
} // namespace xray::render::RENDER_NAMESPACE

#endif // XR_DLSS
