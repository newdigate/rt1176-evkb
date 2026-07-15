# RT1176-EVKB Arduino/Teensyduino core — next-session kickoff prompt

Paste everything below the line into a fresh Claude Code session started in
`~/Development/rt1170`.

---

We're building a from-scratch **Arduino / Teensyduino core for the NXP
MIMXRT1176-EVKB** (Cortex-**M7** @ 996 MHz, XIP from FlexSPI NOR, dual-core CM7+CM4
— we target the **CM7**). Every peripheral/API is brought up **test-first in a
custom QEMU model, then proven on real silicon**, and the QEMU model is refined in
lockstep so it faithfully mirrors the CM7 hardware. Refining QEMU is a first-class
deliverable, not just a means — when a gate exposes a model gap (an unwired IRQ, a
missing register), fix the model to match what the silicon actually does.

**This session: USB host mass storage — read/write files on a USB flash drive
(Sub-project B of the USB host work; `rt1176-usb-host-hid` explicitly deferred it).**

- Core (nested git repo, `master`, → github `teensy-cores`): `~/Development/rt1170/evkb/cores/imxrt1176`
  — **the git repo root is `evkb/cores`**; commit core changes from *inside* `cores/`
  (`git -C ~/Development/rt1170/evkb/cores/imxrt1176 …`), never from `evkb/` (where `cores/`
  shows as a nested untracked repo).
- Gates live in **evkb** — its own **LOCAL** git repo (`git -C ~/Development/rt1170/evkb …`,
  NOT `rt1170`, which isn't git). The working tree is shared across concurrent sessions.
- QEMU (git repo, `master`, → gitlab `qemu-rt1170`): `~/Development/qemu2` (machine `mimxrt1170-evk`)

**Working-tree heads-up:** `git status` in `evkb` will show pre-existing uncommitted WIP
that is NOT from the last finished task — the **USB-host HID + MIDI gate dirs**
(`usb_host_hid_test/`, `usb_midi_test/`) are **local-only, not pushed**, and there's older
`wire_*`/display experimentation (`ssd1306_display/`, `st7735_test/`, `mkr_ssd1306_test/`,
some `wire_*` edits/runners) still floating. `cores/` shows as *untracked* here only because
it's a **separate nested git repo** (→ github `teensy-cores`; core changes are committed —
and mostly pushed — from inside `cores/`). Don't sweep this WIP into your commits; stage only
files you actually touch. **Push ONLY when the user asks.**

## Method — follow it, it's been paid for in bugs

1. **Use the superpowers workflow for every peripheral:** `brainstorming` →
   `writing-plans` → `subagent-driven-development`. Do NOT write code before the
   design is approved (the brainstorming HARD-GATE). Process skills before
   implementation skills.
2. **The reference is ALWAYS the first port of call — before writing a single register.**
   - For a USB *class driver* this means the **Teensy `USBHost_t36`** source (below) plus
     the **USB Mass Storage Class / Bulk-Only Transport** spec, not a peripheral register
     map — the EHCI host controller (USB_OTG2) is already up.
   - When you *do* touch silicon (clocks, PHY, IRQ), read the **NXP MCUXpresso SDK
     example** on *this* board: `~/Development/mcuxsdk-ws/mcuxsdk/examples/_boards/
     evkbmimxrt1170/` and the **cm7** device header for ground truth
     (`.../devices/RT/RT1170/MIMXRT1176/MIMXRT1176_cm7_COMMON.h`). **Use the `cm7`
     header, never `cm4`** — IRQ numbers and which instances even have an interrupt line
     differ between cores.
   - Cross-reference **Zephyr** (`~/Development/zephyr`); only crack the **RM PDF**
     (`~/Development/rt1170/IMXRT1170RM.pdf`, large — grep/extract narrowly) for gaps.
   Every expensive detour in this project came from guessing instead of reading the
   reference; every fix came from reading it.
3. **TDD via a QEMU "gate":** write a self-contained sketch + `run_qemu_*.sh` runner
   that greps its VCOM output for `PASS`/`FAIL`. Make it fail, make it pass, THEN go
   to hardware. Copy an existing gate as a template — for USB host, `evkb/usb_host_hid_test/`
   and `evkb/usb_midi_test/` are the closest scaffolds (same `usbhost.0` bus, `qrun` guard,
   marker-grep checker).
4. **Hardware is the final arbiter.** A QEMU gate you built to match your own
   assumption proves *consistency, not correctness* — that bit us with a circular
   false-pass. Model what the silicon does, then let the gate **and** HW judge.

