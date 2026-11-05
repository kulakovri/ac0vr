#version 450
#ifdef MV
#extension GL_EXT_multiview : require
#endif
layout(location = 0) in vec4 v_color;
layout(location = 1) in vec3 v_stq;
layout(location = 2) in float v_fog;
layout(location = 3) flat in uint v_round_uv;
layout(location = 4) in vec3 v_terr;       /* realistic terrain: relief uv, z = 1 on the terrain's first pass */
layout(location = 5) in vec4 v_wat;        /* shader water (terrain_water.glsl): mask uv, layer, class (0 = none) */
layout(location = 6) in vec4 v_wrel;       /* ... the fragment relative to the centre eye (units), sun transmittance */
layout(location = 7) in vec3 v_wabs;       /* ... its world position (units) */
layout(location = 9) in vec4 v_fac;        /* city facades (zero_props.c): offset from the building's centre, height
                                              above its base (m), kind + seed / 256 (mesh.vert) */
#ifdef CITY
layout(location = 10) in vec4 v_fac2;      /* city, detailed look: wall top / foot, half length / width (m) */
layout(location = 11) in vec3 v_view;      /* ... the fragment relative to the eye (units) */
#endif
#ifdef TDETAIL
layout(location = 8) in vec4 v_tmat;       /* near-ground materials: ground slope, slow variation, light gain */
#endif

layout(location = 0, index = 0) out vec4 out_color;
layout(location = 0, index = 1) out vec4 out_blend;

#ifdef MV
layout(set = 0, binding = 0) uniform sampler2DArray tex;
#else
layout(set = 0, binding = 0) uniform sampler2D tex;
#endif
/* realistic terrain (zero_terrain.c): one light ratio per world texel over the map, 128 = 1, mipmapped; the same in
   both eyes (world-fixed) */
layout(set = 2, binding = 1) uniform sampler2D relief;

layout(push_constant) uniform PC {
    vec2  inv_size;
    vec2  tex_size;
    uvec4 flags;
    vec4  fogcol;
    uvec4 clamp_uv;
    vec2  img_size;
    float scale;
    float point_size;
    float tex_scale;
    float pad0;
    uint  mesh_block;
    uint  mv_flags;        /* multiview: 1 set-0 texture is layered */
} pc;
#ifdef WATER
#include "terrain_water.glsl"      /* (the water variant mesh_wat*.frag.spv: only the terrain's water patches use it) */
#endif
#ifdef TDETAIL
#include "terrain_detail.glsl"     /* (the material variants mesh_det*.frag.spv / mesh_watdet*: terrain patches near the eye) */
#endif
#ifdef TEXPACK
/* T5 terrain texture pack (zero_texpack.c, ps2_texpack.h; the variants mesh*tp*.frag.spv, only for the terrain's first
   pass of patches the pack serves): every tile of the map's set at 2x, layer = tile id, mipmapped (mip 1 = the 2006
   tile bit for bit), clamped to the tile; mesh.vert passes the tile uv and layer in v_wat.xyz */
layout(set = 2, binding = 6) uniform sampler2DArray tp_tex;
#endif
#ifdef MV
#define TL (((pc.mv_flags & 1u) != 0u) ? gl_ViewIndex : 0)
#define TEXSIZE() textureSize(tex, 0).xy
#define FETCH_T(p) texelFetch(tex, ivec3(p, TL), 0)
#define SAMPLE_T(uv) texture(tex, vec3(uv, float(TL)))
#else
#define TEXSIZE() textureSize(tex, 0)
#define FETCH_T(p) texelFetch(tex, p, 0)
#define SAMPLE_T(uv) texture(tex, uv)
#endif
#ifdef ZCENSUS
#include "terrain_zcensus.glsl"      /* (PS2_TERRAIN_ZCENSUS, diagnostic: the variants mesh_zcen*.frag.spv) */
#endif
#ifdef CITY
/* the city's detailed look (ACZ_TERRAIN_CITY_LOOK; the variant mesh_city*.frag.spv, only the city draws use it, with
   push-constant bit 524288). Early depth tests: a building behind another is not shaded. The city draws inherit the
   terrain pass's alpha test (M6: alpha > 0.75) but are opaque buildings on land texels: the variant never discards
   them (the shader writes no depth), so the early tests are exact */
layout(early_fragment_tests) in;
#ifdef MV
#define SAMPLE_TL(uv, l) textureLod(tex, vec3(uv, float(TL)), l)
#else
#define SAMPLE_TL(uv, l) textureLod(tex, uv, l)
#endif
#include "city_look.glsl"
#endif

