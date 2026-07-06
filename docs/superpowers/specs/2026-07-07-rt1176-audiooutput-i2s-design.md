# RT1176 AudioOutputI2S (audio-graph playback node) — Design

**Status:** approved (design), ready for implementation plan
**Date:** 2026-07-07
**Relation:** the playback twin of `AudioInputI2S` ([[rt1176-audioinput-i2s]], DONE + HW-verified). Reuses its groundwork: `config_i2s()` (SAI TX + audio-PLL @ 44.1 kHz) and the `AudioStream` framework ([[rt1176-audiostream]]) are already in place.

## Goal

Bring the Teensy audio **output** path up on RT1176 as a real audio-graph playback node: `AudioOutputI2S` pulls `audio_block_t`s from the graph and streams them out over SAI1 TX via its I2S DMA to the WM8962 DAC. Proven end-to-end via `AudioSynthWaveformSine → AudioOutputI2S`: in QEMU (the SAI1 TX **tap** captures the transmitted samples → assert a non-silent sine) and on silicon (a tone is audible on the J101 headphone/line-out).

## Architecture — repo layering (identical to AudioInputI2S)

- **Core** (`cores/imxrt1176`, teensy-cores) — unchanged except possibly adding missing ARMv7E-M DSP intrinsic macros (see §Synth). Provides `AudioStream`, the SAI/DMAMUX/clock register defs (`SAI1_TDR0`, `DMAMUX_SOURCE_SAI1_TX`, the SAI clock-root + ANATOP audio-PLL access via `config_i2s`), and `DMAChannel`.
- **Audio fork** (`git@github.com:newdigate/Audio.git`, `~/Development/Audio`, `master`) — gets the RT1176 output work:
  - `output_i2s.cpp` — `AudioOutputI2S::begin()` gains an `__IMXRT1176__` TX-DMA branch. `config_i2s()` (already ported), `isr()` (guard already widened, commit `ac70035`), and `update()` (graph-side) are done/shared.
  - `synth_sine.{cpp,h}` + `data_waveforms.c` — already present in the fork; verified to compile for RT1176 via the existing `__ARM_ARCH_7EM__` path.
  - `control_wm8962.{cpp,h}` — reused as-is; its `enable()` already configures the DAC→headphone playback route.

The Audio nodes depend on the core only for `AudioStream` + register defs + `DMAChannel` — **never `I2SClass`** (kept structurally faithful to Teensy).

## Scope

**In scope:** `AudioOutputI2S` (graph → SAI1-TX DMA playback, `__IMXRT1176__` `begin()` branch), `AudioSynthWaveformSine` (the reusable sine-oscillator graph source — verified to compile/run on RT1176), verification via the SAI1 TX tap (QEMU) and audible tone (HW). Stereo output (the same sine on both channels). **44.1 kHz** — matching the graph and `config_i2s()`.

**Explicitly deferred (YAGNI):** `AudioOutputI2Sslave`; the quad/TDM output variants; other synth/effect nodes (`synth_waveform`, `effect_*`); a capture→playback passthrough demo; MQS/SPDIF/PWM outputs.

## `AudioOutputI2S::begin()` — the main new code

The TX-DMA seam, mirroring the input `begin()` and the core's HW-verified `I2SClass::beginDMA()`. The `__IMXRT1176__` branch:
- `dma` TCD: `SADDR = i2s_tx_buffer` (advancing, `SOFF = 2`), `DADDR = &SAI1_TDR0` (**offset +0**, fixed, `DOFF = 0`), 16-bit `ATTR_SRC/ATTR_DST`, `NBYTES = 2`, `SLAST = -sizeof(i2s_tx_buffer)`, `CITER/BITER = sizeof(i2s_tx_buffer)/2`, `CSR = INTHALF|INTMAJOR`.
- `dma.triggerAtHardwareEvent(DMAMUX_SOURCE_SAI1_TX)`.
- Enable: `SAI1_TCSR = SAI_TCSR_TE | SAI_TCSR_BCE | SAI_TCSR_FRDE` (transmitter + bit clock + FIFO DMA request) — the same enable the core's `beginDMA()` uses.
- `update_responsibility = update_setup()`; `dma.enable()`; `dma.attachInterrupt(isr)`.

**KEY (pre-empting the input bug):** `DADDR = &SAI1_TDR0` at **offset +0**, matching the core's HW-verified `i2s_dma.destination(*(volatile uint16_t *)&SAI1_TDR0)` — NOT the Teensy-1062 `+2`. Our `config_i2s` (`FBT=15`) packs the 16-bit word into the lower half; writing `+2` would drop samples into the always-ignored upper half → silence. This is the TX analog of the `AudioInputI2S` `RDR0 +0` fix.

`isr()` (already `__IMXRT1176__`-guarded): on each half/complete DMA interrupt, read `dma.TCD->SADDR` to tell which half just drained, refill it interleaved from `block_left_1st`/`block_right_1st` (advancing `block_*_offset`, swapping in `_2nd` / releasing when a block is consumed), and on the first (half) interrupt pend `update_all()`. `arm_dcache_flush_delete` must be a no-op inline here (dcache off) — verify, as with input's `arm_dcache_delete`. `update()` (graph-side, platform-independent) receives up to two blocks per channel from the graph and queues them into `block_*_1st`/`_2nd`.

