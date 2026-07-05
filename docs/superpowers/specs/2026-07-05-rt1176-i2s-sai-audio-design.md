# RT1176 Arduino `I2S` audio out (SAI1 TX, polled) + WM8962 codec — Design

**Status:** approved (brainstorming) — ready for implementation plan
**Date:** 2026-07-05
**Target:** `cores/imxrt1176` Arduino/Teensy core for the MIMXRT1176-EVKB, verified in QEMU (`mimxrt1170-evk`) then on hardware.

## Goal

Bring up an I²S **transmitter** on **SAI1** so the CM7 can stream 16-bit stereo audio out
the SAI TX data line at 48 kHz, Teensy-style, and drive it through the board's **WM8962**
codec so a known **1 kHz sine** is audible on the **J101** headphone jack. v1 is a
**polled/blocking** transmit of a canned buffer — not the Teensy Audio graph, not DMA.

Success is the verification triad:
- **QEMU gate:** the exact 16-bit sample sequence written to `SAI1_TDR` (captured via a new
  raw-TDR tap) matches the precomputed sine table, **and** the WM8962 I²C init runs to
  completion against a codec stub → VCOM prints `PASS`.
- **Saleae:** on the TX pins — BCLK ≈ **1.536 MHz**, FS = **48 kHz**, data word = the sine
  (MSB-first, 16-bit).
- **Ears/scope:** 1 kHz tone on J101.

## Scope

**In scope**
- SAI1 **TX only**, **master** (generates BCLK + FS), **I²S protocol**, **16-bit**, **stereo**, **48 kHz**.
- **Polled/blocking** engine: `I2S.write()` busy-pushes samples into the TX FIFO, pacing on the FIFO flag. No ISR, no DMA.
- Minimal audio-out class `I2S` (single instance bound to SAI1) via a `hardware_t` struct, matching the `SPI`/`Wire` core pattern.
- Audio-PLL + SAI1 clock-root bring-up (new `sai1_clock_init()`).
- **WM8962 codec** bring-up over a **new `Wire2` = LPI2C5** instance: minimal init (power / clocking / I²S-slave / DAC→headphone route / unmute) ported (trimmed) from the SDK `fsl_wm8962`.
- Two QEMU refinements: a **raw-TDR sample tap** (deterministic capture) and a **WM8962 I²C-slave stub** (ACK/store control regs).

**Out of scope (YAGNI / deferred)**
- **EDMA-fed streaming** — the realistic Teensy path. Deferred until the core has an eDMA driver; QEMU's SAI `dma_tx` request path is already wired for it. *(Explicit follow-up: do this after I²S + eDMA.)*
- Interrupt-driven TX (FRIE/ISR), the AudioStream graph / `AudioOutputI2S` compatibility.
- SAI **RX**/input, additional SAI instances (SAI2/3/4), TDM/multi-dataline.
- Sample rates other than 48 kHz, runtime rate changes, word widths other than 16-bit, mono.
- Modeling BCLK/FS **timing** in QEMU — that is HW/Saleae's job (project philosophy: QEMU proves data/logic, silicon+Saleae prove timing).

## Hardware facts (RT1176 / EVKB) — from the SDK `sai` example, cm7 header, and Zephyr

**SAI1** is the EVKB audio path (SDK `driver_examples/sai/.../app.h`: `#define DEMO_SAI SAI1`; Zephyr `&sai1 status="okay"`).

- **SAI1 base** `0x40404000`; **IRQ = 76** (combined TX/RX line; SAI1/2 share one, SAI3/4 split). *(cm7 `MIMXRT1176_cm7_COMMON.h`.)*
- **TX pin mux** (SDK `pin_mux.c`, confirmed identically by Zephyr `mimxrt1170_evk-pinctrl.dtsi`):

  | Signal | Pad | IOMUXC ALT | Notes |
  |---|---|---|---|
  | MCLK | `GPIO_AD_17` | `SAI1_MCLK` | + `IOMUXC_GPR0.SAI1_MCLK_DIR = 1` (MCLK is an **output**) |
  | TX_BCLK | `GPIO_AD_22` | `SAI1_TX_BCLK` | bit clock |
  | TX_SYNC (LRCLK/FS) | `GPIO_AD_23` | `SAI1_TX_SYNC` | frame sync = sample rate |
  | TX_DATA00 | `GPIO_AD_21` | `SAI1_TX_DATA00` | serial audio data |

  Pads: fast slew, high drive (SDK uses pad ctl `0x02`; matched during implementation).

