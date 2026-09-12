# acid_box ITCM Headroom Implementation Plan (NEW-45, folding in NEW-37)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Restore the four broken `display/acid_box` `M2_BT_OUT` bench builds by giving them ~10–14 KB of ITCM headroom instead of the 272 B they had, and add the guardrails that make the next occurrence visible at close-out rather than months later.

**Architecture:** Route whole archives that are cold or coarse-grained *by construction* out of ITCM in acid_box's derived `M2_BT_OUT` linker script — `libVGLite` first (Phase 0, unbreaks everything), then `libMipiDisplay`/`libWire`/`libTouchPanel` and `Sbc.cpp` once a silicon session says they cost nothing. Add a link-time headroom print and assert, a per-example `bench` sidecar declaring each bench configuration, and a tool that builds those configurations into directories it owns.

**Tech Stack:** CMake 3.24 + ARM GCC 10 (`/Applications/ARM_10/bin`), GNU ld 2.35 linker scripts, POSIX `sh` tooling, `arm-none-eabi-nm`/`size`/`objcopy`, QEMU gate harness (`tools/run-all-qemu-gates.sh`), silicon bench via LinkServer + `tools/rt1170-console.py`.

**Spec:** `docs/superpowers/specs/2026-09-11-acid-box-itcm-headroom-design.md`

**Findings:** `docs/superpowers/specs/2026-09-12-acid-box-itcm-headroom-findings.md` — the decision record, written after execution: what shipped, what was measured, and which predictions were refuted.

---

## Orientation — read before Task 1

Facts you need that are not obvious from the files:

* **A gate never builds anything, and neither does anything else build a bench directory.** That is why this
  broke silently. `examples/display/acid_box/build-bt` and `build-bench{,-pre,-post}` are hand-configured
  directories whose settings live in untracked `CMakeCache.txt` state.
* **`build-bench`, `build-bench-pre` and `build-bench-post` carry the real firmware blob** —
  `M2RADIO_IW416_BT_FW` = `~/Development/mcuxsdk-ws/.../IW416/uartIW416_bt.bin.inc`, 131,840 bytes. **Never
  reconfigure those with a partial `-D` set** — you would silently strip the blob and the board would try to
  boot the radio from the 1 KB synthetic fallback. Build them with `cmake --build`, never `cmake -B`.
  ★ **`build-bt` does NOT carry it** — its `M2RADIO_IW416_BT_FW` is empty and has been since 2026-09-06.
  Corrected 2026-09-11 after Task 1 measured it; the original plan said the opposite and had Task 4 source
  the blob path from `build-bt`, which would have given every silicon arm a synthetic image and voided the
  bench session.
  ★ **Check the VALUE, never the line.** `grep -c M2RADIO_IW416_BT_FW:FILEPATH` returns 1 for an *empty*
  cache entry, which is how the wrong claim survived into the plan. The honest check is the size of the
  linked symbol: `nm --print-size <elf> | grep ' iw416_bt_fw$'` reads `00000400` for the synthetic image and
  `00020300` for the real one.
* **The linker script is generated at configure time.** `examples/display/acid_box/CMakeLists.txt:178-199`
  reads the core's `imxrt1176.ld` out of `LINK_FLAGS`, does a `string(REPLACE)` on the
  `*libLVGL_flash.a:(.text*)` line, and writes `build-*/acid_box_bt.ld`. Editing a generated `acid_box_bt.ld`
  by hand works until the next configure and then silently reverts — **edit `CMakeLists.txt`**.
* **First match wins in a linker script.** `.text.progmem` is emitted before `.text.itcm`, so anything named
  in the replaced block goes to flash and the later `*(.text*)` never sees it.
* **`EXCLUDE_FILE` matches the archive *member* name** under ld 2.35 (established by the 2026-09-08 swap).
* **The pre-change state does not link**, so there is no "before" ELF to `nm`. The instrument for that is in
  Task 1 Step 2: relink against a 1 MB ITCM to get a map and a probe ELF.

Two rules from the spec that every linker edit in this plan obeys:

1. **Route whole archives with `EXCLUDE_FILE` for named hot exceptions — never an inclusion list of objects.**
2. **Do not capture `.fastrun` in new rules**, so `FASTRUN` keeps meaning "ITCM".

---

## File Structure

| File | Responsibility |
| -- | -- |
| `examples/display/acid_box/CMakeLists.txt` (modify, 162-199) | The `M2_BT_OUT` linker-script derivation: which archives leave ITCM, the headroom assert's constant, and `--print-memory-usage` |
| `examples/display/acid_box/bench` (create) | Declares acid_box's two bench configurations and their cmake flags |
| `tools/build-bench-configs.sh` (create) | Discovers `bench` sidecars, builds each configuration into `build-benchcheck-<name>`, reports `BENCH-BUILDS: PASS` |
| `tools/build-bench-configs.test.sh` (create) | Negative tests proving the tool fails by name, skips correctly, and never touches a directory it does not own |
| `examples/display/acid_box/transcript_hw_evkb_bt.txt` (modify, append) | The NEW-45 arm predictions (written before the bench) and their measured results |
| `CLAUDE.md` (modify) | Per-directory headroom, recorded so the next regression is attributable |
| `docs/superpowers/specs/2026-09-11-acid-box-itcm-headroom-design.md` (modify) | Measured results folded back into §6 |

---

## Task 1: Phase 0 — route `libVGLite` to flash and unbreak the four builds

**Files:**
- Modify: `examples/display/acid_box/CMakeLists.txt:175-191`

- [ ] **Step 1: Confirm the failure and record the exact numbers**

```bash
cd ~/Development/rt1170/evkb
cmake --build examples/display/acid_box/build-bt 2>&1 | grep overflowed
cmake --build examples/display/acid_box/build-bench 2>&1 | grep overflowed
```

Expected — both fail, and these two numbers are the baseline the rest of the task is measured against:

```
ld: region `ITCM' overflowed by 140 bytes
ld: region `ITCM' overflowed by 316 bytes
```

- [ ] **Step 2: Capture the pre-change ITCM symbol set with the inflated-ITCM probe**

The section does not fit, so no ELF exists. Relink against a 1 MB ITCM to get one. This is the instrument
named in spec §2 and it is reused in Step 6.

```bash
cd ~/Development/rt1170/evkb/examples/display/acid_box/build-bt
S=/tmp/new45 && mkdir -p $S
sed 's/ITCM (rx):  ORIGIN = 0x00000000, LENGTH = 256K/ITCM (rx):  ORIGIN = 0x00000000, LENGTH = 1024K/' \
    acid_box_bt.ld > $S/probe.ld
sed -e "s#$(pwd)/acid_box_bt.ld#$S/probe.ld#" \
    -e "s#-o acid_box.elf#-Wl,-Map=$S/before.map -o $S/before.elf#" \
    CMakeFiles/acid_box.elf.dir/link.txt > $S/link.sh
sh $S/link.sh
/Applications/ARM_10/bin/arm-none-eabi-nm --defined-only -C $S/before.elf \
  | awk '$1 ~ /^000[0-9a-f]{5}$/ {print $3}' | sort > $S/before.itcm.syms
wc -l < $S/before.itcm.syms
```

Expected: a non-empty symbol list (several thousand lines). ITCM occupies `0x00000000-0x000FFFFF` in the
probe; flash is `0x30000000+` and RAM `0x20000000+`, so the `^000` address filter selects exactly ITCM.

- [ ] **Step 3: Add the VGLite rule and correct the `.fastrun` note**

In `examples/display/acid_box/CMakeLists.txt`, replace the comment at lines 175-177:

```cmake
    # NB: the appended rule also captures *(.fastrun) from libM2Radio -- today M2Radio
    # marks nothing FASTRUN, so this is inert; if a latency-sensitive M2Radio routine is
    # ever marked FASTRUN for ITCM residency, drop `.fastrun` here so it is not sent to flash.
```

with:

```cmake
    # TWO RULES FOR EVERY ROUTING LINE BELOW (NEW-45, spec 2026-09-11-acid-box-itcm-headroom-design.md §5):
    #  1. Route a WHOLE ARCHIVE with EXCLUDE_FILE for named hot exceptions -- never an inclusion list of
    #     objects.  An exclusion rule makes a newly added library file default to FLASH; an inclusion list
    #     makes it default to ITCM, which is exactly how Avrcp.cpp overflowed ITCM for a day on 2026-09-07.
    #  2. Do NOT capture `.fastrun` in a new rule, so FASTRUN keeps meaning "ITCM" and a library can pin one
    #     hot function back without a linker-script change.  The libM2Radio line below predates this rule and
    #     still captures .fastrun; it is inert only because M2Radio marks nothing FASTRUN.  If a
    #     latency-sensitive M2Radio routine is ever marked FASTRUN, drop `.fastrun` from that line.
```

Then, inside the `string(REPLACE)` replacement text, append a new line directly after the existing
`*libM2Radio*.a:(...)` line (CMakeLists.txt:191) and before the closing `"`:

```cmake
		*libM2Radio*.a:(EXCLUDE_FILE(*Sbc.cpp.obj) .text* EXCLUDE_FILE(*Sbc.cpp.obj) .fastrun)
		/* NEW-45 phase 0.  libVGLite is the largest non-LVGL ITCM consumer (9984 B, of which vg_lite.c is
		   5640) and is coarse-grained BY CONSTRUCTION: on this build the GC355 does the pixels and VGLite is
		   called per DRAW CALL -- the same shape as the SdpServer/L2cap service chain, which measured
		   0.46 us/iter from flash after NEW-36 enabled the I-cache.  Note vg_lite_hal.c/vg_lite_os.c carry
		   the GPU2D handler (attached at vg_lite_hal.c:248), so this puts an ISR in flash: a first-use miss,
		   not a steady-state penalty, and the reason the bench watches ACIDBOX_VSYNC and max_us, not only
		   fps.  No `.fastrun` here, per rule 2 above. */
		*libVGLite*.a:(.text*)"
```

- [ ] **Step 4: Rebuild all four bench directories**

```bash
cd ~/Development/rt1170/evkb
for d in build-bt build-bench build-bench-pre build-bench-post; do
  printf '%-18s ' "$d"
  cmake --build examples/display/acid_box/$d >/dev/null 2>&1 && echo LINKED || echo FAILED
done
```

Expected: `LINKED` four times.

- [ ] **Step 5: Record the new headroom**

```bash
cd ~/Development/rt1170/evkb/examples/display/acid_box
for d in build-bt build-bench; do
  sz=$(/Applications/ARM_10/bin/arm-none-eabi-size -A $d/acid_box.elf | awk '/\.text\.itcm/{print $2}')
  echo "$d .text.itcm=$sz headroom=$((262144 - sz))"
done
```

Expected: `build-bt` headroom ≈ **9,800 B** (was 272), `build-bench` ≈ **9,600 B** (was 96). Write both
numbers down — Task 8 puts them in `CLAUDE.md`, and the whole diagnosis in spec §1 exists only because the
2026-09-08 numbers were recorded.

- [ ] **Step 6: Prove exactly what left ITCM, and that nothing else moved**

```bash
S=/tmp/new45
/Applications/ARM_10/bin/arm-none-eabi-nm --defined-only -C build-bt/acid_box.elf \
  | awk '$1 ~ /^000[0-9a-f]{5}$/ {print $3}' | sort > $S/after.itcm.syms
comm -23 $S/before.itcm.syms $S/after.itcm.syms > $S/left.syms   # in ITCM before, not after
comm -13 $S/before.itcm.syms $S/after.itcm.syms > $S/joined.syms # newly in ITCM -- must be empty
wc -l < $S/left.syms; wc -l < $S/joined.syms
/Applications/ARM_10/bin/arm-none-eabi-nm --defined-only -C \
  $(find . -name libVGLite.a | head -1) | awk '$3 {print $3}' | sort -u > $S/vglite.syms
comm -23 $S/left.syms $S/vglite.syms   # symbols that left ITCM but are NOT VGLite's -- must print nothing
```

