# GC355/VGLite conformance probe + quirks reference — design

Date: 2026-08-30
Status: approved (brainstorm 2026-08-30)
Tracking: Linear **NEW-32** (https://linear.app/newdigate/issue/NEW-32).
Related: NEW-31 (LVGL sw floor) — the other half of "what bounds rendering
on this board"; independent of this work.
Prior art this exists because of: NEW-20 (rotary GPU compositor) and NEW-23
(fader GPU compositor) — between them roughly a dozen GC355 quirks, each
found the expensive way.

## 1. Why

Every GC355 defect this tree has hit shares one property: **the driver
reported success**. `vg_lite_*` returned `VG_LITE_SUCCESS`, the error
counters read zero, and the picture was wrong — or the front end hung. The
knowledge is currently spread across ★ comments in two compositors, two
Linear issues, and one memory file, and none of it is re-checkable after an
SDK re-vendor or driver bump.

Known quirks, all measured, none currently probeable:

| quirk | how it presented |
|---|---|
| a path renders only its FIRST contour | ticks vanished; a ring filled solid; per-boot-varying checksums |
| legacy `vg_lite_draw_grad` is GC255-only | solid black fills |
| EXT gradient ramp is placement-dependent (and `update` overwrites both matrix and line, leaking the old ramp) | a moving widget cannot cache a ramp |
| `vg_lite_color_t` is ABGR, ramp image words are ARGB | near-invisible on greys, wrong on any theme colour |
| command buffer must be 64-byte aligned | front end hangs, every call SUCCESS |
| target must be `vg_lite_map`'d | every draw "succeeds" and changes nothing |
| `vg_lite_set_grad` returns SUCCESS with `count=0` | silently substitutes a black→white ramp |
| `compute_interpolation_steps` pushes uninitialised stack on a singular matrix, returns SUCCESS | boot-varying garbage in the command buffer |
| scissor left/top only clamps when `ts_is_fullscreen == 0` | clipping silently disabled if the tess buffer covers the screen |
| SRC_OVER of AA paths is not idempotent | double-composited edges drift |
| Bezier disc AA rotates with the matrix | a delta render differs from a fresh one |
| waits could consume stale IRQ flags | a "successful" wait that never waited |

## 2. Goal

One re-runnable instrument that answers "which vg_lite features behave
correctly on this GC355", and one reference document that records the
answer with its evidence and the safe usage. Then a guard layer over the
traps that have a correct usage.

## 3. Non-goals

- No patching of vendored VGLite driver source. The port layer
  (`VGLite/port/`) is ours and may grow a guard header; `VGLite/VGLite/` is
  NXP's and stays pristine (re-vendor liability, and this tree's vendoring
  discipline is deliberately strict). Driver bugs that cannot be avoided
  from outside are documented as "avoid", not fixed.
- No performance benchmarking. NEW-20's bench owns that question; this probe
  answers correctness only.
- No new widget or rendering feature.
- No attempt to make the two engines (LVGL-sw and GC355) agree pixel-wise —
  two golden sets, never reconciled, remains the rule.

## 4. Architecture — a case table with two verdicts

New example `examples/display/vglite_conformance`, sibling to
`display/vglite_probe` (which answers the prior question: does the GPU
initialise and render at all).

Each case is a `{ id, run, check }` triple:

- **`id`** — a stable slug, e.g. `path/multi-contour-disjoint`. Stable
  because the expectations file (§7) is keyed on it.
- **`run`** — clears the scratch target, issues the vg_lite calls under
  test, `vg_lite_finish()`es, and returns the accumulated API status.
- **`check`** — reads the scratch pixels with the CPU and answers ONE
  structural question, returning a verdict plus a short numeric detail.

Cases run one at a time into a single scratch buffer (clear → draw → finish
→ check → report). No grid layout, so cases can be added without arithmetic,
and a case cannot contaminate its neighbour.

**The scratch target** is a 128×128 `VG_LITE_BGRA8888` buffer in EXTMEM,
`vg_lite_map`'d once at start-up. EXTMEM because the GPU reaches it as a bus
master exactly as it reaches the framebuffer, and because the core never
enables the D-cache, so a CPU read after `vg_lite_finish()` sees the GPU's
pixels with no maintenance. The panel is NOT involved: no scanout, no flip,
no LVGL.

