/* pxp_draw_bench - RT1176 v9 P1: raw draw-op bench, PXP vs LVGL's own SW code
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * The v9 driving measurement: would a PXP DRAW UNIT (accelerating LVGL's
 * rect fills, image blits and alpha composites inside the renderer) earn its
 * place?  This bench produces the raw-op half of the answer: for a 6-size
 * ladder x {fill, blit, composite}, run BOTH the PXP op and LVGL's own SW
 * renderer code on the live RK055 framebuffer, correctness-check each path
 * against its own contract, and DWT-time both.  The hardware table (P2) is
 * the decision input; QEMU numbers are printed but VACUOUS (no timing model)
 * and the transcript says so.
 *
 * SW_PATH=lvgl: the SW comparator is lv_draw_sw_blend() itself -- LVGL's real
 * blend entry, the exact code a draw unit's declined tasks fall through to --
 * driven through a FABRICATED minimal lv_draw_task_t + lv_layer_t +
 * lv_draw_buf_t targeting the framebuffer (no display object, no refresh
 * context; lv_draw_sw_blend() reads only t->clip_area and t->target_layer ->
 * {draw_buf, buf_area, color_format} -- verified in
 * lvgl/src/draw/sw/blend/lv_draw_sw_blend.c:70-167).  A 4x4 smoke fill
 * (SW_SMOKE=OK) proves the fabricated plumbing before any case runs.
 *
 * Correctness per case, at BOTH depths (RGB565 default; XRGB8888 via
 * -DDRAW_BENCH_32=ON):
 *   fill_<w>x<h>:  every in-rect pixel == the path's expected constant
 *       (direct scan), AND whole-frame FNV-1a vs a CPU-maintained expected
 *       frame (the out-of-bounds-write detector, the v8 pattern).  The two
 *       engines' constants differ at 32 bpp in the X byte ONLY: LVGL writes
 *       X=0xFF (lv_color_to_u32, lv_color.c:131-134), the PXP writes X=0
 *       (computed alpha with the alpha engine unconfigured -- v6 bench
 *       hardware DIAG dump, modelled identically in QEMU's imxrt_pxp.c).
 *   blit_<w>x<h>:  same-format image from extmem; byte-compare vs source
 *       (LVGL: row memcpy, X preserved; PXP: RGB-exact, X:=0 at 32 bpp per
 *       the same v6-measured contract) + whole-frame FNV.
 *   comp_<w>x<h> (565 build ONLY -- composites are declined at 32 bpp by
 *       construction, spec 1.4): ARGB8888 source with varied per-pixel alpha
 *       INCLUDING a=0x00 and a=0xFF pixels (the pass-through edges are part
 *       of the measured contract).  The PXP path is asserted against the v8
 *       silicon-measured oracle formulas (transcribed below with citations);
 *       the SW path against its own self-computed expectation (the same
 *       lv_draw_sw_blend() call into the expected-frame scratch -- a
 *       determinism/plumbing check; the SW code itself is LVGL's shipped
 *       renderer, not the code under test).  The two engines LEGITIMATELY
 *       differ on LSBs (LVGL's blend rounding != silicon's /256-truncate):
 *       the per-case max divergence is REPORTED as a measured fact
 *       (COMP n=<name> ENGINE_DELTA=<max lsb, 565 channel units>), never
 *       averaged away or byte-compared across engines.
 *   overhead_16x2: the bare-op constant (program + enable + completion),
 *       the number that determines every crossover.
 *
 * Tokens: DRAW n=<name> CPU_us=<t> PXP_us=<t> OK|BAD per case;
 * DRAWS=<emitted> pinned per depth build (565: 19 = 6 fills + 6 blits +
 * 6 comps + overhead; 32 bpp: 13); DRAW_BENCH_OK; every early-out prints
 * PXP_DRAW_BENCH_DONE (the v8 discipline: the run never hangs silently).
 *
 * Uses Serial1 (LPUART; QEMU captures it), like every sibling gate.
 */
#include <Arduino.h>
#include <string.h>
#include <stdio.h>
#include "Display.h"
#include "PXP.h"
#include "lvgl.h"
#include "lvgl_private.h"   /* lv_draw_task_t / lv_layer_t / blend dsc      */

