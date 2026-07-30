# Brainstorm: vsync ISR + async flush_ready, and the touch test goes double-buffered

Start by invoking the `superpowers:brainstorming` skill — this is a design
conversation, not an implementation request. Explore before proposing.

## The goal

Two follow-ons deferred from v4 (`2026-07-30-rt1176-lvgl-double-buffer-design.md` §2),
to be taken **only after v4 is complete and hardware-verified**:

1. **The vsync ISR refinement.** Replace v4's polled `flush_wait_cb` with the canonical
   async LVGL pattern: the LCDIFv2 vsync interrupt calls `lv_display_flush_ready()`,
   so LVGL never busy-waits on a flip. This is the **first LCDIFv2 interrupt on
   silicon** in this tree — the reason it was staged out of v4, per the v2/v3 rule
   (no interrupt in a load-bearing path on first contact).
2. **Migrate `lvgl_rk055_touch_test` to `lvgl_mipi_panel_create_db`**, retiring the
   tearing-accepted caveat from the v3 evidence. Re-records that example's pre-touch
   scene golden; the v3 transcripts stay as history.

## Where things are (as of v4's design, 2026-07-30 — re-verify, don't trust)

- v4 spec: `docs/superpowers/specs/2026-07-30-rt1176-lvgl-double-buffer-design.md`.
  Its §3 primitives (`lcdifv2VsyncArm/Seen`, `lcdifv2FlipTo`) and §5 QEMU model change
  (shadow latch at vsync, self-clear) are the foundation this session builds on.
  **Check v4 actually shipped before designing anything.**
- The QEMU model raises `INT_VSYNC` in both domains and IRQ 55 follows domain 0
  (`qemu2 hw/display/imxrt_lcdifv2.c:171,201-205`). The **real** CM7 IRQ number for
  LCDIFv2 must be read from the RT1176 RM / core headers, not from the model.
- `lv_display_flush_ready()` from ISR context: widely used pattern, but verify
  ISR-safety against the vendored LVGL version's `flushing` flag handling, and think
  about `volatile`/barrier needs — `lvgl_mipi_panel.cpp`'s statics are deliberately
  non-volatile today with a comment saying an ISR path must revisit that. **That
  comment is the checklist.**
- Memory: `rt1176-lvgl-touch-v3-design` (this project's memory dir) and
  `rt1176-rk055-display` / `rt1176-rk055-touch` (parent workspace memory).

## Things to think hardest about

- **ISR-vs-thread races in the binding.** flip-pending, vsync counters, and LVGL's
  `flushing` flag all become cross-context. The v3 binding's "not volatile, single
  thread" reasoning inverts here.
- **What a QEMU gate can prove about an ISR.** The model's vsync is a 60 Hz timer —
  interrupt delivery is testable, latency is fiction. Decide what is QEMU-provable
  vs hardware-only, and say so (the standing asymmetry discipline).
- **The touch-test migration re-records its golden.** Per the re-record rule: stable
  across two runs AND a human eye on the glass, in the same commit.
- **Do not** let the ISR session grow into PXP or XRGB8888 — those remain their own
  milestones with their own driving measurements.
- Small model debt from the v4 final review, decide-or-defer: the QEMU LCDIFv2 paint
  path gates on enabled/geometry but not `layer0_active_addr != 0`, so a GUI repaint
  in the sub-frame window before the first latch reads guest address 0 — unreachable
  by any gate (`-display none`), but the state-struct comment promises "dark until
  the first load" and the code does not implement it.

## Non-negotiable project conventions

Two-gate rule; silicon wins; assertions must not read stronger than they are; MIT/BSD
firewall (`tools/license-audit.sh` — new gate ⇒ `GATES` entry); run gates as
`./run_qemu.sh`; check `uptime`/`ps` before gate runs; VCOM-free flashing ritual;
read `docs/KNOWN-BROKEN-GATES.md` before any sweep (expectation 71 gates if v4
shipped its example; re-measure, don't assume).

## What I want out of that session

A validated design written to `docs/superpowers/specs/`, then its own plan. Ask
questions one at a time. When all outstanding tasks are complete, delete this file.
