// ps2_clouds.c - our own 3D clouds (CLOUDS_3D_RESEARCH_2026-10-20.md section 8): switches, the C1 double buffer, the
// C3 images and the pipeline at the two split points of ps2_video.c render_list (S0 skeleton, S4 pipeline). The
// contracts are documented in ps2_clouds.h; the far and near renderers are ps2_clouds_far.c / ps2_clouds_near.c.
//
// Per split point (rendering closed by the caller):
//   1. the scene depth goes to SHADER_READ_ONLY_OPTIMAL (sampled by the hooks and the composite);
//   2. a fresh descriptor set (common set 0) and a CloudFrame uniform copy for this pass (C1 + C2 + settings);
//   3. the module hook (S2 far once per command buffer, S3 near per eye pass) or the S4 test pattern;
//   4. the composite (cloud_comp.frag) in its own rendering over the colour target only (no depth attachment, so the
//      depth can be sampled), viewMask 3 in multiview; blend dst.rgb = src.rgb + dst.rgb * src.a, alpha kept;
//   5. the depth goes back to DEPTH_ATTACHMENT_OPTIMAL; the caller reopens the frame's rendering.
// GPU timestamps around each pass (ACZ_DEBUG_CLOUDS=N logs them with C1 and C2 every N frames).

#include "ps2_runtime.h"
#include "ps2_clouds.h"
#include "ps2_vkcen.h"          /* (PS2_VK_CENSUS: vkCmd* calls counted) */
/* PS2_DEBUG_PIPE_STATS (fq_opt, diagnostic; ps2_video.c): the pipelines through its statistics wrapper, modules named */
VkResult ps2_vk_create_gp(VkDevice, VkPipelineCache, uint32_t, const VkGraphicsPipelineCreateInfo *, const VkAllocationCallbacks *, VkPipeline *);
VkResult ps2_vk_create_cp(VkDevice, VkPipelineCache, uint32_t, const VkComputePipelineCreateInfo *, const VkAllocationCallbacks *, VkPipeline *);
void ps2_vk_name_module(VkShaderModule m, const char *path);
#define vkCreateGraphicsPipelines(...) ps2_vk_create_gp(__VA_ARGS__)
#define vkCreateComputePipelines(...) ps2_vk_create_cp(__VA_ARGS__)

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define CL_FIF 4u                 /* >= ps2_video.c FRAMES_IN_FLIGHT (3) */
#define CL_PASSES 12u             /* split-point passes per command buffer (2 points x 2 eye passes, with room; the
                                     PS2_PERF_AB re-draws of a dumped frame add 2 x 3 in the same command buffer) */
#define CL_UBO_SLOT 1280u        /* (1024 until the flash lights grew CloudFrame to 1120 bytes) */

/* ---------------------------------------------------------------------------------------------------------------
   switches
   --------------------------------------------------------------------------------------------------------------- */
static ps2_cloud_cfg cfg;
static int cfg_read;
static struct { VkImage img; VkImageView v; } dkeep[4];   /* (kept views of other depth images, depth_view_for) */
static uint32_t dkeep_next;
static int keep_views;                       /* a composite-only pass has been seen (ACZ_REC_CINE): keep the views */
static float units_per_m = 6.0f;
#define CL_VERIFY_SNAPS 4u
static struct {           /* ACZ_VR_CLOUD_VERIFY (see verify_copy) */
    int every;
    VkBuffer buf; VkDeviceMemory mem; uint8_t *ptr; VkDeviceSize cap, snap;
    uint32_t w, h, layers, nsnap, kind[2], frame, have;   /* have: bit k = both copies of pass kind k */
    int pending;
} V;
static struct {           /* ACZ_VR_CLOUD_DUMP (see dump_copy) */
    int state;                     /* 0 waiting, 1 copies recorded, 2 done */
    int fi; uint32_t field;
    VkBuffer buf; VkDeviceMemory mem; uint8_t *ptr;
    VkDeviceSize off_dir, off_dirz, off_near, off_nearz, off_wx, size;
    uint32_t n, nw, nh, have;      /* have: 1 dir, 2 near */
    ps2_cloud_env env; ps2_cloud_view view;
} D;
static int env_int(const char *n, int def, int lo, int hi) {
    const char *e = getenv(n);
    int v = e && *e ? atoi(e) : def;
    return v < lo ? lo : v > hi ? hi : v;
}
/* ACZ_VR_CLOUD_WX_SQRT (cl_pop 2026-10-31, bug #260; default 1): the weather map's 8-bit density channel holds
   sqrt(density) (cloud_common.glsl cl_weather / cl_wx_enc; the shaders see it as CloudFrame wx.w >= 16). The splat's
   density of a big stratus body is small (M10: 17-33 of 255 levels at opacity 1, M1's overcast 9-18), so linear 8 bits
   stepped a cloud the list fades in (to 0.14) through 0 -> 25 -> 43 -> 57 % visible opacity over its 3.6 km chord in
   three jumps (sqrt: at most 5.4 % a re-splat). 0 = linear, as before. */
static int wx_sqrt = 1;
const ps2_cloud_cfg *ps2_clouds_cfg(void) {
    if (cfg_read) return &cfg;
    const char *e = getenv("ACZ_VR_CLOUDS");
    if (e && (!strcmp(e, "near"))) cfg.near_on = 1;
    else if (e && (!strcmp(e, "far"))) cfg.far_on = 1;
    else if (e && (!strcmp(e, "all") || !strcmp(e, "1"))) cfg.near_on = cfg.far_on = 1;
    e = getenv("ACZ_VR_CLOUD_GAME");
    cfg.drop_game = e && *e ? *e == '0' : cfg.far_on;        /* 0 = drop the game's clouds; default: with far */
    cfg.far_res = env_int("ACZ_VR_CLOUD_FAR_RES", 2048, 256, 4096) & ~7;
    cfg.far_steps = env_int("ACZ_VR_CLOUD_FAR_STEPS", 64, 4, 512);
    cfg.far_slice = env_int("ACZ_VR_CLOUD_FAR_SLICE", 3, 1, 64);
    cfg.near_steps = env_int("ACZ_VR_CLOUD_NEAR_STEPS", 32, 2, 512);
    e = getenv("ACZ_VR_CLOUD_NEAR_RANGE");
    cfg.near_range_m = e && atof(e) > 10.0 ? (float)atof(e) : cfg.far_on ? 600.0f : 1000.0f;   /* the game's sprites fade in
                                                                                          at 350-1000 m */
    cfg.dump_field = env_int("ACZ_VR_CLOUD_DUMP", 0, 0, 0x7FFFFFFF);
    cfg.debug = env_int("ACZ_DEBUG_CLOUDS", 0, 0, 1000000);
    cfg.test = env_int("ACZ_VR_CLOUD_TEST", 0, 0, 3);
    wx_sqrt = env_int("ACZ_VR_CLOUD_WX_SQRT", 1, 0, 1);
    V.every = env_int("ACZ_VR_CLOUD_VERIFY", 0, 0, 1000000);
    cfg.crop = env_int("ACZ_VR_CLOUD_CROP", 1, 0, 1);
    e = getenv("ACZ_VR_UNITS_PER_M");                          /* the same value zero_vr.c uses */
    if (e && atof(e) > 0.5) units_per_m = (float)atof(e);
    cfg_read = 1;
    if (cfg.near_on || cfg.far_on)
        ps2_log("clouds: ACZ_VR_CLOUDS near %d far %d, game clouds %s, far map %d^2 (%d steps, slice %d), near %d steps "
                "over %.0f m%s; weather density %s (ACZ_VR_CLOUD_WX_SQRT)", cfg.near_on, cfg.far_on,
                cfg.drop_game ? "dropped" : "kept", cfg.far_res, cfg.far_steps, cfg.far_slice, cfg.near_steps,
                cfg.near_range_m, cfg.test ? ", TEST PATTERN" : "", wx_sqrt ? "sqrt-coded 8 bits" : "linear 8 bits");
    return &cfg;
}
static int far_avail, near_avail;          /* the module was initialised (ACZ_VR_CLOUDS at start) */
int ps2_clouds_toggle(int which, int on) {
    ps2_clouds_cfg();
    if (which == 0) { if (!far_avail) return -1; if (on >= 0) cfg.far_on = on; return cfg.far_on; }
    if (which == 1) { if (!near_avail) return -1; if (on >= 0) cfg.near_on = on; return cfg.near_on; }
    if (which == 2) { if (!far_avail) return -1; if (on >= 0) cfg.drop_game = !on; return !cfg.drop_game; }
    if (which == 3) return far_avail || near_avail ? ps2_clouds_shadow_toggle(on) : -1;
    if (which == 4) return far_avail ? ps2_clouds_optics_toggle(on) : -1;
    return -1;
}
int ps2_clouds_enabled(void) { const ps2_cloud_cfg *c = ps2_clouds_cfg(); return c->near_on || c->far_on; }
float ps2_clouds_units_per_m(void) { ps2_clouds_cfg(); return units_per_m; }

/* ---------------------------------------------------------------------------------------------------------------
   C1 double buffer
   --------------------------------------------------------------------------------------------------------------- */
static pthread_mutex_t env_mx = PTHREAD_MUTEX_INITIALIZER;
static ps2_cloud_env env_stage, env_pub[2];
static int env_front;
static uint32_t env_count;
ps2_cloud_env *ps2_cloud_env_edit(void) { pthread_mutex_lock(&env_mx); return &env_stage; }
void ps2_cloud_env_done(void) { pthread_mutex_unlock(&env_mx); }
void ps2_cloud_env_publish(uint32_t seq) {
    pthread_mutex_lock(&env_mx);
    env_stage.seq = seq ? seq : ++env_count;
    if (!env_stage.seq) env_stage.seq = 1;
    env_pub[env_front ^ 1] = env_stage;
    env_front ^= 1;
    pthread_mutex_unlock(&env_mx);
}
int ps2_cloud_env_read(ps2_cloud_env *out) {
    pthread_mutex_lock(&env_mx);
    *out = env_pub[env_front];
    pthread_mutex_unlock(&env_mx);
    return out->seq != 0;
}
static void v3set(float *d, float x, float y, float z) { d[0] = x; d[1] = y; d[2] = z; }
/* built-in values for the groups nobody published (a broken cumulus layer, the benchmark's colours) */
static void env_defaults(ps2_cloud_env *e) {
    if (!(e->valid & CL_ENV_PROFILE)) { e->base_m = 1200.0f; e->top_m = 2400.0f; }
    if (!(e->valid & CL_ENV_WEATHER)) { e->coverage = 0.45f; e->type = 0.7f; e->transp = 0.0f; e->precip = 0.0f; e->coverage_unmasked = 0.0f; }
    if (!(e->valid & CL_ENV_COLOURS)) {
        v3set(e->lit, 0.80f, 0.82f, 0.83f); v3set(e->shadow, 0.15f, 0.20f, 0.28f); v3set(e->fog, 0.45f, 0.49f, 0.57f);
        v3set(e->sky_top, 0.14f, 0.20f, 0.29f); v3set(e->sky_hor, 0.45f, 0.49f, 0.57f);
        e->fog_near_m = 2000.0f; e->fog_far_m = 30000.0f;
    }
    if (!(e->valid & CL_ENV_SHADOW_A)) e->shadow_a = 0.85f;
    if (!(e->valid & CL_ENV_SKY_BELOW)) v3set(e->sky_below, 0.40f, 0.43f, 0.47f);
    if (!(e->valid & CL_ENV_INCLOUD_COL)) v3set(e->incloud, 0.80f, 0.82f, 0.83f);
    if (!(e->valid & CL_ENV_SUN)) { float l = sqrtf(0.09f + 0.64f + 0.25f); v3set(e->sun_dir, 0.3f / l, 0.8f / l, 0.5f / l); }
    if (!(e->valid & CL_ENV_INCLOUD)) e->in_cloud = 0.0f;
    if (!(e->valid & CL_ENV_LIST)) e->ncloud = 0;
    if (!(e->valid & CL_ENV_WIND)) e->wind_mps[0] = e->wind_mps[1] = 0.0f;
    if (!(e->valid & CL_ENV_COVMAP)) e->cov_map = NULL;
    if (!(e->valid & CL_ENV_DRIFT)) e->drift_m[0] = e->drift_m[1] = 0.0f;
    if (!(e->valid & CL_ENV_SHAPE)) memset(e->shape, 0, sizeof e->shape);
    if (!(e->valid & CL_ENV_NEAR_ART)) memset(e->near_art, 0, sizeof e->near_art);
    if (!(e->valid & CL_ENV_SHADE)) memset(e->shade, 0, sizeof e->shade);
    if (!(e->valid & CL_ENV_ART_K)) memset(e->art_k, 0, sizeof e->art_k);
}

/* ---------------------------------------------------------------------------------------------------------------
   Vulkan state
   --------------------------------------------------------------------------------------------------------------- */
typedef struct { VkImage img; VkDeviceMemory mem; VkImageView view, sview; uint32_t mips; } cl_image;   /* sview: level 0 */
typedef struct {           /* std140 mirror of cloud_common.glsl CloudFrame (vec4 only) */
    float cam_x[4], cam_y[4], cam_z[4];
    float eye_m[2][4];
    float proj[4], img[4], depth[4], misc[4], origin[4];
    float band[4], env[4], sun[4], lit[4], shadow[4], fog[4], sky_top[4], sky_hor[4], fogr[4];
    float cfg[4], sizes[4], sizes2[4], wx[4];
    float sky_below[4], incloud[4], wind[4], origin_split[4], origin_4k[4], vis[2][4];
    float user[4][4];
    float shd[4][4];      /* the cloud shadow map's parameters (ps2_clouds_shadow_ubo) */
    float opt[23][4];     /* optical phenomena (ps2_clouds_optics_ubo; cloud_optics.glsl) */
    float flash[9][4];    /* flash lights (ps2_clouds_flash_set; cloud_common.glsl cl_flash_light) */
    float shaft[2][4];    /* light shafts (ACZ_VR_CLOUD_SHAFTS): the map's distances; valid, brighten */
} cl_frame_ubo;
_Static_assert(sizeof(cl_frame_ubo) <= CL_UBO_SLOT, "CloudFrame too big for its slot");

static struct {
    int ok;                               /* resources created */
    int ready;                            /* images initialised (noise generated, cleared) */
    ps2_cloud_gpu gpu;
    char shader_dir[1024];
    VkPhysicalDeviceMemoryProperties mp;
    double ts_ns;                         /* ns per timestamp tick */
    uint32_t ubo_slot;
    VkSampler s_rep, s_clamp, s_near, s_mip;
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout pipe_layout;
    VkDescriptorPool pool;
    VkDescriptorSet sets[CL_FIF][CL_PASSES];
    VkBuffer ubo; VkDeviceMemory ubo_mem; uint8_t *ubo_ptr;
    cl_image dir, dir_depth, weather, noise_base, noise_detail, near_c, near_d;
    cl_image dir_prof;                    /* ACZ_VR_CLOUD_FAR_SPLIT: the far map's opacity profile (cloud_dir_prof) */
    cl_image shaft;                       /* ACZ_VR_CLOUD_SHAFTS: the light shaft map (cloud_shaft) */
    VkPipeline p_shafts;
    uint32_t near_w, near_h;
    cl_image near_f, near_p;              /* ACZ_VR_CLOUD_NEAR_REUSE: the game frame's march beyond 60 m, its reprojection depth */
    ps2_cloud_env env_game;               /* (ACZ_VR_CLOUD_NEAR_REUSE) the C1 copy of the last game frame */
    int have_env_game;
    uint8_t inb[CL_FIF];                  /* the frame of this slot is an in-between frame (ACZ_VR_CLOUD_NEAR_REUSE_AB) */
    uint8_t near_drew[CL_FIF];            /* the near point of this slot's frame drew (a composite-only pass may use it) */
    VkImage depth_src; VkImageView depth_view;   /* our 2D-array view of the frame's depth */
    VkImageView grave[8]; uint32_t grave_age[8];
    VkPipeline p_noise, p_weather, p_test, p_comp;
    VkPipeline p_comp_pm;      /* the composite over the eyes' visible mesh (PS2_PASS_MASK; ps2_video.c pm_*) */
    VkPipeline p_comp_x, p_comp_pm_x;   /* ACZ_VR_CLOUD_COMP_UNROLL: the other way (in-run / same-frame A/B only) */
    int comp_unroll;                    /* ACZ_VR_CLOUD_COMP_UNROLL of p_comp / p_comp_pm */
    VkPipeline p_cdv[2][3];             /* (perf6_clouds) the composite by [masked][near divisor - 2]; p_comp / p_comp_pm
                                           are [0 / 1][0] */
    VkQueryPool qp[CL_FIF];
    int wx_module;                        /* S2's weather pass owns the weather map */
    ps2_cloud_view view;                  /* this pass's C2 with the cloud-frame fields */
    double last_cloud_origin[2]; uint32_t rebase; int have_last;
    uint64_t cmd_id[CL_FIF];
    VkCommandBuffer cmd_handle[CL_FIF];   /* the command buffer of the last cmd_begin (re-draws: the same one) */
    uint32_t npass[CL_FIF], qmask[CL_FIF], qkind[CL_FIF][CL_PASSES];
    uint8_t qsplit[CL_FIF][CL_PASSES];    /* a pass's composite in a PS2_PASS_AB "off" block of cloudsplit / cloudshafts /
                                             compearly (the composites' own time per state, ACZ_DEBUG_CLOUDS) */
    double split_ms[2][2]; uint32_t split_n[2][2];   /* [far / near point][on / off block]: the composite (+ stamp) */
    uint64_t far_cmd;                     /* command buffer of the last far update */
    float weather_key[2];
    int test_far_done;
    VkDescriptorSet cur_set;
    uint32_t frame;                       /* renderer frames seen (command buffers) */
    ps2_cloud_env env;                    /* this command buffer's C1 copy */
    double gpu_ms[3][2]; uint32_t gpu_n[3];   /* [far point / near point / early far update][hooks, composite] */
    uint64_t passes[2], composites[2];
} G;