/* Depth coherence: the two -D definitions travel together (CMake option). */
#if LV_COLOR_DEPTH == 16
static_assert(PANEL_BYTES_PER_PIXEL == 2, "depth/panel bpp must agree");
static constexpr lv_color_format_t FB_CF   = LV_COLOR_FORMAT_RGB565;
static constexpr PXPFormat         FB_PXP  = PXP_RGB565;
#elif LV_COLOR_DEPTH == 32
static_assert(PANEL_BYTES_PER_PIXEL == 4, "depth/panel bpp must agree");
static constexpr lv_color_format_t FB_CF   = LV_COLOR_FORMAT_XRGB8888;
static constexpr PXPFormat         FB_PXP  = PXP_XRGB8888;
#else
#error "LV_COLOR_DEPTH must be 16 or 32"
#endif
static constexpr uint32_t FB_BPP = PANEL_BYTES_PER_PIXEL;

/* ========================= the size ladder ================================ */

struct Rect { uint16_t w, h, x, y; };

/* 6 sizes, button-scale to full-frame; odd offsets and edge-hugging rects
 * (the v6 lesson: offset-base pitch arithmetic goes wrong silently). */
static constexpr Rect LADDER[6] = {
    {  32,   32,  13,    7},
    { 120,   40, 600, 1240},   /* bottom-right edge-hugging (719,1279)      */
    { 240,  160,  17,  560},
    { 400,  300, 320,  980},   /* touches both far edges (719,1279)         */
    { 720,   80,   0,  600},   /* full-width band                           */
    { 720, 1280,   0,    0},   /* full frame                                */
};
static constexpr uint32_t NUM_SIZES = 6;

/* Per-size fill colours (varied so a case cannot pass on a stale rect). */
static constexpr uint32_t FILL_RGBS[NUM_SIZES] = {
    0x00E03818u, 0x001870E0u, 0x0030C060u, 0x00F0F020u, 0x00A020F0u, 0x00208080u,
};
static constexpr Rect     OVERHEAD_RECT = { 16, 2, 13, 7 };
static constexpr uint32_t OVERHEAD_RGB  = 0x00FF4000u;

#if LV_COLOR_DEPTH == 16
static constexpr uint32_t PINNED_DRAWS = 19;   /* 6 fills + 6 blits + 6 comps + overhead */
#else
static constexpr uint32_t PINNED_DRAWS = 13;   /* 6 fills + 6 blits + overhead           */
#endif

/* ===================== expected-constant helpers ==========================
 * These encode each engine's OWN contract for a fill -- independently of the
 * code that draws, so a sabotage here goes red (the negative-test anchor). */

/* 888 -> RGB565: take the top bits (truncation).  Both engines: the PXP OUT
 * stage narrows this way (RM 52.3.1.22, v8-verified) and LVGL's
 * lv_color_to_u16 (lv_color.c:126-129) uses the same masks.
 * NEGATIVE-TEST ANCHOR (fill): sabotaging a mask here must turn every
 * fill_* case BAD -- proven red-first in run_qemu_pxp_draw.sh's record. */
static inline uint16_t fill_expected_565(uint32_t rgb)
{
    return (uint16_t)((((rgb >> 16) & 0xF8u) << 8) |
                      (((rgb >>  8) & 0xFCu) << 3) |
                      (((rgb      ) & 0xF8u) >> 3));
}

static inline uint32_t sw_fill_expected(uint32_t rgb)
{
#if LV_COLOR_DEPTH == 16
    return fill_expected_565(rgb);
#else
    return 0xFF000000u | (rgb & 0x00FFFFFFu);   /* lv_color_to_u32: X=0xFF  */
#endif
}

static inline uint32_t pxp_fill_expected(uint32_t rgb)
{
#if LV_COLOR_DEPTH == 16
    return fill_expected_565(rgb);
#else
    return rgb & 0x00FFFFFFu;   /* X := computed alpha = 0 (v6-measured)    */
#endif
}

/* ============================ patterns ==================================== */

/* Position-dependent seeds (the decimate-test idiom: any stride error changes
 * the checksum).  Distinct seeds per role so a blit reading the wrong buffer
 * cannot checksum clean. */
static inline uint32_t pat32(uint32_t x, uint32_t y, uint32_t seed)
{
    uint32_t v = (x * 2654435761u) ^ (y * 2246822519u) ^ seed;
    return v ^ (v >> 15);
}
static constexpr uint32_t SEED_FB   = 0x9E3779B9u;
static constexpr uint32_t SEED_SRC  = 0x85EBCA6Bu;
static constexpr uint32_t SEED_COMP = 0xC2B2AE35u;

/* ============================ FNV-1a-32 =================================== */

static uint32_t fnv1a(const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    uint32_t h = 2166136261u;
    while (n--) { h ^= *b++; h *= 16777619u; }
    return h;
}

/* ====================== buffers (extmem, 64B aligned) ===================== */

