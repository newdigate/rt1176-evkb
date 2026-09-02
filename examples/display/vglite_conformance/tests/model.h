/* tests/model.h - THE REFERENCE RASTERISER the host case-geometry suites run
 * the real cases against, plus the target-side harness services those cases
 * are specified in terms of.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * ★★ WHAT THIS IS. On the board, vglite_conformance.cpp supplies vgc_fb,
 * vgc_px, vgc_clear, vgc_draw_path and friends on top of the real driver.
 * Every host case-geometry suite has to supply the SAME symbols on top of
 * something it can run without a GPU. This file is that something: a scanline
 * reference rasteriser plus the harness services around it, written to the
 * contracts vgc_harness.h states rather than to whatever is convenient.
 *
 * ★★ WHY IT IS A HEADER RATHER THAN LEFT IN ONE SUITE. Phase 2's colour suite
 * needs a SRC_OVER implementation, and a second copy of a blend formula in a
 * second test file is two things that agree on the day they are written and
 * diverge silently after. That is the identical argument that promoted
 * vgc_draw_path and vgc_fb into the harness in Phase 1, where a case-local
 * fb() would already have disagreed with the harness on the engine-absent
 * path. One rasteriser, one blend, one set of arm switches.
 *
 * ★ WHAT THIS FILE DOES NOT CONTAIN, and must not grow: the case-expectation
 * tables, the arm drivers, and anything that knows what a particular case is
 * called. Those belong to the suite. This file knows about pixels and paths;
 * it does not know that path/four-nested-rings exists.
 *
 * ★ IT SAYS NOTHING ABOUT THE SILICON. Not one line here touches a GPU. It is
 * the instrument's calibration jig, and a green run against it is a statement
 * about the case table, never about the GC355. The silicon's answers live in
 * the example's transcript_hw_evkb.txt and expected_silicon.txt.
 *
 * ---- ON THE STATE BEING BARE `static` -------------------------------------
 * ★ ONE INCLUDER PER LINK, ENFORCED BY THE LINKER RATHER THAN BY CONVENTION,
 * WHICH IS WHY BARE STATICS ARE SAFE HERE. The usual hazard of mutable state
 * in a header -- two TUs each getting their own copy, so an arm switch set in
 * one is invisible to the rasteriser in the other -- cannot happen silently in
 * this file, because it also carries EXTERNAL-LINKAGE definitions
 * (vgc_fb, vgc_px, vgc_clear, vgc_draw_path, vg_lite_init_path, vgc_scratch,
 * ...). A second TU including it is a DUPLICATE SYMBOL at link time, not a
 * second quiet copy of g_stray_ink.
 *
 * ★ VERIFIED, NOT REASONED. Two throwaway TUs including this header and linked
 * together were rejected with FOURTEEN `duplicate symbol` errors -- _vgc_scratch
 * plus thirteen mangled function names (__Z6vgc_fbv, __Z6vgc_pxii,
 * __Z13vgc_draw_pathP12vg_lite_path14vg_lite_fill_tjP15vg_lite_error_t, ...)
 * -- before main() could run. So a suite that ever grows a second TU is told,
 * loudly, at the moment it does. That is a better guarantee than a struct or an
 * accessor could give: both would still hand each TU its own state and keep
 * quiet about it. The statics stay bare, and this note is the reason.
 *
 * (Mangled, not `_vgc_fb`: vgc_harness.h wraps only its vg_lite.h include in
 * extern "C", so the harness's own entry points have C++ linkage on the host.
 * The one unmangled name is the vgc_scratch OBJECT.) */
#ifndef VGC_TEST_MODEL_H
#define VGC_TEST_MODEL_H

#include "../vgc_harness.h"
#include "../vgc_predicates.h"      /* vgc_fnv */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* ---- the scratch buffer and the harness services the TARGET provides -------
 * On the board these live in vglite_conformance.cpp. Re-implemented here
 * exactly as documented in vgc_harness.h -- one access path (vgc_fb), vgc_px
 * counting out-of-range rather than answering, vgc_scratch_sum hashing the
 * flat packed byte count -- because the cases under test are specified against
 * those contracts and a convenient deviation here would test something else. */
static uint32_t s_fb[VGC_W * VGC_H];

/* Declared extern by the harness. The cases never read it (that is the whole
 * point of vgc_fb being the one access path), but the symbol must exist. */
vg_lite_buffer_t vgc_scratch;

const uint32_t *vgc_fb(void) { return s_fb; }

static uint32_t s_oob;
uint32_t vgc_px_oob(void)       { return s_oob; }
void     vgc_px_oob_reset(void) { s_oob = 0; }

uint32_t vgc_px(int x, int y)
{
    if (x < 0 || x >= VGC_W || y < 0 || y >= VGC_H) { s_oob++; return 0u; }
    return s_fb[(size_t)y * VGC_W + (size_t)x];
}

