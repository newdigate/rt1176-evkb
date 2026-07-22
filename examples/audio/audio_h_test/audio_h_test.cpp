#include "Arduino.h"
#include "HardwareSerial.h"
#include <Audio.h>

void setup() {
    Serial1.begin(115200);
    while (!Serial1) {}
    Serial1.println("AUDIOH-GATE v1");
    Serial1.println("AUDIOH-DONE");
}
void loop() {}
