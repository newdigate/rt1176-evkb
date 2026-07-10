# RT1176 EXTMEM Dynamic Allocation (`extmem_malloc`) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** implement `extmem_malloc/free/calloc/realloc` (declared in `wiring.h:227-230`) as a heap over the 64 MB SEMC SDRAM, plus zero-init of the static `EXTMEM` `.bss`.

**Architecture:** port teensy4's `smalloc` pool allocator + `extmem.c` verbatim into the imxrt1176 core (with one `__IMXRT1176__` `IS_EXTMEM` branch), and add the pool-init + EXTMEM `.bss` zero-init in `startup.c` after `semc_sdram_init()`. A new `extmem_test` gate proves it (QEMU + HW). No qemu2 change — the SDRAM window is already gated/enabled by the SEMC model.

**Tech Stack:** C (Teensyduino core, RT1176 M7), `smalloc`, CMake cross-build (ARM GCC 10.2.1 `/Applications/ARM_10`), QEMU `mimxrt1170-evk`, LinkServer + LPUART1 VCOM.

**Spec:** `docs/superpowers/specs/2026-07-10-rt1176-extmem-malloc-design.md`. Builds on memory `rt1176-sdram-semc` (static SDRAM + `EXTMEM` + D-cache off + faithful window gate).

**Port sources (copy from these — do NOT invent):** `cores/teensy4/{extmem.c, smalloc.h, smalloc_i.h, sm_*.c}` and the `ARDUINO_MIMXRT1060_EVKB` pool-init path in `cores/teensy4/startup.c` (~line 430). `smalloc_curr_pool` is defined in `sm_pool.c:9`; `extmem_smalloc_pool` is `extern` in `smalloc.h:50` and defined by us in `startup.c`.

---

## File Structure

| File | Change |
|---|---|
| `cores/imxrt1176/smalloc.h`, `smalloc_i.h` | copy verbatim from `teensy4/` |
| `cores/imxrt1176/sm_alloc_valid.c` … `sm_zalloc.c` (13 files) | copy verbatim from `teensy4/` |
| `cores/imxrt1176/extmem.c` | copy from `teensy4/` + add `__IMXRT1176__` `HAS_EXTRAM`/`IS_EXTMEM` branch |
| `cores/imxrt1176/startup.c` | add `external_psram_size` + `extmem_smalloc_pool` globals; EXTMEM `.bss` zero-init + `sm_set_pool` after `semc_sdram_init()` |
| `evkb/extmem_test/` | new gate: firmware + CMakeLists + toolchain + runner |

**Commit to `master`; do NOT push.** Repos: `newdigate/teensy-cores` (git root `evkb/cores`), `evkb` (local). `git add` named files only. The evkb tree is shared across sessions — only touch `extmem_test/`.

**★ Core-glob gotcha:** `import_arduino_library(cores …)` uses `file(GLOB_RECURSE **.c)` with **no `CONFIGURE_DEPENDS`**. After adding the new core `.c` files, any gate must `rm -rf build` + reconfigure or it will miss `sm_*.c` and fail to link the new `startup.c` (`sm_set_pool` undefined). The new `extmem_test` gate configures fresh, so it is fine; this note matters if other gates are rebuilt.

---

## Task 1: Port `smalloc` + `extmem.c` into the core

**Files:** copy into `cores/imxrt1176/`: `smalloc.h`, `smalloc_i.h`, the 13 `sm_*.c`, and `extmem.c` (then edit `extmem.c`).

- [ ] **Step 1: Copy the allocator verbatim.**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/cores/imxrt1176
cp ../teensy4/smalloc.h ../teensy4/smalloc_i.h .
cp ../teensy4/sm_alloc_valid.c ../teensy4/sm_calloc.c ../teensy4/sm_free.c \
   ../teensy4/sm_hash.c ../teensy4/sm_malloc.c ../teensy4/sm_malloc_stats.c \
   ../teensy4/sm_pool.c ../teensy4/sm_realloc.c ../teensy4/sm_realloc_i.c \
   ../teensy4/sm_realloc_move.c ../teensy4/sm_szalloc.c ../teensy4/sm_util.c \
   ../teensy4/sm_zalloc.c .
