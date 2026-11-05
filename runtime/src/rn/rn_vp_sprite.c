#include "rn_vp_int.h"

#include <stdio.h>
#include <stdlib.h>

static u64 st_trees, st_culled, st_batches[3];

#define F(r) (x->vf[r].f)

static int mac_sign(float f) {
    u32 u;
    memcpy(&u, &f, 4);
    return (u & 0x80000000u) && (u & 0x7FFFFFFFu);
}

static float vmax(float a, float b) { return ps2_vu_select(a, b, 0); }
static float vmin(float a, float b) { return ps2_vu_select(a, b, 1); }
/* cloud shadows on the ground (ps2_video.c, ACZ_VR_CLOUD_SHADOW_GROUND): a tree's light factor (1 = full sun) */
float ps2_vk_gshadow_tree(const float w[3]);

/* ACZ_DEBUG_TREES=N (debug, bug #25): the tree program's game constants whenever they change (VU 0x19 = max
   distance / fade start, VU 0x18, the screen scale of the world->screen matrix, the GS qwords of the near and far
   paths: TEST 0x2D / 0x2E and the GIF tags, per tree type the size / texture qwords), every N GS frames a summary
   (trees drawn on the near / far path, culled by the near/far plane, the max distance, the circle and the screen box,
   nearest / farthest drawn, trees drawn this frame that were not drawn the frame before: "new"), and up to 4 lines a
   frame for new trees nearer than 3/4 of the max distance (pops). ACZ_DEBUG_TREES_TINT=1 tints new trees magenta,
   near-path trees red and far-path trees green in the picture (eye dumps). ACZ_DEBUG_TREES_NOZ=1 draws trees without
   the depth test (shows trees the terrain hides). */
static int td_on = -1, td_every = 60, td_tint, td_noz;
static u32 td_frame = 0xFFFFFFFFu, td_prev = 0xFFFFFFFFu, td_lines;
static struct { u32 drawn, nearp, farp, c_plane, c_max, c_circle, c_box, fresh, pops, boxnear, gone, gone_near;
                float pmin, pmax, fresh_pmin, alpha_fresh, gone_pmin; } td;
static u32 td_last[10][4], td_types[16], td_ntypes;
typedef struct { u32 key, stamp; u8 why; float p; } td_ent;
static td_ent td_tab[16384];
static float td_max, td_fade;

static void td_init(void) {
    const char *e = getenv("ACZ_DEBUG_TREES");
    td_on = e && atoi(e) > 0;
    if (td_on) td_every = atoi(e);
    e = getenv("ACZ_DEBUG_TREES_TINT"); td_tint = e && *e == '1';
    e = getenv("ACZ_DEBUG_TREES_NOZ"); td_noz = e && *e == '1';
    if (td_tint || td_noz) td_on = 1;
}

static void td_flush(void) {
    if (td_frame != 0xFFFFFFFFu && td_prev != 0xFFFFFFFFu)
        for (u32 i = 0; i < 16384u; i++)
            if (td_tab[i].key && td_tab[i].stamp == td_prev && td_tab[i].why == 0u) {
                td.gone++;
                if (td_tab[i].p < td.gone_pmin) td.gone_pmin = td_tab[i].p;
                if (td_tab[i].p < 0.75f * td_max) td.gone_near++;
                td_tab[i].why = 0xFEu;                   /* counted once */
            }
    if (td_frame != 0xFFFFFFFFu && td.drawn + td.c_plane + td.c_max + td.c_circle + td.c_box
        && (td_every <= 1 || td_frame % (u32)td_every == 0u))
        ps2_log("trees: frame %u max %.0f fade %.0f | drawn %u (near %u far %u) culled plane %u max %u circle %u "
                "box %u (box within 1.25x: %u) | drawn p %.0f..%.0f | new %u (nearest %.0f, alpha %.0f) pops %u | gone %u "
                "(not submitted; nearest %.0f, %u nearer than 3/4 max)",
                td_frame, td_max, td_fade, td.drawn, td.nearp, td.farp, td.c_plane, td.c_max, td.c_circle, td.c_box,
                td.boxnear, td.drawn ? td.pmin : 0.0f, td.pmax, td.fresh, td.fresh ? td.fresh_pmin : 0.0f,
                td.alpha_fresh, td.pops, td.gone, td.gone ? td.gone_pmin : 0.0f, td.gone_near);
    memset(&td, 0, sizeof td);
    td.pmin = 1e30f; td.fresh_pmin = 1e30f; td.gone_pmin = 1e30f;
}

