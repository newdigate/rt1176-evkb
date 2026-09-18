/* lvgl_pxp_copy_bench - CPU vs PXP vs eDMA for LVGL's cross-buffer sync copy.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * v7 (2026-07-31) measured LVGL's lv_draw_buf_copy against a PXP blit on 14
 * geometries x {RGB565, XRGB8888} and found the PXP's 32-bit copy writes
 * X:=0.  NEW-55 (2026-09-18) turned that into the probe that picks the db
 * pipeline's sync copy: FIVE ARMS, each with ITS OWN BYTE CONTRACT, timed
 * with the panel SCANNING OUT (the v6 numbers had no scanout and read 174 ms
 * where the pipeline reads 260 for the same full-frame copy).
 *
 *   arm=cpu_clib  newlib memcpy per row      byte identity   (the CPU floor)
 *   arm=pxp_x0    PXP.blit                   RGB identity, X:=0 (v7's contract)
 *   arm=pxp_aff   PXP with alphaOut(0xFF)    src=xff:   byte identity with the
 *                                            plain CPU copy (every LVGL buffer
 *                                            has X=0xFF);  src=mixed: RGB
 *                                            identity, X==0xFF -- proves the
 *                                            override is what writes it
 *   arm=pxp_affa  as pxp_aff but the surfaces declared PXP_ARGB8888 (OUT
 *                 encoding 0x00, the one the RM describes as carrying an
 *                 alpha component; pxp_aff's XRGB8888 is 0x04 RGB888).
 *                 Same bytes in memory, so src=xff byte identity holds if
 *                 the bit works at 0x00 -- the arm that separates "silicon
 *                 ignores the bit" from "ignores it for RGB888".
 *                 src=xff: byte identity; src=mixed: RGB identity,
 *                 X==0xFF -- with both rows silicon has three readable
 *                 outcomes: both MATCH = the bit is honoured at 0x00;
 *                 xff MATCH + mixed MISMATCH = 0x00 forwards the PS alpha;
 *                 both MISMATCH = refuted
 *   arm=edma      lvgl_edma_copy_rect        byte identity, any source
 *
 * CORRECTNESS (QEMU-gated): every arm MATCHes its contract on every case, by
 * name; the count is pinned (140).  TIMING (hardware-only): DWT cycles per
 * arm per case, plus ref_us for the lv_draw_buf_copy reference of the same
 * case.  QEMU's numbers are vacuous and the transcript says so.  Decision
 * rule (spec 2): the fastest byte-preserving arm under one refresh period
 * (33 ms) on the full-frame case wins.
 *
 * Fills and checksums cover the WHOLE max-size allocation in both formats,
 * so an out-of-rect write is as red as a wrong pixel (v7's rule).  Only the
 * DEST extent is checksummed: an arm that wrote into the SOURCE would be
 * invisible here (v7's rule, kept).
 * Uses Serial1 (LPUART; QEMU captures it).  Every line stays under 120
 * characters: the core's printf truncates at 128 (NEW-53 finding A). */
#include <Arduino.h>
#include <string.h>
#include "Display.h"
#include "lvgl_rt1176.h"
#include "lvgl_edma_copy.h"
#include "PXP.h"

static constexpr uint32_t BUF_W = 720, BUF_H = 1280;
static constexpr uint32_t BUF_BYTES_MAX = BUF_W * 4u * BUF_H;   /* 8888 extent */

struct Case { uint16_t w, h, x, y; };
/* The v7 matrix, unchanged: button-scale through full-screen, odd offsets
 * and widths, edge-hugging rects.  The gate pins the count. */
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
    const char       *tag;
    lv_color_format_t cf;
    uint8_t           bpp;
    uint32_t          seed_a, seed_b;   /* format-dependent fills: a copy routed */
};                                      /* at the wrong bpp cannot checksum clean */
static constexpr Fmt FMTS[] = {
    { "565",  LV_COLOR_FORMAT_RGB565,   2u, 0xA53Cu,     0x0F1Eu     },
    { "8888", LV_COLOR_FORMAT_XRGB8888, 4u, 0xC3A5F00Du, 0x1E2D3C4Bu },
};

