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
- **Nothing else lives inside this repo.** The core (`teensy-cores`), the
  build macros (`teensy-cmake-macros`) and all peripheral libraries (Wire,
  SPI, Audio, SdFat, SD, Ethernet, NativeEthernet, FNET, lwip, USBHost_t36,
  …) are sibling checkouts under `$TEENSY_LIB_ROOT/<lib>` (default
  `~/Development/<lib>`), each its own repo. A reappearing `?? cores/` or
  `?? teensy-cmake-macros/` in git status is a STALE in-repo copy from before
  2026-08-14 — it is dead (nothing resolves there); delete it.

## Build (CMake only — no Arduino IDE)

Firmware lives in `examples/<category>/<name>/` (11 categories: dualcore, usb,
audio, camera, networking, storage-memory, gpio-analog, timing, serial,
display, framework). Each example is self-contained; build from its own
directory:

```sh
cd examples/gpio-analog/blink
cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake
cmake --build build          # produces <name>.elf (+ .hex)
```

The two toolchain files (`rt1170-evkb.toolchain.cmake`, `rt1062-evkb.toolchain.cmake`)
live once at the repo root, in `toolchain/`, and are shared by every example —
a new example needs no `toolchain/` directory of its own.

Build dirs configured before 2026-08-14 cached an absolute toolchain path that
no longer exists; their elfs remain valid (gates run them unchanged), but the
first reconfigure fails with "toolchain file not found" — `rm -rf` the build
dir and configure fresh with the command above.

(`storage-memory/sd_test`, `audio/sd_wav_play_test`, `display/pxp_composite_test`
and `display/pxp_draw_bench` inline their toolchain, so none of the four need
the flag: `sd_test`/`sd_wav_play_test` `include()` the shared root file
directly, and the two PXP examples `set(CMAKE_TOOLCHAIN_FILE ...)` to it behind
an `if(NOT ...)` guard, so an explicit `-D` still wins.)

- Compiler: ARM GCC 10 at `/Applications/ARM_10/bin/` (override with
  `ARM_TOOLCHAIN_BIN`).
- Every example bootstraps via `../../../evkb.cmake`, which provides the
  `cores` library, the `teensy-cmake-macros` build macros, and
  `import_evkb_library(<name>)` for peripheral libraries.
- **Library resolution is local-first**: a `$TEENSY_LIB_ROOT/<lib>` checkout
  wins (default `~/Development`; env var to override — including uncommitted
  edits); if absent, the library is fetched from GitHub at a SHA pinned in
  `evkb.cmake`. This covers the core (`teensy-cores`) and the build macros
  (`teensy-cmake-macros`) too — the macros are the one repo fetched with plain
  FetchContent rather than CPM (the single CPM pin lives in the macros;
  `CPM_SOURCE_CACHE` covers everything else). `-DEVKB_FORCE_FETCH=ON` forces
  the pinned fetch ("fresh user" mode). After pushing new library work, the
  pin in `evkb.cmake` must be updated by hand.
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
`docs/KNOWN-BROKEN-GATES.md`.** The sweep covers **94 gates** (93 before
VGLite Phase 1 added `display/vglite_probe`; 92 before the
SynthUI Knob pilot added `display/synthui_knob_test`; 91 before the
step sequencer added `audio/step_seq_test`; 90 before the
transport added `audio/transport_test`; 89 before the
acid-bass voice added `audio/acid_bass_test`; 87 before Phase 5b
gated `usb/usb_audio_capstone_test` and `audio/audioinput_i2s_test`'s second
board; 86 before Phase 5a gated
`audio/audiooutput_i2s_test` on two boards; 84 before Phase 4 gated
`usb/usb_audio_uac1_test` on two boards; 83 before
`framework/string_test` was gated on a second board; 82 before Phase 2
gated `usb/usb_descriptor_survey` on a second board; 81 before the
RT1060 board axis gated `serial/serial_test` on a second board; 80 before Phase
7.4 added `dualcore/cm4_graph_usb_capstone`; 79 before Phase
7.3 added `dualcore/cm4_usb_audio_probe`; 78 before Phase
7.2c added `dualcore/cm4_usb_enum_probe`; 77 before Phase 7.1 added
`dualcore/cm4_usb_irq_probe`; 75 before Stage C added
`usb/usb_audio_duplex_test` and the emulated-device gate on
`usb/usb_descriptor_survey`). The target is **94 passed, 0 failed, 0 SKIP**, or
**93 passed, 1 failed, 0 SKIP** when the nondeterministic dual-core gate is red.

