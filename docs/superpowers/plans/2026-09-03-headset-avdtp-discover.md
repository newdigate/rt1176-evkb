# Headset AVDTP DISCOVER — Diagnose & Fix — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reach AVDTP STREAMING to the Shokz headset on silicon and hear a clean tone, by first getting decodable ACL visibility (diagnosis), then fixing the cause the capture names.

**Architecture:** Diagnosis-first. Phase 1 adds an opt-in `L2cap` ACL-trace hook + a btsnoop converter and captures the EVKB↔Shokz exchange, compared against a Mac→Shokz Apple PacketLogger reference. A DIAGNOSIS CHECKPOINT (Task 4) names the cause; the fix (Task 5) is written to it (leading hypothesis coded in full). Phase 3 verifies on silicon with a demonstrated-red regression guard.

**Tech Stack:** C++11 (M2Radio `bt/L2cap`, `bt/Avdtp`; host `c++` unit tests), Python 3 (btsnoop converter), the RT1176 EVKB + IW416 + Shokz, Apple PacketLogger, the custom QEMU gate harness.

**Execution note:** Tasks 1–3, 5, 6, 8 are subagent-executable (code + host tests + build). **Tasks 4 and 7 are bench work** (interactive, real hardware + PacketLogger). **Task 5 is CONTINGENT** on Task 4's checkpoint — implement the branch the capture identifies; the leading hypothesis (b) is coded in full, the others are specified.

**Repos:** `~/Development/M2Radio` (the `bt/` library; branch `master`), `~/Development/rt1170/evkb` (example + tools + pins; commits to `master`). Library resolution is local-first, so Phase-1 bench builds use the working-tree M2Radio without a pin bump; the pin bump + push happen at close-out (Task 8).

---

### Task 1: `L2cap` ACL-trace hook + host test

**Files:**
- Modify: `~/Development/M2Radio/bt/L2cap.h`
- Modify: `~/Development/M2Radio/bt/L2cap.cpp`
- Test: `~/Development/M2Radio/bt/test/l2cap_test.cpp`

Add an optional trace callback that fires for every inbound ACL (before demux, so it catches CIDs we do not route) and every outbound ACL. Both directions hand the callback the **L2CAP PDU** (starts with the 2-byte L2CAP length, then CID, then payload) plus the ACL handle — the same shape `onAcl` already receives, so the converter (Task 2) reconstructs the HCI/H4 header uniformly.

- [ ] **Step 1: Write the failing test**

In `~/Development/M2Radio/bt/test/l2cap_test.cpp`, add a new block inside `main()` before the final `printf`:

```cpp
    {   // N. onAclTrace fires for inbound (a fed ACL) and outbound (a queued send), with the L2CAP PDU + handle.
        struct Cap { struct Rec { bool out; uint16_t handle; std::vector<uint8_t> pdu; }; std::vector<Rec> recs; };
        static Cap cap;   // static so the C-style callback can reach it
        cap.recs.clear();
        CapIo io; L2cap l(io); l.begin(0x0001, 7);
        l.onAclTrace([](void *, bool out, uint16_t h, const uint8_t *p, uint16_t n) {
            cap.recs.push_back({ out, h, std::vector<uint8_t>(p, p + n) }); }, nullptr);
        // Inbound: feed an L2CAP Info Request on the signalling CID 0x0001.
        std::vector<uint8_t> in = l2(0x0001, { 0x0A, 0x01, 0x02, 0x00, 0x02, 0x00 });  // INFO_REQ
        l.onAcl(0x0001, in.data(), (uint16_t)in.size());
        // Outbound: connect() queues a Connection Request; service() writes it.
        l.connect(0x0019, 0x0041); l.service();
        bool sawIn = false, sawOut = false;
        for (auto &r : cap.recs) {
            if (!r.out) { sawIn = true; CHECK(r.handle == 0x0001);
                          CHECK(r.pdu.size() >= 4 && r.pdu[2] == 0x01 && r.pdu[3] == 0x00); }   // CID 0x0001
            else        { sawOut = true; CHECK(r.handle == 0x0001);
                          CHECK(r.pdu.size() >= 4 && r.pdu[0] == (uint8_t)(r.pdu.size() - 4)); } // L2CAP len field
        }
        CHECK(sawIn); CHECK(sawOut);
    }
```

