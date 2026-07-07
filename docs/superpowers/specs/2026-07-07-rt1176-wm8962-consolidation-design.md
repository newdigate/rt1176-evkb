# RT1176 WM8962 codec-driver consolidation — Design

**Status:** approved (design), ready for implementation plan
**Date:** 2026-07-07

## Goal

Move the `WM8962Codec` low-level I2C driver out of the **core** (`cores/imxrt1176/wm8962.{h,cpp}`, teensy-cores) and consolidate it into the Audio fork's **single** `control_wm8962.{h,cpp}` (`~/Development/Audio`, newdigate/Audio), alongside the `AudioControlWM8962` node it already backs. Result: the Audio repo owns its codec control end-to-end (no dependency on a core `wm8962.h`), matching Teensy's boundary (codec control — `control_wm8960`/`control_sgtl5000` — lives in the Audio **library**, not the core), and the deferred ADDCTL3 de-duplication falls out naturally.

## Why this is well-bounded (exploration findings)

- `WM8962Codec` depends only on `Wire.h` (`TwoWire`) — a core library that legitimately stays a core→library dependency (exactly as Teensy's `control_wm8960` uses `Wire`). No IMXRT register defs, no SDK coupling.
- **Nothing in the core references `WM8962Codec`** (verified: no core `.cpp`/`.h` outside `wm8962.*` uses it). Removing it does not break the core. The core's `I2SClass` is independent of the codec.
- The core's **only** audio files are `AudioStream.{h,cpp}` and `wm8962.{h,cpp}`. `AudioStream` is a **core** file in Teensy (the graph framework, not a node) and **stays**. `wm8962` is the single piece of audio-*library* material misplaced in the core. **So nothing else moves — the scope is exactly `WM8962Codec`.**
- Consumers of the codec:
  - `audioinput_i2s_test`, `audiooutput_i2s_test` (evkb gates) — use only `AudioControlWM8962` (the node). **Unaffected.**
  - `control_wm8962` (fork) — already holds a `WM8962Codec Codec` member and `#include "wm8962.h"`; `enable()` hand-duplicates the ADDCTL3 4-byte I2C write because `WM8962Codec::writeReg` is `private`.
  - `i2s_audio_test` (evkb gate) — uses **`WM8962Codec` directly at 48 kHz** (`#include "wm8962.h"`, `Codec.begin(Wire2)`); a raw `I2SClass` test, not an audio-graph test, and it needs 48 kHz (not the node's 44.1 kHz).

## Design

### Consolidated `control_wm8962.{h,cpp}` (fork) — holds both classes

- **`WM8962Codec`** — the low-level I2C driver, moved verbatim from the core: `begin(TwoWire&, uint8_t addr = 0x1A)` (the HW-verified 48 kHz / 16-bit I2S-slave, HP-out sequence) and its register-write helpers. `writeReg(uint16_t reg, uint16_t val)` becomes **accessible to `AudioControlWM8962`** — make it `public` (simplest; a codec register-write accessor is a reasonable public API), which also lets any future node reuse it.
- **`AudioControlWM8962 : public AudioControl`** — unchanged interface; holds a `WM8962Codec Codec` member. `enable()` calls `Codec.begin(Wire2, 0x1A)` then **`Codec.writeReg(WM8962_ADDCTL3, WM8962_ADDCTL3_44100HZ)`** (0x1B ← 0x0000) for 44.1 kHz — replacing the hand-rolled `Wire2.beginTransmission/write×4/endTransmission`. Byte-identical on the wire; just routed through the now-accessible accessor.
- **Header self-contained:** `#include "Wire.h"` + `"AudioControl.h"` (+ `<stdint.h>`). No `#include "wm8962.h"` (it ceases to exist). `WM8962Codec` declared before `AudioControlWM8962` in the header.
- **`control_wm8962.cpp`** contains both implementations (the moved `WM8962Codec` methods + the `AudioControlWM8962` methods). One `.cpp`, one `.h`.

### Core (teensy-cores) — delete `wm8962.{h,cpp}`

Remove both files. Nothing in the core uses them. The core keeps `AudioStream.{h,cpp}` (unchanged).

### evkb gates

- **`audioinput_i2s_test` / `audiooutput_i2s_test`** — **no firmware change** (`AudioControlWM8962 wm; wm.enable();`). Their CMake compiles the fork's `control_wm8962.cpp` (now self-contained); confirm no CMake/source line references the core `wm8962` (the core glob simply no longer contains it).
- **`i2s_audio_test`** — replace `#include "wm8962.h"` + `Codec.begin(Wire2)` with an **inline `static` 48 kHz codec init** in `i2s_audio_test.cpp`, copying the register writes from the current `WM8962Codec::begin()` (the HW-verified full sequence — a *reduced* subset risks silence per [[rt1176-i2s-sai]]). No Audio-fork dependency; the raw-`I2SClass` test stays at the core level. (Deliberate, user-chosen small duplication in exchange for test independence.)

## Data flow / behavior

Unchanged. `AudioControlWM8962::enable()` performs the same I2C writes (via `Codec.begin` + `Codec.writeReg`) it does today; `i2s_audio_test` performs the same 48 kHz codec init it does today (now inline). No register bytes change anywhere. This is a pure relocation + accessor-visibility change + a de-dup, with no functional delta.

## Testing

QEMU gates (TDD): rebuild and run all three affected gates — they must stay green.
- `audioinput_i2s_test` — `STAGE_PEAK` (injector → mic-channel peak) still passes.
- `audiooutput_i2s_test` — `STAGE_SYNTH` + `STAGE_TONE` (tap peak 16383) still pass.
- `i2s_audio_test` — its existing I2S-TX tap assertion still passes (the inline codec init is harmless in QEMU, where the WM8962 on LPI2C5 is unmodeled).

HW (final arbiter, low-risk since no register bytes change): optional spot-check — flash `audiooutput_i2s_test`, confirm the ~1 kHz tone still on J101 (exercises the consolidated `AudioControlWM8962` + the de-duped `writeReg` path). `i2s_audio_test`'s audible-on-J101 behavior is already independently covered by `audiooutput_i2s_test`.

## Risks

- **Silence from a trimmed `i2s_audio_test` init** — mitigated by copying the *full* HW-verified `begin()` sequence, not a reduced one.
- **`writeReg` made public** — a deliberate API widening (a codec register-write accessor); acceptable and useful.
- **Nothing else** — the change is a relocation with byte-identical I2C behavior, so the QEMU gates are a strong regression check.

## References

- Core (source): `cores/imxrt1176/wm8962.{h,cpp}` (`WM8962Codec`), `AudioStream.{h,cpp}` (stays).
- Fork (target): `~/Development/Audio/control_wm8962.{h,cpp}` (consolidation target), `AudioControl.h`.
- Consumers: `evkb/{audioinput_i2s_test,audiooutput_i2s_test,i2s_audio_test}/`.
- [[rt1176-i2s-sai]] (WM8962 init: partial subset is silent — the full sequence is required), [[rt1176-audioinput-i2s]], [[rt1176-audiooutput-i2s]] (both use `AudioControlWM8962`), [[rt1176-lpi2c-wire]] (LPI2C5 = Wire2, the codec bus).
