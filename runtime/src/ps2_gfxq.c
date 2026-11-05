#include "ps2_gfxq.h"
#include "ps2_cpuprof.h"
#include "ps2_capture.h"
#include "rn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

void ps2_vif_transfer(int which, const ps2_reg128 *q, u32 qwc);
void ps2_vif_write(int which, u32 word);
void ps2_vif_clear_stall(int which);
void ps2_vif_fbrst(int which, u32 val);
void ps2_gif_transfer(const ps2_reg128 *data, u32 qwc);
void ps2_gs_priv_write_now(u32 addr, u64 val);
void ps2_gif_ctrl_write(u32 addr, u32 val);
int  ps2_gs_present_now(void);
void ps2_request_exit(void);

int ps2_gfxq_on;

enum {
    GQ_WRAP = 0,
    GQ_VIF_QW,
    GQ_GIF_QW,
    GQ_VIF_W,
    GQ_FBRST,
    GQ_VIFCLR,
    GQ_GS_PRIV,
    GQ_MMIO,
    GQ_FIELD,
    GQ_TAG,
    GQ_INTENT,
    GQ_VU1                                   /* (stage 2 of PS2_GFX_SPLIT: a VU1 record from stage 1, ps2_vu.c) */
};

static u8 *q_buf;
static size_t q_cap;
static size_t q_head, q_tail;
static int q_busy;
static int q_running;
static pthread_t q_thread;
static _Thread_local int q_is_worker;
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_not_empty = PTHREAD_COND_INITIALIZER;
static pthread_cond_t q_not_full  = PTHREAD_COND_INITIALIZER;
static pthread_cond_t q_idle      = PTHREAD_COND_INITIALIZER;

static u64 st_records, st_bytes, st_producer_waits, st_drains, st_drain_waits, st_wakes;
/* (perf6_threads 2026-11-03) stage 1's pthread_cond_wait returns (the real wake-ups behind st_wakes' signals); the EE's
   TSC ticks inside lock + signal + unlock (measured with PS2_SPEED_LOG / PS2_THREAD_PROF / PS2_CPU_AB only); the
   doorbell's rings and the other threads' flush signals */
static u64 st_s1_wakeups, st_sig_ticks, st_db_rings, st_db_flush_sig;
/* PS2_GFXQ_DOORBELL (perf6_threads): the EE publishes every record at once (a release store of q_head) and rings stage 1
   only every q_db_bytes bytes, at every field and before it blocks or polls (0 = after every record it finds stage 1
   idle, as before); producer side, switched at fields */
static u32 q_db_bytes;
static int q_db_on;                                   /* (atomic) q_db_bytes != 0, for the flushes of other threads */
static size_t q_db_mark;                              /* (producer) q_head at the last ring */
static size_t st_peak_fill;
static int q_fields_pending;
static int q_field_depth = 2;
static u64 st_field_waits;
static u64 st_field_wait_ns;

static u64 gq_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}
static u64 st_producer_ns, st_drain_ns;

static u64 speed_present_ns, speed_present_n, speed_work_ns;
static u64 speed_field_work0, speed_field_max, speed_field_long;   /* worker busy time per field (under q_lock) */
static u64 speed_work_cpu_ns;      /* the worker thread's CPU time (Windows): far below its busy wall time = preempted;
                                      equal to it with slower fields = it ran on a shared or slower core */
static u64 thread_cpu_ns(void) {
#ifdef _WIN32
    FILETIME c, e, k, u;
    if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return 0;
    return ((((u64)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((u64)u.dwHighDateTime << 32) | u.dwLowDateTime)) * 100ull;
#else
    return 0;
#endif
}
static u64 speed_vu_runs, speed_vu_insns, speed_vu_guards;
/* EE thread, once per head-tracked game frame (ps2_xr_game_frame): the game's own frame rate. The game steps its
   logic per frame, so a frame that spans two fields or more is the slowdown the player sees ("the game slows down"),
   whatever made it late: the EE itself, or the EE waiting for the graphics worker (EE wait_ms field/ring). */
static u64 sg_frames, sg_long, sg_last_ns, sg_max_ns;
int ps2_speed_enabled(void);
void ps2_speed_game_frame(void) {
    if (!ps2_speed_enabled()) return;
    u64 now = gq_now_ns();
    if (sg_last_ns && now - sg_last_ns < 250000000ull) {       /* not across a pause or a menu */
        u64 d = now - sg_last_ns;
        sg_frames++;
        if (d > 25000000ull) sg_long++;                         /* > 1.5 fields */
        if (d > sg_max_ns) sg_max_ns = d;
    }
    sg_last_ns = now;
}
void ps2_vu1_speed_counters(u64 *, u64 *, u64 *);
int ps2_speed_enabled(void) {
    static int enabled = -1;
    int value = __atomic_load_n(&enabled, __ATOMIC_RELAXED);
    if (value < 0) {
        const char *e = getenv("PS2_SPEED_LOG"); value = e && atoi(e) != 0;
        __atomic_store_n(&enabled, value, __ATOMIC_RELAXED);
    }
    return value;
}
/* ==== Host threads (op_threads, 2026-10-23): where the pipeline's threads run, and a timeline of each game field ====

   This machine (i9-12900K) has 8 P-cores with two hardware threads each (logical processors 0-15, efficiency class 1;
   12-15 are the two favoured cores) and 8 E-cores (16-23, class 0). The game's hot threads are the EE (one guest thread
   at a time; thread 0 spins in the vblank wait unless PS2_PERF_EE_IDLE), this graphics worker (VIF, native VU1
   decoders, GS primitive assembly, texture decode: 4-16 ms a field in heavy scenes) and the renderer (Vulkan recording,
   ~2.7 ms of work per 8.33 ms attempt).

   PS2_SPEED_LOG adds a `threads:` line every 5 s: per hot thread the share of its sampled run time on E-cores, on the
   favoured cores, and on a physical core another hot thread was on at the same time (the worker per record batch, the
   renderer per recording, the EE per scheduling point).

   PS2_THREAD_PROF=path (diagnostic, off): one CSV line per pipeline event (type,t_ns,a,b,c; CLOCK_MONOTONIC ns), written
   by a background thread every 0.25 s, so the path from the game's field to the GPU can be rebuilt per field
   (tools/perf/thread_timeline.py):
     1 VB  vblank posted by the timer               a = timer count
     2 EF  EE queues the field (GQ_FIELD)           a = EE field, b = ring fill in bytes (what the worker has left)
     3 EI  EE starts waiting for the next vblank    a = EE field (first idle INTC_STAT poll after the field's delivery)
     4 WB  worker starts a field's first record     a = worker field
     5 WE  worker reaches the field's end           a = worker field, b = busy ns before it, c = of it on E-cores
     6 WP  worker published the field               a = worker field, b = busy ns with the present, c = thread CPU ns
     7 RP  list published (under the list lock)     a = GS field, b = 1 if it replaced an unconsumed list
     8 RT  renderer takes the list                  a = GS field, b = renderer frame
     9 RW  renderer: xrWaitFrame returned           a = frame, b = 1 in-between frame
    10 RS  renderer: final submission returned      a = frame, b = 1 in-between frame
    11 RG  GPU done (known one frame later)         a = frame, b = recording start (after the wait), c = GPU done (ns)
    12 AB  placement A/B block change               a = block index, b = 1 in the policy block
    13 WS  worker field statistics                  a = worker field, b = record bytes, c = records
    14 S1  PS2_GFX_SPLIT: stage 1 forwarded a field a = field, b = stage 1's busy ns in it, c = of it on E-cores
    15 SP  PS2_GFX_SPLIT_AB block change            a = block index, b = 1 two stages from now on
    22 RF  renderer attempt (perf4_timeline)       t = its start, a = frame, b / c = 4 x 16-bit microseconds each:
           b = fence | collect + window acquire << 16 | before the wait << 32 | xrWaitFrame + xrBeginFrame << 48,
           c = recording | vkQueueSubmit << 16 | xrEndFrame << 32 | the rest (pace, dumps, window present) << 48
    23 GG  GPU start (perf4_timeline)              a = frame, b = its first GPU timestamp on the host clock (ns),
           c = GPU busy ns (without early-submission gaps); known a few frames later, like event 11
    24 CA  PS2_VK_CENSUS_AB block change           a = block index, b = 1 counted (ps2_vkcen.c)
    30 T4  PS2_PERF_TRX_T4_AB block change         a = block index, b = 1 four-row PSMT4 path (ps2_gs.c, perf4_gfx)
    31 LZ  PS2_PERF_TEX_LAZY_AB block change       a = block index, b = 1 lazy direct decodes (ps2_gs.c, perf4_gfx)
    40 VL  the polling EE sees a vblank            a = field, b = ns since the clock posted it, c = 1 when the EE was
           blocked in the idle wait (ps2_kernel.c, perf6_threads; with PS2_CPU_AB, event 16 marks the blocks:
           tools/perf/tpol_ab.py LOG TP.csv)
   (tools/perf/frame_timeline.py joins them with PS2_GPU_TRACE and PS2_XR_PACE_CSV into one row per frame)
   (with PS2_GFX_SPLIT, events 4-6 and 13 come from stage 2, the stage that publishes)

   PS2_THREAD_CORES=p: the hot threads (every EE guest thread, this worker and its stage 2, the renderer) prefer the
   fastest cores (SetThreadSelectedCpuSets with the P-cores' CPU sets: a preference, not a hard affinity, on Windows 11;
   measured unneeded: Windows keeps the hot threads on P-cores, 0-2 % of their run time on E-cores);
   0 / unset = Windows' own placement; `e` (test): the graphics worker (both stages with PS2_GFX_SPLIT) only on the
   efficiency cores (a hard thread affinity), the others as Windows places them: a ~1.45x slower worker, as in the
   campaign session's heaviest windows. PS2_THREAD_CORES_AB=S: in-run A/B, the policy in alternate blocks of S seconds
   (odd blocks), Windows' placement in the others (event 12 marks the blocks).

   PS2_THREAD_CORES=h (ov_cpu 2026-10-25; for a busy machine): when other programs keep the P-cores busy (agent builds
   at idle priority on 2026-10-25 00:13-00:22: the worker and the renderer ran 98-100 % on E-cores for whole runs and
   the game ran ~2x slower; the user's OBS now encodes x264 on the CPU), Windows wakes our blocking threads (worker,
   renderer) on an idle E-core rather than preempting a lower-priority thread on a P-core. Hard placement: the hot
   threads (EE guest threads, the worker and its stage 2, the renderer, the texture decode pool's helpers) get a hard
   affinity to one logical processor of each P-core and High QoS (thread power throttling off), so a waking hot thread
   preempts whatever lower-priority work runs there and two hot threads never share a physical core (exclusive M4 run
   00:43 under agent builds: Windows ran the EE on processor 0 and the worker on its SMT sibling 1, the renderer on 2,
   favoured cores 0 %, a hot thread shared its core 21-38 % of the time: EE work 11.8 ms a field instead of ~4.5,
   the worker 12.2 instead of ~6, the renderer's recording 3.9 instead of ~1.3 -- E-cores 1-6 % only); the background
   workers (terrain bake / materials, water mask,
   texture pack, props, canopy cracks: already lowest priority) get the E-cores and EcoQoS. With _AB the hot threads
   switch per block; one-off background workers keep the placement of the block they started in.
   But under a saturating load (16 idle-priority burner threads, exclusive M4, in-run A/B) h was worse: EE work per
   field 5.94 -> 7.10 ms (+1.16 +- 0.08), the worker 7.71 -> 8.06 (+0.36 +- 0.12): every even processor's sibling ran a
   burner, while Windows had put the worker 21-27 % and the renderer 24-32 % of the time on (unshared) E-cores.
   PS2_THREAD_CORES=s ("separate"): the hot threads on one logical processor per P-core OR any E-core -- two of them
   never share a physical core, and Windows can still use an E-core when the P-cores are taken. */
enum { HT_EE, HT_GFX, HT_VK, HT_GFX2, HT_N };
#define HT_LP 64
static struct {
    int init, n, pmax;                               /* logical processors (group 0), highest efficiency class */
    u8 eff[HT_LP], core[HT_LP], sched[HT_LP];
    unsigned long id[HT_LP];
    unsigned long hot_ids[HT_LP]; int hot_n;         /* the policy's CPU sets for the hot threads */
    int policy, ab_s;                                /* PS2_THREAD_CORES, PS2_THREAD_CORES_AB seconds */
    u64 pmask, emask;                                /* logical processors of the P-cores / the E-cores (policy h) */
} ht;
int ps2_host_thread_sampling;                        /* PS2_SPEED_LOG, PS2_THREAD_PROF or the A/B: hot threads sample */
static _Thread_local int ht_role = -1, ht_applied_block = -1;
static _Thread_local int ht_class = -1;              /* 0 hot thread, 1 background worker (placement), -1 neither */
static u64 ht_ns[HT_N][HT_LP];                       /* sampled run time per logical processor (written by the role) */
static u64 ht_share_ns[HT_N];                        /* ... of it while another hot thread was on the same core */
static volatile int ht_cur[HT_N];                    /* logical processor + 1 the role runs on now, 0 idle */

static u64 ht_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

static void ht_topology(void) {
    if (ht.init) return;
    ht.init = 1;
#ifdef _WIN32
    ULONG len = 0;
    GetSystemCpuSetInformation(NULL, 0, &len, GetCurrentProcess(), 0);
    char *buf = len ? (char *)malloc(len) : NULL;
    if (buf && GetSystemCpuSetInformation((PSYSTEM_CPU_SET_INFORMATION)(void *)buf, len, &len, GetCurrentProcess(), 0)) {
        for (ULONG o = 0; o + 8u <= len; ) {
            SYSTEM_CPU_SET_INFORMATION *s = (SYSTEM_CPU_SET_INFORMATION *)(void *)(buf + o);
            if (!s->Size) break;
            if (s->Type == CpuSetInformation && s->CpuSet.Group == 0 && s->CpuSet.LogicalProcessorIndex < HT_LP) {
                u32 lp = s->CpuSet.LogicalProcessorIndex;
                ht.eff[lp] = s->CpuSet.EfficiencyClass; ht.core[lp] = s->CpuSet.CoreIndex;
                ht.sched[lp] = s->CpuSet.SchedulingClass; ht.id[lp] = s->CpuSet.Id;
                if ((int)lp + 1 > ht.n) ht.n = (int)lp + 1;
                if (s->CpuSet.EfficiencyClass > ht.pmax) ht.pmax = s->CpuSet.EfficiencyClass;
            }
            o += s->Size;
        }
    }
    free(buf);
#endif
}

/* ==== Spin-free thread policy by core count (perf6_threads 2026-11-03; docs/research_2026-11-03_perf6 #1, topic 19) ====
   On a PC with few cores the game's waiting burns cores: the EE spins in the game's vblank wait (~16.5 ms of CPU a
   field for 3-6 ms of work), the three texture decode pool helpers spin 300 us after every job (1.9-4.2 ms a field
   for 0.25-0.44 ms of decoding), and the EE wakes graphics stage 1 after every record that finds it idle (400-1,750
   times a field). With 4-6 cores and Link beside the game the spinning threads share physical cores with the hot ones
   (EE, both graphics stages, the renderer), which then run 1.6-2.6x slower (ov_cpu).
   P = the physical cores of the fastest efficiency class this process may run on (its affinity mask; on a hybrid CPU
   the P-cores only: 12600K / 13400 / 12700H count 6, this 12900K 8; E-cores never raise P).
   PS2_THREAD_POLICY=auto (default) applies the policy when P <= 6 (PS2_THREAD_POLICY_CORES), `1` always, `0` never:
     - the EE's vblank polling blocks on an event (as PS2_PERF_EE_IDLE=1),
     - clamp(P - 4, 0, 3) texture decode pool helpers (PS2_PERF_TEX_POOL) that spin 50 us (PS2_PERF_TEX_POOL_SPIN),
     - the EE publishes every record at once but rings stage 1 every 64 KB (PS2_GFXQ_DOORBELL), at every field and
       before it blocks or polls (ring-full wait, drain, field-depth wait, guest thread switch, INTC_STAT polling);
     - both graphics stages, the renderer and the pool helpers run one priority level above the EE
       (PS2_THREAD_POLICY_PRIO): an EE that sleeps wakes at the vblank onto whatever core is free and works for
       5-8 ms; with all cores taken stage 1, woken by that field, then queued behind it at equal priority and the
       list came out up to 6 ms late (4 E-cores: "EE queues -> stage 1 done" p99 0.11 -> 5.6 ms);
   each of those envs, when set, wins over the policy. Thread placement stays Windows' own (PS2_THREAD_CORES=s was
   measured neutral or worse). Nothing the guest or the GPU sees changes (the same record stream, the same decodes).
   PS2_CPU_LIMIT=spec (test; a weak-CPU proxy): the whole process (game, renderer, driver and OpenXR runtime threads)
   runs on those logical processors only (SetProcessAffinityMask on this process): `eN` the first N E-cores, `pN` the
   first N P-cores with both hardware threads, `lN` logical processors 0..N-1, `0x...` a mask; P is counted on it.
   PS2_CPU_AB=S with `policy` (or `idle`, `pool`, `bell`, `prio`) in PS2_CPU_AB_FLIP: the policy's values in the B blocks, the
   old ones in the A blocks, switched at fields; a `cpu-ab:` log line per block with each thread role's CPU. */
int ps2_cpu_ab_has(const char *name);
int ps2_cpu_ab_b(void);
static void cpu_limit_apply(void) {
#ifdef _WIN32
    const char *e = getenv("PS2_CPU_LIMIT");
    if (!e || !*e || !strcmp(e, "0")) return;
    u64 m = 0;
    int n = atoi(e + 1), got = 0;
    if (e[0] == '0' && (e[1] == 'x' || e[1] == 'X')) m = strtoull(e + 2, NULL, 16);
    else if (e[0] == 'e' || e[0] == 'E') {
        for (int lp = 0; lp < ht.n && lp < 64 && got < n; lp++)
            if (ht.eff[lp] != ht.pmax) { m |= 1ull << lp; got++; }
    } else if (e[0] == 'p' || e[0] == 'P') {
        u8 seen[256];
        memset(seen, 0, sizeof seen);
        for (int lp = 0; lp < ht.n && lp < 64; lp++) {
            if (ht.eff[lp] != ht.pmax) continue;
            if (!seen[ht.core[lp]]) {
                if (got >= n) continue;
                seen[ht.core[lp]] = 1;
                got++;
            }
            m |= 1ull << lp;
        }
    } else if (e[0] == 'l' || e[0] == 'L') {
        for (int lp = 0; lp < n && lp < 64; lp++) m |= 1ull << lp;
    }
    DWORD_PTR pm = 0, sm = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm);
    m &= (u64)sm;
    int ok = m && SetProcessAffinityMask(GetCurrentProcess(), (DWORD_PTR)m);
    ps2_log("threads: PS2_CPU_LIMIT=%s (weak-CPU proxy, test): %s logical processors %llX (%d of them)", e,
            ok ? "the process runs on" : "could not restrict the process to", (unsigned long long)m,
            __builtin_popcountll(m));
#endif
}
/* thread CPU per role (perf6_threads): the hot threads and the pool helpers register a handle when they start (with
   PS2_SPEED_LOG, PS2_THREAD_PROF or PS2_CPU_AB); the `threads-cpu:` lines (every 5 s) and the `cpu-ab:` block lines
   sum QueryThreadCycleTime per role (every EE guest thread together) */
