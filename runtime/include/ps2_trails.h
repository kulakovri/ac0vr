#ifndef PS2_TRAILS_H
#define PS2_TRAILS_H
/* Contrails, wing vapour, heat haze and missile smoke light (wishes #85 / #86 / #87; agent im_trails 2026-10-23).

   zero_trails.c (EE thread, ac5 only) follows the game's aircraft -- every aircraft the game updates goes through its
   burner setup 0x1386D0 once a frame (a0 render object, +4 its model; a2 / a3 the unit's position / angles vectors) --
   and the model's typed nodes (0x133410(model, type) lists, 64-byte node records at *(model+0xC), position at +0x20;
   type 0x63 = the engine nozzles "burner.nNN", 0x66 = the wing tips, 0x60 = the lights) and publishes one frame of
   state per game frame here (ps2_trails.c, in ac5 and gsreplay):
   - contrail points (world positions in game units with their birth time, formation opacity, lifetime and spread),
   - wing vapour (in hard pulls in humid air: flat ellipsoids over each wing's upper surface, drawn as their silhouette),
   - heat haze plumes behind the nozzles (near aircraft and the own afterburner),
   - the light the game's smoke sprites get (rn_vp_fx.c): the sun direction and the strength,
   - the cloud wake (ACZ_VR_CLOUD_WAKE, agent cl_wake 2026-10-29): leaving one of our clouds, an aircraft drags out a
     short veil of the cloud's droplets in its vortex wake, and in the humid shell around the cloud (or in a pull) its
     two vortex cores condense into thin threads; both sink with the wake, spread, break up and evaporate in seconds
     (kinds 1 and 2 below). Whether an aircraft is in cloud comes from a GPU probe of our clouds' density at its
     predicted position (ps2_clouds_wake.c, cloud_wake.comp; pt_wake_request / pt_wake_result below).
   ps2_video.c draws the first three after the world (before the 3D cockpit) in true stereo (pt_render_* below) and
   rn_vp_fx.c multiplies the smoke sprites' vertex colour (pt_smoke_light). */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PT_MAX_TRAILS 96u
#define PT_MAX_PTS    8192u
#define PT_MAX_PUFFS  256u
#define PT_MAX_HAZE   32u

typedef struct {
    float p[3];          /* world position (game units) */
    float birth_s;       /* game time of its birth (s) */
    float a0;            /* formation opacity 0..1 (the Schmidt-Appleman margin, engine power) */
    float life_s;        /* lifetime (s): seconds in dry air, minutes when ice-supersaturated (persistent) */
    float spread;        /* spreading rate (m/s of width) */
    float len_m;         /* distance along the trail from its start (m): a texture coordinate that stays with the air */
    float T;             /* the sun's transmittance through our clouds there (refreshed by zero_trails.c; 1 = none) */
    float dv[3];         /* drift (m/s, world): the wake's descent (cloud wake; 0 for contrails) */
    float t_stop;        /* the drift stops after this age (s): the vortex pair links and breaks up */
} pt_point;

typedef struct {
    uint32_t id;         /* stable while it grows */
    uint32_t first, n;   /* its points in pt_frame.pt (oldest first; the last is the nozzle now) */
    float width_m;       /* initial width (m) */
    float shadow;        /* (renderer) */
    uint32_t kind;       /* 0 contrail, 1 cloud wake veil (dragged cloud), 2 cloud wake vortex-core thread */
} pt_trail;

typedef struct {
    float p[3];          /* centre (game units) */
    float ax[3];         /* semi-axis along the airflow (game units) */
    float sp[3];         /* semi-axis along the span */
    float up[3];         /* semi-axis across the wing (thin: a sheet over the upper surface) */
    float alpha;         /* 0..1 */
    float seed;          /* noise seed (per aircraft and puff) */
} pt_puff;

typedef struct {
    float p[3];          /* the nozzle (game units) */
    float dir[3];        /* exhaust direction (unit, world) */
    float r;             /* nozzle radius (game units) */
    float len;           /* plume length (game units) */
    float k;             /* strength 0..1 (military power ~0.45, full afterburner 1) */
    float seed;
    uint32_t own;        /* the player's own aircraft */
} pt_haze;

typedef struct {
    uint32_t seq;        /* bumps per publish */
    uint32_t field;      /* game field (vblank count) of the publish */
    double now_s;        /* game time (s) of the publish */
    float upm;           /* game units per metre used for the sizes (ACZ_VR_UNITS_PER_M, 6) */
    float sun[3];        /* towards the sun (world), 0 = unknown */
    float rh;            /* the mission's humidity estimate at the aircraft (0..1) */
    uint32_t ntrail, npt, npuff, nhaze;
    pt_trail trail[PT_MAX_TRAILS];
    pt_puff puff[PT_MAX_PUFFS];
    pt_haze haze[PT_MAX_HAZE];
    pt_point pt[PT_MAX_PTS];
} pt_frame;

