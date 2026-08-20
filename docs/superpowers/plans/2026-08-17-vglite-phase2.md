# VGLite Phase 2 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Enable `LV_USE_DRAW_VG_LITE` on the GC355 via a compatibility shim, and measure the 16-knob grid against the spec's ≥30 fps criterion.

**Architecture:** A generated-then-checked-in header supplies the ~47 names LVGL 9.4 references that this driver version lacks, valued above `gcFEATURE_COUNT` so `vg_lite_query_feature()` returns 0 for them. It is force-included for LVGL's translation units only. No vendored file and no LVGL source is edited.

**Tech Stack:** LVGL 9.4 `src/draw/vg_lite/`, NXP/Vivante VGLite (`VGLITE_HEADER_VERSION 6`), ARM GCC 10, CMake, QEMU `mimxrt1170-evk`, MIMXRT1170-EVKB + RK055HDMIPI4MA0.

**Spec:** `docs/superpowers/specs/2026-08-17-vglite-phase2-design.md`. Read §5 before Task 3 — two shimmed names are live format selections, not inert feature bits.

**Worktree:** `.claude/worktrees/vglite-p2`, branch `vglite-p2`, cut from master `566ee0f`. `$WT` below means that path.

---

### Task 1: Lock the missing-name set as a reproducible artifact

The set was computed by hand-run python during design. It must become a script,
because an LVGL bump silently changes it and a stale shim fails as a compile
error at best and a wrong `case` label at worst.

**Files:**
- Create: `$WT/tools/vglite-lvgl-names.py`

- [ ] **Step 1: Write the extractor**

It prints, deterministically sorted, the names LVGL's `src/draw/vg_lite/`
references minus the names the VGLite headers define. Two families: feature
bits (`gcFEATURE_BIT_VG_*`) and everything stringified via
`VG_LITE_ENUM_TO_STRING` (errors, formats, VLC ops).

