/* zero_props_city.h -- the city finder of zero_props.c (ACZ_TERRAIN_CITY; wish #143, with #49 / #101): plain C on one
   64 x 64 tile of the ground photo, no game state; tools/props/citytest.c runs it on a dumped map.

   The city photos (mission 6's Directus, 6.25 m per texel) show their houses as small sunlit roofs (1-3 texels wide,
   2-8 long, in rows along the streets) separated by dark streets, courtyards and shadows. Two steps:
     1. built-up texels: the 9 x 9 gradient energy of the luminance is high (a city: > ZPC_EN; forest 38-43, fields 44,
        residential 58-98 in M6) and the texels brighter than their surroundings are light (their 9 x 9 mean
        > ZPC_BL: roofs 110-135, sunlit tree crowns 75-87) and at least ZPC_DARK of the 9 x 9 texels are dark (streets,
        courtyards, shadows: residential 22-35 %, autumn forest edges 13-15 %); no water; a 9 x 9 majority smooths it;
     2. roofs: built-up texels ZPC_RD above their 7 x 7 mean and brighter than ZPC_RL form 4-connected blobs of
        ZPC_MINN..ZPC_BIG texels, ZPC_CONTRAST above their surroundings; thin long blobs are streets; a blob longer than
        ZPC_MAXLEN texels or larger than ZPC_MAXN is split at the median of its long axis (rows of houses become
        separate buildings), recursively; each part becomes an oriented rectangle from its second moments (as the red
        roofs of stage 1), at least ZPC_HW texels half wide (the photo shows the sunlit half of a pitched roof) and
        ZPC_GROW wider each side.
   Per tile also the built-up fraction (for the district heights, zero_props.c) and up to 4 wall texels (calm, mid-light,
   warm or neutral built-up texels of the tile, 6 texels apart: the photo's own facade colours). */
#ifndef ZERO_PROPS_CITY_H
#define ZERO_PROPS_CITY_H
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define ZPC_EN 58.0f        /* built-up: 9 x 9 rms gradient (central differences) of the luminance above this */
#define ZPC_BL 102.0f       /* and the 9 x 9 mean luminance of its bright texels (10 above their 9 x 9 mean) above this */
#define ZPC_DARK 0.18f      /* and at least 18 % of its 9 x 9 texels dark (L < 40: streets, courtyards, shadows) */
#define ZPC_MAJ 0.5f        /* 9 x 9 majority of the built-up mask */
#define ZPC_RD 8.0f         /* roof texel: this much above its 7 x 7 mean */
#define ZPC_RL 70.0f        /* and brighter than this */
#define ZPC_MINN 2          /* a building: at least 2 texels */
#define ZPC_MAXN 20         /* parts of at most 20 texels */
#define ZPC_MAXLEN 6.0f     /* and at most 6 texels (37 m) long */
#define ZPC_BIG 48          /* blobs over 48 texels (1,900 m2) are plazas, rail yards, light roads: left flat */
#define ZPC_CONTRAST 14.0f  /* a blob's mean luminance this much above its 9 x 9 surroundings (a distinct roof) */
#define ZPC_HW 0.8f         /* half width at least 0.8 texels (10 m deep: the lit roof half and its shadowed one) */
#define ZPC_GROW 0.15f      /* + this each side */
#define ZPC_MAXB 1024

typedef struct {
    float cu, cv;           /* centre, tile texels */
    float hl, hw;           /* half length / half width, texels (>= 0.6) */
    float au, av;           /* unit axis of the length */
    unsigned short n;       /* roof texels */
    unsigned char wall;     /* which of the tile's wall texels (0..3) */
    unsigned char pad;
} zpc_bld;

typedef struct {
    int nb;
    float ufrac;            /* built-up fraction of the tile */
    float wall[4][2];       /* wall texels (texel centres); nwall of them */
    int nwall;
} zpc_tile_info;

