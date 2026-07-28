/* rk055_touch_test - RT1176 -> RK055HDMIPI4MA0 GT911 capacitive touch gate
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Stage 1: the panel is up and the GT911 has been reset, has latched its I2C
 * address from the INT pin, and has identified itself.
 *
 * Stage 2: its 186-byte configuration blob reads back with a valid checksum,
 * and the resolution and contact count it was programmed with are recorded.
 * Recorded, not asserted -- see the RES/POINTS comment in run_qemu.sh.
 *
 * Every stage token is emitted UNCONDITIONALLY so the first FAIL pinpoints the
 * broken layer rather than the run simply stopping.
 *
 * Uses Serial1 (the LPUART console run_qemu.sh captures via `-serial file:`),
 * not Serial (native USB CDC, which QEMU would not capture here).
 */
#include <Arduino.h>
#include <Wire.h>
#include <Display.h>
#include "gt911.h"

// D9 = GPIO_AD_01 = touch reset; D6 = GPIO_AD_00 = touch interrupt.
// Both are RevC3 nets CTP_RST_B / CTP_INT via 0R to J48 pins 28 / 29.
static constexpr uint8_t TOUCH_RST_PIN = 9;
static constexpr uint8_t TOUCH_INT_PIN = 6;

static GT911 touch(Wire2, TOUCH_RST_PIN, TOUCH_INT_PIN);

void setup() {
    Serial1.begin(115200);
    delay(200);
    Serial1.println("RK055_TOUCH_BEGIN");

    Display.begin();
    Serial1.printf("PANEL_%s\n", Display.panelOk() ? "OK" : "FAIL");
    Display.fillScreen(0x0000);

    // LPI2C5 is shared with the WM8962 codec, so the EXAMPLE owns the bus, not
    // the driver.  400 kHz is the GT911's rated maximum; the 24 MHz functional
    // clock the Wire library configures for this instance is what setClock()'s
    // prescale arithmetic assumes.  NXP instead runs the root at 24 MHz / 12
    // and divides less -- we reach a comparable SCL rate from a root that is
    // already hardware-proven on this bus.
    Wire2.begin();
    Wire2.setClock(400000);

    const bool ok = touch.begin();

    // Two independent questions, answered separately.  I2C_ is "did anything
    // ACK at the address we latched"; GT911_ is "was it a GT911".  A part that
    // latched the OTHER address fails the first; a different part on the bus
    // would fail the second.
    //
    // busOk() rather than a test on lastError() here: classifying errors is the
    // driver's job, and an example that enumerated the driver's taxonomy would
    // quietly go wrong the first time an enumerator was added.
    Serial1.printf("I2C_%s\n", touch.busOk() ? "OK" : "FAIL");
    Serial1.printf("ADDR=0x%02X\n", touch.address());

    if (ok) {
        // Design 6.1 asks for "GT911_OK  ID=911".  The ID is RENDERED FROM THE
        // FOUR BYTES ACTUALLY READ at 0x8140, not printed as a literal -- a
        // literal would assert nothing.  Safe to treat as text only here:
        // begin() has already proved these bytes equal "911\0", so the NUL
        // terminates the string and every character is printable.  The failure
        // path keeps the raw hex instead, which is what you want when the
        // bytes are NOT "911".
        const uint32_t raw = touch.lastDeviceId();
        char id[5];
        for (uint8_t i = 0; i < 4; i++) id[i] = (char)((raw >> (8 * i)) & 0xFF);
        id[4] = '\0';
        Serial1.printf("GT911_OK  ID=%s\n", id);
    } else {
        Serial1.println("GT911_FAIL");
    }

    // The configuration layer.  begin() reads and verifies the 186-byte blob as
    // its last act, so `ok` IS "the blob arrived and verified" -- there is no
    // separate query to make, and CFG_FAIL after an earlier failure (a bad ID,
    // say) is honest: the blob was never verified.  TOUCH_ERR below names the
    // layer that actually broke.
    //
    // RES/POINTS are printed unconditionally and are 0 0 on any failure, which
    // is the driver's contract, not a special case here.  They are RECORDED,
    // not asserted: the real panel reports whatever it was programmed with.
    Serial1.printf("CFG_%s\n", ok ? "OK" : "FAIL");
    Serial1.printf("RES=%ux%u\n", (unsigned)touch.resolutionX(),
                                  (unsigned)touch.resolutionY());
    Serial1.printf("POINTS=%u\n", (unsigned)touch.configuredPoints());

    // Last, because it summarises all the layers above it.  Note what the
    // combination says: a config-CHECKSUM failure prints I2C_OK (186 bytes did
    // arrive; only their content is wrong) while a config-READ failure prints
    // I2C_FAIL -- the driver draws that line in busOk(), and this example only
    // reports it.
    if (!ok) {
        Serial1.printf("TOUCH_ERR=%s ID=0x%08lX I2C=%u\n",
                       GT911::errorName(touch.lastError()),
                       (unsigned long)touch.lastDeviceId(),
                       (unsigned)touch.lastI2cStatus());
    }
}

void loop() {}
