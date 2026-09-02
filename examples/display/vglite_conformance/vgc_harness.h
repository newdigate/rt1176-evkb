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
/* The general form, and the ONLY place the byte layout is spelled: alpha in
 * the HIGH byte, red in the LOW one. Phase 2's blend cases need a non-opaque
 * source (every Phase 1 case was opaque, which is why blend/none-honours-alpha
 * exists), and VGC_ABGR is this with alpha 0xFF -- one expression, so the ★
 * above cannot come true in one macro and not the other.
 *
 * Each component is MASKED. That was needless while every caller passed a
 * literal; an alpha that is COMPUTED is exactly what this macro is for, and an
 * out-of-range one would silently corrupt the blue channel beside it -- the
 * instrument fabricating a colour, in a case whose whole question is what
 * colour came out. */
#define VGC_ABGR_A(a, r, g, b) ((((uint32_t)(a) & 0xFFu) << 24) | \
                                (((uint32_t)(b) & 0xFFu) << 16) | \
                                (((uint32_t)(g) & 0xFFu) <<  8) | \
                                 ((uint32_t)(r) & 0xFFu))
#define VGC_ABGR(r, g, b) VGC_ABGR_A(0xFFu, (r), (g), (b))

#define VGC_BG_COLOR   VGC_ABGR(0x00, 0x00, 0x00)   /* opaque black */
#define VGC_FILL_COLOR VGC_ABGR(0xFF, 0xFF, 0xFF)   /* opaque white */

extern vg_lite_buffer_t vgc_scratch;

/* Clear the scratch target to VGC_BG_COLOR and finish.
 *
 * The harness calls this before every run(), so a case cannot contaminate its
 * neighbour.
 *
 * ★ A CASE THAT RENDERS MORE THAN ONCE MUST CALL THIS ITSELF between its
 * sub-renders. The harness's clear happens ONCE, before run() is entered; it
 * cannot see inside a case that draws, measures, and draws again. Forget it
 * and sub-render N+1 composites over N, which does not fail -- it yields a
 * plausible-looking wrong verdict, i.e. the instrument fabricating a GC355
 * defect. That is the top risk named in spec section 11, and it is on the case
 * author, not the harness. Cases known to need it: evenodd-vs-nonzero,
 * self-intersecting, format-agreement. Such a case must also supply the
 * vgc_case_t::sum hook, since only its LAST sub-render survives in the
 * buffer. */
vg_lite_error_t vgc_clear(void);

/* Clear the scratch to an arbitrary colour and finish. vgc_clear() is this
 * with VGC_BG_COLOR; the blend cases need a NON-ZERO backdrop, because
 * SRC_OVER over black is degenerate (dst*(1-a) vanishes) and cannot
 * distinguish a correct blend from one that ignores the destination.
 *
 * ★ THIS IS THE ONE CALL THAT CAN BREAK vgc_is_filled's PRECONDITION, so the
 * warning belongs here rather than only at the predicate. That predicate
 * thresholds green at the midpoint and is sound ONLY under the black-
 * background / white-fill convention every Phase 1 case keeps. Clear to a
 * mid-grey and it reports EVERY pixel filled -- the instrument announcing a
 * full-coverage render of a buffer nothing drew into. A case that clears to a
 * non-black colour must use a predicate that does not assume the convention;
 * see the ★ on vgc_is_filled in vgc_predicates.h. */
vg_lite_error_t vgc_clear_to(uint32_t abgr);

/* ---- scratch access ------------------------------------------------------
 * ★ ONE ACCESS PATH, AND IT IS NOT vgc_scratch.memory. Everything that reads
 * rendered pixels goes through vgc_fb(); vgc_px() and vgc_scratch_sum() are
 * implemented on top of it. Reading via vgc_scratch.memory instead would be a
 * SECOND path that agrees today and diverges twice over: a later case that
 * re-points vgc_scratch, and the engine-absent path, where vgc_scratch is
 * never populated at all (the field setup lives inside the init success
 * branch) so .memory is NULL while the underlying array is a valid zeroed
 * buffer. A predicate reading through NULL is a fault; reading through a
 * stale pointer is worse, because it answers. */
const uint32_t *vgc_fb(void);

/* FNV-1a over the whole scratch buffer.
 *
 * The buffer is PACKED by construction -- it is a fixed static array of
 * VGC_W * VGC_H * 4 bytes, and vgc_scratch.stride is set to VGC_W * 4 from
 * the same constants in the one place that initialises it. There is no stride
 * to be aware of, which is why this hashes a flat byte count rather than
 * walking rows. If a future case ever gives the target a stride wider than its
 * width, this function and vgc_px() both have to learn about it. */
uint32_t vgc_scratch_sum(void);

/* Read one scratch pixel as a memory word.
 *
 * ★ OUT-OF-RANGE IS COUNTED, NOT ANSWERED. An (x,y) outside the buffer
 * returns 0 and bumps a sticky counter that the harness reads after check().
 * A case whose check() went out of range is reported pixel=skip with
 * detail=px_oob:N and never contributes an ok or a broken -- because the
 * alternative is the instrument returning a plausible word for a caller's
 * typo, which is exactly the failure vgc_count_runs_col's -1 contract exists
 * to prevent (see vgc_predicates.h). A defect in the probe must never be
 * spellable as a measurement of the GPU. Note 0 is NOT a safe sentinel on its
 * own here -- it is transparent black, a word a blending case could legally
 * produce -- so the COUNTER is the mechanism and the return value is only
 * damage limitation. */
