// zero_flyby.c - Ace Combat Zero: close passes of other aircraft (wish #140 "an aircraft passing close should sound
// more impressive; work on close objects", agent cv_audio 2026-10-27).
//
// The game plays every aircraft's engine as a 3D voice (zero_audio3d.c places it binaurally, with a quarter of the real
// Doppler), but a fighter passing at 30 m sounds like the same loop turned up: no roar arriving after the aircraft, no
// rip of the exhaust, no pitch drop worth the name, nothing felt. This layer adds the close pass, physically driven:
// - Game side (EE, once a field): every aircraft the game updates goes through its burner setup 0x1386D0 (zero_trails.c
//   found it: a2 = the unit's position vector, the player's at player+0x70); the HUD manager tick 0x24D9D8 (a0 - 0x1310 =
//   the player) is the frame. Per aircraft a 1.2 s ring of positions gives the RETARDED position: the point where the
//   sound now arriving at the listener (0x422C18: position, velocity, front, up -- the head-turned listener of
//   zero_vr.c) was emitted, |x(te) - L| = c (now - te). From it: the direction (head axes), the distance, the Doppler
//   ratio D = (c - vL.n) / (c - vS.n) (n from the source to the listener; ACZ_SND_FLYBY's strength k: D^k, default
//   0.5 of the real effect in log pitch; the game's own engines keep 0.25), the angle between the aircraft's flight
//   direction and the listener (front arc: inlet whine; rear arc: the jet's roar and crackle) and its speed. The three
//   nearest emission points within 600 m (2.1 s of travel) are published (seqlock) for the audio thread. Nothing is written to the game.
// - Audio side (SDL thread, ps2_audio.c calls ps2_audio_flyby_mix after the system sounds): per aircraft a voice
//   - the roar: pink noise through a wide band (~180-300 Hz x D), loudest 30-50 deg off the jet's axis behind it (the
//     jet-mixing noise peak at ~140 deg from the inlet);
//   - the crackle (the "rip" of a fighter at power): skewed random impulses (~600 / s, positive-heavy as real jet
//     crackle), 0.7-7 kHz x D, in the rear arc;
//   - the inlet whine: the fan's blade-passing tone ~2.4 kHz x D with two partials and a little noise, front arc only;
//   - the airframe's whoosh at close range (< 90 m), a broad band at ~1.1 kHz x sqrt(D);
//   - the pressure felt through the airframe (< 120 m): a 30-60 Hz rumble made audible on small speakers by its own
//     harmonics (a soft saturation, then a band at ~110 Hz: the Quest's speakers give little below ~150 Hz, so the
//     ear hears the low pulse through its harmonics, the "missing fundamental"); mono, not placed (felt, not heard).
//   Level ~ 20 m / distance (flat inside 20 m), air absorption (a low-pass 16 kHz / (1 + r / 120 m)), the canopy's
//   muffling in the cockpit view (ACZ_AUDIO_MUFFLE, as the game's outside sounds), binaural placement with the
//   spatial mixer's head model (ACZ_AUDIO_HRTF; Woodworth delay + head shadow; a constant-power pan when off), the
//   immersion bus's pilot-class ducking (ps2_imm_duck(1)) and 4 dB under radio speech.
// ACZ_SND_FLYBY="level[,doppler]" (default 1,0.5; 0 = off; level 0..2). ACZ_SND_FLYBY_TEST="ahead_m,miss_m,closing_ms,
// period_s[,first_field]" (test): a synthetic aircraft on a straight line past the player. ACZ_DEBUG_FLYBY=N: a line
// every N fields while a voice sounds, and the passes (closest distance, closing speed, peak level).
#include "ps2_runtime.h"
#include "ps2_menv.h"
#ifndef ZF_OFFLINE
#include "ps2_hook.h"
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ZF_RATE   48000
#define ZF_SLOTS  3
#define ZF_MAX_AC 48
#define ZF_HIST   128                /* fields of positions per aircraft: 2.1 s, the sound's travel from up to ~730 m */
#define ZF_C      343.0f
#define ZF_UPM    6.656f             /* game units per metre: the airspeed's scale (the HUD speeds), see the report */
#define ZF_RANGE  600.0f             /* m from the emission point: voices fade in from here to 450 m */
#define ZF_PI     3.14159265f
#define ZF_GAIN   2.8f               /* the layer's level: a 30 m head-on pass peaks near -9 dBFS (50 ms) in the cockpit */

u64 ps2_kernel_vblank_count(void);
int ps2_spatial_hrtf_on(void);
float ps2_spatial_muffle(void);
float ps2_imm_duck(int cls);
int ps2_nustream_radio_active(void);

/* ---- the contract: EE -> audio ---- */
typedef struct {
    u32 key;                         /* the aircraft (the address of its position vector); 0 = empty */
    float dir[3];                    /* unit vector to the emission point, head axes (x right, y up, z front) */
    float r;                         /* m, from the emission point */
    float dop;                       /* frequency ratio, strength applied */
    float cth;                       /* cos(flight direction, direction to the listener): +1 ahead of it, -1 behind */
    float speed;                     /* m/s */
    float closing;                   /* m/s, + approaching */
    float vrel;                      /* m/s, the speed relative to the listener (a wingman in formation: ~0) */
} zf_src;
typedef struct { u64 field; float cockpit; zf_src s[ZF_SLOTS]; } zf_frame;
static zf_frame zf_buf[2];
static volatile u32 zf_seq;
static int zf_on, zf_dbg;
static float zf_level = 1.0f, zf_dk = 0.5f;

static void zf_publish(const zf_frame *f) {
    u32 s = zf_seq;
    zf_buf[(s + 1u) & 1u] = *f;
    __atomic_store_n(&zf_seq, s + 1u, __ATOMIC_RELEASE);
}
static int zf_take(zf_frame *out) {
    u32 s = __atomic_load_n(&zf_seq, __ATOMIC_ACQUIRE);
    if (!s) return 0;
    *out = zf_buf[s & 1u];
    return __atomic_load_n(&zf_seq, __ATOMIC_ACQUIRE) == s;   /* (a torn copy: keep the old frame) */
}

