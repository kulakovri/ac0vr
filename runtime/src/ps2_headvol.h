/* ps2_headvol.h -- the 3D cockpit's solid parts around the design eye (bug #156: the head leaning back through the
   seat). Built once per cockpit model at load (ps2_video.c cpit_load_one, the loader thread) from the model file's
   triangles; asked once a frame how far the eyes have gone into the model's parts (ps2_video.c hvf_*). No Vulkan, no
   game state: tools/cockpit/cpit_headvol.c runs the same code offline over the .cpit files. */
#ifndef PS2_HEADVOL_H
#define PS2_HEADVOL_H
#include <stddef.h>

typedef struct ps2_hv ps2_hv;

/* d / n: the whole .cpit file; mesh_pos: the offset of its first mesh record; nmesh records; skip[i] != 0 leaves mesh
   i out (NULL: none). cell: the grid's cell size in metres (0: 0.02). info: a one-line summary for the log. NULL when
   there is no usable volume (no memory, or no free space at the design eye). */
ps2_hv *ps2_hv_build(const unsigned char *d, size_t n, size_t mesh_pos, unsigned nmesh, const unsigned char *skip,
                     float cell, char *info, size_t ninfo);
void ps2_hv_free(ps2_hv *h);

/* How far the point p (cockpit frame: metres from the design eye, x right, y up, z back) lies beyond the first of the
   model's surfaces met on the straight way from the reference point (the design eye, or the free point found next to
   it) -- 0 while nothing is in between -- plus the part of that way outside the grid (leaving the cockpit where it has
   no wall). clear (may be NULL): the distance from p to the nearest surface when nothing is crossed, else 0. */
float ps2_hv_depth(const ps2_hv *h, const float p[3], float *clear);

/* the reference point the ways start from (cockpit frame) */
void ps2_hv_ref(const ps2_hv *h, float r[3]);

/* the distance (m) from p to the nearest surface cell (trilinear; large outside the grid) */
float ps2_hv_dist(const ps2_hv *h, const float p[3]);

#endif
