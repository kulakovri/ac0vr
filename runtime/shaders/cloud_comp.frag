#version 450
#ifdef MV
#extension GL_EXT_multiview : require
#endif
#extension GL_EXT_control_flow_attributes : require
/* Cloud composite (S4, ps2_clouds.c), over the eye image of slot 0 with blending dst.rgb = src.rgb + dst.rgb * src.a
   (premultiplied, a = transmittance; the target's alpha is not written). Multiview: one draw for both layers.
   mode 1 far: the octahedral direction map, sampled by the pixel's world view direction (identical in both eyes),
          hidden where the scene is nearer than the cloud's mean distance (soft over +-10 %); with ACZ_VR_CLOUD_FAR_SPLIT
          (default) the share of the cloud in front of the scene from the map's opacity profile (far_split), and with
          ACZ_VR_CLOUD_SHAFTS the light shafts in the haze in front of it.
   mode 2 near: the half-resolution near buffer of this eye, nearest-depth upsampled (Jansen & Bavoil): bilinear
          where the four low-resolution clip distances agree with the pixel's scene distance, else the texel whose
          clip distance is closest to it.
   Optical phenomena (ACZ_VR_OPTICS, cloud_optics.glsl), evaluated here per pixel when the CPU says a phenomenon is
   possible: halo, glory and spectre on the content of the composite that shows it (the far map / the near volume),
   the rainbow in the far composite behind the far content's scene test (rain in front of the terrain only up to the
   scene distance).
   STAMP variant (compiled with -DSTAMP; ACZ_VR_CLOUD_FX, bug #46): right after a composite, the cloud's depth goes into
   the frame's depth buffer where our cloud is dense (the transmittance in front below pc.f0.x), so the game's effect
   sprites drawn after it (smoke, explosions, fire, missile trails: alpha-blended, depth-tested, no depth write) stay
   in front of nearer cloud and go behind farther dense cloud by their own GS depth. No colour attachment; the depth
   test (GREATER, larger = nearer) keeps the scene where it is nearer than the cloud. mode 3: the far map alone (its
   transmittance-weighted mean distance); mode 4: the near volume first (the half-resolution near buffer: dense within
   its range -> its clip distance, the end of the range, since the near buffer holds no front distance), then near x
   far at the far map's distance; mode 5: the near volume alone (near-only clouds).
   VEIL variant (compiled with -DVEIL; ACZ_VR_CLOUD_FX_SOFT, bug #254): instead of the binary stamp, the effects' depth
   snapshot (ps2_video.c fxs_*, an RGBA16F image the effect draws read as their set-1 texture) gets per pixel the
   scene's view depth (m; 0 = nothing drawn) and our cloud's transmittance in front as a ramp: r scene depth, g the view
   depth where the cloud starts, b where it has reached its total opacity, a its total transmittance (near volume x far
   map, modes 3 / 4 / 5 as the stamp); gs.frag fades an effect by the transmittance in front of its own depth. The ramp
   goes through the depth where half of the opacity is reached (the near volume taken as uniform over its range, the far
   map from its opacity profile, bilinear over the four texels with cloud), so the effect fades as smoothly as the cloud
   does instead of being cut where the transmittance crosses the stamp's limit (the weather's 39 m texels and the far
   map's tiles showed as stair-stepped edges across Morgan's bursts in M18). */
#include "cloud_common.glsl"
/* ACZ_VR_CLOUD_COMP_UNROLL (fq_opt 2026-10-26; ps2_clouds.c make_composite_v): 1 = the near composite's four-tap loop
   marked [[unroll]] (the same statements, the same order), 0 = as before. Not unrolled, the loop's four taps sat in a
   local array indexed by the loop counter and the chosen one was read back by its run-time index (s[bi]): the near
   composite cost 0.43-0.45 ms per stereo frame at 4x (M3 / M4 / M12, own timestamps) against ~0.10 for the far one */
