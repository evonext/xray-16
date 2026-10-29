// RT stage 1 debug: one ray per pixel towards the sun (DXR 1.1 inline ray tracing).
// Compiled at build time by dxc (cs_6_5) into $(IntDir)rtgen/rt_sun_shadow_cs.h, see xrRender_R4.vcxproj.
//
// Inputs:  t_pos  - world position per pixel (w: 1 = geometry, 0 = sky, 2 = HUD), written by D3D11
//          u_out  - copy of the scene color; overwritten with the debug view
// Modes:   1 = sun visibility * N.L (grey), 2 = reconstructed normals, 3 = scene darkened where RT shadow

#define RS "RootConstants(num32BitConstants=12, b0), SRV(t0), DescriptorTable(SRV(t1), UAV(u0))"

cbuffer C : register(b0)
{
    float3 sun_dir; // towards the sun, normalized
    uint mode;
    float3 cam_pos;
    float bias;
    uint width;
    uint height;
    float max_dist;
    float pad;
};

RaytracingAccelerationStructure scene : register(t0);
Texture2D<float4> t_pos : register(t1);
RWTexture2D<float4> u_out : register(u0);

float4 load_pos(int2 p)
{
    p = clamp(p, int2(0, 0), int2(int(width) - 1, int(height) - 1));
    return t_pos.Load(int3(p, 0));
}

// Pick the neighbour difference with the smaller depth step (avoids smearing normals across edges)
float3 pick_delta(float4 c, float4 a, float4 b)
{
    const bool va = abs(a.w - 1.0) < 0.01;
    const bool vb = abs(b.w - 1.0) < 0.01;
    const float3 da = a.xyz - c.xyz;
    const float3 db = c.xyz - b.xyz;
    if (va && vb)
        return dot(da, da) < dot(db, db) ? da : db;
    if (va)
        return da;
    if (vb)
        return db;
    return float3(0, 0, 0);
}

[RootSignature(RS)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height)
        return;
    const int2 p = int2(id.xy);
    const float4 c = t_pos.Load(int3(p, 0));
    const float4 scene_col = u_out[p];

    if (abs(c.w - 2.0) < 0.01)
        return; // HUD: keep as is
    if (c.w < 0.5)
    {
        if (mode == 1)
            u_out[p] = float4(0.35, 0.5, 0.75, 1.0); // sky
        else if (mode == 2)
            u_out[p] = float4(0, 0, 0, 1);
        return;
    }

    const float3 dx = pick_delta(c, load_pos(p + int2(1, 0)), load_pos(p - int2(1, 0)));
    const float3 dy = pick_delta(c, load_pos(p + int2(0, 1)), load_pos(p - int2(0, 1)));
    float3 n = cross(dy, dx);
    const float nl = length(n);
    const float3 to_cam = cam_pos - c.xyz;
    if (nl < 1e-8)
        n = normalize(to_cam);
    else
        n /= nl;
    if (dot(n, to_cam) < 0)
        n = -n;

    if (mode == 2)
    {
        u_out[p] = float4(n * 0.5 + 0.5, 1.0);
        return;
    }

    const float ndl = dot(n, sun_dir);
    float lit = 0.0;
    if (ndl > 0.0)
    {
        // Offset grows with distance: reconstructed positions lose precision far away
        const float dist = length(to_cam);
        RayDesc ray;
        ray.Origin = c.xyz + n * (bias * (1.0 + dist * 0.01)) + sun_dir * 0.01;
        ray.Direction = sun_dir;
        ray.TMin = 0.0;
        ray.TMax = max_dist;
        RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
        q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
        q.Proceed();
        lit = (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? 0.0 : 1.0;
    }

    if (mode == 1)
    {
        const float v = 0.08 + 0.92 * lit * saturate(ndl);
        u_out[p] = float4(v, v, v, 1.0);
    }
    else
    {
        const float k = lit > 0.5 ? 1.0 : 0.3;
        u_out[p] = float4(scene_col.rgb * float3(k, k, k * 1.15), scene_col.a);
    }
}
