// ps2_spatial.c - spatial audio for the SPU2 mixer (bug #29): binaural rendering of the game's positional (3D) voices
// relative to the head, cockpit muffling, Doppler pitch glide, a synthetic test source and the granular engine
// prototype. Runs on the SDL audio thread inside ps2_spu2_mix; the game side (zero_audio3d.c) publishes the geometry
// once per game frame (ps2_spatial.h).
//
// Binaural model (own compact implementation, no measured data set): the spherical-head structural model of
// C. P. Brown and R. O. Duda, "A structural model for binaural sound synthesis", IEEE Trans. Speech Audio Proc. 6(5),
// 1998 -- per ear an interaural time delay from Woodworth's formula (head radius 8.75 cm: up to 0.66 ms) and the
// first-order head-shadow filter H(s) = (1 + a s / 2w0) / (1 + s / 2w0), w0 = c / r, whose high-frequency gain a
// goes from +6 dB facing the ear to about -18 dB in its shadow (normalised so a source straight ahead is flat), plus
// two pinna cues on the mono signal: a high-shelf cut for sources behind (pinna shadow, -5 dB above ~3 kHz) and an
// elevation notch (5-11 kHz; -8 dB below, -5 dB on the horizon, -2.5 dB overhead). The game's own level (distance attenuation, volume, its
// envelope) is kept: the mono level is the constant-power equivalent of the game's L/R pan, sqrt((L^2 + R^2) / 2)
// (its pan law is a sine table: L^2 + R^2 is constant across the pan). Directions are smoothed per 32-sample block
// (8 ms) and every filter parameter is ramped per sample: no clicks on fast head turns. A voice crossfades between
// the game's pan and the binaural path over 20 ms when it starts or stops being positional.
#include "ps2_spatial.h"
#include "ps2_settings.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

u64 ps2_kernel_vblank_count(void);
void ps2_spu2_decode_block(const u8 *blk, s16 *out, int *s1, int *s2, u32 *hdr);
extern u8 *ps2_spu2_ram;

#define SP_RATE     48000.0f
#define SP_BLOCK    32u
#define SP_RING     64u
#define SP_NV       (PS2_SPAT_VOICES + 1)

/* ---- stage switches (environment overrides the saved F4 > Immersion setting) ---- */
static float sp_env(const char *name, float v, int *seen, float *val) {
    if (!*seen) { const char *e = getenv(name); *seen = e && *e ? 1 : -1; if (*seen > 0) *val = (float)atof(e); }
    return *seen > 0 ? *val : v;
}
static int sp_xr(void) { static int x = -1; if (x < 0) { const char *e = getenv("PS2_XR"); x = e && *e == '1'; } return x; }
static int sp_selftest_on;            /* ps2_spatial_selftest: every stage on, whatever the settings */
/* ACZ_AUDIO_HRTF: 1 on, 0 off, 2 auto = on in VR (the headset's headphones), off in flat play (speakers) */
int ps2_spatial_hrtf_on(void) {
    static int s; static float v;
    int m = (int)sp_env("ACZ_AUDIO_HRTF", (float)ps2_cfg.imm_hrtf, &s, &v);
    return sp_selftest_on || (m == 2 ? sp_xr() : m != 0);
}
float ps2_spatial_doppler(void) { static int s; static float v; return sp_selftest_on ? 0.25f : sp_env("ACZ_AUDIO_DOPPLER", ps2_cfg.imm_doppler, &s, &v); }
float ps2_spatial_muffle(void) { static int s; static float v; return sp_selftest_on ? 1.0f : sp_env("ACZ_AUDIO_MUFFLE", ps2_cfg.imm_muffle, &s, &v); }
float ps2_spatial_engine_grain(void) { static int s; static float v; return sp_env("ACZ_IMM_ENGINE_GRAIN", ps2_cfg.imm_engine_grain, &s, &v); }

/* ---- the published frame: a seqlock (the EE never waits for the audio thread) ---- */
static ps2_spat_frame sp_pub;
static u32 sp_seq;
void ps2_spatial_publish(const ps2_spat_frame *f) {
    u32 s = __atomic_load_n(&sp_seq, __ATOMIC_RELAXED);
    __atomic_store_n(&sp_seq, s + 1u, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    memcpy(&sp_pub, f, sizeof sp_pub);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&sp_seq, s + 2u, __ATOMIC_RELEASE);
}
static ps2_spat_frame sp_fr;          /* the audio thread's copy */
static int sp_have;
static void sp_snapshot(void) {
    for (int tries = 0; tries < 4; tries++) {
        u32 a = __atomic_load_n(&sp_seq, __ATOMIC_ACQUIRE);
        if (a & 1u) continue;
        ps2_spat_frame tmp;
        memcpy(&tmp, &sp_pub, sizeof tmp);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&sp_seq, __ATOMIC_RELAXED) == a) {
            if (a) { sp_fr = tmp; sp_have = 1; }
            return;
        }
    }
}

