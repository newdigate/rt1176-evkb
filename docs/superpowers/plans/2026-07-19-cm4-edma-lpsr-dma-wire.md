# CM4 eDMA_LPSR DMA-Wire Implementation Plan (Phase 4 DMA, silicon-corrected)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Genuine **CM4 interrupt-driven DMA** — the CM4 self-configures LPI2C5 + **`eDMA_LPSR`** and DMA-reads the WM8962 device ID, with the eDMA completion interrupt firing on the **CM4's own NVIC natively** (`dmairq>0`).

**Why this exists:** the HW probe of `cm4_spi_dma_test` proved the RT1176 has two eDMAs — the **main eDMA** (`0x40070000`, channel IRQs → CM7 only) and **`eDMA_LPSR`** (`0x40C14000`, channel IRQs → CM4, RM Table 4-2). The CM4 can *drive* the main eDMA but can't take its completion IRQ. True CM4 interrupt-DMA requires `eDMA_LPSR` + an LPSR peripheral (LPI2C5/WM8962). QEMU's single-eDMA-split model hid this — see the `rt1176-cm4-edma-lpsr-split` memory. This plan builds the qemu2 `eDMA_LPSR` foundation and the CM4 DMA firmware, and corrects the main-eDMA→CM4 split fiction.

**Architecture:** qemu2 gains a second `TYPE_IMXRT_EDMA` (`eDMA_LPSR` @ `0x40C14000`, TCD `0x40C15000`) + `TYPE_IMXRT_DMAMUX` (`DMAMUX1/LPSR` @ `0x40C18000`); `eDMA_LPSR`'s 16 channel IRQs go to the **CM4 NVIC only** (IRQ 0-15, native); LPI2C5's `dma-req` re-routes from the main DMAMUX to DMAMUX1/LPSR (source 52); and the main eDMA's 16 IRQs become **CM7-only** (dropping the `connect_irq_both` fiction). The CM4 firmware distills an LPI2C5 register-read into an `eDMA_LPSR` RX channel triggered by LPI2C5's DMA request.

**Tech stack:** qemu2 device instancing + machine wiring (GPL — one-way firewall), bare-metal C (CM4), shared `lpi2c1176` core (MIT), teensy-cmake-macros, gate-lib.sh, LinkServer + pyserial. **HW probe is wiring-free** (WM8962 soldered on LPI2C5; unaffected by the OTG2/A5 header issue).

**Repos:** `~/Development/qemu2` (GPL), `~/Development/rt1170/evkb` (`git -C evkb`). Shared core `~/Development/SPI`... actually `~/Development/Wire/lpi2c1176.{h,c}` (the LPI2C core) consumed unchanged.

---

### Task 1: qemu2 — `eDMA_LPSR` + `DMAMUX1/LPSR` instances

**Files:**
- Modify: `~/Development/qemu2/include/hw/arm/fsl-imxrt1170.h`
- Modify: `~/Development/qemu2/hw/arm/fsl-imxrt1170.c`

- [ ] **Step 1: State fields.** In `fsl-imxrt1170.h`, next to `IMXRTEDMAState edma;` and `IMXRTDMAMUXState dmamux;` add:
```c
    IMXRTEDMAState edma_lpsr;      /* eDMA_LPSR 0x40C14000; IRQs -> CM4 (RM 4-2) */
    IMXRTDMAMUXState dmamux_lpsr;  /* DMAMUX1/LPSR 0x40C18000 */
```

- [ ] **Step 2: Base-address defines.** Near `FSL_IMXRT1170_EDMA_BASE`/`_DMAMUX_BASE`:
```c
#define FSL_IMXRT1170_EDMA_LPSR_BASE   0x40C14000
#define FSL_IMXRT1170_DMAMUX_LPSR_BASE 0x40C18000
```