- [ ] **Step 2: Run the suite to verify it fails to compile**

Run: `cd ~/Development/M2Radio/bt/test && CXX=c++ ./run.sh`
Expected: FAIL — `onAclTrace` is not a member of `L2cap`.

- [ ] **Step 3: Declare the hook in `L2cap.h`**

In `~/Development/M2Radio/bt/L2cap.h`, in the `public:` section (after `onData(...)`, ~line 23), add:

```cpp
    // Optional raw-ACL trace: fires for every inbound ACL (before demux) and every
    // outbound ACL, handing over the L2CAP PDU (2-byte length, CID, payload) plus
    // the handle.  Diagnostic only -- off unless set.  Arduino-free (callback).
    typedef void (*TraceFn)(void *ctx, bool out, uint16_t handle, const uint8_t *l2capPdu, uint16_t len);
    void onAclTrace(TraceFn fn, void *ctx) { m_trace = fn; m_traceCtx = ctx; }
```

And in the `private:` members (near `DataFn m_onData; void *m_dataCtx;`, ~line 53), add:

```cpp
    TraceFn m_trace = nullptr; void *m_traceCtx = nullptr;
```

- [ ] **Step 4: Fire the hook in `L2cap.cpp` (both directions)**

In `~/Development/M2Radio/bt/L2cap.cpp`, in `onAcl()`, add the inbound trace right after the guard:

```cpp
void L2cap::onAcl(uint16_t handle, const uint8_t *d, uint16_t len) {
    if (handle != m_handle || len < 4) return;
    if (m_trace) m_trace(m_traceCtx, false, handle, d, len);
    uint16_t cid = (uint16_t)(d[2] | (d[3] << 8));
    ...
```

In `service()`'s outbound write loop, add the outbound trace right after `m_io.write(...)`:

```cpp
        uint8_t pkt[9 + MAX_PAYLOAD]; memcpy(pkt, h, 9); memcpy(pkt + 9, t.buf, t.len); m_io.write(pkt, (size_t)(9 + t.len));
        if (m_trace) m_trace(m_traceCtx, true, m_handle, pkt + 5, (uint16_t)(t.len + 4));   // L2CAP PDU = len(2)+cid(2)+payload
        m_txHead = (uint8_t)((m_txHead + 1) % TXQ); m_txCount--; m_credits--;
```

- [ ] **Step 5: Run the suite to verify it passes**

Run: `cd ~/Development/M2Radio/bt/test && CXX=c++ ./run.sh`
Expected: `l2cap_test` reports its new higher check count with `0 failures`; run ends `BT-HOST-TESTS: PASS`.

- [ ] **Step 6: Commit (stage only these 3 files; never `git add -A`)**

```bash
cd ~/Development/M2Radio
git add bt/L2cap.h bt/L2cap.cpp bt/test/l2cap_test.cpp
git commit -m "feat(bt): L2cap::onAclTrace() -- optional raw-ACL trace for diagnosis

Fires for every inbound ACL (before demux) and every outbound ACL with the
L2CAP PDU + handle. Off unless set; Arduino-free. For the headset AVDTP
DISCOVER investigation.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 2: `acl-trace-to-btsnoop.py` converter + host test

**Files:**
- Create: `~/Development/rt1170/evkb/tools/acl-trace-to-btsnoop.py`
- Create: `~/Development/rt1170/evkb/tools/acl-trace-to-btsnoop.test.sh`

Parse the firmware's `acl_trace` lines and emit a **btsnoop** file that PacketLogger/Wireshark decode as HCI (L2CAP + AVDTP). Each trace line reads `acl_trace dir=<in|out> h=<handle> t=<micros> hex=<l2cap pdu bytes>`; the converter reconstructs a full H4 ACL packet (`0x02` + 2-byte handle/flags + 2-byte ACL length + the L2CAP PDU).

- [ ] **Step 1: Write the converter**

Create `~/Development/rt1170/evkb/tools/acl-trace-to-btsnoop.py`:

```python
#!/usr/bin/env python3
# Convert bt_tone_test's M2_BT_ACL_TRACE lines to a btsnoop HCI log
# (datalink 1001 = H4/UART) that Apple PacketLogger / Wireshark decode.
#   acl_trace dir=<in|out> h=<handle> t=<micros> hex=<l2cap pdu bytes>
# Usage: acl-trace-to-btsnoop.py in.log out.btsnoop
import sys, re, struct

