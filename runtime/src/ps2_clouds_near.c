// ps2_clouds_near.c - S3, the near cloud volume (agent C; CLOUDS_3D_RESEARCH_2026-10-20.md section 8, shader
// runtime/shaders/cloud_near.comp). Called per eye pass at the near point (after the game's last cloud sprite) with the
// common set 0 bound and the scene depth readable; rec->eye = -1 means both layers (multiview), else the one eye of a
// two-pass frame. Returns 0 (no dispatch, no composite) while no cloud can be within the near range: the pass costs
// nothing in clear sky.
//
// Gate (CPU, per drawn frame): the camera altitude must be within the near range of the C1 band [base_m, top_m]; with
// the game's cloud list (C1 CL_ENV_LIST) also some cloud's body must come within the range: the ellipsoid of 0.9 x its
// semi-axes (radius_m, height_m, radius_m: the profile is gone at 0.9), distance bounded from below by
// (|offset / axes| - 1) x the smaller axis (conservative); the 0.9 radius_m sphere if height_m is unknown. Without a
// list the band alone decides (the procedural weather map fills the band). ACZ_VR_CLOUD_NEAR_GATE=0 always marches.
// With ACZ_VR_CLOUD_LIST_KEEP (default) the clouds tested are the ones the weather map holds (S2's tracked list, incl.
// clouds still fading out; ps2_clouds_far_near_gate), and an empty list means clear sky: before, an empty C1 list
// marched the whole band (M2 cruising: the near pass active 50 % of the time with no cloud anywhere near).
//
// Switches (read once):
//   ACZ_VR_CLOUD_NEAR_STEP    plane spacing near the camera, m (6.25; snapped so that 32 x it divides 4000 m)
//   ACZ_VR_CLOUD_NEAR_STEPS   (S0's) if set: the sample budget of an axis-aligned ray over the range; s0 follows it
//   ACZ_VR_CLOUD_NEAR_TIER    end of the first tier, m (60); ACZ_VR_CLOUD_NEAR_GROW tier growth (2.4): tier i ends at
//                             TIER x GROW^i with spacing STEP x 2^i
//   ACZ_VR_CLOUD_NEAR_SIGMA   extinction per m at density 1 (0 = the shared cl_extinction(), 0.02 x (1 - 0.6 transp))
//   ACZ_VR_CLOUD_NEAR_DETAIL  near-only detail octave strength 0..1 (0.6; period 50 m, gone by 60 m)
//   ACZ_VR_CLOUD_NEAR_BLEND   blend the plane families where two axes tie (0: with the tier march the two families
//                             differ by <= 1 level at a tie, no visible seam; 1 costs ~6%)
//   ACZ_VR_CLOUD_NEAR_LIGHT   one light sample with three-octave multiple scattering (0)
//   ACZ_VR_CLOUD_NEAR_VIEW    debug: 1 = the near opacity as opaque grey
//   ACZ_VR_CLOUD_NEAR_GATE    0 = no gate
//   ACZ_VR_CLOUD_NEAR_LUMPWIN / _LUMPSCALE  [tuning] in-cloud lumps: moving-mean window (m, 250) and the density ratio
//                             to that mean that counts as a full lump (0.35)
//   ACZ_VR_CLOUD_NEAR_CONTRAST [tuning] overrides C1 near_art.y (near_contrast) for A/B against E's per-kind value
//   ACZ_VR_CLOUD_NEAR_CB      adaptive checkerboard (1): march half the texels, average the rest where their four
//                             neighbours agree (smooth fog), march them where not (edges); 0 = march every texel
//   ACZ_VR_CLOUD_NEAR_MIP     prefiltered noise in the far tiers (1; from ~350 m, as the far map at the hand-over)
//   ACZ_VR_CLOUD_NEAR_AB=N    [measurement] every N recorded frames alternate the cost modes above with the plain march
//                             (the old defaults: CB 0, MIP 0, BLEND 1) and log both GPU times (own timestamps) every
//                             600 frames of each
//   ACZ_VR_CLOUD_NEAR_FOV_DEG the coarse periphery (ov_passes, 2026-10-25; 0 = off): beyond this angle from each eye's
//                             axis every distance tier is marched at twice its world-fixed plane spacing, blended with the
//                             fine march over 2 deg (cloud_near.comp). ACZ_VR_CLOUD_NEAR_FOV_AB=N [measurement] alternates it
//                             in blocks of N frames and logs the near pass's own GPU times; PS2_PASS_AB_WHAT=sparse the
//                             frame's; PS2_PERF_AB_FLIP=sparse the exact same-frame image A/B. (Tried and measured slower
//                             in M1's overcast: 1 texel in 4 marched out there with a diagonal fill and a compacted queue,
//                             only the first tier coarse, pass B's marches compacted through a queue.)
//   ACZ_VR_CLOUD_NEAR_SPEC    (perf6_clouds 2026-11-03) the march's pipelines (cloud_near.comp NEAR_SPEC, bits): 1 = one
//                             pipeline per pass kind with the flags as constants (pass A / pass B's fill / the reuse's
//                             game and in-between frames; the lens mask and the reuse tint stay push constants): the
//                             compiler drops what a pass never runs (fq_opt: 72 -> 64 registers, no local memory) and
//                             the fill's neighbours are unrolled. It runs the march's statements in the same order:
//                             the same near buffer bit for bit (ACZ_VR_CLOUD_NEAR_SPEC_VERIFY; PS2_PERF_AB_FLIP=nearspec:
//                             the re-draw with the generic pipeline). 0 = the
//                             generic pipeline (the flags from the push constants), as before. Not with the coarse
//                             periphery (its own pipeline) or the plain-march A/B (no matching pipeline: generic).
//                             Measurement-only values (ACZ_VR_CLOUD_DETAIL_AB's lever spec=N; add 1 for the specialised
//                             pipelines): 8 constant colour (the shading's arithmetic gone, the probe's reads kept), 16
//                             constant colour without the probe's reads, 32 the base and detail noise read twice (bound
//                             by arithmetic or by reads?).
//   ACZ_VR_CLOUD_NEAR_SPEC_VERIFY=N [verification] every N-th near pass is marched twice on the same inputs: with the
//                             configured pipelines, then with the generic ones (NEAR_SPEC 0: the march as before); both
//                             results (the near buffer RGBA16F and its clip depth R16F, both eyes) are copied out and
//                             compared bit for bit once the frame's fence has passed: "clouds-near spec verify:" every
//                             20 checks (texels differing, the largest difference). The frame then shows the generic
//                             march's result. A same-frame check without a re-draw of the whole frame (whose other inputs
//                             change between re-draws: the PS2_PERF_AB=2 control is not byte-identical in cloud).
//   CLOUD DETAIL (ACZ_VR_CLOUD_DETAIL, ps2_clouds.c): the levels' near-buffer divisor, plane spacing, tiers and detail
//                             octave (ps2_clouds_detail_levels; level 0 = HIGH = the values above); this frame's level
//                             from ps2_clouds_detail_cur. The reuse and the coarse periphery only at divisor 2.
// The lens mask (PS2_PASS_MASK with XR_KHR_visibility_mask or PS2_PASS_MASK_TEST_LENS): set 1 binding 1, the hidden
// 8 x 8-texel blocks per frame slot from ps2_vk_pm_lens_blocks (ps2_video.c), skipped like the outside of the crop.
// ACZ_VR_CLOUD_NEAR_LENS_DEV (nx_cloudreuse, default 1): the shaders read a device-local copy, vkCmdUpdateBuffer per
// pass (host-visible memory is system memory without Resizable BAR, and per-texel reads of it cost ~0.3 ms in the
// reprojection pass's first version); 0 = the host-visible buffer read per texel, as before.
//
// Near-only art (C1 near_art -> CloudFrame user[2], S5 per mission; all 0 = these defaults):
//   x 0.8   incloud_mix: samples deep inside a cloud blob are pulled this far toward C1 incloud
//   y 0.5   near_contrast: then lumps from density (thin parts toward LIGHT, dense ones toward DARK); 0.5 gives the
//           game veil's p5-p95 spread of ~20-24 levels on 16 px blocks (E: M13 20-28, the comfort cap ~28)
//   z 1.0   near extinction multiplier (x the shared cl_extinction(), faded to 1 by 0.9 x range)
//   w 0.6   light-sample weight (ACZ_VR_CLOUD_NEAR_LIGHT=1 only)
// All near-only terms fade out by 0.9 x range, so the hand-over to the far map is the shared C4 look and opacity.
//
// In-cloud value: one shader thread measures the mean opacity of six 120 m rays around the centre eye (our rendered
// veil) into a host-visible buffer slot per frame in flight; the slot is read back when it comes round again (its
// fence has passed: ~2-3 frames late) and reported with ps2_clouds_report_in_cloud (main takes the max with C1's
// envelope value from S1) for the canopy mist, airflow sound and rumble. 0 while the pass is idle.
//
// In-between frames reuse the game frame's march (ACZ_VR_CLOUD_NEAR_REUSE, nx_cloudreuse 2026-10-25; plan item #7):
// with smooth motion every other 120 Hz frame re-renders the last game frame for a newer pose (extrapolated camera, new
// head pose), and its near march cost as much as the game frame's. With the switch on, a game frame splits each ray's
// march at the first tier node (ACZ_VR_CLOUD_NEAR_TIER, 60 m: a camera-relative sample, so the split is exact) and
// keeps the part beyond it as the history (cloud_near_far: premultiplied colour and transmittance of [60 m, clip],
// total = near + T_near x far) with its reprojection depth (cloud_near_par: the contribution-weighted mean of 1 /
// distance over that part and its spread, the clip distance). An in-between frame then
//   1. reprojects every texel's far part from the history (cloud_near_reuse.comp: the ray of this pose followed to the
//      history's depth, two fixed-point steps, projected into the game frame's eye; bilinear over the four history
//      texels; the clip distance recomputed from this frame's scene depth) and flags the 8 x 8-texel block for a full
//      march where it is not trusted: a source off the history image or outside what the game frame marched (view
//      edges after a head turn, the crop, the lens mask); a scene edge (a source's clip distance off by more than 3 %
//      + the eye's motion + 1 m: terrain silhouettes, aircraft, anything that moved); a cloud depth edge (the four
//      sources' depths pull them apart by more than ACZ_VR_CLOUD_NEAR_REUSE_DISP texels, 0.75); the error estimate
//      (the eye's motion across the ray x the 1 / distance spread inside and across the sources, in texels, x the
//      colour / transmittance range of the four) above ACZ_VR_CLOUD_NEAR_REUSE_TOL (1.5 levels);
//   2. marches (same pipeline and checkerboard, flag 512) the flagged blocks in full and everywhere else only the near
//      part [4 m, 60 m], fresh for this pose -- the part with the large parallax, where the in-cloud veil, its lumps
//      and the near detail live, so they never swim --, and puts the reprojected far part behind it.
// Both eyes run the same test on their own history layer. No reuse at all (a full march, as before): no history from
// the frame's game frame (that eye's near pass idle, another seq, the near images re-made, a cloud-frame rebase), eye
// motion > 40 m, flash lights in the frame (they pulse per drawn frame), the axis blend or the coarse periphery (their
// two marches per texel), PS2_PERF_AB_FLIP=nreuse's re-draw, ACZ_VR_CLOUD_NEAR_REUSE_AB's off blocks, and while the
// game frames' near pass (own timestamps, moving mean over ~20 frames) is cheaper than ACZ_VR_CLOUD_NEAR_REUSE_MIN_MS
// (0.35 ms; engaged at it, released below 0.8 x it): the reprojection pass and the march to the node cost ~0.2 ms, so in
// thin or distant cloud (M1 above the deck: a 0.22 ms near pass) a full march pays better. While engaged,
// ps2_clouds.c gives all cloud passes of an in-between frame its game frame's C1 (the colours, fog and drift the
// history was marched with; S1 has often published the next game frame's by then: in the M1 climb that alone made the
// reused frame differ from a full march by a uniform +-0.5 level). Game frames
// write the history only while in-between frames are being drawn (one in the last 16 frames: smooth motion on).
// ACZ_VR_CLOUD_NEAR_REUSE_VIEW=1 (debug) tints the texels with a reprojected far part green. Counters ("clouds-near
// reuse:" every 1200 near passes): the near pass's own time of game frames (with / without the history output), of
// in-between frames reprojected / marched in full, the reasons, the share of visible blocks marched in full.
#include "ps2_runtime.h"
#include "ps2_clouds.h"
#include "ps2_vkcen.h"          /* (PS2_VK_CENSUS: vkCmd* calls counted) */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static VkPipeline near_pipe;
static VkPipeline near_pipe_c;               /* the coarse periphery's (NEAR_COARSE = 1; only with ACZ_VR_CLOUD_NEAR_FOV_DEG) */
static VkPipelineLayout near_layout;
/* perf6_clouds: the march's pipelines by (NEAR_SPEC, the flags fixed in it, NEAR_DIV) -- cloud_near.comp constants 1-4,
   made at init for the combinations this session's settings can ask for; near_pipe = the generic one at NEAR_DIV 2 (no
   constants: the shader as before) */
