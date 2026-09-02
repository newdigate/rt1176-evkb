/* vgc_cases_grad.cpp - linear gradient cases (NEW-32 gradients spec section 3).
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Spec: docs/superpowers/specs/2026-09-02-gc355-conformance-gradients-design.md
 *
 * ★★ WHY THIS FILE EXISTS. The quirks doc carried THREE gradient claims and not
 * one of them had a case: two were read from NXP's source and one was a single
 * uncontrolled sighting during the fader work. The guard layer refused to build
 * gradient helpers on that basis, and was right to. These six cases are the
 * measurements.
 *
 * ★★ THE CENTRAL FACT, READ FROM THE DRIVER RATHER THAN FROM THE CLAIM TABLE
 * (vg_lite_path.c, vg_lite_draw_linear_grad; vg_lite.c:7690-7710,
 * vg_lite_update_linear_grad):
 *
 *   update_linear_grad transforms the gradient line (X0,Y0)-(X1,Y1) by
 *   grad->matrix, takes its SCREEN-SPACE length, then OVERWRITES grad->matrix
 *   with translate(x0,y0).rotate.scale(len/width) and grad->linear_grad with
 *   (0,0,width,0), and allocates a width x 1 ABGR8888 ramp WITHOUT freeing the
 *   previous one. draw_linear_grad then derives the per-pixel parameter
 *   (lg_step_x/y_lin, lg_constant_lin) from grad->matrix ALONE, and applies
 *   path_matrix ONLY to the geometry. The two matrices are never composed.
 *
 * So an EXT gradient lives in SCREEN space. Move the path by its matrix and the
 * gradient stays where it was. That is the precise, pixel-decidable form of
 * "the ramp is placement-dependent", and cases 3-5 ask it three ways.
 *
 * ★ ONE PREDICTION CHANGED BETWEEN THE VERBAL DESIGN AND THIS FILE, and it is
 * recorded here because it was changed BEFORE the boot. The design called the
 * re-update cell "broken (double transform)". The algebra says otherwise: a
 * second update reads the matrix the first one WROTE and the line (0,0,w,0),
 * transforms them, and gets the SAME screen endpoints back -- it is
 * IDEMPOTENT, and leaks one ramp. It is still predicted broken, but because the
 * move never reaches the gradient, not because the transform doubles.
 *
 * ★ THE PREDICATE IS STRUCTURAL AND GENEROUS ON PURPOSE (the Phase 2 policy:
 * narrow after measuring, never before). A red-to-blue ramp across a rect must
 * read ~red at its left, ~blue at its right, a mix in the middle, and be
 * MONOTONIC along x. That survives antialiasing, /255-vs-/256, and either
 * premultiply reading (stops are opaque), and it is exactly what a GPU that
 * draws black, draws solid, or draws the ramp in the wrong place fails BY NAME.
 *
 * ★ COVERAGE IS n/a THROUGHOUT, as in the colour cases: the samples sit 4 px
 * inside the rect on its middle row, where coverage is exactly 1.0. The field
 * is still printed. */
#include "vgc_harness.h"
#include "vgc_color.h"
#include <stdio.h>
#include <string.h>

/* The colour cases' rect, for the same reason they share
 * path/single-contour-rect's: if that baseline is broken nothing here means
 * anything, and sharing the geometry makes the dependency visible. */
#define G_X 24
#define G_Y 24
#define G_W 80
#define G_H 80

/* Sample row, and the three columns RELATIVE to a rect's left edge: 4 px in
 * from each side (edge interpolation and PAD cannot reach them) and the
 * middle. The moved cases sample their MOVED rect, so the columns are offsets
 * and each case supplies its own left edge. */
#define G_SY     64
#define G_DX_L   4
#define G_DX_M   40
#define G_DX_R   76

/* The shift the moved cases apply through path_matrix. 16 px: a fifth of the
 * ramp, so a gradient that stayed put reads ~20% into the ramp at the moved
 * rect's left edge -- far outside the 'left is red' band -- while a gradient
 * that FOLLOWED the path reads the same values the static case does. Large
 * enough to be unmistakable, small enough that the moved rect (40..120) is
 * still inside the 128-px scratch with margin. */
#define G_SHIFT  16

/* Thresholds. 'Red' is R >= 200 with B <= 55 and 'blue' the mirror; 'a mix'
 * is both channels in [64,192]. A correct ramp at the sample columns is
 * predicted ~241/14 at the ends and ~126/129 in the middle, so the bands have
 * ~40 units of margin each way. */
