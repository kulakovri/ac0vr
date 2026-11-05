// ps2_clouds_shadow.c - the cloud shadow map (ACZ_VR_CLOUD_SHADOW, 2026-10-21; references: Nubis Evolved / Nubis^3,
// engine-style cloud shadows): a small sun-aligned deep opacity map of our cloud layer over the 20 km weather
// window, from the same weather window the far and near passes march (cloud_shadow.comp; the map's layout and the
// lookup are documented in runtime/shaders/cloud_shadow.glsl). The far and near passes shade with it (cloud_common.glsl
// cl_above_dm: the "cloud above" probe's local sample times the map's transmittance from there toward the light, so
// bases darken with the whole column and overhangs and clouds behind clouds shadow each other over kilometres), the
// cockpit gets the sun's transmittance at the aircraft (ps2_clouds_sun_at_eye), and terrain / fog can sample the same
// map (ps2_clouds_shadow_hook + cloud_shadow.glsl).
//
// Rebuild: the map is toroidal by cloud-frame position (like the weather map), so its texels are world-fixed and only
// the content changes (the list's fades, the band, the sun). A ring of 3 layers: one is built in ACZ_VR_CLOUD_SHADOW_
// SLICES row slices (one slice per command buffer, in-between frames included), and the shown map (its own image,
// set 0 binding 16) = the previous complete layer blended toward the newest one by the progress of the current build,
// so content changes ramp over ~2 rebuilds and never pop. Slice and blend are one dispatch (the blend reads only
// complete layers; a finished layer joins the blend from the next frame). The first frame, a rebase of the cloud frame
// or a switch-on build the whole map at once (shown from the next frame). The frame-time governor's level 1
// (ps2_clouds_far_gov) halves the slice: an invisible shed (the rebuild takes twice as long).
//
// Cockpit: the first thread of each dispatch reads the shown state at the centre eye into a host-visible slot per
// frame in flight; the slot is read when the frame index comes round again (its fence has passed) and smoothed here.
#include "ps2_runtime.h"
#include "ps2_clouds.h"
#include "ps2_vkcen.h"          /* (PS2_VK_CENSUS: vkCmd* calls counted) */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SH_FIF 4u                 /* frame indices handed to the hooks (ps2_clouds.c CL_FIF) */
#define SH_LAYERS 3u
#define SH_MIN_ELEV 0.139f        /* sin 8 deg: the light ray's elevation floor (a low sun stretches the shadows) */
#define SH_UBO 256u               /* per frame slot: ShBands (>= minUniformBufferOffsetAlignment on this GPU) */

static struct {
    int ok, on, avail;            /* resources made; shadows used now (runtime toggle); ACZ_VR_CLOUD_SHADOW at start */
    VkDevice dev;
    uint32_t N;                   /* map size (texels per side over CL_WEATHER_M; a power of two) */
    int slices, steps, gov;
    float k, cockpit, d1f, d2f, fade0, fade1;
    int view;                     /* ACZ_VR_CLOUD_SHADOW_VIEW (debug) */
    float cast, cast_amb;         /* ACZ_VR_CLOUD_SHADOW_CAST, the cast shadow colour's place on the DARK -> LIGHT line */
    VkImage ring, disp; VkDeviceMemory ring_mem, disp_mem;
    VkImageView ring_view, disp_view;
    VkDescriptorSetLayout dsl; VkDescriptorPool pool; VkDescriptorSet set[SH_FIF];
    VkPipelineLayout lay; VkPipeline pipe;
    VkBuffer host_buf; VkDeviceMemory host_mem; uint8_t *host;   /* per slot: probe vec4 at 0, ShBands at 256 */
    uint8_t probe_used[SH_FIF];
    VkQueryPool qp; uint8_t q_used[SH_FIF]; float ts_ns; double q_ms; uint32_t q_n;
    int inited;                   /* images in GENERAL and cleared */
    /* the build state */
    int have_last;                /* l_last holds a complete layer */
    int l_prev, l_last, l_build;  /* ring layers */
    uint32_t row;                 /* next row of l_build */
    uint32_t since;               /* frames since l_last completed (the blend's progress) */
    float band[SH_LAYERS][4];     /* per layer: base, top, D1, D2 */
    float light[SH_LAYERS][3];
    uint32_t rebase; int was_on;
    uint32_t frame_seen;          /* ps2_clouds.c frame counter of the last begin */
    /* this frame's decisions (begin -> ubo -> record) */
    int f_skip, f_full, f_complete, f_shown; uint32_t f_row0, f_rows, f_sl; float f_w;
    /* the eye value */
    float eye_t, eye_s, eye_tau; double eye_time; int eye_have;   /* (eye_tau: ACZ_VR_CLOUD_SHADOW_COCKPIT_S) */
    uint64_t frames, builds;
    int ab_off;                   /* ACZ_VR_CLOUD_SHADOW_AB: this frame records without the map */
    float dump_shd[12];           /* the last parameters (for the dump) */
    uint32_t hook_frame;          /* the frame counter when ps2_clouds_shadow_hook was last called (fragment readers) */
} S;
static struct { VkBuffer buf; VkDeviceMemory mem; uint16_t *ptr; int have; float shd[12]; } SD;   /* ACZ_VR_CLOUD_DUMP */