Expected: the final `comm` prints **nothing** — that is the load-bearing check, and anything it prints means
the rule moved code it does not name, which is a failed acceptance, not a curiosity: stop and find out why.

★ **`joined.syms` is NOT empty, and should not be.** Measured 2026-09-11: **nine** `__vg_lite_*_veneer`
symbols join ITCM at `0x0003d8e0`-`0x0003d9a0`. They are ld long-branch trampolines, synthesised precisely
*because* the ITCM callers now reach `vg_lite` across the `0x30000000` boundary, and they exist in no input
object — `nm --defined-only build-bt/libVGLite.a | grep -c veneer` is `0`. The ELF already carries the same
class for the flash-routed M2Radio (`___ZN5L2cap*_veneer`), and CLAUDE.md records it for the 2026-09-08
wildcard swap. The acceptance is therefore: **every symbol that joins ITCM must be an ld-synthesised
`*_veneer`, proven absent from the input archives** — not that none joins. Check it:

```bash
grep -v '_veneer$' $S/joined.syms    # must print nothing
```

Net ITCM freed is `9984 - 48 = 9936 B`: libVGLite's `.text` less the veneers it costs.

- [ ] **Step 7: Prove the default (gate) build is byte-identical**

The routing is inside `if(M2_BT_OUT)`, so this must hold by construction — which is exactly why it gets
checked rather than argued.

```bash
cd ~/Development/rt1170/evkb/examples/display/acid_box
/Applications/ARM_10/bin/arm-none-eabi-objcopy -O binary build/acid_box.elf /tmp/new45/gate-before.bin
cmake --build build >/dev/null 2>&1
/Applications/ARM_10/bin/arm-none-eabi-objcopy -O binary build/acid_box.elf /tmp/new45/gate-after.bin
cmp /tmp/new45/gate-before.bin /tmp/new45/gate-after.bin && echo IDENTICAL
```

Expected: `IDENTICAL`. (Compare the `objcopy -O binary` images, never the ELFs — DWARF moves with every
edited line.)

- [ ] **Step 8: Run the QEMU gate**

```bash
cd ~/Development/rt1170/evkb/examples/display/acid_box && ./run_qemu.sh
```

Expected: PASS, with the golden `0x25B30A96` unmoved. Run it as `./run_qemu.sh`, **never `sh run_qemu.sh`** —
it re-execs itself under `gtimeout`.

- [ ] **Step 9: Commit**

```bash
cd ~/Development/rt1170/evkb
git add examples/display/acid_box/CMakeLists.txt
git commit -m "acid_box: route libVGLite to flash -- the M2_BT_OUT bench builds link again (NEW-45 phase 0)

build-bt was 140 B over ITCM and the three ACIDBOX_LOOPSTAT dirs 316 B, both
+412 B past the headroom recorded on 2026-09-08.  libVGLite is the largest
non-LVGL ITCM consumer (9984 B) and coarse-grained by construction: the GC355
does the pixels and VGLite is called per draw call.  Headroom is now ~9.8 KB
(build-bt) and ~9.6 KB (loopstat), from 272 B and 96 B.

Provisional pending the silicon arm (a) in the plan: no fps/vsync claim is made
here, and vg_lite_hal.c's GPU2D handler now runs from flash.

Acceptance: nm-diffed ITCM symbol set -- the symbols that left are exactly
libVGLite's and nothing joined -- and the default gate build's objcopy -O binary
image byte-identical, gate green, golden 0x25B30A96 unmoved.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 2: The headroom instrument — print on every link, assert before the cliff

**Files:**
- Modify: `examples/display/acid_box/CMakeLists.txt` (the `if(M2_BT_OUT)` block)

★ **Locate every insertion point by its anchor TEXT, not by the line numbers below** — Task 1 added 18 lines
to this file, so every line number in this task is stale by roughly that much. The anchors are unique.

- [ ] **Step 1: Add the cache variable and the assert injection**

Immediately after `set(_acidbt_coreld "${CMAKE_MATCH_1}")`, insert:

```cmake
    # NEW-45: make the ITCM margin legible instead of only fatal.  Two halves:
    #   * --print-memory-usage prints region used/size/percent on EVERY link, so a shrinking margin shows up
    #     in ordinary build output rather than only at the cliff;
    #   * an ASSERT so the next NEW-41-sized library growth reports "ITCM headroom below 2048" instead of
    #     "region ITCM overflowed by 140 bytes".  ld cannot interpolate a number into an ASSERT message, so
    #     the literal is injected here at configure time along with the expression.
    # Scoped to this target.  Putting either in the core imxrt1176.ld would change every example in the tree,
    # including ones that legitimately run close -- a separate change with its own blast radius.
    set(ACIDBOX_ITCM_MIN_HEADROOM "2048" CACHE STRING "Fail the M2_BT_OUT link when ITCM headroom drops below this many bytes")
