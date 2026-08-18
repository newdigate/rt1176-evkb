# MIMXRT1170-EVKB RevC3 — M.2 socket (J54) wiring

**Source of truth:** the RevC3 design files in `~/Development/rt1170/MIMXRT1170-EVKB-DESIGNFILES_RevC3` —
`pst2kicad/board.net` for connectivity, `BOM/SCH-55139_C3.xlsx` for populate/DNP,
`Schematic/allegro/pstchip.dat` for ball→pad names. Extracted programmatically
on 2026-08-17. The board Hardware User Guide has **no** M.2 section beyond the
connector list, so the schematic is the only source.

Card fitted for this work: u-blox `M2-MAYA-W161-00C` (NXP **IW416** — Wi-Fi 4
dual-band 1×1 + Bluetooth/LE 5.2; WLAN over SDIO, Bluetooth over UART).

**Populate status is load-bearing on this connector.** Every claim below was
checked against the BOM, not just the netlist — a net existing in the schematic
says nothing about whether the resistor on it is fitted. Three of the signals
that look connected are not.

## ★ REQUIRED REWORK: two DNP resistors must be bridged

**An M.2 Wi-Fi card does not work on a stock RevC3 board.** Two 0402 0 Ω
resistors are DNP from the factory and must be fitted. Established on
2026-08-18 by bridging them and watching an M2-MAYA-W161 go from totally silent
to fully enumerating.

| Ref | Connects | Consequence if absent |
|---|---|---|
| **R404** | `GPIO_AD_31` → PDn chain → J54 pin 56 (`W_DISABLE1#`) | **FATAL.** The radio can never be power-cycled and never boots. |
| **R1901** | `U355` → `BT_UART_RXD` → J54 pin 22 | Bluetooth HCI is transmit-only; no reply can ever be read |

### Why R404 is fatal, and why it is so hard to diagnose

PDn is "Full Power-down for the Wi-Fi/BT radio: High = normal, Low = full
power-down". With R404 unpopulated, `GPIO_AD_31` is disconnected and pin 56
merely rests on the 10K pull-up `R829` — which holds it at exactly the logic
high the datasheet asks for. **Every static check therefore passes.** But a
module that has come up in a bad state can only be recovered by asserting PDn
low, and without R404 no firmware can do that. The radio sits there with every
output at zero, indefinitely.

The symptom is deeply misleading: all card→host signals (`SDIO_CMD`,
`UART_TXD` pin 22, `BT_WAKE_HOST` pin 20) are **driven** low, not floating —
which correctly proves the card's DC-DC and level shifters are alive, and
wrongly suggests the module behind them is faulty.

**This also defeats NXP's own software.** `BOARD_WIFI_BT_Enable()` drives
`BOARD_INITPINSM2_WL_RST_GPIO` = `GPIO9_IO30` = `GPIO_AD_31`. On a stock RevC3
that write reaches nothing, so `wifi_cli` hangs at "Initialize WLAN Driver". The
SDK is correct; the board's DNP defeats it.

### The working sequence, after the rework

    PDn (GPIO_AD_31 / GPIO9_IO30, ALT10) LOW    >= 10 ms
    PDn HIGH
    wait ~1 s for the ROM to boot
    then enumerate SDIO

Verified result: `manfid=0x2DF cardid=0x9158 io_functions=1 rca=0x1 cccr_rev=0x3`.

Note both bridges commit an Arduino header pin: `GPIO_AD_31` is D12/MISO, and
the BT UART RX pad `GPIO_DISP_B2_11` is D0.

## The three facts that will bite you

### 1. uSDHC1 carries TWO card sockets

`R366`, `R367`, `R369`–`R372` (MCU→M.2) **and** `R1890`, `R1892`–`R1895`
(MCU→microSD J15) are all **fitted**. There is no DNP on either side: one SD
bus, two sockets, wired in parallel onto the same six MCU balls. The shared nets
are visible directly in the netlist, e.g. `N100526295` carries `U19.B16`,
`R369.1` and `R1890.1` together.

Consequences:

* Wi-Fi and the microSD card are **mutually exclusive**. Wi-Fi work requires
  the microSD slot empty.
* With the M.2 card fitted, `storage-memory/sd_test` and
  `audio/sd_wav_play_test` **hardware** runs may fail for reasons unrelated to
  their own changes. QEMU gates are unaffected (no M.2 model).
