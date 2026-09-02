/* vgc_cases_blit.cpp - images, blits & scissor (Phase 3 spec section 3).
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Spec: docs/superpowers/specs/2026-09-02-gc355-conformance-phase3-design.md
 *
 * ★★ TWO CLAIMS SHIPPING CODE HAS BEEN BUILT ON WITHOUT A CASE. The rotary
 * bench pads every rotor frame to a 64-byte stride because "the GC355 refuses
 * a blit source whose stride is not 64-B aligned", and the fader's header
 * warns that vg_lite_init() with the panel's own size defeats per-fader
 * scissoring. Both come from reading source. Reading it again sharpened both,
 * and these six cases measure them.
 *
 * ★★ THE SCISSOR IS TWO MECHANISMS, NOT ONE (vg_lite_image.c:263,
 * vg_lite.c:3626, vg_lite_path.c:1208-1260). vg_lite_set_scissor writes only
 * s_context.scissor[] -- no register. Then:
 *   - RIGHT and BOTTOM go to HARDWARE: set_render_target pushes 0x0A13 =
 *     MIN(right,width) | MIN(bottom,height)<<16, in every regime.
 *   - LEFT and TOP exist only as a clamp on the TESSELLATION WINDOW inside
 *     vg_lite_draw (point_min = MAX(point_min, scissor.x/y); the tile loop
 *     starts there) -- and that whole block is skipped when ts_is_fullscreen,
 *     i.e. when the tess buffer covers the target.
 * So in the fullscreen regime a scissored draw LOSES LEFT AND TOP AND KEEPS
 * RIGHT AND BOTTOM. scissor/tess-fullscreen asserts each edge separately so
 * it can tell that from a scissor that is simply dead.
 * (A driver oddity worth knowing while reading that code: the tess-window
 * clamp computes its right edge as scissor[0] + scissor[2] -- treating the
 * stored RIGHT as a WIDTH -- while 0x0A13 uses scissor[2] as right. The
 * window therefore over-covers on the right, harmlessly, and the hardware
 * register is what actually clips there.)
 *
 * ★★ THE STRIDE RULE IS A DRIVER CHECK. vg_lite_blit (vg_lite.c:4339) calls
 * srcbuf_align_check -- whose ADDRESS checks are compiled out on this chip
 * (gcFEATURE_VG_SRC_ADDRESS_64BYTES_ALIGNED 0, _DETAIL_ALIGNED 0) -- and then,
 * under gcFEATURE_VG_16PIXELS_ALIGNED 1, _check_source_aligned(format, stride)
 * (:1383): FORMAT_ALIGNMENT(stride, 64) for 32-bpp, 32 for 16-bpp, 16 for
 * 8-bpp, returning VG_LITE_INVALID_ARGUMENT (1) BEFORE any command is built.
 * So blit/stride-unaligned measures a REFUSAL, not a hardware behaviour, and
 * it is safe in the default build: nothing reaches the GPU.
 *
 * ★ EVERY BLIT SOURCE LIVES IN GPU-REACHABLE MEMORY. Unlike path data, which
 * the driver memcpys into the command buffer, an image is read by the GPU
 * where it sits. Bus masters cannot reach TCM, so the sources are EXTMEM,
 * 64-byte aligned (the harness's own rule for the scratch), and mapped ONCE
 * each with vg_lite_map(VG_LITE_MAP_USER_MEMORY) -- a static flag per buffer,
 * because run() is called twice per case and mapping twice is not something
 * the port promises to tolerate. On the host EXTMEM is empty and vg_lite_map
 * is a no-op.
 *
 * ★ COVERAGE IS n/a THROUGHOUT; the field is still printed. */
#include "vgc_harness.h"
#include "vgc_color.h"
#include <stdio.h>
#include <string.h>

#if defined(__arm__)
#include <Arduino.h>          /* EXTMEM */
#define VGC_GPU_BUF EXTMEM __attribute__((aligned(64)))
#else
#define VGC_GPU_BUF __attribute__((aligned(64)))
#endif

/* ---- the checkerboard ------------------------------------------------------
 * 16x16, 4x4 cells, red and blue. Cell (cx,cy) is red when (cx+cy) is even.
 * Placed at (24,24) by a translate-only matrix -- the plain blit path. */
