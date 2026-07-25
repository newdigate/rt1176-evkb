/* camera_preview_synth - RT1176 M1c gate: the camera BACK-HALF, camera-free.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Runs the exact pipeline the OV5640 preview will use, but fed by a synthetic,
 * animated 640x480 UYVY frame instead of the camera:
 *
 *   synth UYVY 640x480 (SDRAM)
 *      -> PXP pass 1: CSC1 YUV->RGB565            -> RGB565 640x480 (SDRAM)
 *      -> PXP pass 2: decimate /2 (pixel-drop)    -> RGB565 320x240 (SDRAM)
 *      -> ILI9341 writeRect, setRotation(1) landscape 320x240
 *
 * Two passes because the PXP cannot decimate a YUV source in one op (the RM's
 * YUV422 chroma adjustment); CSC first, then decimate the RGB result.
 * 640x480 /2 == 320x240 == the ILI9341 in landscape, so the fit is exact - no
 * bilinear, crop or letterbox.
 *
 * QEMU gate: checks the first frame's 320x240 output against a from-first-
 * principles oracle (CSC of the even/even-subsampled source pixels), printed
 * BEFORE the slow SPI push so the token does not wait on writeRect.  On QEMU the
 * ILI9341 is unmodelled (SPI just drains); the real oracle for the moving
 * picture is your eyes on the glass.
 *
 * Wiring (same as ili9341_t3_gate): SCK=D13 MOSI=D11 MISO=D12 CS=D10 DC=D8 RST=D9.
 */
#include <Arduino.h>
#include <SPI.h>
#include <ILI9341_t3.h>
#include <PXP.h>
#include <string.h>

#define CAM_W 640
#define CAM_H 480
#define OUT_W 320       /* CAM_W / 2 */
#define OUT_H 240       /* CAM_H / 2 */

#define TFT_CS 10
#define TFT_DC  8
#define TFT_RST 9
ILI9341_t3 tft = ILI9341_t3(TFT_CS, TFT_DC, TFT_RST);

/* Frame buffers live in SDRAM (SEMC) - too big for OCRAM, and the PXP reaches
 * SDRAM as a bus master (HW-verified in pxp_blit_test).  64B aligned. */
EXTMEM __attribute__((aligned(64))) static uint8_t  uyvy [CAM_W * CAM_H * 2];
EXTMEM __attribute__((aligned(64))) static uint16_t rgb640[CAM_W * CAM_H];
EXTMEM __attribute__((aligned(64))) static uint16_t rgb320[OUT_W * OUT_H];

static uint32_t fnv1a(const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    uint32_t h = 2166136261u;
    while (n--) { h ^= *b++; h *= 16777619u; }
    return h;
}

/* Synthetic UYVY frame: a scrolling colour field with luma texture, so a still
 * looks like "something" and successive frames visibly move.  Deterministic in
 * t, so frame 0 is checksummable. */
static void gen_frame(uint32_t t)
{
    for (int y = 0; y < CAM_H; y++) {
        uint8_t *row = &uyvy[(size_t)y * CAM_W * 2];
        for (int mx = 0; mx < CAM_W; mx += 2) {
            uint8_t U  = (uint8_t)((mx + t * 3) & 0xFF);
            uint8_t V  = (uint8_t)((y  + t * 2) & 0xFF);
            uint8_t Y0 = (uint8_t)(48 + (((mx ^ y) + t) & 0x7F));
            uint8_t Y1 = (uint8_t)(48 + ((((mx + 1) ^ y) + t) & 0x7F));
            uint8_t *mp = &row[mx * 2];
            mp[0] = U; mp[1] = Y0; mp[2] = V; mp[3] = Y1;
        }
    }
}

