/* synthui_step_test - every synthui_step state combination on the RK055
 * (720x1280 XRGB8888, DIRECT render), checksummed.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * The widget has FIVE independent states, so the scene is a state MATRIX
 * rather than a sampling: eight column combinations (the pattern bits and the
 * cursor) x two rows (without and with `selected`) covers all sixteen
 * reachable combinations in one frame. That is deliberate -- the knob pilot's
 * lesson was that a single aggregate checksum can freeze half a feature
 * without changing colour, and here one frame's worth of pixels IS the whole
 * feature, so nothing can hide.
 *
 * Order: all tokens before anything moves, so the golden is deterministic.
 */
#include <Arduino.h>
#include "Display.h"
#include "lvgl_rt1176.h"
#include "lvgl_mipi_panel.h"
#include "synthui_step.h"

/* The eight column combinations. Rows add `selected`, giving all sixteen. */
static const struct { bool gate, accent, slide, cursor; } kCols[8] = {
    { false, false, false, false },   /* empty                       */
    { true,  false, false, false },   /* gated                       */
    { true,  true,  false, false },   /* gated + accent              */
    { true,  false, true,  false },   /* gated + slide               */
    { true,  true,  true,  false },   /* gated + accent + slide      */
    { false, false, false, true  },   /* cursor on an EMPTY step     */
    { true,  false, false, true  },   /* cursor on a gated step      */
    { true,  true,  true,  true  },   /* everything at once          */
};

static lv_obj_t *cell(lv_obj_t *scr, int col, int row)
{
    lv_obj_t *c = synthui_step_create(scr);
    lv_obj_set_size(c, 74, 74);
    lv_obj_set_pos(c, 8 + col * 88, 200 + row * 96);
    synthui_step_set(c, kCols[col].gate, kCols[col].accent, kCols[col].slide);
    synthui_step_set_cursor(c, kCols[col].cursor);
    synthui_step_set_selected(c, row == 1);
    return c;
}

static lv_obj_t *build_scene(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    /* Opaque ground so every pixel of the frame is defined and the checksum
     * means something (the same contract the sibling display gates use). */
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "SynthUI Step");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 60);

    for (int r = 0; r < 2; r++)
        for (int c = 0; c < 8; c++)
            cell(scr, c, r);
    return scr;
}

void setup()
{
    Serial1.begin(115200);
    delay(200);
    Serial1.println("SYNTHUI_STEP_BEGIN");

    const bool ok = Display.begin();
    Serial1.println(ok ? "PANEL_OK" : "PANEL_FAIL");
    if (!ok) {
        /* No lv_init() happened, so loop()'s lv_timer_handler() returns
         * immediately -- same contract as the sibling display examples. */
        Serial1.println("SYNTHUI_STEP_DONE");
        return;
    }
    Display.fillScreen(0x0000);

    lvgl_rt1176_begin();
    lvgl_mipi_panel_create(Display);

    /* The matrix is the FIRST refresh, so LVGL_BYTES pins a whole-screen
     * paint: a corner repaint and a scene edit both merely change the sum. */
    lv_screen_load(build_scene());
    uint32_t t0 = millis();
    while (!lvgl_mipi_panel_frame_done() && (millis() - t0) < 5000)
        lvgl_rt1176_loop();

    lvgl_sum_reset();
    lvgl_sum_feed(Display.framebuffer(), PANEL_FB_BYTES);
    Serial1.printf("LVGL_FLUSHED=%s\n",
                   lvgl_mipi_panel_frame_done() ? "PASS" : "FAIL");
    Serial1.printf("LVGL_BYTES=%lu\n",
                   (unsigned long)(lvgl_mipi_panel_flushed_px() *
                                   PANEL_BYTES_PER_PIXEL));
    Serial1.printf("STEP_GRID_SUM=0x%08lX\n", (unsigned long)lvgl_sum_value());
    Serial1.println("SYNTHUI_STEP_DONE");
}

void loop()
{
    lvgl_rt1176_loop();
}
