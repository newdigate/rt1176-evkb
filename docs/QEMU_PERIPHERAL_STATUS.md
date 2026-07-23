# i.MX RT1170 QEMU Model — Peripheral Status (dual-core)

Status of the **qemu2** machine model for the NXP i.MX RT1176 — now covering
**both cores** (Cortex-M7 primary + Cortex-M4 secondary) — and the gate
harness that verifies it. Last full revision: **2026-07-18** (post Phase 3.2).

> Supersedes the 2026-06-08 revision, which documented the retired
> `~/Development/qemu` `imxrt1170-cm7-skeleton` tree and the old `rt1170-fw`
> bare-metal harness. Everything below refers to the current tree.

- **Machine:** `mimxrt1170-evk` · **SoC type:** `fsl-imxrt1170`
- **CPUs:** Cortex-M7 (FPU, caches, 218 ext IRQs, 4 prio bits) + **Cortex-M4F**
  (own NVIC, `start-powered-off`, released by SRC — see the dual-core section)
- **QEMU tree:** `~/Development/qemu2` (branch `master`)
- **Build:** `ninja -C build qemu-system-arm` (clang ≥ 15 Apple / ≥ 10 upstream —
  CLT clang 14 silently miscompiles and the M7 crashes on boot)
- **Run (evkb firmware):**
  `qemu-system-arm -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel fw.elf -display none -serial file:out.uart`
  — gates go through `evkb/tools/qrun` (gtimeout + log cap) + `gate-lib.sh`.
- **`boot-xip`:** enables the boot-ROM stub — reset enters the ROM, which parses
  the FlexSPI IVT and executes the image's **DCD/XMCD** before dispatch (the
  same path real silicon takes; EVKB-validated against NXP SDK images).

Legend: ✅ modelled & gate-verified · 🟡 stubbed (absorbs accesses, no
behaviour) · ⬜ not modelled.

---

## Memory map

| Region | Base | Size | Notes |
|---|---|---|---|
| CM7 ITCM / DTCM | `0x0000_0000` / `0x2000_0000` | **FlexRAM-programmed** | Bank config from IOMUXC **GPR16/17** is modelled (`imxrt1170_flexram_update`): reset = the RT1176 fuse default **256K+256K**; firmware's 8/8 split (`0xFFFFAAAA`) gives 512K+512K. Unbacked TCM space faults, as on silicon. |
| OCRAM M4 backdoor | `0x2020_0000` | 256K | Backs the **CM4's private ITCM/DTCM** (see dual-core) |
| OCRAM1 / OCRAM2 / OCRAM M7 | `0x2024_0000`… | — | RAM |
| FlexSPI1 XIP | `0x3000_0000` | 16 MiB | RAM-backed; **FlexSPI controller** (`imxrt_flexspi`) modelled — incl. the IP-command path the flash-EEPROM driver uses (program + AHB-invalidate) |
| SEMC SDRAM | `0x8000_0000` | 64 MiB | **Faithful window**: unmapped until firmware's `semc_sdram_init()` completes the SEMC Mode-Set IPCMD — a missing/incorrect init **faults** instead of false-passing on plain RAM |
| Peripherals | `0x4000_0000`+ | — | per-device |
| PPB | `0xE000_0000` | — | per-core (each CPU has its own NVIC/SysTick/DWT) |

---

## ✅ Dual-core: CM4 + MU + SRC (Phases 1–3.2, all ★★HW-verified)

