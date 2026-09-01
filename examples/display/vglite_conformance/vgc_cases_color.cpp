/* vgc_cases_color.cpp - colour and blend cases (Phase 2 spec section 3).
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Spec: docs/superpowers/specs/2026-09-01-gc355-conformance-phase2-design.md
 *
 * ★★ WHY THIS FILE EXISTS AT ALL. All fifteen Phase 1 cases render with
 * VG_LITE_BLEND_NONE and an OPAQUE colour. Both shipping compositors
 * (synthui_rotary_knob_gpu.cpp, synthui_fader_gpu.cpp) use
 * VG_LITE_BLEND_SRC_OVER exclusively -- twelve call sites between them. So the
 * matrix has never once tested the blend mode production depends on, and it
 * has never passed a non-opaque colour through the mode it DOES test. These
 * five cases close both gaps.
 *
 * ★★ AND THE CENTRAL FACT ABOUT THEM: THE DRIVER'S OWN HEADER IS INTERNALLY
 * INCONSISTENT ABOUT WHAT SRC_OVER MEANS, so cases 2-4 must not pre-judge it.
 * Read verbatim from ~/Development/VGLite/inc/vg_lite.h (line numbers are
 * grep -n on that file, checked here, not copied from the spec):
 *
 *   :452  "S and D represent source and destination NON-PREMULTIPLIED RGB
 *          color channels."
 *   :458  section heading: "Non-premultiplied Blending modes"
 *   :459  VG_LITE_BLEND_NONE        = 0     RGB: S, No blend   A: Sa
 *   :461  VG_LITE_BLEND_SRC_OVER    = 1     RGB: S + D*(1 - Sa)
 *   :481  VG_LITE_BLEND_NORMAL_LVGL = 11    RGB: S*Sa + D*(1 - Sa)
 *   :137  #define VG_LITE_BLEND_PREMULTIPLY_SRC_OVER VG_LITE_BLEND_NORMAL_LVGL
 *
 * The NAMES and the FORMULAS are inverted against each other: the mode filed
 * under "non-premultiplied" carries the PREmultiplied operator (no `*Sa`
 * term), and the one aliased "PREMULTIPLY" carries the NON-premultiplied one.
 * Two readings of mode 1 are therefore both defensible, and this file labels
 * them exactly as the spec and tests/model.h do -- A and B swapped across
 * documents is the divergence the shared model exists to prevent:
 *
 *   reading A   S*Sa + D*(1 - Sa)     case 2 -> 128    case 3 -> 160
 *   reading B   S + D*(1 - Sa)        case 2 -> 255    case 3 -> 255 (clamped)
 *
 * tests/model.h implements reading A. That is a CHOICE it makes so the host
 * arms have something to rasterise, not a finding, and nothing here inherits
 * it: cases 2 and 3 ADMIT BOTH and report which they saw (model=A / model=B /
 * model=none), case 4 is reading-agnostic by construction, and only a THIRD
 * value is broken.
 *
 * ★ WHAT CASES 2-4 *DO* ASSERT WITHOUT AMBIGUITY IS THE ALPHA ROW, and it is
 * not decoration -- it is the only thing in those cases that can see a GPU
 * which discards alpha. See the C_EXP_ALPHA note below for the measurement
 * that forced it. Case 5 deliberately does NOT, because BLEND_NONE's alpha row
 * is a different one.
 *
 * ★ THE TWO SRC_OVER CASES MUST AGREE, AND NOTHING HERE ENFORCES IT. If
 * case 2 reports A and case 3 reports B the hardware implements neither
 * formula consistently, which is a bigger finding than which formula it is.
 * They are separate cases so the transcript shows both readings side by side;
 * enforcing it would need the cross-case state the harness forbids (Phase 1
 * deleted s_fmt_fill for exactly that reason).
 *
 * ★ GREYSCALE IN CASES 2-5 IS DELIBERATE. White and grey are channel-
 * symmetric, so a channel PERMUTATION is invisible to them. That is the
 * intent: a word-order fault surfaces in exactly ONE place -- case 1 -- rather
 * than reddening four cases at once with no obvious first cause.
 *
 * ★ COVERAGE IS n/a THROUGHOUT. Filled area is not the question these cases
 * ask; every one of them reads a single solid interior pixel 40 px clear of
 * every edge, where coverage is exactly 1.0 and so cannot confound the alpha
 * term. The field is still printed on every line, via vgc_cover_na(), so a
 * grep for case lines lacking cover= stays a grep for nothing. */
