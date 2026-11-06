/* ac0vr_update.c -- the updater of ac0vr.exe (setup_1105 package D, 2026-11-05; docs/SETTINGS.md "Updates"). Compiled into ac0vr_launcher.c.
 *
 * At start (and from the button) it asks https://api.github.com/repos/kulakovri/ac0vr/releases/latest through WinHTTP with a 3-second timeout; offline or
 * rate-limited is silent and the game starts. A newer tag_name than version.txt brings the window with UPDATE, the release's name and the first
 * lines of its notes. [update] check = 0 in the settings file or ACZ_UPDATE_CHECK=0 turns the check at start off; ACZ_UPDATE_URL points at another
 * JSON, and file:/// works for it and for its assets (the tests).
 *
 * The update: ac0vr-<version>-update.zip and SHA256SUMS downloaded into staging.dl/, the zip's hash verified, unpacked into staging/ (miniz, from
 * third_party/miniz), and only then swapped into place: first what is gone from the folders the zip owns, then each file that differs (so a file that
 * stays keeps its date, which the compile stage and the pipeline cache look at), version.txt last. ac0vr.exe replaces itself through a renamed
 * ac0vr.exe.old, removed at the next start. Then the launcher's build with the cache: a runtime-only release relinks in seconds, a recompiler release
 * rebuilds. Nothing under build/zero/ is touched before the new exe is linked, so anything that fails leaves the previous build runnable: the
 * window offers it. The toolchain comes only when bundle.txt's toolchain_id changed, as its own asset ac0vr-toolchain-<id>.zip.
 *
 * The hash catches a damaged download; it comes from the same place as the zip, so it is no signature. */
#include <shlwapi.h>
#include <winhttp.h>
#include "miniz.h"

static int opt_check_updates, opt_auto_update;
static int upd_pending, upd_busy;
static char upd_tag[64], upd_version[64], upd_name[200], upd_url_zip[1024], upd_url_sums[1024], upd_url_tc[1024];
static char upd_zip_name[128], upd_notes[2000];
static HANDLE upd_thread;

/* ---- fetching: http(s) through WinHTTP, file:/// from the disc ---- */
typedef int (*sink_fn)(const void *, DWORD, void *);

static int fetch_file(const wchar_t *url, sink_fn sink, void *ctx, unsigned long long *total) {
    wchar_t path[MAX_PATH * 2];
    DWORD n = MAX_PATH * 2;
    FILE *f;
    static char buf[1 << 16];
    size_t got;
    int ok = 1;
    if (PathCreateFromUrlW(url, path, &n, 0) != S_OK || !(f = _wfopen(path, L"rb"))) return 0;
    _fseeki64(f, 0, SEEK_END);
    if (total) *total = (unsigned long long)_ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    while (ok && (got = fread(buf, 1, sizeof buf, f)) > 0) ok = sink(buf, (DWORD)got, ctx);
    fclose(f);
    return ok;
}

static int fetch_http(const wchar_t *url, int timeout_ms, int recv_ms, sink_fn sink, void *ctx, unsigned long long *total) {
    URL_COMPONENTSW u;
    wchar_t host[256], rest[1024];
    HINTERNET s = NULL, c = NULL, r = NULL;
    DWORD status = 0, len = sizeof status;
    int ok = 0;
    memset(&u, 0, sizeof u);
    u.dwStructSize = sizeof u;
    u.lpszHostName = host;
    u.dwHostNameLength = 256;
    u.lpszUrlPath = rest;
    u.dwUrlPathLength = 1024;
    if (!WinHttpCrackUrl(url, 0, 0, &u)) return 0;
    s = WinHttpOpen(L"ac0vr", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) goto done;
    WinHttpSetTimeouts(s, timeout_ms, timeout_ms, timeout_ms, recv_ms);
    if (!(c = WinHttpConnect(s, host, u.nPort, 0))) goto done;
    r = WinHttpOpenRequest(c, L"GET", rest, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, u.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!r) goto done;
    WinHttpAddRequestHeaders(r, L"Accept: application/vnd.github+json\r\n", (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
    if (!WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(r, NULL)) goto done;
    WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) goto done;                     /* 403: rate-limited, 404: no release yet: silent */
    if (total) {
        wchar_t cl[32];
        DWORD l = sizeof cl;
        *total = WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, cl, &l, WINHTTP_NO_HEADER_INDEX) ? (unsigned long long)_wtoi64(cl) : 0;
    }
    ok = 1;
    for (;;) {
        static char buf[1 << 16];
        DWORD got = 0;
        if (!WinHttpReadData(r, buf, sizeof buf, &got)) { ok = 0; break; }
        if (!got) break;
        if (!sink(buf, got, ctx)) { ok = 0; break; }
    }
done:
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    return ok;
}

