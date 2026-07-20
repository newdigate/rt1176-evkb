#include "Arduino.h"
#include "HardwareSerial.h"
#include "usb_serial.h"   // Serial (USB CDC)
#include "usb_dev.h"      // usb_configuration

extern volatile uint8_t usb_configuration;

void setup() {
    Serial1.begin(115200);          // debug VCOM (LPUART1)
    // usb_init() is auto-called in main() before setup().
    uint32_t t0 = millis();
    while (usb_configuration == 0 && (millis() - t0) < 3000) { /* wait for host */ }
    Serial1.println(usb_configuration ? "USB=CONFIGURED" : "USB=TIMEOUT");
}

void loop() {
    int n = 0;
    while (Serial.available() > 0) {
        int c = Serial.read();
        if (c < 0) break;
        Serial.write((uint8_t)c);   // echo it back to the host
        n++;
    }
    if (n > 0) {
        Serial.send_now();          // explicit flush (independent of GPTIMER0)
        Serial1.print("ECHOED ");
        Serial1.println(n);
    }
}