#define NEAR_PIPES 48
#define NEAR_DYN_FLAGS (128 | 1024)          /* left to the push constants: the lens mask, the reuse's debug tint */
static struct { int spec, fix, div; VkPipeline p; } NP[NEAR_PIPES];
static int np_n;
#ifndef NEAR_SPEC_DEFAULT
#define NEAR_SPEC_DEFAULT 1                  /* ACZ_VR_CLOUD_NEAR_SPEC's default */
#endif
static struct {                              /* ACZ_VR_CLOUD_NEAR_SPEC_VERIFY */
    int every; uint64_t passes;
    VkBuffer buf; VkDeviceMemory mem; uint8_t *map; VkDeviceSize half;   /* [A | B]: colour then depth, both eyes */
    int pending, slot; uint32_t w, h, layers; int spec, div, inb;
    uint64_t checks, bad, texels, diff_c, diff_d; double maxd; uint64_t checks_inb;
} VF;
#define NEAR_SLOTS 8u                        /* >= frames in flight */
static VkDescriptorSetLayout probe_dsl;
static VkDescriptorPool probe_pool;
static VkDescriptorSet probe_set;
static VkBuffer probe_buf;
static VkDeviceMemory probe_mem;
static float *probe_map;
/* set 1 binding 1: the lens' hidden blocks per frame slot (PS2_PASS_MASK with a lens: ps2_vk_pm_lens_blocks) */
#define NEAR_LENS_UINTS 2048u                /* = cloud_near.comp: 2 eyes x (near w / 8) x (near h / 8) bits, up to 6x */
static VkBuffer lens_buf, lens_dev;          /* lens_buf: the host's copy; lens_dev: the shaders' (device-local, filled
                                                with vkCmdUpdateBuffer: host-visible memory is system memory here, and
                                                the per-texel reads of it are slow -- nx_cloudreuse 2026-10-25) */
static VkDeviceMemory lens_mem, lens_dmem;
static uint32_t *lens_map;
static uint8_t probe_used[NEAR_SLOTS];
static VkQueryPool tq;                       /* own timestamps: 2 per slot (before / after the near dispatches) */
static uint8_t tq_used[NEAR_SLOTS], tq_mode[NEAR_SLOTS];
/* ACZ_VR_CLOUD_NEAR_REUSE (set 1 bindings 2-4, all device-local: host-visible memory is system memory here (no
   Resizable BAR), and a per-thread read of it cost the reprojection pass ~0.3 ms): the full-march flags (one uint per
   8 x 8-texel block and eye layer), the history camera per frame slot and eye pass (NEAR_HIST_VEC4 vec4, written with
   vkCmdUpdateBuffer), the counters per frame slot (4 uints, copied to stat_buf for the host) */
#define NEAR_FLAG_UINTS 65536u               /* 2 layers x (w / 8) x (h / 8): up to 2048 x 2048 texels per layer */
#define NEAR_HIST_VEC4 12u                   /* per frame slot and eye pass (two-pass frames have two near passes) */
static VkPipeline reuse_pipe;
static VkBuffer flag_buf, hist_buf, stat_buf, stat_dev;
static VkDeviceMemory flag_mem, hist_mem, stat_mem, stat_dmem;
static float hist_tmp[NEAR_HIST_VEC4 * 4u];
static uint32_t *stat_map;
static uint8_t stat_used[NEAR_SLOTS], tq_kind[NEAR_SLOTS];   /* tq_kind: 0 game frame, 1 in-between reprojected, 2 in-between
                                                                marched, 3 game frame without the reprojection depth (A/B off) */
static struct {
    int valid[2];                            /* per eye layer: the last game frame marched it with the reprojection depth */
    ps2_cloud_view view[2];                  /* that game frame's view (cloud-frame origin included) */
    uint32_t epoch;
} H;
static struct {
    double ms[4]; uint32_t n[4];             /* the near pass's own time by tq_kind */
    uint64_t inb, reproj, why[7];            /* in-between near passes, reprojected; full-march reasons */
    uint64_t engage;                         /* times the reuse engaged (the near pass got expensive enough) */
    uint64_t blk_vis, blk_bad;               /* visible blocks / re-marched (read back) */
    uint64_t frames;
} RS;
static int reuse_ab_off;
static uint32_t reuse_inb_frame;             /* the last in-between frame seen (no smooth motion: no history output) */
static double reuse_game_ms = -1.0;          /* the game frames' near pass, own timestamps (moving mean over ~20 frames) */
static int reuse_engaged;                    /* the near pass costs enough for the reuse to pay (ACZ_VR_CLOUD_NEAR_REUSE_MIN_MS) */
static float tq_period_ns;
static struct { double sum[2]; uint32_t n[2]; uint64_t frames; } AB;
static struct {
    float s0, d0, q, sigma, detail, detail_period, detail_fade, r0, lump_win, lump_scale, contrast;
    int blend, light, view, gate, cap, cb, mip, ab;
    float fov_deg, fov_tan2; int fov_ab;          /* the coarse periphery (ACZ_VR_CLOUD_NEAR_FOV_DEG / _FOV_AB) */
    int reuse, reuse_view; float reuse_tol, reuse_disp, reuse_maxmove;   /* ACZ_VR_CLOUD_NEAR_REUSE (in-between frames) */
    float reuse_min_ms;
    int lens_dev;                                 /* ACZ_VR_CLOUD_NEAR_LENS_DEV: the shaders read the lens bits device-local */
    int spec;                                     /* ACZ_VR_CLOUD_NEAR_SPEC */
    struct { float s0, d0, q, detail; int cap, div, spec; } L[CL_DETAIL_MAX];   /* the CLOUD DETAIL levels (0 = HIGH) */
    int nl;
} P;
static struct { uint64_t frames, active; int last; float nearest; } S;
/* (ps2_video.c, layered in-between frames: ACZ_VR_SMOOTH_LAYER_CLOUDS) the last near pass found a cloud in reach */
int ps2_clouds_near_on(void) { return S.last; }

