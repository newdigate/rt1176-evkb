/* rpi_panel_test - RT1176 -> RPi 7" MIPI-DSI display v1 gate
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Task 1: scaffold only. Display.begin() is a stub (returns false), so every
 * stage token below prints FAIL and FB_SUM never appears -- this IS the
 * intended red gate. Later tasks (4,6,8,10,12,13) turn each stage green in
 * turn as the real ATtiny/PLL/LCDIFv2/DSI/TC358762/PXP-fill drivers land.
 *
 * NOTE: uses Serial1 (the LPUART console every sibling gate's run_qemu.sh
 * captures via `-serial file:`), not Serial (native USB CDC, which QEMU
 * would not capture here) -- see cores/imxrt1176/usb_serial.h.
 */
#include <Arduino.h>
#include <Wire.h>   // TEMP(Task 3 probe) — remove in Task 4 (kept: Task 4 driver needs it)
#include "Display.h"

// TEMP(Task 3 probe) — remove in Task 4. Raw Wire read of the virtual ATtiny88
// ID register, proving the QEMU model answers over the real emulated LPI2C1
// before the Display/ATtiny driver (Task 4) exists.
#define PROBE_ATTINY 1

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

#ifdef PROBE_ATTINY
    // TEMP(Task 3 probe) — remove in Task 4.
    // Register-pointer read of REG_ID (0xFC) at I2C 0x45: set the pointer with a
    // write, then requestFrom() clocks the byte back. The QEMU ATtiny model
    // returns 0xDE. This is a standalone raw-Wire probe of the model over the
    // real LPI2C1 path — the Display driver (Task 4) supersedes it.
    Wire.begin();
    Wire.beginTransmission(0x45);
    Wire.write((uint8_t)0xFC);          // REG_ID
    Wire.endTransmission(false);        // repeated-START, keep the bus
    Wire.requestFrom((uint8_t)0x45, (uint8_t)1);
    uint8_t attiny_id = Wire.available() ? (uint8_t)Wire.read() : 0xFF;
    Serial1.printf("PROBE_ATTINY=0x%02X\n", attiny_id);
#endif

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
