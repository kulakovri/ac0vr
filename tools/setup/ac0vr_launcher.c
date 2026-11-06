/* ac0vr.exe -- the player's entry point and the shortcut's target (setup_1105 package B, 2026-11-05; docs/SETTINGS.md "Paths"). Win32 only:
 * no SDL, no game code, about 100 KB, built by CMake as a normal target and shipped prebuilt.
 *
 * On start it looks at build/zero/ac5.exe beside it and build_state.json. When the game is built for the installed release and the disc is
 * there, it starts ac5.exe (which reads [paths] disc = of the settings file) and exits. Otherwise the dialog: the disc (a picker for an ISO or a
 * folder; tools/setup/disc_check.c's check in-process: the serial and the executable's hash, the US disc only), then "Build the game from your
 * disc" with a bar, the stage's name and the time left, running the bundled Python on tools/setup/player_build.py through CreateProcessW and a
 * pipe. On success the game starts; on failure the window shows the last 30 lines and build/player_build.log stays. Cancel works: the build's
 * processes sit in a job, and what was compiled stays for the next run.
 *
 *   ac0vr.exe                   as above
 *   ac0vr.exe --check-disc PATH [--out FILE]   the check only, the words into FILE (UTF-8); exit 0 ok, 1 wrong, 2 nothing readable. The installer's disc page.
 *   ac0vr.exe --set-disc PATH   the check, then [paths] disc = of build/zero/ac5_settings.ini
 *   ac0vr.exe --dialog          the window even when nothing is needed: another disc, a rebuild, updates
 *   ac0vr.exe --version         a message box with version.txt
 *   [--auto] [--no-start] [--hidden] [-- ARGS FOR THE GAME]   [test] no clicks; no game at the end; no window
 * ACZ_SETUP_SHOT=PREFIX [test]: the window's own image (PrintWindow, never the screen) into PREFIX_<n>.bmp at each change of the stage.
 *
 * Everything lies under the folder of this exe; all file names are wide. Built by CMake (target ac0vr). The updater (tools/setup/ac0vr_update.c,
 * package D) is compiled into this file when it is in the tree: its code reaches this window's parts directly. */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define COBJMACROS
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define DISC_CHECK_NO_MAIN
#include "disc_check.c"

#if __has_include("ac0vr_update.c")
#define HAVE_UPDATER 1
#endif

enum { ID_PATH = 100, ID_ISO, ID_FOLDER, ID_MSG, ID_BUILD, ID_STAGE, ID_BAR, ID_LEFT, ID_LOG, ID_CANCEL, ID_UPDATE, ID_PREV };
enum { WM_BUILD_LINE = WM_APP + 1, WM_BUILD_END, WM_UPDATE_TEXT, WM_UPDATE_END };
enum { LOG_KEEP = 30 };

static wchar_t root[MAX_PATH];                 /* this exe's folder, no trailing backslash */
static HWND win, h_path, h_iso, h_folder, h_msg, h_build, h_stage, h_bar, h_left, h_log, h_cancel, h_update, h_prev;
static HFONT font;
static int opt_auto, opt_no_start, opt_hidden, opt_dialog;
static wchar_t game_args[1024];
static wchar_t disc[MAX_PATH * 2];
static int disc_ok, need_build, building, built_ok, cancelled;
static HANDLE job, build_thread;
static const wchar_t *shot_prefix;
static int shot_n;
static char release_line[96];

/* ---- paths and small files ---- */
static void join(wchar_t *out, size_t cap, const wchar_t *rel) { _snwprintf(out, cap, L"%ls\\%ls", root, rel); out[cap - 1] = 0; }

static int exists(const wchar_t *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }

static void mkdirs(const wchar_t *path) {                 /* every folder of path, under root */
    wchar_t b[MAX_PATH * 2], *p;
    wcsncpy(b, path, MAX_PATH * 2 - 1);
    b[MAX_PATH * 2 - 1] = 0;
    for (p = b + wcslen(root) + 1; (p = wcschr(p, L'\\')) != NULL; p++) {
        *p = 0;
        CreateDirectoryW(b, NULL);
        *p = L'\\';
    }
    CreateDirectoryW(b, NULL);
}

static char *read_all(const wchar_t *p, size_t *n) {
    FILE *f = _wfopen(p, L"rb");
    char *b;
    long l;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    l = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (char *)malloc((size_t)l + 1);
    if (b) {
        size_t got = fread(b, 1, (size_t)l, f);
        b[got] = 0;
        if (n) *n = got;
    }
    fclose(f);
    return b;
}

