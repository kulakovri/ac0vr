#include <pthread.h>
// zero_vr.c - Ace Combat Zero specific helpers for VR in the recompiled runtime.
//
// FOV widening that the game's own culling follows: the view context's projection distance d (ctx+0x08) is the
// whole field of view (tan(hfov/2) = virtW / (2d)). The context update function (0x105698 in SLUS_213.46) builds
// the screen, clip, world->screen and world->clip matrices from it, so dividing d by k for the duration of that call
// widens every derived matrix and the frustum the game culls against by k in tangent space. d is restored after the
// call, so anything else that reads ctx+8 (and the next frame's caller, which rewrites it from the camera) is
// unaffected.
//
// ACZ_FOV_SCALE=k   (k > 1 widens; 1 or unset = off)
#include "ps2_runtime.h"
#include "ps2_menv.h"
#include "ps2_hook.h"
#include "ps2_xr.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static float env_float(const char *name, float def) {
    const char *e = ps2_menv(name);
    if (!e || !*e) return def;
    float v = def; int ip = 0, fp = 0, fd = 0; const char *p = e; int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    while (*p >= '0' && *p <= '9') ip = ip * 10 + (*p++ - '0');
    if (*p == '.' || *p == ',') { p++; while (*p >= '0' && *p <= '9' && fd < 6) { fp = fp * 10 + (*p++ - '0'); fd++; } }
    v = (float)ip; { float d = 1.0f; for (int i = 0; i < fd; i++) d *= 10.0f; v += (float)fp / d; }
    return neg ? -v : v;
}

#define Z_CTX_UPDATE 0x00105698u
static const u32 z_ctx_update_w[4] = { 0x27BDFF40u, 0x24850040u, 0xFFB10098u, 0x24860030u };

static float z_k = 1.0f;
static u64 z_calls;
#define Z_DEPTH 16
static u32 z_ctx[Z_DEPTH];
static u32 z_saved[Z_DEPTH];
static int z_depth;
static u64 z_dump_field;
static u32 z_dump_ra[Z_DEPTH];
u64 ps2_kernel_vblank_count(void);

static float f_of(u32 w) { float f; memcpy(&f, &w, 4); return f; }
static u32 w_of(float f) { u32 w; memcpy(&w, &f, 4); return w; }

/* ---- head tracking (PS2_XR_MODE=mono|stereo) ----
   The game builds its view as row vectors: view = (world - eye) * R with R = Ry(yaw) * Rx(pitch) * Rz(roll) (column
   convention matrices, angles at ctx+0x40/+0x44/+0x48), camera space x right, y up, looking down -z -- the same axes
   as OpenXR view space (verified from the context matrices, see NOTES_ZERO_VR.md). R is camera->world for column
   vectors, so the head rotation H (relative to the recentred reference) composes as R' = R * H and the head offset
   moves the eye by R * p. The main camera is the 640x480 context; the game updates it several times per frame and
   every update of one frame uses the same head pose. */
static ps2_xr_render_pose z_pose;
static u64 z_pose_field = ~0ull;
static int z_xr = -1;
static float z_units_per_m = 6.0f, z_stereo_scale = 1.0f;
static u64 z_head_updates;
static float z_fov_limit;               /* ACZ_VR_FOV_LIMIT: cap on the half-FOV tangent (0 = none) */
static int z_debug_eyes;              /* ACZ_VR_DEBUG_EYES=1: left eye rolled +0.5 rad, right -0.5 */
static float z_test_yaw, z_test_pitch;  /* ACZ_VR_TEST_YAW / _PITCH (degrees): fixed extra head rotation */
static u32 z_saved_cam[Z_DEPTH][8];             // eye x,y,z, pitch, yaw, roll, flag (1 = restore), virtH

static void m3_euler(float pitch, float yaw, float roll, float m[3][3]) {
    float cp = cosf(pitch), sp = sinf(pitch), cy = cosf(yaw), sy = sinf(yaw), cr = cosf(roll), sr = sinf(roll);
    m[0][0] = cy * cr + sy * sp * sr; m[0][1] = -cy * sr + sy * sp * cr; m[0][2] = sy * cp;
    m[1][0] = cp * sr;                m[1][1] = cp * cr;                 m[1][2] = -sp;
    m[2][0] = -sy * cr + cy * sp * sr; m[2][1] = sy * sr + cy * sp * cr; m[2][2] = cy * cp;
}
static void m3_quat(const float q[4], float m[3][3]) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0][0] = 1 - 2 * (y * y + z * z); m[0][1] = 2 * (x * y - z * w);     m[0][2] = 2 * (x * z + y * w);
    m[1][0] = 2 * (x * y + z * w);     m[1][1] = 1 - 2 * (x * x + z * z); m[1][2] = 2 * (y * z - x * w);
    m[2][0] = 2 * (x * z - y * w);     m[2][1] = 2 * (y * z + x * w);     m[2][2] = 1 - 2 * (x * x + y * y);
}

/* ACZ_VR_HEADLOOK=stick: head yaw/pitch become the right stick (the game's look-around), the camera is not rotated
   and the XR image is head-locked (ps2_xr.c). ACZ_VR_LOOK_YAW_DEG / _PITCH_DEG: head angle for full deflection
   (default 90 / 60); ACZ_VR_LOOK_DZ: the game's stick dead zone to skip (default 64 of 128); ACZ_VR_LOOK_INVERT_Y=1
   flips up/down. */
static int z_stick;
/* Inside the R3 cockpit the stick head look feels like the cockpit turning around you: there the head moves the camera
   directly and the image is world-locked, as in the plain stereo mode. ACZ_VR_COCKPIT_FREELOOK=0 keeps stick look. */
static int z_in_cockpit(void) {
    static int on = -1;
    extern volatile int ps2_vk_cockpit_view;
    if (on < 0) { const char *e = getenv("ACZ_VR_COCKPIT_FREELOOK"); on = !(e && *e == '0'); }
    return on && ps2_vk_cockpit_view;
}
/* The renderer detects the cockpit view from the game's cockpit draws in a field (> 30). The game culls those to the
   head-turned view, and the J35J's cockpit has fewer than 30 when looking to the side or back: the view fell back to
   "outside" (stick head look: head off the camera, image head-locked), the next frame looked ahead, found the cockpit
   again, and so on every frame or two -- violent shaking on head turns (bug #19). The game's own view selection (the
   pad player's view byte, player +0x1AA4: 2 = cockpit, read by zv_player_update) holds the detected cockpit view
   (ps2_vk_cockpit_hold) while it says cockpit, no event holds the camera (flag 0x2000 of +0x1A68) and the player
   update ran within the last 8 fields -- or a menu is over the flight (static screen: pause menu, RETRY's reload,
   the crash's death camera and replay). Menu frames are not head-tracked, so the flag has no effect there; keeping
   it lets the first frame after "return to game" or a RETRY be the cockpit view again (it dropped before: one frame
   of stick look, and the game's look-around swung back for ~0.3 s). When the flight resumes the player update has
   run in that frame and the view byte decides again. Entering the cockpit view still needs the draws.
   ACZ_VR_COCKPIT_HOLD=0: off. */
static int zv_hold_on = -1;
static volatile u32 zv_view_now = 0xFFu;      /* the pad player's view byte at its last update, 0xFE = event camera */
static volatile u64 zv_view_field;            /* field of that update */
static volatile int z_engaged_now;
/* The HUD manager update (instr_hook) runs every flight frame and stops while the pause menu (or a mission start /
   end screen) is up, while the flight camera keeps being updated. Field of its last run; 0 = never this session. */
static volatile u64 z_instr_field;
/* ACZ_VR_EVENT_FLAT (pt_flat 2026-10-29; default on, 0 = before, off with ACZ_VR_TRACK_ALL): a mission event holds the
   camera when the pad player's last update (within 8 fields) had flag 0x2000 -- the game shows the event's camera, not
   the pilot's. Such frames go to the static screen. ACZ_VR_EVENT_FLAT_HOLD=N (default 6 fields): the static screen
   stays N fields after the flag clears, while the game switches back to the kept view (the event's camera can draw
   one or two more frames before the cockpit view's first frame). */
static int z_event_was;
static int z_event_flat_now(u64 f, int engaged) {
    static int on = -1, hold = 6; static unsigned mg;
    static u64 last_ev;
    if (ps2_menv_take(&mg, ps2_menv_gen) | (on < 0)) {
        const int o = on;
        const char *e = ps2_menv("ACZ_VR_EVENT_FLAT");
        on = !(e && *e == '0') && !getenv("ACZ_VR_TRACK_ALL");
        if (o >= 0) ps2_menv_note("ACZ_VR_EVENT_FLAT", o, on);
        e = getenv("ACZ_VR_EVENT_FLAT_HOLD");
        if (e && *e) { hold = atoi(e); if (hold < 0) hold = 0; if (hold > 120) hold = 120; }
    }
    if (!on || !engaged) { last_ev = 0; return 0; }
    u64 vf = zv_view_field;
    if (vf && f - vf <= 8u && zv_view_now == 0xFEu) { last_ev = f; return 1; }
    return last_ev && f - last_ev <= (u64)hold;
}
static void z_recentre_hand_over(u64 f);
/* ACZ_VR_RESUME_HOLD=N (pt_flat 2026-10-29; default 4 fields, 0 = before): head tracking resuming in the field of a
   stage load (the aircraft put elsewhere across a stage (re)load: instr_hook's canopy reset, z_stage_load_field) waits
   N more fields on the static screen. M15's emergency launch: the cut to the air base (static, the HUD manager idle)
   gave way to one tracked frame of the pilot's first-person view at field 3931 before the launch event took the
   camera at 3932 -- a one-frame flash of stereo between two flat screens. A pause menu's resume (no stage load) is
   not delayed. */
static volatile u64 z_stage_load_field;
static int z_resume_hold(u64 f, int menu, int was_menu) {
    static int n = -1;
    static u64 until;
    if (n < 0) {
        const char *e = getenv("ACZ_VR_RESUME_HOLD");
        n = e && *e ? atoi(e) : 4;
        if (n < 0) n = 0;
        if (n > 60) n = 60;
    }
    if (menu) { until = 0; return 1; }
    if (was_menu && !until && n > 0 && z_stage_load_field && f - z_stage_load_field <= 4u) {
        until = f + (u64)n;
        ps2_log("zero-vr: tracking resumes at a stage load: the static screen holds %d fields (ACZ_VR_RESUME_HOLD)", n);
    }
    if (until && f < until) return 1;
    until = 0;
    return 0;
}
static float z_look_yaw = 90.0f, z_look_pitch = 60.0f, z_look_dz = 64.0f;
static int z_look_inv;
static u8 look_axis(float frac) {
    float a = frac < 0 ? -frac : frac;
    if (a < 0.02f) return 128;                         /* small head jitter: centre */
    if (a > 1.0f) a = 1.0f;
    float v = z_look_dz + a * (127.0f - z_look_dz);    /* start at the game's dead-zone edge: linear response */
    int r = 128 + (int)(frac < 0 ? -v : v);
    return (u8)(r < 0 ? 0 : r > 255 ? 255 : r);
}
static int z_head_look(u8 *rx, u8 *ry) {
    if (!z_stick || !z_engaged_now || !z_pose.valid || z_in_cockpit()) return 0;
    float H[3][3];
    m3_quat(z_pose.q, H);
    float fx = -H[0][2], fy = -H[1][2], fz = -H[2][2];             /* head forward (OpenXR: -z forward) */
    float yaw = atan2f(fx, -fz) * 57.29578f, pitch = asinf(fy < -1 ? -1 : fy > 1 ? 1 : fy) * 57.29578f;
    *rx = look_axis(yaw / z_look_yaw);
    float py = -pitch / z_look_pitch;                              /* PS2 sticks: up = smaller values */
    *ry = look_axis(z_look_inv ? -py : py);
    return 1;
}

static void apply_head(u32 c, int slot) {
    float eye[3], ang[3];
    for (u32 i = 0; i < 3; i++) {
        z_saved_cam[slot][i] = ps2_r32(c + 0x30u + 4u * i);
        z_saved_cam[slot][3 + i] = ps2_r32(c + 0x40u + 4u * i);
        eye[i] = f_of(z_saved_cam[slot][i]);
        ang[i] = f_of(z_saved_cam[slot][3 + i]);
    }
    z_saved_cam[slot][6] = 1;
    float R[3][3], H[3][3], N[3][3];
    m3_euler(ang[0], ang[1], ang[2], R);
    m3_quat(z_pose.q, H);
    if (z_stick && !z_in_cockpit()) {   /* head look through the right stick: the camera itself is not rotated */
        memset(H, 0, sizeof H); H[0][0] = H[1][1] = H[2][2] = 1.0f;
    }
    if (z_test_yaw != 0.0f || z_test_pitch != 0.0f) {      /* H = H * Ry(test_yaw) * Rx(test_pitch) */
        float T[3][3], U[3][3];
        m3_euler(z_test_pitch * 0.01745329f, z_test_yaw * 0.01745329f, 0.0f, T);
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) U[i][j] = H[i][0] * T[0][j] + H[i][1] * T[1][j] + H[i][2] * T[2][j];
        memcpy(H, U, sizeof H);
    }
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) N[i][j] = R[i][0] * H[0][j] + R[i][1] * H[1][j] + R[i][2] * H[2][j];
    float sp = -N[1][2]; if (sp > 1) sp = 1; if (sp < -1) sp = -1;
    float pitch = asinf(sp), roll = atan2f(N[1][0], N[1][1]), yaw = atan2f(N[0][2], N[2][2]);
    if (z_debug_eyes && z_pose.eye >= 0) roll += z_pose.eye == 0 ? 0.5f : -0.5f;   /* tilt shows the eye */
    {   /* debug: ACZ_VR_DEBUG_SEQ_ROLL=<rad> tilts frames by +-rad by the parity of their XR frame number, so a
           captured image shows which frame number it was drawn under */
        static float sr = -1.0f;
        if (sr < 0.0f) { const char *e = getenv("ACZ_VR_DEBUG_SEQ_ROLL"); sr = e ? (float)atof(e) : 0.0f; }
        if (sr > 0.0f) roll += (z_pose.seq & 1u) ? sr : -sr;
    }
    float k = z_stick && !z_in_cockpit() ? 0.0f : z_units_per_m * z_stereo_scale;
    float off[3] = { z_pose.p[0] * k, z_pose.p[1] * k, z_pose.p[2] * k };
    for (int i = 0; i < 3; i++) eye[i] += R[i][0] * off[0] + R[i][1] * off[1] + R[i][2] * off[2];
    for (u32 i = 0; i < 3; i++) ps2_w32(c + 0x30u + 4u * i, w_of(eye[i]));
    ps2_w32(c + 0x40u, w_of(pitch)); ps2_w32(c + 0x44u, w_of(yaw)); ps2_w32(c + 0x48u, w_of(roll));
    /* Match the virtual screen to the eye: keep virtW, set virtH so virtH/virtW = tan_v/tan_h, and d so the width
       spans tan_h. The projection, the culling frustum and the HUD marker projection all derive from these.
       (Keeping 4:3 and covering the height made the view ~123 deg wide and overflowed the game's draw lists.) */
    float vw = f_of(ps2_r32(c));
    float d = 0.5f * vw / z_pose.tan_h;
    z_saved_cam[slot][7] = ps2_r32(c + 4u);
    if (d > 1.0f && d < 100000.0f) {
        ps2_w32(c + 8u, w_of(d));
        ps2_w32(c + 4u, w_of(vw * z_pose.tan_v / z_pose.tan_h));
    }
    z_head_updates++;
}

/* The flight-start recentre (unless ACZ_VR_NO_AUTO_RECENTER), due at the hand-over from the static screen (the intro,
   a mission event, the start's loading screens) to the head-tracked view. ACZ_VR_RECENTRE_KEEP=deg (pt_flat
   2026-10-29; default 10, 0 = always recentre as before): a head within that many degrees of the reference's yaw has
   not moved, so the reference is kept -- before, every flight start re-took it from the head (the session's log:
   2.8, 3.9 and 6.4 deg shifts at the M13, M15 and M16 starts; the M14 start's -53.7 deg came while the user looked
   aside, and he recentred by hand 6 s later). Beyond it the reference is taken from the head as before, at the cut,
   and the cinema screen follows only with the first head-tracked frame (ps2_xr.c recenter_hand_over). */
static float z_recentre_keep(void) {
    static float keep = -1.0f;
    if (keep < 0.0f) {
        const char *e = getenv("ACZ_VR_RECENTRE_KEEP");
        keep = e && *e ? (float)atof(e) : 10.0f;
        if (!(keep >= 0.0f)) keep = 0.0f;
        if (keep > 90.0f) keep = 90.0f;
    }
    return keep;
}
static void z_recentre_hand_over(u64 f) {
    float keep = z_recentre_keep();
    if (keep > 0.0f) ps2_xr_request_recenter_hand_over(keep);
    else ps2_xr_request_recenter();
    ps2_log("zero-vr: flight start: recentre due at field %llu (ACZ_VR_RECENTRE_KEEP %.0f)", (unsigned long long)f, keep);
}

static int ctx_update_enter(ps2_ctx *ctx, void *user) {
    (void)user;
    u32 c = ctx->r[4].uw[0];                      // a0 = view context
    if (z_depth < Z_DEPTH) {
        u32 w = ps2_r32(c + 8u);
        float d = f_of(w);
        z_ctx[z_depth] = c; z_saved[z_depth] = w;
        z_dump_ra[z_depth] = ctx->r[31].uw[0];
        z_saved_cam[z_depth][6] = 0;
        if (z_xr < 0) z_xr = ps2_xr_mode();
        // The world camera: a persistent 640x480 context (0x0072A8D0 in mission 1). Temporary contexts on the stack
        // (0x01FFFC10 during the hangar->mission transition, eye and angles zero) are screen-space passes and must
        // stay untouched: rotating them corrupted the transition.
        int main_cam = ps2_r32(c) == 0x44200000u && ps2_r32(c + 4u) == 0x43F00000u && c < 0x01E00000u;
        {   /* ACZ_DEBUG_SITES=f1,f2: count 640x480 camera updates per (context, caller) and print them at those fields */
            static int dbg = -1; static u64 f1, f2; static u32 key_c[256], key_ra[256], cnt[256]; static int nk;
            if (dbg < 0) { const char *e = getenv("ACZ_DEBUG_SITES"); dbg = e != NULL;
                           if (e) { f1 = strtoull(e, (char **)&e, 0); if (*e == ',') f2 = strtoull(e + 1, NULL, 0); } }
            if (dbg && main_cam) {
                u32 ra = ctx->r[31].uw[0]; int k;
                for (k = 0; k < nk && !(key_c[k] == c && key_ra[k] == ra); k++) ;
                if (k == nk && nk < 256) { key_c[nk] = c; key_ra[nk] = ra; cnt[nk] = 0; nk++; }
                if (k < 256) cnt[k]++;
                u64 f = ps2_kernel_vblank_count();
                if ((f1 && f >= f1) || (f2 && f >= f2 && !f1)) {
                    ps2_log("zero-vr: camera sites up to field %llu:", (unsigned long long)f);
                    for (int i = 0; i < nk; i++) if (cnt[i] > 10) ps2_log("   ctx %08X ra %08X  %u", key_c[i], key_ra[i], cnt[i]);
                    for (int i = 0; i < nk; i++) cnt[i] = 0;
                    if (f1) f1 = 0; else f2 = 0;
                }
            }
        }
        if (main_cam && z_xr >= PS2_XR_MONO) {
            /* Only the in-flight camera is head tracked: the call sites below update the world camera every
               flight frame (mission 1, verified by ACZ_DUMP_FIELD). Rotating the hangar camera corrupted the
               hangar->mission transition, so other sites (hangar, briefing, cutscenes) stay on the cinema screen. */
            static const u32 flight_sites[] = { 0x0015D6E8u, 0x00170D90u, 0x001772ECu, 0x001D5588u, 0x001D7230u,
                                                0x001D7238u,
                                                /* the same two flight contexts (013C5DA0, 013C5F60) updated again each
                                                   frame for the sky, horizon haze and environment passes: without the
                                                   head these followed it in world-locked (cockpit) frames
                                                   (ACZ_VR_SKY_SITES=1 adds them; no visible effect found) */
                                                0x00232860u, 0x002319A8u, 0x001D407Cu, 0x001D4084u };
            static int sky_sites = -1;
            if (sky_sites < 0) { const char *e = getenv("ACZ_VR_SKY_SITES"); sky_sites = e && *e == '1'; }   /* opt-in: no visible effect found */
            static u32 other_sites[16]; static int n_other;
            u32 ra = ctx->r[31].uw[0];
            int ok = 0;
            for (unsigned i = 0; i < sizeof flight_sites / sizeof flight_sites[0]; i++)
                if (flight_sites[i] == ra && (sky_sites || i < 6u)) ok = 1;
            static int track_all = -1;
            if (track_all < 0) track_all = getenv("ACZ_VR_TRACK_ALL") != NULL;   /* hangar, briefing 3D, cutscenes */
            if (track_all) ok = 1;
            if (!ok) {
                int seen = 0;
                for (int i = 0; i < n_other; i++) if (other_sites[i] == ra) seen = 1;
                if (!seen && n_other < 16) {
                    other_sites[n_other++] = ra;
                    ps2_log("zero-vr: world camera %08X updated from %08X -- not head tracked", c, ra);
                }
                main_cam = 0;
            }
        }
        if (z_xr >= PS2_XR_MONO && main_cam) {
            /* Engage only after ~5 s of continuous flight-camera frames: the hangar->mission deploy shot uses the
               same call sites for ~40 frames, and its logic breaks when that camera is rotated.
               ACZ_VR_ENGAGE_FIELDS=N (default 60; 0 = always the 300 fields, as before; qw_startup 2026-10-24,
               startup-pipelines.md #2): once the HUD manager (instr_hook: it runs only while the player flies) has run
               within this continuous run of flight-camera fields, tracking engages N fields after its first run there.
               In M2 / M4 the player had control at field ~153 but stereo came at ~453: 5 s of flat screen at every
               such start and after every 2-s pause. Before the HUD manager runs (an intro flyby, the deploy shot) the
               300 fields stay. */
            static u64 last_f, run, run_from, hud_from;
            static int engaged, engage_n = -1;
            if (engage_n < 0) { const char *e = getenv("ACZ_VR_ENGAGE_FIELDS"); engage_n = e && *e ? atoi(e) : 60;
                                if (engage_n < 0) engage_n = 0; }
            u64 f = ps2_kernel_vblank_count();
            {   extern volatile u64 ps2_vk_flight_field;   /* (ACZ_REC_CLEAN_STATIC: the game flies; ps2_video.c) */
                ps2_vk_flight_field = f;
            }
            if (f != z_pose_field) {
                u64 gap = f - last_f;
                if (engaged && gap > 120u) {               // more than 2 s without the flight camera: disengage
                    engaged = 0; run = 0; z_engaged_now = 0;
                    ps2_log("zero-vr: head tracking paused (field %llu)", (unsigned long long)f);
                }
                run = (gap <= 4u || engaged) ? run + 1 : 0;
                if (run <= 1u) { run_from = f; hud_from = 0; }    /* (a new run of flight-camera fields) */
                if (!hud_from && z_instr_field && z_instr_field >= run_from) {
                    hud_from = z_instr_field;
                    extern volatile int ps2_vk_prewarm_req;   /* (PS2_PREWARM_TARGETS: made by now unless the load was short) */
                    if (!engaged && engage_n > 0) ps2_vk_prewarm_req = 1;
                }
                {   static int ta = -1;
                    if (ta < 0) ta = getenv("ACZ_VR_TRACK_ALL") != NULL;
                    if (ta && run < 300u) run = 300u;              /* no engagement delay when tracking everything */
                }
                last_f = f;
                z_pose_field = f;
                /* The mission intro flyby updates the flight camera too, so tracking engaged half-way through it
                   (stereo from the middle of the intro on). ACZ_VR_INTRO_FLAT (default on; 0 = the old behaviour):
                   until the HUD manager has run once this session (the player has control), the flight camera
                   frames stay on the static screen like a menu, and the recentre waits for the flight. Later
                   missions' intros were static already: the HUD manager's last run is old there. */
                static int intro_flat = -1, recentre_due; static unsigned intro_mg;
                if (ps2_menv_take(&intro_mg, ps2_menv_gen) | (intro_flat < 0)) {
                    const int o = intro_flat;
                    const char *e = ps2_menv("ACZ_VR_INTRO_FLAT");
                    intro_flat = !(e && *e == '0') && !getenv("ACZ_VR_TRACK_ALL");
                    if (o >= 0) ps2_menv_note("ACZ_VR_INTRO_FLAT", o, intro_flat);
                }
                int engage_now = 0;
                if (!engaged && (run >= 300u || (engage_n > 0 && hud_from && f - hud_from >= (u64)engage_n))) {
                    engaged = 1; engage_now = 1;
                    /* Recentre on the head's current yaw when the flight starts (ACZ_VR_NO_AUTO_RECENTER=1 keeps the
                       reference taken at session start / F9). */
                    if (!ps2_menv("ACZ_VR_NO_AUTO_RECENTER")) recentre_due = 1;
                }
                /* Menus over the flight (pause, mission start / end): no head tracking, the frame goes to the static
                   screen like the other menus (ACZ_VR_MENU_STATIC=0 keeps tracking) */
                static int menu_static = -1, was_menu;
                if (menu_static < 0) { const char *e = getenv("ACZ_VR_MENU_STATIC"); menu_static = !(e && *e == '0'); }
                int intro = intro_flat && engaged && !z_instr_field;
                /* Mission events that hold the camera (pt_flat 2026-10-29, ACZ_VR_EVENT_FLAT, default on; 0 = before:
                   head-tracked stereo through them): flag 0x2000 of the pad player's +0x1A68 (zv_player_update: view
                   0xFE) means the event's own camera, not the pilot's -- the take-off clearance beside the runway (M2,
                   M13), the emergency launch on the apron (M15), the flyby after a take-off before the stage load
                   ("PRESS START BUTTON TO SKIP"). Head-tracked, those cameras sat at runway level (the user's head half
                   under the ground with the eye height added) and swung past at speed (dizzying). They now go to the
                   static screen like the intro (z_event_flat_now); the take-off roll in the cockpit (flag 0x1000 only)
                   stays in stereo, and the flight-start recentre waits for the pilot's view (z_recentre_hand_over). */
                int event = z_event_flat_now(f, engaged);
                int menu = (menu_static && engaged && z_instr_field && f - z_instr_field > 8u) || intro || event;
                menu = z_resume_hold(f, menu, was_menu);
                if (!menu && (was_menu || engage_now)) {
                    /* The hand-over from the static screen (pt_flat 2026-10-29, ACZ_VR_HANDOVER_VIEW, default on; 0 =
                       before): the renderer finds the cockpit view by counting the game's cockpit draws in head-tracked
                       frames only, so the first tracked frame after an event, the intro or the engagement at a flight
                       start (the cockpit's detection was dropped while the event's camera ran, or never made yet)
                       still counted as "outside": shown head-locked with the head
                       turning the game's right stick -- one frame of stick look kicked the game's look-around (M2,
                       head 15 deg: the camera 3.0 deg off, back to 0 over 0.5 s; "the aircraft moved a bit to the
                       left"). When the game's own view byte says cockpit, the detection starts from it. */
                    static int hov = -1;
                    if (hov < 0) { const char *e = getenv("ACZ_VR_HANDOVER_VIEW"); hov = !(e && *e == '0'); }
                    extern volatile int ps2_vk_cockpit_view;
                    if (hov && zv_hold_on > 0 && zv_view_field && f - zv_view_field <= 8u && zv_view_now == 2u
                        && !ps2_vk_cockpit_view) {
                        ps2_vk_cockpit_view = 1;
                        ps2_log("zero-vr: hand-over to the cockpit view: taken from the game's view byte (field %llu; "
                                "ACZ_VR_HANDOVER_VIEW)", (unsigned long long)f);
                    }
                }
                if (menu != was_menu || (menu && event != z_event_was)) {
                    ps2_log("zero-vr: %s (field %llu)", intro ? "mission intro: static screen until the flight starts"
                                                        : event ? "a mission event holds the camera: static screen until the pilot's view returns"
                                                        : menu ? "menu over the flight: static screen, head tracking off"
                                                               : "flight resumed: head tracking on", (unsigned long long)f);
                    was_menu = menu;
                }
                z_event_was = event;
                /* (with ACZ_VR_RECENTRE_KEEP the recentre waits for the first head-tracked frame: before, a later
                   mission's intro -- static, the HUD manager's last run old -- took it at the engagement, in the
                   middle of the intro, and the cinema screen jumped under the user's eyes) */
                if (recentre_due && (z_recentre_keep() > 0.0f ? !menu : !intro && !event)) {
                    recentre_due = 0;
                    z_recentre_hand_over(f);
                }
                {   /* Smooth mode in in-engine cutscenes (the mission intro flyby: tracked, but no HUD manager update
                       yet). ACZ_VR_SMOOTH_CUTSCENES=0: no frames between game frames while the HUD manager does not
                       run (the cutscene steps at the game's 60 Hz; fallback if effects still trail aircraft there). */
                    static int cut = -1, was_cut;
                    extern volatile int ps2_vk_xr_no_rerender;
                    if (cut < 0) { const char *e = getenv("ACZ_VR_SMOOTH_CUTSCENES"); cut = e && *e == '0'; }
                    int nc = cut && (!z_instr_field || f - z_instr_field > 8u);
                    if (nc != was_cut) {
                        ps2_log("zero-vr: %s (field %llu)", nc ? "cutscene: no frames between game frames"
                                                               : "flight: frames between game frames again", (unsigned long long)f);
                        was_cut = nc;
                    }
                    ps2_vk_xr_no_rerender = nc;
                }
                {   extern volatile int ps2_vk_cockpit_hold;     /* the game's view holds the cockpit (z_in_cockpit) */
                    ps2_vk_cockpit_hold = zv_hold_on > 0 && zv_view_field && zv_view_now == 2u
                                          && (f - zv_view_field <= 8u || menu);   /* menus: kept, see above */
                    static int dbg = -1;                         /* ACZ_DEBUG_CKVIEW=N: every N-th field */
                    if (dbg < 0) { const char *e = getenv("ACZ_DEBUG_CKVIEW"); dbg = e ? atoi(e) : 0; }
                    if (dbg > 0 && f % (u64)dbg == 0u) {
                        float yaw = 0.0f, pitch = 0.0f;
                        if (z_pose.valid) {
                            float H[3][3];
                            m3_quat(z_pose.q, H);
                            float fy = -H[1][2];
                            yaw = atan2f(-H[0][2], H[2][2]) * 57.29578f;
                            pitch = asinf(fy < -1 ? -1 : fy > 1 ? 1 : fy) * 57.29578f;
                        }
                        ps2_log("ckview-ee: field %llu head yaw %.0f pitch %.0f (valid %d) game view %u (%llu fields ago) "
                                "hold %d cockpit %d engaged %d menu %d event %d; game camera yaw %.1f pitch %.1f (ra %08X)",
                                (unsigned long long)f, yaw, pitch, z_pose.valid,
                                zv_view_now, zv_view_field ? (unsigned long long)(f - zv_view_field) : 0ull,
                                ps2_vk_cockpit_hold, z_in_cockpit(), engaged, menu, event,
                                f_of(ps2_r32(c + 0x44u)) * 57.29578f, f_of(ps2_r32(c + 0x40u)) * 57.29578f,
                                ctx->r[31].uw[0]);
                    }
                }
                {   /* ACZ_VR_TRACK_END_TAG (default 1; 0 = old; ov_visual 2026-10-25): when head tracking stops (a menu
                       over the flight, a static spell) the GS stream gets the frame number 0 (ps2_xr_end_tags). Before,
                       the draws kept the tracked spell's last number; it went stale after 0.25 s without tags, but the
                       first tag of the resume made it live again, so the frames still in the pipeline -- drawn with the
                       game's flat camera (its 2D cockpit and full-size HUD, no 3D cockpit: 937 draws a field without
                       cockpit parts against 1875 with 492) -- went to the headset as head-tracked stereo frames with the
                       pose from before the pause (M2 pause > START: a zoomed flat image for ~2 game frames at every
                       resume; M17: a black frame). */
                    static int end_tag = -1, was_tracked;
                    if (end_tag < 0) { const char *e = getenv("ACZ_VR_TRACK_END_TAG"); end_tag = !(e && *e == '0'); }
                    int tracked = engaged && !menu;
                    if (!tracked && was_tracked && end_tag) {
                        extern void ps2_xr_end_tags(void);
                        ps2_xr_end_tags();
                    }
                    was_tracked = tracked;
                }
                if (engaged && !menu) {
                    static int was_cockpit = -1;
                    int ck = z_in_cockpit();
                    ps2_xr_set_head_locked(z_stick && !ck);
                    if (z_stick && ck != was_cockpit && was_cockpit >= 0)
                        ps2_log("zero-vr: %s view: %s", ck ? "cockpit" : "outside", ck ? "free head look (world-locked)" : "stick head look (head-locked)");
                    was_cockpit = ck;
                    ps2_xr_game_frame(&z_pose);
                } else z_pose.valid = 0;
                z_engaged_now = engaged;
                if (engage_now) {
                    char hs[48];
                    if (hud_from) snprintf(hs, sizeof hs, "since field %llu", (unsigned long long)hud_from);
                    else snprintf(hs, sizeof hs, "not yet");
                    ps2_log("zero-vr: head tracking engaged (field %llu; flight camera since field %llu, HUD manager %s; "
                            "%s), FOV tangents %.3f x %.3f (limit %.3f)", (unsigned long long)f, (unsigned long long)run_from,
                            hs, run >= 300u ? "300 fields" : "ACZ_VR_ENGAGE_FIELDS", z_pose.tan_h, z_pose.tan_v, z_fov_limit);
                }
            }
            if (z_pose.valid) apply_head(c, z_depth);
        } else if (z_k != 1.0f && d > 1.0f && d < 100000.0f) ps2_w32(c + 8u, w_of(d / z_k));
    }
    z_depth++;
    z_calls++;
    return 0;
}

/* ACZ_DUMP_FIELD=<vblank field>: log every view context updated during the 3 fields after that one (address,
   caller, size, distance, eye, angles, screen projection +0x90 and world->screen +0x110), to identify the main
   camera and verify the rotation convention offline. */
static void dump_ctx(u32 c, u32 ra) {
    char line[700]; int n = 0;
    n += snprintf(line + n, sizeof line - (size_t)n, "zero-vr: ctx %08X ra %08X field %llu wh %.1f %.1f d %.3f eye", c, ra,
                  (unsigned long long)ps2_kernel_vblank_count(), f_of(ps2_r32(c)), f_of(ps2_r32(c + 4u)), f_of(ps2_r32(c + 8u)));
    for (u32 i = 0; i < 3; i++) n += snprintf(line + n, sizeof line - (size_t)n, " %.3f", f_of(ps2_r32(c + 0x30u + 4u * i)));
    n += snprintf(line + n, sizeof line - (size_t)n, " ang");
    for (u32 i = 0; i < 3; i++) n += snprintf(line + n, sizeof line - (size_t)n, " %.5f", f_of(ps2_r32(c + 0x40u + 4u * i)));
    ps2_log("%s", line);
    for (u32 m = 0; m < 2; m++) {
        u32 base = c + (m ? 0x110u : 0x90u);
        n = snprintf(line, sizeof line, "zero-vr:   %s", m ? "S110" : "P090");
        for (u32 i = 0; i < 16; i++) n += snprintf(line + n, sizeof line - (size_t)n, " %.6g", f_of(ps2_r32(base + 4u * i)));
        ps2_log("%s", line);
    }
}

/* The head-turned camera of the latest update per view context. The update hook gives the game its own eye, angles,
   screen distance and height back when the update returns (game logic must not see the head), but the cloud field
   (0x16A6C0) builds its own world->screen matrix from those fields later in the frame: in world-locked (cockpit) frames
   the clouds were drawn for the camera without the head and followed it. The cloud field gets the head-turned values
   for its duration (ACZ_VR_CLOUD_HEAD=0: off). */
static struct { u32 c; u64 field; u32 v[8]; } z_headcam[4];
static u32 z_cloud_c[4], z_cloud_saved[4][8];
static int z_cloud_n;
/* the sky module's functions that build their own matrix from the view context (0x168130), called by the cloud
   builder 0x16B008: role 0 = the cloud field 0x16A6C0 (context in a1): projects each cloud's world position into one
   screen-aligned sprite (centre, size f*S/w, clipped to the screen); role 1 = the cloud images: the puff texture
   0x168BC8 (two puff layers turned by the camera roll, brightness by the camera pitch), the impostor camera 0x169408
   (the 3D puff clusters projected with the camera rotation, the self-shadow offset from the sun in view space) and
   the impostor build 0x169D60 (reads the roll and pitch itself; context in t2); role 2 = another sky layer 0x16B848
   (context in a2). The game turns the images with its camera, so for the head-turned camera every cloud changed
   shape when the head turned (yaw/pitch re-project the puff clusters) -- ACZ_VR_CLOUD_LOOK=base (default) builds them
   for the aircraft's camera and the renderer turns each sprite by the head's roll instead (xr_cloud_upright). */
static const struct { u32 addr, reg, role; } z_sky_fn[] = {
    { 0x0016A6C0u, 5, 0 }, { 0x00168BC8u, 6, 1 }, { 0x00169408u, 6, 1 }, { 0x0016B848u, 6, 2 },
    { 0x00169D60u, 10, 1 } };
static int z_cloud_look_head;                  /* ACZ_VR_CLOUD_LOOK=head: the images for the head-turned camera */
/* The cloud field clips each sprite to the screen rectangle ctx+0x190/+0x194 +- (s16 ctx+0x18, +0x1A) / 2 and drops
   the ones outside it. The renderer draws the sprites as billboards (ps2_video.c xr_cloud_upright), which reach
   further out radially than the game's squares: a cloud whose square lay just off screen was dropped although its
   billboard reached into the view, and it popped in and out at the edges as the head turned. While the field runs in
   head-tracked frames the rectangle is ACZ_VR_CLOUD_CULL (1.6) times larger (the renderer knows the factor; the GS
   scissor still clips). */
static float z_cloud_cull = -1.0f;
static int z_cloud_bb;                          /* the renderer draws the cloud sprites as billboards */
static u32 z_cloud_wh_c[4], z_cloud_wh_saved[4];
/* The cloud field wraps its 131072-unit tile of cloud positions into a window centred 73728 units (0x3F8468, read
   only by 0x16A6C0) along the view direction, per world axis +-65536: when the view runs along a world axis only
   clouds 8192..139264 units ahead are in it, so a near cloud to the side (or less than 8192 ahead) was in the window
   for one head direction and wrapped 131072 units away for another -- near clouds popped in and out as the head
   turned (mission 1 overcast, cloudcheck yaw +20: a sheet over the upper view after the step). In head-tracked
   frames the window is centred max(0, far - 65536) along the view instead (far = min(environment +0x278, 139264)):
   on the eye when every cloud within the far distance fits (51200 in missions 1 and 3), so no cloud depends on
   where one looks; ACZ_VR_CLOUD_WINDOW=0: the game's window. */
#define Z_CLOUD_WIN 0x003F8468u
static int z_cloud_win = -1;
static u32 z_cloud_win_saved[4]; static int z_cloud_win_set[4];
extern volatile float ps2_vk_cloud_cull;      /* ps2_video.c reads it */
/* Bug #116: the game's clouds decided and shaped independently of the head (ACZ_VR_CLOUD_BASE=1; off by default until a headset A/B). The field
   projects every cloud with the head-turned camera (0x16A2F0), so whether a cloud was drawn at all and the shape of its
   screen square depended on where one looked: the projection drops a cloud whose centre is behind the view plane
   (projected z <= 0) or whose square lies outside the clip rectangle, and the square (f S / w, screen-aligned) grows
   and slides with the head's view depth. Mission 1's overcast clouds are big impostors (S/D 0.6..6 as the renderer
   measures them, i.e. squares 30..80 deg across even 30-75 km away), so a 20 deg head yaw dropped or added a big near
   square (sprite draws 216 -> 182, the upper view's cloud layer 30.6 -> 11.9 levels in one frame; cloudcheck lock_m1
   yaw -20 0.39, yaw +20 0.67). Now in head-tracked frames each cloud is projected with a virtual camera that looks
   straight at it (vf5..vf8 = M_head T, T turning the cloud's direction onto the head's axis about the eye; vf2 scaled
   so the square stays below 30 px, one 64-px column; a 1/16 px row offset per cloud so the renderer can tell them
   apart): the game
   never drops it for the head's sake (its distance fade, coverage and draw order are unchanged), and the renderer
   (ps2_video.c xr_cloud_upright) gets the cloud's world direction and its S/D per XR frame
   (ps2_vk_cloud_base_add) and draws it as a world-fixed shape: a billboard, or, where the renderer kept the game's
   square (S/D above ACZ_VR_CLOUD_BBMAX - 0.3), the square the game draws for the aircraft's own camera on that camera's
   image plane (the 2006 picture when looking ahead), blended into the billboard 60..80 deg off the nose. Clouds whose
   whole shape lies outside the head's view (+4 deg) are dropped as before (nothing visible changes).
   ACZ_VR_CLOUD_BASE=0: every cloud with the head's camera (before 2026-10-25). */
static int z_cb_on, z_cb_dbg;
static float z_cb_bbmax = 0.9f;
static struct { int active, lvl, n; u32 seq; float Rh[3][3], Rb[3][3], fh[3], fb[3]; } z_cb;
static struct { int virt, drop; u32 vf[5][4]; float d[3], r; } z_cbc;    /* one projection call (saved vf2, vf5..vf8) */
extern void ps2_vk_cloud_base_frame(u32 seq);
extern volatile int ps2_vk_cloud_base_ab;     /* ACZ_VR_CLOUD_BASE_AB=N (measurement): blocks of N fields on / off */
extern void ps2_vk_cloud_base_add(u32 seq, const float rows[4], const float d[3], float r);
static const u32 z_headcam_off[8] = { 0x30u, 0x34u, 0x38u, 0x40u, 0x44u, 0x48u, 0x8u, 0x4u };
static void headcam_note(u32 c) {
    int k = 0;
    for (int i = 0; i < 4; i++) if (z_headcam[i].c == c) { k = i; goto found; }
    for (int i = 1; i < 4; i++) if (z_headcam[i].field < z_headcam[k].field) k = i;
found:
    z_headcam[k].c = c;
    z_headcam[k].field = ps2_kernel_vblank_count();
    for (int i = 0; i < 8; i++) z_headcam[k].v[i] = ps2_r32(c + z_headcam_off[i]);
}
static void cloud_data_field(ps2_ctx *ctx);
/* user = context register | role << 8 | head << 16 (head: give the function the head-turned camera) */
static int cloud_enter(ps2_ctx *ctx, void *user) {
    uintptr_t u = (uintptr_t)user;
    u32 c = ctx->r[u & 0xFFu].uw[0];               /* the view context argument */
    u32 role = (u32)(u >> 8) & 0xFFu;
    u64 f = ps2_kernel_vblank_count();
    if (z_cloud_n >= 4) return 0;
    int lvl = z_cloud_n++;
    z_cloud_c[lvl] = 0;
    z_cloud_wh_c[lvl] = 0;
    z_cloud_win_set[lvl] = 0;
    if (!z_pose.valid) return 0;
    for (int i = 0; (u >> 16) & 1u && i < 4; i++)
        if (z_headcam[i].c == c && f - z_headcam[i].field <= 2u) {
            for (int j = 0; j < 6; j++) {                 /* eye and angles (the screen stays the game's) */
                z_cloud_saved[lvl][j] = ps2_r32(c + z_headcam_off[j]);
                ps2_w32(c + z_headcam_off[j], z_headcam[i].v[j]);
            }
            z_cloud_c[lvl] = c;
            break;
        }
    if (role == 0u && z_cloud_win > 0) {
        u32 obj = ctx->r[4].uw[0], env = obj >= 0x100000u && obj < 0x2000000u ? ps2_r32(obj + 0xD4u) : 0u;
        float far = env >= 0x100000u && env < 0x2000000u ? f_of(ps2_r32(env + 0x278u)) : 139264.0f;
        if (!(far > 0.0f) || far > 139264.0f) far = 139264.0f;
        float o = far - 65536.0f;
        if (o < 0.0f) o = 0.0f;
        if (o > 73728.0f) o = 73728.0f;
        z_cloud_win_saved[lvl] = ps2_r32(Z_CLOUD_WIN); z_cloud_win_set[lvl] = 1;
        ps2_w32(Z_CLOUD_WIN, w_of(o));
    }
    if (role == 0u && z_cloud_cull > 1.0f && c >= 0x1000u && c < 0x02000000u) {
        u32 wh = ps2_r32(c + 0x18u);
        s32 w = (s16)(wh & 0xFFFFu), h = (s16)(wh >> 16);
        if (w > 0 && h > 0 && w < 4096 && h < 4096) {
            s32 w2 = (s32)((float)w * z_cloud_cull), h2 = (s32)((float)h * z_cloud_cull);
            z_cloud_wh_saved[lvl] = wh; z_cloud_wh_c[lvl] = c;
            ps2_w32(c + 0x18u, ((u32)h2 << 16) | ((u32)w2 & 0xFFFFu));
        }
    }
    if (role == 0u && z_cb_on && z_cb_dbg > 0 && (z_pose.seq % (u32)z_cb_dbg) == 0u) {
        static int shown;
        if (shown++ < 60)
            ps2_log("cloud base: seq %u field %llu: cloud field level %d context %08X head %s, list %s, a0 %08X a3 %08X",
                    z_pose.seq, (unsigned long long)f, lvl, c, z_cloud_c[lvl] ? "applied" : "not applied",
                    z_cb.active ? "already active" : "new", ctx->r[4].uw[0], ctx->r[7].uw[0]);
    }
    if (role == 0u && z_cb_on && z_cloud_c[lvl] && !z_cb.active
        && !(ps2_vk_cloud_base_ab > 0 && ((f / (u64)ps2_vk_cloud_base_ab) & 1u))) {
        /* the head's and the aircraft's camera (R = view -> world, the camera record's convention, view z backwards) */
        m3_euler(f_of(ps2_r32(c + 0x40u)), f_of(ps2_r32(c + 0x44u)), f_of(ps2_r32(c + 0x48u)), z_cb.Rh);
        m3_euler(f_of(z_cloud_saved[lvl][3]), f_of(z_cloud_saved[lvl][4]), f_of(z_cloud_saved[lvl][5]), z_cb.Rb);
        for (int i = 0; i < 3; i++) { z_cb.fh[i] = -z_cb.Rh[i][2]; z_cb.fb[i] = -z_cb.Rb[i][2]; }
        if (z_cb.seq != z_pose.seq) z_cb.n = 0;
        z_cb.active = 1; z_cb.lvl = lvl; z_cb.seq = z_pose.seq;
        ps2_vk_cloud_base_frame(z_pose.seq);
    }
    if (role == 0u) cloud_data_field(ctx);
    if (role <= 1u && c >= 0x1000u && c < 0x02000000u) {
        /* the roll the positions / images are built with now: world up's angle in that view (view rotation
           Ry(yaw) Rx(pitch) Rz(roll), rows [1][0] = cos p sin r, [1][1] = cos p cos r, as the camera record) */
        float p = f_of(ps2_r32(c + 0x40u)), r = f_of(ps2_r32(c + 0x48u));
        float roll = atan2f(cosf(p) * sinf(r), cosf(p) * cosf(r));
        ps2_xr_set_cloud_roll(z_pose.seq, (int)role, roll);
        {   static int dbg = -1; static u64 shown;
            if (dbg < 0) { const char *e = getenv("ACZ_DEBUG_CLOUD_LOOK"); dbg = e ? atoi(e) : 0; }
            if (dbg > 0 && f >= 600u && (f % (u64)dbg) == 0u && shown != f * 4u + role) {
                shown = f * 4u + role;
                ps2_log("zero-vr: clouds field %llu seq %u %s: %s camera, angles %.2f %.2f %.2f deg -> roll %.2f deg",
                        (unsigned long long)f, z_pose.seq, role ? "images" : "positions",
                        z_cloud_c[lvl] ? "head-turned" : "aircraft's", p * 57.29578f,
                        f_of(ps2_r32(c + 0x44u)) * 57.29578f, r * 57.29578f, roll * 57.29578f);
            }
        }
    }
    return 0;
}
/* ACZ_DEBUG_CLOUD_DATA=f1[,f2...] (debug): the cloud field's world-space data at those fields. The field 0x16A6C0
   (a0 = cloud object, a1 = view context, a3 = parameters P = obj[0]) walks obj[1] (= *(obj+4)): count u16 at P+0x70
   (at most 400) entries of 8 bytes (s16 x, u16 y (12 bits, 0x800 = middle), s16 z, s16 size). Each becomes a position
   relative to a window centred 73728 units along the view axis (vf1, scratchpad 0x700000A0) of a periodic tile of
   131072 units: rel = ((2 x + offset) & 0x1FFFF) - 0x10000 per horizontal axis, y = offset + P[0x80 + 16 v] -
   P[0x84 + 16 v] (y - 0x800) for variant v = index & mask; size = s16 * 2304 * P[0x88 + 16 v]; the offset moves with
   the wind (P+0x10/+0x14 per second x the frame counter obj+0xD0 / 60). World position = rel + eye - vf1 (as the
   game hands it to 0x16E290): fixed in the world while the aircraft flies (checked: mission 3, 200 clouds, all at
   y 6000; mission 1, 280 clouds, 4 variants at 12900..14350 +- 1.2..1.4 x 2048). Then 0x16A2F0 projects it (a0 = rel,
   f12 = size, f0 back = view depth, -1 off screen); the view depth fades it in over 4096..12288 and out from far / 2
   to far (far = min(environment +0x278, 139264): 51200 in missions 1 and 3), and a 256x256 coverage map (0x16A590,
   *(obj+8)) at the world position scales its alpha. */
/* Cloud fade by distance (ACZ_VR_CLOUD_FADE=0: the game's view depth). 0x16A2F0 hands back the cloud's view depth w
   (f0; -1 off screen), and the field fades clouds in over w 4096..12288, out towards its far limit, skips w <= 4096
   and sorts its draw buckets by it; the true distance |p - eye| it also computes goes to the record (a1 + 0, used for
   the order). With the head-turned camera w = distance x cos(angle off the view axis), so a head turn changed every
   near or far cloud's opacity (mission 3, head yaw 20: one cloud's alpha 65 -> 37) -- the clouds breathed with the
   head. Head-tracked frames get the distance instead (the game's own fade curve, independent of where one looks) --
   but only past the near band: the near cut and fade-in by w hide big near clouds close to the view plane. Faded by
   distance they came in as sheets the size of the view as soon as a head turn brought their centre in front of the
   view plane (mission 1 overcast, head yaw 30 + pitch 10: the sky gap above filled with cloud; cloudcheck shape
   agreement 0.86 -> 0.30) -- unbounded squares with the game's sprites, and still a pop with the renderer's
   billboards, since the game drops clouds centred behind the view plane. So the value handed back is w up to 12288
   (the end of the fade-in), the distance from 20480 on, a blend between (continuous in w) -- ACZ_VR_CLOUD_FADE=1.
   Default (=2): the distance everywhere; with the eye-centred wrap window and the renderer's billboards the near
   clouds no longer pop in and out with the head, and the w band also faded the overcast's own clouds as one looked
   around inside it (cloudcheck mission 1 yaw +20: 0.70 with the distance, 0.08 with the band). =0: the game's w. */
static u32 z_cfade_rec;
static int z_cfade_mode = 2;
static int cloud_fade_enter(ps2_ctx *ctx, void *user) {
    (void)user;
    u32 ra = ctx->r[31].uw[0];
    z_cfade_rec = z_pose.valid && ra >= 0x0016A6C0u && ra < 0x0016B008u ? ctx->r[5].uw[0] : 0u;
    return 0;
}
static int cloud_fade_leave(ps2_ctx *ctx, void *user) {
    (void)user;
    if (!z_cfade_rec || !(ctx->f[0].f > 0.0f)) return 0;
    float d = f_of(ps2_r32(z_cfade_rec)), w = ctx->f[0].f;
    if (d >= w && d < 1e7f) {                              /* (never nearer than the depth: a sanity check) */
        float s = z_cfade_mode == 2 ? 1.0f : (w - 12288.0f) / 8192.0f;
        s = s < 0.0f ? 0.0f : s > 1.0f ? 1.0f : s;
        ctx->f[0].f = w + (d - w) * s;
    }
    z_cfade_rec = 0u;
    return 0;
}
/* Bug #116 (ACZ_VR_CLOUD_BASE, see z_cb above): per cloud, 0x16A2F0 (a0 = position, f12 = size S, a1 = record (+8 u0,
   +0xA v0, +0xC u1, +0xE v1 in 1/16 texel), a2 = clip rectangle, vf1 = the eye, vf2 = sprite scale (half size = vf2 S /
   w pixels), vf5..vf8 = the projection; on return vf12 / vf13 = ftoi4 of the clipped corners, f0 = view depth or -1
   when dropped). */
static int cloud_base_enter(ps2_ctx *ctx, void *user) {
    (void)user;
    z_cbc.virt = z_cbc.drop = 0;
    u32 ra = ctx->r[31].uw[0];
    if (!z_cb.active || !(ra >= 0x0016A6C0u && ra < 0x0016B008u)) return 0;
    u32 a0 = ctx->r[4].uw[0], a2 = ctx->r[6].uw[0];
    float p[3], e[3], u[3], D2 = 0.0f, S = ctx->f[12].f;
    for (u32 i = 0; i < 3; i++) p[i] = f_of(ps2_r32(a0 + 4u * i));
    for (int i = 0; i < 3; i++) { e[i] = ctx->vu0.vf[1].f[i]; u[i] = p[i] - e[i]; D2 += u[i] * u[i]; }
    if (!(D2 > 1.0f) || !(S > 0.0f) || a2 < 0x100000u) return 0;
    float D = sqrtf(D2);
    for (int i = 0; i < 3; i++) u[i] /= D;
    /* S/D as the renderer measures it (the square's half size in tangent units times cos off the axis), and the view's
       corner angle: focal lengths from M's x and y rows against its w row, the screen = the clip rectangle / CULL */
    static u32 kmh[4][4], kv2[2], krc[4]; static float ksd, vcorner;
    if (memcmp(kv2, ctx->vu0.vf[2].u, sizeof kv2) || memcmp(kmh[0], ctx->vu0.vf[5].u, 16) || memcmp(kmh[1], ctx->vu0.vf[6].u, 16)
        || memcmp(kmh[2], ctx->vu0.vf[7].u, 16) || memcmp(kmh[3], ctx->vu0.vf[8].u, 16)
        || krc[0] != ps2_r32(a2) || krc[2] != ps2_r32(a2 + 8u) || krc[3] != ps2_r32(a2 + 12u)) {
        float X[3], Y[3], W[3], ww = 0.0f, xw = 0.0f, yw = 0.0f, fx = 0.0f, fy = 0.0f;
        for (int i = 0; i < 3; i++) {
            X[i] = ctx->vu0.vf[5 + i].f[0]; Y[i] = ctx->vu0.vf[5 + i].f[1]; W[i] = ctx->vu0.vf[5 + i].f[3];
            ww += W[i] * W[i]; xw += X[i] * W[i]; yw += Y[i] * W[i];
        }
        for (int i = 0; i < 3 && ww > 0.0f; i++) {
            float aa = X[i] - xw / ww * W[i], bb = Y[i] - yw / ww * W[i];
            fx += aa * aa; fy += bb * bb;
        }
        fx = ww > 0.0f ? sqrtf(fx / ww) : 0.0f; fy = ww > 0.0f ? sqrtf(fy / ww) : 0.0f;
        ksd = fx > 1.0f && fy > 1.0f ? 0.5f * (fabsf(ctx->vu0.vf[2].f[0]) / fx + fabsf(ctx->vu0.vf[2].f[1]) / fy) : 0.0f;
        for (int i = 0; i < 4; i++) { memcpy(kmh[i], ctx->vu0.vf[5 + i].u, 16); krc[i] = ps2_r32(a2 + 4u * (u32)i); }
        memcpy(kv2, ctx->vu0.vf[2].u, sizeof kv2);
        float cf = z_cloud_cull > 1.0f ? z_cloud_cull : 1.0f;
        float hw = 0.5f * (f_of(krc[2]) - f_of(krc[0])) / cf, hh = 0.5f * (f_of(krc[3]) - f_of(krc[1])) / cf;
        vcorner = fx > 1.0f && fy > 1.0f && hw > 0.0f && hh > 0.0f ? atanf(sqrtf((hw / fx) * (hw / fx) + (hh / fy) * (hh / fy))) : 1.6f;
        if (z_cb_dbg > 0) {
            static int shown;
            if (shown++ < 4) ps2_log("cloud base: focal %.1f %.1f, sprite scale %.3f %.3f -> S/D factor %.6f; view corner %.1f deg",
                                     fx, fy, ctx->vu0.vf[2].f[0], ctx->vu0.vf[2].f[1], ksd, vcorner * 57.29578f);
        }
    }
    if (!(ksd > 0.0f)) return 0;
    float r = ksd * S / D;
    /* what the renderer will draw: the billboard (corners at atan(1.42 r) from the centre) and, while it keeps the game's
       square (r > BBMAX - 0.3, centred < 80 deg off the nose), the aircraft camera's square; a cloud all of whose shape
       lies outside the head's view (+4 deg) is dropped as before */
    float ch = u[0] * z_cb.fh[0] + u[1] * z_cb.fh[1] + u[2] * z_cb.fh[2];
    float cb = u[0] * z_cb.fb[0] + u[1] * z_cb.fb[1] + u[2] * z_cb.fb[2];
    float ext = atanf(1.42f * r);
    if (r > z_cb_bbmax - 0.3f && cb > 0.17f) {
        float db[3];
        for (int i = 0; i < 3; i++) db[i] = z_cb.Rb[0][i] * u[0] + z_cb.Rb[1][i] * u[1] + z_cb.Rb[2][i] * u[2];
        float cz = -db[2], tx = db[0] / cz, ty = db[1] / cz, h = r / cz;
        for (int q = 0; q < 4; q++) {
            float cx_ = tx + (q & 1 ? h : -h), cy_ = ty + (q & 2 ? h : -h);
            float dt = (cx_ * db[0] + cy_ * db[1] + cz) / sqrtf(cx_ * cx_ + cy_ * cy_ + 1.0f);
            float ang = acosf(dt < -1.0f ? -1.0f : dt > 1.0f ? 1.0f : dt);
            if (ang > ext) ext = ang;
        }
    }
    float th = acosf(ch < -1.0f ? -1.0f : ch > 1.0f ? 1.0f : ch);
    if (th - ext > vcorner + 0.07f) { z_cbc.drop = 1; return 0; }
    if (z_cb.n >= 500) return 0;                         /* (the renderer's list holds 512 a frame) */
    /* the virtual camera: K turns u onto the head's axis fh (Rodrigues), M = M_head T, T(p) = e + K (p - e) */
    float K[3][3], v[3] = { u[1] * z_cb.fh[2] - u[2] * z_cb.fh[1], u[2] * z_cb.fh[0] - u[0] * z_cb.fh[2],
                            u[0] * z_cb.fh[1] - u[1] * z_cb.fh[0] };
    float s2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    if (s2 > 1e-10f) {
        float g = (1.0f - ch) / s2;
        float V[3][3] = { { 0.0f, -v[2], v[1] }, { v[2], 0.0f, -v[0] }, { -v[1], v[0], 0.0f } };
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
                K[i][j] = (i == j ? 1.0f : 0.0f) + V[i][j] + g * (V[i][0] * V[0][j] + V[i][1] * V[1][j] + V[i][2] * V[2][j]);
    } else if (ch > 0.0f) {
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) K[i][j] = i == j ? 1.0f : 0.0f;
    } else {                                             /* straight behind: half a turn about the head's up axis */
        float a[3] = { z_cb.Rh[0][1], z_cb.Rh[1][1], z_cb.Rh[2][1] };
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) K[i][j] = 2.0f * a[i] * a[j] - (i == j ? 1.0f : 0.0f);
    }
    float M[4][4], B[4][4], c[3];
    for (int k = 0; k < 4; k++) for (int q = 0; q < 4; q++) M[k][q] = ctx->vu0.vf[5 + k].f[q];
    for (int i = 0; i < 3; i++) c[i] = e[i] - (K[i][0] * e[0] + K[i][1] * e[1] + K[i][2] * e[2]);
    float dy = (float)(z_cb.n + 1) / 16.0f;              /* rows 1/16 px apart: the renderer tells the clouds apart */
    for (int q = 0; q < 4; q++) {
        for (int j = 0; j < 3; j++) B[j][q] = M[0][q] * K[0][j] + M[1][q] * K[1][j] + M[2][q] * K[2][j];
        B[3][q] = M[3][q] + M[0][q] * c[0] + M[1][q] * c[1] + M[2][q] * c[2];
    }
    for (int k = 0; k < 4; k++) B[k][1] += dy * B[k][3];
    memcpy(z_cbc.vf[0], ctx->vu0.vf[2].u, 16);
    for (int k = 0; k < 4; k++) memcpy(z_cbc.vf[1 + k], ctx->vu0.vf[5 + k].u, 16);
    for (int k = 0; k < 4; k++) for (int q = 0; q < 4; q++) ctx->vu0.vf[5 + k].f[q] = B[k][q];
    float hpx = fabsf(ctx->vu0.vf[2].f[0]) * S / D;      /* the square stays below 30 px (one or two columns: the */
    if (hpx > 30.0f) { ctx->vu0.vf[2].f[0] *= 30.0f / hpx; ctx->vu0.vf[2].f[1] *= 30.0f / hpx; }   /* renderer slices) */
    for (int i = 0; i < 3; i++) z_cbc.d[i] = u[i];      /* (world: the renderer turns it with its own camera record) */
    z_cbc.r = r;
    z_cbc.virt = 1;
    if (z_cb_dbg > 0 && (z_cb.seq % (u32)z_cb_dbg) == 0u) {
        static int shown;
        if (shown++ < 300)
            ps2_log("cloud base: seq %u cloud D %.0f S/D %.3f off head %.1f nose %.1f deg, shape %.1f deg -> virtual, row "
                    "offset %.4f", z_cb.seq, D, r, th * 57.29578f, acosf(cb < -1.0f ? -1.0f : cb > 1.0f ? 1.0f : cb) * 57.29578f,
                    ext * 57.29578f, dy);
    }
    return 0;
}
static int cloud_base_leave(ps2_ctx *ctx, void *user) {
    (void)user;
    if (z_cbc.virt) {
        memcpy(ctx->vu0.vf[2].u, z_cbc.vf[0], 16);
        for (int k = 0; k < 4; k++) memcpy(ctx->vu0.vf[5 + k].u, z_cbc.vf[1 + k], 16);
        if (ctx->f[0].f > 0.0f) {
            /* the rows (vf12 / vf13 = ftoi4 of the corners) and texel rows (record +0xA, +0xE) as the GS gets them */
            u32 rec = ctx->r[5].uw[0];
            float rows[4] = { (float)ctx->vu0.vf[12].s[1] / 16.0f, (float)ctx->vu0.vf[13].s[1] / 16.0f,
                              rec >= 0x100000u ? (float)ps2_r16(rec + 0xAu) / 16.0f : 0.0f,
                              rec >= 0x100000u ? (float)ps2_r16(rec + 0xEu) / 16.0f : 0.0f };
            ps2_vk_cloud_base_add(z_cb.seq, rows, z_cbc.d, z_cbc.r);
            z_cb.n++;
        }
    }
    if (z_cbc.drop) ctx->f[0].f = -1.0f;
    z_cbc.virt = z_cbc.drop = 0;
    return 0;
}

static long z_cfd_fields[8]; static int z_cfd_n = -1;
static struct { u32 c, obj; float eye[3]; int idx; } z_cfd;
static int z_cfd_now(void) {
    if (z_cfd_n < 0) {
        const char *e = getenv("ACZ_DEBUG_CLOUD_DATA"); z_cfd_n = 0;
        while (e && *e && z_cfd_n < 8) { char *q; z_cfd_fields[z_cfd_n++] = strtol(e, &q, 0); if (q == e) break; e = *q == ',' ? q + 1 : q; }
    }
    long f = (long)ps2_kernel_vblank_count();
    for (int i = 0; i < z_cfd_n; i++) if (f == z_cfd_fields[i]) return 1;
    return 0;
}
static void cloud_data_field(ps2_ctx *ctx) {
    if (!z_cfd_now()) return;
    u32 obj = ctx->r[4].uw[0], c = ctx->r[5].uw[0], P = ctx->r[7].uw[0];
    if (obj < 0x100000u || obj >= 0x2000000u || P < 0x100000u || P >= 0x2000000u) return;
    z_cfd.c = c; z_cfd.obj = obj; z_cfd.idx = 0;
    for (u32 i = 0; i < 3; i++) z_cfd.eye[i] = f_of(ps2_r32(c + 0x30u + 4u * i));
    u32 E = ps2_r32(obj + 4u), n = ps2_r16(P + 0x70u);
    ps2_log("cloud data: field %llu obj %08X P %08X entries %08X count %u (cap 400) mask %u eye %.0f %.0f %.0f ang %.4f %.4f "
            "%.4f; wind %.3f %.3f; counter %d; drift %.1f %.1f; coverage map %08X; P+0x6C %u",
            (unsigned long long)ps2_kernel_vblank_count(), obj, P, E, n, ctx->r[8].uw[0], z_cfd.eye[0], z_cfd.eye[1],
            z_cfd.eye[2], f_of(ps2_r32(c + 0x40u)), f_of(ps2_r32(c + 0x44u)), f_of(ps2_r32(c + 0x48u)),
            f_of(ps2_r32(P + 0x10u)), f_of(ps2_r32(P + 0x14u)), (int)ps2_r32(obj + 0xD0u), f_of(ps2_r32(obj + 0xE0u)),
            f_of(ps2_r32(obj + 0xE8u)), ps2_r32(obj + 8u), ps2_r16(P + 0x6Cu));
    for (u32 v = 0; v < 4; v++)
        ps2_log("cloud data:   variant %u: height %.1f, height scale %.3f, size scale %.4f, %.4f", v,
                f_of(ps2_r32(P + 0x80u + 16u * v)), f_of(ps2_r32(P + 0x84u + 16u * v)), f_of(ps2_r32(P + 0x88u + 16u * v)),
                f_of(ps2_r32(P + 0x8Cu + 16u * v)));
    {   char ln[1200]; int k = 0;
        int x0 = 99999, x1 = -99999, y0 = 99999, y1 = -99999, z0 = 99999, z1 = -99999, s0 = 99999, s1 = -99999;
        for (u32 i = 0; E >= 0x100000u && E < 0x2000000u && i < n && i < 400u; i++) {
            int x = (s16)ps2_r16(E + 8u * i), y = (int)(ps2_r16(E + 8u * i + 2u) & 0xFFFu) - 0x800;
            int z = (s16)ps2_r16(E + 8u * i + 4u), sz = (s16)ps2_r16(E + 8u * i + 6u);
            x0 = x < x0 ? x : x0; x1 = x > x1 ? x : x1; y0 = y < y0 ? y : y0; y1 = y > y1 ? y : y1;
            z0 = z < z0 ? z : z0; z1 = z > z1 ? z : z1; s0 = sz < s0 ? sz : s0; s1 = sz > s1 ? sz : s1;
            if (i < 24u) k += snprintf(ln + k, sizeof ln - (size_t)k, " (%d %d %d %d)", x, y, z, sz);
        }
        ps2_log("cloud data:   entries x %d..%d, y-0x800 %d..%d, z %d..%d, size %d..%d; first:%s", x0, x1, y0, y1, z0, z1,
                s0, s1, ln);
    }
}
static struct { u32 pos, rec; float size, rel[3], vf1[3]; } z_cfd_p;
static int cloud_data_proj_enter(ps2_ctx *ctx, void *user) {
    (void)user;
    if (!z_cfd.obj || !z_cfd_now()) return 0;
    z_cfd_p.pos = ctx->r[4].uw[0]; z_cfd_p.rec = ctx->r[17].uw[0]; z_cfd_p.size = ctx->f[12].f;
    if (z_cfd.idx == 0)
        ps2_log("cloud data:   fades: near %.0f, fade-in to 12288, far %.0f, far fade from %.0f (slope %.6g), alpha floor %.3f",
                ctx->f[23].f, ctx->f[22].f, ctx->f[24].f, ctx->f[26].f, ctx->f[25].f);
    for (u32 i = 0; i < 3; i++) {
        z_cfd_p.rel[i] = f_of(ps2_r32(z_cfd_p.pos + 4u * i));
        z_cfd_p.vf1[i] = ctx->vu0.vf[1].f[i];
    }
    return 0;
}
static int cloud_data_proj_leave(ps2_ctx *ctx, void *user) {
    (void)user;
    if (!z_cfd.obj || !z_cfd_now()) return 0;
    float w[3], d = 0.0f;
    for (int i = 0; i < 3; i++) { w[i] = z_cfd_p.rel[i] + z_cfd.eye[i] - z_cfd_p.vf1[i]; d += (w[i] - z_cfd.eye[i]) * (w[i] - z_cfd.eye[i]); }
    u32 r = z_cfd_p.rec;
    ps2_log("cloud data:   #%d entry %d %d %d %d rel %.0f %.0f %.0f size %.0f -> world %.0f %.0f %.0f dist %.0f (above eye %.0f) "
            "depth %.0f", z_cfd.idx++, (s16)ps2_r16(r), (int)(ps2_r16(r + 2u) & 0xFFFu) - 0x800, (s16)ps2_r16(r + 4u),
            (s16)ps2_r16(r + 6u), z_cfd_p.rel[0], z_cfd_p.rel[1], z_cfd_p.rel[2], z_cfd_p.size, w[0], w[1], w[2], sqrtf(d),
            w[1] - z_cfd.eye[1], ctx->f[0].f);
    return 0;
}

/* the sky's frame draw (0x1618A0: sky gradient, horizon haze, clouds, sun) reaches the view context through its own
   pointers: every context that was head-turned in the last fields gets its head-turned eye and angles meanwhile */
#define Z_SKY_ROOT 0x001618A0u
static u32 z_sky_c[4], z_sky_saved[4][6];
static int z_sky_n, z_sky_depth;
static int sky_root_enter(ps2_ctx *ctx, void *user) {
    (void)ctx; (void)user;
    {   static int shown;                                  /* debug: the sky object's view context */
        u64 fdbg = ps2_kernel_vblank_count();
        if (shown < 3 && getenv("ACZ_DEBUG_SKY") && fdbg >= 1500u) {
            for (int i = 0; i < 4; i++)
                if (z_headcam[i].c)
                    ps2_log("zero-vr:   headcam %08X field %llu eye %.3f %.3f %.3f ang %.5f %.5f %.5f; now ang %.5f %.5f %.5f",
                            z_headcam[i].c, (unsigned long long)z_headcam[i].field, f_of(z_headcam[i].v[0]),
                            f_of(z_headcam[i].v[1]), f_of(z_headcam[i].v[2]), f_of(z_headcam[i].v[3]),
                            f_of(z_headcam[i].v[4]), f_of(z_headcam[i].v[5]), f_of(ps2_r32(z_headcam[i].c + 0x40u)),
                            f_of(ps2_r32(z_headcam[i].c + 0x44u)), f_of(ps2_r32(z_headcam[i].c + 0x48u)));
            u32 o = ctx->r[4].uw[0], s1 = o ? ps2_r32(o + 0xBB0u) : 0u, c = s1 ? ps2_r32(s1 + 0x260u) : 0u;
            for (u32 b = 0; s1 && b < 6; b++) {
                char ln[400]; int k = 0;
                u32 base = b < 4 ? s1 + 0xB0u + b * 0x40u : c + (b == 4 ? 0x90u : 0x110u);
                k += snprintf(ln + k, sizeof ln - (size_t)k, "zero-vr:   %s+%03X", b < 4 ? "cam" : "ctx", b < 4 ? 0xB0u + b * 0x40u : (b == 4 ? 0x90u : 0x110u));
                for (u32 q = 0; q < 16; q++) k += snprintf(ln + k, sizeof ln - (size_t)k, " %.4g", f_of(ps2_r32(base + 4u * q)));
                ps2_log("%s", ln);
            }
            ps2_log("zero-vr: sky a0 %08X +bb0 %08X +260 %08X; eye %.3f %.3f %.3f ang %.5f %.5f %.5f", o, s1, c,
                    c ? f_of(ps2_r32(c + 0x30u)) : 0.0f, c ? f_of(ps2_r32(c + 0x34u)) : 0.0f, c ? f_of(ps2_r32(c + 0x38u)) : 0.0f,
                    c ? f_of(ps2_r32(c + 0x40u)) : 0.0f, c ? f_of(ps2_r32(c + 0x44u)) : 0.0f, c ? f_of(ps2_r32(c + 0x48u)) : 0.0f);
            shown++;
        }
    }
    if (z_sky_depth++ > 0) return 0;
    z_sky_n = 0;
    if (!z_pose.valid) return 0;
    u64 f = ps2_kernel_vblank_count();
    for (int i = 0; i < 4; i++) {
        u32 c = z_headcam[i].c;
        if (!c || f - z_headcam[i].field > 2u) continue;
        for (int j = 0; j < 6; j++) {
            z_sky_saved[z_sky_n][j] = ps2_r32(c + z_headcam_off[j]);
            ps2_w32(c + z_headcam_off[j], z_headcam[i].v[j]);
        }
        z_sky_c[z_sky_n++] = c;
    }
    return 0;
}
static int sky_root_leave(ps2_ctx *ctx, void *user) {
    (void)ctx; (void)user;
    if (z_sky_depth <= 0 || --z_sky_depth > 0) return 0;
    while (z_sky_n > 0) {
        z_sky_n--;
        for (int j = 0; j < 6; j++) ps2_w32(z_sky_c[z_sky_n] + z_headcam_off[j], z_sky_saved[z_sky_n][j]);
    }
    return 0;
}
/* The sky gradient (dome) and the horizon haze ring are screen-space meshes whose shape and colours come from the sky's
   own camera (sky object +0xBB0): rotation-only copies of the camera made before the head is added (+0xB0: rotation x
   normalized projection +0x130, +0xF0: rotation x screen projection +0x170). In world-locked (cockpit) frames they kept
   the horizon of the aircraft, not of the head (a 12 deg head pitch moved the terrain 33 px, the haze band 0). For the
   duration of the sky draw (0x161098) both are rebuilt from the rotation of the view context's world->screen matrix
   (+0x110, head-turned at that point; projection +0x90 = the sky's +0x170) and put back afterwards. Without a head
   turn the rebuild gives the same matrices. The dome and haze do not use these (they use the struct's angles, see
   skyang_enter) but the cloud deck/ceiling planes drawn by 0x1615E8 do: with it they move with the world too.
   ACZ_VR_SKY_CAM=0 turns it off; debug: ACZ_DEBUG_SKY_YAW=<deg>, ACZ_DEBUG_SKY_KEEP=1. */
#define Z_SKY_RENDER 0x00161098u
static u32 z_skycam_addr;
static u32 z_skycam_saved[32];
static int skycam_enter(ps2_ctx *ctx, void *user) {
    (void)user;
    z_skycam_addr = 0;
    if (!z_pose.valid) return 0;
    u32 o = ctx->r[4].uw[0];
    if (o < 0x100000u || o >= 0x2000000u) return 0;
    u32 sc = ps2_r32(o + 0xBB0u);
    if (sc < 0x100000u || sc >= 0x2000000u) return 0;
    u32 c = ps2_r32(sc + 0x260u);
    if (c < 0x100000u || c >= 0x2000000u) return 0;
    {   static int shown2;                      /* debug: the sky object's own camera fields (0x162940 reads them) */
        if (getenv("ACZ_DEBUG_SKY") && ps2_kernel_vblank_count() >= 1500u && shown2++ < 3) {
            char ln[600]; int k = snprintf(ln, sizeof ln, "zero-vr: sky +1F0..+23C:");
            for (u32 q = 0; q < 20; q++) k += snprintf(ln + k, sizeof ln - (size_t)k, " %.5g", f_of(ps2_r32(o + 0x1F0u + 4u * q)));
            ps2_log("%s", ln);
            for (int i = 0; i < 4; i++)
                if (z_headcam[i].c == c)
                    ps2_log("zero-vr:   ctx %08X base eye %.3f %.3f %.3f ang %.5f %.5f %.5f; head eye %.3f %.3f %.3f ang %.5f %.5f %.5f", c,
                            f_of(ps2_r32(c + 0x30u)), f_of(ps2_r32(c + 0x34u)), f_of(ps2_r32(c + 0x38u)),
                            f_of(ps2_r32(c + 0x40u)), f_of(ps2_r32(c + 0x44u)), f_of(ps2_r32(c + 0x48u)),
                            f_of(z_headcam[i].v[0]), f_of(z_headcam[i].v[1]), f_of(z_headcam[i].v[2]),
                            f_of(z_headcam[i].v[3]), f_of(z_headcam[i].v[4]), f_of(z_headcam[i].v[5]));
        }
    }
    float P[16], Pn[16], W[16], R[3][3];
    for (int i = 0; i < 16; i++) {
        P[i] = f_of(ps2_r32(sc + 0x170u + 4u * (u32)i));
        Pn[i] = f_of(ps2_r32(sc + 0x130u + 4u * (u32)i));
        W[i] = f_of(ps2_r32(c + 0x110u + 4u * (u32)i));
    }
    if (fabsf(P[0]) < 1e-3f || fabsf(P[5]) < 1e-3f || fabsf(P[11]) < 1e-6f) return 0;
    for (int i = 0; i < 3; i++) {             /* row i of W = R[i][0] P0 + R[i][1] P1 + R[i][2] P2 (+ translation row) */
        float r2 = W[i * 4 + 3] / P[11];
        float r0 = (W[i * 4] - P[8] * r2) / P[0], r1 = (W[i * 4 + 1] - P[9] * r2) / P[5];
        float l = sqrtf(r0 * r0 + r1 * r1 + r2 * r2);
        if (l < 0.9f || l > 1.1f) return 0;   /* not a rotation: leave the sky alone */
        R[i][0] = r0 / l; R[i][1] = r1 / l; R[i][2] = r2 / l;
    }
    static int dbg = -1;
    static float dyaw;
    if (dbg < 0) { dbg = getenv("ACZ_DEBUG_SKY") != NULL; const char *e = getenv("ACZ_DEBUG_SKY_YAW"); dyaw = e ? (float)atof(e) : 0.0f; }
    if (dyaw != 0.0f) {                       /* debug: turn the sky camera about its vertical axis */
        float a = dyaw * 0.01745329f, ca = cosf(a), sa = sinf(a);
        for (int i = 0; i < 3; i++) {
            float x = R[i][0], z = R[i][2];
            R[i][0] = ca * x + sa * z; R[i][2] = -sa * x + ca * z;
        }
    }
    float nb[16], nf[16];
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 4; k++) {
            nb[i * 4 + k] = i < 3 ? R[i][0] * Pn[k] + R[i][1] * Pn[4 + k] + R[i][2] * Pn[8 + k] : Pn[12 + k];
            nf[i * 4 + k] = i < 3 ? R[i][0] * P[k] + R[i][1] * P[4 + k] + R[i][2] * P[8 + k] : P[12 + k];
        }
    for (int i = 0; i < 32; i++) z_skycam_saved[i] = ps2_r32(sc + 0xB0u + 4u * (u32)i);
    if (dbg) {
        static int shown;
        if (ps2_kernel_vblank_count() >= 1500u && shown++ < 4) {
            float db = 0.0f, df = 0.0f;
            for (int i = 0; i < 16; i++) {
                float a = fabsf(nb[i] - f_of(z_skycam_saved[i])), b = fabsf(nf[i] - f_of(z_skycam_saved[16 + i]));
                if (i < 12 && a > db) db = a;
                if (i < 12 && b > df) df = b;
            }
            ps2_log("zero-vr: sky camera rebuilt (largest change: rotation x projection %.4g, x screen %.4g)", db, df);
        }
    }
    for (int i = 0; i < 16; i++) {
        union { float f; u32 u; } a, b;
        a.f = nb[i]; b.f = nf[i];
        ps2_w32(sc + 0xB0u + 4u * (u32)i, a.u);
        ps2_w32(sc + 0xF0u + 4u * (u32)i, b.u);
    }
    z_skycam_addr = sc;
    return 0;
}
static int skycam_leave(ps2_ctx *ctx, void *user) {
    (void)ctx; (void)user;
    static int keep = -1;
    if (keep < 0) keep = getenv("ACZ_DEBUG_SKY_KEEP") != NULL;
    if (z_skycam_addr && !keep) for (int i = 0; i < 32; i++) ps2_w32(z_skycam_addr + 0xB0u + 4u * (u32)i, z_skycam_saved[i]);
    z_skycam_addr = 0;
    return 0;
}

/* The sky gradient (dome, 0x161318) and the horizon haze ring (0x1615E8) are computed at the start of sky_Render
   (0x161e80 -> 0x162940 / 0x162b88, screen vertices and colours into the scratchpad) from the camera angles of the sky
   camera struct (sky +0xBB0: +0x20 pitch, +0x24 yaw, +0x28 roll; sky_Render copies them to sky +0x200..), which
   0x15D668 fills from the view context after the update, i.e. without the head. In world-locked (cockpit) frames
   both followed the aircraft instead of the world (a 12 deg head pitch moved the terrain 33 px and them 0). For the
   duration of sky_Render the struct gets the head-turned angles of its view context (struct +0x260, values kept by
   headcam_note) and its own afterwards. ACZ_VR_SKY_ANGLES=0 turns it off. */
static u32 z_skyang_obj, z_skyang_sky, z_skyang_saved[3];
static int skyang_enter(ps2_ctx *ctx, void *user) {
    (void)user;
    z_skyang_obj = 0;
    if (!z_pose.valid) return 0;
    u32 o = ctx->r[4].uw[0];
    if (o < 0x100000u || o >= 0x2000000u) return 0;
    u32 sc = ps2_r32(o + 0xBB0u);
    if (sc < 0x100000u || sc >= 0x2000000u) return 0;
    u32 c = ps2_r32(sc + 0x260u);
    u64 f = ps2_kernel_vblank_count();
    for (int i = 0; i < 4; i++)
        if (c && z_headcam[i].c == c && f - z_headcam[i].field <= 2u) {
            /* only when the sky's copy is this context's own (base) camera: same angles as the restored context */
            int same = 1;
            for (int j = 0; j < 3; j++) {
                float a = f_of(ps2_r32(sc + 0x20u + 4u * (u32)j)), b = f_of(ps2_r32(c + 0x40u + 4u * (u32)j));
                if (fabsf(a - b) > 1e-3f) same = 0;
            }
            if (!same) return 0;
            for (int j = 0; j < 3; j++) {
                z_skyang_saved[j] = ps2_r32(sc + 0x20u + 4u * (u32)j);
                ps2_w32(sc + 0x20u + 4u * (u32)j, z_headcam[i].v[3 + j]);
            }
            {   const char *dy = getenv("ACZ_DEBUG_SKY_ADDYAW");    /* debug: exaggerate */
                if (dy) {
                    union { float f; u32 u; } y; y.u = ps2_r32(sc + 0x24u); y.f += (float)atof(dy);
                    ps2_w32(sc + 0x24u, y.u);
                }
            }
            z_skyang_obj = sc;
            z_skyang_sky = o;
            {   static int shown;
                if (getenv("ACZ_DEBUG_SKY") && f >= 1500u && shown++ < 3)
                    ps2_log("zero-vr: sky angles %.5f %.5f %.5f -> %.5f %.5f %.5f (field %llu)", f_of(z_skyang_saved[0]),
                            f_of(z_skyang_saved[1]), f_of(z_skyang_saved[2]), f_of(z_headcam[i].v[3]),
                            f_of(z_headcam[i].v[4]), f_of(z_headcam[i].v[5]), (unsigned long long)f);
            }
            break;
        }
    {   static int shown;
        if (!z_skyang_obj && getenv("ACZ_DEBUG_SKY") && f >= 1500u && shown++ < 3)
            ps2_log("zero-vr: sky angles not changed: obj %08X cam %08X ctx %08X", o, sc, c);
    }
    return 0;
}
static int skyang_leave(ps2_ctx *ctx, void *user) {
    (void)ctx; (void)user;
    if (z_skyang_obj)
        for (int j = 0; j < 3; j++) {               /* the struct, and sky_Render's copy at sky +0x200 */
            ps2_w32(z_skyang_obj + 0x20u + 4u * (u32)j, z_skyang_saved[j]);
            if (z_skyang_sky) ps2_w32(z_skyang_sky + 0x200u + 4u * (u32)j, z_skyang_saved[j]);
        }
    z_skyang_sky = 0;
    z_skyang_obj = 0;
    return 0;
}

/* The effects camera block (0x17E8A0, view context in a1; copied to the scratchpad for the effect programs) takes its
   world->screen matrix from the context (+0x110, head-turned) but the eye and angles the update hook gave back
   (+0x30/+0x40 -> block +0xD0/+0xE0, without the head). From those angles the game builds the billboard rotations of
   its effects and the snow/rain particle box (0x19D360: a 4096-unit box centred 2453 units ahead along the block's
   angles), so in world-locked (cockpit) frames the box stayed ahead of the nose: looking to the side showed its edge
   and the storm seemed to stay where the aircraft points. For the duration of 0x17E8A0 the context gets the
   head-turned angles of its latest update (headcam_note); angles only, the block's eye also feeds the streak velocity.
   Head-locked frames (stick look outside the cockpit) have no head in the camera: nothing changes there.
   ACZ_VR_FX_ANGLES=0 turns it off.
   Effect sprite size (ov_visual 2026-10-25, the audit bug #25 left open): 0x17E8A0 also reads the screen distance
   ctx+8 twice -- at 0x17ECA8 for the projection 0x1069A8 builds from ctx+0..+0x10 and at 0x17EECC for block+0x6000 =
   d, +0x6004 / +0x6008 = 1 / d terms the effect programs size their sprites with -- after the update hook gave the
   game its own d back (571.26) while the block's world->screen matrix (+0x110) was built with the eye's (232.5 on
   the simulator): smoke, explosions, fire, missile smoke and glows were d_game / d_eye = 2.46x too large against the
   world in head-tracked frames, as the trees were (#25); near a burning target a single smoke puff filled the view
   (the user's M4 recording 10-23 21:27:12 / 21:27:19: the whole eye grey with a bright disc for ~0.3 s over the tank
   farm). ACZ_VR_FX_SIZE (default 1): for the duration of 0x17E8A0 the context also gets the d and virtH of its
   head-turned update (headcam_note), the values its +0x110 matrix was built with; 0 = the game's d (2.46x sprites).
   ACZ_VR_FX_SIZE_AB=N [test]: on in even, off in odd blocks of N fields (game frames) (with PS2_XR_DUMP_PAIR=2 a dump and the
   next game frame differ in it). */
#define Z_FX_CAMERA 0x0017E8A0u
static u32 z_fx_c, z_fx_saved[3], z_fx_dsaved[2];
static int z_fx_angles = 1, z_fx_size = 1, z_fx_size_ab, z_fx_sized;
static int fx_enter(ps2_ctx *ctx, void *user) {
    (void)user;
    z_fx_c = 0; z_fx_sized = 0;
    if (!z_pose.valid) return 0;
    u32 c = ctx->r[5].uw[0];
    u64 f = ps2_kernel_vblank_count();
    for (int i = 0; i < 4; i++)
        if (c && z_headcam[i].c == c && f - z_headcam[i].field <= 2u) {
            for (int j = 0; j < 3; j++) {
                z_fx_saved[j] = ps2_r32(c + 0x40u + 4u * (u32)j);
                if (z_fx_angles) ps2_w32(c + 0x40u + 4u * (u32)j, z_headcam[i].v[3 + j]);
            }
            {   int on = z_fx_size;
                if (on && z_fx_size_ab > 0) on = !((f / (u64)z_fx_size_ab) & 1u);   /* blocks of N fields (a game frame each) */
                float dh = f_of(z_headcam[i].v[6]);
                if (on && dh > 1.0f && dh < 100000.0f) {
                    z_fx_dsaved[0] = ps2_r32(c + 8u); z_fx_dsaved[1] = ps2_r32(c + 4u);
                    ps2_w32(c + 8u, z_headcam[i].v[6]); ps2_w32(c + 4u, z_headcam[i].v[7]);
                    z_fx_sized = 1;
                    static int said;
                    if (!said++) ps2_log("zero-vr: effect sprites sized for the head-tracked projection: d %.2f -> %.2f "
                                         "during the effects camera block (ACZ_VR_FX_SIZE=0: the game's d)",
                                         (double)f_of(z_fx_dsaved[0]), (double)dh);
                }
                static u64 ab_last = ~0ull;
                if (z_fx_size_ab > 0 && f / (u64)z_fx_size_ab != ab_last) {
                    ab_last = f / (u64)z_fx_size_ab;
                    static int nlog;
                    if (nlog++ < 40 || z_fx_size_ab == 1)      /* (N = 1: every frame, by game frame for the dump pairs) */
                        ps2_log("fx-size A/B: field %llu game frame %u effect sprites %s", (unsigned long long)f, z_pose.seq,
                                on ? "sized (new)" : "the game's d (old)");
                }
            }
            z_fx_c = c;
            {   static int shown;
                if (getenv("ACZ_DEBUG_SKY") && f >= 1100u && shown++ < 3)
                    ps2_log("zero-vr: effects camera %08X angles %.5f %.5f %.5f -> %.5f %.5f %.5f (field %llu)", c,
                            f_of(z_fx_saved[0]), f_of(z_fx_saved[1]), f_of(z_fx_saved[2]), f_of(z_headcam[i].v[3]),
                            f_of(z_headcam[i].v[4]), f_of(z_headcam[i].v[5]), (unsigned long long)f);
            }
            break;
        }
    return 0;
}
static int fx_leave(ps2_ctx *ctx, void *user) {
    (void)ctx; (void)user;
    if (z_fx_c) {
        if (z_fx_angles) for (int j = 0; j < 3; j++) ps2_w32(z_fx_c + 0x40u + 4u * (u32)j, z_fx_saved[j]);
        if (z_fx_sized) { ps2_w32(z_fx_c + 8u, z_fx_dsaved[0]); ps2_w32(z_fx_c + 4u, z_fx_dsaved[1]); }
    }
    z_fx_c = 0; z_fx_sized = 0;
    return 0;
}

/* debug: ACZ_DEBUG_KILL=addr[,addr...] replaces those functions by "return a1" (packet builders hand back the packet
   pointer), to find which function draws something */
static int kill_fn(ps2_ctx *ctx, void *user) {
    (void)user;
    ctx->r[2].uw[0] = ctx->r[5].uw[0];
    ctx->r[2].uw[1] = 0;
    return 1;                                   /* handled: the original is skipped */
}

static int cloud_leave(ps2_ctx *ctx, void *user) {
    (void)ctx; (void)user;
    if (z_cloud_n <= 0) return 0;
    int lvl = --z_cloud_n;
    if (z_cb.active && z_cb.lvl == lvl) z_cb.active = 0;
    if (z_cloud_c[lvl]) for (int j = 0; j < 6; j++) ps2_w32(z_cloud_c[lvl] + z_headcam_off[j], z_cloud_saved[lvl][j]);
    z_cloud_c[lvl] = 0;
    if (z_cloud_wh_c[lvl]) ps2_w32(z_cloud_wh_c[lvl] + 0x18u, z_cloud_wh_saved[lvl]);
    z_cloud_wh_c[lvl] = 0;
    if (z_cloud_win_set[lvl]) ps2_w32(Z_CLOUD_WIN, z_cloud_win_saved[lvl]);
    z_cloud_win_set[lvl] = 0;
    return 0;
}

/* Tree size in VR (bug #25; ACZ_VR_TREE_SIZE=0: the game's). The tree block builder 0x1771E0 updates the camera
   (0x1772EC, head-injected: d and virtH set so the projection covers the eye's tangents) and then reads the screen
   distance ctx+8 for the sprite scale (VU 0x18.z: half size and centre offset = per-type size x d / w) -- after the
   update hook gave the game its own d back. The sprites were therefore drawn with the game's d (571) on a projection
   built with the eye's (232 on the simulator, tangent 1.376): every tree 2.46x too large against the terrain under it
   (a 27 m tree became ~66 m, and the fade-in at the max distance showed 6x the area). During 0x1771E0 the context
   keeps the d its projection was built with; it is given back when 0x1771E0 returns. The max distance (0x170C28)
   still sees the game's d; 0x1771E0 also scales the fade start table[1] (a3 + 4) by zoom = (d + 7994) / 8565, so
   table[1] is scaled by zoom(game d) / zoom(projection d) for the duration (fade start 10240 stays 10240). */
static int z_tree_size = 1, z_tree_in;
static u32 z_tree_c, z_tree_d, z_tree_tab, z_tree_fade;
static int tree_block_enter(ps2_ctx *ctx, void *user) {
    (void)user;
    z_tree_in = 1;
    z_tree_tab = ctx->r[7].uw[0];
    if (z_tree_tab < 0x100000u || z_tree_tab >= 0x2000000u) z_tree_tab = 0;
    return 0;
}
static int tree_block_leave(ps2_ctx *ctx, void *user) {
    (void)ctx; (void)user;
    z_tree_in = 0;
    if (z_tree_c) {
        ps2_w32(z_tree_c + 8u, z_tree_d);
        if (z_tree_tab) ps2_w32(z_tree_tab + 4u, z_tree_fade);
        z_tree_c = 0;
    }
    return 0;
}

/* [debug] ACZ_DEBUG_CAMREAD=N (ov_visual 2026-10-25; the audit bug #25 left open): which game functions read a camera
   context's screen distance (ctx+8) after the head hook gave the game its own d back -- a sprite size taken from it
   is d_game / d_eye too large in VR (the trees were 2.46x). The pages of the 640x480 contexts the update hook sees
   are unmapped from ps2_pt (ps2_hw.c trap pages: every access is served from RAM and reported); reads of ctx+8 are
   counted per (newest trace-ring function, context, value) and logged every N fields. Diagnostic only (slow). */
extern u8 *ps2_pt[];
extern u32 ps2_trap_pages[4];
extern void (*ps2_trap_cb)(u32 a, int write, u32 v);
static int z_camread = -1;
static u32 cr_ctx[4]; static int cr_n;
static ps2_ctx *cr_ee;                 /* the game thread's registers: r31 at the read = inside the reader (its last call's return) */
static struct { u32 fn, fn2, fn3, ctx, val, n; } cr_tab[128]; static int cr_tn;
static void camread_cb(u32 a, int w, u32 v) {
    if (w || !ps2_kernel_on_ee_thread()) return;   /* the game's own reads (not the renderer's / hooks on other threads) */
    for (int i = 0; i < cr_n; i++) {
        if (a != cr_ctx[i] + 8u) continue;
        u32 fn = cr_ee ? cr_ee->r[31].uw[0] : 0u;
        u32 fn2 = ps2_trace_ring[(ps2_trace_pos - 2u) & (PS2_TRACE_RING - 1u)];
        u32 fn3 = ps2_trace_ring[(ps2_trace_pos - 3u) & (PS2_TRACE_RING - 1u)];
        int k;
        for (k = 0; k < cr_tn; k++)
            if (cr_tab[k].fn == fn && cr_tab[k].fn2 == fn2 && cr_tab[k].ctx == cr_ctx[i] && cr_tab[k].val == v) break;
        if (k == cr_tn) {
            if (cr_tn >= 128) return;
            cr_tab[k].fn = fn; cr_tab[k].fn2 = fn2; cr_tab[k].fn3 = fn3; cr_tab[k].ctx = cr_ctx[i]; cr_tab[k].val = v;
            cr_tab[k].n = 0; cr_tn++;
        }
        cr_tab[k].n++;
    }
}
static void camread_arm(u32 c, int head) {
    if (z_camread < 0) { const char *e = getenv("ACZ_DEBUG_CAMREAD"); z_camread = e ? atoi(e) : 0; }
    if (z_camread <= 0) return;
    {   static u64 last;
        u64 f = ps2_kernel_vblank_count();
        if (f / (u64)z_camread != last / (u64)z_camread && cr_tn) {
            for (int k = 0; k < cr_tn; k++)
                ps2_log("camread: field %llu ctx %08X d %.2f read with ra %08X (functions entered last %08X %08X) x%u",
                        (unsigned long long)f, cr_tab[k].ctx, (double)f_of(cr_tab[k].val), cr_tab[k].fn, cr_tab[k].fn2,
                        cr_tab[k].fn3, cr_tab[k].n);
            cr_tn = 0;
        }
        last = f;
    }
    if (!head || c >= 0x01E00000u) return;          /* only the contexts whose d the head hook replaced */
    for (int i = 0; i < cr_n; i++) if (cr_ctx[i] == c) return;
    if (cr_n >= 4) return;
    u32 pg = (c + 8u) >> 12;
    int slot = -1;
    for (int i = 0; i < 4; i++) if (ps2_trap_pages[i] == pg) slot = i;
    cr_ctx[cr_n++] = c;
    if (slot < 0) {
        for (int i = 0; i < 4; i++) if (ps2_trap_pages[i] == 0xFFFFFFFFu) { slot = i; break; }
        if (slot < 0) return;
        ps2_trap_cb = camread_cb;
        ps2_trap_pages[slot] = pg;
        ps2_pt[pg] = NULL;
    }
    ps2_log("camread: watching ctx %08X (+8 = page %05X, trap slot %d)", c, pg, slot);
}

static int ctx_update_leave(ps2_ctx *ctx, void *user) {
    (void)user;
    if (z_dump_field) {
        u64 f = ps2_kernel_vblank_count();
        if (f >= z_dump_field && f < z_dump_field + 3u && z_depth > 0 && z_depth <= Z_DEPTH)
            dump_ctx(z_ctx[z_depth - 1], z_dump_ra[z_depth - 1]);
        (void)ctx;
    }
    z_depth--;
    if (z_depth >= 0 && z_depth < Z_DEPTH) {
        u32 c = z_ctx[z_depth];
        if (z_saved_cam[z_depth][6]) headcam_note(c);   /* before the game's own values come back */
        u32 dnow = ps2_r32(c + 8u);
        ps2_w32(c + 8u, z_saved[z_depth]);
        cr_ee = ctx;
        camread_arm(c, dnow != z_saved[z_depth]);
        if (z_saved_cam[z_depth][6]) {
            /* Synchronous stereo: the screen projection just built (+0x90, row vectors) maps view x to screen x with
               m00 and view depth to GS Z as Z = -C + K/|z|. An eye offset h along camera x moves a vertex by
               -m00*h/|z| = -(m00*h/K)*(Z + C) pixels. */
            /* The scene renderer updates the camera it is about to draw with at 0x1D7230 (the flight camera in
               flight, the crash/replay camera after a crash); its projection is the one the frame's vertices use.
               Earlier updates of the frame only provide a fallback. */
            static u32 shift_seq_done, player_ctx, player_seq;
            u32 ra = z_dump_ra[z_depth];
            int render_cam = ra == 0x001D7230u;
            /* the player's camera this frame: the context the camera logic (0x15D6E8 / 0x170D90 / 0x1772EC) updates */
            /* (the renderer's update at 0x1D7230 can come before the logic updates of the same frame, so the latest
               player context from any frame is used) */
            if (ra == 0x0015D6E8u || ra == 0x00170D90u || ra == 0x001772ECu) { player_ctx = c; player_seq = z_pose.seq; }
            (void)player_seq;
            if (z_pose.valid && (z_pose.seq != shift_seq_done || render_cam)) {
                float m00 = f_of(ps2_r32(c + 0x90u)), C = f_of(ps2_r32(c + 0x90u + 40u)), K = f_of(ps2_r32(c + 0x90u + 56u));
                float h = 0.5f * z_pose.ipd * z_units_per_m * z_stereo_scale;
                /* parallax only when the frame is drawn with the player's own camera; crash and replay cameras
                   (drawn through the same renderer site) get none -- their near-ground views tore apart */
                if (render_cam && player_ctx && player_ctx != c) {
                    h = 0.0f;
                    static u32 said_ctx;
                    if (said_ctx != c) { said_ctx = c; ps2_log("zero-vr: frames drawn with camera %08X (player camera %08X): no parallax", c, player_ctx); }
                }
                if (K > 1.0f && m00 > 1.0f) ps2_xr_set_shift(z_pose.seq, m00 * h / K, C, m00 * h);
                shift_seq_done = z_pose.seq;
                {   /* the camera the frame is drawn with (head applied), for frames reprojected between game frames */
                    float eye[3], ang[3], R[3][3];
                    for (u32 i = 0; i < 3; i++) {
                        eye[i] = f_of(ps2_r32(c + 0x30u + 4u * i));
                        ang[i] = f_of(ps2_r32(c + 0x40u + 4u * i));
                    }
                    m3_euler(ang[0], ang[1], ang[2], R);
                    float m11 = f_of(ps2_r32(c + 0x90u + 20u));
                    float cx = -f_of(ps2_r32(c + 0x90u + 32u)), cy = -f_of(ps2_r32(c + 0x90u + 36u));
                    if (m00 > 1.0f && m11 < -1.0f) ps2_xr_set_camera(z_pose.seq, eye, &R[0][0], m00, -m11, cx, cy);
                    /* and without the head (the game's own eye and angles, given back below): frames between game
                       frames extrapolate this camera and keep the frame's head pose */
                    for (u32 i = 0; i < 3; i++) {
                        eye[i] = f_of(z_saved_cam[z_depth][i]);
                        ang[i] = f_of(z_saved_cam[z_depth][3 + i]);
                    }
                    m3_euler(ang[0], ang[1], ang[2], R);
                    if (m00 > 1.0f && m11 < -1.0f) ps2_xr_set_camera_base(z_pose.seq, eye, &R[0][0]);
                }
            }
            for (u32 i = 0; i < 3; i++) {
                ps2_w32(c + 0x30u + 4u * i, z_saved_cam[z_depth][i]);
                ps2_w32(c + 0x40u + 4u * i, z_saved_cam[z_depth][3 + i]);
            }
            ps2_w32(c + 4u, z_saved_cam[z_depth][7]);
        }
        if (z_tree_in && z_tree_size && !z_tree_c && z_dump_ra[z_depth] == 0x001772ECu && dnow != z_saved[z_depth]) {
            z_tree_c = c; z_tree_d = z_saved[z_depth];          /* the tree sprites: the projection's d (above) */
            ps2_w32(c + 8u, dnow);
            if (z_tree_tab) {
                float dg = f_of(z_tree_d), dv = f_of(dnow);
                z_tree_fade = ps2_r32(z_tree_tab + 4u);
                if (dg > 1.0f && dv > 1.0f && dg < 100000.0f && dv < 100000.0f)
                    ps2_w32(z_tree_tab + 4u, w_of(f_of(z_tree_fade) * (dg + 7994.0f) / (dv + 7994.0f)));
            }
        }
    }
    if (z_depth < 0) z_depth = 0;
    return 0;
}

// The clip projection (cam+0xD0, and cam+0x150 = clip*view) is built as m00 = d/(0.504*virtW), m11 = d/(0.504*virtH)
// with 0.504 read from 0x3F7160 (only by the clip builder 0x106B00). It drives all frustum culling: terrain blocks and
// cells on the CPU and triangle rejection on VU1. Scaling the constant widens the culling frustum without changing
// the picture, so geometry exists beyond the original view when the head turns.
#define Z_CLIP_CONST 0x003F7160u
#define Z_CLIP_CONST_W 0x3F010625u   /* 0.504f */

/* ACZ_DEBUG_DISP=1: log the display manager's context (0x100F00, a0) once; its +0xC capture request was found
   overwritten when the wide head-tracked view overflowed something. */
static int disp_enter(ps2_ctx *ctx, void *user) {
    static int said;
    (void)user;
    if (!said) { said = 1; ps2_log("zero-vr: display manager context %08X", ctx->r[4].uw[0]); }
    return 0;
}

static int ph_begin(ps2_ctx *ctx, void *user) {
    static u32 seen[8]; static int n;
    (void)user;
    u32 ph = ctx->r[4].uw[0];
    for (int i = 0; i < n; i++) if (seen[i] == ph) return 0;
    if (n < 8) seen[n++] = ph;
    ps2_log("zero-vr: packet heap %08X: base A %08X base B %08X cur %08X size %08X idx %d",
            ph, ps2_r32(ph), ps2_r32(ph + 4u), ps2_r32(ph + 8u), ps2_r32(ph + 12u), (int)(s8)ps2_r8(ph + 0x14u));
    return 0;
}

/* Frame packet heap relocation. The renderer's per-frame DMA packet heap (object at 0x0044EEB0 in mission 1: two
   1 MB buffers at 0x0044EED0 and 0x0054EED0, fields base A, base B, current, size, index at +0x14) has no bounds
   checks. With the headset's field of view, looking at terrain fills more than 1 MB per frame and the second buffer
   runs into the display manager's context at 0x0064EED0 (found with the store watch: a 128-bit packet store from
   0x15E0D0/0x15CAA0 -> 0x102CA0), which then corrupts the game. At the first use of the heap both buffers move to
   fresh host memory mapped at guest 0x02000000 (above the PS2's 32 MB, reachable by the CPU and by DMA through the
   page table, and mirrored at the uncached/kernel aliases), ACZ_PACKET_MB (default 8) MB each. */
#define Z_PH_BEGIN   0x00102798u
#define Z_PH_NEWBASE 0x02000000u
static int ph_done;
static int ph_relocate(ps2_ctx *ctx, void *user) {
    (void)user;
    if (ph_done) return 0;
    u32 ph = ctx->r[4].uw[0];
    u32 a = ps2_r32(ph), b = ps2_r32(ph + 4u), cur = ps2_r32(ph + 8u), size = ps2_r32(ph + 12u);
    int idx = (int)(s8)ps2_r8(ph + 0x14u);
    if (size != 0x100000u || b != a + size || (idx != 0 && idx != 1)) {
        ps2_log("zero-vr: packet heap %08X has an unexpected layout (A %08X B %08X size %08X idx %d); left alone",
                ph, a, b, size, idx);
        ph_done = 1;
        return 0;
    }
    if (cur != (idx ? b : a)) return 0;                 /* mid-frame: wait for a frame start */
    u32 mb = (u32)env_float("ACZ_PACKET_MB", 8.0f);
    if (mb < 2u) mb = 2u;
    if (mb > 32u) mb = 32u;
    u32 nsz = mb << 20;
    u8 *host = (u8 *)calloc(1, (size_t)nsz * 2u + 16u);
    if (!host) { ph_done = 1; return 0; }
    static const u32 alias[] = { 0x00000000u, 0x20000000u, 0x30000000u, 0x80000000u, 0xA0000000u };
    for (unsigned i = 0; i < sizeof alias / sizeof alias[0]; i++)
        ps2_mem_map(alias[i] + Z_PH_NEWBASE, nsz * 2u, host);
    ps2_w32(ph, Z_PH_NEWBASE);
    ps2_w32(ph + 4u, Z_PH_NEWBASE + nsz);
    ps2_w32(ph + 12u, nsz);
    ps2_w32(ph + 8u, idx ? Z_PH_NEWBASE + nsz : Z_PH_NEWBASE);
    ph_done = 1;
    ps2_log("zero-vr: frame packet heap %08X moved from %08X/%08X (1 MB each) to %08X/%08X (%u MB each)",
            ph, a, b, Z_PH_NEWBASE, Z_PH_NEWBASE + nsz, mb);
    return 0;
}

/* ACZ_DEBUG_MC=1: trace the save manager to find where the boot-time "memory card files are corrupted" status 6
   comes from (REVIEW_2026-10-19.md ux-1): result dispatcher 0x17AAB8, op poller 0x17B658, status-to-message switch
   0x2B8FF8 (6 = memorycard_Broken), card insert/remove events 0x17B498 / 0x17B4E8. */
static int mc_dbg(ps2_ctx *ctx, void *user) {
    u32 a0 = ctx->r[4].uw[0], a1 = ctx->r[5].uw[0], a2 = ctx->r[6].uw[0];
    extern u32 ps2_gs_frame_count;
    switch ((u32)(uintptr_t)user) {
    case 0: ps2_log("mcdbg %u: dispatch 17AAB8 a0=%08X cmd(+64)=%08X a1=%08X a2(result)=%08X | card state 42B340: "
                    "%08X %08X %08X %08X %08X %08X %08X %08X", ps2_gs_frame_count,
                    a0, a0 ? ps2_r32(a0 + 0x64u) : 0u, a1, a2,
                    ps2_r32(0x42B340u), ps2_r32(0x42B344u), ps2_r32(0x42B348u), ps2_r32(0x42B34Cu),
                    ps2_r32(0x42B350u), ps2_r32(0x42B354u), ps2_r32(0x42B358u), ps2_r32(0x42B35Cu)); break;
    case 1: ps2_log("mcdbg %u: poller 17B658 a0=%08X state(+1C)=%08X a1=%08X a2=%08X", ps2_gs_frame_count,
                    a0, a0 ? ps2_r32(a0 + 0x1Cu) : 0u, a1, a2); break;
    case 2: ps2_log("mcdbg %u: poller 17B658 returns v0=%08X", ps2_gs_frame_count, ctx->r[2].uw[0]); break;
    case 3: ps2_log("mcdbg %u: status message 2B8FF8 a1(status)=%u", ps2_gs_frame_count, a1); break;
    case 4: ps2_log("mcdbg %u: card event 17B498 (insert?) a0=%08X a1=%08X", ps2_gs_frame_count, a0, a1); break;
    case 5: ps2_log("mcdbg %u: card event 17B4E8 (remove?) a0=%08X a1=%08X", ps2_gs_frame_count, a0, a1); break;
    case 6: ps2_log("mcdbg %u: free-space op 2F57A8 a0=%08X a1=%08X a2=%08X", ps2_gs_frame_count, a0, a1, a2); break;
    case 7: ps2_log("mcdbg %u: free-space op 2F57A8 returns %08X", ps2_gs_frame_count, ctx->r[2].uw[0]); break;
    }
    return 0;
}
static void mc_debug_install(void) {
    if (!getenv("ACZ_DEBUG_MC")) return;
    static const struct { u32 addr; int after; } h[] = {
        { 0x0017AAB8u, 0 }, { 0x0017B658u, 0 }, { 0x0017B658u, 1 }, { 0u, 0 }, { 0x0017B498u, 0 },
        { 0x0017B4E8u, 0 }, { 0x002F57A8u, 0 }, { 0x002F57A8u, 1 } };
    for (u32 i = 0; i < sizeof h / sizeof h[0]; i++) {
        if (!h[i].addr) continue;            /* 0x2B8FF8 is a label inside func_002B9498, not hookable */
        int r = h[i].after ? ps2_hook_after(h[i].addr, mc_dbg, (void *)(uintptr_t)i, 60, "zero-mc-debug")
                           : ps2_hook_before(h[i].addr, mc_dbg, (void *)(uintptr_t)i, 60, "zero-mc-debug");
        ps2_log("mcdbg: hook %08X %s -> %d", h[i].addr, h[i].after ? "after" : "before", r);
    }
}

/* Save manager completion queue (RESEARCH_2026-10-19.md, memcard): the HLE finishes mcserv RPCs inside
   sceSifCallRpc, so the priority-3 Namco card driver thread runs the completion callback 0x179FC0 while the main
   thread is still inside the save manager's dispatcher (0x17AAB8 called from 0x17A640). The callback then re-enters
   the manager (0x17A5C8) and results shift by one command, which ends in status 6 = "memory card files are
   corrupted". On hardware the request completes later. Here the callback only queues {obj, id, result} and returns 0;
   the manager's own step 0x17A5C8 takes the next result whenever its slot (obj+0x88) is free, on the main thread.
   ACZ_MC_FIFO=0 turns it off. */
#define MC_FIFO_N 16
static struct { u32 obj, id, res; } mc_fifo[MC_FIFO_N];
static int mc_fifo_n;
static pthread_mutex_t mc_fifo_lock = PTHREAD_MUTEX_INITIALIZER;
static int mc_cb_queue(ps2_ctx *ctx, void *user) {
    (void)user;
    u32 id = ctx->r[4].uw[0], res = ctx->r[5].uw[0], obj = ctx->r[6].uw[0];
    pthread_mutex_lock(&mc_fifo_lock);
    if (mc_fifo_n < MC_FIFO_N) {
        mc_fifo[mc_fifo_n].obj = obj; mc_fifo[mc_fifo_n].id = id; mc_fifo[mc_fifo_n].res = res;
        mc_fifo_n++;
    } else ps2_log("zero-mc: completion queue full, result %08X dropped", res);
    pthread_mutex_unlock(&mc_fifo_lock);
    if (getenv("ACZ_DEBUG_MC")) ps2_log("zero-mc: queued completion id %08X result %08X obj %08X", id, res, obj);
    ctx->r[2].ud[0] = 0;        /* 0 = handled; a nonzero return on an error makes the driver re-run the request */
    return 1;                   /* skip the original callback */
}
static int mc_step_take(ps2_ctx *ctx, void *user) {
    (void)user;
    u32 obj = ctx->r[4].uw[0];
    if (!obj || ps2_r32(obj + 0x88u) != 0u) return 0;
    pthread_mutex_lock(&mc_fifo_lock);
    for (int i = 0; i < mc_fifo_n; i++) {
        if (mc_fifo[i].obj != obj) continue;
        ps2_w32(obj + 0x8Cu, mc_fifo[i].id);       /* exactly what 0x179FC0 stores */
        ps2_w32(obj + 0x90u, mc_fifo[i].res);
        ps2_w32(obj + 0x94u, obj);
        ps2_w32(obj + 0x88u, obj + 0x8Cu);
        for (int k = i + 1; k < mc_fifo_n; k++) mc_fifo[k - 1] = mc_fifo[k];
        mc_fifo_n--;
        break;
    }
    pthread_mutex_unlock(&mc_fifo_lock);
    return 0;
}
static int mc_init_drop(ps2_ctx *ctx, void *user) {
    (void)user;
    u32 obj = ctx->r[4].uw[0];
    pthread_mutex_lock(&mc_fifo_lock);
    int k = 0;
    for (int i = 0; i < mc_fifo_n; i++) if (mc_fifo[i].obj != obj) mc_fifo[k++] = mc_fifo[i];
    mc_fifo_n = k;
    pthread_mutex_unlock(&mc_fifo_lock);
    return 0;
}
/* ---- immersion signals (research probe; ACZ_DEBUG_IMMERSE=N logs every N fields) ----
   Cloud field (negative result, kept for the record): 0x16A2F0 projects one cloud of the field 0x16A6C0 (a0 = a
   position, f12 = size, returns f0 >= 0 on screen, -1 off screen); the field skips f0 <= f23 (4096) and fades over
   8192. The positions lie on a ~73728-unit sphere about the camera (impostor directions, vf1 = 73728 x the view axis)
   and f0 is not |p - vf1| (trilateration residuals ~20000 units): no in-cloud distance here (2026-10-20 climb runs).
   [Corrected 2026-10-20 evening, ACZ_DEBUG_CLOUD_DATA: a0 is the cloud's position relative to a wrap window centred
   73728 units along the view axis (vf1); world = a0 + eye - vf1 stays fixed while the aircraft flies, |a0 - vf1| is
   the true distance (record a1+0) and f0 the view depth. The clouds are world-placed: see cloud_data_field.]
   Precipitation: the sky update 0x1604A0 (a0 = sky object) lerps the stage environment's particle amount (env +0x84,
   second set +0xA0 later) by the camera height inside the band sky +0x10..+0x14 into sky +0xB8C. */
static int z_imm_dbg = -1;
static u32 z_imm_sky;
static volatile u32 z_imm_reset_req;      /* 1 = GameControl::enter (a mission starts): the canopy starts dry; 2 = the
                                             player's aircraft restored (zero_cracks.c, bug #133: a checkpoint respawn);
                                             3 = the mission (re)started (z_mission_init: a RETRY, a RESTART) */
void zero_vr_imm_reset_request(int why) { if (!z_imm_reset_req) z_imm_reset_req = (u32)why; }
/* Bug #268 (playtest 2026-11-02 10:55-11:01, M18 X-02: "when I went from the second phase to the third phase ... my
   glass was miraculously restored"): the canopy reset (ps2_imm_reset) also fired whenever the aircraft was put
   somewhere else -- more than 300 m between two flight frames, or across a stage (re)load -- and a mission's own stage
   changes do that: M18's phase 2 -> 3 moved the aircraft 9123 m across a stage load (field 9014: 2 impacts cleared,
   the water, soot, chips and ice with them). The 2026-10-29 M18 session shows it is not a restart: the game keeps
   the player's damage over its phase changes (HP 52 of 117 before the stage load at field 51048 and 52 at the next
   hit, 20 -> 20 at 115314, 29 -> 29 at 182697), while the reset cleared 3, 10 and 16 impacts and took the HP scale
   down to the damaged HP (later hits cracked as shares of 52, 20, 29). The same jump rule fired in M12 after the nukes
   (634 .. 340 m in five fields: 4 impacts cleared at HP 40 of 104) and at the stage changes of M13, M15 and M17.
   What a restart is, by the game's own code: the mission (re)start 0x1D1F00 (a0 = the game object), run by the
   mission start (0x28A970 / 0x294258, the two game modes' load steps; returns to 0x28A9BC / 0x2942A4), by the pause
   menu's RETRY (0x1D8A70 case 3, a tail call; returns to 0x1D3CEC) and by the menus after the mission's end (0x1D3540:
   MISSION FAILED's RESTART returns to 0x1D4280; the other call, after 0x2C2D58 = the checkpoint's data, to
   0x1D43FC); a stage change within the mission does not run it (sim M17: none at the take-off sequence's stage load,
   34.8 km; one each at the start, a pause RETRY and MISSION FAILED > RESTART). ACZ_IMM_RESET_KEEP (on; 0 = before):
   the canopy is reset only by GameControl::enter (a new mission, also after the menus), by the mission (re)start
   0x1D1F00 (RETRY, RESTART) and by the aircraft's restore (zero_cracks.c ACZ_IMM_RESET_RESPAWN: the HP rising after
   damage -- a checkpoint respawn, or a repair if the game makes one: M17's take-off sequence -> flight put a damaged
   HP 50 back to 90 in the sim), each at the next flight frame; a jump with none of them behind it keeps the canopy
   (cracks, chips, soot, water, ice, grime, flecks, the cloud feel and the HP scale) and only starts a new flight for
   what is tied to the place (ps2_imm_flight: the trails, the debris model's kills, the governor's fresh window),
   logged `the canopy kept`. */
static int z_keep = -1;
static int z_keep_on(void) {
    if (z_keep < 0) { const char *e = getenv("ACZ_IMM_RESET_KEEP"); z_keep = !(e && *e == '0'); }
    return z_keep;
}
#define Z_MISSION_INIT 0x001D1F00u
void zero_env_reload_mission(int retry);
static int z_mission_init(ps2_ctx *ctx, void *user) {
    (void)user;
    u32 ra = ctx->r[31].uw[0];
    const char *why = ra == 0x0028A9BCu || ra == 0x002942A4u ? "the mission's start"
                    : ra == 0x001D3CECu ? "the pause menu's RETRY"
                    : ra == 0x001D4280u ? "MISSION FAILED's RESTART"
                    : ra == 0x001D43FCu ? "a restart from the checkpoint's data"
                    : "another caller";
    ps2_log("zero-vr: mission (re)start 0x1D1F00 at field %llu (from %08X: %s)%s",
            (unsigned long long)ps2_kernel_vblank_count(), ra, why,
            z_keep_on() ? ": the canopy starts afresh at the next flight frame (ACZ_IMM_RESET_KEEP)" : "");
    if (z_keep_on()) zero_vr_imm_reset_request(3);
    /* A new attempt is a next mission for the page's rows (ACZ_MENU_RETRY_EDGE, default on; 0 = they wait for GameControl::enter).
       Every caller is a menu's choice: the pause menu's RETRY's YES (id 3), the menu after a mission's end (+0x4490, id 1). The other
       menu after the end (+0x449C, id 1, return 0x1D43FC) was never run here: no edge, what is saved goes on waiting. */
    {   const char *e = getenv("ACZ_MENU_RETRY_EDGE");
        if (e && *e == '0') return 0;
        if (ra == 0x001D3CECu || ra == 0x001D4280u) zero_env_reload_mission(1);
        else if (ra == 0x001D43FCu) ps2_log("mission: no edge at this restart: what is saved keeps waiting");
    }
    return 0;
}
static struct { u32 pos, rec, obj; float size, near, far, cam[3], p[4]; } z_cp_in;
typedef struct { u32 n, vis, n2k, n4k, n8k; float dmin, dmin_dy, dmin_size, w, ymin, ymax, near, cam[3], gmin; } z_cloud_acc;
static z_cloud_acc z_cl_acc, z_cl_last;
static u64 z_cl_field;
static void imm_cloud_flush(u64 f) {
    if (f == z_cl_field) return;
    z_cl_last = z_cl_acc;
    memset(&z_cl_acc, 0, sizeof z_cl_acc);
    z_cl_acc.dmin = 1e30f; z_cl_acc.ymin = 1e30f; z_cl_acc.ymax = -1e30f; z_cl_acc.gmin = 1e30f;
    z_cl_field = f;
}
static int cloud_proj_enter(ps2_ctx *ctx, void *user) {
    (void)user;
    z_cp_in.pos = ctx->r[4].uw[0];
    z_cp_in.rec = ctx->r[17].uw[0] >= 0x100000u && ctx->r[17].uw[0] < 0x2000000u ? ctx->r[17].uw[0] : 0x100000u;
    z_cp_in.obj = ctx->r[16].uw[0] >= 0x100000u && ctx->r[16].uw[0] < 0x2000000u ? ctx->r[16].uw[0] : 0x100000u;
    z_cp_in.size = ctx->f[12].f;
    z_cp_in.near = ctx->f[23].f; z_cp_in.far = ctx->f[22].f;
    for (int i = 0; i < 3; i++) z_cp_in.cam[i] = ctx->vu0.vf[1].f[i];
    for (u32 i = 0; i < 4; i++) z_cp_in.p[i] = f_of(ps2_r32(z_cp_in.pos + 4u * i));
    return 0;
}
static int cloud_proj_leave(ps2_ctx *ctx, void *user) {
    (void)user;
    imm_cloud_flush(ps2_kernel_vblank_count());
    float p[3], d2 = 0.0f;
    for (int i = 0; i < 3; i++) { p[i] = z_cp_in.p[i]; d2 += (p[i] - z_cp_in.cam[i]) * (p[i] - z_cp_in.cam[i]); }
    float d = sqrtf(d2);
    z_cloud_acc *a = &z_cl_acc;
    a->n++;
    if (ctx->f[0].f >= 0.0f) a->vis++;
    if (d < 2000.0f) a->n2k++;
    if (d < 4000.0f) a->n4k++;
    if (d < 8000.0f) a->n8k++;
    if (d < a->dmin) { a->dmin = d; a->dmin_dy = p[1] - z_cp_in.cam[1]; a->dmin_size = z_cp_in.size; }
    if (p[1] < a->ymin) a->ymin = p[1];
    if (p[1] > a->ymax) a->ymax = p[1];
    {   float w = 1.0f - (d - z_cp_in.near) / 8192.0f;            /* proximity: 1 at the near limit, 0 at +8192 */
        a->w += w < 0.0f ? 0.0f : w > 1.0f ? 1.0f : w;
    }
    a->near = z_cp_in.near;
    memcpy(a->cam, z_cp_in.cam, sizeof a->cam);
    {   /* ACZ_DEBUG_IMMERSE_CLOUDS=f1,f2,...: every cloud of those fields */
        static u64 cf[8]; static int ncf = -1;
        if (ncf < 0) {
            const char *e = getenv("ACZ_DEBUG_IMMERSE_CLOUDS"); ncf = 0;
            while (e && *e && ncf < 8) { char *q; cf[ncf++] = strtoull(e, &q, 0); e = *q == ',' ? q + 1 : q; if (q == e) break; }
        }
        u64 f = ps2_kernel_vblank_count();
        for (int i = 0; i < ncf; i++)
            if (f == cf[i])
                ps2_log("immerse: cloud field %llu ra? pos %.0f %.0f %.0f %.3f size %.4g f0 %.0f vf1 %.0f %.0f %.0f near %.0f far %.0f "
                        "rec %08X %d %d %d %d obj %.0f %.0f %.0f",
                        (unsigned long long)f, p[0], p[1], p[2], z_cp_in.p[3], z_cp_in.size, ctx->f[0].f,
                        z_cp_in.cam[0], z_cp_in.cam[1], z_cp_in.cam[2], z_cp_in.near, z_cp_in.far, z_cp_in.rec,
                        (s16)ps2_r16(z_cp_in.rec), (s16)ps2_r16(z_cp_in.rec + 2u), (s16)ps2_r16(z_cp_in.rec + 4u),
                        (s16)ps2_r16(z_cp_in.rec + 6u), f_of(ps2_r32(z_cp_in.obj + 0x90u)), f_of(ps2_r32(z_cp_in.obj + 0x94u)),
                        f_of(ps2_r32(z_cp_in.obj + 0x98u)));
    }
    if (ctx->f[0].f >= 0.0f && ctx->f[0].f < a->gmin) a->gmin = ctx->f[0].f;   /* the game's own distance */
    return 0;
}
static float z_imm_ppos[3];
static int sky_update_enter(ps2_ctx *ctx, void *user) { (void)user; z_imm_sky = ctx->r[4].uw[0]; return 0; }
/* ---- ACZ_DEBUG_CFIELD_STEER=1 (test only): a crude autopilot on the left stick that flies through a cloud of the
   game's list -- read from C1 (zero_clouds.c publishes it while ACZ_VR_CLOUD_DATA is on, the default): the first
   well-covered cloud 1.3-11.5 km away within 100 degrees of the nose, chosen once the flight runs; it banks towards the
   cloud's centre and climbs or dives to its height, to check the in-cloud effects on scripted runs (the cloud feel
   reads C1 in_cloud: ps2_video.c cloud_data_in_cloud). ACZ_DEBUG_CFIELD=N logs it every N fields. */
#include "ps2_clouds.h"
#include "zero_instr.h"
static zero_instr z_instr;                 /* (tentative; defined with the instrument hook below) */
static struct { int on, tgt, dbg, ext; double c[3]; float R, key; u8 lx, ly; } z_st = { -1, -1, 0, 0, { 0, 0, 0 }, 0, 0, 128, 128 };
static int cf_steer(u8 *lx, u8 *ly) {
    if (z_st.on <= 0 || z_st.tgt < 0) return 0;
    *lx = z_st.lx; *ly = z_st.ly;
    return 1;
}
static double cf_wrap(double d, double p) {      /* to the nearest copy of the periodic list */
    d = fmod(d + 0.5 * p, p);
    if (d < 0.0) d += p;
    return d - 0.5 * p;
}
static void cf_steer_update(u32 player, u64 f) {
    if (z_st.on <= 0) return;
    static ps2_cloud_env env;
    if (!ps2_cloud_env_read(&env) || !(env.valid & CL_ENV_LIST) || !env.ncloud) return;
    double upm = (double)ps2_clouds_units_per_m();
    if (!(upm > 1.0 && upm < 20.0)) upm = 6.0;
    u32 fm = player + 0xEF0u;
    double P = env.list_period_m > 1.0f ? (double)env.list_period_m : 131072.0 / upm;
    /* the aircraft in the list's frame (cloud frame = world - drift; metres) */
    double e[3] = { f_of(ps2_r32(fm + 0x3B0u)) / upm - env.drift_m[0], f_of(ps2_r32(fm + 0x3B4u)) / upm,
                    f_of(ps2_r32(fm + 0x3B8u)) / upm - env.drift_m[1] };
    {   /* only while the camera is the aircraft (C1 re-centres on the camera: not in the intro's flyby) */
        double ox = cf_wrap(env.origin_m[0] - e[0], P), oz = cf_wrap(env.origin_m[2] - e[2], P);
        if (ox * ox + oz * oz > 2500.0 * 2500.0) return;
    }
    {   /* another environment set (the take-off's list gives way to the flight's): choose again */
        float key = (float)env.ncloud + env.base_m * 0.001f + env.top_m * 0.0001f;
        if (key != z_st.key) { z_st.key = key; z_st.tgt = -1; z_st.ext = 0; }
    }
    if (z_st.tgt >= 0) {                                         /* a list of another stage or a cloud long gone: again */
        double dx = cf_wrap(z_st.c[0] - e[0], P), dz = cf_wrap(z_st.c[2] - e[2], P);
        if (dx * dx + dz * dz > 15000.0 * 15000.0) z_st.tgt = -1;
    }
    if (z_st.tgt < 0) {
        if (!z_instr_field || f - z_instr_field >= 4u) return;
        float h = z_instr.heading_deg * 0.0174533f, fx = sinf(h), fz = -cosf(h);
        double best = 1e30;
        for (u32 i = 0; i < env.ncloud; i++) {                  /* the nearest one in the cone */
            if (env.cloud[i].opacity < 0.8f) continue;
            double c[3] = { env.cloud[i].pos_m[0] + env.origin_m[0], env.cloud[i].pos_m[1], env.cloud[i].pos_m[2] + env.origin_m[2] };
            double dx = cf_wrap(c[0] - e[0], P), dz = cf_wrap(c[2] - e[2], P), dy = c[1] - e[1];
            double d = sqrt(dx * dx + dy * dy + dz * dz);
            if (d < 1300.0 || d > 11500.0 || d >= best || (dx * fx + dz * fz) / sqrt(dx * dx + dz * dz + 1.0) <= -0.17) continue;
            best = d; z_st.tgt = (int)i; memcpy(z_st.c, c, sizeof c); z_st.R = env.cloud[i].radius_m;
        }
        if (z_st.tgt < 0) return;
        ps2_log("cfield: steer target cloud %d at %.0f %.0f %.0f m (cloud frame; %.0f m away) R %.0f m opacity %.2f",
                z_st.tgt, z_st.c[0], z_st.c[1], z_st.c[2], best, z_st.R, env.cloud[z_st.tgt].opacity);
    }
    double rx = cf_wrap(z_st.c[0] - e[0], P), ry = z_st.c[1] - e[1], rz = cf_wrap(z_st.c[2] - e[2], P);
    float hd = z_instr.heading_deg, ht = (float)atan2(rx, -rz) * 57.29578f;
    float eh = fmodf(ht - hd + 540.0f, 360.0f) - 180.0f;
    float hz = (float)sqrt(rx * rx + rz * rz);
    if (hz < 250.0f) eh = 0.0f;                                  /* at the cloud: hold the wings level */
    /* a target inside the turning circle (40 deg of bank at ~320 kt turns on ~3.3 km): fly straight out to 6.5 km
       first, then turn back (it would circle it for ever) */
    if (!z_st.ext && hz < 3000.0f && fabsf(eh) > 75.0f) z_st.ext = 1;
    if (z_st.ext && hz > 6500.0f) z_st.ext = 0;
    if (z_st.ext) eh = 0.0f;
    float bank = eh * 1.5f; bank = bank > 40.0f ? 40.0f : bank < -40.0f ? -40.0f : bank;
    float sx = (bank - (-z_instr.roll_deg)) * 0.05f; sx = sx > 1.0f ? 1.0f : sx < -1.0f ? -1.0f : sx;
    float pc = (float)ry * 0.024f; pc = pc > 15.0f ? 15.0f : pc < -15.0f ? -15.0f : pc;
    float sy = (pc - z_instr.pitch_deg) * 0.08f + fabsf(bank) * 0.012f;
    sy = sy > 1.0f ? 1.0f : sy < -1.0f ? -1.0f : sy;
    /* the game ignores about +-60 around the centre: commands -1..1 go to 128 +- (60 + 60 |c|) */
    int ix = fabsf(sx) < 0.05f ? 0 : (int)copysignf(60.0f + 60.0f * fabsf(sx), sx);
    int iy = fabsf(sy) < 0.05f ? 0 : (int)copysignf(60.0f + 60.0f * fabsf(sy), sy);
    z_st.lx = (u8)(128 + ix); z_st.ly = (u8)(128 + iy);
    if (z_st.dbg > 0 && f % (u64)z_st.dbg == 0u) {
        extern float ps2_vk_imm_body(void);
        float keep = 1.0f;                                       /* the same ball model at the aircraft (a cross-check) */
        for (u32 i = 0; i < env.ncloud; i++) {
            double a = cf_wrap(env.cloud[i].pos_m[0] + env.origin_m[0] - e[0], P), b = env.cloud[i].pos_m[1] - e[1];
            double c = cf_wrap(env.cloud[i].pos_m[2] + env.origin_m[2] - e[2], P), R = env.cloud[i].radius_m;
            double q = sqrt(a * a + b * b + c * c) / R;
            if (q >= 0.9) continue;
            float sq = q <= 0.45 ? 1.0f : 1.0f - (float)(q - 0.45) / 0.45f;
            keep *= 1.0f - env.cloud[i].opacity * sq * sq * (3.0f - 2.0f * sq);
        }
        ps2_log("cfield: steer field %llu hdg %.1f to %.1f roll %.1f pitch %.1f -> LX %u LY %u; cloud %.0f m ahead-ish, "
                "%.0f m up, R %.0f m; in_cloud C1 %.2f, here %.2f, body %.2f%s", (unsigned long long)f, hd, ht, z_instr.roll_deg,
                z_instr.pitch_deg, z_st.lx, z_st.ly, hz, ry, z_st.R, (env.valid & CL_ENV_INCLOUD) ? env.in_cloud : -1.0f,
                1.0f - keep, ps2_vk_imm_body(), z_st.ext ? " (extending)" : "");
    }
}
/* effect/sound instances (0x17C240: a0 manager, a1 type id, a2 parameter block, a3 ...): the first ones of each type */
static int fx_create_enter(ps2_ctx *ctx, void *user) {
    (void)user;
    static u32 seen_t[256], seen_n[256]; static int ns;
    u32 t = ctx->r[5].uw[0] & 0xFFFFu, p = ctx->r[6].uw[0], ra = ctx->r[31].uw[0];
    int k;
    for (k = 0; k < ns && seen_t[k] != (t | ra << 16); k++) ;
    if (k == ns && ns < 256) { seen_t[ns] = t | ra << 16; seen_n[ns] = 0; ns++; }
    if (p < 0x100000u || p >= 0x2000000u) return 0;
    float v[8];
    for (u32 i = 0; i < 8; i++) v[i] = f_of(ps2_r32(p + 4u * i));
    float d = -1.0f;
    if (v[3] == 1.0f) {
        float dx = v[0] - z_imm_ppos[0], dy = v[1] - z_imm_ppos[1], dz = v[2] - z_imm_ppos[2];
        d = sqrtf(dx * dx + dy * dy + dz * dz);
    }
    int boom = t == 4u || (t >= 60u && t <= 80u);
    if (k < 256 && (seen_n[k]++ < 3u || (boom && d >= 0.0f && d < 30000.0f)))
        ps2_log("immerse: fx field %llu type %u ra %08X a3 %08X t0 %08X dist %.0f par %.4g %.4g %.4g %.4g %.4g %.4g %.4g %.4g",
                (unsigned long long)ps2_kernel_vblank_count(), t, ra, ctx->r[7].uw[0], ctx->r[8].uw[0], d,
                v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
    return 0;
}
/* damage (0x1FCD28: a0 target unit, a1 hit record, a2 ?, a3 amount, t0 kind, t1 attacker, t2 flag; units keep their
   hit points as s16 at +0x398, the invincible bit 0x8000 at +0x1F8) */
static u32 z_imm_player;
/* the player's unit record (the unit table: *( *(*(*(player+0x1A5C)) + 4 group) + 0x104) + index * 0x4A0, as 0x231808
   finds it): hit points s16 at +0x398, flags +0x1F8 (0x8000 invincible), position +0x260 */
static void imm_unit(u32 player) {
    u32 t = ps2_r32(player + 0x1A5Cu), u = 0u;
    if (t >= 0x100000u && t < 0x2000000u) {
        u32 g = ps2_r32(t);
        u32 e = g >= 0x100000u && g < 0x2000000u ? ps2_r32(g + 4u * ps2_r8(player + 0x1AB1u)) : 0u;
        u = e >= 0x100000u && e < 0x2000000u ? ps2_r32(e + 0x104u) + (u32)ps2_r8(player + 0x1AB2u) * 0x4A0u : 0u;
    }
    z_imm_player = u >= 0x100000u && u < 0x2000000u ? u : 0u;
}
u32 zero_vr_player_unit(void) { return z_imm_player; }     /* (zero_cracks.c: the player's hits) */
static int damage_enter(ps2_ctx *ctx, void *user) {
    (void)user;
    u32 u = ctx->r[4].uw[0];
    int me = u == z_imm_player && u != 0u;
    static int shown;
    if (me || shown < 40) {
        if (!me) shown++;
        ps2_log("immerse: damage field %llu %s unit %08X hp %d amount %u kind %u attacker %08X a2 %08X t2 %u ra %08X",
                (unsigned long long)ps2_kernel_vblank_count(), me ? "PLAYER" : "other", u,
                u >= 0x100000u && u < 0x2000000u ? (int)(s16)ps2_r16(u + 0x398u) : -1, ctx->r[7].uw[0] & 0xFFFFu,
                ctx->r[8].uw[0] & 0xFFu, ctx->r[9].uw[0], ctx->r[6].uw[0], ctx->r[10].uw[0] & 0xFFu, ctx->r[31].uw[0]);
    }
    return 0;
}
/* Units destroyed (0x1FC880: a0 unit record, a1 kind; it spawns the explosion, effect type 4, at the unit's position
   +0x260; the game calls it again for a dead unit, though not every frame): an explosion within 400 m thumps
   (ps2_imm_in[5]).
   The debris: see imm_debris below (ACZ_IMM_DEBRIS); with ACZ_IMM_DEBRIS=0 (as before) the wreck is followed for 1.5 s
   and passing within 90 m of it is a debris strike (ps2_imm_in[4], ps2_imm_audio[5]). */
static float imm_dist_units(u32 a, u32 b) {
    float s2 = 0.0f;
    for (u32 i = 0; i < 3; i++) {
        float q = f_of(ps2_r32(a + 0x260u + 4u * i)) - f_of(ps2_r32(b + 0x260u + 4u * i));
        s2 += q * q;
    }
    return sqrtf(s2);
}
/* ---- debris passes (wish #47, ACZ_IMM_DEBRIS) ----
   What the game shows of a kill (ACZ_DEBUG_IMM_DEBRIS_FX probe, mission 7, 2026-10-23): the explosion's effects -- eight
   instances of types 79/66/64/69/83 made by 0x180300, lives 18-360 fields -- stay where the unit died (their effect
   slots' positions never move), while the dead unit itself flies on (+0x260: ~230 m/s, slowing a few % a second,
   sinking) with a type-2 effect (the smoke trail) following it, until it reaches the ground (~9 s). The fragments the
   game draws are particles of those static effects. So the debris here, per kill:
   (A) a cloud of bits about the kill point: a gaussian whose width grows from 4 m to ~30 m over ~3 s (fragments flung
       out at tens of m/s and slowed by the air), sinking slowly, fading with the explosion's effects by 6 s;
   (B) ten larger chunks flung from the kill point on their own paths (15-60 m/s, drag 1-3 s, gravity);
   (C) the wreck where the game has it (+0x260 each frame), with a few bits about it (5 m), for 8 s.
   From the aircraft's path through them (closest approach between frames):
   - zips: a chunk passing within 25 m or the wreck within 40 m -> an event with its position and velocity relative to
     the head; ps2_audio.c renders a short whoosh / rattle there (binaural, Doppler);
   - canopy hits: the expected number of bits meeting the canopy (density x 0.5 m^2 x path: a Poisson draw each frame;
     a chunk within 1.2 m is a clank) -> an event with the time to impact and the impact direction (cockpit axes:
     ps2_video.c draws a small soot fleck there; head axes: the tick or clank);
   - the bed: how deep the aircraft is in a cloud of bits (0..1, its direction, the closing speed) -> a rattle and hiss
     while flying through, a faint rumble.
   Rare by construction: straight through a fresh cloud ~0.5-1.5 hits, 15 m to the side ~0.1, nothing beyond 2.5
   widths. Nothing here touches the game: its positions are read, the effects are ours. */
#define ZD_KILLS  8
#define ZD_CHUNKS 10
#define ZD_UPM    5.12f              /* game units per metre (the altimeter's 400/2048, the sound's 3D_DISTANCE_RANGE) */
#define ZD_EV     32
typedef struct {
    int on, syn, hit;                /* syn: ACZ_IMM_DEBRIS_TEST; hit: the old 90 m strike happened (ACZ_IMM_DEBRIS=0) */
    u32 unit; u64 f0, fw;            /* fw: the field of the wreck's last sample */
    int lost;                        /* the game's unit no longer the wreck (alive again: the record reused; a jump) */
    float p0[3];                     /* the kill point (units): the explosion's effects stay there */
    float w[3], wv[3];               /* the wreck (units) and its velocity (units per field) */
    float sv[3];                     /* synthetic wreck: its start velocity (units per field) */
    u32 seed, rng;
    u8 zip[ZD_CHUNKS + 1];           /* chunk i (ZD_CHUNKS: the wreck) passed */
    int hits, forced;
    float dmin;                      /* closest approach to the kill point so far (m) */
    /* ACZ_IMM_SMOKE_PASS (bug #189): the grit in the smoke ball, with its own random stream (the bits' draws stay as
       before); and the pass summary (ACZ_IMM_DEBRIS_LOG) */
    u32 grng; int grit, ngrit_lost;  /* grit hits sent; those past the cap */
    float smin, tmin, vmin, rmin;    /* closest approach to the smoke ball's middle (the kill point, m), the kill's age
                                        (s), the aircraft's speed (m/s) and the ball's radius (m) then */
    float lam_bits, lam_grit;        /* expected canopy hits so far: the bits' clouds, the smoke ball's grit */
    int nzip, nbit, nclank;          /* zips sent, bit ticks and clanks (drawn or a chunk's) */
    float dist0;                     /* distance from the aircraft at the kill (m) */
    float tin;                       /* time inside the game's visible smoke (s; ACZ_IMM_SMOKE_SIZE) */
    int kind;                        /* the game's explosion kind (effect type 4's parameter +0x10; -1 unknown) */
} zd_kill_t;
static zd_kill_t zd_k[ZD_KILLS];
float ps2_imm_debris_look(void);            /* (ps2_cracks.c: ACZ_IMM_DEBRIS_LOOK, bug #260) */
/* to ps2_audio.c (sound), ps2_video.c (canopy flecks, rumble): an event ring and the bed (see the comment above).
   ps2_imm_deb_ev[i]: [0] kind (1 zip of a chunk, 2 zip of the wreck, 3 canopy hit tick, 4 canopy hit clank, 5 a grit
   tick from a kill's smoke ball: ACZ_IMM_SMOKE_PASS), [1] delay
   to the impact (s), [2..4] position relative to the head (m; head axes x right, y up, z front), [5..7] its velocity
   relative to the head (m/s), [8] strength 0..1, [9..11] hits: the impact's direction from the eye in cockpit axes (x
   right, y up, z back; 0 = not on the canopy in view). ps2_imm_deb_seq counts the events written (slot = seq % 32).
   ps2_imm_deb_bed: [0] intensity 0..1, [1..3] direction of the cloud's centre (head axes), [4] its distance (m), [5]
   inside 0..1 (1 = bits all around), [6] closing speed (m/s), [7] field of the update. */
volatile float ps2_imm_deb_ev[ZD_EV][12];
volatile u32 ps2_imm_deb_seq;
volatile float ps2_imm_deb_bed[8];
/* ACZ_IMM_DEBRIS: unset / 1 = sound, glass and rumble; a list of sound,glass,rumble = those; 0 = the old 90 m strike
   (as before); none = no debris effects. Bits: 1 sound, 2 glass, 4 rumble; 0 old; -1 none. */
int ps2_imm_debris_mask(void) {
    static int m = -2; static unsigned mg;
    if (ps2_menv_take(&mg, ps2_menv_gen) | (m == -2)) {      /* (several threads ask: each computes the same value) */
        const char *e = ps2_menv("ACZ_IMM_DEBRIS");
        if (!e || !*e || !strcmp(e, "1") || !strcmp(e, "on")) m = 7;
        else if (!strcmp(e, "0")) m = 0;
        else {
            m = (strstr(e, "sound") ? 1 : 0) | (strstr(e, "glass") ? 2 : 0) | (strstr(e, "rumble") ? 4 : 0);
            if (!m) m = -1;
        }
    }
    return m;
}
static int zd_dbg;                   /* ACZ_DEBUG_IMM_DEBRIS=N: kills, events, and every N fields near a kill */
static float zd_range = 1500.0f;     /* kills followed within this distance (m; ACZ_DEBUG_IMM_DEBRIS_RANGE, test) */
static float zd_hits_k = 1.0f;       /* ACZ_IMM_DEBRIS_HITS: scale of the canopy hit rate */
/* Flying through a kill's smoke (bug #189, playtest 2026-10-28 22:12:56 / 22:13:29, F-15C: the user flew through the
   dark smoke of two fresh kills, ~1.3 s and ~0.8 s after them: "the fragments should knock a little on the window ...
   maybe scratch it a little, not strongly, with no damage"; nothing ticked, scratched or sooted). The bits' cloud
   above is wide (4 -> 30 m in 3 s: at 1.3 s ~21 m) and rare by construction: straight through its middle at 1.3 s
   0.2 expected canopy hits (P(none) 0.82); the soot ball left ~0.15, a film of ~1-3 % alpha. ACZ_IMM_SMOKE_PASS (on;
   0 = as before) adds what the visible smoke itself carries -- fine grit (ash, paint flakes, metal and glass specks
   0.5-3 mm) in the explosion's smoke ball (the soot ball of zero_cracks.c: the kill point, where the game's smoke
   effects stay, radius 5 m + 4 m/s): ZD_GRIT_N grains at the kill, settling out of the smoke with a 2.5 s time
   constant, gaussian over the ball; canopy hits = grains x the canopy's 0.5 m^2 x the path (a Poisson draw each
   frame, at most ZD_GRIT_MAX a kill). Straight through the middle: ~8 at 0.5 s, ~4 at 1 s, ~2.7 at 1.3 s, ~1.3 at 2 s,
   ~0.5 at 3 s; 9 m to the side at 1 s ~2.4, 18 m ~0.5. Each is an event of kind 5 (a grit tick): a light high tick
   (ps2_audio.c: a grain of 0.6-3 mm), a micro-pit with a fine short scratch (ps2_cracks.c ACZ_VR_CANOPY_CHIPS), a
   faint soot fleck (ps2_video.c), no rumble; and zero_cracks.c deposits the ball's soot at 12 instead of 4 per s
   (the wreck's trail 6). ACZ_IMM_SMOKE_GRIT scales the grain count. */
#define ZD_GRIT_N   6000.0f
#define ZD_GRIT_MAX 10
/* Bug #213 (playtest 2026-10-29 11:02:13, M13 F-15C: "now dust -- I don't see it on the glass, nor fragments"; a C-17
   killed 387 m ahead at 11:02:07.6, the aircraft 34 m from the kill point 1.75 s later, inside the explosion's visible
   smoke (video 695.2 s: haze and specks all round), but 22 m outside the ball above (radius 12 m then): 0.02 grit hits
   expected, soot 0.005). The game's own explosion, measured from its sprites (ps2_video.c ACZ_DEBUG_FX_EXTENT with the
   ACZ_DEBUG_FX_BOOM test explosions: M1 in the air, M13 on the ground, all six kinds): the kills' explosions (effect type
   4 kind 0 or 3: 39 / 61 of the real kills logged so far) are 68 smoke sprites (blend 0x144) of half-size (centre to
   corner) 22 -> 42 m whose centres spread from the kill point 11 -> 21 m (median), 13 -> 28 m (75 %) over 0.25 -> 2.5 s,
   rising ~5.5 m/s to ~11 m, and 42 fire sprites (2 -> 19 m, rising to ~24 m); air and ground alike (the ground's
   sits on the ground). Its visible radius -- the centres' 75 % + 0.45 x the half-size (the texture's soft puff) -- is
   R = 48 - 30 exp(-t / 1 s) m: 24 / 30 / 37 / 44 / 46 m at 0.25 / 0.5 / 1 / 2 / 2.5 s, within +-2 m of all four
   measured explosions; its middle min(5.5 t, 12) m above the kill point. Kinds 1 / 4 (a lone fireball: 22 smoke + 44
   fire sprites) rise ~26 m with a ~15 -> 22 m radius; kind 2 spreads ~1.6 x wider; kind 5 (its smoke ~100 m off the
   point) and unknown kinds as 0. ACZ_IMM_SMOKE_SIZE (on; 0 = the ball before: 5 m + 4 m/s, gaussian, at the kill
   point): the grit, the soot (zero_cracks.c) and a soft hiss follow the game's smoke: a flat-topped profile
   1 / (1 + (r / R)^6) -- about even inside the visible smoke, half at its edge, 8 % at 1.5 R -- holding the column the
   grit and soot had through the old ball's middle (grit 8 grains per m^2 at 1 s, settling over 2.5 s: ~4 hits straight
   through at 1 s, ~2.7 at 2 s), fading from 3.5 to 6 s with the game's sprites. */
static int zd_size = -1;
int zero_vr_smoke_size(void) {
    if (zd_size < 0) { const char *e = getenv("ACZ_IMM_SMOKE_SIZE"); zd_size = !(e && *e == '0'); }
    return zd_size;
}
/* the explosion's smoke at t s after the kill: its middle's rise above the kill point (m), visible radius R (m), relative
   density (the kinds' smoke sprite counts) and the fade (the game's sprites going) */
void zero_vr_smoke_ball(int kind, float t, float *rise, float *R, float *dens, float *fade) {
    if (!zero_vr_smoke_size()) {
        *fade = 1.0f; *rise = 0.0f; *R = 5.0f + 4.0f * t; *dens = 1.0f;
        return;
    }
    *fade = t < 3.5f ? 1.0f : t < 6.0f ? 1.0f - (t - 3.5f) / 2.5f : 0.0f;
    if (kind == 1 || kind == 4) { *R = 22.0f - 12.0f * expf(-t / 0.8f); *rise = 26.0f - 19.0f * expf(-t / 0.6f); *dens = 0.35f; }
    else {
        *R = (48.0f - 30.0f * expf(-t)) * (kind == 2 ? 1.6f : 1.0f);
        *rise = 5.5f * t < 12.0f ? 5.5f * t : 12.0f;
        *dens = kind == 2 ? 0.85f : 1.0f;
    }
}
float zero_vr_smoke_profile(float d, float R) { float q = d / R; q = q * q * q; return 1.0f / (1.0f + q * q); }
static int zd_kind_of(const float *p0);
int zero_vr_debris_kind(const float *p0) { return zd_kind_of(p0); }   /* (zero_cracks.c: a followed kill's kind) */
static int zd_smoke = -1;            /* ACZ_IMM_SMOKE_PASS */
static float zd_grit_k = 1.0f;       /* ACZ_IMM_SMOKE_GRIT */
static int zd_log = 1;               /* ACZ_IMM_DEBRIS_LOG: one line per kill followed */
int zero_vr_smoke_pass(void) {
    if (zd_smoke < 0) { const char *e = getenv("ACZ_IMM_SMOKE_PASS"); zd_smoke = !(e && *e == '0'); }
    return zd_smoke;
}
/* Bug #260 (playtest 2026-10-31, M1 / M4 / M10: "no dust, no debris from this (shot down) aircraft", "debris doesn't
   scratch the glass"). A kill by a weapon (the hit's mode 3, as the game's missiles and guns hit) reaches 0x1FC880 with
   a1 = 3 (from 0x1FD014 inside the damage function 0x1FCD28, again from 0x1FB438 when the wreck ends); that path does
   not make effect type 4 (a crash's explosion, a1 = the ground kind; what ACZ_DEBUG_FX_BOOM and pt_fx / fx_boom
   measured) but, by the unit's class byte (unit +0x1C4 -> type data +0x7F), type 3 (class 1: the aircraft seen so far),
   54 (class 2) or 58 (class 3), its parameter +0x10 the type data's +0x80, then the 0x180300 set in the same field
   (traced 2026-10-31, ACZ_DEBUG_IMM_BLAST=2: M1 unit 017253A0 'a1 3 ra 001FD014 class 1/1' -> 'type 3 ra 001FC930 +10
   1', then 79 66 64 69 83 64 64 64 from 0x180300). So every real kill of the 10-29 / 10-31 sessions logged 'explosion
   kind -1' (288 of 289; the other a crash's kind 3) and its blast was the set's (R0 32 m: 84 m away strength 0.35, no
   fragments) instead of a kill's (R0 70 m: 0.79, 2-3 fragments). ACZ_IMM_KILL_FX (1; 0 = before): types 3 / 54 / 58 from
   0x1FC880 are kills' explosions -- the blast's R0 70 m and the followed kill's kind 100 x (1 / 2 / 3) + the parameter
   (zero_vr_smoke_ball: as kind 0) */
static int zd_killfx = -1;
static int zd_airkill_kind(u32 t, u32 ra, u32 p) {
    if (zd_killfx < 0) { const char *e = getenv("ACZ_IMM_KILL_FX"); zd_killfx = !(e && *e == '0'); }
    if (!zd_killfx || !(ra >= 0x001FC880u && ra < 0x001FCD08u)) return -1;
    int c = t == 3u ? 1 : t == 54u ? 2 : t == 58u ? 3 : 0;
    return c ? 100 * c + (int)(ps2_r32(p + 0x10u) & 0xFFu) : -1;
}
/* ACZ_IMM_DEBRIS_TEST=ahead_m[,miss_m[,period_s[,wreck_frac[,first_field[,hits]]]]] (test): a kill placed on the
   aircraft's straight path ahead_m ahead (miss_m to the right), every period_s from first_field; its wreck starts with
   wreck_frac of the aircraft's velocity; hits = canopy hits forced at the pass through the middle (look tests) */
static struct { int on; float ahead, miss, per, frac, force; u64 first, next; } zd_t;
static float zd_rand(u32 *s) { *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5; return (float)(*s >> 8) * (1.0f / 16777216.0f); }
static float zd_dot(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void zd_emit(int kind, float delay, const float *p, const float *v, float st, const float *m) {
    u32 s = ps2_imm_deb_seq;
    volatile float *e = ps2_imm_deb_ev[s % ZD_EV];
    e[0] = (float)kind; e[1] = delay; e[8] = st;
    for (int j = 0; j < 3; j++) { e[2 + j] = p[j]; e[5 + j] = v ? v[j] : 0.0f; e[9 + j] = m ? m[j] : 0.0f; }
    __atomic_store_n(&ps2_imm_deb_seq, s + 1u, __ATOMIC_RELEASE);
}
/* ACZ_IMM_DEBRIS_LOG: the pass summary of a kill when it is no longer followed (8 s, or replaced): how close the
   aircraft came to its smoke ball and to the bits' cloud, the expected and the sent canopy hits, the zips */
static void zd_summary(const zd_kill_t *z, int i) {
    if (!zd_log || !z->on || ps2_imm_debris_mask() <= 0) return;
    char sm[160];
    if (z->smin < 1e8f && zero_vr_smoke_size())
        snprintf(sm, sizeof sm, "%.0f m at %.2f s (the game's smoke there: radius %.0f m; inside it %.2f s; %.0f m/s)", z->smin,
                 z->tmin, z->rmin, z->tin, z->vmin);
    else if (z->smin < 1e8f)
        snprintf(sm, sizeof sm, "%.0f m at %.2f s (ball radius %.1f m, %.0f m/s)", z->smin, z->tmin, z->rmin, z->vmin);
    else snprintf(sm, sizeof sm, "not reached");
    ps2_log("imm-debris: kill %d %s%08X (field %llu, %.0f m away at the kill, explosion kind %d): closest to its smoke ball's middle %s, "
            "to the bits' cloud %.0f m; expected canopy hits: bits %.2f, smoke grit %.2f; sent: %d bit tick%s, %d clank%s, "
            "%d grit%s%s, %d zip%s", i, z->syn ? "(test) " : "unit ", z->unit, (unsigned long long)z->f0, z->dist0, z->kind, sm,
            z->dmin < 1e8f ? z->dmin : -1.0f, z->lam_bits, z->lam_grit, z->nbit, z->nbit == 1 ? "" : "s", z->nclank,
            z->nclank == 1 ? "" : "s", z->grit, z->grit == 1 ? "" : "s", z->ngrit_lost ? " (more over the cap)" : "",
            z->nzip, z->nzip == 1 ? "" : "s");
}
static int zd_kind_of(const float *p0) {
    for (int i = 0; i < ZD_KILLS; i++) if (zd_k[i].on && !memcmp(zd_k[i].p0, p0, sizeof zd_k[i].p0)) return zd_k[i].kind;
    return -1;
}
static zd_kill_t *zd_add(u32 unit, const float *p0, int syn, const float *sv, u64 f) {
    int k = 0;
    for (int i = 0; i < ZD_KILLS; i++) {
        if (!zd_k[i].on) { k = i; break; }
        if (zd_k[i].f0 < zd_k[k].f0) k = i;
    }
    zd_kill_t *z = &zd_k[k];
    zd_summary(z, k);
    memset(z, 0, sizeof *z);
    z->on = 1; z->syn = syn; z->unit = unit; z->f0 = z->fw = f;
    memcpy(z->p0, p0, sizeof z->p0); memcpy(z->w, p0, sizeof z->w);
    if (sv) { memcpy(z->sv, sv, sizeof z->sv); memcpy(z->wv, sv, sizeof z->wv); }
    z->seed = (u32)f * 2654435761u ^ unit ^ 0x5bd1e995u; if (!z->seed) z->seed = 1u;
    z->rng = z->seed;
    z->grng = z->seed ^ 0x2545F491u; if (!z->grng) z->grng = 1u;
    z->dmin = 1e9f; z->smin = 1e9f; z->kind = -1;
    return z;
}
static void imm_probe_add(u32 u, u64 f);
static int zbl_replaces_thump(void);
static int destroy_enter(ps2_ctx *ctx, void *user) {
    (void)user;
    u32 u = ctx->r[4].uw[0], me = z_imm_player;
    if (u < 0x100000u || u >= 0x2000000u || !me) return 0;
    u64 f = ps2_kernel_vblank_count();
    {   static int dbg = -1;
        if (dbg < 0) { const char *e = getenv("ACZ_DEBUG_IMM_BLAST"); dbg = e ? atoi(e) : 0; }
        if (dbg >= 2) {
            u32 ty = ps2_r32(u + 0x1C4u), td = ty >= 0x100000u && ty < 0x2000000u ? ps2_r32(ty) : 0u;
            int cls = td >= 0x100000u && td < 0x2000000u ? (int)ps2_r8(td + 0x7Fu) : -1;
            int cb = td >= 0x100000u && td < 0x2000000u ? (int)ps2_r8(td + 0x80u) : -1;
            ps2_log("imm-blast: 0x1FC880 field %llu unit %08X a1 %u ra %08X hp %d flags %08X +1C0 %08X class %d/%d, %.0f m",
                    (unsigned long long)f, u, ctx->r[5].uw[0] & 0xFFu, ctx->r[31].uw[0], (int)(s16)ps2_r16(u + 0x398u),
                    ps2_r32(u + 0x1F8u), ps2_r32(u + 0x1C0u), cls, cb, imm_dist_units(u, me) / ZD_UPM);
        }
    }
    {   /* the game calls this again while the unit is dead: the first call only */
        static struct { u32 unit; u64 field; } seen[16]; static int ns;
        for (int i = 0; i < 16; i++)
            if (seen[i].unit == u && f - seen[i].field < 600u) { seen[i].field = f; return 0; }
        seen[ns].unit = u; seen[ns].field = f; ns = (ns + 1) & 15;
    }
    float d = u == me ? 0.0f : imm_dist_units(u, me) / 6.0f;
    if (u != me) imm_probe_add(u, f);
    if (z_imm_dbg > 0)
        ps2_log("immerse: destroyed field %llu unit %08X kind %u dist %.0f m%s", (unsigned long long)f, u,
                ctx->r[5].uw[0] & 0xFFu, d, u == me ? " (PLAYER)" : "");
    if (u == me) return 0;
    extern volatile float ps2_imm_in[8];
    if (d < 400.0f && !zbl_replaces_thump()) ps2_imm_in[5] += 1.0f - d / 400.0f;   /* (ACZ_IMM_BLAST: the blast's own) */
    if (d * 6.0f / ZD_UPM < zd_range && ps2_imm_debris_mask() >= 0) {
        float p0[3];
        for (u32 i = 0; i < 3; i++) p0[i] = f_of(ps2_r32(u + 0x260u + 4u * i));
        zd_kill_t *z = zd_add(u, p0, 0, NULL, f);
        z->dist0 = d * 6.0f / ZD_UPM;
        if (zd_dbg > 0)
            ps2_log("imm-debris: kill field %llu unit %08X kind %u hp %d at %.0f %.0f %.0f, %.0f m away", (unsigned long long)f,
                    u, ctx->r[5].uw[0] & 0xFFu, (int)(s16)ps2_r16(u + 0x398u), p0[0] / ZD_UPM, p0[1] / ZD_UPM, p0[2] / ZD_UPM,
                    d * 6.0f / ZD_UPM);
    }
    return 0;
}
/* ACZ_DEBUG_IMM_DEBRIS_FX=1 (research): after a kill, every 6 fields for 10 s the dead unit's record (+0x200..+0x2BC
   as floats) and the game's effect instances within 4000 units of the kill point (the effect manager -- a0 of 0x17C240
   -- has 512 slots: in-use bits at +0x1BC84, records of 0x80 bytes at +0x1BCE0: s16 type +0, u16 age +8, u16 life +0xA,
   position +0x10). How the explosion's effects and the wreck behave (see imm_debris). */
static u32 z_fx_mgr;
static int fx_mgr_enter(ps2_ctx *ctx, void *user) {
    (void)user; u32 m = ctx->r[4].uw[0];
    if (m >= 0x100000u && m < 0x2000000u) z_fx_mgr = m;
    return 0;
}
static struct { u32 unit; u64 field; float p0[3]; } z_probe[8];
static void imm_probe_add(u32 u, u64 f) {
    static int k;
    if (!z_fx_mgr) return;                       /* (the probe is off: its hook never ran) */
    z_probe[k].unit = u; z_probe[k].field = f;
    for (u32 i = 0; i < 3; i++) z_probe[k].p0[i] = f_of(ps2_r32(u + 0x260u + 4u * i));
    k = (k + 1) & 7;
}
static void imm_probe(u64 f) {
    u32 m = z_fx_mgr;
    if (!(m >= 0x100000u && m < 0x2000000u)) return;
    for (int w = 0; w < 8; w++) {
        if (!z_probe[w].unit || f - z_probe[w].field > 600u || (f - z_probe[w].field) % 6u) continue;
        u32 u = z_probe[w].unit;
        char ln[1600]; int n = 0;
        n += snprintf(ln + n, sizeof ln - (size_t)n, "dprobe: unit %08X +%llu f:", u, (unsigned long long)(f - z_probe[w].field));
        for (u32 o = 0x200u; o < 0x2C0u; o += 4u) {
            float v = f_of(ps2_r32(u + o));
            if (v != 0.0f && fabsf(v) < 1e8f && fabsf(v) > 1e-6f) n += snprintf(ln + n, sizeof ln - (size_t)n, " %X:%.4g", o, v);
        }
        ps2_log("%s", ln);
        n = 0;
        n += snprintf(ln + n, sizeof ln - (size_t)n, "dprobe: fx near %08X:", u);
        int cnt = 0;
        for (u32 s = 0; s < 512u && n < (int)sizeof ln - 80; s++) {
            if (!(ps2_r32(m + 0x1BC84u + 4u * (s >> 5)) & (1u << (s & 31u)))) continue;
            u32 r = m + 0x1BCE0u + s * 0x80u;
            float q[3]; float d2 = 0.0f;
            for (u32 i = 0; i < 3; i++) { q[i] = f_of(ps2_r32(r + 0x10u + 4u * i)); d2 += (q[i] - z_probe[w].p0[i]) * (q[i] - z_probe[w].p0[i]); }
            if (!(d2 < 4000.0f * 4000.0f)) continue;
            cnt++;
            n += snprintf(ln + n, sizeof ln - (size_t)n, " [%u t%d a%u/%u %.0f %.0f %.0f]", s, (int)(s16)ps2_r16(r),
                          ps2_r16(r + 8u), ps2_r16(r + 0xAu), q[0] - z_probe[w].p0[0], q[1] - z_probe[w].p0[1], q[2] - z_probe[w].p0[2]);
        }
        if (cnt) ps2_log("%s", ln);
    }
}
/* the wreck of kill z now (units) and its velocity (units per field): the game's unit while it stays dead (hit points
   s16 +0x398 <= 0, -32 at the kill; the game does not call 0x1FC880 every frame for a dead unit, so not that) and moves
   like one (a jump beyond 2400 units/s: the record reused), else where it was last; or the synthetic one's drag path */
static void zd_wreck(zd_kill_t *z, u64 f) {
    if (z->syn) {
        float t = (float)(f - z->f0), tw = 480.0f, e = expf(-t / tw), sink = 10.0f * ZD_UPM / 60.0f;
        for (int j = 0; j < 3; j++) { z->w[j] = z->p0[j] + z->sv[j] * tw * (1.0f - e); z->wv[j] = z->sv[j] * e; }
        z->w[1] -= sink * t; z->wv[1] -= sink;
        return;
    }
    if (f == z->fw || z->lost) return;
    float q[3];
    for (u32 i = 0; i < 3; i++) q[i] = f_of(ps2_r32(z->unit + 0x260u + 4u * i));
    float dt = (float)(f - z->fw), j2 = 0.0f;
    for (int j = 0; j < 3; j++) j2 += (q[j] - z->w[j]) * (q[j] - z->w[j]);
    if ((s16)ps2_r16(z->unit + 0x398u) > 0 || !(j2 < (40.0f * dt * 40.0f * dt)) || dt >= 30.0f) {
        z->lost = 1; z->wv[0] = z->wv[1] = z->wv[2] = 0.0f;
        if (zd_dbg > 0) ps2_log("imm-debris: field %llu the wreck of unit %08X is no longer followed (hp %d, moved %.0f units)",
                                (unsigned long long)f, z->unit, (int)(s16)ps2_r16(z->unit + 0x398u), sqrtf(j2));
        return;
    }
    if (dt > 0.0f) for (int j = 0; j < 3; j++) z->wv[j] = 0.5f * z->wv[j] + 0.5f * (q[j] - z->w[j]) / dt;
    memcpy(z->w, q, sizeof z->w);
    z->fw = f;
}
/* chunk i of kill z at t s after it: position (units) and velocity (m/s); drag time tau, gravity: terminal g tau */
static void zd_chunk(const zd_kill_t *z, int i, float t, float *x, float *v) {
    u32 s = z->seed ^ ((u32)(i + 1) * 0x9E3779B9u);
    for (int k = 0; k < 3; k++) zd_rand(&s);
    float a = zd_rand(&s), b = zd_rand(&s), c = zd_rand(&s), d = zd_rand(&s);
    float uz = 2.0f * a - 1.0f, ur = sqrtf(1.0f - uz * uz), ph = 6.2831853f * b;
    float u[3] = { ur * cosf(ph), uz, ur * sinf(ph) };
    float sp = 15.0f + 45.0f * c, tau = 1.0f + 2.0f * d, e = expf(-t / tau), g = 9.81f;
    for (int k = 0; k < 3; k++) { x[k] = z->p0[k] + sp * u[k] * tau * (1.0f - e) * ZD_UPM; v[k] = sp * u[k] * e; }
    x[1] -= g * tau * (t - tau * (1.0f - e)) * ZD_UPM;
    v[1] -= g * tau * (1.0f - e);
}
/* the bits' cloud about the kill point at t s: centre (units), width (m), fade 0..1 */
static void zd_cloud(const zd_kill_t *z, float t, float *c, float *sig, float *fade) {
    float e = expf(-t);
    memcpy(c, z->p0, 3 * sizeof(float));
    c[1] -= 9.81f * (t - (1.0f - e)) * ZD_UPM;                    /* sinking (drag 1 s) */
    *sig = 4.0f + 26.0f * (1.0f - expf(-t / 1.2f));
    *fade = t < 4.0f ? 1.0f : t < 6.0f ? 1.0f - (t - 4.0f) * 0.5f : 0.0f;
}
/* a canopy impact's direction (cockpit axes, unit) kept on the glass: +2 .. 65 deg up (below: the coaming, the panel) */
static void zd_glass(float *mc) {
    if (mc[1] < 0.035f) mc[1] = 0.035f;
    if (mc[1] > 0.9f) mc[1] = 0.9f;
    float k = sqrtf(fmaxf(0.0f, 1.0f - mc[1] * mc[1])) / (sqrtf(mc[0] * mc[0] + mc[2] * mc[2]) + 1e-6f);
    mc[0] *= k; mc[2] *= k;
}
/* ---- A blast near the aircraft reaches the canopy (bug #232, ACZ_IMM_BLAST) ----
   Playtest 2026-10-29 18:46:21 (M17's canyon, F-15): "something exploded just now and I see no effect on the glass ...
   it would be cool if these explosions made some sound on the glass, some fragments". Only flying through a kill's
   smoke or its bits touched the canopy (imm_debris), and a kill within 400 m gave an instant controller thump; missiles
   bursting nearby, a kill a few hundred metres off: nothing on the glass. Here every explosion the game makes is a
   blast with a size: its effect create 0x17C240 (a1 the type, a2 the parameter block: position +0..+8 with 1.0 at
   +0xC) -- type 4, a kill's explosion (its kind at +0x10: 0 / 3 the usual ~45 m smoke ball, 2 ~1.6x wider, 1 / 4 a lone
   fireball; bug #213), type 51 from 0x20E5A8, a missile's burst or impact, and the 0x180300 set (types 63 66 67 69 73
   79 83) that follows both and other bursts; one within 60 m and 15 fields of another is the same blast (the larger
   kept). Its pressure on the glass falls with the distance: strength = (R0 / d)^1.3 with R0 = 70 m for a kill's
   explosion (90 m kind 2, 45 m a fireball), 40 m a missile's burst, 32 m the set alone (1 at R0, 0.4 at 2 R0, 0.16 at
   4 R0; none below 0.08: a kill beyond ~490 m, a missile's burst beyond ~280 m). The blast wave arrives d / 340 m/s
   after the flash (0.3 s at 100 m): then a dull thump and a short rattle of the canopy in its frame from the blast's
   side (ps2_audio.c, event kind 6) and a thud on the controller (ps2_video.c); within ~1.8 R0 (strength above 0.45) 1-4
   fragments hit the glass where it faces the blast (event kind 3: a tick, a fading soot fleck, a small chip), at
   d / 250 m/s. In the cockpit view only; nothing the game does changes (damage, hits). The kill's instant thump of
   before (ps2_imm_in[5]) is left out while this runs: the kill's blast gives it, on time. ACZ_IMM_BLAST = strength
   scale (1; 0.5 subtle; 0 = as before), ACZ_DEBUG_IMM_BLAST=1 logs every blast and what it did. */
#define ZBL_N 8
static struct { int init; float k; int dbg; u64 n, felt, frag; } zbl;
static struct { float p[3], r0; u64 f; int type, kind, done; } zbl_q[ZBL_N];
static int zbl_k;
static u32 zbl_rng = 0x51ED270Bu;
static int zbl_no_hook;                 /* (the hook did not take: stays off) */
static void zbl_read(void) {
    const char *e = ps2_menv("ACZ_IMM_BLAST");
    zbl.k = e && *e ? (float)atof(e) : 1.0f;
    if (e && !strcmp(e, "off")) zbl.k = 0.0f;
    if (!(zbl.k >= 0.0f)) zbl.k = 0.0f;
    if (zbl.k > 2.0f) zbl.k = 2.0f;
    if (zbl_no_hook) zbl.k = 0.0f;
}
static float zbl_on(void) {
    if (!zbl.init) {
        zbl.init = 1;
        zbl_read();
        const char *e = getenv("ACZ_DEBUG_IMM_BLAST"); zbl.dbg = e ? atoi(e) : 0;
    }
    return zbl.k;
}
static int zbl_fx_enter(ps2_ctx *ctx, void *user) {
    (void)user;
    if (!(zbl_on() > 0.0f)) return 0;
    u32 t = ctx->r[5].uw[0] & 0xFFFFu, p = ctx->r[6].uw[0], ra = ctx->r[31].uw[0];
    if (p < 0x100000u || p >= 0x2000000u) return 0;
    if (zbl.dbg >= 2 && ((ra >= 0x001FC880u && ra < 0x001FCD08u) || ra == 0x00180300u || ra == 0x0020E5A8u)) {
        float q[4]; for (u32 i = 0; i < 4; i++) q[i] = f_of(ps2_r32(p + 4u * i));
        ps2_log("imm-blast: fx create field %llu type %u ra %08X at %.0f %.0f %.0f (w %.3g) +10 %08X",
                (unsigned long long)ps2_kernel_vblank_count(), t, ra, q[0] / ZD_UPM, q[1] / ZD_UPM, q[2] / ZD_UPM, q[3],
                ps2_r32(p + 0x10u));
    }
    float r0 = 0.0f; int kind = -1;
    if (t == 4u) {
        kind = (int)(ps2_r32(p + 0x10u) & 0xFFu);
        r0 = kind == 1 || kind == 4 ? 45.0f : kind == 2 ? 90.0f : 70.0f;
    } else if ((kind = zd_airkill_kind(t, ra, p)) >= 0) r0 = 70.0f;     /* (bug #260: an air kill's explosion) */
    else if (t == 51u && ra == 0x0020E5A8u) r0 = 40.0f;
    else if (ra == 0x00180300u && (t == 63u || t == 66u || t == 67u || t == 69u || t == 73u || t == 79u || t == 83u)) r0 = 32.0f;
    else return 0;
    float v[4];
    for (u32 i = 0; i < 4; i++) v[i] = f_of(ps2_r32(p + 4u * i));
    if (v[3] != 1.0f || !(fabsf(v[0]) < 1e8f && fabsf(v[1]) < 1e8f && fabsf(v[2]) < 1e8f)) return 0;
    u64 f = ps2_kernel_vblank_count();
    float rm = 60.0f * ZD_UPM;
    for (int i = 0; i < ZBL_N; i++) {
        if (!zbl_q[i].f || f - zbl_q[i].f > 15u) continue;
        float dx = v[0] - zbl_q[i].p[0], dy = v[1] - zbl_q[i].p[1], dz = v[2] - zbl_q[i].p[2];
        if (dx * dx + dy * dy + dz * dz < rm * rm) {
            if (r0 > zbl_q[i].r0 && !zbl_q[i].done) { zbl_q[i].r0 = r0; zbl_q[i].type = (int)t; zbl_q[i].kind = kind; }
            return 0;
        }
    }
    zbl_q[zbl_k].p[0] = v[0]; zbl_q[zbl_k].p[1] = v[1]; zbl_q[zbl_k].p[2] = v[2];
    zbl_q[zbl_k].r0 = r0; zbl_q[zbl_k].f = f; zbl_q[zbl_k].type = (int)t; zbl_q[zbl_k].kind = kind; zbl_q[zbl_k].done = 0;
    zbl_k = (zbl_k + 1) % ZBL_N;
    return 0;
}
static void zbl_install(void) {
    if (!(zbl_on() > 0.0f)) { ps2_log("zero-vr: blasts on the canopy off (ACZ_IMM_BLAST=0)"); return; }
    int a = ps2_r32(0x0017C240u) >> 16 == 0x27BDu && ps2_hook_before(0x0017C240u, zbl_fx_enter, NULL, 62, "zero-blast") >= 0;
    if (!a) { zbl.k = 0.0f; zbl_no_hook = 1; }
    ps2_log("zero-vr: blasts on the canopy %s (ACZ_IMM_BLAST %.2f: a thump and rattle on the glass after d / 340 m/s, a "
            "thud on the controller, fragments within ~1.8 R0)", a ? "on" : "off (no hook)", zbl.k);
}
/* (destroy_enter) the kill's instant thump of before, only without the blasts */
static int zbl_replaces_thump(void) { return zbl_on() > 0.0f && ps2_imm_debris_mask() > 0; }
/* once a flight frame: the blasts made since, felt from where the aircraft is now */
static void imm_blast(u32 player, u64 f) {
    if (!(zbl_on() > 0.0f)) return;
    extern volatile int ps2_vk_cockpit_view;
    int mask = ps2_imm_debris_mask();
    u32 fm = player + 0xEF0u;
    float E[3];
    for (u32 i = 0; i < 3; i++) E[i] = f_of(ps2_r32(fm + 0x3B0u + 4u * i));
    float L[12];
    for (u32 i = 0; i < 12; i++) L[i] = f_of(ps2_r32(0x00422C18u + 4u * i));
    float *LP = L, *LF = L + 6, *LU = L + 9;
    float LR[3] = { LF[1] * LU[2] - LF[2] * LU[1], LF[2] * LU[0] - LF[0] * LU[2], LF[0] * LU[1] - LF[1] * LU[0] };
    {   float n = sqrtf(zd_dot(LR, LR)); if (n > 1e-6f) for (int j = 0; j < 3; j++) LR[j] /= n; else { LR[0] = 1.0f; LR[1] = LR[2] = 0.0f; } }
    float pitch = f_of(ps2_r32(fm + 0x3C0u)), yaw = f_of(ps2_r32(fm + 0x3C4u)), roll = f_of(ps2_r32(fm + 0x3C8u));
    float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch), cr = cosf(roll), sr = sinf(roll);
    float art[3] = { cy * cr + sy * sp * sr, cp * sr, -sy * cr + cy * sp * sr };
    float aup[3] = { -sr * cy + cr * sp * sy, cp * cr, sr * sy + cr * sp * cy };
    float afw[3] = { -sy * cp, sp, -cy * cp };
    int ck = ps2_vk_cockpit_view != 0;
    for (int i = 0; i < ZBL_N; i++) {
        if (!zbl_q[i].f || zbl_q[i].done) continue;
        zbl_q[i].done = 1;
        float age = (float)(f - zbl_q[i].f) / 60.0f;
        if (age > 2.0f) continue;                       /* (made during a pause or a menu: stale) */
        float r[3] = { zbl_q[i].p[0] - E[0], zbl_q[i].p[1] - E[1], zbl_q[i].p[2] - E[2] };
        float d = sqrtf(zd_dot(r, r)) / ZD_UPM;
        zbl.n++;
        if (d < 6.0f) continue;                         /* (the own aircraft's: its death, not a blast nearby) */
        float st = zbl.k * powf(zbl_q[i].r0 / d, 1.3f);
        if (st > 1.0f) st = 1.0f;
        int felt = st >= 0.08f && ck && mask > 0, nfr = 0;
        if (felt) {
            zbl.felt++;
            float dw[3] = { r[0] / (d * ZD_UPM), r[1] / (d * ZD_UPM), r[2] / (d * ZD_UPM) };
            float hp[3] = { zbl_q[i].p[0] - LP[0], zbl_q[i].p[1] - LP[1], zbl_q[i].p[2] - LP[2] };
            float hh[3] = { zd_dot(hp, LR) / ZD_UPM, zd_dot(hp, LU) / ZD_UPM, zd_dot(hp, LF) / ZD_UPM };
            float mc[3] = { zd_dot(dw, art), zd_dot(dw, aup), -zd_dot(dw, afw) };
            float delay = d / 340.0f - age;
            if (delay < 0.0f) delay = 0.0f;
            zd_emit(6, delay, hh, NULL, st, mc);
            /* fragments: the glass that faces the blast, within ~20 deg of its direction */
            if (st > 0.45f && (mask & 2)) {
                int n = 1 + (int)((st - 0.45f) / 0.55f * 3.0f + zd_rand(&zbl_rng));
                if (n > 4) n = 4;
                for (int k = 0; k < n; k++) {
                    float jw[3], jm;
                    for (int j = 0; j < 3; j++) jw[j] = dw[j] + (zd_rand(&zbl_rng) - 0.5f) * 0.7f;
                    jm = sqrtf(zd_dot(jw, jw)) + 1e-6f;
                    for (int j = 0; j < 3; j++) jw[j] /= jm;
                    float jc[3] = { zd_dot(jw, art), zd_dot(jw, aup), -zd_dot(jw, afw) };
                    if (jc[2] > 0.35f || jc[1] < -0.15f) continue;          /* behind, or below the coaming: the fuselage */
                    zd_glass(jc);
                    float fd = d / (220.0f + 80.0f * zd_rand(&zbl_rng)) - age;
                    if (fd < 0.0f) fd = 0.0f;
                    if (fd >= 0.95f) continue;
                    float hw[3] = { -jw[0] * 250.0f, -jw[1] * 250.0f, -jw[2] * 250.0f }, mw[3], hq[3];
                    float hv[3] = { zd_dot(hw, LR), zd_dot(hw, LU), zd_dot(hw, LF) };
                    for (int j = 0; j < 3; j++) mw[j] = jw[j] * 0.75f;
                    hq[0] = zd_dot(mw, LR); hq[1] = zd_dot(mw, LU); hq[2] = zd_dot(mw, LF);
                    zd_emit(3, fd, hq, hv, 0.35f + 0.45f * zd_rand(&zbl_rng), jc);
                    nfr++;
                }
                zbl.frag += (u64)nfr;
            }
        }
        if (zbl.dbg > 0 || (felt && st >= 0.3f)) {
            char kn[24] = "";
            if (zbl_q[i].kind >= 0) snprintf(kn, sizeof kn, " kind %d", zbl_q[i].kind);
            float dn[3] = { r[0] / (d * ZD_UPM), r[1] / (d * ZD_UPM), r[2] / (d * ZD_UPM) };   /* (its direction from the
                                            aircraft: azimuth right of the nose, elevation above its wings; fx_mpbm) */
            float az = atan2f(zd_dot(dn, art), zd_dot(dn, afw)) * 57.29578f, el = asinf(fmaxf(-1.0f, fminf(1.0f, zd_dot(dn, aup)))) * 57.29578f;
            ps2_log("imm-blast: field %llu %s (type %d%s) %.0f m away (R0 %.0f m), %.2f s old: strength %.2f%s, %d fragment%s; "
                    "blasts %llu, felt %llu; az %.0f el %.0f", (unsigned long long)f, zbl_q[i].type == 4 || zbl_q[i].kind >= 100 ? "a kill's explosion" : zbl_q[i].type == 51
                    ? "a missile's burst" : "an explosion", zbl_q[i].type, kn, d, zbl_q[i].r0, age, st,
                    felt ? "" : !ck ? " (not felt: not the cockpit view)" : mask <= 0 ? " (not felt: ACZ_IMM_DEBRIS off)"
                    : " (not felt: too weak)", nfr, nfr == 1 ? "" : "s", (unsigned long long)zbl.n, (unsigned long long)zbl.felt, az, el);
        }
    }
}
static void fxk_publish(u64 f);
static void imm_debris(u32 player, u64 f) {
    extern volatile float ps2_imm_in[8];
    extern volatile float ps2_imm_audio[8];
    extern volatile int ps2_vk_cockpit_view;
    int mask = ps2_imm_debris_mask();
    if (z_fx_mgr) imm_probe(f);
    u32 fm = player + 0xEF0u;
    float E[3];
    for (u32 i = 0; i < 3; i++) E[i] = f_of(ps2_r32(fm + 0x3B0u + 4u * i));
    /* the aircraft's velocity (units per field) from its positions */
    static float lE[3], VE[3]; static u64 lf; static int vok;
    u64 dtf = lf ? f - lf : 0u;
    {
        float j2 = 0.0f;
        for (int j = 0; j < 3; j++) j2 += (E[j] - lE[j]) * (E[j] - lE[j]);
        if (dtf > 0u && dtf < 8u && j2 < 40.0f * 40.0f * (float)(dtf * dtf)) {
            for (int j = 0; j < 3; j++) VE[j] = vok ? 0.5f * VE[j] + 0.5f * (E[j] - lE[j]) / (float)dtf : (E[j] - lE[j]) / (float)dtf;
            vok = 1;
        } else if (dtf) vok = 0;
        memcpy(lE, E, sizeof lE); lf = f;
    }
    if (mask == 0) {      /* ACZ_IMM_DEBRIS=0: the old strike, passing within 90 m of the wreck in its first 1.5 s */
        for (int i = 0; i < ZD_KILLS; i++) {
            zd_kill_t *z = &zd_k[i];
            if (!z->on || z->hit || f - z->f0 > 90u || !z_imm_player) continue;
            float d;
            if (z->syn) {
                zd_wreck(z, f);
                float s2 = 0.0f; for (int j = 0; j < 3; j++) s2 += (z->w[j] - E[j]) * (z->w[j] - E[j]);
                d = sqrtf(s2) / 6.0f;
            } else d = imm_dist_units(z->unit, z_imm_player) / 6.0f;
            if (d < 90.0f) {
                float st = 1.0f - d / 90.0f;
                if (st < 0.3f) st = 0.3f;
                ps2_imm_in[4] += st;
                ps2_imm_audio[5] += st;
                z->hit = 1;
                if (z_imm_dbg > 0 || zd_dbg > 0)
                    ps2_log("immerse: debris strike field %llu wreck %08X %.0f m (%.2f)", (unsigned long long)f, z->unit, d, st);
            }
        }
    }
    /* ACZ_IMM_DEBRIS_TEST: a kill on the path ahead */
    if (zd_t.on && vok && f >= zd_t.next) {
        float vm = sqrtf(zd_dot(VE, VE));
        if (vm > 0.5f) {
            float T = zd_t.ahead * ZD_UPM / vm, p0[3], sv[3];
            float pitch = f_of(ps2_r32(fm + 0x3C0u)), yaw = f_of(ps2_r32(fm + 0x3C4u)), roll = f_of(ps2_r32(fm + 0x3C8u));
            float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch), cr = cosf(roll), sr = sinf(roll);
            float rt[3] = { cy * cr + sy * sp * sr, cp * sr, -sy * cr + cy * sp * sr };
            for (int j = 0; j < 3; j++) { p0[j] = E[j] + VE[j] * T + rt[j] * zd_t.miss * ZD_UPM; sv[j] = VE[j] * zd_t.frac; }
            zd_kill_t *zt = zd_add(0u, p0, 1, sv, f);
            zt->dist0 = zd_t.ahead;
            if (zd_dbg > 0)
                ps2_log("imm-debris: test kill field %llu %.0f m ahead (%.2f s), %.1f m to the right, wreck at %.0f %% of %.0f m/s",
                        (unsigned long long)f, zd_t.ahead, T / 60.0f, zd_t.miss, zd_t.frac * 100.0f, vm * 60.0f / ZD_UPM);
        }
        zd_t.next = f + (u64)(zd_t.per * 60.0f);
    }
    if (mask <= 0) { ps2_imm_deb_bed[0] = 0.0f; ps2_imm_deb_bed[7] = (float)f; return; }
    /* the listener (head axes: the game's sound listener, turned with the head) and the aircraft's axes */
    float L[12];
    for (u32 i = 0; i < 12; i++) L[i] = f_of(ps2_r32(0x00422C18u + 4u * i));
    float *LP = L, *LF = L + 6, *LU = L + 9, LR[3] = { LF[1] * LU[2] - LF[2] * LU[1], LF[2] * LU[0] - LF[0] * LU[2], LF[0] * LU[1] - LF[1] * LU[0] };
    {   float n = sqrtf(zd_dot(LR, LR)); if (n > 1e-6f) for (int j = 0; j < 3; j++) LR[j] /= n; else { LR[0] = 1.0f; LR[1] = LR[2] = 0.0f; } }
    float pitch = f_of(ps2_r32(fm + 0x3C0u)), yaw = f_of(ps2_r32(fm + 0x3C4u)), roll = f_of(ps2_r32(fm + 0x3C8u));
    float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch), cr = cosf(roll), sr = sinf(roll);
    float art[3] = { cy * cr + sy * sp * sr, cp * sr, -sy * cr + cy * sp * sr };
    float aup[3] = { -sr * cy + cr * sp * sy, cp * cr, sr * sy + cr * sp * cy };
    float afw[3] = { -sy * cp, sp, -cy * cp };
    int ck = ps2_vk_cockpit_view != 0;
    float vE[3];                                               /* the aircraft's velocity, m/s */
    for (int j = 0; j < 3; j++) vE[j] = vok ? VE[j] * 60.0f / ZD_UPM : 0.0f;
    float best = 0.0f, bdir[3] = { 0, 0, 1 }, bdist = 0.0f, bin = 0.0f, bcl = 0.0f;
    const float dt = (float)(dtf >= 1u && dtf <= 4u ? dtf : 1u) / 60.0f;      /* since the last flight frame */
    for (int i = 0; i < ZD_KILLS; i++) {
        zd_kill_t *z = &zd_k[i];
        if (!z->on) continue;
        float t = (float)(f - z->f0) / 60.0f;
        if (t > 8.0f || !vok) { if (t > 8.0f) { zd_summary(z, i); z->on = 0; } continue; }
        zd_wreck(z, f);
        /* (A) the bits' cloud about the kill point, (C) the bits about the wreck: bed and canopy hits */
        float c[3], sig, fade;
        zd_cloud(z, t, c, &sig, &fade);
        for (int src = 0; src < 2; src++) {
            float cc[3], s = src ? 5.0f : sig, fd = src ? (t < 6.0f ? 1.0f : 1.0f - (t - 6.0f) * 0.5f) : fade;
            float NA = (src ? 120.0f : 600.0f) * zd_hits_k * (1.0f + 2.0f * ps2_imm_debris_look());   /* bits x the canopy's area
                                                                         (m^2); ACZ_IMM_DEBRIS_LOOK (bug #260): x (1 + 2 L) */
            float vs[3] = { 0, 0, 0 };
            if (src) { memcpy(cc, z->w, sizeof cc); for (int j = 0; j < 3; j++) vs[j] = z->wv[j] * 60.0f / ZD_UPM; }
            else memcpy(cc, c, sizeof cc);
            if (fd <= 0.0f) continue;
            float r[3], w[3];
            for (int j = 0; j < 3; j++) { r[j] = (E[j] - cc[j]) / ZD_UPM; w[j] = vs[j] - vE[j]; }   /* w: bits relative to us */
            float d2 = zd_dot(r, r), wm = sqrtf(zd_dot(w, w));
            if (!src && sqrtf(d2) < z->dmin) z->dmin = sqrtf(d2);
            if (d2 > 6.25f * s * s) continue;
            float g = expf(-d2 / (2.0f * s * s));
            float n = g / (15.7496f * s * s * s);                  /* (2 pi)^1.5 s^3 */
            float I = g * (src ? 0.6f : fminf(1.0f, powf(10.0f / s, 1.5f))) * fminf(1.0f, wm / (src ? 80.0f : 120.0f)) * fd;
            if (I > best) {
                best = I; bin = g;
                float h[3] = { cc[0] - LP[0], cc[1] - LP[1], cc[2] - LP[2] }, hm;
                bdir[0] = zd_dot(h, LR); bdir[1] = zd_dot(h, LU); bdir[2] = zd_dot(h, LF);
                hm = sqrtf(zd_dot(bdir, bdir));
                if (hm > 1e-3f) for (int j = 0; j < 3; j++) bdir[j] /= hm; else { bdir[0] = bdir[1] = 0.0f; bdir[2] = 1.0f; }
                bdist = hm / ZD_UPM;
                bcl = d2 > 1e-6f ? zd_dot(r, w) / sqrtf(d2) : wm;   /* + = approaching */
            }
            float lam = NA * n * wm * dt * fd;
            z->lam_bits += lam;
            int force = z->syn && zd_t.force > (float)z->forced && !src && d2 < 64.0f && zd_dot(r, w) > 0.0f;   /* (test) */
            if ((z->hits < (ps2_imm_debris_look() > 0.0f ? 6 : 4) && zd_rand(&z->rng) < 1.0f - expf(-lam)) || force) {
                if (force) z->forced++;
                /* where it meets the glass (the 0.5 m^2 above is the canopy's front seen from ahead; bits lower meet the
                   coaming and the nose): from the direction it comes (-w), through a point up to 30 cm to either side
                   and from just under the eye's line to 40 cm above it, 0.75 m off */
                float wn[3] = { -w[0] / (wm + 1e-6f), -w[1] / (wm + 1e-6f), -w[2] / (wm + 1e-6f) };
                float bu[3], ud = zd_dot(aup, wn), bs[3];
                for (int j = 0; j < 3; j++) bu[j] = aup[j] - ud * wn[j];
                {   float n = sqrtf(zd_dot(bu, bu)) + 1e-6f; for (int j = 0; j < 3; j++) bu[j] /= n; }
                bs[0] = wn[1] * bu[2] - wn[2] * bu[1]; bs[1] = wn[2] * bu[0] - wn[0] * bu[2]; bs[2] = wn[0] * bu[1] - wn[1] * bu[0];
                float ks = (zd_rand(&z->rng) - 0.5f) * 0.6f, ku = -0.05f + 0.45f * zd_rand(&z->rng);
                float mw[3];
                for (int j = 0; j < 3; j++) mw[j] = wn[j] * 0.75f + bs[j] * ks + bu[j] * ku;
                float mm = sqrtf(zd_dot(mw, mw)) + 1e-6f;
                for (int j = 0; j < 3; j++) mw[j] /= mm;
                float mc[3] = { zd_dot(mw, art), zd_dot(mw, aup), -zd_dot(mw, afw) };
                zd_glass(mc);
                int seen = mc[2] < 0.35f;                         /* from behind: no fleck in view, the sound stays */
                /* the impact point relative to the head: 0.75 m from the eye (the listener in the cockpit view), else
                   from the aircraft */
                float hp[3], hw[3] = { zd_dot(w, LR), zd_dot(w, LU), zd_dot(w, LF) };
                for (int j = 0; j < 3; j++) hp[j] = (ck ? 0.0f : (E[j] - LP[j]) / ZD_UPM) + mw[j] * 0.75f;
                float hh[3] = { zd_dot(hp, LR), zd_dot(hp, LU), zd_dot(hp, LF) };
                int clank = zd_rand(&z->rng) < 0.2f;
                float st = 0.5f + 0.5f * zd_rand(&z->rng);
                zd_emit(clank ? 4 : 3, zd_rand(&z->rng) * dt, hh, hw, st, seen ? mc : NULL);
                z->hits++;
                if (clank) z->nclank++; else z->nbit++;
                if (zd_dbg > 0)
                    ps2_log("imm-debris: hit field %llu kill %d %s (%s) t %.2f s, %.1f m from the %s (width %.0f m), %.0f m/s, "
                            "strength %.2f, glass az %.0f el %.0f%s", (unsigned long long)f, i, clank ? "clank" : "tick",
                            force ? "forced" : "drawn", t, sqrtf(d2), src ? "wreck" : "cloud's middle", s, wm, st,
                            atan2f(mc[0], -mc[2]) * 57.29578f, asinf(mc[1]) * 57.29578f, seen ? "" : " (behind: no fleck)");
            }
        }
        /* (D) ACZ_IMM_SMOKE_PASS (bug #189): the grit in the explosion's smoke ball about the kill point (static, as the
           game's smoke effects; see ZD_GRIT_N): grit hits on the canopy's front, kind 5, from their own random stream */
        if (t < 6.0f) {
            /* the game's smoke (bug #213, ACZ_IMM_SMOKE_SIZE; zero_vr_smoke_ball): its middle rises over the kill point */
            float rise, rs, dens, sfd, r[3];
            zero_vr_smoke_ball(z->kind, t, &rise, &rs, &dens, &sfd);
            for (int j = 0; j < 3; j++) r[j] = (E[j] - z->p0[j]) / ZD_UPM;
            r[1] -= rise;
            float d2 = zd_dot(r, r), dd = sqrtf(d2), wm = sqrtf(zd_dot(vE, vE));
            if (dd < z->smin) { z->smin = dd; z->tmin = t; z->vmin = wm; z->rmin = rs; }
            int big = zero_vr_smoke_size();
            if (big && dd < rs && sfd > 0.0f) z->tin += dt;
            if (big && zero_vr_smoke_pass() && sfd > 0.0f && dd < 2.0f * rs && wm > 1.0f) {
                /* inside the visible smoke: a soft hiss of grit in the bed (the strongest source wins) */
                float I = 0.3f * dens * sfd * zero_vr_smoke_profile(dd, rs) * fminf(1.0f, wm / 120.0f);
                if (I > best) {
                    best = I; bin = zero_vr_smoke_profile(dd, rs);
                    float cm[3] = { z->p0[0], z->p0[1] + rise * ZD_UPM, z->p0[2] };
                    float h[3] = { cm[0] - LP[0], cm[1] - LP[1], cm[2] - LP[2] }, hm;
                    bdir[0] = zd_dot(h, LR); bdir[1] = zd_dot(h, LU); bdir[2] = zd_dot(h, LF);
                    hm = sqrtf(zd_dot(bdir, bdir));
                    if (hm > 1e-3f) for (int j = 0; j < 3; j++) bdir[j] /= hm; else { bdir[0] = bdir[1] = 0.0f; bdir[2] = 1.0f; }
                    bdist = hm / ZD_UPM;
                    bcl = dd > 1e-3f ? -zd_dot(r, vE) / dd : wm;
                }
            }
            if (zero_vr_smoke_pass() && zd_grit_k > 0.0f && d2 < (big ? 6.25f : 9.0f) * rs * rs && wm > 1.0f && sfd > 0.0f) {
                float lam;
                if (big) {     /* the column through the middle 8 e^-(t - 1) / 2.5 grains per m^2 over 2.094 R (the profile) */
                    float n = dens * 8.0f * expf(-(t - 1.0f) / 2.5f) / (2.094f * rs) * zero_vr_smoke_profile(dd, rs);
                    lam = zd_grit_k * sfd * 0.5f * n * wm * dt;
                } else {
                    float n = expf(-d2 / (2.0f * rs * rs)) / (15.7496f * rs * rs * rs);
                    lam = ZD_GRIT_N * zd_grit_k * expf(-t / 2.5f) * 0.5f * n * wm * dt;
                }
                z->lam_grit += lam;
                /* a Poisson draw (more than one a frame inside a fresh ball: 1.5 at 0.5 s, 200 m/s) */
                int k = 0; float u = zd_rand(&z->grng), pk = expf(-lam), cdf = pk;
                while (u > cdf && k < 4) { k++; pk *= lam / (float)k; cdf += pk; }
                for (int h = 0; h < k; h++) {
                    if (z->grit >= ZD_GRIT_MAX) { z->ngrit_lost++; continue; }
                    /* from ahead along the relative airflow, through a point up to 35 cm to either side and from just
                       under the eye's line to 45 cm above it, 0.75 m off (the windscreen and the canopy's front) */
                    float wn[3] = { vE[0] / wm, vE[1] / wm, vE[2] / wm }, w[3] = { -vE[0], -vE[1], -vE[2] };
                    float bu[3], ud = zd_dot(aup, wn), bs[3];
                    for (int j = 0; j < 3; j++) bu[j] = aup[j] - ud * wn[j];
                    {   float q = sqrtf(zd_dot(bu, bu)) + 1e-6f; for (int j = 0; j < 3; j++) bu[j] /= q; }
                    bs[0] = wn[1] * bu[2] - wn[2] * bu[1]; bs[1] = wn[2] * bu[0] - wn[0] * bu[2]; bs[2] = wn[0] * bu[1] - wn[1] * bu[0];
                    float ks = (zd_rand(&z->grng) - 0.5f) * 0.7f, ku = -0.05f + 0.5f * zd_rand(&z->grng), mw[3];
                    for (int j = 0; j < 3; j++) mw[j] = wn[j] * 0.75f + bs[j] * ks + bu[j] * ku;
                    float mm = sqrtf(zd_dot(mw, mw)) + 1e-6f;
                    for (int j = 0; j < 3; j++) mw[j] /= mm;
                    float mc[3] = { zd_dot(mw, art), zd_dot(mw, aup), -zd_dot(mw, afw) };
                    zd_glass(mc);
                    int seen = mc[2] < 0.35f;
                    float hp[3], hw[3] = { zd_dot(w, LR), zd_dot(w, LU), zd_dot(w, LF) };
                    for (int j = 0; j < 3; j++) hp[j] = (ck ? 0.0f : (E[j] - LP[j]) / ZD_UPM) + mw[j] * 0.75f;
                    float hh[3] = { zd_dot(hp, LR), zd_dot(hp, LU), zd_dot(hp, LF) };
                    float st = 0.3f + 0.7f * zd_rand(&z->grng);
                    zd_emit(5, zd_rand(&z->grng) * dt, hh, hw, st, seen ? mc : NULL);
                    z->grit++;
                    if (zd_dbg > 0)
                        ps2_log("imm-debris: grit field %llu kill %d t %.2f s, %.1f m from the smoke ball's middle (radius %.1f "
                                "m), %.0f m/s, strength %.2f, glass az %.0f el %.0f%s", (unsigned long long)f, i, t, dd, rs, wm,
                                st, atan2f(mc[0], -mc[2]) * 57.29578f, asinf(mc[1]) * 57.29578f, seen ? "" : " (behind: no mark)");
                }
            }
        }
        /* (B) the chunks and the wreck: zips (and a clank when one comes within 1.2 m) */
        for (int k = 0; k <= ZD_CHUNKS; k++) {
            if (z->zip[k]) continue;
            float x[3], v[3];
            if (k < ZD_CHUNKS) { if (t > 6.0f) continue; zd_chunk(z, k, t, x, v); }
            else { memcpy(x, z->w, sizeof x); for (int j = 0; j < 3; j++) v[j] = z->wv[j] * 60.0f / ZD_UPM; }
            float p[3], w[3];
            for (int j = 0; j < 3; j++) { p[j] = (x[j] - E[j]) / ZD_UPM; w[j] = v[j] - vE[j]; }
            float w2 = zd_dot(w, w);
            if (w2 < 1.0f) continue;
            float ts = -zd_dot(p, w) / w2, q[3];
            for (int j = 0; j < 3; j++) q[j] = p[j] + w[j] * ts;
            float ds = sqrtf(zd_dot(q, q)), wm = sqrtf(w2), R = k < ZD_CHUNKS ? 25.0f : 40.0f;
            float win = fminf(1.5f, fmaxf(0.35f, 80.0f / wm));
            if (ts < -0.1f || ts > win || ds > R) continue;
            z->zip[k] = 1;
            float st = (1.0f - ds / R) * (1.0f - ds / R) * fminf(1.0f, wm / (k < ZD_CHUNKS ? 150.0f : 80.0f));
            if (st < 0.02f && !(k < ZD_CHUNKS && ds < 1.2f)) continue;     /* (inaudible: not sent) */
            float h[3] = { x[0] - LP[0], x[1] - LP[1], x[2] - LP[2] };
            float hp[3] = { zd_dot(h, LR) / ZD_UPM, zd_dot(h, LU) / ZD_UPM, zd_dot(h, LF) / ZD_UPM };
            float hw[3] = { zd_dot(w, LR), zd_dot(w, LU), zd_dot(w, LF) };
            zd_emit(k < ZD_CHUNKS ? 1 : 2, 0.0f, hp, hw, st, NULL);
            z->nzip++;
            if (zd_dbg > 0)
                ps2_log("imm-debris: zip field %llu kill %d %s %d t %.2f s: passes %.1f m in %.2f s at %.0f m/s, strength %.2f, "
                        "head az %.0f el %.0f", (unsigned long long)f, i, k < ZD_CHUNKS ? "chunk" : "wreck", k, t, ds, ts, wm, st,
                        atan2f(hp[0], hp[2]) * 57.29578f, atan2f(hp[1], sqrtf(hp[0] * hp[0] + hp[2] * hp[2])) * 57.29578f);
            if (k < ZD_CHUNKS && ds < 1.2f && ts >= 0.0f && z->hits < 6) {   /* it meets the canopy */
                float wn[3] = { -w[0] / wm, -w[1] / wm, -w[2] / wm };
                float mc[3] = { zd_dot(wn, art), zd_dot(wn, aup), -zd_dot(wn, afw) };
                zd_glass(mc);
                float hq[3];
                for (int j = 0; j < 3; j++) hq[j] = (ck ? 0.0f : (E[j] - LP[j]) / ZD_UPM) + wn[j] * 0.75f;
                float hh[3] = { zd_dot(hq, LR), zd_dot(hq, LU), zd_dot(hq, LF) };
                zd_emit(4, ts, hh, hw, 0.8f, mc[2] < 0.35f ? mc : NULL);
                z->hits++; z->nclank++;
                if (zd_dbg > 0) ps2_log("imm-debris: hit field %llu kill %d clank (chunk %d) in %.2f s", (unsigned long long)f, i, k, ts);
            }
        }
        if (zd_dbg > 0 && f % (u64)zd_dbg == 0u && (z->dmin < 400.0f || zd_range > 1500.0f))
            ps2_log("imm-debris: field %llu kill %d t %.2f s: %.0f m from the cloud's middle (width %.1f m, closest %.0f m), "
                    "wreck %.0f m at %.0f m/s, hits %d", (unsigned long long)f, i, t,
                    sqrtf(((E[0] - c[0]) * (E[0] - c[0]) + (E[1] - c[1]) * (E[1] - c[1]) + (E[2] - c[2]) * (E[2] - c[2]))) / ZD_UPM,
                    sig, z->dmin, sqrtf((E[0] - z->w[0]) * (E[0] - z->w[0]) + (E[1] - z->w[1]) * (E[1] - z->w[1])
                                        + (E[2] - z->w[2]) * (E[2] - z->w[2])) / ZD_UPM,
                    sqrtf(zd_dot(z->wv, z->wv)) * 60.0f / ZD_UPM, z->hits);
    }
    ps2_imm_deb_bed[1] = bdir[0]; ps2_imm_deb_bed[2] = bdir[1]; ps2_imm_deb_bed[3] = bdir[2];
    ps2_imm_deb_bed[4] = bdist; ps2_imm_deb_bed[5] = bin; ps2_imm_deb_bed[6] = bcl;
    ps2_imm_deb_bed[0] = best > 1.0f ? 1.0f : best;
    ps2_imm_deb_bed[7] = (float)f;
    fxk_publish(f);
    if (zd_dbg > 0 && best > 0.02f && f % 6u == 0u)
        ps2_log("imm-debris: bed field %llu intensity %.2f inside %.2f dist %.0f m closing %.0f m/s dir %.2f %.2f %.2f",
                (unsigned long long)f, best, bin, bdist, bcl, bdir[0], bdir[1], bdir[2]);
}
/* (bug #139, zero_cracks.c's soot from the smoke) the kills the debris model follows now: kill point and wreck (game
   units, 3 floats each) and age (s); EE thread, read only */
int zero_vr_debris_kills(float *p0, float *w, float *age, int max) {
    int n = 0;
    u64 f = ps2_kernel_vblank_count();
    for (int i = 0; i < ZD_KILLS && n < max; i++) {
        const zd_kill_t *z = &zd_k[i];
        if (!z->on) continue;
        memcpy(p0 + 3 * n, z->p0, 12); memcpy(w + 3 * n, z->w, 12); age[n] = (float)(f - z->f0) / 60.0f;
        n++;
    }
    return n;
}
/* ---- the game's own explosion (pt_fx, playtest 2026-10-29 11:02:13, bug #213) ----
   A kill's explosion is effect type 4, made by 0x1FC880 through 0x17C240: a0 the effect manager, a1 4, a2 a parameter
   block -- the unit's position +0x260 (x, y, z, w = 1) and at +0x10 the explosion's kind (0x1FC880's a1, + 3 when
   0x35E9D8(unit) is 0) -- and a3 0; its first update spawns the 0x180300 set (types 79/66/64/69/83/64/64/64, wx_debris).
   fxk_enter keeps the kind of each followed kill's explosion (matched by its position: the call comes from inside
   0x1FC880, after destroy_enter made the kill) and the manager for the test below. ps2_fx_kills: the followed kills
   for the renderer (ps2_video.c ACZ_DEBUG_FX_EXTENT): [0..2] kill point (units), [3] age (s), [4] kind; written each
   flight frame, ps2_fx_kills_n of them. */
static u32 z_fxm;
volatile float ps2_fx_kills[ZD_KILLS][5];
volatile u32 ps2_fx_kills_n;
static int fxk_enter(ps2_ctx *ctx, void *user) {
    (void)user;
    u32 m = ctx->r[4].uw[0], p = ctx->r[6].uw[0];
    if (m >= 0x100000u && m < 0x2000000u) z_fxm = m;
    if (p < 0x100000u || p >= 0x2000000u) return 0;
    int ak = zd_airkill_kind(ctx->r[5].uw[0] & 0xFFFFu, ctx->r[31].uw[0], p);     /* (bug #260: an air kill's) */
    if ((ctx->r[5].uw[0] & 0xFFFFu) != 4u && ak < 0) return 0;
    float q[3];
    for (u32 i = 0; i < 3; i++) q[i] = f_of(ps2_r32(p + 4u * i));
    u32 kind = ak >= 0 ? (u32)ak : ps2_r32(p + 0x10u) & 0xFFu;
    u64 f = ps2_kernel_vblank_count();
    for (int i = 0; i < ZD_KILLS; i++) {
        zd_kill_t *z = &zd_k[i];
        if (!z->on || f - z->f0 > 2u) continue;
        float d2 = 0.0f;
        for (int j = 0; j < 3; j++) d2 += (q[j] - z->p0[j]) * (q[j] - z->p0[j]);
        if (d2 < 4.0f) { z->kind = (int)kind; break; }
    }
    if (zd_dbg > 0)
        ps2_log("imm-debris: explosion field %llu kind %u at %.0f %.0f %.0f (w %.3g) ra %08X", (unsigned long long)f, kind,
                q[0] / ZD_UPM, q[1] / ZD_UPM, q[2] / ZD_UPM, f_of(ps2_r32(p + 12u)), ctx->r[31].uw[0]);
    return 0;
}
static void fxk_publish(u64 f) {
    u32 n = 0;
    for (int i = 0; i < ZD_KILLS; i++) {
        const zd_kill_t *z = &zd_k[i];
        if (!z->on) continue;
        for (int j = 0; j < 3; j++) ps2_fx_kills[n][j] = z->p0[j];
        ps2_fx_kills[n][3] = (float)(f - z->f0) / 60.0f;
        ps2_fx_kills[n][4] = (float)z->kind;
        n++;
    }
    __atomic_store_n(&ps2_fx_kills_n, n, __ATOMIC_RELEASE);
}
/* ACZ_DEBUG_FX_BOOM=ahead_m[,miss_m[,kind[,period_s[,first_field[,ground[,up_m]]]]]] (test): the game's own kill explosion --
   effect type 4 of that kind (default 0), as 0x1FC880 makes it -- on the aircraft's straight path ahead_m ahead and
   miss_m to the right (ground = 1: on the ground under that point, from the game's height query 0x158AE0), every
   period_s (default 10) from first_field (default 900); with a debris-model test kill at the same point and time (its
   wreck stays there). For measuring the game's explosion (ps2_video.c ACZ_DEBUG_FX_EXTENT), the look A/Bs of the
   smoke shading and passes through it. The game's functions are called from the HUD manager's hook (instr_hook) with
   a copy of the whole context put back afterwards; parameters on the stack below the caller's. */
static struct { int on, kind, ground; float ahead, miss, per, up; u64 next; u32 ter; float lE[3]; u64 lf; } zb;
static int zb_ground_enter(ps2_ctx *ctx, void *user) {
    (void)user;
    u32 a = ctx->r[4].uw[0];
    if (a >= 0x100000u && a < 0x2000000u) zb.ter = a;
    return 0;
}
static ps2_ctx zb_save;
static int zb_call(ps2_ctx *ctx, u32 addr, u32 a0, u32 a1, const u32 *blk, int nblk, float f12, float f13, float *f0) {
    ps2_fn fn = ps2_dispatch_lookup(addr);
    if (!fn) return 0;
    memcpy(&zb_save, ctx, sizeof zb_save);
    u32 sp = (ctx->r[29].uw[0] - 0x400u) & ~15u, b = sp + 0x100u;
    for (int i = 0; i < nblk; i++) ps2_w32(b + 4u * (u32)i, blk[i]);
    ctx->r[29].sd[0] = (s64)(s32)sp;
    ctx->r[4].sd[0] = (s64)(s32)a0; ctx->r[5].sd[0] = (s64)(s32)a1;
    ctx->r[6].sd[0] = (s64)(s32)(nblk ? b : 0u); ctx->r[7].sd[0] = 0;
    ctx->f[12].f = f12; ctx->f[13].f = f13;
    ctx->r[31].sd[0] = (s64)(s32)0x001FC930u;
    fn(ctx);
    if (f0) *f0 = ctx->f[0].f;
    memcpy(ctx, &zb_save, sizeof zb_save);
    return 1;
}
static u32 zb_u(float v) { u32 w; memcpy(&w, &v, 4); return w; }
static void zb_update(ps2_ctx *ctx, u32 player, u64 f) {
    u32 fm = player + 0xEF0u;
    float E[3];
    for (u32 i = 0; i < 3; i++) E[i] = f_of(ps2_r32(fm + 0x3B0u + 4u * i));
    float V[3] = { 0, 0, 0 }; int vok = 0;
    if (zb.lf && f > zb.lf && f - zb.lf < 4u) {
        for (int j = 0; j < 3; j++) V[j] = (E[j] - zb.lE[j]) / (float)(f - zb.lf);
        vok = 1;
    }
    memcpy(zb.lE, E, sizeof zb.lE); zb.lf = f;
    if (!vok || f < zb.next || !z_fxm) return;
    float vm = sqrtf(zd_dot(V, V));
    if (!(vm > 0.5f)) return;
    zb.next = f + (u64)(zb.per * 60.0f);
    float yaw = f_of(ps2_r32(fm + 0x3C4u)), roll = f_of(ps2_r32(fm + 0x3C8u)), pitch = f_of(ps2_r32(fm + 0x3C0u));
    float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch), cr = cosf(roll), sr = sinf(roll);
    float rt[3] = { cy * cr + sy * sp * sr, cp * sr, -sy * cr + cy * sp * sr };
    float p[3];
    for (int j = 0; j < 3; j++) p[j] = E[j] + V[j] / vm * zb.ahead * ZD_UPM + rt[j] * zb.miss * ZD_UPM;
    p[1] += zb.up * ZD_UPM;
    float gy = 0.0f;
    if (zb.ground) {
        if (!zb.ter || !zb_call(ctx, 0x00158AE0u, zb.ter, 0u, NULL, 0, p[0], p[2], &gy) || !(gy > -1e6f && gy < 1e6f)) {
            ps2_log("zero-vr: ACZ_DEBUG_FX_BOOM: no ground height yet (field %llu)", (unsigned long long)f);
            return;
        }
        p[1] = gy;
    }
    static int nb;
    int kind = zb.kind >= 0 ? zb.kind : nb % 6;                 /* (kind -1: 0..5 in turn) */
    nb++;
    /* (bug #260) kind 100 + n / 200 + n / 300 + n: an air kill's explosion as 0x1FC880 makes it for a weapon's kill --
       effect type 3 / 54 / 58 with the parameter n (1 for the aircraft seen); the debris model's kind as zd_airkill_kind */
    u32 ftype = kind >= 300 ? 58u : kind >= 200 ? 54u : kind >= 100 ? 3u : 4u;
    u32 blk[5] = { zb_u(p[0]), zb_u(p[1]), zb_u(p[2]), zb_u(1.0f), (u32)(kind >= 100 ? kind % 100 : kind) };
    float sv[3] = { 0, 0, 0 };
    zd_kill_t *z = ps2_imm_debris_mask() >= 0 ? zd_add(0u, p, 1, sv, f) : NULL;
    if (z) { z->dist0 = zb.ahead; z->kind = kind; }
    int ok = zb_call(ctx, 0x0017C240u, z_fxm, ftype, blk, 5, 0.0f, 0.0f, NULL);
    ps2_log("zero-vr: ACZ_DEBUG_FX_BOOM: field %llu explosion kind %d %s at %.0f m ahead, %.0f m right%s (%.0f %.0f %.0f m; "
            "the aircraft %.0f m up, %.0f m/s)", (unsigned long long)f, kind, ok ? "made" : "NOT made", zb.ahead, zb.miss,
            zb.ground ? ", on the ground" : "", p[0] / ZD_UPM, p[1] / ZD_UPM, p[2] / ZD_UPM, E[1] / ZD_UPM, vm * 60.0f / ZD_UPM);
}
/* ACZ_DEBUG_UNIT_KILL=field:unit[,field:unit...] (debug, fx_mpbm 2026-10-31; changes the game): at the first flight
   frame at or after the field, the unit record (hex address) takes 1000 points of damage through the game's own damage
   function 0x1FCD28, called as the game calls it for a crash (0x2326C0: a1 a zeroed record, a2 0, kind 5, no attacker,
   mode 1): its own kill path follows (hp <= 0 -> 0x1FC880, the score, the mission's script). Used to reach M18's second
   phase in a test run (Pixy's first form, unit 014AD400 in the debug boot: the boss's bursts follow ~12 s later). The
   unit's hp (s16 +0x398) must be 1..30000 and the invincible bit (+0x1F8 0x8000) clear, else it is left alone.
   field:unit:m = as a hit of the player's missile instead (kind 1, mode 3, the player's unit the attacker); field:unit:hN
   = only its hp set to N (the next real hit kills it through the game's own path).
   field:unit:d (fx_burst 2026-11-02, bug #266) = the damage function's own kill branch alone: hp back to 100 and the
   destroy call 0x1FC880 (a1 = mode 3, as a missile kill) -- the debug damage never reached that branch for M18's first
   form (hp left at -900, no destroy: the boss's second phase and its MPBM bursts did not follow). */
static struct { int n, done; u64 f[4]; u32 u[4]; int kind[4], mode[4], by[4], dk[4]; } zuk;
static void zuk_update(ps2_ctx *ctx, u64 f) {
    static int init;
    if (!init) {
        init = 1;
        const char *e = getenv("ACZ_DEBUG_UNIT_KILL");
        while (e && *e && zuk.n < 4) {
            char *q;
            unsigned long long ff = strtoull(e, &q, 0);
            if (*q != ':') break;
            zuk.f[zuk.n] = ff; zuk.u[zuk.n] = (u32)strtoul(q + 1, &q, 16);
            zuk.kind[zuk.n] = 5; zuk.mode[zuk.n] = 1; zuk.by[zuk.n] = 0;
            if (*q == ':' && (q[1] == 'm' || q[1] == 'k')) {   /* ":m": as the player's missile (kind 1, mode 3); ":k" the
                                                                  same with mode 0 (the amount as it is) */
                zuk.kind[zuk.n] = 1; zuk.mode[zuk.n] = q[1] == 'm' ? 3 : 0; zuk.by[zuk.n] = 1; q += 2;
            } else if (*q == ':' && q[1] == 'h') {             /* ":hN": its hp set to N (the next real hit kills it) */
                zuk.kind[zuk.n] = -(int)strtol(q + 2, &q, 10);
            } else if (*q == ':' && q[1] == 'd') {             /* ":d": the kill branch's destroy call alone */
                zuk.dk[zuk.n] = 1; q += 2;
            }
            zuk.n++;
            e = *q == ',' ? q + 1 : q;
        }
        if (zuk.n) ps2_log("zero-vr: debug: %d unit kill(s) through the game's damage path (ACZ_DEBUG_UNIT_KILL)", zuk.n);
    }
    for (int i = 0; i < zuk.n; i++) {
        if ((zuk.done >> i) & 1 || f < zuk.f[i]) continue;
        zuk.done |= 1 << i;
        u32 u = zuk.u[i];
        int hp = u >= 0x100000u && u < 0x2000000u ? (int)(s16)ps2_r16(u + 0x398u) : -99999;
        u32 fl = u >= 0x100000u && u < 0x2000000u ? ps2_r32(u + 0x1F8u) : 0u;
        ps2_fn fn = ps2_dispatch_lookup(0x001FCD28u);
        if (zuk.dk[i]) {                                 /* (":d": hp 100 and 0x1FC880 as the kill branch at 0x1FD00C) */
            ps2_fn fd = ps2_dispatch_lookup(0x001FC880u);
            if (!fd || hp < -30000 || hp > 30000) {
                ps2_log("zero-vr: ACZ_DEBUG_UNIT_KILL: unit %08X left alone at field %llu (hp %d)", u, (unsigned long long)f, hp);
                continue;
            }
            memcpy(&zb_save, ctx, sizeof zb_save);
            ctx->r[29].sd[0] = (s64)(s32)((ctx->r[29].uw[0] - 0x400u) & ~15u);
            ps2_w16(u + 0x398u, (u16)100);
            ctx->r[4].sd[0] = (s64)(s32)u; ctx->r[5].sd[0] = 3;
            ctx->r[31].sd[0] = (s64)(s32)0x001FD014u;
            fd(ctx);
            memcpy(ctx, &zb_save, sizeof zb_save);
            ps2_log("zero-vr: ACZ_DEBUG_UNIT_KILL: unit %08X at field %llu: destroyed directly (hp %d -> %d, flags %08X)", u,
                    (unsigned long long)f, hp, (int)(s16)ps2_r16(u + 0x398u), ps2_r32(u + 0x1F8u));
            continue;
        }
        if (zuk.kind[i] <= 0 && hp >= 1 && hp <= 30000) {
            ps2_w16(u + 0x398u, (u16)(s16)(-zuk.kind[i] > 0 ? -zuk.kind[i] : 1));
            ps2_log("zero-vr: ACZ_DEBUG_UNIT_KILL: unit %08X at field %llu: hp %d -> %d (set)", u, (unsigned long long)f, hp,
                    (int)(s16)ps2_r16(u + 0x398u));
            continue;
        }
        if (!fn || hp < 1 || hp > 30000 || (fl & 0x8000u)) {
            ps2_log("zero-vr: ACZ_DEBUG_UNIT_KILL: unit %08X left alone at field %llu (hp %d, flags %08X)", u,
                    (unsigned long long)f, hp, fl);
            continue;
        }
        memcpy(&zb_save, ctx, sizeof zb_save);
        u32 sp = (ctx->r[29].uw[0] - 0x400u) & ~15u, rec = sp + 0x1E8u;
        ps2_w32(rec, 0u);
        ctx->r[29].sd[0] = (s64)(s32)sp;
        ctx->r[4].sd[0] = (s64)(s32)u; ctx->r[5].sd[0] = (s64)(s32)rec; ctx->r[6].sd[0] = 0;
        ctx->r[7].sd[0] = 1000; ctx->r[8].sd[0] = zuk.kind[i];
        ctx->r[9].sd[0] = zuk.by[i] ? (s64)(s32)z_imm_player : 0; ctx->r[10].sd[0] = zuk.mode[i];
        ctx->r[31].sd[0] = (s64)(s32)0x002326E0u;
        fn(ctx);
        memcpy(ctx, &zb_save, sizeof zb_save);
        ps2_log("zero-vr: ACZ_DEBUG_UNIT_KILL: unit %08X at field %llu: hp %d -> %d", u, (unsigned long long)f, hp,
                (int)(s16)ps2_r16(u + 0x398u));
    }
}
static void zb_install(void) {
    const char *e = getenv("ACZ_DEBUG_FX_BOOM");
    int hk = ps2_r32(0x0017C240u) >> 16 == 0x27BDu && ps2_hook_before(0x0017C240u, fxk_enter, NULL, 59, "zero-fx") >= 0;
    if (!e || !*e) return;
    unsigned long long ff = 900;
    zb.ahead = 300.0f; zb.miss = 0.0f; zb.kind = 0; zb.per = 10.0f; zb.ground = 0;
    int n = sscanf(e, "%f,%f,%d,%f,%llu,%d,%f", &zb.ahead, &zb.miss, &zb.kind, &zb.per, &ff, &zb.ground, &zb.up);
    if (n < 1 || !hk || !(zb.per >= 1.0f)) { ps2_log("zero-vr: ACZ_DEBUG_FX_BOOM ignored"); return; }
    if (zb.ground && !(ps2_r32(0x00158AE0u) == 0x3C0E003Fu
                       && ps2_hook_before(0x00158AE0u, zb_ground_enter, NULL, 81, "zero-fx") >= 0)) zb.ground = 0;
    zb.on = 1; zb.next = ff;
    ps2_log("zero-vr: debug: the game's explosion (kind %d) %.0f m ahead, %.0f m right%s, every %.1f s from field %llu "
            "(ACZ_DEBUG_FX_BOOM)", zb.kind, zb.ahead, zb.miss, zb.ground ? ", on the ground" : "", zb.per, ff);
}
static void imm_vib_note(int small, int large) {
    static int ps = -1, pl = -1;
    if (z_imm_dbg > 0 && (small != ps || large != pl))
        ps2_log("immerse: vib field %llu small %d large %d", (unsigned long long)ps2_kernel_vblank_count(), small, large);
    ps = small; pl = large;
}
/* player struct words that change rarely (damage candidates): change counts over the run */
#define Z_PL_WORDS 0x800u
static u32 z_pl_prev[Z_PL_WORDS], z_pl_first[Z_PL_WORDS];
static u16 z_pl_cnt[Z_PL_WORDS];
static u32 z_pl_base;
static void imm_player_scan(u32 player, u64 f) {
    if (player != z_pl_base) {
        for (u32 i = 0; i < Z_PL_WORDS; i++) z_pl_prev[i] = z_pl_first[i] = ps2_r32(player + 4u * i);
        memset(z_pl_cnt, 0, sizeof z_pl_cnt);
        z_pl_base = player;
        ps2_log("immerse: player struct %08X", player);
        return;
    }
    for (u32 i = 0; i < Z_PL_WORDS; i++) {
        u32 w = ps2_r32(player + 4u * i);
        if (w != z_pl_prev[i]) {
            if (z_pl_cnt[i] < 65535u) z_pl_cnt[i]++;
            if (z_pl_cnt[i] <= 3u && i * 4u != 0x1AA8u)
                ps2_log("immerse: player +%04X field %llu %08X -> %08X (%g -> %g) n %u", i * 4u, (unsigned long long)f,
                        z_pl_prev[i], w, f_of(z_pl_prev[i]), f_of(w), z_pl_cnt[i]);
            z_pl_prev[i] = w;
        }
    }
}
static void imm_log(u32 player, u64 f, float alt, float spd, float g) {
    if (z_imm_dbg < 0) { const char *e = getenv("ACZ_DEBUG_IMMERSE"); z_imm_dbg = e ? atoi(e) : 0; }
    if (z_imm_dbg <= 0) return;
    for (u32 i = 0; i < 3; i++) z_imm_ppos[i] = f_of(ps2_r32(player + 0xEF0u + 0x3B0u + 4u * i));
    {   static int hp_last = -99999;
        int hp = z_imm_player ? (s16)ps2_r16(z_imm_player + 0x398u) : -1;
        if (hp != hp_last) ps2_log("immerse: hp field %llu %d -> %d (warn byte %d) unit %08X pos %.0f %.0f %.0f", (unsigned long long)f,
                                   hp_last, hp, (int)(s8)ps2_r8(player + 0x1ACFu), z_imm_player,
                                   z_imm_player ? f_of(ps2_r32(z_imm_player + 0x260u)) : 0.0f,
                                   z_imm_player ? f_of(ps2_r32(z_imm_player + 0x264u)) : 0.0f,
                                   z_imm_player ? f_of(ps2_r32(z_imm_player + 0x268u)) : 0.0f);
        hp_last = hp;
    }
    if (f > 600u) imm_player_scan(player, f);
    if (f % (u64)z_imm_dbg) return;
    extern volatile float ps2_vk_imm_cloud[8];
    extern volatile int ps2_vk_cockpit_view;
    char sky[300] = "";
    u32 s = z_imm_sky;
    if (s >= 0x100000u && s < 0x2000000u) {
        u32 sc = ps2_r32(s + 0xBB0u), env = ps2_r32(s + 0x4u);
        float camy = sc >= 0x100000u && sc < 0x2000000u ? f_of(ps2_r32(sc + 0x14u)) : -1.0f;
        int ok = env >= 0x100000u && env < 0x2000000u;
        if (sc >= 0x100000u && sc < 0x2000000u)
            ps2_log("immerse: eye %.0f %.0f %.0f player %.0f %.0f %.0f", f_of(ps2_r32(sc + 0x10u)), f_of(ps2_r32(sc + 0x14u)),
                    f_of(ps2_r32(sc + 0x18u)), z_imm_ppos[0], z_imm_ppos[1], z_imm_ppos[2]);
        snprintf(sky, sizeof sky, "sky camy %.0f band %.0f..%.0f t %.3f pr %.3f %.3f %.3f %.3f %08X h %d env %.3f/%.3f %.3f/%.3f",
                 camy, f_of(ps2_r32(s + 0x10u)), f_of(ps2_r32(s + 0x14u)), f_of(ps2_r32(s + 0xB30u)),
                 f_of(ps2_r32(s + 0xB8Cu)), f_of(ps2_r32(s + 0xB90u)), f_of(ps2_r32(s + 0xB94u)), f_of(ps2_r32(s + 0xB98u)),
                 ps2_r32(s + 0xB9Cu), (int)(s16)ps2_r16(s + 0xB88u),
                 ok ? f_of(ps2_r32(env + 0x84u)) : 0.0f, ok ? f_of(ps2_r32(env + 0x124u)) : 0.0f,
                 ok ? f_of(ps2_r32(env + 0x88u)) : 0.0f, ok ? f_of(ps2_r32(env + 0x128u)) : 0.0f);
    }
    if (s >= 0x100000u && s < 0x2000000u) {
        static u32 env_seen;
        u32 env = ps2_r32(s + 0x4u);
        if (env != env_seen && env >= 0x100000u && env < 0x2000000u) {
            env_seen = env;
            ps2_log("immerse: stage %d env %08X (sky %08X)", (int)(s16)ps2_r16(0x003BF740u), env, s);
            for (u32 o = 0; o < 0x1C0u; o += 0x20u) {
                char ln[400]; int k = 0;
                k += snprintf(ln + k, sizeof ln - (size_t)k, "immerse:   env+%03X", o);
                for (u32 i = 0; i < 8u; i++) {
                    u32 w = ps2_r32(env + o + 4u * i);
                    float fv = f_of(w);
                    if (fv != 0.0f && fabsf(fv) > 1e-6f && fabsf(fv) < 1e7f) k += snprintf(ln + k, sizeof ln - (size_t)k, " %.4g", fv);
                    else k += snprintf(ln + k, sizeof ln - (size_t)k, " %08X", w);
                }
                ps2_log("%s", ln);
            }
        }
    }
    const z_cloud_acc *a = &z_cl_last;
    ps2_log("immerse: field %llu alt %.0f spd %.0f g %.2f ck %d | %s | clouds n %u vis %u <2k %u <4k %u <8k %u w %.2f dmin %.0f "
            "dy %.0f size %.0f near %.0f y %.0f..%.0f cam %.0f %.0f %.0f gmin %.0f | gs n %.0f top %.3f bot %.3f a3000 %.3f all %.2f near %.0f m a %.2f",
            (unsigned long long)f, alt, spd, g, ps2_vk_cockpit_view, sky, a->n, a->vis,
            a->n2k, a->n4k, a->n8k, a->w, a->dmin, a->dmin_dy, a->dmin_size, a->near, a->ymin, a->ymax, a->cam[0],
            a->cam[1], a->cam[2], a->gmin, ps2_vk_imm_cloud[0], ps2_vk_imm_cloud[1], ps2_vk_imm_cloud[2], ps2_vk_imm_cloud[3],
            ps2_vk_imm_cloud[4], ps2_vk_imm_cloud[5], ps2_vk_imm_cloud[6]);
}
static void imm_install(void) {
    const char *on = getenv("ACZ_IMMERSE");
    if (on && *on == '0') { ps2_log("zero-vr: immersion signals off (ACZ_IMMERSE=0)"); return; }
    int b = ps2_r32(0x001604A0u) >> 16 == 0x27BDu
            && ps2_hook_before(0x001604A0u, sky_update_enter, NULL, 60, "zero-immerse") >= 0;
    int x = ps2_r32(0x001FC880u) >> 16 == 0x27BDu
            && ps2_hook_before(0x001FC880u, destroy_enter, NULL, 60, "zero-immerse") >= 0;
    ps2_log("zero-vr: immersion signals: precipitation %d, destroyed units %d (ACZ_IMMERSE=0 turns them off)", b, x);
    {   /* debris passes (imm_debris) */
        const char *pe = getenv("ACZ_DEBUG_IMM_DEBRIS_FX");
        if (pe && *pe == '1' && ps2_r32(0x0017C240u) >> 16 == 0x27BDu)
            ps2_hook_before(0x0017C240u, fx_mgr_enter, NULL, 60, "zero-immerse");
        pe = getenv("ACZ_DEBUG_IMM_DEBRIS"); zd_dbg = pe ? atoi(pe) : 0;
        if (zd_dbg < 0) zd_dbg = 0;
        pe = getenv("ACZ_DEBUG_IMM_DEBRIS_RANGE");
        if (pe && *pe) { zd_range = (float)atof(pe); if (!(zd_range > 0.0f)) zd_range = 1500.0f; }
        pe = getenv("ACZ_IMM_DEBRIS_HITS");
        if (pe && *pe) { zd_hits_k = (float)atof(pe); if (!(zd_hits_k >= 0.0f && zd_hits_k <= 20.0f)) zd_hits_k = 1.0f; }
        pe = getenv("ACZ_IMM_DEBRIS_TEST");
        if (pe && *pe) {
            unsigned long long ff = 900;
            zd_t.ahead = 150.0f; zd_t.miss = 2.0f; zd_t.per = 10.0f; zd_t.frac = 0.7f; zd_t.force = 0.0f;
            int n = sscanf(pe, "%f,%f,%f,%f,%llu,%f", &zd_t.ahead, &zd_t.miss, &zd_t.per, &zd_t.frac, &ff, &zd_t.force);
            if (n >= 1 && zd_t.ahead > 1.0f && zd_t.per >= 2.0f) { zd_t.on = 1; zd_t.first = zd_t.next = ff; }
        }
        pe = getenv("ACZ_IMM_SMOKE_GRIT");
        if (pe && *pe) { zd_grit_k = (float)atof(pe); if (!(zd_grit_k >= 0.0f && zd_grit_k <= 5.0f)) zd_grit_k = zd_grit_k > 5.0f ? 5.0f : 0.0f; }
        pe = getenv("ACZ_IMM_DEBRIS_LOG"); zd_log = !(pe && *pe == '0');
        zb_install();
        zbl_install();
        int m = ps2_imm_debris_mask();
        ps2_log("zero-vr: debris passes: %s%s%s%s (ACZ_IMM_DEBRIS)%s", m < 0 ? "none" : m == 0 ? "old 90 m strike" : "",
                m > 0 && (m & 1) ? "sound " : "", m > 0 && (m & 2) ? "glass " : "", m > 0 && (m & 4) ? "rumble" : "",
                zd_t.on ? "; test kills on the path (ACZ_IMM_DEBRIS_TEST)" : "");
        if (m > 0)
            ps2_log("zero-vr: kills' smoke passes: %s (ACZ_IMM_SMOKE_PASS; grit x %.2f, ACZ_IMM_SMOKE_GRIT); a line per kill "
                    "followed %s (ACZ_IMM_DEBRIS_LOG)", zero_vr_smoke_pass() ? "grit ticks, chips and soot from the smoke ball"
                    : "as before", zd_grit_k, zd_log ? "on" : "off");
    }
    {   const char *se = getenv("ACZ_DEBUG_CFIELD_STEER"), *de = getenv("ACZ_DEBUG_CFIELD");
        z_st.on = se && *se == '1';
        z_st.dbg = de ? atoi(de) : 0;
        if (z_st.on) { extern int (*ps2_pad_test_steer)(u8 *, u8 *); ps2_pad_test_steer = cf_steer; }
    }
    const char *e = getenv("ACZ_DEBUG_IMMERSE");
    if (!e || atoi(e) <= 0) return;
    z_imm_dbg = atoi(e);
    int a = ps2_r32(0x0016A2F0u) >> 16 == 0xD889u
            && ps2_hook_before(0x0016A2F0u, cloud_proj_enter, NULL, 60, "zero-immerse") >= 0
            && ps2_hook_after(0x0016A2F0u, cloud_proj_leave, NULL, 60, "zero-immerse") >= 0;
    int c = ps2_r32(0x0017C240u) >> 16 == 0x27BDu
            && ps2_hook_before(0x0017C240u, fx_create_enter, NULL, 60, "zero-immerse") >= 0;
    int d = ps2_r32(0x001FCD28u) >> 16 == 0x27BDu
            && ps2_hook_before(0x001FCD28u, damage_enter, NULL, 60, "zero-immerse") >= 0;
    ps2_log("zero-vr: immersion probe: clouds %d, sky %d, fx %d, damage %d", a, b, c, d);
}
/* ---- Flashes that light the cockpit and our clouds (wish #83; ACZ_VR_FLASH_LIGHT; ps2_video.c flash_*) ----
   Event readers on the EE thread; the renderer turns the events into short light pulses (positions, colours,
   envelopes). Nothing in the game changes: positions and counts are read only.
   - own launches: the missile count (u8 player+0x1AA9) or the special weapon count (u8 +0x1AAA) dropping between two
     flight frames (instr_hook) -> FL_OWN_LAUNCH; the rail alternates with the count's parity (the game fires from
     alternate pylons);
   - own gun: its muzzle effect (type 36 within 25 m, below) or the gun count (u16 +0x1A9E) dropping -> firing, held
     8 fields after the last muzzle flash (ps2_vk_flash_cont[1]); afterburner FM+0x400 (0..1) -> ps2_vk_flash_cont[0];
     [2] = the field stamp (a stale stamp = paused / menus: the renderer lets the own sources go dark);
   - kills: 0x1FC880 (unit destroyed: a0 = the unit, position +0x260; the first call per unit) -> FL_KILL there;
   - the game's effects: 0x17C240 (effect create: a1 type, a2 parameter block, world position +0..+8 with 1.0 at +0xC)
     -> FL_BLAST / FL_LAUNCH by type (fl_kind_of; the types from ACZ_DEBUG_FLASH runs, see NOTES);
   - hits: 0x1FCD28 (damage: a0 the unit hit, position +0x260) -> FL_HIT at the unit (gun and missile hits that do not
     kill; a kill follows with its own event).
   Events further than 4 km from the aircraft are dropped. ACZ_DEBUG_FLASH=1 logs the events, =2 also every effect the
   game creates within 4 km (type, caller, distance) and the own counts when they change. */
#include "zero_instr.h"
#define FL_OWN_LAUNCH 1
#define FL_LAUNCH     2
#define FL_KILL       3
#define FL_BLAST      4
#define FL_HIT        5
extern void ps2_vk_flash_event(int kind, float x, float y, float z, float strength, float side);
extern volatile float ps2_vk_flash_cont[4];
static int fl_on = -1, fl_dbg;
static float fl_ppos[3];                 /* the aircraft (game units), each flight field */
static u64 fl_pfield;
static float fl_dist_m(const float *p) {
    float dx = p[0] - fl_ppos[0], dy = p[1] - fl_ppos[1], dz = p[2] - fl_ppos[2];
    return sqrtf(dx * dx + dy * dy + dz * dz) / z_units_per_m;
}
static int fl_near(const float *p) {     /* a live flight and within 4 km */
    return fl_pfield && ps2_kernel_vblank_count() - fl_pfield < 30u && fl_dist_m(p) < 4000.0f;
}
/* The game's effects that flash (ACZ_DEBUG_FLASH=2 runs, M1 debug boot, 2026-10-23: three Circle presses, a Cross
   burst, the aircraft diving so the missiles hit the ground). Effect create 0x17C240, a1 = type, caller ra:
   - a missile launch: 48 (ra 0x20E0C8), 49 (0x1B04A4), 50 (0x1B04BC), the same field and place as the launch (5 m
     from the player's own aircraft for its own missiles); type 49 is taken as the launch;
   - the gun: 36 (0x17D2E0) at the muzzle every ~4 fields while firing (6 m from the own aircraft), 32 (0x20C454)
     every 10 fields at 9 m (casings/smoke), the rounds' impacts 35 (0x20CB5C) with two 64s from 0x180300;
   - a missile's detonation / impact: 51 (0x20E5A8) with the explosion set of 0x180300 (types 63 64 66 67 69 73 79 83;
     a kill makes the same set plus type 4 from 0x1FC930, inside 0x1FC880);
   - every field 5 (0x231F4C, the own aircraft's engine), and 7 8 9 10 11 121 (smoke, vapour, start-up) -- no flash.
   The own gun count (u16 player+0x1A9E) stayed at 10000 while the gun fired in the debug boot, so the own gun is
   taken from its muzzle effect (36 within 25 m). Explosions are merged: one within 60 m and 15 fields of an earlier
   one only counts if it is stronger (a kill's 0x180300 set follows its type 4 / destroy event). */
static struct { float p[3]; float st; u64 f; } fl_blast[8];
static int fl_blast_k;
static float fl_lp[3]; static u64 fl_lp_f;   /* the own launch effect's place (game units) and field */
static u64 fl_gun_f;                          /* the own muzzle effect's last field */
static int fl_blast_new(const float *p, float st) {
    u64 f = ps2_kernel_vblank_count();
    float r = 60.0f * z_units_per_m;
    for (int i = 0; i < 8; i++) {
        if (!fl_blast[i].f || f - fl_blast[i].f > 15u) continue;
        float dx = p[0] - fl_blast[i].p[0], dy = p[1] - fl_blast[i].p[1], dz = p[2] - fl_blast[i].p[2];
        if (dx * dx + dy * dy + dz * dz < r * r && st <= fl_blast[i].st) return 0;
    }
    memcpy(fl_blast[fl_blast_k].p, p, 3 * sizeof(float)); fl_blast[fl_blast_k].st = st; fl_blast[fl_blast_k].f = f;
    fl_blast_k = (fl_blast_k + 1) & 7;
    return 1;
}
static int fl_kind_of(u32 t, u32 ra, float dist_m, float *st) {
    *st = 0.0f;
    if (t == 49u && ra == 0x001B04A4u) { *st = 1.0f; return dist_m < 25.0f ? -1 : FL_LAUNCH; }   /* -1: own launch */
    if (t == 36u && ra == 0x0017D2E0u) return dist_m < 25.0f ? -2 : 0;                        /* -2: own gun */
    if (t == 51u && ra == 0x0020E5A8u) { *st = 0.8f; return FL_BLAST; }
    if (ra == 0x00180300u && (t == 63u || t == 66u || t == 67u || t == 69u || t == 73u || t == 79u || t == 83u)) {
        *st = 0.6f; return FL_BLAST;
    }
    return 0;
}
static int fl_fx_enter(ps2_ctx *ctx, void *user) {
    (void)user;
    u32 t = ctx->r[5].uw[0] & 0xFFFFu, p = ctx->r[6].uw[0], ra = ctx->r[31].uw[0];
    if (p < 0x100000u || p >= 0x2000000u) return 0;
    float v[4];
    for (u32 i = 0; i < 4; i++) v[i] = f_of(ps2_r32(p + 4u * i));
    if (v[3] != 1.0f || !fl_near(v)) return 0;
    float st, dm = fl_dist_m(v);
    int k = fl_kind_of(t, ra, dm, &st);
    if (fl_dbg >= 2) {
        static u32 n;
        if (n++ < 4000u)
            ps2_log("flash: fx field %llu type %u ra %08X at %.0f m%s", (unsigned long long)ps2_kernel_vblank_count(), t, ra,
                    dm, k > 0 ? " -> flash" : k == -1 ? " (own launch)" : k == -2 ? " (own gun)" : "");
    }
    if (k == -1) { memcpy(fl_lp, v, sizeof fl_lp); fl_lp_f = ps2_kernel_vblank_count(); return 0; }
    if (k == -2) { fl_gun_f = ps2_kernel_vblank_count(); return 0; }
    if (k == FL_BLAST && !fl_blast_new(v, st)) return 0;
    if (k > 0) ps2_vk_flash_event(k, v[0], v[1], v[2], st, 0.0f);
    return 0;
}
static int fl_destroy_enter(ps2_ctx *ctx, void *user) {
    (void)user;
    u32 u = ctx->r[4].uw[0];
    if (u < 0x100000u || u >= 0x2000000u || u == z_imm_player) return 0;
    u64 f = ps2_kernel_vblank_count();
    static struct { u32 unit; u64 field; } seen[16]; static int ns;
    for (int i = 0; i < 16; i++)
        if (seen[i].unit == u && f - seen[i].field < 600u) { seen[i].field = f; return 0; }
    seen[ns].unit = u; seen[ns].field = f; ns = (ns + 1) & 15;
    float q[3];
    for (u32 i = 0; i < 3; i++) q[i] = f_of(ps2_r32(u + 0x260u + 4u * i));
    if (!fl_near(q)) return 0;
    fl_blast_new(q, 1.0f);                           /* (its 0x180300 set follows: merged) */
    if (fl_dbg) ps2_log("flash: kill field %llu unit %08X kind %u at %.0f m", (unsigned long long)f, u, ctx->r[5].uw[0] & 0xFFu,
                        fl_dist_m(q));
    ps2_vk_flash_event(FL_KILL, q[0], q[1], q[2], 1.0f, 0.0f);
    return 0;
}
static int fl_damage_enter(ps2_ctx *ctx, void *user) {
    (void)user;
    u32 u = ctx->r[4].uw[0];
    if (u < 0x100000u || u >= 0x2000000u) return 0;
    float q[3];
    for (u32 i = 0; i < 3; i++) q[i] = f_of(ps2_r32(u + 0x260u + 4u * i));
    if (!fl_near(q)) return 0;
    u32 amount = ctx->r[7].uw[0] & 0xFFFFu, kind = ctx->r[8].uw[0] & 0xFFu;
    if (amount >= 1000u) return 0;                   /* (a crash: the ground, not a hit) */
    /* a gun round takes little, a missile a lot: the flash grows with the damage (a hit on the player: at the cockpit) */
    float st = amount >= 40u ? 0.6f : amount >= 10u ? 0.3f : 0.12f;
    if (fl_dbg) ps2_log("flash: hit field %llu unit %08X%s amount %u kind %u at %.0f m", (unsigned long long)ps2_kernel_vblank_count(),
                        u, u == z_imm_player ? " (PLAYER)" : "", amount, kind, fl_dist_m(q));
    ps2_vk_flash_event(FL_HIT, q[0], q[1], q[2], st, 0.0f);
    return 0;
}
/* the own aircraft's sources (instr_hook, once per flight field) */
static void fl_own(u32 player, u64 f, const zero_instr *v, int ev) {
    if (fl_on <= 0) return;
    static int lm = -1, ls = -1, lg = -1; static u64 gun_until, lf;
    for (u32 i = 0; i < 3; i++) fl_ppos[i] = f_of(ps2_r32(player + 0xEF0u + 0x3B0u + 4u * i));
    fl_pfield = f;
    int gap = lf && f - lf > 30u;                     /* (a pause, a menu, a mission change: counts may jump) */
    lf = f;
    if (!ev && !gap && lm >= 0) {
        int dm = lm - v->missiles, ds = ls - v->special;
        /* the launch effect's place when it came this field or the last (side 0: the renderer takes the rail from
           it), else the rail by the count's parity */
        int lp = fl_lp_f && f - fl_lp_f <= 2u;
        if (dm > 0 && dm <= 4) {
            ps2_vk_flash_event(FL_OWN_LAUNCH, lp ? fl_lp[0] : 0.0f, lp ? fl_lp[1] : 0.0f, lp ? fl_lp[2] : 0.0f, 1.0f,
                               lp ? 0.0f : (v->missiles & 1) ? 1.0f : -1.0f);
            if (fl_dbg) ps2_log("flash: own missile field %llu (%d left)%s", (unsigned long long)f, v->missiles,
                                lp ? " at its launch effect" : "");
        }
        if (ds > 0 && ds <= 8) {
            ps2_vk_flash_event(FL_OWN_LAUNCH, lp ? fl_lp[0] : 0.0f, lp ? fl_lp[1] : 0.0f, lp ? fl_lp[2] : 0.0f, 1.0f,
                               lp ? 0.0f : (v->special & 1) ? 1.0f : -1.0f);
            if (fl_dbg) ps2_log("flash: own special weapon field %llu (%d left)", (unsigned long long)f, v->special);
        }
        if (dm > 0 || ds > 0) fl_lp_f = 0;
        if (v->gun < lg && lg - v->gun < 200) gun_until = f + 6u;
    }
    if (fl_gun_f && f - fl_gun_f <= 2u && gun_until < fl_gun_f + 8u) gun_until = fl_gun_f + 8u;   /* (the muzzle effect) */
    if (fl_dbg >= 2 && (v->missiles != lm || v->special != ls || v->gun != lg))
        ps2_log("flash: counts field %llu missiles %d special %d gun %d ab %.2f", (unsigned long long)f, v->missiles,
                v->special, v->gun, v->afterburner);
    lm = v->missiles; ls = v->special; lg = v->gun;
    float ab = v->afterburner;
    ps2_vk_flash_cont[0] = ev ? 0.0f : (ab > 0.0f ? (ab > 1.0f ? 1.0f : ab) : 0.0f);
    ps2_vk_flash_cont[1] = !ev && f < gun_until ? 1.0f : 0.0f;
    ps2_vk_flash_cont[2] = (float)f;
}
static void flash_install(void) {
    const char *e = ps2_menv_boot("ACZ_VR_FLASH_LIGHT");   /* (the hooks; a later OFF: the renderer's flc.k ignores the events) */
    fl_on = !(e && *e == '0' && !e[1]) && !(e && !strcmp(e, "off"));
    e = getenv("ACZ_DEBUG_FLASH"); fl_dbg = e ? atoi(e) : 0;
    if (!fl_on) { ps2_log("zero-vr: flash light off (ACZ_VR_FLASH_LIGHT=0)"); return; }
    int a = ps2_r32(0x0017C240u) >> 16 == 0x27BDu && ps2_hook_before(0x0017C240u, fl_fx_enter, NULL, 61, "zero-flash") >= 0;
    int b = ps2_r32(0x001FC880u) >> 16 == 0x27BDu && ps2_hook_before(0x001FC880u, fl_destroy_enter, NULL, 61, "zero-flash") >= 0;
    int c = ps2_r32(0x001FCD28u) >> 16 == 0x27BDu && ps2_hook_before(0x001FCD28u, fl_damage_enter, NULL, 61, "zero-flash") >= 0;
    ps2_log("zero-vr: flash light events: effects %d, kills %d, hits %d, own weapons and afterburner (instruments)", a, b, c);
}
/* ---- Pixy's MPBM burst as a volume (ACZ_VR_FX_BURST; fx_burst 2026-11-02, bug #266) ----
   The user, 2026-11-02 (M18, X-02, trailer): "the huge explosion that Pixy releases ... a very harsh gradient; it does
   not look volumetric, it still looks 2D; there is no beautiful explosion gradient -- it looks like 2006". The MPBM
   (Pixy's ADFX-02 Morgan in M18's second phase; traced in a debug boot with ACZ_DEBUG_IMMERSE / ACZ_DEBUG_FLASH=2: unit
   01493FE0's launch 48 / 49 / 50 from 0x220BB8, ~1.5 s later its detonation 0x2216A4 type 51 with the 0x180300 set
   66 69 102; type 102 came from nothing else seen) is drawn by the game as one big quad of an 8 x 8 texel additive glow
   (up to ~56 GS sprite columns, two layers) that grows for ~4 s into a flat white disc, plus a copy into its glow target
   laid back as a blocky bloom (the stair-stepped lavender rim). The renderer (ps2_video.c bst_*) gets the burst's world
   position here, takes the game's glow out and marches a volume there sized and timed by the game's own sprites.
   ACZ_DEBUG_FX_BURST=1 logs the events. In a debug boot the burst comes with ACZ_DEBUG_UNIT_KILL=3000:014862F0:d (Pixy's
   first form there) and ACZ_DEBUG_GAZE_TP holding the aircraft near the second form's spawn (env reference). */
extern void ps2_vk_burst_event(float x, float y, float z, int kind);
static int zbu_dbg;
static int zbu_fx_enter(ps2_ctx *ctx, void *user) {
    (void)user;
    u32 t = ctx->r[5].uw[0] & 0xFFFFu, p = ctx->r[6].uw[0], ra = ctx->r[31].uw[0];
    if (t != 102u || ra != 0x00180300u || p < 0x100000u || p >= 0x2000000u) return 0;
    float v[4];
    for (u32 i = 0; i < 4; i++) v[i] = f_of(ps2_r32(p + 4u * i));
    if (v[3] != 1.0f || !(fabsf(v[0]) < 1e8f && fabsf(v[1]) < 1e8f && fabsf(v[2]) < 1e8f)) return 0;
    ps2_vk_burst_event(v[0], v[1], v[2], 1);
    if (zbu_dbg)
        ps2_log("zero-vr: MPBM burst field %llu at %.0f %.0f %.0f (units), %.0f m from the aircraft",
                (unsigned long long)ps2_kernel_vblank_count(), v[0], v[1], v[2], fl_pfield ? fl_dist_m(v) : -1.0f);
    return 0;
}
static void burst_install(void) {
    const char *e = ps2_menv_boot("ACZ_VR_FX_BURST");      /* (the hook; a later OFF: the renderer's bst_on ignores the events) */
    if (e && (!strcmp(e, "0") || !strcmp(e, "off"))) { ps2_log("zero-vr: MPBM burst as a volume off (ACZ_VR_FX_BURST=0)"); return; }
    e = getenv("ACZ_DEBUG_FX_BURST"); zbu_dbg = e ? atoi(e) : 0;
    int a = ps2_r32(0x0017C240u) >> 16 == 0x27BDu && ps2_hook_before(0x0017C240u, zbu_fx_enter, NULL, 63, "zero-burst") >= 0;
    ps2_log("zero-vr: MPBM bursts (effect 102 of the 0x180300 set) to the renderer's volume: %s (ACZ_VR_FX_BURST)",
            a ? "on" : "no hook");
}
/* Rumble: the game sends vibration through sceVibSetActParam (0x30A168) to an IOP service the HLE does not provide.
   An observe-only hook decodes it (small motor on/off, large motor 0-255; only when the profile addresses both
   motors) and hands it to the renderer's gamepad (ps2_video.c). ACZ_RUMBLE=0 turns it off. */
/* Rumble counters (pt_retry 2026-10-29, ACZ_RUMBLE_LOG; read by ps2_video.c's rumble summary): calls of
   sceVibSetActParam, those for player 1 with both motors addressed, those with a motor on; the largest large-motor
   value; the game's pattern requests (0x110980) and how many of them its per-player vibration gate refused. The
   10:50 / 14:23 sessions had no such log, so whether the game's rumble ran could not be told afterwards. (Defined in
   ps2_video.c, which gsreplay links without this file.) */
extern volatile u32 ps2_zero_vib_calls, ps2_zero_vib_used, ps2_zero_vib_on, ps2_zero_vib_large_max;
extern volatile u32 ps2_zero_vib_patterns, ps2_zero_vib_gated;
static int vib_hook(ps2_ctx *ctx, void *user) {
    (void)user;
    u32 psize = ctx->r[5].uw[0], prof = ctx->r[6].uw[0], dsize = ctx->r[7].uw[0], data = ctx->r[8].uw[0];
    ps2_zero_vib_calls++;
    if (ctx->r[4].uw[0] != 0u || psize < 1u || dsize < 2u || !prof || !data) return 0;   /* socket 0 = player 1 */
    if ((ps2_r8(prof) & 3u) != 3u) return 0;
    u8 d0 = ps2_r8(data), d1 = ps2_r8(data + 1u);
    int small = d0 & 1, large = (d0 >> 1) | ((d1 & 1) << 7);
    ps2_zero_vib_used++;
    if (small || large) {
        ps2_zero_vib_on++;
        if ((u32)large > ps2_zero_vib_large_max) ps2_zero_vib_large_max = (u32)large;
    }
    extern void ps2_video_set_rumble(int small, int large);
    {   /* ACZ_RUMBLE_LOG=3 (fx_debris, bug #260): every change of the game's motor values */
        static int ls = -1, ll = -1; extern int zero_rumble_log_level(void);
        if (zero_rumble_log_level() >= 3 && (small != ls || large != ll))
            ps2_log("rumble: field %llu the game's motors small %d large %d", (unsigned long long)ps2_kernel_vblank_count(),
                    small, large);
        ls = small; ll = large;
    }
    imm_vib_note(small, large);
    ps2_video_set_rumble(small, large);
    return 0;
}
/* The game's rumble pattern player 0x110980 (a0 = the player's rumble object, player +0xAE4; a2 = pattern): called
   by the player update for its events -- 4 a gun hit, 5 a missile or special-weapon hit, 7 event bit 0x80000000, 6 the
   death and the +0x1AA0 bit 0x100 event (with a camera shake), 8 the player's own launches / gun (+0x1AA0 bits 0x1 /
   0x2 / 0x20), 9 / 10 the stall, 0 the air brake, 1 / 2 / 3 / 13 others -- and by 0x2704BC (8) / 0x2705E8 (9). No
   call site tests the "homing missile on the player" bit (U+0x200 0x04000000) or the missile alert; the setter of
   +0x1AA0 bit 0x100 (pattern 6 + shake) was not found statically, so a near miss through it is not excluded. It plays
   nothing while the byte a0 +0x79 is 0 (the game's own vibration gate for that player). Observe-only: logs the
   requests (ACZ_RUMBLE_LOG: the first 24 and every gate change; 2 = all). */
static int zr_log_lvl = -1;
static int zr_log(void) {
    if (zr_log_lvl < 0) { const char *e = getenv("ACZ_RUMBLE_LOG"); zr_log_lvl = e && *e ? atoi(e) : 1; }
    return zr_log_lvl;
}
int zero_rumble_log_level(void) { return zr_log(); }
static int vib_pattern_hook(ps2_ctx *ctx, void *user) {
    (void)user;
    u32 a0 = ctx->r[4].uw[0];
    int pat = (int)(s8)(ctx->r[6].uw[0] & 0xFFu);
    if (a0 < 0x100000u || a0 >= 0x2000000u) return 0;
    int gate = ps2_r8(a0 + 0x79u) != 0u;
    static int last_gate = -1; static u32 n;
    ps2_zero_vib_patterns++;
    if (!gate) ps2_zero_vib_gated++;
    {   /* the own gun (pattern 13 from the player update's gun shot, 0x232EAC, every 10 fields while firing): the game
           plays it only on its special controller type (pad +0x35 via 0x103AB0: the DualShock's motors get nothing);
           counted for ACZ_IMM_GUN_RUMBLE (ps2_video.c), with the game's own vibration gate */
        extern volatile u32 ps2_zero_gun_seq;
        if (pat == 13 && ctx->r[31].uw[0] == 0x00232EACu && gate) ps2_zero_gun_seq++;
    }
    n++;
    if (zr_log() >= 2 || (zr_log() >= 1 && (n <= 24u || gate != last_gate)))
        ps2_log("rumble: the game asks for pattern %d (field %llu, caller %08X)%s", pat,
                (unsigned long long)ps2_kernel_vblank_count(), ctx->r[31].uw[0],
                gate ? "" : ": NOT played, the game's vibration gate is off for this player (+0x79 = 0)");
    last_gate = gate;
    return 0;
}
/* Flight instruments for the native cockpit (static RE 2026-10-19, see NOTES): the HUD manager update 0x24D9D8 runs
   once per flight frame with a0 = player + 0x1310, right after the frame's HUD info is built. Flight model
   FM = player + 0xEF0: speed FM+0x3F8 (HUD knots = v / 1.3 * 0.54), position FM+0x3B0 (y up; metres = y * 400 /
   2048), pitch / yaw / roll FM+0x3C0.. (radians; heading = -yaw), power FM+0x404, afterburner +0x400, airbrake +0x408,
   AoA candidate +0x224, stick +0x210..0x218, throttle input +0x21C (R1 - L1 pressure), mode byte +0x412. Ammo:
   missiles u8 player+0x1AA9, gun u16 +0x1A9E, special u8 +0x1AAA. Vertical speed and G are derived from the position history. ACZ_DEBUG_INSTR=1 logs them. */
#include "zero_instr.h"
/* Canopy ice (bug #24; ps2_video.c canopy_update): the freezing level in metres of altitude as the cockpit altimeter
   shows it (the flight model's height, y x 400 / 2048). Default 2700 m: a mild 17.5 C at sea level and the standard
   lapse rate (6.5 K per km). The snow missions -- 1 Glacial Skies, 12 The Stage of Apocalypse, 17 Valley of Kings,
   18 ZERO (config_zero/cloud_missions.json precip kind snow; in 12 and 17 only the second environment set snows, so
   the live flake share alone would miss it) -- and any stage where snow falls now (sky +0xB8C > 0 with the flake
   share >= 0.4) have it at the ground (0 m): the whole sky is below freezing there. Mission index: the campaign slot's
   +0x5F48 (0 = mission 1; as ps2_clouds_art.c). ACZ_IMM_FREEZE_M=m forces the level; "m1:field:m2" switches from m1
   to m2 at that field (melt tests). Logged when it changes. */
static float imm_freeze_level(u64 f, float pr, float flakes) {
    static int init, en, last_idx = -99; static float e1, e2, last = -1e9f; static u64 ef;
    if (!init) {
        init = 1;
        const char *e = getenv("ACZ_IMM_FREEZE_M");
        unsigned long long fl = 0;
        if (e && sscanf(e, "%f:%llu:%f", &e1, &fl, &e2) == 3) { en = 2; ef = fl; }
        else if (e && sscanf(e, "%f", &e1) == 1) en = 1;
    }
    u32 s = ps2_r8(0x003B3810u + 0xC04Cu) ? 0x003B3810u + 0x5FE4u : 0x003B3810u + 0x50u;
    int idx = (int)ps2_r32(s + 0x5F48u);
    float lvl = 2700.0f;
    if (idx == 0 || idx == 11 || idx == 16 || idx == 17) lvl = 0.0f;
    if (pr > 0.0f && flakes >= 0.4f && lvl > 0.0f) lvl = 0.0f;
    if (en) lvl = en == 2 && f >= ef ? e2 : e1;
    if (lvl != last || idx != last_idx) {
        ps2_log("zero-vr: canopy ice: freezing level %.0f m (mission index %d%s, field %llu)", lvl, idx,
                en ? ", ACZ_IMM_FREEZE_M" : "", (unsigned long long)f);
        last = lvl; last_idx = idx;
    }
    return lvl;
}
static zero_instr z_instr;
static pthread_mutex_t z_instr_mx = PTHREAD_MUTEX_INITIALIZER;
int zero_instr_get(zero_instr *out) {
    pthread_mutex_lock(&z_instr_mx);
    *out = z_instr;
    pthread_mutex_unlock(&z_instr_mx);
    return out->valid;
}
/* ---- ACZ_DEBUG_LOWFLY=agl_m[,from_field[,bank_deg]] (test only, bug #25): a crude terrain-following autopilot on
   the left stick for repeatable low passes: from that field on (default 900) it pitches towards agl_m metres above
   the ground under the aircraft (the game's own ground height query 0x158AE0, taken from its calls within 400 units
   of the player; 400/2048 m per unit as the altimeter) and holds the given bank (default 0 = wings level).
   ACZ_DEBUG_LOWFLY_LOG=N logs it every N fields. Only for scripted runs: it overrides the left stick. */
static struct { int on, dbg; float agl, bank, qx, qz, gy, pp[3]; u64 from, gfield; u8 lx, ly, act; } z_lf;
static int lf_steer(u8 *lx, u8 *ly) {
    if (!z_lf.act) return 0;
    *lx = z_lf.lx; *ly = z_lf.ly;
    return 1;
}
static int lf_ground_enter(ps2_ctx *ctx, void *user) { (void)user; z_lf.qx = ctx->f[12].f; z_lf.qz = ctx->f[13].f; return 0; }
static int lf_ground_leave(ps2_ctx *ctx, void *user) {
    (void)user;
    float dx = z_lf.qx - z_lf.pp[0], dz = z_lf.qz - z_lf.pp[2];
    if (dx * dx + dz * dz < 400.0f * 400.0f && ctx->f[0].f > -100000.0f && ctx->f[0].f < 100000.0f) {
        z_lf.gy = ctx->f[0].f; z_lf.gfield = ps2_kernel_vblank_count();
    }
    return 0;
}
static void lf_update(u32 player, u64 f, const zero_instr *v) {
    if (z_lf.on <= 0) return;
    u32 fm = player + 0xEF0u;
    for (u32 i = 0; i < 3; i++) z_lf.pp[i] = f_of(ps2_r32(fm + 0x3B0u + 4u * i));
    if (f < z_lf.from || !z_lf.gfield || f - z_lf.gfield > 30u) { z_lf.act = 0; return; }
    float agl = (z_lf.pp[1] - z_lf.gy) * 400.0f / 2048.0f;
    float pc = (z_lf.agl - agl) * 0.12f;
    pc = pc > 12.0f ? 12.0f : pc < -15.0f ? -15.0f : pc;
    float sy = (pc - v->pitch_deg) * 0.08f + fabsf(z_lf.bank) * 0.012f;
    sy = sy > 1.0f ? 1.0f : sy < -1.0f ? -1.0f : sy;
    float sx = (z_lf.bank + v->roll_deg) * 0.05f;
    sx = sx > 1.0f ? 1.0f : sx < -1.0f ? -1.0f : sx;
    int ix = fabsf(sx) < 0.05f ? 0 : (int)copysignf(60.0f + 60.0f * fabsf(sx), sx);
    int iy = fabsf(sy) < 0.05f ? 0 : (int)copysignf(60.0f + 60.0f * fabsf(sy), sy);
    z_lf.lx = (u8)(128 + ix); z_lf.ly = (u8)(128 + iy); z_lf.act = 1;
    if (z_lf.dbg > 0 && f % (u64)z_lf.dbg == 0u)
        ps2_log("lowfly: field %llu agl %.0f m (ground %.0f, aircraft %.0f units) pitch %.1f roll %.1f speed %.0f kt "
                "-> LX %u LY %u", (unsigned long long)f, agl, z_lf.gy, z_lf.pp[1], v->pitch_deg, v->roll_deg,
                v->speed_kt, z_lf.lx, z_lf.ly);
}
static void lf_install(void) {
    const char *e = getenv("ACZ_DEBUG_LOWFLY");
    if (!e) return;
    z_lf.agl = (float)atof(e); z_lf.from = 900u;
    const char *c = strchr(e, ',');
    if (c) { z_lf.from = strtoull(c + 1, NULL, 0); c = strchr(c + 1, ','); if (c) z_lf.bank = (float)atof(c + 1); }
    const char *d = getenv("ACZ_DEBUG_LOWFLY_LOG");
    z_lf.dbg = d ? atoi(d) : 0;
    if (!(z_lf.agl > 5.0f && z_lf.agl < 5000.0f) || ps2_r32(0x00158AE0u) != 0x3C0E003Fu
        || ps2_hook_before(0x00158AE0u, lf_ground_enter, NULL, 80, "zero-lowfly") < 0
        || ps2_hook_after(0x00158AE0u, lf_ground_leave, NULL, 80, "zero-lowfly") < 0) {
        ps2_log("zero-vr: ACZ_DEBUG_LOWFLY ignored"); return;
    }
    z_lf.on = 1;
    { extern int (*ps2_pad_test_steer)(u8 *, u8 *); ps2_pad_test_steer = lf_steer; }
    ps2_log("zero-vr: debug: low-pass autopilot %.0f m above the ground from field %llu, bank %.0f (ACZ_DEBUG_LOWFLY)",
            z_lf.agl, (unsigned long long)z_lf.from, z_lf.bank);
}
/* ---- G presence (wish #77: G vision, G-suit, strain breathing; #81 buffet; #82 breathing in the oxygen mask) ----
   The flight model's own state (static RE 2026-10-23, agent im_gforce; FM = player + 0xEF0, one game frame 1/60 s,
   5.12 units a metre): airspeed FM+0x200 (units per frame; FM+0x3F8 = x 60 / 1.42222), the body's angular rates
   FM+0x1B0 / +0x1B4 / +0x1B8 (pitch, yaw, roll; rad per frame; the physics core 0x22ED78 integrates them into the
   attitude), the stall factor FM+0x3FC (a copy of FM+0x220: 0 above the aircraft's stall onset speed u16 FM+0x1C,
   1 below u16 FM+0x1A -- FM+0x3F8 units at sea level, x sqrt(rho0 / rho) aloft; the HUD shows STALL above 0.5), on the
   ground FM+0x236 bit 0, -AoA FM+0x224 (rad; the velocity follows the nose, so the game's AoA is only the bank sag and
   the stall sink: a few degrees). The game keeps no load factor: the felt G along the aircraft's up axis is
   n = V q / g + cos(pitch) cos(roll) (the path turns with the nose: the pitch rate q). Warnings, from the player's unit
   record U (z_imm_player): U+0x200 = the unit's events of the last frame -- 0x04000000 a homing missile tracks the
   player (MISSILE ALERT, written by the missile manager 0x20EE78 while its seeker sees us), 0x100 an enemy seeker has
   locked on (WARNING), 0x10000000..0x80000000 hits; the missile manager *(player+0x1A20): 16 slots of 0x50 bytes at
   +0xE0, active mask u32 +0x5F0, slot +0x00 position, +0x3E/+0x3F target group / index (the player's: u8
   player+0x1AB1 / +0x1AB2), +0x43 bit 0 homing; the HUD's warning word player+0x1600 (0x40000 STALL, 0x400000 PULL UP).
   From these, once per flight frame (gf_update):
   - G tolerance (ACZ_IMM_GLOC=strength[,onset_g[,full_g[,red]]], default 1,6,9,0.6): a pilot with a G-suit, straining:
     past onset_g sustained the periphery greys (tunnel vision), full at full_g; the retina's few seconds of oxygen
     reserve make it lag (rise ~2.5 s, recovery ~1.2 s), so a short 9 G pull barely touches it; fatigue (G-seconds
     above 5 G) lowers the onset by up to 0.6 G. Negative G past -2 G sustained: a faint red-out (x red).
   - the G-suit valve (ps2_gforce_audio.c), the anti-G straining manoeuvre (strain on above 5 G, off below 4 G), stress
     (missile alert, lock, hits, firing, STALL / PULL UP), exertion (G-work), buffet (the share of the wing's maximum
     lift that the pull needs, n (Vs / V)^2 with Vs the stall onset speed at this altitude: from 55 % up; the stall
     factor; the air brake at speed) -> renderer (ps2_gf_vis: vignette, rumble, needle jitter) and sound (ps2_gf_audio).
   ACZ_IMM_G_TEST="field:g[:buffet[:stress]] ..." (test) holds those inputs from each field on ("field:off" = measured
   again); ACZ_DEBUG_GFORCE=N logs every N fields; ACZ_INSTR_FM=0: the instruments' G and AoA as before (G from the
   position history, the AoA field's raw sign). */
static float gf_gpos = 1.0f, gf_ginst = 1.0f;     /* the instruments' G: from the positions / the flight model */
static struct {
    int init, vis_on, dbg, instr_fm, nt;
    float vis_k, onset, full, red_k;
    float g, grey, red, fat, strain, stress, exert, buffet, cl, sf, msl_d;
    int strain_on, msl_prev, gun_prev; float strain_t, gun_t;
    u64 lf, nupd;
    struct { u64 f; float g, b, s; int off; } t[16];
} gf;
static void gf_read(void) {                     /* (the NEXT MISSION tier: also at the edge, zero_env_reload_mission) */
    gf.vis_k = 1.0f; gf.onset = 6.0f; gf.full = 9.0f; gf.red_k = 0.6f;
    const char *e = ps2_menv("ACZ_IMM_GLOC");
    if (e && *e) {
        float a = 1.0f, b = 6.0f, c = 9.0f, d = 0.6f;
        int n = sscanf(e, "%f,%f,%f,%f", &a, &b, &c, &d);
        if (n >= 1) gf.vis_k = a < 0.0f ? 0.0f : a > 2.0f ? 2.0f : a;
        if (n >= 2 && b > 1.5f && b < 15.0f) gf.onset = b;
        if (n >= 3 && c > gf.onset + 0.5f && c < 20.0f) gf.full = c;
        if (n >= 4 && d >= 0.0f && d <= 2.0f) gf.red_k = d;
    }
    {   const char *im = getenv("ACZ_IMMERSE"); if (im && *im == '0') gf.vis_k = 0.0f; }
    gf.vis_on = gf.vis_k > 0.0f;
}
static void gf_init(void) {
    gf.init = 1;
    gf_read();
    const char *e = getenv("ACZ_DEBUG_GFORCE"); gf.dbg = e ? atoi(e) : 0;
    e = getenv("ACZ_INSTR_FM"); gf.instr_fm = !(e && *e == '0');
    gf.msl_prev = gf.gun_prev = -1;
    e = getenv("ACZ_IMM_G_TEST");
    for (const char *p = e; p && *p && gf.nt < 16; ) {
        while (*p == ' ') p++;
        if (!*p) break;
        unsigned long long fl = 0; float g = 1.0f, b = -1.0f, s = -1.0f; char w[16] = "";
        int n = sscanf(p, "%llu:%15[^: ]", &fl, w);
        if (n == 2) {
            gf.t[gf.nt].f = fl; gf.t[gf.nt].off = !strcmp(w, "off");
            if (!gf.t[gf.nt].off) {
                const char *q = strchr(p, ':'); q = q ? q + 1 : p;
                sscanf(q, "%f:%f:%f", &g, &b, &s);
            }
            gf.t[gf.nt].g = g; gf.t[gf.nt].b = b; gf.t[gf.nt].s = s; gf.nt++;
        }
        while (*p && *p != ' ') p++;
    }
    ps2_log("zero-vr: G presence: vision %.2f (onset %.1f G, full %.1f G, red-out %.2f), instruments' G and AoA %s%s",
            gf.vis_k, gf.onset, gf.full, gf.red_k, gf.instr_fm ? "from the flight model" : "as before",
            gf.nt ? " (ACZ_IMM_G_TEST)" : "");
}
/* the felt load factor from the flight model (airspeed x pitch rate + gravity along the aircraft's up axis). The
   airspeed as the HUD and the cockpit's airspeed indicator show it: the world moves 6.656 units per displayed metre
   (FM+0x200 x 60 / 1.42222 = FM+0x3F8 = 1.3 x km/h), while the altimeter counts 5.12 -- with 5.12 a full pull of the
   F-5E at 300 kt (47 deg/s) would read 15.7 G instead of 11.9 */
static float gf_load(u32 fm, float pitch, float roll) {
    float V = f_of(ps2_r32(fm + 0x200u)) * 60.0f / 6.656f, q = f_of(ps2_r32(fm + 0x1B0u)) * 60.0f;
    if (!(V > -2000.0f && V < 2000.0f) || !(q > -20.0f && q < 20.0f)) return 1.0f;
    return V * q / 9.81f + cosf(pitch) * cosf(roll);
}
static float gf_ss(float a, float b, float x) { float t = (x - a) / (b - a); t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t; return t * t * (3.0f - 2.0f * t); }
static void gf_follow(float *s, float target, float dt, float tup, float tdn) {
    float tau = target > *s ? tup : tdn;
    *s += (target - *s) * (1.0f - expf(-dt / tau));
}
static double gf_cpu_ns; static u32 gf_cpu_n;            /* ACZ_DEBUG_GFORCE: gf_update's own time */
static void gf_update_body(u32 player, u64 f, const zero_instr *v);
static void gf_update(u32 player, u64 f, const zero_instr *v) {
    struct timespec c0, c1;
    clock_gettime(CLOCK_MONOTONIC, &c0);
    gf_update_body(player, f, v);
    clock_gettime(CLOCK_MONOTONIC, &c1);
    gf_cpu_ns += (double)(c1.tv_sec - c0.tv_sec) * 1e9 + (double)(c1.tv_nsec - c0.tv_nsec); gf_cpu_n++;
}
static void gf_update_body(u32 player, u64 f, const zero_instr *v) {
    extern volatile float ps2_gf_vis[8];
    extern volatile float ps2_gf_audio[16];
    if (!gf.init) gf_init();
    u32 fm = player + 0xEF0u;
    u64 gap = gf.lf ? f - gf.lf : 1u;
    gf.lf = f;
    float dt = (float)(gap >= 1u && gap <= 4u ? gap : 1u) / 60.0f;
    gf.nupd++;
    float pitch = v->pitch_deg * 0.01745329f, roll = v->roll_deg * 0.01745329f;
    float n = gf_load(fm, pitch, roll);
    gf_follow(&gf.g, n, dt, 0.05f, 0.05f);
    float G = gf.g, tb = -1.0f, ts = -1.0f;
    for (int i = gf.nt - 1; i >= 0; i--)            /* the test schedule: the newest entry at or before this field */
        if (f >= gf.t[i].f) { if (!gf.t[i].off) { G = gf.t[i].g; tb = gf.t[i].b; ts = gf.t[i].s; } break; }
    /* ---- vision: grey-out over seconds past the onset, red-out at sustained negative G ---- */
    gf.fat += (G > 5.0f ? (G - 5.0f) * dt / 60.0f : 0.0f) - gf.fat * dt / 90.0f;
    gf.fat = gf.fat < 0.0f ? 0.0f : gf.fat > 1.0f ? 1.0f : gf.fat;
    float on = gf.onset - 0.6f * gf.fat;
    float x = (G - on) / (gf.full - gf.onset);
    x = x < 0.0f ? 0.0f : x > 1.3f ? 1.3f : x;
    gf_follow(&gf.grey, x, dt, 2.5f, 1.2f);
    float xr = (-G - 2.0f) / 1.5f;
    xr = xr < 0.0f ? 0.0f : xr > 1.0f ? 1.0f : xr;
    gf_follow(&gf.red, xr, dt, 2.0f, 1.5f);
    /* ---- the anti-G straining manoeuvre: on above 5 G (0.25 s), off below 4 G (0.3 s) ---- */
    if (!gf.strain_on) { gf.strain_t = G > 5.0f ? gf.strain_t + dt : 0.0f; if (gf.strain_t > 0.25f) { gf.strain_on = 1; gf.strain_t = 0.0f; } }
    else { gf.strain_t = G < 4.0f ? gf.strain_t + dt : 0.0f; if (gf.strain_t > 0.3f) { gf.strain_on = 0; gf.strain_t = 0.0f; } }
    gf_follow(&gf.strain, gf.strain_on ? 1.0f : 0.0f, dt, 0.15f, 0.3f);
    /* ---- stress: missile alert (closer: more), lock, hits, firing, STALL / PULL UP; exertion from G-work ---- */
    u32 U = z_imm_player, ev = U ? ps2_r32(U + 0x200u) : 0u, hud = ps2_r32(player + 0x1600u);
    float dmin = 1e9f;
    {   u32 M = ps2_r32(player + 0x1A20u);
        if ((ev & 0x04000000u) && U && M >= 0x100000u && M < 0x2000000u) {
            u32 act = ps2_r32(M + 0x5F0u);
            u8 tg = ps2_r8(player + 0x1AB1u), ti = ps2_r8(player + 0x1AB2u);
            float up[3];
            for (u32 j = 0; j < 3; j++) up[j] = f_of(ps2_r32(U + 0x260u + 4u * j));
            for (u32 s = 0; s < 16u; s++) {
                u32 m = M + 0xE0u + 0x50u * s;
                if (!(act >> s & 1u) || ps2_r8(m + 0x3Eu) != tg || ps2_r8(m + 0x3Fu) != ti || !(ps2_r8(m + 0x43u) & 1u)) continue;
                float d2 = 0.0f;
                for (u32 j = 0; j < 3; j++) { float dd = f_of(ps2_r32(m + 4u * j)) - up[j]; d2 += dd * dd; }
                float d = sqrtf(d2) / 5.12f;
                if (d < dmin) dmin = d;
            }
        }
    }
    gf.msl_d = dmin;
    float st = 0.0f;
    if (ev & 0x04000000u) { float c = dmin < 1e8f ? (2100.0f - dmin) / 1700.0f : 0.5f; st = 0.85f + 0.15f * (c < 0.0f ? 0.0f : c > 1.0f ? 1.0f : c); }
    else if (ev & 0x100u) st = 0.5f;
    if (hud & 0x440000u) st = st > 0.6f ? st : 0.6f;              /* STALL, PULL UP */
    if (gf.gun_prev >= 0 && v->gun < gf.gun_prev) gf.gun_t = 0.5f;
    if (gf.gun_t > 0.0f) { gf.gun_t -= dt; if (st < 0.35f) st = 0.35f; }
    if (gf.msl_prev >= 0 && v->missiles < gf.msl_prev && gf.stress < 0.45f) gf.stress = 0.45f;
    if ((ev & 0xF0000000u) && gf.stress < 0.9f) gf.stress = 0.9f;  /* hit */
    if (gf.grey > 0.3f && st < 0.5f) st = 0.5f;
    gf.gun_prev = v->gun; gf.msl_prev = v->missiles;
    if (ts >= 0.0f) st = ts;
    gf_follow(&gf.stress, st, dt, 0.8f, 25.0f);
    if (ts >= 0.0f) gf.stress = ts;
    {   float et = (G - 3.0f) / 5.0f; et = et < 0.0f ? 0.0f : et > 1.0f ? 1.0f : et; gf_follow(&gf.exert, et, dt, 3.0f, 30.0f); }
    /* ---- buffet: a pull at the wing's limit, the stall, the air brake at speed. The game's flight model is not
       lift-limited (a full pull at 300 kt turns 47 deg/s, ~12 G: the lift is ~5x what the aircraft's stall speed
       FM+0x1C implies), so the wing's limit is where the pilot puts it: the stick near full deflection (FM+0x210 pitch
       beyond 55 %) with the load built up (|n| 2.5 -> 5.5 G) -- the older jets buffet heavily there; stronger with the
       dynamic pressure (sigma (kt / 350)^2: x 0.5 .. 1); the game's stall factor FM+0x3FC adds the low-speed buffet
       (rising from its onset: 0.27 at 0.25, 0.7 at the HUD's STALL (0.5), 1 stalled); the air brake shakes a little at
       speed (up to 0.35 at 500 kt) ---- */
    float kt = v->speed_kt, b = 0.0f;
    gf.sf = f_of(ps2_r32(fm + 0x3FCu)); if (!(gf.sf >= 0.0f && gf.sf <= 1.0f)) gf.sf = 0.0f;
    int ground = ps2_r8(fm + 0x236u) & 1u;
    if (!ground && kt > 60.0f) {
        float h = v->alt_ft / 3.2808f; h = h < 0.0f ? 0.0f : h > 20000.0f ? 20000.0f : h;
        float sig = h < 11000.0f ? powf(1.0f - 2.2558e-5f * h, 4.2559f) : 0.2971f * expf(-(h - 11000.0f) / 6341.6f);
        float qr = sig * (kt / 350.0f) * (kt / 350.0f);
        gf.cl = qr;
        b = gf_ss(0.55f, 1.0f, fabsf(v->stick[0])) * gf_ss(2.5f, 5.5f, fabsf(G)) * (0.5f + 0.5f * (qr > 1.0f ? 1.0f : qr));
        if (gf.sf > 0.0f) { float bs = gf_ss(0.0f, 0.5f, gf.sf) * (0.4f + 0.6f * gf.sf); if (bs > b) b = bs; }
        b += v->airbrake * gf_ss(150.0f, 500.0f, kt) * 0.35f;
        b = b > 1.0f ? 1.0f : b;
    } else gf.cl = 0.0f;
    if (tb >= 0.0f) b = tb;
    gf.buffet = b;
    /* ---- first person: the cockpit or the HUD view, not a cutscene camera ---- */
    int view = ps2_r8(player + 0x1AA4u), fp = view != 1u && !(ps2_r32(player + 0x1A68u) & 0x2000u);
    extern volatile int ps2_vk_cockpit_view;
    ps2_gf_vis[0] = gf.vis_on ? gf.grey * gf.vis_k : 0.0f;
    ps2_gf_vis[1] = gf.vis_on ? gf.red * gf.vis_k * gf.red_k : 0.0f;
    ps2_gf_vis[2] = gf.buffet;
    ps2_gf_vis[4] = (float)fp;
    ps2_gf_vis[5] = G;
    ps2_gf_vis[6] = (float)(gf.nupd & 0xFFFFFFu);
    ps2_gf_vis[3] = (float)f;                                    /* (stamp last: stale = paused) */
    /* the G-suit's direction from the head (the game's listener, turned with the head): the legs and abdomen, ~0.55 m
       below the eye and 0.25 m forward in the aircraft's axes */
    float sd[3] = { 0.0f, -0.9f, 0.4f };
    {   float L[12];
        for (u32 i = 0; i < 12; i++) L[i] = f_of(ps2_r32(0x00422C18u + 4u * i));
        float *LF = L + 6, *LU = L + 9, LR[3] = { LF[1] * LU[2] - LF[2] * LU[1], LF[2] * LU[0] - LF[0] * LU[2], LF[0] * LU[1] - LF[1] * LU[0] };
        float yaw = v->heading_deg * -0.01745329f;
        float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch), cr = cosf(roll), sr = sinf(roll);
        float aup[3] = { -sr * cy + cr * sp * sy, cp * cr, sr * sy + cr * sp * cy }, afw[3] = { -sy * cp, sp, -cy * cp };
        float d[3], o[3];
        for (int j = 0; j < 3; j++) d[j] = -0.55f * aup[j] + 0.25f * afw[j];
        o[0] = d[0] * LR[0] + d[1] * LR[1] + d[2] * LR[2]; o[1] = d[0] * LU[0] + d[1] * LU[1] + d[2] * LU[2];
        o[2] = d[0] * LF[0] + d[1] * LF[1] + d[2] * LF[2];
        float m = sqrtf(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]), lm = sqrtf(LR[0] * LR[0] + LR[1] * LR[1] + LR[2] * LR[2]);
        if (m > 1e-4f && lm > 0.5f && m == m) { sd[0] = o[0] / m; sd[1] = o[1] / m; sd[2] = o[2] / m; }
    }
    ps2_gf_audio[1] = G;
    ps2_gf_audio[2] = gf.strain;
    ps2_gf_audio[3] = gf.stress;
    ps2_gf_audio[4] = gf.exert;
    ps2_gf_audio[5] = gf.buffet;
    ps2_gf_audio[6] = (float)fp;
    ps2_gf_audio[7] = sd[0]; ps2_gf_audio[8] = sd[1]; ps2_gf_audio[9] = sd[2];
    ps2_gf_audio[10] = gf.grey;
    ps2_gf_audio[11] = kt;
    ps2_gf_audio[12] = ps2_vk_cockpit_view ? 1.0f : 0.0f;
    ps2_gf_audio[0] = (float)f;
    if (gf.dbg > 0 && f % (u64)gf.dbg == 0u)
        ps2_log("gforce: field %llu G %.2f (pos %.2f) q %.2f deg/s V %.0f kt h %.0f ft pitch %.1f roll %.1f aoa %.1f | "
                "grey %.2f red %.2f fat %.2f strain %.2f | stress %.2f exert %.2f ev %08X hud %06X msl %.0f m | "
                "stall %.2f vs %u/%u qbar %.2f stick %.2f brake %.2f buffet %.2f | view %u fp %d suit %.2f %.2f %.2f | cpu %.1f us",
                (unsigned long long)f, G, gf_gpos, f_of(ps2_r32(fm + 0x1B0u)) * 60.0f * 57.29578f, kt, v->alt_ft,
                v->pitch_deg, v->roll_deg, -f_of(ps2_r32(fm + 0x224u)) * 57.29578f, gf.grey, gf.red, gf.fat, gf.strain,
                gf.stress, gf.exert, ev, hud & 0xFFFFFFu, dmin < 1e8f ? dmin : -1.0f, gf.sf, ps2_r16(fm + 0x1Au),
                ps2_r16(fm + 0x1Cu), gf.cl, v->stick[0], v->airbrake, gf.buffet, view, fp, sd[0], sd[1], sd[2],
                gf_cpu_n ? gf_cpu_ns / 1e3 / gf_cpu_n : 0.0);
    if (gf.dbg > 0 && f % (u64)gf.dbg == 0u) { gf_cpu_ns = 0.0; gf_cpu_n = 0; }
}
static int instr_hook(ps2_ctx *ctx, void *user) {
    (void)user;
    static u64 last_field;
    static float hist[3][3];           /* positions (m) of the last three distinct frames */
    static int nhist;
    u64 f = ps2_kernel_vblank_count();
    if (f == last_field) return 0;      /* once per field (player 1) */
    last_field = f;
    u32 player = ctx->r[4].uw[0] - 0x1310u;
    u32 fm = player + 0xEF0u;
    if (player < 0x100000u || player >= 0x02000000u) return 0;
    zero_instr v;
    pthread_mutex_lock(&z_instr_mx);
    v = z_instr;
    pthread_mutex_unlock(&z_instr_mx);
    float spd = f_of(ps2_r32(fm + 0x3F8u));
    float px = f_of(ps2_r32(fm + 0x3B0u)), py = f_of(ps2_r32(fm + 0x3B4u)), pz = f_of(ps2_r32(fm + 0x3B8u));
    float pitch = f_of(ps2_r32(fm + 0x3C0u)), yaw = f_of(ps2_r32(fm + 0x3C4u)), roll = f_of(ps2_r32(fm + 0x3C8u));
    v.speed_kt = spd / 1.3f * 0.54f;
    v.alt_ft = py * 400.0f / 2048.0f * 3.2808f;
    float hd = -yaw * 57.29578f;
    while (hd < 0.0f) hd += 360.0f;
    while (hd >= 360.0f) hd -= 360.0f;
    v.heading_deg = hd;
    v.pitch_deg = pitch * 57.29578f;
    v.roll_deg = roll * 57.29578f;
    v.throttle = f_of(ps2_r32(fm + 0x404u));
    v.afterburner = f_of(ps2_r32(fm + 0x400u));
    v.airbrake = f_of(ps2_r32(fm + 0x408u));
    v.aoa_deg = f_of(ps2_r32(fm + 0x224u)) * 57.29578f;
    for (int i = 0; i < 3; i++) v.stick[i] = f_of(ps2_r32(fm + 0x210u + 4u * (u32)i));
    v.missiles = ps2_r8(player + 0x1AA9u);
    v.gun = (int)(ps2_r32(player + 0x1A9Cu) >> 16);
    v.special = ps2_r8(player + 0x1AAAu);
    /* derived: vertical speed and load factor from the position history (60 game frames per second) */
    float pm[3] = { px * 400.0f / 2048.0f, py * 400.0f / 2048.0f, pz * 400.0f / 2048.0f };
    hist[2][0] = hist[1][0]; hist[2][1] = hist[1][1]; hist[2][2] = hist[1][2];
    hist[1][0] = hist[0][0]; hist[1][1] = hist[0][1]; hist[1][2] = hist[0][2];
    hist[0][0] = pm[0]; hist[0][1] = pm[1]; hist[0][2] = pm[2];
    if (nhist < 3) nhist++;
    if (nhist >= 2) {
        float vs = (hist[0][1] - hist[1][1]) * 60.0f;                       /* m/s */
        v.vsi_fpm = 0.9f * v.vsi_fpm + 0.1f * vs * 196.85f;
    }
    if (nhist >= 3) {
        float a[3];
        for (int i = 0; i < 3; i++) a[i] = (hist[0][i] - 2.0f * hist[1][i] + hist[2][i]) * 3600.0f;   /* m/s^2 */
        a[1] += 9.81f;                                                      /* felt = acceleration + gravity */
        /* aircraft up axis from pitch and roll (game euler: the same convention as the camera) */
        float up[3] = { -sinf(roll) * cosf(yaw) + cosf(roll) * sinf(pitch) * sinf(yaw), cosf(pitch) * cosf(roll),
                        sinf(roll) * sinf(yaw) + cosf(roll) * sinf(pitch) * cosf(yaw) };
        float gl = (a[0] * up[0] + a[1] * up[1] + a[2] * up[2]) / 9.81f;
        if (gl > -6.0f && gl < 12.0f) gf_gpos = 0.85f * gf_gpos + 0.15f * gl;
    }
    v.g = gf_gpos;
    if (!gf.init) gf_init();
    if (gf.instr_fm) {
        /* ACZ_INSTR_FM (default): the G from the flight model's airspeed and pitch rate (gf_load), lightly smoothed:
           the position history's second difference spiked to 4-5 G when a field held no game frame (rs_touch trace:
           5.2 G at a steady 19.7 deg climb); the AoA is the field's negative (FM+0x224 = -AoA) */
        gf_ginst += (gf_load(fm, pitch, roll) - gf_ginst) * 0.5f;
        v.g = gf_ginst;
        v.aoa_deg = -v.aoa_deg;
    }
    v.valid = 1;
    v.frame++;
    pthread_mutex_lock(&z_instr_mx);
    z_instr = v;
    pthread_mutex_unlock(&z_instr_mx);
    z_instr_field = f;
    {   extern volatile float ps2_imm_in[8];                /* canopy water inputs (ps2_video.c canopy_update) */
        extern volatile float ps2_imm_audio[8];
        u32 sk = z_imm_sky;
        /* precipitation at the camera: the sky update's amount (sky +0xB8C: 0.1 in mission 1's snow, 0.05..1 in the
           rain sets of missions 7 and 9; faded out over the stage's altitude band). Rain or snow: the next parameter
           (sky +0xB90, environment +0x88) is 0.8 in the snow and 0 in the rain sets -- taken as the flake share */
        int ok = sk >= 0x100000u && sk < 0x2000000u;
        float pr = ok ? f_of(ps2_r32(sk + 0xB8Cu)) : 0.0f, flakes = ok ? f_of(ps2_r32(sk + 0xB90u)) : 0.0f;
        if (!(pr >= 0.0f && pr <= 4.0f)) pr = 0.0f;
        ps2_imm_in[1] = pr > 1.0f ? 1.0f : pr;                  /* raw amount: 0.05 light rain .. 1 downpour */
        /* sound level: square root, so light rain is audible and a downpour not deafening (M7 0.38, M16 0.71) */
        float pn = pr > 0.0f ? 0.2f + 0.8f * sqrtf(pr > 1.0f ? 1.0f : pr) : 0.0f;
        ps2_imm_in[6] = flakes < 0.4f ? 1.0f : 0.0f;
        ps2_imm_in[2] = v.speed_kt;
        {   extern volatile float ps2_imm_freeze_m;              /* canopy ice: altitude and freezing level */
            ps2_imm_in[7] = pm[1];
            ps2_imm_freeze_m = imm_freeze_level(f, pr, flakes);
        }
        /* a mission event that holds the camera (flag 0x2000 of player +0x1A68: mission 2's take-off clearance, the
           cutscenes): the camera is not the pilot's and the flight model stands still (airspeed 0) -- the canopy, the
           cloud feel and the immersion sounds hold as in a pause (mission 2's take-off camera sits in the take-off
           set's stratus, C1 in_cloud 0.35 for ~12 s: the glass was wet and ticking when the flight began; bug #44/#41).
           ACZ_IMM_EVENT_HOLD=0: they run on during events (as before) */
        static int ev_hold = -1;
        if (ev_hold < 0) { const char *v = getenv("ACZ_IMM_EVENT_HOLD"); ev_hold = !(v && *v == '0'); }
        int ev = ev_hold && (ps2_r32(player + 0x1A68u) & 0x2000u);
        if (!ev) {
            ps2_imm_in[3] = (float)f;
            ps2_imm_audio[4] = (float)f;                       /* field stamp: stale = paused (ps2_audio.c) */
        }
        ps2_imm_audio[1] = ps2_imm_in[6] > 0.0f ? pn : 0.0f;
        ps2_imm_audio[2] = ps2_imm_in[6] > 0.0f ? 0.0f : pn;
        ps2_imm_audio[3] = v.speed_kt;
        {   /* a new mission (GameControl::enter), or the aircraft put somewhere else: more than 300 m (+ 600 m/s) between
               two flight frames, or more than 300 m across a stage (re)load -- the sky object or its cloud object's
               frame counter (sky +0x7F0 +0xD0) restarting: a RETRY, mission 2's take-off roll giving way to the flight
               (a jump of 1.6 km). The canopy and the cloud feel then start dry (ps2_video.c canopy_update). Before, a wet
               or iced glass carried over from the last mission (held through the menus), from before a RETRY, or from
               the take-off into the flight's start. A stage change in flight (no jump) keeps the glass.
               Bug #268 (ACZ_IMM_RESET_KEEP, see z_mission_init): a jump alone (a stage change within the mission: M18's
               phases, M12 after the nukes) keeps the canopy; the mission start, the mission (re)start 0x1D1F00 (RETRY,
               RESTART) and the aircraft's restore reset it. Every jump still starts a new flight (ps2_imm_flight). */
            extern volatile u32 ps2_imm_reset, ps2_imm_flight;
            static float lp[3]; static u64 lf; static u32 lsky, lcnt;
            u32 cnt = ok ? ps2_r32(sk + 0x7F0u + 0xD0u) : 0u;
            int restart = ok && lsky && (sk != lsky || cnt + 60u < lcnt);
            if (ok) { lsky = sk; lcnt = cnt; }
            float jump = 0.0f;
            if (lf) {
                float a = pm[0] - lp[0], b = pm[1] - lp[1], c = pm[2] - lp[2];
                jump = sqrtf(a * a + b * b + c * c);
                if (jump != jump) jump = 1e9f;
                u64 gap = f - lf;
                int tele = gap <= 8u ? jump > 300.0f + 10.0f * (float)gap : (restart && jump > 300.0f);
                if (!tele) jump = 0.0f;
            }
            memcpy(lp, pm, sizeof lp); lf = f;
            u32 req = z_imm_reset_req;
            if (req || jump > 0.0f) {
                if (jump > 0.0f && restart) z_stage_load_field = f;   /* (z_resume_hold) */
                if (!req && z_keep_on()) {                     /* bug #268: a stage change within the mission */
                    u32 u = zero_vr_player_unit();
                    int hp = u >= 0x100000u && u < 0x2000000u ? (int)(s16)ps2_r16(u + 0x398u) : -99999;
                    ps2_log("zero-vr: immersion: the aircraft moved %.0f m%s, field %llu: the canopy kept (no mission start, "
                            "RETRY, restart or restore behind it; the player's HP %d; ACZ_IMM_RESET_KEEP)", jump,
                            restart ? " across a stage load" : "", (unsigned long long)f, hp);
                } else {
                    if (jump > 0.0f) ps2_log("zero-vr: immersion: canopy state reset (the aircraft moved %.0f m%s, field %llu)",
                                             jump, restart ? " across a stage load" : "", (unsigned long long)f);
                    if (req == 2u)
                        ps2_log("zero-vr: immersion: canopy state reset (the player's aircraft restored: a respawn, field %llu)",
                                (unsigned long long)f);
                    else if (req == 3u)
                        ps2_log("zero-vr: immersion: canopy state reset (the mission restarted: RETRY / RESTART, field %llu)",
                                (unsigned long long)f);
                    else if (req || !restart)
                        ps2_log("zero-vr: immersion: canopy state reset (mission start, field %llu)", (unsigned long long)f);
                    ps2_imm_reset++;
                }
                z_imm_reset_req = 0;
                ps2_imm_flight++;
                memset(zd_k, 0, sizeof zd_k);                  /* (kills from before: elsewhere now) */
            }
        }
        imm_unit(player);
        {   extern volatile u64 ps2_z_ground_field;          /* zero_clouds.c: the cloud floor over a runway (bug #95) */
            if (ps2_r8(player + 0xEF0u + 0x236u) & 1u) ps2_z_ground_field = f;   /* FM+0x236 bit 0: on the ground */
        }
        gf_update(player, f, &v);                              /* G presence (wishes #77, #81, #82) */
        if (zb.on && !ev) zb_update(ctx, player, f);          /* test explosions (ACZ_DEBUG_FX_BOOM) */
        if (!ev) zuk_update(ctx, f);                           /* test kills of a unit (ACZ_DEBUG_UNIT_KILL) */
        if (!ev) imm_debris(player, f);                        /* debris of kills nearby (wish #47) */
        if (!ev) imm_blast(player, f);                         /* blasts nearby on the canopy (bug #232) */
        fl_own(player, f, &v, ev);                             /* flashes: own launches, gun, afterburner (wish #83) */
    }
    imm_log(player, f, v.alt_ft, v.speed_kt, v.g);
    cf_steer_update(player, f);                                  /* test autopilot (ACZ_DEBUG_CFIELD_STEER) */
    lf_update(player, f, &v);                                    /* test low-pass autopilot (ACZ_DEBUG_LOWFLY) */
    {   static int dbg = -1;
        if (dbg < 0) { const char *e = getenv("ACZ_DEBUG_INSTR"); dbg = e ? (atoi(e) > 1 ? atoi(e) : 120) : 0; }
        if (dbg && (f % (u64)dbg) == 0u)
            ps2_log("instr: field %llu speed %.0f kt alt %.0f ft hdg %.1f pitch %.1f roll %.1f vsi %.0f fpm g %.2f "
                    "power %.2f ab %.2f brake %.2f aoa %.1f stick %.2f %.2f %.2f thr_in %.2f msl %d gun %d sp %d "
                    "mode %u",
                    (unsigned long long)f, v.speed_kt, v.alt_ft, v.heading_deg, v.pitch_deg, v.roll_deg, v.vsi_fpm,
                    v.g, v.throttle, v.afterburner, v.airbrake, v.aoa_deg, v.stick[0], v.stick[1], v.stick[2],
                    f_of(ps2_r32(fm + 0x21Cu)), v.missiles, v.gun, v.special, ps2_r8(fm + 0x412u));
    }
    return 0;
}
/* Both maps at once (ck_f15 2026-10-29, ACZ_VR_MFD_DUAL, default 1): a cockpit with a radar scope beside its map
   display (screen 4 beside screen 1: the F-15C's TEWS and VSD; ps2_video.c MFD_SCR_SCOPE) shows the game's small radar
   on the scope and its big map on the display, both live. The game draws one of the two a frame: the HUD manager
   update 0x24D9D8 (a0 = hud = player + 0x1310) picks them from the map button in f12 -- hud +0xF0 == 1: held, f12 >
   0.1 = the big map; else a toggle, f12 > 0 flips the mode byte hud +0x36D (0 radar, 1 big map); hud +0x36F != 0 = a
   three-state radar (not seen; left alone). While the renderer asks for it (ps2_vk_mfd_dual: cockpit frames of such a
   model, both blocks on its screens) and the player's view is the cockpit (player +0x1AA4 == 2), this hook makes the
   game frames alternate: the radar, the big map, the radar...; each screen keeps its block's last image (30 Hz each at
   the game's 60). The big map's opening animation (hud +0x2FC the map input, +0x300 its size, +0x304 the target,
   +0x308 the start, +0x368 frames left, +0x369) restarts whenever the radar frame resets it, so its state is saved as
   the map frame left it (at the next call) and put back before each map frame: the map stays open. The player's own
   toggle presses go into a shadow mode that is put back when the alternation stops (another view, another model).
   Priority 5: the other hooks of 0x24D9D8 run first and see the player's own f12 (zero_syssnd's map click). */
static struct {
    int env, dbg, active, map, prev_map, have; u8 user; u32 hud; u32 snap[4]; u8 snap8[2]; u64 calls[2];
} mfdx;
/* The other aircraft for the rear-view mirrors (ck_f15 2026-10-29, ACZ_VR_COCKPIT_MIRRORS_AIRCRAFT; ps2_video.c
   mac_*): every aircraft the game updates passes its burner setup 0x1386D0 once a game frame (zero_trails.c: a2 = its
   position, a3 = its angles pitch, yaw, roll; the player's position at player +0x70); the HUD manager update is the
   frame: the positions and angles of the others (not the player) go to the renderer (ps2_mir_ac_put). Reads only. */
static struct { int on; u32 pos[48], ang[48]; int n; u64 field; } mac;
static int mac_burner(ps2_ctx *ctx, void *user) {
    (void)user;
    u32 a2 = ctx->r[6].uw[0], a3 = ctx->r[7].uw[0];
    if (a2 < 0x100000u || a2 >= 0x2000000u || a3 < 0x100000u || a3 >= 0x2000000u) return 0;
    for (int i = 0; i < mac.n; i++) if (mac.pos[i] == a2) return 0;
    if (mac.n < 48) { mac.pos[mac.n] = a2; mac.ang[mac.n] = a3; mac.n++; }
    return 0;
}
static void mac_tick(u32 player) {
    extern void ps2_mir_ac_put(int n, const float (*pos)[3], const float (*ang)[3]);
    u64 f = ps2_kernel_vblank_count();
    if (f == mac.field) return;
    mac.field = f;
    float pos[48][3], ang[48][3];
    int n = 0;
    for (int i = 0; i < mac.n; i++) {
        if (mac.pos[i] == player + 0x70u) continue;                  /* (the player) */
        for (int j = 0; j < 3; j++) {
            u32 w = ps2_r32(mac.pos[i] + 4u * (u32)j), v = ps2_r32(mac.ang[i] + 4u * (u32)j);
            memcpy(&pos[n][j], &w, 4); memcpy(&ang[n][j], &v, 4);
        }
        if (!(fabsf(pos[n][0]) < 1e7f && fabsf(pos[n][1]) < 1e7f && fabsf(pos[n][2]) < 1e7f && fabsf(ang[n][0]) < 100.0f
              && fabsf(ang[n][1]) < 100.0f && fabsf(ang[n][2]) < 100.0f)) continue;
        n++;
    }
    mac.n = 0;
    ps2_mir_ac_put(n, (const float (*)[3])pos, (const float (*)[3])ang);
}
static int mfdx_hook(ps2_ctx *ctx, void *user) {
    (void)user;
    extern volatile int ps2_vk_mfd_dual;
    if (mac.on) mac_tick(ctx->r[4].uw[0] - 0x1310u);
    if (!mfdx.env) {
        const char *e = getenv("ACZ_VR_MFD_DUAL"); mfdx.env = e && *e == '0' ? -1 : 1;
        e = getenv("ACZ_DEBUG_MFD_DUAL"); mfdx.dbg = e ? atoi(e) : 0;
    }
    u32 hud = ctx->r[4].uw[0], p = hud - 0x1310u;
    if (hud < 0x100000u || hud >= 0x2000000u) return 0;
    int hold = ps2_r8(hud + 0xF0u) == 1u;
    int want = mfdx.env > 0 && ps2_vk_mfd_dual && ps2_r8(p + 0x1AA4u) == 2u && ps2_r8(hud + 0x36Fu) == 0u;
    if (mfdx.active && hud != mfdx.hud) want = 0;      /* (a new flight: start over) */
    if (!want) {
        if (mfdx.active) {
            if (!hold && mfdx.hud == hud) ps2_w8(hud + 0x36Du, mfdx.user);   /* the player's own toggle state */
            ps2_log("zero-vr: both maps off (%llu radar / %llu map frames; ACZ_VR_MFD_DUAL)",
                    (unsigned long long)mfdx.calls[0], (unsigned long long)mfdx.calls[1]);
            mfdx.active = 0;
        }
        return 0;
    }
    if (!mfdx.active) {
        mfdx.active = 1; mfdx.hud = hud; mfdx.map = 0; mfdx.have = 0; mfdx.calls[0] = mfdx.calls[1] = 0;
        mfdx.user = (u8)(ps2_r8(hud + 0x36Du) & 1u);
        ps2_log("zero-vr: both maps on: the game draws the radar and the big map in turn (map button %s, mode %u; "
                "ACZ_VR_MFD_DUAL)", hold ? "held" : "toggles", (unsigned)mfdx.user);
    }
    if (!hold && ctx->f[12].f > 0.0f) mfdx.user ^= 1u;   /* the player's toggle press */
    mfdx.prev_map = mfdx.map;
    mfdx.map ^= 1;
    if (mfdx.prev_map) {                                  /* the map's state as its frame left it */
        for (int k = 0; k < 4; k++) mfdx.snap[k] = ps2_r32(hud + 0x2FCu + 4u * (u32)k);
        mfdx.snap8[0] = ps2_r8(hud + 0x368u); mfdx.snap8[1] = ps2_r8(hud + 0x369u);
        mfdx.have = 1;
    }
    if (mfdx.map && mfdx.have) {
        for (int k = 0; k < 4; k++) ps2_w32(hud + 0x2FCu + 4u * (u32)k, mfdx.snap[k]);
        ps2_w8(hud + 0x368u, mfdx.snap8[0]); ps2_w8(hud + 0x369u, mfdx.snap8[1]);
    }
    if (hold) ctx->f[12].f = mfdx.map ? 1.0f : 0.0f;
    else { ctx->f[12].f = 0.0f; ps2_w8(hud + 0x36Du, (u8)mfdx.map); }
    mfdx.calls[mfdx.map]++;
    if (mfdx.dbg > 0 && (mfdx.calls[0] + mfdx.calls[1]) % (u64)mfdx.dbg == 0u) {
        float f[4];
        for (int k = 0; k < 4; k++) { u32 w = ps2_r32(hud + 0x2FCu + 4u * (u32)k); memcpy(&f[k], &w, 4); }
        ps2_log("mfd-dual: field %llu %s frame: map input %.2f size %.3f target %.3f start %.3f left %u (%llu / %llu)",
                (unsigned long long)ps2_kernel_vblank_count(), mfdx.map ? "map" : "radar", f[0], f[1], f[2], f[3],
                (unsigned)ps2_r8(hud + 0x368u), (unsigned long long)mfdx.calls[0], (unsigned long long)mfdx.calls[1]);
    }
    return 0;
}
static void instr_install(void) {
    u32 w = ps2_r32(0x0024D9D8u);
    if ((w >> 16) != 0x27BDu) { ps2_log("zero-vr: instrument hook skipped (code mismatch %08X)", w); return; }
    if (ps2_hook_before(0x0024D9D8u, mfdx_hook, NULL, 5, "zero-mfd-dual") < 0)
        ps2_log("zero-vr: both maps (ACZ_VR_MFD_DUAL): hook FAILED");
    {   /* the other aircraft for the mirrors (mac_*): only with ACZ_VR_COCKPIT_MIRRORS_AIRCRAFT */
        extern int ps2_mir_ac_wanted(void);
        if (ps2_mir_ac_wanted()) {
            if (ps2_r32(0x001386D0u) >> 16 == 0x27BDu
                && ps2_hook_before(0x001386D0u, mac_burner, NULL, 40, "zero-mirror-ac") >= 0)
                mac.on = 1;
            ps2_log("zero-vr: the other aircraft for the mirrors %s (burner setup 0x1386D0; "
                    "ACZ_VR_COCKPIT_MIRRORS_AIRCRAFT)", mac.on ? "read" : "hook FAILED");
        }
    }
    {   /* bug #268: the mission (re)start (z_mission_init; logged always, a canopy reset with ACZ_IMM_RESET_KEEP) */
        int ok = ps2_r32(Z_MISSION_INIT) == 0x27BDFD90u
                 && ps2_hook_before(Z_MISSION_INIT, z_mission_init, NULL, 60, "zero-imm-reset") >= 0;
        ps2_log("zero-vr: canopy reset at a jump %s (ACZ_IMM_RESET_KEEP); the mission (re)start 0x1D1F00 %s",
                z_keep_on() ? "off: only at a mission start, a RETRY / RESTART or the aircraft's restore" : "on (as before)",
                ok ? "watched" : "hook FAILED");
        if (!ok) z_keep = 0;                             /* (no restart signal: the jumps keep resetting, as before) */
    }
    int r = ps2_hook_before(0x0024D9D8u, instr_hook, NULL, 60, "zero-instr");
    extern int (*ps2_vk_instr_get)(zero_instr *out);
    if (r >= 0) ps2_vk_instr_get = zero_instr_get;
    ps2_log("zero-vr: flight instruments %s (HUD manager update 0x24D9D8)", r >= 0 ? "read" : "hook FAILED");
}

static void vib_install(void) {
    const char *e = getenv("ACZ_RUMBLE");
    if (e && *e == '0') return;
    static const u32 w[4] = { 0x27BDFF60u, 0x2402C000u, 0xFFB40080u, 0x2403000Bu };
    for (int i = 0; i < 4; i++)
        if (ps2_r32(0x0030A168u + 4u * (u32)i) != w[i]) { ps2_log("zero-vr: rumble hook skipped (code mismatch)"); return; }
    int r = ps2_hook_before(0x0030A168u, vib_hook, NULL, 60, "zero-rumble");
    extern float ps2_imm_gun_rumble(void);
    int gun = ps2_imm_gun_rumble() > 0.0f;
    int pr = (zr_log() > 0 || gun) && ps2_r32(0x00110980u) == 0x27BDFFD0u
             && ps2_hook_before(0x00110980u, vib_pattern_hook, NULL, 60, "zero-rumble") >= 0;
    ps2_log("zero-vr: rumble %s%s%s", r >= 0 ? "on (sceVibSetActParam -> gamepad)" : "hook FAILED",
            pr && zr_log() > 0 ? "; the game's pattern requests logged (ACZ_RUMBLE_LOG)" : "",
            pr && gun ? "; the own gun buzzes the pad (ACZ_IMM_GUN_RUMBLE)" : "");
}

static void mc_fifo_install(void) {
    const char *e = getenv("ACZ_MC_FIFO");
    if (e && *e == '0') { ps2_log("zero-mc: completion queue off (ACZ_MC_FIFO=0)"); return; }
    int a = ps2_hook_replace(0x00179FC0u, mc_cb_queue, NULL, 60, "zero-mc-fifo");
    int b = ps2_hook_before(0x0017A5C8u, mc_step_take, NULL, 60, "zero-mc-fifo");
    int c = ps2_hook_before(0x0017A128u, mc_init_drop, NULL, 60, "zero-mc-fifo");
    ps2_log("zero-mc: save manager completion queue %s (%d %d %d)", a >= 0 && b >= 0 && c >= 0 ? "on" : "FAILED", a, b, c);
}

/* Debug boot: straight into a campaign mission, skipping the logos, title, menus, movie and briefing.

   ACZ_BOOT_MISSION=n   (1..18) boot straight into campaign mission n (index n-1)
   ACZ_BOOT_BRANCH=b    branch (missions 04/08/14 operation, 03/10/16 ace variant), default 0
   ACZ_BOOT_DIFF=d      0..5 (2 = Normal, inferred), default 2
   ACZ_BOOT_STAGE=s     override the stage id written to slot+0x5F4C (default: zb_tbl below; mission 1 = stage 0)
   ACZ_BOOT_AIRCRAFT=a  player aircraft id (default 15 = F-5E), ACZ_BOOT_WINGMAN=w (default 5 = Pixy's F-15C)
   ACZ_BOOT_HANGAR=1    keep the hangar (aircraft select) instead of going straight to flight
   ACZ_BOOT_SKIP=f      press Start f fields after the mission begins loading, which skips the in-engine intro flyby
                        (default 774: flying at ~field 900, 15 s after launch); 0 keeps the intro. Only while the game
                        offers a skip then -- an intro, a mission-start cutscene, the take-off (zb_skip_check): in a
                        mission whose intro is short (M4: the player flew from field 154) the press opened the pause
                        menu over the flight (bug #99)
   ACZ_BOOT_SKIP_CHECK=0  press Start at that field whatever plays (the old behaviour; default 1)
   ACZ_BOOT_MOVIES=1    [debug] keep the mission's sequence: its story movies, mission title and briefing play as in
                        the campaign (then the briefing waits for the pad); for FMV tests (2026-10-29, fmv_fix)
   ACZ_DEBUG_SCENES=1   log every scene change (factory, caller, depth, field) and the mission fields at flight start

   Scene system (SLUS-21346, static RE): main 0x100118 builds a ModeHolder (vtable 0x3C7EB0: +0x0C change, +0x10 push,
   +0x14 pop, +0x1C update, +0x20 draw; stack of 8 Mode* at +4, top index at +0x24) and pushes InitMode (vt 0x3CD130).
   InitMode's update 0x280E80 loads boot data (states 0-4), then (state 5, 0x2810D0) pushes a CModeSelector
   (vt 0x3CD210, ctor 0x281920) and calls setNext(0x281B20 = CModeWakeMemorycard factory).
   setNext 0x281A88(fn): 0x3B0C80[depth] = fn, byte 0x3FCF60 (gp-0x2110) = 0; depth = word 0x3FCF64 (gp-0x210C).
   A selector whose child popped calls the stored factory (0x281AD0) and pushes the new Mode, or pops itself if the
   factory returns 0 (0x281128). */

#define ZB_SETNEXT        0x00281A88u
#define ZB_F_WAKEMC       0x00281B20u   /* CModeWakeMemorycard */
#define ZB_F_GAMESEQ      0x00281C88u   /* ModeGameSeq: campaign sequence for slot+0x5F48 */
#define ZB_F_MOVIEPLAY    0x00282A28u   /* ModeMoviePlay: first child of ModeGameSeq */
#define ZB_F_HANGAR       0x002827C8u   /* CModeAircraftSelect */
#define ZB_F_RETURNGAME   0x00282778u   /* ModeGameSeqReturnGame -> GameSeq ends -> ModeGameControl (flight) */
#define ZB_RA_INITMODE    0x00281100u   /* return address of InitMode's setNext call (jal at 0x2810F8) */
#define ZB_RA_GAMESEQ     0x00286E64u   /* return address of ModeGameSeq::enter's setNext call (jal at 0x286E5C) */
#define ZB_GC_ENTER       0x00289F40u   /* ModeGameControl::enter (creates the 0x98FB0-byte game object) */
#define ZB_GC_EXIT        0x0028A140u   /* ModeGameControl::exit */
#define ZB_MISSION_TICK   0x001DD378u   /* per-frame mission update (flight only, inferred) */
#define ZB_SAVE           0x003B3810u   /* save/campaign base; slot A = +0x50, slot B = +0x5FE4, selector byte +0xC04C */

#define ZB_PLAYER_UPDATE  0x00231808u   /* the player update (a0 = player; ZV_PLAYER_UPDATE below) */
#define ZB_FRAME          0x00112700u   /* the sound listener update, once a frame from the main loop (Z_SND_LISTENER) */

static int zb_mission, zb_branch, zb_diff = 2, zb_hangar, zb_phase, zb_debug, zb_skip = 774, zb_skip_check_on = 1;
static int zb_movies;             /* ACZ_BOOT_MOVIES: keep the movies / briefing of the mission's sequence */
/* the intro skip (ACZ_BOOT_SKIP): the field it is due (0 = none), the pad player's last update (field) and its flags
   (player +0x1A68) then */
static volatile u64 zb_skip_at, zb_pl_field;
static volatile u32 zb_pl_flags;
static long zb_stage = -1, zb_aircraft = 15, zb_wingman = 5;   /* 15 = F-5E (reset default), 5 = F-15C (Pixy) */
/* Mission -> stage table {type, stage[3]} (type 1 = ace-style variant, 2 = operation choice), found verbatim in
   DATA.PAC members 1244 (+0x26FF50) and 1254 (+0x186240); the briefing exit 0x2848F8 computes
   stage = table[slot+0x5F48].stage[slot+0x5F50]. Index 18 = 0x12 (The Gauntlet, stage 30). */
static const signed char zb_tbl[19][4] = {
    {0,0,-1,-1},{0,1,-1,-1},{1,2,3,4},{2,6,5,7},{0,8,-1,-1},{0,9,-1,-1},{0,10,-1,-1},{2,11,12,13},{0,14,-1,-1},
    {1,15,16,17},{0,18,-1,-1},{0,19,-1,-1},{0,20,-1,-1},{2,21,22,23},{0,24,-1,-1},{1,25,26,27},{0,28,-1,-1},
    {0,29,-1,-1},{0,30,-1,-1} };
volatile int zero_in_mission;            /* ModeGameControl alive (loading + flight + pause + mission end) */
static int zv_mode, zv_debug;            /* start view (ACZ_VR_START_VIEW, see zv_install): 1 = cockpit; view debug log */
static volatile int zv_armed;            /* start view: set at GameControl::enter, cleared once settled (zv_player_update) */
static u32 zv_ev_kept = 0xFFu;           /* the pilot's view a mission event keeps (zv_skip_view), 0xFF none */
volatile u64 zero_last_mission_tick;     /* field of the last 0x1DD378 call: flying if recent */

static u32 zb_slot(void) { return ps2_r8(ZB_SAVE + 0xC04Cu) ? ZB_SAVE + 0x5FE4u : ZB_SAVE + 0x50u; }

static const char *zb_name(u32 f) {
    switch (f) {
    case 0x00281B20u: return "WakeMemorycard"; case 0x00281B98u: return "WakeLogo";
    case 0x00281BE8u: return "Title";          case 0x00281C38u: return "MainSelect";
    case 0x00281C88u: return "GameSeq";        case 0x00281CD8u: return "FreeMissionMenu";
    case 0x00281EE8u: return "CampaignSelect"; case 0x002827C8u: return "AircraftSelect(hangar)";
    case 0x00282818u: return "Shop";           case 0x00282868u: return "BriefingMovie";
    case 0x002829D8u: return "GameControl(flight)"; case 0x00282A28u: return "MoviePlay";
    case 0x00282A78u: return "CM";             case 0x00282AC8u: return "MissionTitle";
    case 0x00282B18u: return "Briefing";       case 0x00282B68u: return "GameEnd";
    case 0x00282BB8u: return "Debrief";        case 0x00282C18u: return "CampaignResult";
    case 0x00282778u: return "GameSeqReturnGame"; case 0x00282448u: return "GameSeqReturnSelect";
    case 0x002823F8u: return "GameSeqReturnReload"; case 0x00281128u: return "(none: selector pops)";
    default: return "?";
    }
}

/* ACZ_BOOT_SAVE_POKE=off:val[:bytes],... (debug, fx_mpbm 2026-10-31): values written into the save slot with the debug
   boot's aircraft (hex ok; bytes 1, 2 or 4, default 4) -- e.g. the hangar's choices the boot skips (the special weapon:
   0x28A548 hands the hangar's record +0x254 / +0x25C / +0x260 and three bytes per aircraft at +0x1E0 + 3 id on) */
static void zb_save_poke(u32 s) {
    const char *e = getenv("ACZ_BOOT_SAVE_POKE");
    while (e && *e) {
        char *q;
        unsigned long off = strtoul(e, &q, 0), val = 0, nb = 4;
        if (*q != ':') break;
        val = strtoul(q + 1, &q, 0);
        if (*q == ':') nb = strtoul(q + 1, &q, 0);
        if (off < 0xC000u) {
            if (nb == 1) ps2_w8(s + (u32)off, (u8)val);
            else if (nb == 2) ps2_w16(s + (u32)off, (u16)val);
            else ps2_w32(s + (u32)off, (u32)val);
            ps2_log("zero-boot: save slot +0x%lX = 0x%lX (%lu bytes; ACZ_BOOT_SAVE_POKE)", off, val, nb);
        }
        while (*q && *q != ',') q++;
        e = *q == ',' ? q + 1 : q;
    }
}
static int zb_setnext(ps2_ctx *ctx, void *user) {
    (void)user;
    u32 f = ctx->r[4].uw[0], ra = ctx->r[31].uw[0];
    if (zb_debug)
        ps2_log("zero-boot: setNext %08X %s from %08X depth %d field %llu", f, zb_name(f), ra,
                (int)ps2_r32(0x003FCF64u), (unsigned long long)ps2_kernel_vblank_count());
    if (!zb_mission) return 0;
    /* ACZ_DEBUG_BOOT_AGAIN=1 (test, menu_1105b: the next mission with a map the real menus' NEW GAME does not reach): a mission
       quit from the pause menu comes back as the debug boot's own, the title and menus skipped as at the start */
    static int zb_again = -1;
    if (zb_again < 0) { const char *a = getenv("ACZ_DEBUG_BOOT_AGAIN"); zb_again = a && *a == '1'; }
    if ((zb_phase == 0 && f == ZB_F_WAKEMC && ra == ZB_RA_INITMODE)
        || (zb_again && zb_phase >= 2 && f == 0x00281C38u && ra == 0x0028A364u)) {   /* (MainSelect, from GameControl's exit) */
        /* InitMode::enter already ran the save reset 0x2BFB48(0x3B3810) (index 0x13, stage -1, ...). Do the parts of
           the New Game handler 0x283728 that matter, then start the campaign sequence instead of the logos. */
        u32 s = zb_slot();
        ps2_w32(s + 0x5F48u, (u32)(zb_mission - 1));   /* mission index (0 = mission 01) */
        ps2_w32(s + 0x5F50u, (u32)zb_branch);
        ps2_w32(s + 0x5F54u, 0u);
        ps2_w32(s + 0x5F4Cu, 0xFFFFFFFFu);              /* stage id: written at GameControl::enter below */
        ps2_w32(s + 0x5F5Cu, (u32)zb_diff);              /* difficulty (copied to options +0x2B by 0x28ABF8) */
        ps2_w32(s + 0x5F60u, 0u);
        ps2_w32(s + 0x5F74u, 0u);                        /* New Game's control/mode arg (0x2C3590) */
        ps2_w32(ZB_SAVE + 0xBF80u, 0u);                  /* 0x3BF790: game mode (1 = free mission) */
        if (!zb_hangar) {                                /* what the hangar would write (read by 0x28A548) */
            ps2_w32(s + 0x24Cu, (u32)zb_aircraft);       /* player aircraft id 0..35 (CE table: 0x3B3AAC) */
            ps2_w32(s + 0x250u, (u32)zb_wingman);        /* wingman aircraft: 5 Pixy F-15C, 10 PJ F-16C */
        }
        zb_save_poke(s);                                 /* ACZ_BOOT_SAVE_POKE (debug) */
        ctx->r[4].ud[0] = ZB_F_GAMESEQ;
        zb_phase = 1;
        ps2_log("zero-boot: mission %d (index %d) branch %d difficulty %d, save slot %08X: logos/title/menu skipped",
                zb_mission, zb_mission - 1, zb_branch, zb_diff, s);
    } else if (zb_phase == 1 && f == ZB_F_MOVIEPLAY && ra == ZB_RA_GAMESEQ && zb_movies) {
        zb_phase = 2;
        ps2_log("zero-boot: the mission's movies and briefing play (ACZ_BOOT_MOVIES)");
    } else if (zb_phase == 1 && f == ZB_F_MOVIEPLAY && ra == ZB_RA_GAMESEQ) {
        /* GameSeq's child sequence: MoviePlay -> MissionTitle -> BriefingMovie -> Briefing -> CampaignSelect ->
           AircraftSelect -> GameSeqReturnGame. Jump to the hangar or straight to the return (= flight). */
        ctx->r[4].ud[0] = zb_hangar ? ZB_F_HANGAR : ZB_F_RETURNGAME;
        zb_phase = 2;
        ps2_log("zero-boot: movie/briefing skipped -> %s", zb_hangar ? "hangar" : "flight");
    }
    return 0;
}

static int zb_gc_enter(ps2_ctx *ctx, void *user) {
    (void)ctx; (void)user;
    u32 s = zb_slot();
    zero_in_mission = 1;
    zero_env_reload_mission(0);                  /* the NEXT MISSION tier: before the loader and the start view below */
    zv_armed = zv_mode;
    zv_ev_kept = 0xFFu;
    if (zr_log() > 0) {   /* the game's VIBRATION option: the mission init 0x1D1F00 sets the gate = (slot +0x5F70 == 0) */
        u32 v = ps2_r32(s + 0x5F70u);
        ps2_log("rumble: the game's own VIBRATION option is %s (save slot +0x5F70 = %u)", v == 0u ? "ON" : "OFF: the game "
                "sends no rumble (ours still runs)", v);
    }
    z_imm_reset_req = 1;
    {   /* the player's aircraft (the hangar wrote it; the native cockpit model is chosen by it) */
        extern volatile int ps2_vk_cockpit_aircraft;
        ps2_vk_cockpit_aircraft = (int)ps2_r32(s + 0x24Cu);
        ps2_log("zero-vr: mission start, player aircraft %d", ps2_vk_cockpit_aircraft);
    }
    {   /* the renderer fits its targets to the flight while the mission loads (a movie before it may have grown them) */
        extern volatile int ps2_vk_rt_refit;
        ps2_vk_rt_refit = 1;
    }
    if (zb_mission && ps2_r32(s + 0x5F4Cu) == 0xFFFFFFFFu) {
        long st = zb_stage;
        if (st < 0) {
            int i = zb_mission - 1, b = zb_branch < 0 || zb_branch > 2 ? 0 : zb_branch;
            st = zb_tbl[i][1 + b] >= 0 ? zb_tbl[i][1 + b] : zb_tbl[i][1];
        }
        ps2_w32(s + 0x5F4Cu, (u32)st);
        if (zb_skip > 0 && zb_phase == 2) {
            extern void ps2_pad_script_add(u64 field, const char *button);
            if (zb_skip_check_on) { zb_skip_at = ps2_kernel_vblank_count() + (u64)zb_skip; zb_pl_field = 0; }
            else ps2_pad_script_add(ps2_kernel_vblank_count() + (u64)zb_skip, "START");
            zb_phase = 3;
        }
    }
    ps2_log("zero-boot: GameControl::enter field %llu: index %d branch %d stage %d type %d diff %d gauge %d mode %d "
            "slot %08X gamemode %u", (unsigned long long)ps2_kernel_vblank_count(),
            (int)ps2_r32(s + 0x5F48u), (int)ps2_r32(s + 0x5F50u), (int)ps2_r32(s + 0x5F4Cu), (int)ps2_r32(s + 0x5F54u),
            (int)ps2_r32(s + 0x5F5Cu), (int)ps2_r32(s + 0x5F58u), (int)ps2_r32(s + 0x5F74u), s, ps2_r32(0x003BF790u));
    return 0;
}
static int zb_gc_exit(ps2_ctx *ctx, void *user) {
    (void)ctx; (void)user; zero_in_mission = 0; zv_armed = 0;
    if (zb_skip_at) { ps2_log("zero-boot: intro skip: the mission ended before it was due"); zb_skip_at = 0; }
    return 0;
}
/* The intro skip, once its field is due (ACZ_BOOT_SKIP, from the per-frame hooks below). Start skips what the game
   offers to skip -- an intro flyby, a mission-start cutscene, the take-off ("PRESS START BUTTON TO SKIP") -- but over
   the flight it opens the pause menu (bug #99: M4's intro had ended at field 154, the press at field 797 paused the
   game). The pad player's update (0x231808) and its flags (player +0x1A68) tell which (ACZ_DEBUG_VIEW / player flag
   logs, 2026-10-23): during an intro flyby the update does not run for it; flag 0x2000 = a mission event holds the
   camera (M2's take-off clearance, fields 153-515); flag 0x1000 = the take-off sequence (M2: from the clearance until
   the climb-out event ends, 0x110EC on the runway, cleared at field 2664 after the pull-up; never set in M1 / M4
   flight: 0x1004C). Flying = the update ran within the last 4 fields with neither flag. During the pause menu the
   update does not run either -- told apart from an intro by the HUD manager, which ran in the flight before a pause
   (z_instr_field) but not yet in an intro. */
#define ZB_PL_EVENT   0x2000u
#define ZB_PL_TAKEOFF 0x1000u
static void zb_skip_check(u64 f) {
    if (!zb_skip_at || f < zb_skip_at) return;
    extern void ps2_pad_script_add(u64 field, const char *button);
    u64 pf = zb_pl_field;
    u32 fl = zb_pl_flags;
    int upd = pf && f - pf <= 4u;
    int flying = upd && !(fl & (ZB_PL_EVENT | ZB_PL_TAKEOFF));
    int paused = !upd && z_instr_field && f - z_instr_field > 8u;
    if (!flying && !paused) ps2_pad_script_add(f, "START");
    ps2_log("zero-boot: intro skip due at field %llu (now %llu): %s (the pad player's update %s, flags %08X; HUD manager "
            "%s)", (unsigned long long)zb_skip_at, (unsigned long long)f,
            flying ? "the player flies already: no Start (ACZ_BOOT_SKIP_CHECK=0 presses anyway)"
            : paused ? "a menu is up: no Start"
            : !upd ? "the intro plays: Start pressed"
            : (fl & ZB_PL_EVENT) ? "a cutscene holds the camera: Start pressed" : "the take-off sequence: Start pressed",
            pf ? (upd ? "runs" : "stopped") : "not run yet", fl,
            z_instr_field ? (f - z_instr_field <= 8u ? "running" : "stopped") : "not run yet");
    zb_skip_at = 0;
}
static int zb_tick(ps2_ctx *ctx, void *user) {
    (void)ctx; (void)user;
    u64 f = ps2_kernel_vblank_count();
    zero_last_mission_tick = f;
    zb_skip_check(f);
    return 0;
}
/* once a frame (the main loop's sound listener update; the mission tick and the player's update do not run during an
   intro flyby) */
static int zb_frame(ps2_ctx *ctx, void *user) { (void)ctx; (void)user; zb_skip_check(ps2_kernel_vblank_count()); return 0; }
/* the pad's player update (installed with a due intro skip only): notes its field and flags */
static int zb_pl_update(ps2_ctx *ctx, void *user) {
    (void)user;
    u32 p = ctx->r[4].uw[0];
    u64 f = ps2_kernel_vblank_count();
    if (p >= 0x100000u && p < 0x2000000u) {
        u32 t = ps2_r32(p + 0x1A5Cu);
        if (t >= 0x100000u && t < 0x2000000u && ps2_r8(t + 0x1Fu) == 1u) {
            zb_pl_flags = ps2_r32(p + 0x1A68u);
            zb_pl_field = f;
        }
    }
    zb_skip_check(f);
    return 0;
}

/* Start view. The player's flight view is a byte at player +0x1AA4: 0 = the start view (first person with the HUD),
   2 = the 3D cockpit, 1 = the chase view. The player update 0x231808 (a0 = player, once per frame before the player
   view update 0x234308 that builds the frame's camera) cycles it on R3 (pad button 0x22: 0 -> 2 -> 1 -> 0; only for
   the pad's player, *(player +0x1A5C) +0x1F == 1, and not while flag 0x2000 of +0x1A68 is set), then takes a requested
   view from +0x1AA6 (0xFF = none), and on any change runs the game's own view switch (camera +0x14E, the view's screen
   distance, look-around reset). Calls with flag 2 of +0x1A68 set skip all of that. Mission events that take the camera
   set flag 0x2000 (R3 locked), keep the view in +0x1AA5 and request it back when they end. +0x1AA7 = 1 while the HUD
   is off: the player view update skips the HUD manager then. Measured with ACZ_DEBUG_VIEW=1 (2026-10-20): during
   mission 1's ~50 s intro flyby the update does not run for the pad's player; it first does in the frame control comes
   back, with the intro's request of the start view (0) still pending. Mission 2 starts with the "cleared for takeoff"
   cutscene as such an event (flag 0x2000, chase view requested, HUD manager running) and requests the kept view at
   its end. The pause menu's RETRY restarts in place (no GameControl::enter, the player keeps his view: the game
   requests it back after the intro -- unless that intro is skipped with START, see zv_skip_view); its REPLAY never
   runs the update for the pad's player. */
#define ZV_PLAYER_UPDATE 0x00231808u
/* ACZ_VR_SKIP_VIEW (pt_retry 2026-10-29; default on in VR, 0 = the game's own behaviour, 1 = also on a flat screen):
   the pilot's view back after a skipped mission event. A mission event that takes the camera (flag 0x2000: the
   "PRESS START BUTTON TO SKIP" approach of a mission's opening, also after RESTART / RETRY) keeps the pilot's view in
   +0x1AA5 and requests the chase view 1; played to its end it requests the kept view back. Skipped with START, the
   game drops that: by the next stage load the kept view has become the event's chase view (sim M17, F-15C, crash >
   RESTART > START at the approach: field 3501 view 2 kept 2 request 1 (the event), 4652 at the stage load view 1
   kept 1 request 1, then the flight in the chase view until R3). In the headset: "MISSION START" from behind,
   head-locked with the head on the stick, until the user pressed R3 twice (14:41:34-36.9, 14:44:08-10.8) or quit
   (14:48:45). The update in which the game takes that request (no event flag, request 1, kept 1) gets the view the
   event had kept (+0x1AA5 seen while the flag was set) instead, when that was not the chase view; R3 choices are
   untouched (R3 changes the view byte, not the request), and an event played to its end clears the memory (the game
   requests the kept view itself). The hand-over (ACZ_VR_HANDOVER_VIEW) then starts the tracked flight in the
   cockpit. */
static int zv_skip_on = -1;
static void zv_skip_view(u32 p, u64 f, u32 view, u32 kept, u32 req, u32 flags) {
    if (zv_skip_on < 0) {
        const char *e = getenv("ACZ_VR_SKIP_VIEW"), *xe = getenv("PS2_XR");
        zv_skip_on = e && *e ? *e != '0' : (xe && *xe == '1');
    }
    if (!zv_skip_on) return;
    if (flags & 0x2000u) {                           /* an event holds the camera: remember the view it keeps */
        if (kept <= 2u) zv_ev_kept = kept;
        return;
    }
    if (zv_ev_kept > 2u) return;
    if (req == 1u && kept == 1u && zv_ev_kept != 1u) {
        ps2_w8(p + 0x1AA6u, (u8)zv_ev_kept);
        ps2_w8(p + 0x1AA5u, (u8)zv_ev_kept);
        ps2_log("zero-vr: a skipped event's chase view replaced by the pilot's view %u (field %llu, view %u; "
                "ACZ_VR_SKIP_VIEW)", zv_ev_kept, (unsigned long long)f, view);
        zv_ev_kept = 0xFFu;
    } else if (req == 0xFFu && z_instr_field && f - z_instr_field <= 2u) zv_ev_kept = 0xFFu;   /* flying: done */
}
static int zv_player_update(ps2_ctx *ctx, void *user) {
    (void)user;
    u32 p = ctx->r[4].uw[0];
    if (p < 0x100000u || p >= 0x2000000u) return 0;
    u32 t = ps2_r32(p + 0x1A5Cu);
    if (t < 0x100000u || t >= 0x2000000u) return 0;
    u32 pad = ps2_r8(t + 0x1Fu) == 1u;
    u32 view = ps2_r8(p + 0x1AA4u), kept = ps2_r8(p + 0x1AA5u), req = ps2_r8(p + 0x1AA6u), hud_off = ps2_r8(p + 0x1AA7u);
    u32 flags = ps2_r32(p + 0x1A68u);
    u64 f = ps2_kernel_vblank_count();
    if (pad) { zv_view_now = (flags & 0x2000u) ? 0xFEu : view; zv_view_field = f; }   /* ACZ_VR_COCKPIT_HOLD */
    {   extern volatile u64 ps2_vk_fly_field;           /* the frame-time governor (ACZ_VR_GOV_INTRO): the player flies */
        if (pad && !(flags & 0x2000u)) ps2_vk_fly_field = f; }
    if (zv_debug) {     /* ACZ_DEBUG_VIEW=1: the view bytes of each player whenever they change */
        static struct { u32 p, key; } seen[4];
        u32 key = view | kept << 8 | req << 16 | hud_off << 24 | pad << 28 | ((flags & 0x2022u) != 0u) << 29
                  | (z_instr_field && f - z_instr_field <= 2u) << 30;
        int k = 0;
        while (k < 3 && seen[k].p && seen[k].p != p) k++;
        if (seen[k].p != p || seen[k].key != key)
            ps2_log("zero-vr: view field %llu player %08X pad %u view %u kept %u request %u hud-off %u flags %08X "
                    "HUD manager %s%s", (unsigned long long)f, p, pad, view, kept, req, hud_off, flags,
                    key >> 30 ? "running" : "idle", zv_armed ? " [armed]" : "");
        seen[k].p = p; seen[k].key = key;
    }
    if (pad) zv_skip_view(p, f, view, kept, req, flags);
    if (!zv_mode || !zv_armed || !pad || hud_off || (flags & 2u)) return 0;
    zv_armed = 0;
    if (flags & 0x2000u) {
        if (kept == 0u) ps2_w8(p + 0x1AA5u, 2u);
        ps2_log("zero-vr: start view: %s (field %llu, an event holds the camera)",
                kept == 0u ? "cockpit once the event ends" : "left as it is", (unsigned long long)f);
    } else if ((req == 0xFFu ? view : req) == 0u) {     /* the view this update ends with is the start view */
        ps2_w8(p + 0x1AA6u, 2u);
        ps2_log("zero-vr: start view: cockpit (field %llu)", (unsigned long long)f);
    } else ps2_log("zero-vr: start view: left as it is (field %llu, view %u, request %u)", (unsigned long long)f, view, req);
    return 0;
}
static void zv_read_mode(void) {                /* ACZ_VR_START_VIEW: also at the edge (zero_env_reload_mission) */
    const char *e = ps2_menv("ACZ_VR_START_VIEW");
    if (e && *e) zv_mode = !strcmp(e, "cockpit");
    else {
        const char *xe = getenv("PS2_XR"), *xm = getenv("PS2_XR_MODE"), *cm = getenv("ACZ_VR_COCKPIT_MODEL");
        zv_mode = xe && *xe == '1' && xm && !strncmp(xm, "stereo", 6) && cm && *cm && strcmp(cm, "none") != 0;
    }
}
static void zv_install(void) {
    zv_debug = getenv("ACZ_DEBUG_VIEW") != NULL;
    /* ACZ_VR_START_VIEW=cockpit|game: the view at the start of every mission. cockpit = the R3 cockpit view from the
       first frame the player flies (once per GameControl::enter, after the intro flyby; when a mission-start cutscene
       holds the camera then, e.g. mission 2's takeoff clearance, the game switches to the cockpit as it ends; a view
       already changed by R3 is left alone), R3 cycles the views as before; game = the game's own start view (first
       person with the HUD). Default: cockpit with stereo head tracking (PS2_XR=1, PS2_XR_MODE=stereo*: the launcher's
       stereo, stick and sharp modes) and a 3D cockpit model (ACZ_VR_COCKPIT_MODEL: the launcher's `cockpit` word), game
       otherwise. ACZ_DEBUG_VIEW=1 logs the player's view bytes whenever they change. */
    zv_read_mode();
    const char *e = getenv("ACZ_VR_COCKPIT_HOLD");      /* the game's view holds the cockpit view (z_in_cockpit) */
    zv_hold_on = !(e && *e == '0');
    {   /* the frame-time governor counts only frames with the flight camera (ps2_video.c hgov_flying, ACZ_VR_GOV_INTRO) */
        const char *gi = getenv("ACZ_VR_GOV_INTRO"), *xe = getenv("PS2_XR");
        int gov_intro = !(gi && *gi == '0') && xe && *xe == '1';
        const char *ef = ps2_menv_boot("ACZ_VR_EVENT_FLAT");   /* (the event camera's flag: z_event_flat_now) */
        int event_flat = !(ef && *ef == '0') && xe && *xe == '1';
        if (!zv_mode && !zv_debug && !zv_hold_on && !gov_intro && !event_flat) return;
        if (ps2_r32(ZV_PLAYER_UPDATE) != 0x27BDFD70u) { zv_hold_on = 0; ps2_log("zero-vr: start view: code mismatch; off"); return; }
    }
    int r = ps2_hook_before(ZV_PLAYER_UPDATE, zv_player_update, NULL, 60, "zero-startview");
    if (r < 0) zv_hold_on = 0;
    else {
        const char *gi = getenv("ACZ_VR_GOV_INTRO");
        extern volatile int ps2_vk_fly_known;
        ps2_vk_fly_known = !(gi && *gi == '0');
    }
    ps2_log("zero-vr: start view %s%s%s", r < 0 ? "hook FAILED" : zv_mode ? "cockpit (ACZ_VR_START_VIEW)" : "game",
            zv_debug ? ", view debug log" : "",
            zv_hold_on ? "; cockpit view held while the game's view is the cockpit (ACZ_VR_COCKPIT_HOLD=0 off)" : "");
}

/* ---- gaze targeting (ACZ_VR_GAZE_TARGET) ----
   The target button (Y / Triangle, the game's action 0x0E) selects the target the head looks at instead of the one
   ahead of the nose, in head-tracked frames (the cockpit view with ACZ_VR_HEADLOOK=stick). Everything else stays the
   game's: its candidate list, its validity test, its range, its "set target" epilogue and so locking, missiles,
   radar and HUD.
   The game's target system: TS = player + 0x1880, updated once per frame by 0x251A48 (from the player update
   0x231808, pad player only). TS+0x84 = team table (as player+0x1A5C: *team = group slot array, team+0x1C group
   count; group+0x104 unit records of 0x4A0 bytes, group+0x108 unit count; unit +0x260 position, +0x270 angles,
   +0x1F8 flags), TS+0x96/0x97 own group/index, TS+0x98/0x99 the current target (group, index; 0xFF = none),
   TS+0x7F/0x80 the same for the lock-on slots (TS+0, 0x1C bytes each, TS+0x7C of them, reset by 0x251630),
   TS+0x8C switch range (units), TS+0x90 cosine of the nose cone, TS+0x94 u16 timer (2 x TS+0x9B frames after each
   switch), TS+0x9A flags: bit 0 = switch request, set by 0x251860(TS, 0) when the target button is released before
   the hold threshold (player +0x1AB6 counts the held frames: holding it is the game's target camera). The update
   then calls 0x251E48 (no target, or the timer has run out: the valid target with the largest cosine to the nose
   within cone and range, the current one excluded; else the nearest in range) or 0x252410 (a quick re-press while
   the timer runs: the next one in list order within cone and range), then 0x250E90, and clears the request bits.
   Validity = 0x251988(TS, unit) (alive, not hidden, the other side, air or ground by the weapon), groups pass
   0x2518A8(TS, &slot).
   Both selection functions are replaced while a press is pending and the head turns the camera: every unit the game
   would consider (same tests, same range) is measured against the gaze (the camera rotation composed with the head
   pose, from the camera eye; the head's forward axis turned ACZ_VR_GAZE_PITCH degrees down, where the eyes rest --
   the Quest has no eye tracking). Within ACZ_VR_GAZE_TARGET degrees of the gaze the press takes the one nearest to
   the gaze. When that one is the current target already (the head looks at it), the press takes the next one
   outward within ACZ_VR_GAZE_CYCLE degrees of the gaze, and quick re-presses with a steady head (within 3 s of the
   last gaze pick, the gaze moved less than 3 deg since) walk on outward through that tight cone and wrap -- so
   pressing again while looking at a group cycles through it; nothing else in the tight cone: the current target
   stays (the press changes nothing, the lock-on goes on). Nothing in the cone: the game's own function runs (its
   nose choice). pt_gaze 2026-10-29 (the 10:50 session, "it aimed not where I look"): the old rule took, whenever the
   current target was anywhere in the 20 deg cone, the next one outward from it in gaze angle -- picks 14-18 deg
   away while the head looked at a group 3-9 deg off, and with only the current one in the cone the game's nose pick;
   ACZ_VR_GAZE_CYCLE=0 = that old rule. pt_target 2026-10-29 eve (bug #234, M17's canyon and tunnel: "it selected a
   target far away behind a wall", "none of these targets got marked"): the scan stopped at group 64 while M17 has
   more (its mission targets sat in groups 65-70), so the targets the head looked at were never candidates and the
   press took a far AA gun or kept it; now every group is scanned as the game's own picks do (ACZ_DEBUG_GAZE_GROUPS).
   And the press prefers what can be seen: each candidate in the cone gets a line of sight from the eye through the
   game's own segment test (terrain and static objects: walls, the dam, the tunnel; zg_seg); hidden ones drop out
   while anything in the cone is visible (ACZ_VR_GAZE_VISIBLE); among the rest the best score wins = the gaze angle
   + ACZ_VR_GAZE_NEAR degrees per km + ACZ_VR_GAZE_TGT degrees for a target that is not a mission target (the HUD's
   red TGT: unit +0x1FC bit 0, 0x241398) + ACZ_VR_GAZE_FRONT degrees outside the game's nose cone (no lock there);
   the walk and the tight cone go by that score. Without a press (the game's automatic pick when the target is lost) the game
   is untouched, and so are the special weapons' multi-target mode (flag 0x80: the update skips the switch request)
   and views where the head does not turn the camera (outside the cockpit in stick head look). Each press logs one
   line (the pick and how it was chosen, the head's direction from the nose, the current target's gaze angle, the
   counts in the cones (visible, hidden, TGT), the pick by angle alone, the old rule's pick; distances in metres as
   the HUD's, 5.12 units a metre -- before they used ACZ_VR_UNITS_PER_M 6); ACZ_DEBUG_GAZE=1 adds every candidate
   with its distance, TGT, sight (and where a hidden one's line hits), its angles to the gaze and to the nose, its
   score, its yaw / pitch from the head's axis and its position, and the game's would-be nose pick (with
   ACZ_VR_GAZE_TARGET=0: what the gaze would have picked), =2 every unit with the game's tests;
   ACZ_DEBUG_GAZE_PROBE=N lists the candidates with their sight every N fields without a press. ACZ_VR_GAZE_TARGET:
   unset or 1 = on with a 20 degree cone (half-angle), N (3-60) = an N degree cone, 0 = off. Cost: two byte reads
   per frame; a press runs the game's tests once per group and unit and one segment test per candidate in the cone. */
#define ZG_TS_UPDATE 0x00251A48u
#define ZG_PICK_NOSE 0x00251E48u
#define ZG_PICK_NEXT 0x00252410u
#define ZG_VALID     0x00251988u
#define ZG_GROUP_OK  0x002518A8u
#define ZG_SET_DONE  0x00251630u
#define ZG_WALK_FIELDS 180u                 /* a quick re-press: within 3 s of the last gaze pick ... */
#define ZG_WALK_DEG    3.0f                 /* ... with the gaze moved less than this since */
static float zg_cone_deg = 20.0f;          /* cone half-angle, degrees */
static float zg_cycle_deg = 8.0f;          /* ACZ_VR_GAZE_CYCLE: the tight cone of a press on the current target; 0 = old */
static float zg_pitch_deg = 2.0f;          /* ACZ_VR_GAZE_PITCH: the gaze below the head's forward axis, degrees */
static int zg_on;                          /* ACZ_VR_GAZE_TARGET != 0 (off + ACZ_DEBUG_GAZE: log what it would pick) */
static int zg_dbg;                          /* ACZ_DEBUG_GAZE */
static u32 zg_ts;                           /* the pad player's target system (set by the update hook) */
static int zg_press;                        /* the update in progress has a switch request */
static u8 zg_cur_g = 0xFFu, zg_cur_i = 0xFFu;   /* target before the update (debug log) */
static u8 zg_last_g = 0xFFu, zg_last_i = 0xFFu; /* the last gaze pick (or kept target), its field and gaze */
static u64 zg_last_field;
static float zg_last_gz[3];
static u64 zg_presses, zg_taken, zg_none, zg_kept, zg_differ;
static int zg_ptr(u32 p) { return p >= 0x100000u && p < 0x2000000u; }
/* Call a game function from a hook; registers are saved and restored around it; a1 may point to 16 bytes of
   scratch above the callee's stack (scratch != 0: *a1 = scratch value). Returns v0. */
static u32 zg_call(ps2_ctx *ctx, u32 fn, u32 a0, u32 a1, int use_scratch, u32 scratch) {
    ps2_reg128 rs[32];
    ps2_fpr fs[32];
    ps2_reg128 hi = ctx->hi, lo = ctx->lo;
    float acc = ctx->acc;
    u32 fcc = ctx->fcc, fcr = ctx->fcr31, sa = ctx->sa, pc = ctx->pc;
    memcpy(rs, ctx->r, sizeof rs);
    memcpy(fs, ctx->f, sizeof fs);
    u32 sp = (ctx->r[29].uw[0] - 0x100u) & ~15u;
    if (use_scratch) { ps2_w32(sp + 0xC0u, scratch); a1 = sp + 0xC0u; }
    ctx->r[4].ud[0] = (u64)(s64)(s32)a0; ctx->r[5].ud[0] = (u64)(s64)(s32)a1;
    ctx->r[29].ud[0] = (u64)(s64)(s32)sp;
    ctx->r[31].ud[0] = 0;
    ps2_dispatch(ctx, fn);
    u32 v0 = ctx->r[2].uw[0];
    memcpy(ctx->r, rs, sizeof rs);
    memcpy(ctx->f, fs, sizeof fs);
    ctx->hi = hi; ctx->lo = lo; ctx->acc = acc; ctx->fcc = fcc; ctx->fcr31 = fcr; ctx->sa = sa; ctx->pc = pc;
    return v0;
}
/* The head: the flight camera (player +0xB70, the game's angles; our head injection is undone after each update)
   turned by the head pose exactly as apply_head does: R = the camera, N = the head's frame (columns: right, up,
   back). 0 when the head does not turn the camera. */
static int zg_head(u32 player, float eye[3], float R[3][3], float N[3][3]) {
    if (z_xr < PS2_XR_MONO || !z_engaged_now || !z_pose.valid || (z_stick && !z_in_cockpit())) return 0;
    if (z_instr_field == 0 || ps2_kernel_vblank_count() - z_instr_field > 8u) return 0;     /* menu over the flight */
    u32 c = player + 0xB70u;
    if (ps2_r32(c) != 0x44200000u || ps2_r32(c + 4u) != 0x43F00000u) return 0;            /* not the 640x480 camera */
    float H[3][3];
    for (u32 i = 0; i < 3; i++) eye[i] = f_of(ps2_r32(c + 0x30u + 4u * i));
    m3_euler(f_of(ps2_r32(c + 0x40u)), f_of(ps2_r32(c + 0x44u)), f_of(ps2_r32(c + 0x48u)), R);
    m3_quat(z_pose.q, H);
    if (z_test_yaw != 0.0f || z_test_pitch != 0.0f) {      /* as apply_head: H = H * Ry(test_yaw) * Rx(test_pitch) */
        float T[3][3], U[3][3];
        m3_euler(z_test_pitch * 0.01745329f, z_test_yaw * 0.01745329f, 0.0f, T);
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) U[i][j] = H[i][0] * T[0][j] + H[i][1] * T[1][j] + H[i][2] * T[2][j];
        memcpy(H, U, sizeof H);
    }
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) N[i][j] = R[i][0] * H[0][j] + R[i][1] * H[1][j] + R[i][2] * H[2][j];
    return 1;
}
/* yaw (+ = left) and pitch (+ = up) of world direction w in the frame R (camera forward = -z), degrees */
static void zg_cam_angles(const float R[3][3], const float w[3], float *yaw, float *pitch) {
    float lx = R[0][0] * w[0] + R[1][0] * w[1] + R[2][0] * w[2];
    float ly = R[0][1] * w[0] + R[1][1] * w[1] + R[2][1] * w[2];
    float lz = R[0][2] * w[0] + R[1][2] * w[1] + R[2][2] * w[2];
    *yaw = atan2f(-lx, -lz) * 57.29578f;
    *pitch = asinf(ly < -1.0f ? -1.0f : ly > 1.0f ? 1.0f : ly) * 57.29578f;
}
static float zg_angle(const float a[3], const float b[3]) {      /* degrees between unit vectors */
    float c = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    return acosf(c < -1.0f ? -1.0f : c > 1.0f ? 1.0f : c) * 57.29578f;
}
/* ---- visibility (pt_target 2026-10-29, bug #234): the game's own segment test 0x16F720(world, out, kind, from, to)
   -- the one its bullets, missiles and aircraft use (35 call sites): a cell walk (2048 units) from `from` to `to`
   testing the static map objects' collision models (0x15ECE0 -> 0x133A20: the dam, the tunnel, bridges) and the
   terrain's triangles (0x159238 over the heightmap); v0 = 1 on a hit, out = the first hit {x, y, z, t} along the
   walk, *kind = what was hit; both endpoints above the map's highest point (0x3F8658) = no hit. `world` = a pointer
   to {terrain T, objects O} as every caller passes it: taken from the game's own calls (zg_seg_enter). The line of
   sight runs from the camera eye to a point ZG_LOS_UP above the unit's origin; a hit within ZG_LOS_NEAR of that
   point is the target's own ground or structure (an AA gun on the dam's crest): visible. Nothing is written but
   the object list's per-frame test marks the walk itself clears (0x15F420), as for a missile. */
#define ZG_SEGMENT   0x0016F720u
#define ZG_UPM       5.12f                  /* the game's units per metre (the altimeter; the HUD's feet = m / 0.3048) */
#define ZG_LOS_UP    (5.0f * ZG_UPM)        /* the line of sight aims 5 m above the unit's origin */
#define ZG_LOS_NEAR  (40.0f * ZG_UPM)       /* a hit within 40 m of that point: the target's own ground: visible */
#define ZG_LOS_MAX   24                     /* lines of sight per press at most (the best by score; the rest count as seen) */
static u32 zg_world;                        /* {T, O} of the game's segment tests (0 until one ran) */
static int zg_world_hooked;                 /* zg_seg_enter / zg_pl_* installed (the gaze's sight or the tunnels' rays) */
static int zg_vis_on = 1;                   /* ACZ_VR_GAZE_VISIBLE */
static float zg_near_deg = 1.0f;            /* ACZ_VR_GAZE_NEAR: degrees added per km of distance */
static float zg_tgt_deg = 3.0f;             /* ACZ_VR_GAZE_TGT: degrees added to a target that is not a mission target */
static float zg_front_deg = 2.0f;           /* ACZ_VR_GAZE_FRONT: degrees added outside the game's nose cone */
static int zg_groups_max = 255;             /* ACZ_DEBUG_GAZE_GROUPS: groups scanned (64 = the cap before 2026-10-29 eve) */
static int zg_probe;                        /* ACZ_DEBUG_GAZE_PROBE: every N fields the candidates and their sight */
/* ACZ_DEBUG_GAZE_TP="field:x,y,z,heading[,pitch] ..." [test]: from that field the pad player's aircraft is put at x, y,
   z (units) with that heading and pitch (degrees, wings level) for 2 fields -- a scripted start inside M17's canyon.
   The position and the attitude live three times in the player: +0x70 / +0x80, +0x1070 / +0x1080, +0x12A0 / +0x12B0
   (FM+0x3B0 / +0x3C0); all three are written. */
static struct { u64 f; float p[5]; } zg_tp[12];
static int zg_tpn;
static u64 zg_los_n, zg_los_hidden, zg_hid_dropped, zg_angle_differ, zg_los_ns, zg_los_max_ns;
static int zg_in_pad;                       /* inside the pad player's update (0x231808): its own collision tests */
static int zg_pl_enter(ps2_ctx *ctx, void *user) {
    (void)user;
    u32 player = ctx->r[4].uw[0], t = zg_ptr(player) ? ps2_r32(player + 0x1A5Cu) : 0u;
    zg_in_pad = zg_ptr(t) && ps2_r8(t + 0x1Fu) == 1u;
    return 0;
}
static int zg_pl_leave(ps2_ctx *ctx, void *user) { (void)ctx; (void)user; zg_in_pad = 0; return 0; }
static int zg_seg_enter(ps2_ctx *ctx, void *user) {
    (void)user;
    u32 w = ctx->r[4].uw[0];
    if (zg_dbg >= 3) {                                  /* which callers use which world (distinct pairs) */
        static u32 seen[64][2]; static int ns;
        u32 ra = ctx->r[31].uw[0];
        int k = 0;
        while (k < ns && !(seen[k][0] == ra && seen[k][1] == w)) k++;
        if (k == ns && ns < 64) {
            seen[ns][0] = ra; seen[ns][1] = w; ns++;
            ps2_log("zero-vr: gaze test: segment test from %08X world %08X (field %llu)", ra, w,
                    (unsigned long long)ps2_kernel_vblank_count());
        }
    }
    /* M17 keeps two maps (the intro's and the flight's), each with its own world, and tests units against theirs:
       only the pad player's own collision tests (0x231808 -> 0x239C40 -> 0x23A490) tell which one it flies in */
    if (!zg_in_pad || w == zg_world || !zg_ptr(w) || !zg_ptr(ps2_r32(w)) || !zg_ptr(ps2_r32(w + 4u))) return 0;
    static int logged;
    if (zg_dbg || logged < 4) {
        logged++;
        ps2_log("zero-vr: gaze target: line of sight through the game's segment test: world %08X (terrain %08X, "
                "objects %08X, %u instances; field %llu)", w, ps2_r32(w), ps2_r32(w + 4u), ps2_r32(ps2_r32(w + 4u) + 0x20u),
                (unsigned long long)ps2_kernel_vblank_count());
    }
    zg_world = w;
    return 0;
}
/* the segment test from `from` to `to` (units); 1 = hit (*hit = the hit point, *t = its fraction), 0 = clear,
   -1 = no world yet */
static int zg_seg(ps2_ctx *ctx, const float from[3], const float to[3], float hit[3], float *t) {
    u32 w = zg_world;
    if (!zg_ptr(w) || !zg_ptr(ps2_r32(w)) || !zg_ptr(ps2_r32(w + 4u))) return -1;
    ps2_reg128 rs[32];
    ps2_fpr fs[32];
    ps2_reg128 hi = ctx->hi, lo = ctx->lo;
    float acc = ctx->acc;
    u32 fcc = ctx->fcc, fcr = ctx->fcr31, sa = ctx->sa, pc = ctx->pc;
    memcpy(rs, ctx->r, sizeof rs);
    memcpy(fs, ctx->f, sizeof fs);
    u32 base = (ctx->r[29].uw[0] - 0x400u) & ~15u;     /* the callee's stack; our vectors above it */
    u32 pf = base + 0x100u, pt = base + 0x110u, po = base + 0x120u, pk = base + 0x130u;
    for (u32 k = 0; k < 3; k++) {
        u32 a, b;
        memcpy(&a, &from[k], 4); memcpy(&b, &to[k], 4);
        ps2_w32(pf + 4u * k, a); ps2_w32(pt + 4u * k, b); ps2_w32(po + 4u * k, 0u);
    }
    ps2_w32(pf + 12u, 0x3F800000u); ps2_w32(pt + 12u, 0x3F800000u); ps2_w32(po + 12u, 0u); ps2_w32(pk, 0u);
    ctx->r[4].ud[0] = (u64)(s64)(s32)w; ctx->r[5].ud[0] = (u64)(s64)(s32)po; ctx->r[6].ud[0] = (u64)(s64)(s32)pk;
    ctx->r[7].ud[0] = (u64)(s64)(s32)pf; ctx->r[8].ud[0] = (u64)(s64)(s32)pt;
    ctx->r[29].ud[0] = (u64)(s64)(s32)base;
    ctx->r[31].ud[0] = 0;
    struct timespec c0, c1;
    clock_gettime(CLOCK_MONOTONIC, &c0);
    ps2_dispatch(ctx, ZG_SEGMENT);
    clock_gettime(CLOCK_MONOTONIC, &c1);
    u64 ns = (u64)((c1.tv_sec - c0.tv_sec) * 1000000000ll + (c1.tv_nsec - c0.tv_nsec));
    zg_los_ns += ns;
    if (ns > zg_los_max_ns) zg_los_max_ns = ns;
    u32 v0 = ctx->r[2].uw[0];
    for (u32 k = 0; k < 3; k++) hit[k] = f_of(ps2_r32(po + 4u * k));
    *t = f_of(ps2_r32(po + 12u));
    memcpy(ctx->r, rs, sizeof rs);
    memcpy(ctx->f, fs, sizeof fs);
    ctx->hi = hi; ctx->lo = lo; ctx->acc = acc; ctx->fcc = fcc; ctx->fcr31 = fcr; ctx->sa = sa; ctx->pc = pc;
    zg_los_n++;
    return v0 ? 1 : 0;
}

#define ZG_MAXC 512
typedef struct {
    u8 g, i, tgt, front;
    s8 vis;                                 /* 1 visible, 0 hidden behind terrain / objects, -1 not tested / unknown */
    float ang, nc, dist, score, occ, occt;  /* gaze angle (deg), cosine to the nose, distance (units), score, occluder (m),
                                               the hit's fraction of the line */
    u32 u;
} zg_cand;
typedef struct {
    float eye[3], R[3][3], N[3][3], ax[3], gz[3], op[3], nose[3], range, nose_cos;
    u32 garr, ng, ngs;
    u8 cg, ci;
    int nc, nvalid, cur_k;
    float cur_ang;
    zg_cand c[ZG_MAXC];
} zg_scan_t;
static zg_scan_t zg_s;                      /* EE thread only (the player update) */
/* the line of sight to candidate k: sets vis and occ */
static void zg_sight(ps2_ctx *ctx, zg_scan_t *s, int k) {
    zg_cand *c = &s->c[k];
    float tp[3], hit[3], t = 0.0f;
    for (u32 j = 0; j < 3; j++) tp[j] = f_of(ps2_r32(c->u + 0x260u + 4u * j));
    tp[1] += ZG_LOS_UP;
    int r = zg_seg(ctx, s->eye, tp, hit, &t);
    c->occ = -1.0f;
    if (r < 0) { c->vis = -1; return; }
    if (r == 0) { c->vis = 1; return; }
    float de = 0.0f, dh = 0.0f;
    for (u32 j = 0; j < 3; j++) {
        float a = tp[j] - s->eye[j], b = hit[j] - s->eye[j];
        de += a * a; dh += b * b;
    }
    de = sqrtf(de); dh = sqrtf(dh);
    if (!(dh >= 0.0f && dh <= de * 1.01f + 1.0f)) dh = t * de;      /* the hit point unreadable: its fraction */
    c->vis = de - dh <= ZG_LOS_NEAR ? 1 : 0;
    c->occ = dh / ZG_UPM; c->occt = t;
    if (!c->vis) zg_los_hidden++;
}
/* the game's candidates (its group and validity tests, its switch range) measured against the gaze; 0 = no head */
static int zg_scan(ps2_ctx *ctx, u32 ts, zg_scan_t *s) {
    u32 player = ps2_r32(ts + 0x88u), team = ps2_r32(ts + 0x84u);
    s->nc = s->nvalid = 0; s->cur_k = -1; s->cur_ang = -1.0f;
    if (!zg_ptr(player) || !zg_ptr(team) || !zg_head(player, s->eye, s->R, s->N)) return 0;
    {   /* the head's forward axis (-z) and the gaze: that axis turned down by the eyes' pitch, local (0, -sin, -cos) */
        float ps = sinf(zg_pitch_deg * 0.01745329f), pc = cosf(zg_pitch_deg * 0.01745329f);
        for (int i = 0; i < 3; i++) { s->ax[i] = -s->N[i][2]; s->gz[i] = -s->N[i][2] * pc - s->N[i][1] * ps; }
    }
    s->garr = ps2_r32(team); s->ng = ps2_r8(team + 0x1Cu);
    u8 og = ps2_r8(ts + 0x96u), oi = ps2_r8(ts + 0x97u);
    s->cg = ps2_r8(ts + 0x98u); s->ci = ps2_r8(ts + 0x99u);
    if (!zg_ptr(s->garr) || og >= s->ng) return 0;
    u32 ogrp = ps2_r32(s->garr + 4u * og);
    if (!zg_ptr(ogrp)) return 0;
    u32 own = ps2_r32(ogrp + 0x104u) + (u32)oi * 0x4A0u;
    s->range = f_of(ps2_r32(ts + 0x8Cu)); s->nose_cos = f_of(ps2_r32(ts + 0x90u));
    float NR[3][3];
    for (u32 k = 0; k < 3; k++) s->op[k] = f_of(ps2_r32(own + 0x260u + 4u * k));
    m3_euler(f_of(ps2_r32(own + 0x270u)), f_of(ps2_r32(own + 0x274u)), f_of(ps2_r32(own + 0x278u)), NR);
    for (int k = 0; k < 3; k++) s->nose[k] = -NR[k][2];
    s->ngs = s->ng < (u32)zg_groups_max ? s->ng : (u32)zg_groups_max;
    for (u32 g = 0; g < s->ngs; g++) {                  /* every group, as the game's own picks (0x251E48 / 0x252410) */
        u32 slot = s->garr + 4u * g;
        if (!zg_call(ctx, ZG_GROUP_OK, ts, 0, 1, slot)) continue;
        u32 grp = ps2_r32(slot);
        if (!zg_ptr(grp)) continue;
        u32 recs = ps2_r32(grp + 0x104u);
        int cnt = (int)ps2_r32(grp + 0x108u);
        for (int i = 0; i < cnt && i < 256; i++) {
            u32 u = recs + (u32)i * 0x4A0u;
            if (!zg_ptr(u) || !zg_call(ctx, ZG_VALID, ts, u, 0, 0)) continue;
            float d[3], e[3], dl = 0.0f, el = 0.0f;
            for (u32 k = 0; k < 3; k++) {
                float p = f_of(ps2_r32(u + 0x260u + 4u * k));
                d[k] = p - s->op[k]; e[k] = p - s->eye[k];
                dl += d[k] * d[k]; el += e[k] * e[k];
            }
            dl = sqrtf(dl); el = sqrtf(el);
            if (!(dl <= s->range) || el < 1.0f) continue;          /* the game's switch range, from the own unit */
            s->nvalid++;
            if (s->nc >= ZG_MAXC) continue;
            float cg_ = (e[0] * s->gz[0] + e[1] * s->gz[1] + e[2] * s->gz[2]) / el;
            float cn = (d[0] * s->nose[0] + d[1] * s->nose[1] + d[2] * s->nose[2]) / (dl > 1e-3f ? dl : 1e-3f);
            zg_cand *c = &s->c[s->nc];
            c->g = (u8)g; c->i = (u8)i; c->u = u;
            c->ang = acosf(cg_ < -1.0f ? -1.0f : cg_ > 1.0f ? 1.0f : cg_) * 57.29578f;
            c->nc = cn; c->dist = dl;
            c->tgt = (u8)(ps2_r32(u + 0x1FCu) & 1u);        /* the HUD's red TGT label (0x241398): a mission target */
            c->front = (u8)(cn >= s->nose_cos);              /* inside the game's nose cone (it can lock it) */
            c->vis = -1; c->occ = -1.0f;
            c->score = c->ang + zg_near_deg * dl / ZG_UPM * 0.001f + (c->tgt ? 0.0f : zg_tgt_deg)
                       + (c->front ? 0.0f : zg_front_deg);
            if (g == s->cg && (u32)i == s->ci) { s->cur_ang = c->ang; s->cur_k = s->nc; }
            s->nc++;
        }
    }
    return 1;
}
static void zg_log_cands(const zg_scan_t *s, int best, int old, int byang) {
    for (int k = 0; k < s->nc; k++) {
        const zg_cand *c = &s->c[k];
        float w[3], y, p, y2, p2;
        for (u32 j = 0; j < 3; j++) w[j] = f_of(ps2_r32(c->u + 0x260u + 4u * j)) - s->eye[j];
        float l = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
        for (u32 j = 0; j < 3; j++) w[j] /= l;
        zg_cam_angles(s->R, w, &y, &p);
        zg_cam_angles(s->N, w, &y2, &p2);
        char occ[48] = "";
        if (c->vis == 0) snprintf(occ, sizeof occ, " HIDDEN (hit at %.0f m, t %.3f)", c->occ, c->occt);
        ps2_log("zero-vr: gaze target:   %u/%u unit %08X %.0f m%s%s, camera yaw %.1f pitch %.1f, head yaw %.1f pitch %.1f, "
                "gaze %.1f deg, nose %.1f deg, score %.1f%s%s%s%s%s; at %.0f %.0f %.0f",
                c->g, c->i, c->u, c->dist / ZG_UPM, c->tgt ? " TGT" : "", c->vis == 1 ? " visible" : c->vis < 0 ? "" : occ,
                y, p, y2, p2, c->ang, acosf(c->nc < -1.0f ? -1.0f : c->nc > 1.0f ? 1.0f : c->nc) * 57.29578f, c->score,
                c->front ? "" : " (outside the nose cone)", k == best ? " <- gaze pick" : "",
                k == byang ? " <- by angle only" : "", k == old ? " <- old rule" : "", k == s->cur_k ? " (current)" : "",
                f_of(ps2_r32(c->u + 0x260u)), f_of(ps2_r32(c->u + 0x264u)), f_of(ps2_r32(c->u + 0x268u)));
    }
}
/* ACZ_DEBUG_GAZE_PROBE: the candidates without a press (the player's position and heading, the sight to each) */
static void zg_probe_run(ps2_ctx *ctx, u32 ts) {
    zg_scan_t *s = &zg_s;
    if (zg_dbg == 3) {                                  /* every unit of every group with its position */
        u32 team = ps2_r32(ts + 0x84u), garr = zg_ptr(team) ? ps2_r32(team) : 0u, ng = zg_ptr(team) ? ps2_r8(team + 0x1Cu) : 0u;
        for (u32 g = 0; g < ng && zg_ptr(garr); g++) {
            u32 slot = garr + 4u * g, grp = ps2_r32(slot);
            if (!zg_ptr(grp)) continue;
            int gok = (int)zg_call(ctx, ZG_GROUP_OK, ts, 0, 1, slot);
            u32 recs = ps2_r32(grp + 0x104u);
            int cnt = (int)ps2_r32(grp + 0x108u);
            for (int i = 0; i < cnt && i < 256; i++) {
                u32 u = recs + (u32)i * 0x4A0u;
                if (!zg_ptr(u)) continue;
                ps2_log("zero-vr: gaze probe unit %u/%u %08X group-ok %d valid %u flags %08X %08X%s side %04X at %.0f %.0f %.0f",
                        g, i, u, gok, zg_call(ctx, ZG_VALID, ts, u, 0, 0), ps2_r32(u + 0x1F8u), ps2_r32(u + 0x1FCu),
                        ps2_r32(u + 0x1FCu) & 1u ? " TGT" : "", ps2_r16(u + 0x23Cu), f_of(ps2_r32(u + 0x260u)),
                        f_of(ps2_r32(u + 0x264u)), f_of(ps2_r32(u + 0x268u)));
            }
        }
    }
    if (zg_dbg >= 3 && zg_ptr(zg_world)) {               /* the static objects of the segment test's world, once */
        static u32 dumped;
        u32 O = ps2_r32(zg_world + 4u), n = ps2_r32(O + 0x20u), ins = ps2_r32(O + 0x24u);
        if (O != dumped && zg_ptr(ins) && n < 4096u) {
            dumped = O;
            for (u32 k = 0; k < n; k++)
                ps2_log("zero-vr: gaze probe object %u model %u at %.0f %.0f %.0f", k, ps2_r32(ins + 16u * k + 12u) & 0x7FFFFu,
                        f_of(ps2_r32(ins + 16u * k)), f_of(ps2_r32(ins + 16u * k + 4u)), f_of(ps2_r32(ins + 16u * k + 8u)));
        }
    }
    if (zg_dbg == 4) {                                   /* every copy of the aircraft's position in the player */
        u32 pl = ps2_r32(ts + 0x88u);
        float x = f_of(ps2_r32(pl + 0xEF0u + 0x3B0u)), z = f_of(ps2_r32(pl + 0xEF0u + 0x3B8u));
        char buf[512]; int n = 0;
        for (u32 o = 0; o < 0x2000u && n < 480; o += 4u) {
            float a = f_of(ps2_r32(pl + o)), c = f_of(ps2_r32(pl + o + 8u));
            if (fabsf(a - x) < 2.0f && fabsf(c - z) < 2.0f) n += snprintf(buf + n, sizeof buf - n, " +%X", o);
        }
        ps2_log("zero-vr: gaze test: player %08X position %.1f %.1f copies at%s", pl, x, z, n ? buf : " none");
        float pa = f_of(ps2_r32(pl + 0xEF0u + 0x3C0u)), ya = f_of(ps2_r32(pl + 0xEF0u + 0x3C4u));
        n = 0;
        for (u32 o = 0; o < 0x2000u && n < 480; o += 4u) {
            float a = f_of(ps2_r32(pl + o)), c = f_of(ps2_r32(pl + o + 4u));
            if (fabsf(a - pa) < 1e-4f && fabsf(c - ya) < 1e-4f) n += snprintf(buf + n, sizeof buf - n, " +%X", o);
        }
        ps2_log("zero-vr: gaze test: attitude %.4f %.4f copies at%s", pa, ya, n ? buf : " none");
    }
    if (zg_dbg >= 5 && zg_ptr(zg_world)) {              /* free distance around the aircraft (test: the trench's shape) */
        u32 pl = ps2_r32(ts + 0x88u);
        float o[3], hd = -f_of(ps2_r32(pl + 0x12B4u));
        for (u32 j = 0; j < 3; j++) o[j] = f_of(ps2_r32(pl + 0x12A0u + 4u * j));
        char buf[600]; int n = 0;
        for (int a = -90; a <= 90 && n < 560; a += 10) {
            float h = hd + (float)a * 0.01745329f, e[3] = { o[0] + sinf(h) * 15360.0f, o[1], o[2] - cosf(h) * 15360.0f }, hit[3], t;
            int r = zg_seg(ctx, o, e, hit, &t);
            float d = r == 1 ? sqrtf((hit[0] - o[0]) * (hit[0] - o[0]) + (hit[2] - o[2]) * (hit[2] - o[2])) / ZG_UPM : -1.0f;
            n += snprintf(buf + n, sizeof buf - n, " %d:%.0f", a, d);
        }
        float dn[3] = { o[0], o[1] - 15360.0f, o[2] }, up[3] = { o[0], o[1] + 15360.0f, o[2] }, hit[3], t;
        int r1 = zg_seg(ctx, o, dn, hit, &t);
        float fl = r1 == 1 ? hit[1] : -1.0f;
        int r2 = zg_seg(ctx, o, up, hit, &t);
        float ce = r2 == 1 ? hit[1] : -1.0f;
        ps2_log("zero-vr: gaze test: free metres by heading offset (-1 = 3 km clear):%s; floor y %.0f, ceiling y %.0f", buf, fl, ce);
    }
    if (!zg_scan(ctx, ts, s)) return;
    int nh = 0, nt = 0;
    u64 ns0 = zg_los_ns, n0 = zg_los_n;
    for (int k = 0; k < s->nc; k++) { zg_sight(ctx, s, k); nh += s->c[k].vis == 0; nt += s->c[k].tgt; }
    double us = (double)(zg_los_ns - ns0) / 1000.0;
    u32 fm = ps2_r32(ts + 0x88u) + 0xEF0u;
    ps2_log("zero-vr: gaze probe field %llu: %u groups (%u scanned), %d valid in range %.0f m, %d hidden (%llu lines of "
            "sight, %.1f us), %d TGT; current %u/%u; aircraft at %.0f %.0f %.0f heading %.1f pitch %.1f, %.0f kt; eye at "
            "%.0f %.0f %.0f",
            (unsigned long long)ps2_kernel_vblank_count(), s->ng, s->ngs, s->nvalid, s->range / ZG_UPM, nh,
            (unsigned long long)(zg_los_n - n0), us, nt, s->cg, s->ci,
            f_of(ps2_r32(fm + 0x3B0u)), f_of(ps2_r32(fm + 0x3B4u)), f_of(ps2_r32(fm + 0x3B8u)),
            -f_of(ps2_r32(fm + 0x3C4u)) * 57.29578f, f_of(ps2_r32(fm + 0x3C0u)) * 57.29578f,
            f_of(ps2_r32(fm + 0x3F8u)) / 1.3f * 0.54f, s->eye[0], s->eye[1], s->eye[2]);
    zg_log_cands(s, -1, -1, -1);
}
static int zg_pick(ps2_ctx *ctx, void *user) {
    uintptr_t which = (uintptr_t)user;                  /* 0: 0x251E48 (fresh pick), 1: 0x252410 (quick re-press) */
    u32 ts = ctx->r[4].uw[0];
    if (!zg_press || ts != zg_ts) return 0;
    zg_press = 0;                                       /* one decision per press */
    zg_presses++;
    zg_scan_t *s = &zg_s;
    if (!zg_scan(ctx, ts, s)) {
        if (zg_dbg) ps2_log("zero-vr: gaze target: press at field %llu without a head-turned camera: the game's choice",
                            (unsigned long long)ps2_kernel_vblank_count());
        return 0;
    }
    if (zg_dbg >= 2) {                                  /* every unit: the game's tests and the distance */
        int shown = 0;
        u32 own = ps2_r32(ps2_r32(s->garr + 4u * ps2_r8(ts + 0x96u)) + 0x104u) + (u32)ps2_r8(ts + 0x97u) * 0x4A0u;
        ps2_log("zero-vr: gaze target: own %u/%u unit %08X, %u groups, flags %02X", ps2_r8(ts + 0x96u), ps2_r8(ts + 0x97u),
                own, s->ng, ps2_r8(ts + 0x9Au));
        for (u32 g = 0; g < s->ng; g++) {
            u32 slot = s->garr + 4u * g, grp = ps2_r32(slot);
            if (!zg_ptr(grp)) continue;
            int gok = (int)zg_call(ctx, ZG_GROUP_OK, ts, 0, 1, slot);
            u32 recs = ps2_r32(grp + 0x104u);
            int cnt = (int)ps2_r32(grp + 0x108u);
            for (int i = 0; i < cnt && i < 256 && shown < 400; i++, shown++) {
                u32 u = recs + (u32)i * 0x4A0u, t = ps2_r32(u + 0x1C4u), tt = zg_ptr(t) ? ps2_r32(t) : 0u;
                float dl = 0.0f;
                for (u32 k = 0; k < 3; k++) { float q = f_of(ps2_r32(u + 0x260u + 4u * k)) - s->op[k]; dl += q * q; }
                ps2_log("zero-vr: gaze target:   unit %u/%u %08X group-ok %d basic %u valid %u flags %08X %08X kind %d/%u "
                        "side %04X %.0f m", g, i, u, gok, zg_call(ctx, 0x00251908u, ts, u, 0, 0),
                        zg_call(ctx, ZG_VALID, ts, u, 0, 0), ps2_r32(u + 0x1F8u), ps2_r32(u + 0x1FCu),
                        zg_ptr(tt) ? (int)ps2_r8(tt + 0x71u) : -1, ps2_r8(u + 0x39Bu), ps2_r16(u + 0x23Cu), sqrtf(dl) / ZG_UPM);
            }
        }
    }
    int nc = s->nc, cur_k = s->cur_k;
    float cur_ang = s->cur_ang;
    zg_cand *cand = s->c;
    u8 cg = s->cg, ci = s->ci;
    /* the old rule (ACZ_VR_GAZE_CYCLE=0; logged for comparison): in the cone, not the current target; the smallest
       gaze angle above the current target's when that one is in the cone (cycling outward), else (or wrapping) the
       smallest */
    int old = -1, wrap = -1;
    int cur_in = cur_ang >= 0.0f && cur_ang <= zg_cone_deg;
    for (int k = 0; k < nc; k++) {
        if (cand[k].ang > zg_cone_deg || k == cur_k) continue;
        if (wrap < 0 || cand[k].ang < cand[wrap].ang) wrap = k;
        if (cur_in && cand[k].ang < cur_ang) continue;
        if (old < 0 || cand[k].ang < cand[old].ang) old = k;
    }
    if (old < 0) old = wrap;
    /* the sight to everything in the cone; hidden ones drop out while anything in the cone is visible */
    int n_cone = 0, n_vis = 0, n_hid = 0, n_tgt = 0, byang = -1;
    u64 ns0 = zg_los_ns;
    for (int k = 0; k < nc; k++) {
        if (cand[k].ang > zg_cone_deg) continue;
        n_cone++;
        n_tgt += cand[k].tgt;
        if (byang < 0 || cand[k].ang < cand[byang].ang) byang = k;
    }
    if (zg_vis_on) {                                    /* the best ZG_LOS_MAX by score get a line of sight (~50 us each) */
        int done[ZG_LOS_MAX], nd = 0;
        while (nd < ZG_LOS_MAX) {
            int b = -1;
            for (int k = 0; k < nc; k++) {
                if (cand[k].ang > zg_cone_deg || cand[k].vis >= 0) continue;
                int seen = 0;
                for (int j = 0; j < nd; j++) seen |= done[j] == k;
                if (!seen && (b < 0 || cand[k].score < cand[b].score)) b = k;
            }
            if (b < 0) break;
            zg_sight(ctx, s, b);
            done[nd++] = b;
        }
    }
    for (int k = 0; k < nc; k++)
        if (cand[k].ang <= zg_cone_deg) { if (cand[k].vis == 0) n_hid++; else n_vis++; }
    double los_us = (double)(zg_los_ns - ns0) / 1000.0;
    int drop_hidden = zg_vis_on && n_vis > 0 && n_hid > 0;
    if (drop_hidden) zg_hid_dropped++;
#define ZG_ELIG(k) (cand[k].ang <= zg_cone_deg && !(drop_hidden && cand[k].vis == 0))
    /* the new rule: the best score (the gaze angle + distance + not a mission target + outside the nose cone) among
       the eligible; on the current target the next one up in score within the tight cone */
    u64 fnow = ps2_kernel_vblank_count();
    int near = -1, n_cycle = 0;
    for (int k = 0; k < nc; k++) {
        if (!ZG_ELIG(k)) continue;
        if (cand[k].ang <= zg_cycle_deg) n_cycle++;
        if (near < 0 || cand[k].score < cand[near].score) near = k;
    }
    float moved = zg_last_field ? zg_angle(s->gz, zg_last_gz) : 180.0f;
    /* a quick re-press walks on only while the current target is still in the tight cone (it may have slid out of it
       since: then a fresh pick, not "stays") */
    int walk = cur_k >= 0 && cg == zg_last_g && ci == zg_last_i && fnow - zg_last_field <= ZG_WALK_FIELDS
               && moved < ZG_WALK_DEG && (zg_cycle_deg <= 0.0f || cur_ang <= zg_cycle_deg);
    int best = -1, keep = 0;
    const char *how = "best";
    if (zg_cycle_deg <= 0.0f) {
        best = old;
        how = cur_in ? "outward (old rule)" : "nearest (old rule)";
    } else if (near >= 0 && (near == cur_k || walk)) {
        float cs = cand[cur_k].score;
        int wr = -1;
        for (int k = 0; k < nc; k++) {                  /* the next one up from the current, in the tight cone */
            if (k == cur_k || !ZG_ELIG(k) || cand[k].ang > zg_cycle_deg) continue;
            if (wr < 0 || cand[k].score < cand[wr].score) wr = k;
            if (cand[k].score > cs && (best < 0 || cand[k].score < cand[best].score)) best = k;
        }
        how = walk ? "next in the group" : "next to the current";
        if (best < 0 && wr >= 0) { best = wr; how = "wrap to the best"; }
        if (best < 0) keep = 1;
    } else {
        best = near;
    }
#undef ZG_ELIG
    if (best >= 0 && byang >= 0 && best != byang) zg_angle_differ++;
    int nose_k = -1;                                    /* the game's pass 1 (0x251E48) for the log */
    for (int k = 0; k < nc; k++)
        if (k != cur_k && cand[k].nc >= s->nose_cos && (nose_k < 0 || cand[k].nc > cand[nose_k].nc)) nose_k = k;
    float hy, hp;                                       /* the head's direction from the nose (the flight camera) */
    zg_cam_angles(s->R, s->ax, &hy, &hp);
    if (zg_dbg) {
        float gy, gp; zg_cam_angles(s->R, s->gz, &gy, &gp);
        ps2_log("zero-vr: gaze target: press field %llu (%s) head yaw %.1f pitch %.1f, gaze yaw %.1f pitch %.1f; current %u/%u "
                "at %.1f deg; %u groups, %d valid in range %.0f m, nose cone %.1f deg, switch timer %u of %u; the game's "
                "nose pick %d/%d; last pick %u/%u %llu fields ago, gaze moved %.1f deg",
                (unsigned long long)fnow, which ? "0x252410 quick re-press" : "0x251E48",
                hy, hp, gy, gp, cg, ci, cur_ang, s->ng, s->nvalid, s->range / ZG_UPM, acosf(s->nose_cos) * 57.29578f,
                ps2_r16(ts + 0x94u), ps2_r8(ts + 0x9Bu) * 2u,
                nose_k >= 0 ? cand[nose_k].g : -1, nose_k >= 0 ? cand[nose_k].i : -1,
                zg_last_g, zg_last_i, (unsigned long long)(zg_last_field ? fnow - zg_last_field : 0), moved);
        zg_log_cands(s, best, old, byang);
    }
    if (!zg_on) {
        ps2_log("zero-vr: gaze target: off: the game's choice (the gaze would pick %d/%d)",
                best >= 0 ? cand[best].g : -1, best >= 0 ? cand[best].i : -1);
        return 0;
    }
    if (best != old) zg_differ++;
    char cone[128];
    snprintf(cone, sizeof cone, "%d in the %.0f deg cone (%d visible, %d hidden%s, %d TGT; sight %.0f us)", n_cone,
             zg_cone_deg, n_vis, n_hid, drop_hidden ? " left out" : "", n_tgt, los_us);
    if (keep) {                                         /* the current target stays: nothing written, lock-on goes on */
        zg_kept++;
        zg_last_g = cg; zg_last_i = ci; zg_last_field = fnow; memcpy(zg_last_gz, s->gz, sizeof zg_last_gz);
        ps2_log("zero-vr: gaze target: field %llu: %u/%u stays, %.1f deg from the gaze: nothing else within %.0f deg "
                "(%s); head yaw %.1f pitch %.1f; the old rule: %d/%d",
                (unsigned long long)fnow, cg, ci, cur_ang, zg_cycle_deg, cone, hy, hp,
                old >= 0 ? cand[old].g : -1, old >= 0 ? cand[old].i : -1);
        return 1;
    }
    if (best < 0) {
        zg_none++;
        zg_last_g = zg_last_i = 0xFFu;
        ps2_log("zero-vr: gaze target: field %llu: nothing within %.0f deg of the gaze (%d valid in range, %u groups; head "
                "yaw %.1f pitch %.1f): the game's choice", (unsigned long long)fnow, zg_cone_deg, s->nvalid, s->ng, hy, hp);
        return 0;
    }
    /* the game's own epilogue (0x251E48 / 0x252410): switch timer, target, lock-on slots' target, their reset */
    u8 g = cand[best].g, i = cand[best].i;
    ps2_w16(ts + 0x94u, (u16)(ps2_r8(ts + 0x9Bu) * 2u));
    ps2_w8(ts + 0x98u, g); ps2_w8(ts + 0x99u, i);
    if (which == 0) ps2_w8(ts + 0x9Au, ps2_r8(ts + 0x9Au) & ~0x20u);
    ps2_w8(ts + 0x7Fu, g); ps2_w8(ts + 0x80u, i);
    zg_call(ctx, ZG_SET_DONE, ts, 0, 0, 0);
    zg_taken++;
    zg_last_g = g; zg_last_i = i; zg_last_field = fnow; memcpy(zg_last_gz, s->gz, sizeof zg_last_gz);
    ps2_log("zero-vr: gaze target: field %llu: %u/%u -> %u/%u (%s), %.1f deg from the gaze, %.0f m%s%s, score %.1f; head "
            "yaw %.1f pitch %.1f; current at %.1f deg; %s, %d within %.0f; by angle only %d/%d; the old rule: %d/%d (the "
            "nose pick: %d/%d)",
            (unsigned long long)fnow, cg, ci, g, i, how, cand[best].ang, cand[best].dist / ZG_UPM,
            cand[best].tgt ? " TGT" : "", cand[best].vis == 0 ? " hidden" : cand[best].vis > 0 ? " visible" : "",
            cand[best].score, hy, hp, cur_ang, cone, n_cycle, zg_cycle_deg,
            byang >= 0 ? cand[byang].g : -1, byang >= 0 ? cand[byang].i : -1, old >= 0 ? cand[old].g : -1,
            old >= 0 ? cand[old].i : -1, nose_k >= 0 ? cand[nose_k].g : -1, nose_k >= 0 ? cand[nose_k].i : -1);
    return 1;
}
static int zg_update_enter(ps2_ctx *ctx, void *user) {
    (void)user;
    u32 ts = ctx->r[4].uw[0];
    zg_press = 0;
    if (!zg_ptr(ts)) return 0;
    u32 player = ps2_r32(ts + 0x88u);
    if (!zg_ptr(player) || player + 0x1880u != ts) return 0;
    u32 t = ps2_r32(player + 0x1A5Cu);
    if (!zg_ptr(t) || ps2_r8(t + 0x1Fu) != 1u) return 0;              /* the pad's player only */
    zg_ts = ts;
    zg_press = (ps2_r8(ts + 0x9Au) & 1u) != 0u;
    zg_cur_g = ps2_r8(ts + 0x98u); zg_cur_i = ps2_r8(ts + 0x99u);
    for (int k = 0; k < zg_tpn; k++) {                   /* ACZ_DEBUG_GAZE_TP (test) */
        u64 f = ps2_kernel_vblank_count();
        if (f < zg_tp[k].f || f > zg_tp[k].f + 1u) continue;
        u32 fm = player + 0xEF0u;
        float v[6] = { zg_tp[k].p[0], zg_tp[k].p[1], zg_tp[k].p[2], zg_tp[k].p[4] * 0.01745329f,
                       -zg_tp[k].p[3] * 0.01745329f, 0.0f };
        static const u32 at[3] = { 0x70u, 0x1070u, 0x12A0u };     /* the object, the flight state, the flight model */
        (void)fm;
        for (u32 c = 0; c < 3; c++)
            for (u32 j = 0; j < 6; j++) { u32 w; memcpy(&w, &v[j], 4); ps2_w32(player + at[c] + (j < 3 ? 0u : 4u) + 4u * j, w); }
        ps2_log("zero-vr: gaze test: field %llu: the aircraft put at %.0f %.0f %.0f heading %.1f pitch %.1f "
                "(ACZ_DEBUG_GAZE_TP)", (unsigned long long)f, v[0], v[1], v[2], zg_tp[k].p[3], zg_tp[k].p[4]);
    }
    if (zg_probe > 0) {
        static u64 last;
        u64 f = ps2_kernel_vblank_count();
        if (f - last >= (u64)zg_probe) { last = f; zg_probe_run(ctx, ts); }
    }
    return 0;
}
static int zg_update_leave(ps2_ctx *ctx, void *user) {
    (void)ctx; (void)user;
    if (zg_dbg && zg_ts && (ps2_r8(zg_ts + 0x98u) != zg_cur_g || ps2_r8(zg_ts + 0x99u) != zg_cur_i))
        ps2_log("zero-vr: gaze target: field %llu target %u/%u -> %u/%u%s", (unsigned long long)ps2_kernel_vblank_count(),
                zg_cur_g, zg_cur_i, ps2_r8(zg_ts + 0x98u), ps2_r8(zg_ts + 0x99u), zg_press ? " (the game's choice)" : "");
    zg_press = 0;
    return 0;
}
static float zg_cycle_set = -1.0f;              /* ACZ_VR_GAZE_CYCLE as set, before the cone's limit */
static int zg_hooks_in;
static void zg_menv_reload(void) {              /* the NEXT MISSION tier: the game thread, at the edge */
    if (!zg_hooks_in) return;
    const char *e = ps2_menv("ACZ_VR_GAZE_TARGET");
    float deg = e && *e ? (float)atof(e) : 1.0f;
    const float a = zg_on ? zg_cone_deg : 0.0f;
    zg_on = deg > 0.0f;
    if (deg < 3.0f) deg = 20.0f;
    if (deg > 60.0f) deg = 60.0f;
    zg_cone_deg = deg;
    if (zg_cycle_set >= 0.0f) zg_cycle_deg = zg_cycle_set > deg ? deg : zg_cycle_set;
    ps2_menv_note("ACZ_VR_GAZE_TARGET", a, zg_on ? deg : 0.0f);
}
static void zg_install(void) {
    const char *e = ps2_menv_boot("ACZ_VR_GAZE_TARGET"), *d = getenv("ACZ_DEBUG_GAZE");
    const char *cy = getenv("ACZ_VR_GAZE_CYCLE"), *pp = getenv("ACZ_VR_GAZE_PITCH");
    zg_dbg = d ? atoi(d) : 0;
    zg_vis_on = env_float("ACZ_VR_GAZE_VISIBLE", 1.0f) != 0.0f;
    zg_near_deg = env_float("ACZ_VR_GAZE_NEAR", 1.0f);
    zg_tgt_deg = env_float("ACZ_VR_GAZE_TGT", 3.0f);
    zg_front_deg = env_float("ACZ_VR_GAZE_FRONT", 2.0f);
    if (!(zg_near_deg >= 0.0f && zg_near_deg <= 10.0f)) zg_near_deg = 1.0f;
    if (!(zg_tgt_deg >= 0.0f && zg_tgt_deg <= 30.0f)) zg_tgt_deg = 3.0f;
    if (!(zg_front_deg >= 0.0f && zg_front_deg <= 30.0f)) zg_front_deg = 2.0f;
    zg_groups_max = (int)env_float("ACZ_DEBUG_GAZE_GROUPS", 255.0f);
    if (zg_groups_max < 1 || zg_groups_max > 255) zg_groups_max = 255;
    zg_probe = (int)env_float("ACZ_DEBUG_GAZE_PROBE", 0.0f);
    for (const char *t = getenv("ACZ_DEBUG_GAZE_TP"); t && *t && zg_tpn < 12; ) {
        unsigned long long f; float q[5] = { 0, 0, 0, 0, 0 };
        int n = sscanf(t, "%llu:%f,%f,%f,%f,%f", &f, &q[0], &q[1], &q[2], &q[3], &q[4]);
        if (n < 5) break;
        zg_tp[zg_tpn].f = f; memcpy(zg_tp[zg_tpn].p, q, sizeof q); zg_tpn++;
        t = strchr(t, ' ');
        while (t && *t == ' ') t++;
    }
    float deg = e && *e ? (float)atof(e) : 1.0f;
    zg_on = deg > 0.0f;
    if (!zg_on) ps2_log("zero-vr: gaze target off (ACZ_VR_GAZE_TARGET=0): the target button keeps the game's choice");
    if (!zg_on && !zg_dbg) return;
    if (deg < 3.0f) deg = 20.0f;
    if (deg > 60.0f) deg = 60.0f;
    const char *xe = getenv("PS2_XR"), *xm = getenv("PS2_XR_MODE");
    if (!(xe && *xe == '1' && xm && (!strcmp(xm, "mono") || !strncmp(xm, "stereo", 6)))) return;   /* no head tracking */
    static const struct { u32 a, w; } chk[] = {
        { ZG_TS_UPDATE, 0x27BDFF40u }, { ZG_PICK_NOSE, 0x27BDFF00u }, { ZG_PICK_NEXT, 0x27BDFF10u },
        { ZG_VALID, 0x27BDFFE0u }, { ZG_GROUP_OK, 0x27BDFFE0u }, { ZG_SET_DONE, 0x27BDFFE0u },
        { 0x00251CECu, 0x0C094792u }, { 0x00251D2Cu, 0x0C094904u },          /* the update's two pick calls */
        { 0x00251868u, 0x908F009Au }, { 0x00233944u, 0x0C094692u },          /* the request flag, the update call */
        { ZG_SEGMENT, 0x27BDFF20u }, { 0x00241398u, 0x30A500FFu },          /* the segment test, the TGT label test */
        { 0x00231808u, 0x27BDFD70u },                                       /* the player update */
    };
    for (unsigned k = 0; k < sizeof chk / sizeof chk[0]; k++)
        if (ps2_r32(chk[k].a) != chk[k].w) { ps2_log("zero-vr: gaze target: code mismatch at %08X; off", chk[k].a); return; }
    if (ps2_hook_before(ZG_TS_UPDATE, zg_update_enter, NULL, 60, "zero-gaze") < 0
        || ps2_hook_after(ZG_TS_UPDATE, zg_update_leave, NULL, 60, "zero-gaze") < 0
        || ps2_hook_replace(ZG_PICK_NOSE, zg_pick, (void *)(uintptr_t)0, 60, "zero-gaze") < 0
        || ps2_hook_replace(ZG_PICK_NEXT, zg_pick, (void *)(uintptr_t)1, 60, "zero-gaze") < 0) {
        ps2_log("zero-vr: gaze target: hooks refused; off"); return;
    }
    if ((zg_vis_on || zg_probe > 0) && (ps2_hook_before(ZG_SEGMENT, zg_seg_enter, NULL, 60, "zero-gaze") < 0
        || ps2_hook_before(0x00231808u, zg_pl_enter, NULL, 59, "zero-gaze") < 0
        || ps2_hook_after(0x00231808u, zg_pl_leave, NULL, 59, "zero-gaze") < 0)) {
        zg_vis_on = 0; ps2_log("zero-vr: gaze target: the segment test hook refused: no line of sight");
    } else if (zg_vis_on || zg_probe > 0) zg_world_hooked = 1;
    zg_cone_deg = deg;
    if (cy && *cy) zg_cycle_deg = (float)atof(cy);
    if (zg_cycle_deg < 0.0f) zg_cycle_deg = 0.0f;
    zg_cycle_set = zg_cycle_deg;
    zg_hooks_in = 1;
    if (zg_cycle_deg > zg_cone_deg) zg_cycle_deg = zg_cone_deg;
    if (pp && *pp) zg_pitch_deg = (float)atof(pp);
    if (zg_pitch_deg < -20.0f) zg_pitch_deg = -20.0f;
    if (zg_pitch_deg > 20.0f) zg_pitch_deg = 20.0f;
    if (zg_on) ps2_log("zero-vr: gaze target: in head-tracked frames the target button picks the target nearest to the gaze "
            "(the head's axis %.1f deg down) within %.0f deg; on the current target the next one within %.0f deg "
            "(ACZ_VR_GAZE_TARGET=0: the game's own choice; ACZ_VR_GAZE_CYCLE=0: the old outward rule)%s",
            zg_pitch_deg, deg, zg_cycle_deg, zg_dbg ? ", debug log" : "");
    if (zg_on) ps2_log("zero-vr: gaze target: all groups scanned (up to %d); %s; score = gaze angle + %.1f deg per km + %.1f "
            "deg if not a mission target (TGT) + %.1f deg outside the nose cone (ACZ_VR_GAZE_VISIBLE / _NEAR / _TGT / "
            "_FRONT)%s", zg_groups_max, zg_vis_on ? "targets hidden behind terrain or objects left out while one in the "
            "cone is visible (the game's segment test)" : "no line of sight (ACZ_VR_GAZE_VISIBLE=0)", zg_near_deg,
            zg_tgt_deg, zg_front_deg, zg_probe > 0 ? "; candidate probe (ACZ_DEBUG_GAZE_PROBE)" : "");
}
static void zg_report(void) {
    if (zg_presses) ps2_log("zero-vr: gaze target: %llu presses, %llu took a target, %llu kept the current one, %llu had "
                            "none in the cone; %llu differ from the old rule",
                            (unsigned long long)zg_presses, (unsigned long long)zg_taken, (unsigned long long)zg_kept,
                            (unsigned long long)zg_none, (unsigned long long)zg_differ);
    if (zg_presses || zg_los_n)
        ps2_log("zero-vr: gaze target: %llu lines of sight (mean %.1f us, max %.1f us), %llu hidden; %llu presses left "
                "hidden targets out; %llu picks differ from the pick by gaze angle alone", (unsigned long long)zg_los_n,
                zg_los_n ? (double)zg_los_ns / (double)zg_los_n / 1000.0 : 0.0, (double)zg_los_max_ns / 1000.0,
                (unsigned long long)zg_los_hidden, (unsigned long long)zg_hid_dropped, (unsigned long long)zg_angle_differ);
}

/* ---- Enclosure: the sun and the sky from the aircraft (tunnel_light 2026-11-02, bug #267; ps2_video.c enclosure) ----
   M17's tunnels under the Avalon Dam (playtest 2026-11-02, F-14D, "the glare on the camera is shown as if I were not
   underground"): the canopy's scratches and dust glinted in the sun behind the roof, the 3D cockpit kept its sunlight,
   shadows and the sun's bounce, the eye adaptation never reacted -- they all took C1's sun as under an open sky. The game
   itself keeps no such state (its model lights, ambient and fog are the same in the tunnel; only its own sun flare is
   hidden, by its depth probe), so the world is asked directly with the game's own segment test (zg_seg: the terrain's
   triangles + the static objects' collision models -- a tunnel's roof and walls, which any tunnel one can fly through
   must have), after each update of the pad player's aircraft (0x231808), from its position (FM +0x3B0, 1 m up):
   - a ray towards C1's sun every ACZ_VR_TUNNEL_SUN_EVERY (2) fields, ACZ_VR_TUNNEL_SUN_M (3000 m) long: the sun's
     visibility 1 / 0 (below the horizon 0);
   - the sky dome: 13 rays of ACZ_VR_TUNNEL_SKY_M (800 m) -- the zenith and rings of 6 at 60 and 35 deg elevation
     (staggered by 30 deg), weighted as an upward face sees the sky (the bands 0-20, 20-45 and 45-70 deg from the zenith:
     0.117, 0.383, 0.383, renormalised) -- round robin in the rest of the field's ACZ_VR_TUNNEL_RAYS (2) rays (a sweep in
     9 fields, 0.15 s; after a jump of the aircraft by 300 m or more -- a respawn, a teleport -- all 13 at once): the
     sky's openness 0..1, the clear share by weight (M17: the cavern, the tunnels and the device chambers 0..0.15, a
     skylight overhead ~0.13, the open river approach 1).
   Both go to the renderer (ps2_vk_enclosure_set), which eases them and drives its sun- and sky-lit effects. Nothing is
   per mission: an overhang, a bridge, a ridge in front of a low sun, a tunnel or a cave anywhere. Cost on the EE thread
   (sim, M17, exclusive 4x runs): ~20-60 us a ray, 41-47 us a field on average in the cavern and over the open river
   (max 145 us; a 13-ray sweep after a jump 110-520 us once). The rays' cost is kept apart from the gaze's line-of-sight
   statistics; ACZ_DEBUG_TUNNEL=N logs them every N fields (`zero-vr: tunnel rays`). */
#define ZE_NSKY 13
static int ze_on, ze_dbg, ze_rays = 2, ze_sun_every = 2;
static float ze_sun_len = 3000.0f, ze_sky_len = 800.0f;    /* metres (the HUD's, ZG_UPM) */
static float ze_dir[ZE_NSKY][3], ze_w[ZE_NSKY];
static s8 ze_res[ZE_NSKY];                  /* 1 clear, 0 hit, -1 not known */
static float ze_hit[ZE_NSKY];               /* the hit's distance (m); -1 clear */
static int ze_next;
static u32 ze_player;                       /* the pad player in this update (ze_pl_enter) */
static float ze_last[3], ze_sun_dir[3] = { 0.0f, 1.0f, 0.0f };
static int ze_have_last, ze_have_sun;
static float ze_sun_vis = 1.0f, ze_sun_hit = -1.0f, ze_open = 1.0f;
static u64 ze_fields, ze_n, ze_ns, ze_max_ns, ze_jumps, ze_sun_hidden, ze_closed, ze_sun_ns, ze_sun_n;
static void ze_dirs_init(void) {
    ze_dir[0][0] = 0.0f; ze_dir[0][1] = 1.0f; ze_dir[0][2] = 0.0f; ze_w[0] = 0.117f;
    for (int r = 0; r < 2; r++)
        for (int k = 0; k < 6; k++) {
            float el = (r ? 35.0f : 60.0f) * 0.01745329f, az = ((float)k * 60.0f + (r ? 30.0f : 0.0f)) * 0.01745329f;
            float *d = ze_dir[1 + 6 * r + k];
            d[0] = cosf(el) * sinf(az); d[1] = sinf(el); d[2] = cosf(el) * cosf(az);
            ze_w[1 + 6 * r + k] = 0.383f / 6.0f;
        }
    for (int i = 0; i < ZE_NSKY; i++) { ze_res[i] = -1; ze_hit[i] = -1.0f; }
}
static int ze_pl_enter(ps2_ctx *ctx, void *user) {
    (void)user;
    u32 player = ctx->r[4].uw[0], t = zg_ptr(player) ? ps2_r32(player + 0x1A5Cu) : 0u;
    ze_player = zg_ptr(t) && ps2_r8(t + 0x1Fu) == 1u ? player : 0u;
    return 0;
}
/* one ray of len_m metres from o (units) along the unit d: 1 hit (*hit_m its distance), 0 clear, -1 no world yet */
static int ze_cast(ps2_ctx *ctx, const float o[3], const float d[3], float len_m, float *hit_m) {
    float to[3], hit[3], t = 0.0f;
    for (int j = 0; j < 3; j++) to[j] = o[j] + d[j] * len_m * ZG_UPM;
    int r = zg_seg(ctx, o, to, hit, &t);
    *hit_m = -1.0f;
    if (r == 1) {
        float dd = 0.0f;
        for (int j = 0; j < 3; j++) dd += (hit[j] - o[j]) * (hit[j] - o[j]);
        dd = sqrtf(dd) / ZG_UPM;
        *hit_m = dd >= 0.0f && dd <= len_m * 1.01f ? dd : t * len_m;
    }
    ze_n++;
    return r;
}
static int ze_pl_leave(ps2_ctx *ctx, void *user) {
    (void)user;
    u32 pl = ze_player;
    ze_player = 0;
    if (!pl || !ze_on || !zg_ptr(zg_world)) return 0;
    struct timespec c0, c1;
    clock_gettime(CLOCK_MONOTONIC, &c0);
    u64 keep_n = zg_los_n, keep_ns = zg_los_ns, keep_max = zg_los_max_ns;   /* (the gaze's statistics stay its own) */
    u32 fm = pl + 0xEF0u;
    float o[3];
    for (u32 j = 0; j < 3; j++) o[j] = f_of(ps2_r32(fm + 0x3B0u + 4u * j));
    if (!(isfinite(o[0]) && isfinite(o[1]) && isfinite(o[2]))) return 0;
    o[1] += 1.0f * ZG_UPM;                              /* (the pilot's eye above the aircraft's origin) */
    u64 f = ps2_kernel_vblank_count();
    if (!ze_have_sun || (f & 7u) == 0u) {               /* C1's sun, re-read every 8 fields */
        static ps2_cloud_env env;
        if (ps2_cloud_env_read(&env) && (env.valid & CL_ENV_SUN)) {
            float l = sqrtf(env.sun_dir[0] * env.sun_dir[0] + env.sun_dir[1] * env.sun_dir[1] + env.sun_dir[2] * env.sun_dir[2]);
            if (l > 0.5f) { for (int j = 0; j < 3; j++) ze_sun_dir[j] = env.sun_dir[j] / l; ze_have_sun = 1; }
        }
    }
    int all = !ze_have_last;                            /* a jump (respawn, teleport): the dome again at once */
    if (ze_have_last) {
        float d2 = 0.0f;
        for (int j = 0; j < 3; j++) d2 += (o[j] - ze_last[j]) * (o[j] - ze_last[j]);
        if (d2 > (300.0f * ZG_UPM) * (300.0f * ZG_UPM)) { all = 1; ze_jumps++; }
    }
    if (all) for (int i = 0; i < ZE_NSKY; i++) ze_res[i] = -1;
    memcpy(ze_last, o, sizeof ze_last); ze_have_last = 1;
    int ok = 1, n = all ? ZE_NSKY : ze_rays;
    double sun_us = -1.0;
    if (!ze_have_sun) ze_sun_vis = 1.0f;
    else if (ze_sun_dir[1] < 0.0f) { ze_sun_vis = 0.0f; ze_sun_hit = 0.0f; }   /* (below the horizon) */
    else if (all || ze_sun_every <= 1 || (f % (u64)ze_sun_every) == 0u) {     /* (one of the field's rays) */
        struct timespec s0, s1;
        clock_gettime(CLOCK_MONOTONIC, &s0);
        int r = ze_cast(ctx, o, ze_sun_dir, ze_sun_len, &ze_sun_hit);
        clock_gettime(CLOCK_MONOTONIC, &s1);
        sun_us = (double)((s1.tv_sec - s0.tv_sec) * 1000000000ll + (s1.tv_nsec - s0.tv_nsec)) / 1000.0;
        ze_sun_ns += (u64)(sun_us * 1000.0); ze_sun_n++;
        if (r < 0) ok = 0; else ze_sun_vis = r == 1 ? 0.0f : 1.0f;
        if (!all && n > 1) n--;
    }
    for (int i = 0; i < n && ok; i++) {
        int k = all ? i : ze_next;
        if (!all) ze_next = (ze_next + 1) % ZE_NSKY;
        int r = ze_cast(ctx, o, ze_dir[k], ze_sky_len, &ze_hit[k]);
        ze_res[k] = (s8)(r < 0 ? -1 : r == 1 ? 0 : 1);
    }
    zg_los_n = keep_n; zg_los_ns = keep_ns; zg_los_max_ns = keep_max;
    if (!ok) return 0;
    float ws = 0.0f, wo = 0.0f;
    for (int i = 0; i < ZE_NSKY; i++) if (ze_res[i] >= 0) { ws += ze_w[i]; wo += ze_w[i] * (float)ze_res[i]; }
    ze_open = ws > 0.0f ? wo / ws : 1.0f;
    {   extern void ps2_vk_enclosure_set(float sun_vis, float sky_open, const float eye_units[3], float units_per_m);
        ps2_vk_enclosure_set(ze_sun_vis, ze_open, o, ZG_UPM); }
    clock_gettime(CLOCK_MONOTONIC, &c1);
    u64 ns = (u64)((c1.tv_sec - c0.tv_sec) * 1000000000ll + (c1.tv_nsec - c0.tv_nsec));
    ze_ns += ns;
    if (ns > ze_max_ns) ze_max_ns = ns;
    ze_fields++; ze_sun_hidden += ze_sun_vis < 0.5f; ze_closed += ze_open < 0.2f;
    if (ze_dbg > 0 && (ze_fields % (u64)ze_dbg) == 0u) {
        int c60 = 0, c35 = 0;
        for (int k = 0; k < 6; k++) { c60 += ze_res[1 + k] == 1; c35 += ze_res[7 + k] == 1; }
        ps2_log("zero-vr: tunnel rays field %llu at %.0f %.0f %.0f: sun %.0f (%s%.0f m; dir %+.2f %+.2f %+.2f) | sky open %.3f "
                "(zenith %s%.0f m, 60 deg %d/6 clear, 35 deg %d/6 clear)%s | %.1f us (the sun's ray %.1f us)",
                (unsigned long long)f, o[0], o[1], o[2], ze_sun_vis, ze_sun_hit >= 0.0f ? "hit at " : "clear ",
                ze_sun_hit >= 0.0f ? ze_sun_hit : ze_sun_len, ze_sun_dir[0], ze_sun_dir[1], ze_sun_dir[2], ze_open,
                ze_res[0] == 0 ? "hit at " : ze_res[0] == 1 ? "clear " : "unknown ", ze_hit[0] >= 0.0f ? ze_hit[0] : ze_sky_len,
                c60, c35, all ? "; all rays (a jump)" : "", (double)ns / 1000.0, sun_us);
    }
    return 0;
}
static void ze_install(void) {
    extern int ps2_vk_enclosure_wanted(void);
    if (!ps2_vk_enclosure_wanted()) { ps2_log("zero-vr: light in tunnels off (ACZ_VR_TUNNEL_LIGHT=0): no rays"); return; }
    if (ps2_r32(ZG_SEGMENT) != 0x27BDFF20u || ps2_r32(0x00231808u) != 0x27BDFD70u) {
        ps2_log("zero-vr: light in tunnels: code mismatch at the segment test / the player update; off"); return;
    }
    ze_sun_len = env_float("ACZ_VR_TUNNEL_SUN_M", 3000.0f);
    if (!(ze_sun_len >= 200.0f && ze_sun_len <= 20000.0f)) ze_sun_len = 3000.0f;
    ze_sky_len = env_float("ACZ_VR_TUNNEL_SKY_M", 800.0f);
    if (!(ze_sky_len >= 100.0f && ze_sky_len <= 5000.0f)) ze_sky_len = 800.0f;
    ze_rays = (int)env_float("ACZ_VR_TUNNEL_RAYS", 2.0f);
    if (ze_rays < 1) ze_rays = 1;
    if (ze_rays > ZE_NSKY) ze_rays = ZE_NSKY;
    ze_sun_every = (int)env_float("ACZ_VR_TUNNEL_SUN_EVERY", 2.0f);
    if (ze_sun_every < 1) ze_sun_every = 1;
    if (ze_sun_every > 8) ze_sun_every = 8;
    ze_dbg = (int)env_float("ACZ_DEBUG_TUNNEL", 0.0f);
    ze_dirs_init();
    if (!zg_world_hooked) {
        if (ps2_hook_before(ZG_SEGMENT, zg_seg_enter, NULL, 60, "zero-gaze") < 0
            || ps2_hook_before(0x00231808u, zg_pl_enter, NULL, 59, "zero-gaze") < 0
            || ps2_hook_after(0x00231808u, zg_pl_leave, NULL, 59, "zero-gaze") < 0) {
            ps2_log("zero-vr: light in tunnels: the segment test hooks refused; off"); return;
        }
        zg_world_hooked = 1;
    }
    if (ps2_hook_before(0x00231808u, ze_pl_enter, NULL, 58, "zero-tunnel") < 0
        || ps2_hook_after(0x00231808u, ze_pl_leave, NULL, 58, "zero-tunnel") < 0) {
        ps2_log("zero-vr: light in tunnels: the player update hooks refused; off"); return;
    }
    ze_on = 1;
    ps2_log("zero-vr: light in tunnels: after each update of the aircraft %d rays through the game's segment test: to the "
            "sun (%.0f m) every %d fields, the rest to the sky (13 round robin, %.0f m) (ACZ_VR_TUNNEL_LIGHT=0 turns it off)",
            ze_rays, (double)ze_sun_len, ze_sun_every, (double)ze_sky_len);
}
static void ze_report(void) {
    if (!ze_fields) return;
    ps2_log("zero-vr: light in tunnels: %llu fields, %llu rays, %.1f us a field (max %.1f us; the sun's ray %.1f us); the "
            "sun hidden in %.1f %% of them, the sky closed (< 0.2 open) in %.1f %%, %llu jumps", (unsigned long long)ze_fields,
            (unsigned long long)ze_n, (double)ze_ns / (double)ze_fields / 1000.0, (double)ze_max_ns / 1000.0,
            ze_sun_n ? (double)ze_sun_ns / (double)ze_sun_n / 1000.0 : 0.0,
            100.0 * (double)ze_sun_hidden / (double)ze_fields, 100.0 * (double)ze_closed / (double)ze_fields,
            (unsigned long long)ze_jumps);
}

static void zero_boot_install(void) {
    const char *e;
    static const struct { u32 a, w; } chk[] = {
        { 0x00281A88u, 0x10800007u }, { 0x00281A8Cu, 0x8F8EDEF4u },     /* setNext: beqz a0 / lw t6,-0x210C(gp) */
        { 0x002810F8u, 0x0C0A06A2u }, { 0x00286E5Cu, 0x0C0A06A2u },     /* the two jal setNext sites */
        { 0x00289F40u, 0x27BDFFE0u }, { 0x0028A140u, 0x27BDFFB0u },     /* GameControl enter / exit prologues */
        { 0x001DD378u, 0x27BDFEA0u } };
    for (unsigned i = 0; i < sizeof chk / sizeof chk[0]; i++)
        if (ps2_r32(chk[i].a) != chk[i].w) { ps2_log("zero-boot: code mismatch at %08X; off", chk[i].a); return; }
    zb_debug = getenv("ACZ_DEBUG_SCENES") != NULL;
    e = getenv("ACZ_BOOT_MISSION"); zb_mission = e ? atoi(e) : 0;
    if (zb_mission < 0 || zb_mission > 18) zb_mission = 0;
    e = getenv("ACZ_BOOT_BRANCH"); zb_branch = e ? atoi(e) : 0;
    e = getenv("ACZ_BOOT_DIFF");   if (e) zb_diff = atoi(e);
    e = getenv("ACZ_BOOT_STAGE");  if (e) zb_stage = strtol(e, NULL, 0);
    e = getenv("ACZ_BOOT_AIRCRAFT"); if (e) zb_aircraft = strtol(e, NULL, 0);
    e = getenv("ACZ_BOOT_WINGMAN");  if (e) zb_wingman = strtol(e, NULL, 0);
    zb_hangar = getenv("ACZ_BOOT_HANGAR") && *getenv("ACZ_BOOT_HANGAR") == '1';
    zb_movies = getenv("ACZ_BOOT_MOVIES") && *getenv("ACZ_BOOT_MOVIES") == '1';
    e = getenv("ACZ_BOOT_SKIP");   if (e) zb_skip = atoi(e);
    e = getenv("ACZ_BOOT_SKIP_CHECK"); zb_skip_check_on = !(e && *e == '0');
    ps2_hook_before(ZB_SETNEXT, zb_setnext, NULL, 60, "zero-boot");
    ps2_hook_before(ZB_GC_ENTER, zb_gc_enter, NULL, 60, "zero-boot");
    ps2_hook_before(ZB_GC_EXIT, zb_gc_exit, NULL, 60, "zero-boot");
    ps2_hook_before(ZB_MISSION_TICK, zb_tick, NULL, 60, "zero-boot");
    if (zb_mission && zb_skip > 0 && zb_skip_check_on) {
        int ok = ps2_r32(ZB_PLAYER_UPDATE) == 0x27BDFD70u && ps2_r32(ZB_FRAME) == 0x27BDFEB0u
                 && ps2_hook_before(ZB_PLAYER_UPDATE, zb_pl_update, NULL, 61, "zero-boot") >= 0
                 && ps2_hook_before(ZB_FRAME, zb_frame, NULL, 61, "zero-boot") >= 0;
        if (!ok) {
            zb_skip_check_on = 0;
            ps2_log("zero-boot: player update / frame hook unavailable: the intro skip presses Start unchecked");
        }
    }
}

/* ---- head-tracked 3D sound (RESEARCH_2026-10-19.md section 7) ----
   The 3D sound listener: the main loop (0x104B64 -> 0x115020) calls 0x112700 (a0 = sound manager) once a frame. It
   takes the listener camera from mgr+0x4180 (view-context layout: eye +0x30, pitch/yaw/roll +0x40; 0 = no update),
   rotates up (0,1,0) and front (0,0,-1) by the camera's angles (0x106000, a VU0 routine, not Camera::Update's) and
   calls 0x2DCE30(a0 position, a1 velocity, a2 front, a3 up; vectors in 0x112700's frame, unused after the call),
   which jumps to 0x2DDB48: that normalises them into 0x422C18 (position), 0x422C24 (velocity), 0x422C30 (front),
   0x422C3C (up). The panner (0x2DD138) measures every source's azimuth against front/up. The camera hook gives the
   game its own eye and angles back after every update, so sound effects were panned by the aircraft, not the head:
   in the cockpit (free look, world-locked) a sound on the right stayed on the right when the head turned to it.
   A before-hook on 0x2DCE30 rewrites front/up as the head-turned camera's (N = R*H: the angles the frame was drawn
   with, kept by headcam_note) when the listener camera is a context the head turned in the last 2 fields and the
   game's own vectors match that context's restored angles (dot > 0.999 for front and up; R = m3_euler, checked per
   frame because 0x106000 is a different routine). Anything else keeps the game's vectors; after 300 mismatches
   without a match the hook gives up and logs it. Head-locked frames (stick look outside the cockpit) have no head
   in the camera (the head drives the game's own camera there), so nothing is applied twice. Position and velocity
   stay the game's. Radio, voice and music are streams with fixed volumes, not 3D voices: untouched.
   ACZ_VR_AUDIO_HEAD=0 turns it off (default on; it only acts while head tracking runs).
   ACZ_DEBUG_AUDIO=N: every N fields log the game's output mode (s16 0x427D90: 0 mono, 1 stereo = default, 2-3
   another pan law, 0x2E33E8), the listener camera, the head-tracked contexts, the vectors, the check and the
   sounding 3D voices (azimuth s16 at 0x424864+16*i: 127 = 180 deg, gain f32 at 0x424860+16*i, 3D flag u8 at
   0x423420+0x6C*i+0x26). */
#define Z_SND_LISTENER  0x00112700u     /* 0x112700(a0 = sound manager): builds the listener from mgr+0x4180 */
#define Z_SND_SET_LSN   0x002DCE30u     /* (pos, vel, front, up) -> j 0x2DDB48 */
#define Z_SND_LSN_RA    0x00112948u     /* return address of its only call */
static u32 z_snd_mgr;
static int z_snd_on = -1, z_snd_dbg;
static u64 z_snd_applied, z_snd_same, z_snd_nocam, z_snd_bad;
static int snd_mgr_enter(ps2_ctx *ctx, void *user) {
    (void)user;
    u32 m = ctx->r[4].uw[0];
    z_snd_mgr = m >= 0x100000u && m < 0x2000000u ? m : 0u;
    return 0;
}
static int snd_listener_enter(ps2_ctx *ctx, void *user) {
    (void)user;
    if (ctx->r[31].uw[0] != Z_SND_LSN_RA || !z_snd_mgr) return 0;
    u32 pf = ctx->r[6].uw[0], pu = ctx->r[7].uw[0];
    if (pf < 0x100000u || pf >= 0x2000000u || pu < 0x100000u || pu >= 0x2000000u) return 0;
    float F[3], U[3];
    for (u32 i = 0; i < 3; i++) { F[i] = f_of(ps2_r32(pf + 4u * i)); U[i] = f_of(ps2_r32(pu + 4u * i)); }
    u32 cam = ps2_r32(z_snd_mgr + 0x4180u);
    u64 f = ps2_kernel_vblank_count();
    int hc = -1;
    if (cam >= 0x100000u && cam < 0x2000000u && z_pose.valid)
        for (int i = 0; i < 4; i++)
            if (z_headcam[i].c == cam && f - z_headcam[i].field <= 2u) hc = i;
    float base[3] = { 0, 0, 0 }, Rb[3][3], Rh[3][3], df = 0.0f, du = 0.0f, dft = 0.0f;
    const char *what = "no head-tracked listener camera";
    if (hc >= 0) {
        for (u32 j = 0; j < 3; j++) base[j] = f_of(ps2_r32(cam + 0x40u + 4u * j));
        m3_euler(base[0], base[1], base[2], Rb);
        /* camera axes in the world (column vectors): front = R * (0,0,-1), up = R * (0,1,0) */
        df = -(F[0] * Rb[0][2] + F[1] * Rb[1][2] + F[2] * Rb[2][2]);
        du = U[0] * Rb[0][1] + U[1] * Rb[1][1] + U[2] * Rb[2][1];
        dft = -(F[0] * Rb[2][0] + F[1] * Rb[2][1] + F[2] * Rb[2][2]);      /* the transposed convention, for the log */
        float dh = 0.0f;
        for (int j = 0; j < 3; j++) dh += fabsf(f_of(z_headcam[hc].v[3 + j]) - base[j]);
        if (df < 0.999f || du < 0.999f) {
            what = "game vectors do not match the context's angles: kept";
            z_snd_bad++;
            if (z_snd_bad <= 5u || (z_snd_bad % 600u) == 0u)
                ps2_log("zero-vr: sound listener check failed (field %llu, camera %08X): front dot %.4f, up dot %.4f "
                        "(transposed %.4f); %llu mismatches, %llu matches", (unsigned long long)f, cam, df, du, dft,
                        (unsigned long long)z_snd_bad, (unsigned long long)(z_snd_applied + z_snd_same));
            if (z_snd_bad >= 300u && !z_snd_applied && !z_snd_same) {
                ps2_log("zero-vr: head-tracked sound off: the listener's rotation never matched the camera's");
                z_snd_on = 0;
            }
        } else if (z_snd_on <= 0) {
            what = "off (ACZ_VR_AUDIO_HEAD=0)";
        } else if (dh < 1e-5f) {
            what = "no head turn in this camera (head-locked or centred)";
            z_snd_same++;
        } else {
            m3_euler(f_of(z_headcam[hc].v[3]), f_of(z_headcam[hc].v[4]), f_of(z_headcam[hc].v[5]), Rh);
            float nf[3] = { -Rh[0][2], -Rh[1][2], -Rh[2][2] }, nu[3] = { Rh[0][1], Rh[1][1], Rh[2][1] };
            for (u32 i = 0; i < 3; i++) { ps2_w32(pf + 4u * i, w_of(nf[i])); ps2_w32(pu + 4u * i, w_of(nu[i])); }
            what = "head applied";
            z_snd_applied++;
        }
    } else z_snd_nocam++;
    if (z_snd_dbg > 0 && f % (u64)z_snd_dbg == 0u) {
        static u64 last;
        if (last == f) return 0;
        last = f;
        char ln[900]; int n = 0;
        n += snprintf(ln + n, sizeof ln - (size_t)n, "zero-vr: sound field %llu mode %d mgr %08X cam %08X (%s) flags %04X sel %u",
                      (unsigned long long)f, (int)(s16)ps2_r16(0x00427D90u), z_snd_mgr, cam,
                      cam >= 0x100000u && cam < 0x2000000u && ps2_r32(cam) == 0x44200000u && ps2_r32(cam + 4u) == 0x43F00000u
                          ? "640x480" : "other", ps2_r16(z_snd_mgr + 0x418Cu), ps2_r8(0x003B1990u));
        n += snprintf(ln + n, sizeof ln - (size_t)n, " headcams");
        for (int i = 0; i < 4; i++)
            if (z_headcam[i].c) n += snprintf(ln + n, sizeof ln - (size_t)n, " %08X@%llu", z_headcam[i].c,
                                              (unsigned long long)(f - z_headcam[i].field));
        n += snprintf(ln + n, sizeof ln - (size_t)n, " | game front %.3f %.3f %.3f up %.3f %.3f %.3f", F[0], F[1], F[2],
                      U[0], U[1], U[2]);
        if (hc >= 0)
            n += snprintf(ln + n, sizeof ln - (size_t)n, " | base ang %.3f %.3f %.3f head ang %.3f %.3f %.3f dot f %.4f u %.4f (T %.4f)",
                          base[0], base[1], base[2], f_of(z_headcam[hc].v[3]), f_of(z_headcam[hc].v[4]),
                          f_of(z_headcam[hc].v[5]), df, du, dft);
        n += snprintf(ln + n, sizeof ln - (size_t)n, " | %s (applied %llu, unturned %llu, no cam %llu, mismatch %llu)", what,
                      (unsigned long long)z_snd_applied, (unsigned long long)z_snd_same, (unsigned long long)z_snd_nocam,
                      (unsigned long long)z_snd_bad);
        ps2_log("%s", ln);
        n = snprintf(ln, sizeof ln, "zero-vr: sound 3D voices (ch:az/gain):");
        for (u32 i = 0; i < 48u && n < (int)sizeof ln - 24; i++) {
            float g = f_of(ps2_r32(0x00424860u + 16u * i));
            if (ps2_r8(0x00423420u + 0x6Cu * i + 0x26u) && g > 0.0f && g < 100.0f)
                n += snprintf(ln + n, sizeof ln - (size_t)n, " %u:%d/%.2f", i, (int)(s16)ps2_r16(0x00424864u + 16u * i), g);
        }
        ps2_log("%s", ln);
    }
    return 0;
}
static int snd_listener_hook(ps2_ctx *ctx, void *user) {
    return z_snd_on > 0 || z_snd_dbg > 0 ? snd_listener_enter(ctx, user) : 0;
}
static void snd_listener_install(void) {
    const char *e = getenv("ACZ_VR_AUDIO_HEAD");
    z_snd_on = !(e && *e == '0');
    e = getenv("ACZ_DEBUG_AUDIO");
    z_snd_dbg = e ? atoi(e) : 0;
    static const u32 lw[4] = { 0x27BDFFF0u, 0xFFBF0000u, 0xDFBF0000u, 0x080B76D2u };   /* 0x2DCE30: j 0x2DDB48 */
    int ok = ps2_r32(Z_SND_LISTENER) == 0x27BDFEB0u && ps2_r32(Z_SND_LSN_RA - 8u) == 0x0C0B738Cu;  /* jal 0x2DCE30 */
    for (u32 i = 0; i < 4; i++) if (ps2_r32(Z_SND_SET_LSN + 4u * i) != lw[i]) ok = 0;
    if (!ok) { ps2_log("zero-vr: head-tracked sound skipped (code mismatch)"); z_snd_on = 0; return; }
    if (!z_snd_on && z_snd_dbg <= 0) { ps2_log("zero-vr: head-tracked sound off (ACZ_VR_AUDIO_HEAD=0)"); return; }
    if (ps2_hook_before(Z_SND_LISTENER, snd_mgr_enter, NULL, 60, "zero-audio") < 0
        || ps2_hook_before(Z_SND_SET_LSN, snd_listener_hook, NULL, 60, "zero-audio") < 0) {
        ps2_log("zero-vr: head-tracked sound hook refused");
        z_snd_on = 0;
        return;
    }
    ps2_log("zero-vr: head-tracked sound %s (listener 0x2DCE30%s)", z_snd_on ? "on" : "off, logging only",
            z_snd_dbg > 0 ? ", ACZ_DEBUG_AUDIO" : "");
}
static void snd_listener_report(void) {
    if (z_snd_applied || z_snd_same || z_snd_bad)
        ps2_log("zero-vr: sound listener: head applied %llu, unturned %llu, no head-tracked camera %llu, mismatch %llu",
                (unsigned long long)z_snd_applied, (unsigned long long)z_snd_same, (unsigned long long)z_snd_nocam,
                (unsigned long long)z_snd_bad);
}

/* 1 when the loaded game is Ace Combat Zero (NTSC-U): code at the view context update matches. Other subsystems
   written for AC5 (the params mod host hooks AC5 addresses) use it to stay off. */
int zero_vr_is_zero(void) {
    for (int i = 0; i < 4; i++)
        if (ps2_r32(Z_CTX_UPDATE + 4u * (u32)i) != z_ctx_update_w[i]) return 0;
    return 1;
}

/* ---- leaving the game (ps2_quit.c): QUIT GAME in the pause menu, the game state for the controller combo, and the
   way back to the main menu ----

   The in-mission pause menu (NMissionControl::NMenu::CMenuPause, vtable 0x3CC5B0; the object hangs at GameControl's
   game object +0x5C494) is a tree of CSelectItem (0x54 bytes, vtable 0x3CC658): +0x04.. child pointers, +0x44 item
   id, +0x48/+0x4C text (font group, index), +0x50 highlighted, +0x51 open, +0x52 child count, +0x53 default child (a
   confirmation's CANCEL). The tree is built once per mission load by 0x258A30 (campaign: RETURN TO GAME 0, REPLAY 1,
   RETRY 2 [YES 3 / CANCEL 4], QUIT MISSION 5 [YES 6 / CANCEL 7], FLIGHT OPTIONS 8 [9..15, values 16..30]) or by
   0x258DF0 / 0x2591A0 (four rows, no REPLAY), each starting with 0x25BBE8(root, n) (allocate n children).
   Per frame the menu's update 0x2576C8 turns the pad into menu +0x4C = highlighted id and +0x48 = chosen id (-1 for
   none); choosing 4 or 7 (CANCEL) or 16..30 (an option value) goes back a level (0x25BDB0(root, 7)). The mission
   controller hands +0x48 to 0x1D8A70, a jump table for ids -2..30 (0 resume, 1 replay, 3 retry, 6 quit mission:
   GameControl ends, MainSelect follows); ids >= 31 do nothing there.
   Texts: the font object *(0x3FC7DC) holds groups of entries; 0x2402F8(font, group, index) returns an entry: +0 glyph
   count, +4 x, +8 y (an entry carries its own screen position: the pause rows are 25 lines apart), +0xC height, +0xE
   spacing, +0x10 y offset, +0x14 sum of glyph widths, +0x16 flags, then per glyph u, v, width, flags (0x80 = space) in
   the font atlas. The pause rows are group 4, their confirmations groups 5 and 6.

   QUIT GAME: the builder's first call 0x25BBE8(root, n) is made with n + 1, so the game allocates the extra row itself
   (and frees it with the menu). When the builder returns, the new last row gets an unused id, its text and two
   children (0x25BBE8(row, 2)): YES (another unused id) and CANCEL (the QUIT MISSION confirmation's CANCEL id, so the
   game's own "go back" handles it). The texts are new entries in a page mapped at guest 0x0F000000, served by a
   replacement of 0x2402F8 for (group, 40..42): "QUIT GAME" is QUIT MISSION's "QUIT " glyphs followed by RETURN TO
   GAME's "GAME", one row below the last item; YES / CANCEL are QUIT MISSION's confirmation one row lower. Choosing YES
   requests the clean quit (ps2_quit_request). The row only appears when every piece checks out (ids, glyph counts);
   otherwise the menu stays as the game built it (logged). The controller prompt's "Main menu" (paused only) gives
   the menu QUIT MISSION's YES as its choice, so the game ends the mission its own way.
   ENHANCEMENTS (bug #64): one more row above QUIT GAME, made the same way: a leaf with an unused id whose text is
   composed from the pause font's own glyphs (the HUD atlas rows v 198/210: the game's full upper-case alphabet; the
   map below is checked against QUIT MISSION's and RETURN TO GAME's glyphs first); while settings wait for a restart it
   reads "ENHANCEMENTS (RESTART)". Choosing it opens the ENHANCEMENTS page (ps2_quit.c: the headset panel in the game's
   font and colours) over the paused game; while the page is up the pause menu's own draw (0x257BB8) is skipped and
   the game sees a neutral pad. ACZ_ENH_MENU=0 leaves the row out (F4 and the View + Menu panel still open the page).
   TOUCH CONTROLS (bug #239, touch_ux 2026-10-29): with the Quest Touch controllers on (ACZ_TOUCH pad / stick) one more
   leaf after ENHANCEMENTS, made the same way; it shows the Touch card (ps2_quit.c ps2_quit_touch_card_open: both
   controllers with what each button does) over the paused game. ACZ_TOUCH_CARD_ROW=0 leaves it out.
   ACZ_QUIT_MENU=0 leaves the pause menu alone. ACZ_DEBUG_PAUSE=1 logs the tree and every change of the menu's state;
   ACZ_DEBUG_PAUSE=2 also takes the fallback on purpose (the extra rows are given back: the game's own menu);
   ACZ_DEBUG_PAUSE=3 also logs the pause font's text entries once (group, glyph records). */
#include "ps2_quit.h"
#include "ps2_settings.h"
#define ZQ_ALLOC_CHILDREN 0x0025BBE8u   /* CSelectItem: (item, n) -> n fresh children */
#define ZQ_BUILD0         0x00258A30u   /* pause tree builders (mode 0 campaign, 1, 2) */
#define ZQ_BUILD1         0x00258DF0u
#define ZQ_BUILD2         0x002591A0u
#define ZQ_PAUSE_UPDATE   0x002576C8u   /* CMenuPause vtable +0x38: per frame while the menu is shown */
#define ZQ_PAUSE_DRAW     0x00257BB8u   /* CMenuPause draw: (menu, packet, ...) -> packet; +4 = shown */
#define ZQ_TEXT           0x002402F8u   /* font text entry lookup (font, group, index) */
#define ZQ_FONT_PTR       0x003FC7DCu
#define ZQ_PAGE           0x0F000000u   /* guest page holding the new text entries */
#define ZQ_TXT_IDX        40u           /* text indices served by the replacement: 40 QUIT GAME, 41 YES, 42 CANCEL, */
#define ZQ_ENH_IDX        43u           /* 43 ENHANCEMENTS */
#define ZQ_TCH_IDX        44u           /* 44 TOUCH CONTROLS */
static int zq_debug, zq_menu_on, zq_enh_on, zq_tch_on, zq_extra;
static u32 zq_enh_id = 0xFFFFFFFFu;      /* the ENHANCEMENTS row */
static u32 zq_tch_id = 0xFFFFFFFFu;      /* the TOUCH CONTROLS row (bug #239) */
static u32 zq_e_quit, zq_e_yes, zq_e_can, zq_e_enh, zq_e_enh2, zq_e_tch;   /* our entries in the page (0 = none) */
static volatile u64 zq_pause_field;      /* field of the last pause menu update: the game is paused */
static u32 zq_root_bumped;               /* root whose first allocation was made one row larger */
static u32 zq_yes_id = 0xFFFFFFFFu;      /* QUIT GAME's YES */
static u32 zq_qm_yes_id = 0xFFFFFFFFu;   /* QUIT MISSION's YES (the way to the main menu) in zq_qm_menu */
static u32 zq_qm_menu;
static volatile int zq_mainmenu_req;
static u32 zq_txt_group = 0xFFFFFFFFu, zq_yes_group = 0xFFFFFFFFu;
static int zq_txt_ok;                    /* entries in the page are valid */
static u8 zq_page[4096] __attribute__((aligned(4096)));

static int zq_ptr(u32 p) { return p >= 0x100000u && p < 0x2000000u; }

/* Call a game function from a hook (EE thread): registers are saved and restored around it; returns v0. */
static u32 zq_call(ps2_ctx *ctx, u32 fn, u32 a0, u32 a1, u32 a2, u32 a3) {
    ps2_reg128 rs[32];
    ps2_fpr fs[32];
    ps2_reg128 hi = ctx->hi, lo = ctx->lo;
    float acc = ctx->acc;
    u32 fcc = ctx->fcc, fcr = ctx->fcr31, sa = ctx->sa, pc = ctx->pc;
    memcpy(rs, ctx->r, sizeof rs);
    memcpy(fs, ctx->f, sizeof fs);
    ctx->r[4].ud[0] = (u64)(s64)(s32)a0; ctx->r[5].ud[0] = (u64)(s64)(s32)a1;
    ctx->r[6].ud[0] = (u64)(s64)(s32)a2; ctx->r[7].ud[0] = (u64)(s64)(s32)a3;
    ctx->r[29].ud[0] = (u64)(s64)(s32)((ctx->r[29].uw[0] - 0x80u) & ~15u);
    ctx->r[31].ud[0] = 0;                               /* no caller site: our own hooks key on ra */
    ps2_dispatch(ctx, fn);
    u32 v0 = ctx->r[2].uw[0];
    memcpy(ctx->r, rs, sizeof rs);
    memcpy(ctx->f, fs, sizeof fs);
    ctx->hi = hi; ctx->lo = lo; ctx->acc = acc; ctx->fcc = fcc; ctx->fcr31 = fcr; ctx->sa = sa; ctx->pc = pc;
    return v0;
}

static void zq_dump_item(u32 it, int depth) {
    if (!zq_ptr(it) || depth > 4) return;
    u32 n = ps2_r8(it + 0x52u);
    ps2_log("zero-quit: %*sitem %08X id %d text %d/%d children %u default %u", depth * 2, "", it,
            (int)ps2_r32(it + 0x44u), (int)ps2_r32(it + 0x48u), (int)ps2_r32(it + 0x4Cu), n, ps2_r8(it + 0x53u));
    for (u32 i = 0; i < n && i < 16; i++) zq_dump_item(ps2_r32(it + 4u + 4u * i), depth + 1);
}
static void zq_ids(u32 it, u32 *used, int depth) {      /* bitmap of the ids in the tree (0..63) */
    if (!zq_ptr(it) || depth > 4) return;
    u32 id = ps2_r32(it + 0x44u);
    if (id < 64u) used[id >> 5] |= 1u << (id & 31u);
    for (u32 i = 0; i < ps2_r8(it + 0x52u) && i < 16u; i++) zq_ids(ps2_r32(it + 4u + 4u * i), used, depth + 1);
}
static u32 zq_child_with_id(u32 root, u32 id) {
    for (u32 i = 0; i < ps2_r8(root + 0x52u) && i < 16u; i++) {
        u32 c = ps2_r32(root + 4u + 4u * i);
        if (zq_ptr(c) && ps2_r32(c + 0x44u) == id) return c;
    }
    return 0;
}
static u32 zq_entry_size(u32 n) { return 0x18u + 4u * n; }

/* before 0x25BBE8: the builders' first call (the root's rows) gets the extra rows (ENHANCEMENTS, QUIT GAME) */
static int zq_alloc_enter(ps2_ctx *ctx, void *u) {
    (void)u;
    u32 ra = ctx->r[31].uw[0], a0 = ctx->r[4].uw[0], n = ctx->r[5].uw[0] & 0xFFu;
    if ((ra == ZQ_BUILD0 + 0x5Cu || ra == ZQ_BUILD1 + 0x5Cu || ra == ZQ_BUILD2 + 0x5Cu) && zq_ptr(a0) && n >= 2u
        && n + (u32)zq_extra < 16u) {
        ctx->r[5].ud[0] = n + (u32)zq_extra;
        zq_root_bumped = a0;
        if (zq_debug) ps2_log("zero-quit: pause tree root %08X: %u rows -> %u", a0, n, n + (u32)zq_extra);
    }
    return 0;
}

/* An extra row could not be set up: give the last k rows back (the game's own item cleanup 0x25BB90 and free
   0x102DF0) and shrink the root, so the menu is exactly the game's (k = all extra rows) or keeps the rows before. */
#define ZQ_ITEM_CLEAR 0x0025BB90u
#define ZQ_FREE       0x00102DF0u
static void zq_undo(ps2_ctx *ctx, u32 root, u32 n, u32 k) {
    for (; k > 0 && n > 0; k--, n--) {
        u32 row = ps2_r32(root + 4u + 4u * (n - 1u));
        if (zq_ptr(row)) {
            zq_call(ctx, ZQ_ITEM_CLEAR, row, 0, 0, 0);
            zq_call(ctx, ZQ_FREE, row, 0, 0, 0);
        }
        ps2_w32(root + 4u + 4u * (n - 1u), 0u);
        ps2_w8(root + 0x52u, (u8)(n - 1u));
    }
}

/* The pause font's h11 glyphs (the HUD atlas: space and '!'..'E' on row v 198, 'F'..'f' on row 210; u, width, flags
   0x80 space / 0x40 digit / 0x20 sign), for texts the game has no entry for. -1 = not in the map. */
static int zq_glyph(char c, u8 *u, u8 *v, u8 *w, u8 *fl) {
    static const u8 fz[21] = { 0, 8, 16, 24, 29, 37, 45, 53, 62, 70, 78, 86, 95, 103, 111, 119, 127, 135, 147, 155, 163 };
    *v = 198; *w = 7; *fl = 0;
    if (c == ' ') { *u = 0; *w = 3; *fl = 0x80; }
    else if (c >= 'A' && c <= 'E') *u = (u8)(211 + 8 * (c - 'A'));
    else if (c >= 'F' && c <= 'Z') {
        *u = fz[c - 'F']; *v = 210;
        if (c == 'I') *w = 4;
        else if (c == 'M' || c == 'Q') *w = 8;
        else if (c == 'W') *w = 11;
    } else if (c >= '0' && c <= '9') { *u = (u8)(97 + 8 * (c - '0')); *fl = 0x40; }
    else if (c == '(') { *u = 49; *w = 5; *fl = 0x20; }
    else if (c == ')') { *u = 55; *w = 5; *fl = 0x20; }
    else if (c == '-') { *u = 79; *fl = 0x20; }
    else if (c == '/') { *u = 89; *fl = 0x20; }
    else return -1;
    return 0;
}
/* 1 when the map gives exactly the glyph records of the game's own entry e for the text s */
static int zq_map_matches(u32 e, const char *s) {
    if (ps2_r32(e) != (u32)strlen(s)) return 0;
    for (u32 j = 0; s[j]; j++) {
        u8 u, v, w, fl;
        if (zq_glyph(s[j], &u, &v, &w, &fl) < 0) return 0;
        u32 g = e + 0x18u + 4u * j;
        if (ps2_r8(g) != u || ps2_r8(g + 1u) != v || ps2_r8(g + 2u) != w || (ps2_r8(g + 3u) & 0xE0u) != fl) return 0;
    }
    return 1;
}
/* a text entry at a for s, with the header (height, spacing, flags, x) of the game's entry hdr, at row y; returns the
   next free 16-byte aligned address */
static u32 zq_compose(u32 a, u32 hdr, const char *s, u32 y) {
    u32 n = (u32)strlen(s), wsum = 0;
    for (u32 k = 0; k < 0x18u; k++) ps2_w8(a + k, ps2_r8(hdr + k));
    ps2_w32(a, n);
    ps2_w32(a + 8u, y);
    for (u32 j = 0; j < n; j++) {
        u8 u, v, w, fl;
        if (zq_glyph(s[j], &u, &v, &w, &fl) < 0) zq_glyph(' ', &u, &v, &w, &fl);
        ps2_w8(a + 0x18u + 4u * j, u); ps2_w8(a + 0x19u + 4u * j, v);
        ps2_w8(a + 0x1Au + 4u * j, w); ps2_w8(a + 0x1Bu + 4u * j, fl);
        wsum += w;
    }
    ps2_w16(a + 0x14u, (u16)wsum);
    return a + ((zq_entry_size(n) + 15u) & ~15u);
}

/* ---- ACZ_ENH_NATIVE (R8, night_menu 2026-11-04, docs/research_2026-11-04/pause_menu_layout.md): the ENHANCEMENTS page
   of the pause menu drawn by the game itself, in its FLIGHT OPTIONS layout: the frame, the map, the grid, the fonts, the
   cursor box, the typing-in and the sounds are the game's. ps2_quit.c keeps the page's logic on the renderer thread (the
   game sees a neutral pad); here the page's state (a snapshot, ps2_quit_enh_snap) goes into the tree and the text entries
   after each menu update:
   - the ENHANCEMENTS row gets 7 row children, each with 6 value children (ids >= 64: nothing in the jump table 0x1D8A70);
     +0x52 of the row and of its rows says how many are in use, the cleanup 0x25BB90 frees all 16 slots anyway;
   - choosing the row (the game's own op 6 opens it) points menu +0x25C, the item the FLIGHT OPTIONS level draws, at it;
     closing puts everything back, so the game's own FLIGHT OPTIONS is untouched;
   - menu +0x274[r] (the value index the game rewrites each frame from the player's options) is set to 0 after the update:
     a row's value is its child 0 at the value column; a row's list is its children at their own x, y;
   - texts: (7, 64..70) labels, (8, 72 + row x 6 + k) values, in a guest page; 3/3 (breadcrumb) and 3/2 (the blank heading
     under the rule: "8-14 OF 21", the notice's title) are served while the page is up;
   - the help line is the game's own text stream in its own help font, not a font entry: a file of a text stream holds
     only the glyphs its text uses, each cell with its character code at +8 (0x107C90; format in the note), so a stream of
     ours in the guest page (one line, the wrap in negative codes) gets the glyph cells of every stream in memory (help,
     objectives, radio ...), other sizes of the face scaled to the help's, and is swapped into menu +0x270 while the page
     is up; the highlighted id menu +0x4C is set by hand to 9 (line 0). ';' is made from ':' and ','; other signs no stream
     has become words or a near sign (zn_help_clean); a letter falls back to the other case (Z in mission 1; logged). A
     text longer than 4 lines of the panel is cut (logged; none of the 84 page rows since 2026-11-05).
     ACZ_ENH_NATIVE_HELP=ascii: the game's ASCII font instead (height 10, upper case only), drawn after 0x2583E8 like the
     ':' (zn_help_after; +0x4C far past the file, so the game's own help draws nothing); also when no stream has a cell.
   ACZ_ENH_NATIVE=1 turns it on (default 0: the headset overlay of ps2_quit.c, also for the View + Menu panel, the F4 menu
   and the TOUCH CONTROLS card in any case). */
#define ZN_PAGE       0x0F100000u
#define ZN_PAGE_SIZE  0x20000u
#define ZN_ROWS       PS2_ENH_SNAP_ROWS
#define ZN_VALS       PS2_ENH_SNAP_VALS
#define ZN_IDX_ROW    64u              /* (7, 64 + slot): a row's label */
#define ZN_IDX_VAL    72u              /* (8, 72 + slot x ZN_VALS + k): a row's k-th value */
#define ZN_GLYPHS     40u
#define ZN_SLOT       0xC0u            /* 0x18 + 4 x ZN_GLYPHS, rounded up */
enum { ZN_S_LABEL = 0, ZN_S_VAL = ZN_ROWS, ZN_S_LIST = ZN_S_VAL + ZN_ROWS, ZN_S_CRUMB = ZN_S_LIST + ZN_VALS, ZN_S_HEAD, ZN_S_N };
#define ZN_SCRATCH    (ZN_PAGE + (u32)ZN_S_N * ZN_SLOT)     /* 16 bytes for the sound's arguments, then a C string */
#define ZN_STRING     (ZN_SCRATCH + 16u)
#define ZN_PAUSE_HELP 0x002583E8u      /* draws the rule, the heading and the help / objectives text */
#define ZN_FONT_DRAW  0x002544D8u      /* (menu, packet, font, ?, x, y, spacing, GS reg, [colour *], [C string]) */
#define ZN_SND_OBJ    0x00114F50u
#define ZN_SND_PLAY   0x00115AD0u
#define ZN_SET_TEXT   0x00107C90u      /* a text stream takes its file (stream, &file) */
#define ZN_MAX_STREAMS 48                /* a ring of the newest: mission 1 sets up 25 (radio 461 lines, hints, help ...) */
static u32 zn_streams[ZN_MAX_STREAMS];   /* (stream + 8 of the class: this is the base part) */
static int zn_nstreams, zn_stream_next;
static int zn_on, zn_ready, zn_active;
static u32 zn_menu, zn_row_item, zn_fo_item, zn_rows[ZN_ROWS], zn_vals[ZN_ROWS][ZN_VALS];
static u32 zn_hdr_row, zn_hdr_val, zn_hdr_crumb, zn_hdr_head;
static s32 zn_col;                     /* the value column of the page */
static int zn_col_cat = -2;
static ps2_enh_snap zn_s;              /* the snapshot in the entries */
static u8 zn_page[ZN_PAGE_SIZE] __attribute__((aligned(4096)));
#define ZN_H_OBJ      (ZN_PAGE + 0x1800u)    /* the text stream object (0x4C bytes, class 0x3CCAB0) */
#define ZN_H_CELLS    (ZN_PAGE + 0x2000u)    /* ... its parts: 16 bytes per cell */
#define ZN_H_LENS     (ZN_PAGE + 0x2C80u)
#define ZN_H_OFFS     (ZN_PAGE + 0x2C90u)
#define ZN_H_CODES    (ZN_PAGE + 0x2CA0u)
#define ZN_H_CLUT     (ZN_PAGE + 0x3400u)
#define ZN_H_PIX      (ZN_PAGE + 0x4000u)    /* 512 bytes per cell */
#define ZN_H_CELLMAX  200
#define ZN_H_CODEMAX  900u
#define ZN_H_WIDTH    386                    /* x 68 .. 454 */
#define ZN_H_LINES    4
#define ZN_H_CELLSIZE 32u
#define ZN_CLASS_CTOR 0x00264A40u
#define ZN_MENU_FREE  0x00257630u            /* the menu's teardown (it frees +0x270) */
static struct { u16 code, w; } zn_hc[ZN_H_CELLMAX];
static int zn_hn, zn_hascii;
static u32 zn_h_saved;                       /* the game's help stream while ours is in +0x270 */

/* the menu font (height 11, HUD atlas rows v 198, 210) and the small one (height 8, row v 22), as ps2_quit.c gf_find */
static int zn_glyph(int small, char ch, u8 *u, u8 *v, u8 *w, u8 *fl) {
    *fl = 0;
    if (ch == ' ') { *u = 0; *v = 198; *w = 3; *fl = 0x80; return 0; }
    if (small) {
        static const char sig[] = ".,:;'\"-/[]>";
        static const u8 su[] = { 216, 218, 220, 222, 224, 226, 230, 236, 242, 247, 252 };
        static const u8 sw[] = { 1, 1, 1, 1, 1, 3, 5, 5, 4, 4, 4 };
        static const u8 du[] = { 0, 6, 11, 17, 23, 29, 35, 41, 47, 53 };
        *v = 22; *w = 5;
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
        if (ch >= '0' && ch <= '9') { *u = du[ch - '0']; if (ch == '1') *w = 4; *fl = 0x40; return 0; }
        if (ch >= 'A' && ch <= 'Z') {
            int k = ch - 'A';
            *u = (u8)(59 + 6 * k - (k > 8 ? 2 : 0) + (k > 22 ? 3 : 0));
            if (ch == 'I') *w = 3;
            if (ch == 'W') *w = 8;
            return 0;
        }
        const char *q = ch ? strchr(sig, ch) : NULL;
        if (q) { *u = su[q - sig]; *w = sw[q - sig]; *fl = 0x20; return 0; }
        return -1;
    }
    if (ch >= '!' && ch <= 'E') {
        static const u8 u198[] = { 8, 10, 15, 23, 31, 39, 47, 49, 55, 61, 69, 77, 79, 87, 89,     /* ! .. / */
                                   97, 105, 113, 121, 129, 137, 145, 153, 161, 169,               /* 0 .. 9 */
                                   177, 179, 181, 188, 196, 203, 0, 211, 219, 227, 235, 243 };    /* : .. E */
        static const u8 w198[] = { 1, 4, 7, 7, 7, 7, 1, 5, 5, 7, 7, 1, 7, 1, 7,
                                   7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 1, 1, 6, 7, 6, 7, 0, 7, 7, 7, 7, 7 };
        int k = ch - '!';
        if (!w198[k]) return -1;
        *u = u198[k]; *v = 198; *w = w198[k];
        *fl = ch >= '0' && ch <= '9' ? 0x40 : ch < 'A' ? 0x20 : 0;
        return 0;
    }
    static const char c210[] = "FGHIJKLMNOPQRSTUVWXYZ[]^_abcdef";
    static const u8 u210[] = { 0, 8, 16, 24, 29, 37, 45, 53, 62, 70, 78, 86, 95, 103, 111, 119, 127, 135, 147, 155,
                               163, 171, 185, 191, 197, 205, 213, 221, 229, 237, 245 };
    static const u8 w210[] = { 7, 7, 7, 4, 7, 7, 7, 8, 7, 7, 7, 8, 7, 7, 7, 7, 7, 11, 7, 7, 7, 5, 5, 5, 7, 7, 7, 7, 7, 7, 7 };
    const char *q = ch ? strchr(c210, ch) : NULL;
    if (q) { *u = u210[q - c210]; *v = 210; *w = w210[q - c210]; return 0; }
    return -1;
}
/* the width as the game measures it: 0x23FF70 with the entry's spacing (2) */
static int zn_width(const char *s) {
    int t = 0, n = 0;
    for (; *s; s++, n++) {
        u8 u, v, w, fl;
        if (zn_glyph(0, *s, &u, &v, &w, &fl) < 0) w = 3;
        t += w;
    }
    return n ? t + 2 * (n - 1) : 0;
}
static u32 zn_slot(int s) { return ZN_PAGE + (u32)s * ZN_SLOT; }
/* the text entry of a slot, with the header of the game's entry hdr, at (x, y) */
static void zn_entry(int slot, u32 hdr, const char *s, s32 x, s32 y, int small) {
    u32 a = zn_slot(slot), n = 0, wsum = 0;
    for (u32 k = 0; k < 0x18u; k++) ps2_w8(a + k, ps2_r8(hdr + k));
    for (; s[n] && n < ZN_GLYPHS; n++) {
        u8 u, v, w, fl;
        if (zn_glyph(small, s[n], &u, &v, &w, &fl) < 0) zn_glyph(small, ' ', &u, &v, &w, &fl);
        ps2_w8(a + 0x18u + 4u * n, u); ps2_w8(a + 0x19u + 4u * n, v);
        ps2_w8(a + 0x1Au + 4u * n, w); ps2_w8(a + 0x1Bu + 4u * n, fl);
        wsum += w;
    }
    ps2_w32(a, n);
    ps2_w32(a + 4u, (u32)x);
    ps2_w32(a + 8u, (u32)y);
    ps2_w16(a + 0x14u, (u16)wsum);
}
/* a text the game asks for that is ours (0: not ours) */
static u32 zn_text(u32 g, u32 i) {
    if (i >= ZN_IDX_ROW && i < ZN_IDX_ROW + ZN_ROWS && g == 7u) return zn_slot(ZN_S_LABEL + (int)(i - ZN_IDX_ROW));
    if (i >= ZN_IDX_VAL && i < ZN_IDX_VAL + ZN_ROWS * ZN_VALS && g == 8u) {
        u32 r = (i - ZN_IDX_VAL) / ZN_VALS, k = (i - ZN_IDX_VAL) % ZN_VALS;
        return (int)r == zn_s.open ? zn_slot(ZN_S_LIST + (int)k) : zn_slot(ZN_S_VAL + (int)r);
    }
    if (zn_active && g == 3u && i == 3u) return zn_slot(ZN_S_CRUMB);
    if (zn_active && g == 3u && i == 2u) return zn_slot(ZN_S_HEAD);
    return 0;
}
/* after the builder: the ENHANCEMENTS row gets its rows; on any doubt it stays a leaf and the overlay serves */
static void zn_build(ps2_ctx *ctx, u32 menu, u32 row) {
    zn_ready = zn_active = 0;
    zn_menu = 0;
    if (!zn_on) return;
    u32 font = ps2_r32(ZQ_FONT_PTR);
    zn_hdr_row = zq_call(ctx, ZQ_TEXT, font, 7u, 0u, 0);
    zn_hdr_val = zq_call(ctx, ZQ_TEXT, font, 8u, 0u, 0);
    zn_hdr_crumb = zq_call(ctx, ZQ_TEXT, font, 3u, 3u, 0);
    zn_hdr_head = zq_call(ctx, ZQ_TEXT, font, 3u, 2u, 0);
    if (!zq_ptr(zn_hdr_row) || !zq_ptr(zn_hdr_val) || !zq_ptr(zn_hdr_crumb) || !zq_ptr(zn_hdr_head)
        || (s32)ps2_r32(zn_hdr_row + 8u) != 90 || (s32)ps2_r32(zn_hdr_val + 4u) != 249) {
        ps2_log("zero-quit: ACZ_ENH_NATIVE: the FLIGHT OPTIONS texts are not as expected; the page stays the overlay");
        return;
    }
    zq_call(ctx, ZQ_ALLOC_CHILDREN, row, ZN_ROWS, 0, 0);
    if (ps2_r8(row + 0x52u) != ZN_ROWS) { ps2_w8(row + 0x52u, 0u); ps2_log("zero-quit: ACZ_ENH_NATIVE: no rows; the page stays the overlay"); return; }
    for (u32 r = 0; r < ZN_ROWS; r++) {
        u32 it = ps2_r32(row + 4u + 4u * r);
        if (!zq_ptr(it)) { ps2_w8(row + 0x52u, 0u); ps2_log("zero-quit: ACZ_ENH_NATIVE: no row %u; the page stays the overlay", r); return; }
        zn_rows[r] = it;
        ps2_w32(it + 0x44u, 9u + r);
        ps2_w32(it + 0x48u, 7u);
        ps2_w32(it + 0x4Cu, ZN_IDX_ROW + r);
        zq_call(ctx, ZQ_ALLOC_CHILDREN, it, ZN_VALS, 0, 0);
        for (u32 k = 0; k < ZN_VALS; k++) {
            u32 v = ps2_r32(it + 4u + 4u * k);
            if (!zq_ptr(v)) { ps2_w8(row + 0x52u, 0u); ps2_log("zero-quit: ACZ_ENH_NATIVE: no value %u/%u; the page stays the overlay", r, k); return; }
            zn_vals[r][k] = v;
            ps2_w32(v + 0x44u, 64u + r * ZN_VALS + k);
            ps2_w32(v + 0x48u, 8u);
            ps2_w32(v + 0x4Cu, ZN_IDX_VAL + r * ZN_VALS + k);
        }
        ps2_w8(it + 0x52u, 1u);
    }
    for (int s = 0; s < ZN_S_N; s++) zn_entry(s, zn_hdr_val, "", 0, 0, 0);
    zn_menu = menu;
    zn_row_item = row;
    zn_ready = 1;
    zn_s.open = -1;
    ps2_log("zero-quit: pause menu %08X: ENHANCEMENTS has %d rows of %d values for the page drawn by the game (ACZ_ENH_NATIVE)",
            menu, ZN_ROWS, ZN_VALS);
}
/* The streams come in four sizes of the same face (the header's line height): 14 help, objectives, weapon notes; 16 hints; 20
   the radio text, which alone has nearly the whole alphabet; 22 mission texts. Cells of another size are scaled to the
   help's: the factor for capitals and digits / for lower case, the baseline's row in the cell (measured on the cells'
   ink, ev/scale_test.png). */
static const struct { u16 line; float kcap, klow, base; } zn_h_font[] = {
    { 14, 1.0f, 1.0f, 15.0f }, { 20, 0.72f, 0.76f, 19.0f }, { 16, 0.78f, 0.85f, 13.0f }, { 22, 0.56f, 0.66f, 23.0f } };
#define ZN_H_FONTS ((int)(sizeof zn_h_font / sizeof zn_h_font[0]))
/* a PSMT4 cell (nibbles, the low one first) from guest sp to guest dp, scaled by k about the baseline: 4 x 4 samples */
static void zn_cell_scale(u32 sp, u32 dp, float k, float bsrc, float bdst) {
    static u8 src[ZN_H_CELLSIZE * ZN_H_CELLSIZE];
    for (u32 i = 0; i < ZN_H_CELLSIZE * ZN_H_CELLSIZE; i++) {
        u8 b = ps2_r8(sp + i / 2u);
        src[i] = i & 1u ? b >> 4 : b & 15u;
    }
    for (u32 ty = 0; ty < ZN_H_CELLSIZE; ty++) {
        for (u32 tx = 0; tx < ZN_H_CELLSIZE; tx += 2u) {
            u8 pair = 0;
            for (u32 h = 0; h < 2u; h++) {
                u32 t = 0;
                for (int a = 0; a < 4; a++)
                    for (int c = 0; c < 4; c++) {
                        float sx = ((float)(tx + h) + ((float)a + 0.5f) * 0.25f) / k;
                        float sy = ((float)ty + ((float)c + 0.5f) * 0.25f - bdst) / k + bsrc;
                        if (sx >= 0.0f && sy >= 0.0f && sx < (float)ZN_H_CELLSIZE && sy < (float)ZN_H_CELLSIZE) t += src[(u32)sy * ZN_H_CELLSIZE + (u32)sx];
                    }
                pair |= (u8)(((t + 8u) / 16u) << (4u * h));
            }
            ps2_w8(dp + (ty * ZN_H_CELLSIZE + tx) / 2u, pair);
        }
    }
}
/* the printable glyph cells of a text stream (s = the object's +8 part) of font size f that we do not have yet */
static int zn_help_take(u32 s, int f) {
    if (!zq_ptr(s)) return 0;
    u32 sz = ps2_r16(s + 0x18u), nc = ps2_r16(s + 0x14u), cells = ps2_r32(s + 4u), pix = ps2_r32(s + 0xCu);
    int got = 0;
    if (ps2_r16(s + 0x1Eu) != zn_h_font[f].line) return 0;
    if (sz != ZN_H_CELLSIZE || nc == 0u || nc > 4096u || !zq_ptr(cells) || !zq_ptr(pix)
        || !zq_ptr(cells + 16u * nc) || !zq_ptr(pix + 512u * nc)) return 0;   /* (an old stream may be stale) */
    for (u32 c = 0; c < nc && zn_hn < ZN_H_CELLMAX; c++) {
        u32 rec = cells + 16u * c, code = ps2_r16(rec + 8u), w = ps2_r16(rec), h = ps2_r16(rec + 2u);
        if (code < 0x20u || code > 0x7Eu || w > sz || h > sz) continue;
        int have = 0;
        for (int i = 0; i < zn_hn && !have; i++) have = zn_hc[i].code == code;
        if (have) continue;
        u32 d = ZN_H_CELLS + 16u * (u32)zn_hn, dp = ZN_H_PIX + 512u * (u32)zn_hn, sp = pix + 512u * c;
        for (u32 k = 0; k < 16u; k++) ps2_w8(d + k, ps2_r8(rec + k));
        ps2_w32(d + 4u, 0u);
        if (f == 0) for (u32 k = 0; k < 512u; k++) ps2_w8(dp + k, ps2_r8(sp + k));
        else {
            float k = code >= 'a' && code <= 'z' ? zn_h_font[f].klow : zn_h_font[f].kcap;
            zn_cell_scale(sp, dp, k, zn_h_font[f].base, zn_h_font[0].base);
            w = (u32)((float)w * k + 0.5f);
            ps2_w16(d, (u16)w);
            if (h > 24u) ps2_w16(d + 2u, 24u);
        }
        zn_hc[zn_hn].code = (u16)code;
        zn_hc[zn_hn].w = (u16)w;
        zn_hn++;
        got++;
    }
    return got;
}
/* our stream at the page's opening: all the glyphs in memory now, the game's stream's parameters */
static int zn_help_begin(ps2_ctx *ctx, u32 m) {
    u32 help = ps2_r32(m + 0x270u), obj = ps2_r32(m + 0x26Cu), gs = help + 8u;
    zn_hn = 0;
    if (zn_hascii || !zq_ptr(help) || ps2_r16(gs + 0x18u) != ZN_H_CELLSIZE) return 0;
    int from = 0;
    from += zn_help_take(gs, 0) > 0;
    if (zq_ptr(obj)) from += zn_help_take(obj + 8u, 0) > 0;
    /* newest first: a stream of an earlier mission may be freed memory by now; it only serves what this mission's lack */
    for (int f = 0; f < ZN_H_FONTS; f++)
        for (int i = 1; i <= zn_nstreams; i++) from += zn_help_take(zn_streams[(zn_stream_next - i + ZN_MAX_STREAMS) % ZN_MAX_STREAMS], f) > 0;
    {   /* no file has ';' (21 uses in our texts): the upper dot of ':' over the comma, rows of a cell being 16 bytes */
        int colon = -1, comma = -1, semi = -1;
        for (int i = 0; i < zn_hn; i++) {
            if (zn_hc[i].code == ':') colon = i;
            if (zn_hc[i].code == ',') comma = i;
            if (zn_hc[i].code == ';') semi = i;
        }
        if (semi < 0 && colon >= 0 && comma >= 0 && zn_hn < ZN_H_CELLMAX) {
            u32 d = ZN_H_CELLS + 16u * (u32)zn_hn, dp = ZN_H_PIX + 512u * (u32)zn_hn;
            for (u32 k = 0; k < 16u; k++) ps2_w8(d + k, ps2_r8(ZN_H_CELLS + 16u * (u32)colon + k));
            ps2_w16(d + 8u, ';');
            for (u32 y = 0; y < ZN_H_CELLSIZE; y++) {
                u32 from_c = ZN_H_PIX + 512u * (u32)(y < 11u ? colon : comma) + 16u * y;
                for (u32 k = 0; k < 16u; k++) ps2_w8(dp + 16u * y + k, ps2_r8(from_c + k));
            }
            zn_hc[zn_hn].code = ';';
            zn_hc[zn_hn].w = zn_hc[colon].w;
            zn_hn++;
        }
    }
    char have[200];
    int k = 0;
    for (int i = 0; i < zn_hn && k < 190; i++) have[k++] = (char)zn_hc[i].code;
    have[k] = 0;
    ps2_log("zero-quit: ACZ_ENH_NATIVE: help font: %d glyphs from %d of the %d text streams: %s", zn_hn, from, zn_nstreams + 2, have);
    if (!zn_hn) return 0;
    zq_call(ctx, ZN_CLASS_CTOR, ZN_H_OBJ, 0, 0, 0);
    u32 s = ZN_H_OBJ + 8u;
    for (u32 i = 0; i < 16u; i++) ps2_w8(s + 0x10u + i, ps2_r8(gs + 0x10u + i));
    ps2_w32(s + 0x10u, 1u);
    ps2_w16(s + 0x14u, (u16)zn_hn);
    ps2_w32(s + 4u, ZN_H_CELLS);
    ps2_w32(s + 8u, ZN_H_CLUT);
    ps2_w32(s + 0xCu, ZN_H_PIX);
    ps2_w32(s + 0x20u, ZN_H_LENS);
    ps2_w32(s + 0x24u, ZN_H_OFFS);
    ps2_w32(s + 0x28u, ZN_H_CODES);
    {   u32 clut = ps2_r32(gs + 8u);
        for (u32 i = 0; i < 64u; i++) ps2_w8(ZN_H_CLUT + i, ps2_r8(clut + i));
    }
    ps2_w16(ZN_H_LENS, 0u);
    ps2_w32(ZN_H_OFFS, 0u);
    zn_h_saved = help;
    ps2_w32(m + 0x270u, ZN_H_OBJ);
    return 1;
}
static int zn_help_cell(int ch) {
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < zn_hn; i++) if (zn_hc[i].code == ch) return i;
        ch = ch >= 'a' && ch <= 'z' ? ch - 32 : ch >= 'A' && ch <= 'Z' ? ch + 32 : ch;
    }
    return -1;
}
/* signs of our texts that the mission's streams may not have (mission 1: Z ; / % + # ~ _ > =): as words or their nearest glyph */
static const char *zn_help_clean(const char *t, char *out, size_t cap) {
    size_t k = 0;
    for (; *t && k + 12 < cap; t++) {
        const char *r = NULL;
        if (zn_help_cell((unsigned char)*t) < 0 || *t == '_' || *t == '=')
            switch (*t) {
            case '%': r = " percent"; break;
            case '+': r = " and "; break;
            case '~': r = "about "; break;
            case '#': r = "no. "; break;
            case '/': r = "-"; break;
            case '_': r = "-"; break;
            case '>': r = ":"; break;
            case '=': r = ":"; break;
            case ';': r = ","; break;
            default: break;
            }
        if (r) { size_t n = strlen(r); memcpy(out + k, r, n); k += n; }
        else out[k++] = *t;
    }
    out[k] = 0;
    return out;
}
/* a text into our stream's one line, wrapped at the panel's width (negative codes break the line, as in the game's files);
   dry (the fit report of ACZ_DEBUG_PAUSE): nothing written, returns the lines it needs (more than ZN_H_LINES: cut) and
   the characters without a glyph in *missing */
static int zn_help_layout(ps2_ctx *ctx, const char *text, int dry, char *missing) {
    int sp = zn_help_cell(' '), x = 0, line = 1, spw = sp >= 0 ? zn_hc[sp].w : 4;
    u32 n = 0;
    int nm = 0;
    static char clean[800];
    const char *t = zn_help_clean(text, clean, sizeof clean);
    while (*t && (dry || line <= ZN_H_LINES)) {
        const char *e = t;
        int w = 0;
        while (*e == ' ') e++;
        int gap = (int)(e - t) * spw;
        t = e;
        while (*e && *e != ' ') {
            int c = zn_help_cell((unsigned char)*e);
            w += c >= 0 ? zn_hc[c].w : spw;
            e++;
        }
        if (x && x + gap + w > ZN_H_WIDTH) {
            if (line == ZN_H_LINES && !dry) break;
            if (!dry) ps2_w16(ZN_H_CODES + 2u * n++, 0xFFFFu);
            line++;
            x = 0;
            gap = 0;
        }
        if (!dry) for (int g = 0; g < gap / (spw ? spw : 1) && sp >= 0 && n < ZN_H_CODEMAX; g++) ps2_w16(ZN_H_CODES + 2u * n++, (u16)sp);
        x += gap + w;
        for (; t < e && n < ZN_H_CODEMAX; t++) {
            int c = zn_help_cell((unsigned char)*t);
            if (c < 0 || zn_hc[c].code != (unsigned char)*t) {
                if (nm < 60 && !memchr(missing, *t, (size_t)nm)) missing[nm++] = *t;
            }
            if (c < 0) c = sp;
            if (c >= 0 && !dry) ps2_w16(ZN_H_CODES + 2u * n++, (u16)c);
        }
    }
    missing[nm] = 0;
    if (dry) return line;
    if (nm) ps2_log("zero-quit: ACZ_ENH_NATIVE: help text without glyphs for \"%s\"", missing);
    if (*t) ps2_log("zero-quit: ACZ_ENH_NATIVE: help text cut at line %d: ...%.40s", ZN_H_LINES, t);
    ps2_w16(ZN_H_LENS, (u16)n);
    ps2_w32(ZN_H_OFFS, 0u);
    u32 vt = ps2_r32(ZN_H_OBJ + 8u);
    zq_call(ctx, ps2_r32(vt + 0x24u), ZN_H_OBJ + 8u, 0, 0, 0);
    return line;
}
static void zn_help_set(ps2_ctx *ctx, const char *t) {
    char missing[64];
    zn_help_layout(ctx, t, 0, missing);
}
/* ACZ_DEBUG_PAUSE: how every item's help fits the panel and this mission's glyphs */
static void zn_help_report(ps2_ctx *ctx) {
    int over = 0, nomiss = 0, maxl = 0, rows = 0;
    char allm[64];
    int na = 0;
    for (int i = 0; i < ps2_enh_count(); i++) {
        const ps2_enh_item *it = ps2_enh_item_at(i);
        char missing[64], t[400];
        if (it->cat >= PS2_ENH_CATS && strcmp(it->key, "throttle_lever") && strcmp(it->key, "lever_neutral")) continue;   /* (on no page) */
        snprintf(t, sizeof t, "%s%s", it->help, it->apply == PS2_ENH_RESTART ? " * Saved: restart the game to apply."
                 : it->apply == PS2_ENH_MISSION ? " Saved for next mission or RETRY." : "");
        int l = zn_help_layout(ctx, t, 1, missing);
        rows++;
        if (l > maxl) maxl = l;
        if (l > ZN_H_LINES) { over++; ps2_log("zero-quit: ACZ_ENH_NATIVE: help of %s needs %d lines", it->label, l); }
        if (missing[0]) nomiss++;
        for (const char *q = missing; *q && na < 60; q++) if (!memchr(allm, *q, (size_t)na)) allm[na++] = *q;
    }
    allm[na] = 0;
    ps2_log("zero-quit: ACZ_ENH_NATIVE: help fit: %d rows, %d longer than %d lines (the longest %d); %d with a stand-in glyph: \"%s\"",
            rows, over, ZN_H_LINES, maxl, nomiss, allm);
    for (int c = 0; c < PS2_ENH_CATS; c++) {           /* the category help lines: one line each (settings review A1) */
        char missing[64];
        ps2_log("zero-quit: ACZ_ENH_NATIVE: %s help: %d line(s): %s", ps2_enh_cat_name(c),
                zn_help_layout(ctx, ps2_enh_cat_help(c), 1, missing), ps2_enh_cat_help(c));
    }
}
static void zn_sound(ps2_ctx *ctx, u32 id) {           /* 0x256250's way: 0 cancel, 1 moved, 2 confirm */
    u32 o = zq_call(ctx, ZN_SND_OBJ, 0, 0, 0, 0);
    ps2_w16(ZN_SCRATCH, 0);
    ps2_w16(ZN_SCRATCH + 2u, (u16)id);
    zq_call(ctx, ZN_SND_PLAY, o, ZN_SCRATCH, ZN_SCRATCH + 2u, 0);
}
static void zn_stream_reset(ps2_ctx *ctx, u32 o) {     /* a text stream's vtable +0x24: its typing starts again */
    if (!zq_ptr(o)) return;
    u32 vt = ps2_r32(o + 8u);
    zq_call(ctx, ps2_r32(vt + 0x24u), o + 8u, 0, 0, 0);
}
/* the value column: right of the category's longest label by the cursor box and the ':', as the game's own at least */
static void zn_column(int cat) {
    if (cat == zn_col_cat) return;
    zn_col_cat = cat;
    s32 x0 = (s32)ps2_r32(zn_hdr_row + 4u);
    int ml = 0, mv = 0;
    for (int i = 0; i < ps2_enh_count(); i++) {
        const ps2_enh_item *it = ps2_enh_item_at(i);
        if (it->cat != cat) continue;
        int w = zn_width(it->label);
        if (w > ml) ml = w;
        for (int k = 0; k < it->n && k < PS2_ENH_CHOICES; k++) {
            w = it->name[k] ? zn_width(it->name[k]) : 0;
            if (w > mv) mv = w;
        }
    }
    zn_col = x0 + ml + 8 + 2 + 10;
    if (zn_col < (s32)ps2_r32(zn_hdr_val + 4u)) zn_col = (s32)ps2_r32(zn_hdr_val + 4u);
    if (zn_col + mv > 454) {
        ps2_log("zero-quit: ACZ_ENH_NATIVE: category %d: longest value %d px from x %d passes the panel; column moved left", cat, mv, zn_col);
        zn_col = 454 - mv;
    }
}
static void zn_open(ps2_ctx *ctx, u32 m) {
    double t0 = ps2_wall_seconds();
    if (!zn_help_begin(ctx, m)) zn_hn = 0;
    if (zq_debug) {
        ps2_log("zero-quit: ACZ_ENH_NATIVE: help stream ready in %.3f ms", (ps2_wall_seconds() - t0) * 1e3);
        if (zn_hn) zn_help_report(ctx);
    }
    zn_fo_item = ps2_r32(m + 0x25Cu);
    ps2_w32(m + 0x25Cu, zn_row_item);
    ps2_w8(m + 0x284u, 1u);
    ps2_w8(zn_row_item + 0x52u, 1u);
    for (int s = 0; s < ZN_S_N; s++) zn_entry(s, zn_hdr_val, "", 0, 0, 0);
    memset(&zn_s, 0, sizeof zn_s);
    zn_s.open = -1;
    zn_col_cat = -2;
    zn_active = 1;
    ps2_quit_enh_open_native();
}
/* the page closed: the tree and the menu as they were, the root's rows type in again (the game's own way back) */
static void zn_close(ps2_ctx *ctx, u32 m) {
    for (u32 r = 0; r < ZN_ROWS; r++) {
        ps2_w8(zn_rows[r] + 0x50u, r == 0u);
        ps2_w8(zn_rows[r] + 0x51u, 0u);
        ps2_w8(zn_rows[r] + 0x52u, 1u);
        for (u32 k = 0; k < ZN_VALS; k++) ps2_w8(zn_vals[r][k] + 0x50u, 0u);
    }
    ps2_w8(zn_row_item + 0x51u, 0u);
    ps2_w8(zn_row_item + 0x52u, ZN_ROWS);
    ps2_w32(m + 0x25Cu, zn_fo_item);
    if (zn_hn) { ps2_w32(m + 0x270u, zn_h_saved); zn_hn = 0; }
    ps2_w8(m + 0x284u, 0u);
    ps2_w8(m + 0x285u, 0u);
    ps2_w32(m + 0x4Cu, ps2_r32(zn_row_item + 0x44u));
    ps2_w32(m + 0x250u, 0u);
    ps2_w32(m + 0x27Cu, 0u);
    zn_stream_reset(ctx, ps2_r32(m + 0x26Cu));
    zn_stream_reset(ctx, ps2_r32(m + 0x270u));
    zn_active = 0;
    zn_sound(ctx, 0u);
    ps2_log("zero-quit: ACZ_ENH_NATIVE: the page closed; the pause menu is back");
}
/* one menu update with the page up, after the game's: the newest snapshot into the tree and the entries */
static void zn_frame(ps2_ctx *ctx, u32 m) {
    if (m != zn_menu) { zn_active = 0; return; }
    if (!ps2_quit_enh_visible()) { zn_close(ctx, m); return; }
    static ps2_enh_snap n;
    if (ps2_quit_enh_snap(&n, zn_s.gen)) {
        double t0 = zq_debug ? ps2_wall_seconds() : 0.0;
        int moved = n.cursor != zn_s.cursor || n.val != zn_s.val || strcmp(n.label[0], zn_s.label[0]) != 0;
        int was = zn_s.kind, valchg = 0;
        if (n.kind == PS2_ENH_SNAP_ITEMS || n.kind == PS2_ENH_SNAP_VALUES) zn_column(n.cat);
        s32 x0 = (s32)ps2_r32(zn_hdr_row + 4u), y0 = (s32)ps2_r32(zn_hdr_row + 8u);
        ps2_w8(zn_row_item + 0x52u, (u8)(n.nrows < 1 ? 1 : n.nrows));
        for (int r = 0; r < ZN_ROWS; r++) {
            int open = r == n.open;
            ps2_w8(zn_rows[r] + 0x50u, r == n.cursor);
            ps2_w8(zn_rows[r] + 0x51u, open);
            ps2_w8(zn_rows[r] + 0x52u, (u8)(open ? n.nvals : 1));
            for (int k = 0; k < ZN_VALS; k++) ps2_w8(zn_vals[r][k] + 0x50u, open && k == n.val);
            if (strcmp(n.value[r], zn_s.value[r]) && !strcmp(n.label[r], zn_s.label[r])) valchg = 1;
            zn_entry(ZN_S_LABEL + r, zn_hdr_row, n.label[r], x0, y0 + 25 * r, 0);
            zn_entry(ZN_S_VAL + r, zn_hdr_val, n.value[r], zn_col, y0 + 25 * r, 0);
        }
        if (n.open >= 0) {                              /* the list drops from its row and stays above the rule */
            s32 ly = y0 + 25 * n.open, lim = y0 + 25 * 7 - 25 * (n.nvals - 1);
            if (ly > lim) ly = lim;
            for (int k = 0; k < n.nvals && k < ZN_VALS; k++) zn_entry(ZN_S_LIST + k, zn_hdr_val, n.list[k], zn_col, ly + 25 * k, 0);
        }
        zn_entry(ZN_S_CRUMB, zn_hdr_crumb, n.crumb, (s32)ps2_r32(zn_hdr_crumb + 4u), (s32)ps2_r32(zn_hdr_crumb + 8u), 1);
        zn_entry(ZN_S_HEAD, zn_hdr_head, n.head, (s32)ps2_r32(zn_hdr_head + 4u), (s32)ps2_r32(zn_hdr_head + 8u), 1);
        if (was != n.kind || zn_s.cat != n.cat) {       /* another level: the game's rows and values type in again */
            ps2_w32(m + 0x250u, 0u);
            ps2_w32(m + 0x278u, 0u);
        }
        if (was != n.kind || moved || strcmp(n.help, zn_s.help)) {
            ps2_w32(m + 0x27Cu, 0u);
            if (zn_hn) zn_help_set(ctx, n.help);
        }
        if (was) {                                      /* the game's sounds: its pad reader sees a neutral pad */
            int deeper = n.kind > was, up = n.kind < was;
            if (deeper) zn_sound(ctx, 2u);
            else if (up) zn_sound(ctx, valchg ? 2u : 0u);
            else if (moved || valchg) zn_sound(ctx, 1u);
        }
        zn_s = n;
        if (zq_debug) {
            static double total;
            static int count;
            double ms = (ps2_wall_seconds() - t0) * 1e3;
            total += ms;
            count++;
            ps2_log("zero-quit: ACZ_ENH_NATIVE: page state %u (kind %d, cat %d, cursor %d, open %d) into the menu in %.3f ms (mean %.3f over %d)",
                    n.gen, n.kind, n.cat, n.cursor, n.open, ms, total / count, count);
        }
    }
    /* every frame: what the update rewrites */
    u32 arr = ps2_r32(m + 0x274u);
    if (zq_ptr(arr)) for (u32 r = 0; r < ZN_ROWS; r++) ps2_w8(arr + r, 0u);
    ps2_w32(m + 0x48u, 0xFFFFFFFFu);
    ps2_w32(m + 0x4Cu, zn_hn ? 9u : 0x7000u);           /* line 0 of our stream, or far past the game's file: no help */
    if (zn_s.kind == PS2_ENH_SNAP_LIST || zn_s.kind == 0) ps2_w8(m + 0x285u, 1u);   /* rows without values or ':' */
}
/* wrap a text into at most 4 lines of the panel's width (measure: px per character of the font in use) */
static int zn_wrap(const char *s, int width, int (*measure)(const char *, int), char lines[4][128]) {
    int nl = 0;
    while (*s && nl < 4) {
        while (*s == ' ') s++;
        int n = 0, last = 0;
        while (s[n] && n < 127) {
            if (s[n] == ' ') last = n;
            if (measure(s, n + 1) > width) break;
            n++;
        }
        if (s[n] && last > 0) n = last;
        memcpy(lines[nl], s, (size_t)n);
        lines[nl][n] = 0;
        nl++;
        s += n;
    }
    if (*s) {                                           /* longer than the panel: cut, and say so once per text */
        static u32 said;
        if (said != zn_s.gen) { said = zn_s.gen; ps2_log("zero-quit: ACZ_ENH_NATIVE: help text cut at the 4th line: ...%.40s", s); }
    }
    return nl;
}
/* wrapping for the ASCII font: its upper case is 6 px wide + the spacing of 2 */
static int zn_measure(const char *s, int n) {
    (void)s;
    return n * 8;
}
/* a call with the stack arguments and a 64-bit t3 of 0x2544D8 (zq_call's rules) */
static u32 zn_font_draw(ps2_ctx *ctx, u32 menu, u32 packet, s32 x, s32 y, u32 colour, u32 str) {
    ps2_reg128 rs[32];
    ps2_fpr fs[32];
    ps2_reg128 hi = ctx->hi, lo = ctx->lo;
    float acc = ctx->acc;
    u32 fcc = ctx->fcc, fcr = ctx->fcr31, sa = ctx->sa, pc = ctx->pc;
    u32 font = ps2_r32(ZQ_FONT_PTR);
    u64 gsreg = ps2_r64(font + 0x280u);
    memcpy(rs, ctx->r, sizeof rs);
    memcpy(fs, ctx->f, sizeof fs);
    u32 sp = (ctx->r[29].uw[0] - 0x80u) & ~15u;
    ps2_w32(sp, colour);
    ps2_w32(sp + 8u, str);
    ctx->r[29].ud[0] = (u64)(s64)(s32)sp;
    ctx->r[31].ud[0] = 0;
    ctx->r[4].ud[0] = (u64)(s64)(s32)menu; ctx->r[5].ud[0] = (u64)(s64)(s32)packet;
    ctx->r[6].ud[0] = 0; ctx->r[7].ud[0] = 0;
    ctx->r[8].ud[0] = (u64)(s64)x; ctx->r[9].ud[0] = (u64)(s64)y;
    ctx->r[10].ud[0] = 2; ctx->r[11].ud[0] = gsreg;
    ps2_dispatch(ctx, ZN_FONT_DRAW);
    u32 v0 = ctx->r[2].uw[0];
    memcpy(ctx->r, rs, sizeof rs);
    memcpy(ctx->f, fs, sizeof fs);
    ctx->hi = hi; ctx->lo = lo; ctx->acc = acc; ctx->fcc = fcc; ctx->fcr31 = fcr; ctx->sa = sa; ctx->pc = pc;
    return v0;
}
/* after 0x2583E8 with the page up: the help text (ACZ_ENH_NATIVE_HELP) */
static int zn_help_after(ps2_ctx *ctx, void *u) {
    (void)u;
    if (!zn_active || zn_hn || !zn_s.help[0]) return 0;
    int ok;
    u32 m = ps2_hook_entry_arg(0, &ok);
    if (!ok || m != zn_menu) return 0;
    char lines[4][128];
    int nl = zn_wrap(zn_s.help, ZN_H_WIDTH, zn_measure, lines);
    u32 font = ps2_r32(ZQ_FONT_PTR), pkt = ctx->r[2].uw[0];
    s32 x = (s32)ps2_r32(zn_hdr_row + 4u);
    for (int l = 0; l < nl; l++) {
        u32 a = ZN_STRING, k = 0;
        for (; lines[l][k]; k++) {                      /* (this font has the upper case only: h10, digits, 11 signs) */
            char c = lines[l][k];
            ps2_w8(a + k, (u8)(c >= 'a' && c <= 'z' ? c - 32 : c));
        }
        ps2_w8(a + k, 0);
        pkt = zn_font_draw(ctx, m, pkt, x, 309 + 20 * l, font + 0x23Cu, a);
    }
    ctx->r[2].ud[0] = (u64)(s64)(s32)pkt;
    return 0;
}
/* the menu goes away with the page up (not reachable by the pad): the game must free its own stream */
static int zn_free_enter(ps2_ctx *ctx, void *u) {
    (void)u;
    u32 m = ctx->r[4].uw[0];
    if (m != zn_menu) return 0;
    if (zn_active) {
        if (zn_hn) ps2_w32(m + 0x270u, zn_h_saved);
        ps2_w32(m + 0x25Cu, zn_fo_item);
        zn_active = 0;
        zn_hn = 0;
    }
    zn_ready = 0;
    zn_menu = 0;
    return 0;
}
/* every text stream the game sets up (objectives, help, radio ...): where the help font's glyphs are */
static int zn_settext_enter(ps2_ctx *ctx, void *u) {
    (void)u;
    u32 s = ctx->r[4].uw[0];
    for (int i = 0; i < zn_nstreams; i++) if (zn_streams[i] == s) return 0;
    zn_streams[zn_stream_next] = s;
    zn_stream_next = (zn_stream_next + 1) % ZN_MAX_STREAMS;
    if (zn_nstreams < ZN_MAX_STREAMS) zn_nstreams++;
    return 0;
}
static void zn_install(void) {
    if (zq_debug == 3 || zn_on) ps2_hook_before(ZN_SET_TEXT, zn_settext_enter, NULL, 60, "zero-quit");
    if (!zn_on) return;
    if (ps2_pt[ZN_PAGE >> PS2_PAGE_BITS]) { ps2_log("zero-quit: guest page %08X in use; ACZ_ENH_NATIVE off", ZN_PAGE); zn_on = 0; return; }
    ps2_mem_map(ZN_PAGE, sizeof zn_page, zn_page);
    {   const char *e = getenv("ACZ_ENH_NATIVE_HELP");
        zn_hascii = e && !strcmp(e, "ascii");
    }
    if (ps2_hook_after(ZN_PAUSE_HELP, zn_help_after, NULL, 60, "zero-quit") < 0
        || ps2_hook_before(ZN_MENU_FREE, zn_free_enter, NULL, 60, "zero-quit") < 0) { zn_on = 0; return; }
    ps2_log("zero-quit: ACZ_ENH_NATIVE=1: the pause menu's ENHANCEMENTS page is drawn by the game");
}

/* after a pause tree builder: the extra rows -- ENHANCEMENTS (a leaf: the page), TOUCH CONTROLS (a leaf: the Touch
   card, bug #239) and QUIT GAME (with its YES / CANCEL) -- get their ids and texts. Rows: the game's n0, then
   ENHANCEMENTS, then TOUCH CONTROLS, then QUIT GAME. */
static int zq_build_leave(ps2_ctx *ctx, void *u) {
    (void)u;
    int ok;
    u32 menu = ps2_hook_entry_arg(0, &ok);
    if (!ok || !zq_ptr(menu)) return 0;
    u32 root = ps2_r32(menu + 0x40u), n = zq_ptr(root) ? ps2_r8(root + 0x52u) : 0u, extra = (u32)zq_extra;
    const char *why = NULL;
    u32 font = ps2_r32(ZQ_FONT_PTR);
    int bumped = root == zq_root_bumped && n >= 2u + extra;
    if (!bumped) why = "the root was not enlarged";
    else if (zq_debug == 2) why = "test of the fallback (ACZ_DEBUG_PAUSE=2)";
    zq_root_bumped = 0;
    u32 n0 = bumped ? n - extra : n;
    u32 qm = why ? 0 : zq_child_with_id(root, 5u), rtg = why ? 0 : zq_child_with_id(root, 0u);
    u32 prev = why ? 0 : ps2_r32(root + 4u + 4u * (n0 - 1u));
    for (u32 k = n0; !why && k < n; k++) {
        u32 r = ps2_r32(root + 4u + 4u * k);
        if (!zq_ptr(r) || ps2_r32(r + 0x44u) != 0u || ps2_r8(r + 0x52u) != 0u) why = "the new rows are not blank";
    }
    if (!why && (!zq_ptr(qm) || !zq_ptr(rtg) || !zq_ptr(prev) || ps2_r8(qm + 0x52u) != 2u))
        why = "QUIT MISSION / RETURN TO GAME not found";
    if (!why && !zq_ptr(font)) why = "no font";
    u32 qyes = 0, qcan = 0, e_qm = 0, e_rtg = 0, e_prev = 0, e_yes = 0, e_can = 0;
    if (!why) {
        u32 d = ps2_r8(qm + 0x53u);
        qyes = ps2_r32(qm + 4u + 4u * (d ? 0u : 1u));
        qcan = ps2_r32(qm + 4u + 4u * (d < 2u ? d : 1u));
        if (!zq_ptr(qyes) || !zq_ptr(qcan) || qyes == qcan) why = "QUIT MISSION's confirmation not found";
    }
    if (!why) {
        e_qm = zq_call(ctx, ZQ_TEXT, font, ps2_r32(qm + 0x48u), ps2_r32(qm + 0x4Cu), 0);
        e_rtg = zq_call(ctx, ZQ_TEXT, font, ps2_r32(rtg + 0x48u), ps2_r32(rtg + 0x4Cu), 0);
        e_prev = zq_call(ctx, ZQ_TEXT, font, ps2_r32(prev + 0x48u), ps2_r32(prev + 0x4Cu), 0);
        e_yes = zq_call(ctx, ZQ_TEXT, font, ps2_r32(qyes + 0x48u), ps2_r32(qyes + 0x4Cu), 0);
        e_can = zq_call(ctx, ZQ_TEXT, font, ps2_r32(qcan + 0x48u), ps2_r32(qcan + 0x4Cu), 0);
        if (!zq_ptr(e_qm) || !zq_ptr(e_rtg) || !zq_ptr(e_prev) || !zq_ptr(e_yes) || !zq_ptr(e_can)) why = "texts not found";
        /* "QUIT MISSION" (12 glyphs, the 5th a space) and "RETURN TO GAME" (14, the 10th a space) */
        else if (ps2_r32(e_qm) != 12u || ps2_r32(e_rtg) != 14u || !(ps2_r8(e_qm + 0x18u + 4u * 4u + 3u) & 0x80u)
                 || !(ps2_r8(e_rtg + 0x18u + 4u * 9u + 3u) & 0x80u) || ps2_r32(e_yes) > 16u || ps2_r32(e_can) > 16u)
            why = "unexpected texts (another language?)";
    }
    u32 used[2] = { 0, 0 }, pid = 0, yid = 0, eid = 0, tid = 0;
    if (!why) {
        zq_ids(root, used, 0);
        for (pid = 31u; pid < 64u && (used[pid >> 5] >> (pid & 31u) & 1u); pid++) ;
        for (yid = pid + 1u; yid < 64u && (used[yid >> 5] >> (yid & 31u) & 1u); yid++) ;
        for (eid = yid + 1u; eid < 64u && (used[eid >> 5] >> (eid & 31u) & 1u); eid++) ;
        for (tid = eid + 1u; tid < 64u && (used[tid >> 5] >> (tid & 31u) & 1u); tid++) ;
        if (eid >= 64u || (zq_tch_on && tid >= 64u)) why = "no free item ids";
    }
    if (why) {
        if (bumped) zq_undo(ctx, root, n, extra);
        ps2_log("zero-quit: pause menu %08X left as the game built it (%s)", menu, why);
        return 0;
    }
    int enh = zq_enh_on, tch = zq_tch_on, quit = zq_menu_on;
    if ((enh || tch) && !(zq_map_matches(e_qm, "QUIT MISSION") && zq_map_matches(e_rtg, "RETURN TO GAME"))) {
        ps2_log("zero-quit: the pause font's glyphs are not where the map expects them; no ENHANCEMENTS / TOUCH CONTROLS row");
        u32 k = (u32)(enh + tch);                       /* drop the composed rows: QUIT GAME (last) moves to n0 */
        if (quit) {
            u32 er = ps2_r32(root + 4u + 4u * n0), last = ps2_r32(root + 4u + 4u * (n - 1u));
            ps2_w32(root + 4u + 4u * n0, last);
            ps2_w32(root + 4u + 4u * (n - 1u), er);
        }
        zq_undo(ctx, root, n, k);
        n -= k;
        enh = tch = 0;
    }
    u32 row_y = ps2_r32(e_prev + 8u) + 25u, tch_y = row_y + (enh ? 25u : 0u), quit_y = tch_y + (tch ? 25u : 0u);
    u32 enh_row = enh ? ps2_r32(root + 4u + 4u * n0) : 0, row = quit ? ps2_r32(root + 4u + 4u * (n - 1u)) : 0;
    u32 tch_row = tch ? ps2_r32(root + 4u + 4u * (n0 + (u32)enh)) : 0;
    zq_txt_group = ps2_r32(qm + 0x48u);
    zq_yes_group = ps2_r32(qyes + 0x48u);
    zq_e_quit = zq_e_yes = zq_e_can = zq_e_enh = zq_e_enh2 = zq_e_tch = 0;
    u32 a = ZQ_PAGE;
    if (quit) {
        /* QUIT GAME: QUIT MISSION's "QUIT " and RETURN TO GAME's "GAME"; YES / CANCEL as QUIT MISSION's */
        u32 e_row = a;
        for (u32 k = 0; k < 0x18u; k++) ps2_w8(e_row + k, ps2_r8(e_qm + k));
        ps2_w32(e_row, 9u);
        ps2_w32(e_row + 8u, quit_y);
        u32 wsum = 0;
        for (u32 j = 0; j < 9u; j++) {
            u32 src = j < 5u ? e_qm + 0x18u + 4u * j : e_rtg + 0x18u + 4u * (10u + (j - 5u));
            ps2_w32(e_row + 0x18u + 4u * j, ps2_r32(src));
            wsum += ps2_r8(src + 2u);
        }
        ps2_w16(e_row + 0x14u, (u16)wsum);
        a += (zq_entry_size(9u) + 15u) & ~15u;
        u32 e_y2 = a, ny = ps2_r32(e_yes);
        for (u32 k = 0; k < zq_entry_size(ny); k++) ps2_w8(e_y2 + k, ps2_r8(e_yes + k));
        ps2_w32(e_y2 + 8u, quit_y);
        a += (zq_entry_size(ny) + 15u) & ~15u;
        u32 e_c2 = a, nc = ps2_r32(e_can);
        for (u32 k = 0; k < zq_entry_size(nc); k++) ps2_w8(e_c2 + k, ps2_r8(e_can + k));
        ps2_w32(e_c2 + 8u, quit_y + 25u);
        a += (zq_entry_size(nc) + 15u) & ~15u;
        zq_e_quit = e_row; zq_e_yes = e_y2; zq_e_can = e_c2;
    }
    if (enh) {
        zq_e_enh = a;
        a = zq_compose(a, e_qm, "ENHANCEMENTS", row_y);
        zq_e_enh2 = a;
        a = zq_compose(a, e_qm, "ENHANCEMENTS (RESTART)", row_y);
    }
    if (tch) {
        zq_e_tch = a;
        a = zq_compose(a, e_qm, "TOUCH CONTROLS", tch_y);
    }
    zq_txt_ok = 1;
    if (enh) {                                          /* ENHANCEMENTS: a leaf; choosing it opens the page */
        ps2_w32(enh_row + 0x44u, eid);
        ps2_w32(enh_row + 0x48u, zq_txt_group);
        ps2_w32(enh_row + 0x4Cu, ZQ_ENH_IDX);
        zq_enh_id = eid;
        ps2_log("zero-quit: pause menu %08X: ENHANCEMENTS added as row %u (id %u, y %u)", menu, n0, eid, row_y);
        zn_build(ctx, menu, enh_row);                   /* ACZ_ENH_NATIVE: it gets rows of its own */
    }
    if (tch) {                                          /* TOUCH CONTROLS: a leaf; choosing it shows the Touch card */
        ps2_w32(tch_row + 0x44u, tid);
        ps2_w32(tch_row + 0x48u, zq_txt_group);
        ps2_w32(tch_row + 0x4Cu, ZQ_TCH_IDX);
        zq_tch_id = tid;
        ps2_log("zero-quit: pause menu %08X: TOUCH CONTROLS added as row %u (id %u, y %u)", menu, n0 + (u32)enh, tid, tch_y);
    }
    if (quit) {                                         /* QUIT GAME and its confirmation */
        ps2_w32(row + 0x44u, pid);
        ps2_w32(row + 0x48u, zq_txt_group);
        ps2_w32(row + 0x4Cu, ZQ_TXT_IDX);
        zq_call(ctx, ZQ_ALLOC_CHILDREN, row, 2u, 0, 0);
        u32 c0 = ps2_r32(row + 4u), c1 = ps2_r32(row + 8u);
        if (ps2_r8(row + 0x52u) != 2u || !zq_ptr(c0) || !zq_ptr(c1)) {
            ps2_log("zero-quit: QUIT GAME row %08X: its confirmation could not be made; the row is given back", row);
            zq_undo(ctx, root, n, 1u);
            return 0;
        }
        ps2_w8(row + 0x53u, 1u);                        /* the cursor starts on CANCEL, as in the game's own */
        ps2_w32(c0 + 0x44u, yid);
        ps2_w32(c0 + 0x48u, zq_yes_group);
        ps2_w32(c0 + 0x4Cu, ZQ_TXT_IDX + 1u);
        ps2_w32(c1 + 0x44u, ps2_r32(qcan + 0x44u));     /* the game's CANCEL id: its own "go back" */
        ps2_w32(c1 + 0x48u, zq_yes_group);
        ps2_w32(c1 + 0x4Cu, ZQ_TXT_IDX + 2u);
        zq_yes_id = yid;
        ps2_log("zero-quit: pause menu %08X: QUIT GAME added as row %u (id %u, YES %u, CANCEL %u, y %u)", menu, n - 1u,
                pid, yid, ps2_r32(c1 + 0x44u), quit_y);
    }
    if (zq_debug) zq_dump_item(root, 0);
    return 0;
}

/* replacement of 0x2402F8 for the new rows' texts; everything else goes to the game's lookup */
static int zq_text(ps2_ctx *ctx, void *u) {
    (void)u;
    if (!zq_txt_ok) return 0;
    u32 g = ctx->r[5].uw[0], i = ctx->r[6].uw[0], e = 0;
    if (zn_ready && (e = zn_text(g, i)) != 0) { ctx->r[2].ud[0] = (u64)e; return 1; }
    if (i < ZQ_TXT_IDX || i > ZQ_TCH_IDX) return 0;
    if (i == ZQ_TXT_IDX && g == zq_txt_group) e = zq_e_quit;
    else if (i == ZQ_TXT_IDX + 1u && g == zq_yes_group) e = zq_e_yes;
    else if (i == ZQ_TXT_IDX + 2u && g == zq_yes_group) e = zq_e_can;
    else if (i == ZQ_ENH_IDX && g == zq_txt_group) e = ps2_enh_pending_count() > 0 ? zq_e_enh2 : zq_e_enh;
    else if (i == ZQ_TCH_IDX && g == zq_txt_group) e = zq_e_tch;
    if (!e) return 0;
    ctx->r[2].ud[0] = (u64)e;
    return 1;
}

/* the pause menu's draw while the ENHANCEMENTS page is up: nothing (the packet pointer comes back unchanged, as when
   the menu is hidden), so only the page shows over the paused game */
static int zq_draw_replace(ps2_ctx *ctx, void *u) {
    (void)u;
    if (!ps2_quit_enh_visible()) return 0;
    if (zn_active && !ps2_quit_touch_card_visible()) return 0;   /* ACZ_ENH_NATIVE: the game draws the page */
    ctx->r[2].ud[0] = ctx->r[5].ud[0];
    return 1;
}

/* ---- Model LOD: ACZ_LOD_BIAS (bug #105, 2026-10-24) ----
   LodModel::SelectLod 0x131118 (vtable slot 0x74 of LodModel and FriendModel, called by their methods 0x12F880,
   0x131270, 0x131440; their callers pass f12 = the camera-to-object distance (0x235360: |obj+0x70 - cam+0x30|) and
   f13 = the camera's focal cam+0x08): focal > 4095 (a zoom) -> LOD 0; else k = distance x [0x3F7800] / focal and the
   LOD is the first i of the model's five thresholds (*(this+0x14)) with k < t[i]; k above all five -> -1, not drawn
   (0x131440 skips the draw; render only). The constant (512.0, only reader 0x131140) scales the DISTANCE, so a bias N
   is written as 512 / N: every LOD switch and the draw cutoff N times farther (N = 16 = the PC port's thresholds
   x16). ACZ_LOD_BIAS_MAP=0 restores the old 512 x N (until 2026-10-24), which brought them N times closer: HIGH (2)
   halved the aircraft's detail and draw distances. */
#define Z_LOD_CONST 0x003F7800u
static int z_lod_map = -1;                     /* ACZ_LOD_BIAS_MAP (1: 512 / N) */
static float z_lod_bias = 1.0f;                /* the bias in effect (start-up or ENHANCEMENTS) */
static float z_lod_word(float lb) {
    if (z_lod_map < 0) z_lod_map = env_float("ACZ_LOD_BIAS_MAP", 1.0f) != 0.0f;
    return z_lod_map ? 512.0f / lb : 512.0f * lb;
}

/* ACZ_DEBUG_LOD=1 (probe): each LOD table (one per model) with its thresholds once (and the switch distances at the
   constant and focal in effect), then every 5 s per table the calls per LOD (cut = -1, not drawn) with the distance
   range of each, and the nearest objects (LodModel, table, distance, LOD); metres at 6.656 world units per metre.
   ACZ_DEBUG_LOD=2 also logs every call nearer than 400 m (field, object, distance, LOD: labels for eye dumps).
   ACZ_LOD_BIAS_AB=s (test): the constant alternates in blocks of s x 60 GS fields between the configured bias (even
   blocks) and NORMAL x1 (odd blocks; `lod A/B:` lines for blocks of 0.5 s or more), for an in-run GPU / host A/B by
   field with PS2_GPU_TRACE and PS2_REC_PROF_CSV (a private script TRACE s --rec REC). No exact image
   A/B: the game picks the LOD before the draw list exists, and with one-field blocks (s = 0.0167) and
   PS2_XR_DUMP_PAIR=2 which neighbouring frame got which constant depends on the run's EE-to-render latency; labelled
   images come from static runs with ACZ_DEBUG_LOD=2 (the scene is deterministic enough). */
#define ZL_TABS 64
#define ZL_OBJS 256
#define ZL_UPM 6.656f
typedef struct { u32 tab, ra, ra2; float t[5]; u32 n[6]; float dmin[6], dmax[6]; } zl_tab_t;
static zl_tab_t zl_tab[ZL_TABS];
static int zl_ntab;
static struct { u32 obj; int tab, lod; float d; } zl_obj[ZL_OBJS];
static int zl_nobj;
static int zl_dbg, zl_ab;                     /* ACZ_DEBUG_LOD; ACZ_LOD_BIAS_AB in GS fields */
static float zl_d, zl_f, zl_fmin, zl_fmax;
static u32 zl_this, zl_ra, zl_ra2, zl_calls, zl_zoom, zl_mism;
static double zl_t0;

static void zl_report(void) {
    extern u32 ps2_gs_frame_count;
    static const char *nm[6] = { "L0", "L1", "L2", "L3", "L4", "cut" };
    ps2_log("lod-probe: field %u: %u calls (zoom %u, mismatch %u), focal %.1f..%.1f, constant %.1f (bias x%.2f, map %d)",
            ps2_gs_frame_count, zl_calls, zl_zoom, zl_mism, zl_fmin, zl_fmax, f_of(ps2_r32(Z_LOD_CONST)), z_lod_bias,
            z_lod_map);
    for (int i = 0; i < zl_ntab; i++) {
        zl_tab_t *t = &zl_tab[i];
        char line[512]; int o = 0; u32 tot = 0;
        for (int l = 0; l < 6; l++) tot += t->n[l];
        if (!tot) continue;
        for (int l = 0; l < 6 && o < (int)sizeof line - 48; l++)
            if (t->n[l]) o += snprintf(line + o, sizeof line - (size_t)o, " | %s %u %.0f-%.0f m", nm[l], t->n[l],
                                       t->dmin[l] / ZL_UPM, t->dmax[l] / ZL_UPM);
        ps2_log("lod-probe:  t%d n %u%s", i, tot, line);
        memset(t->n, 0, sizeof t->n);
    }
    int ord[ZL_OBJS], n = zl_nobj;               /* the nearest objects of the window (their last call) */
    for (int i = 0; i < n; i++) {
        int j = i;
        while (j > 0 && zl_obj[ord[j - 1]].d > zl_obj[i].d) { ord[j] = ord[j - 1]; j--; }
        ord[j] = i;
    }
    char line[640]; int o = 0;
    for (int i = 0; i < n && i < 16 && o < (int)sizeof line - 40; i++)
        o += snprintf(line + o, sizeof line - (size_t)o, " %08X/t%d %.0f m L%d", zl_obj[ord[i]].obj, zl_obj[ord[i]].tab,
                      zl_obj[ord[i]].d / ZL_UPM, zl_obj[ord[i]].lod);
    if (n) ps2_log("lod-probe: nearest of %d:%s", n, line);
    zl_nobj = 0; zl_calls = zl_zoom = zl_mism = 0; zl_fmin = zl_fmax = 0.0f;
}

static int zl_enter(ps2_ctx *ctx, void *u) {
    (void)u;
    if (zl_ab) {
        extern u32 ps2_gs_frame_count;
        static int last = -1;
        int ph = (int)((ps2_gs_frame_count / (u32)zl_ab) & 1u);
        u32 w = w_of(ph ? 512.0f : z_lod_word(z_lod_bias));
        if (ps2_r32(Z_LOD_CONST) != w) ps2_w32(Z_LOD_CONST, w);
        if (ph != last) {
            if (zl_ab >= 30 || last < 0)
                ps2_log("lod A/B: %s (field %u, constant %.1f)", ph ? "NORMAL x1" : "configured bias", ps2_gs_frame_count,
                        f_of(w));
            last = ph;
        }
    }
    if (zl_dbg) {
        u32 sp = ctx->r[29].uw[0];
        zl_d = ctx->f[12].f; zl_f = ctx->f[13].f; zl_this = ctx->r[4].uw[0]; zl_ra = ctx->r[31].uw[0];
        zl_ra2 = zl_ra == 0x0012F8ACu ? ps2_r32(sp + 0x18u) : zl_ra == 0x0013128Cu ? ps2_r32(sp + 0x8u)
               : zl_ra == 0x00131470u ? ps2_r32(sp + 0x20u) : 0u;     /* the method's caller (from its frame) */
    }
    return 0;
}

static int zl_leave(ps2_ctx *ctx, void *u) {
    (void)u;
    int lod = (int)(s32)ctx->r[2].uw[0];
    double now = ps2_wall_seconds();
    zl_calls++;
    if (zl_f > 4095.0f) zl_zoom++;
    else {
        u32 tab = ps2_r32(zl_this + 0x14u);
        float c = f_of(ps2_r32(Z_LOD_CONST));
        int ti = 0;
        while (ti < zl_ntab && zl_tab[ti].tab != tab) ti++;
        if (ti == zl_ntab && zl_ntab < ZL_TABS) {
            zl_tab_t *t = &zl_tab[zl_ntab++];
            memset(t, 0, sizeof *t);
            t->tab = tab; t->ra = zl_ra; t->ra2 = zl_ra2;
            for (int i = 0; i < 5; i++) t->t[i] = f_of(ps2_r32(tab + 4u * (u32)i));
            float s = zl_f / c / ZL_UPM;
            ps2_log("lod-probe: table t%d = %08X (from %08X <- %08X): thresholds %.0f %.0f %.0f %.0f %.0f units; "
                    "switches at %.0f %.0f %.0f %.0f %.0f m now (focal %.1f, constant %.1f)", ti, tab, zl_ra, zl_ra2,
                    t->t[0], t->t[1], t->t[2], t->t[3], t->t[4], t->t[0] * s, t->t[1] * s, t->t[2] * s, t->t[3] * s,
                    t->t[4] * s, zl_f, c);
            {   /* (pt_fx, tanks: the five LOD models, *(LodModel + 0x2C + 4 i) as 0x131180 reads them, and a few words of
                   each -- is the LOD 0 of a table that never selects it (threshold 0) a model of its own?) */
                char ml[400]; int mo = 0;
                for (u32 i = 0; i < 5u && mo < (int)sizeof ml - 60; i++) {
                    u32 mp = ps2_r32(zl_this + 0x2Cu + 4u * i);
                    int okp = mp >= 0x100000u && mp < 0x2000000u;
                    mo += snprintf(ml + mo, sizeof ml - (size_t)mo, " L%u %08X", i, mp);
                    if (okp) mo += snprintf(ml + mo, sizeof ml - (size_t)mo, " [%08X %08X %08X %08X]", ps2_r32(mp),
                                            ps2_r32(mp + 4u), ps2_r32(mp + 8u), ps2_r32(mp + 0x44u));
                }
                ps2_log("lod-probe: table t%d models:%s", ti, ml);
            }
        }
        if (ti < zl_ntab) {
            zl_tab_t *t = &zl_tab[ti];
            float k = zl_d * (c / zl_f);
            int mine = -1;
            for (int i = 0; i < 5; i++) if (k < t->t[i]) { mine = i; break; }
            zl_mism += mine != lod;
            if (zl_dbg >= 2 && zl_d < 400.0f * ZL_UPM) {   /* ACZ_DEBUG_LOD=2: every call nearer than 400 m */
                extern u32 ps2_gs_frame_count;
                ps2_log("lod-probe: field %u obj %08X t%d %.1f m L%d (constant %.1f, focal %.1f)", ps2_gs_frame_count,
                        zl_this, ti, zl_d / ZL_UPM, lod, c, zl_f);
            }
            int li = lod >= 0 && lod < 5 ? lod : 5;
            if (!t->n[li] || zl_d < t->dmin[li]) t->dmin[li] = zl_d;
            if (!t->n[li] || zl_d > t->dmax[li]) t->dmax[li] = zl_d;
            t->n[li]++;
        }
        if (!zl_fmin || zl_f < zl_fmin) zl_fmin = zl_f;
        if (zl_f > zl_fmax) zl_fmax = zl_f;
        int oi = 0;
        while (oi < zl_nobj && zl_obj[oi].obj != zl_this) oi++;
        if (oi == zl_nobj && zl_nobj < ZL_OBJS) zl_nobj++;
        if (oi < zl_nobj) { zl_obj[oi].obj = zl_this; zl_obj[oi].tab = ti; zl_obj[oi].lod = lod; zl_obj[oi].d = zl_d; }
    }
    if (!zl_t0) zl_t0 = now;
    if (now - zl_t0 >= 5.0) { zl_report(); zl_t0 = now; }
    return 0;
}

static void zl_install(void) {
    float ab = env_float("ACZ_LOD_BIAS_AB", 0.0f);
    zl_dbg = (int)env_float("ACZ_DEBUG_LOD", 0.0f);
    zl_ab = ab > 0.0f ? (int)(ab * 60.0f + 0.5f) : 0;
    if (ab > 0.0f && zl_ab < 1) zl_ab = 1;
    if (!zl_dbg && !zl_ab) return;
    if (ps2_r32(0x00131118u) != 0x3C0F003Fu) {
        ps2_log("zero-vr: ACZ_DEBUG_LOD / ACZ_LOD_BIAS_AB: SelectLod 0x131118 not as expected (%08X); off", ps2_r32(0x00131118u));
        zl_dbg = zl_ab = 0;
        return;
    }
    if (ps2_hook_before(0x00131118u, zl_enter, NULL, 60, "zero-lod") < 0
        || (zl_dbg && ps2_hook_after(0x00131118u, zl_leave, NULL, 60, "zero-lod") < 0)) {
        zl_dbg = zl_ab = 0;
        return;
    }
    if (zl_ab && z_lod_bias == 1.0f) ps2_log("zero-vr: ACZ_LOD_BIAS_AB with the bias at x1: both halves are the same");
    ps2_log("zero-vr: LOD %s%s%s (SelectLod 0x131118)", zl_dbg ? "probe (ACZ_DEBUG_LOD)" : "", zl_dbg && zl_ab ? " and " : "",
            zl_ab ? "in-run A/B of the bias against x1 (ACZ_LOD_BIAS_AB)" : "");
    if (zl_ab) ps2_log("zero-vr: ACZ_LOD_BIAS_AB: blocks of %d GS fields, x%.2f (constant %.1f) in even blocks, x1 in odd",
                       zl_ab, z_lod_bias, z_lod_word(z_lod_bias));
}

/* ENHANCEMENTS: the LOD bias and the terrain detail ring at run time (the constants of the start-up block above;
   SelectLod and the terrain read them every frame). Returns 0 when written. */
static int zq_detail_set(float lb, float td) {
    if (!zero_vr_is_zero() || !(lb > 0.05f && lb <= 32.0f) || !(td > 0.25f && td <= 4.0f)) return -1;
    float lod = f_of(ps2_r32(Z_LOD_CONST));
    u32 a = ps2_r32(0x003F8108u), b = ps2_r32(0x003F810Cu);
    float fa = f_of(a), fb = f_of(b);
    int isf = fa >= 8192.0f && fa <= 131072.0f && fabsf(fa * 0.75f - fb) < 1.0f;
    int isi = a >= 8192u && a <= 131072u && (a * 3u) / 4u == b;
    if (!(lod >= 8.0f && lod <= 16384.0f) || (!isf && !isi)) {   /* 512 / 32 .. 512 x 32 */
        ps2_log("zero-vr: LOD / terrain detail words not as expected (%08X %08X %08X); not changed", ps2_r32(Z_LOD_CONST), a, b);
        return -1;
    }
    ps2_w32(Z_LOD_CONST, w_of(z_lod_word(lb)));
    z_lod_bias = lb;
    if (isf) { ps2_w32(0x003F8108u, w_of(32768.0f * td)); ps2_w32(0x003F810Cu, w_of(24576.0f * td)); }
    else { ps2_w32(0x003F8108u, (u32)(32768.0f * td)); ps2_w32(0x003F810Cu, (u32)(24576.0f * td)); }
    ps2_log("zero-vr: LOD bias x%.2f (constant %.1f%s), terrain detail x%.2f (ENHANCEMENTS, now)", lb, z_lod_word(lb),
            z_lod_map ? "" : ", old mapping", td);
    return 0;
}

/* QUIT MISSION's YES in this pause menu (the row with id 5 and its confirmation; the child that is not the default
   CANCEL), for the controller prompt's "Main menu"; also with ACZ_QUIT_MENU=0 or when the extra row was given back. */
static void zq_find_qm_yes(u32 m) {
    zq_qm_menu = m;
    zq_qm_yes_id = 0xFFFFFFFFu;
    u32 root = ps2_r32(m + 0x40u), qm = zq_ptr(root) ? zq_child_with_id(root, 5u) : 0;
    if (!qm || ps2_r8(qm + 0x52u) != 2u) return;
    u32 yes = ps2_r32(qm + 4u + 4u * (ps2_r8(qm + 0x53u) ? 0u : 1u));
    if (zq_ptr(yes)) zq_qm_yes_id = ps2_r32(yes + 0x44u);
}
/* ACZ_DEBUG_PAUSE=3: every text entry of the pause font, once (the font object is a container of groups, each a
   container of entries: u32 count, u32 offsets from the container) */
static void zq_font_dump(void) {
    u32 font = ps2_r32(ZQ_FONT_PTR);
    if (!zq_ptr(font)) return;
    u32 ng = ps2_r32(font), base = ps2_r32(font + 4u), offs = ps2_r32(font + 8u);
    ps2_log("zq-font: font %08X groups %u base %08X offs %08X", font, ng, base, offs);
    for (u32 g = 0; g < ng && g < 64u; g++) {
        u32 go = ps2_r32(offs + 4u * g);
        if (!go) continue;
        u32 grp = base + go, ne = ps2_r32(grp);
        ps2_log("zq-font: group %u at %08X entries %u", g, grp, ne);
        for (u32 i = 0; i < ne && i < 200u; i++) {
            u32 eo = ps2_r32(grp + 4u + 4u * i);
            if (!eo) continue;
            u32 e = grp + eo, n = ps2_r32(e);
            char line[2048];
            int k = snprintf(line, sizeof line, "zq-font: g%u i%u n%u x%d y%d h%d sp%d yo%d w%u fl%02X :", g, i, n,
                             (int)ps2_r32(e + 4u), (int)ps2_r32(e + 8u), (int)(s16)ps2_r16(e + 0xCu),
                             (int)(s16)ps2_r16(e + 0xEu), (int)(s16)ps2_r16(e + 0x10u), ps2_r16(e + 0x14u), ps2_r8(e + 0x16u));
            for (u32 j = 0; j < n && j < 120u && k < (int)sizeof line - 24; j++)
                k += snprintf(line + k, sizeof line - (size_t)k, " %u,%u,%u,%02X", ps2_r8(e + 0x18u + 4u * j),
                              ps2_r8(e + 0x19u + 4u * j), ps2_r8(e + 0x1Au + 4u * j), ps2_r8(e + 0x1Bu + 4u * j));
            ps2_log("%s", line);
        }
    }
}
/* ACZ_DEBUG_PAUSE=3: the text streams of the help under the rule (objectives +0x26C, FLIGHT OPTIONS help +0x270), each
   as out/pause_stream_<name>.bin (the whole resource, parsed by 0x107C90), and the ASCII font records of the ':' */
static void zq_stream_dump(const char *nm, u32 obj) {
    if (!zq_ptr(obj)) return;
    u32 s = obj + 8u, cells = ps2_r32(s + 4u), clut = ps2_r32(s + 8u), pix = ps2_r32(s + 0xCu);
    u32 nl = ps2_r32(s + 0x10u), nc = ps2_r16(s + 0x14u), tex = ps2_r16(s + 0x16u), sz = ps2_r16(s + 0x18u), lh = ps2_r16(s + 0x1Eu);
    ps2_log("zq-font: stream %s obj %08X cells %08X clut %08X pixels %08X lines %u cell count %u tex %04X size %u line %u",
            nm, obj, cells, clut, pix, nl, nc, tex, sz, lh);
    if (!zq_ptr(cells) || !zq_ptr(pix) || nc > 4096u || sz > 64u) return;
    u32 start = cells - 0x10u, len = pix + nc * sz * sz / 2u - start;
    char path[96];
    snprintf(path, sizeof path, "out/pause_stream_%s.bin", nm);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    for (u32 i = 0; i < len; i++) fputc(ps2_r8(start + i), f);
    fclose(f);
}
static void zq_font_records_dump(void) {
    u32 font = ps2_r32(ZQ_FONT_PTR);
    if (!zq_ptr(font)) return;
    for (u32 sel = 0; sel < 4u; sel++) {
        u32 rec = ps2_r32(font + 0x264u + 4u * sel);
        ps2_log("zq-font: ASCII font %u record %08X", sel, rec);
        if (!zq_ptr(rec)) continue;
        char path[96];
        snprintf(path, sizeof path, "out/pause_ascii_%u.bin", sel);
        FILE *f = fopen(path, "wb");
        if (!f) continue;
        for (u32 i = 0; i < 0x400u; i++) fputc(ps2_r8(rec + i), f);
        fclose(f);
    }
    ps2_log("zq-font: colours +22C %08X +230 %08X +234 %08X +238 %08X +23C %08X map alpha +213 %u +217 %u", ps2_r32(font + 0x22Cu),
            ps2_r32(font + 0x230u), ps2_r32(font + 0x234u), ps2_r32(font + 0x238u), ps2_r32(font + 0x23Cu), ps2_r8(font + 0x213u),
            ps2_r8(font + 0x217u));
}
static int zq_update_enter(ps2_ctx *ctx, void *u) {
    (void)u;
    {   static int dumped;
        if (!dumped && zq_debug == 3) {
            dumped = 1;
            zq_font_dump();
            zq_stream_dump("objectives", ps2_r32(ctx->r[4].uw[0] + 0x26Cu));
            zq_stream_dump("help", ps2_r32(ctx->r[4].uw[0] + 0x270u));
            for (int i = 0; i < zn_nstreams; i++) {
                char nm[16];
                snprintf(nm, sizeof nm, "s%02d", i);
                zq_stream_dump(nm, zn_streams[i] - 8u);
            }
            zq_font_records_dump();
        }
    }
    zq_pause_field = ps2_kernel_vblank_count();
    if (ctx->r[4].uw[0] != zq_qm_menu || zq_qm_yes_id == 0xFFFFFFFFu) zq_find_qm_yes(ctx->r[4].uw[0]);
    if (!zq_debug) return 0;
    u32 m = ctx->r[4].uw[0];
    static u32 last[3] = { 1, 1, 1 };
    u32 sig[3] = { ps2_r32(m + 0x48u), ps2_r32(m + 0x4Cu), ps2_r32(m + 0x284u) };
    if (memcmp(sig, last, sizeof sig)) {
        ps2_log("zero-quit: pause menu %08X field %llu: chosen %d highlighted %d submenu %u", m,
                (unsigned long long)zq_pause_field, (int)sig[0], (int)sig[1], sig[2] & 0xFFu);
        memcpy(last, sig, sizeof sig);
    }
    return 0;
}
static int zq_update_leave(ps2_ctx *ctx, void *u) {
    (void)ctx; (void)u;
    int ok;
    u32 m = ps2_hook_entry_arg(0, &ok);
    if (!ok || !zq_ptr(m)) return 0;
    u32 chosen = ps2_r32(m + 0x48u);
    if (chosen == zq_yes_id && zq_yes_id != 0xFFFFFFFFu) {
        ps2_log("zero-quit: QUIT GAME -> YES in the pause menu");
        ps2_quit_request("pause menu: QUIT GAME");
    }
    if (chosen == zq_enh_id && zq_enh_id != 0xFFFFFFFFu) {
        /* ENHANCEMENTS: the page opens over the paused game; the choice is taken back so the mission controller sees
           none (ids >= 31 do nothing there anyway) and the menu stays as it was under the page */
        ps2_w32(m + 0x48u, 0xFFFFFFFFu);
        ps2_log("zero-quit: ENHANCEMENTS in the pause menu: the page opens%s", zn_ready && m == zn_menu ? " (drawn by the game)" : "");
        if (zn_ready && m == zn_menu) zn_open(ctx, m);
        else ps2_quit_enh_open(PS2_ENH_FROM_PAUSE);
    }
    if (zn_active) zn_frame(ctx, m);
    if (chosen == zq_tch_id && zq_tch_id != 0xFFFFFFFFu) {
        /* TOUCH CONTROLS (bug #239): the card over the paused game, the choice taken back as for ENHANCEMENTS */
        ps2_w32(m + 0x48u, 0xFFFFFFFFu);
        ps2_log("zero-quit: TOUCH CONTROLS in the pause menu: the card opens");
        ps2_quit_touch_card_open(PS2_TC_FROM_PAUSE);
    }
    if (zq_mainmenu_req) {
        zq_mainmenu_req = 0;
        if (zq_qm_yes_id != 0xFFFFFFFFu && m == zq_qm_menu && chosen == 0xFFFFFFFFu) {
            ps2_w32(m + 0x48u, zq_qm_yes_id);           /* as if QUIT MISSION -> YES was chosen */
            ps2_log("zero-quit: main menu requested: the pause menu answers QUIT MISSION -> YES (id %u)", zq_qm_yes_id);
        }
    }
    return 0;
}

/* A swg menu movie (zero_menus.c) drawn in the last 10 fields is a menu screen even while a mission is loaded (the menus
   after a mission ends): the combo opens the prompt there. ACZ_QUIT_COMBO_MENUS=0: only the pause menu counts. */
static int zq_swg_menus = 1;
static int zq_game_state(void) {
    extern volatile u64 zero_menus_last_field;
    u64 f = ps2_kernel_vblank_count(), p = zq_pause_field, m = zero_menus_last_field;
    if (p && f - p <= 6u) return PS2_QUIT_GAME_PAUSED;
    if (zero_in_mission && zq_swg_menus && m && f - m <= 10u) return PS2_QUIT_GAME_MENU;
    return zero_in_mission ? PS2_QUIT_GAME_FLYING : PS2_QUIT_GAME_MENU;
}
static int zq_main_menu(int do_it) {
    if (zq_game_state() != PS2_QUIT_GAME_PAUSED || zq_qm_yes_id == 0xFFFFFFFFu) return 0;
    if (do_it) zq_mainmenu_req = 1;
    return 1;
}

static void zq_install(void) {
    {   extern void zero_menus_install(void);           /* the menus between missions (zero_menus.c) */
        zero_menus_install();
    }
    {   const char *d = getenv("ACZ_DEBUG_PAUSE");
        zq_debug = d && *d ? (atoi(d) >= 3 ? 3 : atoi(d) >= 2 ? 2 : 1) : 0;
    }
    const char *e = getenv("ACZ_QUIT_MENU");
    zq_menu_on = !(e && *e == '0');
    e = getenv("ACZ_ENH_MENU");
    zq_enh_on = !(e && *e == '0');
    e = getenv("ACZ_ENH_NATIVE");
    zn_on = zq_enh_on && e && *e == '1';
    e = getenv("ACZ_QUIT_COMBO_MENUS");
    zq_swg_menus = !(e && *e == '0');
    {   /* TOUCH CONTROLS (bug #239): with the Quest Touch controllers in use (ACZ_TOUCH pad / stick), unless
           ACZ_TOUCH_CARD_ROW=0 */
        extern int ps2_touch_mode(void);
        e = getenv("ACZ_TOUCH_CARD_ROW");
        zq_tch_on = !(e && *e == '0') && ps2_touch_mode() != 0;
    }
    ps2_quit_set_game_state_fn(zq_game_state);
    ps2_enh_set_detail_fn(zq_detail_set);
    /* the pause menu's update tells "paused"; the builders' first child allocation must be where it was found */
    static const struct { u32 a, w; } chk[] = {
        { ZQ_PAUSE_UPDATE, 0x27BDFFA0u }, { ZQ_ALLOC_CHILDREN, 0x27BDFFD0u }, { ZQ_TEXT, 0x27BDFFC0u },
        { ZQ_BUILD0 + 0x54u, 0x0C096EFAu }, { ZQ_BUILD1 + 0x54u, 0x0C096EFAu }, { ZQ_BUILD2 + 0x54u, 0x0C096EFAu },
        { 0x001D8A70u, 0x27BDFFD0u } };
    for (unsigned i = 0; i < sizeof chk / sizeof chk[0]; i++)
        if (ps2_r32(chk[i].a) != chk[i].w) { ps2_log("zero-quit: code mismatch at %08X; pause menu hooks off", chk[i].a); return; }
    int a = ps2_hook_before(ZQ_PAUSE_UPDATE, zq_update_enter, NULL, 60, "zero-quit");
    int b = ps2_hook_after(ZQ_PAUSE_UPDATE, zq_update_leave, NULL, 60, "zero-quit");
    if (a < 0 || b < 0) { ps2_log("zero-quit: pause menu hooks FAILED"); return; }
    ps2_quit_set_title_fn(zq_main_menu);
    zq_extra = zq_menu_on + zq_enh_on + zq_tch_on;
    if (!zq_extra) { ps2_log("zero-quit: pause menu left as the game has it (ACZ_QUIT_MENU=0, ACZ_ENH_MENU=0)"); return; }
    if (ps2_pt[ZQ_PAGE >> PS2_PAGE_BITS]) { ps2_log("zero-quit: guest page %08X in use; no extra rows", ZQ_PAGE); return; }
    ps2_mem_map(ZQ_PAGE, sizeof zq_page, zq_page);
    zn_install();
    int c = ps2_hook_before(ZQ_ALLOC_CHILDREN, zq_alloc_enter, NULL, 60, "zero-quit");
    int d = ps2_hook_after(ZQ_BUILD0, zq_build_leave, NULL, 60, "zero-quit");
    int f = ps2_hook_after(ZQ_BUILD1, zq_build_leave, NULL, 60, "zero-quit");
    int g = ps2_hook_after(ZQ_BUILD2, zq_build_leave, NULL, 60, "zero-quit");
    int h = ps2_hook_replace(ZQ_TEXT, zq_text, NULL, 60, "zero-quit");
    int k = 0;
    if (zq_enh_on || zq_tch_on) {                       /* the page (and the card) hide the pause menu's own draw */
        if (ps2_r32(ZQ_PAUSE_DRAW) == 0x27BDFF40u) k = ps2_hook_replace(ZQ_PAUSE_DRAW, zq_draw_replace, NULL, 60, "zero-quit");
        else { k = -1; ps2_log("zero-quit: pause menu draw code mismatch; the page shows over the pause menu"); }
    }
    ps2_log("zero-quit: %s%s%s%s%s in the pause menu %s", zq_enh_on ? "ENHANCEMENTS" : "",
            zq_enh_on && zq_tch_on ? ", " : "", zq_tch_on ? "TOUCH CONTROLS" : "", zq_menu_on && zq_extra > 1 ? " and " : "",
            zq_menu_on ? "QUIT GAME" : "", c >= 0 && d >= 0 && f >= 0 && g >= 0 && h >= 0 && k >= 0 ? "on" : "hooks FAILED");
}

/* Trees (bug #25). The tree object (a0 of 0x1707D0; three type layers at +0, +0x350, +0x6A0) draws through
   0x170C28 (camera update at 0x170D90, head-injected; max distance = param+0xA4 = min(int(table[0] * zoom),
   obj+0xA10) with table = *(obj+0xA08) and zoom = (d + 7994) / 8565 = 1 at the game's d = 571) and 0x1771E0 (the
   VU block: VU 0x19 = (max, table[1] * zoom, max, max) = max distance / fade start, VU 0x18.z = the screen distance
   d read after the camera update, VU 0x1A = the eye). ACZ_DEBUG_TREES=N logs the table, the cap and the result
   whenever they change (before 0x1771E0: a0 = obj + 0x9F0, a2 = param, a3 = table). */
static int trees_dbg_enter(ps2_ctx *ctx, void *user) {
    static u32 last[12];
    u32 obj = ctx->r[4].uw[0] - 0x9F0u, param = ctx->r[6].uw[0], tab = ctx->r[7].uw[0], now[12];
    (void)user;
    if (obj < 0x100000u || obj >= 0x2000000u || param < 0x100000u || param >= 0x2000000u
        || tab < 0x100000u || tab >= 0x2000000u) return 0;
    u32 c = ps2_r32(param + 0xA8u);
    for (u32 i = 0; i < 6; i++) now[i] = ps2_r32(tab + 4u * i);
    now[6] = ps2_r32(obj + 0xA0Cu); now[7] = ps2_r32(obj + 0xA10u); now[8] = ps2_r32(obj + 0xA14u);
    now[9] = ps2_r32(param + 0xA4u); now[10] = ps2_r32(param + 0x80u);
    now[11] = c >= 0x100000u && c < 0x2000000u ? ps2_r32(c + 8u) : 0u;
    if (!memcmp(now, last, sizeof now)) return 0;
    memcpy(last, now, sizeof now);
    ps2_log("zero-trees: field %llu obj %08X table %08X: %.1f %.1f %.4g %.4g %.4g %.4g | obj+A0C %.4g +A10 (cap) %.1f "
            "+A14 %d | max distance %d (param+80 %.4g, d %.2f)", (unsigned long long)ps2_kernel_vblank_count(), obj, tab,
            f_of(now[0]), f_of(now[1]), f_of(now[2]), f_of(now[3]), f_of(now[4]), f_of(now[5]), f_of(now[6]),
            f_of(now[7]), (int)now[8], (int)now[9], f_of(now[10]), f_of(now[11]));
    return 0;
}

/* Tree draw distance and fade band (bug #25 options; both default to the game's). The tree draw 0x1707D0 (a0 = tree
   object, state byte +0xBF8 == 3) reads the per-map table *(obj+0xA08): table[0] = max distance (M2 12800, M3 10240;
   capped by obj+0xA10: 46080 / 51200), table[1] = fade start (M2 10240, M3 6144): trees fade in from alpha 1 at the
   max distance to 128 at the fade start (blended far path, alpha-tested near path). For the duration of 0x1707D0
   (which also picks the cells to visit from the max distance) ACZ_TREES_DIST=k scales both, and ACZ_TREES_FADE=f
   (0..0.9) makes the fade band at least that fraction of the max distance (M2's band is 20 %, M3's 40 %). */
static float z_trees_dist = 1.0f, z_trees_fade = 0.0f;
static u32 z_trees_tab, z_trees_saved[2];
static int trees_draw_enter(ps2_ctx *ctx, void *user) {
    u32 obj = ctx->r[4].uw[0], tab;
    (void)user;
    z_trees_tab = 0;
    if (obj < 0x100000u || obj >= 0x2000000u || ps2_r8(obj + 0xBF8u) != 3u) return 0;
    tab = ps2_r32(obj + 0xA08u);
    if (tab < 0x100000u || tab >= 0x2000000u) return 0;
    float mx = f_of(ps2_r32(tab)), fd = f_of(ps2_r32(tab + 4u));
    if (!(mx > 0.0f && mx < 1e6f) || !(fd >= 0.0f && fd <= mx)) return 0;
    z_trees_saved[0] = ps2_r32(tab); z_trees_saved[1] = ps2_r32(tab + 4u); z_trees_tab = tab;
    float nm = mx * z_trees_dist, nf = fd * z_trees_dist;
    if (z_trees_fade > 0.0f && nf > nm * (1.0f - z_trees_fade)) nf = nm * (1.0f - z_trees_fade);
    ps2_w32(tab, w_of(nm)); ps2_w32(tab + 4u, w_of(nf));
    return 0;
}
static int trees_draw_leave(ps2_ctx *ctx, void *user) {
    (void)ctx; (void)user;
    if (z_trees_tab) { ps2_w32(z_trees_tab, z_trees_saved[0]); ps2_w32(z_trees_tab + 4u, z_trees_saved[1]); z_trees_tab = 0; }
    return 0;
}

/* ---- The NEXT MISSION tier (settings review package B, bug #318; ps2_menv.h, docs/SETTINGS.md) ----
   Where zero_in_mission goes 1 (zb_gc_enter): a mission is entered. Also at a new attempt (z_mission_init: a pause menu's RETRY,
   MISSION FAILED's RESTART; no GameControl::enter), where the rows that need it keep waiting (ps2_settings.c enh_retry_tab). The rows changed
   since the last edge are published; then this thread's statics are read again, here; the renderer's and the graphics
   thread's at their next frame (menv_rt_frame, menv_gs_frame), the audio thread's in its next chunk (ps2_gf_mix). Nothing
   changed: one log line, nothing is read again. The cockpit's loader starts after this (ps2_vk_cockpit_aircraft is
   published below in zb_gc_enter), and what it makes from these switches goes by the start's value (hg_boot, gfx_refl_on,
   ps2_crk_boot_on). */
extern void zero_cracks_menv_reload(void);
extern void zero_trails_menv_reload(void);
extern void zero_flyby_menv_reload(void);
extern void zero_props_menv_reload(void);
extern void zero_water_menv_reload(void);
extern void zero_tmat_menv_reload(void);
extern void zero_clouds_menv_reload(void);
extern void zero_syssnd_menv_reload(void);
void zero_env_reload_mission(int retry) {
    ps2_menv_field = ps2_kernel_vblank_count();
    const int dm0 = ps2_imm_debris_mask();
    if ((retry ? ps2_enh_retry_edge() : ps2_enh_mission_edge()) <= 0) return;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    {   const int a = zv_mode; zv_read_mode(); ps2_menv_note("ACZ_VR_START_VIEW (cockpit)", a, zv_mode); }
    if (gf.init) { const float a = gf.vis_k; gf_read(); ps2_menv_note("ACZ_IMM_GLOC", a, gf.vis_k); }
    if (zbl.init) { const float a = zbl.k; zbl_read(); ps2_menv_note("ACZ_IMM_BLAST", a, zbl.k); }
    ps2_menv_note("ACZ_IMM_DEBRIS (mask)", dm0, ps2_imm_debris_mask());
    zg_menv_reload();
    {   const float a = z_trees_dist; z_trees_dist = env_float("ACZ_TREES_DIST", 1.0f);
        if (!(z_trees_dist >= 0.25f && z_trees_dist <= 4.0f)) z_trees_dist = 1.0f;
        ps2_menv_note("ACZ_TREES_DIST", a, z_trees_dist); }
    {   const float a = z_stereo_scale; z_stereo_scale = env_float("ACZ_VR_STEREO_SCALE", 1.0f);
        ps2_menv_note("ACZ_VR_STEREO_SCALE (game)", a, z_stereo_scale); }
    zero_cracks_menv_reload();
    zero_trails_menv_reload();
    zero_flyby_menv_reload();
    zero_props_menv_reload();
    zero_water_menv_reload();
    zero_tmat_menv_reload();
    zero_clouds_menv_reload();
    zero_syssnd_menv_reload();                   /* (its own reader comes by once a second: a RETRY's flight can start the next field) */
    clock_gettime(CLOCK_MONOTONIC, &t1);
    ps2_log("mission: game thread reload %.0f us at field %llu", (double)(t1.tv_sec - t0.tv_sec) * 1e6 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e3,
            (unsigned long long)ps2_menv_field);
}
void zero_vr_init(void) {
    float k = env_float("ACZ_FOV_SCALE", 1.0f);
    for (int i = 0; i < 4; i++)
        if (ps2_r32(Z_CTX_UPDATE + 4u * (u32)i) != z_ctx_update_w[i]) return;   // not Ace Combat Zero (NTSC-U)
    ps2_log("zero-vr: Ace Combat Zero detected (view context update at %08X)", Z_CTX_UPDATE);
    {   /* native VU1 programs (rn_vp): opt-in on Zero for now (PS2_RN_VP=1 / verify) */
        extern int rn_vp_mode;
        if (!getenv("PS2_RN_VP") && rn_vp_mode) { rn_vp_mode = 0; ps2_log("zero-vr: native VU1 programs off (PS2_RN_VP=1 enables)"); }
    }
    mc_debug_install();
    mc_fifo_install();
    vib_install();
    zero_boot_install();
    instr_install();
    zv_install();
    zg_install();
    ze_install();                                       /* light in tunnels: the sun and sky rays (bug #267) */
    zq_install();
    imm_install();
    flash_install();
    burst_install();                   /* (Pixy's MPBM burst as a volume: ACZ_VR_FX_BURST) */
    snd_listener_install();
    { extern void zero_audio3d_install(void); zero_audio3d_install(); }   /* spatial audio sources (zero_audio3d.c) */
    { extern void zero_syssnd_install(void); zero_syssnd_install(); }   /* cockpit system sounds (zero_syssnd.c) */
    { extern void zero_trails_install(void); zero_trails_install(); }     /* contrails, vapour, heat haze (zero_trails.c) */
    { extern void zero_cracks_install(void); zero_cracks_install(); }     /* canopy cracks, hit camera shake (zero_cracks.c) */
    { extern void zero_flyby_install(void); zero_flyby_install(); }       /* close passes of other aircraft (zero_flyby.c) */
    {   const char *e = getenv("ACZ_VR_HEADLOOK");
        z_stick = e && !strcmp(e, "stick");
        if (z_stick) {
            extern int (*ps2_pad_head_look)(u8 *rx, u8 *ry);
            z_look_yaw = env_float("ACZ_VR_LOOK_YAW_DEG", 90.0f);
            z_look_pitch = env_float("ACZ_VR_LOOK_PITCH_DEG", 60.0f);
            z_look_dz = env_float("ACZ_VR_LOOK_DZ", 64.0f);
            if (z_look_dz < 0.0f) z_look_dz = 0.0f;
            if (z_look_dz > 120.0f) z_look_dz = 120.0f;
            z_look_inv = getenv("ACZ_VR_LOOK_INVERT_Y") && *getenv("ACZ_VR_LOOK_INVERT_Y") == '1';
            ps2_pad_head_look = z_head_look;
            ps2_log("zero-vr: head look through the right stick (full deflection at %.0f deg yaw / %.0f deg pitch, "
                    "dead zone %.0f), head-locked image", z_look_yaw, z_look_pitch, z_look_dz);
        }
    }
    if (!getenv("ACZ_NO_PACKET_RELOC") && ps2_r32(Z_PH_BEGIN) == 0x808F0015u)
        ps2_hook_before(Z_PH_BEGIN, ph_relocate, NULL, 40, "zero-vr-packets");
    if (getenv("ACZ_DEBUG_DISP")) {
        ps2_hook_before(0x00100F00u, disp_enter, NULL, 50, "zero-vr-disp");
        ps2_hook_before(0x00102798u, ph_begin, NULL, 50, "zero-vr-ph");
    }
    float ck = env_float("ACZ_CULL_SCALE", 1.0f);
    ps2_log("zero-vr: ACZ_FOV_SCALE=%s ACZ_CULL_SCALE=%s (parsed %.3f / %.3f), clip const word %08X", getenv("ACZ_FOV_SCALE") ? getenv("ACZ_FOV_SCALE") : "-", getenv("ACZ_CULL_SCALE") ? getenv("ACZ_CULL_SCALE") : "-", k, ck, ps2_r32(Z_CLIP_CONST));
    if (ck > 1.0001f && ck < 16.0f && ps2_r32(Z_CLIP_CONST) == Z_CLIP_CONST_W) {
        ps2_w32(Z_CLIP_CONST, w_of(0.504f * ck));
        ps2_log("zero-vr: culling frustum widened by %.2f (clip constant 0.504 -> %.4f)", ck, 0.504f * ck);
    }
    {   /* "Powerful PC" detail knobs, the two constants the official 2026 PC port raised (LOD x16, terrain detail
           x2). Both default to the PS2 values; each scales draw calls and GPU time, so measure before raising.
           ACZ_LOD_BIAS: LodModel::SelectLod's 512.0 at 0x3F7800 (only reader 0x131140), written as 512 / N (see
           "Model LOD" above: the constant scales the distance); 2-4 keeps distant aircraft detailed longer, 16 = PC port.
           ACZ_TERRAIN_DETAIL: terrain detail ring 32768 / 24576 at 0x3F8108 / 0x3F810C (readers 0x15E230/0x15E240);
           2 = PC port (65536 / 49152). */
        float lb = env_float("ACZ_LOD_BIAS", 1.0f), td = env_float("ACZ_TERRAIN_DETAIL", 1.0f);
        z_lod_word(1.0f);                                /* (reads ACZ_LOD_BIAS_MAP) */
        if (lb != 1.0f) {
            u32 w = ps2_r32(Z_LOD_CONST);
            if (w == 0x44000000u && lb > 0.05f && lb <= 32.0f) {
                float c = z_lod_word(lb);
                ps2_w32(Z_LOD_CONST, w_of(c));
                z_lod_bias = lb;
                ps2_log("zero-vr: LOD bias x%.2f (512 -> %.1f: model LOD switches and draw cutoff at x%.2f the distance%s)",
                        lb, c, 512.0f / c, z_lod_map ? "" : "; ACZ_LOD_BIAS_MAP=0, the old mapping");
            } else ps2_log("zero-vr: ACZ_LOD_BIAS ignored (word %08X at 0x3F7800 is not 512.0, or value out of range)", w);
        }
        zl_install();
        if (td != 1.0f) {
            u32 a = ps2_r32(0x003F8108u), b = ps2_r32(0x003F810Cu);
            if (td > 0.25f && td <= 4.0f && a == w_of(32768.0f) && b == w_of(24576.0f)) {
                ps2_w32(0x003F8108u, w_of(32768.0f * td)); ps2_w32(0x003F810Cu, w_of(24576.0f * td));
                ps2_log("zero-vr: terrain detail range x%.2f (float)", td);
            } else if (td > 0.25f && td <= 4.0f && a == 32768u && b == 24576u) {
                ps2_w32(0x003F8108u, (u32)(32768.0f * td)); ps2_w32(0x003F810Cu, (u32)(24576.0f * td));
                ps2_log("zero-vr: terrain detail range x%.2f (integer)", td);
            } else ps2_log("zero-vr: ACZ_TERRAIN_DETAIL ignored (words %08X %08X at 0x3F8108 are not 32768/24576)", a, b);
        }
    }
    if (getenv("ACZ_DEBUG_TREES") && ps2_r32(0x001771E0u) >> 16 == 0x3C0Fu
        && ps2_hook_before(0x001771E0u, trees_dbg_enter, NULL, 80, "zero-trees-debug") >= 0)
        ps2_log("zero-vr: debug: tree table logged (ACZ_DEBUG_TREES)");
    lf_install();
    z_trees_dist = env_float("ACZ_TREES_DIST", 1.0f);
    z_trees_fade = env_float("ACZ_TREES_FADE", 0.0f);
    if (!(z_trees_dist >= 0.25f && z_trees_dist <= 4.0f)) z_trees_dist = 1.0f;
    if (!(z_trees_fade >= 0.0f && z_trees_fade <= 0.9f)) z_trees_fade = 0.0f;
    if ((z_trees_dist != 1.0f || z_trees_fade > 0.0f) && ps2_r32(0x001707D0u) >> 16 == 0x27BDu
        && ps2_hook_before(0x001707D0u, trees_draw_enter, NULL, 50, "zero-trees") >= 0
        && ps2_hook_after(0x001707D0u, trees_draw_leave, NULL, 50, "zero-trees") >= 0)
        ps2_log("zero-vr: trees: draw distance x%.2f, fade band at least %.0f %% of it (ACZ_TREES_DIST / ACZ_TREES_FADE)",
                z_trees_dist, 100.0f * z_trees_fade);
    { const char *e = getenv("ACZ_DUMP_FIELD"); z_dump_field = e ? strtoull(e, NULL, 0) : 0; }
    z_units_per_m = env_float("ACZ_VR_UNITS_PER_M", 6.0f);   /* flight: 1109 units/s at 324 kt, ~5-6.6 units/m */
    z_stereo_scale = env_float("ACZ_VR_STEREO_SCALE", 1.0f);
    z_debug_eyes = getenv("ACZ_VR_DEBUG_EYES") != NULL;
    z_fov_limit = env_float("ACZ_VR_FOV_LIMIT", 0.0f);
    z_test_yaw = env_float("ACZ_VR_TEST_YAW", 0.0f); z_test_pitch = env_float("ACZ_VR_TEST_PITCH", 0.0f);
    const char *xe = getenv("PS2_XR"), *xm = getenv("PS2_XR_MODE");
    int want_xr = xe && *xe == '1' && xm && (!strcmp(xm, "mono") || !strncmp(xm, "stereo", 6));
    if (want_xr) ps2_log("zero-vr: head tracking requested (%s), %.3f game units per metre, stereo scale %.2f",
                         xm, z_units_per_m, z_stereo_scale);
    if (!(k > 1.0001f && k < 8.0f)) k = 1.0f;
    if (k == 1.0f && !z_dump_field && !want_xr) return;
    z_k = k;
    if (ps2_hook_before(Z_CTX_UPDATE, ctx_update_enter, NULL, 50, "zero-vr") < 0
        || ps2_hook_after(Z_CTX_UPDATE, ctx_update_leave, NULL, 50, "zero-vr") < 0) {
        ps2_log("zero-vr: the hook layer refused the FOV hook");
        return;
    }
    ps2_log("zero-vr: FOV widened by %.3f in tangent space (d / %.3f during the view context update)", k, k);
    {   const char *e = getenv("ACZ_VR_TREE_SIZE");
        z_tree_size = !(e && *e == '0');
        if (z_tree_size && ps2_r32(0x001771E0u) >> 16 == 0x3C0Fu
            && ps2_hook_before(0x001771E0u, tree_block_enter, NULL, 50, "zero-vr-trees") >= 0
            && ps2_hook_after(0x001771E0u, tree_block_leave, NULL, 50, "zero-vr-trees") >= 0)
            ps2_log("zero-vr: tree sprites sized for the head-tracked projection (ACZ_VR_TREE_SIZE=0: the game's d)");
        else z_tree_size = 0;
    }
    {   const char *e = getenv("ACZ_VR_CLOUD_HEAD");
        int n = 0, nrec = 0;
        {   const char *l = getenv("ACZ_VR_CLOUD_LOOK");
            z_cloud_look_head = l && (*l == 'h' || *l == 'H');
            /* the larger clip rectangle only with the billboards (ACZ_VR_CLOUD_PERSP default, upright on) */
            const char *pp = getenv("ACZ_VR_CLOUD_PERSP"), *uu = getenv("ACZ_VR_CLOUD_UPRIGHT");
            z_cloud_bb = !((pp && (*pp == '0' || *pp == 's')) || (uu && (*uu == '0' || *uu == 'o')));
            const char *ww = getenv("ACZ_VR_CLOUD_WINDOW");
            z_cloud_win = !(ww && *ww == '0') && ps2_r32(Z_CLOUD_WIN) == 0x47900000u;   /* 73728.0f */
            z_cloud_cull = z_cloud_bb ? env_float("ACZ_VR_CLOUD_CULL", 1.6f) : 1.0f;
            if (z_cloud_cull < 1.0f) z_cloud_cull = 1.0f;
            if (z_cloud_cull > 3.0f) z_cloud_cull = 3.0f;
            ps2_vk_cloud_cull = z_cloud_cull;
        }
        /* with ACZ_VR_CLOUD_HEAD=0 (or the images for the aircraft's camera) the hooks only record the camera roll
           the clouds are built with (the renderer's sprite turn needs it) */
        for (unsigned i = 0; want_xr && i < sizeof z_sky_fn / sizeof z_sky_fn[0]; i++) {
            u32 a = z_sky_fn[i].addr, role = z_sky_fn[i].role;
            int head = !(e && *e == '0') && (role != 1u || z_cloud_look_head);
            if (!head && role == 2u) continue;
            if (!head && a == 0x00169D60u) continue;               /* reads the context only: nothing to record */
            if (ps2_r32(a) >> 16 != 0x27BDu) { ps2_log("zero-vr: sky function %08X does not match", a); continue; }
            uintptr_t u = (uintptr_t)z_sky_fn[i].reg | (uintptr_t)role << 8 | (uintptr_t)head << 16;
            if (ps2_hook_before(a, cloud_enter, (void *)u, 50, "zero-vr-sky") < 0
                || ps2_hook_after(a, cloud_leave, NULL, 50, "zero-vr-sky") < 0)
                ps2_log("zero-vr: sky function %08X hook refused", a);
            else if (head) n++;
            else nrec++;
        }
        {   const char *cf = getenv("ACZ_VR_CLOUD_FADE");
            if (cf && *cf == '1') z_cfade_mode = 1;
            if (want_xr && !(cf && *cf == '0') && ps2_r32(0x0016A2F0u) >> 16 == 0xD889u
                && ps2_hook_before(0x0016A2F0u, cloud_fade_enter, NULL, 70, "zero-vr-sky") >= 0
                && ps2_hook_after(0x0016A2F0u, cloud_fade_leave, NULL, 70, "zero-vr-sky") >= 0)
                ps2_log("zero-vr: clouds fade by their distance in head-tracked frames, not by the view depth%s "
                        "(ACZ_VR_CLOUD_FADE=0 turns it off)", z_cfade_mode == 2 ? "" : " (past the near fade-in)");
        }
        {   const char *cb = getenv("ACZ_VR_CLOUD_BASE"), *cd = getenv("ACZ_DEBUG_CLOUD_BASE");
            z_cb_dbg = cd ? atoi(cd) : 0;
            {   const char *bm = getenv("ACZ_VR_CLOUD_BBMAX"); z_cb_bbmax = bm ? (float)atof(bm) : 0.9f;
                if (z_cb_bbmax < 0.3f) z_cb_bbmax = 0.3f; }
            if (want_xr && z_cloud_bb && !(e && *e == '0') && cb && *cb && *cb != '0' && ps2_r32(0x0016A2F0u) >> 16 == 0xD889u
                && ps2_hook_before(0x0016A2F0u, cloud_base_enter, NULL, 65, "zero-vr-sky") >= 0
                && ps2_hook_after(0x0016A2F0u, cloud_base_leave, NULL, 65, "zero-vr-sky") >= 0) {
                z_cb_on = 1;
                {   const char *ab = getenv("ACZ_VR_CLOUD_BASE_AB"); ps2_vk_cloud_base_ab = ab ? atoi(ab) : 0; }
                ps2_log("zero-vr: the game's clouds decided and shaped independently of the head in head-tracked frames "
                        "(virtual camera per cloud; ACZ_VR_CLOUD_BASE=0 turns it off)");
            }
        }
        if (getenv("ACZ_DEBUG_CLOUD_DATA") && ps2_r32(0x0016A2F0u) >> 16 == 0xD889u
            && ps2_hook_before(0x0016A2F0u, cloud_data_proj_enter, NULL, 80, "zero-vr-debug") >= 0
            && ps2_hook_after(0x0016A2F0u, cloud_data_proj_leave, NULL, 80, "zero-vr-debug") >= 0)
            ps2_log("zero-vr: debug: cloud field data logged (ACZ_DEBUG_CLOUD_DATA)");
        if (want_xr && nrec)
            ps2_log("zero-vr: cloud images built for the %s camera (%d functions; ACZ_VR_CLOUD_LOOK=head: the head-turned "
                    "one); the renderer turns each cloud sprite by the head's roll", e && *e == '0' ? "game's own" : "aircraft's", nrec);
        const char *sr = getenv("ACZ_VR_SKY_ROOT");
        u32 sra = sr ? (u32)strtoul(sr, NULL, 16) : 0u;
        if (sr && sra < 0x100000u) sra = Z_SKY_ROOT;            /* "1": the default root */
        if (want_xr && !(e && *e == '0') && sr && ps2_r32(sra) >> 16 == 0x27BDu
            && ps2_hook_before(sra, sky_root_enter, NULL, 50, "zero-vr-sky") >= 0
            && ps2_hook_after(sra, sky_root_leave, NULL, 50, "zero-vr-sky") >= 0) {
            n++;
            ps2_log("zero-vr: sky root %08X drawn with the head-turned camera", sra);
        }
        {   const char *sa = getenv("ACZ_VR_SKY_ANGLES");
            if (want_xr && !(e && *e == '0') && !(sa && *sa == '0') && ps2_r32(Z_SKY_RENDER) >> 16 == 0x27BDu
                && ps2_hook_before(Z_SKY_RENDER, skyang_enter, NULL, 50, "zero-vr-sky") >= 0
                && ps2_hook_after(Z_SKY_RENDER, skyang_leave, NULL, 50, "zero-vr-sky") >= 0) n++;
        }
        {   const char *sk = getenv("ACZ_VR_SKY_CAM");
            if (want_xr && !(e && *e == '0') && !(sk && *sk == '0') && ps2_r32(Z_SKY_RENDER) >> 16 == 0x27BDu
                && ps2_hook_before(Z_SKY_RENDER, skycam_enter, NULL, 50, "zero-vr-sky") >= 0
                && ps2_hook_after(Z_SKY_RENDER, skycam_leave, NULL, 50, "zero-vr-sky") >= 0) n++;
        }
        {   /* ACZ_VR_FX_ANGLES=0: the effects camera block (billboards, snow/rain box) keeps the angles without the head */
            const char *fa = getenv("ACZ_VR_FX_ANGLES"), *fs = getenv("ACZ_VR_FX_SIZE"), *fb = getenv("ACZ_VR_FX_SIZE_AB");
            z_fx_angles = !(fa && *fa == '0');
            z_fx_size = !(fs && *fs == '0');
            z_fx_size_ab = fb ? atoi(fb) : 0;
            if (want_xr && !(e && *e == '0') && (z_fx_angles || z_fx_size) && ps2_r32(Z_FX_CAMERA) >> 16 == 0x27BDu
                && ps2_hook_before(Z_FX_CAMERA, fx_enter, NULL, 50, "zero-vr-sky") >= 0
                && ps2_hook_after(Z_FX_CAMERA, fx_leave, NULL, 50, "zero-vr-sky") >= 0) n++;
        }
        {   const char *k = getenv("ACZ_DEBUG_KILL");
            while (k && *k) {
                char *end; u32 a = (u32)strtoul(k, &end, 16);
                if (end == k) break;
                if (a && ps2_hook_replace(a, kill_fn, NULL, 50, "zero-vr-debug") >= 0) ps2_log("zero-vr: debug: %08X killed", a);
                k = *end == ',' ? end + 1 : end;
            }
        }
        if (n) ps2_log("zero-vr: clouds and sky layers (%d functions) drawn with the head-turned camera "
                       "(ACZ_VR_CLOUD_HEAD=0 turns it off)", n);
    }
}

void zero_vr_report(void) {
    if (z_k != 1.0f) ps2_log("zero-vr: view context updates scaled: %llu", (unsigned long long)z_calls);
    if (z_head_updates) ps2_log("zero-vr: head-tracked camera updates: %llu", (unsigned long long)z_head_updates);
    snd_listener_report();
    zg_report();
    ze_report();
    { extern void zero_audio3d_report(void); zero_audio3d_report(); }
    { extern void zero_syssnd_report(void); zero_syssnd_report(); }
    { extern void zero_cracks_report(void); zero_cracks_report(); }
}