static uint32_t find_mem(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < G.mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (G.mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return 0xFFFFFFFFu;
}
/* PS2_PERF_CLOUD_ASYNC=2 (perf4_passes): the far update runs on a queue of a compute-only family; our resources are then
   shared by both families (CONCURRENT) instead of owned by the graphics family (set before ps2_clouds_vk_init) */
uint32_t ps2_clouds_qfam[2]; uint32_t ps2_clouds_qfam_n;
int ps2_clouds_make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, int host_visible,
                           VkBuffer *buf, VkDeviceMemory *mem, void **mapped) {
    VkDevice dev = G.gpu.dev;
    VkBufferCreateInfo bi; memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; bi.size = size; bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (ps2_clouds_qfam_n == 2u) { bi.sharingMode = VK_SHARING_MODE_CONCURRENT; bi.queueFamilyIndexCount = 2; bi.pQueueFamilyIndices = ps2_clouds_qfam; }
    *buf = VK_NULL_HANDLE; *mem = VK_NULL_HANDLE; if (mapped) *mapped = NULL;
    if (vkCreateBuffer(dev, &bi, NULL, buf) != VK_SUCCESS) return -1;
    VkMemoryRequirements rq; vkGetBufferMemoryRequirements(dev, *buf, &rq);
    VkMemoryAllocateInfo ai; memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; ai.allocationSize = rq.size;
    /* host_visible 2 = a readback buffer: host-cached memory (reads from write-combined memory crawl) */
    ai.memoryTypeIndex = 0xFFFFFFFFu;
    if (host_visible == 2)
        ai.memoryTypeIndex = find_mem(rq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                                         | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (ai.memoryTypeIndex == 0xFFFFFFFFu)
        ai.memoryTypeIndex = find_mem(rq.memoryTypeBits, host_visible
            ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == 0xFFFFFFFFu || vkAllocateMemory(dev, &ai, NULL, mem) != VK_SUCCESS
        || vkBindBufferMemory(dev, *buf, *mem, 0) != VK_SUCCESS) {
        vkDestroyBuffer(dev, *buf, NULL); *buf = VK_NULL_HANDLE;
        if (*mem) { vkFreeMemory(dev, *mem, NULL); *mem = VK_NULL_HANDLE; }
        return -1;
    }
    if (mapped && host_visible && vkMapMemory(dev, *mem, 0, VK_WHOLE_SIZE, 0, mapped) != VK_SUCCESS) return -1;
    return 0;
}
static int make_img_m(cl_image *o, VkImageType type, uint32_t w, uint32_t h, uint32_t d, uint32_t layers, VkFormat fmt,
                      VkImageUsageFlags usage, uint32_t mips) {
    VkDevice dev = G.gpu.dev;
    memset(o, 0, sizeof *o);
    VkImageCreateInfo ii; memset(&ii, 0, sizeof ii);
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; ii.imageType = type; ii.format = fmt;
    ii.extent.width = w; ii.extent.height = h; ii.extent.depth = d; ii.mipLevels = mips ? mips : 1u; ii.arrayLayers = layers;
    ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL; ii.usage = usage;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (ps2_clouds_qfam_n == 2u) { ii.sharingMode = VK_SHARING_MODE_CONCURRENT; ii.queueFamilyIndexCount = 2; ii.pQueueFamilyIndices = ps2_clouds_qfam; }
    if (vkCreateImage(dev, &ii, NULL, &o->img) != VK_SUCCESS) { o->img = VK_NULL_HANDLE; return -1; }
    VkMemoryRequirements rq; vkGetImageMemoryRequirements(dev, o->img, &rq);
    VkMemoryAllocateInfo ai; memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; ai.allocationSize = rq.size;
    ai.memoryTypeIndex = find_mem(rq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == 0xFFFFFFFFu || vkAllocateMemory(dev, &ai, NULL, &o->mem) != VK_SUCCESS
        || vkBindImageMemory(dev, o->img, o->mem, 0) != VK_SUCCESS) return -1;
    VkImageViewCreateInfo vi; memset(&vi, 0, sizeof vi);
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; vi.image = o->img; vi.format = fmt;
    vi.viewType = type == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D : layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                                                                               : VK_IMAGE_VIEW_TYPE_2D;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = ii.mipLevels; vi.subresourceRange.layerCount = layers;
    if (vkCreateImageView(dev, &vi, NULL, &o->view) != VK_SUCCESS) { o->view = VK_NULL_HANDLE; return -1; }
    o->mips = ii.mipLevels;
    o->sview = o->view;
    if (ii.mipLevels > 1u) {              /* storage access: level 0 only */
        vi.subresourceRange.levelCount = 1;
        if (vkCreateImageView(dev, &vi, NULL, &o->sview) != VK_SUCCESS) { o->sview = VK_NULL_HANDLE; return -1; }
    }
    return 0;
}
static int make_img(cl_image *o, VkImageType type, uint32_t w, uint32_t h, uint32_t d, uint32_t layers, VkFormat fmt,
                    VkImageUsageFlags usage) {
    return make_img_m(o, type, w, h, d, layers, fmt, usage, 1u);
}
static void free_img(cl_image *o) {
    VkDevice dev = G.gpu.dev;
    if (o->sview && o->sview != o->view) vkDestroyImageView(dev, o->sview, NULL);
    if (o->view) vkDestroyImageView(dev, o->view, NULL);
    if (o->img) vkDestroyImage(dev, o->img, NULL);
    if (o->mem) vkFreeMemory(dev, o->mem, NULL);
    memset(o, 0, sizeof *o);
}
static VkShaderModule load_module(const char *name) {
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", G.shader_dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) { ps2_log("clouds: cannot open shader %s", path); return VK_NULL_HANDLE; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    void *buf = n > 0 ? malloc((size_t)n) : NULL;
    VkShaderModule m = VK_NULL_HANDLE;
    if (buf && fread(buf, 1, (size_t)n, f) == (size_t)n) {
        VkShaderModuleCreateInfo ci; memset(&ci, 0, sizeof ci);
        ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO; ci.codeSize = (size_t)n; ci.pCode = (const uint32_t *)buf;
        if (vkCreateShaderModule(G.gpu.dev, &ci, NULL, &m) != VK_SUCCESS) m = VK_NULL_HANDLE;
    }
    fclose(f); free(buf);
    ps2_vk_name_module(m, name);
    return m;
}
VkPipeline ps2_clouds_make_compute(const char *spv_name, VkDescriptorSetLayout extra_set, VkPipelineLayout *layout_out) {
    VkPipelineLayout lay = G.pipe_layout;
    if (extra_set) {
        VkDescriptorSetLayout sl[2] = { G.set_layout, extra_set };
        VkPushConstantRange pr; memset(&pr, 0, sizeof pr);
        pr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT; pr.size = CL_PUSH_BYTES;
        VkPipelineLayoutCreateInfo lc; memset(&lc, 0, sizeof lc);
        lc.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        lc.setLayoutCount = 2; lc.pSetLayouts = sl; lc.pushConstantRangeCount = 1; lc.pPushConstantRanges = &pr;
        if (vkCreatePipelineLayout(G.gpu.dev, &lc, NULL, &lay) != VK_SUCCESS) return VK_NULL_HANDLE;
    }
    if (layout_out) *layout_out = lay;
    VkShaderModule m = load_module(spv_name);
    if (!m) return VK_NULL_HANDLE;
    VkComputePipelineCreateInfo ci; memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; ci.stage.module = m; ci.stage.pName = "main";
    ci.layout = lay;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateComputePipelines(G.gpu.dev, G.gpu.cache, 1, &ci, NULL, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    vkDestroyShaderModule(G.gpu.dev, m, NULL);
    if (!p) ps2_log("clouds: compute pipeline %s failed", spv_name);
    return p;
}
void ps2_clouds_bind_common(VkCommandBuffer cmd, VkPipelineLayout layout) {
    if (G.cur_set) vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout ? layout : G.pipe_layout, 0, 1,
                                           &G.cur_set, 0, NULL);
}

/* masked: the vertex stage is pmask.vert (the eyes' visible triangles from ps2_video.c's vertex buffer, PS2_PASS_MASK)
   instead of the full-screen triangle; the fragment stage and the state are the same */
static VkPipeline make_composite_g(int masked, int sel, int div);
static VkPipeline make_composite_v(int masked, int sel) { return make_composite_g(masked, sel, 2); }
/* (perf6_clouds) constant 2 of cloud_comp.frag: CL_NEAR_DIV (CLOUD DETAIL's near divisor) */
static VkPipeline make_composite_g(int masked, int sel, int div) {
    VkShaderModule vs = load_module(!masked ? "cockpit_comp.vert.spv" : G.gpu.multiview ? "pmask_mv.vert.spv" : "pmask.vert.spv");
    VkShaderModule fs = load_module(G.gpu.multiview ? "cloud_comp_mv.frag.spv" : "cloud_comp.frag.spv");
    VkPipeline p = VK_NULL_HANDLE;
    if (vs && fs) {
        VkPipelineShaderStageCreateInfo stg[2]; memset(stg, 0, sizeof stg);
        stg[0].sType = stg[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stg[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stg[0].module = vs; stg[0].pName = "main";
        stg[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stg[1].module = fs; stg[1].pName = "main";
        /* ACZ_VR_CLOUD_COMP_UNROLL (cloud_comp.frag constant 0): 1 = the near composite's four-tap loop unrolled, 0 = as
           before; the same statements in the same order */
        int32_t comp_unroll[2] = { sel ? 1 : 0, div };
        VkSpecializationMapEntry sme[2] = { { 0, 0, sizeof(int32_t) }, { 2, 4, sizeof(int32_t) } };
        VkSpecializationInfo spi; memset(&spi, 0, sizeof spi);
        spi.mapEntryCount = 2; spi.pMapEntries = sme; spi.dataSize = sizeof comp_unroll; spi.pData = comp_unroll;
        stg[1].pSpecializationInfo = &spi;
        VkPipelineVertexInputStateCreateInfo vin; memset(&vin, 0, sizeof vin);
        vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkVertexInputBindingDescription vbd = { 0, 16u, VK_VERTEX_INPUT_RATE_VERTEX };   /* (pmask.vert: vec4 a) */
        VkVertexInputAttributeDescription vat = { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0 };
        if (masked) {
            vin.vertexBindingDescriptionCount = 1; vin.pVertexBindingDescriptions = &vbd;
            vin.vertexAttributeDescriptionCount = 1; vin.pVertexAttributeDescriptions = &vat;
        }
        VkPipelineInputAssemblyStateCreateInfo ia; memset(&ia, 0, sizeof ia);
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp; memset(&vp, 0, sizeof vp);
        vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO; vp.viewportCount = 1; vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs; memset(&rs, 0, sizeof rs);
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO; rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms; memset(&ms, 0, sizeof ms);
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds; memset(&ds, 0, sizeof ds);
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO; ds.maxDepthBounds = 1.0f;
        VkPipelineColorBlendAttachmentState cba; memset(&cba, 0, sizeof cba);
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE; cba.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
        VkPipelineColorBlendStateCreateInfo cb; memset(&cb, 0, sizeof cb);
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO; cb.attachmentCount = 1; cb.pAttachments = &cba;
        VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dsi; memset(&dsi, 0, sizeof dsi);
        dsi.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO; dsi.dynamicStateCount = 2; dsi.pDynamicStates = dyn;
        VkFormat cf = VK_FORMAT_R8G8B8A8_UNORM;
        VkPipelineRenderingCreateInfo rci; memset(&rci, 0, sizeof rci);
        rci.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO; rci.colorAttachmentCount = 1;
        rci.pColorAttachmentFormats = &cf; rci.viewMask = G.gpu.multiview ? 3u : 0u;
        VkGraphicsPipelineCreateInfo gp; memset(&gp, 0, sizeof gp);
        gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO; gp.pNext = &rci;
        gp.stageCount = 2; gp.pStages = stg; gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vp; gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pDepthStencilState = &ds;
        gp.pColorBlendState = &cb; gp.pDynamicState = &dsi; gp.layout = G.pipe_layout;
        if (vkCreateGraphicsPipelines(G.gpu.dev, G.gpu.cache, 1, &gp, NULL, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    }
    if (vs) vkDestroyShaderModule(G.gpu.dev, vs, NULL);
    if (fs) vkDestroyShaderModule(G.gpu.dev, fs, NULL);
    return p;
}

static VkPipeline make_composite(void) { return make_composite_v(0, G.comp_unroll); }

/* ---- the cloud depth stamp for the game's effect sprites (ACZ_VR_CLOUD_FX, bug #46) ------------------------------
   The game's smoke, explosions, fire and missile trails are alpha-blended GS sprites that test the depth buffer but
   write none, so a composite drawn after them painted our cloud over a nearer explosion. ps2_video.c now places the far
   composite in front of the frame's first effect sprite (cl_fx), and right after each composite this pass writes our
   cloud's distance into the frame's depth buffer wherever the cloud is dense (transmittance in front below
   ACZ_VR_CLOUD_FX_T): the effects' own depth test then keeps nearer ones in front of the cloud and puts farther ones
   behind it. A full-screen triangle (cloud_comp.frag -DSTAMP), no colour attachment, depth GREATER + write (the
   scene stays wherever it is nearer than the cloud). ACZ_VR_CLOUD_FX: 0 off (the composite where it was, no stamp),
   1 (default) the far composite before the effects + the stamp, 2 the near composite there as well (ps2_video.c);
   ACZ_VR_CLOUD_FX_AB=N: in-run GPU A/B of the stamp; PS2_PERF_AB_FLIP=cloudfx: exact image A/B (placement + stamp). */
static struct { int read, mode, tint, edge; float lim; int ab_off; VkPipeline pipe; uint64_t stamps, px;
                float soft; VkPipeline vpipe; int soft_ab_off;
                VkPipeline pipe_d[3], vpipe_d[3]; } FX;   /* (perf6_clouds: [near divisor - 2]; [0] = pipe / vpipe) */
/* ACZ_VR_CLOUD_FX_SOFT (fx_mpbm 2026-10-31, bug #254): the effects fade by our cloud's transmittance in front of them
   (the veil, cloud_comp.frag -DVEIL, read by gs.frag through ps2_video.c's effect depth snapshot) instead of being cut
   by the binary stamp where it crosses ACZ_VR_CLOUD_FX_T: full where the transmittance in front is above lim + w, gone
   below lim - w, smooth between; the stamp stays only below lim - w (for the effect draws whose blend cannot be faded),
   where the faded effects are gone already. VL: the veil's inputs as the last cloud point of this pass left them. */
static struct { int ok; int32_t pcv[16]; VkDescriptorSet set; uint64_t veils, px; } VL;
static void fx_cfg(void) {
    if (FX.read) return;
    FX.read = 1;
    FX.mode = env_int("ACZ_VR_CLOUD_FX", 1, 0, 2);
    FX.tint = env_int("ACZ_DEBUG_CLOUD_FX_TINT", 0, 0, 1);
    /* the far map's depth fetched edge-aware in the composite and the stamp (cloud_comp.frag far_depth; residual R1 of
       bx_cloudfield, 2026-10-22: a comb along cloud edges against distant terrain); 0 = the plain bilinear fetch */
    FX.edge = env_int("ACZ_VR_CLOUD_EDGE_DEPTH", 1, 0, 1);
    const char *e = getenv("ACZ_VR_CLOUD_FX_T");
    FX.lim = e && *e ? (float)atof(e) : 0.5f;
    if (!(FX.lim >= 0.0f)) FX.lim = 0.0f;
    if (FX.lim > 1.0f) FX.lim = 1.0f;
    e = getenv("ACZ_VR_CLOUD_FX_SOFT");
    FX.soft = e && *e ? (float)atof(e) : 0.25f;          /* the half-width w of the fade about the limit; 0 = binary */
    if (e && !strcmp(e, "off")) FX.soft = 0.0f;
    if (!(FX.soft >= 0.0f)) FX.soft = 0.0f;
    if (FX.soft > 1.0f) FX.soft = 1.0f;
}
static int fx_stamp_now(void) {
    extern int ps2_vk_perf_ab_flipped(const char *name);   /* ps2_video.c: PS2_PERF_AB re-draw with a switch flipped */
    fx_cfg();
    return FX.pipe && !FX.ab_off && ((FX.mode != 0) ^ (ps2_vk_perf_ab_flipped("cloudfx") != 0)) && FX.lim > 0.0f;
}
/* ACZ_VR_CLOUD_FX_SOFT this pass (multiview only; PS2_PERF_AB_FLIP=cloudsoft flips it in the exact A/B re-draw) */
static int fx_soft_now(void) {
    extern int ps2_vk_perf_ab_flipped(const char *name);
    return fx_stamp_now() && FX.vpipe && G.gpu.multiview && !FX.soft_ab_off
        && ((FX.soft > 0.0f) ^ (ps2_vk_perf_ab_flipped("cloudsoft") != 0));
}
/* the stamp's transmittance limit: below lim - w with the veil (none when that is 0) */
static float fx_stamp_lim(void) { return fx_soft_now() ? FX.lim - (FX.soft > 0.0f ? FX.soft : 0.25f) : FX.lim; }
static int fx_edge_now(void) {               /* ACZ_VR_CLOUD_EDGE_DEPTH (PS2_PERF_AB_FLIP=cloudedge flips it) */
    extern int ps2_vk_perf_ab_flipped(const char *name);
    fx_cfg();
    return (FX.edge != 0) ^ (ps2_vk_perf_ab_flipped("cloudedge") != 0);
}
/* ACZ_VR_CLOUD_FAR_SPLIT (cl_split 2026-10-30, bug #250): the far composite shows the part of a cloud in front of a scene
   pixel from the far map's opacity profile (cloud_dir_prof, cloud_comp.frag far_split) instead of all of the cloud or none
   of it by its mean distance; 0 = as before. PS2_PERF_AB_FLIP=cloudsplit flips it in the exact A/B re-draw,
   PS2_PASS_AB_WHAT=cloudsplit turns it off in PS2_PASS_AB's "off" blocks (in-run GPU A/B). */
static int split_cfg = -1;
static int split_now(void) {
    extern int ps2_vk_perf_ab_flipped(const char *name);
    extern int ps2_vk_pass_ab_off(int what);
    if (split_cfg < 0) {
        split_cfg = env_int("ACZ_VR_CLOUD_FAR_SPLIT", 1, 0, 1);
        ps2_log("clouds: far composite %s (ACZ_VR_CLOUD_FAR_SPLIT)", split_cfg ? "shows the part of a cloud in front of the "
                "scene (the far map's opacity profile)" : "by the cloud's mean distance (as before)");
    }
    return (split_cfg && !ps2_vk_pass_ab_off(13)) ^ (ps2_vk_perf_ab_flipped("cloudsplit") != 0);
}
/* ACZ_VR_CLOUD_COMP_EARLY (cl_split 2026-10-30): the far composite leaves an empty map texel before fetching the scene's
   and the map's depth (cloud_comp.frag; the same pixels as before are discarded, the image is identical); 0 = as before.
   PS2_PERF_AB_FLIP=compearly flips it in the exact A/B re-draw, PS2_PASS_AB_WHAT=compearly in PS2_PASS_AB's "off"
   blocks. */
static int early_cfg = -1;
static int early_now(void) {
    extern int ps2_vk_perf_ab_flipped(const char *name);
    extern int ps2_vk_pass_ab_off(int what);
    if (early_cfg < 0) early_cfg = env_int("ACZ_VR_CLOUD_COMP_EARLY", 1, 0, 1);
    return (early_cfg && !ps2_vk_pass_ab_off(15)) ^ (ps2_vk_perf_ab_flipped("compearly") != 0);
}
static VkPipeline make_stamp_d(int veil, int div);
static VkPipeline make_stamp(int veil) { return make_stamp_d(veil, 2); }
static VkPipeline make_stamp_d(int veil, int div) {
    VkShaderModule vs = load_module("cockpit_comp.vert.spv");
    VkShaderModule fs = load_module(veil ? (G.gpu.multiview ? "cloud_veil_mv.frag.spv" : "cloud_veil.frag.spv")
                                         : G.gpu.multiview ? "cloud_stamp_mv.frag.spv" : "cloud_stamp.frag.spv");
    VkPipeline p = VK_NULL_HANDLE;
    if (vs && fs) {
        VkPipelineShaderStageCreateInfo stg[2]; memset(stg, 0, sizeof stg);
        stg[0].sType = stg[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stg[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stg[0].module = vs; stg[0].pName = "main";
        stg[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stg[1].module = fs; stg[1].pName = "main";
        int32_t sdiv = div;                                 /* (perf6_clouds: cloud_comp.frag CL_NEAR_DIV; none at 2) */
        VkSpecializationMapEntry sme = { 2, 0, sizeof(int32_t) };
        VkSpecializationInfo spi; memset(&spi, 0, sizeof spi);
        spi.mapEntryCount = 1; spi.pMapEntries = &sme; spi.dataSize = sizeof sdiv; spi.pData = &sdiv;
        if (div != 2) stg[1].pSpecializationInfo = &spi;
        VkPipelineVertexInputStateCreateInfo vin; memset(&vin, 0, sizeof vin);
        vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo ia; memset(&ia, 0, sizeof ia);
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp; memset(&vp, 0, sizeof vp);
        vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO; vp.viewportCount = 1; vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs; memset(&rs, 0, sizeof rs);
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO; rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms; memset(&ms, 0, sizeof ms);
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds; memset(&ds, 0, sizeof ds);
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO; ds.maxDepthBounds = 1.0f;
        ds.depthTestEnable = veil ? VK_FALSE : VK_TRUE; ds.depthWriteEnable = veil ? VK_FALSE : VK_TRUE;
        ds.depthCompareOp = VK_COMPARE_OP_GREATER;
        VkPipelineColorBlendAttachmentState cba; memset(&cba, 0, sizeof cba);   /* (the veil: RGBA16F, written as it is) */
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT
                           | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb; memset(&cb, 0, sizeof cb);
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        if (veil) { cb.attachmentCount = 1; cb.pAttachments = &cba; }
        VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dsi; memset(&dsi, 0, sizeof dsi);
        dsi.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO; dsi.dynamicStateCount = 2; dsi.pDynamicStates = dyn;
        VkPipelineRenderingCreateInfo rci; memset(&rci, 0, sizeof rci);
        rci.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        VkFormat vf = VK_FORMAT_R16G16B16A16_SFLOAT;
        if (veil) { rci.colorAttachmentCount = 1; rci.pColorAttachmentFormats = &vf; }
        else rci.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
        rci.viewMask = G.gpu.multiview ? 3u : 0u;
        VkGraphicsPipelineCreateInfo gp; memset(&gp, 0, sizeof gp);
        gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO; gp.pNext = &rci;
        gp.stageCount = 2; gp.pStages = stg; gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vp; gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pDepthStencilState = &ds;
        gp.pColorBlendState = &cb; gp.pDynamicState = &dsi; gp.layout = G.pipe_layout;
        if (vkCreateGraphicsPipelines(G.gpu.dev, G.gpu.cache, 1, &gp, NULL, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    }
    if (vs) vkDestroyShaderModule(G.gpu.dev, vs, NULL);
    if (fs) vkDestroyShaderModule(G.gpu.dev, fs, NULL);
    return p;
}

/* ---- CLOUD DETAIL (perf6_clouds 2026-11-03; rs_perf6 report #12, the user's decision 3 of 2026-11-03) ----
   ACZ_VR_CLOUD_DETAIL = high (default: today's clouds, byte for byte) | medium | low -- the CLOUD DETAIL rungs for the
   LOW / MEDIUM presets of weaker PCs (perf6_presets sets the switch); or a lever list for measurements, e.g.
   "div=3,step=12.5,grow=2,tier=40,detail=0.3". A rung changes only the near volume (4-600 m: the in-cloud veil, near
   cloud edges); the far map (> 600 m), its composite, the shadow map, the optics and the wake stay as they are:
     div     the near buffer's texel covers div x div target pixels (2 = half resolution per axis, HIGH);
     step    the plane spacing near the camera (ACZ_VR_CLOUD_NEAR_STEP, HIGH 6.25 m);
     grow / tier  the distance tiers (ACZ_VR_CLOUD_NEAR_GROW / _TIER, HIGH 2.4 / 60 m);
     detail  the near-only detail octave (ACZ_VR_CLOUD_NEAR_DETAIL, HIGH 0.6).
   medium = div 3, low = div 4 (not step 12.5: the two plane families' seam and the plane contours show in thin cloud
   against the sky). An explicitly set ACZ_VR_CLOUD_NEAR_* switch wins on every level; read once: a restart item. The near buffer is made for the smallest divisor in
   use (a lower rung alone: smaller images); the composites, the effects' depth stamp and veil read it with the pass's
   divisor (cloud_comp.frag CL_NEAR_DIV), the march marches it (cloud_near.comp NEAR_DIV). The in-between near reuse
   (ACZ_VR_CLOUD_NEAR_REUSE) and the coarse periphery run at divisor 2 only.
   ACZ_VR_CLOUD_DETAIL_AB=N [measurement; used when no other cloud A/B is set]: blocks of N recorded frames rotate the
   configured level (A) with an entry of ACZ_VR_CLOUD_DETAIL_AB_SET taken at random per pair (default "medium,low"; levers
   as above plus spec=N, the block's ACZ_VR_CLOUD_NEAR_SPEC: 0 = without the exact trim, 9 / 17 / 33 the bound tests;
   "notrim" = spec=0); the first 4 frames of a block are not counted.
   A "clouds detail A/B: block" line per block (frame GPU and the cloud passes' own GPU time, game / in-between frames)
   and every 20 B blocks of an entry "clouds detail A/B:" its paired differences to the A block before it (mean +-
   standard error). PS2_PERF_AB_FLIP=clouddetail: the same frame re-drawn at another level -- with ACZ_VR_CLOUD_DETAIL_AB_SET
   set the k-th dumped frame's _nc images at its k-th entry in turn (the plain images at the configured level), else at
   HIGH (the plain images at the configured rung). */
static const struct { const char *name; ps2_cloud_detail lv; } cd_rungs[] = {
    { "high",   { 2, 0.0f, 0.0f, 0.0f, -1.0f, -1 } },
    { "medium", { 3, 0.0f, 0.0f, 0.0f, -1.0f, -1 } },
    { "low",    { 4, 0.0f, 0.0f, 0.0f, -1.0f, -1 } },   /* (not step 12.5: the two plane families' seam and the plane
                                                           contours show in thin cloud against the sky, M3 passing cumulus;
                                                           near march -71 % with it, -58 % without) */
};
static struct {
    int init, n, cfg;                     /* levels; the configured one */
    ps2_cloud_detail lv[CL_DETAIL_MAX];
    char name[CL_DETAIL_MAX][40];
    int cur;                              /* this frame's level */
    int flip_lv;                          /* PS2_PERF_AB_FLIP=clouddetail: the level of the running re-draw */
    uint32_t flip_seq;                    /* ... the re-draws so far (the rotation over ACZ_VR_CLOUD_DETAIL_AB_SET) */
    int min_div;                          /* the near images are made for this divisor (the smallest in use) */
    int ab, nset, set_lv[8];              /* ACZ_VR_CLOUD_DETAIL_AB: the block length, the B entries */
    char set_name[8][40];
} CD;
/* a rung by name or a lever list -> lv; 1 = ok */
static int cd_parse(const char *e, ps2_cloud_detail *lv) {
    for (size_t i = 0; i < sizeof cd_rungs / sizeof cd_rungs[0]; i++)
        if (!strcmp(e, cd_rungs[i].name)) { *lv = cd_rungs[i].lv; return 1; }
    if (!strchr(e, '=')) return 0;
    ps2_cloud_detail d = { 2, 0.0f, 0.0f, 0.0f, -1.0f, -1 };
    for (const char *q = e; *q; ) {
        const char *eq = strchr(q, '=');
        if (!eq) break;
        size_t kl = (size_t)(eq - q);
        float v = (float)atof(eq + 1);
        if (kl == 3 && !strncmp(q, "div", 3)) d.div = (int)v;
        else if (kl == 4 && !strncmp(q, "step", 4)) d.step = v;
        else if (kl == 4 && !strncmp(q, "grow", 4)) d.grow = v;
        else if (kl == 4 && !strncmp(q, "tier", 4)) d.tier = v;
        else if (kl == 6 && !strncmp(q, "detail", 6)) d.detail = v;
        else if (kl == 4 && !strncmp(q, "spec", 4)) d.spec = (int)v;
        while (*q && *q != ',') q++;
        if (*q == ',') q++;
    }
    if (d.div < 2) d.div = 2;
    if (d.div > 4) d.div = 4;
    *lv = d;
    return 1;
}
static int cd_add(const char *name, const ps2_cloud_detail *lv) {
    for (int i = 0; i < CD.n; i++) if (!strcmp(CD.name[i], name)) return i;
    if (CD.n >= CL_DETAIL_MAX) return 0;
    CD.lv[CD.n] = *lv;
    snprintf(CD.name[CD.n], sizeof CD.name[0], "%s", name);
    return CD.n++;
}
static void cd_cfg(void) {
    if (CD.init) return;
    CD.init = 1;
    CD.n = 1; CD.lv[0] = cd_rungs[0].lv; snprintf(CD.name[0], sizeof CD.name[0], "high");
    const char *e = getenv("ACZ_VR_CLOUD_DETAIL");
    ps2_cloud_detail d;
    if (e && *e && strcmp(e, "high") && strcmp(e, "default")) {
        if (cd_parse(e, &d)) CD.cfg = cd_add(e, &d);
        else ps2_log("clouds: ACZ_VR_CLOUD_DETAIL=%s unknown (high, medium, low or a lever list): high", e);
    }
    CD.ab = env_int("ACZ_VR_CLOUD_DETAIL_AB", 0, 0, 100000);
    const char *ff = getenv("PS2_PERF_AB_FLIP");
    int cd_flip = ff && strstr(ff, "clouddetail") != NULL;
    if (CD.ab || (cd_flip && getenv("ACZ_VR_CLOUD_DETAIL_AB_SET"))) {
        /* the entries: separated by ';' when the list holds a lever list ("div=3,step=12.5;low"), else by ',' */
        const char *s0 = getenv("ACZ_VR_CLOUD_DETAIL_AB_SET");
        const char *q = s0 && *s0 ? s0 : "medium,low";
        char sep = strchr(q, '=') ? ';' : ',';
        while (*q && CD.nset < 8) {
            char u[40]; size_t l = 0;
            while (q[l] && q[l] != sep) l++;
            snprintf(u, sizeof u, "%.*s", (int)(l < sizeof u - 1 ? l : sizeof u - 1), q);
            q += l + (q[l] ? 1u : 0u);
            if (!u[0]) continue;
            int k = CD.nset;
            snprintf(CD.set_name[k], sizeof CD.set_name[0], "%s", u);
            if (!strcmp(u, "notrim")) { d = CD.lv[CD.cfg]; d.spec = 0; CD.set_lv[k] = cd_add(u, &d); CD.nset++; }
            else if (cd_parse(u, &d)) { CD.set_lv[k] = cd_add(u, &d); CD.nset++; }
        }
        if (!CD.nset) CD.ab = 0;
    }
    CD.cur = CD.cfg;
    CD.flip_lv = 0;
    /* the divisors in use: the configured level, the A/B's, HIGH's for the flip's re-draw */
    const char *f = getenv("PS2_PERF_AB_FLIP");
    int flip = f && strstr(f, "clouddetail") != NULL;
    CD.min_div = CD.lv[CD.cfg].div;
    for (int i = 0; i < CD.nset; i++) if (CD.lv[CD.set_lv[i]].div < CD.min_div) CD.min_div = CD.lv[CD.set_lv[i]].div;
    if (flip && CD.lv[0].div < CD.min_div) CD.min_div = CD.lv[0].div;
    if (CD.cfg || CD.ab)
        ps2_log("clouds: CLOUD DETAIL %s (ACZ_VR_CLOUD_DETAIL): near texel %dx%d px, step %.2f, grow %.2f, tier %.0f, detail "
                "%.2f (0 / < 0: HIGH's); %d levels, the near images for divisor %d%s", CD.name[CD.cfg], CD.lv[CD.cfg].div,
                CD.lv[CD.cfg].div, CD.lv[CD.cfg].step, CD.lv[CD.cfg].grow, CD.lv[CD.cfg].tier, CD.lv[CD.cfg].detail, CD.n,
                CD.min_div, CD.ab ? "; A/B (ACZ_VR_CLOUD_DETAIL_AB)" : "");
}
int ps2_clouds_detail_levels(ps2_cloud_detail *lv, int max) {
    cd_cfg();
    int n = CD.n < max ? CD.n : max;
    for (int i = 0; i < n; i++) lv[i] = CD.lv[i];
    return n;
}
int ps2_clouds_detail_cur(void) {
    extern int ps2_vk_perf_ab_flipped(const char *name);
    cd_cfg();
    if (ps2_vk_perf_ab_flipped("clouddetail")) return CD.flip_lv;
    return CD.cur;
}
int ps2_clouds_detail_used(int level) {
    cd_cfg();
    if (level == CD.cfg) return 1;
    for (int i = 0; i < CD.nset; i++) if (CD.set_lv[i] == level) return 1;
    const char *f = getenv("PS2_PERF_AB_FLIP");
    return level == 0 && f && strstr(f, "clouddetail") != NULL;
}
int ps2_clouds_trim_off(void) {
    extern int ps2_vk_perf_ab_flipped(const char *name);
    return ps2_vk_perf_ab_flipped("nearspec");
}
/* PS2_PERF_AB_FLIP=clouddetail: a flipped re-draw starts: its level (the next entry of the set, else HIGH) */
static void cd_flip_step(void) {
    extern int ps2_vk_perf_ab_flipped(const char *name);
    if (!ps2_vk_perf_ab_flipped("clouddetail")) return;
    CD.flip_lv = CD.nset ? CD.set_lv[CD.flip_seq % (uint32_t)CD.nset] : 0;
    ps2_log("clouds detail flip: re-draw %u of this dump's frame at level %s (_nc images; the plain images at %s)",
            CD.flip_seq, CD.name[CD.flip_lv], CD.name[CD.cfg]);
    CD.flip_seq++;
}
/* this pass's divisor and the used part of the near images (the CloudFrame sizes, the march's dispatch); a
   composite-only pass (ACZ_REC_CINE) reads the frame's near buffer: its used part stays the frame's */
static void cd_pass(const ps2_cloud_view *v, int composite_only) {
    int lv = ps2_clouds_detail_cur();
    int d = CD.lv[lv >= 0 && lv < CD.n ? lv : 0].div;
    G.gpu.near_div = d;
    if (composite_only) return;
    uint32_t w = v ? (v->target_w + (uint32_t)d - 1u) / (uint32_t)d : 0u, h = v ? (v->target_h + (uint32_t)d - 1u) / (uint32_t)d : 0u;
    G.gpu.near_lw = w < G.near_w ? w : G.near_w;
    G.gpu.near_lh = h < G.near_h ? h : G.near_h;
}
/* the composite for a near pass at divisor div; the far one: p_comp / p_comp_pm */
static VkPipeline cd_comp(int masked, int div) {
    if (div < 2 || div > 4) div = 2;
    VkPipeline p = G.p_cdv[masked ? 1 : 0][div - 2];
    return p ? p : masked ? G.p_comp_pm : G.p_comp;
}
static VkPipeline cd_fx(int veil) {
    int d = G.gpu.near_div >= 2 && G.gpu.near_div <= 4 ? G.gpu.near_div : 2;
    VkPipeline p = veil ? FX.vpipe_d[d - 2] : FX.pipe_d[d - 2];
    return p ? p : veil ? FX.vpipe : FX.pipe;
}
/* ACZ_VR_CLOUD_DETAIL_AB's sums: the open block (game / in-between frames: frame GPU, cloud passes; the near march and
   composite) and per B entry the paired differences to the last A block */
#define CDA_Q 6                                  /* quantities: game frame, in-between frame, clouds game, clouds in-between,
                                                    near march, near composite */
static struct {
    int open, st; uint32_t blk;
    double s[CDA_Q]; uint32_t n[CDA_Q];
    double a[CDA_Q]; int ha[CDA_Q];
    double ds[9][CDA_Q], dq[9][CDA_Q]; uint32_t dn[9][CDA_Q];
    double ms[9][CDA_Q]; uint32_t mn[9][CDA_Q];
    uint32_t bblocks[9];
} CDA;
static uint32_t cda_blk_of[CL_FIF];
/* the cloud passes of frame slot fi split: [0] early update, [1] far point + composite, [2] near march, [3] near composite */
static void cl_peek_split(int fi, double o[4]) {
    o[0] = o[1] = o[2] = o[3] = 0.0;
    if (fi < 0 || fi >= (int)CL_FIF || !G.qp[fi] || !G.qmask[fi]) return;
    for (uint32_t k = 0; k < CL_PASSES; k++) {
        if (!(G.qmask[fi] & (1u << k))) continue;
        uint64_t t[3];
        if (vkGetQueryPoolResults(G.gpu.dev, G.qp[fi], k * 3u, 3, sizeof t, t, sizeof t[0], VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
            continue;
        double a = (double)(t[1] - t[0]) * G.ts_ns * 1e-6, b = (double)(t[2] - t[1]) * G.ts_ns * 1e-6;
        uint32_t kind = G.qkind[fi][k];
        if (kind == 2u) o[0] += a + b;
        else if (kind == 0u) o[1] += a + b;
        else { o[2] += a; o[3] += b; }
    }
}
static void cda_close(void) {
    static const char *qn[CDA_Q] = { "game", "in-between", "clouds game", "clouds in-between", "near march", "near comp" };
    double m[CDA_Q]; int hm[CDA_Q];
    for (int q = 0; q < CDA_Q; q++) { hm[q] = CDA.n[q] > 0; m[q] = hm[q] ? CDA.s[q] / CDA.n[q] : 0.0; }
    int st = CDA.st;
    ps2_log("clouds detail A/B: block %u %s (%s): frames %u game / %u in-between; frame GPU %.4f / %.4f ms; cloud passes "
            "%.4f / %.4f; near march %.4f, near composite %.4f ms", CDA.blk, st ? "B" : "A",
            st ? CD.set_name[st - 1] : CD.name[CD.cfg], CDA.n[0], CDA.n[1], m[0], m[1], m[2], m[3], m[4], m[5]);
    for (int q = 0; q < CDA_Q; q++) {
        if (!hm[q]) continue;
        CDA.ms[st][q] += m[q]; CDA.mn[st][q]++;
        if (st == 0) { CDA.a[q] = m[q]; CDA.ha[q] = 1; }
        else if (CDA.ha[q]) { double d = m[q] - CDA.a[q]; CDA.ds[st][q] += d; CDA.dq[st][q] += d * d; CDA.dn[st][q]++; }
    }
    if (st && ++CDA.bblocks[st] % 20u == 0u) {
        char line[1024]; size_t o = 0;
        o += (size_t)snprintf(line + o, sizeof line - o, "clouds detail A/B: %s vs %s (%u B blocks), B - A per frame:",
                              CD.set_name[st - 1], CD.name[CD.cfg], CDA.bblocks[st]);
        for (int q = 0; q < CDA_Q && o < sizeof line - 80; q++) {
            uint32_t n = CDA.dn[st][q];
            if (!n) continue;
            double md = CDA.ds[st][q] / n, var = n > 1 ? (CDA.dq[st][q] - n * md * md) / (n - 1) : 0.0;
            o += (size_t)snprintf(line + o, sizeof line - o, " %s %+.4f +- %.4f (A %.4f, %u)%s", qn[q], md,
                                  var > 0.0 ? sqrt(var / n) : 0.0, CDA.mn[0][q] ? CDA.ms[0][q] / CDA.mn[0][q] : 0.0, n,
                                  q + 1 < CDA_Q ? ";" : "");
        }
        ps2_log("%s", line);
    }
    memset(CDA.s, 0, sizeof CDA.s); memset(CDA.n, 0, sizeof CDA.n);
}
static void cda_sample(int fi, int st, double ms) {
    uint32_t blk = cda_blk_of[fi];
    if (CDA.open && blk != CDA.blk) cda_close();
    if (!CDA.open || blk != CDA.blk) { CDA.open = 1; CDA.blk = blk; CDA.st = st; }
    double sp[4];
    cl_peek_split(fi, sp);
    int ib = G.inb[fi] ? 1 : 0;
    CDA.s[ib] += ms; CDA.n[ib]++;
    CDA.s[2 + ib] += sp[0] + sp[1] + sp[2] + sp[3]; CDA.n[2 + ib]++;
    if (sp[2] > 0.0) { CDA.s[4] += sp[2]; CDA.n[4]++; CDA.s[5] += sp[3]; CDA.n[5]++; }
}

static VkSampler make_sampler(VkFilter f, VkSamplerAddressMode am, float max_lod) {
    VkSamplerCreateInfo si; memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO; si.magFilter = si.minFilter = f;
    si.mipmapMode = max_lod > 0.0f ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = am; si.maxLod = max_lod;
    VkSampler s = VK_NULL_HANDLE;
    if (vkCreateSampler(G.gpu.dev, &si, NULL, &s) != VK_SUCCESS) s = VK_NULL_HANDLE;
    return s;
}

/* ---- light shafts in the haze (ACZ_VR_CLOUD_SHAFTS, cl_split 2026-10-30, bug #250) --------------------------------------
   a trueSKY-based game darkens its atmosphere's in-scattered light where clouds shadow the air (two sun-aligned inscatter
   volumes, the "godrays", amortised 16 x 8 texels a frame). Here: an eye-independent octahedral map (cloud_shaft, S x S)
   of the haze's shadow excess along each direction at four distances, marched from the centre eye through the cloud
   shadow map (cloud_shafts.comp), S / SLICES rows a frame (in-between frames too; a full refresh every SLICES frames,
   restarted at a rebase); the far composite darkens the frame by it in front of the scene / the cloud (cloud_comp.frag).
   Relative to the mean shadow of the air around the eye (cloud_shafts.comp reference(): 128 points on a horizontal disc of
   the march's radius): an overcast deck or a uniformly shaded area keeps the game's haze and the horizon's mean is
   unchanged; shaded columns of air darken, sunlit ones between them brighten by the fog colour
   (ACZ_VR_CLOUD_SHAFTS_BRIGHT). Gated by direct sunlight as the game paints it (its LIGHT palette luminance
   0.50 .. 0.62, the optics' ACZ_VR_OPTICS_SUNLIT default: storms, dusk under cloud and night get none). Strength:
   ACZ_VR_CLOUD_SHAFTS (0 = off, the default: experimental; 1 = 0.35 of the shadowed haze). PS2_PERF_AB_FLIP=shafts:
   the exact image A/B; PS2_PASS_AB_WHAT=cloudshafts: the in-run GPU A/B (off blocks: no pass, no use). */
typedef struct {
    int read, on, res, slices, steps, bright;
    float strength, g, mix, H, lit_lo, lit_hi;
    uint32_t row, rebase; int valid, have_rebase;
    float d[4], K, sigma, fn, ff, tend; int lit_ok;
    uint64_t passes;
} cl_shafts;
static cl_shafts SH;
static float env_f(const char *n, float def, float lo, float hi) {
    const char *e = getenv(n);
    float v = e && *e ? (float)atof(e) : def;
    return v < lo ? lo : v > hi ? hi : v;
}
static const cl_shafts *sh_cfg(void) {
    if (SH.read) return &SH;
    SH.read = 1;
    SH.strength = env_f("ACZ_VR_CLOUD_SHAFTS", 0.0f, 0.0f, 3.0f);   /* (experimental: off by default) */
    SH.on = SH.strength > 0.0f;
    SH.res = ((int)env_f("ACZ_VR_CLOUD_SHAFTS_RES", 512.0f, 64.0f, 2048.0f)) & ~7;
    SH.slices = (int)env_f("ACZ_VR_CLOUD_SHAFTS_SLICES", 4.0f, 1.0f, 16.0f);
    SH.steps = (int)env_f("ACZ_VR_CLOUD_SHAFTS_STEPS", 32.0f, 8.0f, 128.0f);
    SH.bright = (int)env_f("ACZ_VR_CLOUD_SHAFTS_BRIGHT", 1.0f, 0.0f, 1.0f);
    SH.H = env_f("ACZ_VR_CLOUD_SHAFTS_H", 2000.0f, 100.0f, 20000.0f);
    SH.g = 0.5f; SH.mix = 0.5f;
    {   const char *e = getenv("ACZ_VR_CLOUD_SHAFTS_PHASE");          /* "g,mix" */
        float a, b;
        if (e && sscanf(e, "%f,%f", &a, &b) == 2) {
            SH.g = a < -0.9f ? -0.9f : a > 0.9f ? 0.9f : a;
            SH.mix = b < 0.0f ? 0.0f : b > 1.0f ? 1.0f : b;
        } }
    SH.lit_lo = 0.50f; SH.lit_hi = 0.62f;
    {   const char *e = getenv("ACZ_VR_CLOUD_SHAFTS_LIT");            /* "lo,hi" (0 = no gate) */
        float a, b;
        if (e && e[0] == '0' && (e[1] == '\0' || e[1] == ',')) SH.lit_lo = SH.lit_hi = 0.0f;
        else if (e && sscanf(e, "%f,%f", &a, &b) == 2 && b > a) { SH.lit_lo = a; SH.lit_hi = b; } }
    return &SH;
}
/* the shafts are used by this pass's composite: on, the map complete, sunlit, not in a PS2_PASS_AB "off" block; the exact
   A/B re-draw flips it (PS2_PERF_AB_FLIP=shafts) */
static int sh_use_now(void) {
    extern int ps2_vk_perf_ab_flipped(const char *name);
    extern int ps2_vk_pass_ab_off(int what);
    int ready = SH.on && G.p_shafts && SH.valid && SH.lit_ok;
    return (ready && !ps2_vk_pass_ab_off(14)) ^ (ready && ps2_vk_perf_ab_flipped("shafts") != 0);
}
/* this frame's parameters (before the first UBO of the command buffer): the sunlit gate, the reference shadow, the
   distances from the game's fog range */
static void sh_params(void) {
    sh_cfg();
    if (!SH.on || !G.p_shafts) return;
    const ps2_cloud_env *e = &G.env;
    float lum = 0.299f * e->lit[0] + 0.587f * e->lit[1] + 0.114f * e->lit[2], gate = 1.0f;
    if (SH.lit_hi > 0.0f) {
        float x = (lum - SH.lit_lo) / (SH.lit_hi - SH.lit_lo);
        x = x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
        gate = x * x * (3.0f - 2.0f * x);
    }
    SH.lit_ok = gate > 0.0f;
    SH.K = 0.35f * SH.strength * gate;
    SH.sigma = -1.0f;                                         /* (the shader's own reference: the air around the eye) */
    SH.ff = (e->valid & CL_ENV_COLOURS) && e->fog_far_m > 1000.0f ? e->fog_far_m : 8000.0f;
    SH.fn = (e->valid & CL_ENV_COLOURS) && e->fog_near_m < SH.ff - 500.0f ? e->fog_near_m : 0.0f;
    SH.tend = SH.ff < 8500.0f ? SH.ff : 8500.0f;               /* (the shadow map fades out by ~8.5 km) */
    static const float q[4] = { 0.125f, 0.3f, 0.55f, 1.0f };
    for (int i = 0; i < 4; i++) SH.d[i] = SH.tend * q[i];
}
/* a slice of the map (early in the command buffer, after the shadow pass; the common set is bound) */
static void shafts_pass(VkCommandBuffer cmd) {
    extern int ps2_vk_pass_ab_off(int what);
    if (!SH.on || !G.p_shafts || ps2_vk_pass_ab_off(14)) return;
    if (!SH.have_rebase || G.rebase != SH.rebase) { SH.rebase = G.rebase; SH.have_rebase = 1; SH.valid = 0; SH.row = 0; }
    if (!SH.lit_ok) return;
    uint32_t N = (uint32_t)SH.res, rows = ((N + (uint32_t)SH.slices - 1u) / (uint32_t)SH.slices + 7u) & ~7u;
    if (SH.row >= N) SH.row = 0;
    if (SH.row + rows > N) rows = N - SH.row;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, G.p_shafts);
    int32_t pc[16]; memset(pc, 0, sizeof pc);
    pc[0] = (int32_t)N; pc[1] = (int32_t)SH.row; pc[2] = (int32_t)rows; pc[3] = SH.steps;
    float f[8] = { SH.K, SH.sigma, SH.H, SH.tend, SH.fn, SH.ff, SH.g, SH.mix };
    memcpy(&pc[8], f, sizeof f);
    vkCmdPushConstants(cmd, G.pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, CL_PUSH_BYTES, pc);
    vkCmdDispatch(cmd, N / 8u, (rows + 7u) / 8u, 1);
    SH.row += rows;
    if (SH.row >= N) { SH.row = 0; SH.valid = 1; }
    SH.passes++;
    if (cfg.debug > 0 && (SH.passes % (uint64_t)cfg.debug) == 0u)
        ps2_log("clouds: shafts K %.3f (LIGHT luminance %.2f), fog %.0f..%.0f m, distances %.0f %.0f %.0f "
                "%.0f m, map %s", (double)SH.K, (double)(0.299f * G.env.lit[0] + 0.587f * G.env.lit[1] + 0.114f * G.env.lit[2]),
                (double)SH.fn, (double)SH.ff, (double)SH.d[0], (double)SH.d[1], (double)SH.d[2],
                (double)SH.d[3], SH.valid ? "complete" : "filling");
}

int ps2_clouds_vk_init(const ps2_clouds_init_info *ii) {
    if (!ps2_clouds_enabled()) return 0;
    memset(&G, 0, sizeof G);
    G.gpu.dev = ii->dev; G.gpu.phys = ii->phys; G.gpu.cache = ii->cache; G.gpu.multiview = ii->multiview;
    snprintf(G.shader_dir, sizeof G.shader_dir, "%s", ii->shader_dir);
    G.gpu.shader_dir = G.shader_dir;
    G.gpu.dir_res = (uint32_t)cfg.far_res;
    VkDevice dev = ii->dev;
    vkGetPhysicalDeviceMemoryProperties(ii->phys, &G.mp);
    VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(ii->phys, &pr);
    G.ts_ns = pr.limits.timestampPeriod;
    G.ubo_slot = CL_UBO_SLOT;
    if (pr.limits.minUniformBufferOffsetAlignment > G.ubo_slot) G.ubo_slot = (uint32_t)pr.limits.minUniformBufferOffsetAlignment;
    {   VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(ii->phys, VK_FORMAT_R16_SFLOAT, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
            ps2_log("clouds: R16_SFLOAT storage images not supported -- clouds off"); return -1;
        }
    }
    G.s_rep = make_sampler(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT, 0.0f);
    G.s_clamp = make_sampler(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 0.0f);
    G.s_near = make_sampler(VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 0.0f);
    G.s_mip = make_sampler(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT, 16.0f);     /* the noise mip chains */
    /* the common set */
    VkDescriptorSetLayoutBinding b[CL_B_COUNT]; memset(b, 0, sizeof b);
    for (uint32_t i = 0; i < CL_B_COUNT; i++) {
        b[i].binding = i; b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        switch (i) {
        case CL_B_FRAME: b[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; break;
        case CL_B_DIR_IMG: case CL_B_DIR_DEPTH_IMG: case CL_B_NEAR_IMG: case CL_B_NEAR_DEPTH_IMG: case CL_B_WEATHER_IMG:
        case CL_B_NOISE_BASE_IMG: case CL_B_NOISE_DETAIL_IMG: case CL_B_NEAR_FAR_IMG: case CL_B_NEAR_PAR_IMG:
        case CL_B_DIR_PROF_IMG: case CL_B_SHAFT_IMG:
            b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; break;
        default: b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; break;
        }
    }
    VkDescriptorSetLayoutCreateInfo lci; memset(&lci, 0, sizeof lci);
    lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO; lci.bindingCount = CL_B_COUNT; lci.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev, &lci, NULL, &G.set_layout) != VK_SUCCESS) goto fail;
    {   VkPushConstantRange pc; memset(&pc, 0, sizeof pc);
        pc.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT; pc.size = CL_PUSH_BYTES;
        VkPipelineLayoutCreateInfo plc; memset(&plc, 0, sizeof plc);
        plc.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plc.setLayoutCount = 1; plc.pSetLayouts = &G.set_layout; plc.pushConstantRangeCount = 1; plc.pPushConstantRanges = &pc;
        if (vkCreatePipelineLayout(dev, &plc, NULL, &G.pipe_layout) != VK_SUCCESS) goto fail;
    }
    {   VkDescriptorPoolSize ps[3];
        uint32_t nsets = CL_FIF * CL_PASSES;
        ps[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; ps[0].descriptorCount = nsets;
        ps[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; ps[1].descriptorCount = nsets * 11u;
        ps[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; ps[2].descriptorCount = nsets * 13u;
        VkDescriptorPoolCreateInfo pci; memset(&pci, 0, sizeof pci);
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO; pci.maxSets = nsets; pci.poolSizeCount = 3; pci.pPoolSizes = ps;
        if (vkCreateDescriptorPool(dev, &pci, NULL, &G.pool) != VK_SUCCESS) goto fail;
        VkDescriptorSetLayout lays[CL_PASSES];
        for (uint32_t k = 0; k < CL_PASSES; k++) lays[k] = G.set_layout;
        for (uint32_t f = 0; f < CL_FIF; f++) {
            VkDescriptorSetAllocateInfo ai; memset(&ai, 0, sizeof ai);
            ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO; ai.descriptorPool = G.pool;
            ai.descriptorSetCount = CL_PASSES; ai.pSetLayouts = lays;
            if (vkAllocateDescriptorSets(dev, &ai, G.sets[f]) != VK_SUCCESS) goto fail;
        }
    }
    if (ps2_clouds_make_buffer((VkDeviceSize)G.ubo_slot * CL_FIF * CL_PASSES, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 1,
                               &G.ubo, &G.ubo_mem, (void **)&G.ubo_ptr) != 0) goto fail;
    {   uint32_t N = G.gpu.dir_res;
        VkImageUsageFlags st = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                             | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (make_img(&G.dir, VK_IMAGE_TYPE_2D, N, N, 1, 1, VK_FORMAT_R16G16B16A16_SFLOAT, st)
            || make_img(&G.dir_depth, VK_IMAGE_TYPE_2D, N, N, 1, 1, VK_FORMAT_R16_SFLOAT, st)
            || make_img(&G.dir_prof, VK_IMAGE_TYPE_2D, N, N, 1, 1, VK_FORMAT_R16G16B16A16_SFLOAT, st)
            || (sh_cfg()->on && cfg.far_on            /* (ACZ_VR_CLOUD_SHAFTS only; off: its bindings show dir_prof) */
                && make_img(&G.shaft, VK_IMAGE_TYPE_2D, (uint32_t)sh_cfg()->res, (uint32_t)sh_cfg()->res, 1, 1,
                            VK_FORMAT_R16G16B16A16_SFLOAT, st))
            || make_img(&G.weather, VK_IMAGE_TYPE_2D, CL_WEATHER_RES, CL_WEATHER_RES, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, st)
            || make_img_m(&G.noise_base, VK_IMAGE_TYPE_3D, CL_NOISE_BASE_RES, CL_NOISE_BASE_RES, CL_NOISE_BASE_RES, 1,
                          VK_FORMAT_R8G8B8A8_UNORM, st, CL_NOISE_BASE_MIPS)
            || make_img_m(&G.noise_detail, VK_IMAGE_TYPE_3D, CL_NOISE_DETAIL_RES, CL_NOISE_DETAIL_RES, CL_NOISE_DETAIL_RES, 1,
                          VK_FORMAT_R8G8B8A8_UNORM, st, CL_NOISE_DETAIL_MIPS)) goto fail;
        G.gpu.dir = G.dir.img; G.gpu.dir_depth = G.dir_depth.img; G.gpu.weather = G.weather.img;
    }
    for (uint32_t f = 0; f < CL_FIF; f++) {
        VkQueryPoolCreateInfo qi; memset(&qi, 0, sizeof qi);
        qi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO; qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = CL_PASSES * 3u;
        if (vkCreateQueryPool(dev, &qi, NULL, &G.qp[f]) != VK_SUCCESS) G.qp[f] = VK_NULL_HANDLE;
    }
    G.gpu.common_layout = G.set_layout;
    G.gpu.common_pipe_layout = G.pipe_layout;
    G.p_noise = ps2_clouds_make_compute("cloud_noise.comp.spv", VK_NULL_HANDLE, NULL);
    if (sh_cfg()->on && cfg.far_on && G.shaft.img) { /* ACZ_VR_CLOUD_SHAFTS (failure: no shafts) */
        G.p_shafts = ps2_clouds_make_compute("cloud_shafts.comp.spv", VK_NULL_HANDLE, NULL);
        ps2_log("clouds: light shafts in the haze %s (ACZ_VR_CLOUD_SHAFTS %.2f: map %d^2, %d rows a frame in %d slices, %d "
                "samples a ray, phase g %.2f mix %.2f, haze scale height %.0f m, sunlit air %s)",
                G.p_shafts ? "on" : "unavailable", (double)SH.strength, SH.res, (SH.res + SH.slices - 1) / SH.slices,
                SH.slices, SH.steps, (double)SH.g, (double)SH.mix, (double)SH.H, SH.bright ? "brightens" : "unchanged");
    }
    G.p_weather = ps2_clouds_make_compute("cloud_weather.comp.spv", VK_NULL_HANDLE, NULL);
    G.p_test = ps2_clouds_make_compute("cloud_test.comp.spv", VK_NULL_HANDLE, NULL);
    G.comp_unroll = env_int("ACZ_VR_CLOUD_COMP_UNROLL", 1, 0, 1);
    cd_cfg();
    G.p_comp = make_composite();
    G.p_comp_pm = make_composite_v(1, G.comp_unroll);  /* (optional: none = always the full-screen triangle) */
    G.p_cdv[0][0] = G.p_comp; G.p_cdv[1][0] = G.p_comp_pm;
    for (int d = 3; d <= 4; d++) {        /* (perf6_clouds) the near composite at the other divisors in use */
        int u = CD.lv[CD.cfg].div == d;
        for (int i = 0; i < CD.nset; i++) if (CD.lv[CD.set_lv[i]].div == d) u = 1;
        if (!u) continue;
        G.p_cdv[0][d - 2] = make_composite_g(0, G.comp_unroll, d);
        if (G.p_comp_pm) G.p_cdv[1][d - 2] = make_composite_g(1, G.comp_unroll, d);
    }
    {   /* the other way, only for the measurements (PS2_PASS_AB_WHAT=compunroll, PS2_PERF_AB_FLIP=compunroll) */
        const char *w = getenv("PS2_PASS_AB_WHAT"), *f = getenv("PS2_PERF_AB_FLIP");
        if ((w && strstr(w, "compunroll")) || (f && strstr(f, "compunroll"))) {
            G.p_comp_x = make_composite_v(0, !G.comp_unroll);
            G.p_comp_pm_x = make_composite_v(1, !G.comp_unroll);
        }
        ps2_log("clouds: near composite loop %s (ACZ_VR_CLOUD_COMP_UNROLL)%s", G.comp_unroll ? "unrolled" : "as before",
                G.p_comp_x ? "; the other one made for an A/B" : "");
    }
    if (!G.p_noise || !G.p_weather || !G.p_test || !G.p_comp) goto fail;
    fx_cfg();
    FX.pipe = make_stamp(0);                              /* (ACZ_VR_CLOUD_FX; failure: no stamp) */
    FX.vpipe = FX.soft > 0.0f && G.gpu.multiview ? make_stamp(1) : VK_NULL_HANDLE;   /* (ACZ_VR_CLOUD_FX_SOFT) */
    FX.pipe_d[0] = FX.pipe; FX.vpipe_d[0] = FX.vpipe;
    for (int d = 3; d <= 4; d++) {        /* (perf6_clouds) the other divisors in use */
        int u = CD.lv[CD.cfg].div == d;
        for (int i = 0; i < CD.nset; i++) if (CD.lv[CD.set_lv[i]].div == d) u = 1;
        if (!u) continue;
        if (FX.pipe) FX.pipe_d[d - 2] = make_stamp_d(0, d);
        if (FX.vpipe) FX.vpipe_d[d - 2] = make_stamp_d(1, d);
    }
    ps2_log("clouds: effect sprites vs our clouds (ACZ_VR_CLOUD_FX %d): %s, transmittance limit %.2f", FX.mode,
            !FX.pipe ? "stamp pipeline unavailable" : FX.mode == 0 ? "off" : FX.mode == 2 ? "far and near composite before "
            "the effects, depth stamp" : "far composite before the effects, depth stamp", FX.lim);
    if (FX.mode && FX.pipe)
        ps2_log("clouds: effects fade by our cloud's transmittance in front of them, %.2f .. %.2f (ACZ_VR_CLOUD_FX_SOFT %.2f): "
                "%s", FX.lim - FX.soft, FX.lim + FX.soft, FX.soft, FX.vpipe ? "the veil; the depth stamp only below the fade"
                : FX.soft > 0.0f ? "veil unavailable (multiview only): the binary stamp" : "off: the binary stamp as before");
    if (ps2_clouds_shadow_init(&G.gpu) != 0) goto fail;   /* (always: the shown shadow map is bound in set 0) */
    ps2_clouds_optics_init(&G.gpu);                       /* (failure only turns the optics off) */
    ps2_clouds_wake_init(&G.gpu);                         /* the cloud wake's probe (ACZ_VR_CLOUD_WAKE) */
    G.ok = 1;
    if (ps2_clouds_weather_init(&G.gpu) != 0) ps2_log("clouds: S2 weather pass unavailable (procedural default)");
    if (cfg.far_on && ps2_clouds_far_init(&G.gpu) != 0) { ps2_log("clouds: far renderer unavailable"); cfg.far_on = 0; }
    if (cfg.near_on && ps2_clouds_near_init(&G.gpu) != 0) { ps2_log("clouds: near renderer unavailable"); cfg.near_on = 0; }
    far_avail = cfg.far_on; near_avail = cfg.near_on;
    ps2_log("clouds: pipeline ready (%s, far map %u^2 = %.0f MB, weather %u^2 / %.0f km, uniform slot %u B)",
            ii->multiview ? "multiview" : "two eye passes", G.gpu.dir_res,
            (double)G.gpu.dir_res * G.gpu.dir_res * 18.0 / 1048576.0, CL_WEATHER_RES, CL_WEATHER_M / 1000.0, G.ubo_slot);
    return 0;
fail:
    ps2_log("clouds: initialisation failed -- clouds off");
    G.ok = 0;
    cfg.near_on = cfg.far_on = 0;
    return -1;
}

void ps2_clouds_vk_shutdown(void) {
    if (!G.gpu.dev) return;
    VkDevice dev = G.gpu.dev;
    if (G.ok) {
        if (cfg.far_on) ps2_clouds_far_shutdown(&G.gpu);
        if (cfg.near_on) ps2_clouds_near_shutdown(&G.gpu);
    }
    ps2_clouds_shadow_shutdown(&G.gpu);
    ps2_clouds_optics_shutdown(&G.gpu);
    ps2_clouds_wake_shutdown(&G.gpu);
    if (G.p_comp_pm) { vkDestroyPipeline(G.gpu.dev, G.p_comp_pm, NULL); G.p_comp_pm = VK_NULL_HANDLE; }
    if (G.p_comp_x) { vkDestroyPipeline(G.gpu.dev, G.p_comp_x, NULL); G.p_comp_x = VK_NULL_HANDLE; }
    if (G.p_comp_pm_x) { vkDestroyPipeline(G.gpu.dev, G.p_comp_pm_x, NULL); G.p_comp_pm_x = VK_NULL_HANDLE; }
    VkPipeline ps[5] = { G.p_noise, G.p_weather, G.p_test, G.p_comp, G.p_shafts };
    for (int i = 0; i < 5; i++) if (ps[i]) vkDestroyPipeline(dev, ps[i], NULL);
    for (int d = 1; d < 3; d++) {         /* (perf6_clouds) */
        if (FX.pipe_d[d]) vkDestroyPipeline(dev, FX.pipe_d[d], NULL);
        if (FX.vpipe_d[d]) vkDestroyPipeline(dev, FX.vpipe_d[d], NULL);
        FX.pipe_d[d] = FX.vpipe_d[d] = VK_NULL_HANDLE;
    }
    for (int m = 0; m < 2; m++)
        for (int d = 1; d < 3; d++) {
            if (G.p_cdv[m][d]) vkDestroyPipeline(dev, G.p_cdv[m][d], NULL);
            G.p_cdv[m][d] = VK_NULL_HANDLE;
        }
    if (FX.pipe) { vkDestroyPipeline(dev, FX.pipe, NULL); FX.pipe = VK_NULL_HANDLE; }
    if (FX.vpipe) { vkDestroyPipeline(dev, FX.vpipe, NULL); FX.vpipe = VK_NULL_HANDLE; }
    cl_image *im[11] = { &G.dir, &G.dir_depth, &G.weather, &G.noise_base, &G.noise_detail, &G.near_c, &G.near_d,
                         &G.near_f, &G.near_p, &G.dir_prof, &G.shaft };
    for (int i = 0; i < 11; i++) free_img(im[i]);
    if (G.depth_view) vkDestroyImageView(dev, G.depth_view, NULL);
    for (int i = 0; i < 8; i++) if (G.grave[i]) vkDestroyImageView(dev, G.grave[i], NULL);
    for (int i = 0; i < 4; i++) if (dkeep[i].v) { vkDestroyImageView(dev, dkeep[i].v, NULL); dkeep[i].v = VK_NULL_HANDLE; dkeep[i].img = VK_NULL_HANDLE; }
    for (uint32_t f = 0; f < CL_FIF; f++) if (G.qp[f]) vkDestroyQueryPool(dev, G.qp[f], NULL);
    if (V.buf) { vkDestroyBuffer(dev, V.buf, NULL); vkFreeMemory(dev, V.mem, NULL); V.buf = VK_NULL_HANDLE; V.cap = 0; }
    if (D.buf) { vkDestroyBuffer(dev, D.buf, NULL); vkFreeMemory(dev, D.mem, NULL); D.buf = VK_NULL_HANDLE; }
    if (G.ubo) { vkDestroyBuffer(dev, G.ubo, NULL); vkFreeMemory(dev, G.ubo_mem, NULL); }
    if (G.pool) vkDestroyDescriptorPool(dev, G.pool, NULL);
    if (G.pipe_layout) vkDestroyPipelineLayout(dev, G.pipe_layout, NULL);
    if (G.set_layout) vkDestroyDescriptorSetLayout(dev, G.set_layout, NULL);
    VkSampler ss[4] = { G.s_rep, G.s_clamp, G.s_near, G.s_mip };
    for (int i = 0; i < 4; i++) if (ss[i]) vkDestroySampler(dev, ss[i], NULL);
    memset(&G, 0, sizeof G);
}

/* our view of a render target's depth is dropped with the image (ps2_video.c destroy_target_images, after a device
   wait: no frame in flight uses it any more) */
void ps2_clouds_image_gone(VkImage img) {
    if (!G.ok || !img) return;
    for (int i = 0; i < 4; i++)                  /* (a kept view of another depth image: ACZ_REC_CINE's) */
        if (dkeep[i].img == img) { vkDestroyImageView(G.gpu.dev, dkeep[i].v, NULL); dkeep[i].v = VK_NULL_HANDLE; dkeep[i].img = VK_NULL_HANDLE; }
    if (img != G.depth_src) return;
    if (G.depth_view) vkDestroyImageView(G.gpu.dev, G.depth_view, NULL);
    G.depth_view = VK_NULL_HANDLE; G.depth_src = VK_NULL_HANDLE;
}
static int depth_view_for(VkImage img, uint32_t layers) {
    if (img == G.depth_src && G.depth_view) return 0;
    /* ACZ_REC_CINE (ps2_video.c cine_*): the recording's view composites over its own depth image in the same frame, so
       two depth images alternate every frame: the other one's view is kept (up to 4), not retired and made again */
    for (int i = 0; i < 4; i++)
        if (dkeep[i].img == img && dkeep[i].v) {
            VkImage oi = G.depth_src; VkImageView ov = G.depth_view;
            G.depth_src = img; G.depth_view = dkeep[i].v;
            dkeep[i].img = oi; dkeep[i].v = ov;
            if (!ov) dkeep[i].img = VK_NULL_HANDLE;
            return 0;
        }
    if (G.depth_view && keep_views) {            /* (the current one kept: a free place, else the oldest retires) */
        int k = -1;
        for (int i = 0; i < 4 && k < 0; i++) if (!dkeep[i].v) k = i;
        if (k < 0) {
            k = (int)(dkeep_next++ & 3u);
            int g = 0;
            for (int i = 0; i < 8; i++) if (!G.grave[i]) { g = i; break; }
            if (G.grave[g]) vkDestroyImageView(G.gpu.dev, G.grave[g], NULL);
            G.grave[g] = dkeep[k].v; G.grave_age[g] = G.frame;
        }
        dkeep[k].img = G.depth_src; dkeep[k].v = G.depth_view;
        G.depth_view = VK_NULL_HANDLE; G.depth_src = VK_NULL_HANDLE;
    }
    if (G.depth_view) {          /* a different depth image while ours is alive: retire the view a few frames later */
        int k = 0;
        for (int i = 0; i < 8; i++) if (!G.grave[i]) { k = i; break; }
        if (G.grave[k]) vkDestroyImageView(G.gpu.dev, G.grave[k], NULL);
        G.grave[k] = G.depth_view; G.grave_age[k] = G.frame;
    }
    VkImageViewCreateInfo vi; memset(&vi, 0, sizeof vi);
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; vi.image = img; vi.format = VK_FORMAT_D32_SFLOAT;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    vi.subresourceRange.levelCount = 1; vi.subresourceRange.layerCount = layers;
    G.depth_view = VK_NULL_HANDLE; G.depth_src = VK_NULL_HANDLE;
    if (vkCreateImageView(G.gpu.dev, &vi, NULL, &G.depth_view) != VK_SUCCESS) { G.depth_view = VK_NULL_HANDLE; return -1; }
    G.depth_src = img;
    return 0;
}

static void img_barrier(VkImageMemoryBarrier2 *b, VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                        VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da) {
    memset(b, 0, sizeof *b);
    b->sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b->srcStageMask = ss; b->srcAccessMask = sa; b->dstStageMask = ds; b->dstAccessMask = da;
    b->oldLayout = from; b->newLayout = to;
    b->srcQueueFamilyIndex = b->dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b->image = img;
    b->subresourceRange.aspectMask = aspect; b->subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
    b->subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
}
static void flush(VkCommandBuffer cmd, const VkImageMemoryBarrier2 *ib, uint32_t ni, const VkMemoryBarrier2 *mb, uint32_t nm) {
    VkDependencyInfo di; memset(&di, 0, sizeof di);
    di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    di.imageMemoryBarrierCount = ni; di.pImageMemoryBarriers = ib;
    di.memoryBarrierCount = nm; di.pMemoryBarriers = mb;
    vkCmdPipelineBarrier2(cmd, &di);
}
/* PS2_PERF_CLOUD_BARRIERS (perf4_passes 2026-11-03; default 1, 0 = as before): barriers that follow each other with no
   command between them go to the GPU in one call (one dependency info holding both): at a split point the depth's
   transition and the memory barrier before our hooks, and after the composite its colour barrier and the depth's
   transition back; the same dependencies, 2 calls fewer a split point */
static int cl_bar_merge(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("PS2_PERF_CLOUD_BARRIERS"); v = !(e && *e == '0'); }
    return v;
}
static void mem_barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da) {
    VkMemoryBarrier2 m; memset(&m, 0, sizeof m);
    m.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    m.srcStageMask = ss; m.srcAccessMask = sa; m.dstStageMask = ds; m.dstAccessMask = da;
    flush(cmd, NULL, 0, &m, 1);
}
#define CS VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
#define FS VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
#define RW (VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT)

/* the near buffer follows the target size (rare: a device wait, then new images); CLOUD DETAIL: made for the smallest
   near divisor in use (2: half the target per axis, as before) */
static int near_size(uint32_t tw, uint32_t th) {
    cd_cfg();
    uint32_t dv = (uint32_t)(CD.min_div >= 2 ? CD.min_div : 2);
    uint32_t w = (tw + dv - 1u) / dv, h = (th + dv - 1u) / dv;
    if (w == G.near_w && h == G.near_h && G.near_c.img) return 0;
    vkDeviceWaitIdle(G.gpu.dev);
    free_img(&G.near_c); free_img(&G.near_d); free_img(&G.near_f); free_img(&G.near_p);
    VkImageUsageFlags st = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                         | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (make_img(&G.near_c, VK_IMAGE_TYPE_2D, w, h, 1, CL_NEAR_LAYERS, VK_FORMAT_R16G16B16A16_SFLOAT, st)
        || make_img(&G.near_d, VK_IMAGE_TYPE_2D, w, h, 1, CL_NEAR_LAYERS, VK_FORMAT_R16_SFLOAT, st)) {
        free_img(&G.near_c); free_img(&G.near_d); G.near_w = G.near_h = 0;
        return -1;
    }
    /* ACZ_VR_CLOUD_NEAR_REUSE: the history of the in-between frames (failure: no reuse, as before) */
    if (ps2_clouds_near_reuse_on()
        && (make_img(&G.near_f, VK_IMAGE_TYPE_2D, w, h, 1, CL_NEAR_LAYERS, VK_FORMAT_R16G16B16A16_SFLOAT, st)
            || make_img(&G.near_p, VK_IMAGE_TYPE_2D, w, h, 1, CL_NEAR_LAYERS, VK_FORMAT_R16G16B16A16_SFLOAT, st))) {
        free_img(&G.near_f); free_img(&G.near_p);
        ps2_log("clouds: no images for the in-between near reuse (ACZ_VR_CLOUD_NEAR_REUSE off)");
    }
    G.near_w = w; G.near_h = h;
    G.gpu.near_epoch++;
    G.gpu.near_img = G.near_c.img; G.gpu.near_depth = G.near_d.img; G.gpu.near_w = w; G.gpu.near_h = h;
    G.ready &= ~2;                       /* to be cleared and put into GENERAL */
    ps2_log("clouds: near buffer %ux%u x %u layers", w, h, CL_NEAR_LAYERS);
    return 0;
}
/* PS2_PREWARM_EXTRA (ov_hitch, 2026-10-25; ps2_video.c prewarm_targets, while the mission loads): the near buffer for
   the flight's target size made before the first stereo frame instead of in it (near_size's device wait and two
   images: ~7 ms of the first stereo frame in M5). 1 made, 0 already there, < 0 not used / failed. */
int ps2_clouds_prewarm(uint32_t tw, uint32_t th) {
    if (!G.ok || !G.gpu.dev || !ps2_clouds_cfg()->near_on) return -1;
    VkImage before = G.near_c.img;
    if (near_size(tw, th) != 0) return -2;
    return G.near_c.img != before ? 1 : 0;
}

/* Flash lights (wish #83; ps2_video.c flash_frame sets them once per drawn frame on the renderer thread, before the
   frame's cloud passes): up to 4, each 8 floats -- world position (m: the game's world / units_per_m), the distance
   where the light has halved (m), rgb (slot-0 units at the light, the pulse included), 0. fill_ubo turns them into
   the pass's cloud space (CloudFrame flash[]: [0].x = count; light i = [1 + 2i] position + radius, [2 + 2i] rgb). */
static float cl_flash[4][8];
static int cl_flash_n;
void ps2_clouds_flash_set(const float *lights, int n) {
    if (n < 0) n = 0;
    if (n > 4) n = 4;
    if (n) memcpy(cl_flash, lights, (size_t)n * 8u * sizeof(float));
    cl_flash_n = n;
}
int ps2_clouds_flash_count(void) { return cl_flash_n; }   /* (ps2_clouds_near.c: no in-between reuse while lights pulse) */

static void fill_ubo(cl_frame_ubo *u, const ps2_cloud_view *v, const ps2_cloud_env *e) {
    memset(u, 0, sizeof *u);
    for (int i = 0; i < 3; i++) {
        u->cam_x[i] = v->cam_x[i]; u->cam_y[i] = v->cam_y[i]; u->cam_z[i] = v->cam_z[i];
        u->eye_m[0][i] = v->eye_m[0][i]; u->eye_m[1][i] = v->eye_m[1][i];
        u->sun[i] = e->sun_dir[i]; u->lit[i] = e->lit[i]; u->shadow[i] = e->shadow[i]; u->fog[i] = e->fog[i];
        u->sky_top[i] = e->sky_top[i]; u->sky_hor[i] = e->sky_hor[i];
        u->sky_below[i] = e->sky_below[i]; u->incloud[i] = e->incloud[i];
    }
    memcpy(u->user[0], e->shape, sizeof u->user[0]);
    memcpy(u->user[1], e->shade, sizeof u->user[1]);
    memcpy(u->user[2], e->near_art, sizeof u->user[2]);
    memcpy(u->user[3], e->art_k, sizeof u->user[3]);
    memcpy(u->vis, v->vis, sizeof u->vis);
    u->wind[0] = e->wind_mps[0]; u->wind[1] = e->wind_mps[1]; u->wind[2] = e->drift_m[0]; u->wind[3] = e->drift_m[1];
    {   double cx = v->cloud_origin_m[0], cz = v->cloud_origin_m[2];
        double hx = floor(cx / 4096.0) * 4096.0, hz = floor(cz / 4096.0) * 4096.0;
        u->origin_split[0] = (float)hx; u->origin_split[1] = (float)hz;
        u->origin_split[2] = (float)(cx - hx); u->origin_split[3] = (float)(cz - hz);
        double mx = fmod(cx, 4000.0), mz = fmod(cz, 4000.0);
        u->origin_4k[0] = (float)(mx < 0.0 ? mx + 4000.0 : mx); u->origin_4k[1] = (float)(mz < 0.0 ? mz + 4000.0 : mz);
        u->origin_4k[2] = (float)(e->list_seq & 0xFFFFFFu); u->origin_4k[3] = G.wx_module ? 1.0f : 0.0f;
    }
    u->shadow[3] = e->shadow_a;
    memcpy(u->proj, v->proj, sizeof u->proj);
    memcpy(u->img, v->img, sizeof u->img);
    u->depth[0] = v->depth_a; u->depth[1] = v->depth_c; u->depth[2] = v->depth_mh; u->depth[3] = v->depth_zmax;
    u->misc[0] = v->units_per_m; u->misc[1] = (float)fmod(v->time_s, 4096.0); u->misc[2] = v->alpha; u->misc[3] = (float)G.frame;
    double ox = fmod(v->cloud_origin_m[0], CL_ORIGIN_WRAP_M), oz = fmod(v->cloud_origin_m[2], CL_ORIGIN_WRAP_M);
    if (ox < 0.0) ox += CL_ORIGIN_WRAP_M;
    if (oz < 0.0) oz += CL_ORIGIN_WRAP_M;
    u->origin[0] = (float)ox; u->origin[1] = (float)v->cloud_origin_m[1]; u->origin[2] = (float)oz;
    u->origin[3] = (float)v->rebase;
    u->band[0] = e->base_m; u->band[1] = e->top_m; u->band[2] = e->coverage; u->band[3] = e->type;
    u->env[0] = e->transp; u->env[1] = e->precip; u->env[2] = e->in_cloud; u->env[3] = (float)e->valid;
    u->fogr[0] = e->fog_near_m; u->fogr[1] = e->fog_far_m; u->fogr[2] = (float)e->ncloud; u->fogr[3] = (float)(e->seq & 0xFFFFFFu);
    u->cfg[0] = (float)cfg.far_steps; u->cfg[1] = (float)cfg.near_steps; u->cfg[2] = cfg.near_range_m; u->cfg[3] = (float)cfg.far_slice;
    u->sizes[0] = (float)(cfg.far_on && !(cfg.test & 1) ? ps2_clouds_far_map_size(e, G.gpu.dir_res) : G.gpu.dir_res);   /* S2 */
    u->sizes[1] = (float)(G.gpu.near_lw ? G.gpu.near_lw : G.near_w); u->sizes[2] = (float)(G.gpu.near_lh ? G.gpu.near_lh : G.near_h);
    u->sizes[3] = (float)v->target_w;                     /* (CLOUD DETAIL: the part of the near images this pass uses) */
    u->sizes2[0] = (float)v->target_h; u->sizes2[1] = (float)v->inbetween; u->sizes2[2] = G.gpu.multiview ? 2.0f : 1.0f;
    u->sizes2[3] = v->half_ipd_m;
    double wu = fmod(v->cloud_origin_m[0] / CL_WEATHER_M, 1.0), wv = fmod(v->cloud_origin_m[2] / CL_WEATHER_M, 1.0);
    u->wx[0] = (float)(wu < 0.0 ? wu + 1.0 : wu); u->wx[1] = (float)(wv < 0.0 ? wv + 1.0 : wv);
    u->wx[2] = 1.0f / CL_WEATHER_M; u->wx[3] = (float)cfg.test + (wx_sqrt ? 16.0f : 0.0f);   /* (wx.w >= 16: sqrt density) */
    ps2_clouds_shadow_ubo((float *)u->shd, v, e);
    ps2_clouds_optics_ubo((float *)u->opt);
    /* light shafts (ACZ_VR_CLOUD_SHAFTS): the map's distances and whether the composite uses it */
    memcpy(u->shaft[0], SH.d, sizeof u->shaft[0]);
    {   extern float ps2_vk_enclosure_closed(void);   /* (tunnel_light, bug #267: none under a roof, as the sun's air) */
        u->shaft[1][0] = sh_use_now() && ps2_vk_enclosure_closed() < 0.5f ? 1.0f : 0.0f; }
    u->shaft[1][1] = SH.bright ? 1.0f : 0.0f;
    /* flash lights (wish #83, ps2_video.c flash_frame): world metres -> this view's cloud space */
    u->flash[0][0] = (float)cl_flash_n;
    for (int i = 0; i < cl_flash_n; i++) {
        const float *f = cl_flash[i];
        for (int j = 0; j < 3; j++) u->flash[1 + 2 * i][j] = (float)((double)f[j] - v->origin_m[j]);
        u->flash[1 + 2 * i][3] = f[3];
        for (int j = 0; j < 4; j++) u->flash[2 + 2 * i][j] = f[4 + j];
    }
}

static void write_set(VkDescriptorSet set, VkDeviceSize ubo_off) {
    VkDescriptorBufferInfo bi; bi.buffer = G.ubo; bi.offset = ubo_off; bi.range = sizeof(cl_frame_ubo);
    VkDescriptorImageInfo im[CL_B_COUNT]; memset(im, 0, sizeof im);
    VkWriteDescriptorSet w[CL_B_COUNT]; memset(w, 0, sizeof w);
    struct { VkImageView v; VkSampler s; VkImageLayout l; } src[CL_B_COUNT] = {
        [CL_B_NOISE_BASE] = { G.noise_base.view, G.s_mip, VK_IMAGE_LAYOUT_GENERAL },
        [CL_B_NOISE_DETAIL] = { G.noise_detail.view, G.s_mip, VK_IMAGE_LAYOUT_GENERAL },
        [CL_B_WEATHER] = { G.weather.view, G.s_rep, VK_IMAGE_LAYOUT_GENERAL },
        [CL_B_SCENE_DEPTH] = { G.depth_view, G.s_near, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
        [CL_B_DIR_IMG] = { G.dir.view, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL },
        [CL_B_DIR_DEPTH_IMG] = { G.dir_depth.view, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL },
        [CL_B_NEAR_IMG] = { G.near_c.view, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL },
        [CL_B_NEAR_DEPTH_IMG] = { G.near_d.view, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL },
        [CL_B_DIR] = { G.dir.view, G.s_clamp, VK_IMAGE_LAYOUT_GENERAL },
        [CL_B_DIR_DEPTH] = { G.dir_depth.view, G.s_clamp, VK_IMAGE_LAYOUT_GENERAL },
        [CL_B_NEAR] = { G.near_c.view, G.s_near, VK_IMAGE_LAYOUT_GENERAL },
        [CL_B_NEAR_DEPTH] = { G.near_d.view, G.s_near, VK_IMAGE_LAYOUT_GENERAL },
        /* ACZ_VR_CLOUD_NEAR_REUSE: the history (without its images: the near pair, never read or written then) */
        [CL_B_NEAR_FAR] = { G.near_f.img ? G.near_f.view : G.near_c.view, G.s_near, VK_IMAGE_LAYOUT_GENERAL },
        [CL_B_NEAR_PAR] = { G.near_p.img ? G.near_p.view : G.near_c.view, G.s_near, VK_IMAGE_LAYOUT_GENERAL },
        [CL_B_NEAR_FAR_IMG] = { G.near_f.img ? G.near_f.view : G.near_c.view, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL },
        [CL_B_NEAR_PAR_IMG] = { G.near_p.img ? G.near_p.view : G.near_c.view, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL },
        [CL_B_WEATHER_IMG] = { G.weather.view, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL },
        [CL_B_NOISE_BASE_IMG] = { G.noise_base.sview, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL },
        [CL_B_NOISE_DETAIL_IMG] = { G.noise_detail.sview, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL },
        [CL_B_SHADOW] = { ps2_clouds_shadow_view(), G.s_rep, VK_IMAGE_LAYOUT_GENERAL },
        [CL_B_DIR_PROF] = { G.dir_prof.view, G.s_near, VK_IMAGE_LAYOUT_GENERAL },
        [CL_B_DIR_PROF_IMG] = { G.dir_prof.view, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL },
        [CL_B_SHAFT] = { G.shaft.img ? G.shaft.view : G.dir_prof.view, G.s_clamp, VK_IMAGE_LAYOUT_GENERAL },
        [CL_B_SHAFT_IMG] = { G.shaft.img ? G.shaft.view : G.dir_prof.view, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL },
    };
    uint32_t n = 0;
    w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n].dstSet = set; w[n].dstBinding = CL_B_FRAME;
    w[n].descriptorCount = 1; w[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[n].pBufferInfo = &bi; n++;
    for (uint32_t i = 1; i < CL_B_COUNT; i++) {
        if (!src[i].v) continue;           /* (the near buffer before the first near pass) */
        im[i].imageView = src[i].v; im[i].sampler = src[i].s; im[i].imageLayout = src[i].l;
        w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n].dstSet = set; w[n].dstBinding = i;
        w[n].descriptorCount = 1;
        w[n].descriptorType = src[i].s ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[n].pImageInfo = &im[i]; n++;
    }
    vkUpdateDescriptorSets(G.gpu.dev, n, w, 0, NULL);
}

static void push(VkCommandBuffer cmd, VkShaderStageFlags st, int32_t a, int32_t b, int32_t c, int32_t d,
                 int32_t e, int32_t f, int32_t g, int32_t h) {
    int32_t pc[16]; memset(pc, 0, sizeof pc);
    pc[0] = a; pc[1] = b; pc[2] = c; pc[3] = d; pc[4] = e; pc[5] = f; pc[6] = g; pc[7] = h;
    vkCmdPushConstants(cmd, G.pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, CL_PUSH_BYTES, pc);
    (void)st;
}

/* the mip chain of a cubic 3D noise texture (GENERAL layout), level by level with linear blits */
static void mip_chain(VkCommandBuffer cmd, const cl_image *im, uint32_t n) {
    for (uint32_t l = 1; l < im->mips; l++) {
        int32_t s = (int32_t)(n >> (l - 1u)), d = (int32_t)(n >> l);
        if (d < 1) d = 1;
        VkImageBlit b; memset(&b, 0, sizeof b);
        b.srcSubresource.aspectMask = b.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.srcSubresource.mipLevel = l - 1u; b.dstSubresource.mipLevel = l;
        b.srcSubresource.layerCount = b.dstSubresource.layerCount = 1;
        b.srcOffsets[1].x = b.srcOffsets[1].y = b.srcOffsets[1].z = s;
        b.dstOffsets[1].x = b.dstOffsets[1].y = b.dstOffsets[1].z = d;
        vkCmdBlitImage(cmd, im->img, VK_IMAGE_LAYOUT_GENERAL, im->img, VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_LINEAR);
        mem_barrier(cmd, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    }
}

/* first use (and a new near buffer): GENERAL layouts, noise, clears */
static void init_images(VkCommandBuffer cmd) {
    VkImageMemoryBarrier2 b[11]; uint32_t n = 0;
    VkClearColorValue clr; memset(&clr, 0, sizeof clr); clr.float32[3] = 1.0f;
    VkClearColorValue clrd; memset(&clrd, 0, sizeof clrd); clrd.float32[0] = CL_DEPTH_NONE;
    VkImageSubresourceRange rr; memset(&rr, 0, sizeof rr);
    rr.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; rr.levelCount = 1; rr.layerCount = VK_REMAINING_ARRAY_LAYERS;
    if (!(G.ready & 1)) {
        cl_image *im[7] = { &G.dir, &G.dir_depth, &G.weather, &G.noise_base, &G.noise_detail, &G.dir_prof, &G.shaft };
        for (int i = 0; i < 7; i++)
            if (im[i]->img) img_barrier(&b[n++], im[i]->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_CLEAR_BIT | CS, VK_ACCESS_2_TRANSFER_WRITE_BIT | RW);
    }
    if (!(G.ready & 2) && G.near_c.img) {
        img_barrier(&b[n++], G.near_c.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_CLEAR_BIT | CS, VK_ACCESS_2_TRANSFER_WRITE_BIT | RW);
        img_barrier(&b[n++], G.near_d.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_CLEAR_BIT | CS, VK_ACCESS_2_TRANSFER_WRITE_BIT | RW);
        if (G.near_f.img) {                                               /* (ACZ_VR_CLOUD_NEAR_REUSE) */
            img_barrier(&b[n++], G.near_f.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_CLEAR_BIT | CS, VK_ACCESS_2_TRANSFER_WRITE_BIT | RW);
            img_barrier(&b[n++], G.near_p.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_CLEAR_BIT | CS, VK_ACCESS_2_TRANSFER_WRITE_BIT | RW);
        }
    }
    if (!n) return;
    flush(cmd, b, n, NULL, 0);
    if (!(G.ready & 1)) {
        vkCmdClearColorImage(cmd, G.dir.img, VK_IMAGE_LAYOUT_GENERAL, &clr, 1, &rr);
        vkCmdClearColorImage(cmd, G.dir_depth.img, VK_IMAGE_LAYOUT_GENERAL, &clrd, 1, &rr);
        {   VkClearColorValue clrp; clrp.float32[0] = clrp.float32[1] = clrp.float32[2] = clrp.float32[3] = CL_DEPTH_NONE;
            vkCmdClearColorImage(cmd, G.dir_prof.img, VK_IMAGE_LAYOUT_GENERAL, &clrp, 1, &rr);
            clrp.float32[0] = clrp.float32[1] = clrp.float32[2] = clrp.float32[3] = 0.0f;
            if (G.shaft.img) vkCmdClearColorImage(cmd, G.shaft.img, VK_IMAGE_LAYOUT_GENERAL, &clrp, 1, &rr); }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, G.p_noise);
        ps2_clouds_bind_common(cmd, G.pipe_layout);
        uint32_t nb = CL_NOISE_BASE_RES, nd = CL_NOISE_DETAIL_RES;
        push(cmd, 0, (int32_t)nb, (int32_t)nb, (int32_t)nb, 0, 0, 0, 0, 0);
        vkCmdDispatch(cmd, nb / 4u, nb / 4u, nb / 4u);
        push(cmd, 0, (int32_t)nd, (int32_t)nd, (int32_t)nd, 0, 1, 0, 0, 0);
        vkCmdDispatch(cmd, nd / 4u, nd / 4u, nd / 4u);
        mem_barrier(cmd, CS, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
        mip_chain(cmd, &G.noise_base, CL_NOISE_BASE_RES);
        mip_chain(cmd, &G.noise_detail, CL_NOISE_DETAIL_RES);
        G.weather_key[0] = -1.0f;            /* the weather map comes next */
    }
    if (!(G.ready & 2) && G.near_c.img) {
        vkCmdClearColorImage(cmd, G.near_c.img, VK_IMAGE_LAYOUT_GENERAL, &clr, 1, &rr);
        vkCmdClearColorImage(cmd, G.near_d.img, VK_IMAGE_LAYOUT_GENERAL, &clrd, 1, &rr);
        if (G.near_f.img) {
            VkClearColorValue clrp; memset(&clrp, 0, sizeof clrp);
            vkCmdClearColorImage(cmd, G.near_f.img, VK_IMAGE_LAYOUT_GENERAL, &clr, 1, &rr);
            vkCmdClearColorImage(cmd, G.near_p.img, VK_IMAGE_LAYOUT_GENERAL, &clrp, 1, &rr);
        }
        G.ready |= 2;
    }
    G.ready |= 1;
    mem_barrier(cmd, VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT | CS, VK_ACCESS_2_TRANSFER_WRITE_BIT | RW,
                CS | FS, RW);
}

/* the far map for the cockpit's mirrors (ps2_clouds.h) */
int ps2_clouds_dir_hook(VkImageView *view, VkSampler *samp, float *uv_scale) {
    if (!G.ok || !(G.ready & 1) || !cfg.far_on || !G.dir.view || !G.s_clamp) return 0;
    if (view) *view = G.dir.view;
    if (samp) *samp = G.s_clamp;
    if (uv_scale) {
        uint32_t n = G.gpu.dir_res, used = !(cfg.test & 1) ? ps2_clouds_far_map_size(&G.env, n) : n;
        *uv_scale = n ? (float)used / (float)n : 1.0f;
    }
    return 1;
}

int ps2_clouds_dir_depth_hook(VkImageView *view) {
    if (!G.ok || !(G.ready & 1) || !cfg.far_on || !G.dir_depth.view) return 0;
    if (view) *view = G.dir_depth.view;
    return 1;
}

/* the GPU ms of all cloud passes of frame slot fi's last command buffer (its fence has passed; read again later by
   read_timestamps): for the renderer's per-frame trace (PS2_GPU_TRACE) */
double ps2_clouds_gpu_ms_peek(int fi) {
    if (fi < 0 || fi >= (int)CL_FIF || !G.qp[fi] || !G.qmask[fi]) return 0.0;
    double ms = 0.0;
    for (uint32_t k = 0; k < CL_PASSES; k++) {
        if (!(G.qmask[fi] & (1u << k))) continue;
        uint64_t t[3];
        if (vkGetQueryPoolResults(G.gpu.dev, G.qp[fi], k * 3u, 3, sizeof t, t, sizeof t[0], VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
            continue;
        ms += (double)(t[2] - t[0]) * G.ts_ns * 1e-6;
    }
    return ms;
}
/* timestamps of an earlier command buffer of this frame slot (its fence has passed) */
static void read_timestamps(int fi) {
    if (!G.qp[fi] || !G.qmask[fi]) return;
    for (uint32_t k = 0; k < CL_PASSES; k++) {
        if (!(G.qmask[fi] & (1u << k))) continue;
        uint64_t t[3];
        if (vkGetQueryPoolResults(G.gpu.dev, G.qp[fi], k * 3u, 3, sizeof t, t, sizeof t[0], VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
            continue;
        int kind = (int)G.qkind[fi][k];
        G.gpu_ms[kind][0] += (double)(t[1] - t[0]) * G.ts_ns * 1e-6;
        G.gpu_ms[kind][1] += (double)(t[2] - t[1]) * G.ts_ns * 1e-6;
        G.gpu_n[kind]++;
        if (kind < 2) { int off = G.qsplit[fi][k] != 0; G.split_ms[kind][off] += (double)(t[2] - t[1]) * G.ts_ns * 1e-6; G.split_n[kind][off]++; }
    }
    G.qmask[fi] = 0;
}

/* ACZ_VR_CLOUD_DUMP=<field> (debug): at the first cloud frame from that GS field on, the far map (after the far pass)
   and the near buffers (after the near pass) are copied out; once the frame's fence has passed they are written to
   out/clouds_<field>_*: dir.ppm (premultiplied rgb over black), dirT.pgm (transmittance), dirZ.pfm (mean distance,
   m), weather.ppm (coverage, local base, local top as rgb) + weatherA.pgm (density) as the frame used them,
   near{L,R}.ppm / nearT{L,R}.pgm / nearZ{L,R}.pfm, and env.txt (C1 + C2 as used). */
static float half_to_float(uint16_t h) {
    uint32_t s = (uint32_t)(h >> 15) << 31, e = (h >> 10) & 31u, m = h & 1023u, f;
    if (e == 0u) { if (!m) f = s; else { e = 113u; while (!(m & 1024u)) { m <<= 1; e--; } m &= 1023u; f = s | (e << 23) | (m << 13); } }
    else if (e == 31u) f = s | 0x7F800000u | (m << 13);
    else f = s | ((e + 112u) << 23) | (m << 13);
    float r; memcpy(&r, &f, sizeof r); return r;
}
static void dump_rgba16(const char *base, const char *tag, const uint16_t *px, uint32_t w, uint32_t h) {
    char fn[512];
    snprintf(fn, sizeof fn, "%s_%s.ppm", base, tag);
    FILE *f = fopen(fn, "wb");
    if (f) {
        fprintf(f, "P6\n%u %u\n255\n", w, h);
        for (size_t i = 0; i < (size_t)w * h; i++) {
            uint8_t c[3];
            for (int k = 0; k < 3; k++) { float v = half_to_float(px[i * 4u + (size_t)k]); c[k] = (uint8_t)(v <= 0.0f ? 0 : v >= 1.0f ? 255 : v * 255.0f + 0.5f); }
            fwrite(c, 1, 3, f);
        }
        fclose(f);
    }
    snprintf(fn, sizeof fn, "%s_%sT.pgm", base, tag);
    f = fopen(fn, "wb");
    if (f) {
        fprintf(f, "P5\n%u %u\n255\n", w, h);
        for (size_t i = 0; i < (size_t)w * h; i++) {
            float v = half_to_float(px[i * 4u + 3u]);
            uint8_t c = (uint8_t)(v <= 0.0f ? 0 : v >= 1.0f ? 255 : v * 255.0f + 0.5f);
            fwrite(&c, 1, 1, f);
        }
        fclose(f);
    }
}
static void dump_r16(const char *base, const char *tag, const uint16_t *px, uint32_t w, uint32_t h) {
    char fn[512];
    snprintf(fn, sizeof fn, "%s_%s.pfm", base, tag);
    FILE *f = fopen(fn, "wb");
    if (!f) return;
    fprintf(f, "Pf\n%u %u\n-1.0\n", w, h);                 /* little-endian, rows bottom to top */
    for (uint32_t y = h; y-- > 0;)
        for (uint32_t x = 0; x < w; x++) { float v = half_to_float(px[(size_t)y * w + x]); fwrite(&v, 4, 1, f); }
    fclose(f);
}
static void dump_write(void) {
    char base[256];
    snprintf(base, sizeof base, "out/clouds_%06u", D.field);
    const uint8_t *b = D.ptr;
    if (D.have & 1u) {
        dump_rgba16(base, "dir", (const uint16_t *)(b + D.off_dir), D.n, D.n);
        dump_r16(base, "dirZ", (const uint16_t *)(b + D.off_dirz), D.n, D.n);
        char fn[300];                       /* weather: RGB = coverage, local base, local top; A = density */
        snprintf(fn, sizeof fn, "%s_weather.ppm", base);
        FILE *f = fopen(fn, "wb");
        if (f) {
            fprintf(f, "P6\n%u %u\n255\n", CL_WEATHER_RES, CL_WEATHER_RES);
            for (size_t i = 0; i < (size_t)CL_WEATHER_RES * CL_WEATHER_RES; i++) fwrite(b + D.off_wx + i * 4u, 1, 3, f);
            fclose(f);
        }
        snprintf(fn, sizeof fn, "%s_weatherA.pgm", base);
        f = fopen(fn, "wb");
        if (f) {
            fprintf(f, "P5\n%u %u\n255\n", CL_WEATHER_RES, CL_WEATHER_RES);
            for (size_t i = 0; i < (size_t)CL_WEATHER_RES * CL_WEATHER_RES; i++) fwrite(b + D.off_wx + i * 4u + 3u, 1, 1, f);
            fclose(f);
        }
    }
    if (D.have & 2u)
        for (uint32_t e = 0; e < CL_NEAR_LAYERS; e++) {
            char tag[16];
            snprintf(tag, sizeof tag, "near%c", e ? 'R' : 'L');
            dump_rgba16(base, tag, (const uint16_t *)(b + D.off_near) + (size_t)e * D.nw * D.nh * 4u, D.nw, D.nh);
            snprintf(tag, sizeof tag, "nearZ%c", e ? 'R' : 'L');
            dump_r16(base, tag, (const uint16_t *)(b + D.off_nearz) + (size_t)e * D.nw * D.nh, D.nw, D.nh);
        }
    ps2_clouds_shadow_dump_write(base);
    char fn[300];
    snprintf(fn, sizeof fn, "%s_env.txt", base);
    FILE *f = fopen(fn, "w");
    if (f) {
        const ps2_cloud_env *e = &D.env; const ps2_cloud_view *v = &D.view;
        fprintf(f, "C1 seq %u valid %#x\nband %.1f %.1f m coverage %.3f (unmasked %.3f) type %.3f transp %.3f precip %.3f "
                "in_cloud %.3f\nsun %.4f %.4f %.4f\nlit %.3f %.3f %.3f shadow %.3f %.3f %.3f shadow_a %.3f\nfog %.3f %.3f %.3f "
                "sky_top %.3f %.3f %.3f sky_hor %.3f %.3f %.3f sky_below %.3f %.3f %.3f incloud %.3f %.3f %.3f\n"
                "fog %.1f .. %.1f m wind %.2f %.2f m/s drift %.1f %.1f m\nshape %g %g %g %g shade %g %g %g %g near_art %g %g %g %g "
                "art_k %g %g %g %g\ncloud list: origin %.1f %.1f %.1f period %.1f m, %u clouds, list_seq %u\n",
                e->seq, e->valid, e->base_m, e->top_m, e->coverage, e->coverage_unmasked, e->type, e->transp, e->precip,
                e->in_cloud, e->sun_dir[0], e->sun_dir[1], e->sun_dir[2], e->lit[0], e->lit[1], e->lit[2], e->shadow[0],
                e->shadow[1], e->shadow[2], e->shadow_a, e->fog[0], e->fog[1], e->fog[2], e->sky_top[0], e->sky_top[1],
                e->sky_top[2], e->sky_hor[0], e->sky_hor[1], e->sky_hor[2], e->sky_below[0], e->sky_below[1], e->sky_below[2],
                e->incloud[0], e->incloud[1], e->incloud[2], e->fog_near_m, e->fog_far_m, e->wind_mps[0], e->wind_mps[1],
                e->drift_m[0], e->drift_m[1], e->shape[0], e->shape[1], e->shape[2], e->shape[3], e->shade[0], e->shade[1],
                e->shade[2], e->shade[3], e->near_art[0], e->near_art[1], e->near_art[2], e->near_art[3], e->art_k[0],
                e->art_k[1], e->art_k[2], e->art_k[3], e->origin_m[0], e->origin_m[1], e->origin_m[2], e->list_period_m,
                e->ncloud, e->list_seq);
        for (uint32_t i = 0; i < e->ncloud && i < CL_MAX_CLOUDS; i++)
            fprintf(f, "  cloud %3u pos %.1f %.1f %.1f r %.1f opacity %.3f variant %u\n", i, e->cloud[i].pos_m[0],
                    e->cloud[i].pos_m[1], e->cloud[i].pos_m[2], e->cloud[i].radius_m, e->cloud[i].opacity, e->cloud[i].variant);
        fprintf(f, "C2 seq %u in-between %u alpha %.3f\norigin %.3f %.3f %.3f m cloud frame %.3f %.3f %.3f rebase %u\n"
                "cam_x %.5f %.5f %.5f cam_y %.5f %.5f %.5f cam_z %.5f %.5f %.5f\neyes %.4f %.4f %.4f / %.4f %.4f %.4f\n"
                "proj %.2f %.2f %.2f %.2f img %.0f %.0f %.0f %.0f target %ux%u\nvis L %.0f %.0f %.0f %.0f R %.0f %.0f %.0f %.0f\n"
                "depth a %.6g c %.6g mh %.6g zmax %.6g units %.3f\n",
                v->seq, v->inbetween, v->alpha, v->origin_m[0], v->origin_m[1], v->origin_m[2], v->cloud_origin_m[0],
                v->cloud_origin_m[1], v->cloud_origin_m[2], v->rebase, v->cam_x[0], v->cam_x[1], v->cam_x[2], v->cam_y[0],
                v->cam_y[1], v->cam_y[2], v->cam_z[0], v->cam_z[1], v->cam_z[2], v->eye_m[0][0], v->eye_m[0][1],
                v->eye_m[0][2], v->eye_m[1][0], v->eye_m[1][1], v->eye_m[1][2], v->proj[0], v->proj[1], v->proj[2], v->proj[3],
                v->img[0], v->img[1], v->img[2], v->img[3], v->target_w, v->target_h, v->vis[0][0], v->vis[0][1],
                v->vis[0][2], v->vis[0][3], v->vis[1][0], v->vis[1][1], v->vis[1][2], v->vis[1][3], v->depth_a, v->depth_c,
                v->depth_mh, v->depth_zmax, v->units_per_m);
        fclose(f);
    }
    ps2_log("clouds: dump of field %u written to %s_* (far map %s, near %s)", D.field, base, (D.have & 1u) ? "yes" : "no",
            (D.have & 2u) ? "yes" : "no");
}
/* record the copies of one kind (0 far after its pass, 1 near after its pass) */
static void dump_copy(VkCommandBuffer cmd, int fi, int kind) {
    if (D.state == 2 || cfg.dump_field <= 0) return;
    extern u32 ps2_gs_frame_count;
    if (D.state == 0) {
        if ((int)ps2_gs_frame_count < cfg.dump_field) return;
        D.n = G.gpu.dir_res; D.nw = G.near_w; D.nh = G.near_h;
        D.off_dir = 0; D.off_dirz = (VkDeviceSize)D.n * D.n * 8u;
        D.off_near = D.off_dirz + (VkDeviceSize)D.n * D.n * 2u;
        D.off_nearz = D.off_near + (VkDeviceSize)D.nw * D.nh * 8u * CL_NEAR_LAYERS;
        D.off_wx = D.off_nearz + (VkDeviceSize)D.nw * D.nh * 2u * CL_NEAR_LAYERS;
        D.size = D.off_wx + (VkDeviceSize)CL_WEATHER_RES * CL_WEATHER_RES * 4u;
        if (ps2_clouds_make_buffer(D.size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 2, &D.buf, &D.mem, (void **)&D.ptr) != 0) {
            D.state = 2; ps2_log("clouds: dump: no host memory"); return;
        }
        D.state = 1; D.fi = fi; D.field = ps2_gs_frame_count; D.have = 0; D.env = G.env; D.view = G.view;
    }
    if (D.state != 1 || D.fi != fi || (D.have & (1u << kind))) return;
    if (kind == 1 && (D.nw != G.near_w || D.nh != G.near_h || !G.near_c.img)) return;
    mem_barrier(cmd, CS | FS, VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy rg; memset(&rg, 0, sizeof rg);
    rg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; rg.imageExtent.depth = 1;
    if (kind == 0) {
        rg.imageSubresource.layerCount = 1; rg.imageExtent.width = D.n; rg.imageExtent.height = D.n;
        rg.bufferOffset = D.off_dir; vkCmdCopyImageToBuffer(cmd, G.dir.img, VK_IMAGE_LAYOUT_GENERAL, D.buf, 1, &rg);
        rg.bufferOffset = D.off_dirz; vkCmdCopyImageToBuffer(cmd, G.dir_depth.img, VK_IMAGE_LAYOUT_GENERAL, D.buf, 1, &rg);
        rg.imageExtent.width = rg.imageExtent.height = CL_WEATHER_RES;      /* the weather map as used this frame */
        rg.bufferOffset = D.off_wx; vkCmdCopyImageToBuffer(cmd, G.weather.img, VK_IMAGE_LAYOUT_GENERAL, D.buf, 1, &rg);
        ps2_clouds_shadow_dump_copy(cmd);                                   /* the shown cloud shadow map */
    } else {
        rg.imageSubresource.layerCount = CL_NEAR_LAYERS; rg.imageExtent.width = D.nw; rg.imageExtent.height = D.nh;
        rg.bufferOffset = D.off_near; vkCmdCopyImageToBuffer(cmd, G.near_c.img, VK_IMAGE_LAYOUT_GENERAL, D.buf, 1, &rg);
        rg.bufferOffset = D.off_nearz; vkCmdCopyImageToBuffer(cmd, G.near_d.img, VK_IMAGE_LAYOUT_GENERAL, D.buf, 1, &rg);
    }
    mem_barrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_HOST_BIT | CS | FS, VK_ACCESS_2_HOST_READ_BIT | RW);
    D.have |= 1u << kind;
}
static void dump_collect(int fi) {
    if (D.state != 1 || D.fi != fi) return;
    dump_write();
    D.state = 2;
}

static void debug_log(const ps2_cloud_view *v) {
    if (cfg.debug <= 0 || (G.frame % (uint32_t)cfg.debug) != 0u) return;
    const ps2_cloud_env *e = &G.env;
    double fm = G.gpu_n[0] ? G.gpu_ms[0][0] / G.gpu_n[0] : 0.0, fc = G.gpu_n[0] ? G.gpu_ms[0][1] / G.gpu_n[0] : 0.0;
    double nm = G.gpu_n[1] ? G.gpu_ms[1][0] / G.gpu_n[1] : 0.0, nc = G.gpu_n[1] ? G.gpu_ms[1][1] / G.gpu_n[1] : 0.0;
    double eu = G.gpu_n[2] ? G.gpu_ms[2][0] / G.gpu_n[2] : 0.0;
    ps2_log("clouds: frame %u seq %u%s alpha %.2f | C1 seq %u valid %02X band %.0f-%.0f m cov %.2f type %.2f in-cloud %.2f "
            "sun %.2f %.2f %.2f ncloud %u | C2 origin %.1f %.1f %.1f m fwd %.3f %.3f %.3f up %.3f %.3f %.3f ipd/2 %.4f "
            "proj %.1f %.1f %.1f %.1f img %.0f %.0f %.0f %.0f depth a %.4g c %.4g mh %.4g zmax %.4g | GPU ms per pass: "
            "early far update %.3f (%u; overlaps the game's draws), far point %.3f + comp %.3f (%u), near %.3f + comp %.3f (%u)",
            G.frame, v->seq, v->inbetween ? " in-between" : "", v->alpha, e->seq, e->valid, e->base_m, e->top_m, e->coverage,
            e->type, e->in_cloud, e->sun_dir[0], e->sun_dir[1], e->sun_dir[2], e->ncloud, v->origin_m[0], v->origin_m[1],
            v->origin_m[2], -v->cam_z[0], -v->cam_z[1], -v->cam_z[2], v->cam_y[0], v->cam_y[1], v->cam_y[2], v->half_ipd_m,
            v->proj[0], v->proj[1], v->proj[2], v->proj[3], v->img[0], v->img[1], v->img[2], v->img[3], v->depth_a,
            v->depth_c, v->depth_mh, v->depth_zmax, eu, G.gpu_n[2], fm, fc, G.gpu_n[0], nm, nc, G.gpu_n[1]);
    memset(G.gpu_ms, 0, sizeof G.gpu_ms); memset(G.gpu_n, 0, sizeof G.gpu_n);
    if (G.split_n[0][0] && G.split_n[0][1])               /* (PS2_PASS_AB_WHAT=cloudsplit / cloudshafts / compearly) */
        ps2_log("clouds: composites (+ stamp) GPU ms in PS2_PASS_AB's on / off blocks: far %.4f (%u) / %.4f (%u), near %.4f "
                "(%u) / %.4f (%u)", G.split_ms[0][0] / G.split_n[0][0], G.split_n[0][0], G.split_ms[0][1] / G.split_n[0][1],
                G.split_n[0][1], G.split_n[1][0] ? G.split_ms[1][0] / G.split_n[1][0] : 0.0, G.split_n[1][0],
                G.split_n[1][1] ? G.split_ms[1][1] / G.split_n[1][1] : 0.0, G.split_n[1][1]);
    memset(G.split_ms, 0, sizeof G.split_ms); memset(G.split_n, 0, sizeof G.split_n);
    ps2_clouds_shadow_debug();
}

/* ACZ_VR_CLOUD_VERIFY=N (debug): every Nth frame (frame slot 0 only, the far and near points of the first eye pass),
   slot 0's colour (both layers, RGBA) and depth are copied right before and right after the cloud pass; once the
   fence has passed the pairs are compared and logged ("clouds verify: ..."): what our pass changed. With the S0 stubs
   (and any pass that draws nothing) both counts must be 0. Host memory: 4 x 2 x w x h x 4 x layers bytes. */
static int verify_due(int fi, int kind, int eye) {    /* the far and the near point of one frame (first eye pass) */
    static uint32_t next, cur, decided;
    if (V.every <= 0 || fi != 0 || kind < 0 || kind > 1 || eye > 0) return 0;
    if (decided != G.frame) {            /* the first point of a frame decides for the frame */
        decided = G.frame; cur = 0;
        if (G.frame >= next && !V.pending) { cur = G.frame; next = G.frame + (uint32_t)V.every; }
    }
    return cur == G.frame && cur != 0u;
}
static void verify_copy(VkCommandBuffer cmd, const ps2_cloud_pass *p, uint32_t slot, VkImageLayout dl) {
    const ps2_cloud_view *v = p->view;
    uint32_t w = v->target_w, h = v->target_h, L = p->layers ? p->layers : 1u;
    VkDeviceSize snap = (VkDeviceSize)w * h * 4u * L * 2u;
    if (snap * CL_VERIFY_SNAPS > V.cap) {
        if (V.buf) { vkDeviceWaitIdle(G.gpu.dev); vkDestroyBuffer(G.gpu.dev, V.buf, NULL); vkFreeMemory(G.gpu.dev, V.mem, NULL); }
        V.buf = VK_NULL_HANDLE; V.cap = 0;
        if (ps2_clouds_make_buffer(snap * CL_VERIFY_SNAPS, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 2, &V.buf, &V.mem, (void **)&V.ptr) != 0) {
            ps2_log("clouds verify: no host memory for %llu MB -- off", (unsigned long long)(snap * CL_VERIFY_SNAPS >> 20));
            V.every = 0; return;
        }
        V.cap = snap * CL_VERIFY_SNAPS;
    }
    V.snap = snap; V.w = w; V.h = h; V.layers = L;
    VkImageMemoryBarrier2 b[2];
    img_barrier(&b[0], p->color, VK_IMAGE_ASPECT_COLOR_BIT, p->color_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    img_barrier(&b[1], p->depth, VK_IMAGE_ASPECT_DEPTH_BIT, dl, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    flush(cmd, b, 2, NULL, 0);
    VkBufferImageCopy rg; memset(&rg, 0, sizeof rg);
    rg.bufferOffset = snap * slot;
    rg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; rg.imageSubresource.layerCount = L;
    rg.imageExtent.width = w; rg.imageExtent.height = h; rg.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cmd, p->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, V.buf, 1, &rg);
    rg.bufferOffset = snap * slot + snap / 2u;
    rg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    vkCmdCopyImageToBuffer(cmd, p->depth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, V.buf, 1, &rg);
    img_barrier(&b[0], p->color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, p->color_layout,
                VK_PIPELINE_STAGE_2_COPY_BIT, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    img_barrier(&b[1], p->depth, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dl,
                VK_PIPELINE_STAGE_2_COPY_BIT, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    flush(cmd, b, 2, NULL, 0);
    mem_barrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                VK_ACCESS_2_HOST_READ_BIT);
    if (slot + 1u > V.nsnap) V.nsnap = slot + 1u;
    if (slot & 1u) V.have |= 1u << (slot >> 1);
    V.pending = 1; V.frame = G.frame;
}
static void verify_collect(void) {
    if (!V.pending || !V.ptr) return;
    for (uint32_t k = 0; 2u * k + 1u < V.nsnap; k++) {
        if (!(V.have & (1u << k))) continue;
        const uint8_t *a = V.ptr + V.snap * (2u * k), *b = V.ptr + V.snap * (2u * k + 1u);
        size_t npx = (size_t)V.w * V.h * V.layers;
        size_t nc = 0, nd = 0, nc1 = 0; int mx = 0;
        for (size_t i = 0; i < npx; i++) {
            const uint8_t *x = a + i * 4u, *y = b + i * 4u;
            if (memcmp(x, y, 4)) {
                nc++;
                if (i >= (size_t)V.w * V.h) nc1++;
                for (int c = 0; c < 4; c++) { int d = abs((int)x[c] - (int)y[c]); if (d > mx) mx = d; }
            }
        }
        const uint8_t *da = a + V.snap / 2u, *db = b + V.snap / 2u;
        for (size_t i = 0; i < npx; i++) if (memcmp(da + i * 4u, db + i * 4u, 4)) nd++;
        ps2_log("clouds verify: frame %u %s pass: colour changed %zu of %zu px (right layer %zu, max %d levels), "
                "depth changed %zu px", V.frame, V.kind[k] ? "near" : "far", nc, npx, nc1, mx, nd);
    }
    V.pending = 0; V.nsnap = 0; V.have = 0;
}

/* the first cloud pass of a command buffer: the readbacks of this frame slot's last use, C1, the query pool */
static void cmd_begin(const ps2_cloud_pass *p, int fi) {
    VkCommandBuffer cmd = p->cmd;
    /* a re-draw recorded into the same command buffer (PS2_PERF_AB: the dumped frame drawn again, as configured and
       with a switch flipped) keeps allocating passes after the frame's own: its uniform copies and descriptor sets must
       not overwrite the ones the frame's earlier passes still read when the command buffer executes */
    extern int ps2_vk_redraw_pass(void);
    int redraw = p->cmd == G.cmd_handle[fi] && ps2_vk_redraw_pass();
    G.cmd_handle[fi] = p->cmd;
    if (!redraw) read_timestamps(fi);
    if (fi == 0 && !redraw) verify_collect();
    if (!redraw) dump_collect(fi);
    G.cmd_id[fi] = p->cmd_id;
    if (!redraw) G.npass[fi] = 0;
    G.frame++;
    ps2_cloud_env_read(&G.env);
    env_defaults(&G.env);
    /* ACZ_VR_CLOUD_NEAR_REUSE: an in-between frame re-draws its game frame for a newer pose, and the near reuse puts that
       game frame's march behind its own: all its cloud passes use the game frame's C1 (by its record time S1 often
       published the next game frame's: colours, fog, drift, list), as its replayed game draws do */
    if (p->view && p->view->inbetween && G.have_env_game && ps2_clouds_near_reuse_active()) G.env = G.env_game;
    else if (p->view && !p->view->inbetween) { G.env_game = G.env; G.have_env_game = 1; }
    for (int i = 0; i < 8; i++)                 /* retired depth views, old enough */
        if (G.grave[i] && G.frame - G.grave_age[i] > 8u) { vkDestroyImageView(G.gpu.dev, G.grave[i], NULL); G.grave[i] = VK_NULL_HANDLE; }
    if (G.qp[fi] && !redraw) vkCmdResetQueryPool(cmd, G.qp[fi], 0, CL_PASSES * 3u);
}
/* C2 + the cloud frame (world minus the wind drift); a jump of the cloud-frame origin bumps rebase */
static void view_setup(const ps2_cloud_pass *p) {
    G.view = *p->view;
    G.view.cloud_origin_m[0] = G.view.origin_m[0] - (double)G.env.drift_m[0];
    G.view.cloud_origin_m[1] = G.view.origin_m[1];
    G.view.cloud_origin_m[2] = G.view.origin_m[2] - (double)G.env.drift_m[1];
    double dx = G.view.cloud_origin_m[0] - G.last_cloud_origin[0], dz = G.view.cloud_origin_m[2] - G.last_cloud_origin[1];
    if (G.have_last && dx * dx + dz * dz > 2000.0 * 2000.0) G.rebase++;
    G.last_cloud_origin[0] = G.view.cloud_origin_m[0]; G.last_cloud_origin[1] = G.view.cloud_origin_m[2]; G.have_last = 1;
    G.view.rebase = G.rebase;
}
/* the weather map: S2's weather pass, else the procedural default when C1 coverage / type change (the first pass of
   a command buffer; the common set is bound). dst = the stages that read it next */
static void weather_pass(VkCommandBuffer cmd, const ps2_cloud_rec *rec, VkPipelineStageFlags2 dst) {
    int own = ps2_clouds_weather_record(&G.gpu, cmd, rec);
    if (own) { G.wx_module = 1; mem_barrier(cmd, CS, RW, dst, RW); }
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, G.pipe_layout, 0, 1, &G.cur_set, 0, NULL);
    float wcov = G.env.coverage_unmasked > 0.0f ? G.env.coverage_unmasked : G.env.coverage;
    if (!G.wx_module && (wcov != G.weather_key[0] || G.env.type != G.weather_key[1])) {
        G.weather_key[0] = wcov; G.weather_key[1] = G.env.type;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, G.p_weather);
        {   int32_t pc[16]; memset(pc, 0, sizeof pc);              /* size; f0.x = the coverage to shape */
            pc[0] = pc[1] = (int32_t)CL_WEATHER_RES; memcpy(&pc[8], &wcov, sizeof wcov);
            vkCmdPushConstants(cmd, G.pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, CL_PUSH_BYTES, pc);
        }
        vkCmdDispatch(cmd, CL_WEATHER_RES / 8u, CL_WEATHER_RES / 8u, 1);
        mem_barrier(cmd, CS, RW, dst, RW);
    }
}
/* the cloud shadow map's slice and blend (ps2_clouds_shadow.c), after the weather pass; ordered before the far march
   (dst CS: early in the command buffer the game's draws must not wait for it -- the far composite's and the near
   point's leading barriers order it for everything later) or, at a split point, before every later reader */
static int shadow_pass(VkCommandBuffer cmd, const ps2_cloud_rec *rec, VkPipelineStageFlags2 dst) {
    if (ps2_clouds_shadow_frag()) dst |= FS;       /* a terrain / fog reader (ps2_clouds_shadow_hook) */
    int rec_ok = ps2_clouds_shadow_record(cmd, rec);   /* (+ the cockpit probe slot to the host after the fence) */
    if (rec_ok)
        mem_barrier(cmd, CS, RW, dst | VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_HOST_READ_BIT);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, G.pipe_layout, 0, 1, &G.cur_set, 0, NULL);
    return rec_ok;
}
/* the far update (S2's hook, or the test pattern), once per command buffer; the common set is bound */
static void far_update(VkCommandBuffer cmd, const ps2_cloud_pass *p, const ps2_cloud_rec *rec) {
    G.far_cmd = p->cmd_id;
    if (cfg.test & 1) {
        if (!G.test_far_done) {
            uint32_t N = G.gpu.dir_res;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, G.p_test);
            push(cmd, 0, 1, 0, 0, 0, 0, 0, (int32_t)N, (int32_t)N);
            vkCmdDispatch(cmd, (N + 7u) / 8u, (N + 7u) / 8u, 1);
            G.test_far_done = 1;
        }
    } else ps2_clouds_far_record(&G.gpu, cmd, rec);
}

/* The far update early in the command buffer (ps2_video.c render_list, right after C2 is known and before the game's
   draws): the weather pass and S2's classify / args / march are latency-bound (dependent in-cloud steps on a few
   hundred tiles), so recorded here they overlap the game's draws instead of stalling the GPU right before the far
   composite. No barrier after them: the one in front of the far composite (split_point) orders the map, the near
   point's leading barrier orders the weather. Runs for near-only too (the weather map). The scene depth is not read
   here (p->depth may be NULL; the common set keeps the last depth view). Returns 1 if recorded. */
static int async_used;                                  /* (PS2_PERF_CLOUD_ASYNC: the last early update used async_cmd) */
int ps2_clouds_far_async_used(void) { return async_used; }
/* ACZ_VR_CLOUD_REDRAW_KEEP (perf6_clouds 2026-11-03; default 1): a re-draw of a dumped frame in its own command buffer
   (PS2_PERF_AB: the frame drawn again as configured and with a switch flipped) keeps the frame's cloud inputs -- its C1
   copy (S1 publishes a newer one with the drift and the list's eased opacities while the frame records) and the maps its
   early update made (no second weather splat, shadow slice or far-map march, whose tiles the scheduler picks anew) -- so
   both re-draws run our passes on the same inputs and a flip's difference is the flip's alone. Before, every re-draw
   re-read C1 and updated the maps: the PS2_PERF_AB=2 control differed by 1-3 levels over whole cloud banks (M1 in the
   overcast: 20-330 k px of 0.9 M). 0 = as before. */
static int redraw_keep(void) {
    static int v = -1;
    if (v < 0) v = env_int("ACZ_VR_CLOUD_REDRAW_KEEP", 1, 0, 1);
    return v;
}
int ps2_clouds_far_early(const ps2_cloud_pass *p) {
    async_used = 0;
    if (!G.ok || !p->view || !p->view->valid || (!cfg.far_on && !cfg.near_on)) return 0;
    int fi = p->frame_index;
    if (fi < 0 || fi >= (int)CL_FIF || p->cmd_id == G.cmd_id[fi]) return 0;     /* the first pass of the buffer only */
    {   extern int ps2_vk_redraw_pass(void);
        if (ps2_vk_redraw_pass() && p->cmd == G.cmd_handle[fi]) cd_flip_step();     /* (the flip's rotation) */
        if (redraw_keep() && p->cmd == G.cmd_handle[fi] && ps2_vk_redraw_pass()) {   /* (ACZ_VR_CLOUD_REDRAW_KEEP) */
            G.cmd_id[fi] = p->cmd_id; G.far_cmd = p->cmd_id;
            return 0;
        }
    }
    VkCommandBuffer cmd = p->cmd;
    cmd_begin(p, fi);
    view_setup(p);
    G.inb[fi] = (uint8_t)(G.view.inbetween != 0);
    ps2_clouds_shadow_begin(&G.env, &G.view, fi, G.frame);
    ps2_clouds_optics_begin(&G.env, &G.view, fi, G.frame);
    ps2_clouds_wake_begin(fi, G.frame);
    debug_log(&G.view);
    if (G.npass[fi] >= CL_PASSES) return 0;
    uint32_t k = G.npass[fi]++;
    VkDeviceSize off = (VkDeviceSize)G.ubo_slot * (fi * CL_PASSES + k);
    sh_params();                                        /* (ACZ_VR_CLOUD_SHAFTS: this frame's distances into the UBO) */
    cd_pass(&G.view, 0);                                /* (CLOUD DETAIL: this pass's near divisor) */
    fill_ubo((cl_frame_ubo *)(G.ubo_ptr + off), &G.view, &G.env);
    G.cur_set = G.sets[fi][k];
    write_set(G.cur_set, off);
    mem_barrier(cmd, CS | FS, RW, CS, RW);              /* the last frame's composites read the maps */
    init_images(cmd);
    if (G.qp[fi]) vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, G.qp[fi], k * 3u);
    G.qkind[fi][k] = 2u;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, G.pipe_layout, 0, 1, &G.cur_set, 0, NULL);
    ps2_cloud_rec rec; memset(&rec, 0, sizeof rec);
    rec.env = &G.env; rec.view = &G.view; rec.eye = -1; rec.frame = G.frame; rec.frame_index = fi; rec.first_in_cmd = 1;
    weather_pass(cmd, &rec, CS);
    /* the optics probe: reads the weather and the previous frame's far map; the shadow pass's barrier orders it
       before the far update and makes its result visible to the host (without a shadow dispatch: its own barrier) */
    int opt_rec = ps2_clouds_optics_record(cmd, &rec, cfg.near_on ? cfg.near_range_m : 0.0f, 0);
    if (ps2_clouds_wake_record(cmd, &rec, 0)) opt_rec = 1;   /* the cloud wake's probe: ordered the same way */
    if (opt_rec) vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, G.pipe_layout, 0, 1, &G.cur_set, 0, NULL);
    if (!shadow_pass(cmd, &rec, CS) && opt_rec)
        mem_barrier(cmd, CS, RW, CS | VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_HOST_READ_BIT);
    /* PS2_PERF_CLOUD_ASYNC (perf4_passes): the far update and the shaft slice into the second queue's command buffer; its
       submission waits for the part holding everything above (and every earlier reader of the maps), the far map's
       readers wait for it (ps2_video.c cla_*) */
    VkCommandBuffer fcmd = p->async_cmd ? p->async_cmd : cmd;
    async_used = 0;
    if (p->async_cmd && cfg.far_on) {
        vkCmdBindDescriptorSets(fcmd, VK_PIPELINE_BIND_POINT_COMPUTE, G.pipe_layout, 0, 1, &G.cur_set, 0, NULL);
        async_used = 1;
    }
    if (cfg.far_on) far_update(fcmd, p, &rec);
    if (cfg.far_on) shafts_pass(fcmd);                  /* (ACZ_VR_CLOUD_SHAFTS: reads the shadow map, no barrier after it:
                                                           the far point's leading barrier orders it for the composite) */
    if (G.qp[fi]) {
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, G.qp[fi], k * 3u + 1u);
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, G.qp[fi], k * 3u + 2u);
        G.qmask[fi] |= 1u << k;
    }
    G.cur_set = VK_NULL_HANDLE;
    return 1;
}

/* ACZ_REC_CINE (composite_only with near_h): the near buffer's pixel of a target pixel, a homography in the composite's
   push constants: i1.y 1 = on; columns (h00 h10 h20) f1.xyz, (h01 h11 h21) f1.w f0.y f0.z, (h02 h12 h22) f0.w i1.z i1.w
   (cloud_comp.frag near_px) */
static void near_h_push(const ps2_cloud_pass *p, int32_t pc[16]) {
    if (!p->composite_only || !p->near_h_on) return;
    const float *h = p->near_h;
    float col[9] = { h[0], h[3], h[6], h[1], h[4], h[7], h[2], h[5], h[8] };
    pc[5] = 1;
    memcpy(&pc[12], &col[0], 4 * sizeof(float));                  /* f1 = h00 h10 h20 h01 */
    memcpy(&pc[9], &col[4], 3 * sizeof(float));                   /* f0.yzw = h11 h21 h02 */
    memcpy(&pc[6], &col[7], 2 * sizeof(float));                   /* i1.zw = h12 h22 */
}
/* kind 0 far, 1 near */
static int split_point(const ps2_cloud_pass *p, int kind) {
    if (!G.ok || !p->view || !p->view->valid || !p->depth || !p->color_view || !p->depth_layout
        || *p->depth_layout == VK_IMAGE_LAYOUT_UNDEFINED) return 0;
    const ps2_cloud_view *v = &G.view;
    int fi = p->frame_index;
    if (fi < 0 || fi >= (int)CL_FIF) return 0;
    VkCommandBuffer cmd = p->cmd;
    {   extern int ps2_vk_redraw_pass(void);              /* (ACZ_VR_CLOUD_REDRAW_KEEP: a re-draw keeps the frame's C1) */
        if (p->cmd_id != G.cmd_id[fi] && redraw_keep() && p->cmd == G.cmd_handle[fi] && ps2_vk_redraw_pass()) {
            cd_flip_step();                               /* (no early update in this re-draw: the flip's rotation) */
            G.cmd_id[fi] = p->cmd_id; G.far_cmd = p->cmd_id;
        }
    }
    if (p->cmd_id != G.cmd_id[fi]) cmd_begin(p, fi);   /* (no early far update in this command buffer) */
    view_setup(p);
    ps2_clouds_shadow_begin(&G.env, &G.view, fi, G.frame);   /* (once per command buffer) */
    ps2_clouds_optics_begin(&G.env, &G.view, fi, G.frame);   /* (once per command buffer) */
    ps2_clouds_wake_begin(fi, G.frame);                       /* (once per command buffer) */
    if (G.npass[fi] == 0u) debug_log(v);
    uint32_t k = G.npass[fi];
    if (k >= CL_PASSES) return 0;
    if (kind == 1 && !p->composite_only && near_size(v->target_w, v->target_h) != 0) return 0;   /* (ACZ_REC_CINE: the
                                    frame's near buffer as it is; its own size may differ, ACZ_REC_CINE_SCALE: near_h) */
    if (kind == 1 && p->composite_only && !G.near_c.img) return 0;
    if (depth_view_for(p->depth, p->layers) != 0) return 0;
    G.npass[fi] = k + 1u;
    G.inb[fi] = (uint8_t)(v->inbetween != 0);
    int first_in_cmd = k == 0u;
    VkDeviceSize off = (VkDeviceSize)G.ubo_slot * (fi * CL_PASSES + k);
    cd_pass(v, p->composite_only);                    /* (CLOUD DETAIL: this pass's near divisor) */
    fill_ubo((cl_frame_ubo *)(G.ubo_ptr + off), v, &G.env);
    G.cur_set = G.sets[fi][k];
    write_set(G.cur_set, off);

    int co = p->composite_only;                       /* (ACZ_REC_CINE: the composite only) */
    if (co) keep_views = 1;
    if (co && kind == 1 && (!p->near_h_on || !G.near_drew[fi])) { G.cur_set = VK_NULL_HANDLE; return 0; }   /* (no near
                                                                       buffer of this frame: nothing to composite) */
    int vfy = co ? 0 : verify_due(fi, kind, p->eye);
    if (vfy) { V.kind[kind] = (uint32_t)kind; verify_copy(cmd, p, 2u * (uint32_t)kind, *p->depth_layout); }
    /* scene depth -> sampled; colour: earlier attachment writes before our compute reads nothing, the composite later */
    VkImageMemoryBarrier2 ib[2];
    VkImageLayout dl = *p->depth_layout;
    uint32_t ni = 0;
    if (dl != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        img_barrier(&ib[ni++], p->depth, VK_IMAGE_ASPECT_DEPTH_BIT, dl, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, CS | FS, VK_ACCESS_2_SHADER_READ_BIT);
    *p->depth_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    /* earlier composites read our images: before the hooks write them (PS2_PERF_CLOUD_BARRIERS: in the same call as
       the depth's transition -- two barriers with nothing between them, one dependency info) */
    if (cl_bar_merge()) {
        VkMemoryBarrier2 m0; memset(&m0, 0, sizeof m0);
        m0.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        m0.srcStageMask = CS | FS; m0.srcAccessMask = RW; m0.dstStageMask = CS; m0.dstAccessMask = RW;
        flush(cmd, ib, ni, &m0, 1);
    } else {
        if (ni) flush(cmd, ib, ni, NULL, 0);
        mem_barrier(cmd, CS | FS, RW, CS, RW);
    }
    init_images(cmd);

    if (G.qp[fi]) { vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, G.qp[fi], k * 3u); }
    G.qkind[fi][k] = (uint32_t)kind;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, G.pipe_layout, 0, 1, &G.cur_set, 0, NULL);
    ps2_cloud_rec rec; memset(&rec, 0, sizeof rec);
    rec.env = &G.env; rec.view = v; rec.eye = p->eye; rec.frame = G.frame; rec.frame_index = fi;
    rec.first_in_cmd = first_in_cmd;
    if (first_in_cmd && !co) { weather_pass(cmd, &rec, CS | FS); shadow_pass(cmd, &rec, CS | FS); }   /* (no early update) */
    int draw = 1;
    if (co) {                                         /* (ACZ_REC_CINE: the frame's results) */
    } else if (kind == 0) {
        if (p->cmd_id != G.far_cmd) far_update(cmd, p, &rec); /* (normally done early: ps2_clouds_far_early) */
    } else {
        if (cfg.test & 2) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, G.p_test);
            push(cmd, 0, 2, p->eye < 0 ? 0 : p->eye, 0, 0, 0, 0, 0, 0);
            vkCmdDispatch(cmd, (G.near_w + 7u) / 8u, (G.near_h + 7u) / 8u, p->eye < 0 ? 2u : 1u);
        } else draw = ps2_clouds_near_record(&G.gpu, cmd, &rec);
    }
    mem_barrier(cmd, CS, RW, CS | FS, VK_ACCESS_2_SHADER_READ_BIT);
    /* the optics probe, when the early update did not record it (the far map is complete here) */
    if (!co && kind == 0 && ps2_clouds_optics_record(cmd, &rec, cfg.near_on ? cfg.near_range_m : 0.0f, 1))
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, G.pipe_layout, 0, 1, &G.cur_set, 0, NULL);
    if (!co && ps2_clouds_wake_record(cmd, &rec, 1))          /* (the cloud wake's probe, likewise) */
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, G.pipe_layout, 0, 1, &G.cur_set, 0, NULL);
    if (G.qp[fi]) vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, G.qp[fi], k * 3u + 1u);

    VkMemoryBarrier2 mbc; memset(&mbc, 0, sizeof mbc);   /* (PS2_PERF_CLOUD_BARRIERS: the composite's last barrier) */
    if (draw) {
        /* the composite: its own rendering over the colour target (no depth attachment: the depth is sampled) */
        VkMemoryBarrier2 mb; memset(&mb, 0, sizeof mb);
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        mb.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_COPY_BIT | FS;
        mb.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        mb.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        flush(cmd, NULL, 0, &mb, 1);
        VkRenderingAttachmentInfo col; memset(&col, 0, sizeof col);
        col.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        col.imageView = p->color_view; col.imageLayout = p->color_layout;
        col.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; col.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ri; memset(&ri, 0, sizeof ri);
        ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea.extent.width = v->target_w; ri.renderArea.extent.height = v->target_h;
        ri.layerCount = 1; ri.viewMask = G.gpu.multiview ? 3u : 0u;
        ri.colorAttachmentCount = 1; ri.pColorAttachments = &col;
        vkCmdBeginRendering(cmd, &ri);
        VkViewport vp; memset(&vp, 0, sizeof vp);
        vp.width = (float)v->target_w; vp.height = (float)v->target_h; vp.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &vp);
        VkRect2D sc;
        int32_t x0 = (int32_t)v->img[0], y0 = (int32_t)v->img[1];
        int32_t x1 = (int32_t)(v->img[0] + v->img[2]), y1 = (int32_t)(v->img[1] + v->img[3]);
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > (int32_t)v->target_w) x1 = (int32_t)v->target_w;
        if (y1 > (int32_t)v->target_h) y1 = (int32_t)v->target_h;
        if (x1 <= x0 || y1 <= y0) { x0 = y0 = 0; x1 = (int32_t)v->target_w; y1 = (int32_t)v->target_h; }
        if (co) {                                       /* (ACZ_REC_CINE: the window's rectangle) */
            if (p->clip[0] > x0) x0 = p->clip[0];
            if (p->clip[1] > y0) y0 = p->clip[1];
            if (p->clip[2] < x1) x1 = p->clip[2];
            if (p->clip[3] < y1) y1 = p->clip[3];
            if (x1 <= x0) x1 = x0 + 1;
            if (y1 <= y0) y1 = y0 + 1;
        }
        sc.offset.x = x0; sc.offset.y = y0; sc.extent.width = (uint32_t)(x1 - x0); sc.extent.height = (uint32_t)(y1 - y0);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        /* PS2_PASS_MASK (ps2_video.c pm_*): over each eye's visible mesh instead of the whole target */
        extern int ps2_vk_pm_cloud(VkCommandBuffer cmd, uint32_t *first, uint32_t *count);
        uint32_t pm_first = 0, pm_count = 0;
        int pm_mesh = G.p_comp_pm && ps2_vk_pm_cloud(cmd, &pm_first, &pm_count);
        /* ACZ_VR_CLOUD_COMP_UNROLL A/B: the other way in PS2_PASS_AB's "off" blocks (what = compunroll, 10) and in the
           PS2_PERF_AB re-draw with PS2_PERF_AB_FLIP=compunroll */
        extern int ps2_vk_pass_ab_off(int what);
        extern int ps2_vk_perf_ab_flipped(const char *name);
        int cx = G.p_comp_x && (ps2_vk_pass_ab_off(10) || ps2_vk_perf_ab_flipped("compunroll"));
        VkPipeline cpn = kind == 1 ? cd_comp(pm_mesh, G.gpu.near_div) : pm_mesh ? G.p_comp_pm : G.p_comp;   /* (perf6_clouds) */
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          pm_mesh ? (cx && G.p_comp_pm_x ? G.p_comp_pm_x : cpn) : (cx ? G.p_comp_x : cpn));
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, G.pipe_layout, 0, 1, &G.cur_set, 0, NULL);
        {   int32_t pcc[16]; memset(pcc, 0, sizeof pcc);
            pcc[0] = kind == 0 ? 1 : 2; pcc[1] = p->eye < 0 ? 0 : p->eye; pcc[2] = cfg.crop;
            pcc[3] = FX.tint && fx_stamp_now();                 /* ACZ_DEBUG_CLOUD_FX_TINT: where the stamp goes */
            pcc[4] = fx_edge_now();                              /* ACZ_VR_CLOUD_EDGE_DEPTH */
            if (kind == 0) pcc[4] |= split_now() << 1;           /* ACZ_VR_CLOUD_FAR_SPLIT (bit 1) */
            pcc[4] |= early_now() << 2;                          /* ACZ_VR_CLOUD_COMP_EARLY (bit 2; far and near) */
            {   extern int ps2_vk_pass_ab_off(int what);        /* (the composites' own time per PS2_PASS_AB state) */
                G.qsplit[fi][k] = (uint8_t)(ps2_vk_pass_ab_off(13) || ps2_vk_pass_ab_off(14) || ps2_vk_pass_ab_off(15)); }
            memcpy(&pcc[8], &FX.lim, sizeof(float));
            near_h_push(p, pcc);                                 /* (ACZ_REC_CINE: the near buffer's pixels) */
            vkCmdPushConstants(cmd, G.pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, CL_PUSH_BYTES, pcc);
        }
        if (pm_mesh) vkCmdDraw(cmd, pm_count, 1, pm_first, 0);
        else vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
        G.composites[kind]++;
        mb.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        mb.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        mbc = mb;
        if (!cl_bar_merge()) flush(cmd, NULL, 0, &mb, 1);   /* (PS2_PERF_CLOUD_BARRIERS: with the depth's, below) */
    }
    /* the depth back to an attachment (the caller reopens the frame's rendering with it) */
    img_barrier(&ib[0], p->depth, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, CS | FS, 0,
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    flush(cmd, ib, 1, draw && cl_bar_merge() ? &mbc : NULL, draw && cl_bar_merge() ? 1u : 0u);
    *p->depth_layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    /* ACZ_VR_CLOUD_FX: our dense cloud's distance into the frame's depth, over the screen box of the draws that meet it
       before the next point (ps2_video.c; none: no stamp). The near point stamps near then far (mode 4), or the far map
       alone when the near field drew nothing this frame (mode 3; the near buffer is stale then). */
    extern int ps2_vk_cloud_fx_box(int kind, int32_t box[4]);   /* ps2_video.c: x0, x1, y0, y1 (target pixels) */
    int32_t fxb[4], sx0 = 0, sy0 = 0, sx1 = 0, sy1 = 0;
    float slim = fx_stamp_lim();                      /* (ACZ_VR_CLOUD_FX_SOFT: the stamp only below the fade) */
    if ((draw || (kind == 1 && cfg.far_on)) && fx_soft_now()) {   /* the veil's inputs for this pass's effects */
        memset(VL.pcv, 0, sizeof VL.pcv);
        VL.pcv[0] = kind == 0 || !draw ? 3 : cfg.far_on ? 4 : 5;
        VL.pcv[1] = p->eye < 0 ? 0 : p->eye; VL.pcv[2] = cfg.crop; VL.pcv[4] = fx_edge_now() | (split_now() << 1);
        near_h_push(p, VL.pcv);
        VL.set = G.cur_set; VL.ok = 1;
    }
    if ((draw || (kind == 1 && cfg.far_on)) && fx_stamp_now() && slim > 0.0f && ps2_vk_cloud_fx_box(kind, fxb)) {
        sx0 = (int32_t)v->img[0]; sy0 = (int32_t)v->img[1];       /* the eye images, cut to the effects' box */
        sx1 = (int32_t)(v->img[0] + v->img[2]); sy1 = (int32_t)(v->img[1] + v->img[3]);
        if (sx1 <= sx0 || sy1 <= sy0) { sx0 = sy0 = 0; sx1 = (int32_t)v->target_w; sy1 = (int32_t)v->target_h; }
        if (fxb[0] > sx0) sx0 = fxb[0];
        if (fxb[1] < sx1) sx1 = fxb[1];
        if (fxb[2] > sy0) sy0 = fxb[2];
        if (fxb[3] < sy1) sy1 = fxb[3];
        if (sx0 < 0) sx0 = 0;
        if (sy0 < 0) sy0 = 0;
        if (sx1 > (int32_t)v->target_w) sx1 = (int32_t)v->target_w;
        if (sy1 > (int32_t)v->target_h) sy1 = (int32_t)v->target_h;
        if (co) {                                       /* (ACZ_REC_CINE: the window's rectangle) */
            if (p->clip[0] > sx0) sx0 = p->clip[0];
            if (p->clip[1] > sy0) sy0 = p->clip[1];
            if (p->clip[2] < sx1) sx1 = p->clip[2];
            if (p->clip[3] < sy1) sy1 = p->clip[3];
        }
    }
    if (sx1 > sx0 && sy1 > sy0) {
        VkRenderingAttachmentInfo da; memset(&da, 0, sizeof da);
        da.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        da.imageView = G.depth_view; da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ri; memset(&ri, 0, sizeof ri);
        ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea.offset.x = sx0; ri.renderArea.offset.y = sy0;
        ri.renderArea.extent.width = (uint32_t)(sx1 - sx0); ri.renderArea.extent.height = (uint32_t)(sy1 - sy0);
        ri.layerCount = 1; ri.viewMask = G.gpu.multiview ? 3u : 0u;
        ri.pDepthAttachment = &da;
        vkCmdBeginRendering(cmd, &ri);
        VkViewport vp; memset(&vp, 0, sizeof vp);
        vp.width = (float)v->target_w; vp.height = (float)v->target_h; vp.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &vp);
        VkRect2D sc;
        sc.offset.x = sx0; sc.offset.y = sy0; sc.extent.width = (uint32_t)(sx1 - sx0); sc.extent.height = (uint32_t)(sy1 - sy0);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, cd_fx(0));
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, G.pipe_layout, 0, 1, &G.cur_set, 0, NULL);
        int32_t pcv[16]; memset(pcv, 0, sizeof pcv);
        pcv[0] = kind == 0 || !draw ? 3 : cfg.far_on ? 4 : 5;  /* far map / near then near x far / near only */
        pcv[1] = p->eye < 0 ? 0 : p->eye; pcv[2] = cfg.crop; pcv[4] = fx_edge_now() | (split_now() << 1);   /* (+ ACZ_VR_CLOUD_FAR_SPLIT) */
        memcpy(&pcv[8], &slim, sizeof(float));
        near_h_push(p, pcv);
        vkCmdPushConstants(cmd, G.pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, CL_PUSH_BYTES, pcv);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
        VkMemoryBarrier2 mz; memset(&mz, 0, sizeof mz);
        mz.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        mz.srcStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        mz.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        mz.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        mz.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        flush(cmd, NULL, 0, &mz, 1);
        FX.stamps++;
        FX.px += (uint64_t)(sx1 - sx0) * (uint64_t)(sy1 - sy0);
    }
    {   static uint32_t fx_log_frame;                /* the stamp's share of the frame, every ~30 s */
        if (!fx_log_frame) fx_log_frame = G.frame ? G.frame : 1u;
        if (FX.mode && kind == 0 && G.frame >= fx_log_frame + 3600u) {
            ps2_log("clouds fx: %llu depth stamps in %u frames, mean box %.0f px (%.1f %% of the target)",
                        (unsigned long long)FX.stamps, G.frame - fx_log_frame, FX.stamps ? (double)FX.px / (double)FX.stamps : 0.0,
                        FX.stamps ? 100.0 * (double)FX.px / (double)FX.stamps / ((double)v->target_w * (double)v->target_h) : 0.0);
            fx_log_frame = G.frame; FX.stamps = 0; FX.px = 0;
        }
    }
    /* (the pass's end stamp after the depth stamp, so the cloud passes' own time includes it) */
    if (G.qp[fi]) { vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, G.qp[fi], k * 3u + 2u); G.qmask[fi] |= 1u << k; }
    if (vfy) verify_copy(cmd, p, 2u * (uint32_t)kind + 1u, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    if (cfg.dump_field > 0 && !co) dump_copy(cmd, fi, kind);
    if (kind == 1 && !co) G.near_drew[fi] = (uint8_t)(draw != 0);   /* (for a composite-only pass in the same frame) */
    G.passes[kind]++;
    G.cur_set = VK_NULL_HANDLE;
    return draw;
}

