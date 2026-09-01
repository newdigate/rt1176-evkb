/* vgc_cases_path.cpp - paths, contours and winding (spec section 5).
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * The area that produced the ONE-CONTOUR-PER-PATH rule, which is why the spec
 * probes it first -- and MEASURING IT REFUTED THE RULE AS STATED (silicon
 * 2026-08-30, two boots byte-identical; see docs/gc355-vglite-quirks.md and
 * expected_silicon.txt):
 *   - multi-contour-disjoint      BROKEN  runs=1 of 4, fill=1393
 *   - multi-contour-close-padded  ok      runs=4, fill=5120
 *   - two-contour-ring-nonzero    ok      rim=1 centre=0 -- the hole IS cut
 *   - evenodd-vs-nonzero          ok      both fill rules honoured (repeat=differs)
 * So the CLOSE ENCODING is what breaks a DISJOINT multi-contour path (a
 * zero-padded slot carries three VLC_OP_END bytes), while NESTED contours
 * render correctly through the ordinary encoding. A truncate-at-the-first-CLOSE
 * story explains the first line and NEITHER of the last two. The mechanism is
 * NOT identified.
 *
 * What the Phase 1 matrix did not separate is DISJOINT-vs-NESTED from
 * FOUR-contours-vs-TWO -- the four rows above vary both at once. PHASE 1b ADDS
 * THE TWO MISSING CELLS OF THAT 2x2: two-disjoint-bars (disjoint, two) and
 * four-nested-rings (nested, four). BUILT, NOT YET MEASURED -- they are
 * pre-registered in expected_silicon.txt and await a bench boot; do not read
 * the prediction there as a result.
 * Six cases here probe that area directly (multi-contour-disjoint,
 * multi-contour-close-padded, two-disjoint-bars, four-nested-rings,
 * two-contour-ring-nonzero, evenodd-vs-nonzero) and
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
 * honours ALL contours and both fill rules, i.e. a correct GPU) and all thirteen
 * reported ok. That is what says a `broken` from the bench is the silicon and
 * not a mis-placed sample point or a too-tight tolerance. Measured there:
 * rect fill 6400/6400; triangle 1770 against the analytic 1800 (-1.7%, inside
 * the +/-8% bound); bars runs=4 down column 64; star sample points 15.00 px
 * and 2.45 px clear of the nearest edge (perpendicular distance to the rounded
 * integer path, not horizontal).
 *
 * ★ AND THE NEGATIVE ARM, which is the half that matters for an instrument:
 * with the same rasteriser re-broken to drop every contour after the first --
 * this GC355's actual defect -- the probe cases went BROKEN by name
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
 * appears at every vgc_finish_path call site in this file and reads as noise, and because dropping
 * the status is the one thing that must not happen: vgc_finish_path() zeroes
 * *p on overflow, so a caller that carried on would submit path=NULL and
 * measure the harness rather than the GPU. The name carries the `return` so
 * the hidden control flow is visible at the call site. */
#define VGC_FINISH_OR_RETURN(p, x0, y0, x1, y1)                               \
    do {                                                                      \
        const vg_lite_error_t fe_ = vgc_finish_path((p), (x0), (y0), (x1), (y1)); \
        if (fe_ != VG_LITE_SUCCESS) return fe_;                               \
    } while (0)

/* ---- COVERAGE: "the structure is right" is NOT "the picture is right" ------
 *
 * ★★ WHY THIS EXISTS, AND WHY THE TOLERANCES ARE THE SIZE THEY ARE. Two
 * silicon boots (2026-08-30, 2026-09-01) measured this pipeline against exact
 * analytic areas, and the two AXIS-ALIGNED controls came back EXACT:
 *
 *     single-contour-rect          analytic 6400   silicon 6400   excess    0
 *     multi-contour-close-padded   analytic 5120   silicon 5120   excess    0
 *     format-s8/16/32/fp32         analytic 1800   silicon 1830   excess  +30
 *     four-nested-rings            analytic 5760   silicon 6931/6875  +1171/+1115
 *
 * So this GC355 rasterises an axis-aligned integer-coordinate rect with ZERO
 * antialiasing excess, and a diagonal costs ~30 px on an 1800 px triangle.
 * Which makes four-nested-rings' +1171 -- a fifth again of the whole shape --
 * STRAY GEOMETRY: pixels drawn that are not in the path. And it was reporting
 * `pixel=ok`, because its predicate counts RUNS down one column and nothing
 * looked at the amount of ink. Four further cases printed no fill at all, so
 * they could have been doing the same thing invisibly.
 *
 * ★ FOLDED INTO pixel=, NOT ADDED AS A NEW VERDICT FIELD. A render carrying
 * 20% stray geometry IS wrong, so `pixel=ok` has to mean "the picture is
 * right" rather than "the structure is right". The case-line SHAPE is parsed
 * by run_qemu.sh and tools/vglite-conformance-check.sh and must not move; the
 * comparison is reported inside `detail=` as cover=ok / cover=stray:<N> /
 * cover=short:<N> so a failure is diagnosable from a transcript alone.
 *
 * ★ TWO TOLERANCE CLASSES, DERIVED FROM THE MEASUREMENTS ABOVE RATHER THAN
 * CHOSEN:
 *   AXIS-ALIGNED (every edge horizontal or vertical, every coordinate an
 *     integer) -- silicon matches the analytic EXACTLY, proven twice. The
 *     bound is ABSOLUTE: perimeter/8, where the perimeter is the sum of the
 *     EMITTED contours' perimeters (not the rendered region's -- the larger,
 *     more forgiving figure, since an interior contour can still leave a
 *     tessellation seam). For the 80x80 baseline that is 40 px against a
 *     measured excess of 0, and for the nested rings 120 px against a
 *     measured 1171. Generous where the instrument is exact; two orders below
 *     stray scale.
 *   ANTIALIASED (any diagonal or curve) -- the boundary is real and the two
 *     rasterisers straddle the analytic: the host model samples pixel centres
 *     and UNDER-counts (triangle 1770), silicon OVER-counts (1830), against
 *     an analytic 1800. +/-5% = +/-90 holds both with 60 px to spare on each
 *     side and still rejects a 20% excess. 4% would also hold them, at 42 px
 *     of margin; 5% buys the extra without approaching the thing being
 *     caught.
 *
 * ★ COVERAGE IS ONLY MEANINGFUL WHEN THE STRUCTURE IS RIGHT, so it is
 * evaluated ONLY IF the structural predicate passed; otherwise the case
 * reports cover=n/a. That is not a softening -- a structurally wrong case is
 * already BROKEN and cannot be rescued by it -- it is what stops the probe
 * asserting a "correct area" for a picture that is admittedly the wrong
 * picture. path/multi-contour-disjoint and path/two-disjoint-bars are the
 * cases this actually bites on today: both are EXPECTED BROKEN (runs=1 of
 * 4 / of 2), so "the analytic area" would be a claim about a render that did
 * not happen. Their fill is still PRINTED -- it is what surfaced this whole
 * finding -- it is simply not judged.
 *
 * ★ AND IT IS A RULE, NOT A PER-CASE EXCLUSION LIST. Written as a list, a
 * future case that started rendering correctly would keep its exemption
 * silently; written as a rule, the moment two-disjoint-bars reports runs=2 its
 * 2560 px expectation starts being checked. */

