// zero_trails.c - Ace Combat Zero: contrails, wing vapour, heat haze and the missile smoke light (wishes #85 / #86 /
// #87, agent im_trails 2026-10-23) and the cloud wake (ACZ_VR_CLOUD_WAKE, agent cl_wake 2026-10-29). Game side (EE
// thread): follows the game's aircraft and publishes the trails, the vapour puffs, the haze plumes and the smoke light
// for the renderer (runtime/include/ps2_trails.h, ps2_trails.c).
// Nothing here writes game memory: positions, angles and model nodes are read; the game's own effects (its missile
// smoke, its explosions) are untouched (the smoke light is a colour factor in rn_vp_fx.c).
#include "ps2_runtime.h"
#include "ps2_hook.h"
#include "ps2_trails.h"
#include "zero_instr.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "ps2_clouds.h"

u64 ps2_kernel_vblank_count(void);

static float zt_f(u32 a) { u32 w = ps2_r32(a); float f; memcpy(&f, &w, 4); return f; }
static int zt_ptr(u32 a) { return a >= 0x100000u && a < 0x2000000u; }
static float env_or(const char *n, float d) {
    const char *e = getenv(n);
    if (!e || !*e) return d;
    float v = (float)atof(e);
    return v == v ? v : d;
}

/* a game function called from a hook (as zero_vr.c zg_call), three arguments */
static u32 zt_call3(ps2_ctx *ctx, u32 fn, u32 a0, u32 a1, u32 a2) {
    ps2_reg128 rs[32];
    ps2_fpr fs[32];
    ps2_reg128 hi = ctx->hi, lo = ctx->lo;
    float acc = ctx->acc;
    u32 fcc = ctx->fcc, fcr = ctx->fcr31, sa = ctx->sa, pc = ctx->pc;
    memcpy(rs, ctx->r, sizeof rs);
    memcpy(fs, ctx->f, sizeof fs);
    u32 sp = (ctx->r[29].uw[0] - 0x100u) & ~15u;
    ctx->r[4].ud[0] = (u64)(s64)(s32)a0; ctx->r[5].ud[0] = (u64)(s64)(s32)a1; ctx->r[6].ud[0] = (u64)(s64)(s32)a2;
    ctx->r[29].ud[0] = (u64)(s64)(s32)sp;
    ctx->r[31].ud[0] = 0;
    ps2_dispatch(ctx, fn);
    u32 v0 = ctx->r[2].uw[0];
    memcpy(ctx->r, rs, sizeof rs);
    memcpy(ctx->f, fs, sizeof fs);
    ctx->hi = hi; ctx->lo = lo; ctx->acc = acc; ctx->fcc = fcc; ctx->fcr31 = fcr; ctx->sa = sa; ctx->pc = pc;
    return v0;
}

/* ---- research probe (ACZ_DEBUG_TRAILS_PROBE=N): the aircraft burner setup 0x1386D0 (a0 render object, +4 model;
   a2 / a3 position / angles?) and the model's typed nodes (0x1337C0(model, type, i) -> table index) ---- */
static int zt_probe_n;
static u64 zt_probe_field, zt_probe_calls;
static u32 zt_seen_model[64]; static int zt_nseen;
static int zt_probe_burner(ps2_ctx *ctx, void *user) {
    (void)user;
    u64 f = ps2_kernel_vblank_count();
    u32 a0 = ctx->r[4].uw[0], a1 = ctx->r[5].uw[0], a2 = ctx->r[6].uw[0], a3 = ctx->r[7].uw[0], t0 = ctx->r[8].uw[0];
    u32 ra = ctx->r[31].uw[0];
    if (f != zt_probe_field) {
        if (zt_probe_field && (zt_probe_field % 120u) == 0u)
            ps2_log("trails-probe: field %llu: %llu burner calls", (unsigned long long)zt_probe_field,
                    (unsigned long long)zt_probe_calls);
        zt_probe_field = f; zt_probe_calls = 0;
    }
    zt_probe_calls++;
    if (!zt_ptr(a0)) return 0;
    u32 model = ps2_r32(a0 + 4u);
    int lg = (f % 120u) == 0u;
    if (lg)
        ps2_log("trails-probe: f %llu ra %08X obj %08X model %08X a1 %08X pos %08X (%.1f %.1f %.1f %.2f) ang %08X (%.3f %.3f "
                "%.3f %.3f) t0 %08X | +8 %04X +A %04X +C %04X +E %04X +10 %02X +11 %02X +1A %02X +20 %08X +28 %.3f +2C %.3f "
                "+30 %.3f +34 %.3f +38 %.3f",
                (unsigned long long)f, ra, a0, model, a1, a2, zt_ptr(a2) ? zt_f(a2) : 0.0f, zt_ptr(a2) ? zt_f(a2 + 4u) : 0.0f,
                zt_ptr(a2) ? zt_f(a2 + 8u) : 0.0f, zt_ptr(a2) ? zt_f(a2 + 12u) : 0.0f, a3, zt_ptr(a3) ? zt_f(a3) : 0.0f,
                zt_ptr(a3) ? zt_f(a3 + 4u) : 0.0f, zt_ptr(a3) ? zt_f(a3 + 8u) : 0.0f, zt_ptr(a3) ? zt_f(a3 + 12u) : 0.0f, t0,
                ps2_r16(a0 + 8u), ps2_r16(a0 + 0xAu), ps2_r16(a0 + 0xCu), ps2_r16(a0 + 0xEu), ps2_r8(a0 + 0x10u),
                ps2_r8(a0 + 0x11u), ps2_r8(a0 + 0x1Au), ps2_r32(a0 + 0x20u), zt_f(a0 + 0x28u), zt_f(a0 + 0x2Cu),
                zt_f(a0 + 0x30u), zt_f(a0 + 0x34u), zt_f(a0 + 0x38u));
    if (!zt_ptr(model)) return 0;
    int k;
    for (k = 0; k < zt_nseen && zt_seen_model[k] != model; k++) ;
    if (k < zt_nseen || zt_nseen >= 64) return 0;
    zt_seen_model[zt_nseen++] = model;
    {   /* the model's typed node lists: 0x133410(model, type, &count) -> u16 node indices; node records of 64 bytes at
           *(model+0xC) */
        u32 nodes = ps2_r32(model + 0xCu);
        ps2_log("trails-probe: model %08X nodes %08X hdr +4C %04X +4E %04X +50 %04X +52 %04X", model, nodes,
                ps2_r16(model + 0x4Cu), ps2_r16(model + 0x4Eu), ps2_r16(model + 0x50u), ps2_r16(model + 0x52u));
        if (zt_ptr(nodes)) {                            /* the first node records: type, parent?, position */
            char l[900]; int o = 0;
            for (u32 i = 0; i < 48u && o < 820; i++) {
                u32 nr = nodes + i * 64u;
                o += snprintf(l + o, sizeof l - (size_t)o, " %u:%X/%X(%.1f %.1f %.1f)", i, ps2_r32(nr + 0x10u) & 0xFFFFu,
                              ps2_r32(nr + 8u) & 0xFFFFu, zt_f(nr + 0x20u), zt_f(nr + 0x24u), zt_f(nr + 0x28u));
            }
            ps2_log("trails-probe: model %08X node list%s", model, l);
        }
        for (u32 ty = 0x60u; ty <= 0x6Au; ty++) {
            u32 sp = (ctx->r[29].uw[0] - 0x100u) & ~15u, scratch = sp + 0xC0u;
            ps2_w32(scratch, 0u);
            u32 list = zt_call3(ctx, 0x00133410u, model, ty, scratch);
            u32 cnt = ps2_r32(scratch);
            if (!zt_ptr(list) || !cnt || cnt > 64u) continue;
            for (u32 i = 0; i < cnt && i < 8u; i++) {
                u32 idx = ps2_r16(list + 2u * i), nr = nodes + idx * 64u;
                if (!zt_ptr(nr)) continue;
                char l[700]; int o = 0;
                o += snprintf(l + o, sizeof l - (size_t)o, "trails-probe:   type %02X [%u/%u] node %u:", ty, i, cnt, idx);
                for (u32 k = 0; k < 16u; k++) {
                    u32 w = ps2_r32(nr + 4u * k);
                    float fv; memcpy(&fv, &w, 4);
                    if ((w >> 23 & 0xFFu) > 0x60u && (w >> 23 & 0xFFu) < 0x9Fu) o += snprintf(l + o, sizeof l - (size_t)o, " %.3f", fv);
                    else o += snprintf(l + o, sizeof l - (size_t)o, " %08X", w);
                }
                ps2_log("%s", l);
            }
        }
    }
    for (u32 ty = 0x5Cu; ty <= 0x6Au; ty++) {
        char l[600]; int o = 0;
        o += snprintf(l + o, sizeof l - (size_t)o, "trails-probe: model %08X type %02X:", model, ty);
        for (u32 i = 0; i < 6u && o < 520; i++) {
            u32 id = zt_call3(ctx, 0x001337C0u, model, ty, i) & 0xFFFFu;
            o += snprintf(l + o, sizeof l - (size_t)o, " [%u] id %u", i, id);
            for (u32 tb = 0; tb < 4u; tb++) {
                u32 t = ps2_r32(0x003F78A0u + 4u * tb);
                if (!zt_ptr(t)) continue;
                u32 e = t + id * 16u;
                o += snprintf(l + o, sizeof l - (size_t)o, " T%u(%.2f %.2f %.2f %08X)", tb, zt_f(e), zt_f(e + 4u),
                              zt_f(e + 8u), ps2_r32(e + 12u));
                if (o > 520) break;
            }
        }
        ps2_log("%s", l);
    }
    return 0;
}