/* ---- per-voice renderer state ---- */
typedef struct {
    int on;                     /* takes the spatial path this chunk */
    int bin_t;                  /* binaural target */
    float dir_t[3], mu_t, dm_t;
    float dir[3], w, mu, dm;
    float tau[2], dtau[2];      /* interaural delays in samples (incl. a 2-sample base for the interpolator) */
    float al[2], dal[2];        /* head-shadow high-frequency gain per ear */
    float fbg, dfbg;            /* rear shelf high-frequency gain */
    float b0, b1, b2, a1, a2;   /* elevation notch (per block) */
    float dir_c[3], tau_c[2], al_c[2], fbg_c;   /* the direction the targets were last computed for, and them */
    float ring[SP_RING];
    u32 wp;
    float hs_x1[2], hs_y1[2], fb_x1, fb_y1, z1, z2, lp1;
    int fresh;
    /* test source generator */
    u32 rng; float ph, pink, burst;
} sp_voice;
static sp_voice sp_v[SP_NV];
static int sp_hrtf, sp_valid;
static float sp_mu_amt, sp_mu_gain, sp_mu_a, sp_dopp;
static u64 sp_samples3d, sp_samples, sp_chunks;
static u32 sp_maxon;
/* granular engine state (see ps2_spatial_grain) */
#define GR_MAXLEN  (1 << 18)
#define GR_GRAINS  8
#define GR_WIN     1024
typedef struct {
    u32 ssa; int ok, len, loop0, cap;
    float *pcm;
    struct { double pos; float inc, amp; int t, dur; } g[GR_GRAINS];
    int ng;
    float next, lp, mix, rms_o, rms_g, gain;
    u32 rng;
} sp_grain;
static sp_grain sp_g[PS2_SPAT_VOICES];
static int sp_eng[PS2_SPAT_VOICES];
static float sp_grain_amt, sp_eng_thr, sp_eng_ab, sp_eng_spd;
static float sp_hann[GR_WIN + 1];
static u64 sp_grain_samples, sp_grains_started;

/* constants of the head-shadow and rear-shelf filters (bilinear transform at 48 kHz) */
#define HS_K   (2.0f * SP_RATE / (2.0f * 343.0f / 0.0875f))        /* 2fs / (2 w0) = 12.24 */
#define FB_K   (2.0f * SP_RATE / (6.2831853f * 3000.0f))           /* 2fs / w1, rear shelf corner 3 kHz */
#define ITD_S  (0.0875f / 343.0f * SP_RATE)                         /* a / c in samples = 12.24 */

static float sp_alpha(float c) {      /* head-shadow HF gain for an ear, c = cos(angle from the ear's axis) */
    float th = acosf(c > 1.0f ? 1.0f : c < -1.0f ? -1.0f : c);
    float a = 1.05f + 0.95f * cosf(th * (180.0f / 150.0f));
    a *= 1.0f / 0.7564f;                                           /* straight ahead (90 deg from both ears) flat */
    return a < 0.08f ? 0.08f : a > 2.0f ? 2.0f : a;
}
static float sp_itd(float c) {        /* Woodworth: delay of an ear (samples) relative to a far-side reference */
    float th = acosf(c > 1.0f ? 1.0f : c < -1.0f ? -1.0f : c);
    return 2.0f + ITD_S * (th < 1.5707963f ? 1.0f - c : 1.0f + th - 1.5707963f);
}
static void sp_notch(sp_voice *v, float el_s) {   /* el_s = sin(elevation) */
    /* the notch climbs with the elevation (5 kHz below, 8 kHz on the horizon, 11 kHz overhead) and is deepest below:
       -8 dB straight down, -5 dB on the horizon (a mild colouring of everything ahead), -2.5 dB overhead */
    float f0 = 8000.0f + 3000.0f * el_s, g = -5.0f - 3.0f * (el_s < 0.0f ? -el_s : 0.0f) + 2.5f * (el_s > 0.0f ? el_s : 0.0f);
    float A = powf(10.0f, g / 40.0f), w0 = 6.2831853f * f0 / SP_RATE, al = sinf(w0) / (2.0f * 2.0f), cw = cosf(w0);
    float a0 = 1.0f + al / A;
    v->b0 = (1.0f + al * A) / a0; v->b1 = -2.0f * cw / a0; v->b2 = (1.0f - al * A) / a0;
    v->a1 = -2.0f * cw / a0; v->a2 = (1.0f - al / A) / a0;
}
static void sp_targets(sp_voice *v, float *tau, float *al, float *fbg) {
    float x = v->dir[0], z = v->dir[2];
    tau[0] = sp_itd(-x); tau[1] = sp_itd(x);
    al[0] = sp_alpha(-x); al[1] = sp_alpha(x);
    float b = z < 0.0f ? -z : 0.0f;
    *fbg = powf(10.0f, -5.0f * b / 20.0f);
}