static uint8_t *s_fb;        /* the live panel framebuffer                   */
static uint8_t *s_exp;       /* CPU-maintained expected frame (FNV oracle)   */
static uint8_t *s_src;       /* blit source, full-frame footprint at fb depth*/
#if LV_COLOR_DEPTH == 16
static uint8_t *s_comp;      /* ARGB8888 composite source, full frame        */
static uint8_t *s_snap;      /* PXP composite result snapshot (ENGINE_DELTA) */
#endif

static uint8_t *alloc_ext(size_t bytes)
{
    uint8_t *raw = (uint8_t *)extmem_malloc(bytes + 64);
    if (!raw) return nullptr;
    return (uint8_t *)(((uintptr_t)raw + 63) & ~(uintptr_t)63);
}

/* PXPError -> token for the per-case failure line (the sibling idiom). */
static const char *pxp_err_name(PXPError e)
{
    switch (e) {
    case PXP_OK:                return "PXP_OK";
    case PXP_ERR_BUSY:          return "PXP_ERR_BUSY";
    case PXP_ERR_TIMEOUT:       return "PXP_ERR_TIMEOUT";
    case PXP_ERR_CONFIG:        return "PXP_ERR_CONFIG";
    case PXP_ERR_UNREACHABLE:   return "PXP_ERR_UNREACHABLE";
    case PXP_ERR_ALIGN:         return "PXP_ERR_ALIGN";
    case PXP_ERR_AXI_READ:      return "PXP_ERR_AXI_READ";
    case PXP_ERR_AXI_WRITE:     return "PXP_ERR_AXI_WRITE";
    case PXP_ERR_NOT_BEGUN:     return "PXP_ERR_NOT_BEGUN";
    case PXP_ERR_FORMAT:        return "PXP_ERR_FORMAT";
    case PXP_ERR_UNIMPLEMENTED: return "PXP_ERR_UNIMPLEMENTED";
    }
    return "PXP_ERR_UNKNOWN";
}

static uint32_t cycles_us(uint32_t cyc) { return cyc / 996u; }

/* ========================= rect helpers =================================== */

static inline uint32_t rect_off(const Rect &r)
{
    return (uint32_t)r.y * PANEL_PITCH_BYTES + (uint32_t)r.x * FB_BPP;
}

/* Seed a rect with the position pattern in BOTH fb and expected -- they stay
 * in agreement, so the whole-frame FNV catches exactly the writes the current
 * op should not have made. */
static void seed_rect(const Rect &r)
{
    for (uint32_t y = 0; y < r.h; y++) {
        uint8_t *frow = s_fb  + ((uint32_t)r.y + y) * PANEL_PITCH_BYTES
                              + (uint32_t)r.x * FB_BPP;
        uint8_t *erow = s_exp + ((uint32_t)r.y + y) * PANEL_PITCH_BYTES
                              + (uint32_t)r.x * FB_BPP;
        for (uint32_t x = 0; x < r.w; x++) {
            uint32_t v = pat32(r.x + x, r.y + y, SEED_FB);
#if LV_COLOR_DEPTH == 16
            ((uint16_t *)frow)[x] = (uint16_t)v;
            ((uint16_t *)erow)[x] = (uint16_t)v;
#else
            ((uint32_t *)frow)[x] = v;      /* X byte varied deliberately    */
            ((uint32_t *)erow)[x] = v;
#endif
        }
    }
}

static bool scan_rect_const(const uint8_t *b, const Rect &r, uint32_t v)
{
    for (uint32_t y = 0; y < r.h; y++) {
        const uint8_t *row = b + ((uint32_t)r.y + y) * PANEL_PITCH_BYTES
                               + (uint32_t)r.x * FB_BPP;
        for (uint32_t x = 0; x < r.w; x++) {
#if LV_COLOR_DEPTH == 16
            if (((const uint16_t *)row)[x] != (uint16_t)v) return false;
#else
            if (((const uint32_t *)row)[x] != v) return false;
#endif
        }
    }
    return true;
}

static void set_rect_const(uint8_t *b, const Rect &r, uint32_t v)
{
    for (uint32_t y = 0; y < r.h; y++) {
        uint8_t *row = b + ((uint32_t)r.y + y) * PANEL_PITCH_BYTES
                         + (uint32_t)r.x * FB_BPP;
        for (uint32_t x = 0; x < r.w; x++) {
#if LV_COLOR_DEPTH == 16
            ((uint16_t *)row)[x] = (uint16_t)v;
#else
            ((uint32_t *)row)[x] = v;
#endif
        }
    }
}

/* Row copies between full-stride buffers. */
static void copy_rect(uint8_t *dst, const uint8_t *src, const Rect &r)
{
    for (uint32_t y = 0; y < r.h; y++)
        memcpy(dst + ((uint32_t)r.y + y) * PANEL_PITCH_BYTES + (uint32_t)r.x * FB_BPP,
               src + ((uint32_t)r.y + y) * PANEL_PITCH_BYTES + (uint32_t)r.x * FB_BPP,
               (size_t)r.w * FB_BPP);
}

