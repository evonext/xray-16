#include "stdafx.h"
#pragma hdrstop

#if defined(XR_DLSS)

#include "dx11RT.h"
#include "Layers/xrRender/xrRender_console.h"
#include "xrEngine/IGame_Level.h"
#include "xrCDB/xr_area.h"

#include <d3d12.h>
#include <dxgi1_4.h>

// Stage 1 of the ray tracing work: a private D3D12 device (same adapter as D3D11) with DXR 1.1,
// and a BLAS/TLAS built from the level's static collision geometry (CObjectSpace::Static).
// Later stages trace RayQuery compute shaders against this TLAS and hand masks back to D3D11
// through shared textures (same scheme as dx11DLSS_NR.cpp).

namespace xray::render::RENDER_NAMESPACE
{
namespace rt
{
namespace rt_impl // named (not anonymous): unity builds merge .cpp files
{
template <typename T>
void rt_rel(T*& p)
{
    if (p)
    {
        p->Release();
        p = nullptr;
    }
}

bool rt_tried = false;
bool rt_ok = false; // device with DXR 1.1 ready

HMODULE rt_d3d12_dll = nullptr;
ID3D12Device5* rt_dev = nullptr;
ID3D12CommandQueue* rt_queue = nullptr;
ID3D12CommandAllocator* rt_alloc = nullptr;
ID3D12GraphicsCommandList4* rt_list = nullptr;
ID3D12Fence* rt_fence = nullptr;
HANDLE rt_event = nullptr;
u64 rt_fence_value = 0;

// Acceleration structures of the current level
ID3D12Resource* rt_blas = nullptr;
ID3D12Resource* rt_tlas = nullptr;
const CDB::MODEL* rt_built_model = nullptr; // what the BVH was built from
u32 rt_built_tris = 0;
bool rt_build_failed = false; // do not retry every frame for the same model

void rt_wait_gpu()
{
    rt_queue->Signal(rt_fence, ++rt_fence_value);
    if (rt_fence->GetCompletedValue() < rt_fence_value)
    {
        rt_fence->SetEventOnCompletion(rt_fence_value, rt_event);
        WaitForSingleObject(rt_event, INFINITE);
    }
}

ID3D12Resource* rt_buffer(u64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = heap;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Alignment = 0;
    rd.Width = size < 256 ? 256 : size;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = flags;
    ID3D12Resource* r = nullptr;
    const HRESULT hr = rt_dev->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&r));
    if (FAILED(hr))
    {
        Msg("! [RT] buffer allocation failed (%llu bytes): 0x%08x", static_cast<unsigned long long>(size), hr);
        return nullptr;
    }
    return r;
}

bool rt_init(ID3D11Device* device11)
{
    rt_d3d12_dll = LoadLibraryW(L"d3d12.dll");
    if (!rt_d3d12_dll)
    {
        Msg("! [RT] d3d12.dll not available");
        return false;
    }
    const auto create_device = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(rt_d3d12_dll, "D3D12CreateDevice"));
    if (!create_device)
        return false;

    IDXGIDevice* dxgi_dev = nullptr;
    IDXGIAdapter* adapter = nullptr;
    if (FAILED(device11->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi_dev))) ||
        FAILED(dxgi_dev->GetAdapter(&adapter)))
    {
        rt_rel(dxgi_dev);
        Msg("! [RT] cannot get DXGI adapter");
        return false;
    }
    rt_rel(dxgi_dev);
    DXGI_ADAPTER_DESC ad{};
    adapter->GetDesc(&ad);
    HRESULT hr = create_device(adapter, D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device5), reinterpret_cast<void**>(&rt_dev));
    rt_rel(adapter);
    if (FAILED(hr))
    {
        Msg("! [RT] D3D12CreateDevice (ID3D12Device5) failed: 0x%08x", hr);
        return false;
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5{};
    if (FAILED(rt_dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5))) ||
        o5.RaytracingTier < D3D12_RAYTRACING_TIER_1_1)
    {
        Msg("! [RT] DXR 1.1 not supported (tier %d)", int(o5.RaytracingTier));
        return false;
    }
    D3D12_FEATURE_DATA_SHADER_MODEL sm{ D3D_SHADER_MODEL_6_5 };
    if (FAILED(rt_dev->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))) ||
        sm.HighestShaderModel < D3D_SHADER_MODEL_6_5)
    {
        Msg("! [RT] shader model 6.5 not supported");
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(rt_dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&rt_queue))) ||
        FAILED(rt_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
            reinterpret_cast<void**>(&rt_alloc))) ||
        FAILED(rt_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, rt_alloc, nullptr,
            __uuidof(ID3D12GraphicsCommandList4), reinterpret_cast<void**>(&rt_list))) ||
        FAILED(rt_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&rt_fence))))
    {
        Msg("! [RT] cannot create D3D12 queue / command list / fence");
        return false;
    }
    rt_list->Close();
    rt_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    char name[128];
    WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof(name), nullptr, nullptr);
    Msg("* [RT] D3D12 device ready: %s, DXR tier %d, SM 6.5", name, int(o5.RaytracingTier) == D3D12_RAYTRACING_TIER_1_1 ? 11 : int(o5.RaytracingTier));
    return true;
}

