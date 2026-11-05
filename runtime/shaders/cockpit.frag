#version 450
#ifdef MV
#extension GL_EXT_multiview : require
#endif
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec3 v_n;
layout(location = 2) in vec3 v_p;
layout(location = 3) in float v_ao;
layout(location = 0) out vec4 out_color;

#ifdef MV
layout(set = 0, binding = 0) uniform sampler2DArray tex;
#define SAMPLE(uv) texture(tex, vec3(uv, 0.0))
#define SAMPLE_BIAS(uv, k) texture(tex, vec3(uv, 0.0), k)
#else
layout(set = 0, binding = 0) uniform sampler2D tex;
#define SAMPLE(uv) texture(tex, uv)
#define SAMPLE_BIAS(uv, k) texture(tex, uv, k)
#endif

layout(push_constant) uniform CP {
    vec4 rot[3];      /* w: light block (-1 none), specular strength, shininess */
    vec4 head;
    vec4 proj;
    vec4 misc;
    vec4 color;
    vec4 light;       /* x brightness, y ambient (fixed light); z textured; w alpha cut (< 0: glass) */
    vec4 model[3];    /* rows of the part's animation transform (rotation xyz, translation w) */
    vec4 su, sv, sd;  /* sun shadow map: rows u, v (map -1..1), d (depth 0..1); su.xyz = 0: none. Glass parts take
                         no shadow, so for them su / sv / sd carry the canopy water: su.xyz up to three debris flecks
                         (ps2_video.c canopy_mark_code; 0 = none), sv.x 1 = the world image is bound (set 3) + 2 = the
                         shadow map is on, sv.y wet smear 0..1, sv.z drop size scale, sv.w strength; sd.x speed 0..1
                         (450 kt,
                         also the clearing of the windscreen by the airflow), sd.y canopy ice: the frozen share of
                         the water and the rime / frost cover, each 0..1023 packed as frozen x 1024 + frost (0 =
                         none), sd.z cloud light 0..1 (as wx.w below), sd.w condensation film 0..1; su.w the drop
                         intensity 0..1 (0.45 = the drops as before; less = fine sparse droplets, more = larger drops
                         and streaming rivulets) */
    vec4 uvoff;       /* texture offset (drum counters); glass: z grime 0..1 (< 0: -(1 + soot x 255 + 256 x grime x 255),
                         bug #139), w streak stretch (1 = round); other
                         parts: w = minus the share of the direct sun left in a cloud's shadow (ps2_video.c cloud_sun;
                         >= 0: none) */
    vec4 wx;          /* glass: canopy water -- x wetness 0..1, y / z flow offsets of the bead / rivulet layers (in
                         cells), w mist 0..1 (ps2_video.c canopy_update); other parts: the light inside a cloud --
                         rgb the cloud's colour, w how far the sun gives way to the cloud's diffuse light 0..1 */
} cp;

/* the native mesh blocks (mesh.vert); the reserved one holds the game's lights in the cockpit frame: light i points
   along (l[0][i], l[1][i], l[2][i]) with colour r[i], r[3] is the ambient */
struct Block {
    vec4  m[4];
    vec4  c[4];
    vec4  l[3];
    vec4  r[4];
    vec4  e[3];
    float fog0, fog1, zscale, zmax;
    float xoff, yoff, spec_alpha, xr_k;
    uint  mode, pass, clip, pad1;
    uint  prog;
    float xr_wmin;
    uint  xr_reproj, pad4;
    vec4  p[4];
    vec4  mp[4];
};
layout(std430, set = 2, binding = 0) readonly buffer Blocks { Block b[]; };
layout(set = 1, binding = 0) uniform sampler2DShadow smap;     /* the sun's depth (cpit_shadow_pass) */
/* the cockpit's interior seen from the design eye (fx_glass, ps2_video.c gfx_refl_init): the canopy reflects it */
layout(set = 4, binding = 0) uniform samplerCube interior;
/* the frame behind the cockpit (a snapshot of the world image taken right before the cockpit pass): the canopy water
   refracts it */
#ifdef MV
layout(set = 3, binding = 0) uniform sampler2DArray world;
#define WORLD(uv) texture(world, vec3(uv, float(gl_ViewIndex))).rgb
#else
layout(set = 3, binding = 0) uniform sampler2D world;
#define WORLD(uv) texture(world, uv).rgb
#endif

/* fraction of the sun reaching p: 8 rotated Poisson taps of the hardware 2x2 compare, ~3 texels wide */
float sun_visible(vec3 p, vec3 n, vec3 Ln) {
    if (cp.su.xyz == vec3(0.0)) return 1.0;
    vec3 q = p + n * 0.004 + Ln * 0.002;          /* normal offset against acne */
    vec3 s = vec3(dot(cp.su.xyz, q) + cp.su.w, dot(cp.sv.xyz, q) + cp.sv.w, dot(cp.sd.xyz, q) + cp.sd.w);
    vec2 uv = s.xy * 0.5 + 0.5;
    if (uv.x <= 0.0 || uv.y <= 0.0 || uv.x >= 1.0 || uv.y >= 1.0 || s.z >= 1.0) return 1.0;
    const vec2 P[8] = vec2[](vec2(-0.326, -0.406), vec2(-0.840, -0.074), vec2(-0.696, 0.457), vec2(-0.203, 0.621),
                             vec2(0.962, -0.195), vec2(0.473, -0.480), vec2(0.519, 0.767), vec2(0.185, -0.893));
    float a = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453) * 6.2831853;
    mat2 rot = mat2(cos(a), sin(a), -sin(a), cos(a));
    vec2 r = 3.0 / vec2(textureSize(smap, 0));
    float sum = 0.0;
    for (int i = 0; i < 8; i++) sum += texture(smap, vec3(uv + rot * P[i] * r, s.z - 0.0005));
    return sum / 8.0;
}

/* Flashes (wish #83, ps2_video.c flash_*): launches, explosions, hits, the gun and the afterburner as up to 4 point
   lights in the cockpit frame, in the light block's spare fields -- m[0] = the cockpit tub (canopy rail height, half
   width, front edge; m), w = the number of lights; mp[i] = position (m), w = its distance^2 from the eye + r0^2;
   c[i] = rgb at the eye, w = r0^2. Lambert with a little wrap, the distance falloff relative to the eye's, and the tub
   as a coarse shadow: inside it below the rail a light from outside arrives only through the opening above (a ray to
   it must cross the rail plane within the side walls and behind the coaming); a faint bounce (the canopy and the
   interior scattering it) reaches everything. The light added to the part's own (amb + dir) */
vec3 flash_light(int bi, vec3 p, vec3 n, float ao) {
    vec4 tub = b[bi].m[0];
    bool inside = abs(p.x) < tub.y && p.z > tub.z - 0.05 && p.z < 1.2;
    vec3 s = vec3(0.0), bn = vec3(0.0);
    for (int i = 0; i < 4; i++) {
        if (float(i) >= tub.w) break;
        vec4 lp = b[bi].mp[i], lc = b[bi].c[i];
        vec3 L = lp.xyz - p;
        float d2 = dot(L, L);
        vec3 Ld = L * inversesqrt(max(d2, 1e-6));
        float fall = min(lp.w / (d2 + lc.w), 3.0);
        float ndl = dot(n, Ld);
        float lam = max(ndl, 0.0) * 0.85 + 0.15 * (0.5 + 0.5 * ndl);
        float open = 1.0;
        if (inside && p.y < tub.x) {
            if (Ld.y <= 0.02) open = 0.0;
            else {
                vec3 q = p + Ld * ((tub.x - p.y) / Ld.y);
                open = (1.0 - smoothstep(tub.y - 0.08, tub.y + 0.08, abs(q.x))) * smoothstep(tub.z - 0.08, tub.z + 0.08, q.z);
            }
        }
        s += lc.rgb * (fall * lam * open);
        bn += lc.rgb * fall;
    }
    return s * (0.55 + 0.45 * ao) + bn * (0.15 * ao);
}

#ifdef GXV
/* ACZ_VR_GLASS_VARIANTS (perf5_glassexact 2026-11-03; ps2_video.c gxv_*): this file compiled with -DGXV is the glass's
   variant module (cockpit_gx[_mv].frag.spv; the plain module is the same source without these blocks, byte for byte the
   SPIR-V it was): its constant 1 = the parts of the glass shader a pipeline leaves out. ps2_video.c picks a variant for
   a glass draw only when that draw's push constants make the part a no-op -- the same branch the plain shader takes for
   them, so the same colour -- and the driver then compiles without that part's code and registers. ps2_video.c gxv_can
   mirrors the tests below on the CPU: a change to how canopy_water, crk_glass or glass_light read their inputs needs the
   same change there (PS2_PERF_AB=1 with PS2_PERF_AB_FLIP=gxv: the same-frame check). On a draw where a part is not a
   no-op the bit leaves it out (ACZ_VR_COCKPIT_AB items gxa..gxf: measurement only, wrong images). */
layout(constant_id = 1) const int GXV_OFF = 0;
#define GXOFF(b) ((GXV_OFF & (b)) != 0)
const int GXV_WATER = 1;      /* canopy_water returns gc at once (wet, mist, grime, smear, film, ice, soot 0, no flecks) */
const int GXV_CRK = 2;        /* no crack mesh (light.w's crack bits 0) */
const int GXV_LIGHT = 4;      /* no fx_glass light ((light.w code & 3) < 2) */
const int GXV_CLASSIC = 8;    /* the drops in the lensdrops look (sv.z's refraction quarters >= 1): the classic code out */
const int GXV_LENS = 16;       /* the classic look: the lensdrops code out */
const int GXV_ICE = 32;       /* sd.y <= 0: no frozen drops, no frost or rime */
const int GXV_FILM = 64;      /* sd.w <= 0: no condensation film */
const int GXV_MIST = 128;     /* wx.w <= 0: no mist */
const int GXV_GRIME = 256;    /* the decoded grime and the smear 0 */
const int GXV_SOOT = 512;     /* the decoded soot 0 */
const int GXV_FLECK = 1024;   /* su.xyz <= 0.5: no debris flecks */
const int GXV_EARLY = 2048;   /* (not a part: the lensdrops drop layers' empty cells return after the first hash) */
const int GXV_DROPS = 4096;   /* wx.x <= 0: no drop layers (as a measurement on a wet glass: a wrong image) */
const int GXV_NOWEAR = 8192;  /* glass_light's wear and glare: no sunlight, or its image over 23 deg from every ray */
const int GXV_NODUST = 16384; /* glass_light's dust: no dust strength or no sunlight (e[1].rgb 0) */
/* ACZ_VR_CANOPY_QUALITY (perf6_glassladder 2026-11-04; ps2_video.c cq_*): the rungs below HIGH for the LOW / MEDIUM presets.
   Unlike the bits above these change the image; every part stays on the glass, drawn with less work (the patterns differ, the
   drops stay in their cells). All 0 = HIGH: the same branches as the plain shader. */
const int GXQ_HASH = 32768;   /* the water's value noise on a one-round hash: another pattern, the same statistics */
const int GXQ_FILM = 65536;   /* the film: one noise octave and two sines, 3 blur taps (6) */
const int GXQ_CLUMP = 2097152;   /* where the water gathers: three sines instead of a noise lookup */
const int GXQ_GRAIN = 8388608;   /* grime and soot: one noise octave, no soot grain */
#endif
/* ---- canopy water (glass parts only) ----
   Water on the outside of the canopy and windscreen, in the cockpit frame (metres; x right, y up, z back = the
   airflow), so it sits on the real glass in both eyes. Drops are small lenses: they show the world image behind the
   glass displaced (inverted towards their rims), a thin darker rim and a glint only where the sun or the sky lights
   them; nothing is painted opaque. Beads (mostly small, a few large, clumped, thinning in the middle of the windscreen
   at speed) and thin rivulets that run aft; a fine film in cloud (the world slightly diffused); grime from debris is
   a faint translucent smudge plus wet smears that slide off. One drop at most per cell, inside its inner part, so one
   hash per layer. */
uvec3 pcg3(uvec3 v) {
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return v;
}
vec4 hash4(ivec3 c) {
    uvec3 h = pcg3(uvec3(c & 4095));
    uvec3 g = pcg3(h.zxy ^ 0x9E3779B9u);
    return vec4(h, g.x) * (1.0 / 4294967296.0);
}
float hash1(ivec3 c) { return float(pcg3(uvec3(c & 4095)).x) * (1.0 / 4294967296.0); }
/* a cheaper hash and value noise for the ice: the frost covers large parts of the glass, so one multiply-xorshift
   round instead of pcg3's two */
float hash_i(ivec3 c) {
    uvec3 u = uvec3(c & 1023);
    uint h = u.x * 0x8DA6B343u ^ u.y * 0xD8163841u ^ u.z * 0xCB1AB31Fu;
    h ^= h >> 15u; h *= 0x2C1B3C6Du; h ^= h >> 12u;
    return float(h) * (1.0 / 4294967296.0);
}
float vnoise_i(vec3 x) {
    ivec3 i = ivec3(floor(x));
    vec3 f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    float a = mix(hash_i(i), hash_i(i + ivec3(1, 0, 0)), f.x), b = mix(hash_i(i + ivec3(0, 1, 0)), hash_i(i + ivec3(1, 1, 0)), f.x);
    float c = mix(hash_i(i + ivec3(0, 0, 1)), hash_i(i + ivec3(1, 0, 1)), f.x), e = mix(hash_i(i + ivec3(0, 1, 1)), hash_i(i + ivec3(1, 1, 1)), f.x);
    return mix(mix(a, b, f.y), mix(c, e, f.y), f.z);
}
float vnoise(vec3 x) {                                   /* value noise 0..1 */
    ivec3 i = ivec3(floor(x));
    vec3 f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    float a = mix(hash1(i), hash1(i + ivec3(1, 0, 0)), f.x), b = mix(hash1(i + ivec3(0, 1, 0)), hash1(i + ivec3(1, 1, 0)), f.x);
    float c = mix(hash1(i + ivec3(0, 0, 1)), hash1(i + ivec3(1, 0, 1)), f.x), e = mix(hash1(i + ivec3(0, 1, 1)), hash1(i + ivec3(1, 1, 1)), f.x);
    return mix(mix(a, b, f.y), mix(c, e, f.y), f.z);
}
#ifdef GXV
/* (CANOPY QUALITY, GXQ_HASH) the water's noise on the ice's hash */
float vn_w(vec3 x) { return GXOFF(GXQ_HASH) ? vnoise_i(x) : vnoise(x); }
float hash1_w(ivec3 c) { return GXOFF(GXQ_HASH) ? hash_i(c) : hash1(c); }
/* (GXQ_CLUMP) clumps of ~6 cm, as vnoise(p * 16) around 0.5 */
float cq_clump(vec3 p) {
    return 0.5 + 0.17 * (sin(p.x * 52.0 + 1.7 * sin(p.z * 23.0)) + sin(p.y * 47.0 + p.z * 31.0) + sin((p.x + p.y) * 37.0 - p.z * 19.0));
}
#else
#define vn_w vnoise
#define hash1_w hash1
#endif
/* a bead layer: cell size s (m), offset along the flow (cells), stretch along the flow, presence probability, radius
   range (cells, cubed distribution: mostly small); the drop intensity grows the drops in place (radius x rk, at most
   to the cell's edge) and shifts the distribution (u^ex: ex < 3 more large ones); rk 1 / ex 3 = the drops as before.
   Returns the coverage in m, the distance from the centre in d (1 at the rim), the slope of the cap (cockpit frame,
   grows towards the rim) in g and the drop's size draw u (0..1, small drops low: they freeze first) */
float bead_layer(vec3 p, float s, float off, float stretch, float prob, float rmin, float rmax, float px, float rk,
                 float ex, out float d, out vec3 g, out float u) {
    vec3 q = p / s;
    q.z = q.z / stretch - off;
    ivec3 c = ivec3(floor(q));
    vec3 f = fract(q) - 0.5;
    vec4 h = hash4(c);
    u = fract(h.x * 7.13 + h.w * 3.7);
    float r = mix(rmin, rmax, u * u * u);
    vec3 o = (h.yzw - 0.5) * (0.9 - 2.0 * r);              /* the centre keeps the drop inside its cell */
    if (rk != 1.0 || ex != 3.0)                            /* (the centre stays where it was: the drop grows in place) */
        r = min(mix(rmin, rmax, pow(u, ex)) * rk, 0.48 - max(abs(o.x), max(abs(o.y), abs(o.z))));
    vec3 v = (f - o) / r;
    d = length(v);
    float aa = max(px / (s * r), 1e-3);                    /* a pixel in units of the radius */
    float cover = clamp(1.6 / aa - 0.6, 0.0, 1.0);         /* drops below ~1.5 px fade out instead of flickering */
    aa = min(aa, 0.5);
    float m = (1.0 - smoothstep(1.0 - aa, 1.0, d)) * cover * step(h.x, prob);
    float hgt = sqrt(max(1.0 - d * d, 0.0));
    g = v / max(hgt, 0.25) * m;
    g.z /= stretch;
    return m;
}
/* a rivulet layer: thin water lines along the flow (cells s across, s * len along), sliding aft; wk widens them (drop
   intensity, 1 = as before); u = the line's own draw (freezing order) */
