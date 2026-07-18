# i.MX RT1170 (CM7) QEMU Model — Peripheral Status

Status of the QEMU machine model for the **Cortex-M7 core** of the NXP
i.MX RT1170, plus the bare-metal verification harness in this directory.

- **Machine:** `mimxrt1170-evk`  ·  **SoC type:** `fsl-imxrt1170`
- **CPU:** Cortex-M7 (FPU, 8-region MPU, caches present), NVIC = 218 external
  IRQs, 4 priority bits, no Security Extension (non-secure VTOR only).
- **QEMU tree:** `/Users/nicholasnewdigate/Development/qemu` (branch
  `imxrt1170-cm7-skeleton`)
- **Build:** configured with `--disable-sdl --disable-sdl-image` (headless;
  no libjxl/SDL dependency). Rebuild: `ninja -C build qemu-system-arm`.
- **Run:** `qemu-system-arm -M mimxrt1170-evk -nographic -kernel <fw.elf>`

Legend: ✅ modelled & verified · 🟡 stubbed (absorbs accesses, won't hang,
no behaviour) · ⬜ not modelled (would fault if accessed)

---

## Memory map (modelled regions)

| Region | Base | Size | Notes |
|---|---|---|---|
| ITCM | `0x0000_0000` | 512 KiB | RAM (reset vector here for ITCM-linked images) |
| DTCM | `0x2000_0000` | 512 KiB | RAM |
| OCRAM M4 / OCRAM1 / OCRAM2 / OCRAM M7 | `0x2020_0000` … `0x2036_0000` | 256K–640K | RAM |
| FlexSPI1 XIP | `0x3000_0000` | 16 MiB | RAM (firmware loaded here; reset VTOR = base) |
| SEMC SDRAM | `0x8000_0000` | 64 MiB | RAM (always-on; no SEMC init needed) |
| Peripherals | `0x4000_0000`–`0x40FF_FFFF` | — | per-device (see below) |
| PPB (NVIC/SysTick/SCS) | `0xE000_0000` | — | provided by the ARMv7-M container |

---

## ✅ Completed peripherals

