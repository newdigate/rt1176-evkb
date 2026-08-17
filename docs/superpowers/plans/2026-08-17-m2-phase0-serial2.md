# M.2 Phase 0 + B1 (Serial2 on LPUART2) Implementation Plan

> ## ⛔ DEFERRED 2026-08-17 — do not execute tasks 2-8 as written
>
> **Task 1 is done** (`8dcae1d`, corrected in `89f0ad3`). Everything after it is
> on hold, and parts of it are now known to be wrong:
>
> * **Flow control must be removed, not implemented.** `R1866` (fitted) ties
>   `BT_UART_RTS` to `ETHPHY_RST_B` on the RTL8211FDI gigabit PHY. Calling
>   `setFlowControl(true)` — which Task 2's gate firmware does in `setup()` —
>   **holds the PHY in reset**. Do not flash that firmware to the board.
>   Task 4's `flowpins_t`, Task 5's `setFlowControl()` and Task 6's
>   `UART2_FlowPins` are all superseded.
> * **Serial2 has no consumer.** `R1901` is DNP, so the Bluetooth link is
>   transmit-only and Track B is parked. `Serial2` itself remains correct and
>   buildable; it is deferred under YAGNI, not cancelled.
> * Task 8's premise that `BT_DISABLE#` must be asserted to stop the module
>   fighting the loopback jumper is **unnecessary** — with `R1901` unpopulated
>   the module cannot drive that pad at all.
>
> If Track B is revived by fitting `R1901`, re-derive tasks 2-8 from the updated
> spec rather than executing this file. Board facts: `docs/m2-evkb-revc3.md`.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Document the M.2/J54 board wiring, and add `Serial2` (LPUART2 — the
M2-MAYA-W161 Bluetooth HCI UART) with hardware CTS/RTS flow control to the
`imxrt1176` core, proven by a new QEMU gate.

**Architecture:** LPUART2 reaches the M.2 card on `GPIO_DISP_B2_10/11/12/13`
(TXD/RXD ALT2, CTS_B/RTS_B ALT3). The core already has a generic
`HardwareSerialIMXRT` driver plus a small per-instance table file
(`HardwareSerial1.cpp`, 57 lines); `Serial2` follows that shape exactly. Flow
control was deliberately dropped from this port ("YAGNI", `HardwareSerial.h:38`)
and comes back as an opt-in `setFlowControl()` with the CTS/RTS pads carried in
an optional sub-struct, so `Serial1` is untouched.

**Tech Stack:** C++ (Arduino/Teensyduino core), CMake + ARM GCC 10, QEMU
`mimxrt1170-evk` machine, POSIX-sh gates via `tools/gate-lib.sh`.

**Spec:** `docs/superpowers/specs/2026-08-17-m2-maya-w161-design.md`

**Out of scope (later plans):** W1 SDIO probe, firmware download, HCI.

---

## File structure

| File | Responsibility |
|---|---|
| `docs/m2-evkb-revc3.md` (create) | Board facts: J54↔MCU map, DNP findings, the uSDHC1 sharing landmine |
| `~/Development/teensy-cores/imxrt1176/imxrt1176.h` (modify) | LPUART2 pad/clock/MODIR register defines |
| `~/Development/teensy-cores/imxrt1176/HardwareSerial.h` (modify) | `flowpins_t`, `hardware_t.flow`, `setFlowControl()`, `Serial2` externs |
| `~/Development/teensy-cores/imxrt1176/HardwareSerial.cpp` (modify) | No-daisy dummy target, `setFlowControl()`, per-instance ISR counter fix |
| `~/Development/teensy-cores/imxrt1176/HardwareSerial2.cpp` (create) | The `Serial2` instance table + ISR trampoline |
| `examples/serial/serial2_test/serial2_test.cpp` (create) | Firmware exercising Serial2 + flow control |
| `examples/serial/serial2_test/CMakeLists.txt` (create) | Build |
| `examples/serial/serial2_test/run_qemu.sh` (create) | The gate |
| `tools/license-audit.sh` (modify) | `GATES` entry for the new example |
| `evkb.cmake` (modify) | Bump the `cores` pin after the core work is pushed |

**Note on the core repo:** `teensy-cores` is a **sibling checkout**, not part of
this repo. Core commits happen in `~/Development/teensy-cores` and are pushed
separately; `evkb.cmake:110` pins its SHA.

---

## Task 1: Board-facts document

No code. This is the artefact another session working on SD needs.

**Files:**
- Create: `docs/m2-evkb-revc3.md`

- [ ] **Step 1: Write the document**

Create `docs/m2-evkb-revc3.md` with exactly this content:

```markdown
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
```

- [ ] **Step 2: Verify the tables against the netlist**

Run, from the repo root:

```bash
python3 - <<'PY'
import re
p="/Users/nicholasnewdigate/Development/rt1170/MIMXRT1170-EVKB-DESIGNFILES_RevC3/pst2kicad/board.net"
s=open(p).read()
nets={m.group(1): re.findall(r'\(node \(ref "([^"]+)"\) \(pin "([^"]+)"\)', m.group(2))
      for m in re.finditer(r'\(net \(code "?\d+"?\) \(name "([^"]+)"\)(.*?)\n    \)', s, re.S)}
for n in ["BT_UART_TXD","BT_UART_RXD","BT_UART_CTS","BT_UART_RTS","SPDIF_IN","SPDIF_OUT"]:
    print(n, "->", ", ".join(f"{r}.{q}" for r,q in nets[n]))
PY
```

Expected: `BT_UART_TXD` includes `U19.D9` and `R3.2`; `BT_UART_RXD` includes
`U19.A6` and `R2.2`; `BT_UART_CTS` includes `U19.B6` and `R8.2`;
`BT_UART_RTS` includes `U19.A5`; `SPDIF_IN` includes `U19.M14`; `SPDIF_OUT`
includes `U19.N17`.

- [ ] **Step 3: Commit**

```bash
git add docs/m2-evkb-revc3.md
git commit -m "docs: M.2 J54 wiring map for the EVKB RevC3

uSDHC1 carries BOTH the microSD slot and the M.2 socket with every series
resistor fitted, so Wi-Fi and the SD card are mutually exclusive. R404 is DNP,
so W_DISABLE1# has no MCU control. BT UART is LPUART2 on the D0/D1/D2 pads."
```

---

## Task 2: The failing gate (example + `run_qemu.sh`)

Written **first**, so red→green is real. It fails at build with an undeclared
`Serial2`, then goes green in Task 6.

**Files:**
- Create: `examples/serial/serial2_test/serial2_test.cpp`
- Create: `examples/serial/serial2_test/CMakeLists.txt`
- Create: `examples/serial/serial2_test/run_qemu.sh`

- [ ] **Step 1: Write the firmware**

Create `examples/serial/serial2_test/serial2_test.cpp`:

```cpp
// Serial2 (LPUART2) gate.  LPUART2 is the M.2 (J54) Bluetooth HCI UART on the
// MIMXRT1170-EVKB -- GPIO_DISP_B2_10/11 TXD/RXD ALT2, DISP_B2_12/13 CTS/RTS
// ALT3.  See docs/m2-evkb-revc3.md.
//
// QEMU binds the Nth -serial to LPUART(N+1), so the console (LPUART1) lands in
// slot 0 and LPUART2 in slot 1 -- two separate capture files, which is what
// makes this gate REAL rather than absent-fallback: the bytes have to leave
// LPUART2 specifically.
#include <Arduino.h>

void setup() {
    Serial1.begin(115200);
    while (!Serial1) {}
    Serial1.println("RT1176 Serial2 gate up");

    Serial2.begin(115200);
    // Flow control is opt-in and must follow begin() -- begin() rewrites CTRL
    // but never touches MODIR, so the order here is the safe one.
    bool fc = Serial2.setFlowControl(true);
    Serial1.print("flowcontrol=");
    Serial1.println(fc ? "yes" : "no");

    // Serial1 must NOT be able to turn flow control on: it has no CTS/RTS pads
    // in its hardware table, so setFlowControl() has to refuse.  This is the
    // assertion that proves the opt-in is really opt-in rather than a no-op
    // that returns true for everyone.
    Serial1.print("serial1_flowcontrol=");
    Serial1.println(Serial1.setFlowControl(true) ? "yes" : "no");
}

void loop() {
    static uint32_t n = 0;
    Serial2.print("uart2 count=");
    Serial2.println(n);
    Serial1.print("tick=");
    Serial1.println(n);
    n++;
    delay(100);
}
```

- [ ] **Step 2: Write the build file**

Create `examples/serial/serial2_test/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.24)
project(serial2_test)

# TEENSY_VERSION / CPU_CORE_SPEED / COMPILERPATH are supplied by the toolchain
# file (../../../toolchain/rt1170-evkb.toolchain.cmake). Keep a fallback here
# so a bare `cmake -B build .` without the toolchain file still selects 117.
if(NOT DEFINED TEENSY_VERSION)
    set(TEENSY_VERSION 117 CACHE STRING "")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

teensy_add_executable(serial2_test serial2_test.cpp)
teensy_target_link_libraries(serial2_test cores)

target_link_libraries(serial2_test.elf stdc++)
```

- [ ] **Step 3: Write the gate**

Create `examples/serial/serial2_test/run_qemu.sh`:

```sh
#!/bin/sh
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/serial2_test.elf"
OUT=$(gate_capture_path "$DIR" serial.uart)
OUT2=$(gate_capture_path "$DIR" uart2.uart)
rm -f "$OUT" "$OUT2"
# gate_console emits the slot-0 chain (LPUART1 = the console on both boards).
# Appending one more -serial puts LPUART2 in slot 1.  Do NOT hardcode the whole
# chain -- see the gate-lib.sh comment about the rt1062 slot mistake.
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none $(gate_console "$OUT") -serial "file:$OUT2" \
    -d guest_errors -D "$(gate_capture_path "$DIR" serial.dbg)" &
P=$!; gate_pid $P
# Poll for the last token rather than sleeping a fixed duration -- a fixed sleep
# makes the gate load-sensitive and produces reds that say nothing about the
# firmware.  This firmware free-runs, so the terminal condition is the token.
for _ in $(seq 1 40); do
    [ -f "$OUT2" ] && grep -q "uart2 count=3" "$OUT2" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT"
gate_require_capture "$OUT2"
echo "==== captured LPUART1 ===="; cat "$OUT"
echo "==== captured LPUART2 ===="; cat "$OUT2"
grep -q "RT1176 Serial2 gate up" "$OUT" || { echo "FAIL: banner missing"; exit 1; }
# Flow control must be ON for Serial2 and REFUSED for Serial1.  Asserting only
# the first would pass against a setFlowControl() that ignores its hardware
# table and always returns true.
grep -q "^flowcontrol=yes" "$OUT" || { echo "FAIL: Serial2 flow control not enabled"; exit 1; }
grep -q "^serial1_flowcontrol=no" "$OUT" || { echo "FAIL: Serial1 wrongly accepted flow control"; exit 1; }
# The point of the gate: bytes must come out of LPUART2 itself, not the console.
grep -q "uart2 count=3" "$OUT2" || { echo "FAIL: no LPUART2 output"; exit 1; }
if grep -q "uart2 count=" "$OUT"; then
    echo "FAIL: LPUART2 output leaked onto the console -- wrong -serial slot"; exit 1
fi
echo "PASS: LPUART2 verified on its own serial slot"
```

- [ ] **Step 4: Make the gate executable**

```bash
chmod +x examples/serial/serial2_test/run_qemu.sh
```

- [ ] **Step 5: Run the build and watch it fail for the right reason**

```bash
cd examples/serial/serial2_test && cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake && cmake --build build
```

Expected: **FAIL**, compile error naming `Serial2` — `'Serial2' was not declared
in this scope`. If it fails with anything else (missing toolchain, CMake error),
fix that first: this red must be the missing `Serial2`, nothing else.

- [ ] **Step 6: Commit the red**

```bash
git add examples/serial/serial2_test
git commit -m "test: failing Serial2 (LPUART2) gate

Asserts bytes leave LPUART2 on its own -serial slot, that flow control is
enabled for Serial2, and that Serial1 REFUSES it -- the last one is what stops
a setFlowControl() that always returns true from passing. Red: Serial2 does
not exist yet."
```

---

## Task 3: Core — LPUART2 register defines

**Files:**
- Modify: `~/Development/teensy-cores/imxrt1176/imxrt1176.h`

- [ ] **Step 1: Confirm the addresses you are about to add**

The existing header documents the CCM strides at `imxrt1176.h:1778`:
ROOT n @ `0x40CC0000 + n*0x80`, LPCG n DIRECT @ `0x40CC6000 + n*0x20`.
`Serial1` uses ROOT25 (`0x40CC0C80`) and LPCG86 (`0x40CC6AC0`); LPUART2 is one
index past each.

```bash
python3 -c "print(hex(0x40CC0000+26*0x80), hex(0x40CC6000+87*0x20))"
```

Expected: `0x40cc0d00 0x40cc6ae0`

Pad registers follow the RM's IOMUXC offset table (`SW_MUX_CTL_PAD_GPIO_DISP_B2_13`
at `248h`, `SW_PAD_CTL_PAD_GPIO_DISP_B2_13` at `48Ch`, IOMUXC base `0x400E8000`),
continuing the `DISP_B2_10/11/12` defines already at `imxrt1176.h:764-769`.

- [ ] **Step 2: Add the defines**

In `~/Development/teensy-cores/imxrt1176/imxrt1176.h`, immediately after the
existing `IOMUXC_SW_PAD_CTL_PAD_GPIO_DISP_B2_12` line (currently line 769), add:

```c
/* LPUART2 = the M.2 (J54) Bluetooth HCI UART on the MIMXRT1170-EVKB.
 * TXD GPIO_DISP_B2_10 ALT2, RXD GPIO_DISP_B2_11 ALT2,
 * CTS_B GPIO_DISP_B2_12 ALT3, RTS_B GPIO_DISP_B2_13 ALT3.
 * LPUART2 has NO *_SELECT_INPUT daisy register: the IOMUXC daisy list covers
 * only LPUART1/7/8/10 (plus 11/12 in the LPSR block), so each LPUART2 signal
 * has exactly one pad option and no routing write is possible or needed.
 * Root/LPCG follow the strides documented further down this file
 * (ROOT n @ 0x40CC0000 + n*0x80; LPCG n DIRECT @ 0x40CC6000 + n*0x20), one
 * index past LPUART1's ROOT25/LPCG86.  Board map: docs/m2-evkb-revc3.md. */
#define IOMUXC_SW_MUX_CTL_PAD_GPIO_DISP_B2_13 (*(volatile uint32_t *)0x400E8248u)
#define IOMUXC_SW_PAD_CTL_PAD_GPIO_DISP_B2_13 (*(volatile uint32_t *)0x400E848Cu)
#define CCM_CLOCK_ROOT26_CONTROL (*(volatile uint32_t *)0x40CC0D00u)
#define CCM_LPCG87_DIRECT        (*(volatile uint32_t *)0x40CC6AE0u)

/* LPUART MODIR (flow control).  Bit positions taken verbatim from the teensy4
 * core's imxrt.h in this same repo (cores/teensy4/imxrt.h:7546-7554) rather
 * than hand-decoded from the RM's PDF tables, which do not extract cleanly. */
#define LPUART_MODIR_TXCTSE   (1u << 0)
#define LPUART_MODIR_TXRTSE   (1u << 1)
#define LPUART_MODIR_TXRTSPOL (1u << 2)
#define LPUART_MODIR_RXRTSE   (1u << 3)
```