static void read_release(void) {
    wchar_t p[MAX_PATH * 2];
    char *b;
    release_line[0] = 0;
    join(p, MAX_PATH * 2, L"version.txt");
    if ((b = read_all(p, NULL)) != NULL) {
        snprintf(release_line, sizeof release_line, "%s", b);
        release_line[strcspn(release_line, "\r\n")] = 0;
        free(b);
    }
}

/* ---- [paths] disc = of the settings file (the same rules as runtime/src/ps2_profile.c: a value runs to the line's end) ---- */
static void ini_path(wchar_t *out, size_t cap) { join(out, cap, L"build\\zero\\ac5_settings.ini"); }

static int ini_read_disc(wchar_t *out, size_t cap) {
    wchar_t p[MAX_PATH * 2];
    char *b, *s, *end;
    int in_paths = 0, found = 0;
    ini_path(p, MAX_PATH * 2);
    if (!(b = read_all(p, NULL))) return 0;
    for (s = b; *s && !found; s = end) {
        char *e;
        end = strchr(s, '\n');
        end = end ? end + 1 : s + strlen(s);
        if (!memcmp(s, "\xEF\xBB\xBF", 3)) s += 3;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '[') in_paths = !strncmp(s, "[paths]", 7);
        else if (in_paths && !strncmp(s, "disc", 4) && (s[4] == ' ' || s[4] == '\t' || s[4] == '=')) {
            char val[MAX_PATH * 3];
            size_t l;
            s += 4;
            while (*s == ' ' || *s == '\t') s++;
            if (*s != '=') continue;
            s++;
            while (*s == ' ' || *s == '\t') s++;
            e = end;
            while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) e--;
            l = (size_t)(e - s);
            if (l >= sizeof val) l = sizeof val - 1;
            memcpy(val, s, l);
            val[l] = 0;
            found = MultiByteToWideChar(CP_UTF8, 0, val, -1, out, (int)cap) > 0;
        }
    }
    free(b);
    return found;
}

static int ini_write_disc(const wchar_t *value) {
    wchar_t p[MAX_PATH * 2], tmp[MAX_PATH * 2];
    char val[MAX_PATH * 3];
    char *b;
    FILE *f;
    int in_paths = 0, done = 0, ok;
    if (WideCharToMultiByte(CP_UTF8, 0, value, -1, val, sizeof val, NULL, NULL) <= 0) return 0;
    if (strlen(val) == 2 && val[1] == ':') strcat(val, "\\");       /* the installer's "D:": the drive's root, not its current folder */
    ini_path(p, MAX_PATH * 2);
    _snwprintf(tmp, MAX_PATH * 2, L"%ls.tmp", p);
    join(tmp, MAX_PATH * 2, L"build\\zero");
    mkdirs(tmp);
    _snwprintf(tmp, MAX_PATH * 2, L"%ls.tmp", p);
    b = read_all(p, NULL);
    if (!(f = _wfopen(tmp, L"wb"))) { free(b); return 0; }
    if (b) {
        char *s, *end;
        for (s = b; *s; s = end) {
            char *t = s;
            end = strchr(s, '\n');
            end = end ? end + 1 : s + strlen(s);
            while (*t == ' ' || *t == '\t') t++;
            if (*t == '[') {
                if (in_paths && !done) { fprintf(f, "disc = %s\n", val); done = 1; }
                in_paths = !strncmp(t, "[paths]", 7);
            } else if (in_paths && !strncmp(t, "disc", 4) && (t[4] == ' ' || t[4] == '\t' || t[4] == '=')) {
                if (!done) fprintf(f, "disc = %s\n", val);
                done = 1;
                continue;
            }
            fwrite(s, 1, (size_t)(end - s), f);
        }
        free(b);
    }
    if (!done) fprintf(f, "%s[paths]\ndisc = %s\n", in_paths ? "" : "\n", val);
    ok = fclose(f) == 0;
    return ok && MoveFileExW(tmp, p, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

/* ---- is the game built for this release? ---- */
static int build_current(void) {
    wchar_t p[MAX_PATH * 2];
    char *b, want[128];
    int ok;
    join(p, MAX_PATH * 2, L"build\\zero\\ac5.exe");
    if (!exists(p)) return 0;
    join(p, MAX_PATH * 2, L"build_state.json");
    if (!(b = read_all(p, NULL))) return 0;
    snprintf(want, sizeof want, "\"release\": \"%s\"", release_line);
    ok = strstr(b, want) != NULL;
    free(b);
    return ok;
}

/* ---- the game ---- */
static int start_game(void) {
    wchar_t exe[MAX_PATH * 2], dir[MAX_PATH * 2], cmd[MAX_PATH * 2 + 1100];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    join(exe, MAX_PATH * 2, L"build\\zero\\ac5.exe");
    join(dir, MAX_PATH * 2, L"build\\zero");
    _snwprintf(cmd, MAX_PATH * 2 + 1100, L"\"%ls\"%ls%ls", exe, game_args[0] ? L" " : L"", game_args);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, dir, &si, &pi)) return 0;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
}

