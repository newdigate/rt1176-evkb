# Toolchain-File Dedup Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Collapse the 104 identical per-example toolchain files + 2 inlined copies into exactly two files at `evkb/toolchain/`, per `docs/superpowers/specs/2026-08-14-toolchain-dedup-design.md`.

**Architecture:** Pure file movement — two root canonicals (content byte-identical to today's), all per-example `toolchain/` dirs deleted under md5 guard, the two inline examples switch to `include()`, 3 scripts + 5 doc snippets re-pointed. One atomic mechanical commit (the tree must never have a state where scripts point at deleted files), one docs commit.

**Tech Stack:** CMake, shell, the repo's gate/audit harness.

**Execute in the main checkout** `/Users/nicholasnewdigate/Development/rt1170/evkb` (not a worktree — verification compares against pre-existing build dirs). Tree must start clean at the pushed state.

```
SCRATCH=/private/tmp/claude-501/-Users-nicholasnewdigate-Development-rt1170-evkb/2f964c6a-5f66-4b74-b61e-f17a47e6b202/scratchpad/tc-dedup
```

Canonical md5s (from the resolution migration's Task 6, still current):
- rt1170: `9943c85cff0ed3e7618b8eca63fce761`
- rt1062: `04895cac491ee3faa40e41c1c68955b6`

Hex-naming note: blink's target is `blinky`, so its hex is `blinky.hex`.

---

### Task 0: Preconditions + baseline oracle

**Files:** none modified.

- [ ] **Step 0.1: Clean tree at pushed state**

```bash
cd ~/Development/rt1170/evkb
git status --short              # expect: empty
git rev-list --count origin/master..master   # expect: 0
mkdir -p $SCRATCH/oracle
```

Any dirt or unpushed commits: STOP and report — the baseline must be the pushed state.

- [ ] **Step 0.2: Baseline builds of the five oracle examples (current commands)**

```bash
cd ~/Development/rt1170/evkb
b() { d=$1; bd=$2; shift 2; (cd "$d" && rm -rf "$bd" && cmake -B "$bd" "$@" >/dev/null && cmake --build "$bd" --parallel 8 >/dev/null) || { echo "BUILD FAIL: $d"; return 1; }; }
b examples/gpio-analog/blink build-pre -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake
b examples/serial/serial_test build-pre -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake
b examples/serial/serial_test build-pre-rt1062 -DEVKB_BOARD=rt1062 -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1062-evkb.toolchain.cmake
b examples/storage-memory/sd_test build-pre
b examples/audio/sd_wav_play_test build-pre
cp examples/gpio-analog/blink/build-pre/blinky.hex                 $SCRATCH/oracle/blink.hex
cp examples/serial/serial_test/build-pre/serial_test.hex           $SCRATCH/oracle/serial_test.hex
cp examples/serial/serial_test/build-pre-rt1062/serial_test.hex    $SCRATCH/oracle/serial_test-rt1062.hex
cp examples/storage-memory/sd_test/build-pre/sd_test.hex           $SCRATCH/oracle/sd_test.hex
cp examples/audio/sd_wav_play_test/build-pre/sd_wav_play_test.hex  $SCRATCH/oracle/sd_wav_play_test.hex
ls -la $SCRATCH/oracle/
```

Expected: five hexes stashed, no BUILD FAIL. Keep the `build-pre*` dirs until Task 4.

---

### Task 1: Create the two root canonical files

**Files:**
- Create: `toolchain/rt1170-evkb.toolchain.cmake`
- Create: `toolchain/rt1062-evkb.toolchain.cmake`

- [ ] **Step 1.1: Copy from existing canonicals and verify hashes**

```bash
cd ~/Development/rt1170/evkb
mkdir -p toolchain
cp examples/gpio-analog/blink/toolchain/rt1170-evkb.toolchain.cmake toolchain/
cp examples/serial/serial_test/toolchain/rt1062-evkb.toolchain.cmake toolchain/
md5 -q toolchain/rt1170-evkb.toolchain.cmake   # expect 9943c85cff0ed3e7618b8eca63fce761
md5 -q toolchain/rt1062-evkb.toolchain.cmake   # expect 04895cac491ee3faa40e41c1c68955b6
```

Any hash mismatch: STOP — a per-example file diverged; investigate before copying.

- [ ] **Step 1.2: Prove the new command works before anything is deleted**

```bash
cd ~/Development/rt1170/evkb/examples/gpio-analog/blink
rm -rf build-new && cmake -B build-new -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake >/dev/null && cmake --build build-new --parallel 8 >/dev/null
cmp build-new/blinky.hex $SCRATCH/oracle/blink.hex && echo "NEW-COMMAND HEX IDENTICAL"
rm -rf build-new
```

Expected: `NEW-COMMAND HEX IDENTICAL`. (Relative `CMAKE_TOOLCHAIN_FILE` resolves against the source dir — verified by probe during design.)

Do NOT commit yet — Task 4 makes the atomic commit.

---

### Task 2: Switch the two inline examples to include()

**Files:**
- Modify: `examples/storage-memory/sd_test/CMakeLists.txt`
- Modify: `examples/audio/sd_wav_play_test/CMakeLists.txt`

- [ ] **Step 2.1: sd_test** — read the file, then replace the whole inlined block (every line from `set(TEENSY_VERSION 117 ...)` through `set(CMAKE_CXX_LINK_EXECUTABLE ...)` inclusive — it contains the duplicated nested `ARM_TOOLCHAIN_BIN` if-block) with:

```cmake
# Board + compiler come from the shared toolchain file, included BEFORE
# project() — same mechanism as passing -DCMAKE_TOOLCHAIN_FILE, so a plain
# `cmake -B build` still cross-compiles with no extra flags.
include(${CMAKE_CURRENT_LIST_DIR}/../../../toolchain/rt1170-evkb.toolchain.cmake)
```

The `cmake_minimum_required` line stays first; `project(sd_test)` and everything after stay untouched.

- [ ] **Step 2.2: sd_wav_play_test** — same replacement (its block is wrapped by `# --- Toolchain bootstrap ...` and `# ---...---` comment lines; those go too). Keep `cmake_minimum_required` first, `project(sd_wav_play_test)` after the include.

- [ ] **Step 2.3: Verify plain-configure still works for both**

```bash
cd ~/Development/rt1170/evkb/examples/storage-memory/sd_test
rm -rf build-new && cmake -B build-new >/dev/null && cmake --build build-new --parallel 8 >/dev/null
cmp build-new/sd_test.hex $SCRATCH/oracle/sd_test.hex && echo "SD_TEST IDENTICAL"
cd ../../audio/sd_wav_play_test
rm -rf build-new && cmake -B build-new >/dev/null && cmake --build build-new --parallel 8 >/dev/null
cmp build-new/sd_wav_play_test.hex $SCRATCH/oracle/sd_wav_play_test.hex && echo "SD_WAV IDENTICAL"
rm -rf build-new ../../storage-memory/sd_test/build-new
```

Expected: both IDENTICAL.

---

### Task 3: Re-point the three scripts

**Files:**
- Modify: `tools/driftrun.sh` (line ~98)
- Modify: `tools/uacvalidate/corpus/run_case.sh` (line ~107)
- Modify: `tools/uacvalidate/corpus/run_case_swap.sh` (line ~75)

- [ ] **Step 3.1:** Read each script's surrounding ~20 lines first (what directory it cds to, what variables exist). Current lines:
  - `driftrun.sh:98`: `-DCMAKE_TOOLCHAIN_FILE="$EXDIR/toolchain/rt1170-evkb.toolchain.cmake" \` — `$EXDIR` is the example dir; replace the value with the repo-root file. If the script already has a repo-root variable, use it; otherwise derive one near the top (e.g. `EVKB_ROOT=$(cd "$(dirname "$0")/.." && pwd)`) and use `"$EVKB_ROOT/toolchain/rt1170-evkb.toolchain.cmake"`.
  - `run_case.sh:107` and `run_case_swap.sh:75`: `-DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake` — these run with the example dir as cwd; `../../../toolchain/rt1170-evkb.toolchain.cmake` is the equivalent. Verify the cwd assumption from the surrounding cd/pushd before editing; if cwd is not `examples/<cat>/<name>`, use the same repo-root-variable approach as driftrun.

- [ ] **Step 3.2: Syntax-check all three** — `sh -n tools/driftrun.sh tools/uacvalidate/corpus/run_case.sh tools/uacvalidate/corpus/run_case_swap.sh` (expect silence). Full end-to-end runs need hardware/bench state; the configure-path correctness is covered by Task 5's grep (no stale path survives) plus the Task 1 proof that the root file configures.

---

### Task 4: Delete the 104 per-example dirs (md5-guarded) + atomic commit

**Files:** delete 104 × `examples/*/*/toolchain/`

- [ ] **Step 4.1: Guarded deletion**

```bash
cd ~/Development/rt1170/evkb
fail=0; n70=0; n62=0
for f in $(find examples -path '*/toolchain/rt1170-evkb.toolchain.cmake' -not -path '*build*'); do
  [ "$(md5 -q "$f")" = "9943c85cff0ed3e7618b8eca63fce761" ] || { echo "UNEXPECTED: $f"; fail=1; continue; }
  rm "$f"; n70=$((n70+1))
done
for f in $(find examples -path '*/toolchain/rt1062-evkb.toolchain.cmake' -not -path '*build*'); do
  [ "$(md5 -q "$f")" = "04895cac491ee3faa40e41c1c68955b6" ] || { echo "UNEXPECTED: $f"; fail=1; continue; }
  rm "$f"; n62=$((n62+1))
done
echo "deleted: $n70 rt1170, $n62 rt1062, fail=$fail"
find examples -type d -name toolchain -not -path '*build*' -empty -delete
find examples -type d -name toolchain -not -path '*build*' | head   # expect: empty
```

Expected: `deleted: 97 rt1170, 7 rt1062, fail=0`, no surviving toolchain dirs. Any `UNEXPECTED`: STOP, report the file, delete nothing further.

- [ ] **Step 4.2: No stale references anywhere in the build system**

```bash
cd ~/Development/rt1170/evkb
grep -rn "toolchain/rt1" --include=CMakeLists.txt --include=*.cmake --include=*.sh examples tools evkb.cmake 2>/dev/null | grep -v build | grep -v "^toolchain/" | grep -v "\.\./\.\./\.\./toolchain/\|EVKB_ROOT/toolchain/\|CMAKE_CURRENT_LIST_DIR}/\.\./\.\./\.\./toolchain/"
```

Expected: empty (every surviving reference points at the root `toolchain/`). Investigate any hit.

- [ ] **Step 4.3: Atomic commit**

```bash
cd ~/Development/rt1170/evkb
git add -A toolchain examples tools
git status --short | head -12    # sanity: 2 adds, 104 deletes, 2 M examples, 3 M tools
git commit -m "build: dedup toolchain files — 104 per-example copies become 2 root files

The resolution migration left these as identical 18-line board-identity
stubs (one md5 x 97, one x 7). Two canonicals now live at toolchain/;
the command becomes -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/<board>
(same reach-up shape as the evkb.cmake include). sd_test and
sd_wav_play_test include() the shared file before project() — plain
cmake -B build still works, and their duplicated nested
ARM_TOOLCHAIN_BIN if-block goes with the paste. driftrun + the two
uacvalidate corpus runners re-pointed. Hexes verified identical via
both the new flag and the include path before anything was deleted.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 5: Verification battery

**Files:** none modified.

- [ ] **Step 5.1: Post-change oracle — all five via the new world**

```bash
cd ~/Development/rt1170/evkb
b() { d=$1; bd=$2; shift 2; (cd "$d" && rm -rf "$bd" && cmake -B "$bd" "$@" >/dev/null && cmake --build "$bd" --parallel 8 >/dev/null) || echo "BUILD FAIL: $d"; }
b examples/gpio-analog/blink build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake
b examples/serial/serial_test build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake
b examples/serial/serial_test build-rt1062 -DEVKB_BOARD=rt1062 -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1062-evkb.toolchain.cmake
b examples/storage-memory/sd_test build
b examples/audio/sd_wav_play_test build
cmp examples/gpio-analog/blink/build/blinky.hex                 $SCRATCH/oracle/blink.hex && echo V1
cmp examples/serial/serial_test/build/serial_test.hex           $SCRATCH/oracle/serial_test.hex && echo V2
cmp examples/serial/serial_test/build-rt1062/serial_test.hex    $SCRATCH/oracle/serial_test-rt1062.hex && echo V3
cmp examples/storage-memory/sd_test/build/sd_test.hex           $SCRATCH/oracle/sd_test.hex && echo V4
cmp examples/audio/sd_wav_play_test/build/sd_wav_play_test.hex  $SCRATCH/oracle/sd_wav_play_test.hex && echo V5
rm -rf examples/gpio-analog/blink/build-pre examples/serial/serial_test/build-pre \
       examples/serial/serial_test/build-pre-rt1062 examples/storage-memory/sd_test/build-pre \
       examples/audio/sd_wav_play_test/build-pre
```

Expected: V1–V5, no BUILD FAIL. NOTE: these five rebuilds refresh their real `build*/` dirs — the five gates owned by these examples keep working. All OTHER examples' build dirs are now stale-on-reconfigure by design (elfs remain valid).

- [ ] **Step 5.2: Stale elfs still gate green (one per board)**

```bash
cd ~/Development/rt1170/evkb
./tools/run-all-qemu-gates.sh -l | tail -1                          # expect (89 gate(s))
./tools/run-all-qemu-gates.sh "rt1176:gpio-analog/analog_test" 2>&1 | tail -1   # untouched stale build dir
./tools/run-all-qemu-gates.sh "rt1062:serial/serial_test" 2>&1 | tail -1        # freshly rebuilt dir
```

Expected: `(89 gate(s))`, both runs `gates: 1 passed`.

- [ ] **Step 5.3: Audit + suites unchanged**

```bash
cd ~/Development/rt1170/evkb
sh tools/license-audit.sh > /tmp/a.out 2>&1; echo "audit exit: $?"; tail -2 /tmp/a.out
sh tools/license-audit.test.sh > /tmp/t.out 2>&1; echo "suite exit: $?"; grep -c "^PASS" /tmp/t.out
sh tools/gate-lib.test.sh 2>&1 | tail -2
sh tools/gate-vacuity.test.sh 2>&1 | tail -2
```

Expected: `LICENSE-AUDIT: PASS` exit 0; suite exit 0 with 22 PASS; both gate suites pass. (Depfiles never reference toolchain files, so the audit cannot move — a change here means something unexpected happened.)

---

### Task 6: Docs + migration note

**Files:**
- Modify: `README.md` (~lines 77, 165), `CLAUDE.md` (~lines 39, 128), `examples/README.md` (~line 11)

- [ ] **Step 6.1:** In each of the five snippets, change `toolchain/rt1170-evkb.toolchain.cmake` → `../../../toolchain/rt1170-evkb.toolchain.cmake` (and `toolchain/rt1062-...` → `../../../toolchain/rt1062-...` at CLAUDE.md:128). Verify each anchor by content — line numbers are approximate.

- [ ] **Step 6.2:** Check for prose that asserts per-example toolchain dirs: `grep -n "toolchain" README.md CLAUDE.md examples/README.md` — fix any sentence claiming each example carries its own toolchain dir (examples/README's "The two SD examples ... inline their toolchain" needs rewording: they now include the shared file, plain `cmake -B build` unchanged).

- [ ] **Step 6.3:** Add the migration note (once in README.md near the build instructions, once in CLAUDE.md's build section):

```markdown
Build dirs configured before 2026-08-14 cached an absolute toolchain path
that no longer exists; their elfs remain valid, but the first reconfigure
fails with "toolchain file not found" — `rm -rf` the build dir and configure
fresh with the command above.
```

- [ ] **Step 6.4: Commit**

```bash
cd ~/Development/rt1170/evkb
git add README.md CLAUDE.md examples/README.md
git commit -m "docs: toolchain command moves to the shared root file; stale-dir note

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 7: Close out

- [ ] **Step 7.1:** `git status --short` clean; `git log --oneline origin/master..master` shows exactly the two commits. Report the verification table (V1–V5, gate runs, audit, suites) and STOP — pushing is the user's call.

---

## Self-review notes

- Spec §2 → Tasks 1–4 + 6; §4 verification table → Tasks 0, 1.2, 2.3, 5; §5 sequencing (atomic mechanical commit) → Task 4.3 single commit covering adds+edits+deletions.
- Task 1.2 proves the new command BEFORE any deletion; Task 2.3 proves the include path before deletion; the scripts are syntax-checked and covered by Task 4.2's no-stale-reference grep.
- blink hex is `blinky.hex` throughout.
- The five oracle rebuilds in 5.1 deliberately land in the REAL build dirs so those gates stay fresh; 5.2 proves an untouched stale dir still gates.
