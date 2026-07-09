# RT1176 SDRAM (SEMC) Bring-up — Design Spec

**Date:** 2026-07-09
**Status:** Approved (design)
**Board:** MIMXRT1170-EVKB — 64 MB SDRAM on the SEMC

## Goal

Bring up the EVKB's 64 MB SEMC SDRAM so the `EXTMEM` macro works at `0x80000000` on
real silicon — with a **faithful QEMU gate** (the SDRAM window stays unmapped until the
guest completes the SEMC init) and a **hardware memory test** as the arbiter.

## Scope

- **Static `EXTMEM` placement** (data/buffers at `0x80000000`) + a **memory test**.
- **Deferred:** the dynamic `extmem_malloc`/`free`/`calloc`/`realloc` allocator — it is
  *declared* in `wiring.h` but stays unimplemented this cycle (a small follow-up heap
  layer on top of the raw bring-up).
- **Faithful gate:** QEMU's SEMC model gates the SDRAM window.

## Hardware configuration (from the SDK, `evkbmimxrt1170` SEMC SDRAM example)

The EVKB has **two 16-bit SDRAM chips forming a 32-bit data bus** (64 MB total), on SEMC
**CS0**, at `0x80000000`. The SDK config is explicit — use it verbatim:

| Param | Value |
|---|---|
| `address` / size | `0x80000000` / `2*32*1024 KB` = **64 MB** |
| `portSize` | **32-bit** (`kSEMC_PortSize32Bit`, two 16-bit chips) |
| `csxPinMux` | `kSEMC_MUXCSX0` |
| `burstLen` | 8 (`kSEMC_Sdram_BurstLen8`) |
| `columnAddrBitNum` | 9-bit |
| `casLatency` | 3 (`kSEMC_LatencyThree`) |
| timings | tRP 15 ns, tRCD 15 ns, tRFC/tXSR 70 ns, tWR 10 ns, tCKEoff 42 ns, tRAS 40 ns, tRRD 10 ns, tRC(refresh2refresh) 60 ns |
| refresh | 64 ms / 8192 rows; 8 auto-refresh cycles at init; `delayChain=2` |
| clock | `EXAMPLE_SEMC_CLK_FREQ` — the SEMC clock root (~166 MHz; **exact root/PLL = O1**) |

SEMC controller @ **`0x400D4000`**. Data bus + address + control = ~50 `GPIO_EMC_*` pads.

## Architecture & repos

| Repo | Change |
|---|---|
| `newdigate/teensy-cores` (`evkb/cores`) | SEMC register defs (generator) + ~50 `GPIO_EMC_*` pins + SEMC clock root/LPCG + **SEMC SDRAM init** (port `SEMC_ConfigureSDRAM`+`SEMC_SendIPCommand` from `fsl_semc.c`) run **early in `startup.c`**; add the **`EXTMEM`** macro |
| `qemu2` (gitlab) | replace the `semc-ctrl` unimplemented stub with a **minimal SEMC model** that starts `s->sdram` **disabled** and enables it on SDRAM-init completion |
| `evkb` (local) | new `sdram_test/` gate (EXTMEM buffer + memory test) |

## Component design

### C1. Core SEMC bring-up

- **Register defs** (`gen_imxrt1176_h.py` + regen): the SEMC peripheral @ `0x400D4000` —
  `MCR`, `IOCR`, `BMCR0/1`, `BR0..8`, `SDRAMCR0..3`, `DBICR0/1`, `IPCR0..2`, `IPCMD`,
  `IPTXDAT`, `IPRXDAT`, `INTEN`, `INTR`, `STATUS`, etc. (port names/offsets from the RT1176
  RM / CMSIS `PERI_SEMC.h`).
- **Pins:** the ~50 `GPIO_EMC_*` pads → SEMC ALT + pad control (32 data D0–D31, address
  A0–A12, BA0/1, CS0, RAS, CAS, WE, CKE, CLK, DQM0–3, DM). List from the board `pin_mux`
  (**O3**). One wrong mux = memory corruption.
- **Clock:** the SEMC clock root (~166 MHz) + its LPCG gate (**O1** for the exact root).
- **Init (`semc_sdram_init()`):** port `SEMC_ConfigureSDRAM` for the config table above —
  program the timing/geometry registers (timings derived from the clock), then the
  IP-command init sequence: **precharge-all → 8× auto-refresh → mode-register-set**
  (burst len + CAS latency), via a ported `SEMC_SendIPCommand` (IPCR/IPCMD + poll). Run in
  `startup.c` **before `__libc_init_array`** and before any `.externalram` access.
- **`EXTMEM` macro:** `#define EXTMEM __attribute__((section(".externalram"), used))`
  (parallel to `DMAMEM`) → static data lands in `.externalram` = `0x80000000`. The
  `.externalram` section stays **NOLOAD / not zero-initialised** (like Teensy's EXTMEM;
  zeroing 64 MB at boot is wasteful). Documented; an EXTMEM-C++-object zero-init is a
  future follow-up mirroring the `.bss.dma` zero-init in [[rt1176-usb-host-hid]].