## Verification triad + the MKR Zero test-bed

- **QEMU gate** — fast, deterministic logic check; also where model gaps surface.
- **Saleae Logic** — signal-level truth (edges, bus traffic). Automate it:
  `from saleae import automation`, server `127.0.0.1:10430`, `TimedCaptureMode` → export
  raw digital CSV. Reference: `evkb/pwm_test/measure_pwm.py`. (Less relevant for MSC,
  where the real arbiter is the file surviving a round-trip through a PC.)
- **Hardware (EVKB)** — flash + read the board's own output.
- **MKR Zero (SAMD21, known-good Arduino) = the bench partner / test bed.** Run
  reference sketches on it to A/B a behaviour against a trusted Arduino core.

## Flashing & serial (macOS gotchas)

- **LinkServer, not pyOCD.** Reliable boot: `LinkServer run MIMXRT1176:MIMXRT1170-EVKB
  <img.elf>` (load+reset+run; clears the vector-catch that a plain `flash load`
  leaves the core halted on — and a USB replug is *not* always a true POR).
  Binary: `/Applications/LinkServer_26.6.137/LinkServer`.
- **Flash fails / probe stuck?** ALWAYS `pkill -9 -f LinkServer; pkill -9 -f redlinkserv`
  **before every flash** — a stale `redlinkserv` backend keeps the probe, so the next
  flash "Reconnected to existing LinkServer process" and dies ("stub terminated return
  code 1"). A *recurring* `Unable to retrieve DAPInfo: Hardware interface transfer error`
  even after killing both = physical probe fault → **unplug/replug the MCU-Link USB (J11)**
  (resets the probe AND power-cycles the target; clears a stuck FlexSPI NOR too). See the
  `rt1170-evkb-flashing` memory note.
- **VCOM console** = `/dev/cu.usbmodem5DQ2DDHVWO5EI3` @ **115200**. Read with
  **pyserial**, never `cat` (resets the port to 9600 on open → garbage). One-shot
  banners: start the pyserial reader first, then `LinkServer run`. See the
  `macos-serial-capture` note. **Note:** the board also enumerates a **native-USB CDC**
  port (`Serial`, VID 0x1209) — a *second* modem node, distinct from the MCU-Link VCOM;
  don't confuse them. (For USB-host gates the firmware prints markers over **`Serial1`**
  = LPUART1, so the native-USB port and OTG2's host role stay out of each other's way.)
- Toolchain: ARM GCC 10.2.1, `ARMGCC_DIR=/Applications/ARM_10`. QEMU build:
  `cd ~/Development/qemu2/build && ninja qemu-system-arm`; running needs
  `-global fsl-imxrt1170.boot-xip=on`.
- **QEMU runaway guard:** an orphaned `qemu-system-arm` with `-d guest_errors` once
  wrote ~100 GB and filled the disk. QEMU has no native log cap, so all `run_qemu_*.sh`
  runners go through **`evkb/tools/qrun`** (`QEMU=~/Development/rt1170/evkb/tools/qrun`):
  it runs QEMU under `gtimeout` (self-kills an orphan) and caps the `.dbg` log at 100 MB.
  **Copy an existing runner for any new gate** so it inherits the guard (and `gate-lib.sh`'s
  `gate_init`/PID+temp reaping). `*.dbg`/`*.uart` are git-ignored.

## Your memory already holds the hard-won lessons

MEMORY.md auto-loads. **Read the note relevant to this task before touching it.**
Especially, for USB host mass storage:

- **`rt1176-usb-host-hid`** — the host controller this builds directly on. EHCI on
  **USB_OTG2**; the virtual host device attaches to bus **`usbhost.0`** pinned to
  **`port=1`**. ★HW traps to reuse verbatim: **OTG2 VBUS is ID-gated hardware**
  (R160/R162 DNP, no firmware hook → ground the ID pin with an **OTG adapter**);
  **DMAMEM the driver *objects*** (not just statics); **NEVER `Serial1.print` from the
  USB ISR** (deadlocks the interrupt-driven TX — this masqueraded as a device "crash" for
  many iterations); **LS devices need a hub** (flash drives are HS/FS so direct is fine).
- **`rt1176-usb-midi`** — another host-class driver example (device class over the same
  EHCI). Note the pattern: a MIDI-only gate links **without `bluetooth.cpp`**; and a new
  device class may need a *new* qemu `dev-*.c` model (MIDI did) — **but MSC does NOT**
  (see First move: qemu has a built-in `usb-storage`).