LINE = re.compile(r"acl_trace dir=(in|out) h=(\w+) t=(\d+) hex=([0-9A-Fa-f ]*)")

def records(text):
    for m in LINE.finditer(text):
        direction, h, t, hexs = m.group(1), int(m.group(2), 0), int(m.group(3)), m.group(4)
        pdu = bytes(int(b, 16) for b in hexs.split())
        # H4 ACL packet: 0x02 | handle(12)+PB(2=first non-flush)+BC(2) | ACL len | L2CAP PDU
        hf = (h & 0x0FFF) | (0x02 << 12)
        acl = bytes([0x02, hf & 0xFF, (hf >> 8) & 0xFF, len(pdu) & 0xFF, (len(pdu) >> 8) & 0xFF]) + pdu
        yield (direction == "in"), t, acl

def write_btsnoop(recs, out):
    out.write(b"btsnoop\x00")                       # identification
    out.write(struct.pack(">II", 1, 1001))          # version 1, datalink 1001 (H4)
    for is_in, t_us, acl in recs:
        # timestamp: btsnoop microseconds since 2000-01-01 00:00 (0x00E03AB44A676000 offset from epoch us)
        ts = 0x00E03AB44A676000 + t_us
        flags = (1 if is_in else 0)                 # bit0: 1=received (controller->host), 0=sent
        out.write(struct.pack(">IIIIq", len(acl), len(acl), flags, 0, ts))
        out.write(acl)

def main():
    text = open(sys.argv[1]).read()
    with open(sys.argv[2], "wb") as f:
        write_btsnoop(records(text), f)
    print("btsnoop written:", sys.argv[2])

if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Write the failing test**

Create `~/Development/rt1170/evkb/tools/acl-trace-to-btsnoop.test.sh`:

```sh
#!/bin/sh
set -e
DIR=$(cd "$(dirname "$0")" && pwd); OUT=$(mktemp -d); trap 'rm -rf "$OUT"' EXIT
cat > "$OUT/in.log" <<'EOF'
noise line, ignored
acl_trace dir=out h=0x0001 t=1000 hex=02 00 01 00 0A 01 02 00 02 00
acl_trace dir=in h=0x0001 t=2000 hex=02 00 01 00 0B 01 06 00 02 00 00 00
EOF
python3 "$DIR/acl-trace-to-btsnoop.py" "$OUT/in.log" "$OUT/out.btsnoop"
python3 - "$OUT/out.btsnoop" <<'PY'
import sys, struct
d = open(sys.argv[1], "rb").read()
assert d[:8] == b"btsnoop\x00", "bad magic"
ver, dl = struct.unpack(">II", d[8:16]); assert ver == 1 and dl == 1001, (ver, dl)
off = 16; n = 0
while off < len(d):
    ol, il, fl, drops, ts = struct.unpack(">IIIIq", d[off:off+24]); off += 24
    acl = d[off:off+il]; off += il; n += 1
    assert acl[0] == 0x02, "H4 type must be ACL (0x02)"
    hf = acl[1] | (acl[2] << 8); assert (hf & 0x0FFF) == 0x0001, "handle"
    aclLen = acl[3] | (acl[4] << 8); assert aclLen == len(acl) - 5, "ACL length field"
    if n == 1: assert fl == 0, "first record is sent (dir=out -> flag 0)"
    if n == 2: assert fl == 1, "second record is received (dir=in -> flag 1)"
assert n == 2, ("record count", n)
print("BTSNOOP-CONVERTER-TEST: PASS")
PY
```

Make both executable and run:

Run: `chmod +x ~/Development/rt1170/evkb/tools/acl-trace-to-btsnoop.py ~/Development/rt1170/evkb/tools/acl-trace-to-btsnoop.test.sh && ~/Development/rt1170/evkb/tools/acl-trace-to-btsnoop.test.sh`
Expected: `BTSNOOP-CONVERTER-TEST: PASS`.

- [ ] **Step 3: Demonstrate the test can fail (mutation check)**

Temporarily change the converter's datalink from `1001` to `9999`; re-run — the test FAILS on `ver == 1 and dl == 1001`. Revert and re-run — PASS.

- [ ] **Step 4: Commit**