enum { TC_EE, TC_GFX, TC_GFX2, TC_VK, TC_POOL, TC_N };
#define TC_MAX 64
#ifdef _WIN32
static HANDLE tc_h[TC_MAX];
#endif
static u8 tc_role[TC_MAX];
static int tc_n;
static pthread_mutex_t tc_mx = PTHREAD_MUTEX_INITIALIZER;
static void tc_register(int role) {
#ifdef _WIN32
    HANDLE h;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h,
                         THREAD_QUERY_LIMITED_INFORMATION | THREAD_SET_LIMITED_INFORMATION, FALSE, 0)) return;
    pthread_mutex_lock(&tc_mx);
    if (tc_n < TC_MAX) {
        tc_h[tc_n] = h;
        tc_role[tc_n] = (u8)role;
        __atomic_store_n(&tc_n, tc_n + 1, __ATOMIC_RELEASE);
    } else CloseHandle(h);
    pthread_mutex_unlock(&tc_mx);
#else
    (void)role;
#endif
}
/* the thread cycles per role so far (cnt: threads per role); returns the process's CPU ns (GetProcessTimes) */
static u64 tc_snapshot(u64 cyc[TC_N], int cnt[TC_N]) {
    u64 proc = 0;
    memset(cyc, 0, sizeof(u64) * TC_N);
    memset(cnt, 0, sizeof(int) * TC_N);
#ifdef _WIN32
    typedef BOOL (WINAPI *qtct_fn)(HANDLE, PULONG64);
    static qtct_fn fn;
    static int init;
    if (!init) {
        init = 1;
        fn = (qtct_fn)(void (*)(void))GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "QueryThreadCycleTime");
    }
    int n = __atomic_load_n(&tc_n, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++) {
        ULONG64 c = 0;
        if (fn && fn(tc_h[i], &c)) cyc[tc_role[i]] += (u64)c;
        cnt[tc_role[i]]++;
    }
    FILETIME a, b, k, u;
    if (GetProcessTimes(GetCurrentProcess(), &a, &b, &k, &u))
        proc = ((((u64)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((u64)u.dwHighDateTime << 32) | u.dwLowDateTime)) * 100ull;
#endif
    return proc;
}
static struct {
    int mode, cores, P, ne, lps, hybrid, on, helpers, spin_us, prio;
    u32 db;
} tpol;
int ps2_tpol_now;                                     /* the policy's state now: the static decision, or the PS2_CPU_AB
                                                         block's (set per field by the EE; the idle wait and the pool) */
static int tpol_ab;                                   /* switched by PS2_CPU_AB blocks: 1 idle, 2 pool, 4 bell, 8 prio */
static void tpol_init(void) {
    const char *e = getenv("PS2_THREAD_POLICY");
    tpol.mode = !e || !*e || !strcmp(e, "auto") ? -1 : atoi(e) != 0;
    e = getenv("PS2_THREAD_POLICY_CORES");
    tpol.cores = e && *e ? atoi(e) : 6;
    u64 allowed = ~0ull;
#ifdef _WIN32
    {   DWORD_PTR pm = 0, sm = 0;
        if (GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm) && pm) allowed = (u64)pm;
    }
#endif
    int emax = -1, emin = 1 << 20;
    for (int lp = 0; lp < ht.n && lp < 64; lp++)
        if (allowed >> lp & 1ull) {
            tpol.lps++;
            if (ht.eff[lp] > emax) emax = ht.eff[lp];
            if (ht.eff[lp] < emin) emin = ht.eff[lp];
        }
    {   u8 seen[256];
        memset(seen, 0, sizeof seen);
        for (int lp = 0; lp < ht.n && lp < 64; lp++)
            if ((allowed >> lp & 1ull) && !seen[ht.core[lp]]) {
                seen[ht.core[lp]] = 1;
                if (ht.eff[lp] == emax) tpol.P++; else tpol.ne++;
            }
    }
    tpol.hybrid = tpol.lps && emax > emin;
    tpol.on = tpol.mode >= 0 ? tpol.mode : tpol.P > 0 && tpol.P <= tpol.cores;
    tpol.helpers = tpol.P <= 4 ? 0 : tpol.P >= 7 ? 3 : tpol.P - 4;   /* clamp(P - 4, 0, 3) */
    tpol.spin_us = 50;
    e = getenv("PS2_GFXQ_DOORBELL");
    tpol.db = e && *e && atoi(e) > 0 ? (u32)atoi(e) : 65536u;
    e = getenv("PS2_THREAD_POLICY_PRIO");
    tpol.prio = !e || !*e || atoi(e) != 0;
    tpol_ab = (ps2_cpu_ab_has("policy") ? (tpol.prio ? 15 : 7) : 0) | (ps2_cpu_ab_has("idle") ? 1 : 0)
            | (ps2_cpu_ab_has("pool") ? 2 : 0) | (ps2_cpu_ab_has("bell") ? 4 : 0) | (ps2_cpu_ab_has("prio") ? 8 : 0);
    __atomic_store_n(&ps2_tpol_now, tpol_ab ? 0 : tpol.on, __ATOMIC_RELEASE);
    {   extern double ps2_host_tsc_per_ns(void);
        ps2_host_tsc_per_ns();                        /* (starts the TSC calibration of the CPU lines) */
    }
    ps2_log("threads: spin-free thread policy %s (PS2_THREAD_POLICY=%s, auto threshold P <= %d): %d logical processors "
            "allowed, P = %d physical cores of the fastest class, %d slower cores%s -- %s%s", tpol.on ? "ON" : "off",
            tpol.mode < 0 ? "auto" : tpol.mode ? "1" : "0", tpol.cores, tpol.lps, tpol.P, tpol.ne,
            tpol.hybrid ? " (hybrid)" : "",
            tpol.on ? "the EE's vblank wait blocks, clamp(P - 4, 0, 3) pool helpers spin 50 us, the EE rings stage 1 in "
                      "batches, the graphics stages, renderer and pool one priority level above the EE (each of "
                      "PS2_PERF_EE_IDLE, PS2_PERF_TEX_POOL, PS2_PERF_TEX_POOL_SPIN, PS2_GFXQ_DOORBELL, "
                      "PS2_THREAD_POLICY_PRIO wins when set)"
                    : "as before: the EE's vblank wait spins, 3 pool helpers spin 300 us, a ring per record",
            tpol_ab ? "; in-run A/B: the policy's values in the B blocks of PS2_CPU_AB" : "");
}

static int ht_lp_now(void) {
#ifdef _WIN32
    PROCESSOR_NUMBER pn;
    GetCurrentProcessorNumberEx(&pn);
    return pn.Group == 0 && pn.Number < HT_LP ? (int)pn.Number : 0;
#else
    return 0;
#endif
}

/* ---- the timeline ---- */
typedef struct { u64 t, b, c; u32 type, a; } tp_ev;
#define TP_N (1u << 20)
static tp_ev *tp_buf;
static u32 tp_w;                                     /* next slot (atomic) */
static int tp_on = -1;
static FILE *tp_f;
static u32 tp_pub_field;                             /* the published list's GS field (under the renderer's list lock) */
int ps2_tprof_on(void);
void ps2_tprof_ev_at(u32 type, u64 t, u32 a, u64 b, u64 c) {
    if (!ps2_tprof_on()) return;
    u32 i = __atomic_fetch_add(&tp_w, 1u, __ATOMIC_RELAXED) & (TP_N - 1u);
    tp_ev *e = &tp_buf[i];
    if (__atomic_load_n(&e->type, __ATOMIC_ACQUIRE)) return;      /* the writer is a whole ring behind: drop */
    e->t = t; e->a = a; e->b = b; e->c = c;
    __atomic_store_n(&e->type, type, __ATOMIC_RELEASE);
}
void ps2_tprof_ev(u32 type, u32 a, u64 b, u64 c) {
    if (!ps2_tprof_on()) return;
    ps2_tprof_ev_at(type, ht_now_ns(), a, b, c);
}
static void *tp_writer(void *unused) {
    (void)unused;
    u32 r = 0;
    for (;;) {
        struct timespec ts = { 0, 250000000L };
        nanosleep(&ts, NULL);
        for (;;) {
            tp_ev *e = &tp_buf[r & (TP_N - 1u)];
            u32 type = __atomic_load_n(&e->type, __ATOMIC_ACQUIRE);
            if (!type) break;
            fprintf(tp_f, "%u,%llu,%u,%llu,%llu\n", type, (unsigned long long)e->t, e->a,
                    (unsigned long long)e->b, (unsigned long long)e->c);
            __atomic_store_n(&e->type, 0u, __ATOMIC_RELEASE);
            r++;
        }
        fflush(tp_f);
    }
    return NULL;
}
int ps2_tprof_on(void) {
    int v = __atomic_load_n(&tp_on, __ATOMIC_ACQUIRE);
    if (v >= 0) return v;
    static pthread_mutex_t once = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&once);
    if (tp_on < 0) {
        const char *e = getenv("PS2_THREAD_PROF");
        int on = 0;
        if (e && *e && strcmp(e, "0") != 0) {
            tp_buf = (tp_ev *)calloc(TP_N, sizeof(tp_ev));
            tp_f = tp_buf ? fopen(e, "w") : NULL;
            if (tp_f) {
                pthread_t th;
                fprintf(tp_f, "type,t_ns,a,b,c\n");
                on = pthread_create(&th, NULL, tp_writer, NULL) == 0;
                if (on) pthread_detach(th);
            }
            ps2_log("threads: pipeline timeline (PS2_THREAD_PROF) %s %s", on ? "into" : "could not open", e);
        }
        __atomic_store_n(&tp_on, on, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&once);
    return tp_on;
}