uint32_t vgc_scratch_sum(void) { return vgc_fnv(s_fb, sizeof(s_fb)); }

/* ---- ★★ vg_lite_color_t IN, MEMORY WORD OUT. THE MODEL MODELS THE TARGET. --
 * Every colour that reaches this model is a vg_lite_color_t, which is ABGR --
 * alpha high, RED IN THE LOW BYTE. Every colour a predicate reads back is a
 * scratch memory word, which for this VG_LITE_BGRA8888 target is ARGB -- alpha
 * high, BLUE in the low byte (vgc_color.h: VGC_B=0, VGC_G=1, VGC_R=2,
 * VGC_A=3). The two are not the same order, so the store has to convert, and
 * this is where.
 *
 * ★ THE MAPPING IS MEASURED, NOT A CONVENTION CHOSEN HERE. vglite_probe
 * cleared a VG_LITE_BGRA8888 target with the vg_lite_color_t 0xFF204060 --
 * which the driver reads as ABGR, so B=0x20, G=0x40, R=0x60 -- and memory
 * returned 0xFF604020, red in bits 23:16 (vglite_probe.cpp:56-59, quoted in
 * vgc_color.h). So the conversion swaps bytes 0 and 2 and leaves bytes 1 and 3
 * alone. This function reproduces that reading exactly: feed it 0xFF204060 and
 * it returns 0xFF604020.
 *
 * ★★ WHY CONVERT AT ALL, RATHER THAN LETTING THE CASES SPEAK ABGR. Because the
 * whole design compiles the REAL case functions against this model. A colour
 * case written in vg_lite_color_t order to suit an unswizzled model would be
 * WRONG ON SILICON -- the model would have dictated the cases instead of
 * standing in for the hardware, which is the one thing a stand-in may never
 * do. The model's framebuffer must contain what the real framebuffer contains.
 *
 * ★ AND THIS DOES NOT MAKE ARM 1 CIRCULAR. What is encoded here is a fact
 * measured on silicon, not an assumption the suite then confirms about itself.
 * A green arm 1 says the colour cases read this model correctly; it says
 * NOTHING about whether the hardware still lays bytes out this way. That is
 * color/solid-word-order's job, ON SILICON, ONCE PER BOOT -- the role
 * path/single-contour-rect plays for geometry. Read its result before trusting
 * any colour verdict below it, and never quote a green host suite in its
 * place.
 *
 * ★ INVISIBLE TO PHASE 1, WHICH IS WHY THIS SAT UNDECIDED UNTIL NOW. Every
 * path case draws VGC_FILL_COLOR (0xFFFFFFFF) over VGC_BG_COLOR (0xFF000000);
 * both are FIXED POINTS of a byte-0/byte-2 swap, so no path arm can tell the
 * two orders apart and all 200 of their checks are unmoved by this function
 * existing. If one of them ever moves, the swizzle is in the wrong place. */
/* ---- COLOUR ARM SWITCH 1 of 5: R/B PERMUTING ------------------------------
 * Set by the colour suite's arm 4. Models a target whose memory word is NOT
 * the order vglite_probe measured -- red left in byte 0, blue moved to byte 2,
 * i.e. the swizzle above not happening. Every predicate that reads a NAMED
 * channel (vgc_ch(px, VGC_R) is byte 2) then reads the wrong byte.
 *
 * ★ IT IS THE COLOUR ANALOGUE OF g_one_contour_only, NOT A SECOND SPELLING OF
 * "correct". A GPU that laid bytes out this way would still return SUCCESS
 * from every call and still produce a picture of the right SHAPE -- which is
 * why the matrix needs a case that reads the raw word rather than trusting the
 * channel names, and why that case is the one thing this arm must break.
 *
 * It is OFF by default and touched by no path arm: every path case draws
 * VGC_FILL_COLOR (0xFFFFFFFF) over VGC_BG_COLOR (0xFF000000) and both are
 * fixed points of any byte permutation, so the 200 path checks cannot see this
 * switch in either position.
 * ★ MEASURED BOTH WAYS RATHER THAN ARGUED FROM THE FIXED POINTS: with
 * g_permute_rb forced to 1 for the whole of cases_path_geom_test's main, that
 * suite still reports OK (200 checks) -- the same count and the same checks as
 * with it 0.
 * ★ RE-MEASURED 2026-09-02. This note and the two above it said 199; the path
 * suite has since grown to 200 and the number had gone stale where it stood.
 * The CLAIM was re-run rather than the figure patched -- forced permutation,
 * 200 checks, green -- because a pass count is a claim like any other. */
static int g_permute_rb;

