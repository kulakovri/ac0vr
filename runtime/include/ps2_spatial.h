#ifndef PS2_SPATIAL_H
#define PS2_SPATIAL_H
/* Spatial audio (bug #29): the contract between the game side (zero_audio3d.c, EE thread, once per game frame after
   the game's 3D sound panner) and the SPU2 mixer (ps2_spu2.c + ps2_spatial.c, SDL audio thread).
   The game side publishes one frame: per SPU2 voice (= the game's sound channel index) whether it is a positional
   (3D) voice and where its source is relative to the listener's head, the pitch multiplier for Doppler, whether the
   source belongs to the own aircraft, plus the cockpit-view flag and the player's engine state. The mixer takes the
   latest complete frame at each mix chunk (seqlock, never blocks the EE) and renders 3D voices binaurally; 2D voices
   (music and radio streams, UI, warnings, the game's own stereo ambiences) never enter the spatial path. */
#include "ps2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PS2_SPAT_VOICES 48
#define PS2_SPAT_TEST   48              /* virtual voice: the synthetic test source (ACZ_AUDIO3D_TEST) */

typedef struct ps2_spat_src {
    float dir[3];       /* unit vector from the head to the source, head axes: x right, y up, z front */
    float dist_m;       /* distance in metres (game units / 5.12) */
    float dopp;         /* pitch multiplier on top of the game's pitch (1 = none; the game's own weak Doppler is
                           divided out for external sources, ACZ_AUDIO_DOPPLER) */
    float level;        /* test source only: linear level 0..1 */
    u8 is3d;            /* positional voice with fresh geometry */
    u8 own;             /* source on the own aircraft (co-moving and close): not muffled */
    u8 pad[2];
} ps2_spat_src;

typedef struct ps2_spat_engine {
    s8 voice[4];        /* the player's engine voices (SPU2 voice index, -1 none), loudest first */
    u8 n;
    u8 pad[3];
    float throttle;     /* engine power 0..1 (the flight model's, 0.4 with no button pressed) */
    float afterburner;  /* 0..1 */
    float speed_kt;
} ps2_spat_engine;

typedef struct ps2_spat_frame {
    u64 field;                          /* vblank field of the update */
    float cockpit;                      /* 1 = cockpit view (canopy closed): external 3D voices muffled */
    ps2_spat_src v[PS2_SPAT_VOICES + 1];  /* [48] = test source */
    ps2_spat_engine eng;
} ps2_spat_frame;

/* EE thread (zero_audio3d.c): publish a complete frame (copied). */
void ps2_spatial_publish(const ps2_spat_frame *f);

/* stage switches as used (environment overrides the F4 > Immersion setting), read by both sides */
int   ps2_spatial_hrtf_on(void);        /* ACZ_AUDIO_HRTF: binaural rendering of 3D voices */
float ps2_spatial_doppler(void);        /* ACZ_AUDIO_DOPPLER: strength (fraction of the real speed of sound) */
float ps2_spatial_muffle(void);         /* ACZ_AUDIO_MUFFLE: cockpit muffling amount 0..1 */
float ps2_spatial_engine_grain(void);   /* ACZ_IMM_ENGINE_GRAIN: granular engine mix 0..1 */

/* SDL audio thread (ps2_spu2.c mixer) */
void  ps2_spatial_begin(u32 frames);    /* per mix chunk: take the newest frame, set per-voice targets */
int   ps2_spatial_voice_on(u32 i);      /* 1: voice i takes the spatial path in this chunk */
void  ps2_spatial_note_on(u32 i);       /* key-on of voice i: restart its filters (no tail of the old note) */
void  ps2_spatial_tick(u32 k, float *fl, float *fr, int *rl, int *rr);   /* before sample k: block updates, test source */
int   ps2_spatial_test_solo(void);      /* ACZ_AUDIO3D_TEST_SOLO=1: only the test source is mixed */
/* one sample of voice i: e = the voice's sample after its envelope, gl/gr = the game's glided volumes (x256, signed:
   a negative side is the game's rear phase inversion, kept where the plain pan is used, e.g. flat play with muffling
   only); adds the output to fl, fr (float, 16-bit scale); returns the pitch multiplier for this sample's advance */
float ps2_spatial_voice(u32 i, int e, int gl, int gr, float *fl, float *fr);
/* granular engine (prototype, ACZ_IMM_ENGINE_GRAIN): for a player engine voice, the sample s (before the envelope)
   is crossfaded with grains cut from the voice's own waveform (ssa, loop start lsax) at the voice's pitch, shaped by
   throttle, afterburner and airspeed; returns the sample to use (other voices: s unchanged) */
int   ps2_spatial_grain(u32 i, int s, u32 ssa, u32 lsax, u32 pitch);
void  ps2_spatial_report(void);
int   ps2_spatial_selftest(void);       /* part of the SPU2 self-test; 0 = passed */

#ifdef __cplusplus
}
#endif
#endif
