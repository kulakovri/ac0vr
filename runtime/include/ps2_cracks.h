#ifndef PS2_CRACKS_H
#define PS2_CRACKS_H
/* Canopy cracks from hits (the user's wish 2026-10-23; agent fx_cracks; design: CRACKS_RESEARCH_2026-10-23.md).

   zero_cracks.c (EE thread, ac5 only) watches the game's damage function 0x1FCD28 for the player's unit and turns each
   hit into an event: the kind (gun / missile / special / other), the damage as a share of the HP at the mission start,
   the HP left, and the direction of the source from the pilot's eye in cockpit axes (x right, y up, z back: the missile
   at its proximity burst, else the attacker's unit). ACZ_IMM_CRACK_TEST makes the same events without a hit.
   ps2_cracks.c (renderer thread, in ac5 and gsreplay) owns the cracks: at the cockpit model's load it keeps a copy of
   the canopy / windscreen triangles (ps2_video.c passes them), at a hit it casts the direction onto the glass and grows
   a crack pattern there for the glass's material -- radial arms that kink, branch and stop at the frame and at earlier
   cracks, concentric chords, a crushed whitened core (or a hole) -- in the impact's tangent plane, stored as line
   segments with a coarse grid per impact (one storage buffer, cockpit.frag draws them in the glass branch). It also
   lets stretched acrylic creep slowly under load, plays the crack's sound (ps2_crk_mix, the audio thread) and gives
   a short rumble jolt (ps2_crk_rumble). Nothing in the game changes. */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- hit events (EE thread -> renderer) ---- */
enum { CRK_GUN = 0, CRK_MISSILE = 1, CRK_SPECIAL = 2, CRK_AREA = 3 };
typedef struct {
    uint32_t field;      /* game field (vblank count) */
    int32_t kind;        /* CRK_* */
    float share;         /* damage / HP at the mission start (one F-5E missile: 40 / 90 = 0.44) */
    float hp_left;       /* HP left / HP at the start (0..1) */
    float dir[3];        /* towards the source from the eye, cockpit axes (x right, y up, z back); 0,0,0 = unknown */
    float dist_m;        /* the source's distance (m; 0 = unknown) */
    uint32_t src;        /* the attacker (group << 8 | index), 0xFFFF = none: hits of one burst add up */
    uint32_t test;       /* 1 = a fake hit (ACZ_IMM_CRACK_TEST) */
} crk_hit;
void ps2_crk_hit_push(const crk_hit *h);          /* (EE thread) */

/* ---- the glass of a cockpit model (the model loader's thread; the renderer uses it once the model is in use) ---- */
typedef struct crk_glass crk_glass;
crk_glass *ps2_crk_glass_begin(const char *path);
/* a glass mesh of the model: mesh = its index in the model, name (<= 32 chars), its triangles' vertices (three a
   triangle, positions as 3 floats at the start of each stride-byte vertex) */
void ps2_crk_glass_mesh(crk_glass *g, uint32_t mesh, const char *name, const void *verts, uint32_t stride, uint32_t ntri);
/* after the model is in: the canopy / windscreen meshes keep their triangles (keep = 0: the HUD's combiner, gauge
   glasses, an inner copy), then the borders are found */
void ps2_crk_glass_keep(crk_glass *g, uint32_t mesh, int keep);
void ps2_crk_glass_end(crk_glass *g);
void ps2_crk_glass_free(crk_glass *g);
/* the crack mesh id cockpit.frag gets for model mesh `mesh` (light.w), or -1: no crack on it now */
int ps2_crk_mesh_slot(const crk_glass *g, uint32_t mesh);

/* ---- per frame (renderer) ---- */
typedef struct {
    float sun[3];        /* towards the sun, cockpit frame (unit); 0 = none */
    float sun_rgb[3];    /* its colour x visibility (0 at night, in cloud shadow) */
    float head[9];       /* rows: cockpit frame -> head axes (x right, y up, z back) of the latest cockpit draw */
    int head_ok;
    float bore[3];       /* the gunsight / HUD combiner direction from the eye (cockpit frame, unit) */
    int first_person;    /* cockpit / HUD view (sound, rumble) */
} crk_view;
/* passes the glass and the view to the generator's thread (the hit events, the impacts, creep); returns the number of
   impacts (0: nothing to draw) */
int ps2_crk_update(crk_glass *g, const crk_view *v);
/* GPU data: a float part (header, impacts, segments: vec4s) and a uint part (grid cells, segment indices) */
#define CRK_MAX_STARS 16u
#define CRK_MAX_SEGS  4096u
#define CRK_OCC_CELLS 65536u                     /* the occupancy grid: a 16-bit impact mask a cell (2 a uint) */
#define CRK_MAX_U     (CRK_OCC_CELLS / 2u + 49152u)
#define CRK_HDR       6u
#define CRK_F_VEC4    (CRK_HDR + CRK_MAX_STARS * 8u + CRK_MAX_SEGS * 2u)
void ps2_crk_set_sun(const float *S, const float *rgb);    /* the sun (cockpit frame) and its colour (ps2_video.c) */
uint32_t ps2_crk_gpu_version(void);               /* bumps when the impacts / segments change */
/* writes the float part (vec4s) and the uint part; full = 0: the header only (the rest unchanged since version);
   returns 1 when it wrote it all (0: the generator was busy -- only the header) */
int ps2_crk_gpu_write(float *f4, uint32_t *u, int full);
int ps2_crk_on(void);                             /* ACZ_VR_CANOPY_CRACKS > 0 */
float ps2_imm_debris_look(void);                  /* ACZ_IMM_DEBRIS_LOOK 0..2 (bug #260): what a debris pass leaves, visible */
int ps2_crk_boot_on(void);                        /* (the start's value: what a cockpit's load makes for it) */
void ps2_crk_menv_reload(void);                   /* the NEXT MISSION tier (ps2_menv.h): the game thread, at the edge */
int ps2_crk_ab_off(void);                         /* ACZ_VR_CANOPY_CRACK_AB: the off half of the in-run A/B */
int ps2_crk_count(void);                          /* impacts now */
int ps2_crk_hits(void);                           /* ... of them the hits' cracks, not debris chips (as of the last step) */
int ps2_crk_grit(void);                           /* ... of them grit pits from a kill's smoke (chips, bug #189) */

/* sound (the audio thread: mixed into the immersion layer) and rumble (the renderer's gamepad) */
void ps2_crk_mix(int16_t *buf, int frames);
void ps2_crk_rumble(uint64_t now_ns, float *lo, float *hi);

#ifdef __cplusplus
}
#endif
#endif
