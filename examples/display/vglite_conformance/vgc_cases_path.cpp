/* vgc_cases_path.cpp - paths, contours and winding (spec section 5).
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * The area that produced the ONE-CONTOUR-PER-PATH rule, which is why the spec
 * probes it first: on this GC355 a path renders only its FIRST contour, every
 * subpath after the first VLC_OP_MOVE vanishing while every vg_lite_* call
 * returns VG_LITE_SUCCESS. Three cases here measure that directly
 * (multi-contour-disjoint, two-contour-ring-nonzero, evenodd-vs-nonzero) and
 * each is PAIRED with a control that must pass if the harness is sound:
 * single-contour-rect (the baseline -- if THIS is broken nothing else in the
 * matrix means anything), two-draws-ring (the safe usage) and
 * self-intersecting (a single contour whose fill rules must both be honoured).
 * If a control fails, suspect the harness, not the silicon (spec section 11).
 *
 * Every path ends with an explicit VLC_OP_END via vgc_finish_path(); see the
 * note there for why a trailing CLOSE is never used.
 *
 * ★ THE GEOMETRY AND EVERY PREDICATE IN THIS FILE WERE CHECKED ON THE HOST
 * before any of it reached a GPU, because QEMU has no GC355 and so cannot
 * exercise one line of the pixel logic -- every case there reports skip, so a
 * green gate says nothing at all about what is below. The real check_*()
 * functions were compiled against a scanline reference rasteriser (one that
 * honours ALL contours and both fill rules, i.e. a correct GPU) and all twelve
 * reported ok. That is what says a `broken` from the bench is the silicon and
 * not a mis-placed sample point or a too-tight tolerance. Measured there:
 * rect fill 6400/6400; triangle 1770 against the analytic 1800 (-1.7%, inside
 * the +/-8% bound); bars runs=4 down column 64; star sample points 15.00 px
 * and 2.45 px clear of the nearest edge (perpendicular distance to the rounded
 * integer path, not horizontal).
 *
 * ★ AND THE NEGATIVE ARM, which is the half that matters for an instrument:
 * with the same rasteriser re-broken to drop every contour after the first --
 * this GC355's actual defect -- the three probe cases went BROKEN by name
 * (runs=1 fill=1280; rim=1 centre=1; eo_centre=1) while ALL SIX controls and
 * the whole format set stayed ok. A matrix that cannot go red on the defect it
 * is aimed at would be decoration. */
#include "vgc_harness.h"
#include "vgc_predicates.h"
#include <stdio.h>
#include <string.h>

/* ---- shared drawing helpers live in the HARNESS ----------------------------
 * ★ `vgc_ident()`, `vgc_draw_path()`, `vgc_finish_into()` and `vgc_fb()` are
 * declared in vgc_harness.h, NOT defined here. They were local to this file in
 * the plan's first draft and were promoted in Task 2 for two reasons that only
 * showed up under review:
 *  - draw/finish implement a contract the HEADER already states ("the FIRST
 *    non-success code"). A contract specified in one place and re-implemented
 *    in each of Phases 1, 2 and 3 will silently diverge.
 *  - a local `fb()` returning `vgc_scratch.memory` would be a SECOND access
 *    path to the scratch buffer, disagreeing with the harness's own
 *    `vgc_px`/`vgc_scratch_sum` the moment a later case re-points the buffer
 *    -- and already disagreeing on the engine-absent path, where
 *    `vgc_scratch.memory` is NULL while the backing array is valid. */

/* Terminate the arena path and BAIL OUT of run() on overflow.
 *
 * The macro exists because the alternative -- a braced block per call site --
 * appears twelve times in this file and reads as noise, and because dropping
 * the status is the one thing that must not happen: vgc_finish_path() zeroes
 * *p on overflow, so a caller that carried on would submit path=NULL and
 * measure the harness rather than the GPU. The name carries the `return` so
 * the hidden control flow is visible at the call site. */
#define VGC_FINISH_OR_RETURN(p, x0, y0, x1, y1)                               \
    do {                                                                      \
        const vg_lite_error_t fe_ = vgc_finish_path((p), (x0), (y0), (x1), (y1)); \
        if (fe_ != VG_LITE_SUCCESS) return fe_;                               \
    } while (0)

