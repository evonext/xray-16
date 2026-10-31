// RT stage 1 debug: one ray per pixel towards the sun (DXR 1.1 inline ray tracing).
// Compiled at build time by dxc (cs_6_5) into $(IntDir)rtgen/rt_sun_shadow_cs.h, see xrRender_R4.vcxproj.
//
// width/height: size of the depth (render resolution); pixels map 1:1 onto the color target's top-left
// Inputs:  t_z    - view-space depth per pixel (0 = sky, < 0 = HUD), written by D3D11
//          u_out  - copy of the scene color; overwritten with the debug view
//          t_n    - G-buffer normal, view space, octahedral-encoded (0,0 = none)
// Modes:   1 = sun visibility * N.L (grey), 2 = G-buffer normals (world), 3 = scene darkened where RT shadow,
//          4 = normals reconstructed from depth, 5 = sun visibility only (R8 mask for the sun shader: 1 lit, 0 shadow)
//          7 = water reflections, 9 = visibility of up to 4 local lights (RGBA8, one channel per light)
//          11 = path traced indirect light (RGBA8: rgb = sqrt(bounced light / 2), a = 0.01 + 0.99 * sky visibility)

#define RS "RootConstants(num32BitConstants=48, b0), SRV(t0), DescriptorTable(SRV(t1, numDescriptors=2), UAV(u0)), DescriptorTable(SRV(t3, numDescriptors=11)), CBV(b1), StaticSampler(s0, filter=FILTER_MIN_MAG_LINEAR_MIP_POINT, addressU=TEXTURE_ADDRESS_WRAP, addressV=TEXTURE_ADDRESS_WRAP, addressW=TEXTURE_ADDRESS_WRAP)"

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