- **`rt1176-sd-usdhc`** — the **FAT / SdFat / `SD.h`** filesystem layer to reuse. MSC is
  just another block backend under the *same* SdFat stack; the FAT layer is silicon-agnostic
  and already brought in. Also: card images must be **MBR** (SdFat has no superfloppy fallback).
- **`rt1170-evkb-flashing`** + **`macos-serial-capture`** — HW flash (LinkServer) + VCOM capture.

Subagent caveat: **verify a subagent's real git/file state — don't just trust its
self-report.** Independently `diff` a ported file against its source and re-run the gate
yourself. Read-only-review subagents are especially reliable (a read-only review is what
caught the CM7 GPIO bug). **Watch for the CMake `file(GLOB)` trap:** adding a NEW core
`.c`/`.cpp` needs a from-scratch reconfigure (`rm -rf build && cmake -B build …`) of every
test dir, not just a rebuild (no `CONFIGURE_DEPENDS`). (MSC should be **all library-side**
over the existing EHCI, so new *core* files are unlikely — but the gate still adds new
*library* sources, so start its `build/` clean.)

## Status

**Done, HW-verified:** startup/clocks/SysTick, millis/micros/delay, GPIO + full
Arduino-header pin table (`LED_BUILTIN=3`), digital I/O, LPUART `Serial1`, LPADC
`analogRead`, FlexRAM, 996 MHz OverDrive voltage, `Wire` (I²C master+slave, now a
`newdigate/Wire` lib), `SPI` (LPSPI1, master + full-duplex DMA, now a `newdigate/SPI` lib),
FlexPWM `analogWrite`/`Freq`/`Res`, `attachInterrupt`, `IntervalTimer` (PIT1), `tone()`,
USB CDC `Serial`/`SerialUSB`, **I²S/SAI TX+RX + WM8962 codec**, **EDMA/`DMAChannel`**,
**`EventResponder`**, the **`AudioStream` graph** + `AudioInputI2S`/`AudioOutputI2S` nodes,
**flash-emulated `EEPROM`**, the **SD card (USDHC/SDIO) + SdFat/`SD.h`** stack,
**`AudioPlaySdWav`** (WAV off SD → J101), and the **USB host controller (EHCI on
USB_OTG2)**: **HID keyboard + mouse** and **USB MIDI** both enumerate and stream on real
silicon. Most core work is pushed (`teensy-cores`/`qemu2`/`SdFat`/`Audio` master); the
**USB-host HID + MIDI gate dirs are local-only** (see the working-tree heads-up).

**This session (Sub-project B of USB host):** **USB host mass storage** — enumerate a USB
flash drive on OTG2 and read/write files (FAT). **After this:** FlexCAN, RTC (SNVS),
Watchdog (WDOG/RTWDOG) remain on the horizon.

## First move — USB host mass storage (read/write files on a USB flash drive)

**Goal:** enumerate a **USB flash drive** on **USB_OTG2** and **read/write files** on it
(FAT). This is **Sub-project B of the USB host work** — `rt1176-usb-host-hid` brought up
the EHCI host controller (kbd/mouse/MIDI, all HW-verified) and explicitly **deferred mass
storage**. The controller is done; this session is the **MSC class driver + the FAT mount**
on top of it. **Start by invoking the brainstorming skill and exploring the landscape below
before proposing a design.**

**1. Teensy reference — `USBHost_t36` (`~/Development/USBHost_t36`).** The MSC driver is
   **`MassStorageDriver.cpp`** (NOT a file called `msc.cpp` — the SCSI/BOT constants +
   structs live in **`utility/msc.h`**; the classes live in **`USBHost_t36.h`**; helpers
   are `utility/USBFilesystemFormatter.{h,cpp}` and `mscSenseKeyList.h`). It implements
   **USB Mass Storage Class = Bulk-Only Transport** (CBW sig `0x43425355` "USBC" / CSW sig
   `0x53425355` "USBS" over the bulk-IN/OUT pipes) **+ SCSI**: `INQUIRY` (0x12),
   `TEST UNIT READY` (0x00), `READ CAPACITY(10)` (0x25), `READ(10)` (0x28), `WRITE(10)`
   (0x2A), `REQUEST SENSE` (0x03). **★The reuse win — MSC is just another SdFat block
   backend:** `class USBDrive : public USBDriver, public FsBlockDeviceInterface` presents
   the drive as an **SdFat block device** (`readSectors()`/`writeSectors()`/`sectorCount()`,
   512-byte sectors), and `class USBFilesystem : public USBFSBase (: public FS)` holds an
   SdFat `FsVolume mscfs` mounted over that block device behind the Arduino `FS` API. So the
   **FAT layer is the *same* SdFat stack the SD-card bring-up (`rt1176-sd-usdhc`) already
   pulled in** — silicon-agnostic, nothing to re-port. `USBHost_t36.h` already
   `#include <SdFat.h>`. Reference sketch: **`USBHost_t36/examples/Storage/`**.