The headline change since the June revision (which listed the CM4 as "not
represented at all"): the secondary core and the whole boot/IPC surface are
modelled and validated **byte-identical against EVKB silicon** transcripts.

- **CM4 CPU:** M4F (`vfp`, bitband), own NVIC, `start-powered-off`; held until
  the CM7 sets `SRC.SCR.BT_RELEASE_M4` (write-1-only, as on silicon); reset
  VTOR from the **IOMUXC LPSR GPR0/GPR1** vector registers; `CTRL_M4CORE.SW_RESET`
  re-pulse restarts it (D7 new-VTOR reboot: likely-works, clean probe queued).
- **CM4 private view** (`cm4_view`): ITCM `0x1FFE0000` / DTCM `0x2000_0000`
  (128K+128K fixed LMEM) alias the **OCRAM-M4 backdoor** `0x2020_0000` the CM7
  stages images through; everything else falls through to shared system memory
  — so the CM4 reaches GPIO, LPSPI, LPI2C, CCM, LPSR-domain peripherals, etc.
- **MU** (`imxrt_mu`, MUA `0x40C48000`-side/MUB): mailboxes TR/RR ×4, GIR/GIP
  doorbells, flags; **IRQ 118 wired to both NVICs**. Models the measured
  silicon quirks: `ASR` bit 9 always-1, `ASR.RS` never sets (probe-cited in
  the source).
- **Verified by:** `dualcore_mu_test` (probe), `cm4_boot_test` (Multicore/MU
  library), `cm4_image_test` (real C image, `.data`/`.bss`/FPU/DTCM stack),
  `cm4_intr_test` (CM4 DWT + SysTick + MU IRQ in the CM4's own ISR),
  `cm4_dual_test` (CM4-driven GPIO + cross-core visibility + IPC),
  `cm4_spi_test` (CM4 self-configures LPSPI1, polled loopback),
  `cm4_wire_test` (CM4 self-configures LPI2C5, polled I2C to the WM8962) —
  plus NXP SDK **MCMGR hello_world** and **rpmsg_lite pingpong** byte-identical
  QEMU-vs-board.
- **Known gap:** peripheral interrupts fan out to the **CM7 NVIC only** (only
  the MU reaches both cores). Routing a peripheral IRQ to the CM4 needs a
  per-line `TYPE_SPLIT_IRQ` — a new-model risk trigger + probe when first
  needed. Hence CM4 peripheral drivers are **polled-first**.

---

## ✅ Modelled peripherals (by area)

Per-register details live in the model sources (`hw/*/imxrt_*.c`) — each model
carries probe-cited comments where silicon corrected it. "Gate" names the
evkb/library gate that exercises it end-to-end.

| Area | Devices (instances) | Model | Gate(s) |
|---|---|---|---|
| Console / UART | LPUART ×12 | `imxrt_lpuart` | `serial_test`, `serial_test_rx`, every gate's token output |
| Clocks / power | CCM (+OBS), ANADIG, DCDC | `imxrt_ccm`, `imxrt_anadig`, `imxrt_dcdc` | boot path of every gate; ★CCM is a RAM-backed register file — LPCG/root **readback** matches HW but **functional gating is not enforced** (see divergences) |
| Pins / reset | IOMUXC ×6 windows (incl. LPSR + GPR), SRC | `imxrt_iomuxc`, `imxrt_src` | all gates; SRC also owns the **CM4 release** |
| GPIO | ×13 | `imxrt_gpio` | `blink`, `irq_attach_test`, `cm4_dual_test`; ★PSR masks output bits (see divergences) |
| Timers | GPT ×6, PIT ×2, QTMR, FlexPWM | `imxrt_gpt`, `imxrt_pit`, `imxrt_qtmr`, `imxrt_flexpwm` | `interval_timer_test`, `tone_test`, `pwm_test`; ★timing gates need `-icount` |
| DMA | eDMA ×32ch + DMAMUX | `imxrt_edma`, `imxrt_dmamux` | `edma_test`, SPI-DMA, I2S; ★sw-START = whole-major in QEMU vs one minor loop on silicon (`single_minor` flag) |
| I2C | LPI2C ×6 (master + **slave persona** on LPI2C2) | `imxrt_lpi2c` | Wire `wire_master_test`/`wire_slave_test`, `cm4_wire_test`; ★NDF deliberately **deferred** to trail TDF (silicon-corrected — immediate NDF let a broken scan false-pass) |
| SPI | LPSPI ×6 | `imxrt_lpspi` | SPI `spi_loopback_test`/`spi_dma_test`, `cm4_spi_test`; ★`min_access_size` 1 so 8-bit DMA works; TCR is a queued command word |
| Audio / analog | SAI ×4 (TX **and RX**), WM8962 stub, DAC, LPADC ×2 | `imxrt_sai`, `wm8962_stub`, `imxrt_dac`, `imxrt_lpadc` (`imxrt_adc` is the 1062 machine's) | `i2s_audio_test`, `sai_rx_test`, `audioinput/output_i2s_test`, `audiostream_test`, `sd_wav_play_test`, `dac_test`, `analog_test`; SAI1 has **tap/inject chardevs** (`sai1-tap`, `sai1-rxinject`) for sample-exact gating |
| Storage | USDHC ×2 (+sd-card), FlexSPI ctrl (flash EEPROM) | reuses `imx-usdhc`; `imxrt_flexspi` | `sd_test`, `eeprom_test` |
| Networking | ENET 10/100 + ENET_1G (imx.enet ×2) | reuses `imx_fec` | `enet_test`, `ethernet_test` (lwIP), `native_ethernet_test` (FNET) — DHCP/TCP/UDP/DNS over SLIRP (`-nic user`, guestfwd); ★checksum-offload off under SLIRP; ENET IRQ 137 exercised by FNET |
| USB | OTG1/2 + PHYs — **host AND device mode** | reuses `chipidea` (+ its UDC device mode), `imx-usb-phy`, plus `dev-midi` | device: `usb_enum/data/keyboard/mouse/joystick_test` (CDC+HID composite; reactive `hid_in_mask` int-IN tap); host: `usb_host_hid_test`, `usb_midi_test`, `usb_msc_block/fs_test`; ★OTG2 host DMA has a deliberate **TCM hole** (caught 2 real stack-DMA bugs pre-HW) |
| CAN | FlexCAN ×3 | `imxrt_flexcan` | FlexCAN lib gates; ★`SRXDIS`-gated loopback (honesty fix) + **synchronous** delivery vs ~108µs on silicon |
| External RAM | SEMC + SDRAM | `imxrt_semc` | `sdram_test`, `extmem_test` (faithful window) |
| Graphics | PXP (2D Pixel Pipeline) | `imxrt_pxp` | `pxp_blit_test`; ★models documented reset values (`CTRL`=`0xC000_0000` held in reset until the RM §52.5 sequence runs), real per-format bpp decode, `PS_BACKGROUND` RGB888→output conversion, and 90°/180°/270° rotation via the source-frame convention (`OUT_LRC`/`OUT_PS_LRC` = source dims — the hardware rotates that frame and lays it out via `OUT_PITCH`); does **not** model CSC1 — plain RGB pixel copy only (see divergences) — scaling/decimation, alpha-surface compositing, YUV formats, the `NEXT` queue, or AXI-error injection |
| RTC / security / misc | SNVS (SRTC), OCOTP, Key Manager, WDOG/RTWDOG/EWM, LPADC, FlexIO, eLCDIF | `imxrt_snvs`, `imxrt_ocotp`, `imxrt_key_manager`, `imx2.wdt`/`imxrt_rtwdog`/`imxrt_ewm`, `imxrt_lpadc`, `imxrt_flexio`, `imx6ul_lcdif` | `rtc_test` (★HPCR[HP_TS] honoured), display/misc gates from the CM7 bring-up era |
| Dual-core | CM4 + MU + SRC release + LPSR GPRs | machine + `imxrt_mu` | see the dual-core section |

### Board test fixtures (`hw/arm/mimxrt1170-evk.c`)

Attached at machine creation so firmware sees a realistic EVKB:

| Fixture | Where | Purpose |
|---|---|---|
| **AT24C02 EEPROM** @0x50 | LPI2C1 (Arduino header) | Wire master round-trips real I2C |
| **`wm8962-stub`** @0x1A | LPI2C5 (codec bus) | ACKs everything, reads 0x00 — lets `WM8962_Init` complete; **not a codec model** |
| **`ssi-loopback`** | LPSPI1 | echoes MOSI→MISO (mirrors the SDO→SDI jumper) |
| GPIO **D13→D9 loop** | GPIO3.27→GPIO3.0 | attachInterrupt self-stimulation |
| **sd-card** ×2 | USDHC1/2 | from `-drive if=sd[,index=1]` |
| `sai1-tap` / `sai1-rxinject` chardevs | SAI1 | raw TDR capture / RX sample injection |
| (CLI) `usb-storage`, `usb-kbd/mouse`, `dev-midi` | OTG2 host bus | USB-host gates |

---

## ⚠️ Known QEMU-vs-silicon divergences (deliberate, probe-documented)

The silicon-truth loop's accumulated map of where a green QEMU run is **not**
sufficient — each entry is documented at its source and in the owning gate:

| Divergence | Consequence |
|---|---|
| **CCM readback ≠ gating**: LPCG/clock-root registers store/readback faithfully, but peripherals run regardless of the gate | A missing clock-ungate passes in QEMU → **circular pass**; HW is the arbiter (proven load-bearing by `cm4_spi_test`/`cm4_wire_test`) |
| **`ssi-loopback` echoes on `CR.MEN` alone** (ignores clock/pins) | same circular-pass shape for SPI |
| **`wm8962-stub` reads 0x0000**; real codec returns real registers (R15 → `0x6243` device ID) | `cm4_wire_test`'s `rdv` token is world-split by design |
| **GPIO PSR masks output bits** (`psr & ~gdir`) | verify outputs via **DR readback**, never PSR/`digitalRead` |
| **eDMA software-START drains the whole major loop**; silicon runs one minor loop | `single_minor` compat flag in the model |
| **FlexCAN loopback delivers synchronously**; silicon needs ~108µs + Rx-MB C/S read LOCKS the MB | tight read-polls starve real frames — poll with a delay gap |
| **LPI2C NDF is deferred to trail TDF** (matches silicon; immediate NDF false-passed a scan) | never judge ACK at TDF — judge at STOP/SDF |
| **Timing gates need `-icount`** (shift=2 typical) | else `delay()`/ptimer decouple (PIT, tone, SD-in-audio-ISR) |
| **SysTick time-base differs** (`-icount` vs real 400 MHz+) | characterisation tokens only (`cm4_intr_test`) |
| Boot-ROM stub executes IVT+DCD/XMCD but `clean_boot.scp` on HW **cannot** re-run the real ROM's pass | POR (SW4) is the only true full-ROM path on the board |
| **`imxrt_pxp` applies no CSC1 colour-space conversion** (plain RGB pixel copy) | Silicon runs every PS source pixel through CSC1's YUV→RGB matrix unless `CSC1_COEF0[BYPASS]` is explicitly set (it resets NOT-bypassed, with YUV coefficients loaded); QEMU passed regardless, since it never applies CSC1 math — the colour-mangling bug (Amendment 2 finding 1) was HW-only |

---

## 🟡 Stubbed (unimplemented-device, 8 entries)

`gpc-cpu-mode` (0x40C00000), `flexram` ctrl block (0x40028000 — banking itself
comes from the modelled IOMUXC GPR16/17), `flexspi2-ctrl`, `enet-qos`,
`adc-etc`, `spdif`, `lcdifv2`, `csi`. (List: `unimplemented_peripherals[]` in
`hw/arm/fsl-imxrt1170.c`.) These absorb accesses; `-d unimp` logs them.

## ⬜ Not modelled

- **ENET-QOS** (Synopsys DWC EQOS — no QEMU model exists).
- CM4-side **peripheral IRQ routing** (see the dual-core gap above).
- CAAM, PUF, MIPI-DSI/CSI, LCDIFv2 datapath.

---

## Verification harness

**Primary: the evkb gates** (`evkb/*/run_qemu*.sh`, gate-lib.sh pattern —
deterministic `token=HEX` transcripts; HW-verified gates check in
`transcript_qemu.txt` + `transcript_hw_evkb.txt`, byte-identical except
documented divergences). ~45 gates spanning: serial, GPIO/IRQ, timers/PWM/tone,
ADC/DAC, I2C/SPI (+DMA), I2S/audio graph (+SD WAV), eDMA, EEPROM, SD, SDRAM/
extmem, RTC, Ethernet (raw/lwIP/Arduino-API/FNET), USB device (CDC+HID trio) +
USB host (HID/MIDI/MSC), FlexCAN + ST7735 + Wire/SPI gates in their library
repos (`~/Development/{SPI,Wire,FlexCAN,Ethernet,...}/tests`), and the seven
dual-core gates listed above. License coverage: `evkb/tools/license-audit.sh`
(depfile link-manifest walk incl. CM4 sub-images, + copyleft sweep) must PASS.

**qemu2 regression set** (run whenever qemu2 is touched — see
`evkb/.claude/skills/cm4-bringup/references/silicon-truth-loop.md`):
`tests/functional/arm/test_imxrt1170.py` + `test_imxrt1062.py`, the most-affected
evkb gates, NXP SDK MCMGR hello_world + rpmsg pingpong, and
`scripts/checkpatch.pl` on the diff.

**Historical:** the June-era Zephyr v4.4 sample matrix (hello_world,
synchronization, philosophers, blinky, GPT alarm, eDMA/EEPROM/SPI-flash ztests)
was verified on the predecessor tree; those results predate qemu2 and have not
been re-run on it. The NXP SDK bare-metal examples above are the current
third-party-firmware cross-check.

---

## TODO / likely next

1. **CM4 peripheral IRQ routing** (`TYPE_SPLIT_IRQ` per line) — needed for
   interrupt-driven/DMA drivers on the CM4 (Phase 3 deferred list).
2. **Clock-gate fidelity** — enforce LPCG/pin-mux in peripheral/fixture paths
   to close the circular-pass divergences (machine-wide blast radius; HW is
   the arbiter meanwhile).
3. **ENET-QOS** — would need a new DWC EQOS model.
4. CCM_OBS computed frequencies are still placeholders (modelled peripherals
   don't consume them).