✅ **Measured 2026-08-16: 94 passed, 0 failed, 0 SKIP.** A fully clean sweep on
the merge of VGLite Phase 1, `rt1176:dualcore/cm4_audio_test` included. Note the
runner prints only non-zero categories, so `gates: 94 passed` with exit 0 IS
`94 / 0 / 0` — don't go looking for the zeros.

★ **Two gates are now SKIP-class on a fresh clone** — `display/synthui_knob_test`
and `display/vglite_probe` — a different failure mode from every other
documented exception, and the reason `docs/KNOWN-BROKEN-GATES.md` has an entry
for each. SynthUI and VGLite are both unpushed, so `import_evkb_synthui()` /
`import_evkb_vglite()` FATAL_ERROR, the examples cannot configure at all, and
the runner reports `(not built)` — invisible in the pass/fail columns and
visible only in the SKIP count. A fresh-clone sweep therefore reports **2 SKIP**;
check the NAMES against those two before concluding a sweep under-reported. On
this bench both checkouts exist, so both build and pass like any other gate.

★ **A green `display/vglite_probe` does NOT mean the GPU works.** QEMU has no
GC355 model, so that gate asserts the GPU-ABSENT fallback — the same ELF
detecting no GPU and taking the software path rather than spinning in
`vg_lite_init()`. The GC355 rendering is verified on silicon only, in the
example's `transcript_hw_evkb.txt`. This is the sharpest current instance of
"QEMU pass is necessary but not sufficient".

The previous baseline, kept because its account is still the reference for the
WM8962 lesson: **measured 2026-08-15: 92 passed, 0 failed, 0 SKIP**, a fully
clean sweep including the two dual-core Wire gates that were red earlier the
same day —
`cm4_wire_test` and `cm4_wire_int_master_test` had been asserting
`rdv=00000000`, a value produced only by QEMU's old WM8962 *stub*. That stub is
now a real model returning the true 0x6243 device ID, so both gates assert what
silicon asserts. Full account in `docs/KNOWN-BROKEN-GATES.md`.

★ **A QEMU model can change under you with no commit to show for it.** That
change was uncommitted in the qemu2 working tree and already compiled into the
binary, so `git log` on the model looked a month stale while the behaviour had
already moved. When a gate goes red and nothing in the firmware's history
explains it, check the model's WORKING TREE and the binary's mtime, not just
its log. Related: a concurrent session rebuilt that binary three times in an
hour and then removed it entirely with an ASan `configure`, which is enough to
invalidate a sweep taken across it and enough to make every gate report "no
UART capture". **Confirm with an untouched control gate before believing a
broad red.**

Beyond those there is **one** permitted red:

- `rt1176:dualcore/cm4_audio_test` — **nondeterministic.** Long called a load
  artefact, and load does not predict it: on 2026-08-08 it failed a sweep
  starting at load 6.8 and passed one starting at 8.6. Re-run before believing a
  red, and note that **consecutive readings are not a trend** — four in a row
  that day looked like a clean threshold near load 4 and the next measurement
  refuted it. Don't infer a load number; `docs/KNOWN-BROKEN-GATES.md` has all
  six readings.

Both the count and the pass/fail were measured that day; nothing here is
carried forward.
**0 SKIP is the load-bearing number in every case**: it is what says the
sweep actually covered everything rather than quietly measuring less.
Note `-l` prints a trailing "(N gate(s))" summary line, so `wc -l` on its
output is one more than the gate count. Any failure that is **not** that one is
a real regression from what you are doing — read the gate NAMES in the summary
rather than trusting the count alone.

**The tree is multi-board.** `EVKB_BOARD` selects `rt1176` (MIMXRT1170-EVKB,
the default) or `rt1062` (MIMXRT1060-EVKB). An example declares the boards it
supports in a `boards` sidecar file; absent means `rt1176` only, which is why
most examples have none. Gate ids are `<board>:<category>/<name>` and no gate
names a QEMU machine — `tools/gate-lib.sh` derives `-M`, `-global`, the build
directory and the `-serial` chain from the board. Build a non-default board with
`cmake -B build-rt1062 -DEVKB_BOARD=rt1062 -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1062-evkb.toolchain.cmake`.

`usb/usb_descriptor_survey` is gated on both boards and is the RT1062's USB
host proof: it enumerates QEMU's emulated `usb-audio` and reads `46F4:0002` off
the wire — an oracle the firmware has no knowledge of. Like
`dualcore/cm4_usb_irq_probe` and `audio/audioinput_i2s_test` (whose rt1062 half
needs the `sai1-rxinject` binding, qemu2 `2141a5d781`), its rt1062 half depends
on LOCAL-ONLY qemu2 changes, so **a fresh clone sees it red for that reason
too**; that is the GPL firewall working, not a regression.

