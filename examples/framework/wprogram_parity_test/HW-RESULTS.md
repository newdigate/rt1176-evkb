# WProgram.h include-parity — HW results (MIMXRT1170-EVKB)

**Date:** 2026-07-13
**ELF:** `wprogram_parity_test/build/wprogram_parity_test.elf` built from cores `3270568`
(WProgram.h byte-identical teensy4 mirror) + gate sketch at evkb `05359b7`.
**Flash/run:** `LinkServer run MIMXRT1176:MIMXRT1170-EVKB <elf>` (LinkServer 26.6.137,
MCU-LINK on-board CMSIS-DAP). Serial: MCU-LINK VCOM @115200 via `rt1170-console.py`.
**Jumper:** Arduino header **D11 ↔ D12** (GPIO_AD_30 ↔ GPIO_AD_31 — the SPI-loopback
pair) for the pulseIn measurement.

## Captured output

```
WPROGRAM PARITY GATE
WCHAR=OK
STRING=OK
WORD=OK
RAND=OK
EMILLIS=OK
ITIMER=OK
PULSE_TIMEOUT=OK
PULSE_HW=498
CRASHREPORT_BOOL=OK
CrashReport: not yet supported on IMXRT1176
BOUNCE=OK
USBSERIAL=OK
GATE=DONE
```

All ten markers OK — identical to the QEMU gate — plus the HW-only measurement:
**PULSE_HW=498 µs**, within the accepted 400..600 µs window (nominal 500 µs =
half-period of the 1 kHz `tone()` on D11 read by `pulseIn()` on D12). This is the
first HW-verified `pulseIn` on this core.

## Anomalies / lessons

1. **"D4/D5" jumper shorted a power rail.** The pin table (Zephyr-derived) maps
   D4→GPIO_AD_06, but the RevC3 schematic shows net GPIO_AD_06 never reaches the
   Arduino socket page (only MCU page 7 + Arduino&Moto control block page 26).
   Jumpering the presumed D4/D5 socket positions drooped the MCU rail and made
   SWD attach fail (`Wire not connected` / `Ee(42)`). Removing the jumper and
   switching the gate to the HW-proven D11/D12 pair resolved everything.
   → Follow-up spawned: full pin-table-vs-RevC3-schematic audit.
2. **`LinkServer flash` leaves the core halted** (DEMCR VC_CORERESET set), so a
   subsequent wire reset just re-halts at the reset vector — no boot output.
   Use **`LinkServer run`** (the flow already in project memory) which loads AND
   starts the application.

## Verdict

WProgram.h include-parity milestone **HW-VERIFIED**: QEMU gate green AND silicon
green, stock Bounce2 compiled unmodified, `diff teensy4/WProgram.h
imxrt1176/WProgram.h` empty.
