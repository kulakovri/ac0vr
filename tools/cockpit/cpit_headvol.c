/* cpit_headvol.c -- the head volume of a cockpit model offline (bug #156; the runtime's own code,
   runtime/src/ps2_headvol.c): the clearance from the design eye along the main directions, the depth the head fade
   would see at test offsets, and two slices of the surface field through the design eye (side view x = 0 and top view
   y = 0, PGM, 4 px a cell: dark = a surface, the reference marked).
   Build: gcc -O2 -o build/tools/cpit_headvol.exe tools/cockpit/cpit_headvol.c runtime/src/ps2_headvol.c -lm
   Usage: cpit_headvol FILE.cpit [OUT_PREFIX] */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../runtime/src/ps2_headvol.h"

static void slice(const ps2_hv *h, const char *path, int axis) {
    /* axis 0: side view (z across, y up) at x = 0; 1: top view (x across, z down) at y = 0 */
    const float c = 0.005f;
    int W = axis == 0 ? (int)(2.0f / c) : (int)(1.6f / c), H = axis == 0 ? (int)(1.5f / c) : (int)(2.0f / c);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P5\n%d %d\n255\n", W, H);
    float r[3]; ps2_hv_ref(h, r);
    for (int j = 0; j < H; j++)
        for (int i = 0; i < W; i++) {
            float p[3];
            if (axis == 0) { p[0] = 0.0f; p[2] = -1.1f + (i + 0.5f) * c; p[1] = 0.7f - (j + 0.5f) * c; }
            else { p[1] = 0.0f; p[0] = -0.8f + (i + 0.5f) * c; p[2] = -1.1f + (j + 0.5f) * c; }
            float s = ps2_hv_dist(h, p), dp = ps2_hv_depth(h, p, NULL);
            int v = s < 0.01f ? 0 : (int)(60.0f + 1200.0f * s);
            if (v > 235) v = 235;
            if (dp > 0.0f) v = v / 2 + 10;                       /* beyond a surface from the reference: darker */
            float dx = p[0] - r[0], dy = p[1] - r[1], dz = p[2] - r[2];
            if (dx * dx + dy * dy + dz * dz < 0.012f * 0.012f) v = 255;
            fputc(v, f);
        }
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: cpit_headvol FILE.cpit [OUT_PREFIX]\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *d = (unsigned char *)malloc((size_t)n);
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "read failed\n"); return 1; }
    fclose(f);
    if (memcmp(d, "ACZCPIT3", 8)) { fprintf(stderr, "not ACZCPIT3\n"); return 1; }
    unsigned ntex, nmesh, nanim;
    memcpy(&ntex, d + 8, 4); memcpy(&nmesh, d + 12, 4); memcpy(&nanim, d + 16, 4);
    size_t p = 20;
    for (unsigned i = 0; i < ntex; i++) { unsigned w, h; memcpy(&w, d + p, 4); memcpy(&h, d + p + 4, 4); p += 8u + (size_t)w * h * 4u; }
    for (unsigned i = 0; i < nanim; i++) { unsigned nt; memcpy(&nt, d + p + 36, 4); p += 40u + (size_t)nt * 8u; }
    char info[512];
    ps2_hv *h = ps2_hv_build(d, (size_t)n, p, nmesh, NULL, 0.02f, info, sizeof info);
    const char *base = strrchr(argv[1], '/'); base = base ? base + 1 : argv[1];
    printf("%s: %s%s\n", base, info, h ? "" : " -- NO VOLUME");
    if (!h) return 0;
    static const struct { const char *nm; float d[3]; } dirs[] = {
        { "up", { 0, 1, 0 } }, { "back", { 0, 0, 1 } }, { "back-up", { 0, 0.7071f, 0.7071f } },
        { "left", { -1, 0, 0 } }, { "right", { 1, 0, 0 } }, { "fwd", { 0, 0, -1 } }, { "down", { 0, -1, 0 } },
        { "up-left", { -0.7071f, 0.7071f, 0 } }, { "up-right", { 0.7071f, 0.7071f, 0 } },
        { "back-left", { -0.7071f, 0, 0.7071f } }, { "back-right", { 0.7071f, 0, 0.7071f } },
        { "fwd-up", { 0, 0.5f, -0.866f } }, { "fwd-down", { 0, -0.5f, -0.866f } } };
    float r[3]; ps2_hv_ref(h, r);
    printf("  clearance from the reference (m):");
    for (unsigned k = 0; k < sizeof dirs / sizeof dirs[0]; k++) {
        float t = 0.0f;
        for (t = 0.005f; t < 1.2f; t += 0.005f) {
            float q[3] = { r[0] + dirs[k].d[0] * t, r[1] + dirs[k].d[1] * t, r[2] + dirs[k].d[2] * t };
            if (ps2_hv_depth(h, q, NULL) > 0.0f) break;
        }
        printf(" %s %.3f", dirs[k].nm, t);
    }
    printf("\n  depth at head offsets (m):");
    /* leaning back 10 / 20 / 30 cm, back and up, back and down; aside; aside and forward; down to the panel; up;
       checking six over each shoulder (a real head turns about the neck: about 150 deg, eyes -0.10 / +0.10 aside and
       0.17 back, PS2_XR_TEST_HEAD_SCRIPT's note), and the same with the eyes 3 cm higher / 5 cm further back */
    static const float tp[][3] = { { 0, 0, 0.10f }, { 0, 0, 0.20f }, { 0, 0, 0.30f }, { 0, 0.15f, 0.20f }, { 0, -0.1f, 0.25f },
                                   { 0.2f, 0, 0 }, { -0.25f, 0.05f, -0.1f }, { 0, -0.2f, -0.35f }, { 0, 0.2f, 0 },
                                   { -0.10f, 0, 0.17f }, { 0.10f, 0, 0.17f }, { -0.10f, 0.03f, 0.17f }, { -0.10f, 0, 0.22f } };
    for (unsigned k = 0; k < sizeof tp / sizeof tp[0]; k++) {
        float cl, dp = ps2_hv_depth(h, tp[k], &cl);
        printf(" (%.2f %.2f %.2f) %.3f/%.3f", tp[k][0], tp[k][1], tp[k][2], dp, cl);
    }
    printf("\n");
    if (argc > 2) {
        char path[1024];
        snprintf(path, sizeof path, "%s_side.pgm", argv[2]); slice(h, path, 0);
        snprintf(path, sizeof path, "%s_top.pgm", argv[2]); slice(h, path, 1);
    }
    ps2_hv_free(h);
    free(d);
    return 0;
}
