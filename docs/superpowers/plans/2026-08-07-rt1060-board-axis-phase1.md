# RT1060 Board Axis (Phase 1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give this repo a board dimension so a single example can be built and
gated for both the MIMXRT1170-EVKB (`rt1176`) and the MIMXRT1060-EVKB
(`rt1062`), proven end to end by `examples/serial/serial_test` passing on both
— with every existing `rt1176` image byte-identical.

**Architecture:** One cache variable `EVKB_BOARD` (default `rt1176`) drives
`evkb.cmake`'s core selection. Gates stop naming a QEMU machine: `gate-lib.sh`
derives `-M`/`-global`/build-dir from `$EVKB_BOARD`. The sweep runner reads a
per-example `boards` sidecar file (absent ⇒ `rt1176` only) and runs each gate
once per declared board, reporting `<board>:<category>/<name>`.

**Tech Stack:** CMake 3.24, POSIX sh, ARM GCC 10, qemu2 (`mimxrt1170-evk` /
`mimxrt1060-evk`), `teensy-cmake-macros` (`TEENSY_VERSION` 117 / 42).

**Spec:** `docs/superpowers/specs/2026-08-07-rt1060-board-axis-design.md`

---

## Critical context for the implementer

Read these before Task 1. Each has cost someone time in this tree.

- **Run gates as `./run_qemu.sh`, never `sh run_qemu.sh`.** `gate_init` does
  `exec gtimeout ... "$0"`, which needs the shebang.
- **Never count gates with `find`.** Ask `./tools/run-all-qemu-gates.sh -l`.
  Its trailing `(N gate(s))` line means `wc -l` is one MORE than the count.
- **Zero SKIP is load-bearing.** A SKIP means a gate silently never ran.
- **`dualcore/cm4_audio_test` is a documented intermittent** — see
  `docs/KNOWN-BROKEN-GATES.md`. It is load-sensitive; check `uptime` before
  believing a lone dual-core red. Do not delete or weaken it.
- **Do not hold the VCOM during any LinkServer operation.** Not needed in this
  phase (no silicon work), but the rule stands.
- **The three files a gate currently hardcodes** are `-M mimxrt1170-evk`,
  `-global fsl-imxrt1170.boot-xip=on`, and `build/<name>.elf`. All 81 gates
  have all three.

**Baseline to record before touching anything** (Task 0).

---

### Task 0: Record the byte-identity baseline

**Files:**
- Create: `/tmp/rt1060-axis-baseline.txt` (scratch, not committed)

- [ ] **Step 1: Confirm the tree is clean and on the right branch**

```bash
cd ~/Development/rt1170/evkb && git status --short && git branch --show-current
```

Expected: only `?? cores/` and `?? teensy-cmake-macros/` (both are nested
independent repos — normal), and branch `rt1060-board-axis`.

- [ ] **Step 2: Record hashes of a representative sample of built images**

```bash
cd ~/Development/rt1170/evkb && shasum -a256 \
  examples/serial/serial_test/build/serial_test.elf \
  examples/framework/string_test/build/string_test.elf \
  examples/gpio-analog/dac_test/build/dac_test.elf \
  examples/dualcore/cm4_audiostream_test/build/*.cm4.bin \
  examples/usb/usb_audio_uac1_test/build/usb_audio_uac1_test.elf \
  | tee /tmp/rt1060-axis-baseline.txt
```

Expected: five lines of `<sha256>  <path>`. If any file is missing, build that
example first — the baseline must cover a `dualcore` (CM4 image), a `usb`, and
plain CM7 examples.

- [ ] **Step 3: Record the gate count and sweep baseline**

```bash
cd ~/Development/rt1170/evkb && ./tools/run-all-qemu-gates.sh -l | tail -1
```

Expected: `(81 gate(s))`

---

### Task 1: Board helpers in `gate-lib.sh`

**Files:**
- Modify: `tools/gate-lib.sh` (append after `gate_require_capture`, before `gate_cleanup`)
- Test: `tools/gate-lib.test.sh`

