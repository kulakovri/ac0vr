/* terrain_water.glsl -- shader water (bug #63; zero_water.c, ps2_water.h, ACZ_TERRAIN_WATER), included by mesh.frag.

   The terrain's first pass (mesh.vert prog 1 pass 0) of a patch whose tile carries water passes: v_wat = the tile
   texel position (0..1) for the mask, the mask layer (tile id) and the tile's class (1 partial: fetch the mask, 2 full);
   v_wrel = the fragment relative to the frame's centre eye (game units) and our clouds' sun transmittance there; v_wabs
   = its world position (units). The frame's parameters are in the reserved block WAT_BLOCK (ps2_video.c wat_frame,
   zero_water.c zero_water_frame_block):
     m[0], m[1]: wave layer A: world x/z -> texture uv (2 x 2, rows), then (offset uv, RMS slope, 0)
     m[2], m[3]: wave layer B, the same
     r[0], r[1]: the layers' rotations back into world axes (2 x 2, rows)
     c[0] = (towards the sun xyz, sun up 0..1), c[1] = (sun glitter colour x strength, unresolved slope variance),
     c[2] = (the game's sky colour at the zenith, reflection strength), c[3] = (the game's horizon sky colour, F0)
     l[0] = (fade start (units), 1 / fade length, debug tint, 0), l[1] = (cloud band base, top (units), cloud reflection
     strength, 0), l[2] = (a cloud's underside colour, strength: 1, less while the frame-time governor fades the water)
   Everything is world-fixed (the waves by world position, the view from the centre eye), so both eyes and the frames
   drawn between game frames agree; the time only moves the layers' offsets. The base stays the game's own colour (the
   photo texel x the vertex colour with relief and cloud shadows); the water only adds the reflection of the game's sky
   (Fresnel by the facet's angle), the reflection of our clouds' underside (through the cloud shadow map, by the
   reflected ray's crossing of the band base) and the sun's glitter (Cox-Munk: the share of wave facets whose slope
   mirrors the sun into the eye, with the slope variance the filtered waves hide, so far water gets a broad sheen, not
   sparkles), all faded out with distance to the game's flat look. */
#ifndef TERRAIN_WATER_GLSL
#define TERRAIN_WATER_GLSL
#include "cloud_shadow.glsl"

struct WatBlock {                        /* = ps2_vk_mesh_block (mesh.vert Block), std430 stride 480 */
    vec4 m[4]; vec4 c[4]; vec4 l[3]; vec4 r[4]; vec4 e[3];
    vec4 f0; vec4 f1; uvec4 u0; uvec4 u1; vec4 p[4]; vec4 mp[4];
};
layout(std430, set = 2, binding = 0) readonly buffer WatBlocks { WatBlock wat_b[]; };
layout(set = 2, binding = 2) uniform sampler2D wat_cl_map;      /* the ground's copy of our cloud shadow map (gsh_*) */
layout(set = 2, binding = 3) uniform sampler2DArray wat_mask;    /* the tiles' water masks */
layout(set = 2, binding = 4) uniform sampler2D wat_waves;        /* slopes s_x / 8 + 0.5, s_z / 8 + 0.5, s_x^2 / 16, s_z^2 / 16 */
#define WAT_BLOCK 16381u                 /* = ps2_video.c MESH_WATER_BLOCK */
#define WAT_GSH_BLOCK 16382u             /* = MESH_GSHADOW_BLOCK (the cloud shadow map's parameters) */

float wat_fresnel(float f0, float c) { float x = 1.0 - clamp(c, 0.0, 1.0); float x2 = x * x; return f0 + (1.0 - f0) * x2 * x2 * x; }

