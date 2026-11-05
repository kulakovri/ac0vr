// ps2_clouds.h - our own 3D clouds for the Zero VR port (CLOUDS_3D_RESEARCH_2026-10-20.md section 8): the contracts
// between the game-data side (S1 zero_clouds.c, S5 ps2_clouds_art.c), the cloud renderers (S2 ps2_clouds_far.c +
// cloud_far.comp, S3 ps2_clouds_near.c + cloud_near.comp) and the pipeline (S0/S4 ps2_clouds.c, cloud_comp.frag, the
// ps2_video.c touch points). The GLSL side of the same contracts is runtime/shaders/cloud_common.glsl.
//
// Frame flow (renderer thread, ps2_video.c render_list, head-tracked XR frames only, in-between frames included):
//   - at the game's first cloud-impostor sprite: [far] the far update (S2 hook, once per command buffer) and the far
//     composite (cloud_comp.frag, mode far: the direction map sampled by view direction, stopped at the scene depth);
//   - after the game's last cloud sprite: [near] the near march (S3 hook, per eye) and the near composite (nearest-depth
//     upsample of the half-resolution near buffer).
//   The frame's rendering is closed for these passes and reopened by ps2_video.c afterwards (as cloud_patch_copy does),
//   so the sun's visibility chain (rt 13-16, later in the list), the particle box, the native cockpit and the HUD stay
//   on top in their usual order. Composite: dst.rgb = src.rgb + dst.rgb * src.a (src premultiplied, a = transmittance),
//   the target's alpha (the game's destination alpha) is never written.
//
// World space of every cloud shader ("cloud space"): METRES, Y UP, CAMERA-RELATIVE.
//   - The game's world (Zero, PS2) is y up with ~6 units per metre (ACZ_VR_UNITS_PER_M, the same value zero_vr.c
//     uses). Cloud space has the game's axes, divided by units_per_m, with the origin at the frame's centre eye.
//   - cloud space p  <->  game world g:  g = (origin_m + p) * units_per_m   (origin_m = ps2_cloud_view.origin_m, double)
//   - The CLOUD FRAME is the frame in which clouds stand still: the world minus the accumulated wind drift (C1
//     drift_m, x/z, continuous; the game's cloud list repeats every 131072 units, see C1). Its coordinates of a point
//     p are cloud_origin_m + p (C2 cloud_origin_m = origin_m - drift); y is the plain altitude. C2 rebase counts the
//     jumps of the cloud-frame origin (a wrap the drift did not absorb, a camera cut): history keyed on cloud-frame
//     positions is invalid across a change.
//   - Shaders never see the doubles. CloudFrame gives the cloud-frame origin as: origin.xz modulo CL_ORIGIN_WRAP_M
//     (60 km; every noise period in x/z must divide 60000 m: 60, 250, 600, 4000, 20000 all do), origin_split = hi/lo
//     (x_hi + x_lo, z_hi + z_lo = the full value, for hashes over the whole map), origin_4k = modulo 4000 m (the base
//     noise period; the most precise), and wx.xy = the weather map's uv offset. cl_world(p) / cl_world4k(p) add p.
//   - Directions (sun, view rays, the far map) are plain world directions (unit, y up); they need no origin.

#ifndef PS2_CLOUDS_H
#define PS2_CLOUDS_H

