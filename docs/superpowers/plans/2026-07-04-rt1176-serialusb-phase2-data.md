# RT1176 SerialUSB Phase 2 (USB CDC bulk data + Serial:Stream) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the enumerated CDC device move bytes — `Serial.print()/write()` reach the host, host bytes arrive via `Serial.read()/available()` — proven by an end-to-end **echo** in QEMU and on hardware.

**Architecture:** Port the compact Teensy 4 `usb_serial.c` (RX/TX ring buffers + `usb_serial_class : Stream`) into `cores/imxrt1176`, calling the transfer scheduler already built in the Phase-1 `usb.c`. Resolve the duplicate CDC line-state symbols (move ownership to `usb_serial.c`), put **all** bulk DMA structures in `DMAMEM`/OCRAM (RT1176 `.bss`=DTCM is DMA-unreachable), port the USB GPTIMER0 auto-flush timer, instantiate `Serial`+`SerialUSB`, and auto-init USB at startup. Gate in QEMU (the ChipIdea CDC virtual host bridges bulk data to a socket chardev bidirectionally) and on hardware (pyserial echo on the native-USB CDC port).

**Tech Stack:** C/C++ (Arduino core), ARM GCC 10.2.1 (`ARMGCC_DIR=/Applications/ARM_10`), CMake+Ninja, QEMU (`mimxrt1170-evk`, local build at `~/Development/qemu2/build`), LinkServer, Python 3 (socket echo driver + pyserial).

## Confirmed constants (extracted — do not re-derive)

- **Already ported & present in `cores/imxrt1176/usb.c`** (call, do not re-implement): `usb_config_rx/tx`, `usb_prepare_transfer`, `usb_transmit`, `usb_receive`, `usb_transfer_status`, `schedule_transfer`, `run_callbacks`, and the `endpointN_notify_mask` completion dispatch in `usb_isr`.
- **Endpoints** (`cores/imxrt1176/usb_desc.h`): `CDC_ACM_ENDPOINT 2` (interrupt-IN, `CDC_ACM_SIZE 16`), `CDC_RX_ENDPOINT 3` (bulk-OUT), `CDC_TX_ENDPOINT 4` (bulk-IN); `CDC_RX_SIZE_480 512`, `CDC_TX_SIZE_480 512`, `CDC_RX_SIZE_12 64`, `CDC_TX_SIZE_12 64`; `NUM_ENDPOINTS 4`. VID/PID `0x1209`/`0x0001`.
- **GPTIMER0** (from teensy4 `imxrt.h`, IP-identical): `USB1_GPTIMER0LD` = `USB_OTG1_BASE + 0x080`, `USB1_GPTIMER0CTRL` = `USB_OTG1_BASE + 0x084`; `USB_USBSTS_TI0 = 1<<24`, `USB_USBINTR_TIE0 = 1<<24`, `USB_GPTIMERCTRL_GPTRUN = 1<<31`, `USB_GPTIMERCTRL_GPTRST = 1<<30`. `USB_OTG1_BASE 0x40430000` (already in `imxrt1176.h:539`).
- **IRQ:** `IRQ_USB_OTG1 = 136` (`core_pins.h:56`). teensy4 `usb_serial.c` says `IRQ_USB1` → replace with `IRQ_USB_OTG1`.
- **D-cache is OFF** (Phase-1 HW-confirmed) → `arm_dcache_*` are no-ops; DMA structs go in `DMAMEM` (`#define DMAMEM __attribute__((section(".dmabuffers"), used))`, already in `imxrt1176.h`; `.dmabuffers` → OCRAM per `imxrt1176.ld`).
- **QEMU (unchanged):** `qemu2/hw/usb/chipidea_cdc.c` bridges data both ways; the `usbcdc` named chardev is wired in `qemu2/hw/arm/fsl-imxrt1170.c:~890`. The Phase-2 gate only swaps the runner's `-chardev null,id=usbcdc` for `-chardev socket,id=usbcdc,...`. Success log line: `CI-CDC: DTR asserted -> bridging USB serial`.
- **Ring sizes (teensy4 defaults, keep):** `RX_NUM 8`, `TX_NUM 4`, `TX_SIZE 2048`. OCRAM: `rx_buffer` 8×512 = 4 KB, `txbuffer` 4×2048 = 8 KB, `tx_transfer[4]`+`rx_transfer[8]` ≈ 384 B.
- **Build:** `import_arduino_library(cores …/cores/imxrt1176)` globs the whole core dir → new `.c`/`.cpp` are picked up with no CMake edit. Build a test: `export ARMGCC_DIR=/Applications/ARM_10 && cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake . && cmake --build build`.

