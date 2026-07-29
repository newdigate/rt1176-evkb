# EEPROM clean-room — design

**Date:** 2026-07-29
**Goal:** Remove the last copyleft file compiled into RT1176 firmware. Replace
the LGPL-2.1-or-later `EEPROM.h` with a clean-room MIT header in a new
`newdigate/EEPROM` repo, close the two audit blind spots that hid it, and
strengthen the EEPROM gate so it proves the cold-start path rather than only
read-back within one run.

Precedent and method: `docs/superpowers/specs/2026-07-13-rt1176-license-cleanroom-design.md`
and `docs/superpowers/plans/2026-07-13-rt1176-license-cleanroom.md`. This is the
same exercise, one file later, under a stricter reading rule (§1).

## The defect

`tools/license-audit.sh` fails:

```
COPYLEFT FILE COMPILED into examples/storage-memory/eeprom_test: ~/Development/EEPROM/EEPROM.h
COPYLEFT FILE COMPILED into examples/usb/usb_host_hid_test:      ~/Development/EEPROM/EEPROM.h
```

This contradicts README's stated position — every inherited LGPL file was
replaced with a clean-room rewrite, enforced by the audit. This file was missed
because of a double blind spot, both since closed: `EEPROM` was absent from the
audit's Part-1 `REPOS` list, and neither example was in its Part-2 `GATES` list.
`usb_host_hid_test` does not use EEPROM directly — USBHost_t36's `bluetooth.cpp`
does `#include <EEPROM.h>`, and `examples/usb/usb_host_hid_test/CMakeLists.txt:42`
adds the include directory for exactly that reason.

## Verified facts (2026-07-29 exploration)

Established without reading the tainted header. Anything not listed here was not
established, and the plan must not assume it.

- **Backend is MIT and complete.** `cores/imxrt1176/eeprom.c` (© 2019 PJRC.COM,
  LLC) implements log-structured, byte-granular flash emulation across 63
  sectors in the top 256K of FlexSPI1 NOR (`FLASH_BASEADDR 0x30FC0000`). It
  exports, via `cores/imxrt1176/avr_functions.h` (MIT):
  `eeprom_initialize`, `eeprom_is_ready`, `eeprom_read_byte/_word/_dword/_block`,
  `eeprom_write_byte/_word/_dword/_block`, `eepromemu_flash_write`,
  `eepromemu_flash_erase_sector`.
- **The backend already skips no-op writes** — `eeprom.c:137`,
  `if (data == olddata) return;`. So `update()` and `write()` have identical
  wear behaviour on this part, and `put()` is inherently wear-friendly because
  `eeprom_write_block` goes byte-by-byte through that same check.
- **The backend already bounds-checks every access.** `eeprom_read_byte` returns
  `0xFF` for `addr > E2END` (`eeprom.c:101`); `eeprom_write_byte` returns early
  (`eeprom.c:124`). Out-of-range is silent and never memory-unsafe — rejected,
  not clamped.
- **`E2END` resolves in `cores/imxrt1176/avr/eeprom.h:33` to `0x10BB`** (4283),
  with a build-time guard at `eeprom.c:61`. So `length()` is `E2END + 1` = 4284,
  which is the number the existing gate already asserts.
- **Consumer surface** (harvested from callers, never from the header):
  | Member | Used by |
  |---|---|
  | `read(addr)` | `eeprom_test`, USBHost_t36 `bluetooth.cpp` |
  | `write(addr, val)` | `eeprom_test`, USBHost_t36 `bluetooth.cpp` |
  | `length()` | `eeprom_test`, USBHost_t36 `bluetooth.cpp` |
  | `get(addr, T&)` | `eeprom_test` |
  | `put(addr, const T&)` | `eeprom_test` |
- **`length()` feeds signed arithmetic.** `bluetooth.cpp:2104`:
  `pairing_keys_eeprom_start_index_ = EEPROM.length() - (pairing_keys_max_ * 22 - pairing_keys_eeprom_start_index_);`
  where both operands are `int` and `pairing_keys_eeprom_start_index_` is `-1`
  (`USBHost_t36.h:2391`). Address call sites pass `uint32_t`
  (`bluetooth.cpp:2112`) and `uint16_t` (`bluetooth.cpp:2123`).
