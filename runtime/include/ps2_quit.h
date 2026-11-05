/* ps2_quit.h -- leaving the game cleanly from the headset (runtime/src/ps2_quit.c). */
#ifndef PS2_QUIT_H
#define PS2_QUIT_H

#include "ps2_runtime.h"
#include "ps2_hle.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What the game is doing, as the game hooks report it (zero_vr.c): the pad combo is refused while flying. */
enum { PS2_QUIT_GAME_MENU = 0,      /* title, menus, briefing, movies, loading: the combo works */
       PS2_QUIT_GAME_FLYING = 1,    /* a mission is being flown: the combo only shows how to quit */
       PS2_QUIT_GAME_PAUSED = 2 };  /* the pause menu is up: the combo works */

/* Game hooks: the current state, and the way back to the game's main menu (do_it 0 = only ask whether it is
   possible now, 1 = start it; returns 1 when possible / started). Call these setters once at start-up. */
void ps2_quit_set_game_state_fn(int (*fn)(void));
void ps2_quit_set_title_fn(int (*fn)(int do_it));

/* 1 while the combo would open the prompt (combo on, the game not flying, no prompt or quit under way): the menus
   between missions then show a hint for it (zero_menus.c). *playstation = the pad shows PlayStation glyphs. */
int  ps2_quit_hint_wanted(int *playstation);

/* Start the clean quit to desktop (any thread): waits for the memory card to be idle, then closes the game the way
   Esc twice does; the process then exits with 0 (ps2_quit_exit). why goes into the log. */
void ps2_quit_request(const char *why);
int  ps2_quit_requested(void);

/* Renderer thread, once per input poll: the View + Menu combo and its prompt. back/start/a/b/x/y are the raw
   controller buttons (SDL Back, Start, South, East, West, North; scripted test pads included), glyphs = the button
   glyphs shown (ps2_texrep.h PS2_GLYPHS_*: 0 PlayStation, 1 Xbox, 2 Quest Touch; the active controller's). May clear
   buttons in *st (Start while Back is held); returns 1 while the prompt takes the controller (the game must then see
   a neutral pad). Also advances a requested quit (card wait, then the close). */
int  ps2_quit_pad(ps2_pad_state *st, int back, int start, int a, int b, int x, int y, int glyphs);
/* Renderer thread, every loop: advances a requested quit even while the pad is not polled (the F4 menu is open). */
void ps2_quit_tick(void);

/* The prompt or the ENHANCEMENTS page as an RGBA image (premultiplied alpha, sRGB colours; always the same size) for
   the headset: returns 0 when nothing is shown. *gen changes whenever the image does. Renderer thread. */
int  ps2_quit_overlay_image(const u8 **rgba, u32 *w, u32 *h, u32 *gen);
/* the width in metres of the headset quad that shows the current overlay image 1.1 m ahead (ps2_xr.c scales it
   with the distance when the ENHANCEMENTS page sits on the cinema screen) */
float ps2_quit_overlay_width_m(void);
/* The same prompt as text lines for the desktop window (NULL when nothing is shown): the first line is the title. */
const char *ps2_quit_overlay_text(void);

/* ---- the ENHANCEMENTS page (bug #64): every improvement as a setting (ps2_settings.h ps2_enh_*), in the game's font
   and colours, on the headset quad (and as text on the window without a headset). Opened from the pause menu's
   ENHANCEMENTS row (zero_vr.c) or the View + Menu panel ({X} outside the pause menu); the D-pad or the left stick
   moves, A changes / opens, B or Y goes back; while it is up the game sees a neutral pad. ---- */
enum { PS2_ENH_FROM_PAUSE = 0, PS2_ENH_FROM_MENUS = 1 };
void ps2_quit_enh_open(int from);          /* any thread */
int  ps2_quit_enh_visible(void);           /* any thread: the page is up (or opening) */
/* the page as text for the desktop window (NULL when it is closed); *sel = the line of the cursor (-1 none) */
const char *ps2_quit_enh_text(int *sel);
/* ACZ_ENH_NATIVE (R8, night_menu 2026-11-04): the page opened from the pause menu is drawn by the game itself, in its
   FLIGHT OPTIONS layout (zero_vr.c zn_*). The logic stays here on the renderer thread (pg_pad: the levels, the cursor,
   scrolling, left / right / A, the settings; the game sees a neutral pad as ever); zero_vr.c reads the page's state on
   the EE thread through this snapshot and writes it into the pause menu's tree and text entries. No overlay image is
   made meanwhile (the TOUCH CONTROLS card stays one). */