/* ---- CPU zones (PS2_CPU_PROF; ov_cpu 2026-10-25; ps2_cpuprof.h) ----
   Per zone and 5 s window: instances, total and largest duration, instances over the spike threshold and a histogram
   in quarter octaves of TSC ticks / 16 (p50 / p99). Atomic adds on the zone's own cache line (each zone is one
   thread's, so they do not contend). The window goes into the log with the speed lines (`cpu:`), the largest
   instances over the threshold as `cpu: spike` lines, and with PS2_THREAD_PROF every instance of at least
   PS2_CPU_PROF_TL_US (default 20) as a timeline event 20 (a = zone, b = duration ns, c = the host thread id, t = start);
   the zone names go into <PS2_THREAD_PROF>.zones. */
int ps2_cz_on;
#define CZ_HB 160
typedef struct {
    u64 n, sum, max, max_t, over;
    u32 h[CZ_HB];
} __attribute__((aligned(64))) cz_acc;
static cz_acc *cz_w;                                 /* [CZ_MAX] (allocated when on) */
static char (*cz_nm)[72];
static int cz_n = CZ_N_STATIC, cz_named;             /* zones in use; names written to the .zones file */
static u64 cz_spike_t, cz_tl_t;                      /* thresholds in TSC ticks */
static u64 *cz_over_t;                               /* [CZ_MAX] a zone's spike threshold (the field for per-field
                                                        zones, never for waits) */
