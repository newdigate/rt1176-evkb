# audio_h_test — full Audio-library compile sweep (Task 2 inventory → Task 3 fixes)

First-ever whole-fork compile of `newdigate/Audio` for RT1176, driven by
`import_evkb_audio_full()` (globs all 87 `.cpp` + 6 `.c` + `utility/`). This is
the honest error inventory produced by Task 2; Task 3 works it to zero.

- **Configure:** GREEN (helper wires CMSIS-DSP + Wire/SdFat/SD/SerialFlash + the
  whole Audio glob; two harmless "Empty library" warnings for the header-only
  SdFat/SD targets).
- **Build:** RED, as expected. `cmake --build build -- -k` (keep-going) attempted
  every TU. **11 of 82 Audio/SerialFlash translation units failed.** The other 71
  Audio TUs + all `cores` + all `CMSIS-DSP` compiled clean.
- **Every failure is a `fatal error: <header>: No such file or directory`** — a
  missing include. There are **zero** deeper semantic/type/collision errors in
  this pass. See the "Latent errors" caveat below: a fatal include error aborts
  the TU before its body is parsed, so body-level errors (and the 81-header
  aggregation collisions inside `#include <Audio.h>`) are structurally invisible
  until Task 3 resolves these includes and re-runs.

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

- [ ] `effect_reverb.cpp:math_helper.h` — Stale/external include — **[node]** STRIP. `math_helper.h` is a CMSIS-DSP *Examples* utility (SNR/error helpers); `effect_reverb.cpp` references **no** math_helper symbol (grep = the include line only). Remove the `#include "math_helper.h"` with the NOTE convention. Pure leftover; nothing else needed.
- [ ] `play_sd_wav.cpp:SD.h` (via `play_sd_wav.h:32 #include <SD.h>`) — Stale/external include — **[helper]** SD is a manifest lib but its header is `PaulS_SD/src/SD.h`; the helper imported SD with no subdir so only the repo root is on `teensy_flags`. Fix in helper: `import_evkb_library(SD src)` (and almost certainly `import_evkb_library(SdFat src)` too — `SD.h` chains to `SdFat/src/SdFat.h`). Genuine SD user (SD works on RT1176 — see `sd_wav_play_test`), so wire it in rather than guard.
- [ ] `play_sd_raw.cpp:SD.h` (via `play_sd_raw.h:32`) — Stale/external include — **[helper]** identical to play_sd_wav; same `SD src` / `SdFat src` helper fix resolves both.
- [ ] `SerialFlashChip.cpp:SPI.h` (via `SerialFlash.h:32 #include <SPI.h>`) — Stale/external include — **[helper]** SerialFlash genuinely uses SPI; SPI is a manifest lib (`SPI/SPI.h` at root) that the helper does **not** import. Fix in helper: `import_evkb_library(SPI)`. (SerialFlash is globbed as a full lib, so its 2 sources MUST compile.)
- [ ] `SerialFlashDirectory.cpp:SPI.h` (via `SerialFlash.h:32`) — Stale/external include — **[helper]** same `import_evkb_library(SPI)` fix.
- [ ] `play_serialflash_raw.cpp:SPI.h` (via `play_serialflash_raw.h:33 #include <SerialFlash.h>` → SPI.h) — Stale/external include — **[helper]** resolved by the same `import_evkb_library(SPI)`; genuine SerialFlash user.
- [ ] `spi_interrupt.cpp:SPI.h` (via `spi_interrupt.h:32 #include <SPI.h>`) — Stale/external include — **[helper|node]** `spi_interrupt.h` genuinely calls `SPI.usingInterrupt()/notUsingInterrupt()` (the Teensy audio-vs-SPI ISR-priority shim). Either `import_evkb_library(SPI)` (compiles against `newdigate/SPI`) OR guard it empty on RT1176 if the audio graph never shares the SPI bus. Task-3 decision — see "Other".
- [ ] `effect_delay_ext.cpp:SPI.h` (via `effect_delay_ext.h:31 #include "spi_interrupt.h"` → SPI.h) — Stale/external include — **[helper|node]** external SPI-RAM delay line (23LC1024 / CY15B104) — an un-ported hardware-I/O node. `import_evkb_library(SPI)` makes it compile, but per the "compile empty, uninstantiable" doctrine for HW-I/O nodes it may instead want a Pattern-B guard. Task-3 decision — see "Other".

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

