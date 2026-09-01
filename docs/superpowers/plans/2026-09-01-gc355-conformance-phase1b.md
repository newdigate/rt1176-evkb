# GC355 Conformance Phase 1b — separating disjoint-vs-nested from four-vs-two

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Add two cases to `examples/display/vglite_conformance` that separate the two variables Phase 1 left entangled, and answer them in ONE bench boot. Sweep count unchanged (124 — same gate, more cases inside it).

**Architecture:** Phase 1 measured (silicon 2026-08-30, two boots byte-identical) that four DISJOINT contours in one path render as one (`runs=1`) with the ordinary CLOSE encoding and as four (`runs=4`) with the slot padded `0x01010101` — but that two NESTED-contour paths on the *ordinary* encoding rendered BOTH contours. A truncate-at-the-first-CLOSE story explains the first and neither of the last two. Two variables are confounded: **disjoint vs nested**, and **four contours vs two**. This phase completes the 2×2.

**Tech Stack:** unchanged — the Phase 1 harness (`vgc_harness.h`), its arena, its predicates, its three host suites, its gate, its pre-registered expectation and drift checker.

---

## The experiment

|  | 2 contours | 4 contours |
|---|---|---|
| **disjoint** | `path/two-disjoint-bars` **(new)** | `path/multi-contour-disjoint` — measured **BROKEN** |
| **nested** | `path/two-contour-ring-nonzero`, `path/evenodd-vs-nonzero` — measured **OK** | `path/four-nested-rings` **(new)** |

Every outcome is informative, and each names a different rule:

| `two-disjoint-bars` | `four-nested-rings` | what it means |
|---|---|---|
| broken | ok | **DISJOINTNESS is the variable.** Contour count is irrelevant. |
| ok | broken | **COUNT is the variable.** Disjointness is irrelevant. |
| broken | broken | Both matter — nesting protects only at two contours. |
| ok | ok | Neither alone; only the four-disjoint *combination* breaks. An interaction. |

**Pre-registration (the honest bit).** I predict **`two-disjoint-bars = broken`, `four-nested-rings = ok`** — the disjointness hypothesis. It is a hypothesis under test, not a confident forecast, and a refutation is the informative outcome. It goes in `expected_silicon.txt` as two ordinary verdict lines, NOT as a `pair:` — all four outcomes here are coherent, and the checker's rule 3 rightly refuses a pair that admits every combination of its members. Being wrong will show as drift, loudly, which is what the checker is for.

## Why these two shapes

**`path/two-disjoint-bars`** is `path/multi-contour-disjoint` with two bars deleted. Same x-extent, same bar height, same column, same encoding — the *only* change is the contour count. Bars 0 and 2 of the existing case (y=16 and y=72) so the gap is 56 px and adjacency is not in question.

**`path/four-nested-rings`** is four concentric rects with **alternating winding**, so under `VG_LITE_FILL_NON_ZERO` the centre column crosses four filled bands. That makes the predicate a **counter, not a pass/fail**:

| contours that rendered | column x=64 shows | `runs` |
|---|---|---|
| 1 (outer only) | one solid 96×96 block | 1 |
| 2 | one ring | 2 |
| 3 | ring + solid core | 3 |
| 4 (correct) | four bands | 4 |

Geometry, centred on (64,64):

| contour | rect | winding |
|---|---|---|
| R0 | (16,16) 96×96 → spans 16..112 | CW |
| R1 | (28,28) 72×72 → 28..100 | CCW |
| R2 | (40,40) 48×48 → 40..88 | CW |
| R3 | (52,52) 24×24 → 52..76 | CCW |

Winding down x=64 (all four contain x=64): `16..28`→+1 **filled**; `28..40`→0 empty; `40..52`→+1 **filled**; `52..76`→0 empty; `76..88`→+1 **filled**; `88..100`→0 empty; `100..112`→+1 **filled**. Four bands, each 12 px tall. Correct `fill` = (96²−72²) + (48²−24²) = 4032 + 1728 = **5760**.

---

### Task 1: The two cases, and a free diagnostic on the existing one

**Files:** Modify `examples/display/vglite_conformance/vgc_cases_path.cpp`

- [ ] **Step 1: Enrich `check_multi_contour`'s detail — WHICH bar survived?**

The existing case reports `runs=1,expect=4,fill=1393` and we do not know *which* bar rendered. `vgc_filled_rows` gives it for free and `detail=` is not part of the expectation (the checker parses only `pixel=` and `repeat=`), so this cannot move a golden.