- [ ] **Step 3: Verify the symbols are present and nothing else changed**

```bash
cd ~/Development/teensy-cores && git diff --stat && \
for s in IOMUXC_SW_MUX_CTL_PAD_GPIO_DISP_B2_13 IOMUXC_SW_PAD_CTL_PAD_GPIO_DISP_B2_13 \
         CCM_CLOCK_ROOT26_CONTROL CCM_LPCG87_DIRECT LPUART_MODIR_TXCTSE LPUART_MODIR_RXRTSE; do
  grep -q "define $s" imxrt1176/imxrt1176.h || echo "MISSING: $s"; done; echo "check done"
```

Expected: `git diff --stat` shows only `imxrt1176/imxrt1176.h` changed, and the
loop prints only `check done`.

- [ ] **Step 4: Commit**

```bash
cd ~/Development/teensy-cores
git add imxrt1176/imxrt1176.h
git commit -m "imxrt1176: LPUART2 pad/clock defines + LPUART_MODIR bits

For Serial2 = the M.2 (J54) Bluetooth HCI UART. DISP_B2_13 mux/pad, CCM
ROOT26/LPCG87, and the MODIR flow-control bits. LPUART2 has no IOMUXC daisy
register, so none is emitted."
```

---

## Task 4: Core — header changes

**Files:**
- Modify: `~/Development/teensy-cores/imxrt1176/HardwareSerial.h`

- [ ] **Step 1: Add the flow-control pin struct and the `hardware_t` field**

