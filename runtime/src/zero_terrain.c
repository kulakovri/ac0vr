/* zero_terrain.c -- realistic terrain, stage T2 of TERRAIN_RESEARCH_2026-10-21.md (bug #25 follow-up): relief lighting
   baked from the game's own heightmap. Shading only: the surface the game draws and collides with is unchanged.

   The game's data (SLUS-21346; docs/terrain/code_findings_2026-10-21.md section 3, read again from the ground query
   0x158AE0): the terrain manager 0x15E1D8 runs once per frame in flight with a0 = manager, terrain object T = *(a0+4).
   The map is 513 x 513 height samples 1024 units apart from (-262144, -262144), height = u8 x 80 units, stored as
   16 x 16 blocks of 33 x 33 bytes (the shared edge rows repeated): block (bx, bz) -> index u8 [*(T+0xB8) + bz*16 + bx]
   -> bytes at *(T+0xBC) + index * 0x441, sample (lx, lz) at lz * 33 + lx (rows by z). Tiles of 2 x 2 samples: record
   u8 [*(T+0x14) + block] -> u16 ids at *(T+0x18) + record * 512 + ((lz >> 1) * 16 + (lx >> 1)) * 2; flag 0x800 =
   water (the ground query returns height 0 there), 0x400 = a coarse-only tile. Maps are built from a small pool of
   height blocks, repeated (mission 1's mountains every ~11 km, mission 2 a plain with one repeated hill block). The
   terrain's vertex colours are a constant 90/90/90/128 (missions 2 and 3, ACZ_DEBUG_TERRAIN_COL): the 2006 terrain has
   no geometric light at all, its shading is painted into the atlas.

   What happens: every frame (while ACZ_TERRAIN_REAL / F4 "Realistic terrain" is on) a signature of the map (the
   pointers, both block tables, a sparse sample of the heights) is compared with the last one; a new map that stays
   the same for 30 frames is copied (513 x 513 bytes + the water flags) and baked on worker threads at the lowest
   thread priority, with the mission's sun (C1 sun_dir, zero_clouds.c: the stage environment's fixed sun angles; baked
   again if it moves by more than 3 degrees, as at the end of a mission intro). The bake writes one light ratio per
   texel of an N x N grid over the map (N = ACZ_TERRAIN_RES, default 2048: 256 units = ~43 m per texel), 128 = 1.0,
   with its mip chain (2 x 2 box filter, so a distant pixel gets the mean ratio of its footprint: detail fades to
   neutral, shadows stay as their average). 0.2-0.5 s on 4 threads, once per map.

     ratio = R_relief x R_erosion x R_shadow x R_cavity, each blended from 1 by its strength (the directional ones
             also by the sun's elevation), then limited softly (tanh in log space) to [ACZ_TERRAIN_CLAMP lo, hi]
             (default 0.62 .. 1.38)

   with the sun light I(N, v) = A + (1 - A) v max(N.L, 0) / max(L.y, 0.25) (A = 0.4 sky share), normalised to flat
   sunlit ground (= 1: plains and sea keep exactly the game's look):
   - R_relief = I(Nb, 1) / I(up, 1): Nb = the normal of a smooth (Catmull-Rom) surface through the game's samples after
     a 1.2-sample blur (the u8 heights are 80-unit terraces: lit as they are they draw contour stripes), slopes x 1.5
     (shading only), so slopes towards the sun are lighter, the others darker (ACZ_TERRAIN_RELIEF);
   - R_erosion = I(Ne, 1) / I(Nb, 1) x (gully floors a little darker): Ne = Nb tilted by a gully field that follows
     the slope (ACZ_TERRAIN_EROSION). Own filter in the lineage of Clay John's "eroded terrain noise" (Shadertoy MtGcWh,
     2018) and Rune Skovbo Johansen's "fast and gorgeous erosion filter" (blog.runevision.com, 2026, MPL-2.0 code; no
     code taken over): per octave, jittered cell points each carry a cosine stripe pattern running down the local slope
     (base slope + the gullies so far, so smaller gullies branch off larger ones), weighted by a smooth window; strength
     fades in with the slope (none below 0.05); periods 4096 / 2048 / 1024 units (+512 at N = 4096), >= 4 texels;
   - R_shadow = I(Ne, vis) / I(Ne, 1): vis = the terrain's own shadow for the sun, a horizon march over the heightmap
     on a 1024 x 1024 grid with a 2.5 degree soft edge (ACZ_TERRAIN_SHADOW). With the 45-60 degree suns of most missions
     there are almost no cast shadows (the game's slopes stay under ~25 degrees); they matter for low suns;
   - R_cavity = 1 - 0.10 clamp(c / 600 units): c = blurred minus actual height at two scales (sigma 1.5 and 5
     samples): valleys a little darker, ridges a little lighter (ACZ_TERRAIN_CAVITY).
   Half of the land's mean shift is given back per map (x mean^-0.5, 0.97..1.04): mountain maps darken on average
   because lit slopes saturate (mission 1: mean 0.942 -> ~0.97; flat ground there +3 %). Low suns: the directional
   terms x (0.35 + 0.65 s), the shadow x (0.3 + 0.7 s), s = smoothstep(2, 20 degrees of elevation) -- mission 5's
   4 degree sun otherwise darkened the whole map (mean ratio 0.86 -> 1.03 now); nothing directional below 1 degree
   (night). Water samples (tile flag 0x800) keep only the shadow term, half strength.

   The renderer (ps2_video.c relief_*) uploads a finished bake into its relief texture at the start of a frame and
   applies it only while the uploaded generation belongs to the map that is loaded (ps2_terrain_relief_valid_from), so
   another mission's shading is never drawn over a new map. mesh.vert reads it per vertex, prefiltered to the vertex
   spacing (mip 3 = 2048 units, mip 4 from the geomorph distance), into the vertex colour of the terrain's first pass
   (prog 1 pass 0, which modulates the atlas texel), before the fog: no fragment cost (in-run A/B within +-0.01 ms).
   ACZ_TERRAIN_PIXEL=1 fetches it per pixel in mesh.frag instead (the full 43 m detail, +0.13..0.15 ms per stereo frame
   in mission 3's low pass at 4x). The VU1 fallback path and program 3A6610 (GS path) stay unchanged.

   Switches: ACZ_TERRAIN_REAL (master; F4 > Rendering > Realistic terrain, ini [graphics] realistic_terrain),
   ACZ_TERRAIN_RELIEF (0.8) / _EROSION (1) / _SHADOW (0.7) / _CAVITY (1) (strengths, 0 = the term off),
   ACZ_TERRAIN_CLAMP=lo,hi, ACZ_TERRAIN_RES (512..4096), ACZ_TERRAIN_THREADS (bake threads, default 4). Debug:
   ACZ_DEBUG_TERRAIN=1 (log), ACZ_DEBUG_TERRAIN_DUMP=dir (PGM images of the heights, water, terms and ratio per bake),
   ACZ_DEBUG_TERRAIN_VIEW=k (bake only term k: 1 relief, 2 erosion, 3 shadow, 4 cavity; 5 = a test pattern: 8192-unit
   checks, +x half lighter), ACZ_DEBUG_TERRAIN_COL=f (rn_vp_terrain.c: the vertices of GS frame f), ACZ_TERRAIN_AB=N
   (ps2_video.c: in-run GPU A/B), PS2_PERF_AB_FLIP=terrain (exact image A/B of one frame). */
#include "ps2_runtime.h"
#include "ps2_hook.h"
#include "ps2_terrain.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#if defined(__has_include)
#if __has_include("ps2_clouds.h")
#include "ps2_clouds.h"
#define ZT_C1 1
#endif
#endif
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

u64 ps2_kernel_vblank_count(void);

#define ZT_MGR 0x0015E1D8u                 /* terrain manager: a0 = manager, T = *(a0 + 4) */
#define ZT_S 513                           /* height samples per side */
#define ZT_SH 1024                         /* shadow grid per side (512 units) */
#define ZT_PI 3.14159265f
#define ZT_SKY 0.4f                        /* sky share of the light on flat sunlit ground */
#define ZT_ZREL 1.5f                       /* relief normals: slopes x 1.5 (shading only; reads on gentle ground) */

