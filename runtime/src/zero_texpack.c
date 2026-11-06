/* zero_texpack.c -- T5 terrain texture pack (realistic terrain stage T5; ps2_texpack.h, ACZ_TERRAIN_TEXPACK; offline
   pipeline tools/terrain/t5_pack.py; research docs/research/2026-10-24/terrain-upscale-t5.md).

   What it replaces: the terrain's first pass samples the atlas tile of its cell (64 x 64 PSMT8 texels of 5.3 m, CLUT
   T+0x20, atlas T+0x24, the tile grid of zero_terrain.c) -- the 2006 ground photo, magnified 10-60x below 1 km. A pack
   holds every tile of a tile set at 128 x 128, upscaled offline in the tile's real 3 x 3 world neighbourhoods (averaged
   per tile id: no seams from missing context) and back-projected: every 2 x 2 box of a pack tile equals the 2006 texel
   bit for bit, so the pack's mip 1 is the 2006 tile and its mips 2.. are the box mips of it -- the image changes only
   where a 2006 texel covers more than about one pixel (within ~3-4 km in the headset) and is the game's beyond. Patches
   the game draws with its own mip level 1 (TEX1 K: far patches) keep the game's texture.

   Flow (the shader water's, zero_water.c): the EE thread (a hook before the terrain manager 0x15E1D8) copies the atlas,
   the palette and the grid once a map has been stable for 30 frames; a lowest-priority worker computes the set's
   content key (CRC-32 of the atlas bytes followed by the 1 KB palette, as tools/terrain/t5lib.py does), loads
   ACZ_TERRAIN_TEXPACK_DIR/<name>/t5_<CRC>_<nt>.png (one sheet, 32 tiles of 128 x 128 per row, RGBA; SDL_LoadPNG), builds
   the box mips ((a + b + c + d + 2) >> 2) and writes all levels into the renderer's staging buffer (one buffer of 85 MiB,
   host-visible, made at start when the feature is on), then publishes the map's grid. The renderer (ps2_video.c tp_*)
   copies the staging buffer into an RGBA8 2D array (layer = tile id) in chunks of ACZ_TERRAIN_TEXPACK_CHUNK layers per
   frame, and applies the pack once all of it is there and it belongs to the loaded map. rn_vp_terrain.c calls
   zero_texpack_patch() for every terrain patch's first pass: it finds the patch's cell and tile and solves the
   tile-local uv from the patch's corners (as zero_water_patch does) into the block; mesh.vert hands it to mesh.frag
   (v_wat.xyz) and the -DTEXPACK variant fetches the pack there instead of the GS region texture (implicit derivatives,
   trilinear, the anisotropy of the game's terrain textures, clamp to the tile). Another map signature of the same set
   (mission 4's three) reuses the uploaded pack.

   A 4x pack (gr_mush 2026-10-28, tools/terrain/t5_pack4.py; the folder `x4`, ENHANCEMENTS GROUND TEXTURES: REAL 4X): the
   same at 256 x 256 (1.33 m texels; 9 levels, level 2 = the 2006 tile), its sheet 8192 wide -- the tile side comes from
   the folder's first sheet at start (tp_cfg), the renderer's image is made with it (1024 layers: 341 MiB of VRAM and the
   same host staging); 32 layers a frame by default.

   Switches: ACZ_TERRAIN_TEXPACK (default off; `faithful`, `crisp` or any pack folder name; `1` = faithful; `0` / `off` /
   `game` = off: no hook, no images, no variant, the game's texture), _DIR (folder of the pack folders, default
   out/terrain_t5), _BIAS (mip bias, default 0), _MINLOD (the finest level sampled, default 0; 1 = the 2006 texels
   everywhere: with a nearest-neighbour pack the image must be the game's -- the alignment check), _ANISO (0 =
   as the game's terrain textures), _CHUNK (layers per frame, 128), _AB=N (in-run GPU A/B, ps2_video.c),
   PS2_PERF_AB_FLIP=texpack (exact same-frame image A/B). Debug: ACZ_DEBUG_TEXPACK=1 (log; 2 = the first patches' uv
   solve against the GS clamp window), ACZ_DEBUG_TEXPACK_TINT=1 (patches the pack serves tinted magenta). */
#include "ps2_runtime.h"
#include "ps2_hook.h"
#include "ps2_texpack.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

