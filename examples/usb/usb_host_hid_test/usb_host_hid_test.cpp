#include "Arduino.h"
#include "HardwareSerial.h"   // Serial1 (LPUART1) -- not pulled in by USBHost_t36.h
#include "USBHost_t36.h"

// RT1176 EVKB USB Host gate (USB_OTG2 / USBPHY2).  Sub-project A: HID keyboard +
// mouse over USBHost_t36.  Markers go out over Serial1 (LPUART1 / VCOM); connect
// edges are detected via each controller's operator bool + idVendor()/idProduct().
USBHost myusb;
// hid1/keyboard1 carry class-member DMA structures the EHCI DMA master must reach
// (USBHIDParser::_bigBuffer/mypipes[3]/mytransfers[5]/setup; KeyboardController::
// leds_).  On RT1176 plain .bss is DTCM (DMA-unreachable), so place the whole
// objects in OCRAM via DMAMEM.  The core zero-inits .bss.dma before global ctors
// run, so their non-in-class-initialised members are safe.  myusb has no instance
// data members and mouse1 has no DMA members, so both correctly stay in DTCM.
// USBHub: low-speed devices can't run directly on the EHCI root port (no TT) --
// they need a hub's Transaction Translator.  DMAMEM: USBHub carries member DMA
// buffers (setup_t etc.), same trap as hid1/keyboard1.  Two tiers for a compound
// USB3 hub (its internal USB2 hub can present as more than one).
DMAMEM USBHub        hub1(myusb);
DMAMEM USBHub        hub2(myusb);
DMAMEM USBHIDParser  hid1(myusb);
DMAMEM KeyboardController keyboard1(myusb);
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