static double cz_tick_ns = 3.0;                      /* TSC ticks per ns (calibrated at init, refined each window) */
static u64 cz_tsc0, cz_mono0;
static const char *cz_static_nm[CZ_N_STATIC] = {
    "vk pump events (SDL, pad, rumble, Touch)", "vk   of it: Touch sync (ps2_touch_update)", "vk fence + GPU times",
    "vk prefill (before the wait)", "vk preupload (cmd begin + uploads before the wait)", "vk xrWaitFrame + xrBeginFrame",
    "vk recording after the wait", "vk   of it: render_list", "vk early recording (before the wait)",
    "vk final submit + xrEndFrame", "vk window present", "vk pipeline warm-up step", "vk pipeline cache save",
    "vk idle (waiting for a list)", "vk whole attempt (draw_and_present)",
    "gfx worker busy per field", "gfx   present (display list hand-off)", "gfx2 stage 2 busy per field",
    "ee work per field (vblank -> idle poll)",
    "aud callback", "aud   game mix (SPU2 + streams)", "aud   immersion layer", "aud   cockpit system sounds",
    "crk worker step (every 4 ms; builds crack patterns)"
};
static inline int cz_bucket(u64 v) {
    if (v < 4u) return (int)v;
    int e = 63 - __builtin_clzll(v);
    int b = e * 4 + (int)((v >> (e - 2)) & 3u);
    return b < CZ_HB ? b : CZ_HB - 1;
}
static double cz_bucket_ticks(int b) {                /* the bucket's middle, in ticks */
    if (b < 4) return (double)b * 16.0 + 8.0;
    int e = b / 4, f = b & 3;
    double lo = (double)((u64)(4 + f) << (e - 2)), w = (double)(1ull << (e - 2));
    return (lo + 0.5 * w) * 16.0;
}
void ps2_cz_init(void) {
    static int done;
    if (done) return;
    done = 1;
    const char *e = getenv("PS2_CPU_PROF");
    if (!e || !*e || *e == '0') return;
    cz_w = (cz_acc *)calloc(CZ_MAX, sizeof(cz_acc));
    cz_nm = (char (*)[72])calloc(CZ_MAX, 72);
    if (!cz_w || !cz_nm) return;
    for (int i = 0; i < CZ_N_STATIC; i++) snprintf(cz_nm[i], 72, "%s", cz_static_nm[i]);
    {   /* TSC rate: 2 ms against CLOCK_MONOTONIC (refined at every window) */
        u64 m0 = gq_now_ns(), t0 = (u64)__rdtsc(), m1;
        do m1 = gq_now_ns(); while (m1 - m0 < 2000000ull);
        u64 t1 = (u64)__rdtsc();
        cz_tick_ns = (double)(t1 - t0) / (double)(m1 - m0);
        cz_tsc0 = t0; cz_mono0 = m0;
    }
    const char *s = getenv("PS2_CPU_PROF_SPIKE_US");
    double sp = s && *s ? atof(s) : 1000.0;
    s = getenv("PS2_CPU_PROF_TL_US");
    double tl = s && *s ? atof(s) : 20.0;
    cz_spike_t = (u64)(sp * 1000.0 * cz_tick_ns);
    cz_tl_t = (u64)(tl * 1000.0 * cz_tick_ns);
    cz_over_t = (u64 *)malloc(CZ_MAX * sizeof(u64));
    if (!cz_over_t) return;
    for (int i = 0; i < CZ_MAX; i++) cz_over_t[i] = cz_spike_t;
    cz_over_t[CZ_GFX_FIELD] = cz_over_t[CZ_GFX2_FIELD] = cz_over_t[CZ_EE_FIELD] = (u64)(16.667e6 * cz_tick_ns);
    cz_over_t[CZ_VK_XRWAIT] = cz_over_t[CZ_VK_IDLE] = cz_over_t[CZ_VK_FENCE] = cz_over_t[CZ_VK_FRAME] = ~0ull;
    __atomic_store_n(&ps2_cz_on, 1, __ATOMIC_RELEASE);
    ps2_log("cpu: CPU zones on (PS2_CPU_PROF): TSC %.4f ticks/ns, spikes over %.0f us, timeline from %.0f us%s", cz_tick_ns,
            sp, tl, ps2_tprof_on() ? " (PS2_THREAD_PROF)" : "");
}
int ps2_cz_dynamic(const char *name) {
    if (!ps2_cz_on || !cz_nm) return -1;
    int z = __atomic_fetch_add(&cz_n, 1, __ATOMIC_RELAXED);
    if (z >= CZ_MAX) { __atomic_store_n(&cz_n, CZ_MAX, __ATOMIC_RELAXED); return -1; }
    snprintf(cz_nm[z], 72, "%s", name);
    return z;
}
void ps2_cz_add(int z, u64 t0, u64 t1) {
    if (!cz_w || z < 0 || z >= CZ_MAX || t1 <= t0) return;
    cz_acc *a = &cz_w[z];
    u64 d = t1 - t0;
    __atomic_fetch_add(&a->n, 1u, __ATOMIC_RELAXED);
    __atomic_fetch_add(&a->sum, d, __ATOMIC_RELAXED);
    __atomic_fetch_add(&a->h[cz_bucket(d >> 4)], 1u, __ATOMIC_RELAXED);
    u64 m = __atomic_load_n(&a->max, __ATOMIC_RELAXED);
    while (d > m && !__atomic_compare_exchange_n(&a->max, &m, d, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
    if (d > m) __atomic_store_n(&a->max_t, t0, __ATOMIC_RELAXED);
    if (d >= cz_over_t[z]) __atomic_fetch_add(&a->over, 1u, __ATOMIC_RELAXED);
    if (d >= cz_tl_t && ps2_tprof_on()) {
        u64 st = cz_mono0 + (u64)((double)(t0 - cz_tsc0) / cz_tick_ns);
#ifdef _WIN32
        u64 tid = (u64)GetCurrentThreadId();
#else
        u64 tid = 0;
#endif
        ps2_tprof_ev_at(20, st, (u32)z, (u64)((double)d / cz_tick_ns), tid);
    }
}
/* the window: one line per zone with instances (static zones in their order, then the 16 costliest guest hooks), the
   largest instance over the threshold per zone as a spike line; then a new window */
static void cz_report(u64 fields) {
    if (!ps2_cz_on || !cz_w) return;
    {   u64 m = gq_now_ns(), t = (u64)__rdtsc();     /* refine the TSC rate over the run so far */
        if (m > cz_mono0 + 1000000000ull) cz_tick_ns = (double)(t - cz_tsc0) / (double)(m - cz_mono0);
    }
    double nf = fields ? (double)fields : 1.0, tpm = cz_tick_ns * 1e6, tpu = cz_tick_ns * 1e3;
    int n = __atomic_load_n(&cz_n, __ATOMIC_RELAXED);
    if (n > CZ_MAX) n = CZ_MAX;
    static cz_acc snap[CZ_MAX];
    for (int z = 0; z < n; z++) {                     /* take the window (racy by an instance or two: fine) */
        cz_acc *a = &cz_w[z];
        snap[z].n = __atomic_exchange_n(&a->n, 0u, __ATOMIC_RELAXED);
        snap[z].sum = __atomic_exchange_n(&a->sum, 0u, __ATOMIC_RELAXED);
        snap[z].max = __atomic_exchange_n(&a->max, 0u, __ATOMIC_RELAXED);
        snap[z].max_t = a->max_t;
        snap[z].over = __atomic_exchange_n(&a->over, 0u, __ATOMIC_RELAXED);
        for (int b = 0; b < CZ_HB; b++) snap[z].h[b] = __atomic_exchange_n(&a->h[b], 0u, __ATOMIC_RELAXED);
    }
    int order[CZ_MAX], no = 0;
    for (int z = 0; z < CZ_N_STATIC && z < n; z++) if (snap[z].n) order[no++] = z;
    {   int dyn[CZ_MAX], nd = 0;                      /* guest hooks: the 16 costliest of the window */
        for (int z = CZ_N_STATIC; z < n; z++) if (snap[z].n) dyn[nd++] = z;
        for (int i = 0; i < nd && i < 16; i++) {
            int best = i;
            for (int j = i + 1; j < nd; j++) if (snap[dyn[j]].sum > snap[dyn[best]].sum) best = j;
            int t = dyn[i]; dyn[i] = dyn[best]; dyn[best] = t;
            order[no++] = dyn[i];
        }
        if (nd > 16) {
            double rest = 0.0; u64 rn = 0;
            for (int i = 16; i < nd; i++) { rest += (double)snap[dyn[i]].sum; rn += snap[dyn[i]].n; }
            ps2_log("cpu: (%d more guest hooks: %.3f ms/field in %.1f calls/field)", nd - 16, rest / tpm / nf, (double)rn / nf);
        }
    }
    for (int i = 0; i < no; i++) {
        int z = order[i];
        cz_acc *a = &snap[z];
        u64 c50 = (a->n + 1) / 2, c99 = a->n - a->n / 100, acc = 0;
        double p50 = 0.0, p99 = 0.0;
        for (int b = 0; b < CZ_HB; b++) {
            acc += a->h[b];
            if (!p50 && acc >= c50) p50 = cz_bucket_ticks(b);
            if (acc >= c99) { p99 = cz_bucket_ticks(b); break; }
        }
        ps2_log("cpu: %-52s %8.2f/field %8.3f ms/field  mean %8.1f us  p50 %8.1f  p99 %8.1f  max %7.3f ms  over %llu",
                cz_nm[z], (double)a->n / nf, (double)a->sum / tpm / nf, (double)a->sum / tpu / (double)a->n, p50 / tpu,
                p99 / tpu, (double)a->max / tpm, (unsigned long long)a->over);
    }
    for (int i = 0; i < no; i++) {                    /* the largest instance of each zone with spikes */
        int z = order[i];
        if (!snap[z].over) continue;
        double at = (double)(snap[z].max_t - cz_tsc0) / cz_tick_ns / 1e9;
        ps2_log("cpu: spike %s: %llu over %.1f ms, largest %.3f ms at %.3f s", cz_nm[z],
                (unsigned long long)snap[z].over, (double)cz_over_t[z] / tpm, (double)snap[z].max / tpm, at);
    }
    if (ps2_tprof_on() && cz_named != n) {           /* the zone names beside the timeline */
        const char *e = getenv("PS2_THREAD_PROF");
        char p[600];
        snprintf(p, sizeof p, "%s.zones", e);
        FILE *f = fopen(p, "w");
        if (f) {
            fprintf(f, "tsc_ticks_per_ns %.6f\n", cz_tick_ns);
            for (int z = 0; z < n; z++) fprintf(f, "%d,%s\n", z, cz_nm[z]);
            fclose(f);
            cz_named = n;
        }
    }
}

/* ---- PS2_CPU_AB=S, PS2_CPU_AB_FLIP=name,... (ov_cpu 2026-10-25): in-run A/B of ov_cpu's switches -- the named ones
   take their new behaviour in the odd ("B") blocks of S seconds only (`pause`: PS2_PERF_EE_PAUSE). The first caller
   that sees a new block puts a timeline event 16 (a = block, b = 1 in a B block) and a log line. ---- */
static int cab_s = -1;
static char cab_flip[160];
static void cab_init(void) {
    static pthread_mutex_t once = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&once);
    if (cab_s < 0) {
        const char *e = getenv("PS2_CPU_AB");
        int s = e && *e ? atoi(e) : 0;
        e = getenv("PS2_CPU_AB_FLIP");
        snprintf(cab_flip, sizeof cab_flip, ",%s,", e ? e : "");
        if (s > 0) ps2_log("cpu: in-run A/B (PS2_CPU_AB): blocks of %d s, the B blocks switch on: %s", s, e ? e : "(none)");
        __atomic_store_n(&cab_s, s > 0 ? s : 0, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&once);
}
int ps2_cpu_ab_has(const char *name) {
    if (__atomic_load_n(&cab_s, __ATOMIC_ACQUIRE) < 0) cab_init();
    char k[40];
    snprintf(k, sizeof k, ",%s,", name);
    return cab_s > 0 && strstr(cab_flip, k) != NULL;
}
int ps2_cpu_ab_b(void) {
    if (__atomic_load_n(&cab_s, __ATOMIC_ACQUIRE) < 0) cab_init();
    if (cab_s <= 0) return 0;
    static u64 last = ~0ull;
    u64 blk = gq_now_ns() / (1000000000ull * (u64)cab_s);
    if (__atomic_exchange_n(&last, blk, __ATOMIC_RELAXED) != blk) ps2_tprof_ev(16, (u32)blk, blk & 1u, 0);
    return (int)(blk & 1u);
}

/* ---- placement and core sampling ---- */
static void ht_policy_init(void) {
    static pthread_mutex_t once = PTHREAD_MUTEX_INITIALIZER;
    static int done;
    pthread_mutex_lock(&once);
    if (done) { pthread_mutex_unlock(&once); return; }
    done = 1;
    ht_topology();
    cpu_limit_apply();                                /* PS2_CPU_LIMIT (perf6_threads; test) */
    const char *e = getenv("PS2_THREAD_CORES");
    ht.policy = e && (*e == 'p' || *e == 'P') ? 1 : e && (*e == 'e' || *e == 'E') ? 2 : e && (*e == 'h' || *e == 'H') ? 3
              : e && (*e == 's' || *e == 'S') ? 4 : 0;
    e = getenv("PS2_THREAD_CORES_AB");
    ht.ab_s = e ? atoi(e) : 0;
    if (ht.ab_s < 0 || !ht.policy) ht.ab_s = 0;
    ht.hot_n = 0;
    for (int lp = 0; lp < ht.n; lp++)                   /* the P-cores (policy p) or the E-cores (test policy e) */
        if ((ht.policy == 1 && ht.eff[lp] == ht.pmax) || (ht.policy == 2 && ht.eff[lp] != ht.pmax))
            ht.hot_ids[ht.hot_n++] = ht.id[lp];
    ht.pmask = ht.emask = 0;
    for (int lp = 0; lp < ht.n && lp < 64; lp++) {          /* (policy h: one logical processor per P-core) */
        int first = 1;
        for (int o = 0; o < lp; o++) if (ht.core[o] == ht.core[lp] && ht.eff[o] == ht.eff[lp]) first = 0;
        if (ht.eff[lp] == ht.pmax) { if (first) ht.pmask |= 1ull << lp; } else ht.emask |= 1ull << lp;
    }
    if (ht.policy >= 3) ht.hot_n = ht.pmask && ht.emask ? 1 : 0;
    if (ht.policy && (!ht.hot_n || ht.hot_n == ht.n)) ht.policy = ht.ab_s = 0;   /* all cores alike: nothing to choose */
    ps2_host_thread_sampling = ps2_speed_enabled() || ps2_tprof_on() || ht.ab_s > 0;
    {   int np = 0;
        for (int lp = 0; lp < ht.n; lp++) np += ht.eff[lp] == ht.pmax;
        ps2_log("threads: %d logical processors, %d of them on the fastest cores; hot threads placed %s%s", ht.n, np,
                ht.policy == 1 ? "on the fastest cores (PS2_THREAD_CORES=p)" : ht.policy == 2
                ? "by Windows, the graphics worker on the efficiency cores (test: PS2_THREAD_CORES=e)"
                : ht.policy == 3 ? "on the fastest cores only, High QoS; background workers on the efficiency cores, "
                  "EcoQoS (PS2_THREAD_CORES=h)"
                : ht.policy == 4 ? "on separate physical cores (one logical processor per P-core, or an E-core), High "
                  "QoS; background workers on the efficiency cores, EcoQoS (PS2_THREAD_CORES=s)"
                : "by Windows (PS2_THREAD_CORES=p: fastest cores)",
                ht.ab_s > 0 ? ", in-run A/B in blocks (PS2_THREAD_CORES_AB)" : "");
        if (ht.policy >= 3)
            ps2_log("threads: hard placement masks: hot %llX (one per P-core%s), background %llX (E-cores)",
                    (unsigned long long)(ht.policy == 4 ? ht.pmask | ht.emask : ht.pmask),
                    ht.policy == 4 ? " + E-cores" : "", (unsigned long long)ht.emask);
    }
    tpol_init();                                      /* the spin-free thread policy (perf6_threads) */
    pthread_mutex_unlock(&once);
}
/* (perf6_threads) the policy for the EE idle wait (ps2_kernel.c) and the decode pool (ps2_gs.c) */
void ps2_thread_policy(int *on, int *helpers, int *spin_us) {
    ht_policy_init();
    if (on) *on = tpol.on;
    if (helpers) *helpers = tpol.helpers;
    if (spin_us) *spin_us = tpol.spin_us;
}
/* `what` (1 idle, 2 pool, 4 bell) follows the PS2_CPU_AB blocks (ps2_tpol_now) */
int ps2_tpol_ab(int what) { ht_policy_init(); return (tpol_ab & what) != 0; }
static int ht_block_now(void) {
    if (!ht.policy) return 0;
    if (ht.ab_s <= 0) return 1;
    return (int)((ht_now_ns() / (1000000000ull * (u64)ht.ab_s)) & 1ull);
}
#ifdef _WIN32
/* the calling thread's QoS: 1 High (power throttling off), 2 Eco (on), 0 the system's choice */
static void ht_qos(int q) {
    typedef BOOL (WINAPI *sti_fn)(HANDLE, THREAD_INFORMATION_CLASS, LPVOID, DWORD);
    static sti_fn fn; static int init;
    if (!init) { init = 1; fn = (sti_fn)(void (*)(void))GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadInformation"); }
    if (!fn) return;
    THREAD_POWER_THROTTLING_STATE st;
    memset(&st, 0, sizeof st);
    st.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    st.ControlMask = q ? THREAD_POWER_THROTTLING_EXECUTION_SPEED : 0;
    st.StateMask = q == 2 ? THREAD_POWER_THROTTLING_EXECUTION_SPEED : 0;
    fn(GetCurrentThread(), ThreadPowerThrottling, &st, sizeof st);
}
#endif
/* the calling thread: apply the placement of the current block (cheap when unchanged) */
static void ht_place(void) {
    if (ht_role < 0 && ht_class < 0) return;
    int b = ht_block_now();
    if (b == ht_applied_block) return;
    ht_applied_block = b;
    if (ht.policy >= 3) {                             /* (hard placement: hot threads and background workers) */
#ifdef _WIN32
        DWORD_PTR pm = 0, sm = 0;
        GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm);
        DWORD_PTR want = (DWORD_PTR)(ht_class == 1 ? ht.emask : ht.policy == 4 ? ht.pmask | ht.emask : ht.pmask) & pm;
        SetThreadAffinityMask(GetCurrentThread(), b && want ? want : pm);
        ht_qos(b ? (ht_class == 1 ? 2 : 1) : 0);
#endif
        if (ht_role == HT_GFX && ht.ab_s > 0)
            ps2_tprof_ev(12, (u32)(ht_now_ns() / (1000000000ull * (u64)ht.ab_s)), (u64)b, 0);
        return;
    }
    if (ht_role < 0) return;                          /* (policies p / e: the hot threads with a role only) */
    if (ht.policy == 2 && ht_role != HT_GFX && ht_role != HT_GFX2) b = 0;   /* (test policy e: the worker only) */
#ifdef _WIN32
    if (ht.policy == 2 && (ht_role == HT_GFX || ht_role == HT_GFX2)) {   /* (test: a hard affinity, CPU sets are soft) */
        DWORD_PTR pm = 0, sm = 0, em = 0;
        GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm);
        for (int lp = 0; lp < ht.n && lp < 64; lp++) if (ht.eff[lp] != ht.pmax) em |= (DWORD_PTR)1 << lp;
        SetThreadAffinityMask(GetCurrentThread(), b && (em & pm) ? (em & pm) : pm);
    } else if (b) SetThreadSelectedCpuSets(GetCurrentThread(), ht.hot_ids, (ULONG)ht.hot_n);
    else SetThreadSelectedCpuSets(GetCurrentThread(), NULL, 0);
#endif
    if (ht_role == HT_GFX && ht.ab_s > 0)
        ps2_tprof_ev(12, (u32)(ht_now_ns() / (1000000000ull * (u64)ht.ab_s)), (u64)b, 0);
}
/* a hot thread starts: "ee" (every guest thread), "gfx" (this worker), "vk" (the renderer), "gfx2" (stage 2), "pool"
   (a texture decode helper: placement only); "bg": a background worker (placement only, policy h) */
void ps2_host_thread_start(const char *who) {
    int r = !strcmp(who, "ee") ? HT_EE : !strcmp(who, "gfx") ? HT_GFX : !strcmp(who, "vk") ? HT_VK
          : !strcmp(who, "gfx2") ? HT_GFX2 : -1;
    ht_policy_init();
    ht_role = r;
    ht_class = r >= 0 || !strcmp(who, "pool") ? 0 : !strcmp(who, "bg") ? 1 : -1;
    ht_applied_block = -1;
    if (ht_class >= 0) ht_place();
    if (ps2_host_thread_sampling || tpol_ab) {        /* (perf6_threads: thread CPU per role) */
        int tr = r == HT_EE ? TC_EE : r == HT_GFX ? TC_GFX : r == HT_GFX2 ? TC_GFX2 : r == HT_VK ? TC_VK
               : !strcmp(who, "pool") ? TC_POOL : -1;
        if (tr >= 0) tc_register(tr);
    }
#ifdef _WIN32
    /* (perf6_threads) the thread policy: the pipeline after the EE one level above it (PS2_THREAD_POLICY_PRIO) */
    if (tpol.on && tpol.prio && !(tpol_ab & 8) && (r == HT_GFX || r == HT_GFX2 || r == HT_VK || !strcmp(who, "pool")))
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif
}
/* a placement-only thread (pool, bg) in its loop: follow the _AB blocks */
void ps2_host_thread_replace(void) { if (ht.ab_s > 0 && ht_class >= 0) ht_place(); }
/* the calling hot thread ran `ns` up to now on the processor it is on now; returns 1 on a slower (E-) core */
int ps2_host_thread_ran(u64 ns) {
    int r = ht_role;
    if (r < 0) return 0;
    int lp = ht_lp_now();
    ht_ns[r][lp] += ns;
    ht_cur[r] = lp + 1;
    for (int o = 0; o < HT_N; o++) {
        int c = ht_cur[o];
        if (o != r && c && ht.core[c - 1] == ht.core[lp]) { ht_share_ns[r] += ns; break; }
    }
    if (ht.ab_s > 0) ht_place();
    return ht.eff[lp] != ht.pmax;
}
void ps2_host_thread_idle(void) { if (ht_role >= 0) ht_cur[ht_role] = 0; }
static void ht_log_cores(void) {
    static u64 prev[HT_N][HT_LP], prev_share[HT_N];
    static const char *nm[HT_N] = { "ee", "gfx", "renderer", "gfx stage 2" };
    char line[600]; int o = 0;
    for (int r = 0; r < HT_N; r++) {
        u64 tot = 0, eff = 0, fav = 0, sh = ht_share_ns[r] - prev_share[r];
        int top = 0; u64 topv = 0;
        for (int lp = 0; lp < ht.n; lp++) {
            u64 d = ht_ns[r][lp] - prev[r][lp];
            prev[r][lp] = ht_ns[r][lp];
            tot += d;
            if (ht.eff[lp] != ht.pmax) eff += d;
            if (ht.sched[lp] > 1) fav += d;
            if (d > topv) { topv = d; top = lp; }
        }
        prev_share[r] = ht_share_ns[r];
        if (!tot) continue;
        o += snprintf(line + o, sizeof line - (size_t)o, "%s%s E-cores %.1f%% favoured %.1f%% shared core %.1f%% (most on %d: %.0f%%)",
                      o ? "; " : "", nm[r], 100.0 * eff / tot, 100.0 * fav / tot, 100.0 * sh / tot, top, 100.0 * topv / tot);
    }
    if (o) ps2_log("threads: %s", line);
}

/* renderer hooks (ps2_video.c): the list's publication and its take run under the renderer's list lock */
void ps2_tprof_publish(u32 field, int replace) {
    tp_pub_field = field;
    ps2_tprof_ev(7, field, (u64)(replace != 0), 0);
}
void ps2_tprof_take(u64 frame) { ps2_tprof_ev(8, tp_pub_field, frame, 0); }
void ps2_tprof_rec(u32 what, u64 frame, int rr, u64 t) {
    static _Thread_local u64 t_wait;
    if (!ps2_host_thread_sampling) return;
    u64 now = t ? t : ht_now_ns();
    if (what == 9) { t_wait = now; ps2_host_thread_ran(0); }
    else if (what == 10) { if (t_wait && now > t_wait) ps2_host_thread_ran(now - t_wait); t_wait = 0; ps2_host_thread_idle(); }
    ps2_tprof_ev_at(what, now, (u32)frame, (u64)(rr != 0), 0);
}
void ps2_tprof_gpu(u64 frame, u64 t0, double lat_ms) {
    if (lat_ms > 0.0 && t0) ps2_tprof_ev(11, (u32)frame, t0, t0 + (u64)(lat_ms * 1e6));
}
/* the EE: queued a field (ps2_gfxq_field) and its first idle poll after it (ps2_kernel_ee_idle) */
static u32 tp_ee_field;
u32 ps2_tprof_ee_idle_pending;                       /* EE field + 1 whose first idle poll is still to come */
static u64 cz_ee_t0;                                 /* (PS2_CPU_PROF: the EE queued its field at this TSC) */
void ps2_tprof_ee_idle(void) {
    u32 f = ps2_tprof_ee_idle_pending;
    ps2_tprof_ee_idle_pending = 0;
    if (f) ps2_tprof_ev(3, f - 1u, 0, 0);
    if (f && cz_ee_t0) { ps2_cz_end(CZ_EE_FIELD, cz_ee_t0); cz_ee_t0 = 0; }
}
/* ==== end of host threads ==== */
void ps2_scene_speed_report(double dt);
static void q_split_speed_log(void);
void ps2_speed_sample(u64 fields, u64 dropped) {
    static u64 prev_ns, prev_fields, prev_dropped, prev_present, prev_n, prev_work;
    static u64 prev_field_wait, prev_ring_wait, prev_drain, prev_bytes;
    static u64 prev_vu_runs, prev_vu_insns, prev_vu_guards;
    if (!ps2_speed_enabled()) return;
    u64 now = gq_now_ns();
    if (!prev_ns) {
        ps2_log("speed: diagnostics v1; 5s intervals, scene dispatch is not simulation FPS; collision/VIF fixes 2026-09-06");
        ps2_log("speed: EE/present/worker ms are interval totals; present_ms is GS display-list handoff, not Vulkan GPU time. See vk-speed for renderer host stages.");
        prev_ns = now; prev_fields = fields; prev_dropped = dropped;
        prev_field_wait=st_field_wait_ns; prev_ring_wait=st_producer_ns;
        prev_drain=st_drain_ns; prev_bytes=st_bytes;
        pthread_mutex_lock(&q_lock);
        prev_present=speed_present_ns; prev_n=speed_present_n; prev_work=speed_work_ns;
        prev_vu_runs=speed_vu_runs; prev_vu_insns=speed_vu_insns; prev_vu_guards=speed_vu_guards;
        pthread_mutex_unlock(&q_lock);
        ps2_scene_speed_report(0);
        return;
    }
    if (now - prev_ns < 5000000000ull) return;
    double dt = (now - prev_ns) / 1e9;
    pthread_mutex_lock(&q_lock);
    u64 pn = speed_present_n, pt = speed_present_ns, work = speed_work_ns;
    u64 vr=speed_vu_runs, vi=speed_vu_insns, vg=speed_vu_guards;
    size_t fill = q_head - q_tail;
    int pending = q_fields_pending;
    u64 fmax = speed_field_max, flong = speed_field_long, wcpu = speed_work_cpu_ns;
    speed_field_max = speed_field_long = speed_work_cpu_ns = 0;
    pthread_mutex_unlock(&q_lock);
    ps2_log("speed: dt=%.2fs fields=%.2f/s dropped=%llu gfxq=%d pending=%d ring_KB=%zu queued_MB=%.2f",
        dt, (fields-prev_fields)/dt, (unsigned long long)(dropped-prev_dropped),
        ps2_gfxq_on, pending, fill >> 10, (st_bytes-prev_bytes)/1048576.0);
    ps2_log("speed: EE wait_ms field=%.2f ring=%.2f drain=%.2f; present=%.2f/s present_ms=%.2f worker_active_ms=%.2f",
        (st_field_wait_ns-prev_field_wait)/1e6, (st_producer_ns-prev_ring_wait)/1e6,
        (st_drain_ns-prev_drain)/1e6, (pn-prev_n)/dt, (pt-prev_present)/1e6, (work-prev_work)/1e6);
    ps2_log("speed: game frames=%.2f/s long(>1.5 fields)=%llu max_ms=%.1f; worker per field max_ms=%.2f over_16.7ms=%llu "
        "cpu_ms=%.0f (far below worker_active_ms: the worker was preempted)",
        sg_frames/dt, (unsigned long long)sg_long, sg_max_ns/1e6, fmax/1e6, (unsigned long long)flong, wcpu/1e6);
    sg_frames = sg_long = sg_max_ns = 0;
    {   /* CPU time per host thread over the window (GetThreadTimes; 0 where not available), and per field */
        extern u64 ps2_host_thread_cpu_ns(const char *who);
        static u64 pc_ee, pc_gfx, pc_vk;
        u64 c_ee = ps2_host_thread_cpu_ns("ee"), c_gfx = ps2_host_thread_cpu_ns("gfx"), c_vk = ps2_host_thread_cpu_ns("vk");
        double nf = (double)(fields - prev_fields);
        if (nf < 1.0) nf = 1.0;
        ps2_log("speed: thread cpu_ms ee=%.0f gfx=%.0f renderer=%.0f; per field ee=%.2f gfx=%.2f renderer=%.2f",
            (c_ee - pc_ee) / 1e6, (c_gfx - pc_gfx) / 1e6, (c_vk - pc_vk) / 1e6,
            (c_ee - pc_ee) / 1e6 / nf, (c_gfx - pc_gfx) / 1e6 / nf, (c_vk - pc_vk) / 1e6 / nf);
        {   static u64 pw, pr;                         /* how often the EE had to wake the idle worker */
            ps2_log("speed: EE -> worker records %.0f per field, wake-ups of the idle worker %.0f per field",
                    (st_records - pr) / nf, (st_wakes - pw) / nf);
            pw = st_wakes; pr = st_records;
        }
        {   /* (perf6_threads) how often stage 1 really woke, the EE's time in the signal path, the doorbell */
            static u64 ps, pt, pd, pf;
            extern double ps2_host_tsc_per_ns(void);
            double r = ps2_host_tsc_per_ns();
            u64 fs = __atomic_load_n(&st_db_flush_sig, __ATOMIC_RELAXED);
            ps2_log("speed: EE -> stage 1: stage 1 woke from its wait %.0f times per field (pthread_cond_wait returns), "
                    "the EE signalled for %.3f ms per field; doorbell %s: %.1f rings, %.1f flush signals per field",
                    (st_s1_wakeups - ps) / nf, r > 0.0 ? (st_sig_ticks - pt) / r / 1e6 / nf : 0.0,
                    q_db_bytes ? "batched" : "per record", (st_db_rings - pd) / nf, (fs - pf) / nf);
            ps = st_s1_wakeups; pt = st_sig_ticks; pd = st_db_rings; pf = fs;
        }
        {   /* (perf6_threads) CPU per field of every thread of each role (QueryThreadCycleTime) and of the process */
            static u64 pc[TC_N], pp;
            static int init;
            extern double ps2_host_tsc_per_ns(void);
            u64 c[TC_N];
            int cnt[TC_N];
            u64 proc = tc_snapshot(c, cnt);
            double r = ps2_host_tsc_per_ns(), k = r > 0.0 ? 1.0 / r / 1e6 / nf : 0.0;
            if (init && r > 0.0)
                ps2_log("threads-cpu: ms of CPU per field: ee %.2f (%d threads) gfx %.2f gfx2 %.2f renderer %.2f pool %.2f "
                        "(%d helpers); process %.2f", (c[TC_EE] - pc[TC_EE]) * k, cnt[TC_EE], (c[TC_GFX] - pc[TC_GFX]) * k,
                        (c[TC_GFX2] - pc[TC_GFX2]) * k, (c[TC_VK] - pc[TC_VK]) * k, (c[TC_POOL] - pc[TC_POOL]) * k,
                        cnt[TC_POOL], (proc - pp) / 1e6 / nf);
            memcpy(pc, c, sizeof pc);
            pp = proc;
            init = 1;
        }
        {   extern void ps2_kernel_speed_report(double nf);
            ps2_kernel_speed_report(nf);              /* (the EE's vblank wake latency and idle waits) */
        }
        pc_ee = c_ee; pc_gfx = c_gfx; pc_vk = c_vk;
    }
    ht_log_cores();                      /* where the hot threads ran (host threads, above) */
    q_split_speed_log();                 /* PS2_GFX_SPLIT: stage 2 (below) */
    {   extern void ps2_vif_fast_report(void);
        ps2_vif_fast_report();           /* PS2_PERF_VIF_FAST: UNPACK words by format and path so far (verify counts) */
    }
    if (ps2_phase_on) {                  /* PS2_PROFILE_PHASES: the phase profile of this 5 s window, then a new window */
        ps2_phase_report(fields - prev_fields);
        ps2_phase_init();
    }
    ps2_scene_speed_report(dt);
    cz_report(fields - prev_fields);     /* PS2_CPU_PROF: the CPU zones of this window */
    ps2_log("speed: VU1 runs=%.2f/s instructions_M=%.3f guard_hits=%llu",
        (vr-prev_vu_runs)/dt, (vi-prev_vu_insns)/1e6, (unsigned long long)(vg-prev_vu_guards));
    prev_vu_runs=vr; prev_vu_insns=vi; prev_vu_guards=vg;
    prev_ns=now; prev_fields=fields; prev_dropped=dropped; prev_present=pt; prev_n=pn;
    prev_field_wait=st_field_wait_ns; prev_ring_wait=st_producer_ns; prev_drain=st_drain_ns;
    prev_bytes=st_bytes;
    prev_work=work;
}

static size_t q_producer_tail, q_reserved_end;
static u8 *q_reserve(u32 n) {
    size_t off = q_head & (q_cap - 1);
    size_t pad = off + n > q_cap ? q_cap - off : 0;
    size_t need = pad + n;
    if (q_cap - (q_head - q_producer_tail) < need) {
        pthread_mutex_lock(&q_lock);
        q_producer_tail = q_tail;
        while (q_cap - (q_head - q_producer_tail) < need) {
            u64 t0 = gq_now_ns();
            st_producer_waits++;
            if (q_db_bytes) pthread_cond_signal(&q_not_empty);   /* (doorbell: stage 1 may sleep on unrung records) */
            pthread_cond_wait(&q_not_full, &q_lock);
            st_producer_ns += gq_now_ns() - t0;
            q_producer_tail = q_tail;
        }
        pthread_mutex_unlock(&q_lock);
    }
    q_reserved_end = q_head + need;
    if (pad) {
        q_buf[off] = GQ_WRAP;
        off = 0;
    }
    return q_buf + off;
}

static void q_signal(void) {                          /* the EE wakes the idle worker: a futex round trip */
    u64 t0 = ps2_host_thread_sampling ? (u64)__rdtsc() : 0;
    st_wakes++;
    pthread_mutex_lock(&q_lock);
    pthread_cond_signal(&q_not_empty);
    pthread_mutex_unlock(&q_lock);
    if (t0) st_sig_ticks += (u64)__rdtsc() - t0;
}
static void q_ring(void) {                            /* (PS2_GFXQ_DOORBELL, producer) wake stage 1 if it sleeps */
    q_db_mark = q_head;
    st_db_rings++;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);          /* (our published head against stage 1's q_busy = 0 + re-check) */
    if (!__atomic_load_n(&q_busy, __ATOMIC_SEQ_CST)) q_signal();
}
static void q_commit(u32 n) {
    size_t fill;
    if (q_db_bytes) {                                 /* (PS2_GFXQ_DOORBELL: published now, rung per batch) */
        __atomic_store_n(&q_head, q_reserved_end, __ATOMIC_RELEASE);
        st_records++;
        st_bytes += n;
        fill = q_reserved_end - __atomic_load_n(&q_tail, __ATOMIC_RELAXED);
        if (fill > st_peak_fill) st_peak_fill = fill;
        if (q_reserved_end - q_db_mark >= q_db_bytes) q_ring();
        return;
    }
    __atomic_store_n(&q_head, q_reserved_end, __ATOMIC_SEQ_CST);
    st_records++;
    st_bytes += n;
    fill = q_reserved_end - __atomic_load_n(&q_tail, __ATOMIC_RELAXED);
    if (fill > st_peak_fill) st_peak_fill = fill;
    if (!__atomic_load_n(&q_busy, __ATOMIC_SEQ_CST)) q_signal();
}
/* (perf6_threads) PS2_GFXQ_DOORBELL: before a thread of the EE blocks or polls (INTC_STAT polling, a guest thread
   switch) and once per field from the vblank clock -- wake stage 1 if it sleeps while published records wait. Any
   thread: the caller's own records are published before it calls this, so the fence orders them against stage 1's
   q_busy = 0 and its re-check of the head (stage 1 then sees them, or we see it idle and signal). */
