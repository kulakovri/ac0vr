/* ps2_recexact.c -- ACZ_REC_EXACT: the frame-exact recording (rec_pace 2026-11-02, bug #264).
   The trailer recordings (`play_zero_desktop.sh record cine`) came from OBS's game capture of the window: the hook
   copies the window at its presents (at most one every 16.67 ms of its own clock: limit_framerate) and OBS's video
   thread takes the newest copy at its own 60 Hz ticks. The window presents in the XR frame that picks the game frame
   up, at the headset's vsync, which drifts against the field clock (QPC); in phases the pickups alternate 1 / 3
   refreshes at 120 Hz (8 / 25 ms), and OBS repeated 8-20 game frames a second and lost as many. Here the runtime
   itself hands every game frame's window image to an encoder: ps2_video.c (rx_*) copies the presented window into a
   host buffer as NV12 (rx_nv12.comp) in the frame that shows a game frame's content for the first time, tagged with
   the field it belongs to (the guest's field count + the fields the field clock dropped: real time); this file writes
   each tagged image once, in field order, to ffmpeg's stdin-like named pipe as raw NV12 at the field rate (a field
   without an image of its own -- the renderer skipped it, a capture was lost -- repeats the previous image, counted),
   and the game's own mix (the audio callback's chunks, aligned to the first frame by the monotonic clock and kept in
   step with it) to a second pipe. ffmpeg (ACZ_REC_EXACT_FFMPEG) encodes them into ACZ_REC_EXACT_OUT (x264 CRF 16
   superfast as OBS's cine profile, keyframe every second, AAC 192k "Game"), pinned to the efficiency cores
   (ACZ_REC_EXACT_AFFINITY). The video's timeline is the field clock: frame k = field f0 + k, so the file plays in real
   time whatever the headset's or the desktop's refresh is. Nothing here touches what the headset shows. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#include "ps2_runtime.h"

#ifdef _WIN32
#include <windows.h>
#endif

#define RX_MAXB   96
#define RX_ACH    2048            /* audio chunks in the ring (~20 s at 512-frame chunks) */
#define RX_ACHF   1024            /* frames per chunk at most */
#define RX_RATE   48000

u64 ps2_rx_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

typedef struct { u64 t; u32 n; s16 d[RX_ACHF * 2]; } rx_chunk;

static struct {
    int cfg, on;                       /* cfg: env read; on: ACZ_REC_EXACT=1 */
    int begun, failed, stopping, video_done, finished;
    u64 limit;                         /* ACZ_REC_EXACT_FRAMES (test): the file ends after this many frames */
    u32 w, h; size_t fbytes;
    u64 fns;                           /* the field period (ns) */
    char fps[24];
    char out[1024], ffmpeg[1024], log[1100];
    /* slots: the renderer's host buffers (ps2_video.c) */
    int nb; u8 *ptr[RX_MAXB];
    int state[RX_MAXB];                /* 0 free, 1 recorded (GPU), 2 ready (queued), 3 written / held */
    u64 tag[RX_MAXB], tpub[RX_MAXB];
    int q[RX_MAXB]; int qn, qh;        /* ready queue (FIFO) */
    pthread_mutex_t mu; pthread_cond_t cv;
    pthread_t vth, ath; int vth_ok, ath_ok;
    /* timeline */
    int have0; u64 f0, t0; long long fix;   /* first tag and its publication time; fix: timer-stall corrections */
    u64 next; int held;
    /* stats */
    u64 nready, written, filled, late, lost, stall_n, stall_ns, last_log_ns, last_written, last_filled, last_lost;
    u32 fill_logs;
    double wr_ms_sum, wr_ms_max; u64 wr_n;
    /* audio */
    rx_chunk *ach; volatile u32 aw, ar;   /* ring: written by the audio callback, read by the audio thread */
    u64 adrop;
    int astarted, afill; long long apos;   /* samples written to the audio pipe; afill: silence filled */
    double aema; int acorr; u64 adup, askip, asil, ahard;
    u64 alast_t;
#ifdef _WIN32
    HANDLE vpipe, apipe, proc, logh;
    OVERLAPPED vov, aov;
#endif
} rx;

