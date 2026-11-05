/* zero_props.c -- towns, villages and bases (wishes #49 / #55 / #78, TOWNS_RESEARCH_2026-10-23.md): a research probe of
   the game's static map objects (ACZ_DEBUG_PROPS) and houses raised from the red roofs of the ground photo
   (ACZ_TERRAIN_PROPS=1, default off; stage 1 below), and from the desert / industrial buildings and round tanks found by
   their shadows (ACZ_TERRAIN_PROPS=2: stage 2, zero_props_find.h; bright desert maps only: mission 4), and the dense
   houses of the city photos (ACZ_TERRAIN_CITY with any ACZ_TERRAIN_PROPS: stage 3, zero_props_city.h; mission 6's
   Directus), never near a ground unit (target) of the mission.

   The game's data (SLUS-21346, read from 0x15E1D8 / 0x15FA80 / 0x15F758 / 0x15F1D8 / 0x15F2A8 / 0x15ECE0 / 0x15F4D0):
   the terrain manager (a0 of 0x15E1D8) holds the terrain object T = *(mgr+4) and the map-object manager O = *(mgr+8):
     O+0x10 T; O+0x14 s16 [256 x 256] cell -> list index (0 = none), cell = 2048 units from (-262144, -262144), rows by z;
     O+0x18 lists: 16 bytes each, up to 8 s16 instance indices, -1 ends; O+0x20 instance count; O+0x24 instances,
     16 bytes: float x, y, z, u32 (model index in the low 19 bits, bits 19..31 = drawn-this-frame marks per wrap copy);
     O+0x28 model count; O+0x2C models, 0x90 bytes: +0x08 model data (collision data at +0x40 from version 3), +0x40 ->
     bounds {radius, ymax, xmin, xmax, ymin, ymax, zmin, zmax}, +0x44 bit 0 (closing tag 0x1C / 0x1D), +0x48 the
     drawable (0x134200), +0x6C -> s8 texture-group list (-1 ends, <= 16) uploaded once per frame by 0x15F6C8.
   Per block (13 x 13 around the camera) 0x15FA80 visits the 8 x 8 cells (all for a fully visible block, else the cells
   whose scratchpad flag 0x70000100 + row*0x20 + col*4 is 0) and 0x15F758 draws each listed instance once per frame:
   identity rotation with the instance position as translation, texture groups, tag 0x1B, model draw 0x133A08, tag
   0x1C/0x1D; no distance LOD. 0x15F1D8 / 0x15F2A8 give the tallest object per cell / per block (T+0x350) for the block
   culling bounds. The objects are solid: the segment tests 0x16F720 / 0x16FC08 call 0x15ECE0 per cell, which tests each
   instance's model collision (0x133A20). The ground: T+0x24 / +0x28 / +0x2C / +0x30 the tile atlas levels 0-3 (N tiles
   of 64 x 64 PSMT8, 6.25 m per texel), T+0x20 the CLUT (CSM1 order), the tile grid as in zero_terrain.c. */
#include "ps2_runtime.h"
#include "ps2_menv.h"
#include "ps2_hook.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

u64 ps2_kernel_vblank_count(void);

#define ZP_MGR 0x0015E1D8u
#define ZP_GROUND 0x00158AE0u              /* ground height: a0 = T, f12 = x, f13 = z -> f0 */
#define ZP_HUD 0x0024D9D8u                 /* HUD manager update: a0 = player + 0x1310 (flight model at player+0xEF0) */

static int zp_dbg;
static u32 zp_O, zp_T, zp_sig;
static u32 zp_stable, zp_dumped_sig;
static float zp_pp[3];
static u64 zp_pp_field;

static int zp_ptr(u32 p) { return p >= 0x100000u && p < 0x2000000u; }
static float zp_f(u32 a) { u32 w = ps2_r32(a); float f; memcpy(&f, &w, 4); return f; }

/* call a game function from a hook (EE thread); registers saved and restored; f12/f13 float args; returns f0 */
static float zp_callf(ps2_ctx *ctx, u32 fn, u32 a0, float f12, float f13) {
    ps2_reg128 rs[32];
    ps2_fpr fs[32];
    ps2_reg128 hi = ctx->hi, lo = ctx->lo;
    float acc = ctx->acc;
    u32 fcc = ctx->fcc, fcr = ctx->fcr31, sa = ctx->sa, pc = ctx->pc;
    memcpy(rs, ctx->r, sizeof rs);
    memcpy(fs, ctx->f, sizeof fs);
    ctx->r[4].ud[0] = (u64)(s64)(s32)a0;
    ctx->f[12].f = f12; ctx->f[13].f = f13;
    ctx->r[29].ud[0] = (u64)(s64)(s32)((ctx->r[29].uw[0] - 0x80u) & ~15u);
    ctx->r[31].ud[0] = 0;
    ps2_dispatch(ctx, fn);
    float r = ctx->f[0].f;
    memcpy(ctx->r, rs, sizeof rs);
    memcpy(ctx->f, fs, sizeof fs);
    ctx->hi = hi; ctx->lo = lo; ctx->acc = acc; ctx->fcc = fcc; ctx->fcr31 = fcr; ctx->sa = sa; ctx->pc = pc;
    return r;
}

/* the terrain tile id of cell (cx, cz), 0..255 each (zero_terrain.c's tables) */
static u32 zp_tile(u32 T, int cx, int cz) {
    u32 ti = ps2_r32(T + 0x14u), tr = ps2_r32(T + 0x18u);
    if (!zp_ptr(ti) || !zp_ptr(tr) || cx < 0 || cz < 0 || cx > 255 || cz > 255) return 0xFFFFFFFFu;
    u32 b = (u32)((cz >> 4) * 16 + (cx >> 4));
    u32 rec = ps2_r8(ti + b);
    return ps2_r16(tr + rec * 512u + (u32)((cz & 15) * 16 + (cx & 15)) * 2u);
}

static u32 zp_signature(u32 O) {
    u32 h = 2166136261u;
    for (u32 k = 0x10u; k <= 0x2Cu; k += 4u) { h ^= ps2_r32(O + k); h *= 16777619u; }
    h ^= O; h *= 16777619u;
    return h ? h : 1u;
}

static void zp_dump(ps2_ctx *ctx) {
    u32 O = zp_O, T = zp_T;
    u32 grid = ps2_r32(O + 0x14u), lists = ps2_r32(O + 0x18u), ni = ps2_r32(O + 0x20u), inst = ps2_r32(O + 0x24u);
    u32 nm = ps2_r32(O + 0x28u), models = ps2_r32(O + 0x2Cu);
    ps2_log("props: map objects O %08X T %08X (O+0x10 %08X): grid %08X lists %08X instances %u at %08X, models %u at "
            "%08X, O+0x38 %08X O+0xA4 %08X O+0xA8 %08X", O, T, ps2_r32(O + 0x10u), grid, lists, ni, inst, nm, models,
            ps2_r32(O + 0x38u), ps2_r32(O + 0xA4u), ps2_r32(O + 0xA8u));
    for (u32 base = 0; base < 0x380u; base += 0x40u) {             /* the terrain object's words, for the atlas hunt */
        char line[400]; int n = 0;
        for (u32 k = 0; k < 0x40u; k += 4u) n += snprintf(line + n, sizeof line - (size_t)n, "%08X ", ps2_r32(T + base + k));
        ps2_log("props: T+%03X: %s", base, line);
    }
    for (u32 base = 0; base < 0xC0u; base += 0x40u) {
        char line[400]; int n = 0;
        for (u32 k = 0; k < 0x40u; k += 4u) n += snprintf(line + n, sizeof line - (size_t)n, "%08X ", ps2_r32(O + base + k));
        ps2_log("props: O+%03X: %s", base, line);
    }
    {   /* ACZ_DEBUG_PROPS_DUMP=dir: the map as raw data for tools (tile grid, atlas mip 0, CLUT candidates, objects) */
        const char *dir = getenv("ACZ_DEBUG_PROPS_DUMP");
        if (dir && *dir) {
            char fn[400];
            snprintf(fn, sizeof fn, "%s/map_%08X.bin", dir, zp_sig);
            FILE *fp = fopen(fn, "wb");
            if (fp) {
                u32 nio = (zp_ptr(inst) && zp_ptr(models) && ni < 65536u) ? ni : 0u;
                u32 hdr[16] = { 0x5A50524Fu, 1u, nio, nm, ps2_r32(T + 0x24u), ps2_r32(T + 0x28u), ps2_r32(T + 0x1Cu), ps2_r32(T + 0x20u), 0, 0, 0, 0, 0, 0, 0, 0 };
                u32 a0 = ps2_r32(T + 0x24u), a1 = ps2_r32(T + 0x28u);
                u32 nt = (zp_ptr(a0) && zp_ptr(a1) && a1 > a0) ? (a1 - a0) / 4096u : 0u;
                if (nt > 1024u) nt = 0;
                hdr[8] = nt;
                fwrite(hdr, 4, 16, fp);
                static u16 tg[65536];
                for (int cz = 0; cz < 256; cz++) for (int cx = 0; cx < 256; cx++) tg[cz * 256 + cx] = (u16)zp_tile(T, cx, cz);
                fwrite(tg, 2, 65536, fp);
                static u8 buf[1024 * 4096];
                for (u32 k = 0; k < nt * 4096u; k++) buf[k] = ps2_r8(a0 + k);
                fwrite(buf, 1, nt * 4096u, fp);
                for (int c = 0; c < 2; c++) {                      /* 1 KB at T+0x1C and at T+0x20 */
                    u32 p = ps2_r32(T + 0x1Cu + 4u * (u32)c);
                    for (u32 k = 0; k < 1024u; k++) buf[k] = zp_ptr(p) ? ps2_r8(p + k) : 0;
                    fwrite(buf, 1, 1024, fp);
                }
                for (u32 i = 0; zp_ptr(inst) && zp_ptr(models) && ni < 65536u && i < ni; i++) {   /* x y z model ground size */
                    u32 a = inst + i * 16u, m = ps2_r32(a + 12u) & 0x7FFFFu;
                    float rec[8] = { zp_f(a), zp_f(a + 4u), zp_f(a + 8u), (float)m, zp_callf(ctx, ZP_GROUND, T, zp_f(a), zp_f(a + 8u)), 0, 0, 0 };
                    if (m < nm) { u32 bb = ps2_r32(models + m * 0x90u + 0x40u);
                        if (zp_ptr(bb)) { rec[5] = zp_f(bb + 0xCu) - zp_f(bb + 8u); rec[6] = zp_f(bb + 0x14u) - zp_f(bb + 0x10u); rec[7] = zp_f(bb + 0x1Cu) - zp_f(bb + 0x18u); } }
                    fwrite(rec, 4, 8, fp);
                }
                fclose(fp);
                ps2_log("props: map dumped to %s (%u tiles)", fn, nt);
            }
        }
    }
    if (!zp_ptr(grid) || !zp_ptr(lists) || !zp_ptr(inst) || !zp_ptr(models) || ni > 65536u || nm > 4096u) return;
    static u32 cnt[4096];
    static float ymin[4096], ymax[4096], agl_min[4096], agl_max[4096];
    memset(cnt, 0, sizeof cnt);
    for (u32 m = 0; m < nm && m < 4096u; m++) { agl_min[m] = 1e9f; agl_max[m] = -1e9f; }
    /* models */
    for (u32 m = 0; m < nm && m < 4096u; m++) {
        u32 r = models + m * 0x90u, bb = ps2_r32(r + 0x40u), parts = ps2_r32(r + 0x6Cu);
        char ps[80]; int pn = 0;
        ps[0] = 0;
        if (zp_ptr(parts))
            for (int k = 0; k < 16; k++) { int p = (s8)ps2_r8(parts + (u32)k); if (p < 0) break; pn += snprintf(ps + pn, sizeof ps - (size_t)pn, "%d,", p); if (pn > 70) break; }
        if (zp_ptr(bb)) { ymin[m] = zp_f(bb + 0x10u); ymax[m] = zp_f(bb + 0x14u); }
        else ymin[m] = ymax[m] = 0.0f;
        {   /* collision data as 0x133A20 reads it: *(rec+8)+4 >= 3 -> *(*(rec+8)+0x40) */
            u32 d8 = ps2_r32(r + 8u), ver = zp_ptr(d8) ? ps2_r32(d8 + 4u) : 0u, col = (zp_ptr(d8) && ver >= 3u) ? ps2_r32(d8 + 0x40u) : 0u;
            ps2_log("props: model %u collision: rec+8 %08X version %u collision %08X", m, d8, ver, col);
        }
        if (zp_dbg >= 2 || m < 400u) {
            if (zp_ptr(bb))
                ps2_log("props: model %u rec %08X bounds %08X: %.0f %.0f %.0f %.0f | %.0f %.0f %.0f %.0f | %.0f %.0f %.0f %.0f  "
                        "flags44 %08X draw48 %08X groups [%s]", m, r, bb, zp_f(bb), zp_f(bb + 4u), zp_f(bb + 8u), zp_f(bb + 12u),
                        zp_f(bb + 16u), zp_f(bb + 20u), zp_f(bb + 24u), zp_f(bb + 28u), zp_f(bb + 32u), zp_f(bb + 36u),
                        zp_f(bb + 40u), zp_f(bb + 44u), ps2_r32(r + 0x44u), ps2_r32(r + 0x48u), ps);
            else
                ps2_log("props: model %u rec %08X bounds %08X flags44 %08X draw48 %08X groups [%s]", m, r, bb,
                        ps2_r32(r + 0x44u), ps2_r32(r + 0x48u), ps);
        }
        if (m < 3u) {
            char line[600]; int n = 0;
            for (u32 k = 0; k < 0x90u; k += 4u) n += snprintf(line + n, sizeof line - (size_t)n, "%08X ", ps2_r32(r + k));
            ps2_log("props: model %u raw: %s", m, line);
        }
    }
    /* instances */
    float x0 = 1e9f, x1 = -1e9f, z0 = 1e9f, z1 = -1e9f;
    for (u32 i = 0; i < ni; i++) {
        u32 a = inst + i * 16u, w = ps2_r32(a + 12u), m = w & 0x7FFFFu;
        float x = zp_f(a), y = zp_f(a + 4u), z = zp_f(a + 8u);
        if (m < nm && m < 4096u) {
            cnt[m]++;
            float g = zp_callf(ctx, ZP_GROUND, T, x, z), agl = y - g;
            if (agl < agl_min[m]) agl_min[m] = agl;
            if (agl > agl_max[m]) agl_max[m] = agl;
        }
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (z < z0) z0 = z;
        if (z > z1) z1 = z;
        if (zp_dbg >= 2 || i < 64u) {
            int cx = (int)floorf((x + 262144.0f) / 2048.0f), cz = (int)floorf((z + 262144.0f) / 2048.0f);
            ps2_log("props: inst %u model %u pos %.0f %.0f %.0f flags %05X cell %d,%d tile %04X", i, m, x, y, z,
                    w >> 19, cx, cz, zp_tile(T, cx, cz));
        }
    }
    ps2_log("props: instances span x %.0f..%.0f z %.0f..%.0f", x0, x1, z0, z1);
    for (u32 m = 0; m < nm && m < 4096u; m++)
        if (cnt[m])
            ps2_log("props: model %u used %u times, bounds y %.0f..%.0f, above ground %.0f..%.0f units", m, cnt[m],
                    ymin[m], ymax[m], agl_min[m], agl_max[m]);
    /* cells and tiles */
    static u8 tile_obj[1024], tile_any[1024];
    static u32 tile_cells[1024], tile_cells_obj[1024];
    memset(tile_obj, 0, sizeof tile_obj); memset(tile_any, 0, sizeof tile_any);
    memset(tile_cells, 0, sizeof tile_cells); memset(tile_cells_obj, 0, sizeof tile_cells_obj);
    u32 cells_obj = 0, maxlist = 0, flagsseen = 0;
    for (int cz = 0; cz < 256; cz++)
        for (int cx = 0; cx < 256; cx++) {
            u32 t = zp_tile(T, cx, cz);
            if (t == 0xFFFFFFFFu) continue;
            flagsseen |= t & 0xFC00u;
            u32 id = t & 0x3FFu;
            tile_cells[id]++;
            s16 li = (s16)ps2_r16(grid + (u32)(cz * 256 + cx) * 2u);
            if (li) {
                cells_obj++;
                tile_cells_obj[id]++;
                u32 n = 0;
                for (u32 k = 0; k < 8u; k++) { if ((s16)ps2_r16(lists + (u32)li * 16u + 2u * k) == -1) break; n++; }
                if (n > maxlist) maxlist = n;
            }
        }
    u32 tiles_with_obj = 0, tiles_used = 0, cells_rep = 0;
    for (u32 id = 0; id < 1024u; id++) {
        if (tile_cells[id]) tiles_used++;
        if (tile_cells_obj[id]) { tiles_with_obj++; cells_rep += tile_cells[id] - tile_cells_obj[id]; }
    }
    ps2_log("props: cells with objects %u of 65536 (longest list %u), tile flags seen %04X; tile ids used %u, with "
            "objects in some cell %u; cells sharing such a tile id but without objects %u", cells_obj, maxlist,
            flagsseen, tiles_used, tiles_with_obj, cells_rep);
    if (zp_dbg >= 2)
        for (u32 id = 0; id < 1024u; id++)
            if (tile_cells_obj[id])
                ps2_log("props: tile %03X: %u cells, %u with objects", id, tile_cells[id], tile_cells_obj[id]);
}