static int zt_on;                          /* the feature exists this session (ACZ_TERRAIN_REAL != 0 at start) */
static int zt_dbg, zt_view, zt_threads = 4;
static u32 zt_res = 2048, zt_levels;
static float zt_s_relief = 0.8f, zt_s_ero = 1.0f, zt_s_shadow = 0.7f, zt_s_cav = 1.0f, zt_lo = 0.62f, zt_hi = 1.38f;
static char zt_dump[260];

/* shared state: map signature / bake generations (EE thread <-> bake thread <-> renderer) */
static pthread_mutex_t zt_mx = PTHREAD_MUTEX_INITIALIZER;
static u32 zt_map_sig;                     /* the map loaded now (0 = none seen) */
static u32 zt_stable;                      /* frames the map signature has stayed the same */
static u32 zt_baked_sig;                   /* the map of the newest published bake */
static float zt_baked_sun[3];
static volatile u32 zt_valid_from = 0xFFFFFFFFu;   /* the first generation that belongs to the loaded map */
static u32 zt_gen;                         /* last generation handed out */
static int zt_busy;                        /* a bake is running */
static u8 *zt_pub;                         /* the published bake (all levels) */
static size_t zt_pub_bytes;
static u32 zt_pub_gen;

static void zt_cfg(void);
uint32_t ps2_terrain_relief_size(void) { zt_cfg(); return zt_on ? zt_res : 0u; }
uint32_t ps2_terrain_relief_valid_from(void) { return zt_valid_from; }

/* the smooth ground for the near-ground materials (zero_terrain_mat.c, stage T3+): per height sample the slope of the
   shading surface (central differences of the 1.2-sample blur) and the cavity, published by each bake before its texels
   are computed; older grids are kept (a reader may still hold one for the length of a patch) and freed three bakes
   later */
typedef struct { u32 sig; float g[513 * 513 * 3]; float h[513 * 513]; } zt_ground;   /* (h: ps2_terrain_height) */
static zt_ground *volatile zt_gpub;
static zt_ground *zt_gold[3];
/* the mirrors' grid (ps2_terrain_mirror_grid): published by the bake, older ones freed three bakes later */
typedef struct { u32 sig, gen, nt; u8 *tex; u16 tgrid[65536]; float v[PS2_TERRAIN_MIR_SIDE * PS2_TERRAIN_MIR_SIDE * 4]; } zt_mgrid;
static zt_mgrid *volatile zt_mpub;
static zt_mgrid *zt_mold[3];
static int zt_mir = -1;                    /* ACZ_VR_COCKPIT_MIRRORS_TERRAIN (copy the atlas for it) */
static float zt_tmean[1024][3];            /* the last copied map's tile means (0..255) and its signature */
static u32 zt_tmean_sig, zt_tmean_nt;
/* (phase 4, ACZ_VR_COCKPIT_MIRRORS_TERRAIN_TEX) the last copied map's tiles for the mirrors: 32 x 32 BC1 with mips,
   mip-major (zt_tt_build) */
static int zt_mtex = -1;
static u8 *zt_ttex;
static const u32 zt_tt_off[7] = { 0, 512, 640, 672, 680, 688, 696 };   /* a tile's bytes before each level */
static u16 zt_bc1_565(const float c[3]) {
    long r = lrintf(c[0] * 31.0f / 255.0f), g = lrintf(c[1] * 63.0f / 255.0f), b = lrintf(c[2] * 31.0f / 255.0f);
    r = r < 0 ? 0 : r > 31 ? 31 : r; g = g < 0 ? 0 : g > 63 ? 63 : g; b = b < 0 ? 0 : b > 31 ? 31 : b;
    return (u16)((r << 11) | (g << 5) | b);
}
/* one 4 x 4 block (16 colours 0..255) to BC1: the endpoints the block's colour box inset by 1/16 of it, each texel the
   nearest of the four colours */
static void zt_bc1_block(float px[16][3], u8 *o) {
    float lo[3] = { 255, 255, 255 }, hi[3] = { 0, 0, 0 };
    for (int k = 0; k < 16; k++) for (int c = 0; c < 3; c++) { if (px[k][c] < lo[c]) lo[c] = px[k][c]; if (px[k][c] > hi[c]) hi[c] = px[k][c]; }
    for (int c = 0; c < 3; c++) { float in = (hi[c] - lo[c]) / 16.0f; lo[c] += in; hi[c] -= in; }
    u16 c0 = zt_bc1_565(hi), c1 = zt_bc1_565(lo);
    u32 idx = 0;
    if (c0 < c1) { u16 t = c0; c0 = c1; c1 = t; }
    if (c0 != c1) {
        float p[4][3];
        for (int e = 0; e < 2; e++) {
            u16 v = e ? c1 : c0;
            p[e][0] = (float)((v >> 11) & 31) * 255.0f / 31.0f; p[e][1] = (float)((v >> 5) & 63) * 255.0f / 63.0f;
            p[e][2] = (float)(v & 31) * 255.0f / 31.0f;
        }
        for (int c = 0; c < 3; c++) { p[2][c] = (2.0f * p[0][c] + p[1][c]) / 3.0f; p[3][c] = (p[0][c] + 2.0f * p[1][c]) / 3.0f; }
        for (int k = 0; k < 16; k++) {
            int best = 0; float bd = 1e30f;
            for (int q = 0; q < 4; q++) {
                float d = 0.0f;
                for (int c = 0; c < 3; c++) { float t = px[k][c] - p[q][c]; d += t * t; }
                if (d < bd) { bd = d; best = q; }
            }
            idx |= (u32)best << (2 * k);
        }
    }
    o[0] = (u8)c0; o[1] = (u8)(c0 >> 8); o[2] = (u8)c1; o[3] = (u8)(c1 >> 8);
    o[4] = (u8)idx; o[5] = (u8)(idx >> 8); o[6] = (u8)(idx >> 16); o[7] = (u8)(idx >> 24);
}
/* the tiles (nt, the atlas's 64 x 64 PSMT8 texels through pal) at 32 x 32 (2 x 2 boxes) and their mips, BC1, mip-major
   into zt_ttex */
static void zt_tt_build(const u8 *atlas, const u8 (*pal)[3], u32 nt) {
    if (!zt_ttex) zt_ttex = (u8 *)malloc((size_t)1024u * 696u);
    if (!zt_ttex) return;
    static float lv[32 * 32][3], nx[16 * 16][3];
    for (u32 t = 0; t < nt; t++) {
        const u8 *a = atlas + (size_t)t * 4096u;
        for (int y = 0; y < 32; y++)
            for (int x = 0; x < 32; x++)
                for (int c = 0; c < 3; c++)
                    lv[y * 32 + x][c] = 0.25f * ((float)pal[a[(2 * y) * 64 + 2 * x]][c] + pal[a[(2 * y) * 64 + 2 * x + 1]][c]
                                                 + pal[a[(2 * y + 1) * 64 + 2 * x]][c] + pal[a[(2 * y + 1) * 64 + 2 * x + 1]][c]);
        for (int L = 0, n = 32; L < 6; L++, n >>= 1) {
            u8 *o = zt_ttex + (size_t)nt * zt_tt_off[L] + (size_t)t * (zt_tt_off[L + 1] - zt_tt_off[L]);
            int nb = n >= 4 ? n / 4 : 1;
            for (int by = 0; by < nb; by++)
                for (int bx = 0; bx < nb; bx++) {
                    float px[16][3];
                    for (int k = 0; k < 16; k++) {
                        int x = bx * 4 + (k & 3), y = by * 4 + (k >> 2);
                        if (x >= n) x = n - 1;
                        if (y >= n) y = n - 1;
                        for (int c = 0; c < 3; c++) px[k][c] = lv[y * n + x][c];
                    }
                    zt_bc1_block(px, o + (size_t)(by * nb + bx) * 8u);
                }
            if (n > 1) {                                   /* the next level: 2 x 2 boxes */
                int m = n / 2;
                for (int y = 0; y < m; y++)
                    for (int x = 0; x < m; x++)
                        for (int c = 0; c < 3; c++)
                            nx[y * m + x][c] = 0.25f * (lv[(2 * y) * n + 2 * x][c] + lv[(2 * y) * n + 2 * x + 1][c]
                                                        + lv[(2 * y + 1) * n + 2 * x][c] + lv[(2 * y + 1) * n + 2 * x + 1][c]);
                memcpy(lv, nx, (size_t)m * m * sizeof lv[0]);
            }
        }
    }
}
uint32_t ps2_terrain_mirror_tiles(const uint16_t **grid, const uint8_t **bc1, uint32_t *nt) {
    const zt_mgrid *G = __atomic_load_n(&zt_mpub, __ATOMIC_ACQUIRE);
    *grid = NULL; *bc1 = NULL; *nt = 0u;
    if (!G || G->sig != zt_map_sig || !G->tex) return 0u;
    *grid = G->tgrid; *bc1 = G->tex; *nt = G->nt;
    return G->gen;
}
uint32_t ps2_terrain_mirror_grid(const float **v) {
    const zt_mgrid *G = __atomic_load_n(&zt_mpub, __ATOMIC_ACQUIRE);
    *v = NULL;
    if (!G || G->sig != zt_map_sig) return 0u;
    *v = G->v;
    return G->gen;
}
/* the ground's height (units) at world (x, z) of the loaded map, bilinear between the game's height samples (the
   mirrors' live map, ps2_video.c mlv_*: an object near the ground is a ground object); 0 = none yet */