static void rx_cfg(void) {
    if (rx.cfg) return;
    rx.cfg = 1;
    const char *e = getenv("ACZ_REC_EXACT");
    rx.on = e && *e == '1';
    if (!rx.on) return;
    pthread_mutex_init(&rx.mu, NULL);
    pthread_cond_init(&rx.cv, NULL);
    rx.held = -1;
    e = getenv("ACZ_REC_EXACT_FFMPEG");
    snprintf(rx.ffmpeg, sizeof rx.ffmpeg, "%s", e && *e ? e : "ffmpeg.exe");
    e = getenv("ACZ_REC_EXACT_OUT");
    if (e && *e) snprintf(rx.out, sizeof rx.out, "%s", e);
    else {
        time_t t = time(NULL); struct tm tm; localtime_s(&tm, &t);
        snprintf(rx.out, sizeof rx.out, "out/rec_exact_%04d-%02d-%02d_%02d-%02d-%02d.mkv", tm.tm_year + 1900,
                 tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    }
    snprintf(rx.log, sizeof rx.log, "%s.ffmpeg.log", rx.out);
    e = getenv("ACZ_REC_EXACT_FRAMES"); rx.limit = e ? strtoull(e, NULL, 10) : 0;
    e = getenv("PS2_FIELD_HZ");
    double hz = e && *e ? atof(e) : 0.0;
    rx.fns = hz > 1.0 && hz < 10000.0 ? (u64)(1e9 / hz) : 16683333ull;
    if (hz > 1.0 && hz < 10000.0) {
        if (hz == (double)(long)hz) snprintf(rx.fps, sizeof rx.fps, "%ld", (long)hz);
        else snprintf(rx.fps, sizeof rx.fps, "%.6f", hz);
    } else snprintf(rx.fps, sizeof rx.fps, "60000/1001");
    rx.ach = (rx_chunk *)calloc(RX_ACH, sizeof(rx_chunk));
    if (!rx.ach) { ps2_log("rec-exact: no memory for the audio ring: off"); rx.on = 0; }
}
int ps2_rx_enabled(void) { rx_cfg(); return rx.on && !rx.failed; }

/* ---- the encoder process (Windows) ---- */
#ifdef _WIN32
static u64 rx_ecores(void) {             /* the efficiency cores' mask (lower EfficiencyClass), 0 = all alike */
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &len);
    if (!len) return 0;
    u8 *buf = (u8 *)malloc(len);
    if (!buf || !GetLogicalProcessorInformationEx(RelationProcessorCore, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf, &len)) {
        free(buf); return 0;
    }
    int cmax = 0; u64 m = 0;
    for (DWORD o = 0; o < len;) {
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX p = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf + o);
        if (p->Processor.EfficiencyClass > cmax) cmax = p->Processor.EfficiencyClass;
        o += p->Size;
    }
    for (DWORD o = 0; o < len;) {
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX p = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf + o);
        if (p->Processor.EfficiencyClass < cmax && p->Processor.GroupMask[0].Group == 0) m |= (u64)p->Processor.GroupMask[0].Mask;
        o += p->Size;
    }
    free(buf);
    return m;
}
static HANDLE rx_pipe(const char *name, DWORD bufsz, OVERLAPPED *ov) {
    HANDLE h = CreateNamedPipeA(name, PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE | PIPE_WAIT, 1, bufsz,
                                0, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    memset(ov, 0, sizeof *ov);
    ov->hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    return h;
}
/* wait until ffmpeg opens the pipe (or exits): 1 = connected */
static int rx_connect(HANDLE h, OVERLAPPED *ov, DWORD ms) {
    ResetEvent(ov->hEvent);
    if (ConnectNamedPipe(h, ov)) return 1;
    DWORD er = GetLastError();
    if (er == ERROR_PIPE_CONNECTED) return 1;
    if (er != ERROR_IO_PENDING) return 0;
    HANDLE w[2] = { ov->hEvent, rx.proc };
    DWORD r = WaitForMultipleObjects(2, w, FALSE, ms);
    if (r != WAIT_OBJECT_0) { CancelIo(h); return 0; }
    DWORD n;
    return GetOverlappedResult(h, ov, &n, FALSE) ? 1 : 0;
}
static int rx_write(HANDLE h, OVERLAPPED *ov, const void *p, size_t n) {
    const u8 *b = (const u8 *)p;
    while (n) {
        DWORD k = n > (1u << 22) ? (1u << 22) : (DWORD)n, got = 0;
        ResetEvent(ov->hEvent);
        if (!WriteFile(h, b, k, &got, ov)) {
            if (GetLastError() != ERROR_IO_PENDING) return 0;
            if (!GetOverlappedResult(h, ov, &got, TRUE)) return 0;
        } else if (!GetOverlappedResult(h, ov, &got, TRUE)) return 0;
        if (!got) return 0;
        b += got; n -= got;
    }
    return 1;
}
static int rx_spawn(void) {
    char vn[96], an[96];
    snprintf(vn, sizeof vn, "\\\\.\\pipe\\acz_rx_v_%lu", (unsigned long)GetCurrentProcessId());
    snprintf(an, sizeof an, "\\\\.\\pipe\\acz_rx_a_%lu", (unsigned long)GetCurrentProcessId());
    rx.vpipe = rx_pipe(vn, 16u << 20, &rx.vov);
    rx.apipe = rx_pipe(an, 1u << 20, &rx.aov);
    if (!rx.vpipe || !rx.apipe) { ps2_log("rec-exact: cannot create the pipes (%lu)", GetLastError()); return -1; }
    /* the encoder: x264 as OBS's "ACZ VR cine" profile (CRF 16 superfast, a keyframe a second), or NVENC HEVC CQP */
    const char *e = getenv("ACZ_REC_EXACT_ENCODER");
    int nv = e && !strcmp(e, "nvenc");
    const char *crf = getenv("ACZ_REC_EXACT_CRF"); if (!crf || !*crf) crf = nv ? "17" : "16";
    const char *pre = getenv("ACZ_REC_EXACT_PRESET"); if (!pre || !*pre) pre = nv ? "p4" : "superfast";
    char venc[256];
    if (nv) snprintf(venc, sizeof venc, "-c:v hevc_nvenc -preset %s -rc constqp -qp %s -bf 2", pre, crf);
    else snprintf(venc, sizeof venc, "-c:v libx264 -preset %s -crf %s -profile:v high", pre, crf);
    const char *xa = getenv("ACZ_REC_EXACT_ARGS");
    char cmd[4096];
    snprintf(cmd, sizeof cmd,
             "\"%s\" -hide_banner -nostdin -nostats -loglevel warning -y "
             "-f rawvideo -pix_fmt nv12 -video_size %ux%u -framerate %s -i %s "
             "-f s16le -ar %d -ac 2 -i %s "
             "-map 0:v -map 1:a %s -g %d -colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv "
             "-c:a aac -b:a 192k -metadata:s:a:0 title=Game -metadata:s:v:0 title=\"frame-exact cine (ACZ_REC_EXACT)\" "
             "%s \"%s\"",
             rx.ffmpeg, rx.w, rx.h, rx.fps, vn, RX_RATE, an, venc, (int)(1e9 / (double)rx.fns + 0.5),
             xa ? xa : "", rx.out);
    SECURITY_ATTRIBUTES sa; memset(&sa, 0, sizeof sa); sa.nLength = sizeof sa; sa.bInheritHandle = TRUE;
    rx.logh = CreateFileA(rx.log, GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    STARTUPINFOA si; memset(&si, 0, sizeof si); si.cb = sizeof si;
    PROCESS_INFORMATION pi; memset(&pi, 0, sizeof pi);
    if (rx.logh != INVALID_HANDLE_VALUE) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = NULL; si.hStdOutput = rx.logh; si.hStdError = rx.logh;
    }
    /* out of the launcher's job if it lets us: a game that is killed (a test's timeout) leaves the encoder to see the
       pipes' end and close the file properly */
    if (!CreateProcessA(NULL, cmd, NULL, NULL, rx.logh != INVALID_HANDLE_VALUE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB, NULL, NULL, &si, &pi)
        && !CreateProcessA(NULL, cmd, NULL, NULL, rx.logh != INVALID_HANDLE_VALUE, CREATE_SUSPENDED | CREATE_NO_WINDOW,
                           NULL, NULL, &si, &pi)) {
        ps2_log("rec-exact: cannot start the encoder (%lu): %s", GetLastError(), cmd);
        return -1;
    }
    rx.proc = pi.hProcess;
    u64 aff = 0;
    e = getenv("ACZ_REC_EXACT_AFFINITY");
    if (!e || !*e || !strcmp(e, "auto")) aff = rx_ecores();
    else aff = strtoull(e, NULL, 16);
    if (aff) SetProcessAffinityMask(pi.hProcess, (DWORD_PTR)aff);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    ps2_log("rec-exact: the encoder started (pid %lu, affinity %llX%s): %s", (unsigned long)pi.dwProcessId,
            (unsigned long long)aff, aff ? "" : " = Windows places it", cmd);
    return 0;
}
#endif

/* ---- video ---- */
void ps2_rx_buffers(int n, u8 *const *ptrs, u32 w, u32 h) {   /* (renderer, once: the host buffers, the image size) */
    rx_cfg();
    pthread_mutex_lock(&rx.mu);
    rx.nb = n > RX_MAXB ? RX_MAXB : n;
    for (int i = 0; i < rx.nb; i++) { rx.ptr[i] = ptrs[i]; rx.state[i] = 0; }
    rx.w = w; rx.h = h; rx.fbytes = (size_t)w * h * 3u / 2u;
    pthread_mutex_unlock(&rx.mu);
}
/* a free buffer for this frame's capture (renderer): waits up to ms while all are queued or being written (the
   encoder behind); -1 = none (the field will repeat the previous image, counted) */
int ps2_rx_acquire(int ms) {
    if (!rx.on || rx.failed || rx.stopping) return -1;
    u64 t0 = 0;
    int got = -1;
    pthread_mutex_lock(&rx.mu);
    for (;;) {
        for (int i = 0; i < rx.nb && got < 0; i++) if (rx.state[i] == 0) got = i;
        if (got >= 0) {
            rx.state[got] = 1;
            if (t0) { rx.stall_n++; rx.stall_ns += ps2_rx_now_ns() - t0; }
            break;
        }
        if (!t0) t0 = ps2_rx_now_ns();
        if (rx.failed || ps2_rx_now_ns() - t0 > (u64)ms * 1000000ull) { rx.lost++; break; }
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 2000000; if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&rx.cv, &rx.mu, &ts);
    }
    pthread_mutex_unlock(&rx.mu);
    return got;
}
void ps2_rx_cancel(int slot) {                       /* (a recorded capture that did not reach the GPU) */
    if (slot < 0 || slot >= rx.nb) return;
    pthread_mutex_lock(&rx.mu); rx.state[slot] = 0; pthread_cond_broadcast(&rx.cv); pthread_mutex_unlock(&rx.mu);
}
static void *rx_video_thread(void *arg);
static void *rx_audio_thread(void *arg);
/* the GPU finished a capture: queue it (renderer, in submission order = field order) */
void ps2_rx_ready(int slot, u64 tag, u64 tpub) {
    if (slot < 0 || slot >= rx.nb) return;
    pthread_mutex_lock(&rx.mu);
    if (!rx.have0) {
        rx.have0 = 1; rx.f0 = tag; rx.t0 = tpub;
        /* the wall clock of the video's 0 s (recmux.py lines OBS's voice track up with it) */
        struct timespec rt; clock_gettime(CLOCK_REALTIME, &rt);
        double wall = (double)rt.tv_sec + (double)rt.tv_nsec * 1e-9 - ((double)ps2_rx_now_ns() - (double)tpub) * 1e-9;
        time_t ws = (time_t)wall; struct tm tm; localtime_s(&tm, &ws);
        ps2_log("rec-exact: the first frame: field %llu, %ux%u NV12 at %s fps, %d host buffers (%.1f MB); file %s; "
                "wall %04d-%02d-%02d %02d:%02d:%02d.%03d", (unsigned long long)tag, rx.w, rx.h, rx.fps, rx.nb,
                (double)rx.fbytes * rx.nb / 1048576.0, rx.out, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                tm.tm_min, tm.tm_sec, (int)((wall - (double)ws) * 1000.0));
    }
    rx.tag[slot] = tag; rx.tpub[slot] = tpub; rx.state[slot] = 2;
    rx.q[(rx.qh + rx.qn) % RX_MAXB] = slot; rx.qn++;
    rx.nready++;
    if (!rx.begun && !rx.failed) {
        rx.begun = 1;
        rx.vth_ok = pthread_create(&rx.vth, NULL, rx_video_thread, NULL) == 0;
        rx.ath_ok = pthread_create(&rx.ath, NULL, rx_audio_thread, NULL) == 0;
    }
    pthread_cond_broadcast(&rx.cv);
    pthread_mutex_unlock(&rx.mu);
}
static void rx_free(int s) { rx.state[s] = 0; pthread_cond_broadcast(&rx.cv); }
static void rx_report(const char *why) {
    u64 now = ps2_rx_now_ns();
    double secs = rx.written ? (double)rx.written * (double)rx.fns / 1e9 : 0.0;
    ps2_log("rec-exact: %s: %llu frames written (%.1f s of video), %llu captured, %llu fields repeated (no image of "
            "their own: skipped by the renderer or a lost capture), %llu late, %llu captures lost (no free buffer), "
            "renderer waited for a buffer %llu times (%.1f ms); pipe write %.2f ms a frame (max %.1f); audio: %llu "
            "chunks dropped, %llu samples repeated / %llu skipped to follow the clock, %llu of silence, %llu "
            "resyncs, offset %.1f ms", why, (unsigned long long)rx.written, secs, (unsigned long long)rx.nready,
            (unsigned long long)rx.filled, (unsigned long long)rx.late, (unsigned long long)rx.lost,
            (unsigned long long)rx.stall_n, (double)rx.stall_ns / 1e6, rx.wr_n ? rx.wr_ms_sum / (double)rx.wr_n : 0.0,
            rx.wr_ms_max, (unsigned long long)rx.adrop, (unsigned long long)rx.adup, (unsigned long long)rx.askip,
            (unsigned long long)rx.asil, (unsigned long long)rx.ahard, rx.aema * 1000.0 / RX_RATE);
    rx.last_log_ns = now;
}
static void rx_finish(const char *why);
static void *rx_video_thread(void *arg) {
    (void)arg;
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    if (rx_spawn() != 0 || !rx_connect(rx.vpipe, &rx.vov, 20000)) {
        ps2_log("rec-exact: the encoder did not open its video input (see %s): the recording is off", rx.log);
        pthread_mutex_lock(&rx.mu);
        rx.failed = 1;
        for (int i = 0; i < rx.nb; i++) if (rx.state[i] >= 2) rx.state[i] = 0;
        rx.qn = 0;
        pthread_cond_broadcast(&rx.cv);
        pthread_mutex_unlock(&rx.mu);
        return NULL;
    }
    rx.last_log_ns = ps2_rx_now_ns();
    for (;;) {
        pthread_mutex_lock(&rx.mu);
        while (!rx.qn && !rx.stopping) pthread_cond_wait(&rx.cv, &rx.mu);
        if (!rx.qn && rx.stopping) { pthread_mutex_unlock(&rx.mu); break; }
        int s = rx.q[rx.qh]; rx.qh = (rx.qh + 1) % RX_MAXB; rx.qn--;
        rx.state[s] = 3;
        u64 tag = rx.tag[s], tp = rx.tpub[s];
        pthread_mutex_unlock(&rx.mu);
        /* the frame's index on the field clock: its tag (real-time field count) from the first; a timer stall the
           count missed (the publication more than 6 fields later than the count says) moves the rest on */
        long long idx = (long long)(tag - rx.f0) + rx.fix;
        long long ti = (long long)(((double)tp - (double)rx.t0) / (double)rx.fns + 0.5);
        if (ti > idx + 6) { rx.fix += ti - idx; idx = ti; }
        if (idx < (long long)rx.next) {
            rx.late++;
            pthread_mutex_lock(&rx.mu); rx_free(s); pthread_mutex_unlock(&rx.mu);
            continue;
        }
        int ok = 1;
        u64 w0 = ps2_rx_now_ns();
        if ((long long)rx.next < idx && rx.fill_logs < 40u) {
            rx.fill_logs++;
            ps2_log("rec-exact: frame %llu (field %llu): %lld field(s) before it without an image of their own: the "
                    "previous image repeated%s", (unsigned long long)idx, (unsigned long long)tag,
                    idx - (long long)rx.next, rx.fill_logs == 40u ? " (no more of these lines)" : "");
        }
        while (ok && (long long)rx.next < idx) {      /* fields without an image of their own: the previous again */
            int src = rx.held >= 0 ? rx.held : s;
            ok = rx_write(rx.vpipe, &rx.vov, rx.ptr[src], rx.fbytes);
            rx.next++; rx.filled++;
        }
        if (ok) ok = rx_write(rx.vpipe, &rx.vov, rx.ptr[s], rx.fbytes);
        double ms = (double)(ps2_rx_now_ns() - w0) / 1e6;
        rx.wr_ms_sum += ms; rx.wr_n++; if (ms > rx.wr_ms_max) rx.wr_ms_max = ms;
        rx.next++; rx.written = rx.next;
        pthread_mutex_lock(&rx.mu);
        if (rx.held >= 0) rx_free(rx.held);
        rx.held = s;
        if (!ok) {
            rx.failed = 1;
            for (int i = 0; i < rx.nb; i++) if (rx.state[i] >= 2) rx.state[i] = 0;
            rx.qn = 0; rx.held = -1;
            pthread_cond_broadcast(&rx.cv);
        }
        pthread_mutex_unlock(&rx.mu);
        if (!ok) { ps2_log("rec-exact: the encoder stopped taking frames (see %s): the recording ends here", rx.log); break; }
        if (ps2_rx_now_ns() - rx.last_log_ns > 30000000000ull) rx_report("last 30 s; since the start");
        if (rx.limit && rx.written >= rx.limit) {     /* (ACZ_REC_EXACT_FRAMES, a test: the file ends, the game goes on) */
            pthread_mutex_lock(&rx.mu);
            rx.stopping = 1;
            for (int i = 0; i < rx.nb; i++) if (rx.state[i] >= 2 && i != rx.held) rx.state[i] = 0;
            rx.qn = 0;
            pthread_cond_broadcast(&rx.cv);
            pthread_mutex_unlock(&rx.mu);
            rx.video_done = 1;
            rx_finish("ACZ_REC_EXACT_FRAMES reached");
            return NULL;
        }
    }
    rx.video_done = 1;
#endif
    return NULL;
}