- **The build macro forbids a header-only library.** `import_arduino_library`
  globs `*.cpp`/`*.c`/`*.S` and calls `teensy_add_library(${LIB_NAME} ...)`
  (`teensy-cmake-macros/CMakeLists.include.txt:289-355`). With no sources that
  configures to `add_library(EEPROM)` with none, which fails. An `EEPROM.cpp`
  must exist.
- **The local checkout is pristine upstream** — `~/Development/EEPROM` is at
  `PaulStoffregen/EEPROM` `9790da76`, no local commits, clean status. Nothing of
  ours is lost by replacing it.
- **`newdigate/EEPROM` does not exist yet** — `git ls-remote` prompts for
  credentials. It has to be created.
- **`EEPROM` is the only *Arduino-layout* manifest entry still pointing at an
  upstream** (`evkb.cmake:70`). Wire, SPI, PXP, ILI9341_t3, MipiDisplay,
  TouchPanel, Audio, SdFat, SD, SerialFlash, Ethernet, NativeEthernet, FNET,
  lwip, USBHost_t36 and LVGL are all `newdigate/<lib>`. The task brief claims
  EEPROM is the only upstream entry outright; that is **not** accurate —
  `Bounce2` is also `PaulStoffregen/`, and `CMSIS-DSP`/`CMSIS-Core` are
  `ARM-software/`. Neither is implicated here (see Out of scope), but the
  "established pattern" argument rests on the Arduino-layout libraries, not on
  the whole manifest.
- **QEMU's FlexSPI model is real, not a stub.** `~/Development/qemu2/hw/ssi/imxrt_flexspi.c`
  implements page program as bitwise-AND and sector/32K/64K/chip erase to `0xFF`,
  so the existing wear stage exercises genuine erase/reprogram. Its `realize()`
  comment notes a `-drive ...,if=mtd` image "survives program/erase" — but **no
  gate in this tree boots that way**; the only `-drive` uses are USB mass storage
  (`usb_msc_block_test`, `usb_msc_fs_test`). The EEPROM gate uses `-kernel` with
  no backing store, so nothing it writes can survive the process.
- **`docs.arduino.cc/learn/built-in-libraries/eeprom` could not be retrieved** in
  this session — the page is JS-rendered and returned empty on two fetches. The
  API surface below is therefore specified from the public Arduino API and the
  consumer harvest above, not from a cited page.

## Decisions (user-confirmed)

1. **Home: a fresh `newdigate/EEPROM` repo** — `git init`, *not* a GitHub fork.
   A fork would carry the LGPL history, leaving the tainted blob in every clone.
2. **API surface: the full upstream surface** — `read`, `write`, `update`,
   `get`, `put`, `length`, `operator[]`, plus `EEPtr` and `begin()`/`end()` so
   range-for iteration works. Rationale: drop-in compatibility for third-party
   Teensy sketches is this repo's premise.
3. **Strictness: compile-time guards only.** `static_assert` on trivial
   copyability and size; runtime out-of-range keeps today's backend rejection, so
   no existing sketch changes meaning.
4. **Persistence: re-init stage in QEMU + a real power-cycle on hardware.** Not
   an mtd-backed two-boot gate — that would need full-flash-image (FCB + IVT +
   app) build tooling that does not exist here, which is a larger scope increase
   than the rewrite itself.
5. **Repo contents: ours only.** Nothing copied from the LGPL package.

## Out of scope

- An mtd-backed two-boot QEMU gate and the flash-image tooling it needs
  (decision 4). If persistence-in-QEMU is ever wanted, that is its own spec.
- `Bounce2`, `CMSIS-DSP` and `CMSIS-Core` — the other non-`newdigate` manifest
  entries. None is implicated: none is copyleft and none appears in the audit's
  findings. Repointing them is a separate question about vendoring policy, not
  about licence compliance.
- `cores/teensy*` reference copies — uncompiled, already allowlisted.

