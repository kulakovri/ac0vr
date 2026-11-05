#ifndef PS2_VK_H
#define PS2_VK_H

#include "ps2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ps2_vk_vertex {
    float x, y, z;
    float r, g, b, a;
    float s, t, q;
    float fog;
    u32 round_uv;
    float xr_dx;     /* synchronous stereo: right-eye x shift in PS2 pixels (left eye uses -xr_dx); not read by
                        shaders, the renderer applies it per eye pass. 0 for 2D/HUD and off-screen targets. */
    float xr_iw;     /* 1 / view depth (game units) of a world vertex, for reprojecting a frame to another camera
                        (frames between game frames); 0 = at infinity, < 0 = never reprojected (HUD, 2D, cockpit) */
} ps2_vk_vertex;

#define PS2_VK_RUV_SPRITE (1u << 29)

typedef struct ps2_vk_state {
    u32 fb_w, fb_h;
    u32 fb_psm;
    u32 fb_mask;
    int shuffle_rg;
    u32 shuffle_alpha;
    s32 scissor[4];

    u32 ztst;
    int zwrite;

    int abe;
    u32 alpha_a, alpha_b, alpha_c, alpha_d, alpha_fix;

    int tme;
    u32 tex_index;
    float tex_w, tex_h;
    u32 tfx;
    int tcc;

    u32 rt;
    u32 tex_rt;
    int tex_depth;
    int tex_self;
    int fst;
    u32 tex_lod;
    int tex_point;

    u32 wms, wmt;
    u32 minu, maxu, minv, maxv;

    int ate;
    u32 atst, aref;
    u32 afail;
    int zte;

    int fge;
    u32 fogcol;
    int fba;
    int date, datm;
    u32 zrt;
    int zcopy;
    int colclip;
    u32 emitter;
    int native_region;
} ps2_vk_state;

enum { PS2_VK_POINTS = 0, PS2_VK_LINES = 1, PS2_VK_TRIANGLES = 2 };

int  ps2_vk_enabled(void);
int  ps2_vk_closed(void);

void ps2_vk_draw(int topology, const ps2_vk_state *st,
                 const ps2_vk_vertex *v, int n);

enum { PS2_VK_NATIVE_NONE = 0, PS2_VK_NATIVE_SUN_FLARE = 1,
       PS2_VK_NATIVE_SUN_GHOST = 2,
       PS2_VK_NATIVE_2D = 3 };

void ps2_vk_draw_2d(int topology, const ps2_vk_state *st,
                    const ps2_vk_vertex *v, int n);
typedef struct ps2_vk_native {
    u32 pass;
    u32 rt;
    u32 zrt;
    u32 tex;
    float tex_w, tex_h;
    int tex_point;
    u32 wrap;
    float box[4];
    float sun_z;
    s32 scissor[4];
} ps2_vk_native;

void ps2_vk_native_draw(const ps2_vk_native *nd, const ps2_vk_vertex *v, int n);

typedef struct ps2_vk_mesh_block {
    float m[16];
    float c[16];
    float l[12];
    float r[16];
    float e[12];
    float fog0, fog1;
    float zscale, zmax;
    float xoff, yoff;
    float spec_alpha;
    float xr_k;        /* stereo (set by the renderer): eye shift = -xr_k / max(w, xr_wmin) PS2 pixels; 0 = none */
    u32 mode;
    u32 pass;
    u32 clip;
    u32 pad1;
    u32 prog;
    float xr_wmin;
    u32 xr_reproj;     /* frames between game frames: 0 never (cockpit, 2D), 1 as static world (camera
                          extrapolation), 2 own motion (m extrapolated from mp, the same object one game frame earlier) */
    u32 pad4;
    float p[16];
    float mp[16];      /* xr_reproj 2: the object's m in the previous game frame */
} ps2_vk_mesh_block;

enum { PS2_VK_NATIVE_MESH = 4 };

/* The rear-view mirrors' live map (ps2_video.c mlv_*, ACZ_VR_COCKPIT_MIRRORS_LIVE): the graphics worker's side
   (rn_vp_model.c mlv_batch). The rear camera of the game frame being recorded: head view -> rear view (rows of T,
   translation t, game units), the game frame's projection (hf, hc: x = hc0 + hf0 X / -Z, y = hc1 - hf1 Y / -Z, GS
   pixels) and the map's (rf, rc, map pixels), the rear frustum's planes (n.v + d >= 0 inside, rear view), the range,
   the coarse test's radius, the own aircraft's margin and the near plane (game units); identity 1 / 2: the test modes
   (the head camera itself; 2 = the rows as they come). NULL: no map for this game frame. */
