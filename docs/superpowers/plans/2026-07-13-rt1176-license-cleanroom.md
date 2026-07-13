# RT1176 License Clean-Room Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace every LGPL file compiled into RT1176 firmware with clean-room MIT implementations, delete the GPL SdFat/Audio extras, and commit a link-manifest license audit — per the approved spec `docs/superpowers/specs/2026-07-13-rt1176-license-cleanroom-design.md`.

**Architecture:** Same-filename in-place swaps in `cores/imxrt1176` and the two Ethernet repos; gate-first with the old implementation as behavioral oracle (tests pass against OLD, then swap, then re-run); two-agent clean-room separation (spec-author may read anything, implementer gets only contract + tests).

**Tech Stack:** ARM GCC 10 (`/Applications/ARM_10`), CMake + teensy-cmake-macros (Makefile generator), QEMU gates via `evkb/tools/qrun` + `gate-lib.sh`, LinkServer for HW.

---

## Repos and commit targets

| Change | Repo (git -C …) |
|---|---|
| `cores/imxrt1176/*` | `~/Development/rt1170/evkb/cores` |
| Gates, tools, docs | `~/Development/rt1170/evkb` (LOCAL-ONLY, no remote) |
| `src/Client.h`, `Server.h`, `IPAddress.cpp` | `~/Development/Ethernet` AND `~/Development/NativeEthernet` |
| DigitalIO deletion | `~/Development/SdFat` |
| miditones deletion | `~/Development/Audio` |

## Corrections to the spec discovered during planning (harvest results)

1. **No compiled code calls `breakTime`/`makeTime` today** — the only callers are MTP files in the uncompiled `cores/teensy4` reference dir, and `rtc_test` doesn't exercise them either. Consequence: Task 4 ADDS direct vector tests to `rtc_test.cpp` (they are the oracle), and the "SD timestamps depend on Time.cpp" HW risk is downgraded (SdFat never calls it). HW re-verify still runs `rtc_test` on the board.
2. **Consumer String usage is tiny** (must-pass set): `String()`, `String(const char*)`, copy/RVO, `+=` char / C-string, `== "literal"`, `.length()`, `.c_str()`, `.getBytes(buf,size,index)` (load-bearing for `Print::print(String&)` 32-byte chunking), `.toUpperCase()`. Full documented API still implemented; gate covers both.
3. **Stream virtuals**: only `available()/read()/peek()` originate in Stream (pure). `write/flush/availableForWrite` are Print virtuals — Print.h is MIT and MUST NOT be touched. `MTP_Teensy.h` is the only `Stream*`-polymorphic consumer; MIDI is templated (no vtable dependency).
4. **Only `readBytes`** among Stream helpers is called by consumers (USBHost examples), and those resolve to subclass re-declarations (`usb_serial.h:175`, `FS.h:248`). Full helper set still implemented (declared API + docs semantics).
5. **Audit hardened**: CMake depfiles (`*.obj.d`) record the source AND every header each object included, with absolute paths — audit part 2 walks depfiles of three fat gates (`sd_wav_play_test`, `ethernet_test`, `native_ethernet_test`) instead of guessing from archive basenames, and mechanically verifies allowlisted dual-licensed sources compile to empty objects.

## Clean-room protocol (applies to Tasks 3, 4, 6, 8)

**Spec-author subagent** (may read anything): extracts the bare declaration surface of the old file — signatures verbatim, no comments, no bodies, no private members — into `/private/tmp/claude-501/…/scratchpad/<name>-api.md` (scratchpad dir from the session), plus greps subclass/consumer usage of protected members. Output = contract only.

**Implementer subagent prompt template** (fill `<>`):

```text
You are writing a clean-room MIT implementation of <FILE> for the imxrt1176
Arduino core. LEGAL CONSTRAINT — you must NOT open, read, grep, or otherwise
inspect any of these files (or copies of them anywhere):
  cores/imxrt1176/{WString.h,WString.cpp,Stream.h,Stream.cpp,Time.cpp,
  Printable.h}, anything under cores/teensy, cores/teensy3, cores/teensy4,
  ~/Development/Ethernet/src/{Client.h,Server.h,IPAddress.cpp},
  ~/Development/NativeEthernet/src/{Client.h,Server.h,IPAddress.cpp},
  and any Arduino/Teensyduino source online or from memory of its text.
  EXCEPTION: <FILE-BEING-REPLACED only if it is the file you overwrite — no,
  do not read it either; overwrite blind with Write.>
You MAY read: the API contract at <CONTRACT-PATH>, the test sketch at
<TEST-PATH>, and these MIT/clean files: Print.h, core_pins.h,
avr_functions.h, WCharacter.h, wiring.h.
Author the implementation from the contract + the documented Arduino API
semantics. Well-known algorithms by name are fine (Howard Hinnant
days-from-civil/civil-from-days; grow-by-doubling buffers).
File must start with the MIT provenance header (template below), keep the
EXACT same filename/path, and be source-compatible with the contract.
Build & test loop: <BUILD-AND-GATE-COMMANDS>. Iterate until the gate PASSES.
Do NOT commit. Report: files written, gate output tail, any contract
ambiguity you had to resolve (and how).
```

**Provenance header template** (every new file; adjust the one-line description):

```cpp
/* <FILENAME> - <one-line what it is>.
 *
 * Clean-room MIT implementation for the imxrt1176 core: written from the
 * documented Arduino API surface, not derived from the LGPL Arduino/Wiring
 * <original name>.
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
```

**After each implementer run:** orchestrator skims the subagent transcript for forbidden reads before committing (the separation is only defensible if checked).

**Source compatibility, not ABI:** everything rebuilds from source; private member layout of String/Stream is the implementer's free choice.

---

### Task 0: Preflight — clean trees, synced base, green baseline

**Files:** none (commands only)

- [ ] **Step 0.1: Verify working trees + push the cores repo's pending commit**

```bash
git -C ~/Development/rt1170/evkb/cores status -sb   # expect: ahead 1, clean tree
git -C ~/Development/rt1170/evkb/cores push
git -C ~/Development/Ethernet status -sb            # expect clean
git -C ~/Development/NativeEthernet status -sb      # expect clean
git -C ~/Development/SdFat status -sb               # expect clean
git -C ~/Development/Audio status -sb               # check; note any dirt before proceeding
```

Expected: all clean; cores no longer ahead. If Audio is dirty, STOP and report (shared working tree — see memory note).

- [ ] **Step 0.2: Baseline gate sweep (prove all-green BEFORE touching anything)**

```bash
cd ~/Development/rt1170/evkb
for s in */run_qemu*.sh; do echo "== $s"; sh "$s" >/tmp/gate.$$.log 2>&1 \
  && echo PASS || { echo "FAIL: $s"; tail -20 /tmp/gate.$$.log; }; done
```

Expected: every line PASS (~26 gates, ~10-15 min). Record any pre-existing FAIL — do not attribute it to this work later. (Gates whose build/ dir is missing: configure first — see Step 0.3.)

- [ ] **Step 0.3: (Only for gates lacking build/) configure**

Every gate dir vendors `toolchain/rt1170-evkb.toolchain.cmake`:

```bash
cd ~/Development/rt1170/evkb/<gate_dir>
cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake && cmake --build build -j8
```