static float zpc_lum(const unsigned char *p) { return 0.299f * (float)p[0] + 0.587f * (float)p[1] + 0.114f * (float)p[2]; }

/* (2r+1)^2 box mean over 64 x 64, edges clamped, separable */
static void zpc_boxm(const float *src, float *dst, int r) {
    float hs[4096];
    float k = 1.0f / (float)((2 * r + 1) * (2 * r + 1));
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            float s = 0.0f;
            for (int d = -r; d <= r; d++) { int xx = x + d < 0 ? 0 : x + d > 63 ? 63 : x + d; s += src[y * 64 + xx]; }
            hs[y * 64 + x] = s;
        }
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            float s = 0.0f;
            for (int d = -r; d <= r; d++) { int yy = y + d < 0 ? 0 : y + d > 63 ? 63 : y + d; s += hs[yy * 64 + x]; }
            dst[y * 64 + x] = s * k;
        }
}

/* one part (texel list) -> an oriented rectangle, or two halves split at the median of the long axis */
static void zpc_part(const unsigned short *ix, int n, zpc_bld *out, int *nb, int cap, unsigned seed, int depth) {
    if (n < ZPC_MINN || *nb >= cap) return;
    double su = 0, sv = 0, suu = 0, svv = 0, suv = 0;
    for (int i = 0; i < n; i++) {
        double u = (ix[i] & 63) + 0.5, v = (ix[i] >> 6) + 0.5;
        su += u; sv += v; suu += u * u; svv += v * v; suv += u * v;
    }
    double cu = su / n, cv = sv / n;
    double a = suu / n - cu * cu + 1.0 / 12.0, c = svv / n - cv * cv + 1.0 / 12.0, b = suv / n - cu * cv;
    double tr = a + c, det = a * c - b * b, disc = sqrt(fmax(tr * tr / 4.0 - det, 0.0));
    double l1 = tr / 2.0 + disc, l2 = tr / 2.0 - disc;
    double hl = sqrt(3.0 * fmax(l1, 1e-6)), hw = sqrt(3.0 * fmax(l2, 1e-6));
    double au = 1.0, av = 0.0;
    if (fabs(b) > 1e-9) { au = b; av = l1 - a; }
    else if (c > a) { au = 0.0; av = 1.0; }
    double al = sqrt(au * au + av * av);
    au /= al; av /= al;
    if ((n > ZPC_MAXN || 2.0 * hl > ZPC_MAXLEN) && n >= 2 * ZPC_MINN && depth < 8) {
        /* split at the median projection on the long axis */
        unsigned short lo[ZPC_MAXB], hi[ZPC_MAXB];
        float pr[ZPC_MAXB], s[ZPC_MAXB];
        int m = n > ZPC_MAXB ? ZPC_MAXB : n;
        for (int i = 0; i < m; i++) {
            pr[i] = (float)(((ix[i] & 63) + 0.5 - cu) * au + ((ix[i] >> 6) + 0.5 - cv) * av);
            s[i] = pr[i];
        }
        /* median by partial insertion sort (parts are small) */
        for (int i = 1; i < m; i++) { float t = s[i]; int j = i - 1; while (j >= 0 && s[j] > t) { s[j + 1] = s[j]; j--; } s[j + 1] = t; }
        float med = s[m / 2];
        int nl = 0, nh = 0;
        for (int i = 0; i < m; i++) { if (pr[i] < med) lo[nl++] = ix[i]; else hi[nh++] = ix[i]; }
        if (nl >= 1 && nh >= 1) {
            zpc_part(lo, nl, out, nb, cap, seed * 31u + 7u, depth + 1);
            zpc_part(hi, nh, out, nb, cap, seed * 31u + 13u, depth + 1);
            return;
        }
    }
    if (hl < 0.6) hl = 0.6;
    if (hw < ZPC_HW) hw = ZPC_HW;
    hl += ZPC_GROW; hw += ZPC_GROW;
    if (hw > hl) { double t = hw; hw = hl; hl = t; double q = au; au = -av; av = q; }
    zpc_bld *o = &out[(*nb)++];
    o->cu = (float)cu; o->cv = (float)cv; o->hl = (float)hl; o->hw = (float)hw;
    o->au = (float)au; o->av = (float)av; o->n = (unsigned short)n; o->wall = 0; o->pad = 0;
}