typedef struct ps2_vk_mlv_cam {
    float T[9], t[3];
    float hf[2], hc[2], rf[2], rc[2];
    float pl[5][4];
    float range, rmax, own, near_u;
    float wR[9], we[3];        /* the head camera in the world (world = wR view + we, game units): the height test */
    float ground, agl;         /* ground objects (nearer than agl to the ground, units): their range (0 = left out) */
    int identity;
    u32 seq;                   /* the game frame */
    u32 gmax;                  /* ground objects' strips a frame at most */
} ps2_vk_mlv_cam;
const ps2_vk_mlv_cam *ps2_vk_mlv_cam_now(void);
int  ps2_vk_mlv_room(u32 n_verts, u32 n_indices, u32 n_draws, int same_object);
void ps2_vk_mlv_mesh(const ps2_vk_state *st, const ps2_vk_mesh_block *b, const u8 *recs, u32 n_verts,
                     const u32 *indices, u32 n_indices, int cull, int pass, int same);
int  ps2_gs_native_state_ad(const u64 *ad, u32 n, u32 prim, ps2_vk_state *st, float *xoff, float *yoff, float *zmax);

int  ps2_vk_mesh_available(void);
int  ps2_vk_mesh_material_ok(const ps2_vk_state *st);
int  ps2_vk_mesh_store(const u8 *records, u32 n_verts, const u32 *indices,
                       u32 n_indices, u32 *vbase, u32 *ifirst);
int  ps2_vk_mesh_room(u32 n_verts, u32 n_indices, u32 n_draws);
enum { PS2_VK_MESH_CULL = 1, PS2_VK_MESH_GSFRAG = 4 };
void ps2_vk_draw_mesh(const ps2_vk_state *st, const ps2_vk_mesh_block *b,
                      u32 vbase, u32 ifirst, u32 n_indices, int cull);
void ps2_vk_draw_mesh_topo(const ps2_vk_state *st, const ps2_vk_mesh_block *b,
                           u32 vbase, u32 ifirst, u32 n_indices, int cull,
                           int topology);
extern void (*ps2_vk_before_record)(void);

u32  ps2_vk_texture(u64 key, u32 hash, const u8 *rgba, u32 w, u32 h);
u32  ps2_vk_texture_scaled(u64 key, u32 hash, const u8 *rgba, u32 w, u32 h, u32 scale);
u32  ps2_vk_texture_scale(u32 idx);
int  ps2_vk_texture_is(u32 idx, u64 key, u32 hash);

u32  ps2_vk_texture_native(u32 src, u32 x, u32 y, u32 w, u32 h);
u32  ps2_vk_texture_native_find(u64 key, u32 hash, u32 w, u32 h);
u32  ps2_vk_texture_native_rgba(u64 key, u32 hash, const u8 *rgba, u32 w, u32 h);
const u8 *ps2_vk_texture_pixels(u32 idx);
void ps2_vk_texture_size(u32 idx, u32 *w, u32 *h);

void ps2_vk_invalidate(u32 base, u32 size);

#define PS2_VK_PRESENT_BLANK 0xFFFFFFFFu
int  ps2_vk_present(u32 disp_x, u32 disp_y, u32 disp_w, u32 disp_h, u32 rt);

extern int ps2_vk_lockstep;

int  ps2_vk_screenshot(const char *path);

extern float ps2_vk_display_aspect;

void ps2_vk_stats(u64 *frames, u64 *draws, u64 *verts, u64 *textures);

/* Enclosure (tunnel_light 2026-11-02, bug #267; ps2_video.c): what of the sun and the sky reaches the eye, from a game
   module's rays through its own world (Zero: zero_vr.c, the game's segment test). The game module publishes the raw
   values once a game frame; the renderer smooths them in display time and its sun- and sky-lit effects follow. */
int   ps2_vk_enclosure_wanted(void);                 /* ACZ_VR_TUNNEL_LIGHT > 0: worth casting the rays */
void  ps2_vk_enclosure_set(float sun_vis, float sky_open, const float eye_units[3], float units_per_m);
float ps2_vk_enclosure_sun(void);                    /* the sun at the eye: 1 open .. 0 hidden (smoothed, x strength) */
float ps2_vk_enclosure_closed(void);                 /* 0 open sky .. 1 enclosed (a tunnel, a roof) */
float ps2_vk_enclosure_sun_at(const float w_units[3]);    /* the same near the eye, 1 from ~600 m away */
float ps2_vk_enclosure_closed_at(const float w_units[3]);

#ifdef __cplusplus
}
#endif

#endif