#if LV_COLOR_DEPTH == 32
/* The PXP-blit contract at 32 bpp: RGB from the source, X := 0 (computed
 * alpha with the engine unconfigured -- v6 bench DIAG dump; QEMU models the
 * same bytes). */
static void copy_rect_x0(uint8_t *dst, const uint8_t *src, const Rect &r)
{
    for (uint32_t y = 0; y < r.h; y++) {
        const uint32_t *sp = (const uint32_t *)(src + ((uint32_t)r.y + y) * PANEL_PITCH_BYTES
                                                    + (uint32_t)r.x * 4u);
        uint32_t *dp = (uint32_t *)(dst + ((uint32_t)r.y + y) * PANEL_PITCH_BYTES
                                        + (uint32_t)r.x * 4u);
        for (uint32_t x = 0; x < r.w; x++) dp[x] = sp[x] & 0x00FFFFFFu;
    }
}
#endif

static bool cmp_rect(const uint8_t *a, const uint8_t *b, const Rect &r)
{
    for (uint32_t y = 0; y < r.h; y++) {
        uint32_t off = ((uint32_t)r.y + y) * PANEL_PITCH_BYTES + (uint32_t)r.x * FB_BPP;
        if (memcmp(a + off, b + off, (size_t)r.w * FB_BPP) != 0) return false;
    }
    return true;
}

static bool frames_match(void)
{
    return fnv1a(s_fb, PANEL_FB_BYTES) == fnv1a(s_exp, PANEL_FB_BYTES);
}

/* ================= the fabricated LVGL draw context =======================
 * lv_draw_sw_blend(t, dsc) reads t->clip_area and t->target_layer ->
 * {draw_buf->header.stride, color_format, buf_area} and resolves the dest
 * pointer via lv_draw_buf_goto_xy (lv_draw_sw_blend.c:70-167, lv_draw.c:520).
 * No display, no refresh context, no OS -- fabricate exactly those fields. */

static lv_draw_buf_t  s_db_fb,   s_db_exp;
static lv_layer_t     s_ly_fb,   s_ly_exp;
static lv_draw_task_t s_task_fb, s_task_exp;

static bool sw_ctx_init(lv_draw_buf_t *db, lv_layer_t *ly, lv_draw_task_t *t,
                        void *buf)
{
    if (lv_draw_buf_init(db, PANEL_WIDTH, PANEL_HEIGHT, FB_CF,
                         PANEL_PITCH_BYTES, buf, PANEL_FB_BYTES) != LV_RESULT_OK)
        return false;
    lv_memzero(ly, sizeof(*ly));
    ly->draw_buf       = db;
    ly->buf_area.x1    = 0;
    ly->buf_area.y1    = 0;
    ly->buf_area.x2    = (int32_t)PANEL_WIDTH - 1;
    ly->buf_area.y2    = (int32_t)PANEL_HEIGHT - 1;
    ly->phy_clip_area  = ly->buf_area;
    ly->color_format   = FB_CF;
    lv_memzero(t, sizeof(*t));
    t->target_layer = ly;
    t->clip_area    = ly->buf_area;
    return true;
}

static void sw_fill(lv_draw_task_t *t, const Rect &r, uint32_t rgb)
{
    lv_area_t a;
    a.x1 = r.x; a.y1 = r.y;
    a.x2 = (int32_t)r.x + r.w - 1; a.y2 = (int32_t)r.y + r.h - 1;
    lv_draw_sw_blend_dsc_t d;
    lv_memzero(&d, sizeof(d));
    d.blend_area = &a;
    d.color      = lv_color_make((uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8),
                                 (uint8_t)rgb);
    d.opa        = LV_OPA_COVER;
    d.blend_mode = LV_BLEND_MODE_NORMAL;
    lv_draw_sw_blend(t, &d);
}

/* src_base points at the SOURCE BUFFER's rect top-left; src_area == the blend
 * area, so lv_draw_sw_blend's src offset arithmetic resolves to zero. */
static void sw_blit(lv_draw_task_t *t, const Rect &r, const uint8_t *src_base,
                    uint32_t src_stride, lv_color_format_t scf)
{
    lv_area_t a;
    a.x1 = r.x; a.y1 = r.y;
    a.x2 = (int32_t)r.x + r.w - 1; a.y2 = (int32_t)r.y + r.h - 1;
    lv_draw_sw_blend_dsc_t d;
    lv_memzero(&d, sizeof(d));
    d.blend_area       = &a;
    d.src_area         = &a;
    d.src_buf          = src_base;
    d.src_stride       = src_stride;
    d.src_color_format = scf;
    d.opa              = LV_OPA_COVER;
    d.blend_mode       = LV_BLEND_MODE_NORMAL;
    lv_draw_sw_blend(t, &d);
}