```python
#!/usr/bin/env python3
"""Names LVGL's VG_LITE backend references that the vendored driver lacks.

Run from anywhere:  tools/vglite-lvgl-names.py [--check <compat header>]
Exit 0 = the header covers every missing name; 1 = drift.

★ The `##e` in FEATURE_ENUM_TO_STRING/VG_LITE_ENUM_TO_STRING is matched by a
naive regex as the literal name `..._e`. That is an artifact of scanning text
before token pasting, not a real symbol -- discard it, or the shim grows a
bogus `gcFEATURE_BIT_VG_e`.
"""
import glob, os, re, sys

LIB = os.environ.get("TEENSY_LIB_ROOT", os.path.expanduser("~/Development"))
BACKEND = os.path.join(LIB, "LVGL", "lvgl", "src", "draw", "vg_lite")
HEADERS = [os.path.join(LIB, "VGLite", "inc", "vg_lite.h")]

def backend_sources():
    return sorted(glob.glob(os.path.join(BACKEND, "*.c")) +
                  glob.glob(os.path.join(BACKEND, "*.h")))

def missing():
    need_feat, need_enum = set(), set()
    for f in backend_sources():
        s = open(f).read()
        need_feat |= set(re.findall(r"\bgcFEATURE_BIT_VG_\w+", s))
        need_feat |= set("gcFEATURE_BIT_VG_" + m
                         for m in re.findall(r"FEATURE_ENUM_TO_STRING\((\w+)\)", s))
        need_enum |= set("VG_LITE_" + m
                         for m in re.findall(r"VG_LITE_ENUM_TO_STRING\((\w+)\)", s))
    need_feat.discard("gcFEATURE_BIT_VG_e")      # the ##e artifact
    need_enum.discard("VG_LITE_e")
    have = set()
    for h in HEADERS:
        s = open(h).read()
        have |= set(re.findall(r"\bgcFEATURE_BIT_VG_\w+", s))
        have |= set(re.findall(r"\bVG_LITE_\w+", s))
    return sorted(need_feat - have), sorted(need_enum - have)

def main():
    feat, enum = missing()
    if "--check" in sys.argv:
        hdr = open(sys.argv[sys.argv.index("--check") + 1]).read()
        gaps = [n for n in feat + enum if not re.search(r"\b%s\b" % re.escape(n), hdr)]
        if gaps:
            print("VGLITE-COMPAT DRIFT: %d name(s) referenced by LVGL, absent from "
                  "both the driver and the compat header:" % len(gaps))
            for n in gaps:
                print("   ", n)
            print("Add them to the compat header. Read the spec's section 5 FIRST "
                  "if any is a pixel format -- formats are not inert.")
            return 1
        print("VGLITE-COMPAT: OK (%d feature, %d enum names shimmed)" % (len(feat), len(enum)))
        return 0
    for n in feat:
        print(n)
    for n in enum:
        print(n)
    return 0

sys.exit(main())
```

- [ ] **Step 2: Run it and record the numbers**

```bash
chmod +x $WT/tools/vglite-lvgl-names.py
$WT/tools/vglite-lvgl-names.py | wc -l
$WT/tools/vglite-lvgl-names.py | grep -c gcFEATURE_BIT
```

Expected at the pinned LVGL/VGLite: **47 total, 38 feature bits, 9 enum names**.
A different number is not a failure — it means a pin moved. Record what it
actually prints and carry that number into Task 2.

- [ ] **Step 3: Commit**

```bash
git -C $WT add tools/vglite-lvgl-names.py
git -C $WT commit -m "tools: extract the LVGL-vs-VGLite name gap reproducibly

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

### Task 2: The compat header — feature bits only

Feature bits first, deliberately: they are inert (worst case LVGL takes a
slower path). The nine enum names include live format selections and are
Task 3, under their own rules.

**Files:**
- Create: `$TEENSY_LIB_ROOT/VGLite/port/baremetal/vg_lite_lvgl_compat.h`

- [ ] **Step 1: Write the header**

Every feature bit gets a value at or above `gcFEATURE_COUNT`, so
`vg_lite_query_feature()` takes its documented `else result = 0` branch. Values
must be mutually distinct or the `switch` in `lv_vg_lite_feature_string()`
fails to compile on duplicate case labels — which is a real safety net, keep it.

```c
/* LVGL 9.4 <-> VGLite (VGLITE_HEADER_VERSION 6) compatibility shim.
 *
 * LVGL's VG_LITE backend targets a NEWER NXP driver than the one vendored
 * here. It references 47 gcFEATURE_BIT_VG_* names; this driver defines 9.
 * lv_vg_lite_feature_string() is compiled unconditionally, so every name must
 * EXIST even though most are only ever used for logging.
 *
 * C enums cannot be reopened, so these are macros rather than enumerators.
 * vg_lite_query_feature((vg_lite_feature_t)N) compiles, and
 * `case gcFEATURE_BIT_VG_X:` remains a valid constant case label.
 *
 * ★ Values start at gcFEATURE_COUNT because the vendor bounds-checks:
 *      if (feature < gcFEATURE_COUNT) result = ctx->s_ftable.ftable[feature];
 *      else                           result = 0;
 *   so every name below reads 0 with no out-of-bounds table access. That is a
 *   documented path, not an exploited accident.
 *
 * What 0 MEANS: "this GC355 does not have that capability", asserted by
 * construction rather than by asking the hardware. If a bit here is in fact
 * supported, we lose the optimisation, never correctness -- LVGL's every use
 * of these is `if (supported) fast_path else portable_path`. Re-vendoring a
 * newer driver is how you find out; see the Phase 2 spec section 4.
 *
 * Generated set verified by tools/vglite-lvgl-names.py --check (a gate).
 */
#ifndef VG_LITE_LVGL_COMPAT_H
#define VG_LITE_LVGL_COMPAT_H

#include "vg_lite.h"

#define VGL_COMPAT_FEATURE(n) ((vg_lite_feature_t)(gcFEATURE_COUNT + (n)))

#define gcFEATURE_BIT_VG_16PIXELS_ALIGN          VGL_COMPAT_FEATURE(0)
#define gcFEATURE_BIT_VG_24BIT                   VGL_COMPAT_FEATURE(1)
/* ... one line per name printed by tools/vglite-lvgl-names.py, in its sorted
 *     order, with consecutive indices. Write them ALL; do not abbreviate. */

#endif /* VG_LITE_LVGL_COMPAT_H */
```

- [ ] **Step 2: Verify the values cannot collide with real ones**

```bash
cat > /tmp/compat_probe.c <<'EOF'
#include "vg_lite_lvgl_compat.h"
#include <stdio.h>
int main(void) {
    printf("gcFEATURE_COUNT=%d SCISSOR=%d LVGL_SUPPORT=%d\n",
           (int)gcFEATURE_COUNT, (int)gcFEATURE_BIT_VG_SCISSOR,
           (int)gcFEATURE_BIT_VG_LVGL_SUPPORT);
    return 0;
}
EOF
```
Compile host-side with the VGLite include dirs. Expected: `gcFEATURE_COUNT=9`
and every shimmed value ≥ 9. **A shimmed value below the count would silently
alias a REAL feature** — e.g. reading DITHER's table entry when asked for
SCISSOR — so assert this rather than eyeballing it.

- [ ] **Step 3: Commit in the VGLite repo and bump the pin**

```bash
git -C $TEENSY_LIB_ROOT/VGLite add port/baremetal/vg_lite_lvgl_compat.h
git -C $TEENSY_LIB_ROOT/VGLite commit -m "port: LVGL 9.4 feature-bit compatibility shim

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
git -C $TEENSY_LIB_ROOT/VGLite push origin master
```
Then paste the new SHA into `$WT/evkb.cmake`'s VGLite pin and commit.

### Task 3: The nine enum names — formats are NOT inert

**Read the spec's §5 before this task.** Seven are stringify-only and safe.
`VG_LITE_BGR888` and `VG_LITE_BGRA5658` are **returned as real format
selections** (`lv_vg_lite_utils.c:582,585`) for `LV_COLOR_FORMAT_ARGB8565` and
`RGB888`, and this driver **hangs rather than erroring** on input it does not
understand (Phase 1's alignment bug).

**Files:**
- Modify: `$TEENSY_LIB_ROOT/VGLite/port/baremetal/vg_lite_lvgl_compat.h`

- [ ] **Step 1: Add the seven safe names**

`VG_LITE_ABGR8565`, `VG_LITE_ARGB8565`, `VG_LITE_RGB888`, `VG_LITE_RGBA5658`,
`VG_LITE_RGBA8888_ETC2_EAC`, `VG_LITE_FLEXA_TIME_OUT`,
`VG_LITE_FLEXA_HANDSHAKE_FAIL` — values distinct from every real
`vg_lite_buffer_format_t` / `vg_lite_error_t`. ETC2_EAC appears only in
equality tests, so a never-produced value makes them permanently false, which
is correct.

- [ ] **Step 2: Add the two live formats WITH a guard**

Primary guard is build-time. This port cannot currently produce either colour
format — `LV_USE_LODEPNG`, `LV_USE_BMP`, `LV_USE_TJPGD` and
`LV_BIN_DECODER_RAM_LOAD` are all 0 in `LVGL/port/lv_conf.h`, so no decoder
emits RGB888 or ARGB8565. Encode that as an assertion so enabling a decoder
later trips it:

```c
/* ★ These two are NOT inert. LVGL returns them as the format a buffer will be
 * drawn with, so a shimmed value would reach the GPU as a real format -- and
 * this driver HANGS on input it cannot parse rather than reporting an error
 * (Phase 1: a misaligned command buffer hung the front end while every API
 * call returned VG_LITE_SUCCESS). Silence, not a wrong colour, is the failure.
 *
 * Safe today only because nothing produces those LVGL colour formats: every
 * image decoder is off in port/lv_conf.h. If you enable one, this shim is no
 * longer safe and the answer is to re-vendor a driver that has the formats --
 * NOT to map them onto something that "looks close". */
#if defined(LV_USE_LODEPNG) && LV_USE_LODEPNG
#error "vg_lite_lvgl_compat.h: an image decoder is enabled; see the Phase 2 spec 5"
#endif
```
Repeat for BMP / TJPGD / `LV_BIN_DECODER_RAM_LOAD`.

- [ ] **Step 3: Prove the guard fires** — mutation test, the house rule:

```bash
# temporarily build one LVGL example with -DLV_USE_LODEPNG=1
```
Expected: the `#error` fires by name. A guard never seen to fire is decoration.
Revert after.

- [ ] **Step 4: Commit + bump the pin** (same shape as Task 2 Step 3).

### Task 4: Wire the build

**Files:**
- Modify: `$WT/evkb.cmake` (`import_evkb_lvgl`)

- [ ] **Step 1: Force-include the shim for LVGL only**

The shim must be visible to LVGL's translation units without editing LVGL
source. Inside `import_evkb_lvgl()`, when the VGLite target is present:

```cmake
# The shim is force-included rather than #include'd, because LVGL's backend
# does `#include <vg_lite.h>` and we do not edit LVGL source. LVGL-only: the
# VGLite target itself must compile against the UNSHIMMED headers, or the
# driver would see names its own switch statements do not handle.
target_compile_options(LVGL PRIVATE
    -include "${_evkb_vglite_dir}/port/baremetal/vg_lite_lvgl_compat.h")
target_compile_definitions(LVGL PUBLIC LV_USE_DRAW_VG_LITE=1)
target_link_libraries(LVGL PUBLIC VGLite)
```

★ `PRIVATE` on the force-include is load-bearing — see the comment. Verify the
VGLite target's own compile line does NOT carry `-include`.

- [ ] **Step 2: Keep it opt-in.** `LV_USE_DRAW_VG_LITE` must not turn on for
every LVGL example: the existing display gates have recorded goldens and the
GPU path will not reproduce them. Gate it behind an example-level opt-in
(e.g. `import_evkb_lvgl(VGLITE)`), leaving all current examples on software.
Confirm by rebuilding `lvgl_smoke_test` and re-running its gate unchanged.

### Task 5: First compile of the backend

- [ ] **Step 1:** Build a scratch example with the backend enabled. Expect
fallout beyond names — struct fields and function signatures also moved
between driver versions.
- [ ] **Step 2:** Fix compile errors ONLY in the shim or the build wiring.
**If a fix requires editing LVGL or a vendored VGLite file, STOP** — that is
the signal that §4's shim approach has hit its limit and re-vendoring is the
answer. Report rather than pushing through.
- [ ] **Step 3:** Record every fix in the shim's header comment.

### Task 6: QEMU gate — the fallback still works

**Files:**
- Create: `$WT/examples/display/vglite_lvgl_test/` (CMakeLists, sketch, `run_qemu.sh`)

- [ ] **Step 1:** A scene of `synthui_knob` widgets, the same 4×4 grid the fps
target names, with `LV_USE_DRAW_VG_LITE 1`.
- [ ] **Step 2:** The gate asserts the ABSENT path: chip ID probe answered,
`VGLITE_INIT=ABSENT`, the scene still renders in software, `TIMEOUTS=0`, and a
DONE token. Route artifacts through `gate_capture_path` (gate-lib rule).
- [ ] **Step 3:** Record the software golden. ★ This is the SOFTWARE golden —
label it as such in the script, because Task 7 adds a different one.
- [ ] **Step 4:** Prove it fails on a sentinel and on a one-nibble mutation.
- [ ] **Step 5:** Commit with `transcript_qemu.txt`.

### Task 7: Silicon — does LVGL render on the GPU?

- [ ] **Step 1:** Flash VCOM-free (`pkill LinkServer; pkill redlinkserv; pkill
crt_emu_cm_redlink` first; never hold the port during `flash ... load` — it
panics this Mac, see the memory note).
- [ ] **Step 2:** Capture. Required: `VGLITE_INIT=OK`, chip ID `0x355`,
`TIMEOUTS=0`, IRQ count advancing, and a GPU-path golden.
- [ ] **Step 3:** **Eyes on glass.** Confirm the knobs render correctly. A
checksum cannot tell "the GPU drew it" from "the GPU drew it wrong but
reproducibly" — Phase 1 and the knob's arc bug both proved that.
- [ ] **Step 4:** Record BOTH goldens with a comment saying why they differ
(hardware AA vs LVGL masks). Never reconcile them.
- [ ] **Step 5:** Write `transcript_hw_evkb.txt` and commit.

### Task 8: The number

- [ ] **Step 1:** `#ifdef FPSBENCH` variant timing `lv_refr_now()` with
`micros()`, built into a SEPARATE `build-fpsbench/` so the golden-producing ELF
is untouched (exactly Phase 1's method).
- [ ] **Step 2:** Measure the 16-knob grid, GPU path, ≥3 samples. Compare
against the software baseline (~208 ms ⇒ ~5 fps) measured the same way on the
same scene — measure it, do not quote it from the spec.
- [ ] **Step 3:** Record mean/worst and the speedup in the transcript.
**Report the number whatever it is.** A Phase 2 that renders but is not faster
has failed its own success criterion, and saying so is the deliverable.

### Task 9: Audit, docs, sweep, wrap

- [ ] **Step 1:** `tools/license-audit.sh` GATES entry for the new example.
Mutation-test that dropping it is detected.
- [ ] **Step 2:** Wire `tools/vglite-lvgl-names.py --check` into the audit or
`run-all-qemu-gates.sh` so LVGL/VGLite pin drift fails loudly.
- [ ] **Step 3:** `docs/KNOWN-BROKEN-GATES.md` — the two-golden-sets rule and
what the QEMU gate does and does not prove.
- [ ] **Step 4:** `examples/README.md` line.
- [ ] **Step 5:** Full sweep; update CLAUDE.md's baseline with the measured
numbers (94 → 95 if one gate was added). Read the gate NAMES, not just counts.
- [ ] **Step 6:** Update the spec's status line with the measured fps.
- [ ] **Step 7:** Commit, then **superpowers:finishing-a-development-branch**.