In `HardwareSerial.h`, replace the `hardware_t` typedef (currently at lines
120-133, inside `class HardwareSerialIMXRT`'s first `public:` block) with:

```cpp
public:
	// CTS/RTS pads, carried separately because most instances have none.  A
	// null `flow` pointer in hardware_t is what makes setFlowControl() refuse.
	typedef struct {
		volatile uint32_t &cts_mux_reg;	uint32_t cts_mux_val;	volatile uint32_t &cts_pad_reg;
		volatile uint32_t &rts_mux_reg;	uint32_t rts_mux_val;	volatile uint32_t &rts_pad_reg;
	} flowpins_t;
	typedef struct {
		uint8_t serial_index;			// which object are we? 0 based
		IRQ_NUMBER_t irq;
		void (*irq_handler)(void);
		void (*_serialEvent)(void);
		volatile uint32_t &lpcg_register;	// CCM->LPCG[n].DIRECT
		volatile uint32_t &clock_root_reg;	// CCM->CLOCK_ROOT[n].CONTROL
		uint32_t clock_root_val;		// mux|div for 24 MHz
		volatile uint32_t &tx_mux_reg;	uint32_t tx_mux_val;	volatile uint32_t &tx_pad_reg;
		volatile uint32_t &rx_mux_reg;	uint32_t rx_mux_val;	volatile uint32_t &rx_pad_reg;
		volatile uint32_t &rx_select_input_reg;	uint32_t rx_select_input_val;
		uint16_t irq_priority;
		const flowpins_t *flow;			// nullptr = no CTS/RTS pads on this instance
	} hardware_t;
```

- [ ] **Step 2: Declare `setFlowControl` on the driver class**

In the same file, immediately after the `operator bool()` line
(`operator bool()			{ return true; }`), add:

```cpp
	// Enable hardware CTS/RTS.  Returns false (and changes nothing) when this
	// instance has no CTS/RTS pads.  Call AFTER begin(): begin() rewrites CTRL
	// but never touches MODIR, so this ordering is the safe one.
	bool setFlowControl(bool enable);
```

- [ ] **Step 3: Declare the Serial2 instance and its ISR**

Change the existing `extern "C"` block near the top of the `__cplusplus`
section from:

```cpp
extern "C" {
	extern void IRQHandler_Serial1();
}
```

to:

```cpp
extern "C" {
	extern void IRQHandler_Serial1();
	extern void IRQHandler_Serial2();
}
```

Change the friend declaration at the end of `class HardwareSerialIMXRT` from:

```cpp
	friend void IRQHandler_Serial1();
```

to:

```cpp
	friend void IRQHandler_Serial1();
	friend void IRQHandler_Serial2();
```

And after the existing `extern void serialEvent1(void);` line, add:

```cpp
// Serial2 hardware serial port (LPUART2 = the M.2 socket J54 Bluetooth HCI
// UART; TXD/RXD on GPIO_DISP_B2_10/11, CTS/RTS on DISP_B2_12/13).  These pads
// are also Arduino header D0/D1/D2 -- see docs/m2-evkb-revc3.md.
extern HardwareSerialIMXRT Serial2;
extern void serialEvent2(void);
```

- [ ] **Step 4: Update the stale port comment**

The header comment at `HardwareSerial.h:36-38` says flow control was dropped.
Replace:

```
// the abstract HardwareSerial base, the HardwareSerialIMXRT ring-buffer driver,
// and a single console instance (Serial1 on LPUART1).  RTS/CTS flow control,
// half-duplex, 9-bit mode, DMA, and XBAR triggering were dropped (YAGNI).
```

with:

```
// the abstract HardwareSerial base, the HardwareSerialIMXRT ring-buffer driver,
// and two instances: Serial1 (LPUART1, the EVKB VCOM console) and Serial2
// (LPUART2, the M.2 socket's Bluetooth HCI UART).  Half-duplex, 9-bit mode,
// DMA, and XBAR triggering are still dropped (YAGNI).  RTS/CTS flow control is
// back, but opt-in via setFlowControl() and only on instances whose hardware_t
// carries a `flow` pin set -- Bluetooth HCI needs it at 3 Mbaud.
```

- [ ] **Step 5: Commit**

```bash
cd ~/Development/teensy-cores
git add imxrt1176/HardwareSerial.h
git commit -m "imxrt1176: declare Serial2 and opt-in setFlowControl()

flowpins_t is a separate sub-struct hung off hardware_t so instances without
CTS/RTS pads carry a null pointer and setFlowControl() refuses, rather than
every instance growing six unused reference fields."
```

---

## Task 5: Core — driver implementation

**Files:**
- Modify: `~/Development/teensy-cores/imxrt1176/HardwareSerial.cpp`

- [ ] **Step 1: Add the no-daisy dummy target**

`configure_hardware()` writes `rx_select_input_reg` unconditionally, and
`hardware_t` holds it as a reference, which cannot be null. LPUART2 has no
daisy register, so it needs a harmless target.

After the existing `volatile uint32_t serial1_rx_isr_count = 0;` line
(`HardwareSerial.cpp:37`), add:

```cpp
// Write target for instances with no IOMUXC *_SELECT_INPUT daisy register.
// LPUART2's signals each have exactly one pad option, so there is no routing
// register to write; configure_hardware() writes here instead of branching.
volatile uint32_t iomuxc_no_daisy = 0;
```

- [ ] **Step 2: Stop Serial2 polluting the Serial1 diagnostic counter**

`serial1_rx_isr_count` lives in the *shared* `IRQHandler`, so a second instance
would bump it too and `serial_test_rx`'s assertion would stop meaning what it
says. At `HardwareSerial.cpp:246`, change:

```cpp
		serial1_rx_isr_count++;   // diagnostic: an RX-servicing pass ran in the ISR
```

to:

```cpp
		// diagnostic: an RX-servicing pass ran in the ISR.  Guarded by index so
		// this stays a SERIAL1 counter now that a second instance exists --
		// serial_test_rx asserts on it.
		if (hardware->serial_index == 0) serial1_rx_isr_count++;
```

- [ ] **Step 3: Implement `setFlowControl`**

After the closing brace of `HardwareSerialIMXRT::end(void)`, add:

```cpp
bool HardwareSerialIMXRT::setFlowControl(bool enable)
{
	const flowpins_t *fp = hardware->flow;
	if (fp == nullptr) return false;   // no CTS/RTS pads on this instance

	IMXRT_LPUART_t *port = (IMXRT_LPUART_t *)port_addr;
	if (enable) {
		// Same PAD_CFG the SDK pin_mux.c writes for the console pads; RT1176
		// pad-field polarity is non-obvious, so reuse the known-good value
		// rather than hand-decoding bits (see configure_hardware()).
		const uint32_t PAD_CFG = 0x02u;
		fp->cts_mux_reg = fp->cts_mux_val;  fp->cts_pad_reg = PAD_CFG;
		fp->rts_mux_reg = fp->rts_mux_val;  fp->rts_pad_reg = PAD_CFG;
		// TXCTSE: gate transmission on CTS.  RXRTSE: assert RTS from the RX
		// FIFO watermark.  Together these are what an HCI controller expects.
		port->MODIR = LPUART_MODIR_TXCTSE | LPUART_MODIR_RXRTSE;
	} else {
		port->MODIR = 0;
	}
	return true;
}
```

- [ ] **Step 4: Verify it still compiles against the existing gates**

```bash
cd ~/Development/rt1176-evkb-m2-maya-w161/examples/serial/serial_test && \
  rm -rf build && cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake && \
  cmake --build build && ./run_qemu.sh
```

Expected: `PASS: QEMU serial output verified` — Serial1 is unaffected by any of
this.

- [ ] **Step 5: Commit**

```bash
cd ~/Development/teensy-cores
git add imxrt1176/HardwareSerial.cpp
git commit -m "imxrt1176: setFlowControl(), no-daisy write target, per-index ISR counter

serial1_rx_isr_count lives in the shared IRQHandler, so a second instance would
have silently inflated it and broken what serial_test_rx asserts."
```

---

## Task 6: Core — the `Serial2` instance (gate goes green here)

**Files:**
- Create: `~/Development/teensy-cores/imxrt1176/HardwareSerial2.cpp`

- [ ] **Step 1: Write the instance table**

Create `~/Development/teensy-cores/imxrt1176/HardwareSerial2.cpp` with the same
MIT/PJRC licence header as `HardwareSerial1.cpp` (copy lines 1-29 of that file
verbatim), then:

```cpp
#include "HardwareSerial.h"
#include "core_pins.h"

#define IRQ_PRIORITY 64  // 0 = highest priority, 255 = lowest

void IRQHandler_Serial2();
static uint8_t tx_buffer2[64];
static uint8_t rx_buffer2[64];

extern volatile uint32_t iomuxc_no_daisy;

// CTS_B / RTS_B are ALT3 on GPIO_DISP_B2_12/13.  Muxed only when
// setFlowControl(true) is called, so a sketch that does not want flow control
// leaves those two pads (Arduino header D2 and its neighbour) alone.
static const HardwareSerialIMXRT::flowpins_t UART2_FlowPins = {
	IOMUXC_SW_MUX_CTL_PAD_GPIO_DISP_B2_12, 3u, IOMUXC_SW_PAD_CTL_PAD_GPIO_DISP_B2_12, // CTS_B ALT3
	IOMUXC_SW_MUX_CTL_PAD_GPIO_DISP_B2_13, 3u, IOMUXC_SW_PAD_CTL_PAD_GPIO_DISP_B2_13, // RTS_B ALT3
};

const HardwareSerialIMXRT::hardware_t UART2_Hardware = {
	1, IRQ_LPUART2, &IRQHandler_Serial2, &serialEvent2,
	CCM_LPCG87_DIRECT,                        // lpcg_register  (0x40CC6AE0)
	CCM_CLOCK_ROOT26_CONTROL,                 // clock_root_reg (0x40CC0D00)
	(0u /*mux OscRC48MDiv2*/ | 0u /*div=1*/), // clock_root_val -> 24 MHz
	IOMUXC_SW_MUX_CTL_PAD_GPIO_DISP_B2_10, 2u, IOMUXC_SW_PAD_CTL_PAD_GPIO_DISP_B2_10, // TXD ALT2
	IOMUXC_SW_MUX_CTL_PAD_GPIO_DISP_B2_11, 2u, IOMUXC_SW_PAD_CTL_PAD_GPIO_DISP_B2_11, // RXD ALT2
	iomuxc_no_daisy, 0u,                      // LPUART2 has no RXD daisy register
	IRQ_PRIORITY,
	&UART2_FlowPins,
};

HardwareSerialIMXRT Serial2(0x40080000, &UART2_Hardware, tx_buffer2, sizeof(tx_buffer2),
                            rx_buffer2, sizeof(rx_buffer2));

void IRQHandler_Serial2() { Serial2.IRQHandler(); }

void serialEvent2() __attribute__((weak));
void serialEvent2() {}
```

- [ ] **Step 2: Add the `flow` field to the Serial1 table**

`hardware_t` gained a field, so `UART1_Hardware` needs it too. In
`~/Development/teensy-cores/imxrt1176/HardwareSerial1.cpp`, change:

```cpp
	IOMUXC_LPUART1_RXD_SELECT_INPUT, 0u,      // rx daisy -> GPIO_AD_25
	IRQ_PRIORITY,
};
```

to:

```cpp
	IOMUXC_LPUART1_RXD_SELECT_INPUT, 0u,      // rx daisy -> GPIO_AD_25
	IRQ_PRIORITY,
	nullptr,                                  // no CTS/RTS pads on the console
};
```

- [ ] **Step 3: Build the gate's example**

```bash
cd ~/Development/rt1176-evkb-m2-maya-w161/examples/serial/serial2_test && \
  rm -rf build && cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake && \
  cmake --build build
```

Expected: builds clean, produces `build/serial2_test.elf`.

- [ ] **Step 4: Run the gate — this is the green**

```bash
cd ~/Development/rt1176-evkb-m2-maya-w161/examples/serial/serial2_test && ./run_qemu.sh
```

Expected: `PASS: LPUART2 verified on its own serial slot`.

If `flowcontrol=no`: `UART2_Hardware.flow` is null — check the `&UART2_FlowPins`
line. If LPUART2 output is empty but the console banner appears: the `-serial`
slot is wrong, not the firmware — re-read the slot comment in `gate-lib.sh`
before touching driver code.

- [ ] **Step 5: Re-run the Serial1 gates to prove nothing regressed**

```bash
cd ~/Development/rt1176-evkb-m2-maya-w161/examples/serial/serial_test && ./run_qemu.sh && \
cd ../serial_test_rx && ./run_qemu.sh
```

Expected: both PASS.

- [ ] **Step 6: Commit and push the core**

```bash
cd ~/Development/teensy-cores
git add imxrt1176/HardwareSerial2.cpp imxrt1176/HardwareSerial1.cpp
git commit -m "imxrt1176: add Serial2 on LPUART2 (M.2 Bluetooth HCI UART)

TXD/RXD GPIO_DISP_B2_10/11 ALT2, CTS/RTS DISP_B2_12/13 ALT3 behind
setFlowControl(). No RXD daisy register exists for LPUART2, so the table points
at the shared no-daisy write target."
git push origin master
```

- [ ] **Step 7: Bump the core pin**

Take the pushed SHA and update `evkb.cmake:110` — replace
`5bcae781b6c0e451f073298ddf7e1cd859f3e4de` with it.

```bash
cd ~/Development/teensy-cores && git rev-parse HEAD
```

Then edit `evkb.cmake:110` and verify the fetch path works from a clean fetch:

```bash
cd ~/Development/rt1176-evkb-m2-maya-w161/examples/serial/serial2_test && \
  rm -rf build-fetch && cmake -B build-fetch -DEVKB_FORCE_FETCH=ON \
  -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake && \
  cmake --build build-fetch
```

Expected: builds clean from the GitHub-fetched core. Then `rm -rf build-fetch`.

```bash
cd ~/Development/rt1176-evkb-m2-maya-w161
git add evkb.cmake
git commit -m "build: bump cores pin -- Serial2 on LPUART2"
```

---

## Task 7: Register the gate and re-measure the sweep

**Files:**
- Modify: `tools/license-audit.sh:245+` (the `GATES` list)

- [ ] **Step 1: Add the GATES entry**

In `tools/license-audit.sh`, inside the `GATES=` string, add
`examples/serial/serial2_test:serial2_test` next to the existing
`examples/serial/serial_test:serial_test` entry, preserving the trailing
backslash line-continuation style of the surrounding lines.

- [ ] **Step 2: Run the licence audit**

```bash
cd ~/Development/rt1176-evkb-m2-maya-w161 && ./tools/license-audit.sh
```

Expected: `LICENSE-AUDIT: PASS`. A failure naming `serial2_test` means the
GATES drift check found the example has no entry — that is the check working;
fix the entry rather than adding an exemption.

- [ ] **Step 3: Confirm the gate is discovered**

```bash
./tools/run-all-qemu-gates.sh -l | grep serial2
```

Expected: `rt1176:serial/serial2_test`. (Discovery globs `run_qemu*.sh` and
prunes `build*`; if nothing appears, check the file is executable.)

- [ ] **Step 4: Build everything, then run the full sweep**

A missing ELF reports as SKIP, not failure, so an unbuilt tree silently
under-measures. Read `docs/KNOWN-BROKEN-GATES.md` first.

```bash
cd ~/Development/rt1176-evkb-m2-maya-w161 && ./tools/run-all-qemu-gates.sh
```

Expected: **95 passed, 0 failed, 0 SKIP** (94 + this gate), or 94/1/0 if the
nondeterministic `rt1176:dualcore/cm4_audio_test` is red — re-run that one idle
before believing it. Any other failure is a real regression from this work.

- [ ] **Step 5: Update the measured baseline in CLAUDE.md**

In `CLAUDE.md`, update the sweep count from 94 to the number you just measured,
adding `serial2_test` to the running list of what moved the count, and update
the `✅ Measured` line with today's date and the actual figures. Record what you
ran, not what you expect.

- [ ] **Step 6: Commit**

```bash
git add tools/license-audit.sh CLAUDE.md
git commit -m "test: gate serial/serial2_test; re-measure sweep baseline"
```

---

## Task 8: Hardware verification — **STOP, needs the board**

This task needs the EVKB, which another session shares. Do not start it without
confirming the board is free.

**Files:**
- Create: `examples/serial/serial2_test/transcript_hw_evkb.txt`
- Modify: `docs/m2-evkb-revc3.md` (record the reset-line finding)

- [ ] **Step 1: Confirm the board is free and clear stale probe daemons**

`pkill LinkServer` alone leaves `redlinkserv`/`crt_emu_cm_redlink` resident and
silently kills the next few runs — and this command will yank the board out from
under an active debug session, so confirm first.

```bash
pkill LinkServer; pkill redlinkserv; pkill crt_emu_cm_redlink
```

- [ ] **Step 2: Fit the loopback jumper**

Jumper **J9 pin 2 ↔ J9 pin 4** (D0/D1 = the LPUART2 RX/TX pads). The M.2 card
is fitted and its UART is on the same wires, so the firmware asserts
`BT_DISABLE#` (`GPIO_AD_15` low) in `setup()` before transmitting — otherwise
the module's TX fights the jumper. Add this to `serial2_test.cpp` before
`Serial2.begin()`:

`IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_15` does **not** exist in `imxrt1176.h` yet
(the file has `GPIO_AD_13`/`GPIO_AD_14` but stops there). Add it first, next to
those, using the RM's IOMUXC offsets — `SW_MUX_CTL_PAD_GPIO_AD_15` at `148h`,
`SW_PAD_CTL_PAD_GPIO_AD_15` at `38Ch`, IOMUXC base `0x400E8000`:

```c
/* GPIO_AD_15 -> M.2 (J54) pin 54 BT_DISABLE# via R209/R834.  ALT5 = GPIO3_IO14.
 * GPIO_AD_16 -> M.2 pin 23 WIFI_RST_B via R835.  ALT5 = GPIO3_IO15.
 * Board map: docs/m2-evkb-revc3.md. */
#define IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_15 (*(volatile uint32_t *)0x400E8148u)
#define IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_15 (*(volatile uint32_t *)0x400E838Cu)
#define IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_16 (*(volatile uint32_t *)0x400E814Cu)
#define IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_16 (*(volatile uint32_t *)0x400E8390u)
```

`GPIO3_GDIR` (`0x40134004`), `GPIO3_DR_SET` and `GPIO3_DR_CLEAR` already exist
(`imxrt1176.h:53-56`). Then add to `serial2_test.cpp`, before `Serial2.begin()`:

```cpp
    // GPIO_AD_15 -> M.2 pin 54 BT_DISABLE#, driven LOW to hold the module's
    // UART off the shared D0/D1 pads, so a loopback jumper measures LPUART2
    // alone rather than fighting the card's transmitter.
    IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_15 = 5u;   // ALT5 = GPIO3_IO14
    GPIO3_GDIR |= (1u << 14);
    GPIO3_DR_CLEAR = (1u << 14);
```

- [ ] **Step 3: Add the loopback read-back

The QEMU gate proves bytes leave LPUART2. Only the jumper proves they come back
in. Replace `loop()` in `serial2_test.cpp` with:

```cpp
void loop() {
    static uint32_t n = 0;
    while (Serial2.available()) Serial2.read();   // drain before transmitting

    Serial2.print("uart2 count=");
    Serial2.println(n);
    Serial2.flush();

    // With J9 pin 2 <-> pin 4 jumpered, everything just sent comes straight
    // back. Without the jumper this reads zero, which is the honest result on
    // a bare board and in QEMU -- so the QEMU gate must not assert on it.
    delay(20);
    int echoed = 0;
    char buf[64];
    while (Serial2.available() && echoed < (int)sizeof(buf) - 1) {
        buf[echoed++] = (char)Serial2.read();
    }
    buf[echoed] = '\0';
    Serial1.print("tick=");
    Serial1.print(n);
    Serial1.print(" loopback_bytes=");
    Serial1.print(echoed);
    Serial1.print(" echo=");
    Serial1.println(buf);
    n++;
    delay(100);
}
```

Rebuild and re-run `./run_qemu.sh` before flashing — it must still PASS, with
`loopback_bytes=0` in the console capture. If the gate now fails, the gate was
asserting something it should not have been.

- [ ] **Step 4: Flash, without holding the VCOM**

```bash
LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load build/serial2_test.elf
```

Then verify, **then** attach the reader, then reset — in that order. Holding the
VCOM during load produces `status 131` / `LOAD_EXIT=255` and the port
re-enumerates mid-attempt.

- [ ] **Step 5: Capture the console**

Find the port, then read it — macOS `cat` silently resets the port to 9600, so
use the pyserial reader:

```bash
ls /dev/cu.usbmodem*
```

```bash
python3 ../../../tools/rt1170-console.py /dev/cu.usbmodem0000000000001 115200
```

Expected: the banner, `flowcontrol=yes`, `serial1_flowcontrol=no`, and lines
reading `tick=N loopback_bytes=14 echo=uart2 count=N` — a non-zero
`loopback_bytes` is the assertion no QEMU run can produce.

- [ ] **Step 6: Save the transcript**

Save the captured session to
`examples/serial/serial2_test/transcript_hw_evkb.txt`, following the format of
the transcripts in neighbouring examples.

- [ ] **Step 7: Record the reset-line finding**

While the board is up, settle the open question from the spec: whether
`GPIO_AD_15` and `GPIO_AD_16` need a particular assertion order, and what pin 23
does. Update the "Reset lines" section of `docs/m2-evkb-revc3.md` with what the
board actually did, replacing the "unconfirmed" wording.

- [ ] **Step 8: Remove the jumper and commit**

```bash
cd ~/Development/rt1176-evkb-m2-maya-w161
git add examples/serial/serial2_test/transcript_hw_evkb.txt docs/m2-evkb-revc3.md
git commit -m "test: Serial2 hardware verification on the EVKB

Loopback across J9 pin 2/4 with the M.2 module held in reset. Records the
GPIO_AD_15/AD_16 assertion behaviour, which was a guess until now."
```

---

## Notes for whoever executes this

- **Two repos.** Tasks 3-6 commit in `~/Development/teensy-cores`; everything
  else in `~/Development/rt1176-evkb-m2-maya-w161`. Task 6 Step 7 is the seam —
  the core must be pushed before the pin bump means anything.
- **The gate is the test.** There is no unit-test framework here; red→green
  happens at `./run_qemu.sh`. Run gates as `./run_qemu.sh`, never
  `sh run_qemu.sh` — they re-exec themselves under `gtimeout`.
- **A green sweep does not prove a build directory can still configure.** Gates
  run cached ELFs. If a `COMPILERPATH is UNDEFINED` or missing-toolchain error
  appears, `rm -rf` that build dir and configure fresh.