## `AudioSynthWaveformSine` (the graph source)

Already in the fork (`synth_sine.{cpp,h}`), pure computation: a phase accumulator indexing the 257-entry `AudioWaveformSine` table (`data_waveforms.c`, also in the fork) with linear interpolation, scaled by `amplitude`. The `update()` fast path is `#if defined(__ARM_ARCH_7EM__)` — which the RT1176 Cortex-M7 satisfies — so **no `__IMXRT1176__` branch is needed**. The only port risk is whether the ARMv7E-M DSP intrinsics it uses (`signed_multiply_32x16b`, `signed_saturate_rshift`, `multiply_32x32_rshift32`, etc.) are declared in the core's headers; if any are missing, add the small inline-asm macros to the core (from the Teensy `cores/teensy4` originals). API: `frequency(hz)`, `amplitude(0..1)`. Verified standalone via `synth → analyze_peak` (a sine's peak ≈ its amplitude) before it is trusted as the output test source.

## `AudioControlWM8962` (codec) — reused as-is

`enable()` already runs the WM8962 full init, which the core's `wm8962.cpp` shows configures the DAC→headphone playback route (`DACToHeadphonePowerUp`, DAC volume `LDAC/RDAC`, headphone volume `LOUT1/ROUT1`) — the same init HW-verified earlier as an audible 1 kHz sine on J101. No output-specific codec change is required.

## Data flow

`AudioSynthWaveformSine::update()` (phase accumulator + table lookup) `transmit()`s a block → `AudioConnection` → `AudioOutputI2S::update()` queues it into `block_left/right_1st/2nd` → the TX DMA drains `i2s_tx_buffer` (double-buffered, half/complete IRQ) into `SAI1_TDR0` → SAI1 TX shifts it to the WM8962 → DAC → J101 (and, in QEMU, the `sai1-tap` chardev writes the transmitted samples to a file).

## Testing — QEMU gate + HW

**Gate `evkb/audiooutput_i2s_test/`** — compiles the core (AudioStream + regs + DMAChannel) plus the fork files (`output_i2s`, `synth_sine`, `data_waveforms`, `control_wm8962`, and `analyze_peak` for the synth sanity). Firmware: `AudioSynthWaveformSine sine; AudioOutputI2S out; AudioConnection c0(sine,0,out,0); AudioConnection c1(sine,0,out,1); AudioControlWM8962 wm;` — `AudioMemory(N)`, `wm.enable()`, `sine.frequency(1000); sine.amplitude(0.5)`.
- **QEMU (`STAGE_TONE`)**: run the graph for a fixed interval; the `sai1-tap` file (reuse `i2s_audio_test`'s `-chardev file,id=sai1-tap`) captures the transmitted SAI samples; assert the tap is non-silent with the expected peak amplitude (a broken DMA/queue/alignment → silent or wrong). Optionally a `STAGE_SYNTH` first: `synth → analyze_peak` non-zero. `AUDIOOUTPUT_ALL=PASS`. Mirror the existing gate scaffolding.
- **HW**: flash; the 1 kHz tone is audible on J101 headphone/line-out. Controller drives flash+VCOM; the user confirms the tone (a brief bench moment, like the mic test). QEMU is timer/tap-paced, so the exact 44.1 kHz rate is a HW item (sanity-check the frame-sync / pitch if measurable).

## HW verification

The silicon proof is the audible tone on J101 (playback route already HW-verified for the raw I2S driver; this proves it through the audio graph). On HW the tap is absent, so the QEMU exact-amplitude assert is QEMU-only; the audible tone is the silicon proof.

## Key risks (all pre-empted or low)

- **TDR0 half-word alignment** — use `+0` (match the core), not the Teensy `+2`. Explicitly called out above.
- **`arm_dcache_flush_delete` no-op** — verify it's an inline no-op in the core (dcache off), as `arm_dcache_delete` was for input.
- **Synth DSP intrinsics** — verify `signed_multiply_32x16b` et al. are in the core; add if missing (small inline asm).
- **`update()` platform-independence** — confirm `AudioOutputI2S::update()` has no unguarded Teensy-only bits.

## References

- Teensy Audio (verbatim structure): `~/.platformio/.../libraries/Audio/{output_i2s,synth_sine,data_waveforms,control_wm8960}.*`; fork copies at `~/Development/Audio/`.
- The core's HW-verified I2S TX DMA: `cores/imxrt1176/I2S.cpp` `beginDMA()` (`destination(*(volatile uint16_t *)&SAI1_TDR0)`, `DMAMUX_SOURCE_SAI1_TX`, `TCSR |= TE|BCE|FRDE`). WM8962 playback route: `wm8962.cpp`.
- [[rt1176-audioinput-i2s]] (the input twin + the two silicon-only-bug lessons), [[rt1176-audiostream]] (the framework), [[rt1176-i2s-sai]] (SAI TX + WM8962 playback, HW-verified J101), [[rt1176-edma-dmachannel]] (DMAChannel + DMAMUX).
