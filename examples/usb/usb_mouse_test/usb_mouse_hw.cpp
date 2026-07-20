// Hardware-observation variant of the mouse gate (NOT the QEMU gate sketch).
// Jiggles the cursor right<->left every ~1 s (visible twitch, net-zero so it never
// runs off-screen), and echoes CDC in loop() so Serial is provably alive alongside
// the HID mouse. Keyboard tx / Serial1 debug run in thread context, never the USB ISR.
#include "Arduino.h"
#include "HardwareSerial.h"
#include "usb_mouse.h"      // Mouse
#include "usb_dev.h"        // usb_configuration

extern volatile uint8_t usb_configuration;

void setup() {
    Serial1.begin(115200);
    uint32_t t0 = millis();
    while (usb_configuration == 0 && (millis() - t0) < 5000) { /* wait for host */ }
    Serial1.println(usb_configuration ? "USB=CONFIGURED" : "USB=TIMEOUT");
}

void loop() {
    // Concurrent CDC echo: prove Serial still works next to the mouse.
    int n = 0;
    while (Serial.available() > 0) {
        int c = Serial.read();
        if (c < 0) break;
        Serial.write((uint8_t)c);
        n++;
    }
    if (n > 0) {
        Serial.send_now();
    }
    // Jiggle the cursor 30 px right then 30 px left every ~1 s (visible, net-zero).
    static uint32_t last = 0;
    static int8_t dir = 1;
    if (usb_configuration && (millis() - last) > 1000) {
        last = millis();
        Mouse.move(30 * dir, 0);
        dir = -dir;
        Serial1.println("MOUSE=JIGGLE");
    }
}
