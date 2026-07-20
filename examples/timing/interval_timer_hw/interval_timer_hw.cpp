/* IntervalTimer hardware check for the MIMXRT1176-EVKB.
 *
 * Runs PIT1 via IntervalTimer at a 1000 us period and toggles D9 in the ISR,
 * producing a 500 Hz square wave (one toggle per callback => half the callback
 * frequency).  Probe D9 on the Saleae; VCOM (Serial1 @115200) prints a
 * ticks/sec heartbeat (~1000) confirming the period.
 */
#include "Arduino.h"
#include "HardwareSerial.h"
#include "IntervalTimer.h"

#define SCOPE_PIN 9      /* toggled in the ISR; Saleae probe here */

IntervalTimer itimer;
volatile uint32_t ticks = 0;
volatile uint8_t  level = 0;

static void onTick() {
    ticks++;
    level ^= 1;
    digitalWrite(SCOPE_PIN, level);   /* 1000 us period -> toggle -> 500 Hz */
}

void setup() {
    Serial1.begin(115200);
    pinMode(SCOPE_PIN, OUTPUT);
    digitalWrite(SCOPE_PIN, LOW);
    bool ok = itimer.begin(onTick, 1000);
    Serial1.println(ok ? "IT-HW: begin OK  (D9 = 500 Hz, 1000 us period)"
                       : "IT-HW: begin FAILED");
}

void loop() {
    uint32_t t0 = ticks;
    delay(1000);
    uint32_t dt = ticks - t0;             /* callbacks in ~1 s -> expect ~1000 */
    Serial1.print("IT-HW ticks/sec=");
    Serial1.println(dt);
}
