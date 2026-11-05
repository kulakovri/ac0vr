// ps2_clouds_optics.c - optical phenomena as rare rewards (ACZ_VR_OPTICS, bug #30, 2026-10-21): the 22 deg halo
// (+ 46 deg, parhelia) through a thin ice veil, the glory around the antisolar point on cloud tops below, the Brocken
// spectre (the aircraft's shadow on them) and the rainbow in sunlit rain. The lobes are evaluated per pixel by the
// cloud composite that runs anyway (runtime/shaders/cloud_optics.glsl, which documents the physics and CloudFrame
// opt[]); this module decides, once per command buffer, whether each phenomenon is possible and how strong it is:
//   - the probe (cloud_optics.comp, one workgroup at the far point): the cloud surface along the antisolar ray (its
//     distance D, opacity, sunlight from the cloud shadow map, near or far), the sun's transmittance from the eye
//     through the near volume and the far map; read back when the frame slot comes round (its fence has passed);
//   - the gates: day (night: none; ACZ_VR_OPTICS_MOON=1 a dim lunar halo), not inside cloud, halo: the sun visible
//     through the clouds (thick = none), the ice level from the freezing level (the shader weighs the pixel's own
//     optical depth and altitude); with ACZ_VR_OPTICS_HALO_PHYS (bug #72) the halo needs halo-forming ice: cloud at
//     -20 C and colder (full at -35 C, the standard lapse rate from the freezing level), so the band must reach that
//     level at all, a layer cloud rather than cumulus (convective clouds stay supercooled water to ~-20 C), and per
//     pixel the sun's own light on the ice (the cloud shadow map at the pixel's cloud); glory / spectre: a sunlit
//     opaque surface below the aircraft; rainbow: rain (C1
//     CL_ENV_PRECIP, not snow) and the sun below ACZ_VR_OPTICS_BOW_ELEV (42 deg); every gate fades over ~1 s;
//   - the spectre's silhouette: five parallelograms of a generic fighter planform around the cockpit, projected
//     along the sun ray from the aircraft's attitude (the flight model's angles, zero_instr) into the shadow plane.
// Everything is world-fixed (directions, the aircraft's own attitude), identical for both eyes except the per-eye
// centre of the spectre/glory on the surface; no screen-space history.
#include "ps2_runtime.h"
#include "ps2_clouds.h"
#include "ps2_vkcen.h"          /* (PS2_VK_CENSUS: vkCmd* calls counted) */
#include "zero_instr.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OPT_FIF 4u                /* frame indices handed to the hooks (ps2_clouds.c CL_FIF) */
#define OPT_N 92                  /* floats of CloudFrame opt[] (23 vec4) */
#define OPT_DEG 0.017453293f

extern int (*ps2_vk_instr_get)(zero_instr *out);   /* ps2_video.c; set by zero_vr.c (Ace Combat Zero) */
extern volatile float ps2_imm_freeze_m;            /* ps2_video.c; the mission's freezing level (zero_vr.c) */
/* the luminance (0..1) of the stage's cloud LIGHT colour, written by the game module (zero_clouds.c: the environment
   set's +0x1F8) for ACZ_VR_OPTICS_SUNLIT; -1 = unknown (then C1's captured LIGHT colour) */
volatile float ps2_clouds_game_lit_y = -1.0f;

static struct {
    int ok, avail, on;            /* resources; ACZ_VR_OPTICS at start; F4 toggle */
    int en[4];                    /* halo, glory, spectre, rainbow (ACZ_VR_OPTICS_HALO / _GLORY / _SPECTRE / _BOW) */
    int test;                     /* ACZ_VR_OPTICS_TEST bits: 1 halo, 2 glory, 4 spectre, 8 rainbow */
    float k[4];                   /* strengths (ACZ_VR_OPTICS_K x the per-phenomenon constants) */
    float bow_elev, drop_um, size, test_d;
    int moon, dbg, ab_off;
    int phys;                     /* ACZ_VR_OPTICS_HALO_PHYS: the halo needs cold ice cloud (bug #72); 0 = as before */
    float ice_c[2];               /* ACZ_VR_OPTICS_ICE_C: halo ice from this cloud temperature, full at the second (C) */
    float ice_top;                /* (debug) the ice share at the band's top, the cloud form factor */
    float form;
    float sunlit[2];              /* ACZ_VR_OPTICS_SUNLIT: luminance of the game's LIGHT colour for no / full direct sun
                                     (0, 0 = off) */
    float direct, sunvis;         /* (debug) this frame's direct-sun and sun-from-the-eye factors */
    VkDevice dev;
    VkDescriptorSetLayout dsl; VkDescriptorPool pool; VkDescriptorSet set;
    VkPipelineLayout lay; VkPipeline pipe;
    VkBuffer buf; VkDeviceMemory mem; float *map;
    uint8_t used[OPT_FIF];
    uint32_t frame_seen, rec_frame;
    /* the probe, smoothed */
    int have; double t_probe, t_frame;
    float D, lD, op, tsurf, inside, tsun, found, ta;
    int near_surface;             /* the surface was found in the near range (the near composite owns it) */
    float g[4], tgt[4];           /* gate (faded) and its target per phenomenon */
    float opt[OPT_N];             /* this command buffer's CloudFrame opt[] */
    uint64_t frames, dispatched, on_frames[4];
    char why[4][48];
} O;

