# CM4 Phase 4 — interrupt-driven & DMA SPI/Wire on the Cortex-M4

**Date:** 2026-07-18
**Status:** Approved (brainstorming) → ready for `writing-plans`
**Phase:** 4 — first "Deferred beyond Phase 3" item. The CM4 gains the
**interrupt-driven and DMA** I/O paths the CM7 already has, across four
capabilities over one shared qemu2 foundation.
**Skill:** governed by `cm4-bringup`. Carries the Phase-3 architecture
forward: the CM4 **self-configures** each peripheral (reusing the
HW-verified 3.1/3.2 clock+pin bring-up); each capability is a distilled or
fresh C image that **doubles as the probe**; one artifact per slice, each
with its own QEMU gate + EVKB probe.

---

## 0. Scope & decomposition (locked in brainstorming)

Phase 4 delivers **four capabilities** over a **shared qemu2 split-IRQ
foundation**. The work item bundled "interrupt-driven / DMA SPI/Wire"; the
idiomatic mapping onto *this* silicon (established below) is:

- **DMA SPI** = the async `SPI.transfer(…EventResponder)` path (Teensy SPI
  has no byte-level LPSPI-interrupt path; its interrupt path *is* DMA).
- **Interrupt-driven Wire** = the LPI2C ISR path, which on the RT1176 exists
  in **two roles**: the **slave** ISR (`handle_slave_isr`, HW-verified) and a
  **master** ISR (which the CM7 does *not* have — its master is polled).
- **DMA Wire** = eDMA-driven LPI2C (uncommon on Teensy; fresh).

| # | Slice | CM4 code | Source | qemu2 delta | HW probe |
|---|---|---|---|---|---|
| **4.1** | Interrupt Wire-**master** (LPI2C5) | fresh ISR master | RM + SDK/Zephyr-validated | split-IRQ helper + LPI2C5 line | **wiring-free** WM8962 (reuse 3.2) |
| **4.2** | Interrupt Wire-**slave** (mirrors `wire_slave_test` fixture) | distill `handle_slave_isr` | HW-verified `WireIMXRT1176.cpp` | + the slave-instance line (likely ≠ LPI2C5) | **wired** CM7-master↔CM4-slave |
| **4.3** | **DMA SPI** (LPSPI1 async) | distill DMA path + `dma_rxisr`→flag | HW-verified `SPI.cpp` / `spi_dma_test.cpp` | split all 16 eDMA lines | **jumper** SDO→SDI (reuse 3.1) |
| **4.4** | **DMA Wire** (LPI2C5) | fresh LPI2C5+eDMA | RM + SDK `fsl_lpi2c_edma` | (eDMA split from 4.3) | **wiring-free** WM8962 |

**Sequence rationale:** 4.1 proves the *new split-IRQ mechanism* with the
least surrounding machinery (one peripheral line + one ISR + a wiring-free
probe), and its gate isolates an `irqcnt` "CM4 took the IRQ" token from the
transaction so a routing bug and a driver bug are distinguishable. 4.2 mirrors
the `wire_slave_test` fixture (CM7 polled master + a bridged slave instance)
with distilled slave code; new cost = the wired master fixture + that
instance's split & bring-up. 4.3 adds the heavier eDMA/`DMAChannel`/`DMAMEM`-OCRAM stack
+ the 16-line eDMA split, reusing the proven jumper. 4.4 is pure composition
of 4.1's LPI2C5 bring-up + 4.3's eDMA. **Each slice is its own commit +
QEMU gate + EVKB probe** — the full silicon-truth loop per increment.

**Blast-radius decision (locked):** the qemu2 split is **targeted, via a
reusable helper** — only `LPSPI1`, `LPI2C5`, and the 16 eDMA lines. Every
other peripheral keeps its current single-NVIC wiring. (The general
"split every line per RM Tables 4-1/4-2" option was rejected: machine-wide
blast radius, and the roadmap files machine-wide qemu2 fidelity under
"HW-arbitrated, defer.")

