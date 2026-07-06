# RT1176 AudioInputI2S (audio-graph capture node) — Design

**Status:** approved (design), ready for implementation plan
**Date:** 2026-07-06
**Sub-project:** B of the AudioInputI2S effort. Depends on sub-project A ([[rt1176-audiostream]], the `AudioStream` framework — DONE + HW-verified + pushed).

## Goal

Bring the Teensy audio input path up on RT1176 as a real audio-graph capture: `AudioInputI2S` pulls SAI-RX audio into `audio_block_t`s and transmits them to the graph, driven by its I2S DMA. Proven end-to-end via `AudioInputI2S → AudioAnalyzePeak`: in QEMU (the SAI RX injector feeds a signal → non-zero peak) and on silicon (the onboard mic → peak rises on sound).

## Architecture — repo layering (mirrors Teensy exactly)

- **Core** (`cores/imxrt1176`, teensy-cores) — unchanged; provides `AudioStream` (sub-project A), the SAI/DMAMUX/clock **register defs** (`imxrt1176.h`: `SAI1_RCSR`/`RDR0`/…, `DMAMUX_SOURCE_SAI1_RX=54`, the SAI clock-root + ANATOP audio-PLL access), and `DMAChannel`.
- **Audio fork** (`git@github.com:newdigate/Audio.git`, `~/Development/Audio`, `master`) — gets the RT1176 ports:
  - `input_i2s.{cpp,h}` — `AudioInputI2S` (raw SAI+DMA, Teensy-structured).
  - `output_i2s.cpp` — `config_i2s()` (the shared SAI + audio-clock setup; `AudioInputI2S::begin` calls it).
  - `control_wm8962.{cpp,h}` — **new** `AudioControlWM8962` node (see §Codec).
  - `analyze_peak.{cpp,h}` — as-is (platform-independent; scans a block for min/max, `read()` → peak 0..1).

The Audio nodes depend on the core only for `AudioStream` + register defs + `DMAChannel` — **never `I2SClass`** (the core's `I2SClass` stays a separate direct-use driver the audio graph doesn't touch). This keeps the Audio library structurally faithful to Teensy so future nodes port cleanly.

## Scope

**In scope:** `AudioInputI2S` (SAI-RX DMA capture → graph), `config_i2s()` (SAI + audio-clock setup for RT1176 **at 44.1 kHz**), `AudioControlWM8962` (codec record enable at 44.1 kHz), `analyze_peak` (verify). Stereo capture (mic on the **right** channel = WM8962 Input3; node captures both, downstream picks). **44.1 kHz** — matching the graph's `AUDIO_SAMPLE_RATE=44100` for a fully coherent graph (input, graph, and future filters all at one rate); the SAI/WM8962 are **reclocked from the core's 48 kHz** to 44.1 kHz.

**Explicitly deferred (YAGNI):** `AudioOutputI2S` playback / a capture→playback passthrough; `AudioInputI2Sslave`; the quad/TDM/PDM input variants; any other Audio-library node.

## `config_i2s()` — the bulk of the work (honest about scope)

This is the SAI peripheral + **audio-clock** setup ported to RT1176 — the trickiest piece, and now with a **new-derivation** twist: it targets **44.1 kHz**, whereas the core's I2S work was all at 48 kHz. It re-derives the SAI1 clock (the SAI1 clock root + the audio PLL, reached through the **ANATOP AI interface**, not plain MMIO — per [[rt1176-i2s-sai]]) for a 44.1 kHz, 16-bit, TX-clock-mastered stream (RX synchronous to TX). The **audio-PLL fractional setting for 44.1 kHz is new** (target MCLK ≈ 44100 × 256 = 11.2896 MHz; the PLL_AUDIO fractional loop divider + post-divider, then the SAI bit-clock divider for 32 bit-clocks/frame). The SAI register-programming structure and the ANATOP AI-interface access are known from the core's 48 kHz work (HW-verified) — so this is "known mechanism, recomputed constants." The fork's `config_i2s` re-implements this in the fork (the user's deliberate "same as Teensy" choice; the core's `I2SClass` is not reused), using the core's ANATOP AI-interface access to program the PLL. Verify the achieved rate on HW (the frame-sync frequency).

## `AudioInputI2S` (raw SAI+DMA, Teensy-structured)

Static state: `block_left`/`block_right` (`audio_block_t*`), `block_offset`, `update_responsibility`, `dma` (`DMAChannel`), and `DMAMEM __attribute__((aligned(32))) uint32_t i2s_rx_buffer[AUDIO_BLOCK_SAMPLES]` (DTCM is DMA-unreachable — `DMAMEM` required).

