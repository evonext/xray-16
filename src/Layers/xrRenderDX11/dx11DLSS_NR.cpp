#include "stdafx.h"
#pragma hdrstop

#if defined(XR_DLSS)

#include "dx11DLSS.h"
#include "Layers/xrRender/xrRender_console.h"

#include <atomic>
#include <mutex>
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

// D3D11 compute pass: color -> shared color with alpha forced to 1 (DLSS-NR expects an opaque input;
// the engine keeps other data in the alpha channel)
ID3D11ComputeShader* nr_opaque_cs = nullptr;
ID3D11ShaderResourceView* nr_src_srv = nullptr;
ID3D11Texture2D* nr_src_tex = nullptr; // not owned
ID3D11UnorderedAccessView* nr_color_uav = nullptr;

constexpr const char* NR_OPAQUE_CS = R"(
Texture2D<float4> t_in : register(t0);
RWTexture2D<float4> u_out : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    u_out[id.xy] = float4(t_in.Load(int3(id.xy, 0)).rgb, 1.0);
}
)";

bool opaque_copy(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* src, ID3D11Texture2D* dst, u32 w, u32 h)
{
    if (!nr_opaque_cs)
    {
        ID3DBlob* blob = nullptr;
        ID3DBlob* err = nullptr;
        if (FAILED(D3DCompile(NR_OPAQUE_CS, xr_strlen(NR_OPAQUE_CS), "nr_opaque", nullptr, nullptr, "main", "cs_5_0", 0, 0, &blob, &err)))
        {
            if (err) err->Release();
            return false;
        }
        if (err) err->Release();
        dev->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &nr_opaque_cs);
        blob->Release();
        if (!nr_opaque_cs)
            return false;
    }
    if (nr_src_tex != src)
    {
        if (nr_src_srv) { nr_src_srv->Release(); nr_src_srv = nullptr; }
        if (FAILED(dev->CreateShaderResourceView(src, nullptr, &nr_src_srv)))
            return false;
        nr_src_tex = src;
    }
    if (!nr_color_uav && FAILED(dev->CreateUnorderedAccessView(dst, nullptr, &nr_color_uav)))
        return false;
    ctx->CSSetShader(nr_opaque_cs, nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &nr_src_srv);
    ctx->CSSetUnorderedAccessViews(0, 1, &nr_color_uav, nullptr);
    ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    ID3D11ShaderResourceView* null_srv = nullptr;
    ID3D11UnorderedAccessView* null_uav = nullptr;
    ctx->CSSetShaderResources(0, 1, &null_srv);
    ctx->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
    ctx->CSSetShader(nullptr, nullptr, 0);
    return true;
}

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

std::atomic<int> nr_log_lines{ 0 };
void NVSDK_CONV nr_ngx_log(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature feature)
{
    if (!message || nr_log_lines.fetch_add(1) > 600)
        return;
    string1024 line;
    xr_strcpy(line, message);
    for (char* c = line; *c; ++c)
        if (*c == '\n' || *c == '\r')
            *c = ' ';
    Msg("- [NGX:%d] %s", int(feature), line);
}

// ---- GPU architecture spoof (same approach as NeuralScreen) ----
// nvngx_dlssnr.dll asks nvapi for the GPU architecture and refuses anything below Blackwell, although the
// runtime carries kernels for sm_86/89 as well. NvAPI_GPU_GetArchInfo is patched in this process only,
// and only while DLSS-NR calls run (DLSS SR keeps seeing the real GPU).
constexpr u32 NVAPI_ID_GPU_GETARCHINFO = 0xD8265D24;
constexpr u32 NV_ARCH_BLACKWELL = 0x1B0;
std::mutex arch_mutex;
u8* arch_target = nullptr;
u8 arch_saved[12] = {};
u8 arch_patch[12] = {};
bool arch_ready = false;
int arch_depth = 0;

int __cdecl arch_hook(void* gpu, u32* info);

void arch_write(const u8* bytes)
{
    memcpy(arch_target, bytes, sizeof(arch_patch));
    FlushInstructionCache(GetCurrentProcess(), arch_target, sizeof(arch_patch));
}

int __cdecl arch_hook(void* gpu, u32* info)
{
    std::lock_guard<std::mutex> lock(arch_mutex);
    arch_write(arch_saved);
    const int r = reinterpret_cast<int(__cdecl*)(void*, u32*)>(arch_target)(gpu, info);
    arch_write(arch_patch);
    if (r == 0 && info && info[1] < NV_ARCH_BLACKWELL) // NV_GPU_ARCH_INFO: version, architecture, ...
        info[1] = NV_ARCH_BLACKWELL;
    return r;
}

