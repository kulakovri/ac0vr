#version 450
#ifdef MV
#extension GL_EXT_multiview : require
#endif
layout(location = 0) in vec4 v_color;
layout(location = 1) in vec3 v_stq;
layout(location = 2) in float v_fog;
layout(location = 3) flat in uint v_round_uv;

layout(location = 0, index = 0) out vec4 out_color;
layout(location = 0, index = 1) out vec4 out_blend;

#ifdef MV
layout(set = 0, binding = 0) uniform sampler2DArray tex;
#else
layout(set = 0, binding = 0) uniform sampler2D tex;
#endif
#ifdef LOCAL_READ
/* DATE reads the target's own pixel straight from the colour attachment (dynamic rendering local read) */
layout(input_attachment_index = 0, set = 1, binding = 0) uniform subpassInput dstin;
#elif defined(MV)
layout(set = 1, binding = 0) uniform sampler2DArray dsttex;
#else
layout(set = 1, binding = 0) uniform sampler2D dsttex;
#endif

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
    float rep_scale;       /* > 1: a replacement image (ps2_texrep.c) at this multiple of the decoded size */
    uint  mesh_block;
    uint  mv_flags;        /* multiview: 1 set-0 texture is layered, 2 set-1 texture is layered, 4 shift eyes */
    vec4  rp[4];           /* (gs.vert's: the reprojection) */
    vec4  rp2;
    vec4  fogr;
    vec4  soft;            /* ACZ_VR_FX_SOFT (bug #231): K = mh / (a upm), zmax, c, the fraction of the view depth; w = 0
                              off. On: the set-1 texture is the frame depth over the effects (ps2_video.c fxs_*) */
    vec4  fxv;             /* ACZ_VR_CLOUD_FX_SOFT (bug #254): x 1 the alpha fades by our cloud's transmittance in front,
                              2 the colour (additive), 3 / 4 the same as a tint; y, z the transmittance range of the fade;
                              w 1: the set-1 texture is the veil (r the scene's view depth in m, 0 = nothing; g, b the view
                              depths where the cloud starts and has reached its total transmittance a) */
} pc;
#ifdef MV
#define TL (((pc.mv_flags & 1u) != 0u) ? gl_ViewIndex : 0)
#define DL (((pc.mv_flags & 2u) != 0u) ? gl_ViewIndex : 0)
#define TEXSIZE() textureSize(tex, 0).xy
#define FETCH_T(p) texelFetch(tex, ivec3(p, TL), 0)
#ifdef LOCAL_READ
#define FETCH_D(p) subpassLoad(dstin)
#else
#define FETCH_D(p) texelFetch(dsttex, ivec3(p, DL), 0)
#endif
#define SAMPLE_T(uv) texture(tex, vec3(uv, float(TL)))
#define SAMPLE_TG(uv, dx, dy) textureGrad(tex, vec3(uv, float(TL)), dx, dy)
#else
#define TEXSIZE() textureSize(tex, 0)
#define FETCH_T(p) texelFetch(tex, p, 0)
#ifdef LOCAL_READ
#define FETCH_D(p) subpassLoad(dstin)
#else
#define FETCH_D(p) texelFetch(dsttex, p, 0)
#endif
#define SAMPLE_T(uv) texture(tex, uv)
#define SAMPLE_TG(uv, dx, dy) textureGrad(tex, uv, dx, dy)
#endif


const uint RUV_SPRITE = 1u << 29;

float gs_wrap(float t, uint mode, float lo, float hi, float size, bool own) {
    if (mode == 0u) return own ? mod(t, max(size, 1.0)) : t;
    if (mode == 1u) return clamp(t, 0.0, max(size - 1.0, 0.0));
    if (mode == 2u) return clamp(t, lo, max(hi, lo));
    return float((int(floor(t)) & int(lo)) | int(hi));
}

