// USB device HID joystick gate: once the host configures the composite device, send one
// 12-byte joystick report on the joystick interrupt-IN endpoint (EP7). The QEMU host taps
// it (reactive hid_in_mask) and the driver asserts:
//   01 00 00 00 00 20 00 00 00 00 00 00
//   (button 1 -> data[0]=0x1; X=512 -> data[1]=512<<4=0x2000; data[2]=0)
#include "Arduino.h"
#include "HardwareSerial.h"
#include "usb_joystick.h"  // Joystick
#include "usb_dev.h"       // usb_configuration

extern volatile uint8_t usb_configuration;

void setup() {
    Serial1.begin(115200);                              // debug VCOM (LPUART1)
    uint32_t t0 = millis();
    while (usb_configuration == 0 && (millis() - t0) < 3000) { /* wait for host */ }
    Serial1.println(usb_configuration ? "USB=CONFIGURED" : "USB=TIMEOUT");
    delay(100);
    Joystick.useManualSend(true);   // update data[] without sending on each setter
    Joystick.button(1, 1);          // data[0] = 0x00000001
    Joystick.X(512);                // data[1] = 512 << 4 = 0x00002000
    Joystick.send_now();            // one report: 01 00 00 00 00 20 00 00 00 00 00 00
    Serial1.println("JOY=SENT");
}

void loop() { }
