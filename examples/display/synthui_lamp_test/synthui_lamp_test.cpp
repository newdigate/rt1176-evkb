/* synthui_lamp_test - synthui_lamp (NEW-24) on the RK055 (720x1280
 * XRGB8888, db pipeline), checksummed.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Scene: 16-lamp bank (4x4 grid) on 720x1280 RK055 panel:
 *   Row 0: 4 round lamps (70x70) - Pink on, Red on, Green off, Amber on
 *   Row 1: 4 bar lamps (90x40)   - Blue on, Cyan off, Pink off, Red on
 *   Row 2: 4 pill lamps (90x40)  - Green on, Amber off, Blue on, Cyan on
 *   Row 3: 4 edge cases          - col 0: Round, Pink on, LV_STATE_DISABLED
 *                                - col 1: Bar, custom hex 0xF0A0D0, on
 *                                - col 2: Pill, custom hex 0x30E0D0, off
 *                                - col 3: Round, Amber on
 *
 * Phase A (gated): lamp_crc golden -> 64-step LCG delta sequence ->
 * lamp_delta_crc vs lamp_fresh_crc (the gate compares them), lamp_damage
 * engagement, lamp_vsync, crc_done, PASS: SynthUI lamp render verified.
 * Phase B (after crc_done, ungated): continuous chaser animation loop
 * measuring frame time (lamp_fps), then continues animating indefinitely. */
#include <Arduino.h>
#include <string.h>
#include <math.h>
#include "Display.h"
#include "lvgl_rt1176.h"
#include "lvgl_mipi_panel.h"
#include "synthui_lamp.h"

#ifdef LAMP_EYEBALL_HOLD
static void eyeball_hold(int n)
{
    if (n != LAMP_EYEBALL_HOLD) return;
    Serial1.printf("LAMP_EYEBALL_HOLD=%d\n", n);
    for (;;) { }
}
#else
#define eyeball_hold(n) ((void)0)
#endif

struct LampConfig {
    synthui_lamp_shape_t shape;
    uint32_t color;
    bool initial_on;
    bool disabled;
    int32_t w;
    int32_t h;
};

static const LampConfig kLampConfigs[16] = {
    /* Row 0: 4 round lamps (70x70) */
    { SYNTHUI_LAMP_SHAPE_ROUND, SYNTHUI_LAMP_COLOR_PINK,  true,  false, 70, 70 }, /* col 0 */
    { SYNTHUI_LAMP_SHAPE_ROUND, SYNTHUI_LAMP_COLOR_RED,   true,  false, 70, 70 }, /* col 1 */
    { SYNTHUI_LAMP_SHAPE_ROUND, SYNTHUI_LAMP_COLOR_GREEN, false, false, 70, 70 }, /* col 2 */
    { SYNTHUI_LAMP_SHAPE_ROUND, SYNTHUI_LAMP_COLOR_AMBER, true,  false, 70, 70 }, /* col 3 */

    /* Row 1: 4 bar lamps (90x40) */
    { SYNTHUI_LAMP_SHAPE_BAR,   SYNTHUI_LAMP_COLOR_BLUE,  true,  false, 90, 40 }, /* col 0 */
    { SYNTHUI_LAMP_SHAPE_BAR,   SYNTHUI_LAMP_COLOR_CYAN,  false, false, 90, 40 }, /* col 1 */
    { SYNTHUI_LAMP_SHAPE_BAR,   SYNTHUI_LAMP_COLOR_PINK,  false, false, 90, 40 }, /* col 2 */
    { SYNTHUI_LAMP_SHAPE_BAR,   SYNTHUI_LAMP_COLOR_RED,   true,  false, 90, 40 }, /* col 3 */

    /* Row 2: 4 pill lamps (90x40) */
    { SYNTHUI_LAMP_SHAPE_PILL,  SYNTHUI_LAMP_COLOR_GREEN, true,  false, 90, 40 }, /* col 0 */
    { SYNTHUI_LAMP_SHAPE_PILL,  SYNTHUI_LAMP_COLOR_AMBER, false, false, 90, 40 }, /* col 1 */
    { SYNTHUI_LAMP_SHAPE_PILL,  SYNTHUI_LAMP_COLOR_BLUE,  true,  false, 90, 40 }, /* col 2 */
    { SYNTHUI_LAMP_SHAPE_PILL,  SYNTHUI_LAMP_COLOR_CYAN,  true,  false, 90, 40 }, /* col 3 */

    /* Row 3: 4 edge cases */
    { SYNTHUI_LAMP_SHAPE_ROUND, SYNTHUI_LAMP_COLOR_PINK,  true,  true,  70, 70 }, /* col 0: disabled */
    { SYNTHUI_LAMP_SHAPE_BAR,   0xF0A0D0,                 true,  false, 90, 40 }, /* col 1: custom hex */
    { SYNTHUI_LAMP_SHAPE_PILL,  0x30E0D0,                 false, false, 90, 40 }, /* col 2: custom hex */
    { SYNTHUI_LAMP_SHAPE_ROUND, SYNTHUI_LAMP_COLOR_AMBER, true,  false, 70, 70 }, /* col 3: round amber */
};

