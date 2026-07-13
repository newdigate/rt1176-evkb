# MIMXRT1170-EVKB RevC3 Arduino header — verified physical pin map

**Source of truth:** RevC3 schematic `SPF-55139_c3.pdf` sheet 26 (INTERFACE/JTAG —
this is the Arduino socket page; the four headers J9/J10/J25/J26 live there) plus
the RevC3 BOM `SCH-55139_C3.xlsx` for resistor population.  Extracted
programmatically (pdfplumber word/line geometry + rendered-image review) on
2026-07-13.  **Verdict: `digital_pin_to_info` in `cores/imxrt1176/digital.c` is
correct as-is for D0–D15 and A0–A5.**  The bugs found were on the analog side
(see "Corrections made" below).

## The one fact that prevents fried boards

Each Arduino header is a **dual-row (2×N) connector.  Only the EVEN-numbered
pin column is the Arduino socket.**  The ODD column is the FRDM motor-control
interface (PWM/encoder/current-sense) or GND — it is NOT a duplicate of the
socket column:

* **J25 odd pins carry `GPIO_AD_00`…`AD_05` raw (no series resistor)** — the
  same pads as D6, D9-adjacent signals, D3 (the User LED) and D5 — directly
  beside the power pins.  On J25 the even column has **3V3 on pin 8 next to 5V
  on pin 10**: jumpering two adjacent "socket-looking" positions there shorts
  the rails, which is almost certainly what dropped the MCU rail during the
  2026-07-13 WProgram-parity jumper test.  D4 (`GPIO_AD_06`) **is** on the
  socket (J9 pin 10, via populated 0Ω R5); the earlier "D4 doesn't reach the
  socket" conclusion came from reading sheet *22* as the socket page — sheet 22
  is the SIM CARD page.  The `{7,22,26}` markers on sheet 26 are OrCAD flat-net
  cross-references (net also appears on sheets 7/22/26), not socket routing.
* On J9 the odd column is GND (pins 5–15) except pin 3 (`ENC_I` divider); on
  J10 the odd pins 1–9 are current/voltage-sense inputs (`CUR_A`…`CUR_DCB`,
  RC-filtered into `GPIO_AD_11/12/13` — the A1/A2/A3 pads!) and pins 13–19 GND.

## Digital socket, J9 (CON 2×8) — D0–D7

Every net reaches the socket through a **populated** 0Ω series resistor.

| Socket | J9 pin | 0Ω  | Pad (net)         | GPIO (ALT5)  | Shared with (as built) |
|--------|--------|-----|-------------------|--------------|------------------------|
| D0/UART_RX | 2  | R2  | `GPIO_DISP_B2_11` | GPIO5.12     | sheet 23 (M.2); DNP R1889→LPUART12_TXD |
| D1/UART_TX | 4  | R3  | `GPIO_DISP_B2_10` | GPIO5.11     | sheet 23; DNP R1888→LPUART12_RXD; DNP R1841 divider to J25.1 |
| D2/INT0    | 6  | R8  | `GPIO_DISP_B2_12` | GPIO5.13     | sheet 23; ENC_I divider (R1831/32, DNP R1839) |
| D3/INT1/PWM| 8  | R1  | `GPIO_AD_04`      | GPIO3.3      | **User LED**; J25 pin 7 (PWM_CT, raw); SIM sheet 22 |
| D4/T0      | 10 | R5  | `GPIO_AD_06`      | GPIO3.5      | nothing else (cross-ref `{7}` only) |
| D5/T1/PWM  | 12 | R4  | `GPIO_AD_05`      | GPIO3.4      | J25 pin 5 (PWM_CB, raw); SIM sheet 22 |
| D6/AIN0/PWM| 14 | R6  | `GPIO_AD_00`      | GPIO2.31     | J25 pin 15 (PWM_AT, raw); SIM sheet 22 |
| D7/AIN1    | 16 | R7  | `GPIO_AD_14`      | GPIO3.13     | sheet 17 (SPDIF) |

J9 odd column: 3 = `ENC_I`; 5,7,9,11,13,15 = GND; 1 = (see schematic, routes up).

## Digital socket, J10 (CON 2×10) — D8–D15, AREF

| Socket | J10 pin | 0Ω   | Pad (net)      | GPIO (ALT5) | Shared with |
|--------|---------|------|----------------|-------------|-------------|
| D8         | 2  | R9   | `GPIO_AD_07`   | GPIO3.6     | sheet 20 (USB OTG2_OC fn) |
| D9/PWM     | 4  | R10  | `GPIO_AD_01`   | GPIO3.0     | J25 pin 13 (PWM_AB, raw); SIM sheet 22 |
| D10/SPI_CS | 6  | R11  | `GPIO_AD_29`   | GPIO3.28    | sheet 12 |
| D11/MOSI   | 8  | R12  | `GPIO_AD_30`   | GPIO3.29    | sheet 12; DNP R1809/R1810 verticals |
| D12/MISO   | 10 | R13  | `GPIO_AD_31`   | GPIO3.30    | sheet 12 |
| D13/SCK    | 12 | R14  | `GPIO_AD_28`   | GPIO3.27    | sheet 12 |
| (GND pos.) | 14 | —    | not connected on MCU side | — | socket GND position is NC here |
| AREF       | 16 | —    | AREF network (from below)  | — | |
| D14/I2C_SDA| 18 | R359 | `GPIO_LPSR_04` | GPIO6.4     | **LPI2C5 SDA** — WM8962 codec bus (Wire2); sheets 9,15,23,27 |
| D15/I2C_SCL| 20 | R338 | `GPIO_LPSR_05` | GPIO6.5     | **LPI2C5 SCL** — same |