#define G_HI   200
#define G_LO    55
#define G_MIX0  64
#define G_MIX1 192

/* ---- the shared structural predicate ---------------------------------------
 * Reads three pixels on row G_SY of the rect whose left edge is `x0`, writes
 * the profile into `d`, and returns the four sub-verdicts as bits so a case
 * can say WHICH one failed rather than only that one did. */
typedef struct {
    int rl, gl, bl, rm, gm, bm, rr, gr, br;
    int left_red, right_blue, mid_mix, monotonic;
} grad_profile_t;

static void grad_profile(int x0, grad_profile_t *p)
{
    const uint32_t pl = vgc_px(x0 + G_DX_L, G_SY);
    const uint32_t pm = vgc_px(x0 + G_DX_M, G_SY);
    const uint32_t pr = vgc_px(x0 + G_DX_R, G_SY);
    p->rl = vgc_ch(pl, VGC_R); p->gl = vgc_ch(pl, VGC_G); p->bl = vgc_ch(pl, VGC_B);
    p->rm = vgc_ch(pm, VGC_R); p->gm = vgc_ch(pm, VGC_G); p->bm = vgc_ch(pm, VGC_B);
    p->rr = vgc_ch(pr, VGC_R); p->gr = vgc_ch(pr, VGC_G); p->br = vgc_ch(pr, VGC_B);
    p->left_red   = (p->rl >= G_HI && p->bl <= G_LO);
    p->right_blue = (p->br >= G_HI && p->rr <= G_LO);
    p->mid_mix    = (p->rm >= G_MIX0 && p->rm <= G_MIX1 &&
                     p->bm >= G_MIX0 && p->bm <= G_MIX1);
    p->monotonic  = (p->rl > p->rm && p->rm > p->rr &&
                     p->bl < p->bm && p->bm < p->br);
}

static int grad_ok(const grad_profile_t *p)
{
    return p->left_red && p->right_blue && p->mid_mix && p->monotonic;
}

/* "l=R.G.B,m=R.G.B,r=R.G.B,mono=N" -- dots inside a triple, commas between
 * fields, so tools/vglite-conformance-check.sh's comma split still lands on
 * field boundaries. ~38 bytes. */
static int profile_str(const grad_profile_t *p, char *d, size_t n)
{
    return snprintf(d, n, "l=%d.%d.%d,m=%d.%d.%d,r=%d.%d.%d,mono=%d",
                    p->rl, p->gl, p->bl, p->rm, p->gm, p->bm,
                    p->rr, p->gr, p->br, p->monotonic);
}

/* ---- shared geometry ---------------------------------------------------- */

static vg_lite_error_t rect_path(vg_lite_path_t *p)
{
    vgc_emit_rect_cw(G_X, G_Y, G_W, G_H);
    return vgc_finish_path(p, G_X, G_Y, G_X + G_W, G_Y + G_H);
}

/* ★ STATIC, NOT ON THE STACK. vg_lite_ext_linear_gradient_t carries
 * color_ramp[256] and converted_ramp[258] of five floats each -- ~10 KB -- and
 * this runs inside setup(). One object per API, reused by every case: the
 * harness runs cases one at a time, and each EXT case clears it before it
 * returns (see the ★ at ext_teardown). */
static vg_lite_ext_linear_gradient_t s_ext;
static vg_lite_linear_gradient_t     s_legacy;

/* The red-to-blue ramp. Named float channels: the EXT API has NO input word
 * order to get wrong, which is why grad/ramp-word-order can only be about the
 * driver's PACKED ramp image and the hardware's reading of it. */
static vg_lite_color_ramp_t s_red_blue[2] = {
    { 0.0f, 1.0f, 0.0f, 0.0f, 1.0f },
    { 1.0f, 0.0f, 0.0f, 1.0f, 1.0f },
};
static vg_lite_color_ramp_t s_red_red[2] = {
    { 0.0f, 1.0f, 0.0f, 0.0f, 1.0f },
    { 1.0f, 1.0f, 0.0f, 0.0f, 1.0f },
};

/* Build the EXT gradient with its line from x0 to x0+G_W on the sample row,
 * identity gradient matrix, and update it. The line is SCREEN coordinates --
 * that is the whole finding, and it is why the moved cases must pass a
 * different x0 to rebuild correctly. */