**2. QEMU — REUSE the built-in `usb-storage` device (no new model).** Unlike `usb-midi`
   (which needed a net-new `hw/usb/dev-midi.c`), QEMU ships a mature MSC device:
   **`usb-storage`** (`TYPE_USB_STORAGE`, `~/Development/qemu2/hw/usb/dev-storage.c`) — a
   BOT+SCSI device backed by a `BlockBackend` + a `scsi-disk`. Attach it to OTG2's host bus
   with a backing image, mirroring the HID/MIDI runners' `bus=usbhost.0,port=1` pinning:
   ```
   -drive if=none,id=stick,file=usb.img,format=raw \
   -device usb-storage,drive=stick,bus=usbhost.0,port=1 \
   -icount shift=auto -display none -serial file:"$OUT"
   ```
   `bus=usbhost.0` = USB_OTG2's host root (given a stable id in `fsl-imxrt1170.c`); `port=1`
   pins the device to the single physical root port so QEMU's convenience auto-hub doesn't
   splice a `usb-hub` in front (same reason HID/MIDI pin it). **Unlike HID/MIDI, MSC needs
   NO QMP injection** — it's bulk-only, so the gate just drives the firmware: mount, **write
   a file, read it back byte-exact, list the dir** (markers over `Serial1`). Format the
   backing image **FAT with an MBR** (like the SD `card.img`, per `rt1176-sd-usdhc` — SdFat
   has no superfloppy fallback). Assess the `usb-storage`/SCSI/BlockBackend path is complete
   enough for the CBW/CSW + `READ(10)`/`WRITE(10)` the driver issues; a gap here is the
   QEMU-fix contingency, but reusing a mature device makes that unlikely.

**3. Build the gate — copy `usb_host_hid_test/` or `usb_midi_test/`.** Same CMake shape
   (`import_arduino_library(cores …)` → `teensy_add_executable` → `target_sources(<name>.elf
   PRIVATE …)`), same `qrun`/`gate-lib.sh` runner. Compile+link the USBHost host core
   (`${USBHOST}/{ehci,enumeration,hub,memory}.cpp`) **+ `MassStorageDriver.cpp` +
   `utility/USBFilesystemFormatter.cpp`**, and — the difference from the HID gate — now
   **actually compile & link the SdFat sources + the FS layer** (the HID gate carried
   `~/Development/SdFat/src` as *include-only* precisely because it *excluded*
   `MassStorageDriver.cpp`; the MSC gate references SdFat symbols, so it must build them).
   **`MassStorageDriver.cpp` references `BluetoothController` 0 times** → link **WITHOUT
   `bluetooth.cpp`** (like the MIDI gate; the HID gate only needs it because
   keyboard/mouse hard-reference it). `SdFat.h` also drags in `SPI.h` (`SdSpiDriver`) — keep
   `~/Development/SPI` and `~/Development/EEPROM` on the include path as the HID gate does.

**4. HW (real arbiter).** A **real USB flash drive on OTG2 via the OTG adapter that grounds
   the ID pin** (OTG2 VBUS is ID-gated hardware — R160/R162 DNP, no firmware hook, per
   `rt1176-usb-host-hid`). Enumerate the stick, **write a file, read it back on the board,
   then verify that file on a PC** (round-trip is the truth MSC gates can't fully fake).
   **Reuse the host-side lessons:** **DMAMEM the driver objects** (`USBDrive`/`USBFilesystem`,
   not just statics); **NEVER `Serial1.print` from the USB ISR** (deadlock); a real flash
   drive is HS/FS so direct attach is fine, but **an LS device (or a bus-powered hub) needs
   a `USBHub`** if you exercise that path.

Method: **brainstorm → spec → `writing-plans` → `subagent-driven-development`, gate-first**
(no code before the brainstorming hard-gate). Decompose during brainstorming: **(A)** the
MSC block driver (BOT + SCSI over the EHCI bulk pipes) with a **raw sector read/write** gate,
then **(B)** the **SdFat/FS mount** on top (file write / read-back / dir-list). Builds
directly on `rt1176-usb-host-hid`; the FAT layer is `rt1176-sd-usdhc` reused.