| Peripheral | Instances | Base(s) | NVIC IRQ(s) | Modelled | Verified |
|---|---|---|---|---|---|
| **LPUART** | 12 | `0x4007C000`+ (1–10), `0x40C24000/0x40C28000` (11–12) | 20–31 | TX (instant), RX FIFO (depth 4), STAT/CTRL/BAUD/FIFO/WATER, combined IRQ | Console TX, RX echo over socket |
| **ANADIG** (PLL/OSC/PMU/MISC) | 1 | `0x40C84000` | — | Register file; PLL/OSC `*_STABLE` set; PFD stable toggles on `PLLn_UPDATE`; PMU `BIAS_CTRL2[WB_OK]` set; anatop-AI `AITOGGLE_DONE` flips on `AITOGGLE` write | Full Zephyr `clock_init` (PLL/OSC/PFD/body-bias/AI) completes |
| **DCDC** | 1 | `0x40CA8000` | — | Register file; `REG0[STS_DC_OK]` reads set | Zephyr/SDK core-voltage settle poll falls through |
| **GPT** | 6 | `0x400EC000`+ | 119–124 | Counter, prescaler, OCR1-3 compare, free-run/restart, rollover, IRQ; 24 MHz input clock | Polled compare + interrupt-driven NVIC ISR |
| **PIT** | 2 (×4 ch) | `0x400D8000`, `0x40CB0000` | 155, 156 | 4 down-counters, LDVAL/CVAL/TCTRL/TFLG, MCR[MDIS] gate, periodic IRQ (one combined line/instance); 24 MHz clock | MDIS gate, CVAL counting, periodic NVIC IRQ, stop-on-disable |
| **eDMA** | 32 ch | `0x40070000` | 0–15 (+err 16) | TCD engine; software-START (sync) + hardware-request (async via BH) transfers; DONE/INT, channels n & n+16 share a line | M2M copy, NVIC IRQ, LPUART1 TX-DMA |
| **DMAMUX** | 1 (×32) | `0x40074000` | — | CHCFG register file; routes each peripheral request source to the enabled channel(s) | LPUART TX request → channel routing |
| **GPIO** | 13 | `0x4012C000`+ (1–6), `0x40C5C000`+ (7–12), `0x40CA0000` (13) | 100–109, 61–62, 93 | DR/GDIR/PSR/ICR/IMR/ISR/EDGE_SEL + atomic DR_SET/CLEAR/TOGGLE; edge/level IRQ; 2 combined lines/bank | Output set/clear/toggle readback; input level IRQ → NVIC ISR |
| **LPI2C** (master) | 6 | `0x40104000`+ (1–4), `0x40C34000/0x40C38000` (5–6) | 32–37 | MTDR command FIFO (START/TX/RX/STOP), MRDR RX FIFO, MSR, NAK, combined DMA request (MDER); QEMU I2C bus | NAK; EEPROM round-trip; DMA request -> eDMA |
| **LPSPI** (master) | 6 | `0x40114000`+ (1–4), `0x40C2C000/0x40C30000` (5–6) | 38–43 | TDR/RDR full-duplex, TCR frame size/PCS/CONT, SR, CS gpio outputs, TX/RX DMA request (DER); QEMU SSI bus | JEDEC ID read; DMA request -> eDMA |
| **CCM** (+ CCM_OBS) | 1 | `0x40CC0000`, `0x40150000` | — | Register file (clock-root CONTROL RMW); OSCPLL/LPCG `DIRECT[ON]`→`STATUS0[ON]` mirror; CCM_OBS `FREQUENCY_CURRENT` non-zero | Clock-root MUX/DIV read-back, LPCG gate wait loop, `CLOCK_GetFreqFromObs` |
| **IOMUXC** (+GPR/LPSR/SNVS) | 6 windows | `0x400E4000`, `0x400E8000`, `0x40C08000`, `0x40C0C000`, `0x40C94000`, `0x40C98000` | — | Register file per window (pin mux write-only; GPR RMW); maps the LPSR_GPR/SNVS/SNVS_GPR windows that were unmapped | GPR RMW, pad-mux read-back, SNVS window access |
| **SRC** | 1 | `0x40C04000` | — | Register file; SRSR reports power-on reset (W1C); SBMR read-only; GPR RMW | SRSR POR + W1C, GPR RMW, SBMR read-only |
| **SNVS** (RTC+GPR) | 1 | `0x40C90000` | 66 (not asserted) | HP + LP secure RTC (47-bit, 32.768 kHz, advances in virtual time); 4 battery-backed LP GPRs (aliased); HPSR/LPSR W1C | LPGPR alias read-back, HP RTC counter advances |
| **SAI** (I2S) | 4 | `0x40404000`+ (1-3), `0x40C40000` (4) | 76, 77, 78, 80 | Register file; TX FIFO always drained so TCSR[TE]&[FRDE] drives the TX DMA request; SR/FR self-clear, FWF/FRF status set when enabled; combined IRQ/instance; TX streams to a host audio voice | TX request -> DMAMUX slot 55 -> eDMA copy (saitest); 750 Hz sine -> WAV (rt1170-sai) |
| **WDOG** | 2 | `0x40030000` (1), `0x40034000` (2) | 112, 65 | Reuses QEMU `imx2.wdt`: WCR/WSR/WRSR, WDE enable, WT timeout, 0x5555/0xAAAA service; timeout fires the watchdog action (reset) | unserviced -> SoC reset loop; serviced -> stays up (wdogtest) |
| **RTWDOG** (WDOG32) | 2 | `0x40038000` (3, CM7), `0x40C10000` (4, CM4) | 113 (RTWDOG3; RTWDOG4 has no CM7 line) | New `imxrt.rtwdog` model: CS/CNT/TOVAL/WIN; unlock key `0xD928C520` (or halves `0xC520`/`0xD928`), refresh key `0xB480A602` (or `0xA602`/`0xB480`); ULK/RCS poll bits set so the MCUXpresso driver + SystemInit complete; ptimer timeout fires the watchdog action (reset). Armed only once firmware writes CS/refreshes, so untouched images aren't reset | configure ~1 s + no refresh -> SoC reset; refresh forever -> stays up (rtwdogtest) |
| **OCOTP** (eFUSE) | 1 | `0x40CAC000` | 115/116 (not asserted; driver polls) | New `imxrt.ocotp` model: 144 fuse words; controller read (CTRL.ADDR + READ_CTRL.READ_FUSE -> READ_FUSE_DATA) and direct FUSE[] shadow array; program via 0x3E77 write-unlock key + DATA with OTP OR-only semantics; CTRL/OUT_STATUS SET/CLR/TOG aliases; BUSY always ready, RELOAD_SHADOWS self-clears; wrong-key/range -> CTRL.ERROR; WORDLOCK per-word lock. Fuses blank at cold start, survive warm reset | program + read-back via both paths, OR semantics, key error, RELOAD, WORDLOCK (ocotptest 13/13) |
| **EWM** | 1 | `0x4002C000` | 114 | New `imxrt.ewm` model: CTRL/SERV/CMPL/CMPH/CLKCTRL/CLKPRESCALER (8-bit); CTRL write-once, EWMEN locks config + starts the prescaled up-counter; service sequence `0xB4`/`0x2C` refreshes; counter past CMPH fires the watchdog action (reset); INTEN routes the IRQ. Counter runs only after EWMEN; CMPL lower window stored but not enforced | configure ~1 s + no refresh -> SoC reset; refresh forever -> stays up (ewmtest) |
| **Key Manager** (CSR) | 1 | `0x40C80000` | — (64 is the PUF line) | New `imxrt.key-manager` model: MASTER/OTFAD1/OTFAD2/IEE/PUF key-control + 5 slot-control registers. Routing SELECT bits stored; security locks enforced — write-once LOCK freezes MASTER/OTFAD/PUF, slot LOCK_LIST freezes the white-list (TZ bits stay writable), LOCK_CONTROL freezes the whole slot; IEE RELOAD self-clears | SELECT read-back, per-register write-once lock, RELOAD self-clear, slot LOCK_LIST/LOCK_CONTROL scopes (kmtest 11/11) |
| **LPADC** | 2 | `0x40050000` (1), `0x40054000` (2) | 88, 89 | New `imxrt.lpadc` model: CTRL/STAT/IE/CFG, CMDL/CMDH command buffers (15), TCTRL triggers (8), SWTRIG, FCTRL, RESFIFO. SWTRIG launches the command TCTRL.TCMD points at; converts synchronously and pushes a RESFIFO entry (VALID + CMDSRC + TSRC + 12-bit synthetic per-channel sample); RESFIFO read pops; STAT.RDY/IRQ track FCOUNT vs FWMARK; RSTFIFO flush, FOF W1C. Averaging/loop/HW-trigger/compare not modelled | trigger->command->FIFO, channel routing, CMDSRC/TSRC, FIFO order, RSTFIFO, watermark RDY (adctest 15/15) |
| **FlexCAN** | 3 | `0x400C4000` (1), `0x400C8000` (2), `0x40C3C000` (3) | 44, 46, 48 | New `imxrt.flexcan` model: MCR/CTRL1/IMASK/IFLAG + 64 classic 8-byte message buffers. Freeze handshake (FRZ/HALT->FRZACK), self-clearing SOFTRST, MDIS/LPMACK; CS CODE=TX_DATA in normal mode transmits; internal loopback (CTRL1.LPB) matches Rx MBs (CODE=EMPTY) via global/per-MB mask, delivers (CODE->FULL) and sets IFLAG; ORed MB IRQ. Self-contained (loopback only; no QEMU CAN-bus attach) | soft-reset/freeze handshake, loopback TX->RX with code/DLC/ID/data, TX+RX IFLAG, W1C (cantest 11/11) |
| **USDHC** (SD/eMMC) | 2 | `0x40418000` (1), `0x4041C000` (2) | 133, 134 | **Reuses QEMU `imx-usdhc`** (SDHCI-compatible, as on imx6/imx7), capareg 0x057834b4. Board attaches a real `sd-card` from `-drive if=sd` onto the controller's sd-bus. Full standard SD data path works; HS200/HS400 DLL/strobe tuning regs not modelled (read 0) | reset, card-detect, clock-stable, full SD init (CMD0/8/ACMD41/2/3/7/16) + CMD17 single-block PIO read verified vs image pattern (sdtest 15/15) |
| **ENET** (10/100) | 1 | `0x40424000` | 137 | **Reuses QEMU `imx.enet`** (i.MX FEC, as on imx6/imx7/8). phy-num=2 (emulated LAN9118 PHY); two output lines (MAC, MAC\|1588) OR-ed onto the single ENET IRQ; NIC backend via `qemu_configure_nic_device` (optional `-nic user,model=imx.enet`). Reset/MII/MAC/legacy-BD TX work; RCR.LOOP not implemented so no self-contained RX loopback | reset, PHY ID over MDIO (0x0007/0xc0d1), BMCR, EIR.MII set+W1C, PALR/PAUR, TX descriptor consumed + EIR.TXF (enettest 9/9) |
| **ENET_1G** (gigabit) | 1 | `0x40420000` | 141, 142 | Second **`imx.enet`** instance with its own PHY (phy-num=1) and 3 AVB rings (tx-ring-num=3). MAC and 1588-timer lines wired directly to their own NVIC vectors (141/142). Exercises the enhanced (1588) 32-byte-descriptor multi-queue TX datapath -- the gigabit-distinct feature. RGMII/1000M link config and ENET-QOS are not modelled; QEMU places the ring-1/2 registers at model-specific offsets (not the RT1170 0x604/0x608) | PHY ID over MDIO, PALR/PAUR, enhanced-descriptor TX on rings 0 and 1 (EIR.TXF/TXF1 + descriptor write-back), NVIC 141 assert/deassert (enet1gtest 13/13) |
| **USB OTG** (1/2) | 2 + 2 PHY | OTG `0x40430000`/`0x4042C000`, PHY `0x40434000`/`0x40438000` | 136, 135 (PHY 90/91 not asserted) | **Reuses QEMU `usb-chipidea`** (ChipIdea EHCI host, caps@0x100/op@0x140/1 port) + **`imx.usbphy`**. PHY has no IRQ line; USBNC (+0x200) and RT1170-only PHY tuning regs not modelled (read 0). Host-mode EHCI; device-controller mode not emulated | EHCI caps, ChipIdea DCIVERSION/DCCPARAMS, USBCMD HCRESET self-clear, HCHALTED, PORTSC; PHY VERSION/CTRL/PWD power-up (usbtest 24/24); `-device usb-storage,bus=usb-bus.0` -> PORTSC connect (0x1003) |
| **FlexIO** | 2 | `0x400AC000` (1), `0x400B0000` (2) | 110, 111 | New `imxrt.flexio` register model: VERID/PARAM (8 shifters/8 timers), CTRL FLEXEN/SWRST (reset clears all but CTRL), SHIFTCTL/CFG/BUF + TIMCTL/CFG/CMP arrays, SHIFTBUF bit/byte/half-word-swap alias views, status-driven IRQ. SHIFTSTAT reads all-ready when FLEXEN=1 so blocking UART/SPI drivers don't hang; shifter/timer serial datapath NOT emulated | VERID/PARAM, SWRST clears config, FLEXEN, SHIFTSTAT ready, config readback, SHIFTBUF swaps, IRQ assert/deassert via NVIC pending (flexiotest 17/17) |
| **eLCDIF** (display) | 1 | `0x40804000` | 54 | **Reuses QEMU `imx6ul_lcdif`** (MXS/i.MX6 eLCDIF). Scans a framebuffer out of system memory to a QEMU console surface; CTRL.RUN + WORD_LENGTH (RGB565 / XRGB8888), TRANSFER_COUNT (HxV), CUR_BUF/NEXT_BUF. Renders under `-display none` (captured via QMP `screendump`). LCDIFv2/MIPI-DSI/CSI stay stubbed | 64x64 RGB565 four-quadrant pattern -> eLCDIF scanout -> `screendump` PPM, quadrant colours verified (lcdiftest + run_lcdif.py, 4/4) |
| **PXP** (2D blitter) | 1 | `0x40814000` | 57 | New `imxrt.pxp` register model: memory-to-memory PS -> OUT copy. CTRL.ENABLE runs a synchronous blit honouring the OUT rectangle (OUT_PS_ULC/LRC), PS/OUT pitches and CTRL ROTATE(180)/HFLIP/VFLIP, then sets STAT.IRQ (driver-polled completion) and self-clears ENABLE; CTRL.IRQ_ENABLE raises the interrupt. SET/CLR/TOG aliases on CTRL/STAT/OUT_CTRL/PS_CTRL. Scaling, CSC, alpha blend and 90/270 rotation NOT modelled | identity / rotate-180 / hflip / vflip / dest-offset copies of a position-encoding RGB565 surface, plus STAT.IRQ, ENABLE self-clear and NVIC assert/deassert (pxptest 13/13) |