static float envf(const char *n, float def, float lo, float hi) {
    const char *e = getenv(n);
    if (!e || !*e) return def;
    float v = (float)atof(e);
    return v < lo ? lo : v > hi ? hi : v;
}
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static float sstep(float a, float b, float x) { float t = clampf((x - a) / (b - a), 0.0f, 1.0f); return t * t * (3.0f - 2.0f * t); }
static void v3norm(float v[3]) { float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); if (l > 1e-9f) { v[0] /= l; v[1] /= l; v[2] /= l; } }
static float v3dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void v3cross(const float a[3], const float b[3], float o[3]) {
    o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0];
}

int ps2_clouds_optics_init(const ps2_cloud_gpu *g) {
    memset(&O, 0, sizeof O);
    O.dev = g->dev;
    O.avail = (int)envf("ACZ_VR_OPTICS", 1.0f, 0.0f, 1.0f);
    O.on = O.avail;
    O.en[0] = (int)envf("ACZ_VR_OPTICS_HALO", 1.0f, 0.0f, 2.0f);   /* 2: with the 46 deg halo */
    O.en[1] = (int)envf("ACZ_VR_OPTICS_GLORY", 1.0f, 0.0f, 1.0f);
    O.en[2] = (int)envf("ACZ_VR_OPTICS_SPECTRE", 1.0f, 0.0f, 1.0f);
    O.en[3] = (int)envf("ACZ_VR_OPTICS_BOW", 1.0f, 0.0f, 1.0f);
    O.test = (int)envf("ACZ_VR_OPTICS_TEST", 0.0f, 0.0f, 15.0f);
    float k = envf("ACZ_VR_OPTICS_K", 1.0f, 0.0f, 10.0f);
    /* bug #72 ("is the halo really always visible like this?", M1 2026-10-23 09:59): the halo was drawn on every thin
       cloud at 22 deg from the sun wherever the freezing level put ice -- in the snow missions (freezing level 0 m)
       on all of them, in M2 on the cumulus tops above 3.3 km (-4 C). Real halos need cirrus-type ice: a thin veil of
       well-formed hexagonal crystals, which forms at -20 C and colder (supercooled water dominates warmer, in
       cumulus to ~-20 C) and gives no halo from below a thick deck (the ice there gets no direct sun).
       ACZ_VR_OPTICS_HALO_PHYS=1 (default): ice by the cloud's temperature (ACZ_VR_OPTICS_ICE_C, -20 .. -35 C, 6.5 K per
       km above the freezing level; the snow missions' 0 m level = 0 C at sea level), the band must reach it, layer
       clouds only (cumulus 0.3), the per-pixel sun on the ice (the cloud shadow map), the sun clearly visible from
       the eye (transmittance 0.05 .. 0.25), and a subtler peak (ACZ_VR_OPTICS_HALO_K 0.09 of the sunlit cloud
       colour, was 0.13). 0 = as before. */
    O.phys = (int)envf("ACZ_VR_OPTICS_HALO_PHYS", 1.0f, 0.0f, 1.0f);
    O.ice_c[0] = -20.0f; O.ice_c[1] = -35.0f;
    {   const char *e = getenv("ACZ_VR_OPTICS_ICE_C");
        float a, b;
        if (e && sscanf(e, "%f,%f", &a, &b) == 2 && a > -80.0f && a <= 0.0f && b < a - 1.0f && b > -90.0f) {
            O.ice_c[0] = a; O.ice_c[1] = b;
        }
    }
    float hk = envf("ACZ_VR_OPTICS_HALO_K", O.phys ? 0.09f : 0.13f, 0.0f, 1.0f);
    /* peak additions in slot-0 units of the sunlit cloud colour: halo, glory, spectre (share of the direct light the
       shadow takes away), rainbow */
    O.k[0] = hk * k; O.k[1] = 0.22f * k; O.k[2] = clampf(0.6f * k, 0.0f, 1.0f); O.k[3] = 0.30f * k;
    O.bow_elev = envf("ACZ_VR_OPTICS_BOW_ELEV", 42.0f, 0.0f, 90.0f);
    /* 2026-10-27 13:51, M7 storm ("a rainbow probably should not appear in this weather; rainbows need the sun"): the
       rainbow (like the glory, the spectre and the halo) was gated only by our own clouds -- the probe's surface, the
       cloud shadow map on the rain -- and our volume in M7 is the game's sparse cloud list (coverage 0.57, opacity
       ~0.13), so the sun read as shining on the rain while the game paints a storm: a dark overcast sky, the clouds'
       sunlit side a dim blue grey (LIGHT 111,122,132: luminance 0.47; the sunny missions 0.62..0.90), rain everywhere.
       What the game's art says about direct sunlight is its LIGHT palette colour (a cloud's sunlit side): bright only
       under a sun that shines. ACZ_VR_OPTICS_SUNLIT="lo,hi" (default 0.50,0.62): every phenomenon x smoothstep(lo, hi,
       luminance of LIGHT) (M7 0: none in its storm; M4 0.62, M9 0.71, M15 0.72, M16 0.66 keep theirs), and the
       rainbow, the glory and the spectre also need the sun visible from the eye through our clouds (the probe's sun
       transmittance 0.10 .. 0.40; the halo keeps its own 0.05 .. 0.25): the antisolar point lies in the eye's own
       shadow column, so a shaded eye sees neither the glory nor sunlit rain right behind it. 0 = as before. */
    O.sunlit[0] = 0.50f; O.sunlit[1] = 0.62f;
    {   const char *e = getenv("ACZ_VR_OPTICS_SUNLIT");
        float a, b;
        if (e && e[0] == '0' && (e[1] == '\0' || e[1] == ',')) O.sunlit[0] = O.sunlit[1] = 0.0f;
        else if (e && sscanf(e, "%f,%f", &a, &b) == 2 && a >= 0.0f && b > a + 0.01f && b <= 1.0f) {
            O.sunlit[0] = a; O.sunlit[1] = b;
        }
    }
    O.drop_um = envf("ACZ_VR_OPTICS_DROP_UM", 10.0f, 3.0f, 40.0f);
    O.size = envf("ACZ_VR_OPTICS_SIZE", 1.0f, 0.3f, 3.0f);
    O.test_d = envf("ACZ_VR_OPTICS_TEST_D", 300.0f, 30.0f, 5000.0f);
    O.moon = (int)envf("ACZ_VR_OPTICS_MOON", 0.0f, 0.0f, 1.0f);
    O.dbg = (int)envf("ACZ_DEBUG_OPTICS", 0.0f, 0.0f, 1000000.0f);
    if (!O.avail) { ps2_log("clouds: optics off (ACZ_VR_OPTICS=0)"); O.ok = 1; return 0; }
    VkDevice dev = g->dev;
    VkDescriptorSetLayoutBinding b; memset(&b, 0, sizeof b);
    b.binding = 0; b.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo lc; memset(&lc, 0, sizeof lc);
    lc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO; lc.bindingCount = 1; lc.pBindings = &b;
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
    VkDescriptorPoolCreateInfo pc; memset(&pc, 0, sizeof pc);
    pc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO; pc.maxSets = 1; pc.poolSizeCount = 1; pc.pPoolSizes = &ps;
    if (vkCreateDescriptorSetLayout(dev, &lc, NULL, &O.dsl) != VK_SUCCESS
        || vkCreateDescriptorPool(dev, &pc, NULL, &O.pool) != VK_SUCCESS
        || ps2_clouds_make_buffer(OPT_FIF * 32u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 2, &O.buf, &O.mem, (void **)&O.map) != 0)
        goto fail;
    {   VkDescriptorSetAllocateInfo ai; memset(&ai, 0, sizeof ai);
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO; ai.descriptorPool = O.pool;
        ai.descriptorSetCount = 1; ai.pSetLayouts = &O.dsl;
        if (vkAllocateDescriptorSets(dev, &ai, &O.set) != VK_SUCCESS) goto fail;
        VkDescriptorBufferInfo bi = { O.buf, 0, OPT_FIF * 32u };
        VkWriteDescriptorSet w; memset(&w, 0, sizeof w);
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet = O.set; w.dstBinding = 0; w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w.pBufferInfo = &bi;
        vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
        memset(O.map, 0, OPT_FIF * 32u);
    }
    O.pipe = ps2_clouds_make_compute("cloud_optics.comp.spv", O.dsl, &O.lay);
    if (!O.pipe) goto fail;
    O.ok = 1;
    ps2_log("clouds: optics on (halo %d, glory %d, spectre %d, rainbow %d; strength %.2f, rainbow below %.0f deg of sun, "
            "glory droplets %.0f um, aircraft size x%.2f%s%s)", O.en[0], O.en[1], O.en[2], O.en[3], (double)k,
            (double)O.bow_elev, (double)O.drop_um, (double)O.size, O.moon ? ", lunar halo" : "",
            O.test ? ", TEST: forced" : "");
    if (O.phys)
        ps2_log("clouds: optics halo only in cold ice cloud: %.0f .. %.0f C, layer clouds, sunlit, peak %.3f "
                "(ACZ_VR_OPTICS_HALO_PHYS=0: as before)", (double)O.ice_c[0], (double)O.ice_c[1], (double)O.k[0]);
    if (O.sunlit[1] > 0.0f)
        ps2_log("clouds: optics only in direct sunlight: the game's LIGHT colour luminance %.2f .. %.2f (a storm's dim "
                "palette: none), the rainbow / glory / spectre with the sun visible from the eye (ACZ_VR_OPTICS_SUNLIT=0: "
                "as before)", (double)O.sunlit[0], (double)O.sunlit[1]);
    return 0;
fail:
    ps2_log("clouds: optics initialisation failed -- optics off");
    O.avail = O.on = 0; O.ok = 1;
    return 0;
}

