/* vgc_harness.h - case-table types and shared state for the GC355/VGLite
 * conformance probe.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Spec: docs/superpowers/specs/2026-08-30-gc355-conformance-design.md
 *
 * ★ TWO VERDICTS PER CASE, ALWAYS BOTH PRINTED. Every GC355 defect this tree
 * has hit shares one property: the driver reported success. So a case reports
 * what the API said (`api=`) and, independently, what the pixels say
 * (`pixel=`). `api=success pixel=broken` is the cell this whole example
 * exists to populate. */
#ifndef VGC_HARNESS_H
#define VGC_HARNESS_H

#include <stddef.h>
#include <stdint.h>

extern "C" {
#include "vg_lite.h"
}

/* Scratch render target. 128x128 BGRA8888 in EXTMEM (SDRAM at 0x80000000,
 * brought up by the core's startup before setup()). EXTMEM because the GPU
 * reaches it as a bus master exactly as it reaches a framebuffer, and because
 * the imxrt1176 core never enables the D-cache -- so a CPU read after
 * vg_lite_finish() sees the GPU's pixels with no maintenance. */
#define VGC_W 128
#define VGC_H 128

/* ★ TESSELLATION BUFFER SMALLER THAN THE TARGET, ON PURPOSE.
 * A tess buffer >= the target puts the driver in its ts_is_fullscreen == 1
 * regime, in which (spec section 1) scissor left/top clamping is silently
 * disabled -- a different machine from the one the production compositors
 * run on (720x1280 target, 256x256 tess: multi-tile). 64x64 against a 128x128
 * target keeps Phase 1 in the SAME regime as shipping code. Phase 3's
 * scissor/tess-fullscreen case deliberately probes the other one. */
#define VGC_TESS_W 64
#define VGC_TESS_H 64

/* vg_lite_color_t is ABGR (0xAABBGGRR) -- red in the LOW byte. Measured in
 * vglite_probe; getting it backwards does not fail, it renders the wrong
 * colour while every status says success. */
#define VGC_ABGR(r, g, b) (0xFF000000u | ((uint32_t)(b) << 16) | \
                           ((uint32_t)(g) << 8) | (uint32_t)(r))
#define VGC_BG_COLOR   VGC_ABGR(0x00, 0x00, 0x00)   /* opaque black */
#define VGC_FILL_COLOR VGC_ABGR(0xFF, 0xFF, 0xFF)   /* opaque white */

extern vg_lite_buffer_t vgc_scratch;

/* Clear the scratch target to VGC_BG_COLOR and finish. Every run() starts
 * here, so a case cannot contaminate its neighbour. */
vg_lite_error_t vgc_clear(void);

/* FNV-1a over the whole scratch buffer, rows only (stride-aware). */
uint32_t vgc_scratch_sum(void);

/* Read one scratch pixel as a memory word. */
uint32_t vgc_px(int x, int y);

/* ---- shared path arena ----------------------------------------------------
 * Cases emit path words here. vg_lite_draw() -> push_data() memcpys the path
 * into the command buffer before returning (vg_lite_path.c; vg_lite_init_path
 * never sets the upload bit), so the arena is reusable the instant the
 * preceding draw returns. Overflow is COUNTED and REFUSED rather than
 * truncating: a truncated path has no VLC_OP_END, and unterminated path data
 * is exactly what hangs the Vivante front end while every call still returns
 * VG_LITE_SUCCESS. */
#define VGC_ARENA_WORDS 512

void     vgc_arena_reset(void);
void     vgc_emit(int32_t w);
/* Terminates the path with an explicit VLC_OP_END and inits `p` over the
 * words emitted since the last vgc_arena_reset()/vgc_finish_path().
 *
 * ★ EXPLICIT END, NEVER A TRAILING CLOSE. vg_lite_init_path() rewrites a
 * trailing VLC_OP_CLOSE into VLC_OP_END in place, and its VG_LITE_S8 branch
 * does so through an (int*) cast -- four bytes where one was meant. Ending on
 * an explicit END means the fixup never fires, so no case here is measuring
 * that fixup instead of the hardware.
 *
 * Bounds are padded one unit on every side: the driver derives its
 * tessellation window from this box (rounded, not exact), and an exact bound
 * can land a half-pixel short at a tile boundary.
 *
 * Returns false if the arena overflowed; the caller must NOT draw. */
bool vgc_finish_path(vg_lite_path_t *p, float x0, float y0, float x1, float y1);

/* Emit a closed axis-aligned rect contour (CW: x,y -> x+w,y -> x+w,y+h ->
 * x,y+h). Coordinates are S32 path units == scratch pixels (identity matrix,
 * no fixed-point scaling: these cases probe geometry, not transforms). */
void vgc_emit_rect_cw(float x, float y, float w, float h);
/* The same rect wound the other way (CCW), for non-zero hole cutting. */
void vgc_emit_rect_ccw(float x, float y, float w, float h);

/* ---- the case table ------------------------------------------------------ */
typedef enum { VGC_SKIP = 0, VGC_OK = 1, VGC_BROKEN = 2 } vgc_verdict_t;

#define VGC_DETAIL_MAX 96

typedef struct {
    const char *id;                 /* stable slug -- expected_silicon.txt keys on it */
    /* Issues the vg_lite calls under test into vgc_scratch and finishes.
     * Returns VG_LITE_SUCCESS if every call succeeded, else the FIRST
     * non-success code. The harness has already cleared the scratch. */
    vg_lite_error_t (*run)(void);
    /* Reads scratch pixels and answers ONE structural question. Writes a
     * short "k=v,k=v" string (no spaces) into `detail`. */
    vgc_verdict_t (*check)(char *detail, size_t detail_len);
    /* OPTIONAL. A case whose run() renders more than once leaves only its
     * LAST sub-render in the scratch buffer, so the harness's default
     * repeat= sum would cover only that one. Such a case supplies this to
     * return an FNV accumulated over EVERY sub-render. NULL => the harness
     * uses vgc_scratch_sum(). */
    uint32_t (*sum)(void);
} vgc_case_t;

/* Defined in vgc_cases_path.cpp */
extern const vgc_case_t vgc_path_cases[];
extern const size_t     vgc_path_case_count;

/* Defined in vgc_dangerous.cpp. Empty unless built -DVGC_DANGEROUS=1. */
extern const vgc_case_t vgc_dangerous_cases[];
extern const size_t     vgc_dangerous_case_count;

#endif /* VGC_HARNESS_H */
