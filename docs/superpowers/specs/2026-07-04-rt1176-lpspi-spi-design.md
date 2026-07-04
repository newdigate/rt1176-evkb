# RT1176 Arduino `SPI` (LPSPI master) — Design

**Status:** approved (brainstorming) — ready for implementation plan
**Date:** 2026-07-04
**Target:** `cores/imxrt1176` Arduino/Teensy core for the MIMXRT1176-EVKB, verified in QEMU (`mimxrt1170-evk`) then on hardware.

## Goal

Implement the modern Arduino `SPI` library (master only) on the RT1176's **LPSPI1** (the EVKB Arduino header SPI bus), as a blocking polled engine matching the existing `Wire`/`analog` cores. Verify by loopback: an `ssi-loopback` peripheral in QEMU and an external SDO→SDI jumper on hardware, cross-checked A/B against the NXP SDK LPSPI loopback example.

## Scope

**In scope**
- Master only (Arduino `SPI` is a master API).
- Modern transaction API: `begin()`, `end()`, `SPISettings(clockHz, bitOrder, dataMode)`, `beginTransaction()`, `endTransaction()`, `transfer(uint8_t)`, `transfer16(uint16_t)`, `transfer(void *buf, size_t len)`.
- Manual chip-select: the sketch drives CS with `digitalWrite`; `SPI` does not manage CS (matches Arduino/library convention).
- Single instance `SPI` bound to LPSPI1, via a `hardware_t` struct so more instances can be added later.

**Out of scope (YAGNI)**
- Slave mode. Arduino SPI has no standard slave API; rare, high-effort.
- Deprecated pre-transaction setters (`setClockDivider`, `setBitOrder`, `setDataMode`).
- Interrupt/DMA/async transfers. `transfer()` is synchronous; polled is sufficient.
- Hardware PCS management, `usingInterrupt()`, transaction interrupt-masking.

## Hardware facts (RT1176 / EVKB)

