#pragma once

#include <functional>

// NVIDIA DLSS (NGX, D3D11) integration for the R4 renderer.
// DLAA (native resolution) and DLSS Super Resolution (scene rendered at lower resolution,
// upscaled before post-process and UI). Camera motion vectors are reconstructed from depth.
//
// Console:
//   r__dlss            0/1      enable DLSS
//   r__dlss_mode       0..4     0 = DLAA, 1 = Quality, 2 = Balanced, 3 = Performance, 4 = Ultra Performance
//                               (changing mode requires vid_restart)
//   r__dlss_preset     0..15    0 = driver default, 10 = J, 11 = K (transformer)
//   r__dlss_jitter     0/1      projection jitter (keep 1, 0 only for debugging)
//   r__dlss_jsign      0..3     bit0 flips X jitter sign, bit1 flips Y (debug)

namespace xray::render::RENDER_NAMESPACE
{
namespace dlss
{
struct FrameInputs
{
    ID3D11DeviceContext* ctx;
    ID3D11Texture2D* color; // scene color (render resolution)
    ID3D11Texture2D* depth; // R24G8_TYPELESS hardware depth (render resolution)
    ID3D11Texture2D* output; // upscaled result (output resolution); nullptr = in place into color (DLAA)
    u32 width, height; // render resolution
    u32 outWidth, outHeight; // output resolution
    Fmatrix view; // current view
    Fmatrix projUnjittered; // current projection without jitter
    Fvector2 jitterPx; // jitter used this frame, pixels
    Fvector2 jitterNdc; // same, in NDC (as written into the projection)
};

// Returns true if DLSS is enabled in console AND supported by GPU/driver.
bool Enabled(ID3D11Device* device);

// Runs DLSS on inputs.color in place. Returns false if nothing was done.
// beforeUpscale: optional callback run after the guides (motion vectors, depth) are built and
// before DLSS itself, with the engine's context state active (used for DLSS-NR at render resolution).
bool Evaluate(ID3D11Device* device, ID3D11DeviceContext1* ctx1, const FrameInputs& in,
    const std::function<void()>& beforeUpscale = {});

// Computes jitter for the next frame (pixels and NDC). Zero if disabled.
// width/height = render resolution, outWidth = output width (affects the number of jitter phases).
void NextJitter(u32 width, u32 height, u32 outWidth, Fvector2& outPx, Fvector2& outNdc);

// Render resolution for the current r__dlss_mode. Returns false when no upscaling is needed
// (DLSS off/unavailable or DLAA mode): then rw/rh = outW/outH.
bool GetRenderSize(ID3D11Device* device, u32 outW, u32 outH, u32& rw, u32& rh);

// Temporarily makes Device.dwWidth/dwHeight report the render resolution, so the whole
// scene pipeline (viewports, screen_res constants, effects) works at the lower resolution.
void BeginRenderRes(u32 rw, u32 rh);
void EndRenderRes();
bool InRenderRes();

void Shutdown(ID3D11Device* device);

// Guides produced by the last Evaluate (render resolution): motion vectors (R16G16_FLOAT, pixels) and depth (R32_FLOAT).
ID3D11Texture2D* LastMotionVectors();
ID3D11Texture2D* LastDepth();
bool LastWasReset();

// ---- DLSS-NR ("DLSS 5" neural rendering, NGX feature 18) ----
// NGX ships DLSS-NR for D3D12/Vulkan only, so it runs on a private D3D12 device that shares
// textures and a fence with the engine's D3D11 device. Needs nvngx_dlssnr.dll next to the exe.
// Console: r__dlss_nr 0/1, r__dlss_nr_preset, r__dlss_nr_intensity, r__dlss_nr_global_tone,
// r__dlss_nr_local_tone, r__dlss_nr_local_structure, r__dlss_nr_skin_structure, r__dlss_nr_style,
// r__dlss_nr_automask, r__dlss_nr_ui_correction
//
// Runs on the full-resolution (already anti-aliased / upscaled) image in place.
bool EvaluateNR(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11Texture2D* color, u32 renderW, u32 renderH);
void ShutdownNR();
} // namespace dlss
} // namespace xray::render::RENDER_NAMESPACE