bool arch_init()
{
    if (arch_ready)
        return true;
    HMODULE nvapi = LoadLibraryW(L"nvapi64.dll");
    if (!nvapi)
        return false;
    using qi_fn = void*(__cdecl*)(u32);
    const auto qi = reinterpret_cast<qi_fn>(GetProcAddress(nvapi, "nvapi_QueryInterface"));
    if (!qi)
        return false;
    arch_target = static_cast<u8*>(qi(NVAPI_ID_GPU_GETARCHINFO));
    if (!arch_target)
        return false;
    DWORD old = 0;
    if (!VirtualProtect(arch_target, sizeof(arch_patch), PAGE_EXECUTE_READWRITE, &old))
        return false;
    memcpy(arch_saved, arch_target, sizeof(arch_saved));
    // mov rax, imm64 ; jmp rax
    arch_patch[0] = 0x48;
    arch_patch[1] = 0xB8;
    const u64 addr = reinterpret_cast<u64>(&arch_hook);
    memcpy(arch_patch + 2, &addr, 8);
    arch_patch[10] = 0xFF;
    arch_patch[11] = 0xE0;
    arch_ready = true;
    Msg("* [DLSS-NR] GPU architecture spoof ready (reports Blackwell to the DLSS-NR runtime only)");
    return true;
}

struct ArchSpoofScope
{
    bool on = false;
    ArchSpoofScope()
    {
        if (!ps_r__dlss_nr_arch_spoof || !arch_ready)
            return;
        std::lock_guard<std::mutex> lock(arch_mutex);
        if (arch_depth++ == 0)
            arch_write(arch_patch);
        on = true;
    }
    ~ArchSpoofScope()
    {
        if (!on)
            return;
        std::lock_guard<std::mutex> lock(arch_mutex);
        if (--arch_depth == 0)
            arch_write(arch_saved);
    }
};

// ---- direct runtime calls through the forwarder (bypasses NGX Core's snippet arch metadata check) ----
using fwd_load_fn = int(__cdecl*)(const wchar_t*);
using fwd_init_fn = int(__cdecl*)(unsigned long long, const wchar_t*, void*, int, const void*);
using fwd_create_fn = int(__cdecl*)(void*, int, void*, void**);
using fwd_eval_fn = int(__cdecl*)(void*, const void*, const void*, void*);
using fwd_release_fn = int(__cdecl*)(void*);
fwd_init_fn fwd_init = nullptr;
fwd_create_fn fwd_create = nullptr;
fwd_eval_fn fwd_eval = nullptr;
fwd_release_fn fwd_release = nullptr;
bool nr_direct = false;

std::wstring exe_dir()
{
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring s(buf);
    const size_t p = s.find_last_of(L"\\/");
    return p == std::wstring::npos ? L"" : s.substr(0, p + 1);
}

