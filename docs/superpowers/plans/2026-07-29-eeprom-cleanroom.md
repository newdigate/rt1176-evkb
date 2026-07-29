# EEPROM Clean-Room Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the LGPL-2.1-or-later `EEPROM.h` — the last copyleft file compiled into RT1176 firmware — with a clean-room MIT header in a new `newdigate/EEPROM` repo, close the two audit blind spots that hid it, and strengthen the EEPROM gate to cover the cold-start path.

**Architecture:** The storage backend already exists and is MIT (`cores/imxrt1176/eeprom.c`, log-structured flash emulation over 63 FlexSPI NOR sectors). The taint is only the thin C++ wrapper, so the deliverable is a header mapping an Arduino-shaped class (`EERef` byte proxy, `EEPtr` iterator, `EEPROMClass`) onto the C API declared in `avr_functions.h`. The behavioural oracle is the *running binary*, never the source: extend the gate first, prove it green against the LGPL build, then swap the header and prove the output identical.

**Tech Stack:** C++ (ARM GCC 10 at `/Applications/ARM_10/bin`), CMake + teensy-cmake-macros, QEMU `mimxrt1170-evk` machine, LinkServer for hardware.

**Spec:** `docs/superpowers/specs/2026-07-29-eeprom-cleanroom-design.md`

---

## §1 — THE READING RULE (read this before touching anything)

**Never open, read, `grep`, `cat`, `less`, `head`, `tail`, or otherwise inspect
`EEPROM.h` from the LGPL package.** Not to check a signature, not to check the
API, not "just to see how `put` works". Reading it makes this rewrite derived
from LGPL expression and voids the entire exercise.

This ban covers the whole upstream package: its `README.md`, `keywords.txt`, and
its ten `examples/*.ino` sketches. In Task 3 that package is moved to the
scratchpad specifically so that a careless `grep -r ~/Development` cannot reach
it — **do not grep the scratchpad copy either.**

Permitted sources, read freely:
- `cores/imxrt1176/avr_functions.h`, `cores/imxrt1176/eeprom.c`, `cores/imxrt1176/avr/eeprom.h` (all MIT)
- the in-tree consumer call sites (`examples/storage-memory/eeprom_test/eeprom_test.cpp`, `~/Development/USBHost_t36/bluetooth.cpp`)
- this plan and its spec

**If you need something that is not in this plan, stop and say so. Do not go and look.**

---

## File Structure

**New repo `newdigate/EEPROM`** (created at `~/Development/EEPROM` in Task 3):

| File | Responsibility |
|---|---|
| `EEPROM.h` | The whole clean-room API: `EERef`, `EEPtr`, `EEPROMClass`. Header-only logic; all storage delegated to the MIT C backend. |
| `EEPROM.cpp` | The single `EEPROMClass EEPROM;` definition. Must exist — the build macro globs `*.cpp` and `add_library` with no sources fails to configure. |
| `LICENSE` | MIT. |
| `README.md` | Provenance record: what this is, what it replaced, why it is not derived. |
| `library.properties` | Arduino library metadata. |
| `keywords.txt` | Arduino IDE syntax highlighting. |

**Modified in `evkb`:**

| File | Change |
|---|---|
| `examples/storage-memory/eeprom_test/eeprom_test.cpp` | Three new stages: `EEPROM_BOOT`, `EEPROM_API`, `EEPROM_PERSIST`. |
| `examples/storage-memory/eeprom_test/run_qemu_eeprom.sh` | Poll for the terminal token instead of `sleep 4`; three new assertions. |
| `evkb.cmake` (line 70) | Repoint the `EEPROM` pin from `PaulStoffregen` to `newdigate` at the new SHA. |
| `tools/license-audit.sh` | Add `$HOME/Development/EEPROM` to `REPOS`. |
| `CLAUDE.md` | Correct the stale full-sweep baseline. |
| `examples/storage-memory/eeprom_test/transcript_qemu.txt` | New — QEMU evidence. |
| `examples/storage-memory/eeprom_test/transcript_hw_evkb.txt` | New — hardware evidence including the power-cycle. |

---

## Task 1: Baseline — pin what is true before anything changes

No files change. This task exists because every later "it passes now" claim is
meaningless without a recorded "it failed / passed before".

**Files:** none (read-only verification)

- [ ] **Step 1: Confirm no stray QEMU is running**

Gates in one directory share a UART capture file, so a leftover process
corrupts results.

```bash
ps aux | grep -E 'qemu-system-arm|run_qemu' | grep -v grep
```

Expected: no output. If anything is listed, kill it before continuing.

- [ ] **Step 2: Build both affected examples**

```bash
cd examples/storage-memory/eeprom_test && cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake && cmake --build build
```

```bash
cd examples/usb/usb_host_hid_test && cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake && cmake --build build
```

Expected: both produce `build/<name>.elf`.

- [ ] **Step 3: Confirm the audit fails, and fails for exactly the stated reason**

```bash
LICENSE_AUDIT_PARTS=2 LICENSE_AUDIT_GATES="examples/storage-memory/eeprom_test:eeprom_test examples/usb/usb_host_hid_test:usb_host_hid_test" ./tools/license-audit.sh
```

Expected: two `COPYLEFT FILE COMPILED into …: …/EEPROM/EEPROM.h` lines, then
`LICENSE-AUDIT: FAIL`, exit 1.

(`LICENSE_AUDIT_PARTS=2` with an explicit `GATES` keeps this to the two examples
just built — the full Part 2 requires all 67 gate-owning examples built.)

- [ ] **Step 4: Confirm both gates pass today**

```bash
cd examples/storage-memory/eeprom_test && ./run_qemu_eeprom.sh
```

Expected: `PASS: EEPROM flash-emulation verified (RW + wear-leveling + length)`.

```bash
cd examples/usb/usb_host_hid_test && ./run_qemu_usbhost.sh
```

Expected: its own PASS line.

**Run as `./run_qemu_eeprom.sh`, never `sh run_qemu_eeprom.sh`** — the runner
re-execs itself under `gtimeout` via `tools/gate-lib.sh`.

- [ ] **Step 5: Find every consumer of the header**

```bash
grep -rn '#include *<EEPROM\.h>\|#include *"EEPROM\.h"' ~/Development --include=*.cpp --include=*.h --include=*.ino -l | grep -v '/EEPROM/'
```

Expected: `~/Development/USBHost_t36/bluetooth.cpp` and nothing else. If any
other consumer appears, **stop** — the API surface in Task 4 was harvested
against a two-consumer assumption and must be re-checked before proceeding.

---

## Task 2: Extend the gate — against the LGPL header (the oracle step)

This is the load-bearing step of the clean-room method. The new stages must pass
against the **existing** implementation first; only then does "identical output
after the swap" mean anything.

**Files:**
- Modify: `examples/storage-memory/eeprom_test/eeprom_test.cpp`
- Modify: `examples/storage-memory/eeprom_test/run_qemu_eeprom.sh`

- [ ] **Step 1: Rewrite `eeprom_test.cpp` with the three new stages**