* **You cannot power the module down to get the bus back.** `WL_3V3` comes from
  `SENSOR_3V3` through ferrite `L49` (fitted) with no switch anywhere. Physical
  removal of the card is the only isolation.

### 2. The Bluetooth UART is transmit-only as built

The module→MCU half of the link is **not populated**:

| | Ref | Status | Path |
|---|---|---|---|
| RXD | `R1901` | **DNP** | `U355.20` ──╳── `BT_UART_RXD` (`U19.A6`) |
| CTS | `R1902` | **DNP** | `U355.19` ──╳── `BT_UART_CTS` (`U19.B6`) |

TX is fine — `BT_UART_TXD` reaches `U354.4` with no series resistor at all. So
LPUART2 can transmit to the IW416 and **will never receive a byte back**. An HCI
driver written against this connector without fitting `R1901` will look like a
firmware bug indefinitely.

`R1903` (BT_PCM_RXD) is DNP for the same reason.

To restore Bluetooth receive: fit `R1901` (0402, 0 Ω). `R1902` is not worth
fitting — see fact 3.

### 3. LPUART2's CTS/RTS pads belong to the gigabit Ethernet PHY

Both of these resistors **are** fitted:

| Ref | Path | Consequence |
|---|---|---|
| `R1866` | `BT_UART_RTS` (`U19.A5`) ── `ETHPHY_RST_B` ── `U10.12` | Asserting RTS **holds the RTL8211FDI-CG in reset** |
| `R1816` | `BT_UART_CTS` (`U19.B6`) ── `RGMII1_PHY_INTB` ── `R124` ── `U10.31` | UART and PHY interrupt fight over one net |

`U10` is the 1 Gb PHY behind `networking/enet_test`, `ethernet_test`,
`lwip_test` and `native_ethernet_test`. `ETHPHY_RST_B` has a 4.7K pull-up
(`R81`); `RGMII1_PHY_INTB` has one too (`R103`).

**Do not enable hardware flow control on LPUART2 on this board.** RTS is not a
UART signal here, it is the PHY's reset line.

### And the header collision (less severe, still real)

`R2`/`R3`/`R8` are fitted, so `GPIO_DISP_B2_10/11/12` reach both J54 and Arduino
header J9. Anything using header D0–D2 collides with Bluetooth — and D2/CTS
additionally collides with the PHY interrupt, per fact 3.

Useful in the other direction: a jumper between **J9 pin 2 and J9 pin 4** is a
real TX↔RX loopback for LPUART2. Both are even-column socket pins, so the
odd-column GND hazard documented in `arduino-header-revc3.md` is not engaged.
Because `R1901` is DNP the module physically cannot drive the RX pad, so there
is no contention to worry about — this loopback is currently the **only** way to
exercise LPUART2 receive at all.

## J54 to MCU

Populate status is from the BOM. "fitted" means the whole path is populated.

| M.2 function | J54 pins | MCU pad | Status | Path / notes |
|---|---|---|---|---|
| SDIO CMD | 11 | `GPIO_SD_B1_00` ALT0 | fitted | uSDHC1; shared with J15 |
| SDIO CLK | 9 | `GPIO_SD_B1_01` ALT0 | fitted | uSDHC1; shared with J15 |
| SDIO D0–D3 | 13,15,17,19 | `GPIO_SD_B1_02..05` ALT0 | fitted | uSDHC1; shared with J15 |
| BT UART TXD | 32 | `GPIO_DISP_B2_10` **ALT2** | fitted | 3V3→1V8 via U354; no series R |
| BT UART RXD | 22 | `GPIO_DISP_B2_11` **ALT2** | **DNP (`R1901`)** | see fact 2 |
| BT UART CTS | 34 | `GPIO_DISP_B2_12` **ALT3** | **DNP (`R1902`)** | see fact 2; pad also = PHY INTB |
| BT UART RTS | 36 | `GPIO_DISP_B2_13` **ALT3** | fitted | **pad is also `ETHPHY_RST_B`** — see fact 3 |
| BT_DISABLE# | 54 | `GPIO_AD_15` (ball M14, "SPDIF_IN" pad) | fitted | R209→R834; 10K pull-up R832; 27Ω R833 |
| WIFI_RST_B | 23 | `GPIO_AD_16` (ball N17, "SPDIF_OUT" pad) | fitted | R835→U354→R809; **no pull anywhere** |
| W_DISABLE1# | 56 | — | **DNP (`R404`)** | pull-up R829 to WL_3V3 only; MCU pad would have been `GPIO_AD_31` = Arduino **D12/MISO** |
| WL_DEV_WAKE | 66 | `GPIO_AD_07` (Arduino D8) | fitted | R1850→U354 |
| BT_DEV_WAKE | 42 | `GPIO_AD_28` (Arduino D13) | fitted | R406→U354 |
| BT_WAKE→host | 20 | `GPIO_AD_27` (ball N16) | fitted | R811→R238; **no level shifter — direct 3V3** |
| WIFI_WAKE→host | 21 | `GPIO_AD_29` (Arduino D10) | fitted | R2015, **via J104 — open by default** |
| I²C SDA / SCL | 58 / 60 | `GPIO_LPSR_04/05` ALT0 | fitted | LPI2C5 = `Wire2`; shared with the WM8962 codec |
| 32.768 kHz sleep clock | 50 | — (from `Y2`) | fitted | R823; oscillator, not an MCU signal |
| BT PCM | 8,10,12,14 | `GPIO_EMC_B2_13..16` | **DNP** (`R228`/`R229`/`R232`/`R234`) | see below |

