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
#endif