typedef struct {
    int  ok;
    char s[24];     /* "cover=stray:" (12) + an int (11) + NUL */
} vgc_cover_t;

/* Not applicable: the structural predicate failed, so there is no correct
 * area to compare against. Never fails a case on its own. */
static vgc_cover_t vgc_cover_na(void)
{
    vgc_cover_t c;
    c.ok = 1;
    snprintf(c.s, sizeof(c.s), "cover=n/a");
    return c;
}

static vgc_cover_t vgc_cover_within(int fill, int expect, int tol)
{
    vgc_cover_t c;
    const int d = fill - expect;
    c.ok = (d >= -tol && d <= tol);
    if (c.ok)       snprintf(c.s, sizeof(c.s), "cover=ok");
    else if (d > 0) snprintf(c.s, sizeof(c.s), "cover=stray:%d", d);
    else            snprintf(c.s, sizeof(c.s), "cover=short:%d", -d);
    return c;
}

/* Axis-aligned integer geometry: absolute bound of perimeter/8. */
static vgc_cover_t vgc_cover_axis(int fill, int expect, int perimeter)
{
    return vgc_cover_within(fill, expect, perimeter / 8);
}

/* Anything with a diagonal or a curve: +/-5% of the analytic area. */
static vgc_cover_t vgc_cover_aa(int fill, int expect)
{
    return vgc_cover_within(fill, expect, expect * 5 / 100);
}

/* ---- 1. path/single-contour-rect ------------------------------------------
 * THE BASELINE. One closed rect, one contour, one draw. 80x80 at (24,24). */

#define R_X 24
#define R_Y 24
#define R_W 80
#define R_H 80
#define R_AREA (R_W * R_H)              /* 80*80        = 6400 */
#define R_PERIM (2 * (R_W + R_H))       /* 2*(80+80)    =  320 -> tol 40 */

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
    const int structural = centre && !corner;
    /* ★ THE FILL BOUND USED TO BE +/-3% HERE AND IS NOW THE COVERAGE CHECK'S
     * +/-40, and the history is worth keeping because the direction of travel
     * has been one way. It started at +/-6% (384 px) and was tightened to 3%
     * (192 px) on the argument that a baseline must be the TIGHTEST case in
     * the matrix rather than the loosest -- 6% admits a rect rendered FOUR
     * PIXELS TOO NARROW (76*80 = 6080), and this is the case every other
     * verdict leans on. Both bands were sized for an antialiased 320 px
     * perimeter landing either side of the 50% threshold. THE SILICON SAYS
     * THAT NEVER HAPPENS: two boots measured 6400 against an analytic 6400,
     * excess zero. So the AA argument does not apply to axis-aligned integer
     * geometry at all, and R_PERIM/8 = 40 px is the honest bound -- still four
     * times the largest excess this pipeline has ever shown on such a shape
     * (the +30 on a diagonal), and it narrows the smallest detectable error
     * from 2 px of width to under half a pixel. The far-wrong shapes were
     * never the question: a dropped draw is 0, a full-target fill 16384. */
    const vgc_cover_t cv = structural ? vgc_cover_axis(fill, R_AREA, R_PERIM)
                                      : vgc_cover_na();
    snprintf(d, n, "fill=%d,expect=%d,centre=%d,corner=%d,%s",
             fill, R_AREA, centre, corner, cv.s);
    return (structural && cv.ok) ? VGC_OK : VGC_BROKEN;
}

/* ---- 2. path/multi-contour-disjoint ----------------------------------------
 * Four separated bars in ONE path. Count filled runs down column x=64.
 * EXPECTED BROKEN on this GC355: runs=1 (only the first contour renders). */

