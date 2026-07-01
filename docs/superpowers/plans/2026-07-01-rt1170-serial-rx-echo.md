# `Serial1` RX Echo (RT1176 Phase 2) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Validate the LPUART1 receive path end-to-end — RX interrupt → ring buffer → `yield()` → `serialEvent1()` → echo — proving the interrupt (not just a FIFO peek) did the work, verified in QEMU (injected input via a socket chardev) then on hardware.

**Architecture:** The RX path already exists from Phase 1 (`begin()` sets `RE|RIE`; `IRQHandler()` drains `DATA`→ring on `RDRF`; `yield()` calls `serialEvent1()`). This phase adds ONE diagnostic (`serial1_rx_isr_count`, incremented in the ISR RX branch) so the test can prove the interrupt fired, an echo sketch whose `serialEvent1()` is the sole echo path, and a socket-chardev QEMU harness that injects bytes and asserts the echo + `rx_isr>0`.

**Tech Stack:** C/C++ bare-metal, ARM GCC 10.2.1 (`/Applications/ARM_10`), CMake + teensy-cmake-macros, custom QEMU `qemu-system-arm -M mimxrt1170-evk` (`~/Development/qemu2/build/qemu-system-arm`), Python (socket + pyserial), LinkServer for hardware flash.

---

## Reference facts

- Core repo: `~/Development/rt1170/evkb/cores` (github `newdigate/teensy-cores`), branch off `master` (currently `a572577`). Core dir `cores/imxrt1176/`.
- Project repo (sketches, local-only, no remote): `~/Development/rt1170/evkb`, branch `master`.
- ARM build flags/defines (for standalone compile checks): `-mthumb -mcpu=cortex-m7 -mfloat-abi=hard -mfpu=fpv5-d16 -std=gnu++17 -fno-exceptions -fpermissive -fno-rtti -fno-threadsafe-statics -felide-constructors -Wno-error=narrowing -Os -I. -D__IMXRT1176__ -DTEENSYDUINO=159 -DARDUINO=10607 -DARDUINO_MIMXRT1170_EVKB -DF_CPU=996000000 -DUSB_SERIAL -DLAYOUT_US_ENGLISH`
- IRQHandler RX branch: `HardwareSerial.cpp` — `if (port->STAT & (LPUART_STAT_RDRF | LPUART_STAT_IDLE)) {` (~line 243). `Serial1` is the only instance, so an unconditional increment in the shared `IRQHandler()` counts only LPUART1 RX servicing.
- `HardwareSerial.h`: `extern HardwareSerialIMXRT Serial1;` (~line 199) — put the new extern beside it.
- QEMU LPUART1 is the FIRST `-serial` chardev on `mimxrt1170-evk`. RX is modeled (`imxrt_lpuart_receive` → FIFO → `RDRF` → IRQ when `RIE`).
- QEMU run (TX regression, Phase 1): `~/Development/rt1170/evkb/serial_test/run_qemu.sh` must still print `PASS`.
- Phase-1 sketch harness to clone: `~/Development/rt1170/evkb/serial_test/` (blink-derived CMake + toolchain).
- Board VCOM (when connected): `/dev/tty.usbmodem*` @115200; use pyserial NOT `cat`; `gtimeout` not `timeout`. LinkServer: `/Applications/LinkServer_26.6.137/LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load <hex>`. After flash the core may sit halted (VC_CORERESET) — power-cycle to run.

## File structure

- `cores/imxrt1176/HardwareSerial.cpp` — MODIFY: add `volatile uint32_t serial1_rx_isr_count` + increment in ISR RX branch.
- `cores/imxrt1176/HardwareSerial.h` — MODIFY: `extern volatile uint32_t serial1_rx_isr_count;`.
- `~/Development/rt1170/evkb/serial_test_rx/` — CREATE: `serial_test_rx.cpp`, `CMakeLists.txt`, `toolchain/` (cloned from `serial_test/`), `run_qemu_rx.sh`, `qemu_rx_driver.py`, `capture_rx_hw.py`.

---

## Task 1: Driver RX-interrupt counter

**Files:**
- Modify: `cores/imxrt1176/HardwareSerial.cpp`
- Modify: `cores/imxrt1176/HardwareSerial.h`

- [ ] **Step 1: Add the extern declaration to HardwareSerial.h**

