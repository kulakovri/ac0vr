// ps2_clouds_far.c - S2 far cloud renderer (CLOUDS_3D_RESEARCH_2026-10-20.md 8.1/8.2; contracts in ps2_clouds.h):
// the weather window (C3 weather: the splat of the game's cloud list with its density softened across the borders
// between clouds (PASS_WSOFT, bug #31), or the procedural field) and the update of the
// direction map cloud_dir / cloud_dir_depth (cloud_far.comp, cloud_far.glsl).
//
// The map is marched from the centre eye, so head rotation never needs an update; what goes stale is the parallax of
// the aircraft's translation. The scheduler (cloud_far.comp PASS_TILES, on the GPU, one thread per 8x8-texel tile)
// keeps per tile the eye position of its last march and the distance of its nearest cloud, and re-marches a tile
// when the stored content has moved by more than ACZ_VR_CLOUD_FAR_ERR eye pixels (near clouds abeam: every frame or
// two; far ones rarely), at least every ACZ_VR_CLOUD_FAR_SLICE frames (staggered, so colours and the weather keep
// up), and with looser limits outside both eyes' views ("visible first"). Tiles whose rays cannot reach the band are
// written once. The march is an indirect dispatch over the listed tiles. Bench numbers: tools/cloudbench/farbench.c.
//
// Frames in flight: the host-written buffers (parameters, the cloud list) have one copy per frame index; the tile
// metadata and the lists live on the GPU only and are ordered by barriers (one queue).
//
// 2026-10-22 (bugs #33 / #35 / #36 / #40 / #57, agent bx_cloudfield): an empty C1 list stays the list path (clear sky,
// fades continue, no procedural field, no new generation: ACZ_VR_CLOUD_LIST_KEEP), list changes ramp over
// ACZ_VR_CLOUD_LIST_RAMP seconds, the splat weighs the clouds by profile x opacity (ACZ_VR_CLOUD_SPLAT_MIX), and the
// governor's level 1 loosens every tile's limits instead of leaving most tiles stale (ACZ_VR_GOV_FAR_MODE).
#include "ps2_runtime.h"
#include "ps2_clouds.h"
#include "ps2_vkcen.h"          /* (PS2_VK_CENSUS: vkCmd* calls counted) */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

u64 ps2_kernel_vblank_count(void);
#define FAR_FIF 4u                /* frame indices handed to the hooks (ps2_clouds.c CL_FIF) */
#define FAR_MAXCL 512u            /* cloud list capacity (C1 CL_MAX_CLOUDS = 400) */

/* set 1 of the far pipelines (cloud_far.glsl ClFarParams, cloud_far.comp ClTiles / ClList / ClClouds) */
typedef struct { float mz[4], spl[4], haze[4], stp[4], sch[4], fine[4], win[4], ext[4], dbg[4]; } far_params;
typedef struct { float pos_r[4], info[4]; } far_cloud;
typedef struct { float cam[4]; uint32_t st[4]; } far_tile;
typedef struct { float cam[4], march[4], sched[4]; uint32_t fr[4]; } far_push;
/* a cloud of C1's list as the splat sees it: identified by its cloud-frame position and radius (stable across S1's
   re-centres), with its opacity eased toward the published value */
typedef struct { double x, z; float y, r, h, cur, tgt; uint32_t var, seed; int seen; float from, age, dur, vel; } far_track;
_Static_assert(sizeof(far_push) <= CL_PUSH_BYTES, "far push constants");
static int far_gov_on;                    /* the renderer's frame-time governor sheds (ps2_clouds_far_gov) */
/* PS2_GPU_TRACE (diagnostic, ps2_clouds_far_trace): per frame slot what the far module recorded -- bit 0 the weather
   splat, bit 1 its soft pass, bit 2 a new generation (every tile due), bit 3 the march, bits 4-5 the governor state the
   map followed (1 level 0, 2 level 1 relaxed, 3 level 1 capped); the tile counters are copied to the host slot as
   with ACZ_DEBUG_CLOUDS */
static uint8_t far_tr_flags[FAR_FIF];
static int far_trace_on(void) {
    static int t = -1;
    if (t < 0) { const char *e = getenv("PS2_GPU_TRACE"); t = e && *e; }
    return t;
}
static int far_gov_cap(void) {            /* ACZ_VR_GOV_FAR_CAP: tiles a frame while it does (256) */
    static int cap = -1;
    if (cap < 0) { const char *e = getenv("ACZ_VR_GOV_FAR_CAP"); cap = e ? atoi(e) : 256; if (cap < 0) cap = 0; }
    return cap;
}
/* ACZ_VR_GOV_FAR_MODE (bugs #35 / #36 / #57, 2026-10-22): what the governor's level 1 does to the far map.
   1 (default) = relax: the error limit x ACZ_VR_GOV_FAR_ERR (4) and the periodic refresh half as often for every tile,
   the tile cap only on the tiles outside both eyes' views -- every visible tile still meets a (looser) bound.
   0 = the cap of before: at most ACZ_VR_GOV_FAR_CAP tiles a frame of all lists. With 600-2000 tiles due a frame inside
   M1's overcast the cap left most of them stale for seconds, updated in list order: rings and rows of tiles marched
   at different eye positions = the "corridor" of nested frames, stripes and streaks the user saw (all at level >= 1). */
static int far_gov_mode(void) {
    static int m = -1;
    if (m < 0) { const char *e = getenv("ACZ_VR_GOV_FAR_MODE"); m = e && *e ? (atoi(e) != 0) : 1; }
    return m;
}
/* ACZ_VR_GOV_FAR_AB=N [measurement]: every N far frames the far map's governor state rotates level 0 / level 1 relaxed /
   level 1 capped (whatever the governor says), and the far update's own GPU time (timestamps around the tile classify,
   args and march) is logged per state, the first quarter of each phase dropped (a cap phase leaves a backlog) */
static int far_ab(void) {
    static int n = -1;
    if (n < 0) { const char *e = getenv("ACZ_VR_GOV_FAR_AB"); n = e && *e ? atoi(e) : 0; if (n < 0) n = 0; }
    return n;
}
/* ACZ_VR_GOV_FAR_AB_SEQ="0,1" [measurement]: the states the rotation visits (0 level 0, 1 relaxed, 2 capped; default
   0,1,2): "0,1" = the current governor's step up and down only (the transitions' own spikes in PS2_GPU_TRACE) */
static int far_ab_phase(uint32_t frames) {
    static int seq[8], ns = -1;
    if (ns < 0) {
        const char *e = getenv("ACZ_VR_GOV_FAR_AB_SEQ");
        ns = 0;
        for (const char *p = e; p && *p && ns < 8; p++) if (*p >= '0' && *p <= '2') seq[ns++] = *p - '0';
        if (ns == 0) { seq[0] = 0; seq[1] = 1; seq[2] = 2; ns = 3; }
    }
    int n = far_ab();
    return n > 0 ? seq[(frames / (uint32_t)n) % (uint32_t)ns] : -1;
}
/* the governor state the far map follows this frame: on = level >= 1, relax = ACZ_VR_GOV_FAR_MODE */
static void far_gov_state(uint32_t frames, int *on, int *relax) {
    int ph = far_ab_phase(frames);
    if (ph >= 0) { *on = ph != 0; *relax = ph == 1; return; }
    *on = far_gov_on; *relax = far_gov_mode();
}
static float far_gov_err(void) {
    static float k = -1.0f;
    if (k < 0.0f) { const char *e = getenv("ACZ_VR_GOV_FAR_ERR"); k = e && *e ? (float)atof(e) : 4.0f; if (k < 1.0f) k = 1.0f; if (k > 8.0f) k = 8.0f; }
    return k;
}

