/* ps2_path.c -- UTF-8 file names on Windows (setup_1105; ps2_path.h says why). A name that is pure ASCII takes the C library's own
   function, as before: no cost and no change for every path a script gives. */
#define PS2_PATH_NO_MACROS
#include "ps2_path.h"

#ifdef _WIN32
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define WIDE_MAX 2048

static int is_ascii(const char *s) {
    for (; *s; s++) if ((unsigned char)*s >= 0x80) return 0;
    return 1;
}

int ps2_path_wide(const char *path, wchar_t *out, int cap) {
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, out, cap) > 0) return 1;
    return MultiByteToWideChar(CP_ACP, 0, path, -1, out, cap) > 0;
}

FILE *ps2_fopen(const char *path, const char *mode) {
    wchar_t w[WIDE_MAX], m[16];
    int i;
    if (is_ascii(path)) return fopen(path, mode);
    if (!ps2_path_wide(path, w, WIDE_MAX)) { errno = ENOENT; return NULL; }
    for (i = 0; mode[i] && i < 15; i++) m[i] = (wchar_t)(unsigned char)mode[i];
    m[i] = 0;
    return _wfopen(w, m);
}

int ps2_remove(const char *path) {
    wchar_t w[WIDE_MAX];
    if (is_ascii(path)) return remove(path);
    if (!ps2_path_wide(path, w, WIDE_MAX)) { errno = ENOENT; return -1; }
    return _wremove(w);
}

int ps2_rename(const char *from, const char *to) {
    wchar_t a[WIDE_MAX], b[WIDE_MAX];
    if (is_ascii(from) && is_ascii(to)) return rename(from, to);
    if (!ps2_path_wide(from, a, WIDE_MAX) || !ps2_path_wide(to, b, WIDE_MAX)) { errno = ENOENT; return -1; }
    return _wrename(a, b);
}

int ps2_mkdir(const char *path) {
    wchar_t w[WIDE_MAX];
    if (is_ascii(path)) return _mkdir(path);
    if (!ps2_path_wide(path, w, WIDE_MAX)) { errno = ENOENT; return -1; }
    return _wmkdir(w);
}

int ps2_stat64(const char *path, struct _stat64 *st) {
    wchar_t w[WIDE_MAX];
    if (is_ascii(path)) return _stat64(path, st);
    if (!ps2_path_wide(path, w, WIDE_MAX)) { errno = ENOENT; return -1; }
    return _wstat64(w, st);
}

/* the folder functions always go wide: a name inside the folder may be anything, and comes back as UTF-8 */
struct ps2_dir { _WDIR *d; struct ps2_dirent e; };

ps2_dir *ps2_opendir(const char *path) {
    wchar_t w[WIDE_MAX];
    ps2_dir *r;
    _WDIR *d;
    if (!ps2_path_wide(path, w, WIDE_MAX) || !(d = _wopendir(w))) return NULL;
    r = (ps2_dir *)malloc(sizeof *r);
    if (!r) { _wclosedir(d); return NULL; }
    r->d = d;
    return r;
}

struct ps2_dirent *ps2_readdir(ps2_dir *r) {
    struct _wdirent *e = _wreaddir(r->d);
    if (!e) return NULL;
    if (WideCharToMultiByte(CP_UTF8, 0, e->d_name, -1, r->e.d_name, (int)sizeof r->e.d_name, NULL, NULL) <= 0) r->e.d_name[0] = 0;
    return &r->e;
}

int ps2_closedir(ps2_dir *r) {
    int rc = _wclosedir(r->d);
    free(r);
    return rc;
}
#endif
