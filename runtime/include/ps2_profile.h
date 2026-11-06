/* ps2_profile.h -- how the exe starts when no script started it, and the version string (setup_1105; ps2_profile.c, ps2_version.c,
   docs/SETTINGS.md "Paths"). */
#ifndef PS2_PROFILE_H
#define PS2_PROFILE_H

#ifdef __cplusplus
extern "C" {
#endif

/* "0.1.0 (ec0b1df)": version.txt beside the exe when the player's build put one there, else what CMake wrote into ps2_version.h */
const char *ps2_version_line(void);

/* 1: a player's start (no --data, no test option): the switch table, the folder beside the exe, the exe's own log. */
int ps2_profile_player(void);
/* the --data default: "gen_zero" for a player's start (the current folder is then the exe's), "." as ever otherwise */
const char *ps2_profile_data_dir(void);
/* main()'s argv as UTF-8 (the C library's is in the code page: a path outside it would be lost) */
void ps2_profile_args(int *argc, char ***argv);

/* [paths] disc = of the settings file, for a player's start without --disc: NULL when it is not set or the file is gone */
const char *ps2_profile_disc(void);
/* 1: a player's start with no --disc and no usable [paths] disc =: the launcher asks */
int ps2_profile_disc_wanted(void);
/* the launcher's choice: 0 accepted and saved; else the words to show (not a disc, or the file could not be written) */
const char *ps2_profile_disc_set(const char *path);

#ifdef __cplusplus
}
#endif

#endif
