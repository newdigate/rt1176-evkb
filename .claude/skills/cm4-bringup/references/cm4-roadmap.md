# CM4 roadmap (LIVING document — update every session)

**Current phase: 2 (ready to start).** Phase 1 is DONE — QEMU-gate-green,
license-audited, and ★★HW-VERIFIED on the EVKB (2026-07-17, transcript
byte-identical to QEMU). Append a dated entry to the session log whenever
anything here changes.

## Phase 1 — CM7 boots the CM4 + MU IPC library  ✅ DONE (HW-verified)

**Entry criteria:** none — qemu2 models the CM4/MU/SRC, EVKB-validated
(dualcore_mu_test transcripts, 2026-07-16).
**Deliverables — SHIPPED:**
- `cores/imxrt1176/Multicore.{h,cpp}` — `Multicore.begin(image, bytes,
  stageAddr=0x20200000)` stages a CM4 blob into the TCM backdoor,
  programs `IOMUXC_LPSR_GPR0/GPR1` (GPR0 = VTOR & 0xFFF8, GPR1 =
  VTOR>>16), defensively ungates CCM LPCG1, pulses
  `SRC.CTRL_M4CORE.SW_RESET`, then sets `SRC.SCR.BT_RELEASE_M4`
  (write-1-only boot edge, first call only). `restart()` re-pulses
  CTRL_M4CORE.SW_RESET (same VTOR); `running()` reads
  `SRC.STAT_M4CORE.UNDER_RST` (bit 0 — the reliable hold indicator;
  ASR.RS is dead on silicon).
- `cores/imxrt1176/MessagingUnit.{h,cpp}` — the `MU` global (Processor A
  / MUA). Mailbox `send/trySend/receive/tryReceive/available` (TR/RR 4
  ch), doorbell `trigger/triggerPending/doorbell/acknowledge` (GIR/GIP
  with ack-side GIR auto-clear), flags `writeFlags/readFlags` (Fn),
  interrupt-driven `onReceive/onDoorbell` on IRQ 118, `status()` masks
  the always-set silicon `SR` bit 9. Classic single-SR/CR `mu` flavor
  (RT1176 does NOT build `mu1`).
- Register defs live in `imxrt1176.h` (+ mirrored into
  `tools/gen_imxrt1176_h.py`); `IRQ_MU=118` added to `core_pins.h`.
  NOTE: the generator has pre-existing drift vs the committed header
  (hand-edited ADC/DAC blocks) — do NOT regenerate blindly.
**Gate:** `evkb/cm4_boot_test/` (run_qemu.sh, gate-lib.sh pattern) —
boots the embedded blob via the library and exercises mailbox, doorbell
(incl. GIR auto-clear), IRQ-118 receive, and restart; 10/10 tokens green,
stable 3×. License audit extended (`cm4_boot_test` added to GATES) and
PASS. No qemu2 change was needed (the machine already models everything),
so the GPL firewall is trivially clean this phase.
**No new probe was triggered:** every register access the library makes
was already HW-verified on this exact core+startup by dualcore_mu_test
(2026-07-16) — boot release, VTOR=0x20200000, GPR masking, LPCG1
already-on, STAT_M4CORE hold, MU both directions, GIR/GIP auto-clear, IRQ
118 (irqcnt=1), ASR bit9/bit7 quirks, SW-reset restart, dsb/isb-sufficient
staging. Triangulation (RM + MCMGR + fsl_mu + Zephyr) is archived in
`references/phase1-triangulation.md`.
**HW confirmation — DONE 2026-07-17:** flashed `cm4_boot_test` on the
EVKB (LinkServer + clean_boot.scp for an uncontaminated M4-held boot);
the clean-run VCOM transcript is BYTE-IDENTICAL to QEMU (all deterministic
tokens; the timing-sensitive `gir` also matched). Checked in as
`cm4_boot_test/transcript_hw_evkb.txt` + `transcript_qemu.txt`. Clean-boot
snapshot confirmed `SCR=0`, `STAT_M4=1` (held), `MUA_SR=0x00F00200`
(bit9 set / bit7 clear) before dispatch.
**D7 (reboot CM4 at a NEW VTOR) — now LIKELY-WORKS, still not cleanly
isolated:** in the *contaminated* post-flash autorun the CM4 was
pre-released by LinkServer at its spin address, so `begin()` rebooted it
at the new VTOR 0x20200000 purely via the `CTRL_M4CORE.SW_RESET` pulse —
and it produced the full correct transcript (hello…DONE). Strong
suggestion D7 works on silicon, but LinkServer's exact CM4 state is a
confound; keep D7 queued for a clean controlled probe before a
hot-swap-CM4-image feature depends on it.

