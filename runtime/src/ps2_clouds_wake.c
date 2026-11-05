// ps2_clouds_wake.c - the cloud wake's probe (ACZ_VR_CLOUD_WAKE; agent cl_wake 2026-10-29). zero_trails.c (EE
// thread) asks once per game frame for our clouds' density around the aircraft it follows (pt_wake_request: up to 32,
// each where it will be when the answer comes back); here, on the renderer thread, a game frame's command buffer
// evaluates them early (cloud_wake.comp, next to the optics probe: one thread per corner of a cube about each point,
// the shared density cl_density with its detail octave, as the near volume marches it) into this frame slot's part of
// a host-visible buffer, and when the slot comes round again (its fence has passed) the means go back
// (pt_wake_result). Cost: up to 256 density samples a game frame (~0.002 ms), nothing when no aircraft asks.
#include "ps2_runtime.h"
#include "ps2_clouds.h"
#include "ps2_vkcen.h"          /* (PS2_VK_CENSUS: vkCmd* calls counted) */
#include "ps2_trails.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WK_FIF 4u                 /* frame indices handed to the hooks (ps2_clouds.c CL_FIF) */
#define WK_SLOT_F 384u            /* floats per frame slot: 32 points x 4, then 256 answers */

static struct {
    int ok, on;
    VkDevice dev;
    VkDescriptorSetLayout dsl; VkDescriptorPool pool; VkDescriptorSet set;
    VkPipelineLayout lay; VkPipeline pipe;
    VkBuffer buf; VkDeviceMemory mem; float *map;
    uint8_t used[WK_FIF];
    uint32_t n[WK_FIF];
    pt_wake_req req[WK_FIF][PT_WAKE_MAX];
    uint32_t frame_seen, rec_frame;
    uint64_t dispatched, answered, points;
    int dbg;
} W;

int ps2_clouds_wake_init(const ps2_cloud_gpu *g) {
    const pt_cfg_t *cf = pt_cfg();
    memset(&W, 0, sizeof W);
    W.dev = g->dev;
    { const char *e = getenv("ACZ_DEBUG_CLOUD_WAKE"); W.dbg = e ? atoi(e) : 0; }
    if (!(cf->wake > 0.0f)) return 0;
    VkDevice dev = g->dev;
    VkDescriptorSetLayoutBinding b; memset(&b, 0, sizeof b);
    b.binding = 0; b.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo lc; memset(&lc, 0, sizeof lc);
    lc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO; lc.bindingCount = 1; lc.pBindings = &b;
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
    VkDescriptorPoolCreateInfo pc; memset(&pc, 0, sizeof pc);
    pc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO; pc.maxSets = 1; pc.poolSizeCount = 1; pc.pPoolSizes = &ps;
    VkDeviceSize sz = (VkDeviceSize)WK_FIF * WK_SLOT_F * sizeof(float);
    if (vkCreateDescriptorSetLayout(dev, &lc, NULL, &W.dsl) != VK_SUCCESS
        || vkCreateDescriptorPool(dev, &pc, NULL, &W.pool) != VK_SUCCESS
        || ps2_clouds_make_buffer(sz, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 2, &W.buf, &W.mem, (void **)&W.map) != 0)
        goto fail;
    {   VkDescriptorSetAllocateInfo ai; memset(&ai, 0, sizeof ai);
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO; ai.descriptorPool = W.pool;
        ai.descriptorSetCount = 1; ai.pSetLayouts = &W.dsl;
        if (vkAllocateDescriptorSets(dev, &ai, &W.set) != VK_SUCCESS) goto fail;
        VkDescriptorBufferInfo bi = { W.buf, 0, sz };
        VkWriteDescriptorSet w; memset(&w, 0, sizeof w);
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet = W.set; w.dstBinding = 0; w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w.pBufferInfo = &bi;
        vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
        memset(W.map, 0, (size_t)sz);
    }
    W.pipe = ps2_clouds_make_compute("cloud_wake.comp.spv", W.dsl, &W.lay);
    if (!W.pipe) goto fail;
    W.ok = W.on = 1;
    ps2_log("clouds: cloud wake probe ready (ACZ_VR_CLOUD_WAKE %.2f: our clouds' density about up to %u aircraft a game "
            "frame)", (double)cf->wake, PT_WAKE_MAX);
    return 0;
fail:
    ps2_log("clouds: cloud wake probe unavailable -- no cloud wake");
    ps2_clouds_wake_shutdown(g);
    return 0;
}

