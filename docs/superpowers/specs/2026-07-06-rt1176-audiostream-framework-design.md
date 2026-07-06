# RT1176 AudioStream framework — Design

**Status:** approved (design), ready for implementation plan
**Date:** 2026-07-06
**Sub-project:** A of the AudioInputI2S effort. B (`AudioInputI2S` in the `newdigate/Audio` fork + a `analyze_peak` verify) depends on this and gets its own spec → plan → implement cycle after this ships.

## Goal

Port the Teensy 4 audio-graph engine — `AudioStream` — into the RT1176 core, so audio nodes (`AudioInputI2S`, `analyze_*`, etc., which already live in the `newdigate/Audio` fork) have their base class, block pool, connection mechanism, and update dispatch. This is the foundation every audio node depends on; nothing audio works without it.

## Background / current state

- **No audio-graph infrastructure exists** in `cores/imxrt1176` — no `AudioStream`, `audio_block_t`, or `AudioConnection`.
- In Teensy, `AudioStream.{h,cpp}` is a **core** file (`cores/teensy4/`), NOT part of the Audio library; the Audio library holds only the nodes. So the framework belongs in **teensy-cores** (`cores/imxrt1176/`); the nodes belong in the **`newdigate/Audio` fork** (`git@github.com:newdigate/Audio.git`, at `~/Development/Audio`, branch `master`) — the user's fork, used for all Audio-repo commits.
- The update dispatch is portable: Teensy runs the graph from a low-priority **software IRQ** (`update_setup()` → `attachInterruptVector(IRQ_SOFTWARE, software_isr)` + low NVIC priority; a clock owner pends it → `software_isr()` → `AudioStream::update_all()`). This core already has `attachInterruptVector` (IntervalTimer/Wire/GPIO/USB use it) and `NVIC_SET_PENDING` — the only retarget is choosing a spare NVIC vector as `IRQ_SOFTWARE`. No PendSV needed.
- Reference: `~/.platformio/packages/framework-arduinoteensy/cores/teensy4/AudioStream.{h,cpp}` (~179 + 337 lines) — verbatim source of truth.

## Scope

**In scope:** `AudioStream.{h,cpp}` ported into `cores/imxrt1176/` — `audio_block_t`, the block pool (`allocate`/`release`, `AudioMemory(n)`), the `AudioStream` base (`update()`, `transmit`/`receive`/`releaseBlock`, connection arrays), `AudioConnection`, `update_all()`, and the software-IRQ dispatch (`software_isr` + `IRQ_SOFTWARE`). Constants `AUDIO_BLOCK_SAMPLES=128`, `AUDIO_SAMPLE_RATE=44100`. Block pool in `DMAMEM`. A synthetic-node QEMU gate.

**Explicitly deferred (YAGNI):** the actual audio nodes (`AudioInputI2S` etc. — sub-project B, in the fork); `AudioSample_t`/interpolation helpers not needed by the minimal node set; a 48000 sample-rate variant; CPU-usage/`processorUsage` instrumentation beyond what the verbatim port includes.

## Key decisions

- **`AUDIO_SAMPLE_RATE = 44100`** (Teensy default, NOT our SAI's 48000). Rationale: the Teensy filters/oscillators/effects bake 44100 into their coefficient math, so keeping the framework at 44100 makes those port cleanly later. Consequence for sub-project B: the SAI/WM8962 (currently 48 kHz) will be reconciled to 44.1 kHz then, so capture rate matches the graph — a B-time decision, out of scope here.
- **Block pool in `DMAMEM` (OCRAM).** Sub-project B's `AudioInputI2S` DMA fills `audio_block_t`s directly, and DTCM is DMA-unreachable on this silicon (the [[rt1176-serialusb]] rule). Placing the pool in `DMAMEM` now avoids reworking it in B.
- **`IRQ_SOFTWARE` = a spare/reserved NVIC vector.** The plan picks a confirmed-unused IRQ number (checked against `startup.c`'s vector table / the core's `IRQ_NUMBER_t` enum), `attachInterruptVector`s `software_isr` to it, sets it low priority, and pends it via `NVIC_SET_PENDING`. In real use the audio clock owner (the I2S DMA ISR, in B) pends it; here the gate pends it directly.

## Architecture — components

| Piece | Responsibility |
|---|---|
| `audio_block_t` | 128-sample (`AUDIO_BLOCK_SAMPLES`) int16 block + refcount. |
| Block pool | Static `DMAMEM` array + bitmap; `allocate()`/`release()`; `AudioMemory(n)` sizes usable blocks. |
| `AudioStream` base | Per-node `update()` (virtual), `transmit(block, ch)`, `receiveReadOnly`/`receiveWritable`, the output/input connection arrays, node registration into the global update list. |
| `AudioConnection` | Binds a source node's output channel to a destination node's input channel; `connect()`/`disconnect()`. |
| `update_all()` | Walks the registered `AudioStream` list, invokes each `update()`. |
| `software_isr` / `IRQ_SOFTWARE` | Low-priority software IRQ whose handler calls `update_all()`; pended by the clock owner. |

## Data flow

A node's `update()` `allocate()`s output blocks, fills them, and `transmit()`s them to its output channels; each `AudioConnection` makes a transmitted block visible to the destination's input; the destination's `update()` `receive*()`s it and `release()`s when done. `update_all()` drives one pass over the whole graph. The pass is triggered by pending `IRQ_SOFTWARE` (→ `software_isr` → `update_all`) at the audio block rate — by the I2S DMA in real use, by the gate here.

## Testing — QEMU gate (`evkb/audiostream_test/`, pure firmware — no QEMU device)

Two trivial synthetic nodes prove the engine with no peripheral:
- A **source** node: `update()` `allocate()`s a block and fills it with a known, per-block-incrementing pattern (e.g. block *k* is filled with value *k*), then `transmit()`s it.
- A **sink** node: `update()` `receiveReadOnly()`s its input and records the first sample (and that it received a block at all), then `release()`s it.
- Wire source→sink with an `AudioConnection`; `AudioMemory(N)`.

Stages (VCOM markers): `STAGE_FLOW` — pend `IRQ_SOFTWARE` a fixed number of times (standing in for the DMA trigger); assert the sink received each expected per-block value in order (proves `transmit`/`AudioConnection`/`receive`/`update_all`/dispatch). `STAGE_NOLEAK` — after the run, the pool's free-block count equals its starting count (proves `allocate`/`release` balance, no leak). Final `AUDIOSTREAM_ALL=PASS`. Mirror the existing gate scaffolding (`sleep 5`). Runs on the existing `mimxrt1170-evk` machine — no device-model work.

## HW verification

Light smoke test: flash the gate, confirm `STAGE_FLOW` + `STAGE_NOLEAK` PASS on silicon (the software-IRQ dispatch + block pool run on real hardware). The framework has no peripheral, so the substantive audio HW proof (mic → graph) comes with sub-project B.

## References

- Teensy 4 core (verbatim source): `~/.platformio/packages/framework-arduinoteensy/cores/teensy4/AudioStream.{h,cpp}`.
- Audio nodes fork (sub-project B target): `git@github.com:newdigate/Audio.git` @ `~/Development/Audio`.
- Core hooks: `attachInterruptVector`/`NVIC_SET_PENDING` (`core_pins.h`/`imxrt1176.h`), `DMAMEM`, `startup.c` vector table. See [[rt1176-i2s-sai]] / [[rt1176-sai-rx]] (the I2S the nodes will use), [[rt1176-edma-dmachannel]], [[rt1176-eventresponder]].