uint32_t vgc_px(int x, int y);
/* Out-of-range vgc_px() calls since the last vgc_px_oob_reset(). */
uint32_t vgc_px_oob(void);
void     vgc_px_oob_reset(void);

/* ---- per-case helpers ----------------------------------------------------
 * These live here rather than in each case file because Phase 2 and Phase 3
 * would otherwise each re-declare them, and because vgc_draw_path/
 * vgc_finish_into implement the status-accumulation contract that
 * vgc_case_t::run is specified in terms of. A contract stated in one place
 * and re-implemented in three diverges silently. */

/* A pointer to an identity matrix, re-identity'd on every call. Valid until
 * the next vgc_ident(). */
vg_lite_matrix_t *vgc_ident(void);

/* Draw `p` into vgc_scratch with VG_LITE_BLEND_NONE and the identity matrix.
 * The FIRST non-success status is accumulated into *acc; a later success
 * never clears an earlier failure. */
void vgc_draw_path(vg_lite_path_t *p, vg_lite_fill_t rule, uint32_t color,
                   vg_lite_error_t *acc);

/* Draw with an explicit blend mode. vgc_draw_path() is this with
 * VG_LITE_BLEND_NONE -- which is what all fifteen Phase 1 cases use, and what
 * NO shipping code uses: both compositors use SRC_OVER exclusively. That gap
 * is why Phase 2 exists. */
void vgc_draw_path_blend(vg_lite_path_t *p, vg_lite_fill_t rule, uint32_t color,
                         vg_lite_blend_t blend, vg_lite_error_t *acc);

/* vg_lite_finish(), accumulating the first non-success into *acc. */
void vgc_finish_into(vg_lite_error_t *acc);

/* ---- shared path arena ----------------------------------------------------
 * Cases emit path words here. vg_lite_draw() -> push_data() memcpys the path
 * into the command buffer before returning (vg_lite_path.c; vg_lite_init_path
 * never sets the upload bit), so the arena is reusable the instant the
 * preceding draw returns. Overflow is COUNTED and REFUSED rather than
 * truncating: a truncated path has no VLC_OP_END, and unterminated path data
 * is exactly what hangs the Vivante front end while every call still returns
 * VG_LITE_SUCCESS. */
#define VGC_ARENA_WORDS 512

/* Drop every word emitted so far and clear the overflow latch. THE HARNESS
 * ALREADY CALLS THIS before each run() (and again before the repeat run), so
 * a case wanting one arena's worth of paths need not call it at all. A case
 * calls it only to reuse the arena WITHIN a run() -- which is safe the instant
 * the preceding vg_lite_draw() has returned, per the note above, but which
 * also invalidates every vg_lite_path_t already initialised over the arena. */
void vgc_arena_reset(void);
void vgc_emit(int32_t w);

/* Terminates the path with an explicit VLC_OP_END and inits `p` over the
 * words emitted since the last vgc_arena_reset()/vgc_finish_path().
 *
 * ★ EXPLICIT END, NEVER A TRAILING CLOSE, AND THE REASON IS AN OUT-OF-BOUNDS
 * WRITE. vg_lite_init_path() rewrites a trailing VLC_OP_CLOSE into VLC_OP_END
 * in place, and its VG_LITE_S8 branch reads and writes at DIFFERENT offsets
 * (vg_lite_path.c:203-205, read verbatim):
 *
 *     if (path_data && (*((char*)path_data + num - 1) == VLC_OP_CLOSE))
 *         *(char*)((int*)path_data + num - 1) = VLC_OP_END;
 *
 * The test reads byte num-1 -- correct, the last element. The store writes ONE
 * byte at (char*)path_data + 4*(num-1), four times the intended offset. For an
 * 11-byte S8 path that is a single-byte write at offset 40: 29 bytes PAST the
 * end of the array, into whatever follows it in .data. (S16 is unaffected --
 * that branch reads and writes the same 2*(num-1).)
 *
 * Ending on an explicit END means the branch never fires at all, since
 * VLC_OP_END is 0x00 and VLC_OP_CLOSE is 0x01, so no case here can be
 * corrupted by that fixup or can accidentally end up measuring it instead of
 * the hardware.
 *
 * Bounds are padded one unit on every side: the driver derives its
 * tessellation window from this box (rounded, not exact), and an exact bound
 * can land a half-pixel short at a tile boundary.
 *
 * ★ ON OVERFLOW: returns VG_LITE_OUT_OF_RESOURCES and leaves *p ZEROED. The
 * zeroing is not tidiness -- callers declare vg_lite_path_t on the stack, and
 * a caller that ignored the status and drew anyway would hand the GPU a path
 * pointer and length made of stack garbage. That is precisely the
 * unterminated-path-data condition described above: the harness would be
 * manufacturing the one failure it exists to measure. Zeroed, the worst a
 * dropped status can do is submit path=NULL, path_length=0. The status is a
 * vg_lite_error_t rather than a bool so that discarding it reads as wrong at
 * the call site. */