/* ---- 1. path/single-contour-rect ------------------------------------------
 * THE BASELINE. One closed rect, one contour, one draw. 80x80 at (24,24). */

#define R_X 24
#define R_Y 24
#define R_W 80
#define R_H 80
#define R_AREA (R_W * R_H)      /* 6400 */

static vg_lite_error_t run_single_rect(void)
{
    vg_lite_error_t acc = VG_LITE_SUCCESS;
    vg_lite_path_t p;
    vgc_emit_rect_cw(R_X, R_Y, R_W, R_H);
    VGC_FINISH_OR_RETURN(&p, R_X, R_Y, R_X + R_W, R_Y + R_H);
    vgc_draw_path(&p, VG_LITE_FILL_NON_ZERO, VGC_FILL_COLOR, &acc);
    vgc_finish_into(&acc);
    return acc;
}

static vgc_verdict_t check_single_rect(char *d, size_t n)
{
    const int fill   = vgc_count_filled(vgc_fb(), VGC_W, VGC_H, VGC_W);
    const int centre = vgc_is_filled(vgc_px(64, 64));
    const int corner = vgc_is_filled(vgc_px(10, 10));
    snprintf(d, n, "fill=%d,expect=%d,centre=%d,corner=%d",
             fill, R_AREA, centre, corner);
    /* +/-6% is 384 px against a 320 px perimeter: wide enough that a whole
     * antialiased boundary ring landing either side of the 50% threshold is
     * absorbed, narrow enough that the nearest WRONG shape this case could
     * produce is rejected. The wrong shapes worth naming are all far outside
     * it -- a dropped draw is 0, a full-target fill is 16384, and the 32x32
     * inset used by cases 3-5 is 1024. */
    const int lo = R_AREA - R_AREA * 6 / 100, hi = R_AREA + R_AREA * 6 / 100;
    return (centre && !corner && fill >= lo && fill <= hi) ? VGC_OK : VGC_BROKEN;
}

/* ---- 2. path/multi-contour-disjoint ----------------------------------------
 * Four separated bars in ONE path. Count filled runs down column x=64.
 * EXPECTED BROKEN on this GC355: runs=1 (only the first contour renders). */

static const int BAR_Y[4] = { 16, 44, 72, 100 };
#define BAR_H 16

static vg_lite_error_t run_multi_contour(void)
{
    vg_lite_error_t acc = VG_LITE_SUCCESS;
    vg_lite_path_t p;
    for (int i = 0; i < 4; i++) vgc_emit_rect_cw(R_X, BAR_Y[i], R_W, BAR_H);
    VGC_FINISH_OR_RETURN(&p, R_X, BAR_Y[0], R_X + R_W, BAR_Y[3] + BAR_H);
    vgc_draw_path(&p, VG_LITE_FILL_NON_ZERO, VGC_FILL_COLOR, &acc);
    vgc_finish_into(&acc);
    return acc;
}

static vgc_verdict_t check_multi_contour(char *d, size_t n)
{
    /* Column 64 crosses all four bars (they span x 24..104) and the 12-row
     * gaps between them, so a correct render gives exactly 4 runs. fill= is
     * carried alongside because it DISCRIMINATES the two ways this can fail:
     * one bar rendered is fill~1280, all four merged into one block would be
     * ~8000. runs= alone cannot tell those apart. */
    const int runs = vgc_count_runs_col(vgc_fb(), VGC_W, VGC_H, VGC_W, 64);
    const int fill = vgc_count_filled(vgc_fb(), VGC_W, VGC_H, VGC_W);
    snprintf(d, n, "runs=%d,expect=4,fill=%d", runs, fill);
    return runs == 4 ? VGC_OK : VGC_BROKEN;
}

/* ---- 3. path/two-contour-ring-nonzero --------------------------------------
 * Outer CW + reversed inner CCW in ONE path under NON_ZERO: the classic hole.
 * EXPECTED BROKEN: the inner contour is dropped and the ring fills solid. */

#define I_X 48
#define I_Y 48
#define I_W 32
#define I_H 32