#if LV_COLOR_DEPTH == 16
/* =================== the composite oracle (PXP path) ======================
 * Transcribed from the v8 silicon-measured truth table
 * (pxp_composite_test/transcript_hw_evkb.txt RESOLVED AMBIGUITIES; the same
 * formulas live in pxp_composite_test.cpp's MEASURED-tagged knobs and in
 * QEMU's imxrt_pxp.c).  Only the EMBEDDED-alpha, no-key, no-ROP path is
 * exercised here, so only that path is transcribed. */

/* RGB565 -> 888 by bit replication (RM 52.3.1.22 rule 3, uncontested). */
static inline uint32_t o_expand565(uint16_t c)
{
    uint32_t r = (c >> 11) & 0x1Fu, g = (c >> 5) & 0x3Fu, b = c & 0x1Fu;
    r = (r << 3) | (r >> 2);
    g = (g << 2) | (g >> 4);
    b = (b << 3) | (b >> 2);
    return (r << 16) | (g << 8) | b;
}

/* 888 -> RGB565 output narrowing: truncation (RM 52.3.1.22, v8-verified). */
static inline uint16_t o_narrow565(uint32_t rgb)
{
    return (uint16_t)((((rgb >> 16) & 0xF8u) << 8) |
                      (((rgb >>  8) & 0xFCu) << 3) |
                      (((rgb      ) & 0xF8u) >> 3));
}

/* MEASURED: 2026-07-31 v8 gradient dump (P2b) -- SOLVED EXACTLY, 256/256
 * points: out = (a*AS + (256-a)*PS) >> 8, plain a, /256, TRUNCATING; exact
 * AS pass-through at a=255.  (Not /255-rounded, not a+(a>>7)-mapped.)
 * NEGATIVE-TEST ANCHOR (composite): sabotaging this arithmetic must turn
 * every comp_* case BAD -- proven red-first in run_qemu_pxp_draw.sh's
 * record. */
static inline uint8_t o_blend_channel(uint8_t ps, uint8_t as, uint32_t a)
{
    if (a == 255u) return as;   /* measured: exact pass-through at full alpha */
    return (uint8_t)((a * as + (256u - a) * ps) >> 8);
}

/* Composite the ARGB8888 source rect over the expected frame's rect, in
 * place (EMBEDDED alpha -- v8 MROW sweeps: effective alpha = the embedded
 * byte, applied per pixel; a=0 resolves to (256*ps)>>8 == ps, the exact
 * PS pass-through edge). */
static void oracle_comp(const Rect &r)
{
    for (uint32_t y = 0; y < r.h; y++) {
        uint16_t *erow = (uint16_t *)(s_exp + ((uint32_t)r.y + y) * PANEL_PITCH_BYTES)
                       + r.x;
        const uint32_t *srow =
            (const uint32_t *)(s_comp + ((uint32_t)r.y + y) * (PANEL_WIDTH * 4u))
            + r.x;
        for (uint32_t x = 0; x < r.w; x++) {
            uint32_t as_w  = srow[x];
            uint32_t a     = as_w >> 24;
            uint32_t as24  = as_w & 0x00FFFFFFu;
            uint32_t ps24  = o_expand565(erow[x]);
            uint32_t out24 =
                ((uint32_t)o_blend_channel((uint8_t)(ps24 >> 16),
                                           (uint8_t)(as24 >> 16), a) << 16) |
                ((uint32_t)o_blend_channel((uint8_t)(ps24 >> 8),
                                           (uint8_t)(as24 >> 8), a) << 8) |
                 (uint32_t)o_blend_channel((uint8_t)ps24, (uint8_t)as24, a);
            erow[x] = o_narrow565(out24);
        }
    }
}

/* Max per-channel divergence between the two engines' composite results, in
 * 565 channel LSB units (5/6/5-bit channels compared directly). */
