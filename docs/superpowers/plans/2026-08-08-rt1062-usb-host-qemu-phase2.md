# RT1062 USB Host in QEMU (Phase 2) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `rt1062:usb/usb_descriptor_survey` enumerates QEMU's emulated
`usb-audio` device on the `mimxrt1060-evk` machine and reads `46F4:0002` off
the wire — proving the RT1062 USB host path in emulation.

**Architecture:** Two changes to `hw/arm/fsl-imxrt1062.c`, both mirroring what
`fsl-imxrt1170.c` already does for OTG2: a stable `usbhost` device id so the
EHCI host bus is named deterministically, and a restricted DMA address space
with ITCM/DTCM holes so the model cannot pretend the USB master can reach
CPU-private TCM. The firmware side needs no source changes — only a toolchain
file, a `boards` sidecar, and a `TEENSY_VERSION` guard.

**Tech Stack:** QEMU (C, `MemoryRegion`/`AddressSpace`), CMake, ARM GCC 10,
POSIX sh gates.

**Spec:** `docs/superpowers/specs/2026-08-08-rt1062-usb-host-qemu-design.md`

---

## Critical context for the implementer

- **The GPL firewall is absolute.** `~/Development/qemu2` is GPL-2.0. Its code
  must NEVER be copied into this MIT/BSD firmware repo. Firmware→qemu2 is fine;
  the reverse is not. **Never push qemu2.** Its changes stay local by design,
  which is why a fresh clone will see this gate red.
- **Run gates as `./run_qemu.sh`, never `sh run_qemu.sh`** (`gate_init` does
  `exec gtimeout ... "$0"`).
- **Ask the runner for gate counts** (`./tools/run-all-qemu-gates.sh -l`), never
  a bare `find`. Its trailing `(N gate(s))` line means `wc -l` is one more.
- **Zero SKIP is load-bearing.**
- `dualcore/cm4_audio_test` is a documented load-sensitive intermittent
  (`docs/KNOWN-BROKEN-GATES.md`). It is the one permitted failure.
- `cores/` and `teensy-cmake-macros/` untracked is NORMAL.

**Two things this plan deliberately does NOT do, despite precedent suggesting
them:**

1. **No hotplug.** Phase 7.1 learned that a device present at reset raises PCD
   before the firmware is listening. That lesson applies to gates asserting an
   interrupt or plug **edge**. This gate asserts **enumeration**, and it already
   passes on rt1176 with `-device usb-audio,bus=usbhost.0,port=1` present from
   startup. Do not add hotplug machinery it does not need.
2. **No firmware source changes.** The RT1062 is USBHost_t36's home silicon.

---

### Task 0: Baseline

- [ ] **Step 1: Confirm branch, tree, and the rt1176 gate**

```bash
cd ~/Development/rt1170/evkb && git branch --show-current && git status --short
./tools/run-all-qemu-gates.sh -l | tail -1
./tools/run-all-qemu-gates.sh usb/usb_descriptor_survey
```

Expected: branch `rt1060-board-axis`; only `?? cores/` and
`?? teensy-cmake-macros/`; `(82 gate(s))`; and
`PASS  rt1176:usb/usb_descriptor_survey`.

- [ ] **Step 2: Record the qemu2 baseline commit**

```bash
git -C ~/Development/qemu2 log --oneline -1 && git -C ~/Development/qemu2 status --short | head
```

Expected: a commit SHA and a clean-ish tree. Note the SHA — if this task's
qemu2 work must be reverted, that is where to go back to.

---

### Task 1: Give the survey an rt1062 build, and watch the gate go RED

**Files:**
- Create: `examples/usb/usb_descriptor_survey/toolchain/rt1062-evkb.toolchain.cmake`
- Create: `examples/usb/usb_descriptor_survey/boards`
- Modify: `examples/usb/usb_descriptor_survey/CMakeLists.txt:4`
- Create: `examples/usb/usb_descriptor_survey/transcript_qemu_red.txt`

- [ ] **Step 1: Create the toolchain file**

This is a verbatim copy of `examples/serial/serial_test/toolchain/rt1062-evkb.toolchain.cmake`
— both examples sit at the same depth (`../../../evkb.cmake`), so the
`get_filename_component` walk to the repo root is identical.