(Existing gates were all built in prior sessions; this step is contingency.)

---

### Task 1: Clean-room `Printable.h` (warm-up)

**Files:**
- Overwrite: `~/Development/rt1170/evkb/cores/imxrt1176/Printable.h`
- Oracle/consumers: `wprogram_parity_test`, `ethernet_test` (IPAddress implements Printable)

The whole file is public interface — no implementer subagent needed. CRITICAL: exactly ONE virtual member (`printTo`), NO virtual destructor (vtable layout of every Printable subclass).

- [ ] **Step 1.1: Overwrite Printable.h with the clean-room version** (provenance header from the template above, description: "interface for classes that know how to print themselves via Print"):

```cpp
// (after the provenance header block)
#pragma once
#include <stddef.h>

class Print;

// A class inheriting Printable can be passed directly to Print::print /
// Print::println. Exactly one virtual member — do not add a destructor.
class Printable
{
public:
	virtual size_t printTo(Print &p) const = 0;
};
```

- [ ] **Step 1.2: Rebuild + rerun the two consumer gates**

```bash
cd ~/Development/rt1170/evkb/wprogram_parity_test && cmake --build build -j8 && sh run_qemu_wprogram_parity.sh
cd ~/Development/rt1170/evkb/ethernet_test && cmake --build build -j8 && sh run_qemu_ethernet.sh
```

Expected: both PASS.

- [ ] **Step 1.3: Commit (cores repo)**

```bash
git -C ~/Development/rt1170/evkb/cores add imxrt1176/Printable.h
git -C ~/Development/rt1170/evkb/cores commit -m "Clean-room MIT Printable.h (replaces LGPL Arduino header)

Written from the documented Arduino API surface (single pure-virtual
printTo(Print&)). Part of the license clean-room pass; see
evkb/docs/superpowers/specs/2026-07-13-rt1176-license-cleanroom-design.md.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 2: Clean-room `Client.h` + `Server.h` (both Ethernet repos)

**Files:**
- Overwrite: `~/Development/Ethernet/src/Client.h`, `~/Development/Ethernet/src/Server.h`
- Copy to: `~/Development/NativeEthernet/src/Client.h`, `~/Development/NativeEthernet/src/Server.h`
- Oracle: `ethernet_test` + `native_ethernet_test` gates (EthernetClient/EthernetServer override every pure virtual — compile is the test)

Interface-only files; authored inline. The pure-virtual sets were harvested from BOTH repos' subclasses (12 for Client, 1 for Server) — the sets below are the verified compatibility surface.

- [ ] **Step 2.1: Write `~/Development/Ethernet/src/Client.h`** (provenance header, description: "abstract base class for network client streams"):

```cpp
// (after the provenance header block)
#pragma once
#include "Stream.h"
#include "IPAddress.h"

class Client : public Stream {
public:
	virtual int connect(IPAddress ip, uint16_t port) = 0;
	virtual int connect(const char *host, uint16_t port) = 0;
	virtual size_t write(uint8_t b) = 0;
	virtual size_t write(const uint8_t *buf, size_t size) = 0;
	virtual int available() = 0;
	virtual int read() = 0;
	virtual int read(uint8_t *buf, size_t size) = 0;
	virtual int peek() = 0;
	virtual void flush() = 0;
	virtual void stop() = 0;
	virtual uint8_t connected() = 0;
	virtual operator bool() = 0;
protected:
	uint8_t *rawIPAddress(IPAddress &addr) { return addr.raw_address(); }
};
```

- [ ] **Step 2.2: Write `~/Development/Ethernet/src/Server.h`** (provenance header, description: "abstract base class for network servers"):

```cpp
// (after the provenance header block)
#pragma once
#include "Print.h"

class Server : public Print {
public:
	virtual void begin() = 0;
};
```

- [ ] **Step 2.3: Copy byte-identical to NativeEthernet + verify**

```bash
cp ~/Development/Ethernet/src/Client.h ~/Development/NativeEthernet/src/Client.h
cp ~/Development/Ethernet/src/Server.h ~/Development/NativeEthernet/src/Server.h
diff ~/Development/Ethernet/src/Client.h ~/Development/NativeEthernet/src/Client.h && \
diff ~/Development/Ethernet/src/Server.h ~/Development/NativeEthernet/src/Server.h && echo IDENTICAL
```

- [ ] **Step 2.4: Rebuild + rerun both Ethernet gates**

```bash
cd ~/Development/rt1170/evkb/ethernet_test && cmake --build build -j8 && sh run_qemu_ethernet.sh
cd ~/Development/rt1170/evkb/native_ethernet_test && cmake --build build -j8 && sh run_qemu_native_ethernet.sh
```

Expected: both PASS.

- [ ] **Step 2.5: Commit BOTH repos** (same message; note byte-identical single source):

```bash
for r in ~/Development/Ethernet ~/Development/NativeEthernet; do
git -C "$r" add src/Client.h src/Server.h
git -C "$r" commit -m "Clean-room MIT Client.h/Server.h (replace LGPL Arduino headers)

Authored once and copied byte-identical into Ethernet and NativeEthernet.
Written from the documented Arduino API surface (12 Client pure virtuals,
Server::begin). License clean-room pass.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
done
```

---

### Task 3: Clean-room `IPAddress.cpp` (both Ethernet repos)

**Files:**
- Test: add unit section to `~/Development/rt1170/evkb/ethernet_test/ethernet_test.cpp` + token in `run_qemu_ethernet.sh`
- Overwrite (implementer subagent): `~/Development/Ethernet/src/IPAddress.cpp`
- Copy to: `~/Development/NativeEthernet/src/IPAddress.cpp`

Contract (from the MIT `IPAddress.h` + docs): storage is `union { uint8_t bytes[4]; uint32_t dword; }` — **network order in memory** (bytes[0] = first dotted octet; on little-endian ARM, `operator uint32_t` returns that memory as an LSB-first integer, which is what lwIP/FNET expect). Out-of-line members to define = whatever the header declares that doesn't already have an inline body (link errors enumerate them mechanically; expected set: the 4 constructors, `fromString(const char*)`, `operator=` ×2, `printTo`). `fromString` accepts strict dotted-quad, each octet 0–255, returns false on anything malformed. `printTo` prints `bytes[0].bytes[1].bytes[2].bytes[3]` in decimal, returns character count.

- [ ] **Step 3.1: Write the failing-able test — add IPADDR section to `ethernet_test.cpp`** (near the top of `setup()`, BEFORE network phases; adapt `Serial1`/print helper names to the file's existing style):

```cpp
class CapturePrint : public Print {
public:
	char buf[64]; size_t n = 0;
	virtual size_t write(uint8_t c) { if (n < sizeof(buf)-1) buf[n++] = c; buf[n] = 0; return 1; }
};