u64 ps2_kernel_vblank_count(void);
extern u32 ps2_gs_frame_count;

#define TP_MGR 0x0015E1D8u                 /* terrain manager: a0 = manager, T = *(a0 + 4) */
#define TP_NONE 0xFFFFFFFFu

static int tp_cfg_done, tp_on, tp_dbg, tp_tint, tp_ab_n, tp_chunk = 128;
static u32 tp_side = PS2_TEXPACK_TILE, tp_nlev = PS2_TEXPACK_LEVELS;   /* (a 4x pack: 256, 9) */
static float tp_bias = 0.0f, tp_aniso = 0.0f, tp_minlod = 0.0f;
static char tp_name[64], tp_dir[300];

static float env_f(const char *n, float def, float lo, float hi) {
    const char *e = getenv(n);
    if (!e || !*e) return def;
    float v = (float)atof(e);
    return v < lo ? lo : v > hi ? hi : v;
}
static void tp_cfg(void) {
    if (tp_cfg_done) return;
    tp_cfg_done = 1;
    const char *e = getenv("ACZ_TERRAIN_TEXPACK");     /* default off (until the headset verdict) */
    tp_on = 0;
    if (e && *e && strcmp(e, "0") && strcmp(e, "off") && strcmp(e, "game")) {
        const char *n = strcmp(e, "1") ? e : "faithful";
        size_t k = 0;
        for (; n[k] && k + 1 < sizeof tp_name; k++) {        /* a folder name: letters, digits, '_', '-' only */
            char c = n[k];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) break;
            tp_name[k] = c;
        }
        tp_name[k] = 0;
        if (k && !n[k]) tp_on = 1;
        else ps2_log("texpack: ACZ_TERRAIN_TEXPACK=%s is not a pack folder name: off", e);
    }
    e = getenv("ACZ_TERRAIN_TEXPACK_DIR");
    snprintf(tp_dir, sizeof tp_dir, "%s", e && *e ? e : "out/terrain_t5");
    tp_bias = env_f("ACZ_TERRAIN_TEXPACK_BIAS", 0.0f, -2.0f, 4.0f);
    tp_aniso = env_f("ACZ_TERRAIN_TEXPACK_ANISO", 0.0f, 0.0f, 16.0f);
    tp_minlod = env_f("ACZ_TERRAIN_TEXPACK_MINLOD", 0.0f, 0.0f, 7.0f);
    /* the pack's tile side from its first sheet's PNG header (width 4096 = 32 tiles of 128: a 2x pack; 8192 = 32 of 256:
       a 4x pack, tools/terrain/t5_pack4.py); the renderer's image is made with it at start */
    if (tp_on) {
        char pat[400], first[400];
        first[0] = 0;
#ifdef _WIN32
        WIN32_FIND_DATAW fd;
        wchar_t wpat[400];
        char nm[260];
        snprintf(pat, sizeof pat, "%s/%s/t5_*.png", tp_dir, tp_name);
        HANDLE fh = ps2_path_wide(pat, wpat, 400) ? FindFirstFileW(wpat, &fd) : INVALID_HANDLE_VALUE;
        if (fh != INVALID_HANDLE_VALUE) {
            if (WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, nm, sizeof nm, NULL, NULL) > 0)
                snprintf(first, sizeof first, "%s/%s/%s", tp_dir, tp_name, nm);
            FindClose(fh);
        }
#else
        (void)pat;