layout(constant_id = 0) const int CL_COMP_UNROLL = 1;
/* perf6_clouds 2026-11-03 (ps2_clouds.c make_composite_g / make_stamp_d): CL_NEAR_DIV (ACZ_VR_CLOUD_DETAIL, the CLOUD
   DETAIL rungs): a near texel covers CL_NEAR_DIV x CL_NEAR_DIV target pixels (2 = half resolution per axis, as before) */
layout(constant_id = 2) const int CL_NEAR_DIV = 2;
/* the near buffer's texel position of target pixel position fc (near_px's result) */
vec2 near_tc(vec2 fc) { return CL_NEAR_DIV == 2 ? fc * 0.5 : fc / float(CL_NEAR_DIV); }
layout(push_constant) uniform PC { ivec4 i0; ivec4 i1; vec4 f0; vec4 f1; } pc;   /* i0.x mode, i0.y eye (two-pass),
                                                                                    i0.z 1: only the visible crop,
                                                                                    i0.w debug tint (STAMP: -),
                                                                                    i1.x bit 0 edge-aware far depth,
                                                                                    bit 1 the far split;
                                                                                    f0.x the stamp's transmittance
                                                                                    limit */
/* The far map's depth at uv (ACZ_VR_CLOUD_EDGE_DEPTH, 2026-10-22): bilinear over the texels that hold cloud only. The
   plain linear fetch blended 65504 (no cloud) into a cloud's edge texels, so the depth there jumped to tens of km and,
   against distant terrain near the horizon, the scene test (k below) hid the cloud's edge in a comb / saw-tooth along
   the map's texel grid. pc.i1.x 0 = the plain fetch of before. */
/* ACZ_REC_CINE (ps2_clouds.c near_h_push, i1.y 1): the recording's cinematic view composites the frame's near buffer,
   marched for the head's view: this target pixel's place in the head's image (a homography of the two views' turn) */
vec2 near_px(vec2 fc) {
    if (pc.i1.y == 0) return fc;
    vec3 h = vec3(pc.f1.x, pc.f1.y, pc.f1.z) * fc.x + vec3(pc.f1.w, pc.f0.y, pc.f0.z) * fc.y
           + vec3(pc.f0.w, intBitsToFloat(pc.i1.z), intBitsToFloat(pc.i1.w));
    return h.xy / h.z;
}
float far_depth(vec2 uv) {
    if ((pc.i1.x & 1) == 0) return textureLod(cl_dir_depth_tex, uv, 0.0).r;
    vec2 sz = vec2(textureSize(cl_dir_depth_tex, 0));
    vec2 t = uv * sz - 0.5, f = fract(t);
    vec4 g = textureGather(cl_dir_depth_tex, (floor(t) + 1.0) / sz, 0);   /* (i0,j1) (i1,j1) (i1,j0) (i0,j0) */
    vec4 w = vec4((1.0 - f.x) * f.y, f.x * f.y, f.x * (1.0 - f.y), (1.0 - f.x) * (1.0 - f.y));
    vec4 v = vec4(lessThan(g, vec4(0.5 * CL_DEPTH_NONE)));
    float ws = dot(w, v);
    return ws > 1.0e-6 ? dot(w * v, g) / ws : CL_DEPTH_NONE;
}
/* ACZ_VR_CLOUD_FAR_SPLIT (cl_split 2026-10-30, bug #250; pc.i1.x bit 1): the share of the far map's cloud in front of the
   scene at distance sd (cl_prof_frac per texel, bilinear over the four texels around uv that hold cloud). Before (and
   with the bit clear) the whole cloud showed where the scene lay beyond 1.1 x its mean distance and none of it nearer
   than 0.9 x: an aircraft or a slope inside a cloud lost the veil in front of it, and a slope behind a cloud's mean
   distance got all of the cloud, the part behind the slope too -- a contour across terrain at the cloud's mean
   distance. a modern game splits its cloud image into near and far parts at the geometry depth for the same reason. */
