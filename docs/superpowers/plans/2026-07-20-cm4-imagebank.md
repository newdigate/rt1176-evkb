# CM4 Image Bank Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a small CM7-side `Cm4ImageBank` that registers several build-fixed
CM4 images in uniform ITCM slots and switches the running CM4 among them by
handle — a fast no-copy VTOR flip when the target is resident, a stage + evict
when it is not — proven by a QEMU gate and a wiring-free EVKB clean-boot probe.

**Architecture:** Pure bookkeeping over the existing HW-verified `Multicore`
(`begin`/`switchImage`/`running`) — **no `Multicore` change, no qemu2 change**.
Three new core files (`Cm4ImageBank.{h,cpp}`, `Cm4Slots.h`), one reusable build
helper (`teensy_add_cm4_slot_image` + `cm4_slot.ld.in`), and one gate
(`cm4_imagebank_test`) whose 4 images (A/B/C in distinct slots, D sharing A's
slot) exercise co-residency, the no-copy flip, and slot-scoped eviction with
un-fakeable MU-identity + `isResident()` assertions.

**Tech Stack:** bare-metal C/C++ (Cortex-M7 CM7 + Cortex-M4 images),
arm-none-eabi-gcc, the `teensy-cmake-macros` package, qemu2 (`mimxrt1170-evk`)
via `tools/qrun` + `gate-lib.sh`, LinkServer + `clean_boot.scp` for the EVKB
probe. Governed by the `cm4-bringup` skill (silicon wins; GPL one-way firewall).

**Spec:** `docs/superpowers/specs/2026-07-20-cm4-imagebank-design.md`.

---

## Ground truth (verified — reuse verbatim)

- `Multicore` (global `extern MulticoreClass Multicore`): `begin(image,bytes,
  stageAddr)→bool` (word-copy stage → `GPR0/1` → `BT_RELEASE_M4` first call only
  → `SW_RESET`; true when `STAT_M4CORE.UNDER_RST→0`); `switchImage(stageAddr)`
  (reprogram `GPR0/1` + `SW_RESET`, **no copy**, no-op unless released);
  `running()`. `cores/imxrt1176/Multicore.{h,cpp}`.
- `CM4_BOOT_ADDRESS` = `CM4_TCM_BACKDOOR` = `0x20200000u`
  (`cores/imxrt1176/imxrt1176.h:433-434`). ITCM code window = first 128 KB of the
  backdoor (`0x20200000`..`0x20220000`); DTCM half is data.
- `cm4/startup_cm4.S` (from `cm4_hotswap2_test`) sets `SCB->VTOR = __isr_vector`
  = the linker `ORIGIN(ITCM)` symbol (NOT hardcoded) → a slot image at any
  `ORIGIN` relocates VTOR correctly. Vector table = 135 words (540 B) → VTOR
  needs 1 KB (`0x400`) alignment; every slot size ≥ `0x1000` satisfies it. Copy
  **verbatim**.
- `cm4/main_cm4.c` (from `cm4_hotswap2_test`): `mu_send(0,0xCAFE0001u)` (ready) +
  `mu_send(0,HS_IDENTITY)` + park in `WFI`. Copy **verbatim** (identity via
  `-DHS_IDENTITY`).
- `teensy_add_cm4_image(NAME LINKER <ld> SOURCES … [DEFINES …])` →
  `objcopy -O binary` → `cm4_bin2header.cmake` → `static const uint32_t NAME[]`;
  `teensy_target_link_cm4_image(TARGET NAME)` exposes it.
  `teensy-cmake-macros/CMakeLists.include.txt:461,532`. `TEENSY_CMAKE_MACROS_DIR`
  is set by the package (used at `:522-524`).
- `run_qemu.sh` / `clean_boot.scp` / `toolchain/` patterns: copy from
  `cm4_hotswap2_test`. Gate is run as `./run_qemu.sh` (**never** `sh
  run_qemu.sh` — `gate_init` re-execs under `gtimeout`).
- `tools/license-audit.sh:60` `GATES` = a space-separated `dir:target` string;
  append one entry.

---

## File Structure

**Create:**
- `cores/imxrt1176/Cm4ImageBank.h` — the bank class + ITCM-window constants.
- `cores/imxrt1176/Cm4ImageBank.cpp` — `add`/`switchTo`/`isResident`/`name`.
- `cores/imxrt1176/Cm4Slots.h` — `CM4_SLOT_STAGE(k)` (build↔runtime slot glue).
- `teensy-cmake-macros/cm4_slot.ld.in` — parameterized slot linker template.
- `evkb/cm4_imagebank_test/` — the gate: `CMakeLists.txt`,
  `cm4_imagebank_test.cpp` (reporter), `cm4/{startup_cm4.S,main_cm4.c}` (copied),
  `cm4/` generated `.ld`s (build-time), `run_qemu.sh`, `clean_boot.scp`,
  `toolchain/rt1170-evkb.toolchain.cmake` (copied), `README.md`, transcripts.

**Modify:**
- `teensy-cmake-macros/CMakeLists.include.txt` — add
  `teensy_add_cm4_slot_image` (after `teensy_target_link_cm4_image`, ~line 539).
- `tools/license-audit.sh:60` — add `cm4_imagebank_test` to `GATES`.
- `.claude/skills/cm4-bringup/references/cm4-roadmap.md` — new-capability entry.
- memory (`rt1176-cm4-boot-mu` or a new `rt1176-cm4-imagebank`) + `MEMORY.md`.

**Note on repos** (memory `rt1176-evkb-git-repo`): `cores/` and
`teensy-cmake-macros/` are their **own nested git repos** inside the `evkb`
working tree. Commit core-file changes with `git -C cores …`, macro changes with
`git -C teensy-cmake-macros …`, and everything under `evkb/` (the test, audit,
docs) with `git -C evkb …`. Check `git status` in each before committing.