/* before 0x15F4D0 (the end of the terrain manager clears the marks): which instances the game drew this frame (marks
   in bits 19..31), and how far */
static int zp_marks(ps2_ctx *ctx, void *user) {
    (void)ctx; (void)user;
    static u64 last_f;
    u64 f = ps2_kernel_vblank_count();
    if (!zp_dbg || f - last_f < 300u || !zp_O || zp_stable < 60u) return 0;
    last_f = f;
    u32 O = zp_O, ni = ps2_r32(O + 0x20u), inst = ps2_r32(O + 0x24u);
    if (!zp_ptr(inst) || ni > 65536u) return 0;
    u32 drawn = 0, near2 = 0, near5 = 0;
    float dmax = 0.0f;
    for (u32 i = 0; i < ni; i++) {
        u32 a = inst + i * 16u;
        float dx = zp_f(a) - zp_pp[0], dz = zp_f(a + 8u) - zp_pp[2], d = sqrtf(dx * dx + dz * dz);
        if (d < 2.0f * 5120.0f) near2++;
        if (d < 5.0f * 5120.0f) near5++;
        if (ps2_r32(a + 12u) >> 19) { drawn++; if (d > dmax) dmax = d; }
    }
    ps2_log("props: field %llu player %.0f %.0f %.0f (age %llu): instances drawn %u of %u, farthest drawn %.0f units "
            "(%.1f km); within 2 km %u, 5 km %u", (unsigned long long)f, zp_pp[0], zp_pp[1], zp_pp[2],
            (unsigned long long)(f - zp_pp_field), drawn, ni, dmax, dmax * 400.0f / 2048.0f / 1000.0f, near2, near5);
    return 0;
}

/* ACZ_DEBUG_PROPS_TP="x,z,agl_m,heading_deg,field" (test only): from that field on, once, the player's aircraft is put
   at (x, z) agl_m metres above the game's ground there, flying towards heading_deg (0 = -z, 90 = +x), for repeatable
   looks at a chosen village or base; the flight model's position FM+0x3B0 and yaw FM+0x3C4 are written once */
static float zp_tp[5], zp_tpq[32][5];         /* up to 32 teleports, ';'-separated, each at its own field */
static int zp_tp_on, zp_tp_done, zp_tpn, zp_tpi;
static void zp_units_scan(u32 player);
static int zp_hud(ps2_ctx *ctx, void *user) {
    (void)user;
    u32 player = ctx->r[4].uw[0] - 0x1310u;
    if (!zp_ptr(player)) return 0;
    u32 fm = player + 0xEF0u;
    zp_pp_field = ps2_kernel_vblank_count();
    zp_units_scan(player);
    if (zp_tp_on && zp_tpi < zp_tpn && zp_pp_field >= (u64)zp_tpq[zp_tpi][4] && zp_ptr(zp_T)) {
        memcpy(zp_tp, zp_tpq[zp_tpi++], sizeof zp_tp);
        zp_tp_done = 1;
        float g = zp_callf(ctx, ZP_GROUND, zp_T, zp_tp[0], zp_tp[1]), y = g + zp_tp[2] * 5.12f, yaw = -zp_tp[3] * 3.14159265f / 180.0f;
        u32 w;
        {   /* every copy of the position vector in the player object (x, y, z floats with this x and z) */
            float px = zp_f(fm + 0x3B0u), pz = zp_f(fm + 0x3B8u);
            for (u32 o = 0; o < 0x21D0u; o += 4u) {
                float a = zp_f(player + o), c = zp_f(player + o + 8u);
                if (fabsf(a - px) < 2.0f && fabsf(c - pz) < 2.0f) {
                    ps2_log("props: debug teleport: position copy at player+0x%X (%.1f %.1f %.1f)", o, a, zp_f(player + o + 4u), c);
                    memcpy(&w, &zp_tp[0], 4); ps2_w32(player + o, w);
                    memcpy(&w, &y, 4); ps2_w32(player + o + 4u, w);
                    memcpy(&w, &zp_tp[1], 4); ps2_w32(player + o + 8u, w);
                }
            }
        }
        memcpy(&w, &zp_tp[0], 4); ps2_w32(fm + 0x3B0u, w);
        memcpy(&w, &y, 4); ps2_w32(fm + 0x3B4u, w);
        memcpy(&w, &zp_tp[1], 4); ps2_w32(fm + 0x3B8u, w);
        memcpy(&w, &yaw, 4); ps2_w32(fm + 0x3C4u, w);
        {   /* the player's unit record too (zero_vr.c imm_unit: position +0x260) */
            u32 t = ps2_r32(player + 0x1A5Cu), G = zp_ptr(t) ? ps2_r32(t) : 0u;
            u32 e = zp_ptr(G) ? ps2_r32(G + 4u * ps2_r8(player + 0x1AB1u)) : 0u;
            u32 u = zp_ptr(e) ? ps2_r32(e + 0x104u) + (u32)ps2_r8(player + 0x1AB2u) * 0x4A0u : 0u;
            if (zp_ptr(u)) {
                memcpy(&w, &zp_tp[0], 4); ps2_w32(u + 0x260u, w);
                memcpy(&w, &y, 4); ps2_w32(u + 0x264u, w);
                memcpy(&w, &zp_tp[1], 4); ps2_w32(u + 0x268u, w);
            }
            ps2_log("props: debug teleport: unit record %08X", u);
        }
        ps2_log("props: debug teleport at field %llu to %.0f %.0f %.0f (ground %.0f), yaw %.3f", (unsigned long long)zp_pp_field,
                zp_tp[0], y, zp_tp[1], g, yaw);
    }
    for (u32 i = 0; i < 3; i++) zp_pp[i] = zp_f(fm + 0x3B0u + 4u * i);
    {   /* ACZ_DEBUG_PROPS_UNITS=field (research): the unit table once -- groups, records, positions, hit points */
        static long uf = -2, ue = 0;
        if (uf == -2) {
            const char *e = getenv("ACZ_DEBUG_PROPS_UNITS");
            uf = e ? atol(e) : -1;
            const char *c = e ? strchr(e, ',') : NULL;
            ue = c ? atol(c + 1) : 0;                    /* ",N": again every N fields */
        }
        if (uf >= 0 && zp_pp_field >= (u64)uf) {
            uf = ue > 0 ? (long)zp_pp_field + ue : -1;
            u32 t = ps2_r32(player + 0x1A5Cu), G = zp_ptr(t) ? ps2_r32(t) : 0u, ng = zp_ptr(t) ? ps2_r8(t + 0x1Cu) : 0u;
            ps2_log("props: units: field %llu player %08X table %08X groups %08X count %u", (unsigned long long)zp_pp_field,
                    player, t, G, ng);
            for (u32 g = 0; zp_ptr(G) && g < ng && g < 64u; g++) {
                u32 e = ps2_r32(G + 4u * g);
                if (!zp_ptr(e)) continue;
                u32 u0 = ps2_r32(e + 0x104u), cnt = ps2_r32(e + 0x108u);
                ps2_log("props: units: group %u at %08X units %u", g, e, cnt);
                for (u32 i = 0; zp_ptr(u0) && i < cnt && i < 256u; i++) {
                    u32 u = u0 + i * 0x4A0u, kt = ps2_r32(u + 0x1C4u), tt = zp_ptr(kt) ? ps2_r32(kt) : 0u;
                    float x = zp_f(u + 0x260u), y = zp_f(u + 0x264u), z = zp_f(u + 0x268u);
                    float gy = (zp_ptr(zp_T) && x > -262144.0f && x < 262144.0f && z > -262144.0f && z < 262144.0f)
                               ? zp_callf(ctx, ZP_GROUND, zp_T, x, z) : 0.0f;
                    ps2_log("props: units:   %u.%u at %08X pos %.0f %.0f %.0f agl %.0f hp %d flags %08X kind %d/%u side %04X",
                            g, i, u, x, y, z, y - gy, (int)(s16)ps2_r16(u + 0x398u), ps2_r32(u + 0x1F8u),
                            zp_ptr(tt) ? (int)ps2_r8(tt + 0x71u) : -1, ps2_r8(u + 0x39Bu), ps2_r16(u + 0x23Cu));
                }
            }
        }
    }
    if (zp_tp_on && zp_tp_done && zp_dbg >= 2 && (zp_pp_field % 30u) == 0u)
        ps2_log("props: field %llu player %.0f %.0f %.0f yaw %.3f pitch %.3f roll %.3f", (unsigned long long)zp_pp_field,
                zp_pp[0], zp_pp[1], zp_pp[2], zp_f(fm + 0x3C4u), zp_f(fm + 0x3C0u), zp_f(fm + 0x3C8u));
    return 0;
}

/* ==== stage 1: houses raised from the ground photo (ACZ_TERRAIN_PROPS) ============================================

   Every map is an aerial photo: 64 x 64 PSMT8 tiles (6.25 m per texel) laid out by the tile grid, the 3D objects only a
   few (mission 2: 105 instances, all along one road; mission 4: 40 cell-sized pieces of the ports). The painted villages
   are flat. Here the red roofs of the photo become low houses: per tile the roof blobs are found once per map (EE
   thread copies, one worker finds, when a map is loaded), and the renderer (rn_vp_terrain.c, after the first pass of
   every terrain patch, fine or coarse, within ACZ_TERRAIN_PROPS_DIST) draws the houses of that cell as one more mesh
   draw of the patch itself: the same
   texture (the roof is the photo's own texels, projected from above exactly onto the ground's), the same vertex colour,
   fog, relief light and cloud shadows (mesh.vert prog 1 pass 0), the same stereo and in-between frame handling. The
   walls take one calm mid-grey texel of the tile, shaded by their facing to the mission's sun; small houses get a gable
   roof along their long side. The ground under a house is the patch's own triangles (the surface the game collides
   with), so the houses stand exactly on the drawn ground; over the outer 15 % of the reach they grow out of their
   roofs instead of popping up.

   Nothing of the game changes: no collision (a house is 5-11 m tall; the game's own buildings stay the solid ones), no
   targets (houses within ACZ_TERRAIN_PROPS_CLEAR of a static object are left out), no new art. */
#include "ps2_vk.h"
#include "ps2_props.h"
#include <pthread.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#if defined(__has_include)
#if __has_include("ps2_clouds.h")
#include "ps2_clouds.h"
#define ZP_C1 1
#endif
#endif

extern u32 ps2_gs_frame_count;

typedef struct {
    float cu, cv;          /* centre, tile texels (u = column, v = row) */
    float hl, hw;          /* half length / half width, texels */
    float au, av;          /* unit axis of the length, texel space */
    float eave, ridge;     /* heights above the highest ground under it, units (red roofs) */
    u32 kind;              /* 1 red roof, 2 a building found by its shadow (axis-aligned box), 3 a round tank (hl = r),
                              4 a city building (oriented, eave / ridge set by its district; zero_props_city.h) */
    float slen;            /* kind 2: the shadow's length in texels (height = slen x 6.25 m x tan(sun elevation)) */
    float wu, wv;          /* kinds 2, 3: the wall texel (its own roof's, texel centre); < 0: the tile's wall texel */
} zp_box;

typedef struct zp_map {
    u32 sig, nt;
    u16 grid[65536];       /* tile id + flags per cell, rows by z */
    u32 first[1025];       /* houses of tile t: [first[t], first[t + 1]) */
    float wall[1024][2];   /* the wall texel of each tile (texel centre) */
    u32 nbox;
    zp_box *box;
    u32 nex;
    float (*ex)[4];        /* areas left out (x0, z0, x1, z1): the game's own static objects */
    u8 city[1024];         /* tiles whose houses are the city finder's (kind 4 only; drawn by zp_city_patch, cached) */
    u32 *exc_first;        /* city: per cell [65537] the areas left out that reach into it (indices into ex) */
    u16 *exc_idx;
    u32 ncity;             /* city buildings in the tiles */
    u32 serial;            /* the build's number (the city cache belongs to one) */
    struct zp_map *prev;   /* the map before (freed when the next one comes: the renderer is long done with it) */
} zp_map;

static int zp_on;                          /* ACZ_TERRAIN_PROPS */
#define ZP_K_ROOFS 1
#define ZP_K_BLOCKS 2
#define ZP_K_TANKS 4
#define ZP_K_DESERT (ZP_K_BLOCKS | ZP_K_TANKS)
static int zp_kinds;                       /* what ACZ_TERRAIN_PROPS raises: 1 = red roofs, 2 = also buildings and tanks */
#define ZP_K_CITY 8
static int zp_city = 1;                    /* ACZ_TERRAIN_CITY: 0 off, 1 city maps (the guard), 2 any map (test) */
static float zp_city_dist = 2500.0f * 5.12f;   /* ACZ_TERRAIN_CITY_DIST (metres -> units) */
static float zp_city_reach = 2500.0f * 5.12f;  /* the reach in effect: shrinks when a frame nears the vertex budget */
static float zp_city_hs = 1.0f;            /* ACZ_TERRAIN_CITY_HEIGHT */
static u32 zp_city_budget = 65536u;        /* ACZ_TERRAIN_CITY_BUDGET: city vertices per GS frame */
static int zp_city_win = 1;                /* ACZ_TERRAIN_CITY_WINDOWS: rows of windows on the facades (mesh.frag) */
static int zp_city_look = 1;               /* ACZ_TERRAIN_CITY_LOOK: the detailed look (city_look.glsl; bug #261) */
static int zp_city_look_ab;                /* ACZ_TERRAIN_CITY_LOOK_AB: in-run GPU A/B blocks (ps2_video.c) */
#define ZP_CITY_LAND 4u                    /* city: tiles used by at most 4 land cells (the photo's own places, not filler) */
#define ZP_CITY_UMIN 0.10f                 /* city: a tile keeps its finds only with >= 10 % built-up texels */
#define ZP_CITY_CELLS 40u                  /* city map: >= 40 own cells (land <= 2) with >= 40 % built-up (M6 239; the
                                              other 22 maps 0-19) */
static int zp_shadow_cfg = -1;             /* ACZ_TERRAIN_PROPS_SHADOW: -1 the mission's sun, 0..3 fixed, 4 the photo's vote */
static float zp_unit_clear = 409.6f;       /* ACZ_TERRAIN_PROPS_UNIT_CLEAR: nothing within this of a ground unit (80 m) */
/* ground / sea units seen this map (EE thread appends, the renderer reads): positions x, z; per cell a mark when a
   unit's clear area reaches into it */
#define ZP_MAXU 4096u
static float zp_ux[ZP_MAXU][2];
static u32 zp_uid[ZP_MAXU];                /* (group << 8 | index), | 0x80000000 for a unit's current place */
static volatile u32 zp_nu;
static u8 zp_ucell[65536];                 /* 0 = no unit place near; else a version, bumped when one is added or moves */
static u32 zp_u_sig;
static u64 zp_u_next, zp_st_unit_skip;
static float zp_dist = 17920.0f;           /* units (3500 m) */
static float zp_hscale = 1.0f, zp_clear = 96.0f;
static int zp_ab_n;
static zp_map *volatile zp_pub;
static u32 zp_map_serial;                  /* builds so far (zp_map.serial) */
static u32 zp_built_sig;
static u64 zp_st_draws, zp_st_houses, zp_st_room, zp_st_frames, zp_st_kind[3];
static u32 zp_fs[6];                       /* debug, per GS frame: patches fine / coarse, near, with houses; draws, houses */
static u64 zp_fs_ns, zp_fs_ns_sum, zp_fs_ns_n;  /* debug: CPU time of the patch hook per GS frame (GS replay thread) */
static u64 zp_now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (u64)t.tv_sec * 1000000000ull + (u64)t.tv_nsec; }
static u32 zp_frame_verts, zp_frame_at;

static u32 zp_hash(u32 a) { a ^= a >> 16; a *= 0x7FEB352Du; a ^= a >> 15; a *= 0x846CA68Bu; a ^= a >> 16; return a; }

/* the roofs of one 64 x 64 tile (rgb 3 bytes per texel; tools/props/detect.py is the same finder in Python): red roof
   texels (R - max(G, B) > 30 and 25 above the 9 x 9 mean of that redness; a red-pink hue, R - G > 1.3 (G - B) + 10, in
   green or grey surroundings, 9 x 9 mean of G - R > -5 -- a roof is red against green, the red rock and sand of the
   desert maps are red everywhere -- or an orange hue, R - G > 0.6 (G - B) + 10, among greener surroundings, mean of
   G - R > 3) seed 4-connected blobs that take in the adjacent light texels (the photo's sunlit roof half) without
   growing through them; 2..64 texels; no water within 2 texels (3 dark blue texels: a shore's red sand); oriented
   rectangles from the second moments; streets (thin and long) are not houses. Also picks the tile's wall texel (a calm
   mid-light grey). */