Replace the whole file with:

```cpp
#include "Arduino.h"
#include "HardwareSerial.h"
#include <EEPROM.h>

struct Settings { uint32_t magic; int16_t a; int16_t b; char tag[6]; };

// Persistence marker. 4200 is clear of stage RW (0..255), put() (1000..1015)
// and the wear stage (42, 43); 4200 + sizeof(BootMark) = 4208, inside
// E2END (4283). See stage BOOT below for why it lives at a fixed address.
struct BootMark { uint32_t magic; uint32_t payload; };
static const int  BOOT_ADDR    = 4200;
static const uint32_t BOOT_MAGIC   = 0xB007EE99;
static const uint32_t BOOT_PAYLOAD = 0xCAFEF00D;

void setup() {
	Serial1.begin(115200);
	while (!Serial1) {}

	// --- Stage BOOT: persistence marker, read BEFORE anything else writes ---
	// QEMU boots from -kernel with no backing store behind the FlexSPI window,
	// so nothing written survives the process: this is deterministically FIRST
	// there, and the gate asserts exactly that. On the EVKB it reads RETURN
	// after a power-cycle with no reflash -- the only un-fakeable proof that
	// the data actually persisted. A QEMU-only test cannot produce RETURN, and
	// that asymmetry is the point, not a gap.
	BootMark bm;
	EEPROM.get(BOOT_ADDR, bm);
	if (bm.magic == BOOT_MAGIC) {
		Serial1.print("EEPROM_BOOT=RETURN payload=0x"); Serial1.println(bm.payload, HEX);
		Serial1.print("EEPROM_PERSIST_HW=");
		Serial1.println(bm.payload == BOOT_PAYLOAD ? "PASS" : "FAIL");
	} else {
		BootMark w = { BOOT_MAGIC, BOOT_PAYLOAD };
		EEPROM.put(BOOT_ADDR, w);
		Serial1.println("EEPROM_BOOT=FIRST");
	}

	// --- Stage RW: byte-level write/read across the address space + a struct ---
	bool rw_ok = true;
	for (int i = 0; i < 256; i++) {
		uint8_t v = (uint8_t)(i * 7 + 3);
		EEPROM.write(i, v);
	}
	for (int i = 0; i < 256; i++) {
		if (EEPROM.read(i) != (uint8_t)(i * 7 + 3)) { rw_ok = false; break; }
	}
	Settings s = { 0xEE9702AA, -1234, 4321, {'R','T','1','1','7','\0'} };
	EEPROM.put(1000, s);
	Settings r;
	EEPROM.get(1000, r);
	if (r.magic != s.magic || r.a != s.a || r.b != s.b || strcmp(r.tag, s.tag) != 0) rw_ok = false;
	Serial1.print("EEPROM_RW="); Serial1.println(rw_ok ? "PASS" : "FAIL");

	// --- Stage WEAR: hammer one address past a full sector to force compaction+erase ---
	// A sector holds 2048 two-byte entries; >2048 changing writes to the same
	// address forces eepromemu_flash_erase_sector + rewrite. Verify the value +
	// a neighbor (same sector) survive the compaction.
	const int A = 42, B = 43;
	EEPROM.write(B, 0x5A);
	bool wear_ok = true;
	for (int n = 0; n < 2100; n++) {
		EEPROM.write(A, (uint8_t)n);       // changes each time -> new entry each time
		if (EEPROM.read(A) != (uint8_t)n) { wear_ok = false; break; }
	}
	if (EEPROM.read(B) != 0x5A) wear_ok = false;   // neighbor survived the erase/compaction
	Serial1.print("EEPROM_WEAR="); Serial1.println(wear_ok ? "PASS" : "FAIL");

	// --- Stage API: the surface beyond read/write/get/put/length ---
	// update(), operator[] in both directions, proxy-to-proxy assignment, and
	// range-for iteration. Without this stage the wider API is compiled but
	// never executed, which is how an API regression reaches hardware.
	// Addresses 300..302 are outside every other stage's range.
	bool api_ok = true;
	EEPROM.update(300, 0x11);
	if (EEPROM.read(300) != 0x11) api_ok = false;
	EEPROM.update(300, 0x11);                       // the unchanged-value path
	if (EEPROM.read(300) != 0x11) api_ok = false;
	EEPROM[301] = 0x22;                             // proxy write
	if (EEPROM.read(301) != 0x22) api_ok = false;
	uint8_t via_proxy = EEPROM[301];                // proxy read
	if (via_proxy != 0x22) api_ok = false;
	EEPROM[302] = EEPROM[301];                      // must copy the VALUE, not rebind
	if (EEPROM.read(302) != 0x22) api_ok = false;
	uint32_t seen = 0;
	for (uint8_t b : EEPROM) { (void)b; seen++; }   // begin()/end()/EEPtr/EERef
	if (seen != EEPROM.length()) api_ok = false;
	Serial1.print("EEPROM_API="); Serial1.println(api_ok ? "PASS" : "FAIL");

	// --- Stage PERSIST: the cold-start path, without a reboot ---
	// eeprom_initialize() re-scans all 63 sectors and rebuilds sector_index[]
	// (cores/imxrt1176/eeprom.c:77) -- exactly what runs on a fresh boot against
	// flash that already holds data. Calling it again proves the rescan
	// reconstructs the same view of everything written above, including across
	// the sector the wear stage erased and compacted.
	// It does NOT prove data survives a power cycle; only the hardware run can,
	// via EEPROM_BOOT above. Do not let this stage's name suggest otherwise.
	bool persist_ok = true;
	eeprom_initialize();
	Settings r2;
	EEPROM.get(1000, r2);
	if (r2.magic != s.magic || r2.a != s.a || r2.b != s.b || strcmp(r2.tag, s.tag) != 0) persist_ok = false;
	if (EEPROM.read(A) != (uint8_t)2099) persist_ok = false;   // last wear value
	if (EEPROM.read(B) != 0x5A) persist_ok = false;            // its neighbour
	for (int i = 0; i < 256; i++) {
		if (i == A || i == B) continue;                        // rewritten by stage WEAR
		if (EEPROM.read(i) != (uint8_t)(i * 7 + 3)) { persist_ok = false; break; }
	}
	BootMark bm2;
	EEPROM.get(BOOT_ADDR, bm2);
	if (bm2.magic != BOOT_MAGIC || bm2.payload != BOOT_PAYLOAD) persist_ok = false;
	Serial1.print("EEPROM_PERSIST="); Serial1.println(persist_ok ? "PASS" : "FAIL");

	Serial1.print("EEPROM_LENGTH="); Serial1.println(EEPROM.length());   // expect 4284
	Serial1.print("EEPROM_ALL=");
	Serial1.println((rw_ok && wear_ok && api_ok && persist_ok) ? "PASS" : "FAIL");
}
void loop() {}
```

- [ ] **Step 2: Update the gate runner**

Replace `run_qemu_eeprom.sh` with:

