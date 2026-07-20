# Arduino core for the i.MX RT1176 (MIMXRT1170-EVKB)

An Arduino/Teensyduino-style core for the NXP **i.MX RT1176** crossover MCU,
targeting the **MIMXRT1170-EVKB** evaluation board — dual-core Cortex-M7 @
996 MHz + Cortex-M4 @ 400 MHz, 2 MB on-chip SRAM, 64 MB SDRAM, 16 MB QSPI
flash, on-board WM8962 audio codec, 10/100 Ethernet, dual USB, and the classic
Arduino UNO-style header.

## Overview

This repository is the board bring-up tree: the core itself
(`cores/imxrt1176/`), the CMake build glue (`teensy-cmake-macros/`), 57
example/verification firmwares (`examples/`), and tooling (`tools/`).

- **Teensy 4 heritage.** The core is derived from the Teensy 4.x core
  (`PaulStoffregen/cores`), ported register-by-register to the RT1176. The
  familiar API surface works: `setup()`/`loop()`, `digitalWrite`, `analogRead`,
  `analogWrite` (FlexPWM), `analogWriteDAC0`, `tone`, `attachInterrupt`,
  `IntervalTimer`, `elapsedMillis`, `Serial`/`Serial1`, `SerialUSB`, `String`,
  `EventResponder`, `DMAChannel`, the Teensy Audio graph (`AudioStream`), USB
  device (CDC + HID keyboard/mouse/joystick, MIDI) and USB host, and more.
- **Dual-core is first-class.** The CM7 boots and manages the CM4 with
  `Multicore` (stage/boot/restart/`switchImage`), talks to it over the
  `MessagingUnit` (MU) mailbox/doorbell API, and can keep several CM4 firmware
  images resident at once and hot-swap between them with `Cm4ImageBank`
  (uniform ITCM slots, fast no-copy VTOR switch). CM4 sketches are compiled by
  the same build system and embedded into the CM7 image.