★ **The RT1062 USB host needs the CCM_ANALOG SET/CLR/TOG aliases modelled.**
`hw/misc/imxrt1060_anatop.c` treated the `base+0x4/+0x8/+0xC` words as ordinary
storage rather than alias ports onto the base register, so every alias write
vanished. USBHost_t36's `PLL_USB2` powerup (`ehci.cpp:182-214`) drives that PLL
*only* through `_SET`, so it spun forever and `USBHost::begin()` never
returned. Worth knowing because of how it presents: **not** "USB does not
enumerate" but a hard hang in `setup()`, with the banner printed and the
2-second heartbeat absent. If an rt1062 image goes quiet after one line, check
whether it is looping on a register whose only writes go through an alias —
`-d unimp` will NOT show it, because the registers are all implemented.

Three per-board divergences are already known, all handled in `gate-lib.sh`,
and any new two-board example must go through it rather than spelling them out:
- **QEMU machine**: `mimxrt1170-evk` vs `mimxrt1060-evk`.
- **Boot property**: the RT1170 model's `boot-xip` is a boot-ROM stub that
  parses the real IVT; the RT1062 model has `boot-xip` AND `boot-ivt` as
  *different* properties selecting different reset vectors, and a Teensy-core
  image is the `boot-ivt` kind. Using the wrong one double-faults into Lockup
  before a line of firmware runs.
- **Which object is the console**: **LPUART1 on both boards**, but the cores
  name it differently — `Serial1` on the `imxrt1176` core, `Serial6` on
  the `teensy4` core (which follows the Teensy pin-0/1 convention and gives the
  name `Serial1` to LPUART6). Every two-board sketch therefore defines a
  `CONSOLE` alias rather than naming a `SerialN` directly, and every gate takes
  its chain from `gate_console` — which now emits a plain `-serial file:` for
  both boards, because both land in slot 0.
  ★ **This was got wrong first time and the mistake is worth knowing.** The
  rt1062 sketches originally printed to `Serial1` = LPUART6, and `gate-lib`
  emitted five `-serial null` to reach it. That passed in QEMU and was **useless
  on silicon**: on the MIMXRT1060-EVKB, LPUART6 only reaches Arduino header pins
  D0/D1, while the DAPLink/OpenSDA VCOM is wired to LPUART1
  (`GPIO_AD_B0_12/13` — `core_pins.h` pins 21/22). The gate and the bench were
  reading different wires, and nothing caught it until a hardware run was
  attempted. Get the slot wrong in either direction and the firmware runs
  perfectly while the capture stays empty — indistinguishable from firmware that
  never started.

★ **`rt1062` links the `teensy4` core, which is LGPL** — see the licence-audit
note under `tools/` below. That core is upstream Teensy, not the clean-room
`imxrt1176` core, and building it is what put copyleft source into a link
manifest for the first time in this tree.

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
  An entry may name a **build directory** (`examples/…/build-rt1062:target`)
  rather than an example directory, so a second board's build of the same
  example gets its own depfile walk. It needs one: `EVKB_BOARD=rt1062` links a
  different core.
  ★ **The GATES drift check accepts `<rel>/build*:` as well as `<rel>:`** (fixed
  Phase 5b). An example gated on a NON-default board only has no plain `build/`,
  so its build-directory entry is its *only* entry — and the old `"$rel:"`
  substring test false-positived on it. `usb/usb_audio_capstone_test` (rt1062
  only) is the first such example; before it every two-board example also built
  for rt1176, so a plain entry always existed and the narrower test was
  sufficient by accident. Left unfixed, the only ways to green the audit were
  deleting a real entry or writing a bogus `GATES_EXEMPT` — the check meant to
  keep GATES honest applying pressure to weaken it.
  ✅ **CLOSED 2026-08-08 — the audit PASSES, rt1062 builds included.** It was
  open for most of that day: `cores/teensy4/` sits in the audit's `ALLOW` list
  on the condition that its objects define **no** symbols, which held while
  that directory was "an uncompiled upstream reference copy — never built".
  The board axis builds it, and five of its files were LGPL-2.1 (`WString.cpp`,
  `IPAddress.cpp`, `Stream.cpp`, `WMath.cpp`, `Time.cpp` — all
  Arduino-inherited), compiling to real symbols in `libcores.o.a`.
  Resolved the only acceptable way — the five were **replaced** with the MIT
  clean-room versions (`cores` `99f7657`, pinned in evkb.cmake's manifest and
  pushed to `origin/master`), not excused by relaxing `ALLOW` or dropping the
  `build-rt1062` GATES entry. Two headers went the same way, `Printable.h` and
  `WCharacter.h`, because they were in the rt1062 link manifest and the
  EMPTY-object rule cannot see headers — they define no symbols. `Client.h` and
  `Server.h` still carry LGPL text and are deliberately left: no link manifest
  includes them. **Check the manifest, not this note, before adding a header.**
  Measured: `LICENSE-AUDIT: PASS` with both rt1062 entries walked —
  `serial_test/build-rt1062` 136 dep paths, `usb_descriptor_survey/build-rt1062`
  203.
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