- [ ] **Step 3: instance_init.** Next to the `edma`/`dmamux` `object_initialize_child`:
```c
    object_initialize_child(obj, "edma-lpsr", &s->edma_lpsr, TYPE_IMXRT_EDMA);
    object_initialize_child(obj, "dmamux-lpsr", &s->dmamux_lpsr, TYPE_IMXRT_DMAMUX);
```

- [ ] **Step 4: realize + map + IRQs (CM4-only) + DMAMUX wiring.** Mirror the main eDMA/DMAMUX realize block. Critically:
  - `eDMA_LPSR`'s 16 channel IRQs go to the **CM4 NVIC only** (no split): `sysbus_connect_irq(SYS_BUS_DEVICE(&s->edma_lpsr), i, qdev_get_gpio_in(armv7m_m4, i))` for `i` in `0..15`. (Error IRQ 16 → CM4 too, or leave; match the main-eDMA pattern.)
  - Wire `dmamux_lpsr` `req-out[ch]` → `edma_lpsr` `dma-req[ch]` for all channels (mirror `:1496`).
  - Map `edma_lpsr` at `FSL_IMXRT1170_EDMA_LPSR_BASE`, `dmamux_lpsr` at `FSL_IMXRT1170_DMAMUX_LPSR_BASE`.

```c
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->edma_lpsr), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->edma_lpsr), 0, FSL_IMXRT1170_EDMA_LPSR_BASE);
    for (int i = 0; i < IMXRT_EDMA_NUM_IRQS; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->edma_lpsr), i,
                           qdev_get_gpio_in(armv7m_m4, i));   /* CM4 only (RM 4-2) */
    }
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->edma_lpsr), IMXRT_EDMA_NUM_IRQS,
                       qdev_get_gpio_in(armv7m_m4, FSL_IMXRT1170_DMA_ERROR_IRQ));
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->dmamux_lpsr), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->dmamux_lpsr), 0, FSL_IMXRT1170_DMAMUX_LPSR_BASE);
    for (int ch = 0; ch < IMXRT_EDMA_NUM_CHANNELS; ch++) {
        qdev_connect_gpio_out_named(DEVICE(&s->dmamux_lpsr), "req-out", ch,
                                    qdev_get_gpio_in_named(DEVICE(&s->edma_lpsr),
                                                           "dma-req", ch));
    }
```

- [ ] **Step 5: Re-route LPI2C5 `dma-req` → DMAMUX1/LPSR.** In the LPI2C dma-req loop (`:1518-1522`), special-case `i == 4` (LPI2C5) to the LPSR dmamux at its LPI2C5 source slot. Confirm the LPSR-DMAMUX LPI2C5 slot: RM DMAMUX1 source 52 = LPI2C5; adapt to the LPSR dmamux's own slot numbering (likely the same 52 minus the LPSR base, or 52 — verify against the RM DMAMUX1 table and the `IMXRT_DMAMUX_NUM_*` bound). The other LPI2C instances stay on the main dmamux.
```c
    for (int i = 0; i < FSL_IMXRT1170_NUM_LPI2CS; i++) {
        DeviceState *mux = (i == 4) ? DEVICE(&s->dmamux_lpsr) : DEVICE(&s->dmamux);
        int slot = (i == 4) ? 52 : (48 + i);   /* LPI2C5 -> DMAMUX1/LPSR src 52 */
        qdev_connect_gpio_out_named(DEVICE(&s->lpi2c[i]), "dma-req", 0,
                            qdev_get_gpio_in_named(mux, "req-in", slot));
    }
```
(If `IMXRT_DMAMUX_NUM_CHANNELS`/source count is < 53, bump the device's source bound or map LPI2C5 to a valid LPSR slot — the implementer verifies the device's `req-in` count and the RM DMAMUX1 numbering.)

