#include "rn_vp_int.h"

#include <stdio.h>
#include <stdlib.h>

static u64 st_fast, st_outside, st_clipped, st_declined_clip, st_declined_other;

static int aircraft;
static u64 st_mode[2][3][2];

typedef struct { float p[4], n[4], col[4], uv[4]; } mv_in;
typedef struct {
    float st[4], rgba[4], xyz[4];
    float est[4], spec[4];
} mv_out;

enum { MODE_PLAIN, MODE_LIT, MODE_ENV };

static void rd_in(vpx *x, u32 a, mv_in *in) {
    vp_rd(x, a, in->p);
    vp_rd(x, a + 1u, in->n);
    vp_rd(x, a + 2u, in->col);
    vp_rd(x, a + 3u, in->uv);
}

static void model_vertex(vpx *x, int mode, const mv_in *in, mv_out *o) {
    float f13[4], uv[4], fogc[4], q;
    const float *M5 = vp_F(x, 5), *M6 = vp_F(x, 6), *M7 = vp_F(x, 7), *M8 = vp_F(x, 8);
    vp_rd(x, 0x18u, fogc);
    uv[0] = in->uv[0]; uv[1] = in->uv[1]; uv[2] = 1.0f; uv[3] = vp_F(x, 16)[3];
    vp_rows(f13, M5, M6, M7, M8, in->p[0], in->p[1], in->p[2], 1.0f, VP_MALL);

    if (mode == MODE_PLAIN) {
        q = 1.0f / f13[3];
        f13[2] = f13[2] * (aircraft ? 16.0f : vp_F(x, 20)[2]);
        f13[3] = 1.0f * fogc[0] + f13[3] * fogc[1];
        f13[0] = f13[0] * q; f13[1] = f13[1] * q; f13[2] = f13[2] * q;
        uv[0] = uv[0] * q; uv[1] = uv[1] * q; uv[2] = uv[2] * q;
        f13[3] = vp_min(f13[3], 255.0f);
        f13[2] = vp_min(f13[2], 16777215.0f);
        f13[3] = vp_max(f13[3], 0.0f);
        f13[2] = vp_max(f13[2], 0.0f);
        memcpy(o->st, uv, 16);
        memcpy(o->rgba, in->col, 16);
        memcpy(o->xyz, f13, 16);
        return;
    }

    {
        float n[4], l[4], l2[4], col[4], acc;
        const float *L17 = vp_F(x, 17), *L18 = vp_F(x, 18), *L19 = vp_F(x, 19);
        const float *R21 = vp_F(x, 21), *R22 = vp_F(x, 22), *R23 = vp_F(x, 23),
                    *R24 = vp_F(x, 24);
        float e25[4] = {0}, e4[4] = {0}, e26[4] = {0};
        for (int i = 0; i < 3; i++) n[i] = vp_lane_itof(in->n[i], 12);
        n[3] = 0.0f;
        for (int i = 0; i < 3; i++) {
            acc = L17[i] * n[0];
            acc = acc + L18[i] * n[1];
            l[i] = acc + L19[i] * n[2];
        }
        l[3] = 1.0f;
        q = 1.0f / f13[3];
        if (aircraft) f13[2] = f13[2] * 16.0f;
        for (int i = 0; i < 4; i++) col[i] = vp_lane_itof(in->col[i], 0);
        if (mode == MODE_ENV) {
            f13[3] = 1.0f * fogc[0] + f13[3] * fogc[1];
            for (int i = 0; i < 3; i++) {
                acc = vp_F(x, 9)[i] * n[0];
                acc = acc + vp_F(x, 10)[i] * n[1];
                e25[i] = acc + vp_F(x, 11)[i] * n[2];
            }
            e25[3] = vp_F(x, 25)[3];
        }
        for (int i = 0; i < 4; i++) l2[i] = l[i] * l[i];
        f13[0] = f13[0] * q; f13[1] = f13[1] * q; f13[2] = f13[2] * q;
        for (int i = 0; i < 4; i++) l[i] = l2[i] * l[i];
        uv[0] = uv[0] * q; uv[1] = uv[1] * q; uv[2] = uv[2] * q;
        if (mode == MODE_ENV) {
            e4[0] = vp_max(e25[0], 0.0f); e4[1] = vp_max(e25[1], 0.0f);
            e4[2] = vp_max(e25[2], 0.0f); e4[3] = vp_max(e25[3], 1.0f);
        }
        if (!aircraft) f13[2] = f13[2] * vp_F(x, 20)[2];
        for (int i = 0; i < 3; i++) l[i] = l[i] * 2.0f;
        if (mode == MODE_LIT) {
            for (int i = 0; i < 4; i++) l[i] = vp_max(l[i], 0.0f);
            f13[3] = 1.0f * fogc[0] + f13[3] * fogc[1];
        } else {
            f13[3] = vp_min(f13[3], 255.0f);
            e25[1] = e25[2];
            e4[0] = e4[1]; e4[2] = e4[1];
            f13[2] = vp_min(f13[2], 16777215.0f);
            for (int i = 0; i < 4; i++) l[i] = vp_max(l[i], 0.0f);
            f13[3] = vp_max(f13[3], 0.0f);
            for (int i = 0; i < 3; i++) e4[i] = e4[i] * 1280.0f;
            e25[0] = e25[0] + 1.0f; e25[1] = e25[1] + 1.0f;
        }
        vp_rows(l2, R21, R22, R23, R24, l[0], l[1], l[2], l[3], VP_MALL);
        memcpy(l, l2, 16);
        if (mode == MODE_LIT) {
            f13[2] = vp_min(f13[2], 16777215.0f);
            f13[3] = vp_min(f13[3], 255.0f);
            for (int i = 0; i < 4; i++) l[i] = vp_max(l[i], 0.0f);
            for (int i = 0; i < 3; i++) col[i] = col[i] * l[i];
            col[3] = 1.0f * 128.0f;
            f13[2] = vp_max(f13[2], 0.0f);
            f13[3] = vp_max(f13[3], 0.0f);
            for (int i = 0; i < 3; i++) col[i] = vp_min(col[i], 255.0f);
        } else {
            e26[3] = vp_rd_lane(x, 0x08u, 3);
            f13[2] = vp_max(f13[2], 0.0f);
            for (int i = 0; i < 3; i++) e4[i] = vp_min(e4[i], 128.0f);
            e25[0] = e25[0] * 0.5f; e25[1] = e25[1] * 0.5f;
            for (int i = 0; i < 4; i++) l[i] = vp_max(l[i], 0.0f);
            for (int i = 0; i < 3; i++) col[i] = col[i] * l[i];
            col[3] = 1.0f * 128.0f;
            for (int i = 0; i < 3; i++) col[i] = vp_min(col[i], 255.0f);
            e25[2] = 0.0f + 1.0f;
            for (int i = 0; i < 3; i++) e26[i] = 0.0f + e4[i];
            for (int i = 0; i < 3; i++) e25[i] = e25[i] * uv[2];
            for (int i = 0; i < 4; i++) o->spec[i] = vp_lane_ftoi(e26[i], 0);
            memcpy(o->est, e25, 16);
        }
        for (int i = 0; i < 4; i++) o->rgba[i] = vp_lane_ftoi(col[i], 0);
        memcpy(o->st, uv, 16);
        memcpy(o->xyz, f13, 16);
    }
}

typedef struct {
    u16 flags, count;
    int changed;
    float C[4][4];
} model_setup;

static void setup_read(vpx *x, model_setup *s) {
    u32 t = x->top & VP_QW_MASK;
    s->count = vp_ilw(x, t, 0);
    s->changed = (s16)vp_ilw(x, 0x1B, 2) > 0;
    s->flags = s->changed ? vp_ilw(x, 0x1B, 0) : x->vi[5];
    if (s->changed) {
        float B[4][4], A[4][4];
        for (int k = 0; k < 4; k++) {
            vp_rd(x, 0x28u + k, B[k]);
            vp_rd(x, 0x14u + k, A[k]);
            if (aircraft)
                for (int i = 0; i < 3; i++) B[k][i] = B[k][i] * 16.0f;
        }
        for (int k = 0; k < 4; k++)
            vp_rows(s->C[k], A[0], A[1], A[2], A[3], B[k][0], B[k][1], B[k][2], B[k][3],
                    VP_MALL);
    } else {
        for (int k = 0; k < 4; k++) vp_rd(x, 0x30u + k, s->C[k]);
    }
}

