/* cpit_view.c -- offline software renderer + audit statistics for cockpit files: ACZCPIT3 (tools/cockpit/ac2cpit.py) and
 * ACZCPIT4 (tools/cockpit/cpit_pack.cpp, runtime/include/ps2_cpit4.h: indexed meshes, BC7 textures with stored mips, which it
 * decodes in software; night_assets 2026-11-04: the same views of both files, compared by tools/cockpit/cpit_diff.py).
 *
 * cpit_view IN.cpit OUTDIR [options] [-view NAME:YAW,PITCH[,X,Y,Z]] ...
 *   -w W -h H        image size (default 720 x 720)
 *   -fov DEG         horizontal field of view (default 100, about a Quest 3 eye)
 *   -hide LIST       mesh names left out, comma separated, '*' at the end = any rest (as ACZ_VR_COCKPIT_HIDE)
 *   -noglass         do not draw glass
 *   -tex             also write the per-texture / per-mesh table (OUTDIR/meshes.tsv, textures.tsv)
 *   -lod FILE,N      draw mesh LOD level N of tools/cockpit/cpit_lod.c's side file (the runtime's ACZ_VR_COCKPIT_LOD)
 * YAW + = left, PITCH + = up (degrees, as PS2_XR_TEST_HEAD); X, Y, Z metres in the eye frame (+ right, up, back).
 * Without -view: the audit set (ahead, panel, legs, consoles, sides, up, over the shoulders, lean forward).
 *
 * Per view it writes OUTDIR/NAME.ppm (a lit preview: texture x material x baked AO x a fixed light from above; glass
 * as a light tint; the sky above the eye's horizon blue, below brown) and OUTDIR/NAME_d.ppm (diagnosis: magenta = no
 * surface and no glass (a hole: the outside seen without glass), cyan tint = seen through glass only, blue = an
 * untextured part, and textured parts coloured by their texel density on the eye's image: red < 5 texels / degree
 * (blurry, texels larger than ~4 headset pixels), orange 5-10, yellow 10-20, green >= 20), and one TSV line per view on
 * stdout: shares of the image, texel-density shares, the parts covering most blurry / untextured pixels.
 * The animated parts stay in their rest pose (needles at zero, controls centred). Hands (mesh flag 16) are left out.
 * Every other part is drawn, also those the runtime leaves out at load (ACZ_VR_COCKPIT_CONST_HIDE: parts behind a
 * constant-false select): check a finding against the game's eye dumps. -pick X,Y prints the part under that pixel.
 * Supersedes cpit_preview.py (format 1 only). ck_audit 2026-10-28 (a private script: the audit of all models).
 *
 * Texel density as the GPU samples it (perf4_cockpit 2026-11-03):
 *   -proj FX,FY      the eye target's focal lengths in pixels per tangent instead of -fov (the runtime's 4x stereo
 *                    target: -w 2048 -h 1792 -proj 744,627.4, logged as "vrs-fov: ... 744.0 x 627.4 px per tangent")
 *   -crop X0,Y0,X1,Y1  the statistics count only these pixels (the headset's crop of one eye: 0,290,1649,1792)
 *   -texlod          per textured pixel the mip level the sampler takes (Vulkan's anisotropic LOD: rho_x, rho_y the
 *                    texel footprint along the pixel's x / y, eta = min(rho_max / rho_min, aniso), lambda =
 *                    log2(rho_max / eta) + bias; lambda < 0 magnifies level 0, trilinear reads floor(lambda) and the
 *                    level above) and the taps (ceil(eta)); OUTDIR/texlod.tsv per texture: pixels, the share magnified,
 *                    the share touching level 0, the least lambda over all views (the finest level any view reads:
 *                    "max_tex_for" = the size whose levels are all that is ever sampled, i.e. a cap that leaves every
 *                    pixel as it is), mean taps, the shares over 2 / 4 / 8 taps; a "lod-summary" line per model
 *   -aniso N         the sampler's max anisotropy (16 = the runtime's sampler_mip), -lodbias B the LOD bias (0)
 *   -lodviews        the audit set plus six head-box views (lean forward / left / right, close to the side consoles)
 *   -zones R1,R2     the shares of the solid model's pixels in the crop (no glass in front) and of the glass pixels
 *                    inside R1 / between R1 and R2 / beyond R2 degrees off the image's axis, and the shading work left
 *                    at 1x1 / 2x1 / 2x2 (ACZ_VR_COCKPIT_VRS); a "zones-summary" line over all views
 * Pure C, no dependencies but the format's header: gcc -O2 -Iruntime/include -o build/tools/cpit_view.exe tools/cockpit/cpit_view.c -lm
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ps2_cpit4.h"

typedef struct { uint32_t w, h, levels; uint8_t *lv[16]; uint32_t lw[16], lh[16]; double chroma, lstd, lmean; int alpha_cut; } Tex;
typedef struct { char name[33]; uint32_t tex, flags, ntri; float rgba[4], spec, shin; float *v; int hidden;
                 double area, uvtexels; long px, px_blur, px_soft; } Mesh;
static Tex *tex; static uint32_t ntex; static Mesh *mesh; static uint32_t nmesh;
static int W = 720, H = 720; static double FOV = 100.0;
static float *zb, *col, *dens; static int *mid; static uint8_t *glass;
/* -proj / -crop / -texlod (see the header) */
static double PFX, PFY;                    /* -proj: focal lengths (px per tangent); 0 = from -fov, square pixels */
static int CX0, CY0, CX1 = 1 << 30, CY1 = 1 << 30;
static int texlod; static double ANISO = 16.0, LODBIAS = 0.0;
static float *lam; static uint8_t *taps;   /* per pixel: the sampler's lambda, its taps (0: untextured) */
typedef struct { long px, mag, l0, t2, t4, t8; double taps, lmin; } TexLod;
static TexLod *tl;
static long lh_bin[8], tap_bin[5], lod_px; static double lod_taps;   /* the model over all views */
static double ZR1 = -1, ZR2 = -1; static double zs_all[2][3];         /* -zones: [solid, glass][inside, ring, beyond] */

