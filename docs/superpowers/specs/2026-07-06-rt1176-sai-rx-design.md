# RT1176 SAI1 RX (Audio Capture) — Design

**Date:** 2026-07-06
**Status:** Approved (brainstorming)
**Depends on:** [I2S/SAI TX audio](2026-07-05-rt1176-i2s-sai-audio-design.md) (the TX path this receives alongside), [eDMA driver](2026-07-05-rt1176-edma-driver-design.md) (DMA-RX reuses `DMAChannel` + DMAMUX source 54)

## Goal

Add the SAI1 **receive** (audio capture) path to the imxrt1176 Arduino/Teensyduino core — the twin of the completed I²S TX + DMA-fed work — so the RT1176 can capture audio from the WM8962 codec's ADC. **v1 delivers polled `read()` + DMA-RX, full-duplex (simultaneous playback + capture), verified in QEMU (bit-exact via an injector) then on silicon (onboard mic + acoustic loopback).**

## Background — recon

- **Core is TX-only.** `I2SClass` has `begin`/`write`/`beginDMA`; `imxrt1176.h` has only the SAI TX registers (`TCSR`/`TCR1-5`/`TDR0`); the SAI1 RX_DATA pin is not muxed.
- **The RX registers mirror TX exactly** (verified against teensy4 `imxrt.h`, matching the QEMU model): `RCSR`@0x88, `RCR1`@0x8C, `RCR2`@0x90, `RCR3`@0x94, `RCR4`@0x98, `RCR5`@0x9C, `RDR0`@0xA0, `RFR0`@0xC0, `RMR`@0xE0 (offsets from base `0x40404000`). Key bits: `RCR2_SYNC(1)` = synchronous to the transmitter, `RCR3_RCE` = receive channel enable, `RCR1_RFW` = RX FIFO watermark; `RCR4`/`RCR5` frame/word fields match `TCR4`/`TCR5`.
- **eDMA + DMAMUX SAI1_RX source 54** are already in place (from the eDMA work).
- **The WM8962 record path is already configured** by the full `WM8962_Init` port: input PGA volume, `LADC`/`RADC` volume, `AnalogueInputPowerUp`, and `SetDataRoute` for `leftInputPGASource=Input1`, `rightInputPGASource=Input3`, `InputMixerSource=InputPGA`. So the codec is already capturing; only the RT1176 RX side + a possible input-pin route match are new.
- **QEMU `imxrt_sai.c` decodes `RCSR`/`RDR0` but has no receive data** — *"the receive side has no sample source, so it never has data to report"*; `dma_rx` is never asserted. It has all the TX plumbing to mirror: `tx_ring`/`tx_head`/`tx_tail`/`tx_level`, the frame-clock `tx_timer`, the `tap` `CharBackend`, and `dma_tx`/`dma_rx` GPIOs.
- **The board has a real capture input.** Schematic (SPF-55139_c3): an onboard electret condenser mic (`POM-2244P-C3310-2-R`) powered by the WM8962 `MICBIAS`, feeding a WM8962 `IN` pin → codec ADC → `ADCDAT` → `SAI1_RXD[0]` (`GPIO_AD_20`, series resistor R2004). The 3.5 mm jack J101 is headphone-*out* only; the board DMIC (PDM) mics route to a PDM peripheral, not the WM8962/SAI.
- **Reference:** Teensy Audio `input_i2s.cpp` (`AudioInputI2S`).

## Scope

**v1 delivers:** SAI1 RX config (synchronous to TX) + `I2S.read()` (polled) + `I2S.beginReceiveDMA()` (continuous capture, DMAMUX source 54), full-duplex with the existing TX; a QEMU RX **injector**; and a gate proving polled RX, DMA-RX, and full-duplex — then HW verification via the onboard mic and an acoustic loopback.

**Explicitly deferred (YAGNI):** async RX (RX generating its own clock — unneeded, the codec ADC uses the shared clock); the AudioStream `AudioInputI2S` graph node (this builds the `read`/`beginReceiveDMA` seam it plugs into); RX from a second SAI or the PDM/DMIC mics; multi-channel (>2) capture.

## Architecture & data flow

```
onboard electret mic → WM8962 (MICBIAS + input PGA + ADC — record path already configured)
   → codec ADCDAT → SAI1_RXD[0] (GPIO_AD_20) → SAI1 RX FIFO  (RCR2.SYNC=1, rides TX's BCLK/FS)
      → RDR0   (polled  I2S.read)      OR      eDMA (DMAMUX src 54 → capture ring, beginReceiveDMA)
```

