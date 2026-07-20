// USB device HID mouse gate: once the host configures the composite device, send a
// single relative Mouse.move(10, 5) on the mouse interrupt-IN endpoint (EP6). The QEMU
// host taps it off the interrupt-IN and the driver asserts the 6-byte report:
//   01 00 0A 05 00 00   (report-ID 1, buttons 0, dx 10, dy 5, wheel 0, horiz 0)
#include "Arduino.h"
#include "HardwareSerial.h"
#include "usb_mouse.h"      // Mouse
#include "usb_dev.h"        // usb_configuration

extern volatile uint8_t usb_configuration;

void setup() {
    Serial1.begin(115200);                              // debug VCOM (LPUART1)
    uint32_t t0 = millis();
    while (usb_configuration == 0 && (millis() - t0) < 3000) { /* wait for host */ }
    Serial1.println(usb_configuration ? "USB=CONFIGURED" : "USB=TIMEOUT");
    delay(100);
    Mouse.move(10, 5);                                  // report: 01 00 0A 05 00 00
    Serial1.println("MOUSE=SENT");
}

void loop() { }
