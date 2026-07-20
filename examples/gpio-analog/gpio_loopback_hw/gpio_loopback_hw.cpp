/* D4 <-> D5 GPIO loopback hardware check for the MIMXRT1170-EVKB RevC3.
 *
 * Requires a jumper across J9 pin 10 (D4, GPIO_AD_06) and J9 pin 12
 * (D5, GPIO_AD_05) -- the even/socket column.  Re-test for the debunked
 * "D4 not on socket" claim: docs/arduino-header-revc3.md.
 *
 * Drives each pin in turn and reads it back on the other, both levels,
 * both directions.  Note D5/GPIO_AD_05 is also loaded by the SIM circuit
 * (populated 0R, sheet 22) and J25 pin 5 -- a driven loopback must still
 * win over that passive loading; a FAIL_* here means the jumper or table
 * is wrong, not the loading. */
#include "Arduino.h"
#include "HardwareSerial.h"

#define PIN_A 4   /* J9 pin 10, GPIO_AD_06, GPIO3.5 */
#define PIN_B 5   /* J9 pin 12, GPIO_AD_05, GPIO3.4 */

static int drive_and_read(uint8_t out, uint8_t in, int level) {
    pinMode(in, INPUT);
    pinMode(out, OUTPUT);
    digitalWrite(out, level);
    delay(2);
    int r = digitalRead(in);
    pinMode(out, INPUT);   /* release before swapping direction */
    return r;
}

void setup() {
    Serial1.begin(115200);
    Serial1.println("GPIO-LOOPBACK-HW: D4 (J9.10) <-> D5 (J9.12)");
}

void loop() {
    int a_hi = drive_and_read(PIN_A, PIN_B, HIGH);
    int a_lo = drive_and_read(PIN_A, PIN_B, LOW);
    int b_hi = drive_and_read(PIN_B, PIN_A, HIGH);
    int b_lo = drive_and_read(PIN_B, PIN_A, LOW);

    Serial1.print("D4->D5 hi=");  Serial1.print(a_hi);
    Serial1.print(" lo=");        Serial1.print(a_lo);
    Serial1.print("  D5->D4 hi="); Serial1.print(b_hi);
    Serial1.print(" lo=");        Serial1.println(b_lo);

    if (a_hi == 1 && a_lo == 0 && b_hi == 1 && b_lo == 0)
        Serial1.println("LOOPBACK=PASS");
    else
        Serial1.println("LOOPBACK=FAIL");
    delay(1000);
}