#endif
        FILE *f = first[0] ? fopen(first, "rb") : NULL;
        u8 h[24];
        if (f && fread(h, 1, 24, f) == 24 && h[1] == 'P' && h[2] == 'N' && h[3] == 'G') {
            u32 w = ((u32)h[16] << 24) | ((u32)h[17] << 16) | ((u32)h[18] << 8) | h[19];
            if (w == 32u * 256u) { tp_side = 256u; tp_nlev = 9u; }
        }
        if (f) fclose(f);
    }
    tp_chunk = (int)env_f("ACZ_TERRAIN_TEXPACK_CHUNK", (float)(128u * PS2_TEXPACK_TILE / tp_side * PS2_TEXPACK_TILE / tp_side),
                          8.0f, 1024.0f);   /* (default ~11 MB of copies a frame: 128 layers of a 2x pack, 32 of a 4x) */
    tp_ab_n = (int)env_f("ACZ_TERRAIN_TEXPACK_AB", 0.0f, 0.0f, 100000.0f);
    e = getenv("ACZ_DEBUG_TEXPACK");
    tp_dbg = e ? atoi(e) : 0;
    e = getenv("ACZ_DEBUG_TEXPACK_TINT");
    tp_tint = e ? atoi(e) : 0;
}
int zero_texpack_on(void) { tp_cfg(); return tp_on; }
float zero_texpack_bias(void) { tp_cfg(); return tp_bias; }
float zero_texpack_minlod(void) { tp_cfg(); return tp_minlod; }
float zero_texpack_aniso(void) { tp_cfg(); return tp_aniso; }
int zero_texpack_ab_n(void) { tp_cfg(); return tp_ab_n; }
int zero_texpack_tint(void) { tp_cfg(); return tp_tint; }
int zero_texpack_chunk(void) { tp_cfg(); return tp_chunk; }
uint32_t zero_texpack_tile(void) { tp_cfg(); return tp_side; }
uint32_t zero_texpack_levels(void) { tp_cfg(); return tp_nlev; }

size_t zero_texpack_bytes(uint32_t nt) {
    tp_cfg();
    size_t o = 0;
    for (u32 l = 0; l < tp_nlev; l++) { size_t s = tp_side >> l; o += (size_t)nt * s * s * 4u; }
    return o;
}
static size_t tp_level_off(u32 nt, u32 l) {
    size_t o = 0;
    for (u32 k = 0; k < l; k++) { size_t s = tp_side >> k; o += (size_t)nt * s * s * 4u; }
    return o;
}