---

### Task 1: `imxrt1176.h` — GPTIMER0 registers/bits + no-op D-cache inlines

**Files:**
- Modify: `cores/imxrt1176/imxrt1176.h` (append after the `USB1_ENDPTCTRL7` define, ~line 560)

- [ ] **Step 1: Add the GPTIMER0 defines and no-op cache inlines**

Insert this block immediately after the `USB1_ENDPTCTRL7` `#define` (line 560):

```c
/* USB GPTIMER0 — general-purpose one-shot; usb_serial TX auto-flush (Phase 2). */
#define USB1_GPTIMER0LD        (*(volatile uint32_t *)(USB_OTG1_BASE + 0x080))
#define USB1_GPTIMER0CTRL      (*(volatile uint32_t *)(USB_OTG1_BASE + 0x084))
#define USB_USBSTS_TI0             ((uint32_t)(1u<<24))
#define USB_USBINTR_TIE0          ((uint32_t)(1u<<24))
#define USB_GPTIMERCTRL_GPTRUN    ((uint32_t)(1u<<31))
#define USB_GPTIMERCTRL_GPTRST    ((uint32_t)(1u<<30))

/* D-cache is OFF in this core (Phase-1 HW-confirmed) → cache maintenance is a
 * no-op.  Provided so the teensy4 usb_serial.c port compiles verbatim.  If a
 * future build enables the D-cache, replace these with real CMSIS operations. */
static inline void arm_dcache_delete(void *addr, uint32_t size) { (void)addr; (void)size; }
static inline void arm_dcache_flush_delete(void *addr, uint32_t size) { (void)addr; (void)size; }
```

(If any of these macros already exist, remove the duplicate line rather than redefining.)

- [ ] **Step 2: Verify the additions**

Run: `grep -nE "USB1_GPTIMER0CTRL|USB_USBSTS_TI0|USB_GPTIMERCTRL_GPTRUN|arm_dcache_flush_delete" cores/imxrt1176/imxrt1176.h`
Expected: one match for each name.

- [ ] **Step 3: Confirm the core still builds (nothing uses these yet)**

Run:
```bash
cd ~/Development/rt1170/evkb/usb_enum_test && export ARMGCC_DIR=/Applications/ARM_10 && cmake --build build 2>&1 | tail -3
```
Expected: builds clean (`Linking … usb_enum_test.elf`), no errors.

- [ ] **Step 4: Commit**

```bash
cd ~/Development/rt1170/evkb/cores && git add imxrt1176/imxrt1176.h && git commit -m "imxrt1176: USB GPTIMER0 regs/bits + no-op arm_dcache_* (Phase 2 prep)"
```

---

### Task 2: `usb.c` / `usb_dev.h` — GPTIMER0 callback + ISR branch (additive)

**Files:**
- Modify: `cores/imxrt1176/usb.c` (add global near line 82; add ISR branch after the URI block, ~line 227)
- Modify: `cores/imxrt1176/usb_dev.h` (add extern)

- [ ] **Step 1: Add the callback global to `usb.c`**

After the line `volatile uint8_t usb_high_speed = 0;    // non-zero if running at 480 Mbit/sec speed` (line 82), add:

```c
void (*usb_timer0_callback)(void) = NULL; // USB GPTIMER0 one-shot (usb_serial flush timer)
```

- [ ] **Step 2: Add the TI0 branch to `usb_isr`**

In `usb_isr`, immediately after the `if (status & USB_USBSTS_URI) { … }` block closes (the `}` at line 227) and before `if (status & USB_USBSTS_PCI) {` (line 228), insert:

```c
	if (status & USB_USBSTS_TI0) {
		if (usb_timer0_callback != NULL) usb_timer0_callback();
	}
```

- [ ] **Step 3: Declare the extern in `usb_dev.h`**

After the line `uint32_t usb_transfer_status(const transfer_t *transfer);` (line 34), add:

```c
extern void (*usb_timer0_callback)(void);
```

- [ ] **Step 4: Build + confirm enumeration still works (callback is NULL → branch inert)**

Run:
```bash
cd ~/Development/rt1170/evkb/usb_enum_test && export ARMGCC_DIR=/Applications/ARM_10 && cmake --build build && ./run_qemu_usb.sh 2>&1 | tail -5
```
Expected: `USB=CONFIGURED` and `PASS: USB CDC enumeration verified`.

