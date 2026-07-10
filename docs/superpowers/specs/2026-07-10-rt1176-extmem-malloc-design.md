# RT1176 EXTMEM Dynamic Allocation (`extmem_malloc`) — Design Spec

**Date:** 2026-07-10
**Status:** Approved (design)
**Board:** MIMXRT1170-EVKB — 64 MB SEMC SDRAM at `0x80000000`
**Builds on:** [[rt1176-sdram-semc]] (the static SDRAM bring-up + `EXTMEM` macro, HW-verified)

## Goal

Implement the `extmem_malloc` / `extmem_free` / `extmem_calloc` / `extmem_realloc` family
(declared but unimplemented in `wiring.h:227-230`) so code can dynamically allocate out of
the 64 MB SEMC SDRAM at runtime — a heap layered on top of the static `EXTMEM` bring-up.

## Scope

- **In:** the four `wiring.h` functions, backed by a dedicated `smalloc` pool over the SDRAM
  heap region (`_extram_end` → top of the 64 MB); the pool init in `startup.c`; and
  **zero-initialisation of the static `EXTMEM` `.bss`** at boot (so `EXTMEM int x;` defaults
  to 0 like normal `.bss` — approved, reversing the [[rt1176-sdram-semc]] "not zero-initialised"
  note; teensy4 does this and it is correct C semantics).
- **Deferred (YAGNI):** a C++ `operator new`-into-EXTMEM overload; exposing `smalloc`'s
  stats/introspection (`sm_malloc_stats`); any allocator tuning beyond the teensy4 defaults.

## Architecture — a core bring-up, verbatim teensy4 port

`extmem_malloc` is a `malloc`-class **primitive**, so it lives in the **core** (as in teensy4),
not an Arduino library. `extmem_malloc` needs a heap arena **separate** from newlib's
internal-RAM `malloc` (which is single-arena `sbrk`); teensy4 solves this with **`smalloc`**,
a proven platform-agnostic pool allocator. We port teensy4's pieces faithfully.

| Repo | Change |
|---|---|
| `newdigate/teensy-cores` (`evkb/cores`) | add `smalloc` (13 `sm_*.c` + 2 headers, verbatim); port `extmem.c` with an `__IMXRT1176__` branch; add the pool-init + EXTMEM `.bss` zero-init in `startup.c` |
| `qemu2` | **none** — the allocator is deterministic RAM logic; the SDRAM window is already enabled post-init by the [[rt1176-sdram-semc]] SEMC model |
| `evkb` (local) | new `extmem_test/` gate |

## Component design

### C1. `smalloc` allocator (port verbatim)

Copy from `cores/teensy4/` into `cores/imxrt1176/`, unchanged (~781 LOC, portable C):
`smalloc.h`, `smalloc_i.h`, and the 13 sources `sm_alloc_valid.c`, `sm_calloc.c`, `sm_free.c`,
`sm_hash.c`, `sm_malloc.c`, `sm_malloc_stats.c`, `sm_pool.c`, `sm_realloc.c`, `sm_realloc_i.c`,
`sm_realloc_move.c`, `sm_szalloc.c`, `sm_util.c`, `sm_zalloc.c`. Provides
`struct smalloc_pool` + `sm_set_pool` / `sm_malloc_pool` / `sm_free_pool` / `sm_realloc_pool`.
The core CMake glob compiles them — but `file(GLOB)` has **no `CONFIGURE_DEPENDS`**, so any gate
must `rm -rf build` and reconfigure after the files are added.

### C2. `extmem.c` (port + one branch)

Port `cores/teensy4/extmem.c`. It is gated on `HAS_EXTRAM`, currently only defined for
`ARDUINO_TEENSY41` / `ARDUINO_MIMXRT1060_EVKB`. Add an `__IMXRT1176__` arm:

```c
#if defined(__IMXRT1176__)
#define HAS_EXTRAM
// EVKB: 64 MB SEMC SDRAM at 0x80000000..0x83FFFFFF
#define IS_EXTMEM(addr) (((uint32_t)(addr) >> 28) == 8)
#endif
```

The four wrappers are then reused verbatim: `extmem_malloc` → `sm_malloc_pool(&extmem_smalloc_pool,…)`,
falling back to internal `malloc()` if the pool returns NULL; `extmem_calloc` likewise (the pool is
created with `do_zero=1`); `extmem_free` / `extmem_realloc` route by `IS_EXTMEM(ptr)` to the pool vs.
newlib. `extmem_smalloc_pool` is a global defined in `startup.c` (C3).

### C3. `startup.c` — pool init + EXTMEM zero-init

Mirror teensy4's `ARDUINO_MIMXRT1060_EVKB` path (same SEMC-SDRAM-at-`0x80000000` case). Declare
the globals and run the init **after `semc_sdram_init()`** — the SDRAM must be live because the
pool's control metadata is placed at `&_extram_end` (inside the SDRAM):

```c
extern unsigned long _extram_start;
extern unsigned long _extram_end;
uint8_t external_psram_size = 0;          // reported size in MB (fixed 64 here)
struct smalloc_pool extmem_smalloc_pool;

// ... in the startup sequence, after semc_sdram_init():
uint32_t extram_bytes = (uint32_t)&_extram_end - (uint32_t)&_extram_start;
if (extram_bytes) {
    memset(&_extram_start, 0, extram_bytes);   // zero-init the static EXTMEM .bss globals
}
external_psram_size = 64;                       // fixed — SDRAM is soldered, no probe
sm_set_pool(&extmem_smalloc_pool, &_extram_end,
            64u * 0x100000u - extram_bytes, 1, NULL);   // heap = the rest of the 64 MB; do_zero=1
```

