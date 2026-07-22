/* pxp_blit_test - RT1176 PXP Phase 1 gate
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 */
#include <Arduino.h>
#include <PXP.h>
#include <string.h>

/* ---- surfaces (OCRAM: PXP is a bus master, so never DTCM) ---------------- */
#define SRC_W 32
#define SRC_H 24
#define FB_W  64
#define FB_H  48

DMAMEM static uint16_t src_buf[SRC_W * SRC_H];
DMAMEM static uint16_t fb_buf[FB_W * FB_H];
static   uint16_t ref_buf[FB_W * FB_H];      /* software reference (DTCM ok) */

/* Deterministic, position-dependent pattern - any pitch/stride error shows up. */
static uint16_t pattern(int x, int y)
{
    return (uint16_t)(((x * 7 + y * 31) & 0x1F) << 11 |
                      ((x * 3 + y * 5)  & 0x3F) << 5  |
                      ((x + y * 11)     & 0x1F));
}

static uint32_t fnv1a(const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    uint32_t h = 2166136261u;
    while (n--) { h ^= *b++; h *= 16777619u; }
    return h;
}

static void fill_src(void)
{
    for (int y = 0; y < SRC_H; y++)
        for (int x = 0; x < SRC_W; x++)
            src_buf[y * SRC_W + x] = pattern(x, y);
}

/* Software reference blit: src -> dst at (dx,dy), no rotation yet. */
static void ref_blit(uint16_t *dst, int dpitchPx, int dx, int dy)
{
    for (int y = 0; y < SRC_H; y++)
        for (int x = 0; x < SRC_W; x++)
            dst[(dy + y) * dpitchPx + (dx + x)] = src_buf[y * SRC_W + x];
}

static bool check(const char *token, const uint16_t *got, const uint16_t *want,
                  size_t count)
{
    bool ok = (memcmp(got, want, count * sizeof(uint16_t)) == 0);
    Serial1.print(token); Serial1.println(ok ? "=PASS" : "=FAIL");
    Serial1.print(token); Serial1.print("_SUM=0x");
    Serial1.println(fnv1a(got, count * sizeof(uint16_t)), HEX);
    return ok;
}

void setup() {
    Serial1.begin(115200);
    delay(200);
    Serial1.println("PXP_GATE_START");

    /* Emit CTRL unconditionally, BEFORE branching, so the token is a live
     * observation on every run - clean success, begin()'s self-check tripping,
     * or dirty readback bits alike - and a QEMU-vs-HW datapoint for Task 11.
     * After the RM 52.5 sequence, SFTRST and CLKGATE must both be clear. */
    bool ok = PXP.begin();
    uint32_t ctrl = PXP_CTRL;
    Serial1.print("PXP_CTRL_POST_BEGIN=0x"); Serial1.println(ctrl, HEX);
    if (!ok || (ctrl & (PXP_CTRL_SFTRST | PXP_CTRL_CLKGATE))) {
        Serial1.println("PXP_BEGIN=FAIL");
        Serial1.println("PXP_ALL=FAIL");
        return;
    }
    Serial1.println("PXP_BEGIN=PASS");

    bool all = true;
    fill_src();

    PXPSurface src(src_buf, SRC_W, SRC_H, PXP_RGB565);
    PXPSurface fb (fb_buf,  FB_W,  FB_H,  PXP_RGB565);

    /* --- full-surface blit into the top-left corner --------------------- */
    memset(fb_buf, 0xA5, sizeof(fb_buf));
    memset(ref_buf, 0xA5, sizeof(ref_buf));
    ref_blit(ref_buf, FB_W, 0, 0);
    if (PXP.blit(src, fb) != PXP_OK) { Serial1.println("PXP_BLIT=FAIL op"); all = false; }
    else all &= check("PXP_BLIT", fb_buf, ref_buf, FB_W * FB_H);

    /* --- offset sub-rect blit: surroundings MUST be untouched ----------- */
    memset(fb_buf, 0x5A, sizeof(fb_buf));
    memset(ref_buf, 0x5A, sizeof(ref_buf));
    ref_blit(ref_buf, FB_W, 16, 12);
    if (PXP.op().source(src).output(fb).outputAt(16, 12).run() != PXP_OK) {
        Serial1.println("PXP_SUBRECT=FAIL op"); all = false;
    } else all &= check("PXP_SUBRECT", fb_buf, ref_buf, FB_W * FB_H);

    Serial1.println(all ? "PXP_ALL=PASS" : "PXP_ALL=FAIL");
}

void loop() {}
