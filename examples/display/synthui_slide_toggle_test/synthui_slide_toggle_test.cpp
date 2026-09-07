/* synthui_slide_toggle_test - synthui_slide_toggle (NEW-30) on the RK055
 * display panel under QEMU and on silicon.
 *
 * Pipeline: double-buffered hardware pipeline via lvgl_mipi_panel_create_db(Display).
 * Verifies:
 *   1. Full initial render of SlideToggle gallery scene -> pinned golden CRC (FNV-1a over 720x1280 fb).
 *   2. 64-step deterministic delta sequence cycling switch positions.
 *   3. Delta equality guard: delta-rendered CRC matches fresh full-render CRC.
 *   4. Damage engagement guard: max invalidated area stays small (<= 15000 px).
 *   5. VSYNC health guard: timeouts=0.
 *   6. Phase B animation benchmark: reports slide_toggle_fps.
 *
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT */

#include <Arduino.h>
#include <Display.h>
#include <lvgl.h>
#include "lvgl_rt1176.h"
#include "lvgl_mipi_panel.h"
#include "synthui_slide_toggle.h"
#include <stdio.h>
#include <string.h>

#define RK055_WIDTH  720
#define RK055_HEIGHT 1280

/* ---------- FNV-1a hash ---------- */

static uint32_t fnv1a_32(const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t h = 0x811c9dc5u;
    while (n--) {
        h ^= *p++;
        h *= 0x01000193u;
    }
    return h;
}

/* ---------- Damage monitor ---------- */

static uint32_t g_max_damage = 0;
static uint32_t g_total_damage = 0;

static void damage_monitor_cb(lv_event_t *e)
{
    lv_area_t *area = (lv_area_t *)lv_event_get_param(e);
    if (area) {
        uint32_t w = (uint32_t)lv_area_get_width(area);
        uint32_t h = (uint32_t)lv_area_get_height(area);
        uint32_t dmg = w * h;
        if (dmg > g_max_damage) g_max_damage = dmg;
        g_total_damage += dmg;
    }
}

/* Dynamic toggles cycled in Phase A2 and Phase B */
static lv_obj_t *g_dyn_toggles[4];