#ifndef NEAR_FOV_DEG_DEFAULT
#define NEAR_FOV_DEG_DEFAULT 0.0f          /* ACZ_VR_CLOUD_NEAR_FOV_DEG's default (the coarse periphery off) */
#endif
#ifndef NEAR_REUSE_DEFAULT
#define NEAR_REUSE_DEFAULT 0               /* ACZ_VR_CLOUD_NEAR_REUSE's default */
#endif
static float envf(const char *name, float def, float lo, float hi) {
    const char *e = getenv(name);
    if (!e || !*e) return def;
    float v = (float)atof(e);
    return v < lo ? lo : v > hi ? hi : v;
}

/* samples on an axis-aligned ray over [r0, range] for spacing s0 (the tier sum), tiers d0 x q^i */
static float near_samples_t(float s0, float range, float d0, float q) {
    float n = 0.0f, a = P.r0, end = d0, s = s0;
    for (int i = 0; i < 8 && a < range; i++) {
        float b = i == 7 || end > range ? range : end;
        if (b > a) n += (b - a) / s + 1.0f;
        a = b; end *= q; s *= 2.0f;
    }
    return n;
}
static float near_samples(float s0, float range) { return near_samples_t(s0, range, P.d0, P.q); }
/* the spacing snapped so that every tier spacing (up to s0 x 32) divides 4000 m */
static float near_snap(float s0) {
    float n = roundf(4000.0f / (s0 * 32.0f));
    if (n < 1.0f) n = 1.0f;
    return 4000.0f / (n * 32.0f);
}
static int near_cap(float s0, float range, float d0, float q) {
    int cap = (int)(2.0f * near_samples_t(s0, range, d0, q)) + 8;
    return cap < 16 ? 16 : cap > 256 ? 256 : cap;
}

/* perf6_clouds: a march pipeline from cloud_near.comp with constants 0-4 (coarse, NEAR_SPEC, the fixed flags, the
   dynamic flags, NEAR_DIV) */