/* ACZ_VR_CLOUD_AB=N (measurement): every N recorded frames the clouds switch off and on again (off = the game's own
   clouds, nothing of ours); the frame GPU times of both phases are averaged and logged every 1200 frames, so a cost
   comparison sees the same scenes (no run-to-run variance). */
/* ACZ_VR_CLOUD_SHADOW_AB=N (measurement, used when ACZ_VR_CLOUD_AB is not set): every N recorded frames the cloud
   shadow map switches off and on again (off = the "cloud above" probe, no map passes); logged like the clouds A/B, plus
   the GPU time of all cloud passes (own timestamps) per phase */
/* ACZ_VR_OPTICS_AB=N (measurement, used when neither of the above is set): every N recorded frames the optical
   phenomena switch off and on again (off = no probe, opt[] zero); logged like the shadow A/B */
/* ACZ_VR_CLOUD_FX_AB=N (measurement, used when none of the above is set): every N recorded frames the effect depth
   stamp (ACZ_VR_CLOUD_FX) switches off and on again (the placement stays); logged like the shadow A/B */
/* ACZ_VR_CLOUD_FX_SOFT_AB=N (measurement, fx_mpbm 2026-10-31, bug #254; used when none of the above is set): every N
   recorded frames the effects' cloud veil (ACZ_VR_CLOUD_FX_SOFT) switches off -- the binary stamp at its limit as before
   -- and on again; logged like the stamp's A/B ("clouds veil A/B") */
