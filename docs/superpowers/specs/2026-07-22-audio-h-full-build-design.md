# `Audio.h` compiles — full RT1176 library build (Phase A step 3) — design

**Date:** 2026-07-22
**Goal:** make `#include <Audio.h>` compile and link on the RT1176 CM7 by
building the entire Audio fork as one library (every `.cpp`), so a user gets
the Teensy experience — include the umbrella header, use any compatible node,
no cherry-picking. Phase A step 3 (the final 🟡) of
`Audio/docs/rt1170-evkb-status.md`; follows the CMSIS-DSP and guard-sweep
phases.

## Decisions (from brainstorm 2026-07-22)

| Question | Decision |
|---|---|
| Compile scope | **Full library build** — every Audio `.cpp` compiles clean for RT1176 (empty-on-1176 nodes contribute empty objects), not just header resolution. |
| SerialFlash (the one missing dep) | **Add to the manifest** (`newdigate/SerialFlash`, MIT-verified). Chain-imported by Audio; not guarded out. |
| Runtime proof | **Modest multi-family known-answer chain** through 4-5 node families via the full `<Audio.h>` path. |

## Established facts (grounding)

- `Audio.h` pulls **81** node headers. Beyond `Arduino.h`/`AudioStream.h`/
  `DMAChannel.h`/`arm_math.h`, the header graph reaches four externals:
  `Wire.h` (I2C codec controls), `<SD.h>` (SD players — manifest lib exists),
  `<SerialFlash.h>` (`play_serialflash_raw` — NOT in the manifest, MIT,
  `newdigate/SerialFlash`), and the fork's own `sai1176.h`/`spi_interrupt.h`.