## §1 — The reading rule

**`~/Development/EEPROM/EEPROM.h` is not opened, read, `grep`ed, `cat`ed, or
inspected by anyone at any point.** Not to check the API, not to check a
signature. Reading it makes the rewrite derived from LGPL expression and voids
the exercise.

This is stricter than the 2026-07-13 precedent, which allowed a spec-author agent
to read the original and pass a distilled contract to an implementer. Here nobody
reads it. The same applies to the rest of the LGPL package: its `README.md`,
`keywords.txt`, and its ten `examples/*.ino` sketches are all off limits.

The permitted sources are:

1. **The MIT backend** — `cores/imxrt1176/avr_functions.h`, `cores/imxrt1176/eeprom.c`,
   `cores/imxrt1176/avr/eeprom.h`. Read freely. (In a git worktree without the
   nested `cores/` checkout these land at `<example>/build/_deps/cores-src/imxrt1176/`.)
2. **The public Arduino EEPROM API**, as a required interface for source
   compatibility.
3. **The in-tree consumer call sites**, harvested above.
4. **Standard C++ proxy-iterator design**, for `EERef`/`EEPtr` — a byte-addressed
   container's iterator shape is determined by the language, not by any one
   implementation.

If the implementation needs something from the tainted header that is not in this
spec, **stop and say so** — do not go and look.

### Oracle protocol (gate-first, black box)

The precedent's oracle protocol still applies, with the oracle being the running
binary rather than the source:

1. Extend `eeprom_test.cpp` with the new stages **first**.
2. Build and run the extended gate against the **existing LGPL header**. It must
   pass. That pins observable behaviour without reading a line of it.
3. Swap in the clean-room header under the same filenames.
4. Re-run. Output must be identical.

A new stage that fails to **compile** against the LGPL header is a legitimate
black-box signal that the upstream surface differs from what this spec assumes.
Adjust from the compiler diagnostic; do not look at the file.

## Architecture

### The header

`EEPROM.h` declares three types and one object:

- **`EERef`** — an assignable proxy for one byte. Holds an `int index`. Converts
  to `uint8_t` on read; `operator=` writes. Carries `update()`, the compound
  assignment operators, pre/post increment and decrement, and `operator&`
  returning an `EEPtr`.
- **`EEPtr`** — an iterator over the address space. Holds an `int index`.
  Converts to `int`, dereferences to `EERef`, compares, and increments/decrements.
- **`EEPROMClass`** — `read`, `write`, `update`, `get`, `put`, `length`,
  `operator[]`, `begin()`, `end()`.
- **`extern EEPROMClass EEPROM;`**, defined once in `EEPROM.cpp`.

Every operation bottoms out in the MIT C backend: `eeprom_read_byte` /
`eeprom_write_byte` for single bytes, `eeprom_read_block` / `eeprom_write_block`
for `get`/`put`. The header adds no storage logic of its own — it is a mapping
layer, which is precisely why the taint was only ever in the wrapper.

Design decisions, each with its reason:

- **`length()` returns `uint16_t`, computed as `E2END + 1`.** Never a hardcoded
  4284 — `avr/eeprom.h:33` is the single source of truth and `eeprom.c:61`
  guards it. `uint16_t` promotes to `int` in `bluetooth.cpp:2104`, keeping that
  expression's arithmetic signed exactly as it is today; a `size_t` return would
  silently make it unsigned.
- **Addresses are `int`.** Both consumers' `uint32_t` and `uint16_t` call sites
  convert cleanly. A negative index becomes a large `uint32_t` in the backend,
  exceeds `E2END`, and is rejected there (reads yield `0xFF`, writes are
  dropped) — silent, but not undefined. Nothing is clamped to the last valid
  address.
- **`static_assert` uses the GCC builtin `__is_trivially_copyable(T)`, not
  `<type_traits>`.** `EEPROM.h` is included by `bluetooth.cpp` and every
  consumer TU; pulling a libstdc++ header into all of them for one trait is the
  wrong cost. Second assert: `sizeof(T) <= E2END + 1`.
