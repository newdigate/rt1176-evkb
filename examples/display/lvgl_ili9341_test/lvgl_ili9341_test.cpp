/* lvgl_ili9341_test - LVGL 9.4 widgets on the ILI9341 (320x240, SPI).
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * The scene is deliberately STATIC: no animation, no time-derived text, bitmap
 * fonts only.  LVGL's software renderer is deterministic, so the FNV-1a of the
 * pixels it produces is a stable golden value -- which is the only automated
 * signal available for this panel, because QEMU does not model an ILI9341.
 * The SPI transport is verified on silicon (Task 5).
 *
 * Wiring (same as the ili9341 gate): SCK=D13 MOSI=D11 MISO=D12 CS=D10 DC=D8 RST=D9.
 */
#include <Arduino.h>
#include <SPI.h>
#include <ILI9341_t3.h>
#include "lvgl_rt1176.h"
#include "lvgl_ili9341.h"

#define TFT_CS 10
#define TFT_DC  8
#define TFT_RST 9
ILI9341_t3 tft = ILI9341_t3(TFT_CS, TFT_DC, TFT_RST);

/* Partial-render buffers: 320x40 px each (25 KB), double buffered.
 * 40 rows is 1/6 of the 240-row screen, so a full refresh is 6 flushes. Bigger
 * slices mean fewer, longer writeRect bursts (less per-slice overhead, coarser
 * tearing granularity); smaller slices cost more overhead per frame. 1/6 is a
 * middle setting, not a constraint -- any row count divides the frame fine.
 * EXTMEM is deliberate rather than the default: 50 KB for the pair would fit in
 * OCRAM, but the render target is written once and blitted straight out, so it
 * gains little from fast RAM and OCRAM is better spent on hotter data. The
 * 64-byte alignment satisfies the binding's LV_DRAW_BUF_ALIGN contract with
 * cache-line headroom for a future DMA blit path. */
#define LVGL_BUF_PX (LVGL_ILI9341_W * 40)
EXTMEM __attribute__((aligned(64))) static uint16_t lv_buf1[LVGL_BUF_PX];
EXTMEM __attribute__((aligned(64))) static uint16_t lv_buf2[LVGL_BUF_PX];

static void build_scene()
{
    lv_obj_t *scr = lv_screen_active();
    /* Opaque background: forces LVGL to paint every pixel of the first refresh,
     * so the checksum covers a fully-defined frame rather than stale memory. */
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "RT1176 + LVGL");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    lv_obj_t *sub = lv_label_create(scr);
    lv_label_set_text(sub, "ILI9341 320x240");
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(sub, lv_color_hex(0x9FD4FF), LV_PART_MAIN);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 52);

    lv_obj_t *bar = lv_bar_create(scr);
    lv_obj_set_size(bar, 240, 20);
    lv_obj_align(bar, LV_ALIGN_CENTER, 0, 20);
    lv_bar_set_value(bar, 62, LV_ANIM_OFF);   /* fixed value: no animation */
}

void setup()
{
    Serial1.begin(115200);
    delay(200);
    Serial1.println("LVGL_ILI9341_BEGIN");

    tft.begin();
    tft.setRotation(1);   /* landscape: must match LVGL_ILI9341_W/H */
    tft.fillScreen(ILI9341_BLACK);

    lvgl_rt1176_begin();
    lvgl_ili9341_create(tft, lv_buf1, lv_buf2, LVGL_BUF_PX);
    build_scene();

    /* Render one full frame, checksumming what the renderer emits. */
    lvgl_sum_reset();
    uint32_t t0 = millis();
    while (!lvgl_ili9341_frame_done() && (millis() - t0) < 3000) {
        lvgl_rt1176_loop();
    }

    Serial1.printf("LVGL_FLUSHED=%s\n", lvgl_ili9341_frame_done() ? "PASS" : "FAIL");
    Serial1.printf("LVGL_BYTES=%lu\n", (unsigned long)lvgl_sum_bytes());
    Serial1.printf("LVGL_SUM=0x%08lX\n", (unsigned long)lvgl_sum_value());
    Serial1.println("LVGL_ILI9341_DONE");
}

void loop() { lvgl_rt1176_loop(); }
