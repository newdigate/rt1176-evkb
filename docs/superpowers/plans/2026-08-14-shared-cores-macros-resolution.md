# Shared cores/macros resolution — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Dissolve the two in-repo nested checkouts (`cores/`, `teensy-cmake-macros/`) into ordinary sibling checkouts under `TEENSY_LIB_ROOT` (default `$HOME/Development`), resolved local-first with pinned-git fallback — per `docs/superpowers/specs/2026-08-14-shared-cores-macros-resolution-design.md`.

**Architecture:** Stage 1 adds a generic declared-library layer to `teensy-cmake-macros` and re-homes its unpinned `COREPATH` fallback onto it (pinned, local-first). Stage 2 rewrites `evkb.cmake` onto that layer. Stage 3 deletes the in-repo copies, strips all 106 `COREPATH` setter sites, and re-points the licence audit. Primary oracle: byte-identical `.hex` before/after; final gate: the 89/0/0 QEMU sweep.

**Tech Stack:** CMake ≥3.24, FetchContent + CPM 0.42.3, ARM GCC 10 (`/Applications/ARM_10/bin/`), the repo's gate/audit shell harness.

**Two repos are edited:** `~/Development/rt1170/evkb` (this repo) and `~/Development/teensy-cmake-macros` (sibling; its own git). The in-repo `evkb/teensy-cmake-macros` copy is NOT edited — after Task 5 it is dead code, deleted in Task 7.

**Do NOT execute in a git worktree.** The work edits a second repo outside this one, compares build artifacts in place, and deletes untracked nested repos a worktree would not even contain. Run in the main checkouts. Avoid running other builds/flashing concurrently (dual-core gates are load-sensitive — CLAUDE.md).

**Scratch area** (session scratchpad; create once):
`SCRATCH=/private/tmp/claude-501/-Users-nicholasnewdigate-Development-rt1170-evkb/2f964c6a-5f66-4b74-b61e-f17a47e6b202/scratchpad/shared-cores`

---

### Task 0: Preconditions + baseline oracle hexes (no edits)

**Files:** none modified. Outputs go to `$SCRATCH/oracle/pre/`.

- [ ] **Step 0.1: Verify preconditions**

```bash
cd ~/Development/rt1170/evkb
git -C cores status --porcelain            # expect: empty
git -C teensy-cmake-macros status --porcelain   # expect: empty
git -C cores rev-parse HEAD                # expect: 5bcae781b6c0e451f073298ddf7e1cd859f3e4de
git -C teensy-cmake-macros rev-parse HEAD  # expect: e948da4d43cf76e3a0d8813cd85e6da314a0a569
git -C ~/Development/teensy-cores cat-file -e 5bcae781b6c0e451f073298ddf7e1cd859f3e4de && echo cores-twin-ok
git -C ~/Development/teensy-cmake-macros cat-file -e e948da4d43cf76e3a0d8813cd85e6da314a0a569 && echo macros-twin-ok
git -C ~/Development/teensy-cmake-macros status --porcelain   # expect: empty (Stage 1 starts from clean)
ls /Applications/ARM_10/bin/arm-none-eabi-gcc
```

Expected: both porcelains empty, both SHAs as shown, both `*-twin-ok`, compiler present. **Any mismatch: STOP and report** — the migration preconditions in the spec are not met.

- [ ] **Step 0.2: Build the 6 oracle examples at baseline (blink twice — determinism control)**

The oracle set (spec §6): `blink` (core-only), `serial/serial_test` on BOTH boards (two-board example), `dualcore/cm4_boot_test` (CM4 image embedded in CM7 ELF), `storage-memory/sd_test` + `audio/sd_wav_play_test` (the 2 inline-toolchain examples, peripheral libs).

```bash
cd ~/Development/rt1170/evkb
mkdir -p $SCRATCH/oracle/pre

b() { # <dir> <builddir> <extra cmake args...>
  d=$1; bd=$2; shift 2
  (cd "$d" && rm -rf "$bd" && cmake -B "$bd" "$@" >/dev/null && cmake --build "$bd" --parallel 8 >/dev/null) || { echo "BUILD FAIL: $d"; return 1; }
}
b examples/gpio-analog/blink build-pre  -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake
b examples/gpio-analog/blink build-pre2 -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake
b examples/serial/serial_test build-pre -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake
b examples/serial/serial_test build-pre-rt1062 -DEVKB_BOARD=rt1062 -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1062-evkb.toolchain.cmake
b examples/dualcore/cm4_boot_test build-pre -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake
b examples/storage-memory/sd_test build-pre
b examples/audio/sd_wav_play_test build-pre

cmp examples/gpio-analog/blink/build-pre/blink.hex examples/gpio-analog/blink/build-pre2/blink.hex && echo DETERMINISTIC
cp examples/gpio-analog/blink/build-pre/blink.hex                       $SCRATCH/oracle/pre/blink.hex
cp examples/serial/serial_test/build-pre/serial_test.hex                $SCRATCH/oracle/pre/serial_test.hex
cp examples/serial/serial_test/build-pre-rt1062/serial_test.hex         $SCRATCH/oracle/pre/serial_test-rt1062.hex
cp examples/dualcore/cm4_boot_test/build-pre/cm4_boot_test.hex          $SCRATCH/oracle/pre/cm4_boot_test.hex
cp examples/storage-memory/sd_test/build-pre/sd_test.hex                $SCRATCH/oracle/pre/sd_test.hex
cp examples/audio/sd_wav_play_test/build-pre/sd_wav_play_test.hex       $SCRATCH/oracle/pre/sd_wav_play_test.hex
```

Expected: all builds succeed, `DETERMINISTIC` printed. If `cmp` fails (blink not self-identical), the hex oracle is void: record the fact in `$SCRATCH/notes.md`, continue — the sweep in Task 9 is then the sole oracle. If a `.hex` name differs, `ls` the build dir for the actual name.

- [ ] **Step 0.3: Clean up the `build-pre*` dirs? NO** — keep them until Task 9's comparison, then delete.

---

### Task 1: Baseline the 5 existing macros tests (no edits)

**Files:** none modified. Repo: `~/Development/teensy-cmake-macros`.

The tests hardcode a Linux GCC-9 path, so local runs need a wrapper toolchain. They also fetch the macros from GitHub `main` — override with `FETCHCONTENT_SOURCE_DIR_TEENSY_CMAKE_MACROS` so they test the LOCAL tree (without this, every "test run" in this plan would be vacuous).

- [ ] **Step 1.1: Write the local wrapper toolchain (scratch, not committed)**

Write `$SCRATCH/teensy41-local.toolchain.cmake`:

```cmake
# Local wrapper: the committed test toolchain hardcodes the CI's Linux GCC-9
# path. Include it, then re-point the compiler at this machine's ARM GCC 10.
include(${CMAKE_CURRENT_LIST_DIR}/../REPLACED_AT_USE/tests/teensy41.toolchain.cmake)
set(COMPILERPATH "/Applications/ARM_10/bin/")
set(CMAKE_C_COMPILER ${COMPILERPATH}arm-none-eabi-gcc)
set(CMAKE_CXX_COMPILER ${COMPILERPATH}arm-none-eabi-g++)
set(CMAKE_CXX_LINK_EXECUTABLE "${CMAKE_C_COMPILER} <FLAGS> <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")
```

The include path must be absolute at use — replace the first line with:
`include($ENV{HOME}/Development/teensy-cmake-macros/tests/teensy41.toolchain.cmake)`
(write it that way directly; the `REPLACED_AT_USE` form above is just to show why).

- [ ] **Step 1.2: Run all 5 tests at baseline, record results**

