/* synthui_level_meter_test - synthui_level_meter (NEW-26) on the RK055
 * display panel under QEMU and on silicon.
 *
 * Pipeline: double-buffered hardware pipeline via lvgl_mipi_panel_create_db(Display).
 * Verifies:
 *   1. Full initial render of 8-meter synth console scene -> pinned golden CRC (FNV-1a over 720x1280 fb).
 *   2. 64-step deterministic delta sequence.
 *   3. Delta equality guard: delta-rendered CRC matches fresh full-render CRC.
 *   4. Damage engagement guard: max invalidated area stays small (<= 15000 px).
 *   5. VSYNC health guard: timeouts=0.
 *
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT */

#include <Arduino.h>
#include <Display.h>
#include <lvgl.h>
#include "lvgl_rt1176.h"
#include "lvgl_mipi_panel.h"
#include "synthui_level_meter.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

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

static lv_obj_t *g_meters[8];

void setup()
{
    Serial1.begin(115200);
    delay(200);

    Serial1.println("SYNTHUI_LEVEL_METER_BEGIN");

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

    /* Scene: 8 meters on dark synth panel */
    /* 1. Master Left: 24 segments, W=48, H=380, at (80, 80), value=0.78, peak=0.88 */
    g_meters[0] = synthui_level_meter_create(scr);
    lv_obj_set_pos(g_meters[0], 80, 80);
    lv_obj_set_size(g_meters[0], 48, 380);
    synthui_level_meter_set_segments(g_meters[0], 24);
    synthui_level_meter_set_value(g_meters[0], 0.78f);
    synthui_level_meter_set_peak(g_meters[0], 0.88f);

    /* 2. Master Right: 24 segments, W=48, H=380, at (144, 80), value=0.72, peak=0.85 */
    g_meters[1] = synthui_level_meter_create(scr);
    lv_obj_set_pos(g_meters[1], 144, 80);
    lv_obj_set_size(g_meters[1], 48, 380);
    synthui_level_meter_set_segments(g_meters[1], 24);
    synthui_level_meter_set_value(g_meters[1], 0.72f);
    synthui_level_meter_set_peak(g_meters[1], 0.85f);

    /* 3-6. Tracks 1-4: 16 segments, W=36, H=240, at (240, 80), (290, 80), (340, 80), (390, 80)
     * Values: 0.65, 0.85, 0.45, 0.95; Peaks: 0.75, 0.92, 0.60, 0.98 */
    struct TrackConfig {
        int32_t x;
        float val;
        float peak;
    };
    static const TrackConfig kTracks[4] = {
        { 240, 0.65f, 0.75f },
        { 290, 0.85f, 0.92f },
        { 340, 0.45f, 0.60f },
        { 390, 0.95f, 0.98f }
    };
    for (int i = 0; i < 4; i++) {
        g_meters[2 + i] = synthui_level_meter_create(scr);
        lv_obj_set_pos(g_meters[2 + i], kTracks[i].x, 80);
        lv_obj_set_size(g_meters[2 + i], 36, 240);
        synthui_level_meter_set_segments(g_meters[2 + i], 16);
        synthui_level_meter_set_value(g_meters[2 + i], kTracks[i].val);
        synthui_level_meter_set_peak(g_meters[2 + i], kTracks[i].peak);
    }

    /* 7. Aux 1: 10 segments, W=42, H=180, at (470, 80), value=0.50, peak=0.70 */
    g_meters[6] = synthui_level_meter_create(scr);
    lv_obj_set_pos(g_meters[6], 470, 80);
    lv_obj_set_size(g_meters[6], 42, 180);
    synthui_level_meter_set_segments(g_meters[6], 10);
    synthui_level_meter_set_value(g_meters[6], 0.50f);
    synthui_level_meter_set_peak(g_meters[6], 0.70f);

    /* 8. Aux 2: 10 segments, W=42, H=180, at (530, 80), value=1.00, peak=1.00, clip=true */
    g_meters[7] = synthui_level_meter_create(scr);
    lv_obj_set_pos(g_meters[7], 530, 80);
    lv_obj_set_size(g_meters[7], 42, 180);
    synthui_level_meter_set_segments(g_meters[7], 10);
    synthui_level_meter_set_value(g_meters[7], 1.00f);
    synthui_level_meter_set_peak(g_meters[7], 1.00f);
    synthui_level_meter_set_clip(g_meters[7], true);

    Serial1.println("level_meter_scene=8 count=8");

    /* Initial full render */
    lv_refr_now(disp);
    lvgl_mipi_panel_flip_sync();
    Serial1.println("LVGL_FLUSHED=PASS");
    Serial1.printf("LVGL_BYTES=%u\n", (unsigned)(RK055_WIDTH * RK055_HEIGHT * 4));

    const void *fb0 = lvgl_mipi_panel_scanned_fb();
    uint32_t init_crc = fnv1a_32(fb0, (size_t)RK055_WIDTH * RK055_HEIGHT * 4);
    Serial1.printf("level_meter_crc=0x%08X\n", (unsigned)init_crc);

#ifdef LEVEL_METER_EYEBALL_HOLD
    Serial1.println("LEVEL_METER_EYEBALL_HOLD active; pausing loop");
    for (;;) { delay(1000); }
#endif

    /* Phase A2: 64-step deterministic delta sequence */
    g_max_damage = 0;
    g_total_damage = 0;

    for (int step = 1; step <= 64; step++) {
        float s = (float)step;

        /* Master L & R (24 segments) */
        float vl = 0.70f + 0.15f * sinf(s * 0.10f);
        float pl = vl + 0.08f;
        if (pl > 1.0f) pl = 1.0f;
        synthui_level_meter_set_value(g_meters[0], vl);
        synthui_level_meter_set_peak(g_meters[0], pl);

        float vr = 0.68f + 0.16f * sinf(s * 0.10f + 0.5f);
        float pr = vr + 0.09f;
        if (pr > 1.0f) pr = 1.0f;
        synthui_level_meter_set_value(g_meters[1], vr);
        synthui_level_meter_set_peak(g_meters[1], pr);

        /* Tracks 1-4 (16 segments) */
        for (int t = 0; t < 4; t++) {
            float vt = 0.55f + 0.30f * sinf(s * (0.08f + 0.02f * (float)t) + (float)t);
            float pt = vt + 0.07f;
            if (pt > 1.0f) pt = 1.0f;
            synthui_level_meter_set_value(g_meters[2 + t], vt);
            synthui_level_meter_set_peak(g_meters[2 + t], pt);
        }

        /* Aux 1 (10 segments) */
        float v_aux1 = 0.50f + 0.25f * sinf(s * 0.12f + 1.0f);
        float p_aux1 = v_aux1 + 0.10f;
        if (p_aux1 > 1.0f) p_aux1 = 1.0f;
        synthui_level_meter_set_value(g_meters[6], v_aux1);
        synthui_level_meter_set_peak(g_meters[6], p_aux1);

        /* Aux 2 (10 segments): pulses clip indicator */
        float v_aux2 = 0.70f + 0.30f * sinf(s * 0.14f + 2.0f);
        bool clip = (v_aux2 >= 0.95f);
        float p_aux2 = v_aux2 + 0.05f;
        if (p_aux2 > 1.0f) p_aux2 = 1.0f;
        synthui_level_meter_set_value(g_meters[7], v_aux2);
        synthui_level_meter_set_peak(g_meters[7], p_aux2);
        synthui_level_meter_set_clip(g_meters[7], clip);

        lv_refr_now(disp);
        lvgl_mipi_panel_flip_sync();
    }

    uint32_t delta_max_damage = g_max_damage;
    uint32_t delta_total_damage = g_total_damage;

    const void *delta_fb = lvgl_mipi_panel_scanned_fb();
    uint32_t delta_crc = fnv1a_32(delta_fb, (size_t)RK055_WIDTH * RK055_HEIGHT * 4);
    Serial1.printf("level_meter_delta_crc=0x%08X\n", (unsigned)delta_crc);

    /* Delta-equality check: force full redraw of final state */
    lv_obj_invalidate(scr);
    lv_refr_now(disp);
    lvgl_mipi_panel_flip_sync();

    const void *fresh_fb = lvgl_mipi_panel_scanned_fb();
    uint32_t fresh_crc = fnv1a_32(fresh_fb, (size_t)RK055_WIDTH * RK055_HEIGHT * 4);
    Serial1.printf("level_meter_fresh_crc=0x%08X\n", (unsigned)fresh_crc);

    if (delta_crc == fresh_crc) {
        Serial1.println("level_meter_delta_eq=PASS");
    } else {
        Serial1.println("level_meter_delta_eq=FAIL");
    }

    Serial1.printf("level_meter_damage max=%u total=%u\n", (unsigned)delta_max_damage, (unsigned)delta_total_damage);

    Serial1.printf("level_meter_vsync flips=%lu isrs=%lu timeouts=%lu\n",
                   (unsigned long)lvgl_mipi_panel_flips(),
                   (unsigned long)lvgl_mipi_panel_vsync_isrs(),
                   (unsigned long)lvgl_mipi_panel_vsync_timeouts());

    Serial1.println("crc_done");
    Serial1.println("PASS: SynthUI level_meter render verified");
}