void rt_release_as()
{
    if (rt_queue && rt_fence && rt_fence_value)
        rt_wait_gpu();
    rt_rel(rt_tlas);
    rt_rel(rt_blas);
    if (rt_built_model)
        Msg("* [RT] BVH released");
    rt_built_model = nullptr;
    rt_built_tris = 0;
}

bool rt_build(const CDB::MODEL& model)
{
    const u32 nv = model.get_verts_count();
    const u32 nt = model.get_tris_count();
    const Fvector* verts = model.get_verts();
    const CDB::TRI* tris = model.get_tris();
    if (!nv || !nt || !verts || !tris)
    {
        Msg("! [RT] level static geometry is empty");
        return false;
    }

    CTimer timer;
    timer.Start();

    // Geometry in upload heaps (read once by the build, then freed)
    const u64 vb_size = u64(nv) * sizeof(Fvector);
    const u64 ib_size = u64(nt) * 3 * sizeof(u32);
    ID3D12Resource* vb = rt_buffer(vb_size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource* ib = rt_buffer(ib_size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!vb || !ib)
    {
        rt_rel(vb);
        rt_rel(ib);
        return false;
    }
    void* p = nullptr;
    D3D12_RANGE no_read{ 0, 0 };
    vb->Map(0, &no_read, &p);
    CopyMemory(p, verts, size_t(vb_size));
    vb->Unmap(0, nullptr);
    Fbox bb;
    bb.invalidate();
    for (u32 i = 0; i < nv; ++i)
        bb.modify(verts[i]);
    ib->Map(0, &no_read, &p);
    u32* idx = static_cast<u32*>(p);
    u32 bad = 0;
    for (u32 i = 0; i < nt; ++i)
    {
        for (u32 k = 0; k < 3; ++k)
        {
            u32 v = tris[i].verts[k];
            if (v >= nv)
            {
                v = 0;
                ++bad;
            }
            idx[i * 3 + k] = v;
        }
    }
    ib->Unmap(0, nullptr);
    if (bad)
        Msg("! [RT] %u out-of-range indices clamped", bad);

    D3D12_RAYTRACING_GEOMETRY_DESC geom{};
    geom.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geom.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geom.Triangles.VertexBuffer.StartAddress = vb->GetGPUVirtualAddress();
    geom.Triangles.VertexBuffer.StrideInBytes = sizeof(Fvector);
    geom.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geom.Triangles.VertexCount = nv;
    geom.Triangles.IndexBuffer = ib->GetGPUVirtualAddress();
    geom.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
    geom.Triangles.IndexCount = nt * 3;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS bl{};
    bl.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    bl.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    bl.NumDescs = 1;
    bl.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    bl.pGeometryDescs = &geom;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO bl_info{};
    rt_dev->GetRaytracingAccelerationStructurePrebuildInfo(&bl, &bl_info);

    // TLAS: one instance with identity transform
    D3D12_RAYTRACING_INSTANCE_DESC inst{};
    inst.Transform[0][0] = inst.Transform[1][1] = inst.Transform[2][2] = 1.f;
    inst.InstanceMask = 0xFF;
    inst.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tl{};
    tl.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    tl.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    tl.NumDescs = 1;
    tl.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tl_info{};
    rt_dev->GetRaytracingAccelerationStructurePrebuildInfo(&tl, &tl_info);

    const u64 scratch_size = std::max(bl_info.ScratchDataSizeInBytes, tl_info.ScratchDataSizeInBytes);
    ID3D12Resource* scratch = rt_buffer(scratch_size, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource* inst_buf = rt_buffer(sizeof(inst), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_GENERIC_READ);
    rt_blas = rt_buffer(bl_info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    rt_tlas = rt_buffer(tl_info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    if (!scratch || !inst_buf || !rt_blas || !rt_tlas)
    {
        rt_rel(scratch);
        rt_rel(inst_buf);
        rt_rel(vb);
        rt_rel(ib);
        rt_rel(rt_blas);
        rt_rel(rt_tlas);
        return false;
    }
    inst.AccelerationStructure = rt_blas->GetGPUVirtualAddress();
    inst_buf->Map(0, &no_read, &p);
    CopyMemory(p, &inst, sizeof(inst));
    inst_buf->Unmap(0, nullptr);
    tl.InstanceDescs = inst_buf->GetGPUVirtualAddress();

    rt_alloc->Reset();
    rt_list->Reset(rt_alloc, nullptr);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd{};
    bd.Inputs = bl;
    bd.DestAccelerationStructureData = rt_blas->GetGPUVirtualAddress();
    bd.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
    rt_list->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);

    D3D12_RESOURCE_BARRIER uav{};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav.UAV.pResource = rt_blas;
    rt_list->ResourceBarrier(1, &uav);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC td{};
    td.Inputs = tl;
    td.DestAccelerationStructureData = rt_tlas->GetGPUVirtualAddress();
    td.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
    rt_list->BuildRaytracingAccelerationStructure(&td, 0, nullptr);

    rt_list->Close();
    ID3D12CommandList* lists[] = { rt_list };
    rt_queue->ExecuteCommandLists(1, lists);
    rt_wait_gpu();

    rt_rel(scratch);
    rt_rel(inst_buf);
    rt_rel(vb);
    rt_rel(ib);

    const HRESULT removed = rt_dev->GetDeviceRemovedReason();
    if (FAILED(removed))
    {
        Msg("! [RT] D3D12 device removed during BVH build: 0x%08x", removed);
        rt_rel(rt_blas);
        rt_rel(rt_tlas);
        return false;
    }

    Msg("* [RT] BVH built: %u tris, %u verts, BLAS %.1f MB, TLAS %.1f KB, scratch %.1f MB, %u ms",
        nt, nv, double(bl_info.ResultDataMaxSizeInBytes) / (1024.0 * 1024.0),
        double(tl_info.ResultDataMaxSizeInBytes) / 1024.0, double(scratch_size) / (1024.0 * 1024.0),
        u32(timer.GetElapsed_ms()));
    Msg("* [RT] level bounds: (%.1f %.1f %.1f) - (%.1f %.1f %.1f)", bb.vMin.x, bb.vMin.y, bb.vMin.z, bb.vMax.x,
        bb.vMax.y, bb.vMax.z);
    return true;
}
} // namespace rt_impl

using namespace rt_impl;

void Tick(ID3D11Device* device11)
{
    if (!ps_r__rt)
    {
        if (rt_built_model)
            rt_release_as();
        rt_build_failed = false;
        return;
    }
    if (!rt_tried)
    {
        rt_tried = true;
        rt_ok = rt_init(device11);
        if (!rt_ok)
            Msg("! [RT] ray tracing unavailable");
    }
    if (!rt_ok || !g_pGameLevel)
        return;

    const CDB::MODEL* model = g_pGameLevel->ObjectSpace.GetStaticModel();
    const u32 nt = model ? u32(model->get_tris_count()) : 0;
    if (!nt)
        return;
    if (model == rt_built_model && nt == rt_built_tris)
        return;
    model->syncronize(); // collision model may still be building on another thread
    if (rt_build_failed && model == rt_built_model)
        return;

    rt_release_as();
    if (rt_build(*model))
    {
        rt_built_model = model;
        rt_built_tris = nt;
        rt_build_failed = false;
    }
    else
    {
        Msg("! [RT] BVH build failed");
        rt_built_model = model;
        rt_built_tris = nt;
        rt_build_failed = true;
        rt_rel(rt_blas);
        rt_rel(rt_tlas);
    }
}

void OnLevelUnload()
{
    rt_release_as();
    rt_build_failed = false;
}

void Shutdown()
{
    rt_release_as();
    if (rt_event)
    {
        CloseHandle(rt_event);
        rt_event = nullptr;
    }
    rt_rel(rt_fence);
    rt_rel(rt_list);
    rt_rel(rt_alloc);
    rt_rel(rt_queue);
    rt_rel(rt_dev);
    rt_ok = false;
    rt_tried = false;
    rt_fence_value = 0;
    // d3d12.dll stays loaded: the NR module may use it too
}
} // namespace rt
} // namespace xray::render::RENDER_NAMESPACE

#endif