static bool ipaddr_checks() {
	IPAddress a(192, 168, 1, 101);
	if (a[0] != 192 || a[1] != 168 || a[2] != 1 || a[3] != 101) return false;
	uint32_t v = a;                       // network-order-in-memory
	const uint8_t *vb = (const uint8_t *)&v;
	if (vb[0] != 192 || vb[1] != 168 || vb[2] != 1 || vb[3] != 101) return false;
	IPAddress b(v);
	if (!(b == a)) return false;
	const uint8_t raw[4] = {192, 168, 1, 101};
	if (!(a == raw)) return false;
	IPAddress c;
	if (!(c == IPAddress(0, 0, 0, 0))) return false;   // default = 0.0.0.0
	if (!c.fromString("10.0.2.15")) return false;
	if (c[0] != 10 || c[1] != 0 || c[2] != 2 || c[3] != 15) return false;
	if (c.fromString("999.1.2.3")) return false;       // octet out of range
	if (c.fromString("1.2.3")) return false;           // too few octets
	if (c.fromString("banana")) return false;
	c = raw;                                            // operator=(const uint8_t*)
	if (c[3] != 101) return false;
	c[3] = 7;                                           // operator[] write
	if (c[3] != 7) return false;
	CapturePrint cp;
	a.printTo(cp);
	if (strcmp(cp.buf, "192.168.1.101") != 0) return false;
	return true;
}
// in setup(), before network phases:
//   Serial1.println(ipaddr_checks() ? "IPADDR=OK" : "IPADDR=FAIL");
```

- [ ] **Step 3.2: Wire the token into the gate script** — in `run_qemu_ethernet.sh`, add next to the existing grep checks:

```sh
grep -q "IPADDR=OK" "$OUT" || { echo "FAIL: ipaddr"; exit 1; }
```

- [ ] **Step 3.3: Run against the OLD IPAddress.cpp — must pass (oracle)**

```bash
cd ~/Development/rt1170/evkb/ethernet_test && cmake --build build -j8 && sh run_qemu_ethernet.sh
```

Expected: PASS including IPADDR. If the oracle FAILS a case, the test encodes a wrong expectation — fix the test, not the understanding, and note it.

- [ ] **Step 3.4: Dispatch implementer subagent** with the prompt template. `<FILE>` = `~/Development/Ethernet/src/IPAddress.cpp`. MAY-read additions: `~/Development/Ethernet/src/IPAddress.h` (MIT), the CapturePrint test above. Build/gate loop = Step 3.3 commands. Contract inline (the paragraph above Step 3.1).

- [ ] **Step 3.5: Orchestrator re-runs the gate, checks transcript for forbidden reads, copies to NativeEthernet**

```bash
cd ~/Development/rt1170/evkb/ethernet_test && sh run_qemu_ethernet.sh
cp ~/Development/Ethernet/src/IPAddress.cpp ~/Development/NativeEthernet/src/IPAddress.cpp
cd ~/Development/rt1170/evkb/native_ethernet_test && cmake --build build -j8 && sh run_qemu_native_ethernet.sh
```

Expected: both gates PASS.

- [ ] **Step 3.6: Commit** — evkb repo (gate changes) + both Ethernet repos (byte-identical note as in Task 2), message: `"Clean-room MIT IPAddress.cpp (replaces LGPL implementation)"`.

---

### Task 4: Clean-room `Time.cpp`

**Files:**
- Test: `~/Development/rt1170/evkb/rtc_test/rtc_test.cpp` (add TIME section), `run_qemu_rtc.sh` (add token)
- Overwrite (implementer subagent): `~/Development/rt1170/evkb/cores/imxrt1176/Time.cpp`

Contract: exactly two exported functions, declared in `core_pins.h:2983-2985`:
`void breakTime(uint32_t time, DateTimeFields &tm);` and `uint32_t makeTime(const DateTimeFields &tm);`
`DateTimeFields` = 7×uint8_t `{sec,min,hour,wday,mday,mon,year}`; sec/min 0-59, hour 0-23, wday 0-6 (0=Sunday), mday 1-31, mon 0-11, year 70-206 (offset from 1900; 70=1970, 206=2106). Epoch = Unix 1970 UTC, unsigned 32-bit seconds (valid through 2106-02-07). `breakTime` fills all 7 fields; `makeTime` ignores `wday`. Use Hinnant civil-from-days / days-from-civil (public domain, cite by name in a comment).

- [ ] **Step 4.1: Add TIME vectors to `rtc_test.cpp`** (before the existing RTC checks; vectors computed independently with Python `datetime`, NOT taken from the old Time.cpp):

```cpp
struct TimeVec { uint32_t t; uint8_t sec, min, hour, wday, mday, mon, year; };
static const TimeVec time_vecs[] = {
	{ 0u,          0,  0,  0, 4, 1,  0, 70 },  // 1970-01-01 00:00:00 Thu
	{ 86399u,      59, 59, 23, 4, 1,  0, 70 }, // 1970-01-01 23:59:59 Thu
	{ 946684800u,  0,  0,  0, 6, 1,  0, 100 }, // 2000-01-01 Sat
	{ 951782400u,  0,  0,  0, 2, 29, 1, 100 }, // 2000-02-29 Tue (century leap)
	{ 1234567890u, 30, 31, 23, 5, 13, 1, 109 },// 2009-02-13 23:31:30 Fri
	{ 4102444800u, 0,  0,  0, 5, 1,  0, 200 }, // 2100-01-01 Fri
	{ 4107542399u, 59, 59, 23, 0, 28, 1, 200 },// 2100-02-28 23:59:59 Sun
	{ 4107542400u, 0,  0,  0, 1, 1,  2, 200 }, // 2100-03-01 Mon (2100 NOT leap)
	{ 4294967295u, 15, 28, 6,  0, 7,  1, 206 },// 2106-02-07 06:28:15 Sun (u32 max)
};

