/* zero_water.c -- shader water (bug #63, the user's wish "make the shader water"; ps2_water.h, ACZ_TERRAIN_WATER).

   What the game has (SLUS-21346; the map dumps of all 18 missions, a private script): the sea, lakes, rivers and
   canals are ordinary terrain tiles of the ground photo (64 x 64 PSMT8 texels of 6.25 m, CLUT T+0x20, atlas T+0x24, the
   tile grid of zero_terrain.c), painted with ONE flat water colour per map (mission 4 (70, 89, 97), mission 12
   (14, 21, 24), mission 8's lake an olive (52, 54, 32), mission 2's river (24, 52, 33); mission 14's sea a dark textured
   (9, 18, 20) family). The grid marks the cells that carry water: flag 0x2000 (with 0x400) = open water, a flat tile
   (mission 4: 16230 cells, mission 14: 27216, mission 12: 48, mission 8: 11); 0x1000 = the shores, river banks, canal
   sides and ports (detail pass 1 there); 0x800 (rare: mission 9's pond, mission 17/18's dam channels) = the ground query
   returns 0. The water flag 0x800 that zero_terrain.c knows is not what marks the sea ("water samples 0" in mission 4).

   The water texels: the palette entries that the game uses (almost) only in those flagged cells (fraction outside
   < 2e-6, or 300 x rarer than inside), starting from the most used one and growing through near colours (<= 12 apart,
   <= 45 from the first) -- the painted water and its shallow-water shades, never land: mission 8's lake colour is 9
   levels from its forest's, but the forest's entries are used all over the map. Weight 1 within 30 of the first colour,
   fading to 0 at 45 (shore blends); per tile the texels' weights, closed over isolated dots (boats, white caps: >= 6 of
   8 neighbours water). Missions with water (maps by the dump signature of zero_props.c): 4 (sea + canal, 16544 cells of
   water), 14 (sea, 27213), 12 (lake + river, 76), 8 (lake + river, 155), 2 (river, 322), 13 (river, 242), 11 (river at
   night, 297), 6 (a thin river, 21); none in 1, 3, 5, 7, 9, 10, 15-18 (17/18: the dam's flagged cells have no painted
   water).

   Flow: the EE thread (a hook before the terrain manager 0x15E1D8, after zero_terrain.c / zero_props.c) copies the
   atlas, the palette and the grid once a map has been stable for 30 frames (~1 ms); one worker thread at the lowest
   priority builds the masks with their mips (64 .. 1) and the per-tile class (none / partial / full) and publishes them
   (generation counter; the renderer applies only generations of the loaded map). The renderer (ps2_video.c wat_*)
   uploads them into an R8 2D-array texture (layer = tile id); rn_vp_terrain.c calls zero_water_patch() for the first
   pass of every terrain patch, which finds the patch's cell and tile, and for a tile with water solves the patch's uv
   mapping from its corners (as zero_props.c does) into the block (ps2_water.h). The per-frame parameters (sun, sky
   colours, strengths, the wave layers' offsets for the frame's display time) go into a reserved block. mesh.vert /
   mesh.frag + terrain_water.glsl do the shading.

   Switches: ACZ_TERRAIN_WATER (master, default on; 0 = off: no hook, no textures, nothing changes), _WAVES (wave slope strength, 1.5; 1 before 2026-10-23), _BODY (the painted colour as the mean look, 1),
   _SCALE (the wave layers' size, 1), _GLOSS
   (sun glitter, 1), _REFL (sky reflection, 0.85), _CLOUDS (reflection of our clouds through the cloud shadow map, 0.8),
   _FADE=near,far (m, 3000,10000; the effect fades to the game's flat look between them), _SPEED (wave motion, 1),
   _ANISO (the waves' anisotropic filtering, 4), _AB=N (in-run GPU A/B, ps2_video.c), PS2_PERF_AB_FLIP=water (exact
   image A/B of one frame). Debug: ACZ_DEBUG_WATER=1 (log; 2 more), ACZ_DEBUG_WATER_TINT=1 (the mask as blue) / 2 (the
   wave normals), ACZ_DEBUG_WATER_DUMP=dir (the masks: a PGM sheet of the water tiles and level 0 raw),
   ACZ_DEBUG_WATER_COST=bits (cost hunt: 1 no cloud reflection, 2 no glitter, 4 flat water).
*/
#include "ps2_runtime.h"
#include "ps2_menv.h"
#include "ps2_hook.h"
#include "ps2_water.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(__has_include)
#if __has_include("ps2_clouds.h")
#include "ps2_clouds.h"
#define ZW_C1 1
#endif
#endif
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

u64 ps2_kernel_vblank_count(void);
extern u32 ps2_gs_frame_count;

#define ZW_MGR 0x0015E1D8u                 /* terrain manager: a0 = manager, T = *(a0 + 4) */
#define ZW_FLAGS 0x3800u                   /* grid flags of cells that carry water: 0x2000 open, 0x1000 shores, 0x800 */
#define ZW_UPM 5.12f                       /* game units per metre (400 m per 2048-unit cell) */

static int zw_cfg_done, zw_on, zw_dbg, zw_tint, zw_ab_n, zw_cost;
static float zw_waves = 1.5f, zw_gloss = 1.0f, zw_refl = 0.85f, zw_clouds = 0.8f, zw_speed = 1.0f;
static float zw_fade0 = 3000.0f, zw_fade1 = 10000.0f;
static float zw_scale = 1.0f;                   /* ACZ_TERRAIN_WATER_SCALE: the wave layers' size */
static float zw_body = 1.0f;                    /* ACZ_TERRAIN_WATER_BODY: the painted colour as the mean look */
static char zw_dump[260];

