#include "ps2_nustream.h"
#include "ps2_hle.h"
#include "ps2_vfs.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>

void ps2_spu2_decode_block(const u8 *, s16 *, int *, int *, u32 *);
typedef struct {
    u8 *data;
    u32 bytes, channels, interleave, pitch, voice[2], paused;
    s32 loop;
    u64 phase;
    u32 block, ready, playing, ended;
    u64 mixed, audible;
    int h1[2], h2[2];
    s16 pcm[2][28];
    s16 previous_block[2];
    u32 radio;                 /* a radio message (RADIO*.PAC): ps2_nustream_radio_active */
    u32 music;                 /* the music (BGM.PAC): PS2_AUDIO_MUTE_MUSIC */
    u32 gen;                   /* ACZ_NUSTREAM_ASYNC: the open this slot's state belongs to (a newer one cancels) */
    u32 loading, want_play;    /* its data is being read; a play command came before the data */
} nu_stream;
static nu_stream streams[8];
static u64 nu_mix_frames, nu_radio_last;     /* audio thread: frames mixed, the last audible radio sample */
static int nu_radio_seen;
static SDL_SpinLock stream_lock;

/* ACZ_NUSTREAM_ASYNC (ov_hitch, 2026-10-25; default 1): an open (cmd 7) read the whole stream -- a BGM track 5-15 MB,
   a radio message 30-70 KB -- from the disc image on the EE thread inside the game's RPC, so the game's frame waited
   for it: BGM 10-33 ms in the sims (mission starts, music changes in flight: M11 field 1233 21.5 ms, M17's stage
   change 33 ms, M18's start 32 ms), radio 1-3 ms and 205 ms once (a cold read under disk load: the EE and the world
   stood 12 fields, M10). Now a loader thread reads it (256 KB a call, so other disc reads of the EE wait at most for
   one chunk) and the stream becomes ready when the data is in; a play command (cmd 10) that comes first starts the
   stream then (the music or the message starts a few ms later). The slot's voices stay the stream's while it loads;
   status / position read as a stream not started yet. `0` = the read inside the open, as before (the self-tests
   always use it). */
static int nu_async = -1, nu_sync_test;
static u32 nu_gen_next;
#define NU_JOBS 16u
static struct { u32 slot, gen, span; u64 pos; const ps2_disc_file *f; char name[96]; u32 a[7]; } nu_job[NU_JOBS];
static u32 nu_job_head, nu_job_tail;
static pthread_mutex_t nu_job_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t nu_job_cv = PTHREAD_COND_INITIALIZER;
static int nu_loader_on;
static int read_bytes(const ps2_disc_file *f, u64 pos, u32 size, u8 *out);
static void *nu_loader(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&nu_job_mx);
        while (nu_job_head == nu_job_tail) pthread_cond_wait(&nu_job_cv, &nu_job_mx);
        u32 k = nu_job_tail % NU_JOBS;
        u32 slot = nu_job[k].slot, gen = nu_job[k].gen, span = nu_job[k].span, a[7];
        u64 pos = nu_job[k].pos;
        const ps2_disc_file *f = nu_job[k].f;
        char name[96];
        memcpy(name, nu_job[k].name, sizeof name); memcpy(a, nu_job[k].a, sizeof a);
        nu_job_tail++;
        pthread_mutex_unlock(&nu_job_mx);
        u64 t0 = SDL_GetTicksNS();
        u8 header[4], *data = NULL;
        int ok = read_bytes(f, pos, 4, header) && !memcmp(header, "NPSF", 4);
        if (ok) data = (u8 *)malloc(span);
        for (u32 off = 0; ok && data && off < span; off += 262144u) {
            SDL_LockSpinlock(&stream_lock);                   /* (cancelled meanwhile: stop reading) */
            int live = streams[slot].gen == gen && streams[slot].loading;
            SDL_UnlockSpinlock(&stream_lock);
            if (!live) { ok = 0; break; }
            u32 n = span - off < 262144u ? span - off : 262144u;
            ok = read_bytes(f, pos + 2048 + off, n, data + off);
        }
        int used = 0;
        SDL_LockSpinlock(&stream_lock);
        nu_stream *s = &streams[slot];
        if (s->gen == gen && s->loading) {
            s->loading = 0;
            if (ok && data) {
                s->data = data; s->ready = 1; used = 1;
                if (s->want_play) s->playing = 1;
            }
            s->want_play = 0;
        }
        SDL_UnlockSpinlock(&stream_lock);
        if (!used) free(data);
        ps2_log("nustream: open slot=%u file='%s' offset=%08X bytes/ch=%u channels=%u interleave=%u pitch=%u result=%s "
                "(loader thread, %.1f ms)", slot, name, a[6], a[1], a[2], a[3], a[4],
                used ? "loaded" : ok && data ? "superseded" : "FAILED", (double)(SDL_GetTicksNS() - t0) / 1e6);
    }
    return NULL;
}
static int nu_async_on(void) {
    if (nu_async < 0) { const char *e = getenv("ACZ_NUSTREAM_ASYNC"); nu_async = !(e && *e == '0'); }
    if (nu_sync_test || !nu_async) return 0;
    if (!nu_loader_on) {
        pthread_t th;
        nu_loader_on = pthread_create(&th, NULL, nu_loader, NULL) == 0 ? 1 : -1;
        if (nu_loader_on > 0) pthread_detach(th);
    }
    return nu_loader_on > 0;
}