float rivulet_layer(vec3 p, float s, float len, float off, float prob, float px, float wk, out float d, out vec3 g,
                    out float u) {
    vec3 q = vec3(p.x / s, p.y / s, p.z / (s * len) - off);
    ivec3 c = ivec3(floor(q));
    vec3 f = fract(q) - 0.5;
    vec4 h = hash4(c + ivec3(1371, 2087, 593));
    vec2 o = (h.yz - 0.5) * 0.6;
    float w = mix(0.035, 0.09, h.w) * wk;                  /* half width (cells) */
    u = h.w;
    vec2 v = (f.xy - o) / w;
    d = length(v);
    float t = f.z + (h.w - 0.5) * 0.3;                     /* along the line: a head at the front, a fading tail */
    float along = smoothstep(-0.48, -0.25, t) * (1.0 - smoothstep(0.1, 0.45, t));
    float aa = max(px / (s * w), 1e-3);
    float cover = clamp(1.6 / aa - 0.6, 0.0, 1.0);
    aa = min(aa, 0.5);
    float m = (1.0 - smoothstep(1.0 - aa, 1.0, d)) * along * cover * step(h.x, prob);
    float hgt = sqrt(max(1.0 - d * d, 0.0));
    g = vec3(v / max(hgt, 0.3), 0.0) * m * 0.6;            /* a flat lens: bends only across the line */
    return m;
}
/* ---- the drops as real beads (cv_drops 2026-10-29, ACZ_VR_CANOPY_DROPS=lensdrops) ----
   What makes a modern game's canopy water read as volume (its M7 footage): every drop is a strong lens. A bead of water
   on the glass is a spherical cap (contact angle 90 deg, a rain-repellent canopy: ACZ_VR_CANOPY_DROP_ANGLE) that
   bends the eye's ray towards its far side by up to ~40 deg near its rim, so it shows the world turned over and
   shrunk -- the dark ground in the drops against the sky, the bright sky in the drops against the ground; beyond
   ~0.75 of its radius the ray meets the cap past the critical angle and is turned back into the cockpit (total
   internal reflection): a dark rim. At speed the airflow drags each bead aft and its receding (upstream) side leaves a
   thin trail of film, often broken into droplets. The same cells, hashes, centres and radii as bead_layer /
   rivulet_layer (the classic look): the same drops in the same places, plus the rain beads (cv_drops2) where the
   water comes hard. Each returns the coverage; rho = the distance from the drop's centre (or its trail's axis) along
   the glass over the half width of its slice there (1 at the rim), rv = the unit direction of that offset along the
   glass (cockpit frame), rpx = the half width in pixels; kc = the cap's steepness (1 the head, the trail's film
   flatter). */
/* (cv_drops2) the average transmission a bead under ~4 px fades to (with a 90 deg contact angle its turned-back rim is
   ~44 % of its area); the rain beads' amount at ACZ_VR_CANOPY_RAIN_BEADS 1 */
const float DROP_AVG = 0.6, RAIN_BEADS = 4.0;
float bead_lens(vec3 p, vec3 n, float s, float off, float stretch, float tail, float prob, float rmin, float rmax,
               float px, float rk, float ex, out float rho, out vec3 rv, out float kc, out float rpx, out float u) {
    vec3 q = p / s;
    q.z = q.z / stretch - off;
    ivec3 c = ivec3(floor(q));
    vec3 f = fract(q) - 0.5;
#ifdef GXV
    /* (GXV_EARLY: an empty cell returns before its second hash and its drop's draws -- the same test on the same first
       hash; u is read only where this drop covers the pixel) */
    if (GXOFF(GXV_EARLY) && float(pcg3(uvec3(c & 4095)).x) * (1.0 / 4294967296.0) >= prob) {
        rho = 1.0; rv = vec3(0.0); kc = 1.0; rpx = 0.0; u = 0.0;
        return 0.0;
    }
#endif
    vec4 h = hash4(c);
    u = fract(h.x * 7.13 + h.w * 3.7);
    float r = mix(rmin, rmax, u * u * u);
    vec3 o = (h.yzw - 0.5) * (0.9 - 2.0 * r);
    if (rk != 1.0 || ex != 3.0)
        r = min(mix(rmin, rmax, pow(u, ex)) * rk, 0.48 - max(abs(o.x), max(abs(o.y), abs(o.z))));
    rho = 1.0; rv = vec3(0.0); kc = 1.0; rpx = 0.0;
    if (h.x >= prob) return 0.0;
    /* the cell in true proportions (cell units, z aft x the cell's stretch): a round head and room behind it */
    vec3 w = vec3(f.xy - o.xy, (f.z - o.z) * stretch);
    float hv = fract(h.w * 13.7 + h.y * 5.1);              /* the drop's own second draw */
    /* the trail upstream (-z): longer at speed, each drop its own length, kept inside the cell */
    float L = min(tail * r * (0.35 + 0.95 * hv), max((o.z + 0.47) * stretch - 0.25 * r, 0.0));
    float t = L > 0.0 ? clamp(-w.z / L, 0.0, 1.0) : 0.0;
    vec3 v = w + vec3(0.0, 0.0, t * L);                    /* from the nearest point of the drop's axis */
    float rr = r * mix(1.0, 0.2, sqrt(t));                  /* a thick head, a thin trail */
    /* (exact early out: the droplets only narrow the drop and the lumps move it by at most 0.087 of its width, so
       nothing beyond 1.09 x the width reaches the drop -- most of a cell: the rain beads made this matter) */
    if (dot(v, v) > 1.19 * rr * rr) return 0.0;
    if (hv > 0.45) rr *= 1.0 - 0.6 * smoothstep(0.12, 0.3, t) * (0.5 - 0.5 * cos(t * L / r * 4.4));   /* droplets */
    v += rr * 0.05 * sin(v.yzx * (4.5 / rr) + hv * 6.2832);   /* merged drops are a little lumpy, not circles */
    float hd = dot(v, n);                                   /* across the glass: the glass cuts the cell's drop here */
    float rs2 = rr * rr - hd * hd;
    if (rs2 <= 0.0) return 0.0;
    float rs = sqrt(rs2);
    vec3 vp = v - n * hd;
    float lp = length(vp);
    rho = lp / rs;
    rv = lp > 1e-6 ? vp / lp : vec3(0.0);
    kc = mix(1.0, 0.55, t);
    float aa = max(px / (s * rs), 1e-3);                    /* a pixel in units of the half width */
    rpx = 1.0 / aa;
    float cover = clamp(1.6 / aa - 0.6, 0.0, 1.0);          /* below ~1.5 px they fade out instead of flickering */
    aa = min(aa, 0.5);
    return (1.0 - smoothstep(1.0 - aa, 1.0, rho)) * cover;
}
/* (lensdrops) a rivulet: the classic line, wandering across as it runs (it follows the wet path, not a ruler), with a bead
   at its head (aft, the way it runs) and its trail fading upstream; a cylinder of water across, so the same lens */
float rivulet_lens(vec3 p, vec3 n, float s, float len, float off, float prob, float px, float wk, out float rho,
                  out vec3 rv, out float rpx, out float u) {
    vec3 q = vec3(p.x / s, p.y / s, p.z / (s * len) - off);
    ivec3 c = ivec3(floor(q));
    vec3 f = fract(q) - 0.5;
#ifdef GXV
    if (GXOFF(GXV_EARLY) && float(pcg3(uvec3((c + ivec3(1371, 2087, 593)) & 4095)).x) * (1.0 / 4294967296.0) >= prob) {
        rho = 1.0; rv = vec3(0.0); rpx = 0.0; u = 0.0;                         /* (as bead_lens's GXV_EARLY) */
        return 0.0;
    }
#endif
    vec4 h = hash4(c + ivec3(1371, 2087, 593));
    vec2 o = (h.yz - 0.5) * 0.6;
    float w = mix(0.035, 0.09, h.w) * wk;                  /* half width (cells) */
    u = h.w;
    rho = 1.0; rv = vec3(0.0); rpx = 0.0;
    if (h.x >= prob) return 0.0;
    float t = f.z + (h.w - 0.5) * 0.3;                     /* along the line (aft +) */
    o += vec2(0.06, 0.04) * sin(vec2(8.0, 5.0) * f.z + h.yz * 6.2832);
    float along = smoothstep(-0.48, -0.2, t) * (1.0 - smoothstep(0.3, 0.42, t));
    float hb = (t - 0.3) * 20.0;
    w *= 0.7 + 0.3 * along + 1.1 * exp(-hb * hb);           /* the head's bead */
    vec3 v = vec3(f.xy - o, 0.0);
    float hd = dot(v, n);
    float rs2 = w * w - hd * hd;
    if (rs2 <= 0.0) return 0.0;
    float rs = sqrt(rs2);
    vec3 vp = v - n * hd;
    float lp = length(vp);
    rho = lp / rs;
    rv = lp > 1e-6 ? vp / lp : vec3(0.0);
    float aa = max(px / (s * rs), 1e-3);
    rpx = 1.0 / aa;
    float cover = clamp(1.6 / aa - 0.6, 0.0, 1.0);
    aa = min(aa, 0.5);
    return (1.0 - smoothstep(1.0 - aa, 1.0, rho)) * smoothstep(0.0, 0.15, along) * cover;
}
/* debris flecks (wish #47): where a bit of a destroyed aircraft hit the glass (ps2_video.c canopy_marks; su.xyz on the
   glass, one fleck each: its direction from the eye point = the cockpit frame's origin octahedral-encoded in 2 x 8 bits
   from bit 7, strength class 2 bits, the share left 5 bits; 0 = none). A small soot fleck, 5-9 mm in radius with a
   ragged edge and a darker middle, a few 2 mm specks thrown around it and a faint pale scuff ring (the coating
   scattering light); as it fades it smears aft with the airflow (along the glass towards +z). Never a crack, never
   opaque: at most ~0.55 alpha in its middle. Premultiplied colour + alpha over the water's lower layers. */
vec3 fleck_dir(uint u) {
    vec2 e = vec2(float((u >> 15u) & 255u), float((u >> 7u) & 255u)) * (1.0 / 127.5) - 1.0;
    vec3 d = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    float t = max(-d.z, 0.0);
    d.xy += vec2(d.x >= 0.0 ? -t : t, d.y >= 0.0 ? -t : t);
    return normalize(d);
}
/* a fleck within 4 cm of this point of the glass (the rest of the glass returns at once) */
bool fleck_near(vec3 p) {
    for (int i = 0; i < 3; i++) {
        if (cp.su[i] < 0.5) continue;
        uint u = uint(cp.su[i] + 0.25);              /* (+ 0.25: codes past 2^23 have no halves in float) */
        vec3 m = fleck_dir(u);
        float t = dot(p, m);
        vec3 q = p - m * t;
        if (t > 0.05 && dot(q, q) < ((u >> 23u) != 0u ? 0.0036 : 0.0016)) return true;   /* (bug #260: 6 cm, a big one) */
    }
    return false;
}
vec4 debris_flecks(vec3 p, vec3 n, vec3 base, vec3 amb, float px) {
    vec4 W = vec4(0.0);
    for (int i = 0; i < 3; i++) {
        float code = cp.su[i];
        if (code < 0.5) continue;
        uint u = uint(code + 0.25);                    /* (bit 23: + 0.5 would round up to the next code) */
        float fq = float(u & 31u) / 31.0, s0 = 0.4 + 0.2 * float((u >> 5u) & 3u);
        vec3 m = fleck_dir(u);
        float t = dot(p, m);
        if (t <= 0.05) continue;
        vec3 q = p - m * t;                                    /* off the fleck's line of sight (m) */
        /* bit 23 (ACZ_IMM_DEBRIS_LOOK, bug #260): 1.5 x across (7.5-13.5 mm), its specks as far out, x 1.35 darker */
        bool big = (u >> 23u) != 0u;
        float gs = big ? 1.5 : 1.0;
        if (dot(q, q) > (big ? 0.0036 : 0.0016)) continue;    /* beyond 4 cm (a big one 6 cm): none */
        vec3 ta = vec3(0.0, 0.0, 1.0) - n * n.z;               /* aft along the glass */
        ta = dot(ta, ta) > 1e-4 ? normalize(ta) : normalize(cross(n, vec3(1.0, 0.0, 0.0)));
        vec3 tc = cross(n, ta);
        vec2 o = vec2(dot(q, tc), dot(q, ta));                 /* across, aft (m) */
        float h = fract(float((u >> 7u) & 0xFFFFu) * 0.6180339);   /* the fleck's own draw, from its place */
        float age = 1.0 - fq;
        float sm = 1.0 + 2.2 * age;                            /* smeared aft as it fades */
        vec2 os = vec2(o.x, o.y > 0.0 ? o.y / sm : o.y * 1.4);
        float R = (0.005 + 0.004 * h) * (1.0 + 0.25 * age) * gs;
        float rr = length(os);
        float edge = vnoise(vec3(o * 1500.0, h * 31.0));
        float aa = max(px, 2e-4);
        float core = 1.0 - smoothstep(R * (0.72 + 0.25 * edge) - aa, R * (0.86 + 0.25 * edge) + aa, rr);
        core *= 0.7 + 0.3 * vnoise(vec3(o * 4200.0, h * 7.0));    /* soot is grainy, not a disc */
        float mid = 1.0 - smoothstep(0.0, R * 0.6, rr);        /* darker where it struck */
        vec2 gq = os * 330.0;                                  /* specks: ~3 mm cells, one in five holds one */
        float hs = hash1(ivec3(ivec2(floor(gq)), int(h * 997.0)));
        float sp = step(0.8, hs) * (1.0 - smoothstep(0.15, 0.35, length(fract(gq) - 0.5)))
                   * (1.0 - smoothstep(R * 1.4, 0.022 * gs, rr)) * step(R, rr);
        sp *= clamp(0.003 / max(px, 1e-5) - 0.5, 0.0, 1.0);    /* below a pixel they fade instead of flickering */
        float ring = smoothstep(R * 0.9, R * 1.2, rr) * (1.0 - smoothstep(R * 1.2, R * 1.9, rr));
        float k = s0 * fq * (big ? 1.35 : 1.0);
        float ar = ring * 0.06 * k, a = min((core * 0.38 + mid * 0.17 + sp * 0.35) * k, 0.8);
        vec3 soot = mix(base * 0.5, vec3(0.09, 0.08, 0.07), 0.65), pale = amb * 1.3 + 0.06;
        W = vec4(pale * ar + W.rgb * (1.0 - ar), ar + W.a * (1.0 - ar));
        W = vec4(soot * a + W.rgb * (1.0 - a), a + W.a * (1.0 - a));
    }
    return W;
}
/* a cockpit-frame direction (an angle in radians, small) to a texture-coordinate offset in the frame image */
vec2 dir_uv(vec3 a) {
    return vec2(dot(cp.rot[0].xyz, a) * cp.proj.x * cp.misc.x, -dot(cp.rot[1].xyz, a) * cp.proj.y * cp.misc.y) * 0.5;
}
/* a cockpit-frame direction (any angle in front of the eye) to its place in the frame image, less the image's centre
   (cockpit.vert's projection; the difference of two is an exact offset) */
vec2 view_uv(vec3 D) {
    float w = max(-dot(cp.rot[2].xyz, D), 0.05);
    return vec2(dot(cp.rot[0].xyz, D) * cp.proj.x * cp.misc.x, -dot(cp.rot[1].xyz, D) * cp.proj.y * cp.misc.y) * (0.5 / w);
}
/* (rain_layers 2026-10-30, ACZ_VR_CANOPY_LAYERS: a modern game's canopy is a reflection mesh and a weather mesh "separated by the
   thickness of the glass") the point of the glass's outer face this eye sees through the inner face's point p: the
   eye's own ray refracted into the acrylic (n 1.49) and carried through the glass's thickness thk (m) along it -- a
   parallel slab, exact for glass that is flat over a few millimetres. Each eye reaches the outer face at its own place,
   so the weather evaluated there sits at the outer face's depth in stereo and with head motion: behind the inner
   face's reflections, wear and cracks by the glass's apparent thickness (~thk / 1.49 square on). n faces the eye. */
vec3 glass_outer(vec3 p, vec3 n, float thk) {
    float eye = cp.misc.w;
#ifdef MV
    eye = gl_ViewIndex == 0 ? -1.0 : 1.0;
#endif
    vec3 I = normalize(p - cp.head.xyz - eye * cp.head.w * cp.rot[0].xyz);
    vec3 t = refract(I, n, 0.6711);
    return p + t * (thk / max(-dot(t, n), 0.25));
}
/* the water layer over the glass colour gc (premultiplied; the glass is nearer than the water on its outside); wash =
   how much water, film, frost or grime covers this spot (0..1: the glass's own scratches fill with water and hide) */
