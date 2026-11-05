#ifndef PS2_PROPS_H
#define PS2_PROPS_H
/* Houses raised from the ground photo (wishes #49 / #55 / #78, TOWNS_RESEARCH_2026-10-23.md; runtime/src/zero_props.c,
   ACZ_TERRAIN_PROPS, default off: 1 the red roofs, 2 also the desert buildings and tanks found by their shadows,
   zero_props_find.h; with either, a city photo's dense houses: ACZ_TERRAIN_CITY, zero_props_city.h, drawn per city cell
   from a vertex cache, their facades' window data in the records' second quadword for mesh.vert / mesh.frag). zero_props.c finds them in every terrain tile once per map (EE thread + one worker), never near a
   ground unit; rn_vp_terrain.c hands it every fine terrain patch right after the patch's first pass, and it draws the
   houses of that cell as one more mesh draw of the patch (same texture, colour, fog, relief and cloud shadows: mesh.vert
   prog 1 pass 0). Its draws carry PS2_PROPS_PAD4 in the block's pad4; ps2_video.c leaves them out of a frame for the
   in-run GPU A/B (ACZ_TERRAIN_PROPS_AB) and the exact image A/B (PS2_PERF_AB_FLIP=props). */
#include "ps2_vk.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PS2_PROPS_PAD4 0x40000000u        /* mesh block pad4 bit: a house draw of zero_props.c */

/* GS replay thread: after pass 0 of a terrain patch (recs = its count vertex records of 64 bytes, idx its nidx triangle
   indices as drawn) */
void zero_props_patch(const unsigned char *recs, unsigned count, const unsigned *idx, unsigned nidx, const ps2_vk_state *st,
                      const ps2_vk_mesh_block *b);
int  zero_props_on(void);                 /* ACZ_TERRAIN_PROPS (fixed for the session) */
int  zero_props_ab_n(void);               /* ACZ_TERRAIN_PROPS_AB: blocks of N frames on / off, 0 = no A/B */
/* the city's detailed look (ACZ_TERRAIN_CITY_LOOK, bug #261): the city draws take mesh.vert / mesh.frag push-constant
   bit PS2_CITY_LOOK_BIT (city_look.glsl); without it they look as before (their records carry the extra data either
   way). ACZ_TERRAIN_CITY_LOOK_AB: blocks of N frames with / without it, 0 = no A/B */
#define PS2_CITY_LOOK_BIT 524288u
int  zero_props_city_look(void);
int  zero_props_city_look_ab_n(void);
void zero_props_report(void);             /* counters at exit */

#ifdef __cplusplus
}
#endif
#endif