---

## Task 1: `Cm4ImageBank` core class + slot glue

> **★ Post-implementation refinement (final-review catch, 2026-07-20, cores
> `483b279`):** the `Cm4ImageBank.cpp` below was shipped as written, then the
> final holistic review found an error-path bug — `switchTo`'s stage-path
> eviction was gated behind `if (ok)`, but `Multicore::begin()` copies + resets
> *unconditionally*, so a `begin()` timeout would leave `resident_` stale and a
> later fast-flip could reboot a replaced slot (silent wrong-image boot). The
> shipped fix moves the eviction outside `if (ok)`, sets `current_ = -1` on a
> failed boot, and makes `add()`'s window check overflow-safe. **Use the
> corrected logic in spec §3.2, not the code block below.** (Gate output
> byte-identical; happy path unchanged.)

**Files:**
- Create: `cores/imxrt1176/Cm4ImageBank.h`
- Create: `cores/imxrt1176/Cm4ImageBank.cpp`
- Create: `cores/imxrt1176/Cm4Slots.h`

- [ ] **Step 1: Write `Cm4ImageBank.h`**

```cpp
/* Cm4ImageBank.h - a tiny registry over Multicore.switchImage(): register
 * several build-fixed CM4 images and switch the running CM4 between them by
 * handle. Distinct stage addresses stay co-resident (fast VTOR flip); images
 * sharing a stage address page (re-stage + evict). CM7-side; uses the global
 * Multicore. Public domain (author: Nicholas Newdigate). */
#ifndef Cm4ImageBank_h
#define Cm4ImageBank_h

#include <stdint.h>
#include <stddef.h>
#include "imxrt1176.h"     /* CM4_BOOT_ADDRESS */

/* The CM4 ITCM backdoor window (code images only; the DTCM half is data). */
#define CM4_ITCM_BACKDOOR_BASE   CM4_BOOT_ADDRESS   /* 0x20200000 */
#define CM4_ITCM_BACKDOOR_SIZE   0x20000u           /* 128 KB      */

#ifdef __cplusplus

struct Cm4ImageDesc {
    const void *blob;       /* embedded image in flash (e.g. cm4_ib_a[]) */
    uint32_t    bytes;      /* image length */
    uint32_t    stageAddr;  /* build-fixed backdoor alias = image's link base */
    const char *name;       /* optional, debug only (may be nullptr) */
};

class Cm4ImageBank {
public:
    /* Register an image. Returns a handle >= 0, or -1 on error: table full,
     * stageAddr/bytes outside the 128K ITCM backdoor window, or a partial
     * overlap with a *different* slot (same stageAddr = same slot, allowed). */
    int  add(const void *blob, uint32_t bytes, uint32_t stageAddr,
             const char *name = nullptr);

    /* Boot/switch the CM4 into `handle`. Fast VTOR flip (Multicore.switchImage)
     * if it is already staged in its slot and the CM4 is running; otherwise
     * (re)stage + boot via Multicore.begin(), evicting any same-slot image.
     * Returns true once the CM4 has left reset (hardware-level ready), false
     * on a bad handle or a reset timeout. */
    bool switchTo(int handle);

    int  current() const { return current_; }   /* running handle, -1 if none */
    bool isResident(int handle) const;           /* its slot currently holds it */
    int  count() const { return n_; }
    const char *name(int handle) const;          /* debug label, "" if none */

private:
    static constexpr int MAX_IMAGES = 16;        /* cap on *registered* images */
    Cm4ImageDesc imgs_[MAX_IMAGES];
    bool         resident_[MAX_IMAGES] = { false };
    int          n_ = 0;
    int          current_ = -1;
};

#endif /* __cplusplus */
#endif /* Cm4ImageBank_h */
```

- [ ] **Step 2: Write `Cm4ImageBank.cpp`**

```cpp
/* Cm4ImageBank.cpp - see Cm4ImageBank.h. Bookkeeping only; the boot machinery
 * is the HW-verified global Multicore. Public domain (N. Newdigate). */
#include "Cm4ImageBank.h"
#include "Multicore.h"

/* [a, a+alen) intersects [b, b+blen) ? */
static inline bool ranges_overlap(uint32_t a, uint32_t alen,
                                  uint32_t b, uint32_t blen)
{
    return a < b + blen && b < a + alen;
}

int Cm4ImageBank::add(const void *blob, uint32_t bytes, uint32_t stageAddr,
                      const char *name)
{
    if (n_ >= MAX_IMAGES) return -1;
    if (stageAddr < CM4_ITCM_BACKDOOR_BASE ||
        stageAddr + bytes > CM4_ITCM_BACKDOOR_BASE + CM4_ITCM_BACKDOOR_SIZE)
        return -1;
    for (int i = 0; i < n_; i++)                 /* partial overlap with a */
        if (imgs_[i].stageAddr != stageAddr &&   /* *different* slot = layout bug */
            ranges_overlap(stageAddr, bytes, imgs_[i].stageAddr, imgs_[i].bytes))
            return -1;
    imgs_[n_].blob = blob; imgs_[n_].bytes = bytes;
    imgs_[n_].stageAddr = stageAddr; imgs_[n_].name = name;
    resident_[n_] = false;
    return n_++;
}

bool Cm4ImageBank::switchTo(int h)
{
    if (h < 0 || h >= n_) return false;
    bool ok;
    if (resident_[h] && Multicore.running()) {           /* FAST PATH: VTOR flip */
        Multicore.switchImage(imgs_[h].stageAddr);
        ok = Multicore.running();
    } else {                                             /* STAGE PATH: copy + boot */
        ok = Multicore.begin(imgs_[h].blob, imgs_[h].bytes, imgs_[h].stageAddr);
        if (ok) {
            for (int i = 0; i < n_; i++)                 /* evict same-slot siblings */
                if (imgs_[i].stageAddr == imgs_[h].stageAddr) resident_[i] = false;
            resident_[h] = true;
        }
    }
    if (ok) current_ = h;
    return ok;
}

bool Cm4ImageBank::isResident(int h) const
{
    return (h >= 0 && h < n_) ? resident_[h] : false;
}

const char *Cm4ImageBank::name(int h) const
{
    if (h < 0 || h >= n_ || !imgs_[h].name) return "";
    return imgs_[h].name;
}
```

