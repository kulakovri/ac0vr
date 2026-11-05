/* cloud_common.glsl - shared declarations of every cloud shader (CLOUDS_3D_RESEARCH_2026-10-20.md section 8; the C
   side is runtime/src/ps2_clouds.h). Owner S0; changes by request.
   Include after #version (and #extension GL_EXT_multiview for a multiview fragment shader). Define CL_STORAGE before
   the include in compute shaders that write the C3 images.

   Cloud space: metres, y up, camera-relative (origin = the frame's centre eye). The cloud frame (where clouds stand
   still: world minus the wind drift) position of p is cl_world(p): x/z modulo 60 km (every noise period in x/z must
   divide 60000 m), y = altitude in metres; cl_world4k(p): x/z modulo 4000 m (the base noise period, most precise);
   cl_world_full_xz(p): the full x/z (hi + lo, for hashes). */
#extension GL_GOOGLE_include_directive : enable

#define CL_INF 1.0e9
#define CL_DEPTH_NONE 65504.0
#define CL_WEATHER_M 20000.0
#define CL_NOISE_BASE_M 4000.0
#define CL_NOISE_DETAIL_M 250.0

/* C1 + C2 + settings, one copy per pass (ps2_clouds.c cl_frame_ubo mirrors this, std140, vec4 only) */
layout(set = 0, binding = 0, std140) uniform CloudFrame {
    vec4 cam_x;          /* xyz: the view's right axis in world (both eyes) */
    vec4 cam_y;          /* up */
    vec4 cam_z;          /* back (= -forward) */
    vec4 eye_m[2];       /* xyz: left / right eye in cloud space (m, relative to the origin = centre eye) */
    vec4 proj;           /* target pixels: fx, fy, cx, cy */
    vec4 img;            /* the eye image in the target: x0, y0, w, h (pixels) */
    vec4 depth;          /* a, c, mh, zmax: view depth (game units) = mh / (a (z zmax + c)) */
    vec4 misc;           /* x units per metre, y time (s, wraps every 4096 s), z in-between alpha, w frame counter */
    vec4 origin;         /* cloud-frame origin: x, z modulo 60 km (m); y: altitude (m); w: rebase counter */
    vec4 band;           /* C1: base_m, top_m, coverage, type */
    vec4 env;            /* C1: transp, precip, in_cloud, valid bits */
    vec4 sun;            /* C1: xyz towards the sun (world, unit) */
    vec4 lit;            /* C1 colours in slot-0 space (PS2 display value / 255, no gamma decode): LIGHT palette */
    vec4 shadow;         /* rgb: DARK palette (zenith sky), w: shadow_a (dark-rim strength) */
    vec4 fog;
    vec4 sky_top;
    vec4 sky_hor;
    vec4 fogr;           /* C1: fog_near_m, fog_far_m, ncloud, seq */
    vec4 cfg;            /* far steps, near steps, near range (m), far slice */
    vec4 sizes;          /* dir N, near w, near h, target w */
    vec4 sizes2;         /* target h, in-between (0/1), scene depth layers (1 two-pass, 2 multiview), half ipd (m) */
    vec4 wx;             /* weather: uv offset of the origin (x, y), 1 / CL_WEATHER_M, test pattern mode */
    vec4 sky_below;      /* C1: the ground-side haze ring colour below the horizon */
    vec4 incloud;        /* C1: the frame colour inside the game's overcast */
    vec4 wind;           /* C1: wind world x, z (m/s); drift x, z (m) */
    vec4 origin_split;   /* cloud-frame origin x_hi, z_hi (multiples of 4096 m), x_lo, z_lo: full = hi + lo */
    vec4 origin_4k;      /* cloud-frame origin x, z modulo 4000 m; C1 list_seq (& 0xFFFFFF); weather source (0 S0
                            procedural default, 1 S2's weather pass) */
    vec4 vis[2];         /* per eye: the part the headset shows, target pixels x0, y0, x1, y1 */
    vec4 user[4];        /* C1, S5 per mission: user[0] shape (base-noise period m, detail period m, erosion, billow
                            weight; defaults in cl_shape_params when x == 0); user[1] shared shading (cl_shade_params;
                            all 0 = defaults 0.15, 0.5, 0.25, 1); user[2] near-only (S3); user[3] spare (S5) */
    vec4 shd[4];         /* the cloud shadow map's parameters s0, s1, s2 (cloud_shadow.glsl; ps2_clouds_shadow.c):
                            s0.w = 0 when the map is off (the shading as before); shd[3] = the cast shadow's strength,
                            its colour's place on the DARK -> LIGHT line (cl_cast_shade) */
    vec4 opt[23];        /* optical phenomena (ACZ_VR_OPTICS; ps2_clouds_optics.c, layout in cloud_optics.glsl): all
                            zero = off */
    vec4 flash[9];       /* flash lights (ACZ_VR_FLASH_LIGHT; ps2_video.c flash_frame -> ps2_clouds.c): [0].x = count
                            (0..4); light i: [1 + 2i] = position (cloud space, m), w = the distance where it has
                            halved (m); [2 + 2i] = rgb (slot-0 units at the light, the pulse included) */
    vec4 shaft[2];       /* light shafts in the haze (ACZ_VR_CLOUD_SHAFTS, cloud_shafts.comp): [0] = the map's four
                            distances (m); [1].x = 1 when the map is valid, y = 1: sunlit air may brighten (D < 0) */
} cl;