static uint32_t engine_delta(const Rect &r)
{
    uint32_t maxd = 0;
    for (uint32_t y = 0; y < r.h; y++) {
        const uint16_t *aw = (const uint16_t *)(s_fb   + ((uint32_t)r.y + y) * PANEL_PITCH_BYTES) + r.x;
        const uint16_t *bw = (const uint16_t *)(s_snap + ((uint32_t)r.y + y) * PANEL_PITCH_BYTES) + r.x;
        for (uint32_t x = 0; x < r.w; x++) {
            uint16_t pa = aw[x], pb = bw[x];
            uint32_t d;
            d = (uint32_t)abs((int)((pa >> 11) & 0x1F) - (int)((pb >> 11) & 0x1F));
            if (d > maxd) maxd = d;
            d = (uint32_t)abs((int)((pa >>  5) & 0x3F) - (int)((pb >>  5) & 0x3F));
            if (d > maxd) maxd = d;
            d = (uint32_t)abs((int)( pa        & 0x1F) - (int)( pb        & 0x1F));
            if (d > maxd) maxd = d;
        }
    }
    return maxd;
}
#endif /* LV_COLOR_DEPTH == 16 */

/* ============================ case runners ================================ */

static bool     s_all_ok  = true;
static uint32_t s_emitted = 0;

static void report(const char *name, uint32_t cpu_cyc, uint32_t pxp_cyc, bool ok)
{
    s_emitted++;
    if (!ok) s_all_ok = false;
    Serial1.printf("DRAW n=%s CPU_us=%lu PXP_us=%lu %s\n", name,
                   (unsigned long)cycles_us(cpu_cyc),
                   (unsigned long)cycles_us(pxp_cyc), ok ? "OK" : "BAD");
}

static void run_fill_case(const Rect &r, uint32_t rgb, const char *name)
{
    /* --- SW path: LVGL's own fill ---------------------------------------- */
    seed_rect(r);
    uint32_t t0 = ARM_DWT_CYCCNT;
    sw_fill(&s_task_fb, r, rgb);
    uint32_t cpu_cyc = ARM_DWT_CYCCNT - t0;
    uint32_t swc = sw_fill_expected(rgb);
    bool sw_ok = scan_rect_const(s_fb, r, swc);
    set_rect_const(s_exp, r, swc);
    sw_ok = frames_match() && sw_ok;

    /* --- PXP path: offset-base sub-rect surface -------------------------- */
    seed_rect(r);
    PXPSurface dst(s_fb + rect_off(r), r.w, r.h, FB_PXP,
                   (uint16_t)PANEL_PITCH_BYTES);
    t0 = ARM_DWT_CYCCNT;
    PXPError e = PXP.fill(dst, rgb);
    uint32_t pxp_cyc = ARM_DWT_CYCCNT - t0;
    if (e != PXP_OK) {   /* failure token, never a hang; not counted -> the
                          * DRAWS pin goes red too */
        s_all_ok = false;
        Serial1.printf("DRAW n=%s PXP_ERR=%s\n", name, pxp_err_name(e));
        return;
    }
    uint32_t pxc = pxp_fill_expected(rgb);
    bool pxp_ok = scan_rect_const(s_fb, r, pxc);
    set_rect_const(s_exp, r, pxc);
    pxp_ok = frames_match() && pxp_ok;

    report(name, cpu_cyc, pxp_cyc, sw_ok && pxp_ok);
}

static void run_blit_case(const Rect &r, const char *name)
{
    /* --- SW path: LVGL same-format image blend (row memcpy) -------------- */
    seed_rect(r);
    const uint8_t *src_rect = s_src + rect_off(r);
    uint32_t t0 = ARM_DWT_CYCCNT;
    sw_blit(&s_task_fb, r, src_rect, PANEL_PITCH_BYTES, FB_CF);
    uint32_t cpu_cyc = ARM_DWT_CYCCNT - t0;
    copy_rect(s_exp, s_src, r);         /* memcpy semantics: X preserved     */
    bool sw_ok = cmp_rect(s_fb, s_exp, r) && frames_match();

    /* --- PXP path -------------------------------------------------------- */
    seed_rect(r);
    PXPSurface ssrc((void *)src_rect, r.w, r.h, FB_PXP,
                    (uint16_t)PANEL_PITCH_BYTES);
    PXPSurface sdst(s_fb + rect_off(r), r.w, r.h, FB_PXP,
                    (uint16_t)PANEL_PITCH_BYTES);
    t0 = ARM_DWT_CYCCNT;
    PXPError e = PXP.blit(ssrc, sdst);
    uint32_t pxp_cyc = ARM_DWT_CYCCNT - t0;
    if (e != PXP_OK) {
        s_all_ok = false;
        Serial1.printf("DRAW n=%s PXP_ERR=%s\n", name, pxp_err_name(e));
        return;
    }
#if LV_COLOR_DEPTH == 16
    copy_rect(s_exp, s_src, r);         /* byte-exact (v6-measured)          */
#else
    copy_rect_x0(s_exp, s_src, r);      /* RGB-exact, X := 0 (v6-measured)   */
#endif
    bool pxp_ok = cmp_rect(s_fb, s_exp, r) && frames_match();

    report(name, cpu_cyc, pxp_cyc, sw_ok && pxp_ok);
}