static void td_frame_check(void) {
    if (ps2_gs_frame_count == td_frame) return;
    td_flush();
    td_prev = td_frame;
    td_frame = ps2_gs_frame_count;
    td_lines = 0;
}

static td_ent *td_find(const float *w) {
    u32 a = (u32)(s32)floorf(w[0] * 0.125f), b = (u32)(s32)floorf(w[1] * 0.125f), c = (u32)(s32)floorf(w[2] * 0.125f);
    u32 h, best;                                  /* (the batch origin moves by float noise: 8-unit cells) */
    h = (a * 0x9E3779B1u) ^ (b * 0x85EBCA77u) ^ (c * 0xC2B2AE3Du);
    if (!h) h = 1u;
    best = h & 16383u;
    for (u32 i = 0; i < 8u; i++) {
        td_ent *t = &td_tab[(h + i) & 16383u];
        if (t->key == h) return t;
        if (t->stamp < td_tab[best].stamp || !t->key) best = (h + i) & 16383u;
    }
    td_tab[best].key = h; td_tab[best].stamp = 0xFFFFFFFEu; td_tab[best].why = 0xFFu;
    return &td_tab[best];
}

/* a tree finished: why 0 = drawn, 1 near/far plane, 2 max distance, 3 circle, 4 screen box; returns 1 for a new tree */
static int td_note(const float *w, float p, int why, int far_path, float alpha, const float *scr, const float *half) {
    td_ent *t = td_find(w);
    int was = t->stamp == td_prev && t->why == 0u, fresh = 0;
    u8 prev_why = t->stamp == td_prev ? t->why : 0xFFu;
    t->stamp = td_frame; t->why = (u8)why; t->p = p;
    if (was && why && p < 0.75f * td_max && td_lines < 8u) {
        td_lines++;
        ps2_log("trees: vanish frame %u p %.0f (max %.0f): %s", td_frame, p, td_max,
                why == 1 ? "plane cull" : why == 2 ? "max distance" : why == 3 ? "circle cull" : "box cull");
    }
    switch (why) {
    case 1: td.c_plane++; return 0;
    case 2: td.c_max++; return 0;
    case 3: td.c_circle++; return 0;
    case 4:
        td.c_box++;
        if (scr && fabsf(scr[0] - 2048.0f) < 320.0f && fabsf(scr[1] - 2048.0f) < 280.0f) td.boxnear++;
        return 0;
    default: break;
    }
    td.drawn++;
    if (far_path) td.farp++; else td.nearp++;
    if (p < td.pmin) td.pmin = p;
    if (p > td.pmax) td.pmax = p;
    if (!was && td_prev != 0xFFFFFFFFu) {
        fresh = 1;
        td.fresh++;
        if (p < td.fresh_pmin) { td.fresh_pmin = p; td.alpha_fresh = alpha; }
        if (p < 0.75f * td_max) {
            td.pops++;
            if (td_lines < 4u) {
                td_lines++;
                ps2_log("trees: pop frame %u p %.0f (max %.0f fade %.0f) alpha %.0f %s path, screen %.0f,%.0f half "
                        "%.1f x %.1f, previous frame: %s", td_frame, p, td_max, td_fade, alpha,
                        far_path ? "far" : "near", scr[0], scr[1], half[0], half[1],
                        prev_why == 0xFFu ? "not submitted" : prev_why == 1u ? "plane cull" : prev_why == 2u
                        ? "max distance" : prev_why == 3u ? "circle cull" : prev_why == 4u ? "box cull" : "drawn");
            }
        }
    }
    return fresh;
}

