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
 * Stage 3 (MINIMAL, deliberately): read() is polled just far enough to prove
 * two things -- that a contact comes back at all, and that the part goes on to
 * publish the RELEASE after it.  The second is the load-bearing one: this part
 * republishes only after the host acknowledges the previous buffer by writing 0
 * to the status register, so a driver that skips that write sees the first
 * contact for ever.  A full three-phase check of the scripted path (five taps
 * in order, a monotonic drag, two contacts with distinct track ids) is a later
 * stage; this one covers the driver's central discipline and nothing more.
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
    // combination says: a config-CONTENT failure (CFGVER / CFGSUM / CFGRES /
    // CFGPTS) prints I2C_OK, because 186 bytes did arrive and only what they
    // hold is wrong, while a config-READ failure prints I2C_FAIL.  The driver
    // draws that line in busOk(); this example only reports it, and deliberately
    // does not enumerate the taxonomy itself -- the four content tokens above
    // were added to the driver without touching a line of this file.
    if (!ok) {
        Serial1.printf("TOUCH_ERR=%s ID=0x%08lX I2C=%u\n",
                       GT911::errorName(touch.lastError()),
                       (unsigned long)touch.lastDeviceId(),
                       (unsigned)touch.lastI2cStatus());
    }

    // --- Stage 3: the coordinate layer -------------------------------------
    // Ten slots, and sized by a literal ten on purpose: the driver's header
    // promises the register file holds ten contact slots and that
    // configuredPoints() is validated into 1..10, so a caller may size by ten
    // and read configuredPoints() contacts into it safely.  Sizing this by
    // touch.configuredPoints() instead would let a byte the PART reported
    // decide a stack array's length, which is the hazard that validation
    // exists to close -- do not "improve" it that way.
    TouchPoint pts[10];
    TouchPoint first{};
    bool haveFirst = false;
    bool advanced  = false;

    // A bounded poll, not a spin: a wedged part must reach the tokens below so
    // the gate can name the layer that broke, rather than hanging until the
    // harness kills QEMU with nothing in the transcript.  ~500 ms is generous
    // -- the first published instant and the one after it are 20 ms apart.
    for (uint16_t i = 0; i < 500 && !advanced; i++) {
        uint8_t n = 0;
        const GT911::Poll p =
            touch.read(pts, (uint8_t)(sizeof(pts) / sizeof(pts[0])), &n);

        if (!haveFirst) {
            if (p == GT911::Poll::Contacts && n > 0) {
                first = pts[0];
                haveFirst = true;
            }
        } else if (p == GT911::Poll::Released) {
            // THE RELEASE, and this single comparison is the whole assertion.
            //
            // Released is a buffer the part PUBLISHED carrying zero contacts.
            // An unacknowledged part re-serves the same one-contact buffer for
            // ever, so this state is unreachable unless read() performed the
            // mandatory status clear and the part moved on.
            //
            // Two earlier versions of this test were wrong, both by rebuilding
            // "release" out of ingredients instead of asking for it:
            //
            //   n == 0 alone            -- also true of an idle poll, and the
            //                              poll loop runs far faster than the
            //                              part publishes, so almost every poll
            //                              is idle.
            //   fresh && n == 0         -- a wedged part reports the ready bit
            //                              set on every poll, and a faulting
            //                              contact read then yields a zero
            //                              count, so a permanently stuck part
            //                              satisfied it and the gate went GREEN.
            //
            // The driver now answers the question directly.  Poll::Failed is a
            // distinct state, so a fault can no longer masquerade as a release.
            advanced = true;
        }
        delay(1);
    }

    // Unconditional, like every stage above, and the coordinates are RECORDED
    // rather than asserted for the same reason RES/POINTS are: in QEMU the
    // scripted path and any on-screen target are both expressed as percentages
    // of the same resolution, so model and firmware share an assumption about
    // which corner is (0,0).  No QEMU run can falsify a shared assumption.
    // Only a finger on glass settles the coordinate-to-display mapping.
    if (haveFirst) {
        Serial1.printf("FIRST_TOUCH x=%u y=%u id=%u\n",
                       (unsigned)first.x, (unsigned)first.y, (unsigned)first.id);
    } else {
        Serial1.printf("FIRST_TOUCH_NONE err=%s I2C=%u\n",
                       GT911::errorName(touch.lastError()),
                       (unsigned)touch.lastI2cStatus());
    }
    // The stalled line carries diagnostics because the two ways of reaching it
    // need telling apart in a transcript: a part that is wedged on one buffer
    // (err=NONE -- nothing failed, it simply never moved on) versus one whose
    // transfers are failing (err=PTREAD / STATCLR / STATREAD).
    if (advanced) {
        Serial1.println("TOUCH_ADVANCED");
    } else {
        Serial1.printf("TOUCH_STALLED err=%s I2C=%u\n",
                       GT911::errorName(touch.lastError()),
                       (unsigned)touch.lastI2cStatus());
    }
}

void loop() {}