void ps2_gfxq_doorbell_flush(void) {
    static size_t flushed;                            /* the head of the last flush: a spinning EE polls this thousands
                                                         of times a field, and stage 1 needs q_lock to wake up */
    if (!__atomic_load_n(&q_db_on, __ATOMIC_RELAXED) || q_is_worker || !ps2_gfxq_on) return;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    size_t h = __atomic_load_n(&q_head, __ATOMIC_ACQUIRE);
    if (h == __atomic_load_n(&flushed, __ATOMIC_RELAXED)) return;
    __atomic_store_n(&flushed, h, __ATOMIC_RELAXED);
    if (h == __atomic_load_n(&q_tail, __ATOMIC_ACQUIRE)) return;
    if (__atomic_load_n(&q_busy, __ATOMIC_SEQ_CST)) return;
    __atomic_fetch_add(&st_db_flush_sig, 1ull, __ATOMIC_RELAXED);
    pthread_mutex_lock(&q_lock);
    pthread_cond_signal(&q_not_empty);
    pthread_mutex_unlock(&q_lock);
}
static void tpol_set_bell(int on) {                   /* (producer, at a field) */
    u32 want = on ? tpol.db : 0u;
    if (want == q_db_bytes) return;
    q_db_mark = q_head;
    q_db_bytes = want;
    __atomic_store_n(&q_db_on, want != 0u, __ATOMIC_RELAXED);
}
/* (perf6_threads; the EE, once per field) PS2_CPU_AB with `policy` / `idle` / `pool` / `bell`: the block's state
   (ps2_tpol_now, read by the EE idle wait and the decode pool; the doorbell switches here) and a `cpu-ab:` line per
   finished block: CPU per field of each thread role and of the process, the EE's idle waits and vblank wake latency,
   stage 1's wake-ups, the EE's signals */