void ps2_spatial_note_on(u32 i) {
    if (i >= SP_NV) return;
    sp_voice *v = &sp_v[i];
    memset(v->ring, 0, sizeof v->ring);
    v->hs_x1[0] = v->hs_x1[1] = v->hs_y1[0] = v->hs_y1[1] = 0.0f;
    v->fb_x1 = v->fb_y1 = v->z1 = v->z2 = v->lp1 = 0.0f;
    v->fresh = 1;
}

void ps2_spatial_begin(u32 frames) {
    sp_snapshot();
    sp_chunks++;
    sp_samples += frames;
    sp_hrtf = ps2_spatial_hrtf_on();
    sp_dopp = ps2_spatial_doppler();
    float mu = ps2_spatial_muffle();
    sp_mu_amt = mu < 0.0f ? 0.0f : mu > 1.0f ? 1.0f : mu;
    {   static int s1, s2; static float v1, v2;
        float hz = sp_env("ACZ_AUDIO_MUFFLE_HZ", 3000.0f, &s1, &v1), db = sp_env("ACZ_AUDIO_MUFFLE_DB", 3.0f, &s2, &v2);
        sp_mu_a = 1.0f - expf(-6.2831853f * hz / SP_RATE);
        sp_mu_gain = powf(10.0f, -db / 20.0f);
    }
    u64 now = ps2_kernel_vblank_count();
    sp_valid = sp_have && now - sp_fr.field <= 15u;     /* older: paused or no flight -- back to the game's pan */
    {   float ga = ps2_spatial_engine_grain();
        sp_grain_amt = ga < 0.0f ? 0.0f : ga > 1.0f ? 1.0f : ga;
        memset(sp_eng, 0, sizeof sp_eng);
        if (sp_valid && sp_grain_amt > 0.0f)
            for (u32 j = 0; j < sp_fr.eng.n && j < 4u; j++)
                if (sp_fr.eng.voice[j] >= 0 && sp_fr.eng.voice[j] < PS2_SPAT_VOICES) sp_eng[(int)sp_fr.eng.voice[j]] = 1;
        float th = sp_fr.eng.throttle, kt = sp_fr.eng.speed_kt;
        sp_eng_thr = th < 0.0f ? 0.0f : th > 1.0f ? 1.0f : th;
        sp_eng_ab = sp_fr.eng.afterburner < 0.0f ? 0.0f : sp_fr.eng.afterburner > 1.0f ? 1.0f : sp_fr.eng.afterburner;
        sp_eng_spd = (kt - 250.0f) / 550.0f; sp_eng_spd = sp_eng_spd < 0.0f ? 0.0f : sp_eng_spd > 1.0f ? 1.0f : sp_eng_spd;
        if (sp_hann[GR_WIN / 2] == 0.0f)
            for (int k = 0; k <= GR_WIN; k++) sp_hann[k] = 0.5f - 0.5f * cosf(6.2831853f * (float)k / (float)GR_WIN);
    }
    float cockpit = sp_valid ? sp_fr.cockpit : 0.0f;
    u32 non = 0;
    for (u32 i = 0; i < SP_NV; i++) {
        sp_voice *v = &sp_v[i];
        const ps2_spat_src *s = &sp_fr.v[i];
        int is3d = sp_valid && s->is3d;
        if (v->dm == 0.0f) v->dm = 1.0f;
        if (is3d) { v->dir_t[0] = s->dir[0]; v->dir_t[1] = s->dir[1]; v->dir_t[2] = s->dir[2]; }
        v->bin_t = is3d && sp_hrtf;
        v->mu_t = is3d && !s->own ? cockpit * sp_mu_amt : 0.0f;
        v->dm_t = is3d && (sp_dopp > 0.0f || i == PS2_SPAT_TEST) ? s->dopp : 1.0f;   /* test: the game's factor too */
        if (!(v->dm_t > 0.25f && v->dm_t < 4.0f)) v->dm_t = 1.0f;
        int need = v->bin_t || v->w > 0.0f || v->mu_t > 0.0f || v->mu > 0.0f || v->dm_t != 1.0f || v->dm != 1.0f
                   || (i < PS2_SPAT_VOICES && (sp_eng[i] || sp_g[i].mix > 0.0f));
        if (i == PS2_SPAT_TEST) need = is3d;
        if (need && !v->on) {       /* entering the path: start from the game's pan, the target direction, clean filters */
            ps2_spatial_note_on(i);
            memcpy(v->dir, v->dir_t, sizeof v->dir);
        }
        v->on = need;
        if (!need) { v->w = 0.0f; v->mu = 0.0f; v->dm = 1.0f; }
        if (need && i < PS2_SPAT_VOICES) non++;
    }
    if (non > sp_maxon) sp_maxon = non;
    {   /* ACZ_DEBUG_SPATIAL=voice (test): about once a second that voice's renderer state */
        static int dv = -2;
        if (dv == -2) { const char *e = getenv("ACZ_DEBUG_SPATIAL"); dv = e ? atoi(e) : -1; }
        if (dv >= 0 && dv < (int)SP_NV && sp_chunks % 94u == 0u) {
            const sp_voice *v = &sp_v[dv];
            ps2_log("spatial: voice %d on %d bin %d w %.2f valid %d field %llu/%llu dir_t %.2f %.2f %.2f dir %.2f %.2f %.2f "
                    "tau %.1f %.1f al %.2f %.2f fb %.2f mu %.2f dm %.3f", dv, v->on, v->bin_t, v->w, sp_valid,
                    (unsigned long long)sp_fr.field, (unsigned long long)now, v->dir_t[0], v->dir_t[1], v->dir_t[2],
                    v->dir[0], v->dir[1], v->dir[2], v->tau[0], v->tau[1], v->al[0], v->al[1], v->fbg, v->mu, v->dm);
        }
    }
}

