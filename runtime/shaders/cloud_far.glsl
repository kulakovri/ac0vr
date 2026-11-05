/* cloud_far.glsl - S2 far renderer: the march into the direction map, shared by cloud_far.comp and the cloudbench
   reference view. Include after cloud_common.glsl. Set 1 is the far module's own (ps2_clouds_far.c). */
#ifndef CLOUD_FAR_GLSL
#define CLOUD_FAR_GLSL

/* Colour and extinction come from the shared core in cloud_common.glsl (cl_shade_s, cl_above, cl_cast_t,
   cl_extinction: the game's impostor model plus the cloud shadow map's cast shadow, identical in the near field, so the
   600 m hand-over has no seam). No multiple scattering, no powder, no silver lining. */
layout(set = 1, binding = 0, std140) uniform ClFarParams {
    vec4 mz;     /* x jitter amount (0 = none), y noise prefilter: x the step (0 = off), z the "above" probe every N fine
                    steps (reused in between), w 1 = half rate: tiles below the hard limit (2x the error) are marched
                    on even frames only, the must list every frame */
    vec4 spl;    /* cloud[] splat (S1: radius_m / height_m = the body's horizontal / vertical semi-axes; full to 0.45,
                    gone at 0.9 of them): x half-height / R when a cloud has no height_m, y extent below the centre
                    relative to above (< 1: flatter base), z full-body fraction (0.45), w cut-off fraction (0.9) */
    vec4 haze;   /* x thin-medium steps: in faint cloud step up to x times the step of stp.w optical depth (at most the
                    empty-space step; 0 = off), y opacity fade start (m), z fade end (m), w detail range (m) */
    vec4 stp;    /* x empty-space step factor, y max step (m), z min transmittance (early out), w max optical depth
                    per step */
    vec4 sch;    /* x error factor for tiles outside the view, y refresh-period factor outside the view,
                    z view margin (rad, head motion before the next refresh), w allowed error in 8-bit levels (the
                    contrast-aware limit: a smooth tile may move further than the pixel threshold) */
    vec4 fine;   /* x step inside cloud relative to the empty-space base step (< 1: denser sampling where it shows),
                    y splat: vertical optical depth through a cloud's centre (the density is scaled per cloud by its
                    height; the game's clouds are semi-transparent veils; 0 = full density), z splat: the envelope's
                    billows (0 = smooth ellipsoids; else a core 1 - 0.8 z wide plus w dome-topped puffs) */
    vec4 win;    /* xy: centre of the weather window relative to the centre eye (cloud space x, z; m); z: 1 = nothing
                    beyond the window (the cloud-list path: the game shows clouds to ~12 km), 0 = the procedural field
                    continues; w: weather texels per metre (512 / 20000) */
    vec4 ext;    /* x core fill for cumulus-like weather (C1 type >= 0.6, blended from 0.2): the splat's coverage R is
                    scaled by it, so the noise remap cuts holes inside the bodies (0 or 1 = off); y the radius of the
                    density's softening across the borders between clouds (PASS_WSOFT; weather texels, < 0.5 = off);
                    z 1 = the opacity-weighted splat (ACZ_VR_CLOUD_SPLAT_MIX; 0 = the strongest profile's cloud and
                    the union of all columns, as before); w 1 = a tile cap applies only to the tiles outside the view
                    (the governor's level 1 with ACZ_VR_GOV_FAR_MODE=1) */
    vec4 dbg;    /* x 1 = ACZ_DEBUG_CLOUD_FAR_STALE (PASS_TILES: each visible tile left alone is checked against a march
                    of its centre texel now); y-w spare */
} fp;

struct ClFarP { vec3 ro; float tstart; float tmax; int nmax; float dtmin; float k; };
/* prof (ACZ_VR_CLOUD_FAR_SPLIT, cl_split 2026-10-30): x = the first distance with cloud (= first), y / z / w = where the
   accumulated transmittance falls to 0.8 / 0.5 / 0.2 (CL_DEPTH_NONE: never) -- cloud_dir_prof */