int ps2_terrain_height(float x, float z, float *h) {
    const zt_ground *G = __atomic_load_n(&zt_gpub, __ATOMIC_ACQUIRE);
    *h = 0.0f;
    if (!G || G->sig != zt_map_sig) return 0;
    float s = (x + 262144.0f) * (1.0f / 1024.0f), t = (z + 262144.0f) * (1.0f / 1024.0f);
    if (!(s >= 0.0f && t >= 0.0f && s <= 512.0f && t <= 512.0f)) return 0;
    int ix = (int)s, iz = (int)t;
    if (ix > 511) ix = 511;
    if (iz > 511) iz = 511;
    float fx = s - (float)ix, fz = t - (float)iz;
    const float *a = G->h + (size_t)iz * 513u + (size_t)ix;
    *h = (a[0] * (1.0f - fx) + a[1] * fx) * (1.0f - fz) + (a[513] * (1.0f - fx) + a[514] * fx) * fz;
    return 1;
}
int ps2_terrain_ground(float x, float z, float *gx, float *gz, float *cav) {
    const zt_ground *G = __atomic_load_n(&zt_gpub, __ATOMIC_ACQUIRE);
    *gx = *gz = *cav = 0.0f;
    if (!G || G->sig != zt_map_sig) return 0;
    float s = (x + 262144.0f) * (1.0f / 1024.0f), t = (z + 262144.0f) * (1.0f / 1024.0f);
    if (!(s >= 0.0f && t >= 0.0f && s <= 512.0f && t <= 512.0f)) return 0;
    int ix = (int)s, iz = (int)t;
    if (ix > 511) ix = 511;
    if (iz > 511) iz = 511;
    float fx = s - (float)ix, fz = t - (float)iz;
    float w[4] = { (1.0f - fx) * (1.0f - fz), fx * (1.0f - fz), (1.0f - fx) * fz, fx * fz };
    const float *a = G->g + ((size_t)iz * 513u + (size_t)ix) * 3u;
    const float *q[4] = { a, a + 3, a + 513 * 3, a + 513 * 3 + 3 };
    for (int k = 0; k < 4; k++) { *gx += w[k] * q[k][0]; *gz += w[k] * q[k][1]; *cav += w[k] * q[k][2]; }
    return 1;
}

uint32_t ps2_terrain_relief_copy(void *dst, size_t cap, uint32_t have_gen, uint32_t *n, uint32_t *levels) {
    uint32_t g = 0;
    pthread_mutex_lock(&zt_mx);
    if (zt_pub && zt_pub_gen != have_gen && zt_pub_bytes <= cap) {
        memcpy(dst, zt_pub, zt_pub_bytes);
        g = zt_pub_gen; *n = zt_res; *levels = zt_levels;
    }
    pthread_mutex_unlock(&zt_mx);
    return g;
}

/* ---- the bake ------------------------------------------------------------------------------------------------- */
typedef struct {
    float h[ZT_S * ZT_S];                  /* heights, units */
    float hs[ZT_S * ZT_S];                 /* the shading surface: heights blurred by 1.2 samples (no 80-unit terraces) */
    u8 water[ZT_S * ZT_S];                 /* 1 = a sample of a water tile */
    float cav[ZT_S * ZT_S];                /* blurred - actual height, units */
    float shadow[ZT_SH * ZT_SH];           /* terrain sun visibility 0..1 */
    float sun[3], lref, imean, hmax, iflat, kdir, kshadow, kmean;
    u32 sig, gen, n;
    int sun_src;                           /* 1 = C1, 0 = the default sun */
    u8 *out;                               /* N x N + mips */
    u8 *dbg[4];                            /* ACZ_DEBUG_TERRAIN_DUMP: the four terms at level 0 */
    volatile long next;                    /* parallel row counter */
    void (*row)(void *job, int r);
    int rows;
    /* the rear-view mirrors' terrain (ACZ_VR_COCKPIT_MIRRORS_TERRAIN; ps2_terrain_mirror_grid): the tile grid (id + flags
       per 2 x 2 samples, rows by z), the atlas (nt tiles of 64 x 64 PSMT8 texels) and its palette, copied with a new map;
       NULL atlas = a re-bake of the same map (the tile means of the last copy serve) */
    u16 tgrid[256 * 256];
    u8 *atlas;
    u8 pal[256][3];
    u32 nt;
} zt_job;

