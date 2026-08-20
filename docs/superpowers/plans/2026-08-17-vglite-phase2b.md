# VGLite Phase 2b Implementation Plan — re-vendor, then the LVGL backend

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the vendored VGLite driver with MCUXpresso SDK v26.06.00-LTS's, re-prove Phase 1 on silicon, then enable LVGL's VG_LITE draw unit and measure the 16-knob grid against ≥30 fps.

**Architecture:** The SDK driver defines every name LVGL 9.4 references, so the Phase 2a compat shim goes to zero entries (its extractor gate stays, and is what proves it). Our `port/baremetal/` keeps its structure: 4 new HAL functions, 2 changed signatures, and the single-thread `vg_lite_os_*` scaffolding demoted to static helpers. Phase 1's ISR-counter and bounded-wait logic relocates under `vg_lite_hal_wait_interrupt` rather than being rewritten.

**Tech Stack:** VGLite from `~/Development/mcuxsdk-ws/mcuxsdk/middleware/vglite/driver`, LVGL 9.4 `src/draw/vg_lite/`, ARM GCC 10, CMake, QEMU `mimxrt1170-evk`, MIMXRT1170-EVKB + RK055HDMIPI4MA0.

**Spec:** `docs/superpowers/specs/2026-08-17-vglite-phase2-design.md`, especially **§8** (the measured re-vendor scope) and **§5** (why formats are not inert — still true, but §8 removes the shimmed ones).

**Branch:** continue on `vglite-p2` (worktree `.claude/worktrees/vglite-p2`). Phase 2a's shim work is already committed there and most of it survives.

---

## ★ Sequencing rule, and it is the whole shape of this plan

**Task 4 re-proves Phase 1 on silicon BEFORE any LVGL work.** If the new driver does not render the blue square, every measurement after it is worthless, and the cheapest moment to discover that is before the backend is in the picture. Phase 1 took three silicon-only defects — an alignment hang, a display-mix reset, and a colour-order transposition — and **all three passed QEMU**. Do not reorder Tasks 4 and 5.

---

### Task 1: Vendor the SDK driver

**Files:**
- Replace: `$TEENSY_LIB_ROOT/VGLite/{inc,VGLite,VGLiteKernel}/`
- Modify: `$TEENSY_LIB_ROOT/VGLite/VENDORING.md`

- [ ] **Step 1: Replace, do not merge** — `VENDORING.md`'s own rule; copying over
      leaves upstream-deleted files behind and silently drifts the tree.

```bash
SDK=~/Development/mcuxsdk-ws/mcuxsdk/middleware/vglite/driver
V=$TEENSY_LIB_ROOT/VGLite
rm -rf $V/inc $V/VGLite $V/VGLiteKernel
mkdir -p $V/inc $V/VGLite $V/VGLiteKernel
cp $SDK/inc/*.h                 $V/inc/
cp $SDK/VGLite/*.c $SDK/VGLite/*.h        $V/VGLite/
cp $SDK/VGLiteKernel/*.c $SDK/VGLiteKernel/*.h $V/VGLiteKernel/
rm -f $V/VGLite/vg_lite_dump.c $V/VGLite/dumpAPI.c $V/VGLite/vg_lite_debug.h
```

★ Dropping `vg_lite_dump.c`/`dumpAPI.c` is what removes the
`vg_lite_os_fopen`/`fclose`/`fread`/`fwrite`/`fseek`/`ftell`/`fflush`/`fprintf`
contract entirely — eight functions we would otherwise have to stub. Confirm
nothing else references them:

```bash
grep -ran "vg_lite_os_f" $V/VGLite $V/VGLiteKernel $V/inc | grep -v Binary
```
Expected: no output. If a `.c` you kept calls them, either drop that file too or
stub them — do not vendor the dump files back.

- [ ] **Step 2: ★ Transcode the ISO-8859-1 file** — the audit hole from §8:

```bash
file -b --mime-encoding $V/VGLite/vg_lite_stroke.c    # -> iso-8859-1
iconv -f ISO-8859-1 -t UTF-8 $V/VGLite/vg_lite_stroke.c > /tmp/s.c && mv /tmp/s.c $V/VGLite/vg_lite_stroke.c
file -b --mime-encoding $V/VGLite/vg_lite_stroke.c    # -> us-ascii or utf-8
```

Then prove the audit can now see it, which is the point:

```bash
grep -Il "Permission is hereby granted" $V/VGLite/vg_lite_stroke.c
```
Expected: the path prints. Before the transcode it printed nothing while
`grep -a` found the text — that is a source file the copyleft sweep never reads.

- [ ] **Step 3: Re-run the whole-repo licence survey** (`VENDORING.md`'s step 5,
      added after the Apache-2.0 miss):