### C2. QEMU faithful SEMC model

Replace the `semc-ctrl` unimplemented stub with a small `sysbus` device at `0x400D4000`:
- Accept the guest's config writes (BR0–8, SDRAMCR0–3, IPCR/IPCMD), enough to not
  `LOG_UNIMP`-spam and to observe the init.
- Create `s->sdram` **disabled** (`memory_region_set_enabled(&s->sdram, false)` at realize).
- Detect **SDRAM-ready** and enable the window: the trigger is the SDRAM init completing —
  candidate signals are BR0's chip-select-valid bit and/or the **MODESET** `IPCMD`
  completing (**O2** — pick the one the ported init actually reaches last). On that event,
  `memory_region_set_enabled(&s->sdram, true)`.
- Result: an `EXTMEM` access *before* a correct SEMC init reads nothing / faults — QEMU now
  catches a missing or out-of-order init (the point of the faithful gate). Scope the model
  to the RT1170 machine; don't disturb other users (there are none — SEMC is RT-specific).

### C3. Gate firmware (`evkb/sdram_test/`)

```cpp
#include "Arduino.h"
#include "HardwareSerial.h"
EXTMEM uint32_t buf[SDRAM_TEST_WORDS];   // lives at 0x80000000

// walking 1s/0s over a word (data lines); address-in-address over a stride sample
// spanning the 64 MB (address lines + aliasing); read-back verify.
setup(): Serial1.begin(115200); Serial1.println("SDRAM_INIT");
         run data-line test  -> "SDRAM_DATA=PASS|FAIL"
         run address test     -> "SDRAM_ADDR=PASS|FAIL"
         overall              -> "SDRAM_TEST=PASS|FAIL"
```
Markers over Serial1 (LPUART1). Optionally a first phase asserting a **pre-init access is
dead** (proving the faithful gate — read `0x80000000` before `semc_sdram_init`, expect
not-the-written-value / fault-handled).

## Test strategy

- **QEMU gate:** SEMC init → window enabled → memory test passes; a build with the init
  skipped/mis-ordered → window stays disabled → test fails. That's the faithfulness the
  RAM-backed model can't give. `qrun` + `gate-lib` + LPUART1 markers.
- **Hardware (arbiter):** flash, run the memory test across the full 64 MB — the real proof
  of pins + timing + refresh (stuck bits, address/data-line shorts, marginal timing). A
  32-bit-port test should exercise all 32 data lines. Flash/VCOM per [[rt1170-evkb-flashing]]
  + [[macos-serial-capture]].

## Risks

| # | Risk | Mitigation |
|---|---|---|
| 1 | **SEMC timing registers** — deriving tRP/tRCD/tRAS/tRFC/… from the clock + datasheet | SDK config is explicit (table above); `fsl_semc.c` does the ns→cycles math; HW is the arbiter |
| 2 | **~50 pins** — one wrong mux = corruption | Port the board `pin_mux` list verbatim; the memory test catches a bad data/address line |
| 3 | **QEMU gate trigger** — which write means "SDRAM ready" | O2 — instrument the ported init to see its last SEMC write; gate on that |
| 4 | **SEMC clock root** — wrong PLL/divider → wrong timing | O1 — resolve the exact root; ~166 MHz |
| 5 | EXTMEM not zero-initialised | Documented; matches Teensy; allocator/zero-init deferred |

## Open questions (resolve during planning)

- **O1.** Exact SEMC clock root + PLL/divider (what `EXAMPLE_SEMC_CLK_FREQ` resolves to;
  ~166 MHz — confirm the root index + source).
- **O2.** The precise register write that signals "SDRAM usable" for the QEMU window gate
  (BR0 valid vs MODESET `IPCMD` done).
- **O3.** The full `GPIO_EMC_*` pin list (pad/mux/ctl) from the board `pin_mux`.
- **O4.** Confirm nothing links `extmem_malloc` (declared in `wiring.h`) so it can stay
  unimplemented this cycle.

## References

- SDK SEMC SDRAM config: `mcuxsdk .../evkbmimxrt1170/driver_examples/semc/sdram/.../hardware_init.c`
- SEMC driver to port: `fsl_semc.c` (`SEMC_ConfigureSDRAM`, `SEMC_SendIPCommand`)
- Core linker already reserves it: `imxrt1176.ld` `ERAM @0x80000000 64M` + `.bss.extram`/`_extram_start/end`
- QEMU state: `hw/arm/fsl-imxrt1170.c` (`semc-ctrl` stub, `s->sdram` init_ram)
- Patterns reused — DMAMEM/section macros + `.bss.dma` zero-init lesson: [[rt1176-usb-host-hid]]; generator + gate infra: [[rt1170-qemu]], [[rt1170-gate-lib]]; flashing: [[rt1170-evkb-flashing]]