/* ACZ_VR_CLOUD_NEAR_REUSE_AB=N (measurement, nx_cloudreuse; used when none of the above is set): every N recorded frames
   the in-between near reuse (ACZ_VR_CLOUD_NEAR_REUSE, with the game frames' history output) switches off and on again;
   the first 4 frames of a block are not counted; every 1200 counted frames a "clouds near-reuse A/B:" line with the
   frame GPU and all cloud passes' GPU time (own timestamps) per phase, split into game frames and in-between frames */
static int ab_every = -1, ab_shadow, ab_optics, ab_fx, ab_nreuse, ab_soft, ab_detail;
static uint32_t ab_frames, ab_n[2];
static int ab_phase_of[CL_FIF];
static double ab_sum[2], ab_max[2], ab_cl[2];
static double abr_sum[2][2], abr_cl[2][2]; static uint32_t abr_n[2][2];   /* [phase: 0 on, 1 off][in-between] */
int ps2_clouds_ab_off(int fi) {
    if (ab_every < 0) {
        ab_every = env_int("ACZ_VR_CLOUD_AB", 0, 0, 100000);
        if (ab_every <= 0) { ab_every = env_int("ACZ_VR_CLOUD_SHADOW_AB", 0, 0, 100000); ab_shadow = ab_every > 0; }
        if (ab_every <= 0) { ab_every = env_int("ACZ_VR_OPTICS_AB", 0, 0, 100000); ab_optics = ab_every > 0; }
        if (ab_every <= 0) { ab_every = env_int("ACZ_VR_CLOUD_FX_AB", 0, 0, 100000); ab_fx = ab_every > 0; }
        if (ab_every <= 0) { ab_every = env_int("ACZ_VR_CLOUD_FX_SOFT_AB", 0, 0, 100000); ab_soft = ab_every > 0; }
        if (ab_every <= 0) { ab_every = env_int("ACZ_VR_CLOUD_NEAR_REUSE_AB", 0, 0, 100000); ab_nreuse = ab_every > 0; }
        if (ab_every <= 0) { cd_cfg(); ab_every = CD.ab; ab_detail = ab_every > 0; }   /* (perf6_clouds) */
    }
    if (ab_every <= 0 || fi < 0 || fi >= (int)CL_FIF) return 0;
    uint32_t abf = ab_frames++;
    int ph = (int)((abf / (uint32_t)ab_every) & 1u);
    ab_phase_of[fi] = ph + 1;
    if (ab_nreuse) {
        ps2_clouds_near_reuse_ab(ph);
        if (abf % (uint32_t)ab_every < 4u) ab_phase_of[fi] = 0;      /* (the block's first frames: not counted) */
        return 0;
    }
    if (ab_detail) {                       /* ACZ_VR_CLOUD_DETAIL_AB: A, B1, A, B2, ... */
        uint32_t blk = abf / (uint32_t)ab_every;
        uint32_t rh = (blk >> 1) * 2654435761u; rh ^= rh >> 15; rh *= 2246822519u; rh ^= rh >> 13;   /* (the B entry at
                                       random per pair: a fixed rotation locks onto the scripted head motion, LESSONS 5.1) */
        int st = (blk & 1u) ? 1 + (int)(rh % (uint32_t)CD.nset) : 0;
        CD.cur = st ? CD.set_lv[st - 1] : CD.cfg;
        ab_phase_of[fi] = abf % (uint32_t)ab_every < 4u ? 0 : st + 1;
        cda_blk_of[fi] = blk;
        return 0;
    }
    if (ab_shadow) { ps2_clouds_shadow_ab(ph); return 0; }
    if (ab_optics) { ps2_clouds_optics_ab(ph); return 0; }
    if (ab_fx) { FX.ab_off = ph; return 0; }
    if (ab_soft) { FX.soft_ab_off = ph; if (abf % (uint32_t)ab_every < 4u) ab_phase_of[fi] = 0; return 0; }
    return ph;
}
void ps2_clouds_ab_sample(int fi, double ms) {
    if (ab_every <= 0 || fi < 0 || fi >= (int)CL_FIF || !ab_phase_of[fi]) return;
    int ph = ab_phase_of[fi] - 1;
    ab_phase_of[fi] = 0;
    if (ab_detail) { cda_sample(fi, ph, ms); return; }   /* ACZ_VR_CLOUD_DETAIL_AB */
    if (ab_nreuse) {                       /* ACZ_VR_CLOUD_NEAR_REUSE_AB */
        int ib = G.inb[fi] ? 1 : 0;
        abr_sum[ph][ib] += ms; abr_cl[ph][ib] += ps2_clouds_gpu_ms_peek(fi); abr_n[ph][ib]++;
        uint32_t tot = abr_n[0][0] + abr_n[0][1] + abr_n[1][0] + abr_n[1][1];
        if (tot >= 1200u && abr_n[0][0] && abr_n[0][1] && abr_n[1][0] && abr_n[1][1]) {
#define ABR(a, k, i) ((a)[k][i] / (double)abr_n[k][i])
            ps2_log("clouds near-reuse A/B: frame GPU game %.4f / %.4f ms (%u / %u frames), in-between %.4f / %.4f ms "
                    "(%u / %u): %+.4f / %+.4f ms; cloud passes game %.4f / %.4f, in-between %.4f / %.4f: %+.4f / %+.4f ms "
                    "(reuse on / off)", ABR(abr_sum, 0, 0), ABR(abr_sum, 1, 0), abr_n[0][0], abr_n[1][0],
                    ABR(abr_sum, 0, 1), ABR(abr_sum, 1, 1), abr_n[0][1], abr_n[1][1],
                    ABR(abr_sum, 0, 0) - ABR(abr_sum, 1, 0), ABR(abr_sum, 0, 1) - ABR(abr_sum, 1, 1),
                    ABR(abr_cl, 0, 0), ABR(abr_cl, 1, 0), ABR(abr_cl, 0, 1), ABR(abr_cl, 1, 1),
                    ABR(abr_cl, 0, 0) - ABR(abr_cl, 1, 0), ABR(abr_cl, 0, 1) - ABR(abr_cl, 1, 1));
#undef ABR
            memset(abr_sum, 0, sizeof abr_sum); memset(abr_cl, 0, sizeof abr_cl); memset(abr_n, 0, sizeof abr_n);
        }
        return;
    }
    ab_sum[ph] += ms; ab_n[ph]++;
    if (ab_shadow || ab_optics || ab_fx || ab_soft) ab_cl[ph] += ps2_clouds_gpu_ms_peek(fi);
    if (ms > ab_max[ph]) ab_max[ph] = ms;
    if (ab_n[0] + ab_n[1] >= 1200u && ab_n[0] && ab_n[1]) {
        if (ab_shadow || ab_optics || ab_fx || ab_soft)
            ps2_log("clouds %s A/B: frame GPU with %s %.3f ms (max %.3f, %u frames), without %.3f ms (max %.3f, "
                    "%u frames): %+.3f ms; cloud passes %.3f vs %.3f ms: %+.3f ms",
                    ab_optics ? "optics" : ab_fx ? "fx" : ab_soft ? "veil" : "shadow",
                    ab_optics ? "the optics" : ab_fx ? "the effect depth stamp" : ab_soft ? "the effects' cloud veil (the binary "
                    "stamp without)" : "the shadow map", ab_sum[0] / ab_n[0], ab_max[0], ab_n[0],
                    ab_sum[1] / ab_n[1], ab_max[1], ab_n[1], ab_sum[0] / ab_n[0] - ab_sum[1] / ab_n[1], ab_cl[0] / ab_n[0],
                    ab_cl[1] / ab_n[1], ab_cl[0] / ab_n[0] - ab_cl[1] / ab_n[1]);
        else
            ps2_log("clouds A/B: frame GPU with our clouds %.3f ms (max %.3f, %u frames), without %.3f ms (max %.3f, %u frames): "
                    "%+.3f ms", ab_sum[0] / ab_n[0], ab_max[0], ab_n[0], ab_sum[1] / ab_n[1], ab_max[1], ab_n[1],
                    ab_sum[0] / ab_n[0] - ab_sum[1] / ab_n[1]);
        ab_sum[0] = ab_sum[1] = ab_max[0] = ab_max[1] = ab_cl[0] = ab_cl[1] = 0.0; ab_n[0] = ab_n[1] = 0;
    }
}

