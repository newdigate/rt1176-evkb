# Next session: clean-room rewrite of `EEPROM.h`

> ## ✅ DONE — 2026-07-29. This brief has been executed; do not action it again.
>
> Everything below is written in the present tense as an open task, and is now
> **historical**. The rewrite shipped as `newdigate/EEPROM` (MIT, fresh history),
> `evkb.cmake` was repointed off `PaulStoffregen/EEPROM`, `EEPROM` was added to
> the audit's Part-1 `REPOS`, and `./tools/license-audit.sh` exits 0 with nothing
> added to `ALLOW` or `GATES_EXEMPT`.
>
> What actually happened, and the decisions taken, are recorded in:
> - `docs/superpowers/specs/2026-07-29-eeprom-cleanroom-design.md`
> - `docs/superpowers/plans/2026-07-29-eeprom-cleanroom.md`
> - `examples/storage-memory/eeprom_test/transcript_qemu.txt` and
>   `transcript_hw_evkb.txt`
>
> **Two figures below are wrong and were corrected during execution.** The brief
> says the full sweep is `66 passed, 1 failed`; it is 68 gates (measured
> 2026-07-29 — see `CLAUDE.md`). And it says EEPROM is the only manifest entry
> still pointing at an upstream; `Bounce2` and the two CMSIS repos are as well.
>
> **Nothing left open.** The hardware persistence proof went through two unsound
> attempts before landing. A console-only test cannot work (the board boots before
> a console can attach, so a lost marker is silently recreated), and neither can a
> raw-flash byte comparison (the marker was deterministic, so re-creation is
> byte-identical). It is now settled by a `boots` counter in the marker: read out
> of the NOR array over SWD, it went 1 -> 2 across a physical power cycle, and a
> boot finding blank flash writes 1. See `transcript_hw_evkb.txt`.

---

## The task

`tools/license-audit.sh` fails. `~/Development/EEPROM/EEPROM.h` is
**LGPL-2.1-or-later** and is compiled into two firmware images:

```
COPYLEFT FILE COMPILED into examples/storage-memory/eeprom_test: ~/Development/EEPROM/EEPROM.h
COPYLEFT FILE COMPILED into examples/usb/usb_host_hid_test:      ~/Development/EEPROM/EEPROM.h
```

This contradicts the tree's stated position — README: *"every LGPL file inherited
from upstream was replaced with a clean-room rewrite, and
`tools/license-audit.sh` enforces this as a gate"*. This file is the one that got
missed. It survived because of a double blind spot, both now closed: `EEPROM` was
absent from the audit's Part-1 `REPOS` list, and neither example was in its
Part-2 `GATES` list.

**Brainstorm the design first, then write a spec and a plan.** Do not start
editing code. Use the `superpowers:brainstorming` skill.

## §0 — The one rule that makes this a clean-room rewrite

**Do NOT open, read, `grep`, `cat`, or otherwise inspect the contents of
`~/Development/EEPROM/EEPROM.h`.** Not to "check the API", not to "see how it
handles `put`". If you read it, the rewrite is derived from LGPL expression and
the entire exercise is void — you would be laundering the file, not replacing it.

You do not need it. Everything below was established without reading its
implementation, and the two sources you *will* work from are both clean:

1. **The C backend API** — read `cores/imxrt1176/avr_functions.h` and
   `cores/imxrt1176/eeprom.c` freely. They are **MIT** (Teensyduino Core Library,
   © 2019 PJRC.COM, LLC) and already audited-clean. This is your implementation
   substrate. (`cores/` is a nested checkout present in the main tree; in a git
   worktree without it, the same files land at
   `<example>/build/_deps/cores-src/imxrt1176/`.)
2. **The Arduino EEPROM API** — publicly documented at
   <https://docs.arduino.cc/learn/built-in-libraries/eeprom>. An interface
   required for source compatibility, specified independently of any one
   implementation.

That is a defensible provenance chain: public interface + MIT backend. Keep it.

If you believe you need something from the tainted header that is not in this
document, say so and stop — do not go and look.

## What is actually in scope (good news: it is small)

The taint is **only the thin C++ wrapper**. The hard part — flash-emulated EEPROM
on the RT1176 — already exists and is MIT.

- `~/Development/EEPROM/EEPROM.cpp` is a stub whose entire content is the comment
  `// this file no longer used`. The library is one header.
- `cores/imxrt1176/eeprom.c` (MIT, PJRC) implements the whole storage backend and
  exports, via `avr_functions.h`:
  - `eeprom_initialize()`, `eeprom_is_ready()`
  - `eeprom_read_byte/_word/_dword/_block`
  - `eeprom_write_byte/_word/_dword/_block`
  - `eepromemu_flash_write()`, `eepromemu_flash_erase_sector()`
  - linker symbol `_eeprom_region_start` reserves the flash region
- So you are writing a header that maps an Arduino-shaped C++ class onto that C
  API. Verified by `nm` on the built ELF: those symbols come from
  `cores-src/imxrt1176/eeprom.c`, not from the LGPL header.

## Required API surface (union of every in-tree consumer)

Established by grepping the *consumers*, never the tainted header:

| Member | Used by |
|---|---|
| `read(addr)` | `eeprom_test`, USBHost_t36 `bluetooth.cpp` |
| `write(addr, val)` | `eeprom_test`, USBHost_t36 `bluetooth.cpp` |
| `length()` | `eeprom_test`, USBHost_t36 `bluetooth.cpp` |
| `get(addr, T&)` | `eeprom_test` |
| `put(addr, const T&)` | `eeprom_test` |

`usb_host_hid_test` does not use EEPROM directly — it inherits the dependency
because USBHost_t36's `bluetooth.cpp` does `#include <EEPROM.h>`
(`examples/usb/usb_host_hid_test/CMakeLists.txt:42` adds the include dir for
exactly this reason).