- **`update()` is kept but documented as behaviourally identical to `write()`
  on this part**, because `eeprom.c:137` already elides unchanged bytes. Recorded
  so nobody later "optimises" it on the assumption that it saves a write.
- **`EEPROM.cpp` holds the single `EEPROMClass EEPROM;` definition.** The file
  has to exist for the build macro regardless; one extern definition is a better
  use of it than a stub comment, and it avoids a per-TU object.

### Repo layout

`newdigate/EEPROM`, fresh history:

```
EEPROM.h            clean-room header (the deliverable)
EEPROM.cpp          the one EEPROM object definition
LICENSE             MIT
README.md           provenance: what this is, what it replaced, why
library.properties  ours
keywords.txt        ours
```

No examples. **The trap this avoids:** upstream's ten example sketches carry no
licence header, so they would pass the copyleft sweep silently while still being
LGPL-package files — a green Part 1 would prove nothing about them. Not carrying
them over is the only way the sweep result means what it says.

Provenance header on `EEPROM.h` and `EEPROM.cpp` in the exact style of
`cores/imxrt1176/WCharacter.h`: SPDX MIT, and a paragraph naming the public
Arduino API and the MIT `avr_functions.h` backend as the sources.

**Sweep trap:** `EEPROM` joins Part-1 `REPOS`, so our own files are swept. The
`COPYLEFT` regex matches the spelled-out phrase; write **"LGPL"**, never "GNU
Lesser General Public License", in `.h`/`.cpp`. `WCharacter.h` already threads
this needle. (`README.md` and `LICENSE*` are excluded from the sweep, so they
may spell it out.)

### Wiring changes in evkb

- `evkb.cmake:70` — repoint from `https://github.com/PaulStoffregen/EEPROM`
  `9790da76…` to `https://github.com/newdigate/EEPROM` at the new SHA. **This is
  load-bearing on its own:** local-first means that if `~/Development/EEPROM`
  merely disappeared, resolution would fall back to that URL and re-fetch the
  LGPL header from GitHub. Renaming the checkout fixes nothing by itself.
- `tools/license-audit.sh` — add `$HOME/Development/EEPROM` to `REPOS`. Its
  absence is half of why this was missed.
- `~/Development/EEPROM` — replaced by a clone of the new repo. The old checkout
  is moved aside until every gate is green, then deleted; it is pristine upstream
  and re-clonable from `PaulStoffregen/EEPROM` `9790da76` if ever needed.

Ordering constraint: the new repo must be **pushed** before `-DEVKB_FORCE_FETCH=ON`
can prove the fresh-user path, and the pin must be updated before that test means
anything.

Nothing changes in either example's `CMakeLists.txt` — `import_evkb_library(EEPROM)`
and the `usb_host_hid_test` include-dir line resolve through the manifest.

### Gate strengthening

Three stages added to `examples/storage-memory/eeprom_test/eeprom_test.cpp`,
three greps added to `run_qemu_eeprom.sh`. Existing stages (`EEPROM_RW`,
`EEPROM_WEAR`, `EEPROM_LENGTH`, `EEPROM_ALL`) are unchanged.

- **`EEPROM_API`** — exercises `update()`, `operator[]` in both directions, and
  range-for iteration, so the newly added surface is covered by a gate rather
  than merely compiled. Without this, decision 2 buys an untested API.
- **`EEPROM_PERSIST`** — calls `eeprom_initialize()` again after the writes, then
  re-reads. That is exactly the cold-start path (rescan the sectors, rebuild
  `sector_index[]`, `eeprom.c:77-92`) that makes persistence work, and it is
  testable inside one run with no new infrastructure.
- **`EEPROM_BOOT`** — a magic marker plus payload at address 4200 (clear of stage
  RW's 0–255, `put()`'s 1000–1015, and wear's 42/43; the marker is two
  `uint32_t`s, so 4200–4207 is well inside E2END = 4283). Marker absent → write
  it, print `EEPROM_BOOT=FIRST`. Marker present → verify the retained payload,
  print `EEPROM_BOOT=RETURN`.

  QEMU has no backing store, so it is deterministically `FIRST` and the gate
  asserts exactly that. On the EVKB, a power-cycle turns it into `RETURN`, and
  that is the un-fakeable persistence proof. The stage runs **first**, before the
  other stages write anything.