#include <stdint.h>
#include <vulkan/vulkan.h>
#include "ps2_vk.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------------------------------------------
   C1, game -> renderer (ps2_cloud_env). Double-buffered, published once per game frame with a sequence number.
   Written by S1 (data) and S5 (colours), possibly from different threads: each writer edits the staging copy under
   ps2_cloud_env_edit() / ps2_cloud_env_done(), and one of them (S1, once per game frame) publishes it. The renderer
   reads the last published copy (ps2_cloud_env_read). Fields a writer does not know keep their last value; the
   valid bits say which groups are real. Missing groups fall back to built-in defaults in ps2_clouds.c (a broken
   cumulus layer 1200-2400 m, the benchmark's colours), so S2-S4 can run before S1/S5 deliver.
   --------------------------------------------------------------------------------------------------------------- */
enum {
    CL_ENV_PROFILE = 1,       /* base_m, top_m */
    CL_ENV_WEATHER = 2,       /* coverage, type, transp, precip */
    CL_ENV_LIST = 4,          /* ncloud, cloud[] */
    CL_ENV_COLOURS = 8,       /* lit, shadow, fog, sky_top, sky_hor, fog_near_m, fog_far_m */
    CL_ENV_SUN = 16,          /* sun_dir */
    CL_ENV_INCLOUD = 32,      /* in_cloud (cloudfeel's value) */
    CL_ENV_INCLOUD_COL = 64,  /* incloud[] */
    CL_ENV_SHADOW_A = 128,    /* shadow_a */
    CL_ENV_SKY_BELOW = 256,   /* sky_below[] */
    CL_ENV_WIND = 512,        /* wind_mps[] */
    CL_ENV_COVMAP = 1024,     /* cov_map, cov_w, cov_h, cov_cell_m, cov_origin_m */
    CL_ENV_DRIFT = 2048,      /* drift_m[] */
    CL_ENV_SHAPE = 4096,      /* shape[] */
    CL_ENV_NEAR_ART = 8192,   /* near_art[] */
    CL_ENV_SHADE = 16384,     /* shade[] */
    CL_ENV_ART_K = 32768,     /* art_k[] */
    CL_ENV_PRECIP = 65536     /* precip_amt, precip_snow, precip_lo_m, precip_hi_m (the optics' rain volume) */
};
#define CL_MAX_CLOUDS 400
#define CL_CLOUD_SPRITE 1           /* cloud[] carries height_m and sprite_m (S1 fills them when this is defined) */
typedef struct {
    uint32_t seq, valid;            /* valid bits: CL_ENV_* */
    float base_m, top_m;            /* cloud band, m of altitude: P10 / P90 of (Y -+ 0.675 height_m) over the list (S1) */
    float coverage, type, transp;   /* coverage: the horizontal fraction covered by cloud bodies under the game's stage
                                       mask (M1 0.20, M3 0.012); type 0 stratus .. 1 cumulus from the list's median R;
                                       transp = 1 - alpha / 128 */
    float coverage_unmasked;        /* the same without the stage mask (M1 0.57, M3 0.07): the procedural default
                                       weather uses it (valid with CL_ENV_WEATHER; 0 = use coverage) */
    float precip, in_cloud;         /* sky +0xB8C; cloudfeel's in-cloud value 0..1 */
    float sun_dir[3];               /* world (cloud space axes), unit, towards the sun: S1 from the env-set sun angles
                                       (sky +0x240; valid with the sun off screen), S5's screen-disc value cross-checks */
    /* Colours: slot-0 space (PS2 display value / 255, no gamma decode): the game's colours and our composite both
       live in slot 0. A shader that wants physically linear maths decodes with pow(c, 2.2) and re-encodes before
       the composite. From the game's draws (S5), held per mission where noted. */
    float lit[3];                   /* the game's raw LIGHT palette colour (per mission) */
    float shadow[3];                /* the game's raw DARK palette colour (the zenith sky colour, per mission); cl_shade
                                       does the mixing */
    float shadow_a;                 /* the dark-rim (self-shadow) strength, ~0.85 */
    float fog[3], sky_top[3], sky_hor[3];
    float sky_below[3];             /* the ground-side haze ring below the horizon (the far map covers the sphere) */
    float incloud[3];               /* the frame colour inside the game's overcast */
    float fog_near_m, fog_far_m;    /* fog_near_m can be negative (mission 1: -1290 .. 8590 m) */
    float shape[4];                 /* cloud shape (S5 per mission; -> CloudFrame user[0]): base-noise period (m),
                                       detail period (m), detail erosion 0..1, billow weight 0..1; both periods must
                                       divide 60000 m. All 0 = the built-in defaults (cl_shape_params) */
    float shade[4];                 /* shared near/far shading (S5 per mission; -> CloudFrame user[1], cl_shade_params):
                                       base darkening, light rim, dark core, DARK-copy strength; all 0 = defaults
                                       (0.15, 0.5, 0.25, 1) */
    float near_art[4];              /* near-only art (S5 per mission; -> CloudFrame user[2]): S3 defines the meaning;
                                       all 0 = S3's defaults */
    float art_k[4];                 /* S5's per-mission constants (-> CloudFrame user[3]; cl_shade reads them as t_a, t_b,
                                       aL_a, aD_a, zeros = the mission-1 fits) */
    float wind_mps[2];              /* wind, world x and z (m/s) */
    float drift_m[2];               /* wind drift, world x and z (m) = wind x (u32 obj+0xD0) / 60 / units_per_m (S1):
                                       continuous, never wrapped; jumps only at a mission start or retry.
                                       cloud frame = world - drift */
    /* The game's per-field cloud list (up to 400; found by the cloudfeel agent, published by S1). Clouds the game drops
       are not published. Into a renderer's cloud space (C2 cloud_origin_m = eye - drift):
         p.xz = pos_m.xz + (origin_m.xz - view.cloud_origin_m.xz),   p.y = pos_m.y - view.origin_m.y */
    double origin_m[3];             /* CLOUD-FRAME reference point: x/z re-centred on the camera when the camera is more
                                       than 2 km from it; y = 0 */
    float list_period_m;            /* the list repeats every 131072 units = 21845.3 m in x and z (S2 tiles by it) */
    uint32_t ncloud;                /* 0 if the game has no per-cloud world positions */
    uint32_t list_seq;              /* bumps on a re-centre, a membership change or an opacity step (the splat re-runs) */
    struct {
        float pos_m[3];             /* x/z relative to origin_m (the copy of the periodic list nearest to origin_m);
                                       y = altitude (m) */
        float radius_m;             /* the visible body's horizontal semi-axis (m). The body is an ellipsoid with the
                                       profile full to 0.45 and gone at 0.9 of its semi-axes; its half density sits on the
                                       game impostor's 50 % alpha (0.667 x sprite_m x the variant's build width) */
        float height_m;             /* its vertical semi-axis (m), 0.262 x sprite_m: the game's clouds are wide and flat */
        float sprite_m;             /* the game's screen sprite half-size S (m): the quad that carries the whole 128 x 128
                                       impostor (S = size x 2304 x scale / 206.6 units; B's sprite fit, M1) */
        float opacity;              /* the game's sprite alpha / 128 without its near fade: coverage at that copy's
                                       position x the far fade at its distance from the camera (full to far / 2, 0 at
                                       far = the stage visibility, M1 8533 m, M13 5973 m), 0..1 in steps of 1/64 */
        uint32_t variant;
    } cloud[CL_MAX_CLOUDS];
    /* The game's coverage map: S1's own 64 KB copy, u8 [z][x], 256 x 256, periodic, bilinear, sample (0, 0) at
       cov_origin_m, cell 341.3 m. Safe to read from any thread while CL_ENV_COVMAP is set. */
    const void *cov_map;
    uint32_t cov_w, cov_h, cov_fmt; /* 256, 256, 0 (u8) */
    float cov_cell_m;               /* metres per cell (2048 units / units_per_m = 341.3) */
    double cov_origin_m[2];         /* THIS frame's cloud-frame x, z (m) of sample (0, 0): the map is anchored to the
                                       world's geography, not to the clouds, so this moves with the drift every frame
                                       (world position - drift) */
    /* The stage's precipitation as a volume (S1, valid with CL_ENV_PRECIP; the optics' rainbow, ps2_clouds_optics.c):
       precip above is the amount AT THE CAMERA (0 above the band); these say what falls below the band wherever the
       camera is. precip_amt = the environment set's amount (halves A/B blended by sky +0xB30: 0.05 light rain .. 1
       downpour), precip_snow = 1 for snow (the set's kind, sky +0xB90 >= 0.4, as the canopy water reads it), 0 rain;
       precip_lo_m / precip_hi_m = the band (m of altitude, sky +0x10 / +0x14): full below lo, none above hi. */
    float precip_amt, precip_snow, precip_lo_m, precip_hi_m;
    /* The stage's cloud alpha (S1: cloud descriptor +0x6C = env +0x1FC, / 128; valid with CL_ENV_WEATHER, 0 =
       unknown). The game's impostor build scales every impostor's alpha by it (night_sky 2026-10-29: after the puffs
       each target is drawn onto itself with vertex colour (0, alpha / 2, 0): M11 alpha 64 -> G 32, M7 84 -> G 42, 128
       -> 64 = x1), so its clouds are that much more transparent on screen: M11 0.50, M7 0.66, M2 / M6 / M8 0.75, M12
       0.78, the other missions 1. ps2_clouds_far.c scales the drawn cloud list by it (ACZ_VR_CLOUD_IMP_ALPHA); transp
       stays 0 (it is the drop threshold's input only). */
    float imp_alpha;
} ps2_cloud_env;

ps2_cloud_env *ps2_cloud_env_edit(void);        /* lock + the staging copy (any thread) */
void ps2_cloud_env_done(void);                  /* unlock */
void ps2_cloud_env_publish(uint32_t seq);       /* staging -> front buffer, seq (0: an internal counter) */
int  ps2_cloud_env_read(ps2_cloud_env *out);    /* renderer: the last published copy; 0 if none was published yet */

/* ---------------------------------------------------------------------------------------------------------------
   C2, view per drawn frame (ps2_cloud_view), in-between frames included. Filled by S4 in ps2_video.c from the
   camera records (ps2_xr_camera_of: head-turned centre camera of the game frame), the extrapolation of in-between
   frames (xr_extrap_params), the stereo shift (ps2_xr_shift_of) and the frame's GS Z format.
   --------------------------------------------------------------------------------------------------------------- */
typedef struct {
    uint32_t valid;
    uint32_t seq;                   /* XR sequence number of the game frame */
    uint32_t inbetween;             /* 1: an extrapolated frame between game frames (smooth mode) */
    float alpha;                    /* fraction of a game frame extrapolated (0 for game frames) */
    double origin_m[3];             /* the centre eye in world metres (game world / units_per_m): cloud-space origin */
    double cloud_origin_m[3];       /* the centre eye in the cloud frame (x, z: origin_m - C1 drift; y = origin_m y);
                                       filled by ps2_clouds.c before the hooks */
    uint32_t rebase;                /* increments whenever the cloud-frame origin jumps (> 2 km between frames) */
    float eye_m[2][3];              /* left / right eye, cloud space (relative to origin_m) */
    float cam_x[3], cam_y[3], cam_z[3];   /* the view's axes in world (x right, y up, z back = -forward); both eyes */
    float tan_l, tan_r, tan_u, tan_d;     /* image tangents (symmetric in this renderer: -t, +t, +t, -t) */
    float proj[4];                  /* target pixels: fx, fy, cx, cy; view ray of pixel centre (x, y) is
                                       ((x - cx) / fx, -(y - cy) / fy, -1) in view axes */
    float img[4];                   /* the eye image in the target: x0, y0, w, h (pixels) */
    float vis[2][4];                /* per eye: the part the headset shows (the native per-eye FOV crop of ps2_xr.c),
                                       target pixels x0, y0, x1, y1; = img where unknown */
    uint32_t target_w, target_h;    /* full target size (pixels) */
    float depth_a, depth_c, depth_mh, depth_zmax;   /* view depth (game units) = mh / (a (z zmax + c)), z = D32 value */
    float units_per_m;              /* ACZ_VR_UNITS_PER_M (6) */
    float half_ipd_m;               /* eye_m[1] = +half_ipd_m cam_x, eye_m[0] = -half_ipd_m cam_x */
    double time_s;                  /* display time of the frame (s, monotonic) */
} ps2_cloud_view;

/* ---------------------------------------------------------------------------------------------------------------
   C3, images (created by ps2_clouds.c; all kept in VK_IMAGE_LAYOUT_GENERAL, so writers and readers only need memory
   barriers, which ps2_clouds.c issues around every hook call).
   - cloud_dir        RGBA16F N x N, N = ACZ_VR_CLOUD_FAR_RES (default 2048): full-sphere octahedral, y up (the upper
                      hemisphere is the inner diamond), cl_dir_uv / cl_uv_dir in cloud_common.glsl. RGB = premultiplied
                      in-scattered colour, A = transmittance. Eye-independent: both eyes sample the same texel.
   - cloud_dir_depth  R16F N x N: transmittance-weighted mean distance (m); 65504 where transmittance > 0.99.
   - cloud_dir_prof   RGBA16F N x N (ACZ_VR_CLOUD_FAR_SPLIT, cl_split 2026-10-30, bug #250): the opacity profile along the
                      texel's ray, so the composite can show the part of a cloud in front of a scene pixel (trueSKY-based games
                      split its cloud image into near and far parts at the geometry depth): x = the first distance with
                      cloud, y / z / w = the distances where the accumulated transmittance falls to 0.8 / 0.5 / 0.2
                      (65504 = never). Written with cloud_dir by the far march.
   - cloud_shaft      RGBA16F S x S (ACZ_VR_CLOUD_SHAFTS_RES, 512), full-sphere octahedral like cloud_dir (cl_dir_uv, its
                      own size): light shafts in the haze -- the haze's signed shadow excess in front of the four
                      distances CloudFrame shaft[0] along the texel's ray (cloud_shafts.comp; ps2_clouds.c updates a few
                      rows a frame).
   - cloud_near       RGBA16F, 2 layers (left, right eye), half the target per axis ((W+1)/2 x (H+1)/2): texel (i, j)
                      of layer e covers target pixels (2i..2i+1, 2j..2j+1) of eye e. Same RGBA meaning.
   - cloud_near_depth R16F, same layout: the ray distance (m) the texel's march was clipped at, min(scene, range);
                      the composite's nearest-depth upsample compares it with the full-resolution scene distance.
   - with ACZ_VR_CLOUD_NEAR_REUSE (nx_cloudreuse, 2026-10-25; ps2_clouds_near.c), written by game frames, read by the
     in-between frames after them (the history):
     cloud_near_far   RGBA16F, same layout: the march's part beyond its first distance tier (60 m): premultiplied
                      colour and transmittance of [60 m, clip] alone (total = near + T_near x far);
     cloud_near_par   RGBA16F, same layout: x = the contribution-weighted mean of 1 / distance over that part (1/m; the
                      depth it reprojects by), y = the weighted standard deviation of 1 / distance, z = the clip
                      distance (m), w = 0.
   - weather          RGBA8 CL_WEATHER_RES^2 covering CL_WEATHER_M, sampled by CLOUD-FRAME position with REPEAT
                      (toroidal): R coverage (the blob's horizontal profile, 1 at the centre, 0 at the radius), G local
                      base and B local top (0..1 within C1 [base_m, top_m]), A density (cloud[].opacity x C1 transp).
                      Type stays global (C1 type). Content: the 20 km window around the camera, refreshed toroidally by
                      S2's weather pass (cloud_far.comp PASS_WEATHER: the splat of cloud[], re-run when the window moves
                      a texel or list_seq changes). Until then ps2_clouds.c writes a periodic procedural default
                      (G 0, B 1, A 1).
   - noise_base       RGBA8 128^3 (R Perlin-Worley, GBA Worley octaves), noise_detail RGBA8 32^3 (Worley); periods
                      from C1 shape (defaults in cl_shape_params). Generated once (cloud_noise.comp, after
                      sebh/TileableVolumeNoise), full mip chains (8 / 6 levels); cl_density_ws picks the level by step.
   - cloud_shadow     RGBA16F M x M (ACZ_VR_CLOUD_SHADOW_RES, 256) over CL_WEATHER_M, toroidal like the weather map:
                      the shown cloud shadow map (ps2_clouds_shadow.c; layout and lookup in cloud_shadow.glsl). Bound
                      in set 0 (CL_B_SHADOW) for every cloud shader; CloudFrame shd[3] carries its parameters (s0.w = 0:
                      no map). Its build ring (3 layers) is the shadow module's own set 1.
   --------------------------------------------------------------------------------------------------------------- */
#define CL_ORIGIN_WRAP_M 60000.0    /* x/z of the origin go to shaders modulo this (all noise periods divide it) */
#define CL_WEATHER_RES 512u
#define CL_WEATHER_M 20000.0f
#define CL_NOISE_BASE_RES 128u
#define CL_NOISE_DETAIL_RES 32u
#define CL_NOISE_BASE_MIPS 8u       /* full mip chains (linear blits after the generator); sampled with LINEAR mips */
#define CL_NOISE_DETAIL_MIPS 6u
#define CL_NOISE_BASE_M 4000.0f
#define CL_NOISE_DETAIL_M 250.0f
#define CL_NEAR_LAYERS 2u
#define CL_DEPTH_NONE 65504.0f      /* the R16F depth of "no cloud" */

/* Descriptor set 0 of every cloud shader ("common set"; the GLSL declarations are in cloud_common.glsl) */
enum {
    CL_B_FRAME = 0,         /* uniform buffer (dynamic offset): CloudFrame (C1 + C2 + settings) */
    CL_B_NOISE_BASE = 1,    /* sampler3D, linear, repeat */
    CL_B_NOISE_DETAIL = 2,  /* sampler3D, linear, repeat */
    CL_B_WEATHER = 3,       /* sampler2D, linear, repeat */
    CL_B_SCENE_DEPTH = 4,   /* sampler2DArray, nearest: the frame's D32 depth (layer = eye; 1 layer in two-pass) */
    CL_B_DIR_IMG = 5,       /* image2D rgba16f (storage) */
    CL_B_DIR_DEPTH_IMG = 6, /* image2D r16f (storage) */
    CL_B_NEAR_IMG = 7,      /* image2DArray rgba16f (storage) */
    CL_B_NEAR_DEPTH_IMG = 8,/* image2DArray r16f (storage) */
    CL_B_DIR = 9,           /* sampler2D, linear, clamp (octahedral border texels are written by the far pass) */
    CL_B_DIR_DEPTH = 10,    /* sampler2D, linear, clamp */
    CL_B_NEAR = 11,         /* sampler2DArray, nearest */
    CL_B_NEAR_DEPTH = 12,   /* sampler2DArray, nearest */
    CL_B_WEATHER_IMG = 13,  /* image2D rgba8 (storage) */
    CL_B_NOISE_BASE_IMG = 14,   /* image3D rgba8 (storage, the generator) */
    CL_B_NOISE_DETAIL_IMG = 15, /* image3D rgba8 (storage) */
    CL_B_SHADOW = 16,       /* sampler2D, linear, repeat: the cloud shadow map (cloud_shadow.glsl) */
    /* ACZ_VR_CLOUD_NEAR_REUSE (declared in cloud_near.comp / cloud_near_reuse.comp, not in cloud_common.glsl) */
    CL_B_NEAR_FAR = 17,     /* sampler2DArray, nearest: cloud_near_far (the last game frame's march beyond 60 m) */
    CL_B_NEAR_PAR = 18,     /* sampler2DArray, nearest: cloud_near_par (its reprojection depth, spread, clip) */
    CL_B_NEAR_FAR_IMG = 19, /* image2DArray rgba16f (storage): cloud_near_far (game frames write it) */
    CL_B_NEAR_PAR_IMG = 20, /* image2DArray rgba16f (storage): cloud_near_par */
    /* ACZ_VR_CLOUD_FAR_SPLIT (cl_split 2026-10-30, bug #250; declared in cloud_common.glsl): cloud_dir_prof */
    CL_B_DIR_PROF = 21,     /* sampler2D, nearest, clamp (read with texelFetch) */
    CL_B_DIR_PROF_IMG = 22, /* image2D rgba16f (storage) */
    /* ACZ_VR_CLOUD_SHAFTS (cl_split 2026-10-30, bug #250): cloud_shaft (cl_shaft_tex in cloud_common.glsl; the storage image
       is declared in cloud_shafts.comp) */
    CL_B_SHAFT = 23,        /* sampler2D, linear, clamp */
    CL_B_SHAFT_IMG = 24,    /* image2D rgba16f (storage) */
    CL_B_COUNT = 25
};
#define CL_PUSH_BYTES 64u   /* push constants of every cloud pipeline (compute and fragment): free per shader */

/* Switches (ACZ_VR_CLOUDS etc., read once; see ps2_clouds.c and docs/ENV_REFERENCE.md rows in the NOTES) */
typedef struct {
    int near_on, far_on;            /* ACZ_VR_CLOUDS: 0 / near / far / all */
    int drop_game;                  /* ACZ_VR_CLOUD_GAME: drop the game's cloud sprites + impostor build (auto: far) */
    int far_res, far_steps, far_slice;   /* ACZ_VR_CLOUD_FAR_RES / _FAR_STEPS / _FAR_SLICE */
    int near_steps;                 /* ACZ_VR_CLOUD_NEAR_STEPS */
    float near_range_m;             /* ACZ_VR_CLOUD_NEAR_RANGE */
    int dump_field;                 /* ACZ_VR_CLOUD_DUMP */
    int debug;                      /* ACZ_DEBUG_CLOUDS: log every N frames */
    int test;                       /* ACZ_VR_CLOUD_TEST: synthetic test pattern in the C3 images (S4 checks) */
    int crop;                       /* ACZ_VR_CLOUD_CROP (1): composite only the part each eye's headset view shows
                                       (C2 vis); 0 = the whole image (eye dumps of the full image) */
} ps2_cloud_cfg;
const ps2_cloud_cfg *ps2_clouds_cfg(void);
/* F4 > Immersion (runtime A/B, not saved): which=0 far, 1 near, 2 keep the game's clouds, 3 cloud shadows, 4 optical
   phenomena; on < 0 only reads. Returns the state, or -1 when that part cannot be switched (ACZ_VR_CLOUDS / ACZ_VR_CLOUD_SHADOW did not
   start it) */
int ps2_clouds_toggle(int which, int on);
int ps2_clouds_enabled(void);       /* near or far on (cheap; no Vulkan needed) */

/* ---------------------------------------------------------------------------------------------------------------
   Renderer modules (S2 far, S3 near): what ps2_clouds.c gives them and what they implement.
   --------------------------------------------------------------------------------------------------------------- */
typedef struct {
    VkDevice dev;
    VkPhysicalDevice phys;
    VkPipelineCache cache;
    const char *shader_dir;
    VkDescriptorSetLayout common_layout;     /* set 0 */
    VkPipelineLayout common_pipe_layout;     /* set 0 + CL_PUSH_BYTES push constants (compute | fragment) */
    int multiview;                           /* render targets have 2 layers; 0: two eye passes */
    uint32_t dir_res;                        /* N of cloud_dir */
    VkImage dir, dir_depth, near_img, near_depth, weather;   /* (GENERAL layout) */
    uint32_t near_w, near_h;                 /* current near buffer size (changes with the target size) */
    uint32_t near_epoch;                     /* counts re-creations of the near images (history validity) */
    /* CLOUD DETAIL (perf6_clouds, ACZ_VR_CLOUD_DETAIL): this pass's near texel size in target pixels (2 = HIGH) and the
       part of the near images it uses (= near_w x near_h unless a smaller divisor made them larger: an A/B, a flip) */
    int near_div;
    uint32_t near_lw, near_lh;
} ps2_cloud_gpu;

/* what a hook records against: the frame's data (C1 copy, C2, the eye pass) */
typedef struct {
    const ps2_cloud_env *env;                /* C1 (defaults filled in where the valid bits are missing) */
    const ps2_cloud_view *view;              /* C2 */
    int eye;                                 /* near hook: -1 both layers (multiview), else 0 / 1 (two-pass) */
    uint32_t frame;                          /* renderer frame counter (in-between frames count) */
    int frame_index;                         /* frame in flight (0..2): for per-frame buffers of a module */
    int first_in_cmd;                        /* the first cloud pass of this command buffer */
} ps2_cloud_rec;

/* Helpers for modules (ps2_clouds.c). A module may have its own set 1 (extra_set, may be VK_NULL_HANDLE): the
   pipeline layout is then {common set 0, extra set 1} with the same push constant range, and stays compatible with
   the common set binding that ps2_clouds.c makes before calling the hook. Indirect dispatch, private buffers and
   their barriers are the module's own business; ps2_clouds.c only orders the C3 images around the hook. */
VkPipeline ps2_clouds_make_compute(const char *spv_name, VkDescriptorSetLayout extra_set, VkPipelineLayout *layout_out);
/* host_visible: 0 device-local, 1 upload (host-coherent), 2 readback (host-cached) */
int  ps2_clouds_make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, int host_visible,
                            VkBuffer *buf, VkDeviceMemory *mem, void **mapped);