static vg_lite_error_t ext_build(vg_lite_color_ramp_t *ramp, float x0)
{
    vg_lite_linear_gradient_parameter_t line;
    line.X0 = x0;         line.Y0 = (float)G_SY;
    line.X1 = x0 + G_W;   line.Y1 = (float)G_SY;
    memset(&s_ext, 0, sizeof(s_ext));
    vg_lite_error_t e = vg_lite_set_linear_grad(&s_ext, 2, ramp, line,
                                                VG_LITE_GRADIENT_SPREAD_PAD, 0);
    if (e != VG_LITE_SUCCESS) return e;
    vg_lite_identity(vg_lite_get_linear_grad_matrix(&s_ext));
    return vg_lite_update_linear_grad(&s_ext);
}

/* ★ EVERY EXT CASE CLEARS THE OBJECT BEFORE RETURNING. update_linear_grad
 * allocates a ramp image each call and frees NOTHING (vg_lite.c:7690 --
 * memset(&grad->image, 0, ...) then vg_lite_allocate); clear_linear_grad frees
 * the CURRENT image. So a case that returned without clearing would leave a
 * live ramp in the pool for the repeat run, and the reupdate case would leak
 * two per run instead of the one it is measuring. The status is folded into
 * *acc like any other. */
static void ext_teardown(vg_lite_error_t *acc)
{
    const vg_lite_error_t e = vg_lite_clear_linear_grad(&s_ext);
    if (e != VG_LITE_SUCCESS && *acc == VG_LITE_SUCCESS) *acc = e;
}

/* Draw the standard rect through the EXT gradient under `pm`, then finish. */
static vg_lite_error_t ext_draw(vg_lite_matrix_t *pm)
{
    vg_lite_error_t acc = VG_LITE_SUCCESS;
    vg_lite_path_t p;
    const vg_lite_error_t fe = rect_path(&p);
    if (fe != VG_LITE_SUCCESS) return fe;
    vgc_draw_linear_grad(&p, &s_ext, pm, &acc);
    vgc_finish_into(&acc);
    return acc;
}

static vg_lite_matrix_t *shifted(void)
{
    static vg_lite_matrix_t m;
    vg_lite_identity(&m);
    vg_lite_translate((float)G_SHIFT, 0.0f, &m);
    return &m;
}

/* ---- 1. grad/legacy-linear ------------------------------------------------
 * The pre-EXT API, vg_lite_init_grad / set_grad / update_grad / draw_grad.
 * draw_grad is one line -- vg_lite_draw_pattern over the 1024x1 ramp image
 * with grad->matrix as the pattern matrix (vg_lite_path.c:5739) -- and nothing
 * in it is GC255-specific. The "GC255-only" claim rests on NXP's vglite_layer.c
 * gating it by chip id, and on ONE sighting of solid black.
 *
 * ★ THE CALLER OWNS THE MATRIX, AND THAT IS THE ALTERNATIVE EXPLANATION FOR
 * THE BLACK. The ramp is 1024 px wide; a caller leaving grad->matrix at
 * identity maps 1024 ramp px across a 64 px rect and samples ~6% of it. This
 * case sets it correctly -- translate(G_X,G_Y) then scale(G_W/1024), which
 * with the driver's POST-multiplying helpers (vg_lite.c, multiply(matrix, &t))
 * gives screen = T(S(ramp)) -- so a black here is the hardware, not the
 * matrix.
 *
 * ★ THE COLOURS ARE THE DRIVER'S OWN 0xAARRGGBB, NOT vg_lite_color_t. set_grad
 * stores the words verbatim and update_grad unpacks them with A()/R()/G()/B()
 * (vg_lite_context.h:95-99: A is bits 31:24, R is 23:16) -- ARGB, the OPPOSITE
 * of the ABGR every other colour in this tree is packed in. A second word-order
 * trap, in the one API where the caller packs the ramp.
 *
 * ★ THE count=0 SUB-EXPERIMENT rides in detail= rather than in a case of its
 * own. set_grad(count=0) returns SUCCESS with count 0 (vg_lite.c:8131-8133) and
 * update_grad then substitutes black@0 -> white@255 and sets count=2
 * (vg_lite.c:8167ff). Both are pure driver software and both are asserted from
 * the object's own fields, with no second draw: c0= the return code, c0n= the
 * count after update, c0k=/c0w= the two substituted colours. Predicted
 * 0 / 2 / FF000000 / FFFFFFFF. */