float far_split(vec2 uv, float sd) {
    vec2 sz = vec2(textureSize(cl_dir_prof_tex, 0));
    vec2 t = uv * sz - 0.5, f = fract(t);
    ivec2 b = ivec2(floor(t)), lim = ivec2(sz) - 1;
    vec4 gm = textureGather(cl_dir_depth_tex, (floor(t) + 1.0) / sz, 0);   /* (i0,j1) (i1,j1) (i1,j0) (i0,j0) */
    vec4 gt = textureGather(cl_dir_tex, (floor(t) + 1.0) / sz, 3);
    vec4 w = vec4((1.0 - f.x) * f.y, f.x * f.y, f.x * (1.0 - f.y), (1.0 - f.x) * (1.0 - f.y));
    const ivec2 o[4] = ivec2[4](ivec2(0, 1), ivec2(1, 1), ivec2(1, 0), ivec2(0, 0));
    float ks = 0.0, ws = 0.0;
    [[unroll]] for (int i = 0; i < 4; i++) {
        float a = 1.0 - gt[i];
        if (a <= 1.0e-3) continue;
        vec4 pr = texelFetch(cl_dir_prof_tex, clamp(b + o[i], ivec2(0), lim), 0);
        if (!(pr.x < 0.5 * CL_DEPTH_NONE)) continue;
        float wi = w[i] * a;                                   /* (by opacity: a faint rim texel weighs little) */
        ks += wi * cl_prof_frac(pr, gm[i], a, sd); ws += wi;
    }
    return ws > 1.0e-6 ? ks / ws : 1.0;
}
/* ACZ_VR_CLOUD_FAR_SPLIT in the depth stamp: where the far cloud's transmittance in front falls to tl (the profile of
   the nearest texel: 1 at its first cloud, 0.8 / 0.5 / 0.2 at its crossings, linear in between); the mean distance cd
   where the profile does not reach it */
float far_dist_at(vec2 uv, float tl, float cd) {
    ivec2 sz = textureSize(cl_dir_prof_tex, 0);
    vec4 pr = texelFetch(cl_dir_prof_tex, clamp(ivec2(uv * vec2(sz)), ivec2(0), sz - 1), 0);
    float a = pr.x, b = pr.y, k = (1.0 - tl) / 0.2;
    if (tl < 0.8) { a = pr.y; b = pr.z; k = (0.8 - tl) / 0.3; }
    if (tl < 0.5) { a = pr.z; b = pr.w; k = (0.5 - tl) / 0.3; }
    if (tl < 0.2) return cd;
    return a < 0.5 * CL_DEPTH_NONE && b < 0.5 * CL_DEPTH_NONE ? mix(a, b, clamp(k, 0.0, 1.0)) : cd;
}
#if defined(VEIL)
/* the along-ray distance where a far map texel's opacity in front reaches at (0 < at <= a = 1 - its transmittance):
   its profile read as cl_prof_frac reads it -- (first, 0), the crossings at 0.2 / 0.5 / 0.8 it reached, then on to a
   at the end estimated from the mean distance m (2 m - first: a uniform cloud) */
float far_dist_opac(vec4 p, float m, float a, float at) {
    if (!(p.x < 0.5 * CL_DEPTH_NONE)) return m;
    float t0 = p.x, a0 = 0.0;
    const vec3 av = vec3(0.2, 0.5, 0.8);
    for (int k = 0; k < 3; k++) {
        float d = p[k + 1];
        if (d < 0.5 * CL_DEPTH_NONE && av[k] <= a) {
            if (at <= av[k]) return mix(t0, d, clamp((at - a0) / max(av[k] - a0, 1.0e-4), 0.0, 1.0));
            t0 = d; a0 = av[k];
        }
    }
    float e = max(m < 0.5 * CL_DEPTH_NONE ? 2.0 * m - p.x : t0, t0) + 1.0;
    return mix(t0, e, clamp((at - a0) / max(a - a0, 1.0e-4), 0.0, 1.0));
}
/* the far map's first cloud and the distance where its opacity reaches at (m along the ray), bilinear over the four
   texels around uv that hold cloud, weighted by their opacity (as far_split); x < 0: none. Without the profile
   (ACZ_VR_CLOUD_FAR_SPLIT off) a ramp about the mean distance. */