**Divergence from teensy4 (documented):** omit teensy4's `arm_dcache_flush_delete(&_extram_start,…)`
after the memset — our core runs with the **D-cache off** (confirmed in the [[rt1176-sdram-semc]]
final review), so the `memset` reaches SDRAM directly and no cache maintenance is needed. Requires
`<string.h>` for `memset` and `smalloc.h` for the pool API.

### C4. Gate (`evkb/extmem_test/`)

Firmware (Serial1/LPUART1 markers, setup-context) modeled on `eeprom_test` / `sdram_test`:

```
EXTMEM_INIT
  p = extmem_malloc(N); IS_EXTMEM(p)?  write pattern, read back      -> EXTMEM_ALLOC=PASS|FAIL
  c = extmem_calloc(M); all zero?                                    -> EXTMEM_CALLOC=PASS|FAIL
  r = extmem_realloc(p, 2N); contents preserved across a grow/move?  -> EXTMEM_REALLOC=PASS|FAIL
  extmem_free(r); (re-malloc reuses freed space)                     -> EXTMEM_FREE=PASS|FAIL
  huge = extmem_malloc(>64 MB);  huge==NULL (pool + internal both    -> EXTMEM_FALLBACK=PASS|FAIL
        refuse an over-large request; degrades gracefully, no crash)
  overall                                                            -> EXTMEM_TEST=PASS|FAIL
```

Runner `run_qemu_extmem.sh` (qrun + gate-lib, `-serial file`, no `-icount` — allocator logic isn't
timing-sensitive) greps the PASS markers. CMakeLists = the core only (no library), plus a
`rm -rf build` reconfigure note (new core files).

## Data flow

`extmem_malloc(n)` → `sm_malloc_pool(&extmem_smalloc_pool, n)` → SDRAM pointer (in the
`_extram_end`→top region), or NULL on exhaustion → `malloc(n)` (internal RAM). `extmem_free(p)`
→ `IS_EXTMEM(p)` ? `sm_free_pool` : `free`. `extmem_realloc`/`calloc` analogous. Callers can mix
`extmem_*` freely; routing is by address range, so an internally-fallen-back block still frees
correctly.

## Test strategy

- **QEMU gate:** proves the allocator logic end-to-end — `IS_EXTMEM` routing, alloc/write/read,
  calloc-zero, realloc-preserve(+move), free-reuse, and the internal fallback. The SDRAM window is
  live (the SEMC model enables it on Mode-Set), so allocations land at real `0x8…` addresses in the
  model's RAM. Deterministic — no `-icount`.
- **Hardware (arbiter):** flash + run — confirms allocations occupy real SDRAM (`0x8…`), survive
  read-back, and don't corrupt (the pool metadata + blocks live in the physical SDRAM proven by
  [[rt1176-sdram-semc]]). Flash/serial per [[rt1170-evkb-flashing]] + [[macos-serial-capture]].

## Risks

| # | Risk | Mitigation |
|---|---|---|
| 1 | `file(GLOB)` misses new `sm_*.c` / `extmem.c` | plan step: `rm -rf build` + reconfigure after adding core files (no `CONFIGURE_DEPENDS`) |
| 2 | Pool init before SDRAM is live (metadata at `_extram_end` unbacked) | place the init strictly **after** `semc_sdram_init()` in `startup.c` |
| 3 | `smalloc` portability | it is unmodified, proven, platform-agnostic C already shipping in teensy4 |
| 4 | EXTMEM `.bss` zero-init cost | only the *declared* static EXTMEM globals are zeroed (typically KB), never the 64 MB heap |
| 5 | `external_psram_size` consumers expect a probe | we set a fixed 64 (SDRAM is soldered); no detection path needed unlike T4.1 PSRAM |

## Open questions — resolved

- **EXTMEM zero-init?** YES (user-approved) — `memset` the static `.bss.extram`; reverses the
  [[rt1176-sdram-semc]] note.
- **Allocator?** Port `smalloc` verbatim (vs. a custom pool allocator or a second newlib arena —
  both rejected: reinvention / single-arena `sbrk` mismatch).
- **QEMU change?** None — the SEMC window is already gated/enabled by [[rt1176-sdram-semc]].
- **Fixed vs. probed size?** Fixed 64 MB (soldered).

## References

- Port sources: `cores/teensy4/extmem.c`, `cores/teensy4/sm_*.c` + `smalloc*.h`, and the
  `ARDUINO_MIMXRT1060_EVKB` pool-init path in `cores/teensy4/startup.c` (~line 430).
- Linker already reserves it: `cores/imxrt1176/imxrt1176.ld` `ERAM @0x80000000 64M`, `.bss.extram`,
  `_extram_start` / `_extram_end`.
- Declarations to implement: `cores/imxrt1176/wiring.h:227-230`.
- Foundations: static SDRAM + `EXTMEM` + D-cache-off + faithful SEMC window gate [[rt1176-sdram-semc]];
  the `.bss.dma` zero-init precedent [[rt1176-usb-host-hid]]; gate infra [[rt1170-qemu]] +
  [[rt1170-gate-lib]]; flashing/serial [[rt1170-evkb-flashing]] + [[macos-serial-capture]].