vec4 canopy_water(vec3 p, vec3 n, vec3 V, vec3 Ln, vec3 amb, vec3 sun, float shade, vec4 gc, float thk, out float wash) {
    wash = 0.0;
#ifdef GXV
    if (GXOFF(GXV_WATER)) return gc;            /* (a dry glass without flecks: the test below returns gc for it too) */
#endif
    float wet = cp.wx.x, mist = cp.wx.w, grime = cp.uvoff.z, st = max(cp.uvoff.w, 1.0), soot = 0.0;
    if (grime < 0.0) {                                     /* (soot packed with the grime: ps2_video.c, bug #139) */
        float q = -grime - 1.0, gq = floor(q * (1.0 / 256.0) + 1e-3);
        soot = (q - gq * 256.0) * (1.0 / 255.0); grime = gq * (1.0 / 255.0);
    }
#ifdef GXV
    if (GXOFF(GXV_GRIME)) grime = 0.0;          /* (each bit: the CPU found the same value in this draw's push constants) */
    if (GXOFF(GXV_SOOT)) soot = 0.0;
    if (GXOFF(GXV_MIST)) mist = 0.0;
    if (GXOFF(GXV_DROPS)) wet = 0.0;            /* (wet <= 0: no drop layers; wet is only compared with 0) */
#endif
    /* (ACZ_VR_CANOPY_DROPS=lensdrops: sv.z = the size + 4 x the refraction's strength in quarters (1..16) + 128 x the beads'
       contact angle in 5 deg steps above 45 deg) */
    float aq = floor(cp.sv.z * 0.25), ai = floor(aq * (1.0 / 32.0)), sz = cp.sv.z - 4.0 * aq;
    aq -= 32.0 * ai;
    float ak = aq * 0.25;
    bool lensdrops = aq >= 1.0;
#ifdef GXV
    if (GXOFF(GXV_CLASSIC)) lensdrops = true;         /* (one drop look in the binary) */
    if (GXOFF(GXV_LENS)) lensdrops = false;
#endif
    float dsin = sin(radians(45.0 + 5.0 * ai));             /* the beads' contact angle (ACZ_VR_CANOPY_DROP_ANGLE) */
    /* (lensdrops: su.w = the drop intensity + 2 x the rain beads' amount in quarters, ACZ_VR_CANOPY_RAIN_BEADS) */
    float rq = lensdrops ? floor(max(cp.su.w, 0.0) * 0.5) : 0.0, dint = cp.su.w - 2.0 * rq, rbk = rq * 0.25;
    float smear = cp.sv.y, size = lensdrops ? sz : (cp.sv.z > 0.0 ? cp.sv.z : 1.0), gain = cp.sv.w > 0.0 ? cp.sv.w : 1.0;
#ifdef GXV
    if (GXOFF(GXV_GRIME)) smear = 0.0;
#endif
    bool flecks = max(cp.su.x, max(cp.su.y, cp.su.z)) > 0.5 && fleck_near(p);
#ifdef GXV
    if (GXOFF(GXV_FLECK)) flecks = false;
#endif
    if (wet <= 0.0 && mist <= 0.0 && grime <= 0.0 && smear <= 0.0 && cp.sd.w <= 0.0 && cp.sd.y <= 0.0 && !flecks
        && soot <= 0.0) return gc;
    /* (rain_layers, ACZ_VR_CANOPY_LAYERS: thk > 0) from here on the outside weather lies on the glass's outer face (a dry
       glass returned above: no cost there) */
    if (thk > 0.0) p = glass_outer(p, n, thk);
    bool have = mod(cp.sv.x, 2.0) > 0.5;
    vec2 tsz = have ? vec2(textureSize(world, 0).xy) : vec2(1.0);
    vec2 uv0 = gl_FragCoord.xy / tsz;
    float px = length(fwidth(p)) * 0.7;
    vec3 base = have ? WORLD(uv0) : amb;
    /* canopy ice (ps2_video.c canopy_update): frz = the share of the water frozen where it sits, frost = how far rime
       and frost have grown in from the sides */
    float ipk = max(cp.sd.y, 0.0);
#ifdef GXV
    if (GXOFF(GXV_ICE)) ipk = 0.0;
#endif
    bool rime = ipk >= 1048576.0;                          /* (rain_layers, ACZ_VR_CANOPY_RIME: + 2^20) */
    if (rime) ipk -= 1048576.0;
    float ifz = floor(ipk * (1.0 / 1024.0));
    float frz = ifz * (1.0 / 1023.0), frost = (ipk - ifz * 1024.0) * (1.0 / 1023.0);
    /* drop intensity: how hard the water comes (a dense cloud core or a downpour: larger drops, more of the large
       ones, streaming rivulets; a thin veil or drizzle: fine sparse droplets); 0.45 = exactly the drops as before */
    float di = clamp(dint, 0.0, 1.0) - 0.45;
    float rk = 1.0 + 0.6 * di, ex = 3.0 - 2.4 * di, pf1 = 1.0 + 1.6 * di, pf2 = 1.0 - 0.55 * di;
    float pv = 1.0 + di * (di > 0.0 ? 3.6 : 1.65), wk = 1.0 + 0.45 * di;

    /* where the water gathers: clumps (~6 cm), more towards the sides, the airflow clears the middle of the screen */
    float side = smoothstep(0.06, 0.35, abs(p.x));
    /* (the clump noise also shapes the frost's front; on a glass with frost but no water it is only looked up where
       the frost can reach) */
#ifdef GXV
    float vn0 = wet > 0.0 ? (GXOFF(GXQ_CLUMP) ? cq_clump(p) : vn_w(p * 16.0 + vec3(3.1, 7.7, 1.3))) : 0.5;
#else
    float vn0 = wet > 0.0 ? vnoise(p * 16.0 + vec3(3.1, 7.7, 1.3)) : 0.5;
#endif
    float dens = 0.3 + 0.7 * smoothstep(0.3, 0.75, vn0);
    dens *= mix(1.0 - 0.55 * cp.sd.x, 1.0, side);

    /* drops: big and fine beads, rivulets running aft (none to look up on a dry glass that only has frost) */
    float d1 = 0.0, d2 = 0.0, d3 = 0.0, u1 = 0.0, u2 = 0.0, u3 = 0.0, m1 = 0.0, m2 = 0.0, m3 = 0.0;
    vec3 g1 = vec3(0.0), g2 = vec3(0.0), g3 = vec3(0.0);
    float k1 = 1.0, k2 = 1.0, x1 = 0.0, x2 = 0.0, x3 = 0.0;    /* (lensdrops: steepness, half width in pixels) */
    float m4 = 0.0, d4 = 0.0, u4 = 0.0, k4 = 1.0, x4 = 0.0;   /* (lensdrops: the rain beads) */
    vec3 g4 = vec3(0.0);
    if (wet > 0.0 && lensdrops) {     /* g1..g4 carry the unit direction along the glass, d1..d4 rho */
        float tl = 1.1 * (st - 1.0);                       /* the trail: ~1.8 radii at 250 kt, ~3.3 at 450 kt */
        m1 = bead_lens(p, n, 0.014 * size, cp.wx.y, 1.0 + 0.3 * (st - 1.0), tl, 0.7 * wet * dens * pf1, 0.08, 0.4, px,
                      rk, ex, d1, g1, k1, x1, u1);
        m2 = bead_lens(p + 0.37, n, 0.007 * size, cp.wx.y * 1.7, 1.0 + 0.5 * (st - 1.0), tl, 0.6 * wet * dens * pf2, 0.1,
                      0.34, px, rk, ex, d2, g2, k2, x2, u2);
        m3 = rivulet_lens(p + 1.13, n, 0.016 * size, 2.0 + 2.5 * st, cp.wx.z,
                         0.35 * wet * mix(0.4, 1.0, side) * cp.sd.x * pv, px, wk, d3, g3, x3, u3);
        /* rain beads (cv_drops2): where the water comes hard -- real rain, a dense cloud core: drop intensity above
           ~0.3 -- a third layer of small beads (~1.8-5.5 mm, sizes evenly spread rather than mostly fine), as many as
           the water and its intensity give: a modern game's downpour covers up to a quarter of the glass. Thin water (a veil, a
           cloud's edge, the fine droplets left after a cloud: intensity ~0.3) has none, so the clearing stays */
        float pr = RAIN_BEADS * rbk * wet * dens * smoothstep(0.3, 0.8, clamp(dint, 0.0, 1.0));
        if (pr > 0.0)
            m4 = bead_lens(p + 0.71, n, 0.0065 * size, cp.wx.y * 1.3, 1.0 + 0.4 * (st - 1.0), tl, pr, 0.14, 0.42, px, 1.0,
                          1.5, d4, g4, k4, x4, u4);
    } else if (wet > 0.0) {
        m1 = bead_layer(p, 0.014 * size, cp.wx.y, 1.0 + 0.3 * (st - 1.0), 0.7 * wet * dens * pf1, 0.08, 0.4, px, rk, ex,
                        d1, g1, u1);
        m2 = bead_layer(p + 0.37, 0.007 * size, cp.wx.y * 1.7, 1.0 + 0.5 * (st - 1.0), 0.6 * wet * dens * pf2, 0.1, 0.34,
                        px, rk, ex, d2, g2, u2);
        m3 = rivulet_layer(p + 1.13, 0.016 * size, 2.0 + 2.5 * st, cp.wx.z,
                           0.35 * wet * mix(0.4, 1.0, side) * cp.sd.x * pv, px, wk, d3, g3, u3);
    }
    float m = max(m1, max(m2, m3));
    vec3 g = g1 + g2 * (1.0 - m1) + g3 * (1.0 - max(m1, m2));
    float dr = m1 >= max(m2, m3) ? d1 : (m2 >= m3 ? d2 : d3);
    float du = m1 >= max(m2, m3) ? u1 : (m2 >= m3 ? u2 : u3);
    vec3 gt = g - n * dot(g, n);                           /* the cap's slope along the glass */
    float tl = length(gt);
    vec3 tilt = tl > 0.0 ? gt / tl * (tl / sqrt(1.0 + tl * tl)) : vec3(0.0);    /* sine of the cap's tilt */
    /* (lensdrops: the top drop's direction along the glass, steepness, half width in pixels) */
    vec3 rva = m1 >= max(m2, m3) ? g1 : (m2 >= m3 ? g2 : g3);
    float kca = m1 >= max(m2, m3) ? k1 : (m2 >= m3 ? k2 : 0.6), rpa = m1 >= max(m2, m3) ? x1 : (m2 >= m3 ? x2 : x3);
    if (m4 > m) { m = m4; dr = d4; du = u4; rva = g4; kca = k4; rpa = x4; }

    /* mist: the world a little diffused and paler behind a fine film (cloud) */
    vec4 W = vec4(0.0);
    if (mist > 0.0) {
        float am = mist * 0.35 * gain;
        vec3 mc = amb * 1.4 + 0.08;
        if (have) {
            vec2 r = 3.0 / tsz;
            vec3 bl = (WORLD(uv0 + r) + WORLD(uv0 - r) + WORLD(uv0 + vec2(r.x, -r.y)) + WORLD(uv0 + vec2(-r.x, r.y))) * 0.25;
            mc = mix(bl, mc, 0.25);
        } else am *= 0.3;
        W = vec4(mc * am, am);
    }
    /* condensation: humid air from the cloud fogs the glass in patches that grow over seconds from the frame and the
       sides inwards (the airflow keeps the middle of the windscreen clearest) and clear slowly after the cloud; behind a
       fogged patch the world is frosted (a wide blur, rotated per spot of glass so it reads as a texture of the glass,
       not of the screen) and lifted towards the light it scatters */
    float film = cp.sd.w;
#ifdef GXV
    if (GXOFF(GXV_FILM)) film = 0.0;
#endif
    if (film > 0.0) {
        float edge = smoothstep(0.08, 0.42, abs(p.x)) * 0.6 + smoothstep(0.1, 0.6, p.z + 0.25) * 0.4;
#ifdef GXV
        /* (GXQ_FILM: the second octave as two sines that wander with the first octave: ragged patch edges without a lookup) */
        float fn = vn_w(p * 34.0 + vec3(1.7, 4.1, 9.3));
        fn = GXOFF(GXQ_FILM) ? fn * 0.62 + 0.38 * (0.5 + 0.11 * (sin((p.x + p.z) * 280.0 + fn * 9.0) + sin((p.y - p.z) * 255.0 - fn * 7.0)))
                             : fn * 0.62 + vn_w(p * 97.0 + 3.3) * 0.38;
#else
        float fn = vnoise(p * 34.0 + vec3(1.7, 4.1, 9.3)) * 0.62 + vnoise(p * 97.0 + 3.3) * 0.38;
#endif
        float th = 1.02 - film * (0.75 + 0.55 * edge);
        float fm = smoothstep(th - 0.1, th + 0.14, fn) * mix(0.55, 1.0, edge);
        if (fm > 0.003) {
            float af = fm * 0.5 * min(gain, 1.3);
            vec3 fc = amb * 1.5 + 0.1;
            if (have) {
                ivec3 gp = ivec3(floor(p * 900.0));
                float rot = hash1_w(gp) * 6.2831853, cs = cos(rot), sn = sin(rot);
                vec2 r = (5.0 + 9.0 * fm) / tsz;
                vec3 bl = vec3(0.0);
                const vec2 P[6] = vec2[](vec2(1.0, 0.0), vec2(0.5, 0.87), vec2(-0.5, 0.87), vec2(-1.0, 0.0),
                                         vec2(-0.5, -0.87), vec2(0.5, -0.87));
#ifdef GXV
                /* (GXQ_FILM: the same reach with fewer taps) */
                if (GXOFF(GXQ_FILM)) {
                    for (int i = 0; i < 6; i += 2) {
                        vec2 o = vec2(cs * P[i].x - sn * P[i].y, sn * P[i].x + cs * P[i].y) * 0.8;
                        bl += WORLD(uv0 + o * r) * 2.0;
                    }
                } else
#endif
                for (int i = 0; i < 6; i++) {
                    vec2 o = vec2(cs * P[i].x - sn * P[i].y, sn * P[i].x + cs * P[i].y) * (i % 2 == 0 ? 1.0 : 0.55);
                    bl += WORLD(uv0 + o * r);
                }
                fc = mix(bl / 6.0, fc, 0.3);
            } else af *= 0.4;
            W = vec4(fc * af + W.rgb * (1.0 - af), af + W.a * (1.0 - af));
        }
    }
    /* grime from debris: faint translucent smudges, streaked aft; wet smears slide off with the airflow */
    if (grime > 0.0 || smear > 0.0) {
        vec3 gp = vec3(p.x * 11.0, p.y * 11.0, p.z * 7.0 - cp.wx.z * 0.02);
#ifdef GXV
        float gn = GXOFF(GXQ_GRAIN) ? 0.5 + 0.72 * (vn_w(gp * 1.5) - 0.5) : vn_w(gp) * 0.6 + vn_w(gp * 3.7 + 5.0) * 0.4;
#else
        float gn = vnoise(gp) * 0.6 + vnoise(gp * 3.7 + 5.0) * 0.4;
#endif
        float gs = smoothstep(0.55, 0.9, gn);
        float ag = gs * grime * 0.14 * gain;                   /* a dusty film, never a blot */
        vec3 gcol = mix(base * 0.75, vec3(0.32, 0.29, 0.25), 0.5);
        W = vec4(gcol * ag + W.rgb * (1.0 - ag), ag + W.a * (1.0 - ag));
        if (smear > 0.0 && have) {
            float sm = smoothstep(0.45, 0.8, vn_w(gp * 0.7 + 11.0)) * smear;
            vec2 off = dir_uv(vec3(0.0, 0.0, 0.025));             /* the water film shifts the view aft a little */
            vec3 sc = WORLD(uv0 + off * sm) * 0.97 + amb * 0.06 * sm;
            float as = sm * 0.85;
            W = vec4(sc * as + W.rgb * (1.0 - as), as + W.a * (1.0 - as));
        }
    }
    /* soot from smoke (bug #139, ACZ_IMM_SOOT): fine particles impacted on the faces the airflow meets (the windscreen and
       the canopy's front: the glass's inward normal pointing aft), streaked aft by the flow, a faint brownish grey that
       dims and veils the world behind a little; never opaque (at most ~0.22) */
    if (soot > 0.0) {
        float front = 0.3 + 0.7 * smoothstep(-0.2, 0.6, n.z);
        vec3 sq = vec3(p.x * 55.0, p.y * 55.0, p.z * 7.0 - cp.wx.z * 0.01);
#ifdef GXV
        float sn = GXOFF(GXQ_GRAIN) ? 0.5 + 0.74 * (vn_w(sq * 1.5) - 0.5) : vn_w(sq) * 0.65 + vn_w(sq * 3.1 + 7.0) * 0.35;
        float sg = GXOFF(GXQ_GRAIN) ? 0.5 : mix(0.5, vn_w(p * 420.0 + 2.0), clamp(0.0024 / max(px, 1e-5) - 0.5, 0.0, 1.0));
#else
        float sn = vnoise(sq) * 0.65 + vnoise(sq * 3.1 + 7.0) * 0.35;           /* streaks along the flow */
        float sg = vnoise(p * 420.0 + 2.0);                                     /* fine grain */
        sg = mix(0.5, sg, clamp(0.0024 / max(px, 1e-5) - 0.5, 0.0, 1.0));
#endif
        float as = soot * front * (0.08 + 0.14 * smoothstep(0.35, 0.8, sn)) * (0.75 + 0.5 * sg) * min(gain, 1.3);
        vec3 sc = mix(base * 0.55, vec3(0.11, 0.10, 0.09) + amb * 0.12, 0.6);
        W = vec4(sc * as + W.rgb * (1.0 - as), as + W.a * (1.0 - as));
    }
    /* debris flecks (under the water) */
    if (flecks) {
        vec4 F = debris_flecks(p, n, base, amb, px);
        W = vec4(F.rgb + W.rgb * (1.0 - F.a), F.a + W.a * (1.0 - F.a));
    }
    /* the drops over all of it */
    if (m > 0.0) {
        vec3 N, H = normalize(Ln + V), Hs = normalize(vec3(0.0, 1.0, 0.0) + V);
        float rim = smoothstep(0.72, 1.0, dr), glint, sky;
        vec3 dc;
        float ad;
        if (lensdrops) {
            /* (lensdrops) the drop as a lens of water on the glass, ray by ray for this eye: into the water through the flat
               glass (air -> water: the acrylic between them is a parallel plate), out through the cap -- Snell's law at
               its slope (sine = rho x dsin x steepness), or turned back past the critical angle (the dark rim) */
            float eye = cp.misc.w;
#ifdef MV
            eye = gl_ViewIndex == 0 ? -1.0 : 1.0;
#endif
            vec3 I = normalize(p - cp.head.xyz - eye * cp.head.w * cp.rot[0].xyz);
            float sa = min(dr, 1.0) * dsin * kca;
            vec3 Nc = rva * sa - n * sqrt(1.0 - sa * sa);           /* the cap's outward normal (n faces the eye) */
            vec3 t1 = refract(I, n, 0.75);
            float ci = dot(t1, Nc);
            float s2 = 1.333 * sqrt(max(1.0 - ci * ci, 0.0));       /* sine of the way out (> 1: turned back) */
            float tr = 1.0 - smoothstep(0.93, 1.0, s2);             /* (the transmission falls towards the critical angle) */
            vec3 t2 = s2 < 1.0 ? refract(t1, -Nc, 1.333) : I;
            /* below ~4 px of half width the turned-over image and the rim would sparkle: towards their average (a bead
               with its rim; a rivulet's flat film turns nothing back) */
            float sm = clamp((rpa - 1.5) * 0.4, 0.0, 1.0);
            tr = mix(mix(0.97, DROP_AVG, smoothstep(0.6, 1.0, kca)), tr, sm);
            N = -Nc;
            sky = pow(max(dot(N, Hs), 0.0), 60.0) * 0.35;
            /* the sun through the lens (a small image of it in each drop near the sun's direction) and, behind the pilot,
               off the turned-back rim */
            glint = (pow(max(dot(t2, Ln), 0.0), 300.0) * 1.5 * tr
                     + pow(max(dot(reflect(t1, Nc), Ln), 0.0), 200.0) * (1.0 - tr)) * shade * sm;
            if (have) {
                vec3 wv = WORLD(clamp(uv0 + (view_uv(t2) - view_uv(I)) * (ak * mix(0.4, 1.0, sm)), vec2(0.0), vec2(1.0)));
                vec3 back = base * 0.3 + amb * 0.12;                /* turned back into the cockpit: its dim interior */
                dc = mix(back, wv * 0.97, tr) + amb * (sky * 0.4) + sun * glint;
                ad = m;
            } else {
                dc = amb * (0.4 * (1.0 - tr) + sky) / max(0.05, 0.1 + 0.3 * (1.0 - tr)) + sun * glint;
                ad = m * (0.1 + 0.3 * (1.0 - tr));
            }
        } else {
            N = normalize(n + tilt * 1.2);
            float fr = pow(1.0 - max(dot(N, V), 0.0), 4.0);
            glint = pow(max(dot(N, H), 0.0), 260.0) * shade * 2.5;
            sky = pow(max(dot(N, Hs), 0.0), 60.0) * 0.35;
            if (have) {
                /* a lens: towards the rim it shows the world from further away, inverted */
                vec2 off = -dir_uv(tilt * 0.12);
                /* a lens turns the world over: the sky shows in its lower half, the ground in its upper half (kept even
                   against a flat overcast); a thin darker rim; the light it gathers brightens the far side a little */
                float up = dot(tilt, vec3(0.0, 1.0, 0.0));
                float turn = 1.03 + 0.28 * clamp(-up, 0.0, 1.0) - 0.2 * clamp(up, 0.0, 1.0);
                float caustic = clamp(dot(tilt, normalize(Ln)), 0.0, 1.0) * (1.0 - rim) * 0.12;
                dc = WORLD(uv0 + off) * (turn - 0.18 * rim + caustic) + amb * (0.2 * fr + sky) + sun * glint;
                ad = m;
            } else {
                dc = amb * (0.4 * fr + sky) / max(0.05, 0.1 + 0.3 * rim) + sun * glint;
                ad = m * (0.1 + 0.3 * rim);
            }
        }
        /* frozen where it sat (small drops first): milky ice that scatters instead of a lens -- the world straight
           behind shows only as a pale glow, the frozen-in air whitens the middle and the rim, crystal facets catch the
           sun in small glints; frozen rivulets are streaks of the same ice */
        float fz = frz > 0.0 ? smoothstep(0.0, 0.18, frz * 1.18 - du) : 0.0;
        if (fz > 0.0) {
            float gr = vnoise_i(p * 1500.0 + 2.7);             /* crystal mottling, ~0.7 mm */
            float gr2 = hash_i(ivec3(floor(p * 900.0)));        /* facets, ~1.1 mm */
            float core = 1.0 - smoothstep(0.05, 0.5, dr);      /* the air the freezing pushed to the middle */
            float milk = min(0.55 + 0.2 * core + 0.35 * rim + 0.25 * (gr - 0.5), 1.0);
            vec3 ice = amb * 1.55 + vec3(0.15, 0.16, 0.18);
            vec3 Nf = normalize(N + (vec3(gr2, gr, fract(gr * 7.3 + gr2)) - 0.5) * 0.9);
            float spk = pow(max(dot(Nf, H), 0.0), 160.0) * shade * 2.2 * step(0.72, gr2);
            /* clear ice still bends the light a little: a third of the lens image under the milk */
            vec3 ic = mix(mix(base, dc, 0.35), ice, max(milk, 0.0)) + amb * sky * 0.6 + sun * (spk + glint * 0.3);
            float ai = m * min(0.55 + 0.15 * core + 0.3 * rim + 0.1 * gr, 1.0);
            dc = mix(dc, ic, fz);
            ad = mix(ad, ai, fz);
        }
        ad *= min(gain, 1.0);
        W = vec4(dc * ad + W.rgb * (1.0 - ad), ad + W.a * (1.0 - ad));
    }
    /* rime and frost: in cloud above the freezing level it grows from the sides and the canopy's edges inwards (frost =
       how far). The cold side of the glass is judged by the angle off the nose seen from the cockpit frame's origin
       (the pilot's eye point; the same spot of glass for both eyes): the windscreen ahead is heated and swept by the
       airflow, so it stays clearest and only ever gets a few faint patches. The front is ragged (the clump noise above
       plus a fine one), the frost itself feathery (thin bright veins of ridged noise stretched along the growth
       direction), the world behind it frosted, a few crystals glinting in the sun. */
    if (frost > 0.0) {
        vec3 dir = p * inversesqrt(max(dot(p, p), 1e-4));
        float ax = 1.0 + dir.z;                                /* 0 ahead, 1 abeam, 2 behind */
        /* how cold this spot of glass runs: nothing within ~35 deg of the nose, most at the sides, the canopy top and
           the rails */
        float e = smoothstep(0.2, 1.05, ax + 0.3 * max(dir.y - 0.35, 0.0) + 0.25 * max(-dir.y - 0.2, 0.0));
        float th = 1.0 - 0.72 * frost;                         /* the front moves in as the frost grows */
        if (e + 0.24 > th - 0.02) {
#ifdef GXV
            if (wet <= 0.0) vn0 = GXOFF(GXQ_CLUMP) ? cq_clump(p) : vn_w(p * 16.0 + vec3(3.1, 7.7, 1.3));
#else
            if (wet <= 0.0) vn0 = vnoise(p * 16.0 + vec3(3.1, 7.7, 1.3));
#endif
            /* growth direction: along the glass towards the nose; feathers grow along it, barbs at 45 deg */
            vec3 tin = vec3(0.0, 0.0, -1.0) + dir * dir.z;
            tin -= n * dot(tin, n);
            tin *= inversesqrt(max(dot(tin, tin), 1e-6));
            vec3 q = p * 130.0;
            q -= tin * dot(q, tin) * 0.6;                      /* fronds ~2 cm long, ~8 mm apart */
            float fn = vnoise_i(q);
            float fe = e + 0.32 * (vn0 - 0.5) + 0.16 * (fn - 0.5);  /* a ragged front with fingers along the growth */
            float cov = smoothstep(th - 0.02, th + 0.05, fe);
            if (cov > 0.003 && rime) {
                /* (rain_layers 2026-10-30, ACZ_VR_CANOPY_RIME) in-flight icing is rime, not a window's frost ferns (those
                   grow by deposition in still air): the supercooled cloud droplets freeze where they strike, a granular
                   white crust mottled by the way the drops piled up, its feathers grown into the airflow (streaks along
                   the flow on the glass, ~2 mm across, ~2 cm along), thicker behind its front; as a modern game's mottled grey ice
                   layer (ice11 / ice12 and their pre-blurred copies). The world behind it is seen through a rough ice
                   surface: displaced by its facets (glass space: the same for both eyes) and blurred, more where it is
                   thick; never opaque (alpha <= ~0.55) */
                vec3 ta = vec3(0.0, 0.0, 1.0) - n * n.z;            /* aft along the glass (the flow) */
                ta = dot(ta, ta) > 1e-4 ? normalize(ta) : normalize(cross(n, vec3(1.0, 0.0, 0.0)));
                vec3 tc = cross(n, ta);
                /* (solid noise in the cockpit frame, as the rest of the weather: no parametrisation of the glass; the
                   feathers stretched along z, the flow, show along the flow's line on any pane) */
                float kd = clamp(0.0014 / max(px, 1e-5) - 0.6, 0.0, 1.0);   /* (fine grain faded where a pixel is coarser) */
                float gr1 = kd > 0.0 ? mix(0.5, vnoise_i(p * 700.0 + 1.3), kd) : 0.5;       /* grains ~1.4 mm */
                float gr2 = fn;               /* mottling: the front's own noise (~8 mm, drawn out along the flow) */
                float fe1 = mix(0.5, vnoise_i(vec3(p.x * 520.0, p.y * 520.0, p.z * 55.0) + 3.7),
                                clamp(0.002 / max(px, 1e-5) - 0.6, 0.0, 1.0));                /* feathers ~2 x 18 mm */
                float deep = smoothstep(th + 0.04, th + 0.4, fe);   /* behind the front the crust thickens */
                float h = 0.5 * gr2 + 0.25 * fe1 + 0.25 * gr1;
                float thick = clamp(mix(0.55 * h, 0.55 + 0.9 * (h - 0.5), deep), 0.0, 1.0);
                float af = cov * (0.07 + 0.45 * thick) * mix(0.3, 1.0, e) * min(gain, 1.3);
                vec3 ic = amb * 1.45 + vec3(0.13, 0.14, 0.16);
                vec3 fc = ic;
                if (have) {        /* the world through the rough ice: displaced by its facets, blurred by its grain */
                    vec2 o = dir_uv(tc * ((gr2 - 0.5) * 0.06) + ta * ((fe1 - 0.5) * 0.04));
                    float hg = hash_i(ivec3(floor(p * 900.0))) * 6.2831853;
                    vec2 r = (2.0 + 9.0 * thick) / tsz, cs = vec2(cos(hg), sin(hg));
                    /* (4 taps: two radii on a cross turned per crystal grain) */
                    vec3 bl = (WORLD(uv0 + o + r * cs * 0.5) + WORLD(uv0 + o - r * cs)
                               + WORLD(uv0 + o + r * vec2(-cs.y, cs.x) * 0.8) + WORLD(uv0 + o - r * vec2(-cs.y, cs.x) * 0.35)) * 0.25;
                    fc = mix(bl, ic, 0.25 + 0.5 * thick);
                } else af *= 0.6;
                vec3 cq = p * 650.0;                                /* glints: 1.5 mm crystals, a third of them */
                ivec3 ci = ivec3(floor(cq));
                float hgl = hash_i(ci);
                if (hgl > 0.66) {
                    vec3 Nf = normalize(n + (vec3(hash_i(ci + ivec3(17, 0, 0)), hgl, hash_i(ci + ivec3(0, 31, 0))) - 0.5) * 0.9);
                    float spot = 1.0 - smoothstep(0.15, 0.45, length(fract(cq) - 0.5));
                    fc += sun * pow(max(dot(Nf, normalize(Ln + V)), 0.0), 220.0) * shade * 2.0 * spot * cov;
                }
                W = vec4(fc * af + W.rgb * (1.0 - af), af + W.a * (1.0 - af));
            } else if (cov > 0.003) {
                /* fern-like fronds: a spine where the feather noise crosses its middle, barbs leaning towards the
                   growth on either side (chevrons; thin lines ~2 mm apart that fade to their average where a pixel
                   is too coarse for them -- px comes from outside this branch) */
                float sg = 2.0 * fn - 1.0;                          /* across a frond: 0 on its spine */
                vec3 bd = normalize(tin + cross(n, tin) * (sg > 0.0 ? 1.0 : -1.0));
                float ac = dot(p, cross(n, bd)) * 500.0 + 3.0 * vn0 + 1.5 * fn;   /* (curling a little) */
                float hw = 1.0 - abs(2.0 * fract(ac) - 1.0);
                hw *= hw; hw *= hw; hw *= hw;
                hw = mix(hw, 0.12, smoothstep(0.2, 0.5, 700.0 * px));
                float spine = 1.0 - abs(sg); spine *= spine; spine *= spine;
                /* each barb (per frond patch) its own length and strength, so the fronds do not read as a weave */
                float bi = hash_i(ivec3(floor(q)) + ivec3(int(floor(ac)) * 7, sg > 0.0 ? 3 : 0, 0));
                float barb = hw * (1.0 - smoothstep(0.08, 0.2 + 0.55 * bi, abs(sg))) * (0.45 + 0.55 * bi);
                float gn = hash_i(ivec3(floor(p * 1100.0)));        /* crystal grain ~0.9 mm */
                float deep = smoothstep(th + 0.04, th + 0.4, fe);   /* behind the front the crystals close up into rime */
                float fth = min(spine * 0.7 + barb * 0.9, 1.0) * (1.0 - 0.35 * deep);
                float af = cov * (0.05 + 0.2 * fth + (0.04 + 0.1 * deep) * gn + 0.2 * deep) * mix(0.3, 1.0, e) * min(gain, 1.3);
                vec3 ic = amb * 1.45 + vec3(0.13, 0.14, 0.16);
                vec3 fc = ic;
                if (have) {        /* frosted: the world behind blurred (one diagonal per crystal grain, 3 taps) */
                    vec2 r = (3.0 + 8.0 * deep) / tsz * vec2(1.0, gn > 0.5 ? 1.0 : -1.0);
                    vec3 bl = (base + WORLD(uv0 + r) + WORLD(uv0 - r)) * (1.0 / 3.0);
                    fc = mix(bl, ic, 0.3 + 0.4 * fth + 0.25 * deep);
                } else af *= 0.6;
                vec3 cq = p * 650.0;                                /* glints: 1.5 mm crystals, a third of them */
                ivec3 ci = ivec3(floor(cq));
                float hg = hash_i(ci);
                if (hg > 0.66) {
                    vec3 Nf = normalize(n + (vec3(hash_i(ci + ivec3(17, 0, 0)), hg, hash_i(ci + ivec3(0, 31, 0))) - 0.5) * 0.9);
                    float spot = 1.0 - smoothstep(0.15, 0.45, length(fract(cq) - 0.5));
                    fc += sun * pow(max(dot(Nf, normalize(Ln + V)), 0.0), 220.0) * shade * 2.0 * spot * cov;
                }
                W = vec4(fc * af + W.rgb * (1.0 - af), af + W.a * (1.0 - af));
            }
        }
    }
    /* the glass (inside) over the water */
    wash = clamp(max(m, W.a * 2.0), 0.0, 1.0);
    return vec4(gc.rgb + (1.0 - gc.a) * W.rgb, gc.a + (1.0 - gc.a) * W.a);
}