void main() {
    vec4 c = v_color;
#ifdef ZCENSUS
    zc_begin(pc.mesh_block);
    vec3 zc_tex = vec3(-1.0);
#endif
#ifdef CITY
    /* (the city's detailed look: the texel, its coordinates and the texture function's factor / term) */
    vec4 cl_t = vec4(0.0);
    vec2 cl_uv = vec2(0.0);
    vec3 cl_mod = vec3(1.0), cl_add = vec3(0.0);
#endif
    uint f = pc.flags.x;
    uint tfx = f & 3u;
    bool ate = ((f >> 3) & 1u) != 0u;
    uint atst = (f >> 4) & 7u;
    float aref = float((f >> 8) & 0xFFu) / 128.0;
    bool fge = ((f >> 16) & 1u) != 0u;
    bool tcc = ((f >> 17) & 1u) != 0u;
    bool afkeep = ((f >> 18) & 1u) != 0u;
    bool point = ((f >> 19) & 1u) != 0u;
    bool afonly = ((f >> 20) & 1u) != 0u;
    bool fba = ((f >> 21) & 1u) != 0u;

#ifdef TDETAIL
    vec3 td_atlas = vec3(-1.0);
#endif
    if (pc.flags.y != 0u) {
        vec2 tc = (v_stq.xy / max(v_stq.z, 1e-9)) * pc.tex_size;
        uint wms = pc.flags.z & 3u, wmt = (pc.flags.z >> 2) & 3u;
        bool region_image = ((pc.flags.z >> 8) & 1u) != 0u;
        vec2 org = vec2(0.0);
        if (region_image) {
            if (wms == 2u) org.x = float(pc.clamp_uv.x);
            if (wmt == 2u) org.y = float(pc.clamp_uv.z);
        }
        vec2 uv = (tc - org) / pc.img_size;
        vec2 lo = 0.5 / pc.img_size;
        vec2 hi = (pc.tex_size - 0.5) / pc.img_size;
        if (wms == 2u) {
            lo.x = (float(pc.clamp_uv.x) - org.x + 0.5) / pc.img_size.x;
            hi.x = (float(pc.clamp_uv.y) - org.x + 0.5) / pc.img_size.x;
        }
        if (wmt == 2u) {
            lo.y = (float(pc.clamp_uv.z) - org.y + 0.5) / pc.img_size.y;
            hi.y = (float(pc.clamp_uv.w) - org.y + 0.5) / pc.img_size.y;
        }
        if (wms == 1u || wms == 2u) uv.x = clamp(uv.x, lo.x, hi.x);
        if (wmt == 1u || wmt == 2u) uv.y = clamp(uv.y, lo.y, hi.y);
        vec4 t;
#ifdef TEXPACK
        /* the pack's texel in place of the GS region fetch (implicit derivatives: the same footprint rules as the
           game's region texture, at twice its texels); the near-ground materials still classify by the 2006 texel */
        t = texture(tp_tex, vec3(v_wat.xy, v_wat.z));
#ifdef TDETAIL
        td_atlas = SAMPLE_T(uv).rgb;
#endif
        if ((pc.mv_flags & 65536u) != 0u) t.rgb = mix(t.rgb, vec3(1.0, 0.25, 0.85), 0.35);   /* (ACZ_DEBUG_TEXPACK_TINT) */
#else
        if (point) {
            ivec2 size = TEXSIZE();
            ivec2 p = ivec2(floor(uv * vec2(size)));
            p = ((p % size) + size) % size;
            t = FETCH_T(p);
        } else {
            t = SAMPLE_T(uv);
        }
#ifdef TDETAIL
        td_atlas = t.rgb;
#endif
#endif
#ifdef ZCENSUS
        zc_tex = t.rgb;
#endif
        float af = c.a;
        vec3 rgb;
        float a;
#ifdef CITY
        cl_t = t; cl_uv = uv;
        cl_mod = tfx == 1u ? vec3(1.0) : c.rgb;
        cl_add = tfx >= 2u ? vec3(af * (128.0 / 255.0)) : vec3(0.0);
#endif
        if (tfx == 1u) {
            rgb = t.rgb;
            a   = tcc ? t.a : af;
        } else if (tfx == 0u) {
            rgb = t.rgb * c.rgb;
            a   = tcc ? t.a * af : af;
        } else if (tfx == 2u) {
            rgb = t.rgb * c.rgb + af * (128.0 / 255.0);
            a   = tcc ? t.a + af : af;
        } else {
            rgb = t.rgb * c.rgb + af * (128.0 / 255.0);
            a   = tcc ? t.a : af;
        }
        c = vec4(rgb, a);
    } else {
        c.rgb *= 128.0 / 255.0;
    }

    /* city facades (zero_props.c): the walls share their corners, so their light comes here from the face's own normal
       (the screen derivatives of the world position, turned outward from the building's centre): 0.50 + 0.40 x its
       facing to the sun. Kind 1 adds rows of windows (ACZ_TERRAIN_CITY_WINDOWS), world-fixed (both eyes, in-between
       frames): bays of 3.0-4.2 m and floors of 3.0-3.4 m by the building's seed, the window 46 x 48 % of its bay, 45 %
       darker than the wall; the pattern fades into its mean (10 % darker) where a bay is under ~3 pixels, so far walls
       never shimmer. The detailed look (ACZ_TERRAIN_CITY_LOOK, the CITY variant: city_look.glsl) takes the walls and
       roofs instead */
    vec2 fnh = vec2(0.0);
#ifdef CITY
    bool clook = (pc.mv_flags & 524288u) != 0u && v_fac.w > 0.5;
    if (clook) c.rgb = city_look(c.rgb, cl_t, cl_uv, cl_mod, cl_add, pc.fogcol.rgb, 0.0);
    if (v_fac.w > 0.5 && !clook) {
#else
    if (v_fac.w > 0.5) {                       /* (relative to the building's centre: small numbers, a clean normal) */
#endif
        vec3 fp = vec3(v_fac.x, v_wabs.y, v_fac.y);
        fnh = cross(dFdx(fp), dFdy(fp)).xz;
        float nl = length(fnh);
        fnh = nl > 1e-12 ? fnh / nl : vec2(0.0);
        if (dot(fnh, v_fac.xy) < 0.0) fnh = -fnh;
        c.rgb *= 0.50 + 0.40 * max(dot(fnh, v_wrel.xy), 0.0);
    }
#ifdef CITY
    if (v_fac.w > 0.5 && v_fac.w < 2.0 && !clook) {
#else
    if (v_fac.w > 0.5 && v_fac.w < 2.0) {
#endif
        float sd = fract(v_fac.w);
        vec2 q = vec2(dot(v_fac.xy, vec2(-fnh.y, fnh.x)) * (1.0 / 5.12) / (3.0 + 1.2 * sd),
                      v_fac.z / (3.0 + 0.4 * fract(sd * 7.0)));
        vec2 fq = fract(q), fw = max(fwidth(q), vec2(1e-4));
        float wx = smoothstep(0.27 - fw.x, 0.27 + fw.x, fq.x) - smoothstep(0.73 - fw.x, 0.73 + fw.x, fq.x);
        float wy = smoothstep(0.30 - fw.y, 0.30 + fw.y, fq.y) - smoothstep(0.78 - fw.y, 0.78 + fw.y, fq.y);
        float win = clamp(wx * wy, 0.0, 1.0) * step(0.15, q.y);
        c.rgb *= 1.0 - 0.45 * mix(0.22, win, 1.0 - smoothstep(0.18, 0.35, max(fw.x, fw.y)));
    }

    /* realistic terrain, per pixel (ACZ_TERRAIN_PIXEL=1; the default is per vertex in mesh.vert): the baked relief
       light as a ratio over the game's shading (vertex colour x atlas texel), before the fog so distant ground fades
       exactly as before */
    if (v_terr.z > 0.5) c.rgb *= texture(relief, v_terr.xy).r * (255.0 / 128.0);

    /* near-ground materials (zero_terrain_mat.c, terrain_detail.glsl; push-constant bit 8192): the terrain's first pass
       near the eye, before the water and the fog (water texels keep the water's own look) */
#ifdef TDETAIL
    if (td_atlas.x >= 0.0 && (pc.mv_flags & 8192u) != 0u) {
        float wm = 0.0;
#ifdef WATER
        if (v_wat.w > 0.5 && (pc.mv_flags & 4096u) != 0u)
            wm = v_wat.w > 1.5 ? 1.0 : texture(wat_mask, vec3(v_wat.xy, v_wat.z)).r;
#endif
        c.rgb = td_apply(c.rgb, td_atlas, v_wrel.xyz, v_wabs, v_tmat, wm);
    }
#endif

    /* shader water (zero_water.c, terrain_water.glsl; push-constant bit 4096): the water texels of the terrain's first
       pass, before the fog */
#ifdef WATER
    if (v_wat.w > 0.5 && (pc.mv_flags & 4096u) != 0u)
        c.rgb = water_shade(c.rgb, v_wat, v_wrel, v_wabs, (pc.mv_flags & 2048u) != 0u);
#endif

    if (fge) c.rgb = mix(pc.fogcol.rgb, c.rgb, clamp(v_fog, 0.0, 1.0));

#ifdef CITY
    if (ate && !clook) {
#else
    if (ate) {
#endif
        bool pass;
        if      (atst == 0u) pass = false;
        else if (atst == 1u) pass = true;
        else if (atst == 2u) pass = c.a <  aref;
        else if (atst == 3u) pass = c.a <= aref;
        else if (atst == 4u) pass = abs(c.a - aref) < (0.5 / 128.0);
        else if (atst == 5u) pass = c.a >= aref;
        else if (atst == 6u) pass = c.a >  aref;
        else                 pass = abs(c.a - aref) >= (0.5 / 128.0);
        if (afonly ? pass : (!pass && afkeep)) discard;
    }

#ifdef ZCENSUS
    zc_end(v_color.rgb, zc_tex, c.rgb);
#endif
    out_blend = vec4(0.0, 0.0, 0.0, clamp(c.a, 0.0, 1.0));
    if (fba) c.a = max(c.a, 1.0);
    out_color = clamp(c, vec4(0.0), vec4(2.0));
}
