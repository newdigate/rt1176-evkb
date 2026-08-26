# M.2 Bluetooth hardware rework — MIMXRT1170-EVKB and MIMXRT1060-EVKB

**Purpose.** Both NXP EVK boards multiplex the M.2 card's Bluetooth sideband
signals (UART, PDn, reset) onto pins they also use for other peripherals, and
ship several of the required links **DNP** (do-not-populate). This document
records, for each board, exactly which resistors to add or remove, *why*, what
that does to the rest of the board, and which items were verified on silicon.

**Scope.** This is the **Bluetooth** rework. The Wi-Fi (SDIO) side of the M.2
card works on both boards without any of this — it is a separate bus. Every
populate state below is read from the boards' own design files
(`MIMXRT1170-EVKB-DESIGNFILES_RevC3`, `MIMXRT1060-EVKB-DESIGNFILE-RevB1`); every
"verified" claim comes from `examples/networking/m2_hci_probe/transcript_hw_evkb.txt`.

> **★ Read this first.** None of the rework below makes the M2-MAYA-W161's
> Bluetooth **controller** respond. On the RT1170, with the rework done, the
> firmware downloads completely and the controller is still silent — that is a
> module-level issue (u-blox case **CA-276115**), not a board one. The rework
> is what gets you a *readable, power-cyclable* BT link; it is necessary, not
> sufficient. See "What the rework does not fix" at the end.

---

## TL;DR — what to do on each board

| Board | Add (0 Ω) | Remove | Minimum for a **readable** BT link |
|---|---|---|---|
| **MIMXRT1170-EVKB** | `R404`, `R1901` | — | those two (both DNP from factory) |
| **MIMXRT1170-EVKB** *(full, for HW flow control)* | `+ R1902` | `R183`, `R1816` | all five — NXP's official list |
| **MIMXRT1060-EVKB** | `R345`, `R96` | — | those two (both DNP from factory) |

The two "minimum" reworks are what this project actually fitted and verified.
The removals matter only if you enable **hardware flow control** (RT1170) or
drive the shared reset pin for **more than a Bluetooth-only bench** (RT1060) —
see each board's section.

---

## Why any rework is needed

The M.2 Key-E connector carries the BT HCI UART, a power-down line (`PDn`), an
independent BT reset, and wake signals. On a purpose-built board these would go
to dedicated MCU pins. On a general-purpose EVK, pins are scarce, so NXP:

1. **Multiplex** the M.2 sidebands with Arduino-header pins, an on-board SPI
   flash, the Ethernet PHY, and audio — anything the M.2 does not use by
   default.
2. **DNP the M.2-side links** so the shared pins default to their *other*
   function, and you opt into M.2 Bluetooth by moving the resistors.

The rework therefore always has two halves: **connect** the M.2 link (fit a DNP
resistor) and, where the shared peripheral would fight it, **disconnect** the
other tenant (remove a fitted resistor).

---

## MIMXRT1170-EVKB (Rev C3)

BT HCI UART = **LPUART2**. NXP's own guide
(`middleware/edgefast_bluetooth/docs/.../MIMXRT1170EVKB_hwrework.md`) says:

> **Remove `R183` and `R1816`. Solder 0 Ω to `R404`, `R1901`, and `R1902`.**

### The five items

