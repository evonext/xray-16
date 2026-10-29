#pragma once

#if defined(XR_DLSS)

// Ray tracing (DXR 1.1) on a private D3D12 device that shares the adapter with the engine's D3D11 device.
// Stage 1: acceleration structures for the level's static geometry (collision model, CForm).
//
// Console:
//   r__rt  0/1   build the level BVH (released when switched off)

namespace xray::render::RENDER_NAMESPACE
{
namespace rt
{
// Once per frame (before post-process). Builds / rebuilds the BVH when the level geometry changes.
void Tick(ID3D11Device* device11);
// Level unloaded: drop the acceleration structures (the device stays).
void OnLevelUnload();
void Shutdown();
} // namespace rt
} // namespace xray::render::RENDER_NAMESPACE

#endif