#define B_W    16
#define B_H    16
#define B_CELL 4
#define B_X    24
#define B_Y    24

/* Sample points: the CENTRES of cells (0,0) (1,0) (0,1) (1,1), and one pixel
 * well outside the image (the harness clear must survive there). */
#define B_S00X (B_X + 2)
#define B_S10X (B_X + B_CELL + 2)
#define B_S0Y  (B_Y + 2)
#define B_S1Y  (B_Y + B_CELL + 2)
#define B_OUTX 64
#define B_OUTY 64

/* A cell is RED if R >= 248 and G,B <= 7; BLUE the mirror. The 8-unit
 * tolerance exists for the RGB565 case: a 5-bit 0x1F expands to 248 by shift
 * and 255 by replication, and both are correct expansions. */
#define B_SAT 248
#define B_ZERO 7

static int is_red(int r, int g, int b)  { return r >= B_SAT && g <= B_ZERO && b <= B_ZERO; }
static int is_blue(int r, int g, int b) { return b >= B_SAT && g <= B_ZERO && r <= B_ZERO; }
static int is_black(int r, int g, int b){ return r <= B_ZERO && g <= B_ZERO && b <= B_ZERO; }

typedef struct { int r, g, b; } rgb_t;
static rgb_t rgb_at(int x, int y)
{
    const uint32_t px = vgc_px(x, y);
    rgb_t c = { vgc_ch(px, VGC_R), vgc_ch(px, VGC_G), vgc_ch(px, VGC_B) };
    return c;
}

/* The five samples, formatted, plus the checker verdict. `order_agnostic`
 * accepts the checker with red and blue SWAPPED (for the 16-bit format case,
 * whose component order is what it measures) and reports which it saw. */
typedef struct { rgb_t c00, c10, c01, c11, out; int checker; const char *order; } blit_profile_t;

static void blit_profile(blit_profile_t *p, int order_agnostic)
{
    p->c00 = rgb_at(B_S00X, B_S0Y); p->c10 = rgb_at(B_S10X, B_S0Y);
    p->c01 = rgb_at(B_S00X, B_S1Y); p->c11 = rgb_at(B_S10X, B_S1Y);
    p->out = rgb_at(B_OUTX, B_OUTY);
    const int straight = is_red (p->c00.r, p->c00.g, p->c00.b) && is_blue(p->c10.r, p->c10.g, p->c10.b) &&
                         is_blue(p->c01.r, p->c01.g, p->c01.b) && is_red (p->c11.r, p->c11.g, p->c11.b);
    const int swapped  = is_blue(p->c00.r, p->c00.g, p->c00.b) && is_red (p->c10.r, p->c10.g, p->c10.b) &&
                         is_red (p->c01.r, p->c01.g, p->c01.b) && is_blue(p->c11.r, p->c11.g, p->c11.b);
    const int untouched = is_black(p->out.r, p->out.g, p->out.b);
    p->order = straight ? "low" : swapped ? "high" : "none";
    p->checker = untouched && (straight || (order_agnostic && swapped));
}

static int profile_str(const blit_profile_t *p, char *d, size_t n)
{
    return snprintf(d, n, "c00=%d.%d.%d,c10=%d.%d.%d,c01=%d.%d.%d,c11=%d.%d.%d,out=%d.%d.%d",
                    p->c00.r, p->c00.g, p->c00.b, p->c10.r, p->c10.g, p->c10.b,
                    p->c01.r, p->c01.g, p->c01.b, p->c11.r, p->c11.g, p->c11.b,
                    p->out.r, p->out.g, p->out.b);
}

/* ---- the source buffers ------------------------------------------------------
 * Three BGRA8888 buffers that differ ONLY in stride, and one RGB565. Each is
 * exactly as wide in memory as its stride says, so the model and the hardware
 * read the same bytes. The memory word for BGRA8888 is A<<24|R<<16|G<<8|B
 * (vgc_color.h -- measured by vglite_probe), so the CPU packs it that way. */
#define B_RED_W   0xFFFF0000u
#define B_BLUE_W  0xFF0000FFu
/* RGB565, RED IN THE LOW FIVE BITS -- the PRE-REGISTERED convention, by
 * analogy with the measured BGRA8888 word (first-named component lowest).
 * blit/formats reports order= and its expectation carries the prediction;
 * if the hardware reads it the other way the case says `high`. */
