/* pxp_rotate_probe - Phase 0 of the acid_box landscape design: does the PXP
 * rotate a 1280x720 XRGB8888 canvas into the RK055's 720x1280 scanout buffer
 * correctly -- at the origin AND at pointer-offset sub-rectangles -- and how
 * long does each op take?
 * Spec: docs/superpowers/specs/2026-09-14-acid-box-landscape-design.md, section 4
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * NO PANEL.  The core's startup brings the SEMC SDRAM up before setup(), so
 * EXTMEM is live without Display.begin(); the two buffers here are the canvas
 * and one scanout buffer of the design, with nothing scanning them out.
 *
 * TWO PHASES, the rotary_knob_bench pattern:
 *   A  correctness, gated in QEMU: every case prints api= (what the driver
 *      said) and pixel= (what a CPU predicate found) SEPARATELY, because a
 *      driver that returns PXP_OK while writing the wrong picture is the
 *      failure this probe exists to catch.  Ends with `crc_done`.
 *   B  timing, after crc_done, NEVER gated: QEMU time is a fiction.
 *
 * WHY POINTER-OFFSET SURFACES AND NOT outputAt(): the QEMU PXP model writes a
 * ROTATED op from the output buffer's origin and ignores OUT_PS for it (its
 * own comment: "the firmware issues no letterboxed rotated op").  A source
 * surface pointing INTO the canvas and an output surface pointing INTO the
 * scanout buffer make every op a rotation at the origin -- the path
 * pxp_blit_test has already matched on silicon (PXP_ROT90_SUM=0x54F838ED) --
 * so the model and the silicon agree BY CONSTRUCTION.  Every rect is 16-px
 * grid aligned so every derived pointer is 64-byte aligned at 4 bpp: x*4 and
 * (720-y-h)*4 are multiples of 64, and the row offsets y*5120 / x*2880 are
 * multiples of 64 regardless (5120 = 64*80, 2880 = 64*45).
 *
 * The CPU reference below is DELIBERATELY independent of the port's
 * lvgl_panel_rotation.h: the two are cross-checks, not one function twice. */
#include <Arduino.h>
#include <PXP.h>

#define CONSOLE Serial1
#define PROBE_VERSION 1

#define LOG_W  1280
#define LOG_H  720
#define PHYS_W 720
#define PHYS_H 1280
#define BPP    4
#define LOG_PITCH  (LOG_W * BPP)     /* 5120 */
#define PHYS_PITCH (PHYS_W * BPP)    /* 2880 */
#define SENTINEL 0x5A5A5A5Au

/* One spare row on each: a sub-rect surface's pitch*height can run past the
 * last row of the buffer it points into.  The driver's reachable() checks the
 * REGION, not the allocation, so this is belt-and-braces, not a fix. */
EXTMEM __attribute__((aligned(64))) static uint32_t canvas[LOG_W * (LOG_H + 1)];
EXTMEM __attribute__((aligned(64))) static uint32_t dst[PHYS_W * (PHYS_H + 1)];

static uint32_t fnv1a(const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}

/* Every canvas pixel unique in position: a misplaced or mis-strided op cannot
 * reproduce the reference by accident. */
static void fill_canvas(void)
{
    for (int y = 0; y < LOG_H; y++)
        for (int x = 0; x < LOG_W; x++)
            canvas[y * LOG_W + x] = 0xFF000000u | ((uint32_t)x << 12) | (uint32_t)y;
}

/* CW90 reference: physical (px,py) reads logical (py, 719-px).
 *
 * Masked to 0x00FFFFFF: for a 32-bit XRGB8888 OUTPUT the PXP's alpha engine
 * is unconfigured (no AS, Porter-Duff off, OUT_CTRL[ALPHA_OUTPUT]=0), so the
 * hardware writes byte 3 (the X/alpha byte) as the pipeline's COMPUTED alpha,
 * which is 0 -- silicon-measured (lvgl_pxp_copy_bench DIAG dump: source bytes
 * C3/5D/FF/19 all wrote back as 00, RGB bytes exact) and reproduced in the
 * QEMU model (imxrt_pxp.c, the `ofmt == 0x00 || ofmt == 0x04` branch).  The
 * canvas fills that byte with 0xFF so a stray copy of it would be visible;
 * the reference must expect what the hardware actually writes, not the
 * source's own byte. */
static inline uint32_t ref_px(int px, int py)
{
    return canvas[(PHYS_W - 1 - px) * LOG_W + py] & 0x00FFFFFFu;
}

static void fill_dst(void)
{
    for (size_t i = 0; i < (size_t)PHYS_W * PHYS_H; i++) dst[i] = SENTINEL;
}

/* Rotate the logical rect (x,y,w,h) into its physical place (720-y-h, x, h, w). */
static PXPError rot_rect(int x, int y, int w, int h)
{
    uint8_t *s = (uint8_t *)canvas + (size_t)y * LOG_PITCH + (size_t)x * BPP;
    const int px0 = PHYS_W - y - h, py0 = x;
    uint8_t *d = (uint8_t *)dst + (size_t)py0 * PHYS_PITCH + (size_t)px0 * BPP;
    PXPSurface src(s, (uint16_t)w, (uint16_t)h, PXP_XRGB8888, LOG_PITCH);
    PXPSurface out(d, (uint16_t)h, (uint16_t)w, PXP_XRGB8888, PHYS_PITCH);   /* the rotated extent */
    return PXP.op().source(src).output(out).rotate(PXP_ROT_90).run();
}

/* Predicate: inside the physical rect every pixel is the reference; outside it
 * every pixel is still the sentinel.  WHOLE buffer, so a stray write anywhere
 * is caught, not only one near the target. */