**Two verdicts per case, always both printed:**

```
vgc case=<id> api=<success|error:NNN> pixel=<ok|broken|skip> detail=<k=v,...>
```

`api=success pixel=broken` is the cell this whole exercise exists to
populate — it is where every defect we have hit lives. Printing both axes on
every case is the API-honesty sweep; it is not a separate section.

Trailing summary line:

```
vgc_summary engine=<gpu|absent> cases=<n> ok=<n> broken=<n> skip=<n> dangerous=<on|off>
```

## 5. Predicates — structural, never checksums

A predicate must be independent of antialiasing and of engine, or it decays
the way a golden would. Each reads a handful of pixels and answers a
yes/no about geometry or colour relationships. Initial set (Phase 1 unless
noted):

**Paths, contours & winding**
- `path/single-contour-rect` — baseline: the rect is filled. If THIS is
  broken nothing else in the matrix means anything.
- `path/multi-contour-disjoint` — four separated bars in ONE path; count
  filled runs down a column, expect 4. (Expected BROKEN: GC355 gives 1.)
- `path/two-contour-ring-nonzero` — outer + reversed inner in one path;
  expect rim filled AND centre background. (Expected BROKEN: fills solid.)
- `path/two-draws-ring` — the same ring as a filled plate with an inset
  plate over it, two draws; expect rim + hole. The safe-usage control.
- `path/evenodd-vs-nonzero` — one nested path drawn under each fill rule;
  expect the hole to appear per the rule.
- `path/self-intersecting` — a bowtie; expect the winding rule's answer,
  and stability (see repeat, below).
- `path/format-s8|s16|s32|fp32` — the same triangle in each path format;
  expect equal filled-pixel counts across all four.
- `path/degenerate-zero-area` — a zero-height rect; expect no crash and a
  defined outcome (either nothing drawn or a hairline — both acceptable,
  the point is that it is recorded).

**Gradients & colour** (Phase 2)
- `grad/legacy-linear` — `vg_lite_draw_grad`; expect a monotonic ramp with
  distinct endpoints. (Expected BROKEN on GC355: GC255-only API.)
- `grad/ext-linear-static` — EXT API used exactly per NXP's
  `vglite_layer.c` ordering (set → matrix → update → draw, never touched
  after); expect a correct ramp.
- `grad/ext-linear-moved` — the same ramp object reused at a second
  position WITHOUT re-update; expect BROKEN, documenting the
  placement-dependence directly rather than by inference.
- `grad/ramp-word-order` — a pure-red ramp stop; expect the rendered pixel
  to be red, catching an ABGR/ARGB swap by construction.
- `color/solid-word-order` — a pure-red solid fill; the companion control.
- `color/premultiplied-srcover` — 50 % white over black; expect ≈128, not
  64 or 255.
- `blend/modes` — each supported blend mode over a known backdrop; expect
  the documented arithmetic, one row per mode.

**Images/blits & scissor** (Phase 3)
- `blit/basic` — blit a small 2-colour image; expect the pattern to land.
- `blit/stride-64` and `blit/stride-unaligned` — the 64-byte source-stride
  rule, stated in the rotary's comments but never tested here.
- `blit/formats` — the formats we might plausibly use, each pass/fail.
- `scissor/basic` — an oversized rect under a scissor; expect pixels
  outside untouched on all FOUR edges (left/top is the interesting pair).
- `scissor/tess-fullscreen` — the same with a tessellation buffer ≥ the
  target, which is the condition under which the driver stops clamping
  left/top; expect BROKEN and pin the precondition.

**Cross-cutting**
- Every case runs **twice** and reports `repeat=same|differs`. Per-boot and
  per-run nondeterminism is a first-class GC355 symptom (7 boots, 7
  checksums on the fader), so a case that renders differently on a second
  identical run is a finding in its own right, independent of whether the
  first render was correct.

## 6. Dangerous cases

Two known traps HANG the front end rather than failing: a misaligned
command buffer, and (suspected) unterminated path data. A hang would cost
the whole matrix and a bench cycle.

