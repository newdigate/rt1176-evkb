# CM4 takes the USB port — Phase 7.1 design

**Date:** 2026-08-06
**Status:** designed, not implemented
**Skill:** `cm4-bringup` (silicon-truth dual-core methodology)
**Arc:** Phase 7 — USB host audio on the CM4, sub-project 1 of 4

## 1. Why this exists

The user's goal is a **fully autonomous CM4**: the CM4 owns the USB host
stack, the AudioStream graph, the WM8962 codec and SAI, while the CM7 boots
it and parks. Phase 6 already proved the second half of that sentence on
silicon (`cm4_audio_test`: CM4 owns codec + interrupt-driven SAI + graph +
FFT, `cm7_audio_isers=0`, audible 1 kHz). This phase attacks the first half.

**It is worth being honest about what this buys.** Measured on the EVKB the
same day (`examples/usb/usb_audio_duplex_test/transcript_hw_cpu_budget.txt`),
the genuine USB host duplex work is **1.16% of the CM7 at 996 MHz**
(regression of instrumented buckets against loop rate over a 19× range,
intercept 1.163%, R² = 1.00000). So moving USB to the CM4 **does not buy CPU
back** — it buys *isolation*: a CM7 that is entirely free, and a CM4 that
owns a complete audio path end to end. That is the stated goal, and this
design serves it; but nobody should later read this phase as a performance
optimisation, because it is not one.

## 2. Phase 7 decomposition

"Fully autonomous" is four sub-projects. Each gets its own spec → plan →
implementation cycle. This document covers **7.1 only**.

| | Sub-project | Proves |
|---|---|---|
| **7.1** | **CM4 takes the USB port** | CM4 self-configures LPCG115 + USBPHY2 PLL + EHCI host mode, and takes IRQ 135 on its own NVIC |
| 7.2 | Stack compiles and enumerates on the CM4 | Shim expansion, OCRAM DMA section in `cm4.ld`, header decoupling — CM4 reports the dongle's VID/PID |
| 7.3 | Iso streaming on the CM4 | Rings in OCRAM1; CM4 measures 1000 pkts/s |
| 7.4 | Autonomous capstone | CM4 owns USB + graph + WM8962/SAI, CM7 parks; priority discipline per Phase 6 finding 2 |

## 3. What 7.1 delivers

