/* vglite_lvgl_test - LVGL's VG_LITE draw unit on the GC355, 4x4 synthui_knob
 * grid on the RK055 (720x1280 XRGB8888).
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * ONE BINARY, TWO PATHS, and the whole point of the example is that it takes
 * whichever is real:
 *   - Silicon: the GC355 answers its chip ID, vg_lite_init() succeeds, and
 *     LVGL renders through VG_LITE. VGLITE_LVGL=GPU.
 *   - QEMU: there is no GC355 model, the chip-ID probe reads 0, init is never
 *     attempted, and LVGL falls back to its software renderer.
 *     VGLITE_LVGL=SOFTWARE.
 *
 * ★ THE TWO PATHS DO NOT PRODUCE THE SAME PIXELS, and that is expected rather
 * than a defect: hardware antialiasing is not LVGL's mask arithmetic, and this
 * build also runs with LV_USE_FLOAT=1 (required by LV_USE_MATRIX, which the
 * backend needs), so coordinates round differently from every software gate in
 * the tree. The gate therefore records ONE golden PER PATH and never
 * reconciles them. Copying one over the other to make a gate green would throw
 * away the only evidence that the GPU is doing something different.
 *
 * The scene is the 4x4 grid deliberately: it is the workload the Phase 1 spec
 * set the >=30 fps criterion against, so the fps variant (FPSBENCH) measures
 * the thing that was actually promised rather than a synthetic case.
 */
#include <Arduino.h>
#include "Display.h"
#include "lvgl_rt1176.h"
#include "lvgl_mipi_panel.h"
#include "synthui_knob.h"

extern "C" {
#include "vg_lite.h"
#include "vg_lite_platform.h"
}

/* Same pool siting and reasoning as vglite_probe: EXTMEM, not DMAMEM. OCRAM is
 * 512K and already spoken for, so a 2 MB pool there overflows the region at
 * link time; SDRAM at 0x80000000 is reachable by the GPU as a bus master
 * exactly as the framebuffer is. */
#define VGLITE_POOL_BYTES (2u * 1024u * 1024u)
EXTMEM __attribute__((aligned(64))) static uint8_t vglite_pool[VGLITE_POOL_BYTES];
#define TESS_W 256
#define TESS_H 256

static const float               col_angle[4] = { -105.0f, -35.0f, 35.0f, 105.0f };
static const lv_state_t          col_state[4] = { LV_STATE_DEFAULT, LV_STATE_PRESSED,
                                                  LV_STATE_FOCUSED, LV_STATE_DISABLED };
static const synthui_knob_mode_t row_mode[4]  = { SYNTHUI_KNOB_MODE_ENDLESS,
                                                  SYNTHUI_KNOB_MODE_BOUNDED,
                                                  SYNTHUI_KNOB_MODE_DETENTS,
                                                  SYNTHUI_KNOB_MODE_ARC };

static bool s_gpu = false;

static lv_obj_t *build_grid(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    /* Opaque ground forces LVGL to paint every pixel, so a frame is fully
     * defined and its checksum means something. */
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "SynthUI Knob / VGLite");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            lv_obj_t *k = synthui_knob_create(scr);
            lv_obj_set_size(k, 150, 150);
            synthui_knob_set_mode(k, row_mode[r]);
            synthui_knob_set_angle(k, col_angle[c]);
            if (col_state[c] != LV_STATE_DEFAULT) lv_obj_add_state(k, col_state[c]);
            lv_obj_set_pos(k, 15 + c * 175, 120 + r * 175);
        }
    }
    return scr;
}

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

    /* ★ ASK BEFORE COMMITTING. vg_lite_init() SPINS on absent hardware rather
     * than returning an error, so the chip-ID probe is what makes one binary
     * safe on both paths -- see vglite_probe's transcript. */
    vg_lite_init_mem(VGLITE_RT1176_REGISTER_BASE, 0u, vglite_pool, VGLITE_POOL_BYTES);
    const uint32_t chip_id = vg_lite_hal_probe_chip_id();
    Serial1.printf("VGLITE_CHIP_ID=0x%08lX\n", (unsigned long)chip_id);

    if (chip_id != 0u) {
        const vg_lite_error_t err = vg_lite_init(TESS_W, TESS_H);
        s_gpu = (err == VG_LITE_SUCCESS);
        Serial1.printf("VGLITE_INIT=%s err=%d\n", s_gpu ? "OK" : "FAIL", (int)err);
        /* ★ A mismatch here is self-diagnosing: vg_lite_init() compares
         * CHIPID/REVISION/CID/ECOID against the silicon and prints BOTH sides
         * before returning VG_LITE_NOT_SUPPORT. If this says FAIL, read the
         * lines above it and set EVKB_VGLITE_SERIES accordingly. */
    } else {
        Serial1.println("VGLITE_INIT=ABSENT err=0 reason=no_chip_id");
    }
    Serial1.printf("VGLITE_LVGL=%s\n", s_gpu ? "GPU" : "SOFTWARE");

    lvgl_rt1176_begin();
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
    Serial1.println("VGLITE_LVGL_DONE");
}

void loop()
{
    lvgl_rt1176_loop();
}