// mode 9: local lights (point / spot), set by the engine every frame
cbuffer L : register(b1)
{
    float4 l_pos[4]; // xyz: world position, w: range (<= 0: slot unused)
    float4 l_par[4]; // x: radius of the light source (soft shadows)
    float4 cap_a[16]; // dynamic occluders (characters): xyz capsule segment start, w radius (<= 0: unused)
    float4 cap_b[16]; // xyz capsule segment end
    float4 l_col[4]; // rgb: light colour (mode 11)
    float4 pt_a; // x: bounces, y: paths per pixel, z: max path segment length, w: local light strength
    float4 pt_b; // rgb: sun colour, w: sun strength
    float4 pt_c; // rgb: average sky radiance (secondary bounces)
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
// path tracing (mode 11): geometry of the hit points and the average colour of each material
ByteAddressBuffer b_ov : register(t8); // opaque vertices (float3)
ByteAddressBuffer b_oi : register(t9); // opaque indices
ByteAddressBuffer b_om : register(t10); // opaque triangle -> material
ByteAddressBuffer b_av : register(t11); // alpha-tested vertices (float3); its indices / alpha are b_idx
ByteAddressBuffer b_am : register(t12); // alpha-tested triangle -> material
Texture2D<float4> t_alb : register(t13); // average colour per material (n x 1)
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

// ray vs capsule (segment pa-pb, radius ra), nearest positive t or -1. A ray that starts close to the capsule axis
// belongs to the character itself (its own surface): no self shadowing from the approximation.
float cap_hit(float3 ro, float3 rd, float3 pa, float3 pb, float ra)
{
    const float3 ba = pb - pa;
    const float baba = dot(ba, ba);
    const float3 oa = ro - pa;
    const float baoa = dot(ba, oa);
    const float h0 = saturate(baoa / max(baba, 1e-6));
    const float3 dd = oa - ba * h0;
    if (dot(dd, dd) < ra * ra * 1.8)
        return -1.0;
    const float bard = dot(ba, rd);
    const float rdoa = dot(rd, oa);
    const float oaoa = dot(oa, oa);
    const float a = baba - bard * bard;
    float b = baba * rdoa - baoa * bard;
    float c = baba * oaoa - baoa * baoa - ra * ra * baba;
    float h = b * b - a * c;
    if (h >= 0.0 && a > 1e-6)
    {
        const float t = (-b - sqrt(h)) / a;
        const float y = baoa + t * bard;
        if (y > 0.0 && y < baba)
            return t;
        const float3 oc = (y <= 0.0) ? oa : ro - pb;
        b = dot(rd, oc);
        c = dot(oc, oc) - ra * ra;
        h = b * b - c;
        if (h > 0.0)
            return -b - sqrt(h);
    }
    return -1.0;
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
    float res = 1.0;
    hit_t = tmax;
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        hit_t = q.CommittedRayT();
        res = 0.0;
    }
    // characters are not in the BVH: capsules
    [loop]
    for (uint k = 0; k < 16; ++k)
    {
        const float ra = cap_a[k].w;
        if (ra <= 0.0)
            break;
        const float t = cap_hit(origin, dir, cap_a[k].xyz, cap_b[k].xyz, ra);
        if (t > 0.0 && t < hit_t)
        {
            hit_t = t;
            res = 0.0;
        }
    }
    return res;
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
    g += float2(0.97, -0.25) * cos(dot(pos.xz, float2(0.97, -0.25)) * 6.1 + t * 2.3) * 0.3;
    g += float2(-0.2, -0.98) * cos(dot(pos.xz, float2(-0.2, -0.98)) * 9.7 - t * 3.1) * 0.12;
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
        float3 n = water_normal(pw, ex2.w, ex1.w);
        {
            const float2 jn = float2(ign(float2(p), ex0.x), ign(float2(p) + 17.0, ex0.x)) - 0.5;
            n = normalize(n + float3(jn.x, 0.0, jn.y) * 0.03);
        }
        float3 r = reflect(dir, n);
        if (r.y < 0.02)
            r = normalize(float3(r.x, 0.02, r.z)); // never reflect into the water itself
        const float cosv = saturate(dot(-dir, n));
        const float fres = 0.10 + 0.90 * pow(1.0 - cosv, 3.0);
        const float3 refl = trace_refl(pw + n * 0.03, r);
        // soft shoreline: reflection fades out where the water gets shallow along the view ray
        const float depth_w = (c.w > 0.5) ? (tgeo - tw) : 1.0;
        const float edge = smoothstep(0.0, 0.5, depth_w);
        const float k = saturate(fres * ex1.z * 1.3) * edge;
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
    // only surfaces that exist in the BVH: grass, detail objects and dynamic models are not there and
    // their G-buffer normals (edge-on blades) would light up with a bogus sky-coloured Fresnel glow
    {
        float hb;
        if (trace_sun(c.xyz - dir * 0.15, dir, 0.30, hb) > 0.5)
            return;
    }
    const float3 refl = trace_refl(c.xyz + n * 0.03, r);
    u_out[p] = float4(lerp(sc.rgb, refl, saturate(k)), sc.a);
}