static u32 disk_offset(const nu_stream *s, u32 byte, u32 ch) {
    return byte / s->interleave * s->interleave * s->channels
         + ch * s->interleave + byte % s->interleave;
}
static int read_bytes(const ps2_disc_file *f, u64 pos, u32 size, u8 *out) {
    if (!f || pos + size > f->size) return 0;
    return ps2_vfs_read(f, pos, size, out) == (int)size;
}

void ps2_nustream_command(u32 cmd, const u32 *a, const char *name) {
    u32 slot = a[0];
    u8 *data = NULL;
    nu_stream candidate = {0};
    int async = cmd == 7 && slot < 8 && nu_async_on(), queued = 0;
    u32 job_span = 0; const ps2_disc_file *job_f = NULL;
    if (async) {                                  /* (the checks as below; the read goes to the loader thread) */
        candidate.bytes = a[1]; candidate.channels = a[2];
        candidate.interleave = a[3]; candidate.pitch = a[4] & 0x3fff;
        candidate.radio = name && (strstr(name, "RADIO") || strstr(name, "radio"));
        candidate.music = name && (strstr(name, "BGM") || strstr(name, "bgm"));
        if (candidate.bytes >= 16 && !(candidate.bytes & 15) &&
            candidate.channels >= 1 && candidate.channels <= 2 &&
            candidate.interleave >= 16 && candidate.interleave <= 0x10000 &&
            !(candidate.interleave & 15) && candidate.bytes <= 32u*1024*1024) {
            job_span = disk_offset(&candidate, candidate.bytes - 16, candidate.channels - 1) + 16;
            job_f = ps2_vfs_find(name);
            queued = job_f != NULL && (u64)a[6] + 2048 + job_span <= job_f->size;
        }
        if (!queued)
            ps2_log("nustream: open slot=%u file='%s' offset=%08X bytes/ch=%u channels=%u interleave=%u pitch=%u result=%s",
                    slot, name, a[6], a[1], a[2], a[3], a[4], "FAILED");
    } else if (cmd == 7 && slot < 8) {
        candidate.bytes = a[1]; candidate.channels = a[2];
        candidate.interleave = a[3]; candidate.pitch = a[4] & 0x3fff;
        candidate.radio = name && (strstr(name, "RADIO") || strstr(name, "radio"));
        candidate.music = name && (strstr(name, "BGM") || strstr(name, "bgm"));
        if (candidate.bytes >= 16 && !(candidate.bytes & 15) &&
            candidate.channels >= 1 && candidate.channels <= 2 &&
            candidate.interleave >= 16 && candidate.interleave <= 0x10000 &&
            !(candidate.interleave & 15) && candidate.bytes <= 32u*1024*1024) {
            u32 span = disk_offset(&candidate, candidate.bytes - 16, candidate.channels - 1) + 16;
            const ps2_disc_file *f = ps2_vfs_find(name);
            u8 header[4];
            data = (u8 *)malloc(span);
            if (!data || !read_bytes(f, a[6], 4, header) || memcmp(header, "NPSF", 4) ||
                !read_bytes(f, (u64)a[6] + 2048, span, data)) {
                free(data); data = NULL;
            }
        }
        ps2_log("nustream: open slot=%u file='%s' offset=%08X bytes/ch=%u channels=%u interleave=%u pitch=%u result=%s",
                slot, name, a[6], a[1], a[2], a[3], a[4], data ? "loaded" : "FAILED");
    }
    u32 job_gen = 0;
    SDL_LockSpinlock(&stream_lock);
    if (cmd == 2) {
        for (u32 i = 0; i < 8; i++) { free(streams[i].data); memset(&streams[i], 0, sizeof streams[i]); }
    } else if (slot < 8) {
        nu_stream *s = &streams[slot];
        switch (cmd) {
        case 10: if (s->ready) s->playing = 1; else if (s->loading) s->want_play = 1; break;
        case 6: s->voice[0] = a[1]; s->voice[1] = a[2]; break;
        case 7:
            free(s->data); s->data = data;
            s->bytes = candidate.bytes; s->channels = candidate.channels;
            s->interleave = candidate.interleave; s->pitch = candidate.pitch; s->radio = candidate.radio; s->music = candidate.music;
            s->phase = 0; s->block = ~0u; s->ended = s->playing = 0;
            s->mixed = s->audible = 0;
            s->ready = data != NULL;
            memset(s->h1, 0, sizeof s->h1); memset(s->h2, 0, sizeof s->h2);
            s->gen = job_gen = ++nu_gen_next; if (!s->gen) s->gen = job_gen = ++nu_gen_next;
            s->loading = (u32)queued; s->want_play = 0;
            break;
        case 8: s->loop = (s32)a[1]; break;
        case 11: s->playing = s->ready = 0; free(s->data); s->data = NULL; s->loading = s->want_play = 0; s->gen = 0; break;
        case 13: s->paused = a[1] != 0; break;
        case 15: s->paused = 0; s->loop = -1; break;
        case 23: s->pitch = a[1] & 0x3fff; break;
        }
    }
    SDL_UnlockSpinlock(&stream_lock);
    if (queued) {                                 /* (ACZ_NUSTREAM_ASYNC) */
        pthread_mutex_lock(&nu_job_mx);
        if (nu_job_head - nu_job_tail < NU_JOBS) {
            u32 k = nu_job_head % NU_JOBS;
            nu_job[k].slot = slot; nu_job[k].gen = job_gen; nu_job[k].span = job_span; nu_job[k].pos = a[6];
            nu_job[k].f = job_f; memcpy(nu_job[k].a, a, sizeof nu_job[k].a);
            snprintf(nu_job[k].name, sizeof nu_job[k].name, "%s", name ? name : "");
            nu_job_head++;
            pthread_cond_signal(&nu_job_cv);
        } else {                                  /* (16 opens pending: cannot happen with 8 slots; give it up) */
            SDL_LockSpinlock(&stream_lock);
            if (streams[slot].gen == job_gen) { streams[slot].loading = 0; streams[slot].want_play = 0; }
            SDL_UnlockSpinlock(&stream_lock);
        }
        pthread_mutex_unlock(&nu_job_mx);
    }
}

