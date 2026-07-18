# CM4 roadmap (LIVING document — update every session)

**Current phase: 3 (IN PROGRESS).** 3.1 (SPI polled master, CM4
self-configured) is **IMPLEMENTED + QEMU-gate-GREEN** 2026-07-18
(`evkb/cm4_spi_test`, spec + plan in `docs/superpowers/`, 4 commits,
license-audited, all reviews passed); ★**HW probe PENDING** — the operator
runs the SDO→SDI jumper + `clean_boot.scp` (the load-bearing clock-gating
proof; QEMU can't prove it — see the 3.1 entry). NOT HW-verified yet. Phases
1 and 2 are DONE — all QEMU-gated + ★★HW-VERIFIED on the EVKB (2026-07-17,
transcripts byte-identical to QEMU), license-audited. Append a dated entry to
the session log whenever anything here changes.

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

**Entry criteria:** Phase 1 merged; its gate green. ✅ met.
**Work items:**
- **2A — real compiled CM4 image + startup/linker.** ✅ DONE, ★★HW-VERIFIED
  2026-07-17 (`evkb/cm4_image_test`, transcript byte-identical to QEMU). A
  real C image (own `cm4/startup_cm4.S` + `cm4.ld`), staged by the Phase-1
  `Multicore.begin()`, that copies `.data` ITCM→DTCM, zeroes `.bss`, enables
  the M4F FPU, uses a DTCM stack, and reports canaries over the MU. Proved on
  silicon the paths Phase 1's leaf blob never touched (CM4-private DTCM
  read/write, `.data` copy, FPU, runtime VTOR relocation). ★★NO qemu2 change
  needed — the machine already aliases CM4 ITCM 0x1FFE0000 / DTCM 0x20000000
  to `ocram_m4` (= system 0x20200000 / 0x20220000), matching the RM; the gate
  confirmed the model handles a real DTCM-resident image. Build:
  `cm4/build_cm4.sh` (bare arm-gcc → objcopy → `cm4_image.h`), wired into the
  gate's CMake.
- **2B — teensy-cmake-macros dual-target build.** ✅ DONE 2026-07-17.
  `teensy_add_cm4_image(<name> LINKER <ld> SOURCES ...)` +
  `teensy_target_link_cm4_image(<exe> <name>)` in `teensy-cmake-macros`
  (MIT): compiles the bare-metal CM4 sources (cortex-m4 hard-float, own
  linker) and emits `<name>.h` (a `uint32_t[]`) via a pure-CMake
  `cm4_bin2header.cmake` (no python), auto-embedded in the CM7 image.
  `cm4_image_test` refactored to use it (per-gate `build_cm4.sh`/`bin2header.py`
  deleted). ★★No EVKB re-probe: the macro-built CM4 `.bin` is BYTE-IDENTICAL
  to the HW-verified 2A image (verified via `cmp`) → nothing changed on
  silicon. Non-breaking (serial_test + cm4_boot_test still build); QEMU gate
  still 9/9. NOT pushed to github/newdigate/teensy-cmake-macros yet (evkb
  gates FetchContent the LOCAL checkout) — push when ready to share.