#define B_RED_565  0x001Fu
#define B_BLUE_565 0xF800u

VGC_GPU_BUF static uint32_t s_src64 [B_H * 16];        /* stride  64 B: 16 px */
VGC_GPU_BUF static uint32_t s_src128[B_H * 32];        /* stride 128 B: 32 px, 16 used */
VGC_GPU_BUF static uint32_t s_src80 [B_H * 20];        /* stride  80 B: 20 px, 16 used */
VGC_GPU_BUF static uint16_t s_src565[B_H * 16];        /* stride  32 B: 16 px x 2 B */

static vg_lite_buffer_t s_b64, s_b128, s_b80, s_b565;
static int s_mapped64, s_mapped128, s_mapped80, s_mapped565;

static void fill_checker32(uint32_t *buf, int pitch_px)
{
    for (int y = 0; y < B_H; y++)
        for (int x = 0; x < B_W; x++)
            buf[y * pitch_px + x] = (((x / B_CELL) + (y / B_CELL)) & 1) ? B_BLUE_W : B_RED_W;
}
static void fill_checker16(uint16_t *buf, int pitch_px)
{
    for (int y = 0; y < B_H; y++)
        for (int x = 0; x < B_W; x++)
            buf[y * pitch_px + x] = (uint16_t)((((x / B_CELL) + (y / B_CELL)) & 1) ? B_BLUE_565 : B_RED_565);
}

/* Describe a source once and map it ONCE (see the file header). Returns the
 * map status the first time and SUCCESS thereafter. */
static vg_lite_error_t source_ready(vg_lite_buffer_t *b, int *mapped, void *mem,
                                    int32_t stride, vg_lite_buffer_format_t fmt)
{
    if (*mapped) return VG_LITE_SUCCESS;
    memset(b, 0, sizeof(*b));
    b->width   = B_W;
    b->height  = B_H;
    b->stride  = stride;
    b->tiled   = VG_LITE_LINEAR;
    b->format  = fmt;
    b->memory  = mem;
    b->address = (uint32_t)(uintptr_t)mem;
    const vg_lite_error_t e = vg_lite_map(b, VG_LITE_MAP_USER_MEMORY, 0);
    if (e == VG_LITE_SUCCESS) *mapped = 1;
    return e;
}

static vg_lite_matrix_t *at_origin(void)
{
    static vg_lite_matrix_t m;
    vg_lite_identity(&m);
    vg_lite_translate((float)B_X, (float)B_Y, &m);
    return &m;
}

/* One blit of `b` at (B_X,B_Y), point filter, then finish. */
static vg_lite_error_t blit_once(vg_lite_buffer_t *b)
{
    vg_lite_error_t acc = VG_LITE_SUCCESS;
    vgc_blit(b, at_origin(), VG_LITE_FILTER_POINT, &acc);
    vgc_finish_into(&acc);
    return acc;
}

/* The three same-shape checks (basic, stride-64, formats) differ only in
 * order-agnosticism; one body. */
static vgc_verdict_t check_checker(char *d, size_t n, int order_agnostic)
{
    blit_profile_t p;
    blit_profile(&p, order_agnostic);
    const vgc_cover_t cv = vgc_cover_na();
    int off = profile_str(&p, d, n);
    if (off < 0) off = 0;
    if (order_agnostic)
        snprintf(d + off, n - (size_t)off, ",order=%s,%s", p.order, cv.s);
    else
        snprintf(d + off, n - (size_t)off, ",%s", cv.s);
    return p.checker ? VGC_OK : VGC_BROKEN;
}

/* ---- 1. blit/basic ------------------------------------------------------------
 * The natural layout: 16 px wide, stride 64 B. The pattern must land. */
static vgc_verdict_t check_basic(char *d, size_t n) { return check_checker(d, n, 0); }
static vg_lite_error_t run_basic(void)
{
    fill_checker32(s_src64, 16);
    const vg_lite_error_t e = source_ready(&s_b64, &s_mapped64, s_src64, 64, VG_LITE_BGRA8888);
    if (e != VG_LITE_SUCCESS) return e;
    return blit_once(&s_b64);
}

