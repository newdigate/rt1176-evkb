# GC355 conformance — gradients: implementation plan

> **For agentic workers:** steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** six linear-gradient cases in `display/vglite_conformance`, host-tested
against six model arms, pre-registered, then one silicon boot.

**Spec:** `docs/superpowers/specs/2026-09-02-gc355-conformance-gradients-design.md`

---

### Task 1: stub + model (host side first, so the cases have a rasteriser)

**Files:** `tests/stub/vg_lite.h`, `tests/model.h`

- [ ] Stub gains: `vg_lite_color_ramp_t`, `vg_lite_linear_gradient_parameter_t`,
  `vg_lite_gradient_spreadmode_t`, `vg_lite_filter_t`, `vg_lite_ext_linear_gradient_t`
  (+ `_ext_t` alias), `vg_lite_linear_gradient_t`, `VLC_MAX_GRADIENT_STOPS`,
  `VLC_GRADIENT_BUFFER_WIDTH`, `VLC_MAX_COLOR_RAMP_STOPS` (kept small on the
  host: 16 — only 2 are used), `VG_LITE_INVALID_ARGUMENT`, buffer `handle`,
  `format`, `image_mode`; declarations of the ten driver functions and three
  matrix helpers. Every value real (`inc/vg_lite.h` line-cited).
- [ ] `model.h`: matrix helpers; `vg_lite_allocate/free` on `malloc` with
  `g_ramp_live` (allocated − freed) so a leak is countable; the EXT trio and the
  legacy trio modelled **from the driver's source** (§2); `vgc_draw_linear_grad`
  / `vgc_draw_grad` harness helpers rasterising through a paint callback; arm
  switches `g_draw_black`, `g_paint_follows_path`, `g_solid_first_stop`,
  `g_ramp_permute_rb`.

### Task 2: the cases

**Files:** create `vgc_cases_grad.cpp`; `vgc_harness.h` (extern table + two
helper declarations); `vglite_conformance.cpp` (helpers on the real driver,
run the table); `CMakeLists.txt`.

- [ ] Six cases per spec §3. `grad_profile()` shared. `legacy-linear` carries
  the `count=0` sub-experiment in `detail=` and `api2=`.
- [ ] Target build green; QEMU run prints 26 `skip` lines.

### Task 3: host suite

**Files:** create `tests/cases_grad_test.cpp`; `tests/run.sh`

- [ ] Six arms per spec §4, pinned details, `CHECK(vgc_grad_case_count == 6)`.
- [ ] Demonstrate RED: hard-wire `check_ext_static` to `VGC_OK`; arms 2/3 must
  catch it by name. Restore.

### Task 4: gate, checker, expectation

**Files:** `run_qemu.sh`, `tools/gate-vacuity.test.sh`, `expected_silicon.txt`,
`transcript_qemu.txt`

- [ ] Count 26, six ids by name; vacuity `got 25`; refresh the fixture; the
  gate and the vacuity suite green.
- [ ] `expected_silicon.txt` GRADIENTS block, **before the press**.

### Task 5: silicon

- [ ] Flash the `.hex` (LinkServer refuses this example's `.elf`), two boots,
  `tools/vglite-conformance-check.sh` on both; every refuted prediction gets a
  written reason, never a pasted transcript.
- [ ] Quirks doc rows, CLAUDE.md, memory, Linear.
