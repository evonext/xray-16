#include "stdafx.h"
#pragma hdrstop

#if defined(XR_DLSS)

#include "dx11DLSS.h"
#include "Layers/xrRender/xrRender_console.h"

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>

// DLSS-NR ("DLSS 5" neural rendering) through NGX feature 18 on a private D3D12 device.
//
// Frame flow (all GPU-side, no CPU stalls in the steady state):
//   D3D11: copy color / motion vectors / depth into shared textures -> Signal(fence, N)
//   D3D12: Wait(fence, N) -> NGX Evaluate(feature 18) -> Signal(fence, N+1)
//   D3D11: Wait(fence, N+1) -> copy shared output back into the color target
//
// Parameter names / values follow what NVIDIA's sl.dlss_nr plugin passes to NGX.

namespace xray::render::RENDER_NAMESPACE
{
namespace dlss
{
namespace nr_impl // named (not anonymous): CI builds use unity files together with dx11DLSS.cpp
{
constexpr NVSDK_NGX_Feature NGX_FEATURE_DLSS_NR = static_cast<NVSDK_NGX_Feature>(18);
constexpr const char* NGX_PROJECT_ID_NR = "6b1f6f0e-2a63-4d8e-9d2b-0f9e5a1c7d43";

template <typename T>
void rel(T*& p)
{
    if (p)
    {
        p->Release();
        p = nullptr;
    }
}

struct SharedTex
{
    ID3D12Resource* r12{};
    ID3D11Texture2D* t11{};
    void release()
    {
        rel(t11);
        rel(r12);
    }
};

bool g_nr_tried = false;
bool g_nr_ok = false; // D3D12 + NGX ready
bool g_nr_failed_feature = false; // feature creation failed: stop retrying every frame

HMODULE nr_d3d12_dll = nullptr;
ID3D12Device* nr_dev12 = nullptr;
ID3D12CommandQueue* nr_queue = nullptr;
ID3D12CommandAllocator* nr_alloc[2] = {};
ID3D12GraphicsCommandList* nr_list = nullptr;
ID3D12Fence* nr_fence12 = nullptr;
ID3D11Fence* nr_fence11 = nullptr;
ID3D11DeviceContext4* nr_ctx4 = nullptr;
HANDLE nr_event = nullptr;
u64 nr_fence_value = 0;
u64 nr_alloc_fence[2] = {};
u32 nr_frame = 0;

NVSDK_NGX_Parameter* g_nr_params = nullptr;
NVSDK_NGX_Handle* g_nr_feature = nullptr;
u32 g_nr_w = 0, g_nr_h = 0;
int g_nr_preset = -1;

SharedTex nr_color, nr_out, nr_mvec, nr_depth;
DXGI_FORMAT nr_color_fmt = DXGI_FORMAT_UNKNOWN;
u32 nr_guide_w = 0, nr_guide_h = 0;
u32 nr_res_w = 0, nr_res_h = 0;

std::wstring logs_dir_w()
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

// Scaling-ratio callback NGX asks for (we always run DLSS-NR at native resolution: ratio 1)
NVSDK_NGX_Result NVSDK_CONV nr_scaling_ratio_cb(NVSDK_NGX_Parameter* params)
{
    if (params)
        NVSDK_NGX_Parameter_SetF(params, "DLSSNR.ScalingRatio", 1.f);
    return NVSDK_NGX_Result_Success;
}

bool init_d3d12(ID3D11Device* device11, ID3D11DeviceContext* ctx11)
{
    nr_d3d12_dll = LoadLibraryW(L"d3d12.dll");
    if (!nr_d3d12_dll)
    {
        Msg("! [DLSS-NR] d3d12.dll not available");
        return false;
    }
    const auto create_device = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(nr_d3d12_dll, "D3D12CreateDevice"));
    if (!create_device)
        return false;