static uint32_t mem_word(uint32_t abgr)
{
    if (g_permute_rb)
        return (abgr & 0xFF00FF00u)            /* alpha and green, as always */
             | (abgr & 0x000000FFu)            /* red stays in byte 0 */
             | (((abgr >> 16) & 0x000000FFu) << 16);  /* blue stays in byte 2 */
    return (abgr & 0xFF00FF00u)            /* alpha (byte 3) and green (byte 1) */
         | ((abgr & 0x000000FFu) << 16)    /* red   byte 0 -> byte 2 */
         | ((abgr >> 16) & 0x000000FFu);   /* blue  byte 2 -> byte 0 */
}

/* ONE clear implementation, and vgc_clear() is vgc_clear_to(VGC_BG_COLOR) --
 * the same shape vglite_conformance.cpp uses, for the same reason: two copies
 * are two chances to diverge on the half a caller cannot see. The parameter is
 * a vg_lite_color_t, exactly as vg_lite_clear's is, so it converts on store
 * like every other colour entering the buffer. */
vg_lite_error_t vgc_clear_to(uint32_t abgr)
{
    const uint32_t w = mem_word(abgr);
    for (size_t i = 0; i < (size_t)VGC_W * VGC_H; i++) s_fb[i] = w;
    return VG_LITE_SUCCESS;
}

vg_lite_error_t vgc_clear(void) { return vgc_clear_to(VGC_BG_COLOR); }

static vg_lite_matrix_t s_ident;
vg_lite_matrix_t *vgc_ident(void) { return &s_ident; }

/* The reference rasteriser is synchronous, so there is nothing to wait for and
 * nothing that can fail. */
void vgc_finish_into(vg_lite_error_t *acc) { (void)acc; }

/* ---- the stubbed driver entry point ---------------------------------------
 * ★ WHAT THIS MODELS AND WHAT IT DOES NOT. It models the three things the
 * cases depend on: it memsets the path, it records format/length/data, and it
 * stores the bounding box. It does NOT model the real function's
 * CLOSE->END fixup (vg_lite_path.c:200-231) -- it only DETECTS whether that
 * fixup would have fired, and counts it. Performing it is not an option: the
 * real S8 branch reads byte num-1 and writes at 4*(num-1), 29 bytes past the
 * end of an 11-byte array, and reproducing an out-of-bounds write in a host
 * test would corrupt the test rather than measure anything. The COUNT is the
 * assertion (see the close_fixup check in each suite's main): every path in
 * the files under test is supposed to end on an explicit VLC_OP_END so the
 * branch never fires, and this is the only place in the tree that can prove it
 * does not. */
static int data_size_of(vg_lite_format_t f)
{
    return f == VG_LITE_S8 ? 1 : f == VG_LITE_S16 ? 2 : 4;
}

static int g_close_fixup_fired;

vg_lite_error_t vg_lite_init_path(vg_lite_path_t *path, vg_lite_format_t format,
                                  vg_lite_quality_t quality, uint32_t length,
                                  void *data, float min_x, float min_y,
                                  float max_x, float max_y)
{
    memset(path, 0, sizeof(*path));
    path->format         = format;
    path->quality        = quality;
    path->path_length    = length;
    path->path           = data;
    path->bounding_box[0] = min_x;
    path->bounding_box[1] = min_y;
    path->bounding_box[2] = max_x;
    path->bounding_box[3] = max_y;

    if (data && length) {
        const size_t ds  = (size_t)data_size_of(format);
        const size_t num = (size_t)length / ds;
        if (num && ((const unsigned char *)data)[(num - 1) * ds] == VLC_OP_CLOSE)
            g_close_fixup_fired++;
    }
    return VG_LITE_SUCCESS;
}

/* ---- the reference rasteriser ---------------------------------------------
 * A model of a CORRECT GPU: it parses the path exactly as the driver lays one
 * out, collects every contour, and fills by winding number (NON_ZERO) or
 * crossing parity (EVEN_ODD) sampled at pixel centres. No antialiasing --
 * deliberately, because the predicates under test threshold at ~50% coverage
 * and a hard-edged reference is the cleanest thing to hold them to. The one
 * cost is that pixel-centre sampling under-counts a diagonal edge, which is
 * why the triangle reads 1770 against its analytic 1800; the +/-8% tolerance
 * in the case under test has to hold that, and this is where that is checked.
 *
 * ★ PATH LAYOUT, taken from the driver rather than guessed (vg_lite_path.c
 * ~line 573: `*(pathc + offset) = cmd[i]; offset++;` then
 * `offset = CDALIGN(offset, data_size);`): an opcode is ONE BYTE at the base
 * of a slot, the cursor then re-aligns to the format's element width, and
 * coordinates follow at that width. That is why (float)VLC_OP_MOVE is not a
 * MOVE -- its first byte is 0x00, VLC_OP_END. */
