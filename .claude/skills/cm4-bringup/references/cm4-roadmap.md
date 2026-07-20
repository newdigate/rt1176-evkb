# CM4 roadmap (LIVING document — update every session)

**Current phase: 4 — 4.1 + 4.2 (interrupt Wire) + the eDMA_LPSR DMA-Wire
milestone ★★ALL HW-VERIFIED (2026-07-19); 4.3 SPI-DMA is a polled CM4 result +
the two-eDMA finding.** Phase 4 = "Interrupt-driven / DMA SPI/Wire on the CM4".
★★**BIGGEST silicon truth of the phase — the RT1176 has TWO eDMAs** (RM Tables
4-1/4-2, HW-confirmed): the **main eDMA** (`0x40070000`) channel IRQs go to the
**CM7 only**; **eDMA_LPSR** (`0x40C14000`) IRQs go to the **CM4**. The 4.1
"eDMA split→both NVICs" foundation was a fiction for the main eDMA. Exposed by
the 4.3 SPI-DMA HW probe (`cm4_spi_dma_test`: CM4-driven main-eDMA data works,
`rxb/rxa=1`, but `dmairq=0` — the completion IRQ went to the CM7). **Pivot
(user-directed) DONE + ★★HW-VERIFIED**: qemu2 gained `eDMA_LPSR` +
`DMAMUX1/LPSR` device instances (IRQs→CM4; LPI2C5 dma-req→source 52; main eDMA
corrected to CM7-only), and `cm4_wire_dma_test` DMA-reads the WM8962 device ID
over LPI2C5 via eDMA_LPSR with the completion IRQ on the CM4's OWN NVIC
natively — HW `rdv=0x6243`, `dmairq=2`, wiring-free, stable 3× (7b949f3). That
is the genuine "interrupt-driven DMA on the CM4" (Phase 4's DMA goal), on the
correct eDMA. See [[rt1176-cm4-edma-lpsr-split]]. `cm4_spi_dma_test` stays the
polled main-eDMA SPI-DMA result. Original plan: 4 slices (4.1 int Wire-master,
4.2 int Wire-slave, 4.3 DMA SPI, 4.4 DMA Wire) — 4.3/4.4 subsumed by the
eDMA_LPSR reality. **4.2 DONE + HW-VERIFIED**: the
CM4 runs an interrupt-driven LPI2C1 slave @0x42 (shared-core `lpi2c1176_slave_*`
+ distilled `handle_slave_isr`); an external Arduino MKR-Zero master writes
{A5 5A C3} and reads back the slave's 0x3C (EVKB `irqcnt=0x0C`, `b0/b1/b2`
correct, `resp=3C`, PASS; master `wr=0 rd=3C`). World-split instance (LPI2C2
persona/IRQ 33 in the QEMU gate via the existing loopback bridge, LPI2C1/IRQ 32
on HW); qemu2 delta = the one LPI2C2 split. ★★HW-DEBUG FINDING: `A5`=`AD_08`=
`LPI2C1_SCL` is ALSO `USB_OTG2_ID` — a USB OTG adapter on OTG2 grounds ID and
clamps SCL to 0V (0Ω board-off), silently killing header I2C; unplug OTG2
(see the `rt1176-a5-ad08-otg2-id-short` memory). QEMU model limit: the
master-observed read byte races cross-vCPU (no TXDSTALL modeled), so the QEMU
gate asserts the deterministic slave-side `resp` and leaves `mrd` to the HW
oracle. **Foundation + 4.1 DONE + HW-VERIFIED**:
the qemu2 machine now fans `LPSPI1`/`LPI2C5`/16 eDMA IRQ lines to BOTH NVICs
(targeted `TYPE_SPLIT_IRQ` via `fsl_imxrt1170_connect_irq_both`; realized
connection-free, then wired; whole regression set green), and `cm4_wire_int_master_test`
proves the CM4 takes the LPI2C5 IRQ on its OWN NVIC (irqcnt>0) and reads the
WM8962 device ID (`rdv=00006243`) via an interrupt-driven read — the EVKB
clean-boot transcript byte-identical to QEMU on all tokens except the two
world-varying (`irqcnt` >0-only; `rdv` 0000/6243). **★★KEY silicon-truth
discovery** (only the cold-boot HW probe could expose it — the QEMU wm8962-stub
reads 0x0000 for everything): a fully-ISR master that issued the repeated START
from the ISR the instant the write cursor drained RACED the last register-
pointer byte still clocking on a cold bus, so the WM8962 never latched the
pointer → read the wrong register (rdv=0x0000) with a FALSE PASS. Fixed by
sequencing write→read as the HW-verified polled core does (polled reg-pointer
write + TDF-wait before the repeated START; the DATA READ stays interrupt-driven).
**Phase 4 COMPLETE** — interrupt Wire master (4.1) + slave (4.2) + the
eDMA_LPSR DMA-Wire milestone, all HW-VERIFIED; the two-eDMA finding recorded.
**D7 (CM4 runtime hot-swap) RESOLVED + ★★HW-VERIFIED (2026-07-20)** — the last
open Phase-1 item is closed (`cm4_hotswap_test`, clean-boot probe, `31c0d53`).
Next: **a new capability** (the CM4 bring-up backlog — interrupt/DMA Wire+SPI,
dual-core boot, and hot-swap — is all HW-verified). Phase 3 (3.1/3.2/3.3)
COMPLETE (2026-07-18); Phases 1-2 DONE + ★★HW-VERIFIED (2026-07-17). Append a
dated entry to the session log whenever anything here changes.

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
**D7 (reboot CM4 at a new VTOR / restart a running CM4) — ★★RESOLVED +
HW-VERIFIED (2026-07-20).** `cm4_hotswap_test`: the CM7 boots the CM4 with
image A (identity `0xA1A1A1A1`), then `Multicore.begin(imageB)` re-pulses
`SRC_CTRL_M4CORE.SW_RESET` and the *running* CM4 reboots into image B
(`0xB2B2B2B2`). The clean-boot probe removed the old confound: the
`clean_boot.scp` snapshot showed the CM4 **genuinely held** (`SCR=0`,
`STAT_M4CORE=1`) before the CM7 ran, so `begin(A)` was the first clean release
and `begin(B)` the true hot-swap edge. HW `idA=A1A1A1A1 → idB=B2B2B2B2`,
`HOTSWAP=PASS`, stable 2× (`31c0d53`). **No qemu2 or library change** — the
existing `Multicore::begin` (SW_RESET on every call) + qemu2
`fsl_imxrt1170_cm4_boot` (re-reads GPR0/1, `cpu_reset`s a running CM4) already
supported it. Unlocks runtime CM4 firmware swapping. A "two resident images at
different VTORs" variant needs only a second CM4 linker layout (GPR0/1 reprogram
already works). See [[rt1176-cm4-boot-mu]].

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

