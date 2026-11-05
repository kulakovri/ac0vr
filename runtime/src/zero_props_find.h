/* zero_props_find.h -- the desert / industrial building and tank finder of zero_props.c (ACZ_TERRAIN_PROPS=2, wish #78 /
   #55): plain C on one 64 x 64 tile of the ground photo, no game state. The reference is tools/props/shadowfind.py
   (same rules, same thresholds); tools/props/findtest.c runs this file on a dumped map for the parity check.

   The photo's buildings are boxes outlined by thin dark shadows on their sun-away sides (6.25 m per texel: a 12 x 25 m
   building is 2 x 4 texels, its shadow one texel wide). The finder works in a frame where the shadow's main direction
   is up (-v); the tile is flipped / transposed for the other three (zpf_dir: 0 up, 1 down, 2 left, 3 right, from the
   mission's sun). A building is a rectangle of roof texels [x0..x1] x [y0..y1] with
     - a top band (row y0-1) of shadow texels, each a valley: darker than the roof texel below (ZPF_TR) and than the
       ground one or two texels above (ZPF_TG); at most one texel at an end may miss it (the sun's minor component
       shifts the shadow by up to a texel along the side);
     - left / right sides: a side shadow band (an L with the top band) or a visible edge (ZPF_TEDGE) that is not dark;
     - a bottom edge (not dark); every non-shadow side and the ground beyond the shadow step the same way from the
       roof (a distinct roof, not sand between bushes);
     - band ends that stop with the roof and do not go on diagonally (not a road edge, a crack or a gully);
     - a calm roof (spread <= ZPF_ROOFSD) ZPF_TS above the band; deep shadow (band <= ZPF_DEEP x the ground beyond) or
       a medium one with a side band; open light ground around it (the 4-texel ring's 25th percentile and median at
       least ZPF_RING_P25 / ZPF_RING_MED x the map's land median luminance: not among dark bushes and rocks);
     - 2..16 texels each way; candidates from L corners (top + side band) and from top bands alone (the roof grows down
       while its rows keep its level); the strongest win, overlapping ones are dropped; a tile keeps its buildings
       only when it has at least ZPF_CLUSTER of them (a town or a works, not shrubs on sand).
   Tanks: light round blobs (L 30 above the 13 x 13 mean, saturation < 0.25, L > 150), 3.5..13 texels across, the fill
   and the empty corners of a disc, a dark crescent (60 below the tank) on the shadow side, open ground around. */
#ifndef ZERO_PROPS_FIND_H
#define ZERO_PROPS_FIND_H
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define ZPF_TR 22.0f
#define ZPF_TG 22.0f
#define ZPF_TE 18.0f
#define ZPF_TS 35.0f
#define ZPF_TROOF 22.0f
#define ZPF_TEDGE 16.0f
#define ZPF_ROOFSD 32.0f
#define ZPF_MINS 2
#define ZPF_MAXS 16
#define ZPF_DEEP 0.50f
#define ZPF_MEDIUM 0.65f
#define ZPF_CLUSTER 4
#define ZPF_RING_P25 0.58f
#define ZPF_RING_MED 0.74f
#define ZPF_MAXB 256
#define ZPF_MAXT 64

typedef struct { int x0, y0, x1, y1, k, ord; float f, score; } zpf_block;  /* tile texels, inclusive; k shadow texels */
typedef struct { float cu, cv, r, score; } zpf_tank;                      /* centre and radius, tile texels */

static float zpf_lum(const unsigned char *p) { return 0.299f * (float)p[0] + 0.587f * (float)p[1] + 0.114f * (float)p[2]; }

static int zpf_cmpf(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return x < y ? -1 : x > y; }
/* numpy's percentile (linear) of n sorted values */
static float zpf_pct(const float *s, int n, float p) {
    float idx = p / 100.0f * (float)(n - 1);
    int i = (int)floorf(idx);
    if (i >= n - 1) return s[n - 1];
    float t = idx - (float)i;
    return s[i] + (s[i + 1] - s[i]) * t;
}