static vg_lite_error_t run_ring_two_contour(void)
{
    vg_lite_error_t acc = VG_LITE_SUCCESS;
    vg_lite_path_t p;
    vgc_emit_rect_cw(R_X, R_Y, R_W, R_H);
    vgc_emit_rect_ccw(I_X, I_Y, I_W, I_H);
    VGC_FINISH_OR_RETURN(&p, R_X, R_Y, R_X + R_W, R_Y + R_H);
    vgc_draw_path(&p, VG_LITE_FILL_NON_ZERO, VGC_FILL_COLOR, &acc);
    vgc_finish_into(&acc);
    return acc;
}

/* Shared by cases 3 and 4: rim filled AND centre background is the ring.
 * (32,64) is inside the outer plate (24..104) and clear of the inner one
 * (48..80) by 16 px; (64,64) is the inner plate's centre. */
static vgc_verdict_t check_ring(char *d, size_t n)
{
    const int rim    = vgc_is_filled(vgc_px(32, 64));   /* inside outer, outside inner */
    const int centre = vgc_is_filled(vgc_px(64, 64));   /* inside inner */
    snprintf(d, n, "rim=%d,centre=%d,expect=rim1centre0", rim, centre);
    return (rim && !centre) ? VGC_OK : VGC_BROKEN;
}

/* ---- 4. path/two-draws-ring ------------------------------------------------
 * THE SAFE-USAGE CONTROL for case 3. Same ring, built as a filled plate with
 * an inset plate in the background colour over it: two single-contour paths,
 * two draws. This is the construction both shipping compositors use.
 *
 * The inset is drawn with VG_LITE_BLEND_NONE (vgc_draw_path's fixed mode), so
 * it OVERWRITES rather than compositing -- which is what makes "background
 * colour" mean "hole" here and not "black over white at some alpha".
 *
 * ★ THIS IS THE ONE CASE THAT DRAWS TWICE AND MUST *NOT* CLEAR BETWEEN, which
 * is why it takes no sum() hook either. vgc_harness.h's rule is about
 * sub-RENDERS -- separate pictures measured one after another, as in cases 5,
 * 6 and 11. Here the two draws compose ONE picture and the second is meant to
 * land on the first, so the buffer holds the whole render when check() and the
 * harness's default sum() read it. Clearing between would erase the plate the
 * inset is supposed to punch through. */

static vg_lite_error_t run_ring_two_draws(void)
{
    vg_lite_error_t acc = VG_LITE_SUCCESS;
    vg_lite_path_t outer, inner;

    vgc_emit_rect_cw(R_X, R_Y, R_W, R_H);
    VGC_FINISH_OR_RETURN(&outer, R_X, R_Y, R_X + R_W, R_Y + R_H);
    vgc_draw_path(&outer, VG_LITE_FILL_NON_ZERO, VGC_FILL_COLOR, &acc);

    vgc_emit_rect_cw(I_X, I_Y, I_W, I_H);
    VGC_FINISH_OR_RETURN(&inner, I_X, I_Y, I_X + I_W, I_Y + I_H);
    vgc_draw_path(&inner, VG_LITE_FILL_NON_ZERO, VGC_BG_COLOR, &acc);

    vgc_finish_into(&acc);
    return acc;
}

/* ---- 5. path/evenodd-vs-nonzero --------------------------------------------
 * Nested rects with the SAME winding, drawn under each fill rule. EVEN_ODD
 * must cut the hole, NON_ZERO must fill solid -- that difference IS the fill
 * rule, and it is the only thing this case asks about.
 *
 * Renders twice inside one run(), so it clears between the passes itself (the
 * harness's clear happens once, before run() is entered) and supplies
 * sum_evenodd_nonzero() accumulating over BOTH sub-renders. See vgc_harness.h. */

static int      s_eo_rim, s_eo_centre;
static uint32_t s_eo_sum;

static void emit_nested(void)
{
    vgc_emit_rect_cw(R_X, R_Y, R_W, R_H);
    vgc_emit_rect_cw(I_X, I_Y, I_W, I_H);   /* same winding, deliberately */
}