- **Everything is verified twice.** Each capability has a scripted QEMU gate
  (a custom [QEMU machine model](https://gitlab.com/Newdigate/qemu-rt1170) of
  the RT1176, both cores) *and* a hardware
  probe on the real EVKB with un-fakeable assertions. "Silicon wins" — QEMU
  divergences found on the board are documented, never absorbed silently.
- **Permissive licensing throughout.** The firmware tree is MIT/BSD/public-
  domain; every LGPL file inherited from upstream was replaced with a
  clean-room rewrite, and `tools/license-audit.sh` enforces this as a gate
  (it walks the actual build depfiles, not just the source tree).

Peripheral libraries (Wire, SPI, Audio, SdFat, SD, Ethernet, NativeEthernet,
FNET, lwip, USBHost_t36, EEPROM, Bounce2) are resolved **local-first with a
pinned-GitHub fallback** by `evkb.cmake`: a developer's `~/Development/<lib>`
checkout wins (uncommitted edits included), and when it's absent the library is
fetched from GitHub at a SHA pinned in the manifest — so a fresh clone builds
with no sibling checkouts at all.

## Getting started

**Prerequisites**

- macOS (the tree is developed on macOS; paths below reflect that)
- **ARM GCC 10** (`arm-none-eabi-gcc`) — default path `/Applications/ARM_10/bin/`;
  point the `ARM_TOOLCHAIN_BIN` environment variable at your toolchain's `bin`
  directory to override
- **CMake ≥ 3.24**
- **NXP LinkServer** (e.g. `/Applications/LinkServer_26.6.137/`) for flashing
  via the on-board MCU-Link — use LinkServer, not pyOCD (pyOCD is unreliable
  programming this board's FlexSPI NOR)
- Optional: the custom
  [**qemu-rt1170**](https://gitlab.com/Newdigate/qemu-rt1170) (`mimxrt1170-evk`
  machine) to run every example without hardware
- Optional: sibling library checkouts under `~/Development/` — used when
  present; otherwise fetched automatically from GitHub at pinned refs. Set the
  `CPM_SOURCE_CACHE` env var (e.g. `~/.cache/CPM`) so each repo is cloned once
  and shared across build directories

**Try an example**

```sh
cd examples/gpio-analog/blink
cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake
cmake --build build
./run_qemu.sh        # runs the QEMU gate — asserts the expected UART tokens
```

Examples are grouped by category under `examples/` (dualcore, usb, audio,
networking, storage-memory, gpio-analog, timing, serial, display, framework) —
see [examples/README.md](examples/README.md) for the full index.

**Flash the board**

```sh
pkill LinkServer; pkill redlinkserv     # always clear stale probe daemons first
LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load build/<name>.elf
```

Console output is on the MCU-Link VCOM (`/dev/cu.usbmodem…`) at **115200**.
Note: macOS `cat` resets the port to 9600 — read it with
`tools/rt1170-console.py <port> 115200` (pyserial; holds the baud and
auto-reconnects across board resets) or any terminal program that holds the
baud rate. `tools/rt1170-flash.sh <image.elf>` wraps the flash + console
combo, and `tools/rt1170-qemu.sh <image.elf>` boots an arbitrary image in the
QEMU machine outside the gate harness. If the board is silent after a
plain `flash load`, the debug probe left the core halted: power-cycle the
board, or use `LinkServer run MIMXRT1176:MIMXRT1170-EVKB <name>.elf` which
loads, resets and free-runs in one step.

## Build

The build is plain CMake — no Arduino IDE. `teensy-cmake-macros/` provides the
macros; each example is a self-contained consumer project:

```cmake
cmake_minimum_required(VERSION 3.24)
project(my_sketch)
include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)  # macros + cores + manifest

import_evkb_library(Wire)     # optional libraries, by manifest name — local
                              # checkout if present, else pinned GitHub fetch

teensy_add_executable(my_sketch my_sketch.cpp)              # .ino works too
teensy_target_link_libraries(my_sketch cores Wire)
target_link_libraries(my_sketch.elf stdc++)
```

`evkb.cmake` bootstraps `teensy-cmake-macros`, imports the `cores` library, and
defines the pinned manifest (`import_evkb_library(<name> [subdirs])`, plus
`evkb_library_dir(<name> OUT_DIR)` for cherry-picked sources). Configure with
`-DEVKB_FORCE_FETCH=ON` to ignore all local checkouts and build purely from
the pinned GitHub refs (the "fresh user" mode).

Configure with the board toolchain file (`TEENSY_VERSION 117`, core clock
996 MHz, `COREPATH` → `cores/imxrt1176/`, linker script `imxrt1176.ld`, XIP
image at `0x30002000`):

```sh
cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake
cmake --build build        # produces my_sketch.elf (+ hex)
```

**CM4 (second core) images** are built by the same macros and embedded into
the CM7 executable as C arrays:

```cmake
teensy_add_cm4_image(my_cm4 LINKER cm4.ld SOURCES cm4/startup_cm4.S cm4/main_cm4.c)
teensy_target_link_cm4_image(my_sketch my_cm4)
# or place it in a uniform ITCM slot for use with Cm4ImageBank:
teensy_add_cm4_slot_image(my_cm4 SLOT 0 SLOT_SIZE 0x1000 SOURCES ...)
```

At runtime the CM7 calls `Multicore.begin(my_cm4, sizeof(my_cm4))` to stage
and boot it, and exchanges data over `MU` (see `examples/dualcore/`).

## Status

Everything listed here has both a green QEMU gate and a hardware verification
on a real EVKB unless noted.

| Area | Status |
|---|---|
| Core runtime (startup, FlexRAM, 996 MHz w/ OverDrive voltage, delay/millis, yield) | ✅ HW-verified |
| Digital GPIO, `attachInterrupt`, header pin table (LED_BUILTIN = D3) | ✅ HW-verified |
| `analogRead` (LPADC), `analogWrite` (FlexPWM), `analogWriteDAC0` (DAC12) | ✅ HW-verified |
| `tone`, `IntervalTimer` (PIT), `elapsedMillis`, `EventResponder` | ✅ HW-verified |
| Serial (LPUART/VCOM), `SerialUSB` (USB CDC) | ✅ HW-verified |
| Wire (LPI2C master + slave, interrupt-driven) — sibling `Wire` lib | ✅ HW-verified |
| SPI (LPSPI, blocking + full-duplex DMA/async) — sibling `SPI` lib | ✅ HW-verified |
| eDMA / `DMAChannel` (Teensy DMAChannel port) | ✅ HW-verified |
| Audio graph: I2S in/out via SAI1 + WM8962 codec, WAV playback from SD | ✅ HW-verified (audible) |
| SD card (USDHC/SDIO via SdFat), flash-emulated EEPROM | ✅ HW-verified |
| 64 MB SDRAM (SEMC) + `extmem_malloc`, SNVS RTC | ✅ HW-verified |
| Ethernet 10/100: lwIP stack + Arduino `Ethernet` API, and FNET/NativeEthernet | ✅ HW-verified (DHCP/ping/TCP/UDP/DNS) |
| USB device: CDC + HID keyboard/mouse/joystick composite, MIDI | ✅ HW-verified |
| USB host: HID (keyboard/mouse via hub), MIDI, mass storage r/w | ✅ HW-verified |
| FlexCAN (CAN3 on J47), ST7735 display | ✅ HW-verified |
| **Dual-core:** CM4 boot (`Multicore`), MU IPC, CM4 GPIO/SPI/I2C, CM4 interrupt + DMA I/O (eDMA_LPSR), runtime hot-swap, `Cm4ImageBank` multi-image slots | ✅ HW-verified |
| CrashReport, MTP, USB audio/touch/rawhid/flightsim headers | ⚠️ present in tree, not verified on this board |

## Limitations

- **One board.** Only the MIMXRT1170-EVKB (RT1176) is supported — pin tables,
  linker script, clocks and the flash layout are board-specific. No Arduino
  IDE / arduino-cli integration; the build is CMake-only.
- **macOS-centric tooling.** LinkServer paths and the default toolchain
  location are macOS-flavoured (the compiler is overridable via
  `ARM_TOOLCHAIN_BIN`); other hosts may need small toolchain-file edits. The
  serial console must be read with something that holds 115200 (macOS `cat`
  drops it to 9600).
- **Pinned-manifest maintenance.** Fresh-clone builds fetch libraries at SHAs
  pinned in `evkb.cmake`; after pushing new library work, the pin must be
  updated by hand (push → paste the new SHA → commit) or fresh users keep
  building the older ref.
- **CM4 constraints.** The main eDMA's completion interrupts are wired to the
  CM7 only — CM4 interrupt-driven DMA requires the eDMA_LPSR instance and an
  LPSR-domain peripheral (LPI2C5/6, LPSPI5/6, LPUART11/12…). The CM4's fast
  GPIO ports (GPIO7-12 aliases) have no CM4 interrupt; DMA cannot reach the
  CM4's private TCM (use OCRAM for buffers). CM4 images are position-dependent
  (linked for a fixed ITCM address). No cross-core peripheral arbitration
  protocol yet — assign each peripheral instance to one core.
- **Audio on the CM4** is not supported: SAI/I2S DMA completion is CM7-domain,
  so the audio graph's clock source can't interrupt the CM4 (the graph runs on
  the CM7; the CM4 can serve as a DSP worker over MU/OCRAM).
- **Board traps worth knowing.** Header pin **A5** (`GPIO_AD_08`, also
  LPI2C1_SCL) is wired to `USB_OTG2_ID` — plugging a USB-OTG adapter into the
  second USB port clamps A5/SCL to 0 V and silently kills header I²C. FlexCAN
  RX mailboxes lock when their C/S word is read; drain via the proper sequence
  rather than tight-polling.
- **QEMU is a gate, not an oracle.** The
  [qemu-rt1170](https://gitlab.com/Newdigate/qemu-rt1170) model doesn't enforce
  clock
  gating/pin muxing everywhere and stubs some devices (e.g. codec register
  reads); several real bugs only ever reproduced on silicon. Hardware remains
  the final arbiter — treat a QEMU pass as necessary, not sufficient.
- **USB device classes** beyond CDC/HID/MIDI (audio, touch, rawhid, flightsim,
  MTP) are inherited headers, not yet ported/verified on this board.
