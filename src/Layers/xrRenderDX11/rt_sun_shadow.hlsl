// RT stage 1 debug: one ray per pixel towards the sun (DXR 1.1 inline ray tracing).
// Compiled at build time by dxc (cs_6_5) into $(IntDir)rtgen/rt_sun_shadow_cs.h, see xrRender_R4.vcxproj.
//
// width/height: size of the depth (render resolution); pixels map 1:1 onto the color target's top-left
// Inputs:  t_z    - view-space depth per pixel (0 = sky, < 0 = HUD), written by D3D11
//          u_out  - copy of the scene color; overwritten with the debug view
//          t_n    - G-buffer normal, view space, octahedral-encoded (0,0 = none)
// Modes:   1 = sun visibility * N.L (grey), 2 = G-buffer normals (world), 3 = scene darkened where RT shadow,
//          4 = normals reconstructed from depth, 5 = sun visibility only (R8 mask for the sun shader: 1 lit, 0 shadow)

#define RS "RootConstants(num32BitConstants=32, b0), SRV(t0), DescriptorTable(SRV(t1, numDescriptors=2), UAV(u0)), DescriptorTable(SRV(t3, numDescriptors=4)), StaticSampler(s0, filter=FILTER_MIN_MAG_LINEAR_MIP_POINT, addressU=TEXTURE_ADDRESS_WRAP, addressV=TEXTURE_ADDRESS_WRAP, addressW=TEXTURE_ADDRESS_WRAP)"

cbuffer C : register(b0)
{
    float3 sun_dir; // towards the sun, normalized
    uint mode;
    float3 cam_pos;
    float bias;
    uint width;
    uint height;
    float max_dist;
    float alpha_thr; // > 0: alpha-tested geometry (foliage) is traced; hits with a lower texture alpha are skipped
    float4 iv0; // inverse view, rows (row-vector convention), xyz used
    float4 iv1;
    float4 iv2;
    float4 iv3;
    float4 proj; // P11, P22, P31, P32 (as rendered)
};

RaytracingAccelerationStructure scene : register(t0);
Texture2D<float> t_z : register(t1);
Texture2D<float2> t_n : register(t2);
// alpha-tested geometry (second BLAS, non-opaque): UVs, indices and texture slice per triangle, alpha masks
Texture2DArray<float> t_alpha : register(t3);
ByteAddressBuffer b_uv : register(t4);
ByteAddressBuffer b_idx : register(t5);
ByteAddressBuffer b_slice : register(t6);
SamplerState smp : register(s0);

float3 oct_decode(float2 e)
{
    float3 v = float3(e.xy, 1.0 - abs(e.x) - abs(e.y));
    if (v.z < 0.0)
        v.xy = (1.0 - abs(v.yx)) * float2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
    return normalize(v);
}
RWTexture2D<float4> u_out : register(u0);

// World position + class (w: 1 = geometry, 0 = sky, 2 = HUD)
float4 load_pos(int2 p)
{
    p = clamp(p, int2(0, 0), int2(int(width) - 1, int(height) - 1));
    const float z = t_z.Load(int3(p, 0));
    if (z < 0.0)
        return float4(0, 0, 0, 2);
    if (z == 0.0)
        return float4(0, 0, 0, 0);
    const float2 uv = (float2(p) + 0.5) / float2(width, height);
    const float2 ndc = uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
    const float3 v = float3((ndc.x - proj.z) * z / proj.x, (ndc.y - proj.w) * z / proj.y, z);
    const float3 w = v.x * iv0.xyz + v.y * iv1.xyz + v.z * iv2.xyz + iv3.xyz;
    return float4(w, 1.0);
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

// 1 = the sun is visible from origin along dir, 0 = blocked (opaque hit, or an alpha-tested hit that passes the test)
float trace_sun(float3 origin, float3 dir, float tmax)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = dir;
    ray.TMin = 0.0;
    ray.TMax = tmax;
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
    while (q.Proceed())
    {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
        {
            bool hit = true;
            if (alpha_thr > 0.0)
            {
                const uint tri = q.CandidatePrimitiveIndex();
                const uint slice = b_slice.Load(tri * 4);
                if (slice != 0xFFFFFFFFu)
                {
                    const uint3 ix = b_idx.Load3(tri * 12);
                    const float2 uv0 = asfloat(b_uv.Load2(ix.x * 8));
                    const float2 uv1 = asfloat(b_uv.Load2(ix.y * 8));
                    const float2 uv2 = asfloat(b_uv.Load2(ix.z * 8));
                    const float2 bc = q.CandidateTriangleBarycentrics();
                    const float2 uv = uv0 * (1.0 - bc.x - bc.y) + uv1 * bc.x + uv2 * bc.y;
                    hit = t_alpha.SampleLevel(smp, float3(uv, float(slice)), 0) >= alpha_thr;
                }
            }
            if (hit)
                q.CommitNonOpaqueTriangleHit();
        }
        if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
        {
            q.Abort();
            break;
        }
    }
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0 : 1.0;
}

[RootSignature(RS)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint ow, oh;
    u_out.GetDimensions(ow, oh);
    if (id.x >= width || id.y >= height || id.x >= ow || id.y >= oh)
        return;
    const int2 p = int2(id.xy);
    const float4 c = load_pos(p);
    float4 scene_col = float4(0, 0, 0, 1);
    if (mode != 5)
        scene_col = u_out[p];

    if (abs(c.w - 2.0) < 0.01)
    {
        if (mode == 5)
            u_out[p] = float4(1, 1, 1, 1);
        return; // HUD: keep as is
    }
    if (c.w < 0.5)
    {
        if (mode == 5)
        {
            u_out[p] = float4(1, 1, 1, 1);
            return;
        }
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
    const float3 n_geo = n;

    // G-buffer normal (view space -> world) when present: smooth on terrain, sane on foliage
    const float2 ne = t_n.Load(int3(p, 0));
    if (dot(ne, ne) > 1e-8)
    {
        const float3 nv = oct_decode(ne);
        n = normalize(nv.x * iv0.xyz + nv.y * iv1.xyz + nv.z * iv2.xyz);
    }

    if (mode == 2 || mode == 4)
    {
        const float3 shown = mode == 2 ? n : n_geo;
        u_out[p] = float4(shown * 0.5 + 0.5, 1.0);
        return;
    }

    const float ndl = dot(n, sun_dir);
    float lit = 0.0;
    // mode 5 traces every pixel: the sun shader applies N.L / translucency itself
    if (ndl > 0.0 || mode == 5)
    {
        // Offset grows with distance: reconstructed positions lose precision far away
        const float dist = length(to_cam);
        // offset along the geometric normal: it matches the depth surface the position came from
        lit = trace_sun(c.xyz + n_geo * (bias * (1.0 + dist * 0.01)) + sun_dir * 0.01, sun_dir, max_dist);
    }

    if (mode == 5)
    {
        u_out[p] = float4(lit, lit, lit, lit);
        return;
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