static u32 setup_commit(vpx *x, const model_setup *s, u32 vi7) {
    u32 vi8 = vi7;
    u16 vi5;
    if (!s->changed) {
        for (int k = 0; k < 4; k++) vp_lq(x, 9 + k, 0x30u + k, VP_MALL);
        return vi8;
    }
    vp_isw(x, 0x1B, 2, 0);
    vi5 = vp_ilw(x, 0x1B, 0);
    x->vi[5] = vi5;
    if (aircraft) {
        for (int k = 0; k < 4; k++) vp_lq(x, 21 + k, 0x28u + k, VP_MALL);
        for (int k = 0; k < 4; k++) vp_lq(x, 1 + k, 0x20u + k, VP_MALL);
        for (int k = 0; k < 3; k++)
            vp_rows(vp_F(x, 17 + k), vp_F(x, 1), vp_F(x, 2), vp_F(x, 3), vp_F(x, 4),
                    vp_F(x, 21 + k)[0], vp_F(x, 21 + k)[1], vp_F(x, 21 + k)[2],
                    vp_F(x, 21 + k)[3], VP_MALL);
        for (int k = 0; k < 4; k++)
            for (int i = 0; i < 3; i++) vp_F(x, 21 + k)[i] = vp_F(x, 21 + k)[i] * 16.0f;
        for (int k = 0; k < 4; k++) vp_lq(x, 1 + k, 0x10u + k, VP_MALL);
        for (int k = 0; k < 4; k++)
            vp_rows(vp_F(x, 5 + k), vp_F(x, 1), vp_F(x, 2), vp_F(x, 3), vp_F(x, 4),
                    vp_F(x, 21 + k)[0], vp_F(x, 21 + k)[1], vp_F(x, 21 + k)[2],
                    vp_F(x, 21 + k)[3], VP_MALL);
        for (int k = 0; k < 4; k++) vp_lq(x, 28 + k, 0x14u + k, VP_MALL);
        for (int k = 0; k < 4; k++)
            vp_rows(vp_F(x, 9 + k), vp_F(x, 28), vp_F(x, 29), vp_F(x, 30), vp_F(x, 31),
                    vp_F(x, 21 + k)[0], vp_F(x, 21 + k)[1], vp_F(x, 21 + k)[2],
                    vp_F(x, 21 + k)[3], VP_MALL);
        for (int k = 0; k < 4; k++) vp_sq(x, 0x30u + k, vp_F(x, 9 + k), VP_MALL);
        vp_lq(x, 1, 0x09u, VP_MALL);
        if ((vi5 & 1u) == 1u) {
            vp_lq(x, 2, 0x0Bu, VP_MALL);
            vp_lq(x, 3, 0x0Cu, VP_MALL);
        } else {
            vp_lq(x, 2, 0x0Au, VP_MALL);
            vp_lq(x, 3, (vi5 & 2u) == 2u ? 0x0Cu : 0x0Du, VP_MALL);
        }
        pkt_putf(vi8 - vi7, vp_F(x, 1)); vi8++;
        pkt_putf(vi8 - vi7, vp_F(x, 2)); vi8++;
        pkt_putf(vi8 - vi7, vp_F(x, 3)); vi8++;
        return vi8;
    }
    for (int k = 0; k < 4; k++) vp_lq(x, 21 + k, 0x28u + k, VP_MALL);
    for (int k = 0; k < 4; k++) vp_lq(x, 1 + k, 0x10u + k, VP_MALL);
    for (int k = 0; k < 4; k++)
        vp_rows(vp_F(x, 5 + k), vp_F(x, 1), vp_F(x, 2), vp_F(x, 3), vp_F(x, 4),
                vp_F(x, 21 + k)[0], vp_F(x, 21 + k)[1], vp_F(x, 21 + k)[2],
                vp_F(x, 21 + k)[3], VP_MALL);
    for (int k = 0; k < 4; k++) vp_lq(x, 1 + k, 0x14u + k, VP_MALL);
    for (int k = 0; k < 4; k++)
        vp_rows(vp_F(x, 9 + k), vp_F(x, 1), vp_F(x, 2), vp_F(x, 3), vp_F(x, 4),
                vp_F(x, 21 + k)[0], vp_F(x, 21 + k)[1], vp_F(x, 21 + k)[2],
                vp_F(x, 21 + k)[3], VP_MALL);
    for (int k = 0; k < 4; k++) vp_lq(x, 1 + k, 0x20u + k, VP_MALL);
    for (int k = 0; k < 4; k++) vp_sq(x, 0x30u + k, vp_F(x, 9 + k), VP_MALL);
    for (int k = 0; k < 4; k++)
        vp_rows(vp_F(x, 17 + k), vp_F(x, 1), vp_F(x, 2), vp_F(x, 3), vp_F(x, 4),
                vp_F(x, 21 + k)[0], vp_F(x, 21 + k)[1], vp_F(x, 21 + k)[2],
                vp_F(x, 21 + k)[3], VP_MALL);
    vp_lq(x, 1, 0x07u, VP_MALL);
    if ((vi5 & 0x800u) == 0x800u) {
        vp_lq(x, 3, (vi5 & 2u) == 2u ? 0x0Bu : 0x0Cu, VP_MALL);
        pkt_putf(vi8 - vi7, vp_F(x, 1)); vi8++;
        pkt_putf(vi8 - vi7, vp_F(x, 3)); vi8++;
    }
    if ((vi5 & 0x1000u) == 0x1000u) {
        vp_lq(x, 4, 0x0Au, VP_MALL);
        vp_F(x, 20)[2] = 1.03f;
    } else {
        vp_lq(x, 4, 0x09u, VP_MALL);
        vp_F(x, 20)[2] = 1.0f;
    }
    pkt_putf(vi8 - vi7, vp_F(x, 1)); vi8++;
    pkt_putf(vi8 - vi7, vp_F(x, 4)); vi8++;
    return vi8;
}

static void convert_records(vpx *x, u32 t, u16 count) {
    u32 a = t + 1u;
    u16 n = count;
    if ((s16)vp_ilw(x, t, 1) > 0) return;
    do {
        float f[4];
        vp_lq(x, 14, a + 1u, VP_MXYZ);
        vp_lq(x, 15, a + 2u, VP_MALL);
        memcpy(f, vp_F(x, 14), 16);
        for (int i = 0; i < 3; i++) vp_F(x, 14)[i] = vp_lane_ftoi(f[i], 12);
        memcpy(f, vp_F(x, 15), 16);
        for (int i = 0; i < 4; i++) vp_F(x, 15)[i] = vp_lane_ftoi(f[i], 0);
        vp_sq(x, a + 1u, vp_F(x, 14), VP_MXYZ);
        vp_sq(x, a + 2u, vp_F(x, 15), VP_MALL);
        n--;
        a += 5u;
    } while (n != 0);
}

static int culled(vpx *x, u16 flags, u16 k, const float *a, const float *b, const float *c) {
    float e1x = a[0] - b[0], e1y = a[1] - b[1], e2x = b[0] - c[0], e2y = b[1] - c[1];
    float cz = e1x * e2y - e2x * e1y;
    u16 par = (u16)(vp_ilw(x, 0x0F, 3) + k);
    if ((flags & 0x80u) == 0x80u) { if (!(par & 1u)) cz = cz * -1.0f; }
    else { if (par & 1u) cz = cz * -1.0f; }
    return !signbit(cz);
}

static s32 fog_lane(const float *xyz, int adc) {
    s32 f = vp_ftoi(xyz[3], 4);
    return adc ? (s32)(s16)(u16)((u16)f + 0x8000u) : f;
}

static float near_d(const model_setup *s, const float *p) {
    float c[4];
    vp_rows(c, s->C[0], s->C[1], s->C[2], s->C[3], p[0], p[1], p[2], 1.0f, VP_MALL);
    return c[2] + c[3];
}

static void lerp_in(const mv_in *a, const mv_in *b, float t, mv_in *o) {
    for (int i = 0; i < 4; i++) {
        o->p[i] = a->p[i] + (b->p[i] - a->p[i]) * t;
        o->uv[i] = a->uv[i] + (b->uv[i] - a->uv[i]) * t;
    }
    for (int i = 0; i < 4; i++) {
        float na = vp_lane_itof(a->n[i], 0), nb = vp_lane_itof(b->n[i], 0);
        float ca = vp_lane_itof(a->col[i], 0), cb = vp_lane_itof(b->col[i], 0);
        o->n[i] = vp_lane_ftoi(na + (nb - na) * t, 0);
        o->col[i] = vp_lane_ftoi(ca + (cb - ca) * t + 0.5f, 0);
    }
}

