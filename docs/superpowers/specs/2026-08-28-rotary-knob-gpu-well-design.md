# synthui_rotary_knob GPU well — the 30 fps lever

Date: 2026-08-28
Status: approved (continuation of the delta-damage work at the user's
direction: "proceed with the remaining 30fps levers")
Basis: `2026-08-27-rotary-knob-speed-findings.md` and
`2026-08-27-rotary-knob-delta-damage-design.md` §7b.

## 1. Why this lever, by arithmetic

At wedge-delta damage the gpu engine's frame is 44.9 ms: 7.2 ms GPU pass +
37.7 ms LVGL software. The software residual is fully accounted for by the
clipped WELL repaints: ~32 areas × ~2.5 k px × the measured ~0.48 µs/px of
LVGL sw mask arithmetic ≈ 38 ms. (Two floor measurements fix that unit cost:
10.8 ms per 22.5 k-px full-knob area and 1.18 ms per 2.5 k-px wedge area —
per-area fixed overhead extrapolates to ≈0.) Moving the well to the GC355
removes essentially the whole residual; the projected gpu frame is the GPU
pass (grown by the well draws) plus ground fills at ~630 MB/s plus refresh
machinery — well under 33.3 ms. **Acceptance: a measured ≥30 fps
(`mfps_med ≥ 30000`) on silicon at the all-16-knob delta workload.**
The union-bbox lever stays in reserve, added only if the measurement falls
short.

## 2. Scope: gpu engine only, sw untouched

When the compositor is enabled, the widget's DRAW_MAIN paints **nothing**
(it only marks `gpu_pending`); the compositor draws well + rotor per
rendered area. The software path — QEMU, GPU-absent boards, and every
consumer that doesn't attach the compositor — is byte-identical to today:
**no QEMU golden moves anywhere**, including the delta guards' sw values.
The sw engine stays at its measured 14 fps worst case (single-knob use is
far past 30); lifting it would be a well-image-cache lever (the v9 PXP
revisit), deliberately not taken now.

## 3. GPU well geometry (per knob, angle-independent — `m_fixed`)

- Endless: disc r39 filled `well`; border ring r(39−bw)..39 filled
  `well_stroke`, bw = 3 (focus) or 1.6 viewBox units — LVGL's
  border-inside-radius convention, matching the sw look (pixel parity with
  sw is NOT required — the gpu golden set is separate by standing rule —
  but visual parity is).
- Bounded: disc r39 filled `well`; track ring r41.5..44.5 from min to max
  filled `well_stroke` + two cap discs r1.5 at P(43, min) and P(43, max)
  (the sw arc's rounded linecaps).
- All well paths are drawn with the UNROTATED matrix — the delta-damage
  §7b lesson is now a rule: **nothing angle-independent may render through
  the rotation matrix**.
- Well geometry varies per instance (mode, min/max, focus width), all
  rarely-changing but not global, so well paths are emitted per pending
  knob per frame into a bump arena that is reset only AFTER
  `vg_lite_finish()` (safe whether the driver inlines path data into the
  command buffer or references it). Cost basis: the bench measured ~15 µs
  to build 3 paths — 16 knobs × ≤5 well paths is well under 1 ms.
- Draw order per area: well disc, border/track (+caps), then rotor body,
  inner, wedge — the sw painter's order.
- The one-composite-per-pixel decomposition and the per-area scissor apply
  to the well draws exactly as to the rotor draws.

Constraint made explicit (was already true for the rotor, now covers r39):
in gpu mode nothing else may be z-ordered above a knob inside its bbox —
the composite would overdraw it. Every consumer already satisfies this.

## 4. Measurement: an fps phase in synthui_knob_test, after the gate's eyes

After `SYNTHUI_KNOB_DONE` (so the QEMU gate, which stops at DONE, never
waits on it and never parses it — the bench's ungated-Phase-B precedent),
the test builds the 4×4 grid, arms a 15 ms angle-advancing timer (all-16
damage per refresh, through the widget's own delta path), measures 64
REFR_START→REFR_READY intervals damage-gated by RENDER_READY (the bench's
method), prints
`rk_fps frames=64 mfps_med=<n> us_med=<n> us_min=<n> us_max=<n> engine=<sw|gpu>`,
then loads the hero. On silicon this is the widget-level gpu number the
acceptance criterion reads; in QEMU the line exists but is timing-meaningless
and nothing asserts it.

## 5. Verification

- QEMU gate: all existing goldens and delta guards must pass UNCHANGED —
  the sw path is untouched, and that stability is itself the assertion.
- Silicon: the delta equality guard must PASS on the gpu engine (it now
  exercises the GPU well thoroughly — every damaged area re-renders well
  paths); `rk_gpu_err=0`; a NEW gpu golden set (GPU-AA wells differ from sw
  wells), two-boot stable, recorded in the transcript with the superseded
  set noted; a held-frame SWD dump eyeballed (bounded track caps, focus
  rings, well rims); and the §1 acceptance number from the rk_fps phase.
- Close-out: sweep (SynthUI core touched → consumer ELFs rebuild; their
  goldens must hold), vacuity, audit, pin bump + FORCE_FETCH gate run.

## 6. Risks

- **Track cap discs** double-covering the track ring's ends (both painted
  `well_stroke`, opaque over each other in one composite — benign) but AA
  seams under the equality guard would surface on silicon; the guard
  decides.
- **Arena lifetime**: reset strictly after finish; sized for 16 knobs × 5
  paths with a sticky overflow flag counted into `rk_gpu_err` (a truncated
  well is a wrong picture that still draws — the bench's rule).
- **Focus border width** is part of path geometry (not color): emitted per
  frame, so a focus toggle just changes what is emitted; no caching to
  invalidate.
