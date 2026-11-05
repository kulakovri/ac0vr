#ifndef PS2_CONTROLS_H
#define PS2_CONTROLS_H
/* The pilot's controls as the 3D cockpit shows them (Touch S0-S2, wish #76). Renderer thread only, no lock: the Touch
   input (ps2_xr.c ps2_xr_frame_begin + ps2_video.c poll_input) writes the input fields once per renderer loop; the
   cockpit renderer (ps2_video.c cpit_values / cpit_anim_update / hands) reads them and writes the cockpit_* fields.
   Defined in ps2_video.c (cockpit animation area). All poses: the recentred reference frame ps2_xr_head_of() returns
   the head in (metres; x right, y up, z back; the cockpit model's frame, design eye at the origin). */
#define PS2_CTL_HAND_FREE     0   /* not holding anything (or Touch off) */
#define PS2_CTL_HAND_STICK    1   /* holding the stick */
#define PS2_CTL_HAND_THROTTLE 2   /* holding the throttle lever */
#define PS2_CTL_MODE_OFF      0   /* no Touch: the cockpit shows the game's own values, the gloves sit on the controls */
#define PS2_CTL_MODE_PAD      1   /* Touch as a pad: as OFF for the cockpit (the gloves stay on the controls) */
#define PS2_CTL_MODE_STICK    2   /* virtual stick/throttle: a FREE hand follows its grip pose */
typedef struct ps2_controls_hand {
    int   present;        /* controller connected and bound (an interaction profile is active) */
    int   rot_ok, pos_ok; /* orientation / position valid this frame (pos can drop in the lap) */
    int   grab;           /* PS2_CTL_HAND_* */
    float q[4];           /* GRIP pose orientation x, y, z, w */
    float p[3];           /* GRIP pose position, metres */
    float aq[4], ap[3];   /* AIM pose, same frame */
    float trigger;        /* index trigger 0..1 (index curl) */
    float squeeze;        /* grip 0..1 */
    int   trigger_touch;  /* 1 index finger on the trigger, 0 pointing, -1 unknown (no proximity) */
    int   thumb_touch;    /* 1 thumb on a button / thumbstick / rest, 0 thumb up, -1 unknown */
} ps2_controls_hand;
typedef struct ps2_controls {
    unsigned seq;         /* +1 per update by the Touch side (0 = never filled) */
    int   touch_mode;     /* PS2_CTL_MODE_* */
    int   touch;          /* 1 = Touch drives at least one of the controls below this frame */
    int   touch_stick, touch_throttle, touch_rudder;   /* which ones Touch drives now (else the game's own value) */
    float stick_x;        /* roll -1..1, + = stick top right: PHYSICAL deflection as a fraction of the throw (before
                             the game's 64/127 dead-zone remap) */
    float stick_y;        /* pitch -1..1, + = pulled back (nose up) */
    float rudder;         /* -1..1, + = right pedal forward (yaw right) */
    float throttle;       /* lever 0..1: 0 IDLE stop, 0.95 MIL, 1.0 through the AB gate (= 0.95 power + 0.05 ab,
                             power = 0.4 + 2/3 thr: cruise 0.38) */
    int   ab;             /* lever past the AB gate */
    float airbrake;       /* 0..1 */
    float stick_throw_deg;/* the throw the Touch stick uses (ACZ_TOUCH_STICK_THROW) */
    ps2_controls_hand hand[2];   /* 0 left, 1 right */
    /* written by the cockpit renderer when a cockpit model is in use (read by the Touch side) */
    unsigned cockpit_serial;     /* changes with the cockpit model in use (0 = none) */
    float cockpit_throw_deg;     /* the model's stick throw at full deflection (deg; 0 = unknown / no animated stick) */
    int   cockpit_has_stick, cockpit_has_throttle, cockpit_has_pedals;   /* animated in this model */
    float stick_grip[3];         /* the stick handle's grip point (the right glove's grip origin), metres, and */
    float throttle_grip[3];      /* the throttle lever's (the left glove's), both as modelled (the controls' rest pose) */
} ps2_controls;
extern ps2_controls ps2_ctl;
#endif
