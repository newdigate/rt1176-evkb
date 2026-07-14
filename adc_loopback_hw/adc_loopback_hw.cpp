/* D4 -> A0 analog loopback hardware check for the MIMXRT1170-EVKB RevC3.
 *
 * Requires a jumper from J9 pin 10 (D4, GPIO_AD_06) to J26 pin 2
 * (A0, GPIO_AD_10 = ADC1 CH2A).  Verifies the corrected analogRead
 * mapping end-to-end on silicon: drive D4 high/low, A0 must read near
 * full-scale / near zero (10-bit).  See docs/arduino-header-revc3.md.
 *
 * A0's pad is left in GPIO-input mux (ALT5, non-driving); the ADC input
 * is a dedicated pad function and taps the pad regardless of mux. */
#include "Arduino.h"
#include "HardwareSerial.h"

#define DRIVE_PIN 4    /* J9 pin 10, GPIO_AD_06 */

void setup() {
    Serial1.begin(115200);
    pinMode(DRIVE_PIN, OUTPUT);
    pinMode(A0, INPUT);          /* non-driving mux; digitalRead cross-check */
    Serial1.println("ADC-LOOPBACK-HW: D4 (J9.10) -> A0 (J26.2, ADC1 CH2A)");
}

void loop() {
    digitalWrite(DRIVE_PIN, HIGH);
    delay(2);
    int a_hi = analogRead(A0);          /* 10-bit: expect near 1023 */
    int d_hi = digitalRead(A0);
    digitalWrite(DRIVE_PIN, LOW);
    delay(2);
    int a_lo = analogRead(A0);          /* expect near 0 */
    int d_lo = digitalRead(A0);

    Serial1.print("A0 hi=");   Serial1.print(a_hi);
    Serial1.print(" lo=");     Serial1.print(a_lo);
    Serial1.print("  dig hi=");Serial1.print(d_hi);
    Serial1.print(" lo=");     Serial1.println(d_lo);

    if (a_hi > 900 && a_lo < 100 && d_hi == 1 && d_lo == 0)
        Serial1.println("ADC-LOOPBACK=PASS");
    else
        Serial1.println("ADC-LOOPBACK=FAIL");
    delay(1000);
}
