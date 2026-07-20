#include "Arduino.h"

// millis()-based blink (no delay()) - proves millis() works on hardware.
static uint32_t last = 0;
static bool on = false;

void setup() {
    pinMode(LED_BUILTIN, OUTPUT);
}

void loop() {
    if (millis() - last >= 250) {   // toggle every 250 ms -> ~2 Hz
        last = millis();
        on = !on;
        digitalWrite(LED_BUILTIN, on ? HIGH : LOW);
    }
}