```

- [ ] **Step 2: Inject the ASSERT into the derived script**

Immediately after the existing `file(READ "${_acidbt_coreld}" _acidbt_ld)` — i.e. before the `string(REPLACE)`
that adds the routing lines — add a second replacement anchored on the script's existing EEPROM assert:

```cmake
        string(REPLACE
            "ASSERT(__text_csf_end <= 0x30FC0000, \"Image overlaps the EEPROM flash region (top 256K)\")"
            "ASSERT(__text_csf_end <= 0x30FC0000, \"Image overlaps the EEPROM flash region (top 256K)\")
	ASSERT(SIZEOF(.text.itcm) + SIZEOF(.ARM.exidx) <= LENGTH(ITCM) - ${ACIDBOX_ITCM_MIN_HEADROOM}, \"NEW-45: ITCM headroom below ${ACIDBOX_ITCM_MIN_HEADROOM} bytes -- see docs/superpowers/specs/2026-09-11-acid-box-itcm-headroom-design.md\")"
            _acidbt_ld "${_acidbt_ld}")
```

★ **The expression is `SIZEOF(.text.itcm) + SIZEOF(.ARM.exidx)`, not `.text.itcm` alone** — corrected
2026-09-11 after Task 2's implementer measured it. The ITCM region carries BOTH sections, and there is a
**4-byte alignment gap between them**: `.text.itcm` is 252,336 B at VMA 0 so it ends at 252,336, while
`.ARM.exidx` is 8 B at VMA 252,340. The region's true top is therefore **252,348**, which is what
`--print-memory-usage` reports.

★★ **So the assert is NOT exact, and must not be described as if it were.** The summed expression evaluates
to 252,344 — it does not see the padding, and is **4 B optimistic**. A `.text.itcm`-only assert would have
been 12 B optimistic; this is 4, and 4 B cannot decide anything against a 2,048 B floor. The exact form is
`ADDR(.ARM.exidx) + SIZEOF(.ARM.exidx) - ORIGIN(ITCM)`, and it is **declined deliberately**: the same linker
script already measures the region as `SIZEOF(.text.itcm) + SIZEOF(.ARM.exidx)` in `_itcm_block_count`,
**eighteen lines ABOVE** this assert (line 146 against 165 in the generated `acid_box_bt.ld`). One
convention that is 4 B loose beats two that disagree. Record the slack; do not hide it.

★ **`string(REPLACE)` is a silent no-op when its anchor does not match**, and this anchor is a line of the
*core* `imxrt1176.ld` — a different repository. Reword it upstream and the guard quietly stops being
injected while every build keeps passing unguarded: the exact failure class this feature exists to close,
relocated one level up. So the injection is checked with `string(FIND)` + `message(FATAL_ERROR)`, and the
check is demonstrated RED by breaking the anchor. The *routing* `string(REPLACE)` needs no such check —
if its anchor fails, every routing rule vanishes, ITCM overflows, and the link fails loudly on its own.

- [ ] **Step 3: Add `--print-memory-usage` to the target's link flags**

Replace the existing `set_target_properties(acid_box.elf PROPERTIES LINK_FLAGS "${_acidbt_lf}")` with:

```cmake
        set_target_properties(acid_box.elf PROPERTIES
            LINK_FLAGS "${_acidbt_lf} -Wl,--print-memory-usage")
```

- [ ] **Step 4: Verify the print appears and the assert passes**

`build-bt` must be reconfigured for the new cache variable to reach the generated script. **Reconfigure it
in place with no `-D` at all** — CMake reuses every cached value:

```bash
cd ~/Development/rt1170/evkb/examples/display/acid_box
cmake -B build-bt >/dev/null && cmake --build build-bt 2>&1 | grep -A4 "Memory region"
```

Expected: a memory-usage table naming `ITCM` at roughly 96.26 % used — `252348 B` of `256 KB`. Note that
figure is the **region**, so it exceeds `.text.itcm`'s 252,336 B by the 8 B of `.ARM.exidx` plus alignment;
that is the reason the assert sums both sections.

★ Do the same for `build-bench`, and there **check the blob survived by its linked SIZE, not by grepping the
cache line** — an empty `M2RADIO_IW416_BT_FW:FILEPATH=` still matches a `grep -c`:

```bash
cmake -B build-bench >/dev/null && cmake --build build-bench >/dev/null
/Applications/ARM_10/bin/arm-none-eabi-nm --print-size build-bench/acid_box.elf | grep ' iw416_bt_fw$'
```

Expected: `00020300` (131,840 B, the real image). `00000400` means the blob was stripped — stop and restore
it from `build-bench-pre`'s cache before doing anything else.

- [ ] **Step 5: Demonstrate the assert RED**

An assert never shown to fire is decoration.

```bash
cmake -B build-bt -DACIDBOX_ITCM_MIN_HEADROOM=65536 >/dev/null
cmake --build build-bt 2>&1 | grep "NEW-45"
```

Expected: the link fails with `NEW-45: ITCM headroom below 65536 bytes -- see docs/...`. Then restore:

```bash
cmake -B build-bt -DACIDBOX_ITCM_MIN_HEADROOM=2048 >/dev/null && cmake --build build-bt >/dev/null && echo RESTORED
```

Expected: `RESTORED`.

- [ ] **Step 6: Confirm the gate build is still untouched**

★ Capture the baseline **before editing `CMakeLists.txt`**, if `/tmp/new45/gate-before.bin` is not still
present from Task 1:

```bash
cd ~/Development/rt1170/evkb/examples/display/acid_box
mkdir -p /tmp/new45
[ -f /tmp/new45/gate-before.bin ] || \
  /Applications/ARM_10/bin/arm-none-eabi-objcopy -O binary build/acid_box.elf /tmp/new45/gate-before.bin
```

then, after the edit:

```bash
cmake --build build >/dev/null 2>&1
/Applications/ARM_10/bin/arm-none-eabi-objcopy -O binary build/acid_box.elf /tmp/new45/gate-after2.bin
cmp /tmp/new45/gate-before.bin /tmp/new45/gate-after2.bin && echo IDENTICAL
```

Expected: `IDENTICAL` — everything in this task is inside `if(M2_BT_OUT)`.

- [ ] **Step 7: Commit**

```bash
cd ~/Development/rt1170/evkb
git add examples/display/acid_box/CMakeLists.txt
git commit -m "acid_box: print ITCM usage on every M2_BT_OUT link and assert a 2 KB floor (NEW-45)

--print-memory-usage makes a shrinking margin visible in ordinary build output;
the ASSERT makes the next growth report 'ITCM headroom below 2048' instead of
'region ITCM overflowed by 140 bytes'.  ld cannot interpolate into an ASSERT
message, so the literal is injected at configure time from a cache variable.

Demonstrated RED at -DACIDBOX_ITCM_MIN_HEADROOM=65536.  Scoped to this target:
the same lines in the core imxrt1176.ld would change every example in the tree.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 3: The `bench` sidecar and its builder — tests first

**Files:**
- Create: `tools/build-bench-configs.test.sh`
- Create: `tools/build-bench-configs.sh`
- Create: `examples/display/acid_box/bench`

- [ ] **Step 1: Write the failing test**

The tool must be driveable without a real build — a genuine `cmake --build` of these configurations takes
minutes. `BENCH_CMAKE` is the seam, the same idiom as `qrun`'s `REAL_QEMU` hook. Create
`tools/build-bench-configs.test.sh`:

```sh
#!/bin/sh
# Usage: tools/build-bench-configs.test.sh
#
# Negative tests for build-bench-configs.sh.  A tool that only ever reports PASS
# is indistinguishable from one that looks at nothing, so every check here is a
# case where the tool MUST fail, skip, or stay out of a directory.
#
# Drives the real tool against throwaway trees with a FAKE cmake (BENCH_CMAKE)
# and a FAKE arm-none-eabi-nm/size (ARM_TOOLCHAIN_BIN), so it needs no toolchain,
# no network and no gate builds -- the license-audit and gate-vacuity suites use
# the same technique.
set -e
REPO=$(cd "$(dirname "$0")/.." && pwd)
TOOL="$REPO/tools/build-bench-configs.sh"
fails=0
check() { # check <description> <expected-substring> <actual>
    case "$3" in *"$2"*) echo "PASS: $1" ;;
                 *) echo "FAIL: $1 (wanted '$2')"; fails=$((fails+1)) ;; esac
}
nocheck() { # nocheck <description> <forbidden-substring> <actual>
    case "$3" in *"$2"*) echo "FAIL: $1 (found '$2')"; fails=$((fails+1)) ;;
                 *) echo "PASS: $1" ;; esac
}
rc_is() { # rc_is <description> <expected-rc>
    [ "$rc" -eq "$2" ] && echo "PASS: $1" || { echo "FAIL: $1 (rc=$rc, wanted $2)"; fails=$((fails+1)); }
}

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# A fake cmake that logs its argv, fails for a configuration we mark, and
# otherwise EMULATES A REAL BUILD by dropping an .elf -- unless the configuration
# is marked MAKENOTHING, which models the tool's own historical bug (a missing -S
# configured the root project, which builds nothing and succeeds).
mkdir -p "$WORK/bin"
cat > "$WORK/bin/fakecmake" <<'FAKE'
#!/bin/sh
echo "$@" >> "$FAKE_LOG"
case "$*" in *BREAKME*) echo "ld: region \`ITCM' overflowed by 140 bytes" >&2; exit 1 ;; esac
case "$1" in
  --build) d=$2; ex=$(dirname "$d")
     [ -d "$d" ] && [ ! -f "$d/.makenothing" ] && : > "$d/fake.elf"
     # The tool wipes its owned dir before configuring (by design), so a test
     # cannot seed the dir directly -- the fake build plants what the fake nm
     # will read, exactly as a real build produces the symbols nm reads.
     [ -f "$ex/OWNED_SYMS" ] && cp "$ex/OWNED_SYMS" "$d/ITCM_SYMS"
     [ -f "$ex/OWNED_SIZE" ] && cp "$ex/OWNED_SIZE" "$d/ITCM_SIZE"
     : ;;
  *) nextb=0
     for a in "$@"; do
        if [ "$nextb" = 1 ]; then mkdir -p "$a"
           case "$*" in *MAKENOTHING*) : > "$a/.makenothing" ;; esac
           nextb=0
        fi
        [ "$a" = "-B" ] && nextb=1
     done ;;
esac
exit 0
FAKE
chmod +x "$WORK/bin/fakecmake"

# Fake nm/size.  Both read a sidecar file dropped beside the .elf so a test can
# say what each build "contains" -- ITCM_SYMS and ITCM_SIZE.  A missing tool is
# modelled by pointing ARM_TOOLCHAIN_BIN somewhere empty.
mkdir -p "$WORK/arm"
cat > "$WORK/arm/arm-none-eabi-nm" <<'FAKENM'
#!/bin/sh
elf=$3; [ -n "$elf" ] || elf=$2
d=$(dirname "$elf")
[ -f "$d/ITCM_SYMS" ] || exit 0
i=0
while IFS= read -r s; do
    printf '000%05x T %s\n' "$i" "$s"; i=$((i+1))
done < "$d/ITCM_SYMS"
# an absolute symbol inside the ITCM window, which must be filtered out
printf '00051800 A _flashimagelen\n'
FAKENM
cat > "$WORK/arm/arm-none-eabi-size" <<'FAKESIZE'
#!/bin/sh
d=$(dirname "$2"); s=252336
[ -f "$d/ITCM_SIZE" ] && s=$(cat "$d/ITCM_SIZE")
echo "section size addr"
echo ".text.itcm $s 0"
FAKESIZE
chmod +x "$WORK/arm/arm-none-eabi-nm" "$WORK/arm/arm-none-eabi-size"

mktree() { # mktree <name> ; echoes a REPO-shaped root with a toolchain file
    root="$WORK/$1"; mkdir -p "$root/tools" "$root/toolchain" "$root/examples/display/acid_box"
    cp "$TOOL" "$root/tools/"; : > "$root/toolchain/rt1170-evkb.toolchain.cmake"; echo "$root"
}

# ★ BENCH_CMAKE and ARM_TOOLCHAIN_BIN must be EXPORTED, not merely assigned.  The
# tool is a separate process; `FOO=1 out=$(cmd)` is a simple command with no
# command word, so POSIX applies both as ordinary shell assignments and FOO never
# reaches the child.
export BENCH_CMAKE="$WORK/bin/fakecmake"
export ARM_TOOLCHAIN_BIN="$WORK/arm"
run_tool() { # run_tool <root> <logfile> [args...] ; sets $out and $rc
    _r=$1; _l=$2; shift 2
    FAKE_LOG="$WORK/$_l"; export FAKE_LOG; : > "$FAKE_LOG"
    out=$("$_r/tools/build-bench-configs.sh" "$@" 2>&1) && rc=0 || rc=1
}

# 1. A configuration that does not build must fail BY NAME, and the run must be non-zero.
root=$(mktree fail)
printf 'bt        -DM2_BT_OUT=ON\nbroken    -DBREAKME=ON\n' > "$root/examples/display/acid_box/bench"
run_tool "$root" log1
check "failing config is named"      "display/acid_box[broken]" "$out"
check "failing config reports FAIL"  "BENCH-BUILDS: FAIL"       "$out"
rc_is "broken config exits non-zero" 1
nocheck "a good config is not blamed" "display/acid_box[bt] FAILED" "$out"
# ★ The operator must be shown the line that NAMES the fault, not make's tail.
check "failure names the fault"      "overflowed by 140 bytes"  "$out"

# 2. All-good must PASS, exit 0, and build BOTH declared configurations.
root=$(mktree ok)
printf '# a comment\nbt        -DM2_BT_OUT=ON\nloopstat  -DM2_BT_OUT=ON -DACIDBOX_LOOPSTAT=ON\n' \
    > "$root/examples/display/acid_box/bench"
run_tool "$root" log2
check "clean run passes" "BENCH-BUILDS: PASS" "$out"
check "config count"     "2 configuration(s)" "$out"
rc_is "clean run exits zero" 0
log=$(cat "$WORK/log2")
check "flags reach cmake (bt)"       "-DM2_BT_OUT=ON"        "$log"
check "flags reach cmake (loopstat)" "-DACIDBOX_LOOPSTAT=ON" "$log"
# ★ THE SOURCE DIRECTORY MUST REACH CMAKE.  Without -S, cmake takes the source dir
#   from the CWD; since 912c8d1 the repo root holds a CMakeLists.txt --
#   project(rt1170_evkb_root NONE) with an empty `all` target -- so a run from the
#   repo root configured THE ROOT PROJECT into the example's build dir, built
#   nothing, exited 0 and reported OK.  Measured on the real tree 2026-09-11, and
#   no other arm could see it: they all inspect -B and -D only.  Checked HERE,
#   where $log and $root still refer to the same tree.
check "source dir reaches cmake" "-S $root/examples/display/acid_box" "$log"

# 3. ★ THE LOAD-BEARING ARM.  build-bench and its -pre/-post siblings carry
#    M2RADIO_IW416_BT_FW pointing at a real 131,840-byte blob; a tool that
#    reconfigured one from the declared flags alone would silently strip it.  The
#    tool must only ever name directories it owns.  Both patterns are
#    PATH-ANCHORED: "/build-bench" would be satisfied by the legitimate
#    "/build-benchcheck-..." and prove nothing, and an unanchored " build-bt"
#    would also be satisfied by a tool that named no directory at all.
nocheck "never configures build-bt"     "/build-bt"     "$log"
nocheck "never configures build-bench-" "/build-bench-" "$log"
check   "owns build-benchcheck-bt"       "build-benchcheck-bt"       "$log"
check   "owns build-benchcheck-loopstat" "build-benchcheck-loopstat" "$log"

# 4. A sidecar that is empty after comments is an error, not a silent pass -- the
#    same rule the `boards` parser applies, and for the same reason: a declaration
#    nobody reads hides in a count.
root=$(mktree empty)
printf '# nothing here\n\n' > "$root/examples/display/acid_box/bench"
run_tool "$root" log4
check "empty sidecar is an error" "declares no configuration" "$out"
rc_is "empty sidecar exits non-zero" 1

# 5. A declared name with no flags is an error -- it would configure a DEFAULT
#    build under a bench name, which links fine and proves nothing.
root=$(mktree noflags)
printf 'bt\n' > "$root/examples/display/acid_box/bench"
run_tool "$root" log5
check "name with no flags is an error" "no cmake flags" "$out"
rc_is "flagless declaration exits non-zero" 1

# 6. No sidecar anywhere is a clean no-op, not a failure -- most examples have none.
root=$(mktree none)
run_tool "$root" log6
check "no sidecars passes"   "BENCH-BUILDS: PASS"  "$out"
check "no sidecars counts 0" "0 configuration(s)"  "$out"
rc_is "no sidecars exits zero" 0

# 7. A pattern argument selects a subset.
root=$(mktree pattern)
mkdir -p "$root/examples/audio/bt_tone_test"
printf 'bt  -DM2_BT_OUT=ON\n'    > "$root/examples/display/acid_box/bench"
printf 'soak  -DM2_BT_SOAK=ON\n' > "$root/examples/audio/bt_tone_test/bench"
run_tool "$root" log7 acid_box
check   "pattern selects"        "display/acid_box[bt]" "$out"
nocheck "pattern excludes other" "bt_tone_test"         "$out"

# 8. ★ A pattern that selects NOTHING is not the same claim as "nothing declared",
#    and only the second deserves a pass.  A typo'd pattern printing PASS is the
#    exact disease this tool treats.
run_tool "$root" log8 typo_nonexistent
check "unmatched pattern is an error" "selected none of the" "$out"
rc_is "unmatched pattern exits non-zero" 1

# 9. ★ An unknown option must be rejected, not swallowed as a pattern -- a
#    mistyped flag becoming a filter that matches nothing would print PASS.
run_tool "$root" log9 -z
check "unknown option rejected" "unknown option" "$out"
rc_is "unknown option exits non-zero" 1

# 10. ★ A cmake that exits 0 having produced no .elf must be a FAILURE, not an OK.
#     This catches the class rather than the instance -- -S could be right and the
#     build still make nothing.
root=$(mktree noelf)
printf 'ghost  -DMAKENOTHING=ON\n' > "$root/examples/display/acid_box/bench"
run_tool "$root" log10
check "no .elf is a failure" "BENCH-BUILDS: FAIL" "$out"
check "no .elf says why"     "cmake exited 0 but produced no .elf" "$out"
rc_is "no .elf exits non-zero" 1

# 11. ★ A STALE OWNED DIRECTORY MUST BE WIPED BEFORE CONFIGURING.  A CMake cache
#     retains every -D ever passed, so a flag removed from a `bench` line would
#     stay in effect forever and the tool would measure a configuration nobody
#     declared.  Measured on the real tree 2026-09-11: a one-off
#     -DACIDBOX_ITCM_MIN_HEADROOM=65536 survived into later runs and kept the
#     build red on a clean tree.
root=$(mktree stale)
printf 'bt  -DM2_BT_OUT=ON\n' > "$root/examples/display/acid_box/bench"
mkdir -p "$root/examples/display/acid_box/build-benchcheck-bt"
: > "$root/examples/display/acid_box/build-benchcheck-bt/STALE-CACHE-MARKER"
run_tool "$root" log11
[ -e "$root/examples/display/acid_box/build-benchcheck-bt/STALE-CACHE-MARKER" ] \
  && { echo "FAIL: stale owned dir was NOT wiped before configure"; fails=$((fails+1)); } \
  || echo "PASS: stale owned dir wiped before configure"
check "still builds after the wipe" "BENCH-BUILDS: PASS" "$out"

# 12. ★ AN INDENTED SIDECAR LINE MUST NOT BECOME AN UNNAMED CONFIGURATION.
#     `name=${line%% *}` yields an EMPTY name for a leading-space line, then
#     builds `build-benchcheck-` and hands cmake the real name as a bare
#     positional argument -- measured, and it reported OK.  Tabs must separate too.
root=$(mktree indent)
printf '   bt\t-DM2_BT_OUT=ON\n' > "$root/examples/display/acid_box/bench"
run_tool "$root" log12
check   "indented line keeps its name" "display/acid_box[bt]" "$out"
nocheck "no unnamed configuration"     "acid_box[] "          "$out"
check   "tab separates name from flags" "-DM2_BT_OUT=ON" "$(cat "$WORK/log12")"
nocheck "no build-benchcheck- dir"     "build-benchcheck- "   "$(cat "$WORK/log12")"

# --- the -n nm-diff path -------------------------------------------------------
# Every arm below drives -n, which until 2026-09-11 had NO coverage at all -- and
# it is the path that certifies the tool's central soundness claim, that an owned
# directory is a valid ITCM proxy for the hand-made one.
nmtree() { # nmtree <name> <owned-syms> <owned-size> <human-syms> <human-size>
    root=$(mktree "$1"); ex="$root/examples/display/acid_box"
    printf 'bt  -DM2_BT_OUT=ON\n' > "$ex/bench"
    # what the fake BUILD will plant in the owned dir (it survives the tool's wipe)
    printf '%s\n' "$2" > "$ex/OWNED_SYMS"; printf '%s\n' "$3" > "$ex/OWNED_SIZE"
    # the hand-made directory, which the tool must never touch
    mkdir -p "$ex/build-bt"; : > "$ex/build-bt/acid_box.elf"
    printf '%s\n' "$4" > "$ex/build-bt/ITCM_SYMS"; printf '%s\n' "$5" > "$ex/build-bt/ITCM_SIZE"
    echo "$root"
}

# 13. Matching symbol sets and sizes -> OK, and the pair is COUNTED.
root=$(nmtree nmok "alpha
beta" 252336 "alpha
beta" 252336)
run_tool "$root" log13 -n
check "nm-diff reports OK"        "nm-diff OK: build-benchcheck-bt == build-bt" "$out"
check "nm-diff counts its pairs"  "nm-diff: 1 pair(s) compared" "$out"
check "nm-diff OK passes overall" "BENCH-BUILDS: PASS" "$out"
rc_is "nm-diff OK exits zero" 0
# ★ The fake nm also emits an ABSOLUTE symbol (_flashimagelen) inside the ITCM
#   address window.  It appears on both sides, so this arm would pass either way;
#   arm 19 is what actually pins the filter.

# 14. Differing symbol sets -> DIFFERS, named, and the run fails.
root=$(nmtree nmdiff "alpha
gamma" 252336 "alpha
beta" 252336)
run_tool "$root" log14 -n
check "nm-diff reports DIFFERS"   "nm-diff DIFFERS" "$out"
check "DIFFERS names both causes" "OR the hand-configured" "$out"
check "DIFFERS fails the run"     "BENCH-BUILDS: FAIL" "$out"
rc_is "DIFFERS exits non-zero" 1

# 15. ★ Same symbol NAMES, different .text.itcm SIZE -> must still be DIFFERS.
#     CLAUDE.md's precedent for this check (2026-09-08, the acid_box ITCM wildcard
#     swap) is "an IDENTICAL ITCM symbol set AND .text.itcm size"; names alone
#     would accept two builds whose ITCM footprints differ.
root=$(nmtree nmsize "alpha
beta" 999999 "alpha
beta" 252336)
run_tool "$root" log15 -n
check "size difference is caught" "nm-diff DIFFERS" "$out"
check "size difference is shown"  "999999 vs 252336" "$out"
rc_is "size difference exits non-zero" 1

# 16. ★ A BROKEN nm MUST NOT READ AS OK.  itcm_syms is a pipeline, so its exit
#     status is sort's -- always 0 -- and two EMPTY files compare EQUAL.  Measured
#     2026-09-11 with ARM_TOOLCHAIN_BIN pointed at a nonexistent directory: the
#     tool printed nm-diff OK and BENCH-BUILDS: PASS having read nothing at all.
root=$(nmtree nmbroken "alpha" 252336 "alpha" 252336)
ARM_TOOLCHAIN_BIN="$WORK/no-such-toolchain" run_tool "$root" log16 -n
export ARM_TOOLCHAIN_BIN="$WORK/arm"
check   "broken nm is reported" "nm-diff BROKEN" "$out"
nocheck "broken nm is not OK"   "nm-diff OK"     "$out"
rc_is "broken nm exits non-zero" 1

# 17. ★ -n THAT COMPARES NO PAIRS MUST SAY SO -- silence read as success is the
#     disease this tool treats -- but must NOT FAIL.  A machine that has never
#     benched this example has no hand-made build-<name> to compare against, and
#     every declared configuration may still have built perfectly.  Making it fatal
#     made the root `bench_check` target red for everyone but one bench machine,
#     which is a guard that gets switched off.  An UNREADABLE pair is a different
#     claim and is still fatal -- arm 16.
root=$(mktree nmnopairs)
printf 'bt  -DM2_BT_OUT=ON\n' > "$root/examples/display/acid_box/bench"
run_tool "$root" log17 -n
check "no pairs counts zero"     "nm-diff: 0 pair(s) compared" "$out"
check "no pairs says why"        "verified nothing"            "$out"
rc_is "no pairs does not fail the run" 0

# 18. ★ -n MUST HONOUR THE PATTERN.  Without it the loop globs every owned dir in
#     the tree, so a scoped invocation could go red for a configuration it was told
#     not to touch -- measured 2026-09-11 against a leftover from an earlier run.
root=$(nmtree nmpattern "alpha" 252336 "alpha" 252336)
other="$root/examples/audio/bt_tone_test"
mkdir -p "$other/build-benchcheck-soak" "$other/build-soak"
printf 'soak  -DM2_BT_SOAK=ON\n' > "$other/bench"
: > "$other/build-benchcheck-soak/x.elf"; : > "$other/build-soak/x.elf"
printf 'zzz\n' > "$other/build-benchcheck-soak/ITCM_SYMS"; echo 1 > "$other/build-benchcheck-soak/ITCM_SIZE"
printf 'yyy\n' > "$other/build-soak/ITCM_SYMS";            echo 2 > "$other/build-soak/ITCM_SIZE"
run_tool "$root" log18 -n acid_box
nocheck "-n ignores dirs outside the pattern" "build-benchcheck-soak" "$out"
check   "-n still compares the selected pair" "nm-diff OK: build-benchcheck-bt" "$out"

# 19. ★ ABSOLUTE SYMBOLS MUST BE FILTERED.  nm type A carries a VALUE, not an
#     address: _flashimagelen's value is the image length, and it drifts into the
#     0x000xxxxx window as an image grows.  So the two sides must STRADDLE the
#     window -- one image under 1 MB (value 0x00051800, which the address regex
#     accepts) and one over (0x00151800, which it rejects) -- and then an unfiltered
#     nm puts the symbol in ONE set only and reports a spurious DIFFERS pointing at
#     entirely the wrong cause.
#     ★★ The first version of this arm gave the two sides DIFFERENT absolute VALUES
#     but both inside the window.  awk prints the symbol NAME, so both sets
#     contained _flashimagelen either way and the arm passed with the filter
#     removed -- vacuous, and only the mutation run exposed it.  An arm that cannot
#     fail is worse than no arm.
root=$(nmtree nmabs "alpha" 252336 "alpha" 252336)
cat > "$WORK/arm/arm-none-eabi-nm" <<'FAKENM2'
#!/bin/sh
elf=$3; [ -n "$elf" ] || elf=$2
d=$(dirname "$elf")
[ -f "$d/ITCM_SYMS" ] || exit 0
i=0
while IFS= read -r s; do printf '000%05x T %s\n' "$i" "$s"; i=$((i+1)); done < "$d/ITCM_SYMS"
# the owned build is small, so its _flashimagelen VALUE lands inside the ITCM
# address window; the human build is over 1 MB, so its value lands outside it
case "$d" in *benchcheck*) printf '00051800 A _flashimagelen\n' ;;
             *)            printf '00151800 A _flashimagelen\n' ;; esac
FAKENM2
chmod +x "$WORK/arm/arm-none-eabi-nm"
run_tool "$root" log19 -n
check "absolute symbols are filtered" "nm-diff OK: build-benchcheck-bt" "$out"
rc_is "absolute-symbol filter keeps the run green" 0

# 20. ★ DISCOVERY MUST PRUNE DIRECTORIES, NOT PATH SUBSTRINGS.  `-not -path
#     '*/build*'` matches the WHOLE path, so a checkout under ~/buildfarm or
#     ~/builds prunes EVERYTHING, finds no sidecars and prints PASS -- this tool's
#     own disease, in the tool itself.  Measured 2026-09-11.  No other arm can see
#     it: they all build their throwaway tree under a path with no "build"
#     component, so this arm deliberately puts one there.
root=$(mktree buildfarm)
printf 'bt  -DM2_BT_OUT=ON\n' > "$root/examples/display/acid_box/bench"
run_tool "$root" log20
check "sidecar found under a build* path" "display/acid_box[bt]" "$out"
check "and is actually counted"           "1 configuration(s)"   "$out"
# ...while a sidecar inside a build directory is still pruned, which is the point
# of pruning at all.
mkdir -p "$root/examples/display/acid_box/build-bt"
printf 'ghost  -DSHOULD_NOT_BE_SEEN=ON\n' > "$root/examples/display/acid_box/build-bt/bench"
run_tool "$root" log20b
nocheck "sidecar inside a build dir is pruned" "ghost" "$out"
check   "still exactly one configuration"      "1 configuration(s)" "$out"

# 21. ★ A CRLF SIDECAR MUST NOT LEAK A CARRIAGE RETURN INTO THE LAST FLAG.  The
#     sibling `boards` parser strips it for the same reason.  Without the strip
#     cmake receives -DM2_BT_OUT=ON\r, which is a different flag.
root=$(mktree crlf)
printf 'bt  -DM2_BT_OUT=ON\r\n' > "$root/examples/display/acid_box/bench"
run_tool "$root" log21
check   "CRLF sidecar builds"        "display/acid_box[bt]" "$out"
nocheck "no CR reaches cmake"        "$(printf 'ON\r')"    "$(cat "$WORK/log21")"
check   "the flag itself is intact"  "-DM2_BT_OUT=ON"       "$(cat "$WORK/log21")"

echo "-------------------------------------------------------------"
if [ "$fails" -eq 0 ]; then echo "build-bench-configs tests PASS"
else echo "$fails failure(s)"; exit 1; fi
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
chmod +x tools/build-bench-configs.test.sh && ./tools/build-bench-configs.test.sh
```

Expected: FAIL — `tools/build-bench-configs.sh: No such file or directory` (the `cp` in `mktree` errors under
`set -e`).

- [ ] **Step 3: Write the tool**

Create `tools/build-bench-configs.sh`:

```sh
#!/bin/sh
# build-bench-configs.sh — configure and BUILD every declared bench configuration.
#
# Gates do not build, and until NEW-45 nothing built a BENCH directory either:
# display/acid_box's M2_BT_OUT builds sat broken for two days in September 2026
# before an unrelated workstream happened to rebuild them.  An example declares
# its bench configurations in a `bench` sidecar, one per line, `<name> <flags>`:
#
#     # examples/display/acid_box/bench
#     bt      -DM2_BT_OUT=ON
#     bench   -DM2_BT_OUT=ON -DACIDBOX_LOOPSTAT=ON
#
# Usage: build-bench-configs.sh [-n] [<pattern>]
#   -n         also nm-diff each owned dir's ITCM symbol set and .text.itcm size
#              against the human's build-<name>, where one exists
#   <pattern>  substring of the example path, e.g. acid_box
#
# ★ THE TOOL BUILDS INTO DIRECTORIES IT OWNS -- build-benchcheck-<name> -- and
#   never touches a human's bench directory.  build-bench, -pre and -post carry
#   M2RADIO_IW416_BT_FW pointing at the real firmware image (131,840 bytes,
#   supplied as a .bin.inc C array); a tool
#   that reconfigured one from the declared flags alone would silently strip it
#   and the bench would run the 1 KB synthetic image instead.  That is the
#   2026-08-27 red inverted (there, a bench-configured dir made its own gate
#   fail).  build-bench-configs.test.sh asserts the tool never names one.
#
# ★ A configuration counts as built only if an .elf actually appeared.  "cmake
#   said 0" is not the same claim, and this tool exists precisely because a
#   build that silently does nothing reads as green.  Every "measured nothing"
#   path below is therefore a FAILURE, not a quiet pass: no pairs compared, no
#   symbols read, a pattern that selected nothing.
#
# Exit 0 = BENCH-BUILDS: PASS.  Run from anywhere.  NEVER concurrently with the
# QEMU sweep: CLAUDE.md records a sweep invalidated by a concurrent licence
# audit, and this is heavier than that.
set -e
# ★ set -f for the whole script: `set -- $line` below splits a sidecar line into
# words, and without it a flag containing * or ? would glob against the cwd.
# Nothing here wants pathname expansion.
set -f
NL='
'
REPO=$(cd "$(dirname "$0")/.." && pwd)
CMAKE=${BENCH_CMAKE:-cmake}          # the seam build-bench-configs.test.sh drives
TOOLBIN=${ARM_TOOLCHAIN_BIN:-/Applications/ARM_10/bin}
TOOLCHAIN="$REPO/toolchain/rt1170-evkb.toolchain.cmake"

# A here-doc, not sed on $0: a line-number range goes stale the moment the header
# is edited, and it had -- `-h` printed the sidecar example and no option list.
usage() {
    cat <<'USAGE'
Usage: build-bench-configs.sh [-n] [<pattern>]
  -n         also nm-diff each owned dir's ITCM symbol set and .text.itcm size
             against the human's build-<name>, where one exists
  <pattern>  substring of the example path, e.g. acid_box
USAGE
}

NMDIFF=0
PATTERN=""
while [ $# -gt 0 ]; do
    case "$1" in
        -n)        NMDIFF=1 ;;
        -h|--help) usage; exit 0 ;;
        # ★ Reject unknown options rather than treating them as a pattern: a
        # mistyped flag that silently becomes a filter matching nothing would
        # print PASS, which is the failure mode this whole tool is about.
        -*)        echo "error: unknown option '$1'" >&2; usage >&2; exit 2 ;;
        *)         [ -z "$PATTERN" ] || { echo "error: only one pattern allowed" >&2; exit 2; }
                   PATTERN=$1 ;;
    esac
    shift
