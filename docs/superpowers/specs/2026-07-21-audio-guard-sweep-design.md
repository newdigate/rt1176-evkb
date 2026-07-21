# Audio-fork guard sweep (Phase A step 2) — design

**Date:** 2026-07-21
**Goal:** revive the Audio components silently dead or degraded on RT1176 due to
Teensy chip-list guards, and prove each revival with runtime known-answer
assertions. Phase A step 2 of `Audio/docs/rt1170-evkb-status.md`; follows the
CMSIS-DSP phase (`2026-07-21-arm-math-cmsis-dsp-design.md`).

## Decisions (from brainstorm 2026-07-21)

| Question | Decision |
|---|---|
| Capability-guard fix | **Switch `KINETISK \|\| __IMXRT1062__` → `__ARM_ARCH_7EM__`** — provably behavior-preserving on every Teensy (KINETISK = Cortex-M4, 1062 = M7, both define it; KINETISL = M0+ stays excluded) and future-proof. Memory-size guards stay chip-lists and gain `__IMXRT1176__`. |
| synth_wavetable proof | **Synthetic mini-instrument** built in the gate source (single-cycle sine, no loop) — real synthesis proof, not compile-only. |

## 1. Audio-fork changes (~/Development/Audio, 6 files)

**Capability guards → `__ARM_ARCH_7EM__`** (4 sites; each currently
`#if defined(KINETISK) || defined(__IMXRT1062__)`):
- `synth_karplusstrong.cpp:30` (pseudorand helper) and `:47` (update body)
- `synth_simple_drum.cpp:112` (update body)
- `synth_wavetable.cpp:179` (update body)

**Memory-size guards gain `__IMXRT1176__`** (RAM-class chip lists — RT1176
joins the largest tier; these gate *pointer-array* depths, so real memory stays
bounded by the sketch's `AudioMemory()` pool):
- `effect_delay.h:33` → `#if defined(__IMXRT1062__) || defined(__IMXRT1176__)`
  (4.00-second `DELAY_QUEUE_SIZE`)
- `play_queue.h:36` → add `__IMXRT1176__` (MAX_BUFFERS 80)
- `record_queue.h:36` → add `__IMXRT1176__` (max_buffers 209)

**Stale-include strips** (fork precedent: synth_sine.h's NOTE comment):
- `synth_waveform.h:32` — remove `#include <arm_math.h>` (no arm_ symbols used)
- `analyze_notefreq.cpp:26` — remove `#include "arm_math.h"` (no arm_ calls)
- `synth_wavetable.cpp:30` — remove `#include <SerialFlash.h>` (zero SerialFlash
  symbols used; discovered at implementation time when guard_sweep_test became
  the first gate to compile this file — the red-phase gate uses a temporary
  empty stub header until this strip lands)

> **Amendment (post-implementation, 2026-07-21):** a FOURTH strip site shipped:
> `synth_waveform.cpp:29` also carried its own stale `arm_math.h` include
> (caught in code review — the .h strip's NOTE claim would otherwise have been
> false). Final tally: 11 edit sites across 9 files (Audio d886718 + 460d0c1).
> Recorded red-phase gate signature on pre-sweep sources, reproduced
> independently at final review: `delay early=0.9000`, karplus/drum/wt
> `a=0.0000`, `recq held=52`, PLAYQ hung (old 32-cap busy-wait), `FAIL: DELAY`.

Without the strips, any consumer of these headers/sources needs CMSIS-DSP for
no reason; with them, neither file depends on the manifest library at all.

## 2. Gate: `examples/audio/guard_sweep_test` (evkb repo)

Same architecture as `filter_fir_test`: GraphClock no-op node to arm
`IRQ_SOFTWARE` dispatch, manual `pump()` drive, cherry-picked Audio sources,
LPUART1 tokens, gate-lib/qrun runner. CMSIS-DSP NOT imported (none of these
nodes needs it — which also proves the include-strips).

Five runtime stages (tokens `STAGE_<NAME>=PASS|FAIL` + a final
`GUARD_SWEEP_ALL`):

- **KARPLUS** — `noteOn(220, 1.0)`; assert an initial peak burst above a
  threshold, then a later window measurably quieter (pluck decay). Both
  windows read via `analyze_peak`.
- **DRUM** — configure `length/frequency/pitchMod`, `noteOn()`; assert
  burst-then-decay like KARPLUS.
- **WAVETABLE** — a synthetic single-cycle sine `instrument_data` defined in
  the gate source (exact struct layout taken from this fork's
  `synth_wavetable.h` at plan time); `playNote`, assert sustained output level
  within a predicted range, then `stop` and assert silence returns.
- **DELAY** — sine → `effect_delay` tap at 200 ms → peak. 200 ms needs a
  ~69-block queue: **impossible under the old `#else` fallback (~139 ms max),
  guaranteed under the new 4-second branch** — the stage fails on pre-sweep
  sources by construction. Assert the tap is silent for ~60 pumped blocks and
  loud after ~75.
- **QUEUES** — `record_queue`: capture and HOLD >53 blocks (old cap), then
  drain and verify count and content ordering; `play_queue`: enqueue >32
  blocks. Proves the deep-buffer branches are live.

`AudioMemory(90)` — sized so pool exhaustion cannot masquerade as a guard
failure (record-queue hold of ~60 + delay queue ~69 pointers + graph slack;
exact number re-derived in the plan).

## 3. Verification & rollout

1. QEMU gate green (`./run_qemu.sh`, transcript committed).
2. Same ELF HW-verified on the EVKB (LinkServer + pyserial reader; transcript
   committed; integer-derived lines expected identical to QEMU).
3. Regressions re-run (Audio sources changed): `filter_fir_test` and
   `audiostream_test` QEMU gates.
4. Push Audio → **bump the Audio pin in `evkb.cmake`** to the new SHA (the
   documented manual maintenance step) → verify a `-DEVKB_FORCE_FETCH=ON`
   configure of guard_sweep_test fetches and builds green.
5. Docs: status doc — `synth_karplusstrong`, `synth_simple_drum`,
   `synth_wavetable`, `effect_delay`, `play_queue`, `record_queue` → ✅
   (HW-verified via guard_sweep_test); `synth_waveform`/`analyze_notefreq`
   notes drop the stale-include caveat; 🟡 legend narrows to the remaining
   guard-fix candidates. Roadmap: Phase A step 2 DONE; step 3 (Audio.h gate)
   next. Memory updates.

## 4. Risks & notes

- **Wavetable instrument struct**: ~2 dozen fields (`sample_data` +
  `instrument_data`); the plan must derive every field value from the fork's
  own header definitions, not upstream docs. Envelope defaults may shape
  output level — the WAVETABLE stage asserts a generous range, not an exact
  value.
- **Decay-based assertions** (KARPLUS/DRUM) must use well-separated windows so
  q15 rounding can't flip them; thresholds derived in the plan with ≥3× margin.
- The sweep leaves `synth_karplusstrong`'s Teensy-3-era `#else` (silent
  fallback) in place where upstream had one — behavior for non-7EM chips is
  unchanged in every file.
- `Audio.h` master-include compile remains 🟡 (step 3 scope, not this phase).

## Out of scope

Phase A step 3 (Audio.h compile gate + chain-import), all 🟢-tier gate-writing
(fft1024/ladder/flange/tonesweep), all hardware I/O ports, CM4.