RX runs **synchronous to TX**: it shares the TX-generated BCLK/FS that `begin()` already produces, so only the RXD data pin is new. **Full-duplex**: TX (`begin`/`write`/`beginDMA`) and RX (`read`/`beginReceiveDMA`) run on the one shared clock with independent FIFOs. **Rejected** alternatives: async RX (duplicates clock config for no benefit); a separate input class (same SAI1 peripheral → one class, matching Teensy's `AudioInputI2S`/`AudioOutputI2S` sharing the SAI).

## File layout

All under `cores/imxrt1176/` unless noted.

| File | Change | Responsibility |
|---|---|---|
| `imxrt1176.h` + `tools/gen_imxrt1176_h.py` | mod | SAI1 RX register block + bits; `SAI1_RXD[0]` (AD_20) mux + `SAI1_RX_DATA0_SELECT_INPUT` daisy. Generator-first, reconcile-clean. |
| `I2S.h` / `I2S.cpp` | mod | RX config in `begin()` (sync-to-TX, `RE`); `read()` polled; `beginReceiveDMA()` (source 54) + capture ISR + `rxBlockCount()`. |
| `wm8962.cpp` | maybe | Confirm/adjust the ADC input route to the mic's actual `IN` pin (currently Input1/Input3). |
| `qemu2/hw/audio/imxrt_sai.{c,h}` | mod | RX injector: `rx-inject` chardev + `rx_ring` + frame-clock pacing of `dma_rx` + `RDR0`-pop + RX data-ready flag. |
| `qemu2/hw/arm/mimxrt1170-evk.c` | mod | Bind a `"sai1-rxinject"` chardev to SAI1 (alongside `"sai1-tap"`). |
| `evkb/sai_rx_test/` | **new** | Gate: Stage A polled `read()`, Stage B DMA-RX, Stage C full-duplex; HW mic + acoustic loopback. |

## SAI1 RX register definitions (`imxrt1176.h`, via generator)

Emit the RX block mirroring the existing TX block:
- Registers (base `0x40404000`): `SAI1_RCSR`@`0x88`, `SAI1_RCR1`@`0x8C`, `SAI1_RCR2`@`0x90`, `SAI1_RCR3`@`0x94`, `SAI1_RCR4`@`0x98`, `SAI1_RCR5`@`0x9C`, `SAI1_RDR0`@`0xA0`, `SAI1_RFR0`@`0xC0`, `SAI1_RMR`@`0xE0`.
- Bits: `SAI_RCSR_RE`(1<<31), `SAI_RCSR_BCE`(1<<28), `SAI_RCSR_FRDE`(1<<0, RX FIFO DMA request), `SAI_RCSR_FEF`(1<<18, overflow), plus the RX data-ready flag polled by `read()` (`FRF`/`FWF`, bit confirmed in the plan against the RM); `SAI_RCR2_SYNC(x)` (=1 for sync-to-TX), `SAI_RCR3_RCE(x)`, `SAI_RCR1_RFW(x)`, and `RCR4`/`RCR5` field macros matching the `TCR4`/`TCR5` set (`FRSZ`/`SYWD`/`MF`/`FSE`/`FSP`, `WNW`/`W0W`/`FBT`).
- Pin: `SAI1_RXD[0]` = `GPIO_AD_20` — mux ALT0 (input, no SION); pad `0x02`; and set `IOMUXC_SAI1_RX_DATA0_SELECT_INPUT` (RX data is an input, so it needs the daisy-chain select, unlike the TX output pins).

## RX config + `read()` / `beginReceiveDMA()`

**`begin()` gains RX setup** after the existing TX config (sharing the clock), then enables both directions:
```cpp
hw->rcr1 = SAI_RCR1_RFW(16);
hw->rcr2 = SAI_RCR2_SYNC(1);                 // synchronous to the transmitter
hw->rcr3 = SAI_RCR3_RCE(1);
hw->rcr4 = SAI_RCR4_FRSZ(1) | SAI_RCR4_SYWD(15) | SAI_RCR4_MF | SAI_RCR4_FSE | SAI_RCR4_FSP;
hw->rcr5 = SAI_RCR5_WNW(15) | SAI_RCR5_W0W(15) | SAI_RCR5_FBT(15);
hw->rcsr = SAI_RCSR_RE;                       // (TX already enables TE|BCE, which clocks RX)
```

**New `I2SClass` API** (twins of `write`/`beginDMA`):
```cpp
void     read(int16_t *interleavedLR, size_t nFrames);                 // polled: poll RCSR, pop RDR0 (L,R)
bool     beginReceiveDMA(int16_t *ring, size_t nFrames,
                         void (*onBlock)(int16_t *half) = nullptr);     // continuous capture
uint32_t rxBlockCount() const;                                         // half/complete ISR count (gate)
```
- `read()` mirrors `write()`: for each frame, poll `RCSR` for RX-data-available and pop `RDR0` (left, then right), bounded by a guard.
- `beginReceiveDMA()` mirrors `beginDMA()` **with source/destination inverted**: a file-static `DMAChannel` with **source = `RDR0` (fixed)** and **destination = `ring` (circular)**, `transferSize` 16-bit, `triggerAtHardwareEvent(DMAMUX_SOURCE_SAI1_RX)` (54), `interruptAtHalf`+`interruptAtCompletion`, `enable()`, then `RCSR |= FRDE`. The ISR passes the just-filled half to `onBlock` (the AudioStream seam) and bumps `rxBlockCount`. Returns `false` if no DMA channel is free. The capture `ring` lives in **DMAMEM/OCRAM**.

## QEMU RX injector (`imxrt_sai.{c,h}` + `mimxrt1170-evk.c`)

The receive mirror of the TX tap. Add:
- **State**: `rx_ring[]` + `rx_head`/`rx_tail`/`rx_level`, and an `rx_inject` `CharBackend` (`DEFINE_PROP_CHR("rx-inject")`, like `"tap"`).
- **Feed**: register receive handlers on the inject chardev (`can_receive` = room in `rx_ring`; `receive` = push LE int16s in). `-chardev file,path=inject.raw` streams a known pattern.
- **Pace + deliver**: extend the frame-clock timer so that when `RE` is set it also drives the receive side — each tick, if `rx_level > 0`, pulse `dma_rx` (eDMA reads `RDR0`) and set the RX data-ready flag in `RCSR` (for polled `read()`). An `RDR0` **read pops** `rx_ring`. One timer paces both directions (they share the frame clock physically), carefully leaving the proven TX path unchanged.
- **Wire**: `mimxrt1170-evk.c` binds a `"sai1-rxinject"` chardev to SAI1, next to `"sai1-tap"`.

## Testing — gate + acoustic-loopback HW verification

`evkb/sai_rx_test/`, deterministic QEMU stages then silicon. A small host helper generates `inject.raw` as a known sine (matching the firmware's `build_sine`).
- **Stage A — polled `read()`**: `-chardev file,id=sai1-rxinject,path=inject.raw`; firmware `read()`s 96 frames, compares to the sine it reproduces, prints `STAGE_A_PASS`. Bit-exact, self-checking.
- **Stage B — DMA-RX**: `beginReceiveDMA(ring,96)` captures the injected pattern → verify the ring matches + `rxBlockCount()≥2` → `STAGE_B_PASS`.
- **Stage C — full-duplex**: run `beginDMA` (TX sine → the tap) *and* `beginReceiveDMA` (RX ← inject) simultaneously; the runner checks **both** the TX tap (`check_tap.py`) and the RX ring → `STAGE_FD_PASS`. Proves simultaneous capture + playback.
- **HW — acoustic loopback**: flash; play a 1 kHz tone out the speaker/headphone (`beginDMA`) while capturing the mic (`beginReceiveDMA`); firmware confirms the captured buffer is non-silent with energy at ~1 kHz (a coarse Goertzel/amplitude check — *tone present*, not bit-exact, since it is a real acoustic path). Plus a bare mic test (capture responds to tapping/speech).

The `run_qemu_sai_rx.sh` runner mirrors the I²S/eDMA gate pattern (qrun + `-chardev file` for the injector, `-chardev file` for the TX tap in Stage C).

## Error handling & gotchas

- **RX FIFO overflow** (`RCSR.FEF`) if a polled `read()` lags the FIFO — the bounded `read()` keeps up at 48 k; DMA-RX avoids it via the ring.
- **RX is an input** → `IOMUXC_SAI1_RX_DATA0_SELECT_INPUT` must select the AD_20 pad (the TX output pins needed no select_input).
- **Codec input pin**: confirm the onboard mic's WM8962 `IN` pin against the schematic; our route already enables Input1 + Input3, so a mono mic on either is captured — a one-line route tweak only if it sits on IN2/IN4. `MICBIAS` is already powered by `WM8962_Init`.
- **`SAI1_RXD[0]` series resistor (R2004)** is the same populate-or-not HW uncertainty the TX header taps had; the codec audio bus itself proved intact for TX, so RX is likely wired — fall back to probing/checking R2004 if capture is silent.
- **QEMU frame-clock unification** must not perturb the HW-verified TX path (regression-check the existing I²S/eDMA gates after the change).
- **Capture ring in DMAMEM/OCRAM** (DMA-reachability, same as TX; `[[rt1176-serialusb]]`).
- **Acoustic loopback is lossy** — the mic hears the speaker through air, so the HW check is "tone present" (band energy), not bit-exact.

## References

- Teensy Audio `input_i2s.cpp` (`AudioInputI2S`); teensy4 `imxrt.h` SAI RX registers.
- QEMU model: `qemu2/hw/audio/imxrt_sai.c` (mirror the `tap`/`tx_ring`/`tx_timer` for RX).
- Board: `MIMXRT1170-EVKB-DESIGNFILES_RevC3/Schematic/SPF-55139_c3.pdf` (WM8962 + mic), `MIMXRT1170EVKBHUG.pdf`.
- Prior art (same core): the I²S/SAI TX and eDMA specs in this directory.