int ps2_spatial_voice_on(u32 i) { return i < PS2_SPAT_VOICES && sp_v[i].on; }

static void sp_block(sp_voice *v) {
    const float kd = 1.0f - expf(-(float)SP_BLOCK / (SP_RATE * 0.008f));   /* direction smoothing, 8 ms */
    float tau[2], al[2], fbg;
    if (v->fresh) memcpy(v->dir, v->dir_t, sizeof v->dir);
    else for (int j = 0; j < 3; j++) v->dir[j] += (v->dir_t[j] - v->dir[j]) * kd;
    float n = sqrtf(v->dir[0] * v->dir[0] + v->dir[1] * v->dir[1] + v->dir[2] * v->dir[2]);
    if (n > 1e-6f) for (int j = 0; j < 3; j++) v->dir[j] /= n; else { v->dir[0] = v->dir[1] = 0.0f; v->dir[2] = 1.0f; }
    /* the filter targets only when the direction moved (they cost a few transcendental functions per voice) */
    float dd = fabsf(v->dir[0] - v->dir_c[0]) + fabsf(v->dir[1] - v->dir_c[1]) + fabsf(v->dir[2] - v->dir_c[2]);
    float dt_ = fabsf(v->dir[0] - v->dir_t[0]) + fabsf(v->dir[1] - v->dir_t[1]) + fabsf(v->dir[2] - v->dir_t[2]);
    if (v->fresh || dd > 0.002f || (dd > 1e-6f && dt_ < 0.001f)) {        /* ... and once more when it settles */
        sp_targets(v, v->tau_c, v->al_c, &v->fbg_c);
        if (v->fresh || fabsf(v->dir[1] - v->dir_c[1]) > 0.002f) sp_notch(v, v->dir[1]);
        memcpy(v->dir_c, v->dir, sizeof v->dir_c);
    }
    tau[0] = v->tau_c[0]; tau[1] = v->tau_c[1]; al[0] = v->al_c[0]; al[1] = v->al_c[1]; fbg = v->fbg_c;
    if (v->fresh) {
        for (int e = 0; e < 2; e++) { v->tau[e] = tau[e]; v->al[e] = al[e]; v->dtau[e] = v->dal[e] = 0.0f; }
        v->fbg = fbg; v->dfbg = 0.0f;
        v->fresh = 0;
    } else {
        const float r = 1.0f / (float)SP_BLOCK;
        for (int e = 0; e < 2; e++) { v->dtau[e] = (tau[e] - v->tau[e]) * r; v->dal[e] = (al[e] - v->al[e]) * r; }
        v->dfbg = (fbg - v->fbg) * r;
    }
}