#include "vgc_harness.h"
#include "vgc_color.h"
#include "vgc_predicates.h"      /* vgc_fnv, for case 4's sum hook */
#include <stdio.h>

/* ---- the one geometry every case here draws --------------------------------
 * 80x80 at (24,24), sampled at (64,64). Deliberately the SAME rect
 * path/single-contour-rect uses: if that baseline is broken these cases are
 * measuring nothing, and sharing the geometry makes that dependency visible
 * rather than implied. The macros are re-declared rather than shared because
 * they are static to vgc_cases_path.cpp; promoting them to the harness would
 * put a Phase 1 case's private geometry in a header four host suites read. */
#define C_X 24
#define C_Y 24
#define C_W 80
#define C_H 80

/* The sample point. 40 px from every edge of the rect. */
#define C_SX 64
#define C_SY 64

/* ---- the colours ---------------------------------------------------------- */

/* Source alpha, 0x80 -- a = 128/255 = 0.502, close enough to a half that the
 * arithmetic is readable and far enough from 0/255 that a dropped or doubled
 * alpha term is unmistakable. */
#define C_ALPHA 0x80

/* Backdrop grey, 0x40. NON-ZERO on purpose: SRC_OVER over black is degenerate
 * (the D*(1 - Sa) term vanishes), so a black backdrop cannot distinguish a
 * correct blend from one that ignores the destination entirely. Case 2 keeps
 * the black backdrop anyway because it is the cleanest reading of the SOURCE
 * term; case 3 is the one that exercises the destination. */
#define C_GREY 0x40

/* White at alpha 0x80, and opaque grey. VGC_ABGR_A masks each component, so a
 * computed alpha cannot silently corrupt the blue byte beside it. */
#define C_SRC_HALF_WHITE VGC_ABGR_A(C_ALPHA, 0xFF, 0xFF, 0xFF)
#define C_BACKDROP_GREY  VGC_ABGR(C_GREY, C_GREY, C_GREY)

/* Which channel cases 2-5 sample. Green, arbitrarily: the source is white and
 * the backdrop grey, so all three colour channels carry the same value and the
 * choice cannot matter -- which is the point of section 7 above. Named rather
 * than spelled at four call sites so it stays one choice. */
#define C_SAMPLE_CH VGC_G

/* ---- tolerances ------------------------------------------------------------
 * ★ GENEROUS FIRST, JUSTIFIED BY THE PLAUSIBLE ROUNDING MODELS, EXACT VALUE
 * ALWAYS PRINTED. +/-4 spans `/255` versus `/256` scaling and either rounding
 * direction with room to spare: 255*a + 64*(1 - a) is 159.87 under /255 and
 * 159.25 under /256, so the two models are already 0.6 apart before rounding.
 * Case 4's error compounds through two composites, hence +/-6.
 *
 * ★ AND THE POLICY IS A RESPONSE TO A MEASURED MISTAKE IN THIS PROJECT, not
 * caution for its own sake: Phase 1's antialiasing tolerance k=1/2 was
 * extrapolated from a SINGLE data point and turned out ~13x more generous than
 * the silicon needed. A tolerance invented from one measurement is a guess
 * wearing a number. These narrow after the boot, against what the hardware
 * actually does -- a deliberate narrowing WITH the measurement behind it,
 * which is a different act from re-goldening a checksum and must not be
 * written up as one.
 *
 * ★ THE BANDS STAY DISJOINT, which is what makes "which reading" a readable
 * answer rather than an overlap: 128 +/- 4 is [124,132], 160 +/- 4 is
 * [156,164], 255 +/- 4 is [251,259]. Case 2's named defect (~64, the
 * double-premultiply) and case 5's SRC_OVER answer (~160) both fall outside
 * every band they must fall outside. */
