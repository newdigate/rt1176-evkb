/* lvgl_rk055_flip_test - double buffering with page flip on vsync (v4).
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * An animated box sweeps the panel while LVGL renders into the OFF-SCREEN
 * buffer and the LCDIFv2 flips at vsync.  The scene is deliberately
 * GOLDEN-FREE: it is time-driven, so the gate asserts flip DISCIPLINE --
 * never pixel checksums of a fixed scene:
 *
 *   FLIP_A/B MATCH   firmware's FNV-1a of the buffer it just flipped in
 *                    equals the virtual HX8394 tap's PANEL_SUM -- the panel
 *                    SCANNED that buffer.  Two consecutive frames, and
 *                    A != B is itself asserted (DISTINCT), or alternation
 *                    would be unfalsifiable.  QEMU-only by construction:
 *                    the tap is emulator fiction (branch on TAP_ID, exactly
 *                    as rk055_panel_test does).
 *   FLIPS==REFRESHES one shadow-load pulse per full refresh.
 *   VSYNCS==FLIPS    every flip's landing was consumed exactly once.
 *   VSYNC_TIMEOUTS=0 no wait gave up; a dead vsync names itself.
 *
 *   WHAT ONLY HARDWARE PROVES: tearing's ABSENCE -- an eye on the sweeping
 *   box (QEMU has no partial-scanout model), and the real 58.7 Hz cadence.
 *   Build with -DFLIP_DEMO_SINGLE=ON for the v1 single-buffer "before".
 *
 * Uses Serial1 (LPUART; QEMU captures it), like every sibling gate.
 */
#include <Arduino.h>
#include <Display.h>
#include "lvgl_rt1176.h"
#include "lvgl_mipi_panel.h"

/* The HX8394 tap window (QEMU-only; reads as 0 on silicon).  Address and
 * TAP_ID protocol: transcribed from rk055_panel_test.cpp -- same oracle,
 * same rules (branch on TAP_ID, never assume the tap exists).  Layout:
 * qemu2 include/hw/display/imxrt_hx8394.h; mapped over the RESERVED upper
 * part of the MIPI-DSI host's own AIPS slot, so real silicon reads 0
 * instead of faulting. */
#define HX_TAP(off)         (*(volatile uint32_t *)(0x4080E000u + (off)))
#define HX_TAP_ID           HX_TAP(0x00)
#define HX_TAP_STATUS       HX_TAP(0x04)
#define HX_TAP_PANEL_SUM    HX_TAP(0x08)
#define HX_TAP_ID_MAGIC     0x48583934u   // "HX94"; 0 on real silicon

static constexpr int32_t  BOX_W = 120, BOX_H = 120;
static constexpr int32_t  BOX_STEP = 12;        /* px per frame */
static constexpr uint32_t TOTAL_FRAMES = 120;   /* 2 full sweeps of 720 px */
static constexpr uint32_t FRAME_TIMEOUT_MS = 2000;

static lv_obj_t *s_box;
static int32_t   s_box_x = 0;

static void build_scene()
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    s_box = lv_obj_create(scr);
    lv_obj_set_size(s_box, BOX_W, BOX_H);
    lv_obj_set_pos(s_box, 0, (int32_t)(PANEL_HEIGHT / 2) - BOX_H / 2);
    lv_obj_set_style_bg_color(s_box, lv_color_hex(0xE0A030), LV_PART_MAIN);
}

/* Advance the box one step and pump LVGL until exactly one more full refresh
 * (== one more flip) has happened.  False on timeout. */
[[nodiscard]] static bool renderFrame()
{
    s_box_x = (s_box_x + BOX_STEP) % (int32_t)(PANEL_WIDTH - BOX_W);
    lv_obj_set_x(s_box, s_box_x);
    const uint32_t want = lvgl_mipi_panel_flips() + 1;
    const uint32_t deadline = millis() + FRAME_TIMEOUT_MS;
    while ((int32_t)(millis() - deadline) < 0) {
        lvgl_rt1176_loop();
        if (lvgl_mipi_panel_flips() >= want) return true;
    }
    return false;
}

/* FNV-1a over a whole framebuffer, matching the tap's arithmetic. */
static uint32_t fb_sum(const uint16_t *fb)
{
    lvgl_sum_reset();
    lvgl_sum_feed(fb, PANEL_FB_BYTES);
    return lvgl_sum_value();
}