`GPIO_AD_15`, `GPIO_AD_16` and `GPIO_AD_27` are **not** on the Arduino header;
they need direct pad/GPIO control.

**The PCM row is worse than DNP.** The four MCU-side resistors are unpopulated,
and the pads behind them (`GPIO_EMC_B2_13..16`) run through **fitted**
`R1985`–`R1988` to `SEMC_D28`–`SEMC_D31` — the SDRAM data bus. Routing BT PCM
audio would mean lifting SDRAM data lines. Treat this interface as unavailable.
(The `J79`–`J82` jumper defaults are moot given the DNP resistors.)

## Reset lines — settled against NXP's own board support

Names reconciled with `mcuxsdk/examples/_boards/evkbmimxrt1170` (`pin_mux.h`,
`wifi_bt_config.c`), which is the authoritative software view of this socket:

| NXP name | MCU pad | GPIO used by NXP | J54 | Schematic net |
|---|---|---|---|---|
| `SDIO_RST` | `GPIO_AD_16` (ball N17) | **GPIO9_IO15** (ALT10) | 23 | `WIFI_RST_B` |
| `WL_RST` / **PDn** | `GPIO_AD_31` (ball J17) | **GPIO9_IO30** (ALT10) | 56 | `WL_RST#`. R404 is DNP from the factory — **bridge it**, see the REQUIRED REWORK section at the top. This is the master power-down and the card does not boot without it. |

**The required sequence** (`BOARD_WIFI_BT_Enable(true)`) is: both lines
initialised as outputs driven **low**, then `SDIO_RST` high → wait **100 ms** →
`WL_RST` high → wait **100 ms**. NXP drives the **fast** GPIO alias (GPIO9,
ALT10), not GPIO3/ALT5.

`GPIO_AD_16` has **no pull resistor anywhere** — its net has exactly two nodes,
`R835.2` and `U354.3`, and the 1V8 side is likewise unpulled. At POR the pad is
a high-Z input feeding a 74AVC8T245 input, so the module's reset state is
indeterminate until firmware drives it. `BT_RST#` is better defined: 10K `R832`
to WL_3V3.

**Reading these pads back requires SION.** Mux value `0x1A` (SION | ALT10), not
`0x0A`. With SION off, `GPIO9_PSR` reads 0 whatever the pin is doing — which
looks exactly like a drive that failed. Cost a debug cycle on 2026-08-17.

★ **Doing all of the above is still not sufficient to enumerate the
M2-MAYA-W161.** With both resets confirmed high *at the pin*, the bus idling
high, INITA sent and the clock root taken from OSC_24M, the module does not
answer CMD5. See `examples/networking/m2_sdio_probe/transcript_hw_evkb.txt` for
the full evidence and the four refuted hypotheses. The open questions are
physical: contact, and whether Y2's 32.768 kHz on pin 50 is oscillating.

## Module LEDs

