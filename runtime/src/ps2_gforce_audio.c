// ps2_gforce_audio.c - the pilot's own sounds under G and stress (wishes #77, #81, #82), synthesised and mixed with the
// immersion layer on the SDL audio thread (ps2_audio.c imm_mix calls ps2_gf_mix before its own sounds), and the
// immersion bus: how all our added layers duck under the game's own mix.
//
// Inputs (zero_vr.c gf_update, once per flight frame, ps2_gf_audio): [0] field stamp (older than 8 fields: paused, a
// menu, a cutscene -- everything fades out), [1] G along the aircraft's up axis, [2] the anti-G straining manoeuvre
// 0..1, [3] stress 0..1 (missile alert, lock, hits, firing, STALL / PULL UP), [4] exertion 0..1 (G-work, slow to
// fade), [5] buffet 0..1, [6] first person (cockpit or HUD view, no cutscene), [7..9] the G-suit's direction in head
// axes (x right, y up, z ahead), [10] grey-out 0..1.3, [11] airspeed kt, [12] cockpit view (muffling).
//
// What a fighter pilot hears (research 2026-10-23, im_audio2 report): under the helmet the cockpit's 100-120 dB is
// 85-100 dBA of low-frequency roar (earcups take 40-50 dB above 2 kHz, only 12-19 dB at 125-250 Hz; HGU-55/P data
// sheet, NATO RTO-EN-HFM-111); airflow and the air conditioning dominate, the engine is a dull rumble; the radio and the
// warning tones are the loudest things at the ear. The pilot's own breathing reaches the ears only through the mask
// microphone on the intercom ("hot mic"): a close, band-limited (~0.1-4 kHz) sound whose level is the pilot's
// intercom knob -- many pilots turn it down ("I don't want to hear myself breathe in the jet", an F-16 pilot), and
// sims keep calm breathing off (Falcon BMS) while players like it as a G cue. The demand valve "clicks shut sharply
// at the end of each inspiration" (RAF AP1182E). The G-suit is felt (a squeeze), not heard: no account describes a
// hiss. The straining manoeuvre is clearly audible on hot mic ("hook": a grunt, a held breath ~3 s, a forceful
// release and a quick gasp). So (ACZ_IMM_PILOT_MODEL=hotmic; the default since 2026-10-27 is `natural`: the same voices
// made from a model of the breathing itself -- the airflow, the valves, the glottis; see gfn_* below):
// - BREATH (ACZ_IMM_BREATH="level[,calm]", default 0 = off; the ENHANCEMENTS menu sets it): through the hot-mic chain
//   (high-pass 180 Hz, mask-cavity presence ~1.5 kHz, 4th-order low-pass 3.4 kHz -- nothing above ~4 kHz, where the
//   old voice hissed): inhale = the regulator's flow through the mask cavity (500 / 1700 / 2700 Hz, as measured on
//   demand regulators), ending in the valve's click; exhale = a softer, darker puff through the exhalation valve
//   with its flutter. Heard only under effort (stress, recent G-work, grey-out) unless ",calm": then also at rest,
//   6 dB lower. 12 breaths a minute, up to 26 with effort.
// - STRAIN (ACZ_IMM_STRAIN, level, default 1): the AGSM as the Navy's "hook": above ~5 G SUSTAINED (G > 5.5 for
//   1 s or > 7 G for 0.3 s: the 6 G spike of a lift-off pull does not start it) a quick gasp, a short voiced grunt
//   closing on the glottis ("hoo-"), the breath held 2.5-3.2 s, then the forceful release ("k-hah") and the next gasp;
//   a long relieved exhale when the G comes off. Through the same hot-mic chain.
// - G-SUIT (ACZ_IMM_GSUIT, level, default 0 = off): felt rather than heard, so no hiss: when the anti-G valve starts to
//   fill the bladders (from ~2 G at 1.5 psi per G) one low "whump" (the bladders pushing on the body, ~90 Hz) with a
//   short muffled air rush; venting is a soft short "pff". From below (binaural with ACZ_AUDIO_HRTF).
// - BUFFET (ACZ_IMM_BUFFET=strength[,sound,rumble,needles], default 1; sound part here): the airframe's shudder
//   (60-150 Hz pulses every 60-110 ms) and the separated airflow as a low rumble (~380 Hz; was a 700 Hz band),
//   outside the canopy (muffled in the cockpit view as the game's outside sounds, ACZ_AUDIO_MUFFLE).
// ACZ_IMM_PILOT_MODEL=hiss: the first versions of 2026-10-23 (breath hiss through 0.9 / 2.2 / 5.2 kHz heard always,
// the hick before the gasp, the G-suit's inflation hiss and venting "pssht", the 700 Hz buffet air), for A/B.
//
// The immersion bus (ACZ_IMM_DUCK="from_dbfs,slope,max_db", default "-24,0.7,10"; 0 = off): ps2_audio.c hands the
// game's own mix of each chunk (engine, effects, music, radio) to ps2_imm_bus_game before our layers are added; its
// level (RMS, attack 50 ms, release 0.6 s) pushes our continuous beds down -- the wheels' roll, the airflow of the gear,
// flaps and air brake (zero_syssnd.c), the cloud airflow (ps2_audio.c), the buffet's air (half) -- by `slope` dB per dB
// above `from_dbfs`, at most `max_db` (HDR-style mixing: what the loud game would mask anyway is taken out instead of
// smearing it), and the pilot voices by 0.4 dB per dB above from_dbfs + 4, at most 6 dB. While a radio message plays
// (ps2_nustream_radio_active) the breath ducks by 10 dB, the strain by 6 dB, the G-suit and the beds by 4 dB.
// ACZ_IMMERSE=0 turns everything off. ACZ_DEBUG_GF_SOUND=N logs a summary every N s.
#include "ps2_runtime.h"
#include "ps2_menv.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define GF_RATE 48000
#define GF_PI 3.14159265f
u64 ps2_kernel_vblank_count(void);
int ps2_spatial_hrtf_on(void);
float ps2_spatial_muffle(void);
int ps2_nustream_radio_active(void);

volatile float ps2_gf_audio[16];

typedef struct { float b0, b1, b2, a1, a2, z1, z2; } gf_bq;
static float bq_run(gf_bq *f, float x) {
    float y = f->b0 * x + f->z1;
    f->z1 = f->b1 * x - f->a1 * y + f->z2;
    f->z2 = f->b2 * x - f->a2 * y;
    return y;
}
/* RBJ band-pass, 0 dB peak gain */
static void bq_bp(gf_bq *f, float fc, float q) {
    float w = 2.0f * GF_PI * fc / GF_RATE, al = sinf(w) / (2.0f * q), a0 = 1.0f + al;
    f->b0 = al / a0; f->b1 = 0.0f; f->b2 = -al / a0; f->a1 = -2.0f * cosf(w) / a0; f->a2 = (1.0f - al) / a0;
}
static void bq_lp(gf_bq *f, float fc, float q) {
    float w = 2.0f * GF_PI * fc / GF_RATE, al = sinf(w) / (2.0f * q), c = cosf(w), a0 = 1.0f + al;
    f->b0 = (1.0f - c) * 0.5f / a0; f->b1 = (1.0f - c) / a0; f->b2 = f->b0; f->a1 = -2.0f * c / a0; f->a2 = (1.0f - al) / a0;
}
static void bq_hp(gf_bq *f, float fc, float q) {
    float w = 2.0f * GF_PI * fc / GF_RATE, al = sinf(w) / (2.0f * q), c = cosf(w), a0 = 1.0f + al;
    f->b0 = (1.0f + c) * 0.5f / a0; f->b1 = -(1.0f + c) / a0; f->b2 = f->b0; f->a1 = -2.0f * c / a0; f->a2 = (1.0f - al) / a0;
}
static void bq_peak(gf_bq *f, float fc, float q, float db) {
    float A = powf(10.0f, db / 40.0f), w = 2.0f * GF_PI * fc / GF_RATE, al = sinf(w) / (2.0f * q), c = cosf(w);
    float a0 = 1.0f + al / A;
    f->b0 = (1.0f + al * A) / a0; f->b1 = -2.0f * c / a0; f->b2 = (1.0f - al * A) / a0; f->a1 = -2.0f * c / a0; f->a2 = (1.0f - al / A) / a0;
}
/* a damped two-pole resonator started by an impulse (peak ~amp) */
typedef struct { float c, r2, y1, y2; } gf_res;
static void res_start(gf_res *r, float f, float tau, float amp) {
    float dt = 1.0f / GF_RATE, rr = expf(-dt / tau);
    r->c = 2.0f * rr * cosf(2.0f * GF_PI * f * dt); r->r2 = rr * rr;
    r->y1 += amp * sinf(2.0f * GF_PI * f * dt);
}
static float res_run(gf_res *r) {
    if (r->c == 0.0f) return 0.0f;
    float y = r->c * r->y1 - r->r2 * r->y2;
    r->y2 = r->y1; r->y1 = y;
    if (fabsf(y) < 1e-7f && fabsf(r->y2) < 1e-7f) { r->y1 = r->y2 = 0.0f; r->c = 0.0f; }
    return y;
}