/* ---- audio: the game's mix, aligned to the first frame on the monotonic clock ---- */
void ps2_rx_audio(const s16 *buf, int frames) {      /* (the SDL audio callback; never blocks) */
    if (!rx.on || !rx.ach || rx.failed || rx.stopping || frames <= 0) return;
    u64 t = ps2_rx_now_ns();
    while (frames > 0) {
        int n = frames > RX_ACHF ? RX_ACHF : frames;
        u32 w = __atomic_load_n(&rx.aw, __ATOMIC_RELAXED), r = __atomic_load_n(&rx.ar, __ATOMIC_ACQUIRE);
        if (w - r >= RX_ACH) { rx.adrop++; return; }
        rx_chunk *c = &rx.ach[w % RX_ACH];
        c->t = t; c->n = (u32)n;
        memcpy(c->d, buf, (size_t)n * 4u);
        __atomic_store_n(&rx.aw, w + 1u, __ATOMIC_RELEASE);
        buf += n * 2; frames -= n;
    }
}
#ifdef _WIN32
static s16 rx_zero[RX_ACHF * 2];
static int rx_silence(long long n) {
    while (n > 0) {
        int k = n > RX_ACHF ? RX_ACHF : (int)n;
        if (!rx_write(rx.apipe, &rx.aov, rx_zero, (size_t)k * 4u)) return 0;
        rx.apos += k; rx.asil += (u64)k; n -= k;
    }
    return 1;
}
#endif
static void *rx_audio_thread(void *arg) {
    (void)arg;
#ifdef _WIN32
    /* (ffmpeg opens its audio input after the video input's first frames) */
    for (int i = 0; i < 400 && !rx.proc && !rx.failed; i++) Sleep(50);
    if (!rx.proc || !rx_connect(rx.apipe, &rx.aov, 30000)) {
        if (!rx.failed) ps2_log("rec-exact: the encoder did not open its audio input (see %s)", rx.log);
        return NULL;
    }
    const double spn = (double)RX_RATE / 1e9;          /* samples per ns */
    static s16 tmp[RX_ACHF * 2 + 64];
    for (;;) {
        int stop = __atomic_load_n(&rx.stopping, __ATOMIC_ACQUIRE);
        u32 r = __atomic_load_n(&rx.ar, __ATOMIC_RELAXED), w = __atomic_load_n(&rx.aw, __ATOMIC_ACQUIRE);
        int ok = 1;
        while (ok && r != w) {
            rx_chunk *c = &rx.ach[r % RX_ACH];
            double exp_end = ((double)c->t - (double)rx.t0) * spn;   /* where this chunk's last sample belongs */
            int n = (int)c->n;
            if (!rx.astarted || rx.afill) {
                if (exp_end < 0.0) { r++; continue; }               /* before the first frame */
                long long pad = (long long)(exp_end - n) - rx.apos; /* the chunk where it belongs: after silence */
                if (pad > 0) ok = rx_silence(pad);
                else if (pad < 0) {                                 /* (after a fill: its overlap left out) */
                    int cut = -pad >= n ? n : (int)-pad;
                    rx.askip += (u64)cut;
                    if (cut == n) { r++; rx.afill = 0; rx.astarted = 1; continue; }
                    memmove(c->d, c->d + 2 * cut, (size_t)(n - cut) * 4u); n -= cut; c->n = (u32)n;
                }
                rx.astarted = 1; rx.afill = 0; rx.aema = 0.0;
            }
            double e = (double)(rx.apos + n) - exp_end;            /* + : the file's audio is ahead of the clock */
            rx.aema = rx.aema * 0.98 + e * 0.02;
            const s16 *src = c->d;
            if (rx.aema < -0.1 * RX_RATE) {                        /* 100 ms behind (the device stalled): silence */
                long long gap = (long long)(-rx.aema);
                if (ok) ok = rx_silence(gap);
                rx.aema += (double)gap; rx.ahard++;
            } else if (rx.aema > 0.1 * RX_RATE) {                  /* 100 ms ahead: this chunk out */
                rx.aema -= n; rx.ahard++; rx.askip += (u64)n; r++; continue;
            }
            /* the device's clock against the monotonic one: 1 sample in 200 repeated / left out while the file is
               more than 10 ms off (down to 3 ms) */
            if (rx.acorr == 0 && rx.aema > 0.010 * RX_RATE) rx.acorr = -1;
            else if (rx.acorr == 0 && rx.aema < -0.010 * RX_RATE) rx.acorr = 1;
            else if (rx.acorr && rx.aema > -0.003 * RX_RATE && rx.aema < 0.003 * RX_RATE) rx.acorr = 0;
            int m = n;
            if (rx.acorr) {
                m = 0;
                for (int i = 0; i < n; i++) {
                    int k = (int)((rx.apos + i) % 200);
                    if (rx.acorr < 0 && k == 0) { rx.askip++; continue; }
                    tmp[2 * m] = src[2 * i]; tmp[2 * m + 1] = src[2 * i + 1]; m++;
                    if (rx.acorr > 0 && k == 0) { tmp[2 * m] = src[2 * i]; tmp[2 * m + 1] = src[2 * i + 1]; m++; rx.adup++; }
                }
                src = tmp;
                rx.aema += (double)(m - n);
            }
            if (ok) ok = rx_write(rx.apipe, &rx.aov, src, (size_t)m * 4u);
            rx.apos += m; rx.alast_t = c->t;
            r++;
        }
        __atomic_store_n(&rx.ar, r, __ATOMIC_RELEASE);
        if (!ok) { ps2_log("rec-exact: the encoder stopped taking audio"); break; }
        /* no game audio (no device, a stall): silence up to 100 ms behind the clock, so the file goes on */
        u64 now = ps2_rx_now_ns();
        if (now > rx.t0) {
            long long want = (long long)(((double)now - (double)rx.t0) * spn) - RX_RATE / 10;
            if ((!rx.astarted || now - rx.alast_t > 200000000ull) && want > rx.apos) {
                if (!rx_silence(want - rx.apos)) break;
                rx.astarted = 1; rx.afill = 1;
            }
        }
        if (stop) {
            /* the video is closed: the audio as long as it */
            while (!rx.video_done && !rx.failed) Sleep(5);
            long long want = (long long)((double)rx.written * (double)rx.fns * spn + 0.5);
            if (want > rx.apos) rx_silence(want - rx.apos);
            break;
        }
        Sleep(10);
    }
#endif
    return NULL;
}