typedef struct zw_map {
    u32 sig, gen, nt;
    u16 grid[65536];                       /* tile id + flags per cell, rows by z */
    u8 cls[PS2_WATER_TILES];               /* 0 none, 1 partial, 2 full */
    float frac[PS2_WATER_TILES];           /* water share of the tile */
    u8 *mask;                              /* all levels: level l of tile t at off[l] + t * (64 >> l)^2 */
    size_t bytes;
    u32 ncol;                              /* water palette entries */
    float wcol[3];                         /* the first water colour (0..255) */
    struct zw_map *prev;
} zw_map;

static zw_map *volatile zw_pub;            /* the newest built map (renderer reads) */
static volatile u32 zw_valid_from = 0xFFFFFFFFu;
static u32 zw_gen;
static volatile int zw_busy;
static u32 zw_sig, zw_stable, zw_built_sig, zw_T;
static u64 zw_st_patches, zw_st_water, zw_st_full, zw_st_nomap;

static float env_f(const char *n, float def, float lo, float hi) {
    const char *e = getenv(n);
    if (!e || !*e) return def;
    float v = (float)atof(e);
    return v < lo ? lo : v > hi ? hi : v;
}
static void zw_cfg(void) {
    if (zw_cfg_done) return;
    const char *e = ps2_menv("ACZ_TERRAIN_WATER");          /* default on (bug #63: within the cost target) */
    zw_on = !(e && *e == '0');
    e = getenv("ACZ_DEBUG_WATER");
    zw_dbg = e ? atoi(e) : 0;
    e = getenv("ACZ_DEBUG_WATER_TINT");
    zw_tint = e ? atoi(e) : 0;
    e = getenv("ACZ_DEBUG_WATER_COST");                   /* (cost hunt: 1 no cloud reflection, 2 no glitter, 4 flat) */
    zw_cost = e ? atoi(e) : 0;
    e = getenv("ACZ_DEBUG_WATER_DUMP");
    if (e && *e) snprintf(zw_dump, sizeof zw_dump, "%s", e);
    zw_waves = env_f("ACZ_TERRAIN_WATER_WAVES", zw_waves, 0.0f, 4.0f);
    zw_gloss = env_f("ACZ_TERRAIN_WATER_GLOSS", zw_gloss, 0.0f, 4.0f);
    zw_refl = env_f("ACZ_TERRAIN_WATER_REFL", zw_refl, 0.0f, 1.0f);
    zw_clouds = env_f("ACZ_TERRAIN_WATER_CLOUDS", zw_clouds, 0.0f, 1.0f);
    zw_speed = env_f("ACZ_TERRAIN_WATER_SPEED", zw_speed, 0.0f, 4.0f);
    zw_scale = env_f("ACZ_TERRAIN_WATER_SCALE", zw_scale, 0.25f, 8.0f);
    zw_body = env_f("ACZ_TERRAIN_WATER_BODY", zw_body, 0.0f, 1.0f);
    zw_ab_n = (int)env_f("ACZ_TERRAIN_WATER_AB", 0.0f, 0.0f, 100000.0f);
    e = getenv("ACZ_TERRAIN_WATER_FADE");
    if (e && *e) {
        float a, b;
        if (sscanf(e, "%f,%f", &a, &b) == 2 && a >= 0.0f && b > a + 10.0f) { zw_fade0 = a; zw_fade1 = b; }
    }
    zw_cfg_done = 1;
}
int zero_water_on(void) { zw_cfg(); return zw_on; }
int zero_water_ab_n(void) { zw_cfg(); return zw_ab_n; }
int zero_water_tint(void) { zw_cfg(); return zw_tint; }
uint32_t zero_water_valid_from(void) { return zw_valid_from; }

static size_t zw_level_off(u32 nt, u32 l) {
    size_t o = 0;
    for (u32 k = 0; k < l; k++) { size_t s = PS2_WATER_TILE >> k; o += (size_t)nt * s * s; }
    return o;
}

uint32_t zero_water_copy(void *dst, size_t cap, uint32_t have_gen, uint32_t *ntiles) {
    const zw_map *m = __atomic_load_n(&zw_pub, __ATOMIC_ACQUIRE);
    if (!m || !m->mask || m->gen == have_gen || m->bytes > cap) return 0;
    memcpy(dst, m->mask, m->bytes);
    *ntiles = m->nt;
    return m->gen;
}

/* ---- the build (worker thread) ---------------------------------------------------------------------------------- */
typedef struct { zw_map *m; u8 *idx; u8 pal[256][3]; u64 f0; } zw_job;