/* ================= the immersion bus: our layers under the game's mix ================= */
static struct {
    int init, on; float from_db, slope, max_db;
    float env_db;                    /* the game's level (dBFS), attack 50 ms / release 0.6 s */
    float bed, pilot;                /* the current gains (linear), smoothed */
    float radio_s;                   /* radio ducking 0..1 (attack 80 ms / release 0.4 s) */
    double e_game; u32 n_game;       /* debug: the game's mean level */
    float min_bed;
} bus;
static void bus_read(void) {
    bus.on = 1; bus.from_db = -24.0f; bus.slope = 0.7f; bus.max_db = 10.0f;
    const char *e = ps2_menv("ACZ_IMM_DUCK");
    if (e && *e) {
        float a = -24.0f, b = 0.7f, c = 10.0f;
        int n = sscanf(e, "%f,%f,%f", &a, &b, &c);
        if (n == 1 && a == 0.0f) bus.on = 0;
        else {
            if (n >= 1 && a < 0.0f && a > -80.0f) bus.from_db = a;
            if (n >= 2 && b >= 0.0f && b <= 2.0f) bus.slope = b;
            if (n >= 3 && c >= 0.0f && c <= 40.0f) bus.max_db = c;
        }
    }
}
static void bus_init(void) {
    bus.init = 1;
    bus_read();
    bus.env_db = -90.0f; bus.bed = bus.pilot = 1.0f; bus.min_bed = 1.0f;
}
/* ps2_audio.c audio_cb: the game's own mix of this chunk (interleaved stereo), before our layers are added */
void ps2_imm_bus_game(const s16 *game, int frames) {
    if (!bus.init) bus_init();
    if (frames <= 0) return;
    double e = 0.0;
    for (int i = 0; i < 2 * frames; i++) e += (double)game[i] * game[i];
    e /= (double)(2 * frames) * 32768.0 * 32768.0;
    float db = e > 1e-12 ? (float)(10.0 * log10(e)) : -120.0f;
    float T = (float)frames / GF_RATE;
    float k = 1.0f - expf(-T / (db > bus.env_db ? 0.05f : 0.6f));
    bus.env_db += (db - bus.env_db) * k;
    bus.e_game += e; bus.n_game++;
    /* the gains: beds by `slope` dB per dB above from_db (at most max_db); the pilot voices 0.4 dB per dB above
       from_db + 4 (at most 6); radio speech: beds -4 dB (the pilot's breath -10 dB is applied per voice) */
    float over = bus.env_db - bus.from_db, gb = 0.0f, gp = 0.0f;
    if (bus.on && over > 0.0f) { gb = over * bus.slope; if (gb > bus.max_db) gb = bus.max_db; }
    if (bus.on && over > 4.0f) { gp = (over - 4.0f) * 0.4f; if (gp > 6.0f) gp = 6.0f; }
    int radio = bus.on && ps2_nustream_radio_active();
    float kr = 1.0f - expf(-T / (radio ? 0.08f : 0.4f));
    bus.radio_s += ((radio ? 1.0f : 0.0f) - bus.radio_s) * kr;
    float tb = powf(10.0f, -(gb + 4.0f * bus.radio_s) / 20.0f), tp = powf(10.0f, -gp / 20.0f);
    float ks = 1.0f - expf(-T / 0.08f);
    bus.bed += (tb - bus.bed) * ks; bus.pilot += (tp - bus.pilot) * ks;
    if (bus.bed < bus.min_bed) bus.min_bed = bus.bed;
}
/* the gain of one class of our layers: 0 the continuous beds (roll, airflow; with the radio's 4 dB), 1 the pilot's
   voices (the radio part is theirs) */
float ps2_imm_duck(int cls) {
    if (!bus.init) bus_init();
    return cls == 0 ? bus.bed : bus.pilot;
}
float ps2_imm_bus_level(void) { return bus.env_db; }
int ps2_imm_bus_on(void) { if (!bus.init) bus_init(); return bus.on; }

/* ================= the pilot's voices ================= */
/* the natural model's state (ACZ_IMM_PILOT_MODEL=natural): one breath planned at a time from a physiological
   process, its airflow shaped per sample, the sound made from the flow */
typedef struct {
    float z_t, z_v;                  /* AR(1) deviations of the period and of the volume (unit variance) */
    float vol, qpk, ka, w_exp;       /* this phase: volume (rest 1), peak flow (normalised), exhale peak, inhale skew */
    int sigh_in;                     /* breaths until the next sigh */
    float q;                         /* the flow now (normalised, >= 0) */
    float im_lp, tilt, tilt_a;       /* intermittency (smooth random), the flow-dependent low-pass and its coefficient */
    gf_bq f[4]; float ff0[4], ff1[4], fq[4], fg[4], direct;   /* this phase's formants: start / end Hz, Q, gain */
    float wind1, wind2;              /* the breath stream on the mask microphone (low-passed noise) */
    int vstate;                      /* demand valve: 0 shut, 1 open, 2 shut again this inhale; exhalation valve: 3 lifted */
    float cut;                       /* the flow noise after the demand valve shut (fast decay) */
    gf_res click[3];
    float bounce_t, bounce_a, bounce_f;              /* the valve's bounce (a second, weaker click) */
    int flutter; float flut_next, flut_amp;          /* exhalation valve flutter at low flow (irregular flaps) */
    float voice, gph, gf0, gshim, gprev, f0_glide;   /* voicing: amount, glottal phase / f0 / shimmer, glide over the phase */
    float dq, aq, daq;               /* the flow's step and its level (flow^0.9) and step per sample, set per block */
    int blk;                         /* samples to the next re-tune of the filters */
    float kburst;                    /* the release's "k": a short burst of band noise */
    gf_bq kb;
    u32 n_sighs, n_clicks, n_bounces, n_flutters;
} gf_nat;

static struct {
    int init, dbg, buf_snd, v1, nat, calm; float k_breath, k_strain, k_suit, k_buf;
    gf_nat nb;
    u32 rng;
    float run, fp, duck, duck_s;
    /* breath */
    int ph, kind, cycles, hicks; float t, dur, amp, bright, rate_bpm, hold_t;
    gf_bq in1, in2, in3, ex1, ex2, h1, h2, h3, hlp;
    float click_t, click_f, glot_ph, glot_f0, flut_ph, br_lp, release;
    /* hot-mic chain and the new voices */
    gf_bq mic_hp, mic_pk, mic_lp1, mic_lp2;
    gf_res vclick[2];
    float aud, effort, turb, turb_t;     /* breath audibility (effort gate), its effort input, flow turbulence */
    float g_hi_t, g_vhi_t; int agsm;     /* sustained-G gate of the straining manoeuvre */
    int mic_live;                        /* samples the hot-mic voice still runs (0: silent, skipped) */
    /* suit */
    float p, fin, fout, ain, aout, sdir[3];
    gf_bq s1, s2, s3, v1f, v2f;
    float sl_ring[64]; u32 sl_wp; float s_tau[2], s_al[2], s_g[2], s_hx[2], s_hy[2];
    int s_infl, s_vent; float s_idle, s_full;      /* (hot-mic model) inflating / venting now; seconds without filling */
    gf_res thump; float th_env, th_amp, pf_env, pf_amp; gf_bq th_lp, pf_bp, pf_lp;
    u32 n_thumps, n_puffs;
    /* buffet */
    float bs, shud, shud_t, shud_tg, mlp[2];
    gf_bq bl[2], bb[2], ba[2];
    /* debug */
    double e_br, e_su, e_bu, cpu_ns; u32 n_dbg, n_win;
} gfa;

static float gf_noise(void) { gfa.rng ^= gfa.rng << 13; gfa.rng ^= gfa.rng >> 17; gfa.rng ^= gfa.rng << 5; return (float)(gfa.rng >> 8) * (2.0f / 16777216.0f) - 1.0f; }
static float gf_rand(void) { return 0.5f + 0.5f * gf_noise(); }
static float gf_env_level(const char *e, float def) {
    if (!e || !*e) return def;
    float v = (float)atof(e);
    return v < 0.0f ? 0.0f : v > 3.0f ? 3.0f : v;
}
static float gf_ss(float a, float b, float x) { float t = (x - a) / (b - a); t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t; return t * t * (3.0f - 2.0f * t); }
/* the levels: at the start and at a new mission (the NEXT MISSION tier, ps2_menv.h: the audio thread, ps2_gf_mix) */
static void gf_levels_read(int off) {
    {   const char *e = ps2_menv("ACZ_IMM_BREATH");
        gfa.k_breath = off ? 0.0f : gf_env_level(e, gfa.v1 ? 1.0f : 0.0f);
        gfa.calm = gfa.v1 || (e && strstr(e, "calm") != NULL); }
    gfa.k_strain = off ? 0.0f : gf_env_level(ps2_menv("ACZ_IMM_STRAIN"), 1.0f);
    gfa.k_suit = off ? 0.0f : gf_env_level(ps2_menv("ACZ_IMM_GSUIT"), gfa.v1 ? 1.0f : 0.0f);
    {   const char *e = ps2_menv("ACZ_IMM_BUFFET");
        gfa.k_buf = 1.0f; gfa.buf_snd = 1;
        if (e && *e) {
            gfa.k_buf = (float)atof(e);
            if (!(gfa.k_buf >= 0.0f)) gfa.k_buf = 0.0f;
            if (gfa.k_buf > 3.0f) gfa.k_buf = 3.0f;
            const char *c = strchr(e, ',');
            if (c) gfa.buf_snd = strstr(c, "sound") != NULL;
        }
        if (off) gfa.k_buf = 0.0f;
    }
}
static void gf_init(void) {
    gfa.init = 1; gfa.rng = 0x51ED270Bu;
    int off = 0;
    { const char *e = getenv("ACZ_IMMERSE"); off = e && *e == '0'; }
    {   const char *e = getenv("ACZ_IMM_PILOT_MODEL");     /* natural (default), hotmic (2026-10-23), hiss (first) */
        gfa.v1 = e && !strcmp(e, "hiss");
        gfa.nat = !gfa.v1 && !(e && !strcmp(e, "hotmic")); }
    /* the defaults follow the ENHANCEMENTS menu's (2026-10-23 evening): breath and G-suit off, strain on; the first
       versions (hiss) had all three on */
    gf_levels_read(off);
    { const char *e = getenv("ACZ_DEBUG_GF_SOUND"); gfa.dbg = e ? atoi(e) : 0; }
    gfa.ph = 3; gfa.dur = 1.0f; gfa.rate_bpm = 12.0f;
    for (int e = 0; e < 2; e++) { gfa.s_al[e] = 1.0f; gfa.s_g[e] = 1.0f; gfa.s_tau[e] = 2.0f; }
    gfa.sdir[1] = -0.9f; gfa.sdir[2] = 0.4f;
    bq_lp(&gfa.bl[0], 150.0f, 0.8f); gfa.bl[1] = gfa.bl[0];
    bq_bp(&gfa.bb[0], 80.0f, 1.2f); gfa.bb[1] = gfa.bb[0];
    if (gfa.v1) bq_bp(&gfa.ba[0], 700.0f, 0.6f); else bq_bp(&gfa.ba[0], 380.0f, 0.7f);
    gfa.ba[1] = gfa.ba[0];
    bq_bp(&gfa.s1, 1500.0f, 0.7f); bq_bp(&gfa.s2, 3800.0f, 1.2f); bq_lp(&gfa.s3, 110.0f, 0.7f);
    bq_bp(&gfa.v1f, 2800.0f, 1.0f); bq_bp(&gfa.v2f, 6000.0f, 1.5f);
    bq_lp(&gfa.hlp, 3200.0f, 0.7f);
    /* the hot-mic chain: the mask microphone and the intercom (0.18-3.4 kHz, a +2 dB presence lift at the mask cavity) */
    bq_hp(&gfa.mic_hp, 180.0f, 0.707f); bq_peak(&gfa.mic_pk, 1500.0f, 1.2f, 2.0f);
    bq_lp(&gfa.mic_lp1, 3400.0f, 0.5412f); bq_lp(&gfa.mic_lp2, 3400.0f, 1.3066f);
    bq_lp(&gfa.th_lp, 600.0f, 0.7f); bq_bp(&gfa.pf_bp, 700.0f, 0.8f); bq_lp(&gfa.pf_lp, 1500.0f, 0.7f);
    gfa.s_idle = 10.0f;
    gfa.nb.sigh_in = 33;                 /* (no draw here: the older models keep their noise sequence) */
    if (!bus.init) bus_init();
    ps2_log("gforce-sound: %s model, breath %.2f%s, strain %.2f, G-suit %.2f, buffet %.2f%s; immersion bus %s (from %.0f dBFS, "
            "%.2f dB/dB, max %.0f dB)", gfa.v1 ? "first (hiss)" : gfa.nat ? "natural hot-mic" : "hot-mic", gfa.k_breath, gfa.calm ? " (also calm)" : " (under effort)",
            gfa.k_strain, gfa.k_suit, gfa.k_buf, gfa.buf_snd ? "" : " (no sound)",
            bus.on ? "on" : "off", bus.from_db, bus.slope, bus.max_db);
}

