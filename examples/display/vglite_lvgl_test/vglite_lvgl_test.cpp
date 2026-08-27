/* vglite_lvgl_test - LVGL's VG_LITE draw unit on the GC355, 4x4
 * synthui_rotary_knob grid on the RK055 (720x1280 XRGB8888).
 * (NEW-20 Phase 2 swapped the old synthui_knob for the rotary widget; rows
 * are now mode x theme because the old four visual modes no longer exist.
 * The widget's own direct-vg_lite compositor is deliberately NOT attached
 * here -- this example tests LVGL's OWN VG_LITE draw unit, and a second,
 * direct client in LVGL's command stream would be an uncontrolled mix.)
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * ★ TWO BUILDS, NOT ONE BINARY. An earlier draft of this file claimed a
 * runtime fallback and was WRONG, in a way worth recording because everything
 * reported success:
 *
 *   LVGL registers its draw units in lv_init() (lv_init.c:236, :300) and does
 *   so unconditionally when LV_USE_DRAW_VG_LITE is 1. On absent hardware the
 *   VG_LITE unit still registers and still CLAIMS draw tasks -- which then go
 *   nowhere, because vg_lite_init() was never called (it SPINS on absent
 *   hardware, so calling it is not an option). Measured under QEMU:
 *   LVGL_FLUSHED=PASS, LVGL_BYTES=3686400 -- a full-screen flush -- and a
 *   framebuffer whose checksum was exactly the FNV of all zeros. A black
 *   screen that passed every liveness check.
 *
 * So the path is chosen at BUILD time:
 *   build/         (EVKB_VGLITE=OFF)  software. The QEMU gate, and the fps
 *                                     baseline -- same scene, same toolchain.
 *   build-vglite/  (EVKB_VGLITE=ON)   GPU. Silicon only.
 *
 * ★ THE TWO DO NOT PRODUCE THE SAME PIXELS, by construction: hardware
 * antialiasing is not LVGL's mask arithmetic, and the GPU build carries
 * LV_USE_FLOAT=1 (required by LV_USE_MATRIX, which the backend needs) so
 * coordinates round differently. ONE GOLDEN PER BUILD, never reconciled.
 * Copying one over the other to green a gate throws away the only evidence
 * that the GPU is doing anything different.
 *
 * The scene is the 4x4 grid deliberately: it is the workload the Phase 1 spec
 * set the >=30 fps criterion against, so the fps variant (FPSBENCH) measures
 * the thing that was actually promised rather than a synthetic case.
 */
#include <Arduino.h>
#include "Display.h"
#include "lvgl_rt1176.h"
#include "lvgl_mipi_panel.h"
#include "synthui_rotary_knob.h"

#if LV_USE_DRAW_VG_LITE
extern "C" {
#include "vg_lite.h"
#include "vg_lite_platform.h"
}
#endif

/* Same pool siting and reasoning as vglite_probe: EXTMEM, not DMAMEM. OCRAM is
 * 512K and already spoken for, so a 2 MB pool there overflows the region at
 * link time; SDRAM at 0x80000000 is reachable by the GPU as a bus master
 * exactly as the framebuffer is. */
#if LV_USE_DRAW_VG_LITE
#define VGLITE_POOL_BYTES (2u * 1024u * 1024u)
EXTMEM __attribute__((aligned(64))) static uint8_t vglite_pool[VGLITE_POOL_BYTES];
#define TESS_W 256
#define TESS_H 256
#endif

#ifdef FPSBENCH
/* Task-9 measurement variant (-DFPSBENCH, separate build dir; never a
 * golden). Times FPSBENCH_N full-scene repaints of the 16-knob grid --
 * lv_obj_invalidate() then lv_refr_now() bracketed by micros() -- the same
 * method Phase 1 used for its software figure. Results land in a RAM array
 * read over SWD (the bench VCOM cannot be assumed) and go to Serial1 too. */
#define FPSBENCH_N 64
extern "C" {
volatile uint32_t fpsbench_us[FPSBENCH_N];   /* per-refresh render+flush time */
volatile uint32_t fpsbench_loops[FPSBENCH_N]; /* lv_timer_handler passes/refresh */
volatile uint32_t fpsbench_loopct = 0;       /* incremented by loop() */
volatile uint32_t fpsbench_i = 0;            /* refreshes completed */
volatile uint32_t fpsbench_done = 0;
}
static lv_obj_t *fpsbench_knob[16];
static uint32_t  fpsbench_t0;

