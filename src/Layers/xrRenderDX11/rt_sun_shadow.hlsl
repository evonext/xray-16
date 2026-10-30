// RT stage 1 debug: one ray per pixel towards the sun (DXR 1.1 inline ray tracing).
// Compiled at build time by dxc (cs_6_5) into $(IntDir)rtgen/rt_sun_shadow_cs.h, see xrRender_R4.vcxproj.
//
// width/height: size of the depth (render resolution); pixels map 1:1 onto the color target's top-left
// Inputs:  t_z    - view-space depth per pixel (0 = sky, < 0 = HUD), written by D3D11
//          u_out  - copy of the scene color; overwritten with the debug view
//          t_n    - G-buffer normal, view space, octahedral-encoded (0,0 = none)
// Modes:   1 = sun visibility * N.L (grey), 2 = G-buffer normals (world), 3 = scene darkened where RT shadow,
//          4 = normals reconstructed from depth, 5 = sun visibility only (R8 mask for the sun shader: 1 lit, 0 shadow)

#define RS "RootConstants(num32BitConstants=48, b0), SRV(t0), DescriptorTable(SRV(t1, numDescriptors=2), UAV(u0)), DescriptorTable(SRV(t3, numDescriptors=5)), StaticSampler(s0, filter=FILTER_MIN_MAG_LINEAR_MIP_POINT, addressU=TEXTURE_ADDRESS_WRAP, addressV=TEXTURE_ADDRESS_WRAP, addressW=TEXTURE_ADDRESS_WRAP)"

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
    float4 ex0; // x: frame, y: sun disk tan(half-angle), z: sun rays, w: AO rays
    float4 ex1; // x: AO radius (m), y: flags (bit0: sun, bit1: AO), z: reflection strength, w: time (s)
    float4 ex2; // rgb: fallback sky colour, w: water ripple amplitude
    float4 ex3; // rgb: sun colour, w: glossy surface reflection strength
};

RaytracingAccelerationStructure scene : register(t0);
Texture2D<float> t_z : register(t1);
Texture2D<float2> t_n : register(t2);
// alpha-tested geometry (second BLAS, non-opaque): UVs, indices and texture slice per triangle, alpha masks
Texture2DArray<float> t_alpha : register(t3);
ByteAddressBuffer b_uv : register(t4);
ByteAddressBuffer b_idx : register(t5);
ByteAddressBuffer b_slice : register(t6);
Texture2D<float4> t_color : register(t7); // copy of the scene colour (mode 7)
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
float trace_sun(float3 origin, float3 dir, float tmax, out float hit_t)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = dir;
    ray.TMin = 0.0;
    ray.TMax = tmax;
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0x01, ray);
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
    hit_t = tmax;
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        hit_t = q.CommittedRayT();
        return 0.0;
    }
    return 1.0;
}