/* ---- the window's parts ---- */
static void set_text_u8(HWND h, const char *u8) {
    wchar_t w[4096];
    if (MultiByteToWideChar(CP_UTF8, 0, u8, -1, w, 4096) > 0) SetWindowTextW(h, w);
}

/* [test] the window's own image: PrintWindow, never the screen */
static void shot(void) {
    wchar_t p[MAX_PATH * 2];
    RECT r;
    HDC dc, mem;
    HBITMAP bm;
    BITMAPINFO bi;
    void *bits = NULL;
    FILE *f;
    if (!shot_prefix || !win || !GetWindowRect(win, &r)) return;
    RedrawWindow(win, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    Sleep(60);                                 /* (the compositor's copy, which PW_RENDERFULLCONTENT reads, follows a moment later) */
    _snwprintf(p, MAX_PATH * 2, L"%ls_%02d.bmp", shot_prefix, shot_n++);
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = r.right - r.left;
    bi.bmiHeader.biHeight = r.bottom - r.top;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    dc = GetDC(win);
    mem = CreateCompatibleDC(dc);
    bm = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    SelectObject(mem, bm);
    PrintWindow(win, mem, 2 /* PW_RENDERFULLCONTENT */);
    GdiFlush();
    if ((f = _wfopen(p, L"wb")) != NULL) {
        BITMAPFILEHEADER fh;
        DWORD size = (DWORD)(bi.bmiHeader.biWidth * bi.bmiHeader.biHeight * 4);
        memset(&fh, 0, sizeof fh);
        fh.bfType = 0x4D42;
        fh.bfOffBits = sizeof fh + sizeof bi.bmiHeader;
        fh.bfSize = fh.bfOffBits + size;
        fwrite(&fh, sizeof fh, 1, f);
        fwrite(&bi.bmiHeader, sizeof bi.bmiHeader, 1, f);
        fwrite(bits, size, 1, f);
        fclose(f);
    }
    DeleteObject(bm);
    DeleteDC(mem);
    ReleaseDC(win, dc);
}

static void refresh_buttons(void) {
    SetWindowTextW(h_build, need_build ? L"Build the game from your disc" : L"Start the game");
    EnableWindow(h_build, disc_ok && !building);
    EnableWindow(h_iso, !building);
    EnableWindow(h_folder, !building);
    if (h_update) EnableWindow(h_update, !building);
    SetWindowTextW(h_cancel, building ? L"Cancel" : L"Close");
}

/* the disc's check: the words under the path. A wrong disc is refused with the message, the US disc only. */
static void check_disc_now(void) {
    static char msg[4096];
    char *last;
    int rc;
    GetWindowTextW(h_path, disc, MAX_PATH * 2);
    if (!disc[0]) { disc_ok = 0; SetWindowTextW(h_msg, L""); refresh_buttons(); return; }
    rc = disc_check_run(disc, NULL, msg, sizeof msg);
    disc_ok = rc == DC_OK;
    last = strstr(msg, "\nresult:");                   /* the last line is for scripts */
    if (last) *last = 0;
    set_text_u8(h_msg, msg);
    if (disc_ok) ini_write_disc(disc);
    refresh_buttons();
}

static void pick_iso(void) {
    wchar_t name[MAX_PATH * 2] = L"";
    OPENFILENAMEW o;
    memset(&o, 0, sizeof o);
    o.lStructSize = sizeof o;
    o.hwndOwner = win;
    o.lpstrFilter = L"Disc image (*.iso)\0*.iso\0All files\0*.*\0";
    o.lpstrFile = name;
    o.nMaxFile = MAX_PATH * 2;
    o.lpstrTitle = L"Your disc image";
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
    if (GetOpenFileNameW(&o)) { SetWindowTextW(h_path, name); check_disc_now(); }
}

static void pick_folder(void) {
    IFileOpenDialog *d = NULL;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&d))) return;
    IFileOpenDialog_SetOptions(d, FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    IFileOpenDialog_SetTitle(d, L"The folder with your disc's files, or the disc's drive");
    if (SUCCEEDED(IFileOpenDialog_Show(d, win))) {
        IShellItem *it = NULL;
        if (SUCCEEDED(IFileOpenDialog_GetResult(d, &it))) {
            wchar_t *name = NULL;
            if (SUCCEEDED(IShellItem_GetDisplayName(it, SIGDN_FILESYSPATH, &name))) {
                SetWindowTextW(h_path, name);
                CoTaskMemFree(name);
                check_disc_now();
            }
            IShellItem_Release(it);
        }
    }
    IFileOpenDialog_Release(d);
}