- **Arduino header SPI = LPSPI1.** Pins: SCK = `GPIO_AD_28`, SDO (MOSI) = `GPIO_AD_30`, SDI (MISO) = `GPIO_AD_31`, PCS0 (CS) = `GPIO_AD_29`. (SCK/PCS0 confirmed from SDK `pin_mux.c`; SDO/SDI = AD_30/31 to be verified against IOMUXC during implementation — the plan's first task confirms the ALT mux values and pad addresses.)
- **LPSPI1**: base `0x40114000`, IRQ_LPSPI1 = 38 (no ISR used, but the IRQ number is added for completeness/consistency). Clock root + LPCG follow the same `CCM_CLOCK_ROOT[n]` / `CCM_LPCG[n]` scheme as LPUART/LPI2C; the generator script computes the addresses.
- **No internal loopback bit.** LPSPI loopback = SDO tied to SDI externally. The SDK "loopback" example uses the normal 4-wire pin config and requires the physical short; our HW test uses the same jumper.
- LPSPI is **push-pull** (not open-drain): no pull-up resistors, far less finicky than I²C.

### Key LPSPI registers (master)
- `VERID 0x00`, `PARAM 0x04`, `CR 0x10` (MEN bit0, RST bit1, RTF bit8, RRF bit9), `SR 0x14` (TDF bit0, RDF bit1, TCF bit10, MBF bit24), `IER 0x18`, `CFGR1 0x24` (MASTER bit0), `CCR 0x40` (SCKDIV[7:0], DBT[15:8], PCSSCK, SCKPCS), `FCR 0x58`, `FSR 0x5C`, `TCR 0x60`, `TDR 0x64`, `RSR 0x70` (RXEMPTY bit1, RXFULL bit2), `RDR 0x74`. (Exact offsets emitted by the generated `imxrt1176.h`.)
- **`TCR` (transmit command word)** fields: `FRAMESZ[11:0]` (bits-1), `PRESCALE[29:27]`, `CPHA` bit30, `CPOL` bit31, `LSBF` bit23, `PCS[25:24]`, `RXMSK` bit19, `TXMSK` bit18, `CONT` bit21, `CONTC` bit20, `BYSW` bit22, `WIDTH[17:16]`. TCR is a **queued command word** (goes through the TX FIFO ahead of data) — this matches the existing QEMU model's semantics.

## Architecture & files

```
cores/imxrt1176/
  SPI.h              — SPIClass + SPISettings; hardware_t; extern SPI; MSBFIRST/LSBFIRST, SPI_MODE0..3
  SPI.cpp            — begin/end, beginTransaction/endTransaction, transfer/transfer16/transfer(buf,len), clock calc
  SPI_instances.cpp  — lpspi1_hw literal (LPSPI1 regs, GPIO_AD_28/30/31 mux+pad, clock root, LPCG); SPIClass SPI(&lpspi1_hw)
  imxrt1176.h        — + LPSPI1 register block, CLOCK_ROOT, LPCG (via tools/gen_imxrt1176_h.py)
  core_pins.h        — + IRQ_LPSPI1 (=38)
```

- **Blocking polled**, no ISR (transfer is synchronous) → no NVIC/vector work; simpler than Wire.
- `hardware_t` mirrors `Wire`'s pattern: register refs (cr, sr, tcr, tdr, rdr, ccr, cfgr1, rsr), lpcg, clock_root(+val), pin mux/pad refs + values.

## API semantics & register mapping

**`SPISettings(clockHz, bitOrder, dataMode)`** stores clock/bitOrder/dataMode. Mapping:

| Setting | LPSPI |
|---|---|
| `clockHz` | `CCR.SCKDIV` + `TCR.PRESCALE`, computed from the LPSPI functional clock so actual SCK ≤ requested |
| `bitOrder` MSBFIRST(1)/LSBFIRST(0) | `TCR.LSBF` (LSBFIRST ⇒ bit23=1) |
| `dataMode` SPI_MODE0..3 | `TCR.CPOL` bit31, `TCR.CPHA` bit30 (MODE0=0,0; 1=0,1; 2=1,0; 3=1,1) |
| frame size 8 / 16 | `TCR.FRAMESZ` = bits-1 (7 / 15) |

**Clock calc:** functional clock `f` (chosen clock root; default source documented in the plan, e.g. a PLL/OscRC root). For requested `clockHz`, pick `PRESCALE` (÷1..÷128) and `SCKDIV` (0..255) so `f / (prescale × (SCKDIV+2)) ≤ clockHz`, maximizing the result. Default `SPISettings()` = 4 MHz, MSBFIRST, MODE0.

**Methods**
- `begin()` — ungate LPCG; set clock root; mux SCK/SDO/SDI pads (PCS0 left as GPIO); `CR = RST` then `0`; `CFGR1 = MASTER`; program a default `CCR`; `CR = MEN`.
- `end()` — `CR = 0`; gate LPCG.
- `beginTransaction(SPISettings s)` — store `s`; compute and cache the `TCR` base (CPOL/CPHA/LSBF/PRESCALE) and `CCR` (SCKDIV); write `CCR` (needs `MEN=0` briefly if required by silicon — documented in plan).
- `transfer(uint8_t b)` — write `TCR` (cached base | FRAMESZ=7 | RXMSK=0); write `TDR = b`; poll `RSR.RXEMPTY==0` (bounded `SPI_TIMEOUT`); return `RDR & 0xFF`. On timeout return `0xFF`.
- `transfer16(uint16_t w)` — same with FRAMESZ=15; return `RDR & 0xFFFF`.
- `transfer(void *buf, size_t len)` — in-place byte loop over `transfer()`; full-duplex (rx overwrites tx buffer).
- `endTransaction()` — minimal (idle). No CS action.

## Verification

**QEMU gate (`spi_loopback_test`)** — primary automated gate:
- Machine change: attach a `TYPE_SSI_LOOPBACK` peripheral to `lpspi[0].bus` (LPSPI1) in `hw/arm/mimxrt1170-evk.c` so master transfers echo MOSI→MISO. No LPSPI *model* change required.
- Sketch: `SPI.begin()`; for each of SPI_MODE0..3 and both bit orders: `beginTransaction`; `transfer(0xA5)`, `transfer(0x3C)`; a 4-byte `transfer(buf,len)`; a `transfer16(0xBEEF)`; assert each RX == TX; print `PASS`/`FAIL` + values over Serial1.
- Runner script asserts the `PASS` line (mirrors `run_qemu_wire.sh`).

**Hardware acceptance:**
- Same sketch, external **SDO→SDI jumper** (GPIO_AD_30→GPIO_AD_31). Flash via LinkServer, capture VCOM @115200; expect the same `PASS` and echoed values.
- **A/B cross-check:** build and flash the NXP SDK `lpspi/loopback` example on the same jumper; confirm both pass (isolates firmware vs. bench, per the I²C lesson).

## Error handling
- Bounded poll loops (`SPI_TIMEOUT`, like `WIRE_TIMEOUT`); SPI has no ACK/NACK, so no bus-error taxonomy — a stuck FIFO just times out and `transfer` returns `0xFF`.
- `transfer` before `begin` is undefined (documented; not defended, matching Arduino).

## Non-goals / risks
- SDO/SDI pin ALT/pad values assumed `GPIO_AD_30/31`; first plan task verifies against the RT1176 IOMUXC before wiring anything.
- CCR write may require `MEN=0` on silicon; the plan sequences clock programming safely and the QEMU gate + HW test confirm timing.