int ps2_nustream_selftest(void) {
    nu_sync_test = 1;                             /* (the opens read at once: ACZ_NUSTREAM_ASYNC off) */
    u32 args[7] = {0}, pos, ended;
    s16 out[120*2], volumes[48][2] = {{0}};
    int fail = 0;
    ps2_nustream_command(2, args, "");
    nu_stream *s = &streams[0];
    s->data = (u8 *)calloc(1, 64);
    if (!s->data) return 1;
    s->bytes = 32; s->channels = 2; s->interleave = 16;
    s->pitch = 2048; s->loop = -1; s->voice[0] = 0; s->voice[1] = 1;
    s->ready = s->playing = 1; s->block = ~0u;
    for (u32 b = 0; b < 4; b++) memset(s->data + b*16 + 2, b & 1 ? 0xff : 0x11, 14);
    volumes[0][0] = volumes[1][1] = 32767;
    memset(out, 0, sizeof out);
    ps2_nustream_mix(out, 56, volumes);
    ps2_nustream_status(0, &pos, &ended);
    if (pos != 32 || ended || out[0] != 0 || out[1] != 0 ||
        out[2] != 2047 || out[3] != -2048 ||
        out[4] != 4095 || out[5] != -4096) fail++;
    s->paused = 1;
    ps2_nustream_mix(out, 30, volumes);
    ps2_nustream_status(0, &pos, &ended);
    if (pos != 32 || ended) fail++;
    s->paused = 0;
    ps2_nustream_mix(out, 57, volumes);
    ps2_nustream_status(0, &pos, &ended);
    if (pos != ~0u || ended != 1) fail++;
    s->phase = 0; s->ended = 0; s->playing = 1; s->loop = 28;
    ps2_nustream_mix(out, 120, volumes);
    ps2_nustream_status(0, &pos, &ended);
    if (ended || pos != 32) fail++;
    free(s->data); s->data = (u8 *)calloc(1, 96);
    if (!s->data) return fail + 1;
    s->bytes = 48; s->pitch = 4096; s->loop = 56;
    s->phase = 0; s->block = ~0u; s->ended = 0; s->playing = 1;
    ps2_nustream_mix(out, 90, volumes);
    ps2_nustream_status(0, &pos, &ended);
    if (ended || pos != 64) fail++;
    ps2_nustream_command(11, args, "");
    if (ps2_nustream_voice_mask()) fail++;
    ps2_nustream_command(2, args, "");
    ps2_log("nustream selftest: %s (stereo, byte position, pause, EOF, loop, stop)", fail ? "FAILED" : "passed");
    nu_sync_test = 0;
    return fail;
}