- The ~20 hardware-I/O nodes are whole-file `__IMXRT1062__`/KINETIS guarded, so
  their `.cpp` bodies are empty on RT1176 — they compile as empty objects *iff*
  their includes + any refs outside the guard resolve; they link only if
  instantiated (which a user won't, on this board).
- Existing gates cherry-pick Audio sources (`evkb_library_dir(Audio ...)` +
  `target_sources`); they do NOT glob the fork. This full-build path is new and
  additive — no gate is migrated.
- CMSIS-DSP is a manifest lib with `import_evkb_cmsis_dsp()` (CM7 target) +
  `evkb_cmsis_dsp_cm4_sources()` (CM4). The guard sweep made the DSP nodes
  `__ARM_ARCH_7EM__`-guarded (compile on the CM7). The SerialFlash-in-
  `synth_wavetable.cpp` stale include was already found + stripped — more may
  lurk; the full compile is the discovery mechanism.

## 1. Full-build path: `import_evkb_library(Audio)` + dep chain-import

- A CMake path that compiles ALL of the Audio fork (`*.cpp`, `*.c`, `utility/`)
  into one `Audio` library target for the RT1176 CM7, using the same
  `teensy_flags`/CM7 compile options as `cores`.
- **Chain-imports its dependencies** so `#include <Audio.h>` + link "just
  works": `cores` (AudioStream/DMAChannel/data), the CMSIS-DSP CM7 target
  (`import_evkb_cmsis_dsp`), Wire, SD (+SdFat), SerialFlash — all on the
  include path, linked, `--gc-sections` dropping unused nodes.
- Additive: the cherry-picking gates keep their `evkb_library_dir` +
  `target_sources` shape. The new path is what the `Audio.h` gate (and future
  "just include Audio" consumers) use.
- Mechanism detail deferred to the plan: whether this extends
  `import_evkb_library` (which today wraps `import_arduino_library`, a globbing
  importer) or adds a dedicated `import_evkb_audio_full()`. The globbing-vs-
  explicit and the CONFIGURE_DEPENDS staleness trap ([[rt1170-gate-glob-staleness]])
  are plan-time concerns.

## 2. Compile sweep — make every `.cpp` build clean (the bulk of the work)

Front-loaded task: build the whole library, triage every error, fix in the
Audio fork. Expected categories:

- **Stale/external includes** (like the fixed `SerialFlash.h` in
  `synth_wavetable.cpp`): resolve via the manifest (SerialFlash) or strip if
  genuinely unused (NOTE-comment convention).
- **Kinetis/1062-only nodes** (`output_adat` KINETISK-only, `output_pwm` Kinetis
  FTM, `output_dac`/`output_dacs` Kinetis DAC, `input_adc`/`input_adcs` Kinetis
  ADC, and the `input/output_i2s2`, `_quad`/`_hex`/`_oct`, `_tdm`/`_tdm2`,
  `_pdm`/`_pdm_i2s2`, `spdif*`, `pt8211*` family): the body is empty on 1176,
  but stray includes/type refs *outside* the guard must compile. Fix each to
  produce a clean empty object (guard the offending include/ref; add
  `__IMXRT1176__` awareness where a node should be "present but empty"). They
  remain uninstantiable (link-error only if used — documented).
- **Header collisions** from 81 headers in one TU (macro clashes, ODR) — fix
  minimally; the CMSIS `A0-A2`/IRQ-inline shim is precedent if a similar clash
  appears.
- Constraint: every fix is firewall-clean (MIT/BSD) and must NOT change the
  compiled behavior of the already-ported/HW-verified nodes (guard-swept
  synths, i2s/int nodes, filters, fft). Prove via the regression gates (§5).

## 3. SerialFlash → manifest

- `_evkb_lib(SerialFlash ${_dev}/SerialFlash https://github.com/newdigate/SerialFlash <sha> .)`
  pinned at plan time. Chain-imported by the Audio full-build path.
- `tools/license-audit.sh` REPOS += `$HOME/Development/SerialFlash`; the
  full-lib depfile walk then covers `play_serialflash_raw` automatically.

## 4. Gate `examples/audio/audio_h_test` (CM7)

- One TU: `#include <Audio.h>` (aggregate header resolves) linked against the
  **full** `Audio` library (every `.cpp` compiled + lib links).
- Runtime known-answer chain across 4-5 families: `synth_sine → filter_fir →
  an effect (mixer or bitcrusher) → analyze_fft256 + analyze_peak`.
  Deterministic assertions (FFT peak bin from the sine freq, peak level),
  poll-loop runner (no fixed sleep), un-fakeable tokens.
- Two-gate rule: QEMU green, then HW-verified on the EVKB; transcripts
  committed. Report the gate binary size (`--gc-sections` should bound it
  despite pulling the whole lib).

## 5. Verification & rollout

- **License audit strengthens**: the full-lib compile makes the Part-2 depfile
  walk cover EVERY Audio file (today only cherry-picked files are audited). Add
  `audio_h_test` to GATES; run → PASS.
- **Regressions** (the node fixes must not break ported nodes): re-run
  filter_fir_test, guard_sweep_test, audiostream_test, sd_wav_play_test,
  i2s_int_test, cm4_fft_test, cm4_audio_test.
- Push Audio + SerialFlash; add the SerialFlash pin; bump the Audio pin to the
  new HEAD; `-DEVKB_FORCE_FETCH=ON` proof of `audio_h_test`.
- Docs: `Audio/docs/rt1170-evkb-status.md` — `Audio.h (master include)` row
  🟡 → ✅ (the last 🟡 clears); mark **Phase A COMPLETE**; changelog. Update the
  legend (no 🟡 rows remain among components). `examples/README.md` +
  cm4-roadmap / audio-library-roadmap.

## 6. Scope boundaries

- **CM7 only.** Audio.h on the CM4 (128 K ITCM) is out of scope — the CM4 uses
  cherry-picked interrupt-driven nodes; a full Audio.h wouldn't fit.
- The bar is **compiles + links + the known-answer chain runs**. Nodes with no
  RT1176 hardware path compile to empty, uninstantiable objects; *using* one is
  a documented link-error, not a compile error (honest — you can't drive a node
  that isn't ported).
- Not migrating the existing cherry-pick gates; not porting new I/O nodes
  (SPDIF/MQS/DAC12 remain the separate 🟠 track).

## 7. Risks

- **Unknown compile-issue count** across ~80 files — genuinely uncertain until
  the first full build (the SerialFlash-in-wavetable precedent implies more).
  The plan front-loads a "compile the whole lib, triage every error" task and
  the estimate firms up only after it runs.
- **Header collisions** (81 headers, one TU) — shim precedent exists.
- **Gate size** — `--gc-sections` should keep it bounded; report it, and if a
  node's empty object still drags large const data, that's a finding.
- **Firewall**: SerialFlash and any newly-compiled files must stay permissive
  (the strengthened audit enforces it).

## Out of scope

Audio.h on the CM4; new I/O-node ports (SPDIF/MQS/output_dac12); migrating
cherry-pick gates; simultaneous dual-core audio.