/* CRC-32 (zlib's: reflected 0xEDB88320; crc32(b, crc32(a)) = crc32(a + b)) */
static u32 tp_crc_tab[256];
static u32 tp_crc32(u32 crc, const u8 *p, size_t n) {
    if (!tp_crc_tab[1])
        for (u32 i = 0; i < 256u; i++) {
            u32 c = i;
            for (int k = 0; k < 8; k++) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            tp_crc_tab[i] = c;
        }
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = tp_crc_tab[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

/* ---- published state ------------------------------------------------------------------------------------------ */
typedef struct tp_map {
    u32 sig, nt, key, pack_gen;
    int ok;                                /* a pack serves this map */
    u16 grid[65536];                       /* tile id + flags per cell, rows by z */
    struct tp_map *prev;
} tp_map;
static tp_map *volatile tp_pub;            /* the newest built map (GS thread reads) */
static volatile u32 tp_valid_from = TP_NONE;   /* the pack generation of the loaded map */
static volatile int tp_busy;
static u32 tp_sig, tp_stable, tp_built_sig, tp_T;
static u64 tp_st_patches, tp_st_served, tp_st_lod, tp_st_nomap, tp_st_nosolve;

/* the staging buffer (renderer-owned memory; the worker writes a pack while it is free) */
static u8 *tp_stg;
static size_t tp_stg_cap;
static volatile int tp_stg_state;          /* 0 free, 1 being filled, 2 waiting for the renderer's copies */
static volatile u32 tp_stg_gen, tp_stg_nt, tp_stg_key;
static volatile size_t tp_stg_bytes;
static volatile u32 tp_up_gen, tp_up_key;  /* the pack in the image (after its copies) */
static u32 tp_pack_gen;

void zero_texpack_staging(void *ptr, size_t cap) { tp_stg = (u8 *)ptr; tp_stg_cap = cap; }
uint32_t zero_texpack_pending(uint32_t *nt, size_t *bytes) {
    if (__atomic_load_n(&tp_stg_state, __ATOMIC_ACQUIRE) != 2) return 0;
    *nt = tp_stg_nt; *bytes = tp_stg_bytes;
    return tp_stg_gen;
}
void zero_texpack_uploaded(uint32_t gen) {
    tp_up_key = tp_stg_key;
    __atomic_store_n(&tp_up_gen, gen, __ATOMIC_RELEASE);
    __atomic_store_n(&tp_stg_state, 0, __ATOMIC_RELEASE);
}
uint32_t zero_texpack_valid_from(void) { return tp_valid_from; }

/* ---- the worker ---------------------------------------------------------------------------------------------------- */
typedef struct { tp_map *m; u8 *atlas; u8 pal[1024]; u64 f0; } tp_job;

static void tp_publish(tp_map *m) {
    m->prev = tp_pub;
    if (m->prev && m->prev->prev) { free(m->prev->prev); m->prev->prev = NULL; }   /* (two maps later: long unused) */
    __atomic_store_n(&tp_pub, m, __ATOMIC_RELEASE);
    if (m->sig == tp_sig) tp_valid_from = m->ok ? m->pack_gen : TP_NONE;
}

/* the pack of nt tiles from the sheet into the staging buffer (all levels); 0 = failed */
static int tp_fill(const u8 *px, int pitch, u32 nt) {
    const u32 S = tp_side, H = S / 2u, rb = S * 4u;        /* (tile side, level-1 side, bytes per tile row) */
    size_t s1 = (size_t)nt * H * H * 4u;
    u8 *a = (u8 *)malloc(s1), *b = (u8 *)malloc(s1 / 4u);
    if (!a || !b) { free(a); free(b); return 0; }
    /* level 0: rows of the sheet; level 1 from it at the same time (cached source, write-combined target) */
    u8 *dst = tp_stg;
    for (u32 t = 0; t < nt; t++) {
        const u8 *tl = px + (size_t)(t / 32u) * S * (size_t)pitch + (size_t)(t % 32u) * rb;
        u8 *o = dst + (size_t)t * S * rb;
        for (u32 y = 0; y < S; y++) memcpy(o + (size_t)y * rb, tl + (size_t)y * (size_t)pitch, rb);
        u8 *m1 = a + (size_t)t * H * H * 4u;
        for (u32 y = 0; y < H; y++) {
            const u8 *r0 = tl + (size_t)(2u * y) * (size_t)pitch, *r1 = r0 + pitch;
            for (u32 x = 0; x < H; x++)
                for (u32 c = 0; c < 4u; c++)
                    m1[(y * H + x) * 4u + c] = (u8)((r0[8u * x + c] + r0[8u * x + 4u + c] + r1[8u * x + c]
                                                      + r1[8u * x + 4u + c] + 2u) >> 2);
        }
    }
    u8 *cur = a, *nxt = b;
    for (u32 l = 1; l < tp_nlev; l++) {
        u32 s = S >> l;
        memcpy(dst + tp_level_off(nt, l), cur, (size_t)nt * s * s * 4u);
        if (l + 1u < tp_nlev) {
            u32 h = s / 2u;
            for (u32 t = 0; t < nt; t++) {
                const u8 *src = cur + (size_t)t * s * s * 4u;
                u8 *o = nxt + (size_t)t * h * h * 4u;
                for (u32 y = 0; y < h; y++)
                    for (u32 x = 0; x < h; x++)
                        for (u32 c = 0; c < 4u; c++) {
                            const u8 *p = src + ((2u * y) * s + 2u * x) * 4u + c;
                            o[(y * h + x) * 4u + c] = (u8)((p[0] + p[4] + p[s * 4u] + p[s * 4u + 4u] + 2u) >> 2);
                        }
            }
            u8 *sw = cur; cur = nxt; nxt = sw;
        }
    }
    free(a); free(b);
    return 1;
}

static void *tp_build_thread(void *arg) {
    tp_job *j = (tp_job *)arg;
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    { extern void ps2_host_thread_start(const char *); ps2_host_thread_start("bg"); }  /* (PS2_THREAD_CORES=h) */
#endif
    tp_map *m = j->m;
    u32 nt = m->nt;
    double t0 = (double)clock();
    u32 key = tp_crc32(0u, j->atlas, (size_t)nt * 4096u);
    key = tp_crc32(key, j->pal, 1024u);
    m->key = key;
    char path[420];
    snprintf(path, sizeof path, "%s/%s/t5_%08X_%u.png", tp_dir, tp_name, key, nt);
    if (tp_up_gen && tp_up_key == key && __atomic_load_n(&tp_stg_state, __ATOMIC_ACQUIRE) == 0) {
        m->ok = 1; m->pack_gen = tp_up_gen;                 /* (the same set is in the image: another signature) */
        ps2_log("texpack: map %08X: set t5_%08X_%u already uploaded (generation %u)", m->sig, key, nt, m->pack_gen);
        goto done;
    }
    {
        double tl0 = (double)clock();
        SDL_Surface *s = SDL_LoadPNG(path), *c = NULL;
        if (!s) {
            ps2_log("texpack: map %08X: no pack %s (%s): the game's ground texture", m->sig, path, SDL_GetError());
            goto done;
        }
        c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(s);
        u32 rows = (nt + 31u) / 32u;
        if (!c || c->w != (int)(32u * tp_side) || (u32)c->h < rows * tp_side) {
            ps2_log("texpack: %s: %dx%d, not %u x %u: skipped", path, c ? c->w : 0, c ? c->h : 0, 32u * tp_side,
                    rows * tp_side);
            if (c) SDL_DestroySurface(c);
            goto done;
        }
        double tl1 = (double)clock();
        size_t bytes = zero_texpack_bytes(nt);
        if (!tp_stg || bytes > tp_stg_cap) {
            ps2_log("texpack: no staging buffer for %zu bytes", bytes);
            SDL_DestroySurface(c);
            goto done;
        }
        for (int w = 0; __atomic_load_n(&tp_stg_state, __ATOMIC_ACQUIRE) != 0; w++) {   /* (a previous upload) */
            if (w > 6000) { ps2_log("texpack: staging still busy after 60 s"); SDL_DestroySurface(c); goto done; }
#ifdef _WIN32
            Sleep(10);
#else
            struct timespec ts = { 0, 10000000 }; nanosleep(&ts, NULL);
#endif
        }
        __atomic_store_n(&tp_stg_state, 1, __ATOMIC_RELEASE);
        int ok = tp_fill((const u8 *)c->pixels, c->pitch, nt);
        SDL_DestroySurface(c);
        if (!ok) { __atomic_store_n(&tp_stg_state, 0, __ATOMIC_RELEASE); ps2_log("texpack: out of memory"); goto done; }
        tp_stg_gen = ++tp_pack_gen; tp_stg_nt = nt; tp_stg_bytes = bytes; tp_stg_key = key;
        m->ok = 1; m->pack_gen = tp_stg_gen;
        __atomic_store_n(&tp_stg_state, 2, __ATOMIC_RELEASE);
        ps2_log("texpack: map %08X: pack %s (%u tiles, %.1f MB with mips): PNG %.0f ms, mips + staging %.0f ms",
                m->sig, path, nt, (double)bytes / 1048576.0, (tl1 - tl0) * 1000.0 / CLOCKS_PER_SEC,
                ((double)clock() - tl1) * 1000.0 / CLOCKS_PER_SEC);
    }
done:
    tp_publish(m);
    if (tp_dbg) ps2_log("texpack: map %08X key %08X: %s, %.0f ms, %llu fields", m->sig, key, m->ok ? "pack" : "no pack",
                        ((double)clock() - t0) * 1000.0 / CLOCKS_PER_SEC,
                        (unsigned long long)(ps2_kernel_vblank_count() - j->f0));
    free(j->atlas);
    free(j);
    tp_busy = 0;
    return NULL;
}

/* ---- the game side (EE thread) ---------------------------------------------------------------------------------- */
static int tp_ptr(u32 p) { return p >= 0x100000u && p < 0x2000000u; }
static u32 tp_tile(u32 T, int cx, int cz) {
    u32 ti = ps2_r32(T + 0x14u), tr = ps2_r32(T + 0x18u);
    if (!tp_ptr(ti) || !tp_ptr(tr)) return 0xFFFFu;
    u32 b = (u32)((cz >> 4) * 16 + (cx >> 4));
    return ps2_r16(tr + ps2_r8(ti + b) * 512u + (u32)((cz & 15) * 16 + (cx & 15)) * 2u);
}
static u32 tp_signature(u32 T) {                           /* (zero_water.c's) */
    u32 h = 2166136261u;
    const u32 off[6] = { 0x14u, 0x18u, 0x20u, 0x24u, 0x28u, 0xB8u };
    h ^= T; h *= 16777619u;
    for (int k = 0; k < 6; k++) { h ^= ps2_r32(T + off[k]); h *= 16777619u; }
    u32 cl = ps2_r32(T + 0x20u), ti = ps2_r32(T + 0x14u);
    if (tp_ptr(cl)) for (u32 k = 0; k < 1024u; k += 16u) { h ^= ps2_r32(cl + k); h *= 16777619u; }
    if (tp_ptr(ti)) for (u32 k = 0; k < 256u; k += 4u) { h ^= ps2_r32(ti + k); h *= 16777619u; }
    return h ? h : 1u;
}
static void tp_build(void) {
    u32 T = tp_T;
    u32 a0 = ps2_r32(T + 0x24u), a1 = ps2_r32(T + 0x28u), cl = ps2_r32(T + 0x20u);
    u32 nt = (tp_ptr(a0) && tp_ptr(a1) && a1 > a0) ? (a1 - a0) / 4096u : 0u;
    tp_built_sig = tp_sig;
    if (!nt || nt > PS2_TEXPACK_TILES || !tp_ptr(cl)) { ps2_log("texpack: map %08X: no atlas (%08X %08X %08X)", tp_sig, a0, a1, cl); return; }
    tp_job *j = (tp_job *)calloc(1, sizeof *j);
    tp_map *m = (tp_map *)calloc(1, sizeof *m);
    u8 *atlas = (u8 *)malloc((size_t)nt * 4096u);
    if (!j || !m || !atlas) { free(j); free(m); free(atlas); return; }
    j->m = m; j->atlas = atlas; j->f0 = ps2_kernel_vblank_count();
    m->sig = tp_sig; m->nt = nt;
    for (int cz = 0; cz < 256; cz++) for (int cx = 0; cx < 256; cx++) m->grid[cz * 256 + cx] = (u16)tp_tile(T, cx, cz);
    for (u32 k = 0; k < 1024u; k += 4u) { u32 w = ps2_r32(cl + k); memcpy(j->pal + k, &w, 4); }   /* (as in memory) */
    for (u32 k = 0; k < nt * 4096u; k += 4u) { u32 w = ps2_r32(a0 + k); memcpy(atlas + k, &w, 4); }
    tp_busy = 1;
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &at, tp_build_thread, j) != 0) {
        ps2_log("texpack: build thread failed");
        free(m); free(atlas); free(j);
        tp_busy = 0;
    }
    pthread_attr_destroy(&at);
}
static int tp_mgr_enter(ps2_ctx *ctx, void *user) {
    (void)user;
    static u64 last_f;
    u64 f = ps2_kernel_vblank_count();
    if (f == last_f) return 0;
    last_f = f;
    u32 mgr = ctx->r[4].uw[0];
    if (!tp_ptr(mgr)) return 0;
    u32 T = ps2_r32(mgr + 4u);
    if (!tp_ptr(T)) return 0;
    u32 sig = tp_signature(T);
    if (sig != tp_sig || T != tp_T) {
        if (tp_dbg && tp_sig) ps2_log("texpack: map %08X -> %08X at field %llu", tp_sig, sig, (unsigned long long)f);
        tp_sig = sig; tp_T = T; tp_stable = 0;
        const tp_map *m = __atomic_load_n(&tp_pub, __ATOMIC_ACQUIRE);
        tp_valid_from = (m && m->sig == sig && m->ok) ? m->pack_gen : TP_NONE;
        return 0;
    }
    tp_stable++;
    if (tp_stable >= 30u && tp_built_sig != sig && !tp_busy) tp_build();
    return 0;
}