int ps2_nustream_file_selftest(const char *disc) {
    nu_sync_test = 1;                             /* (as ps2_nustream_selftest; the process ends after the tests) */
    const u32 offsets[] = {0, 0x19800, 0x41800, 0x1a295800, 0xb2000};
    if (ps2_vfs_open(disc)) return 1;
    const char *name = "cd:\\BIN\\RADIOEE.PAC";
    const ps2_disc_file *f = ps2_vfs_find(name);
    int fail = 0;
    for (u32 i = 0; i < sizeof offsets / sizeof offsets[0]; i++) {
        u32 h[16], args[7] = {0}, pos, eof = 0;
        s16 out[512*2], volumes[48][2] = {{0}};
        if (!read_bytes(f, offsets[i], sizeof h, (u8 *)h)) return 1;
        ps2_nustream_command(2, args, "");
        args[1] = 0; args[2] = 1; ps2_nustream_command(6, args, "");
        args[1] = ~0u; ps2_nustream_command(8, args, "");
        args[1] = h[2]; args[2] = h[3]; args[3] = h[4];
        args[4] = h[6]*4096u/48000u; args[5] = 0; args[6] = offsets[i];
        ps2_nustream_command(7, args, name);
        args[0] = 0; ps2_nustream_command(10, args, "");
        volumes[0][0] = volumes[0][1] = volumes[1][0] = volumes[1][1] = 32767;
        u64 limit = ((u64)(h[2]/16)*28*4096 + args[4]-1)/args[4];
        u64 consumed = 0, nonzero = 0;
        while (consumed <= limit && !eof) {
            memset(out, 0, sizeof out);
            ps2_nustream_mix(out, 512, volumes);
            for (u32 j = 0; j < 1024; j++) nonzero += out[j] != 0;
            consumed += 512;
            ps2_nustream_status(0, &pos, &eof);
            if (eof && consumed <= limit) fail++;
        }
        if (!eof || !nonzero || pos != ~0u) fail++;
        ps2_log("nustream file test: offset=%08X frames=%llu expected=%llu nonzero=%llu eof=%u",
                offsets[i], (unsigned long long)consumed, (unsigned long long)limit,
                (unsigned long long)nonzero, eof);
    }
    u32 args[7] = {0}; ps2_nustream_command(2, args, "");
    return fail;
}

void ps2_nustream_status(u32 slot, u32 *position, u32 *ended) {
    SDL_LockSpinlock(&stream_lock);
    nu_stream *s = &streams[slot];
    *position = s->ended ? ~0u : (u32)((s->phase >> 12) / 28 * 16 * s->channels);
    *ended = s->ended;
    SDL_UnlockSpinlock(&stream_lock);
}

void ps2_nustream_report(u32 slot) {
    SDL_LockSpinlock(&stream_lock);
    nu_stream *s = &streams[slot];
    ps2_log("nustream: audio slot=%u ready=%u playing=%u paused=%u voices=%u,%u loop=%08X mixed=%llu audible=%llu",
            slot, s->ready, s->playing, s->paused, s->voice[0], s->voice[1], (u32)s->loop,
            (unsigned long long)s->mixed, (unsigned long long)s->audible);
    SDL_UnlockSpinlock(&stream_lock);
}

u64 ps2_nustream_voice_mask(void) {
    u64 mask = 0;
    SDL_LockSpinlock(&stream_lock);
    for (u32 i = 0; i < 8; i++) if (streams[i].ready || streams[i].loading) {
        for (u32 ch = 0; ch < streams[i].channels; ch++)
            if (streams[i].voice[ch] < 48) mask |= 1ull << streams[i].voice[ch];
    }
    SDL_UnlockSpinlock(&stream_lock);
    return mask;
}

