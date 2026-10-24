#pragma once

// NVIDIA DLSS (NGX, D3D11) integration for the R4 renderer.
// Phase 1: DLAA (native resolution anti-aliasing) with camera motion vectors
// reconstructed from the depth buffer.
//
// Console:
//   r__dlss            0/1      enable DLSS (DLAA)
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
    ID3D11Texture2D* color; // scene color, read and overwritten (in-place result)
    ID3D11Texture2D* depth; // R24G8_TYPELESS hardware depth
    u32 width, height;
    Fmatrix view; // current view
    Fmatrix projUnjittered; // current projection without jitter
    Fvector2 jitterPx; // jitter used this frame, pixels
    Fvector2 jitterNdc; // same, in NDC (as written into the projection)
};

// Returns true if DLSS is enabled in console AND supported by GPU/driver.
bool Enabled(ID3D11Device* device);

// Runs DLSS on inputs.color in place. Returns false if nothing was done.
bool Evaluate(ID3D11Device* device, ID3D11DeviceContext1* ctx1, const FrameInputs& in);

// Computes jitter for the next frame (pixels and NDC). Zero if disabled.
void NextJitter(u32 width, u32 height, Fvector2& outPx, Fvector2& outNdc);

void Shutdown(ID3D11Device* device);
} // namespace dlss
} // namespace xray::render::RENDER_NAMESPACE