static void zw_pgm(const char *name, u32 sig, const u8 *px, u32 w, u32 h) {
    char path[400];
    snprintf(path, sizeof path, "%s/water_%08X_%s.pgm", zw_dump, sig, name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P5\n%u %u\n255\n", w, h);
    fwrite(px, 1, (size_t)w * h, f);
    fclose(f);
}

static void *zw_build_thread(void *arg) {
    zw_job *j = (zw_job *)arg;
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    { extern void ps2_host_thread_start(const char *); ps2_host_thread_start("bg"); }  /* (PS2_THREAD_CORES=h) */
#endif
    zw_map *m = j->m;
    u32 nt = m->nt;
    double t0 = (double)clock();
    /* palette use per tile, then per class of cells (flagged / the rest), weighted by how many cells show the tile */
    static u32 th[PS2_WATER_TILES][256];
    static double cf[PS2_WATER_TILES], cr[PS2_WATER_TILES];
    memset(th, 0, sizeof th); memset(cf, 0, sizeof cf); memset(cr, 0, sizeof cr);
    for (u32 t = 0; t < nt; t++) for (u32 k = 0; k < 4096u; k++) th[t][j->idx[t * 4096u + k]]++;
    for (u32 c = 0; c < 65536u; c++) {
        u32 t = m->grid[c] & 0x3FFu;
        if (t >= nt) continue;
        if (m->grid[c] & ZW_FLAGS) cf[t] += 1.0; else cr[t] += 1.0;
    }
    double hf[256] = { 0 }, hr[256] = { 0 }, nf = 0.0, nr = 0.0;
    for (u32 t = 0; t < nt; t++)
        for (u32 i = 0; i < 256u; i++) { hf[i] += cf[t] * th[t][i]; hr[i] += cr[t] * th[t][i]; }
    for (u32 i = 0; i < 256u; i++) { nf += hf[i]; nr += hr[i]; }
    float w[256];
    memset(w, 0, sizeof w);
    int seed = -1;
    u32 ncol = 0;
    if (nf >= 4096.0) {
        double sf[256], sr[256];
        int excl[256];
        for (u32 i = 0; i < 256u; i++) {
            sf[i] = hf[i] / nf; sr[i] = nr > 0.0 ? hr[i] / nr : 0.0;
            excl[i] = sr[i] < 2e-6 || sf[i] > 300.0 * sr[i];
        }
        for (u32 i = 0; i < 256u; i++)
            if (excl[i] && sf[i] > 0.0015 && (seed < 0 || sf[i] > sf[seed])) seed = (int)i;
        if (seed >= 0 && sf[seed] < 0.02) {      /* only a few structure colours (mission 17's dam): no painted water */
            if (zw_dbg) ps2_log("water: map %08X: strongest flagged-only colour %d (%u %u %u) covers %.4f: no painted water",
                                m->sig, seed, j->pal[seed][0], j->pal[seed][1], j->pal[seed][2], sf[seed]);
            seed = -1;
        }
        if (seed >= 0) {
            u8 mem[256];
            memset(mem, 0, sizeof mem);
            mem[seed] = 1;
            const u8 *ps = j->pal[seed];
            for (int grow = 1; grow; ) {                 /* single linkage <= 12 apart, all within 45 of the seed */
                grow = 0;
                for (u32 i = 0; i < 256u; i++) {
                    if (mem[i] || !excl[i] || !(sf[i] > 0.0002)) continue;
                    float d0 = sqrtf((float)((j->pal[i][0] - ps[0]) * (j->pal[i][0] - ps[0]) + (j->pal[i][1] - ps[1]) * (j->pal[i][1] - ps[1])
                                             + (j->pal[i][2] - ps[2]) * (j->pal[i][2] - ps[2])));
                    if (d0 > 45.0f) continue;
                    for (u32 k = 0; k < 256u; k++) {
                        if (!mem[k]) continue;
                        int a = j->pal[i][0] - j->pal[k][0], b = j->pal[i][1] - j->pal[k][1], c = j->pal[i][2] - j->pal[k][2];
                        if (a * a + b * b + c * c <= 144) { mem[i] = 1; grow = 1; break; }
                    }
                }
            }
            for (u32 i = 0; i < 256u; i++) {
                if (!mem[i]) continue;
                float d0 = sqrtf((float)((j->pal[i][0] - ps[0]) * (j->pal[i][0] - ps[0]) + (j->pal[i][1] - ps[1]) * (j->pal[i][1] - ps[1])
                                         + (j->pal[i][2] - ps[2]) * (j->pal[i][2] - ps[2])));
                w[i] = d0 <= 30.0f ? 1.0f : d0 >= 45.0f ? 0.0f : 1.0f - (d0 - 30.0f) / 15.0f;
                if (w[i] > 0.0f) ncol++;
                if (zw_dbg >= 2) ps2_log("water: map %08X colour %u (%u %u %u) share %.5f outside %.7f weight %.2f", m->sig, i,
                                         j->pal[i][0], j->pal[i][1], j->pal[i][2], sf[i], sr[i], w[i]);
            }
            for (int k = 0; k < 3; k++) m->wcol[k] = (float)ps[k];
        }
    }
    m->ncol = ncol;
    /* the masks, level 0 then the mips */
    m->bytes = zw_level_off(nt, PS2_WATER_LEVELS);
    m->mask = (u8 *)calloc(1, m->bytes ? m->bytes : 1u);
    u32 nwater = 0, nfull = 0;
    double area = 0.0;
    if (m->mask && ncol) {
        static float f[4096];
        for (u32 t = 0; t < nt; t++) {
            if (cf[t] <= 0.0) continue;              /* a tile never shown in a flagged cell has no water colour */
            float mx = 0.0f, mn = 1.0f, sum = 0.0f;
            for (u32 k = 0; k < 4096u; k++) f[k] = w[j->idx[t * 4096u + k]];
            u8 *o = m->mask + (size_t)t * 4096u;
            for (int y = 0; y < 64; y++)
                for (int x = 0; x < 64; x++) {
                    float v = f[y * 64 + x];
                    if (v < 0.5f) {                      /* isolated dots inside water (boats, white caps) */
                        float nb = 0.0f;
                        for (int dy = -1; dy <= 1; dy++)
                            for (int dx = -1; dx <= 1; dx++) {
                                if (!dx && !dy) continue;
                                int xx = x + dx < 0 ? 0 : x + dx > 63 ? 63 : x + dx, yy = y + dy < 0 ? 0 : y + dy > 63 ? 63 : y + dy;
                                nb += f[yy * 64 + xx];
                            }
                        if (nb >= 6.0f && nb / 8.0f > v) v = nb / 8.0f;
                    }
                    o[y * 64 + x] = (u8)lrintf(v * 255.0f);
                }
            {   /* specks away from the water body (river banks whose colours occur only beside the river, port
                   concrete): 4-connected water parts (>= 0.5) under 8 texels that do not touch the tile's edge go */
                static u8 seen[4096];
                static u16 stk[4096], part[4096];
                memset(seen, 0, sizeof seen);
                for (int i0 = 0; i0 < 4096; i0++) {
                    if (seen[i0] || o[i0] < 128u) continue;
                    u32 sp = 0, n = 0;
                    int edge = 0;
                    stk[sp++] = (u16)i0; seen[i0] = 1;
                    while (sp) {
                        int i = stk[--sp], x = i & 63, y = i >> 6;
                        part[n++] = (u16)i;
                        if (x == 0 || x == 63 || y == 0 || y == 63) edge = 1;
                        const int nb[4] = { x > 0 ? i - 1 : -1, x < 63 ? i + 1 : -1, y > 0 ? i - 64 : -1, y < 63 ? i + 64 : -1 };
                        for (int k = 0; k < 4; k++)
                            if (nb[k] >= 0 && !seen[nb[k]] && o[nb[k]] >= 128u) { seen[nb[k]] = 1; stk[sp++] = (u16)nb[k]; }
                    }
                    if (!edge && n < 8u) for (u32 k = 0; k < n; k++) o[part[k]] = 0;
                }
                for (int i = 0; i < 4096; i++)          /* weak texels without a water neighbour go too */
                    if (o[i] > 0 && o[i] < 128u) {
                        int x = i & 63, y = i >> 6, wet = 0;
                        for (int dy = -1; dy <= 1 && !wet; dy++)
                            for (int dx = -1; dx <= 1; dx++) {
                                int xx = x + dx, yy = y + dy;
                                if (xx < 0 || xx > 63 || yy < 0 || yy > 63) continue;
                                if (o[yy * 64 + xx] >= 128u) { wet = 1; break; }
                            }
                        if (!wet) o[i] = 0;
                    }
            }
            for (int i = 0; i < 4096; i++) {
                float v = (float)o[i] / 255.0f;
                if (v > mx) mx = v;
                if (v < mn) mn = v;
                sum += v;
            }
            m->frac[t] = sum / 4096.0f;
            m->cls[t] = mx < 0.004f ? 0u : mn > 0.98f ? 2u : 1u;
            if (m->cls[t]) nwater++;
            if (m->cls[t] == 2u) nfull++;
        }
        for (u32 l = 1; l < PS2_WATER_LEVELS; l++) {
            u32 s = PS2_WATER_TILE >> l, sp = s * 2u;
            u8 *src = m->mask + zw_level_off(nt, l - 1), *dst = m->mask + zw_level_off(nt, l);
            for (u32 t = 0; t < nt; t++)
                for (u32 y = 0; y < s; y++)
                    for (u32 x = 0; x < s; x++) {
                        const u8 *p = src + (size_t)t * sp * sp + (size_t)(2u * y) * sp + 2u * x;
                        dst[(size_t)t * s * s + (size_t)y * s + x] = (u8)((p[0] + p[1] + p[sp] + p[sp + 1] + 2u) >> 2);
                    }
        }
        for (u32 c = 0; c < 65536u; c++) { u32 t = m->grid[c] & 0x3FFu; if (t < nt) area += m->frac[t]; }
    }
    if (zw_dump[0] && m->mask && nwater) {             /* the water tiles' masks side by side, 32 per row */
        u32 rows = (nwater + 31u) / 32u, W = 32u * 66u, H = rows * 66u, i = 0;
        u8 *img = (u8 *)calloc((size_t)W * H, 1);
        if (img) {
            for (u32 t = 0; t < nt; t++) {
                if (!m->cls[t]) continue;
                u32 ox = (i % 32u) * 66u + 1u, oy = (i / 32u) * 66u + 1u;
                for (u32 y = 0; y < 64u; y++) memcpy(img + (size_t)(oy + y) * W + ox, m->mask + (size_t)t * 4096u + y * 64u, 64);
                i++;
            }
            zw_pgm("masks", m->sig, img, W, H);
            free(img);
        }
        char path[400];                                   /* level 0 of every tile, raw (tools: wt_sea/tools/mapcrop.py) */
        snprintf(path, sizeof path, "%s/water_%08X_level0.bin", zw_dump, m->sig);
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(m->mask, 1, (size_t)nt * 4096u, f); fclose(f); }
    }
    u32 g = 0;
    {   /* publish (the previous map is freed one map later: the renderer is long done with it) */
        g = ++zw_gen;
        m->gen = g;
        m->prev = zw_pub;
        if (m->prev && m->prev->prev) { free(m->prev->prev->mask); free(m->prev->prev); m->prev->prev = NULL; }
        __atomic_store_n(&zw_pub, m, __ATOMIC_RELEASE);
        if (m->sig == zw_sig) zw_valid_from = g;
    }
    ps2_log("water: map %08X (gen %u): %u water colours, %u of %u tiles with water (%u full), %.0f cells of water over the "
            "map (%.0f flagged cells); %.0f ms, %llu fields", m->sig, g, ncol, nwater, nt, nfull, area, nf / 4096.0,
            ((double)clock() - t0) * 1000.0 / CLOCKS_PER_SEC, (unsigned long long)(ps2_kernel_vblank_count() - j->f0));
    if (seed >= 0) ps2_log("water: map %08X: first water colour %u %u %u (palette entry %d)", m->sig, j->pal[seed][0],
                           j->pal[seed][1], j->pal[seed][2], seed);
    free(j->idx);
    free(j);
    zw_busy = 0;
    return NULL;
}