**Per-device source:** `hw/char/imxrt_lpuart.c`, `hw/misc/imxrt_anadig.c`,
`hw/misc/imxrt_ccm.c`, `hw/misc/imxrt_iomuxc.c`, `hw/misc/imxrt_src.c`, `hw/misc/imxrt_snvs.c`, `hw/timer/imxrt_gpt.c`, `hw/timer/imxrt_pit.c`, `hw/dma/imxrt_edma.c`,
`hw/dma/imxrt_dmamux.c`,
`hw/gpio/imxrt_gpio.c`, `hw/i2c/imxrt_lpi2c.c`, `hw/ssi/imxrt_lpspi.c`,
`hw/misc/imxrt_dcdc.c`, `hw/audio/imxrt_sai.c`, `hw/watchdog/imxrt_rtwdog.c`,
`hw/nvram/imxrt_ocotp.c`, `hw/watchdog/imxrt_ewm.c`,
`hw/misc/imxrt_key_manager.c`, `hw/adc/imxrt_lpadc.c`,
`hw/net/can/imxrt_flexcan.c`, `hw/misc/imxrt_flexio.c`, `hw/dma/imxrt_pxp.c`
(+ headers in `include/hw/...`).  **USDHC**, **ENET** and **USB** add no new
device file — they reuse QEMU's `hw/sd/sdhci.c` (`TYPE_IMX_USDHC`),
`hw/net/imx_fec.c` (`TYPE_IMX_ENET`), and `hw/usb/chipidea.c` +
`hw/usb/imx-usb-phy.c` (`TYPE_CHIPIDEA` / `TYPE_IMX_USBPHY`), all wired in
`hw/arm/fsl-imxrt1170.c` (the SD card is attached in `hw/arm/mimxrt1170-evk.c`).