#define PS2_ENH_SNAP_ROWS 7
#define PS2_ENH_SNAP_VALS 6
enum { PS2_ENH_SNAP_LIST = 1, PS2_ENH_SNAP_ITEMS, PS2_ENH_SNAP_VALUES };
typedef struct ps2_enh_snap {
    u32 gen;                               /* changes when anything below does; 0 = nothing published yet */
    int kind;                              /* LIST: rows without values (the categories, the restart notice) */
    int cat;                               /* the category of the items (for the value column), else -1 */
    int nrows, cursor;                     /* rows shown, the cursor's slot among them */
    int open, nvals, val;                  /* VALUES: the slot of the row whose list is open, its length, the cursor in it */
    char crumb[48], head[40];              /* the breadcrumb after PAUSE MENU>, the heading under the rule */
    char label[PS2_ENH_SNAP_ROWS][40];
    char value[PS2_ENH_SNAP_ROWS][32];
    char list[PS2_ENH_SNAP_VALS][32];
    char help[400];
} ps2_enh_snap;
void ps2_quit_enh_open_native(void);       /* any thread: as ps2_quit_enh_open(PS2_ENH_FROM_PAUSE), drawn by the game */
int  ps2_quit_enh_snap(ps2_enh_snap *out, u32 have_gen);   /* any thread: 1 when copied (its gen differs from have_gen) */

/* ---- the TOUCH CONTROLS card (bug #239): both Quest Touch controllers drawn with what each control does, for the
   Touch mode in use (HANDS ON STICK or AS A PAD; left / right shows the other), in the ENHANCEMENTS page's look on
   the same headset quad. Opened from the pause menu's TOUCH CONTROLS row (zero_vr.c, with Touch on), ENHANCEMENTS >
   CONTROLS > SHOW TOUCH CONTROLS, a 1 s hold of Touch's Menu button in flight (ps2_touch.c) and by itself once, the
   first time HANDS ON STICK is used (ACZ_TOUCH_CARD). From flight the game is paused first (a Start press) and
   resumed when the card closes. B, Y, A or Menu closes it; while it is up the game sees a neutral pad. ---- */
enum { PS2_TC_FROM_PAUSE = 0, PS2_TC_FROM_MENUS = 1, PS2_TC_FROM_ENH = 2, PS2_TC_FROM_FLIGHT = 3, PS2_TC_FROM_AUTO = 4 };
void ps2_quit_touch_card_open(int from);   /* any thread */
int  ps2_quit_game_state(void);            /* PS2_QUIT_GAME_* as the game hooks report it now (zero_vr.c) */
/* zero_menus.c: a hook on the pad before the game reads it, while no prompt or page takes the controller (the
   OPTIONS screen's ENHANCEMENTS row, bug #237); renderer thread */
void ps2_quit_set_menu_pad_fn(int (*fn)(ps2_pad_state *st));
/* zero_menus.c, every movie of the menus between missions drawn (not the card check or the logos); the Touch side asks
   whether a menu is on screen now (the TOUCH CONTROLS card by itself: not over a loading screen) */
void ps2_quit_note_menu(void);
int  ps2_quit_menu_on_screen(void);
void ps2_quit_touch_hud_line(const char *line, double secs);   /* one line in the game's font in the flight view (bug #314) */
void ps2_quit_touch_hud_line_clear(void);
int  ps2_quit_touch_card_visible(void);    /* any thread: the card is up (or opening) */
/* a short hint panel (the quit prompt's, also in flight) for secs seconds: Touch's "take the stick" hint */
void ps2_quit_touch_hint(const char *title, const char *line, double secs);

/* The last step of every exit: flush stdio and end the process with code. After an OpenXR runtime was loaded this
   skips the DLL detach handlers (TerminateProcess): the Meta runtime's detach handler waited 60 s for a worker
   thread it can no longer start ("std::thread construction FAILED"); the session is already ended by then. */
void ps2_quit_exit(int code);

/* Memory card (ps2_memcard.c): 1 while a card file is open for writing or the card was changed in the last 1.5 s
   (a save writes several files one after another). */
int  ps2_mc_busy(void);
/* Write any unwritten card change to disk now (only when no card file is open). */
void ps2_mc_flush(void);

#ifdef __cplusplus
}
#endif

#endif
