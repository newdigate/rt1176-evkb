# RT1176 USB MIDI Host — Design Spec

**Date:** 2026-07-09
**Status:** Approved (design)
**Builds on:** [[rt1176-usb-host-hid]] (the USB host controller on USB_OTG2, HW-verified)

## Goal

The RT1176 EVKB acts as a USB **MIDI** host: a USB MIDI controller enumerates on
USB_OTG2, and the RT1176 both **receives** MIDI (note-on/off, control-change) *and*
**sends** MIDI to the device. QEMU-gated via a new `usb-midi` device model + HW-verified.

## Scope

- **Bidirectional** (IN: host receives notes; OUT: host sends notes).
- Standard **`MIDIDevice`** (64-byte `MAX_PACKET_SIZE`, full-speed) — not the 512-byte
  `MIDIDevice_BigBuffer`.
- Full-speed MIDI enumerates **directly on OTG2** (like the keyboard); the `USBHub`
  carried from the HID gate covers the rare hub-connected/compound MIDI device.

## Architecture — mostly free; the QEMU device is the real work

`midi.cpp` is platform-generic USB-MIDI-class Teensy code — **no library change** beyond
DMAMEM-ing the driver object. The substantial piece is a new QEMU USB-MIDI device model.

| Repo | Change |
|---|---|
| `newdigate/USBHost_t36` | compile `midi.cpp` into the gate. **No source change**: `midi.cpp` has no file-scope DMA statics (verified) — its DMA buffers (`rx`/`tx1`/`tx2`/`queue`) are `MIDIDevice` members, covered by placing the object in DMAMEM. |
| `qemu2` (gitlab) | **new `hw/usb/dev-midi.c`** — a USB-MIDI-class device (`usb-midi`); register it in the build + device list. |
| `evkb` (local) | new gate `usb_midi_test/` (firmware + CMakeLists + runner + checker). |
| `newdigate/teensy-cores` | **none expected** — MIDI reuses the existing USB host infrastructure. |

## Component design

### C1. Firmware (`evkb/usb_midi_test/usb_midi_test.cpp`)

HID-analogous. `MIDIDevice` carries DMA member buffers → the **object** is `DMAMEM`
(the `.bss.dma` zero-init from [[rt1176-usb-host-hid]] covers its ctor-uninitialised
members). Connect detection + VID/PID come from the `USBDriver` base (`operator bool()`,
`idVendor()`, `idProduct()` at USBHost_t36.h:419–427) — **unambiguous** (single
inheritance, unlike `MouseController`). Callback signatures verified: `setHandleNoteOn`/
`Off(void(*)(uint8_t channel, uint8_t note, uint8_t velocity))`, `setHandleControlChange
(void(*)(uint8_t channel, uint8_t control, uint8_t value))`, `sendNoteOn(uint8_t note,
uint8_t velocity, uint8_t channel, uint8_t cable=0)`.

```cpp
#include "Arduino.h"
#include "HardwareSerial.h"    // Serial1 (LPUART1) markers
#include "USBHost_t36.h"

USBHost myusb;
USBHub  hub1(myusb), hub2(myusb);       // hub-connected / compound MIDI fallback
DMAMEM MIDIDevice midi1(myusb);          // rx/tx1/tx2/queue members are DMA -> DMAMEM

static bool midi_seen = false;
void onNoteOn (uint8_t ch, uint8_t note, uint8_t vel) { Serial1.printf("NOTE_ON=%u,%u,%u\n",  ch, note, vel); }
void onNoteOff(uint8_t ch, uint8_t note, uint8_t vel) { Serial1.printf("NOTE_OFF=%u,%u,%u\n", ch, note, vel); }
void onCC     (uint8_t ch, uint8_t ctl,  uint8_t val) { Serial1.printf("CC=%u,%u,%u\n",       ch, ctl,  val); }

void setup() {
  Serial1.begin(115200);
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
    midi1.sendNoteOn(60, 100, 1);        // TX: note 60, vel 100, channel 1
    midi1.sendNoteOff(60, 0, 1);
    Serial1.println("MIDI_SENT");
  }
  midi1.read();                          // drives the RX callbacks
}
```

Markers (over Serial1/LPUART1): `USB_MIDI_BEGIN`, `MIDI_CONNECT=vid:pid`,
`NOTE_ON=/NOTE_OFF=/CC=` (host RX), `MIDI_SENT` (host TX issued).

### C2. QEMU `usb-midi` device (`qemu2/hw/usb/dev-midi.c`) — the new build

A `TYPE_USB_DEVICE` implementing the USB-MIDI class:
- **Descriptors:** device + config with an **Audio-Control** interface (class 1 / subclass
  1, minimal header) + a **MIDI-Streaming** interface (class 1 / subclass 3) carrying
  embedded **IN + OUT MIDI jack** descriptors, and two endpoints — a **bulk IN**
  (device→host) and a **bulk OUT** (host→device) with the class-specific MS bulk endpoint
  descriptors. Full-speed (`bcdUSB` 0x0200, FS packet sizes).
