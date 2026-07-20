#include "Arduino.h"
#include "HardwareSerial.h"
#include "usb_dev.h"     // usb_init, usb_configuration

extern volatile uint8_t usb_configuration;

void setup() {
    Serial1.begin(115200);
    while (!Serial1) {}
    usb_init();
    uint32_t t0 = millis();
    while (usb_configuration == 0 && (millis() - t0) < 3000) { /* wait for host */ }
    if (usb_configuration) Serial1.println("USB=CONFIGURED");
    else                   Serial1.println("USB=TIMEOUT");
}
void loop() {}
