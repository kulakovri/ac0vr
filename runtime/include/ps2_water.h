#ifndef PS2_WATER_H
#define PS2_WATER_H
/* Shader water (bug #63, the user's wish "make the shader water"; runtime/src/zero_water.c, ACZ_TERRAIN_WATER).

   The sea, lakes, rivers and canals of Zero are ordinary terrain tiles of the ground photo, painted with one flat water
   colour per map, and the grid marks the cells that carry them (0x2000 + 0x400: open water, a flat tile; 0x1000:
   shores, river banks, ports; rarely 0x800). zero_water.c finds the water texels of every tile once per map (EE thread
   copies the atlas, the palette and the grid; one worker builds a 64 x 64 mask per tile from the palette entries that the
   game uses only in those flagged cells) and publishes them; the renderer uploads the masks into one R8 2D-array texture
   (layer = tile id, mipmapped). rn_vp_terrain.c hands every terrain patch's first pass to zero_water_patch(), which
   marks the patches whose tile carries water (block e[], see below); mesh.vert / mesh.frag (terrain_water.glsl) shade
   the water texels of those patches: the game's photo colour stays the base, procedural waves in world space (a
   mipmapped slope texture in two scrolling layers, time from the frame's display time), Fresnel reflection of the
   game's sky colours by the reflected direction's elevation (and of our clouds through the cloud shadow map), a sun
   glitter from the mission's sun with the slope variance of the filtered waves (no sparkle aliasing), dimmed by our
   cloud shadows, faded to the game's flat look with distance. Shading only: the surface the game collides with is
   unchanged. */
#include "ps2_vk.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PS2_WATER_TILES 1024u             /* mask array layers (tile ids are 10 bits) */
#define PS2_WATER_TILE 64u                /* texels per tile side */
#define PS2_WATER_LEVELS 7u               /* 64 .. 1 */

/* Block fields of a water patch (prog 1 pass 0; free for terrain): e[0..3] = u0, v0 (the tile's corner in the patch's
   uv), su, sv (tile texels per uv unit / 64: mask uv = (uv - (u0, v0)) * (su, sv)); e[4..7] = mask layer (tile id),
   water fraction of the tile, 0, class (0 none, 1 partial: fetch the mask, 2 full); e[8..11] = the eye (world units),
   1 */

/* master switch for this session (ACZ_TERRAIN_WATER: 0 off, 1 on); also read by the renderer at init */
int  zero_water_on(void);
/* GS replay thread, before pass 0 of every terrain patch is stored: fills the water fields of b (a copy the caller
   draws pass 0 with) and returns 1 if the patch carries water; recs = the patch's count records of 64 bytes */
int  zero_water_patch(const unsigned char *recs, unsigned count, const float eye[3], int eye_ok, ps2_vk_mesh_block *b);
/* renderer: the first mask generation that belongs to the loaded map (0xFFFFFFFF = none: no water shading) */
uint32_t zero_water_valid_from(void);
/* renderer: if masks newer than have_gen are ready, copy them (ntiles layers of all PS2_WATER_LEVELS levels, level by
   level: level l of tile t at offset off[l] + t * side_l^2, off[] = level offsets for ntiles) into dst (cap bytes); returns
   the generation (0 = nothing new) */
uint32_t zero_water_copy(void *dst, size_t cap, uint32_t have_gen, uint32_t *ntiles);
/* renderer: the water parameters of a frame (sun, sky colours, strengths, cloud band; time in seconds of display) into
   the reserved block (mesh.frag reads it); 0 = no sun info yet (still fine) */
void zero_water_frame_block(ps2_vk_mesh_block *w, double time_s);
/* renderer, once: the wave slope texture (n x n RGBA8 UNORM: s_x / 8 + 0.5, s_z / 8 + 0.5, s_x^2 / 16, s_z^2 / 16)
   with all its levels into out (level by level, 4 bytes per texel); returns the number of bytes written (0 = failed) */
size_t zero_water_waves(uint8_t *out, uint32_t n);
int  zero_water_ab_n(void);               /* ACZ_TERRAIN_WATER_AB: blocks of N frames on / off */
int  zero_water_tint(void);               /* ACZ_DEBUG_WATER_TINT: 1 = the mask as blue, 2 = the wave normals */
void zero_water_report(void);             /* counters at exit */

#ifdef __cplusplus
}
#endif
#endif