/* ---- the build, on a thread of its own: the pipe's lines go to the window ---- */
static HANDLE log_file;

static void post_line(const char *line) { PostMessageW(win, WM_BUILD_LINE, 0, (LPARAM)_strdup(line)); }

static DWORD WINAPI build_main(void *arg) {
    wchar_t py[MAX_PATH * 2], cmd[MAX_PATH * 6], elf[MAX_PATH * 2], lg[MAX_PATH * 2], dir[MAX_PATH * 2], rel[128];
    static char msg[4096];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE rd = NULL, wr = NULL;
    DWORD code = 1, n;
    char buf[4096], line[4096];
    size_t ll = 0;
    SYSTEM_INFO sys;
    (void)arg;
    join(dir, MAX_PATH * 2, L"build\\elf");
    mkdirs(dir);
    join(lg, MAX_PATH * 2, L"build\\player_build.log");
    log_file = CreateFileW(lg, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    join(elf, MAX_PATH * 2, L"build\\elf\\SLUS_213.46");
    /* the disc again, and the executable out of it: the recompiler's input; it stays on this PC and goes away after the build */
    if (disc_check_run(disc, elf, msg, sizeof msg) != DC_OK) {
        post_line(msg);
        PostMessageW(win, WM_BUILD_END, 1, 0);
        return 1;
    }
    join(py, MAX_PATH * 2, L"python\\python.exe");
    GetNativeSystemInfo(&sys);
    MultiByteToWideChar(CP_UTF8, 0, release_line, -1, rel, 128);
    _snwprintf(cmd, MAX_PATH * 6, L"\"%ls\" -I -B \"%ls\\tools\\setup\\player_build.py\" --bundle \"%ls\\bundle\" --elf \"%ls\" --out \"%ls\" --jobs %lu --release \"%ls\"",
               py, root, root, elf, root, (unsigned long)sys.dwNumberOfProcessors, rel);
    if (!CreatePipe(&rd, &wr, &sa, 0)) { post_line("no pipe"); PostMessageW(win, WM_BUILD_END, 1, 0); return 1; }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    job = CreateJobObjectW(NULL, NULL);
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
        memset(&li, 0, sizeof li);
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    {   /* a build must not take the PC away; a lower class that is already there stays (the developer's idle test runs) */
        DWORD cls = GetPriorityClass(GetCurrentProcess());
        DWORD prio = cls == IDLE_PRIORITY_CLASS || cls == BELOW_NORMAL_PRIORITY_CLASS ? 0 : BELOW_NORMAL_PRIORITY_CLASS;
        if (!CreateProcessW(py, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED | prio, NULL, root, &si, &pi)) {
            snprintf(msg, sizeof msg, "The bundled Python did not start (error %lu). Is the install complete?", (unsigned long)GetLastError());
            post_line(msg);
            PostMessageW(win, WM_BUILD_END, 1, 0);
            CloseHandle(wr);
            CloseHandle(rd);
            return 1;
        }
    }
    AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(wr);
    while (ReadFile(rd, buf, sizeof buf, &n, NULL) && n) {
        DWORD w;
        if (log_file != INVALID_HANDLE_VALUE) WriteFile(log_file, buf, n, &w, NULL);
        for (DWORD i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n' || ll + 1 >= sizeof line) {
                line[ll] = 0;
                if (ll && line[ll - 1] == '\r') line[ll - 1] = 0;
                post_line(line);
                ll = 0;
            } else line[ll++] = c;
        }
    }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    if (log_file != INVALID_HANDLE_VALUE) CloseHandle(log_file);
    if (code == 0) DeleteFileW(elf);
    PostMessageW(win, WM_BUILD_END, code, 0);
    return 0;
}

