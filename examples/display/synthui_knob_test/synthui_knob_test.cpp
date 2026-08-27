/* synthui_knob_test - synthui_rotary_knob (NEW-20 Phase 2) on the RK055
 * (720x1280 XRGB8888, DIRECT render), checksummed per feature axis.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * ONE ELF, BOTH ENGINES (rotary_knob_bench's pattern): LVGL is always the
 * software renderer; the GC355 is reached only by the widget's opt-in
 * compositor behind the chip-ID probe. QEMU has no GC355, so there
 * rk_engine=sw and the six goldens below are the software renderer's --
 * asserted by the gate. On silicon rk_engine=gpu and the SAME tokens carry
 * the GPU sums, recorded in transcript_hw_evkb.txt (two golden sets, never
 * reconciled -- hardware AA is not LVGL mask arithmetic).
 *
 * Scene order (all tokens before anything animates -- goldens deterministic):
 *   1. 4x4 grid: rows {endless/light, bounded/light, endless/dark,
 *      bounded/dark} x cols states {idle, active, focus, disabled}.
 *      PANEL_OK, RK_CHIP_ID, rk_engine, LVGL_FLUSHED, LVGL_BYTES,
 *      KNOB_SUM_ALL.
 *   2. One screen per row config: KNOB_SUM_{ENDLESS,BOUNDED}_{LIGHT,DARK}.
 *   3. Accent screen (the DC's four accent options): KNOB_SUM_ACCENT.
 *   4. rk_gpu_err (gpu only -- the gate tripwires on it appearing in QEMU),
 *      SYNTHUI_KNOB_DONE, then the hero spin (eyes-on-glass only, signed
 *      angles, never checksummed).
 */
#include <Arduino.h>
#include "Display.h"
#include "lvgl_rt1176.h"
#include "lvgl_mipi_panel.h"
#include "synthui_rotary_knob.h"
#include "synthui_rotary_knob_gpu.h"

extern "C" {
#include "vg_lite.h"
#include "vg_lite_platform.h"
}

/* Same pool siting and reasoning as the bench: EXTMEM (SDRAM), not DMAMEM --
 * a 2 MB pool overflows the 512K OCRAM at link time, and the GPU reaches
 * SDRAM as a bus master exactly as it reaches the framebuffer. */
#define VGLITE_POOL_BYTES (2u * 1024u * 1024u)
EXTMEM __attribute__((aligned(64))) static uint8_t vglite_pool[VGLITE_POOL_BYTES];
#define TESS_W 256
#define TESS_H 256

static bool s_gpu = false;

static const float      col_angle[4] = { -105.0f, -35.0f, 35.0f, 105.0f };
static const lv_state_t col_state[4] = { LV_STATE_DEFAULT, LV_STATE_PRESSED,
                                         LV_STATE_FOCUSED, LV_STATE_DISABLED };
static const synthui_rotary_mode_t  row_mode[4]  = {
    SYNTHUI_ROTARY_MODE_ENDLESS, SYNTHUI_ROTARY_MODE_BOUNDED,
    SYNTHUI_ROTARY_MODE_ENDLESS, SYNTHUI_ROTARY_MODE_BOUNDED };
static const synthui_rotary_theme_t row_theme[4] = {
    SYNTHUI_ROTARY_THEME_LIGHT, SYNTHUI_ROTARY_THEME_LIGHT,
    SYNTHUI_ROTARY_THEME_DARK,  SYNTHUI_ROTARY_THEME_DARK };
static const char *row_name[4] = { "ENDLESS_LIGHT", "BOUNDED_LIGHT",
                                   "ENDLESS_DARK",  "BOUNDED_DARK" };
/* The DC accent options (first = unset: the default-index path is a feature) */
static const uint32_t accent_opt[4] = { SYNTHUI_ROTARY_ACCENT_DEFAULT,
                                        0xffd24a, 0x5be0a0, 0xff6a52 };

static lv_obj_t *hero;

static void opaque_bg(lv_obj_t *scr)
{   /* Opaque ground forces LVGL to paint every pixel: fully-defined frames. */
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
}

static lv_obj_t *make_knob(lv_obj_t *parent, synthui_rotary_mode_t m,
                           synthui_rotary_theme_t th, lv_state_t st,
                           float angle, int32_t size)
{
    lv_obj_t *k = synthui_rotary_knob_create(parent);
    lv_obj_set_size(k, size, size);
    synthui_rotary_knob_set_mode(k, m);
    synthui_rotary_knob_set_theme(k, th);
    synthui_rotary_knob_set_angle(k, angle);
    if (st != LV_STATE_DEFAULT) lv_obj_add_state(k, st);
    return k;
}

static lv_obj_t *build_grid(void)
{
    lv_obj_t *scr = lv_obj_create(NULL); opaque_bg(scr);
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "SynthUI RotaryKnob");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            lv_obj_t *k = make_knob(scr, row_mode[r], row_theme[r],
                                    col_state[c], col_angle[c], 150);
            lv_obj_set_pos(k, 15 + c * 175, 120 + r * 175);
        }
    return scr;
}

