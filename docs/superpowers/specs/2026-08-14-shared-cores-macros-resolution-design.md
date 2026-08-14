# Shared cores/macros resolution — Design

**Goal.** Stop carrying `cores/` and `teensy-cmake-macros/` as nested checkouts
inside this repo. Both become ordinary sibling checkouts under one configurable
root (`TEENSY_LIB_ROOT`, default `$HOME/Development`), resolved exactly like
the 20 peripheral libraries: local-first, pinned-git fallback. Worktrees and
fresh clones resolve identically; nothing about the built firmware changes.

**Why now.** The two build-system dependencies are the only two that do NOT
follow the `~/Development` convention: `evkb.cmake:31` looks for the macros at
`${EVKB_ROOT}/teensy-cmake-macros` and `:69` for the core at
`${EVKB_ROOT}/cores/<subdir>`, while `~/Development/teensy-cmake-macros` and
`~/Development/teensy-cores` sit at the SAME SHAs (`e948da4`, `5bcae78`),
ignored. Consequences, all measured on 2026-08-14:

- **Worktrees fetch from the network.** Untracked dirs don't materialise in
  `git worktree` checkouts (`.claude/worktrees/pensive-wright-ef912f/` has
  neither), so every worktree build re-fetches both.
- **The macros dodge the CPM cache.** They're fetched with plain
  `FetchContent` (`evkb.cmake:30-38`), which ignores `CPM_SOURCE_CACHE` — the
  one dependency EVERY example needs is the one the documented cache advice
  doesn't cover. 123 `_deps` dirs under `examples/`, 2.0 GB.
- **Two checkouts can drift.** In-repo and `~/Development` copies agree today
  by discipline, not mechanism.
- **An unpinned fetch is one deletion away.** The macros' own
  `if(NOT DEFINED COREPATH)` branch (`CMakeLists.include.txt:13-21`) fetches
  `newdigate/teensy-cores @ master` — unpinned, per build dir. evkb never
  hits it only because every toolchain file happens to set `COREPATH` first.
  The macros' own 5 tests set no `COREPATH`, so ALL of them go through it.

---

## 1. Decisions

| Decision | Choice | Rejected |
|---|---|---|
| End state on disk | **Single checkout under `$HOME/Development`; in-repo copies deleted** | keep in-repo with fallback (drift risk); env-var-over-in-repo (surprising precedence) |
| Configuration | **One root var, `TEENSY_LIB_ROOT`** (env → cache → `$HOME/Development`) | per-dependency vars; both layered (unneeded knobs) |
| Where the generic layer lives | **`teensy-cmake-macros`** — staged, macros first | all in `evkb.cmake` (leaves the unpinned-master bug for every other consumer; manifest layer in the wrong repo) |
| Fresh-clone fallback | **Keep pinned-git fetch**: cores + 20 libs via CPM (cached); **macros via plain FetchContent** | hard prerequisite (kills the `EVKB_FORCE_FETCH` reproducibility property); macros via CPM (see below) |
| CPM dual-pin drift | **Delete the seam**: the ONE CPM pin stays in the macros; evkb pins no third-party bootstrap | licence-audit pin check (wrong gate meaning, vacuous when macros absent); configure-time version check (invariant still exists) |
| `COREPATH` ownership | **`evkb.cmake` is the single owner**; all 106 setter sites stripped | leave toolchain guesses pointing at a deleted directory |
| Licence audit | **Audit reads the same `TEENSY_LIB_ROOT`; `ALLOW` goes root-independent** | resolved-paths manifest from CMake (couples part 1 to a build); repoint hardcoded paths (re-hides the coupling) |

**A decision walked back during design:** macros-via-CPM was chosen first (for
cache coverage), then reverted on noticing it forces `evkb.cmake` to bootstrap
CPM itself — duplicating the CPM version+SHA256 pin across two repos, a seam
that must be policed forever. Plain FetchContent needs no third-party code, so
the evkb side pins nothing. Cost: fresh users re-clone the macros per build dir
— measured **456K including `.git`** (the 2.0 GB problem is CMSIS-DSP and
friends, which DO go through CPM and get cached). Note the version half of that
seam self-catches (`EXPECTED_HASH` fails hard on a wrong SHA256); only the
version string could drift silently, and now nothing duplicates it.

---

## 2. Stage 1 — the generic layer in teensy-cmake-macros

Additive; no existing consumer behaviour changes. New API:

```cmake
# TEENSY_LIB_ROOT: cache var; initialised from $ENV{TEENSY_LIB_ROOT},
# else "$ENV{HOME}/Development". All declared libraries resolve under it.

teensy_declare_library(NAME <subdir-under-root> URL REF PATH)
teensy_resolve_library(NAME OUT_VAR)     # memoized; local-first; CPM fallback
teensy_import_library(NAME [subdirs...]) # resolve + import_arduino_library
```

- `teensy_resolve_library` is a manifest-keyed wrapper over the existing
  `resolve_arduino_library_auto` with local path
  `${TEENSY_LIB_ROOT}/<subdir>`; `TEENSY_FORCE_FETCH=ON` skips the local
  check (the generic form of `EVKB_FORCE_FETCH`).
- Root-relative subdirs express every current entry, including the odd ones
  (`PaulS_SD`, `CMSIS_6`, `FNET/src`, `Bounce2/src`, and now
  `teensy-cores/<core-subdir>`). Absolute local paths are deliberately not
  supported — YAGNI.
- **The `NOT DEFINED COREPATH` fallback is rewritten** to resolve
  `teensy-cores` through this same path: `$TEENSY_LIB_ROOT/teensy-cores` if
  present, else CPM at a **pinned SHA** (today's `5bcae78`), cached — replacing
  the unpinned per-build-dir `master` clone. This pin serves non-evkb
  consumers (including the macros' own tests); evkb always defines `COREPATH`
  before including the macros, so evkb builds never exercise it and it is NOT
  a drift seam for this tree.
- `resolve_arduino_library_auto` / `import_arduino_library_auto` /
  `import_arduino_library_git` stay untouched (back-compat).

**Proof for Stage 1:** the 5 tests under `tests/` pass with no `COREPATH` set,
fetching the core at the pinned SHA through the cache — they are the exact
consumers the unpinned-master bug affects today.

## 3. Stage 2 — evkb.cmake reduced to what is evkb's

Keeps: the macros bootstrap (irreducible — the macros can't resolve
themselves), the 21-entry manifest (cores + 20 libraries) with its pinned
SHAs, the
`EVKB_BOARD`→core-subdir axis, the CMSIS-DSP/LVGL/audio-owner helpers.
Configure order:

1. Compute `TEENSY_LIB_ROOT` (env else default) as a cache var BEFORE the
   macros load, so both computations agree by construction. Compute
   `EVKB_BOARD` → `EVKB_CORE_SUBDIR` (unchanged).
2. Set `COREPATH` pre-include: `$TEENSY_LIB_ROOT/teensy-cores/<subdir>/` when
   that exists and `EVKB_FORCE_FETCH` is off, else a named placeholder under
   the build dir. Purpose: suppress the macros' own core resolution so the
   core is fetched once, by the manifest, at evkb's pin — economy now, not
   safety. (This placeholder-then-FORCE dance is today's proven behaviour;
   the toolchain guess currently plays the placeholder.)
3. Fetch + include the macros: `FetchContent`, local `SOURCE_DIR` at
   `$TEENSY_LIB_ROOT/teensy-cmake-macros` if present, else pinned git —
   today's mechanism with the search path moved. Set `TEENSY_FORCE_FETCH`
   from `EVKB_FORCE_FETCH`.
4. Declare the manifest. `_evkb_lib`/`evkb_library_dir`/`import_evkb_library`
   become thin aliases over the Stage-1 API — **no example CMakeLists.txt
   changes**. `cores` becomes an ordinary entry:
   `teensy-cores/${EVKB_CORE_SUBDIR}`.
5. Resolve cores → `set(COREPATH … FORCE)` → `import_arduino_library(cores …)`
   — unchanged from today (`evkb.cmake:303-311`); the FORCE lands before the
   first import bakes link flags, so the placeholder never reaches a build.

## 4. Stage 3 — migration

- **Precondition (verified 2026-08-14, re-verify at execution):** both in-repo
  copies clean and at SHAs present in `~/Development` twins. Then delete
  `cores/` and `teensy-cmake-macros/` from the tree.
- Strip the `COREPATH` guess (and its `_evkb_root` support line) from all
  **106 setter sites**: 97 byte-identical `rt1170-evkb.toolchain.cmake`, 7
  byte-identical `rt1062-evkb.toolchain.cmake`, and the 2 inline-toolchain
  examples (`storage-memory/sd_test`, `audio/sd_wav_play_test`).
  `TEENSY_VERSION`/`CPU_CORE_SPEED`/compiler lines stay. Measured: nothing
  else consumes `${COREPATH}`; ~20 comment-only mentions are updated only
  where the wording becomes wrong.
- **`tools/license-audit.sh`:**
  - `REPOS` (`:23`): `$EVKB/cores` → `$LIB_ROOT/teensy-cores`; the remaining
    `$HOME/Development/*` entries → `$LIB_ROOT/*`, where `LIB_ROOT` reads
    `TEENSY_LIB_ROOT` env else `$HOME/Development`. Getting this wrong makes
    the core silently drop out of the part-1 sweep — the audit would pass by
    measuring less.
  - `ALLOW` (`:76`): drop the `Development/` path component — entries become
    root-INDEPENDENT (`/SPI/SPI\.(h|cpp)$`, `/Wire/Wire\.(h|cpp)$`,
    `/Wire/utility/twi\.(h|c)$`,
    `/LVGL/lvgl/src/libs/thorvg/tvgLottieInterpolator\.cpp$`;
    `cores/teensy4/` already matches `teensy-cores/teensy4/` and the test
    fixtures' `$t/cores/teensy4/`). This is deliberately slightly WIDER than
    today (a vendored `SPI/SPI.cpp` inside another swept repo would now
    match) — acceptable because `ALLOW` only applies inside `REPOS`
    directories and part 2's EMPTY-object rule independently backstops every
    entry, the same argument the existing allowlist comments already make.
    Chosen over a `LICENSE_AUDIT_ALLOW` test hook, which would let the tests
    stop pinning the real allowlist.
  - `GATES` untouched (entries name example dirs, not core paths).
  - **`license-audit.test.sh` needs no changes** — the tests drive `REPOS`/
    `EVKB` via existing hooks and their fixtures still match `ALLOW`.
- **Docs:** `CLAUDE.md` git-layout + local-first bullets; `README.md`
  prerequisites + `:65` cache advice (now: covers every fetched repo EXCEPT
  the macros — 456K via FetchContent per build dir, deliberate);
  `examples/README.md:22`; macros `README.md` gains `TEENSY_LIB_ROOT` and the
  new API.
- **Deliberately NOT done:** no `.gitignore` entries for the deleted dirs — a
  reappearing `?? cores/` must stay visible as the "stale in-repo copy"
  signal. No hardware run (nothing about the firmware changes; the hex oracle
  below is the proof). No toolchain-file dedupe (separate concern, deferred).

## 5. Sequencing across repos

Local-first makes both working trees co-develop; pins bump at the end:

1. Stage 1 in `~/Development/teensy-cmake-macros`; its 5 tests green.
2. Stage 2+3 in evkb against that working tree (Stage 2's rewritten search
   path already lands on `~/Development`; Stage 3's deletion removes the
   then-dead in-repo copies).
3. Full verification (below) with everything local.
4. Push macros → bump the macros pin (`evkb.cmake:36`) → re-verify fetch mode.
5. Push evkb.

## 6. Verification

| Check | Expectation | What a miss means |
|---|---|---|
| **Hex oracle**: `cmp` of pre/post `.hex` for a spread of examples (≥1 rt1062 build, ≥1 CM4-image example, ≥1 CPM-fetched-lib example, the 2 inline-toolchain ones) | **byte-identical** | something resolved to different sources — chase it, never wave it through |
| Build every gate-owning example, then `./tools/run-all-qemu-gates.sh` | **89 passed, 0 failed, 0 SKIP** (88/1 tolerated only for `rt1176:dualcore/cm4_audio_test`, re-run idle) | any SKIP = a configure broke — the likeliest failure mode of this change |
| `tools/license-audit.sh` | `LICENSE-AUDIT: PASS`, rt1062 entries walked (136 + 203 dep paths) | core dropped from the sweep, or `ALLOW` rework broke matching |
| `license-audit.test.sh`, `gate-lib.test.sh`, `gate-vacuity.test.sh` | pass **unchanged** | the `ALLOW`/`REPOS` rework weakened what the tests pin |
| Macros' 5 tests (Stage 1) | pass, no `COREPATH`, core at pinned SHA via cache | the fallback rewrite regressed the macros' own consumers |
| Worktree probe: configure one example inside a fresh worktree | resolves via `$TEENSY_LIB_ROOT`, **no network** | the original complaint isn't actually fixed |
| `EVKB_FORCE_FETCH=ON` after the pin bump (step 4) | builds; cores + libs arrive via CPM at PINNED SHAs into `CPM_SOURCE_CACHE`; macros via FetchContent (the documented exception) | fresh-user mode broken, or an unpinned fetch survived |

## 7. Out of scope

Toolchain-file dedupe (97 identical copies → one shared file; changes the
documented build command everywhere). Per-dependency override vars. Hard-
prerequisite mode. Any behaviour change to what gets compiled or linked.