- [ ] `input_adcs.cpp:kinetis.h` (via `utility/pdb.h:30`) — Kinetis/1062-only outside guard — **[node]** widen `pdb.h` guard (shared fix).
- [ ] `output_dac.cpp:kinetis.h` (via `utility/pdb.h:30`) — Kinetis/1062-only outside guard — **[node]** widen `pdb.h` guard (shared fix).
- [ ] `output_dacs.cpp:kinetis.h` (via `utility/pdb.h:30`) — Kinetis/1062-only outside guard — **[node]** widen `pdb.h` guard (shared fix).

## Category: Header collision  (0 items — NOT YET OBSERVABLE)

None surfaced. This is expected and NOT proof of absence: every failing TU aborts
at its first missing include before the body/aggregation is parsed, and
`audio_h_test.cpp` — the TU that actually does `#include <Audio.h>` (81 node
headers in one translation unit) — was **never compiled** because the `Audio`
library target failed first. Redefinition/ambiguity collisions can only appear
in Task 3 once (a) the missing includes are resolved so bodies parse, and (b)
the library links so `audio_h_test.elf`'s own TU builds. Re-inventory then.

## Category: Other  (notes for Task 3 scoping)

- [ ] **Helper-vs-node decision for the 5 `SPI.h` TUs.** The plan's given macro
  body (used verbatim) imports Wire/SdFat/SD/SerialFlash but **not** SPI, and
  imports SD/SdFat **without** their `src` subdir. Per Task-3 Pattern A ("for a
  manifest lib the include resolves once the helper puts it on the path — fix the
  helper, not the node"), the natural fix for 7 of the 11 TUs (2×SD, 5×SPI) is a
  **helper tweak**, not an Audio-source edit: add `import_evkb_library(SPI)` and
  change `SD`/`SdFat` imports to include the `src` subdir. Task 2 deliberately
  left the helper at the plan's exact macro (configure is green and the full
  sweep ran), so the coordinator sees the complete inventory. **Recommendation:**
  Task 3 makes the helper tweak first (clears 7), then only guards the true
  un-ported HW-I/O node (`effect_delay_ext`, and optionally `spi_interrupt`) if a
  design reason prefers empty objects over linking `newdigate/SPI`.
- [ ] **Latent errors behind the includes.** All 11 failures are include-fatal, so
  no body was parsed. Resolving them may unmask a second wave (type refs,
  collisions). Task 3 MUST re-run `cmake --build build -- -k` after each batch.
- [ ] **Umbrella-header aggregation untested.** `#include <Audio.h>` (audio_h_test.cpp)
  has not compiled once. First green library build is the first real test of the
  81-header aggregation.

---

## Summary for Task 3 scoping

- **11 failing TUs, all trivial missing-include fatals.** No semantic errors yet.
- **Category counts:** Stale/external include 8 · Kinetis-outside-guard 3 · Header collision 0 (not observable) · Other = notes.
- **By fix locus:** ~7 TUs are a **2-line helper tweak** (import SPI; add `src` to SD/SdFat) · 3 Kinetis TUs are a **single one-line `pdb.h` guard** · 1 is a **one-line stale strip** (`effect_reverb`).
- **Estimated size: SMALL** — the known 11 clear in ~4 edits (2 helper lines, pdb.h guard, reverb strip). The only unknown that could grow it: latent body errors + header-aggregation collisions unmasked once includes resolve (re-inventory required).
