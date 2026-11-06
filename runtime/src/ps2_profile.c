/* ps2_profile.c -- the exe started with nothing (setup_1105 package A, 2026-11-05; docs/SETTINGS.md "Paths"): what
   `play_zero_vr.sh sharp native cockpit smooth modern` does today, without a shell, in any folder.

   Who started me. `--data` on the command line means a script: every owner script and run_test.sh pass it, and such a start is left
   exactly as before (no table, no change of folder, no log of its own, only the log's new first line). So does any option of a run
   that a script ends or tests by itself. ACZ_PROFILE=script or =player overrides this. Everything else is a player's start.

   A player's start, before everything else (a constructor of priority 101: before the settings constructor, which reads
   ACZ_MENU_SETTINGS): a console nobody else shares goes away (a double click from Explorer), the current folder becomes the home
   folder (the exe's; or the tree's root when the exe sits in a developer's build/<name>/, with gen_zero two folders up), the log goes
   to logs/zero_play_<stamp>.log unless stdout is already a file or a pipe (the 8 newest are kept), and the table below is applied:
   each entry only when the variable is not set, as the script's ${VAR:-...}.

   Three things the script's words set are not in the table, because a saved row owns them and an exported switch would lock that row
   for the player (the launcher's "held", the page's lock): SMOOTH MOTION (PS2_XR_RERENDER, default 1), CONTROLLER LAYOUT
   (ACZ_PAD_LAYOUT, default modern with ACZ_MENU_SETTINGS) and the launcher (ACZ_LAUNCHER, the file's `launcher`, default 1, whose
   checkbox would be dead). On a new settings file they come out the same as the words. */
#include "ps2_profile.h"
#include "ps2_runtime.h"
#include "ps2_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#include <shellapi.h>

enum { M_SHARP = 1, M_STEREO = 2, M_MONO = 4, M_CINEMA = 8, M_FLAT = 16, M_XR = 15, M_ALL = 31 };

static const struct { const char *var, *val; int modes; } player_table[] = {
    { "ACZ_MENU_SETTINGS", "1", M_ALL },                /* the ENHANCEMENTS rows apply */
    { "SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1", M_ALL },   /* the pad keeps working when the headset takes the window's focus */
    { "PS2_RN_VP", "1", M_ALL },                        /* the word `native` */
    { "PS2_XR", "1", M_XR },
    { "PS2_XR_MODE", "stereo", M_SHARP | M_STEREO },
    { "PS2_XR_MODE", "mono", M_MONO },
    { "PS2_XR_MODE", "cinema", M_CINEMA },
    { "ACZ_VR_HEADLOOK", "stick", M_SHARP },            /* `sharp` is `stick` in one pass */
    { "PS2_XR_MULTIVIEW", "1", M_SHARP },
    { "PS2_FIELD_HZ", "60", M_XR },
    { "PS2_UPSCALE", "3", M_ALL & ~M_SHARP },           /* (sharp: the row RESOLUTION) */
};

static int profile_player_on, disc_given, table_mode = M_SHARP;
static char table_log[2000];

int ps2_profile_player(void) { return profile_player_on; }
const char *ps2_profile_data_dir(void) { return profile_player_on ? "gen_zero" : "."; }

static void log_add(const char *var, const char *val) {
    size_t l = strlen(table_log);
    snprintf(table_log + l, sizeof table_log - l, "%s%s=%s", l ? " " : "", var, val);
}

static void set_unset(const char *var, const char *val) {
    if (getenv(var)) return;
    _putenv_s(var, val);
    log_add(var, val);
}

static int have_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

/* ---- the settings file's [paths] and [update]: the game does not own them (the setup program writes them too), a save keeps them.
   A value runs to the line's end: a folder name may hold ; and #. ---- */
