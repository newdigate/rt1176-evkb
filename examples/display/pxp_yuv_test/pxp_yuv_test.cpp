/* pxp_yuv_test - RT1176 PXP M1a gate: UYVY422 -> RGB565 via CSC1
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Proves the PXP colour-space path the camera pipeline needs: a UYVY1P422
 * processed surface, converted to RGB565 by CSC1 (the YUV->RGB matrix).  The
 * check is a closed loop against an INDEPENDENT software oracle that recomputes
 * every pixel from first principles using the SAME coefficient registers the
 * driver programs (PXP_CSC1_COEF*_YUV2RGB) and the SAME integer formula the RM
 * documents.  So a wrong coefficient, a wrong field-extract, or a wrong QEMU
 * CSC model all surface as CSC_SUM != EXP, on QEMU and on silicon alike.
 *
 * Serial1 (LPUART, captured by run_qemu.sh), not native-USB Serial.
 */
#include <Arduino.h>
#include <PXP.h>
#include <string.h>

#define W 16          /* even: UYVY packs 2 px / 4 bytes */
#define H 12

/* UYVY source: 2 bytes/pixel, row = W*2 bytes.  PXP is a bus master -> OCRAM
 * (DMAMEM), never DTCM as a matter of habit (DTCM is reachable per the PXP
 * notes, but OCRAM is the plain case). 64B aligned like the sibling gate. */
DMAMEM static uint8_t  uyvy_buf[W * H * 2] __attribute__((aligned(64)));
DMAMEM static uint16_t rgb_buf[W * H]      __attribute__((aligned(64)));
static   uint16_t      ref_buf[W * H];     /* software oracle (DTCM ok) */

/* Deterministic Y/U/V so every channel and both macropixel halves vary. */
static inline uint8_t genY(int x, int y) { return (uint8_t)(16 + ((x * 13 + y * 7) & 0xDF)); }
static inline uint8_t genU(int x, int y) { return (uint8_t)((x * 9  + y * 3) & 0xFF); }
static inline uint8_t genV(int x, int y) { return (uint8_t)((x * 5  + y * 11) & 0xFF); }

static uint32_t fnv1a(const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    uint32_t h = 2166136261u;
    while (n--) { h ^= *b++; h *= 16777619u; }
    return h;
}

/* Two's-complement sign-extend the low `bits` of v. */
static int32_t sext(uint32_t v, unsigned bits)
{
    uint32_t m = 1u << (bits - 1);
    return (int32_t)((v ^ m) - m);
}
static inline uint8_t clamp8(int32_t v) { return v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)v); }

/* Independent CSC oracle: extracts C0..C4 + offsets from the SAME coefficient
 * words the driver writes, and applies the RM formula in integer math. */
static uint16_t oracle_rgb565(uint8_t Y, uint8_t U, uint8_t V)
{
    const uint32_t c0w = PXP_CSC1_COEF0_YUV2RGB;
    const uint32_t c1w = PXP_CSC1_COEF1_YUV2RGB;
    const uint32_t c2w = PXP_CSC1_COEF2_YUV2RGB;
    int32_t c0   = sext((c0w >> 18) & 0x7FF, 11);
    int32_t uoff = sext((c0w >> 9)  & 0x1FF, 9);
    int32_t yoff = sext( c0w        & 0x1FF, 9);
    int32_t c1   = sext((c1w >> 16) & 0x7FF, 11);
    int32_t c4   = sext( c1w        & 0x7FF, 11);
    int32_t c2   = sext((c2w >> 16) & 0x7FF, 11);
    int32_t c3   = sext( c2w        & 0x7FF, 11);

    int32_t yt = c0 * ((int32_t)Y + yoff);
    int32_t uu = (int32_t)U + uoff;
    int32_t vv = (int32_t)V + uoff;
    uint8_t r = clamp8((yt + c1 * vv) >> 8);
    uint8_t g = clamp8((yt + c3 * uu + c2 * vv) >> 8);
    uint8_t b = clamp8((yt + c4 * uu) >> 8);
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static void fill_source(void)
{
    for (int y = 0; y < H; y++) {
        for (int mx = 0; mx < W; mx += 2) {
            uint8_t *mp = &uyvy_buf[(y * W + mx) * 2];   /* [U Y0 V Y1] */
            mp[0] = genU(mx, y);          /* U shared by both pixels    */
            mp[1] = genY(mx, y);          /* Y0                          */
            mp[2] = genV(mx, y);          /* V shared                    */
            mp[3] = genY(mx + 1, y);      /* Y1                          */
            /* oracle: pixel mx uses Y0,U,V; pixel mx+1 uses Y1,U,V */
            ref_buf[y * W + mx]     = oracle_rgb565(mp[1], mp[0], mp[2]);
            ref_buf[y * W + mx + 1] = oracle_rgb565(mp[3], mp[0], mp[2]);
        }
    }
}

void setup()
{
    Serial1.begin(115200);
    delay(150);
    Serial1.println("PXP_YUV_BEGIN");

    Serial1.printf("PXP_BEGIN=%s\n", PXP.begin() ? "PASS" : "FAIL");

    fill_source();
    memset(rgb_buf, 0, sizeof(rgb_buf));

    PXPSurface src(uyvy_buf, W, H, PXP_UYVY1P422);
    PXPSurface dst(rgb_buf,  W, H, PXP_RGB565);
    PXPError e = PXP.op().source(src).output(dst).run();
    Serial1.printf("PXP_CSC_RUN=%s (err=%d)\n", e == PXP_OK ? "PASS" : "FAIL", (int)e);

    uint32_t got = fnv1a(rgb_buf, sizeof(rgb_buf));
    uint32_t exp = fnv1a(ref_buf, sizeof(ref_buf));
    Serial1.printf("CSC_SUM=0x%08lX EXP=0x%08lX %s\n",
                   (unsigned long)got, (unsigned long)exp,
                   got == exp ? "PASS" : "FAIL");

    /* A couple of concrete pixels, so a failure is legible not just a hash. */
    Serial1.printf("px[0,0]=0x%04X ref=0x%04X  px[1,0]=0x%04X ref=0x%04X\n",
                   rgb_buf[0], ref_buf[0], rgb_buf[1], ref_buf[1]);

    Serial1.printf("PXP_YUV=%s\n",
                   (e == PXP_OK && got == exp) ? "PASS" : "FAIL");
    Serial1.println("PXP_YUV_END");
}

void loop() {}