## Phase 2 — CM4 core variant (sketches compile for the CM4)

**Entry criteria:** Phase 1 merged; its gate green.
**Deliverables:** CM4 startup + linker (vector table at the backdoor
address, code/data in CM4 TCM views: ITCM 0x1FFE0000 / DTCM 0x20000000,
128K each), NVIC/SysTick (218 ext IRQs, 4 prio bits, 400 MHz),
`teensy-cmake-macros` dual-target build, dual-sketch blink + IPC demo as
the gate.
**Known qemu2 limitation to work through the loop:** peripheral IRQs fan
out to the CM7 NVIC only (MU excepted). Split-irq wiring lands in qemu2
per-line as CM4 libraries need it — each such change is a
new-model-behavior risk trigger (probe).

## Phase 3+ — per-library CM4 enablement

Wire/SPI/etc., each validated against the SDK bare-metal equivalent and
gated in QEMU. Ordering decided when Phase 2 lands. Each entry records:
entry criteria, deliverables, QEMU gate, probe obligations (met/queued),
audit status.

## Queued hardware checks

- Derive and EVKB-validate a minimal LPUART1 init for the asm probe
  template (templates/probe_firmware/), so probes print on clean-boot
  silicon without the Arduino core (queued 2026-07-17).
- **D7 (Phase 1): reboot CM4 at a NEW VTOR.** Re-pulsing
  CTRL_M4CORE.SW_RESET after reprogramming GPR0/1 to a *different*
  address. LIKELY works (observed in the cm4_boot_test contaminated
  autorun, 2026-07-17) but not cleanly isolated (LinkServer CM4-state
  confound). Do a controlled probe before any hot-swap-CM4-image feature
  relies on it. Queued 2026-07-17.
- ~~Phase 1 library HW transcript~~ — DONE 2026-07-17 (cm4_boot_test
  transcript_hw_evkb.txt, byte-identical to QEMU).

## Session log

- 2026-07-17: roadmap created; Phase 1 ready. Background: CM4/MU/SRC
  modelled in qemu2 and EVKB-validated; boot-ROM XMCD/DCD executes in
  QEMU; NXP MCMGR hello_world and rpmsg_lite_pingpong byte-identical
  QEMU-vs-board.
- 2026-07-17: **Phase 1 implemented + QEMU-gate-green.** Triangulated
  RM + MCMGR (mcmgr_internal_core_api_imxrt1170.c) + fsl_mu (classic
  `mu` v2.3.3, NOT `mu1`) + Zephyr (imxrt11xx/soc.c) via a 4-reader
  workflow — all converge on the silicon truth, no material
  contradictions (archived: `references/phase1-triangulation.md`).
  Shipped `Multicore.{h,cpp}` + `MessagingUnit.{h,cpp}` in
  cores/imxrt1176, register defs into imxrt1176.h (+generator) and
  IRQ_MU=118 into core_pins.h. New gate `evkb/cm4_boot_test` (10/10
  tokens, 3× stable). No qemu2 change (machine already models it) → GPL
  firewall clean. License audit extended + PASS. No new probe mandated
  (all deps HW-verified by dualcore_mu_test on this core). Queued: D7
  (new-VTOR reboot) + library HW transcript. Discoveries: (a) MCMGR
  reads MUA->CR bit7 for InReset — a reference BUG on RT1176 (RS never
  sets); use STAT_M4CORE bit0. (b) gen_imxrt1176_h.py has drifted from
  the committed header (hand-edited ADC/DAC) — do not regenerate blindly.
  Phase 2 (CM4 core variant / dual-target build) is now unblocked.
- 2026-07-17: **Phase 1 HW-VERIFIED on the EVKB.** Flashed cm4_boot_test
  via LinkServer; a clean boot (clean_boot.scp, M4 held) produced a VCOM
  transcript BYTE-IDENTICAL to QEMU (all tokens, incl. the
  timing-sensitive gir). Checked in transcript_hw_evkb.txt +
  transcript_qemu.txt + README (evkb). The contaminated autorun also
  passed — bonus evidence D7 (new-VTOR reboot via SW_RESET) works on
  silicon, though not cleanly isolated. Phase 1 fully closed; QEMU model
  confirmed faithful for the whole boot+MU surface. No code changed
  (silicon matched QEMU), so no re-gate needed.