/* ---- the game side (EE thread) ---------------------------------------------------------------------------------- */
static int zw_ptr(u32 p) { return p >= 0x100000u && p < 0x2000000u; }
static u32 zw_tile(u32 T, int cx, int cz) {
    u32 ti = ps2_r32(T + 0x14u), tr = ps2_r32(T + 0x18u);
    if (!zw_ptr(ti) || !zw_ptr(tr)) return 0xFFFFu;
    u32 b = (u32)((cz >> 4) * 16 + (cx >> 4));
    return ps2_r16(tr + ps2_r8(ti + b) * 512u + (u32)((cz & 15) * 16 + (cx & 15)) * 2u);
}
static u32 zw_signature(u32 T) {
    u32 h = 2166136261u;
    const u32 off[6] = { 0x14u, 0x18u, 0x20u, 0x24u, 0x28u, 0xB8u };
    h ^= T; h *= 16777619u;
    for (int k = 0; k < 6; k++) { h ^= ps2_r32(T + off[k]); h *= 16777619u; }
    u32 cl = ps2_r32(T + 0x20u), ti = ps2_r32(T + 0x14u);
    if (zw_ptr(cl)) for (u32 k = 0; k < 1024u; k += 16u) { h ^= ps2_r32(cl + k); h *= 16777619u; }
    if (zw_ptr(ti)) for (u32 k = 0; k < 256u; k += 4u) { h ^= ps2_r32(ti + k); h *= 16777619u; }
    return h ? h : 1u;
}
static void zw_build(void) {
    u32 T = zw_T;
    u32 a0 = ps2_r32(T + 0x24u), a1 = ps2_r32(T + 0x28u), cl = ps2_r32(T + 0x20u);
    u32 nt = (zw_ptr(a0) && zw_ptr(a1) && a1 > a0) ? (a1 - a0) / 4096u : 0u;
    zw_built_sig = zw_sig;
    if (!nt || nt > PS2_WATER_TILES || !zw_ptr(cl)) { ps2_log("water: map %08X: no atlas (%08X %08X %08X)", zw_sig, a0, a1, cl); return; }
    zw_job *j = (zw_job *)calloc(1, sizeof *j);
    zw_map *m = (zw_map *)calloc(1, sizeof *m);
    u8 *idx = (u8 *)malloc((size_t)nt * 4096u);
    if (!j || !m || !idx) { free(j); free(m); free(idx); return; }
    j->m = m; j->idx = idx; j->f0 = ps2_kernel_vblank_count();
    m->sig = zw_sig; m->nt = nt;
    for (int cz = 0; cz < 256; cz++) for (int cx = 0; cx < 256; cx++) m->grid[cz * 256 + cx] = (u16)zw_tile(T, cx, cz);
    for (u32 i = 0; i < 256u; i++) {                      /* CSM1: bits 3 and 4 of the index swapped */
        u32 s = (i & 0xE7u) | ((i & 0x08u) << 1) | ((i & 0x10u) >> 1);
        for (int k = 0; k < 3; k++) j->pal[i][k] = ps2_r8(cl + s * 4u + (u32)k);
    }
    for (u32 k = 0; k < nt * 4096u; k += 4u) { u32 w = ps2_r32(a0 + k); memcpy(idx + k, &w, 4); }
    zw_busy = 1;
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &at, zw_build_thread, j) != 0) {
        ps2_log("water: build thread failed");
        free(m); free(idx); free(j);
        zw_busy = 0;
    }
    pthread_attr_destroy(&at);
}
static int zw_mgr_enter(ps2_ctx *ctx, void *user) {
    (void)user;
    static u64 last_f;
    u64 f = ps2_kernel_vblank_count();
    if (f == last_f) return 0;
    last_f = f;
    u32 mgr = ctx->r[4].uw[0];
    if (!zw_ptr(mgr)) return 0;
    u32 T = ps2_r32(mgr + 4u);
    if (!zw_ptr(T)) return 0;
    u32 sig = zw_signature(T);
    if (sig != zw_sig || T != zw_T) {
        if (zw_dbg && zw_sig) ps2_log("water: map %08X -> %08X at field %llu", zw_sig, sig, (unsigned long long)f);
        zw_sig = sig; zw_T = T; zw_stable = 0;
        const zw_map *m = __atomic_load_n(&zw_pub, __ATOMIC_ACQUIRE);
        zw_valid_from = (m && m->sig == sig) ? m->gen : 0xFFFFFFFFu;
        return 0;
    }
    zw_stable++;
    if (zw_stable >= 30u && zw_built_sig != sig && !zw_busy) zw_build();
    return 0;
}