float sprite_round(float t, uint mode, int origin, int pixel) {
    if (mode == 0u) return t + 1.0 / 1024.0;
    float halftexel = floor(t * 2.0 + 0.5) * 0.5;
    if (abs(t - halftexel) > 1.0 / 64.0) return t;
    return halftexel + ((mode == 1u || pixel == origin) ? 1.0 : -1.0) / 64.0;
}

vec4 sample_region_repeat(vec2 tc) {
    vec2 lo = floor(tc), hi = lo + 1.0, w = fract(tc);
    ivec2 a, b, size = TEXSIZE();
    a.x = int(gs_wrap(lo.x, pc.flags.z & 3u, float(pc.clamp_uv.x), float(pc.clamp_uv.y), pc.tex_size.x, true));
    b.x = int(gs_wrap(hi.x, pc.flags.z & 3u, float(pc.clamp_uv.x), float(pc.clamp_uv.y), pc.tex_size.x, true));
    a.y = int(gs_wrap(lo.y, (pc.flags.z >> 2) & 3u, float(pc.clamp_uv.z), float(pc.clamp_uv.w), pc.tex_size.y, true));
    b.y = int(gs_wrap(hi.y, (pc.flags.z >> 2) & 3u, float(pc.clamp_uv.z), float(pc.clamp_uv.w), pc.tex_size.y, true));
    a = ((a % size) + size) % size;
    b = ((b % size) + size) % size;
    return mix(mix(FETCH_T(a), FETCH_T(ivec2(b.x, a.y)), w.x),
               mix(FETCH_T(ivec2(a.x, b.y)), FETCH_T(b), w.x), w.y);
}

int wrap_up(int k, uint mode, int lo, int hi, int size, int s) {
    if (mode == 0u) {
        int period = max(size, 1) * s;
        return k - period * int(floor(float(k) / float(period)));
    }
    if (mode == 1u) return clamp(k, 0, max(size, 1) * s - 1);
    if (mode == 2u) return clamp(k, lo * s, max(hi, lo) * s + s - 1);
    int g = int(floor(float(k) / float(s)));
    return ((g & lo) | hi) * s + (k - g * s);
}

/* bilinear in an image stored at sc x the texture's texel grid (an upscaled render target, a replacement image), with
   the GS wrap / clamp / region modes applied per original texel */
vec4 sample_region_repeat_up(vec2 tc, float sc) {
    int s = int(sc + 0.5);
    vec2 c = (tc + 0.5) * sc - 0.5;
    vec2 lo = floor(c), w = c - lo;
    ivec2 size = TEXSIZE();
    uint mu = pc.flags.z & 3u, mv = (pc.flags.z >> 2) & 3u;
    int tw = int(pc.tex_size.x), th = int(pc.tex_size.y);
    int x0 = int(lo.x), y0 = int(lo.y);
    ivec2 a = ivec2(wrap_up(x0, mu, int(pc.clamp_uv.x), int(pc.clamp_uv.y), tw, s),
                    wrap_up(y0, mv, int(pc.clamp_uv.z), int(pc.clamp_uv.w), th, s));
    ivec2 b = ivec2(wrap_up(x0 + 1, mu, int(pc.clamp_uv.x), int(pc.clamp_uv.y), tw, s),
                    wrap_up(y0 + 1, mv, int(pc.clamp_uv.z), int(pc.clamp_uv.w), th, s));
    a -= size * ivec2(floor(vec2(a) / vec2(size)));
    b -= size * ivec2(floor(vec2(b) / vec2(size)));
    return mix(mix(FETCH_T(a), FETCH_T(ivec2(b.x, a.y)), w.x),
               mix(FETCH_T(ivec2(a.x, b.y)), FETCH_T(b), w.x), w.y);
}

/* the original texel p of the game's texture: with a replacement image (rep) the centre of its block (the pack keeps
   each texel's average), else the texel itself; the GS wrap / clamp / region modes applied */