### 3.1 — SPI (LPSPI1) polled master, CM4 self-configured  ✅ DONE
**Status:** ✅ DONE + ★★HW-VERIFIED 2026-07-18 (`evkb/cm4_spi_test`; spec +
plan in `docs/superpowers/`; license-audit extended + PASS; per-task + final
reviews passed). The EVKB clean-boot VCOM transcript is **BYTE-IDENTICAL to
QEMU** (`transcript_hw_evkb.txt` == `transcript_qemu.txt`, md5 `b2364766…`, 168
bytes incl. CRLF) — `rxok=1`/`SPI_CM4=PASS` through the SDO(AD_30)→SDI(AD_31)
jumper, closing the circular-pass gap (QEMU's `ssi-loopback` echoes on `CR.MEN`
alone). Observed `lpcg=1`, `croot=0` (match QEMU — no HW status-bit surprise).
No code changed after the QEMU gate, so no re-gate needed. **Entry criteria:**
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

### 3.2 — Wire (LPI2C5) polled master, CM4 self-configured  ✅ DONE
**Status:** ✅ DONE + ★★HW-VERIFIED 2026-07-18 (`evkb/cm4_wire_test`; spec +
plan in `docs/superpowers/`; commits `0331c2a`(RED)→`b74918e`(GREEN)→
`cbd89da`(audit)→`a37a7bc`(README); license-audit PASS; all reviews passed).
**Wiring-free clean-boot run (SCR=0, STAT_M4=1, MUA_SR=0x00F00200):** every
asserted token (`mcr/ack/nack/rdn/done` + PASS lines) byte-identical to QEMU,
`lpcg/croot` matched too, and the full-file diff is EXACTLY the one designed
line — **`rdv=00000000`(stub) → `rdv=00006243`(silicon)**: the real WM8962
answered its device ID (= the Linux reg_default fact) over a bus whose clock
(LPCG102, ROOT41 **mux 1**) and **LPSR pads** the CM4 configured itself —
first HW-proven CM4-driven LPSR-domain peripheral, circular-pass gap closed.
No code changed after the QEMU gate → no re-gate. **Entry criteria:** 3.1
done ✅.
**Target chosen (brainstorming):** **LPI2C5 + the real on-board WM8962 @0x1A**
(not AT24C02/slave-persona — those need external HW or cross-core coupling).
The probe is **wiring-free** (codec soldered on; flash + `clean_boot.scp` only).
**Deliverable:** `evkb/cm4_wire_test/` (clone of `cm4_spi_test`): CM4
self-configures LPI2C5 (LPCG102 @0x40CC6CC0, ROOT41 @0x40CC1480 = **mux 1**,
LPSR pads GPIO_LPSR_05/04 mux 0x10 pad 0x0A — ★first CM4-driven LPSR-domain
peripheral) + three polled transactions: reset-write R15←0x6243 (`ack=0`),
absent-addr 0x2A probe (`nack=2`; avoids WM8962 0x1A + FXLS8974 0x18),
ID read-back R15 (`rdn=2`).
**★rdv split-assertion:** HW asserts `rdv=0x6243` (R15 readback default =
device ID; Linux wm8962.c reg_default `{15, 0x6243}`, fetched 2026-07-18,
FACT-only per license firewall); QEMU asserts `rdv=0x0000` (wm8962-stub
contract). The ONE expected cross-world divergence (precedent: 2C systick).
★qemu2 LPI2C NACK path already silicon-corrected (NDF trails TDF) → nack
token assertable byte-identically. **No qemu2/core/Wire-lib change expected.**
**Probe (MANDATORY):** clock-gating (CM4 writes CCM, mux 1 = new) + LPSR
address-map trigger. **Audit:** add `cm4_wire_test` to GATES same-change.