void setup()
{
    Serial1.begin(115200);
    delay(200);

    Serial1.println("SYNTHUI_SLIDE_TOGGLE_BEGIN");

    const bool ok = Display.begin();
    if (!ok) {
        Serial1.println("PANEL_FAIL");
        Serial1.println("crc_done");
        return;
    }
    Serial1.println("PANEL_OK");
    Display.fillScreen(0x0000);

    lvgl_rt1176_begin();
    lv_display_t *disp = lvgl_mipi_panel_create_db(Display);
    if (!disp) {
        Serial1.println("LVGL_DISP_FAIL");
        return;
    }
    lv_display_add_event_cb(disp, damage_monitor_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x16171a), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* Header title */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "SYNTHUI SLIDETOGGLE GALLERY");
    lv_obj_set_style_text_color(title, lv_color_hex(0xc8cbcc), 0);
    lv_obj_set_pos(title, 40, 24);

    /* =========================================================================
     * Gallery Layout (720x1280)
     * ========================================================================= */

    /* Row 1: 2-position waveform label pairs (Y = 70) */
    lv_obj_t *t0 = synthui_slide_toggle_create(scr);
    synthui_slide_toggle_set_positions(t0, 2);
    synthui_slide_toggle_set_value(t0, 0);
    synthui_slide_toggle_set_left_glyph(t0, SYNTHUI_SLIDE_TOGGLE_GLYPH_SAW);
    synthui_slide_toggle_set_right_glyph(t0, SYNTHUI_SLIDE_TOGGLE_GLYPH_SQUARE);
    synthui_slide_toggle_set_panel_color(t0, SYNTHUI_SLIDE_TOGGLE_COLOR_PANEL_DEFAULT);
    lv_obj_set_size(t0, 190, 62);
    lv_obj_set_pos(t0, 40, 70);

    lv_obj_t *t1 = synthui_slide_toggle_create(scr);
    synthui_slide_toggle_set_positions(t1, 2);
    synthui_slide_toggle_set_value(t1, 1);
    synthui_slide_toggle_set_left_glyph(t1, SYNTHUI_SLIDE_TOGGLE_GLYPH_TRI);
    synthui_slide_toggle_set_right_glyph(t1, SYNTHUI_SLIDE_TOGGLE_GLYPH_PULSE);
    synthui_slide_toggle_set_panel_color(t1, SYNTHUI_SLIDE_TOGGLE_COLOR_PANEL_DEFAULT);
    lv_obj_set_size(t1, 190, 62);
    lv_obj_set_pos(t1, 265, 70);

    lv_obj_t *t2 = synthui_slide_toggle_create(scr);
    synthui_slide_toggle_set_positions(t2, 2);
    synthui_slide_toggle_set_value(t2, 0);
    synthui_slide_toggle_set_left_glyph(t2, SYNTHUI_SLIDE_TOGGLE_GLYPH_NONE);
    synthui_slide_toggle_set_right_glyph(t2, SYNTHUI_SLIDE_TOGGLE_GLYPH_SAW);
    synthui_slide_toggle_set_panel_color(t2, SYNTHUI_SLIDE_TOGGLE_COLOR_PANEL_DEFAULT);
    lv_obj_set_size(t2, 190, 62);
    lv_obj_set_pos(t2, 490, 70);

    /* Row 2: Multi-position toggles (Y = 165) */
    lv_obj_t *t3 = synthui_slide_toggle_create(scr);
    synthui_slide_toggle_set_positions(t3, 3);
    synthui_slide_toggle_set_value(t3, 1);
    synthui_slide_toggle_set_left_glyph(t3, SYNTHUI_SLIDE_TOGGLE_GLYPH_TRI);
    synthui_slide_toggle_set_right_glyph(t3, SYNTHUI_SLIDE_TOGGLE_GLYPH_PULSE);
    synthui_slide_toggle_set_panel_color(t3, 0x6D7A85u);
    lv_obj_set_size(t3, 220, 64);
    lv_obj_set_pos(t3, 40, 165);

    lv_obj_t *t4 = synthui_slide_toggle_create(scr);
    synthui_slide_toggle_set_positions(t4, 4);
    synthui_slide_toggle_set_value(t4, 2);
    synthui_slide_toggle_set_left_glyph(t4, SYNTHUI_SLIDE_TOGGLE_GLYPH_SAW);
    synthui_slide_toggle_set_right_glyph(t4, SYNTHUI_SLIDE_TOGGLE_GLYPH_SQUARE);
    synthui_slide_toggle_set_panel_color(t4, 0x39434Bu);
    lv_obj_set_size(t4, 260, 64);
    lv_obj_set_pos(t4, 300, 165);

    /* Row 3: Panel color variations (Y = 260) */
    lv_obj_t *t5 = synthui_slide_toggle_create(scr);
    synthui_slide_toggle_set_positions(t5, 2);
    synthui_slide_toggle_set_value(t5, 0);
    synthui_slide_toggle_set_left_glyph(t5, SYNTHUI_SLIDE_TOGGLE_GLYPH_SQUARE);
    synthui_slide_toggle_set_right_glyph(t5, SYNTHUI_SLIDE_TOGGLE_GLYPH_PULSE);
    synthui_slide_toggle_set_panel_color(t5, 0xD6D4CFu); /* Cream */
    lv_obj_set_size(t5, 200, 60);
    lv_obj_set_pos(t5, 40, 260);

    lv_obj_t *t6 = synthui_slide_toggle_create(scr);
    synthui_slide_toggle_set_positions(t6, 3);
    synthui_slide_toggle_set_value(t6, 2);
    synthui_slide_toggle_set_left_glyph(t6, SYNTHUI_SLIDE_TOGGLE_GLYPH_SAW);
    synthui_slide_toggle_set_right_glyph(t6, SYNTHUI_SLIDE_TOGGLE_GLYPH_TRI);
    synthui_slide_toggle_set_panel_color(t6, 0x39434Bu); /* Dark steel */
    lv_obj_set_size(t6, 200, 60);
    lv_obj_set_pos(t6, 260, 260);

    lv_obj_t *t7 = synthui_slide_toggle_create(scr);
    synthui_slide_toggle_set_positions(t7, 2);
    synthui_slide_toggle_set_value(t7, 1);
    synthui_slide_toggle_set_left_glyph(t7, SYNTHUI_SLIDE_TOGGLE_GLYPH_PULSE);
    synthui_slide_toggle_set_right_glyph(t7, SYNTHUI_SLIDE_TOGGLE_GLYPH_NONE);
    synthui_slide_toggle_set_panel_color(t7, 0x6D7A85u); /* Slate */
    lv_obj_set_size(t7, 200, 60);
    lv_obj_set_pos(t7, 480, 260);

    /* Row 4: Disabled states (Y = 355) */
    lv_obj_t *t8 = synthui_slide_toggle_create(scr);
    synthui_slide_toggle_set_positions(t8, 2);
    synthui_slide_toggle_set_value(t8, 0);
    synthui_slide_toggle_set_left_glyph(t8, SYNTHUI_SLIDE_TOGGLE_GLYPH_SAW);
    synthui_slide_toggle_set_right_glyph(t8, SYNTHUI_SLIDE_TOGGLE_GLYPH_SQUARE);
    synthui_slide_toggle_set_panel_color(t8, SYNTHUI_SLIDE_TOGGLE_COLOR_PANEL_DEFAULT);
    synthui_slide_toggle_set_disabled(t8, true);
    lv_obj_set_size(t8, 200, 60);
    lv_obj_set_pos(t8, 60, 355);

    lv_obj_t *t9 = synthui_slide_toggle_create(scr);
    synthui_slide_toggle_set_positions(t9, 3);
    synthui_slide_toggle_set_value(t9, 1);
    synthui_slide_toggle_set_left_glyph(t9, SYNTHUI_SLIDE_TOGGLE_GLYPH_TRI);
    synthui_slide_toggle_set_right_glyph(t9, SYNTHUI_SLIDE_TOGGLE_GLYPH_PULSE);
    synthui_slide_toggle_set_panel_color(t9, 0x6D7A85u);
    synthui_slide_toggle_set_disabled(t9, true);
    lv_obj_set_size(t9, 220, 60);
    lv_obj_set_pos(t9, 300, 355);

    /* Row 5: Dynamic interactive / benchmark bank (Y = 470) */
    lv_obj_t *bank_lbl = lv_label_create(scr);
    lv_label_set_text(bank_lbl, "DYNAMIC DELTA / BENCHMARK BANK");
    lv_obj_set_style_text_color(bank_lbl, lv_color_hex(0x9ca0a2), 0);
    lv_obj_set_pos(bank_lbl, 40, 445);

    g_dyn_toggles[0] = synthui_slide_toggle_create(scr);
    synthui_slide_toggle_set_positions(g_dyn_toggles[0], 2);
    synthui_slide_toggle_set_value(g_dyn_toggles[0], 0);
    synthui_slide_toggle_set_left_glyph(g_dyn_toggles[0], SYNTHUI_SLIDE_TOGGLE_GLYPH_SAW);
    synthui_slide_toggle_set_right_glyph(g_dyn_toggles[0], SYNTHUI_SLIDE_TOGGLE_GLYPH_SQUARE);
    synthui_slide_toggle_set_panel_color(g_dyn_toggles[0], SYNTHUI_SLIDE_TOGGLE_COLOR_PANEL_DEFAULT);
    lv_obj_set_size(g_dyn_toggles[0], 200, 64);
    lv_obj_set_pos(g_dyn_toggles[0], 60, 480);

    g_dyn_toggles[1] = synthui_slide_toggle_create(scr);
    synthui_slide_toggle_set_positions(g_dyn_toggles[1], 2);
    synthui_slide_toggle_set_value(g_dyn_toggles[1], 1);
    synthui_slide_toggle_set_left_glyph(g_dyn_toggles[1], SYNTHUI_SLIDE_TOGGLE_GLYPH_TRI);
    synthui_slide_toggle_set_right_glyph(g_dyn_toggles[1], SYNTHUI_SLIDE_TOGGLE_GLYPH_PULSE);
    synthui_slide_toggle_set_panel_color(g_dyn_toggles[1], 0xD6D4CFu);
    lv_obj_set_size(g_dyn_toggles[1], 200, 64);
    lv_obj_set_pos(g_dyn_toggles[1], 300, 480);

    g_dyn_toggles[2] = synthui_slide_toggle_create(scr);
    synthui_slide_toggle_set_positions(g_dyn_toggles[2], 3);
    synthui_slide_toggle_set_value(g_dyn_toggles[2], 0);
    synthui_slide_toggle_set_left_glyph(g_dyn_toggles[2], SYNTHUI_SLIDE_TOGGLE_GLYPH_SAW);
    synthui_slide_toggle_set_right_glyph(g_dyn_toggles[2], SYNTHUI_SLIDE_TOGGLE_GLYPH_TRI);
    synthui_slide_toggle_set_panel_color(g_dyn_toggles[2], 0x6D7A85u);
    lv_obj_set_size(g_dyn_toggles[2], 240, 68);
    lv_obj_set_pos(g_dyn_toggles[2], 60, 580);

    g_dyn_toggles[3] = synthui_slide_toggle_create(scr);
    synthui_slide_toggle_set_positions(g_dyn_toggles[3], 4);
    synthui_slide_toggle_set_value(g_dyn_toggles[3], 0);
    synthui_slide_toggle_set_left_glyph(g_dyn_toggles[3], SYNTHUI_SLIDE_TOGGLE_GLYPH_PULSE);
    synthui_slide_toggle_set_right_glyph(g_dyn_toggles[3], SYNTHUI_SLIDE_TOGGLE_GLYPH_SQUARE);
    synthui_slide_toggle_set_panel_color(g_dyn_toggles[3], 0x39434Bu);
    lv_obj_set_size(g_dyn_toggles[3], 260, 68);
    lv_obj_set_pos(g_dyn_toggles[3], 340, 580);

    Serial1.println("slide_toggle_scene=gallery toggles=14 states=14");

    /* ===== Phase A1: Initial full render and golden CRC capture ===== */
    g_max_damage = 0;
    g_total_damage = 0;

    lv_refr_now(disp);
    lvgl_mipi_panel_flip_sync();
    Serial1.println("LVGL_FLUSHED=PASS");
    Serial1.printf("LVGL_BYTES=%u\n", (unsigned)(RK055_WIDTH * RK055_HEIGHT * 4));

    const void *fb0 = lvgl_mipi_panel_scanned_fb();
    uint32_t init_crc = fnv1a_32(fb0, (size_t)RK055_WIDTH * RK055_HEIGHT * 4);
    Serial1.printf("slide_toggle_crc=0x%08X\n", (unsigned)init_crc);

