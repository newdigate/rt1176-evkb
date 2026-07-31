/* lvgl_rk055_panel_test - LVGL 9.4 widgets on the RK055HDMIPI4MA0
 * (720x1280 portrait RGB565, DIRECT render into the live LCDIFv2 scanout
 * buffer). Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * The scene is deliberately STATIC: no animation, no time-derived text, bitmap
 * fonts only.  LVGL's software renderer is deterministic, so the FNV-1a of the
 * pixels it produces is a stable golden value.
 *
 * UNLIKE lvgl_rpi_panel_test, whose golden pins REPRODUCIBILITY only (that
 * panel is disconnected and hardware-blocked), THIS golden is confirmed by a
 * human eye on real glass -- the first MIPI-DSI LVGL golden in the tree with
 * that property.  See transcript_hw_evkb.txt.
 *
 * What each layer proves:
 *   PANEL_OK      the RK055 chain (PLL_528 roots + LCDIFv2 + MIPI-DSI +
 *                 HX8394) came up -- rk055_panel_test pins the transport in
 *                 detail; here it is a precondition, because in DIRECT mode a
 *                 framebuffer no display owns would still checksum perfectly.
 *   LVGL_*        the renderer drew the expected pixels into that framebuffer.
 *
 * NOTE: uses Serial1 (the LPUART console run_qemu.sh captures via
 * `-serial file:`), not Serial (native USB CDC), like every sibling gate.
 */
#include <Arduino.h>
#include "Display.h"
#include "lvgl_rt1176.h"
#include "lvgl_mipi_panel.h"

/* No LVGL draw buffers are declared here, on purpose: DIRECT mode renders into
 * the panel's own scanout framebuffer, which MipiDisplay already allocated.
 * See lvgl_mipi_panel.h. */

static void build_scene()
{
    lv_obj_t *scr = lv_screen_active();
    /* Opaque background: forces LVGL to paint every pixel of the first
     * refresh, so the checksum covers a fully-defined frame. */
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "RT1176 + LVGL");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 80);

    lv_obj_t *sub = lv_label_create(scr);
    lv_label_set_text(sub, "RK055HDMIPI4MA0 720x1280 MIPI-DSI");
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(sub, lv_color_hex(0x9FD4FF), LV_PART_MAIN);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 132);

    lv_obj_t *bar = lv_bar_create(scr);
    lv_obj_set_size(bar, 520, 32);
    lv_obj_align(bar, LV_ALIGN_CENTER, 0, 40);
    lv_bar_set_value(bar, 62, LV_ANIM_OFF);   /* fixed value: no animation */

    /* v7 depth evidence: a full-width dark-to-light grey ramp.  Grey is the
     * harshest banding case -- at RGB565 the 5-bit channels step ~31 times
     * across 720 px; at XRGB8888 the ramp is smooth.  The DEPTH_DEMO_565
     * variant builds THIS SAME code at 16 bpp for the before-eye. */
    lv_obj_t *grad = lv_obj_create(scr);
    lv_obj_remove_style_all(grad);
    lv_obj_set_size(grad, 720, 120);
    lv_obj_align(grad, LV_ALIGN_BOTTOM_MID, 0, -60);
    lv_obj_set_style_bg_opa(grad, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(grad, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(grad, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(grad, LV_GRAD_DIR_HOR, LV_PART_MAIN);
}

void setup()
{
    Serial1.begin(115200);
    delay(200);
    Serial1.println("LVGL_RK055_PANEL_BEGIN");

    const bool ok = Display.begin();
    Serial1.println(ok ? "PANEL_OK" : "PANEL_FAIL");
    if (!ok) {
        /* Safe-but-only-just: with no lv_init(), lv_timer_handler() in loop()
         * returns immediately (lv_timer_run is zero from static init).  Same
         * contract as lvgl_rpi_panel_test -- see its comment before "fixing". */
        Serial1.println("LVGL_RK055_PANEL_DONE");
        return;
    }

    /* Defined starting state for any pixel LVGL leaves unpainted. */
    Display.fillScreen(0x0000);

    lvgl_rt1176_begin();
    lvgl_mipi_panel_create(Display);
    build_scene();

    /* Render one full frame. */
    uint32_t t0 = millis();
    while (!lvgl_mipi_panel_frame_done() && (millis() - t0) < 5000) {
        lvgl_rt1176_loop();
    }

    /* DIRECT mode: hash the finished framebuffer in one go AFTER the refresh
     * (per-flush hashing would cover only dirty areas).  Same ordering as
     * lvgl_rpi_panel_test -- see its comment. */
    lvgl_sum_reset();
    lvgl_sum_feed(Display.framebuffer(), PANEL_FB_BYTES);

    Serial1.printf("LVGL_FLUSHED=%s\n", lvgl_mipi_panel_frame_done() ? "PASS" : "FAIL");
    /* LVGL_BYTES is the flushed AREA (x2 for bytes), not the feed extent --
     * 720*1280*2 = 1843200 only if LVGL redrew the whole screen.  The feed is
     * one unconditional PANEL_FB_BYTES call; printing lvgl_sum_bytes() would
     * assert a literal against itself. */
    Serial1.printf("LVGL_BYTES=%lu\n",
                   (unsigned long)(lvgl_mipi_panel_flushed_px() * PANEL_BYTES_PER_PIXEL));
    Serial1.printf("LVGL_SUM=0x%08lX\n", (unsigned long)lvgl_sum_value());

    /* v7: the doubled-scanout-bandwidth watchdog.  Vacuous in QEMU (the model
     * never underruns) -- the hardware transcript is where 0/10 is evidence. */
    Display.sampleUnderrun(10);
    Serial1.printf("UNDERRUNS=%lu/%lu\n",
                   (unsigned long)Display.underrunCount(),
                   (unsigned long)Display.underrunFrames());
    Serial1.println("LVGL_RK055_PANEL_DONE");
}

/* Static scene: nothing re-invalidates, so the handler only services timers.
 * Keeps the scene live on the glass for the hardware confirmation. */
void loop() { lvgl_rt1176_loop(); }
