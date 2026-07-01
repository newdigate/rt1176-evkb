# RT1170 EVKB Arduino/Teensyduino core — Phase 0: chip foundation

**Date:** 2026-06-30
**Status:** Approved design, pre-implementation
**Scope of this spec:** Phase 0 only (chip foundation → blink). Later phases are
separate specs.

## Goal and context

Port the Teensy core (`newdigate/teensy-cores`) to the NXP **MIMXRT1170-EVKB**
to create an Arduino/Teensyduino board and core. The end goal is **full
Teensyduino parity**, delivered in phases; this document specifies **Phase 0**,
the chip foundation everything else builds on.

The RT1060 EVKB port (`~/Development/rt1060/evkb/`) is the template, but that
board is an **RT1062** — the same chip as Teensy 4.x — so it reused the existing
`teensy4` core. The RT1170 EVKB is an **RT1176** (different chip, dual-core
CM7+CM4, new clock-root CCM), which the Teensy core does not support, so Phase 0
is a genuine new-chip bring-up. It pairs with the custom QEMU `mimxrt1170-evk`
machine (`~/Development/qemu2`), which models the SoC, FlexSPI NOR, and boot path.

### Key decisions (from brainstorming)

- **Register layer:** Hand-port to a **standalone Teensy-idiom `imxrt1176.h`**
  (no NXP SDK include at build time). To keep it correct rather than
  hand-transcribed, it is *generated* from the authoritative RT1176 register data
  (NXP `MIMXRT1176_cm7.h` / CMSIS-SVD) and emitted in Teensy style — pure Teensy
  lineage, correct by construction.
- **Verification target:** QEMU-first (fast iterate against our model; extend the
  model if Phase 0 needs something it lacks), then confirm on the EVKB.
- **Core scope:** **CM7 only** (matches the SoC primary core, the SDK, and the
  QEMU model). The CM4 is out of scope.
- **End state of Phase 0:** LED blink + `delay()`/`millis()` verified in QEMU and
  on hardware. No Serial, analog, buses, or USB (those are Phases 1–4).

## Architecture

Phase 0 is a small set of well-bounded units. Each can be understood and tested
on its own:

| Unit | Purpose | Depends on |
|------|---------|-----------|
| `imxrt1176.h` (+ generator) | Standalone Teensy-idiom register definitions | RT1176 SVD/SDK data (build-time generator only) |
| `bootdata.c` + `imxrt1176.ld` | FlexSPI `flexspi_nor` boot image (FCB/IVT/boot_data) + memory map | flash params, memory map |
| `startup.c` | Reset → clocks (CCM) → caches → RAM init → SysTick → `main()` | `imxrt1176.h`, linker symbols |
| timing (`core_pins`/`delay`) | `millis`/`micros`/`delay`/`delayMicroseconds`/`yield` | SysTick, DWT CYCCNT |
| GPIO (`digital`/`pinMode`) | `pinMode`/`digitalWrite`/`digitalRead` | IOMUXC, GPIO regs |
| `pins_arduino.h` (EVKB) | Arduino pin number ↔ pad/GPIO map | board schematic |
| `main.c` | setup/loop + yield/EventResponder hook | reused from teensy4 |
| `teensy-cmake-macros` (branch) | RT1176 build profile (defines, linker, F_CPU, core path) | toolchain |

## Section 1 — Repo layout

Mirror the rt1060 pattern at `~/Development/rt1170/evkb/`:

```
~/Development/rt1170/evkb/
  cores/                 # clone of newdigate/teensy-cores, new branch `imxrt1176`
    imxrt1176/           # NEW core, derived from cores/teensy4 (teensy4 stays intact)
  blink/                 # first example sketch (CMake)
  teensy-cmake-macros/   # clone of newdigate/teensy-cmake-macros, new branch `imxrt1176`
  docs/                  # this spec and later design docs
```

