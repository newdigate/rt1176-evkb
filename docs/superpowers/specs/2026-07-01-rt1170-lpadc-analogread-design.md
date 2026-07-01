# RT1176 Arduino Core — Phase 3: LPADC `analogRead` + async

**Date:** 2026-07-01
**Status:** Approved (design), pending implementation plan
**Depends on:** Phase 1 (interrupt infra: RAM vector table + `attachInterruptVector` + NVIC macros).

## Goal

Arduino analog input on the MIMXRT1170-EVKB via the LPADC: blocking `analogRead(pin)`
across both LPADC1 and LPADC2, `analogReadResolution(bits)`, plus an
interrupt-driven async path (`analogReadAsync(pin, callback)`). Verified in the
custom QEMU model (deterministic `channel × 0x111` sample) then on hardware.

## Background / current state

Phase 0–2 delivered the core, GPIO, timing, and interrupt-driven `Serial1`. The
interrupt infrastructure from Phase 1 (RAM vector table, `attachInterruptVector`,
NVIC macros, `IRQ_NUMBER_t`) is reused directly for the LPADC completion IRQ.

The RT1176 uses the **LPADC** (12-bit SAR, command/trigger/FIFO model) — a
different peripheral from the RT106x ADC that teensy4's `analog.c` drives
(`ADC1_HC0`/`ADC1_R0`/`COCO`). So `analog.c` is **not ported**; the LPADC driver
is written fresh against the LPADC register model.

QEMU already models LPADC1/2 (`hw/adc/imxrt_lpadc.c`, `TYPE_IMXRT_LPADC`):
- Firmware loads a conversion command (`CMDL.ADCH` = channel), points a trigger at
  it (`TCTRL[t].TCMD`), and launches via `SWTRIG`. Conversion completes
  **synchronously** — a result is pushed into `RESFIFO` immediately, so `STAT.RDY`
  and `FCTRL.FCOUNT` reflect it at once and poll loops fall through.
- The synthetic sample is **`(CMDL.ADCH) × 0x111`** masked to 12 bits — deterministic
  and assertable (e.g. channel 5 → `0x555` = 1365).
- The completion IRQ asserts when `IE.FWMIE` is set and `FCOUNT > FCTRL.FWMARK`.
- `CTRL.RST`/`CTRL.RSTFIFO` are modeled (flush FIFO). **`CTRL.CAL` is NOT modeled**
  (stored, never self-clears) — so any calibration poll must be bounded or QEMU
  hangs (same discipline as the Phase-0 clock-init loops).

## Reference facts (verified)

- LPADC1 base `0x40050000` (IRQ **88**); LPADC2 base `0x40054000` (IRQ **89**).
- Register offsets (from SDK `PERI_ADC.h`; CMSIS names the IP `ADC`):
  `VERID 0x0, PARAM 0x4, CTRL 0x10, STAT 0x14, IE 0x18, DE 0x1C, CFG 0x20,
  PAUSE 0x24, FCTRL 0x30, SWTRIG 0x34, TCTRL[] 0xC0 (step 4),
  CMDL/CMDH[] 0x100 (step 8), RESFIFO 0x300`.
- Bitfields: `CTRL_ADCEN` bit0, `CTRL_RST` bit1, `CTRL_RSTFIFO` bit8;
  `STAT_RDY` bit0, `STAT_FOF` bit1; `IE_FWMIE` bit0; `CFG_PWRSEL` [5:4],
  `CFG_REFSEL` [7:6]; `FCTRL_FWMARK` [19:16]; `TCTRL_TCMD` [27:24], `TCTRL_HTEN`
  bit0; `CMDL_ADCH` [4:0]; `CMDH_AVGS` [14:12]; `RESFIFO.D` [15:0], `RESFIFO.VALID`
  bit31.
- Clocks: `kCLOCK_Root_Adc1 = 9` → `CCM_CLOCK_ROOT9_CONTROL` @ `0x40CC0480`;
  `kCLOCK_Root_Adc2 = 10` → `0x40CC0500`. LPCG: `kCLOCK_Lpadc1 = 55` →
  `CCM->LPCG[55].DIRECT` @ `0x40CC66E0`; `kCLOCK_Lpadc2 = 56` → `0x40CC6700`.
  Source mux OscRC48MDiv2 (24 MHz) for a simple always-available clock.
- **Verified analog pin:** `GPIO_AD_06` → **LPADC1, channel 0, side A** (`CMDL.ADCH = 0`),
  the exact input the SDK `evkbmimxrt1170/driver_examples/lpadc/polling` example
  samples (Vref Alt1, auto-calibration). This is the hardware-test pin.
- The EVKB Arduino header (J26) exposes A0/ADC0…A5/ADC5, but the per-pin SoC-pad →
  LPADC-channel net is not in the available datasheets (needs the board schematic).
  Only `GPIO_AD_06`/LPADC1-CH0A is confirmed. **Design implication below.**

## Architecture

Layers, bottom-up, each independently testable. New file: `cores/imxrt1176/analog.c`
(fresh LPADC driver; not the teensy4 port). Touches `pins_arduino.h` (analog pin
table), `core_pins.h` (`IRQ_ADC1/2`, API prototypes), `tools/gen_imxrt1176_h.py`
(register block), and reuses Phase-1 interrupt infra.

1. **LPADC register layer** — extend `gen_imxrt1176_h.py` to emit `ADC1_*`/`ADC2_*`
   register macros (the `TCTRL`/`CMDL`/`CMDH` arrays flattened to the indices the
   driver uses: `ADCn_TCTRL0`, `ADCn_CMDL1`, `ADCn_CMDH1`) + the bitfield macros
   above. Regenerate `imxrt1176.h`.