void ps2_clouds_optics_shutdown(const ps2_cloud_gpu *g) {
    VkDevice dev = g->dev;
    if (!O.dev) return;
    if (O.pipe) vkDestroyPipeline(dev, O.pipe, NULL);
    if (O.lay) vkDestroyPipelineLayout(dev, O.lay, NULL);
    if (O.pool) vkDestroyDescriptorPool(dev, O.pool, NULL);
    if (O.dsl) vkDestroyDescriptorSetLayout(dev, O.dsl, NULL);
    if (O.buf) { vkDestroyBuffer(dev, O.buf, NULL); vkFreeMemory(dev, O.mem, NULL); }
    memset(&O, 0, sizeof O);
}

int ps2_clouds_optics_toggle(int on) {
    if (!O.ok || !O.avail) return -1;
    if (on >= 0) O.on = on != 0;
    return O.on;
}
void ps2_clouds_optics_ab(int off) { O.ab_off = off != 0; }
static int enabled(void) { return O.ok && O.avail && O.on && O.pipe; }
static int active(void) { return enabled() && !O.ab_off; }

/* the aircraft's body axes in world (right, up, forward; the flight model's angles, the game's euler convention as
   zero_vr.c m3_euler: columns right / up / back); the view's axes when the instruments are not available */
static void body_axes(const ps2_cloud_view *v, float r[3], float u[3], float f[3]) {
    zero_instr in;
    if (ps2_vk_instr_get && ps2_vk_instr_get(&in) && in.valid) {
        float p = in.pitch_deg * OPT_DEG, y = -in.heading_deg * OPT_DEG, ro = in.roll_deg * OPT_DEG;
        float cp = cosf(p), sp = sinf(p), cy = cosf(y), sy = sinf(y), cr = cosf(ro), sr = sinf(ro);
        r[0] = cy * cr + sy * sp * sr; r[1] = cp * sr; r[2] = -sy * cr + cy * sp * sr;
        u[0] = -cy * sr + sy * sp * cr; u[1] = cp * cr; u[2] = sy * sr + cy * sp * cr;
        f[0] = -sy * cp; f[1] = sp; f[2] = -cy * cp;
        return;
    }
    for (int i = 0; i < 3; i++) { r[i] = v->cam_x[i]; u[i] = v->cam_y[i]; f[i] = -v->cam_z[i]; }
}