static void build_mips(Tex *t) {
    t->levels = 1; t->lw[0] = t->w; t->lh[0] = t->h;
    while (t->levels < 16 && (t->lw[t->levels - 1] > 1 || t->lh[t->levels - 1] > 1)) {
        uint32_t pw = t->lw[t->levels - 1], ph = t->lh[t->levels - 1];
        uint32_t nw = pw > 1 ? pw / 2 : 1, nh = ph > 1 ? ph / 2 : 1;
        uint8_t *s = t->lv[t->levels - 1], *d = malloc((size_t)nw * nh * 4);
        for (uint32_t y = 0; y < nh; y++) for (uint32_t x = 0; x < nw; x++) for (int c = 0; c < 4; c++) {
            uint32_t x0 = x * 2 < pw ? x * 2 : pw - 1, x1 = x * 2 + 1 < pw ? x * 2 + 1 : pw - 1;
            uint32_t y0 = y * 2 < ph ? y * 2 : ph - 1, y1 = y * 2 + 1 < ph ? y * 2 + 1 : ph - 1;
            d[(y * nw + x) * 4 + c] = (uint8_t)((s[(y0 * pw + x0) * 4 + c] + s[(y0 * pw + x1) * 4 + c]
                                                 + s[(y1 * pw + x0) * 4 + c] + s[(y1 * pw + x1) * 4 + c] + 2) / 4);
        }
        t->lv[t->levels] = d; t->lw[t->levels] = nw; t->lh[t->levels] = nh; t->levels++;
    }
}

static void tex_stats(Tex *t) {
    /* colourfulness and luminance spread on a <= 256^2 mip: grey low-contrast textures are raw AO bakes / placeholders */
    uint32_t l = 0; while (l + 1 < t->levels && (t->lw[l] > 256 || t->lh[l] > 256)) l++;
    double sc = 0, sl = 0, sl2 = 0; long n = 0; uint8_t *p = t->lv[l];
    for (uint32_t i = 0; i < t->lw[l] * t->lh[l]; i++, p += 4) {
        if (p[3] < 128) continue;
        double r = p[0] / 255.0, g = p[1] / 255.0, b = p[2] / 255.0;
        double mx = fmax(r, fmax(g, b)), mn = fmin(r, fmin(g, b)), L = 0.2126 * r + 0.7152 * g + 0.0722 * b;
        sc += mx - mn; sl += L; sl2 += L * L; n++;
    }
    if (n) { t->chroma = sc / n; t->lmean = sl / n; t->lstd = sqrt(fmax(0, sl2 / n - t->lmean * t->lmean)); }
    p = t->lv[0];
    for (size_t i = 0; i < (size_t)t->w * t->h; i++) if (p[i * 4 + 3] < 128) { t->alpha_cut = 1; break; }
}

static void sample(const Tex *t, double u, double v, double lod, float out[4]) {
    int l = (int)floor(lod + 0.5); if (l < 0) l = 0; if (l >= (int)t->levels) l = (int)t->levels - 1;
    uint32_t w = t->lw[l], h = t->lh[l]; const uint8_t *p = t->lv[l];
    double x = (u - floor(u)) * w - 0.5, y = (v - floor(v)) * h - 0.5;
    int x0 = (int)floor(x), y0 = (int)floor(y); double fx = x - x0, fy = y - y0;
    int xa = ((x0 % (int)w) + (int)w) % (int)w, xb = (xa + 1) % (int)w, ya = ((y0 % (int)h) + (int)h) % (int)h, yb = (ya + 1) % (int)h;
    for (int c = 0; c < 4; c++)
        out[c] = (float)(((p[(ya * w + xa) * 4 + c] * (1 - fx) + p[(ya * w + xb) * 4 + c] * fx) * (1 - fy)
                         + (p[(yb * w + xa) * 4 + c] * (1 - fx) + p[(yb * w + xb) * 4 + c] * fx) * fy) / 255.0);
}

/* a mesh's area and texel count (m->v, m->ntri, m->tex set) */
static void mesh_stats(Mesh *m) {
    for (uint32_t t = 0; t < m->ntri; t++) {
        float *a = m->v + t * 27, *b = a + 9, *c = a + 18;
        double e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
        double cx = e1[1] * e2[2] - e1[2] * e2[1], cy = e1[2] * e2[0] - e1[0] * e2[2], cz = e1[0] * e2[1] - e1[1] * e2[0];
        double ar = 0.5 * sqrt(cx * cx + cy * cy + cz * cz);
        double uvar = 0.5 * fabs((b[6] - a[6]) * (c[7] - a[7]) - (c[6] - a[6]) * (b[7] - a[7]));
        m->area += ar;
        if (m->tex < ntex) m->uvtexels += uvar * tex[m->tex].w * tex[m->tex].h;
    }
}
/* format 4: the whole file in memory, the textures' stored levels decoded, the meshes expanded to format 3's soup */
static int load4(FILE *f) {
    fseek(f, 0, SEEK_END); long long n = _ftelli64(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)n);
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) return -1;
    fclose(f);
    uint32_t nanim;
    memcpy(&ntex, d + 8, 4); memcpy(&nmesh, d + 12, 4); memcpy(&nanim, d + 16, 4);
    tex = calloc(ntex + 1, sizeof *tex); mesh = calloc(nmesh + 1, sizeof *mesh);
    size_t p = 20;
    for (uint32_t i = 0; i < ntex; i++) {
        uint32_t w, h, fm, fl; size_t tx, by;
        if (!cpit4_tex_rec(d, (size_t)n, p, &w, &h, &fm, &fl, &tx, &by)) { fprintf(stderr, "bad texture %u\n", i); return -1; }
        Tex *t = &tex[i];
        t->w = w; t->h = h;
        if (fm == CPIT_TF_RGBA8) {
            t->lv[0] = malloc((size_t)w * h * 4); memcpy(t->lv[0], d + tx, (size_t)w * h * 4);
            build_mips(t);
        } else {
            uint32_t nl = cpit4_levels(w, h); size_t o = tx;
            t->levels = nl > 16 ? 16 : nl;
            for (uint32_t l = 0; l < nl; l++) {
                uint32_t lw = cpit4_lv_dim(w, l), lh = cpit4_lv_dim(h, l);
                if (l < 16) {
                    t->lw[l] = lw; t->lh[l] = lh; t->lv[l] = malloc((size_t)lw * lh * 4);
                    cpit4_decode_level(fm, d + o, lw, lh, t->lv[l]);
                }
                o += cpit4_level_bytes(fm, lw, lh);
            }
        }
        tex_stats(t);
        p = tx + by;
    }
    for (uint32_t i = 0; i < nanim; i++) { uint32_t ni; memcpy(&ni, d + p + 36, 4); p += 40 + (size_t)ni * 8; }
    for (uint32_t i = 0; i < nmesh; i++) {
        Mesh *m = &mesh[i];
        memcpy(m->name, d + p, 32); m->name[32] = 0;
        memcpy(&m->tex, d + p + 32, 4); memcpy(m->rgba, d + p + 36, 16); memcpy(&m->flags, d + p + 52, 4);
        memcpy(&m->spec, d + p + 72, 4); memcpy(&m->shin, d + p + 76, 4); memcpy(&m->ntri, d + p + 80, 4);
        p += CPIT4_MESH_HDR;
        m->v = malloc((size_t)m->ntri * 27 * 4 + 4);
        if (m->ntri) p += cpit4_mesh_expand(d + p, m->ntri, (uint8_t *)m->v);
        mesh_stats(m);
    }
    free(d);
    return 0;
}