done

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
: > "$WORK/fails"; n=0; found=0; selected=0

IFS=$NL
# ★ PRUNE BY DIRECTORY, NOT BY PATH SUBSTRING.  `-not -path '*/build*'` matches the
# WHOLE path, so a checkout living under any directory whose name starts with
# "build" -- ~/buildfarm/evkb, ~/builds/evkb -- prunes EVERYTHING, finds no
# sidecars and prints BENCH-BUILDS: PASS.  Measured 2026-09-11.  That is this
# tool's own disease (green while measuring nothing) in the tool itself, and no
# test arm could see it: every arm builds its throwaway tree under a path with no
# "build" component.  This is the idiom the sibling runner already uses
# (run-all-qemu-gates.sh) -- prune DIRECTORIES named build*, then print files.
for sidecar in $(find "$REPO/examples" -type d -name 'build*' -prune -o -type f -name bench -print | sort); do
    IFS=$NL
    dir=$(dirname "$sidecar"); rel=${dir#"$REPO"/}
    found=$((found+1))
    case "$rel" in *"$PATTERN"*) ;; *) continue ;; esac
    selected=$((selected+1))
    # tr -d '\r' so a CRLF sidecar does not leak a carriage return into the last
    # flag -- the sibling `boards` parser strips it for the same reason.
    body=$(grep -v '^[[:space:]]*#' "$sidecar" | tr -d '\r' | grep -v '^[[:space:]]*$' || true)
    if [ -z "$body" ]; then
        echo "error: $rel/bench declares no configuration (empty after stripping comments)"
        echo "$rel/bench" >> "$WORK/fails"; continue
    fi
    # A here-doc feeds the loop on stdin so it runs in THIS shell, not a pipeline
    # subshell -- otherwise every failure recorded below would be discarded.
    while IFS= read -r line; do
        [ -n "$line" ] || continue
        # ★ Split on whitespace into positional parameters rather than with
        # ${line%% *}: that idiom yields an EMPTY name for an indented line and
        # then builds `build-benchcheck-` while handing cmake the real name as a
        # bare positional argument -- measured, and it reported OK.  This form
        # also takes tabs as separators, which the sibling `boards` parser does.
        IFS=' 	'; set -- $line; IFS=$NL
        name=$1; shift
        if [ $# -eq 0 ]; then
            echo "error: $rel/bench: '$name' declares no cmake flags"
            echo "$rel[$name]" >> "$WORK/fails"; continue
        fi
        n=$((n+1))
        bdir="$dir/build-benchcheck-$name"
        printf '%-44s ' "$rel[$name]"
        # ★★ ALWAYS CONFIGURE FROM SCRATCH.  A CMake cache RETAINS every -D ever
        # passed, so a flag REMOVED from a `bench` line would stay in effect in
        # this directory forever and the tool would be measuring a configuration
        # nobody declared -- the one thing it exists to rule out.  Same trap
        # CLAUDE.md records: "editing a set(... CACHE ...) DEFAULT does not change
        # an existing build directory ... Check the symbol size, not the source."
        # Measured 2026-09-11: a one-off -DACIDBOX_ITCM_MIN_HEADROOM=65536, passed
        # once to demonstrate the headroom assert going red, survived in the cache
        # and kept the build FAILED on an otherwise unmodified tree.
        rm -rf "$bdir"
        # ★ -S IS LOAD-BEARING.  Without it cmake takes the source directory from
        # the CWD, and since 912c8d1 the repo root holds a CMakeLists.txt --
        # project(rt1170_evkb_root NONE), whose `all` target is empty -- so a run
        # from the repo root CONFIGURES THE ROOT PROJECT into this example's build
        # dir, builds nothing, exits 0 and reports OK.  Measured 2026-09-11.
        # Before 912c8d1 the same omission was LOUD, which is why it survived
        # review: the failure mode is newer than the idiom.
        if "$CMAKE" -S "$dir" -B "$bdir" -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" "$@" > "$WORK/out" 2>&1 \
           && "$CMAKE" --build "$bdir" >> "$WORK/out" 2>&1; then
            if [ -n "$(find "$bdir" -maxdepth 1 -name '*.elf' -print -quit)" ]; then
                echo OK
            else
                echo FAILED
                echo "    cmake exited 0 but produced no .elf -- the build claimed success and made nothing"
                echo "$rel[$name]" >> "$WORK/fails"
            fi
        else
            echo FAILED
            # Show the lines that NAME the fault, not the last line.  make's
            # generic "Error 2" sits nine lines below ld's actual diagnosis, and
            # CMake's own failures say "CMake Error at", which matches neither
            # ld: nor error: -- measured on the headroom-assert RED demo.
            _why=$(grep -E 'ld:|error:|CMake Error' "$WORK/out" | head -3)
            [ -n "$_why" ] || _why=$(tail -3 "$WORK/out")
            printf '%s\n' "$_why" | sed 's/^/    /'
            echo "$rel[$name]" >> "$WORK/fails"
        fi
    done <<EOF
$body
EOF
done
IFS=$NL

# ★ A pattern that selects nothing is NOT the same claim as "nothing is declared
# anywhere", and only the second deserves a pass.  A typo'd pattern printing
# BENCH-BUILDS: PASS is the exact disease this tool treats.
if [ -n "$PATTERN" ] && [ "$selected" -eq 0 ] && [ "$found" -gt 0 ]; then
    echo "error: pattern '$PATTERN' selected none of the $found example(s) with a bench sidecar"
    echo "pattern:$PATTERN" >> "$WORK/fails"
fi

# ITCM is 0x00000000-0x000FFFFF; flash is 0x30000000+, RAM 0x20000000+.  Absolute
# symbols (nm type A) carry a VALUE, not an address -- _flashimagelen's value is
# the image length and drifts into this window once an image reaches 1 MB, which
# would read as a spurious symbol-set difference pointing at the wrong cause.
itcm_syms() {
    "$TOOLBIN/arm-none-eabi-nm" --defined-only "$1" \
      | awk '$2 != "A" && $2 != "a" && $1 ~ /^000[0-9a-f]{5}$/ {print $3}' | sort
}
itcm_size() { "$TOOLBIN/arm-none-eabi-size" -A "$1" | awk '/^\.text\.itcm/ {print $2}'; }

if [ "$NMDIFF" -eq 1 ]; then
    pairs=0
    for owned in $(find "$REPO/examples" -maxdepth 3 -type d -name 'build-benchcheck-*' | sort); do
        rel_o=${owned#"$REPO"/}
        # ★ Honour the pattern here too.  Without this the loop globs EVERY owned
        # directory in the tree, including leftovers from an earlier run of a
        # different example, so a scoped invocation could go red for a
        # configuration it was told not to touch -- measured 2026-09-11.
        case "$rel_o" in *"$PATTERN"*) ;; *) continue ;; esac
        # NOTE: pairs are discovered by glob, not from the declared set, so a
        # RENAMED or DELETED bench entry leaves a gitignored build-benchcheck-<old>
        # that keeps being diffed against a stale build-<old>.  The DIFFERS text
        # names staleness as a cause; delete the orphan when a name changes.
        human=$(printf '%s' "$owned" | sed 's/build-benchcheck-/build-/')
        [ -d "$human" ] || continue
        a=$(find "$owned" -maxdepth 1 -name '*.elf' | head -1)
        b=$(find "$human" -maxdepth 1 -name '*.elf' | head -1)
        [ -n "$a" ] && [ -n "$b" ] || continue
        pairs=$((pairs+1))
        itcm_syms "$a" > "$WORK/owned.syms" 2>/dev/null || true
        itcm_syms "$b" > "$WORK/human.syms" 2>/dev/null || true
        # ★ Two empty files compare EQUAL.  A missing arm-none-eabi-nm, an
        # unreadable ELF or a stripped image would otherwise print nm-diff OK
        # having read nothing at all -- measured with ARM_TOOLCHAIN_BIN pointed
        # at a nonexistent directory, which reported OK and PASS.
        if [ ! -s "$WORK/owned.syms" ] || [ ! -s "$WORK/human.syms" ]; then
            echo "nm-diff BROKEN: read no ITCM symbols from ${owned##*/} or ${human##*/}"
            echo "  (is $TOOLBIN/arm-none-eabi-nm present and are both ELFs readable?)"
            echo "${owned##*/}:nm-unreadable" >> "$WORK/fails"; continue
        fi
        sa=$(itcm_size "$a"); sb=$(itcm_size "$b")
        # ★ Size as well as names.  CLAUDE.md's own precedent for this check
        # (2026-09-08, the acid_box ITCM wildcard swap) is "an IDENTICAL ITCM
        # symbol set AND .text.itcm size, nm-diffed, not eyeballed" -- names
        # alone would accept two builds whose ITCM footprints differ.
        if cmp -s "$WORK/owned.syms" "$WORK/human.syms" && [ -n "$sa" ] && [ "$sa" = "$sb" ]; then
            echo "nm-diff OK: ${owned##*/} == ${human##*/} (ITCM symbol set + .text.itcm $sa)"
        else
            echo "nm-diff DIFFERS: ${owned##*/} vs ${human##*/} (.text.itcm $sa vs $sb)"
            echo "  EITHER the proxy is not equivalent, OR the hand-configured ${human##*/} is stale:"
            echo "  the tool wipes its own directory every run and never touches that one."
            diff "$WORK/human.syms" "$WORK/owned.syms" | head -20 | sed 's/^/    /'
            echo "${owned##*/}:nm" >> "$WORK/fails"
        fi
    done
    echo "nm-diff: $pairs pair(s) compared"
    # ★ Zero pairs must be SAID, not merely implied -- silence read as success is
    # what this tool is for.  But it must not FAIL: a machine that has never
    # benched this example has no hand-made build-<name> to compare against, and
    # every declared configuration may still have built perfectly.  Making it
    # fatal made the root `bench_check` target red for everyone but the one bench
    # machine, which is a guard that gets switched off.  An unreadable pair is a
    # different claim and is still fatal, above (nm-diff BROKEN).
    if [ "$pairs" -eq 0 ]; then
        echo "  no hand-made build-<name> found beside any build-benchcheck-*, so -n verified nothing"
        echo "  (normal on a machine that has never benched these examples)"
    fi
