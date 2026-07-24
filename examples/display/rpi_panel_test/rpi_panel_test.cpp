/* rpi_panel_test - RT1176 -> RPi 7" MIPI-DSI display v1 gate
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * As of Task 12: ALL FIVE stage tokens OK -- ATTINY_OK + PLL_OK + LCDIFV2_OK +
 * DSI_OK + TC358762_OK. begin() now returns true overall (every layer, up to
 * and including the real TC358762 bridge init sequence, is live).
 *
 * The TEMP Task-11 PROBE_BRIDGE block (which hand-poked the QEMU-side virtual
 * TC358762's readiness FSM + PANEL_SUM oracle with a deliberately fake minimal
 * sequence) is GONE: the real firmware bridge driver now drives that same path
 * for real, and TC358762_OK is what asserts it.
 *
 * FB_SUM/PANEL_SUM are still PENDING: begin() returning true means the `if
 * (ok)` block finally runs, but fillScreen() is a Task-13 stub, so there is
 * nothing honest to checksum yet. Task 13 restores both comparisons.
 *
 * NOTE: uses Serial1 (the LPUART console every sibling gate's run_qemu.sh
 * captures via `-serial file:`), not Serial (native USB CDC, which QEMU
 * would not capture here) -- see cores/imxrt1176/usb_serial.h.
 */
#include <Arduino.h>
#include <Wire.h>   // needed by the RPiDisplay ATtiny driver (Display::begin())
#include "Display.h"

static const uint16_t COLOR = 0xF800; // solid red (RGB565) -- arbitrary, checksum is computed

// FNV-1a over the framebuffer bytes. Unused until Task 13's FB_SUM/PANEL_SUM
// checks come back -- kept (not deleted) per the plan, annotated to stay
// warning-clean, exactly as rgb565_to_888() below already is.
static uint32_t fnv1a(const uint8_t *p, uint32_t n) __attribute__((unused));
static uint32_t fnv1a(const uint8_t *p, uint32_t n) {
    uint32_t h = 2166136261u;
    while (n--) { h ^= *p++; h *= 16777619u; }
    return h;
}

// Standard bit-replication RGB565->RGB888 expansion, shared with PXP fill +
// the bridge oracle. Unused until Task 13's fillScreen()/gate check reuse it
// verbatim -- kept (not deleted) per the plan, annotated to stay warning-clean.
static inline uint32_t rgb565_to_888(uint16_t c) __attribute__((unused));
static inline uint32_t rgb565_to_888(uint16_t c) {
    uint32_t r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2);
    return (r << 16) | (g << 8) | b;
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
        // FB_SUM / PANEL_SUM are deliberately NOT computed yet. begin() now
        // returns true, so this block finally runs -- but fillScreen() is still
        // a Task-13 stub, so the framebuffer holds whatever extmem_malloc gave
        // us, not COLOR. Checksumming that would print a guaranteed FAIL (or,
        // worse, invite someone to "fix" it by weakening the expectation).
        // Task 13 implements the PXP fill and restores BOTH comparisons: the
        // software-expected framebuffer checksum here, and the virtual bridge's
        // received-pixel PANEL_SUM oracle.
        Serial1.println("FB_SUM_PENDING");    // filled in at Task 13
        Serial1.println("PANEL_SUM_PENDING"); // filled in at Task 13
    }
    Serial1.println("RPI_PANEL_END");
}

void loop() {}
