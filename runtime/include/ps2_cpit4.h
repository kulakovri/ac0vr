/* ps2_cpit4.h -- the ACZCPIT4 cockpit file format: shared by the runtime's loader (ps2_video.c), the offline viewer
 * (tools/cockpit/cpit_view.c) and the packer (tools/cockpit/cpit_pack.cpp). Plain C, header only, no dependencies.
 * night_assets 2026-11-04 (RELEASE_RESEARCH_2026-11-03.md sections 3 and 15, R1).
 *
 * A format 4 file is format 3 (tools/cockpit/ac2cpit.py) with two sections changed; the animations are as they were:
 *   "ACZCPIT4" u32 ntex u32 nmesh u32 nanim
 *   per texture: u32 w, u32 h | CPIT4_TAG, u32 format (CPIT_TF_*), u32 flags (CPIT_TFL_*), then the texels:
 *       CPIT_TF_RGBA8: w * h * 4 bytes, level 0 only (the loader builds the mips by blits, as for format 3);
 *       CPIT_TF_BC7 / CPIT_TF_BC1: the whole mip chain, level 0 first, each level ceil(w / 4) * ceil(h / 4) blocks
 *       of 16 / 8 bytes; a level's size is max(w >> l, 1) x max(h >> l, 1); levels as the loader counts them
 *       (cpit4_levels). The mips are 2 x 2 box filtered from the source's level 0, then encoded.
 *   per animation: as format 3.
 *   per mesh: the 84-byte header of format 3 (ntri in its last field), then when ntri > 0: u32 nv, u32 vfmt, then
 *       vfmt 0 (CPIT4_V_F32): nv * 36 bytes (f32 pos[3], nrm[3], uv[2], ao: bit-identical to format 3's vertices),
 *       vfmt 1 (CPIT4_V_Q16): f32 pos0[3], f32 pstep, f32 uv0[2], f32 ustep[2], then nv * 16 bytes (u16 pos[3], i16
 *       oct normal[2], u16 uv[2], u8 ao, u8 pad; see cpit4_q16_decode); not exact, only with cpit_pack --q16;
 *       then ntri * 3 indices, u16 when nv <= 65536, else u32, padded to 4 bytes. The triangle order is format 3's, the
 *       vertices in first-use order, so expanding them gives format 3's triangle soup back.
 * A texture record whose h lacks CPIT4_TAG is a format 3 record: the record tells its own format, so an image may mix
 * them (the runtime's fallback gloves are spliced from another file).
 */
#ifndef PS2_CPIT4_H
#define PS2_CPIT4_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define CPIT4_TAG 0x80000000u
enum { CPIT_TF_RGBA8 = 0, CPIT_TF_BC1 = 1, CPIT_TF_BC7 = 7 };
enum { CPIT_TFL_CUT = 1u,      /* some texel of level 0 has alpha < 128 (in the source): the runtime's alpha-cut scan */
       CPIT_TFL_OPAQUE = 2u }; /* every texel of level 0 has alpha 255 (in the source) */
enum { CPIT4_V_F32 = 0, CPIT4_V_Q16 = 1, CPIT4_HDR_TEX = 16, CPIT4_MESH_HDR = 84, CPIT4_VTX = 36, CPIT4_Q16_HDR = 32, CPIT4_Q16_VTX = 16 };