### Known limitations of completed devices
- **LPUART:** baud rate ignored (no real timing); slave/9-bit/modem modes not
  modelled; interrupt logic present but only RX/TX-ready paths exercised; TX/RX
  DMA request lines are wired (TX verified via eDMA).
- **GPT:** input-capture (ICR1/2) not modelled; single fixed 24 MHz clock
  (real CCM clock-root selection not wired).
- **GPIO:** GPIO7–12 have no dedicated NVIC line on HW (left unconnected);
  GPIO13's two halves are OR'd onto its single combined line.
- **LPI2C / LPSPI:** master only (no slave); TX/RX DMA request lines wired to
  the eDMA (verified); interrupt mode has the plumbing but was polling-verified.
- **CLI attach note:** `-device ...,bus=/imxrt.lpi2c/i2c` (or `/imxrt.lpspi/spi`)
  binds to the *first* instance in QEMU's list = the *last-realized* one, i.e.
  **LPI2C6 / LPSPI6**. The i2c/spi tests target those instances.

---

## 🟡 Stubbed peripherals (unimplemented-device)

These absorb reads/writes (reads return 0) and are logged with `-d unimp`.
They will **not hang** the boot, but provide no behaviour. Listed by area:

| Area | Blocks (base) |
|---|---|
| Clocks / power / reset | `gpc-cpu-mode` (`0x40C00000`), `pgmc` (partial) — (CCM, SRC, ANADIG, DCDC are modelled) |
| Watchdogs | `ewm` (`0x4002C000`) — (WDOG1/2 via imx2.wdt, RTWDOG3/4 via imxrt.rtwdog are modelled) |
| External memory ctrls | `flexspi1/2-ctrl` (`0x400CC000/0x400D0000`), `semc-ctrl` (`0x400D4000`), `flexram` (`0x40028000`) |
| Storage / net | `enet-qos` (`0x4043C000`) — (USDHC1/2, ENET 10/100, ENET_1G, and USB OTG1/2 + PHYs are modelled) |
| Connectivity | — (FlexCAN1/2/3 and FlexIO1/2 are modelled) |
| Security / OTP / RTC | caam, puf — (Key Manager, OCOTP, SNVS and EWM are modelled) |
| Analog / audio | `adc-etc` (`0x40048000`), `dac` (`0x40064000`), `spdif` (`0x40400000`) — (LPADC1/2 and SAI1-4 are modelled) |
| Display / camera | `lcdifv2` (`0x40808000`), `csi` (`0x40800000`), `mipi-dsi/csi` — (eLCDIF via imx6ul_lcdif and PXP are modelled) |