static void tpol_field(void) {
    static int cur = -1, fields;
    static u64 t0, c0[TC_N], p0, w0, i0, o0, vn0, vs0, s0, g0, gt0, r0;
    extern double ps2_host_tsc_per_ns(void);
    extern void ps2_kernel_ee_stats(u64 *idle_waits, u64 *idle_ns, u64 *timeouts, u64 *vbl_n, u64 *vbl_ns, u64 *vbl_max);
    fields++;
    int b = ps2_cpu_ab_b();
    if (b == cur) return;
    u64 now = gq_now_ns(), c[TC_N], iw, ins, ito, vn, vns, vmax;
    int cnt[TC_N];
    u64 proc = tc_snapshot(c, cnt);
    ps2_kernel_ee_stats(&iw, &ins, &ito, &vn, &vns, &vmax);
    if (cur >= 0 && fields > 1) {
        double nf = (double)(fields - 1), r = ps2_host_tsc_per_ns(), k = r > 0.0 ? 1.0 / r / 1e6 / nf : 0.0;
        extern u64 ps2_kernel_vblank_count(void);
        ps2_log("cpu-ab: block %llu %s %.2f s %d fields at field %llu: cpu ms per field ee %.3f gfx %.3f gfx2 %.3f renderer %.3f pool %.3f "
                "process %.3f; EE idle waits %.2f per field %.3f ms per field %llu timeouts; vblank -> polling EE %llu mean "
                "%.1f us max %.1f us; stage 1 woke %.1f per field; EE signals %.1f per field %.4f ms per field; rings %.1f "
                "per field",
                (unsigned long long)(now / (1000000000ull * (u64)(cab_s > 0 ? cab_s : 1))) - 1ull, cur ? "B" : "A",
                (now - t0) / 1e9, fields - 1, (unsigned long long)ps2_kernel_vblank_count(), (c[TC_EE] - c0[TC_EE]) * k,
                (c[TC_GFX] - c0[TC_GFX]) * k,
                (c[TC_GFX2] - c0[TC_GFX2]) * k, (c[TC_VK] - c0[TC_VK]) * k, (c[TC_POOL] - c0[TC_POOL]) * k,
                (proc - p0) / 1e6 / nf, (iw - w0) / nf, (ins - i0) / 1e6 / nf, (unsigned long long)(ito - o0),
                (unsigned long long)(vn - vn0), vn > vn0 ? (vns - vs0) / 1e3 / (double)(vn - vn0) : 0.0, vmax / 1e3,
                (st_s1_wakeups - s0) / nf, (st_wakes - g0) / nf, r > 0.0 ? (st_sig_ticks - gt0) / r / 1e6 / nf : 0.0,
                (st_db_rings - r0) / nf);
    }
    cur = b; fields = 1; t0 = now; memcpy(c0, c, sizeof c0); p0 = proc;
    w0 = iw; i0 = ins; o0 = ito; vn0 = vn; vs0 = vns;
    s0 = st_s1_wakeups; g0 = st_wakes; gt0 = st_sig_ticks; r0 = st_db_rings;
    if (tpol_ab & 4) tpol_set_bell(b);
#ifdef _WIN32
    if (tpol_ab & 8) {
        int n = __atomic_load_n(&tc_n, __ATOMIC_ACQUIRE);
        for (int i = 0; i < n; i++)
            if (tc_role[i] != TC_EE)
                SetThreadPriority(tc_h[i], b ? THREAD_PRIORITY_ABOVE_NORMAL : THREAD_PRIORITY_NORMAL);
    }
#endif
    __atomic_store_n(&ps2_tpol_now, b, __ATOMIC_RELEASE);
}

static inline void put8(u8 *p, u32 off, u8 v)   { p[off] = v; }
static inline void put32(u8 *p, u32 off, u32 v) { memcpy(p + off, &v, 4); }
static inline void put64(u8 *p, u32 off, u64 v) { memcpy(p + off, &v, 8); }
static inline u32  get32(const u8 *p, u32 off)  { u32 v; memcpy(&v, p + off, 4); return v; }
static inline u64  get64(const u8 *p, u32 off)  { u64 v; memcpy(&v, p + off, 8); return v; }

static u32 q_apply(const u8 *r) {
    switch (r[0]) {
    case GQ_VIF_QW: {
        int ch = (int)r[1];
        u32 qwc = get32(r, 2);
        const ps2_reg128 *qw = (const ps2_reg128 *)(r + 6);
        ps2_vif_transfer(ch, qw, qwc);
        return 6u + qwc * 16u;
    }
    case GQ_GIF_QW: {
        u32 qwc = get32(r, 1);
        ps2_gif_transfer((const ps2_reg128 *)(r + 5), qwc);
        return 5u + qwc * 16u;
    }
    case GQ_VIF_W:   ps2_vif_write((int)r[1], get32(r, 2));        return 6u;
    case GQ_FBRST:   ps2_vif_fbrst((int)r[1], get32(r, 2));        return 6u;
    case GQ_VIFCLR:  ps2_vif_clear_stall((int)r[1]);               return 2u;
    case GQ_GS_PRIV: ps2_gs_priv_write_now(get32(r, 1), get64(r, 5)); return 13u;
    case GQ_MMIO:    ps2_gif_ctrl_write(get32(r, 1), get32(r, 5)); return 9u;
    case GQ_TAG:     rn_gs_tag(get32(r, 1), get32(r, 5), get32(r, 9)); return 13u;
    case GQ_INTENT: {
        u32 len = get32(r, 1);
        rn_gs_intent(r + 5, len);
        return 5u + len;
    }
    case GQ_FIELD: {
        u64 cz0 = ps2_cz_t();
        if (!ps2_gs_present_now()) ps2_request_exit();
        ps2_cz_end(CZ_GFX_PRESENT, cz0);
        return 1u;
    }
    case GQ_VU1: {
        extern int ps2_vu1_split_apply(const u8 *r, u32 len);
        u32 len = get32(r, 1);
        return ps2_vu1_split_apply(r + 5, len) ? 5u + len : 0u;
    }
    default:
        ps2_log("gfxq: corrupt record opcode %u -- stopping the worker", r[0]);
        return 0u;
    }
}

/* ---- The graphics worker in two stages (PS2_GFX_SPLIT; op_threads 2026-10-23) ----
   One worker thread used to do all of a field's graphics work in stream order: the VIF1 command stream (codes, UNPACK
   into VU1 memory), the VU1 programs (native decoders), the GIF, the GS (image transfers, texture decodes, primitive
   assembly, the display list) and the field's present. Sim M3 J35J, head 35 deg down (user config): 8.9 ms a field,
   of it ~32 % ring dispatch + VIF + UNPACK; in the campaign session's heaviest windows the worker needed 11-15 ms of CPU
   per field (20-30 ms peaks), the EE waited for it and the game ran at 55-59 frames/s.
   With PS2_GFX_SPLIT=1 this thread (stage 1) runs the ring and VIF1 and forwards everything else, in order, through a
   second ring to stage 2 (b_main): VIF1's UNPACK writes as VU1 memory records, MSCAL / MSCNT as activations, MPG and
   DIRECT data, and every other record verbatim (GIF path 3, GS privileged writes, tags, VIF0, the field). Stage 2 does
   what the single worker did with them at the same point of the stream (ps2_vu.c: VIF1 / VU1 split), so the GS, the
   textures and the display list see the same sequence; a field is published by stage 2. Syncs (stage 1 waits for
   stage 2 to finish): a VIF1 interrupt code, a mode change, the EE's drain. PS2_GFX_SPLIT_AB=S: in-run A/B, the split
   in odd blocks of S seconds, one worker in the even ones (switched at a field boundary after a sync). */
static int q_split_cfg, q_split_ab_s;
static int q_split;                                   /* the current mode (stage 1 switches it at field boundaries) */
extern int ps2_vif1_split;
void ps2_vu1_split_flush(void);
int  ps2_vu1_split_apply(const u8 *r, u32 len);
void ps2_vu1_split_stats(u64 *flushes, u64 *qwords, u64 *acts, u64 *irq_syncs);
static u8 *b_buf;
static size_t b_cap, b_head, b_tail;                  /* stage 2's ring: published head, consumed tail */
static size_t b_wr, b_ptail;                          /* stage 1: write position (published at batch ends), tail seen */
static int b_busy, b_running;
static pthread_t b_thread;
static pthread_mutex_t b_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t b_not_empty = PTHREAD_COND_INITIALIZER;
static pthread_cond_t b_not_full = PTHREAD_COND_INITIALIZER;
static pthread_cond_t b_idle = PTHREAD_COND_INITIALIZER;
static u64 st_b_full_waits, st_b_full_ns, st_b_syncs, st_b_sync_ns, st_b_bytes, st_b_wakes;
static u64 s2_field_max, s2_field_long, s2_busy_ns, s2_fields;   /* (speed log, under b_lock) */
static u32 s2_field;                                  /* stage 2's field index (timeline), set at a switch to split */

static void b_publish(void) {
    if (b_wr == b_head) return;
    __atomic_store_n(&b_head, b_wr, __ATOMIC_SEQ_CST);
    if (!__atomic_load_n(&b_busy, __ATOMIC_SEQ_CST)) {
        st_b_wakes++;
        pthread_mutex_lock(&b_lock);
        pthread_cond_signal(&b_not_empty);
        pthread_mutex_unlock(&b_lock);
    }
}
static u8 *b_reserve(u32 n) {
    size_t off = b_wr & (b_cap - 1);
    size_t pad = off + n > b_cap ? b_cap - off : 0;
    size_t need = pad + n;
    if (b_cap - (b_wr - b_ptail) < need) {
        b_publish();                                  /* stage 2 works on what is there */
        u64 t0 = gq_now_ns();
        pthread_mutex_lock(&b_lock);
        b_ptail = b_tail;
        while (b_running && b_cap - (b_wr - b_ptail) < need) {
            st_b_full_waits++;
            pthread_cond_signal(&b_not_empty);
            pthread_cond_wait(&b_not_full, &b_lock);
            b_ptail = b_tail;
        }
        pthread_mutex_unlock(&b_lock);
        st_b_full_ns += gq_now_ns() - t0;
    }
    if (pad) {
        b_buf[off] = GQ_WRAP;
        b_wr += pad;
        off = 0;
    }
    return b_buf + off;
}
/* records become visible to stage 2 at stage 1's batch ends, and every 8 KB in between: one batch can hold a DMA chunk
   with hundreds of VU1 activations, which stage 2 would otherwise see only in one late burst (E-core test: ~3.5 ms
   behind at the field's end for a third of the fields) */
#define B_PUBLISH_BYTES 8192u
static void b_commit(u32 n) {
    b_wr += n; st_b_bytes += n;
    if (b_wr - b_head >= B_PUBLISH_BYTES) b_publish();
}
/* stage 1: wait until stage 2 has done everything forwarded so far */
static void b_sync(void) {
    u64 t0 = gq_now_ns();
    b_publish();
    pthread_mutex_lock(&b_lock);
    st_b_syncs++;
    while (b_running && (__atomic_load_n(&b_head, __ATOMIC_ACQUIRE) != b_tail || b_busy)) {
        pthread_cond_signal(&b_not_empty);
        pthread_cond_wait(&b_idle, &b_lock);
    }
    pthread_mutex_unlock(&b_lock);
    st_b_sync_ns += gq_now_ns() - t0;
}
/* (ps2_vu.c, stage 1) a VU1 record of `len` bytes for stage 2 */
u8 *ps2_gfxq_s2_vu1(u32 len) {
    u8 *r = b_reserve(5u + len);
    r[0] = GQ_VU1;
    memcpy(r + 1, &len, 4);
    return r + 5;
}
void ps2_gfxq_s2_vu1_done(u32 len) { b_commit(5u + len); }
void ps2_gfxq_s2_sync(void) { ps2_vu1_split_flush(); b_sync(); }
static int q_split_wanted(void) {
    if (!b_buf) return 0;
    if (q_split_ab_s > 0) return (int)((gq_now_ns() / (1000000000ull * (u64)q_split_ab_s)) & 1ull);
    return q_split_cfg;
}
static u32 q_rec_len(const u8 *r) {
    switch (r[0]) {
    case GQ_VIF_QW:  return 6u + get32(r, 2) * 16u;
    case GQ_GIF_QW:  return 5u + get32(r, 1) * 16u;
    case GQ_VIF_W:   case GQ_FBRST: return 6u;
    case GQ_VIFCLR:  return 2u;
    case GQ_GS_PRIV: case GQ_TAG: return 13u;
    case GQ_MMIO:    return 9u;
    case GQ_INTENT:  return 5u + get32(r, 1);
    case GQ_FIELD:   return 1u;
    default:         return 0u;
    }
}
/* a field's present is done (under q_lock): the EE may queue another */
static void q_field_done_locked(int speed_on, u64 speed_elapsed) {
    if (speed_on) {
        speed_present_ns += speed_elapsed; speed_present_n++;
        ps2_vu1_speed_counters(&speed_vu_runs,&speed_vu_insns,&speed_vu_guards);
    }
    q_fields_pending--;
}

