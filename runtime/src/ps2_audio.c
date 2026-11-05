#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ps2_runtime.h"
#include "ps2_cpuprof.h"        /* CPU zones (PS2_CPU_PROF, diagnostic) */

void ps2_spu2_mix(s16 *out, u32 n);
void ps2_spu2_mix2(s16 *out, s16 *ref, u32 n);
void ps2_spu2_init(void);
void ps2_rx_audio(const s16 *buf, int frames);   /* (ps2_recexact.c) */

#define AUDIO_RATE      48000
#define AUDIO_CHANNELS  2
#define AUDIO_CHUNK     512

static SDL_AudioStream *audio_stream;
static u64 audio_bytes;
static int audio_started;
/* WAV taps of the final mix: PS2_AUDIO_WAV=path; PS2_AUDIO_WAV_REF=path (test) also writes the A/B reference: the
   same audio with the spatial stages (binaural, cockpit muffling) off, sample-aligned with the main tap */
typedef struct { FILE *fp; u32 bytes; } wav_tap;
static wav_tap wav_main, wav_ref, wav_imm;   /* wav_imm: ACZ_DEBUG_IMM_WAV=path, the immersion layer alone (test) */

static void wav_open1(wav_tap *w, const char *var) {
    const char *path = getenv(var);
    u8 hdr[44];
    if (!path) return;
    w->fp = fopen(path, "wb");
    if (!w->fp) { ps2_log("audio: cannot open %s", path); return; }
    memset(hdr, 0, sizeof hdr);
    fwrite(hdr, 1, sizeof hdr, w->fp);
    ps2_log("audio: tapping the %s to %s", w == &wav_ref ? "A/B reference mix (spatial stages off)"
            : w == &wav_imm ? "immersion layer" : "mix", path);
}
static void wav_open(void) {
    if (getenv("PS2_AUDIO_WAV")) wav_open1(&wav_main, "PS2_AUDIO_WAV");
    if (getenv("PS2_AUDIO_WAV_REF")) wav_open1(&wav_ref, "PS2_AUDIO_WAV_REF");
    if (getenv("ACZ_DEBUG_IMM_WAV")) wav_open1(&wav_imm, "ACZ_DEBUG_IMM_WAV");
}

static void wav_put1(wav_tap *w, const void *buf, int len) {
    if (!w->fp) return;
    size_t wr = fwrite(buf, 1, (size_t)len, w->fp);
    if (wr != (size_t)len) {
        static int said;
        if (said++ < 4) ps2_log("audio: short write to the %s tap (%zu of %d bytes at byte %u)",
                                w == &wav_ref ? "reference" : "main", wr, len, w->bytes);
    }
    w->bytes += (u32)len;
    {   /* PS2_AUDIO_WAV_LOG (test): byte counts of both taps every ~10 s of audio, to tell a lost write from a
           logic difference */
        static int lg = -1; static u32 next;
        if (lg < 0) { const char *e = getenv("PS2_AUDIO_WAV_LOG"); lg = e && *e == '1'; }
        if (lg && w == &wav_main && w->bytes >= next) {
            next = w->bytes + 48000u * 4u * 10u;
            ps2_log("audio: taps main %u bytes, reference %u bytes", wav_main.bytes, wav_ref.bytes);
        }
    }
}
static void wav_put(const void *buf, int len) { wav_put1(&wav_main, buf, len); }

