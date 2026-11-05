/* zero_terrain_mat.c -- realistic terrain stage T3+ (TERRAIN_RESEARCH_2026-10-21.md, widened to all near ground; bug #78,
   the user's wish "individual detail, textures"): near-ground materials. Shading only: the surface the game draws and
   collides with is unchanged.

   Why: the ground near a low aircraft is the 2006 aerial photo (64 x 64 texel tiles, 2048 units = ~341 m: 5.3 m per
   texel) magnified 10-40x; the game's two detail textures (1.3 m per texel, fading to 0 at 9140 units) add a faint
   mottle. Fx_visual's #73 measured the near ground's high-pass energy at 0.35-0.47 levels against the runway's 3.4: it
   reads as murky paint, not as a material, at 30-300 m.

   What: a procedural texture array (made here at start-up on 4 lowest-priority threads, ~1 s; no image files, so
   nothing to license) with one layer per material -- 0 grass / fields (tufts, clumps), 1 sand (patchy wind ripples,
   soft swells, grain), 2 rock / scree (ridged relief, irregular fractures, tilted blocks, stones), 3 snow (wind-shaped
   undulation), 4 soil (clods, a few cracks, mottle). Every layer is one 1024^2 tile of 32 m (3 cm texels) holding its
   pattern at two scales (the fine one and the same 4x larger: 3 cm to 32 m); each RGBA8 texel = the detail surface's
   slope x / z, its height and an albedo variation, all mean-neutral (128 = 0; box-filtered mips: a minified pixel gets
   its footprint's mean -- the detail fades to nothing, never a brightness shift). The terrain's first pass of the
   patches near the eye takes a mesh.frag variant (-DTDETAIL, terrain_detail.glsl) that classifies each pixel by the
   atlas texel's colour (green -> grass, bright warm -> sand, grey -> rock, dark -> soil, white in a snow mission ->
   snow) and the smooth ground's slope (steep -> rock; the T2 bake's shading surface, ps2_terrain_ground, written per
   vertex into the record's second quadword here), fetches the strongest material once (world XZ, implicit
   derivatives in uniform control flow, the same uv for every material), height-blends a second one where two are
   close (explicit derivatives) and multiplies the colour by (1 + albedo detail) x (1 + the detail slope's sun light,
   linear in the slope so its mean is exactly 1; the gain per vertex from the sun, our cloud shadow and the smooth
   ground's own light). The multiplier keeps the atlas colour's hue and local mean (the 2006 palette stays); it fades
   with the distance from the eye (the same for both eyes, world-fixed like ACZ_VR_TERRAIN_MORPH) from full strength
   within ACZ_TERRAIN_MATERIALS_DIST's first value to 0 at its second, and varies slowly (x 0.75..1.25 over ~1 km). No
   TAA here: mipmapped, trilinear, anisotropy 4, +0.5 mip bias, so detail finer than about a pixel is filtered to its
   mean instead of sparkling.

   Block fields of a material patch (prog 1 pass 0; free for terrain): l[0] = towards the sun (world, y up), strength;
   l[1] = full-strength distance (units), 1 / fade length, snow mission (0/1), debug view + 16 x cost bits; r[0..3] =
   layers 0-3 and l[2] = layer 4: (-, slope scale, albedo scale, 2^mip bias); e[8..11] = the eye (world units), 1 (the
   shader water's place). Record quadword 1 (int): slope x 4096 (x, z), cavity (units), 1 = ground known.

   Switches: ACZ_TERRAIN_MATERIALS (master, read at start), _STRENGTH (1), _DIST (near,far metres: 300,1500), _MIX
   (per-material strengths grass,sand,rock,snow,soil), _BIAS (mip bias 0.5), _ANISO (4), _AB (in-run GPU A/B,
   ps2_video.c); debug ACZ_DEBUG_TERRAIN_MAT (1 log), ACZ_DEBUG_TERRAIN_MAT_TINT (1 the chosen material as a colour, 2
   the multiplier alone, 3 the distance fade, 4 the atlas texel / 2), ACZ_DEBUG_TERRAIN_MAT_DUMP=dir (the generated
   layers as PPM / PGM), ACZ_DEBUG_TERRAIN_MAT_COST (cost hunt bits), ACZ_DEBUG_TERRAIN_MAT_BLOCK=f (one marked
   block's fields); PS2_PERF_AB_FLIP=tmat (exact same-frame image A/B). */
#include "ps2_runtime.h"
#include "ps2_menv.h"
#include "ps2_terrain.h"
#include "ps2_vk.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(__has_include)
#if __has_include("ps2_clouds.h")
#include "ps2_clouds.h"
#define TM_C1 1
#endif
#endif
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#define TM_PI 3.14159265f
#define TM_UPM 6.0f                         /* game units per metre (handbook: ~6, +-15 %) */
#define TM_L PS2_TMAT_LAYERS

