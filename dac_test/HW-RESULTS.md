# DAC12 hardware results — MIMXRT1170-EVKB

Date: 2026-07-13. Sketch: `dac_test.cpp` (same ELF as the QEMU gate), flashed via
`LinkServer run MIMXRT1176:MIMXRT1170-EVKB build/dac_test.elf`, VCOM @115200.

## Digital/architectural checks — PASS on silicon (first flash)

Board serial output matched the QEMU gate exactly, all nine checks OK:

```
RT1176 DAC12 test
cr_dacen=1 OK
cr_dacrfs=1 OK
cr_fifoen=0 OK
wfp=4 OK
rfp0=0 OK
rfp1=1 OK
irq=1 OK
wmf=1 OK
nemptf=1 OK
[dac] done
dac=0 … dac=4095
sine 100Hz x5s
```

So on real silicon: CR config readback (DACEN/DACRFS/FIFOEN), FIFO write/read
pointers advancing in PTR, the watermark interrupt firing on NVIC vector 63
(IRQ_DAC), and the WMF/NEMPTF status flags all behave exactly as the QEMU
`imxrt_dac` model predicts — no model deltas found.

## Analog check on TP18 — PENDING (needs a probe on the board)

The HW phase loops forever: 5-step staircase (codes 0/1024/2048/3072/4095, 2 s
each; expected ≈ 0 / 0.45 / 0.90 / 1.35 / 1.80 V with VREFH = 1.8 V nominal),
then 5 s of a 100 Hz sine (0–1.8 V swing centered ~0.9 V). To finish
verification: DMM/scope/Saleae-analog lead on **TP18**, GND on any board GND,
and record the measured staircase voltages + observed sine here.