/* ACZ_VR_CLOUD_FX_SOFT (bug #254): ps2_video.c, at the start of each pass with effects: the veil's inputs are this
   pass's (a cloud point sets them) */
void ps2_clouds_fx_veil_reset(void) { VL.ok = 0; VL.set = VK_NULL_HANDLE; }
int ps2_clouds_fx_soft_on(void) { return G.ok && fx_soft_now(); }
/* the fade's transmittance range for gs.frag: lo .. hi (0 when the veil is off) */
int ps2_clouds_fx_soft_range(float *lo, float *hi) {
    fx_cfg();
    float w = FX.soft > 0.0f ? FX.soft : 0.25f;
    *lo = FX.lim - w; *hi = FX.lim + w;
    if (*hi > 1.0f) *hi = 1.0f;
    if (*hi - *lo < 0.02f) *lo = *hi - 0.02f;
    return VL.ok;
}
/* ps2_video.c's effect depth snapshot with the veil (the rendering closed): over box (x0, x1, y0, y1, target pixels)
   the RGBA16F image dst (dst_view: its layers as the multiview attachment) gets the scene's view depth and our cloud's
   transmittance ramp in front (cloud_comp.frag -DVEIL) from the frame depth (left SHADER_READ_ONLY, *dlayout) and the
   last cloud point's maps. 0 = no veil this pass (no cloud point yet, off, or not multiview): the caller copies the
   depth as before. */
