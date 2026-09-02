/* Host-compiled test for the conformance probe's PHASE 3 CASES -- images,
 * blits and scissor. Build: tests/run.sh
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * ★★ WHAT THIS TEST IS. It runs the REAL vgc_cases_blit.cpp against SIX
 * models of a GPU under a model of the driver's scissor bookkeeping and
 * source-stride check (model.h, from source):
 *   1 CORRECT: right/bottom clip through 0x0A13 in every regime, left/top only
 *     through the tess-window clamp that the fullscreen regime skips; the
 *     stride check refuses a misaligned source. All six as pre-registered.
 *   2 DRAWS NOTHING: every case broken EXCEPT blit/stride-unaligned, whose
 *     defined outcome IS "nothing drawn" -- pinned, as the path suite pins
 *     degenerate-zero-area under its null arm.
 *   3 IGNORES THE SCISSOR: both scissor cases broken with L=0,T=0,R=0,B=0 --
 *     which is what distinguishes "scissor dead" from "left/top lost". Blits
 *     untouched.
 *   4 CLIPS ALL FOUR IN FULLSCREEN (the fader's warning false):
 *     scissor/tess-fullscreen goes OK; nothing else moves. The arm that proves
 *     the case can see the question at all.
 *   5 WIDTH AS PITCH: the GPU walks source rows by width*bpp. blit/stride-64
 *     is sheared and goes broken; basic and formats have stride == pitch and
 *     are untouched; unaligned is refused before any row is read.
 *   6 ALIGNMENT CHECK ABSENT: the misaligned blit DRAWS. api=success and a
 *     checker that lands, so stride-unaligned must go broken by name.
 *
 * ★ THE NEGATIVE ARMS ARE THE POINT. Demonstrated before this suite was
 * trusted: check_scissor_basic hard-wired to VGC_OK leaves arm 1 green and is
 * caught by arms 2 and 3 by name.
 *
 * It is NOT a statement about the silicon. No GPU is involved. */
#include "model.h"
#include "harness_mirror.h"
#include "../vgc_harness.h"
#include "../vgc_color.h"
#include <stdio.h>
#include <string.h>