/* per-frame tick probe (ACZ_DEBUG_TRAILS_PROBE): the unit table of the player's team table (player+0x1A5C: *team =
   group slot array, team+0x1C groups; group+0x104 records of 0x4A0 bytes, +0x108 count; unit +0x260 position,
   +0x270 angles, +0x1C4 type -> kind +0x71) */
static int zt_probe_tick(ps2_ctx *ctx, void *user) {
    (void)user;
    u64 f = ps2_kernel_vblank_count();
    if ((f % 120u) != 0u) return 0;
    u32 player = ctx->r[4].uw[0] - 0x1310u;
    if (!zt_ptr(player)) return 0;
    u32 t = ps2_r32(player + 0x1A5Cu);
    if (!zt_ptr(t)) return 0;
    u32 garr = ps2_r32(t), ng = ps2_r8(t + 0x1Cu);
    if (!zt_ptr(garr)) return 0;
    ps2_log("trails-units: field %llu player %08X groups %u", (unsigned long long)f, player, ng);
    int shown = 0;
    for (u32 g = 0; g < ng && g < 64u; g++) {
        u32 grp = ps2_r32(garr + 4u * g);
        if (!zt_ptr(grp)) continue;
        u32 recs = ps2_r32(grp + 0x104u);
        int cnt = (int)ps2_r32(grp + 0x108u);
        for (int i = 0; i < cnt && i < 64 && shown < 60; i++) {
            u32 u = recs + (u32)i * 0x4A0u;
            if (!zt_ptr(u)) continue;
            u32 ty = ps2_r32(u + 0x1C4u), tt = zt_ptr(ty) ? ps2_r32(ty) : 0u;
            int kind = zt_ptr(tt) ? (int)ps2_r8(tt + 0x71u) : -1;
            shown++;
            ps2_log("trails-units:   %u/%d %08X kind %d/%u hp %d fl %08X pos %.0f %.0f %.0f ang %.3f %.3f %.3f | +200 %.3g %.3g %.3g %.3g "
                    "+210 %.3g %.3g %.3g %.3g +220 %.3g %.3g %.3g %.3g +230 %.3g %.3g %.3g %.3g +240 %.3g %.3g %.3g %.3g +250 %.3g %.3g "
                    "%.3g %.3g +280 %.3g %.3g %.3g %.3g +290 %.3g %.3g %.3g %.3g +2A0 %.3g %.3g %.3g %.3g",
                    g, i, u, kind, ps2_r8(u + 0x39Bu), (int)(s16)ps2_r16(u + 0x398u), ps2_r32(u + 0x1F8u),
                    zt_f(u + 0x260u), zt_f(u + 0x264u), zt_f(u + 0x268u), zt_f(u + 0x270u), zt_f(u + 0x274u), zt_f(u + 0x278u),
                    zt_f(u + 0x200u), zt_f(u + 0x204u), zt_f(u + 0x208u), zt_f(u + 0x20Cu), zt_f(u + 0x210u), zt_f(u + 0x214u),
                    zt_f(u + 0x218u), zt_f(u + 0x21Cu), zt_f(u + 0x220u), zt_f(u + 0x224u), zt_f(u + 0x228u), zt_f(u + 0x22Cu),
                    zt_f(u + 0x230u), zt_f(u + 0x234u), zt_f(u + 0x238u), zt_f(u + 0x23Cu), zt_f(u + 0x240u), zt_f(u + 0x244u),
                    zt_f(u + 0x248u), zt_f(u + 0x24Cu), zt_f(u + 0x250u), zt_f(u + 0x254u), zt_f(u + 0x258u), zt_f(u + 0x25Cu),
                    zt_f(u + 0x280u), zt_f(u + 0x284u), zt_f(u + 0x288u), zt_f(u + 0x28Cu), zt_f(u + 0x290u), zt_f(u + 0x294u),
                    zt_f(u + 0x298u), zt_f(u + 0x29Cu), zt_f(u + 0x2A0u), zt_f(u + 0x2A4u), zt_f(u + 0x2A8u), zt_f(u + 0x2ACu));
        }
    }
    return 0;
}

/* ---- the sampler ----
   Every aircraft the game updates goes through its burner setup 0x1386D0 once per game frame (M1: 21 calls, the
   player twice -- two render objects -- and the wingman, bombers and fighters at any distance once each; M3 before
   the enemies come: 2). a0 = the render object (+4 the model), a2 = the position vector (the unit record +0x260; the
   player's +0x70), a3 = the angles (pitch, yaw, roll: +0x10 after the position; world = R(angles) * body, body x
   right, y up, z back). The model's typed nodes (0x133410(model, type, &count) -> u16 node indices; 64-byte node
   records at *(model+0xC), position x, y, z at +0x20, the node's type at +0x10 low byte, its effect parameter index
   at +0x12): 0x63 the burner nozzles (F-5E: +-1.31, -0.02, 22.79), 0x66 the wing tips (F-5E: +-19.8, -0.8, 5.4),
   0x60 the lights, 0x67 the pylons. The HUD manager update (0x24D9D8, a0 - 0x1310 = the player) is the frame tick. */
#define ZT_BURNER   0x001386D0u
#define ZT_NODES    0x00133410u
#define ZT_HUD_TICK 0x0024D9D8u
#define ZT_UPM_ALT  5.12f            /* game units per metre of the altimeter (and zero_vr.c's G) */
#define ZT_UPM_SPD  6.65f            /* ... of the airspeed (1109 units/s at 324 kt) */
#define ZT_MAX_AC   48
#define ZT_MAX_TR   96                   /* (64 before the cloud wake: up to 3 more trails per aircraft near clouds) */
#define ZT_RING     256
#define ZT_MAX_MOD  64

extern volatile float ps2_imm_freeze_m;  /* ps2_video.c: the mission's freezing level (zero_vr.c) */
int zero_instr_get(zero_instr *out);
float ps2_clouds_units_per_m(void);

static int zt_on;                        /* anything to do */
static int zt_on_boot;                   /* ... at the start: the hooks are in */
static int zt_dbg;
static float zt_upm = 6.0f;              /* sizes: the clouds' units per metre */
static float zt_dT;                      /* ACZ_VR_CONTRAILS_DT: added to the air temperature (K; test) */
static float zt_rh_fix = -1.0f;          /* ACZ_VR_TRAILS_RH: a fixed humidity (0..1; test) */
static float zt_life_k = 1.0f;           /* ACZ_VR_CONTRAILS_LIFE */
static float zt_haze_m = 300.0f;         /* ACZ_VR_HEAT_HAZE_RANGE (m) */
static int zt_own_vapour = 1;            /* ACZ_VR_VAPOUR_OWN */
static float zt_vap_g[2] = { 4.5f, 8.0f };   /* ACZ_VR_VAPOUR_G: load factor where it starts / is full */
/* the cloud wake (ACZ_VR_CLOUD_WAKE; physics and numbers in docs/research/2026-10-29/cloud-wake.md) */
static float zt_wk_drag_m = 220.0f;       /* ACZ_VR_CLOUD_WAKE_DRAG: e-folding length of the cloud dragged out (m) */
/* ACZ_VR_CLOUD_WAKE_LOOK (cl_pop 2026-10-31, bug #260: M10 11:28:11 "good to watch aircraft come out of clouds, but I
   don't see trails from the clouds"): 2 (default) = the wake sized to be seen at dogfight ranges -- the veil dragged
   out e-folds over 220 m (the humid shell around cumulus: < 100 m to > 1 km observed), starts 1.8 spans wide (the
   wake oval with its rolled-up outer part, 2.09 b0) and spreads 2.4 m/s, peak 0.9; the core threads condense up to
   600 m past the cloud at 0.8. In the 10-29 look (1) the tongue was ~200 m x 18 m: at 1-3 km a few pixels against
   its own cloud. The lifetimes (humidity) and the physics of when it forms are the same in both. */
static int zt_wk_look = 2;
static float zt_wk_w = 1.8f, zt_wk_spread = 2.4f, zt_wk_peak = 0.9f, zt_wk_reach = 600.0f, zt_wk_thr_a = 0.8f;
static float zt_wk_d[2] = { 0.01f, 0.035f };  /* ACZ_VR_CLOUD_WAKE_DENS: probe density where "inside" starts / is full
                                            (M3 cumulus: 0.03-0.3 inside with clear pockets, 0-0.01 just outside; M1's
                                            overcast 0.045-0.075) */
static float zt_wk_life_k = 1.0f;        /* ACZ_VR_CLOUD_WAKE_LIFE: x the evaporation time */
static int zt_wk_threads = 1;            /* ACZ_VR_CLOUD_WAKE_THREADS: the vortex-core threads */
static int zt_wk_others = 1;             /* ACZ_VR_CLOUD_WAKE_OTHERS: the other aircraft too */
static float zt_wk_range_m = 3000.0f;    /* ACZ_VR_CLOUD_WAKE_RANGE: other aircraft probed within this distance (m) */
static int zt_wk_test_all;               /* ACZ_VR_CLOUD_WAKE_TEST_ALL=1: the test window for every aircraft (cl_pop) */
static float zt_wk_test[4];              /* ACZ_VR_CLOUD_WAKE_TEST=from,to[,dens[,period]]: the own aircraft "inside" (test;
                                            with a period the window repeats every period fields: a wake after another) */