`begin()`: `config_i2s()` → set the RXD pin mux (`GPIO_AD_20` = `SAI1_RXD[0]`, the daisy input) → `dma` TCD: `SADDR = &SAI1_RDR0 + 2` (upper 16 bits of the 32-bit RDR), `SOFF=0`, `SSIZE/DSIZE=16-bit`, `NBYTES=2`, `DADDR=i2s_rx_buffer`, `DOFF=2`, `CITER/BITER = sizeof/2`, `DLASTSGA = -sizeof` (wrap), `CSR = INTHALF|INTMAJOR`, `triggerAtHardwareEvent(DMAMUX_SOURCE_SAI1_RX=54)` → `SAI1_RCSR = RE|BCE|FRDE|FR` → `update_responsibility = update_setup()` → `dma.enable()` → `dma.attachInterrupt(isr)`.

`isr()` (DMA half + complete): read `dma.TCD->DADDR` to tell which half just filled, `clearInterrupt()`, deinterleave that half of `i2s_rx_buffer` (`L,R,L,R,…`) into `block_left->data[offset]` / `block_right->data[offset]`, advance `block_offset`; on the first (half) interrupt also `if (update_responsibility) AudioStream::update_all()` (pends `IRQ_SOFTWARE`). `arm_dcache_delete` is a no-op here (dcache off). `update()`: allocate 2 fresh blocks; if `block_offset >= AUDIO_BLOCK_SAMPLES` (a full L+R pair captured), swap them in for the DMA and `transmit(out_left,0)` / `transmit(out_right,1)` + release.

## `AudioControlWM8962` (codec node)

New `control_wm8962.{cpp,h}` in the fork — an `AudioControl`-derived node (same interface as `control_wm8960`: `enable()`, `volume()`, `inputSelect()`, etc.), whose `enable()` runs the **WM8962 record init sequence the core's `WM8962Codec` already proved HW-verified** (LPI2C5 @ 0x1A, input PGA / ADC / `AnalogueInputPowerUp`, mic on Input3/right, MICBIAS). The sketch does `AudioControlWM8962 wm; wm.enable();`. (Right chip: WM8962, not the fork's WM8960.)

## Data flow

SAI RX (mic → WM8962 ADC → `SAI1_RXD`) → eDMA fills `i2s_rx_buffer` (double-buffer, half/complete IRQ) → `AudioInputI2S::isr` deinterleaves L/R into blocks + pends `IRQ_SOFTWARE` → `software_isr` → `update_all()` → `AudioInputI2S::update()` transmits → `AudioConnection` → `AudioAnalyzePeak::update()` scans min/max → `peak.read()` returns amplitude.

## Testing — QEMU gate + HW

**Gate `evkb/audioinput_i2s_test/`** — compiles the core (AudioStream + regs + DMAChannel) **plus the four fork files** (`input_i2s`, `output_i2s`, `control_wm8962`, `analyze_peak`). Firmware: `AudioInputI2S in; AudioAnalyzePeak peak; AudioConnection c(in,0,peak,0); AudioControlWM8962 wm;` — `AudioMemory(N)`, `wm.enable()`, `in.begin()`, then loop pumping `yield()`/`update` and reading `peak.available()/read()`.
- **QEMU (`STAGE_PEAK`)**: the SAI RX **injector** (`rx-inject` chardev, from the SAI RX work) feeds a known non-silent signal; assert `peak.read()` is clearly non-zero (and near the injected amplitude). `AUDIOINPUT_ALL=PASS`. `sleep 5`.
- **HW**: flash; the onboard **mic** → `peak.read()` low when quiet, **rises when sound is made** near the mic (the SAI RX HW finding: mic on the right channel). Controller drives flash+VCOM; the user makes noise (a brief bench moment, like the SAI RX mic test).

## HW verification

The silicon proof is the live mic → peak response (quiet ≈ low, sound → rises), reusing the SAI RX mic setup (right channel / WM8962 Input3). On HW the injector is absent, so the QEMU `STAGE_PEAK` exact-amplitude assert is QEMU-only; the mic-responds behavior is the silicon proof.

## References

- Teensy Audio (verbatim structure): `~/.platformio/.../libraries/Audio/{input_i2s,output_i2s,control_wm8960,analyze_peak}.*`; fork copies at `~/Development/Audio/`.
- The clock/SAI/codec values to port: the core's I2S driver + [[rt1176-i2s-sai]], [[rt1176-sai-rx]] (SAI RX + mic-on-right-channel + the injector), `WM8962Codec` (`wm8962.cpp`, the verified record init).
- [[rt1176-audiostream]] (the framework this plugs into), [[rt1176-edma-dmachannel]] (DMAChannel + DMAMUX 54).