/* the bar: the stages' measured times from the bundle (bundle.txt, stage_*=seconds), scaled to what this PC does */
static const struct { const char *key, *label; double fallback; } stage_tab[] = {
    { "recompile", "Translating the game's code to C", 20 },
    { "compile", "Compiling it for your PC", 100 },
    { "link", "Linking the game", 3 },
    { "install", "Putting the files in place", 1 },
};
enum { N_STAGE = 4 };
static double stage_w[N_STAGE];
static int stage_in_plan[N_STAGE], cur_stage = -1;
static double cur_frac;
static double stage_done_w, plan_w;
static DWORD build_t0;
static char tail_lines[LOG_KEEP][300];
static int tail_n;

static void read_stage_weights(void) {
    wchar_t p[MAX_PATH * 2];
    char *b;
    join(p, MAX_PATH * 2, L"bundle\\bundle.txt");
    b = read_all(p, NULL);
    for (int i = 0; i < N_STAGE; i++) {
        char key[40], *s;
        stage_w[i] = stage_tab[i].fallback;
        snprintf(key, sizeof key, "stage_%s=", stage_tab[i].key);
        if (b && (s = strstr(b, key)) != NULL) stage_w[i] = atof(s + strlen(key));
    }
    free(b);
}

static void show_progress(void) {
    double f = plan_w > 0 ? (stage_done_w + (cur_stage >= 0 ? stage_w[cur_stage] * cur_frac : 0)) / plan_w : 0;
    double el = (double)(GetTickCount() - build_t0) / 1000.0;
    double left = f > 0.05 ? el * (1.0 - f) / f : plan_w * (1.0 - f);
    wchar_t t[80];
    SendMessageW(h_bar, PBM_SETPOS, (WPARAM)(f * 1000.0), 0);
    if (left >= 60) _snwprintf(t, 80, L"About %d min %02d s left", (int)left / 60, ((int)left % 60) / 5 * 5);
    else _snwprintf(t, 80, L"About %d s left", ((int)left + 4) / 5 * 5);
    SetWindowTextW(h_left, t);
}

static void on_line(char *line) {
    if (!strncmp(line, "@plan ", 6)) {
        plan_w = stage_w[3];
        stage_in_plan[3] = 1;
        for (int i = 0; i < 3; i++) {
            stage_in_plan[i] = strstr(line, stage_tab[i].key) != NULL;
            if (stage_in_plan[i]) plan_w += stage_w[i];
        }
    } else if (!strncmp(line, "@stage ", 7)) {
        for (int i = 0; i < N_STAGE; i++)
            if (!strcmp(line + 7, stage_tab[i].key)) {
                cur_stage = i;
                cur_frac = 0;
                set_text_u8(h_stage, stage_tab[i].label);
                show_progress();
                shot();
            }
    } else if (!strncmp(line, "@progress ", 10)) {
        cur_frac = atof(line + 10);
        show_progress();
    } else if (!strncmp(line, "@done ", 6)) {
        if (cur_stage >= 0) stage_done_w += stage_w[cur_stage];
        cur_stage = -1;
        cur_frac = 0;
        show_progress();
    } else if (line[0] != '@') {
        snprintf(tail_lines[tail_n % LOG_KEEP], sizeof tail_lines[0], "%s", line);
        tail_n++;
    }
}

static void show_log_tail(void) {
    static char all[LOG_KEEP * 310];
    size_t l = 0;
    int from = tail_n > LOG_KEEP ? tail_n - LOG_KEEP : 0;
    for (int i = from; i < tail_n; i++)
        l += (size_t)snprintf(all + l, sizeof all - l, "%s\r\n", tail_lines[i % LOG_KEEP]);
    set_text_u8(h_log, all);
    ShowWindow(h_log, SW_SHOW);
    SendMessageW(h_log, EM_LINESCROLL, 0, LOG_KEEP);
}