Immediately after `extern HardwareSerialIMXRT Serial1;` (~line 199), add:
```c
// Diagnostic: counts each LPUART1 RX-servicing pass in the ISR (proves the RX
// interrupt fired, vs. bytes drained by an available()/read() FIFO peek).
extern volatile uint32_t serial1_rx_isr_count;
```

- [ ] **Step 2: Define + increment the counter in HardwareSerial.cpp**

At file scope near the top of `HardwareSerial.cpp` (after the includes / `UART_CLOCK` define), add:
```c
volatile uint32_t serial1_rx_isr_count = 0;
```
In `HardwareSerialIMXRT::IRQHandler()`, inside the RX branch, increment it as the FIRST statement of the block:
```c
	if (port->STAT & (LPUART_STAT_RDRF | LPUART_STAT_IDLE)) {
		serial1_rx_isr_count++;   // diagnostic: an RX-servicing pass ran in the ISR
		// See how many bytes are pending.
		uint8_t avail = (port->WATER >> 24) & 0x7;
		...
```
(Only `Serial1` exists, so the shared `IRQHandler()` running its RX branch is always LPUART1. If more instances are added later, gate this on `port_addr == 0x4007C000`.)

- [ ] **Step 3: ARM-compile the driver (guards off, real flags) — verify clean**

```bash
cd ~/Development/rt1170/evkb/cores/imxrt1176
FLAGS="-mthumb -mcpu=cortex-m7 -mfloat-abi=hard -mfpu=fpv5-d16 -std=gnu++17 -fno-exceptions -fpermissive -fno-rtti -fno-threadsafe-statics -felide-constructors -Wno-error=narrowing -Os -I. -D__IMXRT1176__ -DTEENSYDUINO=159 -DARDUINO=10607 -DARDUINO_MIMXRT1170_EVKB -DF_CPU=996000000 -DUSB_SERIAL -DLAYOUT_US_ENGLISH"
/Applications/ARM_10/bin/arm-none-eabi-g++ $FLAGS -c HardwareSerial.cpp -o /tmp/hs.o && echo "HardwareSerial.cpp OK"
```
Expected: `HardwareSerial.cpp OK`.

- [ ] **Step 4: TX regression — the Phase-1 serial_test QEMU test still passes**

```bash
cd ~/Development/rt1170/evkb/serial_test
rm -rf build && cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake . >/dev/null 2>&1
cmake --build build >/dev/null 2>&1
sh run_qemu.sh 2>&1 | tail -3
```
Expected: `PASS: QEMU serial output verified` (the counter addition must not perturb TX).

- [ ] **Step 5: Commit**

```bash
cd ~/Development/rt1170/evkb/cores && git add -A && \
git commit -m "feat(imxrt1176): RX-interrupt diagnostic counter (serial1_rx_isr_count)"
```

---

## Task 2: RX echo sketch + QEMU socket-injection harness

**Files:**
- Create: `~/Development/rt1170/evkb/serial_test_rx/serial_test_rx.cpp`
- Create: `~/Development/rt1170/evkb/serial_test_rx/CMakeLists.txt`, `toolchain/` (cloned)
- Create: `~/Development/rt1170/evkb/serial_test_rx/qemu_rx_driver.py`
- Create: `~/Development/rt1170/evkb/serial_test_rx/run_qemu_rx.sh`

- [ ] **Step 1: Clone the serial_test harness**

```bash
cd ~/Development/rt1170/evkb
cp -r serial_test serial_test_rx
rm -rf serial_test_rx/build serial_test_rx/serial.uart serial_test_rx/serial.dbg
mv serial_test_rx/serial_test.cpp serial_test_rx/serial_test_rx.cpp
```
Edit `serial_test_rx/CMakeLists.txt`: replace every `serial_test` token with `serial_test_rx` (`project(serial_test_rx)`, `teensy_add_executable(serial_test_rx serial_test_rx.cpp)`, `teensy_target_link_libraries(serial_test_rx cores)`, `target_link_libraries(serial_test_rx.elf stdc++)`). Confirm no `serial_test`/`blinky` target names remain (`grep -n 'serial_test\b\|blinky' CMakeLists.txt`).

- [ ] **Step 2: Write the echo sketch**