layout(set = 0, binding = 1) uniform sampler3D cl_noise_base;      /* 128^3, period CL_NOISE_BASE_M */
layout(set = 0, binding = 2) uniform sampler3D cl_noise_detail;    /* 32^3, period CL_NOISE_DETAIL_M */
layout(set = 0, binding = 3) uniform sampler2D cl_weather_tex;     /* 512^2, 20 km, REPEAT by world position */
layout(set = 0, binding = 4) uniform sampler2DArray cl_scene_depth;   /* the frame's D32 depth, nearest */
layout(set = 0, binding = 9) uniform sampler2D cl_dir_tex;         /* far map, linear */
layout(set = 0, binding = 10) uniform sampler2D cl_dir_depth_tex;
layout(set = 0, binding = 11) uniform sampler2DArray cl_near_tex;  /* near buffer, nearest */
layout(set = 0, binding = 12) uniform sampler2DArray cl_near_depth_tex;
layout(set = 0, binding = 16) uniform sampler2D cl_shadow_tex;     /* cloud shadow map (cloud_shadow.glsl), linear, repeat */
layout(set = 0, binding = 21) uniform sampler2D cl_dir_prof_tex;   /* far map's opacity profile (ACZ_VR_CLOUD_FAR_SPLIT) */
layout(set = 0, binding = 23) uniform sampler2D cl_shaft_tex;      /* light shafts (ACZ_VR_CLOUD_SHAFTS), linear, clamp */
#ifdef CL_STORAGE
layout(set = 0, binding = 5, rgba16f) uniform image2D cl_dir_img;
layout(set = 0, binding = 6, r16f) uniform image2D cl_dir_depth_img;
layout(set = 0, binding = 7, rgba16f) uniform image2DArray cl_near_img;
layout(set = 0, binding = 8, r16f) uniform image2DArray cl_near_depth_img;
layout(set = 0, binding = 13, rgba8) uniform image2D cl_weather_img;
layout(set = 0, binding = 14, rgba8) uniform image3D cl_noise_base_img;
layout(set = 0, binding = 15, rgba8) uniform image3D cl_noise_detail_img;
layout(set = 0, binding = 22, rgba16f) uniform image2D cl_dir_prof_img;
#endif

/* ---- C3: the far map's full-sphere octahedral encoding (y up; the upper hemisphere is the inner diamond) ---- */
vec2 cl_dir_uv(vec3 d){ d/= (abs(d.x)+abs(d.y)+abs(d.z)); vec2 p=d.xz; if(d.y<0.0) p=(1.0-abs(p.yx))*vec2(p.x>=0.0?1.0:-1.0, p.y>=0.0?1.0:-1.0); return p*0.5+0.5; }
/* the far map's used size: S2 may march only the top-left sizes.x x sizes.x texels of the N x N image (1536 for
   overcast weather; it repeats the last row / column once); sample the map through cl_dir_tex_uv. cl_dir_uv /
   cl_uv_dir stay the pure mapping on [0, 1]^2. */
