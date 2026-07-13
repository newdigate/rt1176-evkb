# RT1176 license clean-room — design

**Date:** 2026-07-13
**Goal:** No copyleft code of any kind compiled into RT1176 firmware, ever —
proven by a link-manifest audit, not by grep alone. Replace the remaining
LGPL-2.1 Arduino/Wiring-lineage files with genuine clean-room MIT
implementations; delete the GPL soft-SPI path from the SdFat fork; commit a
repeatable audit script.

## Verified facts (2026-07-13 exploration)

- LGPL-2.1 files compiled into firmware, confirmed by header inspection and a
  wrap-tolerant sweep over all ecosystem repos (cores, Ethernet,
  NativeEthernet, SdFat, SPI, Wire, Audio, SD, USBHost_t36, FNET, lwip):
  - `cores/imxrt1176/Printable.h` (42 lines)
  - `cores/imxrt1176/Stream.h` + `Stream.cpp` (76 + 298)
  - `cores/imxrt1176/WString.h` + `WString.cpp` (248 + 823)
  - `cores/imxrt1176/Time.cpp` (127)
  - `Ethernet/src/Client.h`, `Server.h`, `IPAddress.cpp` — and the same three
    in NativeEthernet. All four vendored files (incl. `IPAddress.h`) are
    currently **byte-identical** between the two repos; no drift to reconcile.
- `IPAddress.h` is already MIT (Adrian McEwen, MIT header) in both repos —
  only `IPAddress.cpp` needs rewriting.
- Already clean in `cores/imxrt1176`: `Print.{h,cpp}` (PJRC MIT),
  `WCharacter.h`, `WMath.cpp` (our 2026-07-13 clean-room files),
  `avr_functions.h`/`dtostrf`.
- `DateTimeFields` is declared in `cores/imxrt1176/core_pins.h:2981` (ours,
  clean) — `Time.cpp` builds against a clean struct.
- SdFat GPL: only `src/DigitalIO/` (3 files, GPLv3). `SdSpiSoftDriver.h` is
  MIT but is the **sole consumer** of DigitalIO, reachable via
  `SPI_DRIVER_SELECT == 2` (`SdSpiDriver.h:147`, default is 0 in
  `SdFatConfig.h:125`).
- New sweep finding beyond the brief: `Audio/extras/miditones/` is GPL — a
  host-side PC tool, never compiled into firmware. Already documented as an
  exception in `Audio/LICENSE.md:8`. **Decision: delete it** (user choice).
  `examples/Synthesis/PlaySynthMusic/william_tell_overture.c` is miditones
  *output* (generated note data, no GPL header) and stays.
- `cores/teensy`, `cores/teensy3`, `cores/teensy4` are uncompiled LGPL
  reference copies in the cores repo — they stay (repo contents may be mixed;
  only the firmware image must be copyleft-free) and become documented grep
  exclusions in the audit script.
- Repo states: evkb has **no git remote** (local-only); `evkb/cores` is 1
  commit ahead of origin; Ethernet, NativeEthernet, SdFat are clean and in
  sync with their remotes.

## Decisions (user-confirmed)

1. **HW re-verification in this session** — board is connected. RTC + SD
   timestamp gates re-run on HW after the Time.cpp swap; Ethernet HW gate if
   time allows.
2. **Delete `Audio/extras/miditones/`** from the fork (not just document it);
   update `Audio/LICENSE.md` to drop the exception paragraph.
3. **QEMU gates only** for the String/Stream behavioral suites (no host-side
   unit harness) — same infra as every prior bring-up, real target semantics.
4. Overall scope approved as presented.

## Out of scope

- `SPI.h/.cpp`, `Wire.h/.cpp`, `utility/twi.*` — upstream platform branches,
  preprocessor-dead in RT1176 builds, documented in each repo's LICENSE.md.
  Do not rewrite other platforms' code.
- QEMU (GPLv2 host tool, never linked into firmware).
- The `cores/teensy*` reference directories (see above).

## Architecture

Every rewritten file keeps its **exact filename and location** (the
`file(GLOB)` trap: new filenames would require `rm -rf build` + reconfigure in
every test dir). The public API surface (class names, method signatures,
virtual set) is preserved as a fact of interface compatibility; comments,
bodies, and internal helper structure are authored fresh.

The three Ethernet-library files are authored **once** and copied
byte-identical into both repos; each commit message states this.

### Clean-room process (what makes it defensible)

Two-agent separation via subagent-driven-development, for each rewrite task:

- **Spec/test author agent** — may read anything, including the LGPL file. It
  produces (a) an API contract written from the documented Arduino API
  (docs.arduino.cc language reference) plus the bare declarations consumers
  depend on, and (b) the gate tests. The consumer-surface harvest (grep of
  core + all newdigate libs + all gate sketches for methods/operators
  actually called) is recorded in the implementation plan and defines the
  must-pass test set.
- **Implementer agent** — receives ONLY the spec + tests. Its prompt states:
  do not read `WString.cpp` / `WString.h` / `Stream.cpp` / `Stream.h` /
  `IPAddress.cpp` / `Time.cpp` / `Printable.h` / `Client.h` / `Server.h`
  in `cores/imxrt1176`, nor any copy under `cores/teensy`, `cores/teensy3`,
  `cores/teensy4`, nor the Ethernet/NativeEthernet vendored copies.
- Well-known algorithms used by name: Howard Hinnant's public-domain
  days-from-civil / civil-from-days for `Time.cpp`; standard grow-by-doubling
  buffer management for `String`.
