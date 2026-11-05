/* terrain_detail.glsl -- near-ground materials (realistic terrain stage T3+, bug #78; zero_terrain_mat.c, ps2_terrain.h,
   ACZ_TERRAIN_MATERIALS), included by mesh.frag in its -DTDETAIL variants (mesh_det*.frag, mesh_watdet*.frag), which
   only the terrain's first pass of patches near the eye uses (block pad4 bit 24).

   Inputs: the atlas texel (the 2006 photo's colour, for the classification), the fragment relative to the frame's
   centre eye (v_wrel.xyz, units), its world position (v_wabs, units) and per vertex (v_tmat) the smooth ground's slope
   (T2 bake surface), the slow strength variation and the sun light gain of the detail slope. Block fields
   (zero_terrain_mat.c): l[0] = towards the sun, strength; l[1] = full-strength distance, 1 / fade length (units), snow
   mission, debug view + 16 x cost bits; r[0..3] = layers 0-3 and l[2] = layer 4: (-, slope scale, albedo scale,
   2^mip bias).

   The texture array td_tex: per material layer (0 grass / fields, 1 sand, 2 rock / scree, 3 snow, 4 soil) one 32 m
   tile, RGBA8 = detail slope x, z (per texel, normalised), height, albedo variation; 128 = 0 everywhere and box-filtered
   mips, so a footprint larger than the detail averages it to nothing (frequency-limited by the pixel footprint; no TAA
   needed). One fetch per material (both scales are in the tile).

   Result: the colour times (1 + albedo detail) x (1 + the sun light of the detail slope, linear in the slope: its mean
   over the texture is exactly 0), blended toward 1 by strength x distance fade: the atlas colour's hue and local mean
   stay; everything is world-fixed (world XZ mapping, distance from the centre eye) and the same in both eyes. */
#ifndef TERRAIN_DETAIL_GLSL
#define TERRAIN_DETAIL_GLSL
layout(set = 2, binding = 5) uniform sampler2DArray td_tex;
#ifdef TERRAIN_WATER_GLSL
#define TD_BLK wat_b[pc.mesh_block]
#else
struct TdBlock {                         /* = ps2_vk_mesh_block (mesh.vert Block), std430 stride 480 */
    vec4 m[4]; vec4 c[4]; vec4 l[3]; vec4 r[4]; vec4 e[3];
    vec4 f0; vec4 f1; uvec4 u0; uvec4 u1; vec4 p[4]; vec4 mp[4];
};
layout(std430, set = 2, binding = 0) readonly buffer TdBlocks { TdBlock td_b[]; };
#define TD_BLK td_b[pc.mesh_block]
#endif

#define TD_TILE (32.0 * 6.0)              /* every layer: one 32 m tile (zero_terrain_mat.c TM_TILE_M, 6 units/m) */

/* a layer's texel decoded to -1..1: detail slope x, z (per texel, normalised), height, albedo variation */
vec4 td_dec(vec4 t) { return t * (255.0 / 127.0) - (128.0 / 127.0); }
vec4 td_layer(int i) { return i < 4 ? TD_BLK.r[i] : TD_BLK.l[2]; }   /* -, slope scale, albedo scale, 2^mip bias */

/* col = the shaded colour before the fog; atlas = the atlas texel; rel = the fragment relative to the eye (units);
   tm = the smooth ground's slope, the slow strength variation, the sun light gain of the detail slope (mesh.vert);
   wmask = the water share of the texel (0 on land). Everything up to the first fetch runs for every fragment (uniform
   control flow: its implicit derivatives -- one uv for every material -- choose the mips; a second material in a
   transition takes explicit ones). */