static bool time_checks() {
	for (unsigned i = 0; i < sizeof(time_vecs)/sizeof(time_vecs[0]); i++) {
		const TimeVec &v = time_vecs[i];
		DateTimeFields tm;
		breakTime(v.t, tm);
		if (tm.sec != v.sec || tm.min != v.min || tm.hour != v.hour ||
		    tm.wday != v.wday || tm.mday != v.mday || tm.mon != v.mon ||
		    tm.year != v.year) return false;
		if (makeTime(tm) != v.t) return false;
	}
	uint32_t x = 12345;                 // deterministic LCG round-trip sweep
	for (int i = 0; i < 500; i++) {
		x = x * 1664525u + 1013904223u;
		DateTimeFields tm;
		breakTime(x, tm);
		if (makeTime(tm) != x) return false;
	}
	return true;
}
// in setup(), before RTC phases:  Serial1.println(time_checks() ? "TIME_ALL=PASS" : "TIME_ALL=FAIL");
```

- [ ] **Step 4.2: Add to `run_qemu_rtc.sh`**: `grep -q "TIME_ALL=PASS" "$OUT" || { echo "FAIL: time"; exit 1; }`

- [ ] **Step 4.3: Run against OLD Time.cpp — must pass (oracle)**

```bash
cd ~/Development/rt1170/evkb/rtc_test && cmake --build build -j8 && sh run_qemu_rtc.sh
```

- [ ] **Step 4.4: Dispatch implementer subagent** — `<FILE>` = `cores/imxrt1176/Time.cpp`; contract = the paragraph above; MAY read `core_pins.h` (DateTimeFields + prototypes), rtc_test.cpp. Same filename. Build/gate = Step 4.3.

- [ ] **Step 4.5: Orchestrator re-run + transcript check + commit** — cores repo (`imxrt1176/Time.cpp`) and evkb repo (rtc_test changes). Message: `"Clean-room MIT Time.cpp (breakTime/makeTime via Hinnant civil-days algorithms)"`.

---

### Task 5: New `stream_test` QEMU gate (against OLD Stream)

**Files:**
- Create: `~/Development/rt1170/evkb/stream_test/CMakeLists.txt`, `stream_test.cpp`, `run_qemu_stream.sh`

- [ ] **Step 5.1: CMakeLists.txt** (copy the wprogram pattern, cores only):

```cmake
cmake_minimum_required(VERSION 3.24)
project(stream_test)
set(TEENSY_VERSION 117 CACHE STRING "")
include(FetchContent)
FetchContent_Declare(teensy_cmake_macros SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../teensy-cmake-macros)
FetchContent_MakeAvailable(teensy_cmake_macros)
include(${teensy_cmake_macros_SOURCE_DIR}/CMakeLists.include.txt)
import_arduino_library(cores ${CMAKE_CURRENT_LIST_DIR}/../cores/imxrt1176)
teensy_add_executable(stream_test stream_test.cpp)
teensy_target_link_libraries(stream_test cores)
target_link_libraries(stream_test.elf stdc++)
```

Copy the standard per-gate toolchain dir, then configure:

```bash
cp -r ~/Development/rt1170/evkb/wprogram_parity_test/toolchain ~/Development/rt1170/evkb/stream_test/toolchain
cd ~/Development/rt1170/evkb/stream_test
cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake && cmake --build build -j8
```

(The toolchain file resolves COREPATH relative to itself — `../../cores/imxrt1176/` — so the copy works unchanged from any `evkb/<gate>/toolchain/` location. Same for `string_test` in Task 7.)

- [ ] **Step 5.2: stream_test.cpp** — RAM-backed stubs + every helper path. Complete sketch:

```cpp
// QEMU gate for the Stream helper API (parse/find/readBytes/timeout paths).
// Runs against the current Stream implementation; after the clean-room swap
// it re-runs unchanged — the old implementation is the behavioral oracle.
#include <Arduino.h>
#include <string.h>
#include <math.h>

class MemStream : public Stream {
public:
	const char *buf; size_t len, pos;
	MemStream(const char *s) : buf(s), len(strlen(s)), pos(0) {}
	virtual int available() { return (int)(len - pos); }
	virtual int read() { return pos < len ? (uint8_t)buf[pos++] : -1; }
	virtual int peek() { return pos < len ? (uint8_t)buf[pos] : -1; }
	virtual size_t write(uint8_t) { return 1; }
	void seterr() { setReadError(); }        // exercise protected setReadError
};

class EmptyStream : public Stream {          // timeout paths: never has data
public:
	virtual int available() { return 0; }
	virtual int read() { return -1; }
	virtual int peek() { return -1; }
	virtual size_t write(uint8_t) { return 1; }
};

static void check(bool ok, const char *tag) {
	Serial1.print(tag); Serial1.println(ok ? "=OK" : "=FAIL");
}

void setup() {
	Serial1.begin(115200);
	Serial1.println("STREAM GATE");
	bool all = true; bool ok;

	{ MemStream m("abc-123xyz"); m.setTimeout(100);
	  ok = (m.parseInt() == -123); check(ok, "PARSEINT_SKIP"); all &= ok; }
	{ MemStream m("  42"); m.setTimeout(100);
	  ok = (m.parseInt() == 42); check(ok, "PARSEINT_WS"); all &= ok; }
	{ EmptyStream e; e.setTimeout(200); uint32_t t0 = millis();
	  long r = e.parseInt(); uint32_t dt = millis() - t0;
	  ok = (r == 0 && dt >= 180 && dt < 2000); check(ok, "PARSEINT_TIMEOUT"); all &= ok; }
	{ MemStream m("t=12.50;"); m.setTimeout(100);
	  ok = (fabsf(m.parseFloat() - 12.5f) < 0.001f); check(ok, "PARSEFLOAT"); all &= ok; }
	{ MemStream m("-0.25"); m.setTimeout(100);
	  ok = (fabsf(m.parseFloat() + 0.25f) < 0.001f); check(ok, "PARSEFLOAT_NEG"); all &= ok; }
	{ MemStream m("hello"); m.setTimeout(100); char b[16] = {0};
	  size_t n = m.readBytes(b, sizeof(b));
	  ok = (n == 5 && memcmp(b, "hello", 5) == 0); check(ok, "READBYTES"); all &= ok; }
	{ EmptyStream e; e.setTimeout(150); char b[8]; uint32_t t0 = millis();
	  size_t n = e.readBytes(b, 4); uint32_t dt = millis() - t0;
	  ok = (n == 0 && dt >= 130); check(ok, "READBYTES_TIMEOUT"); all &= ok; }
	{ MemStream m("hello\nworld"); m.setTimeout(100); char b[16] = {0};
	  size_t n = m.readBytesUntil('\n', b, sizeof(b));
	  ok = (n == 5 && memcmp(b, "hello", 5) == 0 && m.read() == 'w');
	  check(ok, "READBYTESUNTIL"); all &= ok; }        // terminator consumed, not stored
	{ MemStream m("xxxNEEDLEyyy"); m.setTimeout(100);
	  ok = (m.find((char *)"NEEDLE") && m.read() == 'y'); check(ok, "FIND"); all &= ok; }
	{ MemStream m("xxxyyy"); m.setTimeout(100);
	  ok = (!m.find((char *)"NEEDLE")); check(ok, "FIND_MISS"); all &= ok; }
	{ MemStream m("aaSTOPbbNEEDLE"); m.setTimeout(100);
	  ok = (!m.findUntil((char *)"NEEDLE", (char *)"STOP")); check(ok, "FINDUNTIL"); all &= ok; }
	{ MemStream m("abc"); m.setTimeout(100);
	  String s = m.readString();
	  ok = (s == "abc"); check(ok, "READSTRING"); all &= ok; }
	{ MemStream m("foo,bar"); m.setTimeout(100);
	  String s = m.readStringUntil(',');
	  ok = (s == "foo" && m.read() == 'b'); check(ok, "READSTRINGUNTIL"); all &= ok; }
	{ MemStream m("x");
	  ok = (m.getTimeout() == 1000); m.setTimeout(250); ok &= (m.getTimeout() == 250);
	  check(ok, "TIMEOUT_API"); all &= ok; }
	{ MemStream m("x");
	  ok = (m.getReadError() == 0); m.seterr(); ok &= (m.getReadError() != 0);
	  m.clearReadError(); ok &= (m.getReadError() == 0);
	  check(ok, "READERROR"); all &= ok; }

	Serial1.println(all ? "STREAM_ALL=PASS" : "STREAM_ALL=FAIL");
	Serial1.println("GATE=DONE");
}