static lv_obj_t *g_lamp[16];
static bool g_states[16];

static void opaque_bg(lv_obj_t *scr)
{
    /* Opaque ground forces LVGL to paint every pixel: fully-defined frames. */
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
}

/* Build the 16-lamp bank screen. Both golden and fresh reference pass use this,
 * guaranteeing identical object hierarchy, layout and styles. Re-points g_lamp[]
 * as a side effect. */
static lv_obj_t *build_bank(const bool *states)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    opaque_bg(scr);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "SynthUI Lamp");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

    for (int i = 0; i < 16; i++) {
        const int row = i / 4;
        const int col = i % 4;
        const LampConfig *cfg = &kLampConfigs[i];

        lv_obj_t *lamp = synthui_lamp_create(scr);
        lv_obj_set_size(lamp, cfg->w, cfg->h);

        const int32_t cx = 90 + col * 180;
        const int32_t cy = 200 + row * 180;
        lv_obj_set_pos(lamp, cx - cfg->w / 2, cy - cfg->h / 2);

        synthui_lamp_set_shape(lamp, cfg->shape);
        synthui_lamp_set_color(lamp, cfg->color);
        synthui_lamp_set_on(lamp, states[i]);

        if (cfg->disabled) {
            lv_obj_add_state(lamp, LV_STATE_DISABLED);
        }

        g_lamp[i] = lamp;
    }
    return scr;
}

/* Checksum the buffer that was PRESENTED (flipped to glass) -- in db mode
 * Display.framebuffer() is only one of the two buffers. flip_sync() first,
 * so a pending flip has retired and scanned_fb() names the front buffer. */
static uint32_t sum_active_screen(void)
{
    lvgl_mipi_panel_flip_sync();
    lvgl_sum_reset();
    lvgl_sum_feed(lvgl_mipi_panel_scanned_fb(), PANEL_FB_BYTES);
    return lvgl_sum_value();
}

static uint32_t sum_screen(lv_obj_t *scr)
{
    lv_screen_load(scr);
    lv_obj_invalidate(scr);
    lv_refr_now(NULL);
    return sum_active_screen();
}

/* --- Delta guards: per-invalidate area recorder */
static int32_t s_delta_maxarea = 0;
static long    s_delta_total = 0;
static bool    s_delta_record = false;

static void delta_inv_cb(lv_event_t *e)
{
    if (!s_delta_record) return;
    const lv_area_t *a = (const lv_area_t *)lv_event_get_param(e);
    const int32_t px = lv_area_get_width(a) * lv_area_get_height(a);
    s_delta_total += px;
    if (px > s_delta_maxarea) s_delta_maxarea = px;
}