`serial_test_rx/serial_test_rx.cpp`:
```cpp
#include "Arduino.h"
#include "core_pins.h"
#include "HardwareSerial.h"

static volatile uint32_t echoed = 0;

// serialEvent1() overrides the weak default in HardwareSerial1.cpp.  It is the
// ONLY echo path: yield() calls it (between loop() iterations) when RX data is
// available.  Echoes each byte verbatim; counts bytes echoed.
void serialEvent1() {
    while (Serial1.available()) {
        int c = Serial1.read();
        if (c < 0) break;
        Serial1.write((uint8_t)c);
        echoed++;
    }
}

void setup() {
    Serial1.begin(115200);
    Serial1.println("RT1176 RX echo ready");
}

void loop() {
    // loop() does NOT read/echo (avoids a read race with serialEvent1); it only
    // reports.  Returning to the core main() calls yield(), which drives
    // serialEvent1() when bytes are available.
    Serial1.print("[status] rx_isr=");
    Serial1.print(serial1_rx_isr_count);
    Serial1.print(" echoed=");
    Serial1.println(echoed);
    delay(1000);
}
```

- [ ] **Step 3: Build the sketch**

```bash
cd ~/Development/rt1170/evkb/serial_test_rx
cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake . >/dev/null 2>&1
cmake --build build 2>&1 | tail -4
```
Expected: `serial_test_rx.elf` built (link succeeds).

- [ ] **Step 4: Write the QEMU RX driver (socket client)**

`serial_test_rx/qemu_rx_driver.py`:
```python
#!/usr/bin/env python3
"""Connect to QEMU's LPUART1 socket, inject bytes, assert echo + rx_isr>0."""
import socket, sys, time

HOST, PORT = "127.0.0.1", int(sys.argv[1]) if len(sys.argv) > 1 else 45455
PAYLOAD = b"hello\n"

def connect(timeout=10.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            s = socket.create_connection((HOST, PORT), timeout=2.0)
            s.settimeout(2.0)
            return s
        except OSError:
            time.sleep(0.2)
    sys.exit("FAIL: could not connect to QEMU serial socket")

def read_until(s, needle, deadline):
    buf = b""
    while time.time() < deadline:
        try:
            b = s.recv(256)
        except socket.timeout:
            continue
        if not b:
            break
        buf += b
        if needle in buf:
            return buf, True
    return buf, False

s = connect()
overall = time.time() + 15
# 1. wait for the banner
buf, ok = read_until(s, b"RT1176 RX echo ready", overall)
if not ok:
    sys.exit(f"FAIL: banner not seen; got {buf!r}")
# 2. inject payload into the UART RX
s.sendall(PAYLOAD)
# 3. read until we see the payload echoed back (after the banner point)
after = buf.split(b"RT1176 RX echo ready", 1)[1]
buf2, ok = read_until(s, PAYLOAD.strip(), time.time() + 8)
stream = after + buf2
if PAYLOAD.strip() not in stream:
    sys.exit(f"FAIL: echo not seen; got {stream!r}")
# 4. read a status line and assert rx_isr>0 and echoed>=len(payload)
buf3, _ = read_until(s, b"[status] rx_isr=", time.time() + 4)
import re
m = None
for line in (stream + buf3).splitlines():
    mm = re.search(rb"\[status\] rx_isr=(\d+) echoed=(\d+)", line)
    if mm:
        m = mm
if not m:
    sys.exit(f"FAIL: no status line seen")
rx_isr, ech = int(m.group(1)), int(m.group(2))
if rx_isr <= 0:
    sys.exit(f"FAIL: rx_isr={rx_isr} (RX interrupt never fired)")
if ech < len(PAYLOAD):
    sys.exit(f"FAIL: echoed={ech} < {len(PAYLOAD)}")
print(f"PASS: echo verified, rx_isr={rx_isr}, echoed={ech}")
s.close()
```

- [ ] **Step 5: Write the QEMU runner**

`serial_test_rx/run_qemu_rx.sh`:
```bash
#!/bin/sh
set -e
QEMU=~/Development/qemu2/build/qemu-system-arm
DIR=$(cd "$(dirname "$0")" && pwd)
ELF="$DIR/build/serial_test_rx.elf"
PORT=45455
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -chardev socket,id=u1,host=127.0.0.1,port=$PORT,server=on,wait=off \
    -serial chardev:u1 -d guest_errors -D "$DIR/rx.dbg" &
P=$!
sleep 1
RC=0
python3 "$DIR/qemu_rx_driver.py" $PORT || RC=1
kill $P 2>/dev/null; wait $P 2>/dev/null || true
exit $RC
```
Make it executable: `chmod +x serial_test_rx/run_qemu_rx.sh`.

- [ ] **Step 6: Run the QEMU RX test — verify PASS**