/* ---- the first versions (ACZ_IMM_PILOT_MODEL=hiss) ---- */
/* the next breath phase. ph: 0 inhale, 1 pause, 2 exhale, 3 rest; 4 hick, 5 gasp, 6 hold, 7 relieved exhale */
static void gf_breath_next_v1(float strain, float heavy) {
    int agsm = strain > 0.5f && gfa.k_strain > 0.0f;
    float P = 60.0f / gfa.rate_bpm, j = 0.92f + 0.16f * gf_rand();
    if (agsm && (gfa.ph <= 3 || gfa.ph == 7)) { gfa.ph = 4; }
    else if (agsm) gfa.ph = gfa.ph == 4 ? 5 : gfa.ph == 5 ? 6 : 4;
    else if (gfa.ph >= 4 && gfa.ph <= 6) gfa.ph = 7;
    else gfa.ph = gfa.ph == 7 ? 3 : (gfa.ph + 1) & 3;
    gfa.t = 0.0f;
    switch (gfa.ph) {
    case 0:         /* inhale: the regulator's valve ticks open, the hiss through the mask */
        gfa.dur = fminf(1.6f, fmaxf(0.7f, 0.36f * P)) * j;
        gfa.amp = (0.6f + 0.8f * heavy) * (0.85f + 0.3f * gf_rand());
        gfa.bright = heavy;
        bq_bp(&gfa.in1, (1900.0f + 900.0f * heavy) * (0.95f + 0.1f * gf_rand()), 0.9f);
        bq_bp(&gfa.in2, 5200.0f * (0.95f + 0.1f * gf_rand()), 1.5f);
        bq_bp(&gfa.in3, 900.0f * (0.95f + 0.1f * gf_rand()), 4.0f);
        gfa.click_t = 0.0f; gfa.click_f = 2100.0f + 600.0f * gf_rand();
        gfa.cycles++;
        break;
    case 1: gfa.dur = fmaxf(0.05f, 0.06f * P) * j; break;
    case 2: case 7:  /* exhale (7: the long relieved one after straining) */
        gfa.dur = gfa.ph == 7 ? 0.9f * j : fminf(2.0f, fmaxf(0.8f, 0.44f * P)) * j;
        gfa.amp = (gfa.ph == 7 ? 1.3f : 0.55f + 0.75f * heavy) * (0.85f + 0.3f * gf_rand());
        bq_bp(&gfa.ex1, 550.0f * (0.92f + 0.16f * gf_rand()), 1.2f);
        bq_bp(&gfa.ex2, 1400.0f * (0.92f + 0.16f * gf_rand()), 2.0f);
        break;
    case 3: gfa.dur = fmaxf(0.1f, P - 0.86f * P) * j; break;
    case 4:          /* hick: a forceful voiced breath through the closing glottis */
        gfa.dur = 0.11f + 0.04f * gf_rand();
        gfa.amp = 1.0f * (0.85f + 0.3f * gf_rand());
        bq_bp(&gfa.h1, 650.0f * (0.94f + 0.12f * gf_rand()), 3.0f);
        bq_bp(&gfa.h2, 1150.0f * (0.94f + 0.12f * gf_rand()), 4.0f);
        bq_bp(&gfa.h3, 2400.0f * (0.94f + 0.12f * gf_rand()), 5.0f);
        gfa.glot_f0 = 105.0f + 25.0f * gf_rand(); gfa.glot_ph = 0.0f;
        gfa.hicks++;
        break;
    case 5:          /* the quick gasp after it */
        gfa.dur = 0.35f + 0.1f * gf_rand();
        gfa.amp = 1.25f * (0.85f + 0.3f * gf_rand());
        bq_bp(&gfa.in1, 2600.0f * (0.95f + 0.1f * gf_rand()), 0.9f);
        bq_bp(&gfa.in2, 5200.0f, 1.5f);
        bq_bp(&gfa.in3, 950.0f, 4.0f);
        gfa.click_t = 0.0f; gfa.click_f = 2300.0f;
        break;
    case 6: gfa.dur = 2.6f + 0.6f * gf_rand(); break;    /* holding, straining */
    }
}
static float gf_breath_sample_v1(float dt) {
    float u = gfa.t / gfa.dur, s = 0.0f;
    if (u > 1.0f) u = 1.0f;
    switch (gfa.ph) {
    case 0: case 5: {
        float ka = gfa.ph == 5 ? 0.15f : 0.25f;
        float e = u < ka ? sinf(0.5f * GF_PI * u / ka) : cosf(0.5f * GF_PI * (u - ka) / (1.0f - ka));
        e = e * e;
        float n = gf_noise();
        s = (bq_run(&gfa.in1, n) * 0.55f + bq_run(&gfa.in2, n) * (0.18f + 0.12f * gfa.bright) + bq_run(&gfa.in3, n) * 0.35f) * e * gfa.amp;
        if (gfa.click_t < 0.006f) {           /* the demand valve's tick */
            s += 0.25f * sinf(2.0f * GF_PI * gfa.click_f * gfa.click_t) * expf(-gfa.click_t / 0.0015f);
            gfa.click_t += dt;
        }
        s *= 0.25f;
        break; }
    case 2: case 7: {
        float e = u < 0.12f ? sinf(0.5f * GF_PI * u / 0.12f) : powf(cosf(0.5f * GF_PI * (u - 0.12f) / 0.88f), 1.5f);
        float n = gf_noise();
        gfa.flut_ph += 28.0f * dt; if (gfa.flut_ph > 1.0f) gfa.flut_ph -= 1.0f;
        float fl = 1.0f + 0.25f * sinf(2.0f * GF_PI * gfa.flut_ph);
        s = (bq_run(&gfa.ex1, n) * 0.7f + bq_run(&gfa.ex2, n) * 0.35f) * e * fl * gfa.amp;
        s *= 0.28f;
        break; }
    case 4: {
        float e = u < 0.05f ? u / 0.05f : u < 0.4f ? 1.0f : expf(-(u - 0.4f) * 6.0f);
        gfa.glot_ph += gfa.glot_f0 * (1.0f + 0.03f * gf_noise()) * dt;
        float g = 0.0f;
        if (gfa.glot_ph >= 1.0f) { gfa.glot_ph -= 1.0f; g = 1.0f; }
        float x = g * 0.9f + gf_noise() * 0.35f;
        s = (bq_run(&gfa.h1, x) * 0.6f + bq_run(&gfa.h2, x) * 0.45f + bq_run(&gfa.h3, x) * 0.2f);
        s = bq_run(&gfa.hlp, s) * e * gfa.amp * 0.33f;
        break; }
    default: s = 0.0f;
    }
    gfa.t += dt;
    return s;
}

/* ---- the hot-mic voices (default) ----
   phases: 0 inhale, 1 pause, 2 exhale, 3 rest; AGSM: 5 gasp, 8 grunt (closing on the glottis), 6 hold, 4 release
   ("k-hah"), 7 relieved exhale when the G comes off */
