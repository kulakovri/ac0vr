// ps2_touch.c - Quest Touch controllers as the pilot's controls (tc_touch, 2026-10-23; CONTROLS_RESEARCH_2026-10-23.md
// sections 3.2-3.5 and 4.2-4.6). ps2_xr.c reads the controllers (OpenXR actions); this file maps them. Renderer thread.
//
// ACZ_TOUCH=pad    Touch as a gamepad with the modern positions: a virtual Xbox pad merged into the SDL pad
//                  (ps2_video.c pad_axis / pad_button: per axis the larger magnitude wins, buttons OR'ed), so the
//                  layouts (modern / classic), the bindings, the View/Back recentre and the quit combo work unchanged.
//                  Left thumbstick = left stick; triggers = triggers; grips = bumpers with an analog value (rest below
//                  0.3); A B X Y = A B X Y; right thumbstick = D-pad flicks (ACZ_TOUCH_RS=look: the right stick);
//                  right click = R3 (on release); left click = View (tap: the map; held 1 s: recentre); both clicks =
//                  L3; Menu = Menu (tap, sent on release; held 0.5 s = View + Menu, the quit / ENHANCEMENTS combo).
// ACZ_TOUCH=stick  While flying: the right hand grabs a virtual stick with the grip (held, or ACZ_TOUCH_GRIP=toggle);
//                  the controller's tilt relative to its orientation at the grab is pitch and roll (VTOL VR style,
//                  position unused), measured about a gravity-up frame turned to the hand's heading at the grab. Full
//                  stick at ACZ_TOUCH_STICK_THROW degrees (default: the cockpit's own stick throw, else 20), inner dead
//                  zone ACZ_TOUCH_STICK_DZ, expo ACZ_TOUCH_STICK_EXPO, a 1-euro filter against tremor
//                  (ACZ_TOUCH_STICK_FILTER); the output starts at the game's per-axis dead-zone edge (64 of 127), so the
//                  first degree of tilt already moves the aircraft. Release: the stick eases to centre
//                  (ACZ_TOUCH_STICK_RELEASE s). Trigger = gun (Cross), A = missile (Circle), B = target (Triangle),
//                  right thumbstick = D-pad flicks, its click = R3 (view). The left hand grabs the throttle lever: hand
//                  fore/aft along the cockpit, ACZ_TOUCH_THROTTLE_CM per range (CRUISE -> MIL forward, CRUISE -> IDLE
//                  back, then the air brake), detents IDLE / CRUISE / MIL and an AB gate past MIL with haptic ticks;
//                  the lever stays where it is let go (ACZ_TOUCH_THROTTLE=spring: back to cruise; tilt: the lever from
//                  the left controller's fore/aft tilt; thumbstick: left thumbstick Y as the PS2's spring throttle).
//                  Left trigger = brake (overrides the lever), left thumbstick X = yaw (L2 / R2 pressure), its click =
//                  autopilot (L2 + R2), X = weapon switch (Square), Y = map (Select; held 1 s: recentre), Menu = Start
//                  (held 0.5 s: the quit combo). Outside flight (menus, pause, events) Touch acts as in pad mode.
//                  Bug #239 (touch_ux 2026-10-29; the user's first try: "how do I speed up ... switch weapons ... the
//                  controls are not ready at all"): the left thumbstick up / down is thrust / air brake while it is held
//                  (full up = afterburner; released: the lever's setting again; ACZ_TOUCH_THUMB_THRUST, in LEVER /
//                  SPRING / TILT throttle modes; the yaw then takes the thumbstick's X only where X dominates); Menu
//                  held ACZ_TOUCH_CARD_HOLD (1 s) pauses and shows the TOUCH CONTROLS card (ps2_quit.c; the View + Menu
//                  combo is not sent in flight any more), which also comes up by itself once, the first time HANDS ON
//                  STICK is used (ACZ_TOUCH_CARD); "SQUEEZE THE RIGHT GRIP" shows when the right controller is waved
//                  or tilted in flight without the stick held (ACZ_TOUCH_HINTS).
// ACZ_TOUCH=auto   (the default since ctl_priority 2026-11-03; unset = auto, off / 0 = off): PAD mode while no
//                  gamepad is connected (the owner: "if no controller is connected at all, the default controller is
//                  the Quest's Touch"); while one is, Touch is muted -- no input, no vibration, the cockpit's hands as
//                  with OFF -- unless the player makes Touch the active controller (ENHANCEMENTS > CONTROLS > ACTIVE
//                  CONTROLLER = TOUCH). ps2_video.c's controller registry decides (ps2_touch_set_muted). The action set
//                  is made whenever ACZ_TOUCH is not off (OpenXR attaches it once per session).
//                  Bugs #314 / #315 (2026-11-05; the owner, Touch only: "I do not see how to grab the stick with the gloves";
//                  with the pad on: "I took the Quest controllers, they are either off or something"): the mute no longer
//                  blinds the registry. In AUTO the controllers are read before the mute (tp_scan): a real press -- trigger or
//                  grip past 0.65, the thumbstick past 0.60, a button -- held 50 ms, after the release levels (0.35, 0.30) were
//                  seen, on a controller bound for 0.5 s, not while the pad is in use (ps2_touch_set_pad_in_use: it holds
//                  something beyond a quarter of its travel or did in the last 0.5 s, or a pad is forced) makes Touch the
//                  active controller, reason "press, <hand> <control>". And in flight a hand closing on the stick or the throttle
//                  at the control (auto_take; ACZ_TOUCH_AUTO_GRAB, default near) turns the session into HANDS ON STICK, with or
//                  without a pad: "touch: AUTO -> HANDS ON STICK (grab, <hand> hand)", the registry's reason "grab", the gloves
//                  and the card (ACZ_TOUCH_CARD) follow as in an explicit HANDS ON STICK. A pad's press then mutes Touch again
//                  (the hands let go and go; the lever back at CRUISE), a Touch press brings the session back. A hand within
//                  reach of the stick without pressing gets one line, once a session (ACZ_TOUCH_HINTS).
// ACZ_TOUCH_GRAB (bug #307, 2026-11-05; ENHANCEMENTS > CONTROLS > GRAB WITH; the owner: "I tried to grab the levers with
//                  the trigger, as I am used to, and they did not react"): which button closes a hand on the stick / lever
//                  in HANDS ON STICK. either (default): the grip as before, from anywhere; the trigger only when the
//                  controller is near the control (ACZ_TOUCH_GRAB_NEAR, default 0.25 m from the model's grip point, 0.38 m
//                  from a generic one in a model without gloves on its controls), and only its press, so a trigger already
//                  held does not grab. grip: as before 2026-11-05. trigger: the trigger, from anywhere. The button that
//                  closed the hand holds it (ACZ_TOUCH_GRIP: hold or tap as before); while a trigger holds, the same hand's
//                  grip does the trigger's job (gun, brake), and a free hand near its control does not fire or brake with
//                  the trigger: there it grabs. grab_by says which one closed it.
// Both modes: the Xbox pad stays live (hybrid); Touch idle sends nothing. Rumble (the game's motors, weather, buffet)
// goes to Touch at ACZ_TOUCH_HAPTICS (0.5) while Touch is the active controller (ps2_video.c apply_rumble: the device
// the player used last); stick mode adds detent / AB / gun / missile cues (ACZ_TOUCH_CUES).
// Tracking / focus: orientation lost holds the stick 0.3 s, then it eases to centre; the lever freezes while position
// tracking is lost and ignores jumps over 3 cm in one poll; focus lost (dashboard, headset off) releases both grabs
// (ACZ_TOUCH_AUTOPAUSE=1 also pauses the game). Everything ends as DualShock 2 bytes: the game, PS2PAD2 recordings and
// replays are unchanged. The cockpit (tc_cockpit, ps2_controls.h) gets the stick / lever / hand state in ps2_ctl.
// Tests: PS2_XR_TEST_TOUCH (synthetic controllers by field), ACZ_DEBUG_TOUCH=N (a log line every N fields).
#include "ps2_runtime.h"
#include "ps2_hle.h"
#include "ps2_touch.h"
#include "ps2_settings.h"
#include "ps2_menv.h"
#include "ps2_quit.h"                /* the TOUCH CONTROLS card and the hints (bug #239) */
#include "zero_instr.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#if defined(__has_include)
#if __has_include("ps2_controls.h")
#include "ps2_controls.h"            /* tc_cockpit: the cockpit's stick / throttle / hands (ps2_ctl, ps2_video.c) */
#define TC_CTL 1
#endif
#endif
#if defined(TC_CTL) && defined(PS2_TOUCH_OWN_CTL)
ps2_controls ps2_ctl;                /* (only in tc_touch's own test tree; tc_cockpit defines it in ps2_video.c) */
#endif

u64 ps2_kernel_vblank_count(void);
int ps2_xr_mode(void);
void ps2_xr_request_recenter(void);
extern volatile float ps2_imm_in[8];               /* ps2_video.c: [3] = the field of the last flight frame */
extern int (*ps2_vk_instr_get)(zero_instr *out);   /* ps2_video.c; set by zero_vr.c */

enum { THR_LEVER, THR_SPRING, THR_TILT, THR_STICK };
enum { CUE_DETENT = 1, CUE_AB = 2, CUE_GUN = 4, CUE_MISSILE = 8, CUE_CENTRE = 16 };

static struct {
    int init, mode, test, toggle, rs_look, thr, frame_cockpit, twist, swap, autopause, remap, dbg;
    int thumb_thrust, card, hints;   /* bug #239: left thumbstick thrust; the card once / always / never; hints */
    int autom;                       /* ACZ_TOUCH=auto: PAD mode, muted while a gamepad is the active controller */
    int auto_grab;                   /* ACZ_TOUCH_AUTO_GRAB: 0 off, 1 the GRAB WITH rule, 2 (default) both buttons only at the control */
    int grab;                        /* ACZ_TOUCH_GRAB (PS2_TOUCH_GRAB_*) */
    float grab_near;                 /* ACZ_TOUCH_GRAB_NEAR: metres, the trigger's reach in PS2_TOUCH_GRAB_EITHER */
    float throw_deg, dz_deg, expo, f_min, f_beta, release_s, lever_cm, haptics, reach, game_dz, grip_rest, card_hold;
    unsigned cues;
} cfg;

#define getenv ps2_menv                  /* the NEXT MISSION tier: this file's switches; all others fall through to getenv */
static float envf(const char *n, float d) {
    const char *e = getenv(n);
    if (!e || !*e) return d;
    char *end;
    float v = strtof(e, &end);
    return end == e ? d : v;
}
static float clampf_(float v, float a, float b) { return v < a ? a : v > b ? b : v; }

/* the rows of the NEXT MISSION tier (ps2_menv.h), the renderer's first frame after the edge; per frame readers, no derived state.
   (Not ACZ_TOUCH itself: the OpenXR actions are made at the session's start. Not the haptics: a live row.) */