- [ ] **Step 3: Write `Cm4Slots.h`**

```cpp
/* Cm4Slots.h - uniform CM4 ITCM slot layout, the single source of truth shared
 * by the build (linker ORIGIN, via teensy_add_cm4_slot_image) and the runtime
 * (the backdoor stage address the CM7 hands Cm4ImageBank.add()). CM4_SLOT_SIZE
 * is supplied by the build (-DCM4_SLOT_SIZE=...) so both agree. Public domain. */
#ifndef Cm4Slots_h
#define Cm4Slots_h

#include "Cm4ImageBank.h"   /* CM4_ITCM_BACKDOOR_BASE */

#ifndef CM4_SLOT_SIZE
#error "CM4_SLOT_SIZE must be defined by the build (e.g. -DCM4_SLOT_SIZE=0x1000)"
#endif

/* Backdoor stage address of ITCM slot k (what Cm4ImageBank.add() consumes).
 * The matching CM4 image is linked at 0x1FFE0000 + k*CM4_SLOT_SIZE by
 * teensy_add_cm4_slot_image; that private-ITCM base aliases this backdoor. */
#define CM4_SLOT_STAGE(k)   (CM4_ITCM_BACKDOOR_BASE + (uint32_t)(k) * (CM4_SLOT_SIZE))

#endif /* Cm4Slots_h */
```

- [ ] **Step 4: Confirm the core library still compiles** (no consumer yet — build
  an existing cores consumer)

Run:
```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/cm4_hotswap2_test
rm -rf build && cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake >/tmp/ib_t1.log 2>&1 && cmake --build build >>/tmp/ib_t1.log 2>&1 && echo BUILD_OK || { tail -30 /tmp/ib_t1.log; echo BUILD_FAIL; }
```
Expected: `BUILD_OK` (the three new headers/cpp are part of `cores` and must not
break its compile; `Cm4Slots.h` is not included by anything here, so its
`#error` guard is dormant).

- [ ] **Step 5: Commit**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/cores
git add imxrt1176/Cm4ImageBank.h imxrt1176/Cm4ImageBank.cpp imxrt1176/Cm4Slots.h
git commit -m "feat(cm4): Cm4ImageBank - registry over Multicore.switchImage

Unified residency: fast VTOR flip when resident, stage+slot-scoped-evict
when not; hardware-level bool readiness. Cm4Slots.h binds the build slot
ORIGIN to the runtime stage address via one CM4_SLOT_SIZE. No Multicore
change.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 2: `teensy_add_cm4_slot_image` build helper

**Files:**
- Create: `teensy-cmake-macros/cm4_slot.ld.in`
- Modify: `teensy-cmake-macros/CMakeLists.include.txt` (after line 539)

- [ ] **Step 1: Write the slot linker template `cm4_slot.ld.in`**

(This is `cm4_hotswap2_test/cm4/cm4_a.ld` with `ORIGIN`/`LENGTH` parameterized;
DTCM + sections identical.)

```
/* GENERATED from cm4_slot.ld.in by teensy_add_cm4_slot_image -- do not edit.
 * A TCM-resident CM4 image in a fixed ITCM slot: code/vectors in ITCM at
 * @CM4_ITCM_ORIGIN@ (LENGTH @CM4_SLOT_SIZE@), data in the shared DTCM at
 * 0x20000000. .data VMA is DTCM but its LMA is appended after .text in ITCM,
 * so the whole loadable image is contiguous from @CM4_ITCM_ORIGIN@ and
 * objcopy -O binary yields exactly the bytes to stage at the backdoor alias
 * (0x20200000 + slot*size). Public domain (author: Nicholas Newdigate). */

MEMORY
{
    ITCM (rx) : ORIGIN = @CM4_ITCM_ORIGIN@, LENGTH = @CM4_SLOT_SIZE@
    DTCM (rw) : ORIGIN = 0x20000000, LENGTH = 128K
}

__StackTop = ORIGIN(DTCM) + LENGTH(DTCM);   /* 0x20020000: top of CM4 DTCM */

ENTRY(Reset_Handler)

SECTIONS
{
    .isr_vector : {
        . = ALIGN(4);
        KEEP(*(.isr_vector))
    } > ITCM

    .text : {
        . = ALIGN(4);
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > ITCM

    /* .data: VMA in DTCM, LMA appended after .text in ITCM. */
    .data : {
        . = ALIGN(4);
        __data_start__ = .;
        *(.data*)
        . = ALIGN(4);
        __data_end__ = .;
    } > DTCM AT> ITCM
    __data_load__ = LOADADDR(.data);

    .bss (NOLOAD) : {
        . = ALIGN(4);
        __bss_start__ = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end__ = .;
    } > DTCM
}
```

- [ ] **Step 2: Add `teensy_add_cm4_slot_image` to `CMakeLists.include.txt`**

Insert immediately after `teensy_target_link_cm4_image` (after the current line
539, the `endfunction()`):