void ps2_clouds_bind_common(VkCommandBuffer cmd, VkPipelineLayout layout);   /* rebind set 0 (compute) */

/* S2, ps2_clouds_far.c: the weather map writer. Initialised and called whenever clouds are on (near only too), at
   the first cloud pass of each command buffer, before the far / near hooks. Return 1 if the module owns the weather
   map (the S0 procedural default then stays off), 0 to keep the default. */
int  ps2_clouds_weather_init(const ps2_cloud_gpu *g);
int  ps2_clouds_weather_record(const ps2_cloud_gpu *g, VkCommandBuffer cmd, const ps2_cloud_rec *r);
/* S2, ps2_clouds_far.c: march (part of) cloud_dir + cloud_dir_depth. Called once per command buffer at the far point
   (in-between frames are their own command buffers). Return 1 if anything was dispatched. */
int  ps2_clouds_far_init(const ps2_cloud_gpu *g);
int  ps2_clouds_far_record(const ps2_cloud_gpu *g, VkCommandBuffer cmd, const ps2_cloud_rec *r);
void ps2_clouds_far_shutdown(const ps2_cloud_gpu *g);
/* S2: the far map's used size for this frame (<= dir_res; 1536 for overcast weather, ACZ_VR_CLOUD_FAR_RES_OVC):
   ps2_clouds.c writes it to CloudFrame sizes.x before the hooks; cl_dir_tex_uv scales the composite's uv by it */