/* ---- the renderer side ----------------------------------------------------------------------------------------- */
/* GS replay thread, pass 0 of a terrain patch: the cell's tile and the patch's tile uv (from three of its corners) */
int zero_texpack_patch(const unsigned char *recs, unsigned count, const ps2_vk_state *st, ps2_vk_mesh_block *b) {
    if (!tp_on || !recs || !b || (count != 10u && count != 4u)) return 0;
    const tp_map *m = __atomic_load_n(&tp_pub, __ATOMIC_ACQUIRE);
    u32 vf = tp_valid_from;
    if (!m || !m->ok || vf == TP_NONE || m->pack_gen != vf || m->sig != tp_sig) { tp_st_nomap++; return 0; }
    tp_st_patches++;
    if (st && st->tex_lod) { tp_st_lod++; return 0; }      /* (the game's own mip level 1: its texture stays) */
    float P[10][4], UV[10][4];
    for (u32 k = 0; k < count; k++) { memcpy(P[k], recs + k * 64u, 16); memcpy(UV[k], recs + k * 64u + 48u, 16); }
    float x0 = P[0][0], z0 = P[0][2], x1 = x0, z1 = z0;
    for (u32 k = 1; k < count; k++) {
        if (P[k][0] < x0) x0 = P[k][0];
        if (P[k][0] > x1) x1 = P[k][0];
        if (P[k][2] < z0) z0 = P[k][2];
        if (P[k][2] > z1) z1 = P[k][2];
    }
    if (!(x1 - x0 > 2000.0f && x1 - x0 < 2100.0f && z1 - z0 > 2000.0f && z1 - z0 < 2100.0f)) { tp_st_nosolve++; return 0; }
    int cx = ((int)floorf((x0 + 262144.0f) / 2048.0f + 0.5f)) & 255, cz = ((int)floorf((z0 + 262144.0f) / 2048.0f + 0.5f)) & 255;
    u32 t = m->grid[cz * 256 + cx] & 0x3FFu;
    if (t >= m->nt) { tp_st_nosolve++; return 0; }
    int i00 = -1, i10 = -1, i01 = -1;
    for (int k = 0; k < (int)count; k++) {
        int lx = P[k][0] > x0 + 1024.0f, lz = P[k][2] > z0 + 1024.0f, mx = fabsf(P[k][0] - x0 - 1024.0f) < 64.0f,
            mz = fabsf(P[k][2] - z0 - 1024.0f) < 64.0f;
        if (mx || mz) continue;
        if (!lx && !lz) i00 = k;
        if (lx && !lz) i10 = k;
        if (!lx && lz) i01 = k;
    }
    if (i00 < 0 || i10 < 0 || i01 < 0) { tp_st_nosolve++; return 0; }
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
    if (!(umax - umin > 1e-7f) || !(vmax - vmin > 1e-7f)) { tp_st_nosolve++; return 0; }
    b->e[0] = umin; b->e[1] = vmin; b->e[2] = 1.0f / (umax - umin); b->e[3] = 1.0f / (vmax - vmin);
    b->e[4] = (float)t; b->e[6] = 1.0f;
    tp_st_served++;
    if (tp_dbg >= 2 && st) {                               /* the solve against the GS clamp window (texels) */
        static int shown;
        if (shown < 12) {
            shown++;
            ps2_log("texpack: patch cell %d,%d tile %03X n%u: tile u %.3f..%.3f v %.3f..%.3f texels (+zw %.3f %.3f) of "
                    "%.0fx%.0f, GS clamp wms %u u %u..%u v %u..%u region %d lod %u", cx, cz, t, count,
                    umin * st->tex_w, umax * st->tex_w, vmin * st->tex_h, vmax * st->tex_h, UV[i00][2] * st->tex_w,
                    UV[i00][3] * st->tex_h, st->tex_w, st->tex_h, st->wms, st->minu, st->maxu, st->minv, st->maxv,
                    st->native_region, st->tex_lod);
        }
    }
    return 1;
}