static float zt_wk_lat = 4.0f;           /* the probe's measured latency (fields), smoothed */
static u64 zt_st_wk_req, zt_st_wk_ans, zt_st_wk_exits, zt_st_wk_pts;
static float zt_st_wk_dmax;

typedef struct { u32 pos_a, ang_a, obj, model; } zt_call_t;
static zt_call_t zt_calls[ZT_MAX_AC];
static int zt_ncalls;

typedef struct {
    u32 model, sig;
    int ok, nnoz;
    float noz[4][3];                     /* nozzles (model units = game units) */
    float tip[3], span;                  /* a wing tip (x > 0) and the half span */
} zt_model_t;
static zt_model_t zt_mod[ZT_MAX_MOD];
static int zt_nmod;

typedef struct {
    u32 key;                             /* the position vector's address */
    u64 seen;                            /* last field */
    int own;
    float p[3], v[3];                    /* units; units per second */
    float hist[25][3]; int nh;           /* metres (5.12), newest first, for the load factor */
    float g, vap;                        /* load factor; vapour strength (smoothed) */
    int tr[4];                           /* trail per nozzle (-1) */
    float seed;
    /* the cloud wake */
    float wk_d, wk_in;                   /* our clouds' density about it (probe), "inside" 0..1 */
    float wk_src;                        /* the cloud at hand: inside its "inside", held and fading after it leaves */
    float wk_out_m;                      /* metres flown since it was last inside (1e9: not near a cloud yet) */
    u32 wk_field;                        /* the request field of the last probe answer used */
    int wk[3];                           /* trails: dragged cloud, left / right vortex-core thread (-1) */
} zt_ac_t;
static zt_ac_t zt_ac[ZT_MAX_AC];

typedef struct {
    int used, owner, noz;                /* owner: aircraft slot (-1 = orphan); noz: the nozzle (wake: 0..2) */
    int kind;                            /* pt_trail.kind: 0 contrail, 1 dragged cloud, 2 / 3 left / right thread */
    u32 id;
    float width_m;
    int n;
    pt_point pt[ZT_RING];                /* oldest first; pt[n-1] = the head (at the nozzle) */
} zt_trail_t;
static zt_trail_t zt_tr[ZT_MAX_TR];
static u32 zt_tr_id;

static pt_frame zt_frame;
static u64 zt_tick_field;
static u64 zt_st_ticks, zt_st_pts, zt_st_puffs, zt_st_haze, zt_st_trails_made;
static float zt_st_form_max, zt_st_alt_max;

static float zt_smooth(float a, float b, float x) {
    if (b == a) return x >= b ? 1.0f : 0.0f;
    float t = (x - a) / (b - a);
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    return t * t * (3.0f - 2.0f * t);
}
static void zt_euler(float pitch, float yaw, float roll, float m[3][3]) {   /* as zero_vr.c m3_euler: world = m body */
    float cp = cosf(pitch), sp = sinf(pitch), cy = cosf(yaw), sy = sinf(yaw), cr = cosf(roll), sr = sinf(roll);
    m[0][0] = cy * cr + sy * sp * sr; m[0][1] = -cy * sr + sy * sp * cr; m[0][2] = sy * cp;
    m[1][0] = cp * sr;                m[1][1] = cp * cr;                 m[1][2] = -sp;
    m[2][0] = -sy * cr + cy * sp * sr; m[2][1] = sy * sr + cy * sp * cr; m[2][2] = cy * cp;
}
static void zt_xf(const float m[3][3], const float p[3], const float b[3], float o[3]) {
    for (int i = 0; i < 3; i++) o[i] = p[i] + m[i][0] * b[0] + m[i][1] * b[1] + m[i][2] * b[2];
}

/* ---- air: temperature, pressure, humidity, Schmidt-Appleman ---- */
static float zt_ew(float t) { return 611.21f * expf(17.502f * t / (240.97f + t)); }     /* Pa over water (Buck) */
static float zt_ei(float t) { return 611.15f * expf(22.452f * t / (272.55f + t)); }     /* Pa over ice */
/* the mission's air at altitude alt_m: 0 C at the freezing level (zero_vr.c: 2700 m, 0 m in the snow missions), the
   standard lapse 6.5 K/km (as the canopy ice and the optics' halo) */
static float zt_temp_c(float alt_m) {
    float fz = ps2_imm_freeze_m;
    if (!(fz >= -2000.0f && fz < 20000.0f)) fz = 2700.0f;
    return -6.5f * (alt_m - fz) * 0.001f + zt_dT;
}
static float zt_press_pa(float alt_m) {
    float a = alt_m < 0.0f ? 0.0f : alt_m > 20000.0f ? 20000.0f : alt_m;
    return 101325.0f * powf(1.0f - 2.25577e-5f * a, 5.25588f);
}
/* The Schmidt-Appleman criterion (Schumann 1996): the mixing line of the exhaust (EI_H2O 1.25 kg/kg, Q 43.2 MJ/kg,
   overall efficiency 0.3) has the slope G = EI cp p / (eps Q (1 - eta)) Pa/K; T_LM = the threshold for saturated
   ambient air (approximation in deg C), T_LC for relative humidity rh over water: e_w(T_LM) - G (T_LM - T) = rh e_w(T).
   Returns the margin T_LC - T (K): > 0 = a contrail forms. */
static float zt_sa_margin(float alt_m, float t_c, float rh) {
    float G = 1.25f * 1004.0f * zt_press_pa(alt_m) / (0.622f * 43.2e6f * 0.7f);
    float lg = logf(G - 0.053f > 0.01f ? G - 0.053f : 0.01f);
    float tlm = -46.46f + 9.43f * lg + 0.720f * lg * lg;
    float tlc = tlm;
    if (rh < 0.999f) {
        float lo = tlm - 40.0f, hi = tlm, e0 = zt_ew(tlm);
        for (int i = 0; i < 24; i++) {
            float m = 0.5f * (lo + hi);
            float fm = e0 - G * (tlm - m) - rh * zt_ew(m);       /* > 0 at the warm end of the bracket */
            if (fm > 0.0f) hi = m; else lo = m;
        }
        tlc = 0.5f * (lo + hi);
    }
    return tlc - t_c;
}
/* the mission's humidity (over water) at altitude: the game keeps none, so from its weather: the cloud cover (C1, the
   coverage under the stage mask), precipitation, inside or near the cloud band humid, drier well above the tops */
static float zt_rh_at(const ps2_cloud_env *e, int ok, float alt_m) {
    if (zt_rh_fix >= 0.0f) return zt_rh_fix;
    float rh = 0.40f;
    if (ok) {
        float cov = e->coverage > 0.0f ? e->coverage : e->coverage_unmasked;     /* (the cover the mission shows) */
        cov = cov < 0.0f ? 0.0f : cov > 1.0f ? 1.0f : cov;
        rh += 0.35f * cov;
        if ((e->valid & CL_ENV_WEATHER) && e->precip > 0.0f) rh += 0.15f;
        if (e->valid & CL_ENV_PROFILE) {
            float b = e->base_m, t = e->top_m;
            if (alt_m > b - 300.0f && alt_m < t + 300.0f) rh = fmaxf(rh, 0.75f + 0.2f * cov);
            else if (alt_m > t + 1000.0f) rh -= 0.08f;
        }
    }
    return rh < 0.1f ? 0.1f : rh > 0.98f ? 0.98f : rh;
}

/* ---- models ---- */
static u32 zt_model_sig(u32 model) {
    u32 n = ps2_r32(model + 0xCu);
    return zt_ptr(n) ? ps2_r32(n + 0x20u) ^ (ps2_r32(n + 0x24u) * 3u) ^ n : 0u;
}
static int zt_nodes(ps2_ctx *ctx, u32 model, u32 type, float out[][3], int max) {
    u32 sp = (ctx->r[29].uw[0] - 0x100u) & ~15u, scratch = sp + 0xC0u;
    ps2_w32(scratch, 0u);
    u32 list = zt_call3(ctx, ZT_NODES, model, type, scratch);
    u32 cnt = ps2_r32(scratch), nodes = ps2_r32(model + 0xCu);
    int n = 0;
    if (!zt_ptr(list) || !zt_ptr(nodes) || !cnt || cnt > 64u) return 0;
    for (u32 i = 0; i < cnt && n < max; i++) {
        u32 nr = nodes + (u32)ps2_r16(list + 2u * i) * 64u;
        if (!zt_ptr(nr) || (ps2_r32(nr + 0x10u) & 0xFFu) != type) continue;
        float x = zt_f(nr + 0x20u), y = zt_f(nr + 0x24u), z = zt_f(nr + 0x28u);
        if (!(fabsf(x) < 300.0f && fabsf(y) < 300.0f && fabsf(z) < 300.0f)) continue;
        out[n][0] = x; out[n][1] = y; out[n][2] = z;
        n++;
    }
    return n;
}
static const zt_model_t *zt_model(ps2_ctx *ctx, u32 model) {
    u32 sig = zt_model_sig(model);
    zt_model_t *m = NULL;
    for (int i = 0; i < zt_nmod; i++) if (zt_mod[i].model == model) { m = &zt_mod[i]; break; }
    if (m && m->sig == sig) return m->ok ? m : NULL;
    if (!m) {
        if (zt_nmod >= ZT_MAX_MOD) { zt_nmod = 0; }              /* (a full table: start over) */
        m = &zt_mod[zt_nmod++];
    }
    memset(m, 0, sizeof *m);
    m->model = model; m->sig = sig;
    float b[16][3];
    m->nnoz = zt_nodes(ctx, model, 0x63u, m->noz, 4);
    /* the wing tip: the outermost of the wing-tip nodes (0x66) and the lights (0x60, the navigation lights sit on the
       tips); one type alone is not enough (the Mirage 2000's 0x66 nodes sit 0.9 units off the centre line) */
    int nt = zt_nodes(ctx, model, 0x66u, b, 16);
    nt += zt_nodes(ctx, model, 0x60u, b + nt, 16 - nt);
    for (int i = 0; i < nt; i++) if (b[i][0] > m->span) { m->span = b[i][0]; memcpy(m->tip, b[i], sizeof m->tip); }
    if (m->span < 8.0f) m->span = 0.0f;                    /* (no wing found: no vapour) */
    m->ok = m->nnoz > 0 || m->span > 0.0f;
    if (zt_dbg)
        ps2_log("zero-trails: model %08X: %d nozzles (%.2f %.2f %.2f) wing tip %.2f %.2f %.2f", model, m->nnoz,
                m->nnoz ? m->noz[0][0] : 0.0f, m->nnoz ? m->noz[0][1] : 0.0f, m->nnoz ? m->noz[0][2] : 0.0f,
                m->tip[0], m->tip[1], m->tip[2]);
    return m->ok ? m : NULL;
}

