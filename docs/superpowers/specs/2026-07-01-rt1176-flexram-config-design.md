# RT1176 Core — FlexRAM bank configuration (ITCM boot fault fix)

**Date:** 2026-07-01
**Status:** Approved (design), pending implementation plan
**Type:** Core bug fix (memory map), affects all phases.

## Goal

Configure the RT1176 FlexRAM ITCM/DTCM bank split at boot so that code placed in
ITCM by the linker is actually backed by ITCM RAM. This fixes an early-boot
`INVSTATE` UsageFault that appears on hardware once a sketch grows past the
BootROM-default ITCM size (uncovered by the Phase-3 LPADC work).

## Background / root cause

`imxrt1176.ld` places general code (`*(.text*)` — `main`, `setup`, `HardwareSerial`,
`Print`, `set_arm_clock_rt1176`, `systick_init`, `memset`, …) in **ITCM** (VMA
`0x00000000`, LMA in FLASH), copied from flash by `ResetHandler`. It sizes the ITCM
region to the actual code via linker symbols:
```
_itcm_block_count   = (SIZEOF(.text.itcm) + SIZEOF(.ARM.exidx) + 0x7FFF) >> 15;  /* 32K banks */
_flexram_bank_config = 0xAAAAAAAA | ((1 << (_itcm_block_count * 2)) - 1);
```
But **Phase-0 `startup.c` deliberately never applied `_flexram_bank_config`** (FlexRAM
config was deferred; `_estack` was parked in OCRAM as a workaround). So the real
FlexRAM split stays at the BootROM default.

**Mechanism:** while `.text.itcm` fits the default ITCM (small sketches — blink,
serial, RX echo), everything works. Once the image grows (the LPADC driver was
enough), `ResetHandler` copies the top of `.text.itcm` to ITCM addresses that the
default FlexRAM does **not** back with ITCM (they fall in DTCM/OCRAM banks). That
both leaves high ITCM code unbacked *and* corrupts DTCM data (e.g. the `Serial1`
vtable), so an early virtual call branches to a bad target → `INVSTATE`
UsageFault, stuck in `fault_isr`, before the first serial byte flushes. QEMU's
lenient model doesn't reproduce it; hardware does.

Confirmed via pyOCD: `CFSR = 0x00020000` (UFSR INVSTATE), ICSR VECTACTIVE=6
(UsageFault), faulting PC in the DTCM vtable region; bisected to *calling* (not just
linking) the LPADC code. See memory `rt1176-core-itcm-veneer-fault`.

## Reference (teensy4 upstream sequence)

teensy4's `ResetHandler` configures FlexRAM first thing:
```c
IOMUXC_GPR_GPR17 = (uint32_t)&_flexram_bank_config;  /* absolute linker symbol; its "address" IS the value */
IOMUXC_GPR_GPR16 = 0x00200007;                       /* FLEXRAM_BANK_CFG_SEL(bit2) + INIT_ITCM/DTCM enable */
IOMUXC_GPR_GPR14 = 0x00AA0000;                        /* TCM size fields */
```
`IOMUXC_GPR` base is `0x400E4000` (already `IOMUXC_GPR_BASE` in `imxrt1176.h`):
GPR14 @ `+0x38`, GPR16 @ `+0x40`, GPR17 @ `+0x44`.

## Architecture / change

Single-file change plus a generated-register addition. Two units:

1. **Register defs** (`tools/gen_imxrt1176_h.py` → `imxrt1176.h`): emit
   `IOMUXC_GPR_GPR14`, `IOMUXC_GPR_GPR16`, `IOMUXC_GPR_GPR17` (if not already
   present) at `IOMUXC_GPR_BASE + {0x38, 0x40, 0x44}`.

2. **FlexRAM init** (`startup.c` `ResetHandler`): as the **first** statements
   (before FPU enable and all memory copies), write GPR17 = `&_flexram_bank_config`,
   GPR16 = `0x00200007`, GPR14 = `0x00AA0000`. Declare
   `extern uint32_t _flexram_bank_config;` (already a linker symbol used by the ld
   script). `ResetHandler` and `memory_copy`/`memory_clear` are FLASH-resident
   (`section(".startup")`), so they execute before ITCM is populated — the GPR
   writes correctly precede any ITCM-resident call.

## What stays the same (scope guard)

- **`_estack` remains in OCRAM** (`0x202C0000`) — independent of FlexRAM, always
  present. Not moved to DTCM-top; that's an optional later cleanup, not needed here.
- **Linker script unchanged** — it already computes `_flexram_bank_config`.
- **MPU/cache setup deferred** — teensy4 also calls `configure_cache()` (MPU
  regions + cache enable); Phases 0–2 ran without it. Add it only if hardware still
  faults after the GPR config (incremental, one variable at a time).

## Data flow

Reset → BootROM → `ResetHandler` (FLASH): write GPR17/16/14 → FlexRAM re-banks so
ITCM = `_itcm_block_count` × 32K (rest DTCM) → `memory_copy(.text.itcm)` now lands
in real ITCM → `.data`/`.bss` init → RAM vector table → clocks/systick →
`__libc_init_array` → `main()`. All ITCM code now backed; virtual calls resolve.

## Error handling

- GPR writes are the first ResetHandler action; nothing ITCM-resident or
  stack-dependent-in-DTCM runs before them (stack is OCRAM).
- If hardware still faults after the GPR config, the single next step is the
  MPU/cache configuration (`configure_cache` equivalent) — added and retested in
  isolation.

## Testing / verification

1. **QEMU (regression gate):** the Phase-1 FlexRAM GPR model resizes TCM on the
   GPR17/16 writes. ALL existing QEMU gates must still pass:
   - blink (GPIO9 toggle), serial TX banner/counter, RX echo (`rx_isr=1`), and the
     LPADC gate (`adc1_ch5=341`, async `341`).
2. **Hardware (acceptance — the definitive proof):**
   - The previously-failing `analog_test` now **boots and prints `A0=<value>`** on
     silicon (optionally tracks GND/3V3 on GPIO_AD_06). This is the fix validation.
   - **Regression on silicon:** blink still toggles the LED; `serial_test`/`serial_test_rx`
     still print/echo over the VCOM. Confirms FlexRAM config didn't break the
     small sketches.

## Out of scope (YAGNI)

- Moving `_estack` to DTCM-top.
- MPU region setup / cache enable (unless required after the GPR fix).
- Any change to the LPADC driver (it is correct; the fault is core memory-map).
- FlexRAM reconfiguration at runtime (set once at boot).