cp ../teensy4/extmem.c .
ls sm_*.c smalloc*.h extmem.c | wc -l   # expect 16 (13 + 2 + 1)
```

- [ ] **Step 2: Add the `__IMXRT1176__` branch to `extmem.c`.** The copied file gates `HAS_EXTRAM` on `ARDUINO_TEENSY41` / `ARDUINO_MIMXRT1060_EVKB` only. Add our board. Find the block:

```c
#if defined(ARDUINO_TEENSY41) || defined(ARDUINO_MIMXRT1060_EVKB)
#define HAS_EXTRAM
```

and insert an `__IMXRT1176__` arm immediately **before** it, so our case is handled first:

```c
#if defined(__IMXRT1176__)
#define HAS_EXTRAM
// MIMXRT1170-EVKB: 64 MB SEMC SDRAM at 0x80000000..0x83FFFFFF
#define IS_EXTMEM(addr) (((uint32_t)(addr) >> 28) == 8)
#elif defined(ARDUINO_TEENSY41) || defined(ARDUINO_MIMXRT1060_EVKB)
#define HAS_EXTRAM
#if defined(ARDUINO_MIMXRT1060_EVKB)
#define IS_EXTMEM(addr) (((uint32_t)(addr) >> 28) == 8)
#else
#define IS_EXTMEM(addr) (((uint32_t)(addr) >> 28) == 7)
#endif
#endif
```

(The four wrapper functions below are unchanged — they already use `HAS_EXTRAM` / `IS_EXTMEM` / `extmem_smalloc_pool`.)

- [ ] **Step 3: Compile-check the self-contained allocator files** under the ARM target. The `sm_*.c` include only `smalloc.h`/`smalloc_i.h` + standard C headers, so they compile standalone (`extmem.c` pulls `wiring.h`/Arduino headers — its compile is proven by the Task-3 gate build, which uses the full toolchain include paths):

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/cores/imxrt1176
for f in sm_*.c; do
  /Applications/ARM_10/bin/arm-none-eabi-gcc -c -mcpu=cortex-m7 -mthumb \
    -D__IMXRT1176__ -I. -o /tmp/sm_check.o "$f" && echo "OK $f" || echo "FAIL $f"
done
rm -f /tmp/sm_check.o
```
Expected: `OK` for all 13 (warnings acceptable; no errors). Also `diff` a couple against the source to confirm verbatim: `diff sm_pool.c ../teensy4/sm_pool.c && diff smalloc.h ../teensy4/smalloc.h && echo VERBATIM`.

- [ ] **Step 4: Commit** (teensy-cores):

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/cores
git add imxrt1176/smalloc.h imxrt1176/smalloc_i.h imxrt1176/sm_*.c imxrt1176/extmem.c
git commit -m "imxrt1176: port smalloc pool allocator + extmem.c (extmem_malloc family)"
```

---

## Task 2: Wire the pool init + EXTMEM zero-init in `startup.c`

**Files:** `cores/imxrt1176/startup.c`. Test: a firmware calling `extmem_malloc` links and the pool symbol resolves (proven fully in Task 3).

- [ ] **Step 1: Add the include + externs + globals.** Near the top of `startup.c`, after the existing `extern uint32_t _ebss_dma;` externs (around line 70), add the EXTMEM linker externs; and near the other file-scope includes add `#include "smalloc.h"`. Then, at file scope (e.g. just before `ResetHandler`), define the two globals:

```c
#include "smalloc.h"          /* extmem_smalloc_pool + sm_set_pool */

extern uint32_t _extram_start; /* .bss.extram (EXTMEM statics) start (imxrt1176.ld) */
extern uint32_t _extram_end;   /* .bss.extram end                                   */

uint8_t external_psram_size = 0;        /* reported external RAM size in MB (fixed 64) */
struct smalloc_pool extmem_smalloc_pool; /* the EXTMEM heap pool (declared extern in smalloc.h) */
```

- [ ] **Step 2: Zero-init EXTMEM `.bss` + set up the pool, right after `semc_sdram_init()`.** Find (around line 233):

```c
	semc_sdram_init();

	/* C++ static constructors, then the application */
	__libc_init_array();
```