```bash
cd ~/Development/rt1170/evkb/serial_test_rx && sh run_qemu_rx.sh
```
Expected: `PASS: echo verified, rx_isr=<N>, echoed=<M>` with `N>0`, `M>=6`. If it fails on connect, confirm the QEMU binary exists (`ninja -C ~/Development/qemu2/build qemu-system-arm`) and that port 45455 is free (change `PORT` in both the script and the driver default if needed). If `rx_isr==0`, the RX interrupt path is broken — debug the ISR/NVIC wiring, do not fake a pass.

- [ ] **Step 7: Add a .gitignore entry + commit (evkb repo)**

Ensure `serial_test_rx/build/`, `serial_test_rx/rx.dbg` are ignored (extend the evkb `.gitignore` if the existing patterns don't already cover `build/` and `*.dbg`). Then:
```bash
cd ~/Development/rt1170/evkb && git add serial_test_rx .gitignore && \
git commit -m "test(serial): RX echo sketch + QEMU socket-injection harness"
```
(Do NOT commit `build/` or `rx.dbg`.)

---

## Task 3: Hardware verification (flash + pyserial send/echo)

**Files:**
- Create: `~/Development/rt1170/evkb/serial_test_rx/capture_rx_hw.py`

- [ ] **Step 1: Write the hardware RX test**

`serial_test_rx/capture_rx_hw.py`:
```python
#!/usr/bin/env python3
"""Flash-then-run this sketch, then: read banner, send 'hello\n', assert echo + rx_isr>0."""
import sys, time, glob, re, serial

port = sys.argv[1] if len(sys.argv) > 1 else None
if not port:
    ms = sorted(glob.glob("/dev/tty.usbmodem*"))
    if not ms:
        sys.exit("no /dev/tty.usbmodem* found - is the board connected?")
    port = ms[0]

s = serial.Serial(port, 115200, timeout=1)
time.sleep(0.2)
s.reset_input_buffer()
# send payload; the board echoes via serialEvent1 and reports [status]
s.write(b"hello\n")
end = time.time() + 6
buf = b""
while time.time() < end:
    buf += s.read(256)
s.close()
sys.stdout.write(buf.decode(errors="replace"))
echo_ok = b"hello" in buf
m = None
for line in buf.splitlines():
    mm = re.search(rb"\[status\] rx_isr=(\d+) echoed=(\d+)", line)
    if mm:
        m = mm
rx_isr = int(m.group(1)) if m else 0
if echo_ok and rx_isr > 0:
    print(f"\nPASS: hardware RX echo verified (rx_isr={rx_isr})")
    sys.exit(0)
print(f"\nFAIL: echo={echo_ok} rx_isr={rx_isr}")
sys.exit(1)
```

- [ ] **Step 2: Flash the sketch**

```bash
/Applications/LinkServer_26.6.137/LinkServer flash MIMXRT1176:MIMXRT1170-EVKB \
    load ~/Development/rt1170/evkb/serial_test_rx/build/serial_test_rx.hex
```
Then ensure the board is RUNNING (power-cycle the target if it sits halted after flash — see the Phase-1 reset note).

- [ ] **Step 3: Run the hardware RX test — verify PASS**

```bash
ls /dev/tty.usbmodem*
python3 ~/Development/rt1170/evkb/serial_test_rx/capture_rx_hw.py /dev/tty.usbmodemXXXX
```
Expected: the sent `hello` echoed back in the output, a `[status] rx_isr=N` line with `N>0`, and `PASS: hardware RX echo verified`. This confirms the RX interrupt path on silicon. (This test only needs the board running — no button press. If it can't drive the board interactively, report the QEMU result as the gate and note the hardware step is manual.)

- [ ] **Step 4: Commit**

```bash
cd ~/Development/rt1170/evkb && git add serial_test_rx/capture_rx_hw.py && \
git commit -m "test(serial): hardware pyserial RX echo verification"
```

---

## Final review

After all tasks: dispatch a final review over the whole change (driver counter + sketch + harnesses), confirm both the Phase-1 TX QEMU test and the new RX QEMU test pass, then use `superpowers:finishing-a-development-branch` to merge the `cores` feature branch to `master`.

**Verification summary:** Task 1 — ARM compile + Phase-1 TX regression; Task 2 — QEMU socket driver asserts echo + `rx_isr>0` + `echoed>=6` (primary gate); Task 3 — hardware pyserial send/echo asserts echo + `rx_isr>0`.
