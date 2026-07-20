// Hardware-observation variant of the keyboard gate (NOT the QEMU gate sketch).
// Types 'a' every 2 s so `hidutil monitor` reliably captures the keystroke, and
// echoes CDC in loop() so `Serial` is provably alive alongside the HID keyboard
// (the composite CDC + Keyboard, both active at once). Keyboard tx + Serial1
// debug run in thread context (loop()), never the USB ISR.
#include "Arduino.h"
#include "HardwareSerial.h"
#include "usb_keyboard.h"   // Keyboard, KEY_A
#include "usb_dev.h"        // usb_configuration

extern volatile uint8_t usb_configuration;

void setup() {
    Serial1.begin(115200);
    uint32_t t0 = millis();
    while (usb_configuration == 0 && (millis() - t0) < 5000) { /* wait for host */ }
    Serial1.println(usb_configuration ? "USB=CONFIGURED" : "USB=TIMEOUT");
}

void loop() {
    // Concurrent CDC echo: prove Serial still works next to the keyboard.
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
    // Type 'a' every 2 s so the host can capture a repeatable keystroke.
    static uint32_t last = 0;
    if (usb_configuration && (millis() - last) > 2000) {
        last = millis();
        Keyboard.press(KEY_A);
        delay(10);
        Keyboard.release(KEY_A);
        Serial1.println("KBD=A");
    }
}