#define C_TOL       4
#define C_TOL_DBL   6

/* ---- expected values, derived here and INDEPENDENTLY in expected_silicon.txt
 * ★ DERIVED TWICE ON PURPOSE. tests/model.h computes these from the formula;
 * if the case simply asked the model what it thought, arm 1 would prove only
 * that the predicate reads what the model wrote -- circular. The hand
 * derivation is spelled out beside each constant so the two can be compared by
 * eye. a = 128/255 = 0.50196; 1 - a = 127/255 = 0.49804. */
#define C_EXP2_A  128   /* A: 255*a + 0*(1-a)  = 128.0                        */
#define C_EXP2_B  255   /* B: 255   + 0*(1-a)  = 255                          */
#define C_EXP3_A  160   /* A: 255*a + 64*(1-a) = 128.0 + 31.9 = 159.87 -> 160 */
#define C_EXP3_B  255   /* B: 255   + 64*(1-a) = 286.9, clamped to 255        */
#define C_EXP5_RAW 255  /* BLEND_NONE as ":459  RGB: S, No blend"             */
#define C_EXP5_MOD 128  /* BLEND_NONE modulating by alpha: 255*a = 128.0      */

/* ---- SRC_OVER's ALPHA row, and why cases 2-4 judge it ----------------------
 * ★★ THE COLOUR CHANNEL ALONE CANNOT SEE A GPU THAT DISCARDS ALPHA, AND THAT
 * WAS MEASURED, NOT SUSPECTED. With a SATURATED source (white, 255), reading B
 * -- `S + D*(1 - Sa)` -- is OBSERVATIONALLY IDENTICAL to writing S raw: over
 * black it is 255 + 0 = 255, and over grey 0x40 it is 286.9 clamped to 255.
 * Both are exactly what a GPU ignoring alpha entirely would write. So a
 * verdict computed from the colour channel and admitting reading B reports
 * `ok` for an alpha-ignoring implementation, and case 4's prediction rides on
 * the same value and reports `ok` too. Run against a deliberately
 * alpha-ignoring reference rasteriser, cases 2, 3 and 4 ALL PASSED with
 * `model=B`.
 *
 * ★ THE ALPHA ROW IS WHAT SEPARATES THEM, AND IT IS THE ONE PART OF SRC_OVER
 * WITH NO PREMULTIPLY QUESTION ATTACHED. inc/vg_lite.h:462 gives it
 * unambiguously as `A: Sa + Da*(1 - Sa)`, the same row under BOTH readings --
 * there is nothing here to pre-judge, which is exactly why this check can be
 * asserted where the colour channel's cannot. The backdrop is opaque
 * (vgc_clear's and vgc_clear_to's colours both carry alpha 0xFF), so Da = 255
 * and the answer is 128 + 255*0.498 = 255 under A and under B alike. An
 * alpha-ignoring GPU leaves the source's own 0x80 = 128. A 127-wide gap that
 * no rounding model closes, tested with the same +/-4 as everything else.
 *
 * ★ AND IT IS COVERAGE, NOT A WORKAROUND: the alpha row is half the operator
 * and nothing in this matrix looked at it before.
 *
 * ★★ CASE 5 MUST NOT GAIN THIS CHECK, and the asymmetry is deliberate rather
 * than an oversight. BLEND_NONE's alpha row is `A: Sa` (:459-460), so a raw
 * write leaving a = 0x80 is CORRECT there. Judging alpha in case 5 would
 * report broken on conforming hardware -- the opposite mistake, and the one
 * this file is most anxious about. Case 5 records alpha and judges only the
 * colour channel; the note at that case says so again where a reader will be
 * standing when the question occurs to them. */
#define C_EXP_ALPHA 255

/* ---- shared drawing ------------------------------------------------------- */