#define GEOM_MAXPT  256
#define GEOM_MAXCON 32

static float g_ptx[GEOM_MAXPT], g_pty[GEOM_MAXPT];
static int   g_cstart[GEOM_MAXCON], g_clen[GEOM_MAXCON], g_ncon;
static int   g_parse_error;

/* Set by the negative arm: drop every contour after the first, which is
 * exactly what this GC355 does to a multi-contour path. */
static int g_one_contour_only;

static float read_coord(const unsigned char *b, size_t off, vg_lite_format_t f)
{
    if (f == VG_LITE_S8)  { int8_t  v; memcpy(&v, b + off, sizeof(v)); return (float)v; }
    if (f == VG_LITE_S16) { int16_t v; memcpy(&v, b + off, sizeof(v)); return (float)v; }
    if (f == VG_LITE_S32) { int32_t v; memcpy(&v, b + off, sizeof(v)); return (float)v; }
    { float v; memcpy(&v, b + off, sizeof(v)); return v; }
}

static void parse_path(const vg_lite_path_t *p)
{
    const unsigned char *b = (const unsigned char *)p->path;
    const size_t ds = (size_t)data_size_of(p->format);
    size_t off = 0;
    int npt = 0;

    g_ncon = 0;
    if (!b) { g_parse_error++; return; }

    while (off < (size_t)p->path_length) {
        const unsigned char op = b[off];
        off += 1;
        off = (off + ds - 1) / ds * ds;                 /* CDALIGN(offset, ds) */
        if (op == VLC_OP_END)   break;
        if (op == VLC_OP_CLOSE) continue;               /* contours are implicitly closed */
        if (op == VLC_OP_MOVE) {
            if (g_ncon >= GEOM_MAXCON) { g_parse_error++; return; }
            g_cstart[g_ncon] = npt;
            g_clen[g_ncon]   = 0;
            g_ncon++;
        } else if (op != VLC_OP_LINE) {
            g_parse_error++;                            /* the cases emit only these */
            return;
        }
        if (g_ncon == 0 || npt >= GEOM_MAXPT || off + 2 * ds > (size_t)p->path_length) {
            g_parse_error++;
            return;
        }
        g_ptx[npt] = read_coord(b, off, p->format); off += ds;
        g_pty[npt] = read_coord(b, off, p->format); off += ds;
        g_clen[g_ncon - 1]++;
        npt++;
    }
}

/* Set by arm 3: model a GPU that accepts everything and draws NOTHING. */
static int g_draw_nothing;

/* Set by arm 4: model a GPU that draws the right shape AND some ink that is
 * not in the path. See the arm's comment for the block's placement. */
static int g_stray_ink;

#define STRAY_X0 0
#define STRAY_X1 20
#define STRAY_Y0 104
#define STRAY_Y1 124
#define STRAY_PX ((STRAY_X1 - STRAY_X0) * (STRAY_Y1 - STRAY_Y0))   /* 400 */