/* ---- the canopy as a material (fx_glass; ps2_video.c gfx_*: ACZ_VR_CANOPY_SCRATCH, _GLARE, _REFLECT) ----
   The acrylic's own wear, lit by the sun through it: hairline scratches, polishing and wiping swirls, pits, dust specks,
   dried water-spot rings and crazing near the frame. Each scatters some of the sunlight that passes the glass. A line
   (a scratch, an arc, a crack) keeps the light's direction along itself, so it sends light towards the eye only where
   its tangent t is at right angles to Q = S - D (S towards the sun, D from the eye through this spot): around the sun's
   image the lit bits line up on circles (the halo of a worn windscreen) and a swirl lights as a streak through its
   middle pointing at the sun. Only with the sun beyond the glass, falling off with the angle from it, dimmed by cloud
   shadow and cloud (the CPU's sun colour; the cockpit's sun map only where its light is the sun), hidden where water,
   film or frost fill the scratches (wash). All of it lives in the cockpit frame (glass space, no UVs needed: one 3D
   cell grid per layer, each feature inside a ball within its cell so it never crosses into the next cell): the same
   wear for both eyes, each eye with its own geometry as real scratches. A line is far thinner than a pixel, so it is
   drawn at least 1.3 pixels wide with its energy kept (true width / drawn width): steady without TAA. Inputs in the
   reserved light block (b[bi]): e[0] = sun (cockpit frame, unit), scratch strength; e[1] = sun colour x visibility,
   glare strength; e[2] = reflection strength, 1 = the sun map rows p[0..2] are valid, debug view
   (ACZ_DEBUG_CANOPY_GLASS_SHOW), the interior's parallax sphere (m); p[3].x 1 = one geometry for both eyes. Push
   light.w = -2 marks the canopy and windscreen. */
const float GL_SIG = 0.012;            /* a scratch's glint half-width (rad): the sun's radius plus waviness */
vec3 gx_off(int salt) { return vec3(0.31, 0.57, 0.13) * float(salt); }
/* four hash values from one pcg3 (hash4 runs it twice: the wear looks up several a pixel) */
vec4 gx_hash4(ivec3 c) {
    uvec3 h = pcg3(uvec3(c & 4095));
    return vec4(vec3(h), float(h.x * 0x9E3779B9u ^ h.z)) * (1.0 / 4294967296.0);
}
/* the feature of a layer's cell (cell size s): false when the cell has none or p lies outside its ball; d = p - the
   ball's centre, r3 = |d|, rho its radius */
bool gx_ball(vec3 p, float s, float prob, int salt, out vec3 d, out float r3, out float rho, out vec4 h, out ivec3 c) {
    vec3 q = p / s + gx_off(salt);
    vec3 f = floor(q);
    c = ivec3(f) + ivec3(salt * 1013, salt * 2027, salt * 3049);
    h = gx_hash4(c);
    d = vec3(0.0); r3 = 0.0; rho = 0.35 * s;
    if (h.x >= prob) return false;
    d = p - (f + 0.5 + (h.yzw - 0.5) * 0.3 - gx_off(salt)) * s;
    r3 = dot(d, d);
    if (r3 >= rho * rho) return false;
    r3 = sqrt(r3);
    return true;
}
/* hairlines: nor scratches a cell, at orientations phi0 + k pi / nor in the glass plane, each with its own place, length,
   width and a gentle bend (bend: 1/m); of them only the two whose orientations bracket the one that lights here
   (square to Q's part in the glass plane) are looked at, each weighted by a tent over the orientation step, so a
   scratch fades in and out smoothly as the head or the sun moves (no popping where the bracket changes). That makes
   a dense field (hundreds a square decimetre, as on a worn canopy) for the cost of two lines; the lit ones line up
   on circles around the sun's image. True width w0..w1 (m), length lmin..1 of the ball */
float gx_hairs(vec3 p, vec3 n, vec3 Q, float px, float s, float prob, float nor, float w0, float w1, float bend,
               float lmin, int salt) {
    vec3 d; float r3, rho; vec4 h; ivec3 c;
    if (!gx_ball(p, s, prob, salt, d, r3, rho, h, c)) return 0.0;
    vec4 g = gx_hash4(c + ivec3(77, 511, 911));
    vec3 e1 = g.xyz - 0.5;
    e1 -= n * dot(e1, n);
    float el = dot(e1, e1);
    if (el < 1e-3) return 0.0;
    e1 *= inversesqrt(el);
    vec3 e2 = cross(n, e1);
    vec2 qp = vec2(dot(Q, e1), dot(Q, e2));
    if (dot(qp, qp) < 1e-10) return 0.0;
    float stp = 3.14159265 / nor;
    float u = (atan(qp.y, qp.x) + 1.5707963 - g.w * 3.14159265) / stp;
    float k0 = floor(u), fr = u - k0;
    float sum = 0.0;
    for (int j = 0; j < 2; j++) {
        float kk = mod(k0 + float(j), nor);
        float ph = g.w * 3.14159265 + kk * stp;
        vec3 t = e1 * cos(ph) + e2 * sin(ph), bn = cross(n, t);
        vec4 hk = gx_hash4(c + ivec3(int(kk) * 131 + 17, salt * 7 + 3, 59));
        float al = dot(d, t) - (hk.y - 0.5) * 0.7 * rho, ac = dot(d, bn) - (hk.x - 0.5) * 1.3 * rho;
        float bk = (hk.z - 0.5) * bend;                    /* across = bk along^2 */
        ac -= bk * al * al;
        vec3 tt = normalize(t + bn * (2.0 * bk * al));
        float L = rho * mix(lmin, 1.0, hk.w);
        float ends = 1.0 - smoothstep(0.5 * L, L, abs(al));
        float w = mix(w0, w1, fract(hk.w * 7.31 + hk.x * 3.17));
        float a = max(1.3 * px, w);
        float prof = (1.0 - smoothstep(0.0, a, abs(ac))) * (w / a);
        float gq = dot(Q, tt);
        sum += (j == 0 ? 1.0 - fr : fr) * prof * ends * exp(-gq * gq * (1.0 / (GL_SIG * GL_SIG)));
    }
    return sum * (1.0 - smoothstep(0.75 * rho, rho, r3));
}
/* a polishing / wiping swirl: fine concentric arcs (rings dr apart, each over some of 8 sectors) around a centre; the
   arcs light where their tangent is square to Q, i.e. along the line through the centre towards the sun's image.
   Band limits: the glint widens where an arc turns more than it within a pixel; rings closer than ~2.5 pixels fade to
   their mean */