void loop() {}
```

- [ ] **Step 5.3: run_qemu_stream.sh** (chmod +x):

```sh
#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/stream_test.elf"; OUT="$DIR/stream.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -icount shift=auto \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/stream.dbg" &
P=$!; gate_pid $P; sleep 20; kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== captured ===="; cat "$OUT"
grep -q "STREAM_ALL=PASS" "$OUT" || { echo "FAIL: stream"; exit 1; }
grep -q "GATE=DONE" "$OUT" || { echo "FAIL: completion"; exit 1; }
echo "PASS: stream gate"
```

- [ ] **Step 5.4: Configure, build, run against OLD Stream — must pass.** If a case fails against the oracle, the expectation is wrong (or a Teensy deviation from Arduino docs) — investigate, fix the TEST, record the finding as a code comment in the sketch.

- [ ] **Step 5.5: Commit gate to evkb repo**: `"Add stream_test QEMU gate (Stream helper/timeout semantics, pre-clean-room oracle run)"`.

---

### Task 6: Clean-room `Stream.h` + `Stream.cpp`

**Files:**
- Contract (spec-author subagent): scratchpad `stream-api.md`
- Overwrite (implementer subagent): `cores/imxrt1176/Stream.h`, `cores/imxrt1176/Stream.cpp`

- [ ] **Step 6.1: Dispatch spec-author subagent**: extract from OLD `Stream.h` the bare declaration list (public + protected, signatures verbatim, default args, no comments/bodies) into `stream-api.md`; ALSO grep subclasses (`HardwareSerial.h`, `usb_serial.h`, `usb_seremu.h`, `FS.h`, Ethernet/NativeEthernet, `USBHost_t36.h`, Wire) for uses of Stream's protected members so the contract marks which protected members are load-bearing. Known constraints to include in the contract verbatim:
  - `class Stream : public Print` — pure virtuals exactly `int available()`, `int read()`, `int peek()`; NO other virtuals (write/flush/availableForWrite belong to Print — do not redeclare).
  - Helper semantics (documented Arduino behavior): `parseInt`/`parseFloat` skip non-numeric lead-in, honor `setTimeout` (default **1000 ms**), return 0 on timeout; `readBytesUntil` consumes but does not store the terminator; `readString`/`readStringUntil` build a String; timeouts via `millis()`.
  - Teensy extensions: `getReadError`/`clearReadError`/protected `setReadError`; protected `timedRead`/`timedPeek`/`peekNextDigit`.

- [ ] **Step 6.2: Dispatch implementer subagent** — `<FILE>` = both `Stream.h` and `Stream.cpp` (same filenames); contract = `stream-api.md`; tests = `stream_test`; build/gate:

```bash
cd ~/Development/rt1170/evkb/stream_test && cmake --build build -j8 && sh run_qemu_stream.sh
```

- [ ] **Step 6.3: Rebuild + rerun ALL Stream-consumer gates** (subclass vtables + MTP polymorphic use). Filenames are unchanged, so incremental rebuilds are sufficient (depfiles catch the header change). If a gate smells stale, the fallback is `rm -rf build && cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake` (every gate vendors that file).

```bash
for g in stream_test serial_test serial_test_rx usb_data_test wprogram_parity_test \
         ethernet_test native_ethernet_test sd_wav_play_test usb_host_hid_test; do
  cd ~/Development/rt1170/evkb/$g && cmake --build build -j8 && sh run_qemu*.sh || { echo "FAIL $g"; break; }
done
```

Expected: all PASS.

- [ ] **Step 6.4: Transcript check + commit cores repo**: `"Clean-room MIT Stream.{h,cpp} (replaces LGPL; available/read/peek pure-virtual surface preserved)"`.

---

### Task 7: New `string_test` QEMU gate (against OLD WString)

**Files:**
- Create: `~/Development/rt1170/evkb/string_test/{CMakeLists.txt,string_test.cpp,run_qemu_string.sh}` (CMakeLists + runner = Task 5 pattern with `string` substituted; runner greps `STRING_ALL=PASS` + `GATE=DONE`)

- [ ] **Step 7.1: string_test.cpp** — complete sketch (must-pass harvested set marked ★; assertions computed independently, never copied from old-String output):

```cpp
// QEMU gate for the String API. ★ = harvested must-pass consumer surface.
#include <Arduino.h>
#include <string.h>
#include <math.h>

static void check(bool ok, const char *tag) {
	Serial1.print(tag); Serial1.println(ok ? "=OK" : "=FAIL");
}
static String make_rvo() { String r("rv"); r += "o"; return r; }