```cmake
# Toolchain file for the NXP MIMXRT1060-EVKB (i.MX RT1062, Cortex-M7).
# Mirrors rt1170-evkb.toolchain.cmake but selects TEENSY_VERSION 42, which
# teensy-cmake-macros already maps to the teensy4 core and imxrt1060_evkb.ld
# (CMakeLists.include.txt:86-89) -- no macro change is needed for this board.
set(TEENSY_VERSION 42 CACHE STRING "RT1062 / MIMXRT1060-EVKB" FORCE)
set(CPU_CORE_SPEED 600000000 CACHE STRING "RT1062 M7 core clock" FORCE)

# Point the macros at the LOCAL teensy4 core. COREPATH must end with a trailing
# slash: the macros build LINKER_FILE as "${COREPATH}imxrt1060_evkb.ld".
# NB: cache type STRING, not PATH -- CMake normalises PATH cache entries and
# strips the trailing slash, but the macros concatenate raw strings.
get_filename_component(_evkb_root "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(COREPATH "${_evkb_root}/cores/teensy4/" CACHE STRING "teensy4 core path" FORCE)

set(CMAKE_EXE_LINKER_FLAGS "--specs=nano.specs" CACHE INTERNAL "")
if(DEFINED ENV{ARM_TOOLCHAIN_BIN})
    set(COMPILERPATH "$ENV{ARM_TOOLCHAIN_BIN}/")
else()
    set(COMPILERPATH "/Applications/ARM_10/bin/")
endif()
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_TRY_COMPILE_TARGET_TYPE "STATIC_LIBRARY")
set(CMAKE_C_COMPILER ${COMPILERPATH}arm-none-eabi-gcc)
set(CMAKE_CXX_COMPILER ${COMPILERPATH}arm-none-eabi-g++)
set(CMAKE_CXX_LINK_EXECUTABLE "${CMAKE_C_COMPILER} <FLAGS> <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")
```

- [ ] **Step 2: Guard the TEENSY_VERSION fallback**

`examples/usb/usb_descriptor_survey/CMakeLists.txt:4` currently reads
`set(TEENSY_VERSION 117 CACHE STRING "")`, unconditionally. Replace that single
line with:

```cmake
# Fallback only: the toolchain file sets this FORCE-fully, so a toolchain-driven
# build wins. A bare `cmake -B build .` with no toolchain still selects 117.
if(NOT DEFINED TEENSY_VERSION)
    set(TEENSY_VERSION 117 CACHE STRING "")
endif()
```

Left unguarded this silently builds an **RT1176** image into `build-rt1062/`,
which then boots the wrong QEMU machine and fails in a way that reads as a
board or model problem rather than a build misconfiguration.

- [ ] **Step 3: Declare the boards**

Create `examples/usb/usb_descriptor_survey/boards`:

```
# Boards this example is built and gated for. See
# docs/superpowers/specs/2026-08-08-rt1062-usb-host-qemu-design.md
rt1176
rt1062
```

- [ ] **Step 4: Build for rt1062**

```bash
cd ~/Development/rt1170/evkb/examples/usb/usb_descriptor_survey
cmake -B build-rt1062 -DEVKB_BOARD=rt1062 \
      -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1062-evkb.toolchain.cmake
cmake --build build-rt1062
```

Expected: `build-rt1062/usb_descriptor_survey.elf` produced.

Confirm it is genuinely an RT1062 image, not an RT1176 one — the entry point is
the clincher, because it comes from the linker script:

```bash
/Applications/ARM_10/bin/arm-none-eabi-readelf -h build-rt1062/usb_descriptor_survey.elf | grep Entry
/Applications/ARM_10/bin/arm-none-eabi-readelf -h build/usb_descriptor_survey.elf | grep Entry
```

Expected: `0x60001000` for rt1062 (FlexSPI base `0x60000000` + IVT offset) and
`0x30001000` for rt1176. If they match, the toolchain was ignored — recheck
Step 2.

- [ ] **Step 5: Verify the rt1176 image did not move**

```bash
cd ~/Development/rt1170/evkb/examples/usb/usb_descriptor_survey && cmake --build build >/dev/null
cd ~/Development/rt1170/evkb && ./tools/run-all-qemu-gates.sh usb/usb_descriptor_survey
```

Expected: `PASS  rt1176:usb/usb_descriptor_survey`, and a **FAIL** for
`rt1062:usb/usb_descriptor_survey`. Two gates now, one green one red.

