#ifndef PS2_TEXREP_H
#define PS2_TEXREP_H

/* Texture replacement by content hash (ps2_texrep.c): PNG files in textures/<set>/ named by the GS decode's hash,
   PSM and size stand in for the game's textures, at the same or an integer multiple of the size. */

#include "ps2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { PS2_TEXREP_REPLACE = 1, PS2_TEXREP_NOTE = 2 };

/* GS thread. Once: index the packs; returns PS2_TEXREP_* bits (0 = nothing to do on any decode). */
int  ps2_texrep_init(void);
/* 1 when the active sets changed since the last call (pad type / layout): cached decodes must be dropped */
int  ps2_texrep_changed(void);
/* the replacement for a decode, or NULL; *iw x *ih is its size (an integer multiple of w x h), *salt tells the
   texture store the replacement apart from the decode it stands for */
const u8 *ps2_texrep_find(u32 hash, u32 psm, u32 w, u32 h, u32 *iw, u32 *ih, u32 *salt);
/* debug: hash log (PS2_TEX_HASHLOG) and PNG dump (PS2_TEX_DUMP) of each new decode */
void ps2_texrep_note(u32 hash, u32 psm, u32 w, u32 h, u32 tbp, u32 tbw, u32 cbp, const u8 *rgba, u32 idx,
                     int replaced);

/* button-glyph classes: the active controller's (ps2_video.c's registry), or ACZ_PAD_GLYPHS */
enum { PS2_GLYPHS_PS = 0, PS2_GLYPHS_XBOX = 1, PS2_GLYPHS_TOUCH = 2 };
/* renderer thread, once per frame: the active controller's class decides the button-glyph set; who = its name for
   the log (NULL: no controller) */
void ps2_texrep_pad(int glyph_class, const char *who);
/* PS2_GLYPHS_*: the set shown now (the quit prompt and the ENHANCEMENTS page draw the same kind) */
int  ps2_texrep_glyph_class(void);

#ifdef __cplusplus
}
#endif

#endif