static u32 zp_find(const u8 *rgb, u32 tile, zp_box *out, u32 cap, float wall[2]) {
    static float L[4096], bg[4096];
    static u8 cls[4096], seen[4096];
    static u16 stack[4096];
    u32 n = 0;
    for (int i = 0; i < 4096; i++) {
        float r = rgb[3 * i], g = rgb[3 * i + 1], b = rgb[3 * i + 2];
        L[i] = 0.299f * r + 0.587f * g + 0.114f * b;
    }
    static float bgr[4096], RD[4096], GR[4096], bgd[4096];  /* redness R - max(G, B), greenness G - R, local means */
    for (int i = 0; i < 4096; i++) {
        float r = rgb[3 * i], g = rgb[3 * i + 1], b = rgb[3 * i + 2];
        RD[i] = r - (g > b ? g : b);
        GR[i] = g - r;
    }
    for (int pass = 0; pass < 3; pass++) {   /* 9 x 9 box means (edges clamped), separable: luminance, redness, greenness */
        const float *src = pass == 2 ? GR : pass ? RD : L;
        float *dst = pass == 2 ? bgd : pass ? bgr : bg;
        static float hs[4096];
        for (int y = 0; y < 64; y++)
            for (int x = 0; x < 64; x++) {
                float s = 0.0f;
                for (int d = -4; d <= 4; d++) { int xx = x + d < 0 ? 0 : x + d > 63 ? 63 : x + d; s += src[y * 64 + xx]; }
                hs[y * 64 + x] = s;
            }
        for (int y = 0; y < 64; y++)
            for (int x = 0; x < 64; x++) {
                float s = 0.0f;
                for (int d = -4; d <= 4; d++) { int yy = y + d < 0 ? 0 : y + d > 63 ? 63 : y + d; s += hs[yy * 64 + x]; }
                dst[y * 64 + x] = s / 81.0f;
            }
    }
    float best = -1e9f;
    wall[0] = wall[1] = 32.0f;
    for (int i = 0; i < 4096; i++) {
        float r = rgb[3 * i], g = rgb[3 * i + 1], b = rgb[3 * i + 2];
        float mx = r > g ? (r > b ? r : b) : (g > b ? g : b), mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
        float sat = (mx - mn) / (mx > 1.0f ? mx : 1.0f);
        cls[i] = 0;
        seen[i] = 0;
        if (RD[i] > 30.0f && RD[i] - bgr[i] > 25.0f
            && ((r - g > 1.3f * (g - b) + 10.0f && bgd[i] > -5.0f)       /* red / pink roofs */
                || (r - g > 0.6f * (g - b) + 10.0f && bgd[i] > 3.0f)))   /* orange ones among greener surroundings */
            cls[i] = 1;
        else if (L[i] - bg[i] > 20.0f && sat < 0.3f && L[i] > 100.0f) cls[i] = 2;
        int x = i & 63, y = i >> 6;                       /* the wall texel: light, grey, in a calm 3 x 3 */
        if (x > 0 && x < 63 && y > 0 && y < 63 && sat < 0.22f) {
            float m = 0.0f, v = 0.0f;
            for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) m += L[i + dy * 64 + dx];
            m /= 9.0f;
            for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) { float d = L[i + dy * 64 + dx] - m; v += d * d; }
            float sc = -fabsf(L[i] - 150.0f) - 0.8f * sqrtf(v / 9.0f) - 60.0f * sat;   /* a calm mid-light grey */
            if (sc > best) { best = sc; wall[0] = (float)x + 0.5f; wall[1] = (float)y + 0.5f; }
        }
    }
    for (int i0 = 0; i0 < 4096 && n < cap; i0++) {
        if (cls[i0] != 1 || seen[i0]) continue;
        u32 sp = 0, cnt = 0, nred = 0;
        int bx0 = 63, bx1 = 0, by0 = 63, by1 = 0;
        double su = 0, sv = 0, suu = 0, svv = 0, suv = 0;
        stack[sp++] = (u16)i0;
        seen[i0] = 1;
        while (sp) {
            int i = stack[--sp], x = i & 63, y = i >> 6;
            cnt++;
            nred += cls[i] == 1;
            if (x < bx0) bx0 = x;
            if (x > bx1) bx1 = x;
            if (y < by0) by0 = y;
            if (y > by1) by1 = y;
            double u = x + 0.5, v = y + 0.5;
            su += u; sv += v; suu += u * u; svv += v * v; suv += u * v;
            const int nb[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
            for (int k = 0; k < 4 && cls[i] == 1; k++) {          /* only red texels grow the roof (one light rim) */
                int xx = x + nb[k][0], yy = y + nb[k][1];
                if (xx < 0 || xx > 63 || yy < 0 || yy > 63) continue;
                int j = yy * 64 + xx;
                if (cls[j] && !seen[j] && sp < 4096u) { seen[j] = 1; stack[sp++] = (u16)j; }
            }
        }
        if (cnt < 2u || cnt > 64u) continue;
        {   /* no water within 2 texels (a shore's red sand against the dark water is no roof): 3 or more dark blue texels */
            int wet = 0;
            for (int y = by0 - 2; y <= by1 + 2; y++)
                for (int x = bx0 - 2; x <= bx1 + 2; x++) {
                    if (x < 0 || x > 63 || y < 0 || y > 63) continue;
                    const u8 *q = rgb + 3 * (y * 64 + x);
                    wet += q[2] > q[0] + 15 && q[2] > q[1] && L[y * 64 + x] < 100.0f;
                }
            if (wet >= 3) continue;
        }
        double cu = su / cnt, cv = sv / cnt;
        double a = suu / cnt - cu * cu + 1.0 / 12.0, c = svv / cnt - cv * cv + 1.0 / 12.0, b = suv / cnt - cu * cv;
        double tr = a + c, det = a * c - b * b, disc = sqrt(fmax(tr * tr / 4.0 - det, 0.0));
        double l1 = tr / 2.0 + disc, l2 = tr / 2.0 - disc;     /* l1 >= l2 */
        double hl = sqrt(3.0 * fmax(l1, 1e-6)), hw = sqrt(3.0 * fmax(l2, 1e-6));
        if (hl < 0.6) hl = 0.6;
        if (hw < 0.6) hw = 0.6;
        if ((2.0 * hw < 1.1 && 2.0 * hl > 3.5) || hl / hw > 4.5) continue;
        double au = 1.0, av = 0.0;
        if (fabs(b) > 1e-9) { au = b; av = l1 - a; }
        else if (c > a) { au = 0.0; av = 1.0; }
        double al = sqrt(au * au + av * av);
        au /= al; av /= al;
        float Lm = (float)(2.0 * hl * 6.25), Wm = (float)(2.0 * hw * 6.25);
        float var = 0.85f + 0.3f * (float)(zp_hash(tile * 131u + n * 7919u) & 1023u) / 1023.0f;
        float eave = (Lm < 20.0f ? 5.5f : Lm < 40.0f ? 7.0f : 9.0f) * var;
        float ridge = Lm < 32.0f ? 0.4f * Wm : 0.0f;
        if (ridge > 4.5f) ridge = 4.5f;
        if (ridge > 0.0f && ridge < 1.5f) ridge = 1.5f;
        if (eave + ridge > 15.0f) eave = 15.0f - ridge;
        zp_box *o = &out[n++];
        o->cu = (float)cu; o->cv = (float)cv; o->hl = (float)hl; o->hw = (float)hw;
        o->au = (float)au; o->av = (float)av;
        o->eave = eave * 5.12f * zp_hscale; o->ridge = ridge * 5.12f * zp_hscale;
        o->kind = 1u;
        o->slen = 0.0f; o->wu = o->wv = -1.0f;
        (void)nred;
    }
    return n;
}

/* once per map: the EE thread copies the tiles, the palette, the grid and the static objects' areas (a few ms), a
   worker thread at the lowest priority finds the roofs of every tile and publishes the map for the renderer */
typedef struct { zp_map *m; u8 *idx; u8 pal[256][3]; u32 nt; u64 f0; int dir; } zp_job;
static volatile int zp_busy;

/* ---- buildings and tanks found by their shadows (ACZ_TERRAIN_PROPS=2; wish #78 / #55, zero_props_find.h) ---- */
#include "zero_props_find.h"
#include "zero_props_city.h"
static float zp_smooth(float e0, float e1, float x) { float t = (x - e0) / (e1 - e0); t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t; return t * t * (3.0f - 2.0f * t); }

/* the city of a map (stage 3): the finder on the photo's own tiles, the guard, every tile's district level and its
   buildings' heights; the result per tile in cr->first / cr->b (kind 4 boxes) */
typedef struct { u32 *first; zp_box *b; u32 n; int on; float lvl[1024]; } zp_city_res;
static void zp_city_build(const zp_job *j, const unsigned *land, zp_city_res *cr) {
    zp_map *m = j->m;
    u32 nt = j->nt;
    static u8 rgb[4096 * 3];
    static zpc_bld fb[ZPC_MAXB];
    static float uf[1024];
    static u32 ccnt[1024];
    cr->first = (u32 *)calloc(nt + 1u, sizeof(u32));
    cr->b = (zp_box *)malloc(sizeof(zp_box) * 160u * 1024u);
    cr->n = 0; cr->on = 0;
    if (!cr->first || !cr->b) return;
    u64 t0 = zp_now_ns();
    zpc_tile_info *inf = (zpc_tile_info *)calloc(nt, sizeof *inf);
    zpc_bld *all = (zpc_bld *)malloc(sizeof(zpc_bld) * 160u * 1024u);
    u32 na = 0, nscan = 0, *af = (u32 *)calloc(nt + 1u, sizeof(u32));
    if (!inf || !all || !af) { free(inf); free(all); free(af); return; }
    for (u32 t = 0; t < nt; t++) {
        af[t] = na; uf[t] = 0.0f;
        if (!land[t] || (zp_city < 2 && land[t] > ZP_CITY_LAND)) continue;
        for (u32 k = 0; k < 4096u; k++) memcpy(rgb + 3 * k, j->pal[j->idx[t * 4096u + k]], 3);
        int n = zpc_tile(rgb, fb, ZPC_MAXB, &inf[t]);
        nscan++;
        uf[t] = inf[t].ufrac;
        if (inf[t].ufrac < ZP_CITY_UMIN || n <= 0) continue;
        if (na + (u32)n > 160u * 1024u) n = (int)(160u * 1024u - na);
        memcpy(all + na, fb, sizeof(zpc_bld) * (size_t)n);
        na += (u32)n;
    }
    af[nt] = na;
    /* the guard: a city map has a city among the photo's own tiles */
    u32 own = 0;
    for (u32 c = 0; c < 65536u; c++) {
        u32 t = m->grid[c] & 0x3FFu;
        if (t < nt && !(m->grid[c] & 0x800u) && land[t] <= 2u && uf[t] >= 0.4f) own++;
    }
    cr->on = zp_city >= 2 || own >= ZP_CITY_CELLS;
    /* district level per tile: the built-up fraction of the 5 x 5 cells (2 km) around the cells that use it, and the
       game's own buildings there (the city centre: its static objects) */
    static u8 objc[65536];
    memset(objc, 0, sizeof objc);
    for (u32 e = 0; e < m->nex; e++) {
        int cx = (int)floorf((0.5f * (m->ex[e][0] + m->ex[e][2]) + 262144.0f) / 2048.0f);
        int cz = (int)floorf((0.5f * (m->ex[e][1] + m->ex[e][3]) + 262144.0f) / 2048.0f);
        if (cx >= 0 && cx < 256 && cz >= 0 && cz < 256 && objc[cz * 256 + cx] < 255) objc[cz * 256 + cx]++;
    }
    memset(cr->lvl, 0, sizeof cr->lvl);
    memset(ccnt, 0, sizeof ccnt);
    for (int cz = 0; cz < 256 && cr->on; cz++)
        for (int cx = 0; cx < 256; cx++) {
            u32 t = m->grid[cz * 256 + cx] & 0x3FFu;
            if (t >= nt || af[t + 1] <= af[t]) continue;
            float s = 0.0f;
            u32 no = 0;
            for (int dz = -2; dz <= 2; dz++)
                for (int dx = -2; dx <= 2; dx++) {
                    int x = cx + dx, z = cz + dz;
                    if (x < 0 || x > 255 || z < 0 || z > 255) continue;
                    u32 tt = m->grid[z * 256 + x];
                    if ((tt & 0x3FFu) < nt && !(tt & 0x800u)) s += uf[tt & 0x3FFu];
                    no += objc[z * 256 + x];
                }
            float lv = 1.3f * s / 25.0f + 0.03f * (float)no;
            cr->lvl[t] += lv > 1.0f ? 1.0f : lv;
            ccnt[t]++;
        }
    float hs = zp_city_hs * zp_hscale;
    u32 ntl = 0, npitch = 0;
    for (u32 t = 0; t < nt; t++) {
        cr->first[t] = cr->n;
        if (!cr->on || af[t + 1] <= af[t] || !ccnt[t]) continue;
        float dl = cr->lvl[t] / (float)ccnt[t], sd = zp_smooth(0.2f, 0.85f, dl);
        cr->lvl[t] = dl;
        ntl++;
        for (u32 i = af[t]; i < af[t + 1] && cr->n < 160u * 1024u; i++) {
            const zpc_bld *q = &all[i];
            zp_box *o = &cr->b[cr->n++];
            memset(o, 0, sizeof *o);
            o->cu = q->cu; o->cv = q->cv; o->hl = q->hl; o->hw = q->hw; o->au = q->au; o->av = q->av;
            o->kind = 4u;
            if (inf[t].nwall > 0) { o->wu = inf[t].wall[q->wall % (u32)inf[t].nwall][0]; o->wv = inf[t].wall[q->wall % (u32)inf[t].nwall][1]; }
            else { o->wu = q->cu; o->wv = q->cv; }
            u32 hh = zp_hash(t * 2654435761u + i * 40503u + 99u);
            float r1 = (float)(hh & 1023u) / 1023.0f, r2 = (float)((hh >> 10) & 1023u) / 1023.0f, r3 = (float)((hh >> 20) & 1023u) / 1023.0f;
            float var = 0.72f + 0.56f * r1;                       /* +-28 % per building */
            if (r2 < 0.05f + 0.07f * sd) var *= 1.45f;           /* now and then a taller block, more in the centre */
            if (4.0f * q->hl * q->hw < 5.0f) var *= 0.85f;       /* small footprints: houses */
            float eave = (6.5f + 13.5f * sd) * var;             /* outskirts ~6.5 m (2 floors), centre ~20 m (6) */
            eave = eave < 4.5f ? 4.5f : eave > 32.0f ? 32.0f : eave;
            float Lm = 2.0f * q->hl * 6.25f, Wm = 2.0f * q->hw * 6.25f, ridge = 0.0f;
            if (sd < 0.6f && Lm < 34.0f && r3 < 0.75f - 0.6f * sd) {   /* pitched roofs on the smaller, outer houses */
                ridge = 0.35f * Wm;
                ridge = ridge < 1.5f ? 1.5f : ridge > 4.5f ? 4.5f : ridge;
                npitch++;
            }
            o->eave = eave * 5.12f * hs; o->ridge = ridge * 5.12f * hs;
        }
    }
    cr->first[nt] = cr->n;
    ps2_log("props: map %08X: city finder: %u buildings in the %u tiles of the photo's own places (used by 1-%u land cells; kept "
            "in tiles >= %.0f %% built-up), %u own cells >= 40 %% built-up (a city map from %u: %s), %u city tiles (%u buildings "
            "with pitched roofs); %.1f ms", m->sig, na, nscan, ZP_CITY_LAND, ZP_CITY_UMIN * 100.0f, own, ZP_CITY_CELLS,
            cr->on ? "yes" : "no", ntl, npitch, (double)(zp_now_ns() - t0) / 1e6);
    free(inf); free(all); free(af);
}
static const char *const zp_dir_name[5] = { "up (-z)", "down (+z)", "left (-x)", "right (+x)", "none" };
#define ZP_LMAP_MIN 150.0f   /* bright desert / industrial photos only (M4 198; M3 110, M7 67, M6 59, M9 44, M12 28) */

/* the wall texel of a find: of its roof texels (inside the rectangle / within r - 1 of a tank's centre) the one
   nearest to their median luminance; texel centre */