```cmake
# teensy_add_cm4_slot_image(IMG_NAME SLOT <k> SLOT_SIZE <bytes> SOURCES <...>
#                           [INCLUDE_DIRS <...>] [DEFINES <...>])
# Like teensy_add_cm4_image, but places the image in a fixed, uniform ITCM
# "slot": generates a linker script from cm4_slot.ld.in with
#   ORIGIN = 0x1FFE0000 + SLOT*SLOT_SIZE,  LENGTH = SLOT_SIZE
# then delegates. The matching runtime backdoor stage address is
#   0x20200000 + SLOT*SLOT_SIZE   (Cm4Slots.h CM4_SLOT_STAGE(SLOT)).
# SLOT + SLOT_SIZE are the single source of truth binding the linker ORIGIN to
# the bank stageAddr; pass the SAME SLOT_SIZE as the firmware's -DCM4_SLOT_SIZE.
# Two images may share a SLOT (they page/evict each other): each gets its own
# generated <IMG_NAME>.ld with the same ORIGIN.
function(teensy_add_cm4_slot_image IMG_NAME)
    cmake_parse_arguments(SLOT "" "SLOT;SLOT_SIZE" "SOURCES;INCLUDE_DIRS;DEFINES" ${ARGN})
    if(NOT DEFINED SLOT_SLOT)
        message(FATAL_ERROR "teensy_add_cm4_slot_image(${IMG_NAME}): SLOT <index> is required")
    endif()
    if(NOT SLOT_SLOT_SIZE)
        message(FATAL_ERROR "teensy_add_cm4_slot_image(${IMG_NAME}): SLOT_SIZE <bytes> is required")
    endif()
    # CM4 private ITCM base: images relocate their own VTOR here (startup_cm4.S
    # sets SCB->VTOR = ORIGIN(ITCM)); the CM7 stages into the aliased backdoor.
    math(EXPR _origin "0x1FFE0000 + ${SLOT_SLOT} * ${SLOT_SLOT_SIZE}" OUTPUT_FORMAT HEXADECIMAL)
    set(CM4_ITCM_ORIGIN "${_origin}")
    set(CM4_SLOT_SIZE   "${SLOT_SLOT_SIZE}")
    set(_ld "${CMAKE_CURRENT_BINARY_DIR}/${IMG_NAME}.ld")
    configure_file("${TEENSY_CMAKE_MACROS_DIR}/cm4_slot.ld.in" "${_ld}" @ONLY)
    teensy_add_cm4_image(${IMG_NAME}
        LINKER       "${_ld}"
        SOURCES      ${SLOT_SOURCES}
        INCLUDE_DIRS ${SLOT_INCLUDE_DIRS}
        DEFINES      ${SLOT_DEFINES})
endfunction()
```

- [ ] **Step 3: Commit** (validated functionally by Task 3's build)

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/teensy-cmake-macros
git add cm4_slot.ld.in CMakeLists.include.txt
git commit -m "feat(cm4): teensy_add_cm4_slot_image - uniform ITCM slot images

Generates a per-image linker script from cm4_slot.ld.in with
ORIGIN=0x1FFE0000+SLOT*SLOT_SIZE, then delegates to teensy_add_cm4_image.
SLOT+SLOT_SIZE bind the linker ORIGIN to the runtime CM4_SLOT_STAGE.
teensy_add_cm4_image untouched (its 2B-cmp discipline preserved).

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 3: `cm4_imagebank_test` gate (RED → GREEN)

**Files:**
- Create: `evkb/cm4_imagebank_test/cm4/startup_cm4.S` (copy verbatim)
- Create: `evkb/cm4_imagebank_test/cm4/main_cm4.c` (copy verbatim)
- Create: `evkb/cm4_imagebank_test/toolchain/rt1170-evkb.toolchain.cmake` (copy)
- Create: `evkb/cm4_imagebank_test/clean_boot.scp` (copy verbatim)
- Create: `evkb/cm4_imagebank_test/CMakeLists.txt`
- Create: `evkb/cm4_imagebank_test/run_qemu.sh`
- Create: `evkb/cm4_imagebank_test/cm4_imagebank_test.cpp` (reporter)

- [ ] **Step 1: Scaffold — copy the reusable, image-agnostic artifacts**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb
mkdir -p cm4_imagebank_test/cm4
cp cm4_hotswap2_test/cm4/startup_cm4.S           cm4_imagebank_test/cm4/
cp cm4_hotswap2_test/cm4/main_cm4.c              cm4_imagebank_test/cm4/
cp -R cm4_hotswap2_test/toolchain               cm4_imagebank_test/
cp cm4_hotswap2_test/clean_boot.scp             cm4_imagebank_test/
```
(`startup_cm4.S` sets `VTOR = ORIGIN(ITCM)` so it works at any slot base;
`main_cm4.c` sends ready + `HS_IDENTITY` then parks in WFI; `clean_boot.scp`
dispatches the flexspi image at `0x30002000` with the M4 held — all
image-agnostic.)

- [ ] **Step 2: Write `CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.24)
project(cm4_imagebank_test)
set(TEENSY_VERSION 117 CACHE STRING "")
include(FetchContent)
FetchContent_Declare(teensy_cmake_macros SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../teensy-cmake-macros)
FetchContent_MakeAvailable(teensy_cmake_macros)
include(${teensy_cmake_macros_SOURCE_DIR}/CMakeLists.include.txt)

import_arduino_library(cores ${CMAKE_CURRENT_LIST_DIR}/../cores/imxrt1176)

# One ITCM-slot size drives BOTH the linker ORIGIN (per image, via
# teensy_add_cm4_slot_image) and the runtime CM4_SLOT_STAGE() the CM7 hands the
# bank -- single source of truth. 0x1000 = 4 KB -> 32 slots; images are ~700 B.
set(CM4_SLOT_SIZE 0x1000)

set(CM4_SRCS ${CMAKE_CURRENT_LIST_DIR}/cm4/startup_cm4.S
             ${CMAKE_CURRENT_LIST_DIR}/cm4/main_cm4.c)

