// zero_audio3d.c - Ace Combat Zero's positional (3D) sound sources for the spatial mixer (ps2_spatial.c, bug #29).
//
// The game's 3D sound code (sound library, static RE 2026-10-21): once per frame the panner 0x2DD138 walks the 48
// sound channels; for every channel that is positional (channel record 0x423420 + 0x6C*ch: active +1, 3D flag +0x26,
// set by 0x2E00D8 and cleared by 0x2E0138) it takes the source's position / velocity / direction (0x422C48 + 0x24*ch,
// three vec3) and the listener (0x422C18 position, +0x0C velocity, +0x18 front, +0x24 up; set through 0x2DCE30,
// which zero_vr.c turns with the head) and computes in 0x2DD688: distance attenuation and cone gain (f32
// 0x424860 + 16*ch), the azimuth in the horizontal plane of the listener (s16 +4, 127 = 180 deg, + = right; the
// elevation is ignored), a Doppler factor (f32 +8: 1 + v_closing / 522 with v in units per frame, clamped to
// [1/3, 3]: an effective speed of sound ~6 km/s, 1/18 of the real one) and a reverb/filter byte (+12). 0x2E0198 then
// sets the voice: pitch = base * (1 + k * (doppler - 1)) with k the channel's Doppler weight (f32 +0x10: 1 for
// engines, 0.05-0.2 for some own-aircraft layers), pan = azimuth + the sound's own pan offset (s16 +0x2C, 64 =
// straight ahead; one own-aircraft layer sits at +128 = mirrored), and L/R through a constant-power sine pan law
// (table 0x3BF968; rear half phase-inverted on the left). Channel ch drives SPU2 voice ch (verified: pitch and
// volumes match the HLE tone-attribute records). Units: 5.12 per metre (the sound config's 3D_DISTANCE_RANGE).
//
// This hook (after 0x2DD138, once per field) gives the mixer per voice: the direction of the source in head axes
// (from the positions: elevation included; the sound's pan offset applied as a turn about the up axis so the
// designers' layout stays), the distance, whether the source is the own aircraft (it moves with the listener and is
// close: the engine, gun, the mirrored layer), the Doppler multiplier, the cockpit-view flag and the engine state.
// Doppler: velocities from the positions' history (the game leaves the own engine's velocity at 0, so its own factor
// pitches the own engine down by 3.5 % at 330 kt -- kept, that is the 2006 sound), f = (c' + v_listener.u) /
// (c' + v_source.u) with c' = 343 m/s / ACZ_AUDIO_DOPPLER (0.25: a jet passing head-on swings about +-5.5 semitones),
// clamped to [0.5, 2], weighted by the channel's k, divided by the game's own factor (not applied twice); own-
// aircraft sources keep the game's pitch.
// ACZ_DEBUG_AUDIO3D=N logs every N fields; ACZ_AUDIO3D_TEST places a synthetic source (see zero_audio3d_install).
#include "ps2_runtime.h"
#include "ps2_hook.h"
#include "ps2_spatial.h"
#include "zero_instr.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

u64 ps2_kernel_vblank_count(void);
int zero_vr_is_zero(void);
int ps2_spu2_voice_info(u32 i, u32 *ssa, u32 *pitch, int *voll, int *volr, int *on, int *env, u32 *lsax);

#define ZA_PANNER   0x002DD138u     /* per frame: 3D parameters of every positional channel */
#define ZA_CHAN     0x00423420u     /* channel records, 0x6C bytes */
#define ZA_SRC      0x00422C48u     /* per channel: position, velocity, direction (0x24 bytes) */
#define ZA_LSN      0x00422C18u     /* listener: position, velocity, front, up */
#define ZA_OUT      0x00424860u     /* per channel: gain f32, azimuth s16 (+4), Doppler f32 (+8), byte (+12) */
#define ZA_UPM      5.12f           /* game units per metre (3D_DISTANCE_RANGE) */
#define ZA_FPS      60.0f
#define ZA_VJUMP    1200.0f         /* m/s: faster than any aircraft or missile -- a cut or a new object, no velocity */
#define ZA_OWN_R    80.0f           /* units (15.6 m): sources this close are the own aircraft (engine, gun, layers) */

