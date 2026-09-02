# GC355 guard layer (NEW-32 Phase 4) — implementation plan

> **For agentic workers:** steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `VGLite/port/vglite_guard.h` — a header that makes the GC355 defects
the conformance probe **measured** unrepresentable in new code, and retrofit
both shipping compositors onto it without moving a single golden.

**Architecture:** one header, split into a **pure** half (a path-data validator
that needs nothing but `<stdint.h>`) and a **driver** half (a checked
`vg_lite_init_path` wrapper plus the shared error-counting macro). The split is
the point: the pure half is host-testable, and *no gate in this tree can see
GPU code* — the QEMU gates all run the software engine.

**Tech stack:** C99 header, ARM GCC 10 on target, host `cc` for the tests.

---

## What the guard may and may not enforce

It enforces **only what the probe confirmed**. That ordering is the whole
reason this phase came last, so the rule is worth stating as a constraint
rather than a preference:

| Measured fact | Probe case | Guard enforces |
|---|---|---|
| Disjoint contours in one path are dropped (`runs=1` of 2 and of 4) | `path/multi-contour-disjoint`, `path/two-disjoint-bars` | one `VLC_OP_MOVE` per path |
| Nested contours render correctly but **nondeterministically** | `path/four-nested-rings`, `path/evenodd-vs-nonzero` | same rule, second independent reason |
| Hole-cutting renders mis-cover in both directions | `path/two-contour-ring-nonzero` (`short:769`), `four-nested-rings` (`stray`) | same rule |
| One contour per path is **exact** | `path/two-draws-ring` (`fill=5376`) | the prescribed construction |
| Unterminated path data hangs the front end while the API returns SUCCESS | Phase 1 | require a trailing `VLC_OP_END` |
| `SRC_OVER` is the premultiplied operator | `color/premultiplied-srcover` | *see below — stays in SynthUI* |

**Deliberately NOT built: gradient helpers.** Design spec §8 conditions them on
*"if the probe confirms it unusable for moving geometry"*. **The probe never
tested gradients** — Phase 2 was redirected to colour and blend after scoping
found the matrix had never exercised the blend mode production uses. The
gradient claims in `docs/gc355-vglite-quirks.md` come from *reading NXP's
source*, not from a boot. Building helpers on them would encode an unmeasured
belief into the very layer whose ordering exists to prevent exactly that. This
is recorded as a gap, not an omission.

**The colour helper stays in SynthUI** (`src/synthui_fader_color.h`) rather than
moving into the guard. It is host-testable there with no VGLite dependency;
moving it would make its 69017-check test depend on the driver, and duplicating
it would create two copies of a measured constant. The guard points at it.

---

## File structure

- **Create** `VGLite/port/vglite_guard.h` — the guard. Pure half always
  available; driver half behind `VGLITE_GUARD_NO_DRIVER` so tests can compile
  the validator alone.
- **Create** `VGLite/tests/vglite_guard_test.c` + `VGLite/tests/run.sh` — host
  suite. VGLite has no tests directory today; this creates it.
- **Modify** `evkb.cmake` — add `${dir}/port` to VGLite's PUBLIC include dirs
  (only `port/baremetal` is on the path today).
- **Modify** `SynthUI/src/vglite/synthui_fader_gpu.cpp` and
  `synthui_rotary_knob_gpu.cpp` — retrofit.

---

### Task 1: The pure validator

**Files:** Create `VGLite/port/vglite_guard.h`

- [ ] **Step 1: the status enum and the walker.**

`vglite_guard_check_path(const int32_t *w, size_t n)` walks S32 path words.
Each opcode is **one byte at the base of a word** (Phase 1 finding — the plan
that assumed otherwise fabricated two BROKENs), followed by a fixed operand
count: `MOVE`/`LINE` 2, `QUAD` 4, `CUBIC` 6, `CLOSE`/`END` 0.

Returns one of: `VGLITE_GUARD_OK`, `_EMPTY`, `_NO_MOVE`, `_MULTI_MOVE`,
`_TRUNCATED`, `_NO_END`, `_BAD_OPCODE`, `_TRAILING`.

`_MULTI_MOVE` is the one that matters — it is the measured defect. `_TRAILING`
(data after the `END`) is separate from `_NO_END` because they are different
bugs: one is an unterminated path, the other a path that ends early and leaves
garbage the parser may still walk into.

- [ ] **Step 2: `vglite_guard_strerror()`** so a caller can print a name
  rather than a number. A guard that reports `3` teaches nobody.

### Task 2: Host tests, with the negative arms

**Files:** Create `VGLite/tests/vglite_guard_test.c`, `VGLite/tests/run.sh`

- [ ] **Step 1: positive arms** — a real `emit_rect` word sequence, a rounded
  rect with cubics, a keyhole ring (the rotary's single-contour construction).
  All must be `OK`.
- [ ] **Step 2: the negative arms, one per status.** Every status must be
  produced by a test, or its branch is unexecuted and the guard could be
  hard-wired to `OK` — the lesson this phase has already been taught three
  times (stray-ink arm, pinned blend reading, the premultiply pin-fires arm).
- [ ] **Step 3: the load-bearing arm** — two `emit_rect`s concatenated (exactly
  what the fader did before NEW-23) must return `_MULTI_MOVE`.
- [ ] **Step 4: run, and demonstrate RED** by hard-wiring the validator to
  `OK` and confirming the suite fails by name.

### Task 3: The driver half

**Files:** Modify `VGLite/port/vglite_guard.h`

- [ ] **Step 1: `VGLITE_GUARD_TRY(call, errctr)`** — replaces the two
  copy-pasted `GPU_TRY`s verbatim in behaviour (`if != VG_LITE_SUCCESS,
  errctr++`). Behaviour-identical by design: this retrofit must not move a
  golden, so the macro may not gain cleverness.
- [ ] **Step 2: `vglite_guard_init_path(...)`** — validates, and on failure
  returns false **without** calling `vg_lite_init_path`, so a bad path is never
  handed to the driver.

### Task 4: Retrofit, and prove nothing moved

**Files:** Modify both compositors, `evkb.cmake`

- [ ] **Step 1:** add `${dir}/port` to the include dirs.
- [ ] **Step 2:** route both `finish_path`s through the checked wrapper and
  both `GPU_TRY`s through `VGLITE_GUARD_TRY`.
- [ ] **Step 3: QEMU acceptance** — `synthui_fader_test`, `synthui_knob_test`,
  `acid_box`, `vglite_lvgl_test` must all PASS with **unmoved** goldens.
- [ ] **Step 4: silicon acceptance** — both widgets, GPU engine, goldens
  unmoved: fader `fd_crc=0x814F4047` / `delta==fresh=0xE9A9A2B5`, knob per its
  transcript. **Needs hand-pressed SW4 boots.**

**The acceptance test is that nothing changes.** A guard that alters a rendered
pixel has changed behaviour, not constrained it.