### 3.3 — shared C register/clock core (consolidation)  ✅ DONE
**Status:** ✅ DONE 2026-07-18 (spec + plan in `docs/superpowers/`; commits:
macros `0b50945`, SPI `eefd879`, Wire `aa58de2`, evkb `f0b5c3e`+`b0d46da`).
Approach C realized: `SPI/lpspi1176.{h,c}` + `Wire/lpi2c1176.{h,c}` hold the
HW-verified sequences (CM7-logic-verbatim: begin/end/set-clock/transfer;
I2C wait_flag/bus_recover/master write+read) as freestanding C — offset-
asserted register overlays cross-`static_assert`ed against
`IMXRT_LPSPI_t`/`IMXRT_LPI2C_t` in the library .cpp; addresses flow in via a
pointer desc struct (CM7: imxrt1176.h macros; CM4: the same literals in the
gate's instance table). `SPIClass`/`TwoWire` delegate (Wire slave block stays
C++/NVIC); the CM4 mains keep only MU scaffolding + desc + token flow.
`teensy_add_cm4_image` gained optional `INCLUDE_DIRS` (compile-only `-I`;
absent ⇒ byte-identical command lines, cm4_dual `.cm4.bin` cmp-proven).
**Guardrail held everywhere:** cm4_spi/cm4_wire QEMU transcripts byte-
identical to the checked-in `transcript_qemu.txt` (stable 3×);
spi_loopback/spi_dma/wire_master/wire_slave uarts byte-identical to
pre-refactor baselines; st7735 + wire_oled + sd_test + sd_wav_play build
green; license-audit PASS (cm4 gate manifests grew 103→105 files = the
shared core is inside the sweep). **Silicon anchor:** wiring-free
`cm4_wire_test` EVKB re-probe (flash + clean_boot.scp) BYTE-IDENTICAL to
`transcript_hw_evkb.txt` incl. `rdv=00006243` — the shared `lpi2c1176.c`
begin/write/read paths proven on silicon. **`cm4_spi_test` re-probe DONE**
(jumper refitted 2026-07-18): clean-boot capture BYTE-IDENTICAL to
`transcript_hw_evkb.txt` (md5 `b2364766…`, == QEMU too) — `rxok=1`,
`SPI_CM4=PASS`, so the shared `lpspi1176.c` begin/transfer paths drove a real
4 MHz SCK through the SDO(AD_30)→SDI(AD_31) jumper. Both shared cores now
silicon-anchored; no code changed (silicon == QEMU) → no re-gate. Known
deltas absorbed by
design (spec §2 D1–D5): the CM4 images gained the CM7 stream's inert
`CR/MCR=0` write and the MCFGR1 RMW — every write value/order is inside the
CM7 HW-verified stream on the same block instances.

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

- ~~cm4_spi_test jumpered re-probe of the 3.3 shared-core binary~~ — DONE
  2026-07-18. Jumper refitted; clean-boot capture BYTE-IDENTICAL to
  `transcript_hw_evkb.txt` (md5 `b2364766…`), `rxok=1`/`SPI_CM4=PASS`. The
  shared `lpspi1176.c` is now silicon-proven through the SDO→SDI jumper.
