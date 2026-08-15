# SynthUI Repo Creation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Create the local-only `~/Development/SynthUI` sibling library repo, seeded with the curated DC component reference set and the recovered ReBirth material, per the approved spec.

**Architecture:** A new git repo (branch `master`, no remote) in the Teensy-sibling-library shape: metadata files at the root, two provenance-separated reference areas (`reference/dc/` = the user's own DC set with its flat layout preserved; `reference/rebirth/` = recovered mod-archive art), and deliberately no `src/` until the first widget lands. One initial commit.

**Tech Stack:** git, POSIX shell (cp/shasum). No compilers, no LVGL yet.

**Spec:** rt1176-evkb `docs/superpowers/specs/2026-08-15-synthui-repo-design.md` (commit 37a9fa0). Read it before starting.

**Constraints from the spec that bind every task:**
- The source directory `/Users/nicholasnewdigate/Development/components` is READ-ONLY throughout — it is a live editor workspace. Never write, move, or delete there.
- `reference/dc/` must keep the flat layout: sheets reference art by bare relative path (`src="crop-fader.png"`), and one sheet uses `src="uploads/strip.png"`.
- Excluded from the copy: `uploads/pasted-*.png` (10 editor droppings), `.thumbnail`.
- Exactly ONE initial commit (spec §6) — do not commit per-task.

---

### Task 1: Repo skeleton and metadata files

**Files:**
- Create: `/Users/nicholasnewdigate/Development/SynthUI/` (git init)
- Create: `/Users/nicholasnewdigate/Development/SynthUI/LICENSE`
- Create: `/Users/nicholasnewdigate/Development/SynthUI/README.md`
- Create: `/Users/nicholasnewdigate/Development/SynthUI/library.properties`
- Create: `/Users/nicholasnewdigate/Development/SynthUI/.gitignore`

- [ ] **Step 1: Snapshot the source directory (the "untouched" oracle for Task 4)**

```bash
find /Users/nicholasnewdigate/Development/components -type f -print0 | xargs -0 shasum > "$SCRATCHPAD/components-before.txt"
wc -l "$SCRATCHPAD/components-before.txt"
```

`$SCRATCHPAD` is the session scratchpad directory. Expected: ~45 lines (32 top-level files incl. `.thumbnail`, plus uploads/). The exact number matters less than the file being reproducible in Task 4.

- [ ] **Step 2: Verify the target does not already exist, then init**

```bash
test ! -e /Users/nicholasnewdigate/Development/SynthUI && echo CLEAR
git init -b master /Users/nicholasnewdigate/Development/SynthUI
```

Expected: `CLEAR`, then `Initialized empty Git repository in /Users/nicholasnewdigate/Development/SynthUI/.git/`. If `CLEAR` does not print, STOP — do not overwrite; report to the user.

- [ ] **Step 3: Write LICENSE (MIT)**

Write exactly:

```text
MIT License

Copyright (c) 2026 Nicholas Newdigate

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

- [ ] **Step 4: Write README.md**

Write exactly:

```markdown
# SynthUI

Synth control-surface widgets for LVGL on the i.MX RT1176 / MIMXRT1170-EVKB —
knobs, faders, lamps, LED buttons, level meters, piano keys, seven-segment
displays, toggles — for the synth firmware in the rt1176-evkb tree (acid bass
voice, transport, step sequencer).

Status: reference material only. `src/` arrives with the first widget (the
Knob pilot).

## Layout

- `reference/dc/` — the DC component set: 9 leaf components and 8 sheets
  (`.dc.html`), the generated `dc-runtime` `support.js`, and the sprite art
  the sheets embed. The flat layout is load-bearing: sheets reference art by
  bare relative path (one sheet uses `uploads/strip.png`).
- `reference/rebirth/` — ReBirth RB-338 material recovered from the
  1997–2000 mod archive (single mod `mellow.rbm`): the component guide and
  its companion images.

## Provenance rules

- `reference/` is design reference. It is **never compiled**.
- Nothing under `reference/` may be converted into C arrays, fonts, or
  sprite data in `src/` without an explicit rights decision recorded here
  first. This bites hardest for `reference/rebirth/`: recovered community
  mod art, rights unclear.
- Clean-room vector rebuilds from the guide's *written descriptions* are the
  intended path — the same firewall discipline as the rt1176-evkb tree's
  license audit.
- The MIT LICENSE covers this repo's own content: the DC component set,
  `support.js`, and all future `src/` code. It does not speak for
  `reference/rebirth/`.

## Relationship to the evkb tree

Sibling library under `$TEENSY_LIB_ROOT` (default `~/Development`), like
`LVGL`, `Audio`, and `MipiDisplay`. Local-only for now: no remote, and no
`evkb.cmake` import macro until the first consuming example.

Design spec: rt1176-evkb
`docs/superpowers/specs/2026-08-15-synthui-repo-design.md`.
```

- [ ] **Step 5: Write library.properties**

Modeled on the `LVGL` sibling's. Write exactly:

```text
name=SynthUI
version=0.1.0
author=Nicholas Newdigate
maintainer=Nicholas Newdigate
sentence=Synth control-surface widgets for LVGL on the NXP i.MX RT1176.
paragraph=Reference-first library: the DC component set (knob, fader, lamps, LED buttons, level meter, piano keys, seven-segment, toggles) and the recovered ReBirth RB-338 guide, ahead of clean-room LVGL widget implementations in src/.
category=Display
url=https://github.com/newdigate/SynthUI
architectures=*
```

(`url=` names the intended future home; nothing is pushed — spec §6.)

- [ ] **Step 6: Write .gitignore**

Write exactly:

```text
build/
.DS_Store
```

- [ ] **Step 7: Verify skeleton**

```bash
ls -A /Users/nicholasnewdigate/Development/SynthUI
```

Expected: exactly `.git`, `.gitignore`, `LICENSE`, `README.md`, `library.properties`. No commit yet (single-commit rule).

### Task 2: Copy the DC component set into reference/dc/

**Files:**
- Create: `/Users/nicholasnewdigate/Development/SynthUI/reference/dc/` (29 files + `uploads/strip.png`)

- [ ] **Step 1: Copy with globs that structurally exclude the droppings**

```bash
SRC=/Users/nicholasnewdigate/Development/components
DST=/Users/nicholasnewdigate/Development/SynthUI/reference/dc
mkdir -p "$DST/uploads"
cp "$SRC"/*.dc.html "$DST"/
cp "$SRC/support.js" "$DST"/
cp "$SRC"/c-*.png "$SRC"/crop-*.png "$DST"/
cp "$SRC/uploads/strip.png" "$DST/uploads/"
```

The globs cannot match `.thumbnail` (dotfile) or `uploads/pasted-*.png` (never globbed); `strip.png` is copied by name only.

- [ ] **Step 2: Verify counts and byte-for-byte parity**

```bash
SRC=/Users/nicholasnewdigate/Development/components
cd /Users/nicholasnewdigate/Development/SynthUI/reference/dc
ls *.dc.html | wc -l          # expected: 17
ls *.png | wc -l              # expected: 10
(cd "$SRC" && shasum *.dc.html support.js c-*.png crop-*.png uploads/strip.png) | shasum -c - | grep -v ": OK$" | wc -l
```

Expected: `17`, `10`, then `0` (every checksum line is `: OK`, so the filtered count is zero). Any other output = a copy defect; fix before proceeding.

### Task 3: Copy the ReBirth material into reference/rebirth/

**Files:**
- Create: `/Users/nicholasnewdigate/Development/SynthUI/reference/rebirth/` (3 files)

- [ ] **Step 1: Copy the three files by name**

```bash
SRC=/Users/nicholasnewdigate/Development/components/uploads
DST=/Users/nicholasnewdigate/Development/SynthUI/reference/rebirth
mkdir -p "$DST"
cp "$SRC/rebirth-component-guide.md" "$SRC/rebirth-components.png" "$SRC/rebirth-palette.png" "$DST/"
```

- [ ] **Step 2: Verify parity**

```bash
SRC=/Users/nicholasnewdigate/Development/components/uploads
DST=/Users/nicholasnewdigate/Development/SynthUI/reference/rebirth
(cd "$SRC" && shasum rebirth-component-guide.md rebirth-components.png rebirth-palette.png) | (cd "$DST" && shasum -c -)
```

Expected: three lines, each ending `: OK`.

### Task 4: Whole-repo verification (spec §8, before the commit)

**Files:** none (checks only)

- [ ] **Step 1: Sheet references all resolve inside the copy**

```bash
cd /Users/nicholasnewdigate/Development/SynthUI/reference/dc
grep -ho 'src="[^"]*"' *.dc.html | sed 's/^src="//;s/"$//' | sort -u | while read -r f; do
  test -f "$f" || echo "MISSING: $f"
done; echo DONE
```

Expected: only `DONE` — no `MISSING:` lines. This is the automatable form of "the sheets render as they do from the original directory": byte-identical pages whose every `src=` target exists beside them.

- [ ] **Step 2: Droppings did not leak**

```bash
cd /Users/nicholasnewdigate/Development/SynthUI
find . -name "pasted-*" -o -name ".thumbnail" | wc -l
grep -rl "pasted-" . | wc -l
```

Expected: `0` and `0`.

- [ ] **Step 3: The source directory is bit-for-bit untouched**

```bash
find /Users/nicholasnewdigate/Development/components -type f -print0 | xargs -0 shasum > "$SCRATCHPAD/components-after.txt"
diff "$SCRATCHPAD/components-before.txt" "$SCRATCHPAD/components-after.txt" && echo UNTOUCHED
```

Expected: `UNTOUCHED` (empty diff against the Task 1 snapshot).

### Task 5: The single initial commit

**Files:** all of the above, committed

- [ ] **Step 1: Stage and commit everything**

```bash
cd /Users/nicholasnewdigate/Development/SynthUI
git add -A
git commit -m "reference: import the DC component set and the ReBirth guide

Curated from ~/Development/components per the rt1176-evkb spec
docs/superpowers/specs/2026-08-15-synthui-repo-design.md: reference/dc/
preserves the flat browser-viewable layout; reference/rebirth/ quarantines
the recovered mod-archive art behind the README's provenance rules.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

- [ ] **Step 2: Verify the spec §8 end state**

```bash
cd /Users/nicholasnewdigate/Development/SynthUI
git log --oneline | wc -l       # expected: 1
git status --porcelain | wc -l  # expected: 0
git remote -v | wc -l           # expected: 0  (no remote — spec §7)
git ls-files | wc -l            # expected: 36
```

36 = 4 root files (`.gitignore`, `LICENSE`, `README.md`, `library.properties`) + 29 in `reference/dc/` (17 html + `support.js` + 10 png + `uploads/strip.png`) + 3 in `reference/rebirth/`. If any count differs, name the unexpected/missing paths (`git ls-files`) before touching anything.

- [ ] **Step 3: Report**

State plainly: repo created at `~/Development/SynthUI`, one commit, no remote, all five verification counts, and that `~/Development/components` was verified untouched.