void setup()
{
    Serial1.begin(115200);
    delay(200);
    Serial1.println("LVGL_RK055_FLIP_BEGIN");

    const bool ok = Display.begin();
    Serial1.println(ok ? "PANEL_OK" : "PANEL_FAIL");
    if (!ok) { Serial1.println("LVGL_RK055_FLIP_DONE"); return; }
    Display.fillScreen(0x0000);

    lvgl_rt1176_begin();
#if defined(FLIP_DEMO_SINGLE)
    /* The bench "before": same animation, v1 single-buffer direct render,
     * tearing accepted and expected.  No flip assertions are possible or
     * attempted -- the counters below would all read 0. */
    lvgl_mipi_panel_create(Display);
    Serial1.println("MODE=SINGLE_BUFFER_DEMO");
#else
    lvgl_mipi_panel_create_db(Display);
    Serial1.println("MODE=DOUBLE_BUFFER");
#endif
    build_scene();

    bool pass = true;

#if !defined(FLIP_DEMO_SINGLE)
    /* --- frames 1 and 2: the panel-scanned-this-buffer proof ------------- */
    uint32_t sumA = 0, sumB = 0;
    for (uint8_t f = 0; f < 2 && pass; f++) {
        if (!renderFrame()) {
            Serial1.printf("FLIP_FAIL frame=%u reason=refresh-timeout\n", f + 1u);
            pass = false;
            break;
        }
        lvgl_mipi_panel_flip_sync();   /* the flip must LAND before we look */
        const uint16_t *fb = lvgl_mipi_panel_scanned_fb();
        const uint32_t fw = fb ? fb_sum(fb) : 0;
        /* PANEL_SUM via the QEMU-only tap, guarded by TAP_ID exactly as
         * rk055_panel_test does: branch on TAP_ID, never on the checksum.
         * On silicon the window reads 0 -- ..._HW=TAP_ABSENT, verify by
         * eye, and `pass` is NOT cleared: the tap is emulator fiction. */
        const uint32_t tap_id = HX_TAP_ID;
        const uint32_t tap = (tap_id == HX_TAP_ID_MAGIC) ? HX_TAP_PANEL_SUM : 0;
        const char *which = (f == 0) ? "A" : "B";
        Serial1.printf("FLIP_%s_SUM=0x%08lX PANEL_%s_SUM=0x%08lX\n",
                       which, (unsigned long)fw, which, (unsigned long)tap);
        if (f == 0) sumA = fw; else sumB = fw;
        /* MATCH/MISMATCH token per frame; MISMATCH clears `pass`. */
        if (tap_id != HX_TAP_ID_MAGIC) {
            Serial1.printf("FLIP_%s_HW=TAP_ABSENT (TAP_ID=0x%08lX)"
                           " -- verify by eye\n",
                           which, (unsigned long)tap_id);
        } else if (fw == tap) {
            Serial1.printf("FLIP_%s=MATCH\n", which);
        } else {
            Serial1.printf("FLIP_%s=MISMATCH\n", which);
            /* Say WHICH tap precondition is missing (HX8394_ST_* bits). */
            Serial1.printf("TAP_STATUS=0x%08lX\n",
                           (unsigned long)HX_TAP_STATUS);
            pass = false;
        }
    }
    if (pass && sumA == sumB) {
        Serial1.println("FLIP_FAIL reason=frames-identical (vacuous alternation)");
        pass = false;
    } else if (pass) {
        Serial1.println("DISTINCT=OK");
    }
#endif

    /* --- the remaining frames: discipline counters ------------------------ */
    uint32_t frames_done = lvgl_mipi_panel_flips();
    while (pass && frames_done < TOTAL_FRAMES) {
        if (!renderFrame()) {
            Serial1.printf("FLIP_FAIL frame=%lu reason=refresh-timeout\n",
                           (unsigned long)(frames_done + 1));
            pass = false;
            break;
        }
        frames_done = lvgl_mipi_panel_flips();
    }
    lvgl_mipi_panel_flip_sync();

    Serial1.printf("REFRESHES=%lu\n", (unsigned long)frames_done);
    Serial1.printf("FLIPS=%lu\n", (unsigned long)lvgl_mipi_panel_flips());
    Serial1.printf("VSYNCS=%lu\n", (unsigned long)lvgl_mipi_panel_vsyncs());
    Serial1.printf("VSYNC_TIMEOUTS=%lu\n",
                   (unsigned long)lvgl_mipi_panel_vsync_timeouts());
    if (pass && lvgl_mipi_panel_vsync_timeouts() == 0) {
        Serial1.println("FLIP_OK");
    }
    Serial1.println("LVGL_RK055_FLIP_DONE");
}

/* Keep sweeping for the bench eye -- this loop is the hardware demonstration.
 * millis-paced so QEMU's virtual clock and silicon behave alike. */
void loop()
{
    static uint32_t last = 0;
    if (millis() - last >= 33) {
        last = millis();
        s_box_x = (s_box_x + BOX_STEP) % (int32_t)(PANEL_WIDTH - BOX_W);
        lv_obj_set_x(s_box, s_box_x);
    }
    lvgl_rt1176_loop();
}
