// zero_cracks.c - the player's hits for the canopy cracks (ps2_cracks.c) and the game's hit camera shake in VR (bug #91).
// Ace Combat Zero only (ac5); EE thread. Nothing in the game changes except, in VR, the camera-shake timer (below).
//
// Hits: the game's damage function 0x1FCD28 (a0 the unit hit, a3 amount, t0 kind, t1 attacker unit, t2 mode; static RE
// in CRACKS_RESEARCH_2026-10-23.md 3.1) -- a before-hook notes the player's HP (s16 U+0x398, U = the player's unit
// record, zero_vr.c imm_unit) and the source, an after-hook reads the HP left: the damage dealt (the game's armour factor
// and its no-damage cases included). The source: at a missile's proximity burst (ra 0x20EBC4) the caller's s4 is the
// missile's slot (+0x00 its position, +0x20 its velocity in units a field, +0x38 the speed); a missile that reaches the
// aircraft (within 15 m of its origin: both real hits seen) counts from where it came (against its velocity), else
// from its burst point; a gun hit from the attacker unit's position (+0x260); else none. The vector from the
// aircraft goes into cockpit axes (x right, y up, z back) with the aircraft's angles (FM+0x3C0 pitch, +0x3C4 yaw,
// +0x3C8 roll). The HP at the mission start (the most seen since the last reset) is the scale. Crashes (kind 5) and
// part chains (kind 6) give no cracks. Every player hit is logged (one line).
// ACZ_IMM_CRACK_TEST="field:kind:amount[:az[:el]] ..." (test): fake hits at those fields -- kind gun / missile / special
// / area (or 0-3), amount in hit points (share = amount / the start HP; 90 when unknown), az / el the source's
// direction from the eye in degrees (az + right, 0 ahead, 180 behind; el + up; defaults 0 / 10), or az = front, left,
// right, rear, up.
//
// Bug #91: the player update calls 0x23DFA8(player+0x16A0, N) at hits (N = 10 gun, 30 missile / special) and at a few
// other events (N = 1, 2, 5); it stores N at controller +0x156, and the camera controller update 0x23CC00 adds a
// random offset of N / (+0x14C) to the camera every frame while it counts N down. In VR the head pose is composed on
// top of the game's camera, so the shake moves the world (and the cockpit) against the head. ACZ_VR_CAMERA_SHAKE: auto
// (default) = no shake in head-tracked VR, the game's shake on the flat screen; 1 = always the game's shake; 0 = never.
// The timer is cleared before the camera update reads it (a before-hook on 0x23CC00); the rumble and everything else
// stay. ACZ_DEBUG_CAM_SHAKE_TEST="field:frames[:period_s]" (test) sets the timer as a hit would.
//
// Bug #133 (playtest 2026-10-27, M5 campaign: "is this crack left from the last game? the aircraft is not damaged"): a
// gun hit cracked the glass at 11:15:05, the player crashed at 11:17:55 (kind 5, HP 82 -> -918) and the game put the
// aircraft back at the mission's checkpoint (11:18:10: the crash call again with HP 255 -> 255, no damage) -- no stage
// load and no jump between two flight frames, so zero_vr.c's canopy reset (ps2_imm_reset: mission start, RETRY, jump)
// never fired and the crack stayed until the next stage load (11:24:26). The game has no repair: the player's HP only
// rises when the aircraft is restored (a checkpoint respawn, a continue). ACZ_IMM_RESET_RESPAWN (on; 0 = as before):
// once the player has taken damage since the last reset (a hit, a crash), the first rise of its HP to above 0 seen in
// a flight frame (the HUD manager runs: not during the crash replay, which re-enacts the last 15 s with its own HP --
// 255 in M5, 0 in a M3 test -- and its own damage calls) asks zero_vr.c for the same canopy reset as a RETRY (the
// cracks, the water, mist, film, ice, grime, flecks, the cloud feel, the trails, the governor's new flight) -- once: a
// further rise needs new damage first, normal hits never reset. While the aircraft is down (HP <= 0 after a hit) the
// damage calls (the replay's) leave the cracks, the watch and the HP scale alone. The HP scale (the HP at the mission
// start) is kept through such a restore: a respawned aircraft's HP must not change how strong later hits crack.
// ACZ_DEBUG_HP_TEST="field:hp ..." (test; changes the game): the player's HP set to hp in the first flight frame at or
// after each field -- a lower value counts as damage (arms the watch), a higher one is a restore as the game's would be.
#include "ps2_runtime.h"
#include "ps2_menv.h"
#include "ps2_hook.h"
#include "ps2_cracks.h"
#include "ps2_xr.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

u64 ps2_kernel_vblank_count(void);
u32 zero_vr_player_unit(void);                    /* zero_vr.c: the player's unit record (0: none) */

