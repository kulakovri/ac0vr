/* ps2_version.c -- the version in the log's first line, `ac5.exe --version`, the launcher's corner and the crash report (setup_1105).
   ps2_version.h is written into the build folder by tools/setup/write_version.cmake: from git, or from -DACZ_VERSION=. It is included
   here only, so a new commit rebuilds this one file. The player's build has no git: tools/setup/player_build.py copies the installed
   runtime's version.txt beside the exe it links, and the old exe keeps its own copy until a link succeeds. */
#include "ps2_profile.h"
#include "ps2_version.h"

#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

static char line[96];

const char *ps2_version_line(void) {
    if (line[0]) return line;
#ifdef _WIN32
    {
        wchar_t p[MAX_PATH + 16];
        DWORD n = GetModuleFileNameW(NULL, p, MAX_PATH);
        wchar_t *sl = n && n < MAX_PATH ? wcsrchr(p, L'\\') : NULL;
        if (sl) {
            FILE *f;
            wcscpy(sl + 1, L"version.txt");
            f = _wfopen(p, L"r");
            if (f) {
                if (fgets(line, sizeof line, f)) line[strcspn(line, "\r\n")] = 0;
                fclose(f);
                if (line[0]) return line;
            }
        }
    }
#endif
    if (PS2_VERSION_GIT[0]) snprintf(line, sizeof line, "%s (%s)", PS2_VERSION, PS2_VERSION_GIT);
    else snprintf(line, sizeof line, "%s", PS2_VERSION);
    return line;
}
