/* ps2_headvol.c -- the 3D cockpit's solid parts around the design eye, for the head fade (bug #156; see
   ps2_headvol.h and ps2_video.c hvf_*).

   The model's surfaces near the pilot (every part of the cockpit model: seat, headrest, panels, consoles, canopy and
   windscreen glass, the airframe skin round the tub -- not the runtime-placed hands, not parts a constant select hides,
   not parts the runtime leaves out) are marked in a grid of cells about the design eye (cell 2 cm, x -0.8..0.8,
   y -0.8..0.7, z -1.1..0.9 m: farther than any seated head moves), by sampling each triangle at half a cell. A Euclidean
   distance transform gives every cell its distance to the nearest marked cell; read trilinearly that is a continuous
   field, and a point counts as "in a surface" where it is nearer than 0.9 cell to a marked cell centre: a wall marked
   as a staircase of cells touching only at their corners stays closed (the middle of a cell's body diagonal is 0.87
   cell from both centres; at half a cell the march slipped through such gaps where it met the canopy at a grazing
   angle), and a surface is found 0.8-2.8 cm before the eye reaches it.

   The question asked each frame is the one a neck answers: can the eye be where it is without passing through the
   cockpit? The straight way from the reference point (the design eye: by construction in the cockpit's air) to the eye
   is marched in quarter cells; the first point in a surface is refined by bisection, and the depth is how far the eye
   lies beyond it (0 while the way is clear). Leaning back through the headrest, putting the head into the panel or out
   through the canopy all cross a surface on that way; turning the head, leaning forward or to the side, looking over
   the shoulder do not. The part of the way outside the grid counts as depth too (a cockpit without a wall there: no
   glass over an open tub). The depth grows continuously from 0 as the eye passes the surface, so a fade driven by it
   needs no smoothing in time and clears the moment the head comes back. */
#include "ps2_headvol.h"
#include "ps2_cpit4.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HV_X0 (-0.8f)
#define HV_X1 0.8f
#define HV_Y0 (-0.8f)
#define HV_Y1 0.7f
#define HV_Z0 (-1.1f)
#define HV_Z1 0.9f
#define HV_HDR 84u
#define HV_VTX 36u

struct ps2_hv {
    int nx, ny, nz;
    float cell, th, org[3];        /* org: the centre of cell (0, 0, 0) */
    float *dist;                   /* metres to the nearest marked cell centre */
    float ref[3];
};

static float hv_at(const ps2_hv *h, int x, int y, int z) {
    return h->dist[((size_t)z * (size_t)h->ny + (size_t)y) * (size_t)h->nx + (size_t)x];
}

float ps2_hv_dist(const ps2_hv *h, const float p[3]) {
    if (!h) return 1e9f;
    float g[3];
    int i[3];
    float f[3];
    int n[3] = { h->nx, h->ny, h->nz };
    for (int k = 0; k < 3; k++) {
        g[k] = (p[k] - h->org[k]) / h->cell;
        if (!(g[k] >= 0.0f && g[k] <= (float)(n[k] - 1))) return 1e9f;     /* outside the grid (NaN too) */
        i[k] = (int)g[k];
        if (i[k] >= n[k] - 1) i[k] = n[k] - 2;
        f[k] = g[k] - (float)i[k];
    }
    float c00 = hv_at(h, i[0], i[1], i[2]) * (1.0f - f[0]) + hv_at(h, i[0] + 1, i[1], i[2]) * f[0];
    float c10 = hv_at(h, i[0], i[1] + 1, i[2]) * (1.0f - f[0]) + hv_at(h, i[0] + 1, i[1] + 1, i[2]) * f[0];
    float c01 = hv_at(h, i[0], i[1], i[2] + 1) * (1.0f - f[0]) + hv_at(h, i[0] + 1, i[1], i[2] + 1) * f[0];
    float c11 = hv_at(h, i[0], i[1] + 1, i[2] + 1) * (1.0f - f[0]) + hv_at(h, i[0] + 1, i[1] + 1, i[2] + 1) * f[0];
    float c0 = c00 * (1.0f - f[1]) + c10 * f[1], c1 = c01 * (1.0f - f[1]) + c11 * f[1];
    return c0 * (1.0f - f[2]) + c1 * f[2];
}