static const int BAR_Y[4] = { 16, 44, 72, 100 };
#define BAR_H 16
#define BAR_AREA  (R_W * BAR_H)                 /* 80*16              = 1280 */
#define BAR_PERIM (2 * (R_W + BAR_H))           /* 2*(80+16)          =  192 */
#define BARS4_AREA  (4 * BAR_AREA)              /* four bars          = 5120 */
#define BARS4_PERIM (4 * BAR_PERIM)             /* 4*192 =  768 -> tol   96 */
#define BARS2_AREA  (2 * BAR_AREA)              /* two bars           = 2560 */
#define BARS2_PERIM (2 * BAR_PERIM)             /* 2*192 =  384 -> tol   48 */

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
    /* ★ y0/y1 SAY *WHICH* BAR SURVIVED, and that was missing from the silicon
     * reading. Measured 2026-08-30 this cell was runs=1,fill=1393 and nothing
     * in it distinguished "bar 0 rendered" from a partial render straddling
     * two bars -- fill~1280 is one bar's worth either way. Bar i occupies
     * BAR_Y[i]..BAR_Y[i]+BAR_H, i.e. 16, 44, 72, 100, so the extent names it.
     *
     * FREE, in the sense that matters here: vgc_filled_rows returns the SAME
     * total filled-pixel count vgc_count_filled did, so `fill=` is unchanged
     * in meaning and still comparable with the recorded 1393 -- and `detail=`
     * is not part of the expectation at all (tools/vglite-conformance-check.sh
     * parses only pixel= and repeat=), so this cannot move a golden.
     *
     * Seeded -99 rather than -1: vgc_filled_rows leaves both out-params
     * UNTOUCHED when nothing is filled, and its own note records that a -1
     * seed makes "untouched" indistinguishable from the internal state it
     * tracks. Same convention as check_degenerate below. */
    int ymin = -99, ymax = -99;
    const int fill = vgc_filled_rows(vgc_fb(), VGC_W, VGC_H, VGC_W, &ymin, &ymax);
    /* ★ COVERAGE ONLY WHEN runs==4, WHICH IS WHY multi-contour-disjoint READS
     * cover=n/a ON THIS SILICON AND ITS PADDED TWIN DOES NOT. The disjoint
     * cell is EXPECTED BROKEN (runs=1: one bar of four, measured fill 1393
     * against bar 0's 1280), and "the correct area" of a render that admits
     * it drew the wrong picture is not a quantity. The padded cell renders all
     * four (measured 5120 == the analytic exactly), so its area IS well
     * defined and is now checked. Same function, same expectation, different
     * answer -- because the rule is about the STRUCTURE, not about the id. */
    const vgc_cover_t cv = (runs == 4)
        ? vgc_cover_axis(fill, BARS4_AREA, BARS4_PERIM) : vgc_cover_na();
    snprintf(d, n, "runs=%d,expect=4,fill=%d,y0=%d,y1=%d,%s",
             runs, fill, ymin, ymax, cv.s);
    return (runs == 4 && cv.ok) ? VGC_OK : VGC_BROKEN;
}

/* ---- 3. path/multi-contour-close-padded ------------------------------------
 * ★ A DISCRIMINATOR BETWEEN TWO HYPOTHESES, NOT A PROBE OF A FEATURE, and it
 * is the most valuable cell in this matrix if it disagrees with case 2.
 *
 * The tree's recorded rule is "the GC355 renders only the FIRST contour of a
 * path". That was only ever measured on ONE encoding -- the raw arena words
 * this file's other cases emit. There is a narrower rule that fits the same
 * evidence, and NXP's own driver thinks it is the real one:
 *
 *   vg_lite_path.c:556-570, inside vg_lite_append_path(), guarded by
 *   `#if (CHIPID == 0x355)` -- and CHIPID IS 0x355 for our Series
 *   (VGLite/Series/gc355/0x0_1216/vg_lite_options.h:31, which evkb.cmake
 *   selects) -- special-cases EXACTLY "a CLOSE at a contour boundary"
 *   (`cmd[i] == VLC_OP_CLOSE && cmd[i+1] == VLC_OP_MOVE`) and writes that
 *   CLOSE as 0x01010101 across the whole 4-byte element instead of as a
 *   single byte.
 *
 * An opcode is one byte at the base of a slot and the rest is padding (see the
 * format-* note below), so the arena's ordinary CLOSE is `01 00 00 00`.
 * MEASURED, by dumping the arena rather than reading the source:
 *
 *     slot 12 @ 48 : 01 00 00 00   CLOSE, then three 0x00 bytes
 *     slot 13 @ 52 : 02 00 00 00   MOVE -- the next contour
 *
 * and VLC_OP_END is 0x00. So every contour boundary in cases 2, 4 and 6 is a
 * CLOSE followed by three END bytes inside its own slot. That is a complete
 * and mundane mechanism for "the path stopped after the first contour", and it
 * is the one the vendor gates on this exact chip.
 *
 * This case is case 2's geometry EXACTLY -- same four bars, same coordinates,
 * same predicate, same column -- with the ONLY variable the contour-boundary
 * CLOSE encoding. So:
 *   ok here + broken in case 2  =>  the rule is not "one contour per path" at
 *       all, it is "a CLOSE slot padded with 0x00 terminates the path"; both
 *       shipping compositors are working around something they could simply
 *       encode away, and docs/gc355-vglite-quirks.md must say the narrow thing.
 *   broken in both              =>  the one-contour rule is confirmed on a
 *       SECOND encoding and is that much stronger.
 * Either answer is worth the case; there is no outcome in which it says
 * nothing. It has to ride the same boot as case 2 because the bench boot is
 * one hand-pressed button.
 *
 * ★ THE BYTES ARE BUILT HERE, NOT OBTAINED FROM vg_lite_append_path(). That
 * function reads cmd[i + 1] at i == seg_count - 1 (vg_lite_path.c:557), one
 * past the end of the caller's array -- so the vendor's own workaround is
 * reachable only through an out-of-bounds read. Nothing in this tree calls it
 * (grepped: the only hits are LVGL's ThorVG shim, which IMPLEMENTS the API for
 * a PC simulator and is not built here), which is also why the padded encoding
 * has never been exercised on this board.
 *
 * ★ THE HOST GEOMETRY SUITE CANNOT ANSWER THIS. Both of its model rasterisers
 * read the opcode byte and ignore the slot's padding, so this case behaves
 * there exactly like case 2 -- ok under a correct model, broken under the
 * first-contour model. That agreement is a check that the two cases really are
 * one variable apart; it is NOT evidence about the padding. Only silicon can
 * separate the hypotheses. */