static float zt_clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static float zt_smooth(float a, float b, float x) { float t = zt_clampf((x - a) / (b - a), 0.0f, 1.0f); return t * t * (3.0f - 2.0f * t); }
static float zt_H(const zt_job *j, int x, int z) {         /* the shading surface */
    x = x < 0 ? 0 : x > ZT_S - 1 ? ZT_S - 1 : x;
    z = z < 0 ? 0 : z > ZT_S - 1 ? ZT_S - 1 : z;
    return j->hs[z * ZT_S + x];
}
/* bilinear in sample coordinates (0..512) */
static float zt_bil(const float *g, int side, float s, float t) {
    float m = (float)(side - 1);
    s = zt_clampf(s, 0.0f, m); t = zt_clampf(t, 0.0f, m);
    int ix = (int)s, iz = (int)t;
    if (ix > side - 2) ix = side - 2;
    if (iz > side - 2) iz = side - 2;
    float fx = s - (float)ix, fz = t - (float)iz;
    const float *p = g + iz * side + ix;
    return (p[0] * (1.0f - fx) + p[1] * fx) * (1.0f - fz) + (p[side] * (1.0f - fx) + p[side + 1] * fx) * fz;
}
/* Catmull-Rom surface through the samples: height and its derivatives per sample step */
static void zt_crw(float f, float w[4], float d[4]) {
    float f2 = f * f, f3 = f2 * f;
    w[0] = 0.5f * (-f3 + 2.0f * f2 - f);        d[0] = 0.5f * (-3.0f * f2 + 4.0f * f - 1.0f);
    w[1] = 0.5f * (3.0f * f3 - 5.0f * f2 + 2.0f); d[1] = 0.5f * (9.0f * f2 - 10.0f * f);
    w[2] = 0.5f * (-3.0f * f3 + 4.0f * f2 + f);  d[2] = 0.5f * (-9.0f * f2 + 8.0f * f + 1.0f);
    w[3] = 0.5f * (f3 - f2);                     d[3] = 0.5f * (3.0f * f2 - 2.0f * f);
}
static void zt_cr(const zt_job *j, float s, float t, float *v, float *ds, float *dt) {
    int ix = (int)floorf(s), iz = (int)floorf(t);
    float wx[4], dx[4], wz[4], dz[4], a = 0.0f, ax = 0.0f, az = 0.0f;
    zt_crw(s - (float)ix, wx, dx);
    zt_crw(t - (float)iz, wz, dz);
    for (int r = 0; r < 4; r++) {
        float rv = 0.0f, rd = 0.0f;
        for (int c = 0; c < 4; c++) { float hv = zt_H(j, ix - 1 + c, iz - 1 + r); rv += wx[c] * hv; rd += dx[c] * hv; }
        a += wz[r] * rv; ax += wz[r] * rd; az += dz[r] * rv;
    }
    *v = a; *ds = ax; *dt = az;
}
static u32 zt_hash(int x, int z) {
    u32 h = (u32)x * 0x8DA6B343u ^ (u32)z * 0xD8163841u ^ 0x5BD1E995u;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return h;
}
/* sin and cos of x, |x| < ~20: reduction to [-pi, pi] and odd/even polynomials (error < 2e-4) */
static void zt_sincos(float x, float *s, float *c) {
    float k = floorf(x * (1.0f / (2.0f * ZT_PI)) + 0.5f);
    x -= k * 2.0f * ZT_PI;
    float y = x;                                       /* sin over [-pi, pi] via [-pi/2, pi/2] */
    if (y > 0.5f * ZT_PI) y = ZT_PI - y; else if (y < -0.5f * ZT_PI) y = -ZT_PI - y;
    float y2 = y * y;
    *s = y * (1.0f + y2 * (-0.16666667f + y2 * (0.0083333310f + y2 * (-0.00019840874f + y2 * 2.7525562e-6f))));
    float z = 0.5f * ZT_PI - fabsf(x), z2 = z * z;   /* cos(x) = sin(pi/2 - |x|), |x| <= pi */
    *c = z * (1.0f + z2 * (-0.16666667f + z2 * (0.0083333310f + z2 * (-0.00019840874f + z2 * 2.7525562e-6f))));
}
/* the gully field at world (px, pz) on a base slope (gx, gz) = dh/dx, dh/dz: height delta (units) and its slope */
static int zt_oct = 3;
static void zt_gully(float px, float pz, float gx, float gz, float *oh, float *ogx, float *ogz) {
    float th = 0.0f, tgx = 0.0f, tgz = 0.0f, fgx = gx, fgz = gz, per = 4096.0f, amp = 150.0f;
    const float R2 = 1.25f * 1.25f;
    float slope0 = sqrtf(gx * gx + gz * gz), fade = zt_smooth(0.05f, 0.30f, slope0);
    if (fade <= 0.0f) { *oh = *ogx = *ogz = 0.0f; return; }
    for (int o = 0; o < zt_oct; o++) {
        float sl = sqrtf(fgx * fgx + fgz * fgz), dx = 1.0f, dz = 0.0f;
        if (sl > 1e-5f) { dx = fgx / sl; dz = fgz / sl; }
        float cx = -dz, cz = dx;                         /* across the slope: the stripes run downhill */
        float qx = px / per, qz = pz / per;
        int ix = (int)floorf(qx), iz = (int)floorf(qz);
        float fx = qx - (float)ix, fz = qz - (float)iz, ah = 0.0f, ad = 0.0f, ws = 0.0f;
        for (int b = -1; b <= 1; b++)
            for (int a = -1; a <= 1; a++) {
                u32 hs = zt_hash(ix + a + o * 7919, iz + b - o * 104729);
                float ox = (float)a + 0.2f + 0.6f * (float)(hs & 0xFFFFu) * (1.0f / 65535.0f);
                float oz = (float)b + 0.2f + 0.6f * (float)(hs >> 16) * (1.0f / 65535.0f);
                float vx = fx - ox, vz = fz - oz, d2 = vx * vx + vz * vz;
                if (d2 >= R2) continue;
                float w = 1.0f - d2 / R2;
                w = w * w * w;
                float sn, cs;
                zt_sincos((vx * cx + vz * cz) * 2.0f * ZT_PI, &sn, &cs);
                ah += w * cs; ad -= w * sn; ws += w;
            }
        if (ws > 1e-6f) { ah /= ws; ad /= ws; }
        float a = amp * fade, g = a * ad * 2.0f * ZT_PI / per;
        th += a * ah;
        tgx += g * cx; tgz += g * cz;
        fgx = gx + tgx; fgz = gz + tgz;
        amp *= 0.5f; per *= 0.5f;
    }
    *oh = th; *ogx = tgx; *ogz = tgz;
}

static void *zt_worker(void *u) {
    zt_job *j = (zt_job *)u;
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    { extern void ps2_host_thread_start(const char *); ps2_host_thread_start("bg"); }  /* (PS2_THREAD_CORES=h) */
#endif
    for (;;) {
        long r = __sync_fetch_and_add(&j->next, 1);
        if (r >= j->rows) break;
        j->row(j, (int)r);
    }
    return NULL;
}
static void zt_par(zt_job *j, int rows, void (*fn)(void *, int)) {
    pthread_t th[16];
    int n = zt_threads, made = 0;
    j->next = 0; j->rows = rows; j->row = fn;
    for (int i = 1; i < n; i++) if (pthread_create(&th[made], NULL, zt_worker, j) == 0) made++;
    zt_worker(j);
    for (int i = 0; i < made; i++) pthread_join(th[i], NULL);
}

static void zt_shadow_row(void *u, int row) {
    zt_job *j = (zt_job *)u;
    const float *L = j->sun;
    float lh = sqrtf(L[0] * L[0] + L[2] * L[2]);
    float *o = j->shadow + (size_t)row * ZT_SH;
    if (lh < 1e-4f || L[1] <= 0.0f) { for (int c = 0; c < ZT_SH; c++) o[c] = L[1] > 0.0f ? 1.0f : 0.0f; return; }
    const float soft = 2.5f * ZT_PI / 180.0f, elev = atan2f(L[1], lh);
    const float tlo = tanf(elev - soft > 0.0f ? elev - soft : 0.0f), thi = tanf(elev + soft);
    float dirx = L[0] / lh, dirz = L[2] / lh;          /* towards the sun, per sample step */
    for (int c = 0; c < ZT_SH; c++) {
        float s = ((float)c + 0.5f) * (512.0f / ZT_SH), t = ((float)row + 0.5f) * (512.0f / ZT_SH);
        float h0 = zt_bil(j->h, ZT_S, s, t) + 30.0f, best = -1e9f, d = 0.5f;
        for (;;) {
            float sx = s + dirx * d, tz = t + dirz * d;
            if (sx < 0.0f || tz < 0.0f || sx > 512.0f || tz > 512.0f) break;
            float tn = (zt_bil(j->h, ZT_S, sx, tz) - h0) / (d * 1024.0f);
            if (tn > best) { best = tn; if (best >= thi) break; }
            if ((j->hmax - h0) / (d * 1024.0f) < tlo) break;   /* nothing further can reach the sun's band */
            d += d * 0.04f > 0.5f ? d * 0.04f : 0.5f;
        }
        float ha = atanf(best);
        o[c] = zt_clampf(0.5f + (elev - ha) / (2.0f * soft), 0.0f, 1.0f);
    }
}