uint32_t ps2_clouds_far_map_size(const ps2_cloud_env *e, uint32_t dir_res);
/* S3, ps2_clouds_near.c: march cloud_near + cloud_near_depth for rec->eye (-1: both layers). The scene depth is
   readable (CL_B_SCENE_DEPTH). Return 1 if anything was dispatched (0: the near composite is skipped). */
int  ps2_clouds_near_init(const ps2_cloud_gpu *g);
int  ps2_clouds_near_record(const ps2_cloud_gpu *g, VkCommandBuffer cmd, const ps2_cloud_rec *r);
/* ACZ_VR_CLOUD_NEAR_REUSE: 1 = game frames keep their march beyond 60 m as the history and in-between frames may
   reproject it (ps2_clouds.c then makes cloud_near_far / _par); ps2_clouds_near_reuse_ab(off) =
   ACZ_VR_CLOUD_NEAR_REUSE_AB's phase (ps2_clouds.c's cloud A/B family, once per frame) */
int  ps2_clouds_near_reuse_on(void);
int  ps2_clouds_near_reuse_active(void);    /* on, engaged, not in an A/B off block: in-between frames take the game
                                               frame's C1 */
void ps2_clouds_near_reuse_ab(int off);
void ps2_clouds_near_shutdown(const ps2_cloud_gpu *g);
/* CLOUD DETAIL (perf6_clouds 2026-11-03; ACZ_VR_CLOUD_DETAIL, ps2_clouds.c): the rungs below HIGH for the LOW / MEDIUM
   presets. A level's levers for the near volume (0 / < 0 in a field = HIGH's value, i.e. the ACZ_VR_CLOUD_NEAR_* switch
   or its built-in default); level 0 is HIGH. ps2_clouds_detail_levels fills up to max levels and returns their number
   (read at the near renderer's init); ps2_clouds_detail_cur = this frame's level (the configured rung; the A/B's block;
   PS2_PERF_AB_FLIP=clouddetail's re-draw). ps2_clouds_trim_off() = the near march's exact trim off for this pass
   (PS2_PERF_AB_FLIP=nearspec). */