static struct {
    int ready, far_ready;
    VkDevice dev;
    VkDescriptorSetLayout set_layout;
    VkDescriptorPool pool;
    VkDescriptorSet set[FAR_FIF];
    VkPipelineLayout layout;         /* the first pass's layout: binds set 0/1 and push constants for all passes */
    VkPipelineLayout layouts[6]; int nlayouts;   /* every pass's own layout, kept until shutdown (no maintenance4) */
    VkPipeline p_weather, p_wsoft, p_tiles, p_args, p_march;
    VkBuffer host_buf; VkDeviceMemory host_mem; uint8_t *host;     /* per frame index: params | clouds | counters */
    VkBuffer wxc; VkDeviceMemory wxc_mem;   /* the splat's weather image copied for PASS_WSOFT (512^2 rgba8 texels) */
    float soft_m;                           /* ACZ_VR_CLOUD_SPLAT_SOFT: the density's softening radius (m; 0 = off) */
    int imp_alpha;                          /* ACZ_VR_CLOUD_IMP_ALPHA: the list's opacities x C1 imp_alpha (1 on) */
    VkBuffer tiles; VkDeviceMemory tiles_mem;
    VkBuffer list; VkDeviceMemory list_mem;
    uint32_t n, tpr, nt;
    /* scheduler state */
    double anchor[3]; int have_anchor, cleared; uint32_t rebase, gen;
    uint32_t frames;
    /* weather state */
    double splat_origin[3]; int have_splat; uint32_t splat_seq, splat_valid, splat_rebase;
    float splat_key[4];
    far_track trk[FAR_MAXCL]; uint32_t ntrk; int trk_ok; double trk_time; float fade_s, size_k;
    uint32_t fade_ctr; int fade_was;
    uint32_t wx_quiet; float quiet_k;
    float od_cu;                        /* ACZ_VR_CLOUD_OD_CU: the per-cloud optical depth for cumulus-like weather */
    float thin;                         /* ACZ_VR_CLOUD_FAR_THIN: thin-medium step factor (cumulus-like weather) */   /* far frames since the weather content last changed; FAR_SLICE factor then */
    int keep;                           /* ACZ_VR_CLOUD_LIST_KEEP: an empty C1 list is still the list path (see list_path) */
    float ramp_s;                       /* ACZ_VR_CLOUD_LIST_RAMP: seconds of a cloud's fade in / out (see track_target) */
    int ease;                           /* ACZ_VR_CLOUD_LIST_EASE: 1 the follower (track_follow), 0 the restarted ramp */
    float follow_s;                     /* ACZ_VR_CLOUD_LIST_FOLLOW: the follower's 95 % time for a change within the list */
    float near_rate, near_m;            /* ACZ_VR_CLOUD_AROUND_RATE / _M: the opt-in rate cap near the eye (0 = off) */
    float mix;                          /* ACZ_VR_CLOUD_SPLAT_MIX: opacity-weighted splat (cloud_far.comp PASS_WEATHER) */
    /* switches */
    float err_px, start_m, max_m, max_m_list, k_step, dt_min, margin_rad;
    int cap, debug, ovc_res, ovc_steps, list_res;
    uint32_t n_used;                 /* the map size the last frame marched (a change = a new generation) */
    uint32_t n_ubo; int ubo_asked;   /* the size ps2_clouds.c last wrote to CloudFrame sizes.x (via the getter) */
    /* ACZ_DEBUG_CLOUD_FAR_STALE (cl_pop 2026-10-31, bug #260): the visible tiles the scheduler leaves alone, checked
       against a march now (cloud_far.comp PASS_TILES); per frame slot the used tile count of its frame, sums */
    int stale; uint32_t st_nt[FAR_FIF], st_field[FAR_FIF]; uint8_t st_used[FAR_FIF]; uint64_t st_sum[6], st_frames, st_worst; uint32_t st_worst_field;
    /* ACZ_DEBUG_CLOUDS: the weather pass's own GPU time (2 timestamps per frame index around the splat + soft pass) */
    VkQueryPool wq; float wq_ns; uint8_t wq_used[FAR_FIF]; double wq_ms; uint32_t wq_n, wq_calls;
    /* ACZ_VR_GOV_FAR_AB: the far update's own GPU time per governor state (2 timestamps per frame index) */
    VkQueryPool mq; int8_t mq_phase[FAR_FIF]; uint32_t mq_frame[FAR_FIF]; double mq_ms[3]; uint32_t mq_n[3], mq_tiles[3];
    far_params par;
} F;

#define HOST_PARAMS 0u
#define HOST_CLOUDS 256u
#define HOST_COUNTS (HOST_CLOUDS + 16u + FAR_MAXCL * (uint32_t)sizeof(far_cloud))
#define HOST_STALE (HOST_COUNTS + 16u)   /* ACZ_DEBUG_CLOUD_FAR_STALE: 8 counters (cloud_far.comp list[3 NT ..]) */
#define HOST_SLOT ((HOST_COUNTS + 48u + 255u) & ~255u)

static float env_f(const char *n, float def, float lo, float hi) {
    const char *e = getenv(n);
    if (!e || !*e) return def;
    float v = (float)atof(e);
    return v < lo ? lo : v > hi ? hi : v;
}

/* The far march's own parameters (cloud_far.glsl ClFarParams documents each field). Colour, extinction and the "cloud
   above" probe are the shared core in cloud_common.glsl (cl_shade, cl_extinction, cl_above; S5 tunes them through
   CloudFrame user[0] / user[1]). */
static void default_params(far_params *p) {
    memset(p, 0, sizeof *p);
    p->mz[0] = 1.0f; p->mz[1] = 1.0f; p->mz[2] = 2.0f;           /* probe every 2nd fine step */
    p->mz[3] = 0.0f;                                             /* half rate below the hard limit: off (see ACZ_VR_CLOUD_FAR_HALF) */
    p->spl[0] = 0.675f; p->spl[1] = 1.0f; p->spl[2] = 0.45f; p->spl[3] = 0.9f;   /* round, as the game's sprites */
    p->haze[1] = 12000.0f; p->haze[2] = 20000.0f; p->haze[3] = 2500.0f;
    p->stp[0] = 3.0f; p->stp[1] = 800.0f; p->stp[2] = 0.02f; p->stp[3] = 0.35f;
    p->sch[0] = 64.0f; p->sch[1] = 16.0f; p->sch[2] = 0.0f; p->sch[3] = 2.0f;   /* outside the view: re-marched on entering the margin */
    p->fine[0] = 0.5f; p->fine[1] = 3.0f; p->fine[2] = 0.5f; p->fine[3] = 8.0f;   /* envelope billows: 8 puffs */
    p->win[3] = 512.0f / CL_WEATHER_M;
}

static VkPipeline make_pass(const ps2_cloud_gpu *g, const char *spv) {
    VkPipelineLayout lay = VK_NULL_HANDLE;
    (void)g;
    VkPipeline p = ps2_clouds_make_compute(spv, F.set_layout, &lay);
    if (lay && !F.layout) F.layout = lay;
    if (lay && F.nlayouts < 6) F.layouts[F.nlayouts++] = lay;   /* live as long as the pipeline (no maintenance4) */
    return p;
}

