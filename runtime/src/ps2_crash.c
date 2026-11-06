/* ps2_crash.c -- the crash report (ACZ_CRASH_REPORT, default 1; night_setup 2026-11-04; RELEASE_RESEARCH R4 item 4).
   ps2_main.c's exception filter calls ps2_crash_report(): crash_<stamp>.dmp (MiniDumpWriteDump) and crash_<stamp>.txt (what
   happened, the exe, the last 200 log lines) go to crash/ next to the exe, or to %TEMP%\ac5_crash when that folder cannot be
   written. new_report.txt in that folder names the newest report; the next start logs it and removes the marker. The eight
   newest reports are kept. ps2_fatal_report() is the same text report without a minidump, for fatal conditions that are not
   exceptions (ps2_fatal calls it; the stall exit and a lost GPU device are the next callers, stab_audit U5, U8). Nothing is
   written unless something fails: before a crash this is one sleeping thread and a memcpy per log line.
   The dump leaves out heaps and module paths (no guest memory, no folder names); the log lines can hold folder names, and
   nothing is ever sent anywhere.
   The writing happens on a thread of its own, started at start-up: a stack overflow leaves no stack to write from, and
   MiniDumpWriteDump should not run on the thread it describes. dbghelp.dll is loaded from System32 at start-up, not during
   the crash, when the loader lock or the heap may be taken.
   ACZ_CRASH_TEST=av|so|abort|fatal (test only, tools/docs/env_template.md): from field ACZ_CRASH_TEST_FIELD (default 300) a
   new thread writes to a null pointer, overflows its stack, calls abort(), or asks for a fatal report and the game goes on. */
#include "ps2_crash.h"
#include "ps2_profile.h"
#include "ps2_runtime.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <signal.h>

#define RING_LINES     300                     /* 200 before the crash + the filter's own lines (frames, call ring) */
#define RING_LINE_MAX  384
#define SHOW_LINES     200
#define KEEP_REPORTS   8
#define MARKER_NAME    L"new_report.txt"

extern u32 ps2_gs_frame_count;

static int crash_on;
static char ring[RING_LINES][RING_LINE_MAX];
static short ring_len[RING_LINES];
static volatile LONG ring_pos;                 /* lines so far */
static volatile LONG ring_mark = -1;           /* ring_pos when the filter started */

/* minidump: the signature of MiniDumpWriteDump without dbghelp.h's import library (its structures are packed to 4) */
#pragma pack(push, 4)
typedef struct {
    DWORD ThreadId;
    EXCEPTION_POINTERS *ExceptionPointers;
    BOOL ClientPointers;
} crash_mdei;
#pragma pack(pop)
typedef BOOL (WINAPI *minidump_fn)(HANDLE, DWORD, HANDLE, DWORD, crash_mdei *, void *, void *);
static minidump_fn minidump_write;
#define DUMP_FLAGS (0x00000000u /* MiniDumpNormal */ | 0x00000080u /* MiniDumpFilterModulePaths */ \
                    | 0x00000020u /* MiniDumpWithUnloadedModules */ | 0x00001000u /* MiniDumpWithThreadInfo */)

static HANDLE ev_request, ev_done;
static const char *req_why;
static EXCEPTION_POINTERS *req_ep;
static DWORD req_tid;
static LPTOP_LEVEL_EXCEPTION_FILTER installed_filter;
static const char *test_kind;
static u32 test_field;

int ps2_crash_enabled(void) { return crash_on; }

void ps2_crash_log_line(const char *line, int len) {
    LONG i;
    unsigned k;
    if (!crash_on) return;
    i = InterlockedIncrement(&ring_pos) - 1;
    k = (unsigned)i % RING_LINES;
    if (len < 0) len = 0;
    if (len > RING_LINE_MAX) len = RING_LINE_MAX;
    memcpy(ring[k], line, (size_t)len);
    ring_len[k] = (short)len;
}

void ps2_crash_mark(void) { InterlockedCompareExchange(&ring_mark, ring_pos, -1); }

/* ---- files (wide names: the folder may be anything the player has) ---- */
static int exe_folder(wchar_t *out, unsigned cap) {
    DWORD n = GetModuleFileNameW(NULL, out, cap);
    wchar_t *sl;
    if (!n || n >= cap) return 0;
    sl = wcsrchr(out, L'\\');
    if (!sl) return 0;
    sl[1] = 0;
    return 1;
}

