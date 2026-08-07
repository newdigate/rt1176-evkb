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

**★ Before running `./tools/run-all-qemu-gates.sh`, read
`docs/KNOWN-BROKEN-GATES.md`.** The sweep covers **81 gates** (80 before Phase
7.4 added `dualcore/cm4_graph_usb_capstone`; 79 before Phase
7.3 added `dualcore/cm4_usb_audio_probe`; 78 before Phase
7.2c added `dualcore/cm4_usb_enum_probe`; 77 before Phase 7.1 added
`dualcore/cm4_usb_irq_probe`; 75 before Stage C added
`usb/usb_audio_duplex_test` and the emulated-device gate on
`usb/usb_descriptor_survey`). Expect **81 passed, 0 failed, 0 SKIP** on an idle
machine, or **80 passed, 1 failed, 0 SKIP** when the single permitted
intermittent (`dualcore/cm4_audio_test`) is red — the latter is what the
2026-08-06 Stage C sweep measured at `-j 2`.
The 81 is `run-all-qemu-gates.sh -l` measured on 2026-08-07 after 7.4 landed;
that sweep was NOT re-run end to end for 7.4 (the four Phase-7 gates were, all
green), so treat the pass/fail counts above as carried forward and the gate
COUNT as re-measured.
**0 SKIP is the load-bearing number in either case**: it is what says the
sweep actually covered everything rather than quietly measuring less.
Note `-l` prints a trailing "(N gate(s))" summary line, so `wc -l` on its
output is one more than the gate count. A single dual-core failure with everything else green is the known
load artefact described below; any *other* failure is a real regression from
what you are doing.

Three things that number depends on:

- **Gates do not build.** The runner assumes `build/<name>.elf` exists and
  reports a missing one as SKIP, not as a failure. A non-zero SKIP count means
  you measured less than you think — build every gate-owning example first, or
  the sweep quietly under-reports.
- **Do not count gates with a bare `find`.** Until 2026-08-06 discovery
  descended into `build*` directories, where `-DEVKB_FORCE_FETCH` clones
  peripheral libraries that carry gates of their OWN — four, in the SPI and
  SdFat trees. A raw find therefore returns 79, and the four extras become
  permanent SKIPs for images this repo never builds. That is worse than a
  wrong count: it destroys the SKIP signal above, which is the only thing
  that tells you a sweep under-reported. Discovery now prunes `build*`; ask
  the runner (`-l`) rather than `find`.
- **Dual-core gates are load-sensitive, not deterministic — and it moves
  between them.** `docs/KNOWN-BROKEN-GATES.md` records `cm4_audio_test` both
  ways on consecutive days. On 2026-08-06 a `-j 4` sweep run *while the
  machine was simultaneously building, flashing and driving hardware* failed
  `cm4_wire_int_slave_test` instead — a gate not previously suspect — while
  `cm4_audio_test` passed in that same run. The failing gate passed in 1 s
  when re-run idle and compiles none of the code that sweep was testing. So
  do not treat one red dual-core gate as a regression without re-running it
  idle first, and do not assume the susceptible gate is the documented one.
  Equally: do not delete, weaken, or drop any of them to make a sweep green.
  That file is the record; keep this line agreeing with it.

The baseline before that was `28 passed, 1 failed, 0 SKIP`, which had gone
stale by more than half the tree. A baseline that understates the gate count
that badly means a sweep can silently lose dozens of gates and still look
right — the same class of defect the audit's `GATES` drift check exists to
catch. Re-measure this line when you add gates.

Re-measuring means running the sweep, not counting files. On 2026-08-06 the
`74` above was judged stale on the strength of a bare `find` returning 79, and
it was not stale — the four extras were another repo's gates inside a build
directory, and 74 was exactly right. The number moved to 75 only because a
gate was genuinely added.

Repo-wide gates in `tools/`:
- `license-audit.sh` — proves no copyleft source is compiled into firmware
  (header sweep + binary-provenance check + link-manifest depfile audit). The
  tree is deliberately MIT/BSD-only; every inherited LGPL file has a clean-room
  rewrite. Don't introduce GPL/LGPL/MPL code or dependencies, and don't vendor a
  prebuilt binary without licence text beside it.
- `license-audit.test.sh` — negative tests proving the audit's part-1 checks
  actually fire (unlicensed binary, MPL header) rather than passing vacuously.
- `gate-lib.test.sh` — tests for the gate runner lifecycle library.
- `gate-vacuity.test.sh` — negative tests proving the *gates themselves* fail
  when they should: a run that produced no UART must fail by name rather than
  die silently or blame the firmware, and a missing counter token must not read
  as proof the good outcome happened. Drives real runners against a fake QEMU
  (via `qrun`'s `REAL_QEMU` hook) using each gate's committed
  `transcript_qemu.txt` as the fixture, so it needs no prior gate run — but it
  does need the examples it covers built.

## Flash / run on hardware

```sh
pkill LinkServer; pkill redlinkserv; pkill crt_emu_cm_redlink   # clear stale probe daemons
LinkServer run MIMXRT1176:MIMXRT1170-EVKB build/<name>.elf      # load + reset + free-run
```

**★ Do NOT hold the VCOM while programming.** With `tools/rt1170-console.py`
attached, `LinkServer flash … load` dies with `request to clear DAP error failed
- status 131` / `LOAD_EXIT=255` and the port re-enumerates mid-attempt. The
identical command succeeds the moment nothing holds the port. Working order:
`flash … load` → `flash … verify` (both VCOM-free) → **then** attach the reader
→ reset. There is no standalone `LinkServer reset` subcommand in 26.6.137;
backgrounding `LinkServer run` is how you trigger one. Note `pkill LinkServer`
alone leaves `redlinkserv`/`crt_emu_cm_redlink` resident, which silently kills
the next few runs.

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
  SPI (LPSPI), Audio (graph nodes + WM8962 codec driver), MipiDisplay
  (MIPI-DSI panels), Ethernet stacks, etc. Core-vs-library boundary follows
  Teensy convention; several subsystems were deliberately moved out of the core
  into `newdigate/<lib>` forks. MipiDisplay is split panel-independent `soc/`
  vs. per-panel `panels/<name>/`, so it is imported as
  `import_evkb_library(MipiDisplay soc panels/<name>)` — the panel is chosen by
  which directory the example imports (the RT1176 has one MIPI-DSI host, so
  only one panel can ever be live).
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