- [ ] **Step 6: Capture the RED transcript and confirm WHY it is red**

```bash
cd ~/Development/rt1170/evkb/examples/usb/usb_descriptor_survey
EVKB_BOARD=rt1062 ./run_qemu.sh > transcript_qemu_red.txt 2>&1 || true
head -30 transcript_qemu_red.txt
```

**The red must be for the stated cause.** Expected: QEMU refuses to start
because `bus=usbhost.0` does not resolve on this machine — a message naming
`usbhost.0` (e.g. `Bus 'usbhost.0' not found`). Then the gate reports
`FAIL: no UART capture ... QEMU produced no serial output`.

If instead the capture shows firmware output that stops somewhere, the red is a
DIFFERENT problem (a stalled peripheral, as SEMC was in Phase 1) and you must
diagnose it before proceeding — see Task 4's diagnostic recipe. **Do not
proceed on a red whose cause you have not confirmed.** A red that would have
happened anyway proves nothing.

Prepend a header to the transcript explaining what it is:

```
RED transcript, committed BEFORE the qemu2 change that makes this gate pass.

The mimxrt1060-evk machine does not name its OTG2 host bus, so
"-device usb-audio,bus=usbhost.0" cannot resolve and QEMU exits before the
firmware runs. This is the red the Phase 2 model change fixes; it is checked in
so the gate is provably failing for THIS reason and not vacuously.

Recorded on branch rt1060-board-axis, evkb <SHA>, qemu2 <SHA>.
```

Replace `<SHA>` with the actual values from Task 0.

- [ ] **Step 7: Commit the red**

Write the message to a scratch file and use `git commit -F` (inline heredocs
with apostrophes have caused shell parse errors in this repo).

```
usb_descriptor_survey: declare rt1062, RED against the unwired 1060 machine

The survey now builds for the MIMXRT1060-EVKB and its gate runs on both boards.
On rt1062 it FAILS, and the red transcript is checked in before the fix: the
mimxrt1060-evk machine gives its OTG2 controller no device id, so its EHCI host
bus is not named "usbhost.0" and the emulated usb-audio device cannot attach.

Committed red-first on purpose, as Phase 7.1 was. A gate that only ever ran
green after the change would not show it fails for the stated reason.

No firmware source changed -- the RT1062 is USBHost_t36's home silicon. The
CMakeLists' TEENSY_VERSION fallback is now guarded, though: unguarded it caches
117 and would build an RT1176 image into build-rt1062/, which boots the wrong
machine and fails looking like a board problem.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
```

Stage: the toolchain file, `boards`, `CMakeLists.txt`, `transcript_qemu_red.txt`.

---

### Task 2: Name the OTG2 host bus `usbhost` (qemu2)

**Files:**
- Modify: `~/Development/qemu2/hw/arm/fsl-imxrt1062.c` (USB realize loop, ~line 614-627)

- [ ] **Step 1: Read the current USB realize loop**

```bash
sed -n '610,630p' ~/Development/qemu2/hw/arm/fsl-imxrt1062.c
```

You should see a `for (int i = 0; i < FSL_IMXRT1062_NUM_USBS; i++)` loop that
realizes `&s->usb[i]`, maps it at `usb_base[i]`, connects `usb_irq[i]`, then
realizes and maps `&s->usbphy[i]`.

- [ ] **Step 2: Add the id before the controller is realized**

Inside that loop, **before** the `sysbus_realize(sbd, errp)` call for the USB
controller, insert:

```c
        /*
         * Give USB_OTG2 (i==1) a stable device id so its child USB buses get
         * deterministic names.  qbus names a nameless bus "<parent-id>.<n>"
         * when the parent has an id, else it falls back to a global
         * "usb-bus.<N>" counter whose number depends on realize order
         * (fragile, and not something a gate can name).  The EHCI *host* bus
         * is the controller's first child bus, created in the parent realize,
         * so it becomes "usbhost.0" -- what "-device usb-audio,bus=usbhost.0"
         * binds to.  USB_OTG1 (i==0) is deliberately left WITHOUT an id so the
         * "-chardev ...,id=usb0" CDC-ACM console path on this board
         * (mimxrt1060-evk.c) is completely unchanged.
         *
         * Must be set BEFORE realize: the bus is created during realize and
         * takes its name from the parent's id at that moment.
         */
        if (i == 1) {
            DEVICE(&s->usb[i])->id = g_strdup("usbhost");
        }
```