static vg_lite_error_t run_evenodd_nonzero(void)
{
    vg_lite_error_t acc = VG_LITE_SUCCESS;
    vg_lite_path_t p;

    /* pass 1: EVEN_ODD, sampled and then cleared away */
    emit_nested();
    VGC_FINISH_OR_RETURN(&p, R_X, R_Y, R_X + R_W, R_Y + R_H);
    vgc_draw_path(&p, VG_LITE_FILL_EVEN_ODD, VGC_FILL_COLOR, &acc);
    vgc_finish_into(&acc);
    s_eo_rim    = vgc_is_filled(vgc_px(32, 64));
    s_eo_centre = vgc_is_filled(vgc_px(64, 64));
    s_eo_sum    = vgc_scratch_sum();

    /* pass 2: NON_ZERO, left in the scratch for check() to read live */
    vgc_arena_reset();
    const vg_lite_error_t ce = vgc_clear();
    if (ce != VG_LITE_SUCCESS && acc == VG_LITE_SUCCESS) acc = ce;
    emit_nested();
    VGC_FINISH_OR_RETURN(&p, R_X, R_Y, R_X + R_W, R_Y + R_H);
    vgc_draw_path(&p, VG_LITE_FILL_NON_ZERO, VGC_FILL_COLOR, &acc);
    vgc_finish_into(&acc);
    return acc;
}

static uint32_t sum_evenodd_nonzero(void)
{
    /* Both sub-renders, folded: the pass-1 sum and a live sum of the pass-2
     * buffer hashed together, so a difference in EITHER pass shows up in
     * repeat=. Depends only on state run() sets, never on check() -- the
     * ordering contract in vgc_harness.h. */
    uint32_t pair[2] = { s_eo_sum, vgc_scratch_sum() };
    return vgc_fnv(pair, sizeof(pair));
}

static vgc_verdict_t check_evenodd_nonzero(char *d, size_t n)
{
    const int nz_rim    = vgc_is_filled(vgc_px(32, 64));
    const int nz_centre = vgc_is_filled(vgc_px(64, 64));
    snprintf(d, n, "eo_rim=%d,eo_centre=%d,nz_rim=%d,nz_centre=%d",
             s_eo_rim, s_eo_centre, nz_rim, nz_centre);
    return (s_eo_rim && !s_eo_centre && nz_rim && nz_centre) ? VGC_OK : VGC_BROKEN;
}

/* ---- 6. path/self-intersecting ---------------------------------------------
 * A pentagram: ONE contour that crosses itself. The centre pentagon is EMPTY
 * under EVEN_ODD (crossing number 2) and FILLED under NON_ZERO (winding 2),
 * and the five tips are filled under both. This is the case that distinguishes
 * the two fill rules on a single contour -- so unlike case 5 it should pass on
 * this GC355, which makes it case 5's control as well as its own probe.
 *
 * Vertices: r=50 about (64,64), at -90 + k*144 degrees, k = 0..4, connected in
 * that order. Integers, rounded once here so the geometry is fixed.
 *
 * ★ THE TWO SAMPLE POINTS HAVE MARGIN, WHICH MATTERS BECAUSE THIS IS A
 * CONTROL -- a false BROKEN here would wrongly discredit the whole matrix.
 * Computed against the ROUNDED integer vertices below, as perpendicular
 * distance to the nearest of the five edges: centre (64,64) is 15.00 px clear
 * (edge V2-V3), tip (64,22) is 2.45 px clear (edge V0-V1). The tip is the
 * tight one by construction -- it sits inside a spike -- and 2.45 px is still
 * two full pixels of antialiased boundary away from the 50% threshold. Both
 * were then confirmed by rendering this exact integer path through a host
 * scanline rasteriser: see the file header. */

static const int STAR[5][2] = {
    {  64,  14 },   /* -90 deg */
    {  93, 104 },   /*  54    */
    {  16,  49 },   /* 198    */
    { 112,  49 },   /* 342    */
    {  35, 104 },   /* 126    */
};

static int      s_star_eo_centre, s_star_eo_tip;
static uint32_t s_star_eo_sum;

static void emit_star(void)
{
    vgc_emit(VLC_OP_MOVE); vgc_emit(STAR[0][0]); vgc_emit(STAR[0][1]);
    for (int i = 1; i < 5; i++) {
        vgc_emit(VLC_OP_LINE); vgc_emit(STAR[i][0]); vgc_emit(STAR[i][1]);
    }
    vgc_emit(VLC_OP_CLOSE);
}

