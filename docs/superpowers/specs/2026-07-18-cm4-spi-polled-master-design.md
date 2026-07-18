# CM4 Phase 3.1 — SPI (LPSPI1) polled master, self-configured on the CM4

**Date:** 2026-07-18
**Status:** Approved (brainstorming) → ready for `writing-plans`
**Phase:** 3.1 — first per-library CM4 enablement. Peripheral: **LPSPI1** (EVKB
Arduino-header SPI), driven **entirely by the Cortex-M4**.
**Skill:** governed by `cm4-bringup` (silicon-truth loop + license firewall).

---

## 1. Goal & success criteria

A **Cortex-M4 image self-configures LPSPI1** — ungates its clock, muxes its pins,
and configures the block — then clocks bytes through a **polled master
self-loopback** (external **SDO→SDI jumper**, the exact wiring already HW-verified
for CM7 SPI). The CM4 has no console, so it streams each observation over the
**MU** to the CM7, which prints `token=HEX` lines on **LPUART1/VCOM** (the
`cm4_dual_test` reporting pattern).

**v1 success = the CM4-driven loopback reads back what it sent** (`a=A5 b=3C
w=BEEF`, `rxok=1`) **and the LPSPI register-config readbacks match** — proven in a
**QEMU gate** *and* on **real silicon** via `clean_boot.scp`.

**The load-bearing insight (why the HW probe is mandatory, not confirmatory).**
The board fixture `hw/arm/mimxrt1170-evk.c:74-81` attaches an **`ssi-loopback`**
echo child to **LPSPI1's** bus, and `imxrt_lpspi_transfer()`
(`hw/ssi/imxrt_lpspi.c:200-266`) echoes `rx=tx` **as soon as `CR_MEN` is set** —
it does **not** consult the LPCG clock gate, the clock root, or the pin mux. So in
QEMU the loopback passes on `MEN` alone; a CM4 image that **forgot to ungate
`CCM_LPCG104`, set `CCM_CLOCK_ROOT43`, or mux the pads would still print
`a=A5` in QEMU and fail on hardware** — a *circular pass* (identical in kind to
the FlexCAN `SRXDIS` gap). Therefore **`rx==tx` on silicon, through the physical
jumper, is the only proof the CM4 brought up the functional clock + pins itself.**
Same transcript, HW proves strictly more. This *is* the clock/power-gating probe
the phase's risk table mandates — it is the deliverable, not a follow-on.

**Scope guard — explicitly deferred:** DMA (needs `DMAChannel`/`EventResponder`/
`yield()` — the full CM7 core), interrupt-driven SPI (needs a qemu2 per-line
`TYPE_SPLIT_IRQ` — peripheral IRQs reach the CM7 NVIC only, confirmed at
`fsl-imxrt1170.c:961-966`), Wire/I2C (Phase 3.2), concurrent CM7+CM4 use of
LPSPI1 / arbitration, and any `SPIClass` C++/Arduino-runtime port to the CM4
(the shared-core consolidation is Phase 3.3). **LPSPI1 is CM4-exclusive in 3.1;
the CM7 sketch never touches it.**

---

## 2. Ground truth established during exploration (all verified this session)

