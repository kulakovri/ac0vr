/* ps2_path.h -- one rule for file names (setup_1105, docs/SETTINGS.md "Paths"): every path in the runtime is UTF-8, whatever the
   Windows code page is. SDL_GetBasePath, SDL's file functions and the OpenXR and Vulkan loaders take UTF-8 or wide names by
   themselves; the C library's fopen does not, so a player's folder outside the code page (a Cyrillic user name on a Western
   Windows, tested in `тест ÄÖ`) lost the settings, the shaders and the pipeline cache. ps2_path.c goes through _wfopen and friends.

   CMake forces this header into the runtime's C files (`-include`), so the sites keep reading fopen(), remove(), opendir(), and a
   new site cannot forget. The C++ files and ps2_vu.c (compiled on the player's PC, without the flag; its dumps go to relative
   paths) do not get the macros: a relative ASCII name needs no conversion, since the current folder is the system's business. */
#ifndef PS2_PATH_H
#define PS2_PATH_H

#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <wchar.h>
#include <sys/stat.h>
#include <direct.h>
#include <dirent.h>

#ifdef __cplusplus
extern "C" {
#endif

/* UTF-8 to wide; bytes that are not UTF-8 are taken as the code page's (an environment value from a tool that writes those).
   0: it does not fit. */
int ps2_path_wide(const char *path, wchar_t *out, int cap);

FILE *ps2_fopen(const char *path, const char *mode);
int   ps2_remove(const char *path);
int   ps2_rename(const char *from, const char *to);
int   ps2_mkdir(const char *path);
int   ps2_stat64(const char *path, struct _stat64 *st);

struct ps2_dirent { char d_name[780]; };        /* (260 UTF-16 units at most, 3 bytes each) */
typedef struct ps2_dir ps2_dir;
ps2_dir *ps2_opendir(const char *path);
struct ps2_dirent *ps2_readdir(ps2_dir *d);
int ps2_closedir(ps2_dir *d);

#ifdef __cplusplus
}
#endif

#ifndef PS2_PATH_NO_MACROS
#define fopen(p, m)     ps2_fopen(p, m)
#define remove(p)       ps2_remove(p)
#define rename(a, b)    ps2_rename(a, b)
#define _mkdir(p)       ps2_mkdir(p)
#define _stat64(p, s)   ps2_stat64(p, s)
#define DIR             ps2_dir
#define dirent          ps2_dirent
#define opendir(p)      ps2_opendir(p)
#define readdir(d)      ps2_readdir(d)
#define closedir(d)     ps2_closedir(d)
#endif

#endif /* _WIN32 */
#endif
