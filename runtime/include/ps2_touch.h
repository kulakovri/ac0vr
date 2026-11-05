// ps2_touch.h - Quest Touch controllers as the pilot's controls (tc_touch, 2026-10-23; CONTROLS_RESEARCH_2026-10-23.md).
//
// ps2_xr.c owns the OpenXR side (action set, bindings, xrSyncActions, the grip/aim spaces, haptics) and hands the raw
// controller state to ps2_touch.c, which turns it into gamepad input: PAD mode = a virtual Xbox pad merged into the
// SDL pad (ps2_video.c pad_axis / pad_button), STICK mode = a virtual stick and throttle written straight into the
// DualShock 2 state after the bindings (ps2_touch_apply). Everything ends as DualShock 2 bytes, so the game, the
// PS2PAD2 recordings and replays are unchanged. Renderer thread only (poll_input, ps2_xr_frame_begin): no locks.
#ifndef PS2_TOUCH_H
#define PS2_TOUCH_H

#include "ps2_hle.h"

/* AUTO (ctl_priority, the default since 2026-11-03): AS A PAD while no gamepad is connected; muted (no input to the game,
   no vibration, the cockpit's hands as with OFF) while one is, unless Touch is the active controller: a press on it
   makes it so (bug #315), and a hand closing on the stick or throttle makes the session HANDS ON STICK (bug #314) */
enum { PS2_TOUCH_OFF = 0, PS2_TOUCH_PAD = 1, PS2_TOUCH_STICK = 2, PS2_TOUCH_AUTO = 3 };
enum { PS2_TOUCH_PROFILE_NONE = 0, PS2_TOUCH_PROFILE_SIMPLE, PS2_TOUCH_PROFILE_TOUCH, PS2_TOUCH_PROFILE_TOUCH_PLUS,
       PS2_TOUCH_PROFILE_TEST };

typedef struct ps2_touch_hand_raw {
    int   active;             /* a controller is bound to this hand and the session is focused (the actions report) */
    int   rot_ok, pos_ok;     /* grip orientation / position valid this frame (tracked or inferred) */
    int   rot_trk, pos_trk;   /* ... actually tracked */
    float q[4], p[3];         /* grip pose (x, y, z, w; metres) in the recentred reference frame: x right, y up, z back,
                                 origin = the head at the last recentre (the frame ps2_xr_head_of returns the head in) */
    float aq[4], ap[3];       /* aim pose, same frame */
    float trigger, squeeze;   /* 0..1 */
    float sx, sy;             /* thumbstick, + right, + up */
    int   click;              /* thumbstick click */
    int   lower, upper;       /* A / B on the right controller, X / Y on the left */
    int   menu;               /* left Menu button */
    int   trig_touch, thumb_touch;   /* capacitive: 1 / 0, -1 = the profile has no such sensor */
} ps2_touch_hand_raw;

typedef struct ps2_touch_raw {
    int   on;                 /* the action set is attached (ACZ_TOUCH set and an XR session) */
    int   focused;            /* the session is FOCUSED: input flows (else the dashboard or the headset is off) */
    int   profile[2];         /* PS2_TOUCH_PROFILE_* per hand (0 left, 1 right) */
    unsigned ref_gen;         /* +1 at every recentre (the reference frame moved: grabs rebase) */
    double pose_age_ms;       /* the poses' XrTime minus now when they were located (> 0: predicted ahead) */
    ps2_touch_hand_raw h[2];  /* 0 left, 1 right (physical) */
} ps2_touch_raw;

/* ps2_xr.c (renderer thread) */
int  ps2_xr_touch_sync(ps2_touch_raw *out);                /* xrSyncActions + the action states; the poses are those of
                                                              the last ps2_xr_frame_begin; 0 = no Touch input at all */
void ps2_xr_touch_haptic(int hand, float amplitude, float seconds);   /* physical hand 0 left / 1 right; 0 = stop */

