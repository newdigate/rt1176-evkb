/* Host-compiled test for the conformance probe's PATH CASE GEOMETRY.
 * Build: tests/run.sh
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * ★★ WHAT THIS TEST IS, AND WHAT IT IS NOT. Read this before quoting a green
 * run at anybody.
 *
 * It IS an exercise of vgc_cases_path.cpp's own geometry, sample points,
 * tolerances and predicates -- the REAL run()/check()/sum() functions, linked
 * and called -- against FOUR MODELS of a GPU:
 *   - a correct one (a scanline reference rasteriser honouring every contour
 *     and both fill rules), under which all fifteen cases must report ok;
 *   - this GC355's KNOWN defect (the same rasteriser dropping every contour
 *     after the first), under which the six cases aimed at that defect must
 *     report broken BY NAME and every control must stay ok; and
 *   - one that draws NOTHING, under which fourteen of fifteen must go broken
 *     (degenerate-zero-area legitimately stays ok -- "nothing drawn" is an
 *     accepted outcome there); and
 *   - one that draws the right SHAPE plus 400 px of ink that is not in the
 *     path, placed where no structural sample point or sampled column can see
 *     it, under which fourteen of fifteen must go broken BY THE COVERAGE
 *     FIELD (cover=stray:).
 * ★ ARMS 3 AND 4 ARE NOT REDUNDANT, AND EACH CLOSES A HOLE THE OTHERS LEAVE.
 * Measured for arm 3: a case hard-wired to VGC_OK leaves arm 1 GREEN and arm 2
 * green for every control, and is caught ONLY by arm 3. Arm 4 is the same
 * argument one level down: arms 1-3 only ever reach cover=ok and cover=n/a --
 * the coverage check's FAILING branch is never executed, so it could be
 * hard-wired to pass and all three would stay green. Arm 4 models the exact
 * defect that motivated coverage (four-nested-rings rendering 1171 px of
 * excess while reporting pixel=ok) and is the only arm that can see it. A
 * positive-only suite is equally consistent with a matrix that cannot detect
 * anything.
 *
 * It is NOT a statement about what the real silicon does. Not one line here
 * touches a GPU. The silicon's answers live in the example's
 * transcript_hw_evkb.txt and expected_silicon.txt, and nothing in this file
 * can confirm, contradict or substitute for them. A future reader who reports
 * "the conformance cases pass" on the strength of this suite has said
 * something true and useless. Silicon wins; this is the instrument's
 * calibration, taken before the instrument is pointed at anything.
 *
 * ★ WHY IT EXISTS AT ALL: the QEMU gate cannot reach this code. QEMU has no
 * GC355, so the chip-ID probe reads 0 and every case reports pixel=skip --
 * meaning a green gate says NOTHING about a sample point, a tolerance or a
 * predicate. Between the gate and the one silicon boot there was no check on
 * any of it. This is that check, and it earned its place before it was
 * written: it caught an FP32 path array whose opcodes were encoded as
 * (float)VLC_OP_* rather than as the driver's one-byte-at-the-slot-base, which
 * renders NOTHING and would have reported two false `broken`s from the bench.
 *
 * ★ THE NEGATIVE ARM IS THE HALF THAT MATTERS. A suite that only ran the
 * correct-GPU model would pass against a case table that cannot detect
 * anything at all -- fifteen predicates hard-wired to VGC_OK included. The
 * first-contour-only arm is what says a `broken` on the bench is a GC355
 * finding rather than a harness artefact, and it is asserted here rather than
 * printed for the same reason every other guard in this tree is: an
 * observation nobody checks stops being true silently.
 *
 * ★ C++ RATHER THAN C, for the same reason arena_test is: vgc_harness.h wraps
 * its vg_lite.h include in `extern "C"` and the code under test is compiled as
 * C++ on the target.
 *
 * ★ CHECK(), NOT assert(), AND IT KEEPS GOING -- the tree's convention:
 * PASS:/FAIL: per check, a count at the end, and a red run still tells you
 * which parts of the matrix are trustworthy. */
