/* pxp_decimate_test - RT1176 PXP M1b gate: PS pre-decimation (pixel-drop scale)
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Proves the PXP decimation path the camera pipeline uses to shrink a frame to
 * display size (640x480 /2 -> 320x240).  Decimation drops pixels (RM 52.3.1.3),
 * so it is exact: the oracle just picks source pixel (ox*f, oy*f).  Checked for
 * /2 and /4 against that independent oracle.  Serial1 (LPUART), captured by
 * run_qemu.sh.
 */
#include <Arduino.h>
#include <PXP.h>
#include <string.h>

#define SRC_W 64
#define SRC_H 48

DMAMEM static uint16_t src_buf[SRC_W * SRC_H] __attribute__((aligned(64)));
DMAMEM static uint16_t dst_buf[SRC_W * SRC_H] __attribute__((aligned(64)));  /* oversized; holds any factor */
static   uint16_t      ref_buf[SRC_W * SRC_H];

/* Position-dependent pattern: any stride/drop error changes the checksum. */
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

/* Run one decimation factor and check against the pixel-drop oracle. */
static bool run_factor(const char *name, PXPDecim d, int f)
{
    int ow = SRC_W / f, oh = SRC_H / f;
    memset(dst_buf, 0, sizeof(dst_buf));
    memset(ref_buf, 0, sizeof(ref_buf));

    PXPSurface src(src_buf, SRC_W, SRC_H, PXP_RGB565);
    PXPSurface dst(dst_buf, ow, oh, PXP_RGB565);
    PXPError e = PXP.op().source(src).output(dst).decimate(d, d).run();

    /* Oracle: dropped pixels -> dst(ox,oy) = src(ox*f, oy*f). Pack into the
     * same top-left ow x oh region of a cleared ref buffer. */
    for (int oy = 0; oy < oh; oy++)
        for (int ox = 0; ox < ow; ox++)
            ref_buf[oy * ow + ox] = src_buf[(oy * f) * SRC_W + (ox * f)];

    uint32_t got = fnv1a(dst_buf, (size_t)ow * oh * 2);
    uint32_t exp = fnv1a(ref_buf, (size_t)ow * oh * 2);
    bool ok = (e == PXP_OK) && (got == exp);
    Serial1.printf("DEC_%s=%s %dx%d->%dx%d SUM=0x%08lX EXP=0x%08lX (err=%d)\n",
                   name, ok ? "PASS" : "FAIL", SRC_W, SRC_H, ow, oh,
                   (unsigned long)got, (unsigned long)exp, (int)e);
    return ok;
}

void setup()
{
    Serial1.begin(115200);
    delay(150);
    Serial1.println("PXP_DECIMATE_BEGIN");
    Serial1.printf("PXP_BEGIN=%s\n", PXP.begin() ? "PASS" : "FAIL");

    fill_src();
    bool ok2 = run_factor("2", PXP_DEC_2, 2);
    bool ok4 = run_factor("4", PXP_DEC_4, 4);

    /* Guard: decimate + rotate must be rejected (RM 52.3.4.1). */
    PXPSurface s(src_buf, SRC_W, SRC_H, PXP_RGB565);
    PXPSurface d(dst_buf, SRC_H / 2, SRC_W / 2, PXP_RGB565);
    PXPError re = PXP.op().source(s).output(d)
                     .decimate(PXP_DEC_2, PXP_DEC_2).rotate(PXP_ROT_90).run();
    bool okg = (re == PXP_ERR_CONFIG);
    Serial1.printf("DEC_ROT_GUARD=%s (err=%d)\n", okg ? "PASS" : "FAIL", (int)re);

    Serial1.printf("PXP_DECIMATE=%s\n", (ok2 && ok4 && okg) ? "PASS" : "FAIL");
    Serial1.println("PXP_DECIMATE_END");
}

void loop() {}