static void gf_breath_next(int agsm, float heavy) {
    float P = 60.0f / gfa.rate_bpm, j = 0.92f + 0.16f * gf_rand();
    int ph = gfa.ph;
    if (agsm) {
        if (ph <= 3 || ph == 7) ph = 5;                  /* entering: a quick gasp first */
        else ph = ph == 5 ? 8 : ph == 8 ? 6 : ph == 6 ? 4 : 5;
    } else if (ph == 6 || ph == 8) ph = 7;               /* the G is off while holding: the relieved release */
    else if (ph == 4 || ph == 5) ph = 3;                 /* (released just now: rest, then breathe) */
    else ph = ph == 7 ? 3 : (ph + 1) & 3;
    gfa.ph = ph; gfa.t = 0.0f;
    switch (ph) {
    case 0:         /* inhale: the demand regulator's flow through the mask cavity, closing with the valve's click */
        gfa.dur = fminf(1.3f, fmaxf(0.55f, 0.30f * P)) * j;
        gfa.amp = (0.55f + 0.6f * heavy) * (0.85f + 0.3f * gf_rand());
        gfa.bright = heavy;
        bq_bp(&gfa.in1, 520.0f * (0.93f + 0.14f * gf_rand()), 2.0f);
        bq_bp(&gfa.in2, (1450.0f + 250.0f * heavy) * (0.94f + 0.12f * gf_rand()), 2.5f);
        bq_bp(&gfa.in3, 2650.0f * (0.95f + 0.1f * gf_rand()), 3.5f);
        gfa.click_t = -1.0f;
        gfa.cycles++;
        break;
    case 1: gfa.dur = fmaxf(0.05f, 0.06f * P) * j; break;
    case 2: case 7:  /* exhale through the exhalation valve (7: the long relieved one after straining) */
        gfa.dur = ph == 7 ? 0.8f * j : fminf(1.6f, fmaxf(0.6f, 0.40f * P)) * j;
        gfa.amp = (ph == 7 ? 1.1f : 0.45f + 0.55f * heavy) * (0.85f + 0.3f * gf_rand());
        bq_bp(&gfa.ex1, 380.0f * (0.92f + 0.16f * gf_rand()), 1.5f);
        bq_bp(&gfa.ex2, 950.0f * (0.92f + 0.16f * gf_rand()), 2.5f);
        gfa.flut_ph = 0.0f;
        res_start(&gfa.vclick[0], 620.0f, 0.003f, 0.08f * gfa.amp);    /* the exhalation valve lifting */
        break;
    case 3: gfa.dur = fmaxf(0.1f, P - 0.76f * P) * j; break;
    case 5:          /* the quick gasp: a fast, full inhale through the regulator */
        gfa.dur = 0.30f + 0.08f * gf_rand();
        gfa.amp = 1.3f * (0.85f + 0.3f * gf_rand());
        bq_bp(&gfa.in1, 560.0f, 2.0f); bq_bp(&gfa.in2, 1600.0f * (0.95f + 0.1f * gf_rand()), 2.2f); bq_bp(&gfa.in3, 2700.0f, 3.0f);
        gfa.click_t = -1.0f;
        break;
    case 8:          /* the grunt: "hoo-" voiced, cut by the closing glottis */
        gfa.dur = 0.12f + 0.05f * gf_rand();
        gfa.amp = 1.0f * (0.85f + 0.3f * gf_rand());
        bq_bp(&gfa.h1, 450.0f * (0.94f + 0.12f * gf_rand()), 4.0f);
        bq_bp(&gfa.h2, 1050.0f * (0.94f + 0.12f * gf_rand()), 5.0f);
        bq_bp(&gfa.h3, 2350.0f * (0.94f + 0.12f * gf_rand()), 6.0f);
        gfa.glot_f0 = 110.0f + 25.0f * gf_rand(); gfa.glot_ph = 0.0f;
        gfa.hicks++;
        break;
    case 6: gfa.dur = 2.5f + 0.7f * gf_rand(); break;    /* holding, straining (silent: the glottis is shut) */
    case 4:          /* the release: the "k" bursting open, a forceful short exhale */
        gfa.dur = 0.22f + 0.06f * gf_rand();
        gfa.amp = 1.2f * (0.85f + 0.3f * gf_rand());
        bq_bp(&gfa.ex1, 700.0f * (0.92f + 0.16f * gf_rand()), 1.4f);
        bq_bp(&gfa.ex2, 1500.0f * (0.92f + 0.16f * gf_rand()), 2.0f);
        res_start(&gfa.vclick[0], 1800.0f, 0.0012f, 0.22f * gfa.amp);  /* the "k" */
        res_start(&gfa.vclick[1], 900.0f, 0.004f, 0.10f * gfa.amp);
        gfa.glot_f0 = 120.0f + 20.0f * gf_rand(); gfa.glot_ph = 0.0f;
        break;
    }
}
/* one sample of the breath / strain voice before the hot-mic chain */
static float gf_breath_sample(float dt) {
    float u = gfa.t / gfa.dur, s = 0.0f;
    if (u > 1.0f) u = 1.0f;
    gfa.turb_t -= dt;
    if (gfa.turb_t <= 0.0f) { gfa.turb_t = 0.06f + 0.06f * gf_rand(); gfa.turb = 0.8f + 0.4f * gf_rand(); }
    switch (gfa.ph) {
    case 0: case 5: {
        float ka = gfa.ph == 5 ? 0.18f : 0.22f;          /* rise, hold, fall (the inhale is cut by the valve) */
        float e = u < ka ? sinf(0.5f * GF_PI * u / ka) : u < 0.85f ? 1.0f : cosf(0.5f * GF_PI * (u - 0.85f) / 0.15f);
        e *= e;
        float n = gf_noise();
        s = (bq_run(&gfa.in1, n) * 0.6f + bq_run(&gfa.in2, n) * (0.42f + 0.15f * gfa.bright) + bq_run(&gfa.in3, n) * 0.2f)
            * e * gfa.amp * (0.9f + 0.1f * gfa.turb);
        if (gfa.click_t < 0.0f && u >= 0.97f && gfa.ph == 0) {   /* the inhalation valve clicks shut */
            res_start(&gfa.vclick[0], 1350.0f, 0.0015f, 0.10f * gfa.amp);
            res_start(&gfa.vclick[1], 2500.0f, 0.0008f, 0.06f * gfa.amp);
            gfa.click_t = 0.0f;
        }
        s *= 0.5f;
        break; }
    case 2: case 7: {
        float e = u < 0.10f ? sinf(0.5f * GF_PI * u / 0.10f) : powf(cosf(0.5f * GF_PI * (u - 0.10f) / 0.90f), 1.6f);
        float n = gf_noise();
        gfa.flut_ph += 24.0f * dt; if (gfa.flut_ph > 1.0f) gfa.flut_ph -= 1.0f;
        float fl = 1.0f + 0.35f * sinf(2.0f * GF_PI * gfa.flut_ph);
        s = (bq_run(&gfa.ex1, n) * 0.8f + bq_run(&gfa.ex2, n) * 0.35f) * e * fl * gfa.amp * 0.42f;
        break; }
    case 8: case 4: {
        /* glottal pulses with jitter and breath noise through the formants: the grunt (8) rises fast and is cut; the
           release (4) is mostly breath with a voiced start */
        float e, voiced;
        if (gfa.ph == 8) { e = u < 0.08f ? u / 0.08f : u < 0.8f ? 1.0f : (1.0f - u) / 0.2f; voiced = 0.9f; }
        else { e = u < 0.04f ? u / 0.04f : expf(-(u - 0.04f) * 4.0f); voiced = 0.6f * (1.0f - u); }
        gfa.glot_ph += gfa.glot_f0 * (1.0f + 0.03f * gf_noise()) * dt;
        float g = 0.0f;
        if (gfa.glot_ph >= 1.0f) { gfa.glot_ph -= 1.0f; g = 1.0f; }
        float n = gf_noise();
        if (gfa.ph == 8) {
            float x = g * voiced + n * 0.3f;
            s = (bq_run(&gfa.h1, x) * 0.6f + bq_run(&gfa.h2, x) * 0.45f + bq_run(&gfa.h3, x) * 0.2f) * e * gfa.amp * 0.40f;
        } else {
            float x = g * voiced * 2.0f;
            s = ((bq_run(&gfa.ex1, n) * 0.8f + bq_run(&gfa.ex2, n) * 0.5f) * 0.55f + (bq_run(&gfa.h1, x) + bq_run(&gfa.h2, x) * 0.5f) * 0.2f)
                * e * gfa.amp;
        }
        break; }
    default: s = 0.0f;
    }
    s += res_run(&gfa.vclick[0]) + res_run(&gfa.vclick[1]);
    gfa.t += dt;
    return s;
}
static void gf_mic_reset(void) {
    gf_bq *f[] = { &gfa.mic_hp, &gfa.mic_pk, &gfa.mic_lp1, &gfa.mic_lp2, &gfa.in1, &gfa.in2, &gfa.in3, &gfa.ex1, &gfa.ex2, &gfa.h1, &gfa.h2, &gfa.h3 };
    for (unsigned i = 0; i < sizeof f / sizeof f[0]; i++) f[i]->z1 = f[i]->z2 = 0.0f;
    memset(gfa.vclick, 0, sizeof gfa.vclick);
}
static float gf_mic(float x) {          /* the mask microphone and the intercom */
    return bq_run(&gfa.mic_lp2, bq_run(&gfa.mic_lp1, bq_run(&gfa.mic_pk, bq_run(&gfa.mic_hp, x))));
}
#define GFN_GAIN 0.20f                   /* the natural voices' level: calm -45 dBFS rms (hot-mic -53), effort -29 (-41) */

