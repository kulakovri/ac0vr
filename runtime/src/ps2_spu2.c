#include "ps2_runtime.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>
#include "ps2_spatial.h"

#define SPU2_RAM_SIZE   (2u * 1024u * 1024u)
#define SPU2_VOICES     48u
#define SPU2_RATE       48000u

u8 *ps2_spu2_ram;

static const int adpcm_f0[5] = { 0, 60, 115,  98, 122 };
static const int adpcm_f1[5] = { 0,  0, -52, -55, -60 };

#define SPU2_LOOP_END   0x0100u
#define SPU2_LOOP_REP   0x0200u
#define SPU2_LOOP_START 0x0400u

static s16 clamp16(int v) {
    return (s16)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

void ps2_spu2_decode_block(const u8 *blk, s16 *out, int *s1, int *s2,
                           u32 *hdr) {
    u32 h = (u32)blk[0] | ((u32)blk[1] << 8);
    u32 shift = h & 0x0Fu, filt = (h >> 4) & 0x0Fu;
    int p1 = *s1, p2 = *s2, i;
    if (filt > 4u) filt = 0u;
    if (hdr) *hdr = h;
    for (i = 0; i < 28; i++) {
        int nib = (blk[2 + (i >> 1)] >> ((i & 1) ? 4 : 0)) & 0x0F;
        int s = (int)(s16)(u16)((u32)nib << 12);
        int v;
        s = (shift > 12u) ? 0 : (s >> shift);
        v = s + ((p1 * adpcm_f0[filt] + p2 * adpcm_f1[filt]) >> 6);
        out[i] = clamp16(v);
        p2 = p1;
        p1 = out[i];
    }
    *s1 = p1;
    *s2 = p2;
}

typedef enum { ADSR_OFF, ADSR_ATTACK, ADSR_DECAY, ADSR_SUSTAIN, ADSR_RELEASE }
    adsr_phase;

typedef struct {
    u32 ssa, lsax, nax;
    u32 pitch;
    u32 counter;
    s16 buf[28];
    int have;
    int idx;
    s16 previous;
    int s1, s2;
    s16 voll, volr;
    int gl, gr;             /* volume being applied, x256 (glides toward voll/volr: vol_glide) */
    u32 adsr1, adsr2;
    adsr_phase phase;
    int env;
    u32 env_clock;
    int on;
    int loop;
} spu2_voice;

static spu2_voice spu2_v[SPU2_VOICES];
static SDL_SpinLock spu2_lock;
void ps2_spu2_command_lock(void) { SDL_LockSpinlock(&spu2_lock); }
void ps2_spu2_command_unlock(void) { SDL_UnlockSpinlock(&spu2_lock); }
u8 ps2_spu2_voice_status(u32 i) {
    if(i>=48||!spu2_v[i].on) return 0;
    if(spu2_v[i].phase==ADSR_RELEASE) return spu2_v[i].env?2:0;
    return spu2_v[i].env?1:3;
}
void ps2_spu2_voice_pitch(u32 i,u32 p) { if(i<48) spu2_v[i].pitch=p&0x3fff; }
/* a snapshot of one voice for the game-side spatial code and its debug log (no lock: a torn read is harmless there) */
int ps2_spu2_voice_info(u32 i, u32 *ssa, u32 *pitch, int *voll, int *volr, int *on, int *env, u32 *lsax) {
    if (i >= 48) return 0;
    *ssa = spu2_v[i].ssa; *pitch = spu2_v[i].pitch; *voll = spu2_v[i].voll; *volr = spu2_v[i].volr;
    *on = spu2_v[i].on; *env = spu2_v[i].env; *lsax = spu2_v[i].lsax;
    return 1;
}
static u32 spu2_endx;
static u64 spu2_deposited;
static u64 spu2_frames;
static u64 spu2_sets, spu2_kons, spu2_koffs, spu2_sweeps;
static u64 spu2_frames_live;
static u32 spu2_peak_live;
static u64 spu2_adsr_stops;
static u64 spu2_end_stops;
static u64 spu2_frames_env;
static u64 spu2_frames_vol;

void ps2_spu2_init(void) {
    if (ps2_spu2_ram) return;
    ps2_spu2_ram = (u8 *)calloc(1, SPU2_RAM_SIZE);
    if (!ps2_spu2_ram) ps2_fatal("cannot allocate SPU2 sound RAM");
    memset(spu2_v, 0, sizeof spu2_v);
    spu2_endx = 0;
}

void ps2_spu2_write(u32 dst, u32 iop_src, u32 size) {
    u32 off;
    if (!ps2_spu2_ram) ps2_spu2_init();
    if (dst >= SPU2_RAM_SIZE) return;
    if (size > SPU2_RAM_SIZE - dst) size = SPU2_RAM_SIZE - dst;
    if (iop_src < PS2_IOP_RAM_BASE) return;
    off = iop_src - PS2_IOP_RAM_BASE;
    if (off >= PS2_IOP_RAM_SIZE) return;
    if (size > PS2_IOP_RAM_SIZE - off) size = PS2_IOP_RAM_SIZE - off;
    memcpy(ps2_spu2_ram + dst, ps2_iop_ram + off, size);
    spu2_deposited += size;
    if (getenv("PS2_SPU2_TRACE")) {
        static unsigned n;
        if (n < 64)
            ps2_log("spu2: xfer %2u  dst=%06X  size=%6u  (end %06X)",
                    n, dst, size, dst + size);
        n++;
    }
}

u64 ps2_spu2_bytes(void) { return spu2_deposited; }

static void adsr_step(spu2_voice *v) {
    u32 shift, subrate = 0, period;
    int down, exponential, delta;
    int target = (int)(((v->adsr1 & 15u) + 1u) * 0x800u);
    switch (v->phase) {
    case ADSR_ATTACK:
        shift = (v->adsr1 >> 10) & 31u; subrate = (v->adsr1 >> 8) & 3u;
        down = 0; exponential = (v->adsr1 >> 15) & 1u; break;
    case ADSR_DECAY:
        shift = (v->adsr1 >> 4) & 15u;
        down = exponential = 1; break;
    case ADSR_SUSTAIN:
        shift = (v->adsr2 >> 8) & 31u; subrate = (v->adsr2 >> 6) & 3u;
        down = (v->adsr2 >> 14) & 1u; exponential = (v->adsr2 >> 15) & 1u; break;
    case ADSR_RELEASE:
        shift = v->adsr2 & 31u;
        down = 1; exponential = (v->adsr2 >> 5) & 1u; break;
    default: return;
    }
    period = 1u << (shift > 11u ? shift - 11u : 0u);
    delta = down ? -8 + (int)subrate : 7 - (int)subrate;
    delta *= 1 << (shift < 11u ? 11u - shift : 0u);
    if (exponential && !down && v->env > 0x6000) period *= 4u;
    if (++v->env_clock < period) return;
    v->env_clock = 0;
    if (exponential && down) delta = (delta * v->env) >> 15;
    v->env += delta;
    if (v->env < 0) v->env = 0;
    if (v->env > 0x7FFF) v->env = 0x7FFF;
    if (v->phase == ADSR_ATTACK && v->env == 0x7FFF) v->phase = ADSR_DECAY;
    else if (v->phase == ADSR_DECAY && v->env <= target) v->phase = ADSR_SUSTAIN;
    else if (v->phase == ADSR_RELEASE && !v->env) {
        v->phase = ADSR_OFF; v->on = 0; spu2_adsr_stops++;
    }
}

void ps2_spu2_key_on(u32 i, u32 ssa, u32 lsax, u32 pitch,
                     u32 adsr1, u32 adsr2, s16 voll, s16 volr) {
    spu2_voice *v;
    if (i >= SPU2_VOICES) return;
    if (!ps2_spu2_ram) ps2_spu2_init();
    v = &spu2_v[i];
    v->ssa = ssa & ~0xFu;
    v->lsax = lsax & ~0xFu;
    v->nax = v->ssa;
    v->pitch = pitch & 0x3FFFu;
    v->counter = 0;
    v->have = 0; v->idx = 0; v->s1 = 0; v->s2 = 0;
    v->previous = 0;
    v->voll = voll; v->volr = volr;
    v->gl = voll * 256; v->gr = volr * 256;     /* no glide into a new note: the envelope's attack is its onset */
    v->adsr1 = adsr1; v->adsr2 = adsr2;
    v->phase = ADSR_ATTACK; v->env = 0; v->env_clock = 0; v->on = 1;
    spu2_endx &= ~(1u << (i & 31u));
    ps2_spatial_note_on(i);      /* the spatial filters start clean with the new note */
}

void ps2_spu2_key_off(u32 i) {
    if (i >= SPU2_VOICES) return;
    if (spu2_v[i].on && spu2_v[i].phase != ADSR_RELEASE) {
        spu2_v[i].phase = ADSR_RELEASE;
        spu2_v[i].env_clock = 0;
    }
}

static s16 spu2_vol_decode(u16 reg) {
    if (reg & 0x8000u) {
        spu2_sweeps++;
        return (reg & 0x2000u) ? 0 : 0x7FFF;
    }
    return (s16)((u16)(reg << 1));
}

void ps2_spu2_set_voice(u32 i, u32 ssa, u32 pitch, u32 adsr1, u32 adsr2,
                        u32 voll_reg, u32 volr_reg, int loop) {
    spu2_voice *v;
    if (i >= SPU2_VOICES) return;
    if (!ps2_spu2_ram) ps2_spu2_init();
    v = &spu2_v[i];
    v->ssa   = ssa & ~0xFu;
    v->pitch = pitch & 0x3FFFu;
    v->adsr1 = adsr1;
    v->adsr2 = adsr2;
    v->voll  = spu2_vol_decode((u16)voll_reg);
    v->volr  = spu2_vol_decode((u16)volr_reg);
    v->loop  = loop;
    spu2_sets++;
}

void ps2_spu2_stream_volume(u32 i, u32 l, u32 r) {
    if (i >= SPU2_VOICES) return;
    spu2_v[i].voll = spu2_vol_decode((u16)l);
    spu2_v[i].volr = spu2_vol_decode((u16)r);
}
void ps2_spu2_key_on_mask(u32 core0, u32 core1) {
    u32 b;
    for (b = 0; b < 24u; b++) {
        if (core0 & (1u << b)) {
            spu2_voice *v = &spu2_v[b];
            ps2_spu2_key_on(b, v->ssa, v->ssa, v->pitch, v->adsr1, v->adsr2,
                            v->voll, v->volr);
            spu2_kons++;
        }
        if (core1 & (1u << b)) {
            spu2_voice *v = &spu2_v[24u + b];
            ps2_spu2_key_on(24u + b, v->ssa, v->ssa, v->pitch, v->adsr1,
                            v->adsr2, v->voll, v->volr);
            spu2_kons++;
        }
    }
}

void ps2_spu2_key_off_mask(u32 core0, u32 core1) {
    u32 b;
    for (b = 0; b < 24u; b++) {
        if (core0 & (1u << b)) { ps2_spu2_key_off(b);        spu2_koffs++; }
        if (core1 & (1u << b)) { ps2_spu2_key_off(24u + b);  spu2_koffs++; }
    }
}

u32 ps2_spu2_endx(void) { return spu2_endx; }

static int voice_load(spu2_voice *v, u32 i) {
    if (!v->have) {
        u32 h;
        if (v->nax > SPU2_RAM_SIZE - 16u) {
            v->on = 0;
            spu2_end_stops++;
            return 0;
        }
        ps2_spu2_decode_block(ps2_spu2_ram + v->nax, v->buf, &v->s1, &v->s2,
                              &h);
        if (h & SPU2_LOOP_START) v->lsax = v->nax;
        v->have = 1;
        v->idx = 0;
        v->nax += 16u;
        if (h & SPU2_LOOP_END) {
            spu2_endx |= 1u << (i & 31u);
            v->nax = (h & SPU2_LOOP_REP) ? v->lsax : 0xFFFFFFFFu;
        }
    }
    return 1;
}

/* Volume changes glide instead of stepping: a one-pole filter moving 1/256 of the remaining distance per sample
   (time constant 256 samples = 5.3 ms at 48 kHz; at most 1/256 of full scale per sample). The game re-pans and
   re-attenuates 3D voices every frame, and with the head-tracked listener (zero_vr.c) every head turn changes the
   pan; instant steps were audible as zipper noise. Settled, the result is bit-identical to the plain product.
   PS2_AUDIO_GLIDE=0 (test switch) applies volume changes at once, as before. */
static int spu2_glide = -1;
static void vol_glide(int *g, int target) {
    int d = target * 256 - *g;
    if ((d > 255 || d < -255) && spu2_glide) *g += d / 256;
    else *g = target * 256;
}

static void voice_advance(spu2_voice *v, u32 i, u32 pitch) {
    v->counter += pitch;
    while (v->counter >= 0x1000u) {
        v->counter -= 0x1000u;
        if (!voice_load(v, i)) break;
        v->previous = v->buf[v->idx];
        if (++v->idx >= 28) {
            v->idx = 0;
            v->have = 0;
        }
    }
    adsr_step(v);
}

static void voice_sample(spu2_voice *v, u32 i, int *l, int *r) {
    int s, e;
    if (!v->on || !voice_load(v, i)) return;
    s = v->previous + (((int)v->buf[v->idx] - v->previous)
                       * (int)v->counter >> 12);
    vol_glide(&v->gl, v->voll);
    vol_glide(&v->gr, v->volr);
    e = (s * v->env) >> 15;
    *l += (int)(((long long)e * v->gl) >> 23);
    *r += (int)(((long long)e * v->gr) >> 23);
    voice_advance(v, i, v->pitch);
}

/* The spatial path (ps2_spatial.c) for a positional voice: the same sample and volume glide as voice_sample, then
   (granular engine,) binaural rendering / cockpit muffling into the float sums, the game's plain pan into rl/rr (A/B
   reference), and the Doppler pitch multiplier on the advance. */
static void voice_sample_spatial(spu2_voice *v, u32 i, float *fl, float *fr, int *rl, int *rr) {
    int s, e;
    if (!v->on || !voice_load(v, i)) return;
    s = v->previous + (((int)v->buf[v->idx] - v->previous)
                       * (int)v->counter >> 12);
    vol_glide(&v->gl, v->voll);
    vol_glide(&v->gr, v->volr);
    e = (s * v->env) >> 15;
    *rl += (int)(((long long)e * v->gl) >> 23);      /* the A/B reference: exactly voice_sample's output */
    *rr += (int)(((long long)e * v->gr) >> 23);
    s = ps2_spatial_grain(i, s, v->ssa, v->lsax, v->pitch);
    e = (s * v->env) >> 15;
    float dm = ps2_spatial_voice(i, e, v->gl, v->gr, fl, fr);
    u32 p = v->pitch;
    if (dm != 1.0f) {
        float q = (float)p * dm + 0.5f;
        p = q >= 65535.0f ? 65535u : q <= 0.0f ? 0u : (u32)q;
    }
    voice_advance(v, i, p);
}

/* PS2_TRACE_AUDIO_VOICES=1: once a second, the RMS level each SPU2 voice contributed to the left and right output
   (in 1/32768 of full scale), to see where a positional sound sits in the stereo image */
static int spu2_trace_voices = -1;
static double spu2_tv_e[SPU2_VOICES][2];
static u32 spu2_tv_n;
static void spu2_trace_voice_flush(void) {
    char line[1600]; int n = snprintf(line, sizeof line, "spu2: voice levels L/R over %.2f s:", spu2_tv_n / (double)SPU2_RATE);
    for (u32 i = 0; i < SPU2_VOICES && n < (int)sizeof line - 32; i++) {
        double a = sqrt(spu2_tv_e[i][0] / spu2_tv_n), b = sqrt(spu2_tv_e[i][1] / spu2_tv_n);
        if (a >= 1.0 || b >= 1.0) n += snprintf(line + n, sizeof line - (size_t)n, " %u:%.0f/%.0f", i, a, b);
    }
    ps2_log("%s", line);
    memset(spu2_tv_e, 0, sizeof spu2_tv_e);
    spu2_tv_n = 0;
}

#include "ps2_nustream.h"
/* Mixer CPU time (SDL performance counter around the whole SPU2 mix incl. the spatial path), reported at exit and,
   with PS2_AUDIO_PROF=N, every N seconds of audio: milliseconds of CPU per second of audio. */
static u64 spu2_prof_ticks, spu2_prof_frames, spu2_prof_win_ticks, spu2_prof_win_frames, spu2_prof_max;
static int spu2_prof = -1;
/* ref (may be NULL): the A/B reference mix -- the same voices with the spatial stages off (the game's plain pan; the
   Doppler pitch, which changes the voice itself, is in both) -- for PS2_AUDIO_WAV_REF. PS2_AUDIO_TAP_2D=1 (test)
   leaves the positional voices out of both mixes: then the two must be bit-identical. PS2_AUDIO_SOLO=list (test, e.g.
   "2,5"): only those SPU2 voices are heard (the others still play, silently). */
void ps2_spu2_mix2(s16 *out, s16 *ref, u32 n) {
    u32 k, i;
    s16 volumes[48][2];
    u64 stream_mask = ps2_nustream_voice_mask(), sp_mask = 0;
    static int tap2d = -1;
    static u64 solo_mask = ~0ull;
    int solo = ps2_spatial_test_solo();
    if (solo_mask == ~0ull) {
        const char *e = getenv("PS2_AUDIO_SOLO");
        solo_mask = 0;
        for (const char *q = e; q && *q; ) {
            char *end; long v = strtol(q, &end, 10);
            if (end == q) break;
            if (v >= 0 && v < 48) solo_mask |= 1ull << v;
            q = *end ? end + 1 : end;
        }
    }
    if (!ps2_spu2_ram) { memset(out, 0, (size_t)n * 4u); if (ref) memset(ref, 0, (size_t)n * 4u); return; }
    if (spu2_trace_voices < 0) { const char *e = getenv("PS2_TRACE_AUDIO_VOICES"); spu2_trace_voices = e && *e == '1'; }
    if (spu2_glide < 0) { const char *e = getenv("PS2_AUDIO_GLIDE"); spu2_glide = !(e && *e == '0'); }
    if (spu2_prof < 0) { const char *e = getenv("PS2_AUDIO_PROF"); spu2_prof = e ? atoi(e) : 0; }
    if (tap2d < 0) { const char *e = getenv("PS2_AUDIO_TAP_2D"); tap2d = e && *e == '1'; }
    u64 t0 = SDL_GetPerformanceCounter();
    SDL_LockSpinlock(&spu2_lock);
    ps2_spatial_begin(n);
    for (i = 0; i < 48; i++) {
        volumes[i][0] = spu2_v[i].voll;
        volumes[i][1] = spu2_v[i].volr;
        if (ps2_spatial_voice_on(i) && !(stream_mask & (1ull << i))) sp_mask |= 1ull << i;
    }
    for (k = 0; k < n; k++) {
        int l = 0, r = 0, rl = 0, rr = 0;
        float fl = 0.0f, fr = 0.0f;
        u32 nlive = 0, nenv = 0, nvol = 0;
        ps2_spatial_tick(k, &fl, &fr, &rl, &rr);
        for (i = 0; i < SPU2_VOICES && !solo; i++) {
            spu2_voice *v = &spu2_v[i];
            if (stream_mask & (1ull << i)) continue;
            if (v->on) {
                nlive++;
                if (v->env > 0) {
                    nenv++;
                    if (v->voll || v->volr) nvol++;
                }
            }
            if (sp_mask & (1ull << i)) {
                float fl0 = fl, fr0 = fr;
                int rl0 = rl, rr0 = rr;
                voice_sample_spatial(v, i, &fl, &fr, &rl, &rr);
                if (tap2d || (solo_mask && !(solo_mask & (1ull << i)))) { fl = fl0; fr = fr0; rl = rl0; rr = rr0; }
                if (spu2_trace_voices && v->on) {
                    spu2_tv_e[i][0] += (double)(fl - fl0) * (fl - fl0);
                    spu2_tv_e[i][1] += (double)(fr - fr0) * (fr - fr0);
                }
            } else if (spu2_trace_voices && v->on) {
                int l0 = l, r0 = r;
                voice_sample(v, i, &l, &r);
                spu2_tv_e[i][0] += (double)(l - l0) * (l - l0);
                spu2_tv_e[i][1] += (double)(r - r0) * (r - r0);
            } else if (solo_mask && !(solo_mask & (1ull << i))) {
                int l0 = l, r0 = r;
                voice_sample(v, i, &l, &r);
                l = l0; r = r0;
            } else voice_sample(v, i, &l, &r);
        }
        if (spu2_trace_voices) spu2_tv_n++;
        if (nlive) {
            spu2_frames_live++;
            if (nlive > spu2_peak_live) spu2_peak_live = nlive;
        }
        if (nenv) spu2_frames_env++;
        if (nvol) spu2_frames_vol++;
        if (ref) {
            ref[k * 2 + 0] = clamp16(l + rl);
            ref[k * 2 + 1] = clamp16(r + rr);
        }
        if (sp_mask || fl != 0.0f || fr != 0.0f) {
            l += (int)lrintf(fl);
            r += (int)lrintf(fr);
        }
        out[k * 2 + 0] = clamp16(l);
        out[k * 2 + 1] = clamp16(r);
        if (tap2d && ref && (out[k * 2] != ref[k * 2] || out[k * 2 + 1] != ref[k * 2 + 1])) {
            static int said;
            if (said++ < 5)
                ps2_log("spu2: 2D tap mismatch at sample %u: out %d/%d ref %d/%d fl %.2f fr %.2f rl %d rr %d mask %llX",
                        k, out[k * 2], out[k * 2 + 1], ref[k * 2], ref[k * 2 + 1], fl, fr, rl, rr,
                        (unsigned long long)sp_mask);
        }
    }
    spu2_frames += n;
    SDL_UnlockSpinlock(&spu2_lock);
    {
        u64 dt = SDL_GetPerformanceCounter() - t0;
        spu2_prof_ticks += dt; spu2_prof_frames += n; spu2_prof_win_ticks += dt; spu2_prof_win_frames += n;
        if (dt > spu2_prof_max) spu2_prof_max = dt;
        if (spu2_prof > 0 && spu2_prof_win_frames >= (u64)spu2_prof * SPU2_RATE) {
            double f = (double)SDL_GetPerformanceFrequency();
            ps2_log("spu2: mixer CPU %.3f ms per second of audio (last %d s; longest chunk %.3f ms)",
                    spu2_prof_win_ticks / f * 1000.0 / (spu2_prof_win_frames / (double)SPU2_RATE), spu2_prof,
                    spu2_prof_max / f * 1000.0);
            spu2_prof_win_ticks = spu2_prof_win_frames = 0; spu2_prof_max = 0;
        }
    }
    if (spu2_trace_voices && spu2_tv_n >= SPU2_RATE) spu2_trace_voice_flush();   /* audio thread only */
    {   /* test switch: PS2_AUDIO_MUTE_STREAMS=1 leaves the disc streams (music, radio voice) out of the mix, so a
           PS2_AUDIO_WAV tap holds only the SPU2 voices (sound effects) and the immersion sounds */
        static int mute = -1;
        if (mute < 0) { const char *e = getenv("PS2_AUDIO_MUTE_STREAMS"); mute = e && *e == '1'; }
        if (mute || solo) return;
    }
    if (!ref) { ps2_nustream_mix(out, n, volumes); return; }
    {   /* with the reference mix the streams go to both: rendered once into a silent buffer, then added */
        s16 tmp[1024 * 2];
        for (u32 o = 0; o < n; o += 1024) {
            u32 m = n - o > 1024 ? 1024 : n - o;
            memset(tmp, 0, (size_t)m * 4u);
            ps2_nustream_mix(tmp, m, volumes);
            for (u32 j = 0; j < m * 2; j++) {
                out[o * 2 + j] = clamp16(out[o * 2 + j] + tmp[j]);
                ref[o * 2 + j] = clamp16(ref[o * 2 + j] + tmp[j]);
            }
        }
    }
}
void ps2_spu2_mix(s16 *out, u32 n) { ps2_spu2_mix2(out, NULL, n); }

void ps2_spu2_report(void) {
    u32 i, live = 0;
    for (i = 0; i < SPU2_VOICES; i++) if (spu2_v[i].on) live++;
    if (spu2_prof_frames) {
        double f = (double)SDL_GetPerformanceFrequency();
        ps2_log("spu2: mixer CPU %.3f ms per second of audio over %.1f s", spu2_prof_ticks / f * 1000.0
                / (spu2_prof_frames / (double)SPU2_RATE), spu2_prof_frames / (double)SPU2_RATE);
    }
    ps2_spatial_report();
    ps2_log("spu2: %llu bytes into sound RAM, %llu frames mixed, %u voices live",
            (unsigned long long)spu2_deposited,
            (unsigned long long)spu2_frames, live);
    ps2_log("spu2: %llu tone-attribute sets, %llu key-ons, %llu key-offs",
            (unsigned long long)spu2_sets, (unsigned long long)spu2_kons,
            (unsigned long long)spu2_koffs);
    if (spu2_sweeps)
        ps2_log("spu2: %llu volume registers were in sweep mode and were taken "
                "at a steady magnitude -- the ramp is milestone M4",
                (unsigned long long)spu2_sweeps);
    if (spu2_frames)
        ps2_log("spu2: a voice was sounding for %llu of %llu mixed frames "
                "(%.2f%%), at most %u at once",
                (unsigned long long)spu2_frames_live,
                (unsigned long long)spu2_frames,
                100.0 * (double)spu2_frames_live / (double)spu2_frames,
                spu2_peak_live);
    if (spu2_frames_live)
        ps2_log("spu2:   of those, %llu had a non-zero envelope and %llu also "
                "a non-zero volume",
                (unsigned long long)spu2_frames_env,
                (unsigned long long)spu2_frames_vol);
    if (spu2_adsr_stops || spu2_end_stops)
        ps2_log("spu2: voices stopped -- %llu by the envelope releasing to "
                "zero, %llu by running off a non-repeating waveform",
                (unsigned long long)spu2_adsr_stops,
                (unsigned long long)spu2_end_stops);
    if (spu2_kons && spu2_frames &&
        (double)spu2_frames_live / (double)spu2_frames < 0.10) {
        ps2_log("spu2: %llu key-ons produced only %.2f%% sounding frames",
                (unsigned long long)spu2_kons,
                100.0 * (double)spu2_frames_live / (double)spu2_frames);
        if (spu2_end_stops > spu2_adsr_stops)
            ps2_log("spu2:   mostly end-of-waveform -- look at looping first. "
                    "The tone-attribute record carries a loop flag at +28 "
                    "which this file stores but does not yet act on (open "
                    "question 4 in docs/AUDIO_PLAN.md)");
        else
            ps2_log("spu2:   mostly envelope releases");
    }
}

static int spu2_fail;

static void spu2_expect(const char *what, int got, int want) {
    if (got != want) {
        spu2_fail++;
        ps2_log("  FAIL %-46s got %d want %d", what, got, want);
    }
}

int ps2_spu2_selftest(void) {
    u8 blk[16];
    s16 out[28];
    int s1, s2, i;
    u32 h;

    ps2_spu2_init();
    spu2_fail = 0;
    ps2_log("---- SPU2 self-test (SPU2_Overview_Manual.pdf p.15-16, p.20) ----");
    {
        spu2_voice v = {0};
        int l = 0, r = 0;
        memset(ps2_spu2_ram, 0, 64);
        memset(ps2_spu2_ram + 2, 0x11, 14);
        memset(ps2_spu2_ram + 18, 0x22, 14);
        v.on = 1; v.pitch = 0x800; v.env = 32767;
        v.voll = spu2_vol_decode(0x2000); v.volr = spu2_vol_decode(0x6000);
        v.gl = v.voll * 256; v.gr = v.volr * 256;    /* settled volume (no glide) */
        voice_sample(&v, 0, &l, &r);
        spu2_expect("interpolation starts at zero history", l, 0);
        voice_sample(&v, 0, &l, &r);
        spu2_expect("half-sample interpolates instead of repeating", l, 1023);
        spu2_expect("rear phase survives in opposite channel", r, -1024);
        v = (spu2_voice){0}; v.on = 1; v.pitch = 0x3800;
        for (int k = 0; k < 10; k++) voice_sample(&v, 0, &l, &r);
        spu2_expect("high pitch consumes all steps across block", v.idx, 7);
        spu2_expect("high pitch retains only fractional phase", v.counter, 0);
        spu2_expect("high pitch loads next ADPCM block", v.nax, 32);
        spu2_expect("interpolation history crosses block", v.previous, 8192);
        memset(ps2_spu2_ram, 0, 64);
    }
    {   /* volume glide: a full-scale step reaches 63% in 256 samples (5.3 ms), at most 1/256 of full scale moves
           per sample, a sign flip glides too, and the gain settles exactly on the target */
        int g = 0, prev = 0, jump = 0, at240 = 0, at256 = 0;
        for (int k = 1; k <= 4000; k++) {
            vol_glide(&g, 32767);
            if (abs(g / 256 - prev) > jump) jump = abs(g / 256 - prev);
            prev = g / 256;
            if (k == 240) at240 = g / 256;
            if (k == 256) at256 = g / 256;
        }
        spu2_expect("glide reaches 60% of a step in 5 ms", at240 >= 19660, 1);
        spu2_expect("glide reaches 63% of a step in 256 samples", at256 >= 20643, 1);
        spu2_expect("glide moves at most 1/256 of full scale", jump <= 128, 1);
        spu2_expect("glide settles on the target", g, 32767 * 256);
        g = -32768 * 256;
        vol_glide(&g, 32767);
        spu2_expect("sign flip glides", g / 256 < -32000, 1);
    }
    {
        spu2_voice v = {0};
        v.phase = ADSR_ATTACK; v.adsr1 = 0x000f;
        adsr_step(&v); spu2_expect("fast attack first step", v.env, 14336);
        adsr_step(&v); adsr_step(&v);
        spu2_expect("attack reaches peak", v.env, 32767);
        adsr_step(&v);
        spu2_expect("decay reaches sustain", v.phase, ADSR_SUSTAIN);
        v.env = 16000; v.adsr2 = 0x0000;
        adsr_step(&v);
        spu2_expect("linear increasing sustain", v.env, 30336);
        v.env = 16000; v.adsr2 = 0x4c00; v.env_clock = 0;
        adsr_step(&v); spu2_expect("slow sustain waits for rate", v.env, 16000);
        adsr_step(&v); spu2_expect("decreasing sustain step", v.env, 15992);
        v.env = 16000; v.adsr2 = 0xc000; v.env_clock = 0;
        adsr_step(&v); spu2_expect("exponential sustain", v.env, 8000);
    }

    memset(blk, 0, sizeof blk);
    blk[0] = 0x00;
    blk[1] = 0x00;
    for (i = 0; i < 14; i++) blk[2 + i] = 0x21;
    s1 = s2 = 0;
    ps2_spu2_decode_block(blk, out, &s1, &s2, &h);
    spu2_expect("filter 0 shift 0, nibble 1", out[0], 0x1000);
    spu2_expect("filter 0 shift 0, nibble 2", out[1], 0x2000);
    spu2_expect("header decoded", (int)h, 0);

    blk[0] = 0x04;
    s1 = s2 = 0;
    ps2_spu2_decode_block(blk, out, &s1, &s2, &h);
    spu2_expect("shift 4 scales the sample down", out[0], 0x0100);

    memset(blk + 2, 0xFF, 14);
    blk[0] = 0x00;
    s1 = s2 = 0;
    ps2_spu2_decode_block(blk, out, &s1, &s2, &h);
    spu2_expect("nibble F is negative", out[0], -0x1000);

    memset(blk + 2, 0, 14);
    blk[0] = 0x10;
    s1 = 0x1000; s2 = 0;
    ps2_spu2_decode_block(blk, out, &s1, &s2, &h);
    spu2_expect("filter 1 carries 60/64 of the previous sample",
                out[0], (0x1000 * 60) >> 6);

    blk[0] = 0x00; blk[1] = 0x07;
    ps2_spu2_decode_block(blk, out, &s1, &s2, &h);
    spu2_expect("LOOP/END   is bit 8",  (h & SPU2_LOOP_END)   ? 1 : 0, 1);
    spu2_expect("LOOP       is bit 9",  (h & SPU2_LOOP_REP)   ? 1 : 0, 1);
    spu2_expect("LOOP/START is bit 10", (h & SPU2_LOOP_START) ? 1 : 0, 1);

    {
        s16 mix[256 * 2];
        int nonzero = 0;
        memset(ps2_spu2_ram, 0, 4096);
        ps2_spu2_ram[0] = 0x00;
        ps2_spu2_ram[1] = 0x03;
        for (i = 0; i < 14; i++) ps2_spu2_ram[2 + i] = 0x71;
        ps2_spu2_key_on(0, 0, 0, 0x1000, 0x000F, 0x0000, 0x3FFF, 0x3FFF);
        ps2_spu2_mix(mix, 256);
        for (i = 0; i < 256 * 2; i++) if (mix[i]) nonzero++;
        spu2_expect("a keyed-on voice produces sound", nonzero > 0, 1);
        spu2_expect("ENDX latches at the endpoint block",
                    (ps2_spu2_endx() & 1u) ? 1 : 0, 1);
        ps2_spu2_key_off(0);
        memset(ps2_spu2_ram, 0, 4096);
    }

    {
        s16 mix[64 * 2];
        memset(ps2_spu2_ram, 0, 4096);
        ps2_spu2_ram[1] = 0x03;
        for (i = 0; i < 14; i++) ps2_spu2_ram[2 + i] = 0x71;
        ps2_spu2_set_voice(0, 0, 0, 0x000F, 0x0000, 0x3FFF, 0x3FFF, 1);
        spu2_expect("tone attribute keeps pitch 0", (int)spu2_v[0].pitch, 0);
        ps2_spu2_key_on_mask(1u, 0u);
        spu2_expect("key-on keeps pitch 0", (int)spu2_v[0].pitch, 0);
        ps2_spu2_mix(mix, 64);
        spu2_expect("pitch 0 holds the sample position", spu2_v[0].idx, 0);
        ps2_spu2_set_voice(0, 0, 0x1000, 0x000F, 0x0000, 0x3FFF, 0x3FFF, 1);
        ps2_spu2_mix(mix, 10);
        spu2_expect("restored pitch resumes from the held position",
                    spu2_v[0].idx, 10);
        ps2_spu2_key_off(0);
        memset(ps2_spu2_ram, 0, 4096);
    }

    {   /* a volume step on a sounding voice (constant waveform, looping) glides through the mixer: no output jump
           above 1/256 of the level per sample, 99.9% of the way after 2048 samples (43 ms), exact by 3072 */
        s16 mix[2048 * 2];
        int jump = 0, glide_was = spu2_glide;
        memset(ps2_spu2_ram, 0, 4096);
        ps2_spu2_ram[1] = 0x03;                          /* loop end + repeat, back to its own start */
        for (i = 0; i < 14; i++) ps2_spu2_ram[2 + i] = 0x77;
        spu2_glide = 1;
        ps2_spu2_key_on(0, 0, 0, 0x1000, 0x000F, 0x0000, spu2_vol_decode(0x3FFF), spu2_vol_decode(0x3FFF));
        ps2_spu2_mix(mix, 256);
        int before = mix[255 * 2];
        ps2_spu2_set_voice(0, 0, 0x1000, 0x000F, 0x0000, 0, 0x3FFF, 1);    /* left 0, right unchanged */
        ps2_spu2_mix(mix, 2048);
        for (int k = 0, prev = before; k < 2048; prev = mix[k * 2], k++)    /* from the last sample before the step */
            if (abs(mix[k * 2] - prev) > jump) jump = abs(mix[k * 2] - prev);
        spu2_expect("sounding voice before the step", before > 20000, 1);
        spu2_expect("volume step glides (largest jump <= 1/256)", jump <= before / 256 + 2, 1);
        spu2_expect("volume step is 99.9% done in 43 ms", mix[2047 * 2] <= before / 1000, 1);
        spu2_expect("the other side keeps its level", mix[2047 * 2 + 1] == before, 1);
        ps2_spu2_mix(mix, 1024);
        spu2_expect("volume step settles exactly", mix[1023 * 2], 0);
        ps2_spu2_key_off(0);
        spu2_v[0].on = 0;
        spu2_glide = glide_was;
        memset(ps2_spu2_ram, 0, 4096);
    }
    {
        s16 mix[8];
        const u32 bad[] = {0xffffffffu, 0xfffffff0u, SPU2_RAM_SIZE};
        for (u32 j = 0; j < sizeof bad / sizeof bad[0]; j++) {
            ps2_spu2_key_on(47, bad[j], 0, 0x1000, 15, 0, 0x3fff, 0x3fff);
            ps2_spu2_mix(mix, 4);
            spu2_expect("invalid waveform address mutes safely", spu2_v[47].on, 0);
        }
    }
    extern int ps2_nustream_selftest(void);
    spu2_fail += ps2_nustream_selftest();
    spu2_fail += ps2_spatial_selftest();
    ps2_log("spu2 selftest: %s", spu2_fail ? "FAILED" : "all checks passed");
    return spu2_fail ? 1 : 0;
}
