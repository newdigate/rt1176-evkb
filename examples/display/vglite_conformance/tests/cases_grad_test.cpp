/* Host-compiled test for the conformance probe's GRADIENT CASES.
 * Build: tests/run.sh
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * ★★ WHAT THIS TEST IS, AND WHAT IT IS NOT. It runs the REAL
 * vgc_cases_grad.cpp -- its geometry, its sample columns, its thresholds, its
 * calls into the gradient API -- against SIX MODELS of a GPU sitting under a
 * model of the DRIVER (model.h reproduces vg_lite_update_linear_grad and the
 * legacy pair from their source, line-cited):
 *   1 a CORRECT GPU: samples the ramp in the byte order the driver wrote it,
 *     and takes the paint parameter from grad->matrix ALONE with path_matrix
 *     applied only to the geometry -- what vg_lite_draw_linear_grad programs.
 *     Under it the six cases must report what expected_silicon.txt predicts,
 *     EXCEPT legacy-linear, which a correct GPU renders fine: its `broken` is a
 *     claim about THIS GC355, and the boot is what tests it.
 *   2 one that draws NOTHING -- every case broken.
 *   3 one that draws BLACK -- the legacy claim applied to every draw; every
 *     case broken with l=0.0.0.
 *   4 one whose PAINT FOLLOWS THE PATH -- path_matrix composed into the
 *     gradient. moved and reupdate go OK (the prediction inverted, by name),
 *     rebuilt goes BROKEN (re-specifying the line in screen space is the WRONG
 *     fix on such a GPU), static/legacy/word-order unchanged. This is the arm
 *     that proves the three cells can see the question.
 *   5 SOLID FIRST STOP -- no interpolation. Every case broken on monotonic
 *     EXCEPT ramp-word-order, which stays ok because its two stops are
 *     identical: pinned, because it says that case is blind to this defect BY
 *     DESIGN and the other five are what catch it.
 *   6 a RAMP STORE THAT PERMUTES R AND B -- the driver packing A,R,G,B where
 *     the sampler expects A,B,G,R. ramp-word-order goes broken reading BLUE;
 *     the four red-to-blue EXT cases go broken too, because their gradient
 *     REVERSES -- and a reversed gradient IS wrong, so that is the correct
 *     verdict, not collateral. legacy-linear stays ok: its ramp is CPU-packed
 *     ARGB words through a different function. What the word-order case adds
 *     is that it can NAME the fault (exact bytes) where the others only reverse.
 *
 * ★ ARM 4 IS THE ONE THIS SUITE WAS WRITTEN FOR. The moved/reupdate/rebuilt
 * row is a single experiment whose answer is derived from reading the driver;
 * a suite that could not produce the OTHER answer would be unable to say
 * whether the cases measure placement at all. Arm 4 produces it, and pins the
 * DIRECTION of every cell that moves.
 *
 * ★ THE NEGATIVE ARMS ARE THE POINT -- the lesson every earlier suite in this
 * example carries. Demonstrated for this suite before it was trusted: with
 * check_ext_static hard-wired to VGC_OK, arm 1 stays green and arms 2 and 3
 * catch it by name.
 *
 * It is NOT a statement about the silicon. No GPU is involved. Silicon wins.
 *
 * ★ C++, for the reason every other case suite is: vgc_harness.h wraps
 * vg_lite.h in extern "C" and the code under test is compiled as C++. */
#include "model.h"
#include "harness_mirror.h"
#include "../vgc_harness.h"
#include "../vgc_color.h"
#include <stdio.h>
#include <string.h>