Note the R359/R338 wires jog down one row on the schematic — they land on pins
18/20 (SDA/SCL), *not* 14/16; verified from the rendered drawing, not just text
geometry.  J10 odd column: 1=`CUR_A` 3=`CUR_B` 5=`CUR_C` 7=`VOLT_DCB`
9=`CUR_DCB` (RC-filtered into AD_11/AD_12/AD_13), 13/15/17/19=GND.

## Analog socket, J26 (CON 2×6) — A0–A5

| Socket | J26 pin | 0Ω  | Pad (net)    | ADC input (dedicated, no mux) | GPIO (ALT5) |
|--------|---------|-----|--------------|-------------------------------|-------------|
| A0     | 2  | R278 | `GPIO_AD_10` | **ADC1 CH2A** | GPIO3.9  |
| A1     | 4  | R276 | `GPIO_AD_11` | **ADC1 CH2B** | GPIO3.10 |
| A2     | 6  | R245 | `GPIO_AD_12` | **ADC1 CH3A** (also ADC2 CH3A) | GPIO3.11 |
| A3     | 8  | R244 | `GPIO_AD_13` | **ADC1 CH3B** (also ADC2 CH3B) | GPIO3.12 |
| A4/SDA | 10 | R294 | `GPIO_AD_09` | **ADC1 CH1B** (LPI2C1 SDA = Wire) | GPIO3.8 |
| A5/SCL | 12 | R290 | `GPIO_AD_08` | **ADC1 CH1A** (LPI2C1 SCL = Wire) | GPIO3.7 |

J26 odd column: 1 = GND; 5/7/9/11 route to LPSPI6 via jumpers J63–J66
(default open).

## Power socket, J25 (CON 2×8) — ⚠ read before probing

| J25 even (socket) | Function | J25 odd (motor column) | Net (RAW, no resistor) |
|-------------------|----------|------------------------|------------------------|
| 2  | NC        | 1  | `ENC_B` (divider; DNP R1841 → `GPIO_DISP_B2_11`) |
| 4  | IOREF     | 3  | `ENC_A` (divider; DNP R1842 → `GPIO_DISP_B2_10`) |
| 6  | RESET_b   | 5  | `PWM_CB` = **`GPIO_AD_05` (= D5)** |
| 8  | **3V3**   | 7  | `PWM_CT` = **`GPIO_AD_04` (= D3 / User LED)** |
| 10 | **5V**    | 9  | `PWM_BB` = **`GPIO_AD_03`** |
| 12 | GND       | 11 | `PWM_BT` = **`GPIO_AD_02`** |
| 14 | GND       | 13 | `PWM_AB` = **`GPIO_AD_01` (= D9)** |
| 16 | VIN       | 15 | `PWM_AT` = **`GPIO_AD_00` (= D6)** |

## SIM card sharing (sheet 22)

`GPIO_AD_00…AD_05` (D6, D9, —, —, D3, D5) also route into the SIM circuit
through **populated** 0Ω resistors (R1904–R1912, R1811–R1813) — pull-ups/level
circuitry load these nets even with no SIM fitted (e.g. 4.7K R430).  `GPIO_AD_06`
(D4) is the *only* AD-pad digital pin with no second load anywhere.

## Corrections made to the core (2026-07-13)

1. **`analog.c`**: `analogRead(A0)` sampled **LPADC1 CH0A = `GPIO_AD_06` = the
   D4 pad**, not the A0 socket pin.  Table now carries all six pins with correct
   channel + A/B side (`ADC_CMDL_ABSEL`, CMDL bit 5).  QEMU gate updated
   (`A0=136`, `A4=68` under the synthetic channel*0x111 model) and green;
   **hardware re-verification still pending** (needs the EVKB: drive a known
   voltage into J26 pin 2 and read A0).
2. **`pins_arduino.h`**: `A1`–`A5` defined (17–21, `NUM_ANALOG_INPUTS=6`);
   stale "A0 = AD_06" comment fixed; `PIN_WIRE_SDA/SCL` corrected 18/19 → 20/21
   (classic-Uno A4/A5 in *this* core's numbering; stock-teensy4 18/19 lands on
   A2/A3 here).
3. **`digital.c` / PWM table: no change needed** — all 22 entries verified
   correct against schematic + BOM.
