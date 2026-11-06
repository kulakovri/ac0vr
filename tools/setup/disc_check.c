/* disc_check -- is this the player's own US disc of the game? (night_setup 2026-11-04; RELEASE_RESEARCH R4 item 1)
 *
 *   disc_check                     look at every disc drive
 *   disc_check D:                  a drive, or a folder with the disc's files
 *   disc_check path/to/game.iso    an ISO image (2048-byte sectors)
 *   disc_check --extract OUT PATH  on success also copy the game's executable to the file OUT (the recompiler's input)
 *
 * It reads SYSTEM.CNF for the boot file (the serial), finds that file, hashes it (SHA-256), and accepts only SLUS_213.46
 * with the executable the recompiler's configuration was made for. Everything is opened read-only; only OUT is written.
 * Prints one plain paragraph for the player and a last line `result: ...`. Exit status: 0 accepted, 1 a disc or image that is
 * not the right one, 2 nothing readable (no drive, no disc, bad path, not an ISO). Build: any of the three bundled compilers,
 *   gcc -O2 -o disc_check.exe tools/setup/disc_check.c -lshell32
 * Self-test (fixtures only, no game data): python -I tools/setup/test_disc_check.py disc_check.exe
 * ac0vr.exe calls disc_check_run() in-process and offers the same check as `ac0vr.exe --check-disc PATH` (the installer's disc page).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define WANT_SERIAL "SLUS_213.46"
/* SHA-256 of the US executable SLUS_213.46, 3293064 bytes (recompiled by tools/ps2recomp with config_zero/) */
#define WANT_SHA256 "34ee58ca92a6e8913aa84b6dc5af3534b878a44ca4e33173e962c6090ea243c1"
#define WANT_SIZE   3293064ull

/* ---- SHA-256 (FIPS 180-4) ---- */
typedef struct { uint32_t h[8]; uint64_t len; unsigned char buf[64]; unsigned fill; } sha256_ctx;