vec3 water_shade(vec3 base, vec4 wat, vec4 rel, vec3 wabs, bool clouds) {
    float m = wat.w > 1.5 ? 1.0 : texture(wat_mask, vec3(wat.xy, wat.z)).r;
    if (m < 0.004) return base;
    vec4 L0 = wat_b[WAT_BLOCK].l[0];
    float d = length(rel.xyz);
    float fade = m * (1.0 - clamp((d - L0.x) * L0.y, 0.0, 1.0)) * wat_b[WAT_BLOCK].l[2].w;
    if (L0.z > 0.5 && L0.z < 1.5) return mix(base, vec3(0.1, 0.35, 1.0), 0.6 * m);     /* (debug: the mask) */
    if (fade < 0.004) return base;
    vec3 V = -rel.xyz / max(d, 1e-3);                     /* towards the eye */
    /* waves: two layers of the slope texture */
    vec4 MA = wat_b[WAT_BLOCK].m[0], OA = wat_b[WAT_BLOCK].m[1], MB = wat_b[WAT_BLOCK].m[2], OB = wat_b[WAT_BLOCK].m[3];
    vec4 RA = wat_b[WAT_BLOCK].r[0], RB = wat_b[WAT_BLOCK].r[1];
    const vec4 dk = vec4(8.0, 8.0, 16.0, 16.0), db = vec4(-4.0, -4.0, 0.0, 0.0);
    vec4 ta = vec4(0.0), tb = vec4(0.0);
    if (L0.w < 3.5) {                                     /* (debug ACZ_DEBUG_WATER_COST & 4: flat, no wave fetch) */
        ta = texture(wat_waves, vec2(dot(MA.xy, wabs.xz), dot(MA.zw, wabs.xz)) + OA.xy) * dk + db;
        tb = texture(wat_waves, vec2(dot(MB.xy, wabs.xz), dot(MB.zw, wabs.xz)) + OB.xy) * dk + db;
    }
    vec2 s = (vec2(dot(RA.xy, ta.xy), dot(RA.zw, ta.xy)) * OA.z + vec2(dot(RB.xy, tb.xy), dot(RB.zw, tb.xy)) * OB.z) * fade;
    float va = max(ta.z + ta.w - dot(ta.xy, ta.xy), 0.0) * OA.z * OA.z, vb = max(tb.z + tb.w - dot(tb.xy, tb.xy), 0.0) * OB.z * OB.z;
    vec4 C1 = wat_b[WAT_BLOCK].c[1];
    float var = 0.5 * (va + vb) * fade * fade + C1.w;     /* per-axis slope variance the pixel hides */
    vec3 n = normalize(vec3(-s.x, 1.0, -s.y));
    if (L0.z > 1.5) return mix(base, n * 0.5 + 0.5, fade);                          /* (debug: the normals) */
    vec4 C2 = wat_b[WAT_BLOCK].c[2], C3 = wat_b[WAT_BLOCK].c[3];
    float F = wat_fresnel(C3.w, dot(n, V));
    vec3 R = reflect(-V, n);
    R.y = abs(R.y);
    vec3 sky = mix(C3.rgb, C2.rgb, sqrt(clamp(R.y, 0.0, 1.0)));
    vec4 L1 = wat_b[WAT_BLOCK].l[1];
    if (clouds && L1.z > 0.0 && R.y > 0.03) {             /* our clouds' underside, where the reflected ray meets the band */
        vec4 s0 = wat_b[WAT_GSH_BLOCK].p[0], s1 = wat_b[WAT_GSH_BLOCK].p[1], s2 = wat_b[WAT_GSH_BLOCK].p[2];
        vec4 o = wat_b[WAT_GSH_BLOCK].p[3];
        vec3 pm = wabs * o.w - o.xyz;                     /* metres from the centre eye */
        float th = (s1.z - (s1.w + pm.y)) / R.y;
        if (th > 0.0) {
            float w, od = cl_shadow_od_map(wat_cl_map, s0, s1, s2, pm + R * th, w);
            sky = mix(sky, wat_b[WAT_BLOCK].l[2].rgb, (1.0 - cl_shadow_t(od)) * w * L1.z);
        }
    }
    sky *= mix(1.0, rel.w, 0.5);                          /* our cloud shadow on the water dims its reflection a little */
    /* the water body under the reflection (ACZ_TERRAIN_WATER_BODY = r[2].x, 2026-10-23 bug #94): the painted colour is
       the 2006 water as a whole -- body and the reflection of the sky together. Taken as the mean look of flat water
       (the reflection of the sky straight in the mirror direction, Fresnel of the unperturbed surface), the body is
       what is left without that reflection: darker, so each wave facet's own reflection (by its Fresnel and its part
       of the sky) varies around the painted colour instead of adding a thin veil over it -- flat water stays the
       painted colour; at grazing distance the reflection takes over (body floored at 0.3 x the painted colour). */
    vec3 body = base;
    float kb = wat_b[WAT_BLOCK].r[2].x;
    if (kb > 0.0) {
        float k0 = clamp(wat_fresnel(C3.w, V.y) * C2.w, 0.0, 0.9);
        vec3 sky0 = mix(C3.rgb, C2.rgb, sqrt(clamp(V.y, 0.0, 1.0))) * mix(1.0, rel.w, 0.5);
        body = mix(base, max((base - k0 * sky0) / (1.0 - k0), 0.3 * base), kb);
    }
    vec3 col = mix(body, sky, clamp(F * C2.w, 0.0, 1.0));
    vec4 C0 = wat_b[WAT_BLOCK].c[0];
    if (C0.w > 0.0) {                                     /* the sun's glitter */
        vec3 H = normalize(C0.xyz + V);
        float hy = max(H.y, 0.1);
        vec2 dd = -H.xz / hy - s;
        float D = exp(-0.5 * dot(dd, dd) / var) / (6.2831853 * var);
        float g = wat_fresnel(C3.w, dot(H, V)) * D / (4.0 * max(V.y, 0.08) * hy * hy * hy * hy);
        col += C1.rgb * min(g, 4.0) * rel.w;
    }
    return mix(base, col, fade);
}
#endif
