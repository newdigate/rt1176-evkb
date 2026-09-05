# CM7 L1 I-cache enable (NEW-36) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Enable the Cortex-M7 L1 instruction cache in the `imxrt1176` core's `ResetHandler` (I-cache ONLY — no D-cache, no MPU), prove the QEMU sweep stays 128/0/0, and measure the speedup on silicon with a new microbench plus the acid_box LOOPSTAT A/B, with two "nothing moved" silicon witnesses.

**Architecture:** Two files in the sibling `teensy-cores` repo change (`imxrt1176/imxrt1176.h` gains the SCB cache register surface + three `static inline` helpers; `imxrt1176/startup.c` snapshots the ROM's `SCB_CCR` and calls `arm_icache_enable()` right after the `.bss`/`.dmabuffers` zero). A new silicon-only example `examples/timing/icache_bench_hw` A/Bs the cache in one boot over three workloads (`calls`, `crc`, the real `Sbc` encoder) in ITCM and in flash, with `place=` derived from each function's address. qemu2 masks `CCR.IC` and NOPs `ICIALLU`, so the 128-gate sweep is a boot-regression check and every timing claim is silicon-only.

**Tech Stack:** ARM GCC 10 (`/Applications/ARM_10/bin/`), CMake ≥ 3.24 with the tree's `evkb.cmake` / teensy-cmake-macros, qemu2 (`~/Development/qemu2`), LinkServer 26.6.137, `tools/rt1170-console.py`, Linear (NEW-36).

**Spec:** `docs/superpowers/specs/2026-09-05-cm7-icache-enable-design.md`

**Branch:** `nicnewdigate/new-36-cm7-icache` (evkb, already created). The core change lands on `teensy-cores` `master` (that repo has no feature-branch convention; every prior core change was committed on master and pinned from here).

**Scratchpad for logs:** `/private/tmp/claude-501/-Users-nicholasnewdigate-Development-rt1170-evkb/1559322c-83b9-42e9-ab62-5d09e4b2fe00/scratchpad` — referred to below as `$SCRATCH`. Export it once per shell:

```bash
export SCRATCH=/private/tmp/claude-501/-Users-nicholasnewdigate-Development-rt1170-evkb/1559322c-83b9-42e9-ab62-5d09e4b2fe00/scratchpad
export ARM=/Applications/ARM_10/bin
```

---

## File structure

| file | responsibility |
|---|---|
| `~/Development/teensy-cores/imxrt1176/imxrt1176.h` | SCB cache register defines; `arm_icache_invalidate_all/enable/disable()`; `extern uint32_t startup_ccr_at_reset` |
| `~/Development/teensy-cores/imxrt1176/startup.c` | defines `startup_ccr_at_reset`; snapshot + `arm_icache_enable()` in `ResetHandler`; header comment corrected |
| `examples/timing/icache_bench_hw/CMakeLists.txt` | new; links `cores` + `M2Radio`; derives the Sbc-to-FLASH linker script unless `ICACHE_BENCH_SBC_ITCM=ON` |
| `examples/timing/icache_bench_hw/icache_bench_hw.cpp` | new; the bench (workloads, timing harness, rows, summary) |
| `examples/timing/icache_bench_hw/README.md` | new; what it measures, how to run, the numbers |
| `examples/timing/icache_bench_hw/transcript_hw_evkb.txt` | new; two SW4 boots of the default build + one of the `build-sbc-itcm` build |
| `examples/README.md` | timing row gains `icache_bench_hw` |
| `examples/display/acid_box/CMakeLists.txt` | the "(I-cache-covered)" comment becomes true and cites the number |
| `examples/display/acid_box/transcript_hw_evkb_bt.txt` | NEW-36 LOOPSTAT A/B block appended |
| `examples/display/acid_box/transcript_hw_evkb.txt` | NEW-36 regression boots appended |
| `examples/dualcore/cm4_audio_test/transcript_hw_evkb.txt` | NEW-36 regression boot appended |
| `evkb.cmake` | `cores` pin → the new SHA |
| `CLAUDE.md` | measurement block; Architecture cache paragraph; NEW-33 finding (a) retired |

---

### Task 1: SCB cache register surface + I-cache helpers (`imxrt1176.h`)

**Files:**
- Modify: `~/Development/teensy-cores/imxrt1176/imxrt1176.h:156-159` (after the `ARM_DEMCR_TRCENA` define) and `:882-885` (after the `arm_dcache_*` no-ops)

There is no host test for six-line register sequences (the spec's Testing table says so); the functional witnesses are Task 3 (boot) and Task 5 (`sbc_crc_match=1`, `isize_kb=32`). The check for this task is that the header still compiles for C (startup.c) and C++ (a sketch), and that a sketch can call the helpers.

- [ ] **Step 1: Add the register defines after line 159 (`#define ARM_DEMCR_TRCENA (1u << 24)`)**

```c
/* ---- Cortex-M7 L1 caches: SCB cache-control surface (ARM DDI0403E §B3.2) ----
 * The INSTRUCTION cache is enabled by ResetHandler (startup.c, NEW-36).  The DATA
 * cache is NOT: it needs MPU regions plus a DMA-coherency review (the rt1062
 * DMAMEM/uncached-OCRAM lesson) -- see the arm_dcache_* no-ops further down. */
#define SCB_CCR            (*(volatile uint32_t *)0xE000ED14u)        /* Configuration and Control */
#define SCB_CCR_BP         (1u << 18)   /* branch prediction enable (RAO/WI on the M7) */
#define SCB_CCR_IC         (1u << 17)   /* L1 instruction cache enable */
#define SCB_CCR_DC         (1u << 16)   /* L1 data cache enable -- never set by this core */
#define SCB_ID_CLIDR       (*(const volatile uint32_t *)0xE000ED78u)  /* Cache Level ID */
#define SCB_ID_CTR         (*(const volatile uint32_t *)0xE000ED7Cu)  /* Cache Type */
#define SCB_ID_CCSIDR      (*(const volatile uint32_t *)0xE000ED80u)  /* Cache Size ID (of the cache CSSELR selects) */
#define SCB_ID_CSSELR      (*(volatile uint32_t *)0xE000ED84u)        /* Cache Size Selection: bit0 InD (1 = I-cache), bits3:1 level-1 */
#define SCB_CACHE_ICIALLU  (*(volatile uint32_t *)0xE000EF50u)        /* I-cache invalidate all to PoU (write-only) */
```

- [ ] **Step 2: Add the helpers and the extern after line 885 (`static inline void arm_dcache_flush_delete(...)`)**

```c
/* L1 INSTRUCTION cache -- 32 KB on the RT1176 CM7.  ResetHandler enables it
 * (NEW-36, 2026-09-05).  The sequences are ARM's (DDI0403E §B2.2; CMSIS
 * SCB_EnableICache / SCB_DisableICache): barriers on both sides of every CCR
 * change so no fetch straddles the switch; invalidate-all BEFORE enable so no
 * line from a previous run (or from the ROM) is ever hit; invalidate-all again
 * AFTER disable so a later enable starts clean.  ITCM and DTCM are TCM ports and
 * never pass through this cache -- only AXIM fetches do (FLASH XIP at
 * 0x30000000, OCRAM, SDRAM).  Nothing in the tree writes code it later
 * executes, so no caller needs arm_icache_invalidate_all() today; it exists for
 * a loader that would.  static inline: usable from C (startup.c) and C++. */
static inline void arm_icache_invalidate_all(void)
{
	__asm__ volatile("dsb" ::: "memory");
	__asm__ volatile("isb" ::: "memory");
	SCB_CACHE_ICIALLU = 0;
	__asm__ volatile("dsb" ::: "memory");
	__asm__ volatile("isb" ::: "memory");
}
static inline void arm_icache_enable(void)
{
	if (SCB_CCR & SCB_CCR_IC) return;          /* already on: nothing to invalidate */
	arm_icache_invalidate_all();
	SCB_CCR |= SCB_CCR_IC;
	__asm__ volatile("dsb" ::: "memory");
	__asm__ volatile("isb" ::: "memory");
}
static inline void arm_icache_disable(void)
{
	__asm__ volatile("dsb" ::: "memory");
	__asm__ volatile("isb" ::: "memory");
	SCB_CCR &= ~SCB_CCR_IC;
	__asm__ volatile("dsb" ::: "memory");
	__asm__ volatile("isb" ::: "memory");
	arm_icache_invalidate_all();
}
/* SCB_CCR exactly as the boot ROM handed it over -- captured by ResetHandler
 * (startup.c) immediately before its arm_icache_enable().  A reading, not an
 * inference: timing/icache_bench_hw prints it as ccr_reset=.  (A global
 * variable's symbol is not mangled by C++, so this one declaration serves
 * both the C definition and C++ sketches -- imxrt1176.h has no extern "C" block.) */
extern uint32_t startup_ccr_at_reset;
```

- [ ] **Step 3: Compile-check the header from C++ (a sketch) — expect a link error on the not-yet-defined global, nothing else**

```bash
cd ~/Development/rt1170/evkb/examples/gpio-analog/blink && cat > $SCRATCH/icache_hdr_probe.cpp <<'EOF'
#include "Arduino.h"
void setup() { Serial1.begin(115200); arm_icache_disable(); arm_icache_enable(); Serial1.println(startup_ccr_at_reset); }
void loop() {}
EOF
$ARM/arm-none-eabi-g++ -c -mcpu=cortex-m7 -mfloat-abi=hard -mfpu=fpv5-d16 -mthumb -O2 -std=gnu++14 -fno-exceptions -fno-rtti \
  -D__IMXRT1176__ -DARDUINO_MIMXRT1170_EVKB -DF_CPU=996000000 -DUSB_SERIAL -DLAYOUT_US_ENGLISH \
  -I ~/Development/teensy-cores/imxrt1176 $SCRATCH/icache_hdr_probe.cpp -o $SCRATCH/icache_hdr_probe.o && echo HEADER_OK
```

Expected: `HEADER_OK` with no warnings mentioning `arm_icache` or `SCB_`. (If the `-D` set above does not match what the macros pass and the compile fails on an unrelated pin-table macro, read the defines out of `examples/gpio-analog/blink/build/compile_commands.json` — or `build/CMakeFiles/cores.o.dir/flags.make` — and reuse them verbatim; the point is only that the new header text compiles as C++.)

- [ ] **Step 4: Compile-check from C (the core itself) — build blink's `cores` library**

```bash
cd ~/Development/rt1170/evkb/examples/gpio-analog/blink && cmake --build build 2>&1 | tail -3
```

Expected: the build completes (`[100%] Built target blink` or the `.hex` line); no error. (`startup_ccr_at_reset` is not referenced by anything yet, so its absence does not fail the link.)

No commit yet — Task 2 completes the core change and commits both files together.

---

### Task 2: Snapshot the ROM's CCR and enable the I-cache in `ResetHandler` (`startup.c`)

**Files:**
- Modify: `~/Development/teensy-cores/imxrt1176/startup.c:52` (header comment), `:145-146` (globals), `:196-198` (the insertion point after `memory_clear(&_sbss_dma, &_ebss_dma);`)

- [ ] **Step 1: Correct the header comment at line 52**

Replace
```c
 * bring-up (DCDC, USB PHY, temp sensor, external RAM, MPU/cache, printf debug)
```
with
```c
 * bring-up (USB PHY, temp sensor, MPU/D-cache, printf debug) -- the I-cache IS
 * enabled below (NEW-36, 2026-09-05); DCDC and SEMC SDRAM have since landed --
```
(the sentence continues "remains removed, to be re-added in proper RT1176 form in later phases." unchanged on the next line).

- [ ] **Step 2: Define the global after line 146 (`struct smalloc_pool extmem_smalloc_pool;`)**

```c
uint32_t startup_ccr_at_reset;           /* SCB_CCR as the boot ROM left it -- read in ResetHandler before the I-cache enable */
```

- [ ] **Step 3: Insert the snapshot + enable after line 196 (`memory_clear(&_sbss_dma, &_ebss_dma);`) and before the "Build the RAM vector table" comment**

```c
	/* L1 INSTRUCTION cache ON (NEW-36, 2026-09-05).  First a reading: what the
	 * boot ROM left in SCB_CCR.  Nothing above touches CCR, so this IS the
	 * hand-off state (timing/icache_bench_hw prints it as ccr_reset=).  It is
	 * stored after the .bss zero so memory_clear cannot erase it -- which is also
	 * why the enable sits here rather than at the top: the copy/clear loops above
	 * are three-instruction loops the FlexSPI AHB prefetch buffer already covers
	 * and their cost is data-bound, while everything that follows (the vector
	 * loop, the DCDC and PLL waits, semc_sdram_init, the constructors, main) runs
	 * cached from here on.  arm_icache_enable() invalidates before it enables, so
	 * no line from a previous run survives a warm reset.  ITCM/DTCM are TCM ports
	 * and never pass through the L1; only AXIM fetches (FLASH XIP, OCRAM, SDRAM)
	 * do -- and nothing in the tree writes code it later executes.  The D-cache
	 * stays OFF (MPU regions + a DMA-coherency review, the rt1062 lesson); every
	 * "D-cache is off" comment in this core remains true.  Measured on silicon
	 * before this: flash-resident code ran at raw QSPI speed, 5-30 us per small
	 * call (NEW-33 finding (a)); the numbers after are in
	 * examples/timing/icache_bench_hw/transcript_hw_evkb.txt. */
	startup_ccr_at_reset = SCB_CCR;
	arm_icache_enable();

```

- [ ] **Step 4: Build blink and prove the sequence was emitted into `ResetHandler`**

```bash
cd ~/Development/rt1170/evkb/examples/gpio-analog/blink && cmake --build build 2>&1 | tail -2 && \
$ARM/arm-none-eabi-nm build/blink.elf | grep -E " [BbDd] startup_ccr_at_reset$" && \
$ARM/arm-none-eabi-objdump -d build/blink.elf | awk '/<ResetHandler>:/,/<set_arm_clock_rt1176>:/' | grep -cE "0xef50|0xed14"
```

Expected: an `nm` line ending in ` B startup_ccr_at_reset` (or `b`/`d`), and a count ≥ 2 — the `movw … ; 0xef50` (ICIALLU) and `movw … ; 0xed14` (CCR) address materialisations inside `ResetHandler`. If the count is 0 the enable was optimised somewhere unexpected: check that `arm_icache_enable()` is not being called through a non-inlined copy (`nm | grep arm_icache`) and fix the placement before continuing.

- [ ] **Step 5: Boot-regression on the fastest gate**

```bash
cd ~/Development/rt1170/evkb/examples/serial/serial_test && cmake --build build 2>&1 | tail -1 && ./run_qemu.sh 2>&1 | tail -3
```

Expected: the gate's PASS line, exit 0. (This is the whole of what QEMU can say — §3 of the spec.)

- [ ] **Step 6: Commit the core change (teensy-cores, master)**

```bash
cd ~/Development/teensy-cores && git add imxrt1176/imxrt1176.h imxrt1176/startup.c && git commit -q -m "feat(imxrt1176): enable the CM7 L1 instruction cache in ResetHandler (I-cache only)

NEW-36.  The core never wrote SCB_CCR, so every flash-resident (XIP FlexSPI)
function ran at raw QSPI speed: 5-30 us per small call, the SBC encode 2-3 ms
per block vs ~145 us in ITCM, acid_box's per-loop M2Radio service chain
24.4 us/iteration (NEW-33 finding (a)).

imxrt1176.h gains the SCB cache-control surface (SCB_CCR + IC/DC/BP, CLIDR,
CTR, CCSIDR, CSSELR, ICIALLU) and three static inline helpers with the CMSIS
sequences: arm_icache_invalidate_all(), arm_icache_enable() (invalidate, then
set CCR.IC, barriers both sides), arm_icache_disable().  ResetHandler snapshots
the ROM's CCR into startup_ccr_at_reset (a reading, printed by
evkb/examples/timing/icache_bench_hw) and calls arm_icache_enable() right after
the .bss/.dmabuffers zero, so everything from the vector-table loop onward runs
cached.  No MPU is needed: the default map makes the XIP window Normal
cacheable and executable, TCM never passes through the L1, and nothing in the
tree writes code it later executes.  The D-cache stays OFF (MPU + DMA-coherency
review; every \"D-cache is off\" comment remains true).

QEMU cannot see this: qemu2 masks CCR.IC out of every write and NOPs ICIALLU,
so the gate sweep is a boot-regression check only; the measurement is silicon's
(icache_bench_hw + the acid_box LOOPSTAT A/B, recorded in evkb).

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>" && git log --oneline -1
```

Expected: a new SHA on `master`. Record it: `export CORES_NEW=$(git -C ~/Development/teensy-cores rev-parse HEAD)`.

---

### Task 3: Rebuild every rt1176 gate image against the new core, then the full sweep + audit + vacuity suite

**Why this task exists:** gates do not build. Every gate-owning build dir compiles its own copy of `cores`; an ELF built before Task 2 boots fine on the old core and would pass the sweep VACUOUSLY. The freshness check below is what makes the sweep evidence about the new startup.

**Files:** none modified (build dirs only; `$SCRATCH/*.log`).

- [ ] **Step 1: Rebuild every rt1176 `build/` (plus `pxp_draw_bench/build-32`) — background, ~1 h**

```bash
cd ~/Development/rt1170/evkb && ( for b in examples/*/*/build examples/display/pxp_draw_bench/build-32; do
  [ -f "$b/CMakeCache.txt" ] || continue
  if cmake --build "$b" -j 8 > "$SCRATCH/rebuild-$(echo $b | tr / _).log" 2>&1; then echo "OK   $b"; else echo "FAIL $b"; fi
done ) > $SCRATCH/rebuild-all.log 2>&1
```

Run with `run_in_background`. When it finishes:

```bash
grep -c "^OK" $SCRATCH/rebuild-all.log; grep "^FAIL" $SCRATCH/rebuild-all.log
```

Expected: ~118 `OK`, and `FAIL` only for `examples/usb/usb_audio_capstone_test/build` if that directory exists (it is rt1062-only by its `boards` sidecar — expected, per CLAUDE.md). Any other FAIL: open its `$SCRATCH/rebuild-…log`; a "toolchain file not found" / `COMPILERPATH is UNDEFINED` failure is the stale-build-dir class — `rm -rf` that dir and configure fresh with `cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake` from the example directory (add `-DDRAW_BENCH_32=ON` for `build-32`), then rebuild.

(`build-rt1062` dirs link the `teensy4` core, which did not change — they are not stale for this change and are not rebuilt.)

- [ ] **Step 2: Freshness check — every gate ELF must carry the new symbol**

```bash
cd ~/Development/rt1170/evkb && stale=0; for d in examples/*/*/; do n=$(basename $d); e="$d/build/$n.elf"; [ -f "$e" ] || continue; \
  $ARM/arm-none-eabi-nm "$e" | grep -q " startup_ccr_at_reset$" || { echo "STALE $e"; stale=$((stale+1)); }; done; \
  e=examples/display/pxp_draw_bench/build-32/pxp_draw_bench.elf; $ARM/arm-none-eabi-nm "$e" | grep -q " startup_ccr_at_reset$" || echo "STALE $e"; echo "stale=$stale"
```

Expected: `stale=0` and no `STALE` lines. Any STALE ELF: rebuild that directory (Step 1's command for it) and re-run this check. Do not run the sweep with a STALE line standing.

- [ ] **Step 3: The sweep — alone, output captured (run from this checkout: its path is 93 bytes, under the 104-byte `sun_path` cap)**

```bash
cd ~/Development/rt1170/evkb && ./tools/run-all-qemu-gates.sh > $SCRATCH/sweep-new36.log 2>&1; echo "exit=$?"; tail -4 $SCRATCH/sweep-new36.log
```

Run with `run_in_background` (30–60 min). Nothing else may run against the tree meanwhile — no builds, no audit, no bench flashing.

Expected: `gates: 128 passed` and `exit=0`. The ONE permitted red is `rt1176:dualcore/cm4_audio_test` (nondeterministic — re-run it idle: `cd examples/dualcore/cm4_audio_test && ./run_qemu.sh`; it must pass idle). Any other red is a regression from this change: read the gate NAME in the summary, reproduce it alone, and stop — do not touch the gate.

- [ ] **Step 4: Licence audit (after the sweep, never during) and the vacuity suite**

```bash
cd ~/Development/rt1170/evkb && LICENSE_AUDIT_EVKB=$(pwd) ./tools/license-audit.sh > $SCRATCH/audit-new36.log 2>&1; tail -2 $SCRATCH/audit-new36.log; \
./tools/gate-vacuity.test.sh > $SCRATCH/vacuity-new36.log 2>&1; echo "exit=$?"; grep -c "^PASS:" $SCRATCH/vacuity-new36.log; grep "^FAIL" $SCRATCH/vacuity-new36.log
```

Expected: `LICENSE-AUDIT: PASS`; vacuity `exit=0`, `29` PASS lines (the count recorded in CLAUDE.md for 2026-08-30; re-derive from the run, do not trust this number), no FAIL lines.

- [ ] **Step 5: Record the three results in `$SCRATCH/new36-results.md`** (a running note the docs task reads from):

```
sweep: gates: <copy the summary line> exit=<n>  (log: sweep-new36.log)
audit: <copy the PASS line>
vacuity: <n>/<n> PASS
```

No repo commit in this task.

---

### Task 4: The microbench example `examples/timing/icache_bench_hw`

**Files:**
- Create: `examples/timing/icache_bench_hw/CMakeLists.txt`
- Create: `examples/timing/icache_bench_hw/icache_bench_hw.cpp`

- [ ] **Step 1: Write `CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.24)
project(icache_bench_hw)

# TEENSY_VERSION / CPU_CORE_SPEED / COMPILERPATH are supplied by the toolchain
# file (../../../toolchain/rt1170-evkb.toolchain.cmake); fallback for a bare configure.
if(NOT DEFINED TEENSY_VERSION)
    set(TEENSY_VERSION 117 CACHE STRING "")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

# Same M2Radio manifest as audio/bt_tone_test.  Only bt/Sbc.cpp.obj is referenced
# from this sketch, so nothing else in the archive links and no IW416 firmware
# blob symbol is needed (nothing in M2Radio references one -- grep'd 2026-09-05).
import_evkb_library(M2Radio sdio iw416 hci bt)

# The reference cell.  Default OFF: Sbc.cpp.obj is routed to FLASH below so the
# bench measures the flash-resident encoder with the I-cache off and on.  ON
# leaves Sbc in ITCM (the core script's default .text placement) for the
# ITCM number -- build it in a SECOND directory (build-sbc-itcm), never
# reconfigure build/ with it, so both ELFs stay side by side.
option(ICACHE_BENCH_SBC_ITCM "Leave Sbc.cpp in ITCM (the reference cell) instead of routing it to FLASH" OFF)

teensy_add_executable(icache_bench_hw icache_bench_hw.cpp)
teensy_target_link_libraries(icache_bench_hw cores M2Radio)
target_link_libraries(icache_bench_hw.elf stdc++)
target_link_libraries(icache_bench_hw.elf m)

if(NOT ICACHE_BENCH_SBC_ITCM)
    # Route libM2Radio's Sbc.cpp.obj to FLASH (XIP): the SAME derived-linker-script
    # mechanism display/acid_box uses for its M2_BT_OUT build -- the core's
    # imxrt1176.ld is read at configure time, the one input pattern is appended
    # beside the libLVGL_flash.a rule, and ONLY this target links the copy, so it
    # never drifts from the core script and touches no other example.
    get_target_property(_icb_lf icache_bench_hw.elf LINK_FLAGS)
    string(REGEX MATCH "-T([^ ]*imxrt1176[.]ld)" _icb_ignore "${_icb_lf}")
    set(_icb_coreld "${CMAKE_MATCH_1}")
    if(_icb_coreld AND EXISTS "${_icb_coreld}")
        file(READ "${_icb_coreld}" _icb_ld)
        string(REPLACE "*libLVGL_flash.a:(.text*)"
                       "*libLVGL_flash.a:(.text*)\n\t\t*libM2Radio*.a:Sbc.cpp.obj(.text* .fastrun)"
                       _icb_ld "${_icb_ld}")
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/icache_bench_hw.ld" "${_icb_ld}")
        string(REPLACE "${_icb_coreld}"
                       "${CMAKE_CURRENT_BINARY_DIR}/icache_bench_hw.ld" _icb_lf "${_icb_lf}")
        set_target_properties(icache_bench_hw.elf PROPERTIES LINK_FLAGS "${_icb_lf}")
        message(STATUS "icache_bench_hw: Sbc.cpp.obj routed to FLASH via ${CMAKE_CURRENT_BINARY_DIR}/icache_bench_hw.ld")
    else()
        message(FATAL_ERROR "icache_bench_hw: could not find the core imxrt1176.ld in LINK_FLAGS to derive the Sbc-to-FLASH linker script (the flash cell IS the bench)")
    endif()
endif()
```

- [ ] **Step 2: Write `icache_bench_hw.cpp`**

```cpp
/* icache_bench_hw -- CM7 L1 instruction-cache microbench for the MIMXRT1170-EVKB
 * (NEW-36, 2026-09-05).  SILICON-ONLY, no QEMU gate: qemu2 masks SCB_CCR.IC out of
 * every CCR write and NOPs ICIALLU, so under QEMU every row honestly reads
 * icache=off and the cycle counts mean nothing.  It still runs there, and the
 * wit_match / sbc_crc_match lines are a real check of the bench's own logic.
 *
 * What it measures, in ONE boot, with the DWT cycle counter and IRQs masked
 * around each timed rep (millis()/micros() are DWT-based in this core, so a
 * deferred SysTick costs nothing):
 *   wl=calls  64 three-instruction functions in an unrolled direct chain --
 *             fetch/branch-bound, the "5-30 us per small call" shape NEW-33
 *             measured on flash-resident code.  Reported per call.
 *   wl=crc    table-driven CRC32 over a 4 KB DTCM buffer, table in DTCM, so the
 *             data side is TCM and the row measures instruction fetch.  Per byte.
 *   wl=sbc    the real M2Radio Sbc encoder, one 128-sample stereo frame per unit,
 *             acid_box's negotiated config (44.1 kHz, joint stereo, 16 blocks x 8
 *             subbands, loudness, bitpool 53 -> 119-byte frames).  The default
 *             build routes Sbc.cpp.obj to FLASH (CMakeLists, acid_box's derived-ld
 *             mechanism); -DICACHE_BENCH_SBC_ITCM=ON leaves it in ITCM for the
 *             reference cell.  Per frame, plus realtime_x = 2.902 ms / us per frame.
 * calls and crc are instantiated TWICE from one macro: the ITCM twin (the core
 * script's default .text placement) and the flash twin (.progmem.icb_code, which
 * imxrt1176.ld sends to .text.progmem in FLASH).  Every function under test is
 * noipa, so no twin is inlined, cloned or ICF-folded into the other.  place= is
 * DERIVED from the function's address, never asserted.
 *
 * Per cell: arm_icache_disable() -> K reps (min, median) -> arm_icache_enable()
 * (which invalidates first, so the next rep is COLD) -> cold rep -> K warm reps.
 * Every row prints the CCR.IC state it actually measured under, and a witness
 * (the workload's result, or the CRC of the last SBC frame) that must agree
 * between the off and on halves -- a cache that changed a result would be the
 * one finding worth more than any speedup.
 */
#include "Arduino.h"
#include "Sbc.h"
#include <math.h>      /* sinf/cosf for the SBC input */
#include <stddef.h>    /* ptrdiff_t in the pointer-to-member union */

#define CONSOLE Serial1

#define ICB_K        8      /* reps per half-cell */
#define ICB_N_CALLS  64     /* chain passes per rep (64 calls each) */
#define ICB_N_CRC    32     /* 4 KB passes per rep */
#define ICB_N_SBC    16     /* frames per rep */

#define ICB_NOIPA __attribute__((noipa))                       /* implies noinline, noclone, no_icf */
#define ICB_FLASH __attribute__((section(".progmem.icb_code")))
#define ICB_ITCM  /* default placement: imxrt1176.ld routes .text* to ITCM */

static volatile uint32_t icb_seed = 0x12345678u;   /* volatile source: nothing folds */
static volatile uint32_t icb_sink;                 /* volatile sink: nothing is dead */

/* ---- wl=calls: 64 tiny functions, direct unrolled chain ------------------- */
#define ICB_FN(S, A, N) \
    ICB_NOIPA A static uint32_t icb_##S##_f##N(uint32_t x) { return (x * 3u) ^ (N + 1u); }
#define ICB_CALL(S, A, N) x = icb_##S##_f##N(x);
#define ICB_X64(M, S, A) \
    M(S,A,0)  M(S,A,1)  M(S,A,2)  M(S,A,3)  M(S,A,4)  M(S,A,5)  M(S,A,6)  M(S,A,7)  \
    M(S,A,8)  M(S,A,9)  M(S,A,10) M(S,A,11) M(S,A,12) M(S,A,13) M(S,A,14) M(S,A,15) \
    M(S,A,16) M(S,A,17) M(S,A,18) M(S,A,19) M(S,A,20) M(S,A,21) M(S,A,22) M(S,A,23) \
    M(S,A,24) M(S,A,25) M(S,A,26) M(S,A,27) M(S,A,28) M(S,A,29) M(S,A,30) M(S,A,31) \
    M(S,A,32) M(S,A,33) M(S,A,34) M(S,A,35) M(S,A,36) M(S,A,37) M(S,A,38) M(S,A,39) \
    M(S,A,40) M(S,A,41) M(S,A,42) M(S,A,43) M(S,A,44) M(S,A,45) M(S,A,46) M(S,A,47) \
    M(S,A,48) M(S,A,49) M(S,A,50) M(S,A,51) M(S,A,52) M(S,A,53) M(S,A,54) M(S,A,55) \
    M(S,A,56) M(S,A,57) M(S,A,58) M(S,A,59) M(S,A,60) M(S,A,61) M(S,A,62) M(S,A,63)
#define ICB_DEFINE_CALLS(S, A) \
    ICB_X64(ICB_FN, S, A) \
    ICB_NOIPA A static uint32_t icb_##S##_calls_rep(void) { \
        uint32_t x = icb_seed; \
        for (uint32_t p = 0; p < ICB_N_CALLS; p++) { ICB_X64(ICB_CALL, S, A) } \
        return x; }
ICB_DEFINE_CALLS(itcm, ICB_ITCM)
ICB_DEFINE_CALLS(flash, ICB_FLASH)

/* ---- wl=crc: table-driven CRC32, table and buffer in DTCM ----------------- */
static uint32_t icb_crc_table[256];   /* .bss (DTCM): built at runtime so it is NOT .rodata in flash */
static uint8_t  icb_crc_buf[4096];    /* .bss (DTCM) */
#define ICB_DEFINE_CRC(S, A) \
    ICB_NOIPA A static uint32_t icb_##S##_crc32(uint32_t c, const uint8_t *p, uint32_t n) { \
        c = ~c; \
        while (n--) c = icb_crc_table[(c ^ *p++) & 0xFFu] ^ (c >> 8); \
        return ~c; } \
    ICB_NOIPA A static uint32_t icb_##S##_crc_rep(void) { \
        uint32_t c = icb_seed; \
        for (uint32_t p = 0; p < ICB_N_CRC; p++) c = icb_##S##_crc32(c, icb_crc_buf, sizeof icb_crc_buf); \
        return c; }
ICB_DEFINE_CRC(itcm, ICB_ITCM)
ICB_DEFINE_CRC(flash, ICB_FLASH)

/* ---- wl=sbc: the real encoder, wherever the linker put Sbc.cpp.obj -------- */
static Sbc      icb_sbc;
static int16_t  icb_pcm_l[128], icb_pcm_r[128];
static uint8_t  icb_sbc_out[128];        /* 119 B at bitpool 53 */
static uint16_t icb_sbc_flen;
static Sbc::Params icb_sbc_params = { Sbc::RATE_44100, Sbc::JOINT_STEREO, 16, 8, Sbc::LOUDNESS, 53 };
ICB_NOIPA static uint32_t icb_sbc_rep(void)   /* ITCM wrapper; the cost under test is Sbc::encode */
{
    uint16_t n = 0;
    for (uint32_t f = 0; f < ICB_N_SBC; f++) n = icb_sbc.encode(icb_pcm_l, icb_pcm_r, icb_sbc_out);
    icb_sbc_flen = n;
    return n;
}
static void icb_sbc_reset(void) { icb_sbc.begin(icb_sbc_params); }   /* fresh state per rep => identical frames */
static uint32_t icb_sbc_witness(void) { return icb_itcm_crc32(0, icb_sbc_out, icb_sbc_flen); }
static const void *icb_sbc_encode_addr(void)
{
    /* Non-virtual member: the Itanium ABI stores the plain code address in the
     * first word of the pointer-to-member -- read it through a union. */
    union { uint16_t (Sbc::*mf)(const int16_t *, const int16_t *, uint8_t *); struct { const void *p; ptrdiff_t adj; } r; } u;
    u.mf = &Sbc::encode;
    return u.r.p;
}

/* ---- harness ---------------------------------------------------------------- */
static uint32_t icb_time(uint32_t (*rep)(void))
{
    __disable_irq();
    uint32_t t0 = ARM_DWT_CYCCNT;
    uint32_t r = rep();
    uint32_t dt = ARM_DWT_CYCCNT - t0;
    __enable_irq();
    icb_sink = r;
    return dt;
}
static void icb_sort(uint32_t *v, int n)
{
    for (int i = 1; i < n; i++) { uint32_t x = v[i]; int j = i; while (j > 0 && v[j - 1] > x) { v[j] = v[j - 1]; j--; } v[j] = x; }
}
static bool icb_ic(void) { return (SCB_CCR & SCB_CCR_IC) != 0; }
static uint32_t icb_us(uint32_t cyc) { return (uint32_t)(((uint64_t)cyc * 1000000u) / F_CPU_ACTUAL); }
static const char *icb_place(const void *fn)
{
    uintptr_t a = (uintptr_t)fn & ~(uintptr_t)1u;            /* strip the Thumb bit */
    if (a < 0x00200000u) return "itcm";                       /* TCM window, below the boot ROM */
    if (a >= 0x30000000u && a < 0x31000000u) return "flash";  /* FlexSPI1 XIP */
    return "other";
}
static uint32_t icb_ratio10(uint32_t a, uint32_t b) { return b ? (uint32_t)(((uint64_t)a * 10u + b / 2u) / b) : 0; }   /* a/b in tenths; 64-bit: an uncached SBC rep is ~50 M cycles */

struct IcbHalf { bool ic; uint32_t min, med, cold, wit; };
struct IcbCell { IcbHalf off, on; };

static IcbHalf icb_half(uint32_t (*rep)(void), void (*reset)(void), uint32_t (*witness)(void), bool with_cold)
{
    IcbHalf h; uint32_t r[ICB_K];
    h.ic = icb_ic();                                           /* the state this half measures UNDER */
    h.cold = 0;
    if (with_cold) { if (reset) reset(); h.cold = icb_time(rep); }
    for (int k = 0; k < ICB_K; k++) { if (reset) reset(); r[k] = icb_time(rep); }
    h.wit = witness ? witness() : icb_sink;
    icb_sort(r, ICB_K);
    h.min = r[0];
    h.med = (r[ICB_K / 2 - 1] + r[ICB_K / 2]) / 2u;
    return h;
}
static IcbCell icb_cell(uint32_t (*rep)(void), void (*reset)(void), uint32_t (*witness)(void))
{
    IcbCell c;
    arm_icache_disable();
    c.off = icb_half(rep, reset, witness, false);
    arm_icache_enable();                                       /* invalidates first: the next rep is cold */
    c.on = icb_half(rep, reset, witness, true);
    return c;
}
/* One row.  Two printf calls on purpose: Print::printf clamps at 128 bytes. */
static void icb_row(const char *wl, const void *fn, const IcbHalf &h, const char *unit, uint32_t units)
{
    CONSOLE.printf("icache_bench wl=%s place=%s addr=0x%08lx icache=%s ",
                   wl, icb_place(fn), (unsigned long)((uintptr_t)fn & ~(uintptr_t)1u), h.ic ? "on" : "off");
    if (h.cold) CONSOLE.printf("cold_us/rep=%lu ", (unsigned long)icb_us(h.cold));
    CONSOLE.printf("min_cyc/%s=%lu med_cyc/%s=%lu us/rep=%lu wit=0x%08lx\n",
                   unit, (unsigned long)(h.min / units), unit, (unsigned long)(h.med / units),
                   (unsigned long)icb_us(h.min), (unsigned long)h.wit);
}
static void icb_pair(const char *wl, const void *fn, const IcbCell &c, const char *unit, uint32_t units)
{
    icb_row(wl, fn, c.off, unit, units);
    icb_row(wl, fn, c.on, unit, units);
    CONSOLE.printf("icache_bench wl=%s place=%s wit_match=%d off/on=%lu.%lux\n", wl, icb_place(fn),
                   c.off.wit == c.on.wit ? 1 : 0,
                   (unsigned long)(icb_ratio10(c.off.min, c.on.min) / 10u), (unsigned long)(icb_ratio10(c.off.min, c.on.min) % 10u));
}

static void icb_header(void)
{
    uint32_t clidr = SCB_ID_CLIDR;
    SCB_ID_CSSELR = 1u;                                        /* InD=1: instruction cache, level 1 */
    __asm__ volatile("dsb" ::: "memory"); __asm__ volatile("isb" ::: "memory");
    uint32_t cc = SCB_ID_CCSIDR;
    uint32_t line_b = 1u << ((cc & 7u) + 4u);                  /* LineSize = log2(words) - 2 */
    uint32_t ways = ((cc >> 3) & 0x3FFu) + 1u;
    uint32_t sets = ((cc >> 13) & 0x7FFFu) + 1u;
    CONSOLE.printf("icache_bench ccr_reset=0x%08lx ccr_now=0x%08lx clidr=0x%08lx ccsidr_i=0x%08lx ",
                   (unsigned long)startup_ccr_at_reset, (unsigned long)SCB_CCR, (unsigned long)clidr, (unsigned long)cc);
    CONSOLE.printf("isize_kb=%lu ways=%lu line_b=%lu hz=%lu\n",
                   (unsigned long)(sets * ways * line_b / 1024u), (unsigned long)ways, (unsigned long)line_b, (unsigned long)F_CPU_ACTUAL);
}

void setup()
{
    CONSOLE.begin(115200);
    delay(50);
    CONSOLE.println("ICACHE-BENCH v1");
    icb_header();

    /* CRC table + buffer (DTCM), deterministic LCG fill. */
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        icb_crc_table[i] = c;
    }
    uint32_t lcg = 0x2545F491u;
    for (uint32_t i = 0; i < sizeof icb_crc_buf; i++) { lcg = lcg * 1664525u + 1013904223u; icb_crc_buf[i] = (uint8_t)(lcg >> 24); }

    /* SBC input: 1 kHz at about -3 dBFS plus a 3-bit LCG dither so the bit
     * allocation is not degenerate; the right channel 90 degrees behind. */
    for (int i = 0; i < 128; i++) {
        lcg = lcg * 1664525u + 1013904223u;
        float ph = 2.0f * 3.14159265f * 1000.0f * (float)i / 44100.0f;
        icb_pcm_l[i] = (int16_t)(23000.0f * sinf(ph)) + (int16_t)((lcg >> 28) & 7u);
        icb_pcm_r[i] = (int16_t)(23000.0f * cosf(ph)) + (int16_t)((lcg >> 24) & 7u);
    }

    /* calls: ITCM twin (control: off == on, TCM is never cached) then the flash twin */
    IcbCell ci = icb_cell(icb_itcm_calls_rep, 0, 0);
    icb_pair("calls", (const void *)icb_itcm_calls_rep, ci, "call", ICB_N_CALLS * 64u);
    IcbCell cf = icb_cell(icb_flash_calls_rep, 0, 0);
    icb_pair("calls", (const void *)icb_flash_calls_rep, cf, "call", ICB_N_CALLS * 64u);

    /* crc */
    IcbCell ri = icb_cell(icb_itcm_crc_rep, 0, 0);
    icb_pair("crc", (const void *)icb_itcm_crc_rep, ri, "byte", ICB_N_CRC * (uint32_t)sizeof icb_crc_buf);
    IcbCell rf = icb_cell(icb_flash_crc_rep, 0, 0);
    icb_pair("crc", (const void *)icb_flash_crc_rep, rf, "byte", ICB_N_CRC * (uint32_t)sizeof icb_crc_buf);

    /* sbc: placement is per build (see CMakeLists); the address decides the label */
    const void *enc = icb_sbc_encode_addr();
    IcbCell s = icb_cell(icb_sbc_rep, icb_sbc_reset, icb_sbc_witness);
    icb_pair("sbc", enc, s, "frame", ICB_N_SBC);
    uint32_t us_off = icb_us(s.off.min) / ICB_N_SBC, us_on = icb_us(s.on.min) / ICB_N_SBC;
    CONSOLE.printf("icache_bench wl=sbc place=%s flen=%u us/frame_off=%lu us/frame_on=%lu ",
                   icb_place(enc), (unsigned)icb_sbc_flen, (unsigned long)us_off, (unsigned long)us_on);
    CONSOLE.printf("realtime_x_off=%lu.%lu realtime_x_on=%lu.%lu sbc_crc=0x%08lx sbc_crc_match=%d\n",
                   (unsigned long)(icb_ratio10(2902u, us_off) / 10u), (unsigned long)(icb_ratio10(2902u, us_off) % 10u),
                   (unsigned long)(icb_ratio10(2902u, us_on) / 10u),  (unsigned long)(icb_ratio10(2902u, us_on) % 10u),
                   (unsigned long)s.on.wit, s.off.wit == s.on.wit ? 1 : 0);

    /* summary: flash off/on speedups, and how close cached flash gets to ITCM */
    CONSOLE.printf("icache_bench summary calls=%lu.%lux crc=%lu.%lux sbc=%lu.%lux ",
                   (unsigned long)(icb_ratio10(cf.off.min, cf.on.min) / 10u), (unsigned long)(icb_ratio10(cf.off.min, cf.on.min) % 10u),
                   (unsigned long)(icb_ratio10(rf.off.min, rf.on.min) / 10u), (unsigned long)(icb_ratio10(rf.off.min, rf.on.min) % 10u),
                   (unsigned long)(icb_ratio10(s.off.min, s.on.min) / 10u),   (unsigned long)(icb_ratio10(s.off.min, s.on.min) % 10u));
    CONSOLE.printf("flash_on_vs_itcm: calls=%lu.%lux crc=%lu.%lux\n",
                   (unsigned long)(icb_ratio10(cf.on.min, ci.on.min) / 10u), (unsigned long)(icb_ratio10(cf.on.min, ci.on.min) % 10u),
                   (unsigned long)(icb_ratio10(rf.on.min, ri.on.min) / 10u), (unsigned long)(icb_ratio10(rf.on.min, ri.on.min) % 10u));

    arm_icache_enable();                                       /* leave the core in its default state */
    CONSOLE.println("ICACHE-BENCH done");
}

void loop()
{
    static uint32_t n = 0;
    delay(1000);
    CONSOLE.printf("hb n=%lu icache=%s\n", (unsigned long)++n, icb_ic() ? "on" : "off");
}
```

- [ ] **Step 3: Configure and build both variants**

```bash
cd ~/Development/rt1170/evkb/examples/timing/icache_bench_hw && \
cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake 2>&1 | grep -E "icache_bench_hw:|Error|error" ; cmake --build build 2>&1 | grep -E "error|warning: .*icb|Built target|hex" ; \
cmake -B build-sbc-itcm -DICACHE_BENCH_SBC_ITCM=ON -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake 2>&1 | grep -E "icache_bench_hw:|Error|error" ; cmake --build build-sbc-itcm 2>&1 | grep -E "error|Built target|hex"
```

Expected: the first configure prints `icache_bench_hw: Sbc.cpp.obj routed to FLASH via …/build/icache_bench_hw.ld`; the second prints no such line; both builds complete with no `error` and no warning naming `noipa` (supported since GCC 8), `icb_`, or `Sbc`. The pointer-to-member union read is plain C++ (no `-Wpmf-conversions` extension involved); if GCC 10 warns about it, silence that one warning at the union with `#pragma GCC diagnostic ignored "-Wstrict-aliasing"` rather than changing the technique.

- [ ] **Step 4: Link-layout witnesses — prove the routing from the ELF before trusting any `place=`**

```bash
cd ~/Development/rt1170/evkb/examples/timing/icache_bench_hw && \
echo "--- default build (Sbc in FLASH expected):"; $ARM/arm-none-eabi-nm -C build/icache_bench_hw.elf | grep -E "Sbc::encode|icb_(itcm|flash)_calls_rep|icb_(itcm|flash)_crc_rep" ; \
echo "--- build-sbc-itcm (Sbc in ITCM expected):"; $ARM/arm-none-eabi-nm -C build-sbc-itcm/icache_bench_hw.elf | grep -E "Sbc::encode"
```

Expected: in `build/`, `Sbc::encode(...)` at `3xxxxxxx` and `icb_flash_*` at `3xxxxxxx`, `icb_itcm_*` at `0000xxxx`; in `build-sbc-itcm/`, `Sbc::encode` at `0000xxxx`. If `Sbc::encode` is at `0000…` in the default build, the derived script's input pattern did not match the archive member name — check `$ARM/arm-none-eabi-ar t build/libM2Radio.o.a | grep Sbc` and adjust the pattern in CMakeLists to the printed member name.

- [ ] **Step 5: QEMU smoke — the bench's own logic (rows print, witnesses agree, no crash)**

```bash
cd ~/Development/rt1170/evkb/examples/timing/icache_bench_hw && gtimeout 20 ../../../tools/rt1170-qemu.sh build/icache_bench_hw.elf < /dev/null > $SCRATCH/icb-qemu.log 2>&1; grep -E "^icache_bench|ICACHE-BENCH|^hb" $SCRATCH/icb-qemu.log | head -30
```

Expected: `ICACHE-BENCH v1`, the header line with `ccr_reset=0x00000200`-ish and `icache=off` in EVERY row (qemu2 drops the IC bit — this is the honest reading), every `wit_match=1`, `sbc_crc_match=1`, `flen=119`, `ICACHE-BENCH done`, then `hb` lines. If `rt1170-qemu.sh` needs a tty for `-serial mon:stdio` and prints nothing under redirection, run its `qemu-system-arm` line by hand with `-serial file:$SCRATCH/icb-qemu.log` in place of `-serial mon:stdio` (read the script for the machine flags; do not guess them).

Any `wit_match=0` or `sbc_crc_match=0` here is a bench defect (a workload reading something the harness changes between halves) and must be fixed before silicon.

- [ ] **Step 6: Commit the example (evkb branch)**

```bash
cd ~/Development/rt1170/evkb && git add examples/timing/icache_bench_hw/CMakeLists.txt examples/timing/icache_bench_hw/icache_bench_hw.cpp && git commit -q -m "feat(timing): icache_bench_hw -- CM7 L1 I-cache microbench (silicon-only, no gate)

NEW-36.  One boot A/Bs the I-cache over three workloads: calls (64 tiny
functions in an unrolled chain), crc (table-driven CRC32, table and buffer in
DTCM) and the real M2Radio Sbc encoder at acid_box's negotiated config.  calls
and crc are instantiated twice from one macro (ITCM twin / .progmem.icb_code
flash twin, all noipa); Sbc.cpp.obj is routed to FLASH by a linker script
derived from the core's imxrt1176.ld (acid_box's mechanism), or left in ITCM
with -DICACHE_BENCH_SBC_ITCM=ON for the reference cell.  place= is derived from
each function's address; every row prints the CCR.IC state it measured under
and a witness that must agree between the off and on halves.  Prints the ROM's
CCR (startup_ccr_at_reset) and the CCSIDR decode (isize_kb) first.

No QEMU gate: qemu2 masks CCR.IC and NOPs ICIALLU, so every row reads
icache=off there and the timings mean nothing; the wit_match/sbc_crc_match
lines were checked under QEMU as a test of the bench's own logic.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 5: Silicon — run the bench, record two boots + the ITCM reference build

**Files:**
- Create: `examples/timing/icache_bench_hw/transcript_hw_evkb.txt`
- Create: `examples/timing/icache_bench_hw/README.md`

Bench hygiene (CLAUDE.md): no VCOM reader may be attached while LinkServer programs; `flash load` → `flash verify` → attach the reader → the human presses SW4. If `flash load` refuses the `.elf` with `Flash operation exited with code -11`, load `build/icache_bench_hw.hex` instead. A DAP wedge (`DAPInfo`/`Wire not connected` while the VCOM still enumerates) means replug the DEBUG USB.

- [ ] **Step 1: Flash the default build**

```bash
cd ~/Development/rt1170/evkb/examples/timing/icache_bench_hw && pkill LinkServer; pkill redlinkserv; pkill crt_emu_cm_redlink; sleep 1; \
/Applications/LinkServer_26.6.137/LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load build/icache_bench_hw.elf 2>&1 | tail -3 && \
/Applications/LinkServer_26.6.137/LinkServer flash MIMXRT1176:MIMXRT1170-EVKB verify build/icache_bench_hw.elf 2>&1 | tail -2
```

Expected: a `… bytes written` line, then `File matches flash` (or the tool's equivalent) on verify.

- [ ] **Step 2: Attach the console reader (background), then ask the user to press SW4 twice, ~15 s apart**

```bash
cd ~/Development/rt1170/evkb && /usr/local/Caskroom/miniconda/base/bin/python3 tools/rt1170-console.py /dev/cu.usbmodem5DQ2DDHVWO5EI3 115200 > $SCRATCH/icb-hw-default.log 2>&1
```

Run with `run_in_background`; tell the user: "reader attached — press SW4, wait for `ICACHE-BENCH done`, press SW4 again". Then:

```bash
grep -cE "^ICACHE-BENCH done" $SCRATCH/icb-hw-default.log; grep -E "^icache_bench (ccr_reset|summary|wl=sbc place=.* flen)" $SCRATCH/icb-hw-default.log
```

Expected: `2`; two header lines with `ccr_reset=0x…` (predicted `IC`=bit 17 clear), `isize_kb=32 ways=2 line_b=32`, `ccr_now` with bit 17 set; two summary pairs; `flen=119`, `sbc_crc_match=1`, and every `wit_match=1`. Compare the two boots' `min_cyc` rows: they should agree within a few percent (min is robust); if any row moves by more than 10 % between boots, boot a third time before writing anything down.

Then stop the reader (`pkill -f rt1170-console.py`).

- [ ] **Step 3: Flash and capture the ITCM reference build (one boot)**

```bash
cd ~/Development/rt1170/evkb/examples/timing/icache_bench_hw && pkill -f rt1170-console.py; pkill LinkServer; pkill redlinkserv; pkill crt_emu_cm_redlink; sleep 1; \
/Applications/LinkServer_26.6.137/LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load build-sbc-itcm/icache_bench_hw.elf 2>&1 | tail -2 && \
/Applications/LinkServer_26.6.137/LinkServer flash MIMXRT1176:MIMXRT1170-EVKB verify build-sbc-itcm/icache_bench_hw.elf 2>&1 | tail -1
```

Then the reader to `$SCRATCH/icb-hw-sbc-itcm.log` (same command as Step 2, different log), one SW4 press. Expected: `wl=sbc place=itcm`, off and on rows within a few percent of each other (TCM is not cached — the control), `sbc_crc=` IDENTICAL to the default build's value (same input, same code, different placement), `sbc_crc_match=1`.

- [ ] **Step 4: Write `transcript_hw_evkb.txt`** — header block, then the raw captures verbatim:

```
icache_bench_hw -- HARDWARE, MIMXRT1170-EVKB (NEW-36)
================================================================================
2026-09-05.  teensy-cores <CORES_NEW SHA> (I-cache enabled in ResetHandler),
M2Radio 7c91cca.  Procedure: flash load -> verify -> console reader attached
-> SW4.  Default build (Sbc.cpp.obj routed to FLASH): TWO boots.  build-sbc-itcm
(-DICACHE_BENCH_SBC_ITCM=ON): ONE boot, the reference cell.

READINGS (fill from the rows below, do not round):
  ccr_reset=<value>  -> the boot ROM hands over with IC=<0|1>, DC=<0|1>
  isize_kb=<n> ways=<n> line_b=<n>  (RT1176 datasheet: 32 KB, 2-way, 32 B)
  calls  flash off->on <x>x ; cached flash vs ITCM <x>x   (ITCM off==on within <n>%: the TCM control)
  crc    flash off->on <x>x ; cached flash vs ITCM <x>x
  sbc    flash off->on <x>x ; us/frame off=<n> on=<n> ; ITCM=<n> ; realtime_x on=<x>
  witnesses: every wit_match=1, sbc_crc=<value> identical across both builds, sbc_crc_match=1
PREDICTIONS (spec §"What the code predicts") -- HELD / REFUTED, one line each:
  ROM leaves IC=0: <held|refuted>
  32 KB 2-way 32 B: <held|refuted>
  calls >= 10x, cached flash within ~1.5x of ITCM: <held|refuted, with the number>
  sbc within ~2x of ITCM, well inside 2.902 ms: <held|refuted, with the number>

--- default build, boot 1 ---
<paste $SCRATCH/icb-hw-default.log from ICACHE-BENCH v1 through the first two hb lines>
--- default build, boot 2 ---
<same for the second boot>
--- build-sbc-itcm, boot 1 ---
<paste $SCRATCH/icb-hw-sbc-itcm.log likewise>
```

Every `<…>` above is a value copied from the named log line — the transcript must contain no angle brackets when committed.

- [ ] **Step 5: Write `README.md`** (short; the transcript is the evidence):

```markdown
# icache_bench_hw — CM7 L1 I-cache microbench (silicon-only)

NEW-36. The `imxrt1176` core enables the Cortex-M7 L1 instruction cache in
`ResetHandler` since teensy-cores `<CORES_NEW short SHA>`; before that every
flash-resident (XIP) function ran at raw QSPI speed (NEW-33 finding (a)). This
bench A/Bs the cache in ONE boot over three workloads, each in ITCM and in
FLASH, and prints the boot ROM's `SCB_CCR` and the `CCSIDR` decode first.

| workload | shape | ITCM | flash, I-cache off | flash, I-cache on |
|---|---|---|---|---|
| `calls` (cyc/call) | 64 tiny functions, unrolled direct chain | <n> | <n> | <n> (cold rep <n> µs) |
| `crc` (cyc/byte) | table-driven CRC32, table+buffer in DTCM | <n> | <n> | <n> |
| `sbc` (µs/frame) | M2Radio `Sbc::encode`, 44.1 kHz joint 16×8 bitpool 53 | <n> | <n> | <n> (`realtime_x` <x>) |

Readings: `ccr_reset=<value>` (ROM: IC=<0|1>), `isize_kb=<n> ways=<n> line_b=<n>`.
Witnesses: every `wit_match=1`, `sbc_crc` identical across both builds.

## No QEMU gate, on purpose
qemu2 masks `CCR.IC` out of every write and NOPs `ICIALLU`, so every row reads
`icache=off` there and the cycle counts mean nothing. The `wit_match` /
`sbc_crc_match` lines are checked under QEMU as a test of the bench's own logic
(`../../../tools/rt1170-qemu.sh build/icache_bench_hw.elf`).

## Build / run
```sh
cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake && cmake --build build
cmake -B build-sbc-itcm -DICACHE_BENCH_SBC_ITCM=ON -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake && cmake --build build-sbc-itcm
```
`build/` routes `Sbc.cpp.obj` to FLASH (a linker script derived from the core's
`imxrt1176.ld`, acid_box's mechanism); `build-sbc-itcm/` leaves it in ITCM for
the reference cell. Flash with LinkServer (`flash … load` → `verify`), attach
`tools/rt1170-console.py`, press SW4. Full captures: `transcript_hw_evkb.txt`.

## Follow-on trigger (spec §8)
If the cached flash-resident `sbc` cell shows `realtime_x ≥ 5`, acid_box's
`M2_BT_OUT` ITCM routing can be reconsidered — its own issue, with this bench as
input. Result: <realtime_x on = x → filed NEW-nn | not met, routing stays>.
```

Fill every `<…>` from the transcript before committing.

- [ ] **Step 6: Commit**

```bash
cd ~/Development/rt1170/evkb && git add examples/timing/icache_bench_hw/README.md examples/timing/icache_bench_hw/transcript_hw_evkb.txt && git commit -q -m "docs(timing): icache_bench_hw silicon transcript -- two boots + ITCM reference, predictions dispositioned

NEW-36.  <one line: ccr_reset reading, isize_kb, the three speedups, sbc us/frame off/on/itcm, realtime_x>

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 6: acid_box LOOPSTAT A/B — the core before vs after, same instrument

**Files:**
- Modify: `examples/display/acid_box/transcript_hw_evkb_bt.txt` (append)

The bench configuration is NEW-33's, byte for byte (`transcript_hw_evkb_bt.txt` lines 4-7 + `-DACIDBOX_LOOPSTAT=ON`). Two build directories, one per core commit; the local-first library resolution means the `teensy-cores` checkout state is what compiles.

- [ ] **Step 1: Build `build-bench-pre` against the pre-change core**

```bash
cd ~/Development/teensy-cores && git checkout -q 672c577 && git log --oneline -1 && \
cd ~/Development/rt1170/evkb/examples/display/acid_box && rm -rf build-bench-pre && \
cmake -B build-bench-pre -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake \
  -DM2_BT_OUT=ON -DM2_BT_TARGET_NAME=Shokz -DM2_BT_RTS_FLOW=ON \
  -DM2_BT_FAST_BAUD=ON -DM2_BT_FAST_BAUD_RATE=3000000 -DM2_BT_LEGACY_PIN=OFF -DACIDBOX_LOOPSTAT=ON \
  -DM2RADIO_IW416_BT_FW=$HOME/Development/mcuxsdk-ws/components/conn_fwloader/fw_bin/inc/IW416/uartIW416_bt.bin.inc 2>&1 | grep -E "IW416 firmware|Error" && \
cmake --build build-bench-pre -j 8 2>&1 | grep -E "error|Built target|hex" && \
$ARM/arm-none-eabi-nm build-bench-pre/acid_box.elf | grep -c " startup_ccr_at_reset$"
```

Expected: `IW416 firmware: …uartIW416_bt.bin.inc`, a clean build, and `0` from the final grep (the PRE image has no such symbol — the witness that it is the old core).

- [ ] **Step 2: Build `build-bench-post` against the new core**

```bash
cd ~/Development/teensy-cores && git checkout -q master && git log --oneline -1 && \
cd ~/Development/rt1170/evkb/examples/display/acid_box && rm -rf build-bench-post && \
cmake -B build-bench-post -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake \
  -DM2_BT_OUT=ON -DM2_BT_TARGET_NAME=Shokz -DM2_BT_RTS_FLOW=ON \
  -DM2_BT_FAST_BAUD=ON -DM2_BT_FAST_BAUD_RATE=3000000 -DM2_BT_LEGACY_PIN=OFF -DACIDBOX_LOOPSTAT=ON \
  -DM2RADIO_IW416_BT_FW=$HOME/Development/mcuxsdk-ws/components/conn_fwloader/fw_bin/inc/IW416/uartIW416_bt.bin.inc 2>&1 | grep -E "IW416 firmware|Error" && \
cmake --build build-bench-post -j 8 2>&1 | grep -E "error|Built target|hex" && \
$ARM/arm-none-eabi-nm build-bench-post/acid_box.elf | grep -c " startup_ccr_at_reset$"
```

Expected: `master` is `$CORES_NEW`; `1` from the final grep. The core repo MUST be back on `master` before anything else is built.

- [ ] **Step 3: Run PRE (human at the panel; headset on and in range)**

Flash `build-bench-pre/acid_box.elf` (Task 5 Step 1's LinkServer commands with this path), attach the reader to `$SCRATCH/acidbox-ab-pre.log`, then ask the user to follow NEW-33's protocol: **SW4 → wait for `bt_streaming` → tap PLAY → tap the "ACID BOX" title (wiggle=1) ~20 s → tap it again (wiggle=0) → let it stream ≥ 60 s.** Stop the reader. Then:

```bash
grep -E "^loopstat|^framestat|^touchstat|bt_streaming|bt_fw_dnld|connect" $SCRATCH/acidbox-ab-pre.log | head -60
```

Expected: `loopstat` lines in both `wiggle=1` and `wiggle=0` states, `pcmdrops=0` on the heartbeat, and the two bring-up phases' durations visible (the `bt_fw_dnld`/download and `connect` lines with their `ms=` or timestamped neighbours).

- [ ] **Step 4: Run POST** — the same, with `build-bench-post/acid_box.elf` and `$SCRATCH/acidbox-ab-post.log`.

- [ ] **Step 5: Reduce both logs to per-second numbers and append the A/B block**

For each log take the last four `wiggle=0` `loopstat` lines and the last four `wiggle=1` ones; per line, `svc_us_per_iter = svc / loops`, `print_us_per_iter = print / loops`, `enc_us_per_block = enc / 345` (the encoder runs 44100/128 ≈ 345 blocks/s while streaming). Append to `examples/display/acid_box/transcript_hw_evkb_bt.txt`:

```
================================================================================
NEW-36 I-CACHE A/B, 2026-09-05 -- the core is the only variable
Spec: docs/superpowers/specs/2026-09-05-cm7-icache-enable-design.md
Same bench config as NEW-33 (lines 4-7 above) + -DACIDBOX_LOOPSTAT=ON, M2Radio 7c91cca.
  PRE  = build-bench-pre:  teensy-cores 672c577 (no I-cache)   nm: startup_ccr_at_reset absent
  POST = build-bench-post: teensy-cores <CORES_NEW>  (I-cache)  nm: startup_ccr_at_reset present
Protocol per build: SW4 -> bt_streaming -> PLAY -> title tap (wiggle=1) ~20 s -> tap (wiggle=0) -> stream >= 60 s.

PREDICTIONS (written before the run):
  svc us/iter     24.4 -> single digits
  print us/iter   ~32  -> ~5
  enc us/block    unchanged (ITCM)
  pcmdrops        0 held; frame interval / touch p95 within noise (vsync-bound)

                              PRE                 POST
  loop it/s (wiggle=0)        <n>                 <n>
  svc  us/s  | us/iter        <n> | <n>           <n> | <n>
  print us/s | us/iter        <n> | <n>           <n> | <n>
  poll us/s  | enc us/block   <n> | <n>           <n> | <n>
  max_us                      <n>                 <n>
  frame med_us (wiggle=1)     <n>                 <n>
  touch p95 us                <n>                 <n>
  pcmdrops                    <n>                 <n>
  fw download / connect (s)   <n> / <n>           <n> / <n>
VERDICT per prediction: <held|refuted, number>

--- PRE loopstat (last 4 wiggle=1, last 4 wiggle=0) ---
<lines>
--- POST loopstat (last 4 wiggle=1, last 4 wiggle=0) ---
<lines>
```

No `<…>` may remain when committed.

- [ ] **Step 6: Commit**

```bash
cd ~/Development/rt1170/evkb && git add examples/display/acid_box/transcript_hw_evkb_bt.txt && git commit -q -m "docs(acid_box): NEW-36 I-cache A/B on silicon -- LOOPSTAT before/after the core change

<one line: svc us/iter pre->post, print pre->post, enc unchanged?, pcmdrops, fw download/connect seconds>

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 7: Silicon regression — acid_box default golden + `cm4_audio_test`

**Files:**
- Modify: `examples/display/acid_box/transcript_hw_evkb.txt` (append)
- Modify: `examples/dualcore/cm4_audio_test/transcript_hw_evkb.txt` (append)

Both ELFs were rebuilt against the new core in Task 3 Step 1 (confirm: `nm … | grep startup_ccr_at_reset` on each before flashing).

- [ ] **Step 1: acid_box default build — two boots + play**

Flash `examples/display/acid_box/build/acid_box.elf` (LinkServer load → verify), attach the reader to `$SCRATCH/acidbox-reg.log`, ask the user: **SW4 → wait for `ACIDBOX_DONE` → SW4 again → wait → tap PLAY → play ~30 s → drag CUTOFF.** Then:

```bash
grep -E "ACIDBOX_ENGINE|ACIDBOX_UI_SUM|ACIDBOX_GPU_ERR|ACIDBOX_VSYNC" $SCRATCH/acidbox-reg.log | head -12; grep -c "timeouts=0" $SCRATCH/acidbox-reg.log; grep -cE "timeouts=[1-9]" $SCRATCH/acidbox-reg.log
```

Expected: `ACIDBOX_ENGINE=gpu`, **`ACIDBOX_UI_SUM=0x1479CEE8`** on BOTH boots, `ACIDBOX_GPU_ERR=0`, every `ACIDBOX_VSYNC` line `timeouts=0` (the last grep prints `0`), per-step RMS lines under PLAY, and audio audible by ear (ask the user to confirm). A different `UI_SUM` on either boot stops the work: the I-cache is not transparent for that path and the cause must be found before anything ships.

- [ ] **Step 2: `cm4_audio_test` — one boot**

Flash `examples/dualcore/cm4_audio_test/build/cm4_audio_test.elf` (load → verify), reader to `$SCRATCH/cm4audio-reg.log`, one SW4 press. Then:

```bash
grep -E "CM4AUDIO-GATE|codec_ack|underruns|rx_overflows|AUDIO_CM4_DET|AUDIO_CM4=|CM4AUDIO-DONE" $SCRATCH/cm4audio-reg.log
```

Expected: `CM4AUDIO-GATE v1`, `codec_ack=00000001`, `underruns=00000000`, `rx_overflows=00000000`, `AUDIO_CM4_DET=PASS`, `AUDIO_CM4=PASS`, `CM4AUDIO-DONE` — the same tokens the file's existing transcript records; ~1 kHz audible on J101 (ask the user).

- [ ] **Step 3: Append both regression blocks**

To `examples/display/acid_box/transcript_hw_evkb.txt` (top of file, above the 2026-08-28 block, matching its style):

```
================================================================================
2026-09-05, NEW-36 I-CACHE REGRESSION (teensy-cores <CORES_NEW>: the CM7 L1
I-cache is now enabled in ResetHandler).  Default build, flashed, TWO SW4 boots
with a persistent reader, then PLAY + CUTOFF drag:
  ACIDBOX_ENGINE=gpu   ACIDBOX_UI_SUM=0x1479CEE8 (both boots -- BIT-IDENTICAL to
  the 2026-08-28 golden)   ACIDBOX_GPU_ERR=0   ACIDBOX_VSYNC timeouts=0 on every
  line (<n> lines), audio audible, RMS table following the steps.
A cache cannot change a picture; an identical checksum is the "nothing moved"
witness for display + GC355 compositor + touch + SAI/DMA audio + SD + I2C.
<paste the grep output from Step 1>
```

To `examples/dualcore/cm4_audio_test/transcript_hw_evkb.txt` (append at the end):

```
================================================================================
2026-09-05, NEW-36 I-CACHE REGRESSION (teensy-cores <CORES_NEW>): the CM7 stages,
boots and talks MU to the CM4 from a cached core.  One SW4 boot, same tokens:
<paste the grep output from Step 2>
```

- [ ] **Step 4: Commit**

```bash
cd ~/Development/rt1170/evkb && git add examples/display/acid_box/transcript_hw_evkb.txt examples/dualcore/cm4_audio_test/transcript_hw_evkb.txt && git commit -q -m "docs: NEW-36 silicon regression -- acid_box golden 0x1479CEE8 bit-identical (2 boots) + cm4_audio_test PASS on the cached core

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 8: Push the core, bump the pin, verify the fresh-user path

**Files:**
- Modify: `evkb.cmake:110` (the `cores` pin line)

- [ ] **Step 1: Push teensy-cores master**

```bash
cd ~/Development/teensy-cores && git status --short | head -3; git push origin master 2>&1 | tail -2 && git rev-parse HEAD
```

Expected: a clean tree, the push accepted, the SHA equals `$CORES_NEW`.

- [ ] **Step 2: Bump the pin in `evkb.cmake` line 110**

Replace `672c577402f42f86a7c1ae3a52a5f999b9ec67f3` with the full `$CORES_NEW` SHA, and append to the END of that line's `#` comment:

```
 2026-09-05 (NEW-36): ResetHandler enables the CM7 L1 INSTRUCTION cache (arm_icache_enable(), after the .bss zero; startup_ccr_at_reset records the ROM's CCR) and imxrt1176.h gains the SCB cache-control surface + arm_icache_{enable,disable,invalidate_all}(). D-cache still OFF. Changes EVERY rt1176 ELF's startup; every gate re-swept 128/0/0 against rebuilt images; silicon: timing/icache_bench_hw + acid_box golden 0x1479CEE8 bit-identical + cm4_audio_test PASS. The variable is not mangled, so C++ sketches see it with no extern "C".
```

```bash
cd ~/Development/rt1170/evkb && grep -c "$CORES_NEW" evkb.cmake && grep -c 672c577402f42f86a7c1ae3a52a5f999b9ec67f3 evkb.cmake
```

Expected: `1` then `0`.

- [ ] **Step 3: Fresh-user build of a gate-owning example at the new pin, and RUN its gate against the fetched ELF**

```bash
cd ~/Development/rt1170/evkb/examples/serial/serial_test && rm -rf $SCRATCH/ff-serial && \
cmake -B $SCRATCH/ff-serial -DEVKB_FORCE_FETCH=ON -DCMAKE_TOOLCHAIN_FILE=$(pwd)/../../../toolchain/rt1170-evkb.toolchain.cmake > $SCRATCH/ff-serial-configure.log 2>&1; echo "configure exit=$?"; \
grep -iE "teensy-cores|already at requested ref|git clone" $SCRATCH/ff-serial-configure.log | head -4; \
cmake --build $SCRATCH/ff-serial 2>&1 | tail -1 && $ARM/arm-none-eabi-nm $SCRATCH/ff-serial/serial_test.elf | grep -c " startup_ccr_at_reset$"
```

Expected: `configure exit=0`, a clone/checkout line naming `teensy-cores` at `$CORES_NEW`, a built ELF, and `1` (the fetched core carries the change). Then run the gate against it:

```bash
cd ~/Development/rt1170/evkb/examples/serial/serial_test && mv build build.keep && ln -s $SCRATCH/ff-serial build && ./run_qemu.sh 2>&1 | tail -2; rm build && mv build.keep build && ls -ld build
```

Expected: the gate's PASS line, and `build` restored as a real directory. (A configure proves the subdir resolves; only the gate run proves the fetched code behaves.)

- [ ] **Step 4: Commit the pin**

```bash
cd ~/Development/rt1170/evkb && git add evkb.cmake && git commit -q -m "build: bump cores pin to $CORES_NEW (CM7 L1 I-cache enabled in ResetHandler, NEW-36)

Fresh-user verified: -DEVKB_FORCE_FETCH=ON cloned teensy-cores at the new pin,
the fetched serial_test ELF carries startup_ccr_at_reset, and its gate PASSED
against that ELF.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 9: Documentation, follow-on decision, Linear close-out, memory

**Files:**
- Modify: `CLAUDE.md:602` (insert a block before the 2026-09-05 NEW-33 block), `:1581-1583` (the cache paragraph), and the NEW-33 block's finding (a) sentence
- Modify: `examples/README.md:47`
- Modify: `examples/display/acid_box/CMakeLists.txt:150-151`

- [ ] **Step 1: CLAUDE.md — the Architecture cache paragraph (line 1581-1583)**

Replace
```
  ★ **The two cores also differ on the D-cache, and DMA correctness depends on
  it.** `teensy4` enables it (`startup.c`, `SCB_CCR_IC | SCB_CCR_DC`);
  `imxrt1176` never writes `SCB_CCR` at all, so OCRAM is coherent there
  for free. On rt1062 it is not: a DMAMEM buffer is cached write-back unless the
```
with
```
  ★ **The two cores also differ on the D-cache, and DMA correctness depends on
  it.** `teensy4` enables it (`startup.c`, `SCB_CCR_IC | SCB_CCR_DC`);
  `imxrt1176` sets ONLY `SCB_CCR_IC` (the L1 INSTRUCTION cache, NEW-36,
  2026-09-05 — `arm_icache_enable()` in `ResetHandler`, no MPU needed) and never
  `SCB_CCR_DC`, so OCRAM is coherent there for free. Until NEW-36 it wrote
  `SCB_CCR` not at all, and every flash-resident (XIP) function ran at raw QSPI
  speed — 5–30 µs per small call; `timing/icache_bench_hw` has the before/after
  and the ROM's own `CCR` reading. On rt1062 it is not: a DMAMEM buffer is cached write-back unless the
```

- [ ] **Step 2: CLAUDE.md — retire NEW-33 finding (a)**

In the 2026-09-05 NEW-33 block, the sentence beginning `(a) The CM7 runs with NO instruction cache -- the imxrt1176 core's startup never writes \`SCB_CCR\`` … `A core change with its own issue.` — append after "its own issue.":

```
 **DONE as NEW-36 the same day** (block above): the I-cache is enabled, and the numbers are the bench's, not this estimate's.
```

- [ ] **Step 3: CLAUDE.md — the measurement block, inserted before line 602 (the existing `✅ **Measured 2026-09-05: 128 gates …` NEW-33 block)**

```
✅ **Measured 2026-09-05 (later the same day): 128 gates discovered, 128 passed,
0 failed, 0 SKIP** (`gates: 128 passed`, exit 0; `-l` reports 128), on the
**NEW-36 CM7 L1 I-CACHE** close-out — the `imxrt1176` core now enables the
instruction cache in `ResetHandler` (`teensy-cores` `<CORES_NEW short>`, pin
bumped, fresh-user `-DEVKB_FORCE_FETCH=ON` verified by RUNNING `serial_test`'s
gate on the fetched ELF). **No new gate**: the new `timing/icache_bench_hw` is a
silicon-only `_hw` example, because **qemu2 masks `CCR.IC` out of every write
and NOPs `ICIALLU`** (`hw/intc/armv7m_nvic.c`) — every row reads `icache=off`
there and the sweep is a boot-regression check of the new startup, nothing more.
`LICENSE-AUDIT: PASS`; vacuity <n>/<n>.
★ **Every gate image was REBUILT before the sweep and checked for freshness by
symbol** (`nm … startup_ccr_at_reset`, <n> ELFs, 0 stale): gates do not build,
and an ELF from the old core boots fine, so an unrebuilt sweep would have
passed VACUOUSLY and said nothing about the change.
★ **The ROM hands over with the I-cache <off|on>** — READ, not inferred:
`ccr_reset=<value>` from `startup_ccr_at_reset`. `CCSIDR`: <n> KB, <n>-way,
<n>-B lines. The bench (two boots, min-of-8): flash-resident `calls`
<n>→<n> cyc/call (<x>×; cached flash <x>× ITCM), `crc` <n>→<n> cyc/byte (<x>×),
the real `Sbc` encoder **<n>→<n> µs/frame (<x>×; ITCM <n>; `realtime_x`
<x>)**, every `wit_match=1` and `sbc_crc` identical across both builds — the
cache changed no result. acid_box LOOPSTAT A/B (same instrument, core the only
variable): `svc` <n>→<n> µs/iter, `print` <n>→<n> µs/iter, `enc` <n>→<n>
µs/block (<unchanged|moved>), `pcmdrops` <n>/<n>, firmware download <n>→<n> s,
connect <n>→<n> s. Predictions <all held | which refuted, with the number>.
★ **Nothing moved on silicon**: acid_box default golden `0x1479CEE8`
bit-identical over two boots with `ACIDBOX_VSYNC timeouts=0` throughout, and
`dualcore/cm4_audio_test` `AUDIO_CM4=PASS` with the CM4 staged and booted
from a cached CM7.
★ **Follow-on**: <realtime_x ≥ 5 → "acid_box: drop the M2_BT_OUT ITCM
routing" filed as NEW-nn | not met at <x>: the routing stays, and this is the
recorded reason>. **D-cache and MPU remain out of scope** — a separate,
riskier change with its own DMA-coherency review.
```

Fill every `<…>` from `$SCRATCH/new36-results.md` and the three transcripts. No angle bracket may survive.

- [ ] **Step 4: `examples/README.md` line 47**

Replace
```
| **timing** | `interval_timer_test`, `interval_timer_hw`, `rtc_test` |
```
with
```
| **timing** | `interval_timer_test`, `interval_timer_hw`, `rtc_test`, `icache_bench_hw` (CM7 L1 I-cache A/B in one boot — `calls`/`crc`/the real `Sbc` encoder, ITCM vs flash; silicon-only, no gate: qemu2 masks `CCR.IC`; the numbers behind the core's I-cache enable, NEW-36) |
```

- [ ] **Step 5: acid_box CMake comment (lines 150-151)**

Replace
```
    # stack runs in setup()/loop(), not a tight audio/display ISR, so flash residency
    # is cheap (I-cache-covered).  Derived at configure time from the current core
```
with
```
    # stack runs in setup()/loop(), not a tight audio/display ISR, so flash residency
    # is cheap -- I-cache-covered SINCE NEW-36 (2026-09-05; the core had NO I-cache
    # when this line was first written: svc measured <n> us/iter then, <n> after,
    # transcript_hw_evkb_bt.txt).  Derived at configure time from the current core
```

- [ ] **Step 6: Follow-on decision**

If the bench's `realtime_x_on` (default build) is ≥ 5.0: create the Linear issue with `save_issue` (team `Newdigate`, project `RT1170 cores & ecosystem`, `relatedTo: ["NEW-36"]`, state Backlog):

> title: `acid_box: drop the M2_BT_OUT ITCM routing now the I-cache covers flash-resident code`
> description: `NEW-36's bench measured the flash-resident Sbc encoder at <n> µs/frame with the I-cache on (realtime_x <x>, ITCM <n>). The M2_BT_OUT linker-script derivation in examples/display/acid_box/CMakeLists.txt (routes libM2Radio + setup() to FLASH to fit ITCM, keeps Sbc.cpp in ITCM) can likely drop the ITCM-side special-casing, or the whole derivation if ITCM no longer overflows with LVGL in flash. Acceptance: the M2_BT_OUT bench build streams to the Shokz with pcmdrops=0 and enc within <n>% of the ITCM number; the default build's ELF byte-identical; sweep 128/0/0.`

Record the outcome (issue id, or "not met at <x>") in the README's last line (Task 5 Step 5) and in the CLAUDE.md block.

- [ ] **Step 7: Commit the docs**

```bash
cd ~/Development/rt1170/evkb && git add CLAUDE.md examples/README.md examples/display/acid_box/CMakeLists.txt examples/timing/icache_bench_hw/README.md && git commit -q -m "docs: NEW-36 close-out -- CM7 L1 I-cache enabled, measured on silicon (sweep 128/0/0)

<one line with the headline numbers: ROM CCR reading, sbc us/frame off/on/itcm, calls x, acid_box svc us/iter pre->post>

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

- [ ] **Step 8: Close NEW-36 in Linear**

`save_issue` with `id: NEW-36`, `state: Done`, and a `patch` `append`:

```
## Close-out (2026-09-05)
- teensy-cores <CORES_NEW>: `arm_icache_enable()` in `ResetHandler`; pin bumped (evkb <sha>), fresh-user verified.
- Sweep 128/0/0 against REBUILT images (freshness checked by symbol); LICENSE-AUDIT PASS; vacuity green.
- Silicon (`timing/icache_bench_hw`, 2 boots): ROM `ccr_reset=<value>` (IC=<0|1>); 32 KB/2-way/32 B; calls <n>→<n> cyc/call (<x>×), crc <x>×, Sbc <n>→<n> µs/frame (<x>×, ITCM <n>, realtime_x <x>); every witness matched.
- acid_box LOOPSTAT A/B: svc <n>→<n> µs/iter, print <n>→<n>, enc unchanged (<n>), pcmdrops 0/0.
- Nothing moved: acid_box golden 0x1479CEE8 ×2 boots, cm4_audio_test PASS.
- Follow-on: <filed NEW-nn | not met>.
```

- [ ] **Step 9: Memory**

Write `/Users/nicholasnewdigate/.claude/projects/-Users-nicholasnewdigate-Development-rt1170-evkb/memory/new36-cm7-icache.md`:

```markdown
---
name: new36-cm7-icache
description: NEW-36 DONE 2026-09-05 — imxrt1176 core now enables the CM7 L1 I-cache (I-cache only); the numbers, the QEMU blindness, the rebuild-before-sweep trap
metadata:
  type: project
---

NEW-36 (2026-09-05): the `imxrt1176` core enables the CM7 L1 INSTRUCTION cache in
`ResetHandler` (`arm_icache_enable()` after the .bss zero; `startup_ccr_at_reset`
records the ROM's CCR — read <value>, IC=<0|1>). D-cache/MPU still OFF, deliberately.
Silicon (`timing/icache_bench_hw`, silicon-only, no gate): calls <x>×, crc <x>×,
Sbc <n>→<n> µs/frame (ITCM <n>), realtime_x <x>; acid_box svc <n>→<n> µs/iter.
Follow-on: <filed NEW-nn | not met>.

**Why:** flash-resident code ran at raw QSPI speed (5–30 µs/call) — NEW-33 finding (a).
**How to apply:** qemu2 masks `CCR.IC` and NOPs `ICIALLU`, so NO gate can see cache
behaviour — every cache claim is silicon's. A core change that touches startup needs
EVERY gate image rebuilt and freshness-checked by symbol before the sweep, or the sweep
passes vacuously on old-core ELFs. See [[new33-acid-box-bt-ui-responsiveness]].
```

Add to `MEMORY.md`: `- [NEW-36 CM7 I-cache](new36-cm7-icache.md) — DONE 2026-09-05: I-cache on (D-cache still off); Sbc <n>→<n> µs/frame; ★ rebuild+freshness-check every gate image before a startup-change sweep; ★ QEMU is blind to caches`.

---

### Task 10: Finish the branch

- [ ] **Step 1: Verify the branch state**

```bash
cd ~/Development/rt1170/evkb && git status --short && git log --oneline master..HEAD && git -C ~/Development/teensy-cores status --short && git -C ~/Development/teensy-cores log --oneline origin/master -1
```

Expected: clean tree; the branch's commits (spec, plan, bench, transcripts, pin, docs); teensy-cores clean with `origin/master` at `$CORES_NEW`.

- [ ] **Step 2: Integrate** — invoke `superpowers:finishing-a-development-branch` (merge to `master` and push, per this tree's convention for closed-out work).

---

## Self-review against the spec

- §1 core change → Tasks 1–2. §2 (no MPU) → the startup comment and the header comment carry the argument; no code. §3 (QEMU) → Task 3 Step 3 and the bench's QEMU smoke (Task 4 Step 5). §4 bench → Task 4 (build, layout witnesses, smoke) + Task 5 (silicon, transcript, README). §5 A/B → Task 6. §6 regression → Task 7. §7 close-out → Tasks 3 (sweep/audit/vacuity), 8 (push/pin/fresh-user), 9 (docs, Linear). §8 follow-on → Task 9 Step 6. Testing table → covered as above. Risks: "ROM already on" → the `ccr_reset` reading (Task 5 Step 2); "CCSIDR disagrees" → `isize_kb` check (Task 5 Step 2); "a witness moves" → Task 7 Step 1's stop rule; "archive pulls a blob" → Task 4 Step 3/4 (nothing in M2Radio references one; the fallback of compiling `bt/Sbc.cpp` directly stands if the link ever fails); "bench time" → Tasks 5–7 are independent captures.
- Placeholders: every `<…>` is a value to be copied from a named log/transcript line, and each task says the committed file must contain none.
- Names: `arm_icache_enable/disable/invalidate_all`, `startup_ccr_at_reset`, `SCB_CCR_IC`, `SCB_CACHE_ICIALLU`, `SCB_ID_CSSELR/CCSIDR/CLIDR` are spelled identically in Tasks 1, 2, 4; the bench's row tokens (`ccr_reset`, `isize_kb`, `wit_match`, `sbc_crc_match`, `realtime_x_on`, `flash_on_vs_itcm`) match between the source (Task 4) and the transcript/README/CLAUDE.md templates (Tasks 5, 9).