#define ZC_DAMAGE  0x001FCD28u
#define ZC_HUDMGR  0x0024D9D8u                    /* the HUD manager update, once per flight frame (a0 = player + 0x1310) */
#define ZC_CAMCTL  0x0023CC00u                    /* the camera controller update (a0 = player + 0x16A0 for the flight) */
#define ZC_MSL_RA  0x0020EBC4u                    /* return address of the missile manager's damage call */

static float zc_f(u32 w) { float f; memcpy(&f, &w, 4); return f; }
static int zc_ptr(u32 a) { return a >= 0x100000u && a < 0x2000000u; }

static struct {
    int on, dbg;
    u32 player;                                   /* the player struct of the last flight frame */
    int hp_max, hp_last; u32 reset_seen;
    /* bug #133: the player's HP watched for a restore after damage (ACZ_IMM_RESET_RESPAWN) */
    int rs_on, hp_seen, hp_seen_ok, armed, keep_max, own_reset, dead; u32 restores;
    struct { u64 field; int hp, done; } ht[16]; int nht;   /* ACZ_DEBUG_HP_TEST */
    /* the hit in flight (between the before- and after-hook) */
    int pend; u32 unit; int hp0, kind, amount, mode; u32 ra, att, slot; float src[3]; int have_src;
    float slotv[20];                              /* the missile slot's first 0x50 bytes as floats (log) */
    /* test hits */
    struct { u64 field; int kind; float amount, az, el; int done; } t[32]; int nt;
    /* camera shake */
    int shake_mode;                               /* -1 auto, 0 never, 1 always */
    u64 st_field; int st_frames; float st_per; u64 st_next; u32 st_log;
    u32 shakes_cleared;
} zc;

/* world vector w (units) -> cockpit axes (x right, y up, z back), unit; 0 if degenerate */
static int zc_cockpit_dir(u32 player, const float *w, float *c) {
    u32 fm = player + 0xEF0u;
    float pitch = zc_f(ps2_r32(fm + 0x3C0u)), yaw = zc_f(ps2_r32(fm + 0x3C4u)), roll = zc_f(ps2_r32(fm + 0x3C8u));
    float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch), cr = cosf(roll), sr = sinf(roll);
    float art[3] = { cy * cr + sy * sp * sr, cp * sr, -sy * cr + cy * sp * sr };
    float aup[3] = { -sr * cy + cr * sp * sy, cp * cr, sr * sy + cr * sp * cy };
    float afw[3] = { -sy * cp, sp, -cy * cp };
    c[0] = w[0] * art[0] + w[1] * art[1] + w[2] * art[2];
    c[1] = w[0] * aup[0] + w[1] * aup[1] + w[2] * aup[2];
    c[2] = -(w[0] * afw[0] + w[1] * afw[1] + w[2] * afw[2]);
    float l = sqrtf(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
    if (!(l > 1e-3f)) { c[0] = c[1] = c[2] = 0.0f; return 0; }
    c[0] /= l; c[1] /= l; c[2] /= l;
    return 1;
}
void zero_vr_imm_reset_request(int why);           /* zero_vr.c: the canopy reset at the next flight frame */
/* ---- Soot and ash on the canopy from smoke (bug #139, the user's wish 2026-10-27: "maybe smoke / ash traces on the
   glass"; ACZ_IMM_SOOT). A kill's smoke where the aircraft flies through it: the debris model's kills (zero_vr.c
   imm_debris: the kill point, where the explosion's effects stay, and the wreck the game flies on for ~8 s with its
   smoke trail) give (1) the explosion's dark smoke ball about the kill point, 5 m + 4 m/s across, thinning as
   (5 / r)^2 and gone 8-12 s after the kill, and (2) the burning wreck's trail: its positions every 0.1 s, each a puff
   of 2 m + 2.5 m/s, 0.6 (2 / r)^2, gone 6-10 s after it was laid. The concentration at the aircraft (the strongest
   puff, 0..1) x the airspeed (impaction grows with it: / 300 kt) x 4 per s x the strength is the soot deposited
   (ps2_imm_soot[0], cumulative); ps2_video.c turns it into a film on the canopy (strongest on the faces the airflow
   meets) that the airflow only slowly streaks off -- dry soot sticks to acrylic -- and water (cloud, rain) washes off
   within seconds. A pass through the middle of a fresh kill (the test kill 150 m ahead, 2 m to the side, its wreck at
   60 % of the speed: the ball, then the wreck's trail) at 324 kt leaves ~0.23: a faint brownish-grey film. Nothing in the game changes; the positions are the debris model's.
   Bug #189 (playtest 2026-10-28: flying through two fresh kills' dark smoke left nothing to see): at 4 per s a pass
   through the middle of the ball 1.3 s after the kill at 400 kt deposits ~0.15, a film of ~1-3 % alpha.
   ACZ_IMM_SMOKE_PASS (zero_vr.c, on; 0 = as before) deposits the ball's soot at 12 per s and the trail's at 6 (the
   ball is what the game draws as the dark smoke; the trail's puffs are a guess about the wreck): ~0.45 for such a
   pass, a ~5-10 % streaky film on the windscreen. Each kill's deposit is logged once it is gone (ACZ_IMM_DEBRIS_LOG). */