### Hardware verification

Two-gate rule; this is persistence code, where the QEMU model's flash behaviour
is furthest from silicon.

1. Flash `eeprom_test`, run, capture `transcript_hw_evkb.txt` showing
   `EEPROM_BOOT=FIRST` and the other stages passing.
2. **Pull power**, reconnect, run **without reflashing**, capture
   `EEPROM_BOOT=RETURN` with the payload intact.

Per CLAUDE.md: do not hold the VCOM while programming — `flash … load` →
`flash … verify` (both VCOM-free) → attach the reader → reset.

## Error handling / risk

- **USBHost_t36 is the likeliest breakage.** It is the transitive consumer and
  never appears in the EEPROM gate's own output. Its `uint32_t`/`uint16_t`
  address call sites and the signed `length()` arithmetic must compile without
  new warnings. Mitigation: `usb_host_hid_test` is rebuilt and its gate re-run
  as an explicit step, not as an afterthought.
- **A new stage may not compile against the LGPL header.** That is a signal, not
  a failure — see the oracle protocol. It must not become a reason to look.
- **Rebuild scope.** Filenames are unchanged, and the globs are
  `CONFIGURE_DEPENDS`, so no `rm -rf build` should be needed — but the library
  path's contents change wholesale, so budget a clean reconfigure of both
  examples if anything looks stale.
- **`license-audit.test.sh` (17 cases)** must still pass. It overrides `REPOS`
  via `LICENSE_AUDIT_REPOS`, so adding an entry to the default should not touch
  it — verified by running it, not assumed.
- **Fresh-user mode is a distinct failure surface.** `-DEVKB_FORCE_FETCH=ON`
  exercises the pin, not the local clone; a correct local tree with a stale pin
  still ships the LGPL header to every new user. Tested explicitly.
- **Full-sweep baseline is disputed.** `CLAUDE.md` states the expected result is
  `28 passed, 1 failed, 0 SKIP`; the task brief states `66 passed, 1 failed`; the
  audit's `GATES` list enumerates 67 gate-owning examples. `CLAUDE.md` is
  stale. Resolve by running the sweep and correcting `CLAUDE.md` in the same
  commit — do not silently adopt either number. Check with `ps` that no other
  `run_qemu*.sh` or `qemu-system-arm` is running first; gates in one directory
  share a UART capture file. Read `docs/KNOWN-BROKEN-GATES.md` first;
  `dualcore/cm4_audio_test` is the one documented failure and is not to be
  chased, weakened, or dropped.

## Done criteria

1. `./tools/license-audit.sh` exits 0 with **no** entry added to `ALLOW` and
   **neither example** added to `GATES_EXEMPT`. Suppressing the finding is not a
   fix.
2. `EEPROM` present in the audit's Part-1 `REPOS` list.
3. `./tools/license-audit.test.sh` still passes (17 cases).
4. `examples/storage-memory/eeprom_test/run_qemu_eeprom.sh` passes, run as
   `./run_qemu_eeprom.sh`, including the three new assertions.
5. `examples/usb/usb_host_hid_test/run_qemu_usbhost.sh` still passes.
6. Both examples configure and build with `-DEVKB_FORCE_FETCH=ON`, resolving
   `EEPROM` from `newdigate/EEPROM` at the new pin.
7. Full QEMU sweep at its true baseline, with `CLAUDE.md` corrected to match.
8. Hardware: `EEPROM_BOOT=FIRST` then, after a power-cycle with no reflash,
   `EEPROM_BOOT=RETURN` — recorded in `transcript_hw_evkb.txt`.
9. `newdigate/EEPROM` pushed, carrying LICENSE + README that record the
   provenance chain: written against the public Arduino API and the MIT
   `avr_functions.h` backend, without reference to the LGPL original.
10. `~/Development/EEPROM` is a clone of the new repo; the LGPL checkout is gone.