#define CL_DETAIL_MAX 6
typedef struct {
    int div;                 /* near texel = div x div target pixels (2 = HIGH: half resolution per axis) */
    float step, grow, tier;  /* ACZ_VR_CLOUD_NEAR_STEP / _GROW / _TIER of this level (0 = HIGH's) */
    float detail;            /* ACZ_VR_CLOUD_NEAR_DETAIL of this level (< 0 = HIGH's) */
    int spec;                /* ACZ_VR_CLOUD_NEAR_SPEC of this level (< 0: the switch; measurements) */
} ps2_cloud_detail;
int ps2_clouds_detail_levels(ps2_cloud_detail *lv, int max);
int ps2_clouds_detail_cur(void);
int ps2_clouds_detail_used(int level);      /* this level is drawn in this session (the configured one, the A/B's, a flip's) */
int ps2_clouds_trim_off(void);
/* ps2_clouds_shadow.c: the cloud shadow map (ACZ_VR_CLOUD_SHADOW). init always runs with our clouds (the shown map is
   bound in set 0; with the shadows off it stays cleared and CloudFrame shd says "off"); begin once per command buffer
   before the CloudFrame copies are filled (ps2_clouds_shadow_ubo fills shd); record at the first cloud pass of the
   command buffer, after the weather pass (1 = written: the caller orders it before the far / near passes). */