static uint32_t s_c0_rc, s_c0_n, s_c0_k, s_c0_w;

static vgc_verdict_t check_legacy(char *d, size_t n)
{
    grad_profile_t p;
    grad_profile(G_X, &p);
    const vgc_cover_t cv = vgc_cover_na();
    int off = profile_str(&p, d, n);
    if (off < 0) off = 0;
    snprintf(d + off, n - (size_t)off, ",c0=%lu,c0n=%lu,c0k=%08lX,c0w=%08lX,%s",
             (unsigned long)s_c0_rc, (unsigned long)s_c0_n,
             (unsigned long)s_c0_k, (unsigned long)s_c0_w, cv.s);
    return grad_ok(&p) ? VGC_OK : VGC_BROKEN;
}

static vg_lite_error_t run_legacy(void)
{
    vg_lite_error_t acc = VG_LITE_SUCCESS;
    /* vg_lite_uint32_t, not uint32_t: on this ARM toolchain uint32_t is
     * `long unsigned int` and the driver's typedef is `unsigned int` -- same
     * width, different type, and g++ only lets the pointer conversion through
     * under -fpermissive. The host stub declares the parameters as uint32_t,
     * so it never saw the mismatch; the target did. */
    vg_lite_uint32_t colors[2] = { 0xFFFF0000u, 0xFF0000FFu };   /* driver-ARGB: red, blue */
    vg_lite_uint32_t stops[2]  = { 0u, VLC_GRADIENT_BUFFER_WIDTH - 1u };

    memset(&s_legacy, 0, sizeof(s_legacy));
    vg_lite_error_t e = vg_lite_init_grad(&s_legacy);
    if (e != VG_LITE_SUCCESS) return e;
    e = vg_lite_set_grad(&s_legacy, 2, colors, stops);
    if (e != VG_LITE_SUCCESS && acc == VG_LITE_SUCCESS) acc = e;
    vg_lite_matrix_t *gm = vg_lite_get_grad_matrix(&s_legacy);
    vg_lite_identity(gm);
    vg_lite_translate((float)G_X, (float)G_Y, gm);
    vg_lite_scale((float)G_W / (float)VLC_GRADIENT_BUFFER_WIDTH, 1.0f, gm);
    e = vg_lite_update_grad(&s_legacy);
    if (e != VG_LITE_SUCCESS && acc == VG_LITE_SUCCESS) acc = e;

    vg_lite_path_t p;
    const vg_lite_error_t fe = rect_path(&p);
    if (fe != VG_LITE_SUCCESS) { vg_lite_clear_grad(&s_legacy); return fe; }
    vgc_draw_grad(&p, &s_legacy, vgc_ident(), &acc);
    vgc_finish_into(&acc);
    e = vg_lite_clear_grad(&s_legacy);
    if (e != VG_LITE_SUCCESS && acc == VG_LITE_SUCCESS) acc = e;

    /* The count=0 sub-experiment, on a freshly initialised object, no draw.
     * Its allocation is released by the clear_grad at the end. */
    memset(&s_legacy, 0, sizeof(s_legacy));
    e = vg_lite_init_grad(&s_legacy);
    if (e != VG_LITE_SUCCESS && acc == VG_LITE_SUCCESS) acc = e;
    s_c0_rc = (uint32_t)vg_lite_set_grad(&s_legacy, 0, NULL, NULL);
    e = vg_lite_update_grad(&s_legacy);
    if (e != VG_LITE_SUCCESS && acc == VG_LITE_SUCCESS) acc = e;
    s_c0_n = s_legacy.count;
    s_c0_k = s_legacy.colors[0];
    s_c0_w = s_legacy.colors[1];
    e = vg_lite_clear_grad(&s_legacy);
    if (e != VG_LITE_SUCCESS && acc == VG_LITE_SUCCESS) acc = e;
    return acc;
}

/* ---- 2. grad/ext-linear-static -----------------------------------------------
 * THE BOOTSTRAP CONTROL for the three cells below. EXT gradient, line from the
 * rect's left edge to its right, identity path matrix. If this is broken, the
 * moved/reupdate/rebuilt verdicts say nothing about placement -- they say the
 * EXT API does not render, which is a different (and larger) finding. */