teensy_add_cm4_slot_image(cm4_ib_a SLOT 0 SLOT_SIZE ${CM4_SLOT_SIZE}
    SOURCES ${CM4_SRCS} DEFINES HS_IDENTITY=0xA1A1A1A1u)
teensy_add_cm4_slot_image(cm4_ib_b SLOT 1 SLOT_SIZE ${CM4_SLOT_SIZE}
    SOURCES ${CM4_SRCS} DEFINES HS_IDENTITY=0xB2B2B2B2u)
teensy_add_cm4_slot_image(cm4_ib_c SLOT 2 SLOT_SIZE ${CM4_SLOT_SIZE}
    SOURCES ${CM4_SRCS} DEFINES HS_IDENTITY=0xC3C3C3C3u)
teensy_add_cm4_slot_image(cm4_ib_d SLOT 0 SLOT_SIZE ${CM4_SLOT_SIZE}  # shares A's slot
    SOURCES ${CM4_SRCS} DEFINES HS_IDENTITY=0xD4D4D4D4u)

teensy_add_executable(cm4_imagebank_test cm4_imagebank_test.cpp)
target_compile_definitions(cm4_imagebank_test.elf PRIVATE CM4_SLOT_SIZE=${CM4_SLOT_SIZE})
teensy_target_link_libraries(cm4_imagebank_test cores)
target_link_libraries(cm4_imagebank_test.elf stdc++)
teensy_target_link_cm4_image(cm4_imagebank_test cm4_ib_a)
teensy_target_link_cm4_image(cm4_imagebank_test cm4_ib_b)
teensy_target_link_cm4_image(cm4_imagebank_test cm4_ib_c)
teensy_target_link_cm4_image(cm4_imagebank_test cm4_ib_d)
```

- [ ] **Step 3: Write `run_qemu.sh`** (the gate = the failing test; it checks the
  full behaviour up front)

```sh
#!/bin/sh
# QEMU gate for Cm4ImageBank over Multicore.switchImage(). Four CM4 images in
# uniform ITCM slots: A/B/C in distinct slots 0/1/2 (co-resident), D sharing
# A's slot 0 (pages). switchTo() flips (no copy) when resident, stages+evicts
# when not. Each boot streams ready(CAFE0001)+identity over the MU; the CM7
# also logs the isResident() nibble (A=8,B=4,C=2,D=1). No qemu2 change -- the
# machine maps the full 256K ocram_m4 backdoor and re-reads GPR0/1 per boot.
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/cm4_imagebank_test.elf"
OUT="$DIR/cm4_imagebank.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/cm4_imagebank.dbg" &
P=$!; gate_pid $P
for _ in $(seq 1 40); do
    [ -f "$OUT" ] && grep -q "CM4IMAGEBANK-DONE" "$OUT" 2>/dev/null && break
    sleep 0.25
done
kill $P 2>/dev/null; wait $P 2>/dev/null || true

echo "==== captured UART ===="; [ -f "$OUT" ] && cat "$OUT" || echo "(no UART output)"

fail=0
check() {
    if grep -q "^$1" "$OUT"; then echo "PASS: $1"; else echo "FAIL: expected $1"; fail=1; fi
}
grep -q "CM4IMAGEBANK-GATE v1" "$OUT" || { echo "FAIL: banner missing"; exit 1; }
check "idA=A1A1A1A1"         # stage A (BT_RELEASE edge)
check "idB=B2B2B2B2"         # stage B (distinct slot; A resident)
check "idC=C3C3C3C3"         # stage C (distinct slot; A,B resident)
check "resABC=0000000E"      # isResident nibble: A,B,C set, D clear (co-residency)
check "idA2=A1A1A1A1"        # switchTo(A) -> FAST FLIP (A still resident)
check "idD=D4D4D4D4"         # switchTo(D) -> slot 0, evicts A
check "resD=00000007"        # B,C,D set, A clear (slot-scoped eviction)
check "idA3=A1A1A1A1"        # switchTo(A) -> RE-STAGE; ==A1 not D4 => re-copied
check "resA3=0000000E"       # A,B,C set, D clear (A re-staged, D evicted)
check "IMAGEBANK=PASS"
grep -q "CM4IMAGEBANK-DONE" "$OUT" || { echo "FAIL: DONE missing"; fail=1; }

if [ $fail -eq 0 ]; then
    echo "PASS: Cm4ImageBank co-residency + no-copy flip + slot-scoped eviction verified in QEMU"
else
    echo "GATE FAILED"; exit 1
fi
```
Then make it executable: `chmod +x run_qemu.sh`.

- [ ] **Step 4: Write the RED reporter** — only the co-residency phase (the
  eviction-phase tokens `idD`/`resD`/`idA3`/`resA3` are deliberately absent)

`cm4_imagebank_test.cpp`:
```cpp
/* cm4_imagebank_test: Cm4ImageBank over Multicore.switchImage -- 4 CM4 images in
 * uniform ITCM slots prove the unified residency model. A/B/C in distinct slots
 * 0/1/2 (co-resident); D shares A's slot 0 (pages). switchTo() flips (no copy)
 * when resident, stages+evicts when not.
 *   idA/idB/idC : stage each (A's is the BT_RELEASE edge); A,B,C co-resident.
 *   resABC=E    : isResident nibble A=8,B=4,C=2,D=1 -> A,B,C set (co-residency).
 *   idA2        : switchTo(A) after B,C -> FAST FLIP (A still resident).
 *   idD         : switchTo(D) -> slot 0, evicts A.
 *   resD=7      : B,C,D set, A clear (slot-scoped eviction).
 *   idA3        : switchTo(A) -> RE-STAGE. ==A1 (not D4) proves A was re-copied,
 *                 not a stale flip onto D's slot -- the un-fakeable evict proof.
 *   resA3=E     : A,B,C set, D clear (A re-staged, D evicted).
 * PASS = all identities correct AND resABC/resD/resA3 == E/7/E. Public domain. */