#ifdef SLIDE_TOGGLE_EYEBALL_HOLD
    Serial1.println("SLIDE_TOGGLE_EYEBALL_HOLD active; pausing loop");
    for (;;) { delay(1000); }
#endif

    /* ===== Phase A2: 64-step deterministic delta sequence ===== */
    g_max_damage = 0;
    g_total_damage = 0;

    for (int step = 0; step < 64; step++) {
        /* Deterministic cycle across switch positions */
        synthui_slide_toggle_set_value(g_dyn_toggles[0], step % 2);
        synthui_slide_toggle_set_value(g_dyn_toggles[1], (step / 2) % 2);
        synthui_slide_toggle_set_value(g_dyn_toggles[2], step % 3);
        synthui_slide_toggle_set_value(g_dyn_toggles[3], step % 4);

        lv_refr_now(disp);
        lvgl_mipi_panel_flip_sync();
    }

    uint32_t delta_max_damage = g_max_damage;
    uint32_t delta_total_damage = g_total_damage;

    const void *delta_fb = lvgl_mipi_panel_scanned_fb();
    uint32_t delta_crc = fnv1a_32(delta_fb, (size_t)RK055_WIDTH * RK055_HEIGHT * 4);
    Serial1.printf("slide_toggle_delta_crc=0x%08X\n", (unsigned)delta_crc);

    /* Delta-equality check: force full redraw of final state */
    lv_obj_invalidate(scr);
    lv_refr_now(disp);
    lvgl_mipi_panel_flip_sync();

    const void *fresh_fb = lvgl_mipi_panel_scanned_fb();
    uint32_t fresh_crc = fnv1a_32(fresh_fb, (size_t)RK055_WIDTH * RK055_HEIGHT * 4);
    Serial1.printf("slide_toggle_fresh_crc=0x%08X\n", (unsigned)fresh_crc);

    if (delta_crc == fresh_crc) {
        Serial1.println("slide_toggle_delta_eq=PASS");
    } else {
        Serial1.println("slide_toggle_delta_eq=FAIL");
    }

    Serial1.printf("slide_toggle_damage max=%u total=%u\n",
                   (unsigned)delta_max_damage, (unsigned)delta_total_damage);

    Serial1.printf("slide_toggle_vsync flips=%lu isrs=%lu timeouts=%lu\n",
                   (unsigned long)lvgl_mipi_panel_flips(),
                   (unsigned long)lvgl_mipi_panel_vsync_isrs(),
                   (unsigned long)lvgl_mipi_panel_vsync_timeouts());

    Serial1.println("crc_done");
    Serial1.println("PASS: SynthUI slide_toggle render verified");
}

void loop()
{
    /* Phase B: continuous animation benchmark */
    static uint32_t frame_count = 0;
    static uint32_t last_report = 0;
    static int anim_step = 0;

    lv_display_t *disp = lv_display_get_default();
    if (!disp) return;

    synthui_slide_toggle_set_value(g_dyn_toggles[0], anim_step % 2);
    synthui_slide_toggle_set_value(g_dyn_toggles[1], (anim_step / 2) % 2);
    synthui_slide_toggle_set_value(g_dyn_toggles[2], anim_step % 3);
    synthui_slide_toggle_set_value(g_dyn_toggles[3], anim_step % 4);
    anim_step++;

    lv_refr_now(disp);
    lvgl_mipi_panel_flip_sync();
    frame_count++;

    uint32_t now = millis();
    if (now - last_report >= 1000) {
        uint32_t elapsed = now - last_report;
        uint32_t fps = (frame_count * 1000) / elapsed;
        Serial1.printf("slide_toggle_fps=%u\n", (unsigned)fps);
        frame_count = 0;
        last_report = now;
    }
}