/* ---- the geometry (EE side, also used by the offline renderer) ---- */
typedef struct {
    u32 key; u64 last;               /* last field seen */
    int n, wp;                       /* positions held, next write */
    float p[ZF_HIST][3];             /* units */
    u64 f[ZF_HIST];
    float dmin, cmax; u64 fmin;      /* (debug) the pass: closest distance, closing speed */
} zf_ac_t;
static zf_ac_t zf_ac[ZF_MAX_AC];
static float zf_lh[4][3]; static u64 zf_lhf[4]; static int zf_lhn;   /* the listener's last positions */

static float zf_dot(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static zf_ac_t *zf_slot(u32 key, u64 f) {
    zf_ac_t *fr = NULL;
    for (int i = 0; i < ZF_MAX_AC; i++) {
        if (zf_ac[i].key == key) return &zf_ac[i];
        if (!fr && (!zf_ac[i].key || f - zf_ac[i].last > 120u)) fr = &zf_ac[i];
    }
    if (fr) { memset(fr, 0, sizeof *fr); fr->key = key; fr->dmin = 1e9f; }
    return fr;
}
static void zf_push(zf_ac_t *a, const float *p, u64 f) {
    if (a->n && f - a->last > 4u) a->n = 0;                     /* (a gap: start over) */
    if (a->n) {                                                  /* (a jump: a respawn, a teleport) */
        const float *q = a->p[(a->wp + ZF_HIST - 1) % ZF_HIST];
        float d[3] = { p[0] - q[0], p[1] - q[1], p[2] - q[2] };
        if (zf_dot(d, d) > (1500.0f * ZF_UPM) * (1500.0f * ZF_UPM) / 3600.0f * (float)((f - a->last) * (f - a->last))) a->n = 0;
    }
    memcpy(a->p[a->wp], p, sizeof a->p[0]); a->f[a->wp] = f;
    a->wp = (a->wp + 1) % ZF_HIST;
    if (a->n < ZF_HIST) a->n++;
    a->last = f;
}
/* the retarded geometry of one aircraft for the listener L (units: pos [0..2], front [6..8], up [9..11]) at field f;
   vl = the listener's velocity (m/s, world). Returns 0 when out of reach. */
static int zf_geom(const zf_ac_t *a, const float *L, const float *vl, u64 f, zf_src *o) {
    if (a->n < 4) return 0;
    /* newest first: k = 0 .. n-1 */
    float gprev = 0.0f, pe[3] = { 0, 0, 0 }, ve[3] = { 0, 0, 0 };
    int found = 0;
    for (int k = 0; k < a->n; k++) {
        const float *x = a->p[(a->wp + ZF_HIST - 1 - k) % ZF_HIST];
        float dx[3] = { x[0] - L[0], x[1] - L[1], x[2] - L[2] };
        float d = sqrtf(zf_dot(dx, dx)) / ZF_UPM;
        float tr = (float)(f - a->f[(a->wp + ZF_HIST - 1 - k) % ZF_HIST]) / 60.0f;
        float g = d - ZF_C * tr;                                  /* > 0: the sound from there is still on its way */
        if (k > 0 && gprev > 0.0f && g <= 0.0f) {
            const float *xn = a->p[(a->wp + ZF_HIST - k) % ZF_HIST];
            float w = gprev / (gprev - g);                        /* between k-1 (w = 0) and k (w = 1) */
            for (int j = 0; j < 3; j++) pe[j] = xn[j] + (x[j] - xn[j]) * w;
            int k0 = k - 1, k2 = k + 2 < a->n ? k + 2 : a->n - 1;   /* the velocity over ~3 fields about there */
            if (k2 <= k0) return 0;
            const float *xa = a->p[(a->wp + ZF_HIST - 1 - k0) % ZF_HIST], *xb = a->p[(a->wp + ZF_HIST - 1 - k2) % ZF_HIST];
            float dtf = (float)(a->f[(a->wp + ZF_HIST - 1 - k0) % ZF_HIST] - a->f[(a->wp + ZF_HIST - 1 - k2) % ZF_HIST]);
            if (dtf < 0.5f) return 0;
            for (int j = 0; j < 3; j++) ve[j] = (xa[j] - xb[j]) / dtf * 60.0f / ZF_UPM;
            found = 1;
            break;
        }
        if (k == 0 && g <= 0.0f) {                                /* (inside a field's travel: the newest point) */
            memcpy(pe, x, sizeof pe);
            const float *xb = a->p[(a->wp + ZF_HIST - 4) % ZF_HIST];
            float dtf = (float)(a->f[(a->wp + ZF_HIST - 1) % ZF_HIST] - a->f[(a->wp + ZF_HIST - 4) % ZF_HIST]);
            if (dtf < 0.5f) return 0;
            for (int j = 0; j < 3; j++) ve[j] = (x[j] - xb[j]) / dtf * 60.0f / ZF_UPM;
            found = 1;
            break;
        }
        gprev = g;
    }
    if (!found) return 0;
    float h[3] = { (pe[0] - L[0]) / ZF_UPM, (pe[1] - L[1]) / ZF_UPM, (pe[2] - L[2]) / ZF_UPM };
    float r = sqrtf(zf_dot(h, h));
    if (!(r > 0.01f && r < 2000.0f)) return 0;
    float n[3] = { -h[0] / r, -h[1] / r, -h[2] / r };             /* from the source to the listener */
    const float *LF = L + 6, *LU = L + 9;
    float LR[3] = { LF[1] * LU[2] - LF[2] * LU[1], LF[2] * LU[0] - LF[0] * LU[2], LF[0] * LU[1] - LF[1] * LU[0] };
    float lm = sqrtf(zf_dot(LR, LR));
    if (lm < 1e-6f) return 0;
    for (int j = 0; j < 3; j++) LR[j] /= lm;
    o->dir[0] = zf_dot(h, LR) / r; o->dir[1] = zf_dot(h, LU) / r; o->dir[2] = zf_dot(h, LF) / r;
    o->r = r;
    float vs = zf_dot(ve, n), vlr = zf_dot(vl, n);                 /* toward the listener; the listener away from it */
    float den = ZF_C - vs; if (den < 0.15f * ZF_C) den = 0.15f * ZF_C;
    float D = (ZF_C - vlr) / den;
    D = D < 0.2f ? 0.2f : D > 6.0f ? 6.0f : D;
    float dk = powf(D, zf_dk);
    o->dop = dk < 0.4f ? 0.4f : dk > 2.5f ? 2.5f : dk;
    float sp = sqrtf(zf_dot(ve, ve));
    o->speed = sp;
    o->cth = sp > 1.0f ? vs / sp : 0.0f;
    o->closing = vs - vlr;
    {   float d[3] = { ve[0] - vl[0], ve[1] - vl[1], ve[2] - vl[2] }; o->vrel = sqrtf(zf_dot(d, d)); }
    o->key = a->key;
    return 1;
}
/* one field: the aircraft seen (keys + positions, units) and the listener -> the published frame */
static void zf_frame_make(u64 f, const float *L, float cockpit, int nac, const u32 *keys, float (*pos)[3], zf_frame *out) {
    /* the listener's velocity from its last positions */
    float vl[3] = { 0, 0, 0 };
    if (zf_lhn && f - zf_lhf[(zf_lhn - 1) & 3] > 4u) zf_lhn = 0;
    memcpy(zf_lh[zf_lhn & 3], L, sizeof zf_lh[0]); zf_lhf[zf_lhn & 3] = f; zf_lhn++;
    if (zf_lhn >= 4) {
        const float *a = zf_lh[(zf_lhn - 1) & 3], *b = zf_lh[(zf_lhn - 4) & 3];
        float dtf = (float)(zf_lhf[(zf_lhn - 1) & 3] - zf_lhf[(zf_lhn - 4) & 3]);
        if (dtf > 0.5f) for (int j = 0; j < 3; j++) vl[j] = (a[j] - b[j]) / dtf * 60.0f / ZF_UPM;
    }
    memset(out, 0, sizeof *out);
    out->field = f; out->cockpit = cockpit;
    zf_src best[ZF_SLOTS]; float bk[ZF_SLOTS]; int nb = 0;
    for (int i = 0; i < nac; i++) {
        zf_ac_t *a = zf_slot(keys[i], f);
        if (!a) continue;
        zf_push(a, pos[i], f);
        {   /* (far away now: no emission point within reach either; skip the search) */
            float dx[3] = { pos[i][0] - L[0], pos[i][1] - L[1], pos[i][2] - L[2] };
            if (zf_dot(dx, dx) > (2500.0f * ZF_UPM) * (2500.0f * ZF_UPM)) continue;
        }
        zf_src s;
        if (!zf_geom(a, L, vl, f, &s) || s.r > ZF_RANGE + 20.0f) continue;
        if (zf_dbg > 0 && s.r < a->dmin) { a->dmin = s.r; a->fmin = f; a->cmax = s.closing; }
        /* the three loudest to be: the nearest, counting aircraft keeping station as far (they stay silent) */
        float gate = (s.vrel - 20.0f) / 60.0f; gate = gate < 0.0f ? 0.0f : gate > 1.0f ? 1.0f : gate;
        float key = s.r / (0.05f + gate);
        if (nb < ZF_SLOTS) { best[nb] = s; bk[nb] = key; nb++; }
        else {
            int w = 0;
            for (int k = 1; k < ZF_SLOTS; k++) if (bk[k] > bk[w]) w = k;
            if (key < bk[w]) { best[w] = s; bk[w] = key; }
        }
    }
    for (int k = 0; k < nb; k++) out->s[k] = best[k];
}

/* ---- the audio side ---- */
typedef struct { float b0, b1, b2, a1, a2, z1, z2; } zf_bq;
static float zf_bq_run(zf_bq *f, float x) {
    float y = f->b0 * x + f->z1;
    f->z1 = f->b1 * x - f->a1 * y + f->z2;
    f->z2 = f->b2 * x - f->a2 * y;
    return y;
}
static void zf_bq_set(zf_bq *f, int kind, float fc, float q) {   /* RBJ: 0 band-pass (0 dB peak), 1 low-pass, 2 high-pass */
    if (fc > 0.45f * ZF_RATE) fc = 0.45f * ZF_RATE;
    if (fc < 10.0f) fc = 10.0f;
    float w = 2.0f * ZF_PI * fc / ZF_RATE, al = sinf(w) / (2.0f * q), c = cosf(w), a0 = 1.0f + al;
    if (kind == 0) { f->b0 = al / a0; f->b1 = 0.0f; f->b2 = -al / a0; }
    else if (kind == 1) { f->b0 = (1.0f - c) * 0.5f / a0; f->b1 = (1.0f - c) / a0; f->b2 = f->b0; }
    else { f->b0 = (1.0f + c) * 0.5f / a0; f->b1 = -(1.0f + c) / a0; f->b2 = f->b0; }
    f->a1 = -2.0f * c / a0; f->a2 = (1.0f - al) / a0;
}
/* binaural placement: as ps2_audio.c's db_bin (ps2_spatial.c's head model) */
#define ZF_RING 64u
#define ZF_HS_K (2.0f * (float)ZF_RATE / (2.0f * 343.0f / 0.0875f))
#define ZF_ITD  (0.0875f / 343.0f * (float)ZF_RATE)
typedef struct {
    float ring[ZF_RING]; u32 wp;
    float tau[2], al[2], g[2], dtau[2], dal[2], dg[2], hx[2], hy[2];
    int fresh;
} zf_bin;
static float zf_alpha(float c) {
    float th = acosf(c > 1.0f ? 1.0f : c < -1.0f ? -1.0f : c);
    float a = (1.05f + 0.95f * cosf(th * (180.0f / 150.0f))) * (1.0f / 0.7564f);
    return a < 0.08f ? 0.08f : a > 2.0f ? 2.0f : a;
}
static float zf_itd(float c) {
    float th = acosf(c > 1.0f ? 1.0f : c < -1.0f ? -1.0f : c);
    return 2.0f + ZF_ITD * (th < 1.5707963f ? 1.0f - c : 1.0f + th - 1.5707963f);
}
static void zf_bin_block(zf_bin *b, const float *dir, int hrtf) {
    float x = dir[0], tau[2], al[2], g[2];
    if (hrtf) { tau[0] = zf_itd(-x); tau[1] = zf_itd(x); al[0] = zf_alpha(-x); al[1] = zf_alpha(x); g[0] = g[1] = 1.0f; }
    else {
        float p = 0.5f + 0.5f * (x > 1.0f ? 1.0f : x < -1.0f ? -1.0f : x);
        tau[0] = tau[1] = 2.0f; al[0] = al[1] = 1.0f;
        g[0] = cosf(p * 1.5707963f) * 1.4142136f; g[1] = sinf(p * 1.5707963f) * 1.4142136f;
    }
    for (int e = 0; e < 2; e++) {
        if (b->fresh) { b->tau[e] = tau[e]; b->al[e] = al[e]; b->g[e] = g[e]; b->dtau[e] = b->dal[e] = b->dg[e] = 0.0f; }
        else { b->dtau[e] = (tau[e] - b->tau[e]) / 32.0f; b->dal[e] = (al[e] - b->al[e]) / 32.0f; b->dg[e] = (g[e] - b->g[e]) / 32.0f; }
    }
    b->fresh = 0;
}
static void zf_bin_run(zf_bin *b, float s, float *l, float *r) {
    b->ring[b->wp & (ZF_RING - 1u)] = s;
    float out[2];
    for (int e = 0; e < 2; e++) {
        float d = b->tau[e];
        int n = (int)d; float fr = d - (float)n;
        float p0 = b->ring[(b->wp - (u32)n) & (ZF_RING - 1u)], p1 = b->ring[(b->wp - (u32)n - 1u) & (ZF_RING - 1u)];
        float y = p0 + fr * (p1 - p0);
        float h = ((y + b->hx[e]) + b->al[e] * ZF_HS_K * (y - b->hx[e])) * (1.0f / (1.0f + ZF_HS_K))
                  - ((1.0f - ZF_HS_K) / (1.0f + ZF_HS_K)) * b->hy[e];
        b->hx[e] = y; b->hy[e] = h;
        out[e] = h * b->g[e];
        b->tau[e] += b->dtau[e]; b->al[e] += b->dal[e]; b->g[e] += b->dg[e];
    }
    b->wp++;
    *l += out[0]; *r += out[1];
}

typedef struct {
    u32 key; int on;                 /* on: 1 sounding, -1 fading out */
    float dir[3], r, dop, cth, spd, vrel;   /* smoothed (per block) */
    float t_dir[3], t_r, t_dop, t_cth, t_spd, t_vrel, t_live;
    float live;                      /* 0..1: the voice's presence (fades in / out) */
    u32 rng;
    float pk[3];                     /* pink noise (Kellet's economy filter) */
    zf_bq roar, roar_lp, crk_hp, crk_lp, wh_bp, air_bp, rum_lp1, rum_lp2, rum_bp;
    float crk_x;                     /* crackle impulse decay */
    float wph[2], wf0;               /* whine partials' phases; the blade-passing frequency of this engine */
    float wjit;                      /* slow random wander of the whine */
    float absl, mlp;                 /* air absorption, muffle low-pass (mono, before the placement) */
    float a_roar, a_crk, a_wh, a_air, a_rum, g;   /* per block */
    float abs_a;
    u32 nblk;
    zf_bin b;
    float peak_ms;                   /* (debug) loudest 10 ms block (dBFS), r and dop there */
    float pk_r, pk_dop;
} zf_voice;
static struct {
    int init;
    zf_voice v[ZF_SLOTS];
    float duck, radio_s, mu_s;
    u32 rng;
    double e_sum; u32 n_win; u64 n_blocks, n_live;
    double mix_ns, mix_samples;          /* (report) the mixer's CPU */
} zfa;
static double zf_now_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec; }
static float zf_n(u32 *s) { *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5; return (float)(*s >> 8) * (2.0f / 16777216.0f) - 1.0f; }
static float zf_u(u32 *s) { return 0.5f + 0.5f * zf_n(s); }
static float zf_ss(float a, float b, float x) { float t = (x - a) / (b - a); t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t; return t * t * (3.0f - 2.0f * t); }

