# RT1176 → PXP-accelerated LVGL sync copy (v6: "measure, then earn it") — Design

**Date:** 2026-07-30
**Status:** validated design, ready for an implementation plan
**Fulfils:** the "PXP-accelerated drawing" third of the v4 roadmap split
(`2026-07-30-rt1176-lvgl-double-buffer-design.md` §2), narrowed to the one candidate every
milestone since has named: the `refr_sync_areas` cross-buffer copy. Builds on v3–v5, all
complete, hardware-verified and merged.

---

## 1. Goal

Decide — by measurement, not assumption — whether the PXP should carry LVGL's
cross-buffer dirty-region copy, and ship the acceleration only if the numbers justify it.
**"Not worth it" is a legitimate, complete outcome of this milestone**: the bench, its
correctness gate, the hardware table, and a documented decision would then be the entire
deliverable.

**Facts this design stands on, verified in source:**

1. **LVGL v9 has an official seam for exactly this copy.** `lv_draw_buf.h:290`:
   "Overwriting dest->handlers->buf_copy_cb can resolve this limitation";
   `lv_draw_buf_get_handlers()` (`lv_draw_buf.h:136`) returns the mutable global handler
   struct, and `refr_sync_areas` reaches the copy through `lv_draw_buf_copy`
   (`lv_refr.c:736-738`). No vendored-LVGL patch.
2. **The PXP sibling library has the machinery**: `PXPClass::blit(const PXPSurface&,
   const PXPSurface&)` + `wait(timeout_ms)` (`~/Development/PXP/PXP.h:178,181`),
   hardware-verified since the camera work; the QEMU machine models the PXP and three
   gates already prove it (`pxp_blit_test`, `pxp_decimate_test`, `pxp_yuv_test`).
3. **The physics cut both ways, which is why the measurement leads.** The copy runs over
   non-cacheable SDRAM (no cache maintenance needed for PXP DMA — but CPU memcpy there
   is slow; a full-screen sync is 1.8 MB/frame against a 33 ms budget). The PXP has
   per-operation setup cost, so small dirty areas (a button press invalidates tens of
   KB) may be faster on the CPU. The expected shape is a crossover, and the handler's
   threshold cites it.