Such cases are compiled only under `-DVGC_DANGEROUS=1`, never in the default
build, and each prints its `case=` line BEFORE issuing the call so a hang is
attributable from the transcript. `vgc_summary … dangerous=off` records
which build produced a matrix. The port's bounded waits (proved by
`vglite_probe`) are what make even the opt-in build survivable in principle,
but the default matrix must never depend on that.

## 7. Gate, and how the silicon answer stays honest

**QEMU** has no GC355. The gate (`run_qemu.sh`) asserts the honest negative:
`vgc_engine=absent`, a non-zero case count all reporting `pixel=skip`, and a
**tripwire that no case may report `pixel=ok`** — the same discipline as
`vglite_probe` and the knob/fader engine tripwires. Sweep 123 → 124, with
the `license-audit.sh` GATES entry and vacuity cases that must be
demonstrated RED.

**Silicon** produces the matrix in ONE boot, committed as
`transcript_hw_evkb.txt`. To stop that answer rotting:

- `expected_silicon.txt` — the committed expectation, one `id verdict` line
  per case, with a one-line reason for every `broken`.
- `tools/vglite-conformance-check.sh <transcript>` — diffs a fresh
  transcript against it and fails on ANY drift, in either direction. A
  quirk that silently *disappears* after an SDK bump matters as much as a
  new one appearing: it means the safe usage we built can be simplified,
  and more importantly that the driver changed under us.

This is the re-runnable half: after a VGLite re-vendor, one boot plus one
diff says what moved.

## 8. Guard layer (Phase 4, deliberately last)

`VGLite/port/vglite_guard.h` — our port code, not vendored. It guards only
what the probe CONFIRMED, which is why it comes last rather than first:

- a path builder that enforces **one contour per path** (asserts a single
  `VLC_OP_MOVE`, refuses a truncated path, requires the `VLC_OP_END`);
- a checked `init_path` wrapper;
- gradient helpers that enforce the EXT ordering, or refuse the API
  outright if the probe confirms it unusable for moving geometry;
- a shared `VGLITE_GUARD_TRY` error-counting macro to replace the
  copy-pasted `GPU_TRY` in both compositors.

Both existing compositors (`synthui_rotary_knob_gpu.cpp`,
`synthui_fader_gpu.cpp`) are then retrofitted onto it, and their goldens
must not move — that is the acceptance test for the guard layer, and it is
a strong one: two widgets, both engines, QEMU and silicon.

## 9. The reference document

`docs/gc355-vglite-quirks.md`, beside the QEMU peripheral status table, with
one row per feature: **verdict · safe usage · evidence**. Every row cites
the case id that establishes it, so a claim without a probe case is visibly
a claim without evidence. Cross-linked from CLAUDE.md's VGLite section, and
from `VGLite/README.md`.

## 10. Phasing — one bench boot each

1. **Harness + paths/contours/winding**, gate, silicon matrix, doc skeleton.
   Highest value: it is the area that produced the one-contour rule.
2. **Gradients & colour** cases.
3. **Images/blits & scissor** cases — unexplored territory NEW-20's bitmap
   and filmstrip strategies would need before they could be built on GPU.
4. **Guard layer** + both compositors retrofitted, goldens unmoved.

Each phase adds cases to the existing harness, extends the doc, and re-runs
one boot. Phases 2 and 3 are independent of each other; 4 depends on 1–3.

## 11. Risks

- **A case hangs the GPU in the default build.** Mitigated by §6, and by
  printing each `case=` line before the call so a hang names itself.
- **A predicate is wrong rather than the hardware.** Every "expected
  BROKEN" case has a paired safe-usage control (e.g.
  `path/two-draws-ring` beside `path/two-contour-ring-nonzero`); if the
  control also fails, the harness is suspect, not the silicon.
- **Scope creep into benchmarking.** Explicitly out (§3); NEW-20 owns it.
- **The expectations file becomes a rubber stamp** — updated to match
  whatever the board printed. Same hazard as re-goldening a checksum: every
  change to `expected_silicon.txt` must carry a reason line, and a verdict
  flipping to `ok` must be explained (driver change? our usage changed?)
  rather than merely accepted.