/* ---- 2. blit/stride-64 -----------------------------------------------------------
 * The same 16x16 image in a 32-px-wide buffer -- 64 B of data then 64 B of
 * padding per row, stride 128 B, width still 16. THE ROTARY BENCH'S LAYOUT
 * (rotor frames padded to a 64-B stride). A driver or GPU that walked rows by
 * width*bpp instead of by stride would shear every odd row and the checker
 * would not land. */
static vgc_verdict_t check_stride64(char *d, size_t n) { return check_checker(d, n, 0); }
static vg_lite_error_t run_stride64(void)
{
    memset(s_src128, 0, sizeof(s_src128));      /* the padding is visibly zero */
    fill_checker32(s_src128, 32);
    const vg_lite_error_t e = source_ready(&s_b128, &s_mapped128, s_src128, 128, VG_LITE_BGRA8888);
    if (e != VG_LITE_SUCCESS) return e;
    return blit_once(&s_b128);
}

/* ---- 3. blit/stride-unaligned ------------------------------------------------
 * A 20-px-wide buffer: stride 80 B, not a multiple of 64. The DEFINED OUTCOME
 * is a refusal -- api=error (VG_LITE_INVALID_ARGUMENT, from
 * _check_source_aligned) AND every sample untouched. That is what ok means
 * here, as it does for path/degenerate-zero-area: the point is that the
 * outcome is defined. api=success with anything painted, OR a refusal that
 * painted anyway, is broken. The api code is carried in detail= as well as in
 * the harness's api= column, because the check() has no other way to see it. */
static vg_lite_error_t s_unaligned_rc;
static vgc_verdict_t check_unaligned(char *d, size_t n)
{
    blit_profile_t p;
    blit_profile(&p, 0);
    const int all_black = is_black(p.c00.r, p.c00.g, p.c00.b) && is_black(p.c10.r, p.c10.g, p.c10.b) &&
                          is_black(p.c01.r, p.c01.g, p.c01.b) && is_black(p.c11.r, p.c11.g, p.c11.b) &&
                          is_black(p.out.r, p.out.g, p.out.b);
    const int refused = (s_unaligned_rc != VG_LITE_SUCCESS);
    const vgc_cover_t cv = vgc_cover_na();
    int off = profile_str(&p, d, n);
    if (off < 0) off = 0;
    snprintf(d + off, n - (size_t)off, ",rc=%d,refused=%d,untouched=%d,%s",
             (int)s_unaligned_rc, refused, all_black, cv.s);
    return (refused && all_black) ? VGC_OK : VGC_BROKEN;
}
static vg_lite_error_t run_unaligned(void)
{
    fill_checker32(s_src80, 20);
    const vg_lite_error_t e = source_ready(&s_b80, &s_mapped80, s_src80, 80, VG_LITE_BGRA8888);
    if (e != VG_LITE_SUCCESS) return e;
    /* ★ THE STATUS IS THE MEASUREMENT, so it is recorded here and the case
     * returns SUCCESS to the harness: api= would otherwise read error:1 for a
     * case whose predicted outcome IS error:1, and the summary would count a
     * correct refusal as a failed call. The code is printed in detail=. */
    vg_lite_error_t acc = VG_LITE_SUCCESS;
    vgc_blit(&s_b80, at_origin(), VG_LITE_FILTER_POINT, &acc);
    s_unaligned_rc = acc;
    vg_lite_error_t fin = VG_LITE_SUCCESS;
    vgc_finish_into(&fin);
    return fin;
}

/* ---- 4. blit/formats ---------------------------------------------------------
 * RGB565 source (stride 32 B -- FORMAT_ALIGNMENT(stride,32) satisfied) into
 * the BGRA8888 target. Two questions: does the 5/6/5 expansion land inside the
 * red/blue bands (>= 248 by shift, 255 by replication -- both accepted), and
 * WHICH WAY ROUND is the 16-bit word. The second has never been measured in
 * this tree; the case is order-agnostic and REPORTS it, and
 * expected_silicon.txt pre-registers `low` (red in bits 4:0, by analogy with
 * the measured BGRA8888 word). After the boot the measured order is PINNED
 * here, as Phase 2 pinned its blend reading -- a case that stays green under
 * either order is invisible to the drift checker. */