`WIFI_LED1_B` (D18) and `BT_LED2_B` (D19) have their anodes on `WL_3V3` through
1.00 kΩ and their cathodes on `J54.6`/`J54.16` — the card sinks them, the MCU is
not involved. A free visual smoke test.

## Corrections made

The first version of this document (commit `8dcae1d`) stated the BT UART rows as
working paths and did not mention the Ethernet PHY collisions. Both errors came
from reading the netlist without re-checking the BOM on those specific
resistors — the same check that was correctly applied to the SDIO rows. If you
add a row here, check populate status for every series resistor on the path.

## Power sequencing — why the MAYA-W1's 1.15 ms rule does not apply here

The G2 Nano board (`~/Development/DEV-G2-NANO`, `kicad/power.kicad_sch`) carries
this note against its MAYA-W1:

> "1.8V supply must be delayed by at least 1.15ms for the MAYA-W1"

That is a real constraint, but it belongs to a **module-down** design, where the
host supplies the bare module BOTH rails and must therefore sequence them.

On this board the M.2 card is supplied **3.3 V only**:

    J54 pins 2, 4, 72, 74 = WL_3V3      (the only supply pins on the connector)
    VDD_1V8 does NOT reach J54

The card generates its own 1.8 V internally, so the sequencing is handled on the
card and cannot be got wrong by the host. Checked because it looked like a
promising explanation for the M2-MAYA-W161 not enumerating; it is not one.

Two things it IS worth knowing:

* `L26` (90 Ω) on J54 pins 3/5 is a common-mode choke on the USB pair, not power.
* `VDD_1V8` still matters board-side: it powers U354/U355 (every M.2 control
  signal is level-shifted through them) and U311's 1.8 V leg. It is a board rail,
  not a module rail.

Related, and established elsewhere in this file: `WL_3V3` reaches the card
through ferrite `L49` from `SENSOR_3V3` with **no switch**, so firmware cannot
power-cycle the module at all. Only a board power cycle or physical removal
re-powers it.

## What the M2-MAYA-W1 datasheet settles (UBX-22004354 R05)

Read 2026-08-17. Three facts that firmware must not get wrong, and one that
retires a whole line of investigation.

### Boot configuration is ON THE CARD — there is nothing for the host to drive

Host-interface and firmware-boot selection is `CONFIG[1:0]` on the MAYA-W1
module, set by resistors on the M.2 card itself:

    R34 = DNI  -> CONFIG[1] = 1   (internal pull-up)
    R35 = 51k  -> CONFIG[0] = 0   (pulled to GND)
    => "10" = Wi-Fi over SDIO, Bluetooth over UART, 1 SDIO function

That is the default and it is what we want. `CONFIG[1:0]` is **not routed to the
M.2 connector**, so no host GPIO affects it.

### J54 pin 23 (`SDIO_RESET#`) is NOT CONNECTED on this card

The datasheet pin table gives pin 23 pin-type **NC**. NXP call it
`WLAN_INDEPENDENT_RESET` and the EVKB wires `GPIO_AD_16` to it, but the
M2-MAYA-W1 does not connect it.

**Everything this repo did driving `GPIO_AD_16` was therefore a no-op**, however
carefully sequenced. Kept in the probe only because it matches NXP's reference
and is harmless.

### PDn is J54 pin 56, and it is already correct

Pin 56 `W_DISABLE1#` is NXP's **PDn**: "High = normal mode, Low = full
power-down", a 3.3 V input. On RevC3 it sits high through the 10K `R829` to
WL_3V3 (`R404` being DNP is irrelevant — the pull-up is what holds it). The
module is in normal mode and the host cannot change that.

### VIO_SD defaults to 1.8 V — the host must switch NVCC_SD

§2.2: "All bus speed modes are supplied from the SDIO I/O power supply (by
default set to 1.8 V)." §6 confirms `VIO_SD` defaults to 1.8 V from the card's
own DC-DC, changeable to 3.3 V only by moving a 0 Ω resistor from R24 to R25 on
the card.

Table 9 gives the card's absolute maximum input as `VIO + 0.4` = **2.2 V**, so
the EVKB's default 3.3 V `NVCC_SD` is out of spec against a stock card. This is
exactly why NXP define `SDMMCHOST_OPERATION_VOLTAGE_1V8` for every IW416 module.