vec2 cl_dir_tex_uv(vec3 d) { return cl_dir_uv(d) * (cl.sizes.x / float(textureSize(cl_dir_tex, 0).x)); }
/* ACZ_VR_CLOUD_FAR_SPLIT (cl_split 2026-10-30, bug #250): the share of a far-map texel's cloud that lies in front of scene
   distance sd. a trueSKY-based game marches near and far cloud images split at the geometry depth; the direction map serves
   every depth at once, so it keeps the ray's opacity profile instead (cloud_dir_prof: the first cloud and the
   distances where the transmittance falls to 0.8 / 0.5 / 0.2) and the composite takes the opacity in front of the
   scene from it: piecewise linear in distance through (first, 0), the crossings it reached, then on to the total
   opacity a = 1 - T at an end point estimated from the mean distance m (2 m - first: a uniform cloud). Returns the
   fraction 0..1 of the texel's opacity (and, the colour going with it, of its in-scattered light). */
float cl_prof_frac(vec4 p, float m, float a, float sd) {
    if (a <= 1.0e-4 || !(p.x < 0.5 * CL_DEPTH_NONE)) return 1.0;
    if (sd <= p.x) return 0.0;
    float t0 = p.x, a0 = 0.0;
    const vec3 av = vec3(0.2, 0.5, 0.8);
    for (int k = 0; k < 3; k++) {
        float d = p[k + 1];
        if (d < 0.5 * CL_DEPTH_NONE && av[k] <= a) {
            if (sd <= d) return mix(a0, av[k], clamp((sd - t0) / max(d - t0, 1.0e-3), 0.0, 1.0)) / a;
            t0 = d; a0 = av[k];
        }
    }
    float e = max(m < 0.5 * CL_DEPTH_NONE ? 2.0 * m - p.x : t0, t0) + 1.0;
    return min(mix(a0, a, clamp((sd - t0) / (e - t0), 0.0, 1.0)) / a, 1.0);
}
/* ACZ_VR_CLOUD_SHAFTS: the haze's signed shadow excess D in front of distance t from the shaft map's texel s (D at the
   distances cl.shaft[0]; 0 at the eye), piecewise linear; constant beyond the last distance */
float cl_shaft_at(vec4 s, float t) {
    vec4 tq = cl.shaft[0];
    if (t <= tq.x) return s.x * clamp(t / max(tq.x, 1.0), 0.0, 1.0);
    if (t <= tq.y) return mix(s.x, s.y, (t - tq.x) / max(tq.y - tq.x, 1.0));
    if (t <= tq.z) return mix(s.y, s.z, (t - tq.y) / max(tq.z - tq.y, 1.0));
    return mix(s.z, s.w, clamp((t - tq.z) / max(tq.w - tq.z, 1.0), 0.0, 1.0));
}
vec3 cl_uv_dir(vec2 uv){ vec2 p=uv*2.0-1.0; vec3 d=vec3(p.x, 1.0-abs(p.x)-abs(p.y), p.y); if(d.y<0.0) d.xz=(1.0-abs(d.zx))*vec2(d.x>=0.0?1.0:-1.0, d.z>=0.0?1.0:-1.0); return normalize(d); }