- Derive and EVKB-validate a minimal LPUART1 init for the asm probe
  template (templates/probe_firmware/), so probes print on clean-boot
  silicon without the Arduino core (queued 2026-07-17).
- ~~**D7 (Phase 1): reboot CM4 at a new VTOR / restart a running CM4.**~~ —
  ★★DONE + HW-VERIFIED 2026-07-20 (`cm4_hotswap_test`, clean-boot controlled
  probe: CM4 held `SCR=0`/`STAT_M4=1`, then `begin(A)`→idA, `begin(B)` re-pulse
  SW_RESET→idB; `HOTSWAP=PASS` 2×, `31c0d53`). The LinkServer confound is gone.
  Unlocks runtime CM4 firmware hot-swapping.
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
- 2026-07-18: **Phase 3.1 ★★HW-VERIFIED on the EVKB — Phase 3.1 COMPLETE.**
  Flashed `cm4_spi_test` + clean boot (`clean_boot.scp`, M4 held: `SCR=0`,
  `STAT_M4=1`, `MUA_SR=0x00F00200` — uncontaminated) with the SDO(AD_30)→
  SDI(AD_31) jumper. The VCOM transcript is **BYTE-IDENTICAL to QEMU**
  (`transcript_hw_evkb.txt` == `transcript_qemu.txt`, md5 `b2364766…`): the
  CM4-driven polled loopback returned `a=A5 b=3C w=BEEF buf=DEADBEEF rxok=1`,
  `SPI_CM4=PASS`. ★★This closes the circular-pass gap — the qemu2 `ssi-loopback`
  echoes on `CR.MEN` alone, so `rxok=1` on silicon (via the physical jumper) is
  the proof the CM4 itself ungated `CCM_LPCG104`, set `CCM_CLOCK_ROOT43`, muxed
  AD_28/30/31, and drove a real 4 MHz SCK. `lpcg=1`/`croot=0` matched QEMU (CCM
  RAM-model faithful, no HW status-bit surprise). ★HW-capture note: the raw
  `/tmp/hw.uart` had a leading `\0` block (console reconnect after the reset's
  USB re-enum); stripped the nulls, kept the raw `\r\n` → byte-identical.
  Committed `transcript_hw_evkb.txt` + this roadmap. No code changed (silicon ==
  QEMU) → no re-gate. **Phase 3.1 done; next = 3.2 (Wire/I2C on the CM4).** D7
  still queued.