```bash
cd ~/Development/teensy-cmake-macros
for t in basic spi eeprom vector audio; do
  rm -rf tests/$t/build-base
  (cd tests/$t && cmake -B build-base -Wno-dev \
      -DCMAKE_TOOLCHAIN_FILE=$SCRATCH/teensy41-local.toolchain.cmake \
      -DFETCHCONTENT_SOURCE_DIR_TEENSY_CMAKE_MACROS=$HOME/Development/teensy-cmake-macros \
      >/dev/null 2> build-base/configure.err && cmake --build build-base >/dev/null 2>&1) \
    && echo "BASELINE $t: PASS" || echo "BASELINE $t: FAIL"
done
grep -l "teensy_cores" tests/basic/build-base/_deps 2>/dev/null; ls tests/basic/build-base/_deps/ | head
```

Expected: 5 lines `BASELINE <t>: PASS|FAIL`. Record all five verdicts in `$SCRATCH/notes.md`. A baseline FAIL is pre-existing breakage — the bar for this plan is **no test that passed at baseline may fail after**. Note `_deps` listing: today it contains `teensy_cores-*` (the unpinned master fetch) — that is the behaviour Task 3 replaces.

---

### Task 2: Macros — `TEENSY_LIB_ROOT` + declare/resolve/import trio (TDD)

**Files:**
- Create: `~/Development/teensy-cmake-macros/tests/resolve/CMakeLists.txt`
- Modify: `~/Development/teensy-cmake-macros/CMakeLists.include.txt` (two insertions)

- [ ] **Step 2.1: Write the failing test**

Create `tests/resolve/CMakeLists.txt`:

```cmake
# tests/resolve — configure-only test of the declared-library API. No
# toolchain, no compiler, no network: the declared URL is unreachable on
# purpose, so if resolution ever reaches for it the configure fails.
#
#   cmake -B build                                → RESOLVE-TEST: PASS
#   cmake -B build-neg -DRESOLVE_TEST_UNDECLARED=ON   → must FAIL, "not declared"
cmake_minimum_required(VERSION 3.24)
project(resolve_test NONE)

set(TEENSY_LIB_ROOT "${CMAKE_BINARY_DIR}/fake-root")   # pre-set: include must respect it
set(COREPATH "${CMAKE_BINARY_DIR}/no-core/")           # suppress the core fallback
include(${CMAKE_CURRENT_LIST_DIR}/../../CMakeLists.include.txt)

file(MAKE_DIRECTORY "${TEENSY_LIB_ROOT}/FakeLib")
teensy_declare_library(FakeLib FakeLib https://invalid.example/never-fetched deadbeef .)

if(RESOLVE_TEST_UNDECLARED)
    teensy_resolve_library(NeverDeclared _boom)        # must FATAL_ERROR
endif()

# local-first: the checkout under TEENSY_LIB_ROOT wins, URL never touched
teensy_resolve_library(FakeLib _first)
if(NOT _first STREQUAL "${TEENSY_LIB_ROOT}/FakeLib")
    message(FATAL_ERROR "local-first: expected ${TEENSY_LIB_ROOT}/FakeLib, got '${_first}'")
endif()

# memoized: same answer twice
teensy_resolve_library(FakeLib _second)
if(NOT _second STREQUAL "${_first}")
    message(FATAL_ERROR "memoization: got '${_second}' after '${_first}'")
endif()

message(STATUS "RESOLVE-TEST: PASS")
```

- [ ] **Step 2.2: Run it — verify it fails for the right reason**

```bash
cd ~/Development/teensy-cmake-macros/tests/resolve && rm -rf build && cmake -B build 2>&1 | tail -3
```

Expected: FAIL with `Unknown CMake command "teensy_declare_library"`.

- [ ] **Step 2.3: Implement — insertion 1, `TEENSY_LIB_ROOT`**

In `CMakeLists.include.txt`, directly AFTER the `set(TEENSY_CMAKE_MACROS_DIR ...)` line (line 7), insert:

```cmake
# TEENSY_LIB_ROOT: the directory that holds sibling library checkouts (the
# local-first side of every declared library below, and of the COREPATH
# fallback at the end of this file). Respect a value the consumer already set
# (variable or -D), else the TEENSY_LIB_ROOT env var, else ~/Development.
if(NOT DEFINED TEENSY_LIB_ROOT)
    if(DEFINED ENV{TEENSY_LIB_ROOT})
        set(TEENSY_LIB_ROOT "$ENV{TEENSY_LIB_ROOT}" CACHE PATH "root for sibling Arduino library checkouts")
    else()
        set(TEENSY_LIB_ROOT "$ENV{HOME}/Development" CACHE PATH "root for sibling Arduino library checkouts")
    endif()
endif()
```

- [ ] **Step 2.4: Implement — insertion 2, the trio**

Directly AFTER `endmacro(resolve_arduino_library_auto)` (line 468), insert:

```cmake
# --- declared-library manifest layer -----------------------------------------
# teensy_declare_library(NAME SUBDIR URL REF PATH): register NAME so it can be
# resolved by name alone. SUBDIR is relative to TEENSY_LIB_ROOT (the local
# checkout, sub-path included, e.g. "FNET/src"); PATH is the sub-path inside
# the fetched repo ("." for the repo root).
macro(teensy_declare_library LIB_NAME LIB_SUBDIR LIB_URL LIB_REF LIB_PATH)
    set(TEENSY_LIB_${LIB_NAME}_SUBDIR "${LIB_SUBDIR}")
    set(TEENSY_LIB_${LIB_NAME}_URL    "${LIB_URL}")
    set(TEENSY_LIB_${LIB_NAME}_REF    "${LIB_REF}")
    set(TEENSY_LIB_${LIB_NAME}_PATH   "${LIB_PATH}")
endmacro()

# teensy_resolve_library(NAME OUT_VAR): resolve a declared library's source
# directory — local-first under TEENSY_LIB_ROOT, else CPM at the pinned REF
# (DOWNLOAD_ONLY, shared through CPM_SOURCE_CACHE). Memoized per configure.
# TEENSY_FORCE_FETCH=ON skips the local checkout (fresh-user simulation).
macro(teensy_resolve_library LIB_NAME OUT_VAR)
    if(NOT DEFINED TEENSY_LIB_${LIB_NAME}_URL)
        message(FATAL_ERROR "teensy_resolve_library(${LIB_NAME}): not declared via teensy_declare_library")
    endif()
    if(NOT DEFINED _teensy_resolved_${LIB_NAME})
        if(TEENSY_FORCE_FETCH)
            set(_teensy_local "${TEENSY_LIB_ROOT}/.force-fetch-no-local")
        else()
            set(_teensy_local "${TEENSY_LIB_ROOT}/${TEENSY_LIB_${LIB_NAME}_SUBDIR}")
        endif()
        resolve_arduino_library_auto(${LIB_NAME} "${_teensy_local}"
            "${TEENSY_LIB_${LIB_NAME}_URL}" "${TEENSY_LIB_${LIB_NAME}_REF}"
            "${TEENSY_LIB_${LIB_NAME}_PATH}" _teensy_resolved_${LIB_NAME})
    endif()
    set(${OUT_VAR} "${_teensy_resolved_${LIB_NAME}}")
endmacro()

# teensy_import_library(NAME [subdirs...]): resolve + import_arduino_library.
macro(teensy_import_library LIB_NAME)
    teensy_resolve_library(${LIB_NAME} _teensy_import_dir)
    import_arduino_library(${LIB_NAME} "${_teensy_import_dir}" ${ARGN})
endmacro()
```

- [ ] **Step 2.5: Run both test invocations**

```bash
cd ~/Development/teensy-cmake-macros/tests/resolve
rm -rf build build-neg
cmake -B build 2>&1 | grep "RESOLVE-TEST"
cmake -B build-neg -DRESOLVE_TEST_UNDECLARED=ON > $SCRATCH/neg.log 2>&1; echo "cmake-exit=$?"
grep -c "not declared" $SCRATCH/neg.log
```