```sh
#!/bin/sh
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
# Tools come from THIS checkout, derived from the gate's own location. The old
# hardcoded ~/Development/rt1170/evkb/tools/... meant a worktree or a clone at
# any other path silently loaded a DIFFERENT tree's gate-lib.sh -- which surfaces
# as "gate_reap: command not found", or worse, as a gate quietly running against
# the wrong library.
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/build/eeprom_test.elf"; OUT="$DIR/eeprom.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/eeprom.dbg" &
P=$!; gate_pid $P
# Poll for EEPROM_ALL -- the capture's last line, printed after every token
# asserted below -- instead of always burning a fixed window. Same 8 s CEILING
# (32 x 0.25 s) as the `sleep 4` this replaces, doubled because the sketch now
# does ~4.3k extra reads for the iteration stage and a full 63-sector rescan for
# the persist stage on top of the 2100 wear writes. A healthy run exits as soon
# as the token lands; a hung one is still killed here and still goes red on the
# missing token below, never on a silent timeout.
for _ in $(seq 1 32); do
    [ -f "$OUT" ] && grep -q "EEPROM_ALL=" "$OUT" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured ===="; cat "$OUT"
grep -q "EEPROM_RW=PASS"      "$OUT" || { echo "FAIL: RW"; exit 1; }
grep -q "EEPROM_WEAR=PASS"    "$OUT" || { echo "FAIL: wear-leveling"; exit 1; }
grep -q "EEPROM_API=PASS"     "$OUT" || { echo "FAIL: API surface (update/operator[]/iteration)"; exit 1; }
grep -q "EEPROM_PERSIST=PASS" "$OUT" || { echo "FAIL: cold-start rescan"; exit 1; }
# FIRST, not merely "present": QEMU has no backing store behind the FlexSPI
# window, so a RETURN here would mean the model had grown persistence and this
# assertion had stopped testing what it claims. The RETURN case is proven on
# hardware instead -- see transcript_hw_evkb.txt.
grep -q "EEPROM_BOOT=FIRST"   "$OUT" || { echo "FAIL: boot marker not FIRST (QEMU cannot persist)"; exit 1; }
# 4284 = EEPROM.length() = E2END(0x10BB=4283)+1 — proves the emulated region is sized right
grep -q "EEPROM_LENGTH=4284"  "$OUT" || { echo "FAIL: length"; exit 1; }
grep -q "EEPROM_ALL=PASS"     "$OUT" || { echo "FAIL: overall"; exit 1; }
echo "PASS: EEPROM flash-emulation verified (RW + wear + API + cold-start rescan + length)"
```

- [ ] **Step 3: Build against the still-LGPL header**

```bash
cd examples/storage-memory/eeprom_test && cmake --build build
```

Expected: builds clean.

**If it does NOT compile**, the failure is diagnostic, and which one matters:

| Symptom | Meaning | Action |
|---|---|---|
| `no member named 'update'` / `'begin'` / `'end'`, or no `operator[]` | The upstream surface is narrower than this plan assumes | Delete only the failing sub-check from stage API, add a one-line comment saying which member upstream lacks, and continue. Task 5 re-adds it once our header is in. |
| anything else | Unexpected | **Stop and report.** Do not open the header to investigate. |

- [ ] **Step 4: Run the gate — it must PASS against the LGPL implementation**

```bash
cd examples/storage-memory/eeprom_test && ./run_qemu_eeprom.sh
```

Expected: the new `PASS: EEPROM flash-emulation verified (RW + wear + API + cold-start rescan + length)` line, with `EEPROM_BOOT=FIRST` visible in the captured transcript.

This green is the oracle. If it fails here, the *gate* is wrong, not the
library — fix the gate before going near the rewrite.

One sub-check deserves naming, because it is the only one whose upstream
behaviour this plan is genuinely unsure of: `EEPROM[302] = EEPROM[301];`. If
`EEPROM_API` fails at this point, the likeliest cause is that the replaced
implementation's byte proxy rebinds on proxy-to-proxy assignment instead of
copying the value.

If that happens: comment the line and its check out of stage API with a note
saying the oracle could not run it, keep going, and re-enable it in Task 5 Step
3 — our header defines that operator explicitly and must copy the value. **Do
not "fix" it by making our header rebind instead.** Value-copy is the correct
semantic for a reference proxy, and matching a defect would be inheriting
behaviour we cannot see the source of.

- [ ] **Step 5: Commit**

```bash
git add examples/storage-memory/eeprom_test/eeprom_test.cpp examples/storage-memory/eeprom_test/run_qemu_eeprom.sh
git commit -m "eeprom_test: cover the API surface, the cold-start rescan, and boot persistence

Three stages added, and deliberately added BEFORE the clean-room header swap so
they run against the existing implementation first -- a differential test whose
baseline was never observed proves nothing.

EEPROM_API executes update(), operator[] both directions, proxy-to-proxy
assignment and range-for iteration, which were compiled but never run.
EEPROM_PERSIST re-runs eeprom_initialize() to exercise the 63-sector rescan that
rebuilds sector_index[] on a cold boot. EEPROM_BOOT writes a marker at 4200 and
reports FIRST/RETURN; QEMU has no backing store so it is always FIRST there, and
the gate asserts that exactly -- RETURN is what the hardware run proves.

The runner now polls for EEPROM_ALL instead of sleeping a fixed 4 s."
```

---

## Task 3: Stand up the new repo (local, empty of upstream)

**Files:**
- Move: `~/Development/EEPROM` → scratchpad (out of reach of `grep -r ~/Development`)
- Create: `~/Development/EEPROM/{LICENSE,README.md,library.properties,keywords.txt}`

- [ ] **Step 1: Move the LGPL package out of `~/Development` entirely**

Not renamed in place: a renamed sibling is still reachable by a careless
recursive grep over `~/Development`, and §1 forbids reading it. It is pristine
upstream (`9790da76`, no local commits), so it is re-clonable and nothing is
lost.

```bash
mv ~/Development/EEPROM /private/tmp/claude-501/-Users-nicholasnewdigate-Development-rt1170-evkb/431cc620-06cd-4a74-ae8e-0dc03b61f2c9/scratchpad/EEPROM-lgpl-upstream
```

**Do not read anything under that path for the rest of this plan.**

- [ ] **Step 2: Create the new repo**

```bash
mkdir -p ~/Development/EEPROM && git -C ~/Development/EEPROM init -q && git -C ~/Development/EEPROM branch -M master
```

- [ ] **Step 3: Write `~/Development/EEPROM/LICENSE`**

```
MIT License

Copyright (c) 2026 Nicholas Newdigate

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

- [ ] **Step 4: Write `~/Development/EEPROM/README.md`**

`README.md` is excluded from the audit's copyleft sweep (`--exclude='*.md'`), so
it may spell the licence name out in full. `.h`/`.cpp` may not — see Task 4.

```markdown
# EEPROM — Arduino EEPROM API for the i.MX RT1176

