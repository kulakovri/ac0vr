/* ps2_stats.h -- opt-in usage statistics (ps2_stats.c; docs/PRIVACY.md). */
#ifndef PS2_STATS_H
#define PS2_STATS_H
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* the [privacy] section of the settings file (ps2_settings.c): stats, stats_asked, install_id; absent keys mean "ask" */
void ps2_stats_ini_reset(void);
void ps2_stats_ini_kv(const char *key, const char *val);
void ps2_stats_ini_write(FILE *f);

void ps2_stats_init(void);                 /* main, after the settings file */
void ps2_stats_tick(void);                 /* renderer thread, every loop */
void ps2_stats_exit(void);                 /* the end of the process: the session's document, at most 2 s */

int  ps2_stats_available(void);            /* this build (or ACZ_STATS_KEY) has a project key */
const char *ps2_stats_env_value(void);     /* "0" or "1" while ACZ_STATS holds the setting, else NULL */
int  ps2_stats_saved_on(void);             /* the saved answer, 1 = yes */
void ps2_stats_set(int on);                /* the menu: saved by the caller, live */
void ps2_stats_reset_id(void);             /* RESET INSTALL ID: saved by the caller */
void ps2_stats_log_document(void);         /* F4's SHOW WHAT IS SENT: the document into the log */

/* the question (ps2_quit.c): due once at the title of a build with a key, until answered */
void ps2_stats_set_title_fn(int (*fn)(void));   /* zero_vr.c: the title screen is up */
int  ps2_stats_title_up(void);
int  ps2_stats_question_due(void);
void ps2_stats_answer(int yes);            /* saves */

/* SHOW WHAT IS SENT: the next document, the very fields that are sent, a field per row */
typedef struct { char k[32]; char v[32]; } ps2_stats_row;
int  ps2_stats_rows(ps2_stats_row *out, int cap);

/* what the document reports; any of these may never be called */
void ps2_stats_set_gpu(const char *name, const char *driver, double vram_gb);   /* ps2_video.c */
void ps2_stats_set_xr(const char *runtime, const char *system);                 /* ps2_xr.c */
void ps2_stats_set_refresh(double hz);                                          /* ps2_xr.c */
void ps2_stats_note_gpu(double ms, int flying, int gov_level);                  /* ps2_video.c, the governor's frame time */
void ps2_stats_note_ee(double ms);                                              /* ps2_kernel.c, the EE's work in a field */
extern volatile int ps2_stats_collecting;                                       /* (a cheap test before the notes) */
/* zero_vr.c, EE thread: a mission begins (also a retry); the last one ended (a NULL or weaker result never overrides "completed") */
void ps2_stats_mission_begin(int mission, int aircraft);
void ps2_stats_mission_end(const char *result);       /* "completed", "failed", "left" */

#ifdef __cplusplus
}
#endif
#endif