```c
static vgc_verdict_t check_multi_contour(char *d, size_t n)
{
    const int runs = vgc_count_runs_col(vgc_fb(), VGC_W, VGC_H, VGC_W, 64);
    int ymin = -99, ymax = -99;
    const int fill = vgc_filled_rows(vgc_fb(), VGC_W, VGC_H, VGC_W, &ymin, &ymax);
    /* ymin/ymax say WHICH bar survived when runs < 4: bar i occupies
     * BAR_Y[i]..BAR_Y[i]+BAR_H, i.e. 16, 44, 72, 100. Measured 2026-08-30 the
     * verdict was runs=1 and nothing said whether that was bar 0 or a partial
     * render straddling two -- this closes that. Seeded -99 because
     * vgc_filled_rows leaves the out-params UNTOUCHED on an empty buffer and
     * -1 is the one sentinel it documents as unusable. */
    snprintf(d, n, "runs=%d,expect=4,fill=%d,y0=%d,y1=%d", runs, fill, ymin, ymax);
    return runs == 4 ? VGC_OK : VGC_BROKEN;
}
```

- [ ] **Step 2: Add `path/two-disjoint-bars`**

Place immediately after the `path/multi-contour-close-padded` block.

```c
/* ---- path/two-disjoint-bars ------------------------------------------------
 * ★ ONE HALF OF THE PHASE 1b 2x2. This is path/multi-contour-disjoint with two
 * bars DELETED -- same x-extent, same bar height, same sample column, same
 * ordinary CLOSE encoding. The ONLY variable is the contour count.
 *
 * Phase 1 left two variables confounded: four DISJOINT contours broke
 * (runs=1) while two NESTED ones rendered fine, so "disjointness" and "more
 * than two" both fit. Pair this with path/four-nested-rings:
 *   this broken + rings ok  =>  DISJOINTNESS is the variable
 *   this ok + rings broken  =>  COUNT is the variable
 *   both broken             =>  nesting protects only at two contours
 *   both ok                 =>  only the four-disjoint COMBINATION breaks
 * Bars 0 and 2 of the four (y=16 and y=72), so the gap is 56 px and adjacency
 * cannot be confused with a merge. */
static vg_lite_error_t run_two_disjoint(void)
{
    vg_lite_error_t acc = VG_LITE_SUCCESS;
    vg_lite_path_t p;
    vgc_emit_rect_cw(R_X, BAR_Y[0], R_W, BAR_H);
    vgc_emit_rect_cw(R_X, BAR_Y[2], R_W, BAR_H);
    VGC_FINISH_OR_RETURN(&p, R_X, BAR_Y[0], R_X + R_W, BAR_Y[2] + BAR_H);
    vgc_draw_path(&p, VG_LITE_FILL_NON_ZERO, VGC_FILL_COLOR, &acc);
    vgc_finish_into(&acc);
    return acc;
}

static vgc_verdict_t check_two_disjoint(char *d, size_t n)
{
    /* runs COUNTS surviving contours here: 2 = both, 1 = only the first. */
    const int runs = vgc_count_runs_col(vgc_fb(), VGC_W, VGC_H, VGC_W, 64);
    const int fill = vgc_count_filled(vgc_fb(), VGC_W, VGC_H, VGC_W);
    snprintf(d, n, "runs=%d,expect=2,fill=%d", runs, fill);
    return runs == 2 ? VGC_OK : VGC_BROKEN;
}
```

- [ ] **Step 3: Add `path/four-nested-rings`**

```c
/* ---- path/four-nested-rings ------------------------------------------------
 * ★ THE OTHER HALF OF THE 2x2, and its predicate is a COUNTER rather than a
 * pass/fail -- which is what makes it worth more than a yes/no.
 *
 * Four concentric rects with ALTERNATING winding under NON_ZERO. Down column
 * x=64 the winding number goes 1,0,1,0,1,0,1, so a correct render shows FOUR
 * filled bands. If only k contours survive, exactly k runs appear:
 *   1 => one solid 96x96 block (outer only)
 *   2 => one ring
 *   3 => ring plus a solid core
 *   4 => correct
 * So this case reports HOW MANY contours the GPU honoured, not merely whether
 * it honoured them all. Correct fill = (96^2-72^2) + (48^2-24^2) = 5760. */
#define NR_N 4
static const int NR_XY[NR_N] = { 16, 28, 40, 52 };   /* origin of each rect */
static const int NR_WH[NR_N] = { 96, 72, 48, 24 };   /* and its side */

static vg_lite_error_t run_four_nested(void)
{
    vg_lite_error_t acc = VG_LITE_SUCCESS;
    vg_lite_path_t p;
    for (int i = 0; i < NR_N; i++) {
        if (i & 1) vgc_emit_rect_ccw(NR_XY[i], NR_XY[i], NR_WH[i], NR_WH[i]);
        else       vgc_emit_rect_cw (NR_XY[i], NR_XY[i], NR_WH[i], NR_WH[i]);
    }
    VGC_FINISH_OR_RETURN(&p, NR_XY[0], NR_XY[0],
                         NR_XY[0] + NR_WH[0], NR_XY[0] + NR_WH[0]);
    vgc_draw_path(&p, VG_LITE_FILL_NON_ZERO, VGC_FILL_COLOR, &acc);
    vgc_finish_into(&acc);
    return acc;
}

static vgc_verdict_t check_four_nested(char *d, size_t n)
{
    const int runs = vgc_count_runs_col(vgc_fb(), VGC_W, VGC_H, VGC_W, 64);
    const int fill = vgc_count_filled(vgc_fb(), VGC_W, VGC_H, VGC_W);
    snprintf(d, n, "runs=%d,expect=4,fill=%d,expfill=5760", runs, fill);
    return runs == 4 ? VGC_OK : VGC_BROKEN;
}
```