Expected: `-- RESOLVE-TEST: PASS`; then `cmake-exit=1` (configure FAILED) and grep prints `1` (failed with the right message). (Force-fetch's CPM path is deliberately NOT covered here — it needs network; Task 11's `EVKB_FORCE_FETCH=ON` run covers it end-to-end, as recorded in the spec.)

- [ ] **Step 2.6: Commit (macros repo)**

```bash
cd ~/Development/teensy-cmake-macros
git add CMakeLists.include.txt tests/resolve/CMakeLists.txt
git commit -m "feat: declared-library layer — TEENSY_LIB_ROOT, declare/resolve/import

Local-first under a configurable sibling root, pinned CPM fallback,
memoized, TEENSY_FORCE_FETCH for fresh-user simulation. Configure-only
test in tests/resolve (unreachable URL proves local never fetches).

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 3: Macros — pinned, local-first `COREPATH` fallback (replaces unpinned master)

**Files:**
- Create: `~/Development/teensy-cmake-macros/tests/corepath-fallback/CMakeLists.txt`
- Modify: `~/Development/teensy-cmake-macros/CMakeLists.include.txt` (delete lines 8–21 block, append replacement at end)

- [ ] **Step 3.1: Safety check — COREPATH must have no top-level readers**

The fallback moves from line 13 to the file's end; that is only safe if nothing between reads `COREPATH` at include time. Verified during planning (readers are inside `teensy_set_dynamic_properties` and `import_teensy_cores` only); re-verify:

```bash
cd ~/Development/teensy-cmake-macros
awk '/^(function|macro)\(/{d++} /^(endfunction|endmacro)/{d--} /COREPATH/{if(d==0) print NR": "$0}' CMakeLists.include.txt
```

Expected: every printed line number falls within 8–21 (the old fallback block and its comment). Any hit OUTSIDE that range: STOP, reassess placement.

- [ ] **Step 3.2: Write the test**

Create `tests/corepath-fallback/CMakeLists.txt`:

```cmake
# tests/corepath-fallback — include the macros WITHOUT COREPATH and assert the
# fallback resolved a real teensy4 core. Local-first: with a teensy-cores
# checkout under TEENSY_LIB_ROOT this must resolve locally (instant, no clone).
#   cmake -B build    → COREPATH-FALLBACK-TEST: PASS
cmake_minimum_required(VERSION 3.24)
project(corepath_fallback_test NONE)
include(${CMAKE_CURRENT_LIST_DIR}/../../CMakeLists.include.txt)
if(NOT COREPATH MATCHES "/teensy4/$")
    message(FATAL_ERROR "fallback COREPATH is not a teensy4 core: '${COREPATH}'")
endif()
if(NOT EXISTS "${COREPATH}core_pins.h")
    message(FATAL_ERROR "fallback COREPATH has no core_pins.h: '${COREPATH}'")
endif()
if(NOT DEFINED teensy_cores_SOURCE_DIR)
    message(FATAL_ERROR "teensy_cores_SOURCE_DIR not exported (tests/basic consumes it)")
endif()
message(STATUS "COREPATH-FALLBACK-TEST: PASS (${COREPATH})")
```

- [ ] **Step 3.3: Run it against the OLD code — observe today's behaviour**

```bash
cd ~/Development/teensy-cmake-macros/tests/corepath-fallback
rm -rf build && time cmake -B build 2>&1 | grep -E "COREPATH-FALLBACK|Cloning|master" | head
ls build/_deps/ 2>/dev/null | head -3
```

Expected: PASS, but slowly — `_deps/teensy_cores-src` appears (a fresh clone of master). This is the behaviour being replaced; note the time.

- [ ] **Step 3.4: Implement — delete the old block, append the new fallback**

Delete lines 8–21 of `CMakeLists.include.txt` (the comment block + `if (NOT DEFINED COREPATH) ... endif()` that fetches `teensy-cores` at master). At that spot leave only:

```cmake
# COREPATH may be supplied by the consumer (e.g. the RT1170-EVKB tree points it
# at its own resolved core). When the consumer has not set it, the fallback at
# the END of this file resolves teensy-cores through the declared-library layer
# (local-first under TEENSY_LIB_ROOT, else CPM at a pinned ref) — it lives at
# the end because it calls macros defined below.
```

Append at the very END of `CMakeLists.include.txt`:

```cmake
# --- COREPATH fallback (see the note near the top of this file) --------------
# Pinned: an unpinned `master` fetch here cost every non-consumer-configured
# build a fresh clone per build dir and made test results time-dependent.
if (NOT DEFINED COREPATH)
    teensy_declare_library(teensy_cores teensy-cores
        https://github.com/newdigate/teensy-cores
        5bcae781b6c0e451f073298ddf7e1cd859f3e4de .)
    teensy_resolve_library(teensy_cores _teensy_cores_root)
    # Back-compat: consumers (tests/basic among them) read this variable, which
    # FetchContent_MakeAvailable used to export.
    set(teensy_cores_SOURCE_DIR "${_teensy_cores_root}")
    set(COREPATH "${teensy_cores_SOURCE_DIR}/teensy4/")
endif()
```

- [ ] **Step 3.5: Run the test against the NEW code**

```bash
cd ~/Development/teensy-cmake-macros/tests/corepath-fallback
rm -rf build && time cmake -B build 2>&1 | grep -E "COREPATH-FALLBACK|resolve_arduino"
ls build/_deps/ 2>/dev/null | wc -l
```

Expected: `resolve_arduino_library_auto(teensy_cores): local /Users/nicholasnewdigate/Development/teensy-cores`, then `COREPATH-FALLBACK-TEST: PASS (/Users/nicholasnewdigate/Development/teensy-cores/teensy4/)`. Near-instant, `_deps` count 0 (no clone).

- [ ] **Step 3.6: Regression — tests/resolve still passes; one compile test end-to-end**

```bash
cd ~/Development/teensy-cmake-macros/tests/resolve && rm -rf build && cmake -B build 2>&1 | grep "RESOLVE-TEST: PASS"
cd ../basic && rm -rf build-new
cmake -B build-new -Wno-dev -DCMAKE_TOOLCHAIN_FILE=$SCRATCH/teensy41-local.toolchain.cmake \
  -DFETCHCONTENT_SOURCE_DIR_TEENSY_CMAKE_MACROS=$HOME/Development/teensy-cmake-macros >/dev/null \
  && cmake --build build-new >/dev/null && echo "BASIC: PASS"
```

Expected: `RESOLVE-TEST: PASS` and `BASIC: PASS` — tests/basic now compiles the LOCAL `~/Development/teensy-cores/teensy4` core (via the new fallback + `teensy_cores_SOURCE_DIR`), no network. If `BASIC` failed at baseline (Task 1), it must fail the same way, not a new way.

- [ ] **Step 3.7: Run the remaining tests (spi, eeprom, vector, audio) the same way**

Same invocation pattern as Step 3.6's basic run, `build-new` dirs. Expected: same verdict as their Task-1 baseline (PASS stays PASS).

- [ ] **Step 3.8: Commit (macros repo)**

```bash
cd ~/Development/teensy-cmake-macros
git add CMakeLists.include.txt tests/corepath-fallback/CMakeLists.txt
git commit -m "fix: COREPATH fallback — pinned + local-first, was unpinned master per build dir

Resolves teensy-cores through the declared-library layer: a checkout
under TEENSY_LIB_ROOT wins, else CPM at a pinned SHA. Exports
teensy_cores_SOURCE_DIR as before. Relocated to end-of-file (calls
macros defined above); COREPATH has no top-level readers in between.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 4: Macros — document the new layer

**Files:**
- Modify: `~/Development/teensy-cmake-macros/README.md` (the "Dependency caching" section)
- Modify: `~/Development/teensy-cmake-macros/CLAUDE.md` (consumer-libraries paragraph)

- [ ] **Step 4.1: README — insert before the "## Dependency caching" heading:**

