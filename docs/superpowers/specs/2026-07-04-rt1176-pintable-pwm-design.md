# RT1176 Digital Pin Table + FlexPWM `analogWrite` — Design

**Status:** approved (brainstorming) — ready for implementation plan
**Date:** 2026-07-04
**Target:** `cores/imxrt1176` Arduino core for the MIMXRT1176-EVKB. Hardware-verified (Saleae logic analyzer); no QEMU model (FlexPWM is not modelled in our qemu2).

## Goal

Two coupled additions: (A) **complete the digital pin table** so `pinMode`/`digitalWrite`/`digitalRead` work on every Arduino-header pin (today only pin 13/the LED is mapped), and (B) implement Arduino **`analogWrite`** (+ `analogWriteFrequency`, `analogWriteResolution`) on the RT1176 **FlexPWM**. PWM depends on the pin map, so they share one spec/plan (Phase A then Phase B).

## Scope

**In**
- Phase A: map **D0–D15, A0–A5** in `digital_pin_to_info[]` (gpio base, bit, mux reg, mux ALT, pad reg).
- Phase B: `analogWrite(pin, value)`, `analogWriteFrequency(pin, hz)`, `analogWriteResolution(bits)` over FlexPWM. Defaults: **8-bit** duty, **~1 kHz**. Non-PWM pin → `digitalWrite` fallback (value ≥ half-scale ⇒ HIGH).

**Out (YAGNI)**
- QuadTimer PWM (FlexPWM covers the useful header pins).
- Complementary/dead-time/fault features, phase-shift, DMA, PWM interrupts.
- Mapping non-header SoC pads.
- A QEMU FlexPWM model (Saleae measures the real waveform instead).

## Hardware facts (EVKB Arduino header)

Derived from the Zephyr `arduino_header` gpio-map + the confirmed rule **GPIO_AD_nn ↔ GPIO9 bit (nn−1)** (`iomuxc_gpio_ad_30_gpio9_io29`) and `fsl_iomuxc.h`.

**Header → GPIO (Zephyr):** A0=gpio9.9, A1=gpio9.10, A2=gpio9.11, A3=gpio9.12, A4=gpio9.8, A5=gpio9.7; D0=gpio11.12, D1=gpio11.11, D2=gpio11.13, D3=gpio9.3, D4=gpio9.5, D5=gpio9.4, D6=gpio8.31, D7=gpio9.13, D8=gpio9.6, D9=gpio9.0, D10=gpio9.28, D11=gpio9.29, D12=gpio9.30, D13=gpio9.27, D14=gpio12.4, D15=gpio12.5.

**GPIO9 pins → AD pads** (bit b ⇒ GPIO_AD_(b+1), ALT `0xA` for GPIO9): e.g. D3=AD_04, D5=AD_05, D4=AD_06, D8=AD_07, D9=AD_01, D7=AD_14, D10=AD_29, D11=AD_30, D12=AD_31, D13=AD_28; A5=AD_08, A4=AD_09, A0=AD_10, A1=AD_11, A2=AD_12, A3=AD_13. **The GPIO8/11/12 pins (D0,D1,D2,D6,D14,D15) are EMC/LPSR pads — their exact pad names + GPIO ALT are resolved in plan Task A1** (from the Zephyr pinctrl / `fsl_iomuxc.h`); they may be GPIO-only in the table if their pad lookup is deferred, but D0–D13 minimum must work.

**FlexPWM-capable header pins (confirmed in `fsl_iomuxc.h`):**
| Header | Pad | FlexPWM route | ALT |
|---|---|---|---|
| D9 | AD_01 | FLEXPWM1_PWM0_B | 4 |
| D3 | AD_04 | FLEXPWM1_PWM2_A | 4 |
| D5 | AD_05 | FLEXPWM1_PWM2_B | 4 |
| D4 | AD_06 | FLEXPWM1_PWM0_X | 0xB |
| D8 | AD_07 | FLEXPWM1_PWM1_X | 0xB |
| D7 | AD_14 | FLEXPWM3_PWM0_X | 0xB |

The A/B channels (D9/D3/D5) are the clean, standard PWM outputs (duty in `VAL3`/`VAL5`); X channels (D4/D7/D8) use `VAL0` and are secondary. **The EMC-pad header pins (D0,D1,D2,D6) also route to FlexPWM** and are added if their pad lookup lands in Task A1. Final `pin → {FlexPWM module, submodule, channel A/B/X, ALT}` table is produced in plan Task B1.