#include "Arduino.h"
#include "core_pins.h"
#include "HardwareSerial.h"
#include "Multicore.h"
#include "MessagingUnit.h"
#include "Cm4ImageBank.h"
#include "Cm4Slots.h"
#include "cm4_ib_a.h"
#include "cm4_ib_b.h"
#include "cm4_ib_c.h"
#include "cm4_ib_d.h"

#define WAIT_LONG 3000000u

static void phex(const char *k, uint32_t v)
{
    Serial1.print(k); Serial1.print('=');
    for (int i = 28; i >= 0; i -= 4)
        Serial1.print("0123456789ABCDEF"[(v >> i) & 0xF]);
    Serial1.println();
}
static void ptimeout(const char *k) { Serial1.print(k); Serial1.println("=TIMEOUT"); }
static bool wait_recv(uint8_t ch, uint32_t *out)
{
    for (uint32_t n = WAIT_LONG; n; n--)
        if (MU.tryReceive(ch, out)) return true;
    return false;
}
/* Read a boot's {ready, identity}; return the identity (0xFFFFFFFF on timeout). */
static uint32_t read_identity(const char *tag, bool *ok)
{
    uint32_t r = 0, id = 0xFFFFFFFFu;
    if (!wait_recv(0, &r)) { ptimeout(tag); *ok = false; return id; }
    if (wait_recv(0, &id)) phex(tag, id); else { ptimeout(tag); *ok = false; }
    return id;
}

static Cm4ImageBank bank;
static int hA, hB, hC, hD;

/* isResident nibble: A=8, B=4, C=2, D=1. */
static uint32_t resmap(void)
{
    return ((uint32_t)bank.isResident(hA) << 3) | ((uint32_t)bank.isResident(hB) << 2)
         | ((uint32_t)bank.isResident(hC) << 1) |  (uint32_t)bank.isResident(hD);
}
/* switchTo + read this image's identity; flags a switch failure. */
static uint32_t sw(int h, const char *tag, bool *ok)
{
    if (!bank.switchTo(h)) { Serial1.print(tag); Serial1.println("=SWFAIL"); *ok = false; return 0xFFFFFFFFu; }
    return read_identity(tag, ok);
}

void setup()
{
    Serial1.begin(115200);
    Serial1.println("CM4IMAGEBANK-GATE v1");
    MU.begin();
    bool ok = true;

    hA = bank.add(cm4_ib_a, sizeof(cm4_ib_a), CM4_SLOT_STAGE(0), "A");
    hB = bank.add(cm4_ib_b, sizeof(cm4_ib_b), CM4_SLOT_STAGE(1), "B");
    hC = bank.add(cm4_ib_c, sizeof(cm4_ib_c), CM4_SLOT_STAGE(2), "C");
    hD = bank.add(cm4_ib_d, sizeof(cm4_ib_d), CM4_SLOT_STAGE(0), "D");   /* shares A's slot 0 */
    if (hA < 0 || hB < 0 || hC < 0 || hD < 0) { Serial1.println("ADD=FAIL"); ok = false; }

    uint32_t idA = sw(hA, "idA", &ok);          /* stage A (BT_RELEASE edge) */
    uint32_t idB = sw(hB, "idB", &ok);          /* stage B (distinct slot; A resident) */
    uint32_t idC = sw(hC, "idC", &ok);          /* stage C (distinct slot; A,B resident) */
    uint32_t resABC = resmap();  phex("resABC", resABC);   /* expect E: A,B,C */

    uint32_t idA2 = sw(hA, "idA2", &ok);        /* A resident -> FAST FLIP */

    /* RED: eviction phase not yet implemented. */
    bool pass = ok && idA == 0xA1A1A1A1u && idB == 0xB2B2B2B2u && idC == 0xC3C3C3C3u
                   && idA2 == 0xA1A1A1A1u && resABC == 0xEu;
    Serial1.println(pass ? "IMAGEBANK=PASS" : "IMAGEBANK=FAIL");
    Serial1.println("CM4IMAGEBANK-DONE");
}
void loop() {}
```

- [ ] **Step 5: Build + run the gate to confirm RED**

Run:
```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/cm4_imagebank_test
rm -rf build && cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake >/tmp/ib_t3.log 2>&1 && cmake --build build >>/tmp/ib_t3.log 2>&1 && echo BUILD_OK || { tail -30 /tmp/ib_t3.log; echo BUILD_FAIL; }
./run_qemu.sh; echo "exit=$?"
```
Expected: `BUILD_OK`, then the gate prints `FAIL: expected idD=D4D4D4D4` (and
`resD`, `idA3`, `resA3`), `GATE FAILED`, `exit=1`. This proves the gate's
assertions bite (co-residency tokens present, eviction tokens absent).

- [ ] **Step 6: Complete the reporter — add the eviction phase (GREEN)**

Replace the block from `uint32_t idA2 = sw(...)` through `Serial1.println(pass ? ...)`
with:
```cpp
    uint32_t idA2 = sw(hA, "idA2", &ok);        /* A resident -> FAST FLIP */
    uint32_t idD  = sw(hD, "idD", &ok);         /* slot 0 -> evicts A */
    uint32_t resD = resmap();  phex("resD", resD);         /* expect 7: B,C,D */

    uint32_t idA3 = sw(hA, "idA3", &ok);        /* A not resident -> RE-STAGE (==A1, not D4) */
    uint32_t resA3 = resmap(); phex("resA3", resA3);       /* expect E: A,B,C */

    bool pass = ok
        && idA == 0xA1A1A1A1u && idB == 0xB2B2B2B2u && idC == 0xC3C3C3C3u
        && idA2 == 0xA1A1A1A1u && idD == 0xD4D4D4D4u && idA3 == 0xA1A1A1A1u
        && resABC == 0xEu && resD == 0x7u && resA3 == 0xEu;