- [ ] **Step 5: Commit**

```bash
cd ~/Development/rt1170/evkb/cores && git add imxrt1176/usb.c imxrt1176/usb_dev.h && git commit -m "imxrt1176: usb.c GPTIMER0 callback + USBSTS_TI0 ISR branch (Phase 2)"
```

---

### Task 3: The port — `usb_serial.h` + `usb_serial.c` + `usb_inst.cpp`; retire `usb.c` line-state defs & stubs

This is the core task. It creates the three new files AND removes the now-conflicting definitions from `usb.c` in the **same commit** so the core stays linkable.

**Files:**
- Create: `cores/imxrt1176/usb_serial.h`
- Create: `cores/imxrt1176/usb_serial.c`
- Create: `cores/imxrt1176/usb_inst.cpp`
- Modify: `cores/imxrt1176/usb.c` (delete line-state defs 85-89 and stubs 107-109)

- [ ] **Step 1: Create `usb_serial.h` (port the single-CDC block, add SerialUSB)**

```bash
cp cores/teensy4/usb_serial.h cores/imxrt1176/usb_serial.h
```
Then edit `cores/imxrt1176/usb_serial.h`:
1. **Delete the CDC2 and CDC3 blocks:** remove everything from the line `#if defined(CDC2_STATUS_INTERFACE) && defined(CDC2_DATA_INTERFACE)` (teensy4 line 235) through end of file (line 417). The last retained line is the `#endif // CDC_STATUS_INTERFACE && CDC_DATA_INTERFACE` at teensy4 line 230.
2. **Add the `SerialUSB` alias declaration:** immediately after the line `extern usb_serial_class Serial;` inside the enabled C++ block (teensy4 line 188), add:

```cpp
// SerialUSB is an alias (reference) for the same USB CDC object as Serial.
extern usb_serial_class &SerialUSB;
```

- [ ] **Step 2: Verify `usb_serial.h` shape**

Run: `grep -nE "class usb_serial_class|extern usb_serial_class Serial;|extern usb_serial_class &SerialUSB;|CDC2_STATUS_INTERFACE" cores/imxrt1176/usb_serial.h`
Expected: `usb_serial_class` (one class), `Serial;` extern, `&SerialUSB;` extern, and **no** `CDC2_STATUS_INTERFACE` match.

- [ ] **Step 3: Create `usb_serial.c` from the teensy4 source**

```bash
cp cores/teensy4/usb_serial.c cores/imxrt1176/usb_serial.c
```

- [ ] **Step 4: Apply the port adaptations to `usb_serial.c`**

Make exactly these edits (line numbers refer to the freshly-copied teensy4 file):

1. **Includes (lines 36, 38):** delete `#include "avr/pgmspace.h"` and `#include "debug/printf.h"`. (`DMAMEM` comes from `imxrt1176.h` via `usb_dev.h`; `printf` debug is dropped.)

2. **Delete the slow-CPU DMAMEM-undef block (lines 49-52):** remove the entire
```c
#if defined(F_CPU) && F_CPU < 30000000
#undef DMAMEM
#define DMAMEM
#endif
```

3. **DMAMEM on the descriptor arrays (Seam B — the critical adaptation).** teensy4 leaves `tx_transfer`/`rx_transfer` in `.bss`; on RT1176 that is DTCM (DMA-unreachable). Change:
   - Line 72 from `static transfer_t tx_transfer[TX_NUM] __attribute__ ((used, aligned(32)));`
     to `DMAMEM static transfer_t tx_transfer[TX_NUM] __attribute__ ((used, aligned(32)));`
   - Line 79 from `static transfer_t rx_transfer[RX_NUM] __attribute__ ((used, aligned(32)));`
     to `DMAMEM static transfer_t rx_transfer[RX_NUM] __attribute__ ((used, aligned(32)));`
   (`txbuffer` line 73 and `rx_buffer` line 80 already carry `DMAMEM` — leave them.)

4. **`IRQ_USB1` → `IRQ_USB_OTG1`** at all four sites: lines 135 and 141 (`rx_queue_transfer`), 190 and 217 (`usb_serial_read`). E.g. `NVIC_DISABLE_IRQ(IRQ_USB1);` → `NVIC_DISABLE_IRQ(IRQ_USB_OTG1);`.