void setup() {
	Serial1.begin(115200);
	Serial1.println("STRING GATE");
	bool all = true; bool ok;

	// ★ default ctor, ★ c-string ctor, ★ length, ★ c_str
	{ String e; ok = (e.length() == 0 && e.c_str() != NULL && e.c_str()[0] == 0);
	  String h("hello"); ok &= (h.length() == 5 && strcmp(h.c_str(), "hello") == 0);
	  check(ok, "CTOR_BASIC"); all &= ok; }
	// numeric ctors (documented API, no in-tree consumers)
	{ ok = (String('A') == "A") && (String(42) == "42") && (String(-42) == "-42")
	     && (String((unsigned long)1000000UL) == "1000000")
	     && (String(255, HEX) == "ff") && (String(255, BIN) == "11111111")
	     && (String(8, OCT) == "10") && (String((unsigned)0) == "0")
	     && (String(3.14159f, 2) == "3.14") && (String(-2.5f, 1) == "-2.5");
	  check(ok, "CTOR_NUM"); all &= ok; }
	// ★ copy + RVO
	{ String a("x"); String b(a); a += "y";
	  ok = (b == "x" && a == "xy" && make_rvo() == "rvo");
	  check(ok, "COPY_RVO"); all &= ok; }
	// assignment incl. self
	{ String a("abc"); a = a; ok = (a == "abc");
	  a = "def"; ok &= (a == "def"); a = F("ghi"); ok &= (a == "ghi");
	  check(ok, "ASSIGN"); all &= ok; }
	// ★ += char, ★ += c-string; += String/int; self-append
	{ String a; for (char c = 'a'; c <= 'e'; c++) a += c;       // readString pattern
	  ok = (a == "abcde");
	  String b("ab"); b += b; ok &= (b == "abab");              // s += s
	  String d("n="); d += 42; ok &= (d == "n=42");
	  check(ok, "CONCAT"); all &= ok; }
	// operator+ chains
	{ String r = String("a") + "b" + 'c' + 42;
	  ok = (r == "abc42"); check(ok, "PLUS_CHAIN"); all &= ok; }
	// ★ == literal; comparisons
	{ String s("HELLO");
	  ok = (s == "HELLO") && (s != "hello") && s.equals("HELLO")
	     && s.equalsIgnoreCase("hello") && (s.compareTo("HELLO") == 0)
	     && (String("abc").compareTo("abd") < 0) && (String("b") > String("a"))
	     && (String("a") < String("b")) && (String("a") <= String("a"));
	  check(ok, "COMPARE"); all &= ok; }
	// indexOf / lastIndexOf
	{ String s("abcabc");
	  ok = (s.indexOf('b') == 1) && (s.indexOf('b', 2) == 4) && (s.indexOf("ca") == 2)
	    && (s.lastIndexOf('b') == 4) && (s.indexOf('z') == -1) && (s.lastIndexOf("zz") == -1);
	  check(ok, "INDEXOF"); all &= ok; }
	// substring incl. clamp + swapped args (documented: from>to are switched)
	{ String s("hamburger");
	  ok = (s.substring(3) == "burger") && (s.substring(3, 6) == "bur")
	    && (s.substring(3, 999) == "burger") && (s.substring(6, 3) == "bur")
	    && (s.substring(99) == "");
	  check(ok, "SUBSTRING"); all &= ok; }
	// ★ toUpperCase; toLowerCase
	{ String s("Hello World"); s.toUpperCase(); ok = (s == "HELLO WORLD");
	  s.toLowerCase(); ok &= (s == "hello world");
	  check(ok, "CASE"); all &= ok; }
	// trim
	{ String s("  x  "); s.trim(); ok = (s == "x");
	  String w("   "); w.trim(); ok &= (w == "" && w.length() == 0);
	  check(ok, "TRIM"); all &= ok; }
	// replace: char, grow, shrink
	{ String s("banana"); s.replace('a', 'o'); ok = (s == "bonono");
	  String g("ab-ab"); g.replace("ab", "xyz"); ok &= (g == "xyz-xyz");
	  String h("xyz-xyz"); h.replace("xyz", "a"); ok &= (h == "a-a");
	  check(ok, "REPLACE"); all &= ok; }
	// remove with clamp
	{ String s("hello"); s.remove(3); ok = (s == "hel");
	  String t("hello"); t.remove(1, 2); ok &= (t == "hlo");
	  String u("hi"); u.remove(1, 99); ok &= (u == "h");
	  check(ok, "REMOVE"); all &= ok; }
	// charAt/setCharAt/[]
	{ String s("cat");
	  ok = (s.charAt(1) == 'a') && (s[2] == 't');
	  s.setCharAt(0, 'b'); ok &= (s == "bat");
	  s[0] = 'r'; ok &= (s == "rat");
	  check(ok, "CHARAT"); all &= ok; }
	// toInt / toFloat incl. garbage → 0
	{ ok = (String("42").toInt() == 42) && (String("-7x").toInt() == -7)
	    && (String("abc").toInt() == 0) && (fabsf(String("3.5").toFloat() - 3.5f) < 0.001f)
	    && (String("nope").toFloat() == 0.0f);
	  check(ok, "TONUM"); all &= ok; }
	// startsWith / endsWith
	{ String s("filename.wav");
	  ok = s.startsWith("file") && !s.startsWith("x") && s.endsWith(".wav") && !s.endsWith(".mp3");
	  check(ok, "STARTEND"); all &= ok; }
	// ★ getBytes — the Print::print(String&) 32-byte chunk protocol; toCharArray
	{ String s("0123456789"); uint8_t b[8] = {0};
	  s.getBytes(b, 5, 2);                      // copies up to 4 chars from index 2 + NUL
	  ok = (memcmp(b, "2345", 4) == 0 && b[4] == 0);
	  char cb[16] = {0}; s.toCharArray(cb, sizeof(cb));
	  ok &= (strcmp(cb, "0123456789") == 0);
	  check(ok, "GETBYTES"); all &= ok; }
	// reserve keeps content; growth across reallocation
	{ String s("seed"); ok = (s.reserve(200) != 0);
	  for (int i = 0; i < 50; i++) s += "xy";
	  ok &= (s.length() == 4 + 100) && s.startsWith("seedxy") && s.endsWith("xy");
	  check(ok, "RESERVE_GROW"); all &= ok; }
	// OOM invariants: absurd reserve fails cleanly; string stays valid
	{ String s("keep");
	  ok = (s.reserve(100u * 1024u * 1024u) == 0);   // 100MB on a ~1MB-RAM part
	  ok &= (s == "keep") && (s.c_str() != NULL);
	  check(ok, "OOM"); all &= ok; }
	// F() / __FlashStringHelper overloads (flash == RAM on this core)
	{ String s(F("abc")); ok = (s == "abc"); s += F("def"); ok &= (s == "abcdef");
	  ok &= (s == F("abcdef"));
	  check(ok, "FLASH"); all &= ok; }
	// Print::print(String&) end-to-end (chunked getBytes path) — checked by the
	// runner grepping the uart for the exact line below.
	{ String s("print-me-via-Print-chunks-print-me-via-Print-chunks-END");
	  Serial1.print("PRINTSTR:"); Serial1.println(s); }

	Serial1.println(all ? "STRING_ALL=PASS" : "STRING_ALL=FAIL");
	Serial1.println("GATE=DONE");
}

void loop() {}
```

Runner additionally greps: `grep -q "PRINTSTR:print-me-via-Print-chunks-print-me-via-Print-chunks-END" "$OUT"`.

- [ ] **Step 7.2: Configure, build, run against OLD WString — must pass.** Divergences from documented behavior: investigate against docs.arduino.cc; if Teensy deviates deliberately, match Teensy and comment the case.

- [ ] **Step 7.3: Commit gate to evkb repo**: `"Add string_test QEMU gate (String API matrix, pre-clean-room oracle run)"`.

---

### Task 8: Clean-room `WString.h` + `WString.cpp` (the big one)

**Files:**
- Contract (spec-author subagent): scratchpad `wstring-api.md`
- Overwrite (implementer subagent): `cores/imxrt1176/WString.h`, `cores/imxrt1176/WString.cpp`

- [ ] **Step 8.1: Dispatch spec-author subagent**: extract OLD `WString.h` bare declarations (public surface incl. `StringSumHelper`/operator+ machinery shape, `__FlashStringHelper` handling, return types — e.g. which mutators return `unsigned char` vs `String&` vs void — and default args, verbatim signatures only) into `wstring-api.md`. Add to contract:
  - Invariants: invalid/OOM String keeps `c_str() != NULL` returning `""`; `substring` clamps and switches out-of-order args; `toInt` on garbage returns 0; self-assign and `s += s` correct; float formatting via `dtostrf` (`avr_functions.h`).
  - `F()`/`__FlashStringHelper*` overloads must exist (flash == RAM here — thin casts).
  - Growth: grow-by-doubling or equivalent amortized scheme, implementer's choice.
  - Source compatibility only; private layout free.

- [ ] **Step 8.2: Dispatch implementer subagent** — `<FILE>` = `WString.h` + `WString.cpp` (same filenames); tests = `string_test`; build/gate:

```bash
cd ~/Development/rt1170/evkb/string_test && cmake --build build -j8 && sh run_qemu_string.sh
```

- [ ] **Step 8.3: Full-ecosystem rebuild + FULL gate suite** (String touches everything):

```bash
cd ~/Development/rt1170/evkb
for s in */run_qemu*.sh; do d=$(dirname "$s"); echo "== $d"; \
  (cd "$d" && cmake --build build -j8 >/tmp/b.log 2>&1 && sh $(basename "$s") >/tmp/g.log 2>&1) \
  && echo PASS || { echo "FAIL: $d"; tail -30 /tmp/b.log /tmp/g.log; }; done
```

Expected: all PASS (now 28 gates incl. stream_test + string_test).

- [ ] **Step 8.4: Transcript check + commit cores repo**: `"Clean-room MIT WString.{h,cpp} (replaces LGPL Arduino String)"`.

---

### Task 9: SdFat surgery — delete GPL DigitalIO + soft-SPI path

**Files:**
- Delete: `~/Development/SdFat/src/DigitalIO/` (dir), `~/Development/SdFat/src/SpiDriver/SdSpiSoftDriver.h`
- Modify: `~/Development/SdFat/src/SpiDriver/SdSpiDriver.h` (two spots: ~line 88 and ~line 147)

- [ ] **Step 9.1: Delete + edit**

```bash
git -C ~/Development/SdFat rm -r src/DigitalIO src/SpiDriver/SdSpiSoftDriver.h
```

In `SdSpiDriver.h`, replace the `#elif SPI_DRIVER_SELECT == 2` *type* branch (currently `class SdSpiSoftDriver; … typedef SdSpiSoftDriver SpiPort_t;`) with:

```cpp
#elif SPI_DRIVER_SELECT == 2
#error "SPI_DRIVER_SELECT == 2 (software SPI) removed from this fork: GPL DigitalIO/SdSpiSoftDriver deleted (license clean-room 2026-07-13)"
```

and the `#elif SPI_DRIVER_SELECT == 2` *include* branch (currently `#include "SdSpiSoftDriver.h"`) with the same `#error` line.

- [ ] **Step 9.2: Check nothing else references the deleted files**

```bash
grep -rn "SdSpiSoftDriver\|DigitalIO" ~/Development/SdFat/src ~/Development/SdFat/examples 2>/dev/null
```

Expected: no hits in `src/` (fix any stragglers found; examples may be deleted too if they reference it — list first, decide per file). Also check SdFat's own `tests/` and LICENSE file for DigitalIO mentions and update.

- [ ] **Step 9.3: Re-run SD gates (evkb + SdFat's own)**

```bash
cd ~/Development/rt1170/evkb/sd_wav_play_test && cmake --build build -j8 && sh run_qemu_sd_wav.sh
ls ~/Development/SdFat/tests/*/run*.sh 2>/dev/null   # run any that exist, same pattern
```

Expected: PASS.

- [ ] **Step 9.4: Commit SdFat**: `"Remove GPL DigitalIO + soft-SPI driver (SPI_DRIVER_SELECT==2 now #error); USDHC path unaffected"`.

---

### Task 10: Delete `Audio/extras/miditones` (user decision: delete, not document)

**Files:**
- Delete: `~/Development/Audio/extras/miditones/`
- Modify: `~/Development/Audio/LICENSE.md` (remove the exception paragraph, lines ~8-10)

- [ ] **Step 10.1:**

```bash
git -C ~/Development/Audio rm -r extras/miditones
```

In `LICENSE.md` remove the paragraph starting `**Exception:** \`extras/miditones/\`` (the generated `william_tell_overture.c` example is miditones *output*, not GPL — it stays).

- [ ] **Step 10.2: Smoke gate + commit**

```bash
cd ~/Development/rt1170/evkb/audiostream_test && sh run_qemu_audiostream.sh
git -C ~/Development/Audio add -A && git -C ~/Development/Audio commit -m "Remove GPL miditones tool from extras (license clean-room; fork is now copyleft-free)"
```

---

### Task 11: `evkb/tools/license-audit.sh` — the permanent audit gate

**Files:**
- Create: `~/Development/rt1170/evkb/tools/license-audit.sh` (chmod +x)

- [ ] **Step 11.1: Write the script** (complete; expect minor iteration while first running it — that's Step 11.2's job):

```sh
#!/bin/sh
# license-audit.sh — prove no copyleft source is compiled into RT1176 firmware.
# Part 1: wrap-tolerant copyleft-header sweep over every ecosystem repo,
#         against a documented allowlist.
# Part 2: link-manifest audit — walk the CMake depfiles (*.obj.d) of three fat
#         gate builds; every source AND header that fed any object must have a
#         permissive header. Dual-licensed allowlisted SOURCES must compile to
#         EMPTY objects (nm check) — the "preprocessor-dead" claim, enforced.
# Part 3: Ethernet/NativeEthernet shared files must be byte-identical.
set -u
TOOL=/Applications/ARM_10/bin
EVKB=$HOME/Development/rt1170/evkb
fail=0

REPOS="$EVKB/cores $HOME/Development/Ethernet $HOME/Development/NativeEthernet \
$HOME/Development/SdFat $HOME/Development/SPI $HOME/Development/Wire \
$HOME/Development/Audio $HOME/Development/SD $HOME/Development/PaulS_SD \
$HOME/Development/USBHost_t36 $HOME/Development/FNET $HOME/Development/lwip"

# Allowlist (grep -E), each entry justified:
#  cores/teensy*   — uncompiled PJRC reference copies (never in any build)
#  SPI/SPI.{h,cpp}, Wire/{Wire.h,Wire.cpp}, Wire/utility/twi.{h,c}
#                  — dual-licensed upstream platform branches; preprocessor-dead
#                    under __IMXRT1176__ (LICENSE.md in each repo); sources
#                    additionally verified EMPTY in part 2.
ALLOW='cores/teensy/|cores/teensy3/|cores/teensy4/|Development/SPI/SPI\.(h|cpp)$|Development/Wire/Wire\.(h|cpp)$|Development/Wire/utility/twi\.(h|c)$'
COPYLEFT='GNU[[:space:]]+(General|Lesser)[[:space:]]+(General[[:space:]]+)?Public[[:space:]]+License'

echo "== Part 1: repo sweep"
for r in $REPOS; do
  [ -d "$r" ] || continue
  hits=$(grep -rIlz --exclude-dir=.git --exclude='*.img' --exclude='LICENSE*' \
         --exclude='COPYING*' --exclude='*.md' -E "$COPYLEFT" "$r" 2>/dev/null \
         | tr '\0' '\n' | grep -vE "$ALLOW" || true)
  if [ -n "$hits" ]; then
    echo "COPYLEFT header, not allowlisted:"; echo "$hits"; fail=1
  fi
done

echo "== Part 2: link-manifest audit"
# gate_dir:elf_target pairs — union covers cores+SPI+Wire+Audio+SdFat+SD,
# Ethernet+lwip, NativeEthernet+FNET.
GATES="sd_wav_play_test:sd_wav_play_test ethernet_test:ethernet_test native_ethernet_test:native_ethernet_test"
for pair in $GATES; do
  g=${pair%%:*}; t=${pair##*:}
  bdir=$EVKB/$g/build
  [ -f "$bdir/$t.elf" ] || { echo "MISSING BUILD: $g (build it first)"; fail=1; continue; }
  files=$(find "$bdir/CMakeFiles" -name '*.obj.d' -exec cat {} + \
          | tr ' \\' '\n\n' | grep '^/' | sort -u)
  for f in $files; do
    case "$f" in
      /Applications/ARM_10/*) continue ;;  # GCC/newlib: GPL w/ Runtime Library
                                           # Exception + BSD — linking permitted
    esac
    [ -f "$f" ] || continue
    if head -c 6000 "$f" | tr '\n' ' ' | grep -qE "$COPYLEFT"; then
      if echo "$f" | grep -qE "$ALLOW"; then
        case "$f" in
          *.c|*.cpp)  # dual-licensed source: object must be EMPTY
            base=$(basename "$f").obj
            syms=""
            for a in "$bdir"/lib*.a; do
              [ -f "$a" ] || continue
              if "$TOOL/arm-none-eabi-ar" t "$a" 2>/dev/null | grep -qx "$base"; then
                syms=$("$TOOL/arm-none-eabi-nm" --defined-only "$a" 2>/dev/null \
                  | awk -v m="$base:" 'index($0,m){f=1;next} /:$/{f=0} f&&NF{print}')
              fi
            done
            if [ -n "$syms" ]; then
              echo "DUAL-LICENSED SOURCE NOT EMPTY in $g: $f"; echo "$syms" | head -5; fail=1
            fi ;;
        esac
      else
        echo "COPYLEFT FILE COMPILED into $g: $f"; fail=1
      fi
    fi
  done
done

echo "== Part 3: Ethernet/NativeEthernet byte-identical shared files"
for f in Client.h Server.h IPAddress.h IPAddress.cpp; do
  cmp -s "$HOME/Development/Ethernet/src/$f" "$HOME/Development/NativeEthernet/src/$f" \
    || { echo "DRIFT: src/$f differs between Ethernet and NativeEthernet"; fail=1; }
done

[ $fail -eq 0 ] && echo "LICENSE-AUDIT: PASS" || echo "LICENSE-AUDIT: FAIL"
exit $fail
```

- [ ] **Step 11.2: Run it; iterate until it passes for the right reasons**

```bash
sh ~/Development/rt1170/evkb/tools/license-audit.sh
```

Expected: `LICENSE-AUDIT: PASS`. Debug rules: a FAIL naming a file we rewrote = stale build (rebuild the three gates); a FAIL naming a new file = real problem, fix the file not the script. Verify part 2 actually scanned files (add `| wc -l` spot-check: each gate should visit hundreds of paths). **Negative test:** temporarily plant a fake `GNU General Public License` header in a scratch .h included by one gate, confirm the audit FAILS, remove it. The audit must be shown capable of failing.

- [ ] **Step 11.3: Commit to evkb repo**: `"Add tools/license-audit.sh (repo sweep + depfile link-manifest + drift check)"`.

---

### Task 12: cores LICENSE.md + provenance sweep

**Files:**
- Create/overwrite: `~/Development/rt1170/evkb/cores/LICENSE.md`

- [ ] **Step 12.1: Verify every rewritten file carries the provenance header**

```bash
for f in Printable.h Stream.h Stream.cpp WString.h WString.cpp Time.cpp; do
  grep -L "SPDX-License-Identifier: MIT" ~/Development/rt1170/evkb/cores/imxrt1176/$f; done
for r in Ethernet NativeEthernet; do for f in Client.h Server.h IPAddress.cpp; do
  grep -L "SPDX-License-Identifier: MIT" ~/Development/$r/src/$f; done; done
```

Expected: no output (grep -L prints only files MISSING the marker).

- [ ] **Step 12.2: Write `cores/LICENSE.md`**:

```markdown
# License

## imxrt1176 core — MIT (fully permissive)

Everything under `imxrt1176/` is MIT-licensed: PJRC-originated files carry
their original MIT headers; the RT1176 port and the 2026-07-13 clean-room
replacements (`Printable.h`, `Stream.h/.cpp`, `WString.h/.cpp`, `Time.cpp`,
`WCharacter.h`, `WMath.cpp`) are Copyright (c) 2026 Nicholas Newdigate, MIT
(SPDX headers in each file). The clean-room files were written from the
documented Arduino API surface, not derived from the LGPL originals — see
`evkb/docs/superpowers/specs/2026-07-13-rt1176-license-cleanroom-design.md`.

No copyleft code is compiled into RT1176 firmware; this is enforced by
`evkb/tools/license-audit.sh` (repo sweep + link-manifest audit).

## teensy/, teensy3/, teensy4/ — upstream reference copies (NOT compiled)

These directories are unmodified PJRC Teensyduino cores kept for reference
and diffing. They retain their upstream licenses (MIT and LGPL-2.1 per file
header) and are never part of any RT1176 build.
```

- [ ] **Step 12.3: Commit cores repo**: `"Document fully-MIT imxrt1176 core in LICENSE.md"`.

---

### Task 13: Final full QEMU suite

- [ ] **Step 13.1: Run everything once more from clean state** (same loop as Step 8.3, all 28 gates + `sh tools/license-audit.sh`). Expected: all PASS. Fix anything red before HW.

---

### Task 14: HW verification (board connected — user confirmed)

Per memory notes `rt1170-evkb-flashing` / `macos-serial-capture`: kill stale probes before every flash (`pkill -9 -f LinkServer; pkill -9 -f redlinkserv`), use `LinkServer run` (not bare flash — vector-catch), start a pyserial reader at 115200 on `/dev/cu.usbmodem5DQ2DDHVWO5EI3` BEFORE `LinkServer run`, `gtimeout` everything.

**Order matters:** SD gate first (card inserted), then REMOVE the SD card before the Ethernet gate (AD_32 MDC↔SD1_CD_B conflict, REVC).

- [ ] **Step 14.1: rtc_test on HW** (RTC + new Time.cpp vectors on silicon)

```bash
pkill -9 -f LinkServer; pkill -9 -f redlinkserv; sleep 1
python3 - <<'EOF' > /tmp/rtc-hw.uart &
import serial
s = serial.Serial('/dev/cu.usbmodem5DQ2DDHVWO5EI3', 115200, timeout=40)
import sys, time
end = time.time() + 40
while time.time() < end:
    d = s.read(256)
    if d: sys.stdout.write(d.decode('utf-8', 'replace')); sys.stdout.flush()
EOF
READER=$!
gtimeout 60 /Applications/LinkServer_26.6.137/LinkServer run MIMXRT1176:MIMXRT1170-EVKB \
  ~/Development/rt1170/evkb/rtc_test/build/rtc_test.elf
wait $READER; grep -E "TIME_ALL=PASS|RTC_ALL=PASS" /tmp/rtc-hw.uart
```

Expected: both `TIME_ALL=PASS` and `RTC_ALL=PASS` (persistence phase may require the second boot per the gate's design — follow the sketch's printed instructions).

- [ ] **Step 14.2: sd_wav_play_test on HW** (SD card with the gate's WAV inserted) — same flash/capture pattern with `sd_wav_play_test/build/sd_wav_play_test.elf`; expect the gate's PASS tokens on the uart capture.

- [ ] **Step 14.3: ethernet_test on HW** (SD card REMOVED, RJ45 to LAN) — same pattern with `ethernet_test/build/ethernet_test.elf`; expect DHCP lease + ping + HTTP-GET tokens (HW client mode per the sketch). NativeEthernet HW run optional if time allows (Task 3/2 files are byte-identical, QEMU-gated in both).

- [ ] **Step 14.4: Record results** in `evkb/docs/` (append to the relevant HW-RESULTS or create `docs/HW-RESULTS-LICENSE-CLEANROOM.md` with the captured uart extracts) and commit to evkb repo.

---

### Task 15: Wrap-up — pushes + memory

- [ ] **Step 15.1: Push all remote-backed repos**

```bash
for r in ~/Development/rt1170/evkb/cores ~/Development/Ethernet ~/Development/NativeEthernet \
         ~/Development/SdFat ~/Development/Audio; do git -C "$r" push; done
```

(evkb has no remote — its commits stay local, as with every prior session.)

- [ ] **Step 15.2: Update memory** — edit `prefer-permissive-licenses.md` (LGPL core files now clean-roomed; audit script exists) and write a new memory `rt1176-license-cleanroom.md` (what was rewritten, the two-agent process, audit script location, HW-verified status) + MEMORY.md index line.

- [ ] **Step 15.3: Final report** — summary table: files replaced, gates green (count), audit output, HW captures, commits/pushes per repo.
