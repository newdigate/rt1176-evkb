/* Host-compiled test for the conformance probe's COLOUR AND BLEND CASES.
 * Build: tests/run.sh
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * ★★ WHAT THIS TEST IS, AND WHAT IT IS NOT. Read this before quoting a green
 * run at anybody.
 *
 * It IS an exercise of vgc_cases_color.cpp's own sample point, tolerances,
 * readings and predicates -- the REAL run()/check()/sum() functions, linked and
 * called -- against SEVEN MODELS of a GPU:
 *   1 a CORRECT one (the shared scanline rasteriser in model.h, blending
 *     SRC_OVER as reading B -- the operator MEASURED on silicon 2026-09-02 --
 *     and honouring the measured ABGR->ARGB store), under which all five cases
 *     must report ok;
 *   2 one that draws NOTHING, under which all five must go broken;
 *   3 one that DOUBLE-PREMULTIPLIES, under which case 2 must go broken at its
 *     NAMED value v=64 and cases 1 and 5 -- both BLEND_NONE -- must stay ok;
 *   4 one that PERMUTES R AND B (the swizzle vglite_probe measured not
 *     happening), under which case 1 ALONE must go broken and cases 2-5 must
 *     stay ok;
 *   5 one that IGNORES ALPHA -- writes the source raw whatever the mode says --
 *     under which cases 2, 3 and 4 must go broken VIA THE ALPHA FIELD, with
 *     cases 1 and 5 staying ok;
 *   6 one implementing READING A of SRC_OVER, `S*Sa + D*(1 - Sa)` -- the other
 *     admissible reading of the header, and what this model itself implemented
 *     before the boot -- under which cases 2 and 3 must go broken WITH
 *     `model=A` on the line; and
 *   7 one whose BLEND_NONE MODULATES by the source alpha, under which case 5
 *     ALONE must go broken with `read=modulated` on the line.
 *
 * ★★ ARMS 6 AND 7 ARE WHAT MAKES THE MEASURED READING AN ASSERTION RATHER THAN
 * A COMMENT, and they are the reason this file changed on 2026-09-02. Cases 2,
 * 3 and 5 used to return ok under EITHER reading and merely report which in
 * detail=. tools/vglite-conformance-check.sh compares only
 * `<id> <pixel> <repeat>`, so such a case is INVISIBLE to it: an SDK re-vendor
 * that flipped SRC_OVER from B to A, or BLEND_NONE from raw to modulated,
 * would have left every verdict `ok` and the drift checker GREEN -- exactly
 * the "quirk that silently disappears" expected_silicon.txt's header exists to
 * catch. The cases now PIN the measured reading, and a pin nobody has
 * demonstrated RED is decoration. Arms 6 and 7 are that demonstration, and
 * they pin the SYMPTOM (`model=A`, `read=modulated`) and not merely the
 * verdict, so a case that went red for an unrelated reason would not satisfy
 * them.
 *
 * ★ THE SIX NEGATIVE ARMS ARE THE POINT, and this is the Phase 1 lesson
 * repeated rather than a new claim. Measured there: a case hard-wired to
 * `return VGC_OK` leaves the correct arm GREEN -- it would have reported ok
 * anyway -- and is caught ONLY by an arm that forces the case to speak. A
 * positive-only suite is equally consistent with a matrix that cannot detect
 * anything. Demonstrated again for THIS suite before it was trusted:
 * check_srcover_arith hard-wired to VGC_OK left arm 1 green and was caught by
 * arms 2, 3 and 5 by name.
 *
 * ★★ ARMS 3 TO 7 EACH CLOSE A HOLE THE OTHERS LEAVE, which is why there are
 * five of them and not one:
 *   - Arm 2 (draws nothing) breaks everything, so it cannot say WHICH field of
 *     any case is doing the work.
 *   - Arm 5 is the only one that can reach cases 2-4's ALPHA check. It CANNOT
 *     be seen on the colour channel at all: with a saturated white source,
 *     reading B's `S + D*(1 - Sa)` is observationally identical to a raw store
 *     (255 over black, 287 clamped to 255 over grey), so a verdict computed
 *     from v= alone reports ok for an alpha-ignoring GPU. That was MEASURED
 *     while the cases were written, and it is why the alpha row is judged.
 *   - Arm 4 is the only one that can break case 1, because case 1 is the only
 *     case that reads a raw word rather than a channel-symmetric grey.
 *   - Arm 3 reaches case 2's named defect, the one value in the whole colour
 *     matrix that is called out by name in the case's own comment.
 *   - Arm 6 is the only one that can move SRC_OVER to the OTHER ADMISSIBLE
 *     reading rather than to a defect. Nothing else in the suite distinguishes
 *     "the hardware changed its mind" from "the hardware broke", and the first
 *     is the one the drift checker exists for.
 *   - Arm 7 is the only one that can produce case 5's other admissible
 *     reading. Note what it is NOT: an arm that made BLEND_NONE silently
 *     COMPOSITE would not break case 5 at all, because under the measured
 *     reading B that composite CLAMPS to 255 -- the same colour a raw store
 *     writes. Measured: the case then reports `v=255,a=255,read=raw`, ok.
 *     Modulation is the reachable half; see the note above arm 7.
 *
 * ★ AND ARM 4's GREEN HALF IS AN ASSERTION, NOT A BYSTANDER. The Phase 2
 * spec's section 7 claims cases 2-5 use greyscale so that a word-order fault
 * surfaces in EXACTLY ONE PLACE. If cases 2-4 also broke under permutation
 * that claim would be false and the whole diagnosis argument with it, so this
 * suite asserts they stay ok rather than merely printing that they did.
 *
 * It is NOT a statement about what the real silicon does. Not one line here
 * touches a GPU. Model.h implements reading B BECAUSE the hardware was
 * measured doing reading B -- two boots, 2026-09-02, recorded in the example's
 * transcript_hw_evkb.txt -- so arm 1 is a model of THAT hardware and not an
 * independent vote for it. Nothing in this file can confirm, contradict or
 * substitute for the silicon's own answers. A future reader who reports "the
 * colour cases pass" on the strength of this suite has said something true and
 * useless. Silicon wins; this is the instrument's calibration, taken before
 * the instrument is pointed at anything.
 *
 * ★ WHY IT EXISTS AT ALL: the QEMU gate cannot reach this code. QEMU has no
 * GC355, so the chip-ID probe reads 0 and every case reports pixel=skip --
 * meaning a green gate says NOTHING about the sample point, the tolerances,
 * the reading bands or the alpha check. Between the gate and the one silicon
 * boot there is no other check on any of it.
 *
 * ★ C++ RATHER THAN C, for the same reason arena_test and cases_path_geom_test
 * are: vgc_harness.h wraps its vg_lite.h include in `extern "C"` and the code
 * under test is compiled as C++ on the target. */
