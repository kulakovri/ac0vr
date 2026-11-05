#include "rn_vp_int.h"
#include "ps2_menv.h"
#include "ps2_props.h"
#include "ps2_water.h"
#include "ps2_texpack.h"
#include "ps2_terrain.h"

#include <stdio.h>
#include <stdlib.h>

static u64 st_patches, st_outside, st_declined, st_room, st_full;

static float height(vpx *x, u32 a) {
    return vp_lane_itof(vp_rd_lane(x, a, 1), 0) * 80.0f;
}

static float clip_w(vpx *x, const float *p) {
    float acc = vp_F(x, 9)[3] * p[0];
    acc = acc + vp_F(x, 10)[3] * p[1];
    acc = acc + vp_F(x, 11)[3] * p[2];
    return acc + vp_F(x, 12)[3] * 1.0f;
}

/* Terrain detail band in VR (ACZ_VR_TERRAIN_MORPH: 1 = distance from the eye, 0 = the game's view depth; default 1
   with PS2_XR=1, else 0). The geomorph of the fine patches (24576..32768 x s/512 = 27420..36560 in missions 2 and 3)
   and the fade of the two detail textures (passes 1/2, mesh.vert: 0..9140) are driven by the clip-space w of the
   camera the frame is drawn with -- the view depth of the head-turned camera in VR, so the band slid over the ground
   when the head turned (a vertex 30000 units away 40 deg off the view axis: w 30000 -> 23000 for a 40 deg turn, morph
   0.28 -> 0). With 1 they use the distance from the eye instead, the same for every head direction: in the view
   direction it equals the game's w (the band sits where the game puts it), off-axis it is larger (the band comes
   nearer at the sides; fully coarse where w alone would be, so the switch of cells from fine to coarse at the game's
   cell distance stays seamless). The eye is the point where clip x = y = w = 0 (VU 0x14-0x17). ACZ_DEBUG_MORPH=f
   logs the morph of every fine vertex in that GS frame (A/B of head poses); ACZ_DEBUG_MORPH_TINT=1 colours the
   morphing vertices (red = coarse end). */
static int tm_mode = -1, tm_tint;
static u32 tm_dbg_frame;
static float tm_eye[3];
static int tm_eye_ok;

static void tm_init(void) {
    const char *e = getenv("ACZ_VR_TERRAIN_MORPH");
    if (e && *e) tm_mode = *e != '0';
    else { const char *xr = getenv("PS2_XR"); tm_mode = xr && *xr == '1'; }
    e = getenv("ACZ_DEBUG_MORPH");
    tm_dbg_frame = e ? (u32)strtoul(e, NULL, 0) : 0u;
    e = getenv("ACZ_DEBUG_MORPH_TINT");
    tm_tint = e && *e == '1';
    ps2_log("rn: terrain detail band by %s (ACZ_VR_TERRAIN_MORPH=%d)", tm_mode ? "the distance from the eye"
            : "the view depth (the game's)", tm_mode);
}

static void tm_eye_from_clip(vpx *x) {
    const float *a = vp_F(x, 9), *b = vp_F(x, 10), *c = vp_F(x, 11), *d = vp_F(x, 12);
    double m[3][3] = { { a[0], b[0], c[0] }, { a[1], b[1], c[1] }, { a[3], b[3], c[3] } };
    double t[3] = { -d[0], -d[1], -d[3] }, det, e[3];
    det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
        + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    tm_eye_ok = 0;
    if (!(fabs(det) > 1e-12)) return;
    for (int k = 0; k < 3; k++) {
        double n[3][3];
        memcpy(n, m, sizeof n);
        for (int r = 0; r < 3; r++) n[r][k] = t[r];
        e[k] = (n[0][0] * (n[1][1] * n[2][2] - n[1][2] * n[2][1]) - n[0][1] * (n[1][0] * n[2][2] - n[1][2] * n[2][0])
              + n[0][2] * (n[1][0] * n[2][1] - n[1][1] * n[2][0])) / det;
        if (!(fabs(e[k]) < 1e7)) return;
        tm_eye[k] = (float)e[k];
    }
    tm_eye_ok = 1;
}

