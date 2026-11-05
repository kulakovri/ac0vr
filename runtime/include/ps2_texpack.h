#ifndef PS2_TEXPACK_H
#define PS2_TEXPACK_H
/* T5 terrain texture pack (realistic terrain stage T5; runtime/src/zero_texpack.c, ACZ_TERRAIN_TEXPACK; the offline
   pipeline tools/terrain/t5_pack.py; research docs/research/2026-10-24/terrain-upscale-t5.md).

   Per tile set of the ground photo (64 x 64 PSMT8 tiles, 5.3 m texels) an offline 2x upscale made in the tiles' real
   world neighbourhoods and back-projected so that every 2 x 2 box of the pack equals the 2006 texel: the pack's mip 1 IS
   the 2006 tile, so from about 4 km on (and for every patch the game draws with its own mip level 1) the image is the
   game's. It is the shader water's path (zero_water.c): the EE thread copies the atlas, the palette and the grid once a
   map is stable, a worker finds the pack by the content key of atlas + palette (CRC-32: t5_<CRC>_<nt>.png in
   ACZ_TERRAIN_TEXPACK_DIR/<name>/), decodes it and builds its box mips into the renderer's staging buffer; the renderer
   (ps2_video.c tp_*) uploads it in chunks into an RGBA8 2D array (128 x 128, 8 levels, layer = tile id; 85 MiB of VRAM
   for 1024 tiles); rn_vp_terrain.c hands every terrain patch's first pass to zero_texpack_patch(), which solves the
   patch's tile and tile-local uv (block e[], as zero_water_patch does) and marks it; such a patch takes the mesh.frag
   variant -DTEXPACK (pipeline key bit 56), whose pass-0 fetch reads the pack instead of the GS region texture. Passes
   1-3, the vertex colour, relief light, cloud shadows, near-ground materials (classified from the 2006 texel) and the
   shader water stay as they are. No ps2_gs.c change. Shading only. */
#include "ps2_vk.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PS2_TEXPACK_TILES 1024u           /* array layers (tile ids are 10 bits) */
#define PS2_TEXPACK_TILE 128u             /* texels per tile side of a 2x pack (2 x the 2006 tile) */
#define PS2_TEXPACK_LEVELS 8u             /* 128 .. 1 */
#define PS2_TEXPACK_LEVELS_MAX 9u         /* a 4x pack (gr_mush 2026-10-28, tools/terrain/t5_pack4.py): 256 .. 1 */

/* Block fields of a pack patch (prog 1 pass 0): e[0..3] = u0, v0 (the tile's corner in the patch's uv), su, sv (tile uv
   = (uv - (u0, v0)) * (su, sv)), e[4] = layer (tile id), e[6] = 1 (the pack serves this patch). The water's fields of the
   same tile (e[0..4]) are identical; e[5], e[7..11] stay the water's / materials'. */

/* the feature exists this session (ACZ_TERRAIN_TEXPACK names a pack at start; default off) */
int  zero_texpack_on(void);
/* GS replay thread, pass 0 of a terrain patch: if the loaded map has a pack and the game draws the patch with its mip
   level 0, solve the tile uv into b (see above) and return 1 */
int  zero_texpack_patch(const unsigned char *recs, unsigned count, const ps2_vk_state *st, ps2_vk_mesh_block *b);
/* renderer, once at init: the mapped staging buffer the worker writes a pack into (all levels, level by level: level l
   of tile t at off[l] + t * side_l^2 * 4, off[] = level offsets for nt tiles; RGBA8) */
void zero_texpack_staging(void *ptr, size_t cap);
/* renderer: a pack waits in the staging buffer (returns its generation, tile count and bytes; 0 = none) */
uint32_t zero_texpack_pending(uint32_t *nt, size_t *bytes);
/* renderer: the copies of generation gen have run (the staging buffer is free again) */
void zero_texpack_uploaded(uint32_t gen);
/* renderer: the pack generation that belongs to the loaded map (0xFFFFFFFF = none: draw the game's texture) */
uint32_t zero_texpack_valid_from(void);
size_t zero_texpack_bytes(uint32_t nt);   /* staging / upload bytes of nt tiles (all levels) */
/* the session's pack tile side (128 = a 2x pack; 256 = a 4x pack: its sheet is 8192 wide) and its level count, found at
   start from the pack folder's first sheet (the renderer makes its image with them) */
uint32_t zero_texpack_tile(void);
uint32_t zero_texpack_levels(void);
float zero_texpack_bias(void);            /* ACZ_TERRAIN_TEXPACK_BIAS: mip bias of the pack (default 0) */
float zero_texpack_minlod(void);          /* ACZ_TERRAIN_TEXPACK_MINLOD: finest level sampled (0; 1 = the 2006 texels) */
float zero_texpack_aniso(void);           /* ACZ_TERRAIN_TEXPACK_ANISO (default 0 = as the game's terrain textures) */
int  zero_texpack_ab_n(void);             /* ACZ_TERRAIN_TEXPACK_AB: blocks of N frames on / off (0 = off) */
int  zero_texpack_tint(void);             /* ACZ_DEBUG_TEXPACK_TINT: 1 = patches the pack serves tinted */
int  zero_texpack_chunk(void);            /* ACZ_TERRAIN_TEXPACK_CHUNK: layers uploaded per frame (default 128) */
void zero_texpack_report(void);           /* counters at exit */

#ifdef __cplusplus
}
#endif
#endif
