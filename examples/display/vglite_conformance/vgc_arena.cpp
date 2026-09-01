/* vgc_arena.cpp - the conformance probe's shared path arena.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Spec: docs/superpowers/specs/2026-08-30-gc355-conformance-design.md
 *
 * ★ ITS OWN TU SO IT CAN BE UNIT-TESTED ON THE HOST (tests/arena_test.cpp).
 * The arena is the highest-risk piece of the harness -- three interacting
 * pieces of state (s_used, s_start, s_overflow), an overflow path that must
 * REFUSE rather than truncate, and a caller contract about what *p holds when
 * it does. Every other risky piece of this example got a test; this one is
 * separated out so it can have one. It has zero coupling to VGLite
 * initialisation or the run loop: it depends only on VLC_OP_END and
 * vg_lite_init_path, both of which the host test stubs.
 *
 * What makes that worth the split rather than the alternative: a truncated
 * path has no VLC_OP_END, and unterminated path data hangs the Vivante front
 * end while every call returns VG_LITE_SUCCESS. An arena bug therefore does
 * not present as an arena bug -- it presents as a dead board mid-matrix. */
#include <string.h>
#include "vgc_harness.h"

static int32_t s_arena[VGC_ARENA_WORDS];
static size_t  s_used;
static size_t  s_start;
static bool    s_overflow;

void vgc_arena_reset(void) { s_used = 0; s_start = 0; s_overflow = false; }

void vgc_emit(int32_t w)
{
    if (s_used < VGC_ARENA_WORDS) s_arena[s_used++] = w;
    else s_overflow = true;
}

vg_lite_error_t vgc_finish_path(vg_lite_path_t *p,
                                float x0, float y0, float x1, float y1)
{
    vgc_emit(VLC_OP_END);
    /* ★ ZERO *p BEFORE the overflow return, not after. Callers declare
     * vg_lite_path_t on the stack; a caller that dropped the status and drew
     * anyway would otherwise submit a path pointer and length made of stack
     * garbage -- the unterminated-path-data condition that hangs the front end
     * while every call returns SUCCESS. Costs nothing: vg_lite_init_path()
     * memsets the path itself (vg_lite_path.c:188), so on the success path
     * this is redundant work the compiler is free to notice. */
    memset(p, 0, sizeof(*p));
    if (s_overflow) { s_overflow = false; s_start = s_used; return VG_LITE_OUT_OF_RESOURCES; }
    vg_lite_init_path(p, VG_LITE_S32, VG_LITE_HIGH,
                      (uint32_t)((s_used - s_start) * sizeof(int32_t)),
                      &s_arena[s_start],
                      x0 - 1.0f, y0 - 1.0f, x1 + 1.0f, y1 + 1.0f);
    s_start = s_used;
    return VG_LITE_SUCCESS;
}

void vgc_emit_rect_cw(int32_t x, int32_t y, int32_t w, int32_t h)
{
    vgc_emit(VLC_OP_MOVE); vgc_emit(x);     vgc_emit(y);
    vgc_emit(VLC_OP_LINE); vgc_emit(x + w); vgc_emit(y);
    vgc_emit(VLC_OP_LINE); vgc_emit(x + w); vgc_emit(y + h);
    vgc_emit(VLC_OP_LINE); vgc_emit(x);     vgc_emit(y + h);
    vgc_emit(VLC_OP_CLOSE);
}

void vgc_emit_rect_ccw(int32_t x, int32_t y, int32_t w, int32_t h)
{
    vgc_emit(VLC_OP_MOVE); vgc_emit(x);     vgc_emit(y);
    vgc_emit(VLC_OP_LINE); vgc_emit(x);     vgc_emit(y + h);
    vgc_emit(VLC_OP_LINE); vgc_emit(x + w); vgc_emit(y + h);
    vgc_emit(VLC_OP_LINE); vgc_emit(x + w); vgc_emit(y);
    vgc_emit(VLC_OP_CLOSE);
}