static s16 saturate(int v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (s16)v; }
void ps2_nustream_mix(s16 *out, u32 frames, const s16 volumes[48][2]) {
    static int mute_music = -1;    /* PS2_AUDIO_MUTE_MUSIC=1: the music mixed silent, the radio and effects stay */
    if (mute_music < 0) {
        const char *m = getenv("PS2_AUDIO_MUTE_MUSIC");
        mute_music = m && *m == '1';
        if (mute_music) ps2_log("nustream: the music (BGM.PAC) is mixed silent (PS2_AUDIO_MUTE_MUSIC=1); radio and effects stay");
    }
    /* PS2_AUDIO_RADIO_DB / PS2_AUDIO_SFX_DB (dB, default 0; recordings): the radio streams (RADIO*.PAC) and the game's
       sound effects (the SPU2 voices, already in out when the streams are added) louder / quieter, so a radio line
       can stand above the engine in a recording without music; the music stream and our immersion layers keep theirs */
    static int gains_read;
    static float radio_gain = 1.0f, sfx_gain = 1.0f;
    if (!gains_read) {
        const char *r = getenv("PS2_AUDIO_RADIO_DB"), *x = getenv("PS2_AUDIO_SFX_DB");
        float rdb = r ? (float)atof(r) : 0.0f, xdb = x ? (float)atof(x) : 0.0f;
        rdb = rdb < -24.0f ? -24.0f : rdb > 12.0f ? 12.0f : rdb;
        xdb = xdb < -24.0f ? -24.0f : xdb > 12.0f ? 12.0f : xdb;
        radio_gain = powf(10.0f, rdb / 20.0f); sfx_gain = powf(10.0f, xdb / 20.0f);
        if (rdb != 0.0f || xdb != 0.0f)
            ps2_log("nustream: radio %+.1f dB, sound effects %+.1f dB (PS2_AUDIO_RADIO_DB / PS2_AUDIO_SFX_DB)", rdb, xdb);
        gains_read = 1;
    }
    if (sfx_gain != 1.0f) for (u32 k = 0; k < frames * 2; k++) out[k] = saturate((int)lrintf(out[k] * sfx_gain));
    SDL_LockSpinlock(&stream_lock);
    for (u32 i = 0; i < 8; i++) {
        nu_stream *s = &streams[i];
        if (!s->ready || !s->playing || s->paused || s->ended) continue;
        u64 end = (u64)(s->bytes / 16) * 28 << 12;
        for (u32 k = 0; k < frames; k++) {
            if (s->phase >= end) {
                u64 loop_phase = s->loop >= 0
                    ? ((u64)((u32)s->loop / 28) * 28 << 12) : end;
                if (loop_phase < end) {
                    s->phase = loop_phase + (s->phase - end) % (end - loop_phase);
                    s->block = ~0u;
                } else { s->ended = 1; s->playing = 0; break; }
            }
            u32 sample = (u32)(s->phase >> 12), block = sample / 28;
            if (s->block != block) {
                for (u32 ch = 0; ch < s->channels; ch++) {
                    u32 flags;
                    s->previous_block[ch] = (s16)s->h1[ch];
                    ps2_spu2_decode_block(s->data + disk_offset(s, block * 16, ch),
                                         s->pcm[ch], &s->h1[ch], &s->h2[ch], &flags);
                }
                s->block = block;
            }
            int l = out[k*2], r = out[k*2+1];
            if (!(mute_music && s->music)) for (u32 ch = 0; ch < s->channels; ch++) if (s->voice[ch] < 48) {
                u32 idx = sample % 28;
                int previous = idx ? s->pcm[ch][idx - 1] : s->previous_block[ch];
                int v = previous + (((int)s->pcm[ch][idx] - previous)
                                    * (int)(s->phase & 0xfff) >> 12);
                if (s->radio && radio_gain != 1.0f) v = (int)lrintf(v * radio_gain);
                l += v * volumes[s->voice[ch]][0] >> 15;
                r += v * volumes[s->voice[ch]][1] >> 15;
            }
            out[k*2] = saturate(l); out[k*2+1] = saturate(r);
            s->mixed++;
            for (u32 ch = 0; ch < s->channels; ch++) if (s->voice[ch] < 48 &&
                s->pcm[ch][sample % 28] && (volumes[s->voice[ch]][0] || volumes[s->voice[ch]][1])) {
                s->audible++;
                if (s->radio && (s->pcm[ch][sample % 28] > 300 || s->pcm[ch][sample % 28] < -300)) {
                    nu_radio_last = nu_mix_frames + k; nu_radio_seen = 1;
                }
                break;
            }
            s->phase += s->pitch;
        }
    }
    nu_mix_frames += frames;
    SDL_UnlockSpinlock(&stream_lock);
}
/* audio thread: a radio message is being heard (a sample above -40 dBFS within the last 0.15 s); the pilot's breathing
   (ps2_gforce_audio.c) ducks under it */
int ps2_nustream_radio_active(void) { return nu_radio_seen && nu_mix_frames - nu_radio_last < 7200u; }
