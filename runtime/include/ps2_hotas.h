// ps2_hotas.h - joysticks, throttles and pedals as the pilot's controls (ctl_hotas, 2026-11-04;
// RELEASE_RESEARCH_2026-11-03.md section 10, docs/HOTAS.md). Everything here runs on the renderer thread, except
// ps2_hotas_ini_reset / _kv (the settings file's first load, before the renderer starts). See ps2_hotas.c.
#ifndef PS2_HOTAS_H
#define PS2_HOTAS_H

#include <SDL3/SDL.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ps2_pad_state ps2_pad_state;

/* A joystick binding as an int, held per device in its profile (so no slot in it; the gamepad's codes of ps2_settings.h stay as
   they are: 1 + button, 0x100 | axis << 1 | dir). The settings file keeps the text form in [joystick:<GUID>] ("axis2+", "button5",
   "hat0:up"); a device's profile is found by its GUID when it connects, never by slot or path. */
#define PS2_JOYBIND_AXIS   0x200                     /* | axis << 1 | dir (axes 0..63; dir 1 = positive) */
#define PS2_JOYBIND_BUTTON 0x400                     /* | button (0..255) */
#define PS2_JOYBIND_HAT    0x800                     /* | hat << 2 | direction (0 up, 1 right, 2 down, 3 left) */

/* ps2_video.c: renderer_main, pad_init, pump_events, poll_input, apply_rumble, the renderer's exit */
void ps2_hotas_pre_init(void);                       /* before SDL_Init: the device-type hints from the shipped list */
void ps2_hotas_menv_reload(void);                    /* the NEXT MISSION tier (ps2_menv.h): the renderer, after the edge */
void ps2_hotas_init(void);                           /* after SDL_Init: the test devices, every joystick SDL lists */
void ps2_hotas_shutdown(void);
void ps2_hotas_event(const SDL_Event *e);            /* hot-plug, the capture, the sweep report */
int  ps2_hotas_claims(SDL_JoystickID id);            /* a flight stick or throttle SDL also offers as a gamepad: ours */
void ps2_hotas_update(void);                         /* poll_input, every poll */
void ps2_hotas_merge(float *value);                  /* poll_input: value[PS2_ACT_COUNT] by max, as Touch merges */
void ps2_hotas_debug(const ps2_pad_state *st);       /* poll_input, the end: ACZ_DEBUG_HOTAS */
void ps2_hotas_ffb(float lo, float hi);              /* apply_rumble: ACZ_FFB */
int  ps2_hotas_count(void);                          /* devices connected */
int  ps2_hotas_gamepad_button(int sdl_button);       /* pad_button_any: a bound button acts as this gamepad button */

/* ps2_settings.c: [joystick:<GUID>] sections and the ENHANCEMENTS rows */
void ps2_hotas_ini_reset(void);
void ps2_hotas_ini_kv(const char *id, const char *key, const char *val);
void ps2_hotas_ini_write(FILE *f);
void ps2_hotas_set_throttle(const char *mode);       /* "" centred, "spring", "split", "absolute" */
void ps2_hotas_neutral_now(void);                    /* SET LEVER NEUTRAL */
const char *ps2_hotas_neutral_note(void);            /* its answer (the page's help line) */

/* ps2_ui.cpp: F4 > Controller > Joysticks */
typedef struct ps2_hotas_info {
    const char *name, *guid, *type, *profile, *status;
    int vid, pid, ver, crc16, naxes, nbuttons, nhats, slot;
} ps2_hotas_info;
int   ps2_hotas_info_at(int i, ps2_hotas_info *out);
float ps2_hotas_axis_now(int i, int axis);           /* raw -1..1 */
int   ps2_hotas_button_now(int i, int button);
int   ps2_hotas_hat_now(int i, int hat);
void  ps2_hotas_axis_get(int i, int axis, int *invert, float *dz, float *curve, int *slider, float *neutral);
void  ps2_hotas_axis_set(int i, int axis, int invert, float dz, float curve, int slider, float neutral);
int   ps2_hotas_rows(void);                          /* the binding table's rows */
const char *ps2_hotas_row_label(int row);
const char *ps2_hotas_row_group(int row);            /* non-NULL: a heading above this row */
const char *ps2_hotas_binding_name(int row, int slot);   /* "T.16000M: axis 2 +", or "-" */
void  ps2_hotas_unbind(int row, int slot);
void  ps2_hotas_capture_begin(int row, int slot);    /* the next full axis travel, button or hat is bound */
int   ps2_hotas_capture_active(void);
int   ps2_hotas_capture_armed(void);
const char *ps2_hotas_capture_prompt(void);          /* "Pull the stick fully back." */
const char *ps2_hotas_capture_label(void);
void  ps2_hotas_capture_cancel(void);
void  ps2_hotas_capture_clear(void);
int   ps2_hotas_report(char *out, int n);            /* the volunteers' text: the device lines and the profiles */

/* ps2_ui.cpp, for PS2_PAD_JOY's "f4" entry: the window's capture popup at this row (a window-only capture's evidence) */
void ps2_ui_hotas_test(int row, int slot);

#ifdef __cplusplus
}
#endif

#endif