/* ---- C2: views, rays, depth ---- */
float cl_units() { return cl.misc.x; }
vec3 cl_world(vec3 p) { return cl.origin.xyz + p; }          /* cloud frame (x/z mod 60 km, y altitude) */
vec3 cl_world4k(vec3 p) { return vec3(cl.origin_4k.x + p.x, cl.origin.y + p.y, cl.origin_4k.y + p.z); }   /* mod 4 km */
vec2 cl_world_full_xz_hi(void) { return cl.origin_split.xy; }  /* full x/z = hi + (lo + p.xz) */
vec2 cl_world_full_xz_lo(vec3 p) { return cl.origin_split.zw + p.xz; }
float cl_alt(vec3 p) { return cl.origin.y + p.y; }
vec3 cl_eye(int eye) { return cl.eye_m[eye].xyz; }
/* view-space ray (unnormalised, z = -1) of a target pixel position (pixel centres at +0.5) */
vec3 cl_view_ray(vec2 px) { return vec3((px.x - cl.proj.z) / cl.proj.x, -(px.y - cl.proj.w) / cl.proj.y, -1.0); }
vec3 cl_view_to_world(vec3 v) { return cl.cam_x.xyz * v.x + cl.cam_y.xyz * v.y + cl.cam_z.xyz * v.z; }
vec3 cl_world_dir(vec2 px) { return normalize(cl_view_to_world(cl_view_ray(px))); }
/* D32 depth value -> view depth in metres along the view axis; CL_INF where nothing was drawn (cleared to 0) */
float cl_view_depth_m(float z) {
    float d = cl.depth.x * (z * cl.depth.w + cl.depth.y);
    return (z > 0.0 && d > 0.0) ? cl.depth.z / d / cl.misc.x : CL_INF;
}
int cl_depth_layer(int eye) { return cl.sizes2.z > 1.5 ? eye : 0; }
/* scene distance along the pixel's ray (m), CL_INF for sky */
float cl_scene_dist_m(ivec2 px, int eye) {
    float z = texelFetch(cl_scene_depth, ivec3(px, cl_depth_layer(eye)), 0).r;
    float vd = cl_view_depth_m(z);
    return vd >= CL_INF ? CL_INF : vd * length(cl_view_ray(vec2(px) + 0.5));
}

/* ---- C4: weather and density (one model for near and far) -- S2's model, 2026-10-20 ----
   The weather map (C3) holds the splat of the game's cloud list (S2's weather pass): R = the blob's horizontal profile
   (C1 radius_m = the body's horizontal semi-axis, the game impostor's 50 % alpha: full to 0.45, gone at 0.9 of it),
   G/B = the blob's local base / top (0..1 in the C1 band: the ellipsoid chord from C1 height_m, the vertical
   semi-axis; cumulus tops and outlines billowed by a core plus dome-topped puffs inside it), A = density (C1
   opacity, eased across list changes, normalised per cloud to a vertical optical depth of ACZ_VR_CLOUD_OD through
   its centre). Without a cloud list the procedural field fills R (G 0, B 1, A 1).
   The density is a noise-displaced blob: e = R x the vertical profile over the local column (a splatted cloud: soft
   ends only; the procedural band: the C1 type's profile); the noise must
   clear 1 - e, so the core is solid and only billow peaks survive toward the surface (the volume analogue of the
   game's impostors, each built from 64 puff sprites). Shape parameters (cl.user[0], S5 per mission):
     x base-noise period (m; must divide 60000), y detail-noise period (m; must divide 60000),
     z detail erosion 0..1, w billow weight (0 Perlin-Worley .. 1 Worley billows).
   ps2_clouds.c defaults when user[0].x == 0: 4000, 600, 0.3, 0.5. */
float cl_remap(float v, float l0, float h0, float l1, float h1) { return l1 + (v - l0) * (h1 - l1) / (h0 - l0); }
float cl_hash12(vec2 p) { vec3 p3 = fract(vec3(p.xyx) * 0.1031); p3 += dot(p3, p3.yzx + 33.33); return fract((p3.x + p3.y) * p3.z); }
/* R coverage (horizontal blob profile), G local base, B local top (0..1 in the C1 band), A density; sampled by
   cloud-frame position (REPEAT, toroidal). Type is global: cl.band.w (0 stratus .. 1 cumulus).
   ACZ_VR_CLOUD_WX_SQRT (cl_pop 2026-10-31, bug #260; cl.wx.w >= 16): the 8-bit A holds sqrt(density). The splat's
   density of a big stratus body is small (its vertical optical depth normalised to 3 over a 2-3 km chord: M10's clouds
   0.067-0.13 at opacity 1 = 17-33 of 255 levels, M1's overcast 9-18), so linear 8 bits stepped a cloud the list fades in
   (to 0.14) 0 -> 1 -> 2 -> 3 levels: over its 3.6 km chord 0 -> 25 -> 43 -> 57 % visible opacity in three jumps (M10
   11:26:27 "a cloud appeared out of nowhere"). sqrt: the first level is 1.5e-5, the largest jump of that fade 5.4 %. */