#if LV_COLOR_DEPTH == 16
static void run_comp_case(const Rect &r, const char *name)
{
    const uint8_t *comp_rect = s_comp + (uint32_t)r.y * (PANEL_WIDTH * 4u)
                                      + (uint32_t)r.x * 4u;

    /* --- PXP path vs the v8 oracle --------------------------------------- */
    seed_rect(r);
    PXPSurface reg(s_fb + rect_off(r), r.w, r.h, PXP_RGB565,
                   (uint16_t)PANEL_PITCH_BYTES);
    PXPSurface as((void *)comp_rect, r.w, r.h, PXP_ARGB8888,
                  (uint16_t)(PANEL_WIDTH * 4u));
    PXPOp op = PXP.op();
    op.source(reg).output(reg).overlay(as).overlayAt(0, 0)
      .overlayAlpha(PXP_ALPHA_EMBEDDED);
    uint32_t t0 = ARM_DWT_CYCCNT;
    PXPError e = op.run();
    uint32_t pxp_cyc = ARM_DWT_CYCCNT - t0;
    if (e != PXP_OK) {
        s_all_ok = false;
        Serial1.printf("DRAW n=%s PXP_ERR=%s\n", name, pxp_err_name(e));
        return;
    }
    oracle_comp(r);                     /* into s_exp, from its pre-op state */
    bool pxp_ok = cmp_rect(s_fb, s_exp, r) && frames_match();
    copy_rect(s_snap, s_fb, r);         /* keep the PXP result for the delta */

    /* --- SW path: LVGL's ARGB8888-over-RGB565 blend ----------------------
     * Self-computed expectation: the identical call into the expected-frame
     * context, from identical pre-op content (both rects re-seeded).  A
     * determinism/plumbing check -- the SW code is LVGL's shipped renderer,
     * not the code under test here. */
    seed_rect(r);
    uint32_t t1 = ARM_DWT_CYCCNT;
    sw_blit(&s_task_fb, r, comp_rect, PANEL_WIDTH * 4u, LV_COLOR_FORMAT_ARGB8888);
    uint32_t cpu_cyc = ARM_DWT_CYCCNT - t1;
    sw_blit(&s_task_exp, r, comp_rect, PANEL_WIDTH * 4u, LV_COLOR_FORMAT_ARGB8888);
    bool sw_ok = cmp_rect(s_fb, s_exp, r) && frames_match();

    /* --- the measured two-engine divergence, reported, never averaged ---- */
    Serial1.printf("COMP n=%s ENGINE_DELTA=%lu\n", name,
                   (unsigned long)engine_delta(r));

    report(name, cpu_cyc, pxp_cyc, pxp_ok && sw_ok);
}
#endif /* LV_COLOR_DEPTH == 16 */

/* ================================ setup =================================== */