#define LAMP_DELTA_STEPS 64
static uint32_t s_lcg;
static uint32_t lcg_next(void)
{
    s_lcg = s_lcg * 1664525u + 1013904223u;
    return s_lcg;
}

/* Runs ON the already-rendered golden bank; evolves g_states in place. */
static uint32_t delta_run_sequence(void)
{
    lv_display_add_event_cb(lv_display_get_default(), delta_inv_cb,
                            LV_EVENT_INVALIDATE_AREA, NULL);
    s_lcg = 0x5EEDF00Du;
    s_delta_maxarea = 0;
    s_delta_total = 0;
    s_delta_record = true;

    for (int step = 0; step < LAMP_DELTA_STEPS; step++) {
        uint32_t r = lcg_next();
        int idx1 = (r >> 16) & 0x0F;
        int idx2 = (r >> 20) & 0x0F;
        g_states[idx1] = !g_states[idx1];
        synthui_lamp_set_on(g_lamp[idx1], g_states[idx1]);
        if (idx2 != idx1) {
            g_states[idx2] = !g_states[idx2];
            synthui_lamp_set_on(g_lamp[idx2], g_states[idx2]);
        }
        lv_refr_now(NULL);
    }
    s_delta_record = false;
    return sum_active_screen();
}

/* --- Phase B: FPS measurement and continuous chaser animation */
#define LAMP_FPS_MAX 512
static uint32_t g_fps_us[LAMP_FPS_MAX];
static volatile uint32_t g_fps_n = 0, g_fps_frames = 0;
static volatile bool g_fps_timing = false, g_fps_skip = false;
static volatile bool g_fps_rendered = false;
static volatile uint32_t g_fps_t0 = 0;
static uint32_t g_anim_step = 0;

static void fps_refr_cb(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_REFR_START:   g_fps_t0 = micros(); break;
    case LV_EVENT_RENDER_READY: g_fps_rendered = true; break;
    case LV_EVENT_REFR_READY:
        if (g_fps_rendered && g_fps_timing) {
            if (g_fps_skip) g_fps_skip = false;
            else {
                g_fps_frames++;
                if (g_fps_n < LAMP_FPS_MAX)
                    g_fps_us[g_fps_n++] = micros() - g_fps_t0;
            }
        }
        g_fps_rendered = false;
        break;
    default: break;
    }
}

static void lamp_anim_cb(lv_timer_t *t)
{
    (void)t;
    g_anim_step++;
    uint32_t head = g_anim_step % 16;
    for (int i = 0; i < 16; i++) {
        bool on = (i == (int)head) || (i == (int)((head + 15) % 16));
        synthui_lamp_set_on(g_lamp[i], on);
    }
}

static void lamp_fps_phase(const char *tag, uint32_t target_frames)
{
    static bool cbs_added = false;
    if (!cbs_added) {
        lv_display_t *disp = lv_display_get_default();
        lv_display_add_event_cb(disp, fps_refr_cb, LV_EVENT_REFR_START, NULL);
        lv_display_add_event_cb(disp, fps_refr_cb, LV_EVENT_RENDER_READY, NULL);
        lv_display_add_event_cb(disp, fps_refr_cb, LV_EVENT_REFR_READY, NULL);
        cbs_added = true;
    }
    g_fps_n = 0;
    g_fps_frames = 0;
    g_fps_skip = true;
    g_fps_timing = true;

    lv_timer_t *anim = lv_timer_create(lamp_anim_cb, 15, NULL);
    const uint32_t t0 = millis();
    while (g_fps_n < target_frames && (millis() - t0) < 10000u) {
        lvgl_rt1176_loop();
    }
    g_fps_timing = false;
    lv_timer_delete(anim);

    uint32_t s[LAMP_FPS_MAX];
    const uint32_t n = g_fps_n ? g_fps_n : 1;
    memcpy(s, (const void *)g_fps_us, n * sizeof(uint32_t));
    for (uint32_t i = 1; i < n; i++) {
        const uint32_t v = s[i];
        int32_t j = (int32_t)i - 1;
        while (j >= 0 && s[j] > v) { s[j + 1] = s[j]; j--; }
        s[j + 1] = v;
    }
    Serial1.printf("%s frames=%lu mfps_med=%lu us_med=%lu us_min=%lu us_max=%lu\n",
                   tag,
                   (unsigned long)g_fps_n,
                   (unsigned long)(1000000000ull / (s[n / 2] ? s[n / 2] : 1)),
                   (unsigned long)s[n / 2],
                   (unsigned long)s[0],
                   (unsigned long)s[n - 1]);
}

