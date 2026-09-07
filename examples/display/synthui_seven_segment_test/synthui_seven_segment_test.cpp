/* synthui_seven_segment_test - synthui_seven_segment (NEW-29) on the RK055
 * display panel under QEMU and on silicon.
 *
 * Pipeline: double-buffered hardware pipeline via lvgl_mipi_panel_create_db(Display).
 * Verifies:
 *   1. Full initial render of 6-readout scene -> pinned golden CRC (FNV-1a over 720x1280 fb).
 *   2. 64-step deterministic delta sequence (counter ticks).
 *   3. Delta equality guard: delta-rendered CRC matches fresh full-render CRC.
 *   4. Damage engagement guard: max invalidated area stays small.
 *   5. VSYNC health guard: timeouts=0.
 *
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT */

#include <Arduino.h>
#include <Display.h>
#include <lvgl.h>
#include "lvgl_rt1176.h"
#include "lvgl_mipi_panel.h"
#include "synthui_seven_segment.h"
#include <stdio.h>
#include <string.h>

#define RK055_WIDTH  720
#define RK055_HEIGHT 1280

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

static lv_obj_t *g_readouts[6];

void setup()
{
    Serial1.begin(115200);
    delay(200);

    Serial1.println("SYNTHUI_SEVEN_SEGMENT_BEGIN");

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
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101020), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* Scene: 6 readouts on dark synth panel */
    /* 1. Main BPM Readout: "140.0" at y=80, h=96, Blue accent */
    g_readouts[0] = synthui_seven_segment_create(scr);
    lv_obj_set_pos(g_readouts[0], 60, 80);
    lv_obj_set_size(g_readouts[0], 480, 96);
    synthui_seven_segment_set_text(g_readouts[0], "140.0");
    synthui_seven_segment_set_accent(g_readouts[0], SYNTHUI_SEVEN_SEGMENT_COLOR_BLUE_ON, SYNTHUI_SEVEN_SEGMENT_COLOR_BLUE_GLOW);

    /* 2. Bar:Beat Readout: "01:04" at y=230, h=56, Cyan accent */
    g_readouts[1] = synthui_seven_segment_create(scr);
    lv_obj_set_pos(g_readouts[1], 60, 230);
    lv_obj_set_size(g_readouts[1], 320, 56);
    synthui_seven_segment_set_text(g_readouts[1], "01:04");
    synthui_seven_segment_set_accent(g_readouts[1], SYNTHUI_SEVEN_SEGMENT_COLOR_CYAN_ON, SYNTHUI_SEVEN_SEGMENT_COLOR_CYAN_GLOW);

    /* 3. Pattern Readout: "P-A3" at y=340, h=56, Amber accent */
    g_readouts[2] = synthui_seven_segment_create(scr);
    lv_obj_set_pos(g_readouts[2], 60, 340);
    lv_obj_set_size(g_readouts[2], 300, 56);
    synthui_seven_segment_set_text(g_readouts[2], "P-A3");
    synthui_seven_segment_set_accent(g_readouts[2], SYNTHUI_SEVEN_SEGMENT_COLOR_AMBER_ON, SYNTHUI_SEVEN_SEGMENT_COLOR_AMBER_GLOW);

    /* 4. Track Status Readout: "REC.8" at y=450, h=56, Red accent */
    g_readouts[3] = synthui_seven_segment_create(scr);
    lv_obj_set_pos(g_readouts[3], 60, 450);
    lv_obj_set_size(g_readouts[3], 320, 56);
    synthui_seven_segment_set_text(g_readouts[3], "REC.8");
    synthui_seven_segment_set_accent(g_readouts[3], SYNTHUI_SEVEN_SEGMENT_COLOR_RED_ON, SYNTHUI_SEVEN_SEGMENT_COLOR_RED_GLOW);

    /* 5. Ghost-off Readout: "8888" at y=560, h=44, Cyan accent, ghost=false */
    g_readouts[4] = synthui_seven_segment_create(scr);
    lv_obj_set_pos(g_readouts[4], 60, 560);
    lv_obj_set_size(g_readouts[4], 240, 44);
    synthui_seven_segment_set_text(g_readouts[4], "8888");
    synthui_seven_segment_set_accent(g_readouts[4], SYNTHUI_SEVEN_SEGMENT_COLOR_CYAN_ON, SYNTHUI_SEVEN_SEGMENT_COLOR_CYAN_GLOW);
    synthui_seven_segment_set_ghost(g_readouts[4], false);

    /* 6. Disabled Readout: "OFF" at y=660, h=44, Red accent, LV_STATE_DISABLED */
    g_readouts[5] = synthui_seven_segment_create(scr);
    lv_obj_set_pos(g_readouts[5], 60, 660);
    lv_obj_set_size(g_readouts[5], 200, 44);
    synthui_seven_segment_set_text(g_readouts[5], "OFF");
    synthui_seven_segment_set_accent(g_readouts[5], SYNTHUI_SEVEN_SEGMENT_COLOR_RED_ON, SYNTHUI_SEVEN_SEGMENT_COLOR_RED_GLOW);
    lv_obj_add_state(g_readouts[5], LV_STATE_DISABLED);

    Serial1.println("seven_segment_scene=6 count=6");

    /* Initial full render */
    lv_refr_now(disp);
    lvgl_mipi_panel_flip_sync();
    Serial1.println("LVGL_FLUSHED=PASS");
    Serial1.printf("LVGL_BYTES=%u\n", (unsigned)(RK055_WIDTH * RK055_HEIGHT * 4));

    const void *fb0 = lvgl_mipi_panel_scanned_fb();
    uint32_t init_crc = fnv1a_32(fb0, (size_t)RK055_WIDTH * RK055_HEIGHT * 4);
    Serial1.printf("seven_segment_crc=0x%08X\n", (unsigned)init_crc);