int  ps2_clouds_shadow_init(const ps2_cloud_gpu *g);
void ps2_clouds_shadow_shutdown(const ps2_cloud_gpu *g);
VkImageView ps2_clouds_shadow_view(void);
int  ps2_clouds_shadow_frag(void);          /* the hook was used recently: the post-pass barrier covers fragment reads */
void ps2_clouds_shadow_begin(const ps2_cloud_env *e, const ps2_cloud_view *v, int frame_index, uint32_t frame);
void ps2_clouds_shadow_ubo(float shd[16], const ps2_cloud_view *v, const ps2_cloud_env *e);   /* CloudFrame shd[4] */
int  ps2_clouds_shadow_record(VkCommandBuffer cmd, const ps2_cloud_rec *r);
int  ps2_clouds_shadow_toggle(int on);      /* F4 A/B; -1 = not available */
void ps2_clouds_shadow_gov(int on);         /* the frame-time governor's level 1 (via ps2_clouds_far_gov): half slices */
void ps2_clouds_shadow_ab(int off);         /* ACZ_VR_CLOUD_SHADOW_AB: this frame without the map */
void ps2_clouds_shadow_debug(void);
/* ACZ_VR_CLOUD_DUMP: copy the shown map (after the shadow pass, with the far map's copies) and write it with the
   other dump files: <base>_shadow.pfm (RGBA as 4 PFM greys: top altitude, OD at D1, D2, column) + _shadowT.pgm */