(Full list: `unimplemented_peripherals[]` in `hw/arm/fsl-imxrt1170.c`, 36 entries.)

---

## 🦓 Zephyr RTOS verification

The model boots **real Zephyr v4.4** (built with the ARM_10 GCC `gnuarmemb`
toolchain; see the build memo). Booting through the full NXP MCUXpresso driver
stack independently verifies the bring-up peripherals against real firmware —
not just the bare-metal harness. Build images with
`-- -DCONFIG_NXP_IMXRT_BOOT_HEADER=n` (vectors at `0x30000000`); run headless and
capture LPUART1 with `-serial file:`. Helper: `zephyr-samples.sh`.

| Zephyr sample | Result | Verifies |
|---|---|---|
| `hello_world` | `Hello World! mimxrt1170_evk@B/...` | reset, full `clock_init` (DCDC, ANADIG PLL/OSC/PMU/AI, CCM), IOMUXC, LPUART1 console, SDRAM, NVIC, SysTick |
| `synchronization` | thread_a/thread_b ping-pong | kernel scheduler, threads, semaphores, SysTick tick |
| `philosophers` | live dining-philosophers demo | preemptible+cooperative threads, dynamic mutexes, sleeping/timing |
| `basic/blinky` | `LED state: ON/OFF`; GPIO9 DR bit3 toggles (read via monitor `xp/1xw 0x40c64000`) | **GPIO9** output path |
| `drivers/counter/alarm` | `!!! Alarm !!!` at 2 s / 4 s / 8 s | **GPT2** compare → IRQ 120 → NVIC → counter ISR (needs `-DCONFIG_COUNTER_MCUX_GPT=y` + a 1-line `MCUX_GPT` branch in the sample) |
| `tests/drivers/dma/loop_transfer` | 2/3 ztests PASS | **eDMA** M2M via `mcux_edma` driver (DMAMUX always-on + SERQ trigger). suspend_resume fails — synchronous (non-time-paced) transfers can't be halted mid-flight |
| `tests/drivers/eeprom/api` (+at24 on LPI2C6) | 7/7 ztests PASS | **LPI2C6** end-to-end: write/read round-trip + page-crossing multi-byte through the model → QEMU i2c bus → at24 EEPROM |
| `samples/drivers/spi_flash` (+is25wp256 on LPSPI6) | erase + write + read-back PASS | **LPSPI6** end-to-end: erase/write/read through nxp-lpspi + spi_nor → QEMU SSI → is25wp256 flash |