5. **Remove the `printf` debug calls** (no `printf` in this core): line 94 (`usb_serial_reset`), line 102 (`usb_serial_configure`), line 136 (`rx_queue_transfer`), line 149 (`rx_event`). Delete each `printf(...)` statement line.

6. **Remove the TX error-print block** (lines 338-342):
```c
			if (status & 0x68) {
				// TODO: what if status has errors???
				printf("ERROR status = %x, i=%d, ms=%u\n",
					status, tx_head, systick_millis_count);
			}
```
Delete these 5 lines entirely (keep the surrounding `if (!(status & 0x80)) {` and `tx_available = TX_SIZE;`).

7. **Drop the serialEvent/yield hook** (lines 124-125): delete
```c
	// weak serialEvent will be NULL unless user's program defines serialEvent()
	if (serialEvent) yield_active_check_flags |= YIELD_CHECK_USB_SERIAL;
```

Leave everything else (the `arm_dcache_delete`/`arm_dcache_flush_delete` calls stay — they resolve to the no-op inlines from Task 1; `timer_config/timer_start_oneshot/timer_stop` stay verbatim; `extern volatile uint8_t usb_high_speed;` stays).

- [ ] **Step 5: Create `usb_inst.cpp` (instantiate Serial + SerialUSB alias)**

```cpp
/* USB CDC serial object instantiation for the MIMXRT1176 core (SerialUSB Phase 2).
 * Serial is the USB CDC virtual serial port; SerialUSB is a reference alias to it. */
#include "usb_serial.h"

#if defined(CDC_STATUS_INTERFACE) && defined(CDC_DATA_INTERFACE)
usb_serial_class Serial;
usb_serial_class &SerialUSB = Serial;
#endif
```

- [ ] **Step 6: Retire the conflicting `usb.c` definitions**