static void put_list_tag(vpx *x, u32 idx, u32 tmpl, u32 prim, u32 nverts) {
    u32 w[4];
    u64 lo;
    memcpy(w, vp_q(x, tmpl), 16);
    lo = (u64)w[0] | ((u64)w[1] << 32);
    lo &= ~0x7FFFull;
    lo |= nverts & 0x7FFFu;
    lo |= 1ull << 46;
    lo = (lo & ~(0x7FFull << 47)) | ((u64)((prim & ~7u) | 3u) << 47);
    w[0] = (u32)lo;
    w[1] = (u32)(lo >> 32);
    pkt_put(idx, w);
}

#define VMAX 4096u

static u64 st_mesh_batches, st_mesh_room, st_mesh_lost;

static int mesh_room(u32 count, u32 draws) {
    if (ps2_vk_mesh_room(count, vp_tri_index_bound(count), draws)) return 1;
    st_mesh_room++;
    return 0;
}

static void rows_into(float *dst, vpx *x, int first_reg, int n) {
    for (int k = 0; k < n; k++) memcpy(dst + 4 * k, vp_F(x, first_reg + k), 16);
}

static void submit_prefix(vpx *x, u32 tag_addr, u32 data_addr) {
    u32 i = pkt_len();
    pkt_putf(i, (vp_lq(x, 1, tag_addr, VP_MALL), vp_F(x, 1)));
    pkt_putf(i + 1u, (vp_lq(x, 2, data_addr, VP_MALL), vp_F(x, 2)));
    pkt_flush();
}

static int model_mesh(vpx *x, const model_setup *s, u32 rec0, int mode, int cull,
                      u32 prim, u32 env_prim) {
    static u8 recs[VMAX * 64u];
    static u32 idx[VMAX * 3u];
    u32 n = s->count, ni, vbase = 0, ifirst = 0;
    int stored;
    ps2_vk_mesh_block b;
    ps2_vk_state st;
    float fogc[4];

    vp_copy_records(x, rec0, 5u, n, recs);
    ni = vp_tri_indices(prim & 7u, n, cull, (u16)rec0, s->flags, idx);
    stored = ni && ps2_vk_mesh_store(recs, n, idx, ni, &vbase, &ifirst) == 0;

    rn_mesh_block_init(&b);
    rows_into(b.m, x, 5, 4);
    memcpy(b.c, s->C, sizeof b.c);
    for (int k = 0; k < 3; k++) memcpy(b.l + 4 * k, vp_F(x, 17 + k), 12);
    if (mode != MODE_PLAIN) rows_into(b.r, x, 21, 4);
    for (int k = 0; k < 3; k++) memcpy(b.e + 4 * k, vp_F(x, 9 + k), 12);
    vp_rd(x, 0x18u, fogc);
    b.fog0 = fogc[0];
    b.fog1 = fogc[1];
    b.zscale = aircraft ? 16.0f : vp_F(x, 20)[2];
    b.spec_alpha = (float)(vp_ftoi(vp_rd_lane(x, 0x08u, 3), 0) & 0xFF);
    b.mode = mode == MODE_PLAIN ? 0u : 1u;
    b.clip = (s->flags & 0x20u) ? 1u : 0u;
    if (!aircraft) b.pad4 = 1u;          /* a ground object (buildings, vehicles, ships): cloud shadows on the ground */

    pkt_flush();
    submit_prefix(x, 0x07u, 0x19u);
    ps2_gs_write_reg(0x00, prim);
    if (stored) {
        ps2_gs_native_frame(&b.xoff, &b.yoff, &b.zmax);
        ps2_gs_native_state(&st);
        b.pass = 0;
        ps2_vk_draw_mesh(&st, &b, vbase, ifirst, ni, cull);
    }

    if (mode == MODE_ENV) {
        submit_prefix(x, 0x07u, 0x1Au);
        ps2_gs_write_reg(0x00, env_prim);
        if (stored) {
            ps2_gs_native_frame(&b.xoff, &b.yoff, &b.zmax);
            ps2_gs_native_state(&st);
            b.pass = 1;
            ps2_vk_draw_mesh(&st, &b, vbase, ifirst, ni, cull);
        }
    }
    return !ni || stored;
}

/* ---- The rear-view mirrors' live map (ps2_video.c mlv_*, ACZ_VR_COCKPIT_MIRRORS_LIVE; agent mir_live 2026-10-29,
   step M2 of docs/research/2026-10-24/mirrors-lookback.md) ----
   The game draws every aircraft every frame, also the ones behind its camera (no CPU test for the aircraft program):
   this path converts their batches anyway and drops the ones wholly outside the view (st_outside). While the cockpit's
   mirrors are in view (ps2_vk_mlv_cam_now: the renderer's flag, the cockpit view, a model with mirrors) each
   aircraft-program batch is also tested against an aircraft-fixed rear camera (the base camera turned 180 deg about
   up; its frustum the union of the mirrors' reflected cones and a margin for the head): first its model origin with a
   generous radius (40 m: the frontal ones end here), then the sphere of its vertex box. The own aircraft and the
   game's cockpit (a box that reaches the eye) are left out; so is everything beyond ACZ_VR_COCKPIT_MIRRORS_LIVE_RANGE.
   A batch inside goes into the frame's mirror list (ps2_vk_mlv_mesh: its own small arena, capped by _MAXBATCH) with
   its model -> screen rows turned into the rear camera's: m' = m S_head^-1 S_rear in the form the rows take (x = cx w
   + fx X, y = cy w - fy Y, z = zA w + zB, w = -Z: the view position follows from the x, y and w columns with the game
   frame's projection, zA / zB from the batch's own z column), a near plane at 1 m as its clip, the environment rows
   turned with the view; its draw state is the one the GS would have with the batch's setup and prefix registers
   (ps2_gs_native_state_ad: nothing is sent to the GS). No game call, nothing for the EE. */
static u64 st_mlv_seen, st_mlv_coarse, st_mlv_own, st_mlv_out, st_mlv_taken, st_mlv_full, st_mlv_odd, st_mlv_ground;
static u64 st_mlv_gtaken;               /* (ground objects' strips taken: ACZ_VR_COCKPIT_MIRRORS_LIVE_GROUND) */
static u64 st_mlv_tsc, st_mlv_tsc_taken;   /* (ACZ_DEBUG_COCKPIT_MIRRORS_LIVE: the worker's time in mlv_batch, TSC ticks;
                                              the batches taken) */
static int mlv_tdbg = -1;
int ps2_terrain_height(float x, float z, float *h);

static void mlv_view(const ps2_vk_mlv_cam *c, const float *row, float v[3]) {   /* head view of a row of m */
    v[0] = (row[0] - c->hc[0] * row[3]) / c->hf[0];
    v[1] = (c->hc[1] * row[3] - row[1]) / c->hf[1];
    v[2] = -row[3];
}
/* The aircraft program's view units are 16 game units (setup_commit scales its model -> view rows by 16 for the 16x
   depth; its stores on the own pylons read 16x too far without this), the model program's 1: the translation, the
   tests' distances and the near plane go by mlv_ku; the rows stay in the program's units (its depth and fog keep their
   scale). (The aircraft program draws the own aircraft and its stores; the other aircraft come through the model
   program, as the ground objects do.) */