    // Same adapter as the D3D11 device
    IDXGIDevice* dxgi_dev = nullptr;
    IDXGIAdapter* adapter = nullptr;
    if (FAILED(device11->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi_dev))) ||
        FAILED(dxgi_dev->GetAdapter(&adapter)))
    {
        rel(dxgi_dev);
        Msg("! [DLSS-NR] cannot get DXGI adapter");
        return false;
    }
    rel(dxgi_dev);
    HRESULT hr = create_device(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), reinterpret_cast<void**>(&nr_dev12));
    rel(adapter);
    if (FAILED(hr))
    {
        Msg("! [DLSS-NR] D3D12CreateDevice failed: 0x%08x", hr);
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(nr_dev12->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&nr_queue))))
        return false;
    for (auto& a : nr_alloc)
        if (FAILED(nr_dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), reinterpret_cast<void**>(&a))))
            return false;
    if (FAILED(nr_dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, nr_alloc[0], nullptr,
            __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&nr_list))))
        return false;
    nr_list->Close();

    // Shared fence D3D12 <-> D3D11
    if (FAILED(nr_dev12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&nr_fence12))))
    {
        Msg("! [DLSS-NR] cannot create shared fence");
        return false;
    }
    HANDLE fence_handle = nullptr;
    if (FAILED(nr_dev12->CreateSharedHandle(nr_fence12, nullptr, GENERIC_ALL, nullptr, &fence_handle)))
        return false;
    ID3D11Device5* dev5 = nullptr;
    if (FAILED(device11->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void**>(&dev5))))
    {
        CloseHandle(fence_handle);
        Msg("! [DLSS-NR] ID3D11Device5 not available (Windows 10+ required)");
        return false;
    }
    hr = dev5->OpenSharedFence(fence_handle, __uuidof(ID3D11Fence), reinterpret_cast<void**>(&nr_fence11));
    dev5->Release();
    CloseHandle(fence_handle);
    if (FAILED(hr))
    {
        Msg("! [DLSS-NR] OpenSharedFence failed: 0x%08x", hr);
        return false;
    }
    if (FAILED(ctx11->QueryInterface(__uuidof(ID3D11DeviceContext4), reinterpret_cast<void**>(&nr_ctx4))))
        return false;
    nr_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // NGX on the D3D12 device
    const std::wstring logs = logs_dir_w();
    NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init_with_ProjectID(
        NGX_PROJECT_ID_NR, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "OpenXRay", logs.c_str(), nr_dev12);
    if (NVSDK_NGX_FAILED(r))
    {
        Msg("! [DLSS-NR] NGX D3D12 init failed: 0x%08x", r);
        return false;
    }
    r = NVSDK_NGX_D3D12_GetCapabilityParameters(&g_nr_params);
    if (NVSDK_NGX_FAILED(r) || !g_nr_params)
    {
        Msg("! [DLSS-NR] NGX D3D12 GetCapabilityParameters failed: 0x%08x", r);
        return false;
    }
    Msg("* [DLSS-NR] D3D12 interop device ready");
    return true;
}

bool make_shared(ID3D11Device* device11, SharedTex& t, u32 w, u32 h, DXGI_FORMAT fmt, bool uav, const char* name)
{
    t.release();
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w;
    rd.Height = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = fmt;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (uav)
        rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    HRESULT hr = nr_dev12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON,
        nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&t.r12));
    if (FAILED(hr))
    {
        Msg("! [DLSS-NR] cannot create shared %s %ux%u fmt %u: 0x%08x", name, w, h, fmt, hr);
        return false;
    }
    HANDLE handle = nullptr;
    if (FAILED(nr_dev12->CreateSharedHandle(t.r12, nullptr, GENERIC_ALL, nullptr, &handle)))
        return false;
    ID3D11Device1* dev1 = nullptr;
    hr = device11->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&dev1));
    if (SUCCEEDED(hr))
    {
        hr = dev1->OpenSharedResource1(handle, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t.t11));
        dev1->Release();
    }
    CloseHandle(handle);
    if (FAILED(hr))
    {
        Msg("! [DLSS-NR] cannot open shared %s in D3D11: 0x%08x", name, hr);
        t.release();
        return false;
    }
    return true;
}