void ps2_clouds_wake_shutdown(const ps2_cloud_gpu *g) {
    VkDevice dev = g ? g->dev : W.dev;
    if (!dev) return;
    if (W.dispatched)
        ps2_log("clouds: cloud wake probe: %llu dispatches, %llu points, %llu answers", (unsigned long long)W.dispatched,
                (unsigned long long)W.points, (unsigned long long)W.answered);
    if (W.pipe) vkDestroyPipeline(dev, W.pipe, NULL);
    if (W.lay) vkDestroyPipelineLayout(dev, W.lay, NULL);
    if (W.pool) vkDestroyDescriptorPool(dev, W.pool, NULL);
    if (W.dsl) vkDestroyDescriptorSetLayout(dev, W.dsl, NULL);
    if (W.buf) { vkDestroyBuffer(dev, W.buf, NULL); vkFreeMemory(dev, W.mem, NULL); }
    memset(&W, 0, sizeof W);
}

/* once per command buffer: the answers of this slot's last probe (its fence has passed) */
void ps2_clouds_wake_begin(int fi, uint32_t frame) {
    if (!W.ok || frame == W.frame_seen) return;
    W.frame_seen = frame;
    if (fi < 0 || fi >= (int)WK_FIF || !W.used[fi]) return;
    const float *a = W.map + (size_t)fi * WK_SLOT_F + 128u;
    for (uint32_t k = 0; k < W.n[fi]; k++) {
        float s = 0.0f, m = 0.0f;
        for (uint32_t c = 0; c < 8u; c++) {
            float d = a[8u * k + c];
            if (!(d >= 0.0f)) d = 0.0f;
            s += d;
            if (d > m) m = d;
        }
        pt_wake_result(W.req[fi][k].key, W.req[fi][k].field, s * 0.125f, m);
        W.answered++;
        if (W.dbg > 0 && k == 0u && (W.answered % (uint64_t)W.dbg) == 0u)
            ps2_log("cloud-wake: probe answer field %u key %08X mean %.3f max %.3f (%u points)", W.req[fi][k].field,
                    W.req[fi][k].key, (double)(s * 0.125f), (double)m, W.n[fi]);
    }
    W.used[fi] = 0;
}

typedef struct { int32_t i0[4], i1[4]; float f0[4], f1[4]; } wk_push;
_Static_assert(sizeof(wk_push) <= CL_PUSH_BYTES, "cloud wake push constants");
/* the probe, once per game frame's command buffer (the common set is bound); own_barrier: make the answers visible to
   the host here (else the caller's next barrier does) */
int ps2_clouds_wake_record(VkCommandBuffer cmd, const ps2_cloud_rec *r, int own_barrier) {
    if (!W.ok || !W.on || !r || r->frame_index < 0 || r->frame_index >= (int)WK_FIF || r->view->inbetween) return 0;
    if (r->frame == W.rec_frame) return 0;
    int fi = r->frame_index;
    pt_wake_req q[PT_WAKE_MAX];
    uint32_t n = pt_wake_requests(q, PT_WAKE_MAX, 0.25);
    if (!n) return 0;
    const ps2_cloud_view *v = r->view;
    double upm = v->units_per_m > 0.5f ? (double)v->units_per_m : 6.0;
    float *in = W.map + (size_t)fi * WK_SLOT_F;
    uint32_t m = 0;
    for (uint32_t k = 0; k < n; k++) {
        double x = (double)q[k].p[0] / upm - v->origin_m[0], y = (double)q[k].p[1] / upm - v->origin_m[1],
               z = (double)q[k].p[2] / upm - v->origin_m[2];
        if (!(fabs(x) < 9000.0 && fabs(y) < 9000.0 && fabs(z) < 9000.0)) continue;   /* (inside the weather window) */
        in[4u * m] = (float)x; in[4u * m + 1u] = (float)y; in[4u * m + 2u] = (float)z; in[4u * m + 3u] = q[k].r_m;
        W.req[fi][m] = q[k];
        m++;
    }
    W.n[fi] = m;
    if (!m) return 0;
    wk_push pc; memset(&pc, 0, sizeof pc);
    pc.i0[0] = fi; pc.i0[1] = (int32_t)m;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, W.pipe);
    ps2_clouds_bind_common(cmd, W.lay);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, W.lay, 1, 1, &W.set, 0, NULL);
    vkCmdPushConstants(cmd, W.lay, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(cmd, (8u * m + 63u) / 64u, 1, 1);
    W.rec_frame = r->frame;
    W.used[fi] = 1;
    W.dispatched++;
    W.points += m;
    if (!own_barrier) return 1;
    VkMemoryBarrier2 mb; memset(&mb, 0, sizeof mb);
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; mb.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT; mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo di; memset(&di, 0, sizeof di);
    di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO; di.memoryBarrierCount = 1; di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &di);
    return 1;
}
