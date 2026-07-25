/* pxp_csc_color - minimal PXP CSC1 colour correctness test (no camera/CSI).
 * SPDX-License-Identifier: MIT
 *
 * Convert known UYVY pixels through PXP CSC1 -> RGB565 and print the result.
 * Isolates whether the PXP CSC produces correct colours at all:
 *   neutral gray (Y,128,128) -> gray (R~=G~=B)
 *   and the sign of the chroma terms is right.
 */
#include <Arduino.h>
#include <PXP.h>

EXTMEM __attribute__((aligned(64))) static uint32_t srcbuf[64 * 64 / 2]; /* UYVY, 2px/word */
EXTMEM __attribute__((aligned(64))) static uint16_t dstbuf[64 * 64];

struct TC { uint8_t Y, U, V; const char *name; };
static int g_R, g_G, g_B;

static void test(const TC &t)
{
    /* UYVY word packs 2 pixels: U,Y0,V,Y1 -> bytes b0=U,b1=Y0,b2=V,b3=Y1. */
    uint32_t word = (uint32_t)t.U | ((uint32_t)t.Y << 8) | ((uint32_t)t.V << 16) | ((uint32_t)t.Y << 24);
    for (uint32_t i = 0; i < 64 * 64 / 2; i++) srcbuf[i] = word;
    PXPSurface src(srcbuf, 64, 64, PXP_UYVY1P422);
    PXPSurface dst(dstbuf, 64, 64, PXP_RGB565);
    dstbuf[0] = 0xDEAD;
    PXPError e = PXP.op().source(src).output(dst).run(200);
    uint16_t p = dstbuf[0];
    g_R = ((p >> 11) & 0x1F) << 3; g_G = ((p >> 5) & 0x3F) << 2; g_B = (p & 0x1F) << 3;
    Serial1.printf("  %-14s Y=%3u U=%3u V=%3u -> R=%3d G=%3d B=%3d  (0x%04X err=%d)\n",
                   t.name, t.Y, t.U, t.V, g_R, g_G, g_B, p, (int)e);
}

void setup()
{
    Serial1.begin(115200);
    delay(200);
    Serial1.println("PXP_CSC_COLOR_BEGIN");
    Serial1.printf("PXP_BEGIN=%s\n", PXP.begin() ? "PASS" : "FAIL");

    static const TC cases[] = {
        { 0x40, 128, 128, "dark gray" },   /* -> ~ (64,64,64)   */
        { 0xC0, 128, 128, "light gray" },  /* -> ~ (192,192,192)*/
        { 0x51,  90, 240, "red-ish" },     /* YCbCr red         */
        { 0x91,  54,  34, "green-ish" },   /* YCbCr green       */
        { 0x29, 240, 110, "blue-ish" },    /* YCbCr blue        */
    };
    /* Oracle checks: gray is near-neutral & tracks luma; primaries dominate the
     * right channel.  This is the regression that catches the luma-dropping /
     * green-cast CSC1 coefficient bug. */
    bool ok = true;
    int darkY = 0, lightY = 0;
    for (uint32_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        test(cases[i]);
        int spread = 0; { int hi=g_R>g_G?g_R:g_G; hi=hi>g_B?hi:g_B; int lo=g_R<g_G?g_R:g_G; lo=lo<g_B?lo:g_B; spread=hi-lo; }
        switch (i) {
        case 0: darkY  = (g_R+2*g_G+g_B)>>2; ok &= (spread < 24); break;             /* dark gray neutral */
        case 1: lightY = (g_R+2*g_G+g_B)>>2; ok &= (spread < 24) && (lightY > darkY + 60); break; /* lighter, neutral */
        case 2: ok &= (g_R > 180 && g_G < 96 && g_B < 96); break;   /* red   */
        case 3: ok &= (g_G > 180 && g_R < 96 && g_B < 96); break;   /* green */
        case 4: ok &= (g_B > 180 && g_R < 96 && g_G < 96); break;   /* blue  */
        }
    }
    Serial1.printf("PXP_CSC_COLOR=%s\n", ok ? "PASS" : "FAIL");

    /* Read back what the CSC1 coefficient registers actually hold after an op,
     * and the PS/OUT/CTRL state, to see if our writes landed. */
    Serial1.printf("COEF0=0x%08lX COEF1=0x%08lX COEF2=0x%08lX\n",
                   (unsigned long)PXP_CSC1_COEF0, (unsigned long)PXP_CSC1_COEF1,
                   (unsigned long)PXP_CSC1_COEF2);
    Serial1.printf("PS_CTRL=0x%08lX OUT_CTRL=0x%08lX CTRL=0x%08lX PS_SCALE=0x%08lX\n",
                   (unsigned long)PXP_PS_CTRL, (unsigned long)PXP_OUT_CTRL,
                   (unsigned long)PXP_CTRL, (unsigned long)PXP_PS_SCALE);
    Serial1.println("PXP_CSC_COLOR_END");
}
void loop() {}