static float envf(const char *n, float def, float lo, float hi) {
    const char *e = getenv(n);
    if (!e || !*e) return def;
    float v = (float)atof(e);
    return v < lo ? lo : v > hi ? hi : v;
}

int ps2_clouds_shadow_wanted(void) {          /* ACZ_VR_CLOUD_SHADOW (default on with our clouds) */
    const char *e = getenv("ACZ_VR_CLOUD_SHADOW");
    return !(e && *e == '0');
}

static int make_image(VkImage *img, VkDeviceMemory *mem, VkImageView *view, uint32_t n, uint32_t layers,
                      VkPhysicalDevice phys) {
    VkImageCreateInfo ii; memset(&ii, 0, sizeof ii);
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; ii.imageType = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    ii.extent.width = n; ii.extent.height = n; ii.extent.depth = 1; ii.mipLevels = 1; ii.arrayLayers = layers;
    ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
             | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    {   extern uint32_t ps2_clouds_qfam[2], ps2_clouds_qfam_n;   /* (PS2_PERF_CLOUD_ASYNC=2: the shafts read it there) */
        if (ps2_clouds_qfam_n == 2u) { ii.sharingMode = VK_SHARING_MODE_CONCURRENT; ii.queueFamilyIndexCount = 2; ii.pQueueFamilyIndices = ps2_clouds_qfam; } }
    if (vkCreateImage(S.dev, &ii, NULL, img) != VK_SUCCESS) { *img = VK_NULL_HANDLE; return -1; }
    VkMemoryRequirements rq; vkGetImageMemoryRequirements(S.dev, *img, &rq);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    uint32_t mt = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < mp.memoryTypeCount && mt == 0xFFFFFFFFu; i++)
        if ((rq.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) mt = i;
    VkMemoryAllocateInfo ai; memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; ai.allocationSize = rq.size; ai.memoryTypeIndex = mt;
    if (mt == 0xFFFFFFFFu || vkAllocateMemory(S.dev, &ai, NULL, mem) != VK_SUCCESS
        || vkBindImageMemory(S.dev, *img, *mem, 0) != VK_SUCCESS) return -1;
    VkImageViewCreateInfo vi; memset(&vi, 0, sizeof vi);
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; vi.image = *img; vi.format = ii.format;
    vi.viewType = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = layers;
    if (vkCreateImageView(S.dev, &vi, NULL, view) != VK_SUCCESS) { *view = VK_NULL_HANDLE; return -1; }
    return 0;
}

/* Always called when our clouds start (the shown map is bound in every cloud shader's set 0, so it must exist even with
   the shadows off: then it stays cleared and the parameters say "off"). 0 = ok. */
