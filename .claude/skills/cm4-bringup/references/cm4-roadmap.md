# CM4 roadmap (LIVING document — update every session)

**Current phase: 1 (ready to start).** Append a dated entry to the
session log below whenever anything here changes.

## Phase 1 — CM7 boots the CM4 + MU IPC library

**Entry criteria:** none — qemu2 models the CM4/MU/SRC, EVKB-validated
(dualcore_mu_test transcripts, 2026-07-16).
**Deliverables:**
- CM7-side Arduino API: embed a CM4 image and boot it. The
  silicon-validated sequence: copy image to 0x20200000 (the TCM
  backdoor), `IOMUXC_LPSR_GPR0/GPR1` = VTOR (use the SYSTEM address
  0x20200000 — a 0x1FFE0000 TCM-window VTOR does not boot on silicon),
  `SRC->CTRL_M4CORE = SW_RESET`, `SRC->SCR |= BT_RELEASE_M4`.
  Hold-state indicator: `SRC STAT_M4CORE` bit 0 (ASR.RS is dead on
  silicon).
- MU mailbox/doorbell library: TR/RR (4 ch), GIR/GIP with ack-side GIR
  auto-clear, TIE/RIE/GIE, IRQ 118 both cores, `ASR` bit 9 quirk noted.
**Gates:** run_qemu*.sh-style gate (gate-lib.sh pattern); behavior
cross-checked against MCMGR (`mcuxsdk`) and `dualcore_mu_test`
transcripts; probe any register behavior not already covered by those
transcripts; license audit if any tree is added or files are vendored.

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

## Session log

- 2026-07-17: roadmap created; Phase 1 ready. Background: CM4/MU/SRC
  modelled in qemu2 and EVKB-validated; boot-ROM XMCD/DCD executes in
  QEMU; NXP MCMGR hello_world and rpmsg_lite_pingpong byte-identical
  QEMU-vs-board.