static void wav_close1(wav_tap *w) {
    u8 h[44];
    u32 v;
    if (!w->fp) return;
    memcpy(h, "RIFF", 4);   v = 36u + w->bytes;      memcpy(h + 4, &v, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    v = 16;                 memcpy(h + 16, &v, 4);
    { u16 t = 1;            memcpy(h + 20, &t, 2);
      t = AUDIO_CHANNELS;   memcpy(h + 22, &t, 2); }
    v = AUDIO_RATE;         memcpy(h + 24, &v, 4);
    v = AUDIO_RATE * AUDIO_CHANNELS * 2u; memcpy(h + 28, &v, 4);
    { u16 t = AUDIO_CHANNELS * 2u; memcpy(h + 32, &t, 2);
      t = 16;               memcpy(h + 34, &t, 2); }
    memcpy(h + 36, "data", 4);            memcpy(h + 40, &w->bytes, 4);
    fseek(w->fp, 0, SEEK_SET);
    fwrite(h, 1, sizeof h, w->fp);
    fclose(w->fp);
    w->fp = NULL;
}
static void wav_close(void) { wav_close1(&wav_main); wav_close1(&wav_ref); wav_close1(&wav_imm); }

/* Immersion sounds (prototype): procedural airflow, rain/snow on the canopy and debris ticks, mixed after the SPU2
   output on the SDL audio thread. Inputs (written once per game frame by zero_vr.c, read here per chunk):
   ps2_imm_audio [0] inside-cloud density 0..1, [1] rain 0..1, [2] snow 0..1, [3] airspeed (kt), [4] field of the last
   HUD-manager run (older than 8 fields: paused, menu, cutscene -- everything fades out), [5] debris counter (grows
   by each event's strength 0..1).
   Level: F4 > Immersion (ACZ_IMM_SOUND overrides; 0 off). Levels are relative to full scale; the game mix peaks
   near it. */
volatile float ps2_imm_audio[8];
typedef struct { float amp, decay, hp, pan, env; u32 noise; } imm_drop;
typedef struct { float y1, y2, c, r2, amp, pan; } imm_tick;
static struct {
    int init; float gain;
    u32 rng;
    float lp[2], lp2[2];                 /* airflow noise filters per channel */
    float g_air, g_rain, g_snow, run;    /* smoothed gains */
    imm_drop drop[24]; int nd;
    imm_tick tick[16];
    float tick_due[16]; int nt_due;      /* scheduled debris ticks (seconds from now) */
    float thump_t, thump_amp;
    float debris_seen;
    /* cloud: a whoosh on entry and exit (band-passed noise, the band sweeping up), a soft pressure thump on entry,
       fine droplets ticking on the canopy inside (with the humidity) */
    float cl_in_seen, cl_out_seen, w_t, w_amp, w_f0, w_f1, w_y1, w_y2, w_lp, g_drip;
    u32 dbg_n, dbg_drops, dbg_win, dbg_wout, dbg_deb; int dbg;   /* ACZ_DEBUG_IMM_SOUND=N: a summary every N s */
    int drip;                            /* ACZ_IMM_DRIP: 1 fine cloud droplets in cloud only, 2 as before, 0 none */
    /* snow on the canopy (ACZ_IMM_SNOW_SOUND, bug #260): level, the smoothed gain, the hiss's filters (common and per
       side), the snowfall's slow density wander, grains and clumps (bursts through their own filters) */
    float sn_k, sn_g, sn_mod, sn_modv;
    float sn_hp[3], sn_l1[3], sn_l2[3];
    struct { float env, dec, amp, pan, hp, lp, lpa; u32 nz; } sn_b[12];
    u32 dbg_sgrain, dbg_sclump; float dbg_sn_max;
} imm;
/* Snow on the canopy (ACZ_IMM_SNOW_SOUND, fx_debris 2026-10-31, bug #260: M1 10:49:03 "there should be a sound from the
   snow"). Before, snow only thinned the rain patter: ~34 ticks a second at M1's snowfall (0.45) and 320 kt, 0.007-0.037
   peak through a 2 kHz one-pole high-pass, 2-8 ms -- about -45 dBFS RMS under a game mix of -16 dBFS: not heard. Snow at
   speed meets the windscreen as a stream (moderate snowfall holds ~10^3-10^4 flakes per m^3: ~10^5 a second over the
   front glass at 150 m/s), soft crystals that crush rather than knock: a dry, sandy hiss with the bigger flakes and
   clumps as soft "tuk"s. Made here: a hiss of band-limited noise (1.4-6.5 kHz, where the Quest's built-in speakers
   carry it; no bass), mostly from ahead (a common part in both ears plus a little of each side's own); a granular layer
   of short bursts (0.5-1.4 ms, 400 x the gain a second: ~130 at M1, power-law sizes) that gives it the sandy texture; clumps (3-7 ms,
   1.2-3.2 kHz, 6-24 a second); all with the snowfall (sky +0xB8C as the patter: 0.2 + 0.8 sqrt(amount)), the airspeed
   (0.25 + 0.75 (kt / 450)^1.3) and a slow wander of its density (0.3-1.5 Hz, +-35 %); through the immersion bus at half
   the beds' ducking in dB (a sound on the glass, as the rain patter, not a bed of air; the full bed ducking took it 6-9 dB
   under M1's game). Sim M1 (snowfall 0.45, 324 kt; the game's own mix -25 dBFS in this band, -18.7 in all): first
   version -43.7 dBFS in 1.4-6.5 kHz (20 dB under the game there: not heard); now ~-33.
   Level 0..2 (1; 0 = as before: only the thinned patter). */
static u32 imm_rand(void) { imm.rng ^= imm.rng << 13; imm.rng ^= imm.rng >> 17; imm.rng ^= imm.rng << 5; return imm.rng; }
static float imm_frand(void) { return (float)(imm_rand() >> 8) * (1.0f / 16777216.0f); }
u64 ps2_kernel_vblank_count(void);
/* Debris passes (wish #47; ACZ_IMM_DEBRIS, sound part): zero_vr.c imm_debris sends events (ps2_imm_deb_ev, a ring of
   32: zips of chunks and of the wreck, canopy hits; layout there) and the bed (ps2_imm_deb_bed: how deep the aircraft
   is in a cloud of bits). Positioned with the spatial mixer's head model (ps2_spatial.c: Woodworth interaural delay +
   the Brown-Duda head-shadow filter per ear; a constant-power pan when ACZ_AUDIO_HRTF is off), Doppler at the same
   strength (ACZ_AUDIO_DOPPLER: a quarter of the real effect), and the sounds outside the canopy muffled in the cockpit
   view like the game's (ACZ_AUDIO_MUFFLE):
   - zip, a chunk passing: noise through a band-pass at ~900 Hz x Doppler with a tumbling flutter (18-40 Hz), level
     ~ 1 / distance (6 m reference), moving along its straight path relative to the head (0.3-1 s);
   - the wreck passing: lower (380 Hz), longer, with a low roar (15 m reference);
   - canopy hit: tick = two short modes at 2.2-4.5 kHz (10-25 ms, peak 0.07 x strength), clank = three modes at
     0.7-1.4 kHz (40-80 ms, peaks 0.09 / 0.05 / 0.03) and a dull knock, from the impact point on the glass, after the
     time to impact (not muffled: the glass rings);
   - bed: inside or near a cloud of bits a hiss and a sparse rain of tiny clicks from its direction (all around inside).
   Levels stay below the cloud whoosh: a chunk passing at ~8 m ~-31 dBFS rms at its closest, at 20 m ~-40 (several at
   once share the level as 1 / sqrt(n)); the wreck at 20 m ~-32; ticks peak 0.03-0.08 of full scale, clanks up to
   ~0.17 (-15 dBFS); the old strike's burst was -27 dBFS rms, peak -16.
   Physical version (ACZ_IMM_DEBRIS_SOUND=1, default since 2026-10-27, bug #139; 0 = the sounds above):
   - a canopy hit is an impact on a stretched-acrylic shell: the bit's size (a tick: a grain of 2-15 mm, a clank: a
     chunk of 3-12 cm, log-uniform), its speed against the glass (the event's relative velocity) and the angle it meets
     the glass at (the canopy as an ellipsoid about the eye, 0.5 x 0.55 x 1.6 m: the windscreen's foot is met nearly
     square, the canopy's top and sides at a graze) set the force pulse: its length (Hertz-like, ~12 us for grit, 0.1-0.4
     ms for chunks: grit rings up to 10 kHz, a chunk thuds below ~2 kHz) and its strength (the normal impulse m v_n,
     compressed ^0.2: 0.015-0.16 of full scale); the glass answers with plate modes (6 for a tick between 1.8 and 10 kHz,
     8 for a clank between 140 Hz and 5 kHz) weighted by the pulse's spectrum and the plate's radiation (weaker below
     its ~2 kHz coincidence), each decaying with acrylic's loss factor (eta 0.035: tau = 1 / (pi f eta), 9 ms at 1 kHz,
     30 ms at 300 Hz), the contact's own snap, and for chunks a thud through the frame (130-190 Hz, 35 ms);
   - a glancing hit (the normal share under 0.45) skitters: 2-4 bounces 1.5-5 ms apart, each weaker, and a short
     scrape of the bit sliding (8-30 ms band noise at ~3.5 kHz, longer and louder the more it grazes);
   - a chunk zipping past tumbles: irregular whoosh pulses (each turn +-30 % in time, random strength) instead of a
     sine flutter, with a second, broader band;
   - the rattle in a cloud of bits: grains of a power-law size spread (many faint ticks, a few louder), each two
     modes with acrylic's decay, mostly from ahead, some skittering.
   A grit tick (event kind 5, bug #189, ACZ_IMM_SMOKE_PASS: the fine grit in a kill's smoke ball) is a canopy hit of a
   grain of 0.6-3 mm (log-uniform) at 0.6 of the event's strength: a light, high tick (about -8 dB under a tick's
   typical 5 mm grain); the first version (ACZ_IMM_DEBRIS_SOUND=0) plays it as a tick at half strength. */
#define DB_RING 64u
#define DB_HS_K (2.0f * (float)AUDIO_RATE / (2.0f * 343.0f / 0.0875f))      /* head-shadow filter constant (12.24) */
#define DB_ITD  (0.0875f / 343.0f * (float)AUDIO_RATE)                       /* head radius / c in samples (12.24) */
typedef struct {
    float ring[DB_RING]; u32 wp;
    float tau[2], al[2], g[2], dtau[2], dal[2], dg[2];     /* per ear: delay (samples), head-shadow gain, pan gain */
    float hx[2], hy[2];
    int fresh;
} db_bin;
static float db_alpha(float c) {                 /* as ps2_spatial.c sp_alpha: c = cos(angle from the ear's axis) */
    float th = acosf(c > 1.0f ? 1.0f : c < -1.0f ? -1.0f : c);
    float a = (1.05f + 0.95f * cosf(th * (180.0f / 150.0f))) * (1.0f / 0.7564f);
    return a < 0.08f ? 0.08f : a > 2.0f ? 2.0f : a;
}
static float db_itd(float c) {                   /* as sp_itd: Woodworth, samples */
    float th = acosf(c > 1.0f ? 1.0f : c < -1.0f ? -1.0f : c);
    return 2.0f + DB_ITD * (th < 1.5707963f ? 1.0f - c : 1.0f + th - 1.5707963f);
}
/* once per 32 samples: the targets for a direction (head axes, unit), ramped over the block */
static void db_bin_block(db_bin *b, const float *dir, int hrtf) {
    float x = dir[0], tau[2], al[2], g[2];
    if (hrtf) {
        tau[0] = db_itd(-x); tau[1] = db_itd(x); al[0] = db_alpha(-x); al[1] = db_alpha(x); g[0] = g[1] = 1.0f;
    } else {                                     /* the game's constant-power pan on the azimuth, centre = 1 */
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
static void db_bin_run(db_bin *b, float s, float *l, float *r) {
    b->ring[b->wp & (DB_RING - 1u)] = s;
    float out[2];
    for (int e = 0; e < 2; e++) {
        float d = b->tau[e];
        int n = (int)d; float fr = d - (float)n;
        float p0 = b->ring[(b->wp - (u32)n) & (DB_RING - 1u)], p1 = b->ring[(b->wp - (u32)n - 1u) & (DB_RING - 1u)];
        float y = p0 + fr * (p1 - p0);
        float h = ((y + b->hx[e]) + b->al[e] * DB_HS_K * (y - b->hx[e])) * (1.0f / (1.0f + DB_HS_K))
                  - ((1.0f - DB_HS_K) / (1.0f + DB_HS_K)) * b->hy[e];
        b->hx[e] = y; b->hy[e] = h;
        out[e] = h * b->g[e];
        b->tau[e] += b->dtau[e]; b->al[e] += b->dal[e]; b->g[e] += b->dg[e];
    }
    b->wp++;
    *l += out[0]; *r += out[1];
}
typedef struct {
    int on, big; float p[3], v[3], t, st, g, gs, f0, flf, flp, y1, y2, roar, mlp[2];   /* on -1: fading out */
    u32 rng; db_bin b;
    float a1, a2, ain, dop;                      /* band-pass for this block */
    float tstr, b1, b2, ba1, ba2, bain;          /* (physical) this turn's whoosh strength; the broad band at 2.5 x */
} db_zip;
/* A blast's pressure wave on the canopy (bug #232, ACZ_IMM_BLAST: event kind 6 from zero_vr.c imm_blast, timed by the
   wave's arrival, d / 340 m/s after the flash). A pressure pulse of tens of ms meets the whole canopy at once: the shell
   answers in its lowest plate modes, the frame thuds, the canopy rattles a few times against its seals, and the air
   itself gives a low whump. Here: a thump (a damped resonance at 125-160 Hz, 50 ms, with its octave for small speakers:
   the Quest's own lose the bass below ~150 Hz), four of the shell's low modes (180-700 Hz, acrylic's loss factor:
   50 -> 13 ms), 2-7 knocks of the rattle (two modes at 0.9-2.2 kHz, 5 ms, 8-30 ms apart, each weaker) above strength
   0.2, and low-passed noise (~300 Hz, 120 ms); peaks ~0.2 at full strength (a close kill), ~0.07 at 0.2; from the
   blast's side, not muffled (the glass rings). */
typedef struct {
    int on; float delay, st, dir[3], t;
    float c[6], r2[6], y1[6], y2[6];
    float nlp, nlp2, namp;
    int nk; float kt, ka, kc[2], kr2[2], ky1[2], ky2[2];
    db_bin b;
} db_blast;
typedef struct {
    int on, clank, grit; float delay, st, dir[3], t, y1[8], y2[8], c[8], r2[8], th_amp, mlp; db_bin b;
    /* physical version: the impact speed (m/s) and the share of it square to the glass; the contact pulse; the
       skitter (bounces left, time to the next, their kick); the scrape; the frame's thud */
    float spd, cn, a, kick[8], pt, pdur, pamp, pprev, bt, bamp, sc_t, sc_dur, sc_amp, sc_y1, sc_y2, sc_a1, sc_a2, sc_in;
    int nmodes, bounces;
    float th_c, th_r2, th_y1, th_y2;
} db_hit;
static struct {
    int init, mask; u32 rd, rng;
    db_zip z[6]; db_hit h[6]; db_blast bl[3];
    float bed_g, bed_y1, bed_y2, bed_lp[2], bed_dc[2], bed_mlp[2][2]; db_bin bed_b;
    struct { float y1, y2, c, r2, pan, y1b, y2b, cb, r2b, next, nk, ka, kb; } bt[8];
    u32 n_zip, n_hit, n_bedtick, n_blast; float bed_max, zn;
} db;
static float db_frand(void) { db.rng ^= db.rng << 13; db.rng ^= db.rng >> 17; db.rng ^= db.rng << 5; return (float)(db.rng >> 8) * (1.0f / 16777216.0f); }
/* ACZ_IMM_DEBRIS_SOUND: 1 = the physical canopy hits, tumbling zips and rattle (default), 0 = the first version */
static int db_phys(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("ACZ_IMM_DEBRIS_SOUND"); v = !(e && *e == '0'); }
    return v;
}
/* takes the new events (always, so none pile up while the sound is off or paused) */
static void db_take(int play) {
    extern volatile float ps2_imm_deb_ev[32][12];
    extern volatile u32 ps2_imm_deb_seq;
    u32 n = __atomic_load_n(&ps2_imm_deb_seq, __ATOMIC_ACQUIRE);
    if (n - db.rd > 32u) db.rd = n - 32u;
    for (; db.rd != n; db.rd++) {
        volatile float *e = ps2_imm_deb_ev[db.rd % 32u];
        int kind = (int)e[0];
        if (!play || !(db.mask & 1)) continue;
        if (kind == 1 || kind == 2) {
            int k = 0;
            for (int i = 0; i < 6; i++) { if (!db.z[i].on) { k = i; break; } if (db.z[i].st < db.z[k].st) k = i; }
            db_zip *z = &db.z[k];
            memset(z, 0, sizeof *z);
            z->on = 1; z->big = kind == 2; z->st = e[8];
            for (int j = 0; j < 3; j++) { z->p[j] = e[2 + j]; z->v[j] = e[5 + j]; }
            z->rng = 0x9E3779B9u ^ (db.rd * 2654435761u); if (!z->rng) z->rng = 1u;
            z->f0 = (z->big ? 380.0f : 900.0f) * (0.85f + 0.3f * db_frand());
            z->flf = z->big ? 9.0f + 6.0f * db_frand() : 18.0f + 22.0f * db_frand();
            z->tstr = 0.8f;
            z->b.fresh = 1;
            db.n_zip++;
        } else if (kind == 3 || kind == 4 || kind == 5) {
            int k = 0;
            for (int i = 0; i < 6; i++) { if (!db.h[i].on) { k = i; break; } if (db.h[i].t > db.h[k].t) k = i; }
            db_hit *h = &db.h[k];
            memset(h, 0, sizeof *h);
            h->on = 1; h->clank = kind == 4; h->delay = e[1] > 0.0f && e[1] < 1.0f ? e[1] : 0.0f; h->st = e[8];
            if (kind == 5) { h->grit = 1; h->st = e[8] * (db_phys() ? 0.6f : 0.5f); }
            float q[3] = { e[2], e[3], e[4] }, m = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
            if (m > 1e-4f) for (int j = 0; j < 3; j++) h->dir[j] = q[j] / m; else h->dir[2] = 1.0f;
            {   /* (physical version) the impact speed; the share of it square to the glass: the canopy as an ellipsoid
                   about the eye (x 0.5, y 0.55, z 1.6 m), the bits coming along its long axis; from behind or off
                   the glass in view (no direction): a graze */
                float w2 = e[5] * e[5] + e[6] * e[6] + e[7] * e[7], mc[3] = { e[9], e[10], e[11] };
                h->spd = w2 > 1.0f && w2 < 1e6f ? sqrtf(w2) : 150.0f;
                float mm = mc[0] * mc[0] + mc[1] * mc[1] + mc[2] * mc[2];
                if (mm > 0.25f) {
                    float nx = mc[0] / 0.25f, ny = mc[1] / 0.3025f, nz = mc[2] / 2.56f, nn = sqrtf(nx * nx + ny * ny + nz * nz);
                    h->cn = nn > 1e-6f ? fabsf(nz) / nn : 0.5f;
                } else h->cn = 0.3f;
            }
            h->t = -1.0f;                                /* rings once the delay is over */
            h->b.fresh = 1;
            db.n_hit++;
        } else if (kind == 6) {                          /* a blast's pressure wave (bug #232) */
            int k = 0;
            for (int i = 0; i < 3; i++) { if (!db.bl[i].on) { k = i; break; } if (db.bl[i].st < db.bl[k].st) k = i; }
            db_blast *b = &db.bl[k];
            memset(b, 0, sizeof *b);
            b->on = 1; b->delay = e[1] > 0.0f && e[1] < 3.0f ? e[1] : 0.0f;
            b->st = e[8] > 0.0f && e[8] <= 1.0f ? e[8] : 0.3f;
            float q[3] = { e[2], e[3], e[4] }, m = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
            if (m > 1e-3f) for (int j = 0; j < 3; j++) b->dir[j] = q[j] / m; else b->dir[2] = 1.0f;
            b->t = -1.0f;
            b->b.fresh = 1;
            db.n_blast++;
        }
    }
}
static void db_blast_start(db_blast *b) {
    const float dt = 1.0f / (float)AUDIO_RATE;
    float sa = powf(b->st, 0.7f);
    float f0 = 125.0f + 35.0f * db_frand(), fk[6], tau[6], amp[6];
    fk[0] = f0; tau[0] = 0.05f; amp[0] = 0.07f * sa;
    fk[1] = f0 * (2.0f + 0.15f * db_frand()); tau[1] = 0.035f; amp[1] = 0.08f * sa;
    for (int k = 2; k < 6; k++) {                 /* the shell's low modes */
        fk[k] = 180.0f * powf(700.0f / 180.0f, ((float)(k - 2) + db_frand()) / 4.0f);
        tau[k] = 1.0f / (3.14159265f * fk[k] * 0.035f);      /* (acrylic's loss factor, DB_ETA) */
        amp[k] = 0.065f * sa * (0.6f + 0.4f * db_frand()) / (1.0f + fk[k] / 500.0f);
    }
    for (int k = 0; k < 6; k++) {
        float rr = expf(-dt / tau[k]), wq = 6.2831853f * fk[k] * dt;
        b->c[k] = 2.0f * rr * cosf(wq); b->r2[k] = rr * rr;
        b->y1[k] = (k < 2 || db_frand() < 0.5f ? 1.0f : -1.0f) * amp[k] * sinf(wq); b->y2[k] = 0.0f;
    }
    b->namp = 0.05f * b->st;
    b->nk = b->st > 0.2f ? 2 + (int)(5.0f * b->st) : 0;
    b->kt = 0.012f + 0.02f * db_frand(); b->ka = 0.05f * sa;
    b->t = 0.0f;
}
static float db_blast_run(db_blast *b) {
    const float dt = 1.0f / (float)AUDIO_RATE;
    float s = 0.0f;
    for (int k = 0; k < 6; k++) {
        float y = b->c[k] * b->y1[k] - b->r2[k] * b->y2[k];
        b->y2[k] = b->y1[k]; b->y1[k] = y; s += y;
    }
    {   /* the air's whump: noise through two one-pole low-passes (~300 Hz), 3 ms up, 120 ms down */
        float env = b->t < 0.003f ? b->t / 0.003f : expf(-(b->t - 0.003f) / 0.12f);
        b->nlp += 0.038f * ((db_frand() * 2.0f - 1.0f) - b->nlp);
        b->nlp2 += 0.038f * (b->nlp - b->nlp2);
        s += b->nlp2 * b->namp * env * 9.0f;
    }
    if (b->nk > 0) {                              /* the rattle: knocks against the seals, each weaker */
        b->kt -= dt;
        if (b->kt <= 0.0f) {
            for (int m = 0; m < 2; m++) {
                float f = (900.0f + 1300.0f * db_frand()) * (m ? 1.6f : 1.0f), rr = expf(-dt / 0.005f), wq = 6.2831853f * f * dt;
                b->kc[m] = 2.0f * rr * cosf(wq); b->kr2[m] = rr * rr;
                b->ky1[m] += (m ? 0.6f : 1.0f) * b->ka * sinf(wq) * (db_frand() < 0.5f ? -1.0f : 1.0f);
            }
            b->ka *= 0.72f; b->nk--;
            b->kt = 0.008f + 0.022f * db_frand();
        }
    }
    for (int m = 0; m < 2; m++) {
        float y = b->kc[m] * b->ky1[m] - b->kr2[m] * b->ky2[m];
        b->ky2[m] = b->ky1[m]; b->ky1[m] = y; s += y;
    }
    b->t += dt;
    return s;
}
static void db_hit_start(db_hit *h) {
    const float dt = 1.0f / (float)AUDIO_RATE;
    float f1, fk[3], dec[3], amp[3];
    if (h->clank) {
        f1 = 700.0f + 700.0f * db_frand();
        fk[0] = f1; fk[1] = f1 * 2.27f; fk[2] = f1 * 3.93f;
        dec[0] = 0.04f + 0.04f * db_frand(); dec[1] = dec[0] * 0.6f; dec[2] = dec[0] * 0.4f;
        amp[0] = 0.09f; amp[1] = 0.05f; amp[2] = 0.03f;
        h->th_amp = 0.05f * h->st;
    } else {
        f1 = 2200.0f + 2300.0f * db_frand();
        fk[0] = f1; fk[1] = f1 * 1.73f; fk[2] = 0.0f;
        dec[0] = 0.010f + 0.015f * db_frand(); dec[1] = dec[0] * 0.7f; dec[2] = 0.0f;
        amp[0] = 0.07f; amp[1] = 0.035f; amp[2] = 0.0f;
        h->th_amp = 0.015f * h->st;
    }
    for (int k = 0; k < 3; k++) {
        if (fk[k] <= 0.0f) { h->c[k] = h->r2[k] = h->y1[k] = h->y2[k] = 0.0f; continue; }
        float rr = expf(-dt / dec[k]);
        h->c[k] = 2.0f * rr * cosf(6.2831853f * fk[k] * dt); h->r2[k] = rr * rr;
        /* the impulse: a resonator started at y1 = a rings at a / sin(w); so amp is the mode's peak */
        h->y1[k] = amp[k] * h->st * (0.8f + 0.4f * db_frand()) * sinf(6.2831853f * fk[k] * dt); h->y2[k] = 0.0f;
    }
    h->t = 0.0f;
}
/* the physical canopy hit (see the comment at the top) */
#define DB_ETA 0.035f                            /* stretched acrylic's loss factor */
static void db_hit_start_phys(db_hit *h) {
    const float dt = 1.0f / (float)AUDIO_RATE;
    float size = h->clank ? 0.03f * powf(4.0f, db_frand()) : h->grit ? 0.0006f * powf(5.0f, db_frand())
                 : 0.002f * powf(7.5f, db_frand());     /* m */
    float cn = h->cn < 0.05f ? 0.05f : h->cn > 1.0f ? 1.0f : h->cn;
    float vn = h->spd * cn; if (vn < 8.0f) vn = 8.0f;
    float tc = 1.2e-5f * powf(size / 0.002f, 0.9f) * powf(150.0f / vn, 0.2f);   /* contact time (s) */
    float fcor = 0.5f / tc;                                                       /* the pulse's spectral corner */
    float imp = size * size * size * 2700.0f * vn;                                /* the normal impulse (N s), aluminium bits */
    float a = 0.05f * powf(imp / 2.6e-2f, 0.2f);
    a = (a < 0.015f ? 0.015f : a > 0.16f ? 0.16f : a) * (0.5f + 0.5f * h->st);
    h->a = a;
    int nm = h->clank ? 8 : 6;
    float lo = h->clank ? 140.0f : 1800.0f, hi = h->clank ? 5000.0f : 10000.0f;
    float w[8], ws = 0.0f, fk[8];
    for (int k = 0; k < nm; k++) {
        fk[k] = lo * powf(hi / lo, db_frand());
        float S = 1.0f / (1.0f + (fk[k] / fcor) * (fk[k] / fcor));               /* the pulse's spectrum */
        float R = fk[k] < 2000.0f ? sqrtf(fk[k] / 2000.0f) : 1.0f;                /* radiation below coincidence */
        w[k] = S * R * (0.4f + 0.6f * db_frand());
        ws += w[k];
    }
    for (int k = 0; k < 8; k++) {
        if (k >= nm || ws <= 0.0f) { h->c[k] = h->r2[k] = h->y1[k] = h->y2[k] = h->kick[k] = 0.0f; continue; }
        float tau = 1.0f / (3.14159265f * fk[k] * DB_ETA);
        float rr = expf(-dt / tau), wq = 6.2831853f * fk[k] * dt;
        h->c[k] = 2.0f * rr * cosf(wq); h->r2[k] = rr * rr;
        float sg = db_frand() < 0.5f ? -1.0f : 1.0f;                              /* the mode's shape at the point */
        h->kick[k] = sg * a * (w[k] / ws) * (h->clank ? 1.4f : 2.2f) * sinf(wq);
        h->y1[k] = h->kick[k]; h->y2[k] = 0.0f;
    }
    h->nmodes = nm;
    /* the contact's snap: a raised-cosine force pulse, differentiated */
    h->pdur = tc > dt ? tc : dt; h->pt = 0.0f; h->pamp = a * 0.5f * (h->clank ? 0.6f : 1.0f); h->pprev = 0.0f;
    /* a chunk's thud through the frame */
    h->th_c = h->th_r2 = h->th_y1 = h->th_y2 = 0.0f;
    if (h->clank) {
        float f = 130.0f + 60.0f * db_frand(), rr = expf(-dt / 0.035f), wq = 6.2831853f * f * dt;   /* (above the Quest speakers' roll-off) */
        h->th_c = 2.0f * rr * cosf(wq); h->th_r2 = rr * rr; h->th_y1 = 0.3f * a * sinf(wq);
    }
    /* a graze: the skitter and the scrape */
    h->bounces = 0; h->bt = 0.0f; h->sc_amp = 0.0f;
    if (cn < 0.45f) {
        float g = (0.45f - cn) / 0.40f; g = g > 1.0f ? 1.0f : g;
        h->bounces = 2 + (int)(2.99f * db_frand());
        h->bt = 0.0015f + 0.0035f * db_frand(); h->bamp = 0.5f;
        float vt = h->spd * sqrtf(1.0f - cn * cn);
        h->sc_dur = (0.008f + 0.022f * g) * (vt > 60.0f ? 1.0f : vt / 60.0f + 0.2f);
        h->sc_amp = a * (0.25f + 0.35f * g); h->sc_t = 0.0f;
        float fc = 3500.0f * (0.8f + 0.4f * db_frand()), rr = 0.93f, wq = 6.2831853f * fc * dt;
        h->sc_a1 = 2.0f * rr * cosf(wq); h->sc_a2 = -rr * rr; h->sc_in = (1.0f - rr) * 2.0f * sinf(wq); h->sc_y1 = h->sc_y2 = 0.0f;
    }
    h->t = 0.0f;
}
static float db_hit_run_phys(db_hit *h, float *live) {
    const float dt = 1.0f / (float)AUDIO_RATE;
    float s = 0.0f, lv = 0.0f;
    for (int m = 0; m < h->nmodes; m++) {
        float y = h->c[m] * h->y1[m] - h->r2[m] * h->y2[m];
        h->y2[m] = h->y1[m]; h->y1[m] = y;
        s += y; lv += fabsf(y) + fabsf(h->y2[m]);
    }
    if (h->pt < h->pdur + dt) {                  /* the snap */
        float u = h->pt / h->pdur, f = u < 1.0f ? 0.5f - 0.5f * cosf(6.2831853f * u) : 0.0f;
        s += (f - h->pprev) * h->pamp * (h->pdur > 2.0f * dt ? 2.0f : 1.0f);
        h->pprev = f; h->pt += dt; lv += 1.0f;
    }
    if (h->th_c != 0.0f) {                       /* the frame's thud */
        float y = h->th_c * h->th_y1 - h->th_r2 * h->th_y2;
        h->th_y2 = h->th_y1; h->th_y1 = y; s += y; lv += fabsf(y);
    }
    if (h->bounces > 0) {                        /* the skitter: the modes kicked again, weaker */
        h->bt -= dt;
        if (h->bt <= 0.0f) {
            for (int m = 0; m < h->nmodes; m++) h->y1[m] += h->kick[m] * h->bamp * (0.6f + 0.8f * db_frand());
            h->pt = 0.0f; h->pamp *= 0.5f; h->bamp *= 0.55f;
            h->bounces--; h->bt = 0.0015f + 0.0035f * db_frand();
        }
        lv += 1.0f;
    }
    if (h->sc_amp > 0.0f && h->sc_t < h->sc_dur) {   /* the scrape */
        float u = h->sc_t / h->sc_dur, e = u < 0.1f ? u / 0.1f : (1.0f - u) / 0.9f;
        float wn = db_frand() * 2.0f - 1.0f;
        float y = wn * h->sc_in + h->sc_a1 * h->sc_y1 + h->sc_a2 * h->sc_y2;
        h->sc_y2 = h->sc_y1; h->sc_y1 = y;
        s += y * e * h->sc_amp * 1.5f;
        h->sc_t += dt; lv += 1.0f;
    }
    *live = lv;
    return s;
}
/* one chunk of the debris layer added into l / r arrays (float, full scale 1) */
static void db_mix(float *L, float *R, int frames, float run, float gain) {
    if (!db.init) { db.init = 1; db.rng = 0x2545F491u; db.bed_b.fresh = 1; }
    extern int ps2_imm_debris_mask(void);
    db.mask = ps2_imm_debris_mask();
    db_take(run > 0.0f && gain > 0.0f);
    if (db.mask <= 0 || !(db.mask & 1)) return;
    extern volatile float ps2_imm_deb_bed[8];
    extern volatile int ps2_vk_cockpit_view;
    extern int ps2_spatial_hrtf_on(void);
    extern float ps2_spatial_doppler(void), ps2_spatial_muffle(void);
    const float dt = 1.0f / (float)AUDIO_RATE;
    int hrtf = ps2_spatial_hrtf_on();
    float str = ps2_spatial_doppler(), cs = str > 0.0f ? 343.0f / str : 0.0f;
    float mu = ps2_vk_cockpit_view ? ps2_spatial_muffle() : 0.0f;
    mu = mu < 0.0f ? 0.0f : mu > 1.0f ? 1.0f : mu;
    const float ma = 1.0f - expf(-6.2831853f * 3000.0f * dt), mg = 1.0f - mu * (1.0f - 0.708f);
    /* the bed: its target from the newest update (stale: paused, or no flight -- it fades) */
    u64 now = ps2_kernel_vblank_count();
    float bed_t = run > 0.0f && (float)now - ps2_imm_deb_bed[7] <= 8.0f ? ps2_imm_deb_bed[0] : 0.0f;
    if (!(bed_t >= 0.0f && bed_t <= 1.0f)) bed_t = 0.0f;
    float bin = ps2_imm_deb_bed[5], bdir[3] = { ps2_imm_deb_bed[1], ps2_imm_deb_bed[2], ps2_imm_deb_bed[3] };
    if (!(bin >= 0.0f && bin <= 1.0f)) bin = 0.0f;
    {   float m = sqrtf(bdir[0] * bdir[0] + bdir[1] * bdir[1] + bdir[2] * bdir[2]);
        if (m > 1e-3f) for (int j = 0; j < 3; j++) bdir[j] /= m; else { bdir[0] = bdir[1] = 0.0f; bdir[2] = 1.0f; } }
    if (bed_t > db.bed_max) db.bed_max = bed_t;
    const float kb = 1.0f - expf(-dt / 0.08f);
    for (int i = 0; i < frames; i++) {
        float l = 0.0f, r = 0.0f;
        if (i % 32 == 0) {
            if (db.bed_g > 1e-4f || bed_t > 0.0f) db_bin_block(&db.bed_b, bdir, hrtf);
            for (int k = 0; k < 6; k++) {
                db_zip *z = &db.z[k];
                if (!z->on) continue;
                float pos[3], m;
                for (int j = 0; j < 3; j++) pos[j] = z->p[j] + z->v[j] * z->t;
                m = sqrtf(pos[0] * pos[0] + pos[1] * pos[1] + pos[2] * pos[2]);
                float dir[3] = { 0.0f, 0.0f, 1.0f };
                if (m > 1e-3f) for (int j = 0; j < 3; j++) dir[j] = pos[j] / m;
                db_bin_block(&z->b, dir, hrtf);
                float vr = dir[0] * z->v[0] + dir[1] * z->v[1] + dir[2] * z->v[2];          /* + receding */
                float fd = cs > 0.0f ? cs / (cs + vr) : 1.0f;
                fd = fd < 0.5f ? 0.5f : fd > 2.0f ? 2.0f : fd;
                z->dop = fd;
                float vm = sqrtf(z->v[0] * z->v[0] + z->v[1] * z->v[1] + z->v[2] * z->v[2]);
                float fc = z->f0 * fd * (1.0f + 0.5f * (vm > 250.0f ? 1.0f : vm / 250.0f));
                if (fc > 9000.0f) fc = 9000.0f;
                float rr = z->big ? 0.96f : 0.93f, wq = 6.2831853f * fc * dt;
                z->a1 = 2.0f * rr * cosf(wq); z->a2 = -rr * rr; z->ain = (1.0f - rr) * 2.0f * sinf(wq);
                {   float fb = fc * 2.5f > 15000.0f ? 15000.0f : fc * 2.5f, rb = 0.82f, wb = 6.2831853f * fb * dt;
                    z->ba1 = 2.0f * rb * cosf(wb); z->ba2 = -rb * rb; z->bain = (1.0f - rb) * 2.0f * sinf(wb); }
                /* level: the strength (closeness x speed) compressed, ~1 / distance beyond the reference, flat inside 2 m */
                float ref = z->big ? 15.0f : 6.0f, q = ref / (m > 2.0f ? m : 2.0f);
                float g = powf(z->st, 0.6f) * (q > 1.0f ? 1.0f : q);
                z->g = g;
                /* done: receding beyond 45 m (70 m for the wreck), or 3 s: fades out over ~10 ms */
                if ((vr > 0.0f && m > (z->big ? 70.0f : 45.0f) && z->t > 0.1f) || z->t > 3.0f) { z->g = 0.0f; z->on = -1; }
            }
            for (int k = 0; k < 6; k++) {
                db_hit *h = &db.h[k];
                if (!h->on) continue;
                if (h->t < 0.0f) {
                    h->delay -= 32.0f * dt;
                    if (h->delay <= 0.0f) { if (db_phys()) db_hit_start_phys(h); else db_hit_start(h); }
                }
                if (h->t >= 0.0f) db_bin_block(&h->b, h->dir, hrtf);
            }
            for (int k = 0; k < 3; k++) {
                db_blast *b = &db.bl[k];
                if (!b->on) continue;
                if (b->t < 0.0f) {
                    b->delay -= 32.0f * dt;
                    if (b->delay <= 0.0f) db_blast_start(b);
                }
                if (b->t >= 0.0f) db_bin_block(&b->b, b->dir, hrtf);
            }
        }
        /* zips (several at once share the level: 1 / sqrt(n)) */
        if (i % 32 == 0) {
            int nz = 0;
            for (int k = 0; k < 6; k++) nz += db.z[k].on && db.z[k].g > 0.05f;
            db.zn = nz > 1 ? 1.0f / sqrtf((float)nz) : 1.0f;
        }
        for (int k = 0; k < 6; k++) {
            db_zip *z = &db.z[k];
            if (!z->on) continue;
            z->rng ^= z->rng << 13; z->rng ^= z->rng >> 17; z->rng ^= z->rng << 5;
            float wn = (float)(z->rng >> 8) * (2.0f / 16777216.0f) - 1.0f;
            float y = wn * z->ain + z->a1 * z->y1 + z->a2 * z->y2;
            z->y2 = z->y1; z->y1 = y;
            float fl, s;
            if (db_phys()) {                     /* tumbling: an irregular whoosh at each half turn, and a broader band */
                z->flp += z->flf * dt;
                if (z->flp >= 1.0f) {
                    z->flp = 0.0f;
                    z->flf = (z->big ? 11.0f : 28.0f) * (0.7f + 0.6f * db_frand());
                    z->tstr = 0.4f + 0.8f * db_frand();
                }
                fl = 0.45f + z->tstr * expf(-z->flp * 5.0f);
                float yb = wn * z->bain + z->ba1 * z->b1 + z->ba2 * z->b2;
                z->b2 = z->b1; z->b1 = yb;
                s = (y * 1.6f + yb * 0.9f) * fl * 0.68f;
            } else {
                z->flp += z->flf * dt; if (z->flp >= 1.0f) z->flp -= 1.0f;
                fl = 0.7f + 0.3f * sinf(6.2831853f * z->flp);
                s = y * 1.6f * fl;
            }
            if (z->big) { z->roar += (wn - z->roar) * 0.012f; s += z->roar * 2.5f; }
            z->gs += (z->g - z->gs) * (z->t < 0.03f ? 0.002f : 0.0021f);       /* ~10 ms glide (also the fade-in) */
            s *= z->gs * db.zn * (z->big ? 0.5f : 0.45f);
            float zl = 0.0f, zr = 0.0f;
            db_bin_run(&z->b, s, &zl, &zr);
            if (mu > 0.0f) {                     /* outside the canopy: muffled in the cockpit view */
                z->mlp[0] += ma * (zl - z->mlp[0]); z->mlp[1] += ma * (zr - z->mlp[1]);
                zl = (zl + mu * (z->mlp[0] - zl)) * mg; zr = (zr + mu * (z->mlp[1] - zr)) * mg;
            }
            l += zl; r += zr;
            z->t += dt;
            if (z->on < 0 && z->gs < 1e-4f) z->on = 0;
        }
        /* canopy hits */
        for (int k = 0; k < 6; k++) {
            db_hit *h = &db.h[k];
            if (!h->on || h->t < 0.0f) continue;
            float s = 0.0f, live = 0.0f;
            if (db_phys()) {
                s = db_hit_run_phys(h, &live);
                db_bin_run(&h->b, s, &l, &r);
                h->t += dt;
                if (h->t > 0.02f && live < 2e-5f) h->on = 0;
                if (h->t > 0.6f) h->on = 0;
                continue;
            }
            for (int m = 0; m < 3; m++) {
                if (h->c[m] == 0.0f) continue;
                float y = h->c[m] * h->y1[m] - h->r2[m] * h->y2[m];
                h->y2[m] = h->y1[m]; h->y1[m] = y;
                s += y; live += fabsf(y) + fabsf(h->y2[m]);
            }
            s += h->th_amp * sinf(6.2831853f * (h->clank ? 95.0f : 180.0f) * h->t) * expf(-h->t / (h->clank ? 0.06f : 0.02f));
            db_bin_run(&h->b, s, &l, &r);
            h->t += dt;
            if (h->t > 0.05f && live < 2e-5f) h->on = 0;
            if (h->t > 0.6f) h->on = 0;
        }
        /* blasts on the canopy (bug #232) */
        for (int k = 0; k < 3; k++) {
            db_blast *b = &db.bl[k];
            if (!b->on || b->t < 0.0f) continue;
            db_bin_run(&b->b, db_blast_run(b), &l, &r);
            if (b->t > 0.7f) b->on = 0;
        }
        /* the bed */
        db.bed_g += (bed_t - db.bed_g) * kb;
        if (db.bed_g > 1e-4f) {
            float g = db.bed_g;
            float wn = db_frand() * 2.0f - 1.0f;
            float wq = 6.2831853f * 1500.0f * dt, rr = 0.9f;
            float y = wn * (1.0f - rr) * 2.0f * sinf(wq) + 2.0f * rr * cosf(wq) * db.bed_y1 - rr * rr * db.bed_y2;
            db.bed_y2 = db.bed_y1; db.bed_y1 = y;
            float hs = y * 0.05f * g, bl = 0.0f, br = 0.0f;
            db_bin_run(&db.bed_b, hs * (1.0f - 0.7f * bin), &bl, &br);
            for (int c = 0; c < 2; c++) {        /* all around: decorrelated */
                float w2 = db_frand() * 2.0f - 1.0f;
                db.bed_dc[c] += (w2 - db.bed_dc[c]) * 0.35f;
            }
            bl += db.bed_dc[0] * 0.03f * g * bin; br += db.bed_dc[1] * 0.03f * g * bin;
            /* the clicks: tiny bits ticking past, ~150 a second at full depth */
            if (db_frand() < 150.0f * g * dt) {
                for (int k = 0; k < 8; k++)
                    if (db.bt[k].y1 == 0.0f && db.bt[k].y2 == 0.0f) {
                        if (db_phys()) {             /* a grain on the glass: two modes, acrylic's decay, power-law sizes */
                            float f = 2500.0f * powf(3.6f, db_frand()), f2 = f * (1.4f + 0.9f * db_frand());
                            float r2 = expf(-dt * 3.14159265f * f * DB_ETA), r2b = expf(-dt * 3.14159265f * f2 * DB_ETA);
                            float a = 0.010f * powf(db_frand() + 0.02f, -0.5f); a = a > 0.05f ? 0.05f : a;
                            db.bt[k].c = 2.0f * r2 * cosf(6.2831853f * f * dt); db.bt[k].r2 = r2 * r2;
                            db.bt[k].y1 = a * sqrtf(g) * sinf(6.2831853f * f * dt); db.bt[k].y2 = 0.0f;
                            db.bt[k].cb = 2.0f * r2b * cosf(6.2831853f * f2 * dt); db.bt[k].r2b = r2b * r2b;
                            db.bt[k].y1b = 0.6f * a * sqrtf(g) * sinf(6.2831853f * f2 * dt) * (db_frand() < 0.5f ? -1.0f : 1.0f); db.bt[k].y2b = 0.0f;
                            db.bt[k].nk = db_frand() < 0.15f ? 1.0f + (float)(int)(2.0f * db_frand()) : 0.0f;   /* a skitter */
                            db.bt[k].ka = 0.5f * db.bt[k].y1; db.bt[k].kb = 0.5f * db.bt[k].y1b;
                            db.bt[k].next = 0.0015f + 0.003f * db_frand();
                            float az = atan2f(bdir[0], bdir[2]) + (db_frand() - 0.5f) * 3.14159265f * (0.6f + 1.4f * bin);
                            db.bt[k].pan = 0.5f + 0.5f * sinf(az);
                        } else {
                            float f = 3000.0f + 4000.0f * db_frand(), r2 = expf(-dt / (0.002f + 0.003f * db_frand()));
                            db.bt[k].c = 2.0f * r2 * cosf(6.2831853f * f * dt); db.bt[k].r2 = r2 * r2;
                            db.bt[k].y1 = (0.012f + 0.023f * db_frand()) * sqrtf(g) * sinf(6.2831853f * f * dt);
                            db.bt[k].y1b = db.bt[k].y2b = db.bt[k].cb = db.bt[k].r2b = 0.0f; db.bt[k].nk = 0.0f;
                            float az = atan2f(bdir[0], bdir[2]) + (db_frand() - 0.5f) * 6.2831853f * bin;
                            db.bt[k].pan = 0.5f + 0.5f * sinf(az);
                        }
                        db.n_bedtick++;
                        break;
                    }
            }
            for (int k = 0; k < 8; k++) {
                if (db.bt[k].y1 == 0.0f && db.bt[k].y2 == 0.0f) continue;
                float y2 = db.bt[k].c * db.bt[k].y1 - db.bt[k].r2 * db.bt[k].y2;
                db.bt[k].y2 = db.bt[k].y1; db.bt[k].y1 = y2;
                float yb = db.bt[k].cb * db.bt[k].y1b - db.bt[k].r2b * db.bt[k].y2b;
                db.bt[k].y2b = db.bt[k].y1b; db.bt[k].y1b = yb;
                if (db.bt[k].nk > 0.0f) {            /* the grain bounces on: the modes kicked again, weaker */
                    db.bt[k].next -= dt;
                    if (db.bt[k].next <= 0.0f) {
                        db.bt[k].y1 += db.bt[k].ka; db.bt[k].y1b += db.bt[k].kb;
                        db.bt[k].ka *= 0.55f; db.bt[k].kb *= 0.55f;
                        db.bt[k].nk -= 1.0f; db.bt[k].next = 0.0015f + 0.003f * db_frand();
                    }
                }
                float y = y2 + yb;
                bl += y * cosf(db.bt[k].pan * 1.5707963f); br += y * sinf(db.bt[k].pan * 1.5707963f);
                if (fabsf(y2) < 1e-5f && fabsf(db.bt[k].y2) < 1e-5f && db.bt[k].nk <= 0.0f) db.bt[k].y1 = db.bt[k].y2 = 0.0f;
            }
            if (mu > 0.0f) {
                db.bed_mlp[0][0] += ma * (bl - db.bed_mlp[0][0]); db.bed_mlp[0][1] += ma * (br - db.bed_mlp[0][1]);
                bl = (bl + mu * (db.bed_mlp[0][0] - bl)) * mg; br = (br + mu * (db.bed_mlp[0][1] - br)) * mg;
            }
            l += bl; r += br;
        }
        L[i] += l; R[i] += r;
    }
}
static void imm_mix(s16 *buf, int frames) {
    if (!imm.init) {
        imm.rng = 0x9E3779B9u; imm.init = 1; imm.w_t = -1.0f;
        const char *e = getenv("ACZ_DEBUG_IMM_SOUND"); imm.dbg = e ? atoi(e) : 0;
        e = getenv("ACZ_IMM_DRIP"); imm.drip = e ? atoi(e) : 1;
        if (imm.drip < 0 || imm.drip > 2) imm.drip = 1;
        e = getenv("ACZ_IMM_SNOW_SOUND"); imm.sn_k = e && *e ? (float)atof(e) : 1.0f;
        if (e && !strcmp(e, "off")) imm.sn_k = 0.0f;
        if (!(imm.sn_k >= 0.0f)) imm.sn_k = 0.0f;
        if (imm.sn_k > 2.0f) imm.sn_k = 2.0f;
    }
    {   extern float ps2_imm_sound(void);
        imm.gain = ps2_imm_sound();
    }
    {   extern void ps2_gf_mix(s16 *buf, int frames);     /* the pilot under G: breath, strain, G-suit, buffet */
        ps2_gf_mix(buf, frames);
    }
    {   extern void ps2_crk_mix(short *buf, int frames);   /* canopy cracks: the crack's snap and ring, creep ticks */
        ps2_crk_mix(buf, frames);
    }
    if (imm.gain <= 0.0f) { db_take(0); return; }      /* (debris events taken, not kept for later) */
    const float dt = 1.0f / (float)AUDIO_RATE;
    float cloud = ps2_imm_audio[0], rain = ps2_imm_audio[1], snow = ps2_imm_audio[2], kt = ps2_imm_audio[3];
    float run = ps2_imm_audio[4] > 0.0f && (float)ps2_kernel_vblank_count() - ps2_imm_audio[4] <= 8.0f ? 1.0f : 0.0f;
    float spd = kt > 450.0f ? 1.0f : kt / 450.0f;
    {   float d = ps2_imm_audio[5];          /* new debris: schedule a burst of ticks and a thump */
        if (d > imm.debris_seen + 0.01f && run > 0.0f) {
            float st = d - imm.debris_seen; if (st > 1.0f) st = 1.0f;
            int n = 3 + (int)(st * 7.0f);
            for (int i = 0; i < n && imm.nt_due < 16; i++) imm.tick_due[imm.nt_due++] = imm_frand() * 0.28f;
            imm.dbg_deb++;
            imm.thump_t = 0.0f; imm.thump_amp = 0.12f * st;
        }
        imm.debris_seen = d;
    }
    {   extern volatile float ps2_imm_cloud_ev[4];     /* ps2_video.c cloud_measure_collect: entries / exits */
        float ein = ps2_imm_cloud_ev[0], eout = ps2_imm_cloud_ev[1];
        if (ein > imm.cl_in_seen + 0.01f && run > 0.0f) {
            float st = ein - imm.cl_in_seen; if (st > 1.0f) st = 1.0f;
            imm.w_t = 0.0f; imm.w_amp = (0.33f + 0.27f * st) * (0.5f + 0.5f * spd); imm.w_f0 = 260.0f; imm.w_f1 = 1300.0f;
            imm.thump_t = 0.0f; imm.thump_amp = 0.045f * st;
            imm.dbg_win++;
        } else if (eout > imm.cl_out_seen + 0.01f && run > 0.0f) {
            float st = eout - imm.cl_out_seen; if (st > 1.0f) st = 1.0f;
            imm.w_t = 0.0f; imm.w_amp = (0.18f + 0.15f * st) * (0.5f + 0.5f * spd); imm.w_f0 = 500.0f; imm.w_f1 = 2000.0f;
            imm.dbg_wout++;
        }
        imm.cl_in_seen = ein; imm.cl_out_seen = eout;
    }
    /* cloud droplets on the canopy: only while the cloud is around the aircraft (ACZ_IMM_DRIP=1, the default) and, as
       cloud droplets are tiny, a fine soft fizz -- twice as many ticks, ~0.57 x the level, half as long -- instead of
       the knock of snow or ice. Before (ACZ_IMM_DRIP=2) the patter went on from the humidity memory (0.35 x, ~15 s
       linger) for ~40 s after a cloud: after mission 2's take-off 15-18 ticks per 2 s in clear air, still 4-5 per 2 s
       30 s later (bug #41: "like snow or ice knocking on the windows, but it is not that"). 0 = none */
    float t_drip = imm.drip ? run * (cloud > 0.0f ? cloud : 0.0f) : 0.0f;
    if (imm.drip == 2) {
        extern volatile float ps2_imm_cloud_ev[4];
        float hu = ps2_imm_cloud_ev[2];                   /* humidity: the glass stays wet a while after */
        if (hu * 0.35f > t_drip) t_drip = run * hu * 0.35f;
    }
    const int fine = imm.drip == 1;
    /* targets: airflow swells inside cloud (with speed), patter with the precipitation and the airflow */
    float t_air = run * cloud * (0.35f + 0.65f * spd);
    {   extern float ps2_imm_duck(int cls);          /* the immersion bus: under a loud game and the radio, lower */
        t_air *= ps2_imm_duck(0); }
    float t_rain = run * rain, t_snow = run * snow;
    float t_sn = 0.0f;                                  /* snow on the canopy (ACZ_IMM_SNOW_SOUND): its target gain */
    if (imm.sn_k > 0.0f && snow > 0.0f) {
        extern float ps2_imm_duck(int cls);
        t_sn = run * snow * imm.sn_k * (0.25f + 0.75f * powf(spd, 1.3f)) * sqrtf(ps2_imm_duck(0));   /* (half the beds' dB) */
    }
    const float sn_ah = 1.0f - expf(-6.2831853f * 1400.0f * dt), sn_al = 1.0f - expf(-6.2831853f * 6500.0f * dt);
    const float sn_kg = 1.0f - expf(-dt / 0.4f);
    float fc = 300.0f + 900.0f * cloud + 600.0f * spd;          /* airflow low-pass corner (Hz) */
    float a1 = 1.0f - expf(-6.2831853f * fc * dt), a2 = 1.0f - expf(-6.2831853f * fc * 0.5f * dt);
    float k_up = 1.0f - expf(-dt / 0.25f), k_dn = 1.0f - expf(-dt / 0.9f);
    float dbl[AUDIO_CHUNK], dbr[AUDIO_CHUNK];          /* debris passes (db_mix) */
    if (frames > AUDIO_CHUNK) frames = AUDIO_CHUNK;
    memset(dbl, 0, sizeof(float) * (size_t)frames); memset(dbr, 0, sizeof(float) * (size_t)frames);
    db_mix(dbl, dbr, frames, run, imm.gain);
    float rate = (500.0f * imm.g_rain + 90.0f * imm.g_snow + (fine ? 320.0f : 160.0f) * imm.g_drip) * (0.3f + 0.7f * spd);   /* drops a second */
    for (int i = 0; i < frames; i++) {
        imm.g_air += (t_air - imm.g_air) * (t_air > imm.g_air ? k_up : k_dn);
        imm.g_rain += (t_rain - imm.g_rain) * k_dn;
        imm.g_snow += (t_snow - imm.g_snow) * k_dn;
        imm.g_drip += (t_drip - imm.g_drip) * k_dn;
        float l = 0.0f, r = 0.0f;
        imm.sn_g += (t_sn - imm.sn_g) * sn_kg;
        if (imm.sn_g > 1e-4f) {                         /* snow on the canopy (ACZ_IMM_SNOW_SOUND, bug #260) */
            /* the density's slow wander: a smoothed random walk, 0.3-1.5 Hz */
            imm.sn_modv += ((imm_frand() * 2.0f - 1.0f) * 6.0f - imm.sn_modv) * 2.0e-4f;
            imm.sn_mod += imm.sn_modv * dt;
            if (imm.sn_mod > 0.35f) { imm.sn_mod = 0.35f; imm.sn_modv = -fabsf(imm.sn_modv); }
            if (imm.sn_mod < -0.35f) { imm.sn_mod = -0.35f; imm.sn_modv = fabsf(imm.sn_modv); }
            float g = imm.sn_g * (1.0f + imm.sn_mod);
            /* the hiss: three band-limited noises -- [0] from ahead (both ears), [1] / [2] each side's own */
            float hs[3];
            for (int c = 0; c < 3; c++) {
                float w = imm_frand() * 2.0f - 1.0f;
                float y = w - imm.sn_hp[c]; imm.sn_hp[c] += (w - imm.sn_hp[c]) * sn_ah;
                imm.sn_l1[c] += (y - imm.sn_l1[c]) * sn_al; imm.sn_l2[c] += (imm.sn_l1[c] - imm.sn_l2[c]) * sn_al;
                hs[c] = imm.sn_l2[c];
            }
            l += (hs[0] * 0.75f + hs[1] * 0.45f) * 0.55f * g;
            r += (hs[0] * 0.75f + hs[2] * 0.45f) * 0.55f * g;
            /* grains (the sandy texture) and clumps (soft tuks): new bursts */
            float gr = 400.0f * imm.sn_g, cr = 6.0f + 18.0f * imm.sn_g;
            int nb = -1;
            float u = imm_frand();
            if (u < gr * dt) nb = 0; else if (u < (gr + cr) * dt) nb = 1;
            if (nb >= 0)
                for (int j = 0; j < 12; j++)
                    if (imm.sn_b[j].env <= 0.0f) {
                        float a = imm_frand();
                        imm.sn_b[j].env = 1.0f;
                        imm.sn_b[j].dec = expf(-dt / (nb ? 0.003f + 0.004f * imm_frand() : 0.0005f + 0.0009f * imm_frand()));
                        imm.sn_b[j].amp = (nb ? 0.10f + 0.14f * a * a : 0.04f + 0.12f * a * a * a) * g;
                        imm.sn_b[j].pan = 0.5f + (imm_frand() - 0.5f) * 0.7f;
                        imm.sn_b[j].lpa = nb ? 1.0f - expf(-6.2831853f * (1200.0f + 2000.0f * imm_frand()) * dt) : 1.0f;
                        imm.sn_b[j].hp = imm.sn_b[j].lp = 0.0f; imm.sn_b[j].nz = imm_rand() | 1u;
                        if (nb) imm.dbg_sclump++; else imm.dbg_sgrain++;
                        break;
                    }
            for (int j = 0; j < 12; j++) {
                if (imm.sn_b[j].env <= 0.0f) continue;
                u32 *z = &imm.sn_b[j].nz;
                *z ^= *z << 13; *z ^= *z >> 17; *z ^= *z << 5;
                float w = ((float)(*z >> 8) * (2.0f / 16777216.0f) - 1.0f) * imm.sn_b[j].env * imm.sn_b[j].amp;
                float y = w - imm.sn_b[j].hp; imm.sn_b[j].hp += (w - imm.sn_b[j].hp) * sn_ah;
                imm.sn_b[j].lp += (y - imm.sn_b[j].lp) * imm.sn_b[j].lpa;
                l += imm.sn_b[j].lp * (1.0f - imm.sn_b[j].pan) * 1.4f; r += imm.sn_b[j].lp * imm.sn_b[j].pan * 1.4f;
                imm.sn_b[j].env *= imm.sn_b[j].dec;
                if (imm.sn_b[j].env < 0.02f) imm.sn_b[j].env = 0.0f;
            }
            if (imm.sn_g > imm.dbg_sn_max) imm.dbg_sn_max = imm.sn_g;
        }
        if (imm.w_t >= 0.0f && imm.w_amp > 0.0f) {      /* the whoosh: noise through a band-pass sweeping up */
            float u = imm.w_t / 0.9f;
            float env = (u < 0.09f ? u / 0.09f : expf(-(u - 0.09f) * 3.2f));
            float fc = imm.w_f0 + (imm.w_f1 - imm.w_f0) * (u > 1.0f ? 1.0f : u);
            float wq = 6.2831853f * fc * dt, rr = 0.985f;
            float wn = imm_frand() * 2.0f - 1.0f;
            float y = wn * (1.0f - rr) * 2.0f * sinf(wq) + 2.0f * rr * cosf(wq) * imm.w_y1 - rr * rr * imm.w_y2;   /* peak gain ~1 */
            imm.w_y2 = imm.w_y1; imm.w_y1 = y;
            imm.w_lp += (wn - imm.w_lp) * 0.02f;           /* plus a little rumble under it */
            float o = (y * 1.6f + imm.w_lp * 0.5f) * env * imm.w_amp;
            l += o; r += o;
            imm.w_t += dt;
            if (imm.w_t > 2.5f) imm.w_t = -1.0f;
        }
        if (imm.g_air > 1e-4f) {                      /* two decorrelated band-limited noises */
            for (int c = 0; c < 2; c++) {
                float w = imm_frand() * 2.0f - 1.0f;
                imm.lp[c] += (w - imm.lp[c]) * a1;
                imm.lp2[c] += (imm.lp[c] - imm.lp2[c]) * a2;
            }
            l += imm.lp2[0] * 0.22f * imm.g_air; r += imm.lp2[1] * 0.22f * imm.g_air;
        }
        if (rate > 0.5f && imm_frand() < rate * dt && imm.nd < 24) {     /* a new drop hits the glass */
            imm_drop *d = &imm.drop[imm.nd++];
            float snowy = imm.g_snow / (imm.g_snow + imm.g_rain + 1e-6f);
            float dripy = imm.g_drip / (imm.g_drip + imm.g_rain + imm.g_snow + 1e-6f);   /* cloud droplets: finer */
            d->amp = (0.024f + 0.1f * imm_frand()) * (1.0f - 0.7f * snowy) * (1.0f - (fine ? 0.8f : 0.65f) * dripy);
            d->decay = expf(-dt / ((0.0008f + 0.002f * imm_frand()) * (1.0f + 2.0f * snowy) * (fine ? 1.0f - 0.5f * dripy : 1.0f)));
            d->pan = imm_frand(); d->env = 1.0f; d->hp = 0.0f; d->noise = imm_rand() | 1u;
            imm.dbg_drops++;
        }
        for (int k = 0; k < imm.nd; ) {               /* noise bursts through a high-pass: ticks on the canopy */
            imm_drop *d = &imm.drop[k];
            d->noise ^= d->noise << 13; d->noise ^= d->noise >> 17; d->noise ^= d->noise << 5;
            float w = (float)(d->noise >> 8) * (2.0f / 16777216.0f) - 1.0f;
            float x = w * d->env * d->amp;
            float h = x - d->hp; d->hp += (x - d->hp) * 0.25f;          /* ~2 kHz one-pole high-pass */
            l += h * (1.0f - d->pan); r += h * d->pan;
            d->env *= d->decay;
            if (d->env < 0.01f) imm.drop[k] = imm.drop[--imm.nd]; else k++;
        }
        for (int k = 0; k < imm.nt_due; ) {           /* debris: scheduled ticks become resonators */
            imm.tick_due[k] -= dt;
            if (imm.tick_due[k] <= 0.0f) {
                for (int j = 0; j < 16; j++)
                    if (imm.tick[j].amp <= 0.0f) {
                        float f = 1800.0f + 3500.0f * imm_frand(), rr = expf(-dt / (0.012f + 0.025f * imm_frand()));
                        imm.tick[j].c = 2.0f * rr * cosf(6.2831853f * f * dt); imm.tick[j].r2 = rr * rr;
                        imm.tick[j].y1 = 0.0f; imm.tick[j].y2 = 0.0f;
                        imm.tick[j].amp = 0.025f + 0.05f * imm_frand(); imm.tick[j].pan = imm_frand();
                        imm.tick[j].y1 = imm.tick[j].amp;             /* the impulse */
                        break;
                    }
                imm.tick_due[k] = imm.tick_due[--imm.nt_due];
            } else k++;
        }
        for (int j = 0; j < 16; j++) {
            imm_tick *t = &imm.tick[j];
            if (t->amp <= 0.0f) continue;
            float y = t->c * t->y1 - t->r2 * t->y2;
            t->y2 = t->y1; t->y1 = y;
            l += y * (1.0f - t->pan); r += y * t->pan;
            if (fabsf(y) < 1e-4f && fabsf(t->y2) < 1e-4f) t->amp = 0.0f;
        }
        if (imm.thump_amp > 0.0f) {                   /* a dull knock through the airframe */
            float y = imm.thump_amp * sinf(6.2831853f * 85.0f * imm.thump_t) * expf(-imm.thump_t / 0.06f);
            l += y; r += y;
            imm.thump_t += dt;
            if (imm.thump_t > 0.4f) imm.thump_amp = 0.0f;
        }
        l += dbl[i]; r += dbr[i];
        int L = buf[2 * i] + (int)(l * imm.gain * 32767.0f), R = buf[2 * i + 1] + (int)(r * imm.gain * 32767.0f);
        buf[2 * i] = (s16)(L > 32767 ? 32767 : L < -32768 ? -32768 : L);
        buf[2 * i + 1] = (s16)(R > 32767 ? 32767 : R < -32768 ? -32768 : R);
    }
    if (imm.dbg > 0 && (imm.dbg_n += (u32)frames) >= (u32)imm.dbg * AUDIO_RATE) {
        ps2_log("imm-sound: field %llu run %.0f cloud %.2f rain %.2f snow %.2f kt %.0f | g_air %.2f g_rain %.2f g_snow %.2f "
                "g_drip %.2f | %u drops, whoosh in %u out %u, debris %u, debris zips %u hits %u bed clicks %u (bed max %.2f) | "
                "snow on the canopy %.2f (max), %u grains, %u clumps in %d s",
                (unsigned long long)ps2_kernel_vblank_count(), run, cloud, rain, snow, kt, imm.g_air, imm.g_rain, imm.g_snow,
                imm.g_drip, imm.dbg_drops, imm.dbg_win, imm.dbg_wout, imm.dbg_deb, db.n_zip, db.n_hit, db.n_bedtick, db.bed_max,
                imm.dbg_sn_max, imm.dbg_sgrain, imm.dbg_sclump, imm.dbg);
        imm.dbg_sn_max = 0.0f; imm.dbg_sgrain = imm.dbg_sclump = 0;
        imm.dbg_n = imm.dbg_drops = imm.dbg_win = imm.dbg_wout = imm.dbg_deb = 0;
        db.n_zip = db.n_hit = db.n_bedtick = 0; db.bed_max = 0.0f;
    }
}

/* Cockpit system sounds (bug #88, zero_syssnd.c): a layer mixed after the immersion sounds; set by the game side
   (NULL = none, e.g. gsreplay). Adds into the interleaved stereo chunk. */
void (*ps2_audio_sys_mix)(s16 *buf, int frames);
/* Close passes of other aircraft (bug #140, zero_flyby.c): mixed after the system sounds, same contract. */
void (*ps2_audio_flyby_mix)(s16 *buf, int frames);
/* the immersion bus (ps2_gforce_audio.c): the game's own mix of each chunk, before our layers, sets how far they duck */
void ps2_imm_bus_game(const s16 *game, int frames);

static void SDLCALL audio_cb(void *userdata, SDL_AudioStream *stream,
                             int additional_amount, int total_amount) {
    s16 buf[AUDIO_CHUNK * AUDIO_CHANNELS];
    (void)userdata;
    (void)total_amount;
    u64 cz_cb = ps2_cz_t();                                /* (PS2_CPU_PROF) */
    while (additional_amount > 0) {
        int want_bytes = additional_amount;
        int frames, bytes;
        if (want_bytes > (int)sizeof buf) want_bytes = (int)sizeof buf;
        frames = want_bytes / (int)(AUDIO_CHANNELS * sizeof(s16));
        if (frames <= 0) break;
        bytes = frames * (int)(AUDIO_CHANNELS * sizeof(s16));
        if (wav_ref.fp) {                   /* A/B tap: the immersion layer rendered once, added to both mixes */
            s16 ref[AUDIO_CHUNK * AUDIO_CHANNELS], im[AUDIO_CHUNK * AUDIO_CHANNELS];
            ps2_spu2_mix2(buf, ref, (u32)frames);
            ps2_imm_bus_game(buf, frames);
            memset(im, 0, (size_t)bytes);
            imm_mix(im, frames);
            if (ps2_audio_sys_mix) ps2_audio_sys_mix(im, frames);
            if (ps2_audio_flyby_mix) ps2_audio_flyby_mix(im, frames);
            for (int j = 0; j < frames * AUDIO_CHANNELS; j++) {
                int a = buf[j] + im[j], b = ref[j] + im[j];
                buf[j] = (s16)(a > 32767 ? 32767 : a < -32768 ? -32768 : a);
                ref[j] = (s16)(b > 32767 ? 32767 : b < -32768 ? -32768 : b);
            }
            wav_put1(&wav_ref, ref, bytes);
            if (wav_imm.fp) wav_put1(&wav_imm, im, bytes);
        } else if (wav_imm.fp) {            /* test tap of the immersion layer alone */
            s16 im[AUDIO_CHUNK * AUDIO_CHANNELS];
            ps2_spu2_mix(buf, (u32)frames);
            ps2_imm_bus_game(buf, frames);
            memset(im, 0, (size_t)bytes);
            imm_mix(im, frames);
            if (ps2_audio_sys_mix) ps2_audio_sys_mix(im, frames);
            if (ps2_audio_flyby_mix) ps2_audio_flyby_mix(im, frames);
            for (int j = 0; j < frames * AUDIO_CHANNELS; j++) {
                int a = buf[j] + im[j];
                buf[j] = (s16)(a > 32767 ? 32767 : a < -32768 ? -32768 : a);
            }
            wav_put1(&wav_imm, im, bytes);
        } else {
            u64 cz0 = ps2_cz_t();                          /* (PS2_CPU_PROF: the callback's three layers) */
            ps2_spu2_mix(buf, (u32)frames);
            ps2_imm_bus_game(buf, frames);
            ps2_cz_end(CZ_AUD_GAME, cz0);
            cz0 = ps2_cz_t();
            imm_mix(buf, frames);
            ps2_cz_end(CZ_AUD_IMM, cz0);
            cz0 = ps2_cz_t();
            if (ps2_audio_sys_mix) ps2_audio_sys_mix(buf, frames);
            if (ps2_audio_flyby_mix) ps2_audio_flyby_mix(buf, frames);
            ps2_cz_end(CZ_AUD_SYS, cz0);
        }
        SDL_PutAudioStreamData(stream, buf, bytes);
        wav_put(buf, bytes);
        ps2_rx_audio(buf, frames);          /* ACZ_REC_EXACT: the frame-exact recording's game track */
        audio_bytes += (u64)bytes;
        additional_amount -= bytes;
    }
    ps2_cz_end(CZ_AUD_CB, cz_cb);
}

#ifdef _WIN32
/* The friendly name of a Windows audio endpoint from its Core Audio id (IMMDevice::GetId form, what
   XR_OCULUS_audio_device_guid returns): IMMDeviceEnumerator::GetDevice, then PKEY_Device_FriendlyName -- the name
   SDL's WASAPI backend gives the device. *is_default: it is also Windows' default playback endpoint (console role).
   ole32 is loaded at run time (the build does not link it). */
#define COBJMACROS
#include <windows.h>
#include <mmdeviceapi.h>
#include <propsys.h>
static int endpoint_name(const wchar_t *id, char *out, int outsz, int *is_default) {
    static const CLSID clsid_enum = { 0xBCDE0395u, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
    static const IID iid_enum = { 0xA95664D2u, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
    static const PROPERTYKEY key_name = { { 0xA45C254Eu, 0xDF1C, 0x4EFD, { 0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0 } }, 14 };
    typedef HRESULT (WINAPI *pfn_init)(LPVOID, DWORD);
    typedef HRESULT (WINAPI *pfn_create)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID *);
    typedef void (WINAPI *pfn_uninit)(void);
    typedef HRESULT (WINAPI *pfn_clear)(PROPVARIANT *);
    HMODULE ole = LoadLibraryA("ole32.dll");
    if (!ole) return 0;
    pfn_init co_init = (pfn_init)(void *)GetProcAddress(ole, "CoInitializeEx");
    pfn_create co_create = (pfn_create)(void *)GetProcAddress(ole, "CoCreateInstance");
    pfn_uninit co_uninit = (pfn_uninit)(void *)GetProcAddress(ole, "CoUninitialize");
    pfn_clear pv_clear = (pfn_clear)(void *)GetProcAddress(ole, "PropVariantClear");
    typedef void (WINAPI *pfn_free)(LPVOID);
    pfn_free co_free = (pfn_free)(void *)GetProcAddress(ole, "CoTaskMemFree");
    int ok = 0;
    *is_default = 0;
    if (co_init && co_create && co_uninit && pv_clear && co_free) {
        HRESULT hi = co_init(NULL, COINIT_MULTITHREADED);     /* RPC_E_CHANGED_MODE: usable, but not ours to undo */
        IMMDeviceEnumerator *en = NULL; IMMDevice *dev = NULL; IPropertyStore *ps = NULL;
        PROPVARIANT pv; memset(&pv, 0, sizeof pv);
        if (SUCCEEDED(co_create(&clsid_enum, NULL, CLSCTX_ALL, &iid_enum, (void **)&en)) && en
            && SUCCEEDED(IMMDeviceEnumerator_GetDevice(en, id, &dev)) && dev
            && SUCCEEDED(IMMDevice_OpenPropertyStore(dev, STGM_READ, &ps)) && ps
            && SUCCEEDED(IPropertyStore_GetValue(ps, &key_name, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal)
            ok = WideCharToMultiByte(CP_UTF8, 0, pv.pwszVal, -1, out, outsz, NULL, NULL) > 0;
        if (ok) {
            IMMDevice *def = NULL; LPWSTR did = NULL;
            if (SUCCEEDED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &def)) && def
                && SUCCEEDED(IMMDevice_GetId(def, &did)) && did)
                *is_default = !wcscmp(did, id);
            if (did) co_free(did);
            if (def) IMMDevice_Release(def);
        }
        pv_clear(&pv);
        if (ps) IPropertyStore_Release(ps);
        if (dev) IMMDevice_Release(dev);
        if (en) IMMDeviceEnumerator_Release(en);
        if (hi == S_OK || hi == S_FALSE) co_uninit();
    }
    FreeLibrary(ole);
    return ok;
}
#endif
const wchar_t *ps2_xr_audio_output_id(void);   /* ps2_xr.c: the headset's endpoint id, or NULL */

static int name_has(const char *name, const char *want) {   /* case-insensitive substring */
    size_t n = strlen(want);
    for (const char *p = name; *p; p++) if (!SDL_strncasecmp(p, want, n)) return 1;
    return 0;
}

/* The playback device. PS2_AUDIO_DEVICE unset: the headset's own device when the XR runtime names it
   (XR_OCULUS_audio_device_guid, Meta's runtime under Link; ps2_xr.c), else the system default. When the headset is
   the system default anyway (Link usually makes "Headphones (Oculus Virtual Audio Device)" the default), the default
   is opened, so SDL keeps following Windows' default.
   PS2_AUDIO_DEVICE=default: always the system default (follows Windows' choice). PS2_AUDIO_DEVICE=<text>: the first
   playback device whose name contains the text (case-insensitive), e.g. "Oculus" or "Speakers"; the default if none.
   The log lists the devices and says which one was opened. */
static SDL_AudioDeviceID audio_pick_device(void) {
    const char *want = getenv("PS2_AUDIO_DEVICE");
    char xr_name[256] = "";
    const wchar_t *xid = ps2_xr_audio_output_id();
    int count = 0;
    SDL_AudioDeviceID *ids = SDL_GetAudioPlaybackDevices(&count), pick = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    char list[1024]; int n = 0;
    list[0] = 0;
    for (int i = 0; ids && i < count && n < (int)sizeof list - 8; i++) {
        const char *nm = SDL_GetAudioDeviceName(ids[i]);
        n += snprintf(list + n, sizeof list - (size_t)n, "%s%s", i ? " | " : "", nm ? nm : "?");
    }
    ps2_log("audio: %d playback devices: %s", count, list);
    if (want && *want && SDL_strcasecmp(want, "default")) {
        for (int i = 0; ids && i < count && pick == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK; i++) {
            const char *nm = SDL_GetAudioDeviceName(ids[i]);
            if (nm && name_has(nm, want)) pick = ids[i];
        }
        if (pick == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK) ps2_log("audio: no playback device matches PS2_AUDIO_DEVICE=%s", want);
    } else if (!want && xid) {
#ifdef _WIN32
        int is_def = 0;
        if (endpoint_name(xid, xr_name, (int)sizeof xr_name, &is_def) && is_def)
            ps2_log("audio: the headset's device \"%s\" is the system default", xr_name);
        else if (xr_name[0]) {
            for (int i = 0; ids && i < count && pick == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK; i++) {
                const char *nm = SDL_GetAudioDeviceName(ids[i]);
                if (nm && !strcmp(nm, xr_name)) pick = ids[i];
            }
            for (int i = 0; ids && i < count && pick == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK; i++) {
                const char *nm = SDL_GetAudioDeviceName(ids[i]);
                if (nm && name_has(nm, xr_name)) pick = ids[i];
            }
            ps2_log("audio: the headset's device is \"%s\"%s", xr_name,
                    pick == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK ? ", not among the playback devices" : "");
        } else ps2_log("audio: the headset's endpoint id did not resolve to a device name");
#endif
    }
    SDL_free(ids);
    return pick;
}

void ps2_audio_start(void) {
    SDL_AudioSpec spec;
    if (audio_started) return;
    if (getenv("PS2_NO_AUDIO")) {
        ps2_log("audio: disabled by PS2_NO_AUDIO");
        return;
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        ps2_log("audio: SDL_InitSubSystem(AUDIO): %s", SDL_GetError());
        return;
    }
    ps2_spu2_init();
    spec.format = SDL_AUDIO_S16LE;
    spec.channels = AUDIO_CHANNELS;
    spec.freq = AUDIO_RATE;
    {
        SDL_AudioDeviceID dev = audio_pick_device();
        if (dev != SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK) {
            const char *nm = SDL_GetAudioDeviceName(dev);
            audio_stream = SDL_OpenAudioDeviceStream(dev, &spec, audio_cb, NULL);
            if (audio_stream) ps2_log("audio: playing on \"%s\"", nm ? nm : "?");
            else ps2_log("audio: cannot open \"%s\" (%s): the default device instead", nm ? nm : "?", SDL_GetError());
        }
    }
    if (!audio_stream) {
        audio_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_cb, NULL);
        if (audio_stream) ps2_log("audio: playing on the system default device");
    }
    if (!audio_stream) {
        ps2_log("audio: SDL_OpenAudioDeviceStream: %s", SDL_GetError());
        return;
    }
    SDL_ResumeAudioStreamDevice(audio_stream);
    wav_open();
    audio_started = 1;
    ps2_log("audio: %u Hz %u-channel s16 device open, SPU2 mixer attached",
            AUDIO_RATE, AUDIO_CHANNELS);
}

int ps2_audio_running(void) { return audio_started; }
void ps2_audio_stop(void) {
    if (!audio_started) return;
    SDL_DestroyAudioStream(audio_stream);
    audio_stream = NULL;
    audio_started = 0;
    wav_close();
}

void ps2_audio_report(void) {
    if (!audio_started && !audio_bytes) return;
    ps2_log("audio: %llu bytes drained by the device (%.1f s at %u Hz)",
            (unsigned long long)audio_bytes,
            (double)audio_bytes / (double)(AUDIO_RATE * AUDIO_CHANNELS
                                           * (int)sizeof(s16)),
            AUDIO_RATE);
}