- [ ] **Step 4: Insert both into the case table, in this order**

Immediately after `path/multi-contour-close-padded`, so every contour-encoding case sits together:

```c
    { "path/two-disjoint-bars",       run_two_disjoint,      check_two_disjoint,      NULL },
    { "path/four-nested-rings",       run_four_nested,       check_four_nested,       NULL },
```

Case count becomes **15**.

- [ ] **Step 5: Build and run under QEMU**

```bash
cd examples/display/vglite_conformance && cmake --build build
timeout 25 ../../../tools/qrun -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on \
  -kernel build/vglite_conformance.elf -display none -serial file:/tmp/p1b.uart
grep -c "^vgc case=" /tmp/p1b.uart   # expect 15
grep "^vgc_summary" /tmp/p1b.uart    # expect cases=15 ... skip=15
```

- [ ] **Step 6: Commit**

```bash
git add examples/display/vglite_conformance/vgc_cases_path.cpp
git commit -m "NEW-32 Phase 1b: two cases separating disjoint-vs-nested from four-vs-two"
```

---

### Task 2: Host geometry suite — the three arms must cover the new cases

**Files:** Modify `examples/display/vglite_conformance/tests/cases_path_geom_test.cpp`

- [ ] **Step 1: Update the count assertion and all three arms**

- `vgc_path_case_count == 15`.
- **Arm 1** (correct rasteriser): all fifteen `ok`. `two-disjoint-bars` must show `runs=2`, `four-nested-rings` `runs=4,fill=5760`. **Pin `four-nested-rings`' fill at 5760 analytically** — it is an integer-aligned rect arithmetic result with no antialiasing in the model, exactly like `single-contour-rect`'s 6400, so a wrong nesting or a wrong winding shows as a number rather than a verdict.
- **Arm 2** (first-contour-only): BOTH new cases join the probe set — `two-disjoint-bars` → `runs=1`, `four-nested-rings` → one solid block, `runs=1`. Add symptom pins (`runs=1,` in each) so they cannot go red for an unrelated reason. Update `is_multi_contour_probe()` to return **six** ids and fix the comment naming the count.
- **Arm 3** (draws nothing): both new cases BROKEN.

- [ ] **Step 2: Verify the alternating-winding reference is right**

The suite's own rasteriser must produce `runs=4` for the nested rings under NON_ZERO. If it does not, decide whether the REFERENCE or the CASE is wrong before changing either — the reference's winding arithmetic was verified in Phase 1 (a CW rect yields +1, `emit_rect_ccw` yields −1), so a disagreement here most likely means the case's geometry is wrong.

- [ ] **Step 3: Demonstrate two mutants RED, each on its targeted line**

- Make `run_four_nested` emit all four rects CW (no alternation) → the column becomes one solid block, `runs=1`, arm 1 fails on that case.
- Delete the second bar from `run_two_disjoint` → arm 1 fails on that case with `runs=1`.

Revert each and confirm green. Paste the actual output and confirm the failing line is the one targeted.

- [ ] **Step 4: Run all suites and commit**

```bash
./examples/display/vglite_conformance/tests/run.sh
git add examples/display/vglite_conformance/tests/cases_path_geom_test.cpp
git commit -m "NEW-32 Phase 1b: host geometry arms cover the two new cases"
```

---

### Task 3: Registration — gate, expectation, vacuity, docs

**Files:** `run_qemu.sh`, `expected_silicon.txt`, `tools/gate-vacuity.test.sh`, `docs/gc355-vglite-quirks.md`, `CLAUDE.md`