static VkPipeline near_make(const ps2_cloud_gpu *g, VkShaderModule m, int coarse, int spec, int fix, int div) {
    int32_t v[5] = { coarse, spec, (spec & 1) ? fix : 0, (spec & 1) ? NEAR_DYN_FLAGS : -1, div };
    VkSpecializationMapEntry me[5];
    for (int i = 0; i < 5; i++) { me[i].constantID = (uint32_t)i; me[i].offset = (uint32_t)(i * 4); me[i].size = 4; }
    VkSpecializationInfo si = { 5, me, sizeof v, v };
    VkComputePipelineCreateInfo ci; memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; ci.stage.module = m; ci.stage.pName = "main";
    ci.stage.pSpecializationInfo = &si; ci.layout = near_layout;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateComputePipelines(g->dev, g->cache, 1, &ci, NULL, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    return p;
}
static void near_want(const ps2_cloud_gpu *g, VkShaderModule m, int spec, int fix, int div) {
    if (!(spec & 1)) fix = 0;
    for (int i = 0; i < np_n; i++) if (NP[i].spec == spec && NP[i].fix == fix && NP[i].div == div) return;
    if (np_n >= NEAR_PIPES || !m) return;
    VkPipeline p = near_make(g, m, 0, spec, fix, div);
    if (!p) { ps2_log("clouds-near: pipeline (spec %d, flags %d, divisor %d) failed", spec, fix, div); return; }
    NP[np_n].spec = spec; NP[np_n].fix = fix; NP[np_n].div = div; NP[np_n].p = p; np_n++;
}
/* the pipeline for this pass: NEAR_SPEC spec, the pass's flags, the divisor; else the generic one at that divisor */
static VkPipeline near_get(int spec, int flags, int div) {
    int fix = (spec & 1) ? flags & ~NEAR_DYN_FLAGS : 0;
    for (int i = 0; i < np_n; i++) if (NP[i].spec == spec && NP[i].fix == fix && NP[i].div == div) return NP[i].p;
    for (int i = 0; i < np_n; i++) if (NP[i].spec == 0 && NP[i].div == div) return NP[i].p;
    return VK_NULL_HANDLE;
}

int ps2_clouds_near_init(const ps2_cloud_gpu *g) {
    (void)g;
    const ps2_cloud_cfg *c = ps2_clouds_cfg();
    P.r0 = 4.0f;
    P.d0 = envf("ACZ_VR_CLOUD_NEAR_TIER", 60.0f, 10.0f, 1000.0f);
    P.q = envf("ACZ_VR_CLOUD_NEAR_GROW", 2.4f, 1.2f, 8.0f);
    P.s0 = envf("ACZ_VR_CLOUD_NEAR_STEP", 6.25f, 1.0f, 100.0f);
    {   const char *e = getenv("ACZ_VR_CLOUD_NEAR_STEPS");   /* an explicit budget sets the spacing */
        if (e && *e && c->near_steps > 1) P.s0 = near_samples(1.0f, c->near_range_m) / (float)c->near_steps;
    }
    {   float n = roundf(4000.0f / (P.s0 * 32.0f));           /* every tier spacing (up to s0 x 32) divides 4000 m */
        if (n < 1.0f) n = 1.0f;
        P.s0 = 4000.0f / (n * 32.0f);
    }
    P.sigma = envf("ACZ_VR_CLOUD_NEAR_SIGMA", 0.0f, 0.0f, 1.0f);
    P.detail = envf("ACZ_VR_CLOUD_NEAR_DETAIL", 0.6f, 0.0f, 1.0f);
    P.detail_period = 50.0f;
    P.detail_fade = 60.0f;
    P.blend = (int)envf("ACZ_VR_CLOUD_NEAR_BLEND", 0.0f, 0.0f, 1.0f);   /* 0: no seam measurable (<= 1 level at a tie) */
    P.light = (int)envf("ACZ_VR_CLOUD_NEAR_LIGHT", 0.0f, 0.0f, 1.0f);
    P.view = (int)envf("ACZ_VR_CLOUD_NEAR_VIEW", 0.0f, 0.0f, 1.0f);
    P.gate = (int)envf("ACZ_VR_CLOUD_NEAR_GATE", 1.0f, 0.0f, 1.0f);
    P.lump_win = envf("ACZ_VR_CLOUD_NEAR_LUMPWIN", 0.0f, 0.0f, 5000.0f);     /* tuning; 0 = the shader's 250 m */
    P.lump_scale = envf("ACZ_VR_CLOUD_NEAR_LUMPSCALE", 0.0f, 0.0f, 10.0f);  /* tuning; 0 = the shader's 0.35 */
    P.contrast = envf("ACZ_VR_CLOUD_NEAR_CONTRAST", 0.0f, 0.0f, 2.0f);      /* tuning; 0 = C1 near_art.y */
    P.cb = (int)envf("ACZ_VR_CLOUD_NEAR_CB", 1.0f, 0.0f, 1.0f);
    P.mip = (int)envf("ACZ_VR_CLOUD_NEAR_MIP", 1.0f, 0.0f, 1.0f);
    P.ab = (int)envf("ACZ_VR_CLOUD_NEAR_AB", 0.0f, 0.0f, 100000.0f);
    P.fov_deg = envf("ACZ_VR_CLOUD_NEAR_FOV_DEG", NEAR_FOV_DEG_DEFAULT, 0.0f, 80.0f);
    if (P.fov_deg > 0.0f && P.fov_deg < 10.0f) P.fov_deg = 10.0f;
    P.fov_tan2 = P.fov_deg > 0.0f ? tanf(P.fov_deg * 0.01745329f) * tanf(P.fov_deg * 0.01745329f) : 0.0f;
    P.fov_ab = P.ab ? 0 : (int)envf("ACZ_VR_CLOUD_NEAR_FOV_AB", 0.0f, 0.0f, 100000.0f);
    P.reuse = (int)envf("ACZ_VR_CLOUD_NEAR_REUSE", (float)NEAR_REUSE_DEFAULT, 0.0f, 1.0f);
    {   const char *e = getenv("ACZ_VR_CLOUD_NEAR_REUSE_AB");          /* (the A/B needs the images) */
        if (e && atoi(e) > 0) P.reuse = 1;
    }
    P.reuse_tol = envf("ACZ_VR_CLOUD_NEAR_REUSE_TOL", 1.5f, 0.0f, 255.0f) / 255.0f;
    P.reuse_disp = envf("ACZ_VR_CLOUD_NEAR_REUSE_DISP", 0.75f, 0.0f, 64.0f);
    P.reuse_view = (int)envf("ACZ_VR_CLOUD_NEAR_REUSE_VIEW", 0.0f, 0.0f, 1.0f);
    P.reuse_maxmove = 40.0f;
    P.reuse_min_ms = envf("ACZ_VR_CLOUD_NEAR_REUSE_MIN_MS", 0.35f, 0.0f, 100.0f);
    P.lens_dev = (int)envf("ACZ_VR_CLOUD_NEAR_LENS_DEV", 1.0f, 0.0f, 1.0f);
    P.spec = (int)envf("ACZ_VR_CLOUD_NEAR_SPEC", (float)NEAR_SPEC_DEFAULT, 0.0f, 255.0f);
    VF.every = (int)envf("ACZ_VR_CLOUD_NEAR_SPEC_VERIFY", 0.0f, 0.0f, 100000.0f);
    {   VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(g->phys, &pp);
        tq_period_ns = pp.limits.timestampPeriod;
        VkQueryPoolCreateInfo qc; memset(&qc, 0, sizeof qc);
        qc.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO; qc.queryType = VK_QUERY_TYPE_TIMESTAMP; qc.queryCount = 2u * NEAR_SLOTS;
        if (vkCreateQueryPool(g->dev, &qc, NULL, &tq) != VK_SUCCESS) tq = VK_NULL_HANDLE;
    }
    P.cap = (int)(2.0f * near_samples(P.s0, c->near_range_m)) + 8;
    if (P.cap < 16) P.cap = 16;
    if (P.cap > 256) P.cap = 256;
    {   /* the CLOUD DETAIL levels (ps2_clouds.c): level 0 = HIGH = the values above; a rung's lever applies where its
           ACZ_VR_CLOUD_NEAR_* switch is not set explicitly (an explicit switch wins on every level) */
        ps2_cloud_detail lv[CL_DETAIL_MAX];
        int n = ps2_clouds_detail_levels(lv, CL_DETAIL_MAX);
        if (n < 1) n = 1;
        const char *es = getenv("ACZ_VR_CLOUD_NEAR_STEP"), *et = getenv("ACZ_VR_CLOUD_NEAR_TIER");
        const char *eg = getenv("ACZ_VR_CLOUD_NEAR_GROW"), *ed = getenv("ACZ_VR_CLOUD_NEAR_DETAIL");
        const char *en = getenv("ACZ_VR_CLOUD_NEAR_STEPS");
        int own_s = (es && *es) || (en && *en), own_t = et && *et, own_g = eg && *eg, own_d = ed && *ed;
        for (int i = 0; i < n; i++) {
            P.L[i].s0 = P.s0; P.L[i].d0 = P.d0; P.L[i].q = P.q; P.L[i].detail = P.detail; P.L[i].cap = P.cap;
            P.L[i].div = lv[i].div >= 2 && lv[i].div <= 4 ? lv[i].div : 2;
            P.L[i].spec = lv[i].spec >= 0 ? lv[i].spec : P.spec;
            if (i == 0) continue;
            if (lv[i].step > 0.0f && !own_s) P.L[i].s0 = near_snap(lv[i].step);
            if (lv[i].tier > 0.0f && !own_t) P.L[i].d0 = lv[i].tier;
            if (lv[i].grow > 0.0f && !own_g) P.L[i].q = lv[i].grow;
            if (lv[i].detail >= 0.0f && !own_d) P.L[i].detail = lv[i].detail;
            P.L[i].cap = near_cap(P.L[i].s0, c->near_range_m, P.L[i].d0, P.L[i].q);
        }
        P.nl = n;
    }
    /* set 1: the in-cloud probe buffer, the lens mask, the reuse's flags / history camera / counters */
    {   VkDescriptorSetLayoutBinding bb[5]; memset(bb, 0, sizeof bb);
        bb[0].binding = 0; bb[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; bb[0].descriptorCount = 1;
        bb[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        for (int k = 1; k < 5; k++) { bb[k] = bb[0]; bb[k].binding = (uint32_t)k; }
        VkDescriptorSetLayoutCreateInfo lc; memset(&lc, 0, sizeof lc);
        lc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO; lc.bindingCount = 5; lc.pBindings = bb;
        VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 5 };
        VkDescriptorPoolCreateInfo pc0; memset(&pc0, 0, sizeof pc0);
        pc0.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO; pc0.maxSets = 1; pc0.poolSizeCount = 1; pc0.pPoolSizes = &ps;
        if (vkCreateDescriptorSetLayout(g->dev, &lc, NULL, &probe_dsl) != VK_SUCCESS
            || vkCreateDescriptorPool(g->dev, &pc0, NULL, &probe_pool) != VK_SUCCESS
            || ps2_clouds_make_buffer(NEAR_SLOTS * 16u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1, &probe_buf, &probe_mem,
                                      (void **)&probe_map) != 0
            || ps2_clouds_make_buffer(NEAR_SLOTS * NEAR_LENS_UINTS * 4u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1, &lens_buf,
                                      &lens_mem, (void **)&lens_map) != 0
            || ps2_clouds_make_buffer(NEAR_SLOTS * NEAR_LENS_UINTS * 4u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                                      | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0, &lens_dev, &lens_dmem, NULL) != 0
            || ps2_clouds_make_buffer(NEAR_FLAG_UINTS * 4u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0, &flag_buf, &flag_mem, NULL) != 0
            || ps2_clouds_make_buffer(2u * NEAR_SLOTS * NEAR_HIST_VEC4 * 16u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                                      | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0, &hist_buf, &hist_mem, NULL) != 0
            || ps2_clouds_make_buffer(NEAR_SLOTS * 16u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                                      | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0, &stat_dev, &stat_dmem, NULL) != 0
            || ps2_clouds_make_buffer(NEAR_SLOTS * 16u, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 2, &stat_buf, &stat_mem,
                                      (void **)&stat_map) != 0) {
            ps2_log("clouds-near: probe resources failed");
            return -1;
        }
        VkDescriptorSetAllocateInfo ai; memset(&ai, 0, sizeof ai);
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO; ai.descriptorPool = probe_pool;
        ai.descriptorSetCount = 1; ai.pSetLayouts = &probe_dsl;
        if (vkAllocateDescriptorSets(g->dev, &ai, &probe_set) != VK_SUCCESS) return -1;
        VkDescriptorBufferInfo bi[5] = { { probe_buf, 0, NEAR_SLOTS * 16u }, { P.lens_dev ? lens_dev : lens_buf, 0, NEAR_SLOTS * NEAR_LENS_UINTS * 4u },
                                         { flag_buf, 0, NEAR_FLAG_UINTS * 4u }, { hist_buf, 0, 2u * NEAR_SLOTS * NEAR_HIST_VEC4 * 16u },
                                         { stat_dev, 0, NEAR_SLOTS * 16u } };
        VkWriteDescriptorSet w[5]; memset(w, 0, sizeof w);
        for (int k = 0; k < 5; k++) {
            w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[k].dstSet = probe_set; w[k].dstBinding = (uint32_t)k;
            w[k].descriptorCount = 1; w[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[k].pBufferInfo = &bi[k];
        }
        vkUpdateDescriptorSets(g->dev, 5, w, 0, NULL);
        memset(probe_map, 0, NEAR_SLOTS * 16u);
        memset(lens_map, 0, NEAR_SLOTS * NEAR_LENS_UINTS * 4u);
        memset(stat_map, 0, NEAR_SLOTS * 16u);
    }
    near_pipe = ps2_clouds_make_compute("cloud_near.comp.spv", probe_dsl, &near_layout);
    if (near_pipe) { NP[0].spec = 0; NP[0].fix = 0; NP[0].div = 2; NP[0].p = near_pipe; np_n = 1; }
    if (near_pipe && P.reuse) {                  /* the reprojection pass (same layout: common set 0 + this set 1) */
        VkPipelineLayout rl = VK_NULL_HANDLE;
        reuse_pipe = ps2_clouds_make_compute("cloud_near_reuse.comp.spv", probe_dsl, &rl);
        if (rl) vkDestroyPipelineLayout(g->dev, rl, NULL);
        if (!reuse_pipe) { ps2_log("clouds-near: no reprojection pipeline: ACZ_VR_CLOUD_NEAR_REUSE off"); P.reuse = 0; }
    }
    if (near_pipe && P.fov_deg > 0.0f && g->shader_dir) {   /* the coarse periphery: the same shader, NEAR_COARSE = 1 */
        char path[1200];
        snprintf(path, sizeof path, "%s/cloud_near.comp.spv", g->shader_dir);
        FILE *f = fopen(path, "rb");
        long n = 0;
        void *buf = NULL;
        if (f) { fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET); buf = n > 0 ? malloc((size_t)n) : NULL; }
        VkShaderModule m = VK_NULL_HANDLE;
        if (buf && fread(buf, 1, (size_t)n, f) == (size_t)n) {
            VkShaderModuleCreateInfo mi; memset(&mi, 0, sizeof mi);
            mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO; mi.codeSize = (size_t)n; mi.pCode = (const uint32_t *)buf;
            if (vkCreateShaderModule(g->dev, &mi, NULL, &m) != VK_SUCCESS) m = VK_NULL_HANDLE;
        }
        if (f) fclose(f);
        free(buf);
        if (m) {
            int32_t one = 1;
            VkSpecializationMapEntry me = { 0, 0, sizeof one };
            VkSpecializationInfo si = { 1, &me, sizeof one, &one };
            VkComputePipelineCreateInfo ci; memset(&ci, 0, sizeof ci);
            ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; ci.stage.module = m; ci.stage.pName = "main";
            ci.stage.pSpecializationInfo = &si; ci.layout = near_layout;
            if (vkCreateComputePipelines(g->dev, g->cache, 1, &ci, NULL, &near_pipe_c) != VK_SUCCESS) near_pipe_c = VK_NULL_HANDLE;
            vkDestroyShaderModule(g->dev, m, NULL);
        }
        if (!near_pipe_c) { ps2_log("clouds-near: no coarse-periphery pipeline: ACZ_VR_CLOUD_NEAR_FOV_DEG ignored"); P.fov_tan2 = 0.0f; }
    }
    if (near_pipe && g->shader_dir) {   /* (perf6_clouds) the specialised pipelines and the other divisors' */
        char path[1200];
        snprintf(path, sizeof path, "%s/cloud_near.comp.spv", g->shader_dir);
        FILE *f = fopen(path, "rb");
        long n = 0;
        void *buf = NULL;
        if (f) { fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET); buf = n > 0 ? malloc((size_t)n) : NULL; }
        VkShaderModule m = VK_NULL_HANDLE;
        if (buf && fread(buf, 1, (size_t)n, f) == (size_t)n) {
            VkShaderModuleCreateInfo mi; memset(&mi, 0, sizeof mi);
            mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO; mi.codeSize = (size_t)n; mi.pCode = (const uint32_t *)buf;
            if (vkCreateShaderModule(g->dev, &mi, NULL, &m) != VK_SUCCESS) m = VK_NULL_HANDLE;
        }
        if (f) fclose(f);
        free(buf);
        if (m) {
            int base = (P.blend ? 1 : 0) | (P.light ? 2 : 0) | (P.view ? 4 : 0) | (P.mip ? 32 : 0);
            int passes[2] = { P.cb ? 8 : 0, 8 | 16 }, npass = P.cb ? 2 : 1;
            for (int l = 0; l < P.nl; l++) {
                if (!ps2_clouds_detail_used(l)) continue;            /* (a rung alone: none for HIGH) */
                int div = P.L[l].div;
                near_want(g, m, 0, 0, div);                          /* (the generic one: fallback, flips) */
                for (int a = 0; a < npass; a++) {
                    near_want(g, m, P.L[l].spec, base | passes[a], div);
                    if (P.reuse && div == 2) {
                        near_want(g, m, P.L[l].spec, base | passes[a] | 256, div);
                        near_want(g, m, P.L[l].spec, base | passes[a] | 512, div);
                    }
                }
            }
            vkDestroyShaderModule(g->dev, m, NULL);
        }
        ps2_log("clouds-near: %d march pipelines (ACZ_VR_CLOUD_NEAR_SPEC %d; CLOUD DETAIL levels %d)", np_n, P.spec, P.nl);
        for (int l = 1; l < P.nl; l++)
            ps2_log("clouds-near: detail level %d: near texel %dx%d px, planes %.2f m to %.0f m then x2 per tier (x%.2f), "
                    "~%.0f samples on an axis ray (HIGH %.0f), near detail %.2f", l, P.L[l].div, P.L[l].div, P.L[l].s0,
                    P.L[l].d0, P.L[l].q, near_samples_t(P.L[l].s0, c->near_range_m, P.L[l].d0, P.L[l].q),
                    near_samples(P.s0, c->near_range_m), P.L[l].detail);
    }
    ps2_log("clouds-near: %s, range %.0f m, planes %.2f m to %.0f m then x2 per tier (x%.2f), ~%.0f samples on an "
            "axis ray, sigma %.3f, near detail %.2f, blend %d, light %d, checkerboard %d, prefiltered far tiers %d, coarse "
            "beyond %.0f deg%s%s%s", near_pipe ? "ready" : "NO PIPELINE", c->near_range_m, P.s0, P.d0, P.q,
            near_samples(P.s0, c->near_range_m), P.sigma, P.detail, P.blend, P.light, P.cb, P.mip, P.fov_deg,
            P.gate ? "" : ", no gate", P.ab ? ", A/B against the plain march" : "",
            P.fov_ab ? ", A/B of the coarse periphery" : "");
    if (P.reuse)
        ps2_log("clouds-near: in-between frames reuse the game frame's march beyond %.0f m (ACZ_VR_CLOUD_NEAR_REUSE): a "
                "full march where the error estimate > %.1f levels or a cloud depth edge > %.2f texels%s%s", P.d0,
                P.reuse_tol * 255.0f, P.reuse_disp, P.reuse_view ? ", reprojected texels tinted (ACZ_VR_CLOUD_NEAR_REUSE_VIEW)"
                : "", P.blend || P.fov_tan2 > 0.0f ? " -- not with the axis blend / the coarse periphery" : "");
    return near_pipe ? 0 : -1;
}

/* can any cloud be within the near range? (conservative; see the header). *nearest = the lower bound of the distance
   to the nearest cloud body (m; < 0 inside), -1 without a list */
static int near_active(const ps2_cloud_env *e, const ps2_cloud_view *v, float range, float *nearest) {
    const float pad = 60.0f;                     /* a few frames of flight at 167 m/s, the in-between extrapolation */
    double alt = v->cloud_origin_m[1];
    *nearest = -1.0f;
    if (alt < (double)e->base_m - range - pad || alt > (double)e->top_m + range + pad) return 0;
    if (e->valid & CL_ENV_LIST) {          /* the clouds the weather map holds, fading ones too (ACZ_VR_CLOUD_LIST_KEEP) */
        extern int ps2_clouds_far_near_gate(const ps2_cloud_view *v, float range, float pad, float *nearest);
        int g = ps2_clouds_far_near_gate(v, range, pad, nearest);
        if (g >= 0) return g;
    }
    if (!(e->valid & CL_ENV_LIST) || e->ncloud == 0) return 1;
    double ox = e->origin_m[0] - v->cloud_origin_m[0], oz = e->origin_m[2] - v->cloud_origin_m[2];
    double per = e->list_period_m > 1.0f ? e->list_period_m : 0.0;
    float best = 1e30f;
    uint32_t n = e->ncloud < CL_MAX_CLOUDS ? e->ncloud : CL_MAX_CLOUDS;
    for (uint32_t i = 0; i < n; i++) {
        double dx = e->cloud[i].pos_m[0] + ox, dz = e->cloud[i].pos_m[2] + oz, dy = e->cloud[i].pos_m[1] - alt;
        if (per > 0.0) { dx -= per * floor(dx / per + 0.5); dz -= per * floor(dz / per + 0.5); }
        double a = 0.9 * e->cloud[i].radius_m, b = a;
#ifdef CL_CLOUD_SPRITE
        if (e->cloud[i].height_m > 0.0f) b = 0.9 * e->cloud[i].height_m;
#endif
        if (a < 1.0) a = 1.0;
        if (b < 1.0) b = 1.0;
        double k = sqrt((dx * dx + dz * dz) / (a * a) + dy * dy / (b * b));
        float d = (float)((k - 1.0) * (a < b ? a : b));
        if (d < best) best = d;
    }
    *nearest = best;
    return best < range + pad;
}

int ps2_clouds_near_reuse_on(void) { return P.reuse && reuse_pipe != VK_NULL_HANDLE; }
int ps2_clouds_near_reuse_active(void)        /* (this frame: on, engaged, not in an A/B off block) */
{ return P.reuse && reuse_pipe != VK_NULL_HANDLE && !reuse_ab_off && reuse_engaged; }
void ps2_clouds_near_reuse_ab(int off) { reuse_ab_off = off; }

/* ACZ_VR_CLOUD_NEAR_REUSE: may this in-between pass reproject the history? 0 = yes, else the reason (RS.why) */
static int reuse_why(const ps2_cloud_gpu *g, const ps2_cloud_rec *r) {
    extern int ps2_vk_perf_ab_flipped(const char *name);
    extern int ps2_clouds_flash_count(void);
    const ps2_cloud_view *v = r->view;
    int e0 = r->eye < 0 ? 0 : r->eye, e1 = r->eye < 0 ? 1 : r->eye;
    if (reuse_ab_off || ps2_vk_perf_ab_flipped("nreuse")) return 1;               /* A/B off */
    if (H.epoch != g->near_epoch) return 2;                                       /* new images */
    for (int e = e0; e <= e1; e++) {
        const ps2_cloud_view *h = &H.view[e];
        if (!H.valid[e] || h->seq != v->seq || h->rebase != v->rebase) return 3;   /* no history from this game frame */
        double d2 = 0.0;
        for (int i = 0; i < 3; i++) {
            double d = h->cloud_origin_m[i] + h->eye_m[e][i] - v->cloud_origin_m[i] - v->eye_m[e][i];
            d2 += d * d;
        }
        if (d2 > (double)P.reuse_maxmove * P.reuse_maxmove) return 4;             /* a jump */
    }
    if (ps2_clouds_flash_count() > 0) return 5;                                   /* flash lights pulse per frame */
    return 0;
}
/* the history camera for this pass (cloud_near_reuse.comp NearHist): the game frame's axes, its eyes in
   THIS pass's cloud space (the cloud frame: the clouds stand still in it), its projection, its crops */
static void reuse_hist(const ps2_cloud_rec *r) {
    float *h = hist_tmp;
    const ps2_cloud_view *v = r->view;
    memset(h, 0, NEAR_HIST_VEC4 * 16u);
    const ps2_cloud_view *h0 = &H.view[r->eye < 0 ? 0 : r->eye];
    for (int i = 0; i < 3; i++) { h[0 + i] = h0->cam_x[i]; h[4 + i] = h0->cam_y[i]; h[8 + i] = h0->cam_z[i]; }
    for (int e = 0; e < 2; e++) {
        const ps2_cloud_view *he = &H.view[e];
        for (int i = 0; i < 3; i++)
            h[12 + 4 * e + i] = (float)((double)he->eye_m[e][i] + (he->cloud_origin_m[i] - v->cloud_origin_m[i]));
        memcpy(&h[24 + 4 * e], he->vis[e], 16);
    }
    memcpy(&h[20], h0->proj, 16);
}

int ps2_clouds_near_record(const ps2_cloud_gpu *g, VkCommandBuffer cmd, const ps2_cloud_rec *r) {
    if (!near_pipe || !g->near_w || !r->env || !r->view) return 0;
    const ps2_cloud_cfg *c = ps2_clouds_cfg();
    float nearest;
    int on = near_active(r->env, r->view, c->near_range_m, &nearest) || !P.gate;
    S.frames++;
    if (on) S.active++;
    if (c->debug > 0 && (S.frames % (uint64_t)c->debug == 0u || on != S.last))
        ps2_log("clouds-near: %s (alt %.0f m, band %.0f..%.0f, nearest cloud %.0f m, %u clouds), active %.0f%% of %llu",
                on ? "marching" : "idle", r->view->cloud_origin_m[1], r->env->base_m, r->env->top_m, nearest,
                (r->env->valid & CL_ENV_LIST) ? r->env->ncloud : 0u, 100.0 * (double)S.active / (double)S.frames,
                (unsigned long long)S.frames);
    S.last = on; S.nearest = nearest;
    int inb = r->view->inbetween != 0;
    if (inb) reuse_inb_frame = r->frame | 1u;
    if (!on && !inb)                             /* (ACZ_VR_CLOUD_NEAR_REUSE) a game frame without a march: no history */
        for (int e = r->eye < 0 ? 0 : r->eye; e <= (r->eye < 0 ? 1 : r->eye); e++) H.valid[e] = 0;
    {   /* the in-cloud value of this slot's previous use (its fence has passed): our rendered veil around the eye */
        uint32_t slot = (uint32_t)r->frame_index % NEAR_SLOTS;
        float ic = probe_used[slot] ? probe_map[slot * 4u] : 0.0f;
        if (!on) { ic = 0.0f; probe_used[slot] = 0; }
        ps2_clouds_report_in_cloud(ic < 0.0f ? 0.0f : ic > 1.0f ? 1.0f : ic);
    }
    uint32_t slot = (uint32_t)r->frame_index % NEAR_SLOTS;
    if (tq && tq_used[slot]) {                   /* this slot's previous timing (its fence has passed) */
        uint64_t ts[2];
        if (vkGetQueryPoolResults(g->dev, tq, 2u * slot, 2, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            int md = tq_mode[slot];
            AB.sum[md] += (double)(ts[1] - ts[0]) * tq_period_ns * 1e-6; AB.n[md]++;
            if (P.reuse) {
                int kd = tq_kind[slot];
                double t = (double)(ts[1] - ts[0]) * tq_period_ns * 1e-6;
                RS.ms[kd] += t; RS.n[kd]++;
                if (kd == 0 || kd == 3) {            /* a game frame's near pass: does the reuse pay? (hysteresis 0.8) */
                    reuse_game_ms = reuse_game_ms < 0.0 ? t : reuse_game_ms + 0.05 * (t - reuse_game_ms);
                    if (!reuse_engaged && reuse_game_ms >= (double)P.reuse_min_ms) { reuse_engaged = 1; RS.engage++; }
                    else if (reuse_engaged && reuse_game_ms < 0.8 * (double)P.reuse_min_ms) reuse_engaged = 0;
                }
            }
            uint32_t need = P.ab || P.fov_ab ? 600u : 1200u;
            if (((P.ab || P.fov_ab) && AB.n[0] >= need && AB.n[1] >= need)
                || (!P.ab && !P.fov_ab && c->debug > 0 && AB.n[md] >= need)) {
                if (P.ab)
                    ps2_log("clouds-near A/B: near pass %.3f ms (checkerboard %d, prefilter %d; %u frames) vs %.3f ms (plain "
                            "march; %u frames): %+.1f%%", AB.sum[0] / AB.n[0], P.cb, P.mip, AB.n[0], AB.sum[1] / AB.n[1],
                            AB.n[1], 100.0 * (AB.sum[0] / AB.n[0] / (AB.sum[1] / AB.n[1]) - 1.0));
                else if (P.fov_ab)
                    ps2_log("clouds-near FOV A/B: near pass %.4f ms (coarse beyond %.0f deg; %u frames) vs %.4f ms (fine "
                            "everywhere; %u frames): %+.4f ms (%+.1f%%)", AB.sum[0] / AB.n[0], P.fov_deg, AB.n[0],
                            AB.sum[1] / AB.n[1], AB.n[1], AB.sum[0] / AB.n[0] - AB.sum[1] / AB.n[1],
                            100.0 * (AB.sum[0] / AB.n[0] / (AB.sum[1] / AB.n[1]) - 1.0));
                else
                    ps2_log("clouds-near: near pass %.3f ms (own timestamps, %u frames)", AB.sum[md] / AB.n[md], AB.n[md]);
                memset(&AB, 0, sizeof AB);
            }
        }
        tq_used[slot] = 0;
    }
    if (VF.pending && VF.slot == (int)slot) {    /* ACZ_VR_CLOUD_NEAR_SPEC_VERIFY: the two marches of this slot's last use */
        VF.pending = 0;
        size_t nc = (size_t)VF.w * VF.h * VF.layers, ncb = nc * 8u;
        const uint16_t *ca = (const uint16_t *)VF.map, *cb2 = (const uint16_t *)(VF.map + VF.half);
        const uint16_t *da = (const uint16_t *)(VF.map + ncb), *db = (const uint16_t *)(VF.map + VF.half + ncb);
        uint64_t dc = 0, dd = 0;
        double mx = 0.0;
        for (size_t i = 0; i < nc; i++) {
            if (memcmp(ca + 4 * i, cb2 + 4 * i, 8)) {
                dc++;
                for (int k = 0; k < 4; k++) {
                    double x = fabs((double)(int)ca[4 * i + k] - (double)(int)cb2[4 * i + k]);   /* (half bits apart) */
                    if (x > mx) mx = x;
                }
            }
            if (da[i] != db[i]) dd++;
        }
        VF.checks++; VF.texels += nc; VF.diff_c += dc; VF.diff_d += dd;
        if (VF.inb) VF.checks_inb++;
        if (dc || dd) VF.bad++;
        if (mx > VF.maxd) VF.maxd = mx;
        if (dc || dd)
            ps2_log("clouds-near spec verify: pass %llu (%s frame, NEAR_SPEC %d vs 0, divisor %d): %llu colour / %llu depth texels "
                    "differ of %zu, max %.0f half-float steps", (unsigned long long)VF.passes, VF.inb ? "in-between" : "game",
                    VF.spec, VF.div, (unsigned long long)dc, (unsigned long long)dd, nc, mx);
        if (VF.checks % 20u == 0u)
            ps2_log("clouds-near spec verify: %llu checks (%llu in-between frames), %llu with a difference; %llu texels "
                    "compared, %llu colour / %llu depth differ (NEAR_SPEC %d against the generic march)",
                    (unsigned long long)VF.checks, (unsigned long long)VF.checks_inb, (unsigned long long)VF.bad,
                    (unsigned long long)VF.texels, (unsigned long long)VF.diff_c, (unsigned long long)VF.diff_d, P.spec);
    }
    if (P.reuse && stat_used[slot]) {            /* this slot's previous re-march counters (its fence has passed) */
        RS.blk_vis += stat_map[slot * 4u]; RS.blk_bad += stat_map[slot * 4u + 1u];
        stat_used[slot] = 0;
    }
    if (P.reuse && ++RS.frames % 1200u == 0u && RS.inb) {
        ps2_log("clouds-near reuse: near pass %.4f ms game frames (%u; %.4f ms without the reprojection depth, %u), %.4f ms "
                "in-between reprojected (%u), %.4f ms in-between marched (%u); %llu in-between passes, %.1f %% reprojected "
                "(full march: A/B or flip %llu, new images %llu, no history %llu, jump %llu, flashes %llu, cheap near pass "
                "%llu; game near pass mean %.3f ms, engaged %d, %llu times); %.1f %% of the visible blocks marched in full",
                RS.n[0] ? RS.ms[0] / RS.n[0] : 0.0, RS.n[0], RS.n[3] ? RS.ms[3] / RS.n[3] : 0.0, RS.n[3],
                RS.n[1] ? RS.ms[1] / RS.n[1] : 0.0, RS.n[1],
                RS.n[2] ? RS.ms[2] / RS.n[2] : 0.0, RS.n[2], (unsigned long long)RS.inb,
                100.0 * (double)RS.reproj / (double)RS.inb, (unsigned long long)RS.why[1], (unsigned long long)RS.why[2],
                (unsigned long long)RS.why[3], (unsigned long long)RS.why[4], (unsigned long long)RS.why[5],
                (unsigned long long)RS.why[6], reuse_game_ms, reuse_engaged, (unsigned long long)RS.engage,
                RS.blk_vis ? 100.0 * (double)RS.blk_bad / (double)RS.blk_vis : 0.0);
        memset(&RS, 0, sizeof RS);
    }
    if (!on) return 0;
    /* CLOUD DETAIL (perf6_clouds): this frame's level; its divisor sets the near buffer's used part (ps2_clouds.c:
       g->near_lw x g->near_lh, the CloudFrame sizes) */
    int lvl = ps2_clouds_detail_cur();
    if (lvl < 0 || lvl >= P.nl) lvl = 0;
    int div = g->near_div >= 2 ? g->near_div : 2;
    uint32_t nw = g->near_lw ? g->near_lw : g->near_w, nh = g->near_lh ? g->near_lh : g->near_h;
    /* ACZ_VR_CLOUD_NEAR_REUSE: a game frame writes its march beyond the first tier node and the reprojection depth and
       becomes the history of its eye layers; an in-between frame reprojects that when it may, else marches in full */
    int reuse = 0, par = 0;
    int split_ok = !P.blend && !(P.fov_tan2 > 0.0f) && div == 2;   /* (the axis blend / the coarse periphery march twice a
                                                                       texel; cloud_near_reuse.comp: half resolution) */
    if (P.reuse && inb && split_ok) {
        int why = !reuse_engaged ? 6 : reuse_why(g, r);   /* (6: the near pass is cheap: a full march pays better) */
        RS.inb++; RS.why[why]++;
        if (!why && 2u * ((nw + 7u) / 8u) * ((nh + 7u) / 8u) <= NEAR_FLAG_UINTS) { reuse = 1; RS.reproj++; }
    } else if (P.reuse && !inb && split_ok && !reuse_ab_off   /* (the A/B's off blocks: as without the switch) */
               && reuse_inb_frame && r->frame - reuse_inb_frame < 16u   /* (in-between frames are being drawn) */
               && reuse_engaged) {                                    /* (the near pass costs enough) */
        par = 1;
        for (int e = r->eye < 0 ? 0 : r->eye; e <= (r->eye < 0 ? 1 : r->eye); e++) {
            H.valid[e] = 1; H.view[e] = *r->view;
        }
        H.epoch = g->near_epoch;
    } else if (!inb)                             /* a game frame without the history output: no history */
        for (int e = r->eye < 0 ? 0 : r->eye; e <= (r->eye < 0 ? 1 : r->eye); e++) H.valid[e] = 0;
    int mode = P.ab ? (int)((AB.frames++ / (uint64_t)P.ab) & 1u) : 0;   /* 1 = the plain march (A/B) */
    int cb = mode ? 0 : P.cb, mip = mode ? 0 : P.mip, blend = mode ? 1 : P.blend;   /* plain = the old defaults */
    /* the coarse periphery: off in the A/B's other blocks (own: ACZ_VR_CLOUD_NEAR_FOV_AB, mode 1; the frame's:
       PS2_PASS_AB_WHAT=sparse) and in the PS2_PERF_AB re-draw with PS2_PERF_AB_FLIP=sparse */
    extern int ps2_vk_pass_ab_off(int what);
    extern int ps2_vk_perf_ab_flipped(const char *name);
    if (P.fov_ab) mode = (int)((AB.frames++ / (uint64_t)P.fov_ab) & 1u);
    float ftan2 = !(P.fov_ab && mode) && !ps2_vk_pass_ab_off(2) && !ps2_vk_perf_ab_flipped("sparse") && div == 2
                  ? P.fov_tan2 : 0.0f;
    /* push constants (cloud_near.comp): i0 eye layer / flags / cap / rows, i1 per-eye columns, f0 tiers, f1 medium */
    struct { int32_t i0[4], i1[4]; float f0[4], f1[4]; } pc;
    memset(&pc, 0, sizeof pc);
    pc.i0[0] = r->eye < 0 ? 0 : r->eye;
    pc.i0[1] = (blend ? 1 : 0) | (P.light ? 2 : 0) | (P.view ? 4 : 0) | (mip ? 32 : 0) | (par ? 256 : 0) | (reuse ? 512 : 0)
             | (reuse && P.reuse_view ? 1024 : 0);
    pc.i0[2] = P.L[lvl].cap;
    pc.i0[3] = (int32_t)((uint32_t)r->frame_index % NEAR_SLOTS);
    memcpy(&pc.i1[0], &P.lump_win, 4); memcpy(&pc.i1[1], &P.lump_scale, 4);   /* float bits; 0 = shader defaults */
    memcpy(&pc.i1[2], &P.contrast, 4);
    memcpy(&pc.i1[3], &ftan2, 4);
    if (ftan2 > 0.0f) pc.i0[1] |= 64;                                           /* (the coarse periphery) */
    {   /* PS2_PASS_MASK with a lens (the runtime's XR_KHR_visibility_mask, or the test lens): its hidden blocks */
        extern int ps2_vk_pm_lens_blocks(uint32_t *bits, int bw, int bh, int bp);
        int bw = (int)(nw + 7u) / 8, bh = (int)(nh + 7u) / 8;
        if (lens_map && 2 * bw * bh <= (int)NEAR_LENS_UINTS * 32
            && ps2_vk_pm_lens_blocks(lens_map + (size_t)pc.i0[3] * NEAR_LENS_UINTS, bw, bh, 8 * div)) {
            pc.i0[1] |= 128;
            if (P.lens_dev) {                                       /* (to the shaders' device-local copy) */
                uint32_t nu = ((uint32_t)(2 * bw * bh) + 31u) / 32u;
                vkCmdUpdateBuffer(cmd, lens_dev, (VkDeviceSize)pc.i0[3] * NEAR_LENS_UINTS * 4u, (VkDeviceSize)nu * 4u,
                                  lens_map + (size_t)pc.i0[3] * NEAR_LENS_UINTS);
                VkMemoryBarrier lb; memset(&lb, 0, sizeof lb);
                lb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                lb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; lb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &lb,
                                     0, NULL, 0, NULL);
            }
        }
    }
    /* the visible part of each eye comes from C2 vis[] through the CloudFrame (cl.vis) */
    pc.f0[0] = P.r0; pc.f0[1] = P.L[lvl].s0; pc.f0[2] = P.L[lvl].d0; pc.f0[3] = P.L[lvl].q;
    pc.f1[0] = P.sigma; pc.f1[1] = P.L[lvl].detail; pc.f1[2] = P.detail_period; pc.f1[3] = P.detail_fade;
    /* the pipeline (perf6_clouds): the level's NEAR_SPEC (generic in the exact A/B's re-draw with
       PS2_PERF_AB_FLIP=nearspec), this pass's flags, the divisor */
    int spec = P.L[lvl].spec;
    if (ps2_clouds_trim_off()) spec = 0;
    if (tq) {
        vkCmdResetQueryPool(cmd, tq, 2u * slot, 2);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, tq, 2u * slot);
    }
    uint32_t layers = r->eye < 0 ? 2u : 1u;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, near_layout, 1, 1, &probe_set, 0, NULL);
    if (reuse) {                                 /* the reprojection pass: every texel's far part, the flags per block */
        uint32_t hidx = slot * 2u + (r->eye == 1 ? 1u : 0u);
        reuse_hist(r);
        vkCmdUpdateBuffer(cmd, hist_buf, (VkDeviceSize)hidx * NEAR_HIST_VEC4 * 16u, NEAR_HIST_VEC4 * 16u, hist_tmp);
        if (!stat_used[slot]) vkCmdFillBuffer(cmd, stat_dev, (VkDeviceSize)slot * 16u, 16u, 0u);   /* (first pass of the slot) */
        VkMemoryBarrier ub; memset(&ub, 0, sizeof ub);
        ub.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        ub.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; ub.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &ub, 0, NULL, 0, NULL);
        struct { int32_t i0[4], i1[4]; float f0[4], f1[4]; } rp;
        memset(&rp, 0, sizeof rp);
        rp.i0[0] = pc.i0[0]; rp.i0[1] = pc.i0[1] & 128; rp.i0[3] = pc.i0[3];
        rp.i1[0] = (int32_t)((nw + 7u) / 8u); rp.i1[1] = (int32_t)((nh + 7u) / 8u); rp.i1[2] = (int32_t)hidx;
        rp.f0[0] = P.reuse_tol; rp.f0[1] = P.reuse_disp;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, reuse_pipe);
        vkCmdPushConstants(cmd, near_layout, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, CL_PUSH_BYTES, &rp);
        vkCmdDispatch(cmd, (nw + 7u) / 8u, (nh + 7u) / 8u, layers);
        VkMemoryBarrier rb; memset(&rb, 0, sizeof rb);
        rb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        rb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; rb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &rb,
                             0, NULL, 0, NULL);
        stat_used[slot] = 1;
    }
    VkPipeline np0 = ftan2 > 0.0f && near_pipe_c ? near_pipe_c : near_get(spec, pc.i0[1] | (cb ? 8 : 0), div);
    if (!np0) np0 = near_pipe;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, np0);
    if (!cb) {
        vkCmdPushConstants(cmd, near_layout, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, CL_PUSH_BYTES, &pc);
        vkCmdDispatch(cmd, (nw + 7u) / 8u, (nh + 7u) / 8u, layers);
    } else {                                     /* checkerboard: pass A (even texels), then pass B (odd: fill or march) */
        pc.i0[1] |= 8;
        vkCmdPushConstants(cmd, near_layout, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, CL_PUSH_BYTES, &pc);
        vkCmdDispatch(cmd, ((nw + 1u) / 2u + 7u) / 8u, (nh + 7u) / 8u, layers);
        VkMemoryBarrier mb0; memset(&mb0, 0, sizeof mb0);
        mb0.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb0.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; mb0.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb0,
                             0, NULL, 0, NULL);
        pc.i0[1] |= 16;
        VkPipeline np1 = ftan2 > 0.0f && near_pipe_c ? near_pipe_c : near_get(spec, pc.i0[1], div);
        if (np1 && np1 != np0) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, np1);
        vkCmdPushConstants(cmd, near_layout, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, CL_PUSH_BYTES, &pc);
        vkCmdDispatch(cmd, ((nw + 1u) / 2u + 7u) / 8u, (nh + 7u) / 8u, layers);
    }
    if (tq) {
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, tq, 2u * slot + 1u);
        tq_used[slot] = 1; tq_mode[slot] = (uint8_t)mode;
        tq_kind[slot] = (uint8_t)(!inb ? (par ? 0 : 3) : reuse ? 1 : 2);
    }
    /* ACZ_VR_CLOUD_NEAR_SPEC_VERIFY: the near images as marched; the same march again with the generic pipelines; the
       images again (not with the reuse's reprojection, whose input the first march overwrote) */
    if (VF.every && !VF.pending && !reuse && spec != 0 && !(ftan2 > 0.0f) && (VF.passes++ % (uint64_t)VF.every) == 0u
        && g->near_img && g->near_depth) {
        VkDeviceSize need = (VkDeviceSize)g->near_w * g->near_h * CL_NEAR_LAYERS * 10u;
        if (VF.buf && VF.half < need) {
            vkDestroyBuffer(g->dev, VF.buf, NULL); vkFreeMemory(g->dev, VF.mem, NULL);
            VF.buf = VK_NULL_HANDLE; VF.mem = VK_NULL_HANDLE; VF.map = NULL;
        }
        if (!VF.buf && ps2_clouds_make_buffer(2u * need, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 2, &VF.buf, &VF.mem,
                                              (void **)&VF.map) == 0)
            VF.half = need;
        if (VF.buf) {
            uint32_t ly = layers, l0 = (uint32_t)(r->eye < 0 ? 0 : r->eye);
            VkMemoryBarrier vb; memset(&vb, 0, sizeof vb);
            vb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            for (int pass = 0; pass < 2; pass++) {
                vb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; vb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &vb, 0,
                                     NULL, 0, NULL);
                VkBufferImageCopy bc[2]; memset(bc, 0, sizeof bc);
                for (int k = 0; k < 2; k++) {
                    bc[k].bufferOffset = (VkDeviceSize)pass * VF.half + (k ? (VkDeviceSize)nw * nh * ly * 8u : 0u);
                    bc[k].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    bc[k].imageSubresource.baseArrayLayer = l0; bc[k].imageSubresource.layerCount = ly;
                    bc[k].imageExtent.width = nw; bc[k].imageExtent.height = nh; bc[k].imageExtent.depth = 1;
                }
                vkCmdCopyImageToBuffer(cmd, g->near_img, VK_IMAGE_LAYOUT_GENERAL, VF.buf, 1, &bc[0]);
                vkCmdCopyImageToBuffer(cmd, g->near_depth, VK_IMAGE_LAYOUT_GENERAL, VF.buf, 1, &bc[1]);
                if (pass) break;
                vb.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; vb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &vb, 0,
                                     NULL, 0, NULL);
                /* the generic march: the same push constants and dispatches */
                int f0 = cb ? (pc.i0[1] & ~(8 | 16)) : pc.i0[1];
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, near_layout, 1, 1, &probe_set, 0, NULL);
                if (!cb) {
                    pc.i0[1] = f0;
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, near_get(0, f0, div));
                    vkCmdPushConstants(cmd, near_layout, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, CL_PUSH_BYTES, &pc);
                    vkCmdDispatch(cmd, (nw + 7u) / 8u, (nh + 7u) / 8u, layers);
                } else {
                    pc.i0[1] = f0 | 8;
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, near_get(0, pc.i0[1], div));
                    vkCmdPushConstants(cmd, near_layout, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, CL_PUSH_BYTES, &pc);
                    vkCmdDispatch(cmd, ((nw + 1u) / 2u + 7u) / 8u, (nh + 7u) / 8u, layers);
                    vb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; vb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &vb,
                                         0, NULL, 0, NULL);
                    pc.i0[1] = f0 | 8 | 16;
                    vkCmdPushConstants(cmd, near_layout, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, CL_PUSH_BYTES, &pc);
                    vkCmdDispatch(cmd, ((nw + 1u) / 2u + 7u) / 8u, (nh + 7u) / 8u, layers);
                }
            }
            vb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; vb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &vb, 0, NULL, 0, NULL);
            VF.pending = 1; VF.slot = (int)slot; VF.w = nw; VF.h = nh; VF.layers = ly; VF.spec = spec; VF.div = div;
            VF.inb = inb;
        }
    }
    if (reuse) {                                 /* the reuse counters to the host buffer (read when the slot comes round) */
        VkMemoryBarrier cb0; memset(&cb0, 0, sizeof cb0);
        cb0.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        cb0.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; cb0.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &cb0, 0, NULL, 0, NULL);
        VkBufferCopy bc = { (VkDeviceSize)slot * 16u, (VkDeviceSize)slot * 16u, 16u };
        vkCmdCopyBuffer(cmd, stat_dev, stat_buf, 1, &bc);
        cb0.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; cb0.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &cb0, 0, NULL, 0, NULL);
    }
    {   /* the probe write, visible to the host once this frame's fence has passed */
        VkMemoryBarrier mb; memset(&mb, 0, sizeof mb);
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;   /* (+ the counters) */
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    }
    probe_used[slot] = 1;
    return 1;
}

