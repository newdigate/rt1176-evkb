# imxrt1176 core: enable the CM7 L1 instruction cache (NEW-36) — Design

**Date:** 2026-09-05
**Issue:** NEW-36 — "imxrt1176 core: enable the CM7 L1 instruction cache
(I-cache only)". Finding (a) of the NEW-33 close-out
(`2026-09-04-acid-box-bt-ui-responsiveness-design.md`).
**Status:** Design, approved in brainstorming; pending spec review.

## Problem

The `imxrt1176` core runs the Cortex-M7 with **no instruction cache**.
`teensy-cores/imxrt1176/startup.c` never writes `SCB_CCR`, and
`imxrt1176.h` carries no `SCB_CCR`, `SCB_CACHE_ICIALLU` or `SCB_ID_CCSIDR`
define at all — the register surface is absent, not merely unused. The
`startup.c` header (line 52) lists "MPU/cache" among the RT1062 bring-up
"removed, to be re-added in proper RT1176 form in later phases"; this is that
phase, for the I-cache. The `teensy4` core (rt1062) enables both caches in
its `configure_cache()`.

Measured on silicon, flash-resident (XIP FlexSPI) code executes at raw QSPI
speed — **5–30 µs per small function call**:

- the SBC encode measured **2–3 ms/block flash-resident vs ~145 µs in ITCM**
  (NEW-9), which is why `Sbc.cpp` had to be routed to ITCM;
- acid_box's per-loop M2Radio service chain costs **24.4 µs/iteration** after
  NEW-33 fix 1 (`svc` ~225 ms/s at ~9200 it/s), and the LOOPSTAT summary's
  early return costs **4.6 µs (A) / ~32 µs (B)** per idle iteration purely
  from FlexSPI fetch (`examples/display/acid_box/transcript_hw_evkb_bt.txt`,
  INSTRUMENT NOTES).

The linker (`imxrt1176.ld`) puts every `.text*` in ITCM by default, so the
population that pays is the flash-routed exception set: LVGL in every VGLite
build (`libLVGL_flash.a` → `.text.progmem`), the whole M2Radio stack plus
`setup()` in acid_box's `M2_BT_OUT` build, and any `.progmem*` code.
acid_box's CMake comment already calls that flash residency "cheap
(I-cache-covered)" — false when it was written.

## Goal

1. Enable the CM7 L1 I-cache in the core's `ResetHandler`, for every rt1176
   image.
2. Measure the speedup on silicon with a dedicated, repeatable instrument
   (a microbench that A/Bs the cache in one boot) and with the application
   that raised the finding (acid_box LOOPSTAT, core before vs after).
3. Prove nothing else moved: the QEMU sweep stays 128/0/0 and two silicon
   images that exercise every subsystem the cache could touch reproduce
   their recorded witnesses.

## Non-goals

- **No D-cache.** It needs MPU regions and a DMA-coherency review (the
  rt1062 DMAMEM / uncached-OCRAM lessons in CLAUDE.md). Every comment in
  the core that says "D-cache is off" stays true.
- **No MPU.** The I-cache needs none (§2 below).
- **No new QEMU gate.** qemu2 masks the cache bits and NOPs the maintenance
  ops (§3), so a gate could only assert that the bench printed; the other
  128 gates already prove the new startup boots every image. The bench is a
  silicon-only `_hw` example like `timing/interval_timer_hw` and
  `audio/tone_hw` (17 examples in the tree carry no gate).
- **No change to acid_box's `M2_BT_OUT` ITCM routing.** Whether `Sbc.cpp`
  can leave ITCM is a follow-on with a recorded trigger (§8).
- **No rt1062 change.** The `teensy4` core already enables both caches.

## What the code predicts (to be confirmed by measurement, not assumed)

