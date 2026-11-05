#include "ps2_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <intrin.h>
#include <pthread.h>

#define HP_BITS 16
#define HP_N (1u << HP_BITS)
/* PS2_PROFILE_HOST=who[,who...] (ov_cpu 2026-10-25: several threads in one run): one table per sampled thread; the
   sampler visits them in turn every 1 ms tick (each thread is then sampled every N ms for N threads). One thread
   writes PS2_PROFILE_HOST_OUT as before; several write it with ".<who>" before the extension. */
#define HP_T 4
typedef struct { u64 rip, n; } hp_ent;
static struct { HANDLE h; char who[32]; hp_ent *tab; u64 total, dropped; } hp_t[HP_T];
static volatile int hp_nt;                           /* targets attached */
static int hp_want;                                  /* names in PS2_PROFILE_HOST */
static volatile int hp_running;
static pthread_t hp_thread;
static pthread_mutex_t hp_mx = PTHREAD_MUTEX_INITIALIZER;

static void hp_record(int k, u64 rip) {
    static double start = -1.0;
    if (start < 0.0) {
        const char *e = getenv("PS2_PROFILE_HOST_START");   /* seconds of wall time to skip */
        start = e ? atof(e) : 0.0;
    }
    if (start > 0.0 && ps2_wall_seconds() < start) return;
    hp_ent *tab = hp_t[k].tab;
    u32 i = (u32)((rip * 0x9E3779B97F4A7C15ull) >> (64 - HP_BITS));
    for (u32 n = 0; n < HP_N; n++, i = (i + 1u) & (HP_N - 1u)) {
        if (tab[i].rip == rip) { tab[i].n++; hp_t[k].total++; return; }
        if (!tab[i].rip) { tab[i].rip = rip; tab[i].n = 1; hp_t[k].total++; return; }
    }
    hp_t[k].dropped++;
}
static void hp_path(int k, char *out, size_t cap) {
    const char *path = getenv("PS2_PROFILE_HOST_OUT");
    if (!path || !*path) path = "out/host_profile.txt";
    if (hp_want <= 1) { snprintf(out, cap, "%s", path); return; }
    const char *dot = strrchr(path, '.'), *sl = strrchr(path, '/');
    if (!dot || (sl && dot < sl)) { snprintf(out, cap, "%s.%s", path, hp_t[k].who); return; }
    snprintf(out, cap, "%.*s.%s%s", (int)(dot - path), path, hp_t[k].who, dot);
}

/* the loaded modules as "module OFFSET SIZE PATH" lines (OFFSET from the exe's base, like the samples), so samples in the
   driver, ntdll (waits) or the OpenXR runtime can be told apart; K32* through GetProcAddress (no psapi import) */
static void hp_modules(FILE *f, u64 base) {
    typedef BOOL (WINAPI *enum_fn)(HANDLE, HMODULE *, DWORD, LPDWORD);
    typedef BOOL (WINAPI *info_fn)(HANDLE, HMODULE, LPMODULEINFO, DWORD);
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    enum_fn en = k ? (enum_fn)(void (*)(void))GetProcAddress(k, "K32EnumProcessModules") : NULL;
    info_fn inf = k ? (info_fn)(void (*)(void))GetProcAddress(k, "K32GetModuleInformation") : NULL;
    static HMODULE mods[1024];
    DWORD need = 0;
    if (!en || !inf || !en(GetCurrentProcess(), mods, sizeof mods, &need)) return;
    for (DWORD i = 0; i < need / sizeof(HMODULE) && i < 1024u; i++) {
        MODULEINFO mi; char name[MAX_PATH];
        if (!inf(GetCurrentProcess(), mods[i], &mi, sizeof mi)) continue;
        if (!GetModuleFileNameA(mods[i], name, sizeof name)) continue;
        fprintf(f, "module %lld %lu %s\n", (long long)((u64)(uintptr_t)mi.lpBaseOfDll - base), (unsigned long)mi.SizeOfImage,
                name);
    }
}

static void hp_write(int k) {
    char path[600];
    hp_path(k, path, sizeof path);
    u64 base = (u64)(uintptr_t)GetModuleHandleW(NULL);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "thread %s\nsamples %llu\ndropped %llu\n", hp_t[k].who,
            (unsigned long long)hp_t[k].total, (unsigned long long)hp_t[k].dropped);
    for (u32 i = 0; i < HP_N; i++)
        if (hp_t[k].tab[i].rip)
            fprintf(f, "%lld %llu\n", (long long)(hp_t[k].tab[i].rip - base),
                    (unsigned long long)hp_t[k].tab[i].n);
    hp_modules(f, base);
    fclose(f);
}

static void *hp_main(void *unused) {
    HANDLE t = CreateWaitableTimerExW(NULL, NULL, 0x00000002 ,
                                      TIMER_ALL_ACCESS);
    (void)unused;
    int k = 0;
    while (hp_running) {
        CONTEXT c;
        if (t) {
            LARGE_INTEGER due;
            due.QuadPart = -10000;
            SetWaitableTimer(t, &due, 0, NULL, NULL, FALSE);
            WaitForSingleObject(t, INFINITE);
        } else {
            Sleep(1);
        }
        int nt = __atomic_load_n(&hp_nt, __ATOMIC_ACQUIRE);
        if (!nt) continue;
        k = (k + 1) % nt;
        HANDLE h = hp_t[k].h;
        if (SuspendThread(h) == (DWORD)-1) continue;
        memset(&c, 0, sizeof c);
        c.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(h, &c)) hp_record(k, (u64)c.Rip);
        ResumeThread(h);
        /* processes stopped by a timeout never reach ps2_host_prof_report: keep a running copy */
        if (hp_t[k].total && hp_t[k].total % 5000u == 0) hp_write(k);
    }
    if (t) CloseHandle(t);
    return NULL;
}