static void zf_voice_start(zf_voice *v, const zf_src *s) {
    memset(v, 0, sizeof *v);
    v->key = s->key; v->on = 1;
    zfa.rng = zfa.rng * 1664525u + 1013904223u;
    v->rng = (s->key * 2654435761u) ^ zfa.rng; if (!v->rng) v->rng = 1u;
    memcpy(v->dir, s->dir, sizeof v->dir); v->r = s->r; v->dop = s->dop; v->cth = s->cth; v->spd = s->speed; v->vrel = s->vrel;
    v->wf0 = 2100.0f + 700.0f * zf_u(&v->rng);
    v->b.fresh = 1;
    zf_bq_set(&v->rum_lp1, 1, 55.0f, 0.7f); zf_bq_set(&v->rum_lp2, 1, 55.0f, 0.7f); zf_bq_set(&v->rum_bp, 0, 140.0f, 0.9f);
    zf_bq_set(&v->crk_hp, 2, 700.0f, 0.7f); zf_bq_set(&v->crk_lp, 1, 7000.0f, 0.7f);
    v->peak_ms = -200.0f;
}
/* per block (32 samples): the targets glide, the filters follow the Doppler and the distance */
static void zf_voice_block(zf_voice *v, int hrtf) {
    const float k = 1.0f - expf(-32.0f / (0.025f * ZF_RATE));       /* 25 ms glides */
    for (int j = 0; j < 3; j++) v->dir[j] += (v->t_dir[j] - v->dir[j]) * k;
    float dm = sqrtf(v->dir[0] * v->dir[0] + v->dir[1] * v->dir[1] + v->dir[2] * v->dir[2]);
    if (dm > 1e-4f) for (int j = 0; j < 3; j++) v->dir[j] /= dm; else v->dir[2] = 1.0f;
    v->r += (v->t_r - v->r) * k; v->dop += (v->t_dop - v->dop) * k; v->cth += (v->t_cth - v->cth) * k; v->spd += (v->t_spd - v->spd) * k;
    v->vrel += (v->t_vrel - v->vrel) * k;
    v->live += (v->t_live - v->live) * (1.0f - expf(-32.0f / (0.06f * ZF_RATE)));
    float D = v->dop, r = v->r < 1.0f ? 1.0f : v->r;
    /* the jet: power from the speed (the AI's throttle is unknown: military power, more when fast) */
    float pw = zf_ss(60.0f, 260.0f, v->spd) * 0.7f + 0.3f;
    float rear = expf(-((v->cth + 0.75f) / 0.45f) * ((v->cth + 0.75f) / 0.45f));          /* jet noise peak ~140 deg */
    float rear_c = expf(-((v->cth + 0.80f) / 0.32f) * ((v->cth + 0.80f) / 0.32f));        /* crackle: narrower */
    float front = zf_ss(-0.1f, 0.8f, v->cth);
    v->a_roar = (0.35f + 0.65f * rear) * pw;
    v->a_crk = rear_c * pw * zf_ss(150.0f, 280.0f, v->spd > 150.0f ? v->spd : 150.0f + 0.5f * v->spd) * 0.9f;
    v->a_wh = front * 0.22f * (0.5f + 0.5f * pw);
    v->a_air = zf_ss(90.0f, 15.0f, r) * zf_ss(60.0f, 250.0f, v->spd);
    v->a_rum = zf_ss(120.0f, 20.0f, r) * (0.5f + 0.5f * rear) * zf_ss(60.0f, 250.0f, v->spd);
    float gd = 20.0f / (r > 20.0f ? r : 20.0f);
    gd *= 1.0f - zf_ss(ZF_RANGE - 150.0f, ZF_RANGE, r);
    /* a pass, not company: an aircraft keeping station (a wingman in formation) adds nothing to the game's own engine
       sound; the layer comes in with the speed relative to the listener (20 -> 80 m/s) */
    v->g = gd * v->live * zf_ss(20.0f, 80.0f, v->vrel);
    float fr = (330.0f + 150.0f * (1.0f - rear)) * D;                                  /* (>= ~150 Hz receding: small speakers) */
    if ((v->nblk++ & 3u) == 0u) {         /* (the filters every 4th block, 2.7 ms: the Doppler glides over 25 ms) */
        zf_bq_set(&v->roar, 0, fr, 0.5f);
        zf_bq_set(&v->roar_lp, 1, 2000.0f * D, 0.6f);
        if (v->a_crk > 1e-3f) { zf_bq_set(&v->crk_hp, 2, 700.0f * D, 0.7f); zf_bq_set(&v->crk_lp, 1, 7000.0f * D, 0.7f); }
        if (v->a_wh > 1e-3f) zf_bq_set(&v->wh_bp, 0, v->wf0 * D, 12.0f);
        if (v->a_air > 1e-3f) zf_bq_set(&v->air_bp, 0, 1100.0f * sqrtf(D), 0.7f);
        float fa = 16000.0f / (1.0f + r / 120.0f);
        v->abs_a = 1.0f - expf(-2.0f * ZF_PI * fa / ZF_RATE);
    }
    zf_bin_block(&v->b, v->dir, hrtf);
}
/* one sample of a voice: placed (l, r) and the felt part (mono) */
static void zf_voice_sample(zf_voice *v, float mu, float ma, float mg, float *l, float *r, float *mono) {
    if (v->g <= 1e-6f) return;                 /* (silent: a wingman keeping station, out of reach) */
    const float dt = 1.0f / ZF_RATE;
    float w = zf_n(&v->rng);
    /* pink noise (-3 dB / octave) */
    v->pk[0] = 0.99765f * v->pk[0] + w * 0.0990460f;
    v->pk[1] = 0.96300f * v->pk[1] + w * 0.2965164f;
    v->pk[2] = 0.57000f * v->pk[2] + w * 1.0526913f;
    float pink = (v->pk[0] + v->pk[1] + v->pk[2] + w * 0.1848f) * 0.25f;
    float s = 0.0f;
    /* the roar */
    s += (zf_bq_run(&v->roar, pink) * 1.4f + zf_bq_run(&v->roar_lp, pink) * 0.45f) * v->a_roar;
    /* the crackle: skewed impulses, ~600 a second */
    if (v->a_crk > 1e-3f && zf_u(&v->rng) < 600.0f * dt) {
        float a = -logf(zf_u(&v->rng) + 1e-6f); a = a > 4.0f ? 4.0f : a;
        v->crk_x += zf_u(&v->rng) < 0.75f ? a : -0.35f * a;
    }
    if (v->a_crk > 1e-3f) {
        float c = zf_bq_run(&v->crk_lp, zf_bq_run(&v->crk_hp, v->crk_x));
        v->crk_x *= 0.6f;
        s += c * 1.4f * v->a_crk;
    }
    /* the inlet whine: the blade-passing tone and its second partial, wandering slightly, with narrow noise */
    if (v->a_wh > 1e-3f) {
        v->wjit += (zf_n(&v->rng) - v->wjit) * 0.0005f;
        float f0 = v->wf0 * v->dop * (1.0f + 0.004f * v->wjit * 40.0f);
        v->wph[0] += f0 * dt; if (v->wph[0] >= 1.0f) v->wph[0] -= 1.0f;
        v->wph[1] += 2.0f * f0 * dt; if (v->wph[1] >= 1.0f) v->wph[1] -= 1.0f;
        float tone = sinf(2.0f * ZF_PI * v->wph[0]) * 0.6f + sinf(2.0f * ZF_PI * v->wph[1]) * 0.25f;
        s += (tone * 0.12f + zf_bq_run(&v->wh_bp, w) * 0.5f) * v->a_wh;
    }
    /* the airframe's whoosh */
    if (v->a_air > 1e-3f) s += zf_bq_run(&v->air_bp, w) * 0.9f * v->a_air;
    /* air absorption, the canopy */
    v->absl += v->abs_a * (s - v->absl);
    s = v->absl;
    if (mu > 0.0f) { v->mlp += ma * (s - v->mlp); s = (s + mu * (v->mlp - s)) * mg; }
    s *= v->g * ZF_GAIN;
    zf_bin_run(&v->b, s, l, r);
    /* the felt pressure: a 30-60 Hz rumble and its harmonics (soft saturation -> a band at ~140 Hz) */
    if (v->a_rum > 1e-3f) {
        float x = zf_bq_run(&v->rum_lp2, zf_bq_run(&v->rum_lp1, w)) * 9.0f;
        float h = tanhf(2.5f * x);
        *mono += (x * 0.5f + zf_bq_run(&v->rum_bp, h) * 0.8f) * v->a_rum * v->g * ZF_GAIN * 0.9f;
    }
}