**LPSPI / Zephyr (fixed, commit `ca15c5ac97`):** the nxp-lpspi driver ends a
transaction by writing `TCR` with `CONT=0` as a command-only FIFO word to
release PCS. The model now treats `TCR` as a queued command word (correct PCS
timing) and connects the command-line flash's chip-select lazily on the first
frame (LPSPI reset runs before the `-device` flash is attached). `spi_flash`
erase/write/read passes; bare-metal `spitest` (is25wp128) 3/3 and `dmaperi`
2/2 stay green.

eDMA needed commit `c88368b7eb` (DMAMUX A_ON / software-trigger); LPI2C needed
the RX-FIFO pacing fix (3-bit MFSR.RXCOUNT). To attach bus devices:
`-device at24c-eeprom,bus=/imxrt.lpi2c/i2c,address=0x50,rom-size=256` (binds
LPI2C6) or an SSI flash (`is25lp064`) on `/imxrt.lpspi/spi` (LPSPI6).

After boot the `-d unimp` trace is ~23 benign one-shot reads (wdog disable, GPC,
OCOTP fuse). The DCDC/ANADIG fixes that unblocked this are commit `49528bfee2`.

---

## ⬜ Not represented at all

- **Cortex-M4 (CM4) core** — only the CM7 is modelled; the CM4 and its
  message unit (MU), inter-core, and CM4-only peripherals are absent.