- [ ] **Step 3: Rebuild QEMU**

```bash
cd ~/Development/qemu2/build && ninja qemu-system-arm 2>&1 | tail -5
```

Expected: a successful build. If ninja is not configured, use `make -j4
qemu-system-arm` instead.

- [ ] **Step 4: Run the rt1062 gate**

```bash
cd ~/Development/rt1170/evkb/examples/usb/usb_descriptor_survey
EVKB_BOARD=rt1062 ./run_qemu.sh; echo "EXIT=$?"
```

Two outcomes are both informative, and you must report which one you got:

- **PASS** — the bus id was the only thing missing. Proceed to Task 3.
- **FAIL, but now with firmware output in the capture** — the device attaches
  and the machine boots, but something later stalls. This is progress, not a
  regression. Diagnose with Task 4's recipe and report before proceeding.

**If it still fails with `Bus 'usbhost.0' not found`**, the id is not taking
effect — most likely it was set after realize. Recheck Step 2's placement.

- [ ] **Step 5: Confirm rt1176 is unaffected**

```bash
cd ~/Development/rt1170/evkb && ./tools/run-all-qemu-gates.sh usb/
```

Expected: every `rt1176:usb/...` gate still PASSes. The 1170 model was not
touched, but the shared chipidea/EHCI code paths were exercised differently, so
verify rather than assume.

- [ ] **Step 6: Commit (qemu2 only — NEVER push)**

```bash
cd ~/Development/qemu2
git add hw/arm/fsl-imxrt1062.c
git commit -F <scratch-file>
```

Message:

```
hw/arm/fsl-imxrt1062: name OTG2's host bus "usbhost"

Mirrors what fsl-imxrt1170.c already does for its OTG2.  Without a device id,
qbus names the controller's child bus from a global "usb-bus.<N>" counter whose
value depends on realize order, so no gate can reliably attach a device to it.
With the id, the EHCI host bus -- the controller's first child bus -- becomes
"usbhost.0".

OTG1 is deliberately left id-less so this board's "-chardev ...,id=usb0"
CDC-ACM console path is unchanged.

The EHCI host controller itself was never missing: TYPE_CHIPIDEA derives from
TYPE_SYS_BUS_EHCI and is shared with the 1170, so host support already existed
underneath both SoCs.  Only this wiring on top was absent.
```

---

### Task 3: Model the TCM holes in the USB DMA path (qemu2)

**Files:**
- Modify: `~/Development/qemu2/include/hw/arm/fsl-imxrt1062.h` (state struct)
- Modify: `~/Development/qemu2/hw/arm/fsl-imxrt1062.c` (new function + realize hook)

- [ ] **Step 1: Add the state fields**

In `include/hw/arm/fsl-imxrt1062.h`, in `struct FslIMXRT1062State`, immediately
after the existing `MemoryRegion sdram;` line, add:

```c
    /* Restricted view of memory for OTG2's EHCI DMA master: system memory with
     * the CPU-private TCMs punched out (see fsl_imxrt1062_init_usb_host_dma). */
    MemoryRegion usb_dma_view;
    MemoryRegion usb_dma_sysmem;
    MemoryRegion usb_dma_itcm_hole;
    MemoryRegion usb_dma_dtcm_hole;
    AddressSpace usb_dma_as;
```

- [ ] **Step 2: Add the hole ops and the init function**

In `hw/arm/fsl-imxrt1062.c`, near the top (after the includes and before the
peripheral base-address tables), add:

```c
/*
 * The Cortex-M7's TCMs are CPU-private: no other AHB master, including the USB
 * controller, can reach them on silicon.  Model that, so firmware handing a
 * TCM-resident buffer to a transfer fails HERE rather than only on the board.
 *
 * This matters more on the MIMXRT1060-EVKB than it does on the 1170: that
 * board's linker script (cores/teensy4/imxrt1060_evkb.ld) puts _estack in
 * DTCM, so EVERY stack buffer is TCM-resident.  Note this is a separate
 * question from USBHost_t36's static pools, which land in .bss -- and .bss is
 * OCRAM on this board, which is why its __IMXRT1176__-only DMAMEM guards are
 * correctly inert here.  Both facts are true at once.
 */
static uint64_t fsl_imxrt1062_tcm_hole_read(void *opaque, hwaddr addr,
                                            unsigned size)
{
    qemu_log_mask(LOG_GUEST_ERROR,
        "fsl-imxrt1062: USB DMA read from CPU-private TCM @ 0x%08" HWADDR_PRIx
        " (unreachable by peripheral DMA on silicon)\n", addr);
    return 0;
}

static void fsl_imxrt1062_tcm_hole_write(void *opaque, hwaddr addr,
                                         uint64_t val, unsigned size)
{
    qemu_log_mask(LOG_GUEST_ERROR,
        "fsl-imxrt1062: USB DMA write to CPU-private TCM @ 0x%08" HWADDR_PRIx
        " (unreachable by peripheral DMA on silicon)\n", addr);
}

static const MemoryRegionOps fsl_imxrt1062_tcm_hole_ops = {
    .read = fsl_imxrt1062_tcm_hole_read,
    .write = fsl_imxrt1062_tcm_hole_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
};

static void fsl_imxrt1062_init_usb_host_dma(FslIMXRT1062State *s)
{
    memory_region_init(&s->usb_dma_view, OBJECT(s), "usbhost-dma-view",
                       0x100000000ULL);
    /* priority 0: the full system memory */
    memory_region_init_alias(&s->usb_dma_sysmem, OBJECT(s), "usbhost-dma-sysmem",
                             get_system_memory(), 0, 0x100000000ULL);
    memory_region_add_subregion_overlap(&s->usb_dma_view, 0,
                                        &s->usb_dma_sysmem, 0);
    /* priority 1: the two TCM windows, overlaid as unreachable holes */
    memory_region_init_io(&s->usb_dma_itcm_hole, OBJECT(s),
                          &fsl_imxrt1062_tcm_hole_ops, s, "usbhost-itcm-hole",
                          FSL_IMXRT1062_ITCM_SIZE);
    memory_region_add_subregion_overlap(&s->usb_dma_view,
                                        FSL_IMXRT1062_ITCM_BASE,
                                        &s->usb_dma_itcm_hole, 1);
    memory_region_init_io(&s->usb_dma_dtcm_hole, OBJECT(s),
                          &fsl_imxrt1062_tcm_hole_ops, s, "usbhost-dtcm-hole",
                          FSL_IMXRT1062_DTCM_SIZE);
    memory_region_add_subregion_overlap(&s->usb_dma_view,
                                        FSL_IMXRT1062_DTCM_BASE,
                                        &s->usb_dma_dtcm_hole, 1);
    address_space_init(&s->usb_dma_as, &s->usb_dma_view, "usbhost-dma");
}
```

Note this uses the header's existing `FSL_IMXRT1062_ITCM_SIZE` /
`FSL_IMXRT1062_DTCM_SIZE` (512 KiB each). The 1170 had to define its own
`FSL_IMXRT1170_TCM_SIZE` locally because its firmware reconfigures FlexRAM to a
non-default 8/8 split; the 1062 header's values already match this board.

- [ ] **Step 3: Install it on OTG2**

In the USB realize loop, extend the `if (i == 1)` block from Task 2 so it reads:

```c
        if (i == 1) {
            DEVICE(&s->usb[i])->id = g_strdup("usbhost");
            /* Model silicon: OTG2's EHCI DMA master cannot reach the M7 TCM. */
            fsl_imxrt1062_init_usb_host_dma(s);
            s->usb[i].parent_obj.ehci.as = &s->usb_dma_as;
        }
```

- [ ] **Step 4: Rebuild**

```bash
cd ~/Development/qemu2/build && ninja qemu-system-arm 2>&1 | tail -5
```

- [ ] **Step 5: The gate is its own test — it must STAY green**

```bash
cd ~/Development/rt1170/evkb/examples/usb/usb_descriptor_survey
EVKB_BOARD=rt1062 ./run_qemu.sh; echo "EXIT=$?"
```

Expected: still PASS.

**If it now goes RED, that is a genuine finding, not a bug in this task.** It
would mean the firmware really is handing TCM-resident memory to the USB
controller — which fails on silicon. Check `survey.dbg` for the
`USB DMA read from CPU-private TCM` message this task added; it names the
address. Report it rather than removing the holes.

- [ ] **Step 6: Confirm nothing else moved**

```bash
cd ~/Development/rt1170/evkb && ./tools/run-all-qemu-gates.sh usb/
```

Expected: all `usb/` gates green on both boards.