/* the mixer (SDL audio thread): ps2_audio.c calls it through ps2_audio_flyby_mix after the cockpit system sounds */
static void zf_mix_body(s16 *buf, int frames);
static void zf_mix(s16 *buf, int frames) {
    double t0 = zf_now_ns();
    zf_mix_body(buf, frames);
    zfa.mix_ns += zf_now_ns() - t0; zfa.mix_samples += frames;
}
static void zf_mix_body(s16 *buf, int frames) {
    if (!zfa.init) { zfa.init = 1; zfa.rng = 0x7F4A7C15u; zfa.duck = 1.0f; }
    zf_frame fr;
    int have = zf_take(&fr);
    u64 now = ps2_kernel_vblank_count();
    int fresh = have && now - fr.field <= 8u;
    /* match the published aircraft to the voices */
    for (int i = 0; i < ZF_SLOTS; i++) zfa.v[i].t_live = 0.0f;
    if (fresh)
        for (int k = 0; k < ZF_SLOTS; k++) {
            const zf_src *s = &fr.s[k];
            if (!s->key) continue;
            zf_voice *v = NULL;
            for (int i = 0; i < ZF_SLOTS; i++) if (zfa.v[i].on && zfa.v[i].key == s->key) v = &zfa.v[i];
            if (!v) {
                for (int i = 0; i < ZF_SLOTS && !v; i++) if (!zfa.v[i].on) v = &zfa.v[i];
                if (!v) continue;
                zf_voice_start(v, s);
            }
            memcpy(v->t_dir, s->dir, sizeof v->t_dir);
            v->t_r = s->r; v->t_dop = s->dop; v->t_cth = s->cth; v->t_spd = s->speed; v->t_vrel = s->vrel; v->t_live = 1.0f; v->on = 1;
        }
    int any = 0;
    for (int i = 0; i < ZF_SLOTS; i++) if (zfa.v[i].on) any = 1;
    if (!any) return;
    int hrtf = ps2_spatial_hrtf_on();
    float mu = have && fr.cockpit > 0.5f ? ps2_spatial_muffle() : 0.0f;
    mu = mu < 0.0f ? 0.0f : mu > 1.0f ? 1.0f : mu;
    const float ma = 1.0f - expf(-2.0f * ZF_PI * 3000.0f / ZF_RATE), mg = 1.0f - mu * (1.0f - 0.708f);
    float T = (float)frames / ZF_RATE;
    int radio = ps2_nustream_radio_active();
    zfa.radio_s += ((radio ? 1.0f : 0.0f) - zfa.radio_s) * (1.0f - expf(-T / (radio ? 0.08f : 0.4f)));
    float duck = ps2_imm_duck(1) * (1.0f - 0.369f * zfa.radio_s) * zf_level;
    for (int i = 0; i < frames; i++) {
        if ((i & 31) == 0)
            for (int k = 0; k < ZF_SLOTS; k++) if (zfa.v[k].on) zf_voice_block(&zfa.v[k], hrtf);
        float l = 0.0f, r = 0.0f, mono = 0.0f;
        for (int k = 0; k < ZF_SLOTS; k++) {
            zf_voice *v = &zfa.v[k];
            if (!v->on) continue;
            zf_voice_sample(v, mu, ma, mg, &l, &r, &mono);
        }
        l = (l + mono) * duck; r = (r + mono) * duck;
        /* a soft limit (the crackle's spikes, several passes at once): linear to -9 dBFS, at most -4.4 */
        if (fabsf(l) > 0.35f) l = copysignf(0.35f + 0.25f * tanhf((fabsf(l) - 0.35f) / 0.25f), l);
        if (fabsf(r) > 0.35f) r = copysignf(0.35f + 0.25f * tanhf((fabsf(r) - 0.35f) / 0.25f), r);
        zfa.e_sum += 0.5 * ((double)l * l + (double)r * r);
        int L = buf[2 * i] + (int)(l * 32767.0f), R = buf[2 * i + 1] + (int)(r * 32767.0f);
        buf[2 * i] = (s16)(L > 32767 ? 32767 : L < -32768 ? -32768 : L);
        buf[2 * i + 1] = (s16)(R > 32767 ? 32767 : R < -32768 ? -32768 : R);
    }
    zfa.n_win += (u32)frames;
    for (int k = 0; k < ZF_SLOTS; k++) {
        zf_voice *v = &zfa.v[k];
        if (v->on && v->t_live <= 0.0f && v->live < 1e-3f) v->on = 0;
    }
}

