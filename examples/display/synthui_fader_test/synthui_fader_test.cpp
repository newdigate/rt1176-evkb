/* synthui_fader_test - synthui_fader (NEW-23) on the RK055 (720x1280
 * XRGB8888, db pipeline), checksummed.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Scene: the spec section-9 16-fader bank, 2x8 at 78x210, config axes
 * exercised inside the ONE golden (values i/15; center on 4..7; 12
 * DISABLED; 13 PRESSED, so all three palettes are golden-pinned; panel
 * greys cycled i%4; ticks 33 on 8, 5 on 15, 13 elsewhere).
 *
 * Phase A (gated): fd_crc golden -> 64-step LCG delta sequence ->
 * fd_delta_crc vs fd_fresh_crc (the gate compares them -- never
 * re-goldened), fd_damage engagement, fd_vsync, crc_done.
 * Phase B (after crc_done, ungated -- QEMU timing is meaningless): 60 s
 * all-16 sine animation -> fd_fps, then a 10 s full-invalidate run ->
 * fd_fps_fullinv, the honest what-delta-buys baseline (spec section 10).
 * The bank keeps animating forever afterwards for the eyes/camera pass. */
#include <Arduino.h>
#include <string.h>
#include <math.h>
#include "Display.h"
#include "lvgl_rt1176.h"
#include "lvgl_mipi_panel.h"
#include "synthui_fader.h"

static const uint32_t panel_opt[4] = {
    SYNTHUI_FADER_PANEL_DEFAULT, SYNTHUI_FADER_PANEL_DARK,
    SYNTHUI_FADER_PANEL_LIGHT,   SYNTHUI_FADER_PANEL_DEEP };

static lv_obj_t *g_fader[16];
static float g_vals[16];

#ifdef FD_EYEBALL_HOLD
static void eyeball_hold(int n)
{
    if (n != FD_EYEBALL_HOLD) return;
    Serial1.printf("FD_EYEBALL_HOLD=%d\n", n);
    for (;;) { }
}
#else
#define eyeball_hold(n) ((void)0)
#endif

static void opaque_bg(lv_obj_t *scr)
{   /* Opaque ground forces LVGL to paint every pixel: fully-defined frames. */
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
}

/* The one bank builder both the golden and the fresh reference use -- the
 * equality guard depends on the two paths sharing this code. Re-points
 * g_fader[] as a side effect -- callers must build the bank they are about
 * to drive LAST. */