static void zt_blur(const float *in, float *out, float sigma);
static float zt_light(const zt_job *j, float nx, float ny, float nz, float vis) {
    float d = nx * j->sun[0] + ny * j->sun[1] + nz * j->sun[2];
    return ZT_SKY + (1.0f - ZT_SKY) * vis * (d > 0.0f ? d : 0.0f) / j->lref;
}
static void zt_main_row(void *u, int row) {
    zt_job *j = (zt_job *)u;
    u32 N = j->n;
    float st = 512.0f / (float)N, t = ((float)row + 0.5f) * st;
    u8 *o = j->out + (size_t)row * N;
    for (u32 c = 0; c < N; c++) {
        float s = ((float)c + 0.5f) * st, hv, ds, dt, eh = 0.0f, egx = 0.0f, egz = 0.0f;
        zt_cr(j, s, t, &hv, &ds, &dt);
        float gx = ds * (1.0f / 1024.0f), gz = dt * (1.0f / 1024.0f);
        float px = s * 1024.0f - 262144.0f, pz = t * 1024.0f - 262144.0f;
        if (zt_s_ero > 0.0f || zt_view == 2) zt_gully(px, pz, gx, gz, &eh, &egx, &egz);
        float rx = gx * ZT_ZREL, rz = gz * ZT_ZREL, nb = 1.0f / sqrtf(rx * rx + 1.0f + rz * rz);
        float ex = rx + egx, ez = rz + egz, ne = 1.0f / sqrtf(ex * ex + 1.0f + ez * ez);
        float ib = zt_light(j, -rx * nb, nb, -rz * nb, 1.0f);
        float ie = zt_light(j, -ex * ne, ne, -ez * ne, 1.0f);
        float vis = zt_bil(j->shadow, ZT_SH, s * (ZT_SH / 512.0f) - 0.5f, t * (ZT_SH / 512.0f) - 0.5f);
        float ies = zt_light(j, -ex * ne, ne, -ez * ne, vis);
        float cav = zt_bil(j->cav, ZT_S, s, t), wat;
        {   /* water share: bilinear over the 0/1 flags */
            int ix = (int)s, iz = (int)t;
            if (ix > ZT_S - 2) ix = ZT_S - 2;
            if (iz > ZT_S - 2) iz = ZT_S - 2;
            float fx = s - (float)ix, fz = t - (float)iz;
            const u8 *w = j->water + iz * ZT_S + ix;
            wat = ((float)w[0] * (1.0f - fx) + (float)w[1] * fx) * (1.0f - fz) + ((float)w[ZT_S] * (1.0f - fx) + (float)w[ZT_S + 1] * fx) * fz;
        }
        float r_rel = ib / j->iflat;
        float r_ero = (ie / ib) * (1.0f + 0.10f * zt_clampf(eh / 150.0f, -1.0f, 1.0f));
        float r_sh = ies / ie;
        float r_cav = 1.0f - 0.10f * zt_clampf(cav / 600.0f, -1.0f, 1.0f);
        float rr = 1.0f + j->kdir * zt_s_relief * (r_rel - 1.0f), re = 1.0f + j->kdir * zt_s_ero * (r_ero - 1.0f);
        float rs = 1.0f + j->kshadow * zt_s_shadow * (r_sh - 1.0f), rc = 1.0f + zt_s_cav * (r_cav - 1.0f);
        float land = rr * re * rs * rc, sea = 1.0f + 0.5f * j->kshadow * zt_s_shadow * (r_sh - 1.0f);
        float r = land + (sea - land) * wat;
        if (zt_view == 1) r = r_rel; else if (zt_view == 2) r = r_ero; else if (zt_view == 3) r = r_sh;
        else if (zt_view == 4) r = r_cav;
        else if (zt_view == 5) r = ((((int)floorf(px / 8192.0f) + (int)floorf(pz / 8192.0f)) & 1) ? 0.8f : 1.0f) * (px > 0.0f ? 1.25f : 1.0f);
        if (zt_view == 0) {                      /* soft limit in log space: lo .. hi, no flat clamped plateaus */
            float lr = logf(r > 1e-3f ? r : 1e-3f), lim = lr > 0.0f ? logf(zt_hi) : -logf(zt_lo);
            r = expf(lim * tanhf(lr / lim));
        }
        long q = lrintf(r * 128.0f);
        o[c] = (u8)(q < 0 ? 0 : q > 255 ? 255 : q);
        if (j->dbg[0]) {
            float tv[4] = { r_rel, r_ero, r_sh, r_cav };
            for (int k = 0; k < 4; k++) { long b = lrintf(zt_clampf(tv[k], 0.0f, 1.99f) * 128.0f); j->dbg[k][(size_t)row * N + c] = (u8)b; }
        }
    }
}

static void zt_blur(const float *in, float *out, float sigma) {     /* separable Gaussian, clamped edges */
    int r = (int)ceilf(sigma * 3.0f);
    float w[64], ws = 0.0f;
    if (r > 31) r = 31;
    for (int k = -r; k <= r; k++) { w[k + r] = expf(-0.5f * (float)(k * k) / (sigma * sigma)); ws += w[k + r]; }
    for (int k = 0; k <= 2 * r; k++) w[k] /= ws;
    float *tmp = (float *)malloc(sizeof(float) * ZT_S * ZT_S);
    if (!tmp) { memcpy(out, in, sizeof(float) * ZT_S * ZT_S); return; }
    for (int z = 0; z < ZT_S; z++)
        for (int x = 0; x < ZT_S; x++) {
            float a = 0.0f;
            for (int k = -r; k <= r; k++) { int xx = x + k; xx = xx < 0 ? 0 : xx > ZT_S - 1 ? ZT_S - 1 : xx; a += w[k + r] * in[z * ZT_S + xx]; }
            tmp[z * ZT_S + x] = a;
        }
    for (int z = 0; z < ZT_S; z++)
        for (int x = 0; x < ZT_S; x++) {
            float a = 0.0f;
            for (int k = -r; k <= r; k++) { int zz = z + k; zz = zz < 0 ? 0 : zz > ZT_S - 1 ? ZT_S - 1 : zz; a += w[k + r] * tmp[zz * ZT_S + x]; }
            out[z * ZT_S + x] = a;
        }
    free(tmp);
}

