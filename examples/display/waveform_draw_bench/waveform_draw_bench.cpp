/* waveform_draw_bench - how to draw a full-width scrolling waveform lane on
 * the landscape (rotated present) pipeline.  Bench only, run on the board.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * A 1104x336 lane at (8,48) on the 1280x720 canvas scrolls 16 px per frame.
 * The core runs with the D-cache off, so CPU writes to SDRAM are slow and a
 * lane redrawn in full by the CPU every frame is the thing to avoid.
 *
 *   baseline  an 8x8 square moves; nothing else changes: the pipeline's floor
 *   A         plain LVGL: the lane is invalidated and redrawn with one
 *             lv_draw_rect per column
 *   C         the widget keeps the lane in its own strip (a ring in x: only
 *             the 16 new columns are drawn by the CPU) and copies it into the
 *             canvas with two PXP blits from the pre-flip hook; the port then
 *             rotates the lane into the scanout buffer as usual
 *   D-est     as C, but the strip is PXP-ROTATED straight to its place (here:
 *             into a scratch buffer of the same size), and the lane is not
 *             invalidated.  This is the cost of composing into the scanout
 *             buffer directly, which would need a new hook in the LVGL port.
 *
 * Each arm prints  ARM=<name> frames=<n> ms=<total> fps_x10=<n> max_frame_ms=<n>
 *                  rot_us_per_frame=<n> pxp_err=<n>
 */
#include <Arduino.h>
#include <string.h>
#include "Display.h"
#include "PXP.h"
#include "lvgl_rt1176.h"
#include "lvgl_mipi_panel.h"

static const int LX = 8, LY = 48, LW = 1104, LH = 336, STEP = 16, FRAMES = 300;
static const int CANVAS_W = 1280;
static const uint32_t BG = 0xFF0E141Bu, WAVE = 0xFFB9CFE0u;

static uint32_t *strip;        /* LW x LH, ring in x */
static uint32_t *scratch;      /* LH x LW, D-est's rotated target */
static int ring0;              /* strip column that is the lane's x = 0 */
static uint32_t gcol;          /* global index of the lane's leftmost column */
static uint32_t pxp_err;
static enum { ARM_BASE, ARM_A, ARM_C, ARM_D } arm;
static lv_obj_t *lane, *square;

/* deterministic "waveform": min/max in [-128,127] for global column g */
static void col_extent(uint32_t g, int &y0, int &y1) {
    const uint32_t ph = g % 96u;                        /* a beat every 96 columns */
    int a = 120 - (int)ph * 2; if (a < 12) a = 12;
    a = a * (int)(60u + (g * 2654435761u >> 26)) / 124; /* some per-column variation */
    y0 = LH / 2 - a * LH / 256; y1 = LH / 2 + a * LH / 256;
}
static void strip_col(int sx, uint32_t g) {
    int y0, y1; col_extent(g, y0, y1);
    uint32_t *p = strip + sx;
    for (int y = 0; y < LH; y++, p += LW) *p = (y >= y0 && y <= y1) ? WAVE : BG;
}
static void strip_fill() { for (int x = 0; x < LW; x++) strip_col((ring0 + x) % LW, gcol + x); }
static void strip_scroll() {                           /* STEP new columns enter on the right */
    for (int i = 0; i < STEP; i++) strip_col((ring0 + i) % LW, gcol + LW + i);
    ring0 = (ring0 + STEP) % LW; gcol += STEP;
}

static void pxp_run(PXPOp op) { if (op.run() != PXP_OK) pxp_err++; }

static void preflip(uint8_t *canvas) {
    if (arm != ARM_C && arm != ARM_D) return;
    const int w1 = LW - ring0, w2 = ring0;             /* strip [ring0,LW) then [0,ring0) */
    if (arm == ARM_C) {
        uint32_t *dst = (uint32_t *)canvas + LY * CANVAS_W + LX;
        PXPSurface s1(strip + ring0, w1, LH, PXP_XRGB8888, LW * 4), d1(dst, w1, LH, PXP_XRGB8888, CANVAS_W * 4);
        pxp_run(PXP.op().source(s1).output(d1));
        if (w2) {
            PXPSurface s2(strip, w2, LH, PXP_XRGB8888, LW * 4), d2(dst + w1, w2, LH, PXP_XRGB8888, CANVAS_W * 4);
            pxp_run(PXP.op().source(s2).output(d2));
        }
    } else {
        /* rotated extent of a w x LH source is LH x w; stack the two halves down the scratch */
        PXPSurface s1(strip + ring0, w1, LH, PXP_XRGB8888, LW * 4), d1(scratch, LH, w1, PXP_XRGB8888, LH * 4);
        pxp_run(PXP.op().source(s1).output(d1).rotate(PXP_ROT_90));
        if (w2) {
            PXPSurface s2(strip, w2, LH, PXP_XRGB8888, LW * 4), d2(scratch + (uint32_t)w1 * LH, LH, w2, PXP_XRGB8888, LH * 4);
            pxp_run(PXP.op().source(s2).output(d2).rotate(PXP_ROT_90));
        }
    }
}