/* Emit the rect and init `p` over it. Returns the vgc_finish_path status,
 * which every caller checks: that function zeroes *p on arena overflow, so a
 * caller that carried on would hand the GPU path=NULL -- the harness
 * manufacturing the very class of failure it exists to measure. */
static vg_lite_error_t rect_path(vg_lite_path_t *p)
{
    vgc_emit_rect_cw(C_X, C_Y, C_W, C_H);
    return vgc_finish_path(p, C_X, C_Y, C_X + C_W, C_Y + C_H);
}

/* One draw of the standard rect in `color` under `blend`, over whatever the
 * caller has already put in the scratch. */
static vg_lite_error_t draw_rect(uint32_t color, vg_lite_blend_t blend)
{
    vg_lite_error_t acc = VG_LITE_SUCCESS;
    vg_lite_path_t p;
    const vg_lite_error_t fe = rect_path(&p);
    if (fe != VG_LITE_SUCCESS) return fe;
    vgc_draw_path_blend(&p, VG_LITE_FILL_NON_ZERO, color, blend, &acc);
    vgc_finish_into(&acc);
    return acc;
}

/* Which of two admissible readings the measured value matches, or NULL.
 * Both windows are C_TOL wide and (see the tolerance note) disjoint, so at
 * most one can match and the order of the tests carries no meaning.
 *
 * ★ NULL RATHER THAN THE STRING "none", so the verdict is `matched == NULL`
 * and never a test on the returned text. A caller written as
 * `name[0] == 'n'` works today only because no reading is spelled with a
 * leading n -- a coupling between a verdict and a label that a later rename
 * would break SILENTLY, in the direction that turns a broken case green. The
 * caller spells "none" itself, where it is visibly a label. */
static const char *reading_of(int v, int expect_a, int expect_b,
                              const char *name_a, const char *name_b)
{
    if (vgc_near(v, expect_a, C_TOL)) return name_a;
    if (vgc_near(v, expect_b, C_TOL)) return name_b;
    return NULL;
}

/* ---- 1. color/solid-word-order ---------------------------------------------
 * THE BOOTSTRAP CONTROL, and it is first in the table for the reason
 * path/single-contour-rect is first in its own: every case below reads a NAMED
 * channel, and this is the case that justifies the naming. Read its result
 * before any of theirs.
 *
 * It solves a circularity. Colour predicates need the memory word order; the
 * word order is what Phase 2 measures. The way out is an identity case that
 * asserts the order-agnostic half FIRST and the named half second.
 *
 * ★ THE FILL IS OPAQUE PURE RED, SO **TWO** CHANNELS SATURATE, NOT ONE -- red
 * and alpha -- and two are zero. This phrasing was wrong in an earlier draft
 * of the spec (and of vgc_color.h's own comment, corrected there against
 * tests/color_test.c). A reader who took "exactly one saturated" literally
 * would write `sat == 1`, and this case would report pixel=broken on CORRECT
 * silicon: the instrument fabricating a defect, in the one case that gates the
 * interpretation of every colour verdict below it.
 *
 * ★ AND THE COUNTS ARE NECESSARY BUT NOT SUFFICIENT. `sat == 2 && zero == 2`
 * is equally satisfied by two saturated COLOUR channels with alpha 0x00 -- a
 * fully transparent pixel of the wrong colour. What closes the case is the
 * named half the counts cannot express: red at 0xFF *and* alpha at 0xFF. Both
 * halves are asserted.
 *
 * ★ THE MAPPING IS NOT ORIGINATED HERE. vglite_probe measured it: clearing a
 * VG_LITE_BGRA8888 target with the vg_lite_color_t 0xFF204060 -- which the
 * driver reads as ABGR, B=0x20 G=0x40 R=0x60 -- returned 0xFF604020 in memory,
 * red in bits 23:16 (vglite_probe.cpp:56-59). This case RE-CONFIRMS that on
 * this scratch buffer in this boot, so an SDK bump that moved the order under
 * us shows up as one red cell rather than as four inexplicable ones. */