float gx_swirl(vec3 p, vec3 n, vec3 Q, float px, float s, float prob, int salt) {
    vec3 d; float r3, rho; vec4 h; ivec3 c;
    if (!gx_ball(p, s, prob, salt, d, r3, rho, h, c)) return 0.0;
    vec3 d2 = d - n * dot(d, n);
    float rr = length(d2);
    if (rr < 2e-4) return 0.0;
    vec3 u = d2 / rr, tt = cross(n, u);
    float gq = dot(Q, tt), qu = abs(dot(Q, u));
    float sig = max(GL_SIG, 1.5 * qu * px / rr);
    float gl = exp(-gq * gq / (sig * sig)) * (GL_SIG / sig);
    if (gl < 0.002) return 0.0;
    vec4 g = gx_hash4(c + ivec3(313, 71, 1777));
    vec3 e1 = g.xyz - 0.5;                                 /* the patch's own reference axis in the glass plane */
    e1 -= n * dot(e1, n);
    float el = dot(e1, e1);
    if (el < 1e-3) return 0.0;
    e1 *= inversesqrt(el);
    float ang = atan(dot(d2, cross(n, e1)), dot(d2, e1));
    float dr = mix(3e-4, 7e-4, h.y), wr = mix(4e-6, 1e-5, h.w);
    float ri = rr / dr;
    float sec = floor(ang * 1.2732395 + g.w * 8.0);
    float pres = step(0.45, hash1(ivec3(int(floor(ri)), int(sec), c.x ^ (c.z << 3))));
    float a = max(1.3 * px, wr);
    float prof = (1.0 - smoothstep(0.0, a, abs(fract(ri) - 0.5) * dr)) * (wr / a) * pres;
    prof = mix(prof, 0.55 * wr / dr, smoothstep(0.2, 0.45, a / dr));
    float ends = (1.0 - smoothstep(0.5 * rho, rho, r3)) * smoothstep(0.0, 0.15 * rho, rr);
    return gl * prof * ends;
}
/* pits, dust specks (0.05..0.3 mm: they scatter evenly) and dried water-spot rings (1.4..4 mm across: evenly and more
   where their tangent is square to Q); at most one a cell */
float gx_specks(vec3 p, vec3 n, vec3 Q, float px, float s, float prob, int salt) {
    vec3 d; float r3, rho; vec4 h; ivec3 c;
    if (!gx_ball(p, s, prob, salt, d, r3, rho, h, c)) return 0.0;
    vec3 d2 = d - n * dot(d, n);
    float rr = length(d2);
    float kind = fract(h.y * 7.77 + h.w * 1.37), z = fract(h.z * 13.1);
    if (kind < 0.9) {
        float rt = mix(5e-5, 3e-4, z * z);
        float a = max(1.2 * px, rt);
        return (1.0 - smoothstep(0.35 * a, a, rr)) * (rt * rt) / (a * a) * 2.0;
    }
    float R = mix(7e-4, 2e-3, fract(h.z * 5.3)), wr = mix(4e-5, 1e-4, fract(h.z * 17.9));
    float a = max(1.3 * px, wr);
    float prof = (1.0 - smoothstep(0.0, a, abs(rr - R))) * (wr / a);
    vec3 tt = cross(n, d2 / max(rr, 1e-5));
    float gq = dot(Q, tt);
    float rag = smoothstep(0.25, 0.75, vnoise(p * 2500.0 + h.xyz * 50.0));   /* a dried rim is broken, not a circle */
    return 0.35 * prof * rag * (0.3 + 0.7 * exp(-gq * gq * 150.0));
}
/* the sun on this spot of the glass (the canopy frame and the airframe cast; glass does not): five fixed taps of the
   hardware compare, ~2 texels apart (no per-pixel rotation: the same in both eyes) */
float gx_sun_vis(vec3 p, vec4 ru, vec4 rv, vec4 rd) {
    vec3 s = vec3(dot(ru.xyz, p) + ru.w, dot(rv.xyz, p) + rv.w, dot(rd.xyz, p) + rd.w);
    vec2 uv = s.xy * 0.5 + 0.5;
    if (uv.x <= 0.0 || uv.y <= 0.0 || uv.x >= 1.0 || uv.y >= 1.0 || s.z >= 1.0) return 1.0;
    vec2 r = 2.0 / vec2(textureSize(smap, 0));
    float z = s.z - 0.0005;
    float sum = 2.0 * texture(smap, vec3(uv, z)) + texture(smap, vec3(uv + vec2(r.x, 0.0), z))
              + texture(smap, vec3(uv - vec2(r.x, 0.0), z)) + texture(smap, vec3(uv + vec2(0.0, r.y), z))
              + texture(smap, vec3(uv - vec2(0.0, r.y), z));
    return sum / 6.0;
}
/* the light the glass itself adds (premultiplied: no alpha); amb / sc the glass's ambient and the game's light colours
   (main), for the interior's light level */
vec3 glass_light(vec3 p, vec3 n, float wash, int bi, vec3 amb, vec3 sc, bool nodust) {
    float px = length(fwidth(p)) * 0.7;                    /* (uniform control flow: before any per-pixel test) */
    vec4 g0 = b[bi].e[0], g1 = b[bi].e[1], g2 = b[bi].e[2];
    if (g2.z > 1.5 && g2.z < 2.5) {                        /* (ACZ_DEBUG_CANOPY_GLASS_SHOW=2: the edge distance) */
        float ed = v_ao - 1.001;
        return ed < 0.0 ? vec3(0.0, 0.0, 0.5) : vec3(1.0 - smoothstep(0.0, 0.03, ed), 1.0 - smoothstep(0.03, 0.1, ed), 0.0) * 0.6;
    }
    if (g2.z > 0.5 && g2.z < 1.5) return vec3(0.4, 0.0, 0.0) + 0.3 * abs(g0.xyz);   /* (SHOW=1: the meshes) */
    vec3 S = g0.xyz;
    float eye = cp.misc.w;
#ifdef MV
    eye = gl_ViewIndex == 0 ? -1.0 : 1.0;
#endif
    /* this eye (cockpit.vert: half the separation aside): each eye lights its own scratches, as real ones do (the halo
       then floats at the sun's distance); b[bi].p[3].x = 1: one geometry for both (the point between the eyes) */
    if (b[bi].p[3].x > 0.5) eye = 0.0;
    vec3 E = cp.head.xyz + eye * cp.head.w * cp.rot[0].xyz;
    vec3 D = normalize(p - E);
    float cs = dot(D, S), behind = -dot(S, n);             /* (n faces the eye: the sun lies beyond the glass) */
    vec3 col = vec3(0.0);
    /* the interior in the glass: the eye's ray turned back into the cockpit (n faces the eye); only where the glass is
       seen at 50 deg or more off square (faded over 4 deg): nearer square on the acrylic reflects 4-5 %, below a level
       or two of the interior, and the canopy seen through square on is most of the view (the cost) -- unless
       ACZ_VR_CANOPY_REFLECT_ALL (below) */
    /* (ACZ_VR_CANOPY_REFLECT_ALL, bug #142: b[bi].p[3].z = 1 -> at every angle, acrylic's 4 % square on; the F-16C's
       bubble is seen mostly within 50 deg of square and showed no interior at all there) */
    float cv = max(dot(n, -D), 0.0), rw = g2.x * max(1.0 - smoothstep(0.58, 0.64, cv), nodust ? 0.0 : b[bi].p[3].z);
    if (rw > 0.0 || abs(g2.z - 3.0) < 0.5 || abs(g2.z - 4.0) < 0.5) {
        vec3 R = reflect(D, n);
        float bq = dot(p, R), cq = dot(p, p) - g2.w * g2.w, disc = bq * bq - cq;
        float t = disc > 0.0 ? (cq > 0.0 ? -bq - sqrt(disc) : -bq + sqrt(disc)) : -1.0;
        vec3 dir = t > 0.0 ? p + t * R : R;                  /* (parallax: the interior as a sphere of g2.w) */
        vec4 ic = texture(interior, dir);                    /* (128^2 a face: ~0.7 deg texels, bilinear: slightly soft) */
        float c5 = 1.0 - cv;
        c5 = c5 * c5 * c5 * c5 * c5;
        /* Fresnel of acrylic, held below 0.15: at grazing angles the reflection's sphere stand-in for the interior is
           far off, and there the outside (the old uniform term, kept where no interior is seen) outshines it */
        float fr = min(0.04 + 0.96 * c5, 0.15);
        /* the interior (baked unlit) lit as the cockpit is now: the game's ambient + a fifth of its lights */
        float lv = clamp(dot(amb, vec3(0.333)) + 0.2 * dot(sc, vec3(0.333)), 0.03, 1.2);
        /* where the interior shows it takes the place of the old uniform grazing reflection (main: amb 0.35 f) */
        float fo = 1.0 - max(dot(n, normalize(cp.head.xyz - p)), 0.0);
        fo = fo * fo * fo * fo * fo;
        if (abs(g2.z - 4.0) < 0.5) return vec3(ic.rgb);                  /* (SHOW=4: the interior looked up) */
        if (abs(g2.z - 3.0) < 0.5) return vec3(fr, cv, ic.a) * 0.5;      /* (SHOW=3: Fresnel, cos, coverage) */
        col += ic.rgb * (fr * lv * rw) - amb * (0.35 * fo * ic.a * min(rw, 1.0));
    }
    /* dust on the sunlit glass (ACZ_VR_CANOPY_DUST, bug #142: the F-16C's bubble "looking out I feel there is no glass"):
       a speck of dust or a pit scatters the sunlight that falls on it in every direction, not only near the sun's
       image, so wherever the sun shines on the glass (from outside, or from inside through the canopy's other side:
       less, dust scatters forward: 0.6) the specks the wear layer already holds (gx_specks: one cell lookup) show as
       faint points at the glass's depth -- the cue that tells two eyes there is a surface there. b[bi].p[3].y =
       strength x 0.35 (x 25 = the near-sun peak's 1.4 %): at 4x the largest specks (0.3 mm) a few tens of levels on
       one or two pixels, the many small ones a few levels;
       inside the 23 deg cone the near-sun term below adds to it (no seam); hidden under water (wash) and in the
       airframe's shadow (the sun map, when it is the sun's) */
    float dk = nodust ? 0.0 : b[bi].p[3].y;
#ifdef GXV
    if (GXOFF(GXV_NODUST)) dk = 0.0;              /* (no dust or no sunlight this frame: the test below fails) */
#endif
    if (dk > 0.0 && g1.r + g1.g + g1.b > 0.0 && abs(behind) > 0.02) {
        float sd = gx_specks(p, n, S - D, px, 0.022, 0.2, 5);
        if (sd > 0.0) {
            float ld = smoothstep(0.02, 0.12, abs(behind)) * (behind > 0.0 ? 1.0 : 0.6);
            if (g2.y > 0.5) ld *= gx_sun_vis(p, b[bi].p[0], b[bi].p[1], b[bi].p[2]);
            col += g1.rgb * min(dk * ld * (1.0 - 0.85 * wash) * sd, 0.25);    /* (no firefly: a speck <= +64 levels) */
        }
    }
    /* the wear and the glare only within 23 deg of the sun's image (cos 0.92: beyond, a few faint glints -- 0.05-0.09 %
       of the pixels 15-30 deg out -- and glare under half a level were not worth the cost over the whole canopy) */
#ifdef GXV
    if (GXOFF(GXV_NOWEAR)) return col;          /* (the CPU found the sun over 23 deg from every ray of the view) */
#endif
    if (cs < 0.92 || behind <= 0.0 || g1.r + g1.g + g1.b <= 0.0) return col;
    vec3 Q = S - D;
    float th = 2.0 * asin(min(length(Q) * 0.5, 1.0));     /* angle from the sun's image (rad) */
    float lit = smoothstep(0.0, 0.1, behind);
    if (g2.y > 0.5) lit *= gx_sun_vis(p, b[bi].p[0], b[bi].p[1], b[bi].p[2]);
    if (lit <= 0.0) return col;
    float sstr = g0.w;
    if (sstr > 0.0) {
        /* forward scatter: lines ~11 deg, specks ~16 deg e-fold, faded out from 17 to 23 deg */
        float fade = 1.0 - smoothstep(0.3, 0.4, th);
        float ls = exp(-th * 5.0) * fade, lp = exp(-th * 3.5) * fade;
        float sl = 0.0, sp = 0.0;
        if (ls > 0.0) {
            sl += gx_hairs(p, n, Q, px, 0.022, 0.9, 20.0, 4e-6, 1e-5, 4.0, 0.3, 1);   /* fine hairlines */
            sl += gx_hairs(p, n, Q, px, 0.027, 0.85, 16.0, 4e-6, 1e-5, 4.0, 0.3, 7);  /* (offset cells: no gaps) */
            sl += gx_hairs(p, n, Q, px, 0.045, 0.8, 12.0, 8e-6, 2e-5, 3.0, 0.4, 2);   /* hairlines */
            sl += gx_hairs(p, n, Q, px, 0.15, 0.6, 6.0, 1e-5, 2.5e-5, 5.0, 0.6, 3);   /* long wiping strokes */
            sl += 0.6 * gx_swirl(p, n, Q, px, 0.11, 0.3, 4);                         /* polishing swirls */
            float ed = v_ao - 1.001;                       /* the distance to the glass's edge (m; ps2_video.c) */
            if (ed >= 0.0 && ed < 0.07) {                  /* crazing: fine cracks in patches near the frame */
                float zn = (1.0 - smoothstep(0.012, 0.05, ed + 0.03 * (vnoise(p * 9.0 + 5.1) - 0.5)))
                           * smoothstep(0.4, 0.65, vnoise(p * 23.0 + 1.7));
                if (zn > 0.0) sl += 2.5 * zn * gx_hairs(p, n, Q, px, 0.012, 0.85, 10.0, 3e-6, 8e-6, 0.0, 0.25, 6);
            }
        }
        if (lp > 0.0) sp = gx_specks(p, n, Q, px, 0.022, 0.2, 5);
        col += g1.rgb * (sstr * lit * (1.0 - 0.85 * wash) * (50.0 * ls * sl + 25.0 * lp * sp));
    }
    float gstr = g1.w;
    if (gstr > 0.0) {                                      /* veiling glare: the haze of the finest wear and the bulk */
        float gl = (0.05 * exp(-th / 0.035) + 0.022 * exp(-th / 0.15)) * (0.35 + 0.65 * min(sstr, 2.0))
                 * (1.0 - smoothstep(0.3, 0.4, th));
        col += g1.rgb * (gstr * lit * gl * (1.0 - 0.5 * wash));
    }
    return col;
}

/* ---- canopy cracks from hits (fx_cracks; ps2_cracks.c makes them, ps2_video.c crk_* binds them) ----
   Each impact lies in its own tangent plane on the glass (centre c, axes t1 along the glass towards the tail, t2
   across, n towards the eye): a fragment of the same glass mesh within the impact's bounding radius and close to its
   plane takes (u, v) = ((p - c).t1, (p - c).t2) -- the generator's coordinates -- so the cracks sit on the real glass,
   the same in both eyes, without UVs. Segments live in a coarse grid per impact; per fragment one cell's (at most 48)
   segments are tested. A crack is a face through the glass: it mirrors the sky where its face turns the eye's ray up,
   the dark cockpit where it turns it down (a thin silver line against the sky, dark where it mirrors the panel),
   brighter at grazing angles (total internal reflection), and glints where Q = S - D is square to it (fx_glass's
   scratch rule: near the sun's image every crack lights). Lines are drawn at least 1.3 pixels wide with their energy
   kept (true width / drawn width: steady without TAA). The crushed core is milky (at most the material's opacity),
   ringed by a halo of fine radial micro-cracks; polycarbonate holes let the world through. Water in a crack cuts its
   contrast (1 - 0.6 wash). New cracks run outward at the reveal speed (segment times t0 / t1). */