| Fact | Source |
|---|---|
| LPSPI1 base = `0x40114000` | `cores/imxrt1176/imxrt1176.h:579` (`IMXRT_LPSPI1_ADDRESS`) |
| LPSPI1 clock gate = `CCM_LPCG104_DIRECT` @ `0x40CC6D00` (LPCG index 104) | `imxrt1176.h:536`; qemu2 `hw/misc/imxrt_ccm.c:40-41` (LPCG block @ `0x6000`, 138 entries, stride `0x20` → `0x6000+104*0x20 = 0x6D00`) |
| LPSPI1 clock root = `CCM_CLOCK_ROOT43_CONTROL` @ `0x40CC1580`; value `0` → mux0 (OSC24M), div0 → **24 MHz** | `imxrt1176.h:535`; `SPIIMXRT1176.cpp:219` (`clock_root_val=0`, `func_clock=24000000`) |
| Pins (ALT0): SCK=`GPIO_AD_28`, SDO=`GPIO_AD_30`, SDI=`GPIO_AD_31`; mux regs `0x400E817C/8184/8188`, pad `0x0C`; select-inputs SCK/SDO/SDI `0x400E85D0/85D8/85D4`=`1` | `imxrt1176.h:538-546`; `SPIIMXRT1176.cpp:218-228` (HW-verified CM7 config) |
| `begin()` self-configures clock+pins+block with **direct register writes** — no CM7-core dependency | `SPIIMXRT1176.cpp:67-81` |
| Polled `transfer()` touches only `TCR/TDR/RSR/RDR` — no core dependency; DMA path needs `DMAChannel`/`EventResponder`/`yield()` | `SPIIMXRT1176.cpp:128-135` vs `155-213` |
| **LPSPI1 is reachable from the CM4**: `cm4_view` overlays `cm4_sysmem`, a full alias of `system_memory` (`0..UINT64_MAX`) — "everything falls through to shared system memory (peripherals…)" | qemu2 `hw/arm/fsl-imxrt1170.c:945-950` + comment `933-943` |
| qemu2 LPSPI is a **real master** (TDR write shifts a frame; RX FIFO; `RSR_RXEMPTY`; `CR_MEN` gate) | `hw/ssi/imxrt_lpspi.c:12-14, 200-266, 307` |
| **`ssi-loopback` echo child is on LPSPI1's bus** — echoes MOSI→MISO, gated only on `CR_MEN`, ignores LPCG/root/pins | `hw/arm/mimxrt1170-evk.c:74-81`; `hw/ssi/ssi_loopback.c:23-27` |
| qemu2 **CCM is RAM-backed**: `read → s->regs[off/4]`, generic `write → s->regs[off/4]=val` (+ LPCG ON-bit mirrored to STATUS). So LPCG104 / ROOT43 **readback works**, matching HW; functional gating is **not** enforced | `hw/misc/imxrt_ccm.c:100-125` |
| Peripheral IRQs wire to the **CM7 NVIC only**; only MU reaches both cores → **polled-only** on the CM4 | `hw/arm/fsl-imxrt1170.c:961-966` (machine TODO) |
| CM4 image is **pure C**: `Reset_Handler` = VTOR→FPU→`.data` copy→`.bss` zero→`bl main`; **no `__libc_init_array`**, no `.init_array` in the linker | `evkb/cm4_dual_test/cm4/startup_cm4.S:46-80`, `cm4.ld` |
| CM4↔CM7 reporting: CM4 sends over MU `TR`, CM7 (Arduino) reads `RR` and prints | `evkb/cm4_dual_test/cm4/main_cm4.c` + `cm4_dual_test.cpp` |
| CM7 SPI loopback gate asserts `rx==tx` and passes in QEMU (echo child): `a=0xA5 b=0x3C w=0xBEEF … SPI_LOOPBACK=PASS` | `~/Development/SPI/tests/spi_loopback_test/spi.uart` (committed) |

---

## 3. Architecture — one artifact, gate + probe

Everything lives in a **new evkb gate `evkb/cm4_spi_test/`**, cloned from
`cm4_dual_test`'s shape. **No changes** to `newdigate/SPI`, the core, or qemu2 are
expected (see §7).

