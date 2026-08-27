# RotaryKnob render-speed decomposition — measured findings

Date: 2026-08-27 (evening, after NEW-20 Phase 2 merged at 7324049)
Status: findings only — no production code changed. Input to a future
"delta damage" spec.

## What was measured, and how

Three scratch builds of `display/rotary_knob_bench` (instrumentation reverted
after the runs; nothing here is committed code), run on the EVKB + RK055, the
four vector cells only, standard Phase B (64 timed frames, all-16-knob
damage unless stated). Validity anchors: every Phase A CRC in the delta run
matched the committed goldens bit-for-bit (0x41193045 / 0xB7744585 /
0x249D6365 / 0xC4987C05), and the instrumented run 1 reproduced §13's
timings within noise.

1. **Split the GPU pass** (`micros()` around issue vs `vg_lite_finish`).
2. **Well-only floor** (`RKB_WELL_ONLY`: every cell draws ground + well and
   NO rotor, no GPU pass).
3. **Wedge-union delta damage**: `anim_cb` invalidates only the bounding
   boxes of the index wedge at the old and the new angle (5-sample sector
   bbox over r16..36, ±8°, 3 px AA pad) instead of `lv_obj_invalidate(obj)`.
   Everything else untouched — the GPU still redraws the full rotor, which is
   CORRECT for notch because the body/inner discs are rotationally invariant
   and opaque: redrawing them erases the old wedge wherever it was.

## Results (vector/notch, us_med per frame)

| configuration | frame | fps | notes |
|---|---|---|---|
| gpu, full-control damage (= §13) | 180.2 ms | 5.5 | gpu pass: **issue 0.6 ms + finish-wait 6.6 ms = 7.2 ms** |
| sw, full-control damage (= §13) | 310.2 ms | 3.2 | |
| **well-only floor** (either engine) | **172.9 ms** | 5.8 | all four cells CRC-identical (0x5ACEC7C5) — the control |
| gpu, wedge-union damage | **44.9 ms** | **22.3** | gpu pass unchanged (7.2 ms) |
| sw, wedge-union damage | 71.3 ms | 14.0 | |

The decomposition closes exactly: 173.0 (floor) + 7.2 (gpu) = 180.2
(measured whole frame). Everything the bench and Phase 2 called "the LVGL
software floor" is real and is **96% of the winning cell's frame**; the GC355
rotor work is 4%.

Derived unit costs: sw rotor = 310.2 − 173.0 = 137 ms / 48 primitives ≈
**2.9 ms per AA circle/arc primitive** at 150 px — that is what LVGL sw mask
arithmetic costs on this core, and why per-primitive counts, not pixel
counts, dominate.

## What this says about the levers

- **Delta damage is the lever, measured**: 4.0× (gpu) / 4.4× (sw) from
  changing ONLY which rectangles get invalidated. For notch, an angle change
  moves nothing but the ±8° index wedge — the discs are rotationally
  invariant — so the true damage is two small sectors (~3–4 k px vs 22.5 k).
- **Pipelining is a dead lever for vector/gpu**: `vg_lite_finish` costs
  6.6 ms of a 180 ms frame. Removing or overlapping it caps at ~4% (it was a
  fair suspicion in the bench spec; the measurement retires it). Facet's 9
  paths: issue 1.8 ms, wait 8.8 ms — path count still nearly free.
- **Remaining budget at wedge damage** (gpu cell): 44.9 − 7.2 = 37.7 ms of
  LVGL work for 32 invalidated areas ≈ 1.2 ms/area — per-area refresh
  overhead plus the well's clipped mask primitives. Next reductions, in
  order of measured promise: ONE union bbox per knob instead of two
  (16 areas), GPU-drawing the well too (sw side becomes ground fill only),
  vg_lite scissor per clip. Any one of these plausibly crosses 30 fps at the
  all-16 workload; the single-knob interactive case is already far past it
  (damage is 1/16th of this workload's).
- **The sw fallback benefits the same way** (14 fps at wedge damage), which
  matters because QEMU and GPU-absent boards run it.
- Caveats for the production design: wedge-union damage is exact for notch
  only (facet's shaded triangles move with every rotation — the scratch run's
  facet rows rendered stale outside the clip and their timings are invalid);
  other prop changes (state/theme/accent/range/mode) must keep full-control
  invalidation; the periodic us_max outliers (~71–113 ms) are the angle-wrap
  frames where old and new wedges are far apart — real, bounded, and worst
  case still 14 fps.

## Suggested next phase (not yet specced)

`synthui_rotary_knob_set_angle()` computes old∪new wedge bboxes and
invalidates those instead of the whole object (notch knows its own geometry;
the helper is ~20 lines — prototype in this session's scratch). Add a bench
cell or a `[delta]` assertion so the win is regression-guarded, re-golden
nothing (damage geometry is checksum-neutral for full-screen Phase A by
construction — demonstrated by the matching CRCs above). Optional follow-ons
in measured-promise order: per-knob union bbox, GPU well, scissored GPU pass.