/* ---- the reference blend --------------------------------------------------
 * SRC_OVER as `src + dst*(1 - a)` on the three colour bytes, clamped to 255,
 * with `a` the source colour's alpha byte scaled 1/255, and `Sa + Da*(1 - Sa)`
 * on the alpha byte.
 *
 * ★ THIS IS ONE OF TWO INDEPENDENT DERIVATIONS, DELIBERATELY. If the model
 * implemented the same formula the case expects and nothing else, arm 1 would
 * prove only that the predicate reads what the model wrote -- circular. Every
 * expected value in vgc_cases_color.cpp is ALSO derived by hand in
 * expected_silicon.txt, and the two must agree. That is the discipline that
 * validated the pentagram in Phase 1b (analytic 2792.30 vs model 2792).
 * ★ THE 2026-09-02 MOVE TO READING B CHANGED WHICH OPERATOR ALL THREE DERIVE
 * FROM, NOT THE DISCIPLINE -- and it did narrow what arm 1 can claim, which is
 * worth being blunt about. Now that the cases PIN the reading this model
 * implements, arm 1 says "the case functions read this model correctly" and no
 * longer carries even the weak independence of admitting two answers. Its
 * remaining non-circular content is that the model computes from the formula
 * while the case's C_EXP* constants are hand-derived and expected_silicon.txt
 * derives them a third time, so a slip in any one of the three is visible.
 *
 * ★★ THE DRIVER'S OWN HEADER SUPPORTS TWO READINGS OF THIS MODE, AND THEY
 * DISAGREE. This model implements ONE OF THEM. Read verbatim from
 * ~/Development/VGLite/inc/vg_lite.h:
 * ★ LINE NUMBERS ARE grep -n ON THAT FILE, NOT COPIED. The Phase 2 spec's
 * version of this list cites :451/:457/:460 for the first three; measured here
 * they are :452/:458/:461. The other two (:481, :137) agree. Same facts,
 * one-off citations -- believe the grep.
 *   :452  "S and D represent source and destination NON-PREMULTIPLIED RGB
 *          color channels."
 *   :454  "SP and DP represent source and destination alpha-premultiplied RGB
 *          color channels (S*Sa, D*Da)."
 *   :458  section heading: "Non-premultiplied Blending modes"
 *   :461  VG_LITE_BLEND_SRC_OVER    = 1     RGB: S + D*(1 - Sa)
 *   :462                                     A:  Sa + Da*(1 - Sa)
 *   :481  VG_LITE_BLEND_NORMAL_LVGL = 11    RGB: S*Sa + D*(1 - Sa)
 *   :136-137  #define VG_LITE_BLEND_PREMULTIPLY_SRC_OVER VG_LITE_BLEND_NORMAL_LVGL
 *
 * Those statements cannot all be taken at face value at once:
 *   - :452 and :458 file mode 1 under NON-premultiplied, but :461's
 *     `S + D*(1 - Sa)` is the PREMULTIPLIED operator -- no `*Sa` term, so a
 *     non-premultiplied S composites at full strength whatever its alpha.
 *   - :481 gives a DIFFERENT enumerator the formula `S*Sa + D*(1 - Sa)`, which
 *     is what :452's own convention makes SRC_OVER mean.
 *   - :136-137 then aliases that one PREMULTIPLY_SRC_OVER. The names and the
 *     formulas are inverted against each other.
 *
 * ★ THE TWO READINGS, LABELLED AS THE PHASE 2 SPEC LABELS THEM (section 3 --
 * keep these letters agreeing with it; A and B swapped across two documents is
 * the exact divergence this shared model exists to prevent):
 *     reading A   S*Sa + D*(1 - Sa)   white @ 0x80 over black -> 128
 *     reading B   S + D*(1 - Sa)      header-literal          -> 255
 *
 * ★★ THIS MODEL IMPLEMENTS READING B, BECAUSE THE HARDWARE WAS MEASURED AND
 * DOES READING B. Silicon, 2026-09-02, TWO BOOTS BYTE-IDENTICAL:
 * color/premultiplied-srcover read `v=255,a=255,model=B` and
 * blend/srcover-arithmetic read `v=255,a=255,model=B`
 * (examples/display/vglite_conformance/transcript_hw_evkb.txt). Until that
 * boot this file implemented reading A as an ADMITTED CHOICE, and said so; the
 * choice is now a finding and the model follows it.
 *
 * ★★ THE MODEL MUST MODEL THE TARGET -- this is the identical argument that
 * put the ABGR->ARGB swizzle in mem_word above. A model that implements an
 * operator the hardware does not makes the REAL case functions, compiled
 * against it, agree with a fiction: arm 1 would be green on cases whose
 * verdicts on silicon are red, which is worse than no arm at all. Reading A
 * did not become wrong when it was measured against -- it became a MODEL OF A
 * DIFFERENT GPU, which is what the reading-A arm below now is.
 *
 * ★ THE CASES NOW PIN THE MEASURED READING RATHER THAN ADMITTING BOTH
 * (C2_MEASURED / C3_MEASURED / C5_MEASURED in vgc_cases_color.cpp), because
 * tools/vglite-conformance-check.sh compares only `<id> <pixel> <repeat>` and
 * a case that returns ok under either reading is INVISIBLE to it. The detail
 * still reports what was seen, so a flip goes broken WITH `model=A` on the
 * line -- and the reading-A arm below is what proves that path is reachable.
 * blend/srcover-double is the one case that pins nothing absolute: it predicts
 * its second composite from its MEASURED first, and stays green under either
 * reading BY CONSTRUCTION (verified, not assumed -- see the arm 6 note in
 * cases_color_test.cpp).
 *
 * ★ THE ALPHA BYTE IS BLENDED BY ITS OWN ROW, `Sa + Da*(1 - Sa)` (:462), not
 * by the colour formula -- `Sa*Sa + Da*(1 - Sa)` is not any convention's
 * SRC_OVER and would read 191 where every compositor reads 255 for a half-
 * alpha source over an opaque backdrop. Byte 3 is alpha in ABGR (what
 * vg_lite_color_t is) and in the ARGB memory word alike, so the special case
 * needs no knowledge of the channel order the rest of the word is in.
 *
 * ★ THE ROUNDING HERE IS ROUND-TO-NEAREST, AND IT IS NOT A DEFINITION OF THE
 * CORRECT ANSWER. Whether the hardware rounds the same way, and whether it
 * scales by /255 or /256, is unknown -- which is exactly what the deliberately
 * generous tolerances in vgc_cases_color.cpp exist to absorb. Do NOT treat
 * this model as correct to within 1 LSB. */