- **Boot ROM / FlexSPI boot flow** — skipped; firmware is loaded directly and
  the reset vector points at the FlexSPI XIP base.
- Any peripheral whose base is neither modelled nor in the stub list (a guest
  access there will hit the default unassigned-memory handler).

---

## TODO — suggested priority order

1. ~~**Boot a Zephyr `hello_world`**~~ ✅ **DONE** — boots and runs (see the
   Zephyr verification section). Build with `-DCONFIG_NXP_IMXRT_BOOT_HEADER=n`
   so vectors land at `0x30000000`. Required the DCDC + ANADIG fixes (`49528bfee2`).
   Next Zephyr targets needing more plumbing: RTC (no SNVS RTC driver/node yet),
   LPI2C/LPSPI (attach QEMU bus devices + DT), eDMA (UART async or a DMA test).
2. **CCM clock-tree frequencies** — CCM is now modelled (register file + gate
   mirroring + CCM_OBS), but `CLOCK_GetFreqFromObs()` returns a *fixed 24 MHz
   placeholder*. To report accurate per-clock frequencies, map the CCM_OBS
   SELECT signal index to a frequency computed from the ANADIG PLL config and
   the clock-root MUX/DIV. Not a hang risk; affects only computed baud/timer
   rates (which the modelled peripherals currently ignore).
3. **DCDC / GPC** — likely fine as stubs; confirm against a real boot trace.
   (IOMUXC and SRC are now modelled.)
4. **More DMA-driven peripherals** — LPUART, LPSPI and LPI2C DMA request
   lines are wired (peripheral -> DMAMUX -> eDMA, async via a bottom half).
   SAI/FlexIO/ADC DMA requests could be added similarly.
5. **ENET-QOS** (TSN Ethernet) — still stubbed: a Synopsys DWC EQOS that
   QEMU has no model for.  ENET-1G (gigabit) is now modelled as a second
   `imx.enet` instance.  Everything else commonly used is modelled:
   USDHC1/2, ENET 10/100, ENET_1G, USB OTG1/2 + PHYs (reusing QEMU's
   imx-usdhc / imx.enet / chipidea + imx.usbphy), FlexCAN1/2/3, FlexIO1/2,
   LPADC1/2, and
   all the small control blocks (WDOG1/2, RTWDOG3/4, EWM, OCOTP, Key Manager,
   SNVS).
7. **Interrupt-mode coverage** for LPUART/LPI2C/LPSPI (currently polling-verified).
8. **Audio (SPDIF/PDM)** and remaining **display/camera (LCDIFv2/MIPI/CSI)** —
   lowest priority for headless bring-up (SAI, eLCDIF and PXP are modelled).

---

## Verification harness (this directory)

Built with the ARM GCC toolchain via `./build.sh` (uses
`/Applications/ARM_10/bin/arm-none-eabi-gcc`):