static void zp_wall_texel(const u8 *rgb, float x0, float y0, float x1, float y1, float cu, float cv, float rr,
                          float *wu, float *wv) {
    float Lv[256];
    int xs[256], ys[256], n = 0;
    for (int y = (int)floorf(y0); y <= (int)floorf(y1) && n < 256; y++)
        for (int x = (int)floorf(x0); x <= (int)floorf(x1) && n < 256; x++) {
            if (x < 0 || x > 63 || y < 0 || y > 63) continue;
            if (rr > 0.0f) { float dx = (float)x + 0.5f - cu, dy = (float)y + 0.5f - cv; if (dx * dx + dy * dy > rr * rr) continue; }
            Lv[n] = zpf_lum(rgb + 3 * (y * 64 + x)); xs[n] = x; ys[n] = y; n++;
        }
    *wu = cu; *wv = cv;
    if (!n) return;
    float s[256];
    memcpy(s, Lv, sizeof(float) * (size_t)n);
    qsort(s, (size_t)n, sizeof s[0], zpf_cmpf);
    float med = s[n / 2], best = 1e9f;
    for (int i = 0; i < n; i++)
        if (fabsf(Lv[i] - med) < best) { best = fabsf(Lv[i] - med); *wu = (float)xs[i] + 0.5f; *wv = (float)ys[i] + 0.5f; }
}

/* the shadow direction from the photo itself (no sun known, or ACZ_TERRAIN_PROPS_SHADOW=vote): buildings of each of
   the four directions in the tiles that only one or two land cells use (the photo's own places, not the filler); the
   most if at least 20 and 1.5 x the runner-up, else none (-1) */
static int zp_vote(const zp_job *j, const unsigned *land, float lmap) {
    static u8 rgb[4096 * 3];
    static zpf_block b[ZPF_MAXB];
    static zpf_tank tk[ZPF_MAXT];
    u32 cnt[4] = { 0, 0, 0, 0 };
    for (u32 t = 0; t < j->nt; t++) {
        if (!land[t] || land[t] > 2u) continue;
        for (u32 k = 0; k < 4096u; k++) memcpy(rgb + 3 * k, j->pal[j->idx[t * 4096u + k]], 3);
        for (int d = 0; d < 4; d++) { int nb = 0, ntk = 0; zpf_tile(rgb, d, lmap, b, &nb, tk, &ntk); cnt[d] += (u32)nb; }
    }
    int best = 0, second = -1;
    for (int d = 1; d < 4; d++) if (cnt[d] > cnt[best]) best = d;
    for (int d = 0; d < 4; d++) if (d != best && (second < 0 || cnt[d] > cnt[second])) second = d;
    ps2_log("props: map %08X: buildings by shadow direction up %u down %u left %u right %u", j->m->sig, cnt[0], cnt[1], cnt[2], cnt[3]);
    return cnt[best] >= 20u && (float)cnt[best] >= 1.5f * (float)cnt[second] ? best : -1;
}

static void *zp_build_thread(void *arg) {
    zp_job *j = (zp_job *)arg;
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    { extern void ps2_host_thread_start(const char *); ps2_host_thread_start("bg"); }  /* (PS2_THREAD_CORES=h) */
#endif
    zp_map *m = j->m;
    zp_box *tmp = (zp_box *)malloc(sizeof(zp_box) * 512u);
    zp_box *all = (zp_box *)malloc(sizeof(zp_box) * 192u * 1024u);
    static u8 rgb[4096 * 3];
    u32 nb = 0, nroof = 0, nblk = 0, ntank = 0, ntiles = 0;
    /* the desert finder's inputs: the tiles land cells use, the map's land median luminance, the shadow direction */
    static unsigned land[1024];
    memset(land, 0, sizeof land);
    for (u32 c = 0; c < 65536u; c++) { u32 t = m->grid[c] & 0x3FFu; if (t < j->nt && !(m->grid[c] & 0x800u)) land[t]++; }
    float lmap = 190.0f;
    int dir = j->dir;
    if (zp_kinds & ZP_K_DESERT) {
        double hist[256], tot = 0.0, acc = 0.0;
        memset(hist, 0, sizeof hist);
        for (u32 t = 0; t < j->nt; t++) {
            if (!land[t]) continue;
            double w = land[t] > 50u ? 50.0 : (double)land[t];
            for (u32 k = 0; k < 4096u; k += 7u) {
                int bb = (int)zpf_lum(j->pal[j->idx[t * 4096u + k]]);
                hist[bb < 0 ? 0 : bb > 255 ? 255 : bb] += w; tot += w;
            }
        }
        for (int bb = 0; bb < 256 && tot > 0.0; bb++) { acc += hist[bb]; if (acc >= 0.5 * tot) { lmap = (float)bb + 0.5f; break; } }
        if (lmap < ZP_LMAP_MIN) {                         /* the finder is tuned on bright desert photos (M4: 198) */
            ps2_log("props: map %08X: land median luminance %.0f: too dark for the building finder (< %.0f); none", m->sig,
                    lmap, ZP_LMAP_MIN);
            dir = -1;
        } else if (dir == 4) dir = zp_vote(j, land, lmap);
    }
    /* the precision guard: the filler tiles (used by 20 or more land cells: sand, rock, scrub) hold no building; a
       find there is false and would repeat over the map -- if the finder fires on any of them, this map's photo is
       too noisy for it (dark or rocky maps) and nothing of it is raised */
    u32 nfill = 0, nfill_tiles = 0;
    if ((zp_kinds & ZP_K_DESERT) && dir >= 0 && dir < 4) {
        static zpf_block gb[ZPF_MAXB];
        static zpf_tank gt[ZPF_MAXT];
        for (u32 t = 0; t < j->nt; t++) {
            if (land[t] < 20u) continue;
            nfill_tiles++;
            for (u32 k = 0; k < 4096u; k++) memcpy(rgb + 3 * k, j->pal[j->idx[t * 4096u + k]], 3);
            int ngb = 0, ngt = 0;
            zpf_tile(rgb, dir, lmap, gb, &ngb, gt, &ngt);
            nfill += (u32)(((zp_kinds & ZP_K_BLOCKS) ? ngb : 0) + ((zp_kinds & ZP_K_TANKS) ? ngt : 0));
        }
        if (nfill) {
            ps2_log("props: map %08X: %u finds in %u filler tiles (sand, rock): the photo is too noisy for the building "
                    "finder; no buildings or tanks on this map", m->sig, nfill, nfill_tiles);
            dir = -1;
        }
    }
    static zp_city_res cr;                                /* stage 3: the city (its tiles take only city buildings) */
    memset(&cr, 0, sizeof cr);
    if (zp_kinds & ZP_K_CITY) zp_city_build(j, land, &cr);
    if (cr.on) {                                          /* the static objects' areas per cell, for the city's test */
        m->exc_first = (u32 *)calloc(65537u, sizeof(u32));
        u32 tot = 0;
        for (int pass = 0; pass < 2 && m->exc_first; pass++) {
            if (pass) {
                m->exc_idx = (u16 *)malloc(sizeof(u16) * (tot ? tot : 1u));
                if (!m->exc_idx) break;
                u32 a = 0;
                for (u32 c = 0; c < 65536u; c++) { u32 k = m->exc_first[c]; m->exc_first[c] = a; a += k; }
                m->exc_first[65536] = a;
            }
            static u32 fill[65536];
            if (pass) memset(fill, 0, sizeof fill);
            for (u32 e = 0; e < m->nex && e < 65535u; e++) {
                int c0 = (int)floorf((m->ex[e][0] + 262144.0f) / 2048.0f), c1 = (int)floorf((m->ex[e][2] + 262144.0f) / 2048.0f);
                int d0 = (int)floorf((m->ex[e][1] + 262144.0f) / 2048.0f), d1 = (int)floorf((m->ex[e][3] + 262144.0f) / 2048.0f);
                for (int cz = d0; cz <= d1; cz++)
                    for (int cx = c0; cx <= c1; cx++) {
                        if (cx < 0 || cx > 255 || cz < 0 || cz > 255) continue;
                        u32 c = (u32)(cz * 256 + cx);
                        if (!pass) { m->exc_first[c]++; tot++; }
                        else m->exc_idx[m->exc_first[c] + fill[c]++] = (u16)e;
                    }
            }
        }
    }
    if (tmp && all) {
        static zpf_block fb[ZPF_MAXB];
        static zpf_tank ft[ZPF_MAXT];
        for (u32 t = 0; t < j->nt; t++) {
            m->first[t] = nb;
            if (cr.on && cr.first && cr.first[t + 1] > cr.first[t] && m->exc_first) {   /* a city tile */
                u32 n = cr.first[t + 1] - cr.first[t];
                if (nb + n > 192u * 1024u) n = 192u * 1024u - nb;
                memcpy(all + nb, cr.b + cr.first[t], sizeof(zp_box) * n);
                nb += n; m->ncity += n;
                m->city[t] = 1;
                ntiles++;
                continue;
            }
            for (u32 k = 0; k < 4096u; k++) memcpy(rgb + 3 * k, j->pal[j->idx[t * 4096u + k]], 3);
            u32 n = zp_find(rgb, t, tmp, 256u, m->wall[t]);
            if (!(zp_kinds & ZP_K_ROOFS)) n = 0;
            nroof += n;
            if ((zp_kinds & ZP_K_DESERT) && dir >= 0 && dir < 4 && land[t]) {
                int nfb = 0, nft = 0;
                zpf_tile(rgb, dir, lmap, fb, &nfb, ft, &nft);
                for (int i = 0; (zp_kinds & ZP_K_BLOCKS) && i < nfb && n < 512u; i++) {
                    zp_box *o = &tmp[n++];
                    memset(o, 0, sizeof *o);
                    o->cu = 0.5f * (float)(fb[i].x0 + fb[i].x1 + 1); o->cv = 0.5f * (float)(fb[i].y0 + fb[i].y1 + 1);
                    o->hl = 0.5f * (float)(fb[i].x1 + 1 - fb[i].x0); o->hw = 0.5f * (float)(fb[i].y1 + 1 - fb[i].y0);
                    o->au = 1.0f; o->av = 0.0f; o->kind = 2u;
                    o->slen = (float)(fb[i].k - 1) + fb[i].f;
                    zp_wall_texel(rgb, (float)fb[i].x0, (float)fb[i].y0, (float)fb[i].x1, (float)fb[i].y1, o->cu, o->cv, 0.0f, &o->wu, &o->wv);
                    nblk++;
                }
                for (int i = 0; (zp_kinds & ZP_K_TANKS) && i < nft && n < 512u; i++) {
                    zp_box *o = &tmp[n++];
                    memset(o, 0, sizeof *o);
                    o->cu = ft[i].cu; o->cv = ft[i].cv; o->hl = o->hw = ft[i].r;
                    o->au = 1.0f; o->av = 0.0f; o->kind = 3u;
                    zp_wall_texel(rgb, ft[i].cu - ft[i].r, ft[i].cv - ft[i].r, ft[i].cu + ft[i].r, ft[i].cv + ft[i].r, ft[i].cu, ft[i].cv,
                                  ft[i].r - 1.0f, &o->wu, &o->wv);
                    ntank++;
                }
            }
            if (n) ntiles++;
            if (nb + n > 192u * 1024u) n = 192u * 1024u - nb;
            memcpy(all + nb, tmp, sizeof(zp_box) * n);
            nb += n;
        }
    }
    for (u32 t = tmp && all ? j->nt : 0u; t <= 1024u; t++) m->first[t] = nb;
    free(tmp);
    free(cr.first); free(cr.b);
    m->box = all; m->nbox = all ? nb : 0u;
    u32 cells = 0, over = 0;
    for (u32 c = 0; c < 65536u; c++) {
        u32 t = m->grid[c] & 0x3FFu;
        if (t < m->nt && !(m->grid[c] & 0x800u) && m->first[t + 1] > m->first[t]) { cells++; over += m->first[t + 1] - m->first[t]; }
    }
    m->prev = zp_pub;
    if (m->prev && m->prev->prev) {
        free(m->prev->prev->box); free(m->prev->prev->ex); free(m->prev->prev->exc_first); free(m->prev->prev->exc_idx);
        free(m->prev->prev); m->prev->prev = NULL;
    }
    __atomic_store_n(&zp_pub, m, __ATOMIC_RELEASE);
    if (zp_kinds & ZP_K_DESERT)
        ps2_log("props: map %08X: %u red roofs, %u buildings, %u tanks found by their shadows (shadows %s%s, land median "
                "luminance %.0f), %u city buildings in %u of %u tiles; %u cells carry them (%u over the map), %u areas left out "
                "(static objects); built in %llu fields", m->sig, nroof, nblk, ntank, zp_dir_name[dir >= 0 && dir < 4 ? dir : 4],
                j->dir == 4 ? ", the photo's vote" : j->dir >= 0 && zp_shadow_cfg < 0 ? ", the mission's sun" : "", lmap, m->ncity, ntiles,
                m->nt, cells, over, m->nex, (unsigned long long)(ps2_kernel_vblank_count() - j->f0));
    else
    ps2_log("props: map %08X: %u houses in %u tiles (%u city buildings); %u cells carry houses (%u over the map), %u areas "
            "left out (static objects); built in %llu fields", m->sig, nb, m->nt, m->ncity, cells, over, m->nex,
            (unsigned long long)(ps2_kernel_vblank_count() - j->f0));
    free(j->idx);
    free(j);
    zp_busy = 0;
    return NULL;
}

static void zp_build(ps2_ctx *ctx) {
    (void)ctx;
    if (zp_busy) return;
    u32 T = zp_T, O = zp_O;
    u32 a0 = ps2_r32(T + 0x24u), a1 = ps2_r32(T + 0x28u), cl = ps2_r32(T + 0x20u);
    u32 nt = (zp_ptr(a0) && zp_ptr(a1) && a1 > a0) ? (a1 - a0) / 4096u : 0u;
    zp_built_sig = zp_sig;
    if (!nt || nt > 1024u || !zp_ptr(cl)) { ps2_log("props: map %08X: no atlas (%08X %08X %08X)", zp_sig, a0, a1, cl); return; }
    zp_job *j = (zp_job *)calloc(1, sizeof *j);
    zp_map *m = (zp_map *)calloc(1, sizeof *m);
    u8 *idx = (u8 *)malloc((size_t)nt * 4096u);
    if (!j || !m || !idx) { free(j); free(m); free(idx); return; }
    j->m = m; j->idx = idx; j->nt = nt; j->f0 = ps2_kernel_vblank_count();
    j->dir = zp_shadow_cfg >= 0 ? zp_shadow_cfg : 4;
#ifdef ZP_C1
    if (zp_shadow_cfg < 0 && (zp_kinds & ZP_K_DESERT)) {  /* the mission's sun: shadows fall away from it */
        static ps2_cloud_env e;
        if (ps2_cloud_env_read(&e) && (e.valid & CL_ENV_SUN)) {
            float sx = -e.sun_dir[0], sz = -e.sun_dir[2];
            j->dir = fabsf(sz) >= fabsf(sx) ? (sz < 0.0f ? 0 : 1) : (sx < 0.0f ? 2 : 3);
            ps2_log("props: map %08X: the mission's sun %.3f %.3f %.3f (elevation %.0f deg): shadows %s", zp_sig, e.sun_dir[0],
                    e.sun_dir[1], e.sun_dir[2], atan2f(e.sun_dir[1], sqrtf(e.sun_dir[0] * e.sun_dir[0] + e.sun_dir[2] * e.sun_dir[2])) * 57.29578f,
                    zp_dir_name[j->dir]);
        }
    }
#endif
    m->sig = zp_sig; m->nt = nt; m->serial = ++zp_map_serial;
    for (int cz = 0; cz < 256; cz++) for (int cx = 0; cx < 256; cx++) m->grid[cz * 256 + cx] = (u16)zp_tile(T, cx, cz);
    for (u32 i = 0; i < 256u; i++) {                      /* CSM1: bits 3 and 4 of the index swapped */
        u32 s = (i & 0xE7u) | ((i & 0x08u) << 1) | ((i & 0x10u) >> 1);
        for (int k = 0; k < 3; k++) j->pal[i][k] = ps2_r8(cl + s * 4u + (u32)k);
    }
    for (u32 k = 0; k < nt * 4096u; k += 4u) { u32 w = ps2_r32(a0 + k); memcpy(idx + k, &w, 4); }
    /* areas left out: the game's static objects (their bounds, grown by ACZ_TERRAIN_PROPS_CLEAR) */
    u32 ni = ps2_r32(O + 0x20u), inst = ps2_r32(O + 0x24u), nm = ps2_r32(O + 0x28u), models = ps2_r32(O + 0x2Cu);
    if (zp_ptr(inst) && zp_ptr(models) && ni < 65536u && nm < 4096u) {
        m->ex = (float (*)[4])calloc(ni ? ni : 1u, sizeof *m->ex);
        for (u32 i = 0; m->ex && i < ni; i++) {
            u32 a = inst + i * 16u, md = ps2_r32(a + 12u) & 0x7FFFFu;
            if (md >= nm) continue;
            u32 bb = ps2_r32(models + md * 0x90u + 0x40u);
            if (!zp_ptr(bb)) continue;
            float x = zp_f(a), z = zp_f(a + 8u);
            float *e = m->ex[m->nex++];
            e[0] = x + zp_f(bb + 8u) - zp_clear; e[1] = z + zp_f(bb + 0x18u) - zp_clear;
            e[2] = x + zp_f(bb + 0xCu) + zp_clear; e[3] = z + zp_f(bb + 0x1Cu) + zp_clear;
        }
    }
    zp_busy = 1;
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &at, zp_build_thread, j) != 0) {
        ps2_log("props: build thread failed");
        free(m->ex); free(m); free(idx); free(j);
        zp_busy = 0;
    }
    pthread_attr_destroy(&at);
}