struct ClFarR { vec3 L; float T; float depth; float first; uint evals; vec4 prof; };

/* static per-texel jitter: white noise (an interleaved-gradient pattern beat against the step slices into diagonal
   moire); fixed per map texel, so it never flickers and is identical for both eyes */
float cl_far_jit(vec2 p) {
    uvec2 v = uvec2(ivec2(p)) * uvec2(1597334673u, 3812015801u);
    uint n = (v.x ^ v.y) * 1597334673u; n ^= n >> 16; n *= 2246822519u; n ^= n >> 13;
    return float(n) * (1.0 / 4294967296.0);
}

/* ---- procedural weather (the fallback without the game's cloud list): C1 coverage -> R coverage, G 0, B 1, A 1.
   World-anchored (full cloud-frame x/z), not periodic; the weather pass writes it into the window and the march uses
   it beyond the window. */
float cl_far_hash2(ivec2 c) {
    uvec2 v = uvec2(c) * uvec2(1597334673u, 3812015801u);
    uint n = (v.x ^ v.y) * 1597334673u;
    n ^= n >> 16; n *= 2246822519u; n ^= n >> 13;
    return float(n) * (1.0 / 4294967296.0);
}
float cl_far_vnoise(vec2 hi, vec2 lo, float cell) {      /* value noise at hi + lo (hi: multiples of 4096 m) */
    vec2 q = hi / cell + lo / cell, fl = floor(q), f = q - fl;
    ivec2 i = ivec2(fl);
    f = f * f * (3.0 - 2.0 * f);
    float a = cl_far_hash2(i), b = cl_far_hash2(i + ivec2(1, 0)), c = cl_far_hash2(i + ivec2(0, 1)),
          d = cl_far_hash2(i + ivec2(1, 1));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
vec4 cl_weather_proc(vec3 p_m, bool fine) {
    vec2 hi = cl_world_full_xz_hi(), lo = cl_world_full_xz_lo(p_m);
    float n = 0.48 * cl_far_vnoise(hi, lo, 4700.0) + 0.27 * cl_far_vnoise(hi, lo + 1234.5, 2300.0)
            + 0.15 * cl_far_vnoise(hi, lo - 777.0, 1100.0) + (fine ? 0.10 * cl_far_vnoise(hi, lo + 333.0, 470.0) : 0.05);
    float C = cl.band.z;
    /* fBm of value noise: mean 0.5, sd ~0.12; the threshold follows the coverage, overcast (C -> 1) covers all */
    float thr = mix(0.70, 0.30, C);
    float cov = smoothstep(thr - 0.05, thr + 0.10, n);
    cov = max(cov, smoothstep(0.85, 1.0, C));
    return vec4(cov, 0.0, 1.0, 1.0);
}
/* the weather for the far march: the window texture, beyond ~9.5 km from its centre the procedural field (or
   nothing on the cloud-list path); never the toroidal seam */
vec4 cl_far_weather(vec3 p_m) {
    vec2 r2 = abs(p_m.xz - fp.win.xy);
    float r = max(r2.x, r2.y);
    if (r < 9000.0) return cl_weather(p_m);
    vec4 far = fp.win.z > 0.5 ? vec4(0.0, 0.0, 1.0, 0.0) : cl_weather_proc(p_m, false);
    if (r >= 9800.0) return far;
    return mix(cl_weather(p_m), far, smoothstep(9000.0, 9800.0, r));
}
float cl_far_density(vec3 p, bool detail, float step_m) { return cl_density_ws(p, cl_far_weather(p), detail, step_m * fp.mz.y); }

/* March one ray (cloud space) through the band between tstart and tmax. jit in 0..1 (static per map texel). */
#ifndef CL_FAR_SPEC
#define CL_FAR_SPEC 2           /* fine-step samples read ahead (1 = one at a time; 4 costs registers and wasted reads:
                                   3x slower at full load on the 3090 Ti) */
