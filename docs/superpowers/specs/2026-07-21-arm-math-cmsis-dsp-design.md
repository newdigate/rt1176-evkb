# CMSIS-DSP (`arm_math`) for the RT1176 ecosystem — design

**Date:** 2026-07-21
**Goal:** provide `arm_math.h` + the CMSIS-DSP library to RT1176/EVKB firmware,
unblocking the Audio fork's 🔵 components (analyze_fft256/1024, filter_fir,
filter_ladder, effect_flange, synth_tonesweep) and, eventually, the `Audio.h`
master include. Phase A step 1 of `Audio/docs/rt1170-evkb-status.md`.

## Decisions (from brainstorm 2026-07-21)

| Question | Decision |
|---|---|
| Scope | **Full Teensy parity** — the complete CMSIS-DSP, not an Audio-only subset. `--gc-sections` keeps firmware size unaffected. |
| Version | **Latest CMSIS-DSP release (v1.16.x line)** from ARM's actively maintained standalone repo — not the 2017 v1.5.1 that `cores/teensy4` shipped. |
| Placement | **Pinned manifest library in `evkb.cmake`** — no code vendored into our repos; local-first `~/Development/CMSIS-DSP` checkout wins; upgrade = pin bump. |
| Proof | **Standalone known-answer gate + one real Audio filter chain** (sine → filter_fir → analyze_fft256), QEMU + HW per the two-gate rule. |

Why not a prebuilt `.a` (the PJRC approach): our ARM GCC 10 ships no CMSIS
library, and a binary blob would bypass `license-audit.sh`'s depfile audit.
Building from Apache-2.0 sources keeps the license firewall meaningful.

## 1. Manifest & build integration

- `evkb.cmake` gains a `CMSIS-DSP` manifest entry pinned to the SHA of the
  latest `ARM-software/CMSIS-DSP` release tag (resolved at implementation
  time). Local-first resolution as for every other library.
- CMSIS-DSP is not Arduino-layout, so it bypasses `import_arduino_library`:
  the manifest defines one **static library target** compiling
  `Source/*/*.c`, with `Include/` + `PrivateInclude/` public/private include
  dirs. We do **not** use CMSIS-DSP's own CMake build (avoids its option
  surface and CMSISCORE plumbing); our target mirrors the flat
  "compile everything, let the linker garbage-collect" approach.
- Compile flags match the rest of the tree: CM7, `-mfpu=fpv5-d16
  -mfloat-abi=hard`, `-ffunction-sections -fdata-sections`, same optimization
  level as `cores`. Full-table builds are fine — unused functions/tables are
  dropped at link time by `--gc-sections`.
- Consumers: `import_evkb_library(CMSIS-DSP)` +
  `teensy_target_link_libraries(app … CMSIS-DSP)`. The Audio import does
  **not** chain-import it yet (gates cherry-pick Audio sources); revisit when
  the Audio.h master-include gate lands (Phase A step 3).

## 2. CMSIS-Core headers dependency

Modern CMSIS-DSP requires CMSIS-Core (`cmsis_compiler.h`/`cmsis_gcc.h`
intrinsics), which the Teensy-derived core deliberately lacks (it has its own
`imxrt.h` register model). The manifest therefore pins a second,
**headers-only** fetch: `ARM-software/CMSIS_6` (Apache-2.0). Only
`CMSIS/Core/Include` is exposed, and only on the CMSIS-DSP target's include
path — nothing from CMSIS-Core is compiled, and it must not leak onto the
include path of `cores` or sketches (no collision with `imxrt.h`).

## 3. Gates (QEMU + HW, two-gate rule)

### `examples/framework/arm_math_test/` — standalone known-answer

No Audio sources. Exercises the library directly and prints greppable tokens
on LPUART1:

- `arm_cfft_radix4_q15` (+init) on a synthetic 256-point tone: assert the
  energy lands in the expected bin with the expected magnitude
  (`ARM-MATH: fft bin=<n> mag=<m>`).