void ps2_touch_menv_reload(void) {
    if (!cfg.init) return;
    const int a = cfg.toggle, b = cfg.grab, c = cfg.rs_look, d = cfg.thr, f = cfg.swap, g = cfg.remap;
    const float h = cfg.throw_deg, i = cfg.expo;
    const char *e = getenv("ACZ_TOUCH_GRIP"); cfg.toggle = e && !strcmp(e, "toggle");
    e = getenv("ACZ_TOUCH_GRAB");
    cfg.grab = e && !strcmp(e, "grip") ? PS2_TOUCH_GRAB_GRIP : e && !strcmp(e, "trigger") ? PS2_TOUCH_GRAB_TRIGGER
             : PS2_TOUCH_GRAB_EITHER;
    e = getenv("ACZ_TOUCH_RS"); cfg.rs_look = e && !strcmp(e, "look");
    e = getenv("ACZ_TOUCH_THROTTLE");
    cfg.thr = !e ? THR_LEVER : !strcmp(e, "spring") ? THR_SPRING : !strcmp(e, "tilt") ? THR_TILT
            : !strcmp(e, "thumbstick") || !strcmp(e, "stick") ? THR_STICK : THR_LEVER;
    e = getenv("ACZ_TOUCH_SWAP"); cfg.swap = e && *e == '1';
    e = getenv("ACZ_PAD_STICK_REMAP"); cfg.remap = e && *e == '1';
    cfg.throw_deg = envf("ACZ_TOUCH_STICK_THROW", 0.0f);
    if (cfg.throw_deg != 0.0f) cfg.throw_deg = clampf_(cfg.throw_deg, 8.0f, 40.0f);
    cfg.expo = clampf_(envf("ACZ_TOUCH_STICK_EXPO", 0.25f), 0.0f, 0.9f);
    ps2_menv_note("ACZ_TOUCH_GRIP", a, cfg.toggle); ps2_menv_note("ACZ_TOUCH_GRAB", b, cfg.grab);
    ps2_menv_note("ACZ_TOUCH_RS", c, cfg.rs_look); ps2_menv_note("ACZ_TOUCH_THROTTLE", d, cfg.thr);
    ps2_menv_note("ACZ_TOUCH_SWAP", f, cfg.swap); ps2_menv_note("ACZ_PAD_STICK_REMAP (touch)", g, cfg.remap);
    ps2_menv_note("ACZ_TOUCH_STICK_THROW", h, cfg.throw_deg); ps2_menv_note("ACZ_TOUCH_STICK_EXPO", i, cfg.expo);
}
static void cfg_init(void) {
    if (cfg.init) return;
    cfg.init = 1;
    const char *e = getenv("ACZ_TOUCH");
    cfg.test = getenv("PS2_XR_TEST_TOUCH") != NULL;
    /* unset / auto: AUTO (a test script without ACZ_TOUCH: pad mode, as before) */
    cfg.autom = (!e || !*e || !strcmp(e, "auto")) && !(cfg.test && !(e && *e));
    cfg.mode = cfg.autom ? PS2_TOUCH_PAD : !e || !*e ? PS2_TOUCH_OFF : !strcmp(e, "pad") || !strcmp(e, "1") ? PS2_TOUCH_PAD
             : !strcmp(e, "stick") || !strcmp(e, "2") ? PS2_TOUCH_STICK : PS2_TOUCH_OFF;
    e = getenv("ACZ_TOUCH_GRIP"); cfg.toggle = e && !strcmp(e, "toggle");
    e = getenv("ACZ_TOUCH_GRAB");
    cfg.grab = e && !strcmp(e, "grip") ? PS2_TOUCH_GRAB_GRIP : e && !strcmp(e, "trigger") ? PS2_TOUCH_GRAB_TRIGGER
             : PS2_TOUCH_GRAB_EITHER;
    cfg.grab_near = clampf_(envf("ACZ_TOUCH_GRAB_NEAR", 0.25f), 0.05f, 1.0f);
    e = getenv("ACZ_TOUCH_AUTO_GRAB");
    cfg.auto_grab = e && *e == '0' ? 0 : e && *e == '1' ? 1 : 2;
    e = getenv("ACZ_TOUCH_RS"); cfg.rs_look = e && !strcmp(e, "look");
    e = getenv("ACZ_TOUCH_THROTTLE");
    cfg.thr = !e ? THR_LEVER : !strcmp(e, "spring") ? THR_SPRING : !strcmp(e, "tilt") ? THR_TILT
            : !strcmp(e, "thumbstick") || !strcmp(e, "stick") ? THR_STICK : THR_LEVER;
    e = getenv("ACZ_TOUCH_STICK_FRAME"); cfg.frame_cockpit = e && !strcmp(e, "cockpit");
    e = getenv("ACZ_TOUCH_STICK_TWIST"); cfg.twist = e && *e == '1';
    e = getenv("ACZ_TOUCH_SWAP"); cfg.swap = e && *e == '1';
    e = getenv("ACZ_TOUCH_AUTOPAUSE"); cfg.autopause = e && *e == '1';
    e = getenv("ACZ_PAD_STICK_REMAP"); cfg.remap = e && *e == '1';
    e = getenv("ACZ_DEBUG_TOUCH"); cfg.dbg = e ? atoi(e) : 0;
    if (e && cfg.dbg <= 0) cfg.dbg = 60;
    cfg.throw_deg = envf("ACZ_TOUCH_STICK_THROW", 0.0f);
    if (cfg.throw_deg != 0.0f) cfg.throw_deg = clampf_(cfg.throw_deg, 8.0f, 40.0f);
    cfg.dz_deg = clampf_(envf("ACZ_TOUCH_STICK_DZ", 1.0f), 0.0f, 5.0f);
    cfg.expo = clampf_(envf("ACZ_TOUCH_STICK_EXPO", 0.25f), 0.0f, 0.9f);
    cfg.f_min = 1.5f; cfg.f_beta = 0.1f;
    e = getenv("ACZ_TOUCH_STICK_FILTER");
    if (e && *e) {
        float a = 0.0f, b = 0.1f;
        int n = sscanf(e, "%f,%f", &a, &b);
        cfg.f_min = n >= 1 ? a : 0.0f; cfg.f_beta = n >= 2 ? b : 0.1f;
    }
    cfg.release_s = clampf_(envf("ACZ_TOUCH_STICK_RELEASE", 0.2f), 0.0f, 2.0f);
    cfg.lever_cm = clampf_(envf("ACZ_TOUCH_THROTTLE_CM", 8.0f), 3.0f, 20.0f);
    cfg.haptics = clampf_(envf("ACZ_TOUCH_HAPTICS", 0.5f), 0.0f, 1.0f);
    cfg.reach = clampf_(envf("ACZ_TOUCH_REACH", 0.0f), 0.0f, 1.0f);
    cfg.game_dz = clampf_(envf("ACZ_TOUCH_GAME_DZ", 64.0f), 0.0f, 120.0f);
    cfg.grip_rest = clampf_(envf("ACZ_TOUCH_GRIP_REST", 0.3f), 0.0f, 0.8f);   /* PAD: a grip below this is a resting hand */
    e = getenv("ACZ_TOUCH_THUMB_THRUST"); cfg.thumb_thrust = !(e && *e == '0');
    e = getenv("ACZ_TOUCH_CARD");                          /* auto: once (the first STICK session), 1: every start */
    cfg.card = e && *e == '0' ? 0 : e && (*e == '1' || !strcmp(e, "always")) ? 2 : 1;
    cfg.card_hold = clampf_(envf("ACZ_TOUCH_CARD_HOLD", 1.0f), 0.0f, 5.0f);
    if (cfg.card_hold > 0.0f && cfg.card_hold < 0.6f) cfg.card_hold = 0.6f;   /* (past a Start tap's 0.5 s) */
    e = getenv("ACZ_TOUCH_HINTS"); cfg.hints = !(e && *e == '0');
    cfg.cues = CUE_DETENT | CUE_AB | CUE_GUN | CUE_MISSILE;
    e = getenv("ACZ_TOUCH_CUES");
    if (e) {
        cfg.cues = 0;
        if (strstr(e, "all")) cfg.cues = 31;
        if (strstr(e, "detent")) cfg.cues |= CUE_DETENT;
        if (strstr(e, "ab")) cfg.cues |= CUE_AB;
        if (strstr(e, "gun")) cfg.cues |= CUE_GUN;
        if (strstr(e, "missile")) cfg.cues |= CUE_MISSILE;
        if (strstr(e, "centre") || strstr(e, "center")) cfg.cues |= CUE_CENTRE;
    }
    if (cfg.mode != PS2_TOUCH_OFF || cfg.test)
        ps2_log("touch: mode %s, grip %s, stick throw %s%.0f deg dz %.1f expo %.2f filter %.2f Hz / %.3f, release %.2f s, "
                "frame %s, twist %s; throttle %s (%.0f cm per range); right thumbstick %s; haptics %.2f cues %02X; "
                "reach %.2f m; swap %d; autopause %d; game dead zone %.0f; grip rest %.2f",
                cfg.autom ? "auto (as a pad while no gamepad is connected)" : cfg.mode == PS2_TOUCH_PAD ? "pad"
                : cfg.mode == PS2_TOUCH_STICK ? "stick" : "off",
                cfg.toggle ? "toggle" : "hold", cfg.throw_deg > 0.0f ? "" : "auto ", cfg.throw_deg > 0.0f ? cfg.throw_deg : 20.0f,
                cfg.dz_deg, cfg.expo, cfg.f_min, cfg.f_beta, cfg.release_s, cfg.frame_cockpit ? "cockpit" : "hand",
                cfg.twist ? "yaw" : "off",
                cfg.thr == THR_LEVER ? "lever" : cfg.thr == THR_SPRING ? "spring" : cfg.thr == THR_TILT ? "tilt" : "thumbstick",
                cfg.lever_cm, cfg.rs_look ? "look" : "d-pad", cfg.haptics, cfg.cues, cfg.reach, cfg.swap, cfg.autopause,
                cfg.game_dz, cfg.grip_rest);
    if (cfg.mode != PS2_TOUCH_OFF || cfg.test)
        ps2_log("touch: grab with %s (ACZ_TOUCH_GRAB; the trigger within %.2f m of the control)",
                cfg.grab == PS2_TOUCH_GRAB_GRIP ? "the grip" : cfg.grab == PS2_TOUCH_GRAB_TRIGGER ? "the trigger"
                : "either the grip or the trigger", cfg.grab_near);
    if (cfg.autom)
        ps2_log("touch: AUTO: %s (ACZ_TOUCH_AUTO_GRAB); a press on Touch makes it the active controller beside a pad: trigger / "
                "grip 0.65, thumbstick 0.60, a button; held 50 ms, 0.5 s after a controller binds, not while the pad is in use",
                !cfg.auto_grab ? "a hand on the controls does nothing" : cfg.auto_grab == 2 ? "a hand closing at the stick or "
                "throttle (the grip too only there) takes it" : "a hand closing on the stick or throttle by the GRAB WITH rule "
                "takes it");
    if (cfg.mode != PS2_TOUCH_OFF || cfg.test)
        ps2_log("touch: left thumbstick thrust %s; TOUCH CONTROLS card %s, Menu held %.1f s in flight%s; hints %s",
                cfg.thumb_thrust ? "on" : "off (ACZ_TOUCH_THUMB_THRUST=0)",
                cfg.card == 0 ? "off" : cfg.card == 2 ? "at every start" : "once (the first HANDS ON STICK session)",
                cfg.card_hold, cfg.card_hold > 0.0f ? "" : " (off)", cfg.hints ? "on" : "off");
}
int ps2_touch_mode(void) { cfg_init(); return cfg.autom ? PS2_TOUCH_AUTO : cfg.mode; }
static int muted;                    /* AUTO while a gamepad is the active controller (ps2_touch_set_muted) */
static int mode_now(void) { return cfg.autom && muted ? PS2_TOUCH_OFF : cfg.mode; }
void ps2_touch_get_info(ps2_touch_info *o) {
    cfg_init();
    o->mode = mode_now(); o->automatic = cfg.autom; o->thr = cfg.thr; o->rs_look = cfg.rs_look; o->swap = cfg.swap;
    o->thumb_thrust = cfg.thumb_thrust && cfg.thr != THR_STICK; o->card_hold_ms = (int)(cfg.card_hold * 1000.0f + 0.5f);
    o->toggle = cfg.toggle; o->grab = cfg.grab;
}