```bash
cd ~/Development/rt1170/evkb
git add tools/acl-trace-to-btsnoop.py tools/acl-trace-to-btsnoop.test.sh
git commit -m "tools: acl-trace-to-btsnoop converter (+ test) for the AVDTP DISCOVER diag

Turns bt_tone_test's M2_BT_ACL_TRACE lines into a btsnoop HCI log that
PacketLogger/Wireshark decode (L2CAP + AVDTP), for comparing the EVKB exchange
to a Mac->Shokz reference capture.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 3: `bt_tone_test` `M2_BT_ACL_TRACE` wiring + gate check

**Files:**
- Modify: `~/Development/rt1170/evkb/examples/audio/bt_tone_test/CMakeLists.txt`
- Modify: `~/Development/rt1170/evkb/examples/audio/bt_tone_test/bt_tone_test.cpp`

Wire the `L2cap` trace to a VCOM hex dumper under an opt-in flag; OFF by default so gates and the default build are byte-identical.

- [ ] **Step 1: Add the CMake option**

In `CMakeLists.txt`, after the `M2_BT_CONNECT_RETRY` block, add:

```cmake
# Bench diagnostic: dump every in/out ACL packet on the link as hex over the VCOM
# (feed tools/acl-trace-to-btsnoop.py). OFF by default; gates/default build unchanged.
option(M2_BT_ACL_TRACE "Trace every ACL packet (L2cap::onAclTrace) over the console" OFF)
if(M2_BT_ACL_TRACE)
    add_definitions(-DM2_BT_ACL_TRACE=1)
endif()
```

- [ ] **Step 2: Add the dumper and wire it in `setup()`**

In `bt_tone_test.cpp`, add a file-scope dumper near `onEvt` (after `printHex8`/`printHex16` exist):

```cpp
#if defined(M2_BT_ACL_TRACE)
static void aclTrace(void *, bool out, uint16_t handle, const uint8_t *pdu, uint16_t len) {
    CONSOLE.print("acl_trace dir="); CONSOLE.print(out ? "out" : "in");
    CONSOLE.print(" h=0x"); printHex16(handle);
    CONSOLE.print(" t="); CONSOLE.print(micros());
    CONSOLE.print(" hex=");
    for (uint16_t i = 0; i < len; i++) { printHex8(pdu[i]); if (i + 1 < len) CONSOLE.print(' '); }
    CONSOLE.println();
}
#endif
```

In `setup()`, right after `src.setLog(btLog, nullptr); src.setPin("1234");`, add:

```cpp
#if defined(M2_BT_ACL_TRACE)
    src.l2().onAclTrace(aclTrace, nullptr);
#endif
```

- [ ] **Step 3: Verify the default build is unchanged and gates pass**

```bash
cd ~/Development/rt1170/evkb/examples/audio/bt_tone_test
rm -rf build && cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake >/dev/null 2>&1 && cmake --build build >/dev/null 2>&1 && echo BUILD_OK
./run_qemu.sh; echo "exit=$?"
./run_qemu_media.sh; echo "exit=$?"
```
Expected: `BUILD_OK`, both gates PASS (`exit=0`). (The trace is compiled out when the flag is off, so behaviour is byte-identical.)

- [ ] **Step 4: Commit**

```bash
cd ~/Development/rt1170/evkb
git add examples/audio/bt_tone_test/CMakeLists.txt examples/audio/bt_tone_test/bt_tone_test.cpp
git commit -m "feat(bt_tone_test): M2_BT_ACL_TRACE -- dump ACL packets for AVDTP diag

Opt-in (OFF by default): wires L2cap::onAclTrace to a VCOM hex dumper whose
lines feed tools/acl-trace-to-btsnoop.py. Gates and the default build unchanged.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 4: BENCH — capture EVKB↔Shokz + Mac→Shokz reference → DIAGNOSIS CHECKPOINT

**Files:**
- Create (scratch, not committed here): capture logs + btsnoop files.

This is interactive bench work. Bench hygiene: real BT firmware, VCOM detached during LinkServer ops, headset in explicit PAIRING mode per attempt, LinkServer `run` as the reset (physical reset halts on a vector catch when attached / drops the VCOM when not). It produces the finding that decides Task 5.

- [ ] **Step 1: Build the trace bench image (real firmware, SSP, Shokz, retry)**