An MIT-licensed EEPROM library for the `imxrt1176` Arduino/Teensyduino-style
core, used by [newdigate/rt1176-evkb](https://github.com/newdigate/rt1176-evkb).

## What this is

A thin C++ wrapper presenting the standard Arduino `EEPROM` object — `read`,
`write`, `update`, `get`, `put`, `length`, `EEPROM[i]`, and `begin()`/`end()`
iteration — over the flash-emulation backend that ships in the core
(`cores/imxrt1176/eeprom.c`). On this part there is no EEPROM peripheral: the
backend emulates one in the top 256 KB of the 16 MB FlexSPI NOR, log-structured
across 63 sectors with wear levelling. Capacity is 4284 bytes, derived from
`E2END` in the core's `avr/eeprom.h`.

## Provenance

This library is a **clean-room MIT implementation**. It was written against:

1. the publicly documented Arduino EEPROM API, which it must match for source
   compatibility with existing sketches; and
2. the MIT-licensed C backend declared in the core's `avr_functions.h`
   (Teensyduino Core Library, © 2019 PJRC.COM, LLC).

It replaces the GNU Lesser General Public License v2.1 `EEPROM.h` previously
used by that project. **The replaced file was not read, quoted, or consulted at
any point during this library's authoring** — not its implementation, and not
the rest of its package (README, keywords, examples). Only its licence header
was ever examined, and only to identify the licence. This repository was
created with fresh history rather than as a fork, so no copy of the replaced
work is present anywhere in it.

The parent project's `tools/license-audit.sh` sweeps this repository and proves,
via a link-manifest walk of every firmware image, that no copyleft source is
compiled into any binary it ships.

## Behavioural notes specific to this part

- `update()` is provided for API compatibility, but it is **not** a wear
  optimisation here: the backend already elides a write whose value is
  unchanged, so `update(a, v)` and `write(a, v)` cost the same. For the same
  reason `put()` is inherently wear-friendly — it writes byte by byte through
  that same check.
- Out-of-range addresses are rejected by the backend, not by this wrapper: reads
  past the end return `0xFF`, writes past the end are dropped. Nothing is
  clamped to the last valid address. This matches the AVR original's observable
  behaviour.
- `get()`/`put()` are compile-time checked — the type must be trivially
  copyable and must be no larger than the device. Both are `static_assert`s, so
  they cost nothing at runtime and can only reject code that was already
  undefined behaviour or could never have fit.
- That size check is against the device capacity, **not** against the target
  address. `put(4280, something_16_bytes)` starts in range, runs off the end,
  and silently loses the overhanging bytes — a later `get()` reads `0xFF` for
  them. The address is a runtime value, so no compile-time check can catch it;
  budget your layout yourself.

## Licence

MIT — see `LICENSE`.
```

- [ ] **Step 5: Write `~/Development/EEPROM/library.properties`**

```
name=EEPROM
version=2.0.0
author=Nicholas Newdigate
maintainer=Nicholas Newdigate
sentence=Read and write bytes to the i.MX RT1176 flash-emulated EEPROM.
paragraph=Clean-room MIT implementation of the Arduino EEPROM API over the imxrt1176 core's flash-emulation backend.
category=Data Storage
url=https://github.com/newdigate/EEPROM
architectures=imxrt1176
```

- [ ] **Step 6: Write `~/Development/EEPROM/keywords.txt`**

Tab-separated, per the Arduino format.

```
EEPROM	KEYWORD1
EERef	KEYWORD1
EEPtr	KEYWORD1
read	KEYWORD2
write	KEYWORD2
update	KEYWORD2
get	KEYWORD2
put	KEYWORD2
length	KEYWORD2
begin	KEYWORD2
end	KEYWORD2
```

- [ ] **Step 7: Commit the skeleton**

```bash
cd ~/Development/EEPROM && git add -A && git commit -q -m "Initial commit: MIT licence, README, library metadata

Fresh history, deliberately not a fork: a fork would carry the replaced
library's history and leave a copy of it in every clone." && git log --oneline
```

---

## Task 4: Write the clean-room header

**Files:**
- Create: `~/Development/EEPROM/EEPROM.h`
- Create: `~/Development/EEPROM/EEPROM.cpp`

- [ ] **Step 1: Write `EEPROM.h`**

Note the provenance paragraph uses the abbreviation **"LGPL"** and never the
spelled-out phrase. This is not stylistic: the audit's `COPYLEFT` regex matches
`GNU…Lesser…Public License` across wrapped comment lines, and this repo is about
to join the swept `REPOS` list. `cores/imxrt1176/WCharacter.h` already threads
this same needle.

```cpp
/* EEPROM.h - Arduino EEPROM API for the i.MX RT1176.
 *
 * Clean-room MIT implementation: written from the documented Arduino EEPROM
 * API surface (an interface required for source compatibility) mapped onto the
 * MIT flash-emulation backend declared in <avr/eeprom.h> / <avr_functions.h>
 * (Teensyduino Core Library, (c) 2019 PJRC.COM, LLC). It is NOT derived from
 * the LGPL EEPROM.h it replaces, which was not read at any point.
 *
 * There is no EEPROM peripheral on this part. Every operation below delegates
 * to the core's emulation over the top 256K of FlexSPI NOR; this header adds
 * no storage logic of its own.
 *
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef _EEPROM_h_
#define _EEPROM_h_

#include <stdint.h>
#include <avr/eeprom.h>   /* MIT: E2END, and the eeprom_*() backend */

/* Addresses are plain int, matching every in-tree call site (which pass
 * uint32_t and uint16_t values that convert cleanly). A negative index becomes
 * a large uint32_t inside the backend, exceeds E2END, and is REJECTED there --
 * reads yield 0xFF, writes are dropped. Silent, but never undefined. Nothing
 * is clamped to the last valid address. */
typedef int eeprom_index_t;

/* The backend takes pointers whose VALUE is the EEPROM address, not a real
 * pointer into the address space (see eeprom.c, which immediately casts back
 * to uint32_t). These two helpers keep that unusual convention in one place
 * instead of scattering casts through every accessor.
 *
 * Plain inline, NOT static inline: these are odr-used from the implicitly
 * inline member functions below, which have external linkage. Internal linkage
 * here would make those definitions differ between translation units, which is
 * ill-formed-no-diagnostic-required. The names also avoid a leading underscore,
 * which is reserved in the global namespace. */
inline const uint8_t *eeprom_cptr(eeprom_index_t i) {
	return (const uint8_t *)(uintptr_t)i;
}
inline uint8_t *eeprom_ptr(eeprom_index_t i) {
	return (uint8_t *)(uintptr_t)i;
}

class EEPtr;

/* One EEPROM byte, as an assignable proxy. No byte of the emulated device is
 * addressable in RAM, so `EEPROM[i]` cannot return a uint8_t&; it returns this
 * instead, which reads on conversion and writes on assignment. */
class EERef {
public:
	explicit EERef(eeprom_index_t index) : index(index) {}

	operator uint8_t() const { return eeprom_read_byte(eeprom_cptr(index)); }

	EERef &operator=(uint8_t value) {
		eeprom_write_byte(eeprom_ptr(index), value);
		return *this;
	}

	/* Copy-assignment moves the VALUE, not the address: `EEPROM[0] = EEPROM[1]`
	 * must copy a byte. The compiler-generated version would rebind index and
	 * write nothing, which is why this is spelled out. */
	EERef &operator=(const EERef &ref) { return *this = (uint8_t)ref; }

	/* Identical to operator= on this part -- eeprom.c already returns early
	 * when the stored byte is unchanged. Provided for API compatibility; do
	 * NOT document it as saving a write here. */
	EERef &update(uint8_t value) { return *this = value; }

	EERef &operator+=(uint8_t v)  { return *this = (uint8_t)(*this + v); }
	EERef &operator-=(uint8_t v)  { return *this = (uint8_t)(*this - v); }
	EERef &operator*=(uint8_t v)  { return *this = (uint8_t)(*this * v); }
	EERef &operator/=(uint8_t v)  { return *this = (uint8_t)(*this / v); }
	EERef &operator^=(uint8_t v)  { return *this = (uint8_t)(*this ^ v); }
	EERef &operator%=(uint8_t v)  { return *this = (uint8_t)(*this % v); }
	EERef &operator&=(uint8_t v)  { return *this = (uint8_t)(*this & v); }
	EERef &operator|=(uint8_t v)  { return *this = (uint8_t)(*this | v); }
	EERef &operator<<=(uint8_t v) { return *this = (uint8_t)(*this << v); }
	EERef &operator>>=(uint8_t v) { return *this = (uint8_t)(*this >> v); }

	/* Prefix yields the proxy; postfix yields the OLD value as a plain
	 * uint8_t, because returning a copied proxy would alias the same byte. */
	EERef &operator++() { return *this += 1; }
	EERef &operator--() { return *this -= 1; }
	uint8_t operator++(int) { uint8_t v = *this; *this = (uint8_t)(v + 1); return v; }
	uint8_t operator--(int) { uint8_t v = *this; *this = (uint8_t)(v - 1); return v; }

	EEPtr operator&() const;   /* defined below, once EEPtr is complete */

	eeprom_index_t index;
};

/* An iterator over the address space. Dereferences to EERef. */
class EEPtr {
public:
	EEPtr(eeprom_index_t index) : index(index) {}

	operator int() const { return index; }
	EEPtr &operator=(eeprom_index_t in) { index = in; return *this; }

	EERef operator*() const { return EERef(index); }

	bool operator==(const EEPtr &p) const { return index == p.index; }
	bool operator!=(const EEPtr &p) const { return index != p.index; }

	EEPtr &operator++() { ++index; return *this; }
	EEPtr &operator--() { --index; return *this; }
	EEPtr operator++(int) { EEPtr t = *this; ++index; return t; }
	EEPtr operator--(int) { EEPtr t = *this; --index; return t; }

	eeprom_index_t index;
};

inline EEPtr EERef::operator&() const { return EEPtr(index); }

class EEPROMClass {
public:
	uint8_t read(eeprom_index_t idx) const { return eeprom_read_byte(eeprom_cptr(idx)); }
	void write(eeprom_index_t idx, uint8_t val) { eeprom_write_byte(eeprom_ptr(idx), val); }

	/* See EERef::update -- same story, same reason it is not an optimisation. */
	void update(eeprom_index_t idx, uint8_t val) { eeprom_write_byte(eeprom_ptr(idx), val); }

	EERef operator[](eeprom_index_t idx) { return EERef(idx); }

	/* E2END is the LAST valid address, so the size is E2END + 1. Derived, never
	 * hardcoded: avr/eeprom.h is the single source of truth and eeprom.c
	 * guards E2END against the sector count at build time.
	 *
	 * uint16_t and not size_t, matching the AVR convention. It promotes to int,
	 * which keeps an expression like `EEPROM.length() - x` signed. That matters
	 * wherever the difference is USED directly -- `EEPROM.length() - x > 0` is
	 * false when signed and true when unsigned, for x past the end.
	 *
	 * Note it does NOT matter at the one in-tree call site that computes such a
	 * difference (USBHost_t36 bluetooth.cpp), because that assigns straight to
	 * an int, which undoes the wrap either way. Measured, not assumed -- do not
	 * cite that call site as the reason for this return type. */
	uint16_t length() const { return (uint16_t)(E2END + 1); }

	EEPtr begin() const { return EEPtr(0); }
	EEPtr end() const { return EEPtr(E2END + 1); }

	/* __is_trivially_copyable is a GCC builtin, used instead of
	 * <type_traits>::is_trivially_copyable so that including <EEPROM.h> does
	 * not drag a libstdc++ header into every consumer translation unit for the
	 * sake of one trait. */
	template <typename T> T &get(eeprom_index_t idx, T &t) {
		static_assert(__is_trivially_copyable(T),
		              "EEPROM.get() requires a trivially copyable type");
		static_assert(sizeof(T) <= (unsigned)(E2END + 1),
		              "EEPROM.get(): type is larger than the EEPROM");
		eeprom_read_block((void *)&t, (const void *)eeprom_cptr(idx), (uint32_t)sizeof(T));
		return t;
	}

	template <typename T> const T &put(eeprom_index_t idx, const T &t) {
		static_assert(__is_trivially_copyable(T),
		              "EEPROM.put() requires a trivially copyable type");
		static_assert(sizeof(T) <= (unsigned)(E2END + 1),
		              "EEPROM.put(): type is larger than the EEPROM");
		/* Wear-friendly for free: eeprom_write_block goes byte by byte through
		 * eeprom_write_byte, which skips bytes that already hold the value. */
		eeprom_write_block((const void *)&t, (void *)eeprom_ptr(idx), (uint32_t)sizeof(T));
		return t;
	}
};

extern EEPROMClass EEPROM;

#endif
```

- [ ] **Step 2: Write `EEPROM.cpp`**

```cpp
/* EEPROM.cpp - the single EEPROM object.
 *
 * Clean-room MIT implementation; see EEPROM.h for the provenance note. Not
 * derived from the LGPL EEPROM library it replaces.
 *
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "EEPROM.h"

/* Defined here rather than as a per-translation-unit object in the header.
 * The class is stateless so either would work, but this file has to exist
 * regardless: teensy-cmake-macros' import_arduino_library() globs *.cpp and
 * calls teensy_add_library(), which fails to configure with no sources. */
EEPROMClass EEPROM;
```

- [ ] **Step 3: Verify the provenance headers cannot trip the audit's own regex**

```bash
cd ~/Development/EEPROM && grep -REizl '(GNU([[:space:]]|[*/#!-])+(General|Lesser))' EEPROM.h EEPROM.cpp; echo "matches above (none expected); exit=$?"
```

Expected: no filenames printed. A hit here means a provenance comment spelled
the licence name out — reword it to the abbreviation before continuing.

- [ ] **Step 4: Commit**

```bash
cd ~/Development/EEPROM && git add EEPROM.h EEPROM.cpp && git commit -q -m "Clean-room MIT EEPROM API over the imxrt1176 flash-emulation backend

EERef byte proxy, EEPtr iterator and EEPROMClass, mapping the Arduino EEPROM
interface onto the MIT eeprom_*() backend declared in the core's
avr_functions.h. No storage logic here -- the emulation already exists and is
MIT; only the wrapper needed replacing.

Written from the documented Arduino API and standard C++ proxy-iterator design.
The replaced LGPL header was not read." && git log --oneline
```

---

## Task 5: Swap it in — both gates must be identical to Task 2's baseline

Local-first resolution means `~/Development/EEPROM` already wins over the pin, so
the new library is live as soon as the examples are reconfigured. This is the
differential step: same gate, same assertions, new implementation.

**Files:** none in `evkb` (rebuild only)

- [ ] **Step 1: Reconfigure both examples from scratch**

The library's *path* is unchanged but its contents were replaced wholesale, so
do not trust the existing build directories.

```bash
cd examples/storage-memory/eeprom_test && rm -rf build && cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake && cmake --build build 2>&1 | tail -30
```

Expected: builds clean. Watch for **any** new warning mentioning `EEPROM` —
report it rather than ignoring it.

- [ ] **Step 2: Rebuild the transitive consumer**

`usb_host_hid_test` never appears in the EEPROM gate's output, and USBHost_t36's
`bluetooth.cpp` is the only other consumer, so this is the likeliest breakage in
the whole plan.

```bash
cd examples/usb/usb_host_hid_test && rm -rf build && cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake && cmake --build build 2>&1 | tail -30
```

Expected: builds clean, with no new warning from `bluetooth.cpp`.

The two call shapes to watch, both in `~/Development/USBHost_t36/bluetooth.cpp`:
`EEPROM.length() - (pairing_keys_max_ * 22 - pairing_keys_eeprom_start_index_)`
at line 2104 (must stay signed — `length()` returning `uint16_t` promotes to
`int`), and `EEPROM.write(i, 0xff)` at line 2112 where `i` is `uint32_t`.

- [ ] **Step 3: Re-add any stage sub-check dropped in Task 2 Step 3**

If Task 2 removed a sub-check because the LGPL header lacked that member, add it
back now — our header has the full surface — and delete the comment that said
upstream lacked it. If nothing was dropped, skip this step.

- [ ] **Step 4: Run the EEPROM gate — output must match Task 2**

```bash
cd examples/storage-memory/eeprom_test && ./run_qemu_eeprom.sh
```

Expected: identical token lines to Task 2 Step 4 — `EEPROM_BOOT=FIRST`,
`EEPROM_RW=PASS`, `EEPROM_WEAR=PASS`, `EEPROM_API=PASS`, `EEPROM_PERSIST=PASS`,
`EEPROM_LENGTH=4284`, `EEPROM_ALL=PASS`.

Any divergence is a real behavioural difference between the two
implementations. **Investigate it; do not accept it, and do not resolve it by
reading the replaced header.**

- [ ] **Step 5: Run the transitive consumer's gate**

```bash
cd examples/usb/usb_host_hid_test && ./run_qemu_usbhost.sh
```

Expected: its PASS line, unchanged from Task 1 Step 4.

- [ ] **Step 6: Save the QEMU transcript as evidence**

```bash
cd examples/storage-memory/eeprom_test && ./run_qemu_eeprom.sh > transcript_qemu.txt 2>&1 && tail -12 transcript_qemu.txt
```

- [ ] **Step 7: Commit the evidence**

```bash
git add examples/storage-memory/eeprom_test/transcript_qemu.txt
git commit -m "eeprom_test: QEMU transcript against the clean-room library

Same gate and same assertions as the run against the replaced implementation in
the previous commit -- the differential the clean-room method depends on."
```

---

## Task 6: Publish the repo and move the pin

**⚠ This task creates a PUBLIC GitHub repository and pushes to it.** Confirm
with the user before running Step 1 if they are not already expecting it.

**Files:**
- Modify: `evkb.cmake:70`

- [ ] **Step 1: Create and push `newdigate/EEPROM`**

```bash
cd ~/Development/EEPROM && gh repo create newdigate/EEPROM --public --source=. --remote=origin --description "Clean-room MIT Arduino EEPROM API for the i.MX RT1176" --push
```

Expected: repo created, `master` pushed.

- [ ] **Step 2: Capture the pushed SHA**

```bash
git -C ~/Development/EEPROM rev-parse HEAD
```

Record it — it is the new pin. Confirm it is on the remote:

```bash
git ls-remote https://github.com/newdigate/EEPROM master
```

Expected: the same SHA. (Before this repo existed, this command prompted for
credentials — that is what "not found" looks like here.)

- [ ] **Step 3: Repoint the pin in `evkb.cmake`**

This line is load-bearing on its own. Local-first means that if
`~/Development/EEPROM` merely vanished, resolution would fall back to the URL
below and re-fetch the replaced header from GitHub. Moving the checkout aside
fixes nothing without this edit.

Replace line 70:

```cmake
_evkb_lib(EEPROM         ${_dev}/EEPROM               https://github.com/PaulStoffregen/EEPROM     9790da76d62bc633563f763c3dc1526539ed0a6b .)
```

with (substituting the SHA from Step 2):

```cmake
_evkb_lib(EEPROM         ${_dev}/EEPROM               https://github.com/newdigate/EEPROM          <SHA-FROM-STEP-2> .)
```

- [ ] **Step 4: Prove the fresh-user path**

This exercises the pin, not the local clone. A correct local tree with a stale
pin still ships the replaced header to every new user, so this check is not
optional.

```bash
cd examples/storage-memory/eeprom_test && rm -rf build-ff && cmake -B build-ff -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake -DEVKB_FORCE_FETCH=ON && cmake --build build-ff 2>&1 | tail -15
```

Expected: configures, fetches `newdigate/EEPROM`, builds `build-ff/eeprom_test.elf`.

- [ ] **Step 5: Confirm the fetched copy is ours, not the replaced one**

```bash
grep -l 'SPDX-License-Identifier: MIT' examples/storage-memory/eeprom_test/build-ff/_deps/eeprom-src/EEPROM.h
```

Expected: the path prints. If it does not, the pin did not take effect.

- [ ] **Step 6: Clean up the force-fetch build dir and commit**

```bash
rm -rf examples/storage-memory/eeprom_test/build-ff
git add evkb.cmake
git commit -m "evkb.cmake: pin EEPROM to newdigate/EEPROM

The manifest pointed at PaulStoffregen/EEPROM, whose EEPROM.h is LGPL-2.1 and
was being compiled into two firmware images. Replacing the local checkout alone
would not have fixed that: local-first resolution falls back to this URL the
moment ~/Development/EEPROM is absent, so a fresh clone would have re-fetched
the same file from GitHub. The pin had to move.

Verified with -DEVKB_FORCE_FETCH=ON, which resolves the pin rather than the
local checkout."
```

---

## Task 7: Close the audit blind spot

**Files:**
- Modify: `tools/license-audit.sh` (the `REPOS` assignment, lines 24-29)

- [ ] **Step 1: Add `EEPROM` to the Part-1 sweep**

Its absence is half of why this file was missed for so long. In the `REPOS`
default, change the last line from:

```sh
$HOME/Development/PXP $HOME/Development/MipiDisplay $HOME/Development/LVGL"}
```

to:

```sh
$HOME/Development/PXP $HOME/Development/MipiDisplay $HOME/Development/LVGL \
$HOME/Development/EEPROM"}
```

- [ ] **Step 2: Run Part 1 and confirm the new repo sweeps clean**

```bash
LICENSE_AUDIT_PARTS=1 ./tools/license-audit.sh
```

Expected: no `COPYLEFT header` lines, no `UNLICENSED BINARY` lines,
`LICENSE-AUDIT: PASS`.

- [ ] **Step 3: Prove the sweep actually reaches the new repo**

A silent pass and a pass that never looked are indistinguishable from the exit
code, which is exactly the failure mode this whole exercise is about.

```bash
printf '/* GNU Lesser General Public License */\n' > ~/Development/EEPROM/_tripwire.h
LICENSE_AUDIT_PARTS=1 ./tools/license-audit.sh; echo "exit=$?"
rm -f ~/Development/EEPROM/_tripwire.h
```

Expected: the tripwire file is listed under `COPYLEFT header, not allowlisted:`
and the run exits 1. If it exits 0, `REPOS` was not edited correctly — fix it
before continuing.

Then confirm it is clean again:

```bash
LICENSE_AUDIT_PARTS=1 ./tools/license-audit.sh; echo "exit=$?"
```

Expected: `LICENSE-AUDIT: PASS`, `exit=0`.

- [ ] **Step 4: Confirm the original Part-2 finding is gone**

```bash
LICENSE_AUDIT_PARTS=2 LICENSE_AUDIT_GATES="examples/storage-memory/eeprom_test:eeprom_test examples/usb/usb_host_hid_test:usb_host_hid_test" ./tools/license-audit.sh; echo "exit=$?"
```

Expected: no `COPYLEFT FILE COMPILED` lines, `exit=0`.

**No entry may be added to `ALLOW`, and neither example may be added to
`GATES_EXEMPT`.** Suppressing the finding is not a fix — this tree treats a
check that quietly does not run as a defect.

- [ ] **Step 5: Confirm the audit's own negative tests still fire**

```bash
sh tools/license-audit.test.sh 2>&1 | tail -20
```

Expected: 17 `PASS:` lines, no `FAIL:`. (It drives the checks through
`LICENSE_AUDIT_REPOS`, so the new default entry should not affect it — confirm
that, do not assume it.)

- [ ] **Step 6: Commit**

```bash
git add tools/license-audit.sh
git commit -m "license-audit: sweep the EEPROM repo in part 1

EEPROM was absent from REPOS, which is half of why an LGPL-2.1 EEPROM.h sat
compiled into two firmware images without the audit noticing -- the other half
being that neither consumer was in the part-2 GATES list, since closed.

Verified by tripwire: a file containing the copyleft phrase, dropped into the
repo, is reported and exits non-zero; removing it restores the pass. A sweep
that has never been seen firing over a directory proves nothing about it."
```

---

## Task 8: Full sweep, and settle the disputed baseline

**Files:**
- Modify: `CLAUDE.md` (the two-gate rule section)

- [ ] **Step 1: Read the known-broken list first**

```bash
cat docs/KNOWN-BROKEN-GATES.md
```

`dualcore/cm4_audio_test` is the one documented failure. Do not chase it, do not
weaken it, and do not drop it from the runner to make the sweep green.

- [ ] **Step 2: Confirm nothing else is running**

```bash
ps aux | grep -E 'qemu-system-arm|run_qemu' | grep -v grep
```

Expected: no output. Gates in one directory share a UART capture file.

- [ ] **Step 3: Run the full sweep**

```bash
./tools/run-all-qemu-gates.sh 2>&1 | tail -40
```

Record the actual `N passed, M failed, K SKIP` line.

- [ ] **Step 4: Resolve the disputed baseline**

Three numbers are in circulation and at most one is right:

| Source | Claim |
|---|---|
| `CLAUDE.md` | `28 passed, 1 failed, 0 SKIP` |
| the task brief | `66 passed, 1 failed` |
| `tools/license-audit.sh` `GATES` | enumerates 67 gate-owning examples |

Take the measured result from Step 3 as the truth. Update the `★` paragraph in
`CLAUDE.md` so it states that number, and only that number. Do not adopt either
written figure without measuring.

If the sweep shows any failure **other than** `dualcore/cm4_audio_test`, that is
a real regression from this work — stop and investigate before continuing.

- [ ] **Step 5: Run the full audit now that everything is built**

```bash
./tools/license-audit.sh 2>&1 | tail -20
```

Expected: `LICENSE-AUDIT: PASS`, exit 0. This is the first run of all three
parts over the whole tree, and it is Done-criterion 1.

- [ ] **Step 6: Commit**

```bash
git add CLAUDE.md
git commit -m "CLAUDE.md: correct the stale full-sweep baseline

The documented expectation was '28 passed, 1 failed, 0 SKIP', which predates a
large number of examples -- the audit's own GATES list enumerates 67 gate-owning
examples. A baseline that understates the gate count by more than half means a
sweep can lose dozens of gates and still look right, which is the same class of
silent-omission defect the GATES drift check exists to catch.

Replaced with the measured result."
```

---

## Task 9: Hardware verification — the power-cycle

Two-gate rule. This is persistence code, and QEMU's flash behaviour is not the
silicon's: a wrong sector erase, or a write that does not survive power loss, is
exactly the class of bug QEMU will not show you. `EEPROM_BOOT=RETURN` cannot be
produced in QEMU at all.

**Files:**
- Create: `examples/storage-memory/eeprom_test/transcript_hw_evkb.txt`

- [ ] **Step 1: Clear stale probe daemons**

`pkill LinkServer` alone leaves `redlinkserv`/`crt_emu_cm_redlink` resident,
which silently kills the next few runs.

```bash
pkill LinkServer; pkill redlinkserv; pkill crt_emu_cm_redlink; sleep 1; echo cleared
```

- [ ] **Step 2: Flash — with NOTHING holding the VCOM**

With `tools/rt1170-console.py` attached, `flash … load` dies with
`request to clear DAP error failed - status 131` / `LOAD_EXIT=255` and the port
re-enumerates mid-attempt. The identical command succeeds the moment nothing
holds the port.

```bash
LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load examples/storage-memory/eeprom_test/build/eeprom_test.elf
```

```bash
LinkServer flash MIMXRT1176:MIMXRT1170-EVKB verify examples/storage-memory/eeprom_test/build/eeprom_test.elf
```

Expected: both succeed.

- [ ] **Step 3: Attach the console, then reset**

Start the reader *before* triggering the reset, or the boot output is lost.
macOS `cat` silently resets the port to 9600 — use the pyserial reader.

```bash
ls /dev/cu.usbmodem*
```

In one shell: `python3 tools/rt1170-console.py <port> 115200 | tee /tmp/ee_boot1.txt`

There is no standalone `LinkServer reset` subcommand in 26.6.137; backgrounding
`LinkServer run` is how a reset is triggered:

```bash
LinkServer run MIMXRT1176:MIMXRT1170-EVKB examples/storage-memory/eeprom_test/build/eeprom_test.elf &
```

Expected in the capture: `EEPROM_BOOT=FIRST`, then `EEPROM_RW=PASS`,
`EEPROM_WEAR=PASS`, `EEPROM_API=PASS`, `EEPROM_PERSIST=PASS`,
`EEPROM_LENGTH=4284`, `EEPROM_ALL=PASS`.

- [ ] **Step 4: Power-cycle the board — physically**

Unplug the board's power, wait a few seconds, reconnect. **Do not reflash**, and
do not merely press reset: a warm reset leaves the FlexSPI NOR powered, so it
would prove strictly less than a power cycle does.

- [ ] **Step 5: Capture the second boot**

Start the reader again, then trigger the run:

`python3 tools/rt1170-console.py <port> 115200 | tee /tmp/ee_boot2.txt`

```bash
LinkServer run MIMXRT1176:MIMXRT1170-EVKB examples/storage-memory/eeprom_test/build/eeprom_test.elf &
```

Expected — **this is the un-fakeable proof and the whole point of the task**:

```
EEPROM_BOOT=RETURN payload=0xCAFEF00D
EEPROM_PERSIST_HW=PASS
```

followed by the other stages passing as before.

If it prints `EEPROM_BOOT=FIRST` on the second boot, the marker did not survive
power loss — that is a genuine persistence defect in the flash path, and it is
precisely the bug this task exists to catch. Stop and investigate; do not
proceed to Task 10.

- [ ] **Step 6: Record the evidence and commit**

```bash
cd examples/storage-memory/eeprom_test && { echo "=== boot 1: freshly flashed ==="; cat /tmp/ee_boot1.txt; echo; echo "=== boot 2: after a physical power cycle, no reflash ==="; cat /tmp/ee_boot2.txt; } > transcript_hw_evkb.txt && grep -E 'EEPROM_BOOT|EEPROM_PERSIST_HW|EEPROM_ALL' transcript_hw_evkb.txt
```

Expected: `EEPROM_BOOT=FIRST` from boot 1, `EEPROM_BOOT=RETURN` +
`EEPROM_PERSIST_HW=PASS` from boot 2.

```bash
git add examples/storage-memory/eeprom_test/transcript_hw_evkb.txt
git commit -m "eeprom_test: hardware evidence, including a real power-cycle

Boot 1 freshly flashed prints EEPROM_BOOT=FIRST and writes the marker; boot 2,
after physically removing power and WITHOUT reflashing, prints
EEPROM_BOOT=RETURN with the payload intact.

QEMU cannot produce RETURN -- it boots from -kernel with no backing store behind
the FlexSPI window, so the gate asserts FIRST there. Persistence across power
loss is only provable on silicon, which is why the two-gate rule applies to this
example in particular."
```

---

## Task 10: Remove the replaced package and close out

**Files:**
- Delete: the scratchpad copy of the replaced package

- [ ] **Step 1: Re-confirm everything is green before deleting anything**

```bash
./tools/license-audit.sh 2>&1 | tail -3 && sh tools/license-audit.test.sh 2>&1 | tail -3
```

Expected: `LICENSE-AUDIT: PASS`, and the test script's final lines showing no
`FAIL:`.

- [ ] **Step 2: Confirm `~/Development/EEPROM` is the new repo**

```bash
git -C ~/Development/EEPROM remote -v && git -C ~/Development/EEPROM log --oneline && ls ~/Development/EEPROM
```

Expected: `origin` is `newdigate/EEPROM`; the listing shows `EEPROM.h`,
`EEPROM.cpp`, `LICENSE`, `README.md`, `library.properties`, `keywords.txt` and
no `examples/`.

- [ ] **Step 3: Delete the replaced package**

Pristine upstream at `9790da76` with no local commits — re-clonable from
`https://github.com/PaulStoffregen/EEPROM` if it is ever needed for anything
other than reading, which §1 forbids anyway.

```bash
rm -rf /private/tmp/claude-501/-Users-nicholasnewdigate-Development-rt1170-evkb/431cc620-06cd-4a74-ae8e-0dc03b61f2c9/scratchpad/EEPROM-lgpl-upstream && echo removed
```

- [ ] **Step 4: Verify the Done criteria as a checklist**

Walk the spec's Done criteria and confirm each against a command already run:

1. `./tools/license-audit.sh` exits 0, no `ALLOW` entry added, no `GATES_EXEMPT` entry added — Task 8 Step 5, plus inspection of the diff.
2. `EEPROM` in Part-1 `REPOS` — Task 7.
3. `license-audit.test.sh` still 17 cases — Task 7 Step 5.
4. `run_qemu_eeprom.sh` passes with the three new assertions — Task 5 Step 4.
5. `run_qemu_usbhost.sh` still passes — Task 5 Step 5.
6. Both build under `-DEVKB_FORCE_FETCH=ON` — Task 6 Step 4.
7. Full sweep at its measured baseline, `CLAUDE.md` corrected — Task 8.
8. `EEPROM_BOOT=FIRST` then `RETURN` after a power-cycle — Task 9.
9. `newdigate/EEPROM` pushed with LICENSE + README provenance — Tasks 3, 4, 6.
10. `~/Development/EEPROM` is a clone of the new repo; the replaced checkout is gone — Steps 2-3 above.

```bash
git diff master --stat && git log --oneline master..HEAD
```

- [ ] **Step 5: Confirm the diff added nothing to the suppression lists**

```bash
git diff master -- tools/license-audit.sh | grep -E '^\+' | grep -E 'ALLOW|GATES_EXEMPT'; echo "exit=$? (1 = nothing found, which is what we want)"
```

Expected: no output, `exit=1`. Any hit here means the finding was suppressed
rather than fixed.

- [ ] **Step 6: Update `README.md`'s licence section if it enumerates rewrites**

```bash
grep -n -i 'clean-room\|LGPL' README.md
```

If the licence section lists the files that were rewritten, add `EEPROM.h` to
it. If it only states the general position, no edit is needed. Commit only if
changed.

---

## Notes for the executing engineer

- **`./run_qemu_eeprom.sh`, never `sh run_qemu_eeprom.sh`.** The runner re-execs
  itself under `gtimeout` via `tools/gate-lib.sh`; invoking it through `sh`
  bypasses that.
- **Silicon wins.** A QEMU pass is necessary but not sufficient. If hardware and
  QEMU disagree, do not weaken the gate or the QEMU model to make the divergence
  go away — document it.
- **§1 has no exceptions.** If this plan is missing something you need from the
  replaced header, stop and say so.