static int failed = 0;
static int checks = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        checks++;                                                        \
        if (cond) printf("PASS: %s\n", #cond);                           \
        else { printf("FAIL: %s  (line %d)\n", #cond, __LINE__); failed++; } \
    } while (0)

#define CHECK_CASE(cond, id, what)                                       \
    do {                                                                 \
        checks++;                                                        \
        if (cond) printf("PASS: %s %s\n", (id), (what));                 \
        else { printf("FAIL: %s %s  (line %d)\n", (id), (what), __LINE__); failed++; } \
    } while (0)

#define ID_LEGACY   "grad/legacy-linear"
#define ID_STATIC   "grad/ext-linear-static"
#define ID_MOVED    "grad/ext-linear-moved"
#define ID_REUPDATE "grad/ext-linear-reupdate"
#define ID_REBUILT  "grad/ext-linear-rebuilt"
#define ID_WORD     "grad/ramp-word-order"

static int is_id(const vgc_case_t *c, const char *id) { return strcmp(c->id, id) == 0; }

static void arm_verdicts(const char *label,
                         int (*expect_broken)(const vgc_case_t *),
                         const char *why_ok, const char *why_broken,
                         void (*extra)(const vgc_case_t *, const case_result_t *))
{
    printf("-- %s\n", label);
    for (size_t i = 0; i < vgc_grad_case_count; i++) {
        const vgc_case_t *c = &vgc_grad_cases[i];
        case_result_t r;
        const int live_before = g_ramp_live;
        run_one(c, &r);
        const int broken = expect_broken(c);
        printf("   %-28s %-6s %s\n", c->id,
               r.verdict == VGC_OK ? "ok" : r.verdict == VGC_BROKEN ? "BROKEN" : "skip",
               r.detail);
        CHECK_CASE(r.verdict == (broken ? VGC_BROKEN : VGC_OK), c->id,
                   broken ? why_broken : why_ok);
        CHECK_CASE(r.oob == 0u, c->id, "no out-of-range px read");
        CHECK_CASE(r.api == VG_LITE_SUCCESS, c->id, "every driver call succeeded");
        /* ★ THE LEAK IS COUNTED, NOT ASSERTED AWAY. run_one runs each case
         * TWICE; reupdate leaks exactly one ramp per run by construction
         * (update called twice, clear frees only the second), so it must
         * leave the pool +2; every other case must leave it exactly where it
         * found it -- which is also what proves ext_teardown runs. */
        const int leaked = g_ramp_live - live_before;
        if (is_id(c, ID_REUPDATE))
            CHECK_CASE(leaked == 2, c->id, "leaks exactly one ramp image per run");
        else
            CHECK_CASE(leaked == 0, c->id, "frees every ramp image it allocated");
        if (extra) extra(c, &r);
    }
}

static int expect_all(const vgc_case_t *c)  { (void)c; return 1; }

/* Arm 1: what expected_silicon.txt predicts, minus the legacy cell. */
static int expect_correct(const vgc_case_t *c)
{
    return is_id(c, ID_MOVED) || is_id(c, ID_REUPDATE);
}

/* Arm 4: the prediction row inverted. */
static int expect_follows_path(const vgc_case_t *c)
{
    return is_id(c, ID_REBUILT);
}

/* Arm 5: everything but the two-identical-stops case. */
static int expect_solid(const vgc_case_t *c)
{
    return !is_id(c, ID_WORD);
}

/* Arm 6: every EXT case; the legacy ramp is packed elsewhere. */
static int expect_permute(const vgc_case_t *c)
{
    return !is_id(c, ID_LEGACY);
}

/* ---- arm 1's pins ------------------------------------------------------------
 * ★ THE VALUES ARE THIS MODEL'S, and they pin the model as much as the cases:
 * a drifting reference sampler shows up here as a moved number rather than a
 * silently moved judgement. They are NOT predictions about the silicon --
 * expected_silicon.txt pins verdicts, not profiles, for the reason its header
 * gives. Derivation for the static case, sample columns 28/64/100 of a ramp
 * from 24 to 104 (t = (x + 0.5 - 24) / 80): t = 0.056 / 0.506 / 0.956, so
 * R = 255(1 - t) and B = 255 t give ~241/14, ~126/129, ~11/244. The moved
 * case samples 44/80/116 against the SAME unmoved ramp: t = 0.256 / 0.706 /
 * 1.156 (PAD to 1), so its left reads ~190 -- below the red band -- and that
 * is the whole finding in one number. */
static void arm1_pins(const vgc_case_t *c, const case_result_t *r)
{
    if (is_id(c, ID_STATIC))
        CHECK_CASE(strstr(r->detail, "l=241.0.14,m=126.0.129,r=11.0.244,mono=1") != NULL,
                   c->id, "the ramp spans the rect edge to edge");
    if (is_id(c, ID_MOVED))
        CHECK_CASE(strstr(r->detail, "l=190.0.65,") != NULL && strstr(r->detail, "r=0.0.255,") != NULL,
                   c->id, "left is 20% into an unmoved ramp; right is PAD blue");
    if (is_id(c, ID_REUPDATE)) {
        CHECK_CASE(strstr(r->detail, "l=190.0.65,") != NULL,
                   c->id, "IDENTICAL to moved: a second update is idempotent");
        CHECK_CASE(strstr(r->detail, "leak=1") != NULL, c->id, "and it says so");
    }
    if (is_id(c, ID_REBUILT))
        CHECK_CASE(strstr(r->detail, "l=241.0.14,m=126.0.129,r=11.0.244,mono=1") != NULL,
                   c->id, "re-specified in screen space, it matches static exactly");
    if (is_id(c, ID_WORD))
        CHECK_CASE(strstr(r->detail, "l=255.0.0,m=255.0.0,r=255.0.0,mono=0,a=255") != NULL,
                   c->id, "every sample exactly opaque red");
    if (is_id(c, ID_LEGACY)) {
        CHECK_CASE(strstr(r->detail, "mono=1") != NULL, c->id, "a correct GPU renders the legacy ramp");
        CHECK_CASE(strstr(r->detail, "c0=0,c0n=2,c0k=FF000000,c0w=FFFFFFFF") != NULL,
                   c->id, "set_grad(count=0) returns SUCCESS and update substitutes black->white");
    }
    CHECK_CASE(strstr(r->detail, "cover=n/a") != NULL, c->id, "coverage is n/a, and still printed");
}

static void arm3_pins(const vgc_case_t *c, const case_result_t *r)
{
    CHECK_CASE(strstr(r->detail, "l=0.0.0,m=0.0.0,r=0.0.0") != NULL, c->id,
               "reads black at every sample -- the legacy claim's shape");
}

static void arm4_pins(const vgc_case_t *c, const case_result_t *r)
{
    /* The moved cells now read exactly what static reads: the paint went
     * with the path. */
    if (is_id(c, ID_MOVED) || is_id(c, ID_REUPDATE))
        CHECK_CASE(strstr(r->detail, "l=241.0.14,m=126.0.129,r=11.0.244,mono=1") != NULL,
                   c->id, "reads the static profile: the paint followed the path");
    /* And rebuilt -- line re-specified at 40..120 in SCREEN space, then the
     * GPU maps the moved path back by 16 -- reads a ramp shifted the wrong
     * way: left is PAD red, right is short of blue. */
    if (is_id(c, ID_REBUILT))
        CHECK_CASE(strstr(r->detail, "l=255.0.0,") != NULL, c->id,
                   "the screen-space rebuild is the WRONG fix on a path-space GPU");
}

static void arm5_pins(const vgc_case_t *c, const case_result_t *r)
{
    if (is_id(c, ID_WORD))
        CHECK_CASE(strstr(r->detail, "l=255.0.0,m=255.0.0,r=255.0.0") != NULL, c->id,
                   "identical stops: blind to a solid fill BY DESIGN");
    else
        CHECK_CASE(strstr(r->detail, "mono=0") != NULL, c->id, "breaks on monotonic");
}

static void arm6_pins(const vgc_case_t *c, const case_result_t *r)
{
    if (is_id(c, ID_WORD))
        CHECK_CASE(strstr(r->detail, "l=0.0.255,m=0.0.255,r=0.0.255") != NULL, c->id,
                   "reads BLUE where red was packed: names the fault exactly");
    if (is_id(c, ID_STATIC))
        CHECK_CASE(strstr(r->detail, "l=14.0.241,") != NULL, c->id,
                   "reversed -- broken for the right reason, but cannot name it");
    if (is_id(c, ID_LEGACY))
        CHECK_CASE(strstr(r->detail, "mono=1") != NULL, c->id,
                   "the legacy ramp is packed by a different function: untouched");
}

int main(void)
{
    memset(&vgc_scratch, 0, sizeof(vgc_scratch));
    CHECK(vgc_grad_case_count == 6);

    /* ---- ARM 1 ------------------------------------------------------------ */
    g_permute_rb = g_draw_nothing = g_double_premul = g_alpha_ignoring = 0;
    g_reading_a = g_none_modulates = 0;
    g_draw_black = g_paint_follows_path = g_solid_first_stop = g_ramp_permute_rb = 0;
    g_parse_error = 0; g_close_fixup_fired = 0; g_ramp_live = 0;
    arm_verdicts("arm 1: correct GPU under the driver as read (screen-space paint)",
                 expect_correct, "verdict ok", "goes BROKEN: the paint did not follow the path",
                 arm1_pins);
    for (size_t i = 0; i < vgc_grad_case_count; i++) {
        const vgc_case_t *c = &vgc_grad_cases[i];
        case_result_t r;
        run_one(c, &r);
        CHECK_CASE(r.api2 == r.api, c->id, "second run's status matches");
        CHECK_CASE(r.repeat_same, c->id, "repeat identical");
    }
    CHECK(g_parse_error == 0);
    CHECK(g_close_fixup_fired == 0);

    /* ---- ARM 2 ------------------------------------------------------------ */
    g_draw_nothing = 1; g_parse_error = 0;
    arm_verdicts("arm 2: null rasteriser (nothing is drawn)",
                 expect_all, "(unreachable)", "goes BROKEN when nothing is drawn", NULL);
    g_draw_nothing = 0;

    /* ---- ARM 3 ------------------------------------------------------------ */
    g_draw_black = 1;
    arm_verdicts("arm 3: every gradient draw paints black (the legacy claim)",
                 expect_all, "(unreachable)", "goes BROKEN on black", arm3_pins);
    g_draw_black = 0;

    /* ---- ARM 4 ------------------------------------------------------------ */
    g_paint_follows_path = 1;
    arm_verdicts("arm 4: a GPU whose paint FOLLOWS the path (matrices composed)",
                 expect_follows_path,
                 "ok: on this GPU the move reaches the gradient",
                 "goes BROKEN: the screen-space rebuild is wrong here", arm4_pins);
    g_paint_follows_path = 0;

    /* ---- ARM 5 ------------------------------------------------------------ */
    g_solid_first_stop = 1;
    arm_verdicts("arm 5: solid first stop (no interpolation)",
                 expect_solid, "identical stops, so blind by design",
                 "goes BROKEN on monotonic", arm5_pins);
    g_solid_first_stop = 0;

    /* ---- ARM 6 ------------------------------------------------------------ */
    g_ramp_permute_rb = 1;
    arm_verdicts("arm 6: ramp store packs A,R,G,B (the sampler expects A,B,G,R)",
                 expect_permute, "legacy ramp packed elsewhere: untouched",
                 "goes BROKEN: the EXT ramp reads back permuted", arm6_pins);
    g_ramp_permute_rb = 0;

    printf("--\n");
    if (failed) { printf("cases_grad_test: FAILED (%d of %d checks)\n", failed, checks); return 1; }
    printf("cases_grad_test: OK (%d checks)\n", checks);
    return 0;
}
