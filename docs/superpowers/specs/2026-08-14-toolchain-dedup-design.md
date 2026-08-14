# Toolchain-file dedup — Design

**Goal.** Collapse the 104 byte-identical per-example toolchain files (97
`rt1170-evkb.toolchain.cmake` + 7 `rt1062-evkb.toolchain.cmake`) and the two
inlined copies into exactly **two files at the repo root** — `toolchain/
rt1170-evkb.toolchain.cmake` and `toolchain/rt1062-evkb.toolchain.cmake` —
with zero content copies anywhere else.

**Why now.** The 2026-08-14 resolution migration
(`2026-08-14-shared-cores-macros-resolution-design.md`) stripped the COREPATH
guess from these files, leaving 18–19 lines of pure board identity + compiler
setup, identical per board (one md5 × 97, one md5 × 7). The remaining
duplication is pure sprawl: a new example copies a directory it doesn't need,
and the two inline examples carry a third and fourth copy of the same content
(complete with a duplicated nested `ARM_TOOLCHAIN_BIN` if-block bug). This was
"Approach C" of the resolution design, deferred there as a separate concern.

**Blast radius, measured 2026-08-14:** 3 scripts configure with per-example
toolchain paths (`tools/driftrun.sh:98`,
`tools/uacvalidate/corpus/run_case.sh:107`,
`tools/uacvalidate/corpus/run_case_swap.sh:75`) and 5 doc snippets show the
command (`README.md:77,165`, `CLAUDE.md:39,128`, `examples/README.md:11`).

---

## 1. Decisions

| Decision | Choice | Rejected |
|---|---|---|
| Shape | **Delete all 104 per-example `toolchain/` dirs; two shared files at `evkb/toolchain/`** | one-line include shims (kills drift, keeps sprawl); leave as-is |
| Board files | **Two per-board files, names unchanged** | one board-agnostic file keyed on `-DEVKB_BOARD` (erases the mismatch class structurally, but changes the command's meaning, demotes the new configure-time assert, and adds try_compile cache-propagation subtlety for no measured pain) |
| Inline pair | **`sd_test` + `sd_wav_play_test` `include()` the shared file before `project()`** | leave inlined (keeps 2 divergence-prone copies + the duplicated if-block bug) |
| Stale build dirs | **Accepted and documented, no mass rebuild** | rebuilding ~100 dirs whose content this change cannot alter |

## 2. The change

- Create `toolchain/rt1170-evkb.toolchain.cmake` and
  `toolchain/rt1062-evkb.toolchain.cmake` at the repo root, byte-identical to
  the current per-example canonicals (md5s `9943c85cff0ed3e7618b8eca63fce761`
  and `04895cac491ee3faa40e41c1c68955b6`). Content is unchanged — this design
  moves files, nothing else.
- Delete all 104 `examples/*/*/toolchain/` directories (md5-guarded: refuse to
  delete any file that does not match its board's canonical hash).
- The documented command becomes, from any example directory:
  `cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake`
  (rt1062: the rt1062 file + `-DEVKB_BOARD=rt1062`, build dir `build-rt1062`).
  **Verified by probe:** CMake resolves the relative path against the source
  directory and caches the absolute path — the same reach-up-three-levels
  shape as the `include(…/../../../evkb.cmake)` idiom every example uses.
- `sd_test` and `sd_wav_play_test`: the inlined toolchain block (from the
  `TEENSY_VERSION` line through the `CMAKE_CXX_LINK_EXECUTABLE` line,
  including the buggy duplicated `ARM_TOOLCHAIN_BIN` if-block) is replaced by
  `include(${CMAKE_CURRENT_LIST_DIR}/../../../toolchain/rt1170-evkb.toolchain.cmake)`
  placed BEFORE `project()`. Plain `cmake -B build` keeps working — identical
  mechanism, content now sourced once.
- Update the 3 scripts and 5 doc snippets to the new path. `driftrun.sh` uses
  an absolute `$EXDIR`-relative form — it gets the repo-root equivalent.
- Add one migration note to README.md and CLAUDE.md: existing `build*/` dirs
  cached an absolute toolchain path this change deletes; their elfs stay
  valid (gates run them unchanged), but the first reconfigure fails with
  "toolchain file not found" — `rm -rf` the build dir and configure fresh.

## 3. Out of scope

The board-agnostic single file. Any content change inside the toolchain
files. CMake presets. The macros repo (its tests use their own toolchain).
The licence audit (depfiles never reference toolchain files — verified by the
REPOS coverage check landing green with no toolchain paths in any manifest).

## 4. Verification

| Check | Expectation |
|---|---|
| Pre-change baseline: fresh builds of blink, serial_test (both boards), sd_test, sd_wav_play_test; stash hexes | builds green (content identical to pushed state) |
| Post-change fresh builds of the same five via the NEW command (and plain `cmake -B build` for the inline pair) | hexes byte-identical to baseline |
| `grep -r "toolchain/rt1" examples --include=*.cmake --include=CMakeLists.txt` (build dirs pruned) | zero hits — no per-example references survive |
| One `tools/uacvalidate/corpus/run_case.sh`-style configure + `tools/driftrun.sh` path check | updated scripts configure successfully |
| `./tools/run-all-qemu-gates.sh -l` | still `(89 gate(s))` |
| Existing elfs + sweep | untouched — spot-run one gate per board on the PRE-EXISTING build dirs to prove stale elfs still gate green |
| `license-audit.sh` + the three test suites | pass unchanged |

## 5. Sequencing

Single repo, single commit stream: baseline hexes → add root files → update
the 2 inline examples + 3 scripts → delete the 104 dirs (md5-guarded) → docs
+ migration note → verification table. No pushes until the user approves.