#ifndef ZF_OFFLINE
/* ---- game side ---- */
#define ZF_BURNER   0x001386D0u
#define ZF_HUD_TICK 0x0024D9D8u
extern volatile int ps2_vk_cockpit_view;
static float zf_f(u32 a) { u32 w = ps2_r32(a); float f; memcpy(&f, &w, 4); return f; }
static int zf_ptr(u32 a) { return a >= 0x100000u && a < 0x2000000u; }
static u32 zf_calls[ZF_MAX_AC]; static int zf_ncalls;
static u64 zf_tick_field, zf_frames, zf_pub_n;
static struct { int on; float ahead, miss, closing, per; u64 first, start; float p0[3], dirw[3], rtw[3]; } zf_t;
static int zf_burner(ps2_ctx *ctx, void *user) {
    (void)user;
    u32 a2 = ctx->r[6].uw[0];
    if (!zf_ptr(a2)) return 0;
    for (int i = 0; i < zf_ncalls; i++) if (zf_calls[i] == a2) return 0;
    if (zf_ncalls < ZF_MAX_AC) zf_calls[zf_ncalls++] = a2;
    return 0;
}
static double zf_tick_ns, zf_tick_max;
static int zf_tick_body(ps2_ctx *ctx);
static int zf_tick(ps2_ctx *ctx, void *user) {
    (void)user;
    double t0 = zf_now_ns();
    int r = zf_tick_body(ctx);
    double d = zf_now_ns() - t0;
    zf_tick_ns += d; if (d > zf_tick_max) zf_tick_max = d;
    return r;
}
static int zf_tick_body(ps2_ctx *ctx) {
    u64 f = ps2_kernel_vblank_count();
    if (f == zf_tick_field) return 0;
    zf_tick_field = f;
    u32 player = ctx->r[4].uw[0] - 0x1310u;
    int n = 0;
    u32 keys[ZF_MAX_AC + 1]; float pos[ZF_MAX_AC + 1][3];
    for (int i = 0; i < zf_ncalls; i++) {
        u32 a = zf_calls[i];
        if (zf_ptr(player) && a == player + 0x70u) continue;           /* (the player) */
        float p[3] = { zf_f(a), zf_f(a + 4u), zf_f(a + 8u) };
        if (!(fabsf(p[0]) < 1e7f && fabsf(p[1]) < 1e7f && fabsf(p[2]) < 1e7f)) continue;
        keys[n] = a; memcpy(pos[n], p, sizeof pos[n]); n++;
    }
    zf_ncalls = 0;
    float L[12];
    for (u32 i = 0; i < 12; i++) L[i] = zf_f(0x00422C18u + 4u * i);
    if (!(fabsf(L[0]) < 1e7f && fabsf(L[1]) < 1e7f && fabsf(L[2]) < 1e7f)) return 0;
    /* ACZ_SND_FLYBY_TEST: a synthetic aircraft on a straight line past the player (along the aircraft's heading at the
       pass's start: ahead_m ahead, miss_m to the right, closing at closing_ms) */
    if (zf_t.on && zf_ptr(player) && f >= zf_t.first) {
        float E[3] = { zf_f(player + 0x70u), zf_f(player + 0x74u), zf_f(player + 0x78u) };
        float per = zf_t.per * 60.0f;
        if (!zf_t.start || (float)(f - zf_t.start) >= per) {
            zf_t.start = f;
            const float *LF = L + 6, *LU = L + 9;
            /* the listener's front levelled: the pass along the horizon, the miss to the right */
            float fw[3] = { LF[0], 0.0f, LF[2] }, m = sqrtf(fw[0] * fw[0] + fw[2] * fw[2]);
            if (m < 1e-3f) { fw[0] = 0.0f; fw[2] = -1.0f; m = 1.0f; }
            for (int j = 0; j < 3; j++) fw[j] /= m;
            float rt[3] = { fw[1] * LU[2] - fw[2] * LU[1], fw[2] * LU[0] - fw[0] * LU[2], fw[0] * LU[1] - fw[1] * LU[0] };
            float rm = sqrtf(rt[0] * rt[0] + rt[1] * rt[1] + rt[2] * rt[2]);
            if (rm < 1e-3f) { rt[0] = -fw[2]; rt[1] = 0.0f; rt[2] = fw[0]; rm = 1.0f; }
            for (int j = 0; j < 3; j++) { zf_t.dirw[j] = fw[j]; zf_t.rtw[j] = rt[j] / rm; }
            if (zf_dbg > 0) ps2_log("flyby: test pass from field %llu: %.0f m ahead, %.0f m to the right, closing %.0f m/s",
                                     (unsigned long long)f, zf_t.ahead, zf_t.miss, zf_t.closing);
        }
        float t = (float)(f - zf_t.start) / 60.0f;
        /* relative to the player (co-moving frame): from ahead toward the player at the closing speed */
        for (int j = 0; j < 3; j++)
            pos[n][j] = E[j] + (zf_t.dirw[j] * (zf_t.ahead - zf_t.closing * t) + zf_t.rtw[j] * zf_t.miss) * ZF_UPM;
        keys[n] = 0xF1B70000u; n++;
    }
    zf_frame fr;
    zf_frame_make(f, L, ps2_vk_cockpit_view ? 1.0f : 0.0f, n, keys, pos, &fr);
    zf_publish(&fr);
    zf_frames++;
    if (fr.s[0].key) zf_pub_n++;
    if (zf_dbg > 0 && (f % (u64)zf_dbg) == 0u && fr.s[0].key)
        ps2_log("flyby: field %llu: %d aircraft, nearest %.0f m dir %.2f %.2f %.2f Doppler %.2f cos %.2f speed %.0f m/s "
                "closing %.0f m/s relative %.0f m/s%s", (unsigned long long)f, n, fr.s[0].r, fr.s[0].dir[0], fr.s[0].dir[1], fr.s[0].dir[2],
                fr.s[0].dop, fr.s[0].cth, fr.s[0].speed, fr.s[0].closing, fr.s[0].vrel, ps2_vk_cockpit_view ? " (cockpit)" : "");
    if (zf_dbg > 0 && (f % 600u) == 0u)
        ps2_log("flyby: field %llu: EE %.1f us per frame (max %.1f), mixer %.3f ms per second of audio", (unsigned long long)f,
                zf_tick_ns / 1e3 / (double)(zf_frames ? zf_frames : 1), zf_tick_max / 1e3,
                zfa.mix_samples > 0.0 ? zfa.mix_ns / 1e6 / (zfa.mix_samples / ZF_RATE) : 0.0);
    if (zf_dbg > 0)                           /* the passes: the closest point, once the aircraft has gone 2 s past it */
        for (int i = 0; i < ZF_MAX_AC; i++) {
            zf_ac_t *a = &zf_ac[i];
            if (a->key && a->dmin < 300.0f && f - a->fmin == 120u)
                ps2_log("flyby: pass field %llu: aircraft %08X closest %.1f m, closing %.0f m/s", (unsigned long long)a->fmin,
                        a->key, a->dmin, a->cmax);
            if (a->key && f - a->fmin > 120u) a->dmin = 1e9f;
        }
    return 0;
}
static void zf_report(void) {
    if (!zf_frames) return;
    double n = zfa.n_win ? (double)zfa.n_win : 1.0;
    ps2_log("flyby: EE %.1f us per frame (max %.1f), mixer %.3f ms per second of audio", zf_tick_ns / 1e3 / (double)zf_frames,
            zf_tick_max / 1e3, zfa.mix_samples > 0.0 ? zfa.mix_ns / 1e6 / (zfa.mix_samples / ZF_RATE) : 0.0);
    ps2_log("flyby: %llu frames, %llu with an aircraft within %.0f m; layer rms %.1f dBFS over %.1f s",
            (unsigned long long)zf_frames, (unsigned long long)zf_pub_n, ZF_RANGE,
            zfa.e_sum > 0.0 ? 10.0 * log10(zfa.e_sum / n) : -200.0, n / ZF_RATE);
}
int zero_vr_is_zero(void);
static void zf_read(void) {
    const char *e = ps2_menv("ACZ_SND_FLYBY");
    zf_level = 1.0f;
    if (e && *e) {
        float a = 1.0f, b = 0.5f;
        int k = sscanf(e, "%f,%f", &a, &b);
        zf_level = k >= 1 && a >= 0.0f ? (a > 2.0f ? 2.0f : a) : 1.0f;
        if (k >= 2 && b >= 0.0f && b <= 1.0f) zf_dk = b;
    }
}
/* the NEXT MISSION tier (ps2_menv.h): the game thread, at the edge; the mixer takes the level per chunk */
void zero_flyby_menv_reload(void) {
    const float a = zf_level;
    zf_read();
    ps2_menv_note("ACZ_SND_FLYBY", a, zf_level);
}
void zero_flyby_install(void) {
    if (!zero_vr_is_zero()) return;
    const char *e;
    zf_read();
    if (zf_level <= 0.0f) { ps2_log("flyby: close passes off (ACZ_SND_FLYBY=0)"); return; }
    e = getenv("ACZ_DEBUG_FLYBY"); zf_dbg = e ? atoi(e) : 0;
    e = getenv("ACZ_SND_FLYBY_TEST");
    if (e && *e) {
        zf_t.ahead = 600.0f; zf_t.miss = 30.0f; zf_t.closing = 300.0f; zf_t.per = 8.0f; zf_t.first = 1500u;
        unsigned long long ff = 1500ull;
        sscanf(e, "%f,%f,%f,%f,%llu", &zf_t.ahead, &zf_t.miss, &zf_t.closing, &zf_t.per, &ff);
        zf_t.first = (u64)ff;
        if (zf_t.per < 2.0f) zf_t.per = 2.0f;
        zf_t.on = 1;
    }
    if ((ps2_r32(ZF_BURNER) >> 16) != 0x27BDu || (ps2_r32(ZF_HUD_TICK) >> 16) != 0x27BDu) {
        ps2_log("flyby: code mismatch (%08X %08X): off", ps2_r32(ZF_BURNER), ps2_r32(ZF_HUD_TICK));
        return;
    }
    if (ps2_hook_before(ZF_BURNER, zf_burner, NULL, 45, "zero-flyby") < 0 || ps2_hook_before(ZF_HUD_TICK, zf_tick, NULL, 45, "zero-flyby") < 0) {
        ps2_log("flyby: hooks refused: off");
        return;
    }
    {   extern void (*ps2_audio_flyby_mix)(s16 *buf, int frames);
        ps2_audio_flyby_mix = zf_mix; }
    zf_on = 1;
    atexit(zf_report);
    ps2_log("flyby: close passes of other aircraft: level %.2f, Doppler %.2f of the real effect, within %.0f m%s",
            zf_level, zf_dk, ZF_RANGE, zf_t.on ? " (test passes: ACZ_SND_FLYBY_TEST)" : "");
}
#endif