/* switches (ps2_trails.c, read once): strengths 0 = off */
typedef struct {
    float contrails;     /* ACZ_VR_CONTRAILS (1) */
    float vapour;        /* ACZ_VR_VAPOUR (1) */
    float haze;          /* ACZ_VR_HEAT_HAZE (1) */
    float smoke;         /* ACZ_VR_SMOKE_LIGHT (1) */
    float wake;          /* ACZ_VR_CLOUD_WAKE (1): the cloud wake (needs our clouds, ACZ_VR_CLOUDS near / all) */
    int ab;              /* ACZ_VR_TRAILS_AB=N: the in-run GPU A/B (blocks of N frames without the passes) */
    int debug;           /* ACZ_DEBUG_TRAILS=N */
} pt_cfg_t;
const pt_cfg_t *pt_cfg(void);

/* EE thread: publish a frame (copied under a lock) */
void pt_publish(const pt_frame *f);
/* renderer: the latest frame into out (only the used parts are copied, and only when it is newer than *seq, which is
   updated); 0 = none or stale (no publish for stale_s: menus, a pause) */
int  pt_latest(pt_frame *out, double stale_s, uint32_t *seq);

/* The cloud wake's probe (ACZ_VR_CLOUD_WAKE): zero_trails.c (EE thread) asks, once per game frame, for our clouds'
   density around up to PT_WAKE_MAX aircraft (where each will be when the answer comes back: the probe's latency is
   measured); ps2_clouds_wake.c (renderer thread) evaluates them on the GPU with the clouds' own density (cl_density,
   8 points of a cube around each) early in a game frame's command buffer and posts the mean when the frame slot comes
   round again (2-4 game frames later). */
#define PT_WAKE_MAX 32u
typedef struct {
    uint32_t key;        /* the aircraft (zero_trails.c: its position vector's address) */
    uint32_t field;      /* the game field of the request */
    float p[3];          /* where: world, game units */
    float r_m;           /* half size of the probe cube (m) */
} pt_wake_req;
void pt_wake_request(const pt_wake_req *r, uint32_t n);                  /* EE: this frame's requests */
/* renderer: the latest requests into out (0 = none, or stale: older than stale_s) */
uint32_t pt_wake_requests(pt_wake_req *out, uint32_t max, double stale_s);
void pt_wake_result(uint32_t key, uint32_t field, float dens, float dmax);   /* renderer: one answer */
/* EE: the latest answer for an aircraft: 1 = found (dens = mean density, dmax = the cube's densest corner, field = of
   its request) */
int  pt_wake_get(uint32_t key, float *dens, float *dmax, uint32_t *field);

/* the smoke light for rn_vp_fx.c (GS replay thread): published by zero_trails.c each frame */
typedef struct {
    float sun[3];        /* towards the sun (world); 0 = none (night, unknown) */
    float k;             /* ACZ_VR_SMOKE_LIGHT strength (0 = off) */
    float fwd, back;     /* brightness towards / away from the sun (multipliers at k = 1) */
    float shadow_k;      /* how much a full cloud shadow takes away */
    uint32_t seq;
} pt_smoke_t;
extern volatile pt_smoke_t pt_smoke;
/* the factor for a smoke particle at world position w (game units) seen from eye (game units); also returns the cloud
   shadow transmittance used (1 = none) through *T when T is not NULL */
float pt_smoke_light(const float w[3], const float eye[3], float *T);
/* statistics (rn_vp_fx.c): batches / particles relit */
extern uint64_t pt_smoke_batches, pt_smoke_parts;

/* the sun's transmittance through our clouds at world position w (game units) from the cloud shadow map's CPU copy
   (ps2_video.c, ACZ_VR_CLOUD_SHADOW_GROUND's strip readback); 1 = no shadow / no map */
float ps2_vk_cloud_sun_T(const float w[3]);
/* the same x the sun's visibility near the eye (tunnel_light, bug #267: a tunnel's roof, the terrain; ps2_video.c
   ps2_vk_enclosure_sun_at): the light the contrails and the vapour take */
float ps2_vk_sun_T(const float w[3]);
/* the sun's visibility near the eye alone (1 open .. 0 hidden; 1 from ~600 m away): the smoke light's lobe */
float ps2_vk_enclosure_sun_at(const float w_units[3]);

#ifdef __cplusplus
}
#endif
#endif
