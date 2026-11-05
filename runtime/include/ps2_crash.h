/* ps2_crash.h -- the crash report (ACZ_CRASH_REPORT, default 1; night_setup 2026-11-04; ps2_crash.c).
   On a crash: a minidump and a text report with the last 200 log lines in crash/ next to the exe (%TEMP%\ac5_crash when
   that folder cannot be written). Nothing is written or sent unless something fails. */
#ifndef PS2_CRASH_H
#define PS2_CRASH_H

#ifdef __cplusplus
extern "C" {
#endif

/* start of main(): reads the switch, logs a report left by the last run, starts the report thread. filter = the process's
   unhandled-exception filter (a LPTOP_LEVEL_EXCEPTION_FILTER), kept installed if a library replaces it later. */
void ps2_crash_init(void *filter);
int ps2_crash_enabled(void);

/* ps2_log: keep this line for a report (no terminating newline in len). */
void ps2_crash_log_line(const char *line, int len);

/* The exception filter: remember where the log stood before its own lines (the report shows 200 lines before this). */
void ps2_crash_mark(void);

/* The exception filter: a report with a minidump, written on the report thread (so a stack overflow works too); waits for it,
   at most 20 s. why = NULL: described from exception_pointers (an EXCEPTION_POINTERS *, or NULL); thread_id = the faulting
   thread. */
void ps2_crash_report(const char *why, void *exception_pointers, unsigned long thread_id);

/* A report without a minidump, on the caller's thread: `why` and the last 200 log lines. For fatal conditions that are not
   exceptions (the stall exit, a lost GPU device, failing XR calls: stab_audit U5, U8). The caller goes on or exits as it
   likes; safe to call more than once. */
void ps2_fatal_report(const char *why);

#ifdef __cplusplus
}
#endif
#endif