static float mlv_ku = 16.0f;
#define MLV_KU mlv_ku
static void mlv_rot(const ps2_vk_mlv_cam *c, const float v[3], int origin, float o[3]) {   /* -> rear view */
    for (int i = 0; i < 3; i++)
        o[i] = c->T[3 * i] * v[0] + c->T[3 * i + 1] * v[1] + c->T[3 * i + 2] * v[2] + (origin ? c->t[i] * MLV_KU : 0.0f);
}
static int mlv_inside(const ps2_vk_mlv_cam *c, const float p[3], float r) {
    for (int k = 0; k < 5; k++)
        if (c->pl[k][0] * p[0] + c->pl[k][1] * p[1] + c->pl[k][2] * p[2] + c->pl[k][3] * MLV_KU < -r) return 0;
    return 1;
}
/* the A+D pairs of a GIF tag (PACKED, one A+D descriptor) and its data qwords */
static u32 mlv_ad(vpx *x, u32 tagq, const u32 *dq, u32 nd, u64 *ad, u32 n) {
    u64 tag[2];
    memcpy(tag, vp_q(x, tagq), 16);
    u32 nloop = (u32)(tag[0] & 0x7FFFull), flg = (u32)((tag[0] >> 58) & 3ull), nreg = (u32)((tag[0] >> 60) & 15ull);
    if (flg != 0u || nreg != 1u || (tag[1] & 15ull) != 14ull) { st_mlv_odd++; return n; }
    for (u32 i = 0; i < nloop && i < nd && n < 12u; i++) {
        memcpy(&ad[2u * n], vp_q(x, dq[i]), 16);
        n++;
    }
    return n;
}
static void mlv_batch(vpx *x, const model_setup *s, u32 rec0, int mode, int cull, u32 hdr) {
    const ps2_vk_mlv_cam *c = ps2_vk_mlv_cam_now();
    if (!c) return;
    mlv_ku = aircraft ? 16.0f : 1.0f;
    st_mlv_seen++;
    float M[16];
    for (int k = 0; k < 4; k++) memcpy(M + 4 * k, vp_F(x, 5 + k), 16);
    float o[3], ro[3], range = c->range;
    /* the object's verdict (own / out / ground / go on) by its origin, kept for the batches of the same model -> screen
       rows in this game frame (a model's strips come one after another: the game's cockpit ~1500 a frame) */
    static struct { float M[16]; u32 seq; int air, verdict, taken, ground; float o[3], ro[3], range; } om;
    int om_hit = 0;
    if (om.seq == c->seq && om.air == aircraft && !memcmp(om.M, M, sizeof M)) {
        om_hit = 1;
        switch (om.verdict) {
        case 1: st_mlv_own++; return;
        case 2: st_mlv_coarse++; return;
        case 3: st_mlv_ground++; return;
        default: break;
        }
        memcpy(o, om.o, sizeof o); memcpy(ro, om.ro, sizeof ro); range = om.range;
    } else {
        memcpy(om.M, M, sizeof M); om.seq = c->seq; om.air = aircraft;
        mlv_view(c, M + 12, o);
        om.verdict = 0; om.taken = 0; om.ground = 0;
        if (o[0] * o[0] + o[1] * o[1] + o[2] * o[2] < c->own * c->own * (MLV_KU * MLV_KU)) {   /* (the game's cockpit, */
            om.verdict = 1; st_mlv_own++;                                                      /* the own aircraft and */
            return;                                                                            /* its stores) */
        }
        mlv_rot(c, o, 1, ro);
        if (!c->identity && (ro[0] * ro[0] + ro[1] * ro[1] + ro[2] * ro[2]
                             > (c->range + c->rmax) * (c->range + c->rmax) * (MLV_KU * MLV_KU)
                             || !mlv_inside(c, ro, c->rmax * MLV_KU))) { om.verdict = 2; st_mlv_coarse++; return; }
        if (!aircraft && !c->identity) {                         /* a ground object (its origin near the ground)? */
            float w[3], h;
            for (int i = 0; i < 3; i++)
                w[i] = (c->wR[3 * i] * o[0] + c->wR[3 * i + 1] * o[1] + c->wR[3 * i + 2] * o[2]) / MLV_KU + c->we[i];
            if (ps2_terrain_height(w[0], w[2], &h) && w[1] - h < c->agl) {
                if (c->ground <= 0.0f || ro[0] * ro[0] + ro[1] * ro[1] + ro[2] * ro[2]
                                         > (c->ground + c->rmax) * (c->ground + c->rmax) * (MLV_KU * MLV_KU)) {
                    om.verdict = 3; st_mlv_ground++;
                    return;
                }
                range = c->ground;
                om.ground = 1;
            }
        }
        memcpy(om.o, o, sizeof o); memcpy(om.ro, ro, sizeof ro); om.range = range;
    }
    /* the batch's vertex box: its sphere in the head's view */
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    for (u32 k = 0; k < s->count; k++) {
        float p[4];
        vp_rd(x, rec0 + 5u * k, p);
        for (int j = 0; j < 3; j++) { if (p[j] < lo[j]) lo[j] = p[j]; if (p[j] > hi[j]) hi[j] = p[j]; }
    }
    float ax[3][3], vc[3] = { o[0], o[1], o[2] }, r2 = 0.0f;
    for (int i = 0; i < 3; i++) {
        mlv_view(c, M + 4 * i, ax[i]);
        float ce = 0.5f * (lo[i] + hi[i]), he = 0.5f * (hi[i] - lo[i]);
        for (int j = 0; j < 3; j++) vc[j] += ce * ax[i][j];
        r2 += he * he * (ax[i][0] * ax[i][0] + ax[i][1] * ax[i][1] + ax[i][2] * ax[i][2]);
    }
    float r = sqrtf(r2), dh = sqrtf(vc[0] * vc[0] + vc[1] * vc[1] + vc[2] * vc[2]);
    if (dh < r + c->own * MLV_KU) { st_mlv_own++; return; }    /* the own aircraft, its stores, the game's cockpit */
    float rc[3];
    mlv_rot(c, vc, 1, rc);
    {   /* ACZ_DEBUG_COCKPIT_MIRRORS_LIVE_TEST=N: every N-th batch past the own test (where it lies in the rear view) */
        static int dbg = -1; static u32 n;
        if (dbg < 0) { const char *e = getenv("ACZ_DEBUG_COCKPIT_MIRRORS_LIVE_TEST"); dbg = e ? atoi(e) : 0; }
        if (dbg > 0 && (n++ % (u32)dbg) == 0u) {
            float rl = sqrtf(rc[0] * rc[0] + rc[1] * rc[1] + rc[2] * rc[2]);
            ps2_log("mlv test: %s %u verts dist %.1f m r %.1f m az %+.1f el %+.1f (rear view; in %d) counts %llu seen %llu coarse "
                    "%llu own %llu out %llu taken", aircraft ? "aircraft" : "model", s->count, (double)(rl / MLV_KU / 6.0f), (double)(r / MLV_KU / 6.0f),
                    (double)(atan2f(-rc[0], -rc[2]) * 57.29578f), (double)(asinf(rc[1] / (rl > 0.0f ? rl : 1.0f)) * 57.29578f),
                    mlv_inside(c, rc, r), (unsigned long long)st_mlv_seen, (unsigned long long)st_mlv_coarse,
                    (unsigned long long)st_mlv_own, (unsigned long long)st_mlv_out, (unsigned long long)st_mlv_taken);
        }
    }
    if (!c->identity && (sqrtf(rc[0] * rc[0] + rc[1] * rc[1] + rc[2] * rc[2]) - r > range * MLV_KU
                         || !mlv_inside(c, rc, r))) {
        st_mlv_out++;
        return;
    }
    {   /* ACZ_DEBUG_COCKPIT_MIRRORS_LIVE_BATCH=N: every N-th batch taken (its size, place in the rear view, radius) */
        static int dbg = -1; static u32 n;
        if (dbg < 0) { const char *e = getenv("ACZ_DEBUG_COCKPIT_MIRRORS_LIVE_BATCH"); dbg = e ? atoi(e) : 0; }
        if (dbg > 0 && (n++ % (u32)dbg) == 0u) {
            float rl = sqrtf(rc[0] * rc[0] + rc[1] * rc[1] + rc[2] * rc[2]);
            ps2_log("mlv batch: %u verts mode %d flags %04X dist %.0f units (head %.0f) r %.1f az %+.1f el %+.1f | origin %.0f %.0f %.0f "
                    "| box %.2f..%.2f %.2f..%.2f %.2f..%.2f", s->count, mode, s->flags, (double)rl, (double)dh, (double)r,
                    (double)(atan2f(-rc[0], -rc[2]) * 57.29578f), (double)(asinf(rc[1] / (rl > 0.0f ? rl : 1.0f)) * 57.29578f),
                    (double)o[0], (double)o[1], (double)o[2], (double)lo[0], (double)hi[0], (double)lo[1], (double)hi[1],
                    (double)lo[2], (double)hi[2]);
        }
    }
    u32 prim = 0, env_prim = 0;
    if (s->count > VMAX || !vp_tag_prim(x, hdr, &prim) || !vp_prim_tris(prim) || (cull && (prim & 7u) == 5u)
        || (mode == MODE_ENV && !vp_tag_prim(x, 0x02u, &env_prim))) { st_mlv_odd++; return; }
    /* (the cap of batches a frame lets the object in progress finish: no aircraft cut in half; ground objects have a
       small cap of their own, so they never crowd out the aircraft) */
    static u32 gseq, gn;
    if (om.ground) {
        if (gseq != c->seq) { gseq = c->seq; gn = 0; }
        if (gn >= c->gmax && !(om_hit && om.taken)) { st_mlv_full++; return; }
    }
    if (!ps2_vk_mlv_room(s->count, vp_tri_index_bound(s->count), mode == MODE_ENV ? 2u : 1u, om_hit && om.taken)) {
        st_mlv_full++;
        return;
    }
    om.taken = 1;
    if (om.ground) { gn++; st_mlv_gtaken++; }
    static u8 recs[VMAX * 64u];
    static u32 idx[VMAX * 3u];
    vp_copy_records(x, rec0, 5u, s->count, recs);
    u32 ni = vp_tri_indices(prim & 7u, s->count, cull, (u16)rec0, s->flags, idx);
    if (!ni) return;
    /* the draw state: the GS now + this batch's setup registers (a changed setup; the batch's kick sends them) + its
       prefix (0x19; the environment pass 0x1A) + PRIM */
    u64 ad[24];
    u32 nad = 0;
    if (s->changed && aircraft) {
        u16 vi5 = x->vi[5];
        u32 dq[2] = { (vi5 & 1u) ? 0x0Bu : 0x0Au, (vi5 & 1u) ? 0x0Cu : ((vi5 & 2u) ? 0x0Cu : 0x0Du) };
        nad = mlv_ad(x, 0x09u, dq, 2u, ad, nad);
    } else if (s->changed) {                                      /* (the model program: one or two A+D writes) */
        u16 vi5 = x->vi[5];
        u32 dq = (vi5 & 2u) ? 0x0Bu : 0x0Cu;
        if (vi5 & 0x800u) nad = mlv_ad(x, 0x07u, &dq, 1u, ad, nad);
        dq = (vi5 & 0x1000u) ? 0x0Au : 0x09u;
        nad = mlv_ad(x, 0x07u, &dq, 1u, ad, nad);
    }
    {   u32 dq = 0x19u; nad = mlv_ad(x, 0x07u, &dq, 1u, ad, nad); }
    ps2_vk_mesh_block b;
    ps2_vk_state st;
    float fogc[4], zA, zB;
    /* the block of the last batch again when everything it is made of is the same (the strips of one model): its rows,
       lights, environment rows, fog, specular alpha, mode and program */
    static struct { float key[64]; u32 seq; int ok; ps2_vk_mesh_block b; } bk;
    float key[64];
    memcpy(key, M, 64);
    for (int k = 0; k < 3; k++) memcpy(key + 16 + 4 * k, vp_F(x, 17 + k), 16);
    for (int k = 0; k < 4; k++) vp_rd(x, 0x24u + k, key + 28 + 4 * k);
    for (int k = 0; k < 3; k++) vp_rd(x, 0x28u + k, key + 44 + 4 * k);
    vp_rd(x, 0x18u, key + 56);
    key[60] = vp_rd_lane(x, 0x08u, 3);
    key[61] = (float)mode; key[62] = (float)aircraft; key[63] = aircraft ? 16.0f : vp_F(x, 20)[2];
    int same = 0;
    const ps2_vk_mesh_block *bp = &b;
    if (bk.ok && bk.seq == c->seq && c->identity != 2 && !memcmp(bk.key, key, sizeof key)) {
        bp = &bk.b;
        same = 1;
        goto have_block;
    }
    memset(&b, 0, sizeof b);
    {   double sz = 0.0, sw = 0.0;
        for (int i = 0; i < 3; i++) { sz += (double)M[4 * i + 2] * M[4 * i + 3]; sw += (double)M[4 * i + 3] * M[4 * i + 3]; }
        if (!(sw > 1e-20)) { st_mlv_odd++; return; }
        zA = (float)(sz / sw);
        zB = M[14] - zA * M[15];
    }
    if (c->identity == 2) {                                        /* (test: the rows as they are) */
        memcpy(b.m, M, sizeof b.m);
        memcpy(b.c, s->C, sizeof b.c);
        b.clip = (s->flags & 0x20u) ? 1u : 0u;
    } else {
        for (int i = 0; i < 4; i++) {
            float v[3], q[3];
            mlv_view(c, M + 4 * i, v);
            mlv_rot(c, v, i == 3, q);
            float w = -q[2];
            b.m[4 * i] = c->rc[0] * w + c->rf[0] * q[0];
            b.m[4 * i + 1] = c->rc[1] * w - c->rf[1] * q[1];
            b.m[4 * i + 2] = zA * w + (i == 3 ? zB : 0.0f);
            b.m[4 * i + 3] = w;
            b.c[4 * i + 3] = w;                                   /* clip: w' - near >= 0 */
        }
        b.c[14] = -c->near_u * MLV_KU;
        b.clip = 1u;
        b.p[0] = zA; b.p[1] = zB;                                 /* (the program's depth, for the mirrors' terrain) */
        if (c->identity) { memcpy(b.c, s->C, sizeof b.c); b.clip = (s->flags & 0x20u) ? 1u : 0u; }
    }
    for (int k = 0; k < 3; k++) memcpy(b.l + 4 * k, vp_F(x, 17 + k), 12);
    if (mode != MODE_PLAIN) for (int k = 0; k < 4; k++) vp_rd(x, 0x24u + k, b.r + 4 * k);
    if (mode == MODE_ENV)
        for (int k = 0; k < 3; k++) {
            float e[4];
            vp_rd(x, 0x28u + k, e);
            mlv_rot(c, e, 0, b.e + 4 * k);                        /* the chrome's view space: the rear camera's */
        }
    vp_rd(x, 0x18u, fogc);
    b.fog0 = fogc[0];
    b.fog1 = fogc[1];
    b.zscale = aircraft ? 16.0f : vp_F(x, 20)[2];
    b.spec_alpha = (float)(vp_ftoi(vp_rd_lane(x, 0x08u, 3), 0) & 0xFF);
    b.mode = mode == MODE_PLAIN ? 0u : 1u;
    b.xr_wmin = 1.0f;
    memcpy(bk.key, key, sizeof key); bk.seq = c->seq; bk.b = b; bk.ok = 1;
have_block:;
    float xo, yo, zm;
    ps2_gs_native_state_ad(ad, nad, prim, &st, &xo, &yo, &zm);
    if (same && (bk.b.zmax != zm || (c->identity && (bk.b.xoff != xo || bk.b.yoff != yo)))) {
        b = bk.b; bp = &b; same = 0;
    }
    if (!same) {
        b.zmax = zm;
        b.xoff = c->identity ? xo : 0.0f;
        b.yoff = c->identity ? yo : 0.0f;
        b.pass = 0;
        bk.b = b;
    }
    ps2_vk_mlv_mesh(&st, bp, recs, s->count, idx, ni, cull, 0, same);
    if (mode == MODE_ENV) {
        u32 dq = 0x1Au;
        nad = mlv_ad(x, 0x07u, &dq, 1u, ad, nad);
        ps2_gs_native_state_ad(ad, nad, env_prim, &st, &xo, &yo, &zm);
        ps2_vk_mlv_mesh(&st, bp, recs, s->count, idx, ni, cull, 1, same);
    }
    st_mlv_taken++;
}