**Two design calls (locked):** (a) CM4 completion is a direct `volatile`
flag set in the CM4's distilled ISR — **not** the `EventResponder`/`yield`
machinery (lean bare-metal CM4, Phase-3 pattern); the probe only needs "ISR
fired + data correct". (b) DMA buffers live in **OCRAM, not the CM4 DTCM**
(eDMA can't reach TCM).

**Scope guard — explicitly deferred:** byte-level LPSPI-interrupt SPI (non-
idiomatic — SPI's interrupt path is DMA); the general machine-wide IRQ split;
concurrent CM7+CM4 use of the *same* peripheral instance / cross-core
arbitration; qemu2 clock-gate fidelity (the 3.1 circular-pass gap stays
HW-arbitrated).

---

## 1. Goal & success criteria

A **Cortex-M4 image takes a peripheral or eDMA interrupt on its own NVIC**
and drives an interrupt/DMA transaction to completion, for each of the four
slices — streaming observations over the **MU** to the CM7 reporter (the
`cm4_wire_test`/`cm4_spi_test` pattern). The enabling qemu2 change is a
**targeted split-IRQ** routing the relevant lines to both NVICs.

**Per-slice v1 success = QEMU gate green (asserted tokens, stable 3×) AND
the EVKB probe green with asserted tokens byte-identical**, specifically:

- **4.1:** `irqcnt>0` (CM4 serviced the LPI2C5 IRQ) **and** `rdv=0x6243`
  (WM8962 ID read back *by the CM4 ISR master*) on HW; `rdv=0x0000` in QEMU
  (stub) — the documented split-assertion (precedent 3.2/2C).
- **4.2:** CM4 slave `irqcnt>0` and the bytes the master wrote are recorded
  by the CM4 slave ISR (`b0/b1/b2`), with a known single response byte read
  back by the master (single-byte in QEMU per the model limit; multi-byte on
  HW with a real master).
- **4.3:** `rx==tx` through the SDO→SDI jumper in **both** the polled
  (`STAGE_BLOCKING`) and interrupt (`STAGE_ASYNC`) stages, **and** `dmairq>0`
  (CM4 serviced the eDMA completion IRQ). Split failure → `dmairq=0`, async
  hangs to a guarded FAIL, **while blocking still passes** — isolating the
  eDMA-IRQ routing from the DMA-drive.
- **4.4:** `rdv=0x6243` moved by eDMA (HW) / `0x0000` (QEMU stub), with the
  CM4 servicing the eDMA completion.

**The two-layer proof (Phase-3 shape, extended):** QEMU proves the
register/transfer sequence *and* that a split IRQ is delivered when a core's
NVIC has the line enabled — but QEMU delivering it proves **nothing** about
silicon routing (the `ssi-loopback`/CCM circular-pass gaps from 3.1 stand,
and the split is a QEMU model). Only the per-slice HW probe — the CM4 ISR
*actually firing on the EVKB*, witnessed by an un-fakeable data result —
closes the new-model trigger.

---

## 2. Ground truth established during exploration (verified this session)

| Fact | Source |
|---|---|
| Peripheral IRQs are wired to the **CM7 NVIC only**; only the MU reaches both cores, via **two separate sysbus lines** (not a split). Explicit `TODO`: "routing one to the CM4 needs a `TYPE_SPLIT_IRQ` per line." | `hw/arm/fsl-imxrt1170.c:1094` (LPI2C), `:1115` (LPSPI), `:1399` (eDMA 16 ch), `:1402` (eDMA error), `:1477-1480` (MU→both), `:962-966` (TODO) |
| **qemu2 already asserts the peripheral IRQ lines** — the only change needed is the split, no IRQ-raising work: LPI2C `update_irq` covers **master *and* slave** (MIER/SIER × MSR/SSR); LPSPI `update_irq` on `(SR & IER)`; eDMA raises 16 completion IRQs (`mask=(1<<i)|(1<<(i+16))`) | `hw/i2c/imxrt_lpi2c.c:184-200`; `hw/ssi/imxrt_lpspi.c:123-125`; `hw/dma/imxrt_edma.c:129-130,532` |
| eDMA: **32 channels, 16 IRQ lines** (each line shared by ch *i* and ch *i+16*) → split all 16 as a bounded unit (`DMAChannel` allocates dynamically) | `include/hw/dma/imxrt_edma.h:22,24` |
| `TYPE_SPLIT_IRQ` is the correct QEMU idiom (1 in → N out); MU precedent uses two *device* output lines because the MU model exposes two — a single-output peripheral needs the splitter | `hw/core/split-irq.c`; MU wiring `fsl-imxrt1170.c:1477-1480` |
| **Wire master is polled** (`lpi2c1176_master_write/read`, the 3.3 shared C core); the LPI2C **interrupt** is used only for **slave** mode | `Wire/WireIMXRT1176.cpp:107` (`handle_slave_isr`), `:147-156` (polled master delegates to shared core), `:214` (`IRQ_LPI2C5 → wire2_isr → Wire2.handle_slave_isr`) |
| **SPI async == DMA**: `SPI.transfer(buf,ret,count,EventResponder&)` runs on 2 `DMAChannel`s with a **completion ISR** (`dma_rxisr`); no byte-level LPSPI-interrupt path exists | `SPI/SPI.cpp:195` (`_spi_dma_rxISR0`), `:721-741` (`initDMAChannels`, `attachInterrupt`/`interruptAtCompletion`) |
| The CM7 `spi_dma_test` already exercises **both** completion modes in one image: `STAGE_BLOCKING` (DMA, polled DONE) + `STAGE_ASYNC` (DMA, `EventResponder` on RX-completion IRQ); a yield-spin `ASYNC_GUARD` fails a lost completion instead of hanging | `SPI/tests/spi_dma_test/spi_dma_test.cpp:7-45` |
| **DMA buffers must be OCRAM, not DTCM** — "DMA-accessed → OCRAM (DTCM is DMA-unreachable)" | `SPI/tests/spi_dma_test/spi_dma_test.cpp:8-10` |
| The CM7 `wire_slave_test` runs master (`Wire`) + slave (`Wire1@0x42`) on **one board**, bridged onto a shared I2C bus; **QEMU can gate only a single-byte slave response** — "the master reads a whole `requestFrom` in one step without yielding to the slave ISR"; multi-byte is HW-verified against an external MKR-Zero master | `Wire/tests/wire_slave_test/wire_slave_test.cpp:8-36` |
| qemu2 bridges `lpi2c[1]` onto `lpi2c[0]`'s bus (how `Wire`↔`Wire1` loopback works with no wiring in QEMU) — the model for a CM7-master↔CM4-slave QEMU gate | `hw/arm/fsl-imxrt1170.c` (I2CBus `target` select) |
| Peripheral DMA-request routing (for 4.3/4.4): LPSPI RX `36+2n`/TX `37+2n`; LPI2C combined `48+n`; via DMAMUX `req-in`→eDMA `dma-req` | `hw/arm/fsl-imxrt1170.c:1420-1451` |
| LPSPI1/LPI2C5 reachable from the CM4: `cm4_view` overlays a full `system_memory` alias | `hw/arm/fsl-imxrt1170.c:945-950` |
| CM4 DTCM = 128K at `0x20000000` (aliased to `ocram_m4`); TCM is DMA-unreachable → 4.3/4.4 buffers go in system OCRAM | Phase 2A discoveries (roadmap); `fsl-imxrt1170.c:955-960` |
| LPSPI1 clock/pins (LPCG104, ROOT43 mux0, AD_28/30/31) — reuse verbatim | 3.1 spec + `SPI/lpspi1176.c` (HW-verified) |
| LPI2C5 clock/pins (LPCG102, ROOT41 mux1, LPSR pads GPIO_LPSR_05/04) — reuse verbatim | 3.2 spec + `Wire/lpi2c1176.c` (HW-verified) |
| WM8962 R15 readback default = `0x6243` (device ID) | Linux `wm8962.c` reg_default `{15,0x6243}` — **fact source only**, no code (per license firewall); confirmed on silicon by 3.2 |

---

## 3. The qemu2 split-IRQ foundation (built at 4.1; the only qemu2 change)

**Mechanism** — a board-level helper inserts a `TYPE_SPLIT_IRQ` (1→2)
between a peripheral's *existing* IRQ output and both NVICs, so *which core
has the line NVIC-enabled* decides who takes it, exactly as silicon behaves
(RM Tables 4-1/4-2). No peripheral-model change (they already assert their
IRQ lines — §2).

```c
/* Route a peripheral IRQ to BOTH NVICs, as silicon does (RM Tables 4-1/4-2).
 * Inserts a TYPE_SPLIT_IRQ (1->2); enabling the line on the CM4's NVIC is
 * what makes it fire there.  Silicon truth: <probe-name>/2026-07-.. .        */
static void fsl_imxrt1170_connect_irq_both(FslIMXRT1170State *s,
        SysBusDevice *dev, int out, int irqnum, SplitIRQ *split /* in state */)
{
    object_initialize_child(OBJECT(s), <name>, split, TYPE_SPLIT_IRQ);
    qdev_prop_set_uint16(DEVICE(split), "num-lines", 2);
    qdev_realize(DEVICE(split), NULL, &error_abort);
    sysbus_connect_irq(dev, out, qdev_get_gpio_in(DEVICE(split), 0));
    qdev_connect_gpio_out(DEVICE(split), 0, qdev_get_gpio_in(s->armv7m,    irqnum));
    qdev_connect_gpio_out(DEVICE(split), 1, qdev_get_gpio_in(s->armv7m_m4, irqnum));
}
```

*Primitive choice (implementation-time):* the fan-out can be the
`TYPE_SPLIT_IRQ` device (as shown, migration-friendly, what the roadmap TODO
named) **or** the lighter `qemu_irq_split(in_a, in_b)` convenience — same 1→2
behavior; the plan picks. The *design* decision (route each line to both
NVICs; software's NVIC-enable decides who takes it) is what's fixed here.

**Applied to exactly:** `LPSPI1` (1 line, replaces `:1115` for i==0),
`LPI2C5` (1 line, replaces `:1094` for the LPI2C5 index), the **16 eDMA
completion lines** (replaces the `:1399` loop) — all at 4.1 in one change —
**plus 4.2's slave-instance line** when that slice lands (§4.2; not LPI2C5).
The eDMA **error** line (`:1402`) stays CM7-only unless a slice's driver
relies on it (a plan-time detail). Splitter devices are stored as
machine-state children so they are not collected. Every other
`sysbus_connect_irq` is untouched.

**Regression obligation (qemu2 touched — runs once, at 4.1):** the full set
from `silicon-truth-loop.md` — `test_imxrt1170.py` + `test_imxrt1062.py`
functional suites, the `dualcore_mu_test` (diff `transcript_qemu.txt`) /
`serial_test` / mcmgr / rpmsg gates, and `git diff | checkpatch.pl`. The
change only *adds* fan-out to a powered-off / NVIC-masked CM4, so existing
single-core gates should be byte-identical — but the regression set proves
it, not the argument. **4.2 adds one qemu2 delta** — the split for its
slave instance (§4.2) — so the set re-runs at 4.2. **4.3–4.4 add no qemu2
delta** (LPSPI1 + eDMA were split at 4.1).

---

## 4. Per-slice architecture

Each slice is one artifact cloned from `cm4_wire_test`/`cm4_spi_test`: a CM7
Arduino **reporter** (`Multicore.begin(cm4 image)`, read MU tokens, print
`token=HEX` + `<SLICE>=PASS/FAIL`, **never touches the peripheral**), a CM4
**driver+main** (`cm4/main_cm4.c` — the probe), verbatim `cm4/startup_cm4.S`
+ `cm4.ld`, a `run_qemu.sh` (gate-lib pattern), and both transcripts checked
in. CM4 clock+pin self-config is **reused verbatim** from the HW-verified
3.1/3.2 shared cores; only the interrupt/DMA path is new.

### 4.1 — Interrupt Wire-master (LPI2C5) · fresh

- **Code:** distilled LPI2C5 `begin()` (from `lpi2c1176.c`) + a **fresh** ISR
  master — enable `MIER`, `NVIC_ENABLE(IRQ_LPI2C5)`, push the MTDR command
  sequence, and let the CM4 ISR service `MSR` (TDF→next byte, RDF→read,
  NDF→nack) to a completion flag. Validated behaviorally vs SDK
  `LPI2C_MasterTransferHandleIRQ` + Zephyr; **logic fresh** (own file,
  provenance header). Transactions mirror 3.2 (reset-write ACK; ID read-back).
- **Tokens (MU ch0):** `irqcnt` (**isolated** — CM4 serviced the IRQ), `mcr`,
  `lpcg`/`croot` (informative), `ack`, `rdv` (split-assert), `done`,
  → `WIRE_INT_MASTER_CM4=PASS`.
- **Probe:** **wiring-free** WM8962, `clean_boot.scp`. Un-fakeable:
  `rdv=0x6243` produced by the CM4 ISR with `irqcnt>0`. Split-not-routed on
  silicon ⇒ `irqcnt=0` and the transaction hangs to a guarded FAIL.

### 4.2 — Interrupt Wire-slave (mirrors `wire_slave_test`) · distilled

- **Instance:** *not* LPI2C5 — the slave must share a QEMU I2C bus with the
  CM7 master, and the only bus-bridge in qemu2 is `lpi2c[1]→lpi2c[0]` (the
  `wire_slave_test` fixture: `Wire` master + a bridged slave instance). LPI2C5
  gave 4.1 its wiring-free WM8962 benefit, which a *slave* doesn't get. So 4.2
  reuses the `wire_slave_test` instances (CM7 `Wire` master + the bridged
  slave running on the CM4). Exact instance indices/pins pinned at
  triangulation.
- **Code:** distill `handle_slave_isr` from HW-verified `WireIMXRT1176.cpp`
  (SCFGR + slave address, `SIER` TDIE/RDIE/AVIE/SDIE, ISR echo/record) + the
  CM4 clock/pin bring-up for that instance. Provenance + keep-in-sync header.
- **Tokens:** slave `irqcnt`, `b0/b1/b2` (bytes the master wrote), a single
  known response byte, `done` → `WIRE_INT_SLAVE_CM4=PASS`. QEMU master = CM7
  polled on the bridged bus; single-byte response only (model limit).
- **Probe:** **wired** CM7-master ↔ CM4-slave (external SDA↔SDA/SCL↔SCL +
  pull-ups between the two header-accessible instances, `wire_slave_test`'s
  MKR-Zero-master precedent). Multi-byte works on HW (real master yields to
  the slave ISR); single-byte in QEMU. Splitting that instance's line is the
  one extra `connect_irq_both` beyond 4.1 (→ re-run qemu2 regression).
  **Open item (triangulation + bench check):** which two header-accessible
  LPI2C instances to pair for master/slave wiring.

### 4.3 — DMA SPI (LPSPI1 async) · distilled

- **Code:** distilled LPSPI1 `begin()` (from `lpspi1176.c`) + distilled DMA
  path from `SPI.cpp`/`spi_dma_test.cpp`: 2 eDMA channels (TX→`TDR`,
  RX←`RDR` via `triggerAtHardwareEvent`, DMAMUX slots 37/36), **`DMAMEM`
  buffers in OCRAM**, TCDs, kick. Two stages: **`STAGE_BLOCKING`** (poll DMA
  DONE — *no split needed*) and **`STAGE_ASYNC`** (`dma_rxisr`→flag — *needs
  the 16-line eDMA split*). The `dma_rxisr` body is distilled to a **direct
  flag**, not `EventResponder`.
- **Tokens:** `lpcg`/`croot` (informative), `rxb` (polled rx==tx), `dmairq`
  (**isolated** — CM4 serviced the eDMA completion), `rxa` (interrupt
  rx==tx), `done` → `SPI_DMA_CM4=PASS`.
- **Probe:** **jumper** SDO(AD_30)→SDI(AD_31) (reuse 3.1), `clean_boot.scp`.
  Un-fakeable: `rx==tx` through the physical jumper in *both* stages +
  `dmairq>0`. Split-not-routed ⇒ `dmairq=0`, `STAGE_ASYNC` hangs to a guarded
  FAIL, **`STAGE_BLOCKING` still passes** — isolating eDMA-IRQ routing from
  DMA-drive.

### 4.4 — DMA Wire (LPI2C5) · fresh

- **Code:** **fresh** — LPI2C5 `MDER` DMA-enable + eDMA to/from `MTDR`/`MRDR`
  (DMAMUX combined slot `48+n`), completion via the 4.3 eDMA split (or polled
  DONE for a de-risking first stage, symmetric with 4.3). Composes 4.1's
  clock/pins + 4.3's eDMA. Validated vs SDK `fsl_lpi2c_edma` + RM.
- **Tokens/probe:** wiring-free WM8962; `rdv=0x6243` (HW) / `0x0000` (QEMU)
  moved by eDMA, `dmairq>0`, → `WIRE_DMA_CM4=PASS`.

---

## 5. Probe obligations & silicon-truth bookkeeping

**Trigger fired for all four:** the **new qemu2 device model** (split-IRQ) →
**EVKB probe mandatory** per slice. **Clock/power-gating** (CM4 writes CCM) is
covered by *reusing the already-HW-verified 3.1/3.2 bring-up* — no new clock
fact. **Memory-aliasing/TCM** fires for 4.3/4.4 (OCRAM-not-DTCM DMA buffers)
→ the jumper/WM8962 result proves eDMA truly reached CM4-owned system memory.
**Two-sources-disagree** → escalates to a probe if triangulation surfaces it.

**Circular-pass honesty (from 3.1):** QEMU delivers a split IRQ purely from
the model wiring; the `ssi-loopback` echoes on `MEN` and the CCM is a RAM
model. So QEMU green proves the *sequence*, never the *routing* or the
*clock/pins*. The load-bearing proof is the CM4 ISR firing on silicon,
witnessed by `rdv=0x6243` / jumper `rx==tx` — data a mis-routed or
mis-clocked CM4 cannot produce.

**Diff procedure (per slice):** asserted tokens diff byte-identical HW-vs-
QEMU; the **`rdv` split-assertion** (4.1/4.4) is excluded from the cross-world
diff and asserted per-world (`0x0000` QEMU / `0x6243` HW); `lpcg/croot`
printed for diagnosis; each README carries the divergence table. `irqcnt`/
`dmairq` are asserted `>0` in both worlds (the split is modelled in QEMU and
real on HW — both must see the ISR fire).

---

## 6. License & qemu2 firewall

- **Distilled slices (4.2, 4.3):** author-original C re-expressing this
  project's own MIT logic (`WireIMXRT1176.cpp handle_slave_isr`; `SPI.cpp`
  DMA path + `spi_dma_test.cpp`), each with a provenance + keep-in-sync
  header. **Fresh slices (4.1, 4.4):** RM-first logic, behaviorally validated
  vs BSD-3 SDK (`fsl_lpi2c`, `fsl_lpi2c_edma`) and Apache Zephyr, **no copied
  logic**; provenance header naming the validation sources.
- **GPL one-way firewall:** the split-IRQ code is **qemu2-side only** — it
  never enters a firmware repo; trivially clean. (Firmware→qemu2 is the only
  allowed direction and isn't used here.)
- Extend `evkb/tools/license-audit.sh` `GATES` with each new gate
  (`cm4_wire_int_master_test`, `cm4_wire_int_slave_test`, `cm4_spi_dma_test`,
  `cm4_wire_dma_test` — names finalized at plan time) in the **same change**
  as that slice; require `LICENSE-AUDIT: PASS`. The macro-built CM4 sources
  are covered by the `-MMD` depfile walk added 2026-07-18.

---

## 7. Verification sequence (per slice; 4.1 additionally runs the qemu2 set)

1. **QEMU red:** stub the new interrupt/DMA path (leave clock+pins) → no
   `<SLICE>=PASS`, and (4.1/4.3) `irqcnt`/`dmairq`=0; runner exit 1.
2. **QEMU green:** full driver → asserted tokens + world-appropriate `rdv`;
   save `transcript_qemu.txt`; stable 3×.
3. **qemu2 regression (4.1 and 4.2 — the two slices with a qemu2 delta):**
   the full `silicon-truth-loop.md` set + `checkpatch.pl`; `dualcore_mu_test`
   transcript diff clean.
4. **Repo regression:** the source library's own gate stays green
   (`Wire/tests/wire_master_test`, `wire_slave_test`; `SPI/tests/spi_dma_test`)
   + the prior CM4 gates (`cm4_spi_test`, `cm4_wire_test`) build.
5. **License audit:** `LICENSE-AUDIT: PASS` with the new gate covered.
6. **Hardware (final arbiter):** flash + `clean_boot.scp` (4.1/4.3/4.4
   wiring-free / jumper; 4.2 with the master↔slave wiring); confirm
   `<SLICE>=PASS`, asserted tokens byte-identical, `irqcnt`/`dmairq`>0, and
   the world-appropriate `rdv`; save `transcript_hw_evkb.txt`.
7. **Roadmap + commit** per the session-log discipline; flip the slice to
   ★★HW-VERIFIED.

---

## 8. Risks & mitigations

| Risk | Mitigation |
|---|---|
| Split IRQ delivered in QEMU but **not routed on silicon** (the whole new-model bet) | Per-slice HW probe is the arbiter; `irqcnt`/`dmairq` isolate routing from driver logic; a hang→guarded FAIL makes non-routing loud, not silent |
| Fresh ISR master (4.1) / DMA Wire (4.4) logic wrong | RED-first gate; behavioral validation vs SDK/Zephyr; the transactions reuse 3.2's HW-verified WM8962 protocol so only the *interrupt/DMA plumbing* is new |
| eDMA can't reach CM4-owned buffers (4.3/4.4) | Buffers in **OCRAM** per the `spi_dma_test` rule; the jumper `rx==tx` proves eDMA truly moved CM4 memory (not a QEMU-only copy) |
| 4.2 slave probe un-wireable (no two header-accessible LPI2C instances that also bridge in QEMU) | Instance pair chosen at triangulation to satisfy *both* the QEMU bus-bridge and HW header-wireability up front; the `wire_slave_test` fixture is the proven precedent — decided before code |
| qemu2 split changes an existing single-core gate's IRQ timing | Full regression set at 4.1; change only *adds* fan-out to a masked/off CM4 → expected byte-identical, proven not assumed |
| Circular QEMU pass (model ignores clock/pins) | HW probe (`rdv=6243` / jumper) is un-fakeable; unchanged from 3.1/3.2 |
| `EventResponder` scope-creep onto the CM4 | Explicitly excluded — direct `volatile` completion flag in the CM4 ISR |
| Bounded-spin hangs if the ISR never fires (HW) | `ASYNC_GUARD`-style bounded waits (from `spi_dma_test`) → visible FAIL token, not a hang |

---

## 9. Deferred (beyond Phase 4)

- Byte-level LPSPI-interrupt SPI (non-idiomatic; SPI's interrupt path is DMA).
- The **general** machine-wide IRQ split (RM Tables 4-1/4-2) — do-it-once but
  machine-wide blast radius; revisit only when a later phase needs many
  peripherals on the CM4.
- **Concurrent CM7+CM4 use of the same instance / cross-core arbitration** —
  a cross-core ownership protocol (roadmap "Deferred beyond Phase 3" #3).
- qemu2 clock-gate fidelity (close the 3.1 circular pass) — HW-arbitrated.
- D7 (reboot CM4 at a new VTOR) — still queued from Phase 1.