/* set 1, the buffers and the weather pipeline: shared by the weather writer and the far renderer */
static int common_init(const ps2_cloud_gpu *g) {
    if (F.ready && F.dev == g->dev) return 0;
    if (F.ready) memset(&F, 0, sizeof F);      /* a new device without our shutdown (near-only mode): start over */
    VkDevice dev = g->dev;
    F.dev = dev;
    /* bindings 0..3, and 6 = the weather copy of PASS_WSOFT (4 / 5 are the cloudbench's own: tools/cloudbench/far) */
    VkDescriptorSetLayoutBinding b[5]; memset(b, 0, sizeof b);
    VkDescriptorType t[5] = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                              VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER };
    for (uint32_t i = 0; i < 5; i++) {
        b[i].binding = i < 4 ? i : 6u; b[i].descriptorType = t[i]; b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo lc; memset(&lc, 0, sizeof lc);
    lc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO; lc.bindingCount = 5; lc.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev, &lc, NULL, &F.set_layout) != VK_SUCCESS) return -1;
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, FAR_FIF }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 * FAR_FIF } };
    VkDescriptorPoolCreateInfo pc; memset(&pc, 0, sizeof pc);
    pc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO; pc.maxSets = FAR_FIF; pc.poolSizeCount = 2; pc.pPoolSizes = ps;
    if (vkCreateDescriptorPool(dev, &pc, NULL, &F.pool) != VK_SUCCESS) return -1;
    VkDescriptorSetLayout lays[FAR_FIF];
    for (uint32_t i = 0; i < FAR_FIF; i++) lays[i] = F.set_layout;
    VkDescriptorSetAllocateInfo ai; memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO; ai.descriptorPool = F.pool; ai.descriptorSetCount = FAR_FIF; ai.pSetLayouts = lays;
    if (vkAllocateDescriptorSets(dev, &ai, F.set) != VK_SUCCESS) return -1;
    void *mapped = NULL;
    if (ps2_clouds_make_buffer((VkDeviceSize)HOST_SLOT * FAR_FIF, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                               VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1, &F.host_buf, &F.host_mem, &mapped) != 0) return -1;
    F.host = mapped;
    memset(F.host, 0, (size_t)HOST_SLOT * FAR_FIF);
    /* tiles and lists: sized for the configured map (the far renderer may be off: then 8 texels, unused) */
    F.n = g->dir_res ? g->dir_res : 8u;
    F.tpr = F.n / 8u; F.nt = F.tpr * F.tpr;
    if (ps2_clouds_make_buffer((VkDeviceSize)F.nt * sizeof(far_tile), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                               0, &F.tiles, &F.tiles_mem, NULL) != 0) return -1;
    if (ps2_clouds_make_buffer(32 + (VkDeviceSize)3 * F.nt * 4 + 32, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                               VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 0, &F.list, &F.list_mem, NULL) != 0) return -1;
    if (ps2_clouds_make_buffer((VkDeviceSize)CL_WEATHER_RES * CL_WEATHER_RES * 4u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                               VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0, &F.wxc, &F.wxc_mem, NULL) != 0) return -1;
    for (uint32_t i = 0; i < FAR_FIF; i++) {
        VkDescriptorBufferInfo bi[5];
        bi[0].buffer = F.host_buf; bi[0].offset = (VkDeviceSize)HOST_SLOT * i + HOST_PARAMS; bi[0].range = sizeof(far_params);
        bi[1].buffer = F.tiles; bi[1].offset = 0; bi[1].range = VK_WHOLE_SIZE;
        bi[2].buffer = F.list; bi[2].offset = 0; bi[2].range = VK_WHOLE_SIZE;
        bi[3].buffer = F.host_buf; bi[3].offset = (VkDeviceSize)HOST_SLOT * i + HOST_CLOUDS;
        bi[3].range = 16 + FAR_MAXCL * sizeof(far_cloud);
        bi[4].buffer = F.wxc; bi[4].offset = 0; bi[4].range = VK_WHOLE_SIZE;
        VkWriteDescriptorSet w[5]; memset(w, 0, sizeof w);
        for (uint32_t k = 0; k < 5; k++) {
            w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[k].dstSet = F.set[i]; w[k].dstBinding = b[k].binding;
            w[k].descriptorCount = 1; w[k].descriptorType = t[k]; w[k].pBufferInfo = &bi[k];
        }
        vkUpdateDescriptorSets(dev, 5, w, 0, NULL);
    }
    F.p_weather = make_pass(g, "cloud_far_weather.comp.spv");
    if (!F.p_weather) return -1;
    F.p_wsoft = make_pass(g, "cloud_far_wsoft.comp.spv");     /* (missing: the splat stays as it is) */
    default_params(&F.par);
    F.err_px = env_f("ACZ_VR_CLOUD_FAR_ERR", 1.0f, 0.05f, 16.0f);
    F.cap = (int)env_f("ACZ_VR_CLOUD_FAR_CAP", 0.0f, 0.0f, 1.0e7f);
    F.start_m = env_f("ACZ_VR_CLOUD_FAR_START", -1.0f, -1.0f, 5000.0f);
    F.max_m = env_f("ACZ_VR_CLOUD_FAR_MAX", 20000.0f, 2000.0f, 60000.0f);
    F.max_m_list = env_f("ACZ_VR_CLOUD_FAR_MAX_LIST", 9000.0f, 2000.0f, 60000.0f);
    F.k_step = env_f("ACZ_VR_CLOUD_FAR_KSTEP", 0.03f, 0.005f, 0.2f);
    F.dt_min = 30.0f;
    F.margin_rad = env_f("ACZ_VR_CLOUD_FAR_MARGIN", 10.0f, 0.0f, 90.0f) * 3.14159265f / 180.0f;
    F.debug = (int)env_f("ACZ_DEBUG_CLOUDS", 0.0f, 0.0f, 1.0e6f);
    F.stale = (int)env_f("ACZ_DEBUG_CLOUD_FAR_STALE", 0.0f, 0.0f, 1.0e6f);
    F.par.dbg[0] = F.stale > 0 ? 1.0f : 0.0f;
    if (F.stale > 0)
        ps2_log("clouds far: ACZ_DEBUG_CLOUD_FAR_STALE: each visible tile the scheduler leaves alone is checked against a "
                "march of its centre texel (sums every %d frames; frames with >= 4 appearing / vanishing tiles logged)", F.stale);
    F.ovc_res = (int)env_f("ACZ_VR_CLOUD_FAR_RES_OVC", 1536.0f, 0.0f, 4096.0f) & ~7;
    F.ovc_steps = (int)env_f("ACZ_VR_CLOUD_FAR_STEPS_OVC", 48.0f, 0.0f, 255.0f);
    F.list_res = (int)env_f("ACZ_VR_CLOUD_FAR_RES_LIST", 1024.0f, 0.0f, 4096.0f) & ~7;
    F.par.sch[2] = F.margin_rad;
    F.par.sch[3] = env_f("ACZ_VR_CLOUD_FAR_LEVELS", 2.0f, 0.0f, 64.0f);
    F.par.fine[0] = env_f("ACZ_VR_CLOUD_FAR_FINE", 0.5f, 0.1f, 1.0f);
    F.par.fine[1] = env_f("ACZ_VR_CLOUD_OD", 3.0f, 0.0f, 100.0f);
    F.od_cu = env_f("ACZ_VR_CLOUD_OD_CU", 12.0f, 0.0f, 100.0f);
    F.thin = env_f("ACZ_VR_CLOUD_FAR_THIN", 0.5f, 0.0f, 4.0f);
    F.par.ext[0] = env_f("ACZ_VR_CLOUD_FILL_CU", 0.65f, 0.1f, 1.0f);
    F.fade_s = env_f("ACZ_VR_CLOUD_LIST_FADE", 1.0f, 0.0f, 30.0f);
    F.par.fine[2] = env_f("ACZ_VR_CLOUD_BILLOW", F.par.fine[2], 0.0f, 0.9f);
    F.par.spl[0] = env_f("ACZ_VR_CLOUD_BLOB_H", F.par.spl[0], 0.2f, 2.0f);
    /* a factor on C1's semi-axes for measurements (S1 publishes the game's drawn size since CL_CLOUD_SPRITE: keep 1) */
    F.size_k = env_f("ACZ_VR_CLOUD_SIZE", 1.0f, 0.1f, 20.0f);
    F.quiet_k = env_f("ACZ_VR_CLOUD_FAR_QUIET", 4.0f, 1.0f, 16.0f);
    F.par.mz[3] = env_f("ACZ_VR_CLOUD_FAR_HALF", F.par.mz[3], 0.0f, 1.0f);
    /* bug #31: the splat's density ramps over +-SPLAT_SOFT metres across the borders between clouds (PASS_WSOFT; at
       most 16 weather texels = 625 m; 0 = the hard borders of before) */
    F.soft_m = env_f("ACZ_VR_CLOUD_SPLAT_SOFT", 400.0f, 0.0f, 625.0f);
    F.par.ext[1] = F.soft_m * (float)CL_WEATHER_RES / CL_WEATHER_M;
    if (F.p_wsoft && F.par.ext[1] >= 0.5f)
        ps2_log("clouds: weather splat: density softened over %.0f m across the borders between clouds "
                "(ACZ_VR_CLOUD_SPLAT_SOFT=0 turns it off)", (double)F.soft_m);
    /* bug #194 (night_sky 2026-10-29, M11 "the night sky a muddy brown-orange"): the game's impostor build scales each
       cloud impostor's alpha by the stage's cloud alpha (C1 imp_alpha: M11 0.50, M7 0.66, M2/M6/M8 0.75, M12 0.78,
       others 1), so its clouds are that much more transparent; our list drew them at the sprite alpha alone: inside
       M11's smoke layer an opaque brown murk (55,43,37 vs the game's 38,35,34 looking up). 0 = as before */
    F.imp_alpha = (int)env_f("ACZ_VR_CLOUD_IMP_ALPHA", 1.0f, 0.0f, 1.0f);
    ps2_log("clouds: the game's impostor alpha (stage cloud alpha / 128) %s (ACZ_VR_CLOUD_IMP_ALPHA)",
            F.imp_alpha ? "scales the cloud list's opacity" : "ignored: the list at the sprite alpha (as before)");
    /* bugs #33 / #57 / #40 (2026-10-22): the list's continuity and the splat's per-texel choice (see list_path and
       cloud_far.comp PASS_WEATHER) */
    F.keep = (int)env_f("ACZ_VR_CLOUD_LIST_KEEP", 1.0f, 0.0f, 1.0f);
    F.ramp_s = env_f("ACZ_VR_CLOUD_LIST_RAMP", 2.0f, 0.0f, 30.0f);
    /* bug #159 (cl_storm, 2026-10-28): how a tracked cloud follows C1's opacity (see track_follow) */
    F.ease = (int)env_f("ACZ_VR_CLOUD_LIST_EASE", 1.0f, 0.0f, 1.0f);
    F.follow_s = env_f("ACZ_VR_CLOUD_LIST_FOLLOW", 1.0f, 0.1f, 10.0f);
    F.near_rate = env_f("ACZ_VR_CLOUD_AROUND_RATE", 0.0f, 0.0f, 10.0f);
    F.near_m = env_f("ACZ_VR_CLOUD_AROUND_M", 1000.0f, 0.0f, 20000.0f);
    if (F.ramp_s > 0.0f) {
        if (F.ease)
            ps2_log("clouds: list opacities follow C1 smoothly (ACZ_VR_CLOUD_LIST_EASE): entries / exits over %.1f s "
                    "(ACZ_VR_CLOUD_LIST_RAMP), changes within the list 95 %% in %.1f s (ACZ_VR_CLOUD_LIST_FOLLOW)",
                    (double)F.ramp_s, (double)F.follow_s);
        else
            ps2_log("clouds: list opacities restart a %.1f s smoothstep on every C1 change (ACZ_VR_CLOUD_LIST_EASE=0)",
                    (double)F.ramp_s);
    }
    if (F.ramp_s > 0.0f && F.ease && F.near_rate > 0.0f)
        ps2_log("clouds: opacity of clouds within %.0f m of the eye changes at most %.2f per second "
                "(ACZ_VR_CLOUD_AROUND_RATE / _M)", (double)F.near_m, (double)F.near_rate);
    F.mix = env_f("ACZ_VR_CLOUD_SPLAT_MIX", 1.0f, 0.0f, 1.0f);
    F.par.ext[2] = F.mix > 0.5f ? 1.0f : 0.0f;
    ps2_log("clouds: cloud list kept across empty spells %s (ACZ_VR_CLOUD_LIST_KEEP), opacity-weighted splat %s "
            "(ACZ_VR_CLOUD_SPLAT_MIX), governor level 1 on the far map: %s (ACZ_VR_GOV_FAR_MODE)", F.keep ? "on" : "off",
            F.par.ext[2] > 0.5f ? "on" : "off", far_gov_mode() ? "looser limits, cap outside the view only" : "tile cap");
    if (F.debug > 0) {                  /* the weather pass's GPU time (measurement) */
        VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(g->phys, &pp);
        F.wq_ns = pp.limits.timestampPeriod;
        VkQueryPoolCreateInfo qc; memset(&qc, 0, sizeof qc);
        qc.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO; qc.queryType = VK_QUERY_TYPE_TIMESTAMP; qc.queryCount = 2u * FAR_FIF;
        if (vkCreateQueryPool(dev, &qc, NULL, &F.wq) != VK_SUCCESS) F.wq = VK_NULL_HANDLE;
    }
    F.ready = 1;
    return 0;
}