/* ---- the renderer side ----------------------------------------------------------------------------------------- */
/* GS replay thread, pass 0 of a terrain patch: the cell's tile; for a tile with water the patch's uv mapping (from three
   of its corners, as zero_props.c solves it) into the block */
int zero_water_patch(const unsigned char *recs, unsigned count, const float eye[3], int eye_ok, ps2_vk_mesh_block *b) {
    if (!zw_on || !recs || !b || (count != 10u && count != 4u)) return 0;
    const zw_map *m = __atomic_load_n(&zw_pub, __ATOMIC_ACQUIRE);
    if (!m || m->gen < zw_valid_from || zw_valid_from == 0xFFFFFFFFu || !m->ncol) { zw_st_nomap++; return 0; }
    zw_st_patches++;
    float P[10][4], UV[10][4];
    for (u32 k = 0; k < count; k++) { memcpy(P[k], recs + k * 64u, 16); memcpy(UV[k], recs + k * 64u + 48u, 16); }
    float x0 = P[0][0], z0 = P[0][2], x1 = x0, z1 = z0;
    for (u32 k = 1; k < count; k++) {
        if (P[k][0] < x0) x0 = P[k][0];
        if (P[k][0] > x1) x1 = P[k][0];
        if (P[k][2] < z0) z0 = P[k][2];
        if (P[k][2] > z1) z1 = P[k][2];
    }
    if (!(x1 - x0 > 2000.0f && x1 - x0 < 2100.0f && z1 - z0 > 2000.0f && z1 - z0 < 2100.0f)) return 0;
    int cx = ((int)floorf((x0 + 262144.0f) / 2048.0f + 0.5f)) & 255, cz = ((int)floorf((z0 + 262144.0f) / 2048.0f + 0.5f)) & 255;
    u32 tile = m->grid[cz * 256 + cx], t = tile & 0x3FFu;
    if (t >= m->nt || !m->cls[t]) return 0;
    int i00 = -1, i10 = -1, i01 = -1;
    for (int k = 0; k < (int)count; k++) {
        int lx = P[k][0] > x0 + 1024.0f, lz = P[k][2] > z0 + 1024.0f, mx = fabsf(P[k][0] - x0 - 1024.0f) < 64.0f,
            mz = fabsf(P[k][2] - z0 - 1024.0f) < 64.0f;
        if (mx || mz) continue;
        if (!lx && !lz) i00 = k;
        if (lx && !lz) i10 = k;
        if (!lx && lz) i01 = k;
    }
    if (i00 < 0 || i10 < 0 || i01 < 0) return 0;
    float A[2][2], uv0[2];
    for (int c = 0; c < 2; c++) {
        uv0[c] = UV[i00][c];
        A[c][0] = (UV[i10][c] - UV[i00][c]) / (P[i10][0] - P[i00][0]);
        A[c][1] = (UV[i01][c] - UV[i00][c]) / (P[i01][2] - P[i00][2]);
    }
    float umin = 1e30f, umax = -1e30f, vmin = 1e30f, vmax = -1e30f;
    for (int k = 0; k < 4; k++) {
        float wx = (k & 1) ? 2048.0f : 0.0f, wz = (k & 2) ? 2048.0f : 0.0f;
        float u = uv0[0] + A[0][0] * wx + A[0][1] * wz, v = uv0[1] + A[1][0] * wx + A[1][1] * wz;
        if (u < umin) umin = u;
        if (u > umax) umax = u;
        if (v < vmin) vmin = v;
        if (v > vmax) vmax = v;
    }
    if (!(umax - umin > 1e-7f) || !(vmax - vmin > 1e-7f)) return 0;
    b->e[0] = umin; b->e[1] = vmin; b->e[2] = 1.0f / (umax - umin); b->e[3] = 1.0f / (vmax - vmin);
    b->e[4] = (float)t; b->e[5] = m->frac[t]; b->e[7] = (float)m->cls[t];   /* (e[6]: the terrain pack's, zero_texpack.c) */
    if (eye_ok) { b->e[8] = eye[0]; b->e[9] = eye[1]; b->e[10] = eye[2]; }
    else {                                                 /* (no eye: straight above the patch, far up) */
        b->e[8] = 0.5f * (x0 + x1); b->e[9] = P[0][1] + 20000.0f; b->e[10] = 0.5f * (z0 + z1);
    }
    b->e[11] = 1.0f;
    if (zw_dbg) {                                          /* (the eye every 300 GS frames: heading for the tests) */
        static u32 last = 0xFFFFFFFFu;
        if (eye_ok && (last == 0xFFFFFFFFu || ps2_gs_frame_count - last >= 300u)) {
            last = ps2_gs_frame_count;
            ps2_log("water: GS frame %u eye %.0f %.0f %.0f", ps2_gs_frame_count, eye[0], eye[1], eye[2]);
        }
    }
    zw_st_water++;
    if (m->cls[t] == 2u) zw_st_full++;
    if (zw_dbg >= 2) {
        static int shown;
        if (shown < 4) {
            shown++;
            ps2_log("water: patch cell %d,%d tile %03X class %u frac %.2f: uv0 %.5f %.5f A %.3g %.3g / %.3g %.3g region u "
                    "%.5f..%.5f v %.5f..%.5f eye %.0f %.0f %.0f", cx, cz, t, m->cls[t], m->frac[t], uv0[0], uv0[1],
                    A[0][0], A[0][1], A[1][0], A[1][1], umin, umax, vmin, vmax, b->e[8], b->e[9], b->e[10]);
        }
    }
    return 1;
}

