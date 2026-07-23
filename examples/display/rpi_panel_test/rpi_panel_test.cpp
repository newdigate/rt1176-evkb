/* rpi_panel_test - RT1176 -> RPi 7" MIPI-DSI display v1 gate
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * As of Task 4: ATTINY_OK (the RPiDisplay ATtiny88 driver is live), remaining
 * stage tokens FAIL and FB_SUM never appears -- begin() still returns false
 * overall (later stages unimplemented). Later tasks (6,8,10,12,13) turn each
 * remaining stage green in turn as the real PLL/LCDIFv2/DSI/TC358762/PXP-fill
 * drivers land.
 *
 * NOTE: uses Serial1 (the LPUART console every sibling gate's run_qemu.sh
 * captures via `-serial file:`), not Serial (native USB CDC, which QEMU
 * would not capture here) -- see cores/imxrt1176/usb_serial.h.
 */
#include <Arduino.h>
#include <Wire.h>   // needed by the RPiDisplay ATtiny driver (Display::begin())
#include "Display.h"

static const uint16_t COLOR = 0xF800; // solid red (RGB565) -- arbitrary, checksum is computed

// FNV-1a over the framebuffer bytes
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

    if (ok) {
        Display.fillScreen(COLOR);
        // FB_SUM: checksum the SDRAM framebuffer we painted (proves PXP.fill)
        uint32_t fb_sum = fnv1a((const uint8_t*)Display.framebuffer(), (uint32_t)Display.width() * Display.height() * 2u);
        // expected: whole buffer is COLOR
        uint32_t exp = 2166136261u; uint8_t lo = COLOR & 0xFF, hi = (COLOR >> 8) & 0xFF;
        for (uint32_t i = 0; i < (uint32_t)Display.width() * Display.height(); i++) { exp ^= lo; exp *= 16777619u; exp ^= hi; exp *= 16777619u; }
        Serial1.printf("FB_SUM=%08lX EXP=%08lX %s\n", (unsigned long)fb_sum, (unsigned long)exp, fb_sum == exp ? "PASS" : "FAIL");
        // PANEL_SUM: QEMU-only oracle -- the virtual TC358762's received-pixel checksum (Task 11/13)
        Serial1.println("PANEL_SUM_PENDING"); // filled in at Task 13
    }
    Serial1.println("RPI_PANEL_END");
}

void loop() {}