**FlexPWM register model** (per submodule `SM[n]`, base offset `n*0x60`): `CTRL2`, `CTRL` (PRSC prescale, FULL reload), `VAL0` (X duty), `VAL1` (modulo = period−1), `VAL2/VAL3` (A on/off), `VAL4/VAL5` (B on/off), `OCTRL`. Module-level: `OUTEN` (PWMA/B/X_EN per submodule), `MCTRL` (`CLDOK`, `LDOK`, `RUN` per submodule mask). FlexPWM1–4 base addresses + the peripheral clock (bus/IPG clock) resolved in plan Task B1 (from `MIMXRT1176_cm7.h`). Program sequence (mirrors Teensy `pwm.c`): `CLDOK` → set `CTRL`/`VAL*` → `OUTEN` → `LDOK` → `RUN`.

## Architecture & files

**Phase A**
```
digital.c          — populate digital_pin_to_info[] for D0–D15, A0–A5
imxrt1176.h (gen)  — IOMUXC mux/pad defs + GPIO8/9/11/12 base addrs for the header pads
core_pins.h        — CORE_NUM_DIGITAL = 22 (D0–D15 + A0–A5) if not already
```

**Phase B**
```
pwm.c              — analogWrite / analogWriteFrequency / analogWriteResolution; FlexPWM submodule config
pwm.h (or core_pins) — pin_to_pwm[] table: {module, submodule, channel, alt} per PWM pin
imxrt1176.h (gen)  — FLEXPWM1..4 register blocks + clock gate (CCM LPCG)
```
- **Stateless config writes**, no ISR (like `analogRead`).
- Submodule is lazily initialised on first `analogWrite` to a pin on it (tracks which submodules are running).

## API semantics

- `analogWrite(uint8_t pin, int value)` — clamp `value` to `[0, maxval]` (maxval = `(1<<res)-1`, default 255). Non-PWM pin ⇒ `pinMode(pin,OUTPUT)` + `digitalWrite(pin, value >= (maxval+1)/2)`. PWM pin ⇒ mux to FlexPWM ALT; ensure submodule running at current freq; duty count = `value * (VAL1+1) / maxval`; write A(`VAL3`)/B(`VAL5`)/X(`VAL0`); `OUTEN` enable; `LDOK`.
- `analogWriteFrequency(uint8_t pin, float hz)` — for the pin's submodule pick prescale so `modulo = clk/(prescale*hz)` fits 16-bit; set `CTRL.PRSC`, `VAL1=modulo-1`, rescale live `VAL0/3/5`; `LDOK`. Stored per submodule.
- `analogWriteResolution(int bits)` — global; sets `maxval=(1<<bits)-1` used for duty scaling (1–16 bits).

## Verification (Saleae logic analyzer, hardware)

- Sketch drives one confirmed clean pin (**D9 = FLEXPWM1_PWM0_B**) with `analogWrite` at duties **0, 64, 128, 192, 255** (default 8-bit, ~1 kHz), holding each ~0.5 s.
- A host Python script (`logic2-automation`, server already enabled on port 10430) captures the pin, decodes the digital trace, and **computes duty% and frequency**, asserting ≈ 0/25/50/75/100 % and ≈ 1 kHz (with tolerance).
- A second check: `analogWriteFrequency(pin, 50)` → measure ~50 Hz (servo rate); `analogWrite` on a non-PWM pin (e.g. A3) at 200 ⇒ reads/measures steady HIGH (fallback).
- No QEMU gate for PWM (FlexPWM unmodelled). Phase A's pin-table changes are smoke-tested by an existing/quick digital toggle; they don't regress the Wire/SPI/ADC QEMU gates (re-run those).

## Error handling
- `analogWrite`/`pinMode`/`digitalWrite` bounds-check `pin < CORE_NUM_DIGITAL` and skip unmapped (gpio==0) entries (existing pattern).
- Frequency out of range (modulo doesn't fit 16-bit at max prescale) ⇒ clamp to nearest achievable; document.
- `analogWriteResolution` clamps bits to 1–16.

## Risks / open items (resolved in plan Task A1/B1)
- Exact pads + GPIO ALT for the EMC/LPSR header pins (D0,D1,D2,D6,D14,D15). D0–D13 digital must work; PWM on EMC pins is a bonus.
- FlexPWM1–4 base addresses and the FlexPWM peripheral clock frequency (for the modulo math) — from `MIMXRT1176_cm7.h` / CCM.
- Pin/function conflicts are the sketch's responsibility (e.g. D10–D13 are SPI; `analogWrite` on them re-muxes away from SPI) — documented, not defended.