vec2 far_veil(vec2 uv, float at) {
    if ((pc.i1.x & 2) == 0) {
        float cd = far_depth(uv);
        return cd < CL_DEPTH_NONE ? vec2(0.8 * cd, cd) : vec2(-1.0);
    }
    vec2 sz = vec2(textureSize(cl_dir_prof_tex, 0));
    vec2 t = uv * sz - 0.5, f = fract(t);
    ivec2 b = ivec2(floor(t)), lim = ivec2(sz) - 1;
    vec4 gm = textureGather(cl_dir_depth_tex, (floor(t) + 1.0) / sz, 0);   /* (i0,j1) (i1,j1) (i1,j0) (i0,j0) */
    vec4 gt = textureGather(cl_dir_tex, (floor(t) + 1.0) / sz, 3);
    vec4 w = vec4((1.0 - f.x) * f.y, f.x * f.y, f.x * (1.0 - f.y), (1.0 - f.x) * (1.0 - f.y));
    const ivec2 o[4] = ivec2[4](ivec2(0, 1), ivec2(1, 1), ivec2(1, 0), ivec2(0, 0));
    vec2 s = vec2(0.0);
    float ws = 0.0;
    [[unroll]] for (int i = 0; i < 4; i++) {
        float a = 1.0 - gt[i];
        if (a <= 1.0e-3) continue;
        vec4 pr = texelFetch(cl_dir_prof_tex, clamp(b + o[i], ivec2(0), lim), 0);
        if (!(pr.x < 0.5 * CL_DEPTH_NONE)) continue;
        float wi = w[i] * a;
        s += wi * vec2(pr.x, far_dist_opac(pr, gm[i], a, min(at, a)));
        ws += wi;
    }
    if (ws > 1.0e-6) return s / ws;
    float cd = far_depth(uv);
    return cd < CL_DEPTH_NONE ? vec2(0.8 * cd, cd) : vec2(-1.0);
}
layout(location = 0) out vec4 out_veil;
void main() {
#ifdef MV
    int eye = int(gl_ViewIndex);
#else
    int eye = pc.i0.y;
#endif
    if (pc.i0.z != 0) {
        vec4 vr = cl.vis[eye];
        if (gl_FragCoord.x < vr.x || gl_FragCoord.x >= vr.z || gl_FragCoord.y < vr.y || gl_FragCoord.y >= vr.w) discard;
    }
    ivec2 px = ivec2(gl_FragCoord.xy);
    float rl = length(cl_view_ray(gl_FragCoord.xy));
    float sv = cl_view_depth_m(texelFetch(cl_scene_depth, ivec3(px, cl_depth_layer(eye)), 0).r);
    float sdv = sv < 60000.0 ? sv : 0.0;                    /* the scene's view depth (m); 0: nothing drawn (sky) */
    int mode = pc.i0.x;                                     /* 3 far map, 4 near volume + far map, 5 near volume */
    float tn = 1.0, rn = 0.0;
    if (mode >= 4) {
        ivec2 q = clamp(ivec2(near_tc(near_px(gl_FragCoord.xy))), ivec2(0), ivec2(cl.sizes.y, cl.sizes.z) - 1);
        tn = texelFetch(cl_near_tex, ivec3(q, eye), 0).a;
        rn = texelFetch(cl_near_depth_tex, ivec3(q, eye), 0).r;      /* (m along the ray: the end of its range) */
        if (!(rn > 1.0 && rn < CL_DEPTH_NONE)) tn = 1.0;
    }
    vec2 uv = cl_dir_tex_uv(cl_world_dir(gl_FragCoord.xy));
    float tf = mode == 5 ? 1.0 : textureLod(cl_dir_tex, uv, 0.0).a;
    float T = clamp(tn * tf, 0.0, 1.0);
    if (T > 0.996) { out_veil = vec4(sdv, 0.0, 0.0, 1.0); return; }
    float ah = 0.5 * (1.0 - T);                             /* half of the opacity in front */
    float d0, dh;
    if (tn < 0.996) {                                       /* the near volume from the eye, uniform over its range */
        d0 = 0.0;
        if (1.0 - tn >= ah) dh = rn * log(1.0 - ah) / log(max(tn, 1.0e-4));
        else {
            vec2 fv = far_veil(uv, 1.0 - (1.0 - ah) / tn);
            dh = fv.x >= 0.0 ? max(fv.y, rn) : rn;
        }
    } else {
        vec2 fv = far_veil(uv, ah);
        if (fv.x < 0.0) { out_veil = vec4(sdv, 0.0, 0.0, 1.0); return; }
        d0 = fv.x; dh = max(fv.y, fv.x);
    }
    float d1 = d0 + 2.0 * max(dh - d0, 1.0);
    out_veil = vec4(sdv, min(d0 / rl, 60000.0), min(d1 / rl, 60000.0), T);
}
#elif defined(STAMP)
void main() {
#ifdef MV
    int eye = int(gl_ViewIndex);
#else
    int eye = pc.i0.y;
#endif
    if (pc.i0.z != 0) {
        vec4 vr = cl.vis[eye];
        if (gl_FragCoord.x < vr.x || gl_FragCoord.x >= vr.z || gl_FragCoord.y < vr.y || gl_FragCoord.y >= vr.w) discard;
    }
    float lim = pc.f0.x;
    vec2 uv = cl_dir_tex_uv(cl_world_dir(gl_FragCoord.xy));
    float tf = textureLod(cl_dir_tex, uv, 0.0).a;
    float cd = far_depth(uv);                                 /* m along the ray; CL_DEPTH_NONE: no cloud */
    float dist = CL_INF;
    if (pc.i0.x >= 4) {
        ivec2 q = clamp(ivec2(near_tc(near_px(gl_FragCoord.xy))), ivec2(0), ivec2(cl.sizes.y, cl.sizes.z) - 1);
        float tn = texelFetch(cl_near_tex, ivec3(q, eye), 0).a;
        if (tn < lim) dist = texelFetch(cl_near_depth_tex, ivec3(q, eye), 0).r;
        else if (pc.i0.x == 4 && cd < CL_DEPTH_NONE && tn * tf < lim)
            dist = (pc.i1.x & 2) != 0 ? far_dist_at(uv, lim / max(tn, 1.0e-3), cd) : cd;
    } else if (cd < CL_DEPTH_NONE && tf < lim) dist = (pc.i1.x & 2) != 0 ? far_dist_at(uv, lim, cd) : cd;
    if (!(dist < CL_DEPTH_NONE) || dist < 4.0) discard;      /* (never inside the canopy) */
    /* distance along the ray -> view depth (m) -> the depth buffer's value (inverse of cl_view_depth_m) */
    float vm = dist / length(cl_view_ray(gl_FragCoord.xy));
    float z = (cl.depth.z / (vm * cl.misc.x) / cl.depth.x - cl.depth.y) / cl.depth.w;
    if (!(z > 0.0)) discard;
    gl_FragDepth = min(z, 1.0);
}
#else
layout(location = 0) out vec4 out_color;

