# CM4 Phase 3.2 — Wire/I2C (LPI2C5) polled master, self-configured on the CM4

**Date:** 2026-07-18
**Status:** Approved (brainstorming) → ready for `writing-plans`
**Phase:** 3.2 — second per-library CM4 enablement. Peripheral: **LPI2C5** (the
on-board codec bus, `Wire2`), driven **entirely by the Cortex-M4**, talking to
the **real on-board WM8962 codec @0x1A**.
**Skill:** governed by `cm4-bringup`. Architecture carried over from 3.1
(locked 2026-07-18): CM4 self-configures; distilled C driver doubles as the
probe; polled-only; consolidation deferred to 3.3.

---

## 1. Goal & success criteria

A **Cortex-M4 image self-configures LPI2C5** — ungates `CCM_LPCG102`, sets
`CCM_CLOCK_ROOT41=(1<<8)` (mux 1 → 24 MHz), muxes the LPSR-domain pads
`GPIO_LPSR_05` (SCL) / `GPIO_LPSR_04` (SDA) @ ALT0|SION with open-drain pad
`0x0A`, configures the master block (`MCR_RST→0`, `MCFGR1`/`MCCR0` @ ~100 kHz,
`MCR_MEN`) — then runs three polled master transactions against the codec bus,
streaming observations over the **MU** to the CM7 reporter (the `cm4_spi_test`
pattern).

**The three transactions (the WM8962 protocol, from our HW-verified
`control_wm8962.cpp`):**
1. **Reset-write (ACK proof):** write `R15←0x6243` (`[0x34, 0x00, 0x0F, 0x62,
   0x43]` on the wire) — byte-identical to `WM8962_Init`'s first write,
   idempotent at clean boot → expect `err=0`.
2. **Absent-address probe (NACK proof):** START to **0x2A** — absent in both
   worlds (avoids WM8962 @0x1A *and* the on-board FXLS8974 accelerometer
   @0x18, which QEMU does not model) → expect address-NACK (`err=2`).
3. **ID read-back (the crown jewel):** `readReg(0x0F)` — write `[0x00, 0x0F]`,
   repeated-START, read 2 bytes → on **silicon** the WM8962 answers its own
   device ID **`0x6243`** (the R15 readback default, Linux `wm8962_reg`
   reg_default table: `{ 15, 0x6243 }`); in **QEMU** the `wm8962-stub` reads
   `0x0000` by design. The write-ID-then-read-ID pair is self-evidencing: a
   stuck-low bus reads 0x0000, stuck-high reads 0xFFFF — only a live codec on a
   CM4-brought-up bus says `0x6243`.

**v1 success =** QEMU gate green (all asserted tokens + `rdv=00000000` per the
stub contract), **and** the EVKB clean-boot run green with the asserted tokens
byte-identical **and `rdv=00006243` matching the datasheet default** (the one
documented expected divergence, decided during brainstorming: HW `rdv` is
*asserted* against the datasheet value, not merely characterized).

**The two-layer proof (same shape as 3.1):** the qemu2 LPI2C model completes
transfers on `MEN` + bus emulation regardless of the CCM/IOMUXC writes → QEMU
proves the **register/transfer sequence**; only silicon proves the CM4's
**clock-gating + LPSR pin-mux**, witnessed by a real device's ACK + ID through
the on-board pull-ups. **3.2's probe is wiring-free** — the target is soldered
on; flash + `clean_boot.scp` is the whole HW procedure.

**Scope guard — explicitly deferred:** CM4 I2C slave mode, interrupt-driven
Wire (peripheral IRQs are CM7-NVIC-only), talking to the FXLS8974, Wire on
LPI2C1/2 from the CM4, concurrent CM7+CM4 bus use, and the 3.3 shared-C-core
consolidation. **LPI2C5 is CM4-exclusive in 3.2; the CM7 sketch never touches
Wire/LPI2C.**

---

## 2. Ground truth established during exploration (all verified this session)