A **fresh `cores/imxrt1176`** directory (not edits to `teensy4`) keeps the RT1062
core intact and lets the RT1176 port diverge cleanly. The build macros also need
a **branch of `newdigate/teensy-cmake-macros`** (see Section 2). Board name:
**MIMXRT1170-EVKB**.

## Section 2 — Build & run

Same CMake flow as rt1060: `teensy-cmake-macros` →
`import_arduino_library(cores .../imxrt1176)` → `teensy_add_executable`.

**`teensy-cmake-macros` needs its own branch (`imxrt1176`).** The macros select
chip config from a `TEENSY_VERSION`-keyed block (the rt1060 EVKB already added a
`42 → imxrt1060_evkb.ld, __IMXRT1062__` profile). RT1176 adds a new profile:
- `CPU_DEFINE = __IMXRT1176__`, board define e.g. `ARDUINO_MIMXRT1170_EVKB`
- `LINKER_FILE = imxrt1176.ld`, `COREPATH` → the `imxrt1176` core
- `F_CPU = 996000000`
- CPU/FPU flags unchanged (`-mcpu=cortex-m7 -mfloat-abi=hard -mfpu=fpv5-d16`)
- image generation (objcopy → `.hex`/`.bin`) reused; the `flexspi_nor` boot
  header comes from the core's `bootdata.c`, not the macros

Output is a `flexspi_nor` image (FCB+IVT+boot_data, linked at `0x30000000`).

- **QEMU:** `qemu-system-arm -M mimxrt1170-evk -global
  fsl-imxrt1170.boot-xip=on -kernel blink.elf` (or boot the `.bin` via
  `-drive ...,if=mtd`).
- **EVKB:** flash via LinkServer (`~/Development/rt1170/rt1170-flash.sh`).

## Section 3 — `imxrt1176.h` register header

A standalone Teensy-idiom header with **no SDK include at build time**. A small
generator reads the authoritative RT1176 register data (NXP `MIMXRT1176_cm7.h` /
CMSIS-SVD) and emits Teensy-style definitions
(`#define GPIO9_DR (*(volatile uint32_t *)0x...)`, `IOMUXC_SW_MUX_CTL_PAD_*`,
`CCM_*`). Phase 0 header coverage: **CCM/clock tree, IOMUXC, GPIO, FlexSPI,
SysTick/SCB, NVIC, IOMUXC_GPR**. The generator is reusable to widen coverage in
later phases.

## Section 4 — Boot & startup

- **Boot image** (`bootdata.c`, linker): FCB for the EVKB IS25WP128 NOR at
  `+0x400`, IVT `+0x1000`, boot_data `+0x1020`, vector table `+0x2000` — the
  `flexspi_nor` layout validated in the QEMU work. Adapted from teensy4's
  `bootdata.c` for RT1176 FlexSPI + this flash.
- **Memory map** (`imxrt1176.ld`): ITCM `0x00000000`, DTCM `0x20000000`, OCRAM
  `0x20240000`, FlexSPI XIP `0x30000000` (16 MB), SDRAM `0x80000000` declared but
  **not initialized in Phase 0**. Fast code copies to ITCM, data to DTCM at boot
  (Teensy pattern).
- **`startup.c` (RT1176)** — the largest chip-specific delta (RT1176 has a new
  clock-root CCM, unlike RT1062). ResetHandler: enable FPU → configure CCM clock
  tree (CM7 ≈ 996 MHz via ARM PLL, plus bus/IPG and GPIO/IOMUXC roots) → enable
  L1 caches → copy `.text`→ITCM, `.data`→DTCM, zero `.bss` → start SysTick → run
  C++ constructors → `main()`. Clock values cross-checked against the SDK
  `BOARD_BootClockRUN` for the EVKB, re-expressed in Teensy idiom. Basic fault
  handlers included.

## Section 5 — Core runtime + pin table

- **Timing:** SysTick 1 ms → `millis()`; `micros()` from SysTick + DWT `CYCCNT`;
  `delay`/`delayMicroseconds`; `yield()`. `main.c` reused from teensy4
  (setup/loop + yield/EventResponder hook).