| Component | File | Nature |
|---|---|---|
| CM7 reporter sketch | `cm4_spi_test.cpp` | Arduino: `Multicore.begin(cm4 image)`, read MU tokens, print `token=HEX` + verdict. **Never calls `SPI.*`.** |
| CM4 SPI driver + main | `cm4/main_cm4.c` | Distilled C: self-config LPSPI1 + polled loopback + stream observations over MU. **The probe.** |
| CM4 startup / linker | `cm4/startup_cm4.S`, `cm4/cm4.ld` | Reused verbatim from `cm4_dual_test` (pure C, MU vector at 134 not required — polled, but kept for parity/no-cost). |
| Build glue | `CMakeLists.txt`, `toolchain/…` | Clone `cm4_dual_test/CMakeLists.txt`: `import_arduino_library(cores …)` (provides `Multicore`/`MessagingUnit`), `teensy_add_cm4_image(cm4_spi …)`, `teensy_target_link_libraries(… cores)`, `target_link_libraries(….elf stdc++)`, `teensy_target_link_cm4_image(cm4_spi_test cm4_spi)`. **No `SPI` import.** |
| Runner | `run_qemu.sh` | gate-lib.sh pattern; greps asserted tokens. |
| Transcripts | `transcript_qemu.txt`, `transcript_hw_evkb.txt` | checked in, diffed per silicon-truth-loop. |

**Boundary:** the CM4 image owns LPSPI1 end-to-end; the CM7 owns only boot + MU +
console. Clean probe attribution — every LPSPI1/CCM/IOMUXC write is the CM4's.

---

## 4. The CM4 driver (`cm4/main_cm4.c`) — distilled from `SPIIMXRT1176.cpp`

A C re-expression of the HW-verified `begin()`+`transfer()` sequence, carrying a
provenance header per the license firewall:

```c
/* Adapted from this project's own newdigate/SPI SPIIMXRT1176.cpp begin()/
 * transfer() (MIT, N. Newdigate) — the HW-verified LPSPI1 self-config +
 * polled-master sequence, re-expressed in C for the bare-metal CM4 image.
 * Provenance: no logic change; register/clock/pin values identical. Phase 3.3
 * will consolidate this and the C++ class onto a shared C core. */
```

**Self-config (mirrors `SPIIMXRT1176.cpp:67-81` exactly):**
1. `CCM_LPCG104_DIRECT = 1` — ungate LPSPI1 clock.
2. `CCM_CLOCK_ROOT43_CONTROL = 0` — mux0 OSC24M ÷1 = 24 MHz.
3. mux SCK `AD_28` / SDO `AD_30` / SDI `AD_31` = ALT0, pad `0x0C`; select-inputs = 1.
4. `CR=RST; CR=0; CFGR1=MASTER; CCR.SCKDIV` for 4 MHz; `TCR` prescale; `CR=MEN`.

**Polled transfer (mirrors `:128-135`):** `TCR = tcr_base | FRAMESZ(n-1)`; write
`TDR`; spin on `RSR.RXEMPTY`; read `RDR`. Test vector matches the CM7 gate for
parity: `transfer(0xA5)`, `transfer(0x3C)`, `transfer16(0xBEEF)`, plus a 4-byte
buffer `{DE,AD,BE,EF}`.

**MU stream (mirrors `cm4_dual_test` `mu_send`):** the CM4 pushes a fixed,
ordered sequence of observations on `TR0` (poll `SR.TE` between words); the CM7
reads the same count on `RR0` and prints them with known labels.

### 4.1 Token set — asserted vs characterization

| Token | Value (QEMU **and** HW) | Asserted byte-identical? |
|---|---|---|
| `cr` | `0x00000001` (LPSPI `CR.MEN`) | **yes** — CM4-controlled, RAM-modelled identically |
| `cfgr1` | `0x00000001` (`MASTER`) | **yes** |
| `a` `b` | `A5` `3C` (loopback echo) | **yes** — echo child (QEMU) / jumper (HW) |
| `w` | `BEEF` (`transfer16`) | **yes** |
| `buf` | `DEADBEEF` | **yes** |
| `rxok` | `1` (all matched) | **yes** — the headline verdict |
| `lpcg` | `0x…` (CCM_LPCG104 readback) | **informative** — expected equal (RAM-model), but not asserted (HW status-bit latitude) |
| `croot` | `0x…` (ROOT43 readback) | **informative** — same rationale |