static void zt_pgm(const char *name, u32 sig, const u8 *px, u32 w, u32 h) {
    char path[400];
    snprintf(path, sizeof path, "%s/zt_%08X_%s.pgm", zt_dump, sig, name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P5\n%u %u\n255\n", w, h);
    fwrite(px, 1, (size_t)w * h, f);
    fclose(f);
}

static double zt_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void *zt_bake(void *u) {
    zt_job *j = (zt_job *)u;
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    { extern void ps2_host_thread_start(const char *); ps2_host_thread_start("bg"); }  /* (PS2_THREAD_CORES=h) */
#endif
    double t0 = zt_now_ms();
    u32 N = j->n;
    size_t bytes = 0;
    for (u32 s = N; ; s >>= 1) { bytes += (size_t)s * s; if (s == 1u) break; }
    j->out = (u8 *)malloc(bytes);
    if (zt_dump[0]) for (int k = 0; k < 4; k++) j->dbg[k] = (u8 *)malloc((size_t)N * N);
    if (!j->out) goto fail;
    /* sun: the light on flat ground is the reference (low suns clamped so slopes do not blow up) */
    j->lref = j->sun[1] > 0.25f ? j->sun[1] : 0.25f;
    {   /* flat sunlit ground is the reference (ratio 1: plains and sea keep the game's look); low suns soften the
           directional terms (their long shadows would darken whole maps: mission 5's 4 degree sun), none at night */
        float el = asinf(zt_clampf(j->sun[1], -1.0f, 1.0f)) * 57.29578f, fs = zt_smooth(2.0f, 20.0f, el);
        j->iflat = zt_light(j, 0.0f, 1.0f, 0.0f, 1.0f);
        if (!(j->iflat > 0.05f)) j->iflat = 1.0f;
        j->kdir = el < 1.0f ? 0.0f : 0.35f + 0.65f * fs;
        j->kshadow = el < 1.0f ? 0.0f : 0.3f + 0.7f * fs;
    }
    j->hmax = 0.0f;
    for (int i = 0; i < ZT_S * ZT_S; i++) if (j->h[i] > j->hmax) j->hmax = j->h[i];
    zt_blur(j->h, j->hs, 1.2f);                          /* the shading surface (no terrace stripes) */
    {   /* cavity: blurred - actual at two scales */
        float *b1 = (float *)malloc(sizeof(float) * ZT_S * ZT_S), *b2 = (float *)malloc(sizeof(float) * ZT_S * ZT_S);
        if (b1 && b2) {
            zt_blur(j->h, b1, 1.5f);
            zt_blur(j->h, b2, 5.0f);
            for (int i = 0; i < ZT_S * ZT_S; i++) j->cav[i] = 0.6f * (b1[i] - j->h[i]) + 0.4f * (b2[i] - j->h[i]);
        } else memset(j->cav, 0, sizeof j->cav);
        free(b1); free(b2);
    }
    {   /* the smooth ground for the near-ground materials (ps2_terrain_ground) */
        zt_ground *G = (zt_ground *)malloc(sizeof *G);
        if (G) {
            G->sig = j->sig;
            for (int z = 0; z < ZT_S; z++)
                for (int x = 0; x < ZT_S; x++) {
                    float *o = G->g + ((size_t)z * ZT_S + (size_t)x) * 3u;
                    o[0] = (zt_H(j, x + 1, z) - zt_H(j, x - 1, z)) / 2048.0f;
                    o[1] = (zt_H(j, x, z + 1) - zt_H(j, x, z - 1)) / 2048.0f;
                    o[2] = j->cav[z * ZT_S + x];
                }
            memcpy(G->h, j->h, sizeof G->h);
            free(zt_gold[2]);
            zt_gold[2] = zt_gold[1]; zt_gold[1] = zt_gold[0]; zt_gold[0] = zt_gpub;
            __atomic_store_n(&zt_gpub, G, __ATOMIC_RELEASE);
        }
    }
    {   /* the mean relief light over the land (so R_relief keeps the map's average brightness) */
        double acc = 0.0; long n = 0;
        for (int z = 0; z < ZT_S; z++)
            for (int x = 0; x < ZT_S; x++) {
                if (j->water[z * ZT_S + x]) continue;
                float gx = ZT_ZREL * (zt_H(j, x + 1, z) - zt_H(j, x - 1, z)) / 2048.0f;
                float gz = ZT_ZREL * (zt_H(j, x, z + 1) - zt_H(j, x, z - 1)) / 2048.0f;
                float nb = 1.0f / sqrtf(gx * gx + 1.0f + gz * gz);
                acc += zt_light(j, -gx * nb, nb, -gz * nb, 1.0f); n++;
            }
        j->imean = n ? (float)(acc / (double)n) : 1.0f;
        if (!(j->imean > 0.2f)) j->imean = 1.0f;
    }
    double t1 = zt_now_ms();
    zt_par(j, ZT_SH, zt_shadow_row);
    double t2 = zt_now_ms();
    zt_par(j, (int)N, zt_main_row);
    double t3 = zt_now_ms();
    j->kmean = 1.0f;
    if (zt_view == 0) {   /* half of the land's mean shift back (mountain maps darken on average: lit slopes saturate) */
        double acc = 0.0; size_t n = 0;
        for (u32 z = 0; z < N; z++)
            for (u32 x = 0; x < N; x++) {
                u32 sx = (u32)(((float)x + 0.5f) * 512.0f / (float)N), sz = (u32)(((float)z + 0.5f) * 512.0f / (float)N);
                if (j->water[sz * ZT_S + sx]) continue;
                acc += j->out[(size_t)z * N + x]; n++;
            }
        float mean = n ? (float)(acc / (double)n / 128.0) : 1.0f;
        j->kmean = zt_clampf(1.0f / sqrtf(mean > 0.5f ? mean : 0.5f), 0.97f, 1.04f);
        if (fabsf(j->kmean - 1.0f) > 0.002f)
            for (size_t i = 0; i < (size_t)N * N; i++) {
                long q = lrintf((float)j->out[i] * j->kmean);
                j->out[i] = (u8)(q > 255 ? 255 : q);
            }
    }
    {   /* mips: 2 x 2 box filter */
        u8 *src = j->out;
        for (u32 s = N; s > 1u; s >>= 1) {
            u32 d = s >> 1;
            u8 *dst = src + (size_t)s * s;
            for (u32 z = 0; z < d; z++)
                for (u32 x = 0; x < d; x++) {
                    const u8 *p = src + (size_t)(2u * z) * s + 2u * x;
                    dst[(size_t)z * d + x] = (u8)((p[0] + p[1] + p[s] + p[s + 1] + 2u) >> 2);
                }
            src = dst;
        }
    }
    double t4 = zt_now_ms();
    if (zt_mir > 0) {   /* the rear-view mirrors' grid: heights, the game's own tile colours, the relief light */
        if (j->atlas) {                                   /* tile means: the palette colours of the 4096 texels */
            u32 nt = j->nt > 1024u ? 1024u : j->nt;
            for (u32 t = 0; t < nt; t++) {
                u32 acc[3] = { 0, 0, 0 };
                const u8 *a = j->atlas + (size_t)t * 4096u;
                for (u32 k = 0; k < 4096u; k++) for (int c = 0; c < 3; c++) acc[c] += j->pal[a[k]][c];
                for (int c = 0; c < 3; c++) zt_tmean[t][c] = (float)acc[c] / 4096.0f;
            }
            zt_tmean_nt = nt; zt_tmean_sig = j->sig;
            if (zt_mtex > 0) zt_tt_build(j->atlas, (const u8 (*)[3])j->pal, nt);   /* (the textures: phase 4) */
        }
        zt_mgrid *G = zt_tmean_sig == j->sig ? (zt_mgrid *)malloc(sizeof *G) : NULL;
        if (G) {
            G->nt = 0; G->tex = NULL;
            memcpy(G->tgrid, j->tgrid, sizeof G->tgrid);
            if (zt_mtex > 0 && zt_ttex && zt_tmean_nt && (G->tex = (u8 *)malloc((size_t)zt_tmean_nt * 696u))) {
                memcpy(G->tex, zt_ttex, (size_t)zt_tmean_nt * 696u);
                G->nt = zt_tmean_nt;
            }
            u32 lv = 0, side = N;                          /* the bake's level with one texel per tile (2 x 2 samples) */
            size_t off = 0;
            while (side > 256u) { off += (size_t)side * side; side >>= 1; lv++; }
            const u8 *ratio = side == 256u ? j->out + off : NULL;
            G->sig = j->sig; G->gen = j->gen;
            const u32 S = PS2_TERRAIN_MIR_SIDE;
            u32 nwater = 0;
            float wtop = 0.0f;
            for (u32 vz = 0; vz < S; vz++)
                for (u32 vx = 0; vx < S; vx++) {
                    float col[3] = { 0, 0, 0 }, rr = 0.0f, wsum = 0.0f;
                    int wet = 1;
                    for (int dz = -1; dz <= 0; dz++)               /* the (up to) four tiles around the vertex */
                        for (int dx = -1; dx <= 0; dx++) {
                            int cx = (int)vx + dx, cz = (int)vz + dz;
                            if (cx < 0 || cz < 0 || cx > 255 || cz > 255) continue;
                            u16 g = j->tgrid[cz * 256 + cx];
                            u32 t = g & 0x3FFu;
                            if (t >= zt_tmean_nt) continue;
                            for (int c = 0; c < 3; c++) col[c] += zt_tmean[t][c];
                            rr += ratio ? (float)ratio[cz * 256 + cx] / 128.0f : 1.0f;
                            wsum += 1.0f;
                            if (!(g & 0x2800u)) wet = 0;          /* (0x2000 open water, 0x800 the ground query's 0) */
                        }
                    if (wsum <= 0.0f) { wet = 0; col[0] = col[1] = col[2] = 0.0f; rr = 0.0f; wsum = 1.0f; }
                    float k = (90.0f / 128.0f) * (rr / wsum) / wsum;   /* the terrain's vertex colour x the relief */
                    u32 rgba = 0;
                    for (int c = 0; c < 3; c++) {
                        long q = lrintf(col[c] * k);
                        rgba |= (u32)(q < 0 ? 0 : q > 255 ? 255 : q) << (8 * c);
                    }
                    {   /* alpha: the terrain's vertex colour x the relief (x 127.5), the textures' shade (phase 4) */
                        long sh = lrintf((90.0f / 128.0f) * (rr / wsum) * 127.5f);
                        rgba |= (u32)(sh < 0 ? 0 : sh > 255 ? 255 : sh) << 24;
                    }
                    float *o = G->v + ((size_t)vz * S + vx) * 4u;
                    o[0] = -262144.0f + (float)vx * 2048.0f;
                    o[2] = -262144.0f + (float)vz * 2048.0f;
                    float y = j->h[(vz * 2u) * ZT_S + vx * 2u];
                    if (wet) {                     /* the water at its level: the lowest sample around (the sea 0, a lake's) */
                        for (int sz = (int)vz * 2 - 1; sz <= (int)vz * 2 + 1; sz++)
                            for (int sx = (int)vx * 2 - 1; sx <= (int)vx * 2 + 1; sx++)
                                if (sx >= 0 && sz >= 0 && sx < ZT_S && sz < ZT_S && j->h[sz * ZT_S + sx] < y) y = j->h[sz * ZT_S + sx];
                        if (y > wtop) wtop = y;
                    }
                    o[1] = y;
                    memcpy(&o[3], &rgba, 4);
                    nwater += (u32)wet;
                }
            pthread_mutex_lock(&zt_mx);
            if (zt_mold[2]) free(zt_mold[2]->tex);
            free(zt_mold[2]);
            zt_mold[2] = zt_mold[1]; zt_mold[1] = zt_mold[0]; zt_mold[0] = zt_mpub;
            __atomic_store_n(&zt_mpub, G, __ATOMIC_RELEASE);
            pthread_mutex_unlock(&zt_mx);
            ps2_log("terrain: the mirrors' grid of map %08X (gen %u): %u x %u vertices, %u tiles' means, %u at the water's "
                    "level (the highest %.0f units), relief from level %u (%s); %.1f ms (ACZ_VR_COCKPIT_MIRRORS_TERRAIN)",
                    j->sig, j->gen, S, S, zt_tmean_nt, nwater, (double)wtop, lv, ratio ? "1 texel a tile" : "none", zt_now_ms() - t4);
        }
    }
    free(j->atlas); j->atlas = NULL;
    if (zt_dump[0]) {
        u8 *b = (u8 *)malloc(ZT_S * ZT_S);
        if (b) {
            float hm = j->hmax > 1.0f ? j->hmax : 1.0f;
            for (int i = 0; i < ZT_S * ZT_S; i++) b[i] = (u8)lrintf(j->h[i] * 255.0f / hm);
            zt_pgm("height", j->sig, b, ZT_S, ZT_S);
            for (int i = 0; i < ZT_S * ZT_S; i++) b[i] = j->water[i] ? 255 : 0;
            zt_pgm("water", j->sig, b, ZT_S, ZT_S);
            free(b);
        }
        zt_pgm("ratio", j->sig, j->out, N, N);
        static const char *const TN[4] = { "relief", "erosion", "shadow", "cavity" };
        for (int k = 0; k < 4; k++) if (j->dbg[k]) zt_pgm(TN[k], j->sig, j->dbg[k], N, N);
    }
    {   /* statistics of level 0 */
        double acc = 0.0; u32 lo = 255, hi = 0;
        for (size_t i = 0; i < (size_t)N * N; i++) { acc += j->out[i]; if (j->out[i] < lo) lo = j->out[i]; if (j->out[i] > hi) hi = j->out[i]; }
        ps2_log("terrain: bake %u for map %08X done: %ux%u, %u levels, sun %.3f %.3f %.3f (%s, elevation %.1f deg), "
                "mean ratio %.3f (min %.3f max %.3f; mean correction x%.3f), land light mean %.3f (flat %.3f, sun factors %.2f / %.2f), "
                "max height %.0f units; %.0f ms "
                "(prep %.0f, shadow %.0f, texels %.0f, mips %.0f) on %d threads",
                j->gen, j->sig, N, N, zt_levels, j->sun[0], j->sun[1], j->sun[2], j->sun_src ? "C1" : "default",
                asinf(zt_clampf(j->sun[1], -1.0f, 1.0f)) * 57.29578f, acc / ((double)N * N) / 128.0, lo / 128.0, hi / 128.0, j->kmean,
                j->imean, j->iflat, j->kdir, j->kshadow, j->hmax, t4 - t0, t1 - t0, t2 - t1, t3 - t2, t4 - t3, zt_threads);
    }
    pthread_mutex_lock(&zt_mx);
    free(zt_pub);
    zt_pub = j->out; zt_pub_bytes = bytes; zt_pub_gen = j->gen; j->out = NULL;
    if (j->sig == zt_map_sig) {
        if (zt_baked_sig != j->sig || zt_valid_from == 0xFFFFFFFFu) zt_valid_from = j->gen;
        zt_baked_sig = j->sig;
        memcpy(zt_baked_sun, j->sun, sizeof zt_baked_sun);
    }
    zt_busy = 0;
    pthread_mutex_unlock(&zt_mx);
    for (int k = 0; k < 4; k++) free(j->dbg[k]);
    free(j);
    return NULL;
fail:
    ps2_log("terrain: bake %u: out of memory", j->gen);
    free(j->out);
    free(j->atlas);
    for (int k = 0; k < 4; k++) free(j->dbg[k]);
    pthread_mutex_lock(&zt_mx); zt_busy = 0; pthread_mutex_unlock(&zt_mx);
    free(j);
    return NULL;
}

/* ---- the game side (EE thread) -------------------------------------------------------------------------------- */
static int zt_ptr_ok(u32 p) { return p >= 0x100000u && p < 0x2000000u; }
static u32 zt_fnv(u32 h, u32 v) { for (int k = 0; k < 4; k++) { h ^= (v >> (8 * k)) & 0xFFu; h *= 16777619u; } return h; }
/* the sample (x, z) of the map, 0..512 each; *tile = its tile id */
static u8 zt_sample(u32 T, u32 bi, u32 hb, u32 ti, u32 tr, int x, int z, u32 *tile) {
    int bx = x >> 5, bz = z >> 5;
    if (bx > 15) bx = 15;
    if (bz > 15) bz = 15;
    int lx = x - bx * 32, lz = z - bz * 32;
    u32 b = (u32)(bz * 16 + bx);
    u32 idx = ps2_r8(bi + b);
    if (tile) {
        u32 rec = ps2_r8(ti + b);
        int tx = lx >> 1, tz = lz >> 1;
        if (tx > 15) tx = 15;
        if (tz > 15) tz = 15;
        *tile = ps2_r16(tr + rec * 512u + (u32)(tz * 16 + tx) * 2u);
    }
    (void)T;
    return ps2_r8(hb + idx * 0x441u + (u32)(lz * 33 + lx));
}
static u32 zt_signature(u32 T, u32 *bi, u32 *hb, u32 *ti, u32 *tr) {
    *bi = ps2_r32(T + 0xB8u); *hb = ps2_r32(T + 0xBCu); *ti = ps2_r32(T + 0x14u); *tr = ps2_r32(T + 0x18u);
    if (!zt_ptr_ok(*bi) || !zt_ptr_ok(*hb) || !zt_ptr_ok(*ti) || !zt_ptr_ok(*tr)) return 0;
    u32 h = 2166136261u;
    h = zt_fnv(h, T); h = zt_fnv(h, *bi); h = zt_fnv(h, *hb); h = zt_fnv(h, *ti); h = zt_fnv(h, *tr);
    for (u32 k = 0; k < 256u; k += 4u) { h = zt_fnv(h, ps2_r32(*bi + k)); h = zt_fnv(h, ps2_r32(*ti + k)); }
    for (int z = 0; z < ZT_S; z += 16)                   /* a sparse sample of the heights: 33 x 33 */
        for (int x = 0; x < ZT_S; x += 16) h = zt_fnv(h, zt_sample(T, *bi, *hb, *ti, *tr, x, z, NULL));
    return h ? h : 1u;
}
static int zt_sun(float out[3]) {
#ifdef ZT_C1
    static ps2_cloud_env e;
    if (ps2_cloud_env_read(&e) && (e.valid & CL_ENV_SUN)) {
        float l = sqrtf(e.sun_dir[0] * e.sun_dir[0] + e.sun_dir[1] * e.sun_dir[1] + e.sun_dir[2] * e.sun_dir[2]);
        if (l > 0.5f && l < 1.5f) { for (int k = 0; k < 3; k++) out[k] = e.sun_dir[k] / l; return 1; }
    }
#endif
    return 0;
}
static int zt_mgr_enter(ps2_ctx *ctx, void *user) {
    (void)user;
    static u64 last_f, sun_wait;
    u64 f = ps2_kernel_vblank_count();
    if (f == last_f) return 0;
    last_f = f;
    if (!ps2_terrain_real_on()) return 0;
    u32 mgr = ctx->r[4].uw[0];
    if (!zt_ptr_ok(mgr)) return 0;
    u32 T = ps2_r32(mgr + 4u), bi, hb, ti, tr;
    if (!zt_ptr_ok(T)) return 0;
    u32 sig = zt_signature(T, &bi, &hb, &ti, &tr);
    if (!sig) return 0;
    pthread_mutex_lock(&zt_mx);
    if (sig != zt_map_sig) {
        if (zt_dbg || zt_map_sig) ps2_log("terrain: map %08X -> %08X at field %llu (T %08X)", zt_map_sig, sig, (unsigned long long)f, T);
        zt_map_sig = sig; zt_stable = 0; sun_wait = 0;
        zt_valid_from = (zt_baked_sig == sig) ? zt_valid_from : 0xFFFFFFFFu;
    } else zt_stable++;
    int busy = zt_busy, have = zt_baked_sig == sig;
    float bsun[3];
    memcpy(bsun, zt_baked_sun, sizeof bsun);
    pthread_mutex_unlock(&zt_mx);
    if (busy || zt_stable < 30u) return 0;
    float sun[3] = { 0.0f, 0.0f, 0.0f };
    int sun_ok;
    if (have) {                                          /* the bake is current: re-bake only if the sun moved */
        if ((zt_stable % 120u) != 0u || !zt_sun(sun)) return 0;
        float d = sun[0] * bsun[0] + sun[1] * bsun[1] + sun[2] * bsun[2];
        if (d > cosf(3.0f * ZT_PI / 180.0f)) return 0;
        ps2_log("terrain: the sun moved (%.3f %.3f %.3f -> %.3f %.3f %.3f): baking again", bsun[0], bsun[1], bsun[2], sun[0], sun[1], sun[2]);
        sun_ok = 1;
    } else {
        sun_ok = zt_sun(sun);
        if (!sun_ok && ++sun_wait < 600u) return 0;      /* C1 publishes the sun once the sky update runs */
        if (!sun_ok) { float el = 50.0f * ZT_PI / 180.0f, az = 0.6f; sun[0] = -cosf(el) * sinf(az); sun[1] = sinf(el); sun[2] = -cosf(el) * cosf(az); }
    }
    zt_job *j = (zt_job *)calloc(1, sizeof *j);
    if (!j) return 0;
    long nwet = 0, nwet_h = 0;
    for (int z = 0; z < ZT_S; z++)
        for (int x = 0; x < ZT_S; x++) {
            u32 tile = 0;
            u8 v = zt_sample(T, bi, hb, ti, tr, x, z, &tile);
            int wet = (tile & 0x800u) != 0u;
            j->h[z * ZT_S + x] = (float)v * 80.0f;
            j->water[z * ZT_S + x] = (u8)wet;
            if (wet) { nwet++; if (v) nwet_h++; }
            if (!((x | z) & 1) && x < 512 && z < 512)   /* the mirrors' tile grid (one tile per 2 x 2 samples) */
                j->tgrid[(z >> 1) * 256 + (x >> 1)] = (u16)tile;
        }
    if (zt_mir < 0) {
        const char *e = getenv("ACZ_VR_COCKPIT_MIRRORS_TERRAIN"), *l = getenv("ACZ_VR_COCKPIT_MIRRORS_LIVE"),
                   *m = getenv("ACZ_VR_COCKPIT_MIRRORS");
        zt_mir = !(e && *e == '0') && !(l && *l == '0') && !(m && (*m == '0' || !strcmp(m, "sky")))
              && m && *m && strcmp(m, "tone");   /* (perf6_mirrors: unset / "tone" = TONE, the default: no terrain) */
        const char *t = getenv("ACZ_VR_COCKPIT_MIRRORS_TERRAIN_TEX");
        zt_mtex = zt_mir > 0 && !(t && *t == '0');
    }
    double tm0 = zt_now_ms();
    if (zt_mir > 0) {                                    /* the mirrors' terrain: the atlas and its palette with a new map */
        u32 a0 = ps2_r32(T + 0x24u), a1 = ps2_r32(T + 0x28u), cl = ps2_r32(T + 0x20u);
        u32 nt = (zt_ptr_ok(a0) && zt_ptr_ok(a1) && a1 > a0) ? (a1 - a0) / 4096u : 0u;
        if (!have && zt_tmean_sig != sig && nt && nt <= 1024u && a0 + nt * 4096u <= PS2_RAM_SIZE && zt_ptr_ok(cl) &&
            (j->atlas = (u8 *)malloc((size_t)nt * 4096u))) {
            for (u32 i = 0; i < 256u; i++) {                  /* CSM1: bits 3 and 4 of the index swapped (zero_water.c) */
                u32 s = (i & 0xE7u) | ((i & 0x08u) << 1) | ((i & 0x10u) >> 1);
                for (int k = 0; k < 3; k++) j->pal[i][k] = ps2_r8(cl + s * 4u + (u32)k);
            }
            memcpy(j->atlas, ps2_ram + a0, (size_t)nt * 4096u);   /* (main RAM: zt_ptr_ok) */
            j->nt = nt;
        }
    }
    double tm1 = zt_now_ms();
    memcpy(j->sun, sun, sizeof sun);
    j->sun_src = sun_ok;
    j->sig = sig; j->n = zt_res;
    pthread_mutex_lock(&zt_mx);
    j->gen = ++zt_gen;
    zt_busy = 1;
    pthread_mutex_unlock(&zt_mx);
    ps2_log("terrain: baking map %08X (gen %u, field %llu): water samples %ld (%ld of them with a height), sun %s%s",
            sig, j->gen, (unsigned long long)f, nwet, nwet_h, sun_ok ? "from C1" : "default (C1 has none)",
            zt_mir > 0 ? (j->atlas ? "; the mirrors' atlas copied" : "; the mirrors' tile grid") : "");
    if (zt_mir > 0 && j->atlas)
        ps2_log("terrain: the mirrors' atlas: %u tiles (%u KB) copied in %.2f ms on the EE", j->nt, j->nt * 4u, tm1 - tm0);
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &at, zt_bake, j) != 0) {
        ps2_log("terrain: bake thread failed");
        pthread_mutex_lock(&zt_mx); zt_busy = 0; pthread_mutex_unlock(&zt_mx);
        free(j);
    }
    pthread_attr_destroy(&at);
    return 0;
}