/* Two's-complement helpers + the same CSC as the driver/model, for the oracle. */
static int32_t sext(uint32_t v, unsigned bits) { uint32_t m = 1u << (bits-1); return (int32_t)((v ^ m) - m); }
static inline uint8_t clamp8(int32_t v) { return v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)v); }
static uint16_t oracle_csc565(uint8_t Y, uint8_t U, uint8_t V)
{
    const uint32_t c0w = PXP_CSC1_COEF0_YUV2RGB, c1w = PXP_CSC1_COEF1_YUV2RGB, c2w = PXP_CSC1_COEF2_YUV2RGB;
    int32_t c0 = sext((c0w>>18)&0x7FF,11), uoff = sext((c0w>>9)&0x1FF,9), yoff = sext(c0w&0x1FF,9);
    int32_t c1 = sext((c1w>>16)&0x7FF,11), c4 = sext(c1w&0x7FF,11);
    int32_t c2 = sext((c2w>>16)&0x7FF,11), c3 = sext(c2w&0x7FF,11);
    int32_t yt = c0*((int32_t)Y+yoff), uu = (int32_t)U+uoff, vv = (int32_t)V+uoff;
    uint8_t r = clamp8((yt + c1*vv)>>8), g = clamp8((yt + c3*uu + c2*vv)>>8), b = clamp8((yt + c4*uu)>>8);
    return (uint16_t)(((r>>3)<<11) | ((g>>2)<<5) | (b>>3));
}

/* Run both PXP passes for the current uyvy frame into rgb320. */
static PXPError run_pipeline(void)
{
    PXPSurface src(uyvy,   CAM_W, CAM_H, PXP_UYVY1P422);
    PXPSurface mid(rgb640, CAM_W, CAM_H, PXP_RGB565);
    PXPError e = PXP.op().source(src).output(mid).run(200);   /* CSC */
    if (e != PXP_OK) return e;
    PXPSurface dst(rgb320, OUT_W, OUT_H, PXP_RGB565);
    return PXP.op().source(mid).output(dst).decimate(PXP_DEC_2, PXP_DEC_2).run(200); /* /2 */
}

void setup()
{
    Serial1.begin(115200);
    delay(150);
    Serial1.println("CAM_SYNTH_BEGIN");

    Serial1.printf("PXP_BEGIN=%s\n", PXP.begin() ? "PASS" : "FAIL");
    tft.begin();
    tft.setRotation(1);                 /* landscape: 320 wide x 240 tall */
    tft.fillScreen(ILI9341_BLACK);

    gen_frame(0);
    PXPError e = run_pipeline();
    Serial1.printf("PIPE_RUN=%s (err=%d)\n", e == PXP_OK ? "PASS" : "FAIL", (int)e);

    /* Oracle: rgb320(ox,oy) = CSC( even/even source pixel (2ox,2oy) ). */
    uint32_t exp = 2166136261u;
    for (int oy = 0; oy < OUT_H; oy++) {
        for (int ox = 0; ox < OUT_W; ox++) {
            uint8_t *mp = &uyvy[((size_t)(oy*2) * CAM_W + (ox*2)) * 2]; /* even col -> Y0,U,V */
            uint16_t px = oracle_csc565(mp[1], mp[0], mp[2]);
            exp = (exp ^ (px & 0xFF)) * 16777619u;
            exp = (exp ^ (px >> 8))  * 16777619u;
        }
    }
    uint32_t got = fnv1a(rgb320, sizeof(rgb320));
    Serial1.printf("PIPE_SUM=0x%08lX EXP=0x%08lX %s\n",
                   (unsigned long)got, (unsigned long)exp, got == exp ? "PASS" : "FAIL");
    Serial1.printf("CAM_SYNTH=%s\n", (e == PXP_OK && got == exp) ? "PASS" : "FAIL");

    tft.writeRect(0, 0, OUT_W, OUT_H, rgb320);   /* first frame to glass (slow) */
    Serial1.println("CAM_SYNTH_END");
}

void loop()
{
    static uint32_t t = 1;
    gen_frame(t++);
    if (run_pipeline() == PXP_OK)
        tft.writeRect(0, 0, OUT_W, OUT_H, rgb320);   /* animate on HW */
}