extern volatile float ps2_imm_freeze_m;     /* ps2_video.c: the mission's freezing level (0 m in the snow missions) */
extern u32 ps2_gs_frame_count;

/* per material: RMS slope of the detail surface, RMS albedo variation. Every layer is one 32 m tile (TM_TILE_M; the
   shader's TD_TILE) holding two scales of its pattern: the fine one (tufts, ripples, pebbles: 3 cm - 8 m) and the same
   pattern 4x larger (clumps, mega-ripples, blocks: up to 32 m) with 2.4x its height (a self-affine ground: 0.6 of the
   fine slope) and 0.8x its albedo variation, so one fetch per material covers 3 cm to 32 m. */
#define TM_TILE_M 32.0f
typedef struct { const char *name; float slope, albedo; } tm_mat;
static const tm_mat TM_MAT[TM_L] = {
    { "grass", 0.30f, 0.075f },
    { "sand", 0.22f, 0.040f },
    { "rock", 0.55f, 0.100f },
    { "snow", 0.12f, 0.018f },
    { "soil", 0.32f, 0.065f },
};

static int tm_on = -1, tm_dbg, tm_tint, tm_ab, tm_cost;
static u32 tm_n = 1024, tm_levels;
static float tm_strength = 1.0f, tm_d0 = 300.0f, tm_d1 = 1500.0f, tm_bias = 0.5f, tm_aniso = 4.0f;
static float tm_mix[TM_L] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
static char tm_dump[260];

/* the generated texture (all levels) and its per-layer scales */
static u8 *volatile tm_tex;
static size_t tm_tex_bytes;
static float tm_ks[TM_L], tm_ka[TM_L];
static volatile int tm_ready;
static int tm_started;
static pthread_mutex_t tm_mx = PTHREAD_MUTEX_INITIALIZER;
static u64 st_patches, st_marked, st_noground, st_noeye;
static float st_slope_max;                    /* (debug: the steepest smooth ground of the marked patches) */
static u64 st_steep[4];                       /* (debug: vertices with slope > 0.2 / 0.3 / 0.4 / 0.5) */

static float env_f(const char *n, float def, float lo, float hi) {
    const char *e = getenv(n);
    if (!e || !*e) return def;
    float v = (float)atof(e);
    return v < lo ? lo : v > hi ? hi : v;
}