```bash
cd $V
grep -rIl -E "GNU General Public|GNU Lesser|Mozilla Public" . ; echo "^ empty = no copyleft"
for f in $(git ls-files '*.c' '*.h'); do
  printf '%-40s %s\n' "$f" "$(grep -m1 -o 'MIT License\|Permission is hereby granted\|Licensed under the Apache License' "$f")"
done | grep -v "MIT License\|Permission is hereby" || echo "^ every file carries permissive text"
for f in $(git ls-files '*.c' '*.h'); do
  iconv -f UTF-8 -t UTF-8 "$f" >/dev/null 2>&1 || echo "NON-UTF8 (audit would skip): $f"
done
```
Expected: no copyleft, no Apache (the `vg_lite_flat.{c,h}` pair is gone in this
release), no non-UTF-8 files left. **Record the actual output** — if an Apache
file did survive, `NOTICE` must keep its exception rather than being simplified.

- [ ] **Step 4: Update `VENDORING.md` and `NOTICE`** — new SDK tag, commit and
      version; state that the Apache exception is gone if Step 3 says so; add
      the non-UTF-8 check as a numbered re-vendoring step so this is caught by
      procedure next time, not by luck.

- [ ] **Step 5: Commit (do not push yet — the port does not build).**

### Task 2: Adapt the port

**Files:**
- Modify: `$TEENSY_LIB_ROOT/VGLite/port/baremetal/vg_lite_hal.c`
- Modify: `$TEENSY_LIB_ROOT/VGLite/port/baremetal/vg_lite_os.c`, `vg_lite_os.h`

- [ ] **Step 1: The two changed signatures.** NXP's own port is the reference
      (`$SDK/VGLiteKernel/rtos/vg_lite_hal.c`) and it ignores the new `map`
      parameters outright:

```c
void * vg_lite_hal_map(uint32_t flags, uint32_t bytes, void *logical,
                       uint32_t physical, int32_t dma_buf_fd, uint32_t *gpu)
{
    (void)flags; (void)bytes; (void)dma_buf_fd;   /* NXP's port does the same */
    /* ...existing body, which took (size, logical, physical, gpu)... */
}
```

```c
vg_lite_error_t vg_lite_hal_allocate_contiguous(unsigned long size,
                                                vg_lite_vidmem_pool_t pool,
                                                void **logical, void **klogical,
                                                uint32_t *physical, void **node)
{
    /* One flat pool here, so clamp like NXP does rather than indexing blind. */
    if (pool >= VG_SYSTEM_RESERVE_COUNT) pool = (vg_lite_vidmem_pool_t)(VG_SYSTEM_RESERVE_COUNT - 1);
    (void)pool;
    /* ...existing allocator... */
    if (klogical) *klogical = *logical;   /* single address space: kernel == user */
}
```

★ **Keep the 64-byte alignment.** Phase 1's root cause was a 12-byte allocator
node header pushing every payload off a 64-byte boundary, which HUNG the Vivante
front end while every API call returned `VG_LITE_SUCCESS`. The `_Static_assert`
pinning `sizeof(vgl_node_t) == VGL_ALIGN` must survive this edit. If the new
driver brings its own allocator, verify alignment the same way before trusting it.

- [ ] **Step 2: The four new HAL functions.** All trivial in a single flat
      address space with the D-cache off (`imxrt1176` never writes `SCB_CCR`):

```c
vg_lite_error_t vg_lite_hal_map_memory(vg_lite_kernel_map_memory_t *node)
{   /* Already CPU-visible: hand back the logical address. */
    node->logical = (void *)(uintptr_t)node->physical;
    return VG_LITE_SUCCESS;
}
vg_lite_error_t vg_lite_hal_unmap_memory(vg_lite_kernel_unmap_memory_t *node)
{   (void)node; return VG_LITE_SUCCESS; }

vg_lite_error_t vg_lite_hal_operation_cache(void *handle, vg_lite_cache_op_t cache_op)
{   /* No maintenance needed: this core leaves the D-cache off, so CPU and GPU
     * views agree. Do NOT copy rt1062's cache handling here -- see CLAUDE.md. */
    (void)handle; (void)cache_op; return VG_LITE_SUCCESS; }

vg_lite_error_t vg_lite_hal_memory_export(int32_t *fd)
{   (void)fd; return VG_LITE_NOT_SUPPORT; }   /* no dma-buf on bare metal */
```
Check each prototype against `$V/VGLiteKernel/vg_lite_hal.h` after vendoring
rather than trusting the snippets above — they are from the pre-vendor headers.

- [ ] **Step 3: Demote the orphaned `vg_lite_os_*` scaffolding.** The new driver
      calls only `vg_lite_os_malloc`/`free` (48/46 uses). Keep those exported.
      `vg_lite_os_initialize`/`deinitialize`/`sleep`/`wait_interrupt`/
      `irq_count`/`wait_timeouts` lose their external callers — make them
      `static` helpers of the HAL, **keeping their behaviour**:

★ `vg_lite_os_irq_count()` and the bounded wait are not scaffolding, they are
Phase 1's hard-won completion logic. A wait that "succeeds" without the ISR
count advancing is not a success — early Phase 1 runs reported `TIMEOUTS=0` and
`DRAW=OK` with nothing rendered, because the wait consumed interrupt flags left
over from init. Whatever `vg_lite_hal_wait_interrupt` becomes, it must still
count real ISRs and still bound its wait. Keep both observable in the probe's
output.

- [ ] **Step 4: Build the driver alone** (no LVGL yet):

```bash
cd $WT/examples/display/vglite_probe && rm -rf build && \
  cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake && \
  cmake --build build -j 8
```
Fix only port-layer errors. Record every signature that turned out to differ
from §8's four — §8 was measured from headers, and the compile is the oracle.

- [ ] **Step 5: Commit.**

### Task 3: QEMU — the fallback still works

- [ ] **Step 1:** `cd $WT/examples/display/vglite_probe && ./run_qemu.sh`
- [ ] **Step 2:** Must still report `VGLITE_INIT=ABSENT`, `TIMEOUTS=0`,
      `VGLITE_PROBE_DONE`. A new driver that spins on absent hardware instead of
      failing cleanly breaks the one-binary story and must be fixed here, not
      papered over — `vg_lite_init()` spinning was the original reason the
      chip-ID probe exists.
- [ ] **Step 3:** Commit only if green.

### Task 4: ★ SILICON — re-prove Phase 1 before touching LVGL

**This is the gate for the whole plan.** Nothing downstream is trustworthy
until the new driver renders the blue square.

- [ ] **Step 1: Flash, VCOM-free.**

```bash
pkill LinkServer; pkill redlinkserv; pkill crt_emu_cm_redlink; sleep 1
lsof -t -- /dev/cu.usbmodem5DQ2DDHVWO5EI3 && echo "PORT HELD - ABORT" && exit 1
ELF=$WT/examples/display/vglite_probe/build/vglite_probe.elf
/Applications/LinkServer_26.6.137/LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load "$ELF" --erase-all
/Applications/LinkServer_26.6.137/LinkServer flash MIMXRT1176:MIMXRT1170-EVKB verify "$ELF"
```
★ Never hold the VCOM during `flash … load` — it panics this Mac (the port
re-enumerates with the reader attached). Order: load → verify → attach reader →
reset. If SWD fails outright, **check the board's power switch before blaming
the image** — a long Phase 1 debug session was exactly that.

- [ ] **Step 2: Capture and compare against Phase 1's recorded result.**

Required, all of them:
- `VGLITE_CHIP_ID=0x00000355`
- `VGLITE_INIT=OK`
- `VGLITE_DRAW=OK`, `VGLITE_FINISH=OK`
- `VGLITE_TIMEOUTS=0` **and** `VGLITE_IRQS` advancing (neither alone is proof)
- `VGLITE_SUM=0x45465405`

★ **If the checksum differs but the square still looks right, STOP and
investigate rather than re-recording the golden.** A changed sum means the new
driver rasterises differently, which is a real finding that belongs in the
transcript — and it also means the software/GPU golden split in Task 8 needs
re-thinking. Re-recording a golden to make a gate green is exactly what this
tree forbids.

- [ ] **Step 3: Eyes on glass.** Blue square on the RK055, confirmed by a human.
      Counters and checksums both passed in Phase 1 while the panel was black
      (the display-mix reset bug). Record who confirmed and when.

- [ ] **Step 4:** Append to `transcript_hw_evkb.txt` — do not overwrite Phase 1's
      account; this is a second dated entry under the new driver.

- [ ] **Step 5: Commit, push VGLite, bump the pin in `$WT/evkb.cmake`.**
      Only now is the new driver trustworthy enough to build on.

### Task 5: Retire the compat shim

- [ ] **Step 1:**

```bash
cd $WT && ./tools/vglite-lvgl-names.py
```
Expected: **no output** — the SDK driver defines all 47 names. If any remain,
they are the real residue and go in the shim; do not assume zero.

- [ ] **Step 2:** Delete `port/baremetal/vg_lite_lvgl_compat.h` if Step 1 is
      empty, and drop the `-include` from `import_evkb_lvgl()`. **Keep
      `tools/vglite-lvgl-names.py` and its `--check`** — with an empty shim it
      becomes the guard that catches the next LVGL bump reintroducing a gap.
      Point `--check` at `/dev/null` so it still fails on drift.

- [ ] **Step 3:** Keep the configure-time image-decoder guard? **No** — §5's
      hazard was that *shimmed* formats reach the GPU. With a driver that has
      `VG_LITE_BGR888`/`BGRA5658` for real, the guard's premise is gone. Delete
      it and say why in the commit, rather than leaving a check whose reason no
      longer holds.