static int ini_get(const char *section, const char *key, char *out, size_t cap) {
    char line[2048], cur[64] = "";
    FILE *f = fopen(ps2_settings_path(), "r");
    int found = 0;
    size_t kl = strlen(key);
    if (!f) return 0;
    while (!found && fgets(line, sizeof line, f)) {
        char *p = line, *e;
        if (!memcmp(p, "\xEF\xBB\xBF", 3)) p += 3;
        while (*p == ' ' || *p == '\t') p++;
        e = p + strlen(p);
        while (e > p && (e[-1] == '\r' || e[-1] == '\n' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
        if (*p == '[') {
            char *b = strchr(p, ']');
            if (b) { *b = 0; snprintf(cur, sizeof cur, "%s", p + 1); }
        } else if (!strcmp(cur, section) && !strncmp(p, key, kl) && (p[kl] == ' ' || p[kl] == '\t' || p[kl] == '=')) {
            p += kl;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '=') {
                p++;
                while (*p == ' ' || *p == '\t') p++;
                snprintf(out, cap, "%s", p);
                found = 1;
            }
        }
    }
    fclose(f);
    return found && *out;
}

static int ini_set_disc(const char *value) {
    const char *path = ps2_settings_path();
    char tmp[1200], line[2048];
    FILE *in, *out;
    int in_paths = 0, done = 0;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (!(out = fopen(tmp, "w"))) return 0;
    in = fopen(path, "r");
    while (in && fgets(line, sizeof line, in)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '[') {
            if (in_paths && !done) { fprintf(out, "disc = %s\n", value); done = 1; }
            in_paths = !strncmp(p, "[paths]", 7);
        } else if (in_paths && !strncmp(p, "disc", 4) && (p[4] == ' ' || p[4] == '\t' || p[4] == '=')) {
            if (!done) fprintf(out, "disc = %s\n", value);
            done = 1;
            continue;
        }
        fputs(line, out);
    }
    if (in) fclose(in);
    if (!done) fprintf(out, "%s[paths]\ndisc = %s\n", in_paths ? "" : "\n", value);
    if (fclose(out) != 0) return 0;
    remove(path);
    return rename(tmp, path) == 0;
}

/* ---- the disc ---- */
static char disc_path[1200];
static int disc_read;

const char *ps2_profile_disc(void) {
    struct _stat64 st;
    if (!profile_player_on || disc_given) return NULL;
    if (!disc_read) {
        disc_read = 1;
        if (!ini_get("paths", "disc", disc_path, sizeof disc_path)) disc_path[0] = 0;
        else if (_stat64(disc_path, &st) != 0) {
            ps2_log("profile: the disc of the settings file is gone: %s", disc_path);
            disc_path[0] = 0;
        }
    }
    return disc_path[0] ? disc_path : NULL;
}

int ps2_profile_disc_wanted(void) { return profile_player_on && !disc_given && !ps2_profile_disc(); }

/* The launcher's light check, an ISO9660 image or a folder with SYSTEM.CNF; ac0vr.exe's check (the serial, the executable's hash) comes
   before this one on a player's PC, and this asks only when the file went away since. */
const char *ps2_profile_disc_set(const char *path) {
    struct _stat64 st;
    char probe[1300];
    if (_stat64(path, &st) != 0) return "That path cannot be read.";
    if (st.st_mode & _S_IFDIR) {
        snprintf(probe, sizeof probe, "%s/SYSTEM.CNF", path);
        if (!have_file(probe)) return "That folder has no SYSTEM.CNF: not a PlayStation 2 disc.";
    } else {
        unsigned char b[6];
        FILE *f = fopen(path, "rb");
        int ok = f && _fseeki64(f, 16 * 2048, SEEK_SET) == 0 && fread(b, 1, 6, f) == 6 && !memcmp(b + 1, "CD001", 5);
        if (f) fclose(f);
        if (!ok) return "That file is not an ISO image with 2048-byte sectors.";
    }
    if (!ini_set_disc(path)) return "The settings file could not be written.";
    snprintf(disc_path, sizeof disc_path, "%s", path);
    disc_read = 1;
    ps2_log("profile: disc set: %s", path);
    return NULL;
}