| Fact | Source |
|---|---|
| LPI2C5 base = `0x40C34000` (LPSR peripheral region); master offsets MCR 0x10 / MSR 0x14 / MCFGR1 0x24 / MCCR0 0x48 / MTDR 0x60 / MRDR 0x70 | `cores/imxrt1176/imxrt1176.h:631,591-610` |
| LPI2C5 clock gate = `CCM_LPCG102_DIRECT` @ `0x40CC6CC0`; root = `CCM_CLOCK_ROOT41_CONTROL` @ `0x40CC1480`, value `(1<<8)` = **mux 1** → 24 MHz (NB: SPI's root43 used mux 0) | `imxrt1176.h:1100-1101`; `WireIMXRT1176.cpp:292-301` (`lpi2c5_hardware`, HW-verified via all WM8962 audio work) |
| Pads: SCL=`GPIO_LPSR_05` mux @`0x40C08014`, SDA=`GPIO_LPSR_04` mux @`0x40C08010`, mux val `0x10` (ALT0\|SION); pads @`0x40C08054/50` val `0x0A` (LPSR open-drain); select-inputs SCL @`0x40C08084` / SDA @`0x40C08088` val `0` | `imxrt1176.h:758-761,1105-1106`; `WireIMXRT1176.cpp:292-301` |
| Wire master path is **fully polled** — MTDR cmd words (`CMD_START=4`, `CMD_TXD=0`, `CMD_RXD=1`, `CMD_STOP=2` in [10:8]) + MSR waits (`TDF/RDF/SDF` progress, `NDF/ALF/FEF` error); NACK classified via `MSR_NDF` (err 2 = addr, 3 = data); no ISR | `WireIMXRT1176.cpp:37-56,200-245` (`endTransmission`/`requestFrom`) |
| `begin()` self-config sequence: lpcg=1 → root → mux/pad/select → `MCR=RST; MCR=0` → `setClock` (MCFGR1/MCCR0, MEN=0) → `MCR=MEN` | `WireIMXRT1176.cpp:75-88` |
| `setClock(100000)` @24 MHz → **pre=1, div=120, clklo=63 (clamped from 72), clkhi=48** → `MCFGR1=1`, `MCCR0=0x1818303F` (DATAVD=SETHOLD=24) | `WireIMXRT1176.cpp:setClock` (math re-run this session; re-verify at plan time) |
| WM8962 protocol: 16-bit reg / 16-bit data; write = `[0x00, reg, hi, lo]`; read = write `[0x00, reg]` + `endTransmission(false)` (**repeated START**) + `requestFrom(0x1A, 2)` | `~/Development/Audio/control_wm8962.cpp:36-53` (HW-verified by the audio phases) |
| **R15 (Software Reset) readback default = `0x6243`** (the device ID; same constant the init table writes) | Linux `sound/soc/codecs/wm8962.c` reg_default `{ 15, 0x6243 }` — fetched 2026-07-18 from <https://raw.githubusercontent.com/torvalds/linux/master/sound/soc/codecs/wm8962.c> (GPL source used as a **fact source only**, per the license firewall; no code taken). Local SDK/Zephyr only *write* 0x6243 (`fsl_wm8962.c:358`, `wm8962.h:42`) — silent on readback, hence the web triangulation |
| qemu2 `wm8962-stub` on **lpi2c[4]** (=LPI2C5) @0x1A: ACKs everything, all reads return `0x00`; explicitly "NOT a codec model" | `hw/i2c/wm8962_stub.c` (57 lines, read in full); `hw/arm/mimxrt1170-evk.c:71-73` |
| qemu2 LPI2C **NACK path is silicon-corrected**: absent address → `addr_nacked` deferred so NDF trails TDF, "Setting NDF immediately is what let a broken scan pass in simulation while failing on the board" → NACK-probe token assertable byte-identically | `hw/i2c/imxrt_lpi2c.c:230-295` |
| On-board FXLS8974 accelerometer @ **0x18** shares the bus on silicon (unmodelled in QEMU) → NACK-probe address must avoid 0x18/0x1A → **0x2A** chosen | SDK `examples/_boards/evkbmimxrt1170/demo_apps/bubble/bubble.c:37` (`g_accel_address = 0x18U`) |
| LPI2C5 reachable from the CM4: `cm4_view` overlays a full `system_memory` alias (verified for 3.1; LPSR region included) | `hw/arm/fsl-imxrt1170.c:945-950` |
| Peripheral IRQs CM7-NVIC-only → polled-first stands | `hw/arm/fsl-imxrt1170.c:961-966` |

---

## 3. Architecture — one artifact, gate + probe (clone of `cm4_spi_test`)

| Component | File | Nature |
|---|---|---|
| CM7 reporter | `cm4_wire_test.cpp` | Arduino: `Multicore.begin(cm4 image)`, read 8 MU values, print `token=HEX` + `WIRE_CM4=PASS/FAIL`. **Never calls Wire/LPI2C.** |
| CM4 I2C driver + main | `cm4/main_cm4.c` | Distilled C: self-config LPI2C5 + the three transactions + MU stream. **The probe.** Provenance header naming `WireIMXRT1176.cpp` + `control_wm8962.cpp`. |
| CM4 startup/linker | `cm4/startup_cm4.S`, `cm4/cm4.ld` | Verbatim from `cm4_spi_test` (= `cm4_dual_test`). |
| Build glue | `CMakeLists.txt`, `toolchain/…` | Clone of `cm4_spi_test`, names swapped (`cm4_wire`, `cm4_wire_test`). |
| Runner | `run_qemu.sh` | gate-lib pattern; greps asserted tokens incl. `rdv=00000000` (the stub contract). |
| Transcripts | `transcript_qemu.txt`, `transcript_hw_evkb.txt` | Checked in. **Expected to differ on exactly one line** (`rdv=`) — documented in the README, precedent: `cm4_intr_test`'s `systick`. |

### 3.1 Token set (MU channel 0, fixed order — 8 values)

| # | Token | QEMU | HW | Asserted? |
|---|---|---|---|---|
| 0 | `mcr` | `00000001` | `00000001` | **yes** (MEN readback) |
| 1 | `lpcg` | `00000001` | `00000001` | informative (CCM RAM-modelled) |
| 2 | `croot` | `00000100` | `00000100` | informative |
| 3 | `ack` | `00000000` | `00000000` | **yes** — reset-write ACKed (err 0) |
| 4 | `nack` | `00000002` | `00000002` | **yes** — addr-NACK @0x2A (err 2; model silicon-corrected) |
| 5 | `rdn` | `00000002` | `00000002` | **yes** — ID read returned 2 bytes |
| 6 | `rdv` | `00000000` | `00006243` | **split**: QEMU runner asserts `00000000` (stub contract); HW asserts `00006243` (datasheet default). The one expected divergence. |
| 7 | `done` | `00000001` | `00000001` | **yes** — CM4 sequence completed |

`WIRE_CM4=PASS` from the CM7 requires tokens 0, 3, 4, 5, 7 at their expected
values; the CM7 does **not** fold `rdv` into PASS (world-dependent) — it
prints it raw, and each world's checker asserts its own expected value.

---

## 4. Probe obligations & silicon-truth bookkeeping

**Triggers fired:** (a) **clock/power-gating** — the CM4 writes
`CCM_LPCG102`/`ROOT41` (root41 = mux **1**, a different mux than 3.1
exercised); (b) **address-map subtlety** — first CM4-driven use of an
LPSR-domain peripheral (`0x40C34000`) + LPSR IOMUXC pads. → **EVKB probe
mandatory**, and it is **wiring-free**: flash + `clean_boot.scp` only (the
WM8962 + pull-ups are soldered on; codec I2C needs no MCLK for register
access — HW-verified by the audio bring-up doing `WM8962_Init` before SAI).

**Circular-pass honesty (same as 3.1):** the qemu model + stub respond on
`MEN` alone — a CM4 that skipped the LPCG/root/mux writes still goes green in
QEMU. Silicon's `ack=0` + `rdv=6243` are the only proof of the CM4's clock +
LPSR pins. This is the load-bearing purpose of the HW run.

**Diff procedure:** asserted tokens (`mcr/ack/nack/rdn/done` + banner/PASS
lines) diff byte-identical; `rdv` excluded from the cross-world diff and
asserted per-world (`00000000` in the QEMU runner, `00006243` in the HW
check). `lpcg/croot` printed for diagnosis. README documents the divergence
table.

---

## 5. License & qemu2 firewall

- CM4 driver = author-original C re-expressing this project's own MIT logic
  (`WireIMXRT1176.cpp` + `control_wm8962.cpp`) with a provenance + keep-in-sync
  header; **the Linux driver contributed one hardware FACT (R15 default), no
  code** — noted in the header for audit honesty.
- Extend `evkb/tools/license-audit.sh` `GATES` with `cm4_wire_test:cm4_wire_test`
  in the **same change**; require `LICENSE-AUDIT: PASS`.
- **qemu2: no change expected** — stub, silicon-corrected NACK model, and
  `cm4_view` reachability all verified in place. If a gap appears, that is a
  new-model trigger: full regression set + checkpatch + probe citation.

---

## 6. Verification sequence

1. **QEMU red:** stub CM4 (no I2C) → no `WIRE_CM4=PASS`; runner exit 1.
2. **QEMU green:** full driver → all asserted tokens + `rdv=00000000`;
   save `transcript_qemu.txt`; stable 3×.
3. **Regression:** `cm4_spi_test` (shared startup/linker/MU pattern) +
   `~/Development/Wire/tests/wire_master_test` (`run_qemu_wire.sh` — the
   library gate whose logic the CM4 driver mirrors; untouched, must stay
   green) both pass. No qemu2 rebuild (nothing changed there).
4. **License audit:** `LICENSE-AUDIT: PASS` with `cm4_wire_test` covered.
5. **Hardware (final arbiter, wiring-free):** flash + `clean_boot.scp`;
   confirm `WIRE_CM4=PASS`, asserted tokens byte-identical, **`rdv=00006243`**;
   save `transcript_hw_evkb.txt`.
6. **Roadmap + commit** per the session-log discipline.

---

## 7. Risks & mitigations

| Risk | Mitigation |
|---|---|
| Circular QEMU pass (model ignores clock/pins) | HW run is the arbiter; wiring-free so it is cheap; `rdv=6243` is un-fakeable |
| R15 readback ≠ 0x6243 on this codec revision | The Linux reg_default is the strongest available written source; if silicon disagrees, **silicon wins** — record the measured value, update README + this spec, and re-derive (that outcome is itself a discovery worth keeping) |
| `MCCR0` timing math wrong for 100 kHz | Values re-derived at plan time and cross-checked against the HW-verified `setClock`; QEMU is timing-insensitive, silicon exercised the same values via Wire2 audio work |
| Absent-address choice collides with a real device | 0x2A triangulated against WM8962 (0x1A) + FXLS8974 (0x18, SDK bubble.c); no other device on LPI2C5 per the board files |
| Codec left in non-default state after gate | The only write is the soft-reset itself — the gate *restores* defaults by construction |
| Bounded-spin hangs if no clock (HW) | Same `SPI_TIMEOUT`-style bounded waits as 3.1 → visible FAIL tokens, not a hang |

---

## 8. Deferred (Phase 3 arc)

- **3.3 — shared C register/clock core**: consolidate the now-two distilled
  drivers (SPI + Wire) with the CM7 C++ classes; byte-identical-CM7 guardrail.
- CM4 I2C **slave** mode; interrupt-driven Wire (needs `TYPE_SPLIT_IRQ`);
  FXLS8974 accelerometer driving; concurrent CM7+CM4 bus arbitration.