/* Overcast-type weather (C1 type < 0.3, or a high unmasked coverage): soft veils, many semi-transparent layers along
   each ray, so the march is dearer and the detail matters less -> a smaller used map and a step cap. */
static int overcast(const ps2_cloud_env *e) {
    return e && (e->valid & CL_ENV_WEATHER) && (e->type < 0.3f || e->coverage_unmasked > 0.45f);
}
/* The cloud-list path (S1's list, with its tracked fades) instead of the procedural field. Bug #33 (2026-10-22): S1
   publishes an EMPTY list whenever no cloud of the game's field passes its drop threshold (M2: 0-4 of 150 kept while
   flying, 0 for seconds at a time). The old test (ncloud > 0) treated that as "no list": the tracked clouds still
   fading out were dropped at once, the weather map became the procedural field and the far map's used size changed
   (a new generation), and the next listed cloud was "taken as it is" -- full opacity in one frame (M2 field 1978: a
   whole cumulus popped in; V1 09:51:58). ACZ_VR_CLOUD_LIST_KEEP=1 (default): a valid list is the list path even when
   empty (nothing to splat = clear sky; fades continue); 0 = the old test. */
static int list_path(const ps2_cloud_env *e) {
    if (!e || !(e->valid & CL_ENV_LIST)) return 0;
    return e->ncloud > 0 || F.keep;
}
/* The far map's used size N_eff for this frame (ps2_clouds.c puts it into CloudFrame sizes.x before the hooks, and
   cl_dir_tex_uv scales the composite's uv by N_eff / N): ACZ_VR_CLOUD_FAR_RES_LIST (1024) with the game's cloud list
   (S1's soft veils: about 5.7 texels per degree at the horizon are enough), else ACZ_VR_CLOUD_FAR_RES_OVC (1536) for
   overcast weather, else the whole map (the procedural field's cumulus). */
uint32_t ps2_clouds_far_map_size(const ps2_cloud_env *e, uint32_t dir_res) {
    uint32_t n = dir_res;
    int list = list_path(e);
    if (F.ready && F.far_ready && dir_res >= 64u && F.ovc_res >= 64 && overcast(e) && (uint32_t)F.ovc_res < dir_res)
        n = (uint32_t)F.ovc_res;
    if (F.ready && F.far_ready && list && F.list_res >= 64 && (uint32_t)F.list_res < n)
        n = (uint32_t)F.list_res;
    F.n_ubo = n; F.ubo_asked = 1;
    return n;
}

static far_params *slot_params(int fi) { return (far_params *)(F.host + (size_t)HOST_SLOT * (uint32_t)fi + HOST_PARAMS); }

/* the weather window centre relative to the eye, and whether nothing lies beyond it (the cloud-list path) */
/* the per-cloud optical depth for this weather: ACZ_VR_CLOUD_OD for the overcast veils (C1 type <= 0.2), _OD_CU for
   cumulus-like weather (type >= 0.6), smoothly in between; 0 in either = full density */
static float od_eff(const ps2_cloud_env *e) {
    float od = F.par.fine[1], cu = F.od_cu;
    if (od <= 0.0f || cu <= 0.0f) return 0.0f;
    float ty = e && (e->valid & CL_ENV_WEATHER) ? e->type : 0.7f;
    float w = (ty - 0.2f) / 0.4f;
    w = w < 0.0f ? 0.0f : w > 1.0f ? 1.0f : w;
    w = w * w * (3.0f - 2.0f * w);
    return od + (cu - od) * w;
}
/* the march range with the game's cloud list: its clouds fade out by the stage visibility (S1 folds the game's far fade
   into their opacity), so 1.1 x C1 fog_far_m (the bodies reach past their centres), at most ACZ_VR_CLOUD_FAR_MAX_LIST */
static float list_range(const ps2_cloud_env *e) {
    float r = e && (e->valid & CL_ENV_COLOURS) && e->fog_far_m > 1000.0f ? 1.1f * e->fog_far_m : F.max_m_list;
    return r < F.max_m_list ? r : F.max_m_list;
}
static void fill_params(int fi, const ps2_cloud_env *e, const ps2_cloud_view *v) {
    far_params *p = slot_params(fi);
    *p = F.par;
    p->fine[1] = od_eff(e);
    /* thin-medium steps for cumulus-like weather (the list's faint clouds would use up the step budget) */
    {   float ty = e && (e->valid & CL_ENV_WEATHER) ? e->type : 0.7f, w = (ty - 0.2f) / 0.4f;
        w = w < 0.0f ? 0.0f : w > 1.0f ? 1.0f : w;
        p->haze[0] = w > 0.5f ? F.thin : 0.0f; }
    int list = list_path(e);
    /* ext.w: the governor's level 1 caps only the tiles outside the view (ACZ_VR_GOV_FAR_MODE=1; PASS_ARGS) */
    {   int gon, grelax; far_gov_state(F.frames, &gon, &grelax); p->ext[3] = gon && grelax ? 1.0f : 0.0f; }
    if (F.have_splat) {
        p->win[0] = (float)(F.splat_origin[0] - v->cloud_origin_m[0]);
        p->win[1] = (float)(F.splat_origin[2] - v->cloud_origin_m[2]);
    }
    p->win[2] = list ? 1.0f : 0.0f;
    if (list) { p->haze[1] = 1.0e9f; p->haze[2] = 2.0e9f; }   /* no distance fade: S1's opacity carries the game's */
}

static void barrier2(VkCommandBuffer cmd, VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da) {
    VkMemoryBarrier2 mb; memset(&mb, 0, sizeof mb);
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    mb.srcStageMask = ss; mb.srcAccessMask = sa; mb.dstStageMask = ds; mb.dstAccessMask = da;
    VkDependencyInfo di; memset(&di, 0, sizeof di);
    di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO; di.memoryBarrierCount = 1; di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &di);
}
#define CS VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
#define XS VK_PIPELINE_STAGE_2_TRANSFER_BIT
#define RW (VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT)

/* A tracked cloud's new target opacity. ACZ_VR_CLOUD_LIST_RAMP > 0 (bug #33, 2026-10-22): the change runs as a
   smoothstep over ramp x min(1, |change| / 0.1) seconds (at least the constant-rate time below), restarted from the
   current value: a cloud entering or leaving the list (S1's threshold: opacity 0.1, which our volume shows far more
   strongly than the game's 13/128 sprite alpha -- seen edge-on a wide cumulus at 0.125 is mostly opaque) takes the
   whole ramp; S1's 1/64 steps take a fraction of it. 0 = the constant rate of before: ACZ_VR_CLOUD_LIST_FADE seconds
   for a full 0 -> 1 change, so 0 -> 0.125 took 0.125 s (M2: a whole cumulus "appeared from nowhere" in 15 frames). */
static void track_target(far_track *t, float tgt) {
    if (tgt == t->tgt && t->age >= 0.0f) return;
    float d = fabsf(tgt - t->cur);
    float lin = F.fade_s > 0.0f ? d * F.fade_s : 0.0f;
    float dur = F.ramp_s * (d < 0.1f ? d / 0.1f : 1.0f);
    t->from = t->cur; t->tgt = tgt; t->age = 0.0f; t->dur = dur > lin ? dur : lin;
}
/* ACZ_DEBUG_CLOUDS (cl_storm, bug #159): a tracked cloud's surface distance from the eye (m; < 0 inside its body,
   0.9 x its semi-axes as the near gate) and a log line when a cloud near the eye gets a new target */
static double trk_surface(const far_track *t, const ps2_cloud_view *v) {
    double dx = t->x - v->cloud_origin_m[0], dz = t->z - v->cloud_origin_m[2], dy = (double)t->y - v->cloud_origin_m[1];
    double a = 0.9 * t->r, b = t->h > 0.0f ? 0.9 * t->h : a;
    if (a < 1.0) a = 1.0;
    if (b < 1.0) b = 1.0;
    double k = sqrt((dx * dx + dz * dz) / (a * a) + dy * dy / (b * b));
    return (k - 1.0) * (a < b ? a : b);
}
static void trk_note(const far_track *t, const ps2_cloud_view *v, float tgt, const char *why) {
    double s = trk_surface(t, v);
    if (s > 1500.0 || fabsf(tgt - t->tgt) < 0.03f) return;
    double dx = t->x - v->cloud_origin_m[0], dz = t->z - v->cloud_origin_m[2];
    ps2_log("clouds near-change: field %llu: cloud R %.0f H %.0f alt %.0f (centre %.0f m away, surface %.0f m) opacity "
            "%.3f (now %.3f) -> %.3f %s", (unsigned long long)ps2_kernel_vblank_count(), (double)t->r, (double)t->h,
            (double)t->y, sqrt(dx * dx + dz * dz), s, (double)t->tgt, (double)t->cur, (double)tgt, why);
}