/* ★ PINNED 2026-09-02, THREE BOOTS IDENTICAL: order=low, and the 5-bit
 * channels expand by REPLICATION (0x1F -> 255), not by shift (248). The case
 * still REPORTS order= (a flip says which way it went) but no longer accepts
 * `high`: a case green under either order is invisible to the drift checker,
 * exactly the hole Phase 2 closed for its blend reading. tests/cases_blit_test
 * arm 7 models a red-in-the-high-bits sampler and this case must go broken
 * WITH order=high on the line. */
#define B_ORDER_MEASURED "low"
static vgc_verdict_t check_formats(char *d, size_t n)
{
    blit_profile_t p;
    blit_profile(&p, 1);
    const vgc_cover_t cv = vgc_cover_na();
    int off = profile_str(&p, d, n);
    if (off < 0) off = 0;
    snprintf(d + off, n - (size_t)off, ",order=%s,%s", p.order, cv.s);
    return (p.checker && strcmp(p.order, B_ORDER_MEASURED) == 0) ? VGC_OK : VGC_BROKEN;
}
static vg_lite_error_t run_formats(void)
{
    fill_checker16(s_src565, 16);
    const vg_lite_error_t e = source_ready(&s_b565, &s_mapped565, s_src565, 32, VG_LITE_RGB565);
    if (e != VG_LITE_SUCCESS) return e;
    return blit_once(&s_b565);
}

/* ---- scissor ---------------------------------------------------------------- */

/* The oversized rect: 120x120 at (4,4) in the scratch, 56x56 at (4,4) in the
 * small target. Emitted through the arena as every other path is. */
static vg_lite_error_t big_rect(vg_lite_path_t *p, int32_t size)
{
    vgc_emit_rect_cw(4, 4, size, size);
    return vgc_finish_path(p, 4, 4, 4 + size, 4 + size);
}

/* Four edges, judged separately. `px` reads the target in question. Each
 * outside sample sits 4 px beyond its edge on the mid-line of the opposite
 * axis; `in` is the centre. L/T/R/B are 1 when the outside sample stayed
 * black (clipped). */
typedef struct { int L, T, R, B, in; } sc_t;
static void scissor_profile(sc_t *s, uint32_t (*px)(int, int), int x0, int y0, int x1, int y1)
{
    const int mx = (x0 + x1) / 2, my = (y0 + y1) / 2;
    #define BLK(px_) (vgc_ch((px_), VGC_G) <= B_ZERO && vgc_ch((px_), VGC_R) <= B_ZERO && vgc_ch((px_), VGC_B) <= B_ZERO)
    s->L  = BLK(px(x0 - 4, my));
    s->T  = BLK(px(mx, y0 - 4));
    s->R  = BLK(px(x1 + 3, my));
    s->B  = BLK(px(mx, y1 + 3));
    s->in = !BLK(px(mx, my));
    #undef BLK
}

static void scissor_off(vg_lite_error_t *acc)
{
    const vg_lite_error_t e = vg_lite_set_scissor(-1, -1, -1, -1);
    if (e != VG_LITE_SUCCESS && *acc == VG_LITE_SUCCESS) *acc = e;
}

/* ---- 5. scissor/basic ----------------------------------------------------------
 * The shipping regime: 64x64 tess against the 128x128 scratch. Scissor
 * (40,40,88,88) -- right and bottom EXCLUSIVE, as the fader passes
 * area.x2 + 1. All four edges must clip and the interior must paint. */
#define SB_X0 40
#define SB_Y0 40
#define SB_X1 88
#define SB_Y1 88
static vgc_verdict_t check_scissor_basic(char *d, size_t n)
{
    sc_t s; scissor_profile(&s, vgc_px, SB_X0, SB_Y0, SB_X1, SB_Y1);
    const vgc_cover_t cv = vgc_cover_na();
    snprintf(d, n, "L=%d,T=%d,R=%d,B=%d,in=%d,%s", s.L, s.T, s.R, s.B, s.in, cv.s);
    return (s.L && s.T && s.R && s.B && s.in) ? VGC_OK : VGC_BROKEN;
}
static vg_lite_error_t run_scissor_basic(void)
{
    vg_lite_error_t acc = VG_LITE_SUCCESS;
    vg_lite_path_t p;
    const vg_lite_error_t fe = big_rect(&p, 120);
    if (fe != VG_LITE_SUCCESS) return fe;
    const vg_lite_error_t se = vg_lite_set_scissor(SB_X0, SB_Y0, SB_X1, SB_Y1);
    if (se != VG_LITE_SUCCESS) acc = se;
    vgc_draw_path_to(&vgc_scratch, &p, VG_LITE_FILL_NON_ZERO, VGC_FILL_COLOR, &acc);
    vgc_finish_into(&acc);
    scissor_off(&acc);
    return acc;
}