```bash
cd ~/Development/rt1170/evkb/examples/audio/bt_tone_test
rm -rf build-trace
cmake -B build-trace -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake \
  -DM2_BT_UART_DNLD=ON -DM2_BT_WAKE_PULSE=ON -DM2_BT_RTS_FLOW=ON \
  -DM2_BT_FAST_BAUD=ON -DM2_BT_FAST_BAUD_RATE=3000000 \
  -DM2_BT_LEGACY_PIN=OFF -DM2_BT_TARGET_NAME=Shokz -DM2_BT_CONNECT_RETRY=ON \
  -DM2_BT_ACL_TRACE=ON \
  -DM2RADIO_IW416_BT_FW=/Users/nicholasnewdigate/Development/mcuxsdk-ws/mcuxsdk/components/conn_fwloader/fw_bin/inc/IW416/uartIW416_bt.bin.inc
cmake --build build-trace
```
Expected: `Built target bt_tone_test.elf` (real firmware, `IW416 BT firmware: …/uartIW416_bt.bin.inc`).

- [ ] **Step 2: Flash + capture the EVKB→Shokz exchange (Shokz in PAIRING mode)**

Detach any VCOM reader; `pkill LinkServer redlinkserv crt_emu_cm_redlink`; then background `LinkServer run MIMXRT1176:MIMXRT1170-EVKB build-trace/bt_tone_test.elf`; once programmed, capture the VCOM (`tools/rt1170-console.py <port> 115200`) while the retry loop connects, pairs (SSP), and hits AVDTP. Save all `acl_trace` lines to `evkb-shokz.log`. Confirm the trailing `a2dp_try=avdtp_failed` with `avdtp_state=1`.

- [ ] **Step 3: Convert the EVKB capture to btsnoop and open it**

```bash
~/Development/rt1170/evkb/tools/acl-trace-to-btsnoop.py evkb-shokz.log evkb-shokz.btsnoop
```
Open `evkb-shokz.btsnoop` in Apple PacketLogger (File ▸ Open) or Wireshark. Read the L2CAP config exchange on the AVDTP signalling channel (PSM 0x0019) and whether our `DISCOVER` (AVDTP, signal id 0x01) is transmitted and whether anything comes back.

- [ ] **Step 4: Capture the Mac→Shokz reference in PacketLogger**

Pair the Shokz to the Mac; open PacketLogger (Apple "Additional Tools for Xcode"); start a live capture; play audio to the Shokz so the Mac runs a full A2DP SOURCE session; stop and save `mac-shokz.pklg`. This is the golden template for THIS headset (its CONFIG_REQ MTU/options and the AVDTP DISCOVER/GET_CAP/SET_CONFIG it accepts).

- [ ] **Step 5: DIAGNOSIS CHECKPOINT — diff and name the cause**

Compare `evkb-shokz` vs `mac-shokz`. Determine which of these is true and RECORD it (this selects the Task-5 branch):
- **(a)** our `DISCOVER` never appears in `evkb-shokz.btsnoop` → send/txq bug.
- **(b)** our `DISCOVER` is sent, the Shokz is silent, and our L2CAP CONFIG differs from the Mac's (we send an empty CONFIG_REQ / echo-only CONFIG_RSP; the Mac sends CONFIG_REQ with an MTU option `01 02 <mtu>`) → config-exchange fix. **Record the exact MTU + options the Mac uses.**
- **(c)** the Shokz answers on a CID/PSM `evkb-shokz` shows we never routed → routing fix.
- **(d)** the Shokz drives a reverse DISCOVER/SDP (visible in the Mac capture, absent/mishandled in ours) → reverse-flow fix.

Report the finding, the specific bytes, and the chosen branch before starting Task 5.

---

### Task 5: FIX (CONTINGENT on Task 4) — the cause the capture named

**Files:**
- Modify: `~/Development/M2Radio/bt/L2cap.cpp` (branch b/c) and/or `~/Development/M2Radio/bt/Avdtp.cpp` (branch a/d)
- Test: `~/Development/M2Radio/bt/test/l2cap_test.cpp` or `avdtp_test.cpp`

Implement ONLY the branch Task 4 identified. The leading hypothesis **(b)** is coded in full below; (a)/(c)/(d) are specified so the executor writes them to the captured bytes. The fix MUST NOT change the ESP32 path (Task 7 verifies).