/* ACZ_VR_CLOUD_LIST_EASE=1 (bug #159, cl_storm 2026-10-28): the opacity follows C1 as a critically damped spring
   (continuous in value and rate; exact for a constant target over the step), instead of a smoothstep restarted from
   rest on every C1 change. The game's sprite alpha follows the coverage map with no easing at all, and in a windy stage
   C1 changes a cloud every few frames: M8 (Merlon, wind 133 m/s, clouds 3-6 km across) steps a cloud the eye is in by
   0.06-0.13 every 0.3 s, 0 -> 1 or 1 -> 0 in 2-3 s (the clouds drift over the game's fixed coverage map; checked on the
   game's own coverage samples). The restarted ramp (a full 2 s smoothstep from zero rate each time: ~6 % of the gap per
   0.3 s) lagged up to 0.8 behind for seconds and then closed the gap in one 2 s S-curve after the game's change had
   stopped (up to 0.56 in a second, all clouds of a gust together) -- a delayed "reload" of the cloud around the eye the
   PS2 never shows. The spring keeps the list ramp for clouds entering or leaving (from or to below 0.1: the whole cumulus
   still fades in over ACZ_VR_CLOUD_LIST_RAMP, bug #33) and follows changes within the list with a 95 % time of
   ACZ_VR_CLOUD_LIST_FOLLOW (1 s; a steady fade lags 0.42 s behind the game's). ACZ_VR_CLOUD_AROUND_RATE=r (opt-in,
   0 = off): a cloud whose body is within ACZ_VR_CLOUD_AROUND_M of the eye (or around it) changes at most r per
   second -- not the game's timing; it spreads the game's quick fades of the cloud you fly in over 1 / r seconds. */
static void track_follow(far_track *t, float dt, const ps2_cloud_view *v) {
    float w = 4.744f / (t->tgt <= 0.0f || t->cur < 0.1f ? F.ramp_s : F.follow_s);   /* 95 % of a step in that time */
    float x = t->cur - t->tgt, e = expf(-w * dt), k = (t->vel + w * x) * dt;
    float nx = (x + k) * e, nv = (t->vel - w * k) * e;
    if (F.near_rate > 0.0f && trk_surface(t, v) < (double)F.near_m) {   /* the opt-in cap near the eye */
        float lim = F.near_rate * dt, d = (t->tgt + nx) - t->cur;
        if (d > lim) { nx = t->cur + lim - t->tgt; nv = F.near_rate; }
        else if (d < -lim) { nx = t->cur - lim - t->tgt; nv = -F.near_rate; }
    }
    t->cur = t->tgt + nx; t->vel = nv;
    if (t->cur < 0.0f) { t->cur = 0.0f; if (t->vel < 0.0f) t->vel = 0.0f; }
    if (t->cur > 1.0f) { t->cur = 1.0f; if (t->vel > 0.0f) t->vel = 0.0f; }
    if (fabsf(t->cur - t->tgt) < 0.004f && fabsf(t->vel) < 0.02f) { t->cur = t->tgt; t->vel = 0.0f; }   /* settled */
}
/* Ease each cloud's splat opacity toward C1's value (track_target): S1 adds or drops a
   cloud and steps its opacity (1/64) as the clouds drift over the game's coverage map, which the splat followed at
   once (popping; the game's own sprites change gradually). On a new list_seq the published clouds are matched to the
   tracked ones by cloud-frame position and radius: new ones fade in, dropped ones fade out. The first list or a rebase
   are taken as they are (and, with ACZ_VR_CLOUD_LIST_KEEP=0, a list after an empty spell). Returns 1 while anything is
   still fading. */
static int track_update(const ps2_cloud_env *e, const ps2_cloud_view *v) {
    double dt = F.trk_ok ? v->time_s - F.trk_time : 0.0;
    F.trk_time = v->time_s;
    if (dt < 0.0) dt = 0.0;
    if (dt > 0.25) dt = 0.25;                       /* a hitch or a pause: at most a quarter second per frame */
    int reset = !F.trk_ok || F.splat_rebase != v->rebase;
    if (reset || F.splat_seq != e->list_seq) {
        uint32_t nc = e->ncloud < CL_MAX_CLOUDS ? e->ncloud : CL_MAX_CLOUDS;
        if (F.debug > 0 && (reset || nc != F.ntrk))
            ps2_log("clouds track: field %llu: list_seq %u, %u published, %u tracked%s", (unsigned long long)ps2_kernel_vblank_count(), e->list_seq, nc, F.ntrk,
                    !reset ? "" : F.trk_ok ? " -- RESET by a rebase (taken as they are)" : " -- RESET (taken as they are)");
        if (reset) F.ntrk = 0;
        for (uint32_t i = 0; i < F.ntrk; i++) F.trk[i].seen = 0;
        for (uint32_t k = 0; k < nc; k++) {
            double x = e->origin_m[0] + (double)e->cloud[k].pos_m[0], z = e->origin_m[2] + (double)e->cloud[k].pos_m[2];
            float r = e->cloud[k].radius_m, op = e->cloud[k].opacity;
            far_track *t = NULL;
            int fresh = 0;
            for (uint32_t i = 0; i < F.ntrk && !t; i++)
                if (!F.trk[i].seen && fabs(F.trk[i].x - x) < 2.0 && fabs(F.trk[i].z - z) < 2.0 && fabsf(F.trk[i].r - r) < 2.0f)
                    t = &F.trk[i];
            if (!t) {
                if (F.ntrk >= FAR_MAXCL) continue;
                t = &F.trk[F.ntrk++];
                fresh = 1;
                t->cur = reset ? op : 0.0f;          /* a cloud new to the list fades in */
                t->tgt = t->from = t->cur; t->age = -1.0f; t->dur = 0.0f;   /* (track_target below starts its ramp) */
                t->vel = 0.0f;
                if (F.debug > 0) {
                    double ex = x - v->cloud_origin_m[0], ez = z - v->cloud_origin_m[2];
                    ps2_log("clouds track: + cloud R %.0f m alt %.0f m opacity %.3f at %.0f m from the eye: %s", (double)r,
                            (double)e->cloud[k].pos_m[1], (double)op, sqrt(ex * ex + ez * ez), reset ? "at once" : "fading in");
                }
                /* the seed of its envelope puffs: from where it is (the same after a reset at the same place) */
                uint32_t hs = (uint32_t)(int64_t)floor(x) * 0x9E3779B1u ^ (uint32_t)(int64_t)floor(z) * 0x85EBCA77u;
                hs ^= hs >> 15; hs *= 0x2C1B3C6Du; hs ^= hs >> 12;
                t->seed = 1u + (hs & 0xFFFFFEu);                /* 1 .. 2^24 - 1: exact in a float */
            }
            t->x = x; t->z = z; t->y = e->cloud[k].pos_m[1]; t->r = r; t->var = e->cloud[k].variant;
#ifdef CL_CLOUD_SPRITE
            t->h = e->cloud[k].height_m;             /* the body's vertical semi-axis (S1) */
#else
            t->h = 0.0f;                             /* the shader's aspect spl.x */
#endif
            if (F.debug > 0 && !reset) trk_note(t, v, op, fresh ? "(new)" : "");
            if (F.ramp_s > 0.0f && !F.ease) track_target(t, op); else t->tgt = op;
            t->seen = 1;
        }
        for (uint32_t i = 0; i < F.ntrk; i++)                 /* dropped: fade out */
            if (!F.trk[i].seen) {
                if (F.debug > 0) trk_note(&F.trk[i], v, 0.0f, "(dropped)");
                if (F.ramp_s > 0.0f && !F.ease) track_target(&F.trk[i], 0.0f); else F.trk[i].tgt = 0.0f;
            }
        F.trk_ok = 1;
    }
    float step = F.fade_s > 0.0f ? (float)dt / F.fade_s : 1.0f;
    int fading = 0; uint32_t m = 0;
    for (uint32_t i = 0; i < F.ntrk; i++) {
        far_track *t = &F.trk[i];
        if (F.ramp_s > 0.0f && F.ease) {
            if (t->cur != t->tgt || t->vel != 0.0f) track_follow(t, (float)dt, v);
        } else if (F.ramp_s > 0.0f) {                          /* the smoothstep ramp of track_target */
            t->age += (float)dt;
            float a = t->dur > 0.0f ? t->age / t->dur : 1.0f;
            t->cur = a >= 1.0f ? t->tgt : t->from + (t->tgt - t->from) * a * a * (3.0f - 2.0f * a);
        } else {
            float d = t->tgt - t->cur;
            t->cur = fabsf(d) <= step ? t->tgt : t->cur + (d > 0.0f ? step : -step);
        }
        if (t->cur != t->tgt || t->vel != 0.0f) fading = 1;
        if (t->cur > 0.0f || t->tgt > 0.0f) F.trk[m++] = *t;        /* drop the faded-out */
    }
    F.ntrk = m;
    return fading;
}

/* The near gate's cloud set (ps2_clouds_near.c near_active) with ACZ_VR_CLOUD_LIST_KEEP: the clouds the weather map
   holds this frame -- the tracked list, including clouds still fading out after S1 dropped them -- instead of only C1's
   published list (an empty C1 list made the gate march the whole band; a cloud fading out near the eye had no near
   field). Returns -1 when not tracking (the caller keeps C1's list), else 1 if some body (0.9 x its semi-axes) comes
   within range + pad, 0 if none; *nearest = the conservative distance bound near_active uses (1e30 = none). */
int ps2_clouds_far_near_gate(const ps2_cloud_view *v, float range, float pad, float *nearest) {
    *nearest = 1e30f;
    if (!F.ready || !F.keep || !F.trk_ok) return -1;
    double alt = v->cloud_origin_m[1];
    float best = 1e30f;
    for (uint32_t i = 0; i < F.ntrk; i++) {
        const far_track *t = &F.trk[i];
        if (t->cur <= 0.0f) continue;
        double dx = t->x - v->cloud_origin_m[0], dz = t->z - v->cloud_origin_m[2], dy = (double)t->y - alt;
        double a = 0.9 * t->r * F.size_k, b = t->h > 0.0f ? 0.9 * t->h * F.size_k : a;
        if (a < 1.0) a = 1.0;
        if (b < 1.0) b = 1.0;
        double k = sqrt((dx * dx + dz * dz) / (a * a) + dy * dy / (b * b));
        float d = (float)((k - 1.0) * (a < b ? a : b));
        if (d < best) best = d;
    }
    *nearest = best;
    return best < range + pad;
}