/* ---- 6. scissor/tess-fullscreen ------------------------------------------------
 * The same draw into vgc_small (64x64 under the 64x64 tess buffer: the
 * FULLSCREEN regime). Scissor (20,20,44,44).
 *
 * ★ PREDICTED BROKEN WITH L=0,T=0,R=1,B=1 -- not merely broken. Left and top
 * are painted past the scissor because the tess-window clamp that implements
 * them is skipped in this regime; right and bottom still clip because 0x0A13
 * is programmed regardless. L=1,T=1 would retire the fader's warning;
 * R=0,B=0 as well would mean the scissor is DEAD here, a different finding.
 * Either gets a written reason.
 *
 * The case clears its own target: the harness clears only vgc_scratch. */
#define SF_X0 20
#define SF_Y0 20
#define SF_X1 44
#define SF_Y1 44
static vgc_verdict_t check_scissor_full(char *d, size_t n)
{
    sc_t s; scissor_profile(&s, vgc_px_small, SF_X0, SF_Y0, SF_X1, SF_Y1);
    const vgc_cover_t cv = vgc_cover_na();
    snprintf(d, n, "L=%d,T=%d,R=%d,B=%d,in=%d,%s", s.L, s.T, s.R, s.B, s.in, cv.s);
    return (s.L && s.T && s.R && s.B && s.in) ? VGC_OK : VGC_BROKEN;
}
static vg_lite_error_t run_scissor_full(void)
{
    const vg_lite_error_t ce = vgc_clear_small();
    if (ce != VG_LITE_SUCCESS) return ce;
    vg_lite_error_t acc = VG_LITE_SUCCESS;
    vg_lite_path_t p;
    const vg_lite_error_t fe = big_rect(&p, 56);
    if (fe != VG_LITE_SUCCESS) return fe;
    const vg_lite_error_t se = vg_lite_set_scissor(SF_X0, SF_Y0, SF_X1, SF_Y1);
    if (se != VG_LITE_SUCCESS) acc = se;
    vgc_draw_path_to(&vgc_small, &p, VG_LITE_FILL_NON_ZERO, VGC_FILL_COLOR, &acc);
    vgc_finish_into(&acc);
    scissor_off(&acc);
    return acc;
}

/* The fullscreen case renders into vgc_small, which the harness's default
 * sum never sees; without this hook repeat= would compare two identical
 * untouched scratches and say `same` about a render it never hashed. */
static uint32_t sum_scissor_full(void)
{
    uint32_t acc = 2166136261u;
    for (int y = 0; y < VGC_SMALL_H; y++)
        for (int x = 0; x < VGC_SMALL_W; x++) {
            const uint32_t w = vgc_px_small(x, y);
            for (int i = 0; i < 4; i++) { acc ^= (w >> (i * 8)) & 0xFFu; acc *= 16777619u; }
        }
    return acc;
}

/* ---- the table ------------------------------------------------------------- */
const vgc_case_t vgc_blit_cases[] = {
    { "blit/basic",             run_basic,         check_basic,         NULL },
    { "blit/stride-64",         run_stride64,      check_stride64,      NULL },
    { "blit/stride-unaligned",  run_unaligned,     check_unaligned,     NULL },
    { "blit/formats",           run_formats,       check_formats,       NULL },
    { "scissor/basic",          run_scissor_basic, check_scissor_basic, NULL },
    { "scissor/tess-fullscreen",run_scissor_full,  check_scissor_full,  sum_scissor_full },
};
const size_t vgc_blit_case_count = sizeof(vgc_blit_cases) / sizeof(vgc_blit_cases[0]);