static void *q_main(void *unused) {
    (void)unused;
    q_is_worker = 1;
    ps2_host_prof_attach("gfx");
    u64 speed_burst=0;
    int speed_on=ps2_speed_enabled();
    /* host threads (above): core sampling per record batch and the field timeline (PS2_THREAD_PROF) */
    extern u64 ps2_host_thread_cycles(void);
    extern double ps2_host_tsc_per_ns(void);
    const int samp = ps2_host_thread_sampling, tp = ps2_tprof_on();
    u32 tp_field = 0; int tp_open = 0;
    u64 tp_busy = 0, tp_busy_e = 0, tp_bytes = 0, tp_recs = 0, tp_cyc0 = 0, cz_busy = 0;
    if (tp) ps2_host_tsc_per_ns();                         /* (starts the TSC calibration) */
    pthread_mutex_lock(&q_lock);
    for (;;) {
        while (q_running && __atomic_load_n(&q_head, __ATOMIC_SEQ_CST) == q_tail) {
            if (speed_burst) { speed_work_ns += gq_now_ns()-speed_burst; speed_burst=0; }
            __atomic_store_n(&q_busy, 0, __ATOMIC_SEQ_CST);
            pthread_cond_broadcast(&q_idle);
            if (__atomic_load_n(&q_head, __ATOMIC_SEQ_CST) != q_tail) break;
            if (samp) ps2_host_thread_idle();
            pthread_cond_wait(&q_not_empty, &q_lock);
            st_s1_wakeups++;                          /* (perf6_threads: a real wake-up, under q_lock) */
        }
        if (!q_running && __atomic_load_n(&q_head, __ATOMIC_SEQ_CST) == q_tail) break;
        __atomic_store_n(&q_busy, 1, __ATOMIC_SEQ_CST);
        if (!speed_burst && speed_on) speed_burst=gq_now_ns();
        size_t tail = q_tail, head = __atomic_load_n(&q_head, __ATOMIC_ACQUIRE);
        unsigned records = 0;
        int field = 0, corrupt = 0, fwd_field = 0;
        const int split = q_split;                        /* (changes only at a field, after the batch) */
        u64 speed_elapsed = 0;
        pthread_mutex_unlock(&q_lock);
        u64 tb0 = samp ? ht_now_ns() : 0;
        u64 czb0 = ps2_cz_t();                            /* (PS2_CPU_PROF: busy per field) */
        if (tp && !tp_open && !split) { tp_open = 1; ps2_tprof_ev_at(4, tb0, tp_field, 0, 0); }
        while (tail != head && records < 256) {
            const u8 *r = q_buf + (tail & (q_cap - 1));
            if (*r == GQ_WRAP) {
                tail += q_cap - (tail & (q_cap - 1));
                continue;
            }
            field = *r == GQ_FIELD;
            u64 start = field && speed_on ? gq_now_ns() : 0;
            if (field && samp) {                      /* the field's records are done (its present follows) */
                u64 now = ht_now_ns(), d = now - tb0;
                tb0 = now;
                if (ps2_host_thread_ran(d)) tp_busy_e += d;
                tp_busy += d;
                if (tp && !split) ps2_tprof_ev_at(5, now, tp_field, tp_busy, tp_busy_e);
            }
            u32 n;
            int fwd = 0;
            if (split) {                              /* stage 1 keeps VIF1 and forwards the rest in order */
                const u8 op = *r;
                fwd = (op == GQ_VIF_QW || op == GQ_VIF_W || op == GQ_FBRST || op == GQ_VIFCLR) ? !(r[1] & 1u) : 1;
            }
            if (fwd) {
                n = q_rec_len(r);
                if (n) { u8 *d = b_reserve(n); memcpy(d, r, n); b_commit(n); }
                fwd_field = field;
            } else {
                PS2_PHASE_BEGIN(PS2_PH_GFXQ);
                n = q_apply(r);
                PS2_PHASE_END(PS2_PH_GFXQ);
            }
            if (start) speed_elapsed = gq_now_ns() - start;
            if (!n) { corrupt = 1; break; }
            tail += n;
            records++;
            tp_bytes += n; tp_recs++;
            if (field) {
                if (samp) {
                    u64 now = ht_now_ns(), d = now - tb0;
                    tb0 = now;
                    if (ps2_host_thread_ran(d)) tp_busy_e += d;
                    tp_busy += d;
                    if (tp && split) ps2_tprof_ev_at(14, now, tp_field, tp_busy, tp_busy_e);
                    else if (tp) {
                        u64 cyc = ps2_host_thread_cycles();
                        double rate = ps2_host_tsc_per_ns();
                        u64 cpu = tp_cyc0 && cyc > tp_cyc0 && rate > 0.0 ? (u64)((double)(cyc - tp_cyc0) / rate) : 0;
                        ps2_tprof_ev_at(6, now, tp_field, tp_busy, cpu);
                        ps2_tprof_ev_at(13, now, tp_field, tp_bytes, tp_recs);
                        tp_cyc0 = cyc;
                    }
                }
                if (czb0) {                               /* (PS2_CPU_PROF) */
                    u64 t = (u64)__rdtsc();
                    cz_busy += t - czb0;
                    ps2_cz_add(CZ_GFX_FIELD, t - cz_busy, t);
                    cz_busy = 0; czb0 = t;
                }
                tp_field++; tp_open = 0; tp_busy = tp_busy_e = tp_bytes = tp_recs = 0;
                break;
            }
        }
        if (samp) {
            u64 d = ht_now_ns() - tb0;
            if (ps2_host_thread_ran(d)) tp_busy_e += d;
            tp_busy += d;
        }
        if (czb0) cz_busy += (u64)__rdtsc() - czb0;
        if (split) { ps2_vu1_split_flush(); b_publish(); }  /* everything of this batch is with stage 2 */
        if (field && !corrupt) {                          /* PS2_GFX_SPLIT(_AB): the mode for the next field */
            int want = q_split_wanted();
            if (want != q_split) {
                if (q_split) b_sync();                    /* stage 2 finishes; VIF1 / VU1 are one thread's again */
                else s2_field = tp_field;
                ps2_vif1_split = want;
                q_split = want;
                if (tp) ps2_tprof_ev(15, (u32)(gq_now_ns() / 1000000000ull / (u64)(q_split_ab_s > 0 ? q_split_ab_s : 1)),
                                     (u64)want, 0);
            }
        }
        pthread_mutex_lock(&q_lock);
        __atomic_store_n(&q_tail, tail, __ATOMIC_RELEASE);
        if (field && !corrupt) {
            if (speed_burst) { u64 now=gq_now_ns(); speed_work_ns+=now-speed_burst; speed_burst=now; }
            if (speed_on) {
                static u64 cpu0;
                u64 cpu = thread_cpu_ns();
                if (cpu0 && cpu > cpu0) speed_work_cpu_ns += cpu - cpu0;
                cpu0 = cpu;
                u64 fw = speed_work_ns - speed_field_work0;
                speed_field_work0 = speed_work_ns;
                if (fw > speed_field_max) speed_field_max = fw;
                if (fw > 16666667ull) speed_field_long++;
            }
            if (!fwd_field) q_field_done_locked(speed_on, speed_elapsed);   /* (stage 2 does it for its fields) */
        }
        pthread_cond_broadcast(&q_not_full);
        if (corrupt) { q_running = 0; break; }
    }
    __atomic_store_n(&q_busy, 0, __ATOMIC_SEQ_CST);
    pthread_cond_broadcast(&q_idle);
    pthread_mutex_unlock(&q_lock);
    return NULL;
}

/* stage 2 of the split worker: the forwarded records, in order (VU1, GIF, GS, present) */
static void *b_main(void *unused) {
    (void)unused;
    q_is_worker = 1;
    ps2_host_prof_attach("gfx2");
    extern u64 ps2_host_thread_cycles(void);
    extern double ps2_host_tsc_per_ns(void);
    const int samp = ps2_host_thread_sampling, tp = ps2_tprof_on(), speed_on = ps2_speed_enabled();
    const int timed = samp || speed_on;
    int open = 0;
    u64 busy = 0, busy_e = 0, bytes = 0, recs = 0, cyc0 = 0;
    pthread_mutex_lock(&b_lock);
    for (;;) {
        while (b_running && __atomic_load_n(&b_head, __ATOMIC_ACQUIRE) == b_tail) {
            __atomic_store_n(&b_busy, 0, __ATOMIC_SEQ_CST);
            pthread_cond_broadcast(&b_idle);
            if (__atomic_load_n(&b_head, __ATOMIC_SEQ_CST) != b_tail) break;
            if (samp) ps2_host_thread_idle();
            pthread_cond_wait(&b_not_empty, &b_lock);
        }
        if (!b_running && __atomic_load_n(&b_head, __ATOMIC_SEQ_CST) == b_tail) break;
        __atomic_store_n(&b_busy, 1, __ATOMIC_SEQ_CST);
        size_t tail = b_tail, head = __atomic_load_n(&b_head, __ATOMIC_ACQUIRE);
        unsigned records = 0;
        int field = 0, corrupt = 0;
        u64 elapsed = 0, fbusy = 0;
        pthread_mutex_unlock(&b_lock);
        u64 tb0 = timed ? ht_now_ns() : 0;
        if (tp && !open) { open = 1; ps2_tprof_ev_at(4, tb0, s2_field, 0, 0); }
        while (tail != head && records < 256) {
            const u8 *r = b_buf + (tail & (b_cap - 1));
            if (*r == GQ_WRAP) {
                tail += b_cap - (tail & (b_cap - 1));
                continue;
            }
            field = *r == GQ_FIELD;
            u64 start = field && speed_on ? gq_now_ns() : 0;
            if (field && timed) {
                u64 now = ht_now_ns(), d = now - tb0;
                tb0 = now;
                if (samp && ps2_host_thread_ran(d)) busy_e += d;
                busy += d;
                if (tp) ps2_tprof_ev_at(5, now, s2_field, busy, busy_e);
            }
            u32 n;
            { PS2_PHASE_BEGIN(PS2_PH_GFXQ);
              n = q_apply(r);
              PS2_PHASE_END(PS2_PH_GFXQ); }
            if (start) elapsed = gq_now_ns() - start;
            if (!n) { corrupt = 1; break; }
            tail += n;
            records++;
            bytes += n; recs++;
            if (field) {
                if (timed) {
                    u64 now = ht_now_ns(), d = now - tb0;
                    tb0 = now;
                    if (samp && ps2_host_thread_ran(d)) busy_e += d;
                    busy += d;
                    if (tp) {
                        u64 cyc = ps2_host_thread_cycles();
                        double rate = ps2_host_tsc_per_ns();
                        u64 cpu = cyc0 && cyc > cyc0 && rate > 0.0 ? (u64)((double)(cyc - cyc0) / rate) : 0;
                        ps2_tprof_ev_at(6, now, s2_field, busy, cpu);
                        ps2_tprof_ev_at(13, now, s2_field, bytes, recs);
                        cyc0 = cyc;
                    }
                }
                fbusy = busy;
                if (ps2_cz_on && fbusy) {                 /* (PS2_CPU_PROF; busy is measured with PS2_SPEED_LOG) */
                    u64 t = (u64)__rdtsc();
                    ps2_cz_add(CZ_GFX2_FIELD, t - (u64)((double)fbusy * cz_tick_ns), t);
                }
                s2_field++; open = 0; busy = busy_e = bytes = recs = 0;
                break;
            }
        }
        if (timed) {
            u64 d = ht_now_ns() - tb0;
            if (samp && ps2_host_thread_ran(d)) busy_e += d;
            busy += d;
        }
        pthread_mutex_lock(&b_lock);
        __atomic_store_n(&b_tail, tail, __ATOMIC_RELEASE);
        pthread_cond_broadcast(&b_not_full);
        if (field && speed_on) {
            s2_fields++; s2_busy_ns += fbusy;
            if (fbusy > s2_field_max) s2_field_max = fbusy;
            if (fbusy > 16666667ull) s2_field_long++;
        }
        if (corrupt) b_running = 0;
        if ((field && !corrupt) || corrupt) {
            pthread_mutex_unlock(&b_lock);
            pthread_mutex_lock(&q_lock);
            if (corrupt) { ps2_log("gfxq: stage 2 stopped on a corrupt record"); q_running = 0; }
            else q_field_done_locked(speed_on, elapsed);
            pthread_cond_broadcast(&q_not_full);
            pthread_cond_broadcast(&q_idle);
            pthread_mutex_unlock(&q_lock);
            pthread_mutex_lock(&b_lock);
            if (corrupt) break;
        }
    }
    __atomic_store_n(&b_busy, 0, __ATOMIC_SEQ_CST);
    pthread_cond_broadcast(&b_idle);
    pthread_cond_broadcast(&b_not_full);
    pthread_mutex_unlock(&b_lock);
    return NULL;
}

static void q_split_speed_log(void) {
    if (!b_buf) return;                  /* PS2_GFX_SPLIT: stage 2 per field, the rings between the stages */
    {
        extern u64 ps2_host_thread_cpu_ns(const char *who);
        static u64 pc2, pw, pwn, ps, psn, pb, pfl, pqw, pact, pirq, pwk;
        u64 fl, qw, act, irq;
        ps2_vu1_split_stats(&fl, &qw, &act, &irq);
        pthread_mutex_lock(&b_lock);
        u64 n2 = s2_fields, mx = s2_field_max, lg = s2_field_long, bz = s2_busy_ns;
        s2_fields = s2_field_max = s2_field_long = s2_busy_ns = 0;
        pthread_mutex_unlock(&b_lock);
        u64 c2 = ps2_host_thread_cpu_ns("gfx2");
        ps2_log("speed: graphics stage 2 (PS2_GFX_SPLIT, now %s): %llu fields, busy per field avg %.2f max %.2f ms, "
                "over_16.7ms=%llu, cpu_ms=%.0f; stage 1 waited on stage 2's ring %llu times (%.1f ms), synced %llu times "
                "(%.1f ms), woke it %llu times; forwarded %.1f MB, VU1 %.1f MB in %llu flushes, %llu activations, %llu "
                "interrupt syncs",
                q_split ? "on" : "off", (unsigned long long)n2, n2 ? bz / 1e6 / (double)n2 : 0.0, mx / 1e6,
                (unsigned long long)lg, (c2 - pc2) / 1e6, (unsigned long long)(st_b_full_waits - pw),
                (st_b_full_ns - pwn) / 1e6, (unsigned long long)(st_b_syncs - ps), (st_b_sync_ns - psn) / 1e6,
                (unsigned long long)(st_b_wakes - pwk), (st_b_bytes - pb) / 1048576.0, (double)(qw - pqw) * 16.0 / 1048576.0, (unsigned long long)(fl - pfl),
                (unsigned long long)(act - pact), (unsigned long long)(irq - pirq));
        pc2 = c2; pw = st_b_full_waits; pwn = st_b_full_ns; ps = st_b_syncs; psn = st_b_sync_ns; pb = st_b_bytes;
        pfl = fl; pqw = qw; pact = act; pirq = irq; pwk = st_b_wakes;
    }
}

static const char *q_forbidden;
void ps2_gfxq_forbid(const char *why) { if (!q_forbidden) q_forbidden = why; }

/* Windows scheduling of the game's threads. The EE, this worker and the renderer each have to finish a field's work
   in 16.7 ms; flying low over mission 2's forest this worker needs 11-16 ms per field (about 60% of it GS texture
   uploads and decodes), so background CPU load pushed fields over, the EE waited for the worker ("speed: EE wait_ms
   field") and the game itself slowed down (BUGS_PLAYTEST #13: the user's session overlapped cockpit AO bakes and
   builds). With 16 busy normal-priority threads beside the game: 54-57 game frames/s at normal priority, 60.0 with
   this. All 24 hardware threads busy still slow the worker (shared cores run it ~1.6x slower, whatever their
   priority). The process runs at ABOVE_NORMAL priority (ahead of normal-priority background work; the VR compositor's
   own threads stay above it) and opts out of power throttling (EcoQoS: efficiency cores, and timer resolution ignored
   while the window is hidden or behind others). PS2_PRIORITY=0 keeps Windows' defaults. */