static void tm_cfg(void) {
    if (tm_on >= 0) return;
    const char *e = ps2_menv("ACZ_TERRAIN_MATERIALS");
    int on = 1;                                         /* default: see the env reference */
    if (e && *e) on = *e != '0';
    tm_strength = env_f("ACZ_TERRAIN_MATERIALS_STRENGTH", 1.0f, 0.0f, 3.0f);
    e = getenv("ACZ_TERRAIN_MATERIALS_DIST");
    if (e && *e) {
        float a, b;
        if (sscanf(e, "%f,%f", &a, &b) == 2 && a >= 0.0f && b > a + 10.0f && b < 20000.0f) { tm_d0 = a; tm_d1 = b; }
    }
    e = getenv("ACZ_TERRAIN_MATERIALS_MIX");
    if (e && *e) {
        float v[TM_L];
        int k = sscanf(e, "%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3], &v[4]);
        for (int i = 0; i < k && i < (int)TM_L; i++) tm_mix[i] = v[i] < 0.0f ? 0.0f : v[i] > 3.0f ? 3.0f : v[i];
    }
    tm_bias = env_f("ACZ_TERRAIN_MATERIALS_BIAS", 0.5f, -1.0f, 3.0f);
    tm_aniso = env_f("ACZ_TERRAIN_MATERIALS_ANISO", 4.0f, 1.0f, 16.0f);
    tm_levels = 0;
    for (u32 s = tm_n; s; s >>= 1) tm_levels++;
    e = getenv("ACZ_TERRAIN_MATERIALS_AB");
    tm_ab = e ? atoi(e) : 0;
    if (tm_ab < 0) tm_ab = 0;
    e = getenv("ACZ_DEBUG_TERRAIN_MAT");
    tm_dbg = e && *e && *e != '0';
    e = getenv("ACZ_DEBUG_TERRAIN_MAT_TINT");
    tm_tint = e ? atoi(e) : 0;
    e = getenv("ACZ_DEBUG_TERRAIN_MAT_COST");      /* (bits: 1 one material, 8 no fetch, 16 nothing) */
    tm_cost = e ? atoi(e) & 31 : 0;
    e = getenv("ACZ_DEBUG_TERRAIN_MAT_DUMP");
    if (e && *e) snprintf(tm_dump, sizeof tm_dump, "%s", e);
    tm_on = on;
}

int ps2_tmat_on(void) { tm_cfg(); return tm_on; }
/* the NEXT MISSION tier (ps2_menv.h): the game thread, at the edge. Off: no patch is marked (ps2_tmat_patch); the textures stay
   for the way back, on only if they were made at the start (enh_boot_tab) */
void zero_tmat_menv_reload(void) {
    if (tm_on < 0) return;
    const int a = tm_on;
    const char *e = ps2_menv("ACZ_TERRAIN_MATERIALS");
    tm_on = !(e && *e && *e == '0');
    ps2_menv_note("ACZ_TERRAIN_MATERIALS", a, tm_on);
}
int ps2_tmat_ab_n(void) { tm_cfg(); return tm_on ? tm_ab : 0; }
float ps2_tmat_aniso(void) { tm_cfg(); return tm_aniso; }

/* ---- the procedural layers ------------------------------------------------------------------------------------- */
static float tm_gx[256], tm_gy[256];          /* unit gradients */
static u32 tm_hash(u32 x, u32 y, u32 s) {
    u32 h = x * 0x8DA6B343u ^ y * 0xD8163841u ^ s * 0xCB1AB31Fu ^ 0x5BD1E995u;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return h;
}
static int tm_mod(int a, int p) { int r = a % p; return r < 0 ? r + p : r; }
/* gradient noise with a period of (px, py) lattice cells; about -1..1 */
static float tm_gn(float x, float y, int px, int py, u32 s) {
    float fx0 = floorf(x), fy0 = floorf(y);
    int ix = (int)fx0, iy = (int)fy0;
    float fx = x - fx0, fy = y - fy0;
    float ux = fx * fx * fx * (fx * (fx * 6.0f - 15.0f) + 10.0f), uy = fy * fy * fy * (fy * (fy * 6.0f - 15.0f) + 10.0f);
    float v[4];
    for (int c = 0; c < 4; c++) {
        int cx = c & 1, cy = c >> 1;
        u32 h = tm_hash((u32)tm_mod(ix + cx, px), (u32)tm_mod(iy + cy, py), s) & 255u;
        v[c] = tm_gx[h] * (fx - (float)cx) + tm_gy[h] * (fy - (float)cy);
    }
    float a = v[0] + (v[1] - v[0]) * ux, b = v[2] + (v[3] - v[2]) * ux;
    return 1.6f * (a + (b - a) * uy);
}
/* fractal sum over the tile (u, v in 0..1): octave o has (p0 x 2^o) cells per side */
static float tm_fbm(float u, float v, int p0, int oct, float gain, u32 s) {
    float sum = 0.0f, amp = 1.0f, norm = 0.0f;
    int p = p0;
    for (int o = 0; o < oct; o++) {
        sum += amp * tm_gn(u * (float)p, v * (float)p, p, p, s + (u32)o * 977u);
        norm += amp; amp *= gain; p *= 2;
    }
    return sum / norm;
}
/* ridged fractal (sharp crests): sum of (1 - |n|)^2 */
static float tm_ridged(float u, float v, int p0, int oct, float gain, u32 s) {
    float sum = 0.0f, amp = 1.0f, norm = 0.0f;
    int p = p0;
    for (int o = 0; o < oct; o++) {
        float n = 1.0f - fabsf(tm_gn(u * (float)p, v * (float)p, p, p, s + (u32)o * 613u));
        sum += amp * n * n;
        norm += amp; amp *= gain; p *= 2;
    }
    return sum / norm;
}
/* cellular: distances (in cells) to the nearest and second-nearest jittered point, period p cells; *cr = a random
   value 0..1 of the nearest cell */
static void tm_worley(float u, float v, int p, u32 s, float *f1, float *f2, float *cr) {
    float x = u * (float)p, y = v * (float)p;
    int ix = (int)floorf(x), iy = (int)floorf(y);
    float d1 = 1e9f, d2 = 1e9f, c1 = 0.0f;
    for (int b = -1; b <= 1; b++)
        for (int a = -1; a <= 1; a++) {
            u32 h = tm_hash((u32)tm_mod(ix + a, p), (u32)tm_mod(iy + b, p), s);
            float px = (float)(ix + a) + 0.05f + 0.9f * (float)(h & 0xFFFFu) * (1.0f / 65535.0f);
            float py = (float)(iy + b) + 0.05f + 0.9f * (float)(h >> 16) * (1.0f / 65535.0f);
            float d = sqrtf((px - x) * (px - x) + (py - y) * (py - y));
            if (d < d1) { d2 = d1; d1 = d; c1 = (float)(tm_hash(h, 7u, s) & 0xFFFFu) * (1.0f / 65535.0f); }
            else if (d < d2) d2 = d;
        }
    *f1 = d1; *f2 = d2;
    if (cr) *cr = c1;
}
static float tm_sat(float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; }
static float tm_sstep(float a, float b, float x) { float t = tm_sat((x - a) / (b - a)); return t * t * (3.0f - 2.0f * t); }
/* the tile point (u, v) moved by a periodic warp field of amplitude amp (tile units) */
static void tm_warp(float *u, float *v, int p0, float amp, u32 s) {
    float wu = tm_fbm(*u, *v, p0, 3, 0.5f, s), wv = tm_fbm(*u, *v, p0, 3, 0.5f, s + 101u);
    *u += amp * wu; *v += amp * wv;
}

/* one texel of layer k's pattern at tile coordinates (u, v), its lattices S times as fine (S = 4: the fine scale in
   the 32 m tile, features as in an 8 m tile; S = 1: four times larger): height h and albedo a (any scale; normalised
   later). Natural irregularity: every cellular pattern runs on a warped domain and its cells vary in height and tone. */
static void tm_texel_s(int k, float u, float v, int S, float *h, float *a) {
    const float iS = 1.0f / (float)S;
    const u32 sd = (u32)S * 1000u;
    switch (k) {
    case 0: {   /* grass / fields: tufts of varied size and height, clumps (~1 m at the fine scale), colour mottle */
        float f1, f2, c1, g1, g2, c2, wu = u, wv = v;
        tm_warp(&wu, &wv, 6 * S, 0.012f * iS, 15u + sd);
        tm_worley(wu, wv, 36 * S, 11u + sd, &f1, &f2, &c1);
        tm_worley(wu, wv, 9 * S, 12u + sd, &g1, &g2, &c2);
        float tuft = (1.0f - tm_sstep(0.0f, 0.55f + 0.35f * c1, f1)) * (0.4f + 0.6f * c1);
        float clump = (1.0f - tm_sstep(0.05f, 0.95f, g1)) * (0.5f + 0.5f * c2);
        float n = tm_fbm(u, v, 24 * S, 4, 0.55f, 13u + sd);
        *h = 0.35f * tuft + 0.30f * clump + 0.45f * n;
        *a = 0.55f * tm_fbm(u, v, 6 * S, 5, 0.6f, 14u + sd) + 0.25f * clump + 0.20f * tuft + 0.15f * (c2 - 0.5f);
        break;
    }
    case 1: {   /* sand: wind ripples (~12 per tile) with a warped, broken phase; grain */
        if (S < 4) {    /* (the large scale: soft wind-shaped swells, no second set of ripples) */
            float sw = 0.0f, am = 1.0f, no = 0.0f;
            for (int o = 0; o < 4; o++) {
                int px = (2 << o) * S, py = (5 << o) * S;
                sw += am * tm_gn(u * (float)px, v * (float)py, px, py, 27u + (u32)o * 5u + sd);
                no += am; am *= 0.5f;
            }
            *h = sw / no;
            *a = 0.5f * (sw / no) + 0.5f * tm_fbm(u, v, 4 * S, 4, 0.55f, 28u + sd);
            break;
        }
        float w = tm_fbm(u, v, 3 * S, 3, 0.5f, 21u + sd), m = tm_fbm(u, v, 4 * S, 3, 0.5f, 22u + sd);
        float w2 = tm_fbm(u, v, 8 * S, 2, 0.5f, 26u + sd);
        float ph = 2.0f * TM_PI * (12.0f * (float)S * u + 2.0f * (float)S * v) + 5.0f * w + 1.4f * w2;
        float r = sinf(ph) + 0.30f * sinf(2.0f * ph + 0.6f);
        float env = tm_sstep(-0.15f, 0.45f, m);          /* (patches of ripples, smooth sand between) */
        float grain = tm_fbm(u, v, 64 * S, 3, 0.5f, 23u + sd);
        *h = 0.50f * r * env + 0.30f * grain + 0.35f * tm_fbm(u, v, 6 * S, 3, 0.5f, 24u + sd);
        *a = 0.20f * r * env + 0.55f * grain + 0.50f * tm_fbm(u, v, 8 * S, 4, 0.55f, 25u + sd);
        break;
    }
    case 2: {   /* rock / scree: ridged relief, irregular fractures, tilted blocks, scattered stones, stains */
        float f1, f2, c1, wu = u, wv = v;
        tm_warp(&wu, &wv, 4 * S, 0.06f * iS, 36u + sd);
        tm_worley(wu, wv, 7 * S, 31u + sd, &f1, &f2, &c1);
        float crack = (1.0f - tm_sstep(0.0f, 0.06f + 0.06f * c1, f2 - f1))
                    * (0.5f + 0.5f * tm_sat(0.5f + tm_fbm(u, v, 5 * S, 2, 0.5f, 37u + sd)));
        float rd = tm_ridged(u, v, 4 * S, 6, 0.55f, 32u + sd);
        float pebble, p2, pc;
        tm_worley(u, v, 48 * S, 33u + sd, &pebble, &p2, &pc);
        float peb = (1.0f - tm_sstep(0.0f, 0.45f, pebble)) * (pc > 0.55f ? 1.0f : 0.0f);
        *h = 0.85f * rd - 0.25f * crack + 0.20f * (c1 - 0.5f) + 0.12f * peb + 0.15f * tm_fbm(u, v, 8 * S, 4, 0.5f, 34u + sd);
        *a = 0.55f * tm_fbm(u, v, 5 * S, 5, 0.6f, 35u + sd) - 0.30f * crack + 0.25f * rd + 0.15f * (c1 - 0.5f) + 0.08f * peb;
        break;
    }
    case 3: {   /* snow: soft undulations stretched by the wind (sastrugi), faint grain */
        float s1 = 0.0f, amp = 1.0f, norm = 0.0f;
        for (int o = 0; o < 4; o++) {
            int px = (3 << o) * S, py = (8 << o) * S;
            s1 += amp * tm_gn(u * (float)px, v * (float)py, px, py, 41u + (u32)o * 7u + sd);
            norm += amp; amp *= 0.5f;
        }
        s1 /= norm;
        *h = 0.85f * s1 + 0.12f * tm_fbm(u, v, 48 * S, 3, 0.5f, 42u + sd);
        *a = 0.60f * tm_fbm(u, v, 6 * S, 4, 0.5f, 43u + sd) + 0.40f * s1;
        break;
    }
    default: {  /* soil: clods and pebbles of varied size, a few irregular cracks, moisture mottle */
        float f1, f2, c1, g1, g2, c2, wu = u, wv = v;
        tm_warp(&wu, &wv, 5 * S, 0.04f * iS, 55u + sd);
        tm_worley(wu, wv, 40 * S, 51u + sd, &f1, &f2, &c1);
        tm_worley(wu, wv, 11 * S, 52u + sd, &g1, &g2, &c2);
        float clod = (1.0f - tm_sstep(0.0f, 0.6f + 0.3f * c1, f1)) * (0.3f + 0.7f * c1);
        float crack = (1.0f - tm_sstep(0.0f, 0.05f, g2 - g1)) * (c2 > 0.4f ? 1.0f : 0.3f);
        *h = 0.35f * clod - 0.15f * crack + 0.50f * tm_fbm(u, v, 12 * S, 5, 0.55f, 53u + sd);
        *a = 0.60f * tm_fbm(u, v, 5 * S, 5, 0.6f, 54u + sd) - 0.15f * crack + 0.20f * clod + 0.12f * (c2 - 0.5f);
        break;
    }
    }
}
/* the layer's texel: the fine pattern plus the same pattern 4x larger (2.4x the height: 0.6 of the fine slope) */
static void tm_texel(int k, float u, float v, float *h, float *a) {
    float hf, af, hc, ac;
    tm_texel_s(k, u, v, 4, &hf, &af);
    tm_texel_s(k, u, v, 1, &hc, &ac);
    *h = hf + 2.4f * hc;
    *a = af + 0.8f * ac;
}

typedef struct {
    u32 n;
    float *h, *a;                              /* TM_L x n x n */
    volatile long next;
} tm_job;
static void *tm_worker(void *u) {
    tm_job *j = (tm_job *)u;
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    { extern void ps2_host_thread_start(const char *); ps2_host_thread_start("bg"); }  /* (PS2_THREAD_CORES=h) */
#endif
    u32 n = j->n;
    for (;;) {
        long r = __sync_fetch_and_add(&j->next, 1);
        if (r >= (long)(TM_L * n)) break;
        int k = (int)(r / (long)n), y = (int)(r % (long)n);
        float *ho = j->h + ((size_t)k * n + (size_t)y) * n, *ao = j->a + ((size_t)k * n + (size_t)y) * n;
        for (u32 x = 0; x < n; x++)
            tm_texel(k, ((float)x + 0.5f) / (float)n, ((float)y + 0.5f) / (float)n, &ho[x], &ao[x]);
    }
    return NULL;
}
static double tm_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}
static u8 tm_q(float v) { long q = lrintf(128.0f + 127.0f * v); return (u8)(q < 1 ? 1 : q > 255 ? 255 : q); }

