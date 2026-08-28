# GPU well Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move the well to the GC355 in gpu mode (sw path byte-identical),
add the rk_fps phase, and measure ≥30 fps at the all-16 delta workload on
silicon. Spec: `docs/superpowers/specs/2026-08-28-rotary-knob-gpu-well-design.md`.

**Branch:** `nicnewdigate/new20c-rotary-knob-gpu-well`; SynthUI local-first.

### Task 1: SynthUI — DRAW_MAIN paints nothing in gpu mode
- [ ] In `rk_draw`, move the `synthui_rotary_gpu_enabled` check to the TOP
  (mark `gpu_pending`, return before any drawing). sw path unchanged.
- [ ] Build `synthui_step_test` + host tests; commit.

### Task 2: SynthUI — compositor draws the well
- [ ] Bump arena (static, reset after finish, sticky overflow → `s_err`).
  Well emitters: `emit_ring_caps` for the bounded track (ring 41.5..44.5
  min→max + cap discs r1.5 at P(43,min), P(43,max)); endless border ring
  r(39−bw)..39, bw 3 (focus) / 1.6. Disc r39 shared with `emit_circle`.
- [ ] Per pending knob: emit well paths into the arena with per-knob state
  (mode, focus, min/max), then in `composite_minus` draw well paths
  (`m_fixed`) before the 3 rotor paths. Reset arena after finish.
- [ ] Build `synthui_knob_test`; QEMU gate green with UNCHANGED goldens and
  delta guards (the stability IS the assertion). Commit.

### Task 3: rk_fps phase in synthui_knob_test
- [ ] After `SYNTHUI_KNOB_DONE`: build the 4×4 grid, 15 ms timer advancing
  all 16 angles (widget delta path), 64 damage-gated
  REFR_START→REFR_READY samples (RENDER_READY latch, skip the screen-load
  frame — the bench's method), print
  `rk_fps frames=64 mfps_med= us_med= us_min= us_max= engine=`, then hero.
- [ ] QEMU gate green (stops at DONE; rk_fps ungated). Commit + re-capture
  fixture only if the pre-DONE output changed (it must not).

### Task 4: Silicon
- [ ] Flash knob_test: equality guard PASS on gpu, `rk_gpu_err=0`, new gpu
  golden set ×2 boots, **rk_fps mfps_med ≥ 30000** (else: implement the
  union-bbox reserve lever and remeasure). Held-frame dump eyeballed.
- [ ] Update transcript (supersede note for the golden set), restore
  acid_box to flash.

### Task 5: Close out
- [ ] Rebuild + run the four display gates idle; full sweep; vacuity;
  audit. CLAUDE.md sentence. SynthUI push + pin bump + FORCE_FETCH gate
  run. Merge to master, push. Linear addendum. Memory.