- **2C — CM4 NVIC + timing.** ✅ DONE, ★★HW-VERIFIED 2026-07-17
  (`evkb/cm4_intr_test`). The CM4 stands up its own DWT CYCCNT (timing base),
  SysTick, and external NVIC IRQ (MU 118). Asserted tokens (boot/run/dwt/
  irqecho) BYTE-IDENTICAL HW vs QEMU: CM4 **DWT timing** works and the CM4
  **handles MU IRQ 118 in its own ISR** (external NVIC dispatch — Phase 1 only
  proved the CM7 side). ★★NO qemu2 change (DWT/SysTick/NVIC all in TYPE_ARMV7M;
  MU IRQ 118 already wired to the CM4 NVIC). Built via the 2B
  `teensy_add_cm4_image` macro (further validates it). ★★SysTick FINDING: the
  CM7 SysTick millis-ISR is unreliable on RT1176 (delay.c, uses DWT instead) —
  this gate measured whether that extends to the CM4: it does NOT, the CM4
  SysTick exception fires + counts reliably (HW systick=0x10D3=4307). `systick`
  is a characterisation token (QEMU 0x2AC5 vs HW 0x10D3 — differs only by time
  base: -icount vs real 400MHz; not asserted). DWT still preferred for CM7
  parity. ★gate uses -icount shift=2 (deterministic SysTick/DWT time base).
  **Still-open qemu2 gap (for peripherals beyond MU):** non-MU peripheral IRQs
  fan out to the CM7 NVIC only — routing one to the CM4 needs a per-line
  TYPE_SPLIT_IRQ (each a new-model risk trigger + probe); 2C did not need it
  (MU already dual-wired, SysTick is per-core).
- **2D — capstone dual-sketch blink + IPC demo.** ✅ DONE, ★★HW-VERIFIED
  2026-07-17 (`evkb/cm4_dual_test`, transcript byte-identical to QEMU).
  CM7 (Arduino) blinks LED_BUILTIN (GPIO3.3) + drives an MU IPC exchange;
  CM4 (real image via the 2B macro) drives its OWN GPIO5.12 output + answers
  IPC with a compute (f(V)=V*3+7) from its interrupt handler. Proves on
  silicon: CM4 GPIO drive, cross-core shared-peripheral visibility (CM7 reads
  the CM4-driven GPIO5), and bidirectional IPC. ★★NO qemu2 change (GPIO
  modelled + reachable from the CM4 view). ★KEY: verify GPIO via DR readback,
  NOT PSR/digitalRead — the qemu2 imxrt_gpio model masks output bits from PSR
  (`psr & ~gdir`), so PSR-of-an-output reads 0 in QEMU but reflects the pad on
  silicon; DR readback is QEMU/silicon-consistent (bit the CM7 LED token first
  tripped on).