- 2026-07-18: **Phase 3.2 DESIGNED + SPEC'd** (brainstorming; spec at
  `docs/superpowers/specs/2026-07-18-cm4-wire-polled-master-design.md`).
  Target LOCKED: **LPI2C5 + real on-board WM8962 @0x1A** → the probe is
  **wiring-free** (flash + clean_boot only). Three transactions distilled from
  our HW-verified WireIMXRT1176.cpp + control_wm8962.cpp: reset-write
  R15←0x6243 (`ack`), absent-addr 0x2A probe (`nack` — 0x2A triangulated clear
  of WM8962 0x1A + FXLS8974 accel 0x18 per SDK bubble.c), ID read-back R15
  (`rdn`/`rdv`). ★★KEY DESIGN: **rdv split-assertion** — HW asserts 0x6243
  (R15 readback default = device ID; triangulated via Linux wm8962.c
  reg_default `{15, 0x6243}` fetched 2026-07-18, GPL source used as FACT ONLY;
  local SDK/Zephyr only ever WRITE 0x6243, silent on readback), QEMU asserts
  0x0000 (wm8962-stub contract) — the one expected cross-world divergence
  (precedent: 2C systick). ★Found during triangulation: the qemu2 LPI2C NACK
  path is ALREADY silicon-corrected (NDF deferred to trail TDF,
  imxrt_lpi2c.c:238-295) → `nack` assertable byte-identically. ★LPI2C5 =
  first CM4-driven **LPSR-domain** peripheral (0x40C34000; LPSR IOMUXC pads
  0x40C08010-88; ROOT41 **mux 1** unlike SPI's mux 0) → address-map +
  clock-gating probe triggers both fire. No qemu2/core/Wire-lib change
  expected. Next = writing-plans → implement → QEMU gate → wiring-free EVKB
  probe. D7 still queued.
- 2026-07-18: **Phase 3.2 IMPLEMENTED + QEMU-gate-GREEN** (subagent-driven; 4
  commits `0331c2a`→`a37a7bc` on `master`). `evkb/cm4_wire_test`: CM4
  self-configures LPI2C5 (LPCG102, ROOT41 mux1, LPSR pads — first CM4-driven
  LPSR-domain peripheral) + reset-write ACK / 0x2A NACK / R15 ID read-back
  against the wm8962-stub; 8-token MU contract, stable 3×; `rdv=00000000`
  asserted per the stub contract (HW will assert `00006243`). Driver =
  line-faithful distillation of WireIMXRT1176.cpp + control_wm8962.cpp (final
  review confirmed literal-for-literal vs imxrt1176.h + the Wire lib, incl.
  judged-at-STOP NACK + independent MCCR0 re-derivation). license-audit
  extended + PASS; regressions cm4_spi_test + wire_master_test green; NO
  qemu2/core/lib changes. ★Final-review finding (all-cm4-gates, pre-existing):
  the audit's depfile walk does NOT cover `teensy_add_cm4_image` CM4 sources
  (raw add_custom_command, no -MMD → no .obj.d) — CM4-side files rest on the
  provenance-header convention; queued as a background task to extend the
  audit. **HW probe pending (wiring-free)** — then flip 3.2 to HW-VERIFIED.
  D7 still queued.
- 2026-07-18: **Phase 3.2 ★★HW-VERIFIED on the EVKB — Phase 3.2 COMPLETE.**
  Flashed `cm4_wire_test` + clean boot (snapshot: SCR=0, STAT_M4=1 held,
  MUA_SR=0x00F00200 — uncontaminated), NO wiring. Asserted tokens
  byte-identical to QEMU; `lpcg=1`/`croot=0x100` matched too; full-file diff =
  exactly the one designed line: **`rdv=00006243`** — the real WM8962 answered
  its device ID (write-ID-then-read-ID, un-fakeable: stuck-low=0000,
  stuck-high=FFFF) over a bus whose clock (LPCG102, ROOT41 mux 1) and LPSR
  pads the CM4 self-configured. ★★First HW-proven CM4-driven LPSR-domain
  peripheral; ★★ROOT41-mux-1 clock path HW-proven from the CM4; the Linux
  reg_default R15 fact CONFIRMED on silicon (datasheet-assert design decision
  vindicated). Contaminated post-flash autorun ALSO passed (bonus D7-adjacent
  evidence, same confound caveat as Phase 1). Committed
  `transcript_hw_evkb.txt` + this roadmap. No code changed (silicon == QEMU on
  all asserted tokens) → no re-gate. **Phase 3.2 done; next = 3.3 (shared
  C core consolidation).** D7 still queued; license-audit CM4-source coverage
  gap queued as a background task.
- 2026-07-18: **Phase 3.3 DONE — Phase 3 COMPLETE.** Shared-C-core
  consolidation (approach C) landed across four repos: teensy-cmake-macros
  `0b50945` (`teensy_add_cm4_image` optional `INCLUDE_DIRS`, compile-only
  `-I`; untouched images cmp-proven byte-identical), SPI `eefd879`
  (`lpspi1176.{h,c}` + SPIClass delegation), Wire `aa58de2`
  (`lpi2c1176.{h,c}` + TwoWire master delegation; slave stays C++/NVIC),
  evkb `f0b5c3e`/`b0d46da` (both cm4 gates consume the shared cores; local
  mirrors deleted). Triangulation first proved NO drift between the CM7/CM4
  pairs (5 known distillation deltas D1–D5, all resolving to the CM7 form —
  the "two sources disagree" trigger did NOT fire); shared bodies are the
  CM7 logic verbatim. Guardrail = byte-identity everywhere: 6 QEMU gates
  (cm4_spi, cm4_wire ×3 stable, spi_loopback, spi_dma, wire_master,
  wire_slave) transcripts identical to pre-refactor baselines /
  checked-in transcript_qemu.txt; st7735+wire_oled+sd_test+sd_wav_play
  builds green; license-audit PASS (cm4 manifests 103→105 files — shared
  cores swept). ★Silicon anchor: wiring-free `cm4_wire_test` re-probe
  BYTE-IDENTICAL to `transcript_hw_evkb.txt` incl. `rdv=00006243` — the
  shared lpi2c1176 begin/write/read paths ran on real silicon. ★cm4_spi
  re-probe COMPLETED later same day once the jumper was refitted: clean-boot
  capture BYTE-IDENTICAL to `transcript_hw_evkb.txt` (md5 `b2364766…`, ==
  QEMU), `rxok=1`/`SPI_CM4=PASS` — the shared `lpspi1176.c` drove a real
  4 MHz SCK through the jumper. Both shared cores now silicon-anchored.
  ★Offset cross-asserts (`static_assert(offsetof(...))` shared
  overlay vs `IMXRT_LPSPI_t`/`IMXRT_LPI2C_t`) now break the CM7 build on
  any drift. Risk-trigger walk in the spec (§6): the one honest brush is
  "reset/default values you now depend on" via D1/D3 — every such write is
  inside the CM7 HW-verified stream on the SAME block instances, so no new
  silicon fact; the wire anchor closes it empirically. Next: pick from
  "Deferred beyond Phase 3". D7 still queued.
- 2026-07-18: **license-audit CM4-source coverage gap CLOSED** (the queued
  item from the 3.2 final review). `teensy_add_cm4_image` now passes
  `-MMD -MF <obj>.o.d` (macros repo d6c565e) and the audit's part-2 find also
  matches `*.o.d` — all five macro-built cm4 gates went 101→103 checked files
  (+main_cm4.c +startup_cm4.S each; cm4_boot_test stays 101, embedded blob).
  ★Guardrail: all five `.cm4.bin` outputs cmp-verified BYTE-IDENTICAL
  before/after (side-file-only flag, no codegen change) → NO silicon re-probe
  (2B precedent). Audit PASS; cm4_wire_test gate re-run green; stale
  build_cm4.sh comment in the audit script rewritten.
- 2026-07-19: **Phase 4 OPENED (whole interrupt+DMA surface) — foundation +
  4.1 DONE + ★★HW-VERIFIED.** Brainstormed → one spec
  (`docs/superpowers/specs/2026-07-18-cm4-interrupt-dma-spi-wire-design.md`) +
  plan (`docs/.../plans/2026-07-18-cm4-phase4-foundation-and-wire-int-master.md`)
  covering 4 slices over a shared **targeted split-IRQ foundation**. Executed
  subagent-driven (implementer + spec + quality review per task).
  **qemu2 (commits `89bb31b`+`693a466`):** `fsl_imxrt1170_connect_irq_both`
  inserts a `TYPE_SPLIT_IRQ` (1→2) fanning `LPSPI1`(38)/`LPI2C5`(36)/16 eDMA
  lines to BOTH NVICs; mirrors the existing `gpio13_or` OR-gate idiom.
  ★Task-boundary defect caught in review: `object_initialize_child` REQUIRES a
  matching `qdev_realize` before machine-construction ends
  (`qdev_assert_realized_properly`, precedent `exynos4210.c:429-435`) — so the
  splitters are realized connection-free in the SAME commit, wired in the next.
  Inert (only adds fan-out to a powered-off/NVIC-masked CM4): full regression
  set green (`test_imxrt1170`+`test_imxrt1062`+`dualcore_mu_test` byte-identical
  +`serial_test`+checkpatch 0/0; mcmgr/rpmsg covered-by-proxy). **4.1 gate
  `evkb/cm4_wire_int_master_test`:** CM4 NVIC-enables IRQ 36, services LPI2C5
  on its own NVIC, reads the WM8962 R15 ID. QEMU + EVKB clean-boot green,
  byte-identical except the 2 world-varying tokens (`irqcnt` >0-only, `rdv`
  0000-QEMU/6243-HW). license-audit extended (105 files) + PASS.
  **★★KEY DISCOVERY — the whole silicon-truth loop paid off here:** the FIRST
  interrupt-master (pure-ISR) passed QEMU + all reviews but read `rdv=0x0000`
  on a COLD boot (deterministic 3/3) while the CM7 still printed PASS
  (irqcnt>0/err=0/done=1 — the exact false-PASS the code-quality reviewer had
  flagged). The qemu2 `wm8962-stub` reads 0x0000 for ALL reads, so it
  STRUCTURALLY could not expose it. Root-caused via controlled cold-boot probes
  (systematic-debugging): polled-cold read = 0x6243 (codec/register/cold-read
  fine → not H3); instrumented interrupt-first = 0x6243, SAME irqcnt=9
  (Heisenbug → a timing race). Mechanism: the ISR issued the repeated START the
  instant the write cursor drained, racing the last register-pointer byte still
  clocking on the cold bus → WM8962 never latched the pointer → read the wrong
  register. **Fixed (`5736662`):** `i2c_read_reg` sequences write→read as the
  HW-verified polled core does — polled reg-pointer write (sendStop=0), repeated
  START + TDF-wait, then RXD; the DATA READ stays interrupt-driven (ISR captures
  RX on RDF, completes on SDF → split-IRQ proof). HW 3× rdv=00006243, PASS.
  ★Lessons for 4.2–4.4: (a) a gate whose HW value is world-split MUST assert
  that value on the HW side, not lean on the CM7 PASS (which can't see rdv);
  (b) the wm8962-stub can't validate read-DATA paths — HW is the only oracle;
  (c) cold-vs-warm first-transaction timing matters on silicon. **Operator ran
  the EVKB probe this session (board connected); the fix was found + verified
  live.** Next: 4.2 (interrupt Wire-slave — needs the wired CM7-master↔CM4-slave
  fixture). D7 still queued; DMA slices (4.3/4.4) use the eDMA split already
  landed in the foundation.
- 2026-07-19: **Phase 4.2 (interrupt Wire-SLAVE) DONE + ★★HW-VERIFIED.** The
  CM4 runs an interrupt-driven LPI2C slave @0x42, distilled from the HW-verified
  `TwoWire::handle_slave_isr`. Shared-core extended: `lpi2c1176_slave_config`/
  `lpi2c1176_slave_enable` now live once in `newdigate/Wire/lpi2c1176.{h,c}`
  (regs struct to 0x170 + SCR/SSR/SIER defines); `TwoWire::begin(addr)` migrated
  onto them with the `wire_slave_test` transcript byte-identical (Phase-3.3
  guardrail); cross-overlay static_asserts extended to the slave block (Wire
  `0907b31`+`193e949`). **World-split instance** (triangulation fired the 4.2
  "un-wireable" risk — no LPI2C is both QEMU-bridged AND header-accessible):
  one instance-agnostic slave built for LPI2C2-persona/IRQ 33 in the QEMU gate
  (CM7 masters LPI2C1 over the existing loopback bridge) and LPI2C1/IRQ 32 on
  HW (external master); protocol constants ({A5 5A C3}→resp 3C) identical, so
  `b0/b1/b2`+`resp` assert byte-identically both worlds. qemu2 delta = one
  `connect_irq_both` for LPI2C2 IRQ 33 (`31f04067`; full regression green,
  functional suites 1/1+34/34, dualcore_mu byte-identical, checkpatch 0/0).
  **QEMU model limit (contingency fired):** the model serves the master's
  CMD_RXD synchronously on the CM7 vCPU with a 0xFF empty-FIFO fallback and
  does not model TXDSTALL cross-vCPU, so `mrd` races scheduling — the gate
  asserts the deterministic slave-side `resp` and leaves the master-observed
  byte to the HW oracle (extends 4.1's "wm8962-stub can't validate read-DATA"
  lesson to a cross-vCPU slave). HW-VERIFIED (`ebb28bc`, stable 3×): EVKB
  `irqcnt=0x0C`/`b0/b1/b2=A5/5A/C3`/`resp=3C`/PASS + external Arduino MKR-Zero
  master `wr=0 rd=3C`. **★★HW-DEBUG FINDING (cost a full session, now a
  memory + README + gate-not-firmware exoneration):** `A5`=`GPIO_AD_08`=
  `LPI2C1_SCL` is ALSO `USB_OTG2_ID`; a USB OTG adapter on OTG2 grounded ID and
  clamped SCL to 0V (measured 0Ω A5→GND *board-off* — so, un-caused by any
  firmware). Diagnosed by a CM4 MU register-readback probe (SCR/SSR/mux/pad/
  VERID/clock all correct + SSR=0 idle ⇒ the SoC wasn't driving; the low was
  off-chip) → operator isolated the OTG adapter. See
  [[rt1176-a5-ad08-otg2-id-short]]. Operator ran the EVKB probe + supplied the
  external MKR master live this session.
- 2026-07-19: **Phase 4 DMA — the two-eDMA finding + the eDMA_LPSR DMA-Wire
  milestone (★★HW-VERIFIED).** 4.3 built `cm4_spi_dma_test` (CM4-driven
  full-duplex DMA on LPSPI1 via the MAIN eDMA, 2 channels, OCRAM2 buffers,
  blocking-poll + async): QEMU-green, but the SDO→SDI jumper HW probe exposed
  the phase's biggest silicon truth — **the RT1176 has TWO eDMAs**: the main
  eDMA (`0x40070000`) channel IRQs are CM7-domain (RM Table 4-1), and the CM4's
  are `eDMA_LPSR` (`0x40C14000`, RM Table 4-2). The CM4 DROVE the main eDMA
  (data correct through the jumper, `rxb/rxa=1`) but `dmairq=0` — a CM4
  register-readback probe (`dma_int=1`, `NVIC_ISPR bit0=0`) proved the
  completion IRQ went to the CM7. The Phase-4.1 `edma_irq_split[*]`→both-NVICs
  was a fiction QEMU's single-eDMA model hid. **User chose to pivot to
  eDMA_LPSR.** qemu2 gained `eDMA_LPSR` + `DMAMUX1/LPSR` (`TYPE_IMXRT_EDMA`/
  `TYPE_IMXRT_DMAMUX` second instances; IRQs→CM4-only; LPI2C5 dma-req re-routed
  to DMAMUX1/LPSR **source 52**; main eDMA corrected to CM7-only,
  `edma_irq_split` removed) — full regression green (`a39e6b3`), checkpatch
  clean. `cm4_wire_dma_test`: the CM4 self-configs LPI2C5 + eDMA_LPSR and
  DMA-reads the WM8962 device ID, the completion IRQ firing on the CM4's OWN
  NVIC natively (IRQ 0). HW-VERIFIED wiring-free (WM8962 on LPI2C5), stable 3×:
  `rdv=0x6243` (real ID DMA'd — stub reads 0x0000), `dmairq=2` (`7b949f3`).
  eDMA_LPSR LPCG = CCM LPCG23 (`0x40CC62E0`). Two silicon-neutral qemu2-model
  accommodations in the firmware (32-bit MRDR access; RXD-before-RDDE order).
  `cm4_spi_dma_test` reframed as the POLLED main-eDMA CM4-DMA result (dmairq now
  asserted =0, silicon-faithful). This realizes Phase 4's "interrupt-driven DMA
  on the CM4" on the correct eDMA. See [[rt1176-cm4-edma-lpsr-split]]. ★Lesson:
  a CM4 that can DRIVE a peripheral (data) may still not receive its INTERRUPT —
  the RT1176 splits eDMA (and, by extension, some peripherals) by core domain;
  the "all IRQs reach both NVICs" premise from Phase 1/4.1 is NOT universal.
  **Phase 4 COMPLETE** (interrupt Wire master+slave + DMA, all HW-verified).
  Next: **D7** (new-VTOR reboot, still queued) or a new capability; the DMA
  goal is met. Deferred-beyond: DMA on more LPSR peripherals (LPSPI5/6, SAI via
  eDMA_LPSR) if wanted.
- 2026-07-20: **D7 (CM4 runtime hot-swap) RESOLVED + ★★HW-VERIFIED — the last
  open Phase-1 item is closed.** `cm4_hotswap_test`: the CM7 boots the CM4 with
  image A, then `Multicore.begin(imageB)` re-pulses `SRC_CTRL_M4CORE.SW_RESET`
  and the *running* CM4 reboots into a different program (image B). Two images
  built from ONE source via `-DHS_IDENTITY` (A=`0xA1A1A1A1`, B=`0xB2B2B2B2`),
  both staged at the `0x20200000` backdoor (swap-in-place), each `mu_send`ing a
  ready+identity then parking in WFI (so the CM4 isn't fetching while the CM7
  overwrites the backdoor with B). Both images embedded in one CM7 elf
  (`teensy_target_link_cm4_image` ×2). QEMU-GREEN first try (`5d026a9`); the
  qemu2 `fsl_imxrt1170_cm4_boot` already re-reads GPR0/1 + `cpu_reset`s a running
  CM4, and `Multicore::begin` already pulses SW_RESET every call — **no qemu2 or
  library change**. **★★The clean-boot controlled probe finally isolated D7**
  (the 2026-07-17 evidence was confounded by LinkServer pre-releasing the CM4):
  `clean_boot.scp` SYSRESETREQ→snapshot showed the CM4 **held** (`SCR=0`,
  `STAT_M4CORE=1`) before the CM7 ran, so `begin(A)` was the first clean release
  and `begin(B)` the true hot-swap. HW `idA=A1A1A1A1 → idB=B2B2B2B2`,
  `HOTSWAP=PASS`, stable 2× (`31c0d53`). Wiring-free (MU tokens on VCOM).
  license-audit extended + PASS. ★Lesson: a debugger's connect/flow can
  *contaminate* the very state you're probing — a clean-boot script that holds
  the core and dispatches the image itself is the only way to isolate boot/reset
  behavior. **Unlocks runtime CM4 firmware swapping.** The CM4 bring-up backlog
  (Phases 1-4 + D7) is now all HW-verified. Next: a new capability, or the
  two-resident-VTOR hot-swap variant (needs only a 2nd CM4 linker layout).