/* VLC_OP_CLOSE with its three pad bytes set to 0x01 instead of 0x00, as an S32
 * path element. Spelled as one constant because it is a byte pattern rather
 * than an arithmetic value, and because 0x01010101 is what the vendor's source
 * literally writes. */
#define VGC_CLOSE_PADDED_S32 ((int32_t)0x01010101)

/* vgc_emit_rect_cw's contour with the padded terminator. Local to this file
 * rather than added to the arena: the arena's API already expresses it --
 * vgc_emit() takes the raw word -- so no seam was needed, and a shared
 * vgc_emit_rect_cw_padded() would invite Phase 2 and 3 authors to adopt an
 * encoding whose behaviour is exactly what is still in question. */
static void emit_bar_close_padded(int32_t x, int32_t y, int32_t w, int32_t h)
{
    vgc_emit(VLC_OP_MOVE); vgc_emit(x);     vgc_emit(y);
    vgc_emit(VLC_OP_LINE); vgc_emit(x + w); vgc_emit(y);
    vgc_emit(VLC_OP_LINE); vgc_emit(x + w); vgc_emit(y + h);
    vgc_emit(VLC_OP_LINE); vgc_emit(x);     vgc_emit(y + h);
    vgc_emit(VGC_CLOSE_PADDED_S32);
}

static vg_lite_error_t run_multi_contour_padded(void)
{
    vg_lite_error_t acc = VG_LITE_SUCCESS;
    vg_lite_path_t p;
    /* ★ ONLY THE BOUNDARY CLOSES ARE PADDED -- bars 0..2, whose CLOSE is
     * followed by a MOVE. The LAST bar's CLOSE is followed by the END that
     * vgc_finish_path appends, so it is not at a contour boundary and the
     * vendor's condition would not fire on it either. Padding it as well would
     * be a second, unasked variable in a case whose whole value is having
     * exactly one. */
    for (int i = 0; i < 3; i++) emit_bar_close_padded(R_X, BAR_Y[i], R_W, BAR_H);
    vgc_emit_rect_cw(R_X, BAR_Y[3], R_W, BAR_H);
    VGC_FINISH_OR_RETURN(&p, R_X, BAR_Y[0], R_X + R_W, BAR_Y[3] + BAR_H);
    vgc_draw_path(&p, VG_LITE_FILL_NON_ZERO, VGC_FILL_COLOR, &acc);
    vgc_finish_into(&acc);
    return acc;
}

/* ---- 3b. path/two-disjoint-bars --------------------------------------------
 * ★ ONE HALF OF THE PHASE 1b 2x2. This is path/multi-contour-disjoint with two
 * bars DELETED -- same x-extent, same bar height, same sample column, same
 * ordinary CLOSE encoding, same predicate shape. The ONLY variable is the
 * contour COUNT.
 *
 * Phase 1 left two variables confounded: four DISJOINT contours broke
 * (runs=1 of 4) while two NESTED ones rendered fine, so "disjointness" and
 * "more than two" both fit the evidence equally. Read this WITH
 * path/four-nested-rings:
 *   this broken + rings ok  =>  DISJOINTNESS is the variable, count is not
 *   this ok + rings broken  =>  COUNT is the variable, disjointness is not
 *   both broken             =>  nesting protects only at two contours
 *   both ok                 =>  only the four-disjoint COMBINATION breaks
 * Every cell of that table names a different rule, so there is no outcome in
 * which the pair says nothing.
 *
 * Bars 0 and 2 of the existing four (y=16 and y=72), so the gap between them
 * is 56 px -- adjacency cannot be confused with a merge, and the sampled
 * column crosses background between them either way. */
static vg_lite_error_t run_two_disjoint(void)
{
    vg_lite_error_t acc = VG_LITE_SUCCESS;
    vg_lite_path_t p;
    vgc_emit_rect_cw(R_X, BAR_Y[0], R_W, BAR_H);
    vgc_emit_rect_cw(R_X, BAR_Y[2], R_W, BAR_H);
    VGC_FINISH_OR_RETURN(&p, R_X, BAR_Y[0], R_X + R_W, BAR_Y[2] + BAR_H);
    vgc_draw_path(&p, VG_LITE_FILL_NON_ZERO, VGC_FILL_COLOR, &acc);
    vgc_finish_into(&acc);
    return acc;
}

static vgc_verdict_t check_two_disjoint(char *d, size_t n)
{
    /* runs COUNTS surviving contours here: 2 = both, 1 = only the first.
     * fill= discriminates the failure modes exactly as it does in case 2 --
     * one bar is 1280, both are 2560, a merge spanning y 16..88 would be far
     * larger. Verified on the host against a winding-number rasteriser before
     * this reached a GPU: k=1 gives runs=1 fill=1280, k=2 gives runs=2
     * fill=2560 with the column filled over [16,32) and [72,88). */
    const int runs = vgc_count_runs_col(vgc_fb(), VGC_W, VGC_H, VGC_W, 64);
    const int fill = vgc_count_filled(vgc_fb(), VGC_W, VGC_H, VGC_W);
    /* cover=n/a on this silicon for the same reason as its four-bar parent:
     * runs=1, so the picture is admittedly wrong and 2560 is not its area.
     * The measured 1322 is still printed -- and it is 42 px ABOVE bar 0's
     * exact 1280 in a path whose only other content is a bar 56 px away,
     * which is a reading worth having whether or not anything judges it. */
    const vgc_cover_t cv = (runs == 2)
        ? vgc_cover_axis(fill, BARS2_AREA, BARS2_PERIM) : vgc_cover_na();
    snprintf(d, n, "runs=%d,expect=2,fill=%d,%s", runs, fill, cv.s);
    return (runs == 2 && cv.ok) ? VGC_OK : VGC_BROKEN;
}