float cl_wx_enc(float d) { return cl.wx.w >= 15.5 ? sqrt(max(d, 0.0)) : d; }
vec4 cl_weather(vec3 p_m) {
    vec4 w = textureLod(cl_weather_tex, cl.wx.xy + p_m.xz * cl.wx.z, 0.0);
    if (cl.wx.w >= 15.5) w.a *= w.a;
    return w;
}
/* 0..1 inside the C1 band, < 0 or > 1 outside */
float cl_height_frac(vec3 p_m) { return (cl_alt(p_m) - cl.band.x) / max(cl.band.y - cl.band.x, 1.0); }
/* vertical profile over the local column by type: 0 stratus (flat), 1 cumulus (flat base, rounded top) */
float cl_profile(float hl, float type) {
    float top = mix(0.45, 1.0, type);
    float bot = mix(0.10, 0.06, type);
    return smoothstep(0.0, bot, hl) * (1.0 - smoothstep(top * mix(0.4, 0.3, type), top, hl));
}
vec4 cl_shape_params() { return cl.user[0].x > 0.0 ? cl.user[0] : vec4(4000.0, 600.0, 0.3, 0.5); }
/* density at p_m (cloud space) for a given weather sample wx (cl_weather(p_m), or the far renderer's own beyond the
   weather window). step_m = the caller's integration step (m; 0 = full detail): the noise is read from the mip level
   whose texels are about half the step, so a coarse far step integrates band-limited noise instead of aliasing it
   into grain or slices (the noise textures carry mip chains; S3's near steps stay at level 0).
   The reads and the math are split (S2, 2026-10-21) so a latency-bound caller can issue all its reads at once:
   cl_noise_base_ws / cl_noise_detail_ws = the two noise reads, cl_density_tx = the density from given texels
   (dn is only used with detail); cl_density_ws = the column test, the reads, cl_density_tx. */
bool cl_in_column(vec3 p_m, vec4 wx) {
    float h = cl_height_frac(p_m);
    if (h <= 0.0 || h >= 1.0 || wx.r < 0.01 || wx.a <= 0.0) return false;
    float hl = (h - wx.g) / max(wx.b - wx.g, 1.0e-3);
    return hl > 0.0 && hl < 1.0;
}
vec4 cl_noise_base_ws(vec3 p_m, float step_m) {
    vec4 sp = cl_shape_params();
    return textureLod(cl_noise_base, cl_world(p_m) / sp.x, max(log2(max(step_m, 1.0e-3) * 0.5 * 128.0 / sp.x), 0.0));
}
vec4 cl_noise_detail_ws(vec3 p_m, float step_m) {
    vec4 sp = cl_shape_params();
    return textureLod(cl_noise_detail, cl_world(p_m) / sp.y, max(log2(max(step_m, 1.0e-3) * 0.5 * 32.0 / sp.y), 0.0));
}
float cl_density_tx(vec3 p_m, vec4 wx, vec4 b, vec4 dn, bool detail) {
    float h = cl_height_frac(p_m);
    if (h <= 0.0 || h >= 1.0 || wx.r < 0.01 || wx.a <= 0.0) return 0.0;
    float hl = (h - wx.g) / max(wx.b - wx.g, 1.0e-3);
    if (hl <= 0.0 || hl >= 1.0) return 0.0;
    /* the C1 type shapes the procedural band; a splatted cloud (C1 list) is shaped by its own blob (G/B = its chord),
       so it only gets soft ends */
    float prof = cl.fogr.z > 0.5 ? smoothstep(0.0, 0.12, hl) * (1.0 - smoothstep(0.75, 1.0, hl)) : cl_profile(hl, cl.band.w);
    float e = wx.r * prof;
    if (e <= 0.0) return 0.0;
    vec4 sp = cl_shape_params();
    float wfbm = b.g * 0.625 + b.b * 0.25 + b.a * 0.125;              /* billows (inverted Worley fBm) */
    float pw = cl_remap(b.r, wfbm - 1.0, 1.0, 0.0, 1.0);                /* Perlin-Worley eroded by the billows */
    float c = clamp(cl_remap(mix(pw, wfbm, sp.w), 1.0 - e, 1.0, 0.0, 1.0), 0.0, 1.0);
    if (c <= 0.0 || !detail) return c * wx.a;
    float dfbm = dn.r * 0.625 + dn.g * 0.25 + dn.b * 0.125;
    float m = mix(dfbm, 1.0 - dfbm, clamp(hl * 4.0, 0.0, 1.0));        /* wispy at the base, billowy above */
    return clamp(cl_remap(c, m * sp.z, 1.0, 0.0, 1.0), 0.0, 1.0) * wx.a;
}
float cl_density_ws(vec3 p_m, vec4 wx, bool detail, float step_m) {
    if (!cl_in_column(p_m, wx)) return 0.0;                            /* no noise reads outside the column */
    return cl_density_tx(p_m, wx, cl_noise_base_ws(p_m, step_m), detail ? cl_noise_detail_ws(p_m, step_m) : vec4(0.0), detail);
}
float cl_density_w(vec3 p_m, vec4 wx, bool detail) { return cl_density_ws(p_m, wx, detail, 0.0); }
float cl_density(vec3 p_m, bool detail) { return cl_density_ws(p_m, cl_weather(p_m), detail, 0.0); }