- [ ] **Step 7: Commit (qemu2 only — NEVER push)**

```
hw/arm/fsl-imxrt1062: USB DMA cannot reach the CPU-private TCMs

Ports the 1170's usbhost-dma-view: system memory aliased at priority 0, with
ITCM and DTCM overlaid at priority 1 as error-logging holes, installed as
OTG2's EHCI address space.

Silicon fidelity, not enablement -- the gate passes either way today.  It is
worth doing now because this board makes the constraint easy to trip:
imxrt1060_evkb.ld puts _estack in DTCM, so every stack buffer is TCM-resident,
and an unmodelled QEMU would accept one where the board would not.  That is the
divergence class the two-gate rule exists to prevent, and the UAC driver (which
the RT1062 has never compiled) is where it would surface.

Uses the header's existing ITCM/DTCM_SIZE.  The 1170 needed a local TCM_SIZE
because its firmware reconfigures FlexRAM to a non-default split; this board's
header values already match its linker script.
```

---

### Task 4: Close the phase

**Files:**
- Create: `examples/usb/usb_descriptor_survey/transcript_qemu_rt1062.txt`
- Modify: `tools/license-audit.sh` (GATES list)
- Modify: `CLAUDE.md`, `docs/KNOWN-BROKEN-GATES.md`
- Modify: `~/Development/USBHost_t36/README.md`

- [ ] **Step 1: Capture the green transcript**

```bash
cd ~/Development/rt1170/evkb/examples/usb/usb_descriptor_survey
EVKB_BOARD=rt1062 ./run_qemu.sh > transcript_qemu_rt1062.txt 2>&1
tail -5 transcript_qemu_rt1062.txt
```

Expected: ends with `PASS: SURVEY_ENUMERATES_EMULATED_UAC1_DEVICE`.

- [ ] **Step 2: Add the rt1062 build to the licence audit**

In `tools/license-audit.sh`, find
`examples/usb/usb_descriptor_survey:usb_descriptor_survey \` in the GATES list
and add beneath it:

```
examples/usb/usb_descriptor_survey/build-rt1062:usb_descriptor_survey \
```

- [ ] **Step 3: Run the audit, capturing FULL output**

```bash
cd ~/Development/rt1170/evkb && ./tools/license-audit.sh > /tmp/p2-audit.txt 2>&1; echo "exit=$?"
tail -2 /tmp/p2-audit.txt
grep "usb_descriptor_survey/build-rt1062" /tmp/p2-audit.txt
```

Expected: `LICENSE-AUDIT: PASS`, and a line showing the new entry was walked
with a non-zero dep-path count. **Do not `| tail` the audit itself** — a
truncated log cannot tell you what it covered.

- [ ] **Step 4: Full sweep**

```bash
uptime && cd ~/Development/rt1170/evkb && ./tools/run-all-qemu-gates.sh -j 2
```

Expected: `83 passed, 0 failed, 0 SKIP`, or `82 passed, 1 failed, 0 SKIP` with
only `rt1176:dualcore/cm4_audio_test` red.

- [ ] **Step 5: Correct the stale README in USBHost_t36**

`~/Development/USBHost_t36/README.md` currently ends its MIMXRT1060-EVKB
section with a paragraph claiming the RT1062 QEMU model is "device-mode only
(no EHCI host emulation)". Replace that paragraph with:

```markdown
Note: this has been confirmed by source review and a clean compile for
`teensy:avr:mimxrt1060evkb`, and — since 2026-08-08 — by an automated QEMU gate
that enumerates an emulated UAC1 device on the RT1062's OTG2 host controller.
(An earlier revision of this note said the i.MX RT1062 QEMU model was
device-mode only. That was true when written and is no longer: `TYPE_CHIPIDEA`
derives from `TYPE_SYS_BUS_EHCI` and host support is shared across the RT1062
and RT1176 SoC models.)
```

- [ ] **Step 6: Update `CLAUDE.md`**

Change the sweep count from 82 to 83, and add to the multi-board paragraph:

```markdown
`usb/usb_descriptor_survey` is gated on both boards and is the RT1062's USB
host proof: it enumerates QEMU's emulated `usb-audio` and reads `46F4:0002` off
the wire. Like `dualcore/cm4_usb_irq_probe`, its rt1062 half depends on a
LOCAL-ONLY qemu2 change (the `usbhost` bus id and the USB DMA TCM holes on
`fsl-imxrt1062`), so **a fresh clone sees `rt1062:usb/usb_descriptor_survey`
red**. That is the GPL firewall working as intended, not a regression.
```

- [ ] **Step 7: Update `docs/KNOWN-BROKEN-GATES.md`**

Append to the "Current expected sweep result" section:

```markdown
**2026-08-08 (Phase 2 — RT1062 USB host in QEMU):**
`usb/usb_descriptor_survey` gains its rt1062 half: sweep **82 → 83**, again
without a new example. Expectation `83/0/0` or `82/1/0` with the documented
`rt1176:dualcore/cm4_audio_test` singleton, zero SKIP.