static void host_priority(void) {
#ifdef _WIN32
    const char *e = getenv("PS2_PRIORITY");
    if (e && *e == '0') { ps2_log("host: PS2_PRIORITY=0 -- normal process priority, power throttling allowed"); return; }
    int cls = SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS) != 0;
    typedef BOOL (WINAPI *spi_fn)(HANDLE, PROCESS_INFORMATION_CLASS, LPVOID, DWORD);
    spi_fn spi = (spi_fn)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetProcessInformation");
    int qos = 0;
    if (spi) {
        PROCESS_POWER_THROTTLING_STATE st;
        memset(&st, 0, sizeof st);
        st.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        st.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
        st.StateMask = 0;                                    /* controlled and off: high QoS, timer resolution kept */
        qos = spi(GetCurrentProcess(), ProcessPowerThrottling, &st, sizeof st) != 0;
    }
    ps2_log("host: process priority %s, power throttling %s (PS2_PRIORITY=0 keeps the defaults)",
            cls ? "above normal" : "unchanged (failed)", qos ? "off" : "unchanged (failed)");
#endif
}

void ps2_gfxq_init(void) {
    const char *e = getenv("PS2_ASYNC_GFX");
    size_t mb;
    host_priority();
    ps2_cz_init();                                   /* PS2_CPU_PROF (CPU zones, diagnostic) */
    if (e && *e == '0') {
        ps2_log("gfxq: PS2_ASYNC_GFX=0 -- the graphics machine runs on the EE "
                "thread, as it did before.");
        return;
    }

    if (q_forbidden) {
        ps2_log("gfxq: PS2_ASYNC_GFX is set but %s, so the graphics machine "
                "stays on the EE thread.", q_forbidden);
        return;
    }

    mb = 64;
    { const char *m = getenv("PS2_ASYNC_MB");
      if (m && *m) { double v = atof(m); if (v >= 1.0 && v <= 1024.0) mb = (size_t)v; } }
    { const char *d = getenv("PS2_ASYNC_FIELDS");
      if (d && *d) { int v = atoi(d); if (v >= 1 && v <= 8) q_field_depth = v; } }
    q_cap = 1;
    while (q_cap < mb * 1024u * 1024u) q_cap <<= 1;
    q_buf = (u8 *)malloc(q_cap);
    if (!q_buf) {
        ps2_log("gfxq: cannot allocate a %zu MB ring; staying synchronous",
                q_cap >> 20);
        return;
    }
    q_head = q_tail = q_producer_tail = q_reserved_end = 0;
    q_running = 1;
    {   /* PS2_GFX_SPLIT / PS2_GFX_SPLIT_AB: the worker in two stages (stage 2 and its ring first) */
        const char *s = getenv("PS2_GFX_SPLIT"), *ab = getenv("PS2_GFX_SPLIT_AB");
        /* default on since ov_cpu 2026-10-25 (0 = one worker, as before): in-run A/B in the user's settings, the critical
           stage's busy time per field M4 5.16 -> 3.91 ms (-1.24 +- 0.08, 10 pairs; p99 -1.13 +- 0.15), M9 -0.99 +- 0.34,
           op_threads' M3 / M4 -1.25..-1.59; the EE +0.76 ms a field (it wakes stage 1; ~11 ms of slack), the recording
           +0.04, frame latency n.s., the field's publication +0.02 ms (p99 +0.06); identical results (op_threads) */
        q_split_cfg = !(s && *s == '0');
        q_split_ab_s = ab ? atoi(ab) : 0;
        if (q_split_ab_s < 0) q_split_ab_s = 0;
        if (q_split_cfg || q_split_ab_s > 0) {
            b_cap = q_cap;
            b_buf = (u8 *)malloc(b_cap);
            b_head = b_tail = b_wr = b_ptail = 0;
            b_running = b_buf != NULL;
            if (b_buf && pthread_create(&b_thread, NULL, b_main, NULL) != 0) { free(b_buf); b_buf = NULL; b_running = 0; }
            q_split = q_split_wanted();
            ps2_vif1_split = q_split;
            ps2_log("gfxq: %s", !b_buf ? "PS2_GFX_SPLIT: no second stage (allocation or thread failed); one worker"
                    : q_split_ab_s > 0 ? "the worker in two stages in odd blocks of PS2_GFX_SPLIT_AB seconds, one worker "
                                         "in the even ones (in-run A/B)"
                    : "the worker in two stages (PS2_GFX_SPLIT): VIF1 on this thread, VU1/GIF/GS/present on a second");
        }
    }
    {   /* PS2_GFXQ_DOORBELL (perf6_threads): bytes between rings; unset = the thread policy's (64 KB when on, else 0) */
        const char *d = getenv("PS2_GFXQ_DOORBELL");
        int pol = 0;
        ps2_thread_policy(&pol, NULL, NULL);
        q_db_bytes = d && *d ? (atoi(d) > 0 ? (u32)atoi(d) : 0u) : pol ? tpol.db : 0u;
        if (tpol_ab & 4) q_db_bytes = 0;              /* (PS2_CPU_AB: the first field picks its block's) */
        q_db_on = q_db_bytes != 0;
        q_db_mark = 0;
        if (tpol_ab & 4)
            ps2_log("gfxq: the EE rings the graphics worker every %u bytes in the B blocks of PS2_CPU_AB, after every record "
                    "in the A blocks (in-run A/B)", tpol.db);
        else if (q_db_bytes)
            ps2_log("gfxq: the EE publishes each record at once and rings the graphics worker every %u bytes, at every "
                    "field and before it blocks or polls (PS2_GFXQ_DOORBELL%s)", q_db_bytes, d && *d ? "" : ": thread policy");
    }
    if (pthread_create(&q_thread, NULL, q_main, NULL) != 0) {
        ps2_log("gfxq: cannot start the worker thread; staying synchronous");
        free(q_buf); q_buf = NULL; q_running = 0;
        return;
    }
    ps2_gfxq_on = 1;
    ps2_log("gfxq: the graphics machine (VIF, VU1, GIF, GS) runs on its own "
            "thread; %zu MB ring", q_cap >> 20);
}

void ps2_gfxq_drain(void) {
    u64 t0;
    if (!ps2_gfxq_on || q_is_worker) return;
    t0 = gq_now_ns();
    pthread_mutex_lock(&q_lock);
    st_drains++;
    if (q_head != q_tail || q_busy || q_fields_pending) {
        st_drain_waits++;
        pthread_cond_signal(&q_not_empty);
        while (q_running && (q_head != q_tail || q_busy || q_fields_pending))
            pthread_cond_wait(&q_idle, &q_lock);
    }
    pthread_mutex_unlock(&q_lock);
    if (b_buf) {                                     /* (PS2_GFX_SPLIT: stage 1 is idle and has published all) */
        pthread_mutex_lock(&b_lock);
        while (b_running && (__atomic_load_n(&b_head, __ATOMIC_ACQUIRE) != b_tail || b_busy))
            pthread_cond_wait(&b_idle, &b_lock);
        pthread_mutex_unlock(&b_lock);
    }
    st_drain_ns += gq_now_ns() - t0;
}

void ps2_gfxq_shutdown(void) {
    if (!ps2_gfxq_on) return;
    ps2_gfxq_drain();
    pthread_mutex_lock(&q_lock);
    q_running = 0;
    pthread_cond_broadcast(&q_not_empty);
    pthread_mutex_unlock(&q_lock);
    pthread_join(q_thread, NULL);
    if (b_buf) {
        pthread_mutex_lock(&b_lock);
        b_running = 0;
        pthread_cond_broadcast(&b_not_empty);
        pthread_mutex_unlock(&b_lock);
        pthread_join(b_thread, NULL);
    }
    ps2_gfxq_on = 0;
}

int ps2_gfxq_blocks_capture(void) { return ps2_gfxq_on; }

void ps2_gfxq_report(void) {
    if (!st_records) return;
    ps2_log("gfxq: %llu records, %llu MB queued, peak fill %zu KB of %zu MB",
            (unsigned long long)st_records,
            (unsigned long long)(st_bytes >> 20),
            st_peak_fill >> 10, q_cap >> 20);
    ps2_log("gfxq: %llu drains, %llu of them actually waited (%.1f ms total); "
            "the EE waited on a full ring %llu times (%.1f ms total)",
            (unsigned long long)st_drains, (unsigned long long)st_drain_waits,
            (double)st_drain_ns / 1e6,
            (unsigned long long)st_producer_waits,
            (double)st_producer_ns / 1e6);
    ps2_log("gfxq: the EE waited for the graphics machine to catch up on "
            "fields %llu times (%.1f ms total), at a depth of %d",
            (unsigned long long)st_field_waits,
            (double)st_field_wait_ns / 1e6, q_field_depth);
    if (st_producer_waits)
        ps2_log("gfxq:   a non-zero full-ring count means the graphics machine "
                "is the constraint, not the EE -- a bigger ring will not help.");
}

void ps2_gfxq_vif_qw(int ch, const ps2_reg128 *qw, u32 qwc) {
    if (!ps2_gfxq_on) {
        ps2_vif_transfer(ch, qw, qwc);
        return;
    }
    if (!qwc) return;
    {
        u32 n = 6u + qwc * 16u;
        u8 *r = q_reserve(n);
        put8(r, 0, GQ_VIF_QW);
        put8(r, 1, (u8)ch);
        put32(r, 2, qwc);
        memcpy(r + 6, qw, (size_t)qwc * 16u);
        q_commit(n);
    }
}

void ps2_gfxq_gif_qw(const ps2_reg128 *qw, u32 qwc) {
    if (!ps2_gfxq_on) { ps2_gif_transfer(qw, qwc); return; }
    if (!qwc) return;
    {
        u32 n = 5u + qwc * 16u;
        u8 *r = q_reserve(n);
        put8(r, 0, GQ_GIF_QW);
        put32(r, 1, qwc);
        memcpy(r + 5, qw, (size_t)qwc * 16u);
        q_commit(n);
    }
}

void ps2_gfxq_vif_word(int ch, u32 word) {
    if (!ps2_gfxq_on) { ps2_vif_write(ch, word); return; }
    { u8 *r = q_reserve(6);
      put8(r, 0, GQ_VIF_W); put8(r, 1, (u8)ch); put32(r, 2, word);
      q_commit(6); }
}

void ps2_gfxq_fbrst(int ch, u32 val) {
    if (!ps2_gfxq_on) { ps2_vif_fbrst(ch, val); return; }
    { u8 *r = q_reserve(6);
      put8(r, 0, GQ_FBRST); put8(r, 1, (u8)ch); put32(r, 2, val);
      q_commit(6); }
}

void ps2_gfxq_vif_clear(int ch) {
    if (!ps2_gfxq_on) { ps2_vif_clear_stall(ch); return; }
    { u8 *r = q_reserve(2);
      put8(r, 0, GQ_VIFCLR); put8(r, 1, (u8)ch);
      q_commit(2); }
}

void ps2_gfxq_gs_priv(u32 addr, u64 val) {
    if (!ps2_gfxq_on) { ps2_gs_priv_write_now(addr, val); return; }
    { u8 *r = q_reserve(13);
      put8(r, 0, GQ_GS_PRIV); put32(r, 1, addr); put64(r, 5, val);
      q_commit(13); }
}

int ps2_gfxq_field(void) {
    if (tpol_ab) tpol_field();                        /* (perf6_threads: PS2_CPU_AB blocks of the thread policy) */
    if (!ps2_gfxq_on) {
        u64 start = ps2_speed_enabled() ? gq_now_ns() : 0;
        int result = ps2_gs_present_now();
        if (start) {
            speed_present_ns += gq_now_ns()-start; speed_present_n++;
            ps2_vu1_speed_counters(&speed_vu_runs,&speed_vu_insns,&speed_vu_guards);
        }
        return result;
    }
    u64 tp_t0 = ps2_host_thread_sampling ? ht_now_ns() : 0, tp_wait = 0;
    pthread_mutex_lock(&q_lock);
    if (q_fields_pending >= q_field_depth) {
        u64 t0 = gq_now_ns();
        st_field_waits++;
        if (q_db_bytes) pthread_cond_signal(&q_not_empty);       /* (doorbell: before the EE blocks) */
        while (q_running && q_fields_pending >= q_field_depth)
            pthread_cond_wait(&q_not_full, &q_lock);
        st_field_wait_ns += gq_now_ns() - t0;
        tp_wait = gq_now_ns() - t0;
    }
    q_fields_pending++;
    pthread_mutex_unlock(&q_lock);
    if (tp_t0 && ps2_tprof_on()) {             /* (host threads: the field timeline) */
        ps2_tprof_ev_at(2, tp_t0, tp_ee_field, (u64)(q_head - __atomic_load_n(&q_tail, __ATOMIC_RELAXED)), tp_wait);
        ps2_tprof_ee_idle_pending = ++tp_ee_field;
    } else if (ps2_cz_on) ps2_tprof_ee_idle_pending = ++tp_ee_field;
    if (ps2_cz_on) cz_ee_t0 = (u64)__rdtsc();                 /* (PS2_CPU_PROF: the EE's work for the next field) */
    { u8 *r = q_reserve(1);
      put8(r, 0, GQ_FIELD);
      q_commit(1);
      if (q_db_bytes) q_ring(); }                     /* (doorbell: every field at once) */
    if (PS2_UNLIKELY(ps2_cap_ee_hook)) {         /* a DMA-level capture: the EE writes its field records (bug #111) */
        ps2_gfxq_drain();
        ps2_cap_ee_field();
    }
    return 1;
}

void ps2_gfxq_mmio(u32 addr, u32 val) {
    if (!ps2_gfxq_on) { ps2_gif_ctrl_write(addr, val); return; }
    { u8 *r = q_reserve(9);
      put8(r, 0, GQ_MMIO); put32(r, 1, addr); put32(r, 5, val);
      q_commit(9); }
}

void ps2_gfxq_tag(u32 kind, u32 a, u32 b) {
    if (!ps2_gfxq_on) { rn_gs_tag(kind, a, b); return; }
    { u8 *r = q_reserve(13);
      put8(r, 0, GQ_TAG); put32(r, 1, kind); put32(r, 5, a); put32(r, 9, b);
      q_commit(13); }
}

void ps2_gfxq_intent(const u8 *rec, u32 len) {
    if (!ps2_gfxq_on) { rn_gs_intent(rec, len); return; }
    { u8 *r = q_reserve(5u + len);
      put8(r, 0, GQ_INTENT); put32(r, 1, len);
      memcpy(r + 5, rec, len);
      q_commit(5u + len); }
}