/* the ring r texels around [x0..x1] x [y0..y1] (clipped to the tile): light open ground */
static int zpf_ring_ok(const float *L, int x0, int y0, int x1, int y1, int r, float lmap) {
    int ya = y0 - r < 0 ? 0 : y0 - r, yb = y1 + 1 + r > 64 ? 64 : y1 + 1 + r;
    int xa = x0 - r < 0 ? 0 : x0 - r, xb = x1 + 1 + r > 64 ? 64 : x1 + 1 + r;
    float v[1600];
    int n = 0;
    for (int y = ya; y < yb; y++)
        for (int x = xa; x < xb; x++) {
            if (y >= y0 && y <= y1 && x >= x0 && x <= x1) continue;
            if (n < 1600) v[n++] = L[y * 64 + x];
        }
    if (n < 8) return 0;
    qsort(v, (size_t)n, sizeof v[0], zpf_cmpf);
    return zpf_pct(v, n, 25.0f) >= ZPF_RING_P25 * lmap && zpf_pct(v, n, 50.0f) >= ZPF_RING_MED * lmap;
}

typedef struct {
    const float *L;        /* 64 x 64, canonical frame */
    signed char tk[64 * 64];
    float lmap;
} zpf_ctx;

static float zpf_l(const zpf_ctx *c, int x, int y) {          /* edge-clamped (numpy pad "edge") */
    x = x < 0 ? 0 : x > 63 ? 63 : x;
    y = y < 0 ? 0 : y > 63 ? 63 : y;
    return c->L[y * 64 + x];
}
static int zpf_topk(const zpf_ctx *c, int x, int y) {
    float s = zpf_l(c, x, y);
    if (s + ZPF_TR > zpf_l(c, x, y + 1)) return 0;
    if (s + ZPF_TG <= zpf_l(c, x, y - 1)) return 1;
    if (zpf_l(c, x, y - 1) + ZPF_TR <= zpf_l(c, x, y + 1) && s + ZPF_TG <= zpf_l(c, x, y - 2)
        && zpf_l(c, x, y - 1) + ZPF_TG <= zpf_l(c, x, y - 2)) return 2;
    return 0;
}
static int zpf_side(const zpf_ctx *c, int x, int y, int dx) {
    float s = zpf_l(c, x, y);
    return s + ZPF_TR <= zpf_l(c, x - dx, y) && (s + ZPF_TG <= zpf_l(c, x + dx, y) || s + ZPF_TG <= zpf_l(c, x + 2 * dx, y));
}
static int zpf_band_ok(const unsigned char *fl, int n) {
    int bad = 0, at = -1;
    for (int i = 0; i < n; i++) if (!fl[i]) { bad++; at = i; }
    return bad == 0 || (bad == 1 && n >= 2 && (at == 0 || at == n - 1) && n - 1 >= ZPF_MINS - 1);
}