fi

echo "bench: $n configuration(s)"
if [ -s "$WORK/fails" ]; then
    echo "BENCH-BUILDS: FAIL"; sed 's/^/  /' "$WORK/fails"; exit 1
fi
echo "BENCH-BUILDS: PASS"
```

- [ ] **Step 4: Run the test to verify it passes**

```bash
chmod +x tools/build-bench-configs.sh && ./tools/build-bench-configs.test.sh
```

Expected: `build-bench-configs tests PASS`.

- [ ] **Step 4b: Mutation-test the suite — a test never shown to fail is decoration**

Green proves the tool passes its tests; it does not prove the tests *can* fail. Eleven mutations, one per
guarantee the tool makes. Run them with the harness below rather than by hand: **it asserts each anchor is
present**, so a recipe that has gone stale fails loudly instead of silently mutating nothing.

```bash
cd ~/Development/rt1170/evkb
cp tools/build-bench-configs.sh /tmp/tool.orig
python3 - <<'EOF'
import subprocess, os
orig = open('/tmp/tool.orig').read()
MUTANTS = [
 ("prune by path substring again",
    """find "$REPO/examples" -type d -name 'build*' -prune -o -type f -name bench -print""",
    """find "$REPO/examples" -name bench -not -path '*/build*'"""),
 ("zero pairs fatal again",
    '''        echo "  no hand-made build-<name> found beside any build-benchcheck-*, so -n verified nothing"''',
    '''        echo "nm:no-pairs" >> "$WORK/fails"'''),
 ("drop the CRLF strip",              "| tr -d '\\r' ", ""),
 ("drop the pre-configure wipe",      '        rm -rf "$bdir"\n', ''),
 ("drop -S",                          '"$CMAKE" -S "$dir" -B "$bdir"', '"$CMAKE" -B "$bdir"'),
 ("parser back to ${line%% *}",
    '        IFS=\' \t\'; set -- $line; IFS=$NL\n        name=$1; shift\n        if [ $# -eq 0 ]; then',
    '        name=${line%% *}; set -- ${line#"$name"}\n        if [ $# -eq 0 ]; then'),
 ("accept empty symbol reads",
    '        if [ ! -s "$WORK/owned.syms" ] || [ ! -s "$WORK/human.syms" ]; then', '        if false; then'),
 ("compare names only, not size",
    'if cmp -s "$WORK/owned.syms" "$WORK/human.syms" && [ -n "$sa" ] && [ "$sa" = "$sb" ]; then',
    'if cmp -s "$WORK/owned.syms" "$WORK/human.syms"; then'),
 ("-n ignores the pattern",
    '        case "$rel_o" in *"$PATTERN"*) ;; *) continue ;; esac\n', ''),
 ("zero pairs passes",
    '    if [ "$pairs" -eq 0 ]; then\n        echo "error: -n compared no pairs, so it proved nothing"\n        echo "nm:no-pairs" >> "$WORK/fails"\n    fi', '    :'),
 ("absolute symbols not filtered",     '$2 != "A" && $2 != "a" && ', ''),
 ("unmatched pattern passes",
    'if [ -n "$PATTERN" ] && [ "$selected" -eq 0 ] && [ "$found" -gt 0 ]; then', 'if false; then'),
 ("unknown option becomes a pattern",
    "        -*)        echo \"error: unknown option '$1'\" >&2; usage >&2; exit 2 ;;\n", ''),
]
bad = 0
for label, old, new in MUTANTS:
    assert old in orig, f"STALE MUTANT RECIPE -- anchor missing for: {label}"
    open('tools/build-bench-configs.sh','w').write(orig.replace(old, new, 1))
    os.chmod('tools/build-bench-configs.sh', 0o755)
    r = subprocess.run(['./tools/build-bench-configs.test.sh'], capture_output=True, text=True)
    if r.returncode == 0:
        print(f"!! STAYED GREEN !!   {label}"); bad += 1
    else:
        print(f"RED                  {label}")
        for l in r.stdout.split("\n"):
            if l.startswith("FAIL:"): print("      ", l)