`lpcg`/`croot` are printed **for diagnosis** (a HW failure localizes to
gate/root/block/shift), following silicon-truth-loop "print raw values, not
pass/fail," and are treated like 2C's `systick` characterization token — informative,
not part of the pass grep — because a CLOCK_ROOT/LPCG status bit could differ on
silicon without meaning failure. The **verdict is `rxok`**, which *is* asserted and
carries the whole loopback proof.

---

## 5. QEMU gate (TDD, `evkb/cm4_spi_test/run_qemu.sh`)

Greps **`SPI_CM4=PASS`** (printed by the CM7 when `rxok=1` and `cr/cfgr1/a/b/w/buf`
match expectations). **Red first:** before the CM4 driver is written (or with the
LPCG/pin steps present but the block-config stubbed), the run must **not** print
PASS; then green once the sequence is correct. Runner mirrors
`spi_loopback_test/run_qemu_spi.sh` (qrun, `-serial file:`, `boot-xip=on`, 3 s,
grep). CMake per §3 (clone of `cm4_dual_test/CMakeLists.txt`): `cores` gives
`Multicore`/`MessagingUnit`; `teensy_add_cm4_image(cm4_spi …)` compiles the CM4
image and `teensy_target_link_cm4_image` embeds it in the CM7 ELF.

**What the QEMU gate proves:** the register/clock/pin/transfer *sequence* is
correct **given the echo model** — i.e. the code compiles, boots on the CM4,
reaches `MEN`, and round-trips the loopback + MU reporting deterministically. **It
cannot prove** LPCG-gating, clock-root, or pin-mux correctness (echo ignores them).

---

## 6. Probe obligation & silicon-truth bookkeeping

**Trigger fired:** *clock/power-gating* (the CM4 writing `CCM_LPCG104` +
`CCM_CLOCK_ROOT43`). The gate firmware **is** the probe.

**Run (per silicon-truth-loop):** flash `cm4_spi_test.elf`; capture VCOM with the
pyserial reader started first; then an **uncontaminated** boot via
`dualcore_mu_test/clean_boot.scp` (SYSRESETREQ, CM4 held, `SCR=0`, manual dispatch)
so LinkServer's CLOCK_ROOT1/CM4-wake cannot mask a CM4 clock-config bug. **Physical
setup:** the **SDO(AD_30)→SDI(AD_31) jumper** on the Arduino header (same wiring as
the CM7 SPI loopback).

**Pass = the asserted tokens diff clean** against `transcript_qemu.txt`
(`rxok=1`, `a=A5 b=3C w=BEEF buf=DEADBEEF`, `cr/cfgr1`). Since QEMU already shows
`rx==tx` via the echo child, the **silicon delta is meaning, not bytes**: on HW the
identical `a=A5` additionally proves the CM4 ungated the clock, muxed the pins, and
drove a real 24 MHz-derived SCK — the circular-pass gap closed only by silicon.
Document `lpcg`/`croot` observed values in the README; check in both transcripts.

---

## 7. License & qemu2 firewall

- **License:** the CM4 driver is author-original C re-expressing this project's own
  MIT logic against the RM, with the provenance header in §4 → **no new source tree
  vendored.** Add `cm4_spi_test` to `evkb/tools/license-audit.sh` `GATES` (link-
  manifest coverage) in the **same change**; require `LICENSE-AUDIT: PASS`.
- **qemu2: no change expected.** LPSPI1 + `ssi-loopback` + CCM are already
  modelled and reachable from `cm4_view`; polled avoids the CM4-NVIC gap. GPL
  firewall is therefore trivially clean. **If** a gap appears (e.g. LPSPI1 not
  reaching the CM4 view — contradicted by §2, but verify at gate time), that is a
  new-model trigger: run the qemu2 regression set + `checkpatch.pl`, cite the
  probe, and pause to reconsider scope before proceeding.

---

## 8. Verification sequence

1. **QEMU red:** stub the block-config (or omit the driver) → confirm the gate
   does **not** print `SPI_CM4=PASS`.