/* ---- renderer side (GS replay thread): one mesh draw per fine terrain patch with houses ---------------------------- */
static float zp_sun[3] = { 0.45f, 0.7f, -0.55f };
static u32 zp_sun_frame = 0xFFFFFFFFu;

typedef struct { float p[4]; s32 n[4]; s32 c[4]; float uv[4]; } zp_rec;   /* the mesh record (64 bytes) */
/* a wall's light (x the ground's vertex colour) by its facing to the sun, f = cos of the horizontal angle */
static float zp_wall_k(float f) { return 0.58f + 0.30f * (f > 0.0f ? f : 0.0f); }
#define ZP_WALL_FOOT 0.80f   /* buildings and tanks: the vertex light at the wall's foot (a soft dark band near the ground) */

/* the drawn ground of a patch: its own triangles (a fine patch: a fan of 8 around its centre; a coarse one: 2) */
static float zp_ground(const float P[10][4], const unsigned *idx, unsigned ni, float x, float z, int *ok) {
    for (unsigned t = 0; t + 2u < ni + 1u && t + 2u < ni; t += 3u) {
        if (idx[t] > 9u || idx[t + 1] > 9u || idx[t + 2] > 9u) continue;
        const float *a = P[idx[t]], *b = P[idx[t + 1]], *c = P[idx[t + 2]];
        float d = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2]);
        if (fabsf(d) < 1e-3f) continue;
        float l1 = ((b[2] - c[2]) * (x - c[0]) + (c[0] - b[0]) * (z - c[2])) / d;
        float l2 = ((c[2] - a[2]) * (x - c[0]) + (a[0] - c[0]) * (z - c[2])) / d;
        float l3 = 1.0f - l1 - l2;
        if (l1 >= -1e-4f && l2 >= -1e-4f && l3 >= -1e-4f) { *ok = 1; return l1 * a[1] + l2 * b[1] + l3 * c[1]; }
    }
    *ok = 0;
    return P[0][1];
}

/* the renderer: the mission's sun for the wall shading (C1, zero_clouds.c), now and then */
static void zp_sun_update(void) {
#ifdef ZP_C1
    if (!zp_on || (ps2_gs_frame_count - zp_sun_frame) < 120u) return;
    zp_sun_frame = ps2_gs_frame_count;
    static ps2_cloud_env e;
    if (ps2_cloud_env_read(&e) && (e.valid & CL_ENV_SUN)) {
        float l = sqrtf(e.sun_dir[0] * e.sun_dir[0] + e.sun_dir[1] * e.sun_dir[1] + e.sun_dir[2] * e.sun_dir[2]);
        if (l > 0.5f && l < 1.5f) for (int k = 0; k < 3; k++) zp_sun[k] = e.sun_dir[k] / l;
    }
#endif
}

/* ---- stage 3, the city: one draw per city cell, its geometry cached per cell ------------------------------------
   A city cell holds ~50-300 buildings (M6's residential districts); generating them every frame would cost the GS
   thread ~1 us each. The cell's vertices depend only on the patch's records (its ground triangles, texture mapping and
   vertex colour), the cell's grow level and the units' clear areas, so they are made once and kept in a small cache
   (ZP_CCN cells, the least recently used replaced): a frame then only copies them into the mesh arena. Cells grow out
   of their roofs as a whole over the outer 30 % of ACZ_TERRAIN_CITY_DIST (16 levels, from the cell centre's distance
   to the eye), so no building pops up. */
typedef struct {
    const zp_map *m; u32 t; int cx, cz;
    const float (*P)[4]; const unsigned *idx; unsigned nidx; const s32 *col; float uvz[2];
    float umin, du, vmin, dv, uv0[2], Ai[2][2], x0, z0, x1, z1, sx, sz, sy;
    const float (*uxz)[2]; int nux;
} zp_pc;
typedef struct { u32 cell, key, used, nv, ni, nh, capv, capi; zp_rec *vb; u32 *ib; u32 dk[4]; } zp_cce;
#define ZP_CCN 512u
static zp_cce zp_cc[ZP_CCN];
static u32 zp_cc_serial;
static u32 zp_cc_clock;
static u64 zp_st_city_draws, zp_st_city_gen, zp_st_city_hit, zp_st_city_budget, zp_st_city_bld, zp_st_city_verts;
static u32 zp_city_fverts, zp_city_fcells, zp_city_fgen;   /* per GS frame (debug) */
static u64 zp_city_gen_ns, zp_city_store_ns, zp_city_draw_ns, zp_city_key_ns;

static int zp_cc_grow(zp_cce *e, u32 nv, u32 ni) {
    if (nv > e->capv) {
        u32 c = nv + nv / 2u + 256u;
        zp_rec *v = (zp_rec *)realloc(e->vb, sizeof(zp_rec) * c);
        if (!v) return 0;
        e->vb = v; e->capv = c;
    }
    if (ni > e->capi) {
        u32 c = ni + ni / 2u + 384u;
        u32 *q = (u32 *)realloc(e->ib, sizeof(u32) * c);
        if (!q) return 0;
        e->ib = q; e->capi = c;
    }
    return 1;
}

/* a city wall's light (mesh.frag, per pixel from the face's own normal: the 4 walls share their corners) is
   0.50 + 0.40 max(f, 0) with f the cos of the horizontal angle to the sun -- the shadowed sides darker than the red-roof
   houses' 0.58 (the dusk of mission 6 reads as depth), the sunlit up to 0.90; the foot darker still (the street's shade) */
#define ZP_CITY_FOOT 0.70f

/* the cell's buildings at grow g (0..1) into e; walls whose outward normal n has n . cd < cth are left out (far cells:
   cd = the horizontal direction from the cell to the eye, cth < -1 keeps all) */
static void zp_city_gen(zp_cce *e, const zp_pc *pc, float g, const float cd[2], float cth) {
    const zp_map *m = pc->m;
    u32 b0 = m->first[pc->t], b1 = m->first[pc->t + 1];
    e->nv = e->ni = e->nh = 0;
    if (!zp_cc_grow(e, (b1 - b0) * 18u, (b1 - b0) * 42u)) return;
    const float (*P)[4] = pc->P;
    const s32 *col = pc->col;
    u32 cell = (u32)(pc->cz * 256 + pc->cx);
    u32 ex0 = m->exc_first ? m->exc_first[cell] : 0u, ex1 = m->exc_first ? m->exc_first[cell + 1] : 0u;
    zp_rec *vb = e->vb;
    u32 *ib = e->ib, nv = 0, ni = 0, nh = 0;
#define ZP_TEX2W(tu, tv, ox, oz) do { float uu_ = pc->umin + (tu) * pc->du - pc->uv0[0], vv_ = pc->vmin + (tv) * pc->dv - pc->uv0[1]; \
        ox = pc->x0 + pc->Ai[0][0] * uu_ + pc->Ai[0][1] * vv_; oz = pc->z0 + pc->Ai[1][0] * uu_ + pc->Ai[1][1] * vv_; } while (0)
#define ZP_V(X, Y, Z, U, V, K) do { zp_rec *r_ = &vb[nv++]; \
        r_->p[0] = (X); r_->p[1] = (Y); r_->p[2] = (Z); r_->p[3] = pw; memcpy(r_->n, fac, sizeof fac); \
        for (int c_ = 0; c_ < 3; c_++) { int v_ = (int)((float)col[c_] * (K) + 0.5f); r_->c[c_] = v_ > 255 ? 255 : v_; } \
        r_->c[3] = col[3]; r_->uv[0] = (U); r_->uv[1] = (V); r_->uv[2] = pc->uvz[0]; r_->uv[3] = pc->uvz[1]; } while (0)
    s32 fac[4] = { 0, 0, 0, 0 };                          /* quadword 1: 0 = no facade (roofs, gables) */
    float pw = 1.0f;                                      /* the position's w (the detailed look: the heights) */
    for (u32 i = b0; i < b1; i++) {
        const zp_box *h = &m->box[i];
        float cxw, czw;
        ZP_TEX2W(h->cu, h->cv, cxw, czw);
        u32 bh = zp_hash(i * 2246822519u + pc->t * 3266489917u + 7u);
        float tint = 0.88f + 0.24f * (float)(bh & 255u) / 255.0f;   /* +-12 % per building */
        fac[0] = fac[1] = fac[2] = fac[3] = 0;
        int skip = 0;
        for (u32 k = ex0; k < ex1 && !skip; k++) {
            const float *x = m->ex[m->exc_idx[k]];
            skip = cxw >= x[0] && cxw <= x[2] && czw >= x[1] && czw <= x[3];
        }
        if (skip) continue;
        if (pc->nux) {                                    /* a ground unit (target) near: left as the photo */
            float rad = (h->hl > h->hw ? h->hl : h->hw) * 45.3f + zp_unit_clear;
            for (int k = 0; k < pc->nux && !skip; k++) {
                float dx = cxw - pc->uxz[k][0], dz = czw - pc->uxz[k][1];
                skip = dx * dx + dz * dz < rad * rad;
            }
            if (skip) { zp_st_unit_skip++; continue; }
        }
        float tu[4], tv[4], wx[4], wz[4], gy = -1e30f, gmin = 1e30f;
        const float su[4] = { -1, 1, 1, -1 }, sv[4] = { -1, -1, 1, 1 };
        for (int k = 0; k < 4; k++) {
            tu[k] = h->cu + su[k] * h->hl * h->au - sv[k] * h->hw * h->av;
            tv[k] = h->cv + su[k] * h->hl * h->av + sv[k] * h->hw * h->au;
            ZP_TEX2W(tu[k], tv[k], wx[k], wz[k]);
            int ok;
            float gq = zp_ground(P, pc->idx, pc->nidx, wx[k], wz[k], &ok);
            if (!ok) { skip = 1; break; }
            if (gq > gy) gy = gq;
            if (gq < gmin) gmin = gq;
        }
        if (skip) continue;
        { int ok; float gq = zp_ground(P, pc->idx, pc->nidx, cxw, czw, &ok); if (ok && gq > gy) gy = gq; }
        float top = gy + h->eave * g, bot = gmin - 8.0f, ridge = h->ridge * g;
        float hwu = pc->umin + h->wu * pc->du, hwv = pc->vmin + h->wv * pc->dv;   /* the facade texel */
        /* the detailed look (ACZ_TERRAIN_CITY_LOOK, city_look.glsl): the half length / width in 0.5 m steps in the
           records' quadword 1 w bits 16-31, the wall's top and foot relative to the base (1/16 m; + 1024: a pitched
           roof) in the position's w, the roofs as kinds 3 (flat) / 4 (pitched) with their corners' local position
           (m x 16 along the length / width); without the look's push bit mesh.vert / mesh.frag read none of it */
        u32 hsz = 0u;
        s32 gy4 = (s32)lrintf(gy * 4.0f), rk = (s32)(((bh >> 8) & 255u) << 8);
        float hlm = h->hl * 6.25f, hwm = h->hw * 6.25f;
        pw = 1.0f;
        if (zp_city_look) {
            int hq = (int)lrintf(hlm * 2.0f), wq = (int)lrintf(hwm * 2.0f);
            hsz = ((u32)(hq < 0 ? 0 : hq > 255 ? 255 : hq) << 16) | ((u32)(wq < 0 ? 0 : wq > 255 ? 255 : wq) << 24);
            int tq = (int)lrintf((top - gy) / 5.12f * 16.0f), bq = (int)lrintf((gy - bot) / 5.12f * 16.0f);
            tq = tq < 0 ? 0 : tq > 4095 ? 4095 : tq;
            bq = bq < 0 ? 0 : bq > 1023 ? 1023 : bq;
            pw = (float)(tq * 2048 + bq + (h->ridge > 0.0f ? 1024 : 0));
        }
#define ZP_RF(KIND, LX, LZ) do { if (zp_city_look) { fac[0] = (s32)lrintf((LX) * 16.0f); fac[1] = (s32)lrintf((LZ) * 16.0f); \
        fac[2] = gy4; fac[3] = (s32)((u32)(KIND) | (u32)rk | hsz); } } while (0)
        float ruv[4][2], gx[2] = { 0.0f, 0.0f }, gz[2] = { 0.0f, 0.0f }, grt = -1e30f;
        for (int k = 0; k < 4; k++) { ruv[k][0] = pc->umin + tu[k] * pc->du; ruv[k][1] = pc->vmin + tv[k] * pc->dv; }
        if (h->ridge > 0.0f) {                            /* a pitched roof along the length; gables over the short walls */
            float rt = top + ridge, mu[2], mv[2], mx[2], mz[2];
            {   static int shown;                         /* (debug: where to look at one) */
                if (zp_dbg && shown < 4) { shown++; ps2_log("props: city: a pitched roof at %.0f %.0f (ground %.0f, %.1f x %.1f m)",
                                                            cxw, czw, gy, 2.0f * hlm, 2.0f * hwm); }
            }
            mu[0] = 0.5f * (tu[3] + tu[0]); mv[0] = 0.5f * (tv[3] + tv[0]);
            mu[1] = 0.5f * (tu[1] + tu[2]); mv[1] = 0.5f * (tv[1] + tv[2]);
            for (int k = 0; k < 2; k++) ZP_TEX2W(mu[k], mv[k], mx[k], mz[k]);
            for (int sl = 0; sl < 2; sl++) {
                int ka = sl ? 2 : 0, kb = sl ? 3 : 1;
                float ox = 0.5f * (wx[ka] + wx[kb]) - 0.5f * (mx[0] + mx[1]), oz = 0.5f * (wz[ka] + wz[kb]) - 0.5f * (mz[0] + mz[1]);
                float ol = sqrtf(ox * ox + oz * oz), f = ol > 1e-3f ? (ox * pc->sx + oz * pc->sz) / ol : 0.0f, k = 1.0f + 0.12f * f;
                u32 s0 = nv;
                ZP_RF(4, su[ka] * hlm, sv[ka] * hwm);
                ZP_V(wx[ka], top, wz[ka], ruv[ka][0], ruv[ka][1], k);
                ZP_RF(4, su[kb] * hlm, sv[kb] * hwm);
                ZP_V(wx[kb], top, wz[kb], ruv[kb][0], ruv[kb][1], k);
                int e0 = sl ? 0 : 1, e1 = sl ? 1 : 0;
                ZP_RF(4, e0 ? hlm : -hlm, 0.0f);
                ZP_V(mx[e0], rt, mz[e0], pc->umin + mu[e0] * pc->du, pc->vmin + mv[e0] * pc->dv, k);
                ZP_RF(4, e1 ? hlm : -hlm, 0.0f);
                ZP_V(mx[e1], rt, mz[e1], pc->umin + mu[e1] * pc->du, pc->vmin + mv[e1] * pc->dv, k);
                ib[ni++] = s0; ib[ni++] = s0 + 1; ib[ni++] = s0 + 2; ib[ni++] = s0; ib[ni++] = s0 + 2; ib[ni++] = s0 + 3;
            }
            for (int k = 0; k < 2; k++) { gx[k] = mx[k]; gz[k] = mz[k]; }
            grt = rt;
        } else {                                          /* a flat roof: the photo's texels exactly over their place */
            u32 s0 = nv;
            for (int k = 0; k < 4; k++) { ZP_RF(3, su[k] * hlm, sv[k] * hwm); ZP_V(wx[k], top, wz[k], ruv[k][0], ruv[k][1], 1.0f); }
            ib[ni++] = s0; ib[ni++] = s0 + 1; ib[ni++] = s0 + 2; ib[ni++] = s0; ib[ni++] = s0 + 2; ib[ni++] = s0 + 3;
        }
        /* walls: 8 corners shared by the four walls (and 2 gable apexes): the facade texel x the building's tint, the foot
           darker; their light by the facing to the sun and the windows come per pixel (mesh.frag, from the face's
           own normal), the facade data in the records' second quadword: the building's centre, base, kind and seed */
        fac[0] = (s32)lrintf(cxw); fac[1] = (s32)lrintf(czw); fac[2] = (s32)lrintf(gy * 4.0f);
        fac[3] = (s32)((zp_city_win ? 1u : 2u) | (((bh >> 8) & 255u) << 8) | hsz);
        u32 w0 = nv;
        for (int k = 0; k < 4; k++) ZP_V(wx[k], bot, wz[k], hwu, hwv, tint * ZP_CITY_FOOT);
        for (int k = 0; k < 4; k++) ZP_V(wx[k], top, wz[k], hwu, hwv, tint);
        for (int k = 0; k < 4; k++) {
            int k2 = (k + 1) & 3;
            float ex = wz[k2] - wz[k], ez = -(wx[k2] - wx[k]);
            float mxw = 0.5f * (wx[k] + wx[k2]) - cxw, mzw = 0.5f * (wz[k] + wz[k2]) - czw;
            if (ex * mxw + ez * mzw < 0.0f) { ex = -ex; ez = -ez; }
            float el = sqrtf(ex * ex + ez * ez);
            if (el > 1e-3f && (ex * cd[0] + ez * cd[1]) / el < cth) continue;   /* (facing away from the eye) */
            ib[ni++] = w0 + (u32)k; ib[ni++] = w0 + (u32)k2; ib[ni++] = w0 + 4u + (u32)k2;
            ib[ni++] = w0 + (u32)k; ib[ni++] = w0 + 4u + (u32)k2; ib[ni++] = w0 + 4u + (u32)k;
        }
        if (grt > -1e29f)                                 /* the gables over the short walls (3-0 and 1-2) */
            for (int k = 0; k < 2; k++) {
                int ka = k ? 1 : 3, kb = k ? 2 : 0;
                float ox = gx[k] - cxw, oz = gz[k] - czw, ol = sqrtf(ox * ox + oz * oz);
                if (ol > 1e-3f && (ox * cd[0] + oz * cd[1]) / ol < cth) continue;   /* (facing away from the eye) */
                ZP_V(gx[k], grt, gz[k], hwu, hwv, tint);
                ib[ni++] = w0 + 4u + (u32)ka; ib[ni++] = w0 + 4u + (u32)kb; ib[ni++] = nv - 1u;
            }
        fac[0] = fac[1] = fac[2] = fac[3] = 0;
        nh++;
    }