/* ---- 3c. path/four-nested-rings --------------------------------------------
 * ★ THE OTHER HALF OF THE 2x2, and its predicate is a COUNTER rather than a
 * pass/fail -- which is what makes it worth more than a yes/no. It does not
 * merely say whether nesting survives at four contours; it reports HOW MANY
 * contours the GPU honoured.
 *
 * Four concentric rects with ALTERNATING winding under NON_ZERO. Down column
 * x=64 the winding number runs 1,0,1,0,1,0,1, so a correct render shows FOUR
 * separated filled bands of 12 px each. If only the first k contours survive,
 * exactly k runs appear:
 *   1 => one solid 96x96 block (outer only), fill 9216
 *   2 => one ring,                            fill 4032
 *   3 => ring plus a solid core,              fill 6336
 *   4 => correct,                             fill 5760
 *
 * ★ VERIFIED ON THE HOST BEFORE IT REACHED A GPU, against a winding-number
 * rasteriser independent of the tests/ model, measured with these same
 * predicates: all four rects contain x=64 horizontally (spans 16..112,
 * 28..100, 40..88, 52..76); the bands are [16,28) [40,52) [76,88) [100,112);
 * runs came out 1, 2, 3, 4 for k = 1, 2, 3, 4; and the k=4 fill matched the
 * analytic (96^2-72^2) + (48^2-24^2) = 4032 + 1728 = 5760 exactly. A wrong
 * predicate here would fabricate a GC355 finding, which is the top risk this
 * example exists to avoid, so the arithmetic was re-derived rather than
 * carried over from the plan.
 *
 * Arena cost: 4 rects x 13 words + END = 53 of VGC_ARENA_WORDS (512). */
#define NR_N 4
static const int NR_XY[NR_N] = { 16, 28, 40, 52 };   /* origin of each rect */
static const int NR_WH[NR_N] = { 96, 72, 48, 24 };   /* and its side */
/* (96^2 - 72^2) + (48^2 - 24^2) = 4032 + 1728. */
#define NR_AREA  5760
/* Sum of the four EMITTED contours: 4*(96+72+48+24) = 960 -> tol 120. */
#define NR_PERIM (4 * (96 + 72 + 48 + 24))

static vg_lite_error_t run_four_nested(void)
{
    vg_lite_error_t acc = VG_LITE_SUCCESS;
    vg_lite_path_t p;
    for (int i = 0; i < NR_N; i++) {
        /* Alternating winding IS the instrument: without it every contour
         * contributes +1 and the whole 96x96 fills solid under NON_ZERO,
         * collapsing the counter to runs=1 for a correct GPU. */
        if (i & 1) vgc_emit_rect_ccw(NR_XY[i], NR_XY[i], NR_WH[i], NR_WH[i]);
        else       vgc_emit_rect_cw (NR_XY[i], NR_XY[i], NR_WH[i], NR_WH[i]);
    }
    VGC_FINISH_OR_RETURN(&p, NR_XY[0], NR_XY[0],
                         NR_XY[0] + NR_WH[0], NR_XY[0] + NR_WH[0]);
    vgc_draw_path(&p, VG_LITE_FILL_NON_ZERO, VGC_FILL_COLOR, &acc);
    vgc_finish_into(&acc);
    return acc;
}

static vgc_verdict_t check_four_nested(char *d, size_t n)
{
    const int runs = vgc_count_runs_col(vgc_fb(), VGC_W, VGC_H, VGC_W, 64);
    const int fill = vgc_count_filled(vgc_fb(), VGC_W, VGC_H, VGC_W);
    /* ★★ expfill= WAS A READING AND IS NOW A BOUND, AND THIS IS THE CASE THAT
     * FORCED THE WHOLE COVERAGE CHECK. The comment here used to say the
     * verdict is runs alone "since antialiasing moves fill by a perimeter's
     * worth and this case must not go broken on that". Two silicon boots
     * refuted the premise: this pipeline draws axis-aligned integer rects with
     * ZERO antialiasing excess (rect 6400/6400, four bars 5120/5120), while
     * THIS case measured 6931 and 6875 against the analytic 5760 -- +1171 and
     * +1115, a fifth again of the shape, and differing between boots. That is
     * not a perimeter's worth of AA; it is geometry the path does not contain.
     * And it was reporting pixel=ok, because runs=4 is exactly what a correct
     * render and a correct-render-plus-stray-ink both produce.
     *
     * NR_PERIM/8 = 120 px. Both silicon readings are ~10x outside it. */
    const vgc_cover_t cv = (runs == 4)
        ? vgc_cover_axis(fill, NR_AREA, NR_PERIM) : vgc_cover_na();
    snprintf(d, n, "runs=%d,expect=4,fill=%d,expfill=%d,%s",
             runs, fill, NR_AREA, cv.s);
    return (runs == 4 && cv.ok) ? VGC_OK : VGC_BROKEN;
}

/* ---- 4. path/two-contour-ring-nonzero --------------------------------------
 * Outer CW + reversed inner CCW in ONE path under NON_ZERO: the classic hole.
 * EXPECTED BROKEN: the inner contour is dropped and the ring fills solid. */