vec3 td_apply(vec3 col, vec3 atlas, vec3 rel, vec3 wp, vec4 tm, float wmask) {
    vec4 L0 = TD_BLK.l[0], L1 = TD_BLK.l[1];
    int dbg = int(L1.w), cost = dbg >> 4, view = dbg & 15;      /* (debug view, ACZ_DEBUG_TERRAIN_MAT_COST bits) */
    if ((cost & 16) != 0) return col;                                              /* (cost hunt: nothing) */
    if (view >= 4) return 0.5 * atlas;                                             /* (debug 4: the atlas texel / 2) */
    vec2 uv = wp.xz * (1.0 / TD_TILE);

    /* classification by the photo's colour, the ground's slope and the mission (weights 0..1) */
    vec3 a = atlas;
    float Y = dot(a, vec3(0.299, 0.587, 0.114)), Yr = 1.0 / max(Y, 0.04);
    float mx = max(a.r, max(a.g, a.b)), mn = min(a.r, min(a.g, a.b));
    float sat = (mx - mn) / max(mx, 0.02);
    float green = clamp((a.g - max(a.r, a.b)) * Yr * 8.3, 0.0, 1.0);                 /* (chroma relative to the */
    float warm = clamp(((a.r - a.b) * Yr - 0.20) * 4.0, 0.0, 1.0);                     /*  brightness) */
    float wn = L1.z * clamp((Y - 0.48) * 5.5, 0.0, 1.0) * clamp((0.22 - sat) * 7.0, 0.0, 1.0);   /* snow missions */
    float grey = clamp((0.25 - sat) * 6.7, 0.0, 1.0) * clamp((0.42 - Y) * 5.0, 0.0, 1.0);
    float wr = max(clamp((tm.x - 0.18) * 5.5, 0.0, 1.0),                                 /* rock: steep, or grey on */
                   0.8 * grey * (1.0 - green) * max(L1.z, clamp((tm.x - 0.08) * 8.0, 0.0, 1.0)));   /* a slope / snow */
    float ws = warm * clamp((Y - 0.42) * 6.2, 0.0, 1.0) * clamp((sat - 0.15) * 6.7, 0.0, 1.0)    /* sand: bright */
             * clamp((0.42 - sat) * 10.0, 0.0, 1.0) * (1.0 - green);          /* beige (not red earth: soil) */
    float wg = green;
    float wo = (0.35 + 0.65 * clamp((0.30 - Y) * 5.0, 0.0, 1.0) * (1.0 - green)) * (1.0 - max(wg, ws));
    float keep = 1.0 - wn;
    /* flat grey outside the snow missions: paved ground (runways' surroundings, roads, ports) -- a finer touch */
    float paved = 1.0 - 0.6 * grey * (1.0 - L1.z) * (1.0 - clamp((tm.x - 0.08) * 8.0, 0.0, 1.0));
    wg *= keep * (1.0 - 0.8 * wr); ws *= keep * (1.0 - 0.6 * wr); wo *= keep * (1.0 - 0.8 * wr);
    /* the two strongest (no arrays: registers only) */
    int i1 = 0; float w1 = wg;
    if (ws > w1) { i1 = 1; w1 = ws; }
    if (wr > w1) { i1 = 2; w1 = wr; }
    if (wn > w1) { i1 = 3; w1 = wn; }
    if (wo > w1) { i1 = 4; w1 = wo; }
    int i2 = -1; float w2 = 0.0;
    if (i1 != 0 && wg > w2) { i2 = 0; w2 = wg; }
    if (i1 != 1 && ws > w2) { i2 = 1; w2 = ws; }
    if (i1 != 2 && wr > w2) { i2 = 2; w2 = wr; }
    if (i1 != 3 && wn > w2) { i2 = 3; w2 = wn; }
    if (i1 != 4 && wo > w2) { i2 = 4; w2 = wo; }
    vec4 P1 = td_layer(i1);
    vec4 t1 = (cost & 8) != 0 ? vec4(128.0 / 255.0) : texture(td_tex, vec3(uv, float(i1)), log2(P1.w));

    float f = clamp(1.0 - (length(rel) - L1.x) * L1.y, 0.0, 1.0);
    float fade = f * f * (3.0 - 2.0 * f);
    if (view == 3) return mix(col, vec3(fade, 0.5 * fade, 0.0), 0.7);            /* (debug 3: the distance fade) */
    float k = L0.w * fade * tm.y * (1.0 - wmask) * paved;
    if (k < 0.003) return col;
    vec4 d1 = td_dec(t1);
    vec2 s = d1.xy * P1.y;
    float alb = d1.w * P1.z;
    if (w2 > 0.2 * w1 && (cost & 9) == 0) {                     /* a second material: height blend, narrow band */
        vec4 P2 = td_layer(i2);
        vec2 gx = dFdx(uv) * P2.w, gy = dFdy(uv) * P2.w;
        vec4 d2 = td_dec(textureGrad(td_tex, vec3(uv, float(i2)), gx, gy));
        float n1 = w1 / (w1 + w2);
        float e1 = d1.z * 0.5 + 2.0 * n1, e2 = d2.z * 0.5 + 2.0 * (1.0 - n1), top = max(e1, e2) - 0.35;
        float b1 = max(e1 - top, 0.0), b2 = max(e2 - top, 0.0);
        float t = b1 / max(b1 + b2, 1e-4);
        s = mix(d2.xy * P2.y, s, t); alb = mix(d2.w * P2.z, alb, t);
    }
    /* the detail slope's sun light relative to the smooth ground's, linear in the slope (mean-neutral); the gain
       (sun share, our cloud shadow, the smooth ground's light) comes per vertex */
    float light = clamp(1.0 - tm.z * dot(s, L0.xz), 0.55, 1.6);
    float m = clamp(1.0 + alb, 0.6, 1.45) * light;
    if (view == 2) return vec3(0.5 * m);                                          /* (debug 2: the multiplier) */
    if (view == 1) {                                                               /* (debug 1: the materials) */
        vec3 c1 = i1 == 0 ? vec3(0.2, 0.9, 0.2) : i1 == 1 ? vec3(1.0, 0.85, 0.2) : i1 == 2 ? vec3(0.45, 0.5, 0.8)
                : i1 == 3 ? vec3(1.0) : vec3(0.6, 0.35, 0.15);
        return mix(col, c1, 0.65 * fade);
    }
    return col * (1.0 + k * (m - 1.0));
}
#endif