/* ---- COLOUR ARM SWITCH 2 of 5: ALPHA-IGNORING -----------------------------
 * Set by the colour suite's arm 5. Models a GPU that writes the source raw
 * whatever the blend mode says -- the ONE defect a saturated-white source
 * cannot show on the colour channel, because reading B's `S + D*(1 - Sa)` is
 * observationally identical to a raw store when S is 255 (255 over black; 287
 * clamped to 255 over grey 0x40). Measured, and it is why cases 2-4 judge the
 * ALPHA row: before they did, all three reported ok against this arm.
 *
 * ★ THE COLOUR CHANNEL STAYS GREEN UNDER THIS ARM AND THAT IS CORRECT, not a
 * weak assertion. The suite pins the break to the alpha field (a=128 where
 * `Sa + Da*(1 - Sa)` gives 255) precisely because the colour field cannot see
 * it. An arm whose failure could come from either field would not have told us
 * which check was doing the work. */
static int g_alpha_ignoring;

/* ---- COLOUR ARM SWITCH 3 of 5: DOUBLE-PREMULTIPLY -------------------------
 * Set by the colour suite's arm 3. Models a GPU that applies the source alpha
 * to the colour channels TWICE before the measured operator's `S + D*(1 - Sa)`
 * sees them: 255*a*a = 64.3. That is color/premultiplied-srcover's NAMED
 * failure mode, and this is the only thing in the tree that can make it happen.
 *
 * ★ TWICE, NOT ONCE, AND THE DISTINCTION IS THE MEASUREMENT. While the model
 * implemented reading A the base formula supplied one `*Sa` of its own, so a
 * single extra multiply landed on 64. Against the measured reading B the base
 * formula supplies NONE, so ONE extra multiply is reading A (128 -- which is
 * arm 6's whole subject) and TWO is the defect. The observable is unchanged
 * (v=64 over black, v=96 over grey 0x40, v1=96/v2=112/pred=176 in case 4, all
 * pinned in the suite); what changed is that the two defects are now properly
 * separated instead of sharing a switch.
 *
 * The ALPHA byte is untouched (i != 3): doubling it would be a different
 * defect, and the point of this arm is that case 2 goes broken on `v=` while
 * its alpha row stays conforming -- the exact complement of arm 5. */
static int g_double_premul;

/* ---- COLOUR ARM SWITCH 4 of 5: READING A ----------------------------------
 * Set by the colour suite's arm 6. Models a GPU implementing the OTHER
 * admissible reading of SRC_OVER -- `S*Sa + D*(1 - Sa)`, one premultiply the
 * measured hardware does not do.
 *
 * ★★ THIS ARM IS THE PIN'S PROOF, AND IT IS THE REASON THE MODEL COULD NOT
 * SIMPLY BE LEFT AT READING A. Until 2026-09-02 cases 2, 3 and 5 returned ok
 * under EITHER reading and merely reported which in detail=; since
 * tools/vglite-conformance-check.sh compares only `<id> <pixel> <repeat>`, an
 * SDK re-vendor that flipped the operator would have left every verdict `ok`
 * and the drift checker GREEN -- the "quirk that silently disappears" that
 * file's header exists to catch. The cases now pin reading B. A pin nobody has
 * demonstrated RED is decoration, so this arm exists to demonstrate it: under
 * it cases 2 and 3 must go BROKEN with `model=A` on the line.
 *
 * ★ IT IS A MODEL OF A DIFFERENT GPU, NOT A MODEL OF A BUG. Reading A is what
 * inc/vg_lite.h:452's own non-premultiplied convention makes SRC_OVER mean, and
 * it is what this file implemented before the boot. That is precisely what
 * makes it the right shape for a drift arm: a future driver could plausibly
 * BE this, and the matrix must say so out loud rather than stay green.
 *
 * Colour channels only (i != 3): SRC_OVER's alpha row is `Sa + Da*(1 - Sa)`
 * (:462) under BOTH readings, so an arm that moved it would be testing a
 * different claim and would break case 4 for the wrong reason. */
static int g_reading_a;