int zero_vr_smoke_pass(void);                     /* zero_vr.c: ACZ_IMM_SMOKE_PASS */
int zero_vr_debris_kills(float *p0, float *w, float *age, int max);      /* zero_vr.c */
/* bug #213 (zero_vr.c, ACZ_IMM_SMOKE_SIZE): the explosion's smoke as the game draws it -- its middle rises over the kill
   point, its visible radius 24 -> 46 m over 0.25 -> 2.5 s, a flat-topped profile -- holding the old ball's soot column
   through the middle (7 e^-(t - 1) / 3: 0.36 for a pass through the middle at 1 s, 324 kt, as before); off: as above */
int zero_vr_smoke_size(void);
void zero_vr_smoke_ball(int kind, float t, float *rise, float *R, float *dens, float *fade);
float zero_vr_smoke_profile(float d, float R);
int zero_vr_debris_kind(const float *p0);
extern volatile float ps2_imm_soot[4];            /* ps2_video.c: [0] deposited (cumulative), [1] concentration now, [2] field */
#define ZS_K 8
#define ZS_P 48
#define ZS_UPM 5.12f                              /* game units per metre (zero_vr.c ZD_UPM) */
static struct { int on; float p0[3]; u64 f0, last; int np; float pt[ZS_P][3]; u64 pf[ZS_P]; float dep[2]; } zs[ZS_K];
static float zs_k = -1.0f, zs_rate[2] = { 4.0f, 4.0f };  /* deposit per s at concentration 1, 300 kt: ball, trail */
static int zs_dbg, zs_log;
static void zs_summary(int k) {                   /* (ACZ_IMM_DEBRIS_LOG) a kill's soot, once it is gone */
    if (!zs_log || !zs[k].on || zs[k].dep[0] + zs[k].dep[1] < 0.001f) return;
    ps2_log("soot: the kill's smoke (field %llu) deposited %.3f (its ball %.3f, the wreck's trail %.3f; rates %.0f / %.0f "
            "per s); total %.3f", (unsigned long long)zs[k].f0, zs[k].dep[0] + zs[k].dep[1], zs[k].dep[0], zs[k].dep[1],
            zs_rate[0], zs_rate[1], ps2_imm_soot[0]);
}
static void zs_frame(u32 player, u64 f) {
    if (zs_k < 0.0f) {
        const char *e = getenv("ACZ_IMM_SOOT"); zs_k = e && *e ? (float)atof(e) : 1.0f;
        if (!(zs_k >= 0.0f && zs_k <= 4.0f)) zs_k = zs_k > 4.0f ? 4.0f : 0.0f;
        e = getenv("ACZ_DEBUG_IMM_SOOT"); zs_dbg = e ? atoi(e) : 0;
        e = getenv("ACZ_IMM_DEBRIS_LOG"); zs_log = !(e && *e == '0');
        if (zero_vr_smoke_pass()) { zs_rate[0] = 12.0f; zs_rate[1] = 6.0f; }
    }
    if (zs_k <= 0.0f || !zc_ptr(player)) return;
    float p0[3 * ZS_K], w[3 * ZS_K], age[ZS_K];
    int n = zero_vr_debris_kills(p0, w, age, ZS_K);
    for (int i = 0; i < n; i++) {
        int k = -1;
        for (int j = 0; j < ZS_K && k < 0; j++) if (zs[j].on && !memcmp(zs[j].p0, p0 + 3 * i, 12)) k = j;
        if (k < 0) {
            for (int j = 0; j < ZS_K && k < 0; j++) if (!zs[j].on) k = j;
            if (k < 0) { k = 0; for (int j = 1; j < ZS_K; j++) if (zs[j].f0 < zs[k].f0) k = j; }
            zs_summary(k);
            memset(&zs[k], 0, sizeof zs[k]);
            zs[k].on = 1; memcpy(zs[k].p0, p0 + 3 * i, 12);
            zs[k].f0 = f - (u64)(age[i] * 60.0f + 0.5f);
        }
        if (f - zs[k].last >= 6u && zs[k].np < ZS_P && age[i] < 8.5f) {     /* the trail: a puff every 0.1 s */
            float *q = zs[k].pt[zs[k].np], dd = 0.0f;
            if (zs[k].np) for (int j = 0; j < 3; j++) { float a = w[3 * i + j] - zs[k].pt[zs[k].np - 1][j]; dd += a * a; }
            if (!(dd < (250.0f * ZS_UPM) * (250.0f * ZS_UPM))) continue;     /* (the wreck jumped: not followed) */
            memcpy(q, w + 3 * i, 12); zs[k].pf[zs[k].np] = f; zs[k].np++; zs[k].last = f;
        }
    }
    float E[3];
    for (u32 j = 0; j < 3; j++) E[j] = zc_f(ps2_r32(player + 0xEF0u + 0x3B0u + 4u * j));
    float c = 0.0f; int src = 0, ks = -1;
    for (int k = 0; k < ZS_K; k++) {
        if (!zs[k].on) continue;
        float t = (float)(f - zs[k].f0) / 60.0f;
        if (t > 14.0f) { zs_summary(k); zs[k].on = 0; continue; }
        if (zero_vr_smoke_size()) {     /* bug #213: the game's own smoke (zero_vr.c zero_vr_smoke_ball), the old ball's column */
            float rise, R, dens, fd, d2 = 0.0f;
            zero_vr_smoke_ball(zero_vr_debris_kind(zs[k].p0), t, &rise, &R, &dens, &fd);
            for (int j = 0; j < 3; j++) { float a = (E[j] - zs[k].p0[j]) / ZS_UPM - (j == 1 ? rise : 0.0f); d2 += a * a; }
            if (fd > 0.0f && d2 < 6.25f * R * R) {
                float v = fd * dens * 7.0f * expf(-(t - 1.0f) / 3.0f) / (2.094f * R) * zero_vr_smoke_profile(sqrtf(d2), R);
                if (v > c) { c = v; src = 1; ks = k; }
            }
        } else {
            float r = 5.0f + 4.0f * t, fd = t < 8.0f ? 1.0f : 1.0f - (t - 8.0f) * 0.25f, d2 = 0.0f;
            for (int j = 0; j < 3; j++) { float a = (E[j] - zs[k].p0[j]) / ZS_UPM; d2 += a * a; }
            if (fd > 0.0f && d2 < 9.0f * r * r) {
                float v = fd * (5.0f / r) * (5.0f / r) * expf(-d2 / (2.0f * r * r));
                if (v > c) { c = v; src = 1; ks = k; }
            }
        }
        for (int p = 0; p < zs[k].np; p++) {
            float a = (float)(f - zs[k].pf[p]) / 60.0f, rr = 2.0f + 2.5f * a, fp = a < 6.0f ? 1.0f : 1.0f - (a - 6.0f) * 0.25f;
            if (fp <= 0.0f) continue;
            float e2 = 0.0f;
            for (int j = 0; j < 3; j++) { float q = (E[j] - zs[k].pt[p][j]) / ZS_UPM; e2 += q * q; }
            if (e2 > 9.0f * rr * rr) continue;
            float v = 0.6f * fp * (2.0f / rr) * (2.0f / rr) * expf(-e2 / (2.0f * rr * rr));
            if (v > c) { c = v; src = 2; ks = k; }
        }
    }
    extern volatile float ps2_imm_in[8];
    float kt = ps2_imm_in[2];
    if (!(kt >= 0.0f && kt < 2000.0f)) kt = 0.0f;
    float dep = zs_k * c * (kt / 300.0f) * (src == 2 ? zs_rate[1] : zs_rate[0]) / 60.0f;
    ps2_imm_soot[0] += dep; ps2_imm_soot[1] = c; ps2_imm_soot[2] = (float)f;
    if (ks >= 0) zs[ks].dep[src == 2] += dep;
    if (zs_dbg && c > 0.01f && f % (u64)(zs_dbg > 0 ? zs_dbg : 1) == 0u)
        ps2_log("soot: field %llu in the %s's smoke: concentration %.3f at %.0f kt, deposited %.3f (total %.3f)",
                (unsigned long long)f, src == 1 ? "explosion" : "wreck's trail", c, kt, dep, ps2_imm_soot[0]);
}
/* bug #133: the player's HP in a flight frame; a rise to above 0 after damage = the aircraft was restored (a checkpoint
   respawn, a continue): one canopy reset */