| Artifact | What it does | Run command extras |
|---|---|---|
| `selftest.elf` | 19 self-checks across LPUART/GPIO/GPT/ANADIG/IOMUXC | — |
| `i2ctest.elf` | LPI2C NAK + EEPROM round-trip | `-device at24c-eeprom,bus=/imxrt.lpi2c/i2c,address=0x50,rom-size=256,address-size=1` |
| `spitest.elf` | LPSPI JEDEC-ID read | `-device is25wp128,bus=/imxrt.lpspi/spi` |
| `clktest.elf` | CCM clock-root RMW, LPCG gate wait, `CLOCK_GetFreqFromObs` | — |
| `pittest.elf` | PIT module gate, counter, periodic NVIC IRQ | — |
| `dmatest.elf` | eDMA M2M copy + major-loop NVIC IRQ | — |
| `dmauart.elf` | LPUART1 TX driven by eDMA (peripheral request path) | — |
| `dmaperi.elf` | LPSPI1 / LPI2C1 DMA requests drive eDMA | — |
| `saitest.elf` | SAI1 (I2S) TX DMA request drives eDMA | — |
| `wdogtest.elf` / `wdogkick.elf` | WDOG1 timeout resets SoC / service keeps it alive | — |
| `rtwdogtest.elf` / `rtwdogkick.elf` | RTWDOG3 (WDOG32) timeout resets SoC / refresh keeps it alive | — |
| `ocotptest.elf` | OCOTP eFUSE read/program/lock paths (13/13) | — |
| `ewmtest.elf` / `ewmkick.elf` | EWM timeout resets SoC / refresh keeps it alive | — |
| `kmtest.elf` | Key Manager routing + security lock semantics (11/11) | — |
| `adctest.elf` | LPADC trigger/command/FIFO conversion paths (15/15) | — |
| `cantest.elf` | FlexCAN loopback TX->RX mailbox path (11/11) | — |
| `sdtest.elf` | USDHC SD init + block-0 read (15/15) | `-drive file=card.img,if=sd,format=raw` |
| `enettest.elf` | ENET PHY-ID/MAC/TX-descriptor path (9/9) | optional `-nic user,model=imx.enet` |
| `enet1gtest.elf` | ENET_1G PHY/MAC + enhanced multi-ring TX (rings 0/1) + NVIC 141 (13/13) | — |
| `usbtest.elf` | USB EHCI caps/reset + PHY power-up (24/24) | optional `-drive id=u,file=card.img,if=none -device usb-storage,bus=usb-bus.0,drive=u` |
| `flexiotest.elf` | FlexIO regs/SWRST/SHIFTSTAT/swaps/IRQ (17/17) | — |
| `lcdiftest.elf` | eLCDIF framebuffer scanout, colours via screendump (4/4) | run via `python3 run_lcdif.py` (headless QMP screendump) |
| `pxptest.elf` | PXP blit: identity/180/hflip/vflip/dest-offset + STAT/ENABLE/NVIC (13/13) | — |
| `fw.elf` | LPUART/GPIO/GPT demo banner | — |

Example:
```sh
cd /Users/nicholasnewdigate/Development/qemu
./build/qemu-system-arm -M mimxrt1170-evk -nographic -kernel \
    /Users/nicholasnewdigate/Development/rt1170-fw/selftest.elf
```

Source: `startup.c` (vector table + reset), `link.ld` (XIP at `0x30000000`),
`main.c` / `selftest.c` / `i2ctest.c` / `spitest.c`.

---

## Commit history (branch `imxrt1170-cm7-skeleton`)

| Commit | Summary |
|---|---|
| `c978f20` | CM7 SoC + `mimxrt1170-evk` board skeleton |
| `7c437ce` | LPUART ×12 |
| `18caa72` | ANADIG (clock-init unblock) |
| `20a6db8` | GPT ×6 |
| `25bf158` | GPIO ×13 |
| `5361983` | LPI2C ×6 |
| `dcb8879` | LPSPI ×6 |
| `5f1f2b6` | CCM + CCM_OBS clock controller |
| `924e0fe` | IOMUXC pin mux / GPR (6 windows) |
| `8d6a906` | PIT ×2 (4 channels each) |
| `b8eaa90` | eDMA (32 ch) + DMAMUX |
| `c341482` | SRC system reset controller |
| `aaef194` | SNVS (RTC + GPR) |
| `e3de84e` | peripheral DMA request wiring (LPUART -> DMAMUX -> eDMA) |
| `f568a13` | LPSPI + LPI2C DMA request wiring |
