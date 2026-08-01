# RT1176 → LVGL draw-unit PXP (v9: "measure, then earn it" again) — Design

**Date:** 2026-08-01
**Status:** validated design, ready for an implementation plan
**Fulfils:** the "LVGL draw-unit PXP" roadmap item, deferred since v6
(`2026-07-30-rt1176-lvgl-pxp-copy-design.md` §2) with the note "a different, much
larger integration … would need its own driving measurement". This design IS that
driving measurement, with the integration conditional on its verdict. Builds on
v3–v8, all complete, hardware-verified and merged.

---

## 1. Goal

Decide by measurement whether a PXP **draw unit** — accelerating LVGL's rendering
ops (rect fills, image blits, alpha composites) inside the renderer, not just the
v6 cross-buffer sync copy — earns its place. **"Not worth it" is a legitimate,
complete outcome**: the bench, the census, the projection and a documented decision
would then be the entire deliverable.

The **P2 hard stop** puts three artifacts in front of the user before any
draw-unit code exists:
1. the raw-op hardware table (PXP vs LVGL-SW per op × size × depth),
2. task-census histograms from the real flip and touch scenes,
3. a projected ms-per-frame gain per scene (histogram × table − evaluate
   overhead), with stated caveats.

**Facts this design stands on, verified in source:**

1. LVGL's bundled NXP draw unit (`lvgl/src/draw/nxp/pxp/`) hard-includes
   `fsl_pxp.h` (the NXP SDK driver this tree deliberately excludes) — unusable;
   ours is written against the MIT PXP library instead.
2. The draw-unit API is open: `lv_draw_unit_t` with `evaluate_cb` /
   `dispatch_cb` / `wait_for_finish_cb` (`lv_draw_private.h:93-176`); units chain
   ahead of the SW renderer, and any task a unit declines falls through to SW. A
   synchronous unit needs no `LV_USE_OS`.
3. The PXP library covers the candidate ops: `fill` (Phase 1), `blit` (Phase 1,
   v6/v7-measured byte-exact at both depths), `.overlay()` compositing (v8,
   silicon-measured semantics).
4. **v8's truth table was authored at a 565 output.** Compositing into an
   XRGB8888 framebuffer writes the X byte as silicon's underived computed alpha,
   which QEMU deliberately does not model (the v8 stated asymmetry) — so 32-bpp
   composite acceleration would make framebuffer goldens diverge QEMU-vs-silicon.
   Fills and blits at 32 bpp are safe (X:=0 measured on both sides, v7).
5. The v6 handler and a sync draw unit share the PXP strictly sequentially on the
   LVGL thread (`lv_timer_handler`) — the single-owner rule holds with no
   arbitration.
6. The v6 crossover lesson (per-op cost vs bytes moved; height≥2; thresholds)
   applies per-task here, and draw tasks are often tiny — hence the census.

---

## 2. Scope

**In (v9):**

- **`examples/display/pxp_draw_bench`** — the raw-op instrument + correctness
  oracle + QEMU gate (§3), built at BOTH depths via the v7 mechanism.
- **`LVGL/port/lvgl_pxp_draw_census.{h,cpp}`** — the counter-only draw unit (§4),
  compiled into the flip/touch examples under a `-DDRAW_CENSUS=ON` build variant
  (demo-variant pattern; shipped gates untouched).
- **The P2 hard stop** (§5) — table + histograms + projection + recommendation;
  no unit code before the user's verdict.
- **Conditional P3+** (§6, only if adopted): `LVGL/port/lvgl_pxp_draw_unit.{h,cpp}`,
  registration in the RK055 examples, rituals, wrap.

**Ops × depths (decided in brainstorm):** fill + blit + composite at 565;
fill + blit only at 32 bpp — composite tasks are DECLINED at 32 bpp by
construction.

**Out — named, not silently dropped:**

- **Deriving the composite X formula** (would unlock 32-bpp composites) — a
  v8-style instrumented sub-campaign for a byte no consumer reads; deferred.
- The SDK draw unit; async/pipelined dispatch (sync only); label/line/arc ops
  (no PXP support); Porter-Duff; decimation/rotation inside draw tasks.

---

## 3. The bench: `pxp_draw_bench`

RK055 up via `Display.begin()`; every case renders into the live framebuffer.
The size ladder — 32×32, 120×40, 240×160, 400×300, 720×80, 720×1280 — × three
ops, each executed by BOTH paths, correctness-checked, then DWT-timed:

- **Fill** (opaque solid color): `PXP.fill` vs the SW comparator.
- **Blit** (opaque same-format image from extmem): `PXP.blit` vs SW.
- **Composite** (ARGB8888 image with varied per-pixel alpha over the
  framebuffer): v8 `.overlay()` vs LVGL's SW blend — **565 build only**.

**The SW comparator must be LVGL's own code, not a proxy loop.** The plan pins
the exact callable entry — `lv_draw_sw_blend()` with a hand-built
`lv_draw_sw_blend_dsc_t` is the candidate. If the API genuinely cannot run
standalone, the fallback is a pattern-faithful loop whose PROXY status is stated
in the transcript exactly the way QEMU timing vacuity is stated — never silently.

Correctness per case: fills against the expected constant; blits byte-compare;
composites against the v8 silicon-measured oracle formulas. **The two composite
paths legitimately differ on LSBs** (LVGL's blend rounding ≠ silicon's
/256-truncate): each path is asserted against ITS OWN oracle and the divergence
is REPORTED as a measured fact, never averaged away or byte-compared across
engines.

One dedicated case measures **bare op overhead** (a minimal 16×2 op — program +
enable + completion), the constant that determines every crossover. Tokens:
`DRAW n=<op>_<w>x<h> CPU_us=… PXP_us=… OK|BAD`, `DRAWS=<count>` pinned per
depth build, `DRAW_BENCH_OK`. The QEMU gate runs BOTH depth builds (timings
vacuous, stated); the negative test (a sabotaged oracle constant) must go red by
case name before the green is trusted.

---

## 4. The census probe

A real `lv_draw_unit_t` registered ahead of SW whose `evaluate_cb` classifies
every task — type, size bucket, target color format — into counters and **never
claims any task**. No dispatch work. It also measures its own per-task evaluate
cost (DWT), the overhead constant the projection subtracts.

Compiled into `lvgl_rk055_flip_test` and `lvgl_rk055_touch_test` only under
`-DDRAW_CENSUS=ON`; prints `CENSUS type=<t> bucket=<b> n=<count>` lines plus
`CENSUS_EVAL_NS=<n>` after the existing tokens.

**Hard invariant: the census build's goldens and every existing pin must match
the normal build byte-for-byte** — proven by re-running both gates against the
census builds in QEMU AND one hardware ritual each. The probe thereby also
smoke-tests the draw-unit registration API before any real unit exists.

---

## 5. The projection + the stop (P2)

Census histogram × op table → projected ms/frame saved per scene, minus
(task count × evaluate overhead). Presented with the raw artifacts and a
recommendation. **No draw-unit code exists until the user approves.**

Stated caveats, up front in the same document: (a) the flip/touch scenes are
simple and may flatter fills; (b) a census cannot see ops LVGL merges or skips
at render time; (c) synthetic ladder sizes bracket, not enumerate, real task
sizes.

---

## 6. Conditional P3+ (only if adopted): the unit