**Phase 2 COMPLETE (2026-07-17):** the CM4 boots real compiled sketches
(2A), builds them as a first-class dual target (2B), runs interrupt-driven +
timed code (2C), and drives its own peripherals while doing IPC with the CM7
(2D) — all QEMU-gated and HW-verified. Every deliverable was byte-identical
HW-vs-QEMU (2C's `systick` characterisation token excepted, by design).
**Key 2A discoveries (baked in):** the CM4 DTCM is 128K — reusing the CM7
linker's `_estack = 0x20040000` (256K) bus-faults; cap at 0x20020000. RM
line-18757 ("M4 ITCM + M7 ... DTCM 0x1FFE0000 640KB") is the AHAB
allowable-range table (Table 10-64), column-reversed text — NOT a memory-map
contradiction (0x1FFE0000 = M4 ITCM, 0x20000000 = M4 DTCM stands). CM4
`SystemInit` is near-empty by design (CM7/ROM own clocks/PLL/DCDC/FlexRAM;
CM4 TCM is fixed LMEM, not FlexRAM banks). Full triangulation archived in
`references/phase2a-triangulation.md`.

## Phase 3 — per-library CM4 enablement

**Ordering + architecture LOCKED 2026-07-18 (brainstorming):**
- **Ownership: the CM4 self-configures** the peripheral (ungates its own
  clock, muxes its own pins, configures the block), mirroring the 2D pattern
  where the CM4 drove its own GPIO. Keeps each CM4 image self-contained and
  independently testable. Fires the *clock/power-gating* probe trigger every
  time (a CM4 write to CCM) — that probe is the point, not a tax.
- **Code-org: a distilled C driver that doubles as the probe** (approach B).
  A small C unit with the CM4 image re-expresses this project's own
  HW-verified register/clock sequence (provenance + "keep in sync" header);
  it is both the QEMU gate and the HW probe. No C++ runtime on the lean
  bare-metal CM4 image, zero risk to the HW-verified CM7 library.
- **Consolidation deferred to 3.3** (approach C): once SPI + Wire are proven,
  extract the sequences into a shared C core both the CM7 C++ class and the
  CM4 image call, guarded by byte-identical CM7 gate output (the 2B `cmp`
  discipline). Ends the 3.1/3.2 duplication.

### 3.1 — SPI (LPSPI1) polled master, CM4 self-configured  ◀ CURRENT
**Status:** IMPLEMENTED + QEMU-gate-GREEN 2026-07-18 (`evkb/cm4_spi_test`;
spec + plan in `docs/superpowers/`; 4 commits `2aceb19`→`cb2828f`;
license-audit extended + PASS; per-task + final reviews passed). ★**HW probe
PENDING** (operator: SDO(AD_30)→SDI(AD_31) jumper + `clean_boot.scp`; then add
`transcript_hw_evkb.txt` + flip this to HW-VERIFIED). **Entry criteria:**
Phase 2 done ✅.
**Deliverable:** a new gate `evkb/cm4_spi_test/` (cloned from `cm4_dual_test`):
the CM4 self-configures LPSPI1 (`CCM_LPCG104`/`CCM_CLOCK_ROOT43`, mux AD_28/30/31,
CR/CFGR1/CCR/TCR/MEN) and runs a **polled self-loopback** (SDO→SDI jumper),
streaming observations over MU → CM7 → VCOM. CM4-exclusive LPSPI1; CM7 = boot
+ reporter only.
**QEMU gate:** greps `SPI_CM4=PASS` (`rxok=1` + `cr/cfgr1/a/b/w/buf` match).
Red-first = stub the block-config so no PASS.
**Probe (MANDATORY — clock-gating trigger):** ★★the board's `ssi-loopback`
child echoes on `CR_MEN` alone, ignoring LPCG/root/pins → a clock/pin bug is a
**circular QEMU pass** (FlexCAN-SRXDIS shape). **HW `rx==tx` through the jumper
is the ONLY proof the CM4 self-configured the clock + pins + real SCK.** Run via
`clean_boot.scp`; check in both transcripts. `lpcg/croot` are diagnostic-only.
**No qemu2/core/newdigate-SPI change expected** (LPSPI1 + ssi-loopback + CCM all
modelled and reachable from `cm4_view`; polled avoids the CM7-only-NVIC gap).
**Audit:** add `cm4_spi_test` to `license-audit.sh` GATES (same change), require PASS.

### 3.2 — Wire (LPI2C) polled master, CM4 self-configured  (queued)
Same self-config pattern; needs a target that ACKs — the qemu2 LPI2C
master↔slave-persona loopback (`fsl-imxrt1170.c:1099`) + on-board AT24C02 on
LPI2C1, or a CM4↔CM7 arrangement. Design when 3.1 lands.

### 3.3 — shared C register/clock core (consolidation)  (queued)
Approach C above; byte-identical-CM7 guardrail.

### Deferred beyond Phase 3
- **Interrupt-driven / DMA SPI/Wire on the CM4** — needs a qemu2 per-line
  `TYPE_SPLIT_IRQ` routing the peripheral IRQ to the CM4 NVIC
  (`fsl-imxrt1170.c:961-966`, CM7-NVIC-only today) + eDMA-from-CM4. New-model
  trigger + its own probe.
- **qemu2 clock-gate fidelity** (close the 3.1 circular pass) — enforce
  LPCG/pin-mux in the peripheral/echo path; machine-wide blast radius, HW is
  the arbiter for now.
- **Concurrent CM7+CM4 peripheral use / arbitration** — a cross-core ownership
  protocol.

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
- 2026-07-17: **Phase 2D DONE + HW-VERIFIED — Phase 2 COMPLETE.**
  Capstone `evkb/cm4_dual_test` (built via 2B macro): CM7 Arduino sketch
  (LED blink + IPC driver) + CM4 real image (drives GPIO5.12 + IPC compute
  responder). Transcript byte-identical HW vs QEMU: CM4 GPIO drive,
  cross-core GPIO visibility (CM7 reads CM4-driven GPIO5), bidirectional
  IPC f(0x10)=0x37. ★No qemu2 change (GPIO in system mem, reachable from
  cm4_view). ★★DISCOVERY: qemu2 imxrt_gpio PSR read masks output bits
  (`psr & ~gdir`) → PSR/digitalRead of an OUTPUT reads 0 in QEMU but
  reflects the driven pad on silicon; use DR readback (consistent). License
  audit extended + PASS. Phase 3 (per-library CM4 enablement) is next.
- 2026-07-17: **Phase 2C DONE + HW-VERIFIED.** CM4 NVIC + timing
  (`evkb/cm4_intr_test`, built via the 2B macro): CM4 DWT CYCCNT +
  SysTick + external MU IRQ 118. Asserted tokens (boot/run/dwt/irqecho)
  byte-identical HW vs QEMU — CM4 DWT timing + CM4-side MU-IRQ dispatch
  both HW-proven. ★No qemu2 change (DWT/SysTick/NVIC in TYPE_ARMV7M; MU
  IRQ dual-wired). ★★SysTick characterisation: the CM7 SysTick millis-ISR
  is flaky on RT1176 (delay.c → DWT); measured the CM4 — its SysTick
  exception fires + counts reliably (does NOT reproduce the CM7 trap).
  systick token is characterisation-only (QEMU 0x2AC5 vs HW 0x10D3,
  time-base difference, not asserted). License audit extended + PASS.
  Next = 2D (capstone dual-sketch blink+IPC).
- 2026-07-17: **Phase 2B DONE.** First-class CM4 dual target in
  teensy-cmake-macros (`teensy_add_cm4_image` /
  `teensy_target_link_cm4_image` + pure-CMake `cm4_bin2header.cmake`).
  cm4_image_test refactored onto it; per-gate build_cm4.sh/bin2header.py
  removed. ★No re-probe — macro `.bin` byte-identical to the 2A HW-verified
  image (cmp). Non-breaking regression (serial_test, cm4_boot_test build);
  QEMU gate 9/9. teensy-cmake-macros is MIT, committed locally (not pushed).
  Next = 2C (CM4 NVIC/SysTick).
- 2026-07-17: **Phase 2A DONE + HW-VERIFIED.** Real compiled CM4 image
  (`evkb/cm4_image_test`): own startup/linker, `.data` ITCM→DTCM copy,
  `.bss` zero, M4F FPU, DTCM stack, MU canaries. QEMU gate 9/9 green;
  flashed EVKB (clean_boot.scp) → transcript BYTE-IDENTICAL to QEMU.
  Triangulated SDK cm4 startup/linker + RM + teensy4 idiom (4-reader
  workflow, archived phase2a-triangulation.md). ★NO qemu2 change — the
  machine already models CM4-private ITCM/DTCM as aliases of ocram_m4;
  the real-image gate confirmed it (GPL firewall clean). ★Avoided the
  128K-DTCM `_estack` bus-fault trap; ★resolved the RM line-18757
  false-contradiction (AHAB range table). License audit extended
  (cm4_image_test) + PASS. Next = 2B (dual-target build).
- 2026-07-17: **Phase 1 HW-VERIFIED on the EVKB.** Flashed cm4_boot_test
  via LinkServer; a clean boot (clean_boot.scp, M4 held) produced a VCOM
  transcript BYTE-IDENTICAL to QEMU (all tokens, incl. the
  timing-sensitive gir). Checked in transcript_hw_evkb.txt +
  transcript_qemu.txt + README (evkb). The contaminated autorun also
  passed — bonus evidence D7 (new-VTOR reboot via SW_RESET) works on
  silicon, though not cleanly isolated. Phase 1 fully closed; QEMU model
  confirmed faithful for the whole boot+MU surface. No code changed
  (silicon matched QEMU), so no re-gate needed.
- 2026-07-18: **Phase 3 opened — 3.1 (CM4 self-configured polled SPI)
  DESIGNED + SPEC'd** (brainstorming; spec at
  `docs/superpowers/specs/2026-07-18-cm4-spi-polled-master-design.md`).
  Ordering + architecture LOCKED: CM4 **self-configures** the peripheral;
  code-org = **distilled C driver that doubles as the probe** (approach B),
  shared-C-core consolidation deferred to 3.3. 3.1 = new `evkb/cm4_spi_test`
  (clone of `cm4_dual_test`), CM4-exclusive LPSPI1, polled SDO→SDI loopback,
  MU→CM7→VCOM tokens. ★★KEY DISCOVERY (drives the probe): the board fixture
  `hw/arm/mimxrt1170-evk.c:74-81` attaches an **`ssi-loopback`** echo child to
  LPSPI1's bus, and `imxrt_lpspi_transfer` echoes on `CR_MEN` **alone** —
  ignoring LPCG/clock-root/pin-mux — so a CM4 clock/pin bug is a **circular
  QEMU pass** (FlexCAN-SRXDIS shape); **HW `rx==tx` via the jumper is the only
  proof** the CM4 self-configured the clock+pins+real SCK. Verified for the
  spec: LPSPI1 is reachable from `cm4_view` (`cm4_sysmem` = full alias of
  `system_memory`, `fsl-imxrt1170.c:945-950`); qemu2 CCM is **RAM-backed** so
  LPCG104/ROOT43 **readback matches HW** (`imxrt_ccm.c:100-125`, corrects the
  dualcore-README "unmodelled" note — that was about functional *gating*, not
  readback); peripheral IRQs are **CM7-NVIC-only** (`fsl-imxrt1170.c:961-966`)
  → polled-first. **No qemu2/core/newdigate-SPI change expected.** Next =
  writing-plans → implement → QEMU gate → EVKB probe. D7 still queued.