bool init_direct(const std::wstring& logs)
{
    const std::wstring dir = exe_dir();
    HMODULE fwd = LoadLibraryW((dir + L"nvngx.dll_xray-forwarder.dll").c_str());
    if (!fwd)
    {
        Msg("! [DLSS-NR] forwarder nvngx.dll_xray-forwarder.dll not found, using NGX Core path");
        return false;
    }
    const auto load = reinterpret_cast<fwd_load_fn>(GetProcAddress(fwd, "XrNrFwdLoad"));
    fwd_init = reinterpret_cast<fwd_init_fn>(GetProcAddress(fwd, "XrNrFwdInitExt"));
    fwd_create = reinterpret_cast<fwd_create_fn>(GetProcAddress(fwd, "XrNrFwdCreate"));
    fwd_eval = reinterpret_cast<fwd_eval_fn>(GetProcAddress(fwd, "XrNrFwdEvaluate"));
    fwd_release = reinterpret_cast<fwd_release_fn>(GetProcAddress(fwd, "XrNrFwdRelease"));
    if (!load || !fwd_init || !fwd_create || !fwd_eval || !fwd_release)
        return false;
    if (!load((dir + L"nvngx_dlssnr.dll").c_str()))
    {
        Msg("! [DLSS-NR] nvngx_dlssnr.dll not found next to xrEngine.exe (or missing exports)");
        return false;
    }
    int r;
    {
        ArchSpoofScope spoof;
        r = fwd_init(0x1000000ull, logs.c_str(), nr_dev12, NVSDK_NGX_Version_API, g_nr_params);
    }
    Msg("%s [DLSS-NR] direct runtime Init_Ext -> 0x%08x", NVSDK_NGX_FAILED(NVSDK_NGX_Result(r)) ? "!" : "*", r);
    return !NVSDK_NGX_FAILED(NVSDK_NGX_Result(r));
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
    // NGX log lines go to the game log (verbose), so feature failures can be diagnosed
    NVSDK_NGX_FeatureCommonInfo common{};
    common.LoggingInfo.LoggingCallback = &nr_ngx_log;
    common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_VERBOSE;
    common.LoggingInfo.DisableOtherLoggingSinks = false;
    NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init_with_ProjectID(
        NGX_PROJECT_ID_NR, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "OpenXRay", logs.c_str(), nr_dev12, &common);
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
    NVSDK_NGX_Parameter* own = nullptr;
    if (NVSDK_NGX_SUCCEED(NVSDK_NGX_D3D12_AllocateParameters(&own)) && own)
        g_nr_params = own; // our own block; capability block stays owned by NGX
    if (ps_r__dlss_nr_arch_spoof && !arch_init())
        Msg("! [DLSS-NR] GPU architecture spoof unavailable");
    nr_direct = ps_r__dlss_nr_direct && init_direct(logs);
    Msg("* [DLSS-NR] D3D12 interop device ready (%s)", nr_direct ? "direct runtime" : "NGX Core");
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
        Msg("! [DLSS-NR] cannot create shared %s %ux%u fmt %u: 0x%08x (device removed reason 0x%08x)", name, w, h, fmt, hr,
            nr_dev12 ? nr_dev12->GetDeviceRemovedReason() : 0);
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
        if (nr_direct)
            fwd_release(g_nr_feature);
        else
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

    if (nr_color_uav) { nr_color_uav->Release(); nr_color_uav = nullptr; }
    if (!make_shared(device11, nr_color, w, h, color_fmt, true, "color") ||
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
    NVSDK_NGX_Parameter_SetI(g_nr_params, NVSDK_NGX_Parameter_PerfQualityValue, 0); // as the RenoDX DLSS5 add-on does
    NVSDK_NGX_Parameter_SetI(g_nr_params, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, NVSDK_NGX_DLSS_Feature_Flags_MVLowRes);
    NVSDK_NGX_Parameter_SetUI(g_nr_params, NVSDK_NGX_Parameter_CreationNodeMask, 1);
    NVSDK_NGX_Parameter_SetUI(g_nr_params, NVSDK_NGX_Parameter_VisibilityNodeMask, 1);

    const u32 slot = nr_frame & 1;
    wait_cpu(nr_alloc_fence[slot]);
    nr_alloc[slot]->Reset();
    nr_list->Reset(nr_alloc[slot], nullptr);
    NVSDK_NGX_Result r;
    {
        ArchSpoofScope spoof;
        r = nr_direct ? NVSDK_NGX_Result(fwd_create(nr_list, int(NGX_FEATURE_DLSS_NR), g_nr_params, reinterpret_cast<void**>(&g_nr_feature)))
                      : NVSDK_NGX_D3D12_CreateFeature(nr_list, NGX_FEATURE_DLSS_NR, g_nr_params, &g_nr_feature);
    }
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

    // DLAA path: color is still bound as the engine's render target here. A resource bound as RTV
    // cannot be read through an SRV (D3D11 silently nulls the SRV), so unbind outputs first.
    // RCache.Invalidate() after EvaluateNR rebinds everything the engine needs.
    ctx->OMSetRenderTargets(0, nullptr, nullptr);

    // D3D11 -> shared
    if (!opaque_copy(device, ctx, color, nr_color.t11, cd.Width, cd.Height))
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
    NVSDK_NGX_Result r;
    {
        ArchSpoofScope spoof;
        r = nr_direct ? NVSDK_NGX_Result(fwd_eval(nr_list, g_nr_feature, g_nr_params, nullptr))
                      : NVSDK_NGX_D3D12_EvaluateFeature(nr_list, g_nr_feature, g_nr_params, nullptr);
    }
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
    if (nr_color_uav) { nr_color_uav->Release(); nr_color_uav = nullptr; }
    if (nr_src_srv) { nr_src_srv->Release(); nr_src_srv = nullptr; }
    nr_src_tex = nullptr;
    if (nr_opaque_cs) { nr_opaque_cs->Release(); nr_opaque_cs = nullptr; }
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