float ign(float2 p, float frame)
{
    p += 5.588238 * frame;
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

void make_onb(float3 n, out float3 t, out float3 b)
{
    const float sgn = n.z >= 0.0 ? 1.0 : -1.0;
    const float a = -1.0 / (sgn + n.z);
    const float k = n.x * n.y * a;
    t = float3(1.0 + sgn * n.x * n.x * a, sgn * k, -sgn * n.x);
    b = float3(k, sgn + n.y * n.y * a, -n.y);
}

// ---- mode 7: ray traced reflections on water -------------------------------------------------
// Water surfaces live in their own BLAS (ray mask 2). A camera ray that hits the water in front of the depth buffer
// marks a water pixel; the reflected ray is traced against the level (mask 1) and the colour at the hit point is
// taken from the already rendered frame (screen space), with the fogged sky as a fallback.

float3 pixel_dir(int2 p)
{
    const float2 uv = (float2(p) + 0.5) / float2(width, height);
    const float2 ndc = uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
    const float3 v = float3((ndc.x - proj.z) / proj.x, (ndc.y - proj.w) / proj.y, 1.0);
    return normalize(v.x * iv0.xyz + v.y * iv1.xyz + v.z * iv2.xyz);
}

// world position -> pixel coordinates and view depth; false when behind the camera
bool project_px(float3 w, out float2 px, out float vz)
{
    const float3 d = w - iv3.xyz;
    const float3 v = float3(dot(d, iv0.xyz), dot(d, iv1.xyz), dot(d, iv2.xyz));
    vz = v.z;
    px = float2(0, 0);
    if (v.z < 0.05)
        return false;
    const float2 ndc = float2(v.x * proj.x / v.z + proj.z, v.y * proj.y / v.z + proj.w);
    px = (ndc * float2(0.5, -0.5) + 0.5) * float2(width, height);
    return true;
}

float3 water_normal(float3 pos, float amp, float t)
{
    // cheap ripples: a few travelling sine waves
    float2 g = float2(0, 0);
    g += float2(0.8, 0.6) * cos(dot(pos.xz, float2(0.8, 0.6)) * 2.3 + t * 1.1);
    g += float2(-0.5, 0.86) * cos(dot(pos.xz, float2(-0.5, 0.86)) * 3.7 - t * 1.7) * 0.7;
    g += float2(0.97, -0.25) * cos(dot(pos.xz, float2(0.97, -0.25)) * 6.1 + t * 2.3) * 0.5;
    g += float2(-0.2, -0.98) * cos(dot(pos.xz, float2(-0.2, -0.98)) * 9.7 - t * 3.1) * 0.3;
    return normalize(float3(-g.x * amp, 1.0, -g.y * amp));
}

float4 scene_at(int2 q)
{
    q = clamp(q, int2(0, 0), int2(int(width) - 1, int(height) - 1));
    return t_color.Load(int3(q, 0));
}

// colour seen along r from org: screen-space lookup of the RT hit point, fogged sky as fallback
float3 trace_refl(float3 org, float3 r)
{
    float ht;
    float3 refl = ex2.rgb; // fallback: fogged sky colour
    const float lit = trace_sun(org, r, 400.0, ht);
    const float3 target = org + r * (lit > 0.5 ? 4000.0 : ht);
    float2 px;
    float vz;
    bool got = false;
    if (project_px(target, px, vz))
    {
        const int2 q = int2(px);
        if (q.x >= 0 && q.y >= 0 && q.x < int(width) && q.y < int(height))
        {
            const float zb = t_z.Load(int3(q, 0));
            // a ray hit must land on the visible surface; a miss must land on the sky
            const bool ok = lit > 0.5 ? (zb == 0.0) : (zb > 0.0 && abs(zb - vz) < 0.6 + 0.03 * vz);
            if (ok)
            {
                refl = t_color.Load(int3(q, 0)).rgb;
                got = true;
            }
        }
    }
    if (!got && lit < 0.5)
        refl = lerp(ex2.rgb, float3(0.12, 0.12, 0.11), 0.6); // hit somewhere off screen: dark ground tint
    // sun glint where the reflected ray leaves the scene towards the sun
    if (lit > 0.5)
        refl += ex3.rgb * pow(saturate(dot(r, sun_dir)), 1500.0) * 8.0;
    return refl;
}

void reflect_pixel(int2 p, float4 c)
{
    const float4 sc = scene_at(p);
    u_out[p] = sc;
    if (abs(c.w - 2.0) < 0.01)
        return; // HUD
    const float3 dir = pixel_dir(p);
    const float tgeo = c.w > 0.5 ? length(c.xyz - cam_pos) : 100000.0;

    // water in front of the visible surface?
    RayDesc wr;
    wr.Origin = cam_pos;
    wr.Direction = dir;
    wr.TMin = 0.01;
    wr.TMax = tgeo + 0.05;
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_CULL_NON_OPAQUE> wq;
    wq.TraceRayInline(scene, RAY_FLAG_NONE, 0x02, wr);
    while (wq.Proceed())
    {
    }
    if (wq.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        const float tw = wq.CommittedRayT();
        const float3 pw = cam_pos + dir * tw;
        const float3 n = water_normal(pw, ex2.w, ex1.w);
        float3 r = reflect(dir, n);
        if (r.y < 0.02)
            r = normalize(float3(r.x, 0.02, r.z)); // never reflect into the water itself
        const float cosv = saturate(dot(-dir, n));
        const float fres = 0.10 + 0.90 * pow(1.0 - cosv, 3.0);
        const float3 refl = trace_refl(pw + n * 0.03, r);
        const float k = saturate(fres * ex1.z * 1.3);
        u_out[p] = float4(lerp(sc.rgb, refl, k), sc.a);
        return;
    }

    // glossy / wet ground: Fresnel sheen on solid surfaces (stronger in rain)
    const float surf = ex3.w;
    if (surf <= 0.001 || c.w < 0.5 || tgeo > 90.0)
        return;
    float3 n = -dir;
    const float2 ne = t_n.Load(int3(p, 0));
    if (dot(ne, ne) > 1e-8)
    {
        const float3 nv = oct_decode(ne);
        n = normalize(nv.x * iv0.xyz + nv.y * iv1.xyz + nv.z * iv2.xyz);
    }
    if (dot(n, -dir) < 0.0)
        n = -n;
    const float cosv = saturate(dot(-dir, n));
    const float fres = 0.04 + 0.96 * pow(1.0 - cosv, 5.0);
    const float luma = dot(sc.rgb, float3(0.3, 0.59, 0.11));
    // dark surfaces (wet, polished) show reflections, bright diffuse ones don't
    float k = fres * surf * saturate(1.3 - luma * 2.5) * saturate(1.5 - tgeo / 60.0);
    k *= saturate(n.y * 1.5 + 0.2); // mostly ground-like surfaces
    if (k < 0.01)
        return;
    const float3 r = reflect(dir, n);
    if (r.y < 0.0)
        return;
    const float3 refl = trace_refl(c.xyz + n * 0.03, r);
    u_out[p] = float4(lerp(sc.rgb, refl, saturate(k)), sc.a);
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
    if (mode == 7)
    {
        reflect_pixel(p, c);
        return;
    }
    float4 scene_col = float4(0, 0, 0, 1);
    if (mode != 5)
        scene_col = u_out[p];

    if (abs(c.w - 2.0) < 0.01)
    {
        if (mode == 5)
            u_out[p] = float4(1, 1, 0, 0);
        return; // HUD: keep as is
    }
    if (c.w < 0.5)
    {
        if (mode == 5)
        {
            u_out[p] = float4(1, 1, 0, 0);
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
    float ao = 1.0;
    const uint flags = uint(ex1.y);
    const float dist = length(to_cam);
    // Offset grows with distance: reconstructed positions lose precision far away
    const float3 org = c.xyz + n_geo * (bias * (1.0 + dist * 0.01));
    const float rnd = ign(float2(p), ex0.x);
    float ht;
    if (mode == 5)
    {
        // soft sun: rays spread over the sun disk (contact-hardening penumbra comes for free)
        lit = 1.0;
        if (flags & 1u)
        {
            const uint ns = max(1u, uint(ex0.z));
            float3 st, sb;
            make_onb(sun_dir, st, sb);
            float acc = 0.0;
            for (uint i = 0; i < ns; ++i)
            {
                const float r = sqrt((float(i) + 0.5) / float(ns));
                const float th = float(i) * 2.39996323 + rnd * 6.2831853;
                const float3 d = normalize(sun_dir + (st * cos(th) + sb * sin(th)) * (r * ex0.y));
                acc += trace_sun(org + d * 0.01, d, max_dist, ht);
            }
            lit = acc / float(ns);
        }
        if ((flags & 2u) && dist < 40.0) // AO only near the camera: far ambient is baked anyway
        {
            // AO: cosine-weighted hemisphere around the smooth normal, hits weighted by proximity
            const uint na = max(1u, uint(ex0.w));
            float3 at, ab;
            make_onb(n, at, ab);
            float occ = 0.0;
            for (uint j = 0; j < na; ++j)
            {
                const float u1 = (float(j) + frac(rnd * 7.31 + float(j) * 0.618034)) / float(na);
                const float u2 = frac(rnd + float(j) * 0.754877666);
                const float r = sqrt(u1);
                const float th = 6.2831853 * u2;
                const float3 d = at * (r * cos(th)) + ab * (r * sin(th)) + n * sqrt(max(0.0, 1.0 - u1));
                const float vis = trace_sun(org + n * 0.02, d, ex1.x, ht);
                occ += (1.0 - vis) * saturate(1.0 - ht / ex1.x);
            }
            ao = 1.0 - occ / float(na);
        }
        u_out[p] = float4(lit, ao, 0.0, 0.0);
        return;
    }
    if (ndl > 0.0)
        lit = trace_sun(org + sun_dir * 0.01, sun_dir, max_dist, ht);

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