/* ---- the natural voices (ACZ_IMM_PILOT_MODEL=natural, default since 2026-10-27, bug #138 "robotic") ----
   Why the hot-mic voices sounded synthetic (offline renders, cv_audio report): every breath was the same white noise
   through the same three narrow band-passes (Q 2-3.5) under the same envelope (a flat plateau from 22 to 85 % of the
   inhale), the period jittered only +-8 % with no memory, the exhale carried a 24 Hz sine tremolo (35 %), and the
   demand valve clicked at exactly 97 % of every inhale with the same two modes. Here instead:
   - the plan: a breath period and volume from AR(1) processes (period CV ~13 % at rest, 0.6 breath-to-breath
     correlation; bigger breaths last longer), rate 12 -> 28 / min and volume x1.9 with effort, inspiratory share
     0.38 -> 0.44, a sigh (x2-2.5 volume, a longer pause after it) every 20-50 calm breaths;
   - the airflow (per 32 samples, interpolated): the inhale a skewed half-sine (peak ~45 %), the exhale the passive recoil (a fast rise
     to ~22 % then an exponential-like decay; more active with effort); the sound follows the flow, not an envelope:
     turbulence level ~ flow^0.9 (as the mic hears it), its brightness (a low-pass 0.7 -> 3 kHz) rising with the flow, a smooth random
     intermittency (+-20 %, ~15 Hz) instead of stepped gains;
   - broad, low-Q resonances (the airway, the mask cavity, the regulator's seat; Q 1.3-3) re-drawn per breath (+-7 %)
     and gliding within it (the mouth moves); inhale vs exhale differ (the exhale an open "hah");
   - the demand valve opens when the inhale flow passes its cracking threshold (a faint tick) and SHUTS when the flow
     falls below it: a click whose time, level and pitch follow the breath (2 modes +-15 %, sometimes a bounce), the
     regulator's hiss cut within ~15 ms; the exhalation valve lifts with a low soft pop, and on some breaths flaps
     irregularly at the low-flow tail (18-32 Hz, each flap +-30 %) instead of a steady tremolo;
   - the breath stream on the mask microphone at high flow (a low "puff", mostly exhaling);
   - with effort the exhale gets a little voicing (a breathy "huh": a Rosenberg glottal pulse with jitter and shimmer);
     the straining voices (gasp, grunt, release, relieved exhale) use the same flow / glottis models.
   The hot-mic chain (HP 180 Hz, +2 dB at 1.5 kHz, LR4 LP 3.4 kHz) is kept: the band-limited intercom character. */