enum Arm : uint8_t { ARM_CPU_CLIB, ARM_PXP_X0, ARM_PXP_AFF, ARM_PXP_AFFA, ARM_EDMA };
static const char *const ARM_TAG[] = { "cpu_clib", "pxp_x0", "pxp_aff", "pxp_affa", "edma" };
enum Src : uint8_t { SRC_MIXED, SRC_XFF };          /* X byte: whatever the seed gives / forced 0xFF */
static const char *const SRC_TAG[] = { "mixed", "xff" };
enum Contract : uint8_t { CON_BYTES, CON_X0, CON_XFF };

/* One line of the matrix per (arm, source, contract); pxp_aff is 32-bit only
 * (alphaOut() is PXP_ERR_CONFIG on a 16-bit output, by design). */
struct Run { Arm arm; Src src; Contract con; bool only8888; };
static constexpr Run RUNS[] = {
    { ARM_CPU_CLIB, SRC_MIXED, CON_BYTES, false },
    { ARM_PXP_X0,   SRC_MIXED, CON_X0,    false },   /* 565: CON_X0 is byte identity */
    { ARM_PXP_AFF,  SRC_XFF,   CON_BYTES, true  },
    { ARM_PXP_AFF,  SRC_MIXED, CON_XFF,   true  },
    { ARM_PXP_AFFA, SRC_XFF,   CON_BYTES, true  },   /* OUT encoding 0x00 (ARGB8888) */
    /* Without this row a pxp_affa MATCH cannot separate "the override is
     * honoured at OUT 0x00" from "0x00 simply forwards the PS alpha byte"
     * (the PS encoding is 0x04 RGB888_ARGB8888, which carries one): a
     * src=xff source has X=0xFF either way.  src=mixed is where the two
     * readings diverge. */
    { ARM_PXP_AFFA, SRC_MIXED, CON_XFF,   true  },
    { ARM_EDMA,     SRC_MIXED, CON_BYTES, false },
};

static uint8_t *s_src, *s_dst;
static lv_draw_buf_t s_src_db, s_dst_db;

static uint8_t *alloc_buf() {
    uint8_t *raw = (uint8_t *)extmem_malloc(BUF_BYTES_MAX + 64);
    if (!raw) return nullptr;
    return (uint8_t *)(((uintptr_t)raw + 63) & ~(uintptr_t)63);
}

/* Position- and format-dependent fill over the WHOLE allocation; x_ff forces
 * byte 3 of every word to 0xFF -- what an LVGL XRGB8888 render leaves there. */
static void fill_buf(uint8_t *b, uint32_t seed, bool x_ff) {
    uint32_t *w = (uint32_t *)b;
    for (uint32_t i = 0; i < BUF_BYTES_MAX / 4; i++) {
        uint32_t v = seed ^ (i * 2654435761u);
        if (x_ff) v |= 0xFF000000u;
        w[i] = v;
    }
}

static uint32_t dst_sum() {                 /* FNV-1a over the WHOLE dest */
    lvgl_sum_reset();
    lvgl_sum_feed(s_dst, BUF_BYTES_MAX);
    return lvgl_sum_value();
}

static uint32_t cycles_us(uint32_t cyc) { return cyc / 996u; }

/* The contract's X-byte expectation, applied to the in-rect bytes of the CPU
 * reference OUTSIDE the timed window.  No-op for 16-bit and for CON_BYTES. */
static void apply_contract(const Fmt &F, const Case &c, Contract con) {
    if (F.bpp != 4u || con == CON_BYTES) return;
    const uint32_t stride = BUF_W * 4u;
    for (uint32_t ry = 0; ry < c.h; ry++) {
        uint32_t *row = (uint32_t *)(void *)(s_dst + ((uint32_t)c.y + ry) * stride + (uint32_t)c.x * 4u);
        for (uint32_t rx = 0; rx < c.w; rx++)
            row[rx] = (con == CON_X0) ? (row[rx] & 0x00FFFFFFu) : (row[rx] | 0xFF000000u);
    }
}