| observation | prediction with the I-cache on |
|---|---|
| `startup_ccr_at_reset` (what the boot ROM leaves in `SCB_CCR`) | `IC`=0, `DC`=0 — the timings above fit nothing else, but this is the first *reading* of it |
| `CCSIDR` for the instruction cache | 32 KB, 2-way, 32-byte lines (RT1176 datasheet) |
| `calls` workload, flash, cache off → on (warm) | ≥ 10× fewer cycles per call; flash-cached within ~1.5× of the ITCM twin |
| `sbc` workload, flash, cache off → on (warm) | from the recorded 2–3 ms/block to within ~2× of the ITCM ~145 µs — i.e. well inside the 2.902 ms real-time block period |
| acid_box LOOPSTAT `svc` per iteration | 24.4 µs → single digits |
| acid_box LOOPSTAT `print` per idle iteration | ~32 µs (B) → ~5 µs |
| acid_box LOOPSTAT `enc` per block | **unchanged** (ITCM; a move here means something *else* changed) |
| acid_box `pcmdrops`, frame interval, touch p95 | `pcmdrops=0` held; the frame numbers are vsync-bound at 30 fps and should not move outside noise |
| every pixel golden | bit-identical — a cache cannot change a picture |

## Design

### 1. The core change (`teensy-cores/imxrt1176`)

**`imxrt1176.h`**, beside the existing `SCB_VTOR` / `SCB_ICSR` defines:

```c
#define SCB_CCR            (*(volatile uint32_t *)0xE000ED14u)  /* Configuration and Control */
#define SCB_CCR_BP         (1u << 18)   /* branch prediction enable (not written by this core) */
#define SCB_CCR_IC         (1u << 17)   /* L1 instruction cache enable */
#define SCB_CCR_DC         (1u << 16)   /* L1 data cache enable — NOT set by this core */
#define SCB_ID_CLIDR       (*(const volatile uint32_t *)0xE000ED78u)
#define SCB_ID_CTR         (*(const volatile uint32_t *)0xE000ED7Cu)
#define SCB_ID_CCSIDR      (*(const volatile uint32_t *)0xE000ED80u)
#define SCB_ID_CSSELR      (*(volatile uint32_t *)0xE000ED84u)
#define SCB_CACHE_ICIALLU  (*(volatile uint32_t *)0xE000EF50u)
```

and three `static inline` helpers next to the `arm_dcache_*` no-ops (line
~884), each the ARM-documented sequence (DDI0403E §B2 / CMSIS `SCB_*ICache`):

- `arm_icache_invalidate_all()` — DSB; ISB; `SCB_CACHE_ICIALLU = 0`; DSB; ISB.
- `arm_icache_enable()` — return if `SCB_CCR & SCB_CCR_IC`; invalidate-all;
  `SCB_CCR |= SCB_CCR_IC`; DSB; ISB.
- `arm_icache_disable()` — DSB; ISB; `SCB_CCR &= ~SCB_CCR_IC`; DSB; ISB;
  invalidate-all. (Leaves no stale lines to be hit on the next enable.)

plus `extern uint32_t startup_ccr_at_reset;`.

**`startup.c`**: define `uint32_t startup_ccr_at_reset;` beside
`external_psram_size`. In `ResetHandler`, immediately after
`memory_clear(&_sbss_dma, &_ebss_dma)` (line 196) and before the RAM
vector-table build (line 198):

```c
/* Snapshot what the boot ROM left in CCR, then enable the L1 I-cache. ... */
startup_ccr_at_reset = SCB_CCR;
arm_icache_invalidate_all();   /* unconditional -- see below */
arm_icache_enable();
```