static vgc_verdict_t check_word_order(char *d, size_t n)
{
    const uint32_t px   = vgc_px(C_SX, C_SY);
    const int      sat  = vgc_saturated_channels(px);
    const int      zero = vgc_zero_channels(px);
    /* Constant indices, so vgc_ch's -1 sentinel cannot fire here -- but the
     * values are printed as read rather than as booleans, so if it ever did
     * (an index macro edited to something computed) the transcript would carry
     * a -1 that is unmistakable rather than a plausible byte. */
    const int      r    = vgc_ch(px, VGC_R);
    const int      a    = vgc_ch(px, VGC_A);
    const vgc_cover_t cv = vgc_cover_na();

    /* ★ THE RAW WORD IS THE FIRST FIELD, because if this case is wrong the raw
     * value is the only thing in the line worth reading: sat/zero/r/a are all
     * interpretations of it, and every one of them is suspect the moment the
     * case fails. */
    snprintf(d, n, "px=0x%08lX,sat=%d,zero=%d,r=%d,a=%d,%s",
             (unsigned long)px, sat, zero, r, a, cv.s);

    return (sat == 2 && zero == 2 && r == 0xFF && a == 0xFF)
        ? VGC_OK : VGC_BROKEN;
}

static vg_lite_error_t run_word_order(void)
{
    /* The harness has already cleared to VGC_BG_COLOR (opaque black) and reset
     * the arena; a case that renders once does not clear for itself. */
    return draw_rect(VGC_ABGR(0xFF, 0x00, 0x00), VG_LITE_BLEND_NONE);
}

/* ---- 2. color/premultiplied-srcover ----------------------------------------
 * White at alpha 0x80 over BLACK, SRC_OVER. The cleanest reading of the SOURCE
 * term: with D = 0 the destination term vanishes, so whatever comes back is
 * the source contribution alone.
 *
 * ok at ~128 (reading A) or ~255 (reading B) ON THE COLOUR CHANNEL, AND alpha
 * at 255 (:462, the same row under both readings); broken otherwise. ~64 is the
 * NAMED failure mode -- alpha applied TWICE, i.e. 255*a*a = 64.3 -- and it
 * must read broken, which it does: 64 is 60 outside the nearest band.
 *
 * ★ THIS CASE ALONE CANNOT SETTLE SRC_OVER, which is why case 3 exists rather
 * than being redundant with it. Over black, a correct blend and one that
 * ignores the destination entirely produce the identical pixel. */

static vgc_verdict_t check_premul_srcover(char *d, size_t n)
{
    const uint32_t px = vgc_px(C_SX, C_SY);
    const int      v  = vgc_ch(px, C_SAMPLE_CH);
    /* JUDGED, not merely recorded -- see the C_EXP_ALPHA note. Over black the
     * colour channel is 255 under reading B and 255 under a GPU that discards
     * alpha, so v= alone cannot tell a conforming SRC_OVER from one that
     * ignores the alpha term. The alpha row (:462) is unambiguous under both
     * readings and is what separates them. A 191 here would be
     * `Sa*Sa + Da*(1 - Sa)`, which is no convention's SRC_OVER at all. */
    const int      a  = vgc_ch(px, VGC_A);
    const int      a_ok = vgc_near(a, C_EXP_ALPHA, C_TOL);
    const char *const model = reading_of(v, C_EXP2_A, C_EXP2_B, "A", "B");
    const vgc_cover_t cv = vgc_cover_na();

    snprintf(d, n, "v=%d,a=%d,model=%s,%s", v, a, model ? model : "none", cv.s);
    return (model && a_ok) ? VGC_OK : VGC_BROKEN;
}

static vg_lite_error_t run_premul_srcover(void)
{
    /* Backdrop is the harness's own opaque-black clear -- no second clear, so
     * this case renders once and needs no sum hook. */
    return draw_rect(C_SRC_HALF_WHITE, VG_LITE_BLEND_SRC_OVER);
}