- **`imxrt1176/` (in the `teensy-cores` sibling repo)** — the core: startup
  (FlexRAM config, 996 MHz
  OverDrive voltage), linker script `imxrt1176.ld` (XIP image at 0x30002000),
  Teensy-compatible API surface (GPIO, LPADC, FlexPWM, DAC, PIT/IntervalTimer,
  LPUART Serial, USB device stack, DMAChannel/eDMA, EventResponder,
  AudioStream), and the dual-core layer (`Multicore`, `MessagingUnit`,
  `Cm4ImageBank`). **`teensy4/` (same repo) used to be an uncompiled upstream
  reference copy; since the RT1060 board axis it is the core that
  `EVKB_BOARD=rt1062` actually builds.** That is what broke the licence audit
  for a day (see `tools/license-audit.sh` above) — it is upstream
  Teensy/Arduino code, and the five LGPL files it carried have since been
  replaced with the clean-room versions. Its linker scripts still differ from
  `imxrt1176.ld` in a way that matters for DMA: `.bss` goes to **DTCM** in
  `imxrt1060_evkb.ld`, `imxrt1062.ld` and `imxrt1062_t41.ld` alike, and only
  `.bss.dma` (`DMAMEM`) reaches OCRAM. So a buffer that is DMA-reachable by
  default on one board is not on the other.
  ★ **The two cores also differ on the D-cache, and DMA correctness depends on
  it.** `teensy4` enables it (`startup.c`, `SCB_CCR_IC | SCB_CCR_DC`);
  `imxrt1176` never writes `SCB_CCR` at all, so OCRAM is coherent there
  for free. On rt1062 it is not: a DMAMEM buffer is cached write-back unless the
  MPU says otherwise, so a CPU write can sit in cache while a bus master reads
  stale memory. That cost a full silicon debug session — the EHCI walked a
  periodic list of stale garbage and halted with **no error bit set**, because
  the port-connect ISR had already acked the fatal status. OCRAM is now mapped
  `MEM_NOCACHE` under `ARDUINO_MIMXRT1060_EVKB`. **Two facts follow: DMA buffers
  on rt1062 need OCRAM *and* that OCRAM must be uncached, and `SEI`/`UEI`
  reading 0 never proves no error occurred when an ISR is attached.**
- **Peripheral libraries are sibling repos**, not in-core: Wire (LPI2C),
  SPI (LPSPI), Audio (graph nodes + WM8962 codec driver), MipiDisplay
  (MIPI-DSI panels), Ethernet stacks, etc. Core-vs-library boundary follows
  Teensy convention; several subsystems were deliberately moved out of the core
  into `newdigate/<lib>` forks. MipiDisplay is split panel-independent `soc/`
  vs. per-panel `panels/<name>/`, so it is imported as
  `import_evkb_library(MipiDisplay soc panels/<name>)` — the panel is chosen by
  which directory the example imports (the RT1176 has one MIPI-DSI host, so
  only one panel can ever be live).
  **`VGLite` is the newest and the odd one out**: not a newdigate fork but NXP's
  MIT VGLite driver vendored verbatim (`VENDORING.md` records provenance), plus
  this tree's own `port/baremetal/` replacing the FreeRTOS port layer. It drives
  the **Vivante GC355 GPU2D** — which the RT1176 does have, contrary to what
  `docs/superpowers/specs/2026-07-27-rt1176-lvgl-design.md` claimed until that
  claim was corrected. Imported with `import_evkb_vglite()`;
  `VG_DRIVER_SINGLE_THREAD` is load-bearing, not decoration.
  ★ **Vivante wants 64-byte-aligned command buffers and a misaligned one does
  not fail — it hangs the front end while every API call returns
  `VG_LITE_SUCCESS`.** That cost most of Phase 1. When a device API insists
  everything worked, read the device's own status register (`AQHiIdle`, 0x004,
  bit 0 = front end).
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
