#include "Arduino.h"
#include "core_pins.h"
#include "HardwareSerial.h"

void setup() {
    Serial1.begin(115200);
    Serial1.println("RT1176 Serial1 up");
}

void loop() {
    static uint32_t n = 0;
    Serial1.print("count=");
    Serial1.println(n++);
    delay(200);
}
