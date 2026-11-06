#ifndef ZERO_INSTR_H
#define ZERO_INSTR_H
/* Flight-instrument values of the player's aircraft (Ace Combat Zero), read once per game frame by a hook in zero_vr.c
   and used by the native cockpit (ps2_video.c) to drive needles. */
typedef struct zero_instr {
    int valid;
    unsigned frame;           /* increments per update */
    float speed_kt;           /* the HUD SPEED number in knots */
    float alt_ft;             /* the HUD ALT number in feet */
    float heading_deg;        /* 0..360 */
    float pitch_deg, roll_deg;
    float vsi_fpm;            /* vertical speed, feet per minute (derived) */
    float g;                  /* load factor along the aircraft's up axis (derived), 1 in level flight */
    float throttle;           /* engine power 0..1 (0.4 with no button pressed) */
    float afterburner;        /* 0..1 */
    float airbrake;           /* 0..1 */
    float aoa_deg;            /* angle of attack (candidate field) */
    float stick[3];           /* control inputs -1..1 */
    int missiles, gun, special;
    float damage_pct;
} zero_instr;
int zero_instr_get(zero_instr *out);      /* any thread: a consistent copy; 0 when none yet */

/* The telemetry stream's view of the game (ACZ_TELEMETRY, telem_udp 2026-11-04; ps2_telem.c sends it, zero_vr.c fills it,
   docs/TELEMETRY.md is the reader's description). */
enum { ZT_MENU, ZT_FLIGHT, ZT_PAUSED, ZT_BRIEFING, ZT_MOVIE, ZT_CUTSCENE, ZT_LOADING };
enum { ZTF_GUN = 1, ZTF_STALL = 2, ZTF_PULL_UP = 4, ZTF_LOCKED = 8, ZTF_MISSILE_ALERT = 16, ZTF_HIGH_G = 32, ZTF_ON_GROUND = 64 };
typedef struct zero_telem {
    zero_instr in;            /* the instruments of the last flight frame (in.valid 0: none this mission) */
    unsigned long long field; /* the publish's vblank count (flight), else the current one */
    int state;                /* ZT_* */
    int publishing;           /* the EE publishes flight frames now (the HUD manager ran within 8 fields) */
    int aircraft, mission;    /* the mission's aircraft id 0..35 and mission number 1..; -1 before the first mission */
    unsigned flags;           /* ZTF_* */
    float mach;               /* the HUD speed over the ISA speed of sound at the altimeter's height */
    float sideslip_deg;       /* the velocity's angle to the nose in the wing plane; + = moving to the right */
    float lat_g, long_g;      /* felt load along the right wing / the nose (the up axis: in.g) */
    float rate_dps[3];        /* the flight model's own body rates: nose up, nose right, right wing down, degrees a second */
    float vel[3], acc[3];     /* body axes right / up / forward: m/s, felt m/s^2 (acc[1] = 9.81 in level flight) */
    float pos[3];             /* world position in the altimeter's metres (y up) */
    float pitch_rad, yaw_rad, roll_rad;   /* yaw: the game's (heading = -yaw); roll: right wing down positive */
    float health_pct;         /* the hit points against the most seen this mission; -1 unknown */
    float gear;               /* the runtime's own gear state 0 up .. 1 down (the game has none); -1 unknown */
    unsigned gun_shots, missiles_fired, specials_fired, hits_taken;   /* since the start of the process */
} zero_telem;
int zero_telem_get(zero_telem *out);      /* any thread; returns out->state */
#endif
