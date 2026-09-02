# GC355 conformance — Phase 3 (images, blits & scissor): implementation plan

> **For agentic workers:** steps use checkbox (`- [ ]`) syntax for tracking.

**Spec:** `docs/superpowers/specs/2026-09-02-gc355-conformance-phase3-design.md`

### Task 1: stub + harness + target

- [ ] `tests/stub/vg_lite.h`: `vg_lite_map_flag_t`, `vg_lite_buffer_layout_t`,
  `VG_LITE_RGB565`, buffer fields `format`/`tiled`/`image_mode`,
  `vg_lite_rectangle_t`, declarations of `vg_lite_blit`, `vg_lite_set_scissor`,
  `vg_lite_map`.
- [ ] `vgc_harness.h`: `vgc_small`, `vgc_px_small`, `vgc_clear_small`,
  `vgc_draw_path_to`, `vgc_blit`, the blit table externs, `VGC_SMALL_W/H`.
- [ ] `vglite_conformance.cpp`: allocate/map `vgc_small`; the three helpers;
  run the blit table after the gradient table.
- [ ] `CMakeLists.txt`: `vgc_cases_blit.cpp`.

### Task 2: the cases — `vgc_cases_blit.cpp`

- [ ] Six cases per spec §3; sources built on the CPU into EXTMEM buffers,
  mapped once. Target build green; QEMU prints 32 skip lines.

### Task 3: model + host suite

- [ ] `model.h`: scissor state, second fb, regime-aware `vgc_draw_path_to`,
  `vgc_blit` with the driver's stride check and format sampling, four new arm
  switches.
- [ ] `tests/cases_blit_test.cpp`, six arms, pinned details. `run.sh` seventh
  suite. Demonstrate RED, restore.

### Task 4: gate, vacuity, expectation

- [ ] `run_qemu.sh` 32; vacuity 32/31; fixture; `expected_silicon.txt` PHASE 3
  block before the press.

### Task 5: silicon, docs

- [ ] Flash `.hex`, two boots, checker on both; quirks doc, CLAUDE.md, memory,
  Linear, sweep + audit (sequentially), merge, push.