static lv_obj_t *build_bank(const float *vals, bool pressed5)
{
    lv_obj_t *scr = lv_obj_create(NULL); opaque_bg(scr);
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "SynthUI Fader");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);
    for (int i = 0; i < 16; i++) {
        lv_obj_t *fd = synthui_fader_create(scr);
        lv_obj_set_size(fd, 78, 210);
        lv_obj_set_pos(fd, 12 + (i % 8) * 88, 120 + (i / 8) * 280);
        synthui_fader_set_value(fd, vals[i]);
        synthui_fader_set_panel(fd, panel_opt[i % 4]);
        if (i >= 4 && i <= 7) synthui_fader_set_center(fd, true);
        if (i == 8)  synthui_fader_set_ticks(fd, 33);
        if (i == 15) synthui_fader_set_ticks(fd, 5);
        if (i == 12) lv_obj_add_state(fd, LV_STATE_DISABLED);
        if (i == 13) lv_obj_add_state(fd, LV_STATE_PRESSED);
        if (pressed5 && i == 5) lv_obj_add_state(fd, LV_STATE_PRESSED);
        g_fader[i] = fd;
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

/* --- delta guards (spec section 9): per-invalidate area recorder, the
 * knob test's mechanism verbatim (LV_EVENT_INVALIDATE_AREA is a DISPLAY
 * event, sent per lv_inv_area with the clipped area). */
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

/* Fixed-seed LCG -> value steps in [-0.06, +0.06] (spec section 9).
 * Phase B's animation is STEP-indexed (tt advances 15 ms per frame), so
 * its max per-frame delta is 0.5*2pi*0.5*0.015 ~= 0.024 -- the LCG
 * envelope covers it ~2.5x over.  The spec's 0.052 figure assumed a
 * wall-clocked 30 fps animation this code deliberately does not use
 * (the knob's step-indexed house pattern). */
static uint32_t s_lcg;
static float lcg_delta(void)
{
    s_lcg = s_lcg * 1664525u + 1013904223u;
    return ((float)(s_lcg >> 8) * (1.0f / 16777215.0f) - 0.5f) * 0.12f;
}

#define FD_DELTA_STEPS 64

/* Runs ON the already-rendered golden bank; evolves g_vals in place. */
static uint32_t delta_run_sequence(void)
{
    lv_display_add_event_cb(lv_display_get_default(), delta_inv_cb,
                            LV_EVENT_INVALIDATE_AREA, NULL);
    s_lcg = 0x5EEDF00Du;
    s_delta_maxarea = 0; s_delta_total = 0; s_delta_record = true;
    for (int step = 0; step < FD_DELTA_STEPS; step++) {
        for (int i = 0; i < 16; i++) {
            float v = g_vals[i] + lcg_delta();
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            g_vals[i] = v;
            synthui_fader_set_value(g_fader[i], v);
        }
        lv_refr_now(NULL);
    }
    s_delta_record = false;   /* the state toggle below SHOULD be big */
    lv_obj_add_state(g_fader[5], LV_STATE_PRESSED);
    lv_obj_invalidate(g_fader[5]);     /* programmatic-state contract */
    lv_refr_now(NULL);
    for (int i = 0; i < 16; i++) {     /* one more step on the new state */
        float v = g_vals[i] + lcg_delta();
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        g_vals[i] = v;
        synthui_fader_set_value(g_fader[i], v);
    }
    lv_refr_now(NULL);
    return sum_active_screen();
}

/* --- fd_fps: knob-test machinery (REFR_START->REFR_READY, damage-gated by
 * RENDER_READY, first timed frame discarded (g_fps_skip)), run for a fixed
 * wall time. */
#define FD_FPS_MAX 4096
static uint32_t g_fps_us[FD_FPS_MAX];
static volatile uint32_t g_fps_n = 0, g_fps_frames = 0;
static volatile bool g_fps_timing = false, g_fps_skip = false;
static volatile bool g_fps_rendered = false;
static volatile uint32_t g_fps_t0 = 0;
static bool g_anim_full_inv = false;
static uint32_t g_anim_step = 0;
static uint32_t g_anim_n = 16;   /* how many faders the anim drives (scaling probe) */

static void fps_refr_cb(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_REFR_START:  g_fps_t0 = micros(); break;
    case LV_EVENT_RENDER_READY: g_fps_rendered = true; break;
    case LV_EVENT_REFR_READY:
        if (g_fps_rendered && g_fps_timing) {
            if (g_fps_skip) g_fps_skip = false;
            else {
                g_fps_frames++;
                if (g_fps_n < FD_FPS_MAX)
                    g_fps_us[g_fps_n++] = micros() - g_fps_t0;
            }
        }
        g_fps_rendered = false;
        break;
    default: break;
    }
}

static void fd_anim_cb(lv_timer_t *t)
{
    (void)t;
    g_anim_step++;
    const float tt = (float)g_anim_step * 0.015f;   /* 15 ms timer */
    for (int i = 0; i < (int)g_anim_n; i++) {
        synthui_fader_set_value(g_fader[i],
            0.5f + 0.5f * sinf(6.2831853f * 0.5f * tt
                               + (float)i * 0.3926991f));  /* 2*pi/16 */
        if (g_anim_full_inv) lv_obj_invalidate(g_fader[i]);
    }
}