static void build_patch(vpx *x, u32 t, u16 count) {
    u32 v = t + 3u;
    float org[4], uvoff[4];
    vp_rd(x, t + 1u, org);
    vp_rd(x, t + 2u, uvoff);
    if (count == 10u) {
        float pos[9][4], uv[9][4];
        for (int k = 0; k < 9; k++) {
            float tmpl[4];
            vp_rd(x, 0x3ECu + k, tmpl);
            pos[k][0] = tmpl[0] + org[0];
            pos[k][1] = height(x, v + 4u * k);
            pos[k][2] = tmpl[2] + org[2];
            pos[k][3] = tmpl[3];
            vp_rd(x, 0x3F6u + k, uv[k]);
            uv[k][2] = uvoff[2] + 0.0f;
            uv[k][3] = uvoff[3] + 0.0f;
        }
        for (int k = 0; k < 9; k++) {
            vp_sq(x, v + 4u * k + 3u, uv[k], VP_MALL);
            if (k & 1) vp_sq(x, v + 4u * k, pos[k], VP_MALL);
        }
        vp_sq(x, v + 36u, pos[1], VP_MALL);
        vp_sq(x, v + 39u, uv[1], VP_MALL);
        for (int k = 0; k < 9; k += 2) {
            pos[k][3] = clip_w(x, pos[k]);
            vp_sq(x, v + 4u * k, pos[k], VP_MALL);
        }
        {
            float g[4], range;
            static const u8 nb[5][2] = { {1, 5}, {3, 1}, {5, 3}, {7, 5}, {9, 7} };
            vp_rd(x, 0x21u, g);
            range = g[1] - g[0];
            for (int e = 0; e < 5; e++) {
                u32 a = v + 4u * (u32)(e * 2);
                float p[4], d, avg, h, q, mw;
                vp_rd(x, a, p);
                mw = p[3];                                /* the band's measure: view depth, or the distance */
                if (tm_mode > 0 && tm_eye_ok) {
                    float dx = p[0] - tm_eye[0], dy = p[1] - tm_eye[1], dz = p[2] - tm_eye[2];
                    mw = sqrtf(dx * dx + dy * dy + dz * dz);
                }
                if (tm_dbg_frame && ps2_gs_frame_count == tm_dbg_frame) {
                    float dx = p[0] - tm_eye[0], dy = p[1] - tm_eye[1], dz = p[2] - tm_eye[2];
                    float r = sqrtf(dx * dx + dy * dy + dz * dz), qw = (p[3] - g[0]) / range, qr = (r - g[0]) / range;
                    if (qw > -0.25f || qr > -0.25f)
                        ps2_log("morph: frame %u vertex %.0f %.0f %.0f w %.0f r %.0f q_w %.3f q_r %.3f eye %.0f %.0f %.0f",
                                ps2_gs_frame_count, p[0], p[1], p[2], p[3], r, qw < 0 ? 0.0f : qw > 1 ? 1.0f : qw,
                                qr < 0 ? 0.0f : qr > 1 ? 1.0f : qr, tm_eye[0], tm_eye[1], tm_eye[2]);
                }
                d = mw - g[0];
                if (signbit(d)) continue;
                h = p[1];
                avg = vp_rd_lane(x, v + 4u * nb[e][1], 1) + vp_rd_lane(x, v + 4u * nb[e][0], 1);
                avg = avg * 0.5f;
                q = d / range;
                if (tm_tint) {                            /* debug: red = coarse end of the band, yellow = start */
                    u32 col[4] = { 255u, (u32)(255.0f * (1.0f - (q > 1.0f ? 1.0f : q))), 0u, 128u };
                    vp_sq(x, a + 2u, (const float *)col, VP_MALL);
                }
                if (!signbit(mw - g[1])) {
                    p[1] = avg;
                } else {
                    float s = avg - h;
                    s = s * q;
                    p[1] = h + s;
                }
                vp_sq(x, a, p, VP_MY);
            }
        }
    } else {
        for (int k = 0; k < 4; k++) {
            float pos[4], uv[4], tmpl[4];
            pos[1] = height(x, v + 4u * k);
            vp_rd(x, 0x3F7u + 2u * k, uv);
            uv[2] = uvoff[2] + 0.0f;
            uv[3] = uvoff[3] + 0.0f;
            vp_rd(x, 0x3EDu + 2u * k, tmpl);
            pos[0] = tmpl[0] + org[0];
            pos[2] = tmpl[2] + org[2];
            pos[3] = tmpl[3];
            vp_sq(x, v + 4u * k + 3u, uv, VP_MALL);
            vp_sq(x, v + 4u * k, pos, VP_MALL);
        }
    }
}