| Ref | Action | Net / path | Populate | What it does | Peripheral affected |
|---|---|---|---|---|---|
| **`R404`** | **fit** | `GPIO_AD_31` → PDn chain → J54.56 (`W_DISABLE1#`) | DNP | Lets the MCU power-cycle the radio. **Without it the module is held in power-down and never boots — FATAL.** | `GPIO_AD_31` is also **Arduino D12 / LPSPI1 MISO** — driving PDn disturbs header SPI |
| **`R1901`** | **fit** | `U355` → `BT_UART_RXD` → J54.22 (card TX) | DNP | Makes the card's UART TX **readable**. Without it BT is transmit-only — no HCI reply can ever be seen. | `BT_UART_RXD` also reaches **Arduino header J9.2** via `R2` |
| **`R1902`** | fit | card RTS output → `U19.B6` (MCU **CTS input**) | DNP | Lets the host read the card's flow-control output. Only matters with HW flow control. | shares the MCU CTS net with the PHY — see `R1816` |
| **`R1816`** | remove | `BT_UART_CTS` (`U19.B6`) ── `RGMII1_PHY_INTB` | **fitted** | Frees the MCU CTS input from the Ethernet PHY interrupt so `R1902` is usable. | the **1 Gb PHY** interrupt (`U10`, RTL8211FDI-CG) — removing it disables that PHY's IRQ line, **not** Ethernet data. ★ This repo's Ethernet examples are unaffected: they run on the **10/100** RJ45 (a different MAC/PHY) and **poll** link state (`enet_phy_link_up()`), not the IRQ |
| **`R183`** | remove | `GPIO_AD_31` ── `U27.2` (SO of `MX25L4006` SPI NOR) | **fitted** | Removes a third driver from the PDn line — a flash **data output** on the pin you power-cycle the radio with. | an **auxiliary LPSPI1 SPI-NOR** (`U27`, 4 Mbit MX25L4006 on `GPIO_AD_28/29/30/31`) loses its MISO — **not the boot flash**, and rarely used, so cost ≈ zero |

### The flow-control trap — `R1816` / `R1866` / `R1902`

On this board **LPUART2's RTS and CTS pads are the Ethernet PHY's control
lines**, and both connecting resistors are **fitted**:

| Ref | Path | Consequence if used as UART flow control |
|---|---|---|
| `R1866` | `BT_UART_RTS` (`U19.A5`) ── `ETHPHY_RST_B` ── `U10.12` | **Asserting RTS holds the gigabit PHY in reset** |
| `R1816` | `BT_UART_CTS` (`U19.B6`) ── `RGMII1_PHY_INTB` ── `U10.31` | UART CTS and the PHY interrupt fight over one net |

`U10` is the PHY behind `enet_test`, `ethernet_test`, `lwip_test`,
`native_ethernet_test`. **Do not enable hardware flow control on LPUART2 as the
board ships** — RTS is the PHY reset line, not a UART signal.