static vgc_verdict_t check_ext_static(char *d, size_t n)
{
    grad_profile_t p;
    grad_profile(G_X, &p);
    const vgc_cover_t cv = vgc_cover_na();
    int off = profile_str(&p, d, n);
    if (off < 0) off = 0;
    snprintf(d + off, n - (size_t)off, ",%s", cv.s);
    return grad_ok(&p) ? VGC_OK : VGC_BROKEN;
}

static vg_lite_error_t run_ext_static(void)
{
    vg_lite_error_t acc = ext_build(s_red_blue, (float)G_X);
    if (acc != VG_LITE_SUCCESS) return acc;
    acc = ext_draw(vgc_ident());
    ext_teardown(&acc);
    return acc;
}

/* ---- 3. grad/ext-linear-moved ----------------------------------------------
 * The static build, drawn with path_matrix = translate(G_SHIFT, 0) and NO
 * update. The rect is now at 40..120; the ramp (screen space, see the file
 * header) is still at 24..104. Sampled at the MOVED rect's columns.
 *
 * Predicted BROKEN: the left sample (x=44) sits ~20% into a ramp that did not
 * move, so it reads ~R=190 -- below the red band -- and the right sample
 * (x=116) is past X1 and reads PAD blue, which is fine on its own. `left_red`
 * is the sub-verdict that carries it. A GPU that composed path_matrix into the
 * paint would make this cell ok -- and that would be the bigger finding, since
 * it is not what the driver's own code path does. */

static vgc_verdict_t check_ext_moved(char *d, size_t n)
{
    grad_profile_t p;
    grad_profile(G_X + G_SHIFT, &p);
    const vgc_cover_t cv = vgc_cover_na();
    int off = profile_str(&p, d, n);
    if (off < 0) off = 0;
    snprintf(d + off, n - (size_t)off, ",%s", cv.s);
    return grad_ok(&p) ? VGC_OK : VGC_BROKEN;
}

static vg_lite_error_t run_ext_moved(void)
{
    vg_lite_error_t acc = ext_build(s_red_blue, (float)G_X);
    if (acc != VG_LITE_SUCCESS) return acc;
    acc = ext_draw(shifted());
    ext_teardown(&acc);
    return acc;
}

/* ---- 4. grad/ext-linear-reupdate -------------------------------------------
 * The moved case, plus a SECOND vg_lite_update_linear_grad on the same object
 * before the draw -- the fix a reader of the API would reach for first.
 *
 * Predicted BROKEN with the SAME profile as the moved case, NOT a different
 * one. The second update transforms the line (0,0,width,0) the first update
 * stored by the matrix the first update wrote, and gets the SAME screen
 * endpoints back: translate(x0,y0).rotate.scale(len/width) applied to
 * (0,0)-(width,0) is exactly (x0,y0)-(x1,y1). It is idempotent. What it also
 * does is allocate a second ramp image without freeing the first, so this case
 * leaks exactly one by construction -- printed as leak=1, and counted on the
 * host where the pool is a malloc.
 *
 * If this cell comes back ok while moved is broken, a second update is NOT
 * idempotent on this hardware and does move the ramp -- worth knowing, and
 * recorded as a refuted prediction rather than pasted over. */

static vgc_verdict_t check_ext_reupdate(char *d, size_t n)
{
    grad_profile_t p;
    grad_profile(G_X + G_SHIFT, &p);
    const vgc_cover_t cv = vgc_cover_na();
    int off = profile_str(&p, d, n);
    if (off < 0) off = 0;
    snprintf(d + off, n - (size_t)off, ",leak=1,%s", cv.s);
    return grad_ok(&p) ? VGC_OK : VGC_BROKEN;
}

static vg_lite_error_t run_ext_reupdate(void)
{
    vg_lite_error_t acc = ext_build(s_red_blue, (float)G_X);
    if (acc != VG_LITE_SUCCESS) return acc;
    const vg_lite_error_t ue = vg_lite_update_linear_grad(&s_ext);
    if (ue != VG_LITE_SUCCESS) acc = ue;
    const vg_lite_error_t de = ext_draw(shifted());
    if (de != VG_LITE_SUCCESS && acc == VG_LITE_SUCCESS) acc = de;
    ext_teardown(&acc);
    return acc;
}