static const uint32_t sha_k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,
    0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,
    0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,
    0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,
    0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,
    0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(sha256_ctx *c, const unsigned char *p) {
    uint32_t w[64], a, b, d, e, f, g, h, cc;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + sha_k[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void sha_init(sha256_ctx *c) {
    static const uint32_t iv[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
    memcpy(c->h, iv, sizeof iv);
    c->len = 0;
    c->fill = 0;
}

static void sha_add(sha256_ctx *c, const void *data, size_t n) {
    const unsigned char *p = data;
    c->len += n;
    while (n) {
        if (!c->fill && n >= 64) { sha_block(c, p); p += 64; n -= 64; continue; }
        {
            size_t take = 64 - c->fill;
            if (take > n) take = n;
            memcpy(c->buf + c->fill, p, take);
            c->fill += (unsigned)take; p += take; n -= take;
            if (c->fill == 64) { sha_block(c, c->buf); c->fill = 0; }
        }
    }
}

static void sha_hex(sha256_ctx *c, char out[65]) {
    uint64_t bits = c->len * 8;
    unsigned char pad = 0x80, z = 0, l[8];
    int i;
    sha_add(c, &pad, 1);
    while (c->fill != 56) sha_add(c, &z, 1);
    for (i = 0; i < 8; i++) l[i] = (unsigned char)(bits >> (56 - i * 8));
    sha_add(c, l, 8);
    for (i = 0; i < 8; i++) sprintf(out + i * 8, "%08x", c->h[i]);
    out[64] = 0;
}

/* ---- paths and printing ---- */
/* the words for the player: stdout on the command line, a buffer for disc_check_run */
static char *say_buf;
static size_t say_cap, say_len;

static void say(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (say_buf) {
        int n = vsnprintf(say_buf + say_len, say_cap - say_len, fmt, ap);
        if (n > 0) say_len += (size_t)n < say_cap - say_len ? (size_t)n : say_cap - say_len - 1;
    } else vprintf(fmt, ap);
    va_end(ap);
}

static void print_w(const wchar_t *w) {
    char b[3 * 1024];
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, b, sizeof b, NULL, NULL);
    say("%s", n > 0 ? b : "?");
}

/* ---- a file on the disc, whichever way it is reached ---- */
typedef struct {
    FILE *iso;                  /* an ISO: the image, positioned by iso_start */
    wchar_t folder[1024];       /* else a folder or drive root */
    uint64_t iso_size;
} disc_src;

typedef struct { uint64_t offset, size; int found; } iso_entry;

static int iso_read(disc_src *d, uint64_t off, void *buf, size_t n) {
    if (_fseeki64(d->iso, (long long)off, SEEK_SET)) return 0;
    return fread(buf, 1, n, d->iso) == n;
}

static uint32_t le32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* a name in the root directory ("SYSTEM.CNF"), ISO9660: the ";1" version and case are ignored */
static iso_entry iso_find(disc_src *d, uint64_t dir_off, uint64_t dir_size, const char *want) {
    iso_entry r = { 0, 0, 0 };
    size_t wl = strlen(want);
    unsigned char *buf;
    uint64_t pos = 0;
    if (dir_size == 0 || dir_size > 16u * 1024 * 1024) return r;
    buf = malloc((size_t)dir_size);
    if (!buf) return r;
    if (!iso_read(d, dir_off, buf, (size_t)dir_size)) { free(buf); return r; }
    while (pos + 33 <= dir_size) {
        unsigned len = buf[pos];
        if (len == 0) {                                   /* the rest of this sector is empty */
            pos = (pos / 2048 + 1) * 2048;
            continue;
        }
        if (pos + len > dir_size || len < 33) break;
        {
            unsigned nl = buf[pos + 32];
            const char *nm = (const char *)buf + pos + 33;
            if (33 + nl <= len && nl >= wl && !_strnicmp(nm, want, wl) && (nl == wl || nm[wl] == ';')) {
                r.offset = (uint64_t)le32(buf + pos + 2) * 2048;
                r.size = le32(buf + pos + 10);
                r.found = 1;
                break;
            }
        }
        pos += len;
    }
    free(buf);
    return r;
}

/* reads up to cap bytes of a root file; returns 1 if found */
static int disc_open_file(disc_src *d, const char *name, iso_entry *root, FILE **out, uint64_t *size, uint64_t *off) {
    if (d->iso) {
        iso_entry e = iso_find(d, root->offset, root->size, name);
        if (!e.found) return 0;
        if (e.offset + e.size > d->iso_size) return -1;
        *out = d->iso; *size = e.size; *off = e.offset;
        return 1;
    } else {
        wchar_t p[1200], wn[64];
        FILE *f;
        MultiByteToWideChar(CP_UTF8, 0, name, -1, wn, 64);
        _snwprintf(p, 1200, L"%ls\\%ls", d->folder, wn);
        p[1199] = 0;
        f = _wfopen(p, L"rb");
        if (!f) return 0;
        _fseeki64(f, 0, SEEK_END);
        *size = (uint64_t)_ftelli64(f);
        _fseeki64(f, 0, SEEK_SET);
        *out = f; *off = 0;
        return 1;
    }
}

/* the serial from SYSTEM.CNF's "BOOT2 = cdrom0:\SLUS_213.46;1" */
static int parse_serial(const char *cnf, char *serial, size_t cap) {
    const char *b = strstr(cnf, "BOOT2");
    const char *e;
    size_t n;
    if (!b) return 0;
    while (*b && *b != '\\' && *b != ':' && *b != '\n') b++;
    if (*b == ':') b++;
    while (*b == '\\' || *b == '/') b++;
    e = b;
    while (*e && *e != ';' && *e != '\r' && *e != '\n' && *e != ' ') e++;
    n = (size_t)(e - b);
    if (n == 0 || n >= cap) return 0;
    memcpy(serial, b, n);
    serial[n] = 0;
    return 1;
}

static const char *region_of(const char *serial) {
    if (!_strnicmp(serial, "SLUS", 4) || !_strnicmp(serial, "SCUS", 4)) return "North American";
    if (!_strnicmp(serial, "SLES", 4) || !_strnicmp(serial, "SCES", 4)) return "European";
    if (!_strnicmp(serial, "SLPS", 4) || !_strnicmp(serial, "SCPS", 4) || !_strnicmp(serial, "SLPM", 4)) return "Japanese";
    if (!_strnicmp(serial, "SLKA", 4) || !_strnicmp(serial, "SCKA", 4)) return "Korean";
    if (!_strnicmp(serial, "SLAJ", 4) || !_strnicmp(serial, "SCAJ", 4)) return "Asian";
    return NULL;
}

enum { DC_OK = 0, DC_WRONG = 1, DC_NONE = 2 };

static int say_none(const wchar_t *what, const char *why, const char *token) {
    say("%s", "Nothing to check at ");
    print_w(what);
    say(": %s\nresult: none (%s)\n", why, token);
    return DC_NONE;
}

static int check_disc(disc_src *d, const wchar_t *shown, const wchar_t *extract_to) {
    iso_entry root = { 0, 0, 1 };
    char cnf[4096], serial[64], hex[65];
    FILE *f = NULL;
    uint64_t size = 0, off = 0, left;
    int rc;
    sha256_ctx sha;
    FILE *out = NULL;
    static unsigned char buf[1 << 16];

    if (d->iso) {
        unsigned char pvd[2048];
        _fseeki64(d->iso, 0, SEEK_END);
        d->iso_size = (uint64_t)_ftelli64(d->iso);
        if (d->iso_size < 17 * 2048 || !iso_read(d, 16 * 2048, pvd, 2048) || memcmp(pvd + 1, "CD001", 5) || pvd[0] != 1)
            return say_none(shown, "this file is not an ISO image with 2048-byte sectors (compressed images such as CHD or "
                                   "CSO and raw BIN images must be converted to a plain ISO first)", "not_iso");
        root.offset = (uint64_t)le32(pvd + 156 + 2) * 2048;
        root.size = le32(pvd + 156 + 10);
    }

    rc = disc_open_file(d, "SYSTEM.CNF", &root, &f, &size, &off);
    if (rc <= 0) {
        say("%s", "This is not a PlayStation 2 game: there is no SYSTEM.CNF on ");
        print_w(shown);
        say(". The game needs your own US disc of Ace Combat Zero: The Belkan War, serial %s.\nresult: wrong (not_ps2)\n", WANT_SERIAL);
        return DC_WRONG;
    }
    {
        size_t n = size < sizeof cnf - 1 ? (size_t)size : sizeof cnf - 1;
        if (_fseeki64(f, (long long)off, SEEK_SET) || fread(cnf, 1, n, f) != n) {
            if (!d->iso) fclose(f);
            return say_none(shown, "SYSTEM.CNF cannot be read (a damaged or incomplete copy?)", "read_error");
        }
        cnf[n] = 0;
        if (!d->iso) fclose(f);
    }
    if (!parse_serial(cnf, serial, sizeof serial)) {
        say("%s", "This is not a PlayStation 2 game disc: SYSTEM.CNF on ");
        print_w(shown);
        say(" names no boot file. The game needs your own US disc, serial %s.\nresult: wrong (no_boot)\n", WANT_SERIAL);
        return DC_WRONG;
    }
    if (_stricmp(serial, WANT_SERIAL)) {
        const char *reg = region_of(serial);
        say("This disc is %s%s%s%s, not the US disc of Ace Combat Zero (%s). The recompiler is tied to that one executable, "
               "so another region of the game or another game cannot work.\nresult: wrong (serial %s)\n",
               serial, reg ? " (" : "", reg ? reg : "", reg ? " disc)" : "", WANT_SERIAL, serial);
        return DC_WRONG;
    }

    rc = disc_open_file(d, WANT_SERIAL, &root, &f, &size, &off);
    if (rc <= 0) {
        say("SYSTEM.CNF names %s but that file is not there (or lies beyond the end of the image: an incomplete copy?).\n"
               "result: wrong (no_executable)\n", WANT_SERIAL);
        return DC_WRONG;
    }
    if (_fseeki64(f, (long long)off, SEEK_SET)) {
        if (!d->iso) fclose(f);
        return say_none(shown, "the executable cannot be read", "read_error");
    }
    if (extract_to) {
        out = _wfopen(extract_to, L"wb");
        if (!out) {
            say("%s", "Cannot write ");
            print_w(extract_to);
            say(".\nresult: none (write_error)\n");
            if (!d->iso) fclose(f);
            return DC_NONE;
        }
    }
    sha_init(&sha);
    left = size;
    while (left) {
        size_t n = left < sizeof buf ? (size_t)left : sizeof buf;
        if (fread(buf, 1, n, f) != n) {
            if (!d->iso) fclose(f);
            if (out) { fclose(out); _wremove(extract_to); }
            return say_none(shown, "reading the executable failed (a scratched disc or an incomplete copy?)", "read_error");
        }
        sha_add(&sha, buf, n);
        if (out) fwrite(buf, 1, n, out);
        left -= n;
    }
    if (!d->iso) fclose(f);
    if (out) fclose(out);
    sha_hex(&sha, hex);
    if (strcmp(hex, WANT_SHA256)) {
        say("This disc says %s, but its executable is not the one the recompiler was made for: %llu bytes, SHA-256 %s "
               "(expected %llu bytes, %s). It may be another printing of the disc, or a modified or damaged copy.\n"
               "result: wrong (hash %s)\n", WANT_SERIAL, (unsigned long long)size, hex, WANT_SIZE, WANT_SHA256, hex);
        if (out) _wremove(extract_to);
        return DC_WRONG;
    }
    say("%s", "OK: ");
    print_w(shown);
    say(" is the US disc of Ace Combat Zero, %s, executable SHA-256 %s.\nresult: ok\n", WANT_SERIAL, hex);
    return DC_OK;
}

static int check_path(const wchar_t *path, const wchar_t *extract_to) {
    DWORD a = GetFileAttributesW(path);
    disc_src d;
    wchar_t p[1024];
    int rc;
    size_t l;
    memset(&d, 0, sizeof d);
    wcsncpy(p, path, 1023);
    p[1023] = 0;
    l = wcslen(p);
    /* "D:" means the root of D:, not the current folder on D: */
    if (l == 2 && p[1] == L':') { p[2] = L'\\'; p[3] = 0; l = 3; a = GetFileAttributesW(p); }
    if (a == INVALID_FILE_ATTRIBUTES) {
        DWORD e = GetLastError();
        if (e == ERROR_NOT_READY) return say_none(p, "the drive has no disc in it", "no_disc");
        return say_none(p, "the path does not exist or cannot be read", "bad_path");
    }
    if (a & FILE_ATTRIBUTE_DIRECTORY) {
        while (l > 1 && (p[l - 1] == L'\\' || p[l - 1] == L'/') && !(l == 3 && p[1] == L':')) p[--l] = 0;
        if (l == 3 && p[1] == L':') p[2] = 0;               /* "D:" + "\NAME" below */
        wcscpy(d.folder, p);
        return check_disc(&d, path, extract_to);
    }
    d.iso = _wfopen(p, L"rb");
    if (!d.iso) return say_none(p, "the file cannot be opened", "bad_path");
    rc = check_disc(&d, path, extract_to);
    fclose(d.iso);
    return rc;
}

/* no argument: every disc drive; the first accepted disc wins */
static int check_drives(const wchar_t *extract_to) {
    DWORD mask = GetLogicalDrives();
    int drives = 0, best = DC_NONE, i;
    UINT old = SetErrorMode(SEM_FAILCRITICALERRORS);       /* no "insert a disc" box from Windows */
    for (i = 0; i < 26; i++) {
        wchar_t root[4] = { (wchar_t)(L'A' + i), L':', L'\\', 0 };
        wchar_t label[64];
        if (!(mask & (1u << i)) || GetDriveTypeW(root) != DRIVE_CDROM) continue;
        drives++;
        say("%s", "Disc drive ");
        print_w(root);
        say("%s", ": ");
        if (!GetVolumeInformationW(root, label, 64, NULL, NULL, NULL, NULL, 0)) {
            say("no disc, or a disc Windows cannot read.\n");
            continue;
        }
        say("%s", "disc \"");
        print_w(label);
        say("%s", "\".\n");
        {
            int rc = check_path(root, extract_to);
            if (rc == DC_OK) { SetErrorMode(old); return DC_OK; }
            if (rc == DC_WRONG) best = DC_WRONG;
        }
    }
    SetErrorMode(old);
    if (!drives) {
        say("This PC has no disc drive that Windows can see. Give the path of an ISO image made from your own US disc instead: "
               "disc_check path\\to\\game.iso\nresult: none (no_drive)\n");
        return DC_NONE;
    }
    if (best == DC_NONE) say("No disc was found in the %d drive(s).\nresult: none (no_disc)\n", drives);
    return best;
}

/* In-process, for ac0vr.exe (tools/setup/ac0vr_launcher.c, built with DISC_CHECK_NO_MAIN): path NULL = every disc drive. The same words
   go to msg, the last line `result: ...`; the return value is the exit status above. */
int disc_check_run(const wchar_t *path, const wchar_t *extract_to, char *msg, size_t cap) {
    int rc;
    say_buf = msg; say_cap = cap; say_len = 0;
    msg[0] = 0;
    rc = path ? check_path(path, extract_to) : check_drives(extract_to);
    say_buf = NULL;
    return rc;
}

#ifndef DISC_CHECK_NO_MAIN
int main(void) {
    int argc = 0, i, rc;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const wchar_t *extract_to = NULL, *path = NULL;
    SetConsoleOutputCP(CP_UTF8);
    for (i = 1; argv && i < argc; i++) {
        if (!wcscmp(argv[i], L"--extract") && i + 1 < argc) extract_to = argv[++i];
        else if (!path && argv[i][0] != L'-') path = argv[i];
        else {
            printf("usage: disc_check [--extract OUT] [ISO | folder | drive]\nresult: none (usage)\n");
            return DC_NONE;
        }
    }
    rc = path ? check_path(path, extract_to) : check_drives(extract_to);
    fflush(stdout);
    return rc;
}
#endif