vec4 orig_texel(vec2 p, bool rep) {
    if (rep) return sample_region_repeat_up(p, pc.rep_scale);
    ivec2 size = TEXSIZE();
    ivec2 q = ivec2(int(gs_wrap(p.x, pc.flags.z & 3u, float(pc.clamp_uv.x), float(pc.clamp_uv.y), pc.tex_size.x, true)),
                    int(gs_wrap(p.y, (pc.flags.z >> 2) & 3u, float(pc.clamp_uv.z), float(pc.clamp_uv.w), pc.tex_size.y,
                                true)));
    q = ((q % size) + size) % size;
    return FETCH_T(q);
}
float texel_lit(vec4 t, bool tcc) { return (tcc ? t.a : max(t.r, max(t.g, t.b))) > (1.5 / 255.0) ? 1.0 : 0.0; }
/* A target box drawn as shapes (ps2_video.c ACZ_VR_MARKER_SHARP=2). The box's original texels, at the exact sub-pixel
   place the sprite maps them to (u: this fragment's own texel coordinate, not snapped to the game's pixel grid: no
   texel row is lost to the game's 16 x 16 -> 16 x 13 squash, and the box moves smoothly), drawn crisp in screen
   pixels: each lit texel a square whose edges blend over one pixel (the lit texels' occupancy with "sharp bilinear"
   weights: neighbouring lit texels join without a seam); round: a lit texel's convex corners -- where both neighbours
   on that side are empty -- rounded with a radius of half the texel (a lone dot is a disc, a dash a capsule, the box's
   outer corners round); straight runs are not touched. fw: texels per screen pixel. Returns the lit texels' colour
   (weighted as the coverage: inside a texel its own); cov: the coverage (into the alpha). */
vec4 marker_shape(vec2 u, vec2 fw, bool rep, bool tcc, bool round_c, out float cov) {
    fw = max(fw, vec2(1.0 / 64.0));
    vec2 b = floor(u - 0.5), f = u - 0.5 - b;               /* the 2x2 texels around this fragment, its place among them */
    vec4 t00 = orig_texel(b, rep), t10 = orig_texel(b + vec2(1.0, 0.0), rep);
    vec4 t01 = orig_texel(b + vec2(0.0, 1.0), rep), t11 = orig_texel(b + vec2(1.0, 1.0), rep);
    vec4 o = vec4(texel_lit(t00, tcc), texel_lit(t10, tcc), texel_lit(t01, tcc), texel_lit(t11, tcc));
    vec2 w = clamp((f - 0.5) / fw + 0.5, 0.0, 1.0);          /* sharp: each texel a square, one pixel of blend */
    vec4 k = vec4((1.0 - w.x) * (1.0 - w.y), w.x * (1.0 - w.y), (1.0 - w.x) * w.y, w.x * w.y);
    cov = dot(o, k);
    if (round_c) {
        /* the fragment's own texel (one of the 2x2) and, toward the fragment, its neighbours in x, in y and diagonal:
           the other three of the 2x2 */
        ivec2 own = ivec2(floor(u) - b);
        int io = own.y * 2 + own.x, ix = own.y * 2 + (1 - own.x), iy = (1 - own.y) * 2 + own.x;
        if (o[io] > 0.5 && o[ix] < 0.5 && o[iy] < 0.5) {
            vec2 sz = 1.0 / fw;                              /* a texel's size in pixels */
            float r = 0.5 * min(sz.x, sz.y);
            vec2 q = abs(u - floor(u) - 0.5) * sz;           /* from the texel's centre, pixels */
            vec2 d2 = q - (0.5 * sz - vec2(r));
            if (d2.x > 0.0 && d2.y > 0.0) cov = min(cov, clamp(0.5 - (length(d2) - r), 0.0, 1.0));
        }
    }
    vec4 oo = o * k;                                         /* the lit texels' colours, weighted like the coverage */
    float ws = oo.x + oo.y + oo.z + oo.w;
    return ws > 1e-6 ? (oo.x * t00 + oo.y * t10 + oo.z * t01 + oo.w * t11) / ws : vec4(0.0);
}