/* THE MODEL LIVES IN model.h AND IS SHARED. The reference rasteriser, the
 * blend, the harness services (vgc_fb/vgc_px/vgc_clear/vgc_draw_path/...) and
 * the arm switches (g_one_contour_only, g_draw_nothing, g_stray_ink) are all
 * there, so the Phase 2 colour case-geometry suite gets exactly the same ones
 * rather than a second copy that agrees today. What stays HERE is everything
 * that knows what a case is called: the four arm drivers and their
 * expectations. model.h pulls in vgc_harness.h and vgc_predicates.h; the
 * harness include below is kept because this file names vgc_case_t,
 * vgc_path_cases and VGC_OK directly. */
#include "model.h"
#include "../vgc_harness.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int failed = 0;
static int checks = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        checks++;                                                        \
        if (cond) {                                                      \
            printf("PASS: %s\n", #cond);                                 \
        } else {                                                         \
            printf("FAIL: %s  (line %d)\n", #cond, __LINE__);            \
            failed++;                                                    \
        }                                                                \
    } while (0)

/* Per-case variant: the failure message has to name the CASE, since the same
 * source line runs for all fifteen and `#cond` alone would not say which one
 * went red. */
#define CHECK_CASE(cond, id, what)                                       \
    do {                                                                 \
        checks++;                                                        \
        if (cond) {                                                      \
            printf("PASS: %s %s\n", (id), (what));                       \
        } else {                                                         \
            printf("FAIL: %s %s  (line %d)\n", (id), (what), __LINE__);  \
            failed++;                                                    \
        }                                                                \
    } while (0)

/* ---- running one case, exactly as the harness does ------------------------
 * The sequence mirrors run_case() in vglite_conformance.cpp: reset the arena
 * and the oob counter, clear, run, sum, check -- then clear and run AGAIN and
 * sum, which is what makes the repeat= comparison meaningful. Deviating here
 * would test the cases under a lifecycle they never see. */
typedef struct {
    vgc_verdict_t   verdict;
    vg_lite_error_t api;
    vg_lite_error_t api2;      /* the SECOND run's status -- the harness prints it */
    uint32_t        oob;
    int             repeat_same;
    int             hook_distinct;  /* a sum() hook must not be the live buffer */
    char            detail[VGC_DETAIL_MAX];
} case_result_t;

static void run_one(const vgc_case_t *c, case_result_t *r)
{
    memset(r, 0, sizeof(*r));
    vgc_arena_reset();
    vgc_px_oob_reset();
    vgc_clear();
    r->api = c->run();
    const uint32_t live1 = vgc_scratch_sum();
    const uint32_t sum1  = c->sum ? c->sum() : live1;
    /* ★ A HOOK THAT RETURNS THE LIVE BUFFER IS A HOOK THAT DOES NOTHING, and
     * the failure is invisible: repeat= keeps comparing something, just not
     * every sub-render. vgc_harness.h names a multi-render case without a
     * working hook as its top risk, and this is the only place that can see
     * it. Cases WITHOUT a hook are exempt by construction -- for them sum1 IS
     * live1. */
    r->hook_distinct = c->sum ? (sum1 != live1) : 1;
    r->verdict = c->check(r->detail, sizeof(r->detail));
    r->oob = vgc_px_oob();

    vgc_arena_reset();
    vgc_clear();
    r->api2 = c->run();
    const uint32_t sum2 = c->sum ? c->sum() : vgc_scratch_sum();
    r->repeat_same = (sum1 == sum2);
}

/* The SIX cases aimed at the contour-encoding question. Everything else in
 * the table is a control and must survive that model unchanged.
 *
 * ★ THE PHASE 1b PAIR IS IN HERE FOR THE SAME REASON THE OTHERS ARE, AND FOR
 * NO STRONGER ONE. path/two-disjoint-bars and path/four-nested-rings are both
 * multi-contour paths, so a rasteriser that keeps only the first contour must
 * fail their predicates -- two bars collapse to one run, four alternating
 * rings collapse to the outer rect's one solid block. That is a statement
 * about THIS MODEL. The pair exists to separate disjointness from contour
 * COUNT on silicon, and this model cannot separate them: it drops contour two
 * onward whatever they are. Nothing here predicts the bench. */
static int is_multi_contour_probe(const char *id)
{
    /* ★ path/multi-contour-close-padded IS IN THIS SET, and its membership is
     * a statement about the MODEL, not about silicon. Both model rasterisers
     * read the opcode byte and ignore the slot's padding, so the padded case
     * is byte-for-byte the same geometry to them and must behave exactly like
     * multi-contour-disjoint in both arms. That agreement is what proves the
     * two cases really are one variable apart. It is NOT evidence about the
     * padding hypothesis -- only the bench can separate those, which is the
     * whole reason that case exists. */
    return strcmp(id, "path/multi-contour-disjoint")    == 0 ||
           strcmp(id, "path/multi-contour-close-padded") == 0 ||
           strcmp(id, "path/two-disjoint-bars")          == 0 ||
           strcmp(id, "path/four-nested-rings")          == 0 ||
           strcmp(id, "path/two-contour-ring-nonzero")  == 0 ||
           strcmp(id, "path/evenodd-vs-nonzero")        == 0;
}

int main(void)
{
    memset(&vgc_scratch, 0, sizeof(vgc_scratch));

    /* The table's size is part of what the gate and expected_silicon.txt key
     * on, and both arms below iterate it -- so an accidental table edit must
     * not quietly shrink what this suite covers. */
    CHECK(vgc_path_case_count == 15);

    /* ---- ARM 1: a CORRECT GPU. Everything must be ok. ---------------------- */
    printf("-- arm 1: correct rasteriser (all contours, both fill rules)\n");
    g_one_contour_only = 0;
    g_parse_error = 0;
    g_close_fixup_fired = 0;

    for (size_t i = 0; i < vgc_path_case_count; i++) {
        const vgc_case_t *c = &vgc_path_cases[i];
        case_result_t r;
        run_one(c, &r);
        printf("   %-34s %-6s %s\n", c->id,
               r.verdict == VGC_OK ? "ok" : r.verdict == VGC_BROKEN ? "BROKEN" : "skip",
               r.detail);
        CHECK_CASE(r.verdict == VGC_OK,   c->id, "verdict ok");
        /* ★ RELABELLED, because "api success" overstated it. This suite's
         * vgc_finish_into is a no-op and its vg_lite_draw always succeeds, so
         * the ONLY reachable non-success here is vgc_finish_path refusing an
         * arena overflow. That is worth pinning -- an overflow returns early
         * and leaves the case measuring a path it never drew -- but it is not
         * a statement about any driver. */
        CHECK_CASE(r.api == VG_LITE_SUCCESS,  c->id, "arena did not overflow");
        CHECK_CASE(r.oob == 0u,               c->id, "no out-of-range px read");
        CHECK_CASE(r.repeat_same,             c->id, "repeat identical");
        /* api2 is a field the harness PRINTS and the gate can assert on; with
         * the second run's status discarded it was never exercised at all. */
        CHECK_CASE(r.api2 == r.api,           c->id, "second run's status matches");
        CHECK_CASE(r.hook_distinct,           c->id, "sum hook is not the live buffer");

        /* One numeric pin, on the baseline only. 6400 is the exact analytic
         * area of an 80x80 integer-aligned rect under centre sampling, so it
         * catches a reference off by a row or a column -- which would silently
         * shift every tolerance judgement this suite makes. The triangle's
         * 1770 is NOT pinned: it is an artefact of centre sampling, and
         * pinning it would fight a legitimately better rasteriser. */
        if (strcmp(c->id, "path/single-contour-rect") == 0)
            CHECK_CASE(strstr(r.detail, "fill=6400,expect=6400") != NULL,
                       c->id, "fill is the exact analytic area");

        /* ★ THE NESTED RINGS' FILL IS PINNED FOR THE SAME REASON, and it is
         * the case in the table where a right VERDICT is cheapest to get for
         * a wrong reason. Its predicate counts RUNS down one column, and four
         * runs is a number several wrong pictures can produce -- a mis-nested
         * set of rects, or an alternation that pairs the wrong rects. 5760 is
         * the exact analytic area of the alternating nest,
         * (96^2-72^2) + (48^2-24^2) = 4032 + 1728, with no antialiasing in
         * this model and every edge integer-aligned, so it is arithmetic
         * rather than a tolerance. Dropping the alternation makes it the
         * solid 9216; a shifted rect moves it. Either shows up as a NUMBER
         * instead of only a verdict.
         *
         * On the TARGET the same field is a reading, not a bound: hardware
         * antialiasing moves it by a perimeter's worth, which is why
         * check_four_nested judges on runs alone and prints expfill= beside
         * it. Pinning it here costs nothing there. */
        if (strcmp(c->id, "path/four-nested-rings") == 0)
            CHECK_CASE(strstr(r.detail, "runs=4,expect=4,fill=5760") != NULL,
                       c->id, "four bands and the exact analytic area");

        /* Its partner is the baseline rect's arithmetic twice over
         * (2 x 80x16 = 2560), and runs= is what the whole
         * disjoint-vs-nested comparison is read from -- so pin both rather
         * than inferring either from the verdict. */
        if (strcmp(c->id, "path/two-disjoint-bars") == 0)
            CHECK_CASE(strstr(r.detail, "runs=2,expect=2,fill=2560") != NULL,
                       c->id, "two bands and the exact analytic area");

        /* ★★ THE ANALYTIC-VS-MODEL CROSS-CHECK, which is what makes the
         * coverage tolerances in the file under test defensible rather than
         * asserted. Every case with a coverage expectation must report
         * cover=ok against a CORRECT rasteriser -- and for the axis-aligned
         * cases the model's fill IS the analytic area, exactly, so this is a
         * genuine agreement between two independently derived numbers rather
         * than a tolerance absorbing a disagreement. Measured here: rect
         * 6400/6400, four bars 5120/5120, two bars 2560/2560, nested rings
         * 5760/5760, both rings 5376/5376, evenodd's NON_ZERO pass 6400/6400,
         * the pentagram 2792 against an analytic 2792.30. The triangle is the
         * one case where they legitimately differ (1770 vs 1800) -- it has a
         * diagonal, which is precisely why it is in the +/-5% class.
         *
         * path/degenerate-zero-area is the only exemption, and it is a real
         * one: a zero-area path has no correct area, so it reports n/a. */
        if (strcmp(c->id, "path/degenerate-zero-area") == 0)
            CHECK_CASE(strstr(r.detail, "cover=n/a") != NULL, c->id,
                       "no analytic area, so coverage is n/a");
        else
            CHECK_CASE(strstr(r.detail, "cover=ok") != NULL, c->id,
                       "coverage agrees with the analytic area");

        /* The three fills the cases only started REPORTING with the coverage
         * check. Pinned for the same reason the rect's 6400 is: they are the
         * numbers the tolerances are derived from, and a reference rasteriser
         * that drifted would silently move every judgement built on them. */
        if (strcmp(c->id, "path/two-contour-ring-nonzero") == 0 ||
            strcmp(c->id, "path/two-draws-ring") == 0)
            CHECK_CASE(strstr(r.detail, "fill=5376,") != NULL, c->id,
                       "the ring's exact analytic area (6400-1024)");
        if (strcmp(c->id, "path/evenodd-vs-nonzero") == 0)
            CHECK_CASE(strstr(r.detail, "fill=6400,") != NULL, c->id,
                       "NON_ZERO over same-winding nests fills solid");
        if (strcmp(c->id, "path/self-intersecting") == 0)
            CHECK_CASE(strstr(r.detail, "fill=2792,") != NULL, c->id,
                       "the pentagram's NON_ZERO area, model == analytic");
    }
    CHECK(g_parse_error == 0);

    /* ★ Every path in the file under test ends on an explicit VLC_OP_END, so
     * vg_lite_init_path's CLOSE->END fixup -- whose S8 branch writes 4x out of
     * bounds -- must never fire. Nothing on the target can check this; a hit
     * there presents as memory corruption, not as a failed call. */
    CHECK(g_close_fixup_fired == 0);

    /* ---- ARM 2: THIS GC355's DEFECT. The probes must go red. --------- */
    printf("-- arm 2: first-contour-only rasteriser (this GC355's defect)\n");
    g_one_contour_only = 1;
    g_parse_error = 0;
    g_close_fixup_fired = 0;

    for (size_t i = 0; i < vgc_path_case_count; i++) {
        const vgc_case_t *c = &vgc_path_cases[i];
        case_result_t r;
        run_one(c, &r);
        const int probe = is_multi_contour_probe(c->id);
        printf("   %-34s %-6s %s\n", c->id,
               r.verdict == VGC_OK ? "ok" : r.verdict == VGC_BROKEN ? "BROKEN" : "skip",
               r.detail);
        CHECK_CASE(r.verdict == (probe ? VGC_BROKEN : VGC_OK), c->id,
                   probe ? "goes BROKEN on the defect" : "control unaffected");
        CHECK_CASE(r.oob == 0u, c->id, "no out-of-range px read");
    }

    /* ★ BROKEN FOR THE RIGHT REASON. A verdict alone cannot distinguish "this
     * predicate saw the dropped contours" from "this predicate went red for
     * some unrelated reason", and the second would leave the matrix looking
     * healthy while measuring nothing. These pin the SYMPTOM each probe is
     * supposed to report. */
    {
        case_result_t r;
        for (size_t i = 0; i < vgc_path_case_count; i++) {
            const vgc_case_t *c = &vgc_path_cases[i];
            if (!is_multi_contour_probe(c->id)) continue;
            run_one(c, &r);
            if (strncmp(c->id, "path/multi-contour", 18) == 0)
                CHECK_CASE(strstr(r.detail, "runs=1,") != NULL, c->id,
                           "reports one run, not four");
            else if (strcmp(c->id, "path/two-disjoint-bars") == 0)
                CHECK_CASE(strstr(r.detail, "runs=1,") != NULL, c->id,
                           "reports one run, not two");
            else if (strcmp(c->id, "path/four-nested-rings") == 0)
                /* The outer rect alone under NON_ZERO is one solid block, so
                 * the counter reads 1 -- the k=1 cell of its own table. */
                CHECK_CASE(strstr(r.detail, "runs=1,") != NULL, c->id,
                           "reports one run, not four");
            else if (strcmp(c->id, "path/two-contour-ring-nonzero") == 0)
                CHECK_CASE(strstr(r.detail, "rim=1,centre=1") != NULL, c->id,
                           "reports the hole filled in");
            else
                CHECK_CASE(strstr(r.detail, "eo_centre=1") != NULL, c->id,
                           "reports EVEN_ODD failing to cut the hole");
        }
    }
    CHECK(g_parse_error == 0);
    CHECK(g_close_fixup_fired == 0);

    /* ---- ARM 3: a GPU that accepts everything and DRAWS NOTHING. ---------
     * ★ ARM 2 LEAVES A HOLE THIS CLOSES. Under the first-contour model nine of
     * the fifteen cases are controls that must stay ok -- so nine predicates
     * hard-wired to `return VGC_OK` would survive both arms so far. A null GPU
     * is the cheapest model that forces almost all of them to speak: fourteen
     * of fifteen must go broken. The exception is real rather than a
     * concession -- path/degenerate-zero-area accepts "nothing drawn" BY
     * DESIGN (a zero-area path legitimately rasterises to nothing), so it is
     * asserted ok here, which also pins that its `fill == 0` branch is live. */
    printf("-- arm 3: null rasteriser (every call succeeds, nothing is drawn)\n");
    g_one_contour_only = 0;
    g_draw_nothing = 1;
    g_parse_error = 0;

    for (size_t i = 0; i < vgc_path_case_count; i++) {
        const vgc_case_t *c = &vgc_path_cases[i];
        case_result_t r;
        run_one(c, &r);
        const int degenerate = strcmp(c->id, "path/degenerate-zero-area") == 0;
        printf("   %-34s %-6s %s\n", c->id,
               r.verdict == VGC_OK ? "ok" : r.verdict == VGC_BROKEN ? "BROKEN" : "skip",
               r.detail);
        CHECK_CASE(r.verdict == (degenerate ? VGC_OK : VGC_BROKEN), c->id,
                   degenerate ? "accepts an empty render by design"
                              : "goes BROKEN when nothing is drawn");
    }
    g_draw_nothing = 0;
    CHECK(g_parse_error == 0);

    /* ---- ARM 4: a GPU that draws the right SHAPE plus STRAY INK. ---------
     * ★★ WITHOUT THIS ARM THE COVERAGE CHECK'S FAILING BRANCH IS NEVER RUN.
     * Arms 1-3 exercise cover=ok (arm 1) and cover=n/a (arms 2 and 3, where
     * the structural predicate fails first and coverage is not consulted).
     * Nothing reaches cover=stray:/cover=short: -- so the whole check could be
     * hard-wired to pass and all three arms would stay green. That is the
     * positive-only-suite hazard this file's own header names, one level down.
     *
     * The model: the correct rasteriser, PLUS a 20x20 block of the draw colour
     * at x 0..20, y 104..124 -- 400 px, comfortably more than every tolerance
     * in the file under test (the largest is the pentagram's 237, and a block
     * that only just cleared it would make this arm a test of the block's
     * size). It is deliberately
     * clear of every structural sample point (10,10), (20,20), (32,64),
     * (60,60), (64,40), (64,64) and of column 64, which vgc_count_runs_col
     * reads -- so NO structural predicate can see it. That is the point: this
     * is the shape of the real defect, a picture whose structure is right and
     * whose ink is wrong, and before the coverage check every one of these
     * cases reported pixel=ok against it.
     *
     * Fourteen of fifteen must go BROKEN, and BY THE COVERAGE FIELD -- the
     * detail must carry cover=stray:, not merely a red verdict, since a
     * verdict alone cannot distinguish "coverage caught it" from "something
     * else went wrong". path/degenerate-zero-area is exempt because it inks
     * nothing and so is never given the block (see vgc_draw_path). */
    printf("-- arm 4: stray-ink rasteriser (right shape, %d px that are not in the path)\n",
           STRAY_PX);
    g_stray_ink = 1;
    g_parse_error = 0;

    for (size_t i = 0; i < vgc_path_case_count; i++) {
        const vgc_case_t *c = &vgc_path_cases[i];
        case_result_t r;
        run_one(c, &r);
        const int degenerate = strcmp(c->id, "path/degenerate-zero-area") == 0;
        printf("   %-34s %-6s %s\n", c->id,
               r.verdict == VGC_OK ? "ok" : r.verdict == VGC_BROKEN ? "BROKEN" : "skip",
               r.detail);
        CHECK_CASE(r.verdict == (degenerate ? VGC_OK : VGC_BROKEN), c->id,
                   degenerate ? "inks nothing, so it never gets stray ink"
                              : "goes BROKEN on ink that is not in the path");
        if (!degenerate)
            CHECK_CASE(strstr(r.detail, "cover=stray:") != NULL, c->id,
                       "and goes broken BY THE COVERAGE FIELD");
    }
    g_stray_ink = 0;
    CHECK(g_parse_error == 0);

    printf("--\n");
    if (failed) {
        printf("cases_path_geom_test: FAILED (%d of %d checks)\n", failed, checks);
        return 1;
    }
    printf("cases_path_geom_test: OK (%d checks)\n", checks);
    return 0;
}