static float sp_test_sample(sp_voice *v, float dm);
static float sp_render(sp_voice *v, float x, float m, float gl, float gr, float *fl, float *fr) {
    /* x: the voice's sample (16-bit scale); m: mono level 0..1; gl/gr: the game's pan gains 0..1 */
    if (v->mu > 0.0f || v->mu_t > 0.0f) {
        v->lp1 += sp_mu_a * (x - v->lp1);              /* one pole: -3 dB at the corner, 6 dB per octave above */
        x += v->mu * (v->lp1 - x);
        x *= 1.0f - v->mu * (1.0f - sp_mu_gain);
        v->mu += (v->mu_t - v->mu) * (1.0f / (SP_RATE * 0.12f));             /* 120 ms fade on view changes */
        if (v->mu < 1e-4f && v->mu_t == 0.0f) v->mu = 0.0f;
    }
    if (v->w < 1.0f) { *fl += (1.0f - v->w) * x * gl; *fr += (1.0f - v->w) * x * gr; }
    if (v->w > 0.0f || v->bin_t) {
        float y = x * m;
        float t = v->b0 * y + v->z1;                                           /* elevation notch (TDF-II) */
        v->z1 = v->b1 * y - v->a1 * t + v->z2;
        v->z2 = v->b2 * y - v->a2 * t;
        y = ((t + v->fb_x1) + v->fbg * FB_K * (t - v->fb_x1)) * (1.0f / (1.0f + FB_K))
            - ((1.0f - FB_K) / (1.0f + FB_K)) * v->fb_y1;                     /* rear shelf */
        v->fb_x1 = t; v->fb_y1 = y;
        u32 wp = v->wp;
        v->ring[wp & (SP_RING - 1u)] = y;
        float out[2];
        for (int e = 0; e < 2; e++) {
            float d = v->tau[e];
            int n = (int)d; float f = d - (float)n;
            float pm = v->ring[(wp - (u32)n + 1u) & (SP_RING - 1u)], p0 = v->ring[(wp - (u32)n) & (SP_RING - 1u)],
                  p1 = v->ring[(wp - (u32)n - 1u) & (SP_RING - 1u)], p2 = v->ring[(wp - (u32)n - 2u) & (SP_RING - 1u)];
            float s = p0 + 0.5f * f * (p1 - pm + f * (2.0f * pm - 5.0f * p0 + 4.0f * p1 - p2 + f * (3.0f * (p0 - p1) + p2 - pm)));
            float h = ((s + v->hs_x1[e]) + v->al[e] * HS_K * (s - v->hs_x1[e])) * (1.0f / (1.0f + HS_K))
                      - ((1.0f - HS_K) / (1.0f + HS_K)) * v->hs_y1[e];
            v->hs_x1[e] = s; v->hs_y1[e] = h;
            out[e] = h;
            v->tau[e] += v->dtau[e]; v->al[e] += v->dal[e];
        }
        v->fbg += v->dfbg;
        v->wp = wp + 1u;
        *fl += v->w * out[0]; *fr += v->w * out[1];
        if (v->bin_t) { v->w += 1.0f / (SP_RATE * 0.02f); if (v->w > 1.0f) v->w = 1.0f; }
        else { v->w -= 1.0f / (SP_RATE * 0.02f); if (v->w < 0.0f) v->w = 0.0f; }
    }
    float dm = v->dm;
    v->dm += (v->dm_t - v->dm) * (1.0f / (SP_RATE * 0.04f));                 /* Doppler glide, 40 ms */
    if (fabsf(v->dm - v->dm_t) < 1e-5f) v->dm = v->dm_t;
    return dm;
}

/* the test source as the game would have panned it (its constant-power sine law on the azimuth, rear sign
   dropped as in VR), for the reference tap */
static void sp_test_pan(const sp_voice *v, float m, float *gl, float *gr) {
    float az = atan2f(v->dir_t[0], v->dir_t[2]);                     /* + = right */
    float p = az / 3.14159265f * 127.0f + 64.0f;                     /* the game's pan units, 64 = ahead */
    if (p > 127.0f) p = 254.0f - p;                                  /* rear half: mirrored, sign dropped */
    if (p < 0.0f) p = -p;
    *gl = m * cosf(p / 127.0f * 1.5707963f); *gr = m * sinf(p / 127.0f * 1.5707963f);
}
void ps2_spatial_tick(u32 k, float *fl, float *fr, int *rl, int *rr) {
    if (k % SP_BLOCK == 0u)
        for (u32 i = 0; i < SP_NV; i++) if (sp_v[i].on && (sp_v[i].w > 0.0f || sp_v[i].bin_t)) sp_block(&sp_v[i]);
    sp_voice *t = &sp_v[PS2_SPAT_TEST];
    if (t->on && sp_valid) {
        float x = sp_test_sample(t, t->dm);
        float m = sp_fr.v[PS2_SPAT_TEST].level, gl, gr;
        sp_test_pan(t, m, &gl, &gr);
        *rl += (int)lrintf(x * gl); *rr += (int)lrintf(x * gr);
        sp_render(t, x, m, gl, gr, fl, fr);
    }
}
/* ACZ_AUDIO3D_TEST_SOLO=1 (test): only the test source is heard (main and reference taps) */
int ps2_spatial_test_solo(void) {
    static int s = -1;
    if (s < 0) { const char *e = getenv("ACZ_AUDIO3D_TEST_SOLO"); s = e && *e == '1'; }
    return s;
}

float ps2_spatial_voice(u32 i, int e, int gl, int gr, float *fl, float *fr) {
    sp_voice *v = &sp_v[i];
    float fgl = (float)gl * (1.0f / (256.0f * 32768.0f)), fgr = (float)gr * (1.0f / (256.0f * 32768.0f));
    float m = sqrtf((fgl * fgl + fgr * fgr) * 0.5f);
    sp_samples3d++;
    return sp_render(v, (float)e, m, fgl, fgr, fl, fr);
}

/* ---- the synthetic test source (ACZ_AUDIO3D_TEST, zero_audio3d.c places it): noise bursts (ITD/ILD and spectra
   are measurable) or a harmonic tone (pitch is measurable: Doppler) ---- */