/* 1D squared distance transform (Felzenszwalb & Huttenlocher) of f[0..n-1] into d; v, z scratch */
static void hv_edt1(const float *f, float *d, int n, int *v, float *z) {
    int k = 0;
    v[0] = 0; z[0] = -1e30f; z[1] = 1e30f;
    for (int q = 1; q < n; q++) {
        float s = ((f[q] + (float)q * q) - (f[v[k]] + (float)v[k] * v[k])) / (2.0f * (float)(q - v[k]));
        while (s <= z[k] && k > 0) {
            k--;
            s = ((f[q] + (float)q * q) - (f[v[k]] + (float)v[k] * v[k])) / (2.0f * (float)(q - v[k]));
        }
        k++;
        v[k] = q; z[k] = s; z[k + 1] = 1e30f;
    }
    k = 0;
    for (int q = 0; q < n; q++) {
        while (z[k + 1] < (float)q) k++;
        float dq = (float)(q - v[k]);
        d[q] = dq * dq + f[v[k]];
    }
}

static void hv_mark(unsigned char *m, const ps2_hv *h, const float a[3], const float b[3], const float c[3]) {
    float lo[3], hi[3];
    for (int k = 0; k < 3; k++) {
        lo[k] = fminf(a[k], fminf(b[k], c[k])); hi[k] = fmaxf(a[k], fmaxf(b[k], c[k]));
    }
    float half = 0.5f * h->cell;
    if (hi[0] < HV_X0 - half || lo[0] > HV_X1 + half || hi[1] < HV_Y0 - half || lo[1] > HV_Y1 + half
        || hi[2] < HV_Z0 - half || lo[2] > HV_Z1 + half) return;
    float e = 0.0f, ab[3], ac[3];
    for (int k = 0; k < 3; k++) { ab[k] = b[k] - a[k]; ac[k] = c[k] - a[k]; }
    float l1 = sqrtf(ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2]), l2 = sqrtf(ac[0] * ac[0] + ac[1] * ac[1] + ac[2] * ac[2]);
    float bc[3] = { c[0] - b[0], c[1] - b[1], c[2] - b[2] };
    float l3 = sqrtf(bc[0] * bc[0] + bc[1] * bc[1] + bc[2] * bc[2]);
    e = fmaxf(l1, fmaxf(l2, l3));
    if (!(e < 50.0f)) return;                                  /* (NaN or absurd) */
    int ns = (int)ceilf(e / half);
    if (ns < 1) ns = 1;
    if (ns > 600) ns = 600;
    for (int i = 0; i <= ns; i++)
        for (int j = 0; j <= ns - i; j++) {
            float u = (float)i / (float)ns, v = (float)j / (float)ns, p[3];
            for (int k = 0; k < 3; k++) p[k] = a[k] + ab[k] * u + ac[k] * v;
            int x = (int)floorf((p[0] - h->org[0]) / h->cell + 0.5f), y = (int)floorf((p[1] - h->org[1]) / h->cell + 0.5f),
                z = (int)floorf((p[2] - h->org[2]) / h->cell + 0.5f);
            if (x < 0 || y < 0 || z < 0 || x >= h->nx || y >= h->ny || z >= h->nz) continue;
            m[((size_t)z * (size_t)h->ny + (size_t)y) * (size_t)h->nx + (size_t)x] = 1;
        }
}

/* a select animation decided with every property at 0 (the converter's unmapped / constant terms read 0): 1 shown */
static int hv_select_shown(const unsigned char *d, size_t apos) {
    unsigned prop, nt;
    float c[3];
    memcpy(&prop, d + apos, 4); memcpy(c, d + apos + 4, 12); memcpy(&nt, d + apos + 36, 4);
    int any = 0, all = 1;
    for (unsigned t = 0; t < nt; t++) {
        float cf, val;
        memcpy(&cf, d + apos + 40 + (size_t)t * 8u, 4); memcpy(&val, d + apos + 44 + (size_t)t * 8u, 4);
        unsigned code = (unsigned)cf, op = code & 0xFFu;
        float x = 0.0f;
        int r = op == 1u ? x < val : op == 2u ? x <= val : op == 3u ? x > val : op == 4u ? x >= val
              : op == 5u ? x == val : op == 6u ? x != val : x != 0.0f;
        if ((code >> 16) & 1u) r = !r;
        any |= r; all &= r;
    }
    int on = c[0] != 0.0f ? any : all;
    if (c[1] != 0.0f) on = !on;
    (void)prop;
    return on;
}