/* ★ Measured in the NORMAL operating mode -- loop() free-running, an lv_timer
 * rotating every knob's angle (each set_angle invalidates its knob, so every
 * refresh carries all-16 damage: the workload the >=30 fps criterion names) --
 * instrumented from LVGL's own REFR_START/REFR_READY display events. Two
 * earlier drafts drove lv_refr_now() from a bench loop, full-screen
 * invalidate and animated damage alike, and BOTH LIVELOCKED around the third
 * repaint (software: pinned on one SDRAM strb the DAP could not read either;
 * GPU: cycling draw-task/heap code forever, GPU idle, LVGL log EMPTY). The
 * free-running mode is the one every long-lived build demonstrably sustains;
 * repeated setup-context lv_refr_now() is not, and is not the promised
 * workload either. Not chased further. */
static void fpsbench_anim_cb(lv_timer_t *t)
{
    (void)t;
    static uint32_t step = 0;
    step++;
    for (int k = 0; k < 16; k++)
        /* (k%4)*70-105 = col_angle[k%4]; declared later in the file */
        synthui_rotary_knob_set_angle(fpsbench_knob[k],
                               (float)((k % 4) * 70 - 105)
                               + (float)((step * 7u) % 90u));
}
static uint32_t fpsbench_loop0;
static void fpsbench_refr_cb(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_REFR_START) {
        fpsbench_t0 = micros();
        fpsbench_loop0 = fpsbench_loopct;
    }
    else if (code == LV_EVENT_REFR_READY && !fpsbench_done) {
        const uint32_t i = fpsbench_i;
        if (i < FPSBENCH_N) {
            fpsbench_us[i] = micros() - fpsbench_t0;
            fpsbench_loops[i] = fpsbench_loopct - fpsbench_loop0;
            fpsbench_i = i + 1;
        }
        if (i + 1 >= FPSBENCH_N) {
            uint32_t worst = 0, sum = 0;
            for (uint32_t n = 0; n < FPSBENCH_N; n++) {
                sum += fpsbench_us[n];
                if (fpsbench_us[n] > worst) worst = fpsbench_us[n];
            }
            Serial1.printf("FPSBENCH_N=%u\n", (unsigned)FPSBENCH_N);
            Serial1.printf("FPSBENCH_MEAN_US=%lu\n",
                           (unsigned long)(sum / FPSBENCH_N));
            Serial1.printf("FPSBENCH_WORST_US=%lu\n", (unsigned long)worst);
            fpsbench_done = 1;
        }
    }
}
static void fpsbench_arm(void)
{
    lv_display_add_event_cb(lv_display_get_default(), fpsbench_refr_cb,
                            LV_EVENT_REFR_START, NULL);
    lv_display_add_event_cb(lv_display_get_default(), fpsbench_refr_cb,
                            LV_EVENT_REFR_READY, NULL);
    lv_timer_create(fpsbench_anim_cb, 15, NULL);
}
#endif

#if LV_USE_LOG
/* Diagnostic builds only (-DLV_USE_LOG=1): capture LVGL's log stream into a
 * RAM ring readable over SWD, because the backend explains every skipped or
 * failed draw via LV_LOG_WARN/ERROR and the bench VCOM cannot be assumed.
 * Read it with:  gdb ... -ex 'x/s swd_log'  (or dump SWD_LOG_BYTES at the
 * symbol). swd_log_pos never wraps; the tail is simply dropped -- a bounded
 * diagnostic beats a clever one. */
#define SWD_LOG_BYTES 8192
extern "C" {
volatile uint32_t swd_log_pos = 0;
char swd_log[SWD_LOG_BYTES];
}
static void swd_log_cb(lv_log_level_t level, const char *buf)
{
    (void)level;
    uint32_t p = swd_log_pos;
    while (*buf && p < SWD_LOG_BYTES - 1) swd_log[p++] = *buf++;
    if (p < SWD_LOG_BYTES - 1) swd_log[p++] = '\n';
    swd_log[p] = '\0';
    swd_log_pos = p;
}
#endif

static const float               col_angle[4] = { -105.0f, -35.0f, 35.0f, 105.0f };
static const lv_state_t          col_state[4] = { LV_STATE_DEFAULT, LV_STATE_PRESSED,
                                                  LV_STATE_FOCUSED, LV_STATE_DISABLED };
static const synthui_rotary_mode_t  row_mode[4]  = {
    SYNTHUI_ROTARY_MODE_ENDLESS, SYNTHUI_ROTARY_MODE_BOUNDED,
    SYNTHUI_ROTARY_MODE_ENDLESS, SYNTHUI_ROTARY_MODE_BOUNDED };