static float sp_test_sample(sp_voice *v, float dm) {
    static int sig = -1;
    if (sig < 0) { const char *e = getenv("ACZ_AUDIO3D_TEST_SIG"); sig = e && !strcmp(e, "tone") ? 1 : 0; }
    if (!v->rng) v->rng = 0x2545F491u;
    float out;
    if (sig) {
        v->ph += 220.0f / SP_RATE * dm;
        if (v->ph >= 1.0f) v->ph -= 1.0f;
        out = 0.0f;
        for (int h = 1; h <= 8; h++) out += sinf(6.2831853f * v->ph * (float)h) / (float)h;
        out *= 0.45f;
    } else {
        v->rng ^= v->rng << 13; v->rng ^= v->rng >> 17; v->rng ^= v->rng << 5;
        float wn = (float)(v->rng >> 8) * (2.0f / 16777216.0f) - 1.0f;
        v->pink += (wn - v->pink) * 0.25f;
        float env;                                           /* 250 ms on, 150 ms off, 5 ms ramps */
        v->burst += 1.0f / SP_RATE;
        if (v->burst >= 0.4f) v->burst -= 0.4f;
        env = v->burst < 0.25f ? 1.0f : 0.0f;
        if (v->burst < 0.005f) env = v->burst / 0.005f;
        else if (v->burst > 0.245f && v->burst < 0.25f) env = (0.25f - v->burst) / 0.005f;
        out = (0.5f * wn + 0.8f * v->pink) * env;
    }
    return out * 32767.0f;
}

/* ---- granular engine (prototype, ACZ_IMM_ENGINE_GRAIN, default 0) ----
   The player's engine in Zero is a few looping layers on positional voices (static RE + logs 2026-10-21): an
   idle/turbine layer whose level falls as the throttle rises (17B700 in mission 1), the main layer that grows with
   it (17F750), the afterburner roar (188E20, only with the afterburner) and a constant air layer (C94E0); the game
   sets their pitch from the throttle (and its weak Doppler lowers it with speed). Here each engine voice's own
   waveform (decoded once from sound RAM: ssa to its loop end, loop from its loop-start block) is re-synthesised
   from overlapping Hann-windowed grains read at the voice's current pitch: grain length 70 ms at idle down to 25 ms
   with full afterburner, overlap 2.5 to 5, random start points across the loop, a small random detune (20 cents at
   idle, 10 at full power), a pitch rise of up to 3 % and rougher grain levels with airspeed, and a brightness tilt
   (darker at idle, brighter with power and afterburner). An automatic gain keeps the grains at the original layer's
   RMS; the mix crossfades the original sample and the grains (0 = the game's loop, 1 = grains only) over 0.2 s.
   The game's envelope, volume, position (binaural) and pitch automation stay in charge. */

static u32 gr_rand(sp_grain *g) { g->rng ^= g->rng << 13; g->rng ^= g->rng >> 17; g->rng ^= g->rng << 5; return g->rng; }
static float gr_frand(sp_grain *g) { return (float)(gr_rand(g) >> 8) * (1.0f / 16777216.0f); }

static void gr_decode(sp_grain *g, u32 i, u32 ssa, u32 lsax) {
    g->ssa = ssa; g->ok = 0; g->len = 0; g->loop0 = -1; g->ng = 0; g->next = 0.0f;
    if (!ps2_spu2_ram || ssa >= 2u * 1024u * 1024u - 16u) return;
    if (!g->pcm) { g->cap = GR_MAXLEN; g->pcm = (float *)malloc(sizeof(float) * (size_t)g->cap); if (!g->pcm) return; }
    int s1 = 0, s2 = 0, looping = 0, blocks = 0;
    for (u32 a = ssa; a <= 2u * 1024u * 1024u - 16u && g->len + 28 <= g->cap; a += 16u, blocks++) {
        s16 out[28]; u32 h;
        ps2_spu2_decode_block(ps2_spu2_ram + a, out, &s1, &s2, &h);
        if ((h & 0x0400u) && g->loop0 < 0) g->loop0 = g->len;
        if (a == lsax && g->loop0 < 0) g->loop0 = g->len;
        for (int k = 0; k < 28; k++) g->pcm[g->len + k] = (float)out[k];
        g->len += 28;
        if (h & 0x0100u) { looping = (h & 0x0200u) != 0; break; }
    }
    if (g->loop0 < 0) g->loop0 = 0;
    g->ok = looping && g->len - g->loop0 >= 2048;
    if (!g->rng) g->rng = 0x9E3779B9u ^ (ssa * 2654435761u);
    ps2_log("spatial: engine voice %u waveform %06X: %d samples (%.3f s), loop from %d (%d samples)%s", i, ssa, g->len,
            g->len / SP_RATE, g->loop0, g->len - g->loop0, g->ok ? "" : " -- too short or not looping: left as it is");
}