static int load(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) { perror(path); return -1; }
    char mag[8]; uint32_t hd[3];
    if (fread(mag, 1, 8, f) != 8) { fprintf(stderr, "too short\n"); return -1; }
    if (!memcmp(mag, "ACZCPIT4", 8)) return load4(f);
    if (memcmp(mag, "ACZCPIT3", 8) || fread(hd, 4, 3, f) != 3) { fprintf(stderr, "not ACZCPIT3 / ACZCPIT4\n"); return -1; }
    ntex = hd[0]; nmesh = hd[1]; uint32_t nanim = hd[2];
    tex = calloc(ntex + 1, sizeof *tex); mesh = calloc(nmesh + 1, sizeof *mesh);
    for (uint32_t i = 0; i < ntex; i++) {
        uint32_t wh[2]; if (fread(wh, 4, 2, f) != 2) return -1;
        tex[i].w = wh[0]; tex[i].h = wh[1]; tex[i].lv[0] = malloc((size_t)wh[0] * wh[1] * 4);
        if (fread(tex[i].lv[0], 4, (size_t)wh[0] * wh[1], f) != (size_t)wh[0] * wh[1]) return -1;
        build_mips(&tex[i]); tex_stats(&tex[i]);
    }
    for (uint32_t i = 0; i < nanim; i++) {
        uint32_t prop; float a[8]; uint32_t ni;
        if (fread(&prop, 4, 1, f) != 1 || fread(a, 4, 8, f) != 8 || fread(&ni, 4, 1, f) != 1) return -1;
        fseek(f, (long)ni * 8, SEEK_CUR);
    }
    for (uint32_t i = 0; i < nmesh; i++) {
        Mesh *m = &mesh[i]; uint32_t anim[4];
        if (fread(m->name, 1, 32, f) != 32 || fread(&m->tex, 4, 1, f) != 1 || fread(m->rgba, 4, 4, f) != 4
            || fread(&m->flags, 4, 1, f) != 1 || fread(anim, 4, 4, f) != 4 || fread(&m->spec, 4, 1, f) != 1
            || fread(&m->shin, 4, 1, f) != 1 || fread(&m->ntri, 4, 1, f) != 1) return -1;
        m->name[32] = 0;
        m->v = malloc((size_t)m->ntri * 27 * 4);
        if (fread(m->v, 4, (size_t)m->ntri * 27, f) != (size_t)m->ntri * 27) return -1;
        mesh_stats(m);
    }
    fclose(f); return 0;
}

static int hidden_by(const char *name, const char *list) {
    if (!list) return 0;
    char buf[1024]; snprintf(buf, sizeof buf, "%s", list);
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        size_t n = strlen(tok);
        if (n && tok[n - 1] == '*') { if (!strncasecmp(name, tok, n - 1)) return 1; }
        else if (!strcasecmp(name, tok)) return 1;
    }
    return 0;
}

/* camera */
static double R[3][3], E[3], F_;           /* world->camera rotation, eye, focal length in pixels */
static double FX_, FY_;                    /* ... per axis (-proj; else both F_) */
static void set_cam(double yaw, double pitch, const double eye[3]) {
    double cy = cos(yaw), sy = sin(yaw), cp = cos(pitch), sp = sin(pitch);
    /* camera->world = Ry(yaw) Rx(pitch); world->camera = its transpose */
    double M[3][3] = { { cy, sy * sp, sy * cp }, { 0, cp, -sp }, { -sy, cy * sp, cy * cp } };
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) R[i][j] = M[j][i];
    memcpy(E, eye, sizeof E); F_ = 0.5 * W / tan(FOV * M_PI / 360.0);
    FX_ = PFX > 0 ? PFX : F_; FY_ = PFY > 0 ? PFY : F_;
    if (PFX > 0) F_ = FX_;                 /* (the degrees-per-pixel estimate of the density colours) */
}

typedef struct { double x, y, z, n[3], u, v, ao; } CV;     /* camera-space vertex */
static const double LDIR[3] = { 0.28, 0.83, -0.48 };        /* fixed key light (eye frame), normalised below */
static long st_px_tex, st_blur5, st_blur10, st_blur20, st_sharp, st_untex, st_opaque, st_glassonly, st_nohit, st_screen;
static long *mesh_blur, *mesh_untex, *tex_px, *tex_blur;
static int glass_pass;

