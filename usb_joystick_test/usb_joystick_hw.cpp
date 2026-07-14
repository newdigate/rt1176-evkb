// Hardware-observation variant of the joystick gate (NOT the QEMU gate sketch).
// Every ~0.5 s: toggles button 1 and steps the X axis (0/341/682/1023), so a host
// gamepad tester shows live activity. Echoes CDC in loop() so Serial is provably alive
// alongside the HID joystick. Thread-context only, never the USB ISR.
#include "Arduino.h"
#include "HardwareSerial.h"
#include "usb_joystick.h"  // Joystick
#include "usb_dev.h"       // usb_configuration

extern volatile uint8_t usb_configuration;

void setup() {
    Serial1.begin(115200);
    uint32_t t0 = millis();
    while (usb_configuration == 0 && (millis() - t0) < 5000) { /* wait for host */ }
    Serial1.println(usb_configuration ? "USB=CONFIGURED" : "USB=TIMEOUT");
    Joystick.useManualSend(true);   // one combined report per step
}

void loop() {
    // Concurrent CDC echo: prove Serial still works next to the joystick.
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
    // Step button 1 + X every ~0.5 s so a gamepad tester shows movement.
    static uint32_t last = 0;
    static int step = 0;
    if (usb_configuration && (millis() - last) > 500) {
        last = millis();
        step = (step + 1) & 3;             // 0,1,2,3
        Joystick.button(1, step & 1);       // button 1 toggles each step
        Joystick.X(step * 341);             // X = 0, 341, 682, 1023
        Joystick.send_now();
        Serial1.println("JOY=STEP");
    }
}
