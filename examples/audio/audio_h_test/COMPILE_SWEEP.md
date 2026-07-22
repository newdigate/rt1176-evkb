# audio_h_test — full Audio-library compile sweep (Task 2 inventory → Task 3 fixes)

First-ever whole-fork compile of `newdigate/Audio` for RT1176, driven by
`import_evkb_audio_full()` (globs all 87 `.cpp` + 6 `.c` + `utility/`). This is
the honest error inventory produced by Task 2; Task 3 works it to zero.

- **Configure:** GREEN (helper wires CMSIS-DSP + Wire/SPI/SdFat/SD/SerialFlash +
  the whole Audio glob).
- **Build:** ✅ **GREEN after Task 3** (was RED at Task 2 inventory). The whole
  Audio library (82 Audio/SerialFlash TUs + `utility/`) compiles clean, the
  `<Audio.h>` 81-header aggregation compiles clean, and `audio_h_test.elf` links.
- **Task 2 inventory (historical):** `cmake --build build -- -k` found **11 of 82
  TUs failing, every one a `fatal error: <header>: No such file or directory`** (a
  missing include). Because a fatal include aborts the TU before its body parses,
  body-level errors and the aggregation were invisible then; Task 3 resolved the
  includes, re-ran, and surfaced/fixed the one second-wave body error
  (`effect_reverb`) — see the Second-wave findings section below.

Missing-header tally (from `/tmp/audioh_errors.txt`):

| missing header | count | TUs |
|---|---|---|
| `SPI.h`         | 5 | SerialFlashChip.cpp, SerialFlashDirectory.cpp, spi_interrupt.cpp, effect_delay_ext.cpp, play_serialflash_raw.cpp |
| `kinetis.h`     | 3 | input_adcs.cpp, output_dac.cpp, output_dacs.cpp |
| `SD.h`          | 2 | play_sd_wav.cpp, play_sd_raw.cpp |
| `math_helper.h` | 1 | effect_reverb.cpp |

---

## Category: Stale / external include  (8 items)

`X.h: No such file` — resolve by putting an already-manifested dep on the Audio
compile path (helper fix), or strip the include if the TU doesn't use the symbol
(node fix, Pattern A NOTE convention). Sub-locus tagged `[helper]` / `[node]` /
`[helper|node]` for Task 3 scoping.

