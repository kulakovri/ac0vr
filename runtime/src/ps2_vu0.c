/* ps2_vu0.c -- the out-of-line part of ps2_vu0_advance (ps2_vu0.h): the VU0 flag pipeline's slot loop, entered
   only when a VU0 macro instruction left flags pending. This file compiles the header's full inline version once
   (PS2_VU0_ADV_INLINE=1 here), so the generated code's ~210,000 call sites share one copy (the CMake option
   PS2_VU0_ADV_INLINE=ON inlines it at every site again). */
#undef PS2_VU0_ADV_INLINE
#define PS2_VU0_ADV_INLINE 1
#include "ps2_runtime.h"

void ps2_vu0_advance_slow(ps2_ctx *ctx, u32 slots) {
    ps2_vu0_advance(ctx, slots);
}