/* the spectre's parts in the shadow plane (opt[6..17]) for antisolar direction a */
static void spectre_parts(float *o, const float a[3], const ps2_cloud_view *v) {
    float r[3], u[3], f[3], e1[3], e2[3], up[3] = { 0.0f, 1.0f, 0.0f };
    body_axes(v, r, u, f);
    if (fabsf(a[1]) > 0.99f) { up[0] = 1.0f; up[1] = 0.0f; }
    v3cross(a, up, e1); v3norm(e1);
    v3cross(e1, a, e2); v3norm(e2);
    /* a generic fighter around the cockpit (m; x right, y up, z forward): centre, half-extent A, half-extent B */
    static const float P[5][9] = {
        { 0.0f, -0.9f, -4.75f,   0.0f, 0.0f, 8.25f,   0.95f, 0.0f, 0.0f },     /* fuselage (B = radius) */
        { 3.0f, -1.0f, -6.6f,    2.6f, 0.0f, -0.9f,   0.0f, 0.0f, 1.5f },      /* right wing */
        { -3.0f, -1.0f, -6.6f,  -2.6f, 0.0f, -0.9f,   0.0f, 0.0f, 1.5f },      /* left wing */
        { 0.0f, -0.8f, -12.0f,   2.7f, 0.0f, 0.0f,    0.0f, 0.0f, 1.0f },      /* tailplane */
        { 0.0f, 1.0f, -11.4f,    0.0f, 1.5f, -0.7f,   0.0f, 0.0f, 1.1f },      /* fin */
    };
    float s = O.size, rad = 0.0f;
    memcpy(o + 24, e1, 12); memcpy(o + 28, e2, 12);
    for (int k = 0; k < 5; k++) {
        float c[3], A[3], B[3];
        for (int i = 0; i < 3; i++) {
            c[i] = s * (P[k][0] * r[i] + P[k][1] * u[i] + P[k][2] * f[i]);
            A[i] = s * (P[k][3] * r[i] + P[k][4] * u[i] + P[k][5] * f[i]);
            B[i] = s * (P[k][6] * r[i] + P[k][7] * u[i] + P[k][8] * f[i]);
        }
        float c2[2] = { v3dot(c, e1), v3dot(c, e2) }, A2[2] = { v3dot(A, e1), v3dot(A, e2) }, B2[2];
        float la = sqrtf(A2[0] * A2[0] + A2[1] * A2[1]);
        if (k == 0) {                              /* a cylinder's shadow: the axis widened by the radius */
            float rr = s * P[0][6];
            if (la < rr) { if (la > 1e-6f) { A2[0] *= rr / la; A2[1] *= rr / la; } else { A2[0] = rr; A2[1] = 0.0f; } la = rr; }
            B2[0] = -A2[1] / la * rr; B2[1] = A2[0] / la * rr;
        } else {
            B2[0] = v3dot(B, e1); B2[1] = v3dot(B, e2);
            if (la < 0.05f) { A2[0] = 0.05f; A2[1] = 0.0f; la = 0.05f; }
            float det = A2[0] * B2[1] - A2[1] * B2[0];
            if (fabsf(det) < 0.08f * la) {           /* a plate edge-on to the sun: its thickness */
                float sg = det < 0.0f ? -1.0f : 1.0f;
                B2[0] += -A2[1] / la * 0.08f * sg; B2[1] += A2[0] / la * 0.08f * sg;
            }
        }
        float det = A2[0] * B2[1] - A2[1] * B2[0];
        if (fabsf(det) < 1e-6f) det = 1e-6f;
        float lb = sqrtf(B2[0] * B2[0] + B2[1] * B2[1]);
        float *q = o + 32 + 8 * k;
        q[0] = c2[0]; q[1] = c2[1];
        q[2] = B2[1] / det; q[3] = -B2[0] / det;           /* row 0 of the inverse of [A2 B2] */
        q[4] = -A2[1] / det; q[5] = A2[0] / det;           /* row 1 */
        q[6] = 1.0f / fmaxf(la, 1e-3f); q[7] = 1.0f / fmaxf(lb, 1e-3f);
        float ext = sqrtf(c2[0] * c2[0] + c2[1] * c2[1]) + la + lb;
        if (ext > rad) rad = ext;
    }
    o[27] = rad;
}

