/* IntervalTimer-backed tone() hardware check for the MIMXRT1176-EVKB.
 * Plays a steady 1000 Hz square wave on D9 (Saleae probe here); VCOM
 * (Serial1 @115200) prints a heartbeat. Expect ~1000 Hz (±RC tolerance). */
#include "Arduino.h"
#include "HardwareSerial.h"

#define TONE_PIN 9

void setup() {
    Serial1.begin(115200);
    tone(TONE_PIN, 1000);        // steady 1000 Hz on D9
    Serial1.println("TONE-HW: D9 = 1000 Hz tone");
}

void loop() {
    delay(1000);
    Serial1.println("TONE-HW: playing 1000 Hz");
}