void ps2_clouds_shadow_dump_copy(VkCommandBuffer cmd);
void ps2_clouds_shadow_dump_write(const char *base);
/* The cockpit (ps2_video.c, any thread): the sun's transmittance through our clouds at the aircraft, smoothed over
   ACZ_VR_CLOUD_SHADOW_COCKPIT_S (1.5 s; 1 = full sun; also with our clouds or the shadows off), and
   ACZ_VR_CLOUD_SHADOW_COCKPIT (how much of the direct sun a full shadow takes away, 0.5) */
float ps2_clouds_sun_at_eye(void);
float ps2_clouds_shadow_cockpit_k(void);
/* ps2_clouds_optics.c: optical phenomena (ACZ_VR_OPTICS; halo, glory, Brocken spectre, rainbow; cloud_optics.glsl).
   init/shutdown with our clouds; begin once per command buffer before the CloudFrame copies (reads the probe of this
   frame slot's last use, decides the gates); ubo fills CloudFrame opt[] (23 vec4); record = the probe, early in the
   command buffer before the shadow pass (own_barrier 0: the caller orders it) or at the far point (own_barrier 1);
   1 = dispatched; toggle = F4 (-1 = not available);
   ab = ACZ_VR_OPTICS_AB's off phase. */
int  ps2_clouds_optics_init(const ps2_cloud_gpu *g);
void ps2_clouds_optics_shutdown(const ps2_cloud_gpu *g);
void ps2_clouds_optics_begin(const ps2_cloud_env *e, const ps2_cloud_view *v, int frame_index, uint32_t frame);
void ps2_clouds_optics_ubo(float opt[92]);
int  ps2_clouds_optics_record(VkCommandBuffer cmd, const ps2_cloud_rec *r, float near_range_m, int own_barrier);
int  ps2_clouds_optics_toggle(int on);
void ps2_clouds_optics_ab(int off);
/* ps2_clouds_wake.c: the cloud wake's probe (ACZ_VR_CLOUD_WAKE; ps2_trails.h pt_wake_*): our clouds' density about the
   aircraft zero_trails.c asks for, once per game frame's command buffer (cloud_wake.comp, early, next to the optics
   probe; own_barrier as there); begin = the answers of this slot's last probe (once per command buffer, before
   record). */
int  ps2_clouds_wake_init(const ps2_cloud_gpu *g);
void ps2_clouds_wake_shutdown(const ps2_cloud_gpu *g);
void ps2_clouds_wake_begin(int frame_index, uint32_t frame);
int  ps2_clouds_wake_record(VkCommandBuffer cmd, const ps2_cloud_rec *r, int own_barrier);
/* THE HOOK for other renderers (terrain, fog): sample the same map with runtime/shaders/cloud_shadow.glsl
   (cl_sun_shadow_map(map, s0, s1, s2, p_m) = the sun's transmittance at p_m, metres relative to the view's centre eye;
   cl_shadow_od_map for the optical depth and the map weight). Bind the returned view with a LINEAR + REPEAT sampler
   (clamp would break the toroidal addressing) in the renderer's own set, in GENERAL layout. It is written by compute
   early in each command buffer (ps2_clouds_far_early, right after the weather pass, before the game's draws); while
   the hook is being called (within the last 8 frames) the barrier ps2_clouds.c issues after the shadow pass covers
   fragment reads too (compute -> compute | fragment | host), so a terrain / fog draw later in the same command buffer
   needs no barrier of its own (otherwise it is compute only, so the game's draws do not wait for the pass). params =
   s0, s1, s2 (12 floats, as CloudFrame shd[0..2]) for the view v (its origin_m: the centre eye in world metres, the
   game world / units_per_m); returns 0 when there is no map (s0.w = 0: the lookup then returns full sun). Call it on
   the renderer thread after this command buffer's cloud begin (cl_early in render_list), once per command buffer. */
int  ps2_clouds_shadow_hook(const ps2_cloud_view *v, float params[12], VkImageView *view);
/* The far map (cloud_dir) for other renderers (the cockpit's rear-view mirrors, ps2_video.c mir_*): its view (GENERAL
   layout), the sampler the far composite reads it with (linear, clamp) and cl_dir_tex_uv's scale for the current
   weather (the used size N_eff / N). 0 = no map (our clouds or their far part off, F4 included; not created yet). The
   map is written by compute (the far update); a reader orders its own reads after it (compute -> fragment). */
int  ps2_clouds_dir_hook(VkImageView *view, VkSampler *samp, float *uv_scale);
/* the far map's depth (R16F: the transmittance-weighted mean distance, m; 65504 = no cloud), the same layout as the
   map: for the mirrors' live map (ps2_video.c, ACZ_VR_COCKPIT_MIRRORS_LIVE_CLOUDS). 0 = none */