void setup()
{
    Serial1.begin(115200);
    delay(200);
    Serial1.println("SYNTHUI_LAMP_BEGIN");

    const bool ok = Display.begin();
    Serial1.println(ok ? "PANEL_OK" : "PANEL_FAIL");
    if (!ok) {
        Serial1.println("crc_done");
        return;
    }
    Display.fillScreen(0x0000);

    lvgl_rt1176_begin();
    lvgl_mipi_panel_create_db(Display);

    Serial1.println("lamp_scene=16 grid=4x4");

    /* Phase A1: Full initial render producing golden lamp_crc */
    for (int i = 0; i < 16; i++) {
        g_states[i] = kLampConfigs[i].initial_on;
    }
    lv_screen_load(build_bank(g_states));
    uint32_t t0 = millis();
    while (!lvgl_mipi_panel_frame_done() && (millis() - t0) < 5000) {
        lvgl_rt1176_loop();
    }
    Serial1.printf("LVGL_FLUSHED=%s\n",
                   lvgl_mipi_panel_frame_done() ? "PASS" : "FAIL");
    Serial1.printf("LVGL_BYTES=%lu\n",
                   (unsigned long)(lvgl_mipi_panel_flushed_px()
                                   * PANEL_BYTES_PER_PIXEL));
    const uint32_t lamp_crc = sum_active_screen();
    Serial1.printf("lamp_crc=0x%08lX\n", (unsigned long)lamp_crc);
    eyeball_hold(1);

    /* Phase A2: 64-step deterministic delta sequence vs fresh full render */
    const uint32_t d_seq = delta_run_sequence();
    eyeball_hold(2);
    const uint32_t d_full = sum_screen(build_bank(g_states));
    eyeball_hold(3);

    Serial1.printf("lamp_delta_crc=0x%08lX\n", (unsigned long)d_seq);
    Serial1.printf("lamp_fresh_crc=0x%08lX\n", (unsigned long)d_full);
    Serial1.printf("lamp_delta_eq=%s\n", (d_seq == d_full) ? "PASS" : "FAIL");
    Serial1.printf("lamp_damage max=%ld total=%ld steps=%d\n",
                   (long)s_delta_maxarea, s_delta_total, LAMP_DELTA_STEPS);
    Serial1.printf("lamp_vsync flips=%lu isrs=%lu timeouts=%lu\n",
                   (unsigned long)lvgl_mipi_panel_flips(),
                   (unsigned long)lvgl_mipi_panel_vsync_isrs(),
                   (unsigned long)lvgl_mipi_panel_vsync_timeouts());
    Serial1.println("crc_done");
    Serial1.println("PASS: SynthUI lamp render verified");

    /* Phase B: Continuous chaser animation measuring fps */
    lamp_fps_phase("lamp_fps", 64);

    /* Pool health check */
    {
        lv_mem_monitor_t mm;
        lv_mem_monitor(&mm);
        Serial1.printf("lamp_mem total=%lu used_pct=%u max_used=%lu frag_pct=%u\n",
                       (unsigned long)mm.total_size, (unsigned)mm.used_pct,
                       (unsigned long)mm.max_used, (unsigned)mm.frag_pct);
    }

    /* Leave animating forever for eyes/camera pass */
    lv_timer_create(lamp_anim_cb, 15, NULL);
}

void loop()
{
    lvgl_rt1176_loop();
}