int ps2_clouds_fx_veil(VkCommandBuffer cmd, VkImage depth, VkImageLayout *dlayout, VkImage dst, VkImageView dst_view,
                       VkImageLayout *dst_layout, uint32_t tw, uint32_t th, const int32_t box[4]) {
    if (!G.ok || !VL.ok || !VL.set || !FX.vpipe || !fx_soft_now() || !depth || !dst || !dst_view) return 0;
    int32_t x0 = box[0], x1 = box[1], y0 = box[2], y1 = box[3];
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int32_t)tw) x1 = (int32_t)tw;
    if (y1 > (int32_t)th) y1 = (int32_t)th;
    if (x1 <= x0 || y1 <= y0) return 0;
    VkImageMemoryBarrier2 ib[2];
    uint32_t ni = 0;
    if (*dlayout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        img_barrier(&ib[ni++], depth, VK_IMAGE_ASPECT_DEPTH_BIT, *dlayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, FS, VK_ACCESS_2_SHADER_READ_BIT);
    img_barrier(&ib[ni++], dst, VK_IMAGE_ASPECT_COLOR_BIT, *dst_layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    flush(cmd, ib, ni, NULL, 0);
    *dlayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkRenderingAttachmentInfo ca; memset(&ca, 0, sizeof ca);
    ca.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    ca.imageView = dst_view; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri; memset(&ri, 0, sizeof ri);
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea.offset.x = x0; ri.renderArea.offset.y = y0;
    ri.renderArea.extent.width = (uint32_t)(x1 - x0); ri.renderArea.extent.height = (uint32_t)(y1 - y0);
    ri.layerCount = 1; ri.viewMask = 3u;
    ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
    vkCmdBeginRendering(cmd, &ri);
    VkViewport vp; memset(&vp, 0, sizeof vp);
    vp.width = (float)tw; vp.height = (float)th; vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc;
    sc.offset.x = x0; sc.offset.y = y0; sc.extent.width = (uint32_t)(x1 - x0); sc.extent.height = (uint32_t)(y1 - y0);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, cd_fx(1));
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, G.pipe_layout, 0, 1, &VL.set, 0, NULL);
    vkCmdPushConstants(cmd, G.pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, CL_PUSH_BYTES, VL.pcv);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    img_barrier(&ib[0], dst, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, FS, VK_ACCESS_2_SHADER_READ_BIT);
    flush(cmd, ib, 1, NULL, 0);
    *dst_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VL.veils++; VL.px += (uint64_t)(x1 - x0) * (uint64_t)(y1 - y0);
    return 1;
}
int ps2_clouds_far_point(const ps2_cloud_pass *p) { return cfg.far_on ? split_point(p, 0) : 0; }
int ps2_clouds_near_point(const ps2_cloud_pass *p) { return cfg.near_on ? split_point(p, 1) : 0; }

static float near_in_cloud = -1.0f;
static uint32_t near_in_cloud_frame;
void ps2_clouds_report_in_cloud(float v) { near_in_cloud = v; near_in_cloud_frame = G.frame; }
float ps2_clouds_in_cloud(void) {
    if (!G.ok) return -1.0f;
    float v = (G.env.valid & CL_ENV_INCLOUD) ? G.env.in_cloud : -1.0f;
    if (near_in_cloud >= 0.0f && G.frame - near_in_cloud_frame < 30u && near_in_cloud > v) v = near_in_cloud;
    return v;
}