open('tools/build-bench-configs.sh','w').write(orig); os.chmod('tools/build-bench-configs.sh', 0o755)
r = subprocess.run(['./tools/build-bench-configs.test.sh'], capture_output=True, text=True)
print("\nRESTORED:", r.stdout.strip().split("\n")[-1])
raise SystemExit(1 if bad else 0)
EOF
```

Expected: **`RED` on all eleven**, then `RESTORED: build-bench-configs tests PASS`. A `!! STAYED GREEN !!` line
means that guarantee has no working test — stop and report it.

★★ **Three more guarantees were added after a second review round, and two of the three were invisible to
every arm that existed.** `-not -path '*/build*'` matches the WHOLE path, so a checkout under `~/buildfarm`
or `~/builds` pruned everything, found no sidecars and printed PASS — the tool's own disease, in the tool;
no arm could see it because every arm builds its throwaway tree under a path with no `build` component, so
arm 20 deliberately puts one there. And `-n` treating zero pairs as FATAL made the root `bench_check` target
red on every machine without hand-made bench directories, which is a guard that gets switched off; zero
pairs is now a loud notice that names the reason, while an *unreadable* pair stays fatal.

★★ **Two of these arms were vacuous when first written, and only a mutation run found either.** The `.elf`
recipe went stale when the check moved into a nested `if` — `str.replace` matched nothing, changed nothing,
and the suite stayed green, which is why the harness now asserts its anchors. And the absolute-symbol arm
gave the two sides different absolute *values* but both inside the ITCM address window; `awk` prints the
symbol *name*, so both sets contained `_flashimagelen` either way and the arm passed with the filter
removed. It now makes the two sides **straddle** the window, which is the only configuration that
discriminates. An arm that cannot fail is worse than no arm, because it is counted.

★ **One fix in this task is deliberately NOT covered by a mutant, and that is recorded rather than papered
over**: printing the `ld:`/`error:`/`CMake Error` lines instead of make's generic last line. The fake cmake
emits a single line, so the suite cannot tell the two apart — it is a bench-ergonomics fix whose only
evidence is Step 7 on the real tree.

- [ ] **Step 5: Create acid_box's sidecar**

Create `examples/display/acid_box/bench`:

```
# Bench configurations for this example -- configurations that no gate builds.
# Built by tools/build-bench-configs.sh into build-benchcheck-<name>; see
# docs/superpowers/specs/2026-09-11-acid-box-itcm-headroom-design.md §7.
#
# ★ These are NOT the directories a human benches from.  build-bench, -pre and
#   -post carry a real 131,840-byte M2RADIO_IW416_BT_FW blob in their CMakeCache;
#   the tool owns build-benchcheck-* and never touches any of them.
#   (build-bt's M2RADIO_IW416_BT_FW is EMPTY and has been since 2026-09-06, so it
#   links the 1 KB synthetic fallback -- measured by symbol size, 00000400 against
#   build-bench's 00020300.  Check the VALUE, never the cache LINE: an empty
#   FILEPATH entry still matches a grep, which is how the opposite claim got
#   written here in the first place.)
#
# ★ build-bench-pre and build-bench-post are NOT declared: they differ from
#   build-bench only by the CORE PIN (TEENSY_LIB_ROOT state, not a cmake flag),
#   so they are historical A/B directories from NEW-36, not configurations.
#
# ★★ THE NAMES ARE NOT FREE.  The tool builds build-benchcheck-<name> and its -n
#   check pairs that with build-<name>, so the second entry is `bench` and NOT
#   `loopstat`: build-loopstat exists and is M2_BT_OUT=OFF, so a `loopstat` entry
#   would pair a BT build against a non-BT one and report a spurious nm-diff
#   DIFFERS on every run -- and a guard that cries wolf gets switched off.
# ★ The `bench` pairing also EARNS the proxy check: build-bench carries the real
#   131,840-byte blob and build-benchcheck-bench gets the 1 KB synthetic one, so
#   an identical ITCM symbol set across that pair MEASURES the claim this tool
#   rests on -- that the blob lands in .progmem and never touches ITCM.
#
# <name>    <cmake flags>
bt      -DM2_BT_OUT=ON
bench   -DM2_BT_OUT=ON -DACIDBOX_LOOPSTAT=ON
```

- [ ] **Step 6: Run the tool for real, and prove the proxy**

★ **This step does two full from-scratch builds** — LVGL, Audio, the core and the rest compile per build
directory. Budget 10-20 minutes and do not interrupt it.

```bash
cd ~/Development/rt1170/evkb && ./tools/build-bench-configs.sh -n acid_box
```

Expected:

```
examples/display/acid_box[bt]                OK
examples/display/acid_box[bench]             OK
nm-diff OK: build-benchcheck-bench == build-bench (ITCM symbol sets)
nm-diff OK: build-benchcheck-bt == build-bt (ITCM symbol sets)
bench: 2 configuration(s)
BENCH-BUILDS: PASS
```

The two `nm-diff OK` lines are the claim that the tool's directories are valid ITCM proxies for the humans',
measured rather than asserted — and the `bench` one proves the firmware blob (real in `build-bench`,
synthetic in its proxy) never reaches ITCM. The `bt` pair is synthetic on both sides, so it proves only that
the declared flags reproduce the hand-made directory.

- [ ] **Step 7: Demonstrate the tool RED against the real tree**

The negative arms in Step 1 run against a fake cmake. Prove the tool also catches a real overflow:

```bash
cd ~/Development/rt1170/evkb
sed -i.bak 's/^bt      -DM2_BT_OUT=ON$/bt      -DM2_BT_OUT=ON -DACIDBOX_ITCM_MIN_HEADROOM=65536/' \
    examples/display/acid_box/bench