static const synthui_rotary_theme_t row_theme[4] = {
    SYNTHUI_ROTARY_THEME_LIGHT, SYNTHUI_ROTARY_THEME_LIGHT,
    SYNTHUI_ROTARY_THEME_DARK,  SYNTHUI_ROTARY_THEME_DARK };

static bool s_gpu = false;

#ifdef GRADPROBE
/* Diagnostic scene, built only when -DGRADPROBE (separate build dir, never
 * a golden). Isolates the knob face's ingredients after the 4x4 scene showed
 * the GPU painting every face a flat out-of-gamut colour: each tile is ONE
 * primitive with known colors at a known place, so a framebuffer dump turns
 * "the face looks wrong" into per-primitive arithmetic. Tiles, top to
 * bottom at x=285, 150x150, 160 pitch:
 *   0 rect, vertical gradient FCFBF6->E7E7F1 (the exact face gradient)
 *   1 + radius=CIRCLE
 *   2 circle, SOLID FCFBF6 (no gradient)
 *   3 circle, gradient + border (the full face recipe)
 *   4 solid MAGENTA rect + centred FDFDF9@140 disc (the cap over known dst)
 *   5 rect, vertical gradient FF0000->0000FF (saturated signature)
 *   6 rect, vertical gradient 000000->FFFFFF (ramp shape)
 *   7 rect, HORIZONTAL gradient FF0000->0000FF (direction check) */
static lv_obj_t *probe_tile(lv_obj_t *scr, int i, uint32_t c0, uint32_t c1,
                            lv_grad_dir_t dir, int32_t radius, int32_t border)
{
    lv_obj_t *o = lv_obj_create(scr);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, 150, 150);
    lv_obj_set_pos(o, 285, 5 + i * 160);
    lv_obj_set_style_radius(o, radius, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(o, lv_color_hex(c0), LV_PART_MAIN);
    if (dir != LV_GRAD_DIR_NONE) {
        lv_obj_set_style_bg_grad_color(o, lv_color_hex(c1), LV_PART_MAIN);
        lv_obj_set_style_bg_grad_dir(o, dir, LV_PART_MAIN);
    }
    if (border > 0) {
        lv_obj_set_style_border_width(o, border, LV_PART_MAIN);
        lv_obj_set_style_border_color(o, lv_color_hex(0x2b2e5c), LV_PART_MAIN);
        lv_obj_set_style_border_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    }
    return o;
}

static lv_obj_t *build_grid(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    probe_tile(scr, 0, 0xFCFBF6, 0xE7E7F1, LV_GRAD_DIR_VER, 0, 0);
    probe_tile(scr, 1, 0xFCFBF6, 0xE7E7F1, LV_GRAD_DIR_VER, LV_RADIUS_CIRCLE, 0);
    probe_tile(scr, 2, 0xFCFBF6, 0,        LV_GRAD_DIR_NONE, LV_RADIUS_CIRCLE, 0);
    probe_tile(scr, 3, 0xFCFBF6, 0xE7E7F1, LV_GRAD_DIR_VER, LV_RADIUS_CIRCLE, 4);
    lv_obj_t *dst = probe_tile(scr, 4, 0xFF00FF, 0, LV_GRAD_DIR_NONE, 0, 0);
    {
        lv_obj_t *cap = lv_obj_create(dst);
        lv_obj_remove_style_all(cap);
        lv_obj_set_size(cap, 100, 100);
        lv_obj_center(cap);
        lv_obj_set_style_radius(cap, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(cap, lv_color_hex(0xFDFDF9), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(cap, 140, LV_PART_MAIN);
    }
    probe_tile(scr, 5, 0xFF0000, 0x0000FF, LV_GRAD_DIR_VER, 0, 0);
    probe_tile(scr, 6, 0x000000, 0xFFFFFF, LV_GRAD_DIR_VER, 0, 0);
    probe_tile(scr, 7, 0xFF0000, 0x0000FF, LV_GRAD_DIR_HOR, 0, 0);
    return scr;
}

#else /* !GRADPROBE -- the real scene, the one goldens are recorded against */

static lv_obj_t *build_grid(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    /* Opaque ground forces LVGL to paint every pixel, so a frame is fully
     * defined and its checksum means something. */
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "SynthUI RotaryKnob / VGLite");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            lv_obj_t *k = synthui_rotary_knob_create(scr);
            lv_obj_set_size(k, 150, 150);
            synthui_rotary_knob_set_mode(k, row_mode[r]);
            synthui_rotary_knob_set_theme(k, row_theme[r]);
            synthui_rotary_knob_set_angle(k, col_angle[c]);
            if (col_state[c] != LV_STATE_DEFAULT) lv_obj_add_state(k, col_state[c]);
            lv_obj_set_pos(k, 15 + c * 175, 120 + r * 175);
#ifdef FPSBENCH
            fpsbench_knob[r * 4 + c] = k;
#endif
        }
    }
    return scr;
}