// ---- mode 9: ray traced shadows of local lights ------------------------------------------------------
// One ray per pixel and light, aimed at a random point of the light's source disk (soft penumbra, DLSS accumulates
// it). Channel i = 1 where light i reaches the pixel, 0 where level geometry blocks it. Pixels outside the range,
// facing away from the light, sky and HUD stay 1: the light shader has its own falloff and N.L there.
void local_pixel(int2 p, float4 c)
{
    float4 res = float4(1.0, 1.0, 1.0, 1.0);
    if (abs(c.w - 1.0) < 0.01)
    {
        const float3 to_cam = cam_pos - c.xyz;
        const float dist = length(to_cam);
        float3 n = to_cam / max(dist, 1e-4);
        const float2 ne = t_n.Load(int3(p, 0));
        if (dot(ne, ne) > 1e-8)
        {
            const float3 nv = oct_decode(ne);
            n = normalize(nv.x * iv0.xyz + nv.y * iv1.xyz + nv.z * iv2.xyz);
        }
        if (dot(n, to_cam) < 0.0)
            n = -n;
        const float3 org = c.xyz + n * (bias * (1.0 + dist * 0.01));
        [unroll]
        for (uint i = 0; i < 4; ++i)
        {
            const float range = l_pos[i].w;
            if (range <= 0.0)
                continue;
            const float3 to_l = l_pos[i].xyz - org;
            const float d2 = dot(to_l, to_l);
            if (d2 >= range * range)
                continue;
            const float3 ldir = to_l * rsqrt(max(d2, 1e-8));
            if (dot(ldir, n) <= 0.0)
                continue;
            float3 lt, lb;
            make_onb(ldir, lt, lb);
            const float r1 = ign(float2(p) + float(i) * 11.0, ex0.x);
            const float r2 = ign(float2(p) + float(i) * 29.0 + 5.0, ex0.x + 3.0);
            const float rr = sqrt(r1) * l_par[i].x;
            const float th = r2 * 6.2831853;
            const float3 tgt = l_pos[i].xyz + (lt * cos(th) + lb * sin(th)) * rr;
            float3 dv = tgt - org;
            const float len = length(dv);
            dv /= max(len, 1e-4);
            float ht;
            res[i] = trace_sun(org, dv, max(len - 0.1, 0.01), ht);
        }
    }
    u_out[p] = res;
}

// ---- mode 10: denoise of an RGBA8 visibility mask (t7 = raw mask, t1 depth, t2 normals) --------------------
// 16 taps on a golden-angle spiral (rotated per pixel), weights from depth difference and normal agreement.
// pt: path tracing data (mode 12) - a = 0 means "no data" (grass, models, HUD): such pixels are neither filtered
// nor mixed into their neighbours, otherwise grass would get the dark result of the ground under it and vice versa.
void denoise_pixel(int2 p, float4 c, bool pt)
{
    const float4 centre = t_color.Load(int3(p, 0));
    const float z0 = t_z.Load(int3(p, 0));
    if (z0 <= 0.0 || (pt && centre.a <= 0.0)) // sky / HUD / no data
    {
        u_out[p] = centre;
        return;
    }
    const float2 ne0 = t_n.Load(int3(p, 0));
    const bool has_n = dot(ne0, ne0) > 1e-8;
    const float3 n0 = has_n ? oct_decode(ne0) : float3(0, 0, 1);
    const float rot = ign(float2(p), ex0.x) * 6.2831853;
    const float radius = pt ? 8.0 : 4.5;
    float4 acc = centre;
    float wsum = 1.0;
    [unroll]
    for (uint i = 0; i < 16; ++i)
    {
        const float r = radius * sqrt((float(i) + 0.5) / 16.0);
        const float th = float(i) * 2.39996323 + rot;
        const int2 q = clamp(p + int2(round(r * cos(th)), round(r * sin(th))), int2(0, 0),
            int2(int(width) - 1, int(height) - 1));
        const float zq = t_z.Load(int3(q, 0));
        if (zq <= 0.0)
            continue;
        const float4 vq = t_color.Load(int3(q, 0));
        if (pt && vq.a <= 0.0)
            continue;
        const float wz = exp(-abs(zq - z0) / (0.04 * z0 + 0.03));
        float wn = 1.0;
        if (has_n)
        {
            const float2 neq = t_n.Load(int3(q, 0));
            wn = dot(neq, neq) > 1e-8 ? pow(saturate(dot(n0, oct_decode(neq))), 24.0) : 0.0;
        }
        const float w = wz * wn;
        acc += vq * w;
        wsum += w;
    }
    u_out[p] = acc / wsum;
}

// ---- mode 11: path traced indirect light --------------------------------------------------------------
// Cosine-sampled paths from the visible surface. A ray that leaves the scene at once counts as sky visibility (a),
// the light that bounced surfaces send back (sun, nearby lamps, further bounces) is summed into rgb. combine_1.ps
// puts the engine's sky colour on the visibility and adds the bounced light on top of it.
static uint g_rng;