static void fd_fps_phase(const char *tag, bool full_inv, uint32_t run_ms)
{
    static bool cbs_added = false;
    if (!cbs_added) {
        lv_display_t *disp = lv_display_get_default();
        lv_display_add_event_cb(disp, fps_refr_cb, LV_EVENT_REFR_START, NULL);
        lv_display_add_event_cb(disp, fps_refr_cb, LV_EVENT_RENDER_READY, NULL);
        lv_display_add_event_cb(disp, fps_refr_cb, LV_EVENT_REFR_READY, NULL);
        cbs_added = true;
    }
    g_fps_n = 0; g_fps_frames = 0; g_fps_skip = true; g_fps_timing = true;
    g_anim_full_inv = full_inv;
    lv_timer_t *anim = lv_timer_create(fd_anim_cb, 15, NULL);
    const uint32_t t0 = millis();
    while (millis() - t0 < run_ms) lvgl_rt1176_loop();
    const uint32_t elapsed = millis() - t0;
    g_fps_timing = false;
    lv_timer_delete(anim);
    uint32_t s[FD_FPS_MAX];
    const uint32_t n = g_fps_n ? g_fps_n : 1;
    memcpy(s, (const void *)g_fps_us, n * sizeof(uint32_t));
    for (uint32_t i = 1; i < n; i++) {
        const uint32_t v = s[i]; int32_t j = (int32_t)i - 1;
        while (j >= 0 && s[j] > v) { s[j + 1] = s[j]; j--; }
        s[j + 1] = v;
    }
    Serial1.printf("%s frames=%lu n=%lu secs=%lu fps_avg=%lu mfps_med=%lu"
                   " us_med=%lu us_max=%lu\n", tag,
                   (unsigned long)g_fps_frames,
                   (unsigned long)g_fps_n,
                   (unsigned long)(elapsed / 1000u),
                   (unsigned long)((uint64_t)g_fps_frames * 1000u / elapsed),
                   (unsigned long)(1000000000ull / (s[n / 2] ? s[n / 2] : 1)),
                   (unsigned long)s[n / 2], (unsigned long)s[n - 1]);
}

/* --- Phase C probes (NEW-23 fps diagnosis): snapshot the port's cumulative
 * counters around an fps phase, so each phase reports its OWN per-flip fence
 * wait and rendered px.  px_pf also exposes the direct-mode prev+current
 * damage join, which the analytic damage estimate does not include. */
static uint32_t s_pr_px0, s_pr_wait0, s_pr_flips0, s_pr_to0;
static void probe_mark(void)
{
    s_pr_px0    = lvgl_mipi_panel_flushed_px();
    s_pr_wait0  = lvgl_mipi_panel_wait_us();
    s_pr_flips0 = lvgl_mipi_panel_flips();
    s_pr_to0    = lvgl_mipi_panel_vsync_timeouts();
}
static void probe_print(const char *tag)
{
    const uint32_t fl = lvgl_mipi_panel_flips() - s_pr_flips0;
    const uint32_t wu = lvgl_mipi_panel_wait_us() - s_pr_wait0;
    const uint32_t px = lvgl_mipi_panel_flushed_px() - s_pr_px0;
    Serial1.printf("%s flips=%lu timeouts=%lu wait_us_tot=%lu px_tot=%lu"
                   " wait_us_pf=%lu px_pf=%lu\n", tag,
                   (unsigned long)fl,
                   (unsigned long)(lvgl_mipi_panel_vsync_timeouts() - s_pr_to0),
                   (unsigned long)wu, (unsigned long)px,
                   (unsigned long)(fl ? wu / fl : 0),
                   (unsigned long)(fl ? px / fl : 0));
}