static int fetch(const char *url8, int timeout_ms, int recv_ms, sink_fn sink, void *ctx, unsigned long long *total) {
    wchar_t url[1200];
    if (MultiByteToWideChar(CP_UTF8, 0, url8, -1, url, 1200) <= 0) return 0;
    if (!_wcsnicmp(url, L"file:", 5)) return fetch_file(url, sink, ctx, total);
    return fetch_http(url, timeout_ms, recv_ms, sink, ctx, total);
}

typedef struct { char *d; size_t n, cap; } membuf;

static int sink_mem(const void *p, DWORD n, void *ctx) {
    membuf *m = (membuf *)ctx;
    if (m->n + n + 1 > m->cap) {
        size_t cap = (m->n + n + 1) * 2;
        char *d = (char *)realloc(m->d, cap);
        if (!d || cap > (8u << 20)) { free(d); m->d = NULL; return 0; }
        m->d = d;
        m->cap = cap;
    }
    memcpy(m->d + m->n, p, n);
    m->n += n;
    m->d[m->n] = 0;
    return 1;
}

/* ---- the release's JSON: the top level's tag_name, name and body, and each asset's name and URL ---- */
static const char *js_ws(const char *p) { while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++; return p; }

static const char *js_str(const char *p, char *out, size_t cap) {       /* at the opening quote; out may be NULL */
    size_t l = 0;
    if (*p != '"') return NULL;
    for (p++; *p && *p != '"'; p++) {
        char c = *p;
        if (c == '\\') {
            p++;
            switch (*p) {
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'u': {
                unsigned v = 0;
                for (int i = 1; i <= 4 && p[i]; i++) {
                    char h = p[i];
                    v = v * 16 + (unsigned)(h >= '0' && h <= '9' ? h - '0' : (h | 32) - 'a' + 10);
                }
                p += 4;
                if (out && l + 4 < cap) {
                    if (v < 0x80) out[l++] = (char)v;
                    else if (v < 0x800) { out[l++] = (char)(0xC0 | v >> 6); out[l++] = (char)(0x80 | (v & 63)); }
                    else { out[l++] = (char)(0xE0 | v >> 12); out[l++] = (char)(0x80 | ((v >> 6) & 63)); out[l++] = (char)(0x80 | (v & 63)); }
                }
                continue; }
            case 0: return NULL;
            default: c = *p;
            }
        }
        if (out && l + 1 < cap) out[l++] = c;
    }
    if (out && cap) out[l] = 0;
    return *p == '"' ? p + 1 : NULL;
}

static const char *js_skip(const char *p) {                              /* past one value */
    p = js_ws(p);
    if (*p == '"') return js_str(p, NULL, 0);
    if (*p == '{' || *p == '[') {
        int depth = 0;
        do {
            if (*p == '"') { if (!(p = js_str(p, NULL, 0))) return NULL; continue; }
            if (*p == '{' || *p == '[') depth++;
            else if (*p == '}' || *p == ']') depth--;
            else if (!*p) return NULL;
            p++;
        } while (depth);
        return p;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']') p++;
    return p;
}

