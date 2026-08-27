# synthui_rotary_knob wedge-delta damage — design

Date: 2026-08-27 (evening)
Status: approved (direction confirmed by the user after the measured
prototype; this spec productizes it)
Basis: `2026-08-27-rotary-knob-speed-findings.md` — every number below is
from that session's silicon runs. Follows NEW-20 Phase 2
(`2026-08-27-rotary-knob-widget-design.md`, merged 7324049).

## 1. Goal

When ONLY the angle changes, `synthui_rotary_knob` invalidates the union of
the index wedge's old and new bounding boxes instead of the whole control,
and the GPU compositor composites only inside those boxes. Measured on the
prototype: **180.2 → 44.9 ms/frame (5.5 → 22.3 fps) on the all-16-knob
worst case, gpu engine; 310.2 → 71.3 ms (3.2 → 14.0 fps) sw** — a 4×
speedup from damage geometry alone, with bit-identical full-frame renders.

## 2. Why this is legal, and exactly where it stops being legal

The notch rotor's body disc (r36) and inner disc (r27) are circles centred
on the pivot: **rotationally invariant**. An angle change moves nothing but
the ±8° index wedge (ring sector r16..36). Therefore the exact damage of an
angle change is old-wedge ∪ new-wedge (plus AA padding).

- The well (ring or bounded track) never depends on angle.
- Every OTHER prop change (mode, theme, accent, range, state — including the
  input layer's press/release invalidates) keeps **full-control
  invalidation**, unchanged.
- This is a **notch-variant property**. A future variant whose rotor is not
  rotationally symmetric outside the wedge (facet's shaded triangles) must
  not use the wedge box — the prototype measured exactly that failure
  (facet rows rendered stale triangles outside the clip). The §5 equality
  guard is what catches a future variant getting this wrong.

## 3. Widget core changes (SynthUI, `synthui_rotary_knob.cpp`)

`synthui_rotary_knob_set_angle()`:

1. Compute the wedge bbox at the old and the new angle: 5 samples across the
   16° span at r16·S and r36·S about the widget centre, min/max, padded
   **4 px** for AA (constant — AA is ~1 px regardless of S; the 5-sample
   chord error at the largest supported size is <0.1 px).
2. Append both rects to the instance's `delta_clip[]` (capacity
   `RK_DELTA_MAX = 6` — three angle steps can land between two refreshes;
   the bench animates at 15 ms against a 33 ms refresh, so two per refresh
   is NORMAL, not a corner case) and `lv_obj_invalidate_area()` each.
3. If the array would overflow, or the widget is mid-anything-else:
   **escalate** — `delta_n = 0` and full `lv_obj_invalidate()`. Escalation
   is always correct; delta is only ever an optimization.
4. Every other setter and the input-layer state callbacks: full invalidate
   AND `delta_n = 0` (a pending delta must never survive a full
   invalidation — the compositor decides scissored-vs-full from `delta_n`).

`delta_clip`/`delta_n` live in the private struct (plain `lv_area_t` — the
core stays LVGL-only, no vg_lite types). The sw path needs nothing else:
LVGL clips the unchanged draw callback to the damage, and repainting the
discs inside the old-wedge clip is what erases the old wedge.

## 4. GPU compositor changes (SynthUI, `src/vglite/`)

Today the RENDER_READY pass redraws the full rotor for every pending
instance. Unscissored full-rotor redraw over wedge-only sw damage is *almost*
correct — the discs are opaque, so SRC_OVER rewrites identical pixels — but
the body disc's outer AA rim re-blends over its own previous blend and
converges toward pure body colour over frames. Instead of accepting that
drift:

- If `delta_n > 0`: for each stored rect, `vg_lite_set_scissor(rect)` and
  draw the three cached paths; after the instance,
  `vg_lite_set_scissor(-1,-1,-1,-1)`. Inside each rect the sw pass has just
  repainted ground+well, so the composite is exactly a fresh render there;
  outside, nothing is touched. Exact, no drift.
- If `delta_n == 0` (full invalidation): unscissored, as today.
- `delta_n` is consumed (reset to 0) after compositing the instance.
- Scissor support is real on this part (`vg_lite_set_scissor` in the
  vendored driver, `gcFEATURE_VG_SCISSOR = 1` for gc355/0x0_1216 — checked
  in source, and §5's equality guard verifies it BEHAVES on silicon).
  **Fallback if silicon refutes the scissor** (wrong clip, error returns):
  the gpu path drops delta — `set_angle` full-invalidates whenever the
  compositor is enabled — and the sw path keeps the win. The equality guard
  stays strict either way; it is what makes the fallback decision by
  measurement instead of by argument.

## 5. Regression guards (in `display/synthui_knob_test` — the bench stays a
frozen Phase-1 artifact)

A new self-test phase after the existing sums, before the hero, run by the
SAME code on both engines:

1. **Equality guard (correctness).** One screen with two knobs (150 px
   endless/light and 250 px bounded/light — two S values, both well types).
   Drive an angle sequence through `set_angle` + `lv_refr_now()` per step —
   including a wrap (350°→10°) and a detent-sized jump — CRC the
   framebuffer: `KNOB_DELTA_SEQ=0x…`. Then rebuild the same screen fresh at
   the final angles (full render): `KNOB_DELTA_FULL=0x…`. **The gate
   compares the two captured values for equality** (not pinned goldens — the
   assertion is "delta rendering is pixel-identical to full rendering", so
   it never needs re-goldening). The firmware also prints
   `KNOB_DELTA_EQ=PASS|FAIL` for bench readability; the gate trusts its own
   comparison, not the verdict token.
2. **Engagement guard (the win is real, not silently reverted).** Hook
   `LV_EVENT_INVALIDATE_AREA` on the sequence knobs; record the largest
   single invalidated area — **only during the pure-angle segment of the
   sequence** (recording stops before the deliberate state toggle, whose
   full invalidation would and should be large):
   `KNOB_DELTA_MAXAREA=<px>`. Gate asserts it is >0 (vacuity: the hook saw
   damage at all) and ≤ 8000 px (a wedge box at 250 px is ~90×90 ≈ 6.5 k; a
   full 250 px control would be 62.5 k — an order of magnitude of margin
   in both directions). A change that quietly escalates every set_angle to
   full invalidation fails HERE and nowhere else.
   (Verify at implementation that LVGL 9.4 emits `LV_EVENT_INVALIDATE_AREA`
   from `lv_obj_invalidate_area`; if it does not, derive the area guard from
   a display-level `LV_EVENT_INVALIDATE_AREA` hook instead.)
3. Demonstrated RED, both guards: (a) shrink the bbox pad to 0 → stale AA
   pixels → SEQ ≠ FULL, gate fails on the equality; (b) revert set_angle to
   full invalidation → MAXAREA explodes past the bound. Quote both in the
   gate header.

The six existing sw goldens must NOT move (the existing scenes render
identically — every screen there is a fresh full render). QEMU gates the sw
engine; the silicon transcript records the gpu engine's SEQ/FULL equality
and `rk_gpu_err=0` (a scissored draw that errors would otherwise
"pass" by drawing nothing — the counter is the guard, as always).

## 6. Consumers

No source changes and **no re-goldening**: acid_box and vglite_lvgl_test
gate only fresh full renders (boot frame / first refresh), and their
behavioral assertions don't checksum drag frames. Both get the speedup for
free through `set_angle` (acid_box's touch drags, FPSBENCH's animation).
Silicon evidence: one FPSBENCH run (sw build) recorded in the
vglite_lvgl_test transcript as the widget-level before/after, beside the
prototype's numbers.

## 7. Out of scope (measured-promise order, for the phase after)

Per-knob single union bbox (halves area count), GPU-drawing the well
(collapses the remaining sw floor to ground fill), overlap/pipelining
(measured ceiling 4% — retired unless the sw floor shrinks to GPU scale).

## 8. Risks

- **Bbox too tight** at some size/angle → caught by the equality guard at
  two S values; the pad is constant because AA is, and sampling error is
  <0.1 px at 360 px.
- **Escalation bugs** (a pending delta surviving a theme/state change) →
  the equality sequence includes a mid-sequence state toggle on one knob to
  exercise the escalation path.
- **Scissor semantics** (inclusive/exclusive right/bottom edges) → verify
  against driver source at implementation; the equality guard on silicon is
  the backstop.
- **LVGL inv-buffer pressure** (32 areas): 16 knobs × 2 rects is exactly the
  default buffer; LVGL joins on overflow — correctness unaffected (joining
  only grows damage), speed degrades gracefully. Not a blocker; noted for
  the all-16 workload only.