#define I_X 48
#define I_Y 48
#define I_W 32
#define I_H 32
/* The ring both cases 4 and 5 produce: 80*80 - 32*32 = 6400 - 1024. */
#define RING_AREA  (R_AREA - (I_W * I_H))
/* Both contours emitted: 2*(80+80) + 2*(32+32) = 320 + 128 = 448 -> tol 56.
 * The RENDERED region's boundary is the same 448 here, since the hole is a
 * real edge; where the two differ this file takes the emitted figure, which
 * is never the smaller. */
#define RING_PERIM (R_PERIM + 2 * (I_W + I_H))

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

/* Shared by cases 4 and 5: rim filled AND centre background is the ring.
 * (32,64) is inside the outer plate (24..104) and clear of the inner one
 * (48..80) by 16 px; (64,64) is the inner plate's centre. */
static vgc_verdict_t check_ring(char *d, size_t n)
{
    const int rim    = vgc_is_filled(vgc_px(32, 64));   /* inside outer, outside inner */
    const int centre = vgc_is_filled(vgc_px(64, 64));   /* inside inner */
    const int structural = rim && !centre;
    /* ★ THIS CASE PRINTED NO FILL AT ALL UNTIL NOW, and neither did case 5,
     * evenodd-vs-nonzero or self-intersecting. Two sample points say the hole
     * is cut SOMEWHERE and the rim is inked SOMEWHERE; they cannot see a ring
     * a fifth too heavy, which is exactly what four-nested-rings turned out to
     * be while reporting a clean structural verdict. There is no silicon
     * reading for this cell to compare against -- the number did not exist --
     * so the next boot measures it for the first time. */
    const int fill = vgc_count_filled(vgc_fb(), VGC_W, VGC_H, VGC_W);
    const vgc_cover_t cv = structural
        ? vgc_cover_axis(fill, RING_AREA, RING_PERIM) : vgc_cover_na();
    snprintf(d, n, "rim=%d,centre=%d,expect=rim1centre0,fill=%d,%s",
             rim, centre, fill, cv.s);
    return (structural && cv.ok) ? VGC_OK : VGC_BROKEN;
}

/* ---- 5. path/two-draws-ring ------------------------------------------------
 * THE SAFE-USAGE CONTROL for case 4. Same ring, built as a filled plate with
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
 * 7 and 12. Here the two draws compose ONE picture and the second is meant to
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

/* ---- 6. path/evenodd-vs-nonzero --------------------------------------------
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
    const int structural = s_eo_rim && !s_eo_centre && nz_rim && nz_centre;
    /* ★ WHICH SUB-RENDER THIS MEASURES: PASS 2, the NON_ZERO one, because
     * that is the only render still in the buffer when the harness calls
     * check() (pass 1 was sampled into the s_eo_* statics and then CLEARED
     * away by run() itself). Under NON_ZERO two SAME-winding nested rects both
     * contribute +1, so the hole is not cut and the correct picture is the
     * outer 80x80 SOLID: 6400, not the ring. The tolerance is nevertheless
     * built from BOTH emitted contours (RING_PERIM = 448 -> 56), since the
     * inner contour is drawn and can leave a tessellation seam even where it
     * cuts nothing.
     *
     * ★ NOTE WHAT IS NOT COVERED: pass 1's EVEN_ODD area (which should be the
     * 5376 ring). Measuring it would mean stashing a fifth number in a file
     * static, and this case's job is the fill-RULE difference, which the four
     * sample points already answer. The gap is named rather than hidden. */
    const int fill = vgc_count_filled(vgc_fb(), VGC_W, VGC_H, VGC_W);
    const vgc_cover_t cv = structural
        ? vgc_cover_axis(fill, R_AREA, RING_PERIM) : vgc_cover_na();
    snprintf(d, n, "eo_rim=%d,eo_centre=%d,nz_rim=%d,nz_centre=%d,fill=%d,%s",
             s_eo_rim, s_eo_centre, nz_rim, nz_centre, fill, cv.s);
    return (structural && cv.ok) ? VGC_OK : VGC_BROKEN;
}

/* ---- 7. path/self-intersecting ---------------------------------------------
 * A pentagram: ONE contour that crosses itself. The centre pentagon is EMPTY
 * under EVEN_ODD (crossing number 2) and FILLED under NON_ZERO (winding 2),
 * and the five tips are filled under both. This is the case that distinguishes
 * the two fill rules on a single contour -- so unlike case 6 it should pass on
 * this GC355, which makes it case 6's control as well as its own probe.
 *
 * Vertices: r=50 about (64,64), at -90 + k*144 degrees, k = 0..4, connected in
 * that order. Integers, rounded once here so the geometry is fixed.
 *
 * ★ THE TWO SAMPLE POINTS HAVE MARGIN, WHICH MATTERS BECAUSE THIS IS A
 * CONTROL -- a false BROKEN here would wrongly discredit case 6, one of the
 * three headline probes. The margins are ANALYTIC: perpendicular distance from
 * the sample to the nearest of the five edges of the ROUNDED integer path.
 * Centre (64,64) is 15.00 px clear (edge V2-V3); tip (64,40) is 7.97 px clear
 * (edge V0-V1).
 *
 * ★ THE TIP SAMPLE IS (64,40), NOT THE (64,22) THIS STARTED AT. Both lie in
 * the top spike with crossing number 1 and winding 1, so the case asks exactly
 * the same question -- but 22 is only 2.45 px from an edge, which was the
 * tightest point in the whole matrix and sitting on a CONTROL. Sweeping the
 * axis of symmetry, clearance peaks at y=40: 2.45 px at 22, 4.91 at 30, 7.97
 * at 40, 5.00 at 44. That is 3.3x the margin for nothing, and 40 is still
 * 8.55 px above the inner pentagon's upper vertices at y=48.55, so it cannot
 * stray into the region this case requires to be EMPTY under EVEN_ODD.
 *
 * ★ AND NOTE WHAT THE HOST SUITE CANNOT SAY ABOUT THIS. Its rasteriser has no
 * antialiasing and samples at pixel centres, so it confirms INSIDE/OUTSIDE and
 * nothing more -- it is structurally unable to tell whether 2.45 px survives a
 * real AA boundary or a half-pixel raster offset, which is the only question
 * a margin is about. That is why the number is defended analytically here
 * rather than by pointing at a green test. */

