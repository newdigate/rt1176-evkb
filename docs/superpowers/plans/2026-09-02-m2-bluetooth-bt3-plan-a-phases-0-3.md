# BT-3 Plan A (phases 0–3): fast transport, `M2Radio/bt`, AVDTP signalling, SBC encoder — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Get the RT1176 + IW416 to negotiate an A2DP stream with a real sink (the calibrated ESP32 instrument and two headsets) — HCI at 921 600 baud, a clean-room `M2Radio/bt/` layer (L2CAP with credits, link bring-up, SDP client, AVDTP initiator) driven through `SET_CONFIGURATION → OPEN → START` with the sink reporting our configuration as *started* — plus a host-tested clean-room SBC encoder. Plan B (phases 4–5) adds RTP, `AudioOutputBluetooth`, first sound and the capstone.

**Architecture:** BT-2's prototype in `examples/networking/m2_hci_probe/m2_hci_probe.cpp` moves into `M2Radio/bt/` as small single-purpose units (`L2cap`, `BtLink`, `Sdp`, `Avdtp`, `Sbc`), all main-context TX (never from the RX pump — the B6 bus-fault rule), no heap. The probe becomes the library's first client and keeps its gates. The Python fake controller (`hci_peer.py`) grows an AVDTP acceptor so QEMU gates cover signalling with zero qemu2 changes. Spec: `docs/superpowers/specs/2026-09-02-m2-bluetooth-bt3-a2dp-source-design.md`.

**Tech Stack:** C++11 (ARM GCC 10 for the board, host `c++` for tests), CMake via `evkb.cmake`, QEMU gates via `tools/gate-lib.sh` + `tools/qrun`, Python 3 fake controller over `-serial unix:`, LinkServer for silicon, the ESP32 sink (`tools/esp32-a2dp-sink/`) as the silicon oracle.

**House rules that bind every task:** clean-room only (NXP EdgeFast/EtherMind are LA_OPT — protocol facts only; the Linux `btnxpuart` driver is GPL and is never read; no `libsbc`/bluedroid source); every new gate is demonstrated RED against a deliberate break before it is trusted and gets a `tools/gate-vacuity.test.sh` fixture; the sweep count (`./tools/run-all-qemu-gates.sh -l`, **124** today) moves by exactly the gates added; `tools/license-audit.sh` passes; M2Radio changes are committed there, pushed, then pinned in `evkb.cmake` and verified with `-DEVKB_FORCE_FETCH=ON` **plus** a gate run on the fetched ELF. Bench builds keep the real blobs and `M2_BT_*` knobs; gate builds keep every knob OFF so the 124 gates stay byte-identical.

---

## File structure

| Path | Responsibility |
|---|---|
| `M2Radio/hci/HciTransport.{h,cpp}` (modify) | `rebaud(uint32_t)` — re-program LPUART2 at a new rate without losing the RX extension |
| `M2Radio/bt/L2cap.{h,cpp}` (create) | signalling + CO channels + ACL demux + credits; all TX in `service()` |
| `M2Radio/bt/BtLink.{h,cpp}` (create) | inquiry-by-name → connect → pair (SSP, PIN fallback) → encrypt |
| `M2Radio/bt/Sdp.{h,cpp}` (create) | one SSA request: AudioSink → AVDTP version |
| `M2Radio/bt/Avdtp.{h,cpp}` (create) | initiator signalling + media channel; answers a peer's DISCOVER |
| `M2Radio/bt/Sbc.{h,cpp}` (create) | clean-room encoder, 8 subbands / 16 blocks / joint or stereo / loudness |
| `M2Radio/bt/test/{run.sh,l2cap_test.cpp,avdtp_test.cpp,sbc_test.cpp}` (create) | host tests |
| `M2Radio/bt/test/sbc_snr.py` (create) | test TOOL: ffmpeg-decodes `sine.sbc`, computes SNR; nothing links it |
| `examples/networking/m2_hci_probe/m2_hci_probe.cpp` (modify) | phase 0 fast-baud + loopback steps; then re-pointed at `M2Radio/bt` |
| `examples/networking/m2_hci_probe/CMakeLists.txt` (modify) | `M2_BT_FAST_BAUD`, `M2_BT_LOOPBACK` knobs; `import_evkb_library(M2Radio sdio iw416 hci bt)` |
| `examples/networking/m2_hci_probe/hci_peer.py` (modify) | phases `baud` and `avdtp` (ACL parsing, L2CAP/SDP/AVDTP acceptor) |
| `examples/networking/m2_hci_probe/run_qemu_baud.sh`, `run_qemu_avdtp.sh` (create) | the `[baud]` and `[avdtp]` gates |
| `tools/gate-vacuity.test.sh` (modify) | fixtures for both new gates |
| `evkb.cmake` (modify) | M2Radio pin bumps |

`M2Radio`'s `import_evkb_library(M2Radio ...)` takes subdirectories; `bt` is a new one, added alongside `hci`, so `import_evkb_library(M2Radio sdio iw416 hci bt)` compiles `bt/*.cpp` (check `evkb.cmake`'s `resolve_arduino_library_auto` treats each subdir the way it does `hci` — it globs `*.cpp` per listed subdir).

---

## Phase 0 — transport at speed

### Task 1: `hci_peer.py` learns the vendor set-baud command (phase `baud`)

**Files:**
- Modify: `examples/networking/m2_hci_probe/hci_peer.py`

The IW416 keeps HCI at the download rate. The host sends vendor opcode **`0xFC09`** with a **4-byte little-endian baud** and, on Command Complete, re-programs its UART. A chardev has no baud, so the peer only checks the command's bytes and keeps answering; the gate asserts the *sequence* (reset → identity → set-baud → reset → identity again at the "new" rate).

- [ ] **Step 1: Add the opcode, the phase, and the handler**

In `hci_peer.py`, next to the other `OP_*` constants:

```python
OP_VS_SET_BAUD = 0xFC09                  # NXP vendor: param = uint32 LE running baud
```