static u32 zt_cur_model[ZT_MAX_AC];      /* per call: the model (for the tick) */
static int zt_burner(ps2_ctx *ctx, void *user) {
    (void)user;
    u32 a0 = ctx->r[4].uw[0], a2 = ctx->r[6].uw[0], a3 = ctx->r[7].uw[0];
    if (!zt_ptr(a0) || !zt_ptr(a2) || !zt_ptr(a3)) return 0;
    u32 model = ps2_r32(a0 + 4u);
    if (!zt_ptr(model)) return 0;
    for (int i = 0; i < zt_ncalls; i++) if (zt_calls[i].pos_a == a2) return 0;    /* (the player: two objects) */
    if (zt_ncalls >= ZT_MAX_AC) return 0;
    if (!zt_model(ctx, model)) return 0;
    zt_calls[zt_ncalls].pos_a = a2; zt_calls[zt_ncalls].ang_a = a3; zt_calls[zt_ncalls].obj = a0;
    zt_calls[zt_ncalls].model = model;
    zt_ncalls++;
    return 0;
}

/* ---- trails ---- */
static int zt_trail_new_k(int owner, int noz, float width_m, int kind) {
    for (int i = 0; i < ZT_MAX_TR; i++)
        if (!zt_tr[i].used) {
            zt_trail_t *t = &zt_tr[i];
            t->used = 1; t->owner = owner; t->noz = noz; t->id = ++zt_tr_id; t->width_m = width_m; t->n = 0;
            t->kind = kind;
            zt_st_trails_made++;
            return i;
        }
    return -1;
}
static int zt_trail_new(int owner, int noz, float width_m) { return zt_trail_new_k(owner, noz, width_m, 0); }
static void zt_trail_decimate(zt_trail_t *t) {          /* full: every other point of the older half goes */
    int half = t->n / 2, w = 0;
    for (int r = 0; r < t->n; r++) {
        if (r < half && (r & 1)) continue;
        t->pt[w++] = t->pt[r];
    }
    t->n = w;
}
static void zt_trail_prune(zt_trail_t *t, double now) {
    int k = 0;
    while (k < t->n - 1 && (float)(now - t->pt[k].birth_s) > t->pt[k].life_s + 1.0f
           && (float)(now - t->pt[k + 1].birth_s) > t->pt[k + 1].life_s) k++;
    if (k) { memmove(t->pt, t->pt + k, (size_t)(t->n - k) * sizeof t->pt[0]); t->n -= k; }
    if (t->owner < 0 && (t->n < 2 || (float)(now - t->pt[t->n - 1].birth_s) > t->pt[t->n - 1].life_s + 1.0f)) t->used = 0;
}
/* one nozzle's trail this frame: a = formation opacity (0 = none) */
static void zt_trail_step(zt_ac_t *ac, int slot, int k, const float pw[3], float a, float life, float spread, double now,
                          float width_m) {
    int ti = ac->tr[k];
    if (ti >= 0 && (!zt_tr[ti].used || zt_tr[ti].owner != slot)) ti = ac->tr[k] = -1;
    if (ti >= 0) {
        zt_trail_t *t = &zt_tr[ti];
        pt_point *h = &t->pt[t->n - 1], *q = &t->pt[t->n - 2];
        float d[3] = { pw[0] - q->p[0], pw[1] - q->p[1], pw[2] - q->p[2] };
        float dl = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) / zt_upm;
        float jump[3] = { pw[0] - h->p[0], pw[1] - h->p[1], pw[2] - h->p[2] };
        float jl = sqrtf(jump[0] * jump[0] + jump[1] * jump[1] + jump[2] * jump[2]) / zt_upm;
        if (jl > 400.0f || (a < 0.004f && q->a0 < 0.004f && h->a0 < 0.004f)) {   /* a cut, or not forming: let it go */
            t->owner = -1; ac->tr[k] = -1; ti = -1;
        } else {
            memcpy(h->p, pw, sizeof h->p);
            h->birth_s = (float)now; h->a0 = a; h->life_s = life; h->spread = spread; h->len_m = q->len_m + dl;
            h->T = q->T;
            if (dl >= 25.0f || (float)now - q->birth_s >= 0.4f) {               /* fix the head, start a new one */
                if (t->n >= ZT_RING) zt_trail_decimate(t);
                t->pt[t->n] = t->pt[t->n - 1];
                t->n++;
                zt_st_pts++;
            }
            return;
        }
    }
    if (a < 0.004f) return;
    ti = zt_trail_new(slot, k, width_m);
    if (ti < 0) return;
    ac->tr[k] = ti;
    zt_trail_t *t = &zt_tr[ti];
    pt_point p0;
    memset(&p0, 0, sizeof p0);                           /* (no drift: dv, t_stop 0) */
    memcpy(p0.p, pw, sizeof p0.p);
    p0.birth_s = (float)now; p0.a0 = a; p0.life_s = life; p0.spread = spread; p0.len_m = 0.0f; p0.T = ps2_vk_sun_T(pw);
    t->pt[0] = p0; t->pt[1] = p0; t->n = 2;
}

/* ---- the cloud wake (ACZ_VR_CLOUD_WAKE) ----
   Leaving one of our clouds, an aircraft's wake carries cloud air out with it: the vortex pair rolls up the cloud's
   droplets into a veil about one wake oval wide (~1.4 spans) that thins over ~100 m past the cloud's edge
   (the cloud at hand x exp(-distance / ACZ_VR_CLOUD_WAKE_DRAG)); in the humid shell around the cloud (RH falling from
   100 % with an e-folding of ~150 m) the two vortex cores, ~2.5 K x n^2 cooler, condense into thin threads, longer in a pull
   (the core's cooling grows with the load factor squared). Everything sinks with the vortex pair (w0 ~ 1.8 m/s x the
   load factor, against the lift, until the pair links after ~4 t0 = 4 b0 / w0), spreads, and evaporates in the time
   a 12 um droplet needs in the mixed air (0.85 s / (1 - RH)). Numbers: docs/research/2026-10-29/cloud-wake.md. */
static void zt_wake_step(zt_ac_t *ac, int slot, int j, int kind, const float pw[3], float a, float life, float spread,
                         const float dv[3], float t_stop, double now, float width_m) {
    int ti = ac->wk[j];
    if (ti >= 0 && (!zt_tr[ti].used || zt_tr[ti].owner != slot || zt_tr[ti].kind != kind)) ti = ac->wk[j] = -1;
    if (ti >= 0) {
        zt_trail_t *t = &zt_tr[ti];
        pt_point *h = &t->pt[t->n - 1], *q = &t->pt[t->n - 2];
        float d[3] = { pw[0] - q->p[0], pw[1] - q->p[1], pw[2] - q->p[2] };
        float dl = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) / zt_upm;
        float jump[3] = { pw[0] - h->p[0], pw[1] - h->p[1], pw[2] - h->p[2] };
        float jl = sqrtf(jump[0] * jump[0] + jump[1] * jump[1] + jump[2] * jump[2]) / zt_upm;
        if (jl > 400.0f || (a < 0.004f && q->a0 < 0.004f && h->a0 < 0.004f)) {   /* a cut, or nothing left to drag */
            t->owner = -1; ac->wk[j] = -1; ti = -1;
        } else {
            memcpy(h->p, pw, sizeof h->p);
            h->birth_s = (float)now; h->a0 = a; h->life_s = life; h->spread = spread; h->len_m = q->len_m + dl;
            h->T = q->T; memcpy(h->dv, dv, sizeof h->dv); h->t_stop = t_stop;
            if (dl >= 8.0f || (float)now - q->birth_s >= 0.1f) {                 /* fix the head every 8 m / 0.1 s */
                if (t->n >= ZT_RING) zt_trail_decimate(t);
                t->pt[t->n] = t->pt[t->n - 1];
                t->n++;
                zt_st_wk_pts++;
            }
            return;
        }
    }
    if (a < 0.004f) return;
    ti = zt_trail_new_k(slot, j, width_m, kind);
    if (ti < 0) return;
    ac->wk[j] = ti;
    zt_trail_t *t = &zt_tr[ti];
    pt_point p0;
    memset(&p0, 0, sizeof p0);
    memcpy(p0.p, pw, sizeof p0.p);
    p0.birth_s = (float)now; p0.a0 = a; p0.life_s = life; p0.spread = spread; p0.len_m = 0.0f; p0.T = ps2_vk_sun_T(pw);
    memcpy(p0.dv, dv, sizeof p0.dv); p0.t_stop = t_stop;
    t->pt[0] = p0; t->pt[1] = p0; t->n = 2;
}