static int upd_parse(const char *js) {
    const char *p = js_ws(js);
    char key[32];
    upd_tag[0] = upd_name[0] = upd_notes[0] = upd_url_zip[0] = upd_url_sums[0] = upd_url_tc[0] = 0;
    if (*p != '{') return 0;
    p++;
    while (p && *(p = js_ws(p)) && *p != '}') {
        if (*p == ',') { p++; continue; }
        if (!(p = js_str(p, key, sizeof key))) return 0;
        p = js_ws(p);
        if (*p != ':') return 0;
        p = js_ws(p + 1);
        if (!strcmp(key, "tag_name")) p = js_str(p, upd_tag, sizeof upd_tag);
        else if (!strcmp(key, "name") && *p == '"') p = js_str(p, upd_name, sizeof upd_name);
        else if (!strcmp(key, "body") && *p == '"') p = js_str(p, upd_notes, sizeof upd_notes);
        else if (!strcmp(key, "assets") && *p == '[') {
            const char *e = js_skip(p);
            /* each element: its name and browser_download_url, in any order */
            p = js_ws(p + 1);
            while (p && p < e && *p != ']') {
                char an[160] = "", au[1024] = "";
                const char *q;
                if (*p == ',') { p = js_ws(p + 1); continue; }
                if (*p != '{') { p = js_skip(p); continue; }
                q = js_ws(p + 1);
                while (q && *(q = js_ws(q)) && *q != '}') {
                    char k2[32];
                    if (*q == ',') { q++; continue; }
                    if (!(q = js_str(q, k2, sizeof k2))) break;
                    q = js_ws(q);
                    if (*q != ':') break;
                    q = js_ws(q + 1);
                    if (!strcmp(k2, "name") && *q == '"') q = js_str(q, an, sizeof an);
                    else if (!strcmp(k2, "browser_download_url") && *q == '"') q = js_str(q, au, sizeof au);
                    else q = js_skip(q);
                }
                if (!strcmp(an, "SHA256SUMS")) snprintf(upd_url_sums, sizeof upd_url_sums, "%s", au);
                else if (strstr(an, "-update.zip")) { snprintf(upd_url_zip, sizeof upd_url_zip, "%s", au); snprintf(upd_zip_name, sizeof upd_zip_name, "%s", an); }
                else if (!strncmp(an, "ac0vr-toolchain-", 16)) snprintf(upd_url_tc, sizeof upd_url_tc, "%s", au);
                p = js_ws(js_skip(p));
            }
            p = e;
        } else p = js_skip(p);
    }
    return upd_tag[0] != 0;
}

/* 1 when a is newer than b: dotted numbers; 0.1.0 is newer than 0.1.0-dev */
static int version_newer(const char *a, const char *b) {
    for (;;) {
        char *ea, *eb;
        long x = strtol(a, &ea, 10), y = strtol(b, &eb, 10);
        if (x != y) return x > y;
        a = ea; b = eb;
        if (*a == '.' && *b == '.') { a++; b++; continue; }
        if (*a == '.') return 1;
        if (*b == '.') return 0;
        if (!*a && *b) return 1;                /* a release against its -dev */
        return 0;
    }
}

/* ---- the check ---- */
static void ini_update_off(int *off) {
    wchar_t p[MAX_PATH * 2];
    char *b, *s;
    ini_path(p, MAX_PATH * 2);
    if (!(b = read_all(p, NULL))) return;
    if ((s = strstr(b, "[update]")) != NULL) {
        char *n = strchr(s + 1, '[');
        char *k = strstr(s, "check");
        if (k && (!n || k < n)) {
            k = strchr(k, '=');
            if (k && atoi(k + 1) == 0) *off = 1;
        }
    }
    free(b);
}

static int upd_check(int timeout_ms) {
    static const char *def = "https://api.github.com/repos/kulakovri/ac0vr/releases/latest";
    const wchar_t *wu = _wgetenv(L"ACZ_UPDATE_URL");
    char url[1200];
    membuf m = { NULL, 0, 0 };
    int ok;
    char have[64];
    const char *tag;
    if (wu && *wu) WideCharToMultiByte(CP_UTF8, 0, wu, -1, url, sizeof url, NULL, NULL);
    else snprintf(url, sizeof url, "%s", def);
    ok = fetch(url, timeout_ms, timeout_ms, sink_mem, &m, NULL) && m.d && upd_parse(m.d);
    free(m.d);
    upd_pending = 0;
    if (!ok) return 0;
    tag = upd_tag[0] == 'v' ? upd_tag + 1 : upd_tag;
    snprintf(upd_version, sizeof upd_version, "%s", tag);
    snprintf(have, sizeof have, "%s", release_line);
    have[strcspn(have, " ")] = 0;
    upd_pending = version_newer(tag, have) && upd_url_zip[0] && upd_url_sums[0];
    return 1;
}