2. **IRQ enum** — add `IRQ_ADC1 = 88, IRQ_ADC2 = 89` to `IRQ_NUMBER_t` in
   `core_pins.h`.
3. **Analog pin table** (`pins_arduino.h`) — a table mapping an Arduino analog pin
   id → `{ lpadc_instance, channel, side, pad_mux_reg, pad_ctl_reg }`. Populated
   with the ONE verified entry (`A0` → LPADC1, ch 0, side A, `GPIO_AD_06`). The
   table + driver are built to hold A1–A5; those entries are added when the board
   schematic net is confirmed (out of scope here — see "Scope note").
4. **Init + clocks** (`analog.c`, lazy on first use, per instance) —
   `lpadc_init(instance)`: configure the ADC clock root (mux OscRC48MDiv2) + LPCG
   gate, `CTRL.RST` pulse, `CFG` (Vref Alt1, normal power), **bounded** auto-cal
   (matches the SDK example; bounded so QEMU can't hang), then `CTRL.ADCEN`.
5. **Blocking `analogRead(pin)`** — resolve pin → `(instance, channel)`; program
   `CMDL1.ADCH = channel`, `CMDH1` (single sample), `TCTRL0.TCMD = 1`; write
   `SWTRIG = 1`; **bounded** poll `STAT.RDY` / `FCTRL.FCOUNT > 0`; read `RESFIFO`,
   extract `D` [15:0] (12-bit), scale to `analogReadResolution()`. Non-analog pin →
   return 0.
6. **`analogReadResolution(bits)`** (default 10-bit, Teensy-parity; clamps 1–16;
   scales the 12-bit result by shifting) + **`analogReference(type)`** stub
   (no-op; documented — LPADC Vref is fixed to Alt1 in init this phase).
7. **Async path** — `analogReadAsync(pin, void (*callback)(uint16_t value))`:
   resolve pin → instance; store the callback; set `FCTRL.FWMARK = 0`,
   `IE.FWMIE = 1`; `attachInterruptVector(IRQ_ADCn, lpadcN_isr)`;
   `NVIC_ENABLE_IRQ(IRQ_ADCn)`; program CMD/TCTRL as blocking; `SWTRIG = 1`. The
   per-instance ISR reads `RESFIFO`, disables `FWMIE`, and invokes the stored
   callback with the scaled value. One pending async conversion per instance
   (documented limitation; returns false if one is already pending).

## Data flow

`analogRead`: pin → `CMDL1.ADCH=ch`, `TCTRL0.TCMD=1`, `SWTRIG=1` → LPADC SAR →
`RESFIFO` → `D` scaled → return.
`analogReadAsync`: same trigger, but `FWMIE` → `IRQ_ADCn` → `lpadcN_isr` pops
`RESFIFO`, clears `FWMIE`, calls the user callback with the scaled value.

## Error handling

- **All hardware poll loops bounded** (calibration wait, conversion-ready wait) via
  guard counters — QEMU doesn't model `CTRL.CAL` self-clear, so an unbounded wait
  would hang. Same pattern as `set_arm_clock_rt1176()`.
- **`analogRead` on a non-analog pin** returns 0 (table lookup miss).
- **Async re-entrancy:** one pending conversion per instance; `analogReadAsync`
  returns `false` if a conversion is already in flight on that instance.
- **FIFO overflow** (`STAT.FOF`): cleared (write-1) in the ISR / before a new
  conversion; not exercised by the single-shot tests but not left to wedge.

## Testing / verification

1. **QEMU (primary, deterministic gate):**
   - Blocking: a sketch calls `analogRead` for several channels programmed directly
     (via the table entry and via a raw channel test hook), asserting the returned
     value equals `(channel × 0x111)` scaled to the current resolution — for BOTH
     LPADC1 and LPADC2 (both modeled). E.g. at 12-bit, channel 5 → 1365; at the
     10-bit default → `1365 >> 2 = 341`.
   - Async: `analogReadAsync` on a channel; assert the callback fires with the same
     scaled value (proves the LPADC IRQ path: `FWMIE` → `IRQ_ADCn` → ISR → callback).
   - Output/assert over the Phase-1/2 `Serial1` harness (`-serial`/socket).
2. **Hardware:** `analogRead` on the verified `GPIO_AD_06` (LPADC1 ch0) with the pin
   tied to GND (~0), 3V3 (~max), and mid (e.g. a divider/pot) — printed over
   `Serial1`; confirm the reading tracks the applied voltage.
3. **Regression:** the Phase-1 TX and Phase-2 RX QEMU gates still pass.

## Scope note (surfaced for review)

The user chose "full: both LPADCs + interrupt/async". The **driver** delivers that
in full and QEMU validates both instances + the async IRQ. The one limitation is
the **physical A1–A5 pin map**: only `GPIO_AD_06`/LPADC1-CH0A is confirmed from the
SDK; A1–A5's SoC-pad→channel nets require the EVKB schematic (not in the available
docs). The analog pin table and driver are structured to accept those entries with
no code change once the nets are known — so this does not block the driver, the
async path, or the QEMU/hardware verification of the LPADC bring-up. Populating the
remaining Arduino analog pins is a follow-up data-entry task, not new engineering.

## Out of scope (YAGNI)

- Hardware-trigger / continuous / DMA conversion modes (single-shot software
  trigger only).
- Multi-command sequences / channel scans, hardware averaging beyond a fixed
  `CMDH.AVGS` default.
- Differential inputs / side-B unless a table entry needs it.
- Per-conversion Vref selection (fixed Alt1 this phase; `analogReference` is a stub).
- Populating A1–A5 physical mappings (schematic-gated; see Scope note).