#endif
ClFarR cl_far_march(ClFarP P, vec3 rd, float jit) {
    ClFarR r = ClFarR(vec3(0.0), 1.0, CL_DEPTH_NONE, CL_DEPTH_NONE, 0u, vec4(CL_DEPTH_NONE));
    float hb = cl.band.x, ht = cl.band.y, y0 = cl_alt(P.ro);
    float t0 = P.tstart, t1 = P.tmax;
    if (abs(rd.y) < 1e-6) {
        if (y0 <= hb || y0 >= ht) return r;
    } else {
        float ta = (hb - y0) / rd.y, tb = (ht - y0) / rd.y;
        t0 = max(t0, min(ta, tb)); t1 = min(t1, max(ta, tb));
    }
    if (t1 <= t0) return r;
    float sig0 = cl_extinction();
    float dtmax = fp.stp.y, coarse = fp.stp.x;
    float wsum = 0.0, dsum = 0.0;
    jit = 0.5 + (jit - 0.5) * fp.mz.x;
    float t = t0 + jit * clamp(t0 * P.k, P.dtmin, dtmax);
    float tempty = t0;          /* last position known to be empty */
    int zrun = 3;               /* >= 3: empty-space (coarse) stepping */
    int nl = 0; float above = 0.0, sun_t = 1.0;
    int lstep = max(int(fp.mz.z), 1);
    int i = 0;
    bool done = false;
    float dte = -1.0;           /* > 0: the step of the sample right after an entry (from the step before the entry) */
    float dthin = 0.0;          /* the last in-cloud step when it was a thin-medium one (empty samples keep it) */
    while (!done && i < P.nmax && t < t1) {
        if (zrun >= 3) {
            dthin = 0.0;
            float dt = clamp(t * P.k, P.dtmin, dtmax);
            float dc = cl_far_density(P.ro + rd * t, false, dt); r.evals++;
            if (dc <= 0.0) { tempty = t; t += dt * coarse; i++; continue; }
            /* hit after empty space: bisect the entry twice between the last empty sample and here, then march
               finely from there (removes the slice pattern at silhouettes); that fine sample is this iteration's */
            float ta = tempty, tb = t;
            for (int b = 0; b < 2; b++) {
                float tm = 0.5 * (ta + tb);
                if (cl_far_density(P.ro + rd * tm, false, dt) > 0.0) tb = tm; else ta = tm;
                r.evals++;
            }
            zrun = 0;
            dte = max(dt * fp.fine.x, P.dtmin * 0.5);
            t = ta + jit * dte;                            /* from the last empty point, jittered by one fine step */
        }
        /* fine stepping, CL_FAR_SPEC samples read ahead: their positions are fixed unless a sample's optical depth caps
           its step or three empty samples end the fine mode. All their reads go out together (the weather, both noise
           texels, the probe's weather and base noise), then they are integrated in order; where a guess fails the
           loop re-speculates from the true position. Same samples, same order as one-at-a-time stepping: the near
           tiles are latency-bound (~2 us of dependent reads per step with the GPU mostly idle). */
        float ts[CL_FAR_SPEC], ds[CL_FAR_SPEC];
        vec4 W[CL_FAR_SPEC], B[CL_FAR_SPEC], D[CL_FAR_SPEC], WQ[CL_FAR_SPEC], BQ[CL_FAR_SPEC];
        float tt = t;
        [[unroll]] for (int j = 0; j < CL_FAR_SPEC; j++) {
            float dj = (j == 0 && dte > 0.0) ? dte : max(clamp(tt * P.k, P.dtmin, dtmax) * fp.fine.x, P.dtmin * 0.5);
            ts[j] = tt; ds[j] = dj; tt += dj;
        }
        dte = -1.0;
        [[unroll]] for (int j = 0; j < CL_FAR_SPEC; j++) {
            vec3 pj = P.ro + rd * ts[j], qj = cl_probe_pos(pj);
            float lj = ds[j] * fp.mz.y;
            W[j] = cl_far_weather(pj); B[j] = cl_noise_base_ws(pj, lj);
            D[j] = ts[j] < fp.haze.w ? cl_noise_detail_ws(pj, lj) : vec4(0.0);
            WQ[j] = cl_far_weather(qj); BQ[j] = cl_noise_base_ws(qj, lj);
        }
        [[unroll]] for (int j = 0; j < CL_FAR_SPEC; j++) {
            /* sample 0 belongs to the iteration the while condition (or the entry) admitted */
            if (j > 0 && (done || zrun >= 3 || i >= P.nmax || !(t < t1) || t != ts[j])) break;
            float dt = ds[j];
            vec3 p = P.ro + rd * t;
            bool det = t < fp.haze.w;
            float d = cl_density_tx(p, W[j], B[j], D[j], det); r.evals += 0x10000u;
            if (d > 0.0) {
                zrun = 0;
                r.first = min(r.first, t);
                float fade = 1.0 - smoothstep(fp.haze.y, fp.haze.z, t);
                float sg = d * sig0 * fade;
                if (nl++ % lstep == 0) {          /* the one probe: cloud above (+ the cast shadow from there on) */
                    vec3 qp = cl_probe_pos(p);
                    above = cl_above_d(cl_density_tx(qp, WQ[j], BQ[j], vec4(0.0), false));
                    sun_t = cl_cast_t(qp);
                    r.evals += 0x10000u;
                }
                vec3 c = cl_shade_s(p, rd, d, cl_local_h(p, W[j]), above, sun_t);
                float dto = fp.stp.w / max(sg, 1e-6);                      /* the step of stp.w optical depth */
                if (dto < dt) dt = dto;                                     /* at most stp.w optical depth per step */
                else if (fp.haze.x > 0.0)   /* faint cloud: longer steps, so a large faint cloud does not use up the step
                                               budget before the clouds behind it (up to the empty-space step) */
                    dt = min(max(dt, fp.haze.x * dto), clamp(t * P.k, P.dtmin, dtmax) * coarse);
                dthin = fp.haze.x > 0.0 && dto >= ds[j] ? dt : 0.0;
                float tr = exp(-sg * dt);
                float w = r.T * (1.0 - tr);
                r.L += w * c; dsum += w * t; wsum += w;
                float tp = r.T;
                r.T *= tr;
                /* the opacity profile: where the transmittance crosses 0.8 / 0.5 / 0.2 inside this step (exactly, by the
                   step's exponential) */
                if (tp > 0.2 && r.T <= 0.8) {
                    const vec3 th = vec3(0.8, 0.5, 0.2);
                    [[unroll]] for (int q = 0; q < 3; q++)
                        if (tp > th[q] && r.T <= th[q]) r.prof[q + 1] = t + min(log(tp / th[q]) / max(sg, 1e-9), dt);
                }
                if (r.T < fp.stp.z) {           /* early out: the rest (below the limit) at this sample; the profile's
                                                   thresholds still above it end here */
                    [[unroll]] for (int q = 1; q < 4; q++) r.prof[q] = min(r.prof[q], t + dt);
                    r.L += r.T * c; dsum += r.T * t; wsum += r.T; r.T = 0.0; done = true;
                }
            } else {
                zrun++;
                tempty = t;
                dt = max(dt, dthin);    /* a hole in a faint cloud: keep its long step (the fine one crawls) */
            }
            if (!done) t += dt;
            i++;
        }
    }
    if (wsum > 1e-4 && r.T < 0.99) r.depth = dsum / wsum;
    r.prof.x = r.first;
    return r;
}

#endif