static void *tm_build(void *unused) {
    (void)unused;
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    { extern void ps2_host_thread_start(const char *); ps2_host_thread_start("bg"); }  /* (PS2_THREAD_CORES=h) */
#endif
    double t0 = tm_now_ms();
    u32 n = tm_n;
    for (int i = 0; i < 256; i++) { float an = (float)i * (2.0f * TM_PI / 256.0f) + 0.37f; tm_gx[i] = cosf(an); tm_gy[i] = sinf(an); }
    tm_job j;
    memset(&j, 0, sizeof j);
    j.n = n;
    j.h = (float *)malloc(sizeof(float) * TM_L * n * n);
    j.a = (float *)malloc(sizeof(float) * TM_L * n * n);
    size_t bytes = 0;
    for (u32 s = n; s; s >>= 1) bytes += (size_t)s * s * 4u * TM_L;
    u8 *out = (u8 *)malloc(bytes);
    if (!j.h || !j.a || !out) { free(j.h); free(j.a); free(out); ps2_log("terrain-mat: out of memory"); return NULL; }
    pthread_t th[4];
    int made = 0;
    for (int i = 0; i < 3; i++) if (pthread_create(&th[made], NULL, tm_worker, &j) == 0) made++;
    tm_worker(&j);
    for (int i = 0; i < made; i++) pthread_join(th[i], NULL);
    double t1 = tm_now_ms();
    /* per layer: mean-neutral, normalised; slopes by central differences (the tile wraps) */
    for (u32 k = 0; k < TM_L; k++) {
        float *h = j.h + (size_t)k * n * n, *a = j.a + (size_t)k * n * n;
        double mh = 0.0, ma = 0.0;
        for (size_t i = 0; i < (size_t)n * n; i++) { mh += h[i]; ma += a[i]; }
        mh /= (double)n * n; ma /= (double)n * n;
        float xh = 1e-6f, xa = 1e-6f;
        for (size_t i = 0; i < (size_t)n * n; i++) {
            h[i] -= (float)mh; a[i] -= (float)ma;
            if (fabsf(h[i]) > xh) xh = fabsf(h[i]);
            if (fabsf(a[i]) > xa) xa = fabsf(a[i]);
        }
        float smax = 1e-6f;
        double ss = 0.0, sa = 0.0;
        for (u32 y = 0; y < n; y++)
            for (u32 x = 0; x < n; x++) {
                float sx = (h[y * n + (x + 1) % n] - h[y * n + (x + n - 1) % n]) * 0.5f / xh;
                float sz = (h[((y + 1) % n) * n + x] - h[((y + n - 1) % n) * n + x]) * 0.5f / xh;
                if (fabsf(sx) > smax) smax = fabsf(sx);
                if (fabsf(sz) > smax) smax = fabsf(sz);
            }
        u8 *o = out + (size_t)k * n * n * 4u;           /* level 0, layer k */
        for (u32 y = 0; y < n; y++)
            for (u32 x = 0; x < n; x++) {
                float sx = (h[y * n + (x + 1) % n] - h[y * n + (x + n - 1) % n]) * 0.5f / xh / smax;
                float sz = (h[((y + 1) % n) * n + x] - h[((y + n - 1) % n) * n + x]) * 0.5f / xh / smax;
                float an = a[y * n + x] / xa;
                ss += (double)(sx * sx + sz * sz); sa += (double)(an * an);
                u8 *t = o + ((size_t)y * n + x) * 4u;
                t[0] = tm_q(sx); t[1] = tm_q(sz); t[2] = tm_q(h[y * n + x] / xh); t[3] = tm_q(an);
            }
        float rs = (float)sqrt(ss / ((double)n * n)), ra = (float)sqrt(sa / ((double)n * n));
        /* the shader decodes (v * 255 - 128) / 127 */
        tm_ks[k] = rs > 1e-4f ? TM_MAT[k].slope / rs : 0.0f;
        tm_ka[k] = ra > 1e-4f ? TM_MAT[k].albedo / ra : 0.0f;
        if (tm_dbg) ps2_log("terrain-mat: layer %u (%s): tile %.1f m, stored slope RMS %.3f -> scale %.3f, albedo RMS %.3f -> scale %.3f",
                            k, TM_MAT[k].name, TM_TILE_M, rs, tm_ks[k], ra, tm_ka[k]);
    }
    /* mips: 2 x 2 box filter per layer (the means stay 128: minified detail fades to nothing) */
    {
        size_t off = 0;
        for (u32 s = n; s > 1u; s >>= 1) {
            u32 d = s >> 1;
            size_t lvl = (size_t)s * s * 4u * TM_L, doff = off + lvl;
            for (u32 k = 0; k < TM_L; k++) {
                const u8 *src = out + off + (size_t)k * s * s * 4u;
                u8 *dst = out + doff + (size_t)k * d * d * 4u;
                for (u32 y = 0; y < d; y++)
                    for (u32 x = 0; x < d; x++)
                        for (u32 c = 0; c < 4u; c++) {
                            const u8 *p = src + ((size_t)(2u * y) * s + 2u * x) * 4u + c;
                            u32 sum = (u32)p[0] + p[4] + p[(size_t)s * 4u] + p[(size_t)s * 4u + 4u];
                            dst[((size_t)y * d + x) * 4u + c] = (u8)((sum + 2u - ((x + y) & 1u)) >> 2);   /* (unbiased rounding) */
                        }
            }
            off = doff;
        }
    }
    double t2 = tm_now_ms();
    if (tm_dump[0]) {
        for (u32 k = 0; k < TM_L; k++) {
            char path[400];
            const u8 *o = out + (size_t)k * n * n * 4u;
            snprintf(path, sizeof path, "%s/tmat_%u_%s_rgb.ppm", tm_dump, k, TM_MAT[k].name);
            FILE *f = fopen(path, "wb");
            if (f) {
                fprintf(f, "P6\n%u %u\n255\n", n, n);
                for (size_t i = 0; i < (size_t)n * n; i++) fwrite(o + i * 4u, 1, 3, f);
                fclose(f);
            }
            snprintf(path, sizeof path, "%s/tmat_%u_%s_albedo.pgm", tm_dump, k, TM_MAT[k].name);
            f = fopen(path, "wb");
            if (f) {
                fprintf(f, "P5\n%u %u\n255\n", n, n);
                for (size_t i = 0; i < (size_t)n * n; i++) fputc(o[i * 4u + 3u], f);
                fclose(f);
            }
        }
    }
    free(j.h); free(j.a);
    pthread_mutex_lock(&tm_mx);
    tm_tex = out; tm_tex_bytes = bytes;
    __atomic_store_n(&tm_ready, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&tm_mx);
    ps2_log("terrain-mat: material texture %ux%u x %u layers, %u levels, %.1f MB made in %.0f ms (%.0f texels, %.0f "
            "normalise + mips) on %d threads", n, n, TM_L, tm_levels, (double)bytes / 1048576.0, t2 - t0, t1 - t0,
            t2 - t1, made + 1);
    return NULL;
}