static const int STAR[5][2] = {
    {  64,  14 },   /* -90 deg */
    {  93, 104 },   /*  54    */
    {  16,  49 },   /* 198    */
    { 112,  49 },   /* 342    */
    {  35, 104 },   /* 126    */
};

/* ★ THE NON_ZERO AREA OF THE ROUNDED-INTEGER PENTAGRAM, DERIVED EXACTLY --
 * not taken from the model rasteriser, though the two agree to a third of a
 * pixel, which is the point of saying so.
 *
 * The shoelace of a self-crossing polygon integrates the WINDING NUMBER, so
 * for a pentagram it counts the inner pentagon TWICE (winding 2) and the five
 * points once. The NON_ZERO region is every point of winding != 0, i.e. the
 * points plus the inner pentagon ONCE:
 *
 *     |shoelace(V0..V4)|                                    = 3655
 *     inner pentagon (the five non-adjacent edge crossings) =  862.70
 *     NON_ZERO area = 3655 - 862.70                         = 2792.30
 *
 * The five crossings are computed from the ROUNDED integer vertices actually
 * emitted -- (75.278,49), (82.148,70.323), (64,83.286), (45.852,70.323),
 * (52.722,49) -- so this is the area of the shape the case really draws, not
 * of the ideal r=50 pentagram it was derived from. The suite's pixel-centre
 * model rasteriser independently reports 2792 for the same shape.
 *
 * ★ ANTIALIASED CLASS: every edge here is a diagonal, so the +/-5% band (139
 * px) applies rather than a perimeter bound. The 474 px of emitted edge is
 * the longest boundary in the matrix, which is precisely why an absolute
 * bound derived from axis-aligned measurements would be the wrong instrument.
 * EVEN_ODD's area would be 3655 - 2*862.70 = 1929.60 -- not checked here, for
 * the same reason as evenodd-vs-nonzero: pass 1 is cleared before check(). */
#define STAR_NZ_AREA 2792

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
    s_star_eo_tip    = vgc_is_filled(vgc_px(64, 40));   /* inside the top point */
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
    const int nz_tip    = vgc_is_filled(vgc_px(64, 40));
    const int structural = !s_star_eo_centre && s_star_eo_tip
                           && nz_centre && nz_tip;
    /* Pass 2 (NON_ZERO) is what is in the buffer -- see STAR_NZ_AREA. */
    const int fill = vgc_count_filled(vgc_fb(), VGC_W, VGC_H, VGC_W);
    const vgc_cover_t cv = structural
        ? vgc_cover_aa(fill, STAR_NZ_AREA) : vgc_cover_na();
    snprintf(d, n, "eo_centre=%d,eo_tip=%d,nz_centre=%d,nz_tip=%d,fill=%d,%s",
             s_star_eo_centre, s_star_eo_tip, nz_centre, nz_tip, fill, cv.s);
    return (structural && cv.ok) ? VGC_OK : VGC_BROKEN;
}

/* ---- 8-12. path/format-* ---------------------------------------------------
 * The SAME right triangle in each of the four path coordinate formats. The
 * driver reads path data as an array of the FORMAT's element width -- opcodes
 * included -- so each format needs its own typed array rather than a shared
 * int32_t arena.
 *
 * Vertices (10,10),(70,10),(10,70): area 1800 px, all of them small positive
 * integers so VG_LITE_S8 can hold them. Note what that does NOT cover: every
 * coordinate here is 10 or 70, so a sign-extension bug in the narrow formats
 * is invisible to these cases and they must not be read as evidence about one.
 * Negative coordinates are a clipping question and belong to Phase 3. Each
 * format's own case checks its fill count against the analytic area, so each
 * stands alone; path/format-agreement then checks all four against EACH OTHER,
 * which is the cross-format question the per-format cases cannot ask.
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
    /* ★ AREA ALONE DOES NOT SAY "THE RIGHT TRIANGLE", it says "roughly the
     * right amount of ink" -- and a mirrored or transposed triangle has
     * exactly the same area, so a format whose coordinates decoded x and y the
     * wrong way round would pass a count-only check. Two samples pin the
     * ORIENTATION against the hypotenuse x+y=80: (20,20) is deep inside
     * (sum 41), (60,60) well outside (sum 121), and no reflection of this
     * triangle about either axis or the diagonal satisfies both. Cheap, and it
     * turns "some shape of the right size" into "this shape". */
    const int in  = vgc_is_filled(vgc_px(20, 20));
    const int out = vgc_is_filled(vgc_px(60, 60));
    /* ★ +/-8% TIGHTENED TO +/-5%, AND THE JUSTIFICATION IS NOW A MEASUREMENT
     * RATHER THAN AN ESTIMATE. 8% was sized from "the triangle's perimeter is
     * ~205 px, so a pixel of antialiased boundary either way is ~5.7%". Both
     * rasterisers have since been read: the host model under-counts at 1770
     * (-1.7%, pixel-centre sampling) and silicon over-counts at 1830 (+1.7%),
     * against the analytic 1800. The real spread is 60 px, not 205. +/-5% =
     * +/-90 holds both with 60 px of margin on each side, and -- unlike 8% =
     * 144 -- it is comfortably below the ~20% stray-geometry excess that
     * four-nested-rings turned out to be carrying. This is the ANTIALIASED
     * class: the hypotenuse is a real diagonal, so the perimeter/8 bound the
     * axis-aligned cases use would be the wrong instrument here. */
    const vgc_cover_t cv = (in && !out) ? vgc_cover_aa(fill, TRI_AREA)
                                        : vgc_cover_na();
    snprintf(d, n, "fill=%d,expect=%d,in=%d,out=%d,%s",
             fill, TRI_AREA, in, out, cv.s);
    return (in && !out && cv.ok) ? VGC_OK : VGC_BROKEN;
}