int ps2_clouds_shadow_init(const ps2_cloud_gpu *g) {
    memset(&S, 0, sizeof S);
    S.dev = g->dev;
    S.avail = ps2_clouds_shadow_wanted();
    S.on = S.avail;
    uint32_t want = (uint32_t)envf("ACZ_VR_CLOUD_SHADOW_RES", 256.0f, 64.0f, 1024.0f), n = 64u;
    while (n < 1024u && n * 3u < want * 2u) n *= 2u;  /* the nearest power of two (toroidal wrap by mask) */
    S.N = S.avail ? n : 64u;
    S.slices = (int)envf("ACZ_VR_CLOUD_SHADOW_SLICES", 8.0f, 1.0f, 64.0f);
    S.k = envf("ACZ_VR_CLOUD_SHADOW_K", 0.5f, 0.0f, 20.0f);
    /* the cockpit (bug #56, 2026-10-22): a thick cloud's shadow took 0.7 of the direct sun within ~0.6 s (V4 12:03:19:
       the panel -30 % in 0.5 s, then the cloud light -15 % more): now 0.5 of it, eased over 1.5 s (the eye's own
       adaptation and a cloud's soft edge); ACZ_VR_CLOUD_SHADOW_COCKPIT=0.7 ACZ_VR_CLOUD_SHADOW_COCKPIT_S=0.6 = before */
    S.cockpit = envf("ACZ_VR_CLOUD_SHADOW_COCKPIT", 0.5f, 0.0f, 1.0f);
    S.eye_tau = envf("ACZ_VR_CLOUD_SHADOW_COCKPIT_S", 1.5f, 0.05f, 10.0f);
    S.cast = envf("ACZ_VR_CLOUD_SHADOW_CAST", 0.5f, 0.0f, 1.0f);
    S.cast_amb = 0.3f;
    S.steps = (int)envf("ACZ_VR_CLOUD_SHADOW_STEPS", 32.0f, 32.0f, 128.0f);
    S.view = (int)envf("ACZ_VR_CLOUD_SHADOW_VIEW", 0.0f, 0.0f, 1.0f);
    S.d1f = 0.1f; S.d2f = 0.3f;                   /* D1 / D2 as fractions of the band thickness */
    S.fade0 = 7000.0f; S.fade1 = 8500.0f;         /* the edge fade (m from the eye, L-inf, of the ray's base point) */
    VkDevice dev = g->dev;
    VkDescriptorType t[4] = { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                              VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER };
    if (make_image(&S.ring, &S.ring_mem, &S.ring_view, S.N, SH_LAYERS, g->phys)
        || make_image(&S.disp, &S.disp_mem, &S.disp_view, S.N, 1, g->phys)) goto fail;
    {   VkDescriptorSetLayoutBinding b[4]; memset(b, 0, sizeof b);
        for (uint32_t i = 0; i < 4; i++) {
            b[i].binding = i; b[i].descriptorType = t[i]; b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo lc; memset(&lc, 0, sizeof lc);
        lc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO; lc.bindingCount = 4; lc.pBindings = b;
        if (vkCreateDescriptorSetLayout(dev, &lc, NULL, &S.dsl) != VK_SUCCESS) goto fail;
        VkDescriptorPoolSize ps[3] = { { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * SH_FIF }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, SH_FIF },
                                       { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, SH_FIF } };
        VkDescriptorPoolCreateInfo pc; memset(&pc, 0, sizeof pc);
        pc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO; pc.maxSets = SH_FIF; pc.poolSizeCount = 3; pc.pPoolSizes = ps;
        if (vkCreateDescriptorPool(dev, &pc, NULL, &S.pool) != VK_SUCCESS) goto fail;
        VkDescriptorSetLayout lays[SH_FIF];
        for (uint32_t i = 0; i < SH_FIF; i++) lays[i] = S.dsl;
        VkDescriptorSetAllocateInfo ai; memset(&ai, 0, sizeof ai);
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO; ai.descriptorPool = S.pool; ai.descriptorSetCount = SH_FIF;
        ai.pSetLayouts = lays;
        if (vkAllocateDescriptorSets(dev, &ai, S.set) != VK_SUCCESS) goto fail;
        /* host-cached: the probe slots are read back (the small uniform is written through the same mapping) */
        if (ps2_clouds_make_buffer((VkDeviceSize)SH_UBO * 2u * SH_FIF, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                                   | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 2, &S.host_buf, &S.host_mem, (void **)&S.host) != 0) goto fail;
        memset(S.host, 0, (size_t)SH_UBO * 2u * SH_FIF);
        for (uint32_t f = 0; f < SH_FIF; f++) {
            VkDescriptorImageInfo im[2]; memset(im, 0, sizeof im);
            im[0].imageView = S.ring_view; im[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            im[1].imageView = S.disp_view; im[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            VkDescriptorBufferInfo bp = { S.host_buf, (VkDeviceSize)SH_UBO * 2u * f, 16 };
            VkDescriptorBufferInfo bu = { S.host_buf, (VkDeviceSize)SH_UBO * (2u * f + 1u), 48 };
            VkWriteDescriptorSet w[4]; memset(w, 0, sizeof w);
            for (uint32_t i = 0; i < 4; i++) {
                w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = S.set[f]; w[i].dstBinding = i;
                w[i].descriptorCount = 1; w[i].descriptorType = t[i];
            }
            w[0].pImageInfo = &im[0]; w[1].pImageInfo = &im[1]; w[2].pBufferInfo = &bp; w[3].pBufferInfo = &bu;
            vkUpdateDescriptorSets(dev, 4, w, 0, NULL);
        }
    }
    if (S.avail) {
        S.pipe = ps2_clouds_make_compute("cloud_shadow.comp.spv", S.dsl, &S.lay);
        if (!S.pipe) goto fail;
        VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(g->phys, &pp);
        if (pp.limits.minUniformBufferOffsetAlignment > SH_UBO) goto fail;
        S.ts_ns = pp.limits.timestampPeriod;
        if (ps2_clouds_cfg()->debug > 0) {        /* own timestamps only with ACZ_DEBUG_CLOUDS (the A/B uses the pass's) */
            VkQueryPoolCreateInfo qc; memset(&qc, 0, sizeof qc);
            qc.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO; qc.queryType = VK_QUERY_TYPE_TIMESTAMP; qc.queryCount = 2u * SH_FIF;
            if (vkCreateQueryPool(dev, &qc, NULL, &S.qp) != VK_SUCCESS) S.qp = VK_NULL_HANDLE;
        }
    }
    S.l_prev = 0; S.l_last = 1; S.l_build = 2;
    S.ok = 1;
    if (S.avail)
        ps2_log("clouds: shadow map %ux%u over %.0f km (%.0f m texels, %.1f MB), rebuilt in %d slices, %d samples a ray, "
                "strength %.2f, cockpit %.2f", S.N, S.N, CL_WEATHER_M / 1000.0, CL_WEATHER_M / S.N,
                (double)S.N * S.N * 8.0 * (SH_LAYERS + 1) / 1048576.0, S.slices, (S.steps + 31) / 32 * 32, (double)S.k,
                (double)S.cockpit);
    else ps2_log("clouds: shadow map off (ACZ_VR_CLOUD_SHADOW=0: the \"cloud above\" probe as before)");
    return 0;
fail:
    ps2_log("clouds: shadow map initialisation failed -- shadows off");
    S.avail = S.on = 0;
    if (S.disp_view && S.dsl) { S.ok = 1; return 0; }    /* the cleared map stays bound (the probe alone) */
    return -1;
}

void ps2_clouds_shadow_shutdown(const ps2_cloud_gpu *g) {
    VkDevice dev = g->dev;
    if (!S.dev) return;
    if (S.pipe) vkDestroyPipeline(dev, S.pipe, NULL);
    if (S.lay) vkDestroyPipelineLayout(dev, S.lay, NULL);
    if (S.qp) vkDestroyQueryPool(dev, S.qp, NULL);
    if (S.pool) vkDestroyDescriptorPool(dev, S.pool, NULL);
    if (S.dsl) vkDestroyDescriptorSetLayout(dev, S.dsl, NULL);
    if (S.host_buf) { vkDestroyBuffer(dev, S.host_buf, NULL); vkFreeMemory(dev, S.host_mem, NULL); }
    if (S.ring_view) vkDestroyImageView(dev, S.ring_view, NULL);
    if (S.disp_view) vkDestroyImageView(dev, S.disp_view, NULL);
    if (S.ring) vkDestroyImage(dev, S.ring, NULL);
    if (S.disp) vkDestroyImage(dev, S.disp, NULL);
    if (S.ring_mem) vkFreeMemory(dev, S.ring_mem, NULL);
    if (S.disp_mem) vkFreeMemory(dev, S.disp_mem, NULL);
    if (SD.buf) { vkDestroyBuffer(dev, SD.buf, NULL); vkFreeMemory(dev, SD.mem, NULL); }
    memset(&SD, 0, sizeof SD);
    memset(&S, 0, sizeof S);
}

VkImageView ps2_clouds_shadow_view(void) { return S.disp_view; }
/* the shown map's image and size (ps2_video.c, cloud shadows on the ground: a copy read back for the game's tree
   sprites, drawn on the GS path); VK_NULL_HANDLE without the module */
VkImage ps2_clouds_shadow_image(uint32_t *n) { if (n) *n = S.N; return S.ok ? S.disp : VK_NULL_HANDLE; }
/* a fragment shader outside the clouds read the map recently (ps2_clouds_shadow_hook within the last 8 frames): the
   barrier after the shadow pass then also covers fragment reads (else only compute: the game's draws early in the
   command buffer need not wait for it) */
int ps2_clouds_shadow_frag(void) { return S.hook_frame && S.frame_seen - S.hook_frame < 8u; }
int ps2_clouds_shadow_toggle(int on) {
    if (!S.ok || !S.avail) return -1;
    if (on >= 0) S.on = on != 0;
    return S.on;
}
void ps2_clouds_shadow_gov(int on) { S.gov = on != 0; }
void ps2_clouds_shadow_ab(int off) { S.ab_off = off != 0; }
static int active(void) { return S.ok && S.avail && S.on && S.pipe && !S.ab_off; }

/* the light direction for the shadows: the sun; at night (C1 art_k t_b < 0) the game lights its clouds from the side
   opposite the disc (cl_light_side), so x/z flip; the elevation is kept >= 8 degrees */
static void light_dir(const ps2_cloud_env *e, float L[3]) {
    float side = e->art_k[1] < 0.0f ? -1.0f : 1.0f;
    float x = e->sun_dir[0] * side, z = e->sun_dir[2] * side, y = fabsf(e->sun_dir[1]);
    float h = sqrtf(x * x + z * z);
    if (h < 1e-6f) { x = 0.0f; z = 0.0f; y = 1.0f; }
    else if (y < SH_MIN_ELEV * sqrtf(x * x + y * y + z * z)) {
        float c = sqrtf(1.0f - SH_MIN_ELEV * SH_MIN_ELEV);
        x = x / h * c; z = z / h * c; y = SH_MIN_ELEV;
    }
    float l = sqrtf(x * x + y * y + z * z);
    L[0] = x / l; L[1] = y / l; L[2] = z / l;
}
static void band_of(const ps2_cloud_env *e, float b[4]) {
    float base = e->base_m, top = e->top_m;
    if (top < base + 50.0f) top = base + 50.0f;
    b[0] = base; b[1] = top; b[2] = S.d1f * (top - base); b[3] = S.d2f * (top - base);
}

/* once per command buffer, before the CloudFrame copies are filled: this frame's slice and blend state; and the eye
   value of this frame slot's last use */
void ps2_clouds_shadow_begin(const ps2_cloud_env *e, const ps2_cloud_view *v, int fi, uint32_t frame) {
    if (!S.ok || frame == S.frame_seen) return;
    S.frame_seen = frame;
    S.f_skip = 1; S.f_full = 0; S.f_complete = 0; S.f_shown = 0;
    if (fi >= 0 && fi < (int)SH_FIF) {
        if (S.probe_used[fi]) {                     /* the eye value of this slot's last frame (its fence has passed) */
            float t = *(const float *)(S.host + (size_t)SH_UBO * 2u * (uint32_t)fi);
            S.eye_t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
            S.probe_used[fi] = 0;
            double dt = S.eye_have ? v->time_s - S.eye_time : 0.0;
            if (dt < 0.0) dt = 0.0;
            if (dt > 1.0) dt = 1.0;
            S.eye_s = S.eye_have ? S.eye_s + (S.eye_t - S.eye_s) * (float)(1.0 - exp(-dt / (double)S.eye_tau)) : S.eye_t;
            S.eye_time = v->time_s; S.eye_have = 1;
        }
        if (S.qp && S.q_used[fi]) {
            uint64_t ts[2];
            if (vkGetQueryPoolResults(S.dev, S.qp, 2u * (uint32_t)fi, 2, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
                S.q_ms += (double)(ts[1] - ts[0]) * S.ts_ns * 1e-6; S.q_n++;
            }
            S.q_used[fi] = 0;
        }
    }
    if (!active()) {
        if (!S.on || !S.avail) { S.have_last = 0; S.eye_have = 0; S.eye_s = 1.0f; S.was_on = 0; }   /* a full build on return */
        return;                                     /* (an A/B off frame keeps the state) */
    }
    S.f_skip = 0;
    S.frames++;
    int full = !S.have_last || S.rebase != v->rebase || !S.was_on;
    if (S.rebase != v->rebase) S.have_last = 0;     /* (the cloud frame jumped: the old content is not shown) */
    S.was_on = 1; S.rebase = v->rebase;
    uint32_t sl = (uint32_t)S.slices * (S.gov ? 2u : 1u);
    uint32_t rows = S.N / sl;
    if (rows < 1u) rows = 1u;
    S.f_sl = sl;
    if (full) {                                     /* the whole layer now, shown from the next frame */
        S.f_full = 1; S.f_complete = 1;
        band_of(e, S.band[S.l_build]); light_dir(e, S.light[S.l_build]);
        S.f_row0 = 0; S.f_rows = S.N; S.row = 0;
    } else {
        if (S.row == 0) { band_of(e, S.band[S.l_build]); light_dir(e, S.light[S.l_build]); }
        S.f_row0 = S.row; S.f_rows = S.row + rows > S.N ? S.N - S.row : rows;
        S.row += S.f_rows;
        if (S.row >= S.N) { S.row = 0; S.f_complete = 1; }
    }
    if (S.since < (1u << 20)) S.since++;
    float w = (float)S.since / (float)sl;
    S.f_w = w > 1.0f ? 1.0f : w;
    S.f_shown = S.have_last;
}

/* the CloudFrame parameters of the shown map for this frame: cloud_shadow.glsl s0, s1, s2 and the cast shadow's
   strength / colour (cloud_common.glsl cl_shade_s) */
void ps2_clouds_shadow_ubo(float shd[16], const ps2_cloud_view *v, const ps2_cloud_env *e) {
    memset(shd, 0, 16 * sizeof(float));
    (void)e;
    if (!S.ok || !active() || S.f_skip || !S.f_shown) return;   /* s0.w = 0: no map (the probe alone) */
    double wu = fmod(v->cloud_origin_m[0] / CL_WEATHER_M, 1.0), wv = fmod(v->cloud_origin_m[2] / CL_WEATHER_M, 1.0);
    shd[0] = (float)(wu < 0.0 ? wu + 1.0 : wu); shd[1] = (float)(wv < 0.0 ? wv + 1.0 : wv);
    shd[2] = 1.0f / CL_WEATHER_M; shd[3] = S.k;
    const float *L = S.light[S.l_last], *b = S.band[S.l_last];
    shd[4] = L[0] / L[1]; shd[5] = L[2] / L[1]; shd[6] = b[0]; shd[7] = (float)v->cloud_origin_m[1];
    shd[8] = b[2]; shd[9] = b[3]; shd[10] = S.fade0; shd[11] = 1.0f / (S.fade1 - S.fade0);
    if (S.view) shd[11] = -shd[11];                 /* ACZ_VR_CLOUD_SHADOW_VIEW: the shading inputs as grey (debug) */
    shd[12] = S.cast; shd[13] = S.cast_amb;
    memcpy(S.dump_shd, shd, sizeof S.dump_shd);   /* (s0..s2) */
}

static void barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da) {
    VkMemoryBarrier2 mb; memset(&mb, 0, sizeof mb);
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    mb.srcStageMask = ss; mb.srcAccessMask = sa; mb.dstStageMask = ds; mb.dstAccessMask = da;
    VkDependencyInfo di; memset(&di, 0, sizeof di);
    di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO; di.memoryBarrierCount = 1; di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &di);
}
#define CS VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
#define RW (VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT)
typedef struct { int32_t i0[4], i1[4]; float f0[4], f1[4]; } sh_push;
_Static_assert(sizeof(sh_push) <= CL_PUSH_BYTES, "shadow push constants");

/* the CloudFrame copy and set 0 are bound (compute); records the slice and the blend (one dispatch). 1 = something
   was written: the caller orders it before the far / near passes and fragment readers */
int ps2_clouds_shadow_record(VkCommandBuffer cmd, const ps2_cloud_rec *r) {
    if (!S.ok) return 0;
    int fi = r->frame_index;
    if (!S.inited) {                                /* GENERAL layouts, cleared (no shadow) */
        VkImageMemoryBarrier2 ib[2]; memset(ib, 0, sizeof ib);
        VkImage im[2] = { S.ring, S.disp };
        for (int i = 0; i < 2; i++) {
            ib[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            ib[i].srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
            ib[i].dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT | CS;
            ib[i].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT | RW;
            ib[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; ib[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            ib[i].srcQueueFamilyIndex = ib[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            ib[i].image = im[i]; ib[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            ib[i].subresourceRange.levelCount = 1; ib[i].subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
        }
        VkDependencyInfo di; memset(&di, 0, sizeof di);
        di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO; di.imageMemoryBarrierCount = 2; di.pImageMemoryBarriers = ib;
        vkCmdPipelineBarrier2(cmd, &di);
        VkClearColorValue c; memset(&c, 0, sizeof c);
        VkImageSubresourceRange rr; memset(&rr, 0, sizeof rr);
        rr.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; rr.levelCount = 1; rr.layerCount = VK_REMAINING_ARRAY_LAYERS;
        vkCmdClearColorImage(cmd, S.ring, VK_IMAGE_LAYOUT_GENERAL, &c, 1, &rr);
        vkCmdClearColorImage(cmd, S.disp, VK_IMAGE_LAYOUT_GENERAL, &c, 1, &rr);
        barrier(cmd, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, CS | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, RW);
        S.inited = 1;
    }
    if (S.f_skip || !S.pipe || fi < 0 || fi >= (int)SH_FIF) return 0;
    if (S.qp) {
        vkCmdResetQueryPool(cmd, S.qp, 2u * (uint32_t)fi, 2);
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, S.qp, 2u * (uint32_t)fi);
    }
    const float *b = S.band[S.l_build], *L = S.light[S.l_build];
    int spt = (S.steps + 31) / 32;
    if (spt > 4) spt = 4;
    /* this slot's small uniform: the blend's layer bands, the weather limit */
    float *u = (float *)(S.host + (size_t)SH_UBO * (2u * (uint32_t)fi + 1u));
    memset(u, 0, 48);
    u[0] = S.band[S.l_prev][0]; u[1] = S.band[S.l_prev][2]; u[2] = S.band[S.l_prev][3];
    u[4] = S.band[S.l_last][0]; u[5] = S.band[S.l_last][2]; u[6] = S.band[S.l_last][3];
    u[8] = 0.5f * CL_WEATHER_M - 500.0f;
    uint32_t nb = (S.N / 2u) * S.f_rows;              /* build groups: two rays (texels) each */
    uint32_t nbl = S.have_last ? S.N * S.N / 64u : 0u;   /* blend groups: 64 texels each */
    if (S.gov && (S.frames & 1u)) nbl = 0u;          /* governor: the (twice as slow) crossfade every other frame */
    sh_push pc; memset(&pc, 0, sizeof pc);
    pc.i0[0] = S.l_build; pc.i0[1] = (int32_t)S.f_row0; pc.i0[2] = (int32_t)S.f_rows; pc.i0[3] = (int32_t)nb;
    pc.i1[0] = spt; pc.i1[1] = S.l_prev; pc.i1[2] = S.l_last; pc.i1[3] = S.have_last ? fi : -1;
    pc.f0[0] = L[0]; pc.f0[1] = L[1]; pc.f0[2] = L[2]; pc.f0[3] = S.f_w;
    pc.f1[0] = b[0]; pc.f1[1] = b[1]; pc.f1[2] = b[2]; pc.f1[3] = b[3];
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, S.pipe);
    ps2_clouds_bind_common(cmd, S.lay);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, S.lay, 1, 1, &S.set[fi], 0, NULL);
    vkCmdPushConstants(cmd, S.lay, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(cmd, nb + nbl, 1, 1);
    if (S.have_last) S.probe_used[fi] = 1;
    /* (the caller's barrier orders it before the far / near passes and makes the probe slot visible to the host) */
    if (S.qp) {
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, S.qp, 2u * (uint32_t)fi + 1u);
        S.q_used[fi] = 1;
    }
    /* a finished layer joins the blend from the next frame */
    if (S.f_complete) {
        int old_prev = S.l_prev;
        if (S.f_full) { S.l_prev = S.l_last; S.l_last = S.l_build; S.l_build = old_prev; S.since = 1u << 20; }
        else { S.l_prev = S.l_last; S.l_last = S.l_build; S.l_build = old_prev; S.since = 0; }
        S.have_last = 1;
        S.builds++;
    }
    return 1;
}

/* the sun's transmittance through our clouds at the aircraft (the centre eye), smoothed over ACZ_VR_CLOUD_SHADOW_COCKPIT_S
   (1.5 s); 1 = full sun
   (also with the shadows or our clouds off) */
float ps2_clouds_sun_at_eye(void) {
    if (!S.ok || !S.avail || !S.on || !S.eye_have) return 1.0f;
    return S.eye_s;
}
float ps2_clouds_shadow_cockpit_k(void) { return S.cockpit; }

/* ACZ_VR_CLOUD_DUMP (see ps2_clouds.h) */
void ps2_clouds_shadow_dump_copy(VkCommandBuffer cmd) {
    if (!S.ok || !S.avail || SD.have) return;
    VkDeviceSize sz = (VkDeviceSize)S.N * S.N * 8u;
    if (!SD.buf && ps2_clouds_make_buffer(sz, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 2, &SD.buf, &SD.mem, (void **)&SD.ptr) != 0) {
        SD.buf = VK_NULL_HANDLE; return;
    }
    VkBufferImageCopy rg; memset(&rg, 0, sizeof rg);
    rg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; rg.imageSubresource.layerCount = 1;
    rg.imageExtent.width = S.N; rg.imageExtent.height = S.N; rg.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cmd, S.disp, VK_IMAGE_LAYOUT_GENERAL, SD.buf, 1, &rg);
    SD.have = 1;
    memcpy(SD.shd, S.dump_shd, sizeof SD.shd);
}
static float h2f(uint16_t h) {
    uint32_t s = (uint32_t)(h >> 15) << 31, e = (h >> 10) & 31u, m = h & 1023u, f;
    if (e == 0u) { if (!m) f = s; else { e = 113u; while (!(m & 1024u)) { m <<= 1; e--; } m &= 1023u; f = s | (e << 23) | (m << 13); } }
    else if (e == 31u) f = s | 0x7F800000u | (m << 13);
    else f = s | ((e + 112u) << 23) | (m << 13);
    float r; memcpy(&r, &f, sizeof r); return r;
}
void ps2_clouds_shadow_dump_write(const char *base) {
    if (SD.have != 1 || !SD.ptr) return;
    SD.have = 2;
    char fn[400];
    uint32_t n = S.N;
    snprintf(fn, sizeof fn, "%s_shadow.txt", base);
    FILE *f = fopen(fn, "w");
    if (f) {
        fprintf(f, "cloud shadow map %ux%u over %.0f m; s0 %g %g %g %g s1 %g %g %g %g s2 %g %g %g %g\n"
                "texel (i, j): uv = (i + 0.5, j + 0.5) / N; R top altitude (m), G OD at D1, B OD at D2, A column OD "
                "(unscaled; strength s0.w)\n", n, n, CL_WEATHER_M, SD.shd[0], SD.shd[1], SD.shd[2], SD.shd[3], SD.shd[4],
                SD.shd[5], SD.shd[6], SD.shd[7], SD.shd[8], SD.shd[9], SD.shd[10], SD.shd[11]);
        fclose(f);
    }
    for (int c = 0; c < 4; c++) {                   /* each channel as a PFM grey (rows bottom to top) */
        static const char *const tag[4] = { "top", "od1", "od2", "odcol" };
        snprintf(fn, sizeof fn, "%s_shadow_%s.pfm", base, tag[c]);
        f = fopen(fn, "wb");
        if (!f) continue;
        fprintf(f, "Pf\n%u %u\n-1.0\n", n, n);
        for (uint32_t y = n; y-- > 0;)
            for (uint32_t x = 0; x < n; x++) { float v = h2f(SD.ptr[((size_t)y * n + x) * 4u + (size_t)c]); fwrite(&v, 4, 1, f); }
        fclose(f);
    }
    snprintf(fn, sizeof fn, "%s_shadowT.pgm", base);   /* the ground's sun transmittance (column OD x strength) */
    f = fopen(fn, "wb");
    if (f) {
        fprintf(f, "P5\n%u %u\n255\n", n, n);
        for (size_t i = 0; i < (size_t)n * n; i++) {
            float od = h2f(SD.ptr[i * 4u + 3u]) * SD.shd[3];
            float t = expf(-od), t2 = 0.7f * expf(-0.25f * od);
            t = t > t2 ? t : t2;
            uint8_t v = (uint8_t)(t <= 0.0f ? 0 : t >= 1.0f ? 255 : t * 255.0f + 0.5f);
            fwrite(&v, 1, 1, f);
        }
        fclose(f);
    }
}

/* ACZ_DEBUG_CLOUDS line (ps2_clouds.c debug_log) */
void ps2_clouds_shadow_debug(void) {
    if (!S.ok || !S.avail) return;
    ps2_log("clouds shadow: %s, %llu frames, %llu rebuilds, layer %d (prev %d) blend %.2f, band %.0f-%.0f m, light %.2f %.2f "
            "%.2f, eye T %.3f (smoothed %.3f), GPU %.4f ms per frame (%u)%s", S.on ? "on" : "off",
            (unsigned long long)S.frames, (unsigned long long)S.builds, S.l_last, S.l_prev, (double)S.f_w,
            (double)S.band[S.l_last][0], (double)S.band[S.l_last][1], (double)S.light[S.l_last][0],
            (double)S.light[S.l_last][1], (double)S.light[S.l_last][2], (double)S.eye_t, (double)S.eye_s,
            S.q_n ? S.q_ms / S.q_n : 0.0, S.q_n, S.gov ? ", governor: half slices" : "");
    S.q_ms = 0.0; S.q_n = 0;
}

/* The hook for other renderers (terrain, fog; see ps2_clouds.h): the shown map's view and this frame's parameters
   (cloud_shadow.glsl s0, s1, s2) for a view whose centre eye is v. Returns 0 when there is no map (s0.w = 0). */
int ps2_clouds_shadow_hook(const ps2_cloud_view *v, float params[12], VkImageView *view) {
    ps2_cloud_env e; memset(&e, 0, sizeof e);
    ps2_cloud_view cv = *v;
    float p16[16];
    ps2_cloud_env_read(&e);
    cv.cloud_origin_m[0] = v->origin_m[0] - (double)e.drift_m[0];
    cv.cloud_origin_m[1] = v->origin_m[1];
    cv.cloud_origin_m[2] = v->origin_m[2] - (double)e.drift_m[1];
    ps2_clouds_shadow_ubo(p16, &cv, &e);
    S.hook_frame = S.frame_seen ? S.frame_seen : 1u;
    memcpy(params, p16, 12 * sizeof(float));
    params[11] = fabsf(params[11]);                 /* (no debug view outside the clouds) */
    if (view) *view = S.disp_view;
    return params[3] > 0.0f;
}