- [ ] **Step 1: Write the failing tests**

Append to `tools/gate-lib.test.sh`, following the existing `test_*` naming:

```sh
test_board_defaults_to_rt1176() {
    unset EVKB_BOARD
    _got=$(gate_board)
    [ "$_got" = "rt1176" ] || fail "gate_board default: got '$_got'"
}

test_machine_args_rt1176() {
    EVKB_BOARD=rt1176
    _got=$(gate_qemu_machine)
    [ "$_got" = "-M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on" ] \
        || fail "rt1176 machine args: got '$_got'"
}

test_machine_args_rt1062() {
    EVKB_BOARD=rt1062
    _got=$(gate_qemu_machine)
    [ "$_got" = "-M mimxrt1060-evk -global fsl-imxrt1062.boot-xip=on" ] \
        || fail "rt1062 machine args: got '$_got'"
}

test_build_dir_rt1176_is_plain_build() {
    EVKB_BOARD=rt1176
    _got=$(gate_build_dir)
    [ "$_got" = "build" ] || fail "rt1176 build dir: got '$_got'"
}

test_build_dir_rt1062_is_suffixed() {
    EVKB_BOARD=rt1062
    _got=$(gate_build_dir)
    [ "$_got" = "build-rt1062" ] || fail "rt1062 build dir: got '$_got'"
}

test_unknown_board_fails_loudly() {
    EVKB_BOARD=rt9999
    if gate_qemu_machine >/dev/null 2>&1; then
        fail "unknown board should exit non-zero, not fall through to a default"
    fi
}
```

Note `gate_build_dir` is **asymmetric on purpose**: `rt1176` keeps the plain
`build/` so all 81 existing gates, the runner's SKIP probe and every documented
`cmake -B build` command are untouched. Only new boards get a suffix.

- [ ] **Step 2: Run the tests to verify they fail**

```bash
cd ~/Development/rt1170/evkb && ./tools/gate-lib.test.sh
```

Expected: FAIL, reporting `gate_board: command not found` (or equivalent) for
the six new tests. The pre-existing tests must still pass.

- [ ] **Step 3: Implement the helpers**

Insert into `tools/gate-lib.sh` immediately after the `gate_require_capture`
function and before `gate_cleanup`:

```sh
# --- board axis --------------------------------------------------------------
# A gate never names a QEMU machine. It asks here, and the answer comes from
# $EVKB_BOARD (default rt1176). That is what stops a gate booting the wrong
# machine: with the name in 81 scripts, adding a board meant 81 chances to get
# it wrong, and a gate that boots the wrong model can still PASS vacuously.

gate_board() { echo "${EVKB_BOARD:-rt1176}"; }

# Emit the -M/-global pair for the current board. Both machines expose a
# "boot-xip" bool (fsl-imxrt1170.c / fsl-imxrt1062.c DEFINE_PROP_BOOL), so only
# the SoC type name differs. Unknown boards EXIT rather than defaulting: a typo
# in EVKB_BOARD must be a loud failure, never a silent run on the other board.
gate_qemu_machine() {
    case "$(gate_board)" in
        rt1176) echo "-M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on" ;;
        rt1062) echo "-M mimxrt1060-evk -global fsl-imxrt1062.boot-xip=on" ;;
        *) echo "gate-lib: unknown EVKB_BOARD '$(gate_board)'" >&2; exit 2 ;;
    esac
}

# Build directory for the current board. rt1176 keeps the plain "build" so the
# 81 pre-existing gates, the sweep runner's SKIP probe and every documented
# `cmake -B build` line are unaffected; new boards are suffixed.
gate_build_dir() {
    case "$(gate_board)" in
        rt1176) echo "build" ;;
        rt1062) echo "build-rt1062" ;;
        *) echo "gate-lib: unknown EVKB_BOARD '$(gate_board)'" >&2; exit 2 ;;
    esac
}
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cd ~/Development/rt1170/evkb && ./tools/gate-lib.test.sh
```

Expected: all tests PASS, including the six new ones.

- [ ] **Step 5: Commit**

