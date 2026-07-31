/* lvgl_pxp_copy_bench - CPU vs PXP for LVGL's cross-buffer sync copy (v7).
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * TWO CLAIMS x TWO FORMATS, per the two-gate rule:
 *   CORRECTNESS (QEMU-gated): for every one of the 14 geometry cases, in
 *       BOTH RGB565 and XRGB8888, a PXP sub-rect copy produces a byte-
 *       identical destination to LVGL's own lv_draw_buf_copy.  28 cases
 *       total; the gate pins the count so a dropped case OR a dropped
 *       format cannot hide behind a passing sweep.  The offset-base
 *       arithmetic proven here (now byte-based, bpp-scaled) is the
 *       arithmetic the future handler (spec 3, v7 dual-format) will use --
 *       proven BEFORE that handler exists.
 *   TIMING (hardware-only): DWT cycle counts for both paths per case, per
 *       format.  QEMU has no timing model; its numbers are printed but
 *       VACUOUS, and the transcript says so.  The hardware table is the
 *       v7 8888-crossover decision input.
 *
 * The matrix deliberately includes odd widths, odd x-offsets and edge-
 * hugging rects: PXPSurface has no origin field, so sub-rects are offset
 * base addresses -- exactly where pitch arithmetic goes wrong silently.
 *
 * Both formats share ONE pair of buffers, allocated once at the 4 B/px
 * (XRGB8888) maximum extent and reused for the 565 pass.  Fills and
 * checksums always cover the WHOLE max-size allocation, in both formats --
 * not just the bytes the current format's stride/height actually address.
 * That is what keeps an out-of-bounds PXP write in the 565 half-empty case
 * exactly as red as a wrong pixel: 565's real footprint is half the
 * allocation, so any write past it still lands in checksummed bytes.
 *
 * Uses Serial1 (LPUART; QEMU captures it), like every sibling gate.
 */
#include <Arduino.h>
#include "lvgl_rt1176.h"
#include "PXP.h"

static constexpr uint32_t BUF_W = 720, BUF_H = 1280;
static constexpr uint32_t BUF_BYTES_MAX = BUF_W * 4u * BUF_H;   /* 8888 extent */

struct Case { uint16_t w, h, x, y; };
/* The matrix (spec 9 Q1, fixed here): button-scale through full-screen, with
 * odd offsets/widths and edge-hugging rects.  14 cases; the gate pins the
 * count so a dropped case cannot pass silently. */
static constexpr Case CASES[] = {
    {  16,   16,   0,    0}, {  16,   16,  13,    7},
    {  64,   64,   0,    0}, {  64,   64,  13,    7},
    { 120,  140,   0,    0}, { 120,  140, 599, 1139},
    { 200,  160,   8,   48}, { 360,  320,   0,    0},
    { 360,  320, 180,  480}, { 719,    1,   1,    0},
    {   1, 1280, 719,    0}, { 720,  640,   0,  320},
    { 720,  640,   0,  640}, { 720, 1280,   0,    0},
};
static constexpr uint8_t NUM_CASES = sizeof(CASES) / sizeof(CASES[0]);

struct Fmt {
    const char       *tag;      /* token field: f=<tag>  */
    lv_color_format_t cf;
    uint8_t           bpp;
    uint32_t          seed_a, seed_b;   /* format-dependent fills: a copy    */
};                                      /* routed at the wrong bpp cannot    */
                                        /* checksum clean                    */
static constexpr Fmt FMTS[] = {
    { "565",  LV_COLOR_FORMAT_RGB565,   2u, 0xA53Cu,     0x0F1Eu     },
    { "8888", LV_COLOR_FORMAT_XRGB8888, 4u, 0xC3A5F00Du, 0x1E2D3C4Bu },
};

static uint8_t *s_src, *s_dst;
static lv_draw_buf_t s_src_db, s_dst_db;

static uint8_t *alloc_buf() {
    uint8_t *raw = (uint8_t *)extmem_malloc(BUF_BYTES_MAX + 64);
    if (!raw) return nullptr;
    return (uint8_t *)(((uintptr_t)raw + 63) & ~(uintptr_t)63);
}

/* Position-dependent AND format-dependent fills over the WHOLE max-size
 * allocation (the GT911 blob-filler lesson, plus v7's format seed). */
static void fill_buf(uint8_t *b, uint32_t seed) {
    uint32_t *w = (uint32_t *)b;
    for (uint32_t i = 0; i < BUF_BYTES_MAX / 4; i++)
        w[i] = seed ^ (i * 2654435761u);
}

static uint32_t dst_sum() {                 /* FNV-1a over the WHOLE dest */
    lvgl_sum_reset();
    lvgl_sum_feed(s_dst, BUF_BYTES_MAX);
    return lvgl_sum_value();
}

static uint32_t cycles_us(uint32_t cyc) { return cyc / 996u; }