/* The wave layers: one tileable slope texture (ps2_video.c makes it with zero_water_waves) in two layers of different
   size, direction and speed. Deep-water phase speed c = sqrt(g lambda / 2 pi) of each layer's typical wavelength. */
typedef struct { float tile_m, dir_deg, lambda_m, amp; } zw_layer;
static const zw_layer zw_lay[2] = {
    { 200.0f,   0.0f, 16.0f, 0.090f },            /* swell / wind waves: 200 m tile, 4 .. 67 m wavelengths */
    {  37.5f,  38.0f,  3.0f, 0.110f },            /* short waves: 37.5 m tile, 0.8 .. 12.5 m */
};
static float zw_sun[3] = { 0.45f, 0.7f, -0.55f }, zw_sun_ok, zw_sky_top[3] = { 0.14f, 0.20f, 0.29f },
             zw_sky_hor[3] = { 0.45f, 0.49f, 0.57f }, zw_lit[3] = { 0.8f, 0.82f, 0.83f }, zw_dark[3] = { 0.15f, 0.2f, 0.28f },
             zw_band[2] = { 1200.0f, 2400.0f };
static u32 zw_env_frame = 0xFFFFFFFFu;
static void zw_env_update(void) {
#ifdef ZW_C1
    if (zw_env_frame != 0xFFFFFFFFu && ps2_gs_frame_count - zw_env_frame < 30u) return;
    zw_env_frame = ps2_gs_frame_count;
    static ps2_cloud_env e;
    if (!ps2_cloud_env_read(&e)) return;
    if (e.valid & CL_ENV_SUN) {
        float l = sqrtf(e.sun_dir[0] * e.sun_dir[0] + e.sun_dir[1] * e.sun_dir[1] + e.sun_dir[2] * e.sun_dir[2]);
        if (l > 0.5f && l < 1.5f) { for (int k = 0; k < 3; k++) zw_sun[k] = e.sun_dir[k] / l; zw_sun_ok = 1.0f; }
    }
    if (e.valid & CL_ENV_COLOURS) {
        /* the zenith colour of the art capture is not always the sky's (mission 4: 0.137 0.259 0.349 at first, later
           0.075 0.075 0.067): taken only while it is a plausible sky above this horizon, else the horizon deepened */
        float lh = 0.3f * e.sky_hor[0] + 0.59f * e.sky_hor[1] + 0.11f * e.sky_hor[2];
        float lt = 0.3f * e.sky_top[0] + 0.59f * e.sky_top[1] + 0.11f * e.sky_top[2];
        int top_ok = lt >= 0.45f * lh && lt <= 1.3f * lh + 0.02f;
        static const float deep[3] = { 0.62f, 0.78f, 0.92f };
        for (int k = 0; k < 3; k++) {
            zw_sky_hor[k] = e.sky_hor[k]; zw_lit[k] = e.lit[k]; zw_dark[k] = e.shadow[k];
            zw_sky_top[k] = top_ok ? e.sky_top[k] : e.sky_hor[k] * deep[k];
        }
    }
    if ((e.valid & CL_ENV_PROFILE) && e.top_m > e.base_m) { zw_band[0] = e.base_m; zw_band[1] = e.top_m; }
#endif
}
/* the frame's water block (MESH_WATER_BLOCK, terrain_water.glsl reads it) */
void zero_water_frame_block(ps2_vk_mesh_block *w, double time_s) {
    zw_cfg();
    zw_env_update();
    memset(w, 0, sizeof *w);
    const zw_map *m = __atomic_load_n(&zw_pub, __ATOMIC_ACQUIRE);
    float wind = m ? (float)((m->sig >> 8) % 360u) : 0.0f;   /* a fixed wind direction per map */
    for (int l = 0; l < 2; l++) {
        const zw_layer *L = &zw_lay[l];
        float a = (wind + L->dir_deg) * 0.01745329f, ca = cosf(a), sa = sinf(a);
        float tile_m = L->tile_m * zw_scale, lambda_m = L->lambda_m * zw_scale;   /* (ACZ_TERRAIN_WATER_SCALE) */
        float s = 1.0f / (tile_m * ZW_UPM);            /* tiles per unit */
        /* uv = M xz + off: the texture's u axis along the wind; s_world = R s_tex (R = M^T / s) */
        float *M = w->m + 8 * l, *O = w->m + 8 * l + 4, *R = w->r + 4 * l;
        M[0] = ca * s; M[1] = sa * s; M[2] = -sa * s; M[3] = ca * s;
        R[0] = ca; R[1] = -sa; R[2] = sa; R[3] = ca;
        double c = sqrt(9.81 * (double)lambda_m / 6.283185307) * (double)zw_speed;   /* m/s along the wind */
        double d = fmod(time_s * c / (double)tile_m, 1.0);
        O[0] = (float)-d; O[1] = (float)fmod(time_s * 0.013 * (double)zw_speed * (l ? -1.0 : 1.0), 1.0);
        O[2] = L->amp * zw_waves; O[3] = 0.0f;
    }
    float el = zw_sun[1];
    float sun_on = el > 0.02f ? (el > 0.12f ? 1.0f : (el - 0.02f) / 0.1f) : 0.0f;
    for (int k = 0; k < 3; k++) { w->c[k] = zw_sun[k]; w->c[4 + k] = zw_lit[k] * 2.2f * zw_gloss * sun_on;
                                  w->c[8 + k] = zw_sky_top[k]; w->c[12 + k] = zw_sky_hor[k]; }
    w->c[3] = (zw_cost & 2) ? 0.0f : sun_on;
    w->c[7] = 0.0045f;                                  /* unresolved slope variance (capillary waves) */
    w->c[11] = zw_refl;
    w->c[15] = 0.02f;                                   /* F0 of water */
    w->l[0] = zw_fade0 * ZW_UPM; w->l[1] = 1.0f / ((zw_fade1 - zw_fade0) * ZW_UPM); w->l[2] = (float)zw_tint; w->l[3] = (float)zw_cost;
    w->l[4] = zw_band[0] * ZW_UPM; w->l[5] = zw_band[1] * ZW_UPM; w->l[6] = (zw_cost & 1) ? 0.0f : zw_clouds; w->l[7] = 9000.0f * ZW_UPM;
    for (int k = 0; k < 3; k++) w->l[8 + k] = 0.6f * zw_dark[k] + 0.4f * zw_lit[k];   /* a cloud's underside */
    w->l[11] = 1.0f;
    w->r[8] = zw_body;                                  /* (terrain_water.glsl r[2].x: the water body) */
    if (zw_dbg) {
        static u32 last = 0xFFFFFFFFu;
        if (last == 0xFFFFFFFFu || ps2_gs_frame_count - last >= 600u) {
            last = ps2_gs_frame_count;
            ps2_log("water: frame %u time %.2f s: sun %.3f %.3f %.3f (up %.2f), sky top %.3f %.3f %.3f horizon %.3f %.3f %.3f, "
                    "lit %.3f %.3f %.3f, cloud band %.0f..%.0f m, layer offsets %.3f %.3f / %.3f %.3f", ps2_gs_frame_count,
                    time_s, zw_sun[0], zw_sun[1], zw_sun[2], sun_on, zw_sky_top[0], zw_sky_top[1], zw_sky_top[2],
                    zw_sky_hor[0], zw_sky_hor[1], zw_sky_hor[2], zw_lit[0], zw_lit[1], zw_lit[2], zw_band[0], zw_band[1],
                    w->m[4], w->m[5], w->m[12], w->m[13]);
        }
    }
}