In `LAST_OPCODE` (the dict that names each phase's last expected opcode) add:

```python
    "baud": OP_READ_BUFFER_SIZE,          # the SECOND identity pass, after the switch
```

In `Peer.__init__` add `self.baud_seen = []`. In `Peer.handle`, before the final `else`:

```python
        elif opcode == OP_VS_SET_BAUD:
            if len(params) != 4:
                self.log.append("PEER-SETBAUD-BAD-LEN %d" % len(params))
                self.send(cmd_complete(opcode, b"\x12")); return       # 0x12 = Invalid HCI Command Parameters
            rate = struct.unpack("<I", params)[0]
            self.baud_seen.append(rate)
            self.log.append("PEER-SETBAUD rate=%d" % rate)
            self.send(cmd_complete(opcode, b"\x00"))
```

- [ ] **Step 2: Make `PEER-DONE` report the baud**

In the `PEER-DONE` print at the bottom, add `baud=%s` with `",".join(str(b) for b in peer.baud_seen) or "none"` so the gate can assert it.

- [ ] **Step 3: Run the existing `[hci]` gate to prove nothing regressed**

Run: `cd examples/networking/m2_hci_probe && ./run_qemu_hci.sh 2>&1 | tail -3`
Expected: the final `PASS:` line, unchanged (the new opcode is never sent by the current probe).

- [ ] **Step 4: Commit**

```bash
git add examples/networking/m2_hci_probe/hci_peer.py
git commit -m "m2_hci_probe: hci_peer.py answers the NXP vendor set-baud (0xFC09) and has a [baud] phase"
```

### Task 2: `HciTransport::rebaud()`

**Files:**
- Modify: `M2Radio/hci/HciTransport.h`, `M2Radio/hci/HciTransport.cpp`

`begin(baud)` calls `m_port.begin()` **then** `addMemoryForRead()`; the core's `begin()` resets the RX ring, so re-baud must redo both, in that order, and must not be called with bytes in flight (the caller waits for the set-baud Command Complete first).

- [ ] **Step 1: Declare and implement**

`HciTransport.h`, after `void end();`:
```cpp
    // Re-program the port at a new rate.  Call ONLY after the controller has
    // acknowledged its own rate change and the line is idle: begin() resets
    // the RX ring, so anything arriving during the switch is lost by design.
    void rebaud(uint32_t baud) { begin(baud); }
```
(No `.cpp` change: `begin()` already does port `begin` + `addMemoryForRead` in the right order.)

- [ ] **Step 2: Host tests still pass** (no host coverage for the Arduino-bound transport; this confirms the header compiles for the rest)

Run: `~/Development/M2Radio/hci/test/run.sh | tail -1`
Expected: `HCI-HOST-TESTS: PASS`

- [ ] **Step 3: Commit (M2Radio)**

```bash
cd ~/Development/M2Radio && git add hci/HciTransport.h && git commit -m "hci: HciTransport::rebaud() for the vendor baud switch"
```

### Task 3: the probe's fast-baud step + the `[baud]` gate

**Files:**
- Modify: `examples/networking/m2_hci_probe/m2_hci_probe.cpp` (after `probeIdentity()` in `setup()`), `examples/networking/m2_hci_probe/CMakeLists.txt`
- Create: `examples/networking/m2_hci_probe/run_qemu_baud.sh`
- Modify: `tools/gate-vacuity.test.sh`

- [ ] **Step 1: CMake knob** (after the `M2_BT_LEGACY_PIN` block)

```cmake
# Phase 0 (BT-3): after identity at the download rate, switch the controller
# to M2_BT_FAST_BAUD_RATE with the NXP vendor set-baud command (0xFC09, uint32
# LE), re-baud LPUART2 and re-validate.  Default OFF: the gates run at 115200
# (a chardev has no baud) except [baud], which asserts the SEQUENCE.
option(M2_BT_FAST_BAUD "Switch HCI to M2_BT_FAST_BAUD_RATE after identity" OFF)
set(M2_BT_FAST_BAUD_RATE "921600" CACHE STRING "Rate for M2_BT_FAST_BAUD")
if(M2_BT_FAST_BAUD)
    add_definitions(-DM2_BT_FAST_BAUD=${M2_BT_FAST_BAUD_RATE})
endif()
```

- [ ] **Step 2: The probe step** (a new function next to `probeIdentity()`, called right after it in `setup()`; `hciIo` is the `HciTransport`, `hci` the `Hci`)

```cpp
#if defined(M2_BT_FAST_BAUD)
static const uint16_t OP_VS_SET_BAUD = 0xFC09;
// Phase 0: vendor set-baud (uint32 LE), then re-baud the port and re-validate
// with a fresh Reset + identity.  Every exit is named.
static void probeFastBaud() {
    uint32_t rate = M2_BT_FAST_BAUD;
    uint8_t p[4] = { (uint8_t)rate, (uint8_t)(rate >> 8), (uint8_t)(rate >> 16), (uint8_t)(rate >> 24) };
    Hci::Reply r;
    Hci::Error e = hci.run(OP_VS_SET_BAUD, p, 4, &r, 1000, idleMs);
    if (e != Hci::OK) { printFail("bt_baud_switch", e, r, "vendor 0xFC09 refused"); return; }
    delay(20);                                            // let the controller's reply drain and switch
    hciIo.rebaud(rate);
    hciCountersFold(); hci.begin();
    e = hci.run(OP_RESET, nullptr, 0, &r, 1000, idleMs);
    if (e != Hci::OK) { CONSOLE.print("bt_baud_switch=fail rate="); CONSOLE.print(rate);
                        CONSOLE.print(" reason="); CONSOLE.println(Hci::errorName(e)); return; }
    CONSOLE.print("bt_baud_switch=ok rate="); CONSOLE.println(rate);
    probeIdentity();                                      // identity again, at the new rate
}
#endif
```
and in `setup()` immediately after the existing `probeIdentity();`:
```cpp
#if defined(M2_BT_FAST_BAUD)
    probeFastBaud();
#endif
```
(`printFail`, `hciCountersFold`, `idleMs`, `OP_RESET` already exist in the probe.)

- [ ] **Step 3: The gate** — `run_qemu_baud.sh`, modelled on `run_qemu_hci.sh` (same `run_phase` shape; copy that function verbatim, it is not repeated here). It needs an ELF built with the knob, so it builds its own: `build-baud/` with `-DM2_BT_FAST_BAUD=ON` and synthetic blobs.

```sh
#!/bin/sh
# run_qemu_baud.sh -- the [baud] gate (BT-3 phase 0): the vendor set-baud
# SEQUENCE is right.  A chardev has no baud, so what the RATE does is
# silicon-only (transcript_hw_evkb.txt); this proves the driver sends 0xFC09
# with the right bytes, waits for its reply, re-baud()s, and re-validates
# with a fresh Reset + identity -- against a peer that answers.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
[ "$(gate_board)" = rt1176 ] || { echo "FAIL: rt1176-only"; exit 1; }
# This gate owns its build: the knob must be ON here and OFF everywhere else.
cmake -S "$DIR" -B "$DIR/build-baud" -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake \
      -DM2_BT_FAST_BAUD=ON >/dev/null && cmake --build "$DIR/build-baud" >/dev/null
ELF="$DIR/build-baud/m2_hci_probe.elf"
fail() { echo "FAIL: $*"; exit 1; }
# --- run_phase: identical to run_qemu_hci.sh's (socket in /tmp, server without nowait) ---
run_phase() { PHASE=$1; WAIT=$2
    OUT=$(gate_capture_path "$DIR" "baud_$PHASE.uart"); DBG=$(gate_capture_path "$DIR" "baud_$PHASE.dbg"); RES=$(gate_capture_path "$DIR" "baud_$PHASE.peer")
    rm -f "$OUT" "$DBG" "$RES"; SOCK="/tmp/m2baud_$$.sock"; rm -f "$SOCK"; gate_tmp "$SOCK"
    "$QEMU" $(gate_qemu_machine) -kernel "$ELF" -display none $(gate_console "$OUT") -serial unix:"$SOCK",server -d guest_errors -D "$DBG" &
    P=$!; gate_pid $P; PEER_RC=0
    python3 "$DIR/hci_peer.py" "$PHASE" "$SOCK" > "$RES" 2>&1 || PEER_RC=$?
    for _ in $(seq 1 120); do [ -f "$OUT" ] && grep -q "$WAIT" "$OUT" 2>/dev/null && break; sleep 0.25; done
    gate_reap $P; gate_require_capture "$OUT" "phase $PHASE"
    echo "==== captured UART ($PHASE) ===="; cat "$OUT"; echo "==== peer ($PHASE) ===="; cat "$RES"; }
run_phase baud '^hb card=0 btfw=no_start_indication hci=ok n=1 '
grep -q "^hci_reset=ok attempts=" "$OUT"                          || fail "[baud] no first Reset"
grep -q "^bt_baud_switch=ok rate=921600[[:space:]]*$" "$OUT"       || fail "[baud] the switch did not report ok at 921600"
[ "$(grep -c '^hci_version: hci_ver=11 hci_rev=0xBEEF' "$OUT")" -eq 2 ] || fail "[baud] identity must be read TWICE: before and after the switch"
grep -q "^PEER-SETBAUD rate=921600[[:space:]]*$" "$RES"            || fail "[baud] the peer did not receive 0xFC09 with 921600 LE"
grep -q "^PEER-DONE phase=baud cmds=9 resets=2 " "$RES"            || fail "[baud] expected Reset, 3 identity, 0xFC09, Reset, 3 identity = 9 commands, 2 resets"
# The set-baud must come AFTER the first identity and BEFORE the second Reset.
grep "^PEER-DONE" "$RES" | grep -q "opcodes=0c03,1001,1009,1005,fc09,0c03,1001,1009,1005" || fail "[baud] wrong order: $(grep '^PEER-DONE' "$RES")"
[ "$PEER_RC" -eq 0 ] || fail "[baud] peer exited $PEER_RC"
echo "PASS: the vendor set-baud sequence is right (0xFC09 uint32 LE after identity, reply awaited, Reset + identity re-run); the RATE itself is silicon-only"
```
`chmod +x run_qemu_baud.sh`.

- [ ] **Step 4: Run it**

Run: `./run_qemu_baud.sh 2>&1 | tail -2`
Expected: `PASS: the vendor set-baud sequence is right ...`

- [ ] **Step 5: Demonstrate it RED** — in `probeFastBaud()` change `p[4]` to send the rate big-endian (`(uint8_t)(rate >> 24)` first); rebuild via the gate; expect `FAIL: [baud] the peer did not receive 0xFC09 with 921600 LE`. Revert. Then swap `hciIo.rebaud(rate)` to before `hci.run(OP_VS_SET_BAUD ...)`; expect `FAIL: [baud] wrong order` (the peer sees the reset first). Revert. Record both in the script header under `DEMONSTRATED RED`.

- [ ] **Step 6: Vacuity fixture** — in `tools/gate-vacuity.test.sh` section 6, after the `[hci]` block, add the same shape for `run_qemu_baud.sh` against the card-absent `transcript_qemu.txt`: it must FAIL by name (`[baud] no first Reset` is what the fallback capture produces). Run `./tools/gate-vacuity.test.sh | tail -3` → all reports `ok`.

- [ ] **Step 7: Sweep arithmetic** — `./tools/run-all-qemu-gates.sh -l | tail -1` → `(125 gate(s))`; the new id is `rt1176:networking/m2_hci_probe[baud]`.

- [ ] **Step 8: Commit**

```bash
git add examples/networking/m2_hci_probe/m2_hci_probe.cpp examples/networking/m2_hci_probe/CMakeLists.txt examples/networking/m2_hci_probe/run_qemu_baud.sh tools/gate-vacuity.test.sh
git commit -m "m2_hci_probe: phase 0 fast baud (0xFC09 + rebaud + re-validate) and the [baud] gate (sweep 125)"
```

### Task 4: silicon — the switch, then loopback loss at speed

**Files:**
- Modify: `examples/networking/m2_hci_probe/m2_hci_probe.cpp`, `CMakeLists.txt`
- Append: `examples/networking/m2_hci_probe/transcript_hw_evkb.txt`

- [ ] **Step 1: Bench build with the switch** — `cmake -B build -DM2RADIO_IW416_FW=<sdIW416_wlan.bin.inc> -DM2RADIO_IW416_BT_FW=<uartIW416_bt.bin.inc> -DM2_BT_ASSERT_CTS=ON -DM2_BT_FAST_BAUD=ON` (blobs from `mcuxsdk-ws/mcuxsdk/components/conn_fwloader/fw_bin/inc/IW416/`), flash with `LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load build/m2_hci_probe.elf`, read with a DTR-releasing pyserial reader (see `transcript_hw_evkb.txt` 2026-09-02 for the recipe). Expected on the console: `bt_baud_switch=ok rate=921600` and a second `hci_version: ... manufacturer=0x0048`. If the second Reset times out, try 3000000 (`-DM2_BT_FAST_BAUD_RATE=3000000`; the 24 MHz root hits it exactly, OSR 8) and record which rates answer.

- [ ] **Step 2: Loopback knob** — CMake `option(M2_BT_LOOPBACK "After the baud switch: local loopback, N ACL packets echoed" OFF)`; in the probe:

```cpp
#if defined(M2_BT_LOOPBACK)
static volatile uint16_t s_lbHandle = 0; static volatile uint32_t s_lbEchoed = 0, s_lbBytes = 0;
// Loopback ACL packets come back on the loopback handle (Vol 4 Part E 7.6.2):
// count them and their bytes from onAcl (record only -- no TX here).
static void lbOnAcl(void *, uint16_t handle, const uint8_t *, uint16_t len) {
    if (handle == s_lbHandle) { s_lbEchoed++; s_lbBytes += len; }
}
static void probeLoopback() {
    Hci::Reply r; uint8_t mode = 0x01;                     // 0x01 = local loopback
    s_connDone = false;
    if (hci.run(0x1802, &mode, 1, &r, 1000, idleMs) != Hci::OK) { CONSOLE.println("loopback=fail (Write_Loopback_Mode)"); return; }
    uint32_t t0 = millis();                                 // the controller reports its loopback ACL handle
    while (!s_connDone && millis() - t0 < 3000) delay(10);  // via Connection_Complete (link_type ACL)
    if (!s_connDone) { CONSOLE.println("loopback=fail (no loopback Connection_Complete)"); return; }
    s_lbHandle = s_connHandle; s_lbEchoed = 0; s_lbBytes = 0;
    hci.onAcl(lbOnAcl, nullptr);
    const uint32_t N = 200; const uint16_t LEN = 600;       // ~ the media packet size
    static uint8_t pkt[9 + 600];
    uint16_t hf = (uint16_t)((s_lbHandle & 0x0FFF) | (0x02u << 12));
    pkt[0] = 0x02; pkt[1] = (uint8_t)hf; pkt[2] = (uint8_t)(hf >> 8);
    pkt[3] = (uint8_t)(LEN + 4); pkt[4] = (uint8_t)((LEN + 4) >> 8);
    pkt[5] = (uint8_t)LEN; pkt[6] = (uint8_t)(LEN >> 8); pkt[7] = 0x40; pkt[8] = 0x00;
    for (uint16_t i = 0; i < LEN; i++) pkt[9 + i] = (uint8_t)i;
    uint32_t sent = 0, credits = 7, tStart = millis();      // acl_num from Read_Buffer_Size
    while (sent < N && millis() - tStart < 20000) {
        if (credits) { hciIo.write(pkt, sizeof pkt); sent++; credits--; }
        delay(1);
        uint32_t ech = s_lbEchoed;                          // each echo frees one buffer
        if (ech + credits < sent) credits = (uint32_t)(sent - ech) > 7 ? 0 : 7 - (uint32_t)(sent - ech);
    }
    t0 = millis(); while (s_lbEchoed < sent && millis() - t0 < 3000) delay(10);
    uint32_t ms = millis() - tStart;
    CONSOLE.print("loopback_sent="); CONSOLE.print(sent); CONSOLE.print(" echoed="); CONSOLE.print(s_lbEchoed);
    CONSOLE.print(" bytes="); CONSOLE.print(s_lbBytes); CONSOLE.print(" ms="); CONSOLE.print(ms);
    CONSOLE.print(" kbps="); CONSOLE.println(ms ? (s_lbBytes * 8) / ms : 0);
    mode = 0x00; hci.run(0x1802, &mode, 1, &r, 1000, idleMs);
}
#endif
```
called after `probeFastBaud()` in `setup()` under `#if defined(M2_BT_LOOPBACK)`. (The credit rule here is deliberately crude and pessimistic — the real accounting from `0x13` events lands in Task 5's `L2cap`; this only needs to not overrun the controller.)

- [ ] **Step 3: Run on silicon, Wi-Fi up** — build with `-DM2_BT_LOOPBACK=ON` (the Wi-Fi driver is already running in the probe: `card=1`). *Assertion:* `loopback_sent=200 echoed=200` at 921600, and the `kbps` figure (expect ≥ 600 kbps of payload at 921 600 line rate). If `echoed<sent`, repeat at 3000000 and with the Wi-Fi throughput test running from the Mac (`tools/tput_peer.py` per the W11 notes) — the one-sided flow-control risk the spec names; record every number.

- [ ] **Step 4: Transcript + commit** — append a dated `Phase 0` section with the console lines (which rates answered, loopback numbers), then:

```bash
git add examples/networking/m2_hci_probe/m2_hci_probe.cpp examples/networking/m2_hci_probe/CMakeLists.txt examples/networking/m2_hci_probe/transcript_hw_evkb.txt
git commit -m "m2_hci_probe: phase 0 on silicon -- HCI at 921600 + loopback loss measured"
```

---

## Phase 1 — `M2Radio/bt/`: extraction, credits, the fake acceptor

All `bt/` code is pure C++11 with no Arduino dependency (like `hci/`), so it is host-tested. The app forwards HCI events and ACL data into these objects from its own `hci.onEvent`/`hci.onAcl` handlers (record-only paths); every transmission happens inside a `service()` called from the main loop.

### Task 5: `L2cap` — signalling, CO channels, ACL demux, credits

**Files:**
- Create: `M2Radio/bt/L2cap.h`, `M2Radio/bt/L2cap.cpp`
- Create: `M2Radio/bt/test/run.sh`, `M2Radio/bt/test/l2cap_test.cpp`

- [ ] **Step 1: The failing test** — `M2Radio/bt/test/l2cap_test.cpp`. The harness captures what `L2cap` writes through an `HciIo`, feeds it ACL packets as the controller would deliver them, and checks bytes.

```cpp
// Host tests for L2cap: the receiver-side SCID rule, mandatory replies, credits.
#include "L2cap.h"
#include <stdio.h>
#include <string.h>
#include <vector>
static int g_fails = 0, g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
struct CapIo : HciIo {                       // records TX; RX unused (L2cap is fed directly)
    std::vector<std::vector<uint8_t> > tx; uint32_t now = 0;
    size_t write(const uint8_t *p, size_t n) override { tx.push_back(std::vector<uint8_t>(p, p + n)); return n; }
    int available() override { return 0; } int read() override { return -1; } uint32_t nowMs() override { return now; }
};
// ACL packet as Hci hands it to onAcl(): [l2cap len lo, hi][cid lo, hi][payload]
static std::vector<uint8_t> l2(uint16_t cid, std::initializer_list<uint8_t> pl) {
    std::vector<uint8_t> v = { (uint8_t)pl.size(), 0, (uint8_t)cid, (uint8_t)(cid >> 8) }; v.insert(v.end(), pl); return v; }
int main() {
    {   // 1. Config Response to the peer's Config Request names the PEER's CID (receiver-side rule), echoes its options
        CapIo io; L2cap l(io); l.begin(0x0001, 7);
        L2cap::Channel *ch = l.connect(0x0019, 0x0041);           // our SCID 0x0041
        io.tx.clear();
        std::vector<uint8_t> rsp = l2(0x0001, {0x03, 0x10, 8, 0, 0x40, 0x03, 0x41, 0x00, 0, 0, 0, 0}); // Conn Rsp: dcid=0x0340 scid=0x0041 ok
        l.onAcl(0x0001, rsp.data(), (uint16_t)rsp.size());
        std::vector<uint8_t> req = l2(0x0001, {0x04, 1, 8, 0, 0x41, 0x00, 0, 0, 0x01, 0x02, 0x7F, 0x03}); // peer Cfg Req: dcid=ours, MTU 895
        l.onAcl(0x0001, req.data(), (uint16_t)req.size());
        l.service();
        bool found = false;
        for (auto &t : io.tx) if (t.size() >= 9 + 4 && t[9] == 0x05) {         // Config Response
            found = true;
            CHECK(t[9 + 4] == 0x40 && t[9 + 5] == 0x03);                       // SCID = peer's 0x0340, NOT ours
            CHECK(t[9 + 8] == 0x00 && t[9 + 9] == 0x00);                       // Result success
            CHECK(t.size() == 9 + 10 + 4 && t[9 + 10] == 0x01 && t[9 + 12] == 0x7F); // options echoed
        }
        CHECK(found); CHECK(ch->mtuOut == 895);
    }
    {   // 2. Information Request (ext features) and Echo Request are answered from service()
        CapIo io; L2cap l(io); l.begin(0x0001, 7);
        std::vector<uint8_t> inf = l2(0x0001, {0x0A, 2, 2, 0, 0x02, 0x00});
        std::vector<uint8_t> ech = l2(0x0001, {0x08, 5, 0, 0});
        l.onAcl(0x0001, inf.data(), (uint16_t)inf.size()); l.onAcl(0x0001, ech.data(), (uint16_t)ech.size());
        CHECK(io.tx.empty());                                                  // nothing sent from the RX path
        l.service();
        CHECK(io.tx.size() == 2);
        CHECK(io.tx[0][9] == 0x0B && io.tx[0][9 + 1] == 2 && io.tx[0][9 + 4] == 0x02 && io.tx[0][9 + 6] == 0x00); // Info Rsp type 2, success
        CHECK(io.tx[1][9] == 0x09 && io.tx[1][9 + 1] == 5);                   // Echo Rsp, same id
    }
    {   // 3. Credits: acl_num=2 -> the third data packet waits for a Number_Of_Completed_Packets
        CapIo io; L2cap l(io); l.begin(0x0001, 2);
        uint8_t d[4] = {1, 2, 3, 4};
        CHECK(l.send(0x0340, d, 4)); CHECK(l.send(0x0340, d, 4)); CHECK(l.send(0x0340, d, 4));
        l.service(); CHECK(io.tx.size() == 2); CHECK(l.credits() == 0);
        uint8_t ncp[5] = { 1, 0x01, 0x00, 0x01, 0x00 };                        // 1 handle, 0x0001, 1 completed
        l.onEvent(0x13, ncp, 5); l.service();
        CHECK(io.tx.size() == 3); CHECK(l.credits() == 0);
    }
    printf("l2cap_test: %d checks, %d failures\n", g_checks, g_fails); return g_fails ? 1 : 0;
}
```
`M2Radio/bt/test/run.sh` (twin of `hci/test/run.sh`; compiles `bt/*.cpp` + `hci/H4Parser.cpp` if needed):
```sh
#!/bin/sh
set -e
DIR=$(cd "$(dirname "$0")" && pwd); OUT=$(mktemp -d); trap 'rm -rf "$OUT"' EXIT
CXX=${CXX:-c++}
for t in l2cap_test avdtp_test sbc_test; do
    [ -f "$DIR/$t.cpp" ] || continue
    $CXX -std=c++11 -Wall -Wextra -Werror -I"$DIR/.." -I"$DIR/../../hci" "$DIR/$t.cpp" "$DIR"/../*.cpp -o "$OUT/$t"
    "$OUT/$t"
done
echo "BT-HOST-TESTS: PASS"
```
- [ ] **Step 2: Run it, expect failure** — `chmod +x ~/Development/M2Radio/bt/test/run.sh && ~/Development/M2Radio/bt/test/run.sh` → compile error: `L2cap.h: No such file`.

- [ ] **Step 3: Implement** — `M2Radio/bt/L2cap.h`:
```cpp
// L2cap -- basic-mode L2CAP over one ACL link: the signalling channel, up to
// MAX_CHANNELS connection-oriented channels, ACL demux by CID, and ACL credit
// accounting from Number_Of_Completed_Packets.  Pure C++, no heap.  RX entry
// points only RECORD; every byte is transmitted from service() -- writing to
// the transport from the RX pump bus-faults (B6, 2026-08-28).  MIT, clean-room.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "HciIo.h"
class L2cap {
public:
    static const uint8_t MAX_CHANNELS = 3;      // SDP, AVDTP signalling, AVDTP media
    static const uint8_t MAX_OPTS = 32;
    enum State : uint8_t { FREE, WAIT_CONN, CONFIG, OPEN, CLOSED };
    struct Channel {
        State state; uint16_t psm, localCid, remoteCid, mtuOut, mtuIn;
        bool cfgReqSent, cfgRspRcvd, cfgReqSeen, cfgRspSent; uint8_t cfgReqId, optLen; uint8_t opts[MAX_OPTS];
        bool peerInitiated;
    };
    typedef void (*DataFn)(void *ctx, Channel &ch, const uint8_t *payload, uint16_t len);
    explicit L2cap(HciIo &io) : m_io(io) {}
    void begin(uint16_t aclHandle, uint8_t aclCredits, uint16_t aclMax = 1021);
    void onData(DataFn fn, void *ctx) { m_onData = fn; m_dataCtx = ctx; }
    // --- RX (record only) ---
    void onAcl(uint16_t handle, const uint8_t *d, uint16_t len);   // Hci::AclFn payload
    void onEvent(uint8_t code, const uint8_t *p, uint8_t len);      // needs 0x13 only
    // --- main context ---
    Channel *connect(uint16_t psm, uint16_t localCid);              // sends Connection Request on service()
    bool     send(uint16_t remoteCid, const uint8_t *payload, uint16_t len); // queued, credit-paced
    void     service();
    Channel *byLocal(uint16_t cid); Channel *byRemote(uint16_t cid); Channel *byPsm(uint16_t psm);
    uint8_t  credits() const { return m_credits; }
    uint32_t dropped() const { return m_dropped; }
    void     acceptIncoming(bool yes) { m_accept = yes; }          // peer-initiated channels (answered with our next free CID)
private:
    struct Tx { uint16_t cid; uint16_t len; uint8_t buf[700]; bool used; };
    static const uint8_t TXQ = 8;
    void sig(const uint8_t *cmd, uint16_t len);                     // queue a signalling command
    void handleSig(const uint8_t *d, uint16_t len);
    HciIo &m_io; uint16_t m_handle, m_aclMax; uint8_t m_credits; bool m_accept;
    Channel m_ch[MAX_CHANNELS]; uint8_t m_nextId; uint16_t m_nextCid;
    Tx m_txq[TXQ]; uint8_t m_txHead, m_txCount; uint32_t m_dropped;
    struct Pending { bool infoReq; uint8_t infoId; uint16_t infoType; bool echoReq; uint8_t echoId;
                     bool connReq; uint8_t connId; uint16_t connPsm, connScid; } m_p;
    DataFn m_onData; void *m_dataCtx;
};
```
`M2Radio/bt/L2cap.cpp` — the rules the tests pin: Config Response SCID = peer's CID; options echoed; Info/Echo answered; credits = `acl_num`, decremented per ACL write, restored from 0x13's per-handle completed count.
```cpp
#include "L2cap.h"
#include <string.h>
enum { CONN_REQ = 0x02, CONN_RSP = 0x03, CFG_REQ = 0x04, CFG_RSP = 0x05, DISC_REQ = 0x06, DISC_RSP = 0x07,
       ECHO_REQ = 0x08, ECHO_RSP = 0x09, INFO_REQ = 0x0A, INFO_RSP = 0x0B };
void L2cap::begin(uint16_t h, uint8_t credits, uint16_t aclMax) {
    m_handle = h; m_credits = credits; m_aclMax = aclMax; m_accept = false;
    memset(m_ch, 0, sizeof m_ch); memset(&m_p, 0, sizeof m_p); memset(m_txq, 0, sizeof m_txq);
    m_nextId = 0x10; m_nextCid = 0x0040; m_txHead = m_txCount = 0; m_dropped = 0;
}
L2cap::Channel *L2cap::byLocal(uint16_t c)  { for (auto &ch : m_ch) if (ch.state != FREE && ch.localCid == c)  return &ch; return nullptr; }
L2cap::Channel *L2cap::byRemote(uint16_t c) { for (auto &ch : m_ch) if (ch.state != FREE && ch.remoteCid == c) return &ch; return nullptr; }
L2cap::Channel *L2cap::byPsm(uint16_t p)    { for (auto &ch : m_ch) if (ch.state != FREE && ch.psm == p)       return &ch; return nullptr; }
L2cap::Channel *L2cap::connect(uint16_t psm, uint16_t localCid) {
    for (auto &ch : m_ch) if (ch.state == FREE) {
        memset(&ch, 0, sizeof ch); ch.state = WAIT_CONN; ch.psm = psm; ch.localCid = localCid; ch.mtuOut = 672; ch.mtuIn = 672;
        uint8_t c[8] = { CONN_REQ, m_nextId++, 4, 0, (uint8_t)psm, (uint8_t)(psm >> 8), (uint8_t)localCid, (uint8_t)(localCid >> 8) };
        sig(c, 8); return &ch; }
    return nullptr;
}
bool L2cap::send(uint16_t cid, const uint8_t *pl, uint16_t len) {
    if (m_txCount == TXQ || len > 700 - 0) { m_dropped++; return false; }
    Tx &t = m_txq[(m_txHead + m_txCount) % TXQ]; t.cid = cid; t.len = len; memcpy(t.buf, pl, len); t.used = true; m_txCount++; return true;
}
void L2cap::sig(const uint8_t *cmd, uint16_t len) { send(0x0001, cmd, len); }
void L2cap::onEvent(uint8_t code, const uint8_t *p, uint8_t len) {
    if (code != 0x13 || len < 1) return;                             // Number_Of_Completed_Packets
    uint8_t n = p[0];
    for (uint8_t i = 0; i < n && (uint16_t)(1 + i * 4 + 3) < len; i++) {
        uint16_t h = (uint16_t)(p[1 + i * 4] | (p[2 + i * 4] << 8)); uint16_t c = (uint16_t)(p[3 + i * 4] | (p[4 + i * 4] << 8));
        if (h == m_handle) { uint32_t v = (uint32_t)m_credits + c; m_credits = v > 255 ? 255 : (uint8_t)v; }
    }
}
void L2cap::onAcl(uint16_t handle, const uint8_t *d, uint16_t len) {
    if (handle != m_handle || len < 4) return;
    uint16_t cid = (uint16_t)(d[2] | (d[3] << 8));
    if (cid == 0x0001) { handleSig(d, len); return; }
    Channel *ch = byLocal(cid);
    if (ch && m_onData) m_onData(m_dataCtx, *ch, d + 4, (uint16_t)(len - 4));
}
void L2cap::handleSig(const uint8_t *d, uint16_t len) {
    if (len < 8) return;
    uint8_t code = d[4], id = d[5];
    switch (code) {
    case CONN_RSP: if (len >= 16) {
        uint16_t dcid = (uint16_t)(d[8] | (d[9] << 8)), scid = (uint16_t)(d[10] | (d[11] << 8)), res = (uint16_t)(d[12] | (d[13] << 8));
        Channel *ch = byLocal(scid); if (!ch) break;
        if (res == 0x0001) break;                                    // pending: wait for the final response
        if (res != 0) { ch->state = CLOSED; break; }
        ch->remoteCid = dcid; ch->state = CONFIG; } break;
    case CFG_REQ: if (len >= 12) {
        uint16_t dcid = (uint16_t)(d[8] | (d[9] << 8)); Channel *ch = byLocal(dcid); if (!ch) break;
        uint16_t cmdLen = (uint16_t)(d[6] | (d[7] << 8)); uint16_t optLen = cmdLen > 4 ? (uint16_t)(cmdLen - 4) : 0;
        if ((uint16_t)(12 + optLen) > len) optLen = len > 12 ? (uint16_t)(len - 12) : 0;
        if (optLen > MAX_OPTS) optLen = MAX_OPTS;
        memcpy(ch->opts, d + 12, optLen); ch->optLen = (uint8_t)optLen; ch->cfgReqId = id; ch->cfgReqSeen = true; ch->cfgRspSent = false;
        for (uint16_t i = 0; i + 3 < optLen; ) { uint8_t t = ch->opts[i], l = ch->opts[i + 1];       // MTU option: our outgoing limit
            if (t == 0x01 && l == 2) ch->mtuOut = (uint16_t)(ch->opts[i + 2] | (ch->opts[i + 3] << 8)); i += (uint16_t)(2 + l); } } break;
    case CFG_RSP: if (len >= 14) {
        uint16_t scid = (uint16_t)(d[8] | (d[9] << 8)), res = (uint16_t)(d[12] | (d[13] << 8));
        Channel *ch = byLocal(scid); if (!ch) ch = byRemote(scid);   // tolerate either convention on receive
        if (ch && res == 0) ch->cfgRspRcvd = true; } break;
    case CONN_REQ: if (len >= 12) { m_p.connReq = true; m_p.connId = id;
        m_p.connPsm = (uint16_t)(d[8] | (d[9] << 8)); m_p.connScid = (uint16_t)(d[10] | (d[11] << 8)); } break;
    case INFO_REQ: if (len >= 10) { m_p.infoReq = true; m_p.infoId = id; m_p.infoType = (uint16_t)(d[8] | (d[9] << 8)); } break;
    case ECHO_REQ: m_p.echoReq = true; m_p.echoId = id; break;
    case DISC_REQ: if (len >= 12) { Channel *ch = byLocal((uint16_t)(d[8] | (d[9] << 8))); if (ch) ch->state = CLOSED;
        uint8_t r[8] = { DISC_RSP, id, 4, 0, d[8], d[9], d[10], d[11] }; sig(r, 8); } break;
    default: break;
    }
}
void L2cap::service() {
    if (m_p.infoReq) { m_p.infoReq = false; uint8_t r[16]; uint16_t n = 0;
        r[n++] = INFO_RSP; r[n++] = m_p.infoId; n += 2; r[n++] = (uint8_t)m_p.infoType; r[n++] = (uint8_t)(m_p.infoType >> 8);
        if (m_p.infoType == 0x0002)      { r[n++] = 0; r[n++] = 0; r[n++] = 0; r[n++] = 0; r[n++] = 0; r[n++] = 0; }   // ext features: none
        else if (m_p.infoType == 0x0003) { r[n++] = 0; r[n++] = 0; r[n++] = 0x02; for (int i = 0; i < 7; i++) r[n++] = 0; } // fixed: signalling
        else                             { r[n++] = 1; r[n++] = 0; }                                                       // not supported
        r[2] = (uint8_t)(n - 4); r[3] = (uint8_t)((n - 4) >> 8); sig(r, n); }
    if (m_p.echoReq) { m_p.echoReq = false; uint8_t r[4] = { ECHO_RSP, m_p.echoId, 0, 0 }; sig(r, 4); }
    if (m_p.connReq) { m_p.connReq = false; Channel *ch = nullptr;
        if (m_accept) for (auto &c : m_ch) if (c.state == FREE) { ch = &c; break; }
        uint16_t res = ch ? 0x0000 : 0x0004, local = 0;                                   // 0x0004 = no resources
        if (ch) { memset(ch, 0, sizeof *ch); ch->state = CONFIG; ch->psm = m_p.connPsm; ch->remoteCid = m_p.connScid;
                  ch->localCid = local = m_nextCid++; ch->mtuOut = 672; ch->mtuIn = 672; ch->peerInitiated = true; }
        uint8_t r[12] = { CONN_RSP, m_p.connId, 8, 0, (uint8_t)local, (uint8_t)(local >> 8), (uint8_t)m_p.connScid, (uint8_t)(m_p.connScid >> 8),
                          (uint8_t)res, (uint8_t)(res >> 8), 0, 0 }; sig(r, 12); }
    for (auto &ch : m_ch) {
        if (ch.state == CONFIG && !ch.cfgReqSent) { ch.cfgReqSent = true;                 // our Config Request: no options (defaults)
            uint8_t c[8] = { CFG_REQ, m_nextId++, 4, 0, (uint8_t)ch.remoteCid, (uint8_t)(ch.remoteCid >> 8), 0, 0 }; sig(c, 8); }
        if (ch.state == CONFIG && ch.cfgReqSeen && !ch.cfgRspSent) { ch.cfgRspSent = true;
            uint8_t r[10 + MAX_OPTS]; uint16_t n = (uint16_t)(6 + ch.optLen);
            r[0] = CFG_RSP; r[1] = ch.cfgReqId; r[2] = (uint8_t)n; r[3] = (uint8_t)(n >> 8);
            r[4] = (uint8_t)ch.remoteCid; r[5] = (uint8_t)(ch.remoteCid >> 8);           // ★ SCID = the PEER's CID (receiver-side rule)
            r[6] = r[7] = 0; r[8] = r[9] = 0; memcpy(r + 10, ch.opts, ch.optLen); sig(r, (uint16_t)(10 + ch.optLen)); }
        if (ch.state == CONFIG && ch.cfgRspRcvd && ch.cfgRspSent) ch.state = OPEN;
    }
    while (m_txCount && m_credits) {                                                       // credit-paced ACL writes
        Tx &t = m_txq[m_txHead]; uint16_t al = (uint16_t)(t.len + 4); uint16_t hf = (uint16_t)((m_handle & 0x0FFF) | (0x02u << 12));
        uint8_t h[9] = { 0x02, (uint8_t)hf, (uint8_t)(hf >> 8), (uint8_t)al, (uint8_t)(al >> 8), (uint8_t)t.len, (uint8_t)(t.len >> 8), (uint8_t)t.cid, (uint8_t)(t.cid >> 8) };
        uint8_t pkt[9 + 700]; memcpy(pkt, h, 9); memcpy(pkt + 9, t.buf, t.len); m_io.write(pkt, (size_t)(9 + t.len));
        t.used = false; m_txHead = (uint8_t)((m_txHead + 1) % TXQ); m_txCount--; m_credits--;
    }
}
```
Note for the implementer: `send()` from the **RX** callbacks is fine — it only queues; the transport write happens in `service()`. That is the whole point of the queue.

- [ ] **Step 4: Run** — `~/Development/M2Radio/bt/test/run.sh` → `l2cap_test: N checks, 0 failures`, `BT-HOST-TESTS: PASS`.

- [ ] **Step 5: Commit (M2Radio)** — `git add bt/L2cap.h bt/L2cap.cpp bt/test/run.sh bt/test/l2cap_test.cpp && git commit -m "bt: L2cap -- basic-mode signalling, CO channels, ACL demux, credits (host-tested; SCID rule pinned)"`

### Task 6: `BtLink` — inquiry-by-name, connect, pair (SSP → PIN), encrypt

**Files:**
- Create: `M2Radio/bt/BtLink.h`, `M2Radio/bt/BtLink.cpp`

This is `probeConnect()`'s HCI half moved into a class; it is exercised on silicon (Task 9) and by the `[avdtp]` gate (the peer accepts pairing), so it has no separate host test. Interface first, the body ports the probe's code line for line (the probe is the reference — copy its constants and event handling; they are proven on three peers).

- [ ] **Step 1: Header**
```cpp
// BtLink -- one BR/EDR ACL link: inquiry by name, Create_Connection, SSP
// pairing with legacy-PIN fallback, encryption.  Blocking helpers for setup();
// the SSP/PIN events are answered by onEvent() (submit only, no run()).
#pragma once
#include <stdint.h>
#include "Hci.h"
class BtLink {
public:
    enum Result : uint8_t { OK = 0, NO_INQUIRY_HIT, CONNECT_STATUS, PAIRING_FAILED, PIN_FAILED, ENCRYPTION_FAILED, TIMEOUT };
    static const char *resultName(Result r);
    explicit BtLink(Hci &hci) : m_hci(hci) {}
    void setPin(const char *pin4) { for (int i = 0; i < 4; i++) m_pin[i] = pin4[i]; }
    Result connect(const char *nameSubstr, void (*idle)());     // inquiry (10 s) -> connect -> pair -> encrypt
    Result pairAndEncrypt(void (*idle)());                       // SSP first; on failure Write_Simple_Pairing_Mode=0 and retry with PIN
    void onEvent(uint8_t code, const uint8_t *p, uint8_t len);   // forward from the app's Hci::EventFn
    uint16_t handle() const { return m_handle; } const uint8_t *peer() const { return m_bd; }
    bool encrypted() const { return m_encrypted; } const char *pairedBy() const { return m_pairedBy; }
private:
    Hci &m_hci; uint16_t m_handle = 0; uint8_t m_bd[6] = {0}; uint8_t m_psrm = 0; uint16_t m_clk = 0;
    char m_pin[4] = {'1','2','3','4'}; const char *m_pairedBy = "none";
    volatile bool m_connDone = false, m_authDone = false, m_encDone = false, m_found = false, m_named = false;
    volatile uint8_t m_connStatus = 0xFF, m_authStatus = 0xFF, m_encStatus = 0xFF; volatile bool m_encrypted = false;
    char m_name[249] = {0}; char m_want[32] = {0};
};
```
- [ ] **Step 2: Body** — port from the probe: `connect()` = `OP_INQUIRY` (LAP 0x9E8B33, 10 × 1.28 s, unlimited) collecting Inquiry Result events (field-major parse as `probeInquiry()` does) and `Remote_Name_Request` per A/V hit, choosing the first whose name contains `nameSubstr`; then `Set_Event_Mask` all-ones, `Write_Simple_Pairing_Mode=1`, `Create_Connection` (pkt 0xCC18, allow role switch 1), wait `Connection_Complete`. `pairAndEncrypt()` = `Authentication_Requested` → wait `Auth_Complete` (25 s); on failure: `Write_Simple_Pairing_Mode=0`, `Authentication_Requested` again, answer `PIN_Code_Request` with `m_pin` (reply opcode 0x040D: bd(6) len(1)=4 pin(16) zero-padded); on `Auth_Complete` ok → `Set_Connection_Encryption` on → wait `Encryption_Change` enabled=1. `onEvent()` handles 0x03, 0x06, 0x08, 0x16, 0x17 (link key req → negative reply 0x040C), 0x18, 0x31 (IO cap reply 0x042B: NoInputNoOutput, OOB 0, auth 0x04), 0x33 (User Confirmation reply 0x042C accept), 0x36, 0x02/0x01/0x07 (inquiry). All replies via `m_hci.submit(...)`. Print one `key=value` line per stage through a `Print`-free callback? No — `bt/` has no Arduino: expose the outcome and let the app print. `resultName()` returns the spec's names (`no_inquiry_hit`, `connect_status`, `pairing_ssp_failed→pin_fallback` is reported via `pairedBy()` = "ssp" | "pin").

- [ ] **Step 3: Build check** — the bt tests compile `bt/*.cpp`; `BtLink.cpp` includes only `Hci.h` (host-compilable). Run `~/Development/M2Radio/bt/test/run.sh` → still `PASS`.

- [ ] **Step 4: Commit (M2Radio)** — `git add bt/BtLink.h bt/BtLink.cpp && git commit -m "bt: BtLink -- inquiry by name, connect, SSP with PIN fallback, encryption (ported from the probe)"`

### Task 7: `Sdp` client and `Avdtp` initiator

**Files:**
- Create: `M2Radio/bt/Sdp.h`, `M2Radio/bt/Sdp.cpp`, `M2Radio/bt/Avdtp.h`, `M2Radio/bt/Avdtp.cpp`
- Create: `M2Radio/bt/test/avdtp_test.cpp`

Both are pure PDU builders/parsers over an `L2cap::Channel`; the test checks bytes, which is exactly what the fake acceptor (Task 8) and the sinks will check.

- [ ] **Step 1: The failing test** — `M2Radio/bt/test/avdtp_test.cpp`
```cpp
#include "Avdtp.h"
#include "Sdp.h"
#include <stdio.h>
#include <string.h>
static int g_fails = 0, g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
int main() {
    {   // 1. SDP: the AudioSink/ProtocolDescriptorList request is the exact 18 bytes proven on three peers
        uint8_t b[32]; uint16_t n = Sdp::buildAudioSinkPdlRequest(b, 0x0001);
        static const uint8_t want[18] = { 0x06, 0x00,0x01, 0x00,0x0D, 0x35,0x03,0x19,0x11,0x0B, 0x03,0xF0, 0x35,0x03,0x09,0x00,0x04, 0x00 };
        CHECK(n == 18 && memcmp(b, want, 18) == 0);
        // and the response seen on the wire yields AVDTP v1.3
        static const uint8_t rsp[39] = { 0x07,0x00,0x01,0x00,0x1E,0x00,0x1B,0x36,0x00,0x18,0x36,0x00,0x15,0x09,0x00,0x04,0x35,0x10,
                                         0x35,0x06,0x19,0x01,0x00,0x09,0x00,0x19, 0x35,0x06,0x19,0x00,0x19,0x09,0x01,0x03, 0x00 };
        CHECK(Sdp::parseAvdtpVersion(rsp, 39) == 0x0103);
    }
    {   // 2. AVDTP command encodings (single packet, our transaction labels)
        uint8_t b[32]; uint16_t n;
        n = Avdtp::buildDiscover(b, 1);                       CHECK(n == 2 && b[0] == 0x10 && b[1] == 0x01);
        n = Avdtp::buildGetCapabilities(b, 2, 1);             CHECK(n == 3 && b[0] == 0x20 && b[1] == 0x02 && b[2] == (1 << 2));
        Avdtp::SbcConfig cfg = { 44100, Avdtp::JOINT_STEREO, 16, 8, Avdtp::LOUDNESS, 2, 53 };
        n = Avdtp::buildSetConfiguration(b, 3, /*acp seid*/ 1, /*int seid*/ 1, cfg);
        // hdr, sig, ACP SEID<<2, INT SEID<<2, [cat 1 media transport, len 0], [cat 7 media codec, len 6: media type audio<<4, codec SBC=0, cie 21 15 02 35]
        static const uint8_t want[14] = { 0x30, 0x03, 1 << 2, 1 << 2, 0x01, 0x00, 0x07, 0x06, 0x00, 0x00, 0x21, 0x15, 0x02, 0x35 };
        CHECK(n == 14 && memcmp(b, want, 14) == 0);
        n = Avdtp::buildOpen(b, 4, 1);                        CHECK(n == 3 && b[0] == 0x40 && b[1] == 0x06 && b[2] == (1 << 2));
        n = Avdtp::buildStart(b, 5, 1);                       CHECK(n == 3 && b[0] == 0x50 && b[1] == 0x07 && b[2] == (1 << 2));
    }
    {   // 3. Parsing: a Discover accept with two SNK SEPs, and SBC capabilities out of GET_CAPABILITIES
        static const uint8_t disc[6] = { 0x12, 0x01, 0x04, 0x08, 0x08, 0x08 };
        Avdtp::Sep seps[4]; uint8_t n = Avdtp::parseDiscover(disc, 6, seps, 4);
        CHECK(n == 2 && seps[0].seid == 1 && seps[0].sink && seps[0].audio && seps[1].seid == 2);
        static const uint8_t caps[12] = { 0x22, 0x02, 0x01, 0x00, 0x07, 0x06, 0x00, 0x00, 0xFF, 0xFF, 0x02, 0x35 };  // all rates/modes, all blocks/subbands/alloc, bitpool 2..53
        Avdtp::SbcCaps c; CHECK(Avdtp::parseSbcCaps(caps, 12, c));
        CHECK(c.rates == 0xF && c.modes == 0xF && c.blocks == 0xF && c.subbands == 0x3 && c.alloc == 0x3 && c.minBitpool == 2 && c.maxBitpool == 53);
        static const uint8_t rej[3] = { 0x33, 0x03, 0x29 };   // SET_CONFIGURATION reject, error 0x29 (unsupported configuration)
        CHECK(Avdtp::responseType(rej[0]) == Avdtp::REJECT && Avdtp::rejectError(rej, 3) == 0x29);
    }
    printf("avdtp_test: %d checks, %d failures\n", g_checks, g_fails); return g_fails ? 1 : 0;
}
```
- [ ] **Step 2: Run, expect compile failure** — `~/Development/M2Radio/bt/test/run.sh` → `Sdp.h: No such file`.

- [ ] **Step 3: `Sdp.h/.cpp`**
```cpp
// Sdp -- the one client query BT-3 needs: the AudioSink service's
// ProtocolDescriptorList, from which the AVDTP version is read.  Clean-room
// from the SDP data-element grammar (Core Vol 3 Part B).  MIT.
#pragma once
#include <stdint.h>
struct Sdp {
    static const uint16_t PSM = 0x0001;
    // ServiceSearchAttributeRequest: DES{UUID16 0x110B}, max 1008 bytes, DES{UINT16 attr 0x0004}, no continuation
    static uint16_t buildAudioSinkPdlRequest(uint8_t *out, uint16_t txn);
    // Scans a ServiceSearchAttributeResponse for [UUID16 0x0019][UINT16 version]; 0 if absent
    static uint16_t parseAvdtpVersion(const uint8_t *rsp, uint16_t len);
};
```
```cpp
#include "Sdp.h"
uint16_t Sdp::buildAudioSinkPdlRequest(uint8_t *o, uint16_t txn) {
    const uint8_t b[18] = { 0x06, (uint8_t)(txn >> 8), (uint8_t)txn, 0x00, 0x0D, 0x35,0x03,0x19,0x11,0x0B, 0x03,0xF0, 0x35,0x03,0x09,0x00,0x04, 0x00 };
    for (int i = 0; i < 18; i++) o[i] = b[i]; return 18;
}
uint16_t Sdp::parseAvdtpVersion(const uint8_t *r, uint16_t len) {
    if (len < 5 || r[0] != 0x07) return 0;
    for (uint16_t i = 5; i + 5 < len; i++)
        if (r[i] == 0x19 && r[i + 1] == 0x00 && r[i + 2] == 0x19 && r[i + 3] == 0x09) return (uint16_t)((r[i + 4] << 8) | r[i + 5]);
    return 0;
}
```
- [ ] **Step 4: `Avdtp.h/.cpp`** — builders/parsers (stateless) plus a small initiator state machine driven from `service()`; AVDTP v1.3 signalling: header = `tlabel<<4 | packet_type<<2 | msg_type`; signal ids DISCOVER 1, GET_CAPABILITIES 2, SET_CONFIGURATION 3, OPEN 6, START 7, CLOSE 8, SUSPEND 9, ABORT 10; message types CMD 0, GENERAL_REJECT 1, ACCEPT 2, REJECT 3; service categories MEDIA_TRANSPORT 1, MEDIA_CODEC 7; SBC codec info element: byte0 = rate bits (16k 0x80, 32k 0x40, 44.1k 0x20, 48k 0x10) | mode bits (mono 8, dual 4, stereo 2, joint 1); byte1 = blocks (4:0x80 8:0x40 12:0x20 16:0x10) | subbands (4:0x08 8:0x04) | alloc (SNR 0x02, loudness 0x01); byte2 min bitpool; byte3 max bitpool.
```cpp
#pragma once
#include <stdint.h>
#include "L2cap.h"
struct Avdtp {
    static const uint16_t PSM = 0x0019;
    enum Mode : uint8_t { MONO = 8, DUAL = 4, STEREO = 2, JOINT_STEREO = 1 };
    enum Alloc : uint8_t { SNR = 2, LOUDNESS = 1 };
    enum MsgType : uint8_t { COMMAND = 0, GENERAL_REJECT = 1, ACCEPT = 2, REJECT = 3 };
    struct SbcConfig { uint32_t rate; Mode mode; uint8_t blocks, subbands; Alloc alloc; uint8_t minBitpool, maxBitpool; };
    struct SbcCaps   { uint8_t rates, modes, blocks, subbands, alloc, minBitpool, maxBitpool; };
    struct Sep       { uint8_t seid; bool inUse, audio, sink; };
    static uint16_t buildDiscover(uint8_t *o, uint8_t tl);
    static uint16_t buildGetCapabilities(uint8_t *o, uint8_t tl, uint8_t acpSeid);
    static uint16_t buildSetConfiguration(uint8_t *o, uint8_t tl, uint8_t acpSeid, uint8_t intSeid, const SbcConfig &c);
    static uint16_t buildOpen(uint8_t *o, uint8_t tl, uint8_t acpSeid);
    static uint16_t buildStart(uint8_t *o, uint8_t tl, uint8_t acpSeid);
    static uint16_t buildDiscoverAcceptOneSource(uint8_t *o, uint8_t hdrFromPeer); // answers a peer's DISCOVER: SEID 1, audio, SRC
    static MsgType  responseType(uint8_t hdr) { return (MsgType)(hdr & 0x03); }
    static uint8_t  rejectError(const uint8_t *p, uint16_t len);          // last byte of a REJECT
    static uint8_t  parseDiscover(const uint8_t *p, uint16_t len, Sep *out, uint8_t max);
    static bool     parseSbcCaps(const uint8_t *p, uint16_t len, SbcCaps &c);
    static void     sbcCie(const SbcConfig &c, uint8_t out[4]);
    // --- initiator, one stream ---
    enum State : uint8_t { IDLE, DISCOVERING, GETTING_CAPS, CONFIGURING, OPENING, MEDIA_CONNECTING, STARTING, STREAMING, FAILED };
    void begin(L2cap &l2, uint16_t sigLocalCid, uint16_t mediaLocalCid);
    bool start(const SbcConfig &want);        // kick off: DISCOVER on the (already OPEN) signalling channel
    void onSignalling(const uint8_t *p, uint16_t len);   // from the L2cap data callback, signalling channel (record only)
    void service();                            // main context: advance the state machine, send commands
    State state() const { return m_state; } uint8_t error() const { return m_err; } const SbcCaps &caps() const { return m_caps; }
    uint8_t acpSeid() const { return m_acp; } uint16_t mediaRemoteCid() const { return m_media ? m_media->remoteCid : 0; }
    uint16_t mediaMtu() const { return m_media ? m_media->mtuOut : 0; }
private:
    L2cap *m_l2 = nullptr; L2cap::Channel *m_sig = nullptr, *m_media = nullptr; uint16_t m_sigCid = 0, m_mediaCid = 0;
    State m_state = IDLE; uint8_t m_tl = 1, m_acp = 0, m_err = 0; SbcConfig m_want; SbcCaps m_caps;
    volatile bool m_rspSeen = false; uint8_t m_rsp[64]; uint16_t m_rspLen = 0; bool m_peerDiscover = false; uint8_t m_peerHdr = 0;
    void send(const uint8_t *b, uint16_t n) { m_l2->send(m_sig->remoteCid, b, n); }
};
```
`Avdtp.cpp` essentials (the builders follow the test's expected bytes exactly; the state machine sends one command per state and advances on `ACCEPT`, records the REJECT error otherwise):
```cpp
#include "Avdtp.h"
#include <string.h>
static uint8_t hdr(uint8_t tl, uint8_t mt) { return (uint8_t)((tl << 4) | mt); }
uint16_t Avdtp::buildDiscover(uint8_t *o, uint8_t tl) { o[0] = hdr(tl, COMMAND); o[1] = 0x01; return 2; }
uint16_t Avdtp::buildGetCapabilities(uint8_t *o, uint8_t tl, uint8_t s) { o[0] = hdr(tl, COMMAND); o[1] = 0x02; o[2] = (uint8_t)(s << 2); return 3; }
void Avdtp::sbcCie(const SbcConfig &c, uint8_t o[4]) {
    o[0] = (uint8_t)((c.rate == 16000 ? 0x80 : c.rate == 32000 ? 0x40 : c.rate == 44100 ? 0x20 : 0x10) | c.mode);
    o[1] = (uint8_t)((c.blocks == 4 ? 0x80 : c.blocks == 8 ? 0x40 : c.blocks == 12 ? 0x20 : 0x10) | (c.subbands == 4 ? 0x08 : 0x04) | c.alloc);
    o[2] = c.minBitpool; o[3] = c.maxBitpool;
}
uint16_t Avdtp::buildSetConfiguration(uint8_t *o, uint8_t tl, uint8_t acp, uint8_t intS, const SbcConfig &c) {
    o[0] = hdr(tl, COMMAND); o[1] = 0x03; o[2] = (uint8_t)(acp << 2); o[3] = (uint8_t)(intS << 2);
    o[4] = 0x01; o[5] = 0x00;                       // Media Transport, no parameters
    o[6] = 0x07; o[7] = 0x06; o[8] = 0x00; o[9] = 0x00; // Media Codec: audio (0<<4), SBC (0), 4-byte CIE
    sbcCie(c, o + 10); return 14;
}
uint16_t Avdtp::buildOpen(uint8_t *o, uint8_t tl, uint8_t s)  { o[0] = hdr(tl, COMMAND); o[1] = 0x06; o[2] = (uint8_t)(s << 2); return 3; }
uint16_t Avdtp::buildStart(uint8_t *o, uint8_t tl, uint8_t s) { o[0] = hdr(tl, COMMAND); o[1] = 0x07; o[2] = (uint8_t)(s << 2); return 3; }
uint16_t Avdtp::buildDiscoverAcceptOneSource(uint8_t *o, uint8_t peerHdr) { o[0] = (uint8_t)((peerHdr & 0xF0) | ACCEPT); o[1] = 0x01; o[2] = 1 << 2; o[3] = 0x00; return 4; }
uint8_t  Avdtp::rejectError(const uint8_t *p, uint16_t len) { return len ? p[len - 1] : 0; }
uint8_t  Avdtp::parseDiscover(const uint8_t *p, uint16_t len, Sep *out, uint8_t max) {
    if (len < 2 || responseType(p[0]) != ACCEPT) return 0; uint8_t n = 0;
    for (uint16_t i = 2; i + 1 < len && n < max; i += 2) { out[n].seid = (uint8_t)(p[i] >> 2); out[n].inUse = (p[i] >> 1) & 1;
        out[n].audio = (p[i + 1] >> 4) == 0; out[n].sink = (p[i + 1] >> 3) & 1; n++; }
    return n;
}
bool Avdtp::parseSbcCaps(const uint8_t *p, uint16_t len, SbcCaps &c) {
    if (len < 2 || responseType(p[0]) != ACCEPT) return false;
    for (uint16_t i = 2; i + 1 < len; ) { uint8_t cat = p[i], l = p[i + 1];
        if (cat == 0x07 && l >= 6 && i + 2 + 6 <= len && p[i + 2] == 0x00 && p[i + 3] == 0x00) { const uint8_t *e = p + i + 4;
            c.rates = (uint8_t)(e[0] >> 4); c.modes = (uint8_t)(e[0] & 0x0F); c.blocks = (uint8_t)(e[1] >> 4);
            c.subbands = (uint8_t)((e[1] >> 2) & 0x03); c.alloc = (uint8_t)(e[1] & 0x03); c.minBitpool = e[2]; c.maxBitpool = e[3]; return true; }
        i = (uint16_t)(i + 2 + l); }
    return false;
}
void Avdtp::begin(L2cap &l2, uint16_t sigCid, uint16_t mediaCid) { m_l2 = &l2; m_sigCid = sigCid; m_mediaCid = mediaCid; m_state = IDLE; m_tl = 1; }
bool Avdtp::start(const SbcConfig &want) { m_sig = m_l2->byLocal(m_sigCid); if (!m_sig || m_sig->state != L2cap::OPEN) return false;
    m_want = want; m_state = DISCOVERING; m_rspSeen = false; uint8_t b[4]; send(b, buildDiscover(b, m_tl)); return true; }
void Avdtp::onSignalling(const uint8_t *p, uint16_t len) {
    if (len < 2) return;
    if (responseType(p[0]) == COMMAND) { if (p[1] == 0x01) { m_peerDiscover = true; m_peerHdr = p[0]; } return; }   // peer's own DISCOVER: answered in service()
    if (len > sizeof m_rsp) len = sizeof m_rsp; memcpy(m_rsp, p, len); m_rspLen = len; m_rspSeen = true;
}
void Avdtp::service() {
    if (m_peerDiscover) { m_peerDiscover = false; uint8_t b[4]; send(b, buildDiscoverAcceptOneSource(b, m_peerHdr)); }
    if (m_state == MEDIA_CONNECTING) { if (m_media && m_media->state == L2cap::OPEN) { m_state = STARTING; m_rspSeen = false; uint8_t b[4]; send(b, buildStart(b, ++m_tl)); }
                                       return; }
    if (!m_rspSeen) return; m_rspSeen = false;
    if (responseType(m_rsp[0]) != ACCEPT) { m_err = rejectError(m_rsp, m_rspLen); m_state = FAILED; return; }
    uint8_t b[16];
    switch (m_state) {
    case DISCOVERING: { Sep s[4]; uint8_t n = parseDiscover(m_rsp, m_rspLen, s, 4); m_acp = 0;
        for (uint8_t i = 0; i < n; i++) if (s[i].audio && s[i].sink && !s[i].inUse) { m_acp = s[i].seid; break; }
        if (!m_acp) { m_err = 0xFF; m_state = FAILED; return; }
        m_state = GETTING_CAPS; send(b, buildGetCapabilities(b, ++m_tl, m_acp)); } break;
    case GETTING_CAPS: if (!parseSbcCaps(m_rsp, m_rspLen, m_caps)) { m_err = 0xFE; m_state = FAILED; return; }
        m_state = CONFIGURING; send(b, buildSetConfiguration(b, ++m_tl, m_acp, 1, m_want)); break;
    case CONFIGURING: m_state = OPENING; send(b, buildOpen(b, ++m_tl, m_acp)); break;
    case OPENING: m_state = MEDIA_CONNECTING; m_media = m_l2->connect(PSM, m_mediaCid); break;   // second channel = media transport
    case STARTING: m_state = STREAMING; break;
    default: break;
    }
}
```
- [ ] **Step 5: Run** — `~/Development/M2Radio/bt/test/run.sh` → `avdtp_test: N checks, 0 failures`, `BT-HOST-TESTS: PASS`.

- [ ] **Step 6: Commit (M2Radio)** — `git add bt/Sdp.h bt/Sdp.cpp bt/Avdtp.h bt/Avdtp.cpp bt/test/avdtp_test.cpp && git commit -m "bt: Sdp client + Avdtp initiator (DISCOVER..START, media channel), host-tested byte for byte"`

### Task 8: `hci_peer.py` grows an AVDTP acceptor (phase `avdtp`)

**Files:**
- Modify: `examples/networking/m2_hci_probe/hci_peer.py`

Today `feed()` rejects anything but H4 type 0x01 (commands). Phase `avdtp` must also accept **ACL** (type 0x02) from the host, run a minimal L2CAP acceptor, an SDP responder, and an AVDTP acceptor, and reply with ACL packets and the link-layer events the initiator waits for. Everything is encoded from the specs; the byte values below are the ones already measured on the wire against the real sinks.

- [ ] **Step 1: ACL parsing and framing helpers** (module level)
```python
def acl(handle, cid, payload):                    # controller -> host ACL, PB=10 (first, auto-flushable)
    hf = (handle & 0x0FFF) | (0x02 << 12)
    return bytes([0x02]) + struct.pack("<HH", hf, len(payload) + 4) + struct.pack("<HH", len(payload), cid) + payload
def ncp(handle, n=1):                             # Number_Of_Completed_Packets: the credit the host's L2cap pacing needs
    return event(0x13, bytes([1]) + struct.pack("<HH", handle, n))
SBC_CIE_EXPECT = bytes.fromhex("21150235")        # 44.1k joint / 16 blk 8 sub loudness / bitpool 2..53 -- the calibration config
```
In `LAST_OPCODE` add `"avdtp": 0x0413` (Set_Connection_Encryption — the last COMMAND before signalling takes over; the phase's real end is `PEER-AVDTP-STARTED`, checked separately).

- [ ] **Step 2: Connection + pairing acceptance** — in `Peer.handle`, phase `avdtp` answers the link-layer commands the probe sends (all values arbitrary but fixed; `HANDLE = 0x0001`):
```python
        elif opcode == 0x0C01 or opcode == 0x0C56 or opcode == 0x0C1A:      # Set_Event_Mask, Write_Simple_Pairing_Mode, Write_Scan_Enable
            self.send(cmd_complete(opcode, b"\x00"))
        elif opcode == 0x0405:                                              # Create_Connection -> Command Status, Connection Complete
            self.send(cmd_status(opcode)); self.send(event(0x03, b"\x00" + struct.pack("<H", 0x0001) + params[:6] + b"\x01\x00"), 0.1)
        elif opcode == 0x0411:                                              # Authentication_Requested: SSP Just Works, all the way to Auth Complete
            self.send(cmd_status(opcode)); bd = params_bd = self.peer_bd
            self.send(event(0x17, bd), 0.05)                                # Link_Key_Request
        elif opcode == 0x040C:                                              # Link_Key_Request_Negative_Reply -> IO cap dance
            self.send(cmd_complete(opcode, b"\x00" + params[:6])); self.send(event(0x31, params[:6]), 0.05)   # IO_Capability_Request
        elif opcode == 0x042B:                                              # IO_Capability_Request_Reply -> peer caps, user confirm
            self.send(cmd_complete(opcode, b"\x00" + params[:6]))
            self.send(event(0x32, params[:6] + b"\x03\x00\x04"), 0.05)      # IO_Capability_Response: NoInputNoOutput, no OOB, general bonding
            self.send(event(0x33, params[:6] + struct.pack("<I", 123456)), 0.1)   # User_Confirmation_Request
        elif opcode == 0x042C:                                              # User_Confirmation_Request_Reply -> pairing complete, link key, auth complete
            self.send(cmd_complete(opcode, b"\x00" + params[:6]))
            self.send(event(0x36, b"\x00" + params[:6]), 0.05)              # Simple_Pairing_Complete
            self.send(event(0x18, params[:6] + bytes(range(16)) + b"\x04"), 0.1)   # Link_Key_Notification (unauthenticated combination)
            self.send(event(0x06, b"\x00" + struct.pack("<H", 0x0001)), 0.15)      # Authentication_Complete
        elif opcode == 0x0413:                                              # Set_Connection_Encryption -> Encryption_Change on
            self.send(cmd_status(opcode)); self.send(event(0x08, b"\x00" + struct.pack("<H", 0x0001) + b"\x01"), 0.1)
```
(`self.peer_bd` is the BD_ADDR from the Create_Connection params; store it there. The phase must also answer `OP_INQUIRY`/`OP_REMOTE_NAME_REQ` as `full` does — reuse those branches; `DEVICES[0]` is the target, named `FAKE-HEADSET-01`, and the probe is built with `M2_BT_TARGET_NAME=FAKE-HEADSET-01`.)

- [ ] **Step 3: ACL path** — `feed()` gains a branch for type 0x02:
```python
            if self.buf[0] == 0x02:
                if len(self.buf) < 5: return
                hf, alen = struct.unpack("<HH", self.buf[1:5])
                if len(self.buf) < 5 + alen: return
                data, self.buf = self.buf[5:5 + alen], self.buf[5 + alen:]
                self.send(ncp(hf & 0x0FFF))                                   # every ACL packet frees a buffer
                self.handle_acl(hf & 0x0FFF, data); continue
```
and the acceptor state on `Peer.__init__`: `self.chans = {}` (`localcid_of_peer -> (our_cid, psm)`), `self.next_cid = 0x0340`, `self.avdtp = {"config": None, "opened": False, "started": False, "order": []}`.

- [ ] **Step 4: L2CAP acceptor + SDP responder + AVDTP acceptor**
```python
    def sig(self, handle, cmd): self.send(acl(handle, 0x0001, cmd), 0.02)
    def handle_acl(self, handle, d):
        if len(d) < 4: return
        l2len, cid = struct.unpack("<HH", d[:4]); pl = d[4:4 + l2len]
        if cid == 0x0001:                                                    # signalling
            code, ident = pl[0], pl[1]; body = pl[4:]
            if code == 0x02:                                                 # Connection Request: psm, scid
                psm, scid = struct.unpack("<HH", body[:4]); ours = self.next_cid; self.next_cid += 0x40
                self.chans[scid] = (ours, psm)
                self.sig(handle, bytes([0x03, ident, 8, 0]) + struct.pack("<HHHH", ours, scid, 0x0001, 0x0000))   # pending first, like the Shokz
                self.sig(handle, bytes([0x03, ident, 8, 0]) + struct.pack("<HHHH", ours, scid, 0x0000, 0x0000))
                self.sig(handle, bytes([0x04, ident + 1, 8, 0]) + struct.pack("<HH", scid, 0) + bytes([0x01, 0x02, 0xA0, 0x02]))  # our Config Request: MTU 672
            elif code == 0x04:                                               # Config Request for one of OUR endpoints: accept, echo options
                dcid = struct.unpack("<H", body[:2])[0]
                peer = [p for p, (o, _) in self.chans.items() if o == dcid]
                if not peer: self.log.append("PEER-L2CAP-CFG-UNKNOWN-DCID 0x%04x" % dcid); return
                opts = body[4:]
                self.sig(handle, bytes([0x05, ident, 6 + len(opts), 0]) + struct.pack("<HHH", peer[0], 0, 0) + opts)   # SCID = the host's CID
            elif code == 0x05:                                               # Config Response to ours: check the receiver-side SCID rule
                scid, flags, result = struct.unpack("<HHH", body[:6])
                if scid not in [o for (o, _) in self.chans.values()]: self.log.append("PEER-L2CAP-CFGRSP-BAD-SCID 0x%04x" % scid)
            elif code == 0x0A:                                               # Information Request: extended features none / fixed channels
                itype = struct.unpack("<H", body[:2])[0]
                data = b"\x00\x00\x00\x00" if itype == 2 else (b"\x02" + b"\x00" * 7 if itype == 3 else b"")
                self.sig(handle, bytes([0x0B, ident, 4 + len(data), 0]) + struct.pack("<HH", itype, 0 if data else 1) + data)
            elif code == 0x08: self.sig(handle, bytes([0x09, ident, 0, 0]))  # Echo
            return
        # data on one of our channels
        for peer_cid, (ours, psm) in self.chans.items():
            if ours == cid:
                if psm == 0x0001: self.handle_sdp(handle, peer_cid, pl)
                elif psm == 0x0019: self.handle_avdtp(handle, peer_cid, pl)
                return
        self.log.append("PEER-ACL-UNKNOWN-CID 0x%04x" % cid)
    def handle_sdp(self, handle, peer_cid, pl):
        if pl[0] != 0x06: return
        txn = pl[1:3]
        rsp = bytes.fromhex("07") + txn + bytes.fromhex("001E001B36001836001509000435103506190100090019350619001909010300")  # the PDL seen on the wire: AVDTP 1.3
        self.send(acl(handle, peer_cid, rsp), 0.02); self.log.append("PEER-SDP-ANSWERED")
    def handle_avdtp(self, handle, peer_cid, pl):
        hdr, sig = pl[0], pl[1]; tl = hdr & 0xF0; acc = bytes([tl | 0x02]); self.avdtp["order"].append(sig)
        if sig == 0x01:   self.send(acl(handle, peer_cid, acc + b"\x01" + bytes([1 << 2, 0x08])), 0.02)                         # DISCOVER: SEID 1, audio, SNK
        elif sig == 0x02: self.send(acl(handle, peer_cid, acc + b"\x02" + b"\x01\x00" + b"\x07\x06\x00\x00\xFF\xFF\x02\x35"), 0.02)  # caps: all, bitpool 2..53
        elif sig == 0x03:                                                                                                       # SET_CONFIGURATION: record the CIE
            self.avdtp["config"] = pl[10:14] if len(pl) >= 14 else b""
            self.log.append("PEER-SET-CONFIG cie=%s" % self.avdtp["config"].hex())
            self.send(acl(handle, peer_cid, acc + b"\x03"), 0.02)
        elif sig == 0x06: self.avdtp["opened"] = True; self.send(acl(handle, peer_cid, acc + b"\x06"), 0.02)                    # OPEN
        elif sig == 0x07:                                                                                                       # START: only legal after OPEN
            if not self.avdtp["opened"]: self.log.append("PEER-AVDTP-START-BEFORE-OPEN"); self.send(acl(handle, peer_cid, bytes([tl | 0x03, 0x07, 1 << 2, 0x31])), 0.02); return  # 0x31 = bad state
            self.avdtp["started"] = True; self.log.append("PEER-AVDTP-STARTED"); self.send(acl(handle, peer_cid, acc + b"\x07"), 0.02)
        else: self.send(acl(handle, peer_cid, bytes([tl | 0x03, sig, 0x19])), 0.02)                                             # unsupported command
```
End-of-phase for `avdtp`: replace the generic `LAST_OPCODE in peer.cmds` test with `self.avdtp["started"]` (in `__main__`: `ok = peer.avdtp["started"] if phase == "avdtp" else ...`), and print `PEER-AVDTP order=%s config=%s` at the end.

- [ ] **Step 5: `[hci]` still passes** — `./run_qemu_hci.sh | tail -1` → `PASS`. Commit: `git add hci_peer.py && git commit -m "m2_hci_probe: hci_peer.py [avdtp] phase -- L2CAP/SDP/AVDTP acceptor over ACL"`.

### Task 9: the probe on the library; existing gates unchanged

**Files:**
- Modify: `examples/networking/m2_hci_probe/m2_hci_probe.cpp`, `CMakeLists.txt`, `evkb.cmake`

- [ ] **Step 1: Push and pin M2Radio** — `cd ~/Development/M2Radio && git push origin master`; in `evkb.cmake` replace the M2Radio SHA with `git rev-parse HEAD` and append to its comment: `BT-3 phase 1 adds bt/ (L2cap, BtLink, Sdp, Avdtp; Sbc in phase 3).`

- [ ] **Step 2: Import** — `CMakeLists.txt`: `import_evkb_library(M2Radio sdio iw416 hci bt)`. Configure the bench build; expect `teensy_add_library(M2Radio ...bt/L2cap.cpp;...bt/Avdtp.cpp...)` in the output.

- [ ] **Step 3: Replace the prototype under `M2_BT_CONNECT`** — delete the probe's `sendL2cap`, `onAcl`, `serviceSignalling`, `serviceReverse`, `probeSdp`, `probeAvdtp` and their state; keep the prints. New objects: `static L2cap l2(hciIo); static BtLink link(hci); static Avdtp avdtp;`. The app's `onEvent` forwards `link.onEvent(code,p,len); l2.onEvent(code,p,len);` and prints as before; `hci.onAcl` → a thunk calling `l2.onAcl(handle, d, len)`; `l2.onData` → `if (ch.psm == Avdtp::PSM && ch.localCid == 0x0041) avdtp.onSignalling(payload, len); else if (ch.psm == Sdp::PSM) { s_sdpVer = Sdp::parseAvdtpVersion(payload, len); s_sdpDone = true; }`. `probeConnect()` becomes:
```cpp
    BtLink::Result r = link.connect(M2_BT_TARGET_NAME_OR_NULL, idleMs);      // prints per stage
    CONSOLE.print("link="); CONSOLE.println(BtLink::resultName(r)); if (r != BtLink::OK) return;
    r = link.pairAndEncrypt(idleMs);
    CONSOLE.print("secure="); CONSOLE.print(BtLink::resultName(r)); CONSOLE.print(" paired_by="); CONSOLE.println(link.pairedBy());
    if (r != BtLink::OK) return;
    l2.begin(link.handle(), s_aclNum /* from Read_Buffer_Size */); l2.acceptIncoming(true);
    // B6: SDP
    L2cap::Channel *sdp = l2.connect(Sdp::PSM, 0x0040); uint32_t t0 = millis();
    while (sdp->state != L2cap::OPEN && millis() - t0 < 5000) { l2.service(); delay(10); }
    uint8_t q[18]; l2.send(sdp->remoteCid, q, Sdp::buildAudioSinkPdlRequest(q, 1)); s_sdpDone = false; t0 = millis();
    while (!s_sdpDone && millis() - t0 < 5000) { l2.service(); delay(10); }
    CONSOLE.print("sdp_avdtp_version=0x"); printHex16(s_sdpVer); CONSOLE.println(s_sdpVer ? " (B6 DONE)" : " (no response)");
    // B7: AVDTP
    L2cap::Channel *sig = l2.connect(Avdtp::PSM, 0x0041); t0 = millis();
    while (sig->state != L2cap::OPEN && millis() - t0 < 5000) { l2.service(); delay(10); }
    avdtp.begin(l2, 0x0041, 0x0042);
    Avdtp::SbcConfig want = { 44100, Avdtp::JOINT_STEREO, 16, 8, Avdtp::LOUDNESS, 2, 53 };
    avdtp.start(want); t0 = millis(); Avdtp::State last = Avdtp::IDLE;
    while (avdtp.state() != Avdtp::STREAMING && avdtp.state() != Avdtp::FAILED && millis() - t0 < 15000) {
        l2.service(); avdtp.service(); delay(10);
        if (avdtp.state() != last) { last = avdtp.state(); CONSOLE.print("avdtp_state="); CONSOLE.println((int)last); }
    }
    if (avdtp.state() == Avdtp::STREAMING) { CONSOLE.print("avdtp_caps: rates=0x"); printHex8(avdtp.caps().rates);
        CONSOLE.print(" modes=0x"); printHex8(avdtp.caps().modes); CONSOLE.print(" bitpool="); CONSOLE.print(avdtp.caps().minBitpool);
        CONSOLE.print(".."); CONSOLE.println(avdtp.caps().maxBitpool);
        CONSOLE.print("avdtp_start=ok media_mtu="); CONSOLE.println(avdtp.mediaMtu()); CONSOLE.println("B7 DONE"); }
    else { CONSOLE.print("avdtp=fail state="); CONSOLE.print((int)avdtp.state()); CONSOLE.print(" error=0x"); printHex8(avdtp.error()); CONSOLE.println(); }
```
Keep `M2_BT_TARGET_NAME`, `M2_BT_LEGACY_PIN` (→ `link.setPin("1234")` + SSP skipped), `M2_BT_FAST_BAUD`, `M2_BT_LOOPBACK`; drop `M2_BT_SDP_BEFORE_PAIRING`, `M2_BT_AVDTP_DISCOVER`, `M2_BT_PEER_AUTH`, `M2_BT_SC_HOST`, `M2_BT_NO_ROLE_SWITCH`, `M2_BT_AUTH_REQ` (their questions are answered; the transcript keeps the answers).

- [ ] **Step 4: Regression baseline** — gate build (`-UM2RADIO_IW416_FW -UM2RADIO_IW416_BT_FW`, every knob OFF): `./run_qemu.sh`, `./run_qemu_hci.sh`, `./run_qemu_baud.sh` all `PASS`; `./tools/run-all-qemu-gates.sh -l | tail -1` → `(125 gate(s))`.

- [ ] **Step 5: Commit** — `git add examples/networking/m2_hci_probe evkb.cmake && git commit -m "m2_hci_probe: BT-2/B7 on M2Radio/bt (L2cap, BtLink, Sdp, Avdtp); prototype knobs retired; pin bumped"`

---

## Phase 2 — AVDTP signalling against real sinks

### Task 10: the `[avdtp]` gate

**Files:**
- Create: `examples/networking/m2_hci_probe/run_qemu_avdtp.sh`
- Modify: `tools/gate-vacuity.test.sh`

- [ ] **Step 1: Script** — same skeleton as `run_qemu_baud.sh` (own build dir `build-avdtp/` with `-DM2_BT_CONNECT=ON -DM2_BT_TARGET_NAME=FAKE-HEADSET-01`), phase `avdtp`, wait token `'^B7 DONE'` (fallback: `'^avdtp=fail'`), then:
```sh
grep -q '^inq_name: bd=AA:BB:CC:DD:EE:01 status=0x00 name="FAKE-HEADSET-01"' "$OUT" || fail "[avdtp] target not found by name"
grep -q "^secure=ok paired_by=ssp[[:space:]]*$" "$OUT"                  || fail "[avdtp] SSP pairing + encryption did not complete against the peer"
grep -q "^sdp_avdtp_version=0x0103 (B6 DONE)" "$OUT"                    || fail "[avdtp] SDP did not return the peer's AVDTP 1.3"
grep -q "^avdtp_caps: rates=0x0F modes=0x0F bitpool=2..53" "$OUT"       || fail "[avdtp] capabilities not read off the peer"
grep -q "^avdtp_start=ok media_mtu=672" "$OUT"                          || fail "[avdtp] START not accepted / media MTU not negotiated"
grep -q "^PEER-SET-CONFIG cie=21150235" "$RES"                          || fail "[avdtp] our SET_CONFIGURATION bytes are not the calibration config"
grep -q "^PEER-AVDTP-STARTED" "$RES"                                     || fail "[avdtp] the peer never saw an accepted START"
if grep -q "PEER-AVDTP-START-BEFORE-OPEN" "$RES"; then fail "[avdtp] START was sent before OPEN was acknowledged"; fi
if grep -q "PEER-L2CAP-CFGRSP-BAD-SCID" "$RES"; then fail "[avdtp] our Config Response names the wrong CID (receiver-side rule)"; fi
grep "^PEER-AVDTP" "$RES" | grep -q "order=1,2,3,6,7" || fail "[avdtp] signalling order must be DISCOVER, GET_CAPABILITIES, SET_CONFIGURATION, OPEN, START"
echo "PASS: AVDTP initiator negotiates the calibration SBC config and reaches START against the fake acceptor, in order, with the SCID rule and Info Request honoured"
```
- [ ] **Step 2: Run** → `PASS`. **Demonstrate RED**: (a) in `Avdtp::service()` swap `OPENING`'s action to send `buildStart` — expect `[avdtp] START was sent before OPEN`; revert. (b) in `L2cap::service()` put `ch.localCid` in the Config Response SCID — expect `[avdtp] our Config Response names the wrong CID`; revert. Record both in the header.
- [ ] **Step 3: Vacuity fixture** for `run_qemu_avdtp.sh` against the card-absent transcript (must fail `[avdtp] target not found by name`). `./tools/gate-vacuity.test.sh | tail -2` all ok. Sweep: `-l` → `(126 gate(s))`.
- [ ] **Step 4: Commit** — `git add examples/networking/m2_hci_probe/run_qemu_avdtp.sh tools/gate-vacuity.test.sh && git commit -m "m2_hci_probe: [avdtp] gate -- the initiator negotiates the calibration config and reaches START (sweep 126)"`

### Task 11: phase 2 on silicon — the sink reports our configuration

- [ ] **Step 1: ESP32 sink** — bench build (`-DM2_BT_TARGET_NAME=EVKB-SINK -DM2_BT_LEGACY_PIN=ON -DM2_BT_FAST_BAUD=ON` + blobs + `ASSERT_CTS` + `CONNECT`), sink logger armed (`esp_reader.py`, see the 2026-09-02 transcript). *Assertions:* console `avdtp_start=ok media_mtu=<n>` and `B7 DONE`; sink log `a2dp_audio_cfg: codec=SBC rate=44100 chan=joint blocks=16 subbands=8 alloc=loudness bitpool=2..53` **then** `a2dp_audio: state=started` — the instrument's own account of our SET_CONFIGURATION and START.
- [ ] **Step 2: Both headsets over SSP** — `-DM2_BT_TARGET_NAME=Shokz` then `OneOdio`, `M2_BT_LEGACY_PIN=OFF`: `secure=ok paired_by=ssp`, `B7 DONE`, media MTU printed (expect 895 and 335). The headset will start expecting media; with none it may SUSPEND/CLOSE after a few seconds — record what each does, it is plan B's input.
- [ ] **Step 3: Transcript + commit** — append a dated `Phase 2` section with all three consoles and the sink's lines; `git commit -m "m2_hci_probe: phase 2 on silicon -- SET_CONFIGURATION/OPEN/START against the ESP32 sink and both headsets"`.

---

## Phase 3 — clean-room SBC encoder

Everything here comes from A2DP v1.3 §12 (the SBC codec) and nothing else. **Do not open `libsbc` (LGPL) or bluedroid's encoder.** YAGNI: 8 subbands, 16 blocks, joint stereo or stereo, loudness allocation, bitpool 2..53 at 44.1 kHz — the negotiated configuration. One frame per call = 128 PCM samples per channel = one Audio-library block.

### Task 12: `Sbc` encoder

**Files:**
- Create: `M2Radio/bt/Sbc.h`, `M2Radio/bt/Sbc.cpp`
- Create: `M2Radio/bt/test/sbc_test.cpp`

- [ ] **Step 1: The failing tests** — `M2Radio/bt/test/sbc_test.cpp`
```cpp
#include "Sbc.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
static int g_fails = 0, g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
int main() {
    Sbc::Params p = { Sbc::RATE_44100, Sbc::JOINT_STEREO, 16, 8, Sbc::LOUDNESS, 53 };
    {   // 1. Frame length is the spec's formula: 4 + 4*sb*ch/8 + ceil((join*sb + blocks*bitpool)/8) = 4 + 8 + ceil(856/8) = 119
        CHECK(Sbc::frameLength(p) == 119);
        Sbc::Params s = p; s.mode = Sbc::STEREO;   CHECK(Sbc::frameLength(s) == 4 + 8 + 106);   // ceil(848/8)
        Sbc::Params m = p; m.mode = Sbc::MONO;     CHECK(Sbc::frameLength(m) == 4 + 4 + 106);
    }
    {   // 2. Header bytes: sync 0x9C; fs 44.1k (10), 16 blocks (11), joint (11), loudness (0), 8 subbands (1) = 0b10111101 = 0xBD; bitpool; CRC-8 covers header+scale factors
        Sbc enc; enc.begin(p); int16_t L[128] = {0}, R[128] = {0}; uint8_t f[128]; uint16_t n = enc.encode(L, R, f);
        CHECK(n == 119); CHECK(f[0] == 0x9C); CHECK(f[1] == 0xBD); CHECK(f[2] == 53);
        CHECK(f[3] == Sbc::crc8(f + 1, 2, /*scale factor nibbles*/ f + 4, 8 * 2, /*join bits*/ f[4], p));   // recomputed independently below
    }
    {   // 3. CRC-8 poly 0x1D init 0x0F on a known vector (0xBD 0x35 then 16 nibbles of zero + 8 join bits of zero)
        static const uint8_t hb[2] = { 0xBD, 0x35 }; uint8_t z[8] = {0};
        CHECK(Sbc::crc8(hb, 2, z, 16, 0x00, p) == Sbc::crc8Reference(hb, 2, z, 16, 0x00));
    }
    {   // 4. Loudness bit allocation, hand-derived: all scale factors 0 -> bitneed = -5 (offset -2 for sb0 at 44.1k? see table) ...
        //    Simplest normative check: with bitpool 53, 8 subbands, joint stereo, the bits sum over both channels to exactly 53 per block? No --
        //    the spec allocates bitpool bits per CHANNEL (stereo) or per channel-pair (joint/dual).  Assert the invariant the spec states:
        //    sum(bits[ch][sb]) over the allocated set == bitpool (joint/stereo: over both channels together).
        uint8_t sf[2][8] = { {8,7,6,5,4,3,2,1}, {8,7,6,5,4,3,2,1} }; uint8_t bits[2][8];
        Sbc::allocateBits(p, sf, bits); int sum = 0; for (int c = 0; c < 2; c++) for (int s = 0; s < 8; s++) sum += bits[c][s];
        CHECK(sum == 53); for (int c = 0; c < 2; c++) for (int s = 0; s < 8; s++) CHECK(bits[c][s] == 0 || (bits[c][s] >= 2 && bits[c][s] <= 16));
    }
    {   // 5. A 1 kHz sine at -6 dBFS encodes to N frames that all carry the sync word and consistent lengths; write sine.sbc for sbc_snr.py
        Sbc enc; enc.begin(p); FILE *o = fopen("sine.sbc", "wb"); CHECK(o != nullptr);
        double ph = 0; int frames = 0;
        for (int fr = 0; fr < 200; fr++) { int16_t L[128], R[128];
            for (int i = 0; i < 128; i++) { L[i] = R[i] = (int16_t)(16384.0 * sin(ph)); ph += 2 * M_PI * 1000.0 / 44100.0; }
            uint8_t f[128]; uint16_t n = enc.encode(L, R, f); CHECK(n == 119 && f[0] == 0x9C); if (o) fwrite(f, 1, n, o); frames++; }
        if (o) fclose(o); CHECK(frames == 200);
    }
    printf("sbc_test: %d checks, %d failures\n", g_checks, g_fails); return g_fails ? 1 : 0;
}
```
(`crc8Reference` is a second, deliberately naive bit-by-bit CRC in the test's own header — `Sbc.h` declares it `static` for tests to use, implemented in `Sbc.cpp` next to the fast one, so the two are independent implementations of the same spec paragraph.)

- [ ] **Step 2: Run, expect compile failure** — `~/Development/M2Radio/bt/test/run.sh` → `Sbc.h: No such file`.

- [ ] **Step 3: `Sbc.h`**
```cpp
// Sbc -- clean-room SBC ENCODER from A2DP v1.3 section 12.  8 subbands, 16
// blocks (one frame = 128 samples per channel = one Audio-library block),
// joint stereo / stereo / mono, loudness allocation, bitpool 2..53.  Float
// analysis filterbank (the CM7 has an FPU; ~microseconds per frame), integer
// scale factors, allocation, quantisation and CRC exactly as the spec's
// normative procedures state them.  MIT.  Nothing here was derived from any
// existing implementation.
#pragma once
#include <stdint.h>
struct Sbc {
    enum Rate : uint8_t { RATE_16000 = 0, RATE_32000 = 1, RATE_44100 = 2, RATE_48000 = 3 };
    enum Mode : uint8_t { MONO = 0, DUAL = 1, STEREO = 2, JOINT_STEREO = 3 };
    enum Alloc : uint8_t { LOUDNESS = 0, SNR = 1 };
    struct Params { Rate rate; Mode mode; uint8_t blocks, subbands; Alloc alloc; uint8_t bitpool; };
    static uint16_t frameLength(const Params &p);
    static uint8_t  crc8(const uint8_t *hdr, uint8_t hdrLen, const uint8_t *sfNibbles, uint8_t nSf, uint8_t joinByte, const Params &p);
    static uint8_t  crc8Reference(const uint8_t *hdr, uint8_t hdrLen, const uint8_t *sfNibbles, uint8_t nSf, uint8_t joinByte);
    static void     allocateBits(const Params &p, const uint8_t sf[2][8], uint8_t bits[2][8]);
    void begin(const Params &p);
    uint16_t encode(const int16_t *left, const int16_t *right, uint8_t *out);   // right ignored for MONO; returns frame length
private:
    Params m_p; float m_x[2][80];                 // analysis windows (newest first), per channel
    void analyse(uint8_t ch, const int16_t *in, int32_t sub[16][8]);   // 16 blocks x 8 subband samples, Q15-scaled integers
};
```
- [ ] **Step 4: `Sbc.cpp`** — the spec's procedures, in order. The prototype window **must be checked against A2DP v1.3 Appendix B "proto_8_80"** before this task is closed (transcribe from the spec; the values below are the engineer's starting point and Task 13's SNR test is the arbiter).
```cpp
#include "Sbc.h"
#include <math.h>
#include <string.h>
// A2DP v1.3 Appendix B, proto_8_80 (8 subbands): CHECK AGAINST THE SPEC.
static const float PROTO8[80] = {
  0.00000000e+00f, 1.56575398e-04f, 3.43256425e-04f, 5.54620202e-04f, 8.23919506e-04f, 1.13992507e-03f, 1.47640169e-03f, 1.78371725e-03f,
  2.01182542e-03f, 2.10371989e-03f, 1.99454554e-03f, 1.61656283e-03f, 9.02154502e-04f,-1.78805361e-04f,-1.64973098e-03f,-3.49717454e-03f,
  5.65949473e-03f, 8.02941163e-03f, 1.04584443e-02f, 1.27472335e-02f, 1.46525263e-02f, 1.59045603e-02f, 1.62208471e-02f, 1.53184106e-02f,
  1.29371806e-02f, 8.85757540e-03f, 2.92408442e-03f,-4.91578024e-03f,-1.46404076e-02f,-2.61098752e-02f,-3.90751381e-02f,-5.31873032e-02f,
  6.79989431e-02f, 8.29847578e-02f, 9.75753918e-02f, 1.11196689e-01f, 1.23264548e-01f, 1.33264415e-01f, 1.40753505e-01f, 1.45389847e-01f,
  1.46955068e-01f, 1.45389847e-01f, 1.40753505e-01f, 1.33264415e-01f, 1.23264548e-01f, 1.11196689e-01f, 9.75753918e-02f, 8.29847578e-02f,
 -6.79989431e-02f,-5.31873032e-02f,-3.90751381e-02f,-2.61098752e-02f,-1.46404076e-02f,-4.91578024e-03f, 2.92408442e-03f, 8.85757540e-03f,
  1.29371806e-02f, 1.53184106e-02f, 1.62208471e-02f, 1.59045603e-02f, 1.46525263e-02f, 1.27472335e-02f, 1.04584443e-02f, 8.02941163e-03f,
 -5.65949473e-03f,-3.49717454e-03f,-1.64973098e-03f,-1.78805361e-04f, 9.02154502e-04f, 1.61656283e-03f, 1.99454554e-03f, 2.10371989e-03f,
  2.01182542e-03f, 1.78371725e-03f, 1.47640169e-03f, 1.13992507e-03f, 8.23919506e-04f, 5.54620202e-04f, 3.43256425e-04f, 1.56575398e-04f };
// Loudness offsets, 8 subbands, rows = fs 16/32/44.1/48 kHz (spec Table 12.x)
static const int8_t OFFSET8[4][8] = { {-2,0,0,0,0,0,0,1}, {-3,0,0,0,0,0,1,2}, {-4,0,0,0,0,0,1,2}, {-4,0,0,0,0,0,1,2} };
uint16_t Sbc::frameLength(const Params &p) {
    uint16_t ch = (p.mode == MONO) ? 1 : 2, sb = p.subbands, bl = p.blocks;
    uint16_t bits = (p.mode == MONO || p.mode == DUAL) ? (uint16_t)(bl * p.bitpool * ch)
                                                       : (uint16_t)(((p.mode == JOINT_STEREO) ? sb : 0) + bl * p.bitpool);
    return (uint16_t)(4 + (4 * sb * ch) / 8 + (bits + 7) / 8);
}
static uint8_t crcStep(uint8_t crc, uint8_t bit) { uint8_t fb = (uint8_t)(((crc >> 7) ^ bit) & 1); crc = (uint8_t)(crc << 1); if (fb) crc ^= 0x1D; return crc; }
uint8_t Sbc::crc8Reference(const uint8_t *h, uint8_t hl, const uint8_t *sf, uint8_t n, uint8_t join) {
    uint8_t crc = 0x0F;                                         // header bytes 1..2, then join bits (joint only: 8 bits), then scale-factor nibbles
    for (uint8_t i = 0; i < hl; i++) for (int b = 7; b >= 0; b--) crc = crcStep(crc, (uint8_t)((h[i] >> b) & 1));
    if (join != 0xFF) for (int b = 7; b >= 0; b--) crc = crcStep(crc, (uint8_t)((join >> b) & 1));
    for (uint8_t i = 0; i < n; i++) for (int b = 3; b >= 0; b--) crc = crcStep(crc, (uint8_t)((sf[i] >> b) & 1));
    return crc;
}
uint8_t Sbc::crc8(const uint8_t *h, uint8_t hl, const uint8_t *sf, uint8_t n, uint8_t join, const Params &p) {
    return crc8Reference(h, hl, sf, n, p.mode == JOINT_STEREO ? join : 0xFF);   // same procedure; a table-driven form may replace it later
}
void Sbc::allocateBits(const Params &p, const uint8_t sf[2][8], uint8_t bits[2][8]) {   // section 12.7, loudness, 8 subbands
    int ch = (p.mode == MONO) ? 1 : 2; bool pair = (p.mode == STEREO || p.mode == JOINT_STEREO);
    int bitneed[2][8]; int loudness[2][8];
    for (int c = 0; c < ch; c++) for (int s = 0; s < 8; s++) {
        if (sf[c][s] == 0) bitneed[c][s] = -5;
        else { loudness[c][s] = (int)sf[c][s] - OFFSET8[p.rate][s]; bitneed[c][s] = loudness[c][s] > 0 ? loudness[c][s] / 2 : loudness[c][s]; }
    }
    int max_bitneed = -100; for (int c = 0; c < ch; c++) for (int s = 0; s < 8; s++) if (bitneed[c][s] > max_bitneed) max_bitneed = bitneed[c][s];
    int bitcount = 0, slicecount = 0, bitslice = max_bitneed + 1;
    int total = pair ? p.bitpool : p.bitpool;                    // per channel (mono/dual) or per pair (stereo/joint); the loop below is per group
    auto alloc_group = [&](int c0, int c1, int pool) {
        bitcount = 0; slicecount = 0; bitslice = max_bitneed + 1;
        do { bitslice--; bitcount += slicecount; slicecount = 0;
            for (int c = c0; c < c1; c++) for (int s = 0; s < 8; s++) {
                if (bitneed[c][s] > bitslice + 1 && bitneed[c][s] < bitslice + 16) slicecount++;
                else if (bitneed[c][s] == bitslice + 1) slicecount += 2; }
        } while (bitcount + slicecount < pool);
        if (bitcount + slicecount == pool) { bitcount += slicecount; bitslice--; }
        for (int c = c0; c < c1; c++) for (int s = 0; s < 8; s++) {
            if (bitneed[c][s] < bitslice + 2) bits[c][s] = 0;
            else { int b = bitneed[c][s] - bitslice; bits[c][s] = (uint8_t)(b < 16 ? b : 16); } }
        for (int c = c0; c < c1 && bitcount < pool; c++) for (int s = 0; s < 8 && bitcount < pool; s++) {
            if (bits[c][s] >= 2 && bits[c][s] < 16) { bits[c][s]++; bitcount++; }
            else if (bitneed[c][s] == bitslice + 1 && pool > bitcount + 1) { bits[c][s] = 2; bitcount += 2; } }
        for (int c = c0; c < c1 && bitcount < pool; c++) for (int s = 0; s < 8 && bitcount < pool; s++)
            if (bits[c][s] < 16) { bits[c][s]++; bitcount++; }
    };
    if (pair) alloc_group(0, 2, total); else for (int c = 0; c < ch; c++) alloc_group(c, c + 1, total);
}
void Sbc::begin(const Params &p) { m_p = p; memset(m_x, 0, sizeof m_x); }
void Sbc::analyse(uint8_t ch, const int16_t *in, int32_t sub[16][8]) {          // section 12.6.3, 8-subband analysis, 16 blocks
    static float M[8][16]; static bool init = false;
    if (!init) { for (int k = 0; k < 8; k++) for (int i = 0; i < 16; i++) M[k][i] = cosf((i + 4) * (2 * k + 1) * (float)M_PI / 16.0f); init = true; }
    for (int blk = 0; blk < 16; blk++) {
        float *X = m_x[ch];
        for (int i = 79; i >= 8; i--) X[i] = X[i - 8];
        for (int i = 0; i < 8; i++) X[i] = (float)in[blk * 8 + 7 - i] / 32768.0f;     // newest sample at X[0]
        float Y[16];
        for (int i = 0; i < 16; i++) { float y = 0; for (int k = 0; k < 5; k++) y += PROTO8[i + 16 * k] * X[i + 16 * k]; Y[i] = y; }
        for (int k = 0; k < 8; k++) { float s = 0; for (int i = 0; i < 16; i++) s += M[k][i] * Y[i];
            int32_t v = (int32_t)lrintf(s * 32768.0f); if (v > 32767) v = 32767; if (v < -32768) v = -32768; sub[blk][k] = v; }
    }
}
uint16_t Sbc::encode(const int16_t *L, const int16_t *R, uint8_t *out) {
    const Params &p = m_p; int ch = (p.mode == MONO) ? 1 : 2;
    int32_t sub[2][16][8]; analyse(0, L, sub[0]); if (ch == 2) analyse(1, R, sub[1]);
    uint8_t join = 0;
    if (p.mode == JOINT_STEREO) {                                    // per subband (never the last): M/S if it needs fewer scale-factor bits
        for (int s = 0; s < 7; s++) { int32_t maxL = 0, maxR = 0, maxM = 0, maxS = 0;
            for (int b = 0; b < 16; b++) { int32_t l = sub[0][b][s], r = sub[1][b][s], m = (l + r) / 2, d = (l - r) / 2;
                if (labs(l) > maxL) maxL = labs(l); if (labs(r) > maxR) maxR = labs(r); if (labs(m) > maxM) maxM = labs(m); if (labs(d) > maxS) maxS = labs(d); }
            auto sfOf = [](int32_t v) { int sf = 0; while (v >= (1 << (sf + 1)) && sf < 15) sf++; return v ? sf + 1 : 0; };
            if (sfOf(maxM) + sfOf(maxS) < sfOf(maxL) + sfOf(maxR)) { join |= (uint8_t)(0x80 >> s);
                for (int b = 0; b < 16; b++) { int32_t l = sub[0][b][s], r = sub[1][b][s]; sub[0][b][s] = (l + r) / 2; sub[1][b][s] = (l - r) / 2; } } }
    }
    uint8_t sf[2][8] = {{0}}, bits[2][8] = {{0}};
    for (int c = 0; c < ch; c++) for (int s = 0; s < 8; s++) { int32_t mx = 0;
        for (int b = 0; b < 16; b++) { int32_t a = labs(sub[c][b][s]); if (a > mx) mx = a; }
        int e = 0; while (mx >= (1 << (e + 1)) && e < 15) e++; sf[c][s] = (uint8_t)(mx ? e + 1 : 0); }   // scale_factor = floor(log2|max|)+1
    allocateBits(p, sf, bits);
    // --- bitstream ---
    uint16_t n = 0; out[n++] = 0x9C;
    out[n++] = (uint8_t)((p.rate << 6) | (((p.blocks / 4) - 1) << 4) | (p.mode << 2) | (p.alloc << 1) | (p.subbands == 8 ? 1 : 0));
    out[n++] = p.bitpool; uint8_t crcPos = (uint8_t)n; out[n++] = 0;
    uint32_t acc = 0; int nb = 0;
    auto put = [&](uint32_t v, int len) { for (int i = len - 1; i >= 0; i--) { acc = (acc << 1) | ((v >> i) & 1); if (++nb == 8) { out[n++] = (uint8_t)acc; acc = 0; nb = 0; } } };
    if (p.mode == JOINT_STEREO) put(join, 8);
    uint8_t sfN[16]; int k = 0;
    for (int c = 0; c < ch; c++) for (int s = 0; s < 8; s++) { put(sf[c][s], 4); sfN[k++] = sf[c][s]; }
    for (int b = 0; b < 16; b++) for (int c = 0; c < ch; c++) for (int s = 0; s < 8; s++) if (bits[c][s]) {
        int32_t levels = (1 << bits[c][s]) - 1;                   // quantise: q = floor(((x / 2^(sf+1)) + 1) * levels / 2)
        int64_t x = sub[c][b][s]; int64_t q = (((x << 1) + (1LL << (sf[c][s] + 1))) * levels) >> (sf[c][s] + 2);   // = ((x/2^(sf+1)) + 1) * levels / 2, exact
        if (q < 0) q = 0; if (q > levels) q = levels; put((uint32_t)q, bits[c][s]); }
    if (nb) put(0, 8 - nb);                                       // pad the last byte
    out[crcPos] = crc8(out + 1, 2, sfN, (uint8_t)(ch * 8), join, p);
    return n;
}
```
- [ ] **Step 5: Run** — `~/Development/M2Radio/bt/test/run.sh` → `sbc_test: N checks, 0 failures`, `sine.sbc` written (200 × 119 = 23 800 bytes). If check 2's CRC comparison fails, the CRC bit order (header bytes 1–2 MSB-first, then the join byte for joint stereo, then scale-factor nibbles MSB-first) is the first thing to re-read in §12.5.

- [ ] **Step 6: Commit (M2Radio)** — `git add bt/Sbc.h bt/Sbc.cpp bt/test/sbc_test.cpp && git commit -m "bt: clean-room SBC encoder (8 subbands, 16 blocks, joint/stereo, loudness) with host tests"`

### Task 13: the ffmpeg decode + SNR check (a test tool)

**Files:**
- Create: `M2Radio/bt/test/sbc_snr.py`

Nothing links ffmpeg; it is the Mac-side oracle that says the frames are *decodable* (its decoder rejects bad CRCs and malformed frames) and that the audio inside is the sine we put in.

- [ ] **Step 1: The tool**
```python
#!/usr/bin/env python3
# Decode sine.sbc with ffmpeg (a TEST TOOL -- nothing in the tree links it) and
# report the SNR of the decoded 1 kHz tone.  Usage: sbc_snr.py sine.sbc [min_snr_db]
import subprocess, sys, struct, math
src, min_db = sys.argv[1], float(sys.argv[2]) if len(sys.argv) > 2 else 30.0
raw = subprocess.run(["ffmpeg", "-v", "error", "-f", "sbc", "-i", src, "-f", "s16le", "-ac", "2", "-ar", "44100", "-"],
                     capture_output=True, check=True).stdout
n = len(raw) // 4; L = [struct.unpack_from("<h", raw, 4 * i)[0] for i in range(n)]
# Least-squares fit of a 1 kHz sine (unknown amplitude/phase) after the 80-sample filterbank delay, skip the first frame.
xs = range(256, n); w = 2 * math.pi * 1000 / 44100
a = sum(L[i] * math.sin(w * i) for i in xs) * 2 / len(xs); b = sum(L[i] * math.cos(w * i) for i in xs) * 2 / len(xs)
sig = sum((a * math.sin(w * i) + b * math.cos(w * i)) ** 2 for i in xs); err = sum((L[i] - a * math.sin(w * i) - b * math.cos(w * i)) ** 2 for i in xs)
snr = 10 * math.log10(sig / err) if err else 99.0
print("sbc_snr: frames=%d samples=%d amp=%.0f snr_db=%.1f" % (len(open(src, "rb").read()) // 119, n, math.hypot(a, b), snr))
sys.exit(0 if snr >= min_db else 1)
```
- [ ] **Step 2: Run it** — from `M2Radio/bt/test` after `run.sh` produced `sine.sbc`: `python3 sbc_snr.py sine.sbc 30` → `sbc_snr: frames=200 samples=25600 amp≈16384 snr_db=…` and exit 0. Expect ≥ 40 dB at bitpool 53. **Failure modes and where to look:** ffmpeg refuses the stream → header byte 1 or CRC (§12.4/12.5); decodes but SNR < 20 dB → the prototype table or the M matrix (§12.6.3, Appendix B); SNR 20–30 dB → quantiser or allocation (§12.6.5, §12.7). Also listen: `ffmpeg -f sbc -i sine.sbc sine.wav && afplay sine.wav`.

- [ ] **Step 3: Wire it into `run.sh` as optional** — after the loop: `if command -v ffmpeg >/dev/null && [ -f sine.sbc ]; then python3 "$DIR/sbc_snr.py" sine.sbc 30 || exit 1; else echo "sbc_snr: skipped (no ffmpeg)"; fi`. Commit: `git add bt/test/sbc_snr.py bt/test/run.sh && git commit -m "bt/test: ffmpeg-decoded SNR check for the SBC encoder (test tool)"`.

---

## Close-out

### Task 14: pins, audit, transcript, tracking

- [ ] **Step 1:** Push M2Radio; bump the `evkb.cmake` pin; verify the fresh-user path: `cmake -B build-fetch -DEVKB_FORCE_FETCH=ON ...` on `m2_hci_probe`, then run `run_qemu.sh`, `run_qemu_hci.sh`, `run_qemu_baud.sh`, `run_qemu_avdtp.sh` against the fetched ELF (symlink `build` → `build-fetch`, restore after). Commit the pin.
- [ ] **Step 2:** `LICENSE_AUDIT_EVKB=$(pwd) ./tools/license-audit.sh` → `PASS`; `./tools/run-all-qemu-gates.sh` → **126 passed, 0 failed, 0 SKIP** (or the documented single red); `./tools/gate-vacuity.test.sh` all ok; `~/Development/M2Radio/hci/test/run.sh` and `bt/test/run.sh` PASS.
- [ ] **Step 3:** Update `CLAUDE.md`'s gate-count paragraph (124 → 126, naming `[baud]` and `[avdtp]` and what each can and cannot prove) and `docs/KNOWN-BROKEN-GATES.md` if anything is conditional.
- [ ] **Step 4:** Transcript: one dated section per phase is already appended by Tasks 4 and 11; add the phase-3 SNR number.
- [ ] **Step 5:** Linear NEW-9: comment with phases 0–3 results and the numbers (rate that answered, loopback loss, media MTUs per peer, SNR); the plan-B brainstorm/plan is next.

---

## Self-review (done while writing)

* **Spec coverage:** phase 0 → Tasks 1–4; phase 1 → 5–9 (extraction, credits, fake acceptor, host tests for the SCID rule/Info Response/credits); phase 2 → 10–11 (the `[avdtp]` gate's ordering + config-bytes assertions, sink's own `a2dp_audio_cfg`, both headsets); phase 3 → 12–13 (frame length, header, CRC, allocation invariant, sine vector, ffmpeg SNR). Error handling: `BtLink::Result` names, `Avdtp::FAILED` + error code, `L2cap::dropped()`. Non-goals untouched. Phases 4–5 are plan B by design.
* **Placeholders:** none; the one deliberately-flagged item is the prototype table, which the plan says to verify against the spec and which Task 13 arbitrates.
* **Type consistency:** `L2cap::Channel{state, psm, localCid, remoteCid, mtuOut, mtuIn, ...}`, `L2cap::connect/send/service/onAcl/onEvent/byLocal/byRemote/credits/acceptIncoming`, `Avdtp::{SbcConfig, SbcCaps, Sep, begin, start, onSignalling, service, state, error, caps, mediaMtu}`, `Sdp::{buildAudioSinkPdlRequest, parseAvdtpVersion, PSM}`, `Sbc::{Params, frameLength, crc8, crc8Reference, allocateBits, begin, encode}`, `BtLink::{connect, pairAndEncrypt, onEvent, handle, pairedBy, Result, resultName}` are used with the same names and signatures in Tasks 5–13.
