/* synthui_knob_test - all four synthui_knob modes x four states on the RK055
 * (720x1280 XRGB8888, DIRECT render), checksummed PER MODE.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Phase order (all tokens before any animation -- goldens stay deterministic):
 *   1. 4x4 grid (rows=modes, cols=states): PANEL_OK, LVGL_FLUSHED, LVGL_BYTES,
 *      KNOB_SUM_ALL.  LVGL_BYTES is the flushed AREA of the FIRST refresh --
 *      720*1280*4 only if LVGL painted the whole screen (the partial-repaint
 *      guard, same contract as lvgl_rk055_panel_test).
 *   2. One screen per mode, rendered synchronously via lv_refr_now():
 *      KNOB_SUM_<MODE>.  One golden per mode is the acid-bass lesson: a single
 *      aggregate sum can freeze half the feature without changing color.
 *   3. SYNTHUI_KNOB_DONE, then a hero knob animates forever (eyes-on-glass only,
 *      never checksummed).
 */
#include <Arduino.h>
#include "Display.h"
#include "lvgl_rt1176.h"
#include "lvgl_mipi_panel.h"
#include "synthui_knob.h"

static const float               col_angle[4] = { -105.0f, -35.0f, 35.0f, 105.0f };
static const lv_state_t          col_state[4] = { LV_STATE_DEFAULT, LV_STATE_PRESSED,
                                                  LV_STATE_FOCUSED, LV_STATE_DISABLED };
static const synthui_knob_mode_t row_mode[4]  = { SYNTHUI_KNOB_MODE_ENDLESS,
                                                  SYNTHUI_KNOB_MODE_BOUNDED,
                                                  SYNTHUI_KNOB_MODE_DETENTS,
                                                  SYNTHUI_KNOB_MODE_ARC };
static const char               *mode_name[4] = { "ENDLESS", "BOUNDED", "DETENTS", "ARC" };

static lv_obj_t *hero;

static void opaque_bg(lv_obj_t *scr)
{   /* Opaque ground forces LVGL to paint every pixel: fully-defined frames. */
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
}

static lv_obj_t *make_knob(lv_obj_t *parent, synthui_knob_mode_t m,
                           lv_state_t st, float angle, int32_t size)
{
    lv_obj_t *k = synthui_knob_create(parent);
    lv_obj_set_size(k, size, size);
    synthui_knob_set_mode(k, m);
    synthui_knob_set_angle(k, angle);
    if (st != LV_STATE_DEFAULT) lv_obj_add_state(k, st);
    return k;
}

static lv_obj_t *build_grid(void)
{
    lv_obj_t *scr = lv_obj_create(NULL); opaque_bg(scr);
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "SynthUI Knob");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            lv_obj_t *k = make_knob(scr, row_mode[r], col_state[c], col_angle[c], 150);
            lv_obj_set_pos(k, 15 + c * 175, 120 + r * 175);
        }
    return scr;
}

static lv_obj_t *build_mode_screen(int m)
{
    lv_obj_t *scr = lv_obj_create(NULL); opaque_bg(scr);
    lv_obj_t *lbl = lv_label_create(scr);
    lv_label_set_text(lbl, mode_name[m]);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x9FD4FF), LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 460);
    for (int c = 0; c < 4; c++) {
        lv_obj_t *k = make_knob(scr, row_mode[m], col_state[c], col_angle[c], 150);
        lv_obj_set_pos(k, 15 + c * 175, 560);
    }
    return scr;
}

/* Load a screen, render it synchronously, checksum the whole framebuffer. */
static uint32_t sum_screen(lv_obj_t *scr)
{
    lv_screen_load(scr);
    lv_obj_invalidate(scr);
    lv_refr_now(NULL);
    lvgl_sum_reset();
    lvgl_sum_feed(Display.framebuffer(), PANEL_FB_BYTES);
    return lvgl_sum_value();
}

void setup()
{
    Serial1.begin(115200);
    delay(200);
    Serial1.println("SYNTHUI_KNOB_BEGIN");

    const bool ok = Display.begin();
    Serial1.println(ok ? "PANEL_OK" : "PANEL_FAIL");
    if (!ok) {
        /* Safe-but-only-just: with no lv_init(), lv_timer_handler() in loop()
         * returns immediately (lv_timer_run is zero from static init).  Same
         * contract as lvgl_rk055_panel_test -- see its comment before "fixing". */
        Serial1.println("SYNTHUI_KNOB_DONE");
        return;
    }
    Display.fillScreen(0x0000);

    lvgl_rt1176_begin();
    lvgl_mipi_panel_create(Display);

    /* Phase 1: the grid is the FIRST refresh, so LVGL_BYTES pins a whole-
     * screen paint (same contract as lvgl_rk055_panel_test). */
    lv_screen_load(build_grid());
    uint32_t t0 = millis();
    while (!lvgl_mipi_panel_frame_done() && (millis() - t0) < 5000)
        lvgl_rt1176_loop();
    lvgl_sum_reset();
    lvgl_sum_feed(Display.framebuffer(), PANEL_FB_BYTES);
    Serial1.printf("LVGL_FLUSHED=%s\n", lvgl_mipi_panel_frame_done() ? "PASS" : "FAIL");
    Serial1.printf("LVGL_BYTES=%lu\n",
                   (unsigned long)(lvgl_mipi_panel_flushed_px() * PANEL_BYTES_PER_PIXEL));
    Serial1.printf("KNOB_SUM_ALL=0x%08lX\n", (unsigned long)lvgl_sum_value());

    /* Phase 2: one golden per mode. */
    for (int m = 0; m < 4; m++)
        Serial1.printf("KNOB_SUM_%s=0x%08lX\n", mode_name[m],
                       (unsigned long)sum_screen(build_mode_screen(m)));

    Serial1.println("SYNTHUI_KNOB_DONE");

    /* Phase 3: hero spin, glass-only, after every token. */
    lv_obj_t *scr = lv_obj_create(NULL); opaque_bg(scr);
    hero = make_knob(scr, SYNTHUI_KNOB_MODE_ENDLESS, LV_STATE_DEFAULT, 0.0f, 360);
    lv_obj_center(hero);
    lv_screen_load(scr);
}

void loop()
{
    static uint32_t last = 0;
    static float a = 0.0f;
    const uint32_t now = millis();
    /* hero == NULL is ALSO the LVGL-uninitialised guard: on the PANEL_FAIL
     * early-return no screen exists and loop() must touch nothing. */
    if (hero && now - last >= 16) {
        last = now;
        /* wrap into [-360,360): half the cycle feeds NEGATIVE angles on
         * purpose -- glass-only coverage of the signed-angle path (Task 9's
         * eyes-on step should see continuous rotation, no jump). */
        a += 1.8f; if (a >= 360.0f) a -= 720.0f;
        synthui_knob_set_angle(hero, a);
    }
    lvgl_rt1176_loop();
}