static double now_s(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

/* ---- quaternions (x, y, z, w) ---- */
static void q_mul(const float *a, const float *b, float *o) {
    float r[4];
    r[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    r[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    r[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    r[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    memcpy(o, r, sizeof r);
}
static void q_conj(const float *a, float *o) { o[0] = -a[0]; o[1] = -a[1]; o[2] = -a[2]; o[3] = a[3]; }
static void q_rot(const float *q, const float *v, float *o) {     /* v + 2w (u x v) + 2 u x (u x v) */
    float c1[3] = { q[1] * v[2] - q[2] * v[1], q[2] * v[0] - q[0] * v[2], q[0] * v[1] - q[1] * v[0] };
    float c2[3] = { q[1] * c1[2] - q[2] * c1[1], q[2] * c1[0] - q[0] * c1[2], q[0] * c1[1] - q[1] * c1[0] };
    for (int i = 0; i < 3; i++) o[i] = v[i] + 2.0f * (q[3] * c1[i] + c2[i]);
}
static void q_axis(int axis, float deg, float *o) {               /* rotation about x / y / z by deg */
    float h = deg * 0.00872665f;
    o[0] = o[1] = o[2] = 0.0f; o[axis] = sinf(h); o[3] = cosf(h);
}

/* ---- 1-euro filter (Casiez et al. 2012): a low-pass whose cutoff rises with the speed ---- */
typedef struct { int init; float x, dx; } euro;
static float lp_alpha(float cutoff, float dt) { float tau = 1.0f / (6.2831853f * cutoff); return 1.0f / (1.0f + tau / dt); }
static float euro_step(euro *e, float x, float dt) {
    if (cfg.f_min <= 0.0f) return x;
    if (!e->init || dt <= 0.0f) { e->init = 1; e->x = x; e->dx = 0.0f; return x; }
    if (dt > 0.1f) dt = 0.1f;
    e->dx += lp_alpha(1.0f, dt) * ((x - e->x) / dt - e->dx);
    e->x += lp_alpha(cfg.f_min + cfg.f_beta * fabsf(e->dx), dt) * (x - e->x);
    return e->x;
}

/* ---- this poll's input (roles: R.h[0] = the throttle hand, R.h[1] = the stick hand; ACZ_TOUCH_SWAP swaps them) ---- */
static ps2_touch_raw R, Rloc;                 /* R: the poll's states; Rloc: the newest located poses (physical) */
static int have_loc;
static double t_now, t_prev;
static float dt;
static int flying, used;
static double last_use;
static unsigned ref_seen;
static u64 field_now;
static double sync_ms_sum, sync_ms_max; static unsigned sync_n;   /* ps2_xr_touch_sync's host time (ACZ_DEBUG_TOUCH) */

/* ---- PS2_XR_TEST_TOUCH="field:hand:key=value,... field~hand:key=value,..." (deterministic like PS2_XR_TEST_HEAD_SCRIPT):
   hand L or R (physical); from that vblank field on the hand is synthetic (active, tracked, focused) and each entry sets
   the keys it names, the others keep their value; '~' ramps the continuous keys linearly from the previous entry of
   that hand, reached at this field. Keys: grip trig (0..1), sx sy (thumbstick -1..1), pitch roll yaw (degrees: + =
   the controller's top tilted back / to the right / its nose turned right, applied in that order in the cockpit frame
   on a base pose with the handle 40 deg up), px py pz (metres added to the base position: R (0.20, -0.42, -0.30),
   L (-0.22, -0.42, -0.28); + = right / up / back), a|x (lower button) b|y (upper) click menu (0/1), ttouch thtouch
   (capacitive 0/1), rot pos (tracking valid 0/1), act (controller present 0/1), focus (session focused 0/1). ---- */
enum { TK_GRIP, TK_TRIG, TK_SX, TK_SY, TK_PITCH, TK_ROLL, TK_YAW, TK_PX, TK_PY, TK_PZ, TK_CONT,
       TK_LO = TK_CONT, TK_UP, TK_CLICK, TK_MENU, TK_TTOUCH, TK_THTOUCH, TK_ROT, TK_POS, TK_ACT, TK_FOCUS, TK_N };
static const char *const tk_names[TK_N] = { "grip", "trig", "sx", "sy", "pitch", "roll", "yaw", "px", "py", "pz",
                                            "a", "b", "click", "menu", "ttouch", "thtouch", "rot", "pos", "act", "focus" };
#define TT_MAX 192
static struct { long f; int hand, ramp; unsigned mask; float v[TK_N]; } tt[TT_MAX];
static int tt_n, tt_parsed, tt_cur[2] = { -2, -2 };
static void tt_parse(void) {
    tt_parsed = 1;
    const char *p = getenv("PS2_XR_TEST_TOUCH");
    while (p && *p && tt_n < TT_MAX) {
        char *end;
        while (*p == ' ') p++;
        if (!*p) break;
        long f = strtol(p, &end, 10);
        if (end == p || (*end != ':' && *end != '~')) break;
        tt[tt_n].f = f; tt[tt_n].ramp = *end == '~';
        p = end + 1;
        if ((*p != 'L' && *p != 'R' && *p != 'l' && *p != 'r') || p[1] != ':') break;
        tt[tt_n].hand = *p == 'R' || *p == 'r';
        p += 2;
        tt[tt_n].mask = 0;
        while (*p && *p != ' ') {
            char key[16]; int n = 0;
            while (*p && *p != '=' && *p != ' ' && *p != ',' && n < 15) key[n++] = *p++;
            key[n] = 0;
            if (*p != '=') { while (*p && *p != ' ' && *p != ',') p++; if (*p == ',') p++; continue; }
            float v = strtof(p + 1, &end);
            p = end;
            int k = -1;
            if (!strcmp(key, "x")) k = TK_LO; else if (!strcmp(key, "y")) k = TK_UP;
            for (int i = 0; i < TK_N && k < 0; i++) if (!strcmp(key, tk_names[i])) k = i;
            if (k >= 0) { tt[tt_n].mask |= 1u << k; tt[tt_n].v[k] = v; }
            else ps2_log("touch: PS2_XR_TEST_TOUCH: unknown key '%s'", key);
            while (*p && *p != ' ' && *p != ',') p++;
            if (*p == ',') p++;
        }
        tt_n++;
    }
    if (tt_n) ps2_log("touch: test controllers, %d scripted entries (PS2_XR_TEST_TOUCH)", tt_n);
}
/* the synthetic state of `hand` at field vb; 0 = the script has not started for it */
static int tt_eval(int hand, long vb, float *cur, int *entry) {
    static const float def[TK_N] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1 };
    memcpy(cur, def, sizeof def);
    int started = 0; long fprev = 0;
    *entry = -1;
    for (int i = 0; i < tt_n; i++) {
        if (tt[i].hand != hand) continue;
        if (vb >= tt[i].f) {
            for (int k = 0; k < TK_N; k++) if (tt[i].mask & (1u << k)) cur[k] = tt[i].v[k];
            started = 1; fprev = tt[i].f; *entry = i;
            continue;
        }
        if (tt[i].ramp && started && tt[i].f > fprev) {
            float t = (float)(vb - fprev) / (float)(tt[i].f - fprev);
            for (int k = 0; k < TK_CONT; k++) if (tt[i].mask & (1u << k)) cur[k] += (tt[i].v[k] - cur[k]) * t;
        }
        break;
    }
    return started;
}
static void tt_pose(int hand, const float *c, float *q, float *p) {
    static const float base_p[2][3] = { { -0.22f, -0.42f, -0.28f }, { 0.20f, -0.42f, -0.30f } };
    float b[4], a[4], t[4];
    q_axis(0, 40.0f, b);                                    /* the handle 40 deg up from forward */
    q_axis(0, c[TK_PITCH], a); q_mul(a, b, t);              /* + = top back */
    q_axis(2, -c[TK_ROLL], a); q_mul(a, t, t);              /* + = top right (about the forward axis) */
    q_axis(1, -c[TK_YAW], a); q_mul(a, t, q);               /* + = nose right */
    p[0] = base_p[hand][0] + c[TK_PX]; p[1] = base_p[hand][1] + c[TK_PY]; p[2] = base_p[hand][2] + c[TK_PZ];
}
static int tt_focus = 1;                                    /* the script's focus key (any hand) */
static void tt_apply(ps2_touch_raw *r, int poses_only) {
    if (!tt_parsed) tt_parse();
    if (!tt_n) return;
    long vb = (long)ps2_kernel_vblank_count();
    float c[TK_N];
    int any = 0, entry;
    tt_focus = 1;
    for (int h = 0; h < 2; h++) {
        if (!tt_eval(h, vb, c, &entry)) continue;
        any = 1;
        ps2_touch_hand_raw *x = &r->h[h];
        tt_pose(h, c, x->q, x->p);
        memcpy(x->aq, x->q, sizeof x->aq); memcpy(x->ap, x->p, sizeof x->ap);
        x->rot_ok = x->rot_trk = c[TK_ROT] != 0.0f;
        x->pos_ok = x->pos_trk = c[TK_POS] != 0.0f;
        if (c[TK_FOCUS] == 0.0f) tt_focus = 0;
        if (poses_only) continue;
        x->active = c[TK_ACT] != 0.0f;
        x->squeeze = c[TK_GRIP]; x->trigger = c[TK_TRIG]; x->sx = c[TK_SX]; x->sy = c[TK_SY];
        x->lower = c[TK_LO] != 0.0f; x->upper = c[TK_UP] != 0.0f; x->click = c[TK_CLICK] != 0.0f;
        x->menu = c[TK_MENU] != 0.0f;
        x->trig_touch = c[TK_TTOUCH] != 0.0f; x->thumb_touch = c[TK_THTOUCH] != 0.0f;
        r->profile[h] = PS2_TOUCH_PROFILE_TEST;
        if (entry != tt_cur[h]) {
            tt_cur[h] = entry;
            ps2_log("touch: test %s hand field %ld entry %d: grip %.2f trig %.2f stick %.2f,%.2f pitch %.1f roll %.1f "
                    "yaw %.1f pos %+.3f,%+.3f,%+.3f buttons %d%d%d%d rot %d pos %d act %d focus %d",
                    h ? "right" : "left", vb, entry, c[TK_GRIP], c[TK_TRIG], c[TK_SX], c[TK_SY], c[TK_PITCH], c[TK_ROLL],
                    c[TK_YAW], c[TK_PX], c[TK_PY], c[TK_PZ], x->lower, x->upper, x->click, x->menu, x->rot_ok, x->pos_ok,
                    x->active, tt_focus);
        }
    }
    if (any && !poses_only) { r->on = 1; r->focused = tt_focus; }
}

/* ---- grabs ---- */
typedef struct {
    int btn, tog, held, easing;
    int btn_t;              /* (the trigger, as btn for the grip) */
    int by;                 /* which button closed the hand: 1 the grip, 2 the trigger (ACZ_TOUCH_GRAB) */
    int near_ctl;           /* this hand is near its control now */
    float near_d;           /* (how far, metres) */
    float q0[4], fwd[3], rgt[3], dlast[4];
    euro f[3];
    float a[3];             /* filtered tilt (deg): pull (+ = top back), roll (+ = top right), twist (+ = nose right) */
    float out[3];           /* the stick's physical deflection, fraction of the throw (with the release easing) */
    float rel[3];           /* out at the release */
    double t_rel, lost_t;
} grab_t;
static grab_t G[2];                                          /* role 0 throttle (left), 1 stick (right) */
static unsigned stick_grabs;                                 /* this session (the "take the stick" hint stops) */
static float throw_now = 20.0f;

static void grab_frame(grab_t *g, const float *q) {
    float mz[3] = { 0, 0, -1 }, y[3] = { 0, 1, 0 }, t[3], u[3];
    q_rot(q, mz, t); q_rot(q, y, u);
    float f[3] = { t[0] - u[0], 0.0f, t[2] - u[2] };          /* the hand's heading: the handle's forward minus its up */
    float l = sqrtf(f[0] * f[0] + f[2] * f[2]);
    if (cfg.frame_cockpit || l < 0.1f) { f[0] = 0.0f; f[2] = -1.0f; l = 1.0f; }
    g->fwd[0] = f[0] / l; g->fwd[1] = 0.0f; g->fwd[2] = f[2] / l;
    g->rgt[0] = -g->fwd[2]; g->rgt[1] = 0.0f; g->rgt[2] = g->fwd[0];   /* fwd x up */
    memcpy(g->q0, q, sizeof g->q0);
}
/* the tilt since the grab (deg): pull, roll, twist about the grab frame (rotation vector of q * q0^-1, cockpit frame) */
static void grab_tilt(grab_t *g, const float *q, float *a) {
    float c[4], d[4];
    q_conj(g->q0, c); q_mul(q, c, d);
    if (d[3] < 0.0f) { d[0] = -d[0]; d[1] = -d[1]; d[2] = -d[2]; d[3] = -d[3]; }
    memcpy(g->dlast, d, sizeof d);
    float s = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    float w[3] = { 0, 0, 0 };
    if (s > 1e-7f) { float k = 2.0f * atan2f(s, d[3]) / s * 57.29578f; w[0] = d[0] * k; w[1] = d[1] * k; w[2] = d[2] * k; }
    a[0] = w[0] * g->rgt[0] + w[1] * g->rgt[1] + w[2] * g->rgt[2];
    a[1] = w[0] * g->fwd[0] + w[1] * g->fwd[1] + w[2] * g->fwd[2];
    a[2] = -w[1];
}
/* ACZ_TOUCH_GRAB: does this hand want its control closed? The grip's thresholds for both buttons, the hold or tap rule
   (ACZ_TOUCH_GRIP) for the button that closed it. at_ctl: the controller is near the control (hand_near) */
static int grab_want(grab_t *g, float sq, float tr, int can, int at_ctl) {
    g->near_ctl = at_ctl;
    if (!can) { g->btn = g->btn_t = 0; g->tog = 0; return 0; }
    int was = g->btn, was_t = g->btn_t;
    g->btn = g->btn ? sq > 0.35f : sq > 0.65f;
    g->btn_t = g->btn_t ? tr > 0.35f : tr > 0.65f;
    int rise = g->btn && !was, rise_t = g->btn_t && !was_t;
    if (cfg.grab == PS2_TOUCH_GRAB_EITHER && !g->held) {     /* free: which button closes it */
        if (cfg.toggle && g->tog) return 1;                  /* (a tap still waiting for the tracking) */
        if (rise || (g->btn && !cfg.toggle)) { g->by = 1; if (rise) g->tog = 1; return 1; }
        if (rise_t && at_ctl) { g->by = 2; g->tog = 1; return 1; }
        if (rise_t) {                                        /* (for the headset test: how far off it was) */
            static int said;
            if (said < 12) {
                said++;
                ps2_log("touch: %s trigger pressed %.2f m from its control: not near enough to grab (ACZ_TOUCH_GRAB_NEAR %.2f)",
                        g == &G[1] ? "stick hand's" : "throttle hand's", g->near_d, cfg.grab_near);
            }
        }
        return 0;
    }
    if (cfg.grab != PS2_TOUCH_GRAB_EITHER) g->by = cfg.grab == PS2_TOUCH_GRAB_TRIGGER ? 2 : 1;
    if (g->by == 2) {
        if (rise_t) g->tog = !g->tog;
        return cfg.toggle ? g->tog : g->btn_t;
    }
    if (rise) g->tog = !g->tog;
    return cfg.toggle ? g->tog : g->btn;
}
/* the controller near its control: the model's grip point (ps2_ctl.stick_grip / throttle_grip), or in a model without
   gloves on its controls (the fallback gloves: none known) a generic place, hand and seat, a little wider */
static int hand_near(grab_t *g, const ps2_touch_hand_raw *h, int stick) {
#ifdef TC_CTL
    g->near_d = 99.0f;
    if (!h->pos_ok) return 0;
    const float *c = stick ? ps2_ctl.stick_grip : ps2_ctl.throttle_grip;
    static const float gen[2][3] = { { -0.28f, -0.46f, -0.30f }, { 0.05f, -0.48f, -0.33f } };   /* (throttle, stick) */
    int known = c[0] != 0.0f || c[1] != 0.0f || c[2] != 0.0f;
    const float *p = known ? c : gen[stick != 0];
    float r = known ? cfg.grab_near : cfg.grab_near * 1.5f;
    float dx = h->p[0] - p[0], dy = h->p[1] - p[1], dz = h->p[2] - p[2];
    g->near_d = sqrtf(dx * dx + dy * dy + dz * dz);
    return g->near_d <= r;
#else
    (void)g; (void)h; (void)stick;
    return 1;
#endif
}
/* the same hand's other button does the trigger's job while the trigger holds; a free hand near its control does not
   fire or brake with the trigger (ACZ_TOUCH_GRAB) */
static float trigger_job(const grab_t *g, const ps2_touch_hand_raw *h) {
    if (cfg.grab == PS2_TOUCH_GRAB_GRIP) return h->trigger;
    if (cfg.grab == PS2_TOUCH_GRAB_TRIGGER || (g->held && g->by == 2)) return h->squeeze;
    if (!g->held && g->near_ctl) return 0.0f;
    return h->trigger;
}
static const char *grab_by_name(const grab_t *g) { return g->by == 2 ? "the trigger" : "the grip"; }
static void grab_release(grab_t *g, int role, const char *why) {
    if (!g->held) return;
    g->held = 0; g->easing = 1; g->t_rel = t_now;
    memcpy(g->rel, g->out, sizeof g->rel);
    if (role == 1 || cfg.thr == THR_TILT)
        ps2_log("touch: %s released (%s) at pull %.1f roll %.1f deg", role ? "stick" : "throttle", why, g->a[0], g->a[1]);
}

/* ---- the throttle lever: position x in cm along the cockpit, 0 = CRUISE ---- */
static float lever_x, lever_x0, lever_f0, lever_lastf, lever_rel_x;
static double lever_rel_t;
static int lever_pos_lost, lever_zone = -99, lever_springing;
#define GATE_CM 1.5f
#define CRUISE_CM 0.4f
static float lever_thr(float x) {
    float r = cfg.lever_cm;
    if (x <= -r) return -0.6f - 0.4f * clampf_((-x - r) / (0.5f * r), 0.0f, 1.0f);   /* the air brake: half a range more */
    if (x < -CRUISE_CM) return -0.6f * (-x - CRUISE_CM) / (r - CRUISE_CM);
    if (x <= CRUISE_CM) return 0.0f;                                                    /* sticky CRUISE */
    if (x < r) return 0.9f * (x - CRUISE_CM) / (r - CRUISE_CM);
    if (x < r + GATE_CM) return 0.9f;                                                   /* MIL, the gate */
    return 1.0f;                                                                        /* AB */
}
static int lever_zone_of(float x) {
    float r = cfg.lever_cm;
    return x <= -r ? 0 : x < -CRUISE_CM ? 1 : x <= CRUISE_CM ? 2 : x < r ? 3 : x < r + GATE_CM ? 4 : 5;
}
static const char *const zone_names[6] = { "AIR BRAKE", "IDLE-CRUISE", "CRUISE", "CRUISE-MIL", "MIL", "AB" };

/* ---- haptics (physical hands) ---- */
static float rum_lo, rum_hi, ab_hum;
static double ab_t0, gun_until, gun_next;
static struct { double busy_until, last_t, last_cue; float last; } hs[2];
static u64 hap_calls, hap_cues;
static int swap_on(void) { return cfg.swap && cfg.mode == PS2_TOUCH_STICK; }   /* ACZ_TOUCH_SWAP: stick mode only */
static int phys(int role) { return swap_on() ? 1 - role : role; }
static void cue(int role, float amp, float secs, const char *what) {
    int h = phys(role);
    if (cfg.haptics <= 0.0f || t_now - hs[h].last_cue < 0.05) return;     /* >= 50 ms between pulses */
    float a = clampf_(amp * cfg.haptics / 0.5f, 0.0f, 1.0f);
    ps2_xr_touch_haptic(h, a, secs);
    hs[h].busy_until = t_now + secs; hs[h].last_cue = hs[h].last_t = t_now; hs[h].last = a;
    hap_calls++; hap_cues++;
    if (cfg.dbg) ps2_log("touch: haptic cue %s on the %s hand: %.2f for %.0f ms", what, h ? "right" : "left", a, secs * 1e3);
}
static void haptics_update(void) {
    float want[2] = { 0.0f, 0.0f };
    if (used && cfg.haptics > 0.0f) {
        float lo = rum_lo * cfg.haptics, hi = rum_hi * cfg.haptics;
        want[phys(0)] = lo;
        want[phys(1)] = lo > hi ? lo : hi;
    }
    if (ab_hum > 0.0f) {                                    /* AB lit: a faint rumble on the throttle hand, fading */
        float a = 0.12f * (1.0f - (float)(t_now - ab_t0) / 2.0f);
        if (a <= 0.0f) ab_hum = 0.0f;
        else if (a * cfg.haptics / 0.5f > want[phys(0)]) want[phys(0)] = a * cfg.haptics / 0.5f;
    }
    for (int h = 0; h < 2; h++) {
        if (t_now < hs[h].busy_until) continue;
        float a = clampf_(want[h], 0.0f, 1.0f);
        if (a > 0.02f) {
            if (fabsf(a - hs[h].last) > 0.05f || t_now - hs[h].last_t >= 0.08) {
                ps2_xr_touch_haptic(h, a, 0.12f);
                hs[h].last = a; hs[h].last_t = t_now; hap_calls++;
            }
        } else if (hs[h].last > 0.0f) {
            ps2_xr_touch_haptic(h, 0.0f, 0.0f);
            hs[h].last = 0.0f; hs[h].last_t = t_now;
        }
    }
}
void ps2_touch_rumble(float lo, float hi) { rum_lo = lo; rum_hi = hi; }

/* ---- outputs ---- */
static struct {                                             /* the virtual Xbox pad (PAD mapping) */
    int axis[SDL_GAMEPAD_AXIS_COUNT];
    unsigned long long btn, menu_btn;                       /* btn: all; menu_btn: View/Menu only (stick mapping) */
    float sh[2];
    int binds, live;
} vp;
static struct {                                             /* stick mapping (flying): DualShock values after the bindings */
    int on, stick, look, thr_set, yaw_set, autopilot;
    u8 lx, ly, rx, ry;
    float thr, yaw;
    u16 buttons;
} so;

typedef struct { int down; double t0; int chord, fired; double pulse_until; } tbtn;
static tbtn b_menu, b_lc, b_rc, b_sel;
static double r3_until, sel_until, start_until, back_until;
static int flick = -1;

static int flick_dir(float x, float y) {
    float m = sqrtf(x * x + y * y);
    int dom = fabsf(x) > fabsf(y) ? (x > 0 ? 1 : 3) : (y > 0 ? 0 : 2);   /* 0 up, 1 right, 2 down, 3 left */
    if (flick < 0) { if (m > 0.7f) flick = dom; }
    else if (m < 0.4f) flick = -1;
    else if (m > 0.7f && dom != flick) flick = dom;
    return flick;
}
static const int flick_sdl[4] = { SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_DPAD_DOWN,
                                  SDL_GAMEPAD_BUTTON_DPAD_LEFT };
static const int flick_ps2[4] = { PS2_ACT_UP, PS2_ACT_RIGHT, PS2_ACT_DOWN, PS2_ACT_LEFT };

static int edge(tbtn *b, int down) {                        /* 1 press, -1 release, 0 none */
    if (down && !b->down) { b->down = 1; b->t0 = t_now; b->fired = 0; return 1; }
    if (!down && b->down) { b->down = 0; return -1; }
    return 0;
}
#define VB(b) (1ull << (b))

/* the shared button logic: Menu (tap = Start on release, held 0.5 s = View + Menu), the stick clicks (chord = L3) */
static void buttons_common(int stick_map, int focus) {
    ps2_touch_hand_raw *L = &R.h[0];
    int menu = focus && (R.h[0].menu || R.h[1].menu);
    int e = edge(&b_menu, menu);
    if (e < 0 && t_now - b_menu.t0 < 0.5) start_until = t_now + 0.06;
    if (stick_map && cfg.card_hold > 0.0f) {                 /* in flight: held 1 s = the TOUCH CONTROLS card (bug #239) */
        if (b_menu.down && !b_menu.fired && t_now - b_menu.t0 >= cfg.card_hold) {
            b_menu.fired = 1;
            ps2_log("touch: Menu held %.1f s in flight: the TOUCH CONTROLS card", cfg.card_hold);
            ps2_quit_touch_card_open(PS2_TC_FROM_FLIGHT);
        }
    } else if (b_menu.down && !b_menu.fired && t_now - b_menu.t0 >= 0.5)
        vp.menu_btn |= VB(SDL_GAMEPAD_BUTTON_BACK) | VB(SDL_GAMEPAD_BUTTON_START);
    int el = edge(&b_lc, focus && L->click), er = edge(&b_rc, focus && R.h[1].click);
    if (b_lc.down && b_rc.down) b_lc.chord = b_rc.chord = 1;              /* both clicks: L3, neither of the others */
    if (er < 0) {                                                         /* the right click released: R3 */
        if (!b_rc.chord && t_now - b_rc.t0 < 1.0) r3_until = t_now + 0.06;
        b_rc.chord = 0;
    }
    if (el < 0) {                                                         /* the left click released: View tap (map) */
        if (!stick_map && !b_lc.chord && !b_lc.fired && t_now - b_lc.t0 < 0.5) back_until = t_now + 0.06;
        b_lc.chord = 0;
    }
    if (!stick_map && b_lc.down && !b_lc.chord && !b_lc.fired && t_now - b_lc.t0 >= 1.0) {
        b_lc.fired = 1; ps2_xr_request_recenter(); ps2_log("touch: recentre (left thumbstick held)");
    }
    if (stick_map) {                                         /* Y: the map on a tap, recentre when held 1 s */
        int ey = edge(&b_sel, focus && L->upper);
        if (ey < 0 && !b_sel.fired && t_now - b_sel.t0 < 0.5) sel_until = t_now + 0.06;
        if (b_sel.down && !b_sel.fired && t_now - b_sel.t0 >= 1.0) {
            b_sel.fired = 1; ps2_xr_request_recenter(); ps2_log("touch: recentre (Y held)");
        }
    } else edge(&b_sel, 0);
}

static void map_pad(int focus) {
    ps2_touch_hand_raw *L = &R.h[0], *Rh = &R.h[1];
    vp.binds = 1;
    if (!focus) return;
    vp.axis[SDL_GAMEPAD_AXIS_LEFTX] = (int)lroundf(clampf_(L->sx, -1.0f, 1.0f) * 32767.0f);
    vp.axis[SDL_GAMEPAD_AXIS_LEFTY] = (int)lroundf(clampf_(-L->sy, -1.0f, 1.0f) * 32767.0f);
    vp.axis[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] = (int)lroundf(clampf_(L->trigger, 0.0f, 1.0f) * 32767.0f);
    vp.axis[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] = (int)lroundf(clampf_(Rh->trigger, 0.0f, 1.0f) * 32767.0f);
    if (cfg.rs_look) {
        vp.axis[SDL_GAMEPAD_AXIS_RIGHTX] = (int)lroundf(clampf_(Rh->sx, -1.0f, 1.0f) * 32767.0f);
        vp.axis[SDL_GAMEPAD_AXIS_RIGHTY] = (int)lroundf(clampf_(-Rh->sy, -1.0f, 1.0f) * 32767.0f);
    } else {
        int d = flick_dir(Rh->sx, Rh->sy);
        if (d >= 0) vp.btn |= VB(flick_sdl[d]);
    }
    for (int h = 0; h < 2; h++) {                            /* grips: bumpers, analog above the resting hand */
        static int sh_btn[2];
        float g = R.h[h].squeeze;
        vp.sh[h] = clampf_((g - cfg.grip_rest) / (0.9f - cfg.grip_rest), 0.0f, 1.0f);
        sh_btn[h] = sh_btn[h] ? g > cfg.grip_rest + 0.15f : g > cfg.grip_rest + 0.3f;
        if (sh_btn[h]) vp.btn |= VB(h ? SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER : SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    }
    if (r3_until > t_now) vp.btn |= VB(SDL_GAMEPAD_BUTTON_RIGHT_STICK);
    if (b_lc.down && b_rc.down) vp.btn |= VB(SDL_GAMEPAD_BUTTON_LEFT_STICK);
    if (Rh->lower) vp.btn |= VB(SDL_GAMEPAD_BUTTON_SOUTH);
    if (Rh->upper) vp.btn |= VB(SDL_GAMEPAD_BUTTON_EAST);
    if (L->lower) vp.btn |= VB(SDL_GAMEPAD_BUTTON_WEST);
    if (L->upper) vp.btn |= VB(SDL_GAMEPAD_BUTTON_NORTH);
}

/* game bytes for a normalised -1..1 value past the game's per-axis dead zone (128 +- (64 + 63 |s|); 127 = centre) */
static u8 game_byte(float s) {
    if (s == 0.0f) return PS2_PAD_ANALOG_NEUTRAL;
    int d = (int)cfg.game_dz + (int)lroundf((127.0f - cfg.game_dz) * fabsf(s));
    int v = s > 0.0f ? 128 + d : 128 - d;
    return (u8)(v < 0 ? 0 : v > 255 ? 255 : v);
}
static float shape(float a, float thr) {                     /* tilt (deg) -> -1..1: inner dead zone, expo */
    float m = fabsf(a), dz = cfg.dz_deg;
    if (m <= dz || thr <= dz) return 0.0f;
    float s = clampf_((m - dz) / (thr - dz), 0.0f, 1.0f);
    s = (1.0f - cfg.expo) * s + cfg.expo * s * s * s;
    return a < 0.0f ? -s : s;
}

static void stick_hand(int focus) {
    ps2_touch_hand_raw *h = &R.h[1];
    grab_t *g = &G[1];
    int can = focus && h->active;
    int want = grab_want(g, h->squeeze, h->trigger, can, hand_near(g, h, 1));
#ifdef TC_CTL
    if (want && !g->held && cfg.reach > 0.0f && ps2_ctl.cockpit_has_stick && h->pos_ok) {   /* ACZ_TOUCH_REACH */
        float dx = h->p[0] - ps2_ctl.stick_grip[0], dy = h->p[1] - ps2_ctl.stick_grip[1], dz = h->p[2] - ps2_ctl.stick_grip[2];
        if (dx * dx + dy * dy + dz * dz > cfg.reach * cfg.reach) want = 0;
    }
#endif
    if (want && !g->held && h->rot_ok) {
        grab_frame(g, h->q);
        g->held = 1; g->easing = 0; g->lost_t = 0.0;
        memset(g->f, 0, sizeof g->f); memset(g->a, 0, sizeof g->a);
        ps2_log("touch: stick grabbed (field %llu; heading %.0f deg; by %s, %.2f m from its control)", (unsigned long long)field_now,
                atan2f(-g->fwd[0], -g->fwd[2]) * 57.29578f, grab_by_name(g), g->near_d);
        stick_grabs++;
    } else if (!want && g->held) grab_release(g, 1, can ? (g->by == 2 ? "trigger" : "grip") : !focus ? "focus lost" : "controller inactive");
    if (g->held) {
        if (!h->rot_ok) {                                    /* orientation lost: hold 0.3 s, then ease to centre */
            if (g->lost_t == 0.0) {
                g->lost_t = t_now; memcpy(g->rel, g->out, sizeof g->rel);
                ps2_log("touch: stick orientation not tracked (field %llu): holding", (unsigned long long)field_now);
            }
            float u = (float)(t_now - g->lost_t - 0.3) / (cfg.release_s > 0.05f ? cfg.release_s : 0.05f);
            if (u > 0.0f) {
                float k = u >= 1.0f ? 0.0f : 1.0f - u * u * (3.0f - 2.0f * u);
                for (int i = 0; i < 3; i++) { g->out[i] = g->rel[i] * k; g->a[i] = g->out[i] * throw_now; }
            }
        } else {
            float a[3];
            if (g->lost_t != 0.0) {                          /* back: on from the eased value, relative to the grab */
                for (int k = 0; k < 3; k++) g->f[k].x = g->a[k];
                ps2_log("touch: stick orientation tracked again after %.2f s", t_now - g->lost_t);
                g->lost_t = 0.0;
            }
            grab_tilt(g, h->q, a);
            for (int k = 0; k < 3; k++) g->a[k] = euro_step(&g->f[k], a[k], dt);
            for (int k = 0; k < 3; k++) g->out[k] = clampf_(g->a[k] / throw_now, -1.0f, 1.0f);
        }
    }
    if (!g->held && g->easing) {                             /* released: ease to centre (smoothstep) */
        float u = cfg.release_s > 0.0f ? (float)(t_now - g->t_rel) / cfg.release_s : 1.0f;
        if (u >= 1.0f) { g->easing = 0; memset(g->out, 0, sizeof g->out); memset(g->a, 0, sizeof g->a); }
        else {
            float k = 1.0f - u * u * (3.0f - 2.0f * u);
            for (int i = 0; i < 3; i++) { g->out[i] = g->rel[i] * k; g->a[i] = g->out[i] * throw_now; }
        }
    }
    if ((cfg.cues & CUE_CENTRE) && g->held) {                /* the stick crossing its centre: a faint tick */
        static int side[2];
        for (int k = 0; k < 2; k++) {
            int s = g->a[k] > cfg.dz_deg ? 1 : g->a[k] < -cfg.dz_deg ? -1 : side[k];
            if (s != side[k] && side[k] != 0) cue(1, 0.2f, 0.005f, "stick centre");
            side[k] = s;
        }
    }
}

static void throttle_hand(int focus) {
    ps2_touch_hand_raw *h = &R.h[0];
    grab_t *g = &G[0];
    int can = focus && h->active;
    if (cfg.thr == THR_STICK) { g->held = 0; g->near_ctl = 0; return; }
    int want = grab_want(g, h->squeeze, h->trigger, can, hand_near(g, h, 0));
#ifdef TC_CTL
    if (want && !g->held && cfg.reach > 0.0f && ps2_ctl.cockpit_has_throttle && h->pos_ok) {
        float dx = h->p[0] - ps2_ctl.throttle_grip[0], dy = h->p[1] - ps2_ctl.throttle_grip[1],
              dz = h->p[2] - ps2_ctl.throttle_grip[2];
        if (dx * dx + dy * dy + dz * dz > cfg.reach * cfg.reach) want = 0;
    }
#endif
    int ready = cfg.thr == THR_TILT ? h->rot_ok : h->pos_ok;
    if (want && !g->held && ready) {
        g->held = 1; g->easing = 0;
        lever_x0 = lever_x; lever_f0 = lever_lastf = -h->p[2]; lever_pos_lost = 0; lever_springing = 0;
        grab_frame(g, h->q);
        memset(g->f, 0, sizeof g->f);
        ps2_log("touch: throttle grabbed at %s (lever %.1f cm, field %llu; by %s, %.2f m from its control)", zone_names[lever_zone_of(lever_x)], lever_x,
                (unsigned long long)field_now, grab_by_name(g), g->near_d);
    } else if (!want && g->held) {
        g->held = 0;
        ps2_log("touch: throttle released (%s) at %s, lever %.1f cm", can ? (g->by == 2 ? "trigger" : "grip") : !focus ? "focus lost" : "controller inactive",
                zone_names[lever_zone_of(lever_x)], lever_x);
        if (cfg.thr == THR_SPRING) { lever_springing = 1; lever_rel_x = lever_x; lever_rel_t = t_now; }
    }
    float r = cfg.lever_cm, lo = -1.5f * r, hi = r + GATE_CM + 0.5f;
    if (g->held) {
        if (cfg.thr == THR_TILT) {
            if (h->rot_ok) {
                float a[3];
                grab_tilt(g, h->q, a);
                float pull = euro_step(&g->f[0], a[0], dt);
                lever_x = clampf_(lever_x0 - pull * r / 25.0f, lo, hi);          /* 25 deg forward = CRUISE -> MIL */
            }
        } else if (!h->pos_ok) lever_pos_lost = 1;            /* freeze while the position is not tracked */
        else {
            float f = -h->p[2];
            float jump = 2.0f * dt > 0.03f ? 2.0f * dt : 0.03f;  /* faster than 2 m/s: a tracking jump */
            if (lever_pos_lost || fabsf(f - lever_lastf) > jump) {     /* back, or a jump: carry on from here */
                if (!lever_pos_lost) ps2_log("touch: throttle hand jumped %.1f cm in one poll: ignored", (f - lever_lastf) * 100.0f);
                lever_x0 = lever_x; lever_f0 = f; lever_pos_lost = 0;
            }
            lever_lastf = f;
            lever_x = clampf_(lever_x0 + (f - lever_f0) * 100.0f, lo, hi);
        }
    }
    if (!g->held && lever_springing) {                       /* SPRING: back to cruise over 0.3 s */
        float u = (float)(t_now - lever_rel_t) / 0.3f;
        if (u >= 1.0f) { lever_x = 0.0f; lever_springing = 0; }
        else lever_x = lever_rel_x * (1.0f - u * u * (3.0f - 2.0f * u));
    }
    int z = lever_zone_of(lever_x);
    if (z != lever_zone) {
        if (lever_zone != -99 && g->held) {
            if (z == 5 && (cfg.cues & CUE_AB)) { cue(0, 1.0f, 0.02f, "AB gate"); ab_hum = 1.0f; ab_t0 = t_now + 0.02; }
            else if (cfg.cues & CUE_DETENT) {
                int k = z > lever_zone ? z : lever_zone;    /* the boundary crossed */
                if (k == 1) cue(0, 0.5f, 0.015f, "IDLE detent");
                else if (z == 2) cue(0, 0.35f, 0.012f, "CRUISE detent");   /* into the cruise notch only */
                else if (k == 4) cue(0, 0.6f, 0.015f, "MIL detent");
                else if (k == 5) cue(0, 0.5f, 0.015f, "out of AB");
            }
            ps2_log("touch: throttle lever %s -> %s (%.1f cm, thr %.2f)", zone_names[lever_zone], zone_names[z], lever_x,
                    lever_thr(lever_x));
        }
        if (z != 5) ab_hum = 0.0f;
        lever_zone = z;
    }
}

/* gun pulses while the trigger fires the gun (the round count does not drop in every mission: M1 shows 10000), a
   thump when the missile or special-weapon count drops; both on the stick hand */
static void weapon_cues(void) {
    static unsigned last_frame; static int msl0 = -1, sp0 = -1;
    zero_instr in;
    if ((cfg.cues & CUE_GUN) && (so.buttons & (1u << PS2_ACT_CROSS))) gun_until = t_now + 0.03;
    if ((cfg.cues & CUE_MISSILE) && ps2_vk_instr_get && ps2_vk_instr_get(&in) && in.valid && in.frame != last_frame) {
        last_frame = in.frame;
        if ((msl0 >= 0 && in.missiles < msl0 && msl0 - in.missiles <= 4) || (sp0 >= 0 && in.special < sp0 && sp0 - in.special <= 4))
            cue(1, 0.7f, 0.06f, "missile launch");
        msl0 = in.missiles; sp0 = in.special;
    }
    if ((cfg.cues & CUE_GUN) && t_now < gun_until && t_now >= gun_next) {
        cue(1, 0.35f, 0.025f, "gun");
        gun_next = t_now + 0.083;                            /* ~12 Hz */
    }
}

static void map_stick(int focus) {
    ps2_touch_hand_raw *L = &R.h[0], *Rh = &R.h[1];
    so.on = 1;
    /* the stick */
    grab_t *g = &G[1];
    if (g->held || g->easing) {
        so.stick = 1;
        so.lx = game_byte(shape(g->a[1], throw_now));
        so.ly = game_byte(shape(g->a[0], throw_now));        /* LY > 128 = pulled back = nose up */
    }
    /* the throttle: a lever keeps its place while the headset is not focused; the thumbstick springs back */
    if (cfg.thr != THR_STICK || focus) {
        float thr;
        if (cfg.thr == THR_STICK) {                          /* left thumbstick Y, spring (PS2 semantics) */
            float y = fabsf(L->sy) > fabsf(L->sx) ? L->sy : 0.0f;
            float m = clampf_((fabsf(y) - 0.15f) / 0.8f, 0.0f, 1.0f);
            thr = y > 0.0f ? (m >= 0.97f ? 1.0f : m * 0.9f / 0.97f) : -m;
        } else {
            thr = lever_thr(lever_x);
            if (cfg.thumb_thrust && focus) {                 /* bug #239: the left thumbstick up / down while held */
                static int thumb_ab;
                float y = fabsf(L->sy) > fabsf(L->sx) ? L->sy : 0.0f;
                float m = clampf_((fabsf(y) - 0.15f) / 0.8f, 0.0f, 1.0f);
                if (m > 0.0f && y > 0.0f) { float t = m >= 0.97f ? 1.0f : 0.9f * m / 0.97f; if (t > thr) thr = t; }
                else if (m > 0.0f && -m < thr) thr = -m;
                if (thr >= 0.999f && lever_thr(lever_x) < 0.999f && !thumb_ab && (cfg.cues & CUE_AB)) {
                    cue(0, 1.0f, 0.02f, "AB (left thumbstick)"); thumb_ab = 1;
                } else if (thr < 0.999f) thumb_ab = 0;
            }
        }
        if (focus) {                                         /* left trigger: brake (ACZ_TOUCH_GRAB: trigger_job) */
            float t = clampf_((trigger_job(&G[0], L) - 0.05f) / 0.9f, 0.0f, 1.0f);
            if (t > 0.0f && -t < thr) thr = -t;
        }
        so.thr = thr; so.thr_set = 1;
    }
    if (!focus) return;
    if (cfg.rs_look) {
        so.look = 1;
        so.rx = (u8)clampf_(127.5f + Rh->sx * 127.5f, 0.0f, 255.0f);
        so.ry = (u8)clampf_(127.5f - Rh->sy * 127.5f, 0.0f, 255.0f);
    } else {
        int d = flick_dir(Rh->sx, Rh->sy);
        if (d >= 0) so.buttons |= (u16)(1u << flick_ps2[d]);
    }
    {   static int trig;                                     /* trigger = gun (Cross; ACZ_TOUCH_GRAB: trigger_job) */
        float tj = trigger_job(g, Rh);
        trig = trig ? tj > 0.35f : tj > 0.55f;
        if (trig) so.buttons |= 1u << PS2_ACT_CROSS;
    }
    if (Rh->lower) so.buttons |= 1u << PS2_ACT_CIRCLE;      /* A = missile */
    if (Rh->upper) so.buttons |= 1u << PS2_ACT_TRIANGLE;    /* B = target */
    if (L->lower) so.buttons |= 1u << PS2_ACT_SQUARE;       /* X = weapon switch */
    /* yaw: left thumbstick X (and the wrist twist with ACZ_TOUCH_STICK_TWIST) */
    float x = (cfg.thr == THR_STICK || cfg.thumb_thrust) && fabsf(L->sy) >= fabsf(L->sx) ? 0.0f : L->sx;
    float yaw = (x < 0.0f ? -1.0f : 1.0f) * clampf_((fabsf(x) - 0.15f) / 0.8f, 0.0f, 1.0f);
    if (cfg.twist && g->held) { float t = shape(g->a[2], throw_now); if (fabsf(t) > fabsf(yaw)) yaw = t; }
    so.yaw = yaw; so.yaw_set = 1;
    so.autopilot = b_lc.down && !b_lc.chord;                 /* left click: autopilot (L2 + R2) */
    if (sel_until > t_now) so.buttons |= 1u << PS2_ACT_SELECT;
    if (r3_until > t_now) so.buttons |= 1u << PS2_ACT_R3;
    if (b_lc.down && b_rc.down) so.buttons |= 1u << PS2_ACT_L3;
}

/* thr -> L1 / R1 pressure (the game: thr = (1.5 R1 - L1) x 1.176 / 255; AB only at thr 1) */
static void thr_bytes(float thr, u8 *l1, u8 *r1) {
    *l1 = *r1 = 0;
    if (thr >= 0.999f) *r1 = 255;
    else if (thr > 0.0f) *r1 = (u8)clampf_(roundf(thr * 144.6f), 1.0f, 144.0f);
    else if (thr <= -0.999f) *l1 = 255;
    else if (thr < 0.0f) *l1 = (u8)clampf_(roundf(-thr * 216.8f), 1.0f, 254.0f);
}
static float pad_thr(const ps2_pad_state *st) {
    float r1 = (st->buttons >> PS2_ACT_R1) & 1 ? st->r1 : 0.0f, l1 = (st->buttons >> PS2_ACT_L1) & 1 ? st->l1 : 0.0f;
    return clampf_((1.5f * r1 - l1) * 1.176f / 255.0f, -1.0f, 1.0f);
}

static int dev(u8 v) { return abs((int)v - 128); }
static struct { int stick, thr, rudder; } won;               /* what Touch decided in the last merge (the cockpit) */
static ps2_pad_state last_out;

void ps2_touch_apply(ps2_pad_state *st) {
    won.stick = won.thr = won.rudder = 0;
    if (start_until > t_now) st->buttons |= 1u << PS2_ACT_START;
    if (so.on) {
        if (so.stick) {
            int a = dev(so.lx) >= dev(st->lx), b = dev(so.ly) >= dev(st->ly);
            if (a) st->lx = so.lx;
            if (b) st->ly = so.ly;
            won.stick = a || b;
        }
        if (so.look) {
            if (dev(so.rx) > dev(st->rx)) st->rx = so.rx;
            if (dev(so.ry) > dev(st->ry)) st->ry = so.ry;
        }
        if (so.thr_set && fabsf(so.thr) >= fabsf(pad_thr(st))) {
            u8 l1, r1;
            thr_bytes(so.thr, &l1, &r1);
            st->l1 = l1; st->r1 = r1;
            st->buttons = (u16)((st->buttons & ~((1u << PS2_ACT_L1) | (1u << PS2_ACT_R1)))
                                | (l1 ? 1u << PS2_ACT_L1 : 0u) | (r1 ? 1u << PS2_ACT_R1 : 0u));
            won.thr = 1;
        }
        if (so.autopilot) {
            st->l2 = st->r2 = 255;
            st->buttons |= (1u << PS2_ACT_L2) | (1u << PS2_ACT_R2);
            won.rudder = 1;
        } else if (so.yaw_set) {
            float r2 = (st->buttons >> PS2_ACT_R2) & 1 ? st->r2 : 0.0f, l2 = (st->buttons >> PS2_ACT_L2) & 1 ? st->l2 : 0.0f;
            if (so.yaw != 0.0f && fabsf(so.yaw) * 255.0f >= fabsf(r2 - l2)) {
                u8 p = (u8)clampf_(roundf(fabsf(so.yaw) * 255.0f), 1.0f, 255.0f);
                st->buttons &= (u16)~((1u << PS2_ACT_L2) | (1u << PS2_ACT_R2));
                st->l2 = st->r2 = 0;
                if (so.yaw > 0.0f) { st->r2 = p; st->buttons |= 1u << PS2_ACT_R2; }
                else { st->l2 = p; st->buttons |= 1u << PS2_ACT_L2; }
                won.rudder = 1;
            }
        }
        st->buttons |= so.buttons;
    }
    last_out = *st;
}

void ps2_touch_stick_remap(ps2_pad_state *st, const float *ls) {
    (void)ls;
    cfg_init();
    if (!cfg.remap) return;
    u8 *v[2] = { &st->lx, &st->ly };
    for (int k = 0; k < 2; k++) {
        int d = (int)*v[k] - 127;
        float s = clampf_(((float)abs(d) - 4.0f) / 124.0f, 0.0f, 1.0f);    /* a 4-count inner dead zone */
        *v[k] = s > 0.0f ? game_byte(d < 0 ? -s : s) : PS2_PAD_ANALOG_NEUTRAL;
    }
}

int ps2_touch_vpad_live(void) { return vp.live; }
/* the controller registry (ps2_video.c, ctl_priority) */
static unsigned long long in_bits;
unsigned long long ps2_touch_input_bits(void) { return in_bits; }

/* ---- AUTO: the controllers before the mute (bugs #314, #315): what counts as a press. A controller on a desk, one waking
   or going to sleep, a little stick drift or a resting hand on the grip must not take control from a pad player. A
   control fires only from below its release level: a control already beyond it when the controller binds (a thing
   lying on it, the press that woke it) stays disarmed until released. ---- */
enum { TP_TRIG, TP_GRIP, TP_STICK, TP_LOWER, TP_UPPER, TP_CLICK, TP_MENU, TP_N };
enum { TPS_DISARMED, TPS_ARMED, TPS_RISING, TPS_DOWN };
static const char *const tp_names[TP_N] = { "trigger", "grip", "thumbstick", "lower button", "upper button", "thumbstick click", "menu" };
static const float tp_press[TP_N] = { 0.65f, 0.65f, 0.60f, 0.5f, 0.5f, 0.5f, 0.5f };
static const float tp_release[TP_N] = { 0.35f, 0.35f, 0.30f, 0.5f, 0.5f, 0.5f, 0.5f };
#define TP_SETTLE_S 0.5                              /* after a controller binds: nothing fires */
#define TP_HOLD_S 0.05                               /* a press is this long: no single-sample spike */
static struct {
    unsigned char st[TP_N];
    double t0[TP_N], bound;                          /* bound: since when this hand reports (0: it does not) */
    float peak[3];                                   /* (the log at unbinding) trigger, grip, thumbstick */
    unsigned presses, buttons;
} tp[2];
static unsigned tp_fired[2];                         /* controls that became presses in this poll */
static int pad_in_use;                               /* ps2_touch_set_pad_in_use: the registry's word */
static char take_why[48] = "input";
const char *ps2_touch_input_why(void) { return take_why; }
void ps2_touch_set_pad_in_use(int on) { pad_in_use = on != 0; }
static void tp_scan(int focus) {
    for (int h = 0; h < 2; h++) {
        const ps2_touch_hand_raw *x = &R.h[h];
        tp_fired[h] = 0;
        if (!focus || !x->active) {
            if (tp[h].bound > 0.0 && (muted || tp[h].presses))   /* (what the runtime reported: bug #315's open question) */
                ps2_log("touch: AUTO: the %s controller stopped reporting after %.1f s%s: peaks trigger %.2f grip %.2f thumbstick "
                        "%.2f, %u button presses, %u presses counted", h ? "right" : "left", t_now - tp[h].bound,
                        muted ? " (muted)" : "", tp[h].peak[0], tp[h].peak[1], tp[h].peak[2], tp[h].buttons, tp[h].presses);
            tp[h].bound = 0.0;
            memset(tp[h].st, TPS_DISARMED, sizeof tp[h].st);
            continue;
        }
        if (tp[h].bound <= 0.0) { tp[h].bound = t_now; memset(tp[h].peak, 0, sizeof tp[h].peak); tp[h].presses = tp[h].buttons = 0; }
        float m = fabsf(x->sx) > fabsf(x->sy) ? fabsf(x->sx) : fabsf(x->sy);
        float v[TP_N] = { x->trigger, x->squeeze, m, x->lower ? 1.0f : 0.0f, x->upper ? 1.0f : 0.0f, x->click ? 1.0f : 0.0f,
                          x->menu ? 1.0f : 0.0f };
        for (int k = 0; k < 3; k++) if (v[k] > tp[h].peak[k]) tp[h].peak[k] = v[k];
        int settled = t_now - tp[h].bound >= TP_SETTLE_S;
        for (int c = 0; c < TP_N; c++) {
            unsigned char *st = &tp[h].st[c];
            if (v[c] <= tp_release[c]) { *st = TPS_ARMED; continue; }
            if (!settled) continue;
            if (*st == TPS_ARMED && v[c] >= tp_press[c]) { *st = TPS_RISING; tp[h].t0[c] = t_now; }
            if (*st == TPS_RISING) {
                if (v[c] < tp_press[c]) *st = TPS_ARMED;
                else if (t_now - tp[h].t0[c] >= TP_HOLD_S) {
                    if (c >= TP_LOWER) tp[h].buttons++;
                    if (muted && pad_in_use) {                /* the pad in the hands: no press; release first */
                        static int said;
                        *st = TPS_DISARMED;
                        if (said < 12) {
                            said++;
                            ps2_log("touch: AUTO: %s %s pressed while the pad is in use or forced: not a switch", h ? "right" : "left", tp_names[c]);
                        }
                    } else { *st = TPS_DOWN; tp_fired[h] |= 1u << c; tp[h].presses++; }
                }
            }
        }
    }
}
/* the registry's "input" while muted, in its bit layout (ps2_touch_input_bits) */
static unsigned long long tp_bits(void) {
    static const int bit[2][TP_N] = { { 32 + 2 * SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 44, 32 + 2 * SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_BUTTON_WEST,
                                        SDL_GAMEPAD_BUTTON_NORTH, SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_START },
                                      { 32 + 2 * SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 45, 32 + 2 * SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_BUTTON_SOUTH,
                                        SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_RIGHT_STICK, SDL_GAMEPAD_BUTTON_GUIDE } };
    unsigned long long b = 0;
    for (int h = 0; h < 2; h++)
        for (int c = 0; c < TP_N; c++)
            if (tp[h].st[c] == TPS_DOWN) b |= 1ull << bit[h][c];
    return b;
}
int ps2_touch_present(void) {
    cfg_init();
    return (cfg.mode != PS2_TOUCH_OFF || cfg.test) && (R.on || cfg.test);
}
void ps2_touch_set_muted(int on) {
    cfg_init();
    on = on != 0;
    if (!cfg.autom || on == muted) return;
    muted = on;
    if (on)                                          /* what is held as the pad takes over is no new press */
        for (int h = 0; h < 2; h++)
            for (int c = 0; c < TP_N; c++)
                if (tp[h].st[c] >= TPS_RISING) tp[h].st[c] = TPS_DISARMED;
    ps2_log("touch: AUTO: %s", on ? "muted (a gamepad is the active controller)"
            : cfg.mode == PS2_TOUCH_STICK ? "hands on stick (Touch is the active controller)" : "as a pad (no gamepad in use)");
}
int ps2_touch_vpad_axis(int axis, int binds, int *value) {
    if (!vp.live || !vp.binds || !binds || axis < 0 || axis >= SDL_GAMEPAD_AXIS_COUNT) return 0;
    *value = vp.axis[axis];
    return vp.axis[axis] != 0;
}
int ps2_touch_vpad_button(int b, int binds) {
    if (!vp.live || b < 0 || b >= 64) return 0;
    unsigned long long m = binds ? (vp.binds ? vp.btn | vp.menu_btn : 0ull) : vp.btn | vp.menu_btn;
    return (m >> b) & 1ull;
}
float ps2_touch_vpad_shoulder(int right, int binds) {
    if (!vp.live || (binds && !vp.binds)) return 0.0f;
    return vp.sh[right ? 1 : 0];
}

static void ctl_fill(void);
static void debug_line(void);

void ps2_touch_set_haptics(float v) {
    cfg_init();
    cfg.haptics = clampf_(v, 0.0f, 1.0f);
    ps2_log("touch: vibration level %.2f (ENHANCEMENTS > RUMBLE STRENGTH)", (double)cfg.haptics);
}
void ps2_touch_on_locate(const ps2_touch_raw *r) {
    cfg_init();
    Rloc = *r;
    {   static int seen;                                     /* the first valid pose: CONTROLS shows the Touch rows */
        if (!seen && (r->h[0].rot_ok || r->h[0].pos_ok || r->h[1].rot_ok || r->h[1].pos_ok)) {
            extern void ps2_enh_touch_rows(int on);          /* ps2_settings.c */
            seen = 1;
            ps2_enh_touch_rows(1);
            ps2_log("touch: a controller's pose is valid: ENHANCEMENTS > CONTROLS shows the Touch rows");
        }
    }
    if (tt_n || !tt_parsed) tt_apply(&Rloc, 1);
    have_loc = 1;
#ifdef TC_CTL
    for (int h = 0; h < 2; h++) {                            /* the newest poses, for the hands drawn this frame */
        ps2_controls_hand *c = &ps2_ctl.hand[h];
        const ps2_touch_hand_raw *x = &Rloc.h[h];
        memcpy(c->q, x->q, sizeof c->q); memcpy(c->p, x->p, sizeof c->p);
        memcpy(c->aq, x->aq, sizeof c->aq); memcpy(c->ap, x->ap, sizeof c->ap);
        c->rot_ok = x->rot_ok; c->pos_ok = x->pos_ok;
    }
#endif
}

/* bug #239: the TOUCH CONTROLS card by itself once (the first HANDS ON STICK session with both controllers bound and the
   headset focused: 1.5 s in a menu, 2.5 s into a flight, which it pauses; ACZ_TOUCH_CARD), and the hint to take the
   stick when the stick hand's controller is waved or tilted more than 25 deg within a second in flight without the
   stick held (at most 3 times, 20 s apart, until the stick has been taken twice; ACZ_TOUCH_HINTS) */
static void card_and_hints(int focus) {
    static int asked, since_kind, tries;
    static double since, asked_t;
    if (asked && cfg.card == 1 && !ps2_cfg.touch_card_seen && tries < 5 && t_now - asked_t > 3.0
        && !ps2_quit_touch_card_visible()) asked = 0;   /* (closed before it was seen, e.g. a flight started under it) */
    if (cfg.card && !asked) {
        if (cfg.card == 1 && ps2_cfg.touch_card_seen) asked = 1;
        int gs = ps2_quit_game_state();
        int kind = flying && gs == PS2_QUIT_GAME_FLYING ? 1 : !flying && gs == PS2_QUIT_GAME_MENU && ps2_quit_menu_on_screen() ? 2 : 0;
        int ready = !asked && kind && focus && R.profile[0] != PS2_TOUCH_PROFILE_NONE
                    && R.profile[1] != PS2_TOUCH_PROFILE_NONE;      /* (a mission loading or its intro: neither) */
        if (!ready || kind != since_kind) { since = ready ? t_now : 0.0; since_kind = kind; }
        else if (t_now - since >= (kind == 1 ? 2.5 : 1.5)) {
            asked = 1; asked_t = t_now; tries++;
            ps2_log("touch: the TOUCH CONTROLS card by itself (%s; ACZ_TOUCH_CARD)",
                    cfg.card == 2 ? "every start" : "the first HANDS ON STICK session");
            ps2_quit_touch_card_open(PS2_TC_FROM_AUTO);
        }
    }
    static float qref[4];
    static double tref, last_hint;
    static int nhint;
    const ps2_touch_hand_raw *h = &R.h[1];
    if (!cfg.hints || !flying || !focus || !h->active || !h->rot_ok || G[1].held || stick_grabs >= 2u || nhint >= 3) {
        tref = 0.0;
        return;
    }
    if (tref == 0.0 || t_now - tref > 1.0) { memcpy(qref, h->q, sizeof qref); tref = t_now; return; }
    float d = fabsf(h->q[0] * qref[0] + h->q[1] * qref[1] + h->q[2] * qref[2] + h->q[3] * qref[3]);
    float ang = 2.0f * acosf(d > 1.0f ? 1.0f : d) * 57.29578f;
    if (ang > 25.0f && (last_hint == 0.0 || t_now - last_hint > 20.0)) {
        last_hint = t_now; nhint++; tref = 0.0;
        ps2_log("touch: hint: the stick hand moved %.0f deg in flight without holding the stick (%d of 3)", ang, nhint);
        ps2_quit_touch_hint("TAKE THE STICK", cfg.grab == PS2_TOUCH_GRAB_TRIGGER
                            ? (swap_on() ? "Hold the LEFT trigger, then tilt the controller"
                                         : "Hold the RIGHT trigger, then tilt the controller")
                            : swap_on() ? "Squeeze and hold the LEFT grip, then tilt the controller"
                                        : "Squeeze and hold the RIGHT grip, then tilt the controller", 4.0);
    }
}

/* ---- AUTO, before the mute and the poll's role swap (physical hands in R): a hand closing on its control by the GRAB WITH
   rule, in flight: the session is HANDS ON STICK from here. Only a press that tp_scan counted, so the same rules as the
   switch; the grab itself is then the ordinary stick_hand / throttle_hand of this poll (the press is a rising edge
   for grab_want, since it did not run while the session was a pad). ---- */
static void auto_take(int focus) {
    if (!cfg.autom || cfg.mode != PS2_TOUCH_PAD || !cfg.auto_grab || !focus) return;
    if (!(ps2_imm_in[3] > 0.0f && (float)field_now - ps2_imm_in[3] <= 8.0f)) return;   /* (not flying: pad buttons in a menu) */
    for (int role = 1; role >= 0; role--) {
        if (role == 0 && cfg.thr == THR_STICK) continue;
        int hand = cfg.swap ? 1 - role : role;
        const ps2_touch_hand_raw *h = &R.h[hand];
        grab_t *g = &G[role];
        unsigned f = tp_fired[hand];
        if (!h->active || !(f & ((1u << TP_TRIG) | (1u << TP_GRIP)))) continue;
        int at = hand_near(g, h, role);
        int by = 0;
        if (f & (1u << TP_GRIP)) by = cfg.grab != PS2_TOUCH_GRAB_TRIGGER && (cfg.auto_grab == 1 || at) ? 1 : 0;
        if (!by && (f & (1u << TP_TRIG)))
            by = cfg.grab == PS2_TOUCH_GRAB_TRIGGER ? (cfg.auto_grab == 1 || at ? 2 : 0)
               : cfg.grab == PS2_TOUCH_GRAB_EITHER && at ? 2 : 0;
        if (by && cfg.reach > 0.0f && g->near_d > cfg.reach) by = 0;
        int ready = role == 1 || cfg.thr == THR_TILT ? h->rot_ok : h->pos_ok;
        if (!by || !ready) {
            static int said;                          /* (for the headset test: how far off it was) */
            if (said < 12) {
                said++;
                ps2_log("touch: AUTO: %s hand's %s pressed %.2f m from the %s: no grab (GRAB WITH %s, ACZ_TOUCH_GRAB_NEAR %.2f%s)",
                        hand ? "right" : "left", f & (1u << TP_GRIP) ? "grip" : "trigger", g->near_d, role ? "stick" : "throttle",
                        cfg.grab == PS2_TOUCH_GRAB_GRIP ? "grip" : cfg.grab == PS2_TOUCH_GRAB_TRIGGER ? "trigger" : "either",
                        cfg.grab_near, ready ? "" : "; not tracked");
            }
            continue;
        }
        ps2_log("touch: AUTO -> HANDS ON STICK (grab, %s hand; the %s, %.2f m from the %s, field %llu)", hand ? "right" : "left",
                by == 2 ? "trigger" : "grip", g->near_d, role ? "stick" : "throttle", (unsigned long long)field_now);
        cfg.mode = PS2_TOUCH_STICK;
        muted = 0;                                    /* (the registry follows in this poll: its reason is the grab) */
        snprintf(take_why, sizeof take_why, "grab");
        ps2_quit_touch_hud_line_clear();
        for (int r = 0; r < 2; r++) {                 /* what grab_want sees next: this press as a rising edge, the rest held */
            const ps2_touch_hand_raw *o = &R.h[cfg.swap ? 1 - r : r];
            G[r].btn = o->squeeze > 0.65f;
            G[r].btn_t = o->trigger > 0.35f;
            G[r].tog = 0;
        }
        if (by == 2) G[role].btn_t = 0;
        else G[role].btn = 0;
        return;
    }
}
/* the one line, once a session: a hand coming within reach of the stick without pressing (AUTO, not muted, as a pad, flying;
   it was seen out of reach first: a hand resting there from the start, a pad-style player's lap, gets no line) */
static void auto_hint(int focus) {
    static int shown, was_out;
    if (shown || !cfg.hints || !cfg.autom || muted || cfg.mode != PS2_TOUCH_PAD || !cfg.auto_grab || !focus) return;
    if (!(ps2_imm_in[3] > 0.0f && (float)field_now - ps2_imm_in[3] <= 8.0f)) return;
    if (ps2_quit_game_state() != PS2_QUIT_GAME_FLYING) return;
    const ps2_touch_hand_raw *h = &R.h[cfg.swap ? 0 : 1];
    if (!h->active || !h->pos_ok) return;
    if (!hand_near(&G[1], h, 1)) { was_out = 1; return; }
    if (!was_out || h->trigger > 0.35f || h->squeeze > 0.35f) return;
    shown = 1;
    ps2_log("touch: AUTO: a hand %.2f m from the stick without pressing: the line, once (ACZ_TOUCH_HINTS)", G[1].near_d);
    ps2_quit_touch_hud_line(cfg.grab == PS2_TOUCH_GRAB_GRIP ? "SQUEEZE THE GRIP TO TAKE THE STICK"
                            : "PULL THE TRIGGER TO TAKE THE STICK", 3.0);
}

void ps2_touch_update(void) {
    cfg_init();
    if (cfg.mode == PS2_TOUCH_OFF && !cfg.test) return;
    t_prev = t_now ? t_now : now_s();
    t_now = now_s();
    dt = (float)(t_now - t_prev);
    field_now = ps2_kernel_vblank_count();
    memset(&R, 0, sizeof R);
    if (ps2_xr_mode() >= 0) {
        double t0 = now_s();
        ps2_xr_touch_sync(&R);
        double ms = (now_s() - t0) * 1e3;
        sync_ms_sum += ms; sync_n++;
        if (ms > sync_ms_max) sync_ms_max = ms;
    }
    tt_apply(&R, 0);
    if (cfg.autom) { tp_scan(R.on && R.focused); auto_take(R.on && R.focused); }   /* bugs #314, #315 */
    if (swap_on()) { ps2_touch_hand_raw t = R.h[0]; R.h[0] = R.h[1]; R.h[1] = t; int p = R.profile[0]; R.profile[0] = R.profile[1]; R.profile[1] = p; }
    if (cfg.mode == PS2_TOUCH_OFF) cfg.mode = PS2_TOUCH_PAD;   /* (a test script without ACZ_TOUCH: pad mode) */
    {   static int pf = -1, pp[2] = { -1, -1 };
        if (R.focused != pf || R.profile[0] != pp[0] || R.profile[1] != pp[1]) {
            if (pf == 1 && !R.focused && cfg.autopause && flying && (G[0].held || G[1].held)) {
                start_until = t_now + 0.08; ps2_log("touch: focus lost while flying: pause (ACZ_TOUCH_AUTOPAUSE)");
            }
            static const char *const pn[5] = { "none", "khr/simple", "oculus/touch", "meta/touch_plus", "test script" };
            ps2_log("touch: %s, profiles left %s right %s", R.focused ? "focused" : "not focused (no Touch input)",
                    pn[R.profile[0] >= 0 && R.profile[0] < 5 ? R.profile[0] : 0],
                    pn[R.profile[1] >= 0 && R.profile[1] < 5 ? R.profile[1] : 0]);
            pf = R.focused; pp[0] = R.profile[0]; pp[1] = R.profile[1];
        }
    }
    int focus = R.on && R.focused;
    flying = ps2_imm_in[3] > 0.0f && (float)field_now - ps2_imm_in[3] <= 8.0f;
    in_bits = 0;
    if (cfg.autom && muted) {                                /* AUTO with a gamepad in use: Touch says nothing to the game */
        if (G[0].held || G[1].held || G[1].easing) {         /* (a session that went HANDS ON STICK: the pad took over) */
            ps2_log("touch: AUTO: the pad took over: the hands let go (the lever back at CRUISE)");
            memset(&G, 0, sizeof G);
        }
        lever_x = 0.0f;
        in_bits = tp_bits();                                 /* ... but the registry sees its presses (bug #315) */
        if (in_bits) {
            for (int h = 0; h < 2; h++)
                for (int c = 0; c < TP_N; c++)
                    if (tp_fired[h] & (1u << c)) snprintf(take_why, sizeof take_why, "press, %s %s", h ? "right" : "left", tp_names[c]);
        }
        memset(&vp, 0, sizeof vp);
        memset(&so, 0, sizeof so);
        used = 0;
        if (hs[0].last > 0.0f || hs[1].last > 0.0f) {
            ps2_xr_touch_haptic(0, 0.0f, 0.0f); ps2_xr_touch_haptic(1, 0.0f, 0.0f);
            hs[0].last = hs[1].last = 0.0f;
        }
        ctl_fill();
        debug_line();
        return;
    }
#ifdef TC_CTL
    {   /* the cockpit's own stick throw (tc_cockpit): Su-27 13, MiG-29 23, J35J 25, F-14 / T-38 20 deg; a force stick
           (F-16C: 5 deg) or none keeps 20; clamped to 15..30 so a short-throw model does not make the hand twitchy */
        float c = ps2_ctl.cockpit_throw_deg;
        throw_now = cfg.throw_deg > 0.0f ? cfg.throw_deg : c >= 10.0f ? clampf_(c, 15.0f, 30.0f) : 20.0f;
    }
#else
    throw_now = cfg.throw_deg > 0.0f ? cfg.throw_deg : 20.0f;
#endif
    if (R.ref_gen != ref_seen) {                             /* recentred: keep each grab's deflection */
        ref_seen = R.ref_gen;
        for (int k = 0; k < 2; k++)
            if (G[k].held && R.h[k].rot_ok) {
                float c[4], q0[4];
                q_conj(G[k].dlast, c); q_mul(c, R.h[k].q, q0);
                grab_frame(&G[k], q0);
            }
        lever_pos_lost = 1;
    }
    for (int h = 0; h < 2; h++) {                            /* in use: anything off its rest (for the rumble) */
        ps2_touch_hand_raw *x = &R.h[h];
        if (focus && x->active && (x->trigger > 0.1f || x->squeeze > 0.35f || fabsf(x->sx) > 0.2f || fabsf(x->sy) > 0.2f
                                   || x->lower || x->upper || x->click || x->menu))
            last_use = t_now;
    }
    if (G[0].held || G[1].held) last_use = t_now;
    used = focus && last_use > 0.0 && t_now - last_use < 30.0;
    if (cfg.mode == PS2_TOUCH_STICK) card_and_hints(focus);
    auto_hint(focus);

    memset(&vp, 0, sizeof vp);
    memset(&so, 0, sizeof so);
    int stick_map = cfg.mode == PS2_TOUCH_STICK && flying;
    if (cfg.mode == PS2_TOUCH_STICK) { stick_hand(focus); throttle_hand(focus); }
    buttons_common(stick_map, focus);
    if (stick_map) { map_stick(focus); weapon_cues(); }
    else {
        map_pad(focus);
        if (start_until > t_now) vp.btn |= VB(SDL_GAMEPAD_BUTTON_START);
        if (back_until > t_now) vp.btn |= VB(SDL_GAMEPAD_BUTTON_BACK);
    }
    vp.live = vp.btn || vp.menu_btn || vp.sh[0] > 0.0f || vp.sh[1] > 0.0f;
    for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; a++) if (vp.axis[a]) vp.live = 1;
    if (focus) {                                             /* the registry's "input": beyond a quarter of the travel */
        in_bits = (vp.btn | vp.menu_btn) & 0xFFFFFFFFull;
        for (int a = 0; a < 6 && a < SDL_GAMEPAD_AXIS_COUNT; a++) {
            if (vp.axis[a] > 8191) in_bits |= 1ull << (32 + 2 * a);
            else if (vp.axis[a] < -8191) in_bits |= 1ull << (33 + 2 * a);
        }
        if (vp.sh[0] >= 0.25f) in_bits |= 1ull << 44;
        if (vp.sh[1] >= 0.25f) in_bits |= 1ull << 45;
        if (G[0].held) in_bits |= 1ull << 46;
        if (G[1].held) in_bits |= 1ull << 47;
    }
    if (R.on && !focus) {                                    /* not focused: stop the controllers' vibration */
        if (hs[0].last > 0.0f || hs[1].last > 0.0f) {
            ps2_xr_touch_haptic(0, 0.0f, 0.0f); ps2_xr_touch_haptic(1, 0.0f, 0.0f);
            hs[0].last = hs[1].last = 0.0f;
        }
    } else if (R.on) haptics_update();
    ctl_fill();
    debug_line();
}

static void ctl_fill(void) {
#ifdef TC_CTL
    ps2_controls *c = &ps2_ctl;
    c->seq++;
    c->touch_mode = mode_now() == PS2_TOUCH_STICK ? PS2_CTL_MODE_STICK : mode_now() == PS2_TOUCH_PAD ? PS2_CTL_MODE_PAD
                  : PS2_CTL_MODE_OFF;                        /* (AUTO muted: OFF, the hands as without Touch) */
    int stick_map = cfg.mode == PS2_TOUCH_STICK && flying;
    c->touch_stick = stick_map && (G[1].held || G[1].easing);
    c->touch_throttle = stick_map && cfg.thr != THR_STICK && won.thr;
    c->touch_rudder = stick_map && won.rudder;
    c->touch = c->touch_stick || c->touch_throttle || c->touch_rudder;
    c->stick_x = G[1].out[1];
    c->stick_y = G[1].out[0];
    c->rudder = so.autopilot ? 0.0f : so.yaw;
    float thr = so.thr_set ? so.thr : lever_thr(lever_x);
    float pw = clampf_(0.4f + thr * 2.0f / 3.0f, 0.0f, 1.0f);
    c->ab = thr >= 0.999f;
    c->throttle = 0.95f * pw + 0.05f * (float)c->ab;
    c->airbrake = clampf_((-thr - 0.6f) / 0.4f, 0.0f, 1.0f);
    c->stick_throw_deg = throw_now;
    for (int h = 0; h < 2; h++) {                            /* physical hands */
        int role = swap_on() ? 1 - h : h;
        ps2_controls_hand *o = &c->hand[h];
        const ps2_touch_hand_raw *x = &R.h[role];
        o->present = R.on && x->active;
        if (!have_loc) { o->rot_ok = x->rot_ok; o->pos_ok = x->pos_ok; memcpy(o->q, x->q, sizeof o->q); memcpy(o->p, x->p, sizeof o->p); }
        o->grab = cfg.mode != PS2_TOUCH_STICK || !G[role].held ? PS2_CTL_HAND_FREE
                : role ? PS2_CTL_HAND_STICK : PS2_CTL_HAND_THROTTLE;
        o->trigger = x->trigger; o->squeeze = x->squeeze;
        o->trigger_touch = x->trig_touch; o->thumb_touch = x->thumb_touch;
    }
#endif
}

static void debug_line(void) {
    static u64 last_f;
    if (!cfg.dbg || field_now - last_f < (u64)cfg.dbg) return;
    last_f = field_now;
    const ps2_touch_hand_raw *L = &R.h[0], *Rh = &R.h[1];
    ps2_log("touch: field %llu %s%s focus %d | R act %d rot %d pos %d sq %.2f tr %.2f st %+.2f,%+.2f %s pull %+.1f roll %+.1f "
            "twist %+.1f -> LX %u LY %u | L act %d pos %d sq %.2f tr %.2f st %+.2f,%+.2f %s lever %+.1f cm %s thr %+.2f yaw %+.2f%s "
            "| out btn %04X lx %u ly %u L1 %u R1 %u L2 %u R2 %u | vpad %d | throw %.0f | pose age %+.1f ms | haptic calls %llu cues %llu "
            "| sync %.3f ms mean (max %.3f) over %u polls",
            (unsigned long long)field_now, cfg.mode == PS2_TOUCH_STICK ? "stick" : "pad", flying ? " flying" : "",
            R.on && R.focused, Rh->active, Rh->rot_ok, Rh->pos_ok, Rh->squeeze, Rh->trigger, Rh->sx, Rh->sy,
            G[1].held ? "HELD" : G[1].easing ? "easing" : "free", G[1].a[0], G[1].a[1], G[1].a[2], so.lx, so.ly,
            L->active, L->pos_ok, L->squeeze, L->trigger, L->sx, L->sy, G[0].held ? "HELD" : "free", lever_x,
            zone_names[lever_zone_of(lever_x)], so.thr, so.yaw, so.autopilot ? " AUTOPILOT" : "",
            last_out.buttons, last_out.lx, last_out.ly, last_out.l1, last_out.r1, last_out.l2, last_out.r2, vp.live,
            throw_now, R.pose_age_ms, (unsigned long long)hap_calls, (unsigned long long)hap_cues,
            sync_n ? sync_ms_sum / sync_n : 0.0, sync_ms_max, sync_n);
    sync_ms_sum = sync_ms_max = 0.0; sync_n = 0;
#ifdef TC_CTL
    const ps2_controls *c = &ps2_ctl;
    ps2_log("touch: field %llu ctl seq %u mode %d touch %d (stick %d thr %d rudder %d) stick %+.2f,%+.2f rudder %+.2f "
            "throttle %.2f ab %d brake %.2f | L %d grab %d rot %d pos %d p %+.3f,%+.3f,%+.3f q %+.2f,%+.2f,%+.2f,%+.2f tt %d th %d "
            "| R %d grab %d rot %d pos %d p %+.3f,%+.3f,%+.3f q %+.2f,%+.2f,%+.2f,%+.2f tt %d th %d",
            (unsigned long long)field_now, c->seq, c->touch_mode, c->touch, c->touch_stick, c->touch_throttle,
            c->touch_rudder, c->stick_x, c->stick_y, c->rudder, c->throttle, c->ab, c->airbrake,
            c->hand[0].present, c->hand[0].grab, c->hand[0].rot_ok, c->hand[0].pos_ok, c->hand[0].p[0], c->hand[0].p[1],
            c->hand[0].p[2], c->hand[0].q[0], c->hand[0].q[1], c->hand[0].q[2], c->hand[0].q[3], c->hand[0].trigger_touch,
            c->hand[0].thumb_touch, c->hand[1].present, c->hand[1].grab, c->hand[1].rot_ok, c->hand[1].pos_ok,
            c->hand[1].p[0], c->hand[1].p[1], c->hand[1].p[2], c->hand[1].q[0], c->hand[1].q[1], c->hand[1].q[2],
            c->hand[1].q[3], c->hand[1].trigger_touch, c->hand[1].thumb_touch);
#endif
}