vg_lite_error_t vgc_finish_path(vg_lite_path_t *p,
                                float x0, float y0, float x1, float y1);

/* Emit a closed axis-aligned rect contour (CW: x,y -> x+w,y -> x+w,y+h ->
 * x,y+h). Coordinates are S32 path units == scratch pixels (identity matrix,
 * no fixed-point scaling: these cases probe geometry, not transforms), so the
 * parameters are int32_t -- floats here would truncate toward zero at the
 * call site and quietly mislead the first sub-pixel case that tried them. */
void vgc_emit_rect_cw(int32_t x, int32_t y, int32_t w, int32_t h);
/* The same rect wound the other way (CCW), for non-zero hole cutting. */
void vgc_emit_rect_ccw(int32_t x, int32_t y, int32_t w, int32_t h);

/* ---- coverage verdict -----------------------------------------------------
 * A case's pixel= verdict is `structural predicate AND fill within tolerance
 * of the analytic area`, so pixel=ok means THE PICTURE IS RIGHT rather than
 * merely that the structure is. The comparison rides in detail= as
 * cover=ok / cover=stray:N / cover=short:N / cover=n/a.
 *
 * Tolerance is k*PERIMETER, never a percentage of area, and coverage is
 * judged ONLY where the structural predicate passed. Both rules, their
 * derivation and the measurements behind k live with the tolerance helpers in
 * vgc_cases_path.cpp -- not restated here, because a precis in a header is
 * what someone retuning k there would not think to update. */
typedef struct {
    int  ok;
    char s[24];     /* "cover=stray:" (12) + an int (11) + NUL */
} vgc_cover_t;

/* Not applicable: the case has no analytic area to compare against -- every
 * Phase 2 colour case, and any path case whose structural predicate failed.
 * Never fails a case on its own. (The tolerance helpers are static in
 * vgc_cases_path.cpp; see there for why.)
 *
 * static inline for the same reason every vgc_predicates.h helper is: no TU
 * owns it, so no link line has to know about it -- each host suite links
 * whichever case file it tests and gets this for free. */
static inline vgc_cover_t vgc_cover_na(void)
{
    /* An aggregate initialiser rather than snprintf: it ZERO-FILLS the tail.
     * `vgc_cover_t c;` plus a printf leaves s[10..23] indeterminate in a
     * struct RETURNED BY VALUE -- nothing reads past the NUL today, so it is
     * latent, but an instrument whose thesis is that it cannot fabricate must
     * not hand back stack bytes. It also keeps <stdio.h> out of a header four
     * host suites include, and makes a too-long literal a COMPILE ERROR
     * instead of a silent truncation. */
    vgc_cover_t c = { 1, "cover=n/a" };
    return c;
}

/* ---- the case table ------------------------------------------------------ */
typedef enum { VGC_SKIP = 0, VGC_OK = 1, VGC_BROKEN = 2 } vgc_verdict_t;

#define VGC_DETAIL_MAX 96

typedef struct {
    const char *id;                 /* stable slug -- expected_silicon.txt keys on it */
    /* Issues the vg_lite calls under test into vgc_scratch and finishes.
     * Returns VG_LITE_SUCCESS if every call succeeded, else the FIRST
     * non-success code. The harness has already cleared the scratch and reset
     * the arena. */
    vg_lite_error_t (*run)(void);
    /* Reads scratch pixels and answers ONE structural question. Writes a
     * short "k=v,k=v" string (no spaces) into `detail`. */
    vgc_verdict_t (*check)(char *detail, size_t detail_len);
    /* OPTIONAL. A case whose run() renders more than once leaves only its
     * LAST sub-render in the scratch buffer, so the harness's default
     * repeat= sum would cover only that one. Such a case supplies this to
     * return an FNV accumulated over EVERY sub-render. NULL => the harness
     * uses vgc_scratch_sum().
     *
     * ★ ORDERING, which constrains what it may depend on: the harness calls
     * sum() immediately after run() and BEFORE check(), then runs the case a
     * second time and calls sum() again with NO check() in between. So sum()
     * must stand on its own -- it may not read state that check() computes,
     * and it must return the same value for the same rendering whether or not
     * check() has ever run. */
    uint32_t (*sum)(void);
} vgc_case_t;

/* Defined in vgc_cases_path.cpp */
extern const vgc_case_t vgc_path_cases[];
extern const size_t     vgc_path_case_count;

/* Defined in vgc_cases_color.cpp. Runs AFTER the path cases: if basic filling
 * is broken, no colour verdict below it means anything. */
extern const vgc_case_t vgc_color_cases[];
extern const size_t     vgc_color_case_count;

/* Defined in vgc_dangerous.cpp. Empty unless built -DVGC_DANGEROUS=1. */
extern const vgc_case_t vgc_dangerous_cases[];
extern const size_t     vgc_dangerous_case_count;

#endif /* VGC_HARNESS_H */