#endif /* GRADPROBE */

void setup()
{
    Serial1.begin(115200);
    delay(200);
    Serial1.println("VGLITE_LVGL_BEGIN");

    const bool ok = Display.begin();
    Serial1.println(ok ? "PANEL_OK" : "PANEL_FAIL");
    if (!ok) {
        Serial1.println("VGLITE_LVGL_DONE");
        return;
    }
    Display.fillScreen(0x0000);

#if LV_USE_DRAW_VG_LITE
    /* ★ ASK BEFORE COMMITTING. vg_lite_init() SPINS on absent hardware rather
     * than returning an error, so the chip-ID probe is what keeps this build
     * safe to boot anywhere -- see vglite_probe's transcript. But note it is
     * NOT a fallback: if the GPU is missing here the scene renders BLACK,
     * because LVGL has already registered the VG_LITE draw unit in lv_init()
     * and that unit claims tasks it cannot execute. That is why the software
     * path is a separate BUILD, not a runtime branch. */
    vg_lite_init_mem(VGLITE_RT1176_REGISTER_BASE, 0u, vglite_pool, VGLITE_POOL_BYTES);
    const uint32_t chip_id = vg_lite_hal_probe_chip_id();
    Serial1.printf("VGLITE_CHIP_ID=0x%08lX\n", (unsigned long)chip_id);
    if (chip_id != 0u) {
        const vg_lite_error_t err = vg_lite_init(TESS_W, TESS_H);
        s_gpu = (err == VG_LITE_SUCCESS);
        Serial1.printf("VGLITE_INIT=%s err=%d\n", s_gpu ? "OK" : "FAIL", (int)err);
        /* ★ A mismatch is self-diagnosing: vg_lite_init() compares
         * CHIPID/REVISION/CID/ECOID against the silicon and prints BOTH sides
         * before returning VG_LITE_NOT_SUPPORT. Read the lines above and set
         * EVKB_VGLITE_SERIES accordingly. */
    } else {
        Serial1.println("VGLITE_INIT=ABSENT err=0 reason=no_chip_id");
        Serial1.println("VGLITE_LVGL_NOGPU=FATAL");   /* the gate must not see this pass */
    }
#else
    Serial1.println("VGLITE_CHIP_ID=0xNOTBUILT");
    Serial1.println("VGLITE_INIT=NOTBUILT err=0 reason=software_build");
#endif
    Serial1.printf("VGLITE_LVGL=%s\n", s_gpu ? "GPU" : "SOFTWARE");

    lvgl_rt1176_begin();
#if LV_USE_LOG
    lv_log_register_print_cb(swd_log_cb);
#endif
    lvgl_mipi_panel_create(Display);

    lv_screen_load(build_grid());
    uint32_t t0 = millis();
    while (!lvgl_mipi_panel_frame_done() && (millis() - t0) < 5000)
        lvgl_rt1176_loop();

    lvgl_sum_reset();
    lvgl_sum_feed(Display.framebuffer(), PANEL_FB_BYTES);
    Serial1.printf("LVGL_FLUSHED=%s\n", lvgl_mipi_panel_frame_done() ? "PASS" : "FAIL");
    Serial1.printf("LVGL_BYTES=%lu\n",
                   (unsigned long)(lvgl_mipi_panel_flushed_px() * PANEL_BYTES_PER_PIXEL));
    /* Named by PATH, so the two goldens can never be confused for each other
     * in a transcript or a diff. */
    Serial1.printf("KNOB_GRID_SUM_%s=0x%08lX\n", s_gpu ? "GPU" : "SW",
                   (unsigned long)lvgl_sum_value());
#ifdef FPSBENCH
    fpsbench_arm();     /* measurement happens in loop(), the normal mode */
#endif
    Serial1.println("VGLITE_LVGL_DONE");
}

void loop()
{
#ifdef FPSBENCH
    fpsbench_loopct++;
#endif
    lvgl_rt1176_loop();
}