static lv_obj_t *build_row_screen(int r)
{
    lv_obj_t *scr = lv_obj_create(NULL); opaque_bg(scr);
    lv_obj_t *lbl = lv_label_create(scr);
    lv_label_set_text(lbl, row_name[r]);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x9FD4FF), LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 460);
    for (int c = 0; c < 4; c++) {
        lv_obj_t *k = make_knob(scr, row_mode[r], row_theme[r], col_state[c],
                                col_angle[c], 150);
        lv_obj_set_pos(k, 15 + c * 175, 560);
    }
    return scr;
}

static lv_obj_t *build_accent_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL); opaque_bg(scr);
    lv_obj_t *lbl = lv_label_create(scr);
    lv_label_set_text(lbl, "ACCENT");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x9FD4FF), LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 460);
    for (int c = 0; c < 4; c++) {
        lv_obj_t *k = make_knob(scr, SYNTHUI_ROTARY_MODE_ENDLESS,
                                SYNTHUI_ROTARY_THEME_LIGHT, LV_STATE_DEFAULT,
                                col_angle[c], 150);
        synthui_rotary_knob_set_accent(k, accent_opt[c]);
        lv_obj_set_pos(k, 15 + c * 175, 560);
    }
    return scr;
}

/* Load a screen, render it synchronously, checksum the whole framebuffer.
 * With the compositor attached, RENDER_READY fires INSIDE lv_refr_now() and
 * vg_lite_finish() retires before it returns, so the sum always includes the
 * rotors. */
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
         * returns immediately.  Same contract as lvgl_rk055_panel_test. */
        Serial1.println("SYNTHUI_KNOB_DONE");
        return;
    }
    Display.fillScreen(0x0000);

    /* ASK BEFORE COMMITTING (vglite_probe): vg_lite_init() SPINS on absent
     * hardware, so the chip-ID read is what makes QEMU a clean negative. */
    vg_lite_init_mem(VGLITE_RT1176_REGISTER_BASE, 0u, vglite_pool,
                     VGLITE_POOL_BYTES);
    const uint32_t chip_id = vg_lite_hal_probe_chip_id();
    Serial1.printf("RK_CHIP_ID=0x%08lX\n", (unsigned long)chip_id);
    const bool vg_up =
        (chip_id != 0u) && (vg_lite_init(TESS_W, TESS_H) == VG_LITE_SUCCESS);

    lvgl_rt1176_begin();
    lvgl_mipi_panel_create(Display);

    /* Attach the compositor only when the GPU is genuinely up; a false here
     * leaves the widget fully software -- the honest negative the gate pins. */
    if (vg_up)
        s_gpu = synthui_rotary_gpu_begin(Display.framebuffer(),
                                         Display.width(), Display.height(),
                                         Display.width() * PANEL_BYTES_PER_PIXEL);
    Serial1.printf("rk_engine=%s\n", s_gpu ? "gpu" : "sw");

    /* Phase 1: grid is the FIRST refresh, so LVGL_BYTES pins a whole-screen
     * paint (same contract as before the rotary rewrite). */
    lv_screen_load(build_grid());
    uint32_t t0 = millis();
    while (!lvgl_mipi_panel_frame_done() && (millis() - t0) < 5000)
        lvgl_rt1176_loop();
    lvgl_sum_reset();
    lvgl_sum_feed(Display.framebuffer(), PANEL_FB_BYTES);
    Serial1.printf("LVGL_FLUSHED=%s\n",
                   lvgl_mipi_panel_frame_done() ? "PASS" : "FAIL");
    Serial1.printf("LVGL_BYTES=%lu\n",
                   (unsigned long)(lvgl_mipi_panel_flushed_px() * PANEL_BYTES_PER_PIXEL));
    Serial1.printf("KNOB_SUM_ALL=0x%08lX\n", (unsigned long)lvgl_sum_value());

    /* Phase 2: one golden per feature axis (the acid-bass lesson: a single
     * aggregate sum can freeze half a feature without changing color). */
    for (int r = 0; r < 4; r++)
        Serial1.printf("KNOB_SUM_%s=0x%08lX\n", row_name[r],
                       (unsigned long)sum_screen(build_row_screen(r)));
    Serial1.printf("KNOB_SUM_ACCENT=0x%08lX\n",
                   (unsigned long)sum_screen(build_accent_screen()));

    /* gpu lines NEVER appear in a sw run -- the gate tripwires on them. */
    if (s_gpu)
        Serial1.printf("rk_gpu_err=%lu\n",
                       (unsigned long)synthui_rotary_gpu_errors());
    Serial1.println("SYNTHUI_KNOB_DONE");

    /* Phase 3: hero spin, glass-only, after every token. */
    lv_obj_t *scr = lv_obj_create(NULL); opaque_bg(scr);
    hero = make_knob(scr, SYNTHUI_ROTARY_MODE_ENDLESS,
                     SYNTHUI_ROTARY_THEME_LIGHT, LV_STATE_DEFAULT, 0.0f, 360);
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
        /* signed angles on purpose: glass-only coverage of the negative-angle
         * fold in both engines (sw arc fold; gpu rotate takes it natively). */
        a += 1.8f; if (a >= 360.0f) a -= 720.0f;
        synthui_rotary_knob_set_angle(hero, a);
    }
    lvgl_rt1176_loop();
}