static volatile LONG upd_gave_up;

static DWORD WINAPI upd_check_thread(void *arg) {
    (void)arg;
    upd_check(3000);
    return 0;
}

/* on a thread, with the whole 3 seconds counted here: WinHTTP's timeouts are per step and the proxy's discovery comes on top. A late answer is
   ignored: the game has started, or the window is open with the button. */
static void ac0vr_update_at_start(void) {
    const wchar_t *e = _wgetenv(L"ACZ_UPDATE_CHECK");
    int off = 0;
    HANDLE t;
    ini_update_off(&off);
    if (e) off = !wcscmp(e, L"0");                     /* the environment wins over the file */
    if (off && !opt_check_updates) return;
    t = CreateThread(NULL, 0, upd_check_thread, NULL, 0, NULL);
    if (t) {
        if (WaitForSingleObject(t, 3000) != WAIT_OBJECT_0) InterlockedExchange(&upd_gave_up, 1);
        CloseHandle(t);
    }
}

static int ac0vr_update_pending(void) { return upd_pending && !upd_gave_up; }

static void upd_show_offer(void) {
    char t[300];
    char notes[900];
    size_t l = 0;
    int lines = 0;
    const char *s = upd_notes;
    wchar_t w[1000];
    snprintf(t, sizeof t, "A new version is out: %s%s%s", upd_version, upd_name[0] ? ": " : "", upd_name);
    set_text_u8(h_stage, t);
    while (*s && lines < 8 && l + 2 < sizeof notes) {  /* the first lines of the notes; the edit control wants CRLF */
        if (*s == '\r') { s++; continue; }
        if (*s == '\n') { notes[l++] = '\r'; lines++; }
        notes[l++] = *s++;
    }
    notes[l] = 0;
    if (MultiByteToWideChar(CP_UTF8, 0, notes, -1, w, 1000) > 0) SetWindowTextW(h_log, w);
    ShowWindow(h_log, SW_SHOW);
    SetWindowTextW(h_update, L"UPDATE");
    SetWindowTextW(h_build, need_build ? L"Build the game from your disc" : L"Start the game");
}

/* ---- the update, on its own thread ---- */
static void upd_say(const char *text, int fraction_permille) { PostMessageW(win, WM_UPDATE_TEXT, (WPARAM)fraction_permille, (LPARAM)_strdup(text)); }

typedef struct { FILE *f; sha256_ctx sha; unsigned long long got, total; const char *label; DWORD last; } dl_ctx;

static int sink_file(const void *p, DWORD n, void *ctx) {
    dl_ctx *d = (dl_ctx *)ctx;
    if (fwrite(p, 1, n, d->f) != n) return 0;
    sha_add(&d->sha, p, n);
    d->got += n;
    if (GetTickCount() - d->last > 200) {
        char t[120];
        d->last = GetTickCount();
        snprintf(t, sizeof t, "%s: %.1f MB", d->label, (double)d->got / 1e6);
        upd_say(t, -1);
    }
    return 1;
}

/* into FILE; hex gets the SHA-256 */
static int download(const char *url, const wchar_t *file, const char *label, char hex[65]) {
    dl_ctx d;
    int ok;
    memset(&d, 0, sizeof d);
    d.label = label;
    sha_init(&d.sha);
    if (!(d.f = _wfopen(file, L"wb"))) return 0;
    ok = fetch(url, 15000, 60000, sink_file, &d, &d.total);
    if (fclose(d.f) != 0) ok = 0;
    if (ok) sha_hex(&d.sha, hex);
    return ok;
}