- [ ] **Step 1: `run_qemu.sh`** — `CASES -eq 15`, both new ids in the by-name loop (in table order), and the anchored summary grep updated to `cases=15 ok=0 broken=0 skip=15`.

- [ ] **Step 2: `expected_silicon.txt`** — two new PRE-REGISTERED lines placed with their neighbours, each with a reason naming the hypothesis and what the other outcome would mean:

```
path/two-disjoint-bars              broken   same    # PHASE 1b, HALF OF A 2x2 -- a HYPOTHESIS UNDER TEST, not a confident forecast. multi-contour-disjoint with two bars deleted; the only variable is contour count. Predicted broken because the disjointness hypothesis says count is irrelevant. If it measures `ok`, COUNT is the variable and disjointness is not -- that is the informative outcome, not a failure.
path/four-nested-rings              ok       same    # PHASE 1b, THE OTHER HALF. Four concentric alternating-winding rects; runs COUNTS surviving contours (1=outer only, 2=ring, 3=ring+core, 4=correct), so this cell reports HOW MANY the GPU honoured. Predicted ok because two nested contours already render. If it measures broken, COUNT is the variable.
```

- [ ] **Step 3: `tools/gate-vacuity.test.sh`** — the truncated-matrix case greps `"expected 13 case lines, got 12"`; update to 15/14. Verify by running the suite, and check the count is still 29 (no new vacuity cases here — the guards are unchanged, only their numbers).

- [ ] **Step 4: `docs/gc355-vglite-quirks.md`** — two rows in the Phase 1 table marked **PENDING — Phase 1b**, and update the "Open" table in *What IS and IS NOT established* to say the two cases now EXIST and name the boot that will read them. Do NOT pre-write the answer.

- [ ] **Step 5: `CLAUDE.md`** — "thirteen paths/contours/winding cases" → fifteen, "all thirteen `pixel=skip`" → fifteen, and one line in the VGLite section saying Phase 1b's two cases are built and awaiting a boot.

- [ ] **Step 6: Verify and commit**

```bash
cd examples/display/vglite_conformance && ./run_qemu.sh && cp build/vglite_conformance.uart transcript_qemu.txt
cd ../../.. && sh tools/gate-vacuity.test.sh | grep -c "^PASS:"     # 29
./tools/run-all-qemu-gates.sh -l | tail -1                          # 124 gate(s)
LICENSE_AUDIT_EVKB=$(pwd) ./tools/license-audit.sh | tail -1        # PASS
```

Re-capture `transcript_qemu.txt` AFTER the gate is final — a fixture that drifts from its gate goes stale silently and only the vacuity suite sees it.

---

### Task 4: One bench boot

- [ ] **Step 1: Flash — the `.hex`, not the `.elf`**

`LinkServer flash … load` refuses this example's ELF (`code -11`, 0 bytes written, 4/4 reproducible). Clear stale daemons first — `pkill -f LinkServer; pkill -f redlinkserv; pkill -f crt_emu` — and hold nothing on the VCOM while programming.

```bash
/Applications/LinkServer_26.6.137/LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load   build/vglite_conformance.hex
/Applications/LinkServer_26.6.137/LinkServer flash MIMXRT1176:MIMXRT1170-EVKB verify build/vglite_conformance.hex
```

- [ ] **Step 2: Attach the reader on the RIGHT port, then ask for ONE SW4 press**

The MCU-Link VCOM is the port whose suffix matches the probe serial in LinkServer's own log (`[5DQ2DDHVWO5EI]` → `/dev/cu.usbmodem5DQ2DDHVWO5EI3`). Use a generous timeout; a window that closes early costs a press. Captures are classified as binary — use `grep -a`.

- [ ] **Step 3: Read the 2×2 and diff against the pre-registration**

```bash
./tools/vglite-conformance-check.sh examples/display/vglite_conformance/transcript_hw_evkb.txt
```

Read `four-nested-rings`' `runs=` as a COUNT of surviving contours — it is the most informative number in the run.

A second boot is required only if anything reports `repeat=differs` that did not before, or if a Phase 1 cell moves. Phase 1's thirteen must be unchanged; **if any of them moved, that is a bigger finding than the 2×2** and must be chased before the new cases are interpreted.

- [ ] **Step 4: Record**

Update `expected_silicon.txt` (with reasons, never by pasting the transcript), `docs/gc355-vglite-quirks.md` (the Open table becomes an answer, or a sharper question), `CLAUDE.md`, and NEW-32. If the answer identifies the mechanism, say what it licenses for the two compositors — and if it does not, say that plainly too.
