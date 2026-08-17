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

## Reset lines — read this before writing any control code

There is no MCU control over `W_DISABLE1#` (pin 56): `R404` is DNP, so the pin
sits at WL_3V3 through a 10K pull-up permanently. The two lines the MCU *can*
drive are `GPIO_AD_15` → pin 54 and `GPIO_AD_16` → pin 23.

`GPIO_AD_16` has **no pull resistor anywhere** — its net has exactly two nodes,
`R835.2` and `U354.3`, and the 1V8 side is likewise unpulled. At POR the pad is
a high-Z input feeding a 74AVC8T245 input, so the module's reset state is
genuinely **indeterminate until firmware drives it**. `BT_RST#` is better
defined: 10K `R832` to WL_3V3.

The module-side meaning of pin 23, and whether the two lines need a particular
assertion order, is **unconfirmed** — the u-blox M.2 card user guide's pinout is
an image, not extractable text. Driving both high is very likely correct but is
a guess until the board says otherwise. Resolve it empirically and record the
result here.

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