/* ---- the frame ---- */
static u64 zt_tick_ns;
static u64 zt_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec; }
static int zt_tick_body(ps2_ctx *ctx, u64 f);
static int zt_tick(ps2_ctx *ctx, void *user) {
    (void)user;
    u64 f = ps2_kernel_vblank_count();
    if (f == zt_tick_field) return 0;
    zt_tick_field = f;
    u64 t0 = zt_ns();
    zt_tick_body(ctx, f);
    zt_tick_ns += zt_ns() - t0;
    return 0;
}
static int zt_tick_body(ps2_ctx *ctx, u64 f) {
    const pt_cfg_t *cf = pt_cfg();
    u32 player = ctx->r[4].uw[0] - 0x1310u;
    double now = (double)f / 60.0;
    zt_st_ticks++;
    static ps2_cloud_env env;
    static int env_ok;
    if ((f % 30u) == 0u || !env_ok) env_ok = ps2_cloud_env_read(&env);
    float sun[3] = { 0.0f, 0.0f, 0.0f };
    if (env_ok && (env.valid & CL_ENV_SUN)) memcpy(sun, env.sun_dir, sizeof sun);
    {   /* the smoke light (rn_vp_fx.c): the sun above the horizon */
        pt_smoke_t s;
        memset(&s, 0, sizeof s);
        if (sun[1] > -0.02f) memcpy(s.sun, sun, sizeof s.sun);
        s.k = cf->smoke; s.fwd = 1.30f; s.back = 0.80f; s.shadow_k = 0.45f; s.seq = pt_smoke.seq + 1u;
        memcpy((void *)&pt_smoke, &s, sizeof s);
    }
    zero_instr ins;
    int ins_ok = zero_instr_get(&ins);
    float pp[3] = { 0.0f, 0.0f, 0.0f };
    int have_pp = 0;
    if (zt_ptr(player)) {
        for (int i = 0; i < 3; i++) pp[i] = zt_f(player + 0x70u + 4u * (u32)i);
        have_pp = 1;
    }
    {   /* a mission start, a RETRY or the aircraft put somewhere else (zero_vr.c's new flight, ps2_imm_flight: also a
           stage change that keeps the canopy): the old trails go */
        extern volatile u32 ps2_imm_flight;
        static u32 last_reset;
        u32 r = ps2_imm_flight;
        if (r != last_reset) {
            last_reset = r;
            memset(zt_tr, 0, sizeof zt_tr);
            memset(zt_ac, 0, sizeof zt_ac);
            zt_ncalls = 0;
        }
    }
    pt_frame *F = &zt_frame;
    F->ntrail = F->npt = F->npuff = F->nhaze = 0;
    float rh_own = 0.0f;
    static pt_wake_req wq[ZT_MAX_AC];                    /* the cloud wake's probe requests of this frame */
    static float wq_d[ZT_MAX_AC];
    u32 nwq = 0;
    /* the aircraft of this frame */
    for (int c = 0; c < zt_ncalls; c++) {
        const zt_call_t *cl = &zt_calls[c];
        const zt_model_t *md = NULL;
        for (int i = 0; i < zt_nmod; i++) if (zt_mod[i].model == cl->model && zt_mod[i].ok) { md = &zt_mod[i]; break; }
        if (!md) continue;
        int slot = -1, freei = -1;
        for (int i = 0; i < ZT_MAX_AC; i++) {
            if (zt_ac[i].key == cl->pos_a) { slot = i; break; }
            if (freei < 0 && (!zt_ac[i].key || f - zt_ac[i].seen > 120u)) freei = i;
        }
        if (slot < 0) {
            if (freei < 0) continue;
            slot = freei;
            zt_ac_t *n = &zt_ac[slot];
            for (int k = 0; k < 4; k++) if (n->tr[k] >= 0 && n->key && zt_tr[n->tr[k]].owner == slot) zt_tr[n->tr[k]].owner = -1;
            for (int k = 0; k < 3; k++) if (n->wk[k] >= 0 && n->key && zt_tr[n->wk[k]].owner == slot) zt_tr[n->wk[k]].owner = -1;
            memset(n, 0, sizeof *n);
            n->key = cl->pos_a;
            for (int k = 0; k < 4; k++) n->tr[k] = -1;
            for (int k = 0; k < 3; k++) n->wk[k] = -1;
            n->wk_out_m = 1e9f;
            n->seed = (float)((cl->pos_a >> 4) % 997u) * 0.731f;
        }
        zt_ac_t *ac = &zt_ac[slot];
        float p[3] = { zt_f(cl->pos_a), zt_f(cl->pos_a + 4u), zt_f(cl->pos_a + 8u) };
        float an[3] = { zt_f(cl->ang_a), zt_f(cl->ang_a + 4u), zt_f(cl->ang_a + 8u) };
        if (!(fabsf(p[0]) < 1e7f && fabsf(p[1]) < 1e7f && fabsf(p[2]) < 1e7f) || !(fabsf(an[0]) < 100.0f && fabsf(an[1]) < 100.0f))
            continue;
        ac->own = zt_ptr(player) && cl->pos_a == player + 0x70u;
        u64 gap = ac->seen ? f - ac->seen : 0u;
        if (gap >= 1u && gap <= 4u)
            for (int i = 0; i < 3; i++) ac->v[i] = (p[i] - ac->p[i]) * 60.0f / (float)gap;
        else memset(ac->v, 0, sizeof ac->v);
        memcpy(ac->p, p, sizeof p);
        ac->seen = f;
        float R[3][3];
        zt_euler(an[0], an[1], an[2], R);
        static float own_R[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };   /* (debug: bearings in the own body frame) */
        if (zt_ptr(player) && cl->pos_a == player + 0x70u) memcpy(own_R, R, sizeof own_R);
        /* load factor: own from the instruments; others from their path (felt acceleration along their up axis, as
           zero_vr.c's G, over a 0.2 s baseline: the AI's positions do not step evenly every frame) */
        {
            float pm[3] = { p[0] / ZT_UPM_ALT, p[1] / ZT_UPM_ALT, p[2] / ZT_UPM_ALT };
            if (gap != 1u) ac->nh = 0;
            memmove(ac->hist[1], ac->hist[0], 24 * sizeof ac->hist[0]);
            memcpy(ac->hist[0], pm, sizeof pm);
            if (ac->nh < 25) ac->nh++;
            if (ac->own && ins_ok) ac->g = ins.g;
            else if (ac->nh >= 25) {
                float a[3];
                for (int i = 0; i < 3; i++) a[i] = (ac->hist[0][i] - 2.0f * ac->hist[12][i] + ac->hist[24][i]) * 25.0f;
                a[1] += 9.81f;
                float gl = (a[0] * R[0][1] + a[1] * R[1][1] + a[2] * R[2][1]) / 9.81f;
                if (gl > -6.0f && gl < 12.0f) ac->g = 0.9f * ac->g + 0.1f * gl;
            } else ac->g = 1.0f;
        }
        float alt = p[1] / ZT_UPM_ALT, tc = zt_temp_c(alt);
        float rh = zt_rh_at(&env, env_ok, alt);
        if (ac->own) rh_own = rh;
        float spd = sqrtf(ac->v[0] * ac->v[0] + ac->v[1] * ac->v[1] + ac->v[2] * ac->v[2]) / ZT_UPM_SPD;
        float pwr = ac->own && ins_ok ? (ins.afterburner > 0.05f ? 1.0f : ins.throttle) : 0.75f;
        if (alt > zt_st_alt_max) zt_st_alt_max = alt;
        /* the cloud wake: our clouds' density about the aircraft (the probe, ~4 fields late: asked for where it will be),
           the cloud just left, the veil dragged out of it and the vortex-core threads in its humid shell */
        if (cf->wake > 0.0f && md->span > 0.0f && (ac->own || zt_wk_others)) {
            float dist = 0.0f;
            if (have_pp && !ac->own) { float d0 = p[0] - pp[0], d1 = p[1] - pp[1], d2 = p[2] - pp[2]; dist = sqrtf(d0 * d0 + d1 * d1 + d2 * d2) / zt_upm; }
            float span_m = 2.0f * md->span / zt_upm;
            /* (outside the cloud band our clouds have no density: no probe there -- clear air costs nothing) */
            int band = !(env_ok && (env.valid & CL_ENV_PROFILE)) || (alt > env.base_m - 150.0f && alt < env.top_m + 150.0f);
            if (dist < zt_wk_range_m && band && nwq < ZT_MAX_AC) {
                pt_wake_req *r = &wq[nwq];
                r->key = cl->pos_a; r->field = (u32)f;
                for (int i = 0; i < 3; i++) r->p[i] = p[i] + ac->v[i] * (zt_wk_lat + 21.0f) / 60.0f;  /* (+0.35 s: the hold) */
                r->r_m = fminf(fmaxf(0.4f * span_m, 2.0f), 8.0f);
                wq_d[nwq] = ac->own ? -1.0f : dist;
                nwq++;
            }
            float d = 0.0f, dmax = 0.0f;
            u32 fld = 0;
            int got = pt_wake_get(cl->pos_a, &d, &dmax, &fld);
            if (got && fld != ac->wk_field) {
                float lat = (float)(u32)((u32)f - fld);
                if (lat >= 1.0f && lat < 30.0f) zt_wk_lat = 0.92f * zt_wk_lat + 0.08f * lat;
                ac->wk_field = fld;
                ac->wk_d = d;
                zt_st_wk_ans++;
                if (d > zt_st_wk_dmax) zt_st_wk_dmax = d;
            } else if (!got || (u32)f - ac->wk_field > 30u) ac->wk_d = 0.0f;   /* (no answers: no cloud) */
            d = ac->wk_d;
            if ((ac->own || zt_wk_test_all) && zt_wk_test[1] > zt_wk_test[0]) {      /* test: forced inside */
                float ff = (float)f;
                if (zt_wk_test[3] > 0.0f && ff >= zt_wk_test[0]) ff = zt_wk_test[0] + fmodf(ff - zt_wk_test[0], zt_wk_test[3]);
                d = ff >= zt_wk_test[0] && ff < zt_wk_test[1] ? zt_wk_test[2] : 0.0f;
            }
            /* inside: rises at once, held over the clear pockets inside a cloud (falls over 0.35 s; the probe looks that much
               ahead, so the fall still comes where the aircraft leaves the cloud) */
            float was = ac->wk_in;
            float in = zt_smooth(zt_wk_d[0], zt_wk_d[1], d);
            if (in < was - 1.0f / 21.0f) in = was - 1.0f / 21.0f;
            ac->wk_in = in;
            float fly = spd / 60.0f * (float)(gap >= 1u && gap <= 4u ? gap : 1u);
            if (in > 0.5f) ac->wk_out_m = 0.0f;
            else if (ac->wk_out_m < 1e8f) ac->wk_out_m += fly;
            if (was > 0.5f && in <= 0.5f) zt_st_wk_exits++;
            /* the cloud at hand: inside its "inside"; outside it fades over ~1.5 s (several short exits in a row drag
               less each) */
            ac->wk_src = in >= ac->wk_src ? in : ac->wk_src * 0.989f;
            float rh_amb = fminf(rh, 0.92f), rh_mix = rh_amb + 0.5f * (1.0f - rh_amb);
            float life = fminf(fmaxf(0.85f / (1.0f - rh_mix), 2.5f), 30.0f) * zt_wk_life_k;
            float n = ac->g < 0.5f ? 0.5f : ac->g > 9.0f ? 9.0f : ac->g;
            float w0 = 1.8f * n, b0 = 0.785f * span_m;               /* the pair's descent (m/s), spacing (m) */
            float dv[3] = { -R[0][1] * w0, -R[1][1] * w0, -R[2][1] * w0 };   /* against the lift (body up) */
            float t_stop = 4.0f * b0 / w0;
            float go = zt_smooth(30.0f, 80.0f, spd) * cf->wake;
            /* the veil: the cloud air in the wake oval, from where the aircraft crosses the cloud's surface on; at the edge
               about as opaque as ~15 m of real cloud (~0.6-0.7: a real cumulus' visibility is a few tens of metres) */
            float av = ac->wk_src * zt_smooth(0.9f, 0.35f, in) * expf(-ac->wk_out_m / zt_wk_drag_m) * zt_wk_peak * go;
            {   float bl[3] = { 0.0f, -0.04f * md->span, 0.25f * md->span }, pw[3];
                zt_xf(R, p, bl, pw);
                zt_wake_step(ac, slot, 0, 1, pw, av > 1.0f ? 1.0f : av, life, zt_wk_spread, dv, t_stop, now, zt_wk_w * span_m);
            }
            /* the threads: the vortex cores condense where the air's RH beats what their cooling needs (~2.5 K x n^2 at a
               core of ~2.5 % of the span -- field-measured cores are 1-2 %: 83 % at 1 g, 50 % from 2 g); the shell's RH
               falls from 100 % at the cloud's edge (e-folding 150 m); only after a cloud (within ~400 m of it) -- in the
               open air a pull is the wing vapour's */
            if (zt_wk_threads) {
                float rh_s = rh_amb + (1.0f - rh_amb) * expf(-ac->wk_out_m / 150.0f);
                float need = fmaxf(expf(-0.19f * n * n), 0.5f);
                float at = zt_smooth(need - 0.02f, need + 0.04f, rh_s) * zt_smooth(0.6f, 0.2f, in)
                         * zt_smooth(zt_wk_reach, 0.375f * zt_wk_reach, ac->wk_out_m) * fminf(ac->wk_src / 0.25f, 1.0f)
                         * zt_wk_thr_a * go;
                for (int side = 0; side < 2; side++) {
                    float bl[3] = { (side ? 0.8f : -0.8f) * md->span, md->tip[1], md->tip[2] + 0.05f * md->span }, pw[3];
                    zt_xf(R, p, bl, pw);
                    zt_wake_step(ac, slot, 1 + side, 2 + side, pw, at > 1.0f ? 1.0f : at, 0.6f * life, 0.35f, dv, t_stop,
                                 now, 0.12f * span_m + 0.4f);
                }
            }
            if (zt_dbg && (f % (u64)zt_dbg) == 0u && (ac->own || in > 0.0f || ac->wk_out_m < 500.0f)) {
                float az = 0.0f, el = 0.0f;              /* (bearing in the own body frame: az + right, el + up, deg) */
                if (have_pp && !ac->own) {
                    float d[3] = { p[0] - pp[0], p[1] - pp[1], p[2] - pp[2] }, b[3];
                    for (int i = 0; i < 3; i++) b[i] = own_R[0][i] * d[0] + own_R[1][i] * d[1] + own_R[2][i] * d[2];
                    az = atan2f(b[0], -b[2]) * 57.2958f; el = atan2f(b[1], sqrtf(b[0] * b[0] + b[2] * b[2])) * 57.2958f;
                }
                ps2_log("cloud-wake: field %llu %s dens %.3f (max %.3f, lat %.1f fields) in %.2f src %.2f out %.0f m veil %.2f "
                        "life %.1f s rh %.2f g %.1f dist %.0f m az %.0f el %.0f trails %d %d %d", (unsigned long long)f,
                        ac->own ? "own" : "ac ", ac->wk_d, dmax, zt_wk_lat, in, ac->wk_src, ac->wk_out_m < 1e8f ? ac->wk_out_m : -1.0f,
                        av, life, rh, ac->g, dist, az, el, ac->wk[0], ac->wk[1], ac->wk[2]);
            }
        }
        /* contrails */
        if (cf->contrails > 0.0f && md->nnoz > 0) {
            float marg = zt_sa_margin(alt, tc, rh);
            float form = zt_smooth(0.0f, 2.5f, marg) * zt_smooth(40.0f, 90.0f, spd);
            float rhi = rh * zt_ew(tc) / zt_ei(tc);
            float life, spread;
            if (rhi >= 1.0f) {                   /* ice-supersaturated: persistent, spreading */
                float s = (rhi - 1.0f) / 0.15f; s = s > 1.0f ? 1.0f : s;
                life = 90.0f + 60.0f * s; spread = 0.8f + 2.0f * s;
            } else {                             /* evaporates: seconds */
                float s = (rhi - 0.5f) / 0.5f; s = s < 0.0f ? 0.0f : s;
                life = 2.0f + 18.0f * s * s; spread = 0.6f;
            }
            life *= zt_life_k;
            float a = form * (0.65f + 0.35f * (pwr < 0.4f ? 0.4f : pwr));
            if (form > zt_st_form_max) zt_st_form_max = form;
            for (int k = 0; k < md->nnoz && k < 4; k++) {
                float pw[3];
                zt_xf(R, p, md->noz[k], pw);
                zt_trail_step(ac, slot, k, pw, a, life, spread, now, 3.0f);
            }
            if (zt_dbg && (f % (u64)zt_dbg) == 0u && (ac->own || form > 0.0f))
                ps2_log("zero-trails: field %llu %s alt %.0f m T %.1f C rh %.2f rhi %.2f SA margin %+.1f K form %.2f life %.0f s "
                        "speed %.0f m/s power %.2f g %.1f vap %.2f trails %d %d | weather cov %.2f/%.2f precip %.2f band %.0f..%.0f m "
                        "valid %X | tick %.1f us", (unsigned long long)f, ac->own ? "own" : "ac ", alt, tc, rh, rhi, marg, form, life, spd,
                        pwr, ac->g, ac->vap, ac->tr[0], ac->tr[1], env.coverage, env.coverage_unmasked, env.precip, env.base_m,
                        env.top_m, env.valid, zt_st_ticks ? (double)zt_tick_ns / (double)zt_st_ticks * 1e-3 : 0.0);
        }
        /* wing vapour: a hard pull in humid, not too cold air */
        if (cf->vapour > 0.0f && md->span > 0.0f && (!ac->own || zt_own_vapour)) {
            float want = zt_smooth(zt_vap_g[0], zt_vap_g[1], ac->g) * zt_smooth(0.55f, 0.90f, rh)
                       * zt_smooth(-18.0f, -6.0f, tc) * zt_smooth(100.0f, 160.0f, spd);
            float rate = want > ac->vap ? 0.25f : 0.08f;           /* forms fast, lingers a little */
            ac->vap += (want - ac->vap) * rate;
            float dist = 0.0f;
            if (have_pp) { float d0 = p[0] - pp[0], d1 = p[1] - pp[1], d2 = p[2] - pp[2]; dist = sqrtf(d0 * d0 + d1 * d1 + d2 * d2) / zt_upm; }
            if (ac->vap > 0.01f && dist < 3000.0f) {
                /* a sheet over each wing's upper surface: three overlapping flat ellipsoids between 28 % and 72 % of the
                   half span, just above the tip node's height, about the tip's chord position, stretched aft as it grows */
                float b = md->span, s = ac->vap * cf->vapour;
                for (int side = -1; side <= 1; side += 2)
                    for (int j = 0; j < 3 && F->npuff < PT_MAX_PUFFS; j++) {
                        static const float fx[3] = { 0.28f, 0.50f, 0.72f };
                        float grow = 0.8f + 0.6f * (s > 1.0f ? 1.0f : s);
                        float bl[3] = { side * fx[j] * b, md->tip[1] + 0.03f * b,
                                        md->tip[2] - 0.04f * b + 0.10f * b * grow };
                        float ha = 0.20f * b * grow, hs = 0.16f * b, hu = 0.035f * b;
                        pt_puff *q = &F->puff[F->npuff++];
                        zt_xf(R, p, bl, q->p);
                        for (int i = 0; i < 3; i++) {
                            q->ax[i] = R[i][2] * ha;          /* body +z: aft */
                            q->sp[i] = R[i][0] * hs;          /* body x: along the span */
                            q->up[i] = R[i][1] * hu;          /* body y: up */
                        }
                        q->alpha = (s > 1.0f ? 1.0f : s) * (0.50f - 0.06f * (float)j);
                        q->seed = ac->seed + (float)(j * 7 + (side > 0 ? 3 : 0));
                        zt_st_puffs++;
                    }
            }
        }
        /* heat haze behind the nozzles: near aircraft and the own */
        if (cf->haze > 0.0f && md->nnoz > 0 && have_pp) {
            float d0 = p[0] - pp[0], d1 = p[1] - pp[1], d2 = p[2] - pp[2];
            float dist = sqrtf(d0 * d0 + d1 * d1 + d2 * d2) / zt_upm;
            if (ac->own || dist < zt_haze_m) {
                float k = ac->own && ins_ok ? 0.25f + 0.35f * zt_smooth(0.4f, 1.0f, ins.throttle) + 0.40f * ins.afterburner : 0.55f;
                for (int n = 0; n < md->nnoz && n < 4 && F->nhaze < PT_MAX_HAZE; n++) {
                    pt_haze *h = &F->haze[F->nhaze++];
                    zt_xf(R, p, md->noz[n], h->p);
                    for (int i = 0; i < 3; i++) h->dir[i] = R[i][2];
                    h->r = 0.45f * zt_upm;
                    h->len = (5.0f + 9.0f * k) * zt_upm;
                    h->k = k * (ac->own ? 1.0f : 1.0f - zt_smooth(0.6f * zt_haze_m, zt_haze_m, dist));
                    h->seed = ac->seed + (float)n * 3.1f;
                    h->own = (u32)ac->own;
                    zt_st_haze++;
                }
            }
        }
    }
    zt_ncalls = 0;
    /* trails: prune, orphan the trails of aircraft not seen, refresh the cloud shadow of up to 256 points (round robin:
       our clouds drift slowly), publish */
    static u32 t_rr;
    u32 t_budget = 256u;
    for (int i = 0; i < ZT_MAX_TR; i++) {
        zt_trail_t *t = &zt_tr[i];
        if (!t->used) continue;
        for (int k = 0; k < t->n && t_budget; k++)
            if (((u32)k + t_rr) % 16u == 0u) { t->pt[k].T = ps2_vk_sun_T(t->pt[k].p); t_budget--; }
        if (t->owner >= 0 && (zt_ac[t->owner].seen + 2u < f || (t->kind ? zt_ac[t->owner].wk[t->noz] : zt_ac[t->owner].tr[t->noz]) != i))
            t->owner = -1;
        zt_trail_prune(t, now);
        if (!t->used || t->n < 2) continue;
        if (F->ntrail >= PT_MAX_TRAILS || F->npt + (u32)t->n > PT_MAX_PTS) continue;
        pt_trail *o = &F->trail[F->ntrail++];
        o->id = t->id; o->first = F->npt; o->n = (u32)t->n; o->width_m = t->width_m; o->shadow = 1.0f; o->kind = (uint32_t)t->kind;
        memcpy(&F->pt[F->npt], t->pt, (size_t)t->n * sizeof t->pt[0]);
        F->npt += (u32)t->n;
    }
    t_rr++;
    if (cf->wake > 0.0f) {                     /* the probe requests: the own aircraft first, then the nearest others */
        if (nwq > PT_WAKE_MAX) {
            for (u32 i = 0; i < PT_WAKE_MAX; i++)
                for (u32 j = i + 1u; j < nwq; j++)
                    if (wq_d[j] < wq_d[i]) {
                        pt_wake_req tq = wq[i]; wq[i] = wq[j]; wq[j] = tq;
                        float td = wq_d[i]; wq_d[i] = wq_d[j]; wq_d[j] = td;
                    }
            nwq = PT_WAKE_MAX;
        }
        pt_wake_request(wq, nwq);
        zt_st_wk_req += nwq;
    }
    F->field = (u32)f; F->now_s = now; F->upm = zt_upm; memcpy(F->sun, sun, sizeof sun); F->rh = rh_own;
    pt_publish(F);
    return 0;
}