static void zc_hp_watch(int hp, u64 f) {
    if (hp < -30000 || hp > 30000) return;
    if (zc.armed && zc.hp_seen_ok && hp > zc.hp_seen && hp > 0) {
        zc.armed = 0; zc.dead = 0;
        if (zc.rs_on) {
            zc.keep_max = 1; zc.own_reset = 1; zc.restores++;
            ps2_log("cracks: the player's aircraft restored (HP %d -> %d in flight, field %llu): canopy reset "
                    "(ACZ_IMM_RESET_RESPAWN); HP scale kept at %d", zc.hp_seen, hp, (unsigned long long)f, zc.hp_max);
            zero_vr_imm_reset_request(2);
        }
    }
    zc.hp_seen = hp; zc.hp_seen_ok = 1;
}
static int dmg_enter(ps2_ctx *ctx, void *user) {
    (void)user;
    u32 u = ctx->r[4].uw[0], me = zero_vr_player_unit();
    if (!me || u != me || !zc_ptr(u)) return 0;
    zc.pend = 1; zc.unit = u;
    zc.hp0 = (int)(s16)ps2_r16(u + 0x398u);
    zc.amount = (int)(ctx->r[7].uw[0] & 0xFFFFu); zc.kind = (int)(ctx->r[8].uw[0] & 0xFFu);
    zc.att = ctx->r[9].uw[0]; zc.mode = (int)(ctx->r[10].uw[0] & 0xFFu); zc.ra = ctx->r[31].uw[0];
    zc.slot = zc.ra == ZC_MSL_RA ? ctx->r[20].uw[0] : 0u;
    zc.have_src = 0;
    if (zc_ptr(zc.slot)) {
        for (u32 i = 0; i < 3; i++) zc.src[i] = zc_f(ps2_r32(zc.slot + 4u * i));
        for (u32 i = 0; i < 20; i++) zc.slotv[i] = zc_f(ps2_r32(zc.slot + 4u * i));
        zc.have_src = 1;
    } else if (zc_ptr(zc.att) && zc.att != u) {
        for (u32 i = 0; i < 3; i++) zc.src[i] = zc_f(ps2_r32(zc.att + 0x260u + 4u * i));
        zc.have_src = 2;
    }
    for (int i = 0; i < 3 && zc.have_src; i++) if (!(fabsf(zc.src[i]) < 1e8f)) zc.have_src = 0;
    /* a missile that bursts at the aircraft (within 15 m: both real hits in M10 burst ~1 m from the aircraft's origin --
       the direction from there is noise): the fragments come from where it came from -- against its velocity (slot
       +0x20, units a field; +0x38 its length: 52.8 = 620 m/s in the real hit), else its shooter's side */
    if (zc.have_src == 1 && zc_ptr(zc.player)) {
        float d2 = 0.0f;
        for (u32 i = 0; i < 3; i++) { float q = zc.src[i] - zc_f(ps2_r32(zc.player + 0xEF0u + 0x3B0u + 4u * i)); d2 += q * q; }
        if (d2 < (15.0f * 5.12f) * (15.0f * 5.12f)) {
            float v[3], v2 = 0.0f;
            for (u32 i = 0; i < 3; i++) { v[i] = zc_f(ps2_r32(zc.slot + 0x20u + 4u * i)); v2 += v[i] * v[i]; }
            if (v2 > 25.0f && v2 < 1e6f) {             /* (5..1000 units a field) */
                float k = 300.0f / sqrtf(v2);          /* a point ~60 m back along its path */
                for (u32 i = 0; i < 3; i++) zc.src[i] -= v[i] * k;
                zc.have_src = 4;
            } else if (zc_ptr(zc.att) && zc.att != u) {
                for (u32 i = 0; i < 3; i++) zc.src[i] = zc_f(ps2_r32(zc.att + 0x260u + 4u * i));
                zc.have_src = 3;
            }
            for (int i = 0; i < 3 && zc.have_src; i++) if (!(fabsf(zc.src[i]) < 1e8f)) zc.have_src = 0;
        }
    }
    return 0;
}
static int dmg_leave(ps2_ctx *ctx, void *user) {
    (void)ctx; (void)user;
    int ok = 0;
    u32 u = ps2_hook_entry_arg(0, &ok);
    if (!ok || !zc.pend || u != zc.unit) return 0;
    zc.pend = 0;
    u64 f = ps2_kernel_vblank_count();
    int hp1 = (int)(s16)ps2_r16(u + 0x398u), dmg = zc.hp0 - hp1;
    int down = zc.dead && zc.rs_on;                 /* (bug #133: the aircraft was already down -- the crash replay) */
    if (zc.hp0 > zc.hp_max && !zc.keep_max && !down) zc.hp_max = zc.hp0;
    if (dmg > 0 && !down) {                          /* (bug #133: damage arms the watch; HP <= 0: the aircraft is down) */
        zc.armed = 1; zc.hp_seen = hp1; zc.hp_seen_ok = 1;
        if (hp1 <= 0) zc.dead = 1;
    }
    int hpm = zc.hp_max > 0 ? zc.hp_max : 90;
    zc.hp_last = hp1;
    float dir[3] = { 0.0f, 0.0f, 0.0f }, dist = 0.0f;
    u32 p = zc.player;
    if (zc.have_src && zc_ptr(p)) {
        float w[3];
        for (u32 i = 0; i < 3; i++) w[i] = zc.src[i] - zc_f(ps2_r32(p + 0xEF0u + 0x3B0u + 4u * i));
        dist = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]) / 5.12f;
        zc_cockpit_dir(p, w, dir);
    }
    u32 src = 0xFFFFu;
    if (zc_ptr(zc.att)) src = (u32)ps2_r8(zc.att + 0x23Cu) << 8 | ps2_r8(zc.att + 0x23Du);
    else if (ps2_r8(u + 0x242u) != 0xFFu) src = (u32)ps2_r8(u + 0x242u) << 8 | ps2_r8(u + 0x243u);
    static const char *const kn[8] = { "gun", "missile", "special", "3", "4", "crash", "part chain", "7" };
    ps2_log("cracks: PLAYER hit field %llu kind %d (%s) amount %d mode %d -> damage %d (hp %d -> %d of %d) attacker %08X "
            "src %s dir %+.2f %+.2f %+.2f %.0f m ra %08X", (unsigned long long)f, zc.kind, kn[zc.kind & 7], zc.amount,
            zc.mode, dmg, zc.hp0, hp1, hpm, zc.att, zc.have_src == 1 ? "missile" : zc.have_src == 2 ? "attacker" : zc.have_src == 3 ? "missile at the aircraft: its shooter" : zc.have_src == 4 ? "missile at the aircraft: where it came from" : "none",
            dir[0], dir[1], dir[2], dist, zc.ra);
    if (zc.have_src == 4 && zc_ptr(zc.att) && zc_ptr(p)) {          /* (for comparison: the shooter's side) */
        float w[3], sd[3];
        for (u32 i = 0; i < 3; i++) w[i] = zc_f(ps2_r32(zc.att + 0x260u + 4u * i)) - zc_f(ps2_r32(p + 0xEF0u + 0x3B0u + 4u * i));
        if (zc_cockpit_dir(p, w, sd)) ps2_log("cracks:   (its shooter: dir %+.2f %+.2f %+.2f)", sd[0], sd[1], sd[2]);
    }
    if (zc.have_src == 1 || zc.have_src >= 3) {
        char ln[600]; int n = 0;
        for (int i = 0; i < 20; i++) n += snprintf(ln + n, sizeof ln - (size_t)n, " %X:%.4g", i * 4, zc.slotv[i]);
        ps2_log("cracks:   missile slot %08X:%s", zc.slot, ln);
    }
    if (!zc.on || dmg <= 0 || zc.kind == 5 || zc.kind == 6 || down) return 0;
    crk_hit h; memset(&h, 0, sizeof h);
    h.field = (u32)f; h.kind = zc.kind <= 2 ? zc.kind : CRK_AREA;
    h.share = (float)dmg / (float)hpm; h.hp_left = (float)(hp1 > 0 ? hp1 : 0) / (float)hpm;
    memcpy(h.dir, dir, sizeof dir); h.dist_m = dist; h.src = src;
    ps2_crk_hit_push(&h);
    return 0;
}
static void zc_test_parse(void) {
    const char *e = getenv("ACZ_IMM_CRACK_TEST");
    if (!e || !*e) return;
    char buf[1024]; snprintf(buf, sizeof buf, "%s", e);
    for (char *tok = strtok(buf, " ;"); tok && zc.nt < 32; tok = strtok(NULL, " ;")) {
        char *f[5] = { 0 }; int n = 0;
        for (char *p = tok; n < 5;) { f[n++] = p; char *c = strchr(p, ':'); if (!c) break; *c = 0; p = c + 1; }
        if (n < 3) continue;
        int kind = !strcmp(f[1], "gun") ? 0 : !strcmp(f[1], "missile") ? 1 : !strcmp(f[1], "special") ? 2
                 : !strcmp(f[1], "area") ? 3 : atoi(f[1]);
        float az = 0.0f, el = 10.0f;
        if (n > 3) {
            if (!strcmp(f[3], "front")) az = 0.0f;
            else if (!strcmp(f[3], "left")) az = -70.0f;
            else if (!strcmp(f[3], "right")) az = 70.0f;
            else if (!strcmp(f[3], "rear")) az = 150.0f;
            else if (!strcmp(f[3], "up")) { az = 0.0f; el = 70.0f; }
            else az = (float)atof(f[3]);
        }
        if (n > 4) el = (float)atof(f[4]);
        zc.t[zc.nt].field = strtoull(f[0], NULL, 0); zc.t[zc.nt].kind = kind & 3;
        zc.t[zc.nt].amount = (float)atof(f[2]); zc.t[zc.nt].az = az; zc.t[zc.nt].el = el;
        zc.nt++;
    }
    ps2_log("cracks: %d test hit%s (ACZ_IMM_CRACK_TEST)", zc.nt, zc.nt == 1 ? "" : "s");
}
static int hud_enter(ps2_ctx *ctx, void *user) {
    (void)user;
    static u64 last;
    u64 f = ps2_kernel_vblank_count();
    if (f == last) return 0;
    last = f;
    u32 player = ctx->r[4].uw[0] - 0x1310u;
    if (!zc_ptr(player)) return 0;
    zc.player = player;
    extern volatile u32 ps2_imm_reset;
    u32 r = ps2_imm_reset;
    if (r != zc.reset_seen) {
        zc.reset_seen = r;
        if (zc.own_reset) zc.own_reset = 0;              /* (our restore's reset: the same aircraft, its HP scale kept) */
        else { zc.hp_max = 0; zc.keep_max = 0; zc.armed = 0; zc.hp_seen_ok = 0; zc.dead = 0; }
    }
    u32 u = zero_vr_player_unit();
    if (zc_ptr(u)) {
        for (int i = 0; i < zc.nht; i++) {               /* ACZ_DEBUG_HP_TEST: the game's HP set as a hit / a restore would */
            if (zc.ht[i].done || f < zc.ht[i].field) continue;
            zc.ht[i].done = 1;
            int old = (int)(s16)ps2_r16(u + 0x398u), hv = zc.ht[i].hp;
            ps2_w16(u + 0x398u, (u16)(s16)hv);
            ps2_log("cracks: HP test field %llu: the player's HP %d -> %d (ACZ_DEBUG_HP_TEST)%s", (unsigned long long)f, old, hv,
                    hv < old ? ": counts as damage" : "");
            if (hv < old) { zc.armed = 1; zc.hp_seen = hv; zc.hp_seen_ok = 1; if (hv <= 0) zc.dead = 1; }
        }
        int hp = (int)(s16)ps2_r16(u + 0x398u);
        zc_hp_watch(hp, f);
        zs_frame(player, f);                         /* (bug #139: soot from the kills' smoke) */
        if (hp > zc.hp_max && hp < 2000 && !zc.keep_max) {
            if (zc.dbg && zc.hp_max) ps2_log("cracks: the player's HP rose %d -> %d (field %llu)", zc.hp_max, hp, (unsigned long long)f);
            zc.hp_max = hp;
        }
    }
    for (int i = 0; i < zc.nt; i++) {
        if (zc.t[i].done || f < zc.t[i].field) continue;
        zc.t[i].done = 1;
        int hpm = zc.hp_max > 0 ? zc.hp_max : 90;
        float az = zc.t[i].az * 0.01745329f, el = zc.t[i].el * 0.01745329f;
        crk_hit h; memset(&h, 0, sizeof h);
        h.field = (u32)f; h.kind = zc.t[i].kind; h.share = zc.t[i].amount / (float)hpm; h.hp_left = 0.5f;
        h.dir[0] = sinf(az) * cosf(el); h.dir[1] = sinf(el); h.dir[2] = -cosf(az) * cosf(el);
        h.dist_m = 30.0f; h.src = 0x7E00u | (u32)h.kind; h.test = 1;    /* (one attacker a kind: gun hits close together
                                                                              make a burst) */
        ps2_log("cracks: test hit field %llu kind %d amount %.0f of %d hp az %.0f el %.0f", (unsigned long long)f, h.kind,
                zc.t[i].amount, hpm, zc.t[i].az, zc.t[i].el);
        ps2_crk_hit_push(&h);
    }
    /* ACZ_DEBUG_CAM_SHAKE_TEST: the game's shake timer set as a hit would (player+0x16A0 +0x156) */
    if (zc.st_frames > 0 && f >= zc.st_next && zc.st_next) {
        ps2_w8(player + 0x16A0u + 0x156u, (u8)zc.st_frames);
        ps2_log("cracks: shake test field %llu: camera shake timer set to %d", (unsigned long long)f, zc.st_frames);
        zc.st_next = zc.st_per > 0.0f ? f + (u64)(zc.st_per * 60.0f) : 0u;
        zc.st_log = 40u;
    }
    if (zc.st_log) {                             /* the flight camera's angles while the shake runs (game values) */
        zc.st_log--;
        u32 c = player + 0xB70u, k = player + 0x16A0u;
        ps2_log("cracks: shake test field %llu timer %u camera pos %.3f %.3f %.3f ang %.5f %.5f %.5f | controller +14C %u +14E %u "
                "+14F %u view %u +D0 %.4f %.4f %.4f +E0 %.4f %.4f %.4f %.4f +F0 %.4f %.4f %.4f", (unsigned long long)f,
                ps2_r8(k + 0x156u), zc_f(ps2_r32(c + 0x30u)), zc_f(ps2_r32(c + 0x34u)), zc_f(ps2_r32(c + 0x38u)),
                zc_f(ps2_r32(c + 0x40u)), zc_f(ps2_r32(c + 0x44u)), zc_f(ps2_r32(c + 0x48u)), ps2_r8(k + 0x14Cu), ps2_r8(k + 0x14Eu),
                ps2_r8(k + 0x14Fu), ps2_r8(player + 0x1AA4u), zc_f(ps2_r32(k + 0xD0u)), zc_f(ps2_r32(k + 0xD4u)),
                zc_f(ps2_r32(k + 0xD8u)), zc_f(ps2_r32(k + 0xE0u)), zc_f(ps2_r32(k + 0xE4u)), zc_f(ps2_r32(k + 0xE8u)),
                zc_f(ps2_r32(k + 0xECu)), zc_f(ps2_r32(k + 0xF0u)), zc_f(ps2_r32(k + 0xF4u)), zc_f(ps2_r32(k + 0xF8u)));
    }
    return 0;
}
static int cam_enter(ps2_ctx *ctx, void *user) {
    (void)user;
    u32 c = ctx->r[4].uw[0];
    if (!zc_ptr(c)) return 0;
    u8 t = ps2_r8(c + 0x156u);
    if (!t) return 0;
    int off = zc.shake_mode == 0 || (zc.shake_mode < 0 && ps2_xr_mode() >= PS2_XR_MONO);
    if (!off) return 0;
    ps2_w8(c + 0x156u, 0);
    zc.shakes_cleared++;
    if (zc.dbg || zc.st_log) ps2_log("cracks: camera shake of %u frames left out (VR; ACZ_VR_CAMERA_SHAKE)", (unsigned)t);
    return 0;
}
/* the NEXT MISSION tier (ps2_menv.h): the game thread, at the edge */
void zero_cracks_menv_reload(void) {
    ps2_crk_menv_reload();
    zc.on = ps2_crk_on();
    const char *e = ps2_menv("ACZ_VR_CAMERA_SHAKE");
    const int a = zc.shake_mode;
    zc.shake_mode = !e || !*e || !strcmp(e, "auto") ? -1 : (*e == '0' || !strcmp(e, "off")) ? 0 : 1;
    ps2_menv_note("ACZ_VR_CAMERA_SHAKE", a, zc.shake_mode);
}
void zero_cracks_install(void) {
    zc.on = ps2_crk_on();
    {   const char *rv = getenv("ACZ_IMM_RESET_RESPAWN"), *ra = getenv("ACZ_IMM_RESET");
        zc.rs_on = !(rv && *rv == '0') && !(ra && *ra == '0'); }
    const char *e = getenv("ACZ_DEBUG_CANOPY_CRACKS"); zc.dbg = e ? atoi(e) : 0;
    e = ps2_menv("ACZ_VR_CAMERA_SHAKE");
    zc.shake_mode = !e || !*e || !strcmp(e, "auto") ? -1 : (*e == '0' || !strcmp(e, "off")) ? 0 : 1;
    e = getenv("ACZ_DEBUG_CAM_SHAKE_TEST");
    if (e && *e) {
        char *p;
        zc.st_field = strtoull(e, &p, 0);
        zc.st_frames = *p == ':' ? atoi(p + 1) : 30;
        p = *p == ':' ? strchr(p + 1, ':') : NULL;
        zc.st_per = p ? (float)atof(p + 1) : 0.0f;
        zc.st_next = zc.st_field ? zc.st_field : 1u;
    }
    zc_test_parse();
    e = getenv("ACZ_DEBUG_HP_TEST");
    if (e && *e) {
        char buf[512]; snprintf(buf, sizeof buf, "%s", e);
        for (char *tok = strtok(buf, " ;,"); tok && zc.nht < 16; tok = strtok(NULL, " ;,")) {
            char *c = strchr(tok, ':');
            if (!c) continue;
            zc.ht[zc.nht].field = strtoull(tok, NULL, 0); zc.ht[zc.nht].hp = atoi(c + 1); zc.nht++;
        }
        ps2_log("cracks: %d HP test change%s (ACZ_DEBUG_HP_TEST)", zc.nht, zc.nht == 1 ? "" : "s");
    }
    int a = 0, b = 0, c = 0;
    /* the hits are logged always (one line each); the cracks only when on */
    if (ps2_r32(ZC_DAMAGE) >> 16 == 0x27BDu) {
        a = ps2_hook_before(ZC_DAMAGE, dmg_enter, NULL, 62, "zero-cracks") >= 0
            && ps2_hook_after(ZC_DAMAGE, dmg_leave, NULL, 62, "zero-cracks") >= 0;
    }
    if (ps2_r32(ZC_HUDMGR) >> 16 == 0x27BDu) b = ps2_hook_before(ZC_HUDMGR, hud_enter, NULL, 52, "zero-cracks") >= 0;
    if (zc.shake_mode != 1 && ps2_r32(ZC_CAMCTL) >> 16 == 0x27BDu)
        c = ps2_hook_before(ZC_CAMCTL, cam_enter, NULL, 50, "zero-cracks") >= 0;
    ps2_log("zero-vr: canopy cracks %s: player hits %d, flight frames %d; camera shake %s (%d); canopy reset on the "
            "aircraft's restore %s (ACZ_IMM_RESET_RESPAWN)", zc.on ? "on" : "off", a, b,
            zc.shake_mode < 0 ? "off in VR (auto)" : zc.shake_mode ? "the game's" : "off", c, zc.rs_on ? "on" : "off");
}
void zero_cracks_report(void) {
    if (zc.shakes_cleared) ps2_log("cracks: %u camera shakes left out in VR", zc.shakes_cleared);
    if (zc.restores) ps2_log("cracks: %u canopy reset%s on the aircraft's restore (ACZ_IMM_RESET_RESPAWN)", zc.restores,
                             zc.restores == 1u ? "" : "s");
}