/* ---- C4 shading: one colour model for near and far (S2 + S3, 2026-10-20; from S5's measurement of the game) ----
   The game fills each cloud impostor with base = mix(DARK, LIGHT, t) and draws two enlarged copies over it: a DARK
   copy shifted down (and away from the sun) at strength aD, then a LIGHT copy shifted up (and toward the sun) at
   strength aL. t, aL and aD follow c = cos(camera forward, sun); mission-1 fits: t = 0.578 + 0.429 c,
   aL = max(0, 0.291 - 0.598 c), aD = max(0, 0.322 + 0.562 c). Result: lit tops, dark bases, a tone brightening toward
   the sun. Here c is per ray (the game's per-frame value would recolour the clouds as the head turns). In the volume:
   the DARK copy covers a point when there is cloud above it (`above`, from the shared probe below), the LIGHT copy
   when there is cloud below it (the local column height hl). On top (S3): thin parts lighter, dense cores darker, a
   little darker toward the local base. Then the game's distance haze. Everything here colours a sample at the 600 m
   hand-over, so near and far must both use it unchanged; side-specific extras (S3's in-cloud pull) stay near-only.
   Colours in slot-0 space. cl.user[1] (S5 per mission; all zero = the defaults (0.15, 0.5, 0.25, 1)):
     x  base darkening: t -= x (1 - hl)
     y  light rim on thin parts: mix toward LIGHT by y (1 - smoothstep(0.05, 0.45, dens))
     z  dark core: mix toward DARK by z smoothstep(0.15, 0.6, dens);   w  strength of the DARK copy (the probe) */
float cl_local_h(vec3 p_m, vec4 wx) { return clamp((cl_height_frac(p_m) - wx.g) / max(wx.b - wx.g, 1.0e-3), 0.0, 1.0); }
vec4 cl_shade_params() { vec4 u = cl.user[1]; return any(notEqual(u, vec4(0.0))) ? u : vec4(0.15, 0.5, 0.25, 1.0); }
/* S5's per-mission constants of the curves (cl.user[3] = C1 art_k = (t_a, t_b, aL_a, aD_a); all zero = the mission-1
   fits): t = t_a + |t_b| c', aL = aL_a - 0.598 c', aD = aD_a + 0.562 c' (the slopes are fixed), c' = s c with
   s = sign(t_b). At night the game lights its clouds from the side opposite the disc C1 sun_dir points at, with a lower
   t: S5 publishes t_b < 0 there (mission 11: 0.404, -0.407, 0.319, 0.318), which flips the light direction for the
   curves and for the "cloud above" probe's tilt. Day missions publish t_b > 0 (unchanged). */
vec4 cl_art_k() { vec4 k = cl.user[3]; return any(notEqual(k, vec4(0.0))) ? k : vec4(0.578, 0.429, 0.291, 0.322); }
/* extinction per unit density (1/m): C1 transparency lowers it */
float cl_extinction() { return 0.02 * (1.0 - clamp(cl.env.x, 0.0, 1.0) * 0.6); }
/* The "cloud above" probe (the game's DARK copy is shifted down 0.14 of the cloud size and away from the sun): one
   density sample up, tilted toward the sun, a quarter of the band thickness away. cl_probe_pos gives the point; the
   caller fetches the weather there (the far march has its own beyond the window) and calls cl_above. step_m = the
   caller's step (noise prefilter, 0 = full detail). cl_above_probe = both with the window's weather (near). */
