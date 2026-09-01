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
 * two orders apart and all 199 of their checks are unmoved by this function
 * existing. If one of them ever moves, the swizzle is in the wrong place. */
static uint32_t mem_word(uint32_t abgr)
{
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
 * SRC_OVER as `src*a + dst*(1 - a)` on the three colour bytes, with `a` the
 * source colour's alpha byte scaled 1/255, and `Sa + Da*(1 - Sa)` on the alpha
 * byte.
 *
 * ★ THIS IS ONE OF TWO INDEPENDENT DERIVATIONS, DELIBERATELY. If the model
 * implemented the same formula the case expects and nothing else, arm 1 would
 * prove only that the predicate reads what the model wrote -- circular. Every
 * expected value in vgc_cases_color.cpp is ALSO derived by hand in
 * expected_silicon.txt, and the two must agree. That is the discipline that
 * validated the pentagram in Phase 1b (analytic 2792.30 vs model 2792).
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
 * ★ THIS MODEL IMPLEMENTS READING A, AND THAT IS A CHOICE, NOT A FINDING. A
 * rasteriser that produced 255 could not tell reading B from a hardware defect,
 * and 128 is the value the rest of Phase 2 is derived against. THE HARDWARE MAY
 * DO READING B. Nothing here knows, and nothing here may be quoted as if it
 * did.
 *
 * ★ SO THE CASES MUST NOT INHERIT THIS CHOICE, and they do not.
 * color/premultiplied-srcover and blend/srcover-arithmetic ADMIT BOTH readings
 * and report which they saw (`model=A` / `model=B` in detail=); only a third
 * value is broken -- ~64 in case 2 being the double-premultiply defect.
 * blend/srcover-double sidesteps the question altogether by predicting its
 * second composite from its MEASURED first. A case that accepted only this
 * model's answer would be the instrument presupposing its own result, and a
 * green arm 1 would be evidence of nothing but its own construction.
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
static uint32_t model_blend(uint32_t src, uint32_t dst, vg_lite_blend_t mode)
{
    if (mode == VG_LITE_BLEND_NONE) return src;
    /* SRC_OVER */
    const int a = (int)((src >> 24) & 0xFFu);
    uint32_t out = 0;
    for (int i = 0; i < 4; i++) {
        const int s = (int)((src >> (i * 8)) & 0xFFu);
        const int d = (int)((dst >> (i * 8)) & 0xFFu);
        const int v = (i == 3) ? (a + (d * (255 - a) + 127) / 255)
                               : ((s * a + d * (255 - a) + 127) / 255);
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
     * mem_word(d), m). Checked over 800000 random (s, d) pairs across both
     * modes -- 0 mismatches. That is why it belongs at the edge where the
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