static inline uint32_t cpit4_levels(uint32_t w, uint32_t h) {
    uint32_t l = 1;
    while ((w >> l) || (h >> l)) l++;
    return l;
}
static inline uint32_t cpit4_lv_dim(uint32_t d, uint32_t l) { return (d >> l) ? (d >> l) : 1u; }
static inline size_t cpit4_level_bytes(uint32_t fmt, uint32_t w, uint32_t h) {
    if (fmt == CPIT_TF_RGBA8) return (size_t)w * h * 4u;
    return (size_t)((w + 3u) / 4u) * ((h + 3u) / 4u) * (fmt == CPIT_TF_BC1 ? 8u : 16u);
}
/* the bytes of a record's texels */
static inline size_t cpit4_tex_bytes(uint32_t fmt, uint32_t w, uint32_t h) {
    if (fmt == CPIT_TF_RGBA8) return (size_t)w * h * 4u;
    size_t b = 0;
    uint32_t n = cpit4_levels(w, h);
    for (uint32_t l = 0; l < n; l++) b += cpit4_level_bytes(fmt, cpit4_lv_dim(w, l), cpit4_lv_dim(h, l));
    return b;
}
/* one texture record at d + p (d + n the end): its size, format, flags and the offset of its texels; 0 = malformed */
static inline int cpit4_tex_rec(const uint8_t *d, size_t n, size_t p, uint32_t *w, uint32_t *h, uint32_t *fmt, uint32_t *flags,
                                size_t *texels, size_t *bytes) {
    uint32_t a[4];
    if (p + 8 > n) return 0;
    memcpy(a, d + p, 8);
    *w = a[0];
    if (a[1] & CPIT4_TAG) {
        if (p + CPIT4_HDR_TEX > n) return 0;
        memcpy(a + 2, d + p + 8, 8);
        *h = a[1] & ~CPIT4_TAG; *fmt = a[2]; *flags = a[3]; *texels = p + CPIT4_HDR_TEX;
        if (a[2] != CPIT_TF_RGBA8 && a[2] != CPIT_TF_BC1 && a[2] != CPIT_TF_BC7) return 0;
    } else {
        *h = a[1]; *fmt = CPIT_TF_RGBA8; *flags = 0xFFFFFFFFu; *texels = p + 8;     /* (flags unknown: scan the alpha) */
    }
    if (!*w || !*h || *w > 65536u || *h > 65536u) return 0;
    *bytes = cpit4_tex_bytes(*fmt, *w, *h);
    return *texels + *bytes <= n;
}

/* ---- BC7 (all eight modes) and BC1 block decoders: the BPTC / S3TC specifications; checked block for block against
 * bc7decomp of bc7enc_rdo (tools/cockpit/cpit_pack --check), tables as in the Khronos data format specification */