void zero_texpack_report(void) {
    if (tp_st_patches || tp_st_nomap)
        ps2_log("texpack: %llu terrain patches looked up with a pack, %llu served (%llu at the game's mip level 1, %llu "
                "not solved), %llu before a pack was ready", (unsigned long long)tp_st_patches,
                (unsigned long long)tp_st_served, (unsigned long long)tp_st_lod, (unsigned long long)tp_st_nosolve,
                (unsigned long long)tp_st_nomap);
}

void zero_texpack_init(void) {
    tp_cfg();
    if (!tp_on) { if (tp_dbg) ps2_log("texpack: off (ACZ_TERRAIN_TEXPACK)"); return; }
    u32 w = ps2_r32(TP_MGR);
    if ((w >> 16) != 0x27BDu || ps2_hook_before(TP_MGR, tp_mgr_enter, NULL, 59, "zero-texpack") < 0) {
        ps2_log("texpack: terrain manager hook failed (code %08X): off", w);
        tp_on = 0;
        return;
    }
    ps2_log("texpack: terrain texture pack '%s' from %s (ACZ_TERRAIN_TEXPACK): %ux%u tiles (%ux), mip bias %.2f, lowest level "
            "%.2f, anisotropy %s%.0f, %d layers per frame%s%s", tp_name, tp_dir, tp_side, tp_side, tp_side / 64u, tp_bias,
            tp_minlod, tp_aniso > 0.0f ? "" : "as the game's terrain ", tp_aniso,
            tp_chunk, tp_ab_n ? ", in-run GPU A/B (ACZ_TERRAIN_TEXPACK_AB)" : "", tp_tint ? ", debug tint" : "");
}
