#pragma once

#if defined(XR_DLSS)

// Ray tracing (DXR 1.1) on a private D3D12 device that shares the adapter with the engine's D3D11 device.
// Stage 1: acceleration structures for the level's static geometry (collision model, CForm).
//
// Console:
//   r__rt        0/1     build the level BVH (released when switched off)
//   r__rt_debug  0..3    debug view: 1 sun visibility * N.L, 2 normals, 3 scene darkened by RT shadow
//   r__rt_bias   0..1    normal offset of shadow ray origins, meters

namespace xray::render::RENDER_NAMESPACE
{
namespace rt
{
// Once per frame (before post-process). Builds / rebuilds the BVH when the level geometry changes.
void Tick(ID3D11Device* device11);
// Level unloaded: drop the acceleration structures (the device stays).
void OnLevelUnload();
// r__rt_debug: replaces the scene color with the debug view. Returns true when it did.
bool DebugPass(ID3D11Device* device11, ID3D11DeviceContext1* ctx1, ID3D11Texture2D* color, ID3D11Texture2D* depth);
void Shutdown();
} // namespace rt
} // namespace xray::render::RENDER_NAMESPACE

#endif