./tools/build-bench-configs.sh acid_box; echo "exit=$?"
mv examples/display/acid_box/bench.bak examples/display/acid_box/bench
```

Expected: `examples/display/acid_box[bt]  FAILED`, **the `ld: NEW-45: ITCM headroom below 65536` line echoed
directly beneath it** (not make's generic `Error 2` — that is the point of the diagnosis-line fix, and this
step is its only evidence), `BENCH-BUILDS: FAIL`, and `exit=1`.

★ **This demonstration used to poison the owned directory.** `-DACIDBOX_ITCM_MIN_HEADROOM=65536` was written
into `build-benchcheck-bt`'s cache and survived every later run, keeping the build FAILED on an otherwise
clean tree — measured 2026-09-11. The tool now wipes each owned directory before configuring, so a re-run
after restoring the sidecar must come back `BENCH-BUILDS: PASS` **with no manual cleanup**. Verify that:

```bash
./tools/build-bench-configs.sh acid_box; echo "exit=$?"
```

Expected: both configurations `OK`, `BENCH-BUILDS: PASS`, `exit=0`.

- [ ] **Step 7b: Give the tool something that runs it**

★★ **A tool nobody is told to run does not close a discovery gap** — it documents one. The failure this
whole task exists to prevent went unnoticed for two days precisely because no script, target or checklist
named the thing that would have caught it. So wire it in, the same way the repo's other whole-tree checks
are wired.

Add to the root `CMakeLists.txt`, after the `build_all_examples` target and in the same idiom:

```cmake
# Build every BENCH configuration declared in an example's `bench` sidecar.
# Gates never build, and until NEW-45 nothing built a bench directory either:
# display/acid_box's M2_BT_OUT builds sat broken for two days before an unrelated
# workstream happened to rebuild them.  Run at close-out, NEVER concurrently with
# the QEMU sweep -- CLAUDE.md records a sweep invalidated by a concurrent licence
# audit, and this is heavier than that.
add_custom_target(bench_check
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tools/build-bench-configs.sh" -n
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    USES_TERMINAL
    COMMENT "Building every declared bench configuration..."
)
```

Verify it resolves (this does **not** run the builds):

```bash
cd ~/Development/rt1170/evkb
cmake -B /tmp/new45-roottgt -S . >/dev/null && cmake --build /tmp/new45-roottgt --target help 2>/dev/null | grep bench_check
rm -rf /tmp/new45-roottgt
```

Expected: `... bench_check`.

★ The CLAUDE.md half of this is Task 7's job — the close-out narrative is where every other repo-wide check
(`license-audit.sh`, the vacuity suite) is recorded with its measured result, and a check absent from that
narrative is a check nobody runs.

- [ ] **Step 8: Keep the owned directories out of git**

```bash
cd ~/Development/rt1170/evkb
grep -q 'build-benchcheck' .gitignore || printf 'build-benchcheck-*/\n' >> .gitignore
git status --porcelain examples/display/acid_box | grep benchcheck && echo "STILL TRACKED" || echo CLEAN
```

Expected: `CLEAN`.

- [ ] **Step 9: Commit**

```bash
cd ~/Development/rt1170/evkb
git add tools/build-bench-configs.sh tools/build-bench-configs.test.sh \
        examples/display/acid_box/bench .gitignore
git commit -m "tools: build every declared bench configuration (NEW-45)

Nothing in this tree builds a bench directory, which is why acid_box's
M2_BT_OUT builds sat broken for two days before an unrelated workstream
rebuilt them.  An example now declares its bench configurations in a \`bench\`
sidecar, same idiom as \`boards\`, and build-bench-configs.sh builds each one.

★ The tool owns build-benchcheck-<name> and never touches build-bt or
build-bench*: build-bt carries M2RADIO_IW416_BT_FW pointing at a real
131,840-byte blob, and reconfiguring it from the declared flags alone would
silently strip it.  A test asserts the tool never names one.  -n nm-diffs the
owned dir against the human's to prove the ITCM footprint is equivalent rather
than assuming it (the blob lands in .progmem, not ITCM).

Nine negative arms via a fake cmake (BENCH_CMAKE, the qrun REAL_QEMU idiom),
five of them mutation-tested, plus a real RED demonstrated against the tree with
the headroom assert forced.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 4: Build the silicon arms and register the predictions before the bench

**Files:**
- Modify: `examples/display/acid_box/CMakeLists.txt` (temporarily, per arm)
- Modify: `examples/display/acid_box/transcript_hw_evkb_bt.txt` (append)

★ **Predictions go into the transcript before the board is flashed.** A prediction written after the
measurement is a description.

- [ ] **Step 1: Append the prediction block to the transcript**

Append to `examples/display/acid_box/transcript_hw_evkb_bt.txt`:

```
================================================================================
NEW-45 / NEW-37 ITCM ROUTING ARMS -- PREDICTIONS, written 2026-09-11 BEFORE the run
================================================================================
Spec: docs/superpowers/specs/2026-09-11-acid-box-itcm-headroom-design.md §6
Baseline: the NEW-36 I-CACHE A/B section above (POST column), same protocol, one
variable per arm, last four loopstat lines averaged in each window.
Every arm carries ACIDBOX_UI_SUM == 0x1479CEE8.  A moved checksum voids the arm:
placement changes no pixels.

  arm  change                                    prediction
  (a)  libVGLite -> flash                        no change.  VGLite is called per draw call --
                                                 the shape of the svc chain, 0.46 us/iter from flash
  (b)  + MipiDisplay, Wire, TouchPanel           ACIDBOX_VSYNC timeouts=0 holds; touch p95 no worse
                                                 than 78 ms (the worse NEW-36 reading; that sample
                                                 wanders 78->43 with no cause, so nothing more is
                                                 claimed from it)
  (c)  + Sbc.cpp  (NEW-37 option 1)              enc 125 -> ~190 us/block.  At 344 blocks/s that is
                                                 +22 ms/s of CPU against 63k iterations/s -- free in
                                                 aggregate, 15x real time vs the 2.902 ms block period.
                                                 JUDGED ON pcmdrops=0 and drops no worse, NOT on
                                                 NEW-37's "within 50 %" line, which lands at 187 us
                                                 against a predicted 190 -- a coin toss, not a threshold
  (d)  LVGL -> flash, PLACEMENT ONLY             30 fps -> 20 fps.  The lvgl slot costs 844 ms/s at
                                                 30 fps (~28 ms of a 33.3 ms budget); x1.5 is ~42 ms,
                                                 past the next vsync multiple.  Run anyway: one linker
                                                 line, 203 KB if the prediction is wrong

Arms are CUMULATIVE and ordered by confidence, so a regression names its own cause.
Arm (d) is run LAST and stands alone against (a): a 20 fps result makes anything
stacked on it unreadable.
```

- [ ] **Step 2: Commit the predictions before building anything**

```bash
cd ~/Development/rt1170/evkb
git add examples/display/acid_box/transcript_hw_evkb_bt.txt
git commit -m "acid_box: register the NEW-45 arm predictions before the bench (NEW-45)

Written before the board is flashed.  A prediction recorded afterwards is a
description.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

- [ ] **Step 3: Build arm (a)**

Arm (a) is Task 1's committed state. Build it into its own directory so all four arms coexist:

```bash
cd ~/Development/rt1170/evkb/examples/display/acid_box
cmake -B build-arm-a -DM2_BT_OUT=ON -DACIDBOX_LOOPSTAT=ON \
      -DM2RADIO_IW416_BT_FW=$(grep M2RADIO_IW416_BT_FW:FILEPATH build-bench/CMakeCache.txt | cut -d= -f2) \
      -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake
cmake --build build-arm-a 2>&1 | grep -A4 "Memory region"
```

Expected: links, ITCM ~96 % used.

★ The `M2RADIO_IW416_BT_FW` value is copied out of **`build-bench`**, not `build-bt` — `build-bt`'s is EMPTY
(measured 2026-09-11). Verify the arm actually got the real image before flashing anything, by size:

```bash
/Applications/ARM_10/bin/arm-none-eabi-nm --print-size build-arm-a/acid_box.elf | grep ' iw416_bt_fw$'
```

Expected `00020300`. If it reads `00000400` the arm carries the 1 KB synthetic image, the radio will never
come up, and every measurement taken from it is void. **Run this check on all four arms.**

- [ ] **Step 4: Add the arm (b) rule, build arm (b)**

Append three lines to the `string(REPLACE)` replacement in `examples/display/acid_box/CMakeLists.txt`,
directly after the `*libVGLite*.a:(.text*)` line added in Task 1:

```cmake
		/* NEW-45 arm (b): three more archives that are cold or coarse BY CONSTRUCTION -- I2C to the codec
		   and touch controller at UI rate, and the DSI bring-up plus flip path.  libMipiDisplay's
		   lcdifv2.cpp carries the LCDIFv2 vsync ISR, which is why the bench watches ACIDBOX_VSYNC and
		   max_us on this arm and not only fps.  No `.fastrun`, per rule 2 above. */
		*libMipiDisplay*.a:(.text*)
		*libWire*.a:(.text*)
		*libTouchPanel*.a:(.text*)
```

Then:

```bash
cd ~/Development/rt1170/evkb/examples/display/acid_box
cmake -B build-arm-b -DM2_BT_OUT=ON -DACIDBOX_LOOPSTAT=ON \
      -DM2RADIO_IW416_BT_FW=$(grep M2RADIO_IW416_BT_FW:FILEPATH build-bench/CMakeCache.txt | cut -d= -f2) \
      -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake
cmake --build build-arm-b 2>&1 | grep -A4 "Memory region"
```

Expected: links, ITCM usage ~4.4 KB lower than arm (a).

- [ ] **Step 5: Add the arm (c) rule, build arm (c)**

Arm (c) drops `Sbc.cpp`'s ITCM exemption — NEW-37 option 1. Change the existing libM2Radio line from:

```
		*libM2Radio*.a:(EXCLUDE_FILE(*Sbc.cpp.obj) .text* EXCLUDE_FILE(*Sbc.cpp.obj) .fastrun)
```

to:

```
		*libM2Radio*.a:(.text*)
```

Then:

```bash
cmake -B build-arm-c -DM2_BT_OUT=ON -DACIDBOX_LOOPSTAT=ON \
      -DM2RADIO_IW416_BT_FW=$(grep M2RADIO_IW416_BT_FW:FILEPATH build-bench/CMakeCache.txt | cut -d= -f2) \
      -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake
cmake --build build-arm-c 2>&1 | grep -A4 "Memory region"
/Applications/ARM_10/bin/arm-none-eabi-nm --defined-only -C build-arm-c/acid_box.elf \
  | grep -c 'Sbc'   # informational
```

Expected: links, ITCM a further ~2.3 KB lower.

- [ ] **Step 6: Build arm (d) — LVGL placement only**

Revert the arm (c) change (restore the `EXCLUDE_FILE` line), and revert the arm (b) lines, leaving arm (a)'s
VGLite rule. Then add ONE line after it:

```cmake
		/* NEW-45 arm (d), EXPERIMENT ONLY -- do not commit without the fps number.  LVGL is 203050 B of the
		   256 K and the sw renderer is the per-frame hot path; predicted 30 -> 20 fps.  PLACEMENT only: the
		   renderer is unchanged, so ACIDBOX_UI_SUM must not move. */
		*libLVGL.a:(.text*)
```

```bash
cmake -B build-arm-d -DM2_BT_OUT=ON -DACIDBOX_LOOPSTAT=ON \
      -DM2RADIO_IW416_BT_FW=$(grep M2RADIO_IW416_BT_FW:FILEPATH build-bench/CMakeCache.txt | cut -d= -f2) \
      -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake
cmake --build build-arm-d 2>&1 | grep -A4 "Memory region"
```

Expected: links with ITCM around 20 % used.

- [ ] **Step 7: Restore `CMakeLists.txt` to the committed arm (a) state**

```bash
cd ~/Development/rt1170/evkb
git checkout examples/display/acid_box/CMakeLists.txt
git status --porcelain examples/display/acid_box/CMakeLists.txt   # must print nothing
```

The four arm ELFs are already built and are what the bench flashes; the working tree goes back to the
committed state so nothing half-edited can be committed by accident.

---

## Task 5: The silicon session

**Files:**
- Modify: `examples/display/acid_box/transcript_hw_evkb_bt.txt` (append results)

★ **Bench hazards, all recorded in CLAUDE.md and all met before on this example:**
- **Do not hold the VCOM while programming.** `LinkServer flash … load` dies with `status 131` if
  `rt1170-console.py` has the port. Order: `flash load` → `flash verify` → attach reader → SW4.
- `pkill LinkServer` alone leaves `redlinkserv` and `crt_emu_cm_redlink` resident and silently kills the next
  few runs.
- The MCU-Link DAP wedges after repeated flash/run/kill cycles: **replug the DEBUG USB** — a board power
  cycle does *not* clear it.
- Read `DHCSR` (0xE000EDF0) before believing any freeze — `0x01010001` is healthy-running,
  `0x00030003` is halted-by-debugger. `tools/rt1170-swdprobe.py --health` prints the block.