static const uint8_t cpit_bc7_part2[64][16] = {
    {0,0,1,1,0,0,1,1,0,0,1,1,0,0,1,1},{0,0,0,1,0,0,0,1,0,0,0,1,0,0,0,1},{0,1,1,1,0,1,1,1,0,1,1,1,0,1,1,1},{0,0,0,1,0,0,1,1,0,0,1,1,0,1,1,1},
    {0,0,0,0,0,0,0,1,0,0,0,1,0,0,1,1},{0,0,1,1,0,1,1,1,0,1,1,1,1,1,1,1},{0,0,0,1,0,0,1,1,0,1,1,1,1,1,1,1},{0,0,0,0,0,0,0,1,0,0,1,1,0,1,1,1},
    {0,0,0,0,0,0,0,0,0,0,0,1,0,0,1,1},{0,0,1,1,0,1,1,1,1,1,1,1,1,1,1,1},{0,0,0,0,0,0,0,1,0,1,1,1,1,1,1,1},{0,0,0,0,0,0,0,0,0,0,0,1,0,1,1,1},
    {0,0,0,1,0,1,1,1,1,1,1,1,1,1,1,1},{0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1},{0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1},{0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1},
    {0,0,0,0,1,0,0,0,1,1,1,0,1,1,1,1},{0,1,1,1,0,0,0,1,0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0,1,0,0,0,1,1,1,0},{0,1,1,1,0,0,1,1,0,0,0,1,0,0,0,0},
    {0,0,1,1,0,0,0,1,0,0,0,0,0,0,0,0},{0,0,0,0,1,0,0,0,1,1,0,0,1,1,1,0},{0,0,0,0,0,0,0,0,1,0,0,0,1,1,0,0},{0,1,1,1,0,0,1,1,0,0,1,1,0,0,0,1},
    {0,0,1,1,0,0,0,1,0,0,0,1,0,0,0,0},{0,0,0,0,1,0,0,0,1,0,0,0,1,1,0,0},{0,1,1,0,0,1,1,0,0,1,1,0,0,1,1,0},{0,0,1,1,0,1,1,0,0,1,1,0,1,1,0,0},
    {0,0,0,1,0,1,1,1,1,1,1,0,1,0,0,0},{0,0,0,0,1,1,1,1,1,1,1,1,0,0,0,0},{0,1,1,1,0,0,0,1,1,0,0,0,1,1,1,0},{0,0,1,1,1,0,0,1,1,0,0,1,1,1,0,0},
    {0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1},{0,0,0,0,1,1,1,1,0,0,0,0,1,1,1,1},{0,1,0,1,1,0,1,0,0,1,0,1,1,0,1,0},{0,0,1,1,0,0,1,1,1,1,0,0,1,1,0,0},
    {0,0,1,1,1,1,0,0,0,0,1,1,1,1,0,0},{0,1,0,1,0,1,0,1,1,0,1,0,1,0,1,0},{0,1,1,0,1,0,0,1,0,1,1,0,1,0,0,1},{0,1,0,1,1,0,1,0,1,0,1,0,0,1,0,1},
    {0,1,1,1,0,0,1,1,1,1,0,0,1,1,1,0},{0,0,0,1,0,0,1,1,1,1,0,0,1,0,0,0},{0,0,1,1,0,0,1,0,0,1,0,0,1,1,0,0},{0,0,1,1,1,0,1,1,1,1,0,1,1,1,0,0},
    {0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0},{0,0,1,1,1,1,0,0,1,1,0,0,0,0,1,1},{0,1,1,0,0,1,1,0,1,0,0,1,1,0,0,1},{0,0,0,0,0,1,1,0,0,1,1,0,0,0,0,0},
    {0,1,0,0,1,1,1,0,0,1,0,0,0,0,0,0},{0,0,1,0,0,1,1,1,0,0,1,0,0,0,0,0},{0,0,0,0,0,0,1,0,0,1,1,1,0,0,1,0},{0,0,0,0,0,1,0,0,1,1,1,0,0,1,0,0},
    {0,1,1,0,1,1,0,0,1,0,0,1,0,0,1,1},{0,0,1,1,0,1,1,0,1,1,0,0,1,0,0,1},{0,1,1,0,0,0,1,1,1,0,0,1,1,1,0,0},{0,0,1,1,1,0,0,1,1,1,0,0,0,1,1,0},
    {0,1,1,0,1,1,0,0,1,1,0,0,1,0,0,1},{0,1,1,0,0,0,1,1,0,0,1,1,1,0,0,1},{0,1,1,1,1,1,1,0,1,0,0,0,0,0,0,1},{0,0,0,1,1,0,0,0,1,1,1,0,0,1,1,1},
    {0,0,0,0,1,1,1,1,0,0,1,1,0,0,1,1},{0,0,1,1,0,0,1,1,1,1,1,1,0,0,0,0},{0,0,1,0,0,0,1,0,1,1,1,0,1,1,1,0},{0,1,0,0,0,1,0,0,0,1,1,1,0,1,1,1}
};
static const uint8_t cpit_bc7_part3[64][16] = {
    {0,0,1,1,0,0,1,1,0,2,2,1,2,2,2,2},{0,0,0,1,0,0,1,1,2,2,1,1,2,2,2,1},{0,0,0,0,2,0,0,1,2,2,1,1,2,2,1,1},{0,2,2,2,0,0,2,2,0,0,1,1,0,1,1,1},
    {0,0,0,0,0,0,0,0,1,1,2,2,1,1,2,2},{0,0,1,1,0,0,1,1,0,0,2,2,0,0,2,2},{0,0,2,2,0,0,2,2,1,1,1,1,1,1,1,1},{0,0,1,1,0,0,1,1,2,2,1,1,2,2,1,1},
    {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2},{0,0,0,0,1,1,1,1,1,1,1,1,2,2,2,2},{0,0,0,0,1,1,1,1,2,2,2,2,2,2,2,2},{0,0,1,2,0,0,1,2,0,0,1,2,0,0,1,2},
    {0,1,1,2,0,1,1,2,0,1,1,2,0,1,1,2},{0,1,2,2,0,1,2,2,0,1,2,2,0,1,2,2},{0,0,1,1,0,1,1,2,1,1,2,2,1,2,2,2},{0,0,1,1,2,0,0,1,2,2,0,0,2,2,2,0},
    {0,0,0,1,0,0,1,1,0,1,1,2,1,1,2,2},{0,1,1,1,0,0,1,1,2,0,0,1,2,2,0,0},{0,0,0,0,1,1,2,2,1,1,2,2,1,1,2,2},{0,0,2,2,0,0,2,2,0,0,2,2,1,1,1,1},
    {0,1,1,1,0,1,1,1,0,2,2,2,0,2,2,2},{0,0,0,1,0,0,0,1,2,2,2,1,2,2,2,1},{0,0,0,0,0,0,1,1,0,1,2,2,0,1,2,2},{0,0,0,0,1,1,0,0,2,2,1,0,2,2,1,0},
    {0,1,2,2,0,1,2,2,0,0,1,1,0,0,0,0},{0,0,1,2,0,0,1,2,1,1,2,2,2,2,2,2},{0,1,1,0,1,2,2,1,1,2,2,1,0,1,1,0},{0,0,0,0,0,1,1,0,1,2,2,1,1,2,2,1},
    {0,0,2,2,1,1,0,2,1,1,0,2,0,0,2,2},{0,1,1,0,0,1,1,0,2,0,0,2,2,2,2,2},{0,0,1,1,0,1,2,2,0,1,2,2,0,0,1,1},{0,0,0,0,2,0,0,0,2,2,1,1,2,2,2,1},
    {0,0,0,0,0,0,0,2,1,1,2,2,1,2,2,2},{0,2,2,2,0,0,2,2,0,0,1,2,0,0,1,1},{0,0,1,1,0,0,1,2,0,0,2,2,0,2,2,2},{0,1,2,0,0,1,2,0,0,1,2,0,0,1,2,0},
    {0,0,0,0,1,1,1,1,2,2,2,2,0,0,0,0},{0,1,2,0,1,2,0,1,2,0,1,2,0,1,2,0},{0,1,2,0,2,0,1,2,1,2,0,1,0,1,2,0},{0,0,1,1,2,2,0,0,1,1,2,2,0,0,1,1},
    {0,0,1,1,1,1,2,2,2,2,0,0,0,0,1,1},{0,1,0,1,0,1,0,1,2,2,2,2,2,2,2,2},{0,0,0,0,0,0,0,0,2,1,2,1,2,1,2,1},{0,0,2,2,1,1,2,2,0,0,2,2,1,1,2,2},
    {0,0,2,2,0,0,1,1,0,0,2,2,0,0,1,1},{0,2,2,0,1,2,2,1,0,2,2,0,1,2,2,1},{0,1,0,1,2,2,2,2,2,2,2,2,0,1,0,1},{0,0,0,0,2,1,2,1,2,1,2,1,2,1,2,1},
    {0,1,0,1,0,1,0,1,0,1,0,1,2,2,2,2},{0,2,2,2,0,1,1,1,0,2,2,2,0,1,1,1},{0,0,0,2,1,1,1,2,0,0,0,2,1,1,1,2},{0,0,0,0,2,1,1,2,2,1,1,2,2,1,1,2},
    {0,2,2,2,0,1,1,1,0,1,1,1,0,2,2,2},{0,0,0,2,1,1,1,2,1,1,1,2,0,0,0,2},{0,1,1,0,0,1,1,0,0,1,1,0,2,2,2,2},{0,0,0,0,0,0,0,0,2,1,1,2,2,1,1,2},
    {0,1,1,0,0,1,1,0,2,2,2,2,2,2,2,2},{0,0,2,2,0,0,1,1,0,0,1,1,0,0,2,2},{0,0,2,2,1,1,2,2,1,1,2,2,0,0,2,2},{0,0,0,0,0,0,0,0,0,0,0,0,2,1,1,2},
    {0,0,0,2,0,0,0,1,0,0,0,2,0,0,0,1},{0,2,2,2,1,2,2,2,0,2,2,2,1,2,2,2},{0,1,0,1,2,2,2,2,2,2,2,2,2,2,2,2},{0,1,1,1,2,0,1,1,2,2,0,1,2,2,2,0}
};
static const uint8_t cpit_bc7_anchor2[64] = {15,15,15,15,15,15,15,15, 15,15,15,15,15,15,15,15, 15,2,8,2,2,8,8,15, 2,8,2,2,8,8,2,2,
                                             15,15,6,8,2,8,15,15, 2,8,2,2,2,15,15,6, 6,2,6,8,15,15,2,2, 15,15,15,15,15,2,2,15};