/* the video is complete: both inputs closed (the audio padded to the video's length), the encoder waited for */
static void rx_finish(const char *why) {
    if (__atomic_exchange_n(&rx.finished, 1, __ATOMIC_ACQ_REL)) return;
#ifdef _WIN32
    if (rx.vpipe) { FlushFileBuffers(rx.vpipe); CloseHandle(rx.vpipe); rx.vpipe = NULL; }
    if (rx.ath_ok) pthread_join(rx.ath, NULL);
    if (rx.apipe) { FlushFileBuffers(rx.apipe); CloseHandle(rx.apipe); rx.apipe = NULL; }
    rx_report(why);
    if (rx.proc) {
        DWORD r = WaitForSingleObject(rx.proc, 15000), code = 0;
        GetExitCodeProcess(rx.proc, &code);
        ps2_log("rec-exact: the encoder %s (exit code %lu); %s", r == WAIT_OBJECT_0 ? "finished" : "still runs after 15 s",
                (unsigned long)code, rx.out);
        CloseHandle(rx.proc); rx.proc = NULL;
    }
    if (rx.logh && rx.logh != INVALID_HANDLE_VALUE) { CloseHandle(rx.logh); rx.logh = NULL; }
#else
    (void)why;
#endif
}
/* the end of the session (renderer, after the device is idle and the last captures are queued) */
void ps2_rx_stop(void) {
    if (!rx.on || !rx.begun) return;
    pthread_mutex_lock(&rx.mu);
    rx.stopping = 1;
    pthread_cond_broadcast(&rx.cv);
    pthread_mutex_unlock(&rx.mu);
    if (rx.vth_ok) pthread_join(rx.vth, NULL);
    rx.video_done = 1;
    rx_finish("the end");
}
