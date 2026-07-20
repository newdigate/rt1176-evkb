# DAC12 hardware results — MIMXRT1170-EVKB

Date: 2026-07-13. Sketch: `dac_test.cpp` (same ELF as the QEMU gate), flashed via
`LinkServer run MIMXRT1176:MIMXRT1170-EVKB build/dac_test.elf`, VCOM @115200.

## Verdict: DAC12 HW-VERIFIED (digital + analog)

## 1. Digital/architectural checks — PASS on silicon (first flash)

All nine gate checks identical to QEMU: CR config readback (DACEN/DACRFS/
FIFOEN), FIFO write/read pointers in PTR, watermark IRQ on NVIC vector 63
(IRQ_DAC), WMF/NEMPTF flags. No QEMU-model deltas found for the DAC.

## 2. TP18 waveform — visually confirmed (Saleae analog, hand-held probe)

The 5-step staircase (2 s/step) and the 100 Hz sine were observed on TP18
exactly as generated. TP18 is a tiny test pad — a probe cannot be left
clipped — so quantitative analog verification was done via loopback instead.

## 3. Probe-free quantitative check — internal DAC→ADC loopback

RM ch.87 internal sources: **ADC1 CH6A = DAC output** (ADC2 CH7B likewise).
DAC and ADC share VREFH, so written code ≈ read code. Final numbers with the
fixed analogRead (see §4), via `analogReadChannel(0, 6)` at 12-bit:

| written | read | ratio |
|---------|------|-------|
| 0       | 11   | —     |
| 1024    | 972  | 0.949 |
| 2048    | 1950 | 0.952 |
| 3072    | 2926 | 0.952 |
| 4095    | 3935 | 0.961 |

Linear, ~zero offset, ≈95% gain. The ~5% shortfall is sampling droop, not the
DAC: the raw-command experiment matrix (lbB/C/D lines; note those sketch reads
use the pre-fix >>4 shift, so double them to compare) showed CSCALE=1 + 131.5-
cycle sample time + the DAC's opamp buffer (CR2.BFEN|BFHS) reads ≈99.5% of the
written code (2048→2038, 1024→1022, 4095→4054). With the default unbuffered DAC
and minimum ADC sample time the constant ≈0.95 factor is expected and benign.

## 4. Bycatch: the loopback exposed TWO latent analogRead bugs (both fixed)

Baseline loopback read only 0.2275× the written code. Root causes (confirmed
against PERI_ADC.h + SDK lpadc examples, isolated by the variant matrix):

1. **CMDL.CSCALE=0 attenuated every ADC input by 30/64** — analog.c never set
   CSCALE; the SDK default is 1 (full scale). Fixed: both command setups now
   set `ADC_CMDL_CSCALE`.
2. **Result field is RESFIFO.D[14:3], not D[15:4]** — bit 15 is the
   differential sign bit (SDK: `convValue >> 3`, full range 4096). analog.c's
   `>> (16-bits)` halved every reading; QEMU's imxrt_lpadc modelled the same
   wrong justification, so the ADC gate was **circularly green**. Fixed in
   both (driver `>> (15-bits)`, model left-justifies by 3); analog_test gate
   expectations are invariant under the paired fix and stay green.

Cores commit `fc7e17d`, QEMU commit `b30e1caea4`.

## Raw serial (final flash)

```
dacloop[0]=11  dacloop[1024]=972  dacloop[2048]=1950  dacloop[3072]=2926  dacloop[4095]=3935
lbB[…] CSCALE only (×2: 40/972/1948/2926/3936)
lbC[…] +STS=7    (×2: 2/994/1982/2984/4064)
lbD[…] +DAC buffer (×2: 8/1022/2038/3054/4054)
```
