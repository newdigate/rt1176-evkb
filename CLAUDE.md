# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repo is

An Arduino/Teensyduino-style core for the NXP i.MX RT1176 (dual Cortex-M7 @
996 MHz + Cortex-M4) on the MIMXRT1170-EVKB board, derived from the Teensy 4.x
core. `README.md` and `examples/README.md` are accurate and detailed — read
them for anything not covered here.

The parent directory (`~/Development/rt1170/`) is a non-git workspace holding
NXP reference material only (reference-manual and board PDFs, `rm_full.txt` —
the reference manual as searchable text — and the EVKB RevC3 design files).
It is kept out of the repo deliberately: those files are NXP-copyrighted and
large.

### Git layout (important)

- This repo is `github.com/newdigate/rt1176-evkb`. The parent `rt1170/`
  directory is **not** a repo — run git from here (or `git -C evkb` from the
  parent).
- `cores/` and `teensy-cmake-macros/` are **nested independent git repos**
  (they show as untracked in this repo's status — that is normal).
- Peripheral libraries (Wire, SPI, Audio, SdFat, SD, Ethernet, NativeEthernet,
  FNET, lwip, USBHost_t36, …) live as sibling checkouts under
  `~/Development/<lib>`, each its own repo.

## Build (CMake only — no Arduino IDE)

Firmware lives in `examples/<category>/<name>/` (10 categories: dualcore, usb,
audio, networking, storage-memory, gpio-analog, timing, serial, display,
framework). Each example is self-contained; build from its own directory:

```sh
cd examples/gpio-analog/blink
cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake
cmake --build build          # produces <name>.elf (+ .hex)
```

(`storage-memory/sd_test` and `audio/sd_wav_play_test` inline their toolchain —
plain `cmake -B build` for those two.)

- Compiler: ARM GCC 10 at `/Applications/ARM_10/bin/` (override with
  `ARM_TOOLCHAIN_BIN`).
- Every example bootstraps via `../../../evkb.cmake`, which provides the
  `cores` library, the `teensy-cmake-macros` build macros, and
  `import_evkb_library(<name>)` for peripheral libraries.
- **Library resolution is local-first**: a `~/Development/<lib>` checkout wins
  (including uncommitted edits); if absent, the library is fetched from GitHub
  at a SHA pinned in `evkb.cmake`. `-DEVKB_FORCE_FETCH=ON` forces the pinned
  fetch ("fresh user" mode). After pushing new library work, the pin in
  `evkb.cmake` must be updated by hand.
- CM4 (second-core) images are built by the same macros
  (`teensy_add_cm4_image` / `teensy_add_cm4_slot_image`) and embedded into the
  CM7 ELF as C arrays.

## Test / verify — the two-gate rule

Every capability is verified twice, and both matter:

1. **QEMU gate** — each example has a `./run_qemu.sh` that boots the image on
   the custom `mimxrt1170-evk` QEMU machine (`~/Development/qemu2/`, from
   gitlab.com/Newdigate/qemu-rt1170) and asserts expected UART tokens.
   Run it as `./run_qemu.sh`, **never `sh run_qemu.sh`** — it re-execs itself
   under `gtimeout` (via `tools/gate-lib.sh`). QEMU runs through the
   `tools/qrun` wrapper (hard timeout + log cap) so a stuck gate can't burn CPU
   or fill the disk.
2. **Hardware verification** on the real EVKB with un-fakeable assertions
   (loopback jumpers, audible tones, real network traffic). Many examples keep
   `transcript_qemu.txt` / `transcript_hw_evkb.txt` as evidence.

**Silicon wins.** The QEMU model doesn't enforce clock gating/pin muxing
everywhere and stubs some devices; several real bugs only ever reproduced on
the board. Treat a QEMU pass as necessary but not sufficient — never weaken a
gate or the QEMU model to make a divergence disappear; document it instead.

There is a dedicated **`cm4-bringup` skill** — use it for any dual-core/CM4
work in this tree.

Repo-wide gates in `tools/`:
- `license-audit.sh` — proves no copyleft source is compiled into firmware
  (header sweep + link-manifest depfile audit). The tree is deliberately
  MIT/BSD-only; every inherited LGPL file has a clean-room rewrite. Don't
  introduce GPL/LGPL code or dependencies.
- `gate-lib.test.sh` — tests for the gate runner lifecycle library.

## Flash / run on hardware

```sh
pkill LinkServer; pkill redlinkserv        # always clear stale probe daemons first
LinkServer run MIMXRT1176:MIMXRT1170-EVKB build/<name>.elf   # load + reset + free-run
```

Use **LinkServer** (`/Applications/LinkServer_26.6.137/`), not pyOCD — pyOCD is
unreliable programming this board's FlexSPI NOR. Console is the MCU-Link VCOM
(`/dev/cu.usbmodem…`) at 115200; read it with pyserial
(`tools/rt1170-console.py <port> 115200`) — macOS `cat` silently resets the
port to 9600. Start the serial reader *before* triggering a reset if you need
boot output. `tools/rt1170-flash.sh` wraps flash + console;
`tools/rt1170-qemu.sh` boots an arbitrary image in QEMU outside the gate
harness.

## Architecture

- **`cores/imxrt1176/`** — the core: startup (FlexRAM config, 996 MHz
  OverDrive voltage), linker script `imxrt1176.ld` (XIP image at 0x30002000),
  Teensy-compatible API surface (GPIO, LPADC, FlexPWM, DAC, PIT/IntervalTimer,
  LPUART Serial, USB device stack, DMAChannel/eDMA, EventResponder,
  AudioStream), and the dual-core layer (`Multicore`, `MessagingUnit`,
  `Cm4ImageBank`). `cores/teensy4/` is an uncompiled upstream reference copy —
  never built.
- **Peripheral libraries are sibling repos**, not in-core: Wire (LPI2C),
  SPI (LPSPI), Audio (graph nodes + WM8962 codec driver), Ethernet stacks, etc.
  Core-vs-library boundary follows Teensy convention; several subsystems were
  deliberately moved out of the core into `newdigate/<lib>` forks.
- **Dual-core model**: the CM7 stages/boots/hot-swaps CM4 images and talks over
  the MU mailbox. Key constraints: main-eDMA completion IRQs reach the CM7
  only (CM4 interrupt-driven DMA needs eDMA_LPSR + an LPSR peripheral);
  the CM4 has no interrupt on fast-GPIO ports; DMA can't reach CM4 private
  TCM; each peripheral instance is assigned to one core.
- **Board traps**: header pin A5 (`GPIO_AD_08`) doubles as `USB_OTG2_ID` — an
  OTG adapter in the second USB port clamps A5 to 0 V and kills header I²C.
  FlexCAN RX mailboxes lock when their C/S word is read.
- `docs/` holds the QEMU peripheral status table, the RevC3 Arduino-header
  pin audit, and `docs/superpowers/{specs,plans}/` — timestamped design docs
  for past bring-up phases (historical record; old flat example paths in them
  are pre-2026-07-20).