/* the common tests of a roof [xa..xb] x [y..y1] under its top band (canonical frame); 1 = accepted */
static int zpf_validate(const zpf_ctx *c, int xa, int y, int xb, int y1, zpf_block *o) {
    int w = xb - xa + 1, h = y1 - y + 1;
    if (w < ZPF_MINS || h < ZPF_MINS || w > ZPF_MAXS || h > ZPF_MAXS || xa < 3 || xb > 60 || y < 3 || y1 > 60) return 0;
    /* side shadow bands; with one the top band reaches over the corner (the sun's minor component shifts it) */
    int sides[2];
    for (int s = 0; s < 2; s++) {
        int cx = s ? xb + 1 : xa - 1, dx = s ? 1 : -1;
        unsigned char sf[ZPF_MAXS + 2] = { 0 };
        for (int yy = y; yy <= y1; yy++) sf[yy - y] = (unsigned char)zpf_side(c, cx, yy, dx);
        sides[s] = sf[0] && zpf_band_ok(sf, h);
    }
    int bxa = xa - sides[0], bxb = xb + sides[1], nb = 0;
    unsigned char fl[ZPF_MAXS + 4];
    int nfl = 0, nin = 0, k = 0;
    float ls = 0.0f;
    for (int pass = 0; pass < 4; pass++) {                 /* the band over the corners, else over less */
        int el = (pass == 0 || pass == 1) ? sides[0] : 0, er = (pass == 0 || pass == 2) ? sides[1] : 0;
        bxa = xa - el; bxb = xb + er;
        nb = bxb - bxa + 1; nfl = 0; nin = 0; k = 0; ls = 0.0f;
        for (int x = bxa; x <= bxb; x++) {
            fl[x - bxa] = c->tk[(y - 1) * 64 + x] != 0;
            if (fl[x - bxa]) {
                nfl++; ls += c->L[(y - 1) * 64 + x];
                if (c->tk[(y - 1) * 64 + x] > k) k = c->tk[(y - 1) * 64 + x];
                if (x >= xa && x <= xb) nin++;
            }
        }
        if (zpf_band_ok(fl, nb) && nfl >= 1) break;
        if (pass == 3) return 0;
    }
    ls /= (float)nfl;
    float lr = 0.0f, mn = 1e9f, var = 0.0f;
    for (int yy = y; yy <= y1; yy++)
        for (int x = xa; x <= xb; x++) { float v = c->L[yy * 64 + x]; lr += v; if (v < mn) mn = v; }
    lr /= (float)(w * h);
    for (int yy = y; yy <= y1; yy++)
        for (int x = xa; x <= xb; x++) { float d = c->L[yy * 64 + x] - lr; var += d * d; }
    if (lr < ls + ZPF_TS || sqrtf(var / (float)(w * h)) > ZPF_ROOFSD || mn < ls + 0.5f * ZPF_TR) return 0;
    float bot = 0.0f;
    for (int x = xa; x <= xb; x++) bot += c->L[(y1 + 1) * 64 + x];
    bot /= (float)w;
    if (bot < ls + ZPF_TE) return 0;
    float steps[4];
    int ns = 0;
    steps[ns++] = bot - lr;
    for (int s = 0; s < 2; s++) {
        if (sides[s]) continue;
        int cx = s ? xb + 1 : xa - 1;
        float col = 0.0f;
        for (int yy = y; yy <= y1; yy++) col += c->L[yy * 64 + cx];
        col /= (float)h;
        if (col < ls + ZPF_TE) return 0;
        steps[ns++] = col - lr;
    }
    if (!(sides[0] || sides[1]) && nfl < ZPF_MINS) return 0;
    float lg = 0.0f;
    for (int x = bxa; x <= bxb; x++) if (fl[x - bxa]) lg += zpf_l(c, x, y - 1 - k);
    lg /= (float)nfl;
    steps[ns++] = lg - lr;
    int up = 1, dn = 1;
    float smin = 1e9f;
    for (int i = 0; i < ns; i++) {
        if (!(steps[i] >= ZPF_TEDGE)) up = 0;
        if (!(steps[i] <= -ZPF_TEDGE)) dn = 0;
        if (fabsf(steps[i]) < smin) smin = fabsf(steps[i]);
    }
    if (!up && !dn) return 0;
    float e0 = zpf_l(c, xa - 1, y - 1), e1 = zpf_l(c, xa - 2, y - 1), e2 = zpf_l(c, xb + 1, y - 1), e3 = zpf_l(c, xb + 2, y - 1);
    if (!sides[0] && (e0 > e1 ? e0 : e1) < ls + ZPF_TE) return 0;
    if (!sides[1] && (e2 > e3 ? e2 : e3) < ls + ZPF_TE) return 0;
    for (int s = 0; s < 2; s++) {
        int cx = s ? xb + 1 : xa - 1;
        if (!sides[s] && (zpf_l(c, cx, y - 2) < ls + ZPF_TE || zpf_l(c, cx, y) < ls + ZPF_TE)) return 0;
    }
    if (!zpf_ring_ok(c->L, xa, y - 1 - k, xb, y1, 4, c->lmap)) return 0;
    float ratio = ls / (lg > 1.0f ? lg : 1.0f);
    if (!(ratio <= ZPF_DEEP || (ratio <= ZPF_MEDIUM && (sides[0] || sides[1])))) return 0;
    float f = (lg - ls) / (0.6f * lg > 1.0f ? 0.6f * lg : 1.0f);
    f = f < 0.3f ? 0.3f : f > 1.0f ? 1.0f : f;
    float sc = lr - ls;
    if (lg - ls < sc) sc = lg - ls;
    if (smin + ZPF_TS < sc) sc = smin + ZPF_TS;
    sc += (sides[0] || sides[1]) ? 20.0f : 0.0f;
    sc -= 4.0f * (float)(w - nin);
    o->x0 = xa; o->y0 = y; o->x1 = xb; o->y1 = y1; o->k = k; o->f = f; o->score = sc;
    return 1;
}