```markdown
## Declared libraries (local-first, pinned fallback)

`teensy_declare_library(NAME <subdir-under-TEENSY_LIB_ROOT> URL REF <sub-path>)`
registers a library once; `teensy_import_library(NAME [subdirs...])` (or
`teensy_resolve_library(NAME OUT_VAR)` for cherry-picking) then resolves it
**local-first**: a checkout at `${TEENSY_LIB_ROOT}/<subdir>` wins — working
tree, uncommitted edits included — else the repo is fetched with CPM at the
pinned `REF`. `TEENSY_LIB_ROOT` defaults to `$HOME/Development` (env var or
CMake variable to override); `-DTEENSY_FORCE_FETCH=ON` ignores local checkouts
(fresh-user simulation). The `COREPATH` fallback resolves `teensy-cores` the
same way — pinned, not `master`.
```

- [ ] **Step 4.2: In the "Dependency caching" paragraph**, after the sentence ending "identical pins are downloaded only once.", add: `Declared libraries (teensy_declare_library) share the same cache.`

- [ ] **Step 4.3: CLAUDE.md** — in the consumer-firmware paragraph (line ~88), after the sentence about `import_arduino_library_git`/`import_arduino_library`, add: `Consumers with a stable library set should prefer teensy_declare_library + teensy_import_library — local-first under TEENSY_LIB_ROOT (default ~/Development), pinned CPM fallback.`

- [ ] **Step 4.4: Commit (macros repo)**

```bash
cd ~/Development/teensy-cmake-macros
git add README.md CLAUDE.md
git commit -m "docs: TEENSY_LIB_ROOT + declared-library API

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 5: evkb.cmake — rebuild on the declared-library layer

**Files:**
- Modify: `evkb.cmake` (three regions; middle helpers 120–301 untouched)

- [ ] **Step 5.1: Pre-check — nothing else consumes the symbols being removed**

```bash
cd ~/Development/rt1170/evkb
grep -rn "_evkb_lib(\|EVKB_LIB_" --include=CMakeLists.txt examples | grep -v build   # expect: empty
grep -rn "EVKB_ROOT" --include=CMakeLists.txt examples | grep -v build               # expect: empty
```

Expected: both empty (verified during planning; `_evkb_lib`/`EVKB_LIB_*`/`EVKB_ROOT` are internal to evkb.cmake).

- [ ] **Step 5.2: Replace the header comment (lines 1–19)** with:

```cmake
# evkb.cmake — one-line bootstrap for every example: include AFTER project().
#
#   include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)
#
# It (1) pulls in teensy-cmake-macros (a ${TEENSY_LIB_ROOT}/teensy-cmake-macros
# checkout if present, else GitHub at the pinned ref), (2) declares the pinned
# library manifest behind import_evkb_library()/evkb_library_dir(), (3) imports
# the board's core and points COREPATH at wherever it resolved to.
#
# Local-first: a developer's ${TEENSY_LIB_ROOT}/<lib> checkout always wins
# (working tree, uncommitted edits included) — TEENSY_LIB_ROOT defaults to
# ~/Development (env var to override). A fresh clone with no sibling checkouts
# fetches everything from GitHub at the pinned refs below. Set the
# CPM_SOURCE_CACHE env var (e.g. ~/.cache/CPM) to clone each repo once across
# all build dirs — covers every declared library INCLUDING the core; the one
# exception is teensy-cmake-macros itself, fetched with plain FetchContent
# (456K; deliberately not CPM, so the single CPM pin stays in the macros —
# see docs/superpowers/specs/2026-08-14-shared-cores-macros-resolution-design.md).
#
# -DEVKB_FORCE_FETCH=ON ignores every local checkout — the "fresh user"
# simulation used to test this file.
#
# Updating a pin: push the library, paste its new SHA here, commit.
```

- [ ] **Step 5.3: Replace the bootstrap + manifest + helpers (current lines 21–118)**

Replace everything from `if(DEFINED EVKB_CMAKE_INCLUDED)` through `endmacro()` of `import_evkb_library` (inclusive) with:

```cmake
if(DEFINED EVKB_CMAKE_INCLUDED)
    return()
endif()
set(EVKB_CMAKE_INCLUDED 1)

set(EVKB_ROOT ${CMAKE_CURRENT_LIST_DIR})
option(EVKB_FORCE_FETCH "Ignore local sibling checkouts; fetch at the pinned refs" OFF)

# --- board axis (before the macros: the COREPATH pre-set needs the subdir) ---
# Which board this build targets. Drives the core subdir below and the QEMU
# machine in tools/gate-lib.sh. Default rt1176 so every existing example
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

# --- TEENSY_LIB_ROOT: where sibling checkouts live ---------------------------
# Set BEFORE the macros load so both computations agree by construction.
if(NOT DEFINED TEENSY_LIB_ROOT)
    if(DEFINED ENV{TEENSY_LIB_ROOT})
        set(TEENSY_LIB_ROOT "$ENV{TEENSY_LIB_ROOT}" CACHE PATH "root for sibling library checkouts")
    else()
        set(TEENSY_LIB_ROOT "$ENV{HOME}/Development" CACHE PATH "root for sibling library checkouts")
    endif()
endif()
set(TEENSY_FORCE_FETCH ${EVKB_FORCE_FETCH})   # the generic flag the resolver honors

# --- COREPATH pre-set: suppress the macros' own core resolution --------------
# The macros resolve teensy-cores themselves when COREPATH is undefined; evkb
# resolves the core through its own manifest (pinned below) instead. Economy,
# not safety: the real value is FORCEd after resolution, before the first
# import bakes it into link flags.
if(EXISTS "${TEENSY_LIB_ROOT}/teensy-cores/${EVKB_CORE_SUBDIR}" AND NOT EVKB_FORCE_FETCH)
    set(COREPATH "${TEENSY_LIB_ROOT}/teensy-cores/${EVKB_CORE_SUBDIR}/")
else()
    set(COREPATH "${CMAKE_BINARY_DIR}/evkb-corepath-pending/")   # placeholder; FORCEd below
endif()

# --- teensy-cmake-macros (the build system itself) ---------------------------
# Plain FetchContent, deliberately NOT CPM: the macros own the single CPM pin,
# and this bootstrap must not duplicate it (dual-pin drift — see the spec).
include(FetchContent)
if(EXISTS ${TEENSY_LIB_ROOT}/teensy-cmake-macros/CMakeLists.include.txt AND NOT EVKB_FORCE_FETCH)
    message(STATUS "teensy-cmake-macros: local ${TEENSY_LIB_ROOT}/teensy-cmake-macros")
    FetchContent_Declare(teensy_cmake_macros SOURCE_DIR ${TEENSY_LIB_ROOT}/teensy-cmake-macros)
else()
    message(STATUS "teensy-cmake-macros: fetching at the pinned ref")
    FetchContent_Declare(teensy_cmake_macros
        GIT_REPOSITORY https://github.com/newdigate/teensy-cmake-macros
        GIT_TAG        e948da4d43cf76e3a0d8813cd85e6da314a0a569)
endif()
FetchContent_MakeAvailable(teensy_cmake_macros)
include(${teensy_cmake_macros_SOURCE_DIR}/CMakeLists.include.txt)