/* (ps2_video.c's debug line: the counters so far) */
void rn_vp_mlv_counts(u64 c[11]) {
    c[0] = st_mlv_seen; c[1] = st_mlv_coarse; c[2] = st_mlv_own; c[3] = st_mlv_out; c[4] = st_mlv_taken;
    c[5] = st_mlv_full; c[6] = st_mlv_odd; c[7] = st_mlv_ground; c[8] = st_mlv_tsc; c[9] = st_mlv_tsc_taken; c[10] = st_mlv_gtaken;
}

extern volatile int ps2_vk_mlv_want;
static void mlv_call(vpx *x, const model_setup *s, u32 rec0, int mode, int cull, u32 hdr) {
    static u64 ovh;                         /* (the timer's own cost: two TSC reads, calibrated once) */
    if (!ps2_vk_mlv_want) return;           /* (off, no model with mirrors, none in view: one test a batch) */
    if (mlv_tdbg < 0) {
        const char *e = getenv("ACZ_DEBUG_COCKPIT_MIRRORS_LIVE");
        mlv_tdbg = e && atoi(e) > 0;
        if (mlv_tdbg) {
            u64 best = ~0ull;
            for (int i = 0; i < 4096; i++) { u64 a = ps2_tsc(), b2 = ps2_tsc(); if (b2 - a < best) best = b2 - a; }
            ovh = best;
        }
    }
    if (!mlv_tdbg) { mlv_batch(x, s, rec0, mode, cull, hdr); return; }
    u64 t0 = ps2_tsc(), n0 = st_mlv_taken;
    mlv_batch(x, s, rec0, mode, cull, hdr);
    u64 dt = ps2_tsc() - t0;
    dt = dt > ovh ? dt - ovh : 0u;
    st_mlv_tsc += dt;
    if (st_mlv_taken != n0) st_mlv_tsc_taken += dt;
}