- `arm_fir_fast_q15` (+init) fed a unit impulse: output must echo the
  coefficient array.
- `arm_sin_q31` vs libm `sinf` across a sweep, within tolerance.
- Final `ARM-MATH: PASS`.

`run_qemu.sh` (gate-lib.sh + qrun as usual) asserts the tokens. The identical
ELF runs on the EVKB (LinkServer) and must print the identical tokens on the
VCOM. Pure CPU math — zero qemu2 model changes expected; any divergence is a
real finding, not a model gap.

### `examples/audio/filter_fir_test/` — the "filters unblocked" proof

First RT1176 compile of `filter_fir`, `analyze_fft256` (and `mixer` if the
chain needs it). Graph: `synth_sine → AudioFilterFIR (low-pass, known cutoff)
→ analyze_fft256 + analyze_peak`.

- Phase 1: passband sine (e.g. 1 kHz) — assert the FFT peak bin is the sine's
  bin and the level is within tolerance of unfiltered.
- Phase 2: stopband sine (well above cutoff) — assert relative attenuation in
  dB (an un-fakeable relative measurement, robust to absolute-scale drift).
- Tokens: `FIR: pb bin=<n> mag=<m>`, `FIR: sb atten=<db>`, `FIR: PASS`.

Drive mechanism: reuse `examples/audio/audiostream_test/`'s graph-update
drive; adopt `-icount` if timing wobbles, as the tone gates did. HW run: same
firmware, same tokens over VCOM.

## 4. License & docs

- `license-audit.sh` Part 1 sweep gains the CMSIS-DSP and CMSIS_6 checkout
  paths (Apache-2.0 headers are permissive — pass). Part 2's depfile audit
  covers the new gates automatically once they're among the audited builds;
  add `arm_math_test` to the audited-gates list if its coverage differs from
  the existing three.
- Update `Audio/docs/rt1170-evkb-status.md`: 🔵 rows → "unblocked
  (CMSIS-DSP in manifest)"; `filter_fir` + `analyze_fft256` → ✅ once
  HW-verified; ladder/flange/tonesweep stay pending their own gates.
- Update the `rt1176-audio-library-roadmap` memory and add the pin-maintenance
  note (new pins = CMSIS-DSP + CMSIS_6) to the README's existing
  pinned-manifest caveat if wording needs it.

## 5. Risks & drift points

- **`arm_cfft_radix4_*` is deprecated upstream** (still shipped in v1.16.x).
  The pin protects us. If ARM removes it: options are freezing the pin or
  migrating the Audio FFT nodes to `arm_cfft_q15`. Documented, not solved now.
- **v1.5.1 → v1.16 API drift:** Teensy-era code includes the monolithic
  `arm_math.h`; the modern umbrella header re-exports the split per-family
  headers, so includes should work unchanged. `arm_math_test` is the canary —
  if the Audio nodes' calls (`arm_cfft_radix4_init_q15`,
  `arm_fir_init_q15`, …) drift in signature, it surfaces here first.
- **CMSIS-Core include-path leakage** into sketches could shadow nothing today
  (distinct filenames from `imxrt.h`) but is kept private to the CMSIS-DSP
  target as a matter of hygiene.
- **QEMU FP fidelity:** q15/q31 paths are integer; f32 paths use the CM7 FPU,
  which qemu2 models via softfloat — known-answer tolerances are set loose
  enough (relative dB, bin indices) to be robust, and HW remains the oracle.

## Out of scope

- Guard sweep of the 🟡 chip-list-guarded nodes (Phase A step 2).
- The "Audio.h compiles" gate and Audio→CMSIS-DSP chain-import (Phase A step 3).
- Gates for filter_ladder, effect_flange, synth_tonesweep, fft1024, notefreq.
- Any CM4 involvement.