# --- pinned library manifest -------------------------------------------------
# teensy_declare_library(NAME <subdir under TEENSY_LIB_ROOT> URL REF <sub-path
# inside the fetched repo>). NAME is what examples pass to
# import_evkb_library()/evkb_library_dir() (matches the existing call sites).
teensy_declare_library(cores          teensy-cores/${EVKB_CORE_SUBDIR} https://github.com/newdigate/teensy-cores    5bcae781b6c0e451f073298ddf7e1cd859f3e4de ${EVKB_CORE_SUBDIR})
teensy_declare_library(Wire           Wire                 https://github.com/newdigate/Wire            19babd18b83bc2f9ddbd16f6afefcbb42558530d .)
teensy_declare_library(SPI            SPI                  https://github.com/newdigate/SPI             eefd8798c74a727a09f38d34d79e1ab55c0110b3 .)
teensy_declare_library(PXP            PXP                  https://github.com/newdigate/PXP             5658e34885ff3a5cb5516a178ba60743e62a7517 .)
teensy_declare_library(ILI9341_t3     ILI9341_t3           https://github.com/newdigate/ILI9341_t3      e69e657f360e997e93fc7736a23eba8b09d1a043 .)
teensy_declare_library(MipiDisplay    MipiDisplay          https://github.com/newdigate/MipiDisplay     4cdb46c1d96cd7d42c05003481644cf81a8c030f .) # panel chosen by the importer: import_evkb_library(MipiDisplay soc panels/<name>)
teensy_declare_library(TouchPanel     TouchPanel           https://github.com/newdigate/TouchPanel      d20499c707290985379cb407689eca7f2c14fd08 .) # controller chosen by the importer: import_evkb_library(TouchPanel gt911)
teensy_declare_library(Audio          Audio                https://github.com/newdigate/Audio           de2d7bcee3ce72663a71c0e16f446648fbd5da84 .)
teensy_declare_library(SdFat          SdFat                https://github.com/newdigate/SdFat           681bfcf83d05beb943e3d905f15d8181bf9072c7 .)
teensy_declare_library(SD             PaulS_SD             https://github.com/newdigate/SD              e28c549918ea34ffb2942fd84deffc7c76a89880 .)
teensy_declare_library(SerialFlash    SerialFlash          https://github.com/newdigate/SerialFlash     2b6f24168c1ca97af1138c4a5b10255b39c4ad0b .)
teensy_declare_library(ethernet       Ethernet             https://github.com/newdigate/Ethernet        eebbfebc699a1500864236db21d17abf3cf7535a .)
teensy_declare_library(nativeethernet NativeEthernet       https://github.com/newdigate/NativeEthernet  7f5d881d5da80540177caea760d895780478b128 .)
teensy_declare_library(fnet           FNET/src             https://github.com/newdigate/FNET            a50373d50e57778595eb388b7bfeaad79080a077 src)
teensy_declare_library(lwip           lwip                 https://github.com/newdigate/lwip            03dddc67f73113e2beb3807e290a368d5cb7cfe0 .)
teensy_declare_library(USBHost_t36    USBHost_t36          https://github.com/newdigate/USBHost_t36     928bfefc2c9eebcb8e01bb4fd136b2cb6d5017f8 .)
teensy_declare_library(LVGL           LVGL                 https://github.com/newdigate/LVGL            6fa16a733d3d2a30b18f7ec15a2ad3791b02c66f .) # NOT Arduino-layout: use import_evkb_lvgl(), not import_evkb_library()
teensy_declare_library(EEPROM         EEPROM               https://github.com/newdigate/EEPROM          477c4296040d2061c90779f2841cdb953b5aca81 .)
teensy_declare_library(Bounce2        Bounce2/src          https://github.com/PaulStoffregen/Bounce2    eb5ab9fad8a15539743315786beb8236e96c8b9a src)
# ARM upstream (not Arduino-layout; consumed via import_evkb_cmsis_dsp below).
# CMSIS-Core is a headers-only dependency of CMSIS-DSP (cmsis_compiler.h et al).
teensy_declare_library(CMSIS-DSP  CMSIS-DSP https://github.com/ARM-software/CMSIS-DSP 4b4fa8ff218ca5ac20bad71b653a37d93815f24b .) # v1.17.1
teensy_declare_library(CMSIS-Core CMSIS_6   https://github.com/ARM-software/CMSIS_6   45dab712ad84f8cbbf2b7bfc089c19088507df6f .) # v6.3.0

# --- helpers: thin aliases over the macros' declared-library API -------------
# Kept so no example CMakeLists.txt changes (≈90 call sites). Memoization,
# local-first and force-fetch all live in the macros now.
macro(evkb_library_dir NAME OUT_VAR)
    teensy_resolve_library(${NAME} ${OUT_VAR})
endmacro()

macro(import_evkb_library NAME)
    teensy_import_library(${NAME} ${ARGN})
endmacro()
```

- [ ] **Step 5.4: Update the tail (current lines 302–311)** — replace with:

```cmake
# --- the core (every example needs it) ---------------------------------------
evkb_library_dir(cores EVKB_CORES_DIR)
# Re-point COREPATH at the resolved core (replacing the pre-include value set
# above). MUST happen BEFORE the first import_arduino_library call — that call
# runs teensy_set_dynamic_properties once, baking COREPATH into the link flags.
# Trailing slash required: the macros build LINKER_FILE as
# "${COREPATH}imxrt1176.ld" (117) or "${COREPATH}imxrt1060_evkb.ld" (42).
set(COREPATH "${EVKB_CORES_DIR}/" CACHE STRING "resolved core path" FORCE)
import_arduino_library(cores "${EVKB_CORES_DIR}")
```

- [ ] **Step 5.5: Verify — clean configure + build of blink, hex identical, right paths**

```bash
cd ~/Development/rt1170/evkb/examples/gpio-analog/blink
rm -rf build-post && cmake -B build-post -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake 2>&1 | grep -E "teensy-cmake-macros:|resolve_arduino_library_auto\(cores\)"
cmake --build build-post --parallel 8 >/dev/null
cmp build-post/blink.hex $SCRATCH/oracle/pre/blink.hex && echo "HEX IDENTICAL"
```

Expected: `teensy-cmake-macros: local /Users/nicholasnewdigate/Development/teensy-cmake-macros`, `resolve_arduino_library_auto(cores): local /Users/nicholasnewdigate/Development/teensy-cores/imxrt1176`, then `HEX IDENTICAL`. If the hex differs: STOP and diagnose (`arm-none-eabi-objdump`/`strings` on both ELFs; a diff explained ONLY by embedded source paths is a documented-and-accepted outcome per spec — anything else is a real resolution bug).

- [ ] **Step 5.6: Commit (evkb repo)**

```bash
cd ~/Development/rt1170/evkb
git add evkb.cmake
git commit -m "build: resolve cores+macros under TEENSY_LIB_ROOT via the declared-library layer

cores and teensy-cmake-macros stop being in-repo specials: macros
bootstrap local-first at \${TEENSY_LIB_ROOT}/teensy-cmake-macros (plain
FetchContent — the single CPM pin stays in the macros), cores join the
manifest as teensy-cores/<subdir>. Manifest + helpers become thin
aliases over teensy_declare/resolve/import_library. Blink hex verified
byte-identical to the pre-change build.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 6: Strip the 106 COREPATH setter sites

**Files:**
- Modify: 97× `examples/*/*/toolchain/rt1170-evkb.toolchain.cmake` (byte-identical, md5 `de3a026f1ce722f0a7afe6cf31a0e124`)
- Modify: 7× `examples/*/*/toolchain/rt1062-evkb.toolchain.cmake` (byte-identical, md5 `985bc1b883dc9e708473de5b80aef237`)
- Modify: `examples/storage-memory/sd_test/CMakeLists.txt`, `examples/audio/sd_wav_play_test/CMakeLists.txt`

- [ ] **Step 6.1: Write the two canonical toolchain files to scratch**

`$SCRATCH/rt1170-evkb.toolchain.cmake`:

```cmake
# Toolchain file for the NXP MIMXRT1170-EVKB (i.MX RT1176, Cortex-M7).
# Board identity + compiler only — COREPATH is owned by evkb.cmake, which
# resolves the core under TEENSY_LIB_ROOT (local-first, pinned fallback).
set(TEENSY_VERSION 117 CACHE STRING "RT1176 / MIMXRT1170-EVKB" FORCE)
set(CPU_CORE_SPEED 996000000 CACHE STRING "RT1176 M7 core clock" FORCE)

set(CMAKE_EXE_LINKER_FLAGS "--specs=nano.specs" CACHE INTERNAL "") # for linking stdc++ (nano)
if(DEFINED ENV{ARM_TOOLCHAIN_BIN})
    set(COMPILERPATH "$ENV{ARM_TOOLCHAIN_BIN}/")   # portable override
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

`$SCRATCH/rt1062-evkb.toolchain.cmake`:

```cmake
# Toolchain file for the NXP MIMXRT1060-EVKB (i.MX RT1062, Cortex-M7).
# Selects TEENSY_VERSION 42, which teensy-cmake-macros maps to the teensy4
# core and imxrt1060_evkb.ld. Board identity + compiler only — COREPATH is
# owned by evkb.cmake (resolved under TEENSY_LIB_ROOT, local-first).
set(TEENSY_VERSION 42 CACHE STRING "RT1062 / MIMXRT1060-EVKB" FORCE)
set(CPU_CORE_SPEED 600000000 CACHE STRING "RT1062 M7 core clock" FORCE)

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

- [ ] **Step 6.2: Deploy with md5 guard (refuses to overwrite anything unexpected)**

```bash
cd ~/Development/rt1170/evkb
deploy() { # <expected-md5> <canonical> <basename>
  n=0
  for f in $(find examples -path "*/toolchain/$3" -not -path '*build*'); do
    [ "$(md5 -q "$f")" = "$1" ] || { echo "UNEXPECTED CONTENT: $f — STOP"; return 1; }
    cp "$2" "$f"; n=$((n+1))
  done
  echo "$3: $n files replaced"
}
deploy de3a026f1ce722f0a7afe6cf31a0e124 $SCRATCH/rt1170-evkb.toolchain.cmake rt1170-evkb.toolchain.cmake
deploy 985bc1b883dc9e708473de5b80aef237 $SCRATCH/rt1062-evkb.toolchain.cmake rt1062-evkb.toolchain.cmake
```

Expected: `rt1170…: 97 files replaced`, `rt1062…: 7 files replaced`. Any `UNEXPECTED CONTENT`: STOP — a toolchain file diverged from the identical set; inspect it before touching it.

- [ ] **Step 6.3: Strip the 2 inline examples** — in `examples/storage-memory/sd_test/CMakeLists.txt` delete exactly these two lines (4–5):

```cmake
set(EVKB ${CMAKE_CURRENT_LIST_DIR}/../../..)
set(COREPATH "${EVKB}/cores/imxrt1176/" CACHE STRING "imxrt1176 core path" FORCE)
```

In `examples/audio/sd_wav_play_test/CMakeLists.txt` delete the identical pair (lines 8–9). (`${EVKB}` has no other use in either file — `EVKB_AUDIO_DIR` is a different variable, set by `evkb_library_dir`.)

- [ ] **Step 6.4: Verify — one toolchain example + one inline example, hexes identical**

```bash
cd ~/Development/rt1170/evkb/examples/gpio-analog/blink
rm -rf build-post && cmake -B build-post -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake >/dev/null && cmake --build build-post --parallel 8 >/dev/null
cmp build-post/blink.hex $SCRATCH/oracle/pre/blink.hex && echo "BLINK IDENTICAL"
cd ../../storage-memory/sd_test
rm -rf build-post && cmake -B build-post >/dev/null && cmake --build build-post --parallel 8 >/dev/null
cmp build-post/sd_test.hex $SCRATCH/oracle/pre/sd_test.hex && echo "SD_TEST IDENTICAL"
```

Expected: both `IDENTICAL`. (COREPATH now comes solely from evkb.cmake.)

- [ ] **Step 6.5: Commit**

```bash
cd ~/Development/rt1170/evkb
git add -A examples
git commit -m "build: strip the COREPATH guess from all 106 setter sites

evkb.cmake owns COREPATH now (Task 5); the 97+7 toolchain files and the
2 inline-toolchain examples kept pointing it at the in-repo core about
to be deleted. Deployed against md5 guards (both sets byte-identical).

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 7: Delete the in-repo copies

**Files:** delete directories `cores/`, `teensy-cmake-macros/` (both untracked nested repos — no evkb commit results).

- [ ] **Step 7.1: Re-verify the preconditions from Step 0.1** (both porcelains empty; both HEADs present in the `~/Development` twins). Any drift since Task 0: STOP.

- [ ] **Step 7.2: Delete**

```bash
cd ~/Development/rt1170/evkb
rm -rf cores teensy-cmake-macros
git status --short | head    # expect: the ?? cores/ and ?? teensy-cmake-macros/ lines are gone
```

- [ ] **Step 7.3: Full clean rebuild of the remaining oracle examples — all hexes identical**

```bash
cd ~/Development/rt1170/evkb
b() { d=$1; bd=$2; shift 2; (cd "$d" && rm -rf "$bd" && cmake -B "$bd" "$@" >/dev/null && cmake --build "$bd" --parallel 8 >/dev/null); }
b examples/serial/serial_test build-post -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake
b examples/serial/serial_test build-post-rt1062 -DEVKB_BOARD=rt1062 -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1062-evkb.toolchain.cmake
b examples/dualcore/cm4_boot_test build-post -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake
b examples/audio/sd_wav_play_test build-post
cmp examples/serial/serial_test/build-post/serial_test.hex        $SCRATCH/oracle/pre/serial_test.hex        && echo OK1
cmp examples/serial/serial_test/build-post-rt1062/serial_test.hex $SCRATCH/oracle/pre/serial_test-rt1062.hex && echo OK2
cmp examples/dualcore/cm4_boot_test/build-post/cm4_boot_test.hex  $SCRATCH/oracle/pre/cm4_boot_test.hex      && echo OK3
cmp examples/audio/sd_wav_play_test/build-post/sd_wav_play_test.hex $SCRATCH/oracle/pre/sd_wav_play_test.hex && echo OK4
```

Expected: OK1–OK4. The rt1062 build (OK2) is the critical one — it compiles `teensy-cores/teensy4`, which no longer exists in-repo. Same diff protocol as Step 5.5 on any mismatch.

---

### Task 8: License audit — re-point REPOS, root-independent ALLOW

**Files:**
- Modify: `tools/license-audit.sh` (lines 21–30 REPOS block, line 76 ALLOW, adjacent comments)

- [ ] **Step 8.1: Replace lines 22–30** (from `EVKB=` through the `REPOS=` closing `"}`) with:

```sh
EVKB=${LICENSE_AUDIT_EVKB:-$HOME/Development/rt1170/evkb}
# Sibling-checkout root — the same TEENSY_LIB_ROOT the build resolves against
# (evkb.cmake / teensy-cmake-macros). The audit must sweep the SAME trees the
# firmware compiles from, or it passes by measuring less: the core moved from
# $EVKB/cores to $LIB_ROOT/teensy-cores in the 2026-08-14 resolution change.
LIB_ROOT=${TEENSY_LIB_ROOT:-$HOME/Development}
fail=0

REPOS=${LICENSE_AUDIT_REPOS:-"$LIB_ROOT/teensy-cores $LIB_ROOT/Ethernet $LIB_ROOT/NativeEthernet \
$LIB_ROOT/SdFat $LIB_ROOT/SPI $LIB_ROOT/Wire \
$LIB_ROOT/Audio $LIB_ROOT/SD $LIB_ROOT/PaulS_SD \
$LIB_ROOT/USBHost_t36 $LIB_ROOT/FNET $LIB_ROOT/lwip \
$LIB_ROOT/CMSIS-DSP $LIB_ROOT/CMSIS_6 $LIB_ROOT/SerialFlash \
$LIB_ROOT/PXP $LIB_ROOT/MipiDisplay $LIB_ROOT/LVGL \
$LIB_ROOT/EEPROM"}
```

(Note `fail=0` moves with the block — it currently sits between `EVKB=` and `REPOS=`; keep exactly one occurrence.)

- [ ] **Step 8.2: Replace the ALLOW line (currently line 76)** with:

```sh
# Entries are deliberately ROOT-INDEPENDENT (no Development/ prefix): they must
# match under $LIB_ROOT wherever it points, AND under this script's own tests'
# throwaway trees. Slightly wider than path-anchored — acceptable because ALLOW
# only applies inside REPOS directories and part 2's EMPTY-object rule
# independently backstops every entry (see each justification above).
ALLOW='cores/teensy/|cores/teensy3/|cores/teensy4/|/SPI/SPI\.(h|cpp)$|/Wire/Wire\.(h|cpp)$|/Wire/utility/twi\.(h|c)$|/LVGL/lvgl/src/libs/thorvg/tvgLottieInterpolator\.cpp$'
```

(The three `cores/teensy*/` entries are unchanged — they match `$LIB_ROOT/teensy-cores/teensy*/` by substring, and the test fixtures' `$t/cores/teensy4/`.)

- [ ] **Step 8.3: In the big allowlist comment above ALLOW**, update the first line `#   cores/teensy/, cores/teensy3/` context: change the phrase `uncompiled PJRC reference copies, never in any build` to `uncompiled PJRC reference copies inside the teensy-cores sibling repo, never in any build`. Leave the rest of that comment block untouched.

- [ ] **Step 8.4: Run the audit and its negative tests**

```bash
cd ~/Development/rt1170/evkb
sh tools/license-audit.sh | tail -15
sh tools/license-audit.test.sh | tail -5
```

Expected: `LICENSE-AUDIT: PASS`, with the two rt1062 GATES entries walked (dep-path counts 136 and 203 — same sources, so same counts; a *small* delta is suspicious, a zero is a broken walk). `license-audit.test.sh`: all tests pass, **unchanged** — that is the evidence the ALLOW rework didn't weaken what the tests pin.

- [ ] **Step 8.5: Commit**

```bash
git add tools/license-audit.sh
git commit -m "audit: sweep TEENSY_LIB_ROOT siblings; ALLOW goes root-independent

REPOS follows the core to \$LIB_ROOT/teensy-cores (else it silently
drops out of part 1 and the audit passes by measuring less). ALLOW
drops the Development/ anchor so entries match under any root and the
test fixtures alike — backstopped by part 2's EMPTY-object rule.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 9: Mass clean rebuild, full sweep, remaining harness + worktree probe

**Files:** none modified. This is the spec §6 verification table. Budget 1–2 h of builds + the sweep.

- [ ] **Step 9.1: Rebuild EVERY gate-owning example from a clean build dir**

```bash
cd ~/Development/rt1170/evkb
./tools/run-all-qemu-gates.sh -l | grep -E '^rt1(176|062):' > $SCRATCH/gates.txt
wc -l < $SCRATCH/gates.txt    # expect: 89
fails=0
while IFS= read -r gate; do
  board=${gate%%:*}; ex=${gate#*:}; dir=examples/$ex
  [ -d "$dir" ] || { echo "NO DIR: $gate"; fails=$((fails+1)); continue; }
  case "$ex" in
    storage-memory/sd_test|audio/sd_wav_play_test) bd=build; args="" ;;
    *) if [ "$board" = rt1062 ]; then bd=build-rt1062; args="-DEVKB_BOARD=rt1062 -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1062-evkb.toolchain.cmake"
       else bd=build; args="-DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake"; fi ;;
  esac
  ( cd "$dir" && rm -rf "$bd" && cmake -B "$bd" $args >/dev/null 2>"$SCRATCH/cfg-err.txt" \
      && cmake --build "$bd" --parallel 8 >/dev/null 2>"$SCRATCH/bld-err.txt" ) \
    && echo "BUILT $gate" || { echo "FAILED $gate"; fails=$((fails+1)); }
done < $SCRATCH/gates.txt
echo "build failures: $fails"
```

Expected: 89 `BUILT` lines, `build failures: 0`. Every FAILED here would have been a SKIP in the sweep — fix before proceeding (read `$SCRATCH/cfg-err.txt`/`bld-err.txt`).

- [ ] **Step 9.2: Hex oracle — final comparison, then clean up pre dirs**

```bash
cd ~/Development/rt1170/evkb
cmp examples/gpio-analog/blink/build/blink.hex                       $SCRATCH/oracle/pre/blink.hex && echo F1
cmp examples/serial/serial_test/build/serial_test.hex                $SCRATCH/oracle/pre/serial_test.hex && echo F2
cmp examples/serial/serial_test/build-rt1062/serial_test.hex         $SCRATCH/oracle/pre/serial_test-rt1062.hex && echo F3
cmp examples/dualcore/cm4_boot_test/build/cm4_boot_test.hex          $SCRATCH/oracle/pre/cm4_boot_test.hex && echo F4
cmp examples/storage-memory/sd_test/build/sd_test.hex                $SCRATCH/oracle/pre/sd_test.hex && echo F5
cmp examples/audio/sd_wav_play_test/build/sd_wav_play_test.hex       $SCRATCH/oracle/pre/sd_wav_play_test.hex && echo F6
rm -rf examples/gpio-analog/blink/build-pre* examples/serial/serial_test/build-pre* \
       examples/dualcore/cm4_boot_test/build-pre examples/storage-memory/sd_test/build-pre \
       examples/audio/sd_wav_play_test/build-pre \
       examples/gpio-analog/blink/build-post examples/storage-memory/sd_test/build-post \
       examples/serial/serial_test/build-post* examples/dualcore/cm4_boot_test/build-post \
       examples/audio/sd_wav_play_test/build-post
```

Expected: F1–F6.

- [ ] **Step 9.3: The sweep**

```bash
cd ~/Development/rt1170/evkb && ./tools/run-all-qemu-gates.sh
```

Expected: **89 passed, 0 failed, 0 SKIP** — or 88/1/0 where the ONE failure is `rt1176:dualcore/cm4_audio_test` (re-run it idle before believing the red; any OTHER red is a regression from this change — read the gate NAMES). **Any SKIP is a failure of this plan**: it means Step 9.1 missed a build.

- [ ] **Step 9.4: Gate harness test suites**

```bash
cd ~/Development/rt1170/evkb
sh tools/gate-lib.test.sh | tail -3
sh tools/gate-vacuity.test.sh | tail -3
```

Expected: both pass, unchanged.

- [ ] **Step 9.5: Worktree probe — the original complaint, verified fixed**

```bash
cd ~/Development/rt1170/evkb
git worktree add $SCRATCH/wt-probe HEAD
cd $SCRATCH/wt-probe/examples/gpio-analog/blink
cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake 2>&1 | tee $SCRATCH/wt-configure.log | grep -E "local|fetch|Download"
grep -cE "fetching|Downloading|Cloning" $SCRATCH/wt-configure.log
cmake --build build --parallel 8 >/dev/null && echo "WORKTREE BUILD OK"
cd ~/Development/rt1170/evkb && git worktree remove --force $SCRATCH/wt-probe
```

Expected: `teensy-cmake-macros: local …` and `resolve…(cores): local …` lines; the grep count of fetch words is `0`; `WORKTREE BUILD OK`.

---

### Task 10: Documentation (evkb repo)

**Files:**
- Modify: `CLAUDE.md` (git-layout section, library-resolution bullet, architecture core bullet)
- Modify: `README.md` (optional-siblings prerequisite bullet)
- Modify: `examples/README.md` (bootstrap paragraph)

- [ ] **Step 10.1: CLAUDE.md — replace the two git-layout bullets** ("This repo is …" stays; replace the `cores/ and teensy-cmake-macros/` bullet and merge into the peripheral-libraries bullet):

Old:

```markdown
- `cores/` and `teensy-cmake-macros/` are **nested independent git repos**
  (they show as untracked in this repo's status — that is normal).
- Peripheral libraries (Wire, SPI, Audio, SdFat, SD, Ethernet, NativeEthernet,
  FNET, lwip, USBHost_t36, …) live as sibling checkouts under
  `~/Development/<lib>`, each its own repo.
```

New:

```markdown
- **Nothing else lives inside this repo.** The core (`teensy-cores`), the
  build macros (`teensy-cmake-macros`) and all peripheral libraries (Wire,
  SPI, Audio, SdFat, SD, Ethernet, NativeEthernet, FNET, lwip, USBHost_t36,
  …) are sibling checkouts under `$TEENSY_LIB_ROOT` (default
  `~/Development/<lib>`), each its own repo. A reappearing `?? cores/` or
  `?? teensy-cmake-macros/` in git status is a STALE in-repo copy from before
  2026-08-14 — it is dead (nothing resolves there); delete it.
```

- [ ] **Step 10.2: CLAUDE.md — library-resolution bullet.** Replace:

```markdown
- **Library resolution is local-first**: a `~/Development/<lib>` checkout wins
  (including uncommitted edits); if absent, the library is fetched from GitHub
  at a SHA pinned in `evkb.cmake`. `-DEVKB_FORCE_FETCH=ON` forces the pinned
  fetch ("fresh user" mode). After pushing new library work, the pin in
  `evkb.cmake` must be updated by hand.
```

with:

```markdown
- **Library resolution is local-first**: a `$TEENSY_LIB_ROOT/<lib>` checkout
  wins (default `~/Development`; env var to override — including uncommitted
  edits); if absent, the library is fetched from GitHub at a SHA pinned in
  `evkb.cmake`. This covers the core (`teensy-cores`) and the build macros
  (`teensy-cmake-macros`) too — the macros are the one repo fetched with plain
  FetchContent rather than CPM (the single CPM pin lives in the macros;
  `CPM_SOURCE_CACHE` covers everything else). `-DEVKB_FORCE_FETCH=ON` forces
  the pinned fetch ("fresh user" mode). After pushing new library work, the
  pin in `evkb.cmake` must be updated by hand.
```

- [ ] **Step 10.3: CLAUDE.md — architecture bullet.** Change the opening of the core bullet from `**`cores/imxrt1176/`** — the core:` to `**`imxrt1176/` (in the `teensy-cores` sibling repo)** — the core:` and, later in that bullet, change `**`cores/teensy4/` used to be an uncompiled upstream reference copy; since the RT1060 board axis it is the core that `EVKB_BOARD=rt1062` actually builds.**` to `**`teensy4/` (same repo) used to be an uncompiled upstream reference copy; since the RT1060 board axis it is the core that `EVKB_BOARD=rt1062` actually builds.**`

- [ ] **Step 10.4: README.md — replace the optional-siblings bullet:**

```markdown
- Optional: sibling library checkouts under `~/Development/` — used when
  present; otherwise fetched automatically from GitHub at pinned refs. Set the
  `CPM_SOURCE_CACHE` env var (e.g. `~/.cache/CPM`) so each repo is cloned once
  and shared across build directories
```

with:

```markdown
- Optional: sibling library checkouts under `$TEENSY_LIB_ROOT` (default
  `~/Development/`) — used when present, including the core (`teensy-cores`)
  and build macros (`teensy-cmake-macros`); otherwise fetched automatically
  from GitHub at pinned refs. Set the `CPM_SOURCE_CACHE` env var (e.g.
  `~/.cache/CPM`) so each repo is cloned once and shared across build
  directories — the macros themselves are the one exception (456K, plain
  FetchContent per build dir, deliberate)
```

- [ ] **Step 10.5: examples/README.md — in the bootstrap paragraph**, replace `(a `~/Development/<lib>` checkout wins)` with `(a `$TEENSY_LIB_ROOT/<lib>` checkout wins; default `~/Development`)` and append after "fresh-user mode);": `the macros repo itself is fetched with plain FetchContent (the one repo `CPM_SOURCE_CACHE` doesn't cover, deliberately);`

- [ ] **Step 10.6: Commit**

```bash
cd ~/Development/rt1170/evkb
git add CLAUDE.md README.md examples/README.md
git commit -m "docs: cores/macros live under TEENSY_LIB_ROOT, not in-repo

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 11: Push macros, bump the pin, fresh-user verification

**⚠ Pushing is outward-facing — CONFIRM with the user before each push.**

- [ ] **Step 11.1: Ask the user to approve pushing `~/Development/teensy-cmake-macros` (3 commits) to `origin/master`.** Note: GitHub CI (`.github/workflows/*`) will run the 5 tests on ubuntu with GCC 9 — watch it; a CI-only failure (e.g. the pinned core under GCC 9) must be fixed, not ignored.

```bash
cd ~/Development/teensy-cmake-macros && git push origin master && git rev-parse HEAD
```

- [ ] **Step 11.2: Bump the macros pin in evkb.cmake** — replace `e948da4d43cf76e3a0d8813cd85e6da314a0a569` (the `GIT_TAG` in the bootstrap) with the SHA printed by Step 11.1.

- [ ] **Step 11.3: Fresh-user simulation (`EVKB_FORCE_FETCH=ON`)**

```bash
cd ~/Development/rt1170/evkb/examples/gpio-analog/blink
export CPM_SOURCE_CACHE=$SCRATCH/cpm-cache
rm -rf build-ff && cmake -B build-ff -DEVKB_FORCE_FETCH=ON \
  -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake 2>&1 | grep -E "fetching|teensy-cmake-macros:"
cmake --build build-ff --parallel 8 >/dev/null && echo "FF BUILD OK"
ls $SCRATCH/cpm-cache/ | head                      # expect: a cores/ (CPM cache) entry
ls build-ff/_deps/ | grep teensy_cmake_macros      # expect: -src (FetchContent clone, the documented exception)
grep -r "5bcae781" build-ff/CMakeCache.txt build-ff/_deps 2>/dev/null | head -2   # core fetched at the PIN, not master
cmp build-ff/blink.hex $SCRATCH/oracle/pre/blink.hex && echo "FF HEX IDENTICAL"
rm -rf build-ff; unset CPM_SOURCE_CACHE
```

Expected: `teensy-cmake-macros: fetching at the pinned ref`, `resolve…(cores): fetching … @ 5bcae781…`, `FF BUILD OK`, cache + `_deps` evidence as annotated, `FF HEX IDENTICAL` (same source SHAs → same bytes; on mismatch apply the Step 5.5 diff protocol).

- [ ] **Step 11.4: Commit the pin bump; ask the user to approve pushing evkb**

```bash
cd ~/Development/rt1170/evkb
git add evkb.cmake
git commit -m "build: bump teensy-cmake-macros pin to the declared-library layer

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
# after user approval:
git push origin master
```

- [ ] **Step 11.5: Close out** — record final state in `$SCRATCH/notes.md`: sweep result, oracle verdicts, macros/evkb SHAs. Report to the user with the spec's verification table filled in.

---

## Self-review notes (kept for the executor)

- Spec §2 → Tasks 2–4; §3 → Task 5; §4 → Tasks 6–8 + 10; §5 sequencing → task order + Task 11; §6 verification table → Tasks 0, 5.5, 6.4, 7.3, 8.4, 9, 11.3.
- `tests/basic` consumes `teensy_cores_SOURCE_DIR` — the Task 3 fallback exports it explicitly (FetchContent used to; CPM's variable only appears on the fetch path, and the local path sets none).
- All macros test invocations MUST pass `-DFETCHCONTENT_SOURCE_DIR_TEENSY_CMAKE_MACROS=$HOME/Development/teensy-cmake-macros` — the test CMakeLists fetch the macros from GitHub `main`, and without the override every run tests the wrong tree.
- The force-fetch/CPM path of `teensy_resolve_library` is intentionally uncovered by tests/resolve (network); Task 11.3 covers it end-to-end.
- Gate-list format verified: `board:cat/name` lines + trailing `(89 gate(s))` summary — the grep in Step 9.1 strips the summary.