static int model_run(vpx *x, u32 pc, u32 *kick_at);

int rn_vp_model(vpx *x, u32 pc, u32 *kick_at) {
    aircraft = 0;
    return model_run(x, pc, kick_at);
}

int rn_vp_aircraft(vpx *x, u32 pc, u32 *kick_at) {
    aircraft = 1;
    return model_run(x, pc, kick_at);
}

static int model_run(vpx *x, u32 pc, u32 *kick_at) {
    model_setup s;
    u32 t, vi7, vi8, rec0, hdr;
    u16 all = 0x7FFF, any = 0;
    int mode, cull, partial = 0;
    u32 prim = 0, env_prim = 0;
    if (pc == 0x0000u) { x->tpc = 0x0010u; return 0; }
    if (pc != 0x0010u) { st_declined_other++; return -1; }

    t = x->top & VP_QW_MASK;
    setup_read(x, &s);
    if (!s.count) { st_declined_other++; return -1; }
    rec0 = t + 1u;
    if (s.flags & 0x20u) {
        for (u32 k = 0; k < s.count; k++) {
            float p[4], c[4];
            u16 code;
            vp_rd(x, rec0 + 5u * k, p);
            vp_rows(c, s.C[0], s.C[1], s.C[2], s.C[3], p[0], p[1], p[2], 1.0f, VP_MALL);
            code = vp_clip_code(c);
            all &= code;
            any |= code;
        }
    } else {
        all = 0;
    }
    mode = (s.flags & 2u) ? MODE_ENV : (s.flags & 8u) ? MODE_LIT : MODE_PLAIN;
    cull = (s.flags & 1u) != 0;
    st_mode[aircraft][mode][cull]++;
    hdr = cull ? 1u : 0u;
    if (x->native && !all && s.count <= VMAX && vp_mesh_wanted()
        && vp_tag_prim(x, hdr, &prim) && vp_prim_tris(prim)
        && (mode != MODE_ENV || vp_tag_prim(x, 0x02u, &env_prim))
        && !(cull && (prim & 7u) == 5u)
        && mesh_room(s.count, mode == MODE_ENV ? 2u : 1u)) {
        convert_records(x, t, s.count);
        vi7 = rec0 + 5u * s.count;
        (void)setup_commit(x, &s, vi7);
        if (s.flags & 0x20u) mlv_call(x, &s, rec0, mode, cull, hdr);   /* (the mirrors' live map) */
        x->vi[6] = s.count;
        x->vi[7] = (u16)vi7;
        x->vi[9] = s.count;
        vp_isw(x, 0x0F, 3, (u16)rec0);
        x->vi[14] = 0;
        if (mode != MODE_PLAIN)
            for (int k = 0; k < 4; k++) vp_lq(x, 21 + k, 0x24u + k, VP_MALL);
        if (mode == MODE_ENV)
            for (int k = 0; k < 4; k++) vp_lq(x, 9 + k, 0x28u + k, VP_MALL);
        if (model_mesh(x, &s, rec0, mode, cull, prim, env_prim)) st_mesh_batches++;
        else st_mesh_lost++;
        x->tpc = 0x0010u;
        (void)kick_at;
        return 0;
    }
    if (!all && any) {
        if (!x->native || s.count > VMAX || !vp_tag_prim(x, hdr, &prim)
            || !vp_prim_tris(prim)
            || (mode == MODE_ENV && !vp_tag_prim(x, 0x02u, &env_prim))) {
            st_declined_clip++;
            return -1;
        }
        partial = 1;
    }

    convert_records(x, t, s.count);
    vi7 = rec0 + 5u * s.count;
    vi8 = setup_commit(x, &s, vi7);
    if (s.flags & 0x20u) mlv_call(x, &s, rec0, mode, cull, hdr);   /* (the mirrors' live map) */
    x->vi[6] = s.count;
    x->vi[7] = (u16)vi7;

    if (all) {
        st_outside++;
    } else {
        u32 vi11 = 0, w[4];
        mv_in cur;
        mv_out out, prev[2];
        memset(prev, 0, sizeof prev);
        static u8 bad[VMAX];
        static u16 clip_k[VMAX];
        u32 nclip = 0;

        x->vi[9] = s.count;
        vp_isw(x, 0x0F, 3, (u16)rec0);
        x->vi[14] = 0;
        if (mode != MODE_PLAIN)
            for (int k = 0; k < 4; k++) vp_lq(x, 21 + k, 0x24u + k, VP_MALL);
        if (mode == MODE_ENV)
            for (int k = 0; k < 4; k++) vp_lq(x, 9 + k, 0x28u + k, VP_MALL);

        vp_lq(x, 1, 0x07u, VP_MALL);
        vp_lq(x, 2, 0x19u, VP_MALL);
        pkt_putf(vi8 - vi7, vp_F(x, 1)); vi8++;
        pkt_putf(vi8 - vi7, vp_F(x, 2)); vi8++;
        memcpy(w, vp_q(x, hdr), 16);
        w[0] = (u16)(vp_ilw(x, 0x00, 0) | s.count);
        pkt_put(vi8 - vi7, w); vi8++;
        if (mode == MODE_ENV) {
            vi11 = vi8 + 3u * s.count;
            vp_lq(x, 3, 0x1Au, VP_MALL);
            pkt_putf(vi11 - vi7, vp_F(x, 1)); vi11++;
            pkt_putf(vi11 - vi7, vp_F(x, 3)); vi11++;
            memcpy(w, vp_q(x, 0x02u), 16);
            w[0] = (u16)(vp_ilw(x, 0x02, 0) | s.count);
            pkt_put(vi11 - vi7, w); vi11++;
        }
        if (partial)
            for (u16 k = 0; k < s.count; k++) {
                float p[4];
                vp_rd(x, rec0 + 5u * k, p);
                bad[k] = !(near_d(&s, p) >= 0.0f);
            }

        for (u16 k = 0; k < s.count; k++) {
            int adc = 0;
            rd_in(x, rec0 + 5u * k, &cur);
            model_vertex(x, mode, &cur, &out);
            if (partial) {
                u32 idx[3];
                if (bad[k]) {
                    out.xyz[0] = out.xyz[1] = out.xyz[2] = 0.0f;
                }
                if (vp_tri_of(prim & 7u, k, idx)) {
                    int nb = bad[idx[0]] + bad[idx[1]] + bad[idx[2]];
                    if (nb) {
                        adc = 1;
                        if (nb < 3) clip_k[nclip++] = k;
                    }
                } else if (bad[k]) {
                    adc = 1;
                }
            }
            if (cull && k >= 2 && !adc)
                adc = culled(x, s.flags, k, prev[0].xyz, prev[1].xyz, out.xyz);
            prev[0] = prev[1];
            prev[1] = out;
            pkt_putf(vi8 - vi7, out.st); vi8++;
            pkt_putf(vi8 - vi7, out.rgba); vi8++;
            pkt_put_xyzf(vi8 - vi7, out.xyz, fog_lane(out.xyz, adc)); vi8++;
            if (mode == MODE_ENV) {
                pkt_putf(vi11 - vi7, out.est); vi11++;
                pkt_putf(vi11 - vi7, out.spec); vi11++;
                pkt_put_xyzf(vi11 - vi7, out.xyz, fog_lane(out.xyz, adc)); vi11++;
            }
        }
        if (mode == MODE_ENV) vi8 = vi11;

        if (!partial) {
            st_fast++;
        } else {
            static mv_out tri[VMAX * 6u];
            u32 ntri = 0;
            st_clipped++;
            for (u32 c = 0; c < nclip; c++) {
                u16 k = clip_k[c];
                u32 idx[3];
                mv_in v[3], poly[4];
                float d[3];
                int np = 0;
                vp_tri_of(prim & 7u, k, idx);
                for (int i = 0; i < 3; i++) {
                    rd_in(x, rec0 + 5u * idx[i], &v[i]);
                    d[i] = near_d(&s, v[i].p);
                }
                for (int i = 0; i < 3; i++) {
                    int j = (i + 1) % 3;
                    if (d[i] >= 0.0f && np < 4) poly[np++] = v[i];
                    if ((d[i] >= 0.0f) != (d[j] >= 0.0f) && np < 4)
                        lerp_in(&v[i], &v[j], d[i] / (d[i] - d[j]), &poly[np++]);
                }
                for (int i = 1; i + 1 < np && ntri < VMAX * 2u; i++) {
                    mv_out o3[3];
                    model_vertex(x, mode, &poly[0], &o3[0]);
                    model_vertex(x, mode, &poly[i], &o3[1]);
                    model_vertex(x, mode, &poly[i + 1], &o3[2]);
                    if (cull && culled(x, s.flags, k, o3[0].xyz, o3[1].xyz, o3[2].xyz))
                        continue;
                    tri[ntri * 3u] = o3[0];
                    tri[ntri * 3u + 1u] = o3[1];
                    tri[ntri * 3u + 2u] = o3[2];
                    ntri++;
                }
            }
            for (int pass = 0; ntri && pass < (mode == MODE_ENV ? 2 : 1); pass++) {
                put_list_tag(x, vi8 - vi7, pass == 0 ? hdr : 0x02u,
                             pass == 0 ? prim : env_prim, ntri * 3u);
                vi8++;
                for (u32 i = 0; i < ntri * 3u; i++) {
                    const mv_out *o = &tri[i];
                    pkt_putf(vi8 - vi7, pass == 0 ? o->st : o->est); vi8++;
                    pkt_putf(vi8 - vi7, pass == 0 ? o->rgba : o->spec); vi8++;
                    pkt_put_xyzf(vi8 - vi7, o->xyz, fog_lane(o->xyz, 0)); vi8++;
                }
            }
        }
    }

    vp_lq(x, 1, 0x06u, VP_MALL);
    pkt_putf(vi8 - vi7, vp_F(x, 1));
    *kick_at = vi7;
    x->tpc = 0x0010u;
    return 1;
}