static u16 clip_code(vpx *x, const float *p) {
    float c[4];
    vp_rows(c, vp_F(x, 9), vp_F(x, 10), vp_F(x, 11), vp_F(x, 12), p[0], p[1], p[2], 1.0f,
            VP_MALL);
    return vp_clip_code(c);
}

/* Detail textures in VR (bug #54, ACZ_TERRAIN_DETAIL_HOLD=h, default 0 = the game's fade). Passes 1 and 2 subtract
   the game's two 128 x 128 detail textures (16 repeats per 2048-unit cell: 1.3 m per texel, against the atlas's 5.3 m)
   from the atlas colour, faded linearly from the eye to 0 at the game's detail distance D (VU 0x21.z / 0x22.z: 9140
   units, 1.5 km, in missions 2-4); at 30 m over flat ground (mission 4) the ground half a kilometre ahead already has
   only 2/3 of its pattern. The headset resolves about twice the PS2's pixels per degree, so with h > 0 the pattern
   keeps its full strength over the first share h of D and fades over the rest (still to 0 at D: the game draws the
   passes only on its near patches, so the far end cannot move). The darkening of the pattern comes with it: the held
   ground reads a few levels darker in the middle distance (mission 4: -2.5..-4 of 255). mesh.vert detail_fade() reads h
   from block pad4 bits 16-23. ACZ_TERRAIN_DETAIL_AB=N (test): on / off in blocks of N GS frames, logged. Debug:
   ACZ_DEBUG_TERRAIN_TEX_TINT=1 tints the terrain's first pass by the game's fixed mip level (0 red, 1 green) and the
   patches carrying detail passes (blue), pad4 bits 8-10. */
static u32 dh_hold_q = 0xFFFFFFFFu, dh_pad4;
static int dh_ab, dh_tint;
static void dh_init(void) {
    const char *e = ps2_menv("ACZ_TERRAIN_DETAIL_HOLD");
    float h = e ? (float)atof(e) : 0.0f;
    if (!(h >= 0.0f && h <= 0.9f)) h = 0.0f;
    dh_hold_q = (u32)(h * 255.0f + 0.5f);
    e = getenv("ACZ_TERRAIN_DETAIL_AB");
    dh_ab = e ? atoi(e) : 0;
    e = getenv("ACZ_DEBUG_TERRAIN_TEX_TINT");
    dh_tint = e && *e == '1';
    if (dh_hold_q || dh_ab || dh_tint)
        ps2_log("rn: terrain detail textures held at full strength over %.0f %% of their distance (ACZ_TERRAIN_DETAIL_HOLD)%s%s",
                dh_hold_q / 2.55f, dh_ab > 0 ? ", in-run A/B (ACZ_TERRAIN_DETAIL_AB)" : "",
                dh_tint ? ", debug tint by mip level" : "");
}

/* ACZ_DEBUG_TERRAIN_TEX=N (bug #54, diagnostics): every N GS frames one log block of the terrain passes' textures --
   per pass the draws by the game's fixed mip level (TEX1 LCM/K: st.tex_lod) and by the patch's distance from the eye
   (bins 1024 x 2^k units), how many were fine patches and native region textures, the TEX0 sizes, the clamp region,
   the raw A+D qwords of each pass as the game sent them, the blend (GS (A-B)*C+D), the wrap modes and whether the
   mesh material path takes the pass (matok: else gs.frag draws it); and the texel size of the near fine patches.
   Mission 4 at 30 m: pass 0 (atlas) level 0 within ~16-32k units, the game's level 1 beyond, 64 texels per 2048-unit
   cell (5.3 m, as missions 2 and 3); passes 1 / 2 (detail) subtractive, gs.frag; pass 3 subtractive from a render
   target (world-XZ layer). */