```bash
cd ~/Development/rt1170/evkb
git add tools/gate-lib.sh tools/gate-lib.test.sh
git commit -m "gate-lib: board helpers so no gate names a QEMU machine

A gate that names its own machine is a gate that can boot the wrong model and
still pass vacuously; with the name in 81 scripts, adding a second board meant
81 chances to get it wrong. gate_qemu_machine() is now the only place the -M
and -global pair exists.

Unknown EVKB_BOARD exits 2 rather than defaulting, so a typo is a loud failure
instead of a silent run against the other board. gate_build_dir() is
deliberately asymmetric -- rt1176 keeps the plain build/ so the 81 existing
gates, the runner's SKIP probe and every documented cmake -B build line are
untouched.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 2: Convert one gate, prove rt1176 unchanged

**Files:**
- Modify: `examples/serial/serial_test/run_qemu.sh:13,16`

- [ ] **Step 1: Convert the gate**

Replace lines 13 and 16–17 of `examples/serial/serial_test/run_qemu.sh`.

Line 13 becomes:

```sh
ELF="$DIR/$(gate_build_dir)/serial_test.elf"
```

Lines 16–17 become:

```sh
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/serial.dbg" &
```

`$(gate_qemu_machine)` is deliberately **unquoted** — it expands to four
separate words. Quoting it passes one argument containing spaces and QEMU
rejects it.

- [ ] **Step 2: Run the gate and verify it still passes**

```bash
cd ~/Development/rt1170/evkb/examples/serial/serial_test && ./run_qemu.sh
```

Expected: `PASS: QEMU serial output verified`, exit 0. The captured UART must
still contain `RT1176 Serial1 up` and `count=3`.

- [ ] **Step 3: Commit**

```bash
cd ~/Development/rt1170/evkb
git add examples/serial/serial_test/run_qemu.sh
git commit -m "serial_test: take the QEMU machine from gate-lib, not a literal

First gate converted to the board axis. Behaviour on rt1176 is identical --
gate_qemu_machine() emits exactly the -M/-global pair this gate spelled out.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 3: Convert the remaining 80 gates

**Files:**
- Modify: every `examples/**/run_qemu*.sh` except the one done in Task 2

- [ ] **Step 1: Confirm the exact strings are uniform before rewriting**

```bash
cd ~/Development/rt1170/evkb
find examples -type d -name 'build*' -prune -o -name 'run_qemu*.sh' -type f -print \
  | xargs grep -c "mimxrt1170-evk" | grep -v ":1$" || echo "all gates mention it exactly once"
```

Expected: `all gates mention it exactly once`. If any gate mentions it more
than once, convert that gate by hand and exclude it from Step 2.

- [ ] **Step 2: Rewrite mechanically**

```bash
cd ~/Development/rt1170/evkb
find examples -type d -name 'build*' -prune -o -name 'run_qemu*.sh' -type f -print \
  | xargs sed -i '' \
    -e 's|-M mimxrt1170-evk -global fsl-imxrt1170\.boot-xip=on|$(gate_qemu_machine)|g' \
    -e 's|"\$DIR/build/|"$DIR/$(gate_build_dir)/|g'
```

- [ ] **Step 3: Verify no literal survives**

```bash
cd ~/Development/rt1170/evkb
find examples -type d -name 'build*' -prune -o -name 'run_qemu*.sh' -type f -print \
  | xargs grep -l "mimxrt1170-evk" || echo "no gate names a machine any more"
```

Expected: `no gate names a machine any more`. Any file still listed had a
variant spelling — fix it by hand and re-run.

- [ ] **Step 4: Full sweep — every gate must be exactly as green as the baseline**

Check load first; this sweep is only meaningful on an idle machine.

```bash
uptime && cd ~/Development/rt1170/evkb && ./tools/run-all-qemu-gates.sh -j 2
```

Expected: `81 passed, 0 failed` with zero SKIP, **or** `80 passed, 1 failed`
where the single failure is `dualcore/cm4_audio_test` (the documented
intermittent). Any other failure is a real regression from this rewrite —
diagnose it, do not proceed.

