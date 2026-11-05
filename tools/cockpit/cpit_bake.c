/* cpit_bake.c -- bake per-vertex ambient occlusion into a cockpit file (.cpit format 3, tools/cockpit/ac2cpit.py).

   Usage: cpit_bake IN.cpit OUT.cpit [directions (192)] [map size (1024)] [radius m (0.35)] [sky share (0.25)]
   Build: gcc -O2 -o cpit_bake tools/cockpit/cpit_bake.c -lm -lpthread

   For each of N directions spread over the sphere the opaque parts (glass excluded) are rasterised into an
   orthographic map seen from that direction that keeps every surface's depth per texel (an A-buffer). For a vertex
   and a direction the nearest surface above it gives the occlusion: full at contact, fading to none at the radius
   (local ambient occlusion: creases, corners, the space under the glare shield), plus a share of plain sky visibility
   (deep parts of the tub get less sky). Both are cosine-weighted over the hemisphere of the vertex normal on the side
   facing the pilot's eye (the file's origin), so a panel is judged from the front even when the model is open behind
   it. 3x3 texels per lookup soften the result. Animated parts are baked in their rest pose; glass keeps 1.
   AO_PROBE=x,y,z prints how open that point is. AO_FREE=PREFIX[,PREFIX...]: meshes whose names start with one of
   these are left out like glass (no occlusion cast, AO 1): flat gauge layers and needles a few mm in front of a
   panel (generated gauge layers), which would otherwise darken the panel and their own faces. Meshes with flag 16
   (ac2cpit.py --hands: placed by the runtime, stored in their own frame) are always left out so. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdint.h>

typedef unsigned int u32;
enum { MESH_HDR = 84, VTX = 36 };

typedef struct { size_t data; u32 ntri, flags; } mesh;

static unsigned char *read_all(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *d = (unsigned char *)malloc((size_t)len);
    if (d && fread(d, 1, (size_t)len, f) != (size_t)len) { free(d); d = NULL; }
    fclose(f);
    *n = (size_t)len;
    return d;
}

static float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static int ndir, R, probe, nthr, done, nopen_dirs;
static float rmax, sky, rad, texel, sc, c[3], pr[3], pocc;
static size_t nocc, nvtx;
static float *tri, *vp, *vn, *vis, *wsum, *tvis[32], *tw[32];
static unsigned char *vglass;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static void *worker(void *arg);

/* rasterise the projected triangles: pass 0 counts the entries per texel, pass 1 stores their depths */
static void raster(const float *sp, size_t ntri, int R, u32 *cnt, const u32 *off, float *z, int pass) {
    for (size_t t = 0; t < ntri; t++) {
        const float *a = sp + t * 9, *b = a + 3, *c = a + 6;
        float area = (b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1]);
        if (fabsf(area) < 1e-12f) continue;
        int x0 = (int)floorf(fminf(a[0], fminf(b[0], c[0]))), x1 = (int)ceilf(fmaxf(a[0], fmaxf(b[0], c[0])));
        int y0 = (int)floorf(fminf(a[1], fminf(b[1], c[1]))), y1 = (int)ceilf(fmaxf(a[1], fmaxf(b[1], c[1])));
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > R - 1) x1 = R - 1;
        if (y1 > R - 1) y1 = R - 1;
        float ia = 1.0f / area;
        for (int yy = y0; yy <= y1; yy++)
            for (int xx = x0; xx <= x1; xx++) {
                float px = (float)xx + 0.5f, py = (float)yy + 0.5f;
                float w0 = ((b[0] - px) * (c[1] - py) - (c[0] - px) * (b[1] - py)) * ia;
                float w1 = ((c[0] - px) * (a[1] - py) - (a[0] - px) * (c[1] - py)) * ia;
                float w2 = 1.0f - w0 - w1;
                if (w0 < -0.02f || w1 < -0.02f || w2 < -0.02f) continue;     /* a little conservative */
                size_t i = (size_t)yy * R + xx;
                if (pass == 0) cnt[i]++;
                else z[off[i] + cnt[i]++] = w0 * a[2] + w1 * b[2] + w2 * c[2];
            }
    }
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: cpit_bake IN.cpit OUT.cpit [directions] [map size] [radius m] [sky share]\n");
        return 2;
    }
    ndir = argc > 3 ? atoi(argv[3]) : 192; R = argc > 4 ? atoi(argv[4]) : 1024;
    rmax = argc > 5 ? (float)atof(argv[5]) : 0.35f; sky = argc > 6 ? (float)atof(argv[6]) : 0.25f;
    size_t n;
    unsigned char *d = read_all(argv[1], &n);
    if (!d || n < 20 || memcmp(d, "ACZCPIT3", 8)) { fprintf(stderr, "%s: not a format 3 cockpit\n", argv[1]); return 1; }
    u32 ntex, nmesh, nanim;
    memcpy(&ntex, d + 8, 4); memcpy(&nmesh, d + 12, 4); memcpy(&nanim, d + 16, 4);
    size_t pos = 20;
    for (u32 i = 0; i < ntex; i++) { u32 w, h; memcpy(&w, d + pos, 4); memcpy(&h, d + pos + 4, 4); pos += 8 + (size_t)w * h * 4; }
    for (u32 i = 0; i < nanim; i++) { u32 nt; memcpy(&nt, d + pos + 36, 4); pos += 40 + (size_t)nt * 8; }
    mesh *ms = (mesh *)calloc(nmesh, sizeof *ms);
    nocc = 0; nvtx = 0;
    const char *free_pre = getenv("AO_FREE");
    for (u32 i = 0; i < nmesh; i++) {
        memcpy(&ms[i].flags, d + pos + 52, 4); memcpy(&ms[i].ntri, d + pos + 80, 4);
        if (ms[i].flags & 16u) ms[i].flags |= 0x80000000u;   /* placed at run time (ac2cpit.py --hands): as AO_FREE */
        if (free_pre && *free_pre) {                       /* AO_FREE: as glass for the bake (the file keeps its flags) */
            const char *q = free_pre;
            while (*q) {
                size_t l = strcspn(q, ",");
                if (l && l <= 32 && strncmp((const char *)d + pos, q, l) == 0) ms[i].flags |= 0x80000000u;
                q += l; if (*q == ',') q++;
            }
        }
        ms[i].data = pos + MESH_HDR;
        pos = ms[i].data + (size_t)ms[i].ntri * 3 * VTX;
        if (pos > n) { fprintf(stderr, "truncated at mesh %u\n", i); return 1; }
        if (!(ms[i].flags & 0x80000001u)) nocc += ms[i].ntri;
        nvtx += (size_t)ms[i].ntri * 3;
    }
    /* occluders, vertex positions and eye-facing normals */
    tri = (float *)malloc(nocc * 9 * sizeof *tri);
    vp = (float *)malloc(nvtx * 3 * sizeof *vp); vn = (float *)malloc(nvtx * 3 * sizeof *vn);
    vglass = (unsigned char *)malloc(nvtx);
    float lo[3] = { 1e9f, 1e9f, 1e9f }, hi[3] = { -1e9f, -1e9f, -1e9f };
    size_t k = 0, vi = 0;
    for (u32 i = 0; i < nmesh; i++)
        for (u32 t = 0; t < ms[i].ntri * 3; t++, vi++) {
            float *p = vp + vi * 3, *nr = vn + vi * 3;
            memcpy(p, d + ms[i].data + (size_t)t * VTX, 12);
            memcpy(nr, d + ms[i].data + (size_t)t * VTX + 12, 12);
            if (dot3(nr, p) > 0.0f) { nr[0] = -nr[0]; nr[1] = -nr[1]; nr[2] = -nr[2]; }   /* face the eye (origin) */
            vglass[vi] = (ms[i].flags & 0x80000001u) != 0u;
            if (vglass[vi]) continue;
            memcpy(tri + k * 3, p, 12);
            for (int j = 0; j < 3; j++) { if (p[j] < lo[j]) lo[j] = p[j]; if (p[j] > hi[j]) hi[j] = p[j]; }
            k++;
        }
    c[0] = (lo[0] + hi[0]) * 0.5f; c[1] = (lo[1] + hi[1]) * 0.5f; c[2] = (lo[2] + hi[2]) * 0.5f;
    rad = 0.5f * sqrtf((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) +
                             (hi[2] - lo[2]) * (hi[2] - lo[2])) * 1.01f;
    {   /* AO_RANGE (2.5 m): the maps cover that sphere around the eye; parts beyond get no occlusion */
        float range = getenv("AO_RANGE") ? (float)atof(getenv("AO_RANGE")) : 2.5f;
        if (range > 0.0f && range < rad) { c[0] = c[1] = c[2] = 0.0f; rad = range; }
    }
    texel = 2.0f * rad / (float)R; sc = (float)R / (2.0f * rad);
    nthr = getenv("NUMBER_OF_PROCESSORS") ? atoi(getenv("NUMBER_OF_PROCESSORS")) : 8;
    if (nthr < 1) nthr = 1;
    if (nthr > 32) nthr = 32;
    printf("%s: %u meshes, %zu occluding triangles, %zu vertices; radius %.2f m, %d directions, %dx%d maps (%.1f mm), "
           "AO radius %.2f m, sky share %.2f, %d threads\n", argv[1], nmesh, nocc, nvtx, rad, ndir, R, R,
           texel * 1000.0f, rmax, sky, nthr);
    probe = getenv("AO_PROBE") && sscanf(getenv("AO_PROBE"), "%f,%f,%f", &pr[0], &pr[1], &pr[2]) == 3;
    vis = (float *)calloc(nvtx, sizeof *vis); wsum = (float *)calloc(nvtx, sizeof *wsum);
    {   pthread_t th[32];
        for (long i = 0; i < nthr; i++) pthread_create(&th[i], NULL, worker, (void *)(intptr_t)i);
        for (int i = 0; i < nthr; i++) pthread_join(th[i], NULL);
        for (int i = 0; i < nthr; i++) {
            for (size_t v = 0; v < nvtx; v++) { vis[v] += tvis[i][v]; wsum[v] += tw[i][v]; }
            free(tvis[i]); free(tw[i]);
        }
    }
    if (probe)
        printf("probe %.2f %.2f %.2f: open sky towards %d of %d directions, local occlusion %.2f\n", pr[0], pr[1], pr[2],
               nopen_dirs, ndir, pocc / (float)ndir);
    double sum = 0.0;
    vi = 0;
    for (u32 i = 0; i < nmesh; i++)
        for (u32 t = 0; t < ms[i].ntri * 3; t++, vi++) {
            float ao = vglass[vi] || wsum[vi] <= 0.0f ? 1.0f : vis[vi] / wsum[vi];
            memcpy(d + ms[i].data + (size_t)t * VTX + 32, &ao, 4);
            sum += ao;
        }
    FILE *f = fopen(argv[2], "wb");
    if (!f || fwrite(d, 1, n, f) != n) { fprintf(stderr, "cannot write %s\n", argv[2]); return 1; }
    fclose(f);
    printf("%s: mean %.3f\n", argv[2], nvtx ? sum / (double)nvtx : 0.0);
    return 0;
}