/* ---- 3. blend/srcover-arithmetic -------------------------------------------
 * The same source over GREY 0x40. This is the case that exercises the
 * DESTINATION term, and the pair (2, 3) is read together: agreement on a
 * reading is the result; disagreement between them is a larger finding than
 * either value.
 *
 * ok at ~160 (A) or ~255 (B, clamped from 286.9) on the colour channel, AND
 * alpha at 255; broken otherwise. In
 * particular ~128 -- the case-2 answer -- would mean the destination term was
 * dropped, and reads broken here. */

static vgc_verdict_t check_srcover_arith(char *d, size_t n)
{
    const uint32_t px = vgc_px(C_SX, C_SY);
    const int      v  = vgc_ch(px, C_SAMPLE_CH);
    /* JUDGED, for the reason at C_EXP_ALPHA: over grey 0x40 reading B clamps
     * to 255, which is also what a GPU discarding alpha writes, so the colour
     * channel cannot separate the two here either. */
    const int      a  = vgc_ch(px, VGC_A);
    const int      a_ok = vgc_near(a, C_EXP_ALPHA, C_TOL);
    const char *const model = reading_of(v, C_EXP3_A, C_EXP3_B, "A", "B");
    const vgc_cover_t cv = vgc_cover_na();

    snprintf(d, n, "v=%d,a=%d,model=%s,%s", v, a, model ? model : "none", cv.s);
    return (model && a_ok) ? VGC_OK : VGC_BROKEN;
}

/* Clear to grey, then one draw. The clear is not a second SUB-RENDER in the
 * sense vgc_harness.h means -- nothing is measured and discarded between the
 * two -- so the harness's default whole-buffer sum covers the entire result
 * and no sum hook is needed. Contrast case 4, which measures between its
 * draws. */
static vg_lite_error_t run_srcover_arith(void)
{
    const vg_lite_error_t ce = vgc_clear_to(C_BACKDROP_GREY);
    if (ce != VG_LITE_SUCCESS) return ce;
    return draw_rect(C_SRC_HALF_WHITE, VG_LITE_BLEND_SRC_OVER);
}

/* ---- 4. blend/srcover-double -----------------------------------------------
 * Case 3's draw, done TWICE with no clear between.
 *
 * ★★ READING-AGNOSTIC BY CONSTRUCTION, WHICH IS THE WHOLE POINT. It pins no
 * absolute value. It renders once and MEASURES the result (v1), renders again
 * and measures (v2), and asserts v2 lands where the SRC_OVER formula predicts
 * FROM THE MEASURED v1: 255*a + v1*(1 - a). That holds under reading A
 * (160 -> 208) and under reading B (255 -> 255, since the prediction
 * saturates too), so it tests the OPERATOR'S SELF-CONSISTENCY without
 * depending on which operator it is. A case that had to know would have been a
 * fourth vote for whichever reading its author picked.
 *
 * ★ IT ALSO RETIRES A QUIRK-TABLE ENTRY RATHER THAN CONFIRMING ONE. The
 * Phase 1 spec lists "SRC_OVER of AA paths is not idempotent -- double-
 * composited edges drift". That drift is arithmetically CORRECT compositing --
 * true of every conforming implementation, OpenVG and Porter-Duff alike -- so
 * a case asserting "twice differs from once" would confirm nothing about this
 * GPU. What is worth asking is whether the second composite lands where the
 * first one's own arithmetic says it should, and that is what this asks. */

static int      s_dbl_v1;
static uint32_t s_dbl_sum1;

/* The reading-A form of SRC_OVER with S = 255, applied to a MEASURED
 * destination. Under reading B the hardware's v1 is already 255 and this
 * returns 255 as well ((255*128 + 255*127 + 127)/255 = 255.49 -> 255), which
 * is why one predictor serves both readings. Rounds to nearest; the +/-6 band
 * absorbs the difference from a /256 or truncating implementation (worked
 * through: a /256-truncating pipeline gives v1=159, v2=206 against a
 * prediction of 207 -- one unit). */