- [ ] **Step 5: Commit**

```bash
cd ~/Development/rt1170/evkb
git add examples
git commit -m "gates: no gate names a QEMU machine any more (80 remaining)

Mechanical sweep of the same change serial_test took in the previous commit.
Every gate now asks gate-lib for its machine and build directory, so adding a
board is one case arm rather than 81 edits.

Verified by full sweep, not by inspection: same result as the pre-refactor
baseline, zero SKIP.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 4: Board dimension in the sweep runner

**Files:**
- Modify: `tools/run-all-qemu-gates.sh` (discovery loop ~126-137; `run_gate` ~155-190)

- [ ] **Step 1: Write the failing test**

Create `tools/board-axis.test.sh`:

```sh
#!/bin/sh
# Proves the sweep runner honours a per-example `boards` sidecar.
set -e
REPO=$(cd "$(dirname "$0")/.." && pwd)
fails=0
check() { # check <description> <expected-substring> <actual>
    case "$3" in *"$2"*) ;; *) echo "FAIL: $1 (wanted '$2')"; fails=$((fails+1)) ;; esac
}

# serial_test has no `boards` file yet -> rt1176 only, listed with the prefix.
out=$("$REPO/tools/run-all-qemu-gates.sh" -l serial/serial_test)
check "default board prefix" "rt1176:serial/serial_test" "$out"
check "no rt1062 without a sidecar" "" "$out"
case "$out" in *rt1062*) echo "FAIL: rt1062 listed with no sidecar"; fails=$((fails+1));; esac

