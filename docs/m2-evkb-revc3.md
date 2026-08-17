# MIMXRT1170-EVKB RevC3 — M.2 socket (J54) wiring

**Source of truth:** the RevC3 design files in `~/Development/rt1170/MIMXRT1170-EVKB-DESIGNFILES_RevC3` —
`pst2kicad/board.net` for connectivity, `BOM/SCH-55139_C3.xlsx` for populate/DNP.
Extracted programmatically on 2026-08-17. The board Hardware User Guide has **no**
M.2 section beyond the connector list, so the schematic is the only source.

Card fitted for this work: u-blox `M2-MAYA-W161-00C` (NXP **IW416** — Wi-Fi 4
dual-band 1×1 + Bluetooth/LE 5.2; WLAN over SDIO, Bluetooth over UART).

## The two facts that will bite you

### 1. uSDHC1 carries TWO card sockets

`R369–R372/R366/R367` (MCU→M.2) **and** `R1890–R1895` (MCU→microSD J15) are all
**fitted**. There is no DNP on either side: one SD bus, two sockets, wired in
parallel onto the same six MCU balls.

Consequences:

* Wi-Fi and the microSD card are **mutually exclusive**. Wi-Fi work requires
  the microSD slot empty.
* With the M.2 card fitted, `storage-memory/sd_test` and
  `audio/sd_wav_play_test` **hardware** runs may fail for reasons unrelated to
  their own changes — an SDIO card answers CMD3 during SD initialisation. QEMU
  gates are unaffected (no M.2 model).

### 2. The Bluetooth UART pads are the Arduino D0/D1/D2 pads

`R2`/`R3`/`R8` are fitted, so `GPIO_DISP_B2_10/11/12` reach both J54 and the
Arduino header. Anything using header D0–D2 collides with Bluetooth.

Useful in the other direction: a jumper between **J9 pin 2 and J9 pin 4** is a
real TX↔RX loopback for LPUART2 — provided `BT_DISABLE#` is asserted first so
the module is not driving the same wires.

## J54 to MCU

| M.2 function | J54 pins | MCU pad | Path / notes |
|---|---|---|---|
| SDIO CLK/CMD/D0–D3 | 9,11,13,15,17,19 | `GPIO_SD_B1_00..05` | uSDHC1; shared with J15 (above) |
| BT UART TXD | 32 | `GPIO_DISP_B2_10` | LPUART2_TXD **ALT2**; 3V3→1V8 via U354 |
| BT UART RXD | 22 | `GPIO_DISP_B2_11` | LPUART2_RXD **ALT2**; 1V8→3V3 via U355 |
| BT UART CTS | 34 | `GPIO_DISP_B2_12` | LPUART2_CTS_B **ALT3**; via U355 |
| BT UART RTS | 36 | `GPIO_DISP_B2_13` | LPUART2_RTS_B **ALT3**; via U354 |
| BT_DISABLE# | 54 | `GPIO_AD_15` (ball M14, "SPDIF_IN" pad) | R209→R834; 10K pull-up R832; 27Ω R833 |
| WIFI_RST_B | 23 | `GPIO_AD_16` (ball N17, "SPDIF_OUT" pad) | R835→U354→R809 |
| W_DISABLE1# | 56 | **not connected** | **R404 is DNP** — pull-up R829 to WL_3V3 only |
| WL_DEV_WAKE | 66 | `GPIO_AD_07` (Arduino D8) | R1850→U354 |
| BT_DEV_WAKE | 42 | `GPIO_AD_28` (Arduino D13) | R406→U354 |
| WIFI_WAKE→host | 21 | `GPIO_AD_29` (Arduino D10) | R2015, **via J104 — open by default** |
| I²C SDA / SCL | 58 / 60 | `GPIO_LPSR_04/05` | LPI2C5 = `Wire2`; shared with the WM8962 codec |
| BT PCM BCLK/SYNC/RXD/TXD | 8,10,12,14 | SAI via U354/U355 | J81/J82 **open by default** |

`GPIO_AD_15` and `GPIO_AD_16` are **not** on the Arduino header; they need
direct pad/GPIO control.

## Reset lines — read this before writing any control code

There is no MCU control over `W_DISABLE1#` (pin 56): R404 is DNP, so the pin
sits at WL_3V3 through a 10K pull-up permanently. The two lines the MCU *can*
drive are `GPIO_AD_15` → pin 54 and `GPIO_AD_16` → pin 23.

The module-side meaning of pin 23, and whether the two need a particular
assertion order, is **unconfirmed** — the u-blox M.2 card user guide's pinout is
an image, not extractable text. Driving both high is very likely correct but is
a guess until the board says otherwise. Resolve it empirically and record the
result here.

## Module LEDs

`WIFI_LED1_B` (D18) and `BT_LED2_B` (D19) are driven by the card, not the MCU —
a free visual smoke test.