static int zpf_trace_side(const zpf_ctx *c, int x, int y, int dx) {
    int y1 = y;
    while (y1 + 1 < 60 && zpf_side(c, x, y1 + 1, dx)) y1++;
    return y1;
}

static int zpf_cmp_block(const void *a, const void *b) {
    const zpf_block *x = (const zpf_block *)a, *y = (const zpf_block *)b;
    return x->score > y->score ? -1 : x->score < y->score ? 1 : x->ord - y->ord;   /* stable, as Python's sort */
}

/* buildings in the canonical frame (shadow up); returns the count (<= cap), strongest first, no overlaps */
static int zpf_blocks_canon(const float *L, float lmap, zpf_block *out, int cap) {
    static __thread zpf_ctx c;
    static __thread zpf_block cand[2048];
    int nc = 0;
    c.L = L; c.lmap = lmap;
    memset(c.tk, 0, sizeof c.tk);
    for (int y = 1; y < 62; y++)
        for (int x = 1; x < 63; x++) c.tk[y * 64 + x] = (signed char)zpf_topk(&c, x, y);
#define ZPF_ADD(xa_, y_, xb_, y1_) do { zpf_block b_; if (nc < 2048 && zpf_validate(&c, (xa_), (y_), (xb_), (y1_), &b_)) { b_.ord = nc; cand[nc++] = b_; } } while (0)
    for (int y = 3; y < 60; y++) {
        int x = 3;
        while (x < 61) {
            if (!c.tk[(y - 1) * 64 + x]) { x++; continue; }
            int xa = x;
            while (x < 61 && c.tk[(y - 1) * 64 + x]) x++;
            int xb0 = x - 1;
            for (int xs = xa; xs <= xb0 + 1; xs++) {             /* L corners with a left band */
                if (!zpf_side(&c, xs - 1, y, -1)) continue;
                int xe = xs;
                while (xe + 1 <= xb0 && !zpf_side(&c, xe + 1, y, -1)) xe++;
                int ye = zpf_trace_side(&c, xs - 1, y, -1);
                for (int i = 0; i < 2; i++) for (int y1 = y + 1; y1 <= ye + 1; y1++) ZPF_ADD(xs, y, xe + i, y1);
            }
            for (int e = 0; e < 2; e++) {                          /* L corners with a right band at the run's end */
                int xe = xb0 - e;
                if (zpf_side(&c, xe + 1, y, 1)) {
                    int ye = zpf_trace_side(&c, xe + 1, y, 1);
                    for (int i = 0; i < 2; i++) for (int y1 = y + 1; y1 <= ye + 1; y1++) ZPF_ADD(xa - i, y, xe, y1);
                }
            }
            for (int v = 0; v < 3; v++) {                          /* the band alone: the roof grows down */
                int x0 = v == 2 ? xa - 1 : xa, x1 = v == 1 ? xb0 + 1 : xb0, n = x1 - x0 + 1, rows = 1;
                float sum = 0.0f;
                for (int xx = x0; xx <= x1; xx++) sum += zpf_l(&c, xx, y);
                float lr = sum / (float)n;
                int y1 = y;
                while (y1 + 1 < 60 && y1 - y + 1 < ZPF_MAXS) {
                    float rs = 0.0f;
                    for (int xx = x0; xx <= x1; xx++) rs += zpf_l(&c, xx, y1 + 1);
                    if (fabsf(rs / (float)n - lr) > ZPF_TROOF) break;
                    sum += rs; rows++; y1++;
                    lr = sum / (float)(n * rows);
                }
                for (int yy = y + 1; yy <= y1; yy++) ZPF_ADD(x0, y, x1, yy);
            }
        }
    }
#undef ZPF_ADD
    qsort(cand, (size_t)nc, sizeof cand[0], zpf_cmp_block);
    static __thread unsigned char used[64 * 64];
    memset(used, 0, sizeof used);
    int n = 0;
    for (int i = 0; i < nc && n < cap; i++) {
        const zpf_block *b = &cand[i];
        int hit = 0;
        for (int y = b->y0; y <= b->y1 && !hit; y++) for (int x = b->x0; x <= b->x1; x++) if (used[y * 64 + x]) { hit = 1; break; }
        if (hit) continue;
        for (int y = b->y0; y <= b->y1; y++) for (int x = b->x0; x <= b->x1; x++) used[y * 64 + x] = 1;
        out[n++] = *b;
    }
    return n;
}

