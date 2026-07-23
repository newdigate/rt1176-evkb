/* pxp_blit_test - RT1176 PXP Phase 1 gate
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 */
#include <Arduino.h>
#include <PXP.h>
#include <EventResponder.h>
#include <string.h>

/* ---- surfaces (OCRAM: PXP is a bus master, so never DTCM) ---------------- */
#define SRC_W 32
#define SRC_H 24
#define FB_W  64
#define FB_H  48

/* 64-byte aligned: PXP's rotate/flip guard (RM 52.6.4 recommends 64B for
 * OUT_BUF) rejects a misaligned destination.  Explicit, not layout luck. */
DMAMEM static uint16_t src_buf[SRC_W * SRC_H] __attribute__((aligned(64)));
DMAMEM static uint16_t fb_buf[FB_W * FB_H]    __attribute__((aligned(64)));
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

/* Software reference: src -> dst at (dx,dy) under rotation + flips.
 * Mirrors the mapping the hardware performs; see plan Task 7.
 * NOTE: hardware applies flips BEFORE rotation (RM 52.6.1), so this inverse
 * mapping un-rotates first and un-flips second.  That ordering is deliberate. */
static void ref_blit_xf(uint16_t *dst, int dpitchPx, int dx, int dy,
                        PXPRotation rot, bool hflip, bool vflip)
{
    int win_w = (rot == PXP_ROT_90 || rot == PXP_ROT_270) ? SRC_H : SRC_W;
    int win_h = (rot == PXP_ROT_90 || rot == PXP_ROT_270) ? SRC_W : SRC_H;

    for (int py = 0; py < win_h; py++) {
        for (int px = 0; px < win_w; px++) {
            int sx, sy;
            switch (rot) {
            case PXP_ROT_0:   sx = px;              sy = py;              break;
            case PXP_ROT_90:  sx = py;              sy = (SRC_H-1) - px;  break;
            case PXP_ROT_180: sx = (SRC_W-1) - px;  sy = (SRC_H-1) - py;  break;
            default:          sx = (SRC_W-1) - py;  sy = px;              break;
            }
            if (hflip) sx = (SRC_W-1) - sx;
            if (vflip) sy = (SRC_H-1) - sy;
            dst[(dy + py) * dpitchPx + (dx + px)] = src_buf[sy * SRC_W + sx];
        }
    }
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

    /* --- solid fill via PS_BACKGROUND (RGB888 -> output format) ---------- */
    /* Alignment-exact colour: R/B low 3 bits and G low 2 bits are zero, so the
     * RGB888->RGB565 narrow is exact regardless of trunc-vs-round on silicon. */
    const uint32_t FILL_RGB888 = 0x0080C080u;   /* R=0x80 G=0xC0 B=0x80 */
    uint16_t fill565 = (uint16_t)(((0x80u >> 3) << 11) |
                                  ((0xC0u >> 2) << 5)  |
                                   (0x80u >> 3));       /* = 0x8610 */
    memset(fb_buf, 0x00, sizeof(fb_buf));
    for (int i = 0; i < FB_W * FB_H; i++) ref_buf[i] = fill565;
    if (PXP.fill(fb, FILL_RGB888) != PXP_OK) { Serial1.println("PXP_FILL=FAIL op"); all = false; }
    else all &= check("PXP_FILL", fb_buf, ref_buf, FB_W * FB_H);

    /* --- full-surface blit into the top-left corner --------------------- */
    memset(fb_buf, 0xA5, sizeof(fb_buf));
    memset(ref_buf, 0xA5, sizeof(ref_buf));
    ref_blit_xf(ref_buf, FB_W, 0, 0, PXP_ROT_0, false, false);
    if (PXP.blit(src, fb) != PXP_OK) { Serial1.println("PXP_BLIT=FAIL op"); all = false; }
    else all &= check("PXP_BLIT", fb_buf, ref_buf, FB_W * FB_H);

    /* --- offset sub-rect blit: surroundings MUST be untouched ----------- */
    memset(fb_buf, 0x5A, sizeof(fb_buf));
    memset(ref_buf, 0x5A, sizeof(ref_buf));
    ref_blit_xf(ref_buf, FB_W, 16, 12, PXP_ROT_0, false, false);
    if (PXP.op().source(src).output(fb).outputAt(16, 12).run() != PXP_OK) {
        Serial1.println("PXP_SUBRECT=FAIL op"); all = false;
    } else all &= check("PXP_SUBRECT", fb_buf, ref_buf, FB_W * FB_H);

    /* --- geometry: rotations and flips ---------------------------------- */
    struct { const char *tok; PXPRotation rot; bool h, v; } xf[] = {
        { "PXP_ROT90",  PXP_ROT_90,  false, false },
        { "PXP_ROT180", PXP_ROT_180, false, false },
        { "PXP_ROT270", PXP_ROT_270, false, false },
        { "PXP_HFLIP",  PXP_ROT_0,   true,  false },
        { "PXP_VFLIP",  PXP_ROT_0,   false, true  },
    };
    for (unsigned i = 0; i < sizeof(xf)/sizeof(xf[0]); i++) {
        memset(fb_buf, 0x33, sizeof(fb_buf));
        memset(ref_buf, 0x33, sizeof(ref_buf));
        ref_blit_xf(ref_buf, FB_W, 0, 0, xf[i].rot, xf[i].h, xf[i].v);
        PXPError e = PXP.op().source(src).output(fb)
                        .rotate(xf[i].rot).flip(xf[i].h, xf[i].v).run();
        if (e != PXP_OK) {
            Serial1.print(xf[i].tok); Serial1.print("=FAIL op e=");
            Serial1.println((int)e); all = false;
        } else all &= check(xf[i].tok, fb_buf, ref_buf, FB_W * FB_H);
    }

    /* --- async completion via IRQ 57 ------------------------------------ */
    static volatile bool async_done = false;
    static EventResponder er;
    er.attachImmediate([](EventResponderRef) { async_done = true; });

    memset(fb_buf, 0x77, sizeof(fb_buf));
    memset(ref_buf, 0x77, sizeof(ref_buf));
    ref_blit_xf(ref_buf, FB_W, 0, 0, PXP_ROT_0, false, false);
    async_done = false;
    if (PXP.op().source(src).output(fb).runAsync(&er) != PXP_OK) {
        Serial1.println("PXP_ASYNC=FAIL op"); all = false;
    } else {
        uint32_t t0 = millis();
        while (!async_done && (millis() - t0) < 500) { yield(); }
        if (!async_done) { Serial1.println("PXP_ASYNC=FAIL callback"); all = false; }
        else all &= check("PXP_ASYNC", fb_buf, ref_buf, FB_W * FB_H);
    }

    Serial1.println(all ? "PXP_ALL=PASS" : "PXP_ALL=FAIL");
}

void loop() {}