/* bx_immerse (the cloud feel's in-cloud value, bug #34): the clouds the weather splat draws -- the tracked list, fading
   ones included, at their eased opacity -- for the CPU copy of the splat in ps2_video.c (cloud_body). Per cloud 8
   floats: cloud-frame x, z relative to org, altitude, R and H (x ACZ_VR_CLOUD_SIZE), opacity x (1 - transp) (the
   splat's info.y), the puff seed, 0; par[10]: od_eff, core fill ext.x, splat mix ext.z, billows fine.z, puffs fine.w,
   spl.x..w, keep. Renderer thread (as the splat). Returns the count, -1 off the list path or before tracking starts. */
int ps2_clouds_far_splat_list(const ps2_cloud_env *e, const double org[3], float *out, int max, float par[10]) {
    if (!F.ready || !list_path(e) || !F.trk_ok) return -1;
    float tr = 1.0f - (e->valid & CL_ENV_WEATHER ? e->transp : 0.0f);
    int n = 0;
    for (uint32_t i = 0; i < F.ntrk && n < max; i++) {
        const far_track *t = &F.trk[i];
        if (t->cur <= 0.0f) continue;
        float *o = out + 8 * n++;
        o[0] = (float)(t->x - org[0]); o[1] = (float)(t->z - org[2]); o[2] = t->y; o[3] = t->r * F.size_k;
        o[4] = t->h * F.size_k; o[5] = t->cur * tr; o[6] = (float)t->seed; o[7] = 0.0f;
    }
    par[0] = od_eff(e); par[1] = F.par.ext[0]; par[2] = F.par.ext[2]; par[3] = F.par.fine[2]; par[4] = F.par.fine[3];
    par[5] = F.par.spl[0]; par[6] = F.par.spl[1]; par[7] = F.par.spl[2]; par[8] = F.par.spl[3]; par[9] = (float)F.keep;
    return n;
}

int ps2_clouds_weather_init(const ps2_cloud_gpu *g) {
    if (common_init(g) != 0) { ps2_log("clouds: far module init failed (weather)"); return -1; }
    return 0;
}

/* Re-splat when the eye has moved a weather texel in the cloud frame, the list changed (list_seq, membership, valid
   bits), the band changed (the splat normalises the blobs' base/top by it), the procedural inputs changed, or the
   cloud frame was rebased. Returns 1: S2 owns the weather map. */
int ps2_clouds_weather_record(const ps2_cloud_gpu *g, VkCommandBuffer cmd, const ps2_cloud_rec *r) {
    if (!F.ready || !F.p_weather) return 0;
    const ps2_cloud_env *e = r->env;
    const ps2_cloud_view *v = r->view;
    int fi = r->frame_index;
    if (fi < 0 || fi >= (int)FAR_FIF) return 0;
    if (F.wq) {                         /* this slot's previous splat time (its fence has passed); logged per 600 calls */
        F.wq_calls++;
        uint64_t ts[2];
        if (F.wq_used[fi] && vkGetQueryPoolResults(F.dev, F.wq, 2u * (uint32_t)fi, 2, sizeof ts, ts, 8,
                                                   VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            F.wq_ms += (double)(ts[1] - ts[0]) * F.wq_ns * 1e-6; F.wq_n++;
        }
        F.wq_used[fi] = 0;
        if (F.wq_calls % 600u == 0u && F.wq_n) {
            ps2_log("clouds weather: splat %.3f ms GPU (%u splats in the last 600 frames = %.3f ms a frame; %s splat)",
                    F.wq_ms / F.wq_n, F.wq_n, F.wq_ms / 600.0, F.par.ext[2] > 0.5f ? "opacity-weighted" : "old");
            F.wq_ms = 0.0; F.wq_n = 0;
        }
    }
    int list = list_path(e);
    const double T = CL_WEATHER_M / (double)CL_WEATHER_RES;
    float key[4] = { e->base_m, e->top_m, list ? 0.0f : e->coverage, list ? od_eff(e) : e->type };
    int fading = list && track_update(e, v);
    /* a fade re-splats every 3rd frame and once when it ends (the pass costs ~0.12 ms with M1's ~100 clouds on the
       3090 Ti; a 1 s fade is 40 steps of 2.5 %) */
    int fade_due = fading ? (++F.fade_ctr % 3u) == 0u : F.fade_was;
    F.fade_was = fading;
    /* ACZ_VR_CLOUD_FADE_TEST=from,to (fields; test, cl_pop 2026-10-31): every listed cloud's opacity x a linear ramp 0 -> 1
       from field `from` to `to` (0 before), re-splat every 3rd frame inside the window as a list fade: a repeatable fade-in
       of everything in view for the weather map's precision (ACZ_VR_CLOUD_WX_SQRT A/B) */
    float ftk = 1.0f;
    {   static int ft_init; static float ft[2];
        if (!ft_init) {
            ft_init = 1;
            const char *fe = getenv("ACZ_VR_CLOUD_FADE_TEST");
            if (fe && sscanf(fe, "%f,%f", &ft[0], &ft[1]) == 2 && ft[1] > ft[0])
                ps2_log("clouds: ACZ_VR_CLOUD_FADE_TEST: the list's opacities ramp 0 -> 1 from field %.0f to %.0f", (double)ft[0], (double)ft[1]);
            else ft[0] = ft[1] = 0.0f;
        }
        if (ft[1] > ft[0]) {
            float f = (float)ps2_kernel_vblank_count();
            ftk = f <= ft[0] ? 0.0f : f >= ft[1] ? 1.0f : (f - ft[0]) / (ft[1] - ft[0]);
            static float ft_last = -1.0f;
            if (ftk != ft_last && (fade_due || ft_last < 0.0f || ftk >= 1.0f || (++F.fade_ctr % 3u) == 0u)) { fade_due = 1; ft_last = ftk; }
        }
    }
    int need = !F.have_splat || F.splat_rebase != v->rebase || memcmp(key, F.splat_key, sizeof key) != 0
            || (list && F.splat_seq != e->list_seq) || F.splat_valid != (e->valid & CL_ENV_LIST) || fade_due
            || fabs(v->cloud_origin_m[0] - F.splat_origin[0]) >= T || fabs(v->cloud_origin_m[2] - F.splat_origin[2]) >= T;
    if (F.debug > 0 && !list && F.trk_ok)
        ps2_log("clouds track: field %llu: no list path (valid %X, %u clouds): %u tracked clouds dropped at once, the weather map "
                "becomes the procedural field (coverage %.2f)", (unsigned long long)ps2_kernel_vblank_count(), e->valid, e->ncloud, F.ntrk, (double)e->coverage);
    if (!list) F.trk_ok = 0;                       /* a list that comes back later starts without fades */
    if (!need) return 1;
    /* a content change (not only the window following the eye): the far tiles refresh at the full FAR_SLICE rate */
    if (!F.have_splat || F.splat_rebase != v->rebase || memcmp(key, F.splat_key, sizeof key) != 0
        || (list && F.splat_seq != e->list_seq) || F.splat_valid != (e->valid & CL_ENV_LIST) || fade_due)
        F.wx_quiet = 0;
    uint8_t *slot = F.host + (size_t)HOST_SLOT * (uint32_t)fi;
    uint32_t *hdr = (uint32_t *)(slot + HOST_CLOUDS);
    far_cloud *cl = (far_cloud *)(slot + HOST_CLOUDS + 16);
    uint32_t n = 0;
    /* Only the published copies: S1 publishes each cloud once (the copy within +-10.9 km of origin_m, which stays
       within 2 km of the eye) and drops the copies the game's coverage map masks there. Further copies of the periodic
       list would each need their own coverage lookup; they are not needed, since the cloud-list path fades out by
       ACZ_VR_CLOUD_FAR_MAX_LIST (6 km). So no tiling by list_period_m (the shader's wrap stays off: period 0). */
    float period = 0.0f;
    if (list) {
        float tr = 1.0f - (e->valid & CL_ENV_WEATHER ? e->transp : 0.0f);
        if (F.imp_alpha && (e->valid & CL_ENV_WEATHER) && e->imp_alpha > 0.0f && e->imp_alpha < 1.0f)
            tr *= e->imp_alpha;                    /* (bug #194: the game's impostor alpha scale) */
        for (uint32_t i = 0; i < F.ntrk && n < FAR_MAXCL; i++) {
            const far_track *t = &F.trk[i];
            if (t->cur <= 0.0f) continue;
            /* cloud space of this frame: the tracked cloud-frame position minus the eye's; y stays the altitude */
            far_cloud *c = &cl[n++];
            c->pos_r[0] = (float)(t->x - v->cloud_origin_m[0]); c->pos_r[1] = t->y;
            c->pos_r[2] = (float)(t->z - v->cloud_origin_m[2]); c->pos_r[3] = t->r * F.size_k;
            c->info[0] = (float)t->var; c->info[1] = t->cur * tr * ftk; c->info[2] = t->h * F.size_k; c->info[3] = (float)t->seed;
        }
    }
    /* hdr.z: the list path (with ACZ_VR_CLOUD_LIST_KEEP an empty list splats clear sky, not the procedural field) */
    hdr[0] = n; memcpy(&hdr[1], &period, 4); hdr[2] = list && F.keep ? 1u : 0u; hdr[3] = 0;
    F.splat_origin[0] = v->cloud_origin_m[0]; F.splat_origin[1] = v->cloud_origin_m[1]; F.splat_origin[2] = v->cloud_origin_m[2];
    F.have_splat = 1; F.splat_seq = e->list_seq; F.splat_valid = e->valid & CL_ENV_LIST; F.splat_rebase = v->rebase;
    memcpy(F.splat_key, key, sizeof key);
    fill_params(fi, e, v);
    VkDescriptorSet s = F.set[fi];
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, F.p_weather);
    ps2_clouds_bind_common(cmd, F.layout);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, F.layout, 1, 1, &s, 0, NULL);
    far_push pc; memset(&pc, 0, sizeof pc);
    vkCmdPushConstants(cmd, F.layout, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    if (F.wq) {
        vkCmdResetQueryPool(cmd, F.wq, 2u * (uint32_t)fi, 2);
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, F.wq, 2u * (uint32_t)fi);
    }
    vkCmdDispatch(cmd, CL_WEATHER_RES / 8u, CL_WEATHER_RES / 8u, 1);
    far_tr_flags[fi] |= 1u;
    /* the density softened across the borders between the listed clouds (PASS_WSOFT reads a copy of the splat) */
    if (list && n > 0u && F.p_wsoft && g && g->weather && F.par.ext[1] >= 0.5f) {
        barrier2(cmd, CS, RW, XS, VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkBufferImageCopy rg; memset(&rg, 0, sizeof rg);
        rg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; rg.imageSubresource.layerCount = 1;
        rg.imageExtent.width = rg.imageExtent.height = CL_WEATHER_RES; rg.imageExtent.depth = 1;
        vkCmdCopyImageToBuffer(cmd, g->weather, VK_IMAGE_LAYOUT_GENERAL, F.wxc, 1, &rg);
        barrier2(cmd, XS, VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT, CS, RW);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, F.p_wsoft);
        vkCmdDispatch(cmd, CL_WEATHER_RES / 16u, CL_WEATHER_RES / 16u, 1);
        far_tr_flags[fi] |= 2u;
    }
    if (F.wq) { vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, F.wq, 2u * (uint32_t)fi + 1u); F.wq_used[fi] = 1; }
    return 1;
}