- [x] `effect_reverb.cpp:math_helper.h` — Stale/external include — **[node]** RESOLVED, but NOT a pure strip (Task-2 inventory was wrong here). `math_helper.h` is a CMSIS-DSP *Examples* file absent from our include path, and `effect_reverb.cpp` uses no `math_helper` *symbol* — BUT it uses many `arm_math.h` symbols (`q31_t`, `arm_float_to_q31`, `arm_shift_q31`, `arm_add_q31`, `arm_q15_to_q31`, `arm_q31_to_q15`; the two local `*_guard_bits_q31` helpers take `q31_t*`) that `math_helper.h` was pulling in **transitively**. A bare strip broke the TU (12 undeclared-symbol errors, second-wave). **Fix:** replace the stripped include with `#include "arm_math.h"` — the real dependency, exactly how effect_flange/synth_tonesweep/filter_ladder include it (shim intercepts the A0-A2 clash). No existing gate compiles effect_reverb.cpp, so zero regression risk.
- [x] `play_sd_wav.cpp:SD.h` (via `play_sd_wav.h:32 #include <SD.h>`) — Stale/external include — **[helper]** RESOLVED via `import_evkb_library(SD src)` + `import_evkb_library(SdFat src)` in the helper. `src` puts `PaulS_SD/src` and `SdFat/src` on `teensy_flags`; SD.h→SdFat.h→`common/…` resolve relatively from src/, and `<FS.h>` comes from the core (`cores/imxrt1176/FS.h`). Genuine SD user; compiles clean, gc-sectioned at link (stub instantiates nothing).
- [x] `play_sd_raw.cpp:SD.h` (via `play_sd_raw.h:32`) — Stale/external include — **[helper]** RESOLVED by the same `SD src` / `SdFat src` helper fix.
- [x] `SerialFlashChip.cpp:SPI.h` (via `SerialFlash.h:32 #include <SPI.h>`) — Stale/external include — **[helper]** RESOLVED via `import_evkb_library(SPI)` in the helper (`newdigate/SPI`, `SPI/SPI.h` at root). SerialFlash's 2 sources now compile clean.
- [x] `SerialFlashDirectory.cpp:SPI.h` (via `SerialFlash.h:32`) — Stale/external include — **[helper]** RESOLVED by the same `import_evkb_library(SPI)`.
- [x] `play_serialflash_raw.cpp:SPI.h` (via `play_serialflash_raw.h:33 #include <SerialFlash.h>` → SPI.h) — Stale/external include — **[helper]** RESOLVED by `import_evkb_library(SPI)`.
- [x] `spi_interrupt.cpp:SPI.h` (via `spi_interrupt.h:32 #include <SPI.h>`) — Stale/external include — **[helper]** RESOLVED via `import_evkb_library(SPI)` (compiles against `newdigate/SPI`; no node edit). This is the same portable ISR-priority shim `sd_wav_play_test` already links; preferred over a guard per the "resolve via helper, not guard portable nodes" rule.
- [x] `effect_delay_ext.cpp:SPI.h` (via `effect_delay_ext.h:31 #include "spi_interrupt.h"` → SPI.h) — Stale/external include — **[helper]** RESOLVED via `import_evkb_library(SPI)` — compiles clean (no node edit). Kept portable (not Pattern-B guarded): the whole node is gc-sectioned at link since nothing instantiates AudioEffectDelayExternal, so "compiles empty, uninstantiable" is already satisfied without touching the source.

## Category: Kinetis / 1062-only ref outside a guard  (3 items)

Node body is `#if defined(__MK*__)`-guarded (compiles empty on RT1176), but the
file-scope `#include "utility/pdb.h"` sits **outside** that guard, and `pdb.h`'s
own guard `#if !defined(__IMXRT1052__) && !defined(__IMXRT1062__)` fails to
exclude RT1176 → it pulls Kinetis's `kinetis.h`, which does not exist here.

**One-line fix clears all three:** in `Audio/utility/pdb.h` widen the guard to
`#if !defined(__IMXRT1052__) && !defined(__IMXRT1062__) && !defined(__IMXRT1176__)`
(Pattern B — makes `pdb.h` empty on RT1176; the three nodes' bodies are already
`__MK*__`-only, so they emit empty objects). Confirmed body guards:
`output_dac.cpp` L31 `__MK20DX256__/__MK64FX512__/__MK66FX1M0__` (+ `__MKL26Z64__`),
`input_adcs.cpp` L32 same triple, `output_dacs.cpp` L31 `__MK64FX512__/__MK66FX1M0__`.

- [x] `input_adcs.cpp:kinetis.h` (via `utility/pdb.h:30`) — Kinetis/1062-only outside guard — **[node]** RESOLVED: widened `pdb.h` guard to `#if !defined(__IMXRT1052__) && !defined(__IMXRT1062__) && !defined(__IMXRT1176__)` (one-line shared fix). Node body already `__MK*__`-only → empty object on 1176.
- [x] `output_dac.cpp:kinetis.h` (via `utility/pdb.h:30`) — Kinetis/1062-only outside guard — **[node]** RESOLVED by the shared `pdb.h` guard.
- [x] `output_dacs.cpp:kinetis.h` (via `utility/pdb.h:30`) — Kinetis/1062-only outside guard — **[node]** RESOLVED by the shared `pdb.h` guard.

## Category: Header collision  (0 items — CONFIRMED CLEAN once observable)

Task 2 could not observe these; Task 3 can, and there are **none**. Once the
includes resolved, every node body parsed and the `Audio` library built, so
`audio_h_test.cpp` — the TU that does `#include <Audio.h>` (81 node headers in
one translation unit) — finally compiled. It compiled **clean on the first
try**: zero redefinition/ambiguity/type collisions across the full 81-header
aggregation (the CMSIS shim's A0-A2/IRQ armor already on `teensy_flags` covers
the only known macro clash). Pattern C was not needed.