uint32_t ps2_tmat_size(uint32_t *levels) {
    tm_cfg();
    if (!tm_on) { if (levels) *levels = 1u; return 0u; }
    if (levels) *levels = tm_levels;
    if (!tm_started) {
        tm_started = 1;
        pthread_t th;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&th, &at, tm_build, NULL) != 0) ps2_log("terrain-mat: texture thread failed");
        pthread_attr_destroy(&at);
        ps2_log("terrain-mat: near-ground materials on (ACZ_TERRAIN_MATERIALS): strength %.2f, full to %.0f m, 0 at %.0f m, "
                "mix %.2f/%.2f/%.2f/%.2f/%.2f (grass/sand/rock/snow/soil), mip bias %.2f, anisotropy %.0f%s%s",
                tm_strength, tm_d0, tm_d1, tm_mix[0], tm_mix[1], tm_mix[2], tm_mix[3], tm_mix[4], tm_bias, tm_aniso,
                tm_tint ? ", debug tint" : "", tm_ab ? ", in-run A/B" : "");
    }
    return tm_n;
}

int ps2_tmat_copy(void *dst, size_t cap) {
    if (!__atomic_load_n(&tm_ready, __ATOMIC_ACQUIRE)) return 0;
    int ok = 0;
    pthread_mutex_lock(&tm_mx);
    if (tm_tex && tm_tex_bytes <= cap) {          /* (uploaded once: the host copy goes) */
        memcpy(dst, tm_tex, tm_tex_bytes); ok = 1;
        free(tm_tex); tm_tex = NULL;
    }
    pthread_mutex_unlock(&tm_mx);
    return ok;
}