void main() {
#ifdef MV
    int eye = int(gl_ViewIndex);
#else
    int eye = pc.i0.y;
#endif
    ivec2 px = ivec2(gl_FragCoord.xy);
    if (pc.i0.z != 0) {                       /* outside what the headset shows of this eye (C2 vis): nothing to do */
        vec4 vr = cl.vis[eye];
        if (gl_FragCoord.x < vr.x || gl_FragCoord.x >= vr.z || gl_FragCoord.y < vr.y || gl_FragCoord.y >= vr.w) discard;
    }
    vec4 c = vec4(0.0, 0.0, 0.0, 1.0);
    float base_max = 1.0;                                   /* (far: the map's own colour before the optics) */
    float sd;
    if (pc.i0.x == 1) {
        vec3 d = cl_world_dir(gl_FragCoord.xy);
        vec2 uv = cl_dir_tex_uv(d);
        c = textureLod(cl_dir_tex, uv, 0.0);
        /* ACZ_VR_CLOUD_COMP_EARLY (cl_split 2026-10-30; pc.i1.x bit 2; a modern game draws its composite only over the tiles with
           cloud or geometry): an empty map texel with nothing else to add leaves before the scene depth and the far map's
           depth are fetched -- the same pixels the final test below discards, so the image is identical */
        if ((pc.i1.x & 4) != 0 && c.a >= 0.999 && max(c.r, max(c.g, c.b)) <= 0.0 && !cl_opt_any() && cl.shaft[1].x < 0.5
            && pc.i0.w == 0) discard;
        sd = cl_scene_dist_m(px, eye);
        float cd = far_depth(uv);
        float tf = c.a;
        bool opt = cl_opt_any();
        base_max = max(c.r, max(c.g, c.b));
        if (opt) c = cl_optics_cloud(c, d, cd, eye, 0);
        if (cl_flash_n() > 0.5 && cd < CL_DEPTH_NONE && c.a < 0.999)          /* flashes (wish #83) */
            c.rgb += (1.0 - c.a) * cl_flash_far(d * cd, d);
        if (sd < CL_INF && cd < CL_DEPTH_NONE) {
            float k = (pc.i1.x & 2) != 0 ? far_split(uv, sd)  /* the cloud's share in front of the scene */
                    : smoothstep(0.9 * cd, 1.1 * cd, sd);     /* 0: the scene is in front of the cloud */
            c = vec4(c.rgb * k, mix(1.0, c.a, k));
        }
        if (opt) c.rgb += cl_optics_bow(d, sd, cd, tf);
        if (pc.i0.w != 0 && tf < pc.f0.x && cd < CL_DEPTH_NONE && !(sd < cd))   /* ACZ_DEBUG_CLOUD_FX_TINT: the stamp */
            c.rgb += cd < 300.0 ? vec3(0.0, 0.4, 0.0) : vec3(0.3, 0.0, 0.0);   /* (green: a stamp nearer than 300 m) */
        if (cl.shaft[1].x > 0.5) {
            /* ACZ_VR_CLOUD_SHAFTS (cl_split, bug #250; cloud_shafts.comp): the haze in front of the scene -- in front of the
               cloud's first distance where a cloud covers the pixel, the air behind it seen through its transmittance --
               darkened where our clouds shade it (D > 0), the fog colour added where it lies in sunlight between shaded
               air (D < 0). Applied to everything under this composite (the scene, the far cloud). */
            vec4 sh = textureLod(cl_shaft_tex, cl_dir_uv(d), 0.0);
            float dist = min(sd, 2.0 * cl.shaft[0].w), front = CL_INF;
            if (tf < 0.999 && cd < CL_DEPTH_NONE) {
                float pf = texelFetch(cl_dir_prof_tex, clamp(ivec2(uv * vec2(textureSize(cl_dir_prof_tex, 0))), ivec2(0),
                                                               textureSize(cl_dir_prof_tex, 0) - 1), 0).x;
                front = pf < 0.5 * CL_DEPTH_NONE ? pf : 0.9 * cd;
            }
            float D = cl_shaft_at(sh, dist);
            if (front < dist) { float Df = cl_shaft_at(sh, front); D = Df + (D - Df) * tf; }
            float m = 1.0 - clamp(D, 0.0, 0.9);
            c = vec4(c.rgb * m + cl.fog.rgb * clamp(-D, 0.0, 0.5) * cl.shaft[1].y, c.a * m);
        }
    } else if (pc.i0.x == 2) {
        vec2 lp = near_tc(near_px(gl_FragCoord.xy)) - 0.5;     /* low-resolution texel space (centres at integers) */
        ivec2 b = ivec2(floor(lp));
        vec2 f = lp - vec2(b);
        ivec2 lim = ivec2(cl.sizes.y, cl.sizes.z) - 1;
        if ((pc.i1.x & 4) != 0 && pc.i0.w == 0) {
            /* ACZ_VR_CLOUD_COMP_EARLY: all four near texels empty -> whatever the upsample picks is empty (the optics need
               cloud): leave before the scene depth and the four near depths (the same pixels as the final test). (Fetching
               the four here once and reusing them in the loop below put the loop's array into local memory: the near
               composite 0.07 -> 0.35 ms in M3; the second fetch hits the cache.) */
            bool empty = true;
            [[unroll]] for (int k = 0; k < 4; k++) {
                vec4 e = texelFetch(cl_near_tex, ivec3(clamp(b + ivec2(k & 1, k >> 1), ivec2(0), lim), eye), 0);
                empty = empty && e.a >= 0.999 && max(e.r, max(e.g, e.b)) <= 0.0;
            }
            if (empty) discard;
        }
        sd = cl_scene_dist_m(px, eye);
        if (sd < 4.0) discard;                                  /* the game's own cockpit: no fog inside the canopy */
        float range = cl.cfg.z, sdc = min(sd, range);
        vec4 s[4]; float dd[4]; float best = 1e30; int bi = 0, agree = 1;
        if (CL_COMP_UNROLL != 0) {
            [[unroll]] for (int k = 0; k < 4; k++) {           /* (ACZ_VR_CLOUD_COMP_UNROLL: the same loop, unrolled) */
                ivec2 q = clamp(b + ivec2(k & 1, k >> 1), ivec2(0), lim);
                s[k] = texelFetch(cl_near_tex, ivec3(q, eye), 0);
                float nd = texelFetch(cl_near_depth_tex, ivec3(q, eye), 0).r;
                dd[k] = abs(nd - sdc);
                if (dd[k] > 0.1 * sdc + 1.0) agree = 0;
                if (dd[k] < best) { best = dd[k]; bi = k; }
            }
        } else {
            for (int k = 0; k < 4; k++) {                      /* (0: as before) */
                ivec2 q = clamp(b + ivec2(k & 1, k >> 1), ivec2(0), lim);
                s[k] = texelFetch(cl_near_tex, ivec3(q, eye), 0);
                float nd = texelFetch(cl_near_depth_tex, ivec3(q, eye), 0).r;
                dd[k] = abs(nd - sdc);
                if (dd[k] > 0.1 * sdc + 1.0) agree = 0;
                if (dd[k] < best) { best = dd[k]; bi = k; }
            }
        }
        if (agree == 1) c = mix(mix(s[0], s[1], f.x), mix(s[2], s[3], f.x), f.y);
        else c = s[bi];
        if (c.a < 0.999 && cl_opt_any()) {                     /* glory / spectre on near cloud, inside their box */
            vec4 bx = cl.opt[21 + eye];
            if (gl_FragCoord.x >= bx.x && gl_FragCoord.x <= bx.z && gl_FragCoord.y >= bx.y && gl_FragCoord.y <= bx.w)
                c = cl_optics_cloud(c, cl_world_dir(gl_FragCoord.xy), CL_DEPTH_NONE, eye, 1);
        }
        if (pc.i0.w != 0) {                                     /* ACZ_DEBUG_CLOUD_FX_TINT: the near stamp (as STAMP) */
            ivec2 q = clamp(ivec2(near_tc(near_px(gl_FragCoord.xy))), ivec2(0), lim);
            if (texelFetch(cl_near_tex, ivec3(q, eye), 0).a < pc.f0.x)
                c.rgb += texelFetch(cl_near_depth_tex, ivec3(q, eye), 0).r < 300.0 ? vec3(0.0, 0.4, 0.0) : vec3(0.0, 0.0, 0.3);
        }
    }
    float cmax = max(c.r, max(c.g, c.b));
    /* nothing to add: no cloud here, or only optics light below half a level over an empty map (no write) */
    if (c.a >= 0.999 && (cmax <= 0.0 || (base_max <= 0.0 && cmax < 0.002))) discard;
    out_color = vec4(c.rgb, c.a);
}
#endif