static void td_constants(vpx *x) {
    static const u32 qa[10] = { 0x18u, 0x19u, 0x2Du, 0x2Eu, 0x00u, 0x01u, 0x02u, 0x05u, 0x06u, 0x32u };
    u32 now[10][4];
    for (int i = 0; i < 10; i++) memcpy(now[i], vp_q(x, qa[i]), 16);
    now[0][0] = now[0][2] = now[0][3] = 0u;        /* x (a per-frame scale), d and w may move */
    now[2][1] = now[2][3] = now[3][1] = now[3][3] = 0u;   /* TEST: only the register value and address */
    td_max = vp_rd_lane(x, 0x19u, 0); td_fade = vp_rd_lane(x, 0x19u, 1);
    if (!memcmp(now, td_last, sizeof now)) return;
    memcpy(td_last, now, sizeof now);
    td_ntypes = 0;
    {
        float f18[4], f19[4], f32[4], m[4][4], fx, fy, wn;
        vp_rd(x, 0x18u, f18); vp_rd(x, 0x19u, f19); vp_rd(x, 0x32u, f32);
        for (int k = 0; k < 4; k++) vp_rd(x, 0x10u + (u32)k, m[k]);
        fx = sqrtf(powf(m[0][0] - 2048.0f * m[0][3], 2) + powf(m[1][0] - 2048.0f * m[1][3], 2)
                   + powf(m[2][0] - 2048.0f * m[2][3], 2));
        fy = sqrtf(powf(m[0][1] - 2048.0f * m[0][3], 2) + powf(m[1][1] - 2048.0f * m[1][3], 2)
                   + powf(m[2][1] - 2048.0f * m[2][3], 2));
        wn = sqrtf(m[0][3] * m[0][3] + m[1][3] * m[1][3] + m[2][3] * m[2][3]);
        ps2_log("trees: constants max %.1f fade %.1f (VU19 %.1f %.1f %.1f %.1f) VU18 %.4g %.4g %.4g %.4g fog %.4g %.4g "
                "screen scale fx %.2f fy %.2f (w norm %.3f, fx / VU18.z %.3f)", f19[0], f19[1], f19[0], f19[1],
                f19[2], f19[3], f18[0], f18[1], f18[2], f18[3], f32[0], f32[1], fx / wn, fy / wn, wn,
                f18[2] != 0.0f ? fx / wn / f18[2] : 0.0f);
        for (int i = 2; i < 10; i++)
            ps2_log("trees:   VU %02X = %08X %08X %08X %08X", qa[i], now[i][0], now[i][1], now[i][2], now[i][3]);
    }
}

static void td_type(vpx *x, u16 t) {
    float s[4], u[4], c[4];
    u32 tx[4];
    for (u32 i = 0; i < td_ntypes; i++) if (td_types[i] == t) return;
    if (td_ntypes < 16u) td_types[td_ntypes++] = t;
    vp_rd(x, (u32)(t + 0x24u), s); vp_rd(x, (u32)(t + 0x27u), u); vp_rd(x, (u32)(t + 0x2Au), c);
    memcpy(tx, vp_q(x, (u32)(t + 0x1Bu)), 16);
    ps2_log("trees:   type %u: centre offset %.1f %.1f (%.1f %.1f), uv %.3f %.3f, half size %.1f %.1f, tex %08X%08X", t,
            s[0], s[1], s[2], s[3], u[0], u[1], c[0], c[1], tx[1], tx[0]);
}