/* ---- the 3D cockpits: the word `cockpit`, cockpits/map.txt (`id=file` lines) ---- */
/* a set's per-model switches, what a launcher script would export (the runtime's own lists name the public files only):
   cockpits/env.txt, `ACZ_...=value` lines, each only when unset */
static void table_cockpit_env(void) {
    static char line[8192];
    FILE *f = fopen("cockpits/env.txt", "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '='), *e = line + strlen(line);
        while (e > line && (e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
        if (!eq || strncmp(line, "ACZ_", 4)) continue;
        *eq = 0;
        if (getenv(line)) continue;
        _putenv_s(line, eq + 1);
        log_add(line, "(cockpits/env.txt)");
    }
    fclose(f);
}

static void table_cockpits(void) {
    static char m[12000];
    char first[400] = "", line[512];
    size_t n = 0;
    FILE *f;
    table_cockpit_env();
    if (!getenv("ACZ_VR_COCKPIT_MODELS") && (f = fopen("cockpits/map.txt", "r")) != NULL) {
        while (fgets(line, sizeof line, f)) {
            char *w = line, *r, *eq, file[400];
            if ((r = strchr(line, '#')) != NULL) *r = 0;
            for (r = line; *r; r++) if (*r != ' ' && *r != '\t' && *r != '\r' && *r != '\n') *w++ = *r;
            *w = 0;
            if (!(eq = strchr(line, '='))) continue;
            if (strchr(eq + 1, '/')) snprintf(file, sizeof file, "%s", eq + 1);
            else snprintf(file, sizeof file, "cockpits/%s", eq + 1);
            if (!have_file(file)) continue;
            *eq = 0;
            if (!first[0]) snprintf(first, sizeof first, "%s", file);
            n += (size_t)snprintf(m + n, sizeof m - n, "%s%s=%s", n ? "," : "", line, file);
            if (n >= sizeof m - 500) break;
        }
        fclose(f);
        if (n) {
            _putenv_s("ACZ_VR_COCKPIT_MODELS", m);
            log_add("ACZ_VR_COCKPIT_MODELS", "(cockpits/map.txt)");
        }
    }
    if (have_file("cockpits/f5e_t38.cpit")) set_unset("ACZ_VR_COCKPIT_MODEL", "cockpits/f5e_t38.cpit");
    else if (first[0]) set_unset("ACZ_VR_COCKPIT_MODEL", first);
}

/* Meta's runtime when it is installed, else the system's active one (OpenXR's own choice); [paths] xr_runtime = picks another without a
   shell (Virtual Desktop or SteamVR on a PC that also has Meta's app) */
static void table_runtime(void) {
    static const char meta[] = "C:\\Program Files\\Meta Horizon\\Support\\oculus-runtime\\oculus_openxr_64.json";
    char rt[1200];
    if (getenv("PS2_XR_RUNTIME") || getenv("XR_RUNTIME_JSON")) return;
    if (ini_get("paths", "xr_runtime", rt, sizeof rt)) set_unset("PS2_XR_RUNTIME", rt);
    else if (have_file(meta)) set_unset("PS2_XR_RUNTIME", meta);
}

/* ---- the current folder, the console, the log ---- */
static void wide_dir_up(wchar_t *p) {
    wchar_t *sl = wcsrchr(p, L'\\');
    if (sl) *sl = 0;
}

static void home_enter(void) {
    wchar_t dir[MAX_PATH + 64], probe[MAX_PATH + 96];
    DWORD n = GetModuleFileNameW(NULL, dir, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    wide_dir_up(dir);
    _snwprintf(probe, MAX_PATH + 96, L"%ls\\gen_zero\\ps2_image.bin", dir);
    if (GetFileAttributesW(probe) == INVALID_FILE_ATTRIBUTES) {
        wchar_t root[MAX_PATH + 64];
        wcscpy(root, dir);
        wide_dir_up(root);
        wide_dir_up(root);
        _snwprintf(probe, MAX_PATH + 96, L"%ls\\gen_zero\\ps2_image.bin", root);
        if (GetFileAttributesW(probe) != INVALID_FILE_ATTRIBUTES) wcscpy(dir, root);
    }
    SetCurrentDirectoryW(dir);
}

static int stdout_redirected(void) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD t = h && h != INVALID_HANDLE_VALUE ? GetFileType(h) : FILE_TYPE_UNKNOWN;
    return t == FILE_TYPE_DISK || t == FILE_TYPE_PIPE;
}

/* logs/zero_play_<stamp>.log: the 8 newest stay (the names sort by time). The paths are relative: the folder may be anything. */
static void own_log(void) {
    enum { KEEP_LOGS = 8 };
    char names[64][64], path[96];
    int n = 0;
    WIN32_FIND_DATAA fd;
    HANDLE fh;
    SYSTEMTIME t;
    CreateDirectoryA("logs", NULL);
    fh = FindFirstFileA("logs/zero_play_*.log", &fd);
    if (fh != INVALID_HANDLE_VALUE) {
        do {
            if (n < 64 && strlen(fd.cFileName) < sizeof names[0]) strcpy(names[n++], fd.cFileName);
        } while (FindNextFileA(fh, &fd));
        FindClose(fh);
    }
    qsort(names, (size_t)n, sizeof names[0], (int (*)(const void *, const void *))strcmp);
    for (int i = 0; i + KEEP_LOGS - 1 < n; i++) {          /* (this start's log is the next one) */
        snprintf(path, sizeof path, "logs/%s", names[i]);
        remove(path);
    }
    GetLocalTime(&t);
    snprintf(path, sizeof path, "logs/zero_play_%04d-%02d-%02d_%02d-%02d-%02d.log", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    if (!freopen(path, "w", stdout)) return;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (_fileno(stderr) < 0) freopen("NUL", "w", stderr);
    if (_fileno(stderr) >= 0) _dup2(_fileno(stdout), _fileno(stderr));
    setvbuf(stderr, NULL, _IONBF, 0);
    SetStdHandle(STD_OUTPUT_HANDLE, (HANDLE)_get_osfhandle(_fileno(stdout)));
    SetStdHandle(STD_ERROR_HANDLE, (HANDLE)_get_osfhandle(_fileno(stdout)));
}

/* ---- the command line ---- */
static char **u8_argv;
static int u8_argc;

void ps2_profile_args(int *argc, char ***argv) {
    if (u8_argv) { *argc = u8_argc; *argv = u8_argv; }
}

static void args_read(void) {
    int n = 0;
    wchar_t **w = CommandLineToArgvW(GetCommandLineW(), &n);
    if (!w || n < 1) return;
    u8_argv = (char **)calloc((size_t)n + 1, sizeof(char *));
    if (!u8_argv) return;
    for (int i = 0; i < n; i++) {
        int need = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, NULL, 0, NULL, NULL);
        u8_argv[i] = (char *)malloc(need > 0 ? (size_t)need : 1);
        u8_argv[i][0] = 0;
        if (need > 0) WideCharToMultiByte(CP_UTF8, 0, w[i], -1, u8_argv[i], need, NULL, NULL);
    }
    u8_argc = n;
    LocalFree(w);
}

/* A value of ours outside ASCII (a folder): the C library's getenv would hand it over in the code page, losing what the page has not;
   as UTF-8 it reaches ps2_fopen whole. Only names of ours: nothing else in this process reads the others through getenv with a path. */
static void env_to_utf8(void) {
    wchar_t *all = GetEnvironmentStringsW();
    if (!all) return;
    for (wchar_t *e = all; *e; e += wcslen(e) + 1) {
        const wchar_t *eq = wcschr(e + 1, L'=');
        int ascii = 1;
        if (!eq || (wcsncmp(e, L"ACZ_", 4) && wcsncmp(e, L"PS2_", 4))) continue;
        for (const wchar_t *c = eq; *c; c++) if (*c >= 0x80) ascii = 0;
        if (ascii) continue;
        {
            char name[128], val[4096];
            int nl = WideCharToMultiByte(CP_UTF8, 0, e, (int)(eq - e), name, sizeof name - 1, NULL, NULL);
            if (nl <= 0) continue;
            name[nl] = 0;
            if (WideCharToMultiByte(CP_UTF8, 0, eq + 1, -1, val, sizeof val, NULL, NULL) > 0) _putenv_s(name, val);
        }
    }
    FreeEnvironmentStringsW(all);
}

__attribute__((constructor(101))) static void profile_boot(void) {
    const char *pe;
    int script = 0;
    args_read();
    env_to_utf8();
    for (int i = 1; i < u8_argc; i++) {
        const char *a = u8_argv[i];
        if (!strcmp(a, "--version")) {
            printf("%s\n", ps2_version_line());
            fflush(stdout);
            ExitProcess(0);
        }
        if (!strcmp(a, "--disc") && i + 1 < u8_argc) { disc_given = 1; i++; }
        else if (!strcmp(a, "--mode") && i + 1 < u8_argc) {
            const char *m = u8_argv[++i];
            table_mode = !strcmp(m, "flat") ? M_FLAT : !strcmp(m, "stereo") ? M_STEREO : !strcmp(m, "mono") ? M_MONO
                       : !strcmp(m, "cinema") ? M_CINEMA : M_SHARP;
        }
        else if (!strcmp(a, "--watchdog") && i + 1 < u8_argc) i++;
        else if (!strcmp(a, "--hidden") || !strcmp(a, "--verbose")) continue;
        else script = 1;                                    /* --data, a test option, --help, something unknown */
    }
    pe = getenv("ACZ_PROFILE");
    if (pe && !strcmp(pe, "script")) script = 1;
    else if (pe && !strcmp(pe, "player")) script = 0;
    profile_player_on = !script;
    if (profile_player_on) {
        home_enter();
        if (!stdout_redirected()) {
            DWORD me[2];
            own_log();
            /* a console of its own: Explorer made it, nobody reads it. Last: the C library's handles of it must be closed first. After a
               FreeConsole they would be stale numbers, and the freopen above would close whatever took the number (it was the current
               folder's handle: every relative fopen then failed with EBADF). */
            if (GetConsoleProcessList(me, 2) == 1) {
                freopen("NUL", "r", stdin);
                FreeConsole();
            }
        }
    }
    ps2_log("version: %s", ps2_version_line());
    if (!profile_player_on) return;
    {
        wchar_t cw[MAX_PATH];
        char u8[MAX_PATH * 3];
        if (GetCurrentDirectoryW(MAX_PATH, cw) && WideCharToMultiByte(CP_UTF8, 0, cw, -1, u8, sizeof u8, NULL, NULL) > 0)
            ps2_log("profile: home folder %s", u8);
    }
    for (size_t i = 0; i < sizeof player_table / sizeof player_table[0]; i++)
        if (player_table[i].modes & table_mode) set_unset(player_table[i].var, player_table[i].val);
    table_cockpits();
    if (table_mode & M_XR) table_runtime();
    ps2_log("profile: a player's start: %s", table_log[0] ? table_log : "every switch was already set");
}
#else
int ps2_profile_player(void) { return 0; }
const char *ps2_profile_data_dir(void) { return "."; }
void ps2_profile_args(int *argc, char ***argv) { (void)argc; (void)argv; }
const char *ps2_profile_disc(void) { return NULL; }
int ps2_profile_disc_wanted(void) { return 0; }
const char *ps2_profile_disc_set(const char *path) { (void)path; return "No."; }
#endif
