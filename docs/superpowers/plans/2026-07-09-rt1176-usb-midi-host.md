# RT1176 USB MIDI Host Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** the RT1176 acts as a USB MIDI host — a USB MIDI controller on USB_OTG2 enumerates, the RT1176 receives note-on/off + control-change AND sends MIDI; QEMU-gated (new `usb-midi` device) + HW-verified.

**Architecture:** `midi.cpp` (USBHost_t36) is platform-generic — no library source change; the firmware just DMAMEM-places a `MIDIDevice` object and wires callbacks. The real build is a new `qemu2/hw/usb/dev-midi.c` USB-MIDI-class device (bulk IN emits notes, bulk OUT verifies the host's TX), attached to the already-working ChipIdea host bus (`usbhost.0`).

**Tech Stack:** C/C++ (Teensyduino core, RT1176 M7), QEMU (`mimxrt1170-evk`), CMake cross-build, LinkServer + LPUART1 VCOM.

**Spec:** `docs/superpowers/specs/2026-07-09-rt1176-usb-midi-host-design.md`. **Builds on** the HW-verified USB host (memory `rt1176-usb-host-hid`): bus name `usbhost.0`, `,port=1` pinning, DMAMEM driver objects, `.bss.dma` zero-init, **never `Serial1.print` from the USB ISR**.

---

## File Structure

| File | Responsibility | Change |
|---|---|---|
| `qemu2/hw/usb/dev-midi.c` | USB-MIDI-class device model | **new** (the substantial piece) |
| `qemu2/hw/usb/meson.build` + `qemu2/hw/usb/Kconfig` | build wiring | register `dev-midi.c` under `CONFIG_USB_MIDI` |
| `evkb/usb_midi_test/usb_midi_test.cpp` | gate firmware | new (~30 lines, DMAMEM `MIDIDevice`) |
| `evkb/usb_midi_test/CMakeLists.txt` + `toolchain/` | gate build (MIDI-only) | new |
| `evkb/usb_midi_test/run_qemu_midi.sh` + `check_midi.py` | runner + checker | new |

**Repos & commit targets (commit to `master`; do NOT push):** `Newdigate/qemu-rt1170` (qemu2), `evkb` (local). `newdigate/USBHost_t36` — **no change** (midi.cpp compiled as-is; confirm clean). Only `git add` named files.

---

## Task 1: QEMU `usb-midi` device model (`dev-midi.c`)

**Files:**
- Create: `qemu2/hw/usb/dev-midi.c`
- Modify: `qemu2/hw/usb/meson.build`, `qemu2/hw/usb/Kconfig`
- Template to copy structure from: `qemu2/hw/usb/dev-hid.c` (USBDesc framework, `handle_control`/`handle_data`, realize, class_init, type registration) and `dev-serial.c` (bulk IN/OUT data handling).

- [ ] **Step 1: Scaffold the device** from `dev-hid.c`'s shape: `#define TYPE_USB_MIDI "usb-midi"`, a `USBMIDIState { USBDevice dev; ... }`, `usb_midi_handle_control` (delegate to `usb_desc_handle_control`), `usb_midi_handle_data`, `usb_midi_realize` (`usb_desc_create_serial` + `usb_desc_init`), a `USBDescDevice`/`USBDescConfig`/`USBDescIface` chain, `VMStateDescription`, `usb_midi_class_init`, and `type_init`.

- [ ] **Step 2: USB-MIDI class descriptors** (the fiddly part — follow **USB MIDI 1.0 spec Appendix B**, the canonical simple adapter). Two interfaces in one config:
  - **AudioControl** interface: `bInterfaceClass=1` (AUDIO), `bInterfaceSubClass=1` (AUDIOCONTROL), with a CS_INTERFACE HEADER descriptor whose `baInterfaceNr` points at the MS interface. No endpoints.
  - **MIDIStreaming** interface: `bInterfaceClass=1` (AUDIO), `bInterfaceSubClass=3` (MIDISTREAMING), carrying (as the interface's class-specific "extra" descriptors — in QEMU, `USBDescIface.ndesc`/`.descs` of `USBDescOther` raw bytes):
    - CS_INTERFACE MS_HEADER,
    - MIDI_IN_JACK (embedded) + MIDI_IN_JACK (external),
    - MIDI_OUT_JACK (embedded) + MIDI_OUT_JACK (external),
    - two endpoints: **Bulk OUT** (host→device) + **Bulk IN** (device→host), each `bmAttributes=0x02` (bulk), FS `wMaxPacketSize=64`, each followed by a **CS_ENDPOINT MS_GENERAL** descriptor listing the associated jack ID.
  - Set `idVendor`/`idProduct` to recognizable non-zero values (e.g. `0x1209:0xMIDI`); FS device (`bcdUSB=0x0110` is fine for MIDI 1.0). **The driver reads the endpoint transfer-type from the descriptor (`rx_ep_type=p[3]`), so bulk here → the guest sets up bulk pipes (matches `midi.cpp`).**

- [ ] **Step 3: `handle_data` — IN endpoint emits a note sequence.** On an IN token to the bulk IN EP, return a fixed sequence of **USB-MIDI event packets** (4 bytes each), one packet per poll (or a small batch), then NAK:
  - `09 90 3C 64` = CIN 0x9 (note-on), MIDI `90 3C 64` (note-on ch1 note60 vel100)
  - `08 80 3C 00` = CIN 0x8 (note-off), MIDI `80 3C 00`
  - Optionally `0B B0 07 7F` = CIN 0xB (control-change), MIDI `B0 07 7F` (CC7=127).
  Use a per-device index; emit once after the guest starts polling, then NAK further INs. (Guest prints `NOTE_ON`/`NOTE_OFF`/`CC`.)

- [ ] **Step 4: `handle_data` — OUT endpoint verifies host TX.** On an OUT token to the bulk OUT EP, read the guest's 4-byte USB-MIDI packet(s), decode the embedded MIDI, and `qemu_log_mask(LOG_GUEST_ERROR, "VMIDI: RX %02x %02x %02x\n", ...)`. When the expected host note (`90 3C 64` from `sendNoteOn(60,100,1)`) is seen, log `VMIDI=PASS`. (Mirrors the `chipidea_vhost` self-check style; scoped to this device.)

- [ ] **Step 5: Register + build.** `meson.build`: `system_ss.add(when: 'CONFIG_USB_MIDI', if_true: files('dev-midi.c'))`. `Kconfig`: `config USB_MIDI \n bool \n default y \n depends on USB` (or fold into the existing always-on set the arm build pulls in — match how `CONFIG_USB_HID` reaches the `mimxrt1170-evk` build). Build: `ninja -C /Users/nicholasnewdigate/Development/qemu2/build qemu-system-arm 2>&1 | tail`.

- [ ] **Step 6: Smoke test** the device is recognized:
```bash
/Users/nicholasnewdigate/Development/qemu2/build/qemu-system-arm -M mimxrt1170-evk \
  -global fsl-imxrt1170.boot-xip=on -device usb-midi,bus=usbhost.0,port=1 \
  -S -monitor stdio -display none    # then: info qtree  -> usb-midi on usbhost.0
```
Expected: no "usb-midi is not a valid device" / no bus error; device shown on `usbhost.0`.

- [ ] **Step 7: Commit.** `git -C .../qemu2 add hw/usb/dev-midi.c hw/usb/meson.build hw/usb/Kconfig` → `usb: add usb-midi device (USB-MIDI 1.0 class; bulk IN emits notes, OUT verifies)` + trailer.

---

## Task 2: MIDI gate firmware + build proof

**Files:**
- Create: `evkb/usb_midi_test/{usb_midi_test.cpp, CMakeLists.txt, toolchain/, .gitignore}`
- Test: CMake configure + build → compiles + links

- [ ] **Step 1: Confirm no USBHost_t36 change needed.** `grep -nE "^static .*\[|DMAMEM" midi.cpp` → no file-scope DMA statics (already verified); DMA buffers are `MIDIDevice` members. So DMAMEM-ing the object suffices; `midi.cpp` compiles as-is.

- [ ] **Step 2: Firmware** `usb_midi_test.cpp` (loop-context markers only — NEVER print from an ISR):
```cpp
#include "Arduino.h"
#include "HardwareSerial.h"
#include "USBHost_t36.h"

USBHost myusb;
USBHub  hub1(myusb), hub2(myusb);
DMAMEM MIDIDevice midi1(myusb);          // rx/tx1/tx2/queue members are DMA -> DMAMEM the object

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
    midi1.sendNoteOn(60, 100, 1);        // note 60, vel 100, channel 1
    midi1.sendNoteOff(60, 0, 1);
    Serial1.println("MIDI_SENT");
  }
  midi1.read();                          // drains the rx queue + fires the setHandle* callbacks (loop context)
}
```

- [ ] **Step 3: CMakeLists (MIDI-only)** — model on `evkb/usb_host_hid_test/CMakeLists.txt` but drop the HID/bluetooth sources. USBHost_t36 sources to compile: `ehci.cpp enumeration.cpp hub.cpp memory.cpp midi.cpp` (+ `USBHost_t36.h` on the include path). **No** `hid.cpp`/`keyboard.cpp`/`mouse.cpp`/`bluetooth.cpp` → confirm the linker doesn't demand `BluetoothController` (MIDIDevice + USBHub should not reference it; if it does, add `bluetooth.cpp` + EEPROM include like the HID gate). Keep `<SdFat.h>`/SPI include-only paths that `USBHost_t36.h` needs (header-only, as in the HID gate). Copy `toolchain/` from `usb_host_hid_test`.

- [ ] **Step 4: Build.**
```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/usb_midi_test
rm -rf build && cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake
cmake --build build 2>&1 | tail -20
```
Expected: **builds to `usb_midi_test.elf`, no errors.** `nm` check: `midi1` in OCRAM (`0x2024xxxx`), like the HID driver objects.

- [ ] **Step 5: Commit** (evkb). `git add usb_midi_test/{usb_midi_test.cpp,CMakeLists.txt,toolchain,.gitignore}` → `usb_midi_test: MIDI gate firmware (DMAMEM MIDIDevice) — builds`.

---

## Task 3: QEMU MIDI gate — drive both directions, fix the device model

**Files:**
- Create: `evkb/usb_midi_test/run_qemu_midi.sh`, `evkb/usb_midi_test/check_midi.py`
- Modify (as surfaced): `qemu2/hw/usb/dev-midi.c`
- Test: gate asserts enumerate + RX notes + TX-received

- [ ] **Step 1: Runner** — model on `usb_host_hid_test/run_qemu_usbhost.sh`: `qrun` + `gate-lib.sh`, `-icount shift=auto`, `-serial file:$OUT` (LPUART1), `-device usb-midi,bus=usbhost.0,port=1`, `-d guest_errors -D $DIR/midi.dbg` (the `VMIDI=` markers land here). Then `check_midi.py`.

- [ ] **Step 2: Checker** `check_midi.py` — assert the marker file has `USB_MIDI_BEGIN`, `MIDI_CONNECT=<hex>:<hex>`, `NOTE_ON=1,60,100`, `NOTE_OFF=1,60,0`, `MIDI_SENT`; and the `.dbg` has `VMIDI=PASS` (host TX received). Print `USB_MIDI=PASS` iff all present.

- [ ] **Step 3: Drive + fix.** First run will likely expose `dev-midi.c` gaps (descriptor rejected → no MIDI_CONNECT; IN emission timing; OUT decode). Debug via the `.dbg` guest_errors + the marker log; the guest's own `midi.cpp` `claim()` parses the MS descriptors, so a malformed jack/endpoint descriptor → no claim → no `MIDI_CONNECT`. Iterate (rebuild qemu, rerun) to green. Keep fixes scoped to `dev-midi.c`.

- [ ] **Step 4: Green + determinism.** `USB_MIDI=PASS` twice from clean. Confirm the existing HID gate + device gates still pass (dev-midi.c is a new file; shouldn't touch them — quick `run_qemu_usbhost.sh` sanity).

- [ ] **Step 5: Commit** (evkb + qemu2 fixes). evkb → `usb_midi_test: QEMU gate (usb-midi RX notes + host TX verify)`; qemu2 → describe the dev-midi fixes.

---

## Task 4: Hardware verification (manual, controller-run)

- [ ] **Step 1:** Build the ELF; flash via LinkServer (`pkill -9 -f LinkServer; pkill -9 -f redlinkserv`, then `LinkServer run MIMXRT1176:MIMXRT1170-EVKB <elf>`), per memory `rt1170-evkb-flashing`.
- [ ] **Step 2:** Capture VCOM @115200 (pyserial + gtimeout, per `macos-serial-capture`); reader started before the reset to catch `USB_MIDI_BEGIN`.
- [ ] **Step 3:** Plug a **real USB MIDI controller** into OTG2 (full-speed, direct; VBUS via the USB-OTG adapter that grounds ID — per `rt1176-usb-host-hid`). Expect `USB_MIDI_BEGIN`, `MIDI_CONNECT=vid:pid` (real device), and **play the controller** → `NOTE_ON`/`NOTE_OFF`/`CC` over VCOM. The firmware's `sendNoteOn` fires on connect (`MIDI_SENT`) — verify no TX hang (and any device feedback: a tone/LED).
- [ ] **Step 4:** If no `MIDI_CONNECT`: check VBUS (controller powered? LED?), and whether the controller is FS-direct vs behind an internal hub (USBHub covers it). Record VID/PID + behavior for the memory note.

---

## Task 5: Final review, memory, commit hygiene

- [ ] **Step 1: Final code review** over the `dev-midi.c` + gate diff (dispatch a reviewer; focus: descriptor correctness, the IN/OUT data handling, no leftover diagnostics, working trees clean).
- [ ] **Step 2: Update memory** — extend `rt1176-usb-host-hid` (or a short new `rt1176-usb-midi`) with: MIDI = DMAMEM MIDIDevice + `read()`-fires-callbacks-in-loop; the new qemu2 `usb-midi` device (bulk IN/OUT, MIDI 1.0 descriptors); HW result. Update `MEMORY.md`.
- [ ] **Step 3: Confirm commits on `master`** (qemu2, evkb); **do NOT push**. Report SHAs.

---

## Self-review notes (author)

- **Spec coverage:** C1 (firmware)→T2; C2 (dev-midi.c)→T1; C3 (gate)→T3; HW→T4; risks #1 (descriptors)→T1 step 2 + T3 step 3, #2/#3 resolved, #4 (speed/hub)→T4, #5 (emission timing)→T1 step 3. O1 (`read()` fires callbacks in loop)→T2; O2 (bulk endpoints)→T1 step 2; O3 (MIDI-only gate)→T2 step 3.
- **Types consistent:** callback signatures `(ch,note,vel)`/`(ch,ctl,val)` and `sendNoteOn(note,vel,ch)` match USBHost_t36.h; `operator bool`/`idVendor` from `USBDriver` (unambiguous); `DMAMEM`, `usbhost.0`, `port=1` match the HID gate.
- **Placeholder check:** the one genuinely-external reference is "USB MIDI 1.0 spec Appendix B" for the descriptor bytes — a named authoritative source, not a TBD; the descriptor *structure* (interfaces/jacks/endpoints) is enumerated in T1 step 2.