static float env_f(const char *n, float def, float lo, float hi) {
    const char *e = getenv(n);
    if (!e || !*e) return def;
    float v = (float)atof(e);
    return v < lo ? lo : v > hi ? hi : v;
}

static void zt_cfg(void) {                          /* once; also from the renderer's init (any order) */
    static volatile int done;
    if (done) return;
    const char *e = getenv("ACZ_TERRAIN_REAL");
    zt_on = !(e && *e == '0');
    e = getenv("ACZ_DEBUG_TERRAIN");
    zt_dbg = e && *e && *e != '0';
    e = getenv("ACZ_DEBUG_TERRAIN_VIEW");
    zt_view = e ? atoi(e) : 0;
    e = getenv("ACZ_DEBUG_TERRAIN_DUMP");
    if (e && *e) snprintf(zt_dump, sizeof zt_dump, "%s", e);
    e = getenv("ACZ_TERRAIN_RES");
    if (e && *e) {
        u32 r = (u32)atoi(e), p = 512;
        while (p < r && p < 4096u) p <<= 1;
        zt_res = p;
    }
    zt_levels = 0;
    for (u32 s = zt_res; s; s >>= 1) zt_levels++;
    zt_oct = zt_res >= 4096u ? 4 : zt_res >= 2048u ? 3 : 2;
    zt_threads = (int)env_f("ACZ_TERRAIN_THREADS", 4.0f, 1.0f, 16.0f);
    zt_s_relief = env_f("ACZ_TERRAIN_RELIEF", zt_s_relief, 0.0f, 2.0f);
    zt_s_ero = env_f("ACZ_TERRAIN_EROSION", zt_s_ero, 0.0f, 3.0f);
    zt_s_shadow = env_f("ACZ_TERRAIN_SHADOW", zt_s_shadow, 0.0f, 1.0f);
    zt_s_cav = env_f("ACZ_TERRAIN_CAVITY", zt_s_cav, 0.0f, 3.0f);
    e = getenv("ACZ_TERRAIN_CLAMP");
    if (e && *e) {
        float lo, hi;
        if (sscanf(e, "%f,%f", &lo, &hi) == 2 && lo > 0.0f && lo <= 1.0f && hi >= 1.0f && hi < 1.99f) { zt_lo = lo; zt_hi = hi; }
    }
    done = 1;
}

void zero_terrain_init(void) {
    zt_cfg();
    if (!zt_on) { ps2_log("terrain: realistic terrain off for this session (ACZ_TERRAIN_REAL=0)"); return; }
    u32 w = ps2_r32(ZT_MGR);
    if ((w >> 16) != 0x27BDu || ps2_hook_before(ZT_MGR, zt_mgr_enter, NULL, 56, "zero-terrain") < 0) {
        ps2_log("terrain: terrain manager hook failed (code %08X): realistic terrain off", w);
        zt_on = 0;
        return;
    }
    ps2_log("terrain: realistic terrain (T2 relief light) available (switch: F4 Rendering > Realistic terrain, "
            "ACZ_TERRAIN_REAL): %ux%u texels over the map, relief %.2f erosion %.2f shadow %.2f cavity %.2f, clamp "
            "%.2f..%.2f, %d bake threads%s%s", zt_res, zt_res, zt_s_relief, zt_s_ero, zt_s_shadow, zt_s_cav, zt_lo, zt_hi, zt_threads,
            zt_view ? ", debug view" : "", zt_dump[0] ? ", dumps" : "");
}