int ps2_clouds_far_init(const ps2_cloud_gpu *g) {
    if (common_init(g) != 0) return -1;
    if (F.n != g->dir_res) { ps2_log("clouds: far map size changed after init"); return -1; }
    F.p_tiles = make_pass(g, "cloud_far_tiles.comp.spv");
    F.p_args = make_pass(g, "cloud_far_args.comp.spv");
    F.p_march = make_pass(g, "cloud_far_march.comp.spv");
    if (!F.p_tiles || !F.p_args || !F.p_march) return -1;
    F.have_anchor = 0; F.cleared = 0; F.gen = 1; F.frames = 0;
    if (far_ab() > 0) {
        VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(g->phys, &pp);
        F.wq_ns = pp.limits.timestampPeriod;
        VkQueryPoolCreateInfo qc; memset(&qc, 0, sizeof qc);
        qc.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO; qc.queryType = VK_QUERY_TYPE_TIMESTAMP; qc.queryCount = 2u * FAR_FIF;
        if (vkCreateQueryPool(g->dev, &qc, NULL, &F.mq) != VK_SUCCESS) F.mq = VK_NULL_HANDLE;
        for (uint32_t i = 0; i < FAR_FIF; i++) F.mq_phase[i] = -1;
        ps2_log("clouds far: A/B of the governor's far-map states every %d frames (level 0 / level 1 relaxed / level 1 capped)", far_ab());
    }
    F.far_ready = 1;
    const ps2_cloud_cfg *c = ps2_clouds_cfg();
    ps2_log("clouds: far map %ux%u (%u tiles), %d steps, refresh every %d frames, error limit %.2f eye px%s",
            F.n, F.n, F.nt, c->far_steps, c->far_slice, (double)F.err_px, F.cap ? ", capped" : "");
    return 0;
}