float cl_light_side() { return cl_art_k().y < 0.0 ? -1.0 : 1.0; }   /* -1: lit from opposite sun_dir (night) */
vec3 cl_probe_pos(vec3 p_m) {
    vec3 dir = normalize(vec3(0.0, 1.0, 0.0) + vec3(cl.sun.x, 0.0, cl.sun.z) * (0.57 * cl_light_side()));
    return p_m + dir * (0.25 * max(cl.band.y - cl.band.x, 1.0));
}
/* The cloud shadow map (ACZ_VR_CLOUD_SHADOW, 2026-10-21; cloud_shadow.glsl): the optical depth toward the light through
   every cloud above a point (the rest of its own body, overhangs, clouds kilometres away along the light ray).
   cl_shadow_od = the scaled optical depth and the map's weight (1 inside, fading to 0 at its edge; 0 when off);
   cl_sun_shadow = the plain transmittance (1 = full sun), the hook terrain and fog use with their own binding. */
#include "cloud_shadow.glsl"
float cl_shadow_od(vec3 p_m, out float weight) { return cl_shadow_od_map(cl_shadow_tex, cl.shd[0], cl.shd[1], cl.shd[2], p_m, weight); }
float cl_sun_shadow(vec3 p_m) { return cl_sun_shadow_map(cl_shadow_tex, cl.shd[0], cl.shd[1], cl.shd[2], p_m); }
float cl_above_d(float dq) {                                           /* dq = the density at the probe point */
    float od = dq * 0.015 * 0.25 * max(cl.band.y - cl.band.x, 1.0);
    return 1.0 - clamp((exp(-od) - 0.5) * 2.0 + 0.5, 0.0, 1.0);
}
float cl_above(vec3 q_m, vec4 wq, float step_m) { return cl_above_d(cl_density_ws(q_m, wq, false, step_m)); }
float cl_above_probe(vec3 p_m, float step_m) { vec3 q = cl_probe_pos(p_m); return cl_above(q, cl_weather(q), step_m); }
/* The cast shadow (the shadow map's long range): the light's transmittance from the probe point q on toward the light
   -- through the rest of the column above, overhangs, clouds behind clouds kilometres away (multiple-scattering
   softened). The probe's own sample still stands for the stretch p -> q (the game's DARK copy with S5's puffy local
   detail), so the two do not count the same cloud twice. 1 = none (also beyond the map's edge and without a map).
   Evaluated with the probe and held like it (the far march every 2nd fine step, the near volume every 4th plane). */
float cl_cast_t(vec3 q_m) { float w, od = cl_shadow_od(q_m, w); return w > 0.0 ? mix(1.0, cl_shadow_t(od), w) : 1.0; }
/* p_m: the sample (cloud space; its distance from the centre eye drives the haze), rd: the view ray (world, unit),
   dens: the local density (cl_density value), hl: cl_local_h, above: cl_above (0..1), sun_t: cl_cast_t (1 = none).
   The cast shadow takes the sun's share away from whatever the game's curves made of the sample: it pulls the colour
   toward the cloud's ambient, a point on the same DARK -> LIGHT palette line (shd[3].y, 0.3: DARK is the zenith sky
   colour, the ambient a cloud scatters is lighter), by shd[3].x (0.5) x (1 - sun_t). View-independent (the game's DARK
   copy fades out looking away from the sun; a cloud in another's shadow stays in it), palette-bound, before the haze. */