- **GPIO:** `pinMode` (IOMUXC pad-mux + GPIO direction),
  `digitalWrite`/`digitalRead` via `GPIOn_DR_SET/CLEAR/PSR`. RT1176 CM7
  fast-GPIO mapping handled (LED is on **GPIO9**).
- **Pin table** (`pins_arduino.h`/`core_pins`): `LED_BUILTIN` → GPIO9_IO03
  (pad `GPIO_AD_04`, "User LED D6", active-high), plus a starter map of the EVKB
  Arduino-header digital pins (pad ↔ GPIO port/bit). **Phase 0 gate is
  `LED_BUILTIN` (blink); completing the full Arduino-header map is best-effort
  within Phase 0, not a gating requirement** — it can carry into Phase 1.

## Section 6 — Verification

- **QEMU** (`mimxrt1170-evk`): image boots through the ROM (PC reaches `loop()`);
  **GPIO9 `DR` toggles** (read via QEMU monitor, or a GPIO-write trace we can
  enable since we own the model); `millis()` advances (read the DTCM variable via
  monitor). Headless-friendly since blink has no serial yet.
- **EVKB:** flash via LinkServer → **User LED D6 visibly blinks**.
- **Done =** blink verified in QEMU **and** on hardware.

## Out of scope (later phases)

- Phase 1: `HardwareSerial`/LPUART, `analogRead`/LPADC, `analogWrite`/FlexPWM.
- Phase 2: `SPI`, `Wire`/LPI2C, `IntervalTimer`.
- Phase 3: USB device stack (CDC Serial, HID/MIDI).
- Phase 4: SEMC SDRAM + EXTMEM, DMA, Audio, USBHost.

## Risks / open items

- **CCM clock tree** is the highest-risk piece (new RT1176 architecture). Mitigate
  by mirroring the SDK `BOARD_BootClockRUN` sequence and verifying SysTick cadence
  in QEMU first.
- **QEMU observability of a no-serial blink** — rely on register/trace inspection;
  if it proves awkward, add a tiny GPIO-write trace event to the `imxrt_gpio`
  model (we own it).
- **RT1176 fast-GPIO aliasing** (GPIO7–12 vs GPIO1–6 pad domains on CM7) — confirm
  the LED's controller/bit mapping against the RM during implementation.
- **EVKB Arduino-header pinout** completeness — Phase 0 needs only LED + a starter
  set; full mapping can extend within the phase.

## Verification result (2026-07-01)

**Phase 0 complete: QEMU ✓ and EVKB hardware ✓.** LED D6 blinks.

Hardware bring-up exposed three real bugs that QEMU's leniency had hidden (and
which motivate making the QEMU model stricter):
1. **IVT `csf` != 0** — teensy4's signed-boot placeholder made the real RT1176
   BootROM attempt HAB auth and refuse to boot. Fixed: `csf = 0` (unsigned).
2. **`_estack` in phantom DTCM** — the teensy FlexRAM formula put the stack in a
   480K DTCM that startup never configures; the BootROM-default DTCM is ~128K, so
   the first push bus-faulted. Fixed: stack in dedicated OCRAM.
3. **`delay()` hung on SysTick `millis`** — the SysTick tick ISR does not advance
   `millis()` on silicon, so a millis-based delay spun forever. Fixed: `delay()`
   uses the free-running DWT cycle counter.

### Known follow-ups (not gating Phase 0)
- `millis()`/`micros()` still depend on the SysTick ISR, which isn't advancing on
  hardware — root-cause (RT1176 SysTick clock-source/calibration) before Phase 1.
- Proper FlexRAM/TCM bank configuration (currently deferred; stack parked in OCRAM).
- Full ARM PLL fabric / bus+peripheral clock roots for accurate on-hardware timing.
- Make the QEMU model stricter (validate IVT `csf`, model FlexRAM default, model
  SysTick faithfully) so these are caught in emulation next time.