/* ACZ_VR_OPTICS_HALO_PHYS: the altitudes (m) of the halo ice's two temperatures (ACZ_VR_OPTICS_ICE_C): the freezing
   level (zero_vr.c) + 6.5 K per km; the snow missions' level 0 m = 0 C at sea level */
static void ice_alt(float a[2]) {
    float fz = ps2_imm_freeze_m;
    if (!(fz > 0.0f && fz < 20000.0f)) fz = 0.0f;
    a[0] = fz - O.ice_c[0] / 0.0065f; a[1] = fz - O.ice_c[1] / 0.0065f;
}

static void fade(int i, float target, float dt) {
    O.tgt[i] = target;
    float tc = target > O.g[i] ? 0.8f : 0.6f;              /* in over ~1.5 s, out a little faster */
    O.g[i] += (target - O.g[i]) * (float)(1.0 - exp(-(double)dt / tc));
    if (O.g[i] < 1e-3f && target <= 0.0f) O.g[i] = 0.0f;
}

/* once per command buffer, before the CloudFrame copies are filled */
void ps2_clouds_optics_begin(const ps2_cloud_env *e, const ps2_cloud_view *v, int fi, uint32_t frame) {
    if (!O.ok || frame == O.frame_seen) return;
    O.frame_seen = frame;
    memset(O.opt, 0, sizeof O.opt);
    if (fi >= 0 && fi < (int)OPT_FIF && O.used[fi]) {      /* the probe of this slot's last use (its fence has passed) */
        const float *p = O.map + 8u * (uint32_t)fi;
        double dtp = O.have ? v->time_s - O.t_probe : 1.0;
        float kf = (float)(1.0 - exp(-(dtp < 0.0 ? 0.0 : dtp > 1.0 ? 1.0 : dtp) / 0.15));
        if (p[0] > 0.0f) {
            float l = logf(p[0]);
            O.lD = O.have && O.D > 0.0f ? O.lD + (l - O.lD) * kf : l;
            O.D = expf(O.lD);
        } else if (!O.have) O.D = 0.0f;
        O.found = p[0] > 0.0f ? 1.0f : 0.0f;
        O.op = O.have ? O.op + (p[1] - O.op) * kf : p[1];
        O.tsurf = O.have ? O.tsurf + (p[2] - O.tsurf) * kf : p[2];
        O.inside = p[3];
        float ts = clampf(p[4], 0.0f, 1.0f) * clampf(p[5], 0.0f, 1.0f);
        O.tsun = O.have ? O.tsun + (ts - O.tsun) * kf : ts;
        O.ta = p[7];
        O.near_surface = p[6] > 0.5f;
        O.have = 1; O.t_probe = v->time_s;
        O.used[fi] = 0;
    }
    double dtd = O.t_frame > 0.0 ? v->time_s - O.t_frame : 0.0;
    float dt = (float)(dtd < 0.0 ? 0.0 : dtd > 0.5 ? 0.5 : dtd);
    O.t_frame = v->time_s;
    O.frames++;
    float s[3] = { e->sun_dir[0], e->sun_dir[1], e->sun_dir[2] };
    v3norm(s);
    float el = asinf(clampf(s[1], -1.0f, 1.0f)) / OPT_DEG;
    int night = ((e->valid & CL_ENV_ART_K) && e->art_k[1] < 0.0f) || s[1] < 0.0f;
    int on = enabled();                   /* (an A/B off frame keeps the gates and only draws nothing) */
    float ic = ps2_clouds_in_cloud();
    float clear = 1.0f - sstep(0.25f, 0.6f, ic < 0.0f ? 0.0f : ic);  /* inside cloud the light is diffuse */
    {   /* (tunnel_light, bug #267) and none of it with the sun hidden from the eye by a roof or the terrain */
        extern float ps2_vk_enclosure_sun(void);
        clear *= ps2_vk_enclosure_sun();
    }
    int tf = O.test;
    float tgt[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    for (int i = 0; i < 4; i++) snprintf(O.why[i], sizeof O.why[i], "%s", !on ? "off" : !O.en[i] ? "disabled" : "");
    if (on) {
        /* halo: the sun visible through the clouds from the eye (thick: none); day, or the moon at night (opt-in) */
        if (O.en[0]) {
            if (night && !O.moon) snprintf(O.why[0], sizeof O.why[0], "night");
            else if (el < 0.5f) snprintf(O.why[0], sizeof O.why[0], "sun low %.1f", (double)el);
            else if (!O.phys) {
                float type = (e->valid & CL_ENV_WEATHER) ? e->type : 0.5f;
                tgt[0] = sstep(0.03f, 0.2f, O.have ? O.tsun : 1.0f) * clear * (1.0f - 0.5f * sstep(0.5f, 0.9f, type))
                         * (night ? 0.35f : 1.0f);
                snprintf(O.why[0], sizeof O.why[0], "sun T %.2f clear %.2f", (double)O.tsun, (double)clear);
            } else {
                /* ACZ_VR_OPTICS_HALO_PHYS: ice cloud must exist at all (the band's top at or above the -20 C level),
                   a layer cloud (C1 type 0 stratus .. 1 cumulus: cumulus x0.3), the sun clearly visible */
                float type = (e->valid & CL_ENV_WEATHER) ? e->type : 0.5f, ia[2];
                ice_alt(ia);
                float top = (e->valid & CL_ENV_PROFILE) ? e->top_m : 1.0e9f;
                O.ice_top = sstep(ia[0], ia[1], top);
                O.form = 1.0f - 0.7f * sstep(0.4f, 0.8f, type);
                tgt[0] = sstep(0.05f, 0.25f, O.have ? O.tsun : 1.0f) * clear * O.form * sstep(0.0f, 0.03f, O.ice_top)
                         * (night ? 0.35f : 1.0f);
                if (O.ice_top <= 0.0f)
                    snprintf(O.why[0], sizeof O.why[0], "no ice: top %.0f, %.0f C at %.0f m", (double)top,
                             (double)O.ice_c[0], (double)ia[0]);
                else snprintf(O.why[0], sizeof O.why[0], "ice %.2f form %.2f sun T %.2f clear %.2f", (double)O.ice_top,
                              (double)O.form, (double)O.tsun, (double)clear);
            }
        }
        /* glory / spectre: a sunlit, opaque surface below the aircraft, the eye in clear air */
        float a[3] = { -s[0], -s[1], -s[2] };
        int surf = O.have && O.D > 0.0f && O.found > 0.5f;
        float h = surf ? O.D * -a[1] : 0.0f;                /* height of the aircraft above the surface point */
        float base = surf && !night && el > 3.0f ? sstep(0.35f, 0.7f, O.op) * sstep(0.3f, 0.7f, O.tsurf) * clear
                     * (1.0f - sstep(0.05f, 0.3f, O.inside)) * sstep(15.0f, 40.0f, h) : 0.0f;
        float far_m = (e->valid & CL_ENV_COLOURS) && e->fog_far_m > 1000.0f ? e->fog_far_m : 8000.0f;
        if (O.en[1]) {
            tgt[1] = base * (1.0f - sstep(0.5f * far_m, far_m, O.D));
            snprintf(O.why[1], sizeof O.why[1], "%s D %.0f op %.2f sun %.2f", night ? "night" : surf ? "surface" : "none",
                     (double)O.D, (double)O.op, (double)O.tsurf);
        }
        if (O.en[2]) {
            tgt[2] = base * sstep(4000.0f, 2000.0f, O.D);
            snprintf(O.why[2], sizeof O.why[2], "D %.0f h %.0f", (double)O.D, (double)h);
        }
        /* rainbow: rain falling (the stage's volume; not snow), the sun below ACZ_VR_OPTICS_BOW_ELEV */
        if (O.en[3]) {
            int rain = (e->valid & CL_ENV_PRECIP) && e->precip_snow < 0.5f && e->precip_amt > 0.02f;
            if (night) snprintf(O.why[3], sizeof O.why[3], "night");
            else if (!rain) snprintf(O.why[3], sizeof O.why[3], "%s", (e->valid & CL_ENV_PRECIP) && e->precip_snow >= 0.5f
                                     && e->precip_amt > 0.02f ? "snow" : "no rain");
            else if (el < 1.0f || el > O.bow_elev) snprintf(O.why[3], sizeof O.why[3], "sun %.0f deg", (double)el);
            else {
                tgt[3] = clear * sstep(1.0f, 3.0f, el) * (1.0f - sstep(O.bow_elev - 2.0f, O.bow_elev, el));
                snprintf(O.why[3], sizeof O.why[3], "rain %.2f band %.0f-%.0f sun %.0f", (double)e->precip_amt,
                         (double)e->precip_lo_m, (double)e->precip_hi_m, (double)el);
            }
        }
        /* ACZ_VR_OPTICS_SUNLIT: direct sunlight as the game paints it (its LIGHT colour) and the sun seen from the eye
           (the rainbow, the glory and the spectre; the halo has its own visibility gate above; the opt-in lunar halo at
           night keeps its own) */
        O.direct = O.sunvis = 1.0f;
        if (O.sunlit[1] > 0.0f) {
            /* the stage's own palette (zero_clouds.c); C1's lit is the art capture, which with the game's clouds dropped
               (ACZ_VR_CLOUDS) stays at the built-in 0.80,0.82,0.83 */
            float Y = ps2_clouds_game_lit_y >= 0.0f ? ps2_clouds_game_lit_y
                      : (e->valid & CL_ENV_COLOURS) ? 0.299f * e->lit[0] + 0.587f * e->lit[1] + 0.114f * e->lit[2] : 1.0f;
            O.direct = sstep(O.sunlit[0], O.sunlit[1], Y);
            O.sunvis = sstep(0.10f, 0.40f, O.have ? O.tsun : 1.0f);
            for (int i = 0; i < 4; i++) {
                float pre = tgt[i];
                tgt[i] *= i == 0 ? (night ? 1.0f : O.direct) : O.direct * O.sunvis;
                if (pre > 0.0f && tgt[i] < 0.5f * pre)
                    snprintf(O.why[i], sizeof O.why[i], "%s: LIGHT %.2f sun T %.2f", O.direct < 0.5f ? "no direct sun"
                             : "sun hidden", (double)Y, (double)O.tsun);
            }
        }
        for (int i = 0; i < 4; i++) if (tf & (1 << i)) tgt[i] = 1.0f;     /* ACZ_VR_OPTICS_TEST */
    }
    for (int i = 0; i < 4; i++) {
        int was = O.tgt[i] > 0.0f;
        fade(i, tgt[i], dt);
        if (O.g[i] > 0.0f) O.on_frames[i]++;
        if (O.dbg && was != (tgt[i] > 0.0f))
            ps2_log("optics: %s %s (%s)", i == 0 ? "halo" : i == 1 ? "glory" : i == 2 ? "spectre" : "rainbow",
                    tgt[i] > 0.0f ? "possible" : "gone", O.why[i]);
    }
    if (O.dbg && O.frames % (uint64_t)O.dbg == 0u)
        ps2_log("optics: frame %llu sun el %.1f%s in-cloud %.2f type %.2f band %.0f-%.0f m | probe D %.0f (%s) op %.2f "
                "sun@surface %.2f inside %.2f "
                "sun T %.2f far T(anti) %.2f direct sun %.2f seen %.2f | halo %.2f [%s] glory %.2f [%s] spectre %.2f [%s] "
                "rainbow %.2f [%s] | "
                "frames on %llu/%llu/%llu/%llu of %llu", (unsigned long long)O.frames, (double)el, night ? " night" : "",
                (double)ic, (e->valid & CL_ENV_WEATHER) ? (double)e->type : -1.0,
                (e->valid & CL_ENV_PROFILE) ? (double)e->base_m : -1.0, (e->valid & CL_ENV_PROFILE) ? (double)e->top_m : -1.0,
                (double)O.D, O.near_surface ? "near" : "far", (double)O.op, (double)O.tsurf, (double)O.inside,
                (double)O.tsun, (double)O.ta, (double)O.direct, (double)O.sunvis, (double)O.g[0], O.why[0],
                (double)O.g[1], O.why[1], (double)O.g[2], O.why[2],
                (double)O.g[3], O.why[3], (unsigned long long)O.on_frames[0], (unsigned long long)O.on_frames[1],
                (unsigned long long)O.on_frames[2], (unsigned long long)O.on_frames[3], (unsigned long long)O.frames);
    if (!on || O.ab_off || (O.g[0] <= 0.0f && O.g[1] <= 0.0f && O.g[2] <= 0.0f && O.g[3] <= 0.0f)) return;
    /* this frame's opt[] (cloud_optics.glsl) */
    float *o = O.opt;
    float a[3] = { -s[0], -s[1], -s[2] };
    float D = O.D;
    if (tf & 6) D = O.test_d;                              /* (look tests: a fixed distance, ACZ_VR_OPTICS_TEST_D) */
    o[0] = O.k[0] * O.g[0]; o[1] = O.k[1] * O.g[1]; o[2] = O.k[2] > 0.0f ? O.g[2] : 0.0f; o[3] = O.k[3] * O.g[3];
    o[4] = a[0]; o[5] = a[1]; o[6] = a[2]; o[7] = D > 0.0f ? D : 0.0f;
    float lum = night ? 0.35f : 1.0f;
    for (int i = 0; i < 3; i++) o[8 + i] = ((e->valid & CL_ENV_COLOURS) ? e->lit[i] : 0.8f) * lum;
    if (night) { float m = (o[8] + o[9] + o[10]) / 3.0f; o[8] = o[9] = o[10] = m; }
    o[11] = O.phys ? 1.0f : 0.0f;                          /* halo: weight of the sun's light on the ice (shadow map) */
    float fz = ps2_imm_freeze_m;
    if (O.phys) { float ia[2]; ice_alt(ia); o[12] = ia[0]; o[75] = ia[1]; }   /* halo ice from -20 C, full at -35 C */
    else {                                                 /* ice above the freezing level + ~4 K, over +-400 m */
        float lv = fz <= 1.0f ? -1.0e9f : fz + 600.0f;
        o[12] = lv - 400.0f; o[75] = lv + 400.0f;
    }
    o[13] = O.en[0] >= 2 ? 0.07f : 0.0f;                   /* the 46 deg halo (rare and faint; opt-in) */
    o[14] = night ? 0.0f : 0.6f * (1.0f - sstep(25.0f, 55.0f, el));
    o[15] = el;
    float sig = (e->valid & CL_ENV_PRECIP) ? 0.0015f * powf(clampf(e->precip_amt, 0.0f, 2.0f), 0.8f) : 0.0f;
    o[16] = sig;
    o[17] = (e->valid & CL_ENV_PRECIP) ? e->precip_lo_m : 0.0f;
    o[18] = (e->valid & CL_ENV_PRECIP) ? e->precip_hi_m : 1.0f;
    {   float far_m = (e->valid & CL_ENV_COLOURS) && e->fog_far_m > 1000.0f ? e->fog_far_m : 8000.0f;
        o[19] = fminf(6000.0f, 0.8f * far_m); }
    if (tf & 8) {                                          /* look test: heavy rain everywhere below the aircraft */
        o[16] = 0.0015f; o[17] = (float)v->cloud_origin_m[1] + 2000.0f; o[18] = o[17] + 1.0f;
    }
    o[20] = 6.2831853f * O.drop_um / 0.54f;
    o[21] = 0.0042f;                                       /* the sun's 0.53 deg disc as a box of equal variance */
    o[22] = (tf & 6) ? 0.0f : O.near_surface ? 1.0f : 0.0f;
    o[23] = (float)tf;
    if (o[2] > 0.0f || (tf & 4)) spectre_parts(o, a, v);
    {   /* parhelia: their separation from the sun grows with its elevation (~22 deg at the horizon) */
        float sep = (21.8f + 0.0040f * el * el) * OPT_DEG, se = sinf(el * OPT_DEG), ce = cosf(el * OPT_DEG);
        float cphi = ce > 0.05f ? (cosf(sep) - se * se) / (ce * ce) : 1.0f;
        o[72] = acosf(clampf(cphi, -1.0f, 1.0f)) / OPT_DEG; o[73] = ce;
    }
    o[74] = O.k[2];
    /* the glory / spectre cone's half angle (the spectre's extent on a close surface) */
    float rho = 7.0f * OPT_DEG;
    if (o[2] > 0.0f && o[7] > 0.0f) {
        float rs = (o[27] + 2.0f * o[7] * o[21]) / o[7];
        if (rs > rho) rho = rs > 1.2f ? 1.2f : rs;
    }
    for (int e2 = 0; e2 < 2; e2++) {                       /* each eye's antisolar point on the surface */
        float q[3] = { a[0] * o[7] - v->eye_m[e2][0], a[1] * o[7] - v->eye_m[e2][1], a[2] * o[7] - v->eye_m[e2][2] };
        if (o[7] <= 0.0f) { q[0] = a[0]; q[1] = a[1]; q[2] = a[2]; }
        v3norm(q);
        memcpy(o + 76 + 4 * e2, q, 12);
        /* the cone's screen box in target pixels (the near composite tests it before any optics work): eight
           directions on the cone (+ 2 deg) projected, 10 % margin; the whole target when the cone reaches the
           side of or behind the view plane */
        float u1[3], u2[3], up2[3] = { 0.0f, 1.0f, 0.0f }, *bx = o + 84 + 4 * e2;
        if (fabsf(q[1]) > 0.99f) { up2[0] = 1.0f; up2[1] = 0.0f; }
        v3cross(q, up2, u1); v3norm(u1); v3cross(u1, q, u2); v3norm(u2);
        float r = rho + 2.0f * OPT_DEG, cr = cosf(r), sr = sinf(r);
        float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
        int all = 0;
        for (int k = 0; k < 8 && !all; k++) {
            float ph = (float)k * 0.785398f, dd[3];
            for (int i = 0; i < 3; i++) dd[i] = cr * q[i] + sr * (cosf(ph) * u1[i] + sinf(ph) * u2[i]);
            float vz = -v3dot(dd, v->cam_z);
            if (vz < 0.05f) { all = 1; break; }
            float px = v->proj[2] + v->proj[0] * v3dot(dd, v->cam_x) / vz;
            float py = v->proj[3] - v->proj[1] * v3dot(dd, v->cam_y) / vz;
            x0 = fminf(x0, px); x1 = fmaxf(x1, px);
            y0 = fminf(y0, py); y1 = fmaxf(y1, py);
        }
        if (all) { bx[0] = -1e9f; bx[1] = -1e9f; bx[2] = 1e9f; bx[3] = 1e9f; }
        else {
            float mx = 0.1f * (x1 - x0) + 4.0f, my = 0.1f * (y1 - y0) + 4.0f;
            bx[0] = x0 - mx; bx[1] = y0 - my; bx[2] = x1 + mx; bx[3] = y1 + my;
        }
    }
}

extern int ps2_vk_perf_ab_flipped(const char *name);   /* ps2_video.c: PS2_PERF_AB re-draw with a switch flipped */
void ps2_clouds_optics_ubo(float opt[OPT_N]) {
    /* PS2_PERF_AB=1 PS2_PERF_AB_FLIP=optics: the dumped frame's re-draw without the optics (an exact image A/B) */
    if (!O.ok || ps2_vk_perf_ab_flipped("optics")) { memset(opt, 0, OPT_N * sizeof(float)); return; }
    memcpy(opt, O.opt, OPT_N * sizeof(float));
}

typedef struct { int32_t i0[4], i1[4]; float f0[4], f1[4]; } opt_push;
_Static_assert(sizeof(opt_push) <= CL_PUSH_BYTES, "optics push constants");
/* The probe, once per command buffer (the common set is bound): early (ps2_clouds_far_early, after the weather pass and
   before the cloud shadow pass, whose barrier orders it before the far update and makes the result visible to the
   host: own_barrier 0) or, without an early update, at the far point (own_barrier 1). Skipped at night, while off and
   in in-between frames (the probe follows the game frame). */
int ps2_clouds_optics_record(VkCommandBuffer cmd, const ps2_cloud_rec *r, float near_range_m, int own_barrier) {
    if (!active() || !r || r->frame_index < 0 || r->frame_index >= (int)OPT_FIF || r->view->inbetween) return 0;
    if (r->frame == O.rec_frame) return 0;                 /* (already recorded in this command buffer) */
    const ps2_cloud_env *e = r->env;
    if (e->sun_dir[1] <= 0.0f && !O.moon) return 0;
    int fi = r->frame_index;
    opt_push pc; memset(&pc, 0, sizeof pc);
    pc.i0[0] = fi; pc.f0[0] = near_range_m; pc.f0[1] = 1.0f;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, O.pipe);
    ps2_clouds_bind_common(cmd, O.lay);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, O.lay, 1, 1, &O.set, 0, NULL);
    vkCmdPushConstants(cmd, O.lay, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(cmd, 1, 1, 1);
    O.rec_frame = r->frame;
    O.used[fi] = 1;
    O.dispatched++;
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