static void zt_report(void) {
    if (!zt_on) return;
    ps2_log("zero-trails: %llu frames (%.1f us each on the game thread), %llu trails started, %llu points fixed, %llu vapour "
            "puffs, %llu haze plumes; highest aircraft %.0f m, best contrail formation %.2f", (unsigned long long)zt_st_ticks,
            zt_st_ticks ? (double)zt_tick_ns / (double)zt_st_ticks * 1e-3 : 0.0,
            (unsigned long long)zt_st_trails_made, (unsigned long long)zt_st_pts, (unsigned long long)zt_st_puffs,
            (unsigned long long)zt_st_haze, zt_st_alt_max, zt_st_form_max);
    if (pt_cfg()->wake > 0.0f)
        ps2_log("zero-trails: cloud wake: %llu probe requests, %llu answers (latency %.1f fields), densest %.3f, %llu cloud exits, "
                "%llu wake points fixed", (unsigned long long)zt_st_wk_req, (unsigned long long)zt_st_wk_ans, zt_wk_lat,
                zt_st_wk_dmax, (unsigned long long)zt_st_wk_exits, (unsigned long long)zt_st_wk_pts);
}

/* ACZ_DEBUG_TRAILS_FNS="addr,addr,..." (research): call counts of game functions every 120 fields, with a caller */
#define ZT_FN_MAX 32
static u32 zt_fn_addr[ZT_FN_MAX], zt_fn_ra[ZT_FN_MAX], zt_fn_a0[ZT_FN_MAX];
static u64 zt_fn_n[ZT_FN_MAX], zt_fn_tot[ZT_FN_MAX], zt_fn_field;
static int zt_fn_count;
static int zt_fn_hook(ps2_ctx *ctx, void *user) {
    uintptr_t i = (uintptr_t)user;
    u64 f = ps2_kernel_vblank_count();
    if (f / 120u != zt_fn_field / 120u) {
        char l[900]; int o = 0;
        for (int k = 0; k < zt_fn_count && o < 820; k++)
            if (zt_fn_n[k]) o += snprintf(l + o, sizeof l - (size_t)o, " %06X:%llu(ra %06X a0 %08X)", zt_fn_addr[k],
                                          (unsigned long long)zt_fn_n[k], zt_fn_ra[k], zt_fn_a0[k]);
        if (o) ps2_log("trails-fns: fields %llu..: %s", (unsigned long long)(zt_fn_field / 120u * 120u), l);
        memset(zt_fn_n, 0, sizeof zt_fn_n);
        zt_fn_field = f;
    }
    if (i < (uintptr_t)zt_fn_count) {
        if (!zt_fn_n[i]) { zt_fn_ra[i] = ctx->r[31].uw[0]; zt_fn_a0[i] = ctx->r[4].uw[0]; }
        zt_fn_n[i]++; zt_fn_tot[i]++;
    }
    return 0;
}