/* the wave slope texture: n x n, RGBA8 (s_x, s_z, s_x^2, s_z^2; ps2_water.h) for all levels (n, n/2, .. 1), level by level.
   h = sum of A_i cos(2 pi (k_i . p) + phi_i) over 96 waves with integer wave vectors (tileable), directions spread
   cos^2 about the texture's u axis (1 in 8 from any direction), |k| from 3 to 48 cycles per tile, slope ~ |k|^-0.35;
   normalised to an RMS slope of 1 (the layers scale it). Mips average all four channels, so a filtered texel's
   s^2 - s_mean^2 is the slope variance that its footprint hides (the glitter widens instead of sparkling). */
size_t zero_water_waves(uint8_t *out, uint32_t n) {
    u32 levels = 0;
    for (u32 v = n; v; v >>= 1) levels++;
    float *lv = (float *)malloc(sizeof(float) * 4u * n * n);
    if (!lv) return 0;
    enum { NW = 96 };
    float kx[NW], kz[NW], am[NW], ph[NW];
    u32 rng = 0x9E3779B9u;
#define ZW_RND() (rng ^= rng << 13, rng ^= rng >> 17, rng ^= rng << 5, (float)(rng & 0xFFFFFFu) / 16777216.0f)
    double ssum = 0.0;
    for (int i = 0; i < NW; i++) {
        float kk = 3.0f * powf(16.0f, (float)i / (float)(NW - 1)) * (0.9f + 0.2f * ZW_RND());
        float th;                                       /* cos^2 spread about the wind, 1 in 8 from anywhere (short crests) */
        if ((i & 7) == 7) th = (ZW_RND() * 2.0f - 1.0f) * 3.14159265f;
        else for (;;) { th = (ZW_RND() * 2.0f - 1.0f) * 1.5708f; float c = cosf(th); if (ZW_RND() < c * c) break; }
        kx[i] = roundf(kk * cosf(th)); kz[i] = roundf(kk * sinf(th));
        if (kx[i] == 0.0f && kz[i] == 0.0f) kx[i] = 3.0f;
        float km = sqrtf(kx[i] * kx[i] + kz[i] * kz[i]);
        am[i] = powf(km, -0.35f) / km;                  /* height amplitude; slope = am * 2 pi km */
        ph[i] = ZW_RND() * 6.2831853f;
        double sl = am[i] * 6.2831853 * km;
        ssum += 0.5 * sl * sl;
    }
    float norm = (float)(1.0 / sqrt(ssum));
    for (u32 y = 0; y < n; y++)
        for (u32 x = 0; x < n; x++) {
            float sx = 0.0f, sz = 0.0f, u = ((float)x + 0.5f) / (float)n, v = ((float)y + 0.5f) / (float)n;
            for (int i = 0; i < NW; i++) {
                float a = 6.2831853f * (kx[i] * u + kz[i] * v) + ph[i], s = -sinf(a) * am[i] * 6.2831853f * norm;
                sx += s * kx[i]; sz += s * kz[i];
            }
            float *o = lv + 4u * (y * n + x);
            o[0] = sx; o[1] = sz; o[2] = sx * sx; o[3] = sz * sz;
        }
#undef ZW_RND
    size_t at = 0;
    for (u32 l = 0, s = n; l < levels; l++, s >>= 1) {
        if (l) {                                        /* 2 x 2 average in place (the previous level's top-left part) */
            for (u32 y = 0; y < s; y++)
                for (u32 x = 0; x < s; x++)
                    for (int c = 0; c < 4; c++) {
                        u32 sp = s * 2u;
                        float a = lv[4u * ((2u * y) * sp + 2u * x) + (u32)c] + lv[4u * ((2u * y) * sp + 2u * x + 1u) + (u32)c]
                                + lv[4u * ((2u * y + 1u) * sp + 2u * x) + (u32)c] + lv[4u * ((2u * y + 1u) * sp + 2u * x + 1u) + (u32)c];
                        lv[4u * (y * s + x) + (u32)c] = 0.25f * a;   /* (row y < 2y: rows read before written) */
                    }
        }
        for (u32 i = 0; i < 4u * s * s; i++) {         /* RGBA8: slopes / 8 + 0.5, squares / 16 */
            float v = (i & 2u) ? lv[i] * (1.0f / 16.0f) : lv[i] * 0.125f + 0.5f;
            long q = lrintf(v * 255.0f);
            out[at + i] = (u8)(q < 0 ? 0 : q > 255 ? 255 : q);
        }
        at += 4u * (size_t)s * s;
    }
    free(lv);
    return at;
}