static u64 st_part_batches, st_part_declined;

static void kick_stored(vpx *x, u32 addr) {
    u32 n = 0, a = addr, guard = 0;
    pkt_flush();
    while (guard++ < 16u) {
        u64 tag;
        u32 nloop, nreg, i;
        memcpy(&tag, vp_q(x, a), 8);
        nloop = (u32)(tag & 0x7FFFu);
        nreg = (u32)((tag >> 60) & 0xFu);
        if (!nreg) nreg = 16u;
        if (((tag >> 58) & 3u) != 0u || nloop * nreg > 256u) break;
        pkt_putf(n++, (vp_lq(x, 1, a, VP_MALL), vp_F(x, 1)));
        a++;
        for (i = 0; i < nloop * nreg; i++, a++)
            pkt_putf(n++, (vp_lq(x, 1, a, VP_MALL), vp_F(x, 1)));
        if ((tag >> 15) & 1u) break;
    }
    pkt_flush();
}

int rn_vp_part(vpx *x, u32 pc, u32 *kick_at) {
    static u8 recs[VMAX * 64u];
    static u32 idx[VMAX * 3u];
    u32 t, rec0, ni, vbase = 0, ifirst = 0, prim, env_prim = 0, hdr, env_hdr = 4u;
    u16 count, flags;
    int mode, cull, stored;
    ps2_vk_mesh_block b;
    ps2_vk_state st;
    float fogc[4];
    (void)kick_at;
    if (pc == 0x0000u) { x->tpc = 0x0010u; return 0; }
    if (pc != 0x0010u || !x->native || !vp_mesh_wanted()) { st_part_declined++; return -1; }
    t = x->top & VP_QW_MASK;
    count = vp_ilw(x, t, 0);
    if (!count || count > VMAX) { st_part_declined++; return -1; }
    {
        int changed = (s16)vp_ilw(x, 0x1B, 2) > 0;
        flags = changed ? vp_ilw(x, 0x1B, 0) : x->vi[5];
    }
    mode = (flags & 2u) ? MODE_ENV : (flags & 8u) ? MODE_LIT : MODE_PLAIN;
    cull = mode == MODE_ENV && (flags & 1u);
    hdr = mode == MODE_ENV ? ((flags & 1u) ? 1u : 0u) : ((flags & 1u) ? 3u : 2u);
    if (!vp_tag_prim(x, hdr, &prim) || (mode == MODE_ENV && !vp_tag_prim(x, env_hdr, &env_prim))
        || !vp_prim_tris(prim)
        || (cull && (prim & 7u) == 5u)
        || !mesh_room(count, mode == MODE_ENV ? 2u : 1u)) {
        st_part_declined++;
        return -1;
    }

    rec0 = t + 1u;
    convert_records(x, t, count);
    if ((s16)vp_ilw(x, 0x1B, 2) > 0) {
        u16 vi5;
        vp_isw(x, 0x1B, 2, 0);
        vi5 = vp_ilw(x, 0x1B, 0);
        x->vi[5] = vi5;
        for (int k = 0; k < 4; k++) vp_lq(x, 8 + k, 0x28u + k, VP_MALL);
        for (int k = 0; k < 4; k++) vp_lq(x, 4 + k, 0x10u + k, VP_MALL);
        for (int k = 0; k < 4; k++)
            vp_rows(vp_F(x, 12 + k), vp_F(x, 4), vp_F(x, 5), vp_F(x, 6), vp_F(x, 7),
                    vp_F(x, 8 + k)[0], vp_F(x, 8 + k)[1], vp_F(x, 8 + k)[2],
                    vp_F(x, 8 + k)[3], VP_MALL);
        for (int k = 0; k < 4; k++) vp_lq(x, 16 + k, 0x20u + k, VP_MALL);
        for (int k = 0; k < 4; k++)
            vp_rows(vp_F(x, 20 + k), vp_F(x, 16), vp_F(x, 17), vp_F(x, 18), vp_F(x, 19),
                    vp_F(x, 8 + k)[0], vp_F(x, 8 + k)[1], vp_F(x, 8 + k)[2],
                    vp_F(x, 8 + k)[3], VP_MALL);
        for (int k = 0; k < 4; k++) vp_lq(x, 24 + k, 0x24u + k, VP_MALL);
        kick_stored(x, (vi5 & 2u) ? 0x07u : 0x09u);
    }

    vp_copy_records(x, rec0, 5u, count, recs);
    ni = vp_tri_indices(prim & 7u, count, cull, 1u, flags, idx);
    x->tpc = 0x0010u;
    stored = ni && ps2_vk_mesh_store(recs, count, idx, ni, &vbase, &ifirst) == 0;
    if (ni && !stored) st_mesh_lost++;

    rn_mesh_block_init(&b);
    rows_into(b.m, x, 12, 4);
    for (int k = 0; k < 3; k++) memcpy(b.l + 4 * k, vp_F(x, 20 + k), 12);
    rows_into(b.r, x, 24, 4);
    for (int k = 0; k < 3; k++) memcpy(b.e + 4 * k, vp_F(x, 8 + k), 12);
    vp_rd(x, 0x18u, fogc);
    b.fog0 = fogc[0];
    b.fog1 = fogc[1];
    b.spec_alpha = (float)(vp_ftoi(vp_rd_lane(x, 0x06u, 3), 0) & 0xFF);
    b.mode = mode == MODE_PLAIN ? 0u : 1u;
    b.clip = 0u;
    b.pad4 = 2u;                         /* (the part program: shown by ACZ_DEBUG_GSHADOW_TINT only) */

    submit_prefix(x, 0x05u, 0x19u);
    ps2_gs_write_reg(0x00, prim);
    if (stored) {
        ps2_gs_native_frame(&b.xoff, &b.yoff, &b.zmax);
        ps2_gs_native_state(&st);
        b.pass = 0;
        ps2_vk_draw_mesh(&st, &b, vbase, ifirst, ni, cull);
    }
    if (mode == MODE_ENV) {
        submit_prefix(x, 0x05u, 0x1Au);
        ps2_gs_write_reg(0x00, env_prim);
        if (stored) {
            ps2_gs_native_frame(&b.xoff, &b.yoff, &b.zmax);
            ps2_gs_native_state(&st);
            b.pass = 1;
            ps2_vk_draw_mesh(&st, &b, vbase, ifirst, ni, cull);
        }
    }
    st_part_batches++;
    return 0;
}