static float za_f(u32 a) { u32 w = ps2_r32(a); float f; memcpy(&f, &w, 4); return f; }
static int za_dbg, za_on;
static u64 za_frames, za_pub_3d, za_own, za_teleports;
static float za_dmax, za_dmin = 9.0f;

static struct { float S[3], vs[3]; int valid; } za_h[PS2_SPAT_VOICES + 1];
static float za_L[3], za_vl[3];
static int za_lvalid;
static u64 za_last_field;

/* the synthetic test source: ACZ_AUDIO3D_TEST="az,el[,dist_m]" (degrees, + = right / up; a fixed direction in the
   aircraft's axes from the listener: only the head moves it) or "flyby[,speed_mps[,miss_m[,period_s]]]" (a source
   passing the listener from ahead to behind on its right at that relative speed and miss distance, every period).
   ACZ_AUDIO3D_TEST_SIG=tone: a 220 Hz harmonic tone instead of noise bursts (pitch measurable). */
static int za_test;           /* 0 none, 1 fixed, 2 flyby */
static float za_t_az, za_t_el, za_t_dist = 60.0f, za_t_v = 300.0f, za_t_miss = 40.0f, za_t_per = 8.0f;

static void za_cross(const float *a, const float *b, float *o) {
    o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0];
}
static float za_dot(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

/* geometry, ownership and Doppler of one source; h = its history slot; k_game / dp_game: the game's own Doppler */
static void za_source(int idx, const float *S, const float *F, const float *U, const float *R, float off_units,
                      float k_game, float dp_game, u64 dt, ps2_spat_src *o, float *own_rv) {
    float d[3] = { S[0] - za_L[0], S[1] - za_L[1], S[2] - za_L[2] };
    float dist = sqrtf(za_dot(d, d));
    float x = za_dot(d, R), y = za_dot(d, U), z = za_dot(d, F);
    float n = sqrtf(x * x + y * y + z * z);
    if (n > 1e-3f) { x /= n; y /= n; z /= n; } else { x = 0.0f; y = 0.0f; z = 1.0f; }
    if (off_units > 2.5f || off_units < -2.5f) {            /* the sound's own pan offset: a turn about the up axis */
        float a = off_units * (3.14159265f / 127.0f), c = cosf(a), s = sinf(a);
        float x2 = x * c + z * s, z2 = z * c - x * s;
        x = x2; z = z2;
    }
    o->dir[0] = x; o->dir[1] = y; o->dir[2] = z;
    o->dist_m = dist / ZA_UPM;
    o->is3d = 1;
    /* source velocity from its position history (units per field), smoothed; a jump beyond ZA_VJUMP = a new object */
    float vs[3] = { 0, 0, 0 };
    if (za_h[idx].valid && dt > 0u && dt < 8u) {
        float q = 1.0f / (float)dt, raw[3];
        for (int j = 0; j < 3; j++) raw[j] = (S[j] - za_h[idx].S[j]) * q;
        if (sqrtf(za_dot(raw, raw)) * ZA_FPS / ZA_UPM > ZA_VJUMP) { za_h[idx].valid = 0; za_teleports++; }
        else for (int j = 0; j < 3; j++) vs[j] = za_h[idx].vs[j] = 0.5f * za_h[idx].vs[j] + 0.5f * raw[j];
    }
    int hv = za_h[idx].valid && za_lvalid;
    if (!za_h[idx].valid) { za_h[idx].vs[0] = za_h[idx].vs[1] = za_h[idx].vs[2] = 0.0f; }
    memcpy(za_h[idx].S, S, sizeof za_h[idx].S);
    za_h[idx].valid = 1;
    float rel[3] = { vs[0] - za_vl[0], vs[1] - za_vl[1], vs[2] - za_vl[2] };
    float vrel = sqrtf(za_dot(rel, rel)) * ZA_FPS / ZA_UPM;   /* m/s */
    *own_rv = vrel;
    /* own aircraft (not muffled): within ~16 m of the listener -- the engine sources sit 22 units behind the cockpit
       eye; muffling applies only in the cockpit view. Co-moving (within 30..45 m/s of the listener, e.g. the own
       aircraft in the chase view, a wingman in formation): no Doppler of ours, the game's pitch is kept. */
    float cm_w = 0.0f;
    if (hv) cm_w = vrel < 30.0f ? 1.0f : vrel < 45.0f ? (45.0f - vrel) / 15.0f : 0.0f;
    o->own = dist < ZA_OWN_R;
    if (o->own) cm_w = 1.0f;
    float mul = 1.0f, str = ps2_spatial_doppler();
    if (hv && str > 0.0f && dist > 1.0f) {
        float u[3] = { d[0] / dist, d[1] / dist, d[2] / dist };          /* listener -> source */
        float cs = 343.0f / str * ZA_UPM / ZA_FPS;                         /* scaled speed of sound, units/field */
        float num = cs + za_dot(za_vl, u), den = cs + za_dot(vs, u);
        float f = den > cs * 0.05f ? num / den : 2.0f;
        f = f < 0.5f ? 0.5f : f > 2.0f ? 2.0f : f;
        float k = k_game < 0.0f ? 0.0f : k_game > 1.0f ? 1.0f : k_game;
        float feff = 1.0f + k * (f - 1.0f), g = 1.0f + k * (dp_game - 1.0f);
        if (!(g > 0.2f && g < 5.0f)) g = 1.0f;
        mul = feff / g;
        mul = mul + (1.0f - mul) * cm_w;                                   /* co-moving: the game's pitch is kept */
    }
    o->dopp = mul;
}

static int za_panner_leave(ps2_ctx *ctx, void *user) {
    (void)ctx; (void)user;
    u64 f = ps2_kernel_vblank_count();
    if (f == za_last_field) return 0;                       /* once per field */
    u64 dt = za_last_field ? f - za_last_field : 0u;
    za_last_field = f;
    za_frames++;
    float Lw[12];
    for (int i = 0; i < 12; i++) Lw[i] = za_f(ZA_LSN + 4u * (u32)i);
    float *P = Lw, *F = Lw + 6, *U = Lw + 9, R[3];
    za_cross(F, U, R);                                      /* right (verified against the game's azimuth sign) */
    {   float n = sqrtf(za_dot(R, R)); if (n > 1e-6f) for (int j = 0; j < 3; j++) R[j] /= n; }
    if (za_lvalid && dt > 0u && dt < 8u) {
        float raw[3];
        for (int j = 0; j < 3; j++) raw[j] = (P[j] - za_L[j]) / (float)dt;
        if (sqrtf(za_dot(raw, raw)) * ZA_FPS / ZA_UPM > ZA_VJUMP) za_lvalid = 0;     /* camera cut */
        else for (int j = 0; j < 3; j++) za_vl[j] = 0.5f * za_vl[j] + 0.5f * raw[j];
    }
    if (!za_lvalid) { za_vl[0] = za_vl[1] = za_vl[2] = 0.0f; for (int i = 0; i <= PS2_SPAT_VOICES; i++) za_h[i].valid = 0; }
    memcpy(za_L, P, sizeof za_L);
    int lv_was = za_lvalid;
    za_lvalid = 1;
    static ps2_spat_frame fr;
    memset(&fr, 0, sizeof fr);
    fr.field = f;
    {   extern volatile int ps2_vk_cockpit_view;
        fr.cockpit = ps2_vk_cockpit_view ? 1.0f : 0.0f; }
    float rv[PS2_SPAT_VOICES + 1] = { 0 };
    for (u32 ch = 0; ch < PS2_SPAT_VOICES; ch++) {
        u32 c = ZA_CHAN + 0x6Cu * ch;
        if (!ps2_r8(c + 1u) || !ps2_r8(c + 0x26u)) { za_h[ch].valid = 0; continue; }
        float S[3];
        for (int j = 0; j < 3; j++) S[j] = za_f(ZA_SRC + 0x24u * ch + 4u * (u32)j);
        if (!(fabsf(S[0]) < 1e7f && fabsf(S[1]) < 1e7f && fabsf(S[2]) < 1e7f)) continue;
        int az = (s16)ps2_r16(ZA_OUT + 16u * ch + 4u), pan = (s16)ps2_r16(c + 0x2Cu);
        int off = (pan - 64 - az) % 255;
        if (off > 127) off -= 255; else if (off < -127) off += 255;
        za_source((int)ch, S, F, U, R, (float)off, za_f(c + 0x10u), za_f(ZA_OUT + 16u * ch + 8u), lv_was ? dt : 0u,
                  &fr.v[ch], &rv[ch]);
        za_pub_3d++;
        if (fr.v[ch].own) za_own++;
        if (fr.v[ch].dopp > za_dmax) za_dmax = fr.v[ch].dopp;
        if (fr.v[ch].dopp < za_dmin) za_dmin = fr.v[ch].dopp;
    }
    if (za_test) {
        /* placed in the aircraft's axes (the flight model's heading / pitch / roll; the game's euler convention
           R = Ry(-heading) Rx(pitch) Rz(roll)), so only the head moves it */
        float S[3], t = 0.0f, ax[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, -1 } };     /* right, up, front */
        zero_instr in;
        if (zero_instr_get(&in) && in.valid) {
            float y = -in.heading_deg * 0.01745329f, p = in.pitch_deg * 0.01745329f, r = in.roll_deg * 0.01745329f;
            float cy = cosf(y), sy = sinf(y), cp = cosf(p), sp = sinf(p), cr = cosf(r), sr = sinf(r);
            float rt[3] = { cy * cr + sy * sp * sr, cp * sr, -sy * cr + cy * sp * sr };
            float up[3] = { -sr * cy + cr * sp * sy, cp * cr, sr * sy + cr * sp * cy };
            float fw[3] = { -sy * cp, sp, -cy * cp };
            memcpy(ax[0], rt, sizeof rt); memcpy(ax[1], up, sizeof up); memcpy(ax[2], fw, sizeof fw);
        }
        float rel[3];
        if (za_test == 1) {
            float a = za_t_az * 0.01745329f, e = za_t_el * 0.01745329f, D = za_t_dist * ZA_UPM;
            rel[0] = D * sinf(a) * cosf(e); rel[1] = D * sinf(e); rel[2] = D * cosf(a) * cosf(e);
        } else {
            t = fmodf((float)f / ZA_FPS, za_t_per) - 0.5f * za_t_per;       /* closest approach at t = 0 */
            rel[0] = za_t_miss * ZA_UPM; rel[1] = 0.0f; rel[2] = -za_t_v * t * ZA_UPM;
            if (fabsf(t) > 0.5f * za_t_per - 2.0f / ZA_FPS) za_h[PS2_SPAT_TEST].valid = 0;   /* the wrap is a jump */
        }
        for (int j = 0; j < 3; j++) S[j] = P[j] + rel[0] * ax[0][j] + rel[1] * ax[1][j] + rel[2] * ax[2][j];
        /* the game's own Doppler for it, as 0x2DD688 computes it for a real voice (1 + closing speed / 522 units per
           frame, clamped to [1/3, 3]): the test voice has no game pitch, so that factor is applied here -- with
           ACZ_AUDIO_DOPPLER=0 the test source sounds as a game voice would */
        float gdp = 1.0f;
        if (za_h[PS2_SPAT_TEST].valid && lv_was && dt > 0u && dt < 8u) {
            float d[3] = { S[0] - P[0], S[1] - P[1], S[2] - P[2] }, n = sqrtf(za_dot(d, d));
            float vs[3];
            for (int j = 0; j < 3; j++) vs[j] = (S[j] - za_h[PS2_SPAT_TEST].S[j]) / (float)dt - za_vl[j];
            if (n > 1.0f) gdp = 1.0f - za_dot(vs, d) / n / 522.24f;
            gdp = gdp < 0.3333f ? 0.3333f : gdp > 3.0f ? 3.0f : gdp;
        }
        za_source(PS2_SPAT_TEST, S, F, U, R, 0.0f, 1.0f, gdp, lv_was ? dt : 0u, &fr.v[PS2_SPAT_TEST], &rv[PS2_SPAT_TEST]);
        fr.v[PS2_SPAT_TEST].dopp *= gdp;
        fr.v[PS2_SPAT_TEST].own = 0;
        float dm = fr.v[PS2_SPAT_TEST].dist_m;
        fr.v[PS2_SPAT_TEST].level = 0.25f * (dm > 20.0f ? 20.0f / dm : 1.0f);
        if (za_dbg > 0 && f % (u64)za_dbg == 0u)
            ps2_log("audio3d: test field %llu t %.2f dist %.1f m dir %.2f %.2f %.2f dopp %.3f level %.3f", (unsigned long long)f, t,
                    dm, fr.v[PS2_SPAT_TEST].dir[0], fr.v[PS2_SPAT_TEST].dir[1], fr.v[PS2_SPAT_TEST].dir[2],
                    fr.v[PS2_SPAT_TEST].dopp, fr.v[PS2_SPAT_TEST].level);
    }
    {   /* the player's engine (granular engine): own positional voices that have kept sounding the same waveform for
           2 s or more -- the engine layers loop for the whole flight; gun, missile launches and other one-shots of the
           own aircraft do not last (and non-looping waveforms are left alone by the mixer anyway). Oldest first. */
        static u32 eng_ssa[PS2_SPAT_VOICES], eng_age[PS2_SPAT_VOICES];
        zero_instr in;
        if (zero_instr_get(&in) && in.valid) {
            fr.eng.throttle = in.throttle; fr.eng.afterburner = in.afterburner; fr.eng.speed_kt = in.speed_kt;
        }
        u32 age[4] = { 0 };
        for (u32 ch = 0; ch < PS2_SPAT_VOICES; ch++) {
            u32 ssa, pitch, lsax; int vl, vr, on, env;
            ps2_spu2_voice_info(ch, &ssa, &pitch, &vl, &vr, &on, &env, &lsax);
            if (!fr.v[ch].is3d || !fr.v[ch].own || !on || ssa != eng_ssa[ch]) { eng_age[ch] = 0; eng_ssa[ch] = ssa; continue; }
            if (eng_age[ch] < 1000000u) eng_age[ch] += (u32)(dt ? dt : 1u);
            if (eng_age[ch] < 120u) continue;
            int at = fr.eng.n < 4 ? fr.eng.n : 3;
            if (fr.eng.n >= 4 && age[3] >= eng_age[ch]) continue;
            while (at > 0 && age[at - 1] < eng_age[ch]) { age[at] = age[at - 1]; fr.eng.voice[at] = fr.eng.voice[at - 1]; at--; }
            age[at] = eng_age[ch]; fr.eng.voice[at] = (s8)ch;
            if (fr.eng.n < 4) fr.eng.n++;
        }
        for (int j = fr.eng.n; j < 4; j++) fr.eng.voice[j] = -1;
    }
    {   /* the game's warning and lock tones at the instrument panel (cockpit system sounds, zero_syssnd.c) */
        extern void zero_syssnd_place(ps2_spat_frame *fr);
        zero_syssnd_place(&fr);
    }
    ps2_spatial_publish(&fr);
    if (za_dbg > 0 && f % (u64)za_dbg == 0u) {
        char ln[2000]; int n = snprintf(ln, sizeof ln, "audio3d: field %llu cockpit %.0f lsn v %.0f m/s eng thr %.2f ab %.2f %.0f kt |",
                                        (unsigned long long)f, fr.cockpit, sqrtf(za_dot(za_vl, za_vl)) * ZA_FPS / ZA_UPM,
                                        fr.eng.throttle, fr.eng.afterburner, fr.eng.speed_kt);
        for (u32 ch = 0; ch < PS2_SPAT_VOICES && n < (int)sizeof ln - 120; ch++) {
            if (!fr.v[ch].is3d) continue;
            u32 c = ZA_CHAN + 0x6Cu * ch, ssa, pitch, lsax; int vl, vr, on, env;
            ps2_spu2_voice_info(ch, &ssa, &pitch, &vl, &vr, &on, &env, &lsax);
            const ps2_spat_src *s = &fr.v[ch];
            n += snprintf(ln + n, sizeof ln - (size_t)n, " [%u%s %.0fm az%.0f el%.0f game%.0f vrel%.0f k%.2f dp%.3f mul%.3f p%04X v%d/%d e%d ssa%X]",
                          ch, s->own ? " own" : "", s->dist_m, atan2f(s->dir[0], s->dir[2]) * 57.29578f,
                          asinf(s->dir[1] > 1.0f ? 1.0f : s->dir[1] < -1.0f ? -1.0f : s->dir[1]) * 57.29578f,
                          (s16)ps2_r16(ZA_OUT + 16u * ch + 4u) * (180.0f / 127.0f), rv[ch], za_f(c + 0x10u),
                          za_f(ZA_OUT + 16u * ch + 8u), s->dopp, pitch, vl, vr, env, ssa);
        }
        ps2_log("%s", ln);
    }
    return 0;
}