void zero_water_report(void) {
    { extern void zero_texpack_report(void); zero_texpack_report(); }      /* (T5 terrain pack: its counters too) */
    if (zw_st_patches || zw_st_nomap)
        ps2_log("water: %llu terrain patches looked up, %llu with water (%llu full tiles), %llu before a mask was ready",
                (unsigned long long)zw_st_patches, (unsigned long long)zw_st_water, (unsigned long long)zw_st_full,
                (unsigned long long)zw_st_nomap);
}

static int zw_hook_in;
/* the NEXT MISSION tier (ps2_menv.h): the game thread, at the edge. Off: the patches keep their plain look (zw_on gates the
   marking per patch); the masks and images stay for the way back. */
void zero_water_menv_reload(void) {
    if (!zw_hook_in) return;
    const int a = zw_on;
    const char *e = ps2_menv("ACZ_TERRAIN_WATER");
    zw_on = !(e && *e == '0');
    ps2_menv_note("ACZ_TERRAIN_WATER", a, zw_on);
}
void zero_water_init(void) {
    { extern void zero_texpack_init(void); zero_texpack_init(); }          /* (T5 terrain pack: the same map hook point) */
    zw_cfg();
    if (!zw_on) { if (zw_dbg) ps2_log("water: shader water off (ACZ_TERRAIN_WATER)"); return; }
    u32 w = ps2_r32(ZW_MGR);
    if ((w >> 16) != 0x27BDu || ps2_hook_before(ZW_MGR, zw_mgr_enter, NULL, 58, "zero-water") < 0) {
        ps2_log("water: terrain manager hook failed (code %08X): shader water off", w);
        zw_on = 0;
        return;
    }
    zw_hook_in = 1;
    ps2_log("water: shader water on (ACZ_TERRAIN_WATER): waves %.2f (scale %.2f: tiles %.0f / %.1f m) gloss %.2f reflection %.2f "
            "body %.2f clouds %.2f speed %.2f, fade %.0f..%.0f m%s%s", zw_waves, zw_scale, zw_lay[0].tile_m * zw_scale,
            zw_lay[1].tile_m * zw_scale, zw_gloss, zw_refl, zw_body, zw_clouds, zw_speed, zw_fade0, zw_fade1,
            zw_ab_n ? ", in-run GPU A/B (ACZ_TERRAIN_WATER_AB)" : "", zw_tint ? ", debug tint" : "");
}