static float gfn_gauss(void) { return gf_noise() + gf_noise() + gf_noise(); }   /* ~N(0,1) (var 3 x 1/3) */
static float gfn_clamp(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
/* the formants of a phase: kind 0 inhale, 1 exhale, 2 grunt ("hoo"), 3 release ("hah", forceful) */
static void gfn_formants(int kind, float heavy) {
    gf_nat *n = &gfa.nb;
    static const float F[4][4] = { { 520.0f, 1250.0f, 2350.0f, 930.0f },     /* inhale: airway, regulator seat, mask */
                                   { 640.0f, 1150.0f, 2450.0f, 330.0f },     /* exhale: open "hah", the valve's flap */
                                   { 420.0f, 880.0f, 2300.0f, 3000.0f },     /* grunt "hoo" */
                                   { 720.0f, 1250.0f, 2500.0f, 360.0f } };   /* release */
    static const float Q[4][4] = { { 1.6f, 2.0f, 2.2f, 1.3f }, { 2.2f, 2.6f, 3.0f, 1.4f },
                                   { 4.0f, 6.0f, 9.0f, 9.0f }, { 2.0f, 2.4f, 2.8f, 1.4f } };
    static const float G[4][4] = { { 0.55f, 0.55f, 0.40f, 0.30f }, { 0.75f, 0.50f, 0.22f, 0.30f },
                                   { 0.80f, 0.45f, 0.18f, 0.08f }, { 0.75f, 0.55f, 0.25f, 0.25f } };
    float open = kind == 1 ? 1.0f + 0.15f * heavy : 1.0f;                     /* a wider mouth with effort: F1 up */
    for (int k = 0; k < 4; k++) {
        float f = F[kind][k] * (k == 0 ? open : 1.0f) * (1.0f + 0.07f * gfn_clamp(gfn_gauss(), -2.0f, 2.0f));
        n->ff0[k] = f;
        n->ff1[k] = f * (1.0f + 0.05f * gfn_clamp(gfn_gauss(), -2.0f, 2.0f));
        n->fq[k] = Q[kind][k] * (0.85f + 0.3f * gf_rand());
        n->fg[k] = G[kind][k] * (0.8f + 0.4f * gf_rand());
        bq_bp(&n->f[k], f, n->fq[k]);
    }
    n->direct = kind == 0 ? 0.20f : kind == 1 ? 0.12f : 0.05f;
    n->blk = 0;
}
static void gfn_voice_start(float f0, float amount) {
    gf_nat *n = &gfa.nb;
    n->gf0 = f0; n->voice = amount; n->gph = gf_rand(); n->gshim = 1.0f; n->f0_glide = 0.0f;
}
/* the next phase (natural model): as gf_breath_next's state machine, with the plan above */
static void gfn_next(int agsm, float heavy) {
    gf_nat *n = &gfa.nb;
    int ph = gfa.ph;
    if (agsm) {
        if (ph <= 3 || ph == 7) ph = 5;
        else ph = ph == 5 ? 8 : ph == 8 ? 6 : ph == 6 ? 4 : 5;
    } else if (ph == 6 || ph == 8) ph = 7;
    else if (ph == 4 || ph == 5) ph = 3;
    else ph = ph == 7 ? 3 : (ph + 1) & 3;
    gfa.ph = ph; gfa.t = 0.0f;
    n->vstate = 0; n->cut = 1.0f; n->voice = 0.0f; n->flutter = 0; n->kburst = 0.0f; n->blk = 0;
    switch (ph) {
    case 0: {       /* a new breath: draw its period and volume, then the inhale */
        n->z_t = 0.6f * n->z_t + 0.8f * gfn_clamp(gfn_gauss(), -2.5f, 2.5f);
        n->z_v = 0.5f * n->z_v + 0.866f * gfn_clamp(gfn_gauss(), -2.5f, 2.5f);
        float P = 60.0f / gfa.rate_bpm * expf((0.13f - 0.05f * heavy) * n->z_t);
        float vol = (1.0f + 0.9f * heavy) * expf(0.15f * n->z_v + 0.06f * n->z_t);
        float share = 0.38f + 0.06f * heavy;
        int sigh = 0;
        if (--n->sigh_in <= 0) {
            n->sigh_in = 20 + (int)(30.0f * gf_rand());
            if (heavy < 0.35f) { sigh = 1; vol *= 2.0f + 0.5f * gf_rand(); P *= 1.5f; share = 0.45f; n->n_sighs++; }
        }
        n->vol = vol;
        gfa.dur = gfn_clamp(P * share * expf(0.06f * gfn_gauss()), 0.45f, 3.2f);
        gfa.hold_t = P;                                   /* the period, for the exhale and the pause */
        gfa.release = sigh ? 1.0f : 0.0f;
        n->qpk = gfn_clamp(vol / gfa.dur * 1.9f, 0.3f, 3.0f);          /* rest: ~1 */
        n->w_exp = 0.88f + 0.08f * gf_rand();
        gfn_formants(0, heavy);
        gfa.bright = heavy;
        gfa.cycles++;
        break; }
    case 1: gfa.dur = heavy > 0.5f ? 0.02f : (0.03f + 0.10f * gf_rand()) * (gfa.release > 0.0f ? 2.5f : 1.0f); break;
    case 2: case 7: {   /* exhale (7: the long relieved one after straining) */
        float P = gfa.hold_t > 0.5f ? gfa.hold_t : 60.0f / gfa.rate_bpm;
        if (ph == 7) { gfa.dur = 0.8f + 0.35f * gf_rand(); n->vol = 1.8f; }
        else gfa.dur = gfn_clamp(P * (0.42f + 0.02f * heavy) * expf(0.07f * gfn_gauss()) * (gfa.release > 0.0f ? 1.3f : 1.0f), 0.5f, 4.0f);
        n->qpk = gfn_clamp(n->vol / gfa.dur * 2.1f * (ph == 7 ? 1.3f : 1.0f), 0.3f, 3.0f);
        n->ka = ph == 7 ? 0.14f : 0.20f + 0.12f * gfa.bright + 0.04f * gf_rand();
        gfn_formants(1, heavy);
        n->flutter = ph == 2 && gf_rand() < 0.55f;
        n->flut_next = 0.0f; n->flut_amp = 0.6f + 0.6f * gf_rand();
        float vo = ph == 7 ? 0.35f : heavy > 0.45f ? 0.28f * (heavy - 0.45f) / 0.55f : 0.0f;
        if (vo > 0.0f) gfn_voice_start(105.0f + 25.0f * gf_rand(), vo);
        break; }
    case 3: {           /* the pause after the exhale: the rest of the period (sighs rest longer) */
        float P = gfa.hold_t > 0.5f ? gfa.hold_t : 60.0f / gfa.rate_bpm;
        gfa.dur = fmaxf(0.06f, P * (0.20f - 0.08f * heavy) * expf(0.25f * gfn_gauss())) * (gfa.release > 0.0f ? 1.6f : 1.0f);
        gfa.release = 0.0f;
        break; }
    case 5:             /* the quick gasp: a fast, full inhale through the regulator */
        gfa.dur = 0.28f + 0.10f * gf_rand();
        n->vol = 0.45f + 0.15f * gf_rand();
        n->qpk = gfn_clamp(n->vol / gfa.dur * 1.9f, 1.5f, 3.0f);
        n->w_exp = 0.65f + 0.1f * gf_rand();
        gfn_formants(0, 1.0f);
        gfa.bright = 1.0f;
        break;
    case 8:             /* the grunt: "hoo-" voiced, cut by the closing glottis */
        gfa.dur = 0.13f + 0.06f * gf_rand();
        gfn_formants(2, 1.0f);
        gfn_voice_start(112.0f + 25.0f * gf_rand(), 1.0f);
        n->f0_glide = 0.08f + 0.06f * gf_rand();
        n->qpk = 0.35f;
        gfa.hicks++;
        break;
    case 6: gfa.dur = 2.5f + 0.7f * gf_rand(); break;    /* holding, straining (the glottis shut: silent) */
    case 4:             /* the release: the "k" bursting open, then a forceful short exhale */
        gfa.dur = 0.22f + 0.08f * gf_rand();
        n->vol = 0.35f + 0.1f * gf_rand();
        n->qpk = gfn_clamp(n->vol / gfa.dur * 2.1f, 1.4f, 3.0f);
        n->ka = 0.10f + 0.05f * gf_rand();
        gfn_formants(3, 1.0f);
        gfn_voice_start(118.0f + 20.0f * gf_rand(), 0.45f);
        n->kburst = 1.0f;
        bq_bp(&n->kb, 1900.0f * (0.9f + 0.2f * gf_rand()), 1.6f);
        res_start(&n->click[2], 1800.0f * (0.9f + 0.2f * gf_rand()), 0.0012f, 0.16f);
        break;
    }
}
/* the glottis: a Rosenberg pulse (open 40 %, closing 20 %) differentiated (the lips' radiation), jitter 1.5 %,
   shimmer 8 % per period, the f0 pulled back toward its start (no drift) */
static float gfn_glottis(float dt) {
    gf_nat *n = &gfa.nb;
    float f0 = n->gf0 * (1.0f + n->f0_glide * (gfa.dur > 0.0f ? gfa.t / gfa.dur : 0.0f));
    n->gph += f0 * dt;
    if (n->gph >= 1.0f) {
        n->gph -= 1.0f;
        n->gf0 *= 1.0f + 0.015f * gfn_clamp(gfn_gauss(), -2.0f, 2.0f);
        n->gshim = 1.0f + 0.08f * gfn_clamp(gfn_gauss(), -2.0f, 2.0f);
    }
    float p = n->gph, g;
    if (p < 0.4f) g = 0.5f * (1.0f - cosf(GF_PI * p / 0.4f));
    else if (p < 0.6f) g = cosf(0.5f * GF_PI * (p - 0.4f) / 0.2f);
    else g = 0.0f;
    g *= n->gshim;
    float y = (g - n->gprev) * 12.0f;
    n->gprev = g;
    return y;
}
/* the airflow of the current phase at u (0..1 of the phase), normalised */
static float gfn_flow(float u) {
    gf_nat *n = &gfa.nb;
    int ph = gfa.ph;
    if (u > 1.0f) u = 1.0f;
    if (ph == 0 || ph == 5) { float w = powf(u, n->w_exp); return n->qpk * powf(fmaxf(0.0f, sinf(GF_PI * w)), 1.3f); }
    if (ph == 2 || ph == 7 || ph == 4) {
        float x = u / n->ka;
        return n->qpk * x * expf(1.0f - x) * (1.0f - gf_ss(0.72f, 1.0f, u));
    }
    if (ph == 8) return n->qpk * (u < 0.1f ? u / 0.1f : 1.0f);
    return 0.0f;
}
/* one sample of the natural breath / strain voice before the hot-mic chain */
static float gfn_sample(float dt) {
    gf_nat *n = &gfa.nb;
    float u = gfa.dur > 0.0f ? gfa.t / gfa.dur : 1.0f, q, s = 0.0f;
    if (u > 1.0f) u = 1.0f;
    int ph = gfa.ph, inh = ph == 0 || ph == 5, exh = ph == 2 || ph == 7 || ph == 4;
    /* every 32 samples: the flow at the block's end (interpolated in between), the formant glide, the brightness */
    if (n->blk-- <= 0) {
        n->blk = 31;
        float qe = gfn_flow(gfa.dur > 0.0f ? (gfa.t + 32.0f * dt) / gfa.dur : 1.0f);
        n->dq = (qe - n->q) * (1.0f / 32.0f);
        n->daq = ((qe > 1e-6f ? powf(qe, 0.9f) : 0.0f) - n->aq) * (1.0f / 32.0f);
        for (int k = 0; k < 4; k++) {
            float f = n->ff0[k] + (n->ff1[k] - n->ff0[k]) * u;
            float z1 = n->f[k].z1, z2 = n->f[k].z2;
            bq_bp(&n->f[k], f, n->fq[k]);
            n->f[k].z1 = z1; n->f[k].z2 = z2;
        }
        float fc = (700.0f + 2300.0f * powf(qe > 1.6f ? 1.6f : qe, 0.8f)) * (1.0f + 0.25f * gfa.bright);
        n->tilt_a = 1.0f - expf(-2.0f * GF_PI * fc * dt);
    }
    n->q += n->dq; n->aq += n->daq;
    if (n->q < 0.0f) n->q = 0.0f;
    if (n->aq < 0.0f) n->aq = 0.0f;
    q = n->q;
    /* intermittency: a smooth random gain (~15 Hz, about +-20 %) */
    float wn = gf_noise();
    n->im_lp += (gf_noise() - n->im_lp) * 0.00196f;
    float im = 1.0f + 9.0f * n->im_lp;
    /* voicing: the grunt (all voice), effortful exhales and the release (some); through the same resonances */
    float v = 0.0f;
    if (n->voice > 0.0f) {
        float e;
        if (ph == 8) e = u < 0.12f ? u / 0.12f : u < 0.82f ? 1.0f : (1.0f - u) / 0.18f;
        else e = (u < 0.06f ? u / 0.06f : 1.0f) * (1.0f - gf_ss(0.35f, 0.9f, u));
        float g = gfn_glottis(dt) * e * n->voice;
        float asp = gf_noise() * 0.25f * e * n->voice * (n->gph < 0.6f ? 1.0f : 0.4f);   /* breathiness, open phase */
        v = (g + asp) * (ph == 8 ? 0.34f : 0.14f);
    }
    /* the turbulence: level ~ flow^0.9 (heard on the mic: compressed), brighter with more flow */
    if (q > 1e-4f || n->tilt != 0.0f || v != 0.0f) {
        float src = wn * n->aq * im * n->cut;
        n->tilt += (src - n->tilt) * n->tilt_a;
        float x = n->tilt, y = x * n->direct;
        for (int k = 0; k < 4; k++) y += bq_run(&n->f[k], x + v) * n->fg[k];
        s = y * 1.6f;
        if (fabsf(n->tilt) < 1e-9f && q <= 1e-4f) n->tilt = 0.0f;
    }
    /* the breath stream on the mask microphone: a low "puff" at high flow (mostly the exhale) */
    if (q > 0.05f) {
        float pf = (exh ? 1.0f : 0.3f) * q * q * 0.45f;
        n->wind1 += (gf_noise() - n->wind1) * 0.033f; n->wind2 += (n->wind1 - n->wind2) * 0.033f;
        s += n->wind2 * pf * 3.0f;
    }
    /* the demand valve (inhale): opens past the cracking flow, shuts below it in the falling half */
    if (inh) {
        if (n->vstate == 0 && q > 0.05f * n->qpk && u < 0.5f) {
            n->vstate = 1;
            res_start(&n->click[0], 1150.0f * (0.85f + 0.3f * gf_rand()), 0.0009f, 0.018f * (0.6f + 0.8f * gf_rand()) * sqrtf(n->qpk));
        } else if (n->vstate == 1 && u > 0.55f && q < 0.10f * n->qpk) {
            n->vstate = 2;
            float a = (ph == 5 ? 0.32f : 0.17f) * sqrtf(n->qpk) * (0.55f + 0.8f * gf_rand());
            res_start(&n->click[0], 1400.0f * (0.85f + 0.3f * gf_rand()), 0.0012f + 0.0008f * gf_rand(), a);
            res_start(&n->click[1], 2600.0f * (0.85f + 0.3f * gf_rand()), 0.0006f + 0.0004f * gf_rand(), 0.6f * a);
            n->n_clicks++;
            if (gf_rand() < 0.3f) { n->bounce_t = 0.005f + 0.009f * gf_rand(); n->bounce_a = a * (0.25f + 0.2f * gf_rand()); n->bounce_f = 1500.0f * (0.85f + 0.3f * gf_rand()); }
        }
        if (n->vstate == 2) n->cut *= 0.99861f;      /* the regulator's hiss stops within ~15 ms */
    }
    if (n->bounce_t > 0.0f) {
        n->bounce_t -= dt;
        if (n->bounce_t <= 0.0f) { res_start(&n->click[2], n->bounce_f, 0.0008f, n->bounce_a); n->n_bounces++; }
    }
    /* the exhalation valve: lifts with a soft low pop, flaps irregularly at the low-flow tail on some breaths */
    if (exh) {
        if (n->vstate == 0 && q > 0.06f * n->qpk) {
            n->vstate = 3;
            res_start(&n->click[1], 300.0f * (0.85f + 0.4f * gf_rand()), 0.004f, 0.035f * sqrtf(n->qpk) * (0.6f + 0.6f * gf_rand()));
        }
        if (n->flutter && u > 0.45f && q > 0.03f && q < 0.35f * n->qpk) {
            n->flut_next -= dt;
            if (n->flut_next <= 0.0f) {
                float rate = 18.0f + 14.0f * gf_rand();
                n->flut_next = (1.0f / rate) * (0.7f + 0.6f * gf_rand());
                res_start(&n->click[2], 380.0f * (0.8f + 0.4f * gf_rand()), 0.0025f, 0.02f * n->flut_amp * sqrtf(q) * (0.5f + gf_rand()));
                n->n_flutters++;
            }
        }
    }
    /* the release's "k" */
    if (n->kburst > 1e-3f) {
        s += bq_run(&n->kb, gf_noise()) * n->kburst * 0.5f;
        n->kburst *= 0.9965f;                           /* ~6 ms */
    }
    s += res_run(&n->click[0]) + res_run(&n->click[1]) + res_run(&n->click[2]);
    gfa.t += dt;
    return s * GFN_GAIN;
}
static void gfn_reset(void) {
    gf_nat *n = &gfa.nb;
    for (int k = 0; k < 4; k++) n->f[k].z1 = n->f[k].z2 = 0.0f;
    n->kb.z1 = n->kb.z2 = 0.0f;
    n->tilt = n->wind1 = n->wind2 = 0.0f;
    n->q = n->aq = n->dq = n->daq = 0.0f; n->blk = 0;
    memset(n->click, 0, sizeof n->click);
}

/* the G-suit's binaural placement: Woodworth delay and a one-pole head shadow per ear (as ps2_spatial.c) */
static void gf_suit_dir_block(int hrtf) {
    float x = gfa.sdir[0];
    for (int e = 0; e < 2; e++) {
        float c = e ? x : -x, th = acosf(c > 1.0f ? 1.0f : c < -1.0f ? -1.0f : c);
        if (hrtf) {
            gfa.s_tau[e] = 2.0f + (0.0875f / 343.0f * GF_RATE) * (th < 1.5707963f ? 1.0f - c : 1.0f + th - 1.5707963f);
            float a = (1.05f + 0.95f * cosf(th * (180.0f / 150.0f))) * (1.0f / 0.7564f);
            gfa.s_al[e] = a < 0.08f ? 0.08f : a > 2.0f ? 2.0f : a;
            gfa.s_g[e] = 1.0f;
        } else {
            float p = 0.5f + 0.5f * (x > 1.0f ? 1.0f : x < -1.0f ? -1.0f : x);
            gfa.s_tau[e] = 2.0f; gfa.s_al[e] = 1.0f;
            gfa.s_g[e] = (e ? sinf(p * 1.5707963f) : cosf(p * 1.5707963f)) * 1.4142136f;
        }
    }
}
static void gf_suit_bin(float s, float *l, float *r) {
    const float K = 2.0f * GF_RATE / (2.0f * 343.0f / 0.0875f);
    gfa.sl_ring[gfa.sl_wp & 63u] = s;
    float o[2];
    for (int e = 0; e < 2; e++) {
        float d = gfa.s_tau[e];
        int n = (int)d; float fr = d - (float)n;
        float p0 = gfa.sl_ring[(gfa.sl_wp - (u32)n) & 63u], p1 = gfa.sl_ring[(gfa.sl_wp - (u32)n - 1u) & 63u];
        float y = p0 + fr * (p1 - p0);
        float h = ((y + gfa.s_hx[e]) + gfa.s_al[e] * K * (y - gfa.s_hx[e])) * (1.0f / (1.0f + K)) - ((1.0f - K) / (1.0f + K)) * gfa.s_hy[e];
        gfa.s_hx[e] = y; gfa.s_hy[e] = h;
        o[e] = h * gfa.s_g[e];
    }
    gfa.sl_wp++;
    *l += o[0]; *r += o[1];
}

void ps2_gf_mix(s16 *buf, int frames) {
    if (!bus.init) bus_init();
    if (!gfa.init) gf_init();
    {   static unsigned mg;                          /* (a new mission with changed rows: this thread's own statics) */
        if (ps2_menv_take(&mg, ps2_menv_gen)) {
            const char *im = getenv("ACZ_IMMERSE");
            const float a = gfa.k_breath, b = gfa.k_strain, c = gfa.k_suit, d = gfa.k_buf; const int e = gfa.calm, f = bus.on;
            gf_levels_read(im && *im == '0');
            bus_read();
            ps2_menv_note("ACZ_IMM_BREATH", a, gfa.k_breath); ps2_menv_note("ACZ_IMM_BREATH (calm)", e, gfa.calm);
            ps2_menv_note("ACZ_IMM_STRAIN", b, gfa.k_strain); ps2_menv_note("ACZ_IMM_GSUIT", c, gfa.k_suit);
            ps2_menv_note("ACZ_IMM_BUFFET (sound)", d, gfa.k_buf); ps2_menv_note("ACZ_IMM_DUCK", f, bus.on);
        }
    }
    if (gfa.k_breath <= 0.0f && gfa.k_strain <= 0.0f && gfa.k_suit <= 0.0f && (gfa.k_buf <= 0.0f || !gfa.buf_snd)) return;
    struct timespec c0; if (gfa.dbg > 0) clock_gettime(CLOCK_MONOTONIC, &c0);
    const float dt = 1.0f / GF_RATE;
    float stamp = ps2_gf_audio[0];
    int fresh = stamp > 0.0f && (float)ps2_kernel_vblank_count() - stamp <= 8.0f;
    float G = fresh ? ps2_gf_audio[1] : 1.0f, strain = fresh ? ps2_gf_audio[2] : 0.0f;
    float stress = ps2_gf_audio[3], exert = ps2_gf_audio[4], buf_t = fresh ? ps2_gf_audio[5] : 0.0f;
    float fp_t = fresh && ps2_gf_audio[6] > 0.5f ? 1.0f : 0.0f, grey = ps2_gf_audio[10];
    if (!(G > -10.0f && G < 20.0f)) G = 1.0f;
    if (!(stress >= 0.0f && stress <= 1.0f)) stress = 0.0f;
    if (!(exert >= 0.0f && exert <= 1.0f)) exert = 0.0f;
    if (!(grey >= 0.0f && grey <= 2.0f)) grey = 0.0f;
    for (int j = 0; j < 3; j++) { float d = ps2_gf_audio[7 + j]; gfa.sdir[j] = d >= -1.0f && d <= 1.0f ? d : gfa.sdir[j]; }
    float heavy = fmaxf(stress, fmaxf(0.85f * exert, 0.8f * (grey > 1.0f ? 1.0f : grey)));
    heavy = heavy < 0.0f ? 0.0f : heavy > 1.0f ? 1.0f : heavy;
    gfa.rate_bpm = 12.0f + 14.0f * heavy;
    float fdt = (float)frames * dt;
    /* the effort the hot mic makes audible: stress, grey-out, and G-work in the last ~20 s (the exertion input calms over
       ~30 s; only its upper part counts) */
    {   float ex = (exert - 0.3f) / 0.7f; ex = ex < 0.0f ? 0.0f : ex > 1.0f ? 1.0f : ex;
        float ef = fmaxf(stress, fmaxf(0.8f * (grey > 1.0f ? 1.0f : grey), 0.85f * ex));
        gfa.effort = ef;
        float target = gfa.calm ? 0.5f + 0.5f * gf_ss(0.15f, 0.6f, ef) : gf_ss(0.15f, 0.6f, ef);
        if (gfa.v1) target = 1.0f;
        gfa.aud += (target - gfa.aud) * (1.0f - expf(-fdt / 1.5f)); }
    /* the straining manoeuvre only for sustained G (hot-mic model): G > 5.5 for 1 s or > 7 G for 0.3 s; it ends with
       the game's strain input (off below 4 G) */
    int agsm;
    if (gfa.v1) agsm = strain > 0.5f && gfa.k_strain > 0.0f;
    else {
        gfa.g_hi_t = G > 5.5f ? gfa.g_hi_t + fdt : 0.0f;
        gfa.g_vhi_t = G > 7.0f ? gfa.g_vhi_t + fdt : 0.0f;
        if (!gfa.agsm && strain > 0.5f && (gfa.g_hi_t >= 1.0f || gfa.g_vhi_t >= 0.3f)) gfa.agsm = 1;
        if (gfa.agsm && strain < 0.3f) gfa.agsm = 0;
        agsm = gfa.agsm && gfa.k_strain > 0.0f;
    }
    int radio = ps2_nustream_radio_active();
    int hrtf = ps2_spatial_hrtf_on();
    float mu = ps2_gf_audio[12] > 0.5f ? ps2_spatial_muffle() : 0.0f;
    mu = mu < 0.0f ? 0.0f : mu > 1.0f ? 1.0f : mu;
    const float ma = 1.0f - expf(-2.0f * GF_PI * 3000.0f * dt), mg = 1.0f - mu * (1.0f - 0.708f);
    const float k_run = 1.0f - expf(-dt / 0.3f), k_duck_a = 1.0f - expf(-dt / 0.08f), k_duck_r = 1.0f - expf(-dt / 0.4f);
    const float k_buf = 1.0f - expf(-dt / 0.1f);
    float d_pilot = gfa.v1 ? 1.0f : ps2_imm_duck(1), d_bed = gfa.v1 ? 1.0f : sqrtf(ps2_imm_duck(0));
    gf_suit_dir_block(hrtf);
    /* the G-suit valve: per chunk (~10 ms) */
    {   float pt = 1.5f * (G - 2.0f); pt = fresh ? (pt < 0.0f ? 0.0f : pt > 10.5f ? 10.5f : pt) : 0.0f;
        float dp = pt - gfa.p, fl = 0.0f;
        if (dp > 0.15f) { fl = dp / 0.3f; if (fl > 25.0f) fl = 25.0f; }
        else if (dp < -0.15f) { fl = dp / 0.18f; if (fl < -40.0f) fl = -40.0f; }
        gfa.p += fl * fdt;
        if (gfa.p < 0.0f) gfa.p = 0.0f;
        gfa.fin = fl > 0.0f ? powf(fl / 25.0f, 0.6f) : 0.0f;
        gfa.fout = fl < 0.0f ? powf(-fl / 40.0f, 0.6f) : 0.0f;
        if (!gfa.v1) {
            /* the whump when the valve starts filling (after >= 0.4 s without filling), the puff when venting starts
               from a filled suit */
            if (fl > 0.5f && !gfa.s_infl) {
                if (gfa.s_idle >= 0.4f && gfa.k_suit > 0.0f && fresh) {
                    float a = 0.45f + 0.55f * (fl / 25.0f > 1.0f ? 1.0f : fl / 25.0f);
                    res_start(&gfa.thump, 90.0f, 0.11f, 0.06f * a);
                    gfa.th_env = 0.0f; gfa.th_amp = a; gfa.n_thumps++;
                }
                gfa.s_infl = 1;
            }
            if (fl <= 0.5f) gfa.s_infl = 0;
            gfa.s_idle = fl > 0.5f ? 0.0f : gfa.s_idle + fdt;
            if (gfa.p > gfa.s_full) gfa.s_full = gfa.p;
            if (fl < -0.5f && !gfa.s_vent) {
                if (gfa.s_full > 1.5f && gfa.k_suit > 0.0f && fresh) {
                    gfa.pf_env = 0.0f; gfa.pf_amp = 0.35f + 0.65f * (gfa.s_full / 10.5f > 1.0f ? 1.0f : gfa.s_full / 10.5f); gfa.n_puffs++;
                }
                gfa.s_vent = 1;
            }
            if (fl >= -0.5f) gfa.s_vent = 0;
            if (gfa.p < 0.3f) gfa.s_full = 0.0f;
        }
    }
    {   gf_bq *fq[] = { &gfa.mic_hp, &gfa.mic_pk, &gfa.mic_lp1, &gfa.mic_lp2, &gfa.th_lp, &gfa.pf_bp, &gfa.pf_lp };
        for (unsigned q = 0; q < sizeof fq / sizeof fq[0]; q++)      /* (no denormals from the silent hold) */
            if (fabsf(fq[q]->z1) + fabsf(fq[q]->z2) < 1e-15f) fq[q]->z1 = fq[q]->z2 = 0.0f; }
    double e_br = 0.0, e_su = 0.0, e_bu = 0.0;
    for (int i = 0; i < frames; i++) {
        gfa.run += ((float)fresh - gfa.run) * k_run;
        gfa.fp += (fp_t - gfa.fp) * k_run;
        float dk = radio ? 1.0f : 0.0f;
        gfa.duck_s += (dk - gfa.duck_s) * (dk > gfa.duck_s ? k_duck_a : k_duck_r);
        float l = 0.0f, r = 0.0f, pil = gfa.run * gfa.fp;
        /* breath and strain (the mouth: centred) */
        if (gfa.k_breath > 0.0f || gfa.k_strain > 0.0f) {
            if (gfa.ph == 6 && (gfa.v1 ? strain < 0.3f : !agsm)) gfa.t = gfa.dur;   /* the G is off: no need to hold on */
            if (!gfa.v1 && agsm && (gfa.ph == 1 || gfa.ph == 3 || ((gfa.ph == 0 || gfa.ph == 2) && gfa.k_breath * gfa.aud < 0.02f)))
                gfa.t = gfa.dur;                                     /* straining starts now, not after a quiet breath */
            float s;
            if (gfa.v1) {
                if (gfa.t >= gfa.dur) gf_breath_next_v1(strain, heavy);
                s = gf_breath_sample_v1(dt);
                float k = gfa.ph >= 4 && gfa.ph <= 6 ? gfa.k_strain : gfa.k_breath;
                s *= k * pil * (1.0f - 0.684f * gfa.duck_s);
            } else {
                if (gfa.t >= gfa.dur) { if (gfa.nat) gfn_next(agsm, heavy); else gf_breath_next(agsm, heavy); }
                int straining = gfa.ph == 4 || gfa.ph == 5 || gfa.ph == 6 || gfa.ph == 8 || gfa.ph == 7;
                float k = straining ? 0.63f * gfa.k_strain : 0.4f * gfa.k_breath * gfa.aud;
                k *= pil;
                if (k > 1e-3f) gfa.mic_live = 9600;                 /* the voice and the mic chain run while audible (> -60 dB) + 0.2 s */
                if (gfa.mic_live > 0) {
                    gfa.mic_live--;
                    s = gfa.nat ? gfn_sample(dt) : gf_breath_sample(dt);
                    s = gf_mic(s) * k * (1.0f - (straining ? 0.5f : 0.684f) * gfa.duck_s) * d_pilot;
                    if (!gfa.mic_live) { gf_mic_reset(); gfn_reset(); }   /* (no denormals in the idle filters) */
                } else { gfa.t += dt; s = 0.0f; }                   /* silent: only the rhythm runs on */
            }
            l += s; r += s;
            e_br += (double)s * s;
        }
        /* the G-suit (below: binaural) */
        if (gfa.v1) {
            gfa.ain += (gfa.fin - gfa.ain) * 0.004f;               /* (~5 ms glides: no steps at the chunk edges) */
            gfa.aout += (gfa.fout - gfa.aout) * 0.004f;
            if (gfa.k_suit > 0.0f && (gfa.ain > 1e-4f || gfa.aout > 1e-4f)) {
                float n = gf_noise();
                float a = (bq_run(&gfa.s1, n) * 0.8f + bq_run(&gfa.s2, n) * 0.35f + bq_run(&gfa.s3, n) * 1.2f) * gfa.ain
                        + (bq_run(&gfa.v1f, n) * 0.8f + bq_run(&gfa.v2f, n) * 0.4f) * gfa.aout;
                float s = a * 0.2f * gfa.k_suit * pil * (1.0f - 0.37f * gfa.duck_s);
                float sl = 0.0f, sr = 0.0f;
                gf_suit_bin(s, &sl, &sr);
                l += sl; r += sr;
                e_su += (double)s * s;
            }
        } else if (gfa.k_suit > 0.0f && (gfa.thump.c != 0.0f || gfa.th_amp > 1e-4f || gfa.pf_amp > 1e-4f)) {
            /* the whump: the low resonance (bladders on the body) and a short muffled rush of air in the hose */
            float s = res_run(&gfa.thump);
            if (gfa.th_amp > 1e-4f) {
                float e = gfa.th_env < 0.02f ? gfa.th_env / 0.02f : expf(-(gfa.th_env - 0.02f) / 0.2f);
                s += bq_run(&gfa.th_lp, gf_noise()) * e * gfa.th_amp * 0.08f;
                gfa.th_env += dt;
                if (gfa.th_env > 1.2f) gfa.th_amp = 0.0f;
            }
            if (gfa.pf_amp > 1e-4f) {                            /* the vent's soft "pff" */
                float e = gfa.pf_env < 0.012f ? gfa.pf_env / 0.012f : expf(-(gfa.pf_env - 0.012f) / 0.11f);
                s += bq_run(&gfa.pf_lp, bq_run(&gfa.pf_bp, gf_noise())) * e * gfa.pf_amp * 0.04f;
                gfa.pf_env += dt;
                if (gfa.pf_env > 0.8f) gfa.pf_amp = 0.0f;
            }
            s *= gfa.k_suit * pil * (1.0f - 0.37f * gfa.duck_s) * d_pilot;
            float sl = 0.0f, sr = 0.0f;
            gf_suit_bin(s, &sl, &sr);
            l += sl; r += sr;
            e_su += (double)s * s;
        }
        /* buffet: the airframe's shudder and the separated airflow, outside the canopy */
        gfa.bs += (buf_t * gfa.k_buf * (gfa.buf_snd ? 1.0f : 0.0f) - gfa.bs) * k_buf;
        if (gfa.bs > 1e-4f) {
            gfa.shud_t -= dt;
            if (gfa.shud_t <= 0.0f) { gfa.shud_t = 0.06f + 0.05f * gf_rand(); gfa.shud_tg = 0.3f + 0.7f * gf_rand(); }
            gfa.shud += (gfa.shud_tg - gfa.shud) * 0.0021f;
            float bl2[2], ka = gfa.v1 ? 0.12f : 0.10f;
            for (int c = 0; c < 2; c++) {
                float n = gf_noise();
                float lo = bq_run(&gfa.bb[c], bq_run(&gfa.bl[c], n) * 4.0f);
                float air = bq_run(&gfa.ba[c], n);
                bl2[c] = (lo * 0.9f * gfa.shud + air * ka * (0.5f + 0.5f * gfa.shud) * d_bed) * gfa.bs * 0.45f * pil;
                if (mu > 0.0f) { gfa.mlp[c] += ma * (bl2[c] - gfa.mlp[c]); bl2[c] = (bl2[c] + mu * (gfa.mlp[c] - bl2[c])) * mg; }
            }
            l += bl2[0]; r += bl2[1];
            e_bu += 0.5 * ((double)bl2[0] * bl2[0] + (double)bl2[1] * bl2[1]);
        }
        int L = buf[2 * i] + (int)(l * 32767.0f), R = buf[2 * i + 1] + (int)(r * 32767.0f);
        buf[2 * i] = (s16)(L > 32767 ? 32767 : L < -32768 ? -32768 : L);
        buf[2 * i + 1] = (s16)(R > 32767 ? 32767 : R < -32768 ? -32768 : R);
    }
    gfa.e_br += e_br; gfa.e_su += e_su; gfa.e_bu += e_bu; gfa.n_win += (u32)frames;
    if (gfa.dbg > 0) {
        struct timespec c1; clock_gettime(CLOCK_MONOTONIC, &c1);
        gfa.cpu_ns += (double)(c1.tv_sec - c0.tv_sec) * 1e9 + (double)(c1.tv_nsec - c0.tv_nsec);
    }
    if (gfa.dbg > 0 && (gfa.n_dbg += (u32)frames) >= (u32)gfa.dbg * GF_RATE) {
        double n = gfa.n_win > 0 ? (double)gfa.n_win : 1.0;
#define GF_DB(e) ((e) > 0.0 ? 10.0 * log10((e) / n) : -200.0)
        ps2_log("gforce-sound: field %llu run %.2f fp %.2f G %.2f strain %.2f agsm %d stress %.2f exert %.2f heavy %.2f effort %.2f "
                "aud %.2f rate %.1f/min breaths %d hicks %d phase %d | suit %.1f psi in %.2f out %.2f whumps %u puffs %u | buffet %.2f | "
                "radio %d | game %.1f dBFS (mean %.1f), duck bed %.1f dB (min %.1f) pilot %.1f dB | rms breath %.1f suit %.1f buffet %.1f dBFS | "
                "cpu %.3f ms per s of audio", (unsigned long long)ps2_kernel_vblank_count(), gfa.run,
                gfa.fp, G, strain, agsm, stress, exert, heavy, gfa.effort, gfa.aud, gfa.rate_bpm, gfa.cycles, gfa.hicks, gfa.ph, gfa.p,
                gfa.fin, gfa.fout, gfa.n_thumps, gfa.n_puffs, gfa.bs, radio, bus.env_db,
                bus.n_game ? 10.0 * log10(bus.e_game / bus.n_game + 1e-20) : -200.0, 20.0f * log10f(bus.bed + 1e-9f),
                20.0f * log10f(bus.min_bed + 1e-9f), 20.0f * log10f(bus.pilot + 1e-9f),
                GF_DB(gfa.e_br), GF_DB(gfa.e_su), GF_DB(gfa.e_bu), gfa.cpu_ns / 1e6 / (n / GF_RATE));
#undef GF_DB
        gfa.n_dbg = 0; gfa.n_win = 0; gfa.e_br = gfa.e_su = gfa.e_bu = gfa.cpu_ns = 0.0; gfa.cycles = gfa.hicks = 0;
        gfa.n_thumps = gfa.n_puffs = 0; bus.e_game = 0.0; bus.n_game = 0; bus.min_bed = bus.bed;
    }
}