/* ACZ_DEBUG_TRAILS_CLIMB=alt_m[,from_field[,bank_deg]] (test only): a crude autopilot on the left stick for scripted
   runs to the contrail levels -- from that field on (default 1000) it climbs (up to 30 deg) towards alt_m by the
   instruments, and within 300 m of it holds the given bank (a turn: the own trail curves into view). Stick commands
   jump the game's dead zone (64 of 127) as ACZ_DEBUG_LOWFLY's do. It overrides the left stick. */
static struct { int on; float alt, bank; u64 from; } zt_cl;
static int zt_climb_steer(u8 *lx, u8 *ly) {
    if (!zt_cl.on || ps2_kernel_vblank_count() < zt_cl.from) return 0;
    zero_instr v;
    if (!zero_instr_get(&v) || !v.valid) return 0;
    float alt = v.alt_ft * 0.3048f;
    float tp = (zt_cl.alt - alt) * 0.02f;
    tp = tp < -10.0f ? -10.0f : tp > 30.0f ? 30.0f : tp;
    float bank = fabsf(zt_cl.alt - alt) < 300.0f ? zt_cl.bank : 0.0f;
    float sy = (tp - v.pitch_deg) * 0.08f + fabsf(bank) * 0.012f;
    float sx = (bank + v.roll_deg) * 0.05f;
    sy = sy > 1.0f ? 1.0f : sy < -1.0f ? -1.0f : sy;
    sx = sx > 1.0f ? 1.0f : sx < -1.0f ? -1.0f : sx;
    int ix = fabsf(sx) < 0.05f ? 0 : (int)copysignf(60.0f + 60.0f * fabsf(sx), sx);
    int iy = fabsf(sy) < 0.05f ? 0 : (int)copysignf(60.0f + 60.0f * fabsf(sy), sy);
    *lx = (u8)(128 + ix); *ly = (u8)(128 + iy);
    return 1;
}

