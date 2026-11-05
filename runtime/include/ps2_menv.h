#ifndef PS2_MENV_H
#define PS2_MENV_H
/* The NEXT MISSION tier, the readers' side (settings review package B, bug #318, docs/SETTINGS.md).
   A row of the tier is saved at once on the page; nothing else happens until a mission is entered: at GameControl::enter
   (zero_vr.c zero_env_reload_mission, the game thread) ps2_enh_mission_edge publishes the rows changed since and bumps
   ps2_menv_gen, once; with nothing changed it does nothing at all.
   The environment is NOT written after the start-up constructor: UCRT's _putenv_s frees the string an earlier getenv
   returned, and these switches are read on the game, graphics, renderer, audio and loader threads, some of them lazily (a
   first explosion) or periodically (zero_syssnd.c, every 60 fields). ps2_menv(name) answers what getenv answers until the
   row is published, then the row's value: a string literal of the table, or NULL = the switch's built-in default. So no
   string can go away under a reader, and no lazy reader can see a value before the edge: nothing changes in flight.
   A reader of such a switch: ps2_menv instead of getenv, and its static parsed again when the serial moved on
   (ps2_menv_take). ps2_menv_boot: the value this run started with, for what is built once at start or at a cockpit's load
   (a pipeline, a hook, a bake); such a row's "on" is refused for the next mission when it started off (ps2_enh_apply_now).
   tools/test_settings.py checks that no getenv of a NEXT MISSION switch is left in runtime/src. */
#include <stdlib.h>
#ifdef __cplusplus
extern "C" {
#endif
extern volatile unsigned ps2_menv_gen;             /* bumps once per mission edge with changed rows; 0 until then */
extern volatile unsigned long long ps2_menv_field; /* the field of the last mission edge (zero_vr.c) */
const char *ps2_menv(const char *name);
static inline const char *ps2_menv_boot(const char *name) { return getenv(name); }
/* 1 when gen moved on since *seen (each reader keeps its own seen) */
static inline int ps2_menv_take(unsigned *seen, unsigned gen) { if (*seen == gen) return 0; *seen = gen; return 1; }
/* the log's proof of a re-read: `mission: NAME old -> new` (only when the parsed value changed) */
void ps2_menv_note(const char *name, double old_value, double new_value);
#ifdef __cplusplus
}
#endif
#endif