/* one grain-synthesised sample of engine voice i at the voice's pitch (q12) */
static float gr_sample(sp_grain *g, u32 pitch) {
    float thr = sp_eng_thr, ab = sp_eng_ab, spd = sp_eng_spd;
    if (g->next <= 0.0f && g->ng < GR_GRAINS) {
        int dur = (int)(SP_RATE * (0.070f - 0.035f * thr - 0.010f * ab));
        float ov = 2.5f + 1.5f * thr + 1.0f * ab;
        float inc = (float)pitch / 4096.0f;
        float jit = 0.0116f - 0.0058f * thr;                         /* ~20 cents at idle, ~10 at full power */
        inc *= (1.0f + jit * (2.0f * gr_frand(g) - 1.0f)) * (1.0f + 0.03f * spd);
        int loop = g->len - g->loop0;
        double pos = g->loop0 + gr_frand(g) * (float)loop;
        int at = g->ng++;
        g->g[at].pos = pos; g->g[at].inc = inc; g->g[at].t = 0; g->g[at].dur = dur < 64 ? 64 : dur;
        g->g[at].amp = 1.0f - 0.3f * spd * gr_frand(g);
        g->next += (float)dur / ov * (0.85f + 0.3f * gr_frand(g));
        sp_grains_started++;
    }
    g->next -= 1.0f;
    float sum = 0.0f;
    for (int k = 0; k < g->ng; ) {
        float wpos = (float)g->g[k].t * ((float)GR_WIN / (float)g->g[k].dur);
        int wi = (int)wpos;
        float w = sp_hann[wi] + (sp_hann[wi + 1 > GR_WIN ? GR_WIN : wi + 1] - sp_hann[wi]) * (wpos - (float)wi);
        double p = g->g[k].pos;
        int n = (int)p; float fr = (float)(p - n);
        int n1 = n + 1 >= g->len ? g->loop0 : n + 1;
        float x = g->pcm[n] + (g->pcm[n1] - g->pcm[n]) * fr;
        sum += x * w * g->g[k].amp;
        p += g->g[k].inc;
        while (p >= (double)g->len) p -= (double)(g->len - g->loop0);
        g->g[k].pos = p;
        if (++g->g[k].t >= g->g[k].dur) g->g[k] = g->g[--g->ng]; else k++;
    }
    /* brightness tilt around a 900 Hz one-pole split */
    g->lp += (sum - g->lp) * (1.0f - expf(-6.2831853f * 900.0f / SP_RATE));
    float bright = 0.6f + 0.6f * thr + 0.3f * ab + 0.2f * spd;
    return g->lp + bright * (sum - g->lp);
}

int ps2_spatial_grain(u32 i, int s, u32 ssa, u32 lsax, u32 pitch) {
    if (i >= PS2_SPAT_VOICES) return s;
    sp_grain *g = &sp_g[i];
    float target = sp_eng[i] ? sp_grain_amt : 0.0f;
    if (target <= 0.0f && g->mix <= 0.0f) return s;
    if (g->ssa != ssa || !g->pcm) gr_decode(g, i, ssa, lsax);
    if (!g->ok) { g->mix = 0.0f; return s; }
    float out = gr_sample(g, pitch);
    const float k = 1.0f - expf(-1.0f / (SP_RATE * 0.2f));
    g->rms_o += ((float)s * (float)s - g->rms_o) * k;
    g->rms_g += (out * out - g->rms_g) * k;
    float gn = g->rms_g > 1.0f ? sqrtf(g->rms_o / g->rms_g) : 1.0f;
    gn = gn < 0.25f ? 0.25f : gn > 4.0f ? 4.0f : gn;
    g->gain += (gn - g->gain) * k * 4.0f;
    g->mix += (target - g->mix) * (1.0f / (SP_RATE * 0.2f));
    if (g->mix < 1e-4f && target == 0.0f) g->mix = 0.0f;
    sp_grain_samples++;
    float y = (1.0f - g->mix) * (float)s + g->mix * out * g->gain;
    return y > 32767.0f ? 32767 : y < -32768.0f ? -32768 : (int)lrintf(y);
}

/* self-test (ac5.exe's SPU2 self-test): a white-noise voice placed to the right must reach the right ear ~0.6 ms
   earlier and brighter, ahead it must be centred, cockpit muffling must take the highs and the level, the Doppler
   multiplier must glide to its target; the renderer is reset afterwards */