static vg_lite_error_t run_self_intersecting(void)
{
    vg_lite_error_t acc = VG_LITE_SUCCESS;
    vg_lite_path_t p;

    emit_star();
    VGC_FINISH_OR_RETURN(&p, 16, 14, 112, 104);
    vgc_draw_path(&p, VG_LITE_FILL_EVEN_ODD, VGC_FILL_COLOR, &acc);
    vgc_finish_into(&acc);
    s_star_eo_centre = vgc_is_filled(vgc_px(64, 64));   /* centre pentagon */
    s_star_eo_tip    = vgc_is_filled(vgc_px(64, 22));   /* inside the top point */
    s_star_eo_sum    = vgc_scratch_sum();

    vgc_arena_reset();
    const vg_lite_error_t ce = vgc_clear();
    if (ce != VG_LITE_SUCCESS && acc == VG_LITE_SUCCESS) acc = ce;
    emit_star();
    VGC_FINISH_OR_RETURN(&p, 16, 14, 112, 104);
    vgc_draw_path(&p, VG_LITE_FILL_NON_ZERO, VGC_FILL_COLOR, &acc);
    vgc_finish_into(&acc);
    return acc;
}

static uint32_t sum_self_intersecting(void)
{
    uint32_t pair[2] = { s_star_eo_sum, vgc_scratch_sum() };
    return vgc_fnv(pair, sizeof(pair));
}

static vgc_verdict_t check_self_intersecting(char *d, size_t n)
{
    const int nz_centre = vgc_is_filled(vgc_px(64, 64));
    const int nz_tip    = vgc_is_filled(vgc_px(64, 22));
    snprintf(d, n, "eo_centre=%d,eo_tip=%d,nz_centre=%d,nz_tip=%d",
             s_star_eo_centre, s_star_eo_tip, nz_centre, nz_tip);
    return (!s_star_eo_centre && s_star_eo_tip && nz_centre && nz_tip)
           ? VGC_OK : VGC_BROKEN;
}

/* ---- 7-11. path/format-* ---------------------------------------------------
 * The SAME right triangle in each of the four path coordinate formats. The
 * driver reads path data as an array of the FORMAT's element width -- opcodes
 * included -- so each format needs its own typed array rather than a shared
 * int32_t arena.
 *
 * Vertices (10,10),(70,10),(10,70): area 1800 px, and inside the signed 8-bit
 * range so VG_LITE_S8 can express it. Each format's own case checks its fill
 * count against that analytic area, so each stands alone; path/format-agreement
 * then checks all four counts against EACH OTHER, which is the cross-format
 * question the per-format cases cannot ask.
 *
 * ★ AN OPCODE IS ONE BYTE AT THE BASE OF A FORMAT-WIDTH SLOT, NOT A VALUE OF
 * THE FORMAT'S TYPE -- which is invisible for S8/S16/S32 and WRONG for FP32.
 * Read out of the driver, in two independent places:
 *   - vg_lite_path.c:223-229, the CLOSE->END fixup this very file's paths are
 *     built to dodge, tests the FP32 case as
 *     `*(char*)((float*)path_data + num - 1) == VLC_OP_CLOSE` -- a CHAR read
 *     at the float slot's base address.
 *   - vg_lite_stroke.c:5148-5155 builds an FP32 path with
 *     `cpath = (char*)pathdata + offset; *cpath = VLC_OP_MOVE; fpath++;` --
 *     one byte written, then the whole 4-byte slot skipped.
 * So the MOVE slot of an FP32 path is the bit pattern 0x00000002, and
 * `(float)VLC_OP_MOVE` (2.0f = 0x40000000, so little-endian bytes 00 00 00 40
 * and an opcode byte of 0x00 = VLC_OP_END) terminates the path immediately.
 * MEASURED, not reasoned: built that way and run through the host reference
 * rasteriser, `path/format-fp32` reports `broken fill=0` and drags
 * `path/format-agreement` down with it (`fp32=0`) -- two false BROKENs, the
 * instrument fabricating a GC355 defect. So the FP32 array is built through
 * its BYTES rather than written as float literals.
 *
 * The S8/S16/S32 arrays get this for free on little-endian ARM: byte 0 of the
 * integer 2 is 0x02 whatever the width. They are still spelled out per format
 * because the COORDINATES are read at the format's width.
 *
 * Every array ends with an explicit VLC_OP_END -- especially load-bearing for
 * S8, whose CLOSE->END fixup in vg_lite_init_path() writes through an (int*)
 * cast (4 bytes where 1 was meant). Ending on END means it never fires.
 *
 * All four are `static` and NOT `const`: vg_lite_init_path takes a non-const
 * pointer (it is the function that would perform that fixup). */