In `cores/imxrt1176/usb.c`:
1. Delete lines 85-89 (the comment + the three definitions):
```c
// CDC line-state globals (owned here for Phase 1; Phase 2's usb_serial.c
// currently defines its own copies -- when it is added, move these there).
uint32_t usb_cdc_line_coding[2];
volatile uint32_t usb_cdc_line_rtsdtr_millis;
volatile uint8_t usb_cdc_line_rtsdtr = 0;
```
(The `extern` declarations remain in `usb_dev.h:40-42`, so `endpoint0_setup`/`endpoint0_complete` still compile and now link against `usb_serial.c`'s definitions.)

2. Delete lines 107-109 (the stubs):
```c
// Phase 2 fills these in (usb_serial.c).  Stubs so SET_CONFIGURATION links.
void usb_serial_configure(void) {}
void usb_serial_reset(void) {}
```

- [ ] **Step 7: Build and verify the core links with Serial present**

Run:
```bash
cd ~/Development/rt1170/evkb/usb_enum_test && export ARMGCC_DIR=/Applications/ARM_10 && cmake --build build 2>&1 | tail -5
```
Expected: builds clean. No "multiple definition of `usb_cdc_line_coding`", no "undefined reference to `usb_serial_configure`".

- [ ] **Step 8: Verify all four DMA structures land in OCRAM (`.dmabuffers`), not DTCM**

Run:
```bash
cd ~/Development/rt1170/evkb/usb_enum_test && arm-none-eabi-nm -S build/usb_enum_test.elf | grep -E "tx_transfer|rx_transfer|txbuffer|rx_buffer"
```
(Use `/Applications/ARM_10/bin/arm-none-eabi-nm` if `arm-none-eabi-nm` is not on PATH.)
Expected: all four symbols present, with addresses inside **OCRAM `0x20240000`–`0x202BFFFF`** (`.dmabuffers`, per `imxrt1176.ld:10,83-85`) — **not** DTCM `0x20000000`–`0x2003FFFF`. If any of the four is in the DTCM range, `DMAMEM` is missing on it (Seam B) and the bulk path will silently fail on HW. Cross-check the section: `arm-none-eabi-objdump -h build/usb_enum_test.elf | grep dmabuffers` shows the `.dmabuffers` VMA; all four symbols must fall inside it.

- [ ] **Step 9: Confirm enumeration still passes (regression)**

Run: `./run_qemu_usb.sh 2>&1 | tail -5`
Expected: `USB=CONFIGURED`, `PASS`.

- [ ] **Step 10: Commit**

```bash
cd ~/Development/rt1170/evkb/cores && git add imxrt1176/usb_serial.h imxrt1176/usb_serial.c imxrt1176/usb_inst.cpp imxrt1176/usb.c && git commit -m "imxrt1176: port usb_serial.c (CDC RX/TX rings + Serial:Stream); DMAMEM all bulk dTDs; own CDC line-state (revert usb.c to extern)"
```

---

### Task 4: Auto-init USB at startup + idempotent `usb_init`

Auto-init makes `Serial` live for every sketch without a manual `usb_init()`. The idempotency guard makes a second call (e.g. Phase-1 sketches that still call `usb_init()` in `setup()`) a safe no-op.

**Files:**
- Modify: `cores/imxrt1176/usb.c` (guard at top of `usb_init`)
- Modify: `cores/imxrt1176/main.cpp` (call `usb_init()` before `setup()`)

- [ ] **Step 1: Make `usb_init` idempotent**

In `cores/imxrt1176/usb.c`, at the very top of `void usb_init(void)` (the `NUM_ENDPOINTS`-defined variant, currently line 137), insert before `usb_pll_phy_init();`:

```c
	static uint8_t initialized = 0;
	if (initialized) return;
	initialized = 1;
```

- [ ] **Step 2: Auto-call `usb_init()` in `main()`**

In `cores/imxrt1176/main.cpp`, add includes after `#include <Arduino.h>`:

```cpp
#include "usb_desc.h"   // CDC_*_INTERFACE macros
#include "usb_dev.h"    // usb_init()
```

Then inside `extern "C" int main(void)`, in the `#else` (Arduino) branch, immediately before `setup();`, add:

```c
#if defined(CDC_STATUS_INTERFACE) && defined(CDC_DATA_INTERFACE)
	usb_init();     // bring up USB CDC so Serial is live before setup()
#endif
	setup();
```

- [ ] **Step 3: Build + verify enumeration still passes (now auto-init + guarded redundant manual init)**

Run:
```bash
cd ~/Development/rt1170/evkb/usb_enum_test && export ARMGCC_DIR=/Applications/ARM_10 && cmake --build build && ./run_qemu_usb.sh 2>&1 | tail -5
```
Expected: `USB=CONFIGURED`, `PASS`. (The sketch's own `usb_init()` in `setup()` now returns early.)

- [ ] **Step 4: Regression — two non-USB gates still pass with USB auto-init**

Run:
```bash
cd ~/Development/rt1170/evkb/tone_test && export ARMGCC_DIR=/Applications/ARM_10 && cmake --build build && ./run_qemu_tone.sh 2>&1 | tail -3
cd ~/Development/rt1170/evkb/interval_timer_test && cmake --build build && ./run_qemu_interval.sh 2>&1 | tail -3
```
(If a runner script has a different name, discover it: `ls ~/Development/rt1170/evkb/tone_test/*.sh`.)
Expected: both still print their PASS line — auto-init USB (no host attached → never configured) does not disturb them. If a gate now fails, debug with superpowers:systematic-debugging before proceeding (the ~1 ms PLL poll or USB IRQ is the suspect).

- [ ] **Step 5: Commit**

```bash
cd ~/Development/rt1170/evkb/cores && git add imxrt1176/usb.c imxrt1176/main.cpp && git commit -m "imxrt1176: auto-init USB before setup() + idempotent usb_init() (Serial live for every sketch)"
```

---

### Task 5: QEMU DATA gate — `usb_data_test` (the echo)

**Files:**
- Create: `evkb/usb_data_test/usb_data_test.cpp`
- Create: `evkb/usb_data_test/usb_echo_driver.py`
- Create: `evkb/usb_data_test/run_qemu_usb_data.sh`
- Create: `evkb/usb_data_test/CMakeLists.txt` + `toolchain/` (scaffolded from `usb_enum_test`)

- [ ] **Step 1: Scaffold the test directory**

```bash
cd ~/Development/rt1170/evkb
rm -rf usb_data_test && mkdir usb_data_test
cp -r usb_enum_test/toolchain usb_data_test/
sed 's/usb_enum_test/usb_data_test/g' usb_enum_test/CMakeLists.txt > usb_data_test/CMakeLists.txt
```

- [ ] **Step 2: Write the echo firmware `usb_data_test/usb_data_test.cpp`**

```cpp
#include "Arduino.h"
#include "HardwareSerial.h"
#include "usb_serial.h"   // Serial (USB CDC)
#include "usb_dev.h"      // usb_configuration

extern volatile uint8_t usb_configuration;

void setup() {
    Serial1.begin(115200);          // debug VCOM (LPUART1)
    // usb_init() is auto-called in main() before setup().
    uint32_t t0 = millis();
    while (usb_configuration == 0 && (millis() - t0) < 3000) { /* wait for host */ }
    Serial1.println(usb_configuration ? "USB=CONFIGURED" : "USB=TIMEOUT");
}

void loop() {
    int n = 0;
    while (Serial.available() > 0) {
        int c = Serial.read();
        if (c < 0) break;
        Serial.write((uint8_t)c);   // echo it back to the host
        n++;
    }
    if (n > 0) {
        Serial.send_now();          // explicit flush (independent of GPTIMER0)
        Serial1.print("ECHOED ");
        Serial1.println(n);
    }
}
```

- [ ] **Step 3: Write the socket echo driver `usb_data_test/usb_echo_driver.py`**

```python
#!/usr/bin/env python3
"""Drive the QEMU usbcdc socket: send a token, read the guest's echo, compare.
Exit 0 on a matching round-trip, 1 on mismatch, 2 on connection failure."""
import socket, sys, time

host, port, token = sys.argv[1], int(sys.argv[2]), sys.argv[3].encode()
payload = token + b"\n"

# QEMU opens the listening socket at startup; connect with a short retry.
sock = None
deadline = time.time() + 10
while time.time() < deadline:
    try:
        sock = socket.create_connection((host, port), timeout=1)
        break
    except OSError:
        time.sleep(0.2)
if sock is None:
    print("ERROR: could not connect to usbcdc socket")
    sys.exit(2)

# Let the guest reach CDC_RUN (DTR asserted) and prime its RX transfers.
time.sleep(1.0)
sock.settimeout(5)
sock.sendall(payload)

got = b""
try:
    while len(got) < len(payload):
        chunk = sock.recv(64)
        if not chunk:
            break
        got += chunk
except socket.timeout:
    pass

print("sent=%r got=%r" % (payload, got))
sys.exit(0 if got.strip() == token else 1)
```

- [ ] **Step 4: Write the runner `usb_data_test/run_qemu_usb_data.sh`**

```sh
#!/bin/sh
set -e
QEMU=~/Development/qemu2/build/qemu-system-arm
DIR=$(cd "$(dirname "$0")" && pwd)
ELF="$DIR/build/usb_data_test.elf"
VCOM="$DIR/vcom.uart"; DBG="$DIR/usb.dbg"; RES="$DIR/echo.result"
PORT=14555
rm -f "$VCOM" "$DBG" "$RES"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none \
    -serial file:"$VCOM" \
    -chardev socket,id=usbcdc,host=127.0.0.1,port=$PORT,server=on,wait=off \
    -d guest_errors -D "$DBG" &
P=$!
python3 "$DIR/usb_echo_driver.py" 127.0.0.1 $PORT "PHASE2-ECHO" > "$RES" 2>&1
RC=$?
sleep 1; kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== VCOM ===="; cat "$VCOM" 2>/dev/null || true
echo "==== CI-CDC ===="; grep "CI-CDC" "$DBG" 2>/dev/null | head
echo "==== echo driver ===="; cat "$RES"
[ $RC -eq 0 ] || { echo "FAIL: USB CDC echo"; exit 1; }
echo "PASS: USB CDC bulk data echo verified"
```

- [ ] **Step 5: Build**

Run:
```bash
cd ~/Development/rt1170/evkb/usb_data_test && chmod +x run_qemu_usb_data.sh && export ARMGCC_DIR=/Applications/ARM_10 && cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake . && cmake --build build 2>&1 | tail -3
```
Expected: `Linking … usb_data_test.elf`, no errors.

- [ ] **Step 6: Run the DATA gate**

Run: `./run_qemu_usb_data.sh 2>&1 | tail -20`
Expected:
- VCOM shows `USB=CONFIGURED` then one or more `ECHOED …` lines.
- `CI-CDC:` trace shows `config parsed: bulk IN=ep4 OUT=ep3` and `DTR asserted -> bridging USB serial`.
- echo driver prints `sent=b'PHASE2-ECHO\n' got=b'PHASE2-ECHO\n'`.
- Final line: `PASS: USB CDC bulk data echo verified`.

**If it fails,** debug with superpowers:systematic-debugging using the evidence: (a) no `ECHOED` on VCOM but `USB=CONFIGURED` → the guest isn't receiving RX (check `usb_serial_configure` primed RX / EP3 config / DMAMEM placement from Task 3 Step 8); (b) `ECHOED` on VCOM but `got=b''` → guest TX not reaching the chardev (check `send_now`/`usb_transmit` on EP4, bulk-IN re-arm); (c) no `DTR asserted` in `CI-CDC` → enumeration/line-state regressed. Do not proceed until this gate is green.

- [ ] **Step 7: Commit**

```bash
cd ~/Development/rt1170/evkb && git add usb_data_test && git commit -m "usb: CDC bulk-data echo QEMU gate (socket chardev; drives bytes, greps the echo)"
```

---

### Task 6: Hardware verification (the real arbiter)

**Files:**
- Create: `evkb/usb_data_test/hw_echo.py` (host-side pyserial echo check)

- [ ] **Step 1: Flash the echo firmware**

Run (kill stale probe backends first, per rt1170-evkb-flashing):
```bash
cd ~/Development/rt1170/evkb/usb_data_test && cmake --build build
pkill -9 -f LinkServer; pkill -9 -f redlinkserv; sleep 2
/Applications/LinkServer_26.6.137/LinkServer run MIMXRT1176:MIMXRT1170-EVKB build/usb_data_test.elf &
```
If a `DAPInfo … Hardware interface transfer error` recurs, unplug/replug the MCU-Link USB (J11) and retry (see rt1170-evkb-flashing).

- [ ] **Step 2: Confirm the firmware is running (debug VCOM)**

Read `/dev/cu.usbmodem5DQ2DDHVWO5EI3` @115200 with pyserial for `USB=CONFIGURED`:
```bash
python3 - <<'PY'
import serial, time
s = serial.Serial('/dev/cu.usbmodem5DQ2DDHVWO5EI3', 115200, timeout=1)
t=time.time()
while time.time()-t < 5:
    line = s.readline().decode(errors='replace').strip()
    if line: print("VCOM:", line)
    if "USB=CONFIGURED" in line: break
PY
```
Expected: `VCOM: USB=CONFIGURED`.

- [ ] **Step 3: Identify the native-USB CDC port**

Connect the EVKB **native USB** (OTG1 connector, not the MCU-Link debug USB) to the Mac. Then:
```bash
ls /dev/cu.usbmodem*
ioreg -p IOUSB -l | grep -iE "0x1209|USB Serial|idVendor"
```
Expected: a **new** `/dev/cu.usbmodem…` node besides the debug `…5DQ2DDHVWO5EI3`, and an IOUSB entry with `idVendor = 0x1209`. Note the new node path for Step 5.

- [ ] **Step 4: Write `usb_data_test/hw_echo.py`**

```python
#!/usr/bin/env python3
"""Open the native-USB CDC port, send bytes, confirm the firmware echoes them."""
import serial, sys, time

port = sys.argv[1]
msg  = b"PHASE2-HW-ECHO\n"
s = serial.Serial(port, 115200, timeout=2)
time.sleep(0.2)
s.reset_input_buffer()
s.write(msg)
s.flush()
got = b""
t = time.time()
while len(got) < len(msg) and time.time() - t < 3:
    got += s.read(len(msg) - len(got))
print("sent=%r got=%r" % (msg, got))
sys.exit(0 if got.strip() == msg.strip() else 1)
```

- [ ] **Step 5: Run the hardware echo (the arbiter)**

Run (substitute the node from Step 3):
```bash
cd ~/Development/rt1170/evkb/usb_data_test && python3 hw_echo.py /dev/cu.usbmodemXXXXXXX && echo "HW ECHO PASS"
```
Expected: `sent=b'PHASE2-HW-ECHO\n' got=b'PHASE2-HW-ECHO\n'` and `HW ECHO PASS`. This — the Mac round-tripping bytes through the native-USB CDC port — is the real Phase-2 pass.

**If QEMU passed but HW didn't,** debug with superpowers:systematic-debugging: check bulk-path DMA/coherency (all four DMA structs in OCRAM per Task 3 Step 8; D-cache off), the FS-vs-HS bulk packet size (`usb_high_speed` → 512), and confirm the debug VCOM still shows `ECHOED` when bytes are sent. Record the result either way (observational).

- [ ] **Step 6: Commit the HW helper**

```bash
cd ~/Development/rt1170/evkb && git add usb_data_test/hw_echo.py && git commit -m "usb: hardware CDC echo check (pyserial on the native-USB port)"
```

---

### Task 7: Finish — regression, push, memory, branch completion

- [ ] **Step 1: Full QEMU-gate regression** (auto-init touched shared `main.cpp`)

Run each gate's runner and confirm its PASS line:
```bash
cd ~/Development/rt1170/evkb
for d in usb_data_test usb_enum_test tone_test interval_timer_test irq_attach_test wire_master_test spi_loopback_test; do
  echo "=== $d ==="; ( cd "$d" && export ARMGCC_DIR=/Applications/ARM_10 && cmake --build build >/dev/null 2>&1 && ls *.sh >/dev/null 2>&1 && sh "$(ls run_*.sh | head -1)" 2>&1 | tail -1 )
done
```
Expected: every gate prints its `PASS…` line. Fix any regression (superpowers:systematic-debugging) before continuing.

- [ ] **Step 2: Push the core** (evkb stays local)

```bash
cd ~/Development/rt1170/evkb/cores && git push
```
(`qemu2` is unchanged this phase — nothing to push there. `evkb` is local-only, no push.)

- [ ] **Step 3: Update the memory note** `rt1176-serialusb.md`

Update the frontmatter `description` and body: Phase 2 (bulk data + `Serial : Stream`) COMPLETE, HW-verified (native-USB CDC echoes; VID 0x1209). Record: transfer scheduler was already in Phase-1 `usb.c`; the port = teensy4 `usb_serial.c` with the **DMAMEM-on-`tx_transfer`/`rx_transfer`** adaptation (RT1176 `.bss`=DTCM DMA-unreachable — the Phase-1 class-of-bug), GPTIMER0 auto-flush ported into `usb.c`'s ISR, `IRQ_USB1`→`IRQ_USB_OTG1`, `arm_dcache_*` no-op (D-cache off), serialEvent/yield hook dropped; line-state symbols moved usb.c→usb_serial.c; `Serial`+`SerialUSB` alias; USB auto-init in `main.cpp` (idempotent `usb_init`); QEMU DATA gate = `usb_data_test` (socket chardev + `usb_echo_driver.py`, `chipidea_cdc.c` bridges both ways, qemu2 unchanged). Update the one-line pointer in `MEMORY.md`. Link [[rt1170-evkb-flashing]].

- [ ] **Step 4: Complete the branch**

Invoke superpowers:finishing-a-development-branch.

---

## Self-review (author checklist — done)

- **Spec coverage:** Seam A duplicate symbols (T3 S6); Seam B DMAMEM-all-dTDs (T3 S4.3 + linker check T3 S8); Seam C GPTIMER0 flush timer (T1 regs + T2 ISR + T3 timer_config port); Seam D adaptations IRQ/dcache/serialEvent (T1 + T3 S4); `usb_serial_class` port + Serial/SerialUSB (T3 S1/S5); auto-init (T4); QEMU DATA echo gate (T5); HW echo (T6); regression/push/memory/finish (T7). All spec sections mapped.
- **Placeholder scan:** every code step shows complete code; the one runtime-variable value (the native-USB `/dev/cu.usbmodem…` node) is discovered in T6 S3 and substituted in S5 — not a placeholder but a device-enumeration fact. The `usb_serial.c`/`.h` ports are specified as copy + an exhaustive, line-referenced edit list (project convention for verbatim ports; matches the Phase-1 plan), not re-transcribed.
- **Type/name consistency:** `usb_timer0_callback` (T2 def ↔ T2 extern ↔ T3 timer use), `IRQ_USB_OTG1` (T1 ↔ T3), `arm_dcache_delete`/`arm_dcache_flush_delete` (T1 defs ↔ T3 call sites), `Serial`/`SerialUSB` (T3 header extern ↔ T3 usb_inst def ↔ T5 firmware use), `usbcdc`/PORT 14555 (T5 runner ↔ driver), `PHASE2-ECHO` token (runner ↔ driver) all consistent.
- **Build-green ordering:** T1 additive; T2 additive (callback NULL); T3 creates the port and removes usb.c defs/stubs in one commit (link stays resolvable); T4 auto-init guarded by idempotent `usb_init`; each task builds + runs a gate before commit.
- **Gate-first / HW-first:** T5 is the deterministic DATA gate (socket, explicit `send_now()` so it's GPTIMER0-independent); T6 makes the Mac echo the arbiter; both carry concrete systematic-debugging branches keyed to the failure evidence.
```