/* Reference: fresh dest, LVGL's own copy (timed), the contract, the sum. */
static uint32_t reference(const Fmt &F, const Case &c, const lv_area_t &area, Contract con, uint32_t *ref_us) {
    fill_buf(s_dst, F.seed_b, false);
    const uint32_t t0 = ARM_DWT_CYCCNT;
    lv_draw_buf_copy(&s_dst_db, &area, &s_src_db, &area);
    *ref_us = cycles_us(ARM_DWT_CYCCNT - t0);
    apply_contract(F, c, con);
    return dst_sum();
}

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

/* One arm into a fresh dest.  Returns an error token, or nullptr. */
static const char *run_arm(Arm arm, const Fmt &F, const Case &c, uint32_t *us) {
    const uint32_t stride = BUF_W * F.bpp;
    const uint8_t *sp = s_src + (uint32_t)c.y * stride + (uint32_t)c.x * F.bpp;
    uint8_t *dp = s_dst + (uint32_t)c.y * stride + (uint32_t)c.x * F.bpp;
    fill_buf(s_dst, F.seed_b, false);
    const char *err = nullptr;
    const uint32_t t0 = ARM_DWT_CYCCNT;
    switch (arm) {
    case ARM_CPU_CLIB:
        for (uint32_t r = 0; r < c.h; r++)
            memcpy(dp + r * stride, sp + r * stride, (size_t)c.w * F.bpp);
        break;
    case ARM_PXP_X0:
    case ARM_PXP_AFF:
    case ARM_PXP_AFFA: {
        /* pxp_affa declares the SAME bytes as ARGB8888 -- OUT encoding 0x00
         * instead of XRGB8888's 0x04 RGB888 -- because RM 52.6.3 introduces
         * OUT_CTRL[ALPHA] with "when generating an output buffer with an
         * alpha component": a MISMATCH on pxp_aff alone could mean the bit
         * is honoured only at 0x00, and this arm says which. */
        const auto pxp_fmt = (F.bpp == 2u) ? PXP_RGB565
                           : (arm == ARM_PXP_AFFA) ? PXP_ARGB8888 : PXP_XRGB8888;
        /* Offset-base sub-rect surfaces, the v7 idiom; 2880 fits uint16_t. */
        PXPSurface ssrc(const_cast<uint8_t *>(sp), c.w, c.h, pxp_fmt, (uint16_t)stride);
        PXPSurface sdst(dp, c.w, c.h, pxp_fmt, (uint16_t)stride);
        PXPOp op = PXP.op();
        op.source(ssrc).output(sdst);
        if (arm == ARM_PXP_AFF || arm == ARM_PXP_AFFA) op.alphaOut(0xFF);
        const PXPError pe = op.run();      /* synchronous: program + enable + wait */
        if (pe != PXP_OK) err = pxp_err_name(pe);
        break; }
    case ARM_EDMA:
        if (!lvgl_edma_copy_rect(dp, stride, sp, stride, (uint32_t)c.w * F.bpp, c.h)) err = "EDMA_ERR";
        break;
    }
    *us = cycles_us(ARM_DWT_CYCCNT - t0);
    return err;
}