static u64 st_detail_batches, st_detail_declined;

static void submit_qwords(vpx *x, const u32 *addrs, u32 n) {
    pkt_flush();
    for (u32 i = 0; i < n; i++) {
        u32 w[4];
        memcpy(w, vp_q(x, addrs[i]), 16);
        pkt_put(i, w);
    }
    pkt_flush();
}

int rn_vp_detail(vpx *x, u32 pc, u32 *kick_at) {
    static u8 recs[VMAX * 64u];
    static u32 idx[VMAX * 3u];
    u32 t, rec0, ni, vbase = 0, ifirst = 0, prim, dprim = 0;
    u16 count, flags;
    int stored;
    ps2_vk_mesh_block b;
    ps2_vk_state st;
    float fogc[4];
    (void)kick_at;
    if (pc == 0x0000u) { x->tpc = 0x0010u; return 0; }
    if (pc != 0x0010u || !x->native || !vp_mesh_wanted()) { st_detail_declined++; return -1; }
    t = x->top & VP_QW_MASK;
    count = vp_ilw(x, t, 0);
    flags = vp_ilw(x, 0x1B, 0);
    if (!count || count > VMAX || !vp_tag_prim(x, (flags & 1u) ? 1u : 0u, &prim)
        || !vp_prim_tris(prim)
        || ((flags & 0x4000u) && !vp_tag_prim(x, 0x0Au, &dprim))
        || !mesh_room(count, (flags & 0x4000u) ? 2u : 1u)) {
        st_detail_declined++;
        return -1;
    }

    x->tpc = 0x0010u;
    rec0 = t + 1u;
    convert_records(x, t, count);
    vp_isw(x, 0x1B, 2, 0);
    x->vi[5] = flags;
    for (int k = 0; k < 4; k++) vp_lq(x, 21 + k, 0x28u + k, VP_MALL);
    for (int k = 0; k < 4; k++) vp_lq(x, 1 + k, 0x10u + k, VP_MALL);
    for (int k = 0; k < 4; k++)
        vp_rows(vp_F(x, 5 + k), vp_F(x, 1), vp_F(x, 2), vp_F(x, 3), vp_F(x, 4),
                vp_F(x, 21 + k)[0], vp_F(x, 21 + k)[1], vp_F(x, 21 + k)[2],
                vp_F(x, 21 + k)[3], VP_MALL);
    for (int k = 0; k < 4; k++) vp_lq(x, 1 + k, 0x14u + k, VP_MALL);
    for (int k = 0; k < 4; k++)
        vp_rows(vp_F(x, 9 + k), vp_F(x, 1), vp_F(x, 2), vp_F(x, 3), vp_F(x, 4),
                vp_F(x, 21 + k)[0], vp_F(x, 21 + k)[1], vp_F(x, 21 + k)[2],
                vp_F(x, 21 + k)[3], VP_MALL);
    vp_F(x, 17)[2] = (flags & 0x1000u) ? 1.03f : 1.0f;

    vp_copy_records(x, rec0, 5u, count, recs);
    ni = vp_tri_indices(prim & 7u, count, 0, 0u, 0u, idx);
    stored = ni && ps2_vk_mesh_store(recs, count, idx, ni, &vbase, &ifirst) == 0;
    if (ni && !stored) st_mesh_lost++;

    rn_mesh_block_init(&b);
    rows_into(b.m, x, 5, 4);
    rows_into(b.c, x, 9, 4);
    vp_rd(x, 0x18u, fogc);
    b.fog0 = fogc[0];
    b.fog1 = fogc[1];
    b.zscale = (flags & 0x1000u) ? 1.03f : 1.0f;
    b.clip = (flags & 0x20u) ? 1u : 0u;
    b.prog = 2u;
    b.pad4 = 1u;                         /* a ground object: cloud shadows on the ground (mesh.vert) */
    vp_rd(x, 0x21u, b.p);

    {
        const u32 pre[6] = { 0x05u, (flags & 1u) ? 0x07u : 0x06u,
                             (flags & 0x1000u) ? 0x09u : 0x08u, 0x3Cu, 0x19u, 0x0Du };
        submit_qwords(x, pre, 6);
    }
    ps2_gs_write_reg(0x00, prim);
    if (stored) {
        ps2_gs_native_frame(&b.xoff, &b.yoff, &b.zmax);
        ps2_gs_native_state(&st);
        b.pass = 0;
        ps2_vk_draw_mesh(&st, &b, vbase, ifirst, ni, 0);
    }
    if (flags & 0x4000u) {
        const u32 dpre[3] = { 0x0Bu, 0x1Au, 0x1Eu };
        const u32 post[2] = { 0x0Eu, 0x19u };
        submit_qwords(x, dpre, 3);
        ps2_gs_write_reg(0x00, dprim);
        if (stored) {
            ps2_gs_native_frame(&b.xoff, &b.yoff, &b.zmax);
            ps2_gs_native_state(&st);
            b.pass = 1;
            ps2_vk_draw_mesh(&st, &b, vbase, ifirst, ni, 0);
        }
        submit_qwords(x, post, 2);
    }
    st_detail_batches++;
    return 0;
}

void rn_vp_model_report(void) {
    if (st_detail_batches || st_detail_declined)
        ps2_log("rn: native detailed-model program -- %llu batches drawn by mesh.vert, "
                "%llu left to VU1", (unsigned long long)st_detail_batches,
                (unsigned long long)st_detail_declined);
    if (st_part_batches || st_part_declined)
        ps2_log("rn: native part program -- %llu batches drawn by mesh.vert, %llu "
                "left to VU1", (unsigned long long)st_part_batches,
                (unsigned long long)st_part_declined);
    if (st_mesh_batches || st_mesh_room || st_mesh_lost)
        ps2_log("rn: native meshes -- %llu model batches drawn by mesh.vert; %llu "
                "batches sent another way because the frame's mesh arena was full "
                "(the model program's CPU port, VU1 for the others); %llu with "
                "triangles not stored", (unsigned long long)st_mesh_batches,
                (unsigned long long)st_mesh_room, (unsigned long long)st_mesh_lost);
    if (st_fast | st_outside | st_clipped | st_declined_clip | st_declined_other)
        ps2_log("rn: native VU1 model -- %llu unclipped, %llu wholly outside, "
                "%llu clipped natively; left to VU1: %llu needing the clipper, "
                "%llu other", (unsigned long long)st_fast, (unsigned long long)st_outside,
                (unsigned long long)st_clipped, (unsigned long long)st_declined_clip,
                (unsigned long long)st_declined_other);
    for (int a = 0; a < 2; a++)
        ps2_log("rn: native VU1 %s paths -- plain %llu/%llu, lit %llu/%llu, "
                "environment %llu/%llu (unculled/culled)", a ? "aircraft" : "model",
                (unsigned long long)st_mode[a][0][0], (unsigned long long)st_mode[a][0][1],
                (unsigned long long)st_mode[a][1][0], (unsigned long long)st_mode[a][1][1],
                (unsigned long long)st_mode[a][2][0], (unsigned long long)st_mode[a][2][1]);
    if (st_mlv_seen)
        ps2_log("rn: mirrors' live map (ACZ_VR_COCKPIT_MIRRORS_LIVE) -- %llu aircraft batches tested: %llu out at the origin "
                "test, %llu the own aircraft / cockpit, %llu out of the rear camera or range, %llu taken, %llu not taken "
                "(the mirror arena full), %llu odd", (unsigned long long)st_mlv_seen, (unsigned long long)st_mlv_coarse,
                (unsigned long long)st_mlv_own, (unsigned long long)st_mlv_out, (unsigned long long)st_mlv_taken,
                (unsigned long long)st_mlv_full, (unsigned long long)st_mlv_odd);
}