layout(std430, set = 5, binding = 0) readonly buffer CrkF { vec4 crk[]; };
layout(std430, set = 5, binding = 1) readonly buffer CrkU { uint crku[]; };
vec4 crk_over(vec4 top, vec4 under) { return vec4(top.rgb + (1.0 - top.a) * under.rgb, top.a + (1.0 - top.a) * under.a); }
vec4 crk_glass(vec3 p, int cm, vec3 amb, float wash, out float hole) {
    float px = length(fwidth(p)) * 0.7;                    /* (uniform control flow: before any per-pixel test) */
    int show = int(crk[0].w + 0.5);
    vec4 h0 = crk[0], hs = crk[1], hc = crk[2], g0 = crk[4], g1 = crk[5];
    int ns = int(h0.y + 0.5);
    /* the occupancy grid (~4 cm cells over the glass): the impacts with a crack near this cell; none: done */
    ivec3 gi = ivec3(floor((p - g0.xyz) / g0.w)), gd = ivec3(g1.xyz + 0.5);
    hole = 0.0;
    if (any(lessThan(gi, ivec3(0))) || any(greaterThanEqual(gi, gd))) return vec4(0.0);
    uint ci0 = uint(gi.x + gd.x * (gi.y + gd.y * gi.z));
    uint occw = crku[ci0 >> 1u];
    uint mask = ((ci0 & 1u) != 0u ? occw >> 16u : occw) & 0xFFFFu;
    if (mask == 0u && show == 0) return vec4(0.0);
    float now = h0.x;
    float eye = cp.misc.w;
#ifdef MV
    eye = gl_ViewIndex == 0 ? -1.0 : 1.0;
#endif
    vec3 E = cp.head.xyz + eye * cp.head.w * cp.rot[0].xyz;
    vec3 D = normalize(p - E);
    vec4 W = vec4(0.0);
    float wk = 1.0 - 0.6 * wash;                           /* water in the cracks: the index step almost gone */
    for (int i = 0; i < ns && i < 16; i++) {
        if (((mask >> uint(i)) & 1u) == 0u && show == 0) continue;
        int b = 6 + i * 8;
        vec4 s1 = crk[b + 1];
        if (int(s1.w + 0.5) != cm) continue;
        vec4 s0 = crk[b];
        vec3 q = p - s0.xyz;
        if (dot(q, q) > s0.w * s0.w) continue;
        vec4 s3 = crk[b + 3];
        if (abs(dot(q, s3.xyz)) > s3.w) continue;
        vec4 s2 = crk[b + 2], s4 = crk[b + 4], s5 = crk[b + 5], s6 = crk[b + 6], s7 = crk[b + 7];
        vec2 uv = vec2(dot(q, s1.xyz), dot(q, s2.xyz));
        float age = now - s7.x;
        if (show == 1) W = crk_over(vec4(vec3(0.5, 0.25, 0.0) * 0.15, 0.15), W);   /* (debug: the impact's bounds) */
        /* the core: a hole (polycarbonate, big hits), the crushed milky core, the halo of micro-cracks */
        float r = length(uv), rc = s6.x, rh = s6.y, rho = s6.w, grow = clamp(age / 0.03, 0.0, 1.0);
        float sunf = hs.w > 0.5 ? exp(-length(hs.xyz - D) * 3.0) : 0.0;     /* forward scatter near the sun's image */
        vec3 milk = amb * 1.3 + 0.08 + hc.rgb * (0.6 * sunf);
        if (rho > 0.0) {
            float rag = rho * (0.85 + 0.3 * vnoise(vec3(uv * 1400.0, float(i) * 3.7)));
            if (r < rag * grow) { hole = 1.0; return vec4(0.0); }
            float rim = (1.0 - smoothstep(rag * 1.15, rag * 1.45 + px, r)) * grow;
            W = crk_over(vec4(milk * (0.6 * rim), 0.6 * rim), W);
        }
        if (rh > 0.0 && r < rh) {
            float aa = max(px, 1e-4);
            float core = (1.0 - smoothstep(rc * 0.55, rc + aa, r)) * grow;
            float gr = vnoise(vec3(uv * 900.0, float(i) * 7.1));             /* crushed grains, ~1 mm */
            gr = mix(0.5, gr, clamp(0.0011 / max(px, 1e-6) - 0.5, 0.0, 1.0));
            /* the halo: fine radial cracks (40 around), fading out; to their mean where they get closer than ~3 px */
            float ang = atan(uv.y, uv.x);
            float streak = vnoise(vec3(ang * 6.366, r * 160.0, float(i) * 1.3));
            float sp = r * 0.157 / max(px, 1e-6);
            streak = mix(0.45, smoothstep(0.55, 0.85, streak), clamp(sp / 3.0 - 0.3, 0.0, 1.0));
            float halo = rh > rc ? (1.0 - smoothstep(rc, rh, r)) : 0.0;
            halo = halo * halo * streak * grow;
            float a = min(s6.z * core * (0.7 + 0.45 * gr) + 0.4 * halo * (1.0 - core), 0.75) * (0.75 + 0.25 * wk);
            W = crk_over(vec4(milk * a, a), W);
        }
        /* the segments of this impact's grid cell */
        int gn = int(s4.w + 0.5);
        ivec2 ci = ivec2(floor((uv - s4.xy) / s4.z));
        if (any(lessThan(ci, ivec2(0))) || any(greaterThanEqual(ci, ivec2(gn)))) continue;
        uint cell = crku[uint(s5.z + 0.5) + uint(ci.y * gn + ci.x)];
        uint off = cell >> 8u, cnt = cell & 255u;
        if (show == 2) W = crk_over(vec4(vec3(0.0, 0.3, 0.6) * 0.12 * float((ci.x + ci.y) & 1), 0.12 * float((ci.x + ci.y) & 1)), W);
        if (show == 3) { float h = float(cnt) / 48.0; W = crk_over(vec4(vec3(h, 1.0 - h, 0.0) * 0.3, 0.3), W); }
        uint sb = uint(s5.x + 0.5);
        float dmin = 1e9, wb = 0.0, flb = 0.0;
        uint sbest = 0u;
        vec2 tb = vec2(1.0, 0.0);
        for (uint k = 0u; k < cnt; k++) {
            uint si = crku[off + k];
            vec4 A = crk[sb + 2u * si], B = crk[sb + 2u * si + 1u];
            float hv = clamp((now - B.x) / max(B.y - B.x, 1e-4), 0.0, 1.0);   /* how far the crack has run along it */
            if (hv <= 0.0) continue;
            vec2 a = A.xy, ab = (A.zw - A.xy) * hv;
            float l2 = dot(ab, ab);
            float t = l2 > 0.0 ? clamp(dot(uv - a, ab) / l2, 0.0, 1.0) : 0.0;
            float d = length(uv - a - t * ab);
            if (d < dmin) { dmin = d; wb = B.z; flb = B.w; sbest = si; tb = l2 > 0.0 ? ab * inversesqrt(l2) : vec2(1.0, 0.0); }
        }
        if (dmin > 0.01) continue;
        int fl = int(flb + 0.5);
        float tilt = float(fl >> 3) * (0.7 / 15.0);
        vec3 t3 = normalize(s1.xyz * tb.x + s2.xyz * tb.y);
        vec3 bn = cross(s3.xyz, t3);
        /* the line: the crack's edge (true width wb), at least 1.3 px wide with its energy kept */
        float wd = max(wb, 1.3 * px), hh = 0.5 * wd;
        float cov = (wb / wd) * (1.0 - smoothstep(hh - 0.5 * px, hh + 0.5 * px, dmin));
        /* the face through the glass's thickness, seen at an angle: a faint band thick x tan(refracted angle) wide
           across the crack (the depth of a crack inside the glass; s7.z = the glass's thickness) */
        float cn = abs(dot(D, s3.xyz)), sn = sqrt(max(1.0 - cn * cn, 0.0)) / 1.49;
        float hb = min(0.5 * s7.z * sn * inversesqrt(max(1.0 - sn * sn, 1e-4)) * abs(dot(D, bn)), 0.0028);  /* (the grid's margin) */
        float band = hb > hh ? 0.16 * (1.0 - smoothstep(hb - 0.5 * px, hb + 0.5 * px, dmin)) * min(1.0, hb / (2.0 * px)) : 0.0;
        if (cov <= 0.0 && band <= 0.0) continue;
        vec3 nfc = normalize(bn * cos(tilt) + s3.xyz * sin(tilt));      /* the crack's face */
        vec3 R = reflect(D, nfc);
        /* it mirrors the sky (silver-white) where it turns the ray up, the cockpit (dark) where it turns it down */
        vec3 env = mix(amb * 0.3 + 0.02, amb * 1.6 + 0.12, smoothstep(-0.45, 0.25, R.y));
        float ci_ = abs(dot(D, nfc));
        float F = 0.3 + 0.7 * (1.0 - ci_) * (1.0 - ci_) * (1.0 - ci_);   /* towards total internal reflection */
        vec3 col = env * (0.7 + 0.5 * F);
        if (hs.w > 0.5) {                                  /* the sun along the crack */
            vec3 Q = hs.xyz - D;
            float gq = dot(Q, t3);
            col += hc.rgb * (exp(-gq * gq * (1.0 / 0.05)) * (0.35 + 0.65 * F) * 1.3);
        }
        float hk = ((fl >> 2) & 1) != 0 ? 0.5 : 1.0;      /* (in the gunsight cone: half contrast) */
        /* a crack opens unevenly: each segment its own brightness (from its index: the same in both eyes) */
        float op = 0.7 + 0.55 * float(pcg3(uvec3(sbest, uint(i), 7u)).x & 1023u) * (1.0 / 1023.0);
        col *= op;
        float a = max(cov * (0.7 + 0.25 * F) * min(op, 1.0), band) * hk * wk;
        W = crk_over(vec4(col * a, a), W);
    }
    return W;
}

/* ---- rear-view mirrors (qw_mirror; ps2_video.c mir_*: ACZ_VR_COCKPIT_MIRRORS) ----
   The mirror glass of the cockpits that model mirrors (detected at load; push light.w = 2, + 1: the mesh holds its
   frame too and is glass only where its texture is bright, + 2: without our clouds, an A/B re-draw) reflects what lies behind: each eye's own ray reflected
   about the glass, so the reflection lies at infinity and is stereo-correct (as a real mirror's distant view) and moves
   with the head. Sampled from the game's sky (the block's zenith / horizon / below-horizon colours by world elevation,
   the sun of fx_glass's inputs e[0] / e[1]), our far clouds' direction map over it (set 6, as the far composite:
   in-scatter + sky x transmittance), and the model behind the pilot (set 7: a cube from (0, 0, xr_k) in the cockpit
   frame, among the mirrors, with the distance / 4 m in alpha; the ray is marched against it, so the headrest, the
   canopy frame and the tail keep their parallax), lit by the cockpit's light now. In a cloud the colours come pre-mixed with the
   cloud's and the clouds and the sun fade. Silvered glass: ~0.82 reflectance plus the front face's Fresnel at grazing
   angles, a faint cool tint, a faint diffuse film. Inputs (ps2_video.c mir_frame): m[1].yzw zenith, fog0 / fog1 /
   zscale horizon, zmax / xoff / yoff below the horizon, m[2].xyz / m[3].xyz the world's x / up axes in the cockpit
   frame, m[2].w the in-cloud share, m[3].w the far map's uv scale (0: none), spec_alpha the strength; push light.y the
   cube's strength (0: none), uvoff.z the debug view (ACZ_DEBUG_COCKPIT_MIRRORS_SHOW). */
layout(set = 6, binding = 0) uniform sampler2D mir_clouds;
layout(set = 7, binding = 0) uniform samplerCube mir_cube;
/* the mirrors' live map (mir_live 2026-10-29; ps2_video.c mlv_*, ACZ_VR_COCKPIT_MIRRORS_LIVE; push light.w + 4): the
   other aircraft the game draws, drawn once per game frame by an aircraft-fixed rear camera into a small map (x = cx +
   fx X / D, y = cy - fy Y / D map pixels, its X the cockpit's -x, D the cockpit's z), looked up by the reflected ray as
   the sky is (at infinity): rgb premultiplied by the coverage (the map is cleared to 0), the coverage from its depth
   (0 = nothing) with the same bilinear weights. The reserved block 5 below the light block: p[0].x on, p[1] cx cy fx
   fy, p[2] 1 / the target's size and the map's own width and height (map pixels). */
layout(set = 8, binding = 0) uniform sampler2D mir_live;
layout(set = 9, binding = 0) uniform sampler2D mir_live_z;
layout(set = 10, binding = 0) uniform sampler2D mir_clouds_z;   /* our far clouds' depth (m; 65504 none) */
/* the mirrors' real view (mir_sharp 2026-10-29, bug #230; ps2_video.c mirv_*, ACZ_VR_COCKPIT_MIRRORS_REAL; push light.w
   + 32, + 64 x the mirror's slot j): what stands behind the pilot drawn from each eye's mirror image into an atlas (a
   layer an eye), lit as the cockpit, 4x multisampled, alpha = coverage. A glass point reads it by where the eye's ray
   meets the mirror's ideal plane (c, n; the aim turns the shading normal, not the glass), in the plane's axes x, y = n
   x x: texel = (k (u - u0), k (v1 - v)) + the slot's origin. In the live map's block (bi - 5): m[j] = c, k; c[j] = n,
   u0; r[j] = x, v1; slot j's origin and size l[j] (j < 3) / e[0]; e[1].xy 1 / the atlas's size */
layout(set = 11, binding = 0) uniform sampler2DArray mir_view;
vec4 mirv_look(int bi, int j, vec3 E, vec3 D, float layer, out vec2 q) {
    vec4 A = b[bi - 5].m[j], B = b[bi - 5].c[j], X = b[bi - 5].r[j];
    vec4 S = j < 3 ? b[bi - 5].l[j] : b[bi - 5].e[0];
    vec2 inv = b[bi - 5].e[1].xy;
    float dn = dot(D, B.xyz);
    float t = dn < -1e-4 ? dot(A.xyz - E, B.xyz) / dn : 0.0;
    vec3 P = E + D * t - A.xyz;
    vec3 Y = cross(B.xyz, X.xyz);
    vec2 uv = vec2((dot(X.xyz, P) - B.w) * A.w, (X.w - dot(Y, P)) * A.w);
    q = uv / max(S.zw, vec2(1.0));                   /* (ACZ_DEBUG_COCKPIT_MIRRORS_SHOW=9: the slot's point) */
    uv = clamp(uv, vec2(0.5), S.zw - 0.5) + S.xy;
    return textureLod(mir_view, vec3(uv * inv, layer), 0.0);
}
vec4 mlv_look(int bi, vec3 r, out float dm) {
    vec4 c = b[bi - 5].p[1], s = b[bi - 5].p[2];
    dm = 1e30;
    if (r.z < 0.02) return vec4(0.0);
    vec2 px = vec2(c.x - c.z * r.x / r.z, c.y - c.w * r.y / r.z) + 0.5;   /* (the GS's half pixel) */
    if (px.x < 0.5 || px.y < 0.5 || px.x > s.z - 0.5 || px.y > s.w - 0.5) return vec4(0.0);
    vec2 uv = px * s.xy;
    vec4 z = textureGather(mir_live_z, uv, 0);
    vec2 f = fract(px - 0.5);
    float cov = dot(vec4((1.0 - f.x) * f.y, f.x * f.y, f.x * (1.0 - f.y), (1.0 - f.x) * (1.0 - f.y)), step(vec4(1e-12), z));
    /* (phase 4) the nearest content's distance along the ray, m: the depth z = zA' + zB' / D (p[3].xy, D in game units
       along the map's axis), p[3].z the metres a unit */
    vec4 q = b[bi - 5].p[3];
    float zn = max(max(z.x, z.y), max(z.z, z.w));
    if (zn > 1e-12 && zn - q.x > 1e-12) dm = q.y / (zn - q.x) * q.z / r.z;
    if (b[bi - 5].p[0].y < 0.5) return vec4(textureLod(mir_live, uv, 0.0).rgb, cov);
    /* (phase 4, ACZ_VR_COCKPIT_MIRRORS_LIVE_SHARP) the colour through a Catmull-Rom filter (5 bilinear taps): at 4x the
       glass shows ~2.5 eye pixels a map texel, and the bilinear read of it was a blur rather than glass. The taps stay
       inside the map (its edge texels repeat); the result is kept >= 0 */
    vec2 ts = 1.0 / s.xy, t1 = floor(px - 0.5) + 0.5, f2 = px - t1;
    vec2 w0 = f2 * (-0.5 + f2 * (1.0 - 0.5 * f2)), w1 = 1.0 + f2 * f2 * (-2.5 + 1.5 * f2);
    vec2 w2 = f2 * (0.5 + f2 * (2.0 - 1.5 * f2)), w3 = f2 * f2 * (-0.5 + 0.5 * f2);
    vec2 w12 = w1 + w2, lo = vec2(0.5), hi = s.zw - 0.5;
    vec2 p0 = clamp(t1 - 1.0, lo, hi) * s.xy, p3 = clamp(t1 + 2.0, lo, hi) * s.xy;
    vec2 p12 = clamp(t1 + w2 / w12, lo, hi) * s.xy;
    vec3 col = textureLod(mir_live, vec2(p12.x, p0.y), 0.0).rgb * (w12.x * w0.y)
             + textureLod(mir_live, vec2(p0.x, p12.y), 0.0).rgb * (w0.x * w12.y)
             + textureLod(mir_live, p12, 0.0).rgb * (w12.x * w12.y)
             + textureLod(mir_live, vec2(p3.x, p12.y), 0.0).rgb * (w3.x * w12.y)
             + textureLod(mir_live, vec2(p12.x, p3.y), 0.0).rgb * (w12.x * w3.y);
    float ws = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return vec4(max(col / ws, vec3(0.0)), cov);
}
/* our far clouds' depth at uv (m), as the world's composite reads it (cloud_comp.frag far_depth): bilinear over the
   texels that hold cloud only; 65504 none */
float mir_cloud_depth(vec2 uv) {
    vec2 sz = vec2(textureSize(mir_clouds_z, 0));
    vec2 t = uv * sz - 0.5, f = fract(t);
    vec4 g = textureGather(mir_clouds_z, (floor(t) + 1.0) / sz, 0);
    vec4 w = vec4((1.0 - f.x) * f.y, f.x * f.y, f.x * (1.0 - f.y), (1.0 - f.x) * (1.0 - f.y));
    vec4 v = vec4(lessThan(g, vec4(32752.0)));
    float ws = dot(w, v);
    return ws > 1.0e-6 ? dot(w * v, g) / ws : 65504.0;
}
vec2 mir_oct_uv(vec3 d) {                        /* cloud_common.glsl cl_dir_uv: full-sphere octahedral, y up */
    d /= abs(d.x) + abs(d.y) + abs(d.z);
    vec2 p = d.xz;
    if (d.y < 0.0) p = (1.0 - abs(p.yx)) * vec2(p.x >= 0.0 ? 1.0 : -1.0, p.y >= 0.0 ? 1.0 : -1.0);
    return p * 0.5 + 0.5;
}
/* the sky towards w (world, unit; r the same in the cockpit frame); px = a pixel's angle in the reflection */
vec3 mir_sky(int bi, vec3 r, vec3 w, float px, bool haze) {
    vec3 zen = b[bi].m[1].yzw, hor = vec3(b[bi].fog0, b[bi].fog1, b[bi].zscale);
    vec3 gnd = vec3(b[bi].zmax, b[bi].xoff, b[bi].yoff);
    float s = w.y;
    /* above the horizon the dome's gradient: fitted to the game's own sky in M2 (the zenith colour from ~50 deg up;
       rms 0.023 over 2.8-45 deg); below it the haze's colour darkening to the ground's within ~10 deg (0.7x); the
       horizon about a pixel soft */
    float t = 1.0 - min(max(s, 0.0) * 1.25, 1.0);
    vec3 up = mix(zen, hor, t * sqrt(t));
    vec3 dn = gnd * (1.0 - 0.3 * smoothstep(0.0, 0.2, -s));
    /* (mir_live 2026-10-29, ACZ_VR_COCKPIT_MIRRORS_HAZE; haze = false: as before) the distant ground fades into the
       horizon's haze as the world's does (the game's far terrain takes the fog), over ~4 deg below the horizon: with the
       aimed mirrors (ACZ_VR_COCKPIT_MIRRORS_AIM) the horizon crosses the side mirrors, and the step from the horizon sky
       to the flat fog colour read as a hard line (the user 2026-10-28 23:15: "the ground is very strange") */
    if (haze) dn = mix(dn, hor, exp(-max(-s, 0.0) * 14.0));
    vec3 c = mix(dn, up, smoothstep(-px, px, s));
    /* the sun: a disc of ~0.46 deg and its glow */
    vec3 S = b[bi].e[0].xyz, sc = b[bi].e[1].rgb;
    float cs = dot(r, S);
    if (cs > 0.95 && sc.r + sc.g + sc.b > 0.0) {
        float th = sqrt(max(2.0 * (1.0 - cs), 0.0));
        float disc = 1.0 - smoothstep(0.008 - px, 0.008 + px, th);
        c += sc * (disc * 1.4 + 0.3 * exp(-th / 0.025) + 0.06 * exp(-th / 0.12));
    }
    /* our far clouds over the sky, fading in a cloud */
    float uvs = b[bi].m[3].w;
    if (uvs > 0.0 && ((int(cp.light.w + 0.5) - 2) & 2) == 0) {  /* (push bit: the A/B re-draw without them) */
        vec4 cl = textureLod(mir_clouds, mir_oct_uv(w) * uvs, 0.0);
        float kc = b[bi].m[2].w;
        c = cl.rgb * (1.0 - kc) + c * mix(cl.a, 1.0, kc);
    }
    return c;
}
/* the other aircraft (ck_f15 2026-10-29; ps2_video.c mac_*, ACZ_VR_COCKPIT_MIRRORS_AIRCRAFT): up to 8 in the reserved
   block 4 below the light block -- vec4 slot 3i its position (cockpit frame, m; w the distance), 3i + 1 its body's x
   axis (right; w its length, m), 3i + 2 its z axis (back); fog0 the count, fog1 the haze's visibility (m), zscale the
   strength. A fighter's silhouette of four ellipsoids along the reflected ray from the mirror point p: dark grey lit
   by the sky, into the sky with the distance; each edge by the pixel's footprint (fwidth of the ray's scaled miss
   distance: computed for every one, so the control flow stays uniform), a far one a faint speck. */