#define TRI_AREA 1800

/* 11 slots each: MOVE x y | LINE x y | LINE x y | CLOSE | END. The opcode-only
 * slots (CLOSE, END) still occupy a full element -- the driver advances the
 * cursor by one slot and then re-aligns to the element width. */
static int8_t  s_tri_s8[]  = { VLC_OP_MOVE, 10, 10, VLC_OP_LINE, 70, 10,
                               VLC_OP_LINE, 10, 70, VLC_OP_CLOSE, VLC_OP_END };
static int16_t s_tri_s16[] = { VLC_OP_MOVE, 10, 10, VLC_OP_LINE, 70, 10,
                               VLC_OP_LINE, 10, 70, VLC_OP_CLOSE, VLC_OP_END };
static int32_t s_tri_s32[] = { VLC_OP_MOVE, 10, 10, VLC_OP_LINE, 70, 10,
                               VLC_OP_LINE, 10, 70, VLC_OP_CLOSE, VLC_OP_END };
static float   s_tri_f32[11];

/* Write `op` into FP32 slot `i`: the opcode in byte 0, the other three bytes
 * zero padding. memcpy rather than a cast so no strict-aliasing question
 * arises and no union extension is needed. */
static void f32_op(int i, uint8_t op)
{
    const uint32_t w = op;      /* 0x000000NN */
    memcpy(&s_tri_f32[i], &w, sizeof(w));
}

/* Rebuilt on every use rather than once, and cheap enough that a lazy-init
 * flag would be the only thing here able to go wrong. It also means nothing
 * this file does can leave the array mutated between the harness's two
 * identical runs of a case. */
static void build_tri_f32(void)
{
    f32_op(0, VLC_OP_MOVE);  s_tri_f32[1] = 10.0f; s_tri_f32[2] = 10.0f;
    f32_op(3, VLC_OP_LINE);  s_tri_f32[4] = 70.0f; s_tri_f32[5] = 10.0f;
    f32_op(6, VLC_OP_LINE);  s_tri_f32[7] = 10.0f; s_tri_f32[8] = 70.0f;
    f32_op(9, VLC_OP_CLOSE);
    f32_op(10, VLC_OP_END);
}

static vg_lite_error_t run_triangle(vg_lite_format_t fmt, void *data,
                                    uint32_t bytes)
{
    vg_lite_error_t acc = VG_LITE_SUCCESS;
    vg_lite_path_t p;
    memset(&p, 0, sizeof(p));
    vg_lite_init_path(&p, fmt, VG_LITE_HIGH, bytes, data, 9.0f, 9.0f, 71.0f, 71.0f);
    vgc_draw_path(&p, VG_LITE_FILL_NON_ZERO, VGC_FILL_COLOR, &acc);
    vgc_finish_into(&acc);
    return acc;
}

static vg_lite_error_t run_fmt_s8(void)
{ return run_triangle(VG_LITE_S8, s_tri_s8, (uint32_t)sizeof(s_tri_s8)); }
static vg_lite_error_t run_fmt_s16(void)
{ return run_triangle(VG_LITE_S16, s_tri_s16, (uint32_t)sizeof(s_tri_s16)); }
static vg_lite_error_t run_fmt_s32(void)
{ return run_triangle(VG_LITE_S32, s_tri_s32, (uint32_t)sizeof(s_tri_s32)); }
static vg_lite_error_t run_fmt_f32(void)
{ build_tri_f32(); return run_triangle(VG_LITE_FP32, s_tri_f32, (uint32_t)sizeof(s_tri_f32)); }