static void lane_event(lv_event_t *e) {
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_COVER_CHECK) {                /* opaque: LVGL need not draw what is under the lane */
        lv_event_set_cover_res(e, LV_COVER_RES_COVER);
        return;
    }
    if (code != LV_EVENT_DRAW_MAIN || arm != ARM_A) return;
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
    d.bg_opa = LV_OPA_COVER; d.bg_color = lv_color_hex(BG & 0xFFFFFF);
    lv_area_t a = { LX, LY, LX + LW - 1, LY + LH - 1 };
    lv_draw_rect(layer, &d, &a);
    d.bg_color = lv_color_hex(WAVE & 0xFFFFFF);
    for (int x = 0; x < LW; x++) {
        int y0, y1; col_extent(gcol + x, y0, y1);
        lv_area_t c = { LX + x, LY + y0, LX + x, LY + y1 };
        lv_draw_rect(layer, &d, &c);
    }
}

static void run_arm(const char *name, int which) {
    arm = (decltype(arm))which;
    ring0 = 0; gcol = 0; pxp_err = 0;
    strip_fill();
    lv_obj_invalidate(lv_screen_active());
    for (int i = 0; i < 4; i++) { const uint32_t f = lvgl_mipi_panel_flips(); const uint32_t t = millis();
        lv_obj_invalidate(lv_screen_active());
        while (lvgl_mipi_panel_flips() == f && millis() - t < 2000) lvgl_rt1176_loop(); }
    const uint32_t rot0 = lvgl_mipi_panel_rot_us();
    const uint32_t t0 = millis();
    uint32_t worst = 0;
    int done = 0;
    for (int i = 0; i < FRAMES; i++) {
        const uint32_t tf = millis(), f = lvgl_mipi_panel_flips();
        if (arm == ARM_C || arm == ARM_D) strip_scroll(); else gcol += STEP;
        if (arm == ARM_A || arm == ARM_C) lv_obj_invalidate(lane);
        else lv_obj_set_pos(square, 600 + (i % 40) * 8, 420);
        while (lvgl_mipi_panel_flips() == f && millis() - tf < 2000) lvgl_rt1176_loop();
        if (lvgl_mipi_panel_flips() != f) done++;
        const uint32_t dt = millis() - tf; if (dt > worst) worst = dt;
    }
    const uint32_t ms = millis() - t0;
    Serial1.printf("ARM=%s frames=%d ms=%lu fps_x10=%lu max_frame_ms=%lu rot_us_per_frame=%lu pxp_err=%lu rot_err=%lu\n",
                   name, done, (unsigned long)ms, (unsigned long)(ms ? (uint32_t)done * 10000u / ms : 0),
                   (unsigned long)worst, (unsigned long)((lvgl_mipi_panel_rot_us() - rot0) / (uint32_t)FRAMES),
                   (unsigned long)pxp_err, (unsigned long)lvgl_mipi_panel_rot_errors());
}

void setup() {
    Serial1.begin(115200);
    while (!Serial1 && millis() < 2000) {}
    Serial1.println("=== BOOT ===");
    Serial1.printf("[APP: %s] [VER: v%u] [BUILD: %s %s]\n", "waveform_draw_bench", 1, __DATE__, __TIME__);
    if (!Display.begin()) { Serial1.println("PANEL_FAIL"); Serial1.println("BENCH_DONE"); return; }
    Serial1.println("PANEL_OK");
    strip = (uint32_t *)extmem_malloc((size_t)LW * LH * 4 + 64);
    scratch = (uint32_t *)extmem_malloc((size_t)LW * LH * 4 + 64);
    if (!strip || !scratch || !PXP.begin()) { Serial1.println("ALLOC_FAIL"); Serial1.println("BENCH_DONE"); return; }
    strip = (uint32_t *)(((uintptr_t)strip + 63u) & ~(uintptr_t)63u);
    scratch = (uint32_t *)(((uintptr_t)scratch + 63u) & ~(uintptr_t)63u);

    lvgl_rt1176_begin();
    lvgl_mipi_panel_set_rot_threshold(0xFFFFFFFFu);    /* never a forced full-frame rotate */
    lvgl_mipi_panel_create_rotated(Display, LVGL_PANEL_PRESENT_CW90);
    lvgl_mipi_panel_set_preflip_cb(preflip);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x141C26), 0);
    lane = lv_obj_create(scr);
    lv_obj_remove_style_all(lane);
    lv_obj_set_pos(lane, LX, LY); lv_obj_set_size(lane, LW, LH);
    lv_obj_add_event_cb(lane, lane_event, LV_EVENT_ALL, nullptr);
    square = lv_obj_create(scr);
    lv_obj_remove_style_all(square);
    lv_obj_set_size(square, 8, 8); lv_obj_set_pos(square, 600, 420);
    lv_obj_set_style_bg_opa(square, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(square, lv_color_hex(0xFFC861), 0);

    run_arm("baseline", ARM_BASE);
    run_arm("A", ARM_A);
    run_arm("C", ARM_C);
    run_arm("D-est", ARM_D);
    run_arm("baseline", ARM_BASE);
    Serial1.println("BENCH_DONE");
}

void loop() { lvgl_rt1176_loop(); }