int ps2_clouds_far_record(const ps2_cloud_gpu *g, VkCommandBuffer cmd, const ps2_cloud_rec *r) {
    (void)g;
    if (!F.far_ready) return 0;
    const ps2_cloud_env *e = r->env;
    const ps2_cloud_view *v = r->view;
    const ps2_cloud_cfg *cfg = ps2_clouds_cfg();
    int fi = r->frame_index;
    if (fi < 0 || fi >= (int)FAR_FIF) return 0;
    uint8_t *slot = F.host + (size_t)HOST_SLOT * (uint32_t)fi;
    if (F.mq && F.mq_phase[fi] >= 0) {  /* this slot's previous far update (its fence has passed): ACZ_VR_GOV_FAR_AB */
        uint64_t ts[2];
        int ph = F.mq_phase[fi], n = far_ab();
        if (F.mq_frame[fi] % (uint32_t)n >= (uint32_t)n / 4u
            && vkGetQueryPoolResults(F.dev, F.mq, 2u * (uint32_t)fi, 2, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            F.mq_ms[ph] += (double)(ts[1] - ts[0]) * F.wq_ns * 1e-6; F.mq_n[ph]++;
            if (F.debug) F.mq_tiles[ph] += ((const uint32_t *)(slot + HOST_COUNTS))[3];
        }
        F.mq_phase[fi] = -1;
        if (F.mq_n[0] >= 600u && F.mq_n[1] >= 600u && F.mq_n[2] >= 600u) {
            ps2_log("clouds far A/B: far update %.3f ms (level 0) / %.3f ms (level 1 relaxed) / %.3f ms (level 1 capped) GPU, "
                    "own timestamps over %u / %u / %u frames%s; tiles a frame %.0f / %.0f / %.0f", F.mq_ms[0] / F.mq_n[0],
                    F.mq_ms[1] / F.mq_n[1], F.mq_ms[2] / F.mq_n[2], F.mq_n[0], F.mq_n[1], F.mq_n[2],
                    F.debug ? "" : " (tiles need ACZ_DEBUG_CLOUDS)", (double)F.mq_tiles[0] / F.mq_n[0],
                    (double)F.mq_tiles[1] / F.mq_n[1], (double)F.mq_tiles[2] / F.mq_n[2]);
            memset(F.mq_ms, 0, sizeof F.mq_ms); memset(F.mq_n, 0, sizeof F.mq_n); memset(F.mq_tiles, 0, sizeof F.mq_tiles);
        }
    }
    if (F.debug && F.frames > 0 && F.frames % (uint32_t)F.debug == 0) {
        const uint32_t *cnt = (const uint32_t *)(slot + HOST_COUNTS);     /* this slot's frame, FAR_FIF frames ago */
        ps2_log("clouds far: tiles marched %u of %u (must %u, view %u, outside %u)", cnt[3], F.nt, cnt[0], cnt[1], cnt[2]);
    }
    if (F.stale > 0 && F.st_used[fi]) {     /* ACZ_DEBUG_CLOUD_FAR_STALE: this slot's frame (FAR_FIF frames ago) */
        const uint32_t *c = (const uint32_t *)(slot + HOST_STALE);
        for (int k = 0; k < 6; k++) F.st_sum[k] += c[k];
        F.st_frames++;
        uint32_t pop = c[0] + c[1];
        if (pop > F.st_worst) { F.st_worst = pop; F.st_worst_field = F.st_field[fi]; }
        if (pop >= 4u)
            ps2_log("clouds far stale: field %u: %u visible tiles left alone show a cloud appear / vanish against a march "
                    "now (%u of them relaxed by the near fog), %u + %u step > 16 levels, of %u checked (%u relaxed)",
                    F.st_field[fi], pop, c[0], c[2], c[3], c[4], c[5]);
        if (F.st_frames % (uint64_t)F.stale == 0u)
            ps2_log("clouds far stale: %llu frames: per frame %.2f tiles appear / vanish (%.2f relaxed by the near fog, %.2f "
                    "not), %.2f / %.2f step > 16 levels, %.1f checked (%.1f relaxed); worst %llu tiles at field %u",
                    (unsigned long long)F.st_frames, (double)(F.st_sum[0] + F.st_sum[1]) / (double)F.st_frames,
                    (double)F.st_sum[0] / (double)F.st_frames, (double)F.st_sum[1] / (double)F.st_frames,
                    (double)F.st_sum[2] / (double)F.st_frames, (double)F.st_sum[3] / (double)F.st_frames,
                    (double)F.st_sum[4] / (double)F.st_frames, (double)F.st_sum[5] / (double)F.st_frames,
                    (unsigned long long)F.st_worst, F.st_worst_field);
        F.st_used[fi] = 0;
    }
    /* the scheduler's anchor: tiles store eye positions relative to it; a new anchor = a new generation (all tiles
       re-marched, spread by the cap if one is set) */
    double dx = v->cloud_origin_m[0] - F.anchor[0], dy = v->cloud_origin_m[1] - F.anchor[1], dz = v->cloud_origin_m[2] - F.anchor[2];
    if (!F.have_anchor || F.rebase != v->rebase || dx * dx + dz * dz > 20000.0 * 20000.0) {
        if (F.debug > 0 && F.have_anchor) ps2_log("clouds far: new generation (re-anchor%s): every tile re-marched", F.rebase != v->rebase ? ", rebase" : "");
        memcpy(F.anchor, v->cloud_origin_m, sizeof F.anchor); F.have_anchor = 1; F.rebase = v->rebase; F.gen++;
        far_tr_flags[fi] |= 4u;
        dx = dy = dz = 0.0;
    }
    fill_params(fi, e, v);
    int list = list_path(e);
    far_push pc; memset(&pc, 0, sizeof pc);
    pc.cam[0] = (float)dx; pc.cam[1] = (float)dy; pc.cam[2] = (float)dz;
    pc.cam[3] = F.start_m >= 0.0f ? F.start_m : cfg->near_on ? cfg->near_range_m : 300.0f;
    pc.march[0] = list ? list_range(e) : F.max_m; pc.march[1] = F.dt_min; pc.march[2] = F.k_step;
    pc.march[3] = v->proj[0] > 0.0f ? v->proj[0] : 745.0f;           /* eye pixels per radian at the view centre */
    /* the periodic refresh is for content changes the translation error does not see (list, fades, band, colours):
       FAR_SLICE for a second after the weather changed, ACZ_VR_CLOUD_FAR_QUIET x slower while it stays the same */
    float slice = (float)cfg->far_slice * (F.wx_quiet < 120u ? 1.0f : F.quiet_k);
    if (F.wx_quiet < 0xFFFFFFu) F.wx_quiet++;
    /* the renderer's frame-time governor (ps2_clouds_far_gov), ACZ_VR_GOV_FAR_MODE: 1 = every tile's error limit
       x ACZ_VR_GOV_FAR_ERR and the periodic refresh half as often, ACZ_VR_GOV_FAR_CAP tiles a frame only of those outside
       both eyes' views (PASS_ARGS, ext.w); 0 = at most ACZ_VR_GOV_FAR_CAP tiles a frame of all lists (the must list
       first, then the visible, then the rest; what is left stays due) and the refresh half as often */
    int gcap = far_gov_cap();
    uint32_t cap = F.cap;
    int gon, grelax;
    far_gov_state(F.frames, &gon, &grelax);
    far_tr_flags[fi] = (uint8_t)((far_tr_flags[fi] & 0x0Fu) | (uint32_t)(gon ? (grelax ? 2 : 3) : 1) << 4);
    if (gon && gcap > 0 && (!cap || cap > (uint32_t)gcap)) cap = (uint32_t)gcap;
    if (gon) slice *= 2.0f;
    pc.sched[0] = gon && grelax ? F.err_px * far_gov_err() : F.err_px; pc.sched[1] = slice; pc.sched[2] = (float)cap;
    pc.sched[3] = cfg->near_on && F.start_m < 0.0f ? 1.0f : 0.0f;    /* the near field covers 0 .. start */
    uint32_t ne = F.ubo_asked && F.n_ubo <= F.n && F.n_ubo >= 64u ? F.n_ubo : F.n;   /* = this pass's sizes.x */
    if (ne != F.n_used) {                                          /* a new used size: re-march every tile */
        if (F.debug > 0 && F.n_used) ps2_log("clouds far: new generation (used size %u -> %u): every tile re-marched", F.n_used, ne);
        F.n_used = ne; F.gen++; far_tr_flags[fi] |= 4u;
    }
    uint32_t ncap = overcast(e) && F.ovc_steps > 0 ? (uint32_t)F.ovc_steps : 0u;
    uint32_t tpr = ne / 8u, nt = tpr * tpr;
    pc.fr[0] = F.frames; pc.fr[1] = ne; pc.fr[2] = tpr; pc.fr[3] = (ncap & 0xFFu) << 1 | (F.gen & 0x7FFFFFu) << 9;
    VkDescriptorSet s = F.set[fi];
    /* the previous frame's march (and the debug copy) read the counters; this frame's fill writes them */
    barrier2(cmd, CS | XS, RW | VK_ACCESS_2_TRANSFER_READ_BIT, XS, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    if (!F.cleared) { vkCmdFillBuffer(cmd, F.tiles, 0, VK_WHOLE_SIZE, 0); F.cleared = 1; }   /* generation 0 = never */
    vkCmdFillBuffer(cmd, F.list, 0, 32, 0);
    if (F.stale > 0) vkCmdFillBuffer(cmd, F.list, 32u + (VkDeviceSize)3u * nt * 4u, 32, 0);   /* (the stale counters) */
    barrier2(cmd, XS, VK_ACCESS_2_TRANSFER_WRITE_BIT, CS, RW);
    ps2_clouds_bind_common(cmd, F.layout);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, F.layout, 1, 1, &s, 0, NULL);
    vkCmdPushConstants(cmd, F.layout, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    if (F.mq) {
        vkCmdResetQueryPool(cmd, F.mq, 2u * (uint32_t)fi, 2);
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, F.mq, 2u * (uint32_t)fi);
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, F.p_tiles);
    vkCmdDispatch(cmd, (nt + 63u) / 64u, 1, 1);
    barrier2(cmd, CS, RW, CS, RW);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, F.p_args);
    vkCmdDispatch(cmd, 1, 1, 1);
    barrier2(cmd, CS, VK_ACCESS_2_SHADER_WRITE_BIT, CS | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
             VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, F.p_march);
    vkCmdDispatchIndirect(cmd, F.list, 16);
    far_tr_flags[fi] |= 8u;
    if (F.mq) {
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, F.mq, 2u * (uint32_t)fi + 1u);
        F.mq_phase[fi] = (int8_t)far_ab_phase(F.frames); F.mq_frame[fi] = F.frames;
    }
    if (F.debug || far_trace_on()) {          /* the counters into this slot (read FAR_FIF frames later) */
        barrier2(cmd, CS, VK_ACCESS_2_SHADER_WRITE_BIT, XS, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferCopy bc; bc.srcOffset = 0; bc.dstOffset = (VkDeviceSize)HOST_SLOT * (uint32_t)fi + HOST_COUNTS; bc.size = 16;
        vkCmdCopyBuffer(cmd, F.list, F.host_buf, 1, &bc);
    }
    if (F.stale > 0) {                        /* ACZ_DEBUG_CLOUD_FAR_STALE: the counters into this slot */
        barrier2(cmd, CS, VK_ACCESS_2_SHADER_WRITE_BIT, XS, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferCopy bc; bc.srcOffset = 32u + (VkDeviceSize)3u * nt * 4u;
        bc.dstOffset = (VkDeviceSize)HOST_SLOT * (uint32_t)fi + HOST_STALE; bc.size = 32;
        vkCmdCopyBuffer(cmd, F.list, F.host_buf, 1, &bc);
        F.st_used[fi] = 1; F.st_nt[fi] = nt; F.st_field[fi] = (uint32_t)ps2_kernel_vblank_count();
    }
    F.frames++;
    return 1;
}

/* PS2_GPU_TRACE: frame slot fi's far record (its fence has passed): out[0..3] = the tile lists must / visible /
   outside and the tiles marched, out[4] = the flags (far_tr_flags); the flags are cleared for the slot's next frame */
int ps2_clouds_far_trace(int fi, uint32_t out[5]) {
    memset(out, 0, 5 * sizeof out[0]);
    if (!F.ready || fi < 0 || fi >= (int)FAR_FIF) return 0;
    out[4] = far_tr_flags[fi];
    if (far_tr_flags[fi] & 8u) memcpy(out, F.host + (size_t)HOST_SLOT * (uint32_t)fi + HOST_COUNTS, 4 * sizeof out[0]);
    far_tr_flags[fi] = 0;
    return 1;
}
/* the renderer's frame-time governor (ps2_video.c hgov, level 1): fewer far tiles re-marched a frame while on, and the
   cloud shadow map rebuilt in half slices (twice as long; invisible) */
void ps2_clouds_far_gov(int on) { far_gov_on = on != 0; ps2_clouds_shadow_gov(on); }
void ps2_clouds_far_shutdown(const ps2_cloud_gpu *g) {
    VkDevice dev = g->dev;
    if (!F.ready) return;
    VkPipeline ps[5] = { F.p_weather, F.p_wsoft, F.p_tiles, F.p_args, F.p_march };
    for (int i = 0; i < 5; i++) if (ps[i]) vkDestroyPipeline(dev, ps[i], NULL);
    for (int i = 0; i < F.nlayouts; i++) if (F.layouts[i]) vkDestroyPipelineLayout(dev, F.layouts[i], NULL);
    if (F.wq) vkDestroyQueryPool(dev, F.wq, NULL);
    if (F.mq) vkDestroyQueryPool(dev, F.mq, NULL);
    if (F.pool) vkDestroyDescriptorPool(dev, F.pool, NULL);
    if (F.set_layout) vkDestroyDescriptorSetLayout(dev, F.set_layout, NULL);
    if (F.host_buf) { vkDestroyBuffer(dev, F.host_buf, NULL); vkFreeMemory(dev, F.host_mem, NULL); }
    if (F.tiles) { vkDestroyBuffer(dev, F.tiles, NULL); vkFreeMemory(dev, F.tiles_mem, NULL); }
    if (F.list) { vkDestroyBuffer(dev, F.list, NULL); vkFreeMemory(dev, F.list_mem, NULL); }
    if (F.wxc) { vkDestroyBuffer(dev, F.wxc, NULL); vkFreeMemory(dev, F.wxc_mem, NULL); }
    memset(&F, 0, sizeof F);
}
