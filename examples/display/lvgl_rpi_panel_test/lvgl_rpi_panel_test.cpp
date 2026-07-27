/* lvgl_rpi_panel_test - LVGL 9.4 widgets on the RPi 7" MIPI-DSI panel
 * (800x480 RGB565, DIRECT render into the live LCDIFv2 scanout buffer).
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * The scene is deliberately STATIC: no animation, no time-derived text, bitmap
 * fonts only.  LVGL's software renderer is deterministic, so the FNV-1a of the
 * pixels it produces is a stable golden value.
 *
 * What each layer proves:
 *   DISPLAY_OK    the panel chain (ATtiny + VIDEO_PLL + LCDIFv2 + MIPI-DSI +
 *                 TC358762) came up -- rpi_panel_test is the gate that pins
 *                 down the transport in detail; here it is a precondition,
 *                 because in DIRECT mode a framebuffer no display owns would
 *                 still checksum perfectly.
 *   LVGL_*        the renderer drew the expected pixels into that framebuffer.
 *
 * NOTE: uses Serial1 (the LPUART console run_qemu.sh captures via
 * `-serial file:`), not Serial (native USB CDC), like every sibling gate.
 */
#include <Arduino.h>
#include <Wire.h>   // needed by the RPiDisplay ATtiny driver (Display::begin())
#include "Display.h"
#include "lvgl_rt1176.h"
#include "lvgl_rpi_panel.h"

/* No LVGL draw buffers are declared here, on purpose: DIRECT mode renders into
 * the panel's own scanout framebuffer, which RPiDisplay already allocated in
 * SDRAM. See lvgl_rpi_panel.h. */

static void build_scene()
{
    lv_obj_t *scr = lv_screen_active();
    /* Opaque background: forces LVGL to paint every pixel of the first refresh,
     * so the checksum covers a fully-defined frame rather than stale memory.
     * That matters more here than for a partial-mode panel -- an unpainted
     * region would leave whatever fillScreen() put there, which is defined, but
     * would mean the checksum no longer covers only the renderer's work. */
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "RT1176 + LVGL");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t *sub = lv_label_create(scr);
    lv_label_set_text(sub, "Raspberry Pi 7\" 800x480 MIPI-DSI");
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(sub, lv_color_hex(0x9FD4FF), LV_PART_MAIN);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 92);

    lv_obj_t *bar = lv_bar_create(scr);
    lv_obj_set_size(bar, 520, 32);
    lv_obj_align(bar, LV_ALIGN_CENTER, 0, 40);
    lv_bar_set_value(bar, 62, LV_ANIM_OFF);   /* fixed value: no animation */
}

void setup()
{
    Serial1.begin(115200);
    delay(200);
    Serial1.println("LVGL_RPI_PANEL_BEGIN");

    const bool ok = Display.begin();
    Serial1.println(ok ? "DISPLAY_OK" : "DISPLAY_FAIL");
    if (!ok) {
        /* Bail: framebuffer()/geometry are only meaningful after a successful
         * begin(), and handing LVGL a null buffer would fault rather than fail
         * a token (lvgl_rpi_panel_create() asserts on exactly that).
         *
         * This path leaves LVGL uninitialised while loop() below still calls
         * lv_timer_handler() every pass. That is SAFE, but only just: with no
         * lv_init(), LVGL's lv_timer_run flag is still zero from static
         * zero-initialisation, so lv_timer_handler() returns immediately
         * without touching any of the state lv_init() would have built. Spelled
         * out because it reads like a bug and the next person should not
         * "fix" it by guessing -- if that guarantee ever changes, the fix is a
         * flag here, not an lv_init() on a display that failed to come up. */
        Serial1.println("LVGL_RPI_PANEL_DONE");
        return;
    }

    /* Defined starting state, so any pixel LVGL leaves unpainted is a known
     * black rather than whatever SDRAM held at reset. */
    Display.fillScreen(0x0000);

    lvgl_rt1176_begin();
    lvgl_rpi_panel_create(Display);
    build_scene();

    /* Render one full frame. */
    uint32_t t0 = millis();
    while (!lvgl_rpi_panel_frame_done() && (millis() - t0) < 5000) {
        lvgl_rt1176_loop();
    }

    /* ORDERING DIFFERS FROM lvgl_ili9341_test ON PURPOSE -- do not "fix" it
     * into that shape. There, partial mode delivers the frame as a sequence of
     * slices and flush_cb accumulates the sum across them, so the reset must
     * come BEFORE the render loop. Here, DIRECT mode may hand flush_cb only the
     * dirty area, so per-flush hashing would cover less than a frame; instead
     * the whole finished framebuffer is hashed in one go AFTER the refresh --
     * which is also exactly what rpi_panel_test's FB_SUM does. */
    lvgl_sum_reset();
    lvgl_sum_feed(Display.framebuffer(), PANEL_FB_BYTES);

    Serial1.printf("LVGL_FLUSHED=%s\n", lvgl_rpi_panel_frame_done() ? "PASS" : "FAIL");
    /* LVGL_BYTES is the total AREA LVGL flushed, not the extent of the feed
     * above -- and that difference is the whole point. The feed is one
     * unconditional call of PANEL_FB_BYTES, so printing lvgl_sum_bytes() here
     * would be a constant restating a literal: a check with no reachable
     * failure. The flushed area can fall short (LVGL invalidating only part of
     * the screen), and that is the one regression the golden checksum cannot
     * name -- a partial repaint and an intentional scene edit both just print a
     * different LVGL_SUM. Equal to PANEL_FB_BYTES only if LVGL really redrew
     * all 800x480. */
    Serial1.printf("LVGL_BYTES=%lu\n",
                   (unsigned long)(lvgl_rpi_panel_flushed_px() * PANEL_BYTES_PER_PIXEL));
    Serial1.printf("LVGL_SUM=0x%08lX\n", (unsigned long)lvgl_sum_value());
    Serial1.println("LVGL_RPI_PANEL_DONE");
}

/* Kept running deliberately, so the scene stays live on the panel for the
 * hardware pass (Task 5) instead of the gate ending with a frozen display.
 *
 * Unlike the ILI9341 sibling, a lv_timer_handler() here writes into a buffer
 * the LCDIFv2 is actively scanning out, so "just keep calling it" deserves the
 * justification: the scene is static, nothing re-invalidates any area, and so
 * no further rendering actually happens -- the handler only services timers.
 * Adding anything animated to build_scene() would make this a continuous
 * tearing writer into live scanout AND break the golden checksum, which is why
 * the scene is static for both reasons at once. */
void loop() { lvgl_rt1176_loop(); }