- [ ] **Step 6: Correct the main eDMA IRQs to CM7-only.** Change the main-eDMA IRQ loop (`:1477`) from `fsl_imxrt1170_connect_irq_both(&s->edma_irq_split[i], ...)` to a plain `sysbus_connect_irq(SYS_BUS_DEVICE(&s->edma), i, qdev_get_gpio_in(armv7m, i))` (CM7 only), and drop the now-unused `edma_irq_split[*]` (state field, init, realize) — OR leave the splitters unrealized-safe if removal is risky; prefer clean removal with a comment citing RM Table 4-1 (main eDMA = CM7 domain). Add a comment: main eDMA → CM7; eDMA_LPSR → CM4 (the two-eDMA reality, HW-confirmed 2026-07-19).

- [ ] **Step 7: Build + smoke.** `cd ~/Development/qemu2/build && ninja qemu-system-arm`. Then run the existing `cm4_wire_test` (CM4 polled LPI2C5) + `cm4_wire_int_master_test` (4.1) gates — both must still PASS (LPI2C5 register path + the LPI2C5 split-IRQ unaffected). `cm4_spi_dma_test` will now show `dmairq=0` in QEMU too (main eDMA → CM7) — that's the intended correction; note it (its gate now fails the async assertion, matching silicon — it will be reframed as polled in a later task, out of scope here).

- [ ] **Step 8: checkpatch + commit (qemu2).**
```
fsl-imxrt1170: add eDMA_LPSR (0x40C14000) + DMAMUX1/LPSR (0x40C18000), IRQs -> CM4; main eDMA IRQs -> CM7-only

RM Tables 4-1/4-2 (HW-confirmed 2026-07-19): the main eDMA's channel IRQs are
CM7-domain; the CM4 domain's IRQ 0-15 are eDMA_LPSR. Model eDMA_LPSR + its
DMAMUX as second instances with IRQs to the CM4 NVIC, re-route LPI2C5's DMA
request to DMAMUX1/LPSR, and correct the main eDMA to CM7-only (the prior
connect_irq_both split to the CM4 was a fiction no silicon path matches).
```

---

### Task 2: qemu2 regression (foundation touched)

- [ ] Full set per `silicon-truth-loop.md`: `test_imxrt1170.py` + `test_imxrt1062.py`; `dualcore_mu_test` (diff transcript), `serial_test`; the CM4 gates `cm4_wire_test`/`cm4_wire_int_master_test`/`cm4_wire_int_slave_test`; `Wire`/`SPI` gates. Expect all PASS **except** `cm4_spi_dma_test`'s async assertion (now silicon-faithful `dmairq=0` — document, don't "fix" here). `git diff | checkpatch.pl --no-signoff -`.

---

### Task 3: Scaffold `cm4_wire_dma_test` (RED)

**Files:** `evkb/cm4_wire_dma_test/{CMakeLists.txt, toolchain/, cm4/cm4.ld (+OCRAM2), cm4/startup_cm4.S, cm4/main_cm4.c (stub), cm4_wire_dma_test.cpp, run_qemu.sh}`