/* ps2_touch.c (renderer thread) */
int   ps2_touch_mode(void);                                 /* ACZ_TOUCH: PS2_TOUCH_OFF / _PAD / _STICK / _AUTO */
void  ps2_touch_menv_reload(void);                         /* the NEXT MISSION tier (ps2_menv.h): the renderer, after the edge */
void  ps2_touch_on_locate(const ps2_touch_raw *r);          /* ps2_xr.c: the grip poses were located for this frame */
void  ps2_touch_update(void);                               /* poll_input, first: read the controllers, run the logic */
int   ps2_touch_vpad_live(void);                            /* the virtual Xbox pad has something to say */
int   ps2_touch_vpad_axis(int sdl_axis, int binds, int *value);   /* 1 = has a value for this SDL gamepad axis */
int   ps2_touch_vpad_button(int sdl_button, int binds);     /* the virtual pad's button (binds: for the bindings loop) */
float ps2_touch_vpad_shoulder(int right, int binds);        /* analog grip 0..1 for LEFT/RIGHT_SHOULDER bindings */
void  ps2_touch_apply(ps2_pad_state *st);                   /* poll_input, after the bindings: STICK mode's outputs */
void  ps2_touch_rumble(float lo, float hi);                 /* apply_rumble: the merged motor levels 0..1 */
void  ps2_touch_set_haptics(float v);                       /* ENHANCEMENTS > RUMBLE STRENGTH: ACZ_TOUCH_HAPTICS's level, now */
void  ps2_touch_stick_remap(ps2_pad_state *st, const float *ls); /* ACZ_PAD_STICK_REMAP (gamepad side item) */
/* the controller registry (ps2_video.c, ctl_priority): Touch as one of the devices */
int   ps2_touch_present(void);                              /* Touch can give input in this run (on, an XR session or
                                                               the test script) */
void  ps2_touch_set_muted(int on);                          /* AUTO: muted while a gamepad is connected and Touch is not
                                                               the active controller */
unsigned long long ps2_touch_input_bits(void);              /* the controls held beyond a quarter of their travel: SDL
                                                               buttons 0..31, axes 32 + 2a (+) / 33 + 2a (-), grips 44 /
                                                               45, the stick / throttle held 46 / 47 (AUTO muted: only
                                                               real presses, ps2_touch.c tp_scan; else 0 idle) */
const char *ps2_touch_input_why(void);                      /* the registry's reason for those: "grab", "press, right
                                                               trigger", or "input" outside AUTO */
void  ps2_touch_set_pad_in_use(int on);                     /* AUTO: a pad is in the hands or forced: a Touch press is
                                                               no switch and takes nothing */

/* what the TOUCH CONTROLS card (ps2_quit.c) draws: the mapping in use (any thread; fixed after start-up) */
enum { PS2_TOUCH_THR_LEVER, PS2_TOUCH_THR_SPRING, PS2_TOUCH_THR_TILT, PS2_TOUCH_THR_STICK };
enum { PS2_TOUCH_GRAB_GRIP, PS2_TOUCH_GRAB_TRIGGER, PS2_TOUCH_GRAB_EITHER };   /* ACZ_TOUCH_GRAB */
typedef struct ps2_touch_info {
    int mode;                 /* PS2_TOUCH_OFF / _PAD / _STICK in effect now (AUTO: _PAD, or _OFF while muted) */
    int automatic;            /* ACZ_TOUCH=auto (the default) */
    int thr;                  /* PS2_TOUCH_THR_*: the throttle hand's mode (ACZ_TOUCH_THROTTLE) */
    int rs_look;              /* the right thumbstick looks around (else D-pad flicks: wingman commands, menus) */
    int swap;                 /* left-handed: the stick in the left hand (STICK mode) */
    int thumb_thrust;         /* the left thumbstick's up / down is thrust / air brake while held (ACZ_TOUCH_THUMB_THRUST) */
    int card_hold_ms;         /* Menu held this long in flight opens the TOUCH CONTROLS card (0 off) */
    int toggle;               /* the grips take and let go on a tap (ACZ_TOUCH_GRIP=toggle) */
    int grab;                 /* which button closes a hand on a control (ACZ_TOUCH_GRAB: PS2_TOUCH_GRAB_*) */
} ps2_touch_info;
void  ps2_touch_get_info(ps2_touch_info *o);

#endif