/* ---- 5. grad/ext-linear-rebuilt --------------------------------------------
 * The moved case done RIGHT: clear the object, set the line at the NEW screen
 * position (40..120), update, draw at translate(G_SHIFT, 0). This is the
 * path/two-draws-ring of the gradient matrix -- the prescribed usage measured
 * beside its counterexample -- and it is what a moving widget has to do:
 * re-specify the gradient line in screen space per placement. Predicted ok. */

static vgc_verdict_t check_ext_rebuilt(char *d, size_t n)
{
    return check_ext_moved(d, n);   /* same rect, same predicate, same columns */
}

static vg_lite_error_t run_ext_rebuilt(void)
{
    vg_lite_error_t acc = ext_build(s_red_blue, (float)G_X);
    if (acc != VG_LITE_SUCCESS) return acc;
    /* Discard the first build the way a widget would on move. */
    ext_teardown(&acc);
    const vg_lite_error_t be = ext_build(s_red_blue, (float)(G_X + G_SHIFT));
    if (be != VG_LITE_SUCCESS && acc == VG_LITE_SUCCESS) acc = be;
    const vg_lite_error_t de = ext_draw(shifted());
    if (de != VG_LITE_SUCCESS && acc == VG_LITE_SUCCESS) acc = de;
    ext_teardown(&acc);
    return acc;
}

/* ---- 6. grad/ramp-word-order ---------------------------------------------
 * EXT gradient with BOTH stops pure red, static placement. There is nothing to
 * interpolate, so every ramp entry is the same colour and every sample must
 * read EXACTLY R=255 G=0 B=0 A=255 -- no tolerance, because there is no
 * arithmetic to be tolerant of.
 *
 * ★ WHAT IT PROBES. update_linear_grad packs the ramp bytes in memory order
 * A, B, G, R (vg_lite.c: PackColorComponent of color[3], [2], [1], [0]) into
 * an image it tags VG_LITE_ABGR8888. The hardware's ABGR8888 sampler must read
 * them back in that order; if it reads A,R,G,B the ramp comes out blue. The
 * EXT INPUT has no word order (named floats), so this is the only place a
 * word-order fault can enter the EXT path, and this case is the only one that
 * can name it: the four red-to-blue cases would merely reverse. */

static vgc_verdict_t check_word_order(char *d, size_t n)
{
    grad_profile_t p;
    grad_profile(G_X, &p);
    const uint32_t pl = vgc_px(G_X + G_DX_L, G_SY);
    const int al = vgc_ch(pl, VGC_A);
    const vgc_cover_t cv = vgc_cover_na();
    int off = profile_str(&p, d, n);
    if (off < 0) off = 0;
    snprintf(d + off, n - (size_t)off, ",a=%d,%s", al, cv.s);
    const int red = (p.rl == 255 && p.gl == 0 && p.bl == 0 &&
                     p.rm == 255 && p.gm == 0 && p.bm == 0 &&
                     p.rr == 255 && p.gr == 0 && p.br == 0 && al == 255);
    return red ? VGC_OK : VGC_BROKEN;
}

static vg_lite_error_t run_word_order(void)
{
    vg_lite_error_t acc = ext_build(s_red_red, (float)G_X);
    if (acc != VG_LITE_SUCCESS) return acc;
    acc = ext_draw(vgc_ident());
    ext_teardown(&acc);
    return acc;
}

/* ---- the table -------------------------------------------------------------
 * ORDER IS PART OF THE INSTRUMENT. static is the control and sits before the
 * three cells that depend on it; moved / reupdate / rebuilt are one experiment
 * read as a row; legacy is first because it is the API the claim table leads
 * with, and word-order last because it is the one case that names a fault the
 * others can only reverse on. */
const vgc_case_t vgc_grad_cases[] = {
    { "grad/legacy-linear",       run_legacy,       check_legacy,       NULL },
    { "grad/ext-linear-static",   run_ext_static,   check_ext_static,   NULL },
    { "grad/ext-linear-moved",    run_ext_moved,    check_ext_moved,    NULL },
    { "grad/ext-linear-reupdate", run_ext_reupdate, check_ext_reupdate, NULL },
    { "grad/ext-linear-rebuilt",  run_ext_rebuilt,  check_ext_rebuilt,  NULL },
    { "grad/ramp-word-order",     run_word_order,   check_word_order,   NULL },
};
const size_t vgc_grad_case_count =
    sizeof(vgc_grad_cases) / sizeof(vgc_grad_cases[0]);