static const uint8_t cpit_bc7_anchor3a[64] = {3,3,15,15,8,3,15,15, 8,8,6,6,6,5,3,3, 3,3,8,15,3,3,6,10, 5,8,8,6,8,5,15,15,
                                              8,15,3,5,6,10,8,15, 15,3,15,5,15,15,15,15, 3,15,5,5,5,8,5,10, 5,10,8,13,15,12,3,3};
static const uint8_t cpit_bc7_anchor3b[64] = {15,8,8,3,15,15,3,8, 15,15,15,15,15,15,15,8, 15,8,15,3,15,8,15,8, 3,15,6,10,15,15,10,8,
                                              15,3,15,10,10,8,9,10, 6,15,8,15,3,6,6,8, 15,3,15,15,15,15,15,15, 15,15,15,15,3,15,15,8};
static inline uint32_t cpit_bc7_bits(const uint8_t *b, uint32_t *pos, uint32_t n) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < n; i++, (*pos)++) v |= (uint32_t)((b[*pos >> 3] >> (*pos & 7u)) & 1u) << i;
    return v;
}
static inline uint32_t cpit_bc7_interp(uint32_t e0, uint32_t e1, uint32_t idx, uint32_t bits) {
    static const uint8_t w2[4] = {0,21,43,64}, w3[8] = {0,9,18,27,37,46,55,64}, w4[16] = {0,4,9,13,17,21,26,30,34,38,43,47,51,55,60,64};
    uint32_t w = bits == 2 ? w2[idx] : bits == 3 ? w3[idx] : w4[idx];
    return ((64u - w) * e0 + w * e1 + 32u) >> 6;
}
/* one 16-byte block to 16 RGBA texels (row by row); a reserved mode (first byte 0) gives zeros, as the specification */
static inline void cpit_bc7_block(const uint8_t *b, uint8_t *out) {
    uint32_t mode = 0, pos = 0;
    while (mode < 8 && !((b[0] >> mode) & 1u)) mode++;
    if (mode >= 8) { memset(out, 0, 64); return; }
    pos = mode + 1;
    static const uint8_t subsets[8] = {3,2,3,2,1,1,1,2}, partb[8] = {4,6,6,6,0,0,0,6}, cb[8] = {4,6,5,7,5,7,7,5}, ab[8] = {0,0,0,0,6,8,7,5},
                         pb[8] = {6,2,0,4,0,0,2,4}, ib[8] = {3,3,2,2,2,2,4,2}, ib2[8] = {0,0,0,0,3,2,0,0};
    uint32_t ns = subsets[mode], part = partb[mode] ? cpit_bc7_bits(b, &pos, partb[mode]) : 0, rot = 0, isel = 0;
    if (mode == 4 || mode == 5) rot = cpit_bc7_bits(b, &pos, 2);
    if (mode == 4) isel = cpit_bc7_bits(b, &pos, 1);
    uint32_t e[6][4];
    for (uint32_t c = 0; c < 3; c++) for (uint32_t i = 0; i < ns * 2u; i++) e[i][c] = cpit_bc7_bits(b, &pos, cb[mode]);
    for (uint32_t i = 0; i < ns * 2u; i++) e[i][3] = ab[mode] ? cpit_bc7_bits(b, &pos, ab[mode]) : 255u;
    uint32_t pbit[6] = {0,0,0,0,0,0};
    for (uint32_t i = 0; i < pb[mode]; i++) pbit[i] = cpit_bc7_bits(b, &pos, 1);
    for (uint32_t i = 0; i < ns * 2u; i++) for (uint32_t c = 0; c < 4; c++) {
        uint32_t nb = c == 3 ? ab[mode] : cb[mode], v = e[i][c];
        if (c == 3 && !ab[mode]) continue;
        if (pb[mode]) {
            uint32_t p = mode == 1 ? pbit[i >> 1] : pbit[i];
            v = (v << 1) | p; nb++;
        }
        v <<= 8u - nb; v |= v >> nb;
        e[i][c] = v;
    }
    uint32_t idx[16], idx2[16];
    for (uint32_t i = 0; i < 16; i++) {
        uint32_t anchor = i == 0;
        if (ns == 2 && i == cpit_bc7_anchor2[part]) anchor = 1;
        if (ns == 3 && (i == cpit_bc7_anchor3a[part] || i == cpit_bc7_anchor3b[part])) anchor = 1;
        idx[i] = cpit_bc7_bits(b, &pos, ib[mode] - anchor);
    }
    if (ib2[mode]) for (uint32_t i = 0; i < 16; i++) idx2[i] = cpit_bc7_bits(b, &pos, ib2[mode] - (i == 0));
    for (uint32_t i = 0; i < 16; i++) {
        uint32_t s = ns == 2 ? cpit_bc7_part2[part][i] : ns == 3 ? cpit_bc7_part3[part][i] : 0u;
        const uint32_t *e0 = e[s * 2u], *e1 = e[s * 2u + 1u];
        uint32_t ci = idx[i], cbits = ib[mode], ai = idx[i], abits = ib[mode];
        if (ib2[mode]) {
            if (isel) { ci = idx2[i]; cbits = ib2[mode]; }
            else { ai = idx2[i]; abits = ib2[mode]; }
        }
        uint32_t px[4];
        for (uint32_t c = 0; c < 3; c++) px[c] = cpit_bc7_interp(e0[c], e1[c], ci, cbits);
        px[3] = ab[mode] ? cpit_bc7_interp(e0[3], e1[3], ai, abits) : 255u;
        if (rot) { uint32_t t = px[3]; px[3] = px[rot - 1u]; px[rot - 1u] = t; }
        for (uint32_t c = 0; c < 4; c++) out[i * 4u + c] = (uint8_t)px[c];
    }
}
/* one 8-byte block to 16 RGBA texels */
static inline void cpit_bc1_block(const uint8_t *b, uint8_t *out) {
    uint32_t c0 = (uint32_t)b[0] | (uint32_t)b[1] << 8, c1 = (uint32_t)b[2] | (uint32_t)b[3] << 8;
    uint32_t bits = (uint32_t)b[4] | (uint32_t)b[5] << 8 | (uint32_t)b[6] << 16 | (uint32_t)b[7] << 24;
    uint32_t p[4][4] = { { (c0 >> 11) * 255u / 31u, ((c0 >> 5) & 63u) * 255u / 63u, (c0 & 31u) * 255u / 31u, 255u },
                         { (c1 >> 11) * 255u / 31u, ((c1 >> 5) & 63u) * 255u / 63u, (c1 & 31u) * 255u / 31u, 255u } };
    for (uint32_t c = 0; c < 3; c++) {
        if (c0 > c1) { p[2][c] = (2u * p[0][c] + p[1][c]) / 3u; p[3][c] = (p[0][c] + 2u * p[1][c]) / 3u; }
        else { p[2][c] = (p[0][c] + p[1][c]) / 2u; p[3][c] = 0u; }
    }
    p[2][3] = 255u; p[3][3] = c0 > c1 ? 255u : 0u;
    for (uint32_t i = 0; i < 16; i++, bits >>= 2) for (uint32_t c = 0; c < 4; c++) out[i * 4u + c] = (uint8_t)p[bits & 3u][c];
}
/* a level's blocks (src) to w * h RGBA texels (dst) */
static inline void cpit4_decode_level(uint32_t fmt, const uint8_t *src, uint32_t w, uint32_t h, uint8_t *dst) {
    uint8_t px[64];
    uint32_t bs = fmt == CPIT_TF_BC1 ? 8u : 16u;
    for (uint32_t by = 0; by < (h + 3u) / 4u; by++) for (uint32_t bx = 0; bx < (w + 3u) / 4u; bx++, src += bs) {
        if (fmt == CPIT_TF_BC1) cpit_bc1_block(src, px); else cpit_bc7_block(src, px);
        for (uint32_t y = 0; y < 4 && by * 4u + y < h; y++)
            for (uint32_t x = 0; x < 4 && bx * 4u + x < w; x++)
                memcpy(dst + (((size_t)by * 4u + y) * w + bx * 4u + x) * 4u, px + (y * 4u + x) * 4u, 4);
    }
}
/* a 16-byte vertex (CPIT4_V_Q16) to format 3's 36 bytes; hdr = its f32 pos0[3], pstep, uv0[2], ustep[2] */
static inline void cpit4_q16_decode(const float *hdr, const uint8_t *v, uint8_t *out) {
    uint16_t q[3], t[2]; int16_t o[2];
    memcpy(q, v, 6); memcpy(o, v + 6, 4); memcpy(t, v + 10, 4);
    float f[9];
    for (int j = 0; j < 3; j++) f[j] = hdr[j] + hdr[3] * (float)q[j];
    float nx = (float)o[0] / 32767.0f, ny = (float)o[1] / 32767.0f, ax = nx < 0 ? -nx : nx, ay = ny < 0 ? -ny : ny, nz = 1.0f - ax - ay;
    if (nz < 0) { float ox = nx; nx = (1.0f - ay) * (ox >= 0 ? 1.0f : -1.0f); ny = (1.0f - ax) * (ny >= 0 ? 1.0f : -1.0f); }
    {   float l2 = nx * nx + ny * ny + nz * nz;
        /* (Newton step for the inverse square root: no libm here) */
        float il = 1.0f / l2;
        float g = 0.5f * (1.0f + il);
        for (int it = 0; it < 4; it++) g = 0.5f * (g + il / g);
        f[3] = nx * g; f[4] = ny * g; f[5] = nz * g; }
    f[6] = hdr[4] + hdr[6] * (float)t[0]; f[7] = hdr[5] + hdr[7] * (float)t[1];
    f[8] = (float)v[14] / 255.0f;
    memcpy(out, f, 36);
}
/* the size of format 4's meshes expanded to format 3's soup: walks the records at d + p; 0 = malformed */
static inline int cpit4_mesh_walk(const uint8_t *d, size_t n, size_t p, uint32_t nmesh, size_t *end, size_t *soup_bytes) {
    size_t sb = 0;
    for (uint32_t i = 0; i < nmesh; i++) {
        uint32_t ntri, nv, vfmt;
        if (p + CPIT4_MESH_HDR > n) return 0;
        memcpy(&ntri, d + p + CPIT4_MESH_HDR - 4, 4);
        p += CPIT4_MESH_HDR;
        if (ntri) {
            if (p + 8 > n) return 0;
            memcpy(&nv, d + p, 4); memcpy(&vfmt, d + p + 4, 4);
            p += 8 + (vfmt == CPIT4_V_Q16 ? CPIT4_Q16_HDR + (size_t)nv * CPIT4_Q16_VTX : (size_t)nv * CPIT4_VTX);
            p += (((size_t)ntri * 3u * (nv <= 65536u ? 2u : 4u)) + 3u) & ~(size_t)3u;
            if (p > n) return 0;
        }
        sb += CPIT4_MESH_HDR + (size_t)ntri * 3u * CPIT4_VTX;
    }
    *end = p; *soup_bytes = sb;
    return 1;
}
/* one mesh's vertices and indices (at v, after its header) to format 3's soup at out; returns the offset after them */
static inline size_t cpit4_mesh_expand(const uint8_t *v, uint32_t ntri, uint8_t *out) {
    uint32_t nv, vfmt;
    memcpy(&nv, v, 4); memcpy(&vfmt, v + 4, 4);
    const uint8_t *vd = v + 8, *ix;
    float qh[8];
    if (vfmt == CPIT4_V_Q16) { memcpy(qh, vd, sizeof qh); vd += CPIT4_Q16_HDR; ix = vd + (size_t)nv * CPIT4_Q16_VTX; }
    else ix = vd + (size_t)nv * CPIT4_VTX;
    int wide = nv > 65536u;
    for (size_t k = 0; k < (size_t)ntri * 3u; k++) {
        uint32_t i;
        if (wide) memcpy(&i, ix + k * 4u, 4);
        else { uint16_t s; memcpy(&s, ix + k * 2u, 2); i = s; }
        if (i >= nv) i = 0;
        if (vfmt == CPIT4_V_Q16) cpit4_q16_decode(qh, vd + (size_t)i * CPIT4_Q16_VTX, out + k * CPIT4_VTX);
        else memcpy(out + k * CPIT4_VTX, vd + (size_t)i * CPIT4_VTX, CPIT4_VTX);
    }
    return (size_t)(ix - v) + (((size_t)ntri * 3u * (wide ? 4u : 2u) + 3u) & ~(size_t)3u);
}
#endif
