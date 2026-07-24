/* rpi_panel_test - RT1176 -> RPi 7" MIPI-DSI display v1 gate
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * As of Task 13 (the capstone) this gate is a CLOSED, self-validating loop:
 * paint the SDRAM framebuffer with the PXP, then prove -- twice, against two
 * independent oracles -- that exactly those pixels are what the panel gets.
 *
 *   ATTINY_OK / PLL_OK / LCDIFV2_OK / DSI_OK / TC358762_OK
 *       each bring-up layer, emitted unconditionally so the first FAIL
 *       pinpoints the broken one.
 *   FRAME_OK
 *       a whole LCDIFv2 frame scanned out after the fill.  Neither checksum
 *       below can see this: both read pixels, and pixels read the same whether
 *       the display is scanning or stopped dead.
 *   FB_SUM
 *       FNV-1a of the framebuffer as it sits in SDRAM, vs a software-computed
 *       expectation.  Proves fillScreen()/PXP wrote what was asked for.
 *   PANEL_SUM
 *       the virtual TC358762's received-pixel checksum vs the SAME expectation.
 *       Proves the bridge would deliver exactly those pixels -- it returns a
 *       sentinel unless the whole chain (panel power, init contract, DSI link,
 *       enabled layer) really would put them on glass.
 *
 * NOTE: uses Serial1 (the LPUART console every sibling gate's run_qemu.sh
 * captures via `-serial file:`), not Serial (native USB CDC, which QEMU
 * would not capture here) -- see cores/imxrt1176/usb_serial.h.
 */
#include <Arduino.h>
#include <Wire.h>   // needed by the RPiDisplay ATtiny driver (Display::begin())
#include "Display.h"

// Solid red.  ARBITRARY -- the checksum is computed, never keyed to a colour --
// but it does have to survive the RGB565 -> RGB888 -> RGB565 round trip through
// PXP_PS_BACKGROUND intact, and it does: bit-replication expansion is exactly
// undone by the PXP output stage's truncation, for all 65536 RGB565 values (see
// displayRgb565To888() in Display.h).  So no colour is disqualified, and this
// one is simply the most obvious "is it lit?" on a photograph.
static const uint16_t COLOR = 0xF800;

// --- the QEMU-only virtual TC358762 debug tap --------------------------------
// Layout: qemu2 include/hw/display/imxrt_tc358762.h.  Mapped over the RESERVED
// upper part of the MIPI-DSI host's own AIPS slot, so on real silicon these
// reads return 0 instead of faulting -- TAP_ID is how this gate tells "no tap
// here" (hardware) from "tap says the panel is dark" (a real failure).  Nothing
// in the firmware may depend on any of it.
#define TC_TAP(off)         (*(volatile uint32_t *)(0x4080F000u + (off)))
#define TC_TAP_ID           TC_TAP(0x00)
#define TC_TAP_STATUS       TC_TAP(0x04)
#define TC_TAP_PANEL_SUM    TC_TAP(0x08)
#define TC_TAP_ID_MAGIC     0x54433632u   // "TC62"; 0 on real silicon

// --- FNV-1a-32 ---------------------------------------------------------------
// One folding primitive, two walkers: the real framebuffer, and the frame we
// EXPECT.  Spelling the round out twice is how the two would silently drift
// into agreeing with each other instead of with the pixels.
static const uint32_t FNV1A_OFFSET = 2166136261u;
static const uint32_t FNV1A_PRIME  = 16777619u;

static inline uint32_t fnv1a_byte(uint32_t h, uint8_t b) {
    return (h ^ b) * FNV1A_PRIME;
}

// The bytes actually in memory.
static uint32_t fnv1a(const uint8_t *p, uint32_t n) {
    uint32_t h = FNV1A_OFFSET;
    while (n--) h = fnv1a_byte(h, *p++);
    return h;
}

// The bytes a full frame of one colour SHOULD be, folded in the same ascending
// address order, little-endian halfwords -- exactly how an RGB565 pixel sits in
// SDRAM.  Computed, never a literal: a hard-coded checksum would still "pass"
// after someone changed the colour, the geometry or the pixel format.
static uint32_t fnv1a_solid(uint16_t px, uint32_t pixels) {
    uint32_t h = FNV1A_OFFSET;
    while (pixels--) {
        h = fnv1a_byte(h, (uint8_t)(px & 0xFFu));
        h = fnv1a_byte(h, (uint8_t)(px >> 8));
    }
    return h;
}