/* ---- COLOUR ARM SWITCH 5 of 5: BLEND_NONE MODULATES -----------------------
 * Set by the colour suite's arm 7. Models a GPU whose rasteriser ALWAYS
 * modulates the source by its own alpha and whose BLEND_NONE therefore drops
 * only the DESTINATION term: `dst := S*Sa`, 255*a = 128 here.
 *
 * ★ IT IS TO CASE 5 WHAT ARM 6 IS TO CASES 2 AND 3 -- the other admissible
 * reading, made to happen. blend/none-honours-alpha admitted `raw` (255) and
 * `modulated` (128) until the same 2026-09-02 boot pinned it to `raw`, and
 * this is the only thing in the tree that can produce the other one. Without
 * it C5_MEASURED is a constant no test has ever seen fire.
 *
 * ★ IT BREAKS CASE 5 ALONE, and that sharpness is the point -- the same
 * property arm 4 has for case 1. The other BLEND_NONE case,
 * color/solid-word-order, draws an OPAQUE colour (a = 255), and modulating by
 * 255 is the identity, so it is untouched. The three SRC_OVER cases never
 * reach this branch.
 *
 * The ALPHA byte is left as `Sa` (i != 3): BLEND_NONE's alpha row IS `A: Sa`
 * (:459-460), and this arm models a defect in the COLOUR row only. Case 5
 * records alpha and judges only v=, so the pin reads `v=128,a=128,
 * read=modulated`. */
static int g_none_modulates;

/* Round-to-nearest x*a/255, the one scaling this file does. Named because the
 * blend below applies it up to three times per channel and a second spelling
 * of it is a second chance to get the rounding different. */
static int mul255(int x, int a) { return (x * a + 127) / 255; }

static uint32_t model_blend(uint32_t src, uint32_t dst, vg_lite_blend_t mode)
{
    const int a = (int)((src >> 24) & 0xFFu);

    if (mode == VG_LITE_BLEND_NONE) {
        if (!g_none_modulates) return src;      /* ":459  RGB: S, No blend" */
        uint32_t out = src & 0xFF000000u;       /* ":460  A: Sa", untouched */
        for (int i = 0; i < 3; i++)
            out |= ((uint32_t)mul255((int)((src >> (i * 8)) & 0xFFu), a)) << (i * 8);
        return out;
    }
    if (g_alpha_ignoring) return src;

    /* SRC_OVER, reading B -- `RGB: S + D*(1 - Sa)`, `A: Sa + Da*(1 - Sa)`. */
    uint32_t out = 0;
    for (int i = 0; i < 4; i++) {
        const int s = (int)((src >> (i * 8)) & 0xFFu);
        const int d = (int)((dst >> (i * 8)) & 0xFFu);
        int sp = s;
        if (i != 3) {
            /* Mutually exclusive by construction: the suite sets at most one
             * arm switch at a time, and `else if` says so rather than leaving
             * a silent triple-premultiply reachable from a future edit. */
            if (g_double_premul)  sp = mul255(mul255(sp, a), a);
            else if (g_reading_a) sp = mul255(sp, a);
        }
        int v = (i == 3) ? (a + mul255(d, 255 - a))
                         : (sp + mul255(d, 255 - a));
        /* ★ THE CLAMP IS LOAD-BEARING ON THE COLOUR ROW AND INERT ON ALPHA.
         * Reading B has no `*Sa` on the source, so a saturated source over a
         * non-black backdrop OVERFLOWS -- 255 + 64*(1 - a) = 286.9 in case 3,
         * which is exactly the value that case expects clamped. The alpha row
         * cannot exceed 255 -- exhaustively checked over all 65536 (a, d)
         * pairs, max 255 -- so the clamp there is defensive only. */
        if (v > 255) v = 255;
        out |= ((uint32_t)v) << (i * 8);
    }
    return out;
}