**Branch (b) — proper CONFIG_REQ with MTU (leading hypothesis):**

- [ ] **b-Step 1: Write the failing host test**

In `~/Development/M2Radio/bt/test/l2cap_test.cpp`, add a block asserting our CONFIG_REQ now carries the MTU option. `connect()` queues CONN_REQ; feed a CONN_RSP; `service()` then queues our CONFIG_REQ. Use the MTU value the Mac capture showed — the plan uses `<MAC_MTU>`; replace it with that number (a 2-byte LE little-endian value):

```cpp
    {   // CONFIG_REQ carries an MTU option (headset requires it; the ESP32 tolerated its absence).
        CapIo io; L2cap l(io); l.begin(0x0001, 7);
        L2cap::Channel *ch = l.connect(0x0019, 0x0041);           // our SCID 0x0041
        // peer CONN_RSP: our SCID 0x0041, its DCID 0x0080, result success
        std::vector<uint8_t> rsp = l2(0x0001, { 0x03, 0x01, 0x08, 0x00, 0x80, 0x00, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00 });
        l.onAcl(0x0001, rsp.data(), (uint16_t)rsp.size());
        io.tx.clear(); l.service();                                // should now emit CONFIG_REQ with MTU
        bool sawMtu = false;
        for (auto &p : io.tx) {                                    // p is a full ACL packet: [0x02][hf][al][l2caplen][cid][code id len flags dcid opts...]
            if (p.size() >= 13 && p[9] == 0x04 /*CFG_REQ*/) {
                for (size_t i = 13; i + 3 < p.size(); ) { uint8_t t = p[i], ln = p[i + 1];
                    if (t == 0x01 && ln == 2) sawMtu = true; i += 2 + ln; } }
        }
        CHECK(sawMtu);
        (void)ch;
    }
```

- [ ] **b-Step 2: Run it to verify it fails**

Run: `cd ~/Development/M2Radio/bt/test && CXX=c++ ./run.sh`
Expected: FAIL on `CHECK(sawMtu)` — the current CONFIG_REQ is empty.

- [ ] **b-Step 3: Add the MTU option to our CONFIG_REQ**