and insert the EXTMEM setup **between** them (SDRAM must be live; must precede C++ ctors that may touch EXTMEM or call `extmem_malloc`):

```c
	semc_sdram_init();

	/* Zero the static EXTMEM globals (.bss.extram — SDRAM is now live) and hand
	 * the rest of the 64 MB to the extmem_malloc pool. D-cache is off in this
	 * core, so the memory_clear writes reach SDRAM directly (teensy4 needs a
	 * dcache flush here; we do not). */
	memory_clear(&_extram_start, &_extram_end);
	external_psram_size = 64;                       /* soldered 64 MB SDRAM — no probe */
	sm_set_pool(&extmem_smalloc_pool, &_extram_end,
	            64u * 0x100000u - ((uint32_t)&_extram_end - (uint32_t)&_extram_start),
	            1, NULL);                            /* do_zero=1 (calloc semantics) */

	/* C++ static constructors, then the application */
	__libc_init_array();
```

- [ ] **Step 3: Self-check the additions** (the compile + link proof lands in Task 3's gate build, which compiles this `startup.c` with the full toolchain include paths and links `sm_set_pool`/`extmem_smalloc_pool`). Confirm all three edits are present and ordered correctly:

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/cores/imxrt1176
grep -q '#include "smalloc.h"' startup.c && echo "OK include"
grep -q 'struct smalloc_pool extmem_smalloc_pool;' startup.c && echo "OK pool global"
# the sm_set_pool call must come AFTER semc_sdram_init() and BEFORE __libc_init_array()
awk '/semc_sdram_init\(\);/{s=NR} /sm_set_pool/{p=NR} /__libc_init_array\(\);/{l=NR} END{print (s<p && p<l) ? "OK order" : "BAD order"}' startup.c
```
Expected: `OK include`, `OK pool global`, `OK order`.

- [ ] **Step 4: Commit** (teensy-cores):

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/cores
git add imxrt1176/startup.c
git commit -m "imxrt1176: init extmem_malloc pool + zero-init EXTMEM .bss in startup"
```

---

## Task 3: `extmem_test` gate

**Files:** `evkb/extmem_test/{extmem_test.cpp, CMakeLists.txt, toolchain/, .gitignore, run_qemu_extmem.sh}`.

- [ ] **Step 1: Firmware** `extmem_test.cpp` (Serial1 markers, setup context):

```cpp
#include "Arduino.h"
#include "HardwareSerial.h"
#include "wiring.h"    // extmem_malloc / _free / _calloc / _realloc

#define IS_EXTMEM(p) (((uint32_t)(p) >> 28) == 8)   // SDRAM 0x8000_0000..0x83FF_FFFF

void setup()
{
	Serial1.begin(115200);
	while (!Serial1) {}
	Serial1.println("EXTMEM_INIT");

	// 1. malloc: an SDRAM pointer, write + read-back
	uint32_t *a = (uint32_t *)extmem_malloc(4096);
	bool alloc_ok = (a != NULL) && IS_EXTMEM(a);
	if (alloc_ok) {
		for (int i = 0; i < 1024; i++) a[i] = 0xC0DE0000u + (uint32_t)i;
		for (int i = 0; i < 1024; i++) if (a[i] != 0xC0DE0000u + (uint32_t)i) { alloc_ok = false; break; }
	}
	Serial1.print("EXTMEM_ALLOC="); Serial1.println(alloc_ok ? "PASS" : "FAIL");

	// 2. calloc: zeroed SDRAM
	uint32_t *c = (uint32_t *)extmem_calloc(256, sizeof(uint32_t));
	bool calloc_ok = (c != NULL) && IS_EXTMEM(c);
	if (calloc_ok) for (int i = 0; i < 256; i++) if (c[i] != 0u) { calloc_ok = false; break; }
	Serial1.print("EXTMEM_CALLOC="); Serial1.println(calloc_ok ? "PASS" : "FAIL");

	// 3. realloc: contents preserved across a grow (which may move)
	uint32_t *r = (uint32_t *)extmem_realloc(a, 16384);   // a is consumed
	bool realloc_ok = (r != NULL) && IS_EXTMEM(r);
	if (realloc_ok) for (int i = 0; i < 1024; i++) if (r[i] != 0xC0DE0000u + (uint32_t)i) { realloc_ok = false; break; }
	Serial1.print("EXTMEM_REALLOC="); Serial1.println(realloc_ok ? "PASS" : "FAIL");

	// 4. free + re-malloc still lands in SDRAM
	extmem_free(r);
	extmem_free(c);
	uint32_t *d = (uint32_t *)extmem_malloc(4096);
	bool free_ok = (d != NULL) && IS_EXTMEM(d);
	Serial1.print("EXTMEM_FREE="); Serial1.println(free_ok ? "PASS" : "FAIL");
	extmem_free(d);

	// 5. fallback: a request larger than the whole 64 MB pool degrades to NULL
	//    (pool refuses; internal malloc also can't serve 100 MB) — no crash.
	void *huge = extmem_malloc((size_t)100 * 1024 * 1024);
	bool fallback_ok = (huge == NULL);
	if (huge) extmem_free(huge);
	Serial1.print("EXTMEM_FALLBACK="); Serial1.println(fallback_ok ? "PASS" : "FAIL");

	bool all = alloc_ok && calloc_ok && realloc_ok && free_ok && fallback_ok;
	Serial1.print("EXTMEM_TEST="); Serial1.println(all ? "PASS" : "FAIL");
}

void loop() {}
```

- [ ] **Step 2: CMakeLists + toolchain** (core only, model on `sdram_test`):

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb
mkdir -p extmem_test
cp -r sdram_test/toolchain extmem_test/toolchain
```

`extmem_test/CMakeLists.txt`:
```cmake
cmake_minimum_required(VERSION 3.24)
project(extmem_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(FetchContent)
FetchContent_Declare(teensy_cmake_macros SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../teensy-cmake-macros)
FetchContent_MakeAvailable(teensy_cmake_macros)
include(${teensy_cmake_macros_SOURCE_DIR}/CMakeLists.include.txt)

import_arduino_library(cores ${CMAKE_CURRENT_LIST_DIR}/../cores/imxrt1176)

teensy_add_executable(extmem_test extmem_test.cpp)
teensy_target_link_libraries(extmem_test cores)

target_link_libraries(extmem_test.elf stdc++)
```

`extmem_test/.gitignore`:
```
build/
*.uart
*.dbg
*.raw
```

- [ ] **Step 3: Build** (fresh — picks up the new core `sm_*.c`/`extmem.c`):

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/extmem_test
rm -rf build
cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake
cmake --build build
```
Expected: `extmem_test.elf` built. Confirm the allocator linked: `/Applications/ARM_10/bin/arm-none-eabi-nm build/extmem_test.elf | grep -E 'extmem_malloc|sm_set_pool|extmem_smalloc_pool'` shows all three defined (`T`/`B`).

- [ ] **Step 4: Runner** `run_qemu_extmem.sh` (model on `sdram_test/run_qemu_sdram.sh`):

```sh
#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/extmem_test.elf"; OUT="$DIR/extmem.uart"
rm -f "$OUT"
# The SDRAM window is enabled by the SEMC model on the guest's Mode-Set IP command
# (see rt1176-sdram-semc); extmem_malloc then hands out real 0x8... addresses.
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/extmem.dbg" &
P=$!; gate_pid $P; sleep 6; kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== captured ===="; cat "$OUT"
grep -q "EXTMEM_ALLOC=PASS"    "$OUT" || { echo "FAIL: malloc/IS_EXTMEM";   exit 1; }
grep -q "EXTMEM_CALLOC=PASS"   "$OUT" || { echo "FAIL: calloc-zero";        exit 1; }
grep -q "EXTMEM_REALLOC=PASS"  "$OUT" || { echo "FAIL: realloc-preserve";   exit 1; }
grep -q "EXTMEM_FREE=PASS"     "$OUT" || { echo "FAIL: free/re-malloc";     exit 1; }
grep -q "EXTMEM_FALLBACK=PASS" "$OUT" || { echo "FAIL: oversize fallback";  exit 1; }
grep -q "EXTMEM_TEST=PASS"     "$OUT" || { echo "FAIL: overall";            exit 1; }
echo "PASS: extmem_malloc verified (alloc/calloc/realloc/free in SDRAM + graceful fallback)"
```
```bash
chmod +x /Users/nicholasnewdigate/Development/rt1170/evkb/extmem_test/run_qemu_extmem.sh
```

- [ ] **Step 5: Drive to green.** Run `./run_qemu_extmem.sh` → `EXTMEM_TEST=PASS`. The `IS_EXTMEM` asserts are self-validating: if the pool were mis-initialised and `extmem_malloc` fell back to internal RAM, the pointers would not be `0x8…` and `EXTMEM_ALLOC` would FAIL — so a PASS proves the SDRAM pool is live. Fix `extmem.c`/`startup.c` as needed; re-run.

- [ ] **Step 6: Commit** (evkb gate — named files only):

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb
git add extmem_test/extmem_test.cpp extmem_test/CMakeLists.txt \
        extmem_test/toolchain/rt1170-evkb.toolchain.cmake \
        extmem_test/.gitignore extmem_test/run_qemu_extmem.sh
git commit -m "extmem_test: gate for extmem_malloc family over the 64 MB SDRAM"
```

---

## Task 4: Hardware verification (controller-run)

- [ ] Flash `extmem_test.elf` (LinkServer; `pkill -9 -f 'LinkServer|redlinkserv'` first): `LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load "$(pwd)/build/extmem_test.elf"`.
- [ ] Capture VCOM `/dev/cu.usbmodem5DQ2DDHVWO5EI3` @115200 (pyserial reader started **before** reset, per `macos-serial-capture`).
- [ ] Expect `EXTMEM_INIT` + `EXTMEM_ALLOC=PASS` + `EXTMEM_CALLOC=PASS` + `EXTMEM_REALLOC=PASS` + `EXTMEM_FREE=PASS` + `EXTMEM_FALLBACK=PASS` + `EXTMEM_TEST=PASS`. **HW is the arbiter** — this proves the pool metadata + allocations live in real SDRAM (`0x8…`) and survive read-back with no corruption.
- [ ] If FAIL: a non-`IS_EXTMEM` pointer → pool not initialised (check `sm_set_pool` ran after `semc_sdram_init`); a read-back mismatch → SDRAM/allocator corruption (unlikely — SDRAM is HW-proven by [[rt1176-sdram-semc]]). Record the result.

---

## Task 5: Final review, memory, commit hygiene

- [ ] Final code review: the `__IMXRT1176__` `IS_EXTMEM` branch, the `startup.c` pool-init (order after `semc_sdram_init`, `do_zero=1`, D-cache-off divergence), no leftover artifacts, all trees clean. `smalloc`/`sm_*.c` are verbatim copies (spot-check `diff` vs `teensy4/` shows zero changes).
- [ ] Update memory — new `rt1176-extmem-malloc` note (verbatim smalloc port, pool over `_extram_end`→64 MB, EXTMEM `.bss` zero-init, D-cache-off divergence, the core-glob reconfigure gotcha, HW result) + `MEMORY.md` pointer; link [[rt1176-sdram-semc]].
- [ ] Confirm commits on `master` (teensy-cores, evkb); **do NOT push**. Report SHAs.

---

## Self-review notes (author)

- **Spec coverage:** C1 smalloc→T1, C2 extmem.c branch→T1, C3 startup pool-init+zero-init→T2, C4 gate→T3, HW→T4, review/memory→T5. Risks: #1 glob→T3 (fresh build) + the file-structure note, #2 init-order→T2 (after semc_sdram_init), #3 smalloc portability→T1 (verbatim + compile-check), #4 zero-init cost→T2 (only `.bss.extram`), #5 fixed size→T2 (`external_psram_size=64`).
- **Types consistent:** `sm_set_pool(pool, void*, size_t, int do_zero, oom_handler)` matches `smalloc.h:59`; `extmem_smalloc_pool`/`smalloc_curr_pool` defs (startup.c / sm_pool.c) resolve the `smalloc.h` externs; `IS_EXTMEM` (`>>28==8`) identical in `extmem.c` and the gate; `memory_clear(uint32_t*, uint32_t*)` matches `startup.c:399`.
- **No placeholders:** every step has concrete copy commands, the exact `extmem.c` branch, the exact `startup.c` insertion, the full gate firmware/CMake/runner, and expected output.