int  ps2_clouds_dir_depth_hook(VkImageView *view);

/* ---------------------------------------------------------------------------------------------------------------
   ps2_video.c <-> ps2_clouds.c (S4)
   --------------------------------------------------------------------------------------------------------------- */
typedef struct {
    VkPhysicalDevice phys;
    VkDevice dev;
    VkPipelineCache cache;
    const char *shader_dir;
    int multiview;
} ps2_clouds_init_info;
int  ps2_clouds_vk_init(const ps2_clouds_init_info *ii);   /* after the device exists; 0 = ok (or clouds off) */
void ps2_clouds_vk_shutdown(void);
void ps2_clouds_image_gone(VkImage img);                   /* a render target image is being destroyed */

/* one split point of one render pass (rendering must be closed by the caller) */
typedef struct {
    VkCommandBuffer cmd;
    int frame_index;                  /* frame in flight */
    uint64_t cmd_id;                  /* changes per command buffer (far update once per command buffer) */
    VkImage color;                    /* slot 0: R8G8B8A8_UNORM, layered in multiview */
    VkImageView color_view;           /* its attachment view (2D array in multiview) */
    VkImageLayout color_layout;       /* the layout it is in and must be left in (the renderer's attachment layout) */
    VkImage depth;                    /* the frame's D32 depth */
    VkImageLayout *depth_layout;      /* in/out: its tracked layout (left as DEPTH_ATTACHMENT_OPTIMAL) */
    uint32_t layers;                  /* 2 in multiview */
    int eye;                          /* -1 multiview (both), 0 / 1 two-pass eye */
    const ps2_cloud_view *view;
    /* ACZ_REC_CINE (ps2_video.c cine_*): the recording's cinematic view, drawn after the frame's own pass in the same
       command buffer: only the composite (and the effects' depth stamp) from this view over clip (x0, y0, x1, y1 target
       pixels) -- no weather, shadow, far update, near march, probes, verify or dump: the frame's results stay as they
       are for the headset and the frames between. The near point composites the frame's near buffer (marched for the
       head's view) through near_h: this view's target pixel -> the frame's (rows, homogeneous; the head's turn against
       this view's, the eye's few cm left out), when the frame's own near point drew this frame */
    int composite_only;
    int32_t clip[4];
    int near_h_on;
    float near_h[9];
    /* PS2_PERF_CLOUD_ASYNC (perf4_passes, ps2_clouds_far_early only): a command buffer of the second queue that takes the
       far update and the shaft slice (the weather, shadow and probe passes stay in cmd; the renderer submits it after the
       part holding them and makes the far map's readers wait for it); VK_NULL_HANDLE = all in cmd as before */
    VkCommandBuffer async_cmd;
} ps2_cloud_pass;
/* 1 = the composite drew into the colour target (the caller invalidates its snapshots; rendering stays closed) */
/* early in the command buffer (render_list, before the game's draws; rendering closed): the weather pass and the far
   update, so they overlap the game's draws -- no barrier after them. color/depth fields are not used (may be 0). */
int  ps2_clouds_far_early(const ps2_cloud_pass *p);
int  ps2_clouds_far_async_used(void);   /* the last ps2_clouds_far_early recorded into async_cmd (perf4_passes) */
int  ps2_clouds_far_point(const ps2_cloud_pass *p);
int  ps2_clouds_near_point(const ps2_cloud_pass *p);
/* ACZ_VR_CLOUD_AB=N (measurement): 1 = record this frame without clouds (called once per recorded frame), and the
   frame's GPU time once its fence has passed */
int  ps2_clouds_ab_off(int frame_index);
void ps2_clouds_ab_sample(int frame_index, double gpu_ms);
/* immersion: our in-cloud value 0..1 (the larger of C1 in_cloud and the near field's report), -1 = none. ps2_video.c
   feeds it into the image-based in-cloud measure (canopy mist and water, airflow sound, turbulence rumble). */
float ps2_clouds_in_cloud(void);
/* S3 (renderer thread, from the near hook): the near field's own in-cloud estimate 0..1 for this frame */
void ps2_clouds_report_in_cloud(float v);
float ps2_clouds_units_per_m(void);         /* ACZ_VR_UNITS_PER_M (6), the game units per metre of cloud space */

/* ---------------------------------------------------------------------------------------------------------------
   S5 art capture (ps2_clouds_art.c; S0 ships a no-op stub). Its own switch ACZ_CLOUD_ART (default on) and it runs
   with ACZ_VR_CLOUDS=0 too (contact sheets of the game's own clouds); keep each call cheap.
   --------------------------------------------------------------------------------------------------------------- */
typedef struct ps2_cloud_art ps2_cloud_art;  /* defined by S5 */
/* GS thread, ps2_vk_draw just before dump_draw (v = the vertices as recorded, cls = xr class, seq = xr_frame_seq) */
void ps2_clouds_art_draw(const ps2_vk_state *st, const ps2_vk_vertex *v, int n, int cls, u32 seq);
/* GS thread, ps2_vk_draw_mesh_topo after the block is stored */
void ps2_clouds_art_mesh(const ps2_vk_state *st, const ps2_vk_mesh_block *b);
/* renderer thread, cloud_measure_collect: the five 8x8 RGBA8 patches before / after the cloud sprites and the raw
   density of that frame */
void ps2_clouds_art_patches(const u8 *before, const u8 *after, int npatch, int npix, float density);
/* a seqlocked copy of the last finished frame (for the C1 publish); 0 = none */
int  ps2_clouds_art_get(ps2_cloud_art *out);

#ifdef __cplusplus
}
#endif
#endif