4. `LV_USE_DRAW_PXP` (LVGL's own NXP draw unit) is 0 and deliberately inert in our
   `lv_conf.h` — it wants NXP SDK glue and `LV_USE_OS`. This design does not touch it.

---

## 2. Scope

**In scope (v6):**

- **`examples/display/lvgl_pxp_copy_bench/`** — the measurement instrument and the
  correctness oracle in one example (§4), with a QEMU gate and a hardware transcript.
- **`LVGL/port/lvgl_pxp_copy.{h,cpp}`** — the thresholded, chaining handler (§3),
  written in P3 **only if** the P2 decision point approves adoption.
- **Adoption** by the two double-buffered examples (`lvgl_rk055_flip_test`,
  `lvgl_rk055_touch_test`) — an install call + PXP import + a corroboration token each;
  goldens and every existing assertion stay byte-identical (a correct copy is
  pixel-identical by definition).
- **The decision point (P2): the measured hardware table goes to the user before any
  adoption code is written.** That review is a milestone boundary, not a formality.

**Out of scope — named, not silently dropped:**

- **LVGL draw-unit PXP** (fills/blits inside the renderer via `LV_USE_DRAW_PXP`) — a
  different, much larger integration with its own threading model; would need its own
  driving measurement.
- **Async/pipelined copies.** `refr_sync_areas` must complete before rendering begins;
  overlapping the copy with anything needs evidence the sync path is the bottleneck
  *after* v6 — a later measurement's milestone.
- **XRGB8888** (unclaimed, unchanged) and the v2 GT911 INT findings (still a
  scope-session concern).

---

## 3. The handler: thresholded, chaining, observable

`LVGL/port/lvgl_pxp_copy.{h,cpp}` — deliberately **not** part of the display binding:
nothing gains a PXP dependency it didn't ask for. Compiled only by examples that also
`import_evkb_library(PXP)`.

```cpp
// Install the PXP-backed buf_copy handler on LVGL's GLOBAL draw-buf handlers
// (lv_draw_buf_get_handlers()).  Saves the default CPU copy and CHAINS to it:
// anything that is not the exact accelerated shape falls through -- never a
// silent wrong copy.  The accelerated shape is ALL of:
//   - RGB565, source and dest strides equal and unpadded,
//   - copy area >= threshold_px (the bench's measured crossover, cited at the
//     call site),
//   - both buffers PXP-reachable (extmem/SDRAM; the framebuffers qualify).
// Synchronous: blit + wait().  SINGLE PXP OWNER: an example that installs
// this must not run other PXP work concurrently (none does -- fillScreen
// runs before LVGL starts).  Install AFTER lv_init, BEFORE the display
// binding allocates draw buffers.
void lvgl_pxp_copy_install(uint32_t threshold_px);

// Diagnostics since install: copies taken by the PXP vs fallen through to the
// saved CPU default.  Adopting gates assert PXP_COPIES>0 (the IDLE_POLLS
// idiom) so the handler being silently dead cannot pass as adopted.
uint32_t lvgl_pxp_copies();
uint32_t lvgl_pxp_copy_fallbacks();
```

The wrapper checks its conditions per call; a PXP `blit` that returns an error or a
`wait` that times out falls back to the CPU copy for that call and counts it — degraded
loud (a counter), correct always (the CPU copy runs), the v4/v5 philosophy applied to a
new subsystem.

> **AS SHIPPED (P2 re-sync).** The measured table changed the accelerated-shape rule:
> the load-bearing check is **copy height ≥ 2 rows** (the bench's one CPU win was the
> single-row case; a 256-px, 16-row copy won 21× on the PXP), with `threshold_px` kept
> as a belt-and-braces area floor. Per-surface stride-sane checks replaced
> "strides equal and unpadded" (the offset arithmetic handles padded strides), and the
> reachability pre-check was deliberately dropped — an unreachable surface returns a
> PXP error, which chains to the default anyway; the handler header says so. PXP errors
> are counted apart from shape fallbacks (`lvgl_pxp_copy_errors()`, pinned 0 by the
> adopting gates) so a dying PXP is loud by name.

---

## 4. The bench: `lvgl_pxp_copy_bench`

One example, both halves of the two-gate rule:

- **Correctness (QEMU-provable, gate-asserted):** two extmem buffers (the framebuffer
  allocator's exact discipline), a position-dependent fill pattern (the GT911 blob
  filler's lesson: every byte pays into the sum), then for each case in a matrix —
  areas from 16×16 to 720×1280, including deliberately odd widths, odd x-offsets, and
  non-zero y-offsets — CPU-copy and checksum, restore, PXP-copy and checksum. **The
  checksums must match per case**; any divergence names the case and goes red. The
  matrix is the assertion that the PXP surface programming (pitch arithmetic, rect
  clipping, format fields) is right everywhere the handler could route.
- **Timing (hardware-only, stated):** DWT cycle counts for both paths per case, printed
  always; the QEMU transcript marks them `(QEMU: vacuous)` — the model has no timing —
  exactly the `UNDERRUN` stated-asymmetry pattern from v1. The hardware transcript
  records the full table and names the crossover in pixels.

Tokens: `CASE n=<w>x<h>+<x>+<y> CPU_SUM=0x… PXP_SUM=0x… MATCH cpu_us=<t> pxp_us=<t>`
per case, `CASES=<n>`, `COPY_BENCH_OK`. The gate pins `CASES` exactly and greps every
`MATCH`.

---

## 5. Verification

| Claim | QEMU | Hardware |
|---|---|---|
| PXP copy ≡ CPU copy across the matrix | **the gate's per-case checksums** | same tokens on silicon |
| the numbers (crossover, frame-budget impact) | vacuous, marked | **the P2 table — the milestone's entire point** |
| adopting examples unchanged in behaviour | goldens + MATCH pairs byte-identical; `PXP_COPIES>0` corroborates engagement | rituals re-run; eye on the drag |
| a wrong wrapper cannot pass | negative test: skip the `wait()` → per-case checksum divergence goes red | not deliberately provoked |

Adoption adds **no pinned counter** for PXP copies in the flip/touch gates (copy counts
under touch load are input-dependent) — `PXP_COPIES>0` + the unchanged goldens are the
honest claims.

---

## 6. Decomposition

| | Milestone | Gate |
|---|---|---|
| **P1** | The bench example + its QEMU correctness gate (+ negative test measured red). | `COPY_BENCH_OK`, every `MATCH`, `CASES` pinned. |
| **P2** | Hardware run → **the table goes to the user; explicit adoption decision.** | The transcript. No code beyond P1 exists yet. |
| **P3a** (if adopted) | `lvgl_pxp_copy.{h,cpp}` + install in both db examples + `PXP_COPIES>0` corroboration tokens. | Both gates green, goldens byte-identical. |
| **P3b** (if declined) | The close-out: bench + transcript stand; the README row records "measured, not adopted, here is why"; the handler is not written. | — |
| **P4** | Hardware re-rituals for adopting examples (or none, if P3b). | Transcripts. |
| **P5** | Wrap: sweep **71 → 72** (the bench gate), audit `GATES` entry (red-first), LVGL pin bump if P3a, docs, memory. | Sweep + audit. |

---

## 7. Risks

1. **The global handler leaks beyond sync copies** (highest design risk). `buf_copy_cb`
   is global to LVGL: image caching or snapshot paths could route through it with
   non-RGB565 formats or padded strides. Mitigated by construction — the chain-to-default
   guard means anything unexpected falls through to exactly what runs today — and the
   fallback counter makes unexpected traffic visible rather than silent.
2. **The bench mispredicts the real workload.** The bench copies synthetic rects; real
   sync areas are LVGL-merged dirty regions. Mitigated: the threshold is conservative
   (adopt only clearly-winning sizes), and the adopting gates' unchanged goldens prove
   correctness regardless of prediction quality. If the real-workload win matters, the
   flip test's frame timing on hardware before/after is the honest secondary check.
3. **PXP contention.** `Display.fillScreen` uses the PXP before LVGL starts; nothing
   else runs concurrently in these examples. The install comment states the single-owner
   rule; a future concurrent-PXP design owns its own arbitration.
4. **QEMU PXP model fidelity.** The model is gate-proven for blits, but the bench's odd
   offsets/widths may reach corners the existing gates did not. A model bug found here is
   a model bug fixed (stricter/more faithful), never a bench case deleted.

---

## 8. Licence firewall

All new code MIT in its home repos. The PXP library is already MIT and pinned. P5's
`GATES` entry keeps the audit's drift check honest (red-first, as designed).

---

## 9. Open questions for the plan stage

1. The exact bench matrix (sizes/offsets) — enough cases to cover the handler's routing
   space without an unreadable transcript; the plan fixes the list.
2. Whether `PXPSurface` can express an arbitrary sub-rect copy directly or the wrapper
   computes offset base addresses per call — read `PXP.h`'s surface/rect semantics
   before writing the wrapper; the bench proves whichever form is used.
3. DWT cycle-counter enablement on this core (whether `ARM_DWT_CYCCNT` is already
   running under the existing startup; the camera/audio work may have used it — check
   before adding enable code).