void zero_audio3d_install(void) {
    if (!zero_vr_is_zero()) return;
    const char *e = getenv("ACZ_DEBUG_AUDIO3D");
    za_dbg = e ? atoi(e) : 0;
    e = getenv("ACZ_AUDIO3D_TEST");
    if (e && *e) {
        if (!strncmp(e, "flyby", 5)) {
            za_test = 2;
            sscanf(e + 5, ",%f,%f,%f", &za_t_v, &za_t_miss, &za_t_per);
            if (za_t_per < 2.0f) za_t_per = 2.0f;
        } else if (sscanf(e, "%f,%f,%f", &za_t_az, &za_t_el, &za_t_dist) >= 2) za_test = 1;
    }
    if (ps2_r32(ZA_PANNER) != 0x27BDFF60u || ps2_r32(ZA_PANNER + 4u) != 0x3C0E0042u
        || ps2_r32(0x002E00D8u) != 0x00052C00u || ps2_r32(0x002E0198u) != 0x27BDFFC0u) {
        ps2_log("audio3d: sound code mismatch -- spatial audio off");
        return;
    }
    if (ps2_hook_after(ZA_PANNER, za_panner_leave, NULL, 60, "zero-audio3d") < 0) {
        ps2_log("audio3d: panner hook refused -- spatial audio off");
        return;
    }
    za_on = 1;
    ps2_log("audio3d: positional sound sources from the panner 0x2DD138: binaural %s, Doppler %.2f, cockpit muffling "
            "%.2f, granular engine %.2f%s", ps2_spatial_hrtf_on() ? "on" : "off", ps2_spatial_doppler(),
            ps2_spatial_muffle(), ps2_spatial_engine_grain(),
            za_test == 1 ? " (test source fixed)" : za_test == 2 ? " (test source flyby)" : "");
}

void zero_audio3d_report(void) {
    if (!za_on || !za_frames) return;
    ps2_log("audio3d: %llu frames, %llu positional voice-frames (%llu own aircraft), Doppler multiplier %.3f..%.3f, "
            "%llu source jumps", (unsigned long long)za_frames, (unsigned long long)za_pub_3d,
            (unsigned long long)za_own, za_dmin, za_dmax, (unsigned long long)za_teleports);
}