void vgc_draw_path_blend(vg_lite_path_t *p, vg_lite_fill_t rule, uint32_t color,
                         vg_lite_blend_t blend, vg_lite_error_t *acc)
{
    (void)acc;
    /* vg_lite_color_t in, memory word out -- see mem_word. Converted ONCE, at
     * the top, so every store below (shape and stray block alike) is in the
     * same order the destination already is: blending an ABGR source against
     * an ARGB destination would pair red with blue and produce a colour
     * neither the hardware nor the case ever asked for.
     *
     * ★ CONVERTING BEFORE THE BLEND RATHER THAN AFTER IS SAFE, AND IT WAS
     * MEASURED RATHER THAN ARGUED. model_blend is per-channel and takes its `a`
     * from BYTE 3, which mem_word leaves alone, so the swizzle COMMUTES with
     * the blend: mem_word(model_blend(s, d, m)) == model_blend(mem_word(s),
     * mem_word(d), m).
     * ★ RE-MEASURED 2026-09-02 AGAINST THE READING-B BLEND AND ITS TWO NEW ARM
     * SWITCHES, not carried over: 4800000 random (s, d) pairs -- both modes x
     * all six arm configurations (none, and each of the five switches alone) --
     * 0 mismatches. The earlier 800000-pair figure was taken against the
     * reading-A formula and would have been a stale number for a function that
     * had changed. That is why the conversion belongs at the edge where the
     * color argument enters and not inside the arithmetic. */
    const uint32_t src = mem_word(color);
    parse_path(p);
    if (g_one_contour_only && g_ncon > 1) g_ncon = 1;
    if (g_draw_nothing) g_ncon = 0;
    int painted = 0;

    /* ★ THE BOUNDING BOX IS ENFORCED, because on hardware it IS enforced and
     * getting it wrong is silent. The driver derives its tessellation window
     * from path->bounding_box; a box that under-covers the geometry clips the
     * render while every vg_lite_* call returns SUCCESS -- the exact failure
     * class this whole example exists to catch. Every box in the file under
     * test is correct today, but nothing CHECKED that, and Phase 2 and 3
     * authors will write new ones. Honouring it here turns a bbox typo into a
     * host-visible failure instead of a bench cycle. Four lines. */
    const float bx0 = p->bounding_box[0], by0 = p->bounding_box[1];
    const float bx1 = p->bounding_box[2], by1 = p->bounding_box[3];

    for (int y = 0; y < VGC_H; y++) {
        const float sy = (float)y + 0.5f;
        if (sy < by0 || sy > by1) continue;
        for (int x = 0; x < VGC_W; x++) {
            const float sx = (float)x + 0.5f;
            if (sx < bx0 || sx > bx1) continue;
            int wind = 0, cross = 0;
            for (int c = 0; c < g_ncon; c++) {
                const int s = g_cstart[c], len = g_clen[c];
                for (int i = 0; i < len; i++) {
                    const float ax = g_ptx[s + i], ay = g_pty[s + i];
                    const float bx = g_ptx[s + (i + 1) % len];
                    const float by = g_pty[s + (i + 1) % len];
                    if ((ay <= sy) == (by <= sy)) continue;   /* no crossing */
                    const float t = (sy - ay) / (by - ay);
                    if (ax + t * (bx - ax) <= sx) continue;   /* ray runs to +x */
                    cross++;
                    wind += (by > ay) ? 1 : -1;
                }
            }
            const int in = (rule == VG_LITE_FILL_EVEN_ODD) ? (cross & 1) : (wind != 0);
            if (in) {
                const size_t o = (size_t)y * VGC_W + (size_t)x;
                s_fb[o] = model_blend(src, s_fb[o], blend);
                painted++;
            }
        }
    }

    /* ★ THE STRAY BLOCK IS PAINTED ONLY IF THE DRAW ACTUALLY INKED SOMETHING,
     * which is what keeps path/degenerate-zero-area out of arm 4. That case
     * rasterises to nothing BY DESIGN and its check accepts it; giving it
     * invented geometry would make it go broken for a STRUCTURAL reason
     * (ymin/ymax outside row 64 +/-1) and blur the one thing arm 4 is trying
     * to isolate. Every other case inks something, so every other case gets
     * the block.
     *
     * It is painted OUTSIDE the bounding-box loop on purpose: stray geometry
     * that stayed inside the path's own box would be a weaker model of the
     * real defect, which produced 1171 px of excess on a 5760 px shape.
     *
     * ★ AND NOT FOR A BACKGROUND-COLOURED DRAW, which is a fix for a bug in
     * this model rather than a concession. path/two-draws-ring composes its
     * hole by drawing an inset plate in VGC_BG_COLOR over the plate, so the
     * unconditional version painted a WHITE block on draw 1 and then a BLACK
     * one over the same pixels on draw 2 -- the stray ink erased itself, the
     * case reported cover=ok, and arm 4 failed it for a reason that had
     * nothing to do with the case. Measured, before this line existed:
     *   path/two-draws-ring  ok  rim=1,centre=0,...,fill=5376,cover=ok
     * "Stray INK" means ink; a defect that spuriously erased would be a
     * different model, and short: is the field that would report it.
     *
     * The block goes down through the SAME blend the shape did: under
     * BLEND_NONE that is the plain store this has always been, and under
     * SRC_OVER a raw store would make the stray ink brighter than any pixel
     * the modelled GPU could actually produce. */
    if (g_stray_ink && painted && src != mem_word(VGC_BG_COLOR)) {
        for (int y = STRAY_Y0; y < STRAY_Y1; y++)
            for (int x = STRAY_X0; x < STRAY_X1; x++) {
                const size_t o = (size_t)y * VGC_W + (size_t)x;
                s_fb[o] = model_blend(src, s_fb[o], blend);
            }
    }
}

/* Signature UNCHANGED, and it delegates for the same reason the target's does:
 * the status-accumulation contract the case tables are specified in terms of
 * has ONE implementation, and so does the blend. */
void vgc_draw_path(vg_lite_path_t *p, vg_lite_fill_t rule, uint32_t color,
                   vg_lite_error_t *acc)
{
    vgc_draw_path_blend(p, rule, color, VG_LITE_BLEND_NONE, acc);
}

#endif /* VGC_TEST_MODEL_H */