- [ ] Mirror `cm4_wire_int_master_test` (4.1) for the LPI2C5 clock/pin descriptor + MU scaffolding, and `cm4_spi_dma_test` for the OCRAM2 `.dmabuf` linker region. Startup vector: the `eDMA_LPSR` RX channel's completion IRQ (choose RX channel 0 → CM4 IRQ 0 → vector index 16) at `DMA_CH0_IRQHandler`; keep MU at 118. CM7 reporter tokens: `ready, croot, rdv (device ID DMA'd), dmairq (>0), err, done`; PASS = `dmairq>0 && err=0 && done=1` (`rdv` world-split: QEMU wm8962-stub `0x0000` / HW `0x6243`, asserted HW-side). RED = `ready` present, `dmairq=TIMEOUT`.
- [ ] Build + gate RED; commit `cm4_wire_dma_test: scaffold Phase 4 eDMA_LPSR DMA-Wire gate (RED)`.

---

### Task 4: CM4 `eDMA_LPSR` DMA-Wire firmware (GREEN)

**Files:** Modify `evkb/cm4_wire_dma_test/cm4/main_cm4.c`

- [ ] The CM4: ungate `eDMA_LPSR` clock (its LPCG — verify the LPSR-DMA LPCG in the RM), `DMA_CR` on `eDMA_LPSR` (0x40C14000) = GRP1PRI|EMLM|EDBG; self-config LPI2C5 (shared `lpi2c1176_begin`); polled-write the WM8962 register pointer (R15, reuse the 4.1 hybrid discipline), then set up an `eDMA_LPSR` RX channel (TCD at `0x40C15000+ch*0x20`) triggered by LPI2C5's DMA request (LPI2C5 `MDER.RDDE`; DMAMUX1 source 52), reading `MRDR` → an OCRAM2 buffer, `CSR.INTMAJOR` set; NVIC-enable the `eDMA_LPSR` channel IRQ (CM4 IRQ = channel #, native — the whole point); the ISR sets `dmairq`. Stream `rdv` (the DMA'd device ID) + `dmairq`.
- [ ] Gate GREEN in QEMU (`dmairq>0`, `err=0`; `rdv=0x0000` stub). 3× stable; transcript; commit.
- **Contingency:** if the LPI2C5-DMA-read path is too intricate for a clean gate, fall back to the simplest un-fakeable `eDMA_LPSR` proof — DMA a known buffer→`MTDR` (or memory→memory on `eDMA_LPSR`) with `INTMAJOR` → CM4 IRQ, asserting `dmairq>0` + the DMA moved the data. The *essential* proof is the CM4 taking an `eDMA_LPSR` completion IRQ natively; the WM8962 read is the bonus HW oracle. Document any such simplification.

---

### Task 5: license-audit + Task 6: EVKB probe (wiring-free) + Task 7: docs

- [ ] **License:** add `cm4_wire_dma_test` to `GATES`; run; commit.
- [ ] **Probe (wiring-free, WM8962 on LPI2C5):** flash `cm4_wire_dma_test.elf`; pyserial capture. Expect `dmairq>0`, `rdv=00006243` (real WM8962 ID DMA'd by `eDMA_LPSR`, IRQ taken on the CM4's NVIC), `err=0`, PASS. Un-fakeable: the wm8962-stub reads `0x0000`; only silicon returns `0x6243`, and only a real `eDMA_LPSR`→CM4 IRQ yields `dmairq>0`. clean_boot; commit transcripts.
- [ ] **Docs:** roadmap (this becomes the Phase-4 DMA milestone, superseding the main-eDMA-split 4.3/4.4 plan) + spec as-landed + memory (`rt1176-cm4-edma-lpsr-split` update to HW-VERIFIED, and reframe `cm4_spi_dma_test` as the polled-LPSPI1 result). Also: reframe `cm4_spi_dma_test`'s gate to assert only the polled path (drop the `dmairq`/`rxa` interrupt assertions now known CM7-only) so it stays green post-Task-1 — a small follow-up.

---

## Self-review
- **Silicon-truth captured:** the two-eDMA finding drives the whole plan; main eDMA → CM7 (corrected), eDMA_LPSR → CM4 (new), LPI2C5 → DMAMUX1/LPSR (re-routed). 
- **Reuses proven pieces:** the `TYPE_IMXRT_EDMA`/`TYPE_IMXRT_DMAMUX` device models (2nd instances, no model change), the `lpi2c1176` core, the 4.1 LPI2C5 descriptor + WM8962 device-ID oracle, the OCRAM2 `.dmabuf` pattern.
- **Open detail for the implementer:** the exact LPSR-DMAMUX `req-in` bound + LPI2C5 slot (verify `IMXRT_DMAMUX_NUM_*` vs RM DMAMUX1 source 52) and the `eDMA_LPSR` LPCG gate — both verify-in-plan, not assumed.