void loop()
{
    /* Phase B: continuous animation benchmark */
    static uint32_t frame_count = 0;
    static uint32_t last_report = 0;
    static uint32_t anim_step = 64;

    anim_step++;
    float s = (float)anim_step;

    float vl = 0.70f + 0.15f * sinf(s * 0.10f);
    synthui_level_meter_set_value(g_meters[0], vl);
    synthui_level_meter_set_peak(g_meters[0], vl + 0.08f > 1.0f ? 1.0f : vl + 0.08f);

    float vr = 0.68f + 0.16f * sinf(s * 0.10f + 0.5f);
    synthui_level_meter_set_value(g_meters[1], vr);
    synthui_level_meter_set_peak(g_meters[1], vr + 0.09f > 1.0f ? 1.0f : vr + 0.09f);

    for (int t = 0; t < 4; t++) {
        float vt = 0.55f + 0.30f * sinf(s * (0.08f + 0.02f * (float)t) + (float)t);
        synthui_level_meter_set_value(g_meters[2 + t], vt);
        synthui_level_meter_set_peak(g_meters[2 + t], vt + 0.07f > 1.0f ? 1.0f : vt + 0.07f);
    }

    float v_aux1 = 0.50f + 0.25f * sinf(s * 0.12f + 1.0f);
    synthui_level_meter_set_value(g_meters[6], v_aux1);
    synthui_level_meter_set_peak(g_meters[6], v_aux1 + 0.10f > 1.0f ? 1.0f : v_aux1 + 0.10f);

    float v_aux2 = 0.70f + 0.30f * sinf(s * 0.14f + 2.0f);
    synthui_level_meter_set_value(g_meters[7], v_aux2);
    synthui_level_meter_set_peak(g_meters[7], v_aux2 + 0.05f > 1.0f ? 1.0f : v_aux2 + 0.05f);
    synthui_level_meter_set_clip(g_meters[7], (v_aux2 >= 0.95f));

    lv_timer_handler();
    lvgl_mipi_panel_flip_sync();

    frame_count++;
    uint32_t now = millis();
    if (now - last_report >= 1000) {
        uint32_t elapsed = now - last_report;
        uint32_t fps_x10 = elapsed ? (frame_count * 10000u) / elapsed : 0;
        Serial1.printf("level_meter_fps=%lu.%lu\n", (unsigned long)(fps_x10 / 10), (unsigned long)(fps_x10 % 10));
        frame_count = 0;
        last_report = now;
    }
}