static void raster_tri(const CV *a, const CV *b, const CV *c, int mi) {
    const Mesh *m = &mesh[mi];
    double sx[3], sy[3], iw[3]; const CV *vv[3] = { a, b, c };
    for (int k = 0; k < 3; k++) { double w = -vv[k]->z; iw[k] = 1.0 / w; sx[k] = W * 0.5 + FX_ * vv[k]->x * iw[k]; sy[k] = H * 0.5 - FY_ * vv[k]->y * iw[k]; }
    double area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
    if (fabs(area) < 1e-12) return;
    int x0 = (int)floor(fmin(sx[0], fmin(sx[1], sx[2]))), x1 = (int)ceil(fmax(sx[0], fmax(sx[1], sx[2])));
    int y0 = (int)floor(fmin(sy[0], fmin(sy[1], sy[2]))), y1 = (int)ceil(fmax(sy[0], fmax(sy[1], sy[2])));
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > W - 1) x1 = W - 1; if (y1 > H - 1) y1 = H - 1;
    if (x0 > x1 || y0 > y1) return;
    /* barycentric gradients */
    double dl0x = (sy[1] - sy[2]) / area, dl0y = (sx[2] - sx[1]) / area;
    double dl1x = (sy[2] - sy[0]) / area, dl1y = (sx[0] - sx[2]) / area;
    double dl2x = -dl0x - dl1x, dl2y = -dl0y - dl1y;
    double Au[3] = { a->u * iw[0], b->u * iw[1], c->u * iw[2] }, Av[3] = { a->v * iw[0], b->v * iw[1], c->v * iw[2] };
    double dQx = dl0x * iw[0] + dl1x * iw[1] + dl2x * iw[2], dQy = dl0y * iw[0] + dl1y * iw[1] + dl2y * iw[2];
    double dAux = dl0x * Au[0] + dl1x * Au[1] + dl2x * Au[2], dAuy = dl0y * Au[0] + dl1y * Au[1] + dl2y * Au[2];
    double dAvx = dl0x * Av[0] + dl1x * Av[1] + dl2x * Av[2], dAvy = dl0y * Av[0] + dl1y * Av[1] + dl2y * Av[2];
    const Tex *t = m->tex < ntex ? &tex[m->tex] : NULL;
    int screen = (m->flags >> 8) & 0xFF;
    double ln = sqrt(LDIR[0] * LDIR[0] + LDIR[1] * LDIR[1] + LDIR[2] * LDIR[2]);
    for (int py = y0; py <= y1; py++) for (int px = x0; px <= x1; px++) {
        double qx = px + 0.5, qy = py + 0.5;
        double l0 = ((sx[1] - qx) * (sy[2] - qy) - (sx[2] - qx) * (sy[1] - qy)) / area;
        double l1 = ((sx[2] - qx) * (sy[0] - qy) - (sx[0] - qx) * (sy[2] - qy)) / area;
        double l2 = 1.0 - l0 - l1;
        if (l0 < -1e-9 || l1 < -1e-9 || l2 < -1e-9) continue;
        double Q = l0 * iw[0] + l1 * iw[1] + l2 * iw[2];
        int k = py * W + px;
        if (Q <= zb[k]) continue;                                   /* zb holds 1/w of the nearest opaque surface */
        double u = (l0 * Au[0] + l1 * Au[1] + l2 * Au[2]) / Q, v = (l0 * Av[0] + l1 * Av[1] + l2 * Av[2]) / Q;
        float tc[4] = { 1, 1, 1, 1 }; double tpd = -1, glam = 0; int gtaps = 0;
        if (t) {
            double Auq = u * Q, Avq = v * Q;
            double dux = (dAux - Auq / Q * dQx) / Q * t->w, duy = (dAuy - Auq / Q * dQy) / Q * t->w;
            double dvx = (dAvx - Avq / Q * dQx) / Q * t->h, dvy = (dAvy - Avq / Q * dQy) / Q * t->h;
            /* singular values of the texel Jacobian: texels per pixel along the most / least stretched direction */
            double s1 = dux * dux + duy * duy + dvx * dvx + dvy * dvy, det = fabs(dux * dvy - duy * dvx);
            double disc = sqrt(fmax(0, s1 * s1 - 4 * det * det));
            double smax = sqrt(fmax(0, 0.5 * (s1 + disc))), smin = sqrt(fmax(0, 0.5 * (s1 - disc)));
            double lod = smax > 1 ? log2(smax) : 0;
            if (texlod) {      /* Vulkan's anisotropic LOD from the footprint along the pixel's x and y */
                double rx = sqrt(dux * dux + dvx * dvx), ry = sqrt(duy * duy + dvy * dvy);
                double rmax = fmax(rx, ry), rmin = fmin(rx, ry);
                double eta = rmin > 0 ? fmin(rmax / rmin, ANISO) : ANISO;
                if (eta < 1) eta = 1;
                glam = (rmax > 0 ? log2(rmax / eta) : -30) + LODBIAS;
                gtaps = rmax > 1 ? (int)ceil(eta - 1e-9) : 1;
                if (gtaps > 255) gtaps = 255;
            }
            sample(t, u, v, lod, tc);
            if (!glass_pass && tc[3] < 0.5f) continue;              /* alpha cut as the runtime (alpha < 128) */
            double r2 = (qx - W * 0.5) * (qx - W * 0.5) + (qy - H * 0.5) * (qy - H * 0.5);
            double degpp = 180.0 / M_PI * sqrt(F_ / (F_ * F_ + r2) * 1.0 / sqrt(F_ * F_ + r2));   /* degrees per pixel here */
            tpd = smin / degpp;                                     /* texels per degree along the blurriest axis */
        }
        double n[3] = { l0 * a->n[0] * iw[0] + l1 * b->n[0] * iw[1] + l2 * c->n[0] * iw[2],
                        l0 * a->n[1] * iw[0] + l1 * b->n[1] * iw[1] + l2 * c->n[1] * iw[2],
                        l0 * a->n[2] * iw[0] + l1 * b->n[2] * iw[1] + l2 * c->n[2] * iw[2] };
        double nl = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]); if (nl > 0) { n[0] /= nl; n[1] /= nl; n[2] /= nl; }
        double ao = (l0 * a->ao * iw[0] + l1 * b->ao * iw[1] + l2 * c->ao * iw[2]) / Q;
        if (glass_pass) {
            double al = fmax(0.10, fmin(0.5, m->rgba[3] * (t ? tc[3] : 1)));
            float *cc = col + 3 * k;
            for (int ch = 0; ch < 3; ch++) cc[ch] = (float)(cc[ch] * (1 - al) + al * 0.85 * m->rgba[ch] * tc[ch] + 0.04);
            glass[k] = 1;
            continue;
        }
        zb[k] = (float)Q; mid[k] = mi; dens[k] = (float)tpd;
        if (texlod) { lam[k] = (float)glam; taps[k] = (uint8_t)gtaps; }
        /* normals face the eye (as the runtime); camera space: the eye looks down -z, so a facing normal has n.z > 0 */
        double vx = -(l0 * a->x * iw[0] + l1 * b->x * iw[1] + l2 * c->x * iw[2]) / Q, vy = -(l0 * a->y * iw[0] + l1 * b->y * iw[1] + l2 * c->y * iw[2]) / Q,
               vz = -(l0 * a->z * iw[0] + l1 * b->z * iw[1] + l2 * c->z * iw[2]) / Q;
        if (n[0] * vx + n[1] * vy + n[2] * vz < 0) { n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2]; }
        double Lc[3]; for (int i = 0; i < 3; i++) Lc[i] = (R[i][0] * LDIR[0] + R[i][1] * LDIR[1] + R[i][2] * LDIR[2]) / ln;
        double dif = fmax(0, n[0] * Lc[0] + n[1] * Lc[1] + n[2] * Lc[2]);
        double lit = (0.50 + 0.60 * dif) * (0.25 + 0.75 * ao);
        float *cc = col + 3 * k;
        if (screen) { cc[0] = 0.03f; cc[1] = 0.10f; cc[2] = 0.05f; }
        else for (int ch = 0; ch < 3; ch++) cc[ch] = (float)(m->rgba[ch] * tc[ch] * lit);
    }
}