ps2_hv *ps2_hv_build(const unsigned char *d, size_t n, size_t mesh_pos, unsigned nmesh, const unsigned char *skip,
                     float cell, char *info, size_t ninfo) {
    if (info && ninfo) info[0] = '\0';
    if (!d || n < 20) return NULL;
    if (!(cell >= 0.005f && cell <= 0.1f)) cell = 0.02f;
    ps2_hv *h = (ps2_hv *)calloc(1, sizeof *h);
    if (!h) return NULL;
    h->cell = cell; h->th = 0.9f * cell;
    h->nx = (int)ceilf((HV_X1 - HV_X0) / cell) + 1; h->ny = (int)ceilf((HV_Y1 - HV_Y0) / cell) + 1;
    h->nz = (int)ceilf((HV_Z1 - HV_Z0) / cell) + 1;
    h->org[0] = HV_X0; h->org[1] = HV_Y0; h->org[2] = HV_Z0;
    size_t nc = (size_t)h->nx * (size_t)h->ny * (size_t)h->nz;
    unsigned char *m = (unsigned char *)calloc(nc, 1);
    h->dist = (float *)malloc(nc * sizeof *h->dist);
    if (!m || !h->dist) { free(m); free(h->dist); free(h); return NULL; }
    /* the animations' offsets (select conditions are decided once, all properties 0) */
    unsigned ntex, nm2, nanim;
    memcpy(&ntex, d + 8, 4); memcpy(&nm2, d + 12, 4); memcpy(&nanim, d + 16, 4);
    size_t *apos = (size_t *)calloc(nanim ? nanim : 1u, sizeof *apos);
    {   size_t p = 20;
        for (unsigned i = 0; i < ntex && p + 8 <= n; i++) {
            uint32_t w, hh, fm, fl; size_t tx, by;              /* (a record tells its own format: ps2_cpit4.h) */
            if (!cpit4_tex_rec(d, n, p, &w, &hh, &fm, &fl, &tx, &by)) break;
            p = tx + by;
        }
        for (unsigned i = 0; apos && i < nanim && p + 40 <= n; i++) {
            unsigned nt; memcpy(&nt, d + p + 36, 4);
            apos[i] = p;
            p += 40u + (size_t)nt * 8u;
        }
    }
    unsigned used = 0, tris = 0, hidden = 0;
    size_t pos = mesh_pos;
    for (unsigned i = 0; i < nmesh && pos + HV_HDR <= n; i++) {
        unsigned flags, ntri, an[4];
        memcpy(&flags, d + pos + 52, 4); memcpy(an, d + pos + 56, 16); memcpy(&ntri, d + pos + 80, 4);
        size_t vp = pos + HV_HDR, bytes = (size_t)ntri * 3u * HV_VTX;
        pos = vp + bytes;
        if (pos > n) break;
        if (!ntri || (skip && skip[i]) || (flags & (16u | 32u))) continue;      /* (hands: placed at run time) */
        int shown = 1;
        for (int k = 0; k < 4 && apos && !((flags >> 8) & 255u); k++) {     /* (screens are drawn regardless) */
            if (an[k] == 0xFFFFFFFFu || an[k] >= nanim) continue;
            unsigned prop; memcpy(&prop, d + apos[an[k]], 4);
            if ((prop & 0x1000u) || (prop & 0x800u) || !(prop & 0x200u)) continue;
            if (!hv_select_shown(d, apos[an[k]])) { shown = 0; break; }
        }
        if (!shown) { hidden++; continue; }
        used++;
        for (unsigned t = 0; t < ntri; t++) {
            float q[3][3];
            for (int j = 0; j < 3; j++) memcpy(q[j], d + vp + ((size_t)t * 3u + (size_t)j) * HV_VTX, 12);
            hv_mark(m, h, q[0], q[1], q[2]);
        }
        tris += ntri;
    }
    free(apos);
    /* squared distances in cells, axis by axis */
    int nmax = h->nx > h->ny ? h->nx : h->ny;
    if (h->nz > nmax) nmax = h->nz;
    float *f = (float *)malloc((size_t)nmax * sizeof *f), *o = (float *)malloc((size_t)nmax * sizeof *o),
          *z = (float *)malloc((size_t)(nmax + 1) * sizeof *z);
    int *v = (int *)malloc((size_t)nmax * sizeof *v);
    size_t nsolid = 0;
    if (!f || !o || !z || !v) { free(f); free(o); free(z); free(v); free(m); free(h->dist); free(h); return NULL; }
    for (size_t c = 0; c < nc; c++) { h->dist[c] = m[c] ? 0.0f : 1e20f; nsolid += m[c]; }
    free(m);
    for (int zz = 0; zz < h->nz; zz++)
        for (int y = 0; y < h->ny; y++) {
            float *row = h->dist + ((size_t)zz * (size_t)h->ny + (size_t)y) * (size_t)h->nx;
            memcpy(f, row, (size_t)h->nx * sizeof *f);
            hv_edt1(f, o, h->nx, v, z);
            memcpy(row, o, (size_t)h->nx * sizeof *o);
        }
    for (int zz = 0; zz < h->nz; zz++)
        for (int x = 0; x < h->nx; x++) {
            for (int y = 0; y < h->ny; y++) f[y] = h->dist[((size_t)zz * (size_t)h->ny + (size_t)y) * (size_t)h->nx + (size_t)x];
            hv_edt1(f, o, h->ny, v, z);
            for (int y = 0; y < h->ny; y++) h->dist[((size_t)zz * (size_t)h->ny + (size_t)y) * (size_t)h->nx + (size_t)x] = o[y];
        }
    for (int y = 0; y < h->ny; y++)
        for (int x = 0; x < h->nx; x++) {
            for (int zz = 0; zz < h->nz; zz++) f[zz] = h->dist[((size_t)zz * (size_t)h->ny + (size_t)y) * (size_t)h->nx + (size_t)x];
            hv_edt1(f, o, h->nz, v, z);
            for (int zz = 0; zz < h->nz; zz++)
                h->dist[((size_t)zz * (size_t)h->ny + (size_t)y) * (size_t)h->nx + (size_t)x] = sqrtf(o[zz]) * cell;
        }
    free(f); free(o); free(z); free(v);
    /* the reference: the design eye, or the freest point within 6 cm of it */
    h->ref[0] = h->ref[1] = h->ref[2] = 0.0f;
    float best = ps2_hv_dist(h, h->ref);
    if (best < 2.0f * h->th) {
        for (int i = -3; i <= 3; i++)
            for (int j = -3; j <= 3; j++)
                for (int k = -3; k <= 3; k++) {
                    float p[3] = { 0.02f * (float)i, 0.02f * (float)j, 0.02f * (float)k };
                    if (p[0] * p[0] + p[1] * p[1] + p[2] * p[2] > 0.0037f) continue;
                    float s = ps2_hv_dist(h, p);
                    if (s > best) { best = s; memcpy(h->ref, p, sizeof p); }
                }
    }
    if (info && ninfo)
        snprintf(info, ninfo, "%u parts (%u triangles; %u hidden by a constant select), %llu of %llu cells on a surface "
                 "(%.0f mm), reference %.2f %.2f %.2f m (%.0f mm clear)", used, tris, hidden, (unsigned long long)nsolid,
                 (unsigned long long)nc, (double)(cell * 1000.0f), (double)h->ref[0], (double)h->ref[1],
                 (double)h->ref[2], (double)(best * 1000.0f));
    if (best < h->th) { ps2_hv_free(h); return NULL; }
    return h;
}