Placement, written into the comment: after the `.bss` zero so the snapshot
survives `memory_clear` (nothing before this point touches `CCR`, so the
value IS the ROM's hand-off state). What the cache then covers is AXIM
instruction fetch only: the rest of `ResetHandler` itself (flash-resident
`.startup` code — the vector loop, the DCDC wait, the SNVS and SEMC glue)
and every flash-resident function reached later (`.progmem*`,
`libLVGL_flash.a` in VGLite builds, anything a derived linker script routes
to FLASH). `set_arm_clock_rt1176`, `semc_sdram_init`, `__libc_init_array`
and `main` are ITCM-resident (`imxrt1176.ld` sends `.text*` to ITCM) and run
at TCM speed either way. The two `memory_copy` and three `memory_clear`
calls that precede it run uncached — three-instruction loops the FlexSPI
AHB prefetch buffer already covers, and their cost is data-bound. The DCDC
`STS_DC_OK` wait is a bounded timeout, not a beneficiary: caching shrinks
its guard window slightly (each pass is dominated by an AIPS read).
*(Corrected 2026-09-05 after code review: the first draft listed the
ITCM-resident functions as beneficiaries.)*

**The invalidate is unconditional** — `arm_icache_invalidate_all()` is
called before `arm_icache_enable()` in startup. ARMv7-M resets `CCR.IC` to
0, so every reset path (SW4, SYSRESETREQ, LinkServer's VECTRESET) arrives
with IC=0 and the helper would invalidate anyway; but a debugger flow that
halts, re-flashes and jumps to `ResetHandler` without a reset arrives with
IC=1 and the previous image's lines, and the helper's CMSIS early return
would keep them. The M7 does not clear cache RAM on reset. *(Added
2026-09-05 after code review.)*

The header comment at line 52 changes from "MPU/cache" to "MPU/D-cache"
with a pointer to this spec; line 258's "D-cache is off in this core"
stays as written.

### 2. Why the I-cache is safe without an MPU

- **Default memory map.** With no MPU enabled, ARMv7-M's system address map
  applies: `0x20000000–0x3FFFFFFF` (which holds the XIP window at
  `0x30000000`) is Normal, write-back cacheable, executable; `0x00000000`
  (ITCM) and `0x20000000` (DTCM) are TCM interfaces that never go through
  the L1 caches; `0x40000000+` is Device, XN. The GC355, LCDIF, eDMA and
  every other bus master read *data*, which the I-cache never holds.
- **No runtime code writes.** Nothing in the tree writes code that the CM7
  later executes: the eeprom emulation (`eeprom.c`) programs the top 256 K of
  flash as *data*, from ITCM, with IRQs masked; CM4 images are copied by the
  CM7 into the CM4's TCM and executed only by the CM4; `.text.itcm` is copied
  once, in startup, into a TCM. The invalidate helper exists for a future
  loader that would need it — none does today. The FlexSPI `SWRESET`s in
  `eeprom.c` purge the AHB prefetch buffer; the L1 I-cache is a second
  buffer on that read path which they do NOT reach — harmless for the
  eeprom region (no code lives there), and the reason a future flash writer
  of *code* must call `arm_icache_invalidate_all()` (a comment in
  `eeprom.c` now says so).
- **Debugger.** LinkServer's flash algorithm runs after a core reset and the
  next boot re-runs `ResetHandler`, which invalidates before enabling.
  Breakpoints in XIP flash are FPB hardware breakpoints, unaffected.
- **Timing code** (`micros()`, `delayMicroseconds()`) is DWT-cycle based, not
  loop-calibrated, so faster fetch changes no delay.

### 3. What QEMU can and cannot see

qemu2 (`hw/intc/armv7m_nvic.c`): the `CCR` write mask keeps only
`STKALIGN|BFHFNMIGN|DIV_0_TRP|UNALIGN_TRP|USERSETMPEND|NONBASETHRDENA`
(`IC`/`DC` are dropped, so `CCR.IC` reads back 0 forever), and `ICIALLU`
(0xE000EF50) is an explicit "always NOP". `CCSIDR`/`CLIDR` read as the
`cortex-m7` model's values. So under QEMU:

- `arm_icache_enable()` runs its full sequence every time (the early-return
  never fires), faults nothing, and changes nothing;
- every one of the 128 gates runs byte-identical firmware behaviour to
  before — the sweep is a **boot-regression check** for the new startup code
  and proves nothing about the cache;
- the bench prints `icache=off` in every cell (honestly: it reads `CCR`
  back), and no gate reads that.

### 4. The microbench: `examples/timing/icache_bench_hw`

**Layout.** `CMakeLists.txt`, `icache_bench_hw.cpp`, `README.md`,
`transcript_hw_evkb.txt`. No `boards` sidecar (rt1176 only), no
`run_qemu.sh`. Links `cores` and `M2Radio` the way `audio/bt_tone_test`
does (`import_evkb_library(M2Radio sdio iw416 hci bt)`); only `Sbc.cpp.obj`
is referenced, so nothing else from the archive links and no firmware-blob
symbol is needed. Fallback if the archive drags in anything that wants a
blob: compile `bt/Sbc.cpp` straight into the target.

**Placement of code under test.**

- The bench's own flash workloads carry
  `__attribute__((section(".progmem.icb")))` — the core script already
  routes `.progmem*` to `.text.progmem` in FLASH. Their ITCM twins are the
  same source instantiated by a macro without the attribute (default
  `.text*` → ITCM).
- `Sbc` is a library object, so it is routed with acid_box's mechanism: a
  linker script derived at configure time from the core's `imxrt1176.ld` by
  `string(REPLACE "*libLVGL_flash.a:(.text*)" "… \n\t\t*libM2Radio*.a:Sbc.cpp.obj(.text* .fastrun)")`,
  scoped to this target. `option(ICACHE_BENCH_SBC_ITCM … OFF)` skips the
  derivation so the same image measures Sbc's ITCM reference cell from a
  second build directory (`build-sbc-itcm`).

**Workloads**, each `noinline, noclone`, inputs from a `volatile` source,
results into a `volatile` sink, `asm volatile("":::"memory")` between reps:

1. `calls` — 64 three-instruction functions called in an unrolled direct
   chain (`s = f0(s); … s = f63(s);`), N passes. Fetch- and branch-bound: the
   "5–30 µs per small call" shape. Reported per call.
2. `crc` — table-driven CRC32 over a 4 KB DTCM buffer, table in DTCM (a
   non-const array, so the data side is TCM and the cell measures fetch,
   not `.rodata` reads from flash). Branchy loop with data traffic: the
   service-chain shape. Reported per byte.
3. `sbc` — `Sbc::encode()` of one 128-sample stereo frame, N times, with
   the configuration acid_box negotiates (`cie=21150235`: 44.1 kHz, joint
   stereo, 16 blocks, 8 subbands, loudness, bitpool 53 → 119-byte frames).
   Input is a deterministic 1 kHz sine plus a small LCG dither so bit
   allocation is not degenerate. Reported per frame and as `realtime_x` =
   2.902 ms ÷ µs/frame. Every cell CRC32s its concatenated output and
   prints it as `sbc_crc`; the image compares its own cells (flash-off vs
   flash-on) and prints `sbc_crc_match=1`, and the ITCM build's `sbc_crc`
   is compared against them by transcript — a correctness witness the
   bench gets for free.

**Protocol**, in `setup()`, DWT `CYCCNT` deltas, IRQs masked around each
timed rep (a rep is milliseconds, so at most one SysTick tick is deferred
per rep and `micros()`'s 4.3 s wrap service is never at risk):

for each workload × placement: `arm_icache_disable()` → K=8 reps, report
**min** and **median** cycles; `arm_icache_enable()` (which invalidates
first) → the first rep is reported as **cold**, then K warm reps, min and
median. N is sized so an uncached rep takes ≥ 1 ms (DWT quantisation
negligible) and the whole bench finishes in < 10 s. Each cell reads `CCR`
back after the toggle and prints the state it *measured under*, never the
one it asked for. The I-cache is left enabled at the end; `loop()`
heartbeats once a second.

**Output** (fixed tokens; `transcript_hw_evkb.txt` holds two SW4 boots):

```
icache_bench ccr_reset=0x… ccr_now=0x… clidr=0x… ccsidr_i=0x… isize_kb=32 ways=2 line_b=32
icache_bench wl=calls place=itcm  icache=on  min_cyc/call=… med_cyc/call=… us/rep=…
icache_bench wl=calls place=flash icache=off min_cyc/call=… med_cyc/call=… us/rep=…
icache_bench wl=calls place=flash icache=on  cold_us/rep=… min_cyc/call=… med_cyc/call=… us/rep=…
icache_bench wl=crc   … (per byte, same three rows)
icache_bench wl=sbc   place=flash icache=off us/frame=… realtime_x=… sbc_crc=0x…
icache_bench wl=sbc   place=flash icache=on  cold_us/frame=… us/frame=… realtime_x=… sbc_crc=0x…
icache_bench sbc_crc_match=1
icache_bench summary calls=…x crc=…x sbc=…x flash_on_vs_itcm=calls …x crc …x
```

(`sbc place=itcm` appears from the `ICACHE_BENCH_SBC_ITCM` build only.)
`isize_kb`/`ways`/`line_b` are decoded from `CCSIDR` with `CSSELR=1`
(instruction cache, level 1) and are the sanity check that the core has
the 32 KB I-cache the datasheet gives it.

### 5. acid_box LOOPSTAT A/B (core before vs after)

The NEW-33 bench configuration, unchanged: `display/acid_box` with
`-DM2_BT_OUT=ON -DACIDBOX_LOOPSTAT=ON`, the IW416 blobs, the Shokz target
name, 3 Mbaud, RTS flow control. Built **twice**, against `teensy-cores`
at `672c577` (the pre-change HEAD) and at the new commit — the local-first
library resolution means the checkout state is what builds — so the
instrument is identical and the core is the only variable.

Per boot: connect to the Shokz, capture ≥ 60 s of steady streaming
(`wiggle=0`) and a wiggle window, before the headset's ~1–2 min
self-power-off (NEW-34). Compared per second: loop it/s, `svc`, `print`,
`poll`/`enc`, `max_us`, frame interval, touch p95, `pcmdrops`; and the two
bring-up durations (firmware download, connect), both of which run
flash-resident loader code. Predictions are the table above; they are
written down before the run and the transcript records hits and misses.

### 6. Silicon regression ("nothing moved")

1. `display/acid_box`, **default build** (the shipping image: MIPI panel,
   GC355 compositor, touch, SAI/DMA audio, SD, I²C): GPU golden
   `0x1479CEE8` bit-identical over two boots, `ACIDBOX_VSYNC timeouts=0`,
   its audio witnesses as recorded in `transcript_hw_evkb.txt`. A pixel
   golden cannot see a cache; an identical checksum is the strongest cheap
   "no behavioural change" statement, and the fps/loop numbers moving is
   the *expected* side.
2. `dualcore/cm4_audio_test`: the CM7 stages and boots the CM4 and talks
   MU from a cached core — the one subsystem acid_box does not touch. Pass
   tokens as recorded in its `transcript_hw_evkb.txt`.

Bench discipline as recorded in CLAUDE.md: `flash load` → `flash verify`
→ detach every debugger → SW4 to free-run; a DAP wedge means replug the
DEBUG USB; never `pkill -9` mid-program.

### 7. Close-out

- QEMU sweep **128 passed, 0 failed, 0 SKIP** (`-l` still 128 — the bench
  adds no gate), run alone, output captured; `LICENSE-AUDIT: PASS` before or
  after, never during; `gate-vacuity.test.sh` green.
- `cores` committed and pushed; `evkb.cmake` pin bumped; a fresh-user
  `-DEVKB_FORCE_FETCH=ON` build clones the new pin and **a gate is run
  against that fetched ELF** (a configure proves the subdir resolves; only a
  gate run proves the fetched code behaves).
- Docs: CLAUDE.md gains the measurement block and the Architecture
  paragraph "The two cores also differ on the D-cache" gains the I-cache
  fact, with NEW-33 finding (a) retired; the bench's README and transcript;
  `examples/README.md` timing row; acid_box's "(I-cache-covered)" comment
  becomes true and cites the number.
- Linear NEW-36: In Progress now; closed with the numbers.

### 8. Follow-on (out of scope, trigger recorded)

If the bench's flash-resident **cached** `sbc` cell shows ≥ 5× real-time
headroom (`realtime_x ≥ 5`, i.e. < ~580 µs/frame), file "acid_box: drop
the `M2_BT_OUT` ITCM routing" as its own issue, with this bench as its
input. If not, the bench's number is the recorded reason the routing stays.

## Testing

| layer | what | proves |
|---|---|---|
| QEMU sweep | 128 gates, unchanged assertions | the new `ResetHandler` boots every image; the cache sequence faults nothing (QEMU NOPs it) |
| host | none new | the helpers are six-line register sequences with no pure logic to unit-test; the bench's `sbc_crc_match` is their functional witness on silicon |
| silicon, bench | `icache_bench_hw`, two boots | the ROM state reading, `CCSIDR` decode, the speedups, `sbc_crc_match=1` |
| silicon, application | acid_box LOOPSTAT A/B | the application-level number the finding was about |
| silicon, regression | acid_box default golden + `cm4_audio_test` | nothing moved |
| fresh user | `-DEVKB_FORCE_FETCH=ON` + one gate run | the pushed core at the bumped pin |

Order: core change → sweep (cheap, catches a broken startup before any
bench time) → bench → A/B → regression → push/pin/fresh-user → docs.

## Risks

- **The ROM already had the I-cache on.** Then `startup_ccr_at_reset` reads
  `IC=1`, the enable early-returns, and the bench's off→on delta is the
  whole story anyway (the bench disables it itself). The timings say
  otherwise; the reading settles it.
- **A `CCSIDR` decode that disagrees with the datasheet** would mean the
  wrong `CSSELR` index or a core we misidentify — a bench defect, caught
  by the `isize_kb=32` line before any speedup is believed.
- **A silicon witness moves.** A pixel golden or `cm4_audio_test` token
  changing would mean the I-cache is *not* transparent for that path
  (stale instruction lines somewhere code is written at runtime) — the
  bench's `sbc_crc_match` and the goldens are the tripwires, and the
  change does not ship until the cause is found. No such path is known.
- **`M2Radio` archive pulls in a blob symbol.** Handled by the fallback in
  §4 (compile `Sbc.cpp` directly).
- **Instruction timing moves only for flash-resident code.** `.text*`
  defaults to ITCM, so cycle-calibrated spin loops there —
  `set_arm_clock_rt1176`'s ~30 µs settle loop and its OSC/PLL guards — are
  unaffected; the only spin loop whose speed changes is `ResetHandler`'s
  own DCDC `STS_DC_OK` timeout (§1), whose window shrinks slightly.
- **Bench time.** The A/B needs the Shokz twice; the regression needs two
  more images. If the headset misbehaves (NEW-34), the bench and the
  regression still stand on their own and the A/B is recorded as pending.

## File-by-file

**`~/Development/teensy-cores` (sibling repo, pushed; pin bumped here)**
- `imxrt1176/imxrt1176.h` — SCB cache defines, `arm_icache_*` helpers,
  `startup_ccr_at_reset` extern.
- `imxrt1176/startup.c` — the global, the snapshot + enable after the
  `.bss`/`.dmabuffers` zero, header comment correction.

**`evkb` (this repo)**
- `examples/timing/icache_bench_hw/{CMakeLists.txt,icache_bench_hw.cpp,README.md,transcript_hw_evkb.txt}` — new.
- `examples/README.md` — timing row.
- `examples/display/acid_box/CMakeLists.txt` — the "(I-cache-covered)"
  comment, now true, cites the measurement.
- `examples/display/acid_box/transcript_hw_evkb_bt.txt` — the A/B block
  appended; `transcript_hw_evkb.txt` — the regression boots.
- `evkb.cmake` — `cores` pin.
- `CLAUDE.md` — measurement block; Architecture cache paragraph.
- `docs/superpowers/specs/2026-09-05-cm7-icache-enable-design.md` — this.
