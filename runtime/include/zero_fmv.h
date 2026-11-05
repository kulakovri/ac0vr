#ifndef ZERO_FMV_H
#define ZERO_FMV_H
/* AI-upscaled FMV movies in place of the game's MPEG2 ones (runtime/src/zero_fmv.c, ACZ_FMV_PACK). */
#include "ps2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* EE thread, nufile read (ps2_hle_iop.c): got bytes of f at pos went to guest buf */
struct ps2_disc_file;
void zero_fmv_read(const struct ps2_disc_file *f, u64 pos, u32 len, u32 buf, int got);
/* EE thread, IPU CSC (ps2_ipu.c): mbc converted macroblocks (ofm 0: RGB32, 1 KB each) */
void zero_fmv_csc(const u8 *rgb, u32 mbc, int ofm, u32 th0, u32 th1);
/* GS thread, a CT32 texture decode (ps2_gs.c current_texture_inner; texels = the decode, tw x th RGBA): when its
   top-left 16 x H texels are a column of a movie picture, the high-res strip standing in for that region (*rw x *rh
   RGBA = *scale x 16 x H; the game's draws sample only that region) and a salt for the store's hash, else NULL; the
   strip stays valid until zero_fmv_strip_done */
const u8 *zero_fmv_strip(const u8 *texels, u32 tw, u32 th, u32 *rw, u32 *rh, u32 *scale, u32 *salt);
void zero_fmv_strip_done(void);

#ifdef __cplusplus
}
#endif
#endif