/* ONE check for all four formats, reading the scratch LIVE.
 *
 * The plan had run() capture its own fill count into a per-format slot and
 * check() read the slot back. Counting here instead removes four pieces of
 * static state and, with them, the only way one format case could have read
 * another's leftovers -- the coupling path/format-agreement's comment below
 * calls out. It is sound because the harness calls check() immediately after
 * run() with the render still in the buffer, which is how every other case in
 * this file already works. The case id names the format, so the detail need
 * not. */
static vgc_verdict_t check_fmt(char *d, size_t n)
{
    const int fill = vgc_count_filled(vgc_fb(), VGC_W, VGC_H, VGC_W);
    /* +/-8%: the triangle's perimeter is ~205 px, so a pixel of antialiased
     * boundary either way is ~5.7% of 1800. Sampling at pixel centres alone
     * costs -1.7% (1770 measured on the host reference), so the band has to
     * hold both; it still rejects a dropped draw (0) and any wrong shape this
     * case could plausibly produce. */
    const int lo = TRI_AREA - TRI_AREA * 8 / 100, hi = TRI_AREA + TRI_AREA * 8 / 100;
    snprintf(d, n, "fill=%d,expect=%d", fill, TRI_AREA);
    return (fill >= lo && fill <= hi) ? VGC_OK : VGC_BROKEN;
}

/* path/format-agreement: renders all four again, in one run, and compares.
 * It does NOT read any state left by the four cases above -- doing so would
 * make this case's answer depend on the table's ORDER, which is exactly the
 * coupling the one-case-at-a-time design forbids. Renders four times, so it
 * clears between them and supplies its own sum(). */

static int      s_agree[4];
static uint32_t s_agree_sum;

static vg_lite_error_t run_fmt_agreement(void)
{
    vg_lite_error_t acc = VG_LITE_SUCCESS;
    uint32_t sums[4];
    build_tri_f32();
    struct { vg_lite_format_t f; void *d; uint32_t n; } v[4] = {
        { VG_LITE_S8,   s_tri_s8,  (uint32_t)sizeof(s_tri_s8)  },
        { VG_LITE_S16,  s_tri_s16, (uint32_t)sizeof(s_tri_s16) },
        { VG_LITE_S32,  s_tri_s32, (uint32_t)sizeof(s_tri_s32) },
        { VG_LITE_FP32, s_tri_f32, (uint32_t)sizeof(s_tri_f32) },
    };
    for (int i = 0; i < 4; i++) {
        const vg_lite_error_t ce = vgc_clear();
        if (ce != VG_LITE_SUCCESS && acc == VG_LITE_SUCCESS) acc = ce;
        vg_lite_path_t p;
        memset(&p, 0, sizeof(p));
        vg_lite_init_path(&p, v[i].f, VG_LITE_HIGH, v[i].n, v[i].d,
                          9.0f, 9.0f, 71.0f, 71.0f);
        vgc_draw_path(&p, VG_LITE_FILL_NON_ZERO, VGC_FILL_COLOR, &acc);
        vgc_finish_into(&acc);
        s_agree[i] = vgc_count_filled(vgc_fb(), VGC_W, VGC_H, VGC_W);
        sums[i]    = vgc_scratch_sum();
    }
    s_agree_sum = vgc_fnv(sums, sizeof(sums));
    return acc;
}

static uint32_t sum_fmt_agreement(void) { return s_agree_sum; }

static vgc_verdict_t check_fmt_agreement(char *d, size_t n)
{
    snprintf(d, n, "s8=%d,s16=%d,s32=%d,fp32=%d",
             s_agree[0], s_agree[1], s_agree[2], s_agree[3]);
    /* NON-ZERO FIRST, and it is not belt-and-braces: four EMPTY renders agree
     * perfectly, so equality alone makes this case green on a GPU that draws
     * nothing at all. Testing slot 0 here and equality below gives each test
     * one job -- the two folded into one loop condition left the slot-0 check
     * unreachable, since equality already implies it. */
    if (s_agree[0] == 0) return VGC_BROKEN;
    for (int i = 1; i < 4; i++)
        if (s_agree[i] != s_agree[0]) return VGC_BROKEN;
    return VGC_OK;
}