```

- [ ] **Step 7: Rebuild + run to confirm GREEN, stable 3×**

Run:
```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/cm4_imagebank_test
cmake --build build >/tmp/ib_t3b.log 2>&1 && echo BUILD_OK || { tail -30 /tmp/ib_t3b.log; echo BUILD_FAIL; }
for i in 1 2 3; do echo "--- run $i ---"; ./run_qemu.sh >/tmp/ib_run$i.log 2>&1; echo "exit=$?"; grep -E "IMAGEBANK=PASS|GATE FAILED" /tmp/ib_run$i.log; done
```
Expected each run: `exit=0` and `IMAGEBANK=PASS`. Then save the transcript:
```bash
cp cm4_imagebank.uart transcript_qemu.txt
```
(Confirm `transcript_qemu.txt` contains `idA=A1A1A1A1 … resA3=0000000E …
IMAGEBANK=PASS`.)

- [ ] **Step 8: Commit**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb
git add cm4_imagebank_test
git commit -m "test(cm4): cm4_imagebank_test gate - co-residency + eviction (QEMU GREEN)

Cm4ImageBank drives 4 slot images: A/B/C co-resident in slots 0/1/2, D
shares A's slot 0. Proves fast flip (idA2), slot-scoped eviction (resD=7),
and re-stage (idA3==A1A1A1A1 not D4 -- un-fakeable). No qemu2 change.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 4: license audit + repo regression

**Files:**
- Modify: `evkb/tools/license-audit.sh:60`

- [ ] **Step 1: Add the gate to `GATES`**

In `tools/license-audit.sh` line 60, append ` cm4_imagebank_test:cm4_imagebank_test`
inside the closing quote — i.e. change the tail
`… cm4_hotswap2_test:cm4_hotswap2_test"` to
`… cm4_hotswap2_test:cm4_hotswap2_test cm4_imagebank_test:cm4_imagebank_test"`.

- [ ] **Step 2: Run the audit → PASS**

Run:
```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb
./tools/license-audit.sh 2>&1 | tail -5
```
Expected: `LICENSE-AUDIT: PASS` (the new sources are clean-room public-domain;
the macro-built CM4 sources are covered by the existing `-MMD` depfile walk).

- [ ] **Step 3: Regression — the sibling Multicore gates still build**

Run:
```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb
for t in cm4_hotswap_test cm4_hotswap2_test; do
  ( cd $t && rm -rf build && cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake >/tmp/reg_$t.log 2>&1 && cmake --build build >>/tmp/reg_$t.log 2>&1 && echo "$t OK" || { tail -20 /tmp/reg_$t.log; echo "$t FAIL"; } )
done
```
Expected: `cm4_hotswap_test OK` and `cm4_hotswap2_test OK` (the new core files +
the added macro function must not regress existing consumers).

- [ ] **Step 4: Commit**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb
git add tools/license-audit.sh
git commit -m "chore(cm4): license-audit covers cm4_imagebank_test (PASS)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 5: README + roadmap docs

**Files:**
- Create: `evkb/cm4_imagebank_test/README.md`
- Modify: `.claude/skills/cm4-bringup/references/cm4-roadmap.md`

- [ ] **Step 1: Write `cm4_imagebank_test/README.md`**

```markdown
# cm4_imagebank_test — Cm4ImageBank over switchImage (QEMU GREEN; HW pending)

A small CM7-side registry (`Cm4ImageBank`, `cores/imxrt1176`) over the
HW-verified `Multicore` boot machinery: register several build-fixed CM4 images
in **uniform ITCM slots** and switch the running CM4 among them by handle —
a **fast no-copy VTOR flip** when the target is resident, a **stage + evict**
when it is not. No `Multicore` or qemu2 change.

## Layout (uniform slots)

`CM4_SLOT_SIZE` (one CMake knob; 4 KB here → 32 slots) drives BOTH the linker
`ORIGIN` (per image, via `teensy_add_cm4_slot_image`) and the runtime
`CM4_SLOT_STAGE(k)` the CM7 hands `bank.add()` — one source of truth.

| Image | Slot | link `ORIGIN` | stage (`CM4_SLOT_STAGE`) | Identity |
|-------|------|---------------|--------------------------|----------|
| A | 0 | `0x1FFE0000` | `0x20200000` | `0xA1A1A1A1` |
| B | 1 | `0x1FFE1000` | `0x20201000` | `0xB2B2B2B2` |
| C | 2 | `0x1FFE2000` | `0x20202000` | `0xC3C3C3C3` |
| D | **0** (shares A) | `0x1FFE0000` | `0x20200000` | `0xD4D4D4D4` |

## What it proves

- **Co-residency + no-copy flip:** A/B/C in distinct slots stay resident
  (`resABC=E`); `switchTo(A)` after B/C is a flip (`idA2=A1A1A1A1`).
- **Slot-scoped eviction:** `switchTo(D)` (slot 0) evicts A only (`resD=7` —
  B,C,D resident, A gone).
- **Re-stage (un-fakeable):** `switchTo(A)` after D re-copies A onto slot 0;
  `idA3=A1A1A1A1` (NOT `D4D4D4D4`) proves the bank re-staged rather than flipping
  onto D's resident blob. `resA3=E`.

## Gates

- **QEMU:** `cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake && cmake --build build && ./run_qemu.sh` (never `sh run_qemu.sh`). No qemu2 change — the machine maps the full 256 KB `ocram_m4` backdoor and re-reads `GPR0/1` fresh per boot. GREEN, stable 3× (`transcript_qemu.txt`).
- **HW — clean controlled probe (wiring-free):** `clean_boot.scp` (CM4 held: `SCR=0`/`STAT_M4=1`), flash, dispatch, capture VCOM:
  ```sh
  pkill LinkServer; pkill redlinkserv
  # start the pyserial VCOM reader on /dev/cu.usbmodem5DQ2DDHVWO5EI3 @115200
  LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load build/cm4_imagebank_test.elf
  LinkServer probe <serial> runscript clean_boot.scp
  ```

## What this unlocks

An A/B (or A/B/C/…) CM4 firmware bank switched by handle with a fast VTOR flip;
paging more images than fit resident by sharing slots. Foundation for
SD-loaded CM4 images (spec §8): the bank's `blob` is already source-agnostic.
```

