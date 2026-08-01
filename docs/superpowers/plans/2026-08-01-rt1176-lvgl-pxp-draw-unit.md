# v9: LVGL draw-unit PXP — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps
> use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Measure whether a PXP draw unit earns adoption: a raw-op bench (fill/blit/
composite × size ladder × depths, PXP vs LVGL's own SW code), a counter-only census
probe in the real flip/touch scenes, and a projected ms-per-frame verdict at a HARD
STOP — draw-unit code exists only if the user approves it there.

**Architecture:** Spec `docs/superpowers/specs/2026-08-01-rt1176-lvgl-pxp-draw-unit-design.md`
(the contract — read it first). Unlike v8 there is NO QEMU model gap: fills, blits and
composites are all modeled with silicon-measured semantics, so the bench gate lands
green with the bench itself (negative test still red-first).

**Tech stack:** PXP library (pin `5658e34` — fill/blit/overlay), vendored LVGL v9.4
(`lv_draw_create_unit` at `lv_draw.c:86`, task types at `lv_draw.h:48-65`,
`lv_draw_sw_blend(task, dsc)` at `sw/blend/lv_draw_sw_blend.h:52`), the v7 per-example
depth mechanism, hardened SW4 flash flow (memory `mac-kernel-panic-ioserialfamily`).

**Repos touched:** evkb (branch `lvgl-pxp-draw-v9`), LVGL (master — census probe in
`port/`), conditionally more at P3+.

**Resolved plan-stage questions (spec §11):**
- Registration: `lv_draw_create_unit(size)` allocates + links a unit globally; set
  `evaluate_cb`/`dispatch_cb` on the returned struct. Global, not per-display — fine
  (one display).
- Task types: the census enumerates `LV_DRAW_TASK_TYPE_FILL/BORDER/BOX_SHADOW/LETTER/
  LABEL/IMAGE/LAYER/LINE/ARC/TRIANGLE/MASK_*` + other (`lv_draw.h:48-65`).
- SW comparator: `lv_draw_sw_blend()` requires an `lv_draw_task_t` — Task 1 Step 2
  probes fabricating one; the fallback (pattern-faithful loop, PROXY stated in tokens
  and transcript) is pre-approved by the spec.
- `PXP.fill` sub-rects: the v6 offset-base trick (`data + y*pitch + x*bpp`, pitch =
  full stride) applies to fill surfaces identically — the bench proves it per case.

---

### Task 0: Branch

- [ ] From a clean `master`:

```bash
git checkout -b lvgl-pxp-draw-v9
```

---

### Task 1: The bench — `pxp_draw_bench` (both depths) + gate (negative red-first)

**Files:**
- Create: `examples/display/pxp_draw_bench/{CMakeLists.txt,pxp_draw_bench.cpp,toolchain/rt1170-evkb.toolchain.cmake}`
- Create: `examples/display/pxp_draw_bench/run_qemu_pxp_draw.sh`
- Create: `examples/display/pxp_draw_bench/transcript_qemu.txt`

Model CMake/toolchain on `pxp_composite_test` (imports `MipiDisplay soc panels/rk055`
+ PXP; NO LVGL library link needed for PXP paths, BUT the SW comparator needs LVGL —
import `import_evkb_lvgl()` too and link `LVGL stdc++` like the LVGL examples do).
Depth: the default build is 565; a second build dir `build-32/` configured with
`-DLV_COLOR_DEPTH=32 -DPANEL_BYTES_PER_PIXEL=4`... **NOTE the bench must add the same
`add_compile_definitions` OPTION pattern as the v7 panel test** (`option(DRAW_BENCH_32
"XRGB8888 build" OFF)` gating the two definitions) so both builds come from one
CMakeLists; the 32-bpp ELF is built into `build-32/`.

- [ ] **Step 1 (skeleton + tokens):** `setup()`: `PXP_DRAW_BENCH_BEGIN` →
  `Display.begin()` `PANEL_OK` → `PXP.begin()` readback `PXP_OK` → extmem allocs
  (source image buffers 720×1280 max at fb depth + ARGB8888 720×1280 for composite
  sources + an expected-frame scratch) `ALLOC_OK` → cases → `DRAWS=<count>` →
  `DRAW_BENCH_OK` → `PXP_DRAW_BENCH_DONE`; every early-out prints DONE (the v8
  discipline). Print `DEPTH=<16|32>` right after BEGIN (the gate asserts it per
  build).

- [ ] **Step 2 (the SW comparator probe):** attempt LVGL's own code first:
  `lv_init()` (via `lvgl_rt1176_begin()` or plain `lv_init()` — no display needed),
  then fabricate a minimal `lv_draw_task_t` + `lv_draw_sw_blend_dsc_t` targeting the
  framebuffer region (read `lv_draw_sw_blend.h` for the dsc fields: dest buf/stride/
  cf, blend area, color or src_buf, opa). Verify with a 4×4 smoke fill+compare at
  both depths. If the internal asserts/dependencies make this infeasible standalone,
  fall back to pattern-faithful loops AND: print `SW_PATH=proxy` instead of
  `SW_PATH=lvgl` after DEPTH, make the gate assert whichever is shipped, and record
  the proxy status in both transcripts. Report which path you shipped.

- [ ] **Step 3 (the case matrix):** ladder {32×32, 120×40, 240×160, 400×300, 720×80,
  720×1280} × ops:
  - `fill_<w>x<h>`: solid color at an offset rect (offset-base PXPSurface); SW same
    rect. Correctness: every pixel == expected constant (direct scan), whole-frame
    FNV vs a CPU-maintained expected frame (the OOB detector, v8 pattern).
  - `blit_<w>x<h>`: same-format image from extmem; byte-compare + FNV.
  - `comp_<w>x<h>` (565 build ONLY — `#if LV_COLOR_DEPTH == 16` around these cases):
    ARGB8888 source with varied alpha over the fb; PXP path asserted against the v8
    oracle formulas (transcribe blend_channel/effective_alpha locally with the
    MEASURED citations); SW path asserted against ITS OWN expected (computed by
    calling the SW path into the scratch too — self-consistency) and the per-case
    max-LSB divergence between engines REPORTED: `comp_<n> ENGINE_DELTA=<maxlsb>`.
  - `overhead_16x2`: the bare-op constant, PXP fill 16×2, timed alone.
  Each case: `DRAW n=<name> CPU_us=<t> PXP_us=<t> OK|BAD`; correctness failure →
  BAD + `all_ok=false`, never a hang. Count printed as emitted (`DRAWS=`), pinned:
  565 build = 6 fills + 6 blits + 6 comps + overhead = **19**; 32 build = **13**.

- [ ] **Step 4 (gate):** `run_qemu_pxp_draw.sh` runs BOTH ELFs sequentially (two qrun
  invocations, two capture files): asserts per-build `DEPTH=` token, every `DRAW n=`
  name anchored + ` OK`, no `BAD`, `^DRAWS=19$` / `^DRAWS=13$`, `DRAW_BENCH_OK`,
  guest-error logs clean. Timings not asserted (vacuous, stated in a comment).
  Ceilings measured from real runs + 50%.

- [ ] **Step 5 (negative, red-first):** sabotage the local composite oracle constant
  (565 build) → gate RED naming comp cases; revert by hand-edit; green ×2. Then also
  sabotage a fill expected-color constant → RED (proves the fill assertions bite
  too); revert; green. Record both reds in the gate's comment block.

- [ ] **Step 6:** `transcript_qemu.txt` from a green run (both builds' captures,
  banners stripped). Commit:

```bash
git add examples/display/pxp_draw_bench/ && \
git commit -m "pxp_draw_bench: fill/blit/composite ladder, PXP vs LVGL-SW, both depths (v9 P1)"
```

---

### Task 2: The census probe + inert-proof

**Files:**
- Create: `~/Development/LVGL/port/lvgl_pxp_draw_census.{h,cpp}`
- Modify: `examples/display/lvgl_rk055_flip_test/CMakeLists.txt` (+ census option)
- Modify: `examples/display/lvgl_rk055_touch_test/CMakeLists.txt` (same)
- Modify: both examples' `.cpp` (guarded census-install + census-print calls)

- [ ] **Step 1 (probe):** in the LVGL port:

```cpp
/* lvgl_pxp_draw_census.h -- v9 P1: a COUNTER-ONLY draw unit.  Registers ahead
 * of the SW unit, classifies every draw task in evaluate_cb (type, size
 * bucket, target cf) and NEVER claims one: behaviorally inert by contract --
 * the adopting examples' goldens must be byte-identical with it installed,
 * which the v9 gates prove.  Also measures its own per-task evaluate cost
 * (DWT), the overhead constant the P2 projection subtracts. */
void lvgl_pxp_draw_census_install(void);
void lvgl_pxp_draw_census_print(void);   /* CENSUS type=<t> bucket=<b> n=..
                                            + CENSUS_EVAL_NS=<mean> lines   */
```

`evaluate_cb`: bucket by area of `task->area` against the sorted ladder-area edges
(1024, 4800, 38400, 57600, 120000, 921600 — i.e. 32×32, 120×40, 240×160, 720×80,
400×300, 720×1280), count
`[type][bucket]`, accumulate DWT delta; `return 0` WITHOUT setting
`task->preferred_draw_unit_id` (READ how the SW unit and lv_draw.c interpret
evaluate results FIRST — the not-taken convention must be exactly what dispatch
expects; report what you found). No `dispatch_cb` work (return -1/LV_DRAW_UNIT_IDLE
per the convention found).

- [ ] **Step 2 (wiring):** in both examples' CMakeLists:

```cmake
option(DRAW_CENSUS "install the v9 counter-only draw-unit census probe" OFF)
if(DRAW_CENSUS)
    target_compile_definitions(<name>.elf PRIVATE DRAW_CENSUS=1)
    # census .cpp added to the executable's sources unconditionally is fine --
    # it compiles to nothing when the install call is absent... NO: keep it
    # conditional to keep shipped binaries byte-comparable.  Add the source
    # only under the option.
endif()
```

In the `.cpp`s: `#ifdef DRAW_CENSUS` install after LVGL/display init, print after
the existing end tokens.

- [ ] **Step 3 (inert-proof, QEMU):** build both examples NORMALLY → run gates →
  PASS (baseline). Build both with `-DDRAW_CENSUS=ON` in scratch dirs
  (`build-census/`) → run each gate against the census ELF (temporarily point the
  gate at it — the runners take `build/<name>.elf`; copy the census ELF over a
  scratch checkout? NO — simplest: configure `build-census` then run the gate with
  `BUILD_DIR` if the runner supports it; if not, run qrun manually and assert the
  same tokens + THE SAME GOLDEN VALUES as the committed gate expects, plus the new
  CENSUS lines present). The goldens and every pin must be BYTE-IDENTICAL. Record
  the CENSUS histograms from QEMU (they are real data about the scripted scenes —
  QEMU-valid since task generation is timing-independent... verify the flip scene's
  120 cycles produce identical censuses across two QEMU runs; if run-to-run varies,
  say so).

- [ ] **Step 4:** commit (LVGL repo + evkb):

```bash
cd ~/Development/LVGL && git add port/lvgl_pxp_draw_census.* && \
git commit -m "port: v9 census probe -- a counter-only draw unit, provably inert"
cd - && git add examples/display/lvgl_rk055_flip_test examples/display/lvgl_rk055_touch_test && \
git commit -m "flip/touch: -DDRAW_CENSUS variant installs the v9 census probe (gates untouched)"
```

---

### Task 3 (NEEDS USER): hardware — the tables and the census

- [ ] **Step 1:** hardened flow ×4 flashes, one SW4 press each (+ the touch ritual
  under the census build):
  1. `pxp_draw_bench` 565 ELF → capture the 19-case table.
  2. `pxp_draw_bench` 32 ELF → the 13-case table.
  3. flip test census build → capture CENSUS lines (animation runs itself).
  4. touch test census build → USER performs the standard ritual (BTN1-5, drag,
     HOLD/TRAP) → CENSUS lines under a live finger; all existing ritual tokens must
     still pass (inertness on silicon).
- [ ] **Step 2:** write `transcript_hw_evkb.txt` for the bench (both tables,
  verbatim; SW_PATH status; the ENGINE_DELTA facts) and append census sections to
  the flip/touch hardware transcripts (census build, goldens confirmed identical).
- [ ] **Step 3:** commit.

---

### Task 4: The projection + THE HARD STOP

- [ ] **Step 1:** compute, in the transcript (a ★★ ANALYSIS section): per scene,
  Σ over census cells (count × (SW_us − PXP_us) for cells above crossover, from the
  hardware table) − (task_count × eval_overhead) → projected ms/frame and %-of-33ms;
  the crossover per op from the table; the three spec caveats restated.
- [ ] **Step 2:** commit the analysis, then **STOP. Present the table, histograms,
  projection and a recommendation to the user. Do not write ANY draw-unit code
  until they decide.** (P3+ tasks below execute only on an explicit "adopt".)

---

### Task 5 (CONDITIONAL — only on the user's P2 "adopt"): the draw unit

**Files:** Create `~/Development/LVGL/port/lvgl_pxp_draw_unit.{h,cpp}`; modify the
adopting examples (+ install call + counters + import already present).

Shape (finalized against the P2 numbers): `lv_draw_create_unit`; `evaluate_cb`
accepts FILL (opaque, area ≥ measured crossover), IMAGE (same-format, opaque, ≥
crossover), IMAGE-with-alpha (ARGB8888 over 565 fb only, ≥ crossover); everything
else not-taken. `dispatch_cb` synchronous: build the PXPOp (offset-base surfaces;
`.overlay()` for composites), `run()`, mark task done; ANY PXP error → return the
task to SW + `s_errors++` (degraded loud). Counters + `DRAW_PXP_TASKS>0` /
`DRAW_PXP_ERRORS=0` tokens in adopting gates. Goldens: expected UNCHANGED at 32 bpp
(fills/blits byte-exact) — if any golden moves, that is a real finding, STOP.
Rituals re-run (NEEDS USER). Details to their own plan-level checklist at execution
time, informed by P2.

---

### Task 6: Wrap

- [ ] Audit red-first (`GATES` entry `examples/display/pxp_draw_bench:pxp_draw_bench`
  — note the gate's variant name `run_qemu_pxp_draw.sh` is already discovered by the
  sweep's `run_qemu*.sh` glob), then green.
- [ ] Sweep → **74/0/0** expected (or the documented singleton); `CLAUDE.md` count
  73 → 74; `docs/KNOWN-BROKEN-GATES.md` dated note; `examples/README.md` row.
- [ ] Push LVGL; bump its pin (+ PXP pin if P3+ touched it); force-fetch proof.
- [ ] Spec AS-SHIPPED re-sync for anything the measurements changed (SW_PATH status,
  the actual case counts, the P2 verdict itself).
- [ ] Wrap commit; then finishing-a-development-branch (final whole-branch review
  first, as always).

---

## Self-review notes (already applied)

- The bench gate lands WITH the bench (no model gap this time) — but the negative
  tests still run red-first (two of them, composite + fill).
- The census inert-proof is both-sided: QEMU gates + one silicon ritual per
  example, goldens byte-identical everywhere.
- Case counts pinned per depth build (19/13); `DRAWS=` prints emitted count.
- The STOP is structural: Task 5 is marked CONDITIONAL and its dispatch waits for
  the user's verdict in Task 4.
- Census evaluate/dispatch return conventions are flagged as read-first items, not
  assumed.