/* The model (rasteriser, blend, harness services, arm switches) is in model.h
 * and the case lifecycle is in harness_mirror.h -- both shared with
 * cases_path_geom_test.cpp, so the two suites cannot drift on either. What
 * stays HERE is everything that knows what a colour case is called: the seven
 * arm drivers and their expectations. */
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
        if (cond) {                                                      \
            printf("PASS: %s\n", #cond);                                 \
        } else {                                                         \
            printf("FAIL: %s  (line %d)\n", #cond, __LINE__);            \
            failed++;                                                    \
        }                                                                \
    } while (0)

/* Per-case variant: the failure message has to name the CASE, since the same
 * source line runs for all five and `#cond` alone would not say which one went
 * red. */
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

/* ---- the case ids, once ----------------------------------------------------
 * Spelled here rather than at fifteen call sites so a rename in the table
 * turns this file red in ONE place -- and red it must go: the ids are what
 * run_qemu.sh and expected_silicon.txt key on. */
#define ID_WORD   "color/solid-word-order"
#define ID_PREMUL "color/premultiplied-srcover"
#define ID_ARITH  "blend/srcover-arithmetic"
#define ID_DOUBLE "blend/srcover-double"
#define ID_NONE   "blend/none-honours-alpha"

static int is_id(const vgc_case_t *c, const char *id)
{
    return strcmp(c->id, id) == 0;
}

/* The two BLEND_NONE cases. Arms 3 and 5 both patch SRC_OVER only, so these
 * two must be untouched by either -- which is the half of each arm that says
 * the arm models a BLEND defect rather than a broken rasteriser. */
static int is_blend_none_case(const vgc_case_t *c)
{
    return is_id(c, ID_WORD) || is_id(c, ID_NONE);
}

/* Run every case under the arm currently selected, print one line each, and
 * assert the verdict each case is expected to reach. `expect_broken` decides
 * per case; `why_ok`/`why_broken` name what the assertion is about. */
static void arm_verdicts(const char *label,
                         int (*expect_broken)(const vgc_case_t *),
                         const char *why_ok, const char *why_broken,
                         void (*extra)(const vgc_case_t *, const case_result_t *))
{
    printf("-- %s\n", label);
    for (size_t i = 0; i < vgc_color_case_count; i++) {
        const vgc_case_t *c = &vgc_color_cases[i];
        case_result_t r;
        run_one(c, &r);
        const int broken = expect_broken(c);
        printf("   %-30s %-6s %s\n", c->id,
               r.verdict == VGC_OK ? "ok" : r.verdict == VGC_BROKEN ? "BROKEN" : "skip",
               r.detail);
        CHECK_CASE(r.verdict == (broken ? VGC_BROKEN : VGC_OK), c->id,
                   broken ? why_broken : why_ok);
        CHECK_CASE(r.oob == 0u, c->id, "no out-of-range px read");
        if (extra) extra(c, &r);
    }
}

static int expect_none(const vgc_case_t *c)  { (void)c; return 0; }
static int expect_all(const vgc_case_t *c)   { (void)c; return 1; }

/* Arm 3: only the two SRC_OVER-with-a-colour-term cases and the double see the
 * doubled premultiply; the two BLEND_NONE cases are untouched. */
static int expect_double_premul(const vgc_case_t *c)
{
    return !is_blend_none_case(c);
}

/* Arm 4: case 1 ALONE. See the section-7 note in the header -- this is the
 * spec claim, asserted. */
static int expect_permute(const vgc_case_t *c)
{
    return is_id(c, ID_WORD);
}

/* Arm 5: cases 2, 3 and 4 -- the three that judge SRC_OVER's alpha row. */
static int expect_alpha_ignoring(const vgc_case_t *c)
{
    return !is_blend_none_case(c);
}

/* Arm 6: cases 2 and 3 ALONE -- the two that PIN a reading.
 *
 * ★★ CASE 4 STAYS GREEN HERE, AND THAT WAS MEASURED BEFORE IT WAS WRITTEN
 * DOWN, not assumed from its comment. blend/srcover-double pins no absolute
 * value: it predicts v2 from the MEASURED v1 with the reading-A form of
 * SRC_OVER (srcover_predict), which is exact under reading A and saturates
 * under reading B, so BOTH readings satisfy it. Under this arm its inputs move
 * (v1 goes 255 -> 160, v2 255 -> 208) and the prediction moves with them:
 * pred = (255*128 + 160*127 + 127)/255 = 208, which is what v2 is. Its alpha
 * row is untouched by this arm, so a=255 still holds.
 *
 * ★ THAT IS THE CASE'S DESIGN WORKING, NOT A GAP IN THIS ARM. "Reading-agnostic
 * by construction" is the claim vgc_cases_color.cpp makes about case 4 in
 * capitals, and until this arm existed nothing in the tree could test it. A
 * green case 4 beside a red case 2 and 3 IS the assertion: the operator moved,
 * the two absolute cases said so, and the self-consistency case correctly
 * declined to. If case 4 ever goes red here, either the prediction or the model
 * has drifted -- and the arm1/arm6 value pins below say which. */
static int expect_reading_a(const vgc_case_t *c)
{
    return is_id(c, ID_PREMUL) || is_id(c, ID_ARITH);
}

/* Arm 7: case 5 ALONE. The only case whose blend mode this arm touches with a
 * non-opaque source -- color/solid-word-order is BLEND_NONE too but draws at
 * a = 255, where modulating by alpha is the identity. */
static int expect_none_modulates(const vgc_case_t *c)
{
    return is_id(c, ID_NONE);
}

/* ---- arm 1's numeric pins --------------------------------------------------
 * ★ A VERDICT ALONE IS NOT ENOUGH HERE, for the reason arm 1 of the path suite
 * pins fill=6400: `ok` is reachable from more than one picture. Case 2 admits
 * BOTH readings, so ok says only "one of 128 or 255"; case 4 predicts v2 from
 * a measured v1, so ok says only "self-consistent". Pinning the exact values
 * this model produces turns a drifting reference rasteriser into a visible
 * failure instead of a silently moved judgement.
 *
 * ★ THE NUMBERS ARE THE READING-B ARITHMETIC SINCE 2026-09-02, derived
 * independently in vgc_cases_color.cpp's own constants and a third time by
 * hand in expected_silicon.txt. a = 128/255 = 0.50196, 1 - a = 127/255:
 *   case 2, over BLACK:        255 + 0*(1-a)     = 255
 *   case 3, over GREY 0x40:    255 + 64*(1-a)    = 286.9, CLAMPED to 255
 *   case 4, second composite:  255 + 255*(1-a)   = 382,   clamped to 255,
 *                              against pred = (255*128 + 255*127 + 127)/255
 *                                            = 255.49 -> 255
 *   alpha row, all three:      128 + 255*(1-a)   = 255.0  (Sa + Da*(1 - Sa))
 * The reading-A values these pins carried until that date (128 / 160 /
 * v1=160,v2=208) have not been deleted: they are arm 6's pins now, which is
 * the point of that arm.
 *
 * ★ CASE 3's CLAMP IS DOING REAL WORK IN THIS PIN, not decoration. Its
 * unclamped 286.9 is what makes reading B's answer over a NON-BLACK backdrop
 * indistinguishable from a raw store -- the very reason cases 2-4 have to
 * judge the alpha row at all (see arm 5).
 *
 * ★★ AND SINCE THE MODEL MOVED TO READING B, ARM 1's FIVE DETAIL STRINGS ARE
 * BYTE-IDENTICAL TO THE SILICON TRANSCRIPT'S -- checked by eye against
 * examples/display/vglite_conformance/transcript_hw_evkb.txt, all five colour
 * lines, 2026-09-02. That is a check the reading-A model could not have passed
 * and it is worth re-running whenever either side moves. It is NOT a second
 * measurement: the model was changed to match the boot, so the agreement is
 * construction, not evidence. What it DOES prove is that the case functions
 * read the two the same way -- the failure it would catch is a predicate that
 * behaves differently on the target than on the host. */
static void arm1_pins(const vgc_case_t *c, const case_result_t *r)
{
    if (is_id(c, ID_PREMUL))
        CHECK_CASE(strstr(r->detail, "v=255,a=255,model=B") != NULL, c->id,
                   "reading B over black, alpha row composited to 255");
    if (is_id(c, ID_ARITH))
        CHECK_CASE(strstr(r->detail, "v=255,a=255,model=B") != NULL, c->id,
                   "reading B over grey 0x40 (clamped), alpha row 255");
    if (is_id(c, ID_DOUBLE))
        CHECK_CASE(strstr(r->detail, "v1=255,v2=255,a=255,pred=255") != NULL,
                   c->id, "the second composite lands on its own prediction");
    /* ★ CASE 5's GREEN IS THE LOAD-BEARING ONE IN THIS ARM, not a formality.
     * BLEND_NONE's alpha row is `A: Sa` (inc/vg_lite.h:459-460), so a raw store
     * leaving a=128 is CORRECT -- and the case is deliberately the one that
     * does NOT judge alpha. Pinning a=128 beside read=raw is what says the case
     * saw the un-composited alpha and accepted it on purpose, rather than
     * having no opinion because nobody looked. */
    if (is_id(c, ID_NONE))
        CHECK_CASE(strstr(r->detail, "v=255,a=128,read=raw") != NULL, c->id,
                   "BLEND_NONE stores raw and leaves the source's own alpha");
    /* Every colour case reads one interior pixel and asks nothing about area,
     * so all five must print cover=n/a -- and printing it is what keeps "grep
     * for a case line without cover=" a grep for nothing. */
    CHECK_CASE(strstr(r->detail, "cover=n/a") != NULL, c->id,
               "coverage is n/a, and still printed");
}

/* ---- arm 3's named symptom -------------------------------------------------
 * ★ BROKEN FOR THE RIGHT REASON. vgc_cases_color.cpp names ~64 -- alpha applied
 * twice, 255*a*a = 64.3 -- as color/premultiplied-srcover's failure mode. A red
 * verdict alone cannot distinguish that from the case having gone red for some
 * unrelated reason, which would leave the matrix looking healthy while
 * measuring nothing. */
static void arm3_pins(const vgc_case_t *c, const case_result_t *r)
{
    if (is_id(c, ID_PREMUL))
        CHECK_CASE(strstr(r->detail, "v=64,") != NULL, c->id,
                   "breaks at its NAMED value, 255*a*a = 64");
    /* Its two partners break at values that follow from the same defect and
     * are pinned so a change in either is visible: over grey 0x40 the doubled
     * source gives 128*a + 64*(1-a) = 96.6 -> 96, and the double case's second
     * composite lands 64 short of the prediction its own measured v1 makes. */
    if (is_id(c, ID_ARITH))
        CHECK_CASE(strstr(r->detail, "v=96,") != NULL, c->id,
                   "the destination term is intact; only the source doubled");
    if (is_id(c, ID_DOUBLE))
        CHECK_CASE(strstr(r->detail, "v1=96,v2=112,a=255,pred=176") != NULL,
                   c->id, "misses its own prediction by 64");
    /* ★ AND THE ALPHA ROW STAYS CONFORMING under this arm -- a=255 on all
     * three. That is what makes arm 3 and arm 5 complementary rather than two
     * spellings of "something is wrong": here the colour field breaks and the
     * alpha field does not, and in arm 5 it is the other way round. */
    if (!is_blend_none_case(c))
        CHECK_CASE(strstr(r->detail, "a=255,") != NULL, c->id,
                   "the alpha row is untouched by a colour-channel defect");
}

/* ---- arm 4's named symptom -------------------------------------------------
 * With the swizzle not happening, the opaque pure-red fill's red byte is not
 * where vgc_ch(px, VGC_R) looks (byte 2) -- it reads the blue 0x00 instead. */
static void arm4_pins(const vgc_case_t *c, const case_result_t *r)
{
    if (is_id(c, ID_WORD))
        CHECK_CASE(strstr(r->detail, "r=0,") != NULL, c->id,
                   "reads 0 where red should be");
}

/* ---- arm 5's named symptom -------------------------------------------------
 * ★★ THE VERDICT MUST COME FROM THE ALPHA FIELD, AND THIS IS WHERE THAT IS
 * PINNED. Section 8 of the Phase 2 spec claims cases 2-4 break here "via the
 * ALPHA channel"; the claim was aspirational until the cases gained the check.
 * Assert both halves: alpha reads the source's un-composited 128 where
 * `Sa + Da*(1 - Sa)` gives 255, AND the colour channel still matches an
 * ADMISSIBLE reading (model=B) -- which is the whole reason v= cannot see this
 * defect and the alpha row must. */
static void arm5_pins(const vgc_case_t *c, const case_result_t *r)
{
    if (is_id(c, ID_PREMUL) || is_id(c, ID_ARITH))
        CHECK_CASE(strstr(r->detail, "v=255,a=128,model=B") != NULL, c->id,
                   "colour channel matches reading B; only alpha shows it");
    if (is_id(c, ID_DOUBLE))
        CHECK_CASE(strstr(r->detail, "v1=255,v2=255,a=128,pred=255") != NULL,
                   c->id, "self-consistent by construction; only alpha shows it");
    /* ★ CASE 5 STAYING GREEN HERE IS THE COMPLEMENT OF THAT CLAIM, and it is
     * the assertion most likely to be got wrong in a later edit. BLEND_NONE's
     * alpha row IS `A: Sa`, so an implementation that "ignores alpha" is doing
     * exactly what the mode specifies -- and a=128 with read=raw is a
     * CONFORMING reading, not a missed defect. Extending cases 2-4's alpha
     * check to case 5 would report broken on correct hardware; this pin is
     * what would go red if somebody did. */
    if (is_id(c, ID_NONE))
        CHECK_CASE(strstr(r->detail, "v=255,a=128,read=raw") != NULL, c->id,
                   "BLEND_NONE's own alpha row makes a=128 correct here");
}

/* ---- arm 6's named symptom -------------------------------------------------
 * ★★ `model=A` IS THE ASSERTION, NOT THE RED VERDICT. The whole reason cases 2
 * and 3 were pinned is that a reading flip used to be SILENT; the value of
 * pinning it is that the transcript now says WHICH WAY it moved. A check on
 * the verdict alone would be satisfied by a case that broke for any reason at
 * all -- including one that had stopped measuring the blend -- and would leave
 * the drift checker's diagnosis exactly as blind as it was before. Both halves
 * are pinned: the reading-A value on v=, and the label beside it. */
static void arm6_pins(const vgc_case_t *c, const case_result_t *r)
{
    if (is_id(c, ID_PREMUL))
        CHECK_CASE(strstr(r->detail, "v=128,a=255,model=A") != NULL, c->id,
                   "breaks at reading A's value 255*a = 128, and SAYS model=A");
    if (is_id(c, ID_ARITH))
        CHECK_CASE(strstr(r->detail, "v=160,a=255,model=A") != NULL, c->id,
                   "breaks at 255*a + 64*(1-a) = 160, and SAYS model=A");
    /* ★ AND THE GREEN CASE IS PINNED TOO -- see expect_reading_a. Case 4 must
     * stay ok, and it must do so having MOVED: v1 160 and v2 208, not the
     * arm-1 pair. A pin on the verdict alone could not tell "reading-agnostic
     * by construction" from "not connected to the blend at all". */
    if (is_id(c, ID_DOUBLE))
        CHECK_CASE(strstr(r->detail, "v1=160,v2=208,a=255,pred=208") != NULL,
                   c->id, "inputs moved to reading A; the prediction moved too");
    /* The alpha row is not what this arm touches, so all three SRC_OVER cases
     * must still read 255 -- which is what says case 2 and 3's red came from
     * the COLOUR field and not from a collaterally broken alpha check. */
    if (!is_blend_none_case(c))
        CHECK_CASE(strstr(r->detail, "a=255,") != NULL, c->id,
                   "the alpha row is the same under both readings (:462)");
}

/* ---- arm 7's named symptom -------------------------------------------------
 * ★ `read=modulated` is case 5's other admissible reading MADE TO HAPPEN, and
 * this is the only thing in the tree that can produce it. C5_MEASURED pins
 * `raw`; without this arm that constant is a pin no test has ever seen fire.
 *
 * ★★ AND NOTE WHICH DEFECT IS *NOT* MODELLED, because the choice was measured
 * rather than preferred. vgc_cases_color.cpp names ~160 -- BLEND_NONE silently
 * COMPOSITING -- as case 5's must-be-broken value. Under the MEASURED reading B
 * that composite is 255 + 64*(1 - a) = 286.9 CLAMPED TO 255 -- the same colour
 * a raw store writes. MEASURED, by routing BLEND_NONE through the reading-B
 * SRC_OVER path in model.h: the case reports `v=255,a=255,read=raw` and stays
 * OK. Only its alpha moves (128 -> 255), and that field is recorded, not
 * judged. An arm modelling that defect would therefore assert nothing.
 * The ~160 band is reachable only from a GPU that is ALSO doing reading A, and
 * that combination is two independent drifts at once. Modulation is the half
 * that is reachable from one, so modulation is the arm. */
static void arm7_pins(const vgc_case_t *c, const case_result_t *r)
{
    if (is_id(c, ID_NONE))
        CHECK_CASE(strstr(r->detail, "v=128,a=128,read=modulated") != NULL,
                   c->id, "breaks at 255*a = 128, and SAYS read=modulated");
    /* ★ CASE 1 STAYING GREEN IS AN ASSERTION ABOUT THE ARM'S SHARPNESS. It is
     * the OTHER BLEND_NONE case, so this arm does reach it -- but it draws an
     * OPAQUE colour, and modulating by a = 255 is the identity. Pinning the
     * unchanged word is what says so, rather than leaving a reader to assume
     * the arm skipped it. */
    if (is_id(c, ID_WORD))
        CHECK_CASE(strstr(r->detail, "px=0xFFFF0000,") != NULL, c->id,
                   "opaque source: modulating by a=255 is the identity");
}

int main(void)
{
    memset(&vgc_scratch, 0, sizeof(vgc_scratch));

    /* The table's size is what the gate and expected_silicon.txt key on, and
     * every arm below iterates it -- so an accidental table edit must not
     * quietly shrink what this suite covers. */
    CHECK(vgc_color_case_count == 5);

    /* ---- the harness colour macro's own identity ---------------------------
     * ★ DEFERRED FROM tests/color_test.c AND IT LANDS HERE. That suite is pure
     * C over vgc_color.h alone; VGC_ABGR/VGC_ABGR_A live in vgc_harness.h,
     * which pulls in vg_lite.h and would force it to C++ plus the stub. This
     * suite includes the harness anyway, so the pin costs nothing here.
     *
     * ★ AND IT IS NOT VACUOUS SINCE PHASE 2 TASK 1 MADE VGC_ABGR *DELEGATE* TO
     * VGC_ABGR_A: the identity now also guards the per-component `& 0xFF`
     * masking, because a component that overflowed its byte would corrupt the
     * neighbour on ONE side of the identity only. Both an in-range triple and
     * an out-of-range one are checked, plus the alpha placement the colour
     * cases depend on (C_SRC_HALF_WHITE is built with VGC_ABGR_A). */
    CHECK(VGC_ABGR_A(0xFFu, 0x12, 0x34, 0x56) == VGC_ABGR(0x12, 0x34, 0x56));
    CHECK(VGC_ABGR_A(0xFFu, 0x00, 0x00, 0x00) == VGC_ABGR(0x00, 0x00, 0x00));
    CHECK(VGC_ABGR_A(0xFFu, 0xFF, 0xFF, 0xFF) == VGC_ABGR(0xFF, 0xFF, 0xFF));
    CHECK(VGC_ABGR_A(0xFFu, 0x1FF, 0x1FF, 0x1FF) == VGC_ABGR(0x1FF, 0x1FF, 0x1FF));
    /* The layout itself, so the identity above cannot pass by both sides being
     * equally wrong: alpha in bits 31:24, then B, G, and RED IN THE LOW BYTE
     * (vg_lite_color_t is ABGR -- see model.h's mem_word note). */
    CHECK(VGC_ABGR_A(0x80u, 0x11, 0x22, 0x33) == 0x80332211u);
    CHECK(VGC_ABGR(0xFF, 0x00, 0x00) == 0xFF0000FFu);
    /* Masking, on the side that would silently carry into the neighbour. */
    CHECK(VGC_ABGR_A(0x1FFu, 0x100, 0x100, 0x100) == 0xFF000000u);

    /* ---- ARM 1: a CORRECT GPU. Everything must be ok. ---------------------- */
    g_permute_rb = g_draw_nothing = g_double_premul = g_alpha_ignoring = 0;
    g_reading_a = g_none_modulates = 0;
    g_parse_error = 0;
    g_close_fixup_fired = 0;
    arm_verdicts("arm 1: correct rasteriser (reading B, measured word order)",
                 expect_none, "verdict ok", "(unreachable)", arm1_pins);

    /* The lifecycle columns the harness prints, exercised on the colour table
     * for the first time. blend/srcover-double is the only case in the tree
     * with a sum() hook besides none, so its "hook is not the live buffer"
     * check is the only place that hook's usefulness is verified at all. */
    for (size_t i = 0; i < vgc_color_case_count; i++) {
        const vgc_case_t *c = &vgc_color_cases[i];
        case_result_t r;
        run_one(c, &r);
        CHECK_CASE(r.api == VG_LITE_SUCCESS, c->id, "arena did not overflow");
        CHECK_CASE(r.api2 == r.api,          c->id, "second run's status matches");
        CHECK_CASE(r.repeat_same,            c->id, "repeat identical");
        CHECK_CASE(r.hook_distinct,          c->id, "sum hook is not the live buffer");
    }
    CHECK(g_parse_error == 0);
    /* Every path these cases emit ends on an explicit VLC_OP_END, so
     * vg_lite_init_path's CLOSE->END fixup -- whose S8 branch writes 4x out of
     * bounds -- must never fire. Nothing on the target can check this. */
    CHECK(g_close_fixup_fired == 0);

    /* ---- ARM 2: a GPU that accepts everything and DRAWS NOTHING. -----------
     * ★ THE ARM THAT CATCHES A HARD-WIRED PREDICATE. Unlike the path table
     * there is no legitimate exemption here: every colour case reads a pixel
     * inside a rect it drew, so an empty render must break all five. */
    g_draw_nothing = 1;
    g_parse_error = 0;
    arm_verdicts("arm 2: null rasteriser (every call succeeds, nothing is drawn)",
                 expect_all, "(unreachable)", "goes BROKEN when nothing is drawn",
                 NULL);
    g_draw_nothing = 0;
    CHECK(g_parse_error == 0);

    /* ---- ARM 3: DOUBLE-PREMULTIPLY. Case 2's named defect. ----------------- */
    g_double_premul = 1;
    g_parse_error = 0;
    arm_verdicts("arm 3: double-premultiplying rasteriser (alpha applied twice)",
                 expect_double_premul,
                 "BLEND_NONE case unaffected by a SRC_OVER defect",
                 "goes BROKEN on the doubled alpha term", arm3_pins);
    g_double_premul = 0;
    CHECK(g_parse_error == 0);

    /* ---- ARM 4: R/B PERMUTATION. Case 1 ALONE. -----------------------------
     * ★★ THIS ARM'S GREEN CASES CARRY AS MUCH WEIGHT AS ITS RED ONE. Section 7
     * of the Phase 2 spec justifies the greyscale in cases 2-5 by claiming a
     * word-order fault surfaces in exactly one place. That is a DESIGN CLAIM
     * about the cases, and this is the only thing in the tree that can measure
     * it -- if cases 2-4 also went red here, the claim and the diagnosis
     * argument built on it would both be wrong. Asserted, not printed. */
    g_permute_rb = 1;
    g_parse_error = 0;
    arm_verdicts("arm 4: R/B-permuting target (the measured swizzle not happening)",
                 expect_permute,
                 "greyscale, so channel-permutation-blind (spec section 7)",
                 "goes BROKEN on the permuted word", arm4_pins);
    g_permute_rb = 0;
    CHECK(g_parse_error == 0);

    /* ---- ARM 5: ALPHA-IGNORING. Cases 2-4, VIA THE ALPHA ROW. -------------- */
    g_alpha_ignoring = 1;
    g_parse_error = 0;
    arm_verdicts("arm 5: alpha-ignoring rasteriser (writes src raw whatever the mode)",
                 expect_alpha_ignoring,
                 "BLEND_NONE's own alpha row is A: Sa, so this is conforming",
                 "goes BROKEN on the alpha row", arm5_pins);
    g_alpha_ignoring = 0;
    CHECK(g_parse_error == 0);

    /* ---- ARM 6: READING A of SRC_OVER. Cases 2 and 3, WITH model=A. --------
     * ★★ THE ARM THE 2026-09-02 PIN EXISTS FOR. Before that pin, cases 2, 3
     * and 5 reported ok under either reading, so this arm would have been
     * ENTIRELY GREEN -- and the drift checker, which reads only the verdict,
     * would have been green with it while the operator had changed underneath.
     * The arm is the demonstration that a flip is now caught, and it pins the
     * DIRECTION (`model=A`) so the transcript still says which way it went. */
    g_reading_a = 1;
    g_parse_error = 0;
    arm_verdicts("arm 6: reading-A rasteriser (S*Sa + D*(1 - Sa), the other reading)",
                 expect_reading_a,
                 "reading-agnostic by construction, or BLEND_NONE",
                 "goes BROKEN because the measured reading is pinned", arm6_pins);
    g_reading_a = 0;
    CHECK(g_parse_error == 0);

    /* ---- ARM 7: BLEND_NONE MODULATES. Case 5 ALONE. ------------------------
     * The same argument as arm 6, for the other pinned constant: C5_MEASURED
     * says `raw`, and this is the only thing in the tree that can make a case
     * say `modulated`. */
    g_none_modulates = 1;
    g_parse_error = 0;
    arm_verdicts("arm 7: BLEND_NONE-modulating rasteriser (dst := S*Sa)",
                 expect_none_modulates,
                 "opaque source or SRC_OVER, so untouched",
                 "goes BROKEN because BLEND_NONE's raw store is pinned",
                 arm7_pins);
    g_none_modulates = 0;
    CHECK(g_parse_error == 0);

    printf("--\n");
    if (failed) {
        printf("cases_color_test: FAILED (%d of %d checks)\n", failed, checks);
        return 1;
    }
    printf("cases_color_test: OK (%d checks)\n", checks);
    return 0;
}