/* the city buildings of one tile (rgb: 64 x 64 x 3); returns the count */
static int zpc_tile(const unsigned char *rgb, zpc_bld *out, int cap, zpc_tile_info *info) {
    static float L[4096], m9[4096], m7[4096], E[4096], en[4096], br[4096], bL[4096], bm[4096], bLm[4096], u0[4096], um[4096], dk[4096], dm[4096];
    static unsigned char urb[4096], roof[4096], seen[4096];   /* (one finder thread) */
    for (int i = 0; i < 4096; i++) L[i] = zpc_lum(rgb + 3 * i);
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            int xa = x > 0 ? x - 1 : x, xb = x < 63 ? x + 1 : x, ya = y > 0 ? y - 1 : y, yb = y < 63 ? y + 1 : y;
            float gx = (x > 0 && x < 63) ? L[y * 64 + xb] - L[y * 64 + xa] : 0.0f;
            float gy = (y > 0 && y < 63) ? L[yb * 64 + x] - L[ya * 64 + x] : 0.0f;
            E[y * 64 + x] = gx * gx + gy * gy;
        }
    zpc_boxm(L, m9, 4);
    zpc_boxm(L, m7, 3);
    zpc_boxm(E, en, 4);
    for (int i = 0; i < 4096; i++) { br[i] = L[i] > m9[i] + 10.0f ? 1.0f : 0.0f; bL[i] = L[i] * br[i]; dk[i] = L[i] < 40.0f ? 1.0f : 0.0f; }
    zpc_boxm(br, bm, 4);
    zpc_boxm(bL, bLm, 4);
    zpc_boxm(dk, dm, 4);
    for (int i = 0; i < 4096; i++) {
        const unsigned char *p = rgb + 3 * i;
        int water = (int)p[2] > (int)p[0] + 5 && L[i] < 70.0f;
        float bl = bm[i] > 1e-3f ? bLm[i] / bm[i] : 0.0f;
        u0[i] = (sqrtf(en[i]) > ZPC_EN && bl > ZPC_BL && dm[i] > ZPC_DARK && !water) ? 1.0f : 0.0f;
    }
    zpc_boxm(u0, um, 4);
    int nu = 0;
    for (int i = 0; i < 4096; i++) {
        urb[i] = um[i] > ZPC_MAJ;
        nu += urb[i];
        roof[i] = urb[i] && L[i] > m7[i] + ZPC_RD && L[i] > ZPC_RL;
        seen[i] = 0;
    }
    info->ufrac = (float)nu / 4096.0f;
    /* wall texels: calm, light, unsaturated texels of the built-up area (the facades' colours), best 4 apart */
    {
        float best[4] = { -1e9f, -1e9f, -1e9f, -1e9f };
        int bi[4] = { -1, -1, -1, -1 };
        for (int y = 1; y < 63; y++)
            for (int x = 1; x < 63; x++) {
                int i = y * 64 + x;
                if (!urb[i]) continue;
                const unsigned char *p = rgb + 3 * i;
                float mx = (float)(p[0] > p[1] ? (p[0] > p[2] ? p[0] : p[2]) : (p[1] > p[2] ? p[1] : p[2]));
                float mn = (float)(p[0] < p[1] ? (p[0] < p[2] ? p[0] : p[2]) : (p[1] < p[2] ? p[1] : p[2]));
                float sat = (mx - mn) / (mx > 1.0f ? mx : 1.0f);
                float mm = 0.0f, vv = 0.0f;
                for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) mm += L[i + dy * 64 + dx];
                mm /= 9.0f;
                for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) { float d = L[i + dy * 64 + dx] - mm; vv += d * d; }
                float blue = (float)p[2] - (float)p[0];             /* warm or neutral facades, never bluish */
                float sc = -fabsf(L[i] - 120.0f) - 0.6f * sqrtf(vv / 9.0f) - 60.0f * sat - (blue > -8.0f ? 3.0f * (blue + 8.0f) : 0.0f);
                /* keep 4 best, at least 6 texels apart */
                int apart = 1, worst = 0;
                for (int k = 0; k < 4; k++) {
                    if (bi[k] >= 0 && abs((bi[k] & 63) - x) < 6 && abs((bi[k] >> 6) - y) < 6) {
                        apart = 0;
                        if (sc > best[k]) { best[k] = sc; bi[k] = i; }
                        break;
                    }
                    if (best[k] < best[worst]) worst = k;
                }
                if (apart && sc > best[worst]) { best[worst] = sc; bi[worst] = i; }
            }
        info->nwall = 0;
        for (int k = 0; k < 4; k++)
            if (bi[k] >= 0) {
                info->wall[info->nwall][0] = (float)(bi[k] & 63) + 0.5f;
                info->wall[info->nwall][1] = (float)(bi[k] >> 6) + 0.5f;
                info->nwall++;
            }
    }
    int nb = 0;
    static unsigned short stack[4096], comp[4096];
    for (int i0 = 0; i0 < 4096 && nb < cap; i0++) {
        if (!roof[i0] || seen[i0]) continue;
        int sp = 0, n = 0;
        stack[sp++] = (unsigned short)i0;
        seen[i0] = 1;
        while (sp) {
            int i = stack[--sp], x = i & 63, y = i >> 6;
            comp[n++] = (unsigned short)i;
            const int nbo[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
            for (int k = 0; k < 4; k++) {
                int xx = x + nbo[k][0], yy = y + nbo[k][1];
                if (xx < 0 || xx > 63 || yy < 0 || yy > 63) continue;
                int j = yy * 64 + xx;
                if (roof[j] && !seen[j]) { seen[j] = 1; stack[sp++] = (unsigned short)j; }
            }
        }
        if (n < ZPC_MINN || n > ZPC_BIG) continue;
        {   /* a distinct roof: brighter than its surroundings */
            float sl = 0.0f, sm = 0.0f;
            for (int i = 0; i < n; i++) { sl += L[comp[i]]; sm += m9[comp[i]]; }
            if ((sl - sm) / (float)n < ZPC_CONTRAST) continue;
        }
        {   /* streets: thin and long blobs (moments) */
            double su = 0, sv = 0, suu = 0, svv = 0, suv = 0;
            for (int i = 0; i < n; i++) { double u = (comp[i] & 63) + 0.5, v = (comp[i] >> 6) + 0.5; su += u; sv += v; suu += u * u; svv += v * v; suv += u * v; }
            double cu = su / n, cv = sv / n;
            double a = suu / n - cu * cu + 1.0 / 12.0, c = svv / n - cv * cv + 1.0 / 12.0, b = suv / n - cu * cv;
            double tr = a + c, det = a * c - b * b, disc = sqrt(fmax(tr * tr / 4.0 - det, 0.0));
            double hl = sqrt(3.0 * fmax(tr / 2.0 + disc, 1e-6)), hw = sqrt(3.0 * fmax(tr / 2.0 - disc, 1e-6));
            if (2.0 * hw < 1.2 && 2.0 * hl > 7.0) continue;
        }
        int b0 = nb;
        zpc_part(comp, n, out, &nb, cap, (unsigned)i0, 0);
        for (int k = b0; k < nb; k++) out[k].wall = (unsigned char)(info->nwall ? (unsigned)(k * 2654435761u >> 28) % (unsigned)info->nwall : 0);
    }
    info->nb = nb;
    return nb;
}
#endif