#undef ZP_RF
#undef ZP_V
#undef ZP_TEX2W
    e->nv = nv; e->ni = ni; e->nh = nh;
}

static void zp_city_patch(const zp_pc *pc, const unsigned char *recs, unsigned count, const ps2_vk_state *st,
                          const ps2_vk_mesh_block *b, float d) {
    /* the cell's distance from the eye (VR: p[13..15]; else the patch centre's view depth) and its grow level */
    float dmin = d, dcen = d;
    if (b->pad1 & 1u) {
        float ex = b->p[13], ey = b->p[14], ez = b->p[15];
        float dx = ex < pc->x0 ? pc->x0 - ex : ex > pc->x1 ? ex - pc->x1 : 0.0f;
        float dz = ez < pc->z0 ? pc->z0 - ez : ez > pc->z1 ? ez - pc->z1 : 0.0f, dy = ey - pc->P[0][1];
        dmin = sqrtf(dx * dx + dy * dy + dz * dz);
        float cx = 0.5f * (pc->x0 + pc->x1) - ex, cz = 0.5f * (pc->z0 + pc->z1) - ez;
        dcen = sqrtf(cx * cx + dy * dy + cz * cz);
    }
    if (dmin > zp_city_reach) return;
    /* far cells (over 600 m): the walls facing away from the eye are left out -- by the eye's direction in 10-degree
       sectors (part of the cache key) with a margin for the cell's extent and the sector's width */
    float cd[2] = { 0.0f, 0.0f }, cth = -2.0f;
    u32 sector = 0;
    if (b->pad1 & 1u) {
        float hx = b->p[13] - 0.5f * (pc->x0 + pc->x1), hz = b->p[15] - 0.5f * (pc->z0 + pc->z1), dh = sqrtf(hx * hx + hz * hz);
        if (dh > 3072.0f) {
            int sc = (int)floorf(atan2f(hz, hx) / 0.17453293f + 0.5f);
            sector = (u32)((sc % 36 + 36) % 36) + 1u;
            float a = (float)(sector - 1u) * 0.17453293f, half = 1448.0f / dh;
            cd[0] = cosf(a); cd[1] = sinf(a);
            cth = -sinf(asinf(half > 1.0f ? 1.0f : half) + 0.192f);   /* + 6 deg + half a sector */
        }
    }
    float g = (zp_city_reach - dcen) / (0.3f * zp_city_reach);
    g = g >= 1.0f ? 1.0f : g <= 0.0f ? 0.0f : g * g * (3.0f - 2.0f * g);
    u32 gl = (u32)(g * 16.0f + 0.5f);
    if (!gl) return;
    if (zp_cc_serial != pc->m->serial) {                /* a new map: the cache starts empty */
        for (u32 k = 0; k < ZP_CCN; k++) zp_cc[k].cell = 0xFFFFFFFFu;
        zp_cc_serial = pc->m->serial;
    }
    u32 cell = (u32)(pc->cz * 256 + pc->cx);
    u32 dk[4] = { 2166136261u, 2166136261u, 2166136261u, 0u };   /* positions, colour, texture mapping; the rest */
    for (u32 k = 0; k < count * 16u; k++) {             /* (not the second quadword: the materials' per-frame smooth */
        u32 q = (k & 15u) >> 2;                          /*  ground, zero_terrain_mat.c; not the position's w: the */
        if (q == 1u || (k & 15u) == 3u) continue;        /*  game's per-frame geomorph weight) */
        u32 w; memcpy(&w, recs + 4u * k, 4);
        u32 *h = &dk[q == 0u ? 0 : q == 2u ? 1 : 2];
        *h = (*h ^ w) * 16777619u;
    }
    dk[3] = gl * 0x9E3779B1u ^ sector * 0xC2B2AE35u;
    dk[3] ^= (u32)zp_ucell[cell] * 0x85EBCA77u;          /* the units near this cell (a version) */
    u32 key = dk[0] ^ (dk[1] * 0x27D4EB2Fu) ^ (dk[2] * 0x165667B1u) ^ dk[3];
    u32 hs = (cell * 2654435761u) >> 23, slot = ZP_CCN, old = hs;
    for (u32 q = 0; q < 16u; q++) {
        u32 k = (hs + q) & (ZP_CCN - 1u);
        if (zp_cc[k].cell == cell) { slot = k; break; }
        if (zp_cc[k].cell == 0xFFFFFFFFu || zp_cc_clock - zp_cc[k].used > zp_cc_clock - zp_cc[old].used) old = k;
    }
    if (slot == ZP_CCN) slot = old;
    zp_cce *e = &zp_cc[slot];
    zp_cc_clock++;
    if (e->cell != cell || e->key != key) {
        static int shown;
        if (zp_dbg && e->cell == cell && shown < 60) {
            shown++;
            ps2_log("props: city cell %d,%d remade at GS frame %u: positions %s, colour %s, mapping %s, grow / sector / units %s "
                    "(grow %u, sector %u)", pc->cx, pc->cz, ps2_gs_frame_count, e->dk[0] != dk[0] ? "changed" : "same",
                    e->dk[1] != dk[1] ? "changed" : "same", e->dk[2] != dk[2] ? "changed" : "same",
                    e->dk[3] != dk[3] ? "changed" : "same", gl, sector);
        }
        memcpy(e->dk, dk, sizeof dk);
        u64 t0 = zp_now_ns();
        e->cell = cell; e->key = key;
        zp_city_gen(e, pc, (float)gl / 16.0f, cd, cth);
        zp_city_gen_ns += zp_now_ns() - t0;
        zp_st_city_gen++; zp_city_fgen++;
    } else zp_st_city_hit++;
    e->used = zp_cc_clock;
    if (!e->nv) return;
    if (zp_city_fverts + e->nv > zp_city_budget || !ps2_vk_mesh_room(e->nv, e->ni, 1u)) { zp_st_city_budget++; return; }
    u32 vbase = 0, ifirst = 0;
    u64 t1 = zp_dbg ? zp_now_ns() : 0u;
    if (ps2_vk_mesh_store((const u8 *)e->vb, e->nv, e->ib, e->ni, &vbase, &ifirst) != 0) { zp_st_city_budget++; return; }
    u64 t2 = zp_dbg ? zp_now_ns() : 0u;
    zp_city_store_ns += t2 - t1;
    zp_city_fverts += e->nv; zp_city_fcells++;
    ps2_vk_mesh_block hb = *b;
    hb.pass = 0u;
    hb.clip = 1u;
    hb.pad4 |= PS2_PROPS_PAD4;
    hb.r[0] = pc->sx; hb.r[1] = pc->sz; hb.r[2] = 1.0f;  /* the sun (horizontal) for the facades' light (mesh.vert) */
    if (zp_city_look) hb.r[3] = pc->sy;                  /* (the detailed look: the sun's height) */
    ps2_vk_draw_mesh(st, &hb, vbase, ifirst, e->ni, 0);
    if (zp_dbg) zp_city_draw_ns += zp_now_ns() - t2;
    zp_st_city_draws++; zp_st_city_bld += e->nh; zp_st_city_verts += e->nv;
    zp_fs[4]++; zp_fs[5] += e->nh;
}

static void zp_patch(const unsigned char *recs, unsigned count, const unsigned *idx, unsigned nidx, const ps2_vk_state *st,
                     const ps2_vk_mesh_block *b);