★ **A fresh clone sees `rt1062:usb/usb_descriptor_survey` RED**, for exactly
the reason `dualcore/cm4_usb_irq_probe` is red on a fresh clone: it needs a
qemu2 change that stays local under the GPL firewall — here, naming OTG2's host
bus `usbhost` on `fsl-imxrt1062` and giving that controller a DMA view with the
TCM windows punched out.

Two notes worth not rediscovering:

- **The EHCI host was never missing from the 1062 model.** `TYPE_CHIPIDEA`
  derives from `TYPE_SYS_BUS_EHCI` and is shared with the 1170. Only the wiring
  on top was absent. USBHost_t36's README claimed otherwise and was corrected.
- **When an rt1062 image boots but produces no UART, use `-d unimp`, not
  `-d guest_errors`.** That is what localised the Phase 1 SEMC stall: guest
  errors showed 3 lines and looked benign, while `unimp` showed 1,000,000 reads
  of `semc-ctrl` offset `0x3c`.
```

- [ ] **Step 8: Commit**

```
docs: Phase 2 closed -- the RT1062 enumerates USB in QEMU; sweep 82 -> 83

usb/usb_descriptor_survey now runs on both boards and the rt1062 half asserts
46F4:0002 off the wire -- QEMU's usb-audio identity, which the firmware has no
knowledge of, so the oracle is genuinely external rather than firmware agreeing
with itself.

Needed two qemu2 changes, both LOCAL-ONLY per the GPL firewall, so a fresh
clone sees this gate red exactly as it does cm4_usb_irq_probe.  Recorded in
KNOWN-BROKEN-GATES.md rather than worked around.

USBHost_t36's README claim that the RT1062 QEMU model is "device-mode only (no
EHCI host emulation)" is corrected: it was true when written, and stopped being
true when host support landed in the shared TYPE_CHIPIDEA underneath both SoCs.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
```

---

## Diagnostic recipe (for any task where the rt1062 image boots but says nothing)

This is the Phase 1 lesson, written down because it cost real time there:

```bash
cd ~/Development/rt1170/evkb/examples/usb/usb_descriptor_survey
gtimeout 30 ~/Development/qemu2/build/qemu-system-arm \
  -M mimxrt1060-evk -global fsl-imxrt1062.boot-ivt=on \
  -kernel build-rt1062/usb_descriptor_survey.elf \
  -display none -serial file:/tmp/dbg.uart -d unimp,guest_errors -D /tmp/dbg.log \
  >/dev/null 2>&1
sed 's/0x[0-9a-f]*$//' /tmp/dbg.log | sort | uniq -c | sort -rn | head
```

A peripheral appearing hundreds of thousands of times is a stub the firmware is
polling. `-d guest_errors` alone will NOT show this.

Note `boot-ivt`, not `boot-xip`: the RT1062 model has both, and a Teensy-core
image is the IVT kind. `gate-lib.sh` already gets this right; the hand-run
command above must too.

## Definition of done

- [ ] `transcript_qemu_red.txt` committed before the fix, red for the stated cause
- [ ] `rt1062:usb/usb_descriptor_survey` PASSES, asserting `46F4:0002`
- [ ] All `rt1176:` gates unchanged
- [ ] Sweep `(83 gate(s))`, zero SKIP
- [ ] `license-audit.sh` PASS with `build-rt1062` walked
- [ ] USBHost_t36 README corrected
- [ ] qemu2 committed locally, **never pushed**

## Not in this phase

No silicon run (Phase 3, on **J47** — J48 is the device port). No audio driver
or `usb_audio_*` rt1062 build (Phase 4). Task #74 (gate `string_test` for
rt1062) remains separate.