`LVGL/port/lvgl_pxp_draw_unit.{h,cpp}` — deliberately NOT part of the display
binding (the v6 precedent: nothing gains a PXP dependency it didn't ask for).

- `evaluate_cb` gates: op ∈ {opaque rect fill, same-format image blit,
  ARGB8888-over-565 composite}, area/height ≥ the P2-measured crossovers, color
  format checks (composites declined at 32 bpp, §1.4); everything else falls
  through to SW.
- `dispatch_cb` is synchronous (`run()` inside dispatch; bounded wait); on any
  PXP error the task is returned to SW and counted — degraded loud
  (`DRAW_PXP_ERRORS`, pinned 0 by adopting gates), correct always.
- Counters: `DRAW_PXP_TASKS` / `DRAW_PXP_FALLBACKS` / `DRAW_PXP_ERRORS` (the
  house idiom; tasks>0 corroborates engagement).
- **Golden stability comes free at 32 bpp**: fills and blits are byte-identical
  to SW output (v6/v7-measured), so the three migrated RK055 examples' goldens
  survive adoption untouched. Only a 565 example adopting COMPOSITES would ever
  re-record (none exists today); if one ever does, the two-engine LSB divergence
  (§3) makes routing part of the golden's identity — record it then.
- Rituals re-run on all adopting examples; the flip test's frame-time pins
  (REFRESHES=FLIPS=VSYNCS=120, VSYNC_TIMEOUTS=0) are the regression backstop.

---

## 7. Verification

| Claim | QEMU | Hardware |
|---|---|---|
| both paths correct per op × size × depth | bench gate, `DRAWS` pinned per depth | same tokens on silicon |
| the numbers (crossovers, overhead, projection) | vacuous, stated | the P2 table + census |
| census inert | flip/touch gates re-run green against census builds, goldens byte-identical | one ritual each under the census build |
| a wrong oracle cannot pass | negative test red by name | not deliberately provoked |
| adoption regresses nothing (if P3+) | all pins + goldens unchanged at 32 bpp | rituals ×3 |

Sweep 73 → **74** (the bench gate; the census variant owns no gate). Audit
`GATES` entry red-first. All flash work uses the hardened VCOM-free + SW4 flow.

---

## 8. Decomposition

| | Milestone | Gate |
|---|---|---|
| P1 | Bench (both depth builds) + census probe + QEMU gate; negative test red-first | `DRAWS` pinned ×2 builds, all OK |
| P2 | Hardware: bench at both depths; census rituals on flip + touch → **table + histograms + projection → HARD STOP, user decides** | transcripts |
| P3+ | (if adopted) the unit + registration + rituals | adopting gates green, goldens stable |
| P-wrap | sweep 73 → 74, audit entry, pins (LVGL; PXP if touched), docs, memory | sweep + audit |

---

## 9. Risks

1. **The projection misleads** (merged/skipped tasks invisible; synthetic sizes).
   Mitigated: stated caveats, conservative crossovers, and — if adopted — the
   flip test's frame pins as the backstop.
2. **Evaluate overhead on hundreds of tiny tasks** eats the wins. Measured
   explicitly by the census and subtracted in the projection.
3. **SW comparator callability** — proxy status stated if `lv_draw_sw_blend`
   cannot run standalone (§3), never hidden.
4. **Two-engine composite LSB divergence** at 565 — per-path oracles + explicit
   divergence reporting; adoption-time golden implications documented (§6),
   currently moot.
5. **API drift** — the draw-unit callbacks are LVGL-internal API; the census
   probe existing in `port/` means an LVGL re-vendor that changes the API breaks
   the build loudly rather than silently disabling acceleration.

---

## 10. Licence firewall

All new code MIT in its home repos (LVGL port, evkb). One new `GATES` entry,
red-first. No new dependencies; the SDK draw unit stays excluded.

---

## 11. Open questions for the plan stage

1. The exact callable form of the SW comparator (`lv_draw_sw_blend()` descriptor
   fields; what init it needs without a full display refresh context).
2. The census bucket edges (match the bench ladder) and task-type enumeration
   (which `lv_draw_task_type_t` values exist in our vendored LVGL).
3. Draw-unit registration order/API (`lv_draw_create_unit`-equivalent in our
   tree) and whether a unit can be registered per-display or only globally.
4. Whether `PXP.fill` needs a sub-rect form (current API fills a whole surface;
   a draw task fills a rect within the framebuffer — the offset-base trick from
   v6 likely suffices; verify against `PXPSurface` semantics).
5. The bench's QEMU poll ceilings from measured runtime (two builds).

---

> **AS SHIPPED (P2 verdict): ADOPTION DECLINED, 2026-08-01.** The measurement
> worked exactly as designed and answered "no, not today": fills — the only
> accelerable task type either scene generates — are CPU-won at both depths
> (the CM7 write-streams ~630 MB/s into non-cacheable SDRAM; §1's economics
> intuition was wrong for write-only ops), and the census found ZERO image
> tasks in either scene, so the PXP's 12–17× blit and 28–63× composite wins
> have no current consumer. Projection: adoption would cost ~0.3 ms/frame.
> **REVISIT TRIGGER, recorded at the user's request: the moment an
> image-heavy scene exists (large images, sprites, photo viewers), re-run
> the census (`-DDRAW_CENSUS=ON`) and re-project — the win waiting there is
> 12–63×.** Corrections to this spec from measurement: §3's SW comparator
> shipped as genuine `lv_draw_sw_blend` (SW_PATH=lvgl, no proxy); §6's
> "goldens unchanged at 32 bpp" is WRONG for fills/blits (the engines'
> X-byte conventions differ: LVGL 0xFF vs PXP 0) — any future adoption at
> 32 bpp re-records goldens; case counts pinned 19/13. Conditional P3+ was
> never built. Full record: `examples/display/pxp_draw_bench/transcript_hw_evkb.txt`.