vec3 cl_shade_s(vec3 p_m, vec3 rd, float dens, float hl, float above, float sun_t) {
    if (cl.shd[2].w < 0.0)                                                     /* ACZ_VR_CLOUD_SHADOW_VIEW (debug) */
        return vec3(1.0 - above, 1.0 - 0.5 * above, 1.0 - above) * mix(0.2, 1.0, sun_t);
    vec4 a = cl_shade_params(), k = cl_art_k();
    float c = clamp(dot(rd, cl.sun.xyz), -1.0, 1.0) * (k.y < 0.0 ? -1.0 : 1.0);
    float t = clamp(k.x + abs(k.y) * c - a.x * (1.0 - hl), 0.0, 1.0);
    float aL = clamp(k.z - 0.598 * c, 0.0, 1.0), aD = clamp(k.w + 0.562 * c, 0.0, 1.0);
    vec3 col = mix(cl.shadow.rgb, cl.lit.rgb, t);
    col = mix(col, cl.shadow.rgb, aD * a.w * clamp(above, 0.0, 1.0));          /* DARK copy: cloud above */
    col = mix(col, cl.lit.rgb, aL * smoothstep(0.0, 0.25, hl));                /* LIGHT copy: cloud below */
    col = mix(col, cl.lit.rgb, a.y * (1.0 - smoothstep(0.05, 0.45, dens)));    /* thin parts lighter */
    col = mix(col, cl.shadow.rgb, a.z * smoothstep(0.15, 0.6, dens));          /* dense core darker */
    if (sun_t < 1.0) col = mix(col, mix(cl.shadow.rgb, cl.lit.rgb, cl.shd[3].y), cl.shd[3].x * (1.0 - sun_t));   /* cast */
    float fr = max(cl.fogr.y - cl.fogr.x, 1.0);
    return mix(col, cl.fog.rgb, 0.85 * clamp((length(p_m) - cl.fogr.x) / fr, 0.0, 1.0));   /* the game's haze */
}
vec3 cl_shade(vec3 p_m, vec3 rd, float dens, float hl, float above) { return cl_shade_s(p_m, rd, dens, hl, above, 1.0); }

/* ---- flashes (wish #83, 2026-10-23): launches, explosions, the gun and the afterburner light the cloud ----
   Point lights (cl.flash, up to 4, ps2_video.c flash_frame): falloff R^2 / (R^2 + r^2) with R = the distance where the
   light has halved. Near march (cloud_near.comp sample_at): the in-scattered colour of a sample, added to its shaded
   colour (it is integrated with the sample's density like the rest); the coarse shadow takes the sample's own density
   as the medium between it and the light over at most 4 R (a dense core far from the light gets little, the thin rim
   facing it most), softened like multiple scattering (an octave at 0.3). Far composite (cloud_comp.frag): at the map's
   mean cloud distance along the ray, times the cloud's opacity; a light more than R beyond that point (behind the
   cloud) is dimmed to 0.35. */
float cl_flash_n() { return cl.flash[0].x; }
vec3 cl_flash_light(vec3 p_m, float dens) {
    vec3 s = vec3(0.0);
    for (int i = 0; i < 4; i++) {
        if (float(i) >= cl.flash[0].x) break;
        vec4 a = cl.flash[1 + 2 * i], c = cl.flash[2 + 2 * i];
        vec3 d = p_m - a.xyz;
        float r2 = dot(d, d), R2 = a.w * a.w;
        float od = dens * cl_extinction() * min(sqrt(r2), 4.0 * a.w) * 0.5;
        s += c.rgb * (R2 / (R2 + r2)) * max(exp(-od), 0.3 * exp(-0.3 * od));
    }
    return min(s, vec3(0.8));                     /* (a sample next to the light: never a white blot) */
}
vec3 cl_flash_far(vec3 p_m, vec3 rd) {
    vec3 s = vec3(0.0);
    for (int i = 0; i < 4; i++) {
        if (float(i) >= cl.flash[0].x) break;
        vec4 a = cl.flash[1 + 2 * i], c = cl.flash[2 + 2 * i];
        vec3 d = a.xyz - p_m;
        float r2 = dot(d, d), R2 = a.w * a.w;
        s += c.rgb * (R2 / (R2 + r2)) * (1.0 - 0.65 * smoothstep(0.0, 1.0, dot(d, rd) / max(a.w, 1.0)));
    }
    return min(s, vec3(0.8));
}

/* ---- optical phenomena (ACZ_VR_OPTICS, 2026-10-21): halo, glory, Brocken spectre, rainbow (cloud_optics.glsl) ---- */
#include "cloud_optics.glsl"