static void build_start(void) {
    if (building) return;
    building = 1;
    built_ok = 0;
    cancelled = 0;
    tail_n = 0;
    stage_done_w = 0;
    plan_w = 0;
    cur_stage = -1;
    build_t0 = GetTickCount();
    read_stage_weights();
    ShowWindow(h_log, SW_HIDE);
    ShowWindow(h_prev, SW_HIDE);
    ShowWindow(h_bar, SW_SHOW);
    ShowWindow(h_left, SW_SHOW);
    SendMessageW(h_bar, PBM_SETPOS, 0, 0);
    SetWindowTextW(h_stage, L"Starting the build");
    refresh_buttons();
    build_thread = CreateThread(NULL, 0, build_main, NULL, 0, NULL);
}

static void on_build_end(DWORD code) {
    building = 0;
    if (build_thread) { CloseHandle(build_thread); build_thread = NULL; }
    if (job) { CloseHandle(job); job = NULL; }
    if (code == 0 && !cancelled) {
        built_ok = 1;
        need_build = 0;
        SendMessageW(h_bar, PBM_SETPOS, 1000, 0);
        SetWindowTextW(h_stage, L"The game is built.");
        SetWindowTextW(h_left, L"");
        shot();
        if (!opt_no_start) start_game();
        DestroyWindow(win);
        return;
    }
    if (cancelled) {
        SetWindowTextW(h_stage, L"Cancelled. What was compiled stays for the next try.");
        SetWindowTextW(h_left, L"");
    } else {
        wchar_t t[160];
        _snwprintf(t, 160, L"The build failed. The whole log is in build\\player_build.log.");
        SetWindowTextW(h_stage, t);
        SetWindowTextW(h_left, L"");
        show_log_tail();
        {
            wchar_t e[MAX_PATH * 2];
            join(e, MAX_PATH * 2, L"build\\zero\\ac5.exe");
            if (exists(e)) ShowWindow(h_prev, SW_SHOW);        /* the previous build still runs */
        }
    }
    refresh_buttons();
    shot();
    if (opt_auto) DestroyWindow(win);
}

#ifdef HAVE_UPDATER
#include "ac0vr_update.c"
#endif