/* ---- 12. path/degenerate-zero-area -----------------------------------------
 * A zero-height rect. BOTH outcomes are acceptable -- nothing drawn, or a
 * hairline on the degenerate row -- because the point is that the outcome is
 * DEFINED and RECORDED rather than a crash or a hang. Anything OUTSIDE the
 * degenerate row band is a real defect: it means the rasteriser invented
 * geometry. */

#define DEG_Y 64

static vg_lite_error_t run_degenerate(void)
{
    vg_lite_error_t acc = VG_LITE_SUCCESS;
    vg_lite_path_t p;
    vgc_emit(VLC_OP_MOVE); vgc_emit(R_X);         vgc_emit(DEG_Y);
    vgc_emit(VLC_OP_LINE); vgc_emit(R_X + R_W);   vgc_emit(DEG_Y);
    vgc_emit(VLC_OP_LINE); vgc_emit(R_X + R_W);   vgc_emit(DEG_Y);
    vgc_emit(VLC_OP_LINE); vgc_emit(R_X);         vgc_emit(DEG_Y);
    vgc_emit(VLC_OP_CLOSE);
    VGC_FINISH_OR_RETURN(&p, R_X, DEG_Y, R_X + R_W, DEG_Y);
    vgc_draw_path(&p, VG_LITE_FILL_NON_ZERO, VGC_FILL_COLOR, &acc);
    vgc_finish_into(&acc);
    return acc;
}

static vgc_verdict_t check_degenerate(char *d, size_t n)
{
    /* ★ SEEDED OUTSIDE [0,VGC_H) AND -1 WILL NOT DO. vgc_filled_rows leaves
     * both out-params UNTOUCHED when nothing is filled, and its own note
     * records that -1 sentinels make "untouched" indistinguishable from the
     * internal state it tracks -- measured, with a deliberately broken
     * predicate staying green. -99 is the value tests/predicates_test.c uses
     * for the same reason. The return value is what decides; ymin/ymax are
     * read only after it, and appear in the detail as -99 when nothing was
     * drawn, which is a reading rather than an ambiguity. */
    int ymin = -99, ymax = -99;
    const int fill = vgc_filled_rows(vgc_fb(), VGC_W, VGC_H, VGC_W, &ymin, &ymax);
    snprintf(d, n, "fill=%d,ymin=%d,ymax=%d", fill, ymin, ymax);
    if (fill == 0) return VGC_OK;                       /* nothing drawn: fine */
    return (ymin >= DEG_Y - 1 && ymax <= DEG_Y + 1) ? VGC_OK : VGC_BROKEN;
}

/* ---- the table -------------------------------------------------------------
 * ORDER IS PART OF THE INSTRUMENT. single-contour-rect is first because it is
 * the baseline: if it is broken, nothing below it means anything. The gate and
 * expected_silicon.txt key on these ids, so they are stable -- rename one and
 * both go red, which is the intent. */
const vgc_case_t vgc_path_cases[] = {
    { "path/single-contour-rect",     run_single_rect,       check_single_rect,       NULL },
    { "path/multi-contour-disjoint",  run_multi_contour,     check_multi_contour,     NULL },
    { "path/two-contour-ring-nonzero",run_ring_two_contour,  check_ring,              NULL },
    { "path/two-draws-ring",          run_ring_two_draws,    check_ring,              NULL },
    { "path/evenodd-vs-nonzero",      run_evenodd_nonzero,   check_evenodd_nonzero,   sum_evenodd_nonzero },
    { "path/self-intersecting",       run_self_intersecting, check_self_intersecting, sum_self_intersecting },
    { "path/format-s8",               run_fmt_s8,            check_fmt,               NULL },
    { "path/format-s16",              run_fmt_s16,           check_fmt,               NULL },
    { "path/format-s32",              run_fmt_s32,           check_fmt,               NULL },
    { "path/format-fp32",             run_fmt_f32,           check_fmt,               NULL },
    { "path/format-agreement",        run_fmt_agreement,     check_fmt_agreement,     sum_fmt_agreement },
    { "path/degenerate-zero-area",    run_degenerate,        check_degenerate,        NULL },
};
const size_t vgc_path_case_count =
    sizeof(vgc_path_cases) / sizeof(vgc_path_cases[0]);