void zero_props_patch(const unsigned char *recs, unsigned count, const unsigned *idx, unsigned nidx, const ps2_vk_state *st,
                      const ps2_vk_mesh_block *b) {
    if (!zp_on || !zp_pub) return;
    u64 t0 = zp_dbg ? zp_now_ns() : 0u;
    zp_patch(recs, count, idx, nidx, st, b);
    if (zp_dbg) zp_fs_ns += zp_now_ns() - t0;
}
static void zp_patch(const unsigned char *recs, unsigned count, const unsigned *idx, unsigned nidx, const ps2_vk_state *st,
                     const ps2_vk_mesh_block *b) {
    const zp_map *m = __atomic_load_n(&zp_pub, __ATOMIC_ACQUIRE);
    if (!zp_on || !m || (count != 10u && count != 4u) || !recs || !idx || !st || !b) return;
    if (zp_frame_at != ps2_gs_frame_count) {
        if (zp_dbg && (zp_st_frames % 300u) == 0u)
            ps2_log("props: GS frame %u: patches fine %u coarse %u, within reach %u, with houses %u; draws %u, houses %u, %u vertices; "
                    "hook %.1f us (mean of the last frames %.1f us)", zp_frame_at, zp_fs[0], zp_fs[1], zp_fs[2], zp_fs[3], zp_fs[4],
                    zp_fs[5], zp_frame_verts, (double)zp_fs_ns / 1000.0, zp_fs_ns_n ? (double)zp_fs_ns_sum / (double)zp_fs_ns_n / 1000.0 : 0.0);
        if (zp_dbg && (zp_st_frames % 300u) == 0u && (zp_kinds & ZP_K_CITY))
            ps2_log("props: GS frame %u: city cells %u, %u vertices (budget %u), %u cells made this frame; made %llu, reused %llu, "
                    "over budget %llu so far; making %.1f us per cell, store %.1f, draw %.1f us per cell draw; reach %.0f m", zp_frame_at,
                    zp_city_fcells, zp_city_fverts, zp_city_budget, zp_city_fgen, (unsigned long long)zp_st_city_gen,
                    (unsigned long long)zp_st_city_hit, (unsigned long long)zp_st_city_budget,
                    zp_st_city_gen ? (double)zp_city_gen_ns / 1000.0 / (double)zp_st_city_gen : 0.0,
                    zp_st_city_draws ? (double)zp_city_store_ns / 1000.0 / (double)zp_st_city_draws : 0.0,
                    zp_st_city_draws ? (double)zp_city_draw_ns / 1000.0 / (double)zp_st_city_draws : 0.0, zp_city_reach / 5.12f);
        if (zp_dbg && (zp_st_frames % 300u) == 0u) { zp_fs_ns_sum = 0; zp_fs_ns_n = 0; }
        zp_fs_ns_sum += zp_fs_ns; zp_fs_ns_n++; zp_fs_ns = 0;
        memset(zp_fs, 0, sizeof zp_fs);
        zp_frame_at = ps2_gs_frame_count; zp_frame_verts = 0; zp_st_frames++; zp_sun_update();
        /* the city's reach: a frame near the vertex budget pulls it in by 3 % (the far buildings sink smoothly through
           the moving grow band, never a cell dropped at random), a light one lets it out again by 1 % */
        if (zp_city_fverts > zp_city_budget / 20u * 17u) zp_city_reach = zp_city_reach * 0.97f > 4096.0f ? zp_city_reach * 0.97f : 4096.0f;
        else if (zp_city_fverts < zp_city_budget / 20u * 13u) zp_city_reach = zp_city_reach * 1.01f < zp_city_dist ? zp_city_reach * 1.01f : zp_city_dist;
        zp_city_fverts = 0; zp_city_fcells = 0; zp_city_fgen = 0;
    }
    zp_fs[count == 10u ? 0 : 1]++;
    float P[10][4], UV[10][4];
    s32 col[4];
    for (u32 k = 0; k < count; k++) { memcpy(P[k], recs + k * 64u, 16); memcpy(UV[k], recs + k * 64u + 48u, 16); }
    memcpy(col, recs + 32u, 16);
    float x0 = P[0][0], z0 = P[0][2], x1 = x0, z1 = z0;
    for (u32 k = 1; k < count; k++) {
        if (P[k][0] < x0) x0 = P[k][0];
        if (P[k][0] > x1) x1 = P[k][0];
        if (P[k][2] < z0) z0 = P[k][2];
        if (P[k][2] > z1) z1 = P[k][2];
    }
    if (!(x1 - x0 > 2000.0f && x1 - x0 < 2100.0f && z1 - z0 > 2000.0f && z1 - z0 < 2100.0f)) return;
    int cx = ((int)floorf((x0 + 262144.0f) / 2048.0f + 0.5f)) & 255, cz = ((int)floorf((z0 + 262144.0f) / 2048.0f + 0.5f)) & 255;
    u32 tile = m->grid[cz * 256 + cx], t = tile & 0x3FFu;
    if (t >= m->nt || (tile & 0x800u)) return;
    u32 b0 = m->first[t], b1 = m->first[t + 1];
    /* distance: from the eye (rn_vp_terrain.c puts it in p[13..15] in VR), else the view depth */
    float d, pc[3] = { 0.5f * (x0 + x1), P[0][1], 0.5f * (z0 + z1) };
    if (b->pad1 & 1u) {
        float dx = pc[0] - b->p[13], dy = pc[1] - b->p[14], dz = pc[2] - b->p[15];
        d = sqrtf(dx * dx + dy * dy + dz * dz);
    } else d = b->c[3] * pc[0] + b->c[7] * pc[1] + b->c[11] * pc[2] + b->c[15];
    if (d > (zp_dist > zp_city_dist ? zp_dist : zp_city_dist) + 1450.0f) return;
    zp_fs[2]++;
    if (b1 <= b0) return;
    zp_fs[3]++;
    /* the patch's texture mapping uv(x, z) = uv0 + A (x - x0, z - z0) from three of its corners */
    int i00 = -1, i10 = -1, i01 = -1;
    for (int k = 0; k < (int)count; k++) {
        int lx = P[k][0] > x0 + 1024.0f, lz = P[k][2] > z0 + 1024.0f, mx = fabsf(P[k][0] - x0 - 1024.0f) < 64.0f,
            mz = fabsf(P[k][2] - z0 - 1024.0f) < 64.0f;
        if (mx || mz) continue;
        if (!lx && !lz) i00 = k;
        if (lx && !lz) i10 = k;
        if (!lx && lz) i01 = k;
    }
    if (i00 < 0 || i10 < 0 || i01 < 0) return;
    float A[2][2], uv0[2];
    for (int c = 0; c < 2; c++) {
        uv0[c] = UV[i00][c];
        A[c][0] = (UV[i10][c] - UV[i00][c]) / (P[i10][0] - P[i00][0]);
        A[c][1] = (UV[i01][c] - UV[i00][c]) / (P[i01][2] - P[i00][2]);
    }
    float det = A[0][0] * A[1][1] - A[0][1] * A[1][0];
    if (!(fabsf(det) > 1e-14f)) return;
    /* the tile's region in uv: the corners' extents; texel (u, v) of the tile -> uv -> world */
    float umin = 1e30f, umax = -1e30f, vmin = 1e30f, vmax = -1e30f;
    for (int k = 0; k < 4; k++) {
        float wx = (k & 1) ? 2048.0f : 0.0f, wz = (k & 2) ? 2048.0f : 0.0f;
        float u = uv0[0] + A[0][0] * wx + A[0][1] * wz, v = uv0[1] + A[1][0] * wx + A[1][1] * wz;
        if (u < umin) umin = u;
        if (u > umax) umax = u;
        if (v < vmin) vmin = v;
        if (v > vmax) vmax = v;
    }
    float du = (umax - umin) / 64.0f, dv = (vmax - vmin) / 64.0f;
    float Ai[2][2] = { { A[1][1] / det, -A[0][1] / det }, { -A[1][0] / det, A[0][0] / det } };
#define ZP_TEX2W(tu, tv, ox, oz) do { float uu_ = umin + (tu) * du - uv0[0], vv_ = vmin + (tv) * dv - uv0[1]; \
        ox = x0 + Ai[0][0] * uu_ + Ai[0][1] * vv_; oz = z0 + Ai[1][0] * uu_ + Ai[1][1] * vv_; } while (0)
    {   static int shown;                                /* (debug: the mapping of the first patches) */
        if (zp_dbg && shown < 3) {
            shown++;
            ps2_log("props: patch cell %d,%d tile %03X at %.0f,%.0f: uv0 %.5f %.5f A %.3g %.3g / %.3g %.3g region u %.5f..%.5f "
                    "v %.5f..%.5f zw %.5f %.5f tex %.0fx%.0f fst %d region %d col %d %d %d %d", cx, cz, t, x0, z0, uv0[0],
                    uv0[1], A[0][0], A[0][1], A[1][0], A[1][1], umin, umax, vmin, vmax, UV[0][2], UV[0][3], st->tex_w,
                    st->tex_h, st->fst, st->native_region, col[0], col[1], col[2], col[3]);
        }
    }
    float sun[3];
    memcpy(sun, zp_sun, sizeof sun);
    float sh = sqrtf(sun[0] * sun[0] + sun[2] * sun[2]);
    float sx = sh > 1e-3f ? sun[0] / sh : 0.0f, sz = sh > 1e-3f ? sun[2] / sh : 0.0f;
    float tanel = sun[1] / (sh > 1e-3f ? sh : 1e-3f);
    tanel = tanel < 0.3f ? 0.3f : tanel > 3.0f ? 3.0f : tanel;
    /* ground units near this cell (their clear areas), so nothing is raised over or near a target */
    float uxz[64][2];
    int nux = 0;
    if (zp_ucell[(u32)cz * 256u + (u32)cx]) {
        u32 nu = __atomic_load_n(&zp_nu, __ATOMIC_ACQUIRE);
        float ur = zp_unit_clear + 800.0f;
        for (u32 i = 0; i < nu && nux < 64; i++)
            if (zp_ux[i][0] > x0 - ur && zp_ux[i][0] < x1 + ur && zp_ux[i][1] > z0 - ur && zp_ux[i][1] < z1 + ur) {
                uxz[nux][0] = zp_ux[i][0]; uxz[nux][1] = zp_ux[i][1]; nux++;
            }
    }
    if (m->city[t]) {                                    /* stage 3: a city cell (cached geometry, zp_city_patch) */
        zp_pc pc;
        pc.m = m; pc.t = t; pc.cx = cx; pc.cz = cz; pc.P = (const float (*)[4])P; pc.idx = idx; pc.nidx = nidx; pc.col = col;
        pc.uvz[0] = UV[0][2]; pc.uvz[1] = UV[0][3];
        pc.umin = umin; pc.du = du; pc.vmin = vmin; pc.dv = dv; pc.uv0[0] = uv0[0]; pc.uv0[1] = uv0[1];
        memcpy(pc.Ai, Ai, sizeof pc.Ai);
        pc.x0 = x0; pc.z0 = z0; pc.x1 = x1; pc.z1 = z1; pc.sx = sx; pc.sz = sz; pc.uxz = (const float (*)[2])uxz; pc.nux = nux;
        pc.sy = sun[1];
        zp_city_patch(&pc, recs, count, st, b, d);
        return;
    }
    static zp_rec vb[4096];
    static u32 ib[6144];
    u32 nv = 0, ni = 0, nh = 0;
    float wu = umin + m->wall[t][0] * du, wv = vmin + m->wall[t][1] * dv;   /* the wall texel's uv */
    for (u32 i = b0; i < b1 && nv + 64u <= 4096u && ni + 120u <= 6144u; i++) {
        const zp_box *h = &m->box[i];
        float cxw, czw;
        ZP_TEX2W(h->cu, h->cv, cxw, czw);
        int skip = 0;
        for (u32 e = 0; e < m->nex && !skip; e++)
            skip = cxw >= m->ex[e][0] && cxw <= m->ex[e][2] && czw >= m->ex[e][1] && czw <= m->ex[e][3];
        if (skip) continue;
        if (nux) {                                        /* a ground unit (target) near: left as the photo */
            float rad = (h->hl > h->hw ? h->hl : h->hw) * 45.3f + zp_unit_clear;
            for (int e = 0; e < nux && !skip; e++) {
                float dx = cxw - uxz[e][0], dz = czw - uxz[e][1];
                skip = dx * dx + dz * dz < rad * rad;
            }
            if (skip) { zp_st_unit_skip++; continue; }
        }
        float dh = d;                                     /* each house by its own distance from the eye (VR) */
        if (b->pad1 & 1u) { float dx = cxw - b->p[13], dz = czw - b->p[15]; dh = sqrtf(dx * dx + dz * dz); }
        if (dh > zp_dist) continue;
        /* the outer 15 % of the reach: the houses grow out of their roofs' photo instead of popping up (world-fixed) */
        float grow = (zp_dist - dh) / (0.15f * zp_dist);
        grow = grow >= 1.0f ? 1.0f : grow * grow * (3.0f - 2.0f * grow);
        if (grow < 0.02f) continue;
        float hwu = h->wu >= 0.0f ? umin + h->wu * du : wu, hwv = h->wv >= 0.0f ? vmin + h->wv * dv : wv;   /* wall texel */
#define ZP_V(X, Y, Z, U, V, K) do { zp_rec *r_ = &vb[nv++]; memset(r_, 0, sizeof *r_); \
        r_->p[0] = (X); r_->p[1] = (Y); r_->p[2] = (Z); r_->p[3] = 1.0f; \
        for (int c_ = 0; c_ < 3; c_++) { int v_ = (int)((float)col[c_] * (K) + 0.5f); r_->c[c_] = v_ > 255 ? 255 : v_; } \
        r_->c[3] = col[3]; r_->uv[0] = (U); r_->uv[1] = (V); r_->uv[2] = UV[0][2]; r_->uv[3] = UV[0][3]; } while (0)
        if (h->kind == 3u) {
            /* a round tank: a cylinder of 12 (8 when small) sides, the photo's disc on top, walls of its own texel */
            int N = h->hl >= 3.0f ? 12 : 8;
            float rx[12], rz[12], ru[12], rv[12], tgy = -1e30f, tgmin = 1e30f;
            int bad = 0;
            for (int q = 0; q < N && !bad; q++) {
                float a = 6.2831853f * (float)q / (float)N, tu = h->cu + h->hl * cosf(a), tv = h->cv + h->hl * sinf(a);
                ZP_TEX2W(tu, tv, rx[q], rz[q]);
                ru[q] = umin + tu * du; rv[q] = vmin + tv * dv;
                int ok;
                float g = zp_ground(P, idx, nidx, rx[q], rz[q], &ok);
                if (!ok) bad = 1;
                else { if (g > tgy) tgy = g; if (g < tgmin) tgmin = g; }
            }
            if (bad) continue;
            { int ok; float g = zp_ground(P, idx, nidx, cxw, czw, &ok); if (ok && g > tgy) tgy = g; }
            float hm = 0.25f * 2.0f * h->hl * 6.25f;
            hm = (hm < 6.0f ? 6.0f : hm > 16.0f ? 16.0f : hm) * zp_hscale;
            float ttop = tgy + hm * 5.12f * grow, tbot = tgmin - 8.0f;
            u32 c0 = nv;
            ZP_V(cxw, ttop, czw, umin + h->cu * du, vmin + h->cv * dv, 1.0f);
            for (int q = 0; q < N; q++) ZP_V(rx[q], ttop, rz[q], ru[q], rv[q], 1.0f);
            for (int q = 0; q < N; q++) { ib[ni++] = c0; ib[ni++] = c0 + 1u + (u32)q; ib[ni++] = c0 + 1u + (u32)((q + 1) % N); }
            for (int q = 0; q < N; q++) {
                int q2 = (q + 1) % N;
                float ox = 0.5f * (rx[q] + rx[q2]) - cxw, oz = 0.5f * (rz[q] + rz[q2]) - czw, ol = sqrtf(ox * ox + oz * oz);
                float kk = zp_wall_k(ol > 1e-3f ? (ox * sx + oz * sz) / ol : 0.0f);
                u32 s0 = nv;
                ZP_V(rx[q], tbot, rz[q], hwu, hwv, kk * ZP_WALL_FOOT); ZP_V(rx[q2], tbot, rz[q2], hwu, hwv, kk * ZP_WALL_FOOT);
                ZP_V(rx[q2], ttop, rz[q2], hwu, hwv, kk); ZP_V(rx[q], ttop, rz[q], hwu, hwv, kk);
                const u32 tri[6] = { 0, 1, 2, 0, 2, 3 };
                for (int t6 = 0; t6 < 6; t6++) ib[ni++] = s0 + tri[t6];
            }
            nh++;
            zp_st_kind[2]++;
            continue;
        }
        /* eave corners in texel space, counter-clockwise seen from above in texels */
        float tu[4], tv[4], wx[4], wz[4], gy = -1e30f, gmin = 1e30f;
        const float su[4] = { -1, 1, 1, -1 }, sv[4] = { -1, -1, 1, 1 };
        for (int k = 0; k < 4; k++) {
            tu[k] = h->cu + su[k] * h->hl * h->au - sv[k] * h->hw * h->av;
            tv[k] = h->cv + su[k] * h->hl * h->av + sv[k] * h->hw * h->au;
            ZP_TEX2W(tu[k], tv[k], wx[k], wz[k]);
            int ok;
            float g = zp_ground(P, idx, nidx, wx[k], wz[k], &ok);
            if (!ok) { skip = 1; break; }
            if (g > gy) gy = g;
            if (g < gmin) gmin = g;
        }
        if (skip) continue;
        { int ok; float g = zp_ground(P, idx, nidx, cxw, czw, &ok); if (ok && g > gy) gy = g; }
        float eave = h->eave;
        if (h->kind == 2u) {                              /* a building: its height from its shadow and the sun */
            float hm = h->slen * 6.25f * tanel, side_m = 2.0f * (h->hl < h->hw ? h->hl : h->hw) * 6.25f;
            if (hm > 0.8f * side_m) hm = 0.8f * side_m;   /* no slab taller than 0.8 x its narrow side (walls, racks) */
            eave = (hm < 4.0f ? 4.0f : hm > 20.0f ? 20.0f : hm) * zp_hscale * 5.12f;
        }
        float top = gy + eave * grow, bot = gmin - 8.0f, ridge = h->ridge * grow;
        /* roof: flat (4) or a gable along the length (ridge over the middle of the short sides) */
        u32 base = nv;
        float ruv[4][2];
        for (int k = 0; k < 4; k++) { ruv[k][0] = umin + tu[k] * du; ruv[k][1] = vmin + tv[k] * dv; }
        /* world normal of a wall between corner k and k+1 (outward), for the sun shading */
        float rgb_roof = 1.0f;
        if (h->ridge > 0.0f) {
            float rt = top + ridge;
            float mu[2], mv[2], mx[2], mz[2];
            /* ridge ends: middles of the short sides (k 3-0 and 1-2) */
            mu[0] = 0.5f * (tu[3] + tu[0]); mv[0] = 0.5f * (tv[3] + tv[0]);
            mu[1] = 0.5f * (tu[1] + tu[2]); mv[1] = 0.5f * (tv[1] + tv[2]);
            for (int e = 0; e < 2; e++) ZP_TEX2W(mu[e], mv[e], mx[e], mz[e]);
            /* slope 1: corners 0, 1 + ridge; slope 2: corners 2, 3 + ridge; shading by the slope's facing */
            for (int sl = 0; sl < 2; sl++) {
                int ka = sl ? 2 : 0, kb = sl ? 3 : 1;
                float ox = 0.5f * (wx[ka] + wx[kb]) - 0.5f * (mx[0] + mx[1]), oz = 0.5f * (wz[ka] + wz[kb]) - 0.5f * (mz[0] + mz[1]);
                float ol = sqrtf(ox * ox + oz * oz);
                float f = ol > 1e-3f ? (ox * sx + oz * sz) / ol : 0.0f;
                float k = 1.0f + 0.12f * f;
                const float vx[4] = { wx[ka], wx[kb], sl ? mx[0] : mx[1], sl ? mx[1] : mx[0] };
                const float vz[4] = { wz[ka], wz[kb], sl ? mz[0] : mz[1], sl ? mz[1] : mz[0] };
                const float vy[4] = { top, top, rt, rt };
                const float uu[4] = { ruv[ka][0], ruv[kb][0], sl ? umin + mu[0] * du : umin + mu[1] * du, sl ? umin + mu[1] * du : umin + mu[0] * du };
                const float vv[4] = { ruv[ka][1], ruv[kb][1], sl ? vmin + mv[0] * dv : vmin + mv[1] * dv, sl ? vmin + mv[1] * dv : vmin + mv[0] * dv };
                u32 s0 = nv;
                for (int q = 0; q < 4; q++) {
                    zp_rec *r = &vb[nv++];
                    memset(r, 0, sizeof *r);
                    r->p[0] = vx[q]; r->p[1] = vy[q]; r->p[2] = vz[q]; r->p[3] = 1.0f;
                    for (int c = 0; c < 3; c++) { int v = (int)((float)col[c] * k + 0.5f); r->c[c] = v > 255 ? 255 : v; }
                    r->c[3] = col[3];
                    r->uv[0] = uu[q]; r->uv[1] = vv[q]; r->uv[2] = UV[0][2]; r->uv[3] = UV[0][3];
                }
                const u32 tri[6] = { 0, 1, 2, 0, 2, 3 };
                for (int q = 0; q < 6; q++) ib[ni++] = s0 + tri[q];
            }
            /* gable triangles over the short walls */
            for (int e = 0; e < 2; e++) {
                int ka = e ? 1 : 3, kb = e ? 2 : 0;
                u32 s0 = nv;
                const float vx[3] = { wx[ka], wx[kb], mx[e] }, vz[3] = { wz[ka], wz[kb], mz[e] }, vy[3] = { top, top, top + ridge };
                float ox = mx[e] - 0.5f * (mx[0] + mx[1]), oz = mz[e] - 0.5f * (mz[0] + mz[1]), ol = sqrtf(ox * ox + oz * oz);
                float f = ol > 1e-3f ? (ox * sx + oz * sz) / ol : 0.0f, k = zp_wall_k(f);
                for (int q = 0; q < 3; q++) {
                    zp_rec *r = &vb[nv++];
                    memset(r, 0, sizeof *r);
                    r->p[0] = vx[q]; r->p[1] = vy[q]; r->p[2] = vz[q]; r->p[3] = 1.0f;
                    for (int c = 0; c < 3; c++) { int v = (int)((float)col[c] * k + 0.5f); r->c[c] = v > 255 ? 255 : v; }
                    r->c[3] = col[3];
                    r->uv[0] = wu; r->uv[1] = wv; r->uv[2] = UV[0][2]; r->uv[3] = UV[0][3];
                }
                ib[ni++] = s0; ib[ni++] = s0 + 1; ib[ni++] = s0 + 2;
            }
        } else {
            u32 s0 = nv;
            for (int q = 0; q < 4; q++) {
                zp_rec *r = &vb[nv++];
                memset(r, 0, sizeof *r);
                r->p[0] = wx[q]; r->p[1] = top; r->p[2] = wz[q]; r->p[3] = 1.0f;
                for (int c = 0; c < 3; c++) { int v = (int)((float)col[c] * rgb_roof + 0.5f); r->c[c] = v > 255 ? 255 : v; }
                r->c[3] = col[3];
                r->uv[0] = ruv[q][0]; r->uv[1] = ruv[q][1]; r->uv[2] = UV[0][2]; r->uv[3] = UV[0][3];
            }
            const u32 tri[6] = { 0, 1, 2, 0, 2, 3 };
            for (int q = 0; q < 6; q++) ib[ni++] = s0 + tri[q];
        }
        /* walls: corner k to k+1, the wall texel shaded by the facing */
        float cxm = 0.25f * (wx[0] + wx[1] + wx[2] + wx[3]), czm = 0.25f * (wz[0] + wz[1] + wz[2] + wz[3]);
        for (int k = 0; k < 4; k++) {
            int k2 = (k + 1) & 3;
            float ex = wz[k2] - wz[k], ez = -(wx[k2] - wx[k]);      /* a normal of the wall; outward from the centre */
            float mxw = 0.5f * (wx[k] + wx[k2]) - cxm, mzw = 0.5f * (wz[k] + wz[k2]) - czm;
            if (ex * mxw + ez * mzw < 0.0f) { ex = -ex; ez = -ez; }
            float el = sqrtf(ex * ex + ez * ez);
            float f = el > 1e-3f ? (ex * sx + ez * sz) / el : 0.0f;
            float kk = zp_wall_k(f);
            u32 s0 = nv;
            const float vx[4] = { wx[k], wx[k2], wx[k2], wx[k] }, vz[4] = { wz[k], wz[k2], wz[k2], wz[k] };
            const float vy[4] = { bot, bot, top, top };
            for (int q = 0; q < 4; q++) {
                zp_rec *r = &vb[nv++];
                memset(r, 0, sizeof *r);
                r->p[0] = vx[q]; r->p[1] = vy[q]; r->p[2] = vz[q]; r->p[3] = 1.0f;
                float kq = q < 2 && h->kind >= 2u ? kk * ZP_WALL_FOOT : kk;   /* buildings: the wall's foot a little darker */
                for (int c = 0; c < 3; c++) { int v = (int)((float)col[c] * kq + 0.5f); r->c[c] = v > 255 ? 255 : v; }
                r->c[3] = col[3];
                r->uv[0] = hwu; r->uv[1] = hwv; r->uv[2] = UV[0][2]; r->uv[3] = UV[0][3];
            }
            const u32 tri[6] = { 0, 1, 2, 0, 2, 3 };
            for (int q = 0; q < 6; q++) ib[ni++] = s0 + tri[q];
        }
        (void)base;
        nh++;
        zp_st_kind[h->kind == 2u ? 1 : 0]++;
    }