void release_nr_feature()
{
    if (g_nr_feature)
    {
        NVSDK_NGX_D3D12_ReleaseFeature(g_nr_feature);
        g_nr_feature = nullptr;
    }
    g_nr_w = g_nr_h = 0;
    g_nr_preset = -1;
}

void wait_cpu(u64 value)
{
    if (nr_fence12->GetCompletedValue() < value)
    {
        nr_fence12->SetEventOnCompletion(value, nr_event);
        WaitForSingleObject(nr_event, 2000);
    }
}

bool ensure_resources(ID3D11Device* device11, u32 w, u32 h, DXGI_FORMAT color_fmt, u32 gw, u32 gh)
{
    if (nr_color.t11 && nr_res_w == w && nr_res_h == h && nr_color_fmt == color_fmt && nr_guide_w == gw && nr_guide_h == gh)
        return true;

    // make sure the GPU is done with the old ones
    if (nr_fence_value)
        wait_cpu(nr_fence_value);
    release_nr_feature();
    nr_color.release();
    nr_out.release();
    nr_mvec.release();
    nr_depth.release();

    if (!make_shared(device11, nr_color, w, h, color_fmt, false, "color") ||
        !make_shared(device11, nr_out, w, h, color_fmt, true, "output") ||
        !make_shared(device11, nr_mvec, gw, gh, DXGI_FORMAT_R16G16_FLOAT, false, "motion") ||
        !make_shared(device11, nr_depth, gw, gh, DXGI_FORMAT_R32_FLOAT, false, "depth"))
        return false;
    nr_color_fmt = color_fmt;
    nr_res_w = w;
    nr_res_h = h;
    nr_guide_w = gw;
    nr_guide_h = gh;
    Msg("* [DLSS-NR] shared buffers %ux%u (guides %ux%u), format %u", w, h, gw, gh, color_fmt);
    return true;
}

void set_common_size_params(u32 w, u32 h)
{
    NVSDK_NGX_Parameter_SetUI(g_nr_params, "Width", w);
    NVSDK_NGX_Parameter_SetUI(g_nr_params, "Height", h);
    NVSDK_NGX_Parameter_SetUI(g_nr_params, "OutWidth", w);
    NVSDK_NGX_Parameter_SetUI(g_nr_params, "OutHeight", h);
    NVSDK_NGX_Parameter_SetUI(g_nr_params, "DLSSNR.Width", w);
    NVSDK_NGX_Parameter_SetUI(g_nr_params, "DLSSNR.Height", h);
    NVSDK_NGX_Parameter_SetUI(g_nr_params, "DLSSNR.InputWidth", w);
    NVSDK_NGX_Parameter_SetUI(g_nr_params, "DLSSNR.InputHeight", h);
    NVSDK_NGX_Parameter_SetUI(g_nr_params, "DLSSNR.OutputWidth", w);
    NVSDK_NGX_Parameter_SetUI(g_nr_params, "DLSSNR.OutputHeight", h);
    NVSDK_NGX_Parameter_SetUI(g_nr_params, "DLSSNR.Output.Width", w);
    NVSDK_NGX_Parameter_SetUI(g_nr_params, "DLSSNR.Output.Height", h);
    NVSDK_NGX_Parameter_SetF(g_nr_params, "DLSSNR.ScalingRatio", 1.f);
    NVSDK_NGX_Parameter_SetF(g_nr_params, "DLSSNR.Scale", 1.f);
    NVSDK_NGX_Parameter_SetI(g_nr_params, "DLSSNR.Upscaling", 0);
    NVSDK_NGX_Parameter_SetVoidPointer(g_nr_params, "DLSSNRComputeScalingRatioCallback", reinterpret_cast<void*>(&nr_scaling_ratio_cb));
}

