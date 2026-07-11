#include "Arduino.h"
#include "HardwareSerial.h"

void setup() {
    Serial1.begin(115200);          // debug VCOM (LPUART1)
    delay(50);
    Serial1.println("ENET_BOOT");
}
void loop() {}