static void *worker(void *arg) {
    long tid = (long)(intptr_t)arg;
    {
        float *lvis = (float *)calloc(nvtx, sizeof *lvis), *lw = (float *)calloc(nvtx, sizeof *lw);
        float *sp = (float *)malloc(nocc * 9 * sizeof *sp);
        u32 *cnt = (u32 *)malloc((size_t)R * R * sizeof *cnt), *off = (u32 *)malloc(((size_t)R * R + 1) * sizeof *off);
        float *zs = NULL;
        size_t zcap = 0;
        for (int di = (int)tid; di < ndir; di += nthr) {
            float y = 1.0f - 2.0f * ((float)di + 0.5f) / (float)ndir, r = sqrtf(1.0f - y * y);
            float ph = (float)di * 2.39996323f;                              /* Fibonacci sphere */
            float D[3] = { r * cosf(ph), y, r * sinf(ph) }, U[3], V[3];
            {   float a[3] = { fabsf(D[0]) < 0.9f ? 1.0f : 0.0f, fabsf(D[0]) < 0.9f ? 0.0f : 1.0f, 0.0f };
                U[0] = a[1] * D[2] - a[2] * D[1]; U[1] = a[2] * D[0] - a[0] * D[2]; U[2] = a[0] * D[1] - a[1] * D[0];
                float l = sqrtf(dot3(U, U)); U[0] /= l; U[1] /= l; U[2] /= l;
                V[0] = D[1] * U[2] - D[2] * U[1]; V[1] = D[2] * U[0] - D[0] * U[2]; V[2] = D[0] * U[1] - D[1] * U[0]; }
            for (size_t i = 0; i < nocc * 3; i++) {
                const float *p = tri + i * 3;
                float q[3] = { p[0] - c[0], p[1] - c[1], p[2] - c[2] };
                sp[i * 3] = dot3(q, U) * sc + 0.5f * (float)R; sp[i * 3 + 1] = dot3(q, V) * sc + 0.5f * (float)R;
                sp[i * 3 + 2] = dot3(q, D);
            }
            memset(cnt, 0, (size_t)R * R * sizeof *cnt);
            raster(sp, nocc, R, cnt, NULL, NULL, 0);
            off[0] = 0;
            for (size_t i = 0; i < (size_t)R * R; i++) off[i + 1] = off[i] + cnt[i];
            if (off[(size_t)R * R] > zcap) { zcap = off[(size_t)R * R]; free(zs); zs = (float *)malloc(zcap * sizeof *zs); }
            memset(cnt, 0, (size_t)R * R * sizeof *cnt);
            raster(sp, nocc, R, cnt, off, zs, 1);
            /* nearest surface above height z at texel (X, Y): its distance, or -1 (open sky) */
#define ABOVE(X, Y, z, out) do { \
                (out) = -1.0f; \
                if ((X) >= 0 && (Y) >= 0 && (X) < R && (Y) < R) { \
                    size_t ti_ = (size_t)(Y) * R + (X); \
                    for (u32 e_ = off[ti_]; e_ < off[ti_ + 1]; e_++) { \
                        float dz_ = zs[e_] - (z); \
                        if (dz_ > 0.0f && ((out) < 0.0f || dz_ < (out))) (out) = dz_; \
                    } \
                } } while (0)
            if (probe) {
                float q[3] = { pr[0] - c[0], pr[1] - c[1], pr[2] - c[2] }, dist;
                int X = (int)(dot3(q, U) * sc + 0.5f * (float)R), Y = (int)(dot3(q, V) * sc + 0.5f * (float)R);
                ABOVE(X, Y, dot3(q, D) + 0.004f, dist);
                pthread_mutex_lock(&mu);
                if (dist < 0.0f) nopen_dirs++; else if (dist < rmax) pocc += 1.0f - dist / rmax;
                pthread_mutex_unlock(&mu);
            }
            for (size_t v = 0; v < nvtx; v++) {
                if (vglass[v]) continue;
                const float *p = vp + v * 3, *nr = vn + v * 3;
                float cs = dot3(nr, D);
                if (cs <= 0.0f) continue;
                float q[3] = { p[0] - c[0], p[1] - c[1], p[2] - c[2] };
                float fx = dot3(q, U) * sc + 0.5f * (float)R - 0.5f, fy = dot3(q, V) * sc + 0.5f * (float)R - 0.5f;
                float sn = sqrtf(fmaxf(0.0f, 1.0f - cs * cs));
                float z = dot3(q, D) + 0.004f + texel * 1.5f * sn / fmaxf(cs, 0.15f);
                int ix = (int)floorf(fx + 0.5f), iy = (int)floorf(fy + 0.5f);
                float occ = 0.0f, open = 0.0f;
                for (int oy = -1; oy <= 1; oy++)
                    for (int ox = -1; ox <= 1; ox++) {
                        float dist;
                        ABOVE(ix + ox, iy + oy, z, dist);
                        if (dist < 0.0f) open += 1.0f;
                        else if (dist < rmax) occ += 1.0f - dist / rmax;
                    }
                lvis[v] += cs * ((1.0f - sky) * (1.0f - occ / 9.0f) + sky * open / 9.0f);
                lw[v] += cs;
            }
#undef ABOVE
            pthread_mutex_lock(&mu);
            if (++done % 32 == 0) { printf("  %d/%d directions\n", done, ndir); fflush(stdout); }
            pthread_mutex_unlock(&mu);
        }
        tvis[tid] = lvis; tw[tid] = lw;                   /* summed in thread order after the join (reproducible) */
        free(sp); free(cnt); free(off); free(zs);
    }
    return NULL;
}
