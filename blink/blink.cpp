#include "Arduino.h"

void setup() {
    pinMode(LED_BUILTIN, OUTPUT);
}

void loop() {
    // Distinctive "heartbeat": blink-blink, then a long pause.
    digitalWrite(LED_BUILTIN, HIGH); delay(120);
    digitalWrite(LED_BUILTIN, LOW);  delay(120);
    digitalWrite(LED_BUILTIN, HIGH); delay(120);
    digitalWrite(LED_BUILTIN, LOW);  delay(640);
}