static void sp_st_render(const float *dir, int is3d, int own, float cockpit, float dopp, int n, float *L, float *R,
                         float *dm_last) {
    ps2_spat_frame fr;
    memset(&fr, 0, sizeof fr);
    fr.field = ps2_kernel_vblank_count();
    fr.cockpit = cockpit;
    fr.v[0].is3d = (u8)is3d; fr.v[0].own = (u8)own; fr.v[0].dopp = dopp;
    memcpy(fr.v[0].dir, dir, sizeof fr.v[0].dir);
    ps2_spatial_publish(&fr);
    u32 rng = 0x12345678u;
    for (int done = 0; done < n; ) {
        int m = n - done > 512 ? 512 : n - done;
        ps2_spatial_begin((u32)m);
        for (int k = 0; k < m; k++) {
            float fl = 0.0f, fr2 = 0.0f; int rl = 0, rr = 0;
            ps2_spatial_tick((u32)k, &fl, &fr2, &rl, &rr);
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            int e = (int)((rng >> 16) & 0x3FFF) - 0x2000;
            *dm_last = ps2_spatial_voice(0, e, 0x3FFF * 256, 0x3FFF * 256, &fl, &fr2);
            L[done + k] = fl; R[done + k] = fr2;
        }
        done += m;
    }
}
static int sp_st_lag(const float *L, const float *R, int n) {   /* lag of R behind L (samples) at the correlation peak */
    int best = 0; double bc = -1e30;
    for (int lag = -48; lag <= 48; lag++) {
        double c = 0.0;
        for (int i = 48; i < n - 48; i++) c += (double)L[i] * R[i + lag];
        if (c > bc) { bc = c; best = lag; }
    }
    return best;
}
static double sp_st_hf(const float *x, int n) {                  /* energy of the first difference: the highs */
    double e = 0.0;
    for (int i = 1; i < n; i++) e += (double)(x[i] - x[i - 1]) * (x[i] - x[i - 1]);
    return e;
}
int ps2_spatial_selftest(void) {
    enum { N = 9600 };
    static float L[N], R[N];
    float dm = 1.0f;
    int fail = 0;
    ps2_spat_frame saved = sp_fr; int saved_have = sp_have;
    sp_selftest_on = 1;
    memset(sp_v, 0, sizeof sp_v);
    const float right[3] = { 1, 0, 0 }, ahead[3] = { 0, 0, 1 };
    sp_st_render(right, 1, 1, 0.0f, 1.0f, N, L, R, &dm);
    int lag = sp_st_lag(L + N / 2, R + N / 2, N / 2);
    double ild = 10.0 * log10(sp_st_hf(R + N / 2, N / 2) / (sp_st_hf(L + N / 2, N / 2) + 1e-9));
    ps2_log("spatial selftest: source right: right ear %d samples earlier, highs %+.1f dB louder", -lag, ild);
    fail += !(lag <= -26 && lag >= -36);
    fail += !(ild > 10.0);
    sp_st_render(ahead, 1, 1, 0.0f, 1.0f, N, L, R, &dm);
    lag = sp_st_lag(L + N / 2, R + N / 2, N / 2);
    ild = 10.0 * log10(sp_st_hf(R + N / 2, N / 2) / (sp_st_hf(L + N / 2, N / 2) + 1e-9));
    fail += !(lag == 0 && fabs(ild) < 0.5);
    double open = sp_st_hf(L + N / 2, N / 2);
    sp_st_render(ahead, 1, 0, 1.0f, 1.0f, N, L, R, &dm);         /* external source, cockpit view: muffled */
    double muff = sp_st_hf(L + N / 2, N / 2);
    ps2_log("spatial selftest: source ahead centred (lag %d, highs %+.2f dB); cockpit muffling takes the highs by %.1f dB",
            lag, ild, 10.0 * log10(open / (muff + 1e-9)));
    fail += !(10.0 * log10(open / (muff + 1e-9)) > 6.0);
    sp_st_render(ahead, 1, 1, 0.0f, 1.2f, 4800, L, R, &dm);        /* Doppler target 1.2: 100 ms = 2.5 time constants */
    ps2_log("spatial selftest: Doppler multiplier after 100 ms toward 1.2: %.3f", dm);
    fail += !(dm > 1.15f && dm < 1.2f);
    sp_selftest_on = 0;
    memset(sp_v, 0, sizeof sp_v);
    sp_fr = saved; sp_have = saved_have;
    sp_chunks = sp_samples = sp_samples3d = 0; sp_maxon = 0;
    ps2_log("spatial selftest: %s", fail ? "FAILED" : "passed");
    return fail;
}

void ps2_spatial_report(void) {
    if (!sp_samples) return;
    ps2_log("spatial: %llu chunks, %.1f s mixed, %.2f 3D voice-seconds rendered (%.2f voices on average, at most %u); "
            "hrtf %d doppler %.2f muffle %.2f engine grain %.2f (%.1f voice-seconds, %llu grains)",
            (unsigned long long)sp_chunks, (double)sp_samples / SP_RATE, (double)sp_samples3d / SP_RATE,
            (double)sp_samples3d / (double)sp_samples, sp_maxon, ps2_spatial_hrtf_on(), ps2_spatial_doppler(),
            ps2_spatial_muffle(), ps2_spatial_engine_grain(), (double)sp_grain_samples / SP_RATE,
            (unsigned long long)sp_grains_started);
}