In `~/Development/M2Radio/bt/L2cap.cpp`, in `service()`, find the CONFIG_REQ we send (the `ch.state == CONFIG && !ch.cfgReqSent` block, currently `uint8_t c[8] = { CFG_REQ, m_nextId++, 4, 0, (uint8_t)ch.remoteCid, (uint8_t)(ch.remoteCid >> 8), 0, 0 };`). Replace it with an MTU-carrying request (use the Mac's value for `SIG_MTU`):

```cpp
        if (ch.state == CONFIG && !ch.cfgReqSent) {                                       // our Config Request: MTU option
            const uint16_t SIG_MTU = <MAC_MTU>;   // the L2CAP MTU the Mac's A2DP source negotiates with this headset
            uint8_t c[12] = { CFG_REQ, m_nextId++, 8, 0, (uint8_t)ch.remoteCid, (uint8_t)(ch.remoteCid >> 8), 0, 0,
                              0x01, 0x02, (uint8_t)SIG_MTU, (uint8_t)(SIG_MTU >> 8) };
            if (sig(c, 12)) ch.cfgReqSent = true; }
```

- [ ] **b-Step 4: Run the suite (new test + the whole suite) green**

Run: `cd ~/Development/M2Radio/bt/test && CXX=c++ ./run.sh`
Expected: the new check passes and every existing `l2cap_test`/`avdtp_test`/`mediapacketizer_test`/… check still passes; `BT-HOST-TESTS: PASS`.

- [ ] **b-Step 5: Commit**

```bash
cd ~/Development/M2Radio
git add bt/L2cap.cpp bt/test/l2cap_test.cpp
git commit -m "fix(bt): send an MTU option in L2CAP CONFIG_REQ

The AVDTP signalling channel's CONFIG_REQ was empty; the Shokz headset requires
an explicit MTU and silently discarded our DISCOVER until its side configured.
MTU value taken from a Mac->Shokz PacketLogger reference. ESP32 path unchanged.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

**Branch (a) — DISCOVER never leaves:** the capture shows no AVDTP `0x01` command outbound. Trace it to `Avdtp::service()`'s `m_kickoff` send returning false (L2cap TXQ full) or the signalling channel not truly OPEN. Fix in `Avdtp.cpp`/`A2dpSource.cpp` per the captured state; add an `avdtp_test` host case that fails when the DISCOVER is not enqueued.

**Branch (c) — response on an unrouted CID/PSM:** `A2dpSource::onData` routes only `ch.psm == Avdtp::PSM && ch.localCid == 0x0041`. If the Shokz answers on a different local CID (e.g., a channel it initiated), extend the routing in `A2dpSource::onData`/`L2cap::onAcl`; add an `l2cap_test` case for the new routing.

**Branch (d) — Shokz reverse flow:** the Mac capture shows the Shokz issuing its own DISCOVER/SDP that we must answer before it proceeds. `Avdtp::onSignalling`/`service()` already answer a peer DISCOVER (`buildDiscoverAcceptOneSource`); extend to whatever the capture shows (e.g., a reverse GET_CAP or an SDP query of us). Add the matching `avdtp_test`/`l2cap_test` case.

---

### Task 6: Demonstrated-red regression guard

**Files:**
- Modify: `~/Development/rt1170/evkb/examples/networking/m2_hci_probe/hci_peer.py` (tighten the `[avdtp]` fake peer) — for branch (b)/(d); OR the host test added in Task 5 already guards (a)/(c).

Ensure the fix cannot silently regress. For branch (b): the host test in Task 5 (`sawMtu`) already fails against the old code — that is the demonstrated-red guard, and it is deterministic. Additionally, tighten the QEMU `[avdtp]` fake peer so a driver that reverts to the empty CONFIG_REQ fails the gate.

- [ ] **Step 1: Make the `[avdtp]` fake peer require the CONFIG_REQ MTU (branch b)**

In `~/Development/rt1170/evkb/examples/networking/m2_hci_probe/hci_peer.py`, in the AVDTP/L2CAP fake-peer path, before answering our `DISCOVER`, assert the incoming CONFIG_REQ carried an MTU option (type 0x01, len 2); if absent, do NOT answer DISCOVER (mimicking the Shokz), so the gate hangs/fails.

- [ ] **Step 2: Demonstrate it RED against the pre-fix driver, then GREEN**

Temporarily revert the Task-5 CONFIG_REQ change (empty request), rebuild `m2_hci_probe`, run its `[avdtp]` gate → it FAILS by name (no DISCOVER answered). Restore the fix, rebuild → the gate PASSES.

Run: `cd ~/Development/rt1170/evkb/examples/networking/m2_hci_probe && ./run_qemu_avdtp.sh; echo "exit=$?"` (the `[avdtp]` gate; the directory also has `run_qemu.sh`, `run_qemu_baud.sh`, `run_qemu_hci.sh`).
Expected: FAIL pre-fix, PASS post-fix.

- [ ] **Step 3: Commit**

```bash
cd ~/Development/rt1170/evkb
git add examples/networking/m2_hci_probe/hci_peer.py
git commit -m "test(m2_hci_probe): [avdtp] fake peer requires CONFIG_REQ MTU

Guards the headset AVDTP fix: a driver that reverts to an empty L2CAP
CONFIG_REQ no longer gets its DISCOVER answered, so the gate fails. Shown
red against the pre-fix driver.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

(For branch (a)/(c)/(d): the host test added in Task 5 is the demonstrated-red guard; add the `[avdtp]` fake-peer tightening only if it can express the fault.)

---

### Task 7: BENCH — silicon STREAMING + clean tone acceptance

**Files:**
- Modify: `~/Development/rt1170/evkb/examples/audio/bt_tone_test/transcript_hw_evkb.txt`

- [ ] **Step 1: Rebuild the bench image with the fix (local M2Radio) + real firmware**

Rebuild `build-trace` (or a fresh `build-media`) with the same flags as Task 4 Step 1 minus `M2_BT_ACL_TRACE` (or keep it — harmless). Local-first resolution picks up the fixed M2Radio.

- [ ] **Step 2: Flash, put the Shokz in explicit PAIRING mode, capture**

Flash via `LinkServer run`; put the Shokz in pairing mode; capture the VCOM. Confirm the sequence reaches AVDTP `STREAMING` (`a2dp_try=ok`, `streaming frames_per_pkt=…`) and `packets` climbs.

- [ ] **Step 3: Verify audible + measure**

Confirm a **clean, continuous 1 kHz tone** on the headset for ≥ 60 s (no dropouts), `packets` climbing, `drops` not dominating. Note whether the headset sustains real-time (tests the throughput theory on a compliant peer).

- [ ] **Step 4: Capture the transcript and commit**

Save the bring-up (connect → SSP pair → DISCOVER → GET_CAP → SET_CONFIG → OPEN → START) + a streaming window to `examples/audio/bt_tone_test/transcript_hw_evkb.txt`.

```bash
cd ~/Development/rt1170/evkb
git add examples/audio/bt_tone_test/transcript_hw_evkb.txt
git commit -m "test(bt_tone_test): silicon -- AVDTP STREAMING + audible tone to a headset

Shokz OpenMove reaches STREAMING (DISCOVER..START) and plays a clean 1 kHz tone
over SSP -- the headset AVDTP DISCOVER fix verified on the bench.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 8: Close-out — push, pins, fresh-user, sweep, audit, write-up

**Files:**
- Modify: `~/Development/rt1170/evkb/evkb.cmake` (bump `M2Radio` pin)
- Modify: `~/Development/rt1170/evkb/CLAUDE.md` (if the gate count/behaviour changed)

- [ ] **Step 1: Push M2Radio; record the SHA**

```bash
cd ~/Development/M2Radio && git push origin master && git rev-parse HEAD
```

- [ ] **Step 2: Bump the M2Radio pin in `evkb.cmake`**

In `~/Development/rt1170/evkb/evkb.cmake`, set the `M2Radio` pin to the SHA from Step 1, and append a one-line dated note (the `onAclTrace` hook + the headset AVDTP fix).

- [ ] **Step 3: Fresh-user verify**

```bash
cd ~/Development/rt1170/evkb/examples/audio/bt_tone_test
rm -rf build-ff && cmake -B build-ff -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake -DEVKB_FORCE_FETCH=ON 2>&1 | tail -3 && cmake --build build-ff 2>&1 | tail -3 && rm -rf build-ff
```
Expected: fetches the pinned M2Radio + cores and builds `bt_tone_test.elf`.

- [ ] **Step 4: Host tests + full sweep + audit**

```bash
cd ~/Development/M2Radio/bt/test && CXX=c++ ./run.sh
~/Development/rt1170/evkb/tools/acl-trace-to-btsnoop.test.sh
cd ~/Development/rt1170/evkb && ./tools/run-all-qemu-gates.sh   # via a short-path symlink if the checkout path is long
LICENSE_AUDIT_EVKB=$(pwd) ./tools/license-audit.sh 2>&1 | tail -3
```
Expected: `BT-HOST-TESTS: PASS`, `BTSNOOP-CONVERTER-TEST: PASS`, the sweep at its recorded count `passed, 0 failed, 0 SKIP` (unchanged; no new QEMU gate — the `[avdtp]` guard tightens an existing one), `LICENSE-AUDIT: PASS`.

- [ ] **Step 5: Commit evkb (pin + any CLAUDE.md note) and push**

```bash
cd ~/Development/rt1170/evkb
git add evkb.cmake CLAUDE.md
git commit -m "build: bump M2Radio pin for headset AVDTP DISCOVER fix + ACL trace

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
git push origin master
```

- [ ] **Step 6: Write-up**

Update `~/.claude/projects/-Users-nicholasnewdigate-Development-rt1170-evkb/memory/m2-bluetooth-a2dp-programme.md` (+ the MEMORY.md index) and post a NEW-9 comment: the cause the capture named, the fix, silicon STREAMING/audible result on the headset, and whether the headset gave clean audio (throughput). Mark headset A2DP as reached (or note the residual throughput thread).

---

## Notes for the executor

- **Task 5 is one branch, not all four.** Task 4's checkpoint decides it. Do not implement branches the capture did not name.
- **`<MAC_MTU>` and the exact CONFIG bytes come from Task 4** — the Mac→Shokz PacketLogger capture. Do not guess; read them off the reference.
- **Never weaken the ESP32 path.** Every fix branch keeps `run_qemu_media` and the ESP32 silicon path working (Task 7 / Task 8 verify).
- **Clean-room:** L2CAP/AVDTP facts from the specs and our own captures; no third-party stack source transcribed. Stage specific files, never `git add -A`.
