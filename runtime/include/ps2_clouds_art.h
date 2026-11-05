/* ps2_clouds_art.h -- S5 (art): the game's cloud/sky/fog colours captured from its GS draws (ps2_clouds_art.c). */
#ifndef PS2_CLOUDS_ART_H
#define PS2_CLOUDS_ART_H

#include "ps2_vk.h"

#ifdef __cplusplus
extern "C" {
#endif

/* valid bits: set once a value was captured (then held while its source is out of view) */
enum {
    PS2_CLOUD_ART_LIT = 1u << 0,       /* lit = the impostors' LIGHT overlay colour (the base until one was seen) */
    PS2_CLOUD_ART_SHADOW = 1u << 1,    /* shadow = the DARK overlay colour (the base until one was seen) */
    PS2_CLOUD_ART_FOG = 1u << 2,       /* fog (GS FOGCOL) */
    PS2_CLOUD_ART_FOG_RANGE = 1u << 3, /* fog_near_m / fog_far_m (native mesh fog coefficients) */
    PS2_CLOUD_ART_SKY_TOP = 1u << 4,   /* sky_top (zenith if sky_top_exact, else the topmost visible dome ring) */
    PS2_CLOUD_ART_SKY_HOR = 1u << 5,   /* sky_hor (the dome's horizon ring) */
    PS2_CLOUD_ART_SUN = 1u << 6,       /* sun_dir_disc (world, from the sun disc and the frame's camera): a logged
                                          cross-check only -- C1 sun_dir comes from S1 (sky +0x240 env-set angles) */
    PS2_CLOUD_ART_INCLOUD = 1u << 7,   /* incloud (frame colour inside the cloud sprites, immersion patches) */
    /* this frame's own capture (not held): */
    PS2_CLOUD_ART_SEEN_LIT = 1u << 8, PS2_CLOUD_ART_SEEN_SKY = 1u << 9, PS2_CLOUD_ART_SEEN_SUN = 1u << 10,
    PS2_CLOUD_ART_SEEN_FOG = 1u << 11,
    PS2_CLOUD_ART_T = 1u << 12         /* game_t valid (both palette colours known) */
};

/* Colours: the game's 0..255 values / 255 (the space of the frame they are composited into; no gamma decode). */
typedef struct ps2_cloud_art {
    u32 frame, seq;                  /* GS frame (ps2_gs_frame_count, = dump field numbers), XR frame seq */
    u32 valid;
    /* the game's cloud shading (see ps2_clouds_art.c): base = mix(shadow, lit, game_t), then a LIGHT overlay of
       strength a_light and a DARK one of a_dark, offset by off_* (fractions of the impostor size) and scaled ov_scale */
    float lit[3];                    /* LIGHT palette colour (C1 lit) */
    float shadow[3];                 /* DARK palette colour (C1 shadow) */
    float base[3], game_t, a_light, a_dark, off_light[2], off_dark[2], ov_scale;
    float eff_light[3], eff_dark[3]; /* the brightest / darkest the impostor shows at full mask alpha */
    float view_fwd[3], cos_view_sun; /* the frame camera's forward (world) and its cosine to sun_dir_disc (-2 unknown) */
    float sun_view[3];               /* sun_dir_disc in the frame's view axes (x right, y up, z back); 0 unknown */
    float fog[3];                    /* FOGCOL */
    float fog_near_m, fog_far_m;     /* linear fog 0 -> 1 between these view depths (near may be < 0) */
    float fog0, fog1;                /* raw: F = clamp(fog0 + w fog1, 0, 255), w in game units (6 / m) */
    float sky_top[3], sky_hor[3], sky_below[3];
    int sky_top_exact, ndome;
    float haze[3], haze_fix;         /* horizon haze ring colour and its FIX blend factor */
    float sunq[3], sunq_a, sunq_px[2]; /* the tex-395 sun-direction quad at 4096 units (S1: colour file +0x30;
                                          NOT a cloud deck): vertex colour, alpha (/128), screen centre */
    int sunq_n;
    float sun_dir_disc[3];           /* world, unit, y up; cross-check of S1's sun_dir, not for C1 */
    float sun_rgb[3], sun_fix, sun_px[2];
    float incloud[3], incloud_density;
    int nimp;                        /* impostor fills seen this frame (up to 4 listed) */
    float imp_lit[4][3];
    int spr_n;                       /* screen cloud sprite draws, their alpha-weighted vertex RGB (1 = x1) */
    float spr_rgb[3], spr_cover, spr_near_m;
} ps2_cloud_art;

/* art ramp of a cloud kind (config_zero/cloud_art.json), for the far march (S2) and the near volume (S3).
   Zero's cloud shading, measured (missions 1, 3, 7, 13 identical; ps2_clouds_art.c): with c = cos(view direction,
   sun) and D(q) the cloud's opacity (0..1) at a point q,
       colour = mix(C1 shadow, C1 lit, t(c))                          the whole-cloud tone
       colour = mix(colour, C1 lit,    a_light(c) * D(p - s_light * sun))   lit except a rim away from the sun
       colour = mix(colour, C1 shadow, a_dark(c)  * D(p + s_dark  * sun))   shadowed where cloud lies sunward
   The game does this in screen space: copies of the cloud's mask x rim_scale, shifted along the sun's screen
   direction by rim_light / rim_dark of the impostor (the puffs fill ~0.6 of it, so s_light ~ 0.23 and s_dark ~
   0.28 of a cloud's diameter). Curves: PS2_CLOUD_RAMP_N even samples over c = -1..+1. shape[] = C1 user[0] for B's
   density (base-noise period m, detail period m, erosion, billow weight; 0 = B's built-in default). */
#define PS2_CLOUD_RAMP_N 9
typedef struct ps2_cloud_ramp {
    float t_curve[PS2_CLOUD_RAMP_N], a_light_curve[PS2_CLOUD_RAMP_N], a_dark_curve[PS2_CLOUD_RAMP_N];
    float rim_light, rim_dark, rim_scale;    /* offsets along the sun (impostor units), copy scale */
    float cloud_frac;                        /* the puffs' share of the impostor (offset / cloud_frac = of a cloud) */
    float shape[4];                  /* C1 shape -> user[0]: base_period_m, detail_period_m, erosion, billow */
    float shade[4];                  /* C1 shade -> user[1]: base_darkening, thin_rim, dark_core, dark_copy */
    float near_art[4];               /* C1 near_art -> user[2]: S3's near-only art (its meaning; 0 = S3 defaults) */
    float incloud_t;                         /* C1 incloud without a usable measurement: mix(shadow, lit, incloud_t) */
    float ambient_top, ambient_base;         /* extra mix toward sky_top on tops / sky_below on bases (0 = game) */
    float fog_scale, fog_max, opacity_max, edge_softness;
    float near_fade_m[2], far_fade_m[2];
    char kind[16];
} ps2_cloud_ramp;

void ps2_clouds_art_draw(const ps2_vk_state *st, const ps2_vk_vertex *v, int n, int cls, u32 seq);
void ps2_clouds_art_mesh(const ps2_vk_state *st, const ps2_vk_mesh_block *b);
void ps2_clouds_art_patches(const u8 *before, const u8 *after, int npatch, int npix, float density);
int  ps2_clouds_art_get(ps2_cloud_art *out);
int  ps2_clouds_art_ramp(int mission, int env_type, ps2_cloud_ramp *out);
int  ps2_clouds_art_ramp_sun(int mission, int env_type, float sun_y, ps2_cloud_ramp *out);   /* sun_y < 0: night */
int  ps2_clouds_art_current(ps2_cloud_ramp *out);   /* the ramp the capture selected for this mission; 0 none yet */
/* the colour model packed as clamp(a + b c) coefficients (for CloudFrame user slots):
   [0] = t a, t b, a_light a, a_light b; [1] = a_dark a, a_dark b, s_light, s_dark (rim offsets along sun_dir, as
   fractions of a cloud's diameter); [2] = far fade start m, far fade end m, opacity_max, edge_softness */
void ps2_clouds_art_ramp_pack(const ps2_cloud_ramp *r, float pk[3][4]);
extern volatile float ps2_clouds_art_ramp_packed[3][4];   /* the current mission's, updated by the capture */

#ifdef __cplusplus
}
#endif

#endif