/* "<hex> *name" or "<hex>  name" */
static int sums_has(const char *sums, const char *hex, const char *name) {
    const char *s = sums;
    size_t nl = strlen(name);
    while (s && *s) {
        const char *e = strchr(s, '\n');
        size_t l = e ? (size_t)(e - s) : strlen(s);
        if (l > 66 && !_strnicmp(s, hex, 64)) {
            const char *n = s + 65;
            while (*n == ' ' || *n == '*') n++;
            if ((size_t)(s + l - n) >= nl && !strncmp(n, name, nl) && (n[nl] == '\r' || n[nl] == '\n' || !n[nl])) return 1;
        }
        s = e ? e + 1 : NULL;
    }
    return 0;
}

static size_t zip_read(void *opaque, mz_uint64 ofs, void *buf, size_t n) {
    FILE *f = (FILE *)opaque;
    if (_fseeki64(f, (long long)ofs, SEEK_SET)) return 0;
    return fread(buf, 1, n, f);
}

static size_t zip_write(void *opaque, mz_uint64 ofs, const void *buf, size_t n) {
    (void)ofs;
    return fwrite(buf, 1, n, (FILE *)opaque);
}

/* every name stays under the folder: no drive, no .., no absolute path (a damaged or hostile zip writes nowhere else) */
static int zip_name_ok(const char *n) {
    const char *p;
    if (!*n || *n == '/' || *n == '\\' || strchr(n, ':')) return 0;
    for (p = n; *p; ) {
        const char *e = p;
        while (*e && *e != '/' && *e != '\\') e++;
        if (e - p == 2 && p[0] == '.' && p[1] == '.') return 0;
        p = *e ? e + 1 : e;
    }
    return 1;
}

static void mkdirs_for(wchar_t *file) {
    wchar_t *p;
    for (p = file; (p = wcspbrk(p, L"\\/")) != NULL; p++) {
        wchar_t c = *p;
        if (p > file && p[-1] == L':') continue;
        *p = 0;
        CreateDirectoryW(file, NULL);
        *p = c;
    }
}

static int unpack(const wchar_t *zip, const wchar_t *dest, char *err, size_t cap) {
    mz_zip_archive a;
    FILE *f = _wfopen(zip, L"rb");
    long long size;
    int ok = 1;
    mz_uint n;
    if (!f) { snprintf(err, cap, "The download cannot be read."); return 0; }
    _fseeki64(f, 0, SEEK_END);
    size = _ftelli64(f);
    memset(&a, 0, sizeof a);
    a.m_pRead = zip_read;
    a.m_pIO_opaque = f;
    if (!mz_zip_reader_init(&a, (mz_uint64)size, 0)) { fclose(f); snprintf(err, cap, "The download is not a zip file."); return 0; }
    n = mz_zip_reader_get_num_files(&a);
    for (mz_uint i = 0; ok && i < n; i++) {
        mz_zip_archive_file_stat st;
        wchar_t rel[MAX_PATH], out[MAX_PATH * 2];
        FILE *o;
        if (!mz_zip_reader_file_stat(&a, i, &st)) { ok = 0; break; }
        if (st.m_is_directory) continue;
        if (!zip_name_ok(st.m_filename)) { snprintf(err, cap, "A name in the zip goes outside its folder: %s", st.m_filename); ok = 0; break; }
        if (MultiByteToWideChar(CP_UTF8, 0, st.m_filename, -1, rel, MAX_PATH) <= 0) { ok = 0; break; }
        _snwprintf(out, MAX_PATH * 2, L"%ls\\%ls", dest, rel);
        for (wchar_t *c = out; *c; c++) if (*c == L'/') *c = L'\\';
        mkdirs_for(out);
        if (!(o = _wfopen(out, L"wb"))) { snprintf(err, cap, "Cannot write into staging: %s", st.m_filename); ok = 0; break; }
        if (!mz_zip_reader_extract_to_callback(&a, i, zip_write, o, 0)) { snprintf(err, cap, "A file in the zip is damaged: %s", st.m_filename); ok = 0; }
        if (fclose(o) != 0) ok = 0;
    }
    mz_zip_reader_end(&a);
    fclose(f);
    if (!ok && !err[0]) snprintf(err, cap, "The zip could not be unpacked.");
    return ok;
}