/* the NEXT MISSION tier (ps2_menv.h): the sampler follows the switches, when its hooks are in (enh_boot_tab's group) */
void zero_trails_menv_reload(void) {
    extern void pt_menv_reload(void);
    pt_menv_reload();
    const pt_cfg_t *cf = pt_cfg();
    if (zt_on_boot) zt_on = cf->contrails > 0.0f || cf->vapour > 0.0f || cf->haze > 0.0f || cf->wake > 0.0f;
}
void zero_trails_install(void) {
    const pt_cfg_t *cf = pt_cfg();
    const char *e = getenv("ACZ_DEBUG_TRAILS_PROBE");
    zt_probe_n = e ? atoi(e) : 0;
    zt_dbg = cf->debug;
    zt_dT = env_or("ACZ_VR_CONTRAILS_DT", 0.0f);
    zt_rh_fix = env_or("ACZ_VR_TRAILS_RH", -1.0f);
    zt_life_k = env_or("ACZ_VR_CONTRAILS_LIFE", 1.0f);
    if (!(zt_life_k > 0.05f)) zt_life_k = 1.0f;
    zt_haze_m = env_or("ACZ_VR_HEAT_HAZE_RANGE", 300.0f);
    zt_own_vapour = env_or("ACZ_VR_VAPOUR_OWN", 1.0f) > 0.0f;
    {   const char *g = getenv("ACZ_VR_VAPOUR_G");
        float a, b;
        if (g && sscanf(g, "%f,%f", &a, &b) == 2 && b > a) { zt_vap_g[0] = a; zt_vap_g[1] = b; }
    }
    zt_wk_test_all = env_or("ACZ_VR_CLOUD_WAKE_TEST_ALL", 0.0f) > 0.0f;
    zt_wk_look = env_or("ACZ_VR_CLOUD_WAKE_LOOK", 2.0f) >= 1.5f ? 2 : 1;
    if (zt_wk_look == 1) { zt_wk_w = 1.4f; zt_wk_spread = 1.4f; zt_wk_peak = 0.85f; zt_wk_reach = 400.0f; zt_wk_thr_a = 0.7f; }
    zt_wk_drag_m = env_or("ACZ_VR_CLOUD_WAKE_DRAG", zt_wk_look == 1 ? 110.0f : 220.0f);
    if (!(zt_wk_drag_m >= 5.0f && zt_wk_drag_m <= 1000.0f)) zt_wk_drag_m = zt_wk_look == 1 ? 110.0f : 220.0f;
    zt_wk_life_k = env_or("ACZ_VR_CLOUD_WAKE_LIFE", 1.0f);
    if (!(zt_wk_life_k >= 0.1f && zt_wk_life_k <= 10.0f)) zt_wk_life_k = 1.0f;
    zt_wk_threads = env_or("ACZ_VR_CLOUD_WAKE_THREADS", 1.0f) > 0.0f;
    zt_wk_others = env_or("ACZ_VR_CLOUD_WAKE_OTHERS", 1.0f) > 0.0f;
    zt_wk_range_m = env_or("ACZ_VR_CLOUD_WAKE_RANGE", 3000.0f);
    {   const char *g = getenv("ACZ_VR_CLOUD_WAKE_DENS");
        float a, b;
        if (g && sscanf(g, "%f,%f", &a, &b) == 2 && a >= 0.0f && b > a) { zt_wk_d[0] = a; zt_wk_d[1] = b; }
        g = getenv("ACZ_VR_CLOUD_WAKE_TEST");
        float c = 0.5f, per = 0.0f;
        if (g && sscanf(g, "%f,%f,%f,%f", &a, &b, &c, &per) >= 2 && b > a) {
            zt_wk_test[0] = a; zt_wk_test[1] = b; zt_wk_test[2] = c; zt_wk_test[3] = per > b - a ? per : 0.0f;
        }
    }
    zt_upm = ps2_clouds_units_per_m();
    if (!(zt_upm > 1.0f && zt_upm < 20.0f)) zt_upm = 6.0f;
    if (zt_probe_n > 0 && (ps2_r32(ZT_BURNER) >> 16) == 0x27BDu) {
        int r = ps2_hook_before(ZT_BURNER, zt_probe_burner, NULL, 60, "zero-trails");
        ps2_log("zero-trails: burner probe %s", r >= 0 ? "on" : "FAILED");
        ps2_hook_before(ZT_HUD_TICK, zt_probe_tick, NULL, 55, "zero-trails");
    }
    {   const char *fl = getenv("ACZ_DEBUG_TRAILS_FNS");
        for (const char *q = fl; q && *q && zt_fn_count < ZT_FN_MAX; ) {
            char *end;
            unsigned long a = strtoul(q, &end, 16);
            if (end == q) break;
            if (a >= 0x100000u && a < 0x400000u) {
                zt_fn_addr[zt_fn_count] = (u32)a;
                ps2_hook_before((u32)a, zt_fn_hook, (void *)(uintptr_t)zt_fn_count, 40, "zero-trails-probe");
                zt_fn_count++;
            }
            q = *end ? end + 1 : end;
        }
    }
    {   const char *c = getenv("ACZ_DEBUG_TRAILS_CLIMB");
        if (c && atof(c) > 100.0f) {
            extern int (*ps2_pad_test_steer)(u8 *lx, u8 *ly);
            zt_cl.alt = (float)atof(c); zt_cl.from = 1000u;
            const char *k = strchr(c, ',');
            if (k) { zt_cl.from = strtoull(k + 1, NULL, 0); k = strchr(k + 1, ','); if (k) zt_cl.bank = (float)atof(k + 1); }
            zt_cl.on = 1;
            ps2_pad_test_steer = zt_climb_steer;
            ps2_log("zero-trails: debug: climb autopilot to %.0f m from field %llu, then bank %.0f (ACZ_DEBUG_TRAILS_CLIMB)",
                    zt_cl.alt, (unsigned long long)zt_cl.from, zt_cl.bank);
        }
    }
    zt_on = cf->contrails > 0.0f || cf->vapour > 0.0f || cf->haze > 0.0f || cf->wake > 0.0f;
    if ((ps2_r32(ZT_BURNER) >> 16) != 0x27BDu || (ps2_r32(ZT_HUD_TICK) >> 16) != 0x27BDu) {
        ps2_log("zero-trails: code mismatch (%08X %08X): off", ps2_r32(ZT_BURNER), ps2_r32(ZT_HUD_TICK));
        zt_on = 0;
    }
    if (zt_on || cf->smoke > 0.0f) {
        int a = ps2_hook_before(ZT_BURNER, zt_burner, NULL, 50, "zero-trails");
        int b = ps2_hook_before(ZT_HUD_TICK, zt_tick, NULL, 50, "zero-trails");
        if (a < 0 || b < 0) zt_on = 0;
        atexit(zt_report);
    }
    zt_on_boot = zt_on;
    ps2_log("zero-trails: contrails %.2f, wing vapour %.2f, heat haze %.2f, smoke light %.2f%s (ACZ_VR_CONTRAILS / _VAPOUR / "
            "ACZ_VR_HEAT_HAZE / ACZ_VR_SMOKE_LIGHT; air: freezing level + 6.5 K/km%s)", cf->contrails, cf->vapour, cf->haze,
            cf->smoke, zt_on ? "" : " (sampler off)", zt_dT != 0.0f ? ", temperature offset (ACZ_VR_CONTRAILS_DT)" : "");
    if (cf->wake > 0.0f)
        ps2_log("zero-trails: cloud wake %.2f (ACZ_VR_CLOUD_WAKE, look %d: veil %.1f spans + %.1f m/s, peak %.2f, threads to "
                "%.0f m at %.2f): dragged %.0f m, inside at density %.3f..%.3f, life x%.2f, "
                "threads %d, others %d within %.0f m%s", cf->wake, zt_wk_look, (double)zt_wk_w, (double)zt_wk_spread,
                (double)zt_wk_peak, (double)zt_wk_reach, (double)zt_wk_thr_a, zt_wk_drag_m, zt_wk_d[0], zt_wk_d[1], zt_wk_life_k,
                zt_wk_threads, zt_wk_others, zt_wk_range_m, zt_wk_test[1] > zt_wk_test[0] ? " (TEST: forced inside)" : "");
}