vec4 mac_v(int bi, int k) {
    if (k < 4) return b[bi].m[k];
    if (k < 8) return b[bi].c[k - 4];
    if (k < 11) return b[bi].l[k - 8];
    if (k < 15) return b[bi].r[k - 11];
    if (k < 18) return b[bi].e[k - 15];
    if (k < 22) return b[bi].p[k - 18];
    return b[bi].mp[k - 22];
}
float mac_q(vec3 o, vec3 d, vec3 c, vec3 s) {         /* the ray's nearest scaled distance from an ellipsoid's centre */
    vec3 oo = (o - c) / s, dd = d / s;
    float t = max(-dot(oo, dd) / max(dot(dd, dd), 1e-12), 0.0);
    return length(oo + dd * t);
}
float mac_cov(float q) {
    float w = max(fwidth(q), 1e-4);
    return clamp((1.0 - q) / w + 0.5, 0.0, 1.0) * min(1.0, 1.0 / w);
}
vec3 mac_over(int bi, vec3 p, vec3 r, vec3 sky, vec3 hor, vec3 zen, float kc) {
    int ai = bi - 4, n = min(int(b[ai].fog0 + 0.5), 8);
    float vis = max(b[ai].fog1, 1.0), k = b[ai].zscale;
    vec3 body = mix(hor, zen, 0.4) * 0.32;
    for (int i = 0; i < n; i++) {
        vec4 P = mac_v(ai, 3 * i), X = mac_v(ai, 3 * i + 1), Z = mac_v(ai, 3 * i + 2);
        vec3 Y = cross(Z.xyz, X.xyz);
        vec3 rel = p - P.xyz;                             /* the ray's origin from the aircraft */
        vec3 o = vec3(dot(rel, X.xyz), dot(rel, Y), dot(rel, Z.xyz)), d = vec3(dot(r, X.xyz), dot(r, Y), dot(r, Z.xyz));
        float sc = X.w / 19.0, ahead = step(0.0, dot(P.xyz - p, r));
        float cov = mac_cov(mac_q(o, d, vec3(0.0), vec3(1.25, 1.0, 9.5) * sc));            /* fuselage */
        cov = max(cov, mac_cov(mac_q(o, d, vec3(0.0, 0.0, 1.2) * sc, vec3(6.5, 0.25, 3.0) * sc)));   /* wings */
        cov = max(cov, mac_cov(mac_q(o, d, vec3(0.0, 0.1, 7.6) * sc, vec3(4.0, 0.2, 1.4) * sc)));    /* tailplane */
        cov = max(cov, mac_cov(mac_q(o, d, vec3(0.0, 1.9, 7.2) * sc, vec3(0.15, 1.9, 1.5) * sc)));   /* fin */
        float f = exp(-P.w / vis) * k * (1.0 - kc);               /* (not in a cloud) */
        sky = mix(sky, body, cov * ahead * f);
    }
    return sky;
}
/* the model behind the pilot along the ray p + t r (cockpit frame, m; relative to the cube's centre C = (0, 0, xr_k),
   ps2_video.c MIR_CUBE_Z): the cube's surface distance d (alpha x 4 m; alpha 0: nothing that way) against the point's
   own distance from C, in 9 steps from 8 cm (clear of the mirror's own surroundings, which C sees around the glass)
   out to 3.2 m and one secant step. A step
   crosses a surface only if the point is 1 cm beyond it (the bilinear blend of a silhouette with "nothing" reads
   nearer) and the surface lies no nearer to C than the step's segment comes (-3 cm): a segment cannot meet a surface
   closer than all of its points -- that is the step jumping to a direction where a nearer part (the headrest) stands
   in front of a ray that passed beside it. rgb = albedo x (0.5 + 0.5 AO), unlit; a = 1 where the ray meets it */
/* (mir_live 2026-10-29, ACZ_VR_COCKPIT_MIRRORS_INTERIOR_EDGE; edge = false: as before) A hit counts only where the cube's
   distance at the found point agrees with the point's own (within 20 %): beside a part's silhouette the bilinear blend
   with "nothing" (distance 0, black) reads near and dark, and the ray -- a near miss -- marched on as dark specks in the
   mirror (the aimed F-15C top mirror grazes the headrest: M8 a dotted line of them) */
/* (mir_live 2026-10-29, bug #205, ACZ_VR_COCKPIT_MIRRORS_INTERIOR_NEAR; nearcut = false: as before) A hit counts only once
   the ray has been in front of the cube's surfaces: a first step (8 cm) already behind one lies in the mirror's own
   surroundings -- the canopy arch the glass sits against, seen from C in front of it (the Tornado's side mirrors: the
   arch at 4 cm under a third of the glass, a dark wedge) -- which the march's 8 cm start meant to leave out */
/* (mir_live 2026-10-29, bug #205, ACZ_VR_COCKPIT_MIRRORS_INTERIOR_CRISP; crisp = false: as before) A step counts only
   where all four of the cube's nearest texels (textureGather of the distance) hold a surface, and the hit takes the
   secant's point only where they do there too (else the step's own): at a part's silhouette against nothing the
   bilinear blend of its distance with 0 (and of its colour with black) gave a nearer, darker surface a step could stop
   at -- dark specks scattered over the open sky of the glass (the M2000D's, the F-4E's). (Asking the four to agree
   within 10 % as well left light dotted lines where a near part crosses a far one: the J35J's left mirror) */
bool mir_crisp_at(vec3 x) {
    vec4 g = textureGather(mir_cube, x, 3);
    return min(min(g.x, g.y), min(g.z, g.w)) > 0.5 / 255.0;
}
vec4 mir_interior(vec3 p, vec3 r, bool edge, bool nearcut, bool crisp) {
    const float T[9] = float[](0.08, 0.15, 0.25, 0.38, 0.56, 0.85, 1.3, 2.0, 3.2);
    float tp = 0.0, fp = -1.0;
    bool fore = !nearcut;                        /* (the ray has been in front of the surfaces) */
    for (int i = 0; i < 9; i++) {
        vec3 x = p + r * T[i], xp = p + r * tp;
        float a = textureLod(mir_cube, x, 0.0).a, d = a * 4.0;
        float f = a > 0.5 / 255.0 ? length(x) - d - 0.01 : -1.0;
        float seg = length(xp + r * clamp(-dot(xp, r), 0.0, T[i] - tp));   /* the segment's closest approach */
        if (f < 0.0) fore = true;
        if (f >= 0.0 && d >= seg - 0.03 && fore && (!crisp || mir_crisp_at(x))) {
            float t = tp + (T[i] - tp) * clamp(-fp / max(f - fp, 1e-4), 0.0, 1.0);
            vec3 xh = p + r * t;                                  /* (p is relative to C here) */
            if (crisp && !mir_crisp_at(xh)) xh = x;
            vec4 h = textureLod(mir_cube, xh, 0.0);
            if (!edge || h.a * 4.0 >= 0.8 * length(xh)) return vec4(h.rgb, 1.0);
        }
        tp = T[i]; fp = min(f, 0.0);
    }
    return vec4(0.0);
}
/* TONE (perf6_mirrors 2026-11-04; ps2_video.c mir_mode, ACZ_VR_COCKPIT_MIRRORS=tone, the default; push light.w + 256,
   light.y the softness in radians, + 2 without our clouds): the glass carries the scene's tonality and nothing more --
   no live map, no parts behind the pilot. The game's sky by the reflected direction as mir_sky has it, but the horizon
   spread over +-soft (no line to swim with the head) and below it C1's haze giving way to the ground's own tone as the
   eyes see it ahead (the scene light's ground band: green fields, the sea, sand, snow);
   the sun behind as a soft glow (no disc: a lobe of 0.7 soft and a wide halo, x its colour, day and cloud shadow); our
   far clouds' tone over it (5 taps over +-soft/2: their shapes melt into light and shade); in a cloud the colours come
   pre-mixed with the cloud's grey (mir_frame), in a tunnel its gloom (as mirror_light). Smooth functions of each eye's
   own reflected ray only: the tone sits at infinity in stereo, slides across the glass as the head moves, and has
   nothing to alias or shimmer. CPIT_MIR (ps2_video.c, cpit_load_one's ck_si): the code a pipeline holds -- 1 the full
   mirrors without this (LIVE, SKY, OFF: as before), 2 this alone (the default), 0 both by the push */
layout(constant_id = 2) const int CPIT_MIR = 0;
vec3 mir_tone(int bi, vec3 r, vec3 w, float soft, bool clouds) {
    vec3 zen = b[bi].m[1].yzw, hor = vec3(b[bi].fog0, b[bi].fog1, b[bi].zscale);
    vec3 gnd = vec3(b[bi].zmax, b[bi].xoff, b[bi].yoff);
    float sw = max(soft, 0.02), s = w.y, kc = b[bi].m[2].w;
    float t = 1.0 - min(max(s, 0.0) * 1.25, 1.0);
    vec3 up = mix(zen, hor, t * sqrt(t));
    /* below: the horizon's haze, C1's fog colour within ~3 deg, then the ground's own tone (the scene light's ground
       band, ps2_video.c sl_ground, in the live map's block's spare fields; its weight 0: C1's fog darkened) by ~17 deg;
       in a cloud the cloud's grey (gnd comes pre-mixed with it) */
    vec4 G = vec4(b[bi - 5].zmax, b[bi - 5].xoff, b[bi - 5].yoff, b[bi - 5].spec_alpha);
    vec3 deep = mix(mix(gnd * 0.8, G.rgb, 0.85 * clamp(G.w, 0.0, 1.0)), gnd, kc);
    float d = max(-s, 0.0);
    vec3 dn = mix(mix(hor, gnd, smoothstep(0.0, 0.06, d)), deep, smoothstep(0.03, 0.3, d));
    vec3 c = mix(dn, up, smoothstep(-sw, sw, s));
    vec3 S = b[bi].e[0].xyz, sc = b[bi].e[1].rgb;
    float th = sqrt(max(2.0 * (1.0 - dot(r, S)), 0.0)), g = 0.7 * sw;
    c += sc * (0.45 * exp(-0.5 * th * th / (g * g)) + 0.08 * exp(-th / 0.3));
    float uvs = b[bi].m[3].w;
    if (clouds && uvs > 0.0) {
        vec3 a = normalize(cross(w, abs(w.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0))), q = cross(w, a);
        float o = 0.5 * sw;
        vec4 cl = textureLod(mir_clouds, mir_oct_uv(w) * uvs, 0.0)
                + textureLod(mir_clouds, mir_oct_uv(normalize(w + o * a)) * uvs, 0.0)
                + textureLod(mir_clouds, mir_oct_uv(normalize(w - o * a)) * uvs, 0.0)
                + textureLod(mir_clouds, mir_oct_uv(normalize(w + o * q)) * uvs, 0.0)
                + textureLod(mir_clouds, mir_oct_uv(normalize(w - o * q)) * uvs, 0.0);
        cl *= 0.2;
        c = cl.rgb * (1.0 - kc) + c * mix(cl.a, 1.0, kc);
    }
    if (bi >= 3) {                                               /* (tunnel_light, bug #267: as mirror_light) */
        float tcl = clamp(b[bi - 3].m[3].w, 0.0, 1.0);
        if (tcl > 0.0) {
            vec3 tc = b[bi - 3].m[2].rgb;
            if (dot(tc, vec3(1.0)) <= 0.0) tc = c * 0.15;
            c = mix(c, tc, tcl);
        }
    }
    return c;
}
/* the mirror glass's colour: p, n (cockpit frame), the texel colour c, the part's own shading lit, the cockpit's
   ambient, its lights' colour sum sc, the in-cloud share kc */
vec3 mirror_light(vec3 p, vec3 n, int bi, vec4 c, vec3 lit, vec3 amb, vec3 sc, float kc) {
    float eye = cp.misc.w;
#ifdef MV
    eye = gl_ViewIndex == 0 ? -1.0 : 1.0;
#endif
    vec3 E = cp.head.xyz + eye * cp.head.w * cp.rot[0].xyz;      /* this eye (cockpit.vert) */
    vec3 D = normalize(p - E);
    vec3 nf = dot(n, D) > 0.0 ? -n : n;                          /* the side facing this eye */
    vec3 r = reflect(D, nf);
    vec3 X = b[bi].m[2].xyz, Y = b[bi].m[3].xyz;
    vec3 w = vec3(dot(X, r), dot(Y, r), dot(cross(X, Y), r));    /* the reflected ray in the world (y up) */
    float px = max(length(fwidth(w)), 2e-4);                     /* (uniform control flow: the push says mirror) */
    int show = int(cp.uvoff.z + 0.5);
    bool edge = (show & 16) == 0;                                /* (ACZ_VR_COCKPIT_MIRRORS_INTERIOR_EDGE) */
    bool haze = (show & 32) == 0;                                /* (ACZ_VR_COCKPIT_MIRRORS_HAZE) */
    bool nearcut = (show & 64) == 0;                             /* (ACZ_VR_COCKPIT_MIRRORS_INTERIOR_NEAR) */
    bool crisp = (show & 128) == 0;                              /* (ACZ_VR_COCKPIT_MIRRORS_INTERIOR_CRISP) */
    show &= 15;
    float wgt = 1.0;
    if (((int(cp.light.w + 0.5) - 2) & 1) != 0)                  /* glass where the texture is bright */
        wgt = smoothstep(0.10, 0.22, dot(c.rgb, vec3(0.2126, 0.7152, 0.0722)));
    if (show == 1) return mix(lit, vec3(0.6, 0.05, 0.05), wgt);
    if (CPIT_MIR != 1 && (CPIT_MIR == 2 || ((int(cp.light.w + 0.5) - 2) & 256) != 0)) {   /* TONE (perf6_mirrors: mir_tone) */
        vec3 env = mir_tone(bi, r, w, cp.light.y, ((int(cp.light.w + 0.5) - 2) & 2) == 0);
        float cv = max(dot(nf, -D), 0.0), f5 = 1.0 - cv;
        f5 = f5 * f5 * f5 * f5 * f5;
        return mix(lit, env * ((0.82 + 0.14 * f5) * b[bi].spec_alpha) * vec3(0.95, 0.98, 1.0) + lit * 0.035, wgt);
    }
    if (show == 6) return mix(lit, mac_over(bi, p, r, vec3(1.0), vec3(1.0), vec3(1.0), 0.0), wgt);   /* the aircraft */
    if (show == 2) return mix(lit, w * 0.5 + 0.5, wgt);
    vec3 sky = show == 5 ? vec3(0.0) : mir_sky(bi, r, w, px, haze);
    if (b[bi - 4].fog0 > 0.5)                                    /* the other aircraft (mac_over; uniform: the block) */
        sky = mac_over(bi, p, r, sky, vec3(b[bi].fog0, b[bi].fog1, b[bi].zscale), b[bi].m[1].yzw, kc);
    if (((int(cp.light.w + 0.5) - 2) & 4) != 0) {                /* the live map (mlv_look; uniform: the push) */
        float dm;
        /* (mir_sharp, ACZ_VR_COCKPIT_MIRRORS_LIVE_TURN) the map's own frame: the world direction w turned into the
           cockpit frame of the map's game frame (its world x / up axes: e[2].xyz, fog0 / fog1 / zscale), so between game
           frames its terrain and aircraft turn with the extrapolated sky instead of standing still for a frame */
        vec3 rm = r;
        vec4 T0 = b[bi - 5].e[2];
        if (T0.w > 0.5) {
            vec3 Y0 = vec3(b[bi - 5].fog0, b[bi - 5].fog1, b[bi - 5].zscale);
            rm = w.x * T0.xyz + w.y * Y0 + w.z * cross(T0.xyz, Y0);
        }
        vec4 lv = mlv_look(bi, rm, dm);
        /* (phase 4, ACZ_VR_COCKPIT_MIRRORS_LIVE_CLOUDS) our far clouds in front of what the map shows -- the cloud sea
           below over the terrain, as the world's far composite puts them (cloud_comp.frag: k by the far map's depth
           against the scene's): before, the terrain covered the clouds below the horizon */
        float uvs2 = b[bi].m[3].w;
        if (b[bi - 5].p[3].w > 0.5 && uvs2 > 0.0 && ((int(cp.light.w + 0.5) - 2) & 2) == 0 && lv.a > 0.0) {
            vec2 cuv = mir_oct_uv(w) * uvs2;
            vec4 cl = textureLod(mir_clouds, cuv, 0.0);
            float cd = mir_cloud_depth(cuv);
            float k = cd < 32752.0 && dm < 1e29 ? smoothstep(0.9 * cd, 1.1 * cd, dm) : 0.0;
            lv.rgb = lv.rgb * mix(1.0, cl.a, k) + lv.a * cl.rgb * k;
        }
        if (show == 7) return mix(lit, vec3(1.0 - lv.a) + lv.rgb, wgt);
        if (show == 8) {                                         /* (debug: map pixel / its size, coverage) */
            vec4 c = b[bi - 5].p[1], s = b[bi - 5].p[2];
            vec2 q = r.z > 0.02 ? vec2(c.x - c.z * r.x / r.z, c.y - c.w * r.y / r.z) / s.zw : vec2(-1.0);
            return mix(lit, vec3(clamp(q, 0.0, 1.0), lv.a), wgt);
        }
        sky = sky * (1.0 - lv.a * (1.0 - kc)) + lv.rgb * (1.0 - kc);   /* (they fade in a cloud, as the sky does) */
    }
    /* (tunnel_light, bug #267) under a roof the open sky, the far terrain and the clouds behind give way to the
       enclosure's own gloom: the scene light's side colour (the world around as the eyes see it: the tunnel's walls),
       the scene block's m[3].w = how closed (ps2_video.c sl_frame); the cockpit's own parts stay over it */
    if (bi >= 3) {
        float tcl = clamp(b[bi - 3].m[3].w, 0.0, 1.0);
        if (tcl > 0.0) {
            vec3 tc = b[bi - 3].m[2].rgb;
            if (dot(tc, vec3(1.0)) <= 0.0) tc = sky * 0.15;        /* (no scene light: a dark stand-in) */
            sky = mix(sky, tc, tcl);
        }
    }
    float ik = cp.light.y;                                       /* the cube's strength (0: none) */
    int lw = int(cp.light.w + 0.5) - 2;
    vec3 env;
    if ((lw & 32) != 0) {                /* (mir_sharp: the real view, mirv_look -- lit, premultiplied by its coverage) */
        vec2 sq = vec2(0.0);
        vec4 rv = ik > 0.0 && show != 4 ? mirv_look(bi, (lw >> 6) & 3, E, D, eye > 0.0 ? 1.0 : 0.0, sq) : vec4(0.0);
        if (show == 3) return mix(lit, vec3(rv.a, 0.2, 1.0 - rv.a) * 0.6, wgt);
        if (show == 9) return mix(lit, vec3(clamp(sq, 0.0, 1.0), rv.a), wgt);
        env = sky * (1.0 - rv.a * min(ik, 1.0)) + rv.rgb * ik;
    } else {
    /* the cube's centre (ps2_video.c mir_cube_centres: the live map's block mp[cube]; light.w + 8 x the cube) */
    vec3 C = b[bi - 5].mp[(lw >> 3) & 3].xyz;
    vec4 ic = ik > 0.0 && show != 4 ? mir_interior(p - C, r, edge, nearcut, crisp) : vec4(0.0);
    if (show == 3) return mix(lit, vec3(ic.a, 0.2, 1.0 - ic.a) * 0.6, wgt);
    vec3 lv = amb + 0.45 * sc * (1.0 - kc);                      /* the cockpit's light now on what it shows */
    env = mix(sky, ic.rgb * lv * max(ik, 1.0), ic.a * min(ik, 1.0));
    }
    float cv = max(dot(nf, -D), 0.0), f5 = 1.0 - cv;
    f5 = f5 * f5 * f5 * f5 * f5;
    float refl = (0.82 + 0.14 * f5) * b[bi].spec_alpha;
    return mix(lit, env * refl * vec3(0.95, 0.98, 1.0) + lit * 0.035, wgt);
}

