// Tiny forwarder for the DLSS-NR runtime (nvngx_dlssnr.dll).
//
// The runtime only accepts calls whose caller module path contains "nvngx.dll" (normally NGX Core).
// This DLL is shipped as "nvngx.dll_xray-forwarder.dll" and forwards the four entry points the
// engine needs, so the calls originate from a module that satisfies that check.
// Built by CI with:  cl /LD /Od forwarder.cpp
#include <windows.h>

// Keep real calls (no tail-call jumps): the runtime looks at its caller's return address.
#pragma optimize("", off)

namespace
{
HMODULE g_rt = nullptr;
FARPROC g_init = nullptr, g_create = nullptr, g_eval = nullptr, g_release = nullptr;
}

extern "C" __declspec(dllexport) int XrNrFwdLoad(const wchar_t* runtime_path)
{
    if (!g_rt)
        g_rt = LoadLibraryW(runtime_path);
    if (!g_rt)
        return 0;
    g_init = GetProcAddress(g_rt, "NVSDK_NGX_D3D12_Init_Ext");
    g_create = GetProcAddress(g_rt, "NVSDK_NGX_D3D12_CreateFeature");
    g_eval = GetProcAddress(g_rt, "NVSDK_NGX_D3D12_EvaluateFeature");
    g_release = GetProcAddress(g_rt, "NVSDK_NGX_D3D12_ReleaseFeature");
    return (g_init && g_create && g_eval && g_release) ? 1 : 0;
}

extern "C" __declspec(dllexport) int XrNrFwdInitExt(
    unsigned long long app_id, const wchar_t* data_path, void* device, int sdk_version, const void* params)
{
    if (!g_init)
        return int(0xBAD00007);
    using fn = int(__cdecl*)(unsigned long long, const wchar_t*, void*, int, const void*);
    volatile int r = reinterpret_cast<fn>(g_init)(app_id, data_path, device, sdk_version, params);
    return r;
}

extern "C" __declspec(dllexport) int XrNrFwdCreate(void* cmd_list, int feature, void* params, void** out_handle)
{
    if (!g_create)
        return int(0xBAD00007);
    using fn = int(__cdecl*)(void*, int, void*, void**);
    volatile int r = reinterpret_cast<fn>(g_create)(cmd_list, feature, params, out_handle);
    return r;
}

extern "C" __declspec(dllexport) int XrNrFwdEvaluate(void* cmd_list, const void* handle, const void* params, void* callback)
{
    if (!g_eval)
        return int(0xBAD00007);
    using fn = int(__cdecl*)(void*, const void*, const void*, void*);
    volatile int r = reinterpret_cast<fn>(g_eval)(cmd_list, handle, params, callback);
    return r;
}

extern "C" __declspec(dllexport) int XrNrFwdRelease(void* handle)
{
    if (!g_release)
        return int(0xBAD00007);
    using fn = int(__cdecl*)(void*);
    volatile int r = reinterpret_cast<fn>(g_release)(handle);
    return r;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