bool ensure_nr_feature(u32 w, u32 h)
{
    const int preset = ps_r__dlss_nr_preset;
    if (g_nr_feature && g_nr_w == w && g_nr_h == h && g_nr_preset == preset)
        return true;
    if (g_nr_failed_feature)
        return false;
    release_nr_feature();

    set_common_size_params(w, h);
    NVSDK_NGX_Parameter_SetUI(g_nr_params, "DLSSNR.Hint.Render.Preset", unsigned(preset));
    NVSDK_NGX_Parameter_SetI(g_nr_params, NVSDK_NGX_Parameter_PerfQualityValue, NVSDK_NGX_PerfQuality_Value_DLAA);
    NVSDK_NGX_Parameter_SetI(g_nr_params, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, NVSDK_NGX_DLSS_Feature_Flags_MVLowRes);
    NVSDK_NGX_Parameter_SetUI(g_nr_params, NVSDK_NGX_Parameter_CreationNodeMask, 1);
    NVSDK_NGX_Parameter_SetUI(g_nr_params, NVSDK_NGX_Parameter_VisibilityNodeMask, 1);

    const u32 slot = nr_frame & 1;
    wait_cpu(nr_alloc_fence[slot]);
    nr_alloc[slot]->Reset();
    nr_list->Reset(nr_alloc[slot], nullptr);
    const NVSDK_NGX_Result r = NVSDK_NGX_D3D12_CreateFeature(nr_list, NGX_FEATURE_DLSS_NR, g_nr_params, &g_nr_feature);
    nr_list->Close();
    ID3D12CommandList* lists[] = { nr_list };
    nr_queue->ExecuteCommandLists(1, lists);
    nr_queue->Signal(nr_fence12, ++nr_fence_value);
    nr_alloc_fence[slot] = nr_fence_value;
    wait_cpu(nr_fence_value);

    if (NVSDK_NGX_FAILED(r) || !g_nr_feature)
    {
        Msg("! [DLSS-NR] CreateFeature(18) failed: 0x%08x (%s)", r,
            r == NVSDK_NGX_Result_FAIL_OutOfDate ? "NVIDIA driver too old for DLSS-NR: needs 615.00+" :
            r == NVSDK_NGX_Result_FAIL_FeatureNotFound ? "nvngx_dlssnr.dll not found next to xrEngine.exe" :
            r == NVSDK_NGX_Result_FAIL_FeatureNotSupported ? "GPU not supported" : "see NGX log");
        g_nr_feature = nullptr;
        g_nr_failed_feature = true;
        return false;
    }
    g_nr_w = w;
    g_nr_h = h;
    g_nr_preset = preset;
    Msg("* [DLSS-NR] feature created %ux%u, preset %d", w, h, preset);
    return true;
}
} // namespace nr_impl
using namespace nr_impl;