static void clip_and_draw(CV p[3], int mi) {
    const double NEAR = 0.01;
    CV in[4], out[4]; int n = 0;
    for (int k = 0; k < 3; k++) {
        CV *A = &p[k], *B = &p[(k + 1) % 3];
        int ia = A->z <= -NEAR, ib = B->z <= -NEAR;
        if (ia) in[n++] = *A;
        if (ia != ib) {
            double t = (-NEAR - A->z) / (B->z - A->z); CV C;
            C.x = A->x + t * (B->x - A->x); C.y = A->y + t * (B->y - A->y); C.z = -NEAR;
            for (int j = 0; j < 3; j++) C.n[j] = A->n[j] + t * (B->n[j] - A->n[j]);
            C.u = A->u + t * (B->u - A->u); C.v = A->v + t * (B->v - A->v); C.ao = A->ao + t * (B->ao - A->ao);
            if (n < 4) in[n++] = C;
        }
    }
    (void)out;
    for (int k = 1; k + 1 < n; k++) raster_tri(&in[0], &in[k], &in[k + 1], mi);
}

static void render_mesh(int mi) {
    Mesh *m = &mesh[mi];
    for (uint32_t t = 0; t < m->ntri; t++) {
        CV p[3]; int behind = 0;
        for (int k = 0; k < 3; k++) {
            const float *s = m->v + (t * 3 + k) * 9;
            double d[3] = { s[0] - E[0], s[1] - E[1], s[2] - E[2] };
            p[k].x = R[0][0] * d[0] + R[0][1] * d[1] + R[0][2] * d[2];
            p[k].y = R[1][0] * d[0] + R[1][1] * d[1] + R[1][2] * d[2];
            p[k].z = R[2][0] * d[0] + R[2][1] * d[1] + R[2][2] * d[2];
            for (int j = 0; j < 3; j++) p[k].n[j] = R[j][0] * s[3] + R[j][1] * s[4] + R[j][2] * s[5];
            p[k].u = s[6]; p[k].v = s[7]; p[k].ao = s[8];
            if (p[k].z > -0.01) behind++;
        }
        if (behind == 3) continue;
        clip_and_draw(p, mi);
    }
}

static void write_ppm(const char *path, const uint8_t *rgb) {
    FILE *f = fopen(path, "wb"); if (!f) { perror(path); return; }
    fprintf(f, "P6\n%d %d\n255\n", W, H); fwrite(rgb, 3, (size_t)W * H, f); fclose(f);
}
static uint8_t tob(double x) { x = x < 0 ? 0 : x > 1 ? 1 : x; return (uint8_t)(pow(x, 1 / 1.15) * 255 + 0.5); }