`SdioHost::useIoVoltage1V8()` implements the switch, and the register readback
confirms it takes effect (`vend_spec` bit 1 set, `mux=0x4` = ALT4
USDHC1_VSELECT). **It does not make this card respond.**

### Where that leaves it

With boot config correct on-card, PDn held high, the SDIO rail switched to the
1.8 V the card expects, and NXP's own stack failing identically, every
configurable thing is in the state the datasheet asks for. The card remains
silent. See `examples/networking/m2_sdio_probe/transcript_hw_evkb.txt`.

One thing still unproven without a meter: that `VEND_SPEC[1]` actually moves the
rail in the expected DIRECTION. TP33 sits on `NVCC_SD` and would settle it in
seconds. A functional alternative needing no meter: fit a microSD and run
`sd_test` with `useIoVoltage1V8(true)` — a 3.3 V card should FAIL at 1.8 V, and
if it passes regardless, the rail is not moving.

## Pin-by-pin audit: M2-MAYA-W1 card vs EVKB J54

Done 2026-08-17 after the MIPI-display precedent, where a connector that looked
compatible was not. Card side from the datasheet pin tables (UBX-22004354 R05
Tables 4/5), board side from the RevC3 netlist. All 75 pins compared
mechanically.

**Verdict: no connector mismatch.** Everything the card actually uses is
correctly mapped, and no signal direction conflicts.

### Correct — every pin that matters

| Card pins | Function | EVKB |
|---|---|---|
| 9, 11, 13, 15, 17, 19 | SDIO CLK/CMD/D0–D3 | uSDHC1, all series R fitted |
| 2, 4, 72, 74 | 3.3 V supply | `WL_3V3` (all four) |
| 1,7,18,33,39,45,51,57,63,69,75 | GND | GND |
| 32, 36 | UART_RXD, UART_CTS — card **inputs** | driven from U354 (MCU→module) |
| 22, 34 | UART_TXD, UART_RTS — card **outputs** | read via U355 (module→MCU) |
| 20 | UART_WAKE# — card **output**, 3.3 V | direct to `GPIO_AD_27`, no shifter (datasheet says 3.3 V — correct) |
| 21 | SDIO_WAKE# — card **output** | via U355 (module→MCU), then J104 |
| 42 | VENDOR_DEF3 = DEV_BT_WAKE — card **input** | driven via U354 (MCU→module) |
| 54, 56 | BT_INDEPENDENT_RESET, PDn — card **inputs**, 3.3 V | driven / pulled high |

**No contention anywhere.** Every card output lands on a board input path, every
card input is fed from a board output path. Direction and voltage domain agree
on all of them.

### The one genuine disagreement — DEV_WLAN_WAKE

| | Card says | EVKB does |
|---|---|---|
| pin 40 (`VENDOR_DEF2`) | **DEV_WLAN_WAKE**, an input: "platform wakes the Wi-Fi radio, active low" | `VEN_DEF2` → **TP48 test point only — undriven** |
| pin 66 (`UIM_SWP/PERST1#`) | **NC** | drives `WL_DEV_WAKE_1V8` from U354 |

The EVKB puts WL_DEV_WAKE on pin 66, where this card has nothing; the card wants
it on pin 40, where the EVKB has only a test point. The two disagree about where
that signal lives.

**This does not explain the enumeration failure** — DEV_WLAN_WAKE is a wake
sideband, not required to answer CMD5, and an undriven input with an internal
pull-up sits inactive. But it is a real board/card discrepancy and would matter
for low-power work.

### Board signals that go nowhere (card pin is NC)

Harmless, but worth knowing before debugging any of them:

| Pin | EVKB drives | Card |
|---|---|---|
| 23 | `WIFI_RST_B` via U354 (`GPIO_AD_16`) | **NC** — all the reset sequencing was a no-op |
| 50 | 32.768 kHz from `Y2` via `R823` | **NC** — retires the "sleep clock missing" theory outright |
| 58, 60 | LPI2C5 (`Wire2`) | **NC** |
| 66 | `WL_DEV_WAKE_1V8` | **NC** (see above) |
| 6, 16 | `WIFI_LED1_B`, `BT_LED2_B` | **NC** — so the board LEDs never light from this card, and their being dark means nothing |

That last row matters: D18/D19 were listed earlier in this file as a free visual
smoke test. **They are not** — this card does not connect them.