- Every new file gets the MIT header + provenance paragraph in the exact
  style of `cores/imxrt1176/WCharacter.h` ("Clean-room MIT implementation …
  written from the documented Arduino API surface, not derived from the
  LGPL …").

### Oracle protocol (gate-first)

For each task: write the gate tests first, run them against the **old** LGPL
implementation (the behavioral oracle, used as a black box) and watch them
pass; swap in the clean-room file under the same filename; re-run. Assert
against independently computed values, not old-String output text — do not
enshrine quirks, but investigate any divergence before accepting it.

## Task order

1. **Warm-up: `Printable.h` + `Client.h` + `Server.h` + `IPAddress.cpp`** —
   trivial interfaces; establishes the pattern and provenance-header style.
   IPAddress traps: implements `Printable`; `operator uint32_t` is
   network-order-in-memory (dotted string prints in storage order). Oracle:
   existing Ethernet QEMU gates (DNS + HTTP GET catch byte-order mistakes).
2. **`Time.cpp`** — `breakTime`/`makeTime` against the `DateTimeFields`
   layout in `core_pins.h` (epoch Unix 1970; year offset per that struct's
   convention). Oracle: `evkb/rtc_test` QEMU gate; HW re-verify at the end.
3. **`Stream.{h,cpp}`** — new QEMU gate `evkb/stream_test`: a RAM-backed
   Stream stub exercising every parse/read/timeout path. Traps:
   `parseInt`/`parseFloat` skip non-numeric lead-in, honor `setTimeout`
   (default 1000 ms), return 0 on timeout; `readBytesUntil` consumes but does
   not store the terminator; timeout uses `millis()`. The virtual method set
   is enumerated **from the subclasses** (HardwareSerial, usb_serial,
   EthernetClient, File), not from the old header — keep exactly
   `available()/read()/peek()` pure-virtual plus whatever extras the
   subclasses actually override. After the swap: re-run serial + Ethernet +
   SD gates (the real consumers).
4. **`WString.{h,cpp}`** — the big one. New QEMU gate `evkb/string_test` with
   an exhaustive String-API sketch built from the harvested-surface list.
   Invariants: invalid/OOM String has `c_str() != NULL` returning `""`;
   `operator+` chains; `F()`/`__FlashStringHelper*` overloads exist (flash ==
   RAM here); `toInt` on garbage returns 0; `substring` clamps; self-assign
   and `s += s` work; float formatting via `dtostrf`. After the swap: full
   ecosystem rebuild + full QEMU gate suite (String touches everything).
5. **SdFat surgery** — delete `src/DigitalIO/` and `SpiDriver/
   SdSpiSoftDriver.h`; make `SPI_DRIVER_SELECT == 2` a `#error` in
   `SdSpiDriver.h`. Delete `Audio/extras/miditones/` + update
   `Audio/LICENSE.md`. Re-run SD gates (`sd_wav_play_test` etc.).
6. **Audit gate: `evkb/tools/license-audit.sh`** (committed, CI-runnable):
   - Part 1: wrap-tolerant sweep
     (`grep -rIlz -E "GNU[[:space:]]+(General|Lesser)[[:space:]]+(General[[:space:]]+)?Public[[:space:]]+License"`)
     over every ecosystem repo, excluding `.git`, `*.img`, docs/LICENSE
     files, and a **documented exclusion list** (cores/teensy*,
     SPI/Wire platform branches). Exits non-zero on any undocumented hit.
     (Single-line grep variants miss wrapped headers — that mistake already
     happened once.)
   - Part 2: link-manifest verification — build `sd_wav_play_test` (links
     cores+SPI+Wire+Audio+SdFat+SD), enumerate every object in the link from
     the `.map` file / `ninja -t deps`, map objects back to sources, and
     check each contributing source's header is permissive. Exits non-zero
     otherwise.
7. **Wrap-up** — LICENSE.md in the cores repo documenting fully-MIT
   `imxrt1176` (and the uncompiled LGPL reference dirs); HW re-verify (RTC +
   SD timestamps, Ethernet if time allows); update the
   `prefer-permissive-licenses` memory note; push cores, Ethernet,
   NativeEthernet, SdFat, Audio (evkb has no remote — local commit only).
   Push `evkb/cores` first so clean-room commits sit on a synced base.

## Error handling / risk

- **Vtable breakage (Stream/Print):** mitigated by enumerating virtuals from
  subclasses and re-running every gate that links a Stream subclass.
- **String OOM paths:** the gate includes forced-failure cases (huge reserve)
  to pin the invalid-String invariants.
- **Time.cpp HW risk:** the only swap with HW-visible behavior (RTC, SD
  timestamps) — explicitly re-verified on the board.
- **Full-rebuild trap:** same filenames everywhere; still budget a full
  `rm -rf build` sweep across test dirs after the String/Stream swaps since
  headers changed content.
- **Two repos, one source:** byte-identical copies enforced by `diff` in the
  audit script (cheap, permanent).

## Done criteria

- Zero GPL/LGPL-headed files compiled into any RT1176 firmware image, proven
  by `license-audit.sh` part 2 (link manifest), with part 1 (sweep) also
  green against its documented exclusion list.
- All ecosystem QEMU gates green, including new `string_test` and
  `stream_test`; RTC + SD timestamp behavior re-verified on HW.
- Provenance headers in every rewritten file; cores LICENSE.md updated;
  Ethernet/NativeEthernet copies byte-identical; SdFat DigitalIO +
  SdSpiSoftDriver and Audio miditones deleted; memory note updated; all
  remote-backed repos pushed.