- **IN endpoint:** after the host configures + polls, emit a short **USB-MIDI event-packet**
  sequence (4-byte packets: `09 90 3C 64` note-on ch1 note60 vel100; `08 80 3C 00`
  note-off) so the guest prints `NOTE_ON`/`NOTE_OFF`. Auto-emit on configure (or a short
  timer); a fixed sequence is sufficient for the gate.
- **OUT endpoint:** accept the guest's sent packets, decode, and log a
  `VMIDI: RX 90 3C 64` + `VMIDI=PASS` marker (via `qemu_log_mask`, mirroring the existing
  `chipidea_vhost` self-check style) so the runner can confirm the host's TX arrived.
- Attaches to OTG2's bus: `-device usb-midi,bus=usbhost.0,port=1` (same bus name +
  `port=1` pinning as the HID gate — avoids QEMU's auto-hub insertion; see
  [[rt1176-usb-host-hid]]).

### C3. Gate (`evkb/usb_midi_test/`)

Firmware C1 + `run_qemu_midi.sh` + `check_midi.py`, modeled on `usb_host_hid_test/`
(`qrun` + `gate-lib.sh`, `-icount shift=auto`, LPUART1 markers via `-serial file`).
Runner attaches `-device usb-midi,bus=usbhost.0,port=1`. CMakeLists = the HID gate's minus
the HID-only sources plus `midi.cpp` (still links `bluetooth.cpp` if keyboard/mouse remain;
if the MIDI gate drops keyboard/mouse, the BT coupling may not be pulled in — confirm at
build time). PASS = `USB_MIDI_BEGIN` + `MIDI_CONNECT` + `NOTE_ON` (host RX) + `MIDI_SENT`
+ `VMIDI=PASS` (host TX received by the device).

## Test strategy

- **QEMU gate** proves both directions end-to-end: enumerate `usb-midi` → device emits
  notes → host prints `NOTE_ON`/`NOTE_OFF`; host `sendNoteOn` → device logs `VMIDI=PASS`.
- **Hardware:** a real USB MIDI controller on OTG2 (full-speed, direct; VBUS via the
  USB-OTG adapter per [[rt1176-usb-host-hid]]) — play it → `NOTE_ON`/`NOTE_OFF`/`CC` over
  VCOM; host `sendNoteOn` → verify no TX error (and any device-side feedback, e.g. a
  controller that lights an LED / plays a tone). Flash + capture per
  [[rt1170-evkb-flashing]] + [[macos-serial-capture]].
- **★ Diagnostics reminder (learned the hard way in [[rt1176-usb-host-hid]]):** NEVER call
  `Serial1.print`/`write` from the USB ISR — it deadlocks the interrupt-driven TX. All
  firmware markers here are in `loop()` context (safe); any ISR-side debug must be a
  counter printed from `loop()`.

## Risks

| # | Risk | Mitigation |
|---|---|---|
| 1 | **USB-MIDI class descriptors** in `dev-midi.c` (jacks + MS endpoints — fiddly) | Follow the USB MIDI 1.0 spec (Audio class subclass 3); a minimal 1-in/1-out-jack config; validate the guest enumerates it |
| 2 | MIDI device connect-detection API | RESOLVED — `operator bool`/`idVendor`/`idProduct` from `USBDriver`, unambiguous |
| 3 | DMAMEM coverage | RESOLVED — no `midi.cpp` file-scope statics; DMAMEM the `MIDIDevice` object |
| 4 | MIDI device speed (FS-direct assumed) | FS enumerates direct like the keyboard; `USBHub` fallback present if a device is LS/behind a hub |
| 5 | QEMU IN-emission timing (device emits before host has the interrupt/bulk pipe up) | Emit on/after SET_CONFIGURATION + when the guest issues the first IN token; auto-retry |

## Open questions (resolve during planning/implementation)

- **O1.** Does `midi1.read()` auto-fire the `setHandle*` callbacks, or must the firmware
  also call the parsed getters? (verify the `read()` semantics — callback vs polled).
- **O2.** Exact bulk-vs-interrupt endpoint type the Teensy `MIDIDevice` expects on the MS
  interface (USB-MIDI uses **bulk**; confirm `midi.cpp`'s pipe setup so `dev-midi.c`
  matches).
- **O3.** Whether the MIDI gate keeps keyboard/mouse (and thus the `bluetooth.cpp` link) or
  is MIDI-only (simpler link).

## References

- USB host controller + gate infra this builds on: [[rt1176-usb-host-hid]]
- DMAMEM reachability + `.bss.dma` zero-init: [[rt1176-usb-host-hid]], [[rt1176-edma-dmachannel]]
- qemu2 chipidea host bus (`usbhost.0`, `port=1`), the `chipidea_vhost` self-check style
- Flashing + serial capture: [[rt1170-evkb-flashing]], [[macos-serial-capture]]