/* ---- per patch (GS thread) -------------------------------------------------------------------------------------- */
static u32 tm_frame = 0xFFFFFFFFu;
static float tm_sun[3] = { -0.35f, 0.8f, -0.48f }, tm_snow;
static void tm_frame_values(void) {
    if (tm_frame == ps2_gs_frame_count) return;
    tm_frame = ps2_gs_frame_count;
#ifdef TM_C1
    static ps2_cloud_env e;
    static u32 last_read = 0xFFFFFFFFu;
    if (last_read == 0xFFFFFFFFu || tm_frame - last_read >= 30u) {     /* (the mission's sun is fixed: twice a second) */
        last_read = tm_frame;
        if (ps2_cloud_env_read(&e) && (e.valid & CL_ENV_SUN)) {
            float l = sqrtf(e.sun_dir[0] * e.sun_dir[0] + e.sun_dir[1] * e.sun_dir[1] + e.sun_dir[2] * e.sun_dir[2]);
            if (l > 0.5f && l < 1.5f) for (int k = 0; k < 3; k++) tm_sun[k] = e.sun_dir[k] / l;
        }
    }
#endif
    tm_snow = ps2_imm_freeze_m < 50.0f ? 1.0f : 0.0f;
}

/* the frame-time governor's strength share (ps2_video.c tmat_frame; the renderer thread writes, the GS thread reads) */
static volatile float tm_gov = 1.0f;
void ps2_tmat_gov_keep(float k) { tm_gov = k < 0.0f ? 0.0f : k > 1.0f ? 1.0f : k; }
int ps2_tmat_patch(unsigned char *recs, unsigned count, const float eye[3], int eye_ok, struct ps2_vk_mesh_block *bb) {
    ps2_vk_mesh_block *b = (ps2_vk_mesh_block *)bb;
    if (tm_on <= 0 || !recs || !b || !tm_ready) return 0;
    st_patches++;
    if (!eye_ok) { st_noeye++; return 0; }
    const float gk = tm_gov;
    if (gk <= 0.0f) return 0;                          /* (shed: the renderer skips the variant as well) */
    float d1u = tm_d1 * TM_UPM, dmin2 = 1e30f;
    for (u32 k = 0; k < count; k++) {
        float p[4];
        memcpy(p, recs + k * 64u, 16);
        float dx = p[0] - eye[0], dy = p[1] - eye[1], dz = p[2] - eye[2], d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < dmin2) dmin2 = d2;
    }
    /* (the nearest vertex is within the far distance, or the eye is over the patch's 2048-unit square) */
    if (dmin2 > (d1u + 1500.0f) * (d1u + 1500.0f)) return 0;
    tm_frame_values();
    int known = 0;
    for (u32 k = 0; k < count; k++) {
        float p[4], gx, gz, cav;
        memcpy(p, recs + k * 64u, 16);
        int ok = ps2_terrain_ground(p[0], p[2], &gx, &gz, &cav);
        known += ok;
        if (tm_dbg) {
            float sl = sqrtf(gx * gx + gz * gz);
            if (sl > st_slope_max) st_slope_max = sl;
            for (int q = 0; q < 4; q++) if (sl > 0.2f + 0.1f * (float)q) st_steep[q]++;
        }
        s32 q[4] = { (s32)lrintf(gx * 4096.0f), (s32)lrintf(gz * 4096.0f), (s32)lrintf(cav), ok };
        memcpy(recs + k * 64u + 16u, q, 16);
    }
    if (!known) st_noground++;
    float gs = exp2f(tm_bias);                     /* (the mip bias as a gradient scale) */
    b->l[0] = tm_sun[0]; b->l[1] = tm_sun[1]; b->l[2] = tm_sun[2]; b->l[3] = tm_strength * gk;
    b->l[4] = tm_d0 * TM_UPM; b->l[5] = 1.0f / ((tm_d1 - tm_d0) * TM_UPM); b->l[6] = tm_snow;
    b->l[7] = (float)((tm_tint & 15) + 16 * tm_cost);
    b->e[8] = eye[0]; b->e[9] = eye[1]; b->e[10] = eye[2]; b->e[11] = 1.0f;   /* (the shader water's place of the eye) */
    for (u32 k = 0; k < TM_L; k++) {
        float *o = k < 4u ? b->r + 4u * k : b->l + 8;
        o[0] = 0.0f; o[1] = tm_ks[k] * tm_mix[k]; o[2] = tm_ka[k] * tm_mix[k]; o[3] = gs;
    }
    b->pad4 |= PS2_TMAT_PAD4;
    st_marked++;
    {   static int bdbg = -1;
        if (bdbg < 0) { const char *e = getenv("ACZ_DEBUG_TERRAIN_MAT_BLOCK"); bdbg = e ? atoi(e) : 0; }
        if (bdbg > 0 && ps2_gs_frame_count >= (u32)bdbg) {
            bdbg = 0;
            s32 q[4];
            memcpy(q, recs + 16u, 16);
            ps2_log("terrain-mat block: l %.3f %.3f %.3f %.3f | %.1f %.6f %.0f %.0f | %.0f %.0f %.0f %.2f; r %g %g %g %g | %g %g %g %g "
                    "| %g %g %g %g | %g %g %g; rec0 q1 %d %d %d %d dmin %.0f", b->l[0], b->l[1], b->l[2], b->l[3], b->l[4],
                    b->l[5], b->l[6], b->l[7], b->l[8], b->l[9], b->l[10], b->l[11], b->r[0], b->r[1], b->r[2], b->r[3],
                    b->r[4], b->r[5], b->r[6], b->r[7], b->r[8], b->r[9], b->r[10], b->r[11], b->r[12], b->r[13], b->r[14],
                    q[0], q[1], q[2], q[3], sqrtf(dmin2));
        }
    }
    if (tm_dbg) {
        static u32 last = 0xFFFFFFFFu;
        if (last == 0xFFFFFFFFu || ps2_gs_frame_count - last >= 600u) {
            last = ps2_gs_frame_count;
            ps2_log("terrain-mat: GS frame %u: %llu of %llu patches with materials (%llu without ground, %llu without eye); "
                    "sun %.2f %.2f %.2f, snow %d, eye %.0f %.0f %.0f; steepest ground %.2f, vertices over 0.2/0.3/0.4/0.5: "
                    "%llu/%llu/%llu/%llu", ps2_gs_frame_count, (unsigned long long)st_marked,
                    (unsigned long long)st_patches, (unsigned long long)st_noground, (unsigned long long)st_noeye,
                    tm_sun[0], tm_sun[1], tm_sun[2], (int)tm_snow, eye[0], eye[1], eye[2], st_slope_max,
                    (unsigned long long)st_steep[0], (unsigned long long)st_steep[1], (unsigned long long)st_steep[2],
                    (unsigned long long)st_steep[3]);
        }
    }
    return 1;
}

void ps2_tmat_report(void) {
    if (tm_on > 0 && st_patches)
        ps2_log("terrain-mat: %llu terrain patches seen, %llu drawn with materials (%llu without the smooth ground, %llu "
                "without the eye)", (unsigned long long)st_patches, (unsigned long long)st_marked,
                (unsigned long long)st_noground, (unsigned long long)st_noeye);
}
