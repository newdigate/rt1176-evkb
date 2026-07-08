#include "Arduino.h"
#include "HardwareSerial.h"   // Serial1 (LPUART1) -- not pulled in by USBHost_t36.h
#include "USBHost_t36.h"

// RT1176 EVKB USB Host gate (USB_OTG2 / USBPHY2).  Sub-project A: HID keyboard +
// mouse over USBHost_t36.  Markers go out over Serial1 (LPUART1 / VCOM); connect
// edges are detected via each controller's operator bool + idVendor()/idProduct().
USBHost myusb;
USBHIDParser  hid1(myusb);
KeyboardController keyboard1(myusb);
MouseController    mouse1(myusb);

static bool kbd_seen = false, mouse_seen = false;

void onPress(int u) { Serial1.printf("KEY=%d\n", u); }

void setup() {
  Serial1.begin(115200);
  delay(10);
  myusb.begin();
  keyboard1.attachPress(onPress);
  Serial1.println("USB_HOST_BEGIN");
}

void loop() {
  myusb.Task();
  if (keyboard1 && !kbd_seen) {
    kbd_seen = true;
    Serial1.printf("KBD_CONNECT=%x:%x\n", keyboard1.idVendor(), keyboard1.idProduct());
  }
  // MouseController inherits BOTH USBHIDInput and BTHIDInput and (unlike
  // KeyboardController) declares no own operator bool / idVendor / idProduct, so
  // those are ambiguous on mouse1.  Wired USB host uses the USBHIDInput base
  // (its operator bool / idVendor read mydevice, set by USB enumeration).
  USBHIDInput &mouseHid = mouse1;
  if (mouseHid && !mouse_seen) {
    mouse_seen = true;
    Serial1.printf("MOUSE_CONNECT=%x:%x\n", mouseHid.idVendor(), mouseHid.idProduct());
  }
  if (mouse1.available()) {
    Serial1.printf("MOUSE=%d,%d,%02x\n", mouse1.getMouseX(), mouse1.getMouseY(), mouse1.getButtons());
    mouse1.mouseDataClear();
  }
}