/* path/format-agreement: renders all four again, in one run, and compares.
 * It does NOT read any state left by the four cases above -- doing so would
 * make this case's answer depend on the table's ORDER, which is exactly the
 * coupling the one-case-at-a-time design forbids. Renders four times, so it
 * clears between them and supplies its own sum(). */

static int      s_agree[4];
static uint32_t s_agree_sum;
/* ★ THE PER-FORMAT FULL-BUFFER CHECKSUMS WERE BEING COMPUTED AND THROWN AWAY.
 * run() already hashes each render; folding them only into s_agree_sum spends
 * them on repeat= and leaves the check comparing four INTEGERS. Four renders
 * each wrong the same way agree perfectly on a count, and so do four different
 * shapes of equal area -- so a count-only agreement is far weaker than data
 * already in hand.
 *
 * ★ REPORTED, NEVER ASSERTED, and that is deliberate rather than timid: FP32
 * may legitimately reach the same shape down a different precision path, so
 * requiring bit-equality would manufacture a `broken` out of a correct GPU. As
 * a reported field it is strictly additive -- same_px=1 on silicon settles
 * cross-format agreement far more strongly than four equal integers ever
 * could, and same_px=0 alongside four EQUAL counts is itself a finding worth
 * having in the transcript. Note the null case it CANNOT distinguish: four
 * empty buffers are also bit-identical, which is why the zero-count test below
 * is what actually fails that render. */
static int      s_agree_same_px;

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
    s_agree_same_px = (sums[1] == sums[0] && sums[2] == sums[0] && sums[3] == sums[0]);
    s_agree_sum = vgc_fnv(sums, sizeof(sums));
    return acc;
}

static uint32_t sum_fmt_agreement(void) { return s_agree_sum; }

static vgc_verdict_t check_fmt_agreement(char *d, size_t n)
{
    /* NON-ZERO FIRST, and it is not belt-and-braces: four EMPTY renders agree
     * perfectly, so equality alone makes this case green on a GPU that draws
     * nothing at all. Testing slot 0 here and equality below gives each test
     * one job -- the two folded into one loop condition left the slot-0 check
     * unreachable, since equality already implies it. */
    int structural = (s_agree[0] != 0);
    for (int i = 1; i < 4; i++)
        if (s_agree[i] != s_agree[0]) structural = 0;

    /* ★ COVERAGE APPLIES TO ALL FOUR SUB-RENDERS, NOT ONLY THE LAST ONE IN
     * THE BUFFER, and here that is free: run() already captured every count.
     * It is also where coverage adds the most to this particular case --
     * "the four formats agree" is satisfied by four IDENTICALLY WRONG renders,
     * which is the one failure mode the equality test structurally cannot see.
     * The worst offender is reported, so a transcript names the size of the
     * error rather than only its existence. ANTIALIASED class: same triangle
     * as the four cases above, +/-5% of 1800. */
    vgc_cover_t cv = vgc_cover_na();
    if (structural) {
        int worst = 0;                          /* index of the worst offender */
        for (int i = 1; i < 4; i++) {
            const int a = s_agree[i]     - TRI_AREA;
            const int b = s_agree[worst] - TRI_AREA;
            if ((a < 0 ? -a : a) > (b < 0 ? -b : b)) worst = i;
        }
        cv = vgc_cover_aa(s_agree[worst], TRI_AREA);
    }
    /* 67 bytes worst case against VGC_DETAIL_MAX's 96, counted rather than
     * hoped: five %d each bounded by the 16384-pixel target (5 digits), plus
     * the longest cover token, "cover=stray:16384" (17). */
    snprintf(d, n, "s8=%d,s16=%d,s32=%d,fp32=%d,same_px=%d,%s",
             s_agree[0], s_agree[1], s_agree[2], s_agree[3], s_agree_same_px,
             cv.s);
    return (structural && cv.ok) ? VGC_OK : VGC_BROKEN;
}

/* ---- 13. path/degenerate-zero-area -----------------------------------------
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
    /* ★ THE ONE CASE WITH NO ANALYTIC AREA AT ALL, and it is a design fact
     * rather than an omission: a zero-height rect may legitimately rasterise
     * to NOTHING or to a hairline, so no single number is the correct answer
     * and there is nothing for coverage to compare against. Printed as n/a
     * anyway so that EVERY case line in the matrix carries a cover= field --
     * a grep for cases lacking one would otherwise be a grep for nothing. */
    snprintf(d, n, "fill=%d,ymin=%d,ymax=%d,cover=n/a", fill, ymin, ymax);
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
    /* Same geometry and the SAME predicate as the line above; the only
     * variable is the contour-boundary CLOSE encoding. Adjacent on purpose --
     * the pair is read as one answer. */
    { "path/multi-contour-close-padded", run_multi_contour_padded, check_multi_contour,  NULL },
    /* The Phase 1b 2x2. These two separate DISJOINT-vs-NESTED from
     * FOUR-vs-TWO, which the four lines above leave confounded, and they are
     * placed here so every contour-encoding case sits together and is read as
     * one answer. See their case comments for the outcome table. */
    { "path/two-disjoint-bars",       run_two_disjoint,      check_two_disjoint,      NULL },
    { "path/four-nested-rings",       run_four_nested,       check_four_nested,       NULL },
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