static void remove_tree(const wchar_t *dir) {
    wchar_t pat[MAX_PATH * 2];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    _snwprintf(pat, MAX_PATH * 2, L"%ls\\*", dir);
    h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            wchar_t p[MAX_PATH * 2];
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            _snwprintf(p, MAX_PATH * 2, L"%ls\\%ls", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(p);
            else DeleteFileW(p);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

static int same_file(const wchar_t *a, const wchar_t *b) {
    size_t na = 0, nb = 0;
    char *x = read_all(a, &na), *y = x ? read_all(b, &nb) : NULL;
    int same = x && y && na == nb && !memcmp(x, y, na);
    free(x);
    free(y);
    return same;
}

/* what the zip's folders no longer hold goes away: a shader or a header of the old release must not linger */
static void drop_stale(const wchar_t *staging, const wchar_t *target) {
    wchar_t pat[MAX_PATH * 2];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    _snwprintf(pat, MAX_PATH * 2, L"%ls\\*", target);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        wchar_t s[MAX_PATH * 2], t[MAX_PATH * 2];
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        _snwprintf(s, MAX_PATH * 2, L"%ls\\%ls", staging, fd.cFileName);
        _snwprintf(t, MAX_PATH * 2, L"%ls\\%ls", target, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (exists(s)) drop_stale(s, t);
            else remove_tree(t);
        } else if (!exists(s)) DeleteFileW(t);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* each file of staging into place, when it differs. version.txt and ac0vr.exe are left for the end. */
static int swap_tree(const wchar_t *staging, const wchar_t *target, const wchar_t *self, char *err, size_t cap) {
    wchar_t pat[MAX_PATH * 2];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int ok = 1;
    _snwprintf(pat, MAX_PATH * 2, L"%ls\\*", staging);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 1;
    do {
        wchar_t s[MAX_PATH * 2], t[MAX_PATH * 2];
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        _snwprintf(s, MAX_PATH * 2, L"%ls\\%ls", staging, fd.cFileName);
        _snwprintf(t, MAX_PATH * 2, L"%ls\\%ls", target, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            CreateDirectoryW(t, NULL);
            ok = swap_tree(s, t, self, err, cap);
        } else if (self && (!wcscmp(fd.cFileName, L"version.txt") || !wcscmp(fd.cFileName, L"ac0vr.exe"))) {
            continue;
        } else if (!same_file(s, t) && !MoveFileExW(s, t, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            char nm[MAX_PATH * 3];
            WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, nm, sizeof nm, NULL, NULL);
            snprintf(err, cap, "Could not replace %s (error %lu). Is the game still running?", nm, (unsigned long)GetLastError());
            ok = 0;
        }
    } while (ok && FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

static DWORD WINAPI update_main(void *arg) {
    static char err[600];
    wchar_t dl[MAX_PATH * 2], staging[MAX_PATH * 2], zip[MAX_PATH * 2], tczip[MAX_PATH * 2];
    char hex[65] = "", label[200];
    membuf sums = { NULL, 0, 0 };
    int ok = 0;
    (void)arg;
    err[0] = 0;
    join(dl, MAX_PATH * 2, L"staging.dl");
    join(staging, MAX_PATH * 2, L"staging");
    remove_tree(staging);
    remove_tree(dl);
    CreateDirectoryW(dl, NULL);
    CreateDirectoryW(staging, NULL);
    _snwprintf(zip, MAX_PATH * 2, L"%ls\\update.zip", dl);
    _snwprintf(tczip, MAX_PATH * 2, L"%ls\\toolchain.zip", dl);

    snprintf(label, sizeof label, "Downloading %s", upd_version);
    upd_say(label, 0);
    if (!download(upd_url_zip, zip, label, hex)) { snprintf(err, sizeof err, "The download failed. Nothing has changed."); goto end; }
    upd_say("Verifying the download", 400);
    if (!fetch(upd_url_sums, 15000, 15000, sink_mem, &sums, NULL) || !sums.d) { snprintf(err, sizeof err, "SHA256SUMS could not be fetched. Nothing has changed."); goto end; }
    if (!sums_has(sums.d, hex, upd_zip_name)) { snprintf(err, sizeof err, "The download is damaged: its SHA-256 is not in the release's SHA256SUMS. Nothing has changed."); goto end; }
    upd_say("Unpacking", 500);
    if (!unpack(zip, staging, err, sizeof err)) goto end;
    {   /* no file of a zip that is no update of this program comes near the install */
        wchar_t v[MAX_PATH * 2], t[MAX_PATH * 2];
        _snwprintf(v, MAX_PATH * 2, L"%ls\\version.txt", staging);
        _snwprintf(t, MAX_PATH * 2, L"%ls\\bundle\\bundle.txt", staging);
        if (!exists(v) || !exists(t)) { snprintf(err, sizeof err, "This zip is not an update of ac0vr. Nothing has changed."); goto end; }
    }
    {   /* bundle.txt names the toolchain: a new one is a separate asset */
        wchar_t have[MAX_PATH * 2], want[MAX_PATH * 2];
        char *a, *b;
        int differs;
        const char *ka, *kb;
        join(have, MAX_PATH * 2, L"bundle\\bundle.txt");
        _snwprintf(want, MAX_PATH * 2, L"%ls\\bundle\\bundle.txt", staging);
        a = read_all(have, NULL);
        b = read_all(want, NULL);
        ka = a ? strstr(a, "toolchain_id=") : NULL;
        kb = b ? strstr(b, "toolchain_id=") : NULL;
        differs = ka && kb && strncmp(ka, kb, strcspn(kb, "\r\n"));
        free(a);
        if (differs) {
            char hex2[65] = "", tc_name[160];
            wchar_t tcdest[MAX_PATH * 2];
            const char *id = kb + 13;
            size_t il = strcspn(id, "\r\n");
            snprintf(tc_name, sizeof tc_name, "ac0vr-toolchain-%.*s.zip", (int)il, id);
            if (!upd_url_tc[0]) { snprintf(err, sizeof err, "This release needs a new compiler, and its asset is missing. Nothing has changed."); free(b); goto end; }
            upd_say("Downloading the new compiler", 550);
            if (!download(upd_url_tc, tczip, "Downloading the new compiler", hex2) || !sums_has(sums.d, hex2, tc_name)) {
                snprintf(err, sizeof err, "The compiler's download failed or is damaged. Nothing has changed.");
                free(b);
                goto end;
            }
            _snwprintf(tcdest, MAX_PATH * 2, L"%ls\\bundle\\toolchain", staging);
            CreateDirectoryW(tcdest, NULL);
            if (!unpack(tczip, tcdest, err, sizeof err)) { free(b); goto end; }
        }
        free(b);
    }
    upd_say("Putting the files in place", 800);
    {
        /* the folders the zip owns lose what it no longer has. Not the toolchain (it comes by itself), not python/, build/, saves. */
        static const wchar_t *owned[] = { L"bundle\\runtime", L"bundle\\spirv", L"bundle\\dll", L"bundle\\recomp", L"bundle\\data", L"tools\\setup", L"docs", L"licenses" };
        wchar_t self[MAX_PATH * 2], old[MAX_PATH * 2], s[MAX_PATH * 2];
        for (size_t i = 0; i < sizeof owned / sizeof owned[0]; i++) {
            wchar_t a[MAX_PATH * 2], b[MAX_PATH * 2];
            _snwprintf(a, MAX_PATH * 2, L"%ls\\%ls", staging, owned[i]);
            join(b, MAX_PATH * 2, owned[i]);
            if (exists(a)) drop_stale(a, b);
        }
        join(self, MAX_PATH * 2, L"ac0vr.exe");
        if (!swap_tree(staging, root, self, err, sizeof err)) goto end;
        /* ac0vr.exe: a running exe can be renamed, not overwritten; the next start removes the .old */
        _snwprintf(s, MAX_PATH * 2, L"%ls\\ac0vr.exe", staging);
        if (exists(s) && !same_file(s, self)) {
            join(old, MAX_PATH * 2, L"ac0vr.exe.old");
            DeleteFileW(old);
            if (!MoveFileExW(self, old, MOVEFILE_REPLACE_EXISTING) || !MoveFileExW(s, self, MOVEFILE_REPLACE_EXISTING)) {
                snprintf(err, sizeof err, "Could not replace ac0vr.exe (error %lu).", (unsigned long)GetLastError());
                goto end;
            }
        }
        _snwprintf(s, MAX_PATH * 2, L"%ls\\version.txt", staging);
        join(old, MAX_PATH * 2, L"version.txt");
        if (!MoveFileExW(s, old, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { snprintf(err, sizeof err, "Could not write version.txt."); goto end; }
    }
    ok = 1;
end:
    free(sums.d);
    remove_tree(staging);
    remove_tree(dl);
    if (!ok) upd_say(err, -2);
    PostMessageW(win, WM_UPDATE_END, ok ? 0 : 1, 0);
    return 0;
}

static void upd_begin(void) {
    if (upd_busy || building) return;
    upd_busy = 1;
    building = 1;
    upd_pending = 0;
    ShowWindow(h_log, SW_HIDE);
    ShowWindow(h_prev, SW_HIDE);
    ShowWindow(h_bar, SW_SHOW);
    ShowWindow(h_left, SW_SHOW);
    SetWindowTextW(h_left, L"");
    SendMessageW(h_bar, PBM_SETPOS, 0, 0);
    EnableWindow(h_cancel, FALSE);
    refresh_buttons();
    upd_thread = CreateThread(NULL, 0, update_main, NULL, 0, NULL);
}

/* the thread's words: permille >= 0 moves the bar, -1 only the words, -2 the failure's reason */
static void ac0vr_update_text(const char *text, int permille) {
    if (permille == -2) {
        set_text_u8(h_log, text);
        ShowWindow(h_log, SW_SHOW);
        return;
    }
    set_text_u8(h_stage, text);
    if (permille >= 0) SendMessageW(h_bar, PBM_SETPOS, (WPARAM)permille, 0);
    shot();
}

static void ac0vr_update_ended(DWORD code) {
    wchar_t e[MAX_PATH * 2];
    upd_busy = 0;
    building = 0;
    EnableWindow(h_cancel, TRUE);
    if (upd_thread) { CloseHandle(upd_thread); upd_thread = NULL; }
    if (code) {
        SetWindowTextW(h_stage, L"The update failed.");
        join(e, MAX_PATH * 2, L"build\\zero\\ac5.exe");
        if (exists(e)) ShowWindow(h_prev, SW_SHOW);    /* the previous build still runs */
        refresh_buttons();
        shot();
        if (opt_auto_update) DestroyWindow(win);
        return;
    }
    read_release();                                    /* the new version.txt */
    need_build = 1;
    {
        wchar_t title[140], rel[100];
        MultiByteToWideChar(CP_UTF8, 0, release_line, -1, rel, 100);
        _snwprintf(title, 140, L"ac0vr %ls", rel);
        SetWindowTextW(win, title);
    }
    GetWindowTextW(h_path, disc, MAX_PATH * 2);
    if (!disc[0] || !exists(disc)) {
        SetWindowTextW(h_stage, L"Updated. The disc is needed for the build: choose it above.");
        refresh_buttons();
        return;
    }
    build_start();
}

static void ac0vr_update_button(void) {
    if (ac0vr_update_pending()) { upd_begin(); return; }
    upd_gave_up = 0;
    SetWindowTextW(h_stage, L"Checking for updates");
    UpdateWindow(h_stage);
    if (!upd_check(5000)) SetWindowTextW(h_stage, L"No answer from the release page: offline, or asked too often. Try later.");
    else if (upd_pending) upd_show_offer();
    else {
        wchar_t t[160], rel[100];
        MultiByteToWideChar(CP_UTF8, 0, release_line, -1, rel, 100);
        _snwprintf(t, 160, L"This is the newest version (%ls).", rel);
        SetWindowTextW(h_stage, t);
    }
}

/* the window has opened */
static void ac0vr_update_dialog_shown(void) {
    if (ac0vr_update_pending()) {
        upd_show_offer();
        shot();
        if (opt_auto_update) upd_begin();
    } else if (opt_check_updates) ac0vr_update_button();
}