uint pcg_hash(uint v)
{
    const uint st = v * 747796405u + 2891336453u;
    const uint w = ((st >> ((st >> 28u) + 4u)) ^ st) * 277803737u;
    return (w >> 22u) ^ w;
}

float rnd()
{
    g_rng = pcg_hash(g_rng);
    return float(g_rng >> 8) * (1.0 / 16777216.0);
}

float3 cosine_dir(float3 n, float u1, float u2)
{
    float3 t, b;
    make_onb(n, t, b);
    const float r = sqrt(u1);
    const float th = 6.2831853 * u2;
    return normalize(t * (r * cos(th)) + b * (r * sin(th)) + n * sqrt(max(0.0, 1.0 - u1)));
}

// nearest hit: distance, geometric normal (facing the ray), average colour of the material
bool trace_closest(float3 org, float3 dir, float tmax, out float t, out float3 nrm, out float3 alb)
{
    RayDesc ray;
    ray.Origin = org;
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
    }
    t = tmax;
    nrm = float3(0, 1, 0);
    alb = float3(0.4, 0.4, 0.4);
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
        return false;
    t = q.CommittedRayT();
    const uint tri = q.CommittedPrimitiveIndex();
    float3 p0, p1, p2;
    uint mat;
    if (q.CommittedInstanceID() == 1)
    {
        const uint3 ix = b_idx.Load3(tri * 12);
        p0 = asfloat(b_av.Load3(ix.x * 12));
        p1 = asfloat(b_av.Load3(ix.y * 12));
        p2 = asfloat(b_av.Load3(ix.z * 12));
        mat = b_am.Load(tri * 4);
    }
    else
    {
        const uint3 ix = b_oi.Load3(tri * 12);
        p0 = asfloat(b_ov.Load3(ix.x * 12));
        p1 = asfloat(b_ov.Load3(ix.y * 12));
        p2 = asfloat(b_ov.Load3(ix.z * 12));
        mat = b_om.Load(tri * 4);
    }
    float3 gn = cross(p1 - p0, p2 - p0);
    gn = gn / max(length(gn), 1e-12);
    if (dot(gn, dir) > 0.0)
        gn = -gn;
    nrm = gn;
    if (mat != 0xFFFFFFFFu)
        alb = t_alb.Load(int3(mat, 0, 0)).rgb;
    return true;
}

// light that reaches point p (normal n): sun and one random local light, both with a shadow ray; times albedo later
float3 direct_light(float3 p, float3 n)
{
    float3 e = float3(0, 0, 0);
    float ht;
    const float ndl = dot(n, sun_dir);
    if (ndl > 0.0 && pt_b.w > 0.0)
        e += pt_b.rgb * (pt_b.w * ndl * trace_sun(p + n * 0.03, sun_dir, 500.0, ht));
    uint cnt = 0;
    [unroll]
    for (uint i = 0; i < 4; ++i)
        cnt += l_pos[i].w > 0.0 ? 1u : 0u;
    if (cnt > 0u && pt_a.w > 0.0)
    {
        uint pick = min(uint(rnd() * float(cnt)), cnt - 1u);
        uint idx = 0;
        [unroll]
        for (uint j = 0; j < 4; ++j)
        {
            if (l_pos[j].w > 0.0)
            {
                if (pick == 0u)
                    idx = j;
                pick = pick > 0u ? pick - 1u : 0xFFFFFFFFu;
            }
        }
        const float range = l_pos[idx].w;
        const float3 to_l = l_pos[idx].xyz - p;
        const float d2 = dot(to_l, to_l);
        if (d2 < range * range)
        {
            const float d = sqrt(d2);
            const float3 ld = to_l / max(d, 1e-4);
            const float nl = dot(n, ld);
            if (nl > 0.0)
            {
                const float att = saturate(1.0 - d2 / (range * range));
                e += l_col[idx].rgb * (float(cnt) * pt_a.w * nl * att * att *
                    trace_sun(p + n * 0.03, ld, max(d - 0.1, 0.01), ht));
            }
        }
    }
    return e;
}