static int folder_exe(wchar_t *out, unsigned cap) {
    if (!exe_folder(out, cap) || wcslen(out) + 8 >= cap) return 0;
    wcscat(out, L"crash");
    return 1;
}

static int folder_temp(wchar_t *out, unsigned cap) {
    DWORD n = GetTempPathW(cap, out);
    if (!n || n + 12 >= cap) return 0;
    wcscat(out, L"ac5_crash");
    return 1;
}

static int folder_ready(const wchar_t *dir) {
    if (CreateDirectoryW(dir, NULL) || GetLastError() == ERROR_ALREADY_EXISTS) return 1;
    return 0;
}

static void put(HANDLE h, const char *s, size_t n) {
    DWORD w;
    if (n) WriteFile(h, s, (DWORD)n, &w, NULL);
}

static void putf(HANDLE h, const char *fmt, ...) {
    char b[1024];
    int n;
    va_list ap;
    va_start(ap, fmt);
    n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n > 0) put(h, b, (size_t)(n < (int)sizeof b ? n : (int)sizeof b - 1));
}

static void to_utf8(const wchar_t *w, char *out, int cap) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL);
    if (n <= 0) out[0] = 0;
}

/* The report. ep == NULL and no dump wanted: the fatal report. Uses the stack, Win32 and vsnprintf, not the heap. */
static char last_note[MAX_PATH * 3 + 300];

static void describe(EXCEPTION_POINTERS *ep, char *out, size_t cap) {
    const EXCEPTION_RECORD *r = ep ? ep->ExceptionRecord : NULL;
    const char *b = (const char *)GetModuleHandleA(NULL);
    if (!r) { snprintf(out, cap, "unknown"); return; }
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2)
        snprintf(out, cap, "exception %08lX at %p (image %p, rva %08llX), %s address %p", (unsigned long)r->ExceptionCode,
                 r->ExceptionAddress, (const void *)b, (unsigned long long)((const char *)r->ExceptionAddress - b),
                 r->ExceptionInformation[0] ? "writing" : "reading", (void *)r->ExceptionInformation[1]);
    else
        snprintf(out, cap, "exception %08lX at %p (image %p, rva %08llX)", (unsigned long)r->ExceptionCode,
                 r->ExceptionAddress, (const void *)b, (unsigned long long)((const char *)r->ExceptionAddress - b));
}