static void render_view(const char *outdir, const char *vname, double yawd, double pitchd, const double eye[3], int noglass) {
    set_cam(yawd * M_PI / 180, pitchd * M_PI / 180, eye);
    for (int k = 0; k < W * H; k++) { zb[k] = 0; mid[k] = -1; dens[k] = -1; glass[k] = 0; if (texlod) taps[k] = 0; }
    /* background: the outside, by the direction's height over the eye's horizontal */
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        double dc[3] = { (x + 0.5 - W * 0.5) / FX_, -(y + 0.5 - H * 0.5) / FY_, -1 };
        double wy = R[0][1] * dc[0] + R[1][1] * dc[1] + R[2][1] * dc[2];
        double l = sqrt(dc[0] * dc[0] + dc[1] * dc[1] + 1); wy /= l;
        float *c = col + 3 * (y * W + x);
        if (wy >= 0) { c[0] = (float)(0.55 - 0.25 * wy); c[1] = (float)(0.68 - 0.2 * wy); c[2] = (float)(0.88 - 0.05 * wy); }
        else { c[0] = 0.42f; c[1] = 0.36f; c[2] = 0.28f; }
    }
    glass_pass = 0;
    for (uint32_t i = 0; i < nmesh; i++) if (!mesh[i].hidden && !(mesh[i].flags & 1)) render_mesh((int)i);
    if (!noglass) { glass_pass = 1; for (uint32_t i = 0; i < nmesh; i++) if (!mesh[i].hidden && (mesh[i].flags & 1)) render_mesh((int)i); glass_pass = 0; }
    if (texlod) {      /* the visible textured pixels in the crop: the level the sampler takes, its taps */
        long vpx = 0, vb[8] = { 0 }; double vt = 0;
        for (int y = CY0 < 0 ? 0 : CY0; y < H && y < CY1; y++) for (int x = CX0 < 0 ? 0 : CX0; x < W && x < CX1; x++) {
            int k = y * W + x;
            if (mid[k] < 0 || !taps[k]) continue;
            const Mesh *m = &mesh[mid[k]];
            if (m->tex >= ntex || ((m->flags >> 8) & 0xFF)) continue;
            TexLod *q = &tl[m->tex]; double l = lam[k]; int n = taps[k];
            if (!q->px || l < q->lmin) q->lmin = l;
            q->px++; q->taps += n;
            if (l < 0) q->mag++;
            if (l < 1) q->l0++;
            if (n > 2) q->t2++;
            if (n > 4) q->t4++;
            if (n > 8) q->t8++;
            int b = l < -1 ? 0 : l < 0 ? 1 : l < 1 ? 2 : l < 2 ? 3 : l < 3 ? 4 : 5;
            vb[b]++; lh_bin[b]++; vpx++; vt += n; lod_px++; lod_taps += n;
            tap_bin[n <= 1 ? 0 : n <= 2 ? 1 : n <= 4 ? 2 : n <= 8 ? 3 : 4]++;
        }
        double nn = vpx ? (double)vpx : 1;
        printf("%s\tlod: textured px %ld\tlambda <-1 %.1f\t-1..0 %.1f\t0..1 %.1f\t1..2 %.1f\t2..3 %.1f\t>=3 %.1f\tmean taps %.2f\n",
               vname, vpx, 100.0 * vb[0] / nn, 100.0 * vb[1] / nn, 100.0 * vb[2] / nn, 100.0 * vb[3] / nn, 100.0 * vb[4] / nn,
               100.0 * vb[5] / nn, vt / nn);
    }
    if (ZR1 >= 0) {    /* the zones by the pixel's angle off the image's axis (the eye's: the projection centre) */
        double t1 = tan(ZR1 * M_PI / 180), t2 = tan(ZR2 * M_PI / 180), z[2][3] = { { 0 } };
        for (int y = CY0 < 0 ? 0 : CY0; y < H && y < CY1; y++) for (int x = CX0 < 0 ? 0 : CX0; x < W && x < CX1; x++) {
            int k = y * W + x, s = mid[k] >= 0 ? 0 : glass[k] ? 1 : -1;
            if (s < 0) continue;
            double tx = (x + 0.5 - W * 0.5) / FX_, ty = (y + 0.5 - H * 0.5) / FY_, t = sqrt(tx * tx + ty * ty);
            int b = t < t1 ? 0 : t < t2 ? 1 : 2;
            z[s][b]++; zs_all[s][b]++;
            if (s == 0 && glass[k]) { z[1][b]++; zs_all[1][b]++; }   /* (glass over a solid part: shaded too) */
        }
        for (int s = 0; s < 2; s++) {
            double n = z[s][0] + z[s][1] + z[s][2]; if (n <= 0) n = 1;
            printf("%s\tzones %s px %.0f\tinside %.1f\tring %.1f\tbeyond %.1f\twork %.1f%%\n", vname, s ? "glass" : "solid",
                   z[s][0] + z[s][1] + z[s][2], 100 * z[s][0] / n, 100 * z[s][1] / n, 100 * z[s][2] / n,
                   100 * (z[s][0] + 0.5 * z[s][1] + 0.25 * z[s][2]) / n);
        }
    }
    uint8_t *img = malloc((size_t)W * H * 3), *dg = malloc((size_t)W * H * 3);
    st_px_tex = st_blur5 = st_blur10 = st_blur20 = st_sharp = st_untex = st_opaque = st_glassonly = st_nohit = st_screen = 0;
    memset(mesh_blur, 0, nmesh * sizeof *mesh_blur); memset(mesh_untex, 0, nmesh * sizeof *mesh_untex);
    for (int k = 0; k < W * H; k++) {
        float *c = col + 3 * k;
        for (int ch = 0; ch < 3; ch++) img[3 * k + ch] = tob(c[ch]);
        double g = 0.25 + 0.5 * (0.3 * c[0] + 0.59 * c[1] + 0.11 * c[2]);
        double o[3];
        if (mid[k] < 0) {
            if (glass[k]) { st_glassonly++; o[0] = 0.45 + 0.4 * g; o[1] = 0.85; o[2] = 0.9; }
            else { st_nohit++; o[0] = 1; o[1] = 0; o[2] = 1; }
        } else {
            const Mesh *m = &mesh[mid[k]]; st_opaque++;
            double t[3];
            if ((m->flags >> 8) & 0xFF) { st_screen++; t[0] = 0.1; t[1] = 0.1; t[2] = 0.1; }
            else if (m->tex >= ntex) { st_untex++; mesh_untex[mid[k]]++; t[0] = 0.15; t[1] = 0.35; t[2] = 1.0; }
            else {
                double d = dens[k]; st_px_tex++; tex_px[m->tex]++; if (d < 10) tex_blur[m->tex]++;
                if (d < 5) { st_blur5++; mesh_blur[mid[k]]++; t[0] = 1; t[1] = 0.1; t[2] = 0.1; }
                else if (d < 10) { st_blur10++; mesh_blur[mid[k]]++; t[0] = 1; t[1] = 0.55; t[2] = 0.1; }
                else if (d < 20) { st_blur20++; t[0] = 1; t[1] = 0.95; t[2] = 0.2; }
                else { st_sharp++; t[0] = 0.2; t[1] = 0.85; t[2] = 0.25; }
            }
            for (int ch = 0; ch < 3; ch++) o[ch] = 0.45 * t[ch] + 0.55 * g * (0.6 + 0.8 * t[ch]) * 0.7;
            if (glass[k]) { o[1] = o[1] * 0.85 + 0.12; o[2] = o[2] * 0.85 + 0.12; }
        }
        for (int ch = 0; ch < 3; ch++) dg[3 * k + ch] = (uint8_t)(fmin(1, fmax(0, o[ch])) * 255);
    }
    char p[1024];
    snprintf(p, sizeof p, "%s/%s.ppm", outdir, vname); write_ppm(p, img);
    snprintf(p, sizeof p, "%s/%s_d.ppm", outdir, vname); write_ppm(p, dg);
    double N = (double)W * H, NT = st_px_tex ? (double)st_px_tex : 1;
    printf("%s\topaque %.1f\tglass_only %.1f\thole %.1f\tuntex %.1f\tscreen %.1f\t|tex <5 %.1f\t5-10 %.1f\t10-20 %.1f\t>=20 %.1f",
           vname, 100 * st_opaque / N, 100 * st_glassonly / N, 100 * st_nohit / N, 100 * st_untex / N, 100 * st_screen / N,
           100 * st_blur5 / NT, 100 * st_blur10 / NT, 100 * st_blur20 / NT, 100 * st_sharp / NT);
    /* the three parts with most blurry (< 10 t/deg) pixels and the three untextured ones with most pixels */
    for (int pass = 0; pass < 2; pass++) {
        long *arr = pass ? mesh_untex : mesh_blur; int used[3] = { -1, -1, -1 };
        printf(pass ? "\t|untex:" : "\t|blur:");
        for (int r = 0; r < 3; r++) {
            int best = -1;
            for (uint32_t i = 0; i < nmesh; i++) if (arr[i] > 0 && (best < 0 || arr[i] > arr[best]) && (int)i != used[0] && (int)i != used[1]) best = (int)i;
            if (best < 0) break;
            used[r] = best;
            if (pass) printf(" %s(%.1f%%,c%.2f/%.2f/%.2f)", mesh[best].name, 100 * arr[best] / N, mesh[best].rgba[0], mesh[best].rgba[1], mesh[best].rgba[2]);
            else printf(" %s(%.1f%%,t%u %ux%u)", mesh[best].name, 100 * arr[best] / N, mesh[best].tex,
                        mesh[best].tex < ntex ? tex[mesh[best].tex].w : 0, mesh[best].tex < ntex ? tex[mesh[best].tex].h : 0);
        }
    }
    printf("\n"); fflush(stdout);
    free(img); free(dg);
}

/* -lod FILE.lod,LEVEL (perf6_cockpitladder): draw the parts with that mesh LOD level of tools/cockpit/cpit_lod.c's side
   file (per part an index list over its own corners; the runtime's ACZ_VR_COCKPIT_LOD) instead of the file's triangles */
