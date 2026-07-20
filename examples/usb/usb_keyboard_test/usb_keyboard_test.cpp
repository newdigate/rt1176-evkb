// USB device HID keyboard gate: once the host configures the composite device,
// send a single KEY_A press then release on the interrupt-IN endpoint.
// The QEMU host taps the reports off the interrupt-IN and the driver asserts:
//   press   = 00 00 04 00 00 00 00 00   (modifier, reserved, key0..key5)
//   release = 00 00 00 00 00 00 00 00
#include "Arduino.h"
#include "HardwareSerial.h"
#include "usb_keyboard.h"   // Keyboard, KEY_A
#include "usb_dev.h"        // usb_configuration

extern volatile uint8_t usb_configuration;

void setup() {
    Serial1.begin(115200);                              // debug VCOM (LPUART1)
    uint32_t t0 = millis();
    while (usb_configuration == 0 && (millis() - t0) < 3000) { /* wait for host */ }
    Serial1.println(usb_configuration ? "USB=CONFIGURED" : "USB=TIMEOUT");
    delay(100);                                         // let the host arm the interrupt-IN
    Keyboard.press(KEY_A);                              // report: 00 00 04 00 00 00 00 00
    delay(10);                                          // separate the two reports
    Keyboard.release(KEY_A);                            // report: 00 00 00 00 00 00 00 00
    Serial1.println("KBD=SENT");
}

void loop() { }
