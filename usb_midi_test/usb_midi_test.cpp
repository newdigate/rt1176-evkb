#include "Arduino.h"
#include "HardwareSerial.h"
#include "USBHost_t36.h"

USBHost myusb;
// USBHub carries member DMA buffers the EHCI DMA master must reach (Pipe_t
// mypipes[2] / Transfer_t mytransfers[4] / setup_t setup / hub_desc[16]).  On
// RT1176 plain .bss is DTCM (DMA-unreachable), so place the whole objects in
// OCRAM via DMAMEM -- same trap/fix as the HID gate and as midi1 below.  The
// core zero-inits .bss.dma before global ctors run, so their non-in-class-
// initialised members are safe.  Two tiers for a low-speed MIDI device behind a
// hub's Transaction Translator (can't run on the EHCI root port directly).
DMAMEM USBHub hub1(myusb);
DMAMEM USBHub hub2(myusb);
DMAMEM MIDIDevice midi1(myusb);

static bool midi_seen = false;
void onNoteOn (uint8_t ch, uint8_t note, uint8_t vel) { Serial1.printf("NOTE_ON=%u,%u,%u\n",  ch, note, vel); }
void onNoteOff(uint8_t ch, uint8_t note, uint8_t vel) { Serial1.printf("NOTE_OFF=%u,%u,%u\n", ch, note, vel); }
void onCC     (uint8_t ch, uint8_t ctl,  uint8_t val) { Serial1.printf("CC=%u,%u,%u\n",       ch, ctl,  val); }

void setup() {
  Serial1.begin(115200);
  delay(10);
  myusb.begin();
  midi1.setHandleNoteOn(onNoteOn);
  midi1.setHandleNoteOff(onNoteOff);
  midi1.setHandleControlChange(onCC);
  Serial1.println("USB_MIDI_BEGIN");
}
void loop() {
  myusb.Task();
  if (midi1 && !midi_seen) {
    midi_seen = true;
    Serial1.printf("MIDI_CONNECT=%x:%x\n", midi1.idVendor(), midi1.idProduct());
    midi1.sendNoteOn(60, 100, 1);
    midi1.sendNoteOff(60, 0, 1);
    Serial1.println("MIDI_SENT");
  }
  midi1.read();
}