static int lod_apply(const char *arg) {
    char path[1024]; int lev = 1;
    const char *c = strrchr(arg, ',');
    if (c) { snprintf(path, sizeof path, "%.*s", (int)(c - arg), arg); lev = atoi(c + 1); }
    else snprintf(path, sizeof path, "%s", arg);
    FILE *f = fopen(path, "rb"); if (!f) { perror(path); return -1; }
    char mag[8]; uint32_t hd[2];
    if (fread(mag, 1, 8, f) != 8 || memcmp(mag, "ACZCLOD1", 8) || fread(hd, 4, 2, f) != 2 || hd[0] > nmesh
        || lev < 1 || (uint32_t)lev > hd[1]) { fprintf(stderr, "%s: not a LOD file for this model / level\n", path); fclose(f); return -1; }
    fseek(f, 4L * hd[1] + 4L, SEEK_CUR);
    for (uint32_t i = 0; i < hd[0]; i++) {
        uint32_t nt; if (fread(&nt, 4, 1, f) != 1 || nt != mesh[i].ntri) { fprintf(stderr, "%s: made for another build (part %u)\n", path, i); fclose(f); return -1; }
    }
    long before = 0, after = 0;
    for (int l = 1; l <= lev; l++)
        for (uint32_t i = 0; i < hd[0]; i++) {
            uint32_t n; if (fread(&n, 4, 1, f) != 1) { fclose(f); return -1; }
            if (n == 0xFFFFFFFFu) { if (l == lev) { before += mesh[i].ntri; after += mesh[i].ntri; } continue; }
            uint32_t *cs = malloc((size_t)n * 4 + 4);
            if (fread(cs, 4, n, f) != n) { fclose(f); return -1; }
            if (l == lev) {
                Mesh *m = &mesh[i];
                float *nv = malloc((size_t)n * 9 * 4 + 4);
                for (uint32_t k = 0; k < n; k++) memcpy(nv + (size_t)k * 9, m->v + (size_t)(cs[k] < m->ntri * 3 ? cs[k] : 0) * 9, 36);
                before += m->ntri; after += n / 3;
                free(m->v); m->v = nv; m->ntri = n / 3;
            }
            free(cs);
        }
    fclose(f);
    printf("lod\t%s level %d: %ld -> %ld triangles\n", path, lev, before, after);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: cpit_view IN.cpit OUTDIR [-w W] [-h H] [-fov F] [-hide LIST] [-noglass] [-tex] [-view N:Y,P[,X,Y,Z]]...\n"); return 2; }
    const char *in = argv[1], *outdir = argv[2], *hide = NULL; int noglass = 0, dotex = 0, lodviews = 0;
    const char *views[64]; int nv = 0; const char *picks[64]; int npick = 0; const char *lodarg = NULL;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "-w") && i + 1 < argc) W = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-h") && i + 1 < argc) H = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-fov") && i + 1 < argc) FOV = atof(argv[++i]);
        else if (!strcmp(argv[i], "-hide") && i + 1 < argc) hide = argv[++i];
        else if (!strcmp(argv[i], "-noglass")) noglass = 1;
        else if (!strcmp(argv[i], "-tex")) dotex = 1;
        else if (!strcmp(argv[i], "-proj") && i + 1 < argc) sscanf(argv[++i], "%lf,%lf", &PFX, &PFY);
        else if (!strcmp(argv[i], "-crop") && i + 1 < argc) sscanf(argv[++i], "%d,%d,%d,%d", &CX0, &CY0, &CX1, &CY1);
        else if (!strcmp(argv[i], "-texlod")) texlod = 1;
        else if (!strcmp(argv[i], "-aniso") && i + 1 < argc) ANISO = atof(argv[++i]);
        else if (!strcmp(argv[i], "-lodbias") && i + 1 < argc) LODBIAS = atof(argv[++i]);
        else if (!strcmp(argv[i], "-lodviews")) lodviews = 1;
        else if (!strcmp(argv[i], "-zones") && i + 1 < argc) sscanf(argv[++i], "%lf,%lf", &ZR1, &ZR2);
        else if (!strcmp(argv[i], "-pick") && i + 1 < argc && npick < 64) picks[npick++] = argv[++i];
        else if (!strcmp(argv[i], "-view") && i + 1 < argc && nv < 64) views[nv++] = argv[++i];
        else if (!strcmp(argv[i], "-lod") && i + 1 < argc) lodarg = argv[++i];
    }
    if (load(in)) return 1;
    if (lodarg && lod_apply(lodarg)) return 1;
    for (uint32_t i = 0; i < nmesh; i++) mesh[i].hidden = (mesh[i].flags & 16) || hidden_by(mesh[i].name, hide);
    zb = malloc((size_t)W * H * 4); col = malloc((size_t)W * H * 12); dens = malloc((size_t)W * H * 4);
    mid = malloc((size_t)W * H * sizeof *mid); glass = malloc((size_t)W * H);
    tex_px = calloc(ntex + 1, sizeof *tex_px); tex_blur = calloc(ntex + 1, sizeof *tex_blur);
    mesh_blur = calloc(nmesh + 1, sizeof *mesh_blur); mesh_untex = calloc(nmesh + 1, sizeof *mesh_untex);
    if (texlod) { lam = malloc((size_t)W * H * 4); taps = malloc((size_t)W * H); tl = calloc(ntex + 1, sizeof *tl); }
    static const char *defv[] = { "a_ahead:0,-8", "b_panel:0,-30", "c_legs:0,-62", "d_lconsole:55,-42", "e_rconsole:-55,-42",
                                  "f_left:90,-8", "g_right:-90,-8", "h_up:0,62", "i_backleft:145,5", "j_backright:-145,5",
                                  "k_lean:0,-35,0,-0.06,-0.16", "l_upback:180,55" };
    /* (-lodviews: the head box -- a strapped pilot leans ~20 cm forward, ~15 cm aside, and turns to a console) */
    static const char *boxv[] = { "m_close:0,-40,0,-0.08,-0.22", "n_leanleft:30,-38,-0.15,-0.06,-0.12",
                                  "o_leanright:-30,-38,0.15,-0.06,-0.12", "p_lconsole_near:75,-55,-0.12,-0.08,-0.04",
                                  "q_rconsole_near:-75,-55,0.12,-0.08,-0.04", "r_combiner:0,-5,0,0.03,-0.12" };
    if (!nv) { for (size_t i = 0; i < sizeof defv / sizeof *defv; i++) views[nv++] = defv[i]; }
    if (lodviews) { for (size_t i = 0; i < sizeof boxv / sizeof *boxv && nv < 64; i++) views[nv++] = boxv[i]; }
    for (int i = 0; i < nv; i++) {
        char nm[64]; double y = 0, p = 0, e[3] = { 0, 0, 0 };
        const char *c = strchr(views[i], ':'); if (!c) continue;
        snprintf(nm, sizeof nm, "%.*s", (int)(c - views[i]), views[i]);
        sscanf(c + 1, "%lf,%lf,%lf,%lf,%lf", &y, &p, &e[0], &e[1], &e[2]);
        render_view(outdir, nm, y, p, e, noglass);
        for (int q = 0; q < npick; q++) {   /* -pick X,Y: the opaque part under that pixel of this view */
            int px_ = 0, py_ = 0; sscanf(picks[q], "%d,%d", &px_, &py_);
            if (px_ < 0 || py_ < 0 || px_ >= W || py_ >= H) continue;
            int m_ = mid[py_ * W + px_];
            printf("pick %s %d,%d: %s (mesh %d)\n", nm, px_, py_, m_ < 0 ? "-" : mesh[m_].name, m_);
        }
    }
    if (ZR1 >= 0)
        for (int s = 0; s < 2; s++) {
            double n = zs_all[s][0] + zs_all[s][1] + zs_all[s][2]; if (n <= 0) n = 1;
            printf("zones-summary\t%s\tinside %.1f\tring %.1f\tbeyond %.1f\twork %.1f%%\n", s ? "glass" : "solid",
                   100 * zs_all[s][0] / n, 100 * zs_all[s][1] / n, 100 * zs_all[s][2] / n,
                   100 * (zs_all[s][0] + 0.5 * zs_all[s][1] + 0.25 * zs_all[s][2]) / n);
        }
    if (texlod) {
        /* per texture: the finest level any view samples (lmin) and the size cap that keeps every sampled level
           (max_tex_for: half a level of margin for the GPU's own LOD arithmetic, quad derivatives, poses not rendered) */
        char p[1024]; FILE *f = NULL;
        snprintf(p, sizeof p, "%s/texlod.tsv", outdir);
        if ((f = fopen(p, "w")))
            fprintf(f, "tex\tw\th\tpx\tmag_pct\tlevel0_pct\tlambda_min\tmax_tex_for\tmean_taps\ttaps_gt2_pct\ttaps_gt4_pct"
                       "\ttaps_gt8_pct\tvram_mb\tvram_capped_mb\tfirst_mesh\n");
        double vram = 0, vcap = 0, vunseen = 0;
        for (uint32_t t = 0; t < ntex; t++) {
            const TexLod *q = &tl[t];
            double full = 4.0 / 3.0 * tex[t].w * tex[t].h * 4 / 1e6, cap = full;
            uint32_t mx = tex[t].w > tex[t].h ? tex[t].w : tex[t].h, need = mx;
            if (q->px) {
                int lv = (int)floor(q->lmin - 0.5); if (lv < 0) lv = 0;
                while (lv > 0 && (mx >> lv) < 1) lv--;
                need = mx >> lv; cap = full / (double)(1u << (2 * lv));
            } else vunseen += full;
            vram += full; vcap += cap;
            const char *fm = "-";
            for (uint32_t i = 0; i < nmesh; i++) if (mesh[i].tex == t && !mesh[i].hidden) { fm = mesh[i].name; break; }
            double n = q->px ? (double)q->px : 1;
            if (f) fprintf(f, "%u\t%u\t%u\t%ld\t%.1f\t%.1f\t%.2f\t%u\t%.2f\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%s\n", t, tex[t].w,
                           tex[t].h, q->px, 100.0 * q->mag / n, 100.0 * q->l0 / n, q->px ? q->lmin : 99.0, need, q->taps / n,
                           100.0 * q->t2 / n, 100.0 * q->t4 / n, 100.0 * q->t8 / n, full, cap, fm);
        }
        if (f) fclose(f);
        double nn = lod_px ? (double)lod_px : 1;
        printf("lod-summary\ttextured px %ld\tlambda <-1 %.1f\t-1..0 %.1f\t0..1 %.1f\t1..2 %.1f\t2..3 %.1f\t>=3 %.1f\t"
               "taps 1 %.1f\t2 %.1f\t3-4 %.1f\t5-8 %.1f\t>8 %.1f\tmean taps %.2f\tvram %.1f MB\tcapped %.1f MB\tunseen %.1f MB\n",
               lod_px, 100.0 * lh_bin[0] / nn, 100.0 * lh_bin[1] / nn, 100.0 * lh_bin[2] / nn, 100.0 * lh_bin[3] / nn,
               100.0 * lh_bin[4] / nn, 100.0 * lh_bin[5] / nn, 100.0 * tap_bin[0] / nn, 100.0 * tap_bin[1] / nn,
               100.0 * tap_bin[2] / nn, 100.0 * tap_bin[3] / nn, 100.0 * tap_bin[4] / nn, lod_taps / nn, vram, vcap, vunseen);
    }
    if (dotex) {
        char p[1024]; FILE *f;
        snprintf(p, sizeof p, "%s/textures.tsv", outdir);
        if ((f = fopen(p, "w"))) {
            fprintf(f, "tex\tw\th\tchroma\tlum_mean\tlum_std\talpha_cut\tmeshes\tarea_m2\ttexels_per_m\tview_px\tview_px_blur10\tfirst_mesh\n");
            for (uint32_t t = 0; t < ntex; t++) {
                double ar = 0, ut = 0; int nm = 0; const char *fm = "-";
                for (uint32_t i = 0; i < nmesh; i++) if (mesh[i].tex == t && !mesh[i].hidden) { ar += mesh[i].area; ut += mesh[i].uvtexels; if (!nm) fm = mesh[i].name; nm++; }
                fprintf(f, "%u\t%u\t%u\t%.3f\t%.3f\t%.3f\t%d\t%d\t%.4f\t%.0f\t%ld\t%ld\t%s\n", t, tex[t].w, tex[t].h, tex[t].chroma, tex[t].lmean,
                        tex[t].lstd, tex[t].alpha_cut, nm, ar, ar > 0 ? sqrt(ut / ar) : 0, tex_px[t], tex_blur[t], fm);
            }
            fclose(f);
        }
        snprintf(p, sizeof p, "%s/meshes.tsv", outdir);
        if ((f = fopen(p, "w"))) {
            fprintf(f, "mesh\tname\ttex\tflags\tr\tg\tb\ta\tntri\tarea_m2\ttexels_per_m\tcx\tcy\tcz\tdist\n");
            for (uint32_t i = 0; i < nmesh; i++) {
                Mesh *m = &mesh[i]; double c[3] = { 0, 0, 0 };
                for (uint32_t t = 0; t < m->ntri * 3; t++) for (int j = 0; j < 3; j++) c[j] += m->v[t * 9 + j];
                if (m->ntri) for (int j = 0; j < 3; j++) c[j] /= m->ntri * 3.0;
                fprintf(f, "%u\t%s\t%d\t%u\t%.3f\t%.3f\t%.3f\t%.3f\t%u\t%.4f\t%.0f\t%.3f\t%.3f\t%.3f\t%.3f\n", i, m->name,
                        m->tex < ntex ? (int)m->tex : -1, m->flags, m->rgba[0], m->rgba[1], m->rgba[2], m->rgba[3], m->ntri,
                        m->area, m->area > 0 && m->tex < ntex ? sqrt(m->uvtexels / m->area) : 0, c[0], c[1], c[2],
                        sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]));
            }
            fclose(f);
        }
    }
    return 0;
}