void setup()
{
    Serial1.begin(115200);
    delay(200);
    Serial1.println("SYNTHUI_FADER_BEGIN");

    const bool ok = Display.begin();
    Serial1.println(ok ? "PANEL_OK" : "PANEL_FAIL");
    if (!ok) {
        /* Safe-but-only-just: with no lv_init(), lvgl_rt1176_loop() in
         * loop() returns immediately (the knob test's contract). */
        Serial1.println("crc_done");
        return;
    }
    Display.fillScreen(0x0000);

    lvgl_rt1176_begin();
    /* db pipeline: LVGL renders off-screen, the LCDIFv2 flips at vsync --
     * complete frames only, the tear-free-by-construction property the
     * checksums cannot see (spec section 1). */
    lvgl_mipi_panel_create_db(Display);

    Serial1.println("fd_scene=16 grid=2x8 size=78x210");

    /* Phase A1: the bank is the FIRST refresh, so LVGL_BYTES pins a
     * whole-screen paint. */
    for (int i = 0; i < 16; i++) g_vals[i] = (float)i / 15.0f;
    lv_screen_load(build_bank(g_vals, false));
    uint32_t t0 = millis();
    while (!lvgl_mipi_panel_frame_done() && (millis() - t0) < 5000)
        lvgl_rt1176_loop();
    Serial1.printf("LVGL_FLUSHED=%s\n",
                   lvgl_mipi_panel_frame_done() ? "PASS" : "FAIL");
    Serial1.printf("LVGL_BYTES=%lu\n",
                   (unsigned long)(lvgl_mipi_panel_flushed_px()
                                   * PANEL_BYTES_PER_PIXEL));
    Serial1.printf("fd_crc=0x%08lX\n", (unsigned long)sum_active_screen());
    eyeball_hold(1);

    /* Phase A2: delta sequence vs fresh full render.  The gate compares
     * the two sums -- gate-compared, never re-goldened. */
    const uint32_t d_seq = delta_run_sequence();
    eyeball_hold(2);
    const uint32_t d_full = sum_screen(build_bank(g_vals, true));
    eyeball_hold(3);
    Serial1.printf("fd_delta_crc=0x%08lX\n", (unsigned long)d_seq);
    Serial1.printf("fd_fresh_crc=0x%08lX\n", (unsigned long)d_full);
    Serial1.printf("fd_delta_eq=%s\n", d_seq == d_full ? "PASS" : "FAIL");
    Serial1.printf("fd_damage max=%ld total=%ld steps=%d\n",
                   (long)s_delta_maxarea, s_delta_total, FD_DELTA_STEPS);

    /* vsync-fence health (db mode): a timeout means the fence silently
     * degraded to unfenced behaviour -- gated in QEMU. */
    Serial1.printf("fd_vsync flips=%lu isrs=%lu timeouts=%lu\n",
                   (unsigned long)lvgl_mipi_panel_flips(),
                   (unsigned long)lvgl_mipi_panel_vsync_isrs(),
                   (unsigned long)lvgl_mipi_panel_vsync_timeouts());
    Serial1.println("crc_done");

    /* Phase B (ungated; silicon is where these numbers answer NEW-23).
     * A fresh bank so the animation starts from a known state. */
    lv_screen_load(build_bank(g_vals, false));
    lv_refr_now(NULL);
    probe_mark();
    fd_fps_phase("fd_fps", false, 60000u);
    probe_print("fd_probe_delta");
    probe_mark();
    fd_fps_phase("fd_fps_fullinv", true, 10000u);
    probe_print("fd_probe_fullinv");

    /* Phase C (NEW-23 fps diagnosis): scaling probes.  ONE fader animating
     * separates per-widget draw cost from per-refresh pipeline cost; a
     * minimum-tick bank separates per-primitive (draw task) cost from
     * per-pixel cost.  All ungated, all after crc_done. */
    g_anim_n = 1;
    lv_screen_load(build_bank(g_vals, false));
    lv_refr_now(NULL);
    probe_mark();
    fd_fps_phase("fd_fps_one", false, 15000u);
    probe_print("fd_probe_one");

    g_anim_n = 16;
    lv_screen_load(build_bank(g_vals, false));
    lv_refr_now(NULL);
    for (int i = 0; i < 16; i++) synthui_fader_set_ticks(g_fader[i], 2);
    lv_refr_now(NULL);
    probe_mark();
    fd_fps_phase("fd_fps_min", false, 15000u);
    probe_print("fd_probe_min");

    /* Pool health after every scene this image ever builds: an exhausted
     * lv_malloc pool would invalidate the phases above silently. */
    {
        lv_mem_monitor_t mm;
        lv_mem_monitor(&mm);
        Serial1.printf("fd_mem total=%lu used_pct=%u max_used=%lu frag_pct=%u\n",
                       (unsigned long)mm.total_size, (unsigned)mm.used_pct,
                       (unsigned long)mm.max_used, (unsigned)mm.frag_pct);
    }

    /* leave the bank animating for the eyes/camera pass + soak */
    g_anim_full_inv = false;
    lv_timer_create(fd_anim_cb, 15, NULL);
}

void loop()
{
    lvgl_rt1176_loop();
}