void setup()
{
    Serial1.begin(115200);
    delay(200);
    Serial1.println("PXP_DRAW_BENCH_BEGIN");
    /* printf, not println: the gate $-anchors these tokens and println's
     * \r\n would defeat the anchor. */
    Serial1.printf("DEPTH=%d\n", (int)LV_COLOR_DEPTH);
    Serial1.printf("SW_PATH=lvgl\n");  /* lv_draw_sw_blend(), not a proxy    */

    if (!Display.begin()) {
        Serial1.println("PANEL_FAIL");
        Serial1.println("PXP_DRAW_BENCH_DONE");
        return;
    }
    Serial1.println("PANEL_OK");
    s_fb = (uint8_t *)Display.framebuffer();

    /* PXP bring-up + reset-ritual readback, exactly as the v8 test does. */
    bool pxp_up = PXP.begin();
    uint32_t ctrl = PXP_CTRL;
    if (!pxp_up || (ctrl & (PXP_CTRL_SFTRST | PXP_CTRL_CLKGATE))) {
        Serial1.println("PXP_FAIL");
        Serial1.println("PXP_DRAW_BENCH_DONE");
        return;
    }
    Serial1.println("PXP_OK");

    s_exp = alloc_ext(PANEL_FB_BYTES);
    s_src = alloc_ext(PANEL_FB_BYTES);
#if LV_COLOR_DEPTH == 16
    s_comp = alloc_ext((size_t)PANEL_WIDTH * PANEL_HEIGHT * 4u);
    s_snap = alloc_ext(PANEL_FB_BYTES);
    bool alloc_ok = s_exp && s_src && s_comp && s_snap;
#else
    bool alloc_ok = s_exp && s_src;
#endif
    if (!alloc_ok) {
        Serial1.println("ALLOC_FAIL");
        Serial1.println("PXP_DRAW_BENCH_DONE");
        return;
    }
    Serial1.println("ALLOC_OK");

    /* The fabricated LVGL context (SW_PATH=lvgl).  lv_init() only: no
     * display object, no tick -- the blend entry needs neither. */
    lv_init();
    if (!sw_ctx_init(&s_db_fb, &s_ly_fb, &s_task_fb, s_fb) ||
        !sw_ctx_init(&s_db_exp, &s_ly_exp, &s_task_exp, s_exp)) {
        Serial1.println("SW_CTX_FAIL");
        Serial1.println("PXP_DRAW_BENCH_DONE");
        return;
    }

    /* Whole-frame seed, fb == expected everywhere from here on. */
    seed_rect(Rect{ (uint16_t)PANEL_WIDTH, (uint16_t)PANEL_HEIGHT, 0, 0 });

    /* 4x4 smoke: prove the fabricated task/layer plumbing drives LVGL's own
     * fill before any case trusts it. */
    {
        Rect smoke{ 4, 4, 0, 0 };
        sw_fill(&s_task_fb, smoke, 0x00102030u);
        bool ok = scan_rect_const(s_fb, smoke, sw_fill_expected(0x00102030u));
        seed_rect(smoke);   /* restore both frames to agreement */
        if (!ok) {
            Serial1.printf("SW_SMOKE=FAIL\n");
            Serial1.println("PXP_DRAW_BENCH_DONE");
            return;
        }
        Serial1.printf("SW_SMOKE=OK\n");
    }

    /* Blit source: full-frame footprint, its own seed. */
    for (uint32_t y = 0; y < PANEL_HEIGHT; y++) {
        uint8_t *row = s_src + y * PANEL_PITCH_BYTES;
        for (uint32_t x = 0; x < PANEL_WIDTH; x++) {
            uint32_t v = pat32(x, y, SEED_SRC);
#if LV_COLOR_DEPTH == 16
            ((uint16_t *)row)[x] = (uint16_t)v;
#else
            ((uint32_t *)row)[x] = v;   /* X varied: the X:=0 contract bites */
#endif
        }
    }

#if LV_COLOR_DEPTH == 16
    /* Composite source: ARGB8888, varied alpha, with GUARANTEED a=0x00 and
     * a=0xFF pixels in every ladder rect (the pass-through edges are part of
     * the measured contract). */
    for (uint32_t y = 0; y < PANEL_HEIGHT; y++) {
        uint32_t *row = (uint32_t *)(s_comp + y * (PANEL_WIDTH * 4u));
        for (uint32_t x = 0; x < PANEL_WIDTH; x++) {
            uint32_t v = pat32(x, y, SEED_COMP);
            uint32_t a = (v >> 24);
            uint32_t sel = (x ^ y) & 0x0Fu;
            if (sel == 0u)      a = 0x00u;
            else if (sel == 1u) a = 0xFFu;
            row[x] = (a << 24) | (v & 0x00FFFFFFu);
        }
    }
#endif

    /* --------------------------- the matrix ------------------------------ */
    char name[24];
    for (uint32_t i = 0; i < NUM_SIZES; i++) {
        snprintf(name, sizeof(name), "fill_%ux%u",
                 (unsigned)LADDER[i].w, (unsigned)LADDER[i].h);
        run_fill_case(LADDER[i], FILL_RGBS[i], name);
    }
    for (uint32_t i = 0; i < NUM_SIZES; i++) {
        snprintf(name, sizeof(name), "blit_%ux%u",
                 (unsigned)LADDER[i].w, (unsigned)LADDER[i].h);
        run_blit_case(LADDER[i], name);
    }
#if LV_COLOR_DEPTH == 16
    for (uint32_t i = 0; i < NUM_SIZES; i++) {
        snprintf(name, sizeof(name), "comp_%ux%u",
                 (unsigned)LADDER[i].w, (unsigned)LADDER[i].h);
        run_comp_case(LADDER[i], name);
    }
#endif
    /* The bare-op constant: a minimal PXP op, timed like every other case. */
    run_fill_case(OVERHEAD_RECT, OVERHEAD_RGB, "overhead_16x2");

    Serial1.println("NOTE timings are hardware-only; QEMU numbers are vacuous");
    Serial1.printf("DRAWS=%lu\n", (unsigned long)s_emitted);
    if (s_all_ok && s_emitted == PINNED_DRAWS) Serial1.println("DRAW_BENCH_OK");
    Serial1.println("PXP_DRAW_BENCH_DONE");
}

void loop() {}