- **Clock tree (SDK canonical, 48 kHz):**
  - Audio PLL: `loopDivider=32, postDivider=1, numerator=768, denominator=1000` ⇒ `24 MHz × (32 + 768/1000) / 2 =` **393.216 MHz** (`CLOCK_InitAudioPll`).
  - SAI1 clock root: **mux = 4 (Audio PLL), div = 16** ⇒ **24.576 MHz** = MCLK.
  - Derived for 48 kHz / 16-bit / stereo: **MCLK = 24.576 MHz** (512×Fs), **BCLK = 1.536 MHz** (= 48000 × 2ch × 16bit), **FS = 48 kHz**. MCLK/BCLK = 16 ⇒ `TCR2.DIV = 7` (BCLK = MCLK / (2·(DIV+1))).
  - *(teensy4's PLL4 / `CCM_CSCMR1` / `CS1CDR` path is RT1062-specific and NOT used here — RT1176 uses the `CLOCK_InitAudioPll` + `CLOCK_SetRootClockMux/Div(kCLOCK_Root_Sai1)` sequence above.)*

- **Codec: WM8962** on **LPI2C5**, 7-bit addr **0x1A**, MCLK 24.576 MHz, I²S 16-bit slave (SAI is master). The board routes DAC → **J101** (OMTP headphone); needs resistors **R2001–R2007** populated. LPI2C5 also carries the eCompass (0x1f) — different address, same bus.
  - **LPI2C5 is not yet in the core** (`imxrt1176.h` defines only LPI2C1/LPI2C2). v1 adds the LPI2C5 register block + clock root + LPCG + SCL/SDA pin mux (values from the cm7 header / SDK `board.c`).

### Key SAI TX registers (offsets within the SAI block; cm7 `PERI_I2S.h`)

`VERID 0x00`, `PARAM 0x04`, **`TCSR 0x08`**, `TCR1 0x0C`, **`TCR2 0x10`**, **`TCR3 0x14`**, **`TCR4 0x18`**, **`TCR5 0x1C`**, **`TDR0 0x20`** (TDR0..3 to 0x2C), `TFR0 0x40` (FIFO status), **`TMR 0x60`** (word mask). FIFO depth = **32 words × 4 datalines**.

- **`TCSR`**: `TE` bit31, `BCE` bit28, `FRDE` bit0 (DMA req en — *unused in v1*), `FRIE` bit8 (IRQ en — *unused in v1*), `FRF` bit16 (FIFO request flag, status), `FWF` bit17 (FIFO warning flag), `FEF` bit18 (underrun, W1C), `SR` bit24 / `FR` bit25 (momentary resets).
- **`TCR2`**: `DIV[7:0]`, `BCP` bit25, `BCD` bit24 (1=master), `MSEL[27:26]` (1=MCLK1), `SYNC` bit30.
- **`TCR3`**: `TCE[19:16]` (transmit channel enable — set ch0).
- **`TCR4`**: `FSD` bit0 (1=FS master), `FSP` bit1 (FS polarity), `FSE` bit3 (FS early — I²S), `MF` bit4 (MSB first), `SYWD[12:8]` (sync width−1), `FRSZ[20:16]` (frame size−1).
- **`TCR5`**: `FBT[12:8]` (first bit shifted), `W0W[20:16]` (word-0 width−1), `WNW[28:24]` (word-N width−1).

## Architecture & files

```
cores/imxrt1176/
  I2S.h              — I2SClass; hardware_t (SAI regs + pad/mux tables + clock root/LPCG); extern I2S
  I2S.cpp            — begin(rate), end(), write(samples,n) [polled], clock+TCRx config
  I2S_instances.cpp  — sai1_hw literal (SAI1 regs, AD_17/21/22/23 mux+pad, SAI1 root, LPCG); I2SClass I2S(&sai1_hw)
  wm8962.h / wm8962.cpp — WM8962 codec init over a TwoWire& (trimmed from SDK fsl_wm8962); extern codec
  Wire_instances.cpp — + Wire2 (LPI2C5) instance for the codec control bus
  imxrt1176.h        — + SAI1 block, ANADIG Audio-PLL, CCM SAI1 root + LPCG; + LPI2C5 block + root + LPCG (via tools/gen_imxrt1176_h.py where applicable)
  core_pins.h        — + IRQ_SAI1 (=76), IRQ_LPI2C5
```

- **Blocking polled**, no ISR, no NVIC/vector work (like `SPI`).
- `I2S::hardware_t` mirrors the `SPI`/`Wire` singleton pattern: register refs (`tcsr, tcr1..5, tdr, tfr, tmr`), `lpcg`, `clock_root(+val)`, audio-PLL handle, and MCLK/BCLK/SYNC/DATA mux+pad refs + values.
- The codec reuses the **HW-proven `TwoWire` master** via a new `Wire2` bound to LPI2C5 — no new I²C engine.

## API semantics & register mapping

**`I2S.begin(uint32_t sampleRate = 48000)`**
1. `sai1_clock_init()` — power up Audio PLL (393.216 MHz), set SAI1 root mux=4/div=16 (24.576 MHz), ungate SAI1 LPCG.
2. Pin mux: MCLK `AD_17`, BCLK `AD_22`, SYNC `AD_23`, DATA00 `AD_21`; set `IOMUXC_GPR0.SAI1_MCLK_DIR=1`.
3. Software-reset TX (`TCSR.SR` then `TCSR.FR`), `TMR = 0` (unmask all words).
4. Configure I²S 16-bit stereo master (values follow SDK `SAI_TxSetConfig` for `kSAI_BusI2S`, 16-bit, stereo, `kSAI_Master`, MCLK1):

   | Reg | Fields | v1 value |
   |---|---|---|
   | `TCR1` | `TFW` (watermark) | mid-FIFO (e.g. 16) |
   | `TCR2` | `MSEL=1, BCD=1, BCP=1, DIV=7` | bit clock = MCLK/16 = 1.536 MHz |
   | `TCR3` | `TCE` = ch0 | `0x1_0000` |
   | `TCR4` | `FRSZ=1, SYWD=15, MF=1, FSD=1, FSE=1, FSP=1` | I²S, 2 words/frame |
   | `TCR5` | `W0W=15, WNW=15, FBT=15` | 16-bit words, MSB first |

5. Enable: `TCSR |= TE | BCE`.

   *(Exact `DIV`/word-width values are recomputed from the SDK bit-clock formula in the plan's first task; `sampleRate` ≠ 48000 is rejected in v1.)*

**`I2S.write(const int16_t* samples, size_t nFrames)`** — polled. For each stereo frame, poll `TCSR.FWF/FRF` (bounded `I2S_TIMEOUT`) for FIFO room, then write L then R to `TDR0` as 16-bit values. Interleaved L/R input. Blocks until all frames are pushed. (The SAI FIFO provides back-pressure on real HW; QEMU reports room continuously, which is fine — samples still land in the TDR tap in order.)

**`I2S.end()`** — `TCSR &= ~(TE|BCE)`; gate SAI1 LPCG.

**Codec** — `wm8962.h`: `class WM8962Codec { bool begin(TwoWire& bus, uint32_t mclkHz=24576000); }`. `begin()` runs the trimmed SDK init: reset, power management (VMID/BIAS/DAC/HP), clocking for 24.576 MHz MCLK / 48 kHz, digital audio interface = I²S 16-bit slave, DAC → headphone-mixer route, unmute + set a safe volume. Bounded per-register I²C writes; returns false on NAK/timeout. Sketch calls `codec.begin(Wire2)`.

**Sketch shape:** `Wire2.begin(); I2S.begin(48000); codec.begin(Wire2); for(;;) I2S.write(sineLR, N);`

## Verification

**QEMU — two first-class refinements to the existing `hw/audio/imxrt_sai.c` model** (the SAI *device* is already implemented & SoC-wired; these add gate observability + codec ACK):

1. **Raw TDR-sample tap.** Add a `chardev`/file property; in the `TDR0..TDR3` write handler, when `TCSR.TE` is set, emit the low 16 bits of every write as little-endian `int16` **unconditionally** (independent of the audio-backend voice / ring, so capture is exact and never dropped under faster-than-real-time execution). ~30 lines; the existing audio-backend WAV path is untouched (still usable via `-audiodev wav` for an in-sim listen).
2. **WM8962 I²C-slave stub.** A minimal I²C slave at addr **0x1A** attached to **LPI2C5's bus** (`lpi2c[4].bus`) in `hw/arm/mimxrt1170-evk.c`, reusing the existing slave-persona framework (the same mechanism that backs the `Wire` loopback). ACKs writes, stores control registers, and returns benign/expected values for any reads (e.g. device-ID) so the SDK-derived codec init completes. No audio DAC modeled.

**QEMU gate (`evkb/i2s_audio_test/`, copied from `usb_data_test/`):**
- Runner via `tools/qrun`: `-M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel …elf -serial file:vcom.uart -chardev file,id=tdr,path=…tdr.raw` (+ optional `-audiodev wav`). Wire the SAI's tap chardev to `tdr`.
- Sketch: bring up `Wire2`, `I2S.begin(48000)`, `codec.begin(Wire2)` (asserts init returned true, i.e. codec stub ACKed), then `I2S.write()` one period-aligned block of the **1 kHz sine** (48 samples/cycle × N cycles, same value L+R); print `PASS`/`FAIL` + a checksum over Serial1.
- Python check: read `tdr.raw` as LE int16, compare the captured L/R stream to the precomputed interleaved sine table (exact match); assert the `PASS` line. **Fail-first** (e.g. wrong `DIV` or unconfigured TCE) then pass.

**Hardware acceptance:**
- Flash via LinkServer; capture VCOM @115200 for the `PASS` banner.
- **Saleae** on `GPIO_AD_22` (BCLK), `GPIO_AD_23` (FS), `GPIO_AD_21` (DATA): assert BCLK ≈ 1.536 MHz, FS = 48 kHz, and the serialized data word = the sine sample (MSB-first, 16-bit, correct L/R slot). Saleae is the **sole timing arbiter**.
- **Codec/analog:** with R2001–R2007 populated, confirm the 1 kHz tone on J101 (scope/headphones). Optional A/B: run the SDK `sai` example on the same board to isolate firmware vs. bench.
- **CMake trap:** new core `.c/.cpp` ⇒ `rm -rf build && cmake -B build …` for the gate (the core `file(GLOB)` has no `CONFIGURE_DEPENDS`).

## Error handling
- Bounded poll loops (`I2S_TIMEOUT`, like `WIRE_TIMEOUT`/`SPI_TIMEOUT`); a stuck FIFO just times out and `write()` returns early (documented, not defended beyond the bound).
- `codec.begin()` returns false on I²C NAK/timeout; the sketch reports it. `write()`/`begin()` ordering before clock-up is undefined (documented, matching Arduino).
- Underrun (`TCSR.FEF`) is not serviced in polled v1 (a canned finite buffer doesn't starve mid-stream); noted for the streaming follow-up.

## Non-goals / risks
- **Codec is only truly verifiable on hardware** — QEMU has no WM8962 audio model; the stub proves the *control-plane* init runs, not that sound is produced. Analog output is a HW/Saleae/ears check by design.
- **Exact WM8962 register sequence** (and any device-ID readback the init expects) is nailed in the plan against SDK `fsl_wm8962.c`; the QEMU stub's read responses are matched to what that init reads.
- **`TCR2.DIV` / word-width values** are assumed from the 48 kHz math above; the plan's first task re-derives them from the SDK bit-clock formula before wiring.
- **LPI2C5 pin mux / LPCG / root** values are added from the cm7 header + SDK `board.c`; verified in the first plan task.
- Faster-than-real-time QEMU + always-room FIFO means the model can't back-pressure the polled writer — acceptable because the raw tap captures every sample in order regardless.