/* CPU time of the named host threads (ee, gfx, vk) for the speed log: each registers itself on attach */
static HANDLE hp_cpu_thread[4];
static int hp_cpu_index(const char *who) {
    return !strcmp(who, "ee") ? 0 : !strcmp(who, "gfx") ? 1 : !strcmp(who, "vk") ? 2 : !strcmp(who, "gfx2") ? 3 : -1;
}
u64 ps2_host_thread_cpu_ns(const char *who) {
    int k = hp_cpu_index(who);
    FILETIME c, x, kt, ut;
    if (k < 0 || !hp_cpu_thread[k] || !GetThreadTimes(hp_cpu_thread[k], &c, &x, &kt, &ut)) return 0;
    return ((((u64)kt.dwHighDateTime << 32) | kt.dwLowDateTime) + (((u64)ut.dwHighDateTime << 32) | ut.dwLowDateTime)) * 100ull;
}

/* PS2_REC_PROF: the calling thread's CPU time in TSC ticks (QueryThreadCycleTime; on current Windows it accumulates
   TSC ticks at context switches) and the TSC rate in ticks per ns (calibrated against QueryPerformanceCounter after
   0.2 s; 0 before) -- CPU time against wall time tells work from waiting */
u64 ps2_host_thread_cycles(void) {
    typedef BOOL (WINAPI *qtct_fn)(HANDLE, PULONG64);
    static qtct_fn fn; static int init;
    if (!init) {
        HMODULE k = GetModuleHandleW(L"kernel32.dll");
        init = 1;
        fn = k ? (qtct_fn)(void (*)(void))GetProcAddress(k, "QueryThreadCycleTime") : NULL;
    }
    ULONG64 c = 0;
    return fn && fn(GetCurrentThread(), &c) ? (u64)c : 0;
}
double ps2_host_tsc_per_ns(void) {
    static LARGE_INTEGER q0, qf; static u64 t0; static double rate;
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    u64 t = __rdtsc();
    if (!t0) { QueryPerformanceFrequency(&qf); q0 = q; t0 = t; return 0.0; }
    double ns = (double)(q.QuadPart - q0.QuadPart) * 1e9 / (double)qf.QuadPart;
    if (ns > 2e8) rate = (double)(t - t0) / ns;
    return rate;
}

void ps2_host_prof_attach(const char *who) {
    const char *e = getenv("PS2_PROFILE_HOST");
    {   extern void ps2_host_thread_start(const char *who);      /* placement and core sampling (ps2_gfxq.c) */
        ps2_host_thread_start(who);
    }
    {   int k = hp_cpu_index(who);
        if (k >= 0 && !hp_cpu_thread[k])
            DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &hp_cpu_thread[k],
                            THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0);
    }
    if (!e || !*e || *e == '0') return;
    /* PS2_PROFILE_HOST=ee|vk|gfx|gfx2[,...] picks the threads; any other value means the GS thread ("gfx") */
    int named = 0, mine = 0, nw = 0;
    {   char buf[128]; snprintf(buf, sizeof buf, "%s", e);
        for (char *p = strtok(buf, ","); p; p = strtok(NULL, ",")) {
            int ok = !strcmp(p, "ee") || !strcmp(p, "vk") || !strcmp(p, "gfx") || !strcmp(p, "gfx2");
            named |= ok; nw += ok;
            if (ok && !strcmp(p, who)) mine = 1;
        }
    }
    if (named ? !mine : strcmp(who, "gfx") != 0) return;
    pthread_mutex_lock(&hp_mx);
    hp_want = named ? nw : 1;
    int k = hp_nt;
    if (k >= HP_T) { pthread_mutex_unlock(&hp_mx); return; }
    for (int i = 0; i < k; i++) if (!strcmp(hp_t[i].who, who)) { pthread_mutex_unlock(&hp_mx); return; }  /* (once) */
    hp_t[k].tab = (hp_ent *)calloc(HP_N, sizeof(hp_ent));
    if (!hp_t[k].tab || !DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                         &hp_t[k].h, THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT
                         | THREAD_QUERY_INFORMATION, FALSE, 0)) {
        ps2_log("host profile: cannot open the %s thread", who);
        free(hp_t[k].tab); hp_t[k].tab = NULL;
        pthread_mutex_unlock(&hp_mx);
        return;
    }
    snprintf(hp_t[k].who, sizeof hp_t[k].who, "%s", who);
    __atomic_store_n(&hp_nt, k + 1, __ATOMIC_RELEASE);
    if (!hp_running) {
        hp_running = 1;
        if (pthread_create(&hp_thread, NULL, hp_main, NULL) != 0) hp_running = 0;
    }
    pthread_mutex_unlock(&hp_mx);
    ps2_log("host profile: sampling the %s thread%s", who, hp_want > 1 ? " (one of several, in turn)" : "");
}

void ps2_host_prof_report(void) {
    if (!hp_running) return;
    hp_running = 0;
    pthread_join(hp_thread, NULL);
    for (int k = 0; k < hp_nt; k++) {
        char path[600];
        CloseHandle(hp_t[k].h);
        hp_write(k);
        hp_path(k, path, sizeof path);
        ps2_log("host profile: %llu samples of the %s thread written to %s",
                (unsigned long long)hp_t[k].total, hp_t[k].who, path);
    }
}

#else

void ps2_host_prof_attach(const char *who) { (void)who; }
u64 ps2_host_thread_cpu_ns(const char *who) { (void)who; return 0; }
u64 ps2_host_thread_cycles(void) { return 0; }
double ps2_host_tsc_per_ns(void) { return 0.0; }
void ps2_host_prof_report(void) {}

#endif