static int failed = 0, checks = 0;
#define CHECK(cond) do { checks++; if (cond) printf("PASS: %s\n", #cond); \
    else { printf("FAIL: %s  (line %d)\n", #cond, __LINE__); failed++; } } while (0)
#define CHECK_CASE(cond, id, what) do { checks++; if (cond) printf("PASS: %s %s\n", (id), (what)); \
    else { printf("FAIL: %s %s  (line %d)\n", (id), (what), __LINE__); failed++; } } while (0)

#define ID_BASIC   "blit/basic"
#define ID_S64     "blit/stride-64"
#define ID_UNAL    "blit/stride-unaligned"
#define ID_FMT     "blit/formats"
#define ID_SC      "scissor/basic"
#define ID_SCF     "scissor/tess-fullscreen"

static int is_id(const vgc_case_t *c, const char *id) { return strcmp(c->id, id) == 0; }

/* The fullscreen case renders into vgc_small, which run_one's clear does not
 * touch; the case clears it itself, so nothing extra is needed here -- but
 * the blit sources are CPU-filled statics, and the model reads them through
 * the same pointers the target would, so nothing to reset there either. */
static void arm_verdicts(const char *label, int (*expect_broken)(const vgc_case_t *),
                         const char *why_ok, const char *why_broken,
                         void (*extra)(const vgc_case_t *, const case_result_t *))
{
    printf("-- %s\n", label);
    for (size_t i = 0; i < vgc_blit_case_count; i++) {
        const vgc_case_t *c = &vgc_blit_cases[i];
        case_result_t r;
        run_one(c, &r);
        const int broken = expect_broken(c);
        printf("   %-26s %-6s %s\n", c->id,
               r.verdict == VGC_OK ? "ok" : r.verdict == VGC_BROKEN ? "BROKEN" : "skip", r.detail);
        CHECK_CASE(r.verdict == (broken ? VGC_BROKEN : VGC_OK), c->id, broken ? why_broken : why_ok);
        CHECK_CASE(r.oob == 0u, c->id, "no out-of-range px read");
        /* Every case returns SUCCESS to the harness -- INCLUDING stride-unaligned,
         * which records its (expected) refusal in detail= precisely so that the
         * api= column does not count a correct refusal as a failed call. */
        CHECK_CASE(r.api == VG_LITE_SUCCESS, c->id, "reports SUCCESS to the harness");
        if (extra) extra(c, &r);
    }
}

static int expect_all_but_unal(const vgc_case_t *c) { return !is_id(c, ID_UNAL); }
static int expect_scissors(const vgc_case_t *c) { return is_id(c, ID_SC) || is_id(c, ID_SCF); }
static int expect_scf_ok_only(const vgc_case_t *c) { (void)c; return 0; }   /* arm 4: nothing broken */
/* Arms 5 and 6 fix nothing about the fullscreen regime, so tess-fullscreen
 * stays at its pre-registered broken under both. */
static int expect_s64(const vgc_case_t *c)      { return is_id(c, ID_S64) || is_id(c, ID_SCF); }
static int expect_unal(const vgc_case_t *c)     { return is_id(c, ID_UNAL) || is_id(c, ID_SCF); }

/* ★ ARM 1 IS NOT ALL-OK: scissor/tess-fullscreen is pre-registered BROKEN
 * (L=0,T=0,R=1,B=1), and the correct model reproduces it. */
static int expect_correct(const vgc_case_t *c)  { return is_id(c, ID_SCF); }

static void arm1_pins(const vgc_case_t *c, const case_result_t *r)
{
    if (is_id(c, ID_BASIC) || is_id(c, ID_S64))
        CHECK_CASE(strstr(r->detail, "c00=255.0.0,c10=0.0.255,c01=0.0.255,c11=255.0.0,out=0.0.0") != NULL,
                   c->id, "the checker lands, the outside stays black");
    if (is_id(c, ID_FMT))
        CHECK_CASE(strstr(r->detail, "c00=248.0.0,c10=0.0.248,c01=0.0.248,c11=248.0.0,out=0.0.0,order=low") != NULL,
                   c->id, "5-bit channels expand by shift to 248; red read from the LOW bits");
    if (is_id(c, ID_UNAL))
        CHECK_CASE(strstr(r->detail, "rc=1,refused=1,untouched=1") != NULL,
                   c->id, "refused with INVALID_ARGUMENT (1), nothing drawn");
    if (is_id(c, ID_SC))
        CHECK_CASE(strstr(r->detail, "L=1,T=1,R=1,B=1,in=1") != NULL, c->id,
                   "multi-tile regime: all four edges clip");
    if (is_id(c, ID_SCF))
        CHECK_CASE(strstr(r->detail, "L=0,T=0,R=1,B=1,in=1") != NULL, c->id,
                   "fullscreen regime: left/top painted past the scissor, right/bottom clipped");
    CHECK_CASE(strstr(r->detail, "cover=n/a") != NULL, c->id, "coverage n/a, still printed");
}

static void arm2_pins(const vgc_case_t *c, const case_result_t *r)
{
    /* ★ THE ONE CASE THAT STAYS GREEN WHEN NOTHING IS DRAWN, and it must be
     * green for the RIGHT reason: the refusal still happened. */
    if (is_id(c, ID_UNAL))
        CHECK_CASE(strstr(r->detail, "refused=1,untouched=1") != NULL, c->id,
                   "still refused; 'nothing drawn' is its defined outcome");
}

static void arm3_pins(const vgc_case_t *c, const case_result_t *r)
{
    if (is_id(c, ID_SC) || is_id(c, ID_SCF))
        CHECK_CASE(strstr(r->detail, "L=0,T=0,R=0,B=0,in=1") != NULL, c->id,
                   "all four edges painted past the scissor: a DEAD scissor, not a lost pair");
}

static void arm4_pins(const vgc_case_t *c, const case_result_t *r)
{
    /* ★★ THE PIN THIS SUITE EXISTS FOR. Under a GPU that clips all four edges
     * in the fullscreen regime the case must go OK -- proving the pre-
     * registered broken is a claim the case can see falsified, not a verdict
     * it is wired to produce. */
    if (is_id(c, ID_SCF))
        CHECK_CASE(strstr(r->detail, "L=1,T=1,R=1,B=1,in=1") != NULL, c->id,
                   "the fader's warning falsified: every edge clips");
}

static void arm5_pins(const vgc_case_t *c, const case_result_t *r)
{
    /* Sheared: rows walked at 64 B instead of 128 B, so target row y reads
     * memory row y/2. The second cell row's samples (y = +6) therefore read
     * memory row 3 -- still the FIRST cell row -- and the checker comes back
     * with its second row INVERTED, not black. (An earlier draft of this pin
     * said black, reasoning from the padding; the model said otherwise and
     * the model was right: the padding is only reached at target rows >= 32.) */
    if (is_id(c, ID_S64))
        CHECK_CASE(strstr(r->detail, "c00=255.0.0,c10=0.0.255,c01=255.0.0,c11=0.0.255") != NULL, c->id,
                   "walking rows by width*bpp inverts the second cell row");
    if (is_id(c, ID_BASIC) || is_id(c, ID_FMT))
        CHECK_CASE(r->verdict == VGC_OK, c->id, "stride == pitch here, so untouched");
}

static void arm6_pins(const vgc_case_t *c, const case_result_t *r)
{
    if (is_id(c, ID_UNAL))
        CHECK_CASE(strstr(r->detail, "rc=0,refused=0,untouched=0") != NULL, c->id,
                   "with no check the misaligned blit DRAWS -- and that is the broken outcome");
}

int main(void)
{
    memset(&vgc_scratch, 0, sizeof(vgc_scratch));
    CHECK(vgc_blit_case_count == 6);

    g_permute_rb = g_draw_nothing = g_double_premul = g_alpha_ignoring = 0;
    g_reading_a = g_none_modulates = 0;
    g_draw_black = g_paint_follows_path = g_solid_first_stop = g_ramp_permute_rb = 0;
    g_ignore_scissor = g_fullscreen_clips_all = g_width_as_pitch = g_no_align_check = 0;
    g_parse_error = 0; g_close_fixup_fired = 0;

    arm_verdicts("arm 1: correct GPU under the driver as read", expect_correct,
                 "verdict ok", "goes BROKEN: left/top lost in the fullscreen regime", arm1_pins);
    for (size_t i = 0; i < vgc_blit_case_count; i++) {
        const vgc_case_t *c = &vgc_blit_cases[i];
        case_result_t r; run_one(c, &r);
        CHECK_CASE(r.api2 == r.api, c->id, "second run's status matches");
        CHECK_CASE(r.repeat_same, c->id, "repeat identical");
        CHECK_CASE(r.hook_distinct, c->id, "sum hook is not the live buffer");
    }
    CHECK(g_parse_error == 0);
    CHECK(g_close_fixup_fired == 0);
    /* No scissor may leak out of any case: the last thing every scissor case
     * does is disable it, and this is the only place that can see it did. */
    CHECK(g_sc_set == 0);

    g_draw_nothing = 1;
    arm_verdicts("arm 2: null rasteriser (nothing is drawn)", expect_all_but_unal,
                 "'nothing drawn' IS its defined outcome", "goes BROKEN when nothing is drawn", arm2_pins);
    g_draw_nothing = 0;

    g_ignore_scissor = 1;
    arm_verdicts("arm 3: the scissor is DEAD", expect_scissors,
                 "blits are untouched by the scissor", "goes BROKEN on every edge", arm3_pins);
    g_ignore_scissor = 0;

    g_fullscreen_clips_all = 1;
    arm_verdicts("arm 4: a GPU that clips all four edges in the fullscreen regime", expect_scf_ok_only,
                 "ok -- and for tess-fullscreen that is the prediction INVERTED", "(unreachable)", arm4_pins);
    g_fullscreen_clips_all = 0;

    g_width_as_pitch = 1;
    arm_verdicts("arm 5: source rows walked by width*bpp, not stride", expect_s64,
                 "stride equals pitch, or refused first", "goes BROKEN: sheared", arm5_pins);
    g_width_as_pitch = 0;

    g_no_align_check = 1;
    arm_verdicts("arm 6: the driver's stride check absent", expect_unal,
                 "aligned anyway", "goes BROKEN: the misaligned blit was not refused", arm6_pins);
    g_no_align_check = 0;

    printf("--\n");
    if (failed) { printf("cases_blit_test: FAILED (%d of %d checks)\n", failed, checks); return 1; }
    printf("cases_blit_test: OK (%d checks)\n", checks);
    return 0;
}