/* A target marker drawn as the line drawing its texels are (ps2_video.c ACZ_VR_MARKER_SHARP=3, the default; bug #98:
   the boxes as squares of their texels looked "too pixelated" in the headset -- the ground targets' octagons and cube
   icons have diagonal outlines, single texels touching at their corners, which marker_shape drew as staircases of
   beads). Every lit texel is a node at its centre, every two 8-adjacent nodes are joined by a segment: the shape is
   everything within half a texel of a node or a segment (a dot stays a disc, a dash a capsule, a diagonal chain of
   texels a straight line 0.8 texel wide: a staircase of single texels carries 0.71 of a straight line's light, its
   anti-aliasing fringe the rest), its edge anti-aliased over one screen pixel, its value and colour
   interpolated along the segment; the brightest contribution wins. A diagonal joins two nodes only where no orthogonal
   neighbour already does (no filled corners), a segment only nodes within 1 : 0.3 of each other's value; a texel with
   a neighbour over 1 / 0.3 times as bright is the game's anti-aliasing fringe and no node (the cube icons' 255 / 62
   pairs: no double lines); a 2x2 of lit texels is filled. The nodes of the 4x4 texels around the fragment can reach
   it (none lit: done -- most of a box's inside); their fringe test reads 6x6. fw: texels per screen pixel. Returns
   the colour (cov: the coverage, into alpha). */
float mk_val(vec4 t, bool tcc) { return tcc ? t.a : max(t.r, max(t.g, t.b)); }
vec4 marker_lines(vec2 u, vec2 fw, bool rep, bool tcc, out float cov) {
    const float K = 0.3, LIT = 1.5 / 255.0, R = 0.5, RD = 0.4;
    vec2 b = floor(u - 0.5);                                 /* node (1, 1) of the 4x4: the texel at b */
    float w6[36];
    vec4 tx[16];
    float any = 0.0;                                         /* the 4x4 first: nothing lit there = nothing to draw */
    for (int j = 1; j <= 4; j++)
        for (int i = 1; i <= 4; i++) {
            vec4 t = orig_texel(b + vec2(float(i - 2), float(j - 2)), rep);
            w6[j * 6 + i] = mk_val(t, tcc);
            tx[(j - 1) * 4 + (i - 1)] = t;
            any = max(any, w6[j * 6 + i]);
        }
    cov = 0.0;
    if (any <= LIT) return vec4(0.0);
    for (int j = 0; j < 6; j++)                              /* the ring around it (the fringe test only) */
        for (int i = 0; i < 6; i++)
            if (i == 0 || i == 5 || j == 0 || j == 5)
                w6[j * 6 + i] = mk_val(orig_texel(b + vec2(float(i - 2), float(j - 2)), rep), tcc);
    float v[16];
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++) {
            float a = w6[(j + 1) * 6 + (i + 1)], m = 0.0;
            for (int dj = 0; dj < 3; dj++)
                for (int di = 0; di < 3; di++)
                    if (di != 1 || dj != 1) m = max(m, w6[(j + dj) * 6 + (i + di)]);
            v[j * 4 + i] = (a > LIT && a >= K * m) ? a : 0.0;
        }
    float px = max(0.5 * (fw.x + fw.y), 1.0 / 64.0), r = max(R, 0.5 * px);
    float best = 0.0;
    vec4 bc = vec4(0.0);
    cov = 0.0;
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++) {
            float vp = v[j * 4 + i];
            if (vp <= 0.0) continue;
            vec2 p = u - (b + vec2(float(i - 1), float(j - 1)) + 0.5);
            float c0 = clamp(0.5 - (length(p) - r) / px, 0.0, 1.0);          /* the node's disc */
            if (vp * c0 > best) { best = vp * c0; bc = tx[j * 4 + i]; cov = c0; }
            for (int k = 0; k < 4; k++) {
                ivec2 d = k == 0 ? ivec2(1, 0) : k == 1 ? ivec2(0, 1) : k == 2 ? ivec2(1, 1) : ivec2(1, -1);
                int qi = i + d.x, qj = j + d.y;
                if (qi > 3 || qj < 0 || qj > 3) continue;
                float vq = v[qj * 4 + qi];
                if (vq <= 0.0 || min(vp, vq) < K * max(vp, vq)) continue;
                if (d.x != 0 && d.y != 0 && max(v[j * 4 + qi], v[qj * 4 + i]) >= K * min(vp, vq)) continue;
                vec2 dd = vec2(d);
                float t = clamp(dot(p, dd) / dot(dd, dd), 0.0, 1.0);
                float rs = d.x != 0 && d.y != 0 ? max(RD, 0.5 * px) : r;
                float c1 = clamp(0.5 - (length(p - t * dd) - rs) / px, 0.0, 1.0);
                float val = mix(vp, vq, t);
                if (val * c1 > best) { best = val * c1; bc = mix(tx[j * 4 + i], tx[qj * 4 + qi], t); cov = c1; }
            }
        }
    if (v[5] > 0.0 && v[6] > 0.0 && v[9] > 0.0 && v[10] > 0.0) {   /* the fragment's own 2x2 all lit: filled */
        vec2 f = u - 0.5 - b;
        vec4 bl = mix(mix(tx[5], tx[6], f.x), mix(tx[9], tx[10], f.x), f.y);
        float val = mix(mix(v[5], v[6], f.x), mix(v[9], v[10], f.x), f.y);
        if (val > best) { best = val; bc = bl; cov = 1.0; }
    }
    return bc;
}