A new gate `examples/dualcore/cm4_usb_irq_probe/`, in the shape of
`examples/dualcore/cm4_sai_irq_probe` (Phase 5's precedent):

- the **CM4** self-configures the USB host port from scratch — ungates
  `CCM_LPCG115`, brings up the USBPHY2 480 MHz PLL, resets and configures the
  EHCI controller into host mode, powers the port, and enables **IRQ 135** on
  its own NVIC;
- the CM4 counts IRQ-135 entries in its own handler and streams observations
  over the MU;
- the **CM7** boots the CM4 image and reports over VCOM. It configures no USB
  register itself — a CM7 that touched the block would make a CM4 failure
  invisible.

### Approach (chosen: A, with B as fallback)

**A — full self-configuration with staged tokens.** The CM4 does every step,
and emits an observation token per stage so a red gate localises itself
rather than just failing. This matches the Phase 3 doctrine that *the CM4
self-configures* the peripheral (which fires the clock-gating probe trigger
deliberately — "that probe is the point, not a tax"), and it is the exact
shape of `cm4_sai_irq_probe`.

**B — fallback, only if A goes red and the stage tokens do not localise it.**
Split into two probes: 7.1a brings up PHY + EHCI and *polls* PORTSC with no
interrupt (isolating clock/PHY/register access), 7.1b then adds IRQ 135.
Two gates and two silicon runs for one capability, so it is not the default.

**C — rejected: CM7 configures, CM4 only takes the IRQ.** Fastest answer to
"does the split work", but it tests a configuration the final design never
uses. That is the same circular-pass shape 3.1 warned about, where the gate
passes for a reason unrelated to the capability.

## 4. Register sequence

Provenance matters here, so state it plainly: **the register literals are
inherited verbatim from a CM7 path that is currently working on silicon**
(`USBHost_t36/ehci.cpp:233-300`, exercised by every USB audio example and
measured streaming at 1000 pkts/s the same day). They are not freshly derived
from the RM, and they do not need to be — a working, measured implementation
is stronger evidence than a datasheet reading. This is the Phase 3.3 idiom
("CM7-logic-verbatim").

What *is* new in 7.1 is **which core issues the writes**. That is what the
probe answers, and no amount of register triangulation can answer it.

Block bases cross-checked against the RM memory map (`rm_full.txt:2480-2483`)
and against qemu2 (`hw/arm/fsl-imxrt1170.c:253`):

| Symbol | Address | Source |
|---|---|---|
| `USB_OTG2_BASE` | `0x4042C000` | RM 2483, qemu2 253, `imxrt1176.h:940` |
| `USBPHY2_BASE` | `0x40438000` | RM 2480, `imxrt1176.h:956` |
| `CCM_LPCG115_DIRECT` | `0x40CC6E60` | `imxrt1176.h:876`; = `0x40CC6000 + 115*0x20` |
| `IRQ_USB_OTG2` | 135 | RM Table 4-1 (`:3137`) **and Table 4-2** (`:3910`) |

Table 4-2 listing 135 is the fact this whole phase rests on: the USB OTG2
interrupt is in the **CM4 domain**, exactly as SAI1's line 76 was for Phase 5.

Sequence, in order, each stage emitting a token:

1. `CCM_LPCG115_DIRECT = 1` — shared USB clock gate. **Risk trigger:
   clock/power gating**, and the reason this probe is mandatory. Precedent:
   the CM4 has self-ungated LPCG104 (3.1) and LPCG102 (3.2) on silicon.
2. `USBPHY2_CTRL_CLR = SFTRST`; `PLL_SIC_SET = PLL_REG_ENABLE`; wait ≥15 µs;
   `PLL_SIC_SET = PLL_POWER`; `PLL_DIV_SEL = 3` (24 → 480 MHz);
   `PLL_SIC_CLR = PLL_BYPASS`; `PLL_SIC_SET = PLL_EN_USB_CLKS`;
   `CTRL_CLR = CLKGATE`; poll `PLL_LOCK`; `PWD = 0`.
   **Explicitly NOT an ANATOP AI-write handshake** — Phase 6 finding 1 (the
   CM4 hangs on `ai_write`) does not apply, because USBPHY2 has its own
   PLL behind plain MMIO. This is a prediction the probe tests.
3. EHCI: `USBCMD |= RST`, spin until clear.
4. `USBMODE = CM(3) | SDIS` — host mode with stream disable, matching
   `ehci.cpp:283`.
5. `PORTSC1`: port power on; read back CCS.
6. `USBINTR = PCE` — port-change interrupt only. 7.1 does not run schedules,
   so UAI/UPI are deliberately not enabled; that is 7.2's business.
7. NVIC: priority, then enable IRQ 135, preceded by `cpsie i`. Phase 5 hit
   exactly this: the copied `startup_cm4.S` leaves PRIMASK set, which would
   have false-FAILed on silicon while *passing* in QEMU. Note the fix lives
   in the image's own main, not in the shared startup —
   `cm4_sai_irq_probe/cm4/main_cm4.c:101` carries it as a one-line
   `__asm volatile ("cpsie i" ::: "memory")`. **Every new CM4 image must
   repeat it**; it is not inherited.

### Vector table

`attachInterruptVector` is a documented no-op in `cm4_shim/Arduino.h` — the
CM4's vector table is static per image (`startup_cm4.S`). So the image
carries a static `USB_OTG2_IRQHandler` at index `16 + 135 = 151`, the same
pattern Phase 5 used for `Software_IRQHandler` at `16 + 44`. 7.1's handler is
self-contained (increment a counter, clear `USBSTS`), so no library symbol
needs exposing yet.

**Deferred to 7.2, recorded here so it is not rediscovered:** the real stack's
`USBHost::isr` is `private` (`USBHost_t36.h:307`), so 7.2 will need a small,
non-breaking USBHost_t36 change to give the static vector table something to
call — the CM7 gets away with `attachInterruptVector` at runtime, the CM4
cannot.

## 5. The qemu2 change

qemu2 wires both USB IRQs to the CM7 NVIC only:

```c
sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(armv7m, usb_irq[i]));  /* :1418 */
```

The change is one `SplitIRQ` field on `FslIMXRT1170State` plus one
`fsl_imxrt1170_connect_irq_both()` call for `i == 1` (USB_OTG2 / 135) —
identical in shape to the existing LPI2C5, LPI2C2, LPSPI1 and SAI1 splitters
(`:1184`, `:1194`, `:1220`, `:1265`). OTG1 (136) stays CM7-only; nothing in
this tree uses it as a host.

**Run red-first.** The gate is written and run *before* the model changes,
and its failing output is checked in as `transcript_qemu_red.txt`, exactly as
`cm4_sai_irq_probe` did. This is the discipline that proved the SAI fan-out
was genuinely absent rather than assumed — without it, a green gate proves
nothing about whether the split was needed.

**GPL firewall:** the qemu2 change is GPL-2.0 and stays local to `~/Development/qemu2`.
It is never pushed to a firmware repo, and no qemu2 code flows into `evkb`,
`cores`, or any library. Firmware→qemu2 is fine; the reverse is not.

If qemu2 is touched, its regression set and `scripts/checkpatch.pl` run too
(cm4-bringup checklist step 6).

## 6. Gate and probe

### QEMU gate — `run_qemu.sh`

Boots the dual-core image on `mimxrt1170-evk` with an emulated device on the
host bus, reusing what the Option-A survey gate established on 2026-08-06
(`examples/usb/usb_descriptor_survey/run_qemu.sh`):

```
-audiodev none,id=snd0 -device usb-audio,bus=usbhost.0,port=1,audiodev=snd0
```

Asserts: each stage token; PLL lock; `USBMODE` readback in host mode;
`ccs=1` (the emulated device is seen); `irqcnt > 0`; and `USB_IRQ_CM4=PASS`.

`irqcnt` is asserted as **`> 0`, not an exact count** — precedent: 2C's
`systick`, 3.2's `rdv`, and Phase 5's own `irqcnt` (QEMU 64 vs HW 20 on the
SAI probe, a documented fidelity limit). An exact count would be a fiction
about the model.

### Silicon probe — MANDATORY

Risk triggers fired: **clock/power gating** (CM4 writes CCM), **reset/default
values** (PHY PLL bring-up), **boot/reset sequencing**, and **a new qemu2
model behaviour**. Any one mandates a probe; all four fire.

Run via `clean_boot.scp` for an uncontaminated M4-held boot, and check in
`transcript_hw_evkb.txt` beside `transcript_qemu.txt`.

**The un-fakeable assertion is plug/unplug.** A gate that only reads back
registers the CM4 just wrote is circular — the 3.1 `ssi-loopback` trap in a
new costume. So the probe requires physically plugging and unplugging a
device on **J47 (USB_OTG2)** and asserting that **both** move together:

- `irqcnt` increments (the CM4 took a real interrupt from real silicon), and
- `PORTSC1.CCS` changes 0→1 on plug and 1→0 on unplug.

Neither alone is sufficient. `irqcnt` alone could be a spurious or latched
line; `CCS` alone proves the port works but says nothing about the NVIC.

**Board trap to avoid, already documented:** an OTG adapter on OTG2 grounds
`USB_OTG2_ID` = `GPIO_AD_08` = header pin A5, which clamps LPI2C1 SCL to 0 V
and silently kills header I²C. 7.1 uses no header I²C so it is harmless here,
but 7.4 will own the WM8962 over LPI2C5 — record it now.

## 7. The OCRAM DMA trap (found while designing; 7.2 will hit it)

qemu2 gives the EHCI controller a dedicated DMA address space with the CM7's
ITCM (`0x0`) and DTCM (`0x20000000`) overlaid as **unreachable holes**
(`fsl-imxrt1170.c:77-101`), modelling the real constraint recorded at
`USBHost_t36/memory.cpp:60` — "plain `.bss` is DTCM, which the EHCI DMA master
cannot reach".

**The CM4's own DTCM is also at `0x20000000` from its view.** So a buffer in a
CM4 image's `.bss` is doubly unusable: it resolves to the wrong memory in the
system map *and* that address is holed. The CM4 image therefore needs an
OCRAM-resident DMA section — `cm4.ld` gains a `.bss.dma` in **OCRAM1
(`0x20240000`, 512 K)**, clear of **OCRAM M4 (`0x20200000`–`0x2023FFFF`)**
where the CM4's own TCM backdoor and `Multicore.begin`'s default `stageAddr`
live.

7.1 does not run schedules and so allocates no DMA memory — but the trap is
recorded here because it is invisible until it silently corrupts memory, and
because the design that hits it is already decided.

## 8. Success criteria

7.1 is done when **all** of:

1. QEMU gate green, stable 3× (`USB_IRQ_CM4=PASS`, `irqcnt > 0`, `ccs=1`).
2. `transcript_qemu_red.txt` checked in, showing the gate genuinely failing
   before the qemu2 split existed.
3. EVKB clean-boot transcript checked in; `irqcnt` and `CCS` **both** move on
   physical plug and unplug, stable across at least 2 runs.
4. Any HW-vs-QEMU divergence documented rather than absorbed, with the
   measurement and date in a code comment and the commit message.
5. `tools/license-audit.sh` extended with the new gate, and PASS.
6. qemu2 regression set + `scripts/checkpatch.pl` green (qemu2 was touched).
7. Roadmap updated with the phase status and any new discoveries.

## 9. Non-goals

- **Isochronous data.** QEMU's `usb-audio` model enumerates but does not
  stream (measured 2026-08-06: full control sequence completes, then
  `pkts/s=0`). Silicon is the only proof that audio flows, and that is 7.3.
- **Any latency claim.** 7.1 runs no schedule. The ~½ ms service-latency
  budget measured on the CM7 is 7.3/7.4's problem.
- **Porting USBHost_t36.** 7.1 writes its own ~100-line register sequence.
  The 5,797-line stack port is 7.2.
- **OTG1.** One host port; OTG1 stays CM7-side and CM7-only.
- **Concurrent CM7+CM4 use of the USB block.** The CM4 owns it exclusively,
  as the CM4 owned LPSPI1 in 3.1 and LPI2C5 in 3.2.

## 10. Known risks

| Risk | Why it might bite | Mitigation |
|---|---|---|
| CM4 cannot bring up the PHY PLL | Phase 6 found the CM4 hangs on the ANATOP AI handshake. Predicted not to apply (USBPHY2 PLL is plain MMIO), but predicted ≠ proven | Stage token isolates it. If it hangs, fall back to the Phase 6 pattern: CM7 pre-arms the PHY, CM4 image built `-DUSBPHY_EXTERNAL` |
| QEMU passes, silicon fails | qemu2 does not enforce clock gating everywhere — 3.1's `ssi-loopback` echoed on `CR_MEN` alone | The plug/unplug assertion cannot be satisfied by a mis-clocked port |
| Silicon passes, QEMU fails | The model may not raise PCI on device attach | Red-first run tells us the baseline before the split; if PCI never fires in QEMU, gate on silicon and document the model limit |
| CM4 startup leaves PRIMASK set | Phase 5 hit exactly this; QEMU did not catch it | `cpsie i` before the NVIC enable, and a token proving interrupts are unmasked |
| Split IRQ regresses CM7 USB | Every existing USB gate runs on the CM7 | Re-run all 12 USB gates after the qemu2 change; `connect_irq_both` fans out rather than redirecting |

## 11. Fallback trigger

Switch to approach **B** if the QEMU gate or the silicon probe goes red *and*
the stage tokens do not identify which step failed. B splits the probe into
7.1a (PHY + EHCI, polled PORTSC, no interrupt) and 7.1b (add IRQ 135), so a
failure is attributable to clock/PHY or to interrupt routing but never both.