// RGB888 -> RGB565 by truncation: what the PXP's output stage does to the
// 24-bit PS_BACKGROUND on its way into an RGB565 surface (i.MX RT1170 RM rev.5
// 52.6.17; CSC1 is in RGB bypass, so it is a plain format narrow, not a
// matrix).  Composing this with Display.h's displayRgb565To888() reproduces the
// whole hardware seam, so the expectation below is DERIVED from what the
// pipeline does rather than assuming the round trip is the identity.
static inline uint16_t rgb888_to_565(uint32_t c) {
    return (uint16_t)((((c >> 16) & 0xFFu) >> 3) << 11 |
                      (((c >>  8) & 0xFFu) >> 2) <<  5 |
                      (( c        & 0xFFu) >> 3));
}

void setup() {
    Serial1.begin(115200);
    delay(200);
    Serial1.println("RPI_PANEL_BEGIN");

    bool ok = Display.begin();
    // stage tokens -- emitted unconditionally so the first false pinpoints the broken layer
    Serial1.printf("ATTINY_%s\n",   Display.attinyOk() ? "OK" : "FAIL");
    Serial1.printf("PLL_%s\n",      Display.pllOk()    ? "OK" : "FAIL");
    Serial1.printf("LCDIFV2_%s\n",  Display.lcdifOk()  ? "OK" : "FAIL");
    Serial1.printf("DSI_%s\n",      Display.dsiOk()    ? "OK" : "FAIL");
    Serial1.printf("TC358762_%s\n", Display.bridgeOk() ? "OK" : "FAIL");

    // (The Task-7 LCDIFv2 scan-checksum probe, the Task-9 DSI packet probes and
    // the Task-11 PROBE_BRIDGE bridge-FSM probe all lived here. The first two
    // read TEMP debug taps Task 11 retired (LCDIFv2 0x3FFC, DSI 0x3F00..); the
    // third hand-poked the virtual TC358762 with a deliberately FAKE minimal
    // sequence, standing in for the real driver. All three are gone: Task 12's
    // tc358762Init() drives that path for real, and TC358762_OK asserts it.)

    if (ok) {
        Display.fillScreen(COLOR);
        Serial1.printf("FRAME_%s\n", Display.frameOk() ? "OK" : "TIMEOUT");

        // What the pipeline should have left in every pixel: COLOR expanded to
        // RGB888 for PXP_PS_BACKGROUND (Display.h -- the SAME function
        // fillScreen() called, not a copy of it), narrowed back to RGB565 by
        // the PXP output stage.
        const uint16_t expect_px = rgb888_to_565(displayRgb565To888(COLOR));
        const uint32_t pixels    = (uint32_t)Display.width() * Display.height();
        const uint32_t expect    = fnv1a_solid(expect_px, pixels);

        // FB_SUM -- oracle 1: the bytes really in SDRAM. Proves the PXP fill.
        // (Read straight back with the CPU: this core runs with the D-cache
        // off, so there is no stale line between the PXP's AXI writes and this.)
        const uint32_t fb_sum =
            fnv1a((const uint8_t *)Display.framebuffer(),
                  pixels * 2u /* RGB565 */);
        Serial1.printf("FB_SUM=0x%08lX EXP=0x%08lX %s\n",
                       (unsigned long)fb_sum, (unsigned long)expect,
                       fb_sum == expect ? "PASS" : "FAIL");

        // PANEL_SUM -- oracle 2: what the bridge would deliver downstream.
        // Branch on TAP_ID, never on the checksum: the tap is QEMU fiction and
        // reads 0 on silicon, where a sentinel PANEL_SUM means "no tap", not
        // "dark panel". Testing the checksum against 0 instead would conflate
        // the two and fail every hardware run.
        const uint32_t tap_id = TC_TAP_ID;
        if (tap_id != TC_TAP_ID_MAGIC) {
            Serial1.printf("PANEL_SUM_HW=TAP_ABSENT (TAP_ID=0x%08lX)"
                           " -- verify by eye\n", (unsigned long)tap_id);
        } else {
            const uint32_t panel_sum = TC_TAP_PANEL_SUM;
            const bool panel_pass = (panel_sum == expect);
            Serial1.printf("PANEL_SUM=0x%08lX EXP=0x%08lX %s\n",
                           (unsigned long)panel_sum, (unsigned long)expect,
                           panel_pass ? "PASS" : "FAIL");
            // On a mismatch, say WHICH precondition is missing rather than just
            // "wrong number": the bridge returns the sentinel when it is not
            // ready or the layer is not enabled, and TAP_STATUS carries one bit
            // per condition (imxrt_tc358762.h).
            if (!panel_pass) {
                Serial1.printf("TAP_STATUS=0x%08lX\n",
                               (unsigned long)TC_TAP_STATUS);
            }
        }
    }
    Serial1.println("RPI_PANEL_END");
}

void loop() {}