/* ACZ_VR_COCKPIT_SHADER_SPLIT (fq_opt 2026-10-26; ps2_video.c cpit_load_one): which parts the pipeline draws, so that the
   driver compiles only their code -- 0 any part (as before), 1 the solid parts' pipelines (no glass: light.w >= 0
   there), 2 the glass pipelines (light.w < 0 there: the canopy water, wear and cracks, no shadow map, flashes or
   mirrors). The same code runs for every part: the übershader's register count (95-96: 21 warps an SM) is set by its
   longest path (the canopy water), which held the solid parts' colour pass at that occupancy too */
layout(constant_id = 0) const int CPIT_KIND = 0;
void main() {
    vec4 c = cp.color;
    if (cp.light.z != 0.0) c *= SAMPLE(v_uv);
    bool glass = CPIT_KIND == 2 ? true : CPIT_KIND == 1 ? false : cp.light.w < 0.0;
    bool mir = CPIT_KIND == 2 ? false : cp.light.w > 1.5;   /* rear-view mirror glass (mirror_light; its alpha cut 0.5) */
    if (!glass && c.a < (mir ? 0.5 : cp.light.w)) discard;
    vec3 V = normalize(cp.head.xyz - v_p);
    vec3 n = normalize(v_n);
    if (dot(n, V) < 0.0) n = -n;                 /* the side facing the eye (the model's normals are not consistent) */
    float ao = clamp(v_ao, 0.0, 1.0);
    if (cp.uvoff.z < -0.5) ao = 0.5 + 0.5 * ao;  /* (the mirrors' cube bake: half the AO -- small and lit, the parts
                                                    went black under the full one) */
    vec3 amb, dir, L;
    float shade = 1.0, ssl = 0.0, kfac = 1.0;
    int bi = int(cp.rot[0].w), keyi = -1;
    if (bi >= 0) {
        /* the game's own model lighting (VU1 program, mesh.vert): 2 (n.l)^3 per light, coloured, plus the ambient */
        Block k = b[bi];
        vec3 d0 = vec3(k.l[0].x, k.l[1].x, k.l[2].x), d1 = vec3(k.l[0].y, k.l[1].y, k.l[2].y),
             d2 = vec3(k.l[0].z, k.l[1].z, k.l[2].z);
        vec3 l = vec3(dot(n, d0), dot(n, d1), dot(n, d2));
        l = max(2.0 * l * l * l, 0.0);
        /* the strongest light (the sun) gives the shadows and the highlights */
        vec3 lum = vec3(dot(k.r[0].rgb, vec3(1.0)) * length(d0), dot(k.r[1].rgb, vec3(1.0)) * length(d1),
                        dot(k.r[2].rgb, vec3(1.0)) * length(d2));
        int sun = lum.x >= lum.y && lum.x >= lum.z ? 0 : (lum.y >= lum.z ? 1 : 2);
        /* (ACZ_VR_COCKPIT_KEY_LIGHT, ps2_video.c cpit_key_frame: the CPU's key light, index + 1 in m[1].x -- the game light
           nearest C1's sun, sticky: the strongest flipped between the sun and a fixed camera light in M2) */
        int kf = int(k.m[1].x + 0.5);
        if (kf >= 1 && kf <= 3) sun = kf - 1;
        L = sun == 0 ? d0 : (sun == 1 ? d1 : d2);
        /* the shadow map only where the sun lights the surface at all (and not on glass) */
        float sv = l[sun] > 0.0 && !glass ? sun_visible(v_p, n, normalize(L)) : 1.0;
        l[sun] *= sv;
        dir = max(k.r[0].rgb * l.x + k.r[1].rgb * l.y + k.r[2].rgb * l.z, 0.0);
        amb = max(k.r[3].rgb, 0.0);
        shade = sv;
        /* the scene's light (ACZ_VR_COCKPIT_SCENE_LIGHT, ps2_video.c sl_*): the game's ambient and its other model
           lights (a fixed camera light, a light from straight above or below: made for the aircraft seen from outside)
           give way to the light of the world around the aircraft as the eye images show it -- the sky above, the
           horizon band, the ground below, measured before the cockpit is drawn, + the sun's bounce inside -- as a sky
           dome over the world's up. Its own reserved block, 3 below the light block (ps2_video.c MESH_SCENE_BLOCK):
           m[0].xyz the world's up (cockpit frame), m[0].w strength 0..1 (0 = as the game lights its models), m[1].rgb
           E_up, m[1].w the key light's factor, m[2].rgb E_side, m[3].rgb E_down; the key light (the sun) stays. Block
           index fraction 0.25 = without it (the A/B re-draw) */
        vec4 s0 = b[bi - 3].m[0], s1 = b[bi - 3].m[1];          /* (fields, not a copy of the whole block) */
        ssl = s0.w > 0.0 && (uint(fract(cp.rot[0].w) * 8.0 + 0.5) & 2u) == 0u ? min(s0.w, 1.0) : 0.0;
        keyi = sun; kfac = s1.w;
        if (ssl > 0.0) {
            vec3 key = (sun == 0 ? k.r[0].rgb * l.x : (sun == 1 ? k.r[1].rgb * l.y : k.r[2].rgb * l.z)) * kfac;
            float nu = clamp(dot(n, s0.xyz), -1.0, 1.0);
            vec3 es = b[bi - 3].m[2].rgb, env = nu >= 0.0 ? mix(es, s1.rgb, nu) : mix(es, b[bi - 3].m[3].rgb, -nu);
            amb = mix(amb, max(env, 0.0), ssl);
            dir = mix(dir, max(key, 0.0), ssl);
        }
    } else {
        L = normalize(vec3(0.15, 1.0, -0.35));
        amb = vec3(cp.light.y);
        dir = vec3((1.0 - cp.light.y) * max(dot(n, L), 0.0));
    }
    /* inside a cloud the sun gives way to the cloud's own diffuse light: from all around, a little more from above, in
       the cloud's colour -- shadows and highlights fade, the cockpit goes flat and grey (ps2_video.c cloud_light) */
    float kc = glass ? cp.sd.z : cp.wx.w;
    if (kc > 0.0) {
        vec3 cc = glass ? vec3(dot(amb, vec3(0.333)) * 1.35 + 0.06) : cp.wx.rgb;
        vec3 diffuse = cc * (0.62 + 0.38 * (0.5 + 0.5 * n.y));
        amb = mix(amb, diffuse, kc);
        dir *= 1.0 - kc;
        shade *= 1.0 - 0.8 * kc;
    }
    /* in a cloud's shadow (our clouds' shadow map at the aircraft, ACZ_VR_CLOUD_SHADOW): the direct sun and its
       highlights dim, the sky's ambient stays */
    if (!glass && cp.uvoff.w < 0.0) {
        float ks = clamp(-cp.uvoff.w, 0.0, 1.0);
        dir *= ks;
        shade *= ks;
    }
    /* (tunnel_light, bug #267) the glass with the sun hidden from the eye by a roof or the terrain (ps2_video.c sl_frame:
       the scene block's m[2].w = the hidden share): its sunlit tint, its highlight and the water's glints go; the other
       parts have it in uvoff.w (cloud_sun) */
    float ghid = glass && bi >= 3 ? clamp(b[bi - 3].m[2].w, 0.0, 1.0) : 0.0;
    if (ghid > 0.0) { dir *= 1.0 - ghid; shade *= 1.0 - ghid; }
    /* ambient occlusion darkens the ambient fully and the direct light a little (the sun has real shadows) */
    bool smv = glass ? cp.sv.x > 1.5 : cp.su.xyz != vec3(0.0);   /* (glass: su carries the debris flecks) */
    /* (the scene light's sky is occluded as light is: the occlusion is a share of the sky, in linear light; the
       colours here are display values, so it darkens by ao^(1 / 2.2) -- the game's own ambient kept its plain ao, and
       its fill lights, which the sky dome replaces, were barely occluded at all) */
    float aoa = ssl > 0.0 ? mix(ao, pow(ao, 0.4545), ssl) : ao;
    vec3 li = amb * aoa + dir * (smv ? 0.75 + 0.25 * ao : 0.4 + 0.6 * ao);
    /* flashes (not on glass; the block index's fraction 0.5 = without them: the A/B re-draw; + 0.25 = without the
       scene light; + 0.125 = without the night instrument lighting: bits of fract x 8) */
    if (bi >= 0 && !glass && (uint(fract(cp.rot[0].w) * 8.0 + 0.5) & 4u) == 0u && b[bi].m[0].w > 0.5) li += flash_light(bi, v_p, n, ao);
    vec3 col = c.rgb * li * cp.light.x;
    vec3 Ln = normalize(L);
    float spec = 0.0;
    if (cp.rot[1].w > 0.0 && dot(n, Ln) > 0.0) {
        vec3 h = normalize(Ln + V);
        spec = cp.rot[1].w * pow(max(dot(n, h), 0.0), max(cp.rot[2].w, 1.0)) * ao * shade;
    }
    vec3 sc = (bi >= 0 ? b[bi].r[0].rgb + b[bi].r[1].rgb + b[bi].r[2].rgb : vec3(1.0));
    sc = min(sc, vec3(1.5));
    /* (the scene light: highlights from the key light alone, at the game formula's peak, 2 x its colour) */
    if (ssl > 0.0 && keyi >= 0) sc = mix(sc, min(2.0 * b[bi].r[keyi].rgb * kfac, vec3(1.5)), ssl);
    sc *= 1.0 - ghid;                       /* (the glass: no sun to glint or to light the interior's reflection) */
    if (glass) {
        /* premultiplied: the tint lets the world through, the reflection (Fresnel) and highlight add */
        float f = pow(1.0 - max(dot(n, V), 0.0), 5.0);
        float a = clamp(c.a + (1.0 - c.a) * f * 0.35, 0.0, 1.0);
        vec3 refl = amb * 0.35 * f;
        out_color = vec4(col * c.a + refl + spec * sc, a);
        float wash;
        /* light.w = -(1, or 2 with fx_glass, 3 fx_glass without the dust) - 4 (crack mesh id + 1) (ps2_video.c: only on
           glass with cracks now) - 256 x the glass's thickness in whole mm (rain_layers, ACZ_VR_CANOPY_LAYERS; 0: none) */
        int gcode = int(-cp.light.w + 0.5), cmesh = ((gcode >> 2) & 63) - 1;
        /* (rain_layers, ACZ_VR_CANOPY_LAYERS) the outside weather lies on the glass's outer face: where this eye's ray
           leaves the acrylic, the reflections, the wear and the cracks stay on the mesh (the inner face) */
        float thk = float(gcode >> 8) * 0.001;
        out_color = canopy_water(v_p, n, V, Ln, amb, sc, shade, out_color, thk, wash);
        float hole = 0.0;
#ifdef GXV
        if (GXOFF(GXV_CRK)) cmesh = -1;
#endif
        if (cmesh >= 0) {                                                                    /* (fx_cracks) */
            vec4 k = crk_glass(v_p, cmesh, amb, wash, hole);
            if (hole > 0.5) out_color = vec4(0.0);     /* a hole: no glass (premultiplied nothing: the world shows) */
            out_color = crk_over(k, out_color);
        }
#ifdef GXV
        if (GXOFF(GXV_LIGHT)) gcode &= ~3;
#endif
        if ((gcode & 3) >= 2 && bi >= 0 && hole < 0.5) {   /* (fx_glass; 3: without the dust and the all-angle reflection) */
            vec3 gx = glass_light(v_p, n, wash, bi, amb, sc, (gcode & 3) == 3);
            /* clear glass seen square-on has alpha 0, and the composite (cockpit_comp.frag) drops alpha-0 texels: what
               such a texel held never showed (the glass's own highlight there) and stays out; light the glass adds
               needs the least alpha a texel holds (the world behind 0.4 % darker there) */
            vec3 o = max((out_color.a < 0.5 / 255.0 ? vec3(0.0) : out_color.rgb) + gx, vec3(0.0));
            if (o.r + o.g + o.b > 0.002) out_color = vec4(o, max(out_color.a, 1.0 / 255.0));
        }
        return;
    }
    vec3 o = col + spec * sc;
    /* night instrument lighting (bug #197; ps2_video.c nl_pack: b[bi].p[3].w = level, tint): the panel's markings --
       small light, unsaturated features on a darker ground (pointers, numerals, scales, integrally lit legends): the
       texel against its own texture's 8x8-texel mean (3 mip levels up), so large light surfaces (a seat frame, light
       grey paint) do not count -- glow in the light's tint, not occluded (lit from within); every surface of the panel
       area gets a faint flood, texture-modulated and occluded (black stays black). Panel area, from the head in the
       cockpit frame (x right, y up, z back): below the eye by 6 cm or more, within 1.4 m, the main panel ahead
       (z < -0.3 m) and the side consoles (|x| > 0.22 m) -- not the seat, the canopy frame or the headrest; not the glass
       (returned above) or the screens (their own pass) */
    if (bi >= 0 && b[bi].p[3].w >= 1.0 && (uint(fract(cp.rot[0].w) * 8.0 + 0.5) & 1u) == 0u) {
        uint nq = uint(b[bi].p[3].w + 0.5);
        float nlv = float(nq & 255u) / 127.5;
        vec3 tint = vec3(float((nq >> 18) & 31u), float((nq >> 13) & 31u), float((nq >> 8) & 31u)) / 31.0;
        vec3 rp = v_p - cp.head.xyz;                   /* (from the head: the model's origin is not the eye) */
        float area = (1.0 - smoothstep(-0.12, -0.06, rp.y)) * (1.0 - smoothstep(1.1, 1.4, length(rp)))
                   * max(1.0 - smoothstep(-0.38, -0.28, rp.z), smoothstep(0.18, 0.26, abs(rp.x)))
                   * (1.0 - smoothstep(0.15, 0.35, rp.z));
        if (area > 0.0) {
            float mk = 0.0;
            if (cp.light.z != 0.0) {
                vec3 t0 = SAMPLE(v_uv).rgb, t3 = SAMPLE_BIAS(v_uv, 3.0).rgb;
                float l0 = dot(t0, vec3(0.299, 0.587, 0.114)), l3 = dot(t3, vec3(0.299, 0.587, 0.114));
                float sat = max(t0.r, max(t0.g, t0.b)) - min(t0.r, min(t0.g, t0.b));
                mk = smoothstep(0.35, 0.6, l0) * smoothstep(0.10, 0.28, l0 - l3) * (1.0 - smoothstep(0.12, 0.35, sat));
            }
            o += c.rgb * tint * (0.10 * ao + 0.50 * mk) * (nlv * area);
        }
    }
    if (mir && bi >= 0) o = mirror_light(v_p, n, bi, c, o, amb, sc, kc);
    /* (uvoff.z -1: the mirrors' cube bake, ps2_video.c mir_cube_init -- the distance from its centre / 4 m in alpha) */
    out_color = vec4(o, cp.uvoff.z < -0.5 ? clamp(length(v_p - cp.head.xyz) * 0.25, 1.0 / 255.0, 1.0) : 1.0);
}