static void write_report(const char *why, EXCEPTION_POINTERS *ep, DWORD tid, int want_dump) {
    wchar_t dir[MAX_PATH], base[MAX_PATH], path[MAX_PATH + 16];
    char dumpnote[300], u8[MAX_PATH * 3], whatbuf[256];
    SYSTEMTIME st;
    HANDLE h = INVALID_HANDLE_VALUE;
    LONG pos, from, end, mark;
    DWORD dump_line_at;
    int attempt;

    last_note[0] = 0;
    if (!why) {
        describe(ep, whatbuf, sizeof whatbuf);
        why = whatbuf;
    }
    if (!(folder_exe(dir, MAX_PATH) && folder_ready(dir))
        && !(folder_temp(dir, MAX_PATH) && folder_ready(dir))) {
        snprintf(last_note, sizeof last_note, "crash: no folder to write the report to");
        return;
    }
    GetLocalTime(&st);
    for (attempt = 0; attempt < 100; attempt++) {           /* two reports in one second get _1, _2 ... */
        if (attempt)
            _snwprintf(base, MAX_PATH, L"%ls\\crash_%04u-%02u-%02u_%02u-%02u-%02u_%d", dir, st.wYear, st.wMonth, st.wDay,
                       st.wHour, st.wMinute, st.wSecond, attempt);
        else
            _snwprintf(base, MAX_PATH, L"%ls\\crash_%04u-%02u-%02u_%02u-%02u-%02u", dir, st.wYear, st.wMonth, st.wDay,
                       st.wHour, st.wMinute, st.wSecond);
        base[MAX_PATH - 1] = 0;
        _snwprintf(path, MAX_PATH + 16, L"%ls.txt", base);
        path[MAX_PATH + 15] = 0;
        h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE || GetLastError() != ERROR_FILE_EXISTS) break;
    }
    if (h == INVALID_HANDLE_VALUE) {
        snprintf(last_note, sizeof last_note, "crash: cannot create the report file (%lu)", (unsigned long)GetLastError());
        return;
    }

    putf(h, "Crash report of the game (ACZ_CRASH_REPORT=1). Nothing was sent anywhere. To report the problem, attach this\r\n"
            "file and the .dmp; the log lines below can hold your folder names, so read them first.\r\n\r\n");
    putf(h, "what:     %s\r\n", why ? why : "?");
    putf(h, "when:     %04u-%02u-%02u %02u:%02u:%02u (local)\r\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    putf(h, "version:  %s\r\n", ps2_version_line());
    putf(h, "field:    %u\r\n", (unsigned)ps2_gs_frame_count);
    {
        wchar_t ex[MAX_PATH];
        const char *b = (const char *)GetModuleHandleA(NULL);
        const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(b + ((const IMAGE_DOS_HEADER *)b)->e_lfanew);
        DWORD n = GetModuleFileNameW(NULL, ex, MAX_PATH);
        const wchar_t *nm = n ? wcsrchr(ex, L'\\') : NULL;
        to_utf8(nm ? nm + 1 : L"?", u8, sizeof u8);
        putf(h, "exe:      %s, link stamp %08lX, image at %p\r\n", u8, (unsigned long)nt->FileHeader.TimeDateStamp, (const void *)b);
    }
#if defined(__VERSION__)
    putf(h, "compiler: %s\r\n", __VERSION__);
#endif
    {
        typedef LONG (WINAPI *rtlgv_fn)(RTL_OSVERSIONINFOW *);
        rtlgv_fn gv = (rtlgv_fn)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion");
        RTL_OSVERSIONINFOW v;
        memset(&v, 0, sizeof v);
        v.dwOSVersionInfoSize = sizeof v;
        if (gv && gv(&v) == 0)
            putf(h, "windows:  %lu.%lu build %lu\r\n", (unsigned long)v.dwMajorVersion, (unsigned long)v.dwMinorVersion,
                 (unsigned long)v.dwBuildNumber);
    }
    /* a fixed-width line, filled in when the dump is done */
    dump_line_at = SetFilePointer(h, 0, NULL, FILE_CURRENT);
    snprintf(dumpnote, sizeof dumpnote, "%s", want_dump ? "not finished" : "none (a report without a minidump)");
    putf(h, "dump:     %-120s\r\n", dumpnote);

    pos = ring_pos;
    mark = ring_mark;
    end = pos;
    from = (mark >= 0 ? mark : end) - SHOW_LINES;
    if (from < end - RING_LINES) from = end - RING_LINES;
    if (from < 0) from = 0;
    putf(h, "\r\n---- the last %ld log lines, oldest first ----\r\n", (long)(end - from));
    for (; from < end; from++) {
        unsigned k = (unsigned)from % RING_LINES;
        int n = ring_len[k];
        if (n > 0 && ring[k][n - 1] == '\n') n--;
        put(h, ring[k], (size_t)n);
        put(h, "\r\n", 2);
    }
    FlushFileBuffers(h);

    /* the marker for the next start */
    {
        wchar_t mpath[MAX_PATH + 16];
        HANDLE m;
        _snwprintf(mpath, MAX_PATH + 16, L"%ls\\%ls", dir, MARKER_NAME);
        mpath[MAX_PATH + 15] = 0;
        m = CreateFileW(mpath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (m != INVALID_HANDLE_VALUE) {
            to_utf8(path, u8, sizeof u8);
            putf(m, "%s\r\n%s\r\n", u8, why ? why : "?");
            CloseHandle(m);
        }
    }
    /* the dump last: it may hang on a lock of a suspended thread, and the caller's wait then ends the process */
    if (want_dump) {
        if (!minidump_write) {
            snprintf(dumpnote, sizeof dumpnote, "none (dbghelp.dll not available)");
        } else {
            wchar_t dpath[MAX_PATH + 16];
            HANDLE d;
            _snwprintf(dpath, MAX_PATH + 16, L"%ls.dmp", base);
            dpath[MAX_PATH + 15] = 0;
            d = CreateFileW(dpath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (d == INVALID_HANDLE_VALUE) {
                snprintf(dumpnote, sizeof dumpnote, "none (cannot create the file, error %lu)", (unsigned long)GetLastError());
            } else {
                crash_mdei mei = { tid, ep, FALSE };
                BOOL ok = minidump_write(GetCurrentProcess(), GetCurrentProcessId(), d, DUMP_FLAGS, ep ? &mei : NULL, NULL, NULL);
                DWORD err = GetLastError(), hi = 0, lo = GetFileSize(d, &hi);
                CloseHandle(d);
                if (ok) snprintf(dumpnote, sizeof dumpnote, "the .dmp beside this file, %lu bytes", (unsigned long)lo);
                else snprintf(dumpnote, sizeof dumpnote, "none (MiniDumpWriteDump failed, error %08lX)", (unsigned long)err);
            }
        }
        SetFilePointer(h, (LONG)dump_line_at, NULL, FILE_BEGIN);
        putf(h, "dump:     %-120s\r\n", dumpnote);
    }
    CloseHandle(h);
    to_utf8(path, u8, sizeof u8);
    snprintf(last_note, sizeof last_note, "crash: %s: report written: %s (dump: %s)", why, u8, dumpnote);
}

/* ---- the next start ---- */
static void scan_folder(const wchar_t *dir) {
    wchar_t mpath[MAX_PATH + 32], pat[MAX_PATH + 32];
    char text[MAX_PATH * 3 + 300];
    HANDLE m;
    WIN32_FIND_DATAW fd;
    HANDLE f;
    wchar_t (*names)[64] = NULL;
    int count = 0, cap = 256;

    _snwprintf(mpath, MAX_PATH + 32, L"%ls\\%ls", dir, MARKER_NAME);
    mpath[MAX_PATH + 31] = 0;
    m = CreateFileW(mpath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (m != INVALID_HANDLE_VALUE) {
        DWORD got = 0;
        ReadFile(m, text, sizeof text - 1, &got, NULL);
        CloseHandle(m);
        text[got] = 0;
        {
            char *second = strstr(text, "\r\n");
            const char *what = "";
            if (second) {
                char *e;
                *second = 0;
                what = second + 2;
                e = strstr(second + 2, "\r\n");
                if (e) *e = 0;
            }
            ps2_log("crash: the last run ended with a crash report: %s (%s)", text, what);
        }
        DeleteFileW(mpath);
    }

    /* keep the newest reports (the stamp in the name sorts by time) */
    _snwprintf(pat, MAX_PATH + 32, L"%ls\\crash_*.txt", dir);
    pat[MAX_PATH + 31] = 0;
    f = FindFirstFileW(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    names = (wchar_t (*)[64])malloc((size_t)cap * sizeof *names);
    if (names) {
        do {
            if (count < cap && wcslen(fd.cFileName) < 60) wcscpy(names[count++], fd.cFileName);
        } while (FindNextFileW(f, &fd));
    }
    FindClose(f);
    if (names && count > KEEP_REPORTS) {
        int i, j;
        for (i = 1; i < count; i++) {                         /* a short list: insertion sort */
            wchar_t t[64];
            wcscpy(t, names[i]);
            for (j = i - 1; j >= 0 && wcscmp(names[j], t) > 0; j--) wcscpy(names[j + 1], names[j]);
            wcscpy(names[j + 1], t);
        }
        for (i = 0; i < count - KEEP_REPORTS; i++) {
            wchar_t p[MAX_PATH + 96];
            size_t l;
            _snwprintf(p, MAX_PATH + 96, L"%ls\\%ls", dir, names[i]);
            p[MAX_PATH + 95] = 0;
            DeleteFileW(p);
            l = wcslen(p);
            if (l > 4) { wcscpy(p + l - 4, L".dmp"); DeleteFileW(p); }
        }
    }
    free(names);
}

/* ---- the report thread ---- */
static DWORD WINAPI test_fault(void *arg);

static void re_arm_filter(void) {
    LPTOP_LEVEL_EXCEPTION_FILTER prev;
    if (!installed_filter) return;
    prev = SetUnhandledExceptionFilter(installed_filter);
    if (prev != installed_filter) {
        HMODULE m = NULL;
        char nm[MAX_PATH] = "?";
        if (prev && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       (LPCSTR)(void *)prev, &m))
            GetModuleFileNameA(m, nm, sizeof nm);
        ps2_log("crash: a library replaced the exception filter (%p, %s): put back", (void *)prev, nm);
    }
}

static DWORD WINAPI report_thread(void *arg) {
    DWORD tick = test_kind ? 50u : 30000u;
    int test_done = 0;
    (void)arg;
    for (;;) {
        DWORD w = WaitForSingleObject(ev_request, tick);
        if (w == WAIT_OBJECT_0) {
            write_report(req_why, req_ep, req_tid, 1);
            SetEvent(ev_done);                    /* before the log line: the crashed thread may hold the log's lock */
            if (last_note[0]) ps2_log("%s", last_note);
            continue;
        }
        re_arm_filter();
        if (test_kind && !test_done && ps2_gs_frame_count >= test_field) {
            HANDLE t;
            test_done = 1;
            ps2_log("crash: ACZ_CRASH_TEST=%s at field %u (test only)", test_kind, (unsigned)ps2_gs_frame_count);
            t = CreateThread(NULL, 0, test_fault, NULL, 0, NULL);
            if (t) CloseHandle(t);
            tick = 30000u;
        }
    }
    return 0;
}

void ps2_crash_report(const char *why, void *exception_pointers, unsigned long thread_id) {
    if (!crash_on || !ev_request) return;
    req_why = why;
    req_ep = (EXCEPTION_POINTERS *)exception_pointers;
    req_tid = (DWORD)thread_id;
    SetEvent(ev_request);
    if (WaitForSingleObject(ev_done, 20000) == WAIT_OBJECT_0)   /* never hang: after this the process ends anyway */
        Sleep(50);                                /* its last log line */
}

void ps2_fatal_report(const char *why) {
    if (!crash_on) return;
    write_report(why ? why : "fatal", NULL, 0, 0);
    if (last_note[0]) ps2_log("%s", last_note);
}

static void on_abort(int sig) {
    (void)sig;
    ps2_log("crash: abort() was called");
    ps2_crash_mark();
    ps2_crash_report("abort()", NULL, GetCurrentThreadId());
}

/* Test only: a function the optimiser cannot turn into a loop */
static int test_recurse(volatile char *up, int depth) {
    volatile char pad[4096];
    pad[0] = (char)depth;
    if (up) pad[1] = up[0];
    if (depth < 0) return 0;
    return test_recurse(pad, depth + 1) + pad[0];
}

static DWORD WINAPI test_fault(void *arg) {
    (void)arg;
    if (!strcmp(test_kind, "so")) {
        return (DWORD)test_recurse(NULL, 0);
    } else if (!strcmp(test_kind, "abort")) {
        abort();
    } else if (!strcmp(test_kind, "fatal")) {
        ps2_fatal_report("ACZ_CRASH_TEST=fatal (the game goes on)");
    } else {                                       /* av */
        volatile int *volatile bad = (volatile int *)(uintptr_t)0x10;
        *bad = 1;
    }
    return 0;
}

void ps2_crash_init(void *filter) {
    const char *e = getenv("ACZ_CRASH_REPORT");
    wchar_t dir[MAX_PATH];
    HMODULE dbg;
    HANDLE t;
    if (e && *e == '0') return;
    crash_on = 1;
    if (folder_exe(dir, MAX_PATH)) scan_folder(dir);
    if (folder_temp(dir, MAX_PATH)) scan_folder(dir);
    dbg = LoadLibraryExA("dbghelp.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (dbg) minidump_write = (minidump_fn)(void *)GetProcAddress(dbg, "MiniDumpWriteDump");
    installed_filter = (LPTOP_LEVEL_EXCEPTION_FILTER)filter;
    e = getenv("ACZ_CRASH_TEST");
    if (e && *e) {
        const char *f = getenv("ACZ_CRASH_TEST_FIELD");
        test_kind = e;
        test_field = f && *f ? (u32)strtoul(f, NULL, 0) : 300u;
    }
    ev_request = CreateEventA(NULL, FALSE, FALSE, NULL);
    ev_done = CreateEventA(NULL, FALSE, FALSE, NULL);
    t = ev_request && ev_done ? CreateThread(NULL, 256 * 1024, report_thread, NULL, 0, NULL) : NULL;
    if (t) CloseHandle(t);
    else ev_request = NULL;
    signal(SIGABRT, on_abort);
}

#else   /* not Windows: the runtime has no exception filter there */
int ps2_crash_enabled(void) { return 0; }
void ps2_crash_log_line(const char *line, int len) { (void)line; (void)len; }
void ps2_crash_mark(void) {}
void ps2_crash_report(const char *why, void *exception_pointers, unsigned long thread_id) { (void)why; (void)exception_pointers; (void)thread_id; }
void ps2_fatal_report(const char *why) { (void)why; }
void ps2_crash_init(void *filter) { (void)filter; }
#endif