void pt_pixel(int2 p, float4 c)
{
    if (abs(c.w - 2.0) < 0.01)
    {
        u_out[p] = float4(0, 0, 0, 0); // HUD: no data, combine keeps the baked light
        return;
    }
    if (c.w < 0.5)
    {
        u_out[p] = float4(0, 0, 0, 1);
        return;
    }
    // surfaces that are not in the BVH (grass, models, NPCs): no data
    // the closest hit of a short ray through the pixel must lie on the visible point itself; grass a few cm above
    // the ground used to pass this test and got the (dark) paths that start inside the ground
    const float3 vdir = pixel_dir(p);
    const float dist0 = length(cam_pos - c.xyz);
    const float tol = 0.02 + dist0 * 0.0015;
    float tb;
    float3 nb_, ab_;
    if (!trace_closest(c.xyz - vdir * 0.25, vdir, 0.50, tb, nb_, ab_) || abs(tb - 0.25) > tol)
    {
        u_out[p] = float4(0, 0, 0, 0);
        return;
    }

    const float3 dx = pick_delta(c, load_pos(p + int2(1, 0)), load_pos(p - int2(1, 0)));
    const float3 dy = pick_delta(c, load_pos(p + int2(0, 1)), load_pos(p - int2(0, 1)));
    float3 n_geo = cross(dy, dx);
    const float3 to_cam = cam_pos - c.xyz;
    const float nl0 = length(n_geo);
    n_geo = nl0 < 1e-8 ? normalize(to_cam) : n_geo / nl0;
    if (dot(n_geo, to_cam) < 0)
        n_geo = -n_geo;
    float3 n = n_geo;
    const float2 ne = t_n.Load(int3(p, 0));
    if (dot(ne, ne) > 1e-8)
    {
        const float3 nv = oct_decode(ne);
        n = normalize(nv.x * iv0.xyz + nv.y * iv1.xyz + nv.z * iv2.xyz);
        if (dot(n, to_cam) < 0.0)
            n = -n;
    }
    const float dist = length(to_cam);
    const float3 org = c.xyz + n_geo * (bias * (1.0 + dist * 0.01) + 0.02);

    g_rng = pcg_hash(uint(p.x) + uint(p.y) * 7919u + uint(ex0.x) * 104729u);
    const uint spp = max(1u, uint(pt_a.y));
    const uint nb = clamp(uint(pt_a.x), 1u, 4u);
    float3 bounced = float3(0, 0, 0);
    float vis_sky = 0.0;
    for (uint sidx = 0; sidx < spp; ++sidx)
    {
        float3 o = org;
        float3 dir = cosine_dir(n, rnd(), rnd());
        if (dot(dir, n_geo) < 0.05)
            dir = normalize(dir + n_geo * 0.6);
        float3 thr = float3(1, 1, 1);
        [loop]
        for (uint b = 0; b < nb; ++b)
        {
            float t;
            float3 hn, ha;
            if (!trace_closest(o, dir, pt_a.z, t, hn, ha))
            {
                if (b == 0u)
                    vis_sky += 1.0;
                else
                    bounced += thr * pt_c.rgb; // sky seen from a bounce point
                break;
            }
            const float3 hp = o + dir * t;
            bounced += thr * ha * direct_light(hp, hn);
            thr *= ha;
            if (b + 1u == nb)
            {
                bounced += thr * pt_c.rgb * 0.35; // path cut: the sky half of the hemisphere is assumed
                break;
            }
            o = hp + hn * 0.03;
            dir = cosine_dir(hn, rnd(), rnd());
        }
    }
    const float inv = 1.0 / float(spp);
    u_out[p] = float4(sqrt(saturate(bounced * inv * 0.5)), 0.01 + 0.99 * vis_sky * inv);
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
    if (mode == 9)
    {
        local_pixel(p, c);
        return;
    }
    if (mode == 10 || mode == 12)
    {
        denoise_pixel(p, c, mode == 12);
        return;
    }
    if (mode == 11)
    {
        pt_pixel(p, c);
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