**A real design question for the brainstorm, not a decided matter:** whether to
implement only these five, or the fuller documented Arduino surface
(`update()`, `operator[]`/`EERef`, `EEPtr`, iterators). Arguments exist both
ways — minimum surface is less to get wrong and every line is justified by a
caller; fuller surface means third-party Teensy sketches drop in unmodified,
which is this repo's whole premise. Weigh it, decide, and record why.

## Where the replacement should live

Also a genuine open question. Options seen in this tree:

- A `newdigate/EEPROM` fork, pinned in `evkb.cmake:70` (currently points at
  `PaulStoffregen/EEPROM` @ `9790da76`). This matches how other libraries were
  handled — CLAUDE.md notes several subsystems were "deliberately moved out of
  the core into `newdigate/<lib>` forks".
- Into `cores/imxrt1176/` beside `eeprom.c`, dropping the separate library.
  Tempting since the backend is already there, but it breaks the Teensy
  core-vs-library convention the repo follows deliberately.

Note the local-first resolution rule: a `~/Development/EEPROM` checkout wins over
the pin, so whatever you do must work in both modes, and `-DEVKB_FORCE_FETCH=ON`
must give a clean tree. **The existing `~/Development/EEPROM` working copy is
LGPL — decide explicitly what happens to it, because leaving it in place means
local-first silently keeps compiling the tainted file.**

**TRAP — renaming the local checkout out of the way is not enough, and on its own
makes nothing better.** `evkb.cmake:70` still reads:

```
_evkb_lib(EEPROM  ${_dev}/EEPROM  https://github.com/PaulStoffregen/EEPROM  9790da76... .)
```

Local-first means that if `~/Development/EEPROM` disappears, resolution *falls
back to that URL* and re-fetches the LGPL header from GitHub. The pin itself must
move to the replacement repo. Note EEPROM is the only library in that list still
pointing at an upstream rather than a `newdigate/<lib>` fork — every other entry
(Wire, SPI, Audio, SdFat, SD, Ethernet, lwip, USBHost_t36, …) was already forked.
That is the established pattern and the obvious home for this.

## Precedent to follow — read these first

The tree has done this before, with a written method:

- `docs/superpowers/specs/2026-07-13-rt1176-license-cleanroom-design.md`
- `docs/superpowers/plans/2026-07-13-rt1176-license-cleanroom.md`

Follow that method rather than inventing one. Also read `CLAUDE.md` (build,
two-gate rule, "silicon wins") and the licence section of `README.md`.

## Definition of done

1. `./tools/license-audit.sh` exits 0 with **no** entry added to its `ALLOW` list
   and **neither example** added to `GATES_EXEMPT`. Suppressing the finding is
   not a fix — the repo treats a check that quietly does not run as a defect.
2. `EEPROM` added to the audit's Part-1 `REPOS` list, so the header sweep covers
   it from now on. Its absence is half of why this was missed.
3. `./tools/license-audit.test.sh` still passes (17 cases) — the negative tests
   must keep firing.
4. `examples/storage-memory/eeprom_test/run_qemu_eeprom.sh` passes, run as
   `./run_qemu_eeprom.sh`, never `sh run_qemu_eeprom.sh`.
5. `examples/usb/usb_host_hid_test/run_qemu_usbhost.sh` still passes — it is the
   transitive consumer and the one most likely to break on a narrowed API.
6. Full sweep still `66 passed, 1 failed, 0 SKIP` (the one failure being the
   documented `dualcore/cm4_audio_test`). **Read `docs/KNOWN-BROKEN-GATES.md`
   before running `./tools/run-all-qemu-gates.sh`.** Check with `ps` that no
   other `run_qemu*.sh` or `qemu-system-arm` is running first — gates in one
   directory share a UART capture file.
7. Hardware verification on the real EVKB. The QEMU model's flash behaviour is
   not the silicon's, and this is persistence code: a wrong sector erase or a
   write that does not survive a power cycle is exactly the class of bug QEMU
   will not show you. Two-gate rule applies.
8. A LICENSE/README in the new library recording provenance: written against the
   public Arduino API and the MIT `avr_functions.h` backend, without reference to
   the LGPL original.

## Things worth deciding in the brainstorm

- Minimum vs. full Arduino API surface (above).
- `get`/`put` are templates. What are the type constraints, and what happens on a
  type larger than the EEPROM region or an out-of-range address? The AVR original
  is silent/UB in places; you may choose to be stricter, but say so, because a
  sketch that relied on the old behaviour changes meaning.
- `length()` on this part: the backend bounds every access with `E2END`
  (`eeprom.c:101`, `:124` return early / `0xFF` past it), and `eeprom.c:31` says
  *"To configure the EEPROM size, edit E2END in avr/eeprom.h"*, with a build-time
  guard at `:61`. So `length()` must agree with `E2END` rather than hardcode a
  number — work out where `E2END` resolves from for this part and derive it.
- Is `update()` (write-only-if-changed) worth having? On flash-emulated EEPROM it
  is not a micro-optimisation — it is wear reduction, and the backend may already
  do it. Check `eeprom.c` before deciding.
- Does the gate actually prove persistence, or just read-back within one run?
  Persistence across a reset is the property that matters and the one most likely
  to be untested. If the gate does not cover it, that is a gate to strengthen —
  in the same commit, not "later".

## Provenance of this document

Written from: the audit output; `nm`/`addr2line` on the built ELF; `eeprom.c` and
`avr_functions.h` (MIT); the consumers' call sites; and `EEPROM.h`'s **licence
header only**, read solely to identify the licence. Its implementation was not
read and is not described here.