static int tree(vpx *x, u32 rec, u16 t, int mode1, u32 o) {
    float *v1 = F(1), *v2 = F(2), *v3 = F(3), *v13 = F(13), *v14 = F(14), *v15 = F(15);
    float *v18 = F(18), *v19 = F(19), *v21 = F(21), *v22 = F(22), *v24 = F(24);
    float *v25 = F(25), *v26 = F(26), *v27 = F(27), *v28 = F(28), *v29 = F(29);
    float tag_a[4], test[4], tag_s[4], tex[4], q, p, acc, wpos[3], half[2] = { 0.0f, 0.0f };
    int far_path;

    vp_lq(x, 13, rec, VP_MALL);
    for (int i = 0; i < 3; i++) v13[i] = F(17)[i] + v13[i];
    memcpy(wpos, v13, sizeof wpos);
    v18[0] = 0.0f; v18[1] = 0.0f;
    for (int i = 0; i < 3; i++) v19[i] = 1.0f;
    vp_lq(x, 1, (u32)(t + 0x27u), VP_MALL);
    v18[2] = 1.0f;
    vp_lq(x, 3, (u32)(t + 0x27u), VP_MALL);
    vp_rows(v1, F(9), F(10), F(11), F(12), v13[0], v13[1], v13[2], v13[3], VP_MALL);
    v18[0] = 0.0f + v3[0];
    v19[0] = 0.0f + v3[1];
    for (int i = 0; i < 3; i++) v25[i] = v13[i] - F(20)[i];
    v3[2] = v1[2] + v1[3];
    acc = 0.0f + v1[3];
    v2[2] = acc - v1[2] * 1.0f;
    p = sqrtf(v25[0] * v25[0] + v25[1] * v25[1] + v25[2] * v25[2]);
    if (mac_sign(v3[2]) || mac_sign(v2[2])) {
        if (td_on > 0) td_note(wpos, p, 1, 0, 0.0f, NULL, NULL);
        return 0;
    }

    v1[0] = F(16)[0] - F(16)[1];
    for (int i = 0; i < 4; i++) v22[i] = p;
    v1[1] = v22[1] - F(16)[1];
    q = ps2_fdiv(v1[1], v1[0]);
    v1[0] = 0.0f + q; v1[1] = 0.0f + q; v1[2] = 0.0f + q; v1[3] = 1.0f + q;
    v2[0] = v1[1]; v2[1] = v1[2]; v2[2] = v1[3]; v2[3] = v1[0];
    {
        float acc[4] = { 0.0f + 127.0f, 0.0f + 127.0f, 0.0f + 127.0f, 1.0f + 127.0f };
        for (int i = 0; i < 4; i++) v21[i] = acc[i] - v2[i] * 127.0f;
    }
    far_path = mac_sign(v21[3] - 127.0f);
    vp_rd(x, 0x02u, tag_a);
    if (!far_path) {
        vp_rd(x, 0x2Du, test);
        vp_rd(x, mode1 ? 0x01u : 0x06u, tag_s);
        {   float i0 = mode1 ? 128.0f : 127.0f;
            v21[0] = 0.0f + i0; v21[1] = 0.0f + i0; v21[2] = 0.0f + i0; v21[3] = 1.0f + i0; }
    } else {
        vp_rd(x, 0x2Eu, test);
        vp_rd(x, mode1 ? 0x00u : 0x05u, tag_s);
    }
    memcpy(F(23), tag_s, 16);
    memcpy(v1, tag_a, 16);
    memcpy(v2, test, 16);

    for (int i = 0; i < 4; i++) v2[i] = F(16)[i] - v22[i];
    if (mac_sign(v2[0])) {
        if (td_on > 0) td_note(wpos, p, 2, far_path, 0.0f, NULL, NULL);
        return 0;
    }
    if (mac_sign(v2[2]))      vp_rd(x, (u32)(t + 0x21u), tex);
    else if (mac_sign(v2[3])) vp_rd(x, (u32)(t + 0x1Eu), tex);
    else                      vp_rd(x, (u32)(t + 0x1Bu), tex);
    memcpy(v2, tex, 16);

    vp_rows(v13, F(5), F(6), F(7), F(8), v13[0], v13[1], v13[2], v13[3], VP_MALL);
    q = ps2_fdiv(1.0f, v13[3]);
    v13[2] = v13[2] + F(4)[3];
    for (int i = 0; i < 3; i++) v13[i] = v13[i] * q;
    vp_lq(x, 1, 0x32u, VP_MALL);
    acc = 1.0f * v1[0];
    v13[3] = acc + v13[3] * v1[1];
    v13[3] = vmin(v13[3], 255.0f);
    v13[2] = vmin(v13[2], 16777215.0f);
    v13[3] = vmax(v13[3], 0.0f);
    v13[2] = vmax(v13[2], 0.0f);
    vp_lq(x, 1, (u32)(t + 0x24u), VP_MALL);
    v1[0] = v1[0] * F(4)[2]; v1[1] = v1[1] * F(4)[2];
    v1[0] = v1[0] * q;       v1[1] = v1[1] * q;
    v13[0] = v13[0] + v1[0]; v13[1] = v13[1] + v1[1];
    vp_lq(x, 2, (u32)(t + 0x2Au), VP_MALL);
    v1[0] = 0.0f + v2[0];    v1[1] = 0.0f + v2[1];
    v1[0] = v1[0] * F(4)[2]; v1[1] = v1[1] * F(4)[2];
    v1[0] = v1[0] * q;       v1[1] = v1[1] * q;
    v2[0] = v1[0] * v1[0];   v2[1] = v1[1] * v1[1];
    half[0] = v1[0]; half[1] = v1[1];
    v3[0] = v13[0] - 2048.0f; v3[1] = v13[1] - 2048.0f;
    v2[0] = v2[0] + v2[1];
    v3[0] = v3[0] * v3[0];   v3[1] = v3[1] * v3[1];
    v2[0] = v2[0] + 462848.0f;
    v3[0] = v3[0] + v3[1];
    if (mac_sign(v2[0] - v3[0])) {
        if (td_on > 0) td_note(wpos, p, 3, far_path, 0.0f, v13, half);
        return 0;
    }

    v14[0] = v14[1] = v14[2] = 0.0f; v14[3] = 1.0f;
    v15[0] = v15[1] = v15[2] = 0.0f; v15[3] = 1.0f;
    v14[0] = v13[0] - v1[0]; v14[1] = v13[1] - v1[1];
    v15[0] = v13[0] + v1[0]; v15[1] = v13[1] + v1[1];
    v14[2] = v13[2] + 0.0f;  v15[2] = v13[2] + 0.0f;
    v14[3] = v13[3] * 1.0f;  v15[3] = v13[3] * 1.0f;
    v1[0] = 0.0f + 2304.0f;  v1[1] = 0.0f + 2272.0f;
    v2[0] = 0.0f + 1792.0f;  v2[1] = 0.0f + 1824.0f;
    if (!mac_sign(v14[0] - v1[0]) || !mac_sign(v14[1] - v1[1])
        || mac_sign(v15[0] - v2[0]) || mac_sign(v15[1] - v2[1])) {
        if (td_on > 0) td_note(wpos, p, 4, far_path, 0.0f, v13, half);
        return 0;
    }

    v25[0] = v15[0] - v14[0]; v25[1] = v15[1] - v14[1];
    v26[0] = v1[0] - v14[0];  v26[1] = v1[1] - v14[1];
    v27[0] = v2[0] - v14[0];  v27[1] = v2[1] - v14[1];
    v28[0] = v19[0] - v18[0]; v28[1] = v19[1] - v18[1];
    v29[0] = 0.0f + v18[0];   v29[1] = 0.0f + v18[1];
    if (mac_sign(v14[0] - v2[0])) {
        q = ps2_fdiv(v27[0], v25[0]);
        v14[0] = 0.0f + v2[0];
        v3[0] = v28[0] * q;
        v18[0] = v29[0] + v3[0];
    }
    if (mac_sign(v14[1] - v2[1])) {
        q = ps2_fdiv(v27[1], v25[1]);
        v18[1] = 0.0f + q;
        v14[1] = 0.0f + v2[1];
    }
    if (!mac_sign(v15[0] - v1[0])) {
        q = ps2_fdiv(v26[0], v25[0]);
        v15[0] = 0.0f + v1[0];
        v3[0] = v28[0] * q;
        v19[0] = v29[0] + v3[0];
    }
    if (!mac_sign(v15[1] - v1[1])) {
        q = ps2_fdiv(v26[1], v25[1]);
        v19[1] = 0.0f + q;
        v15[1] = 0.0f + v1[1];
    }

    v24[0] = v24[1] = v24[2] = 0.0f + 128.0f;
    v21[0] = v21[0] - 0.0f; v21[1] = v21[1] - 0.0f;
    v21[2] = v21[2] - 0.0f; v21[3] = v21[3] - 1.0f;
    v24[3] = v21[3] + 1.0f;
    if (td_on > 0 && td_note(wpos, p, 0, far_path, v24[3], v13, half) && td_tint) {
        v24[0] = 255.0f; v24[1] = 0.0f; v24[2] = 255.0f;
    } else if (td_tint) {
        v24[0] = far_path ? 90.0f : 200.0f; v24[1] = far_path ? 200.0f : 90.0f; v24[2] = 90.0f;
    } else {                                      /* cloud shadows on the ground: the same factor as the ground's */
        float f = ps2_vk_gshadow_tree(wpos);
        if (f < 1.0f) { v24[0] *= f; v24[1] *= f; v24[2] *= f; }
    }
    for (int i = 0; i < 4; i++) {
        s32 c = vp_ftoi(v24[i], 0);
        memcpy(&v24[i], &c, 4);
    }

    pkt_putf(o + 0u, tag_a);
    if (td_noz) {                                 /* debug: ZTST = always */
        u32 lo;
        memcpy(&lo, &test[0], 4);
        lo = (lo & ~(3u << 17)) | (1u << 17);
        memcpy(&test[0], &lo, 4);
    }
    pkt_putf(o + 1u, test);
    pkt_putf(o + 2u, tex);
    pkt_putf(o + 3u, tag_s);
    pkt_putf(o + 4u, v24);
    pkt_putf(o + 5u, v18);
    pkt_put_xyzf(o + 6u, v14, vp_ftoi(v14[3], 4));
    pkt_putf(o + 7u, v24);
    pkt_putf(o + 8u, v19);
    pkt_put_xyzf(o + 9u, v15, vp_ftoi(v15[3], 4));
    for (int i = 0; i < 4; i++) {
        s32 a = vp_ftoi(v14[i], 4), b = vp_ftoi(v15[i], 4);
        memcpy(&v14[i], &a, 4);
        memcpy(&v15[i], &b, 4);
    }
    return 1;
}