/* PXPError -> token for the per-case failure line (PXP.h:83-95). */
static const char *pxp_err_name(PXPError e) {
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

void setup()
{
    Serial1.begin(115200);
    delay(200);
    Serial1.println("PXP_COPY_BENCH_BEGIN");

    s_src = alloc_buf(); s_dst = alloc_buf();
    if (!s_src || !s_dst) { Serial1.println("ALLOC_FAIL"); Serial1.println("PXP_COPY_BENCH_DONE"); return; }
    Serial1.println("ALLOC_OK");

    lvgl_rt1176_begin();   /* lv_init: the default draw-buf handlers exist */

    /* PXP bring-up exactly as pxp_blit_test does it: begin() performs the
     * RM 52.5 reset ritual internally; afterwards CTRL's SFTRST and CLKGATE
     * must both read clear or the block is not actually out of reset. */
    bool pxp_up = PXP.begin();
    uint32_t ctrl = PXP_CTRL;
    if (!pxp_up || (ctrl & (PXP_CTRL_SFTRST | PXP_CTRL_CLKGATE))) {
        Serial1.println("PXP_FAIL");
        Serial1.println("PXP_COPY_BENCH_DONE");
        return;
    }
    Serial1.println("PXP_OK");

    /* (The old one-shot fill_src()/fill_dst() are gone: fills are per-format
     * now and happen inside the loop below.) */
    bool all_ok = true;
    uint32_t printed = 0;

    for (const Fmt &F : FMTS) {
        const uint32_t stride = BUF_W * F.bpp;
        /* lv_draw_buf_init(buf, w, h, cf, stride, data, data_size) ->
         * lv_result_t (lv_draw_buf.h:259); LV_RESULT_OK on success. */
        if (lv_draw_buf_init(&s_src_db, BUF_W, BUF_H, F.cf, stride,
                             s_src, stride * BUF_H) != LV_RESULT_OK ||
            lv_draw_buf_init(&s_dst_db, BUF_W, BUF_H, F.cf, stride,
                             s_dst, stride * BUF_H) != LV_RESULT_OK) {
            Serial1.println("DRAWBUF_FAIL");
            Serial1.println("PXP_COPY_BENCH_DONE");
            return;
        }
        fill_buf(s_src, F.seed_a);

        for (uint8_t i = 0; i < NUM_CASES; i++) {
            const Case &c = CASES[i];
            lv_area_t area;
            area.x1 = c.x; area.y1 = c.y;
            area.x2 = (int32_t)c.x + c.w - 1; area.y2 = (int32_t)c.y + c.h - 1;

            /* --- CPU path: LVGL's own copy, default handlers -------------- */
            fill_buf(s_dst, F.seed_b);
            uint32_t t0 = ARM_DWT_CYCCNT;
            lv_draw_buf_copy(&s_dst_db, &area, &s_src_db, &area);
            uint32_t cpu_cyc = ARM_DWT_CYCCNT - t0;
            const uint32_t cpu = dst_sum();

            /* --- PXP path: offset-base sub-rect surfaces ------------------ */
            fill_buf(s_dst, F.seed_b);
            uint8_t *sp = s_src + (uint32_t)c.y * stride + (uint32_t)c.x * F.bpp;
            uint8_t *dp = s_dst + (uint32_t)c.y * stride + (uint32_t)c.x * F.bpp;
            const auto pxp_fmt = (F.bpp == 2u) ? PXP_RGB565 : PXP_XRGB8888;
            /* stride 2880 (720*4) fits uint16_t -- same fact panel_config.h's
             * 16-bit-pitch static assert relies on. */
            PXPSurface ssrc(sp, c.w, c.h, pxp_fmt, (uint16_t)stride);
            PXPSurface sdst(dp, c.w, c.h, pxp_fmt, (uint16_t)stride);
            t0 = ARM_DWT_CYCCNT;
            /* pxp_blit_test's idiom: PXP.blit(src, dst) is synchronous -- it
             * is op().source(src).output(dst).run(), and run() waits (100 ms
             * cap), so the cycle window covers program + enable + completion. */
            const PXPError pe = PXP.blit(ssrc, sdst);
            uint32_t pxp_cyc = ARM_DWT_CYCCNT - t0;
            const uint32_t pxp = dst_sum();

            if (pe != PXP_OK) {  /* per-case failure token, never a hang */
                all_ok = false;
                Serial1.printf("CASE i=%u f=%s r=%ux%u+%u+%u PXP_ERR=%s\n",
                               (unsigned)(i + 1), F.tag, c.w, c.h, c.x, c.y,
                               pxp_err_name(pe));
                (void)pxp_cyc;
                continue;
            }

            const bool ok = (cpu == pxp);
            all_ok = all_ok && ok;
            printed++;
            Serial1.printf("CASE i=%u f=%s r=%ux%u+%u+%u CPU=0x%08lX PXP=0x%08lX %s cpu_us=%lu pxp_us=%lu\n",
                           (unsigned)(i + 1), F.tag, c.w, c.h, c.x, c.y,
                           (unsigned long)cpu, (unsigned long)pxp,
                           ok ? "MATCH" : "MISMATCH",
                           (unsigned long)cycles_us(cpu_cyc),
                           (unsigned long)cycles_us(pxp_cyc));
        }
    }

    Serial1.println("NOTE timings are hardware-only; QEMU numbers are vacuous");
    Serial1.printf("CASES=%lu\n", (unsigned long)printed);
    if (all_ok) Serial1.println("COPY_BENCH_OK");
    Serial1.println("PXP_COPY_BENCH_DONE");
}

void loop() {}