void ps2_hv_free(ps2_hv *h) {
    if (!h) return;
    free(h->dist);
    free(h);
}

void ps2_hv_ref(const ps2_hv *h, float r[3]) {
    if (!h) { r[0] = r[1] = r[2] = 0.0f; return; }
    memcpy(r, h->ref, 3 * sizeof *r);
}

static int hv_inside(const ps2_hv *h, const float p[3]) {
    float m = h->cell;      /* one cell in from the grid's border */
    return p[0] >= HV_X0 + m && p[0] <= HV_X1 - m && p[1] >= HV_Y0 + m && p[1] <= HV_Y1 - m && p[2] >= HV_Z0 + m
        && p[2] <= HV_Z1 - m;
}

float ps2_hv_depth(const ps2_hv *h, const float p[3], float *clear) {
    if (clear) *clear = 0.0f;
    if (!h) return 0.0f;
    float d[3] = { p[0] - h->ref[0], p[1] - h->ref[1], p[2] - h->ref[2] };
    float L = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(L < 100.0f)) return 0.0f;                      /* (NaN) */
    if (L < 1e-4f) { if (clear) *clear = ps2_hv_dist(h, p) - h->th; return 0.0f; }
    float step = 0.25f * h->cell, t0 = 0.0f;
    int ns = (int)ceilf(L / step);
    for (int s = 1; s <= ns; s++) {
        float t = s == ns ? L : (float)s * step, x[3];
        for (int k = 0; k < 3; k++) x[k] = h->ref[k] + d[k] * (t / L);
        int out = !hv_inside(h, x);
        if (out || ps2_hv_dist(h, x) < h->th) {
            float a = t0, b = t;                        /* refine: the first point in a surface (or the grid's edge) */
            for (int it = 0; it < 8; it++) {
                float mt = 0.5f * (a + b), y[3];
                for (int k = 0; k < 3; k++) y[k] = h->ref[k] + d[k] * (mt / L);
                if (!hv_inside(h, y) || ps2_hv_dist(h, y) < h->th) b = mt; else a = mt;
            }
            return L - b;
        }
        t0 = t;
    }
    if (clear) *clear = ps2_hv_dist(h, p) - h->th;
    return 0.0f;
}