/* PS2_PERF_GS_SPLIT (fq_opt 2026-10-26; ps2_video.c pipe_build): 0 in the pipelines of draws that cannot be a target
   marker (frag flag bit 31 clear), so the driver leaves out marker_lines / marker_shape -- their 36 + 16 + 16 local
   values set the whole shader's register count (72: 28 warps an SM) for every GS draw. The draws that run them take
   the pipelines with 1 (key bit 57). The same code runs for every draw either way */
layout(constant_id = 0) const int GS_MARKER = 1;
#ifdef ZCENSUS
#include "terrain_zcensus.glsl"      /* (PS2_TERRAIN_ZCENSUS, diagnostic: the variants gs_zcen*.frag.spv) */
#endif
void main() {
#ifdef ZCENSUS
    zc_begin(pc.mesh_block);
    vec3 zc_tex = vec3(-1.0);
#endif
    out_blend = vec4(1.0);
    vec2 tdx = vec2(0.0), tdy = vec2(0.0);
    if (pc.scale > 1.0 && pc.flags.y != 0u) {
        vec2 tcd = ((pc.flags.x >> 2) & 1u) != 0u
                 ? v_stq.xy : (v_stq.xy / max(v_stq.z, 1e-9)) * pc.tex_size;
        tdx = dFdx(tcd);
        tdy = dFdy(tcd);
    }
    if (pc.flags.w == 1u) { out_color = vec4(1.0, 0.0, 1.0, 1.0); return; }
    if (pc.flags.w == 2u) { out_color = vec4(v_color.rgb, 1.0); return; }
    vec4 c = v_color;
    uint f = pc.flags.x;
    if ((f & (1u << 24)) != 0u) {
        vec2 rg = FETCH_T(ivec2(gl_FragCoord.xy)).rg;
        uint green = uint(round(rg.y * 255.0));
        uint ta = (f >> ((green & 128u) != 0u ? 27u : 26u)) & 1u;
        if ((f & (1u << 28)) != 0u && uint(round(rg.x * 255.0)) == 0u && (green & 127u) == 0u)
            ta = 0u;
        green = (green & 127u) | (ta << 7);
        out_color = vec4(rg.x, float(green) / 255.0, rg.x, float(green) / 128.0);
        out_blend = vec4(1.0);
        return;
    }
    uint tfx  = f & 3u;
    bool fst  = ((f >> 2) & 1u) != 0u;
    bool ate  = ((f >> 3) & 1u) != 0u;
    uint atst = (f >> 4) & 7u;
    float aref = float((f >> 8) & 0xFFu) / 128.0;
    bool fge  = ((f >> 16) & 1u) != 0u;
    bool tcc  = ((f >> 17) & 1u) != 0u;
    bool afkeep = ((f >> 18) & 1u) != 0u;
    bool point = ((f >> 19) & 1u) != 0u;
    bool point_rows = point && ((f >> 29) & 1u) != 0u;
    bool afonly = ((f >> 20) & 1u) != 0u;
    bool fba   = ((f >> 21) & 1u) != 0u;
    bool date  = ((f >> 22) & 1u) != 0u;
    bool datm  = ((f >> 23) & 1u) != 0u;
    if (date && pc.flags.w == 3u) {   /* debug: show the destination alpha the DATE test reads */
        float dd = FETCH_D(ivec2(gl_FragCoord.xy)).a;
        out_color = vec4(dd, dd >= 1.0 ? 1.0 : 0.0, 0.0, 1.0); out_blend = vec4(0.0, 0.0, 0.0, 1.0); return;
    }
    if (date) {
        float ad = FETCH_D(ivec2(gl_FragCoord.xy)).a;
        if ((ad >= 1.0) != datm) discard;
    }

    if (pc.flags.y != 0u) {
        vec2 tc;
        if (fst) tc = v_stq.xy;
        else     tc = (v_stq.xy / max(v_stq.z, 1e-9)) * pc.tex_size;
        vec2 tc_frag = tc;     /* this fragment's own texel coordinate (before the snapping below) */
        uint ruv = v_round_uv & (RUV_SPRITE - 1u);
        bool sprite = (v_round_uv & RUV_SPRITE) != 0u;
        bool rt_up = pc.tex_scale > 1.0;
        bool rep = pc.rep_scale > 1.0;          /* a replacement image (ps2_texrep.c) */
        ivec2 pix = ivec2(gl_FragCoord.xy);
        vec2 sub = vec2(0.0);
        if (pc.scale > 1.0) {
            vec2 nat = floor(gl_FragCoord.xy / pc.scale);
            sub = gl_FragCoord.xy - 0.5 - nat * pc.scale;
            pix = ivec2(nat);
            if (point && (sprite || rt_up))
                tc += (point_rows ? vec2(0.0) : tdx * ((nat.x + 0.5) * pc.scale - gl_FragCoord.x))
                    + tdy * ((nat.y + 0.5) * pc.scale - gl_FragCoord.y);
        }
        if (point && ruv != 0u) {
            if (!point_rows)
                tc.x = sprite_round(tc.x, ruv & 3u,
                    int((ruv >> 4) & 4095u) - 2048, pix.x);
            tc.y = sprite_round(tc.y, (ruv >> 2) & 3u,
                int((ruv >> 16) & 4095u) - 2048, pix.y);
        }
        if (!point && (f & (1u << 25)) != 0u) tc -= 0.5;
        vec2 unwrapped_tc = tc;
        vec2 isz = rep ? pc.img_size / pc.rep_scale : pc.img_size;     /* the image's size in decoded texels */
        /* level of detail from the unwrapped coordinates (textures with mips: PS2_GS_MIPS): the wrap below jumps */
        vec2 gdx = dFdx(unwrapped_tc) / isz, gdy = dFdy(unwrapped_tc) / isz;
        bool own = any(notEqual(isz, pc.tex_size));
        tc.x = gs_wrap(tc.x, pc.flags.z & 3u,
                       float(pc.clamp_uv.x), float(pc.clamp_uv.y),
                       pc.tex_size.x, own);
        tc.y = gs_wrap(tc.y, (pc.flags.z >> 2) & 3u,
                       float(pc.clamp_uv.z), float(pc.clamp_uv.w),
                       pc.tex_size.y, own);
        vec4 t;
        float mk_cov = 1.0;
        if (GS_MARKER != 0 && ((f >> 31) & 1u) != 0u && ((f >> 26) & 1u) != 0u && pc.scale > 1.0) {
            /* a target box as crisp anti-aliased shapes of its original texels: bit 28 the line drawing (marker_lines),
               else marker_shape (bit 27: corners round) */
            if (((f >> 28) & 1u) != 0u) t = marker_lines(tc_frag, abs(tdx) + abs(tdy), rep, tcc, mk_cov);
            else t = marker_shape(tc_frag, abs(tdx) + abs(tdy), rep, tcc, ((f >> 27) & 1u) != 0u, mk_cov);
        } else if (rep && point && ((f >> 31) & 1u) != 0u) {
            /* a target box (ps2_video.c ACZ_VR_MARKER_SHARP): the replacement's block of this texel at its centre --
               the original texel's colour (the pack keeps each texel's average), flat: crisp edges at any upscale */
            vec2 p0 = floor(unwrapped_tc + (ruv == 0u ? 1.0 / 32.0 : 0.0));
            t = sample_region_repeat_up(p0, pc.rep_scale);
        } else if (rep && point) {
            /* point-sampled: the texel the game's point filter shows for this native pixel (snapping and sprite
               rounding as above), and inside it this fragment's own position, filtered in that texel's s x s block
               of the replacement ("sharp bilinear", footprint kept in the block): the game's shapes and positions
               with the larger image's detail, and nothing from neighbouring atlas cells */
            float s = pc.rep_scale;
            vec2 p0 = floor(unwrapped_tc + (ruv == 0u ? 1.0 / 32.0 : 0.0));
            vec2 c = (p0 + clamp(tc_frag - p0, 0.0, 1.0)) * s - 0.5;
            vec2 fw = max((abs(tdx) + abs(tdy)) * s, vec2(1.0 / 64.0));
            vec2 lo = floor(c);
            c = clamp(lo + clamp((c - lo - 0.5) / fw + 0.5, 0.0, 1.0), p0 * s, p0 * s + s - 1.0);
            t = sample_region_repeat_up((c + 0.5) / s - 0.5, s);
        } else if (rep) {
            t = sample_region_repeat_up(unwrapped_tc, pc.rep_scale);     /* bilinear: texel-centre coordinates */
        } else if (point) {
            ivec2 size = TEXSIZE();
            ivec2 p = ivec2(floor(tc + (ruv == 0u ? 1.0 / 32.0 : 0.0)));
            if (rt_up) {
                int s = int(pc.tex_scale + 0.5);
                /* (the target's sub-texel under this pixel; a target at another upscale than the one drawn into,
                   nx_offscreen's offrt: scaled by the ratio; the same upscale: exactly as before) */
                float kr = pc.tex_scale == pc.scale ? 1.0 : pc.tex_scale / pc.scale;
                ivec2 k = clamp(ivec2((sub + 0.5) * kr), ivec2(0), ivec2(s - 1));
                if (tdx.x < 0.0) k.x = s - 1 - k.x;
                if (tdy.y < 0.0) k.y = s - 1 - k.y;
                p = p * s + k;
            }
            p = ((p % size) + size) % size;
            t = FETCH_T(p);
        } else if ((pc.flags.z & 3u) == 3u || ((pc.flags.z >> 2) & 3u) == 3u
                   || (own && ((pc.flags.z & 3u) == 0u
                               || ((pc.flags.z >> 2) & 3u) == 0u))) {
            if (rt_up) t = sample_region_repeat_up(unwrapped_tc, pc.tex_scale);
            else       t = sample_region_repeat(unwrapped_tc);
        } else {
            /* mips only for whole textures (repeat / clamp): an atlas region (region clamp) would bleed */
            bool whole = (pc.flags.z & 3u) < 2u && ((pc.flags.z >> 2) & 3u) < 2u;
            t = SAMPLE_TG((tc + 0.5) / pc.img_size, whole ? gdx : vec2(0.0), whole ? gdy : vec2(0.0));
        }
#ifdef ZCENSUS
        zc_tex = t.rgb;
#endif
        float af = c.a;
        vec3 rgb;
        float a;
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
        c = vec4(rgb, a * mk_cov);
    } else {
        c.rgb *= 128.0 / 255.0;
    }

    if (fge) c.rgb = mix(pc.fogcol.rgb, c.rgb, clamp(v_fog, 0.0, 1.0));

    bool apass = true;
    if (ate) {
        bool pass;
        if      (atst == 0u) pass = false;
        else if (atst == 1u) pass = true;
        else if (atst == 2u) pass = c.a <  aref;
        else if (atst == 3u) pass = c.a <= aref;
        else if (atst == 4u) pass = abs(c.a - aref) < (0.5 / 128.0);
        else if (atst == 5u) pass = c.a >= aref;
        else if (atst == 6u) pass = c.a >  aref;
        else                 pass = abs(c.a - aref) >= (0.5 / 128.0);
        apass = pass;
        if (afonly ? pass : (!pass && afkeep)) discard;
    }

    /* bit 30: alpha-test fail = "RGB only" in one pass (no depth write): the colour blend factor comes from
       out_blend.rgb and the target's alpha is replaced only where the test passed (out_blend.a selects it) */
    /* ACZ_VR_FX_SOFT (bug #231): an effect sprite (blend C = As) fades out over the last stretch in front of what lies
       behind it -- the ground, an aircraft, a haze layer, our clouds' depth stamp -- instead of being cut there along a
       hard line: alpha x smoothstep(0, L, scene depth - sprite depth), both view depths (m) from the GS Z (larger =
       nearer), L = the fraction x the sprite's depth within 3..60 m; nothing behind (Z 0, the sky): as before */
    /* ACZ_VR_CLOUD_FX_SOFT (bug #254): with the veil bound (fxv.w) the scene depth is its r in metres, and the effect
       fades by our cloud's transmittance in front of its own depth -- the ramp from g (1) to b (a) -- mapped through the
       range fxv.y .. fxv.z (smooth), instead of a binary depth stamp cutting it where the transmittance crosses a limit */
    if (pc.soft.w != 0.0 || pc.fxv.x != 0.0) {
        vec4 vt = FETCH_D(ivec2(gl_FragCoord.xy));
        bool veil = pc.fxv.w != 0.0;
        float df = pc.soft.x / max(gl_FragCoord.z * pc.soft.y + pc.soft.z, 1e-9);
        if (pc.soft.w != 0.0) {
            float zs = vt.r, k = 1.0;
            if (zs > 0.0) {
                float ds = veil ? zs : pc.soft.x / max(zs * pc.soft.y + pc.soft.z, 1e-9);
                k = clamp((ds - df) / clamp(df * abs(pc.soft.w), 3.0, 60.0), 0.0, 1.0);
            }
            if (pc.soft.w < 0.0) c = vec4(1.0 - k, k, zs > 0.0 ? 0.0 : 1.0, 1.0);   /* (ACZ_DEBUG_FX_SOFT_TINT: red = faded) */
            else c.a *= k * k * (3.0 - 2.0 * k);
        }
        if (pc.fxv.x != 0.0 && veil) {
            float tr = mix(1.0, vt.a, clamp((df - vt.g) / max(vt.b - vt.g, 1e-3), 0.0, 1.0));
            float kc = clamp((tr - pc.fxv.y) / max(pc.fxv.z - pc.fxv.y, 1e-3), 0.0, 1.0);
            kc = kc * kc * (3.0 - 2.0 * kc);
            if (pc.fxv.x > 2.5) c.rgb = vec3(1.0 - kc, kc, vt.a < 0.996 ? 0.0 : 1.0);   /* (tint, the draw's own alpha:
                                                                                              red faded, blue no cloud) */
            else if (pc.fxv.x > 1.5) c.rgb *= kc;
            else c.a *= kc;
        }
    }
#ifdef ZCENSUS
    zc_end(v_color.rgb, zc_tex, c.rgb);
#endif
    if (((f >> 30) & 1u) != 0u) out_blend = vec4(vec3(clamp(c.a, 0.0, 1.0)), apass ? 1.0 : 0.0);
    else out_blend = vec4(0.0, 0.0, 0.0, clamp(c.a, 0.0, 1.0));
    if (fba) c.a = max(c.a, 1.0);
    out_color = clamp(c, vec4(0.0), vec4(2.0));
}