static bool check_rect(int x, int y, int w, int h)
{
    const int px0 = PHYS_W - y - h, px1 = PHYS_W - 1 - y, py0 = x, py1 = x + w - 1;
    for (int py = 0; py < PHYS_H; py++)
        for (int px = 0; px < PHYS_W; px++) {
            const bool in = px >= px0 && px <= px1 && py >= py0 && py <= py1;
            const uint32_t want = in ? ref_px(px, py) : SENTINEL;
            if (dst[py * PHYS_W + px] != want) return false;
        }
    return true;
}

struct Case { const char *id; int x, y, w, h; bool graded; };
static const Case kCases[] = {
    { "full",       0,    0,   LOG_W, LOG_H, true },
    { "corner-tl",  0,    0,   16,    16,    true },
    { "corner-tr",  1264, 0,   16,    16,    true },
    { "corner-bl",  0,    704, 16,    16,    true },
    { "corner-br",  1264, 704, 16,    16,    true },
    { "knob",       16,   512, 160,   160,   true },   /* the CUTOFF knob's damage, layout C */
    { "cell",       224,  96,  112,   112,   true },   /* step cell 2's */
    { "strip-top",  0,    0,   LOG_W, 16,    true },
    { "strip-left", 0,    0,   16,    LOG_H, true },
    { "centre",     640,  352, 16,    16,    true },
    { "tall",       1200, 592, 80,    128,   true },   /* w != h, both edges */
    /* P5, OBSERVATION only: pointers 32 B off the 64-B grid.  The driver treats
     * rotate-alone as alignment-free (pxp_blit_test PXP_ALIGN=off2:ok); this
     * records what silicon does at 8 px.  The gate asserts the line EXISTS. */
    { "offgrid",    8,    8,   16,    16,    false },
};
#define NCASES ((int)(sizeof(kCases) / sizeof(kCases[0])))

/* min-of-N, EXCLUDING failed reps: a timed-out rep leaves CTRL.ENABLE set, so
 * the next rep's run() returns PXP_ERR_BUSY in single-digit microseconds --
 * folding that into the min would report a fast FAILURE as the measurement.
 * *errs counts the excluded reps; if every rep fails, best stays 0xFFFFFFFF
 * and errs names why. */
static uint32_t time_rect(int x, int y, int w, int h, int reps, uint32_t *errs)
{
    uint32_t best = 0xFFFFFFFFu;
    *errs = 0;
    for (int r = 0; r < reps; r++) {
        const uint32_t t0 = micros();
        const PXPError e = rot_rect(x, y, w, h);
        const uint32_t dt = micros() - t0;
        if (e != PXP_OK) { (*errs)++; continue; }
        if (dt < best) best = dt;
    }
    return best;
}

void setup()
{
    CONSOLE.begin(115200);
    while (!CONSOLE && millis() < 2000) {}
    CONSOLE.println("=== BOOT ===");
    CONSOLE.printf("[APP: %s] [VER: v%u] [BUILD: %s %s]\n",
                   "pxp_rotate_probe", PROBE_VERSION, __DATE__, __TIME__);
    CONSOLE.println("pxp_rotate_probe up");

    if (!PXP.begin()) { CONSOLE.println("PXP_BEGIN=FAIL"); return; }
    CONSOLE.println("PXP_BEGIN=PASS");
    fill_canvas();

    int ok = 0, broken = 0;
    for (int i = 0; i < NCASES; i++) {
        const Case &c = kCases[i];
        /* case_begin BEFORE the op: an op that hangs leaves a begin with no
         * case line, and the gate counts both. */
        CONSOLE.printf("case_begin=%s\n", c.id);
        fill_dst();
        const PXPError e = rot_rect(c.x, c.y, c.w, c.h);
        /* skip: the driver errored, so the predicate never ran -- distinct
         * from broken (predicate ran and found the wrong picture).  Printing
         * broken here would read as "the rotation is wrong" when the truth
         * is "the op never happened". */
        const bool skip = (e != PXP_OK);
        const bool pix = !skip && check_rect(c.x, c.y, c.w, c.h);
        const uint32_t sum = fnv1a(dst, (size_t)PHYS_W * PHYS_H * BPP);
        CONSOLE.printf("case=%s api=", c.id);
        if (e == PXP_OK) CONSOLE.print("ok"); else { CONSOLE.print("err"); CONSOLE.print((int)e); }
        CONSOLE.printf(" pixel=%s sum=0x%08lX\n",
                       skip ? "skip" : (pix ? "ok" : "broken"), (unsigned long)sum);
        /* Tally: a graded skip counts as broken here (pix is false for both
         * skip and a failed predicate), never as ok -- a driver error must
         * not silently pass the case it prevented from being checked. */
        if (c.graded) { if (pix) ok++; else broken++; }
    }
    CONSOLE.printf("cases=%d ok=%d broken=%d\n", NCASES, ok, broken);
    CONSOLE.println("crc_done");

    /* Phase B -- silicon only.  Min of 8, icache_bench_hw's convention. */
    uint32_t errs;
    uint32_t us = time_rect(0, 0, LOG_W, LOG_H, 8, &errs);
    CONSOLE.printf("time=full min_us=%lu errs=%lu\n", (unsigned long)us, (unsigned long)errs);
    us = time_rect(16, 512, 160, 160, 8, &errs);
    CONSOLE.printf("time=rect160 min_us=%lu errs=%lu\n", (unsigned long)us, (unsigned long)errs);
    us = time_rect(640, 352, 16, 16, 8, &errs);
    CONSOLE.printf("time=rect16 min_us=%lu errs=%lu\n", (unsigned long)us, (unsigned long)errs);
    CONSOLE.println("probe_done");
}

void loop()
{
    static uint32_t n = 0;
    delay(1000);
    CONSOLE.printf("hb=%lu\n", (unsigned long)++n);
}
