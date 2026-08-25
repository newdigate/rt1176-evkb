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

## ★ FULL DNP AUDIT OF THE M.2 / BLUETOOTH PATHS (2026-08-24)

Done because "is there another 0 Ω we still need to bridge?" deserved a
mechanical answer rather than a recollection. Method: every net touching J54 or
the two level shifters `U354`/`U355` extracted from `pst2kicad/board.net`, every
component on those nets looked up in `BOM/SCH-55139_C3.xlsx` (`ASSY_OPT` column
= `DNP`). The map was validated first against parts whose state this file
already records — `R404`, `R1901`, `R1902`, `R1903` all read DNP; `R183`,
`R1816`, `R1866`, `R2`, `R3`, `R8`, `R366` all read fitted — so the extraction
agrees with everything previously established by hand.

### The verdict: **the Bluetooth data path is COMPLETE. Nothing is missing.**

| signal | path | series part | state |
|---|---|---|---|
| **BT_UART_TXD** (MCU → card) | `U19.D9` → `U354.4` | **none** | complete by construction |
| **BT_UART_RXD** (card → MCU) | `U355.20` → `R1901` → `U19.A6` | `R1901` | **bridged by hand 2026-08-18** |
| **BT_UART_RTS** (MCU → card's CTS input) | `U19.A5` → `U354.5` | **none** | complete by construction |
| **BT_UART_CTS** (card's RTS → MCU) | `U355.19` → `R1902` → `U19.B6` | `R1902` | **DNP, still open** |
| **PDn** (`GPIO_AD_31` → J54.56) | `R404` → `WL_RST#` → `R830` | `R404` | **bridged by hand 2026-08-18** |

★ Two of those rows are worth stating loudly because they are *absences of a
resistor*, which is easy to misread as an absence of a connection:
**BT_UART_TXD and BT_UART_RTS reach the level shifter with NO series resistor
at all.** So the transmit path and the card's CTS input were always complete.
That is what makes the 2026-08-24 CTS experiment meaningful — driving
`GPIO_DISP_B2_13` really does reach the card's CTS pin — and its negative
result therefore stands.

**Both level shifters' direction straps are fitted**: `R1797` (U354 `DIR` →
`WL_3V3`) and `R1798` (U355 `DIR` → `VDD_1V8`). Both directions are also proven
in use — we transmit 131,840 bytes and receive the bootloader's frames.

**The BT reset and wake chains are fully populated**: `BT_RST#` via `R209`+`R834`,
`BT_WAKE_B_3V3` via `R238`+`R811`.

**No DNP part sits on any SIGNAL pin of J54.** The only DNP items on J54 nets
are on `GND` (bypass caps, mounting holes) and on the `NC` pins (the `U124` ESD
array) — neither carries anything.

### The DNP parts that DO exist on these nets, and why none is needed

| ref | net | why it does not matter |
|---|---|---|
| `R1902` | BT_UART_CTS | the card's RTS **output**. Not fitting it only means the HOST cannot see the card's flow control; it cannot stop the card transmitting. NXP pair fitting it with REMOVING `R1816` — see below. |
| `R1903`, `R229`, `R232`, `R234` | BT PCM | PCM is unavailable on this board regardless: the MCU-side pads run through fitted `R1985`–`R1988` to the SDRAM data bus. |
| `R1841`, `R2022` | BT_UART_RXD | alternative routes; the primary path via `R1901` is complete. |
| `R1842`, `R2021` | BT_UART_TXD | alternative routes; the primary path is direct. |
| `R1839` | BT_UART_CTS | alternative route. |
| `R1029`, `R1039` | BT_UART_RTS | an alternative route and a 10K pull; the primary path is direct. |

### NXP's own rework list, reconciled

From the SDK's EdgeFast BT PAL hardware rework guide
(`middleware/edgefast_bluetooth/docs/HWRGEFBTPALUG`, `MIMXRT1170EVKB_hwrework.md`):

> Remove `R183` and `R1816`. Solder 0 Ω to `R404`, `R1901`, and `R1902`.

| their step | our state | consequence of not doing it |
|---|---|---|
| fit `R404` | **done** | none — the radio power-cycles |
| fit `R1901` | **done** | none — the card's UART TX is readable |
| fit `R1902` | not done | the host cannot read the card's RTS. Only matters with hardware flow control, which this board cannot use anyway while `R1816` is fitted. |
| remove `R1816` | not done | `BT_UART_CTS` is shared with `RGMII1_PHY_INTB`. Removing it is what would make `R1902` useful. |
| remove `R183` | not done | ★ `R183` ties `GPIO_AD_31` — the PDn net, and Arduino D12/MISO — to `U27.2`, the **data output of a Macronix `MX25L4006` SPI NOR flash**. A third driver can appear on the line we power-cycle the radio with. Harmless while that flash is never selected; a real hazard once `R404` is fitted, which is why NXP pair the two steps. |

★ **None of the three outstanding items can prevent the controller from
transmitting**, which is the symptom under investigation. They are recorded so
the question does not have to be re-asked, and so that anyone attempting
hardware flow control knows the `R1816`/`R1902` pair must be done together.

## ★ COMPLETE J54 PIN MAP: card ⇄ board ⇄ RT1176 (generated 2026-08-24)

All 75 pins, three columns wide: what the **M2-MAYA-W161** does with the pin,
what the **EVKB** wires it to, and which **RT1176 ball and pad** it lands on.

**Generated, not transcribed.** Board side walked programmatically from
`pst2kicad/board.net` through series resistors and the two 74AVC8T245 level
shifters (whose A-side pin *n* pairs with B-side pin *24−n*), populate status
from `BOM/SCH-55139_C3.xlsx`, and ball→pad names from
`Schematic/allegro/pstchip.dat` (where `PIN_NUMBER` is a 12-field tuple with the
ball in whichever field is non-zero). Card side transcribed from u-blox
**UBX-22004354 R05** tables 4 and 5. Spot-checked against every value this file
already recorded by hand — `GPIO_DISP_B2_11` = A6, `GPIO_AD_31` = J17, the six
SDIO pads — all agree.

Directions are **from the card's point of view**, as u-blox state them.

| J54 | MAYA-W161 pin | dir | card function | board net | path (DNP marked) | RT1176 ball | RT1176 pad |
|---:|---|:-:|---|---|---|:-:|---|
| 1 | GND1 | - | Ground | GND | | | |
| 2 | 3.3V | P | Supply | WL_3V3 | via ferrite L49 from SENSOR_3V3 | | |
| 3 | USB_D+ | NC | Not connected | `N101733829` | L26 (USB common-mode choke) | | |
| 4 | 3.3V | P | Supply | WL_3V3 | via ferrite L49 from SENSOR_3V3 | | |
| 5 | USB_D- | NC | Not connected | `N101733827` | L26 (USB common-mode choke) | | |
| 6 | LED_1# | NC | Not connected | `WIFI_LED1_B` | D18 LED anode (card side NC) | | |
| 7 | GND2 | - | Ground | GND | | | |
| 8 | PCM_CLK/I2S_SCK | I/O | PCM data clock | `N101734644` | J81/J80 jumper | | |
| 9 | SDIO_CLK | I | SDIO clock | `N101734331` | R781 → R370 | D15 | `GPIO_SD_B1_01` |
| 10 | PCM_SYNC/I2S_WS | I/O | PCM frame sync | `N101734640` | J82/J79 jumper | | |
| 11 | SDIO_CMD | I/O | SDIO command | `N101734351` | R782 → R369 | B16 | `GPIO_SD_B1_00` |
| 12 | PCM_OUT/I2S_SD_OUT | O | PCM data output | `N101734642` | R817 → U355 → R1903**(DNP)** → R228**(DNP)** | K5 | `GPIO_EMC_B2_13` |
| 13 | SDIO_D0 | I/O | SDIO data 1 | `N101734355` | R783 → R371 | C15 | `GPIO_SD_B1_02` |
| 14 | PCM_IN/I2S_SD_IN | I | PCM data input | `N101734654` | R820 → U354 → R234**(DNP)** | M4 | `GPIO_EMC_B2_14` |
| 15 | SDIO_D1 | I/O | SDIO data 2 | `N101734367` | R784 → R372 | B17 | `GPIO_SD_B1_03` |
| 16 | LED2# | NC | Not connected | `BT_LED2_B` | D19 LED anode (card side NC) | | |
| 17 | SDIO_D2 | I/O | SDIO data 3 | `N101734379` | R785 → R366 | B15 | `GPIO_SD_B1_04` |
| 18 | GND3 | - | Ground | GND | | | |
| 19 | SDIO_D3 | I/O | SDIO data 4 | `N101734391` | R786 → R367 | A16 | `GPIO_SD_B1_05` |
| 20 | UART_WAKE# | O | BT_WAKE_HOST (3.3 V, open drain) | `N101734503` | R811 → R238 | N16 | `GPIO_AD_27` |
| 21 | SDIO_WAKE# | O | WLAN_WAKE_HOST (open drain) | `N101734410` | J104 jumper (OPEN by default) | | |
| 22 | UART_TXD | O | card TX -> host RX | `N101734471` | R814 → U355 → R1901**(DNP)** | A6 | `GPIO_DISP_B2_11` |
| 23 | SDIO_RESET# | NC | WLAN_INDEPENDENT_RESET - NOT CONNECTED ON THIS CARD | `N101734224` | R809 → U354 → R835 | N17 | `GPIO_AD_16` |
| 24 | (reserved) |  | - | *(absent from netlist)* | | | |
| 25 | (reserved) |  | - | *(absent from netlist)* | | | |
| 26 | (reserved) |  | - | *(absent from netlist)* | | | |
| 27 | (reserved) |  | - | *(absent from netlist)* | | | |
| 28 | (reserved) |  | - | *(absent from netlist)* | | | |
| 29 | (reserved) |  | - | *(absent from netlist)* | | | |
| 30 | (reserved) |  | - | *(absent from netlist)* | | | |
| 31 | (reserved) |  | - | *(absent from netlist)* | | | |
| 32 | UART_RXD | I | host TX -> card RX | `N101734459` | R815 → U354 | D9 | `GPIO_DISP_B2_10` |
| 33 | GND4 | - | Ground | GND | | | |
| 34 | UART_RTS | O | card RTS -> host CTS | `N101734453` | R816 → U355 → R1902**(DNP)** | B6 | `GPIO_DISP_B2_12` |
| 35 | PERP0 | NC | PCIe, not connected | NC (board) | | | |
| 36 | UART_CTS | I | host RTS -> card CTS | `N101734451` | R813 → U354 | A5 | `GPIO_DISP_B2_13` |
| 37 | PERN0 | NC | PCIe, not connected | NC (board) | | | |
| 38 | VENDOR_DEF1 | I/O | JTAG_TDO (debug) | `VEN_DEF1` | TP47 test point only | | |
| 39 | GND5 | - | Ground | GND | | | |
| 40 | VENDOR_DEF2 | I | DEV_WLAN_WAKE | `VEN_DEF2` | TP48 test point only | | |
| 41 | PETP0 | NC | PCIe, not connected | NC (board) | | | |
| 42 | VENDOR_DEF3 | I | DEV_BT_WAKE | `BT_DEV_WAKE_1V8` | U354 → R406 | L17 | `GPIO_AD_28` |
| 43 | PETN0 | NC | PCIe, not connected | NC (board) | | | |
| 44 | COEX3 | I/O | JTAG_TDI (debug) | `COEX3` | TP51 test point only | | |
| 45 | GND6 | - | Ground | GND | | | |
| 46 | COEX2 | I/O | JTAG_TCK (debug) | `COEX2` | TP50 test point only | | |
| 47 | REFCLKP0 | NC | PCIe, not connected | NC (board) | | | |
| 48 | COEX1 | I/O | JTAG_TMS (debug) | `COEX1` | TP49 test point only | | |
| 49 | REFCLKN0 | NC | PCIe, not connected | NC (board) | | | |
| 50 | SUSCLK(32KHZ) | NC | NOT CONNECTED ON THIS CARD | `N101733765` | Y2 32.768 kHz oscillator | | |
| 51 | GND7 | - | Ground | GND | | | |
| 52 | PERST0# | NC | Not connected | NC (board) | | | |
| 53 | CLKREQ0# | NC | PCIe, not connected | NC (board) | | | |
| 54 | W_DISABLE2# | I | BT_INDEPENDENT_RESET (3.3 V) | `N101734218` | R833 → R834 → R209 | M14 | `GPIO_AD_15` |
| 55 | PEWAKE0# | NC | PCIe, not connected | NC (board) | | | |
| 56 | W_DISABLE1# | I | PDn - high=normal, low=FULL POWER-DOWN (3.3 V) | `N101734037` | R831 → R830 → R404**(DNP)** | J17 | `GPIO_AD_31` |
| 57 | GND8 | - | Ground | GND | | | |
| 58 | I2C_DATA | NC | Not connected | `N101733917` | GPIO_LPSR_04 (LPI2C5 SDA = Wire2) | | |
| 59 | PERP1 | NC | PCIe, not connected | NC (board) | | | |
| 60 | I2C_CLK | NC | Not connected | `N101733919` | GPIO_LPSR_05 (LPI2C5 SCL = Wire2) | | |
| 61 | PERN1 | NC | PCIe, not connected | NC (board) | | | |
| 62 | ALERT# | NC | Not connected | NC (board) | | | |
| 63 | GND9 | - | Ground | GND | | | |
| 64 | RESERVED | NC | Not connected | NC (board) | | | |
| 65 | PETP1 | NC | PCIe, not connected | NC (board) | | | |
| 66 | UIM_SWP/PERST1# | NC | Not connected | `WL_DEV_WAKE_1V8` | U354 → R1850 | T17 | `GPIO_AD_07` |
| 67 | PETN1 | NC | PCIe, not connected | NC (board) | | | |
| 68 | UIM_PWR_SNK/CLKREQ1# | NC | Not connected | NC (board) | | | |
| 69 | GND10 | - | Ground | GND | | | |
| 70 | UIM_PWR_SRC/GPIO1/PEWAKE1# | NC | Not connected | NC (board) | | | |
| 71 | REFCLKP1 | NC | PCIe, not connected | NC (board) | | | |
| 72 | 3V3_3 | P | Supply | WL_3V3 | via ferrite L49 from SENSOR_3V3 | | |
| 73 | REFCLKN1 | NC | PCIe, not connected | NC (board) | | | |
| 74 | 3V3_4 | P | Supply | WL_3V3 | via ferrite L49 from SENSOR_3V3 | | |
| 75 | GND11 | - | Ground | GND | | | |


### Reading it

* **The Bluetooth UART occupies four pins**, 22/32/34/36. Note the naming
  inverts between the two sides: J54 pin 34 is the CARD's `UART_RTS` *output*,
  which the board calls `BT_UART_CTS` because it is the HOST's CTS input. Pin
  36 is the reverse. Getting this backwards is easy and expensive.
* **Two pins reach the MCU only through a DNP resistor**: 22 (`R1901`, bridged
  here) and 34 (`R1902`, still open). Everything else on the BT UART is direct.
* **Three pins the board drives are NOT CONNECTED on this card** — 23
  (`SDIO_RESET#`/WLAN_INDEPENDENT_RESET), 50 (the 32.768 kHz sleep clock from
  `Y2`), and 58/60 (I²C). Any effort spent driving them is wasted, which this
  file has recorded since 2026-08-17.
* **The LEDs are not connected on the card either** (6, 16), so `D18`/`D19`
  staying dark means nothing.
* ★ **Pins 38, 44, 46 and 48 are the chip's JTAG** — `TDO`, `TDI`, `TCK`, `TMS`
  in NXP's usage — and on the EVKB every one of them lands on a **test point
  only** (`TP47`, `TP51`, `TP50`, `TP49`). That is a debug port into the IW416
  itself, unused by anything here. Worth knowing exists, though driving it needs
  NXP's own tooling.
* **`DEV_WLAN_WAKE` is on the wrong pin for this card**: the EVKB drives pin 66,
  where the MAYA-W161 has nothing, while the card expects it on pin 40, which
  the EVKB leaves on a test point. A sideband only, but a real board/card
  mismatch.

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

### 2. The Bluetooth UART is transmit-only AS SHIPPED — but this board is reworked

★ **Read the Status column, not the heading.** `R1901` was bridged by hand on
2026-08-18 and the link has been bidirectional ever since: 131,840 bytes of V3
firmware download went across it in both directions on 2026-08-25, with the
card's own CRC-error retransmit exercised. A doc that still called this half
"not populated" is what the W20 brief was complaining about, so it is fixed
here rather than left for the next reader.

The module→MCU half is **DNP from the factory**:

| | Ref | Status | Path |
|---|---|---|---|
| RXD | `R1901` | **DNP from factory — BRIDGED BY HAND 2026-08-18, works** | `U355.20` → `BT_UART_RXD` (`U19.A6`) |
| CTS | `R1902` | **DNP, still open** | `U355.19` ──╳── `BT_UART_CTS` (`U19.B6`) |

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

---

## What the u-blox documents settle (added 2026-08-25)

Read from `M2-MAYA-W1` data sheet **UBX-22004354 R05** (which names this exact
part, `M2-MAYA-W161-00C-00`, NXP IW416) and the `MAYA-W1` system integration
manual **UBX-21010495 R09**, both C1-Public, in
`~/Development/rt1170/m2-maya-w161/`. These resolve several things this file
had guessed at or named ambiguously.

| M.2 pin | Standard name | NXP function | Direction | Notes |
|---|---|---|---|---|
| **54** | `W_DISABLE2#` | **BT_INDEPENDENT_RESET** — resets the Bluetooth radio only | input, active low, 3.3 V | "can be left open if not needed" (§2.4.2) |
| **56** | `W_DISABLE1#` | **PDn** — full power-down for **both** radios | input, 3.3 V | high = normal; **assert ≥ 100 ms for a correct reset** (§2.4.1) |
| **42** | `VENDOR_DEF3` | **BT_DEV_WAKE** — host wakes the BT radio | input, active low | ★ **never driven by this tree**; collides with LPSPI1 SCK |

★ **This file used to call pin 54 `BT_DISABLE#`.** Both names are defensible —
`W_DISABLE2#` is the M.2 standard name, `BT_INDEPENDENT_RESET` is what NXP do
with it — but they are *different pins* from `W_DISABLE1#`/PDn and were easy to
conflate. Pin 54 resets Bluetooth; pin 56 powers the whole module down.

★ **We do not drive BT_RST_N at all, and that is correct.** It idles high (not
in reset) through the fitted 10K `R832`, the datasheet permits leaving it open,
and there is positive proof the BT core resets properly anyway: it greets from
its ROM bootloader on every power-up (`start_inds=2`) and then accepts a full
firmware image. A core held in reset does neither. Its only remaining use is
resetting Bluetooth *without* a PDn cycle, which would otherwise take Wi-Fi
down too.

★★ **J54 PIN 36 HAS THREE ROLES, and confusing them has cost this project
time.** The same pad — `GPIO_DISP_B2_13`, MCU ball A5, net `BT_UART_RTS`,
also `ETHPHY_RST_B` via the fitted `R1866` — is:
  1. the card's **UART CTS input** in normal operation (ALT3 = `LPUART2_RTS_B`);
  2. the configuration pin **`CON[7]`**, sampled at module reset, "Reserved set
     to 1" — so driving it low across a PDn release latches a Reserved
     configuration;
  3. NXP's **boot-sleep wake trigger**: their `fw_loader_uart.c` calls
     `wakeUpControllerFromBootSleep()` from `uart_fw_download()`, *before* the
     image, re-muxing this pad to GPIO (ALT10 = `GPIO11_IO14`; ALT5 =
     `GPIO5_IO14` reaches the same pin through the normal instance), driving it
     **LOW for 10 ms**, then returning it to `LPUART2_RTS_B`.
★ Implemented as `M2_BT_WAKE_PULSE` (default ON) in `networking/m2_hci_probe`.
The card demonstrably reacts — it greets an extra time (`start_inds` 2→3) —
which is the only behavioural change this programme has ever produced on it.
It does not fix the silence.
★ And note the unavoidable side effect: `R1866` ties this net to
`ETHPHY_RST_B`, so any use of roles 2 or 3 also resets the gigabit PHY.
Harmless in a Bluetooth probe, not in an Ethernet example.

★ **`UART_CTSn` (pin 36) and `UART_RTSn` (pin 34) are CONFIGURATION PINS**
sampled at module reset — `CON[7]` and `CON[8]`, both "Reserved set to 1"
(§2.4.5 Table 6) — and only become UART signals ~1 ms later. **Driving CTS low
across the PDn release latches a Reserved configuration**, which invalidated one
2026-08-24 experiment. On this board nothing drives that pad and `R81` pulls
`ETHPHY_RST_B` up to `SENSOR_3V3` through the fitted `R1866`, so the card sees
1 — the default is correct, and only a deliberate CTS assert can break it.
Assert it *after* the reset if you need it.

★ **Card `VIO` defaults to 1.8 V** (§6 of the data sheet), which is why `U354`
and `U355` exist on the EVKB. Already understood and already handled here — not
a fault, and not something to "fix".

★ **The firmware picture.** §4.4.3 names the combo image
`sdiouartiw416_combo_v0.bin` as "the Wi-Fi/Bluetooth combo firmware image" —
one image, both radios — and §4.4.6 attaches Bluetooth with
`hciattach … any 3000000 flow`, i.e. **3 Mbaud with flow control**. That rate
was tested on this board on 2026-08-25 and **refuted**: after a fully
successful firmware download the card answers nothing at 3 M, 921600, 460800 or
115200, with `framing=0` throughout — so it transmits nothing at all, rather
than something we could not decode.
★ **And the combo image does NOT bring Bluetooth up on this card** — dumped in
bytes, not inferred: with the combo loaded and Wi-Fi running (`card=1`), the BT
UART emits a fresh ROM start indication (`AB 01 72 00 47`). The BT core is
still in its bootloader. NXP's real pairing for this mode is one image per bus
— `sdIW416_wlan.bin` over SDIO **plus** `uartIW416_bt.bin` over UART — and with
that pairing both downloads succeed. See
`examples/networking/m2_hci_probe/transcript_hw_evkb.txt`.