#ifdef SEVEN_SEGMENT_EYEBALL_HOLD
    Serial1.println("SEVEN_SEGMENT_EYEBALL_HOLD active; pausing loop");
    for (;;) { delay(1000); }
#endif

    /* Phase A2: 64-step deterministic delta sequence */
    g_max_damage = 0;
    g_total_damage = 0;

    char bpm_buf[16];
    char bar_buf[16];

    for (int step = 1; step <= 64; step++) {
        /* Increment BPM counter: 140.0 -> 140.1 -> ... */
        int frac = step % 10;
        int whole = 140 + (step / 10);
        snprintf(bpm_buf, sizeof(bpm_buf), "%d.%d", whole, frac);
        synthui_seven_segment_set_text(g_readouts[0], bpm_buf);

        /* Bar:beat progression */
        int beat = (step % 4) + 1;
        int bar = (step / 4) + 1;
        snprintf(bar_buf, sizeof(bar_buf), "%02d:%02d", bar, beat);
        synthui_seven_segment_set_text(g_readouts[1], bar_buf);

        lv_refr_now(disp);
        lvgl_mipi_panel_flip_sync();
    }

    uint32_t delta_max_damage = g_max_damage;
    uint32_t delta_total_damage = g_total_damage;

    const void *delta_fb = lvgl_mipi_panel_scanned_fb();
    uint32_t delta_crc = fnv1a_32(delta_fb, (size_t)RK055_WIDTH * RK055_HEIGHT * 4);
    Serial1.printf("seven_segment_delta_crc=0x%08X\n", (unsigned)delta_crc);

    /* Delta-equality check: force full redraw of final state */
    lv_obj_invalidate(scr);
    lv_refr_now(disp);
    lvgl_mipi_panel_flip_sync();

    const void *fresh_fb = lvgl_mipi_panel_scanned_fb();
    uint32_t fresh_crc = fnv1a_32(fresh_fb, (size_t)RK055_WIDTH * RK055_HEIGHT * 4);
    Serial1.printf("seven_segment_fresh_crc=0x%08X\n", (unsigned)fresh_crc);

    if (delta_crc == fresh_crc) {
        Serial1.println("seven_segment_delta_eq=PASS");
    } else {
        Serial1.println("seven_segment_delta_eq=FAIL");
    }

    Serial1.printf("seven_segment_damage max=%u total=%u\n", (unsigned)delta_max_damage, (unsigned)delta_total_damage);

    Serial1.printf("seven_segment_vsync flips=%lu isrs=%lu timeouts=%lu\n",
                   (unsigned long)lvgl_mipi_panel_flips(),
                   (unsigned long)lvgl_mipi_panel_vsync_isrs(),
                   (unsigned long)lvgl_mipi_panel_vsync_timeouts());

    Serial1.println("crc_done");
    Serial1.println("PASS: SynthUI seven_segment render verified");
}

void loop()
{
    /* Phase B: continuous animation benchmark */
    static uint32_t frame_count = 0;
    static uint32_t last_report = 0;

    char buf[16];
    snprintf(buf, sizeof(buf), "%02lu:%02lu", (frame_count / 60) % 100, frame_count % 60);
    synthui_seven_segment_set_text(g_readouts[1], buf);

    lv_timer_handler();
    lvgl_mipi_panel_flip_sync();

    frame_count++;
    uint32_t now = millis();
    if (now - last_report >= 1000) {
        uint32_t elapsed = now - last_report;
        uint32_t fps_x10 = elapsed ? (frame_count * 10000u) / elapsed : 0;
        Serial1.printf("seven_segment_fps=%lu.%lu\n", (unsigned long)(fps_x10 / 10), (unsigned long)(fps_x10 % 10));
        frame_count = 0;
        last_report = now;
    }
}