void ps2_clouds_near_shutdown(const ps2_cloud_gpu *g) {
    if (VF.buf) { vkDestroyBuffer(g->dev, VF.buf, NULL); vkFreeMemory(g->dev, VF.mem, NULL); VF.buf = VK_NULL_HANDLE; VF.map = NULL; }
    for (int i = 0; i < np_n; i++) if (NP[i].p && NP[i].p != near_pipe) vkDestroyPipeline(g->dev, NP[i].p, NULL);
    np_n = 0;
    if (near_pipe) vkDestroyPipeline(g->dev, near_pipe, NULL);
    near_pipe = VK_NULL_HANDLE;
    if (near_pipe_c) vkDestroyPipeline(g->dev, near_pipe_c, NULL);
    near_pipe_c = VK_NULL_HANDLE;
    if (reuse_pipe) vkDestroyPipeline(g->dev, reuse_pipe, NULL);
    reuse_pipe = VK_NULL_HANDLE;
    VkBuffer rb[4] = { flag_buf, hist_buf, stat_buf, stat_dev };
    VkDeviceMemory rm[4] = { flag_mem, hist_mem, stat_mem, stat_dmem };
    for (int k = 0; k < 4; k++) { if (rb[k]) vkDestroyBuffer(g->dev, rb[k], NULL); if (rm[k]) vkFreeMemory(g->dev, rm[k], NULL); }
    flag_buf = hist_buf = stat_buf = stat_dev = VK_NULL_HANDLE; flag_mem = hist_mem = stat_mem = stat_dmem = VK_NULL_HANDLE;
    stat_map = NULL;
    if (near_layout) vkDestroyPipelineLayout(g->dev, near_layout, NULL);   /* ours: {common set 0, probe set 1} */
    near_layout = VK_NULL_HANDLE;
    if (tq) vkDestroyQueryPool(g->dev, tq, NULL);
    tq = VK_NULL_HANDLE;
    if (probe_pool) vkDestroyDescriptorPool(g->dev, probe_pool, NULL);
    if (probe_dsl) vkDestroyDescriptorSetLayout(g->dev, probe_dsl, NULL);
    if (probe_buf) vkDestroyBuffer(g->dev, probe_buf, NULL);
    if (probe_mem) vkFreeMemory(g->dev, probe_mem, NULL);
    if (lens_buf) vkDestroyBuffer(g->dev, lens_buf, NULL);
    if (lens_mem) vkFreeMemory(g->dev, lens_mem, NULL);
    if (lens_dev) vkDestroyBuffer(g->dev, lens_dev, NULL);
    if (lens_dmem) vkFreeMemory(g->dev, lens_dmem, NULL);
    lens_buf = VK_NULL_HANDLE; lens_mem = VK_NULL_HANDLE; lens_map = NULL; lens_dev = VK_NULL_HANDLE; lens_dmem = VK_NULL_HANDLE;
    probe_pool = VK_NULL_HANDLE; probe_dsl = VK_NULL_HANDLE; probe_buf = VK_NULL_HANDLE; probe_mem = VK_NULL_HANDLE;
    probe_map = NULL;
}