- [ ] **Step 2: Add a roadmap entry** for the new capability

In `.claude/skills/cm4-bringup/references/cm4-roadmap.md`, under the
current-status / new-capability section, add a `cm4_imagebank_test` line: what it
is (`Cm4ImageBank` over `switchImage`; unified residency; uniform slots via
`teensy_add_cm4_slot_image`), status (**QEMU GREEN; HW probe pending** — Task 6),
and a session-log line dated 2026-07-20 pointing at the spec + this plan. Keep the
"Future: SD-loaded images" pointer (spec §8). (Match the file's existing entry
format; do not restructure it.)

- [ ] **Step 3: Commit**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb
git add cm4_imagebank_test/README.md .claude/skills/cm4-bringup/references/cm4-roadmap.md
git commit -m "docs(cm4): cm4_imagebank_test README + roadmap entry (QEMU GREEN)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 6: EVKB clean-boot probe (HARDWARE — user runs) + close-out

**Files:**
- Create: `evkb/cm4_imagebank_test/transcript_hw_evkb.txt`
- Modify: roadmap + memory (flip to HW-VERIFIED)

**Probe rationale (`cm4-bringup`):** no new register dependency, no qemu2 model,
no new clock/memory-alias fact — but "co-resident + evict across ≥3 slots" is a
**new usage pattern** of the backdoor (more slots than hotswap2's two), so the
wiring-free clean-boot probe runs. Silicon is the arbiter that the whole 128 KB
window pages as modelled.

- [ ] **Step 1: Flash + clean-boot probe on the EVKB** (user-run; the assistant
  cannot drive the hardware)

```sh
pkill LinkServer; pkill redlinkserv
# start the pyserial VCOM reader on /dev/cu.usbmodem5DQ2DDHVWO5EI3 @115200 FIRST
LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load /Users/nicholasnewdigate/Development/rt1170/evkb/cm4_imagebank_test/build/cm4_imagebank_test.elf
LinkServer probe <serial> runscript /Users/nicholasnewdigate/Development/rt1170/evkb/cm4_imagebank_test/clean_boot.scp
```
Expected VCOM: the clean-boot snapshot shows `SCR = 0` / `STAT_M4 = 1` (CM4 held
— no contamination), then `idA=A1A1A1A1 … resABC=0000000E … idA2=A1A1A1A1 …
idD=D4D4D4D4 … resD=00000007 … idA3=A1A1A1A1 … resA3=0000000E … IMAGEBANK=PASS …
CM4IMAGEBANK-DONE`. Save the capture to `transcript_hw_evkb.txt`.

- [ ] **Step 2: Diff HW vs QEMU** — the asserted tokens must be **byte-identical**
  (this design has no world-varying token; every value is deterministic on both
  sides). Any divergence → stop and apply the `cm4-bringup` silicon-truth loop
  (silicon wins) before proceeding.

- [ ] **Step 3: Flip docs + memory to HW-VERIFIED**

Update the roadmap entry (Task 5) to **★★HW-VERIFIED 2026-07-20** with the
transcript pointer, and update memory: extend `rt1176-cm4-boot-mu` (or add
`rt1176-cm4-imagebank`) noting the bank + `teensy_add_cm4_slot_image` + the
uniform-slot layout are HW-verified, with a `[[rt1176-cm4-boot-mu]]` link and a
`MEMORY.md` index line. (Convert any relative date to absolute.)

- [ ] **Step 4: Commit**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb
git add cm4_imagebank_test/transcript_hw_evkb.txt cm4_imagebank_test/README.md .claude/skills/cm4-bringup/references/cm4-roadmap.md
git commit -m "test(cm4): cm4_imagebank_test HW-VERIFIED on EVKB (clean-boot probe)

Wiring-free clean_boot probe: SCR=0/STAT_M4=1 (CM4 held), then
IMAGEBANK=PASS with tokens byte-identical to QEMU. Co-residency +
no-copy flip + slot-scoped eviction confirmed on silicon.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Verification checklist (per `cm4-bringup`)

- [ ] QEMU gate green, asserted tokens, stable 3× (Task 3).
- [ ] No qemu2 change → no qemu2 regression set needed; GPL firewall trivially
      clean (Task 3).
- [ ] License audit PASS with the new gate covered (Task 4).
- [ ] Sibling Multicore gates still build (Task 4).
- [ ] EVKB clean-boot probe green, tokens byte-identical to QEMU (Task 6).
- [ ] Roadmap + memory left true (Tasks 5–6).

## Risks (from the spec §7)

- Stale MU token false-pass → distinct identities, full-sequence assert.
- Wrong flip/stage decision → `isResident` nibble asserted; the D→A re-stage
  yields `idA3=A1` only if eviction+re-stage is correct (else `D4`).
- Position-dependence (`stageAddr` ≠ link `ORIGIN`) → `CM4_SLOT_STAGE`/`SLOT`
  single source of truth; the gate boots each image and checks identity.
- `MAX_IMAGES` too small → compile-time bump; `add()` returns −1, never overflows.
```