/* ---- the window ---- */
static HWND add(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id, int dpi) {
    HWND c = CreateWindowExW(!wcscmp(cls, L"EDIT") ? WS_EX_CLIENTEDGE : 0, cls, text, WS_CHILD | WS_VISIBLE | style,
                             MulDiv(x, dpi, 96), MulDiv(y, dpi, 96), MulDiv(w, dpi, 96), MulDiv(h, dpi, 96), win, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    return c;
}

static void make_controls(void) {
    int dpi = GetDpiForWindow(win);
    NONCLIENTMETRICSW ncm;
    wchar_t top[300];
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    ncm.lfMessageFont.lfHeight = -MulDiv(10, dpi, 72);
    font = CreateFontIndirectW(&ncm.lfMessageFont);
    _snwprintf(top, 300, L"This program holds no game. It builds the game from your own US disc of Ace Combat Zero: The Belkan War (SLUS-21346), "
                         L"on this PC, once. The disc can be an ISO image or a folder with its files; it is read, never copied.");
    add(L"STATIC", top, 0, 14, 12, 612, 54, 0, dpi);
    h_path = add(L"EDIT", disc, ES_AUTOHSCROLL | ES_READONLY, 14, 70, 396, 24, ID_PATH, dpi);
    h_iso = add(L"BUTTON", L"ISO image...", BS_PUSHBUTTON, 416, 69, 100, 26, ID_ISO, dpi);
    h_folder = add(L"BUTTON", L"Folder...", BS_PUSHBUTTON, 522, 69, 104, 26, ID_FOLDER, dpi);
    h_msg = add(L"STATIC", L"", 0, 14, 100, 612, 56, ID_MSG, dpi);
    h_build = add(L"BUTTON", L"Build the game from your disc", BS_DEFPUSHBUTTON, 14, 160, 260, 32, ID_BUILD, dpi);
#ifdef HAVE_UPDATER
    h_update = add(L"BUTTON", L"Check for updates", BS_PUSHBUTTON, 282, 160, 150, 32, ID_UPDATE, dpi);
#endif
    h_stage = add(L"STATIC", L"", 0, 14, 202, 612, 22, ID_STAGE, dpi);
    h_bar = add(PROGRESS_CLASSW, L"", PBS_SMOOTH, 14, 226, 612, 20, ID_BAR, dpi);
    SendMessageW(h_bar, PBM_SETRANGE32, 0, 1000);
    h_left = add(L"STATIC", L"", 0, 14, 250, 612, 22, ID_LEFT, dpi);
    h_log = add(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL | WS_VSCROLL | WS_HSCROLL, 14, 276, 612, 150, ID_LOG, dpi);
    h_prev = add(L"BUTTON", L"Start the previous build", BS_PUSHBUTTON, 14, 436, 200, 30, ID_PREV, dpi);
    h_cancel = add(L"BUTTON", L"Close", BS_PUSHBUTTON, 526, 436, 100, 30, ID_CANCEL, dpi);
    ShowWindow(h_log, SW_HIDE);
    ShowWindow(h_prev, SW_HIDE);
    ShowWindow(h_bar, SW_HIDE);
    ShowWindow(h_left, SW_HIDE);
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case ID_ISO: pick_iso(); return 0;
        case ID_FOLDER: pick_folder(); return 0;
        case ID_BUILD:
            if (need_build) build_start();
            else {
                if (!opt_no_start) start_game();
                DestroyWindow(win);
            }
            return 0;
        case ID_PREV:
            if (!opt_no_start) start_game();
            DestroyWindow(win);
            return 0;
#ifdef HAVE_UPDATER
        case ID_UPDATE: ac0vr_update_button(); return 0;
#endif
        case ID_CANCEL:
        case IDCANCEL:
            if (building) {
                cancelled = 1;
                if (job) TerminateJobObject(job, 1);
                return 0;
            }
            DestroyWindow(win);
            return 0;
        }
        break;
    case WM_CLOSE:
        if (building) {
            if (MessageBoxW(h, L"Stop the build? What was compiled stays for the next try.", L"ac0vr", MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
            cancelled = 1;
            if (job) TerminateJobObject(job, 1);
            return 0;
        }
        DestroyWindow(h);
        return 0;
    case WM_BUILD_LINE:
        on_line((char *)l);
        free((char *)l);
        return 0;
    case WM_BUILD_END:
        on_build_end((DWORD)w);
        return 0;
#ifdef HAVE_UPDATER
    case WM_UPDATE_TEXT:
        ac0vr_update_text((char *)l, (int)w);
        free((char *)l);
        return 0;
    case WM_UPDATE_END:
        ac0vr_update_ended((DWORD)w);
        return 0;
#endif
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static int run_dialog(int cmd_show) {
    WNDCLASSW wc;
    MSG msg;
    wchar_t title[140];
    RECT r = { 0, 0, 640, 476 };
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
    int dpi;
    InitCommonControlsEx(&icc);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"ac0vr_setup";
    RegisterClassW(&wc);
    {
        wchar_t rel[100];
        MultiByteToWideChar(CP_UTF8, 0, release_line, -1, rel, 100);
        _snwprintf(title, 140, L"ac0vr %ls", rel);
    }
    win = CreateWindowExW(0, wc.lpszClassName, title, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT, 100, 100,
                          NULL, NULL, wc.hInstance, NULL);
    dpi = GetDpiForWindow(win);
    r.right = MulDiv(r.right, dpi, 96);
    r.bottom = MulDiv(r.bottom, dpi, 96);
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, 0, dpi);
    SetWindowPos(win, NULL, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
    make_controls();
    if (disc[0]) check_disc_now();
    else refresh_buttons();
    ShowWindow(win, opt_hidden ? SW_HIDE : cmd_show);
    UpdateWindow(win);
#ifdef HAVE_UPDATER
    ac0vr_update_dialog_shown();
#endif
    shot();
    if (opt_auto && disc_ok) PostMessageW(win, WM_COMMAND, ID_BUILD, 0);
    else if (opt_auto) { shot(); DestroyWindow(win); }
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(win, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return built_ok || !(opt_auto
#ifdef HAVE_UPDATER
                         || opt_auto_update
#endif
                         ) ? 0 : 1;
}

/* the first disc that passes: an .iso beside this exe, then the disc drives */
static void find_disc_default(void) {
    wchar_t pat[MAX_PATH * 2];
    WIN32_FIND_DATAW fd;
    HANDLE f;
    static char msg[4096];
    join(pat, MAX_PATH * 2, L"*.iso");
    f = FindFirstFileW(pat, &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            wchar_t p[MAX_PATH * 2];
            join(p, MAX_PATH * 2, fd.cFileName);
            if (disc_check_run(p, NULL, msg, sizeof msg) == DC_OK) {
                wcsncpy(disc, p, MAX_PATH * 2 - 1);
                FindClose(f);
                return;
            }
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    if (disc_check_run(NULL, NULL, msg, sizeof msg) == DC_OK) {
        DWORD mask = GetLogicalDrives();
        for (int i = 0; i < 26; i++) {
            wchar_t d[4] = { (wchar_t)(L'A' + i), L':', L'\\', 0 };
            if ((mask & (1u << i)) && GetDriveTypeW(d) == DRIVE_CDROM && disc_check_run(d, NULL, msg, sizeof msg) == DC_OK) {
                wcscpy(disc, d);
                return;
            }
        }
    }
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR line, int cmd_show) {
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const wchar_t *check = NULL, *set = NULL, *out = NULL, *given = NULL;
    int want_version = 0;
    (void)inst; (void)prev; (void)line;
    {
        DWORD n = GetModuleFileNameW(NULL, root, MAX_PATH);
        wchar_t *sl = n && n < MAX_PATH ? wcsrchr(root, L'\\') : NULL;
        if (!sl) return 3;
        *sl = 0;
    }
    SetErrorMode(SEM_FAILCRITICALERRORS);
    shot_prefix = _wgetenv(L"ACZ_SETUP_SHOT");
    for (int i = 1; i < argc; i++) {
        const wchar_t *a = argv[i];
        if (!wcscmp(a, L"--check-disc") && i + 1 < argc) check = argv[++i];
        else if (!wcscmp(a, L"--set-disc") && i + 1 < argc) set = argv[++i];
        else if (!wcscmp(a, L"--out") && i + 1 < argc) out = argv[++i];
        else if (!wcscmp(a, L"--disc") && i + 1 < argc) given = argv[++i];
        else if (!wcscmp(a, L"--version")) want_version = 1;
        else if (!wcscmp(a, L"--auto")) opt_auto = 1;
        else if (!wcscmp(a, L"--no-start")) opt_no_start = 1;
        else if (!wcscmp(a, L"--hidden")) opt_hidden = 1;
        else if (!wcscmp(a, L"--dialog")) opt_dialog = 1;
#ifdef HAVE_UPDATER
        else if (!wcscmp(a, L"--check-updates")) { opt_dialog = 1; opt_check_updates = 1; }
        else if (!wcscmp(a, L"--auto-update")) opt_auto_update = 1;
#endif
        else if (!wcscmp(a, L"--")) {
            for (i++; i < argc; i++) {
                wcsncat(game_args, argv[i], 1000 - wcslen(game_args));
                if (i + 1 < argc) wcscat(game_args, L" ");
            }
        }
    }
    read_release();
    if (want_version) {
        wchar_t w[120];
        MultiByteToWideChar(CP_UTF8, 0, release_line, -1, w, 120);
        MessageBoxW(NULL, w, L"ac0vr", MB_OK);
        return 0;
    }
    if (check || set) {
        static char msg[4096];
        int rc = disc_check_run(check ? check : set, NULL, msg, sizeof msg);
        if (out) {
            FILE *f = _wfopen(out, L"wb");
            if (f) { fputs(msg, f); fclose(f); }
        }
        if (set && rc == DC_OK && !ini_write_disc(set)) rc = 2;
        return rc;
    }
    {   /* a self-replacement of the last update (package D) is finished now */
        wchar_t old[MAX_PATH * 2];
        join(old, MAX_PATH * 2, L"ac0vr.exe.old");
        DeleteFileW(old);
    }
    need_build = !build_current();
    if (given) wcsncpy(disc, given, MAX_PATH * 2 - 1);
    else if (!ini_read_disc(disc, MAX_PATH * 2) || !exists(disc)) {
        disc[0] = 0;
        find_disc_default();
    }
#ifdef HAVE_UPDATER
    if (!need_build && !opt_auto) ac0vr_update_at_start();
#endif
    if (!need_build && !opt_dialog && !given && disc[0] && exists(disc)
#ifdef HAVE_UPDATER
        && !ac0vr_update_pending()
#endif
    ) {
        if (opt_no_start) return 0;
        return start_game() ? 0 : 3;
    }
    return run_dialog(cmd_show);
}