static int tt_every = -1;
static u32 tt_last;
static u64 tt_n[4][8][8];                 /* pass, lod, distance bin */
static u64 tt_region[4], tt_sz[4][11][11];
static u32 tt_rw[4][2], tt_raw[4][8];
static int tt_raw_ok[4];
static u64 tt_fine[4][8];
static u32 tt_bl[4][9];
static void tt_note(vpx *x, int pass, const ps2_vk_state *st, u32 v, u16 count) {
    float p[4], d;
    int bin;
    u64 t0;
    u32 lw, lh;
    if (tt_every < 0) { const char *e = getenv("ACZ_DEBUG_TERRAIN_TEX"); tt_every = e ? atoi(e) : 0; }
    if (tt_every <= 0) return;
    vp_rd(x, v, p);
    if (tm_eye_ok) {
        float dx = p[0] - tm_eye[0], dy = p[1] - tm_eye[1], dz = p[2] - tm_eye[2];
        d = sqrtf(dx * dx + dy * dy + dz * dz);
    } else d = p[3];
    for (bin = 0; bin < 7 && d >= (float)(1024 << bin); bin++) {}
    tt_n[pass][st->tex_lod & 7u][bin]++;
    if (count == 10u) tt_fine[pass][st->tex_lod & 7u]++;
    if (st->native_region) tt_region[pass]++;
    t0 = ps2_gs_cur_tex0();
    lw = (u32)((t0 >> 26) & 0xFull); lh = (u32)((t0 >> 30) & 0xFull);
    if (lw > 10) lw = 10;
    if (lh > 10) lh = 10;
    tt_sz[pass][lw][lh]++;
    if (st->wms == 2u) { tt_rw[pass][0] = st->maxu - st->minu + 1u; tt_rw[pass][1] = st->maxv - st->minv + 1u; }
    if (count == 10u && d < 8192.0f) {         /* texel size of a near fine patch: uv and world extents */
        static float acc[4][4];
        static u32 nacc[4];
        float u0 = 1e30f, u1 = -1e30f, x0 = 1e30f, x1 = -1e30f;
        for (u32 k = 0; k < 9u; k++) {
            float q[4], uv[4];
            vp_rd(x, v + 4u * k, q);
            vp_rd(x, v + 4u * k + 3u, uv);
            if (pass == 1 || pass == 2) uv[0] *= 16.0f;
            if (uv[0] < u0) u0 = uv[0];
            if (uv[0] > u1) u1 = uv[0];
            if (q[0] < x0) x0 = q[0];
            if (q[0] > x1) x1 = q[0];
        }
        if (x1 > x0) {
            acc[pass][0] += (u1 - u0) * st->tex_w;
            acc[pass][1] += x1 - x0;
            nacc[pass]++;
        }
        if (ps2_gs_frame_count - tt_last >= (u32)tt_every && nacc[pass]) {
            ps2_log("terrain-tex: pass %d near fine patches: %.1f texels over %.0f units = %.1f units (%.2f m) per texel "
                    "(tex %.0f wide, %u patches)", pass, acc[pass][0] / nacc[pass], acc[pass][1] / nacc[pass],
                    acc[pass][1] / acc[pass][0], acc[pass][1] / acc[pass][0] / 6.0f, st->tex_w, nacc[pass]);
            acc[pass][0] = acc[pass][1] = 0.0f; nacc[pass] = 0;
        }
    }
    if (!tt_raw_ok[pass]) {
        static const u32 dq[4][2] = { {0x19u, 0x23u}, {0x1Au, 0x1Eu}, {0x1Cu, 0x1Fu}, {0x1Du, 0x20u} };
        memcpy(tt_raw[pass], vp_q(x, dq[pass][0]), 16);
        memcpy(tt_raw[pass] + 4, vp_q(x, dq[pass][1]), 16);
        tt_raw_ok[pass] = 1;
        tt_bl[pass][0] = (u32)st->abe; tt_bl[pass][1] = st->alpha_a; tt_bl[pass][2] = st->alpha_b;
        tt_bl[pass][3] = st->alpha_c; tt_bl[pass][4] = st->alpha_d; tt_bl[pass][5] = st->alpha_fix;
        tt_bl[pass][6] = st->tfx; tt_bl[pass][7] = (u32)st->tcc;
        tt_bl[pass][8] = (u32)st->fge | (st->wms << 4) | (st->wmt << 8) | ((u32)ps2_vk_mesh_material_ok(st) << 12)
                       | ((u32)st->fst << 16) | (st->tex_rt ? 1u << 20 : 0u);
    }
    if (ps2_gs_frame_count - tt_last >= (u32)tt_every) {
        tt_last = ps2_gs_frame_count;
        for (int ps = 0; ps < 4; ps++) {
            char line[512];
            int n = 0;
            u64 tot = 0;
            for (int l = 0; l < 8; l++) for (int b2 = 0; b2 < 8; b2++) tot += tt_n[ps][l][b2];
            if (!tot) continue;
            n += snprintf(line + n, sizeof line - n, "terrain-tex: frame %u pass %d draws %llu region %llu;",
                          ps2_gs_frame_count, ps, (unsigned long long)tot, (unsigned long long)tt_region[ps]);
            for (int l = 0; l < 8; l++) {
                u64 s = 0;
                for (int b2 = 0; b2 < 8; b2++) s += tt_n[ps][l][b2];
                if (!s) continue;
                n += snprintf(line + n, sizeof line - n, " lod%d(fine %llu):", l, (unsigned long long)tt_fine[ps][l]);
                for (int b2 = 0; b2 < 8 && n < 480; b2++)
                    n += snprintf(line + n, sizeof line - n, "%s%llu", b2 ? "/" : "", (unsigned long long)tt_n[ps][l][b2]);
            }
            for (int a = 0; a < 11; a++) for (int c = 0; c < 11; c++)
                if (tt_sz[ps][a][c] && n < 480)
                    n += snprintf(line + n, sizeof line - n, " tex%ux%u:%llu", 1u << a, 1u << c,
                                  (unsigned long long)tt_sz[ps][a][c]);
            ps2_log("%s clamp %ux%u raw %08X%08X a%02X %08X%08X a%02X blend abe %u (%u-%u)*%u+%u fix %u tfx %u tcc %u "
                    "fge|wms|wmt|matok|fst|rt %05X",
                    line, tt_rw[ps][0], tt_rw[ps][1],
                    tt_raw[ps][1], tt_raw[ps][0], tt_raw[ps][2] & 0xFFu, tt_raw[ps][5], tt_raw[ps][4],
                    tt_raw[ps][6] & 0xFFu, tt_bl[ps][0], tt_bl[ps][1], tt_bl[ps][2], tt_bl[ps][3], tt_bl[ps][4],
                    tt_bl[ps][5], tt_bl[ps][6], tt_bl[ps][7], tt_bl[ps][8]);
        }
        memset(tt_n, 0, sizeof tt_n);
        memset(tt_region, 0, sizeof tt_region);
        memset(tt_fine, 0, sizeof tt_fine);
        memset(tt_sz, 0, sizeof tt_sz);
        memset(tt_raw_ok, 0, sizeof tt_raw_ok);
    }
}

