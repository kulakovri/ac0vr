#ifndef PS2_TERRAIN_H
#define PS2_TERRAIN_H
/* Realistic terrain, stage T2 (TERRAIN_RESEARCH_2026-10-21.md): relief lighting baked from the game's own heightmap.
   zero_terrain.c (game side) finds the heightmap of the loaded map, bakes one scalar light ratio per world texel on a
   worker thread (smooth relief, gully normals, cavity, the terrain's sun shadow for the mission's fixed sun) with its
   mipmaps; ps2_video.c uploads it into one R8 texture over the whole map and mesh.vert multiplies the vertex colour of
   the terrain's first pass by it, read prefiltered to the vertex spacing (pass 0 modulates the atlas texel with that
   colour; ACZ_TERRAIN_PIXEL=1: per pixel in mesh.frag). Shading only: the surface the game collides with is
   untouched. */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PS2_TERRAIN_MAP_UNITS 524288.0f   /* the map: 513 x 513 height samples, 1024 units apart, from -262144 */

/* master switch: ACZ_TERRAIN_REAL (when set) over the F4 setting (ps2_cfg.terrain_real) */
int ps2_terrain_real_on(void);

/* the first bake generation that belongs to the map loaded now (0xFFFFFFFF = none yet: draw the game's terrain
   unchanged); an uploaded generation >= this one may be applied */
uint32_t ps2_terrain_relief_valid_from(void);

/* renderer: if a bake newer than have_gen is ready, copy all its mip levels (R8, level 0 first, each level's rows
   tightly packed) into dst (cap bytes) and return its generation, size and level count; 0 = nothing new */
uint32_t ps2_terrain_relief_copy(void *dst, size_t cap, uint32_t have_gen, uint32_t *n, uint32_t *levels);

/* the base size N of the baked texture (ACZ_TERRAIN_RES; fixed for the session); 0 = the feature is off for the
   session (ACZ_TERRAIN_REAL=0 at start): the renderer then makes a 1 x 1 texture */
uint32_t ps2_terrain_relief_size(void);

/* the smooth ground of the loaded map (zero_terrain.c, from the T2 bake's shading surface): at world (x, z) the slope
   (dh/dx, dh/dz) and the cavity (blurred minus actual height, units: > 0 in hollows); 0 = no surface for the loaded map
   yet (the outputs are then 0). Any thread; lock-free (a published grid). */
int ps2_terrain_ground(float x, float z, float *gx, float *gz, float *cav);
/* the ground's height (units) at world (x, z) of the loaded map, from the same bake (the mirrors' live map: ground
   objects); 0 = none yet */
int ps2_terrain_height(float x, float z, float *h);
/* the rear-view mirrors' terrain (ps2_video.c mlv_*, ACZ_VR_COCKPIT_MIRRORS_TERRAIN): a static grid of the loaded map,
   PS2_TERRAIN_MIR_SIDE^2 vertices (x, y, z in game units, then RGBA8 bits as a float), every second height sample of the
   game's heightmap (2048 units), the colour the mean of the game's own atlas texels of the tiles around it x the
   terrain's vertex colour (90 / 128) x the realistic terrain's relief light, open water at its level; built once per map
   on the bake thread. Returns its generation (0 = none for the loaded map) and the vertices */
#define PS2_TERRAIN_MIR_SIDE 257u
uint32_t ps2_terrain_mirror_grid(const float **v);
/* the same grid's textures (ACZ_VR_COCKPIT_MIRRORS_TERRAIN_TEX): the tile grid (256 x 256 u16, rows by z: id & 0x3FF),
   and the game's atlas tiles at 32 x 32 with their mips to 1 x 1 in BC1, mip-major (every tile's level 0, then level 1,
   ...; PS2_TERRAIN_MIR_TT bytes a tile in all; texel column along x, row along z); nt tiles. Returns the grid's
   generation (0 = none) */
#define PS2_TERRAIN_MIR_TT 696u
uint32_t ps2_terrain_mirror_tiles(const uint16_t **grid, const uint8_t **bc1, uint32_t *nt);

/* ---- stage T3+ (zero_terrain_mat.c, ACZ_TERRAIN_MATERIALS): near-ground materials ------------------------------
   Band-limited material detail on the terrain's first pass near the eye (fields / grass, sand, rock, snow, soil): a
   procedural RGBA8 texture array (per material one 32 m tile with two scales of detail slope x / z, height, albedo
   variation; mean-neutral, mipmapped), sampled in world XZ and tinted to the atlas colour by a mean-neutral multiplier
   (mesh.frag -DTDETAIL, terrain_detail.glsl). Shading only. */
#define PS2_TMAT_LAYERS 5u                /* grass / fields, sand, rock, snow, soil */
#define PS2_TMAT_PAD4 0x01000000u         /* mesh block pad4 bit: a terrain patch with material detail */
/* the feature exists this session (ACZ_TERRAIN_MATERIALS != 0 at start) */
int ps2_tmat_on(void);
/* the texture side N (0 = off for the session: the renderer makes a 1 x 1 x 1 texture) and its level count */
uint32_t ps2_tmat_size(uint32_t *levels);
/* renderer: once the generated texture is ready, copy all levels (level by level, each level's PS2_TMAT_LAYERS layers
   one after the other, RGBA8 rows tightly packed) into dst (cap bytes) and free the host copy; returns 1 (0 = not
   ready yet, too small, or already taken) */
int ps2_tmat_copy(void *dst, size_t cap);
/* GS thread, terrain pass 0 of a patch (before the records are stored): if the patch lies within the material range of
   the eye, write the smooth ground (slope, cavity) into each record's second quadword, fill the block's material
   fields (l[0..2], r[0..3]) and set PS2_TMAT_PAD4 in b->pad4; returns 1 then. recs = count records of 64 bytes. */
struct ps2_vk_mesh_block;
int ps2_tmat_patch(unsigned char *recs, unsigned count, const float eye[3], int eye_ok, struct ps2_vk_mesh_block *b);
int ps2_tmat_ab_n(void);                 /* ACZ_TERRAIN_MATERIALS_AB: blocks of N frames on / off (0 = off) */
/* renderer, once a frame: the frame-time governor's share of the materials' strength (1 = as configured; ACZ_VR_GOV_LADDER
   "tmat:0.4"); the patches marked from then on carry strength x k (none at 0) */
void ps2_tmat_gov_keep(float k);
float ps2_tmat_aniso(void);              /* ACZ_TERRAIN_MATERIALS_ANISO (default 4) */
void ps2_tmat_report(void);              /* counters at exit */

#ifdef __cplusplus
}
#endif
#endif