static int srcover_predict(int dst)
{
    return (255 * C_ALPHA + dst * (255 - C_ALPHA) + 127) / 255;
}

static vg_lite_error_t run_srcover_double(void)
{
    const vg_lite_error_t ce = vgc_clear_to(C_BACKDROP_GREY);
    if (ce != VG_LITE_SUCCESS) return ce;

    vg_lite_error_t acc = draw_rect(C_SRC_HALF_WHITE, VG_LITE_BLEND_SRC_OVER);
    /* Measured AFTER the finish inside draw_rect, so the GPU's first composite
     * is complete and visible to the CPU (EXTMEM, D-cache never enabled on
     * this core -- no maintenance needed). */
    s_dbl_v1   = vgc_ch(vgc_px(C_SX, C_SY), C_SAMPLE_CH);
    s_dbl_sum1 = vgc_scratch_sum();

    /* No clear: the second composite goes over the first, which is the case.
     * The arena is untouched, so rect_path() simply emits a second copy --
     * safe the instant the preceding vg_lite_draw() returned (it memcpys the
     * path into the command buffer), and cheaper to read than reusing the
     * vg_lite_path_t across two helper calls. */
    const vg_lite_error_t e2 = draw_rect(C_SRC_HALF_WHITE, VG_LITE_BLEND_SRC_OVER);
    if (e2 != VG_LITE_SUCCESS && acc == VG_LITE_SUCCESS) acc = e2;
    return acc;
}

/* Both sub-renders folded together, so a difference in EITHER shows up in
 * repeat=. Depends only on state run() sets and on the live buffer, never on
 * anything check() computes -- the ordering contract in vgc_harness.h (the
 * harness calls sum() before check(), and again after the second run with no
 * check() in between). */
static uint32_t sum_srcover_double(void)
{
    uint32_t pair[2] = { s_dbl_sum1, vgc_scratch_sum() };
    return vgc_fnv(pair, sizeof(pair));
}

static vgc_verdict_t check_srcover_double(char *d, size_t n)
{
    const uint32_t px = vgc_px(C_SX, C_SY);
    const int v2   = vgc_ch(px, C_SAMPLE_CH);
    const int pred = srcover_predict(s_dbl_v1);
    /* JUDGED HERE TOO, and this case needs it MORE than 2 and 3 do rather than
     * less. Its whole design is to predict v2 from the MEASURED v1, so a GPU
     * discarding alpha is self-consistent by construction -- it writes 255,
     * then 255, and the prediction from 255 is 255. Measured: against an
     * alpha-ignoring rasteriser this case reported ok on the colour channel
     * alone. Alpha after two composites over an opaque backdrop is 255 under
     * both readings (255 is a fixed point of `Sa + Da*(1 - Sa)`), so the
     * expectation is the same one cases 2 and 3 use. */
    const int a    = vgc_ch(px, VGC_A);
    const int a_ok = vgc_near(a, C_EXP_ALPHA, C_TOL);
    const vgc_cover_t cv = vgc_cover_na();

    snprintf(d, n, "v1=%d,v2=%d,a=%d,pred=%d,%s", s_dbl_v1, v2, a, pred, cv.s);
    return (vgc_near(v2, pred, C_TOL_DBL) && a_ok) ? VGC_OK : VGC_BROKEN;
}

/* ---- 5. blend/none-honours-alpha -------------------------------------------
 * White at alpha 0x80 over grey, with BLEND_NONE.
 *
 * ★ RECORDED, NOT JUDGED -- the path/degenerate-zero-area pattern. It earns
 * its place because ALL FIFTEEN Phase 1 cases use BLEND_NONE with an OPAQUE
 * colour: if this mode silently honours alpha, we have never once been in a
 * position to see it, and the moment anyone passes a non-opaque colour through
 * it every Phase 1 result would need re-reading.
 *
 * Two defensible readings, BOTH ok:
 *   255  "no blend" means dst := src, written raw. The conventional reading,
 *        the header's own (":459  RGB: S, No blend"), and what every Phase 1
 *        case implicitly relied on.
 *   ~128 the rasteriser always modulates by alpha and BLEND_NONE only drops
 *        the DESTINATION term.
 *
 * ★ AND ~160 MUST READ BROKEN, which is what stops "records rather than
 * judges" collapsing into "asserts nothing". 160 is SRC_OVER's answer over
 * this backdrop, so it would mean BLEND_NONE is silently BLENDING -- a
 * finding, not a defensible reading. With C_TOL = 4 the two admissible bands
 * are [124,132] and [251,259]; 160 sits outside both, 24 clear of the nearer
 * edge. */