/* round light tanks in tile texels; (sx, sv) the shadow direction */
static int zpf_tanks(const unsigned char *rgb, const float *L, float sx, float sy, float lmap, zpf_tank *out, int cap) {
    static __thread float bg[4096], hs[4096];
    static __thread unsigned char light[4096], seen[4096];
    static __thread short stk[4096], px_[4096], py_[4096];
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            float s = 0.0f;
            for (int d = -6; d <= 6; d++) { int xx = x + d < 0 ? 0 : x + d > 63 ? 63 : x + d; s += L[y * 64 + xx]; }
            hs[y * 64 + x] = s;
        }
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            float s = 0.0f;
            for (int d = -6; d <= 6; d++) { int yy = y + d < 0 ? 0 : y + d > 63 ? 63 : y + d; s += hs[yy * 64 + x]; }
            bg[y * 64 + x] = s / 169.0f;
        }
    for (int i = 0; i < 4096; i++) {
        float r = rgb[3 * i], g = rgb[3 * i + 1], b = rgb[3 * i + 2];
        float mx = r > g ? (r > b ? r : b) : (g > b ? g : b), mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
        float sat = (mx - mn) / (mx > 1.0f ? mx : 1.0f);
        light[i] = L[i] > bg[i] + 30.0f && sat < 0.25f && L[i] > 150.0f;
        seen[i] = 0;
    }
    int n = 0;
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            int i0 = y * 64 + x;
            if (!light[i0] || seen[i0]) continue;
            int sp = 0, np = 0;
            stk[sp++] = (short)i0;
            seen[i0] = 1;
            while (sp) {
                int i = stk[--sp], cx = i & 63, cy = i >> 6;
                px_[np] = (short)cx; py_[np] = (short)cy; np++;
                const int nb[4][2] = { { 0, 1 }, { 0, -1 }, { 1, 0 }, { -1, 0 } };
                for (int k = 0; k < 4; k++) {
                    int nx = cx + nb[k][0], ny = cy + nb[k][1];
                    if (nx < 0 || nx > 63 || ny < 0 || ny > 63) continue;
                    int j = ny * 64 + nx;
                    if (light[j] && !seen[j]) { seen[j] = 1; stk[sp++] = (short)j; }
                }
            }
            if (np < 9 || np > 140) continue;
            int x0 = 63, x1 = 0, y0 = 63, y1 = 0;
            float su = 0.0f, sv = 0.0f, sl = 0.0f;
            for (int k = 0; k < np; k++) {
                if (px_[k] < x0) x0 = px_[k];
                if (px_[k] > x1) x1 = px_[k];
                if (py_[k] < y0) y0 = py_[k];
                if (py_[k] > y1) y1 = py_[k];
                su += (float)px_[k]; sv += (float)py_[k]; sl += L[py_[k] * 64 + px_[k]];
            }
            int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
            if (x0 < 2 || y0 < 2 || x1 > 61 || y1 > 61) continue;
            float asp = (float)bw / (float)bh;
            if (!(asp >= 0.75f && asp <= 1.33f) || !((float)(bw > bh ? bw : bh) >= 3.5f && (bw > bh ? bw : bh) <= 13)) continue;
            float fill = (float)np / (float)(bw * bh);
            if (!(fill >= 0.62f && fill <= 0.92f)) continue;
            int corners = light[y0 * 64 + x0] + light[y0 * 64 + x1] + light[y1 * 64 + x0] + light[y1 * 64 + x1];
            if (corners > 1) continue;
            float cu = su / (float)np + 0.5f, cv = sv / (float)np + 0.5f, r = 0.5f * (float)(bw + bh) / 2.0f;
            float dsum = 0.0f;
            int dn = 0;
            for (int a = 0; a < 7; a++) {
                float an = -0.9f + 0.3f * (float)a, ca = cosf(an), sa = sinf(an);
                float ux = sx * ca - sy * sa, uy = sx * sa + sy * ca;
                int qx = (int)floorf(cu + ux * (r + 0.7f)), qy = (int)floorf(cv + uy * (r + 0.7f));
                if (qx >= 0 && qx < 64 && qy >= 0 && qy < 64) { dsum += L[qy * 64 + qx]; dn++; }
            }
            float lin = sl / (float)np;
            if (dn == 0 || dsum / (float)dn > lin - 60.0f) continue;
            if (!zpf_ring_ok(L, x0, y0, x1, y1, 3, lmap)) continue;
            if (n < cap) { out[n].cu = cu; out[n].cv = cv; out[n].r = r; out[n].score = lin - dsum / (float)dn; n++; }
        }
    return n;
}