- [ ] **Step 1: Run each arm on the board**

For each of `build-arm-a`, `build-arm-b`, `build-arm-c`, `build-arm-d`, in that order:

```bash
cd ~/Development/rt1170/evkb/examples/display/acid_box
pkill LinkServer; pkill redlinkserv; pkill crt_emu_cm_redlink
LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load build-arm-a/acid_box.elf
LinkServer flash MIMXRT1176:MIMXRT1170-EVKB verify build-arm-a/acid_box.elf
python3 ../../../tools/rt1170-console.py /dev/cu.usbmodem* 115200 | tee /tmp/new45-arm-a.log
# then press SW4 to free-run
```

Protocol per arm, identical to the NEW-36 A/B section: wait for `bt_streaming` → PLAY → tap the "ACID BOX"
title (`wiggle=1`) ~20 s → tap again (`wiggle=0`) → stream ≥ 60 s → CUTOFF drag.

- [ ] **Step 2: Record, per arm, exactly these**

```
ACIDBOX_ENGINE, ACIDBOX_UI_SUM, ACIDBOX_VSYNC timeouts
framestat: fps, median frame interval (us), in the wiggle=1 window
loopstat (last 4 lines averaged, both windows): loops/s, svc, print, poll/enc, drain, yield, lvgl, max_us
touchstat p95 (final) and n
bt report: pcmdrops, drops, frames_per_pkt, media_mtu
```

- [ ] **Step 3: Apply the verdicts**

| arm | accept if | reject if |
| -- | -- | -- |
| (a) | `UI_SUM=0x1479CEE8`, `VSYNC timeouts=0`, 30 fps, `pcmdrops=0`, `drops` no worse | fps < 30, any vsync timeout, or `max_us` materially above the 12.4 ms baseline |
| (b) | as (a), plus touch p95 ≤ 78 ms | as (a) |
| (c) | as (a); `enc` is *recorded*, not a gate | `pcmdrops > 0` or `drops` worse |
| (d) | 30 fps held **and** `UI_SUM` unmoved | anything less — record the fps and drop the arm |

★ If an arm is rejected, **record the number that rejected it** in the transcript. A refuted prediction with
its measurement is the most valuable line in the file; a silently dropped arm is a gap nobody can see.

- [ ] **Step 4: Append the results to the transcript, under the predictions**

Write the measured table beside the predicted one, with a `HELD` / `REFUTED` verdict per row — the NEW-36
section's format, which is the house style for exactly this.

- [ ] **Step 5: Commit the results**

```bash
cd ~/Development/rt1170/evkb
git add examples/display/acid_box/transcript_hw_evkb_bt.txt
git commit -m "acid_box: NEW-45 silicon arms measured -- <one-line verdict summary>

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 6: Commit the routing that measurement supports

**Files:**
- Modify: `examples/display/acid_box/CMakeLists.txt`

- [ ] **Step 1: Apply the winning arm's rules**

Re-apply exactly the rules from the highest accepted arm (Task 4 Steps 4-6 hold their text verbatim). If arm
(a) was rejected, remove the VGLite rule and apply arm (b)'s three archives plus `Sbc.cpp` instead — spec §10
records that this still yields ~6.7 KB, 24× the 2026-09-08 headroom.

- [ ] **Step 2: Rebuild all four bench directories and record final headroom**

```bash
cd ~/Development/rt1170/evkb/examples/display/acid_box
for d in build-bt build-bench build-bench-pre build-bench-post; do
  cmake --build $d >/dev/null 2>&1 || { echo "$d FAILED"; continue; }
  sz=$(/Applications/ARM_10/bin/arm-none-eabi-size -A $d/acid_box.elf | awk '/\.text\.itcm/{print $2}')
  echo "$d .text.itcm=$sz headroom=$((262144 - sz))"
done
```

Expected: four lines, each with headroom in the thousands.

- [ ] **Step 3: Re-prove the two structural acceptances**

```bash
cmake --build build >/dev/null 2>&1
/Applications/ARM_10/bin/arm-none-eabi-objcopy -O binary build/acid_box.elf /tmp/new45/gate-final.bin
cmp /tmp/new45/gate-before.bin /tmp/new45/gate-final.bin && echo IDENTICAL
./run_qemu.sh
```

Expected: `IDENTICAL`, then the gate PASSes with golden `0x25B30A96`.

- [ ] **Step 4: Commit**

```bash
cd ~/Development/rt1170/evkb
git add examples/display/acid_box/CMakeLists.txt
git commit -m "acid_box: commit the measured ITCM routing (NEW-45, closes NEW-37's option 1)

<arms accepted / refuted, with the numbers>

Default gate build's objcopy -O binary image byte-identical; gate green, golden
0x25B30A96 unmoved.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 7: Fold the results back into the spec and CLAUDE.md

**Files:**
- Modify: `docs/superpowers/specs/2026-09-11-acid-box-itcm-headroom-design.md` (§6)
- Modify: `CLAUDE.md`

- [ ] **Step 1: Record the measured column in the spec's §6 table**

Add a `measured` column beside `prediction`, and a `HELD`/`REFUTED` verdict per arm. Do not delete a refuted
prediction — strike it in place with its reason, the way spec §3's declined options are kept.

- [ ] **Step 2: Add the CLAUDE.md entry**

Insert next to the existing 2026-09-07/08 acid_box ITCM notes. It must carry, at minimum:

```
★ **acid_box's M2_BT_OUT bench builds overflowed ITCM again (NEW-45, 2026-09-11) and the fix was headroom,
not bytes.** build-bt was 140 B over and the three ACIDBOX_LOOPSTAT dirs 316 B -- both exactly +412 B past
the headroom recorded on 2026-09-08, which is what made it attributable: an identical delta across builds
that share only their libraries is library growth, and the only pins that had moved were cores a9b0de5 and
Audio ff610a2 (NEW-41's audioPllTrimPpm/headphoneVolume). The 2026-09-08 M2Radio wildcard HELD -- M2Radio
contributes Sbc.cpp alone. ★ The budget, measured by relinking against a 1 MB ITCM for a map (the section
does not fit, so there is no ELF to nm): libLVGL 203050 B of the 256 K, and the lvgl slot already costs
844 ms/s at 30 fps -- so LVGL is in ITCM on EVIDENCE and the other 53 KB was there by DEFAULT. Routed
<archives> to flash; headroom now build-bt <N> B, loopstat dirs <N> B. ★ ITCM cannot grow: the FlexRAM split
is 8 DTCM + 8 ITCM banks and a non-power-of-2 count leaves the window unbacked. ★ Two rules now in the
script: route WHOLE ARCHIVES with EXCLUDE_FILE for named exceptions (an inclusion list makes a new library
file default to ITCM -- exactly the Avrcp.cpp bug), and never capture `.fastrun` in a new rule so FASTRUN
keeps meaning ITCM. ★ Nothing built a bench dir, which is why this was found by an unrelated workstream two
days later: `tools/build-bench-configs.sh` now builds every configuration declared in an example's `bench`
sidecar, into build-benchcheck-* directories IT OWNS -- reconfiguring build-bt from the declared flags would
silently strip its real 131,840-byte firmware blob. Run it at close-out, NEVER concurrently with the sweep.
```

Fill the `<...>` placeholders with the measured values. Also update the CLAUDE.md line that records the
2026-09-08 headroom figures (`build-bt` **272 B**, loopstat dirs **96 B**) so the next reader diffs against
the current numbers.

- [ ] **Step 3: Commit**

```bash
cd ~/Development/rt1170/evkb
git add CLAUDE.md docs/superpowers/specs/2026-09-11-acid-box-itcm-headroom-design.md
git commit -m "docs: NEW-45 measured results, headroom recorded per build dir

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 8: Close-out

- [ ] **Step 1: Run the bench-config tool and its tests**

```bash
cd ~/Development/rt1170/evkb
./tools/build-bench-configs.test.sh && ./tools/build-bench-configs.sh -n
```

Expected: `build-bench-configs tests PASS`, then `BENCH-BUILDS: PASS` with `nm-diff OK`.

- [ ] **Step 2: Run the full QEMU sweep — alone**

★ One sweep at a time, output captured, nothing else running. CLAUDE.md records a sweep invalidated by a
concurrent licence audit; `build-bench-configs.sh` is heavier than that audit.

```bash
cd ~/Development/rt1170/evkb
./tools/run-all-qemu-gates.sh -l | tail -1          # confirm the count before trusting the sweep
./tools/run-all-qemu-gates.sh 2>&1 | tee /tmp/new45-sweep.log | tail -20
```

Expected: **139 gates discovered**, `gates: 139 passed`, exit 0. If a member of the documented
load-sensitivity class is red (`cm4_audio_test`, `cm4_wire_int_slave_test`, `m2_rx_demo[txaggr]`,
`m2_uap_lwip[uap]`, `synthui_slide_toggle_test`, `bt_tone_test[media]`), **re-run it idle and record both
results** — do not weaken it and do not assume. `0 SKIP` is the load-bearing number: a non-zero SKIP means
the sweep measured less than the count suggests.

- [ ] **Step 3: Licence audit and vacuity suite — after the sweep, never during**

```bash
cd ~/Development/rt1170/evkb
LICENSE_AUDIT_EVKB=$(pwd) ./tools/license-audit.sh | tail -5
./tools/gate-vacuity.test.sh | tail -5
```

Expected: `LICENSE-AUDIT: PASS` and the vacuity suite green at its current count. No new `GATES` entry is
needed — `build-benchcheck-*` directories are not gates, nothing runs them in QEMU, and they link libraries
already audited through `audio/bt_tone_test`. Spec §9 records that so the omission does not later read as
drift.

- [ ] **Step 4: Note what is deliberately NOT done**

No `-DEVKB_FORCE_FETCH=ON` fresh-user verification: **no library pin moves in this change.** Every edit is in
the `evkb` repo itself. Say so explicitly in the close-out rather than leaving the omission to be noticed.

- [ ] **Step 5: Update the Linear issues**

- NEW-45 → Done, with the headroom numbers and the sweep result.
- NEW-37 → Done if arms (c) and (d) both settled; otherwise **narrow it in writing** to exactly what they
  left open (e.g. "arm (d) refuted at 20 fps; the LVGL lever is closed until the sw renderer's per-frame cost
  changes") rather than leaving it vague.

---

## Self-Review

**Spec coverage.** §1 → Task 1 Step 1 (reproduce) and Task 7 (record). §2 → Task 1 Step 2 (the probe
instrument) and Task 4 (arm b's archives). §3 → the arm (d) experiment in Tasks 4-5; the FlexRAM ceiling and
the declined GPU-renderer option are context, not tasks. §4 phases 0/1/2/3 → Tasks 1 / 2+3 / 4+5 / 6+7. §5's
two rules → Task 1 Step 3 (written into the script) and every later rule. §6 → Tasks 4-5. §7.1 → Task 3
Step 5. §7.2 → Task 3 Steps 1-9, with the "never touches build-bt" requirement as a named test arm. §7.3 →
Task 2. §8 → Task 1 Steps 5-8, Task 6 Step 3, Task 8. §9 → Task 8 Step 3 (no GATES entry) and Step 4. §10 →
Task 6 Step 1's fallback and Task 5's per-arm verdicts.

**Placeholder scan.** The only `<...>` are in Task 6/7 commit messages and the CLAUDE.md block, where the
content is a measurement that does not exist until Task 5 runs — each is explicitly labelled as a value to
fill from the recorded numbers, not as work left undescribed.

**Type consistency.** `build-benchcheck-<name>` is spelled identically in the tool, its tests, the sidecar
comment, `.gitignore` and Task 8. `ACIDBOX_ITCM_MIN_HEADROOM` is the same name in the cache variable, the
injected ASSERT message, the RED demonstration and Task 3 Step 7. `BENCH_CMAKE` is the same seam in the tool
and every test arm. The arm directories `build-arm-{a,b,c,d}` are consistent across Tasks 4, 5 and 6.