## Category: Other  (Task 3 resolution)

- [x] **Helper-vs-node decision for the 5 `SPI.h` TUs — resolved via the HELPER.**
  All 7 include-only TUs (2×SD, 5×SPI) were fixed by the helper tweak, no
  Audio-source edit: added `import_evkb_library(SPI)` and changed `SD`/`SdFat`
  imports to `SD src`/`SdFat src`. Neither `effect_delay_ext` nor `spi_interrupt`
  was Pattern-B guarded — the "resolve via helper, not guard portable nodes" rule
  wins, and both are gc-sectioned at link anyway (stub instantiates nothing), so
  "compiles empty, uninstantiable" already holds without touching the sources.
- [x] **Latent errors behind the includes — one second wave, resolved.** Once the
  includes resolved, exactly one TU had a body error: `effect_reverb.cpp` had been
  wrongly stripped (it needs `arm_math.h`; see its item above). Fixed by including
  `arm_math.h` directly. No other body errors surfaced.
- [x] **Umbrella-header aggregation — TESTED, clean.** `#include <Audio.h>`
  (audio_h_test.cpp) now compiles; the 81-header aggregation had zero collisions
  (see Header-collision section).

### Second-wave findings (surfaced only after includes resolved)

1. **`effect_reverb.cpp` needed `arm_math.h`, not a strip** (Audio-node fix). Task-2
   classified it as a pure stale strip; the strip broke it (12 undeclared-symbol
   errors — `q31_t`/`arm_*`). Corrected to `#include "arm_math.h"`. Firewall-clean
   (MIT node; arm_math.h already on the CMSIS path). No gate compiles this file →
   no regression.
2. **Gate link bug: `CMSIS-DSP` was in `teensy_target_link_libraries`** (evkb gate
   CMakeLists fix, NOT an Audio edit). `teensy_target_link_libraries` appends `.o`
   to every name (correct for import_arduino_library targets `Audio.o`/`Wire.o`/…),
   but `CMSIS-DSP` is a plain `add_library(STATIC)` named `CMSIS-DSP` → produced
   `-lCMSIS-DSP.o` (not found). Latent Task-2 scaffold defect (never linked while
   compilation was red). Fixed by moving CMSIS-DSP to the raw
   `target_link_libraries(audio_h_test.elf CMSIS-DSP stdc++ m)`, matching
   filter_fir_test / arm_math_test (documented convention). Also added `SPI` to the
   teensy_ group (the helper now imports it).

---

## Summary — Task 3 result: GREEN

- **All 82 Audio/SerialFlash TUs + the `<Audio.h>` aggregation compile clean; the
  whole Audio library builds and `audio_h_test.elf` links.**
- **Original 11 include-fatals:** all resolved — 7 by the helper (SPI import +
  SD/SdFat `src`), 3 Kinetis by the one-line `pdb.h` guard, 1 (`effect_reverb`) by
  the arm_math.h include.
- **Second wave (2 findings):** `effect_reverb` arm_math.h correction (Audio node)
  + the CMSIS-DSP `.o`-mislink in the gate CMakeLists (evkb, not a node). No header
  collisions — Pattern C never needed.
- **Fix locus tally:** helper (evkb.cmake) = SPI + SD/SdFat src · Audio nodes =
  `utility/pdb.h` guard + `effect_reverb.cpp` arm_math.h · gate CMakeLists =
  CMSIS-DSP link line. **~5 edits total — SMALL, as estimated.**
- **ELF size:** the discovery stub (banner only) was ~38 KB; the SHIPPED gate
  with the real sine→FIR→mixer→FFT256→peak chain is text 14312 · data 24600 ·
  bss 31616 (the data bump is the CMSIS q15 FFT twiddle tables) — bounded;
  `--gc-sections` dropped every uninstantiated node, no empty-node const-data drag.