/* one tile (rgb 3 bytes per texel, 64 x 64, rows by v): buildings and tanks for shadow direction dir (0 up -v, 1 down
   +v, 2 left -u, 3 right +u); lmap = the map's land median luminance. Buildings in tile texels. */
static void zpf_tile(const unsigned char *rgb, int dir, float lmap, zpf_block *blocks, int *nb, zpf_tank *tanks, int *nt) {
    static __thread float L[4096], Lc[4096];
    for (int i = 0; i < 4096; i++) L[i] = zpf_lum(rgb + 3 * i);
    for (int y = 0; y < 64; y++)                                   /* canonical (x, y) <- tile (u, v) */
        for (int x = 0; x < 64; x++) {
            int u, v;
            switch (dir) {
            case 1: u = x; v = 63 - y; break;
            case 2: u = y; v = x; break;
            case 3: u = 63 - y; v = x; break;
            default: u = x; v = y; break;
            }
            Lc[y * 64 + x] = L[v * 64 + u];
        }
    static __thread zpf_block b[ZPF_MAXB];
    int n = zpf_blocks_canon(Lc, lmap, b, ZPF_MAXB);
    const float svec[4][2] = { { 0.0f, -1.0f }, { 0.0f, 1.0f }, { -1.0f, 0.0f }, { 1.0f, 0.0f } };
    *nt = zpf_tanks(rgb, L, svec[dir & 3][0], svec[dir & 3][1], lmap, tanks, ZPF_MAXT);
    int m = 0;
    for (int i = 0; i < n; i++) {
        zpf_block o = b[i];
        switch (dir) {
        case 1: o.y0 = 63 - b[i].y1; o.y1 = 63 - b[i].y0; break;
        case 2: o.x0 = b[i].y0; o.y0 = b[i].x0; o.x1 = b[i].y1; o.y1 = b[i].x1; break;
        case 3: o.x0 = 63 - b[i].y1; o.y0 = b[i].x0; o.x1 = 63 - b[i].y0; o.y1 = b[i].x1; break;
        default: break;
        }
        int in = 0;                                                 /* a tank wins over a block that overlaps it */
        for (int t = 0; t < *nt && !in; t++)
            in = (float)o.x0 - 1.0f <= tanks[t].cu && tanks[t].cu <= (float)o.x1 + 2.0f
                 && (float)o.y0 - 1.0f <= tanks[t].cv && tanks[t].cv <= (float)o.y1 + 2.0f;
        if (!in) blocks[m++] = o;
    }
    *nb = m >= ZPF_CLUSTER ? m : 0;
}

/* the map's land median luminance: every 7th texel of each tile used by land cells, weighted by min(cells, 50), in a
   histogram of whole levels; the median bin + 0.5 */
static inline float zpf_land_median(const unsigned char *const *tile_rgb, const unsigned *land_use, int ntiles) {
    double hist[256];
    memset(hist, 0, sizeof hist);
    double tot = 0.0;
    for (int t = 0; t < ntiles; t++) {
        if (!land_use[t]) continue;
        double w = land_use[t] > 50u ? 50.0 : (double)land_use[t];
        for (int k = 0; k < 4096; k += 7) {
            int b = (int)zpf_lum(tile_rgb[t] + 3 * k);
            hist[b < 0 ? 0 : b > 255 ? 255 : b] += w;
            tot += w;
        }
    }
    if (tot <= 0.0) return 190.0f;
    double acc = 0.0;
    for (int b = 0; b < 256; b++) { acc += hist[b]; if (acc >= 0.5 * tot) return (float)b + 0.5f; }
    return 190.0f;
}

#endif