★ Note `R1866` is **not** in NXP's remove list: even after the full five-item
rework, the host's RTS *output* still routes to the PHY reset, so there is no
clean host-RX back-pressure path. This is why 3 Mbaud downloads corrupt on this
board (measured — see the transcript's chatty-loader section): the fast phase
wants flow control the wiring cannot provide. **Staying at 115200 avoids it**
and completes the whole download.

### Arduino-header collisions (lower severity, still real)

`R2`/`R3`/`R8` are fitted, so the LPUART2 pads reach both J54 **and** the
Arduino header:

| BT net | header pin | MCU pad |
|---|---|---|
| `BT_UART_RXD` (card TX) | J9.2 | `GPIO_DISP_B2_11` (ball A6) |
| `BT_UART_TXD` (card RX) | J9.4 | `GPIO_DISP_B2_10` (ball D9) |
| `BT_UART_CTS` (card RTS) | J9.6 | `GPIO_DISP_B2_12` (ball B6) |

So the Arduino **D0–D2 header block collides with Bluetooth**, and (via `R404`)
**D12/MISO collides with PDn**. Don't stack a shield using those pins while driving the M.2
BT link. Also independent of this: header pin **A5 (`GPIO_AD_08`) doubles as
`USB_OTG2_ID`** — an OTG adapter in the second USB port clamps A5 and kills
header I²C.

### What was actually fitted and verified

`R404` and `R1901` bridged by hand (2026-08-18). Verified: PDn power-cycles the
module (five sessions of clean ROM greetings), and the card's TX is readable
(131,840-byte firmware download received in both directions).

★ **NXP support (2026-08-26) asked for the full rework — remove `R183`/`R1816`,
fit `R1902` — before proceeding.** The consequences are small and were checked:
`R183` removal costs read access to the spare LPSPI1 flash `U27` (not the boot
device, rarely used); `R1816` removal disables the **1 Gb PHY interrupt only**
(this repo's Ethernet examples run on the 10/100 PHY and poll, so they are
unaffected). Both are reversible 0402 removals.

★ **Caveat NXP's list does not mention:** `R1866` is **not** removed, so the
host RTS *output* stays on the 1 Gb PHY reset. After the rework, enabling
RTS/CTS flow control holds that PHY in reset — so **1 Gb Ethernet and
BT-with-flow-control are mutually exclusive** on this board. Fine for BT
bring-up. Neither the flow-control pair nor `R183` can make the controller
*transmit* (that is the CA-276115 / NXP-forum question), but completing the
documented rework unblocks the vendor and gives the download real flow control
for the first time.

---

## MIMXRT1060-EVKB (Rev B1)

BT HCI UART = **LPUART3** (`GPIO_AD_B1_06`/`B1_07`, Teensy-core `Serial2`).
Unlike the 1170 this board has **no published NXP BT rework guide** for the
non-C EVKB, so the two required links were found from the netlist and confirmed
on silicon.

### The two required items — both DNP from factory

| Ref | Action | Net / path | Populate | What it does | Peripheral affected |
|---|---|---|---|---|---|
| **`R345`** | **fit** | `GPIO_AD_B1_03` → `WL_RST#` → `R89`/`R82` → J8.56 (**PDn**) | DNP | Connects the MCU to the module's power-down/reset. Without it the module can never be power-cycled and never boots — the once-per-power-up ROM greeting can never be caught. | `GPIO_AD_B1_03` is shared 3-way — see caution below |
| **`R96`** | **fit** | J8.22 (card TX) → `R18` → `U10` shifter → `R96` → `R200` → `GPIO_AD_B1_07` (LPUART3_RX) | DNP | Completes the **card→MCU** receive leg. `R200` is fitted but only joins the MCU to an internal net; `R96` is what joins that net to the card. Without it the MCU can transmit to the card and never hear it. | none — dedicated to the BT RX path |

The MCU→card direction (`GPIO_AD_B1_06` → `R189` → `U9` shifter → J8.32) ships
**fitted**, so only the receive side and the reset need bridging.

### Caution on `R345` — a 3-way shared pin

`GPIO_AD_B1_03` fans out to **three** loads, two of them fitted:

```
GPIO_AD_B1_03 ─R343 (FITTED)─→ SD_PWREN   (µSD card power enable)
              ─R344 (FITTED)─→ SPDIF_IN   (S/PDIF audio input)
              ─R345 (fit)   ─→ WL_RST#    → J8.56 PDn
```

After fitting `R345`, that pin drives all three. **Driving it low to reset the
module also cuts µSD card power** and disturbs the S/PDIF input. For a
Bluetooth-only bench this is acceptable. For a design that also needs the µSD
slot or S/PDIF, remove `R343`/`R344` — or, better, on a custom board give PDn
its own pin.

★ **`R343`/`R344` do NOT need removing just to make the pin swing.** During
bring-up they were removed on a *wrong diagnosis* (a firmware bug, not a
hardware clamp — see the GPIO6 gotcha below); with the firmware corrected the
pin drives high and low cleanly through the two 47 Ω series parts against both
loads. The removal was very likely unnecessary; it is recorded here so the next
board is not modified for the same wrong reason.

### ★ GPIO6-not-GPIO1 — a software gotcha that masquerades as a hardware fault

Driving these reset pins from the Teensy `teensy4` core (`EVKB_BOARD=rt1062`)
requires the **fast GPIO alias**. The core sets
`IOMUXC_GPR_GPR26..29 = 0xFFFFFFFF` in `startup.c`, so `GPIO_AD_B1_03` is owned
by **`GPIO6`**, even though the IOMUX ALT and NXP's own naming both still say
"`GPIO1_IO19`". Writing `GPIO1_*` registers does **nothing** and reading
`GPIO1_PSR` returns a value unrelated to the pin — which once manufactured a
false "PIN STUCK" verdict and cost two resistors off the board. If you write
direct GPIO register code for rt1062, use `GPIO6..GPIO9`, and prove a pin moves
by reading it back through the **same** instance you drove.

### What was fitted and verified

`R345` and `R96` bridged by hand (2026-08-25). An on-chip continuity probe then
confirmed the RX line is **driven high externally** (a healthy idle UART — the
bridges conduct, the level shifter drives) and **PDn swings** when read back at
the pad. The card is still silent on this board *for a different reason than the
1170* — it never greets at all, a board-level issue below the Bluetooth question
that needs a scope on J8.22/J8.56, and is not evidence about the module (the
same card greets reliably on the 1170).

---

## Side-by-side

| | MIMXRT1170-EVKB | MIMXRT1060-EVKB |
|---|---|---|
| BT HCI UART | LPUART2 (`GPIO_DISP_B2_10/11`) | LPUART3 (`GPIO_AD_B1_06/07`) |
| PDn / reset pin | `GPIO_AD_31` → J54.56 | `GPIO_AD_B1_03` → J8.56 |
| **Fit (DNP → 0 Ω)** | `R404`, `R1901` (+`R1902`) | `R345`, `R96` |
| **Remove (fitted)** | `R183`, `R1816` (flow-control only) | — (`R343`/`R344` only if µSD/SPDIF needed) |
| PDn pin also drives | Arduino D12/MISO; SPI NOR SO | µSD power-enable; S/PDIF input |
| UART pads also used by | Arduino D0–D2; Ethernet PHY (RTS/CTS) | — |
| Card→MCU RX ships | DNP (`R1901`) | DNP (`R96`) |
| MCU→Card TX ships | fitted | fitted |
| Official NXP guide | yes (EdgeFast BT PAL) | no (found from netlist) |

**The shared pattern:** on both boards the **card→MCU receive** resistor and the
**PDn/reset** resistor ship DNP, while the MCU→card transmit side ships fitted.
That is why an un-reworked board *transmits* to the card perfectly and *hears*
nothing — the exact false-symptom (a healthy-looking transmit path, a dead
receive path) that this rework exists to fix.

---

## What the rework does NOT fix

With the minimum rework on the RT1170, the Bluetooth firmware **downloads
completely** (131,840 bytes, CRC-checked, the card's own retransmit path
exercised) and the controller then **never answers HCI** — at any baud rate,
with NXP's own EdgeFast stack failing identically. None of the outstanding
rework items touches that: they govern the *download/command* direction and the
*reset* line, and the download already completes. The surviving hypothesis is
that the module rejects an unsigned/mismatched image internally (secure boot),
which no host-side change can address. This is u-blox support case
**CA-276115**; full evidence and the eliminated hypotheses are in
`examples/networking/m2_hci_probe/transcript_hw_evkb.txt` and
`docs/m2-maya-w161-support-case.md`.

---

## Provenance

* RT1170 populate states and nets: `MIMXRT1170-EVKB-DESIGNFILES_RevC3/pst2kicad/`
  (`board.net`, `pstxprt.dat`), plus NXP's EdgeFast BT PAL rework guide.
* RT1060 populate states and nets:
  `MIMXRT1060-EVKB-DESIGNFILE-RevB1/Schematic/allegro/` (`pstxnet.dat`,
  `pstxprt.dat`).
* All "verified on silicon" claims:
  `examples/networking/m2_hci_probe/transcript_hw_evkb.txt`.
* The RT1170 M.2 pin-by-pin map and the deeper reasoning behind each collision
  live in `docs/m2-evkb-revc3.md`; this file is the actionable rework summary.