[ "$fails" -eq 0 ] && echo "board-axis tests PASS" || { echo "$fails failure(s)"; exit 1; }
```

```bash
chmod +x ~/Development/rt1170/evkb/tools/board-axis.test.sh
```

- [ ] **Step 2: Run it to verify it fails**

```bash
cd ~/Development/rt1170/evkb && ./tools/board-axis.test.sh
```

Expected: FAIL — `default board prefix (wanted 'rt1176:serial/serial_test')`,
because the runner currently emits `serial/serial_test` with no board prefix.

- [ ] **Step 3: Add the board dimension to discovery**

In `tools/run-all-qemu-gates.sh`, replace the discovery loop body (currently
`dir=$(dirname "$gate")` through `NGATES=$((NGATES + 1))`) with:

```sh
    dir=$(dirname "$gate")
    ex=${dir#"$REPO"/examples/}
    # An example declares its boards in a `boards` sidecar, one per line.
    # Absent means rt1176 only -- that default is what keeps all 81 pre-existing
    # examples untouched by this change.
    if [ -f "$dir/boards" ]; then
        _boards=$(grep -v '^[[:space:]]*#' "$dir/boards" | tr -d ' \t' | grep -v '^$')
    else
        _boards=rt1176
    fi
    for _b in $_boards; do
        id="$_b:$ex"
        matches "$id" || continue
        GATES="$GATES$id	$gate"$'\n'
        NGATES=$((NGATES + 1))
    done
```

- [ ] **Step 4: Make `run_gate` board-aware**

In `run_gate`, the id now carries the board. Insert at the top of the function,
immediately after `_dir=$(dirname "$_path")`:

```sh
    _board=${_id%%:*}
    _bdir=build; [ "$_board" = rt1176 ] || _bdir="build-$_board"
```

Replace the SKIP probe (`if ! ls "$_dir"/build/*.elf ...`) with:

```sh
    if ! ls "$_dir/$_bdir"/*.elf >/dev/null 2>&1; then
        echo "no $_bdir/*.elf in $_dir — build the example for $_board first:" > "$_log"
        echo "  cd $_dir && cmake -B $_bdir -DEVKB_BOARD=$_board -DCMAKE_TOOLCHAIN_FILE=toolchain/$_board-evkb.toolchain.cmake && cmake --build $_bdir" >> "$_log"
        echo "SKIP 0" > "$RESULT_DIR/$_slug.result"; return
    fi
```

And pass the board into the gate — replace the execution subshell with:

```sh
    ( unset GATE_GUARDED
      export GATE_TIMEOUT="$GATE_TIMEOUT_SECS"
      export EVKB_BOARD="$_board"
      cd "$_dir" && "$_path" ) >"$_log" 2>&1
```

- [ ] **Step 5: Run the test to verify it passes**

```bash
cd ~/Development/rt1170/evkb && ./tools/board-axis.test.sh
```

Expected: `board-axis tests PASS`.

- [ ] **Step 6: Confirm the count is unchanged at 81**

```bash
cd ~/Development/rt1170/evkb && ./tools/run-all-qemu-gates.sh -l | tail -1
```

Expected: `(81 gate(s))` — no example has a `boards` sidecar yet, so every gate
is `rt1176:` and the count must not move.

- [ ] **Step 7: Full sweep**

```bash
uptime && cd ~/Development/rt1170/evkb && ./tools/run-all-qemu-gates.sh -j 2
```

Expected: same as Task 3 Step 4 — `81/0/0` or `80/1/0` with only
`dualcore/cm4_audio_test` red. Ids now read `rt1176:<category>/<name>`.

- [ ] **Step 8: Commit**

```bash
cd ~/Development/rt1170/evkb
git add tools/run-all-qemu-gates.sh tools/board-axis.test.sh
git commit -m "run-all-qemu-gates: a board dimension, defaulting to rt1176

Gate ids become <board>:<category>/<name> and each gate runs once per board its
example declares in a `boards` sidecar. No example declares one yet, so the
count stays at 81 and every id is rt1176: -- the default is what keeps this
change inert.

The SKIP probe learned the per-board build directory, so the zero-SKIP signal
survives: a declared board with no built ELF is still a SKIP and still says so
by name, with the exact cmake line to fix it.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 5: `EVKB_BOARD` in `evkb.cmake`

**Files:**
- Modify: `evkb.cmake:53-54` (manifest), `evkb.cmake:287-295` (core import)

- [ ] **Step 1: Add the board variable and make the cores entry derive from it**

Replace line 53–54 of `evkb.cmake` with:

```cmake
set(_dev "$ENV{HOME}/Development")

# --- board axis --------------------------------------------------------------
# Which board this build targets. Drives the cores subdirectory below and the
# QEMU machine in tools/gate-lib.sh. Default rt1176 so every existing example
# builds exactly as before without being edited.
set(EVKB_BOARD "rt1176" CACHE STRING "Target board: rt1176 (MIMXRT1170-EVKB) or rt1062 (MIMXRT1060-EVKB)")
set_property(CACHE EVKB_BOARD PROPERTY STRINGS rt1176 rt1062)
if(EVKB_BOARD STREQUAL "rt1176")
    set(EVKB_CORE_SUBDIR imxrt1176)
elseif(EVKB_BOARD STREQUAL "rt1062")
    set(EVKB_CORE_SUBDIR teensy4)
else()
    message(FATAL_ERROR "EVKB_BOARD must be rt1176 or rt1062, got '${EVKB_BOARD}'")
endif()

_evkb_lib(cores          ${EVKB_ROOT}/cores/${EVKB_CORE_SUBDIR} https://github.com/newdigate/teensy-cores    4d871629d0de2421c7482d6c0e8efa3f38da8a26 ${EVKB_CORE_SUBDIR})
```

- [ ] **Step 2: Make the COREPATH comment honest**

Replace lines 287–294's comment block and `set(COREPATH ...)` with:

```cmake
# --- the core (every example needs it) ---------------------------------------
evkb_library_dir(cores EVKB_CORES_DIR)
# The toolchain file guessed COREPATH from its own location; re-point it at the
# resolved core so a fresh clone (fetched core) links the right linker script.
# MUST happen BEFORE the first import_arduino_library call — that call runs
# teensy_set_dynamic_properties once, baking COREPATH into the link flags.
# Trailing slash required: the macros build LINKER_FILE as
# "${COREPATH}imxrt1176.ld" (117) or "${COREPATH}imxrt1060_evkb.ld" (42).
set(COREPATH "${EVKB_CORES_DIR}/" CACHE STRING "resolved core path" FORCE)
```

- [ ] **Step 3: Rebuild the sampled examples and prove byte-identity**

```bash
cd ~/Development/rt1170/evkb
for d in examples/serial/serial_test examples/framework/string_test \
         examples/gpio-analog/dac_test examples/dualcore/cm4_audiostream_test \
         examples/usb/usb_audio_uac1_test; do
  ( cd "$d" && cmake --build build >/dev/null ) || echo "BUILD FAILED: $d"
done
shasum -a256 -c /tmp/rt1060-axis-baseline.txt
```

Expected: five `OK` lines. **A `FAILED` line means this refactor changed
codegen and must be fixed, not accepted** — the whole point of defaulting
`EVKB_BOARD` to `rt1176` is that these images cannot move.

- [ ] **Step 4: Commit**

```bash
cd ~/Development/rt1170/evkb
git add evkb.cmake
git commit -m "evkb.cmake: EVKB_BOARD selects the core, defaulting to rt1176

Two functionally board-specific lines become one variable. rt1176 -> the
imxrt1176 core, rt1062 -> teensy4 (which is RT1062 silicon and already carries
imxrt1060_evkb.ld); an unrecognised value is a hard error rather than a
mysterious link failure against the wrong linker script.

Proven codegen-neutral by the 2B cmp discipline: five sampled images across
serial, framework, gpio-analog, dualcore (CM4 image) and usb rebuild
byte-identical.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 6: An rt1062 toolchain file for `serial_test`

**Files:**
- Create: `examples/serial/serial_test/toolchain/rt1062-evkb.toolchain.cmake`

- [ ] **Step 1: Create the toolchain file**

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

- [ ] **Step 2: Relax the CMakeLists' hardcoded fallback**

`examples/serial/serial_test/CMakeLists.txt:7` unconditionally caches
`TEENSY_VERSION 117`, which would fight the rt1062 toolchain. Replace line 7
with:

```cmake
# Fallback only: the toolchain file sets this FORCE-fully, so a toolchain-driven
# build wins. A bare `cmake -B build .` with no toolchain still selects 117.
if(NOT DEFINED TEENSY_VERSION)
    set(TEENSY_VERSION 117 CACHE STRING "")
endif()
```

- [ ] **Step 3: Verify the rt1176 build is still byte-identical**

```bash
cd ~/Development/rt1170/evkb/examples/serial/serial_test && cmake --build build >/dev/null
shasum -a256 -c /tmp/rt1060-axis-baseline.txt 2>&1 | grep serial_test
```

Expected: `examples/serial/serial_test/build/serial_test.elf: OK`

- [ ] **Step 4: Build for rt1062**

```bash
cd ~/Development/rt1170/evkb/examples/serial/serial_test
cmake -B build-rt1062 -DEVKB_BOARD=rt1062 \
      -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1062-evkb.toolchain.cmake
cmake --build build-rt1062
```

Expected: `build-rt1062/serial_test.elf` produced. If the link fails on a
missing `imxrt1060_evkb.ld`, the `cores/teensy4/` checkout is stale — confirm
with `ls cores/teensy4/imxrt1060_evkb.ld`.

- [ ] **Step 5: Commit**

```bash
cd ~/Development/rt1170/evkb
git add examples/serial/serial_test/toolchain/rt1062-evkb.toolchain.cmake \
        examples/serial/serial_test/CMakeLists.txt
git commit -m "serial_test: build for the MIMXRT1060-EVKB as well

TEENSY_VERSION 42 already maps to the teensy4 core and imxrt1060_evkb.ld in
teensy-cmake-macros, so this needed a toolchain file and nothing else.

The CMakeLists' TEENSY_VERSION fallback is now guarded: it was caching 117
unconditionally, which would have silently overridden the rt1062 toolchain and
built an RT1176 image into build-rt1062/ -- an image that would boot the wrong
QEMU machine and fail in a way that looks like a board problem.

rt1176 image verified byte-identical.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 7: `serial_test` gate green on both boards

**Files:**
- Modify: `examples/serial/serial_test/serial_test.cpp:7`
- Modify: `examples/serial/serial_test/run_qemu.sh` (banner assertion)
- Create: `examples/serial/serial_test/boards`

- [ ] **Step 1: Make the banner name the chip it was actually built for**

Replace line 7 of `serial_test.cpp`:

```cpp
#if defined(__IMXRT1176__)
    Serial1.println("RT1176 Serial1 up");
#elif defined(__IMXRT1062__)
    Serial1.println("RT1062 Serial1 up");
#else
#error "unknown target: expected __IMXRT1176__ or __IMXRT1062__"
#endif
```

The `#error` is deliberate. A third board added later must fail at compile time
rather than print a banner naming the wrong chip — which the gate would then
assert against and pass.

- [ ] **Step 2: Make the gate assert the board-appropriate banner**

Replace the banner assertion in `run_qemu.sh` (currently
`grep -q "RT1176 Serial1 up" ...`) with:

```sh
case "$(gate_board)" in
    rt1176) BANNER="RT1176 Serial1 up" ;;
    rt1062) BANNER="RT1062 Serial1 up" ;;
esac
grep -q "$BANNER" "$OUT" || { echo "FAIL: banner missing (expected '$BANNER')"; exit 1; }
```

Asserting the board-specific banner rather than a neutral substring is the
point: it catches an image built for the wrong board, which is the most likely
mistake this whole phase introduces.

- [ ] **Step 3: Declare the boards**

Create `examples/serial/serial_test/boards`:

```
# Boards this example is built and gated for. See
# docs/superpowers/specs/2026-08-07-rt1060-board-axis-design.md
rt1176
rt1062
```

- [ ] **Step 4: Rebuild both images**

```bash
cd ~/Development/rt1170/evkb/examples/serial/serial_test
cmake --build build && cmake --build build-rt1062
```

Expected: both succeed. The rt1176 image **changes** here — that is intended
and is the first deliberate codegen change in this plan. Re-baseline it:

```bash
cd ~/Development/rt1170/evkb
shasum -a256 examples/serial/serial_test/build/serial_test.elf
```

- [ ] **Step 5: Run the gate on both boards**

```bash
cd ~/Development/rt1170/evkb/examples/serial/serial_test
EVKB_BOARD=rt1176 ./run_qemu.sh && EVKB_BOARD=rt1062 ./run_qemu.sh
```

Expected: two `PASS: QEMU serial output verified`, the first showing
`RT1176 Serial1 up`, the second `RT1062 Serial1 up`. **If the rt1062 run shows
the RT1176 banner, the toolchain was ignored** — recheck Task 6 Step 2.

- [ ] **Step 6: Confirm the sweep now counts 82**

```bash
cd ~/Development/rt1170/evkb && ./tools/run-all-qemu-gates.sh -l | tail -1
```

Expected: `(82 gate(s))` — `serial/serial_test` now contributes two.

- [ ] **Step 7: Commit**

```bash
cd ~/Development/rt1170/evkb
git add examples/serial/serial_test
git commit -m "serial_test: gated on the MIMXRT1060-EVKB as well as the 1170

The first example to declare two boards. Sweep 81 -> 82: the same gate script
runs twice, once per board, with the machine coming from gate-lib.

The banner now names the chip it was compiled for, and the gate asserts the
board-appropriate one rather than a neutral substring -- an image built for the
wrong board is the most likely mistake this phase introduces, and a neutral
assertion would pass straight through it. A future third board hits an #error
instead of printing a banner naming the wrong chip.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 8: Close the phase — audit, sweep, docs

**Files:**
- Modify: `tools/license-audit.sh` (GATES list)
- Modify: `CLAUDE.md` (gate count + board axis)
- Modify: `docs/KNOWN-BROKEN-GATES.md` (gate count move)

- [ ] **Step 1: Add the rt1062 build to the audit's GATES list**

In `tools/license-audit.sh`, find the line
`examples/serial/serial_test:serial_test \` in the GATES list and add beneath
it:

```
examples/serial/serial_test/build-rt1062:serial_test \
```

- [ ] **Step 2: Run the audit**

```bash
cd ~/Development/rt1170/evkb && ./tools/license-audit.sh 2>&1 | tail -3
```

Expected: `LICENSE-AUDIT: PASS`. Capture the full output, not a tail, if you
need to confirm coverage — a truncated log cannot tell you what was walked.

- [ ] **Step 3: Full sweep on an idle machine**

```bash
uptime && cd ~/Development/rt1170/evkb && ./tools/run-all-qemu-gates.sh -j 2
```

Expected: `82 passed, 0 failed, 0 SKIP`, or `81 passed, 1 failed, 0 SKIP` with
only `rt1176:dualcore/cm4_audio_test` red. Zero SKIP either way.

- [ ] **Step 4: Update `CLAUDE.md`**

In the "Test / verify — the two-gate rule" section, change the sweep count from
81 to 82 and add after the gate-count paragraph:

```markdown
**The tree is multi-board.** `EVKB_BOARD` selects `rt1176` (MIMXRT1170-EVKB,
the default) or `rt1062` (MIMXRT1060-EVKB). An example declares the boards it
supports in a `boards` sidecar file; absent means `rt1176` only, which is why
most examples have none. Gate ids are `<board>:<category>/<name>` and no gate
names a QEMU machine — `tools/gate-lib.sh` derives `-M`, `-global` and the
build directory from the board. Build a non-default board with
`cmake -B build-rt1062 -DEVKB_BOARD=rt1062 -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1062-evkb.toolchain.cmake`.
```

- [ ] **Step 5: Update `docs/KNOWN-BROKEN-GATES.md`**

Append to the "Current expected sweep result" section:

```markdown
**2026-08-07 (the board axis):** `serial/serial_test` becomes the first example
gated on two boards, so the sweep moves **81 → 82** without a new example: the
same gate script runs once for `rt1176` and once for `rt1062`
(MIMXRT1060-EVKB). Gate ids now carry a board prefix.

Expectation is `82/0/0` or `81/1/0` with the documented
`rt1176:dualcore/cm4_audio_test` singleton, zero SKIP either way.

Two notes on reading this sweep. **A red on `rt1062:` and green on `rt1176:`
for the same example is not a flake** — it is the board axis doing its job, and
it means the 1060 build genuinely differs. **The SKIP signal is now per board**:
an example that declares `rt1062` but has no `build-rt1062/*.elf` is a SKIP,
and the runner prints the exact `cmake` line to fix it.
```

- [ ] **Step 6: Commit**

```bash
cd ~/Development/rt1170/evkb
git add tools/license-audit.sh CLAUDE.md docs/KNOWN-BROKEN-GATES.md
git commit -m "docs: the tree is multi-board; sweep 81 -> 82

Phase 1 of the RT1060 board axis is complete. The count moved without a new
example: serial_test is gated on both boards, so the same script runs twice.

The licence audit walks the rt1062 build too -- an unaudited second build
directory would be exactly the kind of silent coverage gap the GATES drift
check exists to catch.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Definition of done

- [ ] `./tools/run-all-qemu-gates.sh -l | tail -1` reports `(82 gate(s))`
- [ ] Full sweep: `82/0/0` or `81/1/0` with only `rt1176:dualcore/cm4_audio_test`, **zero SKIP**
- [ ] `rt1062:serial/serial_test` PASSES, printing `RT1062 Serial1 up`
- [ ] Every sampled rt1176 image byte-identical through Task 5 (Task 7's serial_test change is the one intended exception)
- [ ] `./tools/license-audit.sh` → `LICENSE-AUDIT: PASS`
- [ ] No `run_qemu*.sh` contains the string `mimxrt1170-evk`

## Not in this phase

No USB work, no qemu2 change, no silicon run. Phase 1 is the axis and one
canary example. The RT1060's USB host port (**J47**, not J48) is Phase 3.