static vgc_verdict_t check_none_alpha(char *d, size_t n)
{
    const uint32_t px = vgc_px(C_SX, C_SY);
    const int      v  = vgc_ch(px, C_SAMPLE_CH);
    /* ★★ RECORDED, NEVER JUDGED -- AND THIS IS THE ONE PLACE IN THE FILE WHERE
     * THAT IS A DELIBERATE ASYMMETRY RATHER THAN A GAP. Cases 2, 3 and 4 all
     * ASSERT alpha == 255, because SRC_OVER's alpha row is
     * `Sa + Da*(1 - Sa)` (:462) and the colour channel alone cannot see a GPU
     * that discards alpha. BLEND_NONE's alpha row is a DIFFERENT row:
     * `A: Sa` (:459-460). A raw write therefore leaves the source's own
     * 0x80 = 128 in the alpha byte, and that is CORRECT. Applying cases 2-4's
     * check here would report broken on conforming hardware -- the exact
     * inverse of the mistake the alpha check exists to prevent, and a far
     * worse one, since an instrument that invents defects is not an
     * instrument.
     *
     * It is still the most informative recorded field in this case: 128 says
     * the source was stored raw, 255 would say the mode had quietly
     * composited against the opaque backdrop. Printed, so a boot answers it
     * either way; not judged, because both answers are defensible exactly as
     * both v= answers are. */
    const int      a  = vgc_ch(px, VGC_A);
    const char *const read = reading_of(v, C_EXP5_MOD, C_EXP5_RAW,
                                        "modulated", "raw");
    const vgc_cover_t cv = vgc_cover_na();

    snprintf(d, n, "v=%d,a=%d,read=%s,%s", v, a, read ? read : "none", cv.s);
    return read ? VGC_OK : VGC_BROKEN;
}

static vg_lite_error_t run_none_alpha(void)
{
    const vg_lite_error_t ce = vgc_clear_to(C_BACKDROP_GREY);
    if (ce != VG_LITE_SUCCESS) return ce;
    return draw_rect(C_SRC_HALF_WHITE, VG_LITE_BLEND_NONE);
}

/* ---- the table -------------------------------------------------------------
 * ORDER IS PART OF THE INSTRUMENT, as it is in vgc_cases_path.cpp.
 * color/solid-word-order is FIRST because every case below it reads a named
 * channel and it is the case that justifies the naming -- if it is broken,
 * nothing below it means anything.
 *
 * The two SRC_OVER readings cases sit adjacent so the pair is read as one
 * answer, and blend/srcover-double sits immediately after the case whose draw
 * it repeats.
 *
 * The ids are stable: run_qemu.sh and expected_silicon.txt key on them, so
 * renaming one turns both red, which is the intent. */
const vgc_case_t vgc_color_cases[] = {
    { "color/solid-word-order",       run_word_order,      check_word_order,      NULL },
    { "color/premultiplied-srcover",  run_premul_srcover,  check_premul_srcover,  NULL },
    { "blend/srcover-arithmetic",     run_srcover_arith,   check_srcover_arith,   NULL },
    { "blend/srcover-double",         run_srcover_double,  check_srcover_double,  sum_srcover_double },
    { "blend/none-honours-alpha",     run_none_alpha,      check_none_alpha,      NULL },
};
const size_t vgc_color_case_count =
    sizeof(vgc_color_cases) / sizeof(vgc_color_cases[0]);