int rn_vp_trees(vpx *x, u32 pc, u32 *kick_at) {
    u32 top, o = 0;
    u16 mode, count;
    if (pc == 0x0000u) {
        for (int k = 0; k < 8; k++) vp_lq(x, 5 + k, 0x10u + (u32)k, VP_MALL);
        vp_lq(x, 4, 0x18u, VP_MALL);
        vp_lq(x, 16, 0x19u, VP_MALL);
        vp_lq(x, 20, 0x1Au, VP_MALL);
        x->tpc = 0x0068u;
        return 0;
    }
    if (pc != 0x0068u) return -1;
    if (td_on < 0) td_init();
    if (td_on > 0) { td_frame_check(); td_constants(x); }
    top = x->top & VP_QW_MASK;
    mode = vp_ilw(x, top, 1);
    count = vp_ilw(x, top, 0);
    if (!count) return -1;
    x->vi[15] = (u16)top;
    x->vi[1] = mode;
    x->vi[2] = 1;
    x->vi[3] = 2;
    if (mode != 1u) {
        u16 t = vp_ilw(x, top, 3);
        if (td_on > 0) td_type(x, t);
        u16 out = (u16)(top + vp_ilw(x, top, 2));
        x->vi[6] = count;
        x->vi[11] = t;
        vp_lq(x, 17, top + 1u, VP_MALL);
        x->vi[14] = (u16)(top + 2u);
        x->vi[10] = 0;
        x->vi[12] = out;
        x->vi[13] = vp_ilw(x, top, 2);
        st_batches[mode == 0u ? 0 : 2]++;
        do {
            if (tree(x, x->vi[14], t, 0, o)) {
                pkt_kick(x->vi[12]);
                o += 10u;
                x->vi[12] = (u16)(x->vi[12] + 10u);
                st_trees++;
            } else {
                st_culled++;
            }
            x->vi[10]++;
            x->vi[14]++;
        } while (x->vi[6] != x->vi[10]);
        x->tpc = 0x0068u;
        return o ? 1 : 0;
    }

    {
        u16 out = (u16)(top + vp_ilw(x, top, 2));
        x->vi[13] = out;
        x->vi[12] = out;
        x->vi[14] = (u16)(top + 1u);
        x->vi[6] = 0;
        st_batches[1]++;
        if (td_on > 0) for (u16 k = 0; k < 3u; k++) td_type(x, k);
        for (;;) {
            u16 ends[3];
            vp_lq(x, 17, x->vi[14], VP_MALL);
            ends[0] = vp_ilw(x, x->vi[14] + 1u, 0);
            ends[1] = vp_ilw(x, x->vi[14] + 1u, 1);
            ends[2] = vp_ilw(x, x->vi[14] + 1u, 2);
            x->vi[7] = ends[0]; x->vi[9] = ends[1]; x->vi[8] = ends[2];
            x->vi[14] = (u16)(x->vi[14] + 2u);
            x->vi[11] = 0;
            x->vi[10] = 0;
            while (x->vi[11] != 3u) {
                if (x->vi[10] == ends[x->vi[11]]) { x->vi[11]++; continue; }
                if (tree(x, x->vi[14], x->vi[11], 1, o)) {
                    o += 10u;
                    x->vi[12] = (u16)(x->vi[12] + 10u);
                    st_trees++;
                } else {
                    st_culled++;
                }
                x->vi[14]++;
                x->vi[10]++;
                if (o > 10u * 1024u) return o ? 1 : 0;
            }
            x->vi[6]++;
            if (x->vi[6] == count) break;
        }
        {
            float term[4];
            vp_rd(x, 0x04u, term);
            pkt_putf(o, term);
        }
        x->vi[1] = 3;
        *kick_at = out;
        x->tpc = 0x0068u;
        return 1;
    }
}

void rn_vp_sprite_report(void) {
    if (st_trees || st_culled)
        ps2_log("rn: native tree billboards -- %llu sprites, %llu culled; batches mode 0 "
                "%llu, grouped %llu, mode 2 %llu", (unsigned long long)st_trees,
                (unsigned long long)st_culled, (unsigned long long)st_batches[0],
                (unsigned long long)st_batches[1], (unsigned long long)st_batches[2]);
}