void setup()
{
    Serial1.begin(115200);
    while (!Serial1 && millis() < 2000) {}
    Serial1.println("=== BOOT ===");
    Serial1.printf("[APP: %s] [VER: v%u] [BUILD: %s %s]\n", "lvgl_pxp_copy_bench", 2, __DATE__, __TIME__);
    Serial1.println("PXP_COPY_BENCH_BEGIN");

    /* The panel scans out for the whole run: SDRAM contention as in the
     * pipeline.  Its framebuffer is MipiDisplay's own; the bench's buffers
     * are separate.  Recorded as a token so the transcript states the
     * condition the timings were taken under. */
    const bool scan = Display.begin();
    if (scan) Display.fillScreen(0x07E0);
    Serial1.printf("SCANOUT=%d\n", scan ? 1 : 0);

    s_src = alloc_buf(); s_dst = alloc_buf();
    if (!s_src || !s_dst) { Serial1.println("ALLOC_FAIL"); Serial1.println("PXP_COPY_BENCH_DONE"); return; }
    Serial1.println("ALLOC_OK");

    lvgl_rt1176_begin();   /* lv_init: the default draw-buf handlers exist */

    bool pxp_up = PXP.begin();          /* idempotent: fillScreen may have begun it */
    uint32_t ctrl = PXP_CTRL;
    if (!pxp_up || (ctrl & (PXP_CTRL_SFTRST | PXP_CTRL_CLKGATE))) {
        Serial1.println("PXP_FAIL");
        Serial1.println("PXP_COPY_BENCH_DONE");
        return;
    }
    Serial1.println("PXP_OK");

    if (!lvgl_edma_copy_begin()) {
        Serial1.println("EDMA_FAIL");
        Serial1.println("PXP_COPY_BENCH_DONE");
        return;
    }
    Serial1.println("EDMA_OK");

    bool all_ok = true;
    uint32_t printed = 0;

    for (const Fmt &F : FMTS) {
        const uint32_t stride = BUF_W * F.bpp;
        if (lv_draw_buf_init(&s_src_db, BUF_W, BUF_H, F.cf, stride, s_src, stride * BUF_H) != LV_RESULT_OK ||
            lv_draw_buf_init(&s_dst_db, BUF_W, BUF_H, F.cf, stride, s_dst, stride * BUF_H) != LV_RESULT_OK) {
            Serial1.println("DRAWBUF_FAIL");
            Serial1.println("PXP_COPY_BENCH_DONE");
            return;
        }

        for (uint8_t i = 0; i < NUM_CASES; i++) {
            const Case &c = CASES[i];
            lv_area_t area;
            area.x1 = c.x; area.y1 = c.y;
            area.x2 = (int32_t)c.x + c.w - 1; area.y2 = (int32_t)c.y + c.h - 1;

            for (const Run &R : RUNS) {
                if (R.only8888 && F.bpp != 4u) continue;
                fill_buf(s_src, F.seed_a, R.src == SRC_XFF);
                uint32_t ref_us = 0, us = 0;
                const uint32_t ref = reference(F, c, area, R.con, &ref_us);
                const char *err = run_arm(R.arm, F, c, &us);
                const uint32_t got = dst_sum();
                if (err) {
                    all_ok = false;
                    Serial1.printf("CASE i=%u f=%s arm=%s src=%s r=%ux%u+%u+%u ERR=%s\n",
                                   (unsigned)(i + 1), F.tag, ARM_TAG[R.arm], SRC_TAG[R.src],
                                   c.w, c.h, c.x, c.y, err);
                    continue;
                }
                const bool ok = (ref == got);
                all_ok = all_ok && ok;
                printed++;
                Serial1.printf("CASE i=%u f=%s arm=%s src=%s r=%ux%u+%u+%u",
                               (unsigned)(i + 1), F.tag, ARM_TAG[R.arm], SRC_TAG[R.src],
                               c.w, c.h, c.x, c.y);
                Serial1.printf(" REF=0x%08lX GOT=0x%08lX %s ref_us=%lu us=%lu\n",
                               (unsigned long)ref, (unsigned long)got, ok ? "MATCH" : "MISMATCH",
                               (unsigned long)ref_us, (unsigned long)us);
            }
        }
    }

    Serial1.println("NOTE timings are hardware-only; QEMU numbers are vacuous");
    Serial1.printf("CASES=%lu\n", (unsigned long)printed);
    if (all_ok) Serial1.println("COPY_BENCH_OK");
    Serial1.println("PXP_COPY_BENCH_DONE");
}

void loop() {}