- [ ] **Step 4: Commit.**

### Task 6: Compile the LVGL backend

- [ ] **Step 1:** Rebuild the Task 5 scratch example (or the Task 7 one) with
      `import_evkb_lvgl(VGLITE)`.
- [ ] **Step 2: ★ Verify the backend is actually IN the binary**, which a
      successful build does not tell you:

```bash
for o in $(find build -name "lv_draw_vg_lite*.obj" -o -name "lv_vg_lite*.obj"); do
  printf '%-34s %s syms\n' "$(basename $o)" \
    "$(/Applications/ARM_10/bin/arm-none-eabi-nm --defined-only $o | wc -l)"
done
/Applications/ARM_10/bin/arm-none-eabi-nm build/*.elf | grep -c vg_lite
```
Expected: non-zero symbol counts and a non-zero ELF count. **Phase 2a's first
"BUILD OK" had 19 objects with ZERO symbols each** — `LV_USE_DRAW_VG_LITE`
never took effect because `IN_LIST ARGN` does not work inside a CMake macro, and
the build succeeded anyway. Counting symbols is the check that caught it.
- [ ] **Step 3:** Fix fallout. Editing LVGL source is still out of scope; a fix
      that needs it means something else is wrong.
- [ ] **Step 4: Commit.**

### Task 7: QEMU gate for the LVGL scene

**Files:** Create `$WT/examples/display/vglite_lvgl_test/`

- [ ] **Step 1:** A 4×4 `synthui_knob` grid — the same scene the fps target
      names, so Task 9 measures the thing the spec asked about.
- [ ] **Step 2:** Gate the ABSENT path: chip-ID probe answered, `INIT=ABSENT`,
      scene renders in software, `TIMEOUTS=0`, DONE token. Artifacts through
      `gate_capture_path`.
- [ ] **Step 3:** Record the **software** golden, labelled as such in the script.
- [ ] **Step 4:** Prove it fails on a sentinel and a one-nibble mutation.
- [ ] **Step 5:** Commit with `transcript_qemu.txt`.

### Task 8: Silicon — LVGL on the GPU

- [ ] **Step 1:** Flash (Task 4's rules) and capture.
- [ ] **Step 2:** `VGLITE_INIT=OK`, `TIMEOUTS=0`, IRQs advancing, GPU golden.
- [ ] **Step 3: Eyes on glass** — the knobs must look *right*, not merely
      present. The knob's own history is the warning: LVGL's software arc
      clamps negative angles, which rendered a full ring instead of a crescent
      with every checksum perfectly reproducible. Compare against the software
      render side by side.
- [ ] **Step 4:** Record BOTH goldens with a comment on why they differ
      (hardware AA vs mask arithmetic). Never reconcile them.
- [ ] **Step 5:** `transcript_hw_evkb.txt`, commit.

### Task 9: The number

- [ ] **Step 1:** `#ifdef FPSBENCH` variant timing `lv_refr_now()` with
      `micros()`, in a separate `build-fpsbench/` so the golden ELF is untouched.
- [ ] **Step 2:** Measure the 16-knob grid on the GPU path, ≥3 samples. Measure
      the **software** baseline the same way on the same scene — do not quote
      ~208 ms from the spec, it was derived from a 360 px single knob.
- [ ] **Step 3:** Record mean/worst and the speedup. **Report it whatever it
      says.** ≥30 fps meets §1; below that, Phase 2 has failed its own criterion
      and saying so plainly is the deliverable. Note also whether the GPU path
      is CPU-bound on path *construction* rather than rasterisation — that
      changes what the next optimisation would be.

### Task 10: Audit, docs, sweep, wrap

- [ ] **Step 1:** `tools/license-audit.sh` GATES entry for the new example;
      mutation-test that dropping it is detected.
- [ ] **Step 2: ★ Teach the audit about non-UTF-8 source**, or record why not.
      Task 1 Step 2 fixes *this* file; the hole stays open for the next one.
      A `file --mime-encoding` sweep over git-tracked sources, failing on
      anything `grep -I` would skip, closes it generally.
- [ ] **Step 3:** `docs/KNOWN-BROKEN-GATES.md` — two-golden-sets rule, what the
      QEMU gate does and does not prove.
- [ ] **Step 4:** `examples/README.md` line.
- [ ] **Step 5:** Full sweep; update CLAUDE.md's baseline with measured numbers
      (94 → 95 with one gate added). Read gate NAMES, not just counts.
- [ ] **Step 6:** Update the spec status with the measured fps and whether §1's
      criterion was met.
- [ ] **Step 7:** Commit, then **superpowers:finishing-a-development-branch**.