- 2026-07-18: **Phase 3.1 IMPLEMENTED + QEMU-gate-GREEN** (subagent-driven
  execution of the plan; 4 commits on `master` `2aceb19`→`cb2828f`). New gate
  `evkb/cm4_spi_test` (clone of `cm4_dual_test`): the CM4 self-configures LPSPI1
  (`CCM_LPCG104`+`CCM_CLOCK_ROOT43`, mux AD_28/30/31, CR/CFGR1/CCR/MEN) and runs
  a polled loopback, streaming `cr/cfgr1/lpcg/croot/a/b/w/buf/rxok` over MU TR0 →
  CM7 prints → `SPI_CM4=PASS`. QEMU GREEN + stable 3×; asserted tokens
  `cr/cfgr1/a/b/w/buf/rxok` (`lpcg/croot` informative). CM4 driver is a distilled
  C mirror of `newdigate/SPI` `SPIIMXRT1176.cpp` (provenance header; Phase 3.3
  consolidates). license-audit extended (`cm4_spi_test:cm4_spi_test`) + PASS;
  `cm4_dual_test` + `spi_loopback_test` regressions green. ★★NO qemu2/core/
  newdigate-SPI change (LPSPI1 + the board `ssi-loopback` echo child + CCM all
  reachable from `cm4_view`). Verified via imxrt_lpspi.c/imxrt_ccm.c/
  mimxrt1170-evk.c reads. **★HW PROBE PENDING** — the qemu2 `ssi-loopback` echoes
  on `CR.MEN` alone (ignores LPCG/root/pins), so QEMU is a *circular pass*; only
  the real SDO→SDI jumper proves the CM4's clock-gating + pin-mux. Operator:
  jumper + flash + `clean_boot.scp`, then `transcript_hw_evkb.txt` + flip 3.1 to
  HW-VERIFIED (plan Task 5). D7 still queued.