int rn_vp_terrain(vpx *x, u32 pc, u32 *kick_at) {
    static u8 recs[10u * 64u];
    static u32 idx[10u * 3u];
    static const u32 tmpl[4] = { 0x00u, 0x01u, 0x02u, 0x03u };
    static const u32 data[4][2] = { {0x19u, 0x23u}, {0x1Au, 0x1Eu}, {0x1Cu, 0x1Fu},
                                    {0x1Du, 0x20u} };
    u32 t, v, prims[4], ni, vbase = 0, ifirst = 0, passes = 0;
    u16 count, flags;
    int stored;
    ps2_vk_mesh_block b;
    float fogc[4];
    (void)kick_at;
    if (pc == 0x0000u) {
        for (int k = 0; k < 4; k++) vp_lq(x, 5 + k, 0x10u + k, VP_MALL);
        for (int k = 0; k < 4; k++) vp_lq(x, 9 + k, 0x14u + k, VP_MALL);
        if (tm_mode < 0) tm_init();
        if (tm_mode > 0 || tm_dbg_frame || zero_water_on() || ps2_tmat_on())     /* (shader water, materials: */
            tm_eye_from_clip(x);                                                 /*  the eye) */
        x->tpc = 0x0050u;
        return 0;
    }
    if (pc != 0x0050u || !x->native || !vp_mesh_wanted()) { st_declined++; return -1; }
    {   /* ACZ_DEBUG_TREES (bug #25): the detail band constants whenever they change: VU 0x21 = (geomorph start,
           end, detail pass 1 distance, 1/that), VU 0x22 = (pass 2 offset xy, distance, 1/that) */
        static int dbg = -1;
        static u32 last[8];
        if (dbg < 0) dbg = getenv("ACZ_DEBUG_TREES") != NULL;
        if (dbg) {
            u32 now[8];
            memcpy(now, vp_q(x, 0x21u), 16);
            memcpy(now + 4, vp_q(x, 0x22u), 16);
            now[4] = now[5] = 0u;
            if (memcmp(now, last, sizeof now)) {
                float a[4], b[4];
                memcpy(last, now, sizeof now);
                vp_rd(x, 0x21u, a); vp_rd(x, 0x22u, b);
                ps2_log("terrain: frame %u geomorph %.1f..%.1f (scale s/512 = %.4f), detail pass 1 distance %.1f, "
                        "pass 2 distance %.1f", ps2_gs_frame_count, a[0], a[1], a[0] / 24576.0f, a[2], b[2]);
            }
        }
    }
    t = x->top & VP_QW_MASK;
    count = vp_ilw(x, t, 0);
    flags = vp_ilw(x, t, 1);
    if (count != 4u && count != 10u) { st_declined++; return -1; }
    for (int pass = 0; pass < 4; pass++) {
        prims[pass] = 0;
        if (pass && !((flags >> (pass - 1)) & 1u)) continue;
        if (!vp_tag_prim(x, tmpl[pass], &prims[pass]) || !vp_prim_tris(prims[pass])) {
            st_declined++;
            return -1;
        }
        passes++;
    }
    if (!ps2_vk_mesh_room(count, vp_tri_index_bound(count), passes)) {
        st_room++;
        st_declined++;
        return -1;
    }

    x->tpc = 0x0050u;
    build_patch(x, t, count);
    v = t + 3u;
    {   /* ACZ_DEBUG_TERRAIN_COL=f (realistic terrain T2): every patch vertex of GS frame f -- world position and the
           game's vertex colour -- to compare the colours with the heightmap's slope light (up to 6000 lines) */
        static long cf = -2;
        static int cn;
        if (cf == -2) { const char *e = getenv("ACZ_DEBUG_TERRAIN_COL"); cf = e ? atol(e) : -1; }
        if (cf >= 0 && (long)ps2_gs_frame_count == cf && cn < 6000)
            for (u32 k = 0; k < count && cn < 6000; k++, cn++) {
                float p[4];
                u32 col[4];
                vp_rd(x, v + 4u * k, p);
                memcpy(col, vp_q(x, v + 4u * k + 2u), 16);
                ps2_log("tcol: %.0f %.1f %.0f %u %u %u %u n%u f%X", p[0], p[1], p[2], col[0] & 255u, col[1] & 255u,
                        col[2] & 255u, col[3] & 255u, (unsigned)count, (unsigned)flags);
            }
    }
    if (flags & 0x10u) {
        u16 all = 0xFFF;
        for (u32 k = 0; k < count; k++) {
            float p[4];
            vp_rd(x, v + 4u * k, p);
            all &= clip_code(x, p);
        }
        if (all) { st_outside++; return 0; }
    }

    vp_copy_records(x, v, 4u, count, recs);
    ni = vp_tri_indices(prims[0] & 7u, count, 0, 0u, 0u, idx);
    /* near-ground materials (zero_terrain_mat.c, ACZ_TERRAIN_MATERIALS): a patch near the eye gets the smooth ground in
       its records' second quadword (unused by the terrain's passes otherwise) and the material fields of its block */
    ps2_vk_mesh_block tmb;
    int tmat = ps2_tmat_patch(recs, count, tm_eye, tm_eye_ok, &tmb);
    stored = ni && ps2_vk_mesh_store(recs, count, idx, ni, &vbase, &ifirst) == 0;
    if (ni && !stored) st_full++;

    rn_mesh_block_init(&b);
    for (int k = 0; k < 4; k++) memcpy(b.m + 4 * k, vp_F(x, 5 + k), 16);
    for (int k = 0; k < 4; k++) memcpy(b.c + 4 * k, vp_F(x, 9 + k), 16);
    vp_rd(x, 0x18u, fogc);
    b.fog0 = fogc[0];
    b.fog1 = fogc[1];
    b.clip = (flags & 0x10u) ? 1u : 0u;
    b.prog = 1u;
    vp_rd(x, 0x21u, b.p + 0);
    vp_rd(x, 0x22u, b.p + 4);
    vp_rd(x, 0x0Du, b.p + 8);
    b.p[12] = (flags & 6u) ? 128.0f : 255.0f;
    if (tm_mode > 0 && tm_eye_ok) {               /* detail texture fade by the distance from the eye (mesh.vert) */
        b.pad1 = 1u;
        b.p[13] = tm_eye[0]; b.p[14] = tm_eye[1]; b.p[15] = tm_eye[2];
    }
    if (tmat) {                                   /* (materials: pass 0 reads them; the eye where the water has it) */
        memcpy(b.l, tmb.l, sizeof b.l); memcpy(b.r, tmb.r, sizeof b.r); memcpy(b.e + 8, tmb.e + 8, 4 * sizeof(float));
    }

    {   static unsigned mg;                          /* (the NEXT MISSION tier: per patch from the new value on) */
        if (ps2_menv_take(&mg, ps2_menv_gen) && dh_hold_q != 0xFFFFFFFFu) {
            const u32 o = dh_hold_q;
            dh_init();
            ps2_menv_note("ACZ_TERRAIN_DETAIL_HOLD (x255)", o, dh_hold_q);
        }
    }
    if (dh_hold_q == 0xFFFFFFFFu) dh_init();
    dh_pad4 = dh_hold_q << 16;
    if (dh_ab > 0) {                               /* ACZ_TERRAIN_DETAIL_AB: on / off in blocks of N GS frames */
        static int last = -1;
        int on = (int)((ps2_gs_frame_count / (u32)dh_ab) & 1u) == 0;
        if (on != last) { last = on; ps2_log("terrain-detail-ab: frame %u hold %s", ps2_gs_frame_count, on ? "on" : "off"); }
        if (!on) dh_pad4 = 0u;
    }
    for (int pass = 0; pass < 4; pass++) {
        ps2_vk_state st;
        u32 w[4];
        if (pass && !((flags >> (pass - 1)) & 1u)) continue;
        memcpy(w, vp_q(x, 0x06u), 16);
        pkt_put(0, w);
        memcpy(w, vp_q(x, data[pass][0]), 16);
        pkt_put(1, w);
        memcpy(w, vp_q(x, data[pass][1]), 16);
        pkt_put(2, w);
        pkt_flush();
        ps2_gs_write_reg(0x00, prims[pass]);
        if (!stored) continue;
        ps2_gs_native_frame(&b.xoff, &b.yoff, &b.zmax);
        ps2_gs_native_state(&st);
        tt_note(x, pass, &st, v, count);
        b.pad4 = dh_pad4;
        if (dh_tint && pass == 0) b.pad4 |= (st.tex_lod ? 0x200u : 0x100u) | ((flags & 1u) ? 0x400u : 0u);
        b.pass = (u32)pass;
        if (pass == 0 && tmat) b.pad4 |= PS2_TMAT_PAD4;            /* (materials: the mesh.frag variant) */
        if (pass == 0 && (zero_water_on() || zero_texpack_on())) {   /* shader water (zero_water.c): a patch whose */
            ps2_vk_mesh_block bw = b;                                 /* tile carries water; T5 terrain pack */
            int tp = zero_texpack_patch(recs, count, &st, &bw);       /* (zero_texpack.c): the tile uv, e[6] = 1 */
            int wt = zero_water_on() && zero_water_patch(recs, count, tm_eye, tm_eye_ok, &bw);
            if (wt && bw.e[7] > 1.5f) bw.pad4 &= ~PS2_TMAT_PAD4;      /* (all water: no materials) */
            ps2_vk_draw_mesh(&st, (wt || tp) ? &bw : &b, vbase, ifirst, ni, 0);
        } else
            ps2_vk_draw_mesh(&st, &b, vbase, ifirst, ni, 0);
        b.pad4 &= ~PS2_TMAT_PAD4;                                   /* (no materials on the houses) */
        if (pass == 0) zero_props_patch(recs, count, idx, ni, &st, &b);   /* houses from the ground photo (zero_props.c) */
    }
    st_patches++;
    return 0;
}

void rn_vp_terrain_report(void) {
    if (st_patches || st_declined)
        ps2_log("rn: native terrain -- %llu patches drawn by mesh.vert (%llu with "
                "a clip test finding them wholly outside, %llu not stored), %llu "
                "left to VU1 (%llu of them for mesh arena space)",
                (unsigned long long)st_patches, (unsigned long long)st_outside,
                (unsigned long long)st_full, (unsigned long long)st_declined,
                (unsigned long long)st_room);
    ps2_tmat_report();
}
