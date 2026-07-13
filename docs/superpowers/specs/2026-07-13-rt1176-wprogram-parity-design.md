# RT1176 WProgram.h include-parity — design

**Date:** 2026-07-13
**Status:** approved by user (brainstorm session)
**Sub-project:** first of the §3 core-surface parity gaps (outstanding-work doc)

## Goal

A stock Teensy 4 sketch or library that compiles against `cores/teensy4` compiles
unmodified against `cores/imxrt1176`, as far as the include surface of
`Arduino.h`/`WProgram.h` is concerned. After this change,
`diff cores/teensy4/WProgram.h cores/imxrt1176/WProgram.h` is a one-glance parity
audit — that diffability is itself a deliverable (the previous curated trim is how
`IntervalTimer.h` went missing and broke the NativeEthernet port in milestone 4).

## Non-goals / out of scope

- Real CrashReport (fault-handler capture, breadcrumb RAM) — sub-project 4 of §3.
- Any non-CDC USB descriptor configuration (Keyboard/Mouse/MIDI functionality) —
  queued USB-device-HID milestone.
- MTP implementation (`MTP_Storage.{h,cpp}`, `MTP_Teensy.cpp`).
- `Serial2`+ additional LPUARTs — next §3 sub-project.
- `usb_undef.h` leak-prevention line: stays commented out exactly as on teensy4.

## Approach (chosen: full teensy4 mirror)

Rewrite `cores/imxrt1176/WProgram.h` to match teensy4's include list and ordering
**verbatim**, porting each missing header. Rejected alternatives: incremental
curated adds (permanent structural divergence, re-derives the parity mapping every
time); compatibility shim header (stock code includes `Arduino.h`, not a shim —
fixes nothing).

Key ordering consequence: `HardwareSerial.h` moves before the `__cplusplus` guard
and the usb-header block sits in teensy4's position; our current defensive
tail-ordering of `WString.h`/`Printable.h`/`Print.h`/`Stream.h`/`elapsedMillis.h`
is deleted — those arrive transitively exactly as on teensy4.

## File changes

### Ported verbatim from `cores/teensy4/`

| File | Notes |
|---|---|
| `WCharacter.h` | Platform-independent, straight copy |
| `inplace_function.h` | SG14 header-only, straight copy (used by teensy4 `IntervalTimer.h`) |
| `avr/interrupt.h` | One line, straight copy |
| `WMath.cpp` | Fixes latent link error: `random()`/`randomSeed()`/`makeWord()` are declared in our WProgram.h but implemented nowhere |
| `usb_seremu.h`, `usb_keyboard.h`, `usb_mouse.h`, `usb_joystick.h`, `usb_midi.h`, `usb_rawhid.h`, `usb_flightsim.h`, `usb_audio.h`, `usb_touch.h` | All self-gate on `*_INTERFACE` defines from `usb_desc.h`; under our CDC-only descriptors they compile to nothing. `usb_keyboard.h`'s pre-gate includes (`usb_desc.h`, `keylayouts.h`) already exist in the 1176 core |
| `MTP_Teensy.h` | Gates on `MTP_INTERFACE` (line 31); its heavy includes (`MTP_Storage.h`, `usb_dev.h`, …) sit *inside* the gate, so the header ports alone. `MTP_Storage.{h,cpp}` stay unported |
| `pulseIn` implementation | ~60 lines from teensy4 `digital.c:275-350` (`pulseIn_high`, `pulseIn_low`, `pulseIn`) into our `digital.c`. Declared today, unimplemented — same latent-link-error class as WMath |

### New: CrashReport stub

- `CrashReport.h`: teensy4 API surface — class with `printTo(Print &)`,
  `operator bool()`, `clear()`, breadcrumb methods; global `CrashReport` object.
- `CrashReport.cpp` (stub, NOT the teensy4 one): `operator bool()` returns
  `false`; `printTo` prints one line
  (`CrashReport: not yet supported on IMXRT1176`); `clear()`/breadcrumbs no-op.
- Honest behavior: "no crash report available" is a legal Teensy state (clean
  boot), so `if (CrashReport) Serial.print(CrashReport);` behaves correctly and
  simply never fires. `// TODO(crashreport milestone)` marks the real
  implementation as §3 sub-project 4's job.

### Modified

- `WProgram.h` — full rewrite to teensy4 mirror (see Approach).
- `digital.c` — gains `pulseIn`.
- No build-system change: core sources are `GLOB_RECURSE`'d
  (`teensy-cmake-macros/CMakeLists.include.txt:193`), so `WMath.cpp` /
  `CrashReport.cpp` are picked up automatically (clean-reconfigure caveat: globs
  evaluate at configure time — gate runs do a fresh configure, so no issue).
- `usb_desc.h` — only if a gated header touches an `*_INTERFACE`-adjacent macro
  before its gate; add the missing inert define (see Risks).

## Risks & fallbacks

1. **Include-order breakage.** teensy4's order is the origin of our defensive
   reordering. If a cycle or missing-decl error appears, fix the offending
   *header* (add the missing include/guard there) so WProgram.h stays identical
   to teensy4. A documented reorder in WProgram.h is the last resort.
2. **usb_desc.h divergence.** Our trimmed `usb_desc.h` may lack macros the gated
   headers reference before their gates. Fix by adding the missing inert defines
   to `usb_desc.h`, keeping the ported headers byte-identical.
3. **License:** everything ported is PJRC MIT-style (same license header already
   throughout the tree); `inplace_function.h` is SG14 (Boost-ish permissive) —
   verify its embedded header on port. No copyleft.

## Verification

1. **QEMU parity gate** `wprogram_parity_test/` (standard gate-lib harness):
   sketch includes **only** `<Arduino.h>` and exercises at runtime:
   - String + WCharacter ops (`isAlpha`, `toUpperCase`, …)
   - `randomSeed()` → `random()` bounds; `makeWord()`
   - `elapsedMillis` ticks
   - `IntervalTimer` declared/started **without** an explicit include — the
     NativeEthernet-regression test
   - CrashReport stub: `(bool)CrashReport == false`; `Serial1.print(CrashReport)`
     emits the stub line
   - `pulseIn`: link-level + call returns 0 on a quiet pin (timeout path) in QEMU
   - Negative compile check: with CDC-only descriptors, `usb_keyboard_class` etc.
     are absent (a `#ifdef KEYBOARD_INTERFACE` probe in the sketch proves the
     gates hold)
2. **Stock-library compile smoke:** build unmodified **Bounce2** and **Metro**
   against the core inside the gate sketch (compile + trivial runtime touch).
3. **HW flash:** same gate ELF on the EVKB via LinkServer; expect output identical
   to QEMU, plus a real `pulseIn` measurement — `tone()` on one header pin
   jumpered to another, `pulseIn` reads a plausible half-period.

Success = all three green; done-done per project discipline (QEMU gate green AND
HW-verified).

## Notes for the implementation plan

- Port order: leaf headers first (`WCharacter.h`, `inplace_function.h`,
  `avr/interrupt.h`), then `WMath.cpp` + `pulseIn`, then usb_* headers +
  `MTP_Teensy.h`, then CrashReport stub, then the WProgram.h rewrite last (it is
  the integration point), then gate.
- `evkb` repo holds gate + docs; `cores` repo holds the core changes — separate
  commits per established convention (check `status -sb` in both; working tree is
  shared across sessions).