2. **QEMU green:** full driver → `SPI_CM4=PASS`; save `transcript_qemu.txt`.
3. **Regression:** re-run `spi_loopback_test` (CM7, unaffected) + `cm4_dual_test`
   (shared CM4 startup/linker/MU path) — both still green. (No qemu2 rebuild since
   nothing in qemu2 changed; if that assumption breaks, run the full regression set
   from silicon-truth-loop §"qemu2 regression set".)
4. **License audit:** `sh evkb/tools/license-audit.sh` → `LICENSE-AUDIT: PASS`.
5. **Hardware (final arbiter):** SDO→SDI jumper on; `clean_boot.scp` run; confirm
   `SPI_CM4=PASS` + the asserted tokens byte-identical to QEMU; record `lpcg/croot`.
   Save `transcript_hw_evkb.txt`. Independently `diff` HW-vs-QEMU per §6.
6. **Roadmap + commit:** update `references/cm4-roadmap.md` (Phase 3.1 status,
   discoveries, D7 still queued) and session log; commit on `master`
   (`cm4_spi_test: CM4 self-configured polled SPI (Phase 3.1) — HW-verified`).

Independently review each subagent's edits and re-run each gate yourself
(subagent-verification rule). Adding gate files under a fresh dir → configure the
build from scratch (`rm -rf build`) to dodge the CMake `file(GLOB)` staleness trap.

---

## 9. Risks & mitigations

| Risk | Mitigation |
|---|---|
| **Circular QEMU pass** — echo child ignores LPCG/root/pins, so a clock/pin bug passes in QEMU | HW jumper run is the arbiter (§6); `clean_boot.scp` prevents LinkServer masking the CM4 clock state; `lpcg/croot` diagnostics localize a HW failure. (Closing it in qemu2 = machine-wide clock-gating enforcement — deferred, §10.) |
| CM4 clock write races the CM7 / boot ROM on ROOT43 | LPSPI1 is CM4-exclusive; the CM7 sketch never calls `SPI.*`; `clean_boot` leaves clocks at reset. Last-writer = the CM4. |
| Duplicated register logic drifts from `SPIIMXRT1176.cpp` | Provenance + "keep in sync" header; Phase 3.3 consolidates onto a shared C core (byte-identical CM7 guardrail). |
| Polled `transfer()` spins forever if no clock (HW) | Bounded spin (`SPI_TIMEOUT`, as in the library) → returns `0xFF` → `rxok=0` → visible FAIL, not a hang. |
| CMake `file(GLOB)` staleness after new files | From-scratch `rm -rf build && cmake …` of the gate. |
| CCM `croot` status-bit differs on silicon | Not asserted (characterization only); verdict rests on `rxok`. |

---

## 10. Deferred (follow-ons / Phase 3 arc)

- **3.2 — Wire/I2C polled master on the CM4.** Needs a target that ACKs: the qemu2
  LPI2C master↔slave-persona loopback (`fsl-imxrt1170.c:1099`) + the on-board
  AT24C02 fixture on LPI2C1, or a CM4↔CM7 arrangement. Same self-config pattern.
- **3.3 — shared C register/clock core (approach C).** Extract the proven SPI (then
  Wire) sequence into a C-callable core both the CM7 C++ class and the CM4 image
  call; guardrail = byte-identical CM7 gate output (the 2B `cmp` discipline). Ends
  the duplication introduced in 3.1.
- **Interrupt-driven / DMA SPI on the CM4.** Needs a qemu2 per-line
  `TYPE_SPLIT_IRQ` to route LPSPI1_IRQ (38) to the CM4 NVIC (`fsl-imxrt1170.c:961`)
  — a new-model trigger + its own probe — and eDMA-from-CM4. Large; out of Phase 3.1.
- **qemu2 clock-gate fidelity** (close the circular pass) — enforce LPCG/pin-mux in
  the peripheral/echo path. Machine-wide blast radius; deferred, HW is the arbiter.
- **Concurrent CM7+CM4 LPSPI1 / arbitration** — a cross-core ownership protocol.