#undef ZP_V
#undef ZP_TEX2W
    if (!nv) return;
    if (zp_frame_verts + nv > 32768u || !ps2_vk_mesh_room(nv, ni, 1u)) { zp_st_room++; return; }
    u32 vbase = 0, ifirst = 0;
    if (ps2_vk_mesh_store((const u8 *)vb, nv, ib, ni, &vbase, &ifirst) != 0) { zp_st_room++; return; }
    zp_frame_verts += nv;
    ps2_vk_mesh_block hb = *b;
    hb.pass = 0u;
    hb.clip = 1u;
    hb.pad4 |= PS2_PROPS_PAD4;
    ps2_vk_draw_mesh(st, &hb, vbase, ifirst, ni, 0);
    zp_st_draws++;
    zp_st_houses += nh;
    zp_fs[4]++; zp_fs[5] += nh;
}

int zero_props_on(void) { return zp_on; }
int zero_props_ab_n(void) { return zp_ab_n; }
int zero_props_city_look(void) { return zp_on && (zp_kinds & ZP_K_CITY) && zp_city_look; }
int zero_props_city_look_ab_n(void) { return zp_city_look_ab; }

void zero_props_report(void) {
    if (zp_st_city_draws || zp_st_city_gen)
        ps2_log("props: city: %llu cell draws (%.1f buildings, %.0f vertices per draw; store %.1f us, draw %.1f us per draw "
                "with ACZ_DEBUG_PROPS), cells made %llu (%.1f us each), reused %llu, left out over the budget %llu",
                (unsigned long long)zp_st_city_draws,
                zp_st_city_draws ? (double)zp_st_city_bld / (double)zp_st_city_draws : 0.0,
                zp_st_city_draws ? (double)zp_st_city_verts / (double)zp_st_city_draws : 0.0,
                zp_st_city_draws ? (double)zp_city_store_ns / 1000.0 / (double)zp_st_city_draws : 0.0,
                zp_st_city_draws ? (double)zp_city_draw_ns / 1000.0 / (double)zp_st_city_draws : 0.0, (unsigned long long)zp_st_city_gen,
                zp_st_city_gen ? (double)zp_city_gen_ns / 1000.0 / (double)zp_st_city_gen : 0.0, (unsigned long long)zp_st_city_hit,
                (unsigned long long)zp_st_city_budget);
    if (zp_st_draws || zp_st_room)
        ps2_log("props: %llu house draws (%llu houses, %.1f per drawn GS frame: red roofs %llu, buildings %llu, tanks %llu), "
                "%llu left out for mesh arena space, %llu near ground units (%u units seen)",
                (unsigned long long)zp_st_draws, (unsigned long long)zp_st_houses,
                zp_st_frames ? (double)zp_st_houses / (double)zp_st_frames : 0.0, (unsigned long long)zp_st_kind[0],
                (unsigned long long)zp_st_kind[1], (unsigned long long)zp_st_kind[2], (unsigned long long)zp_st_room,
                (unsigned long long)zp_st_unit_skip, zp_nu);
}

/* EE thread (the HUD update hook), every 30 fields: the ground and sea units of the mission (the team table at
   player+0x1A5C as the game's target system reads it: groups at *table, count table+0x1C; units of 0x4A0 bytes at
   group+0x104, count +0x108; position +0x260; +0x39B 1 = an aircraft, left out) -- every place a unit has been seen
   this map keeps a clear area (ACZ_TERRAIN_PROPS_UNIT_CLEAR) where nothing is raised, so a target never stands in or
   under a raised house; the cells such an area reaches are marked for the renderer's quick test */
static void zp_units_scan(u32 player) {
    if (!zp_on || !zp_sig) return;
    if (zp_u_sig != zp_sig) {                               /* a new map: forget the old places */
        __atomic_store_n(&zp_nu, 0u, __ATOMIC_RELEASE);
        memset(zp_ucell, 0, sizeof zp_ucell);
        zp_u_sig = zp_sig; zp_u_next = 0;
    }
    if (zp_pp_field < zp_u_next) return;
    zp_u_next = zp_pp_field + 30u;
    u32 t = ps2_r32(player + 0x1A5Cu);
    if (!zp_ptr(t)) return;
    u32 G = ps2_r32(t), ng = ps2_r8(t + 0x1Cu), n = zp_nu, n0 = n, moved = 0;
    if (!zp_ptr(G)) return;
    for (u32 g = 0; g < ng && g < 64u; g++) {
        u32 grp = ps2_r32(G + 4u * g);
        if (!zp_ptr(grp)) continue;
        u32 u0 = ps2_r32(grp + 0x104u), cnt = ps2_r32(grp + 0x108u);
        for (u32 i = 0; zp_ptr(u0) && i < cnt && i < 256u; i++) {
            u32 u = u0 + i * 0x4A0u;
            if (ps2_r8(u + 0x39Bu) == 1u) continue;           /* aircraft */
            float x = zp_f(u + 0x260u), y = zp_f(u + 0x264u), z = zp_f(u + 0x268u);
            if (!(x > -262144.0f && x < 262144.0f && z > -262144.0f && z < 262144.0f) || (x == 0.0f && z == 0.0f) || y < -10000.0f)
                continue;                                     /* not placed yet / hidden */
            /* two places per unit: where it was first seen (kept) and where it is now (moved along: no trail) */
            u32 id = (g << 8) | i, first = ZP_MAXU, cur = ZP_MAXU;
            for (u32 k = 0; k < n; k++) {
                if (zp_uid[k] == id) first = k;
                else if (zp_uid[k] == (id | 0x80000000u)) cur = k;
            }
            u32 k;
            if (first == ZP_MAXU) k = n < ZP_MAXU ? n++ : ZP_MAXU, first = k;
            else if (fabsf(zp_ux[first][0] - x) < 100.0f && fabsf(zp_ux[first][1] - z) < 100.0f) continue;
            else if (cur == ZP_MAXU) k = n < ZP_MAXU ? n++ : ZP_MAXU, cur = k;
            else k = cur;
            if (k >= ZP_MAXU) continue;
            int was = k < n0 && k == cur;                     /* a moving unit's current place: its old cells too */
            float ox = zp_ux[k][0], oz = zp_ux[k][1];
            zp_uid[k] = k == first ? id : (id | 0x80000000u);
            zp_ux[k][0] = x; zp_ux[k][1] = z;
            moved += k == cur;
            float r = zp_unit_clear + 1200.0f;               /* + the largest find's half size, with a margin */
            for (int pass = was ? 0 : 1; pass < 2; pass++) {
                float px = pass ? x : ox, pz = pass ? z : oz;
                int c0 = (int)floorf((px - r + 262144.0f) / 2048.0f), c1 = (int)floorf((px + r + 262144.0f) / 2048.0f);
                int d0 = (int)floorf((pz - r + 262144.0f) / 2048.0f), d1 = (int)floorf((pz + r + 262144.0f) / 2048.0f);
                for (int cz = d0; cz <= d1; cz++)
                    for (int cx = c0; cx <= c1; cx++) {
                        u8 *v = &zp_ucell[(u32)(cz & 255) * 256u + (u32)(cx & 255)];
                        *v = (u8)(*v + 1u) ? (u8)(*v + 1u) : 1u;   /* (the city's cache key: these cells are made again) */
                    }
            }
        }
    }
    if (n != n0) {
        __atomic_store_n(&zp_nu, n, __ATOMIC_RELEASE);
        if (zp_dbg) ps2_log("props: field %llu: %u ground unit places (+%u)", (unsigned long long)zp_pp_field, n, n - n0);
    }
    if (zp_dbg >= 2 && moved) ps2_log("props: field %llu: %u moving ground units", (unsigned long long)zp_pp_field, moved);
}

static int zp_mgr_enter(ps2_ctx *ctx, void *user) {
    (void)user;
    static u64 last_f;
    u64 f = ps2_kernel_vblank_count();
    if (f == last_f) return 0;
    last_f = f;
    u32 mgr = ctx->r[4].uw[0];
    if (!zp_ptr(mgr)) return 0;
    u32 T = ps2_r32(mgr + 4u), O = ps2_r32(mgr + 8u);
    if (!zp_ptr(T) || !zp_ptr(O)) return 0;
    u32 sig = zp_signature(O);
    if (sig != zp_sig || O != zp_O) { zp_sig = sig; zp_O = O; zp_T = T; zp_stable = 0; return 0; }
    zp_stable++;
    if (zp_dbg && zp_stable == 60u && zp_dumped_sig != sig) { zp_dumped_sig = sig; zp_dump(ctx); }
    if (zp_on && zp_stable >= 30u && zp_built_sig != sig) {
        int wait = 0;                                     /* the desert finder wants the mission's sun (C1): up to 10 s */
#ifdef ZP_C1
        if ((zp_kinds & ZP_K_DESERT) && zp_shadow_cfg < 0 && zp_stable < 630u) {
            static ps2_cloud_env e;
            wait = !(ps2_cloud_env_read(&e) && (e.valid & CL_ENV_SUN));
        }
#endif
        if (!wait) zp_build(ctx);
    }
    return 0;
}

static float env_f(const char *n, float def, float lo, float hi) {
    const char *e = getenv(n);
    if (!e || !*e) return def;
    float v = (float)atof(e);
    return v < lo ? lo : v > hi ? hi : v;
}

static void zp_read_kinds(void) {
    const char *e = ps2_menv("ACZ_TERRAIN_PROPS");
    zp_on = e && *e && *e != '0';
    zp_kinds = 0;
    if (zp_on) {                                          /* 1 = red roofs; 2 = also buildings and tanks; or a list */
        if (*e >= '0' && *e <= '9') zp_kinds = atoi(e) >= 2 ? ZP_K_ROOFS | ZP_K_DESERT : ZP_K_ROOFS;
        else {
            if (strstr(e, "roof")) zp_kinds |= ZP_K_ROOFS;
            if (strstr(e, "block") || strstr(e, "build")) zp_kinds |= ZP_K_BLOCKS;
            if (strstr(e, "tank")) zp_kinds |= ZP_K_TANKS;
            if (strstr(e, "all")) zp_kinds |= ZP_K_ROOFS | ZP_K_DESERT;
            if (!zp_kinds) zp_kinds = ZP_K_ROOFS;
        }
    }
    e = ps2_menv("ACZ_TERRAIN_CITY");                     /* stage 3 (with any ACZ_TERRAIN_PROPS): on unless 0 */
    zp_city = e && *e ? atoi(e) : 1;
    if (zp_on && zp_city > 0) zp_kinds |= ZP_K_CITY;
}
static int zp_hooks_in;
/* the NEXT MISSION tier (ps2_menv.h): the game thread, at the edge. What derives: the built houses: asked again for this map
   (zp_mgr_enter builds in the background while the mission loads; the map's cells show none until then). */
void zero_props_menv_reload(void) {
    if (!zp_hooks_in) return;
    const int a = zp_on, b = zp_kinds;
    zp_read_kinds();
    if (zp_kinds != b || zp_on != a) { zp_built_sig = 0; zp_stable = 0; }
    ps2_menv_note("ACZ_TERRAIN_PROPS (on)", a, zp_on);
    ps2_menv_note("ACZ_TERRAIN_PROPS / ACZ_TERRAIN_CITY (kinds)", b, zp_kinds);
}
void zero_props_init(void) {
    const char *e = getenv("ACZ_DEBUG_PROPS");
    zp_dbg = e ? atoi(e) : 0;
    zp_read_kinds();
    zp_city_dist = env_f("ACZ_TERRAIN_CITY_DIST", 2500.0f, 800.0f, 6000.0f) * 5.12f;
    zp_city_reach = zp_city_dist;
    zp_city_hs = env_f("ACZ_TERRAIN_CITY_HEIGHT", 1.0f, 0.3f, 2.0f);
    zp_city_budget = (u32)env_f("ACZ_TERRAIN_CITY_BUDGET", 65536.0f, 8192.0f, 100000.0f);
    e = getenv("ACZ_TERRAIN_CITY_WINDOWS");
    zp_city_win = !(e && *e == '0');
    e = getenv("ACZ_TERRAIN_CITY_LOOK");
    zp_city_look = !(e && *e == '0');
    zp_city_look_ab = (int)env_f("ACZ_TERRAIN_CITY_LOOK_AB", 0.0f, 0.0f, 100000.0f);
    e = getenv("ACZ_TERRAIN_PROPS_SHADOW");
    if (e && *e) {
        static const char *const nm[5] = { "up", "down", "left", "right", "vote" };
        for (int k = 0; k < 5; k++) if (!strcmp(e, nm[k])) zp_shadow_cfg = k;
    }
    zp_unit_clear = env_f("ACZ_TERRAIN_PROPS_UNIT_CLEAR", 80.0f, 0.0f, 1000.0f) * 5.12f;
    zp_dist = env_f("ACZ_TERRAIN_PROPS_DIST", 3500.0f, 300.0f, 6000.0f) * 5.12f;
    zp_hscale = env_f("ACZ_TERRAIN_PROPS_HEIGHT", 1.0f, 0.3f, 1.5f);
    zp_clear = env_f("ACZ_TERRAIN_PROPS_CLEAR", 96.0f, 0.0f, 2048.0f);
    zp_ab_n = (int)env_f("ACZ_TERRAIN_PROPS_AB", 0.0f, 0.0f, 100000.0f);
    e = getenv("ACZ_DEBUG_PROPS_TP");
    for (const char *s = e; s && *s && zp_tpn < 32; ) {
        float *q = zp_tpq[zp_tpn];
        if (sscanf(s, "%f,%f,%f,%f,%f", &q[0], &q[1], &q[2], &q[3], &q[4]) != 5) break;
        zp_tpn++;
        s = strchr(s, ';');
        if (s) s++;
    }
    zp_tp_on = zp_tpn > 0;
    if (zp_tp_on && !zp_dbg) zp_dbg = 1;
    if (!zp_dbg && !zp_on) return;
    u32 w = ps2_r32(ZP_MGR);
    if ((w >> 16) != 0x27BDu || ps2_hook_before(ZP_MGR, zp_mgr_enter, NULL, 57, "zero-props") < 0
        || (zp_dbg && ps2_hook_before(0x0015F4D0u, zp_marks, NULL, 57, "zero-props") < 0)
        || ps2_hook_before(ZP_HUD, zp_hud, NULL, 61, "zero-props") < 0) {
        ps2_log("props: hooks failed (code %08X)", w);
        zp_on = 0;
        return;
    }
    zp_hooks_in = 1;
    ps2_log("props: houses from the ground photo %s (ACZ_TERRAIN_PROPS: %s%s%s%s), within %.0f m, height x %.2f, nothing within "
            "%.0f m of a ground unit%s%s", zp_on ? "on" : "off", zp_kinds & ZP_K_ROOFS ? "red roofs " : "",
            zp_kinds & ZP_K_BLOCKS ? "buildings " : "", zp_kinds & ZP_K_TANKS ? "tanks " : "",
            zp_kinds & ZP_K_CITY ? "city " : "", zp_dist / 5.12f, zp_hscale,
            zp_unit_clear / 5.12f, zp_ab_n ? ", in-run GPU A/B (ACZ_TERRAIN_PROPS_AB)" : "",
            zp_dbg ? ", map object probe (ACZ_DEBUG_PROPS)" : "");
    if (zp_kinds & ZP_K_CITY)
        ps2_log("props: city buildings (ACZ_TERRAIN_CITY=%d): within %.0f m (growing over the outer 30 %%), height x %.2f, "
                "%u vertices a frame at most, windows %s, detailed look %s%s", zp_city, zp_city_dist / 5.12f, zp_city_hs,
                zp_city_budget, zp_city_win ? "on" : "off", zp_city_look ? "on (ACZ_TERRAIN_CITY_LOOK)" : "off",
                zp_city_look_ab ? ", in-run GPU A/B (ACZ_TERRAIN_CITY_LOOK_AB)" : "");
}