bool EvaluateNR(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11Texture2D* color, u32 renderW, u32 renderH)
{
    if (!ps_r__dlss_nr || !color)
        return false;

    if (!g_nr_tried)
    {
        g_nr_tried = true;
        g_nr_ok = init_d3d12(device, ctx);
        if (!g_nr_ok)
            Msg("! [DLSS-NR] disabled for this session");
    }
    if (!g_nr_ok || g_nr_failed_feature)
        return false;

    ID3D11Texture2D* mv = LastMotionVectors();
    ID3D11Texture2D* depth = LastDepth();
    if (!mv || !depth)
        return false;

    D3D11_TEXTURE2D_DESC cd;
    color->GetDesc(&cd);
    if (!ensure_resources(device, cd.Width, cd.Height, cd.Format, renderW, renderH))
    {
        g_nr_ok = false;
        return false;
    }
    if (!ensure_nr_feature(cd.Width, cd.Height))
        return false;

    // D3D11 -> shared
    ctx->CopyResource(nr_color.t11, color);
    ctx->CopyResource(nr_mvec.t11, mv);
    ctx->CopyResource(nr_depth.t11, depth);
    const u64 v_inputs = ++nr_fence_value;
    nr_ctx4->Signal(nr_fence11, v_inputs);
    ctx->Flush();

    // D3D12: NR
    const u32 slot = (++nr_frame) & 1;
    wait_cpu(nr_alloc_fence[slot]);
    nr_alloc[slot]->Reset();
    nr_list->Reset(nr_alloc[slot], nullptr);

    const u32 w = cd.Width, h = cd.Height;
    set_common_size_params(w, h);
    NVSDK_NGX_Parameter_SetD3d12Resource(g_nr_params, "DLSSNR.Color", nr_color.r12);
    NVSDK_NGX_Parameter_SetD3d12Resource(g_nr_params, "DLSSNR.Output", nr_out.r12);
    NVSDK_NGX_Parameter_SetD3d12Resource(g_nr_params, "DLSSNR.MVec", nr_mvec.r12);
    NVSDK_NGX_Parameter_SetD3d12Resource(g_nr_params, "DLSSNR.Depth", nr_depth.r12);
    NVSDK_NGX_Parameter_SetUI(g_nr_params, "DLSSNR.ColorSubrectBaseX", 0);
    NVSDK_NGX_Parameter_SetUI(g_nr_params, "DLSSNR.ColorSubrectBaseY", 0);
    NVSDK_NGX_Parameter_SetUI(g_nr_params, "DLSSNR.ColorSubrectWidth", w);
    NVSDK_NGX_Parameter_SetUI(g_nr_params, "DLSSNR.ColorSubrectHeight", h);
    NVSDK_NGX_Parameter_SetUI(g_nr_params, "DLSSNR.DepthSubrectBaseX", 0);
    NVSDK_NGX_Parameter_SetUI(g_nr_params, "DLSSNR.DepthSubrectBaseY", 0);
    NVSDK_NGX_Parameter_SetUI(g_nr_params, "DLSSNR.DepthSubrectWidth", renderW);
    NVSDK_NGX_Parameter_SetUI(g_nr_params, "DLSSNR.DepthSubrectHeight", renderH);
    NVSDK_NGX_Parameter_SetUI(g_nr_params, "DLSSNR.MVecSubrectBaseX", 0);
    NVSDK_NGX_Parameter_SetUI(g_nr_params, "DLSSNR.MVecSubrectBaseY", 0);
    NVSDK_NGX_Parameter_SetUI(g_nr_params, "DLSSNR.MVecSubrectWidth", renderW);
    NVSDK_NGX_Parameter_SetUI(g_nr_params, "DLSSNR.MVecSubrectHeight", renderH);
    NVSDK_NGX_Parameter_SetUI(g_nr_params, "DLSSNR.OutputSubrectBaseX", 0);
    NVSDK_NGX_Parameter_SetUI(g_nr_params, "DLSSNR.OutputSubrectBaseY", 0);
    NVSDK_NGX_Parameter_SetUI(g_nr_params, "DLSSNR.OutputSubrectWidth", w);
    NVSDK_NGX_Parameter_SetUI(g_nr_params, "DLSSNR.OutputSubrectHeight", h);
    NVSDK_NGX_Parameter_SetF(g_nr_params, "DLSSNR.MVecScaleX", 1.f);
    NVSDK_NGX_Parameter_SetF(g_nr_params, "DLSSNR.MVecScaleY", 1.f);
    NVSDK_NGX_Parameter_SetI(g_nr_params, "DLSSNR.DepthInverted", 0);
    NVSDK_NGX_Parameter_SetI(g_nr_params, "DLSSNR.Enabled", 1);
    NVSDK_NGX_Parameter_SetI(g_nr_params, "DLSSNR.Reset", LastWasReset() ? 1 : 0);
    NVSDK_NGX_Parameter_SetF(g_nr_params, "DLSSNR.Intensity", ps_r__dlss_nr_intensity);
    NVSDK_NGX_Parameter_SetF(g_nr_params, "DLSSNR.GlobalToneStrength", ps_r__dlss_nr_global_tone);
    NVSDK_NGX_Parameter_SetF(g_nr_params, "DLSSNR.LocalToneStrength", ps_r__dlss_nr_local_tone);
    NVSDK_NGX_Parameter_SetF(g_nr_params, "DLSSNR.LocalStructureStrength", ps_r__dlss_nr_local_structure);
    NVSDK_NGX_Parameter_SetF(g_nr_params, "DLSSNR.SkinStructureStrength", ps_r__dlss_nr_skin_structure);
    NVSDK_NGX_Parameter_SetI(g_nr_params, "DLSSNR.UseAutoMask", ps_r__dlss_nr_automask);
    NVSDK_NGX_Parameter_SetUI(g_nr_params, "DLSSNR.Style", unsigned(ps_r__dlss_nr_style));
    NVSDK_NGX_Parameter_SetI(g_nr_params, "DLSSNR.UICorrection", ps_r__dlss_nr_ui_correction);
    NVSDK_NGX_Parameter_SetI(g_nr_params, "DLSS.Indicator.Invert.X.Axis", 0);
    NVSDK_NGX_Parameter_SetI(g_nr_params, "DLSS.Indicator.Invert.Y.Axis", 0);

    nr_queue->Wait(nr_fence12, v_inputs);
    const NVSDK_NGX_Result r = NVSDK_NGX_D3D12_EvaluateFeature(nr_list, g_nr_feature, g_nr_params, nullptr);
    nr_list->Close();
    ID3D12CommandList* lists[] = { nr_list };
    nr_queue->ExecuteCommandLists(1, lists);
    const u64 v_done = ++nr_fence_value;
    nr_queue->Signal(nr_fence12, v_done);
    nr_alloc_fence[slot] = v_done;

    // D3D11 waits for NR on the GPU, then takes the result
    nr_ctx4->Wait(nr_fence11, v_done);
    if (NVSDK_NGX_FAILED(r))
    {
        static u32 errors = 0;
        if (errors++ < 5)
            Msg("! [DLSS-NR] Evaluate failed: 0x%08x", r);
        return false;
    }
    ctx->CopyResource(color, nr_out.t11);

    static bool logged = false;
    if (!logged)
    {
        logged = true;
        Msg("* [DLSS-NR] first frame evaluated");
    }
    return true;
}

void ShutdownNR()
{
    if (nr_queue && nr_fence12 && nr_fence_value)
        wait_cpu(nr_fence_value);
    release_nr_feature();
    nr_color.release();
    nr_out.release();
    nr_mvec.release();
    nr_depth.release();
    if (g_nr_params)
    {
        NVSDK_NGX_D3D12_DestroyParameters(g_nr_params);
        g_nr_params = nullptr;
    }
    if (g_nr_ok && nr_dev12)
        NVSDK_NGX_D3D12_Shutdown1(nr_dev12);
    rel(nr_ctx4);
    rel(nr_fence11);
    rel(nr_fence12);
    rel(nr_list);
    for (auto& a : nr_alloc)
        rel(a);
    rel(nr_queue);
    rel(nr_dev12);
    if (nr_event)
    {
        CloseHandle(nr_event);
        nr_event = nullptr;
    }
    nr_alloc_fence[0] = nr_alloc_fence[1] = 0;
    nr_fence_value = 0;
    g_nr_tried = false;
    g_nr_ok = false;
    g_nr_failed_feature = false;
}
} // namespace dlss
} // namespace xray::render::RENDER_NAMESPACE

#endif // XR_DLSS
