# BT-1: HCI transport & identity (B0–B2) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Get the first answered HCI command out of the M2-MAYA-W161's Bluetooth block, then a clean-room H4/HCI transport in `M2Radio/hci/` that reads the card's identity (LMP version, manufacturer, BD_ADDR, buffer sizes) and runs a BR/EDR inquiry with remote-name lookups — gated three ways (card-absent fallback, a Python fake controller on QEMU's LPUART2, silicon).

**Architecture:** Two repos change. The library work lands in the sibling `~/Development/M2Radio` (new `hci/` subdirectory: `H4Parser` byte-stream reassembler → `Hci` command queue/event demux → `HciTransport` over `Serial2` → `HciPump` on a yield `EventResponder`), all clean-room MIT, with `H4Parser`/`Hci`/`HciEvents` pure C++ so they unit-test on the host. The example work lands in this repo: B0 edits to `examples/networking/m2_sdio_probe` (bracketed `HCI_Reset` before/after the SDIO blob download, ROM bytes in hex, two stale comments fixed) and a new `examples/networking/m2_hci_probe` with two gates. One tiny core change in `~/Development/teensy-cores/imxrt1176` (`addMemoryForRead/Write`, the Teensy idiom) because the 64-byte `Serial2` ring cannot hold a 257-byte Extended Inquiry Result.

**Tech Stack:** C++11 (ARM GCC 10 for the board, host `c++` for unit tests), CMake via `evkb.cmake`, POSIX sh gates through `tools/gate-lib.sh` + `tools/qrun`, Python 3 peer scripts, qemu2 (`~/Development/qemu2`, unchanged), LinkServer for silicon.

**Spec:** `docs/superpowers/specs/2026-08-23-m2-bluetooth-a2dp-programme-design.md` §4. This plan covers **B0, B1 and B2**; B3 (baud switch, eDMA RX) gets its own plan after B2's silicon result.

---

## Things to know before starting (read once)

* **Three repos, three `git` roots.** This repo (`rt1176-evkb`) holds examples, gates and docs. `~/Development/M2Radio` and `~/Development/teensy-cores` are separate repos with their own history; commit library and core work *there*. `evkb.cmake` resolves both local-first, so uncommitted edits in the siblings are what the examples build against. The pins in `evkb.cmake` (`teensy_declare_library(cores …)` line 110, `teensy_declare_library(M2Radio …)` line 119) are bumped only in the last task, after pushing.
* **Build an example from its own directory**:
  ```bash
  cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake && cmake --build build
  ```
  A stale build dir fails with "toolchain file not found" or `COMPILERPATH is UNDEFINED` — `rm -rf build` and reconfigure.
* **Run a gate as `./run_qemu.sh`, never `sh run_qemu.sh`** (it re-execs itself under `gtimeout`). Gates do not build; a missing ELF is reported as SKIP by the sweep.
* **Library subdirectories are globbed, not registered**: `import_evkb_library(M2Radio sdio iw416 hci)` compiles `M2Radio/hci/*.cpp` and adds `M2Radio/hci` to the include path (`teensy-cmake-macros/CMakeLists.include.txt:331-350`). `hci/test/` is *not* globbed — host tests live there.
* **qemu2 binds the Nth `-serial` to LPUART(N+1)** (`hw/arm/fsl-imxrt1170.c:1110`), and its LPUART model receives from its chardev (`hw/char/imxrt_lpuart.c:162-185`). Slot 0 is the console (LPUART1 = `Serial1`), slot 1 is the HCI UART (LPUART2 = `Serial2`). With no second `-serial`, LPUART2 has no chardev and receives nothing — that *is* the card-absent case.
* **macOS caps a UNIX socket path at 104 bytes.** The `[hci]` gate's socket therefore lives at `/tmp/m2hci_<pid>_<phase>.sock`, never under the example directory (this worktree's path alone is longer than the cap).
* **The NXP reference is facts-only.** `controller_wifi_nxp.c` and EtherMind are LA_OPT-licensed: read for protocol facts, never transcribe. Bluetooth Core 5.2 Vol 4 Part A (H4) and Part E (HCI) are the normative source for every byte below.
* **End-anchor every gate grep with `[[:space:]]*$`, never a bare `$`.** The
  core's `println()` emits CRLF, so a captured line ends `…match=0\r\n` and a
  bare `$` can never match (verified with `od -c` during Task 1). Every
  end-anchored assertion in the sibling gates already does this —
  `m2_sdio_probe/run_qemu_wifi.sh` has seven, `m2_uap_lwip/run_qemu_uap.sh`
  more. It tolerates the line ending without loosening the content match.
* **Hardware tasks (3 and 13) need the board.** Everything else runs on QEMU and the host. Do the hardware tasks when the board is free; do not block the QEMU work on them.

## File structure

| Path | Responsibility |
|---|---|
| `~/Development/M2Radio/hci/H4Parser.{h,cpp}` | H4 byte stream → packets; pure state machine, no I/O |
| `~/Development/M2Radio/hci/HciIo.h` | the 4-method platform interface (`write/available/read/nowMs`) |
| `~/Development/M2Radio/hci/Hci.{h,cpp}` | command queue honouring `Num_HCI_Command_Packets`, Command Complete/Status matching, timeouts, resync, named errors, event/ACL callbacks |
| `~/Development/M2Radio/hci/HciEvents.{h,cpp}` | pure parsers for Inquiry Result (field-major!) and Remote Name Request Complete |
| `~/Development/M2Radio/hci/HciTransport.{h,cpp}` | `HciIo` over `HardwareSerialIMXRT` (`Serial2`) with a 1 KB extra RX ring |
| `~/Development/M2Radio/hci/HciPump.{h,cpp}` | yield-attached `EventResponder`, one bounded `Hci::service()` per `yield()` |
| `~/Development/M2Radio/hci/test/{h4parser_test,hci_test,hcievents_test}.cpp`, `run.sh` | host unit tests |
| `~/Development/teensy-cores/imxrt1176/HardwareSerial.{h,cpp}` | `addMemoryForRead/Write` |
| `~/Development/teensy-cores/imxrt1176/HardwareSerial2.cpp` | stale comment fix |
| `examples/networking/m2_sdio_probe/m2_sdio_probe.cpp`, `run_qemu.sh` | B0 |
| `examples/networking/m2_hci_probe/{CMakeLists.txt,m2_hci_probe.cpp,run_qemu.sh,run_qemu_hci.sh,hci_peer.py,transcript_qemu.txt,transcript_qemu_hci.txt}` | B1 + B2 |
| `tools/gate-vacuity.test.sh` | three new cases |
| `CLAUDE.md`, `~/Development/M2Radio/README.md`, `evkb.cmake` | gate count, `hci/` docs, pins |

---

### Task 1: B0 — bracketed `HCI_Reset` in `m2_sdio_probe`, ROM bytes in hex, stale comments

**Files:**
- Modify: `examples/networking/m2_sdio_probe/m2_sdio_probe.cpp`
- Modify: `examples/networking/m2_sdio_probe/run_qemu.sh`

- [ ] **Step 1: Add `<string.h>` and rewrite the BT_WAKE comment (stale "R1901 is DNP")**

In `m2_sdio_probe.cpp`, after line 12 (`#include <ctype.h>`) add:

```cpp
#include <string.h>
```

Replace this block (starts at line 171):

```cpp
// BT_WAKE_HOST watch.  J54 pin 20 (UART_WAKE#) is the ONLY card->host signal on
// this connector that actually reaches the MCU:
//
//   J54.20 -> R811 (0R, fitted) -> BT_WAKE_B_3V3 -> R238 (0R, fitted)
//          -> U19.N16 = GPIO_AD_27      (no level shifter -- pin 20 is 3.3 V)
//
// Pin 21 (SDIO_WAKE#) is populated but blocked at jumper J104, open by default.
// Pin 22 (the BT UART TX) dies at R1901, which is DNP. So this pin is the only
// way this card could ever tell us it is alive.
```

with:

```cpp
// BT_WAKE_HOST watch.  J54 pin 20 (UART_WAKE#) reaches the MCU directly:
//
//   J54.20 -> R811 (0R, fitted) -> BT_WAKE_B_3V3 -> R238 (0R, fitted)
//          -> U19.N16 = GPIO_AD_27      (no level shifter -- pin 20 is 3.3 V)
//
// Pin 21 (SDIO_WAKE#) is populated but blocked at jumper J104, open by default.
// Pin 22 (the BT UART TX) reaches the MCU through R1901 -- DNP from the
// factory, BRIDGED by hand on 2026-08-18.  Until that bridge this pin was the
// only way the card could tell us it was alive; now Serial2 receives too.
```

- [ ] **Step 2: Rewrite the `HCI_RESET` comment and add the expected reply**

Replace (line 243):

```cpp
// HCI Reset, H4 framing: packet type 0x01 (command), opcode 0x0C03, plen 0.
// If the card is running BT firmware it must answer with a Command Complete --
// which we can never read (R1901 is DNP) -- but processing it is exactly the
// kind of thing that makes an NXP controller assert BT_WAKE_HOST.
static const uint8_t HCI_RESET[] = { 0x01, 0x03, 0x0C, 0x00 };
```

with:

```cpp
// HCI Reset, H4 framing: packet type 0x01 (command), opcode 0x0C03, plen 0.
// A controller running BT firmware answers with a Command Complete, H4-framed
// as 04 0E 04 01 03 0C 00 (Core 5.2 Vol 4 Part E 7.7.14) -- readable since
// R1901 was bridged on 2026-08-18.
static const uint8_t HCI_RESET[]       = { 0x01, 0x03, 0x0C, 0x00 };
static const uint8_t HCI_RESET_REPLY[] = { 0x04, 0x0E, 0x04, 0x01, 0x03, 0x0C, 0x00 };
```

- [ ] **Step 3: Delete the BT-reset pulse and the GPIO edge watch (they re-mux the RX pad)**

Delete these three pieces exactly (keep `m2R404Continuity`, which sits between the first two):

1. The comment block beginning `// BT_INDEPENDENT_RESET (J54 pin 54, W_DISABLE2#) is driven from GPIO_AD_15` through `#define M2_BT_RST_BIT 14` (lines 270–282).
2. The function `static void m2PulseBtReset() { … }` (lines 295–301).
3. The comment `// Watch the card's UART TX pad as a GPIO for \`ms\` milliseconds.` and the whole `static void m2WatchRxLine(...) { … }` (lines 303–316).

- [ ] **Step 4: Add the byte-capture helpers after `m2RxContinuity`**

Immediately after the closing brace of `static void m2RxContinuity(bool *up, bool *down) { … }` add:

```cpp
// Collect whatever Serial2 receives for `ms` milliseconds.  Returns the byte
// count; up to `cap` bytes are kept in `buf`.  The pad STAYS on LPUART2 --
// the earlier edge-counting watch re-muxed it to GPIO, which is why the
// post-PDn reading on record (2026-08-18) is "26 edges" rather than bytes.
static uint32_t m2CaptureSerial2(uint32_t ms, uint8_t *buf, uint32_t cap) {
    uint32_t n = 0; uint32_t t0 = millis();
    while (millis() - t0 < ms) {
        while (Serial2.available()) {
            int c = Serial2.read();
            if (n < cap) buf[n] = (uint8_t)c;
            n++;
        }
        delay(1);
    }
    return n;
}

static void m2PrintHex(const uint8_t *p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (p[i] < 0x10) Serial1.print('0');
        Serial1.print(p[i], HEX);
    }
}

// Bracketed HCI_Reset: an equal-length QUIET window before the command, so a
// ROM that transmits continuously is not read as a reply, then the command
// and a window for its answer.  Prints both ends:
//   <tag>: quiet=<n> reply=<hex|none> n=<n> match=<0|1>
// match=1 means the reply begins with the spec's Command Complete for Reset.
static void m2HciResetProbe(const char *tag) {
    uint8_t scratch[64]; uint8_t reply[64];
    uint32_t quiet = m2CaptureSerial2(300, scratch, sizeof scratch);
    Serial2.write(HCI_RESET, sizeof HCI_RESET);
    Serial2.flush();
    uint32_t n = m2CaptureSerial2(300, reply, sizeof reply);
    uint32_t shown = n < sizeof reply ? n : (uint32_t)sizeof reply;
    bool match = n >= sizeof HCI_RESET_REPLY &&
                 memcmp(reply, HCI_RESET_REPLY, sizeof HCI_RESET_REPLY) == 0;
    Serial1.print(tag); Serial1.print(": quiet="); Serial1.print(quiet);
    Serial1.print(" reply=");
    if (n == 0) Serial1.print("none"); else m2PrintHex(reply, shown);
    Serial1.print(" n="); Serial1.print(n);
    Serial1.print(" match="); Serial1.println(match ? 1 : 0);
}
```

- [ ] **Step 5: Replace the edge globals with byte globals**

Replace (line 345):

```cpp
static bool g_rxAfterPdn = false; static uint32_t g_rxEdgesAfterPdn = 0;
```

with:

```cpp
static uint8_t g_romBytes[32]; static uint32_t g_romByteCount = 0;
```

- [ ] **Step 6: Print the ROM bytes in `reportProbe()`**

Replace (in `reportProbe()`, around line 707):

```cpp
    Serial1.print("after_pdn_cycle: rx_any_high=");
    Serial1.print(g_rxAfterPdn);
    Serial1.print(" rx_edges=");
    Serial1.println(g_rxEdgesAfterPdn);
```

with:

```cpp
    Serial1.print("after_pdn_cycle: rom_bytes=");
    Serial1.print(g_romByteCount);
    Serial1.print(" hex=");
    if (g_romByteCount == 0) Serial1.print("none");
    else m2PrintHex(g_romBytes, g_romByteCount < sizeof g_romBytes
                                    ? g_romByteCount : (uint32_t)sizeof g_romBytes);
    Serial1.println();
```

- [ ] **Step 7: `setup()` — capture bytes, then the pre-download bracket**

Replace (in `setup()`, lines 1086–1097):

```cpp
    // LPUART2 -> J54 pin 32 (card UART_RXD).  TX only in practice: the card's
    // reply path dies at R1901, which is DNP on this board.  No flow control --
    // CTS/RTS are the gigabit PHY's interrupt and reset lines here.
    m2R404Continuity(&g_r404Up, &g_r404Down);
    m2RxContinuity(&g_rxUp, &g_rxDown);
    Serial2.begin(115200);
    Serial1.println("serial2=up_115200");

    m2ReleaseWifiReset();
    // Immediately after the PDn power-up, watch the card's UART TX for any
    // sign of a booting module: its line should leave the stuck-low state.
    m2WatchRxLine(400, &g_rxAfterPdn, &g_rxEdgesAfterPdn);
    Serial1.println("m2_wifi_reset=released");
```

with:

```cpp
    // LPUART2 <-> J54 pins 32/22 (card UART_RXD/TXD).  Bidirectional since
    // R1901 was bridged on 2026-08-18.  No flow control -- CTS/RTS are the
    // gigabit PHY's interrupt and reset lines on this board.
    m2R404Continuity(&g_r404Up, &g_r404Down);
    m2RxContinuity(&g_rxUp, &g_rxDown);
    Serial2.begin(115200);
    Serial1.println("serial2=up_115200");

    m2ReleaseWifiReset();
    // What the card's UART TX says coming out of PDn -- as BYTES, the pad left
    // on LPUART2.  Serial2's ring already holds what arrived during the 1 s
    // ROM-boot wait inside m2ReleaseWifiReset(); this drains it and listens
    // 400 ms more.  26 edges were counted here on 2026-08-18 with the pad
    // re-muxed to GPIO; this is what they were.
    g_romByteCount = m2CaptureSerial2(400, g_romBytes, sizeof g_romBytes);
    Serial1.println("m2_wifi_reset=released");

    // B0 bracket 1: does the BT core answer HCI_Reset BEFORE the SDIO blob
    // download?  NXP's sequence says no -- the combo image carries the BT
    // firmware and goes down over SDIO -- but nobody has asked this card.
    m2HciResetProbe("hci_pre_download");
```

- [ ] **Step 8: `setup()` — the post-download bracket**

Find the end of the download block in `setup()` — the line `#endif` immediately before `    reportProbe();` (line 1191). Insert between them:

```cpp
    // B0 bracket 2: the same question AFTER the download (or after the
    // attempt, card-absent).  NXP's controller_hci_uart_init waits 100 ms plus
    // up to 260 ms before its first command; 400 ms covers both.
    delay(400);
    m2HciResetProbe("hci_post_download");
```

- [ ] **Step 9: `loop()` — replace the re-muxing probe**

Replace the whole `if (n % 25 == 24) { … }` block in `loop()` (from the comment `// Every ~5 s: characterise the wake line quietly, transmit HCI Reset, then` through the closing `}` after `Serial1.println(lastByte < 0 ? 0 : lastByte, HEX);`) with:

```cpp
    // Every ~5 s: sample the wake line, then the bracketed HCI_Reset probe.
    // The pad stays on LPUART2 throughout -- the old version re-muxed it to
    // GPIO and re-called Serial2.begin(), which is what latched the spurious
    // 0x00 in the 2026-08-17 reading.
    if (n % 25 == 24) {
        bool hi = false; uint32_t edges = 0;
        m2BtWakeSample(2000, &hi, &edges);                  // ~20 ms
        Serial1.print("bt_wake_sample: high="); Serial1.print(hi);
        Serial1.print(" edges="); Serial1.println(edges);
        m2HciResetProbe("hci_probe");
    }
```

- [ ] **Step 10: Build**

```bash
cd examples/networking/m2_sdio_probe && rm -rf build && cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake && cmake --build build 2>&1 | tail -3
```

Expected: ends with `m2_sdio_probe.elf` and `.hex` produced, no warnings about unused functions (`m2WatchRxLine`/`m2PulseBtReset` are gone). If `-Werror=unused-function` fires on anything else, that function was only used by the deleted code — delete it too.

- [ ] **Step 11: Extend the card-absent gate**

In `run_qemu.sh`, change the poll ceiling (the brackets add ~1.6 s):

```sh
for _ in $(seq 1 60); do
```

and add, after the `^alive=2` assertion:

```sh
# B0 (2026-08-23): the two bracketed HCI_Reset probes must be PRESENT in their
# card-absent form.  LPUART2 has no chardev here, so nothing can be received:
# quiet=0 and n=0 are what "nothing answered" looks like, and match=0 says the
# firmware did not mistake silence for the Command Complete.  A missing line
# would mean the probe never reached that bracket.
grep -q "^hci_pre_download: quiet=0 reply=none n=0 match=0[[:space:]]*$" "$OUT" || {
    echo "FAIL: B0 pre-download bracket missing or not in its card-absent form"; exit 1; }
grep -q "^hci_post_download: quiet=0 reply=none n=0 match=0[[:space:]]*$" "$OUT" || {
    echo "FAIL: B0 post-download bracket missing or not in its card-absent form"; exit 1; }
grep -q "^after_pdn_cycle: rom_bytes=0 hex=none[[:space:]]*$" "$OUT" || {
    echo "FAIL: after_pdn_cycle line missing or not in its card-absent form"; exit 1; }
```

- [ ] **Step 12: Run both of the example's gates**

```bash
cd examples/networking/m2_sdio_probe && ./run_qemu.sh 2>&1 | tail -4 && ./run_qemu_wifi.sh 2>&1 | tail -2
```

Expected: `PASS: SDIO enumerate reached the cmd5-no-response fallback cleanly` and the wifi gate's PASS line. If `run_qemu_wifi.sh` asserts any of the removed lines (`bt_reset_pulse`, `after_pdn_cycle: rx_any_high`), it does not — check with `grep -n "bt_reset_pulse\|rx_any_high\|rx_edges" run_qemu_wifi.sh` (expected: no output).

- [ ] **Step 13: Commit**

```bash
git add examples/networking/m2_sdio_probe/m2_sdio_probe.cpp examples/networking/m2_sdio_probe/run_qemu.sh
git commit -m "m2_sdio_probe: B0 -- bracketed HCI_Reset before and after the blob download

The pad now stays on LPUART2: the post-PDn window captures the ROM's bytes in
hex instead of counting edges, and the HCI probe no longer re-muxes the pad or
re-calls Serial2.begin() (the source of the spurious 0x00 on record).  Each
HCI_Reset is bracketed by an equal-length quiet window so a chattering ROM
cannot read as a reply.  Two stale 'R1901 is DNP' comments fixed -- the bridge
was fitted 2026-08-18.  The card-absent gate asserts the new lines in their
card-absent form.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 2: B0 — the core's stale `Serial2` comment

**Files:**
- Modify: `~/Development/teensy-cores/imxrt1176/HardwareSerial2.cpp:46-53`

- [ ] **Step 1: Check the core repo is clean and note its branch**

```bash
git -C ~/Development/teensy-cores status --short && git -C ~/Development/teensy-cores branch --show-current && git -C ~/Development/teensy-cores log --oneline -1
```

Expected: no modified files, HEAD `fcd22b0` (the SHA pinned in `evkb.cmake:110`). If the tree is dirty, stop and report — something else is mid-flight in the core.

- [ ] **Step 2: Fix the comment**

In `HardwareSerial2.cpp` replace:

```cpp
// Two board facts worth knowing before using this port:
//   * RX IS DEAD AS BUILT.  R1901 (module->MCU) is DNP on the EVKB RevC3, so
//     the card's TX reaches the level shifter and stops there.  Transmit works;
//     nothing is ever received.
//   * No flow control is offered.  CTS/RTS would mux GPIO_DISP_B2_12/13, which
//     on this board are RGMII1_PHY_INTB and ETHPHY_RST_B -- asserting RTS holds
//     the gigabit PHY in reset.
// Both are documented in docs/m2-evkb-revc3.md in the rt1176-evkb repo.
```

with:

```cpp
// Two board facts worth knowing before using this port:
//   * RX needs a hand bridge.  R1901 (module->MCU) is DNP on a stock EVKB
//     RevC3, so the card's TX reaches the level shifter and stops there; with
//     R1901 bridged (0402, 0 ohm -- done on this bench 2026-08-18) the port is
//     fully bidirectional.  Transmit works either way.
//   * No flow control is offered.  CTS/RTS would mux GPIO_DISP_B2_12/13, which
//     on this board are RGMII1_PHY_INTB and ETHPHY_RST_B -- asserting RTS holds
//     the gigabit PHY in reset.
// Both are documented in docs/m2-evkb-revc3.md in the rt1176-evkb repo.
```

- [ ] **Step 3: Commit (core repo)**

```bash
git -C ~/Development/teensy-cores add imxrt1176/HardwareSerial2.cpp
git -C ~/Development/teensy-cores commit -m "imxrt1176: Serial2 comment -- R1901 is a bridge, not a dead end

The comment said RX was dead as built.  True of a stock RevC3; the bench
board has had R1901 bridged since 2026-08-18 and the port receives.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 3: B0 — silicon run (needs the board)

**Files:**
- Modify: `examples/networking/m2_sdio_probe/transcript_hw_evkb.txt` (append a section)

- [ ] **Step 1: Build with the blob and no credentials**

```bash
cd examples/networking/m2_sdio_probe && rm -rf build-hw && cmake -B build-hw -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake -DM2RADIO_IW416_FW=$HOME/Development/mcuxsdk-ws/mcuxsdk/components/conn_fwloader/fw_bin/inc/IW416/sduartIW416_wlan_bt.bin.inc && cmake --build build-hw 2>&1 | tail -2
```

Expected: `IW416 firmware: …sduartIW416_wlan_bt.bin.inc` in the configure output; ELF built. (A separate `build-hw` keeps the gate's `build/` ELF blob-free.)

- [ ] **Step 2: Flash with the VCOM free, then attach the console**

```bash
pkill LinkServer; pkill redlinkserv; pkill crt_emu_cm_redlink; sleep 1; LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load examples/networking/m2_sdio_probe/build-hw/m2_sdio_probe.elf 2>&1 | tail -3
```

Expected: a load success line (LinkServer is silent for up to a minute while programming — silent is not hung). Then, in a second terminal:

```bash
python3 tools/rt1170-console.py /dev/cu.usbmodem5DQ2DDHVWO5EI3 115200 | tee /tmp/b0.uart
```

and trigger the reset from the first terminal:

```bash
LinkServer run MIMXRT1176:MIMXRT1170-EVKB examples/networking/m2_sdio_probe/build-hw/m2_sdio_probe.elf > /dev/null 2>&1 &
```

Read for ~40 s, then stop the console reader (Ctrl-C) **before** any further LinkServer command.

- [ ] **Step 3: Read the four lines that matter**

```bash
grep -n "^after_pdn_cycle\|^hci_pre_download\|^fw_download=\|^hci_post_download\|^hci_probe" /tmp/b0.uart | head -12
```

Interpretation, to write into the transcript verbatim with the captured lines:
- `after_pdn_cycle: rom_bytes=N hex=…` — the 26 edges as bytes. Record the hex; do not interpret beyond "the ROM transmits on power-up" unless the bytes are recognisable.
- `hci_pre_download: … match=0` with `quiet=0` — expected: the BT block is not up before the download.
- `hci_post_download: … match=1` — **the first answered HCI command on this card.** If `match=0` but `n>0`, record the bytes; if `n=0` *and* `quiet=0`, the BT block did not come up on the combo download — the B1 risk row ("BT-only UART download path") is live and Task 8's probe should still be built, but flag it to the user before Task 13.
- `hci_probe: …` every 5 s — must keep matching; a reset that answers once and never again is a finding in itself.

- [ ] **Step 4: Compare the card's reply with the host fixture**

The `reply=` hex of whichever bracket matched must begin `040E0401030C00` — the same seven bytes `h4parser_test.cpp` and `hci_test.cpp` use as `RESET_CC`. If the card's Command Complete differs (a different `Num_HCI_Command_Packets`, say `040E0402030C00`), that is the first silicon-captured fixture: change the host tests' `RESET_CC` (and the peer's `cmd_complete(OP_RESET, …, ncmd)`) to the captured bytes in Task 5/6/11, so the fixtures are what the card sent, not what the spec says it should send.

- [ ] **Step 5: Append the transcript section**

Append to `transcript_hw_evkb.txt` a section headed `B0 — BRACKETED HCI_RESET (2026-08-XX)` (use the real date) containing: the build command's blob path, the captured lines from Step 3 verbatim, and a one-paragraph reading that states which bracket answered and quotes the reply bytes. Follow the file's existing convention: claims are backed by quoted capture lines, and nothing is stated that a line does not show.

- [ ] **Step 6: Commit**

```bash
git add examples/networking/m2_sdio_probe/transcript_hw_evkb.txt
git commit -m "m2_sdio_probe: B0 silicon -- HCI_Reset bracketed before/after the download

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

(Amend the commit subject to state the result — e.g. `answered after the download, not before` — once it is known.)

---

### Task 4: Core — `addMemoryForRead()` / `addMemoryForWrite()`

**Files:**
- Modify: `~/Development/teensy-cores/imxrt1176/HardwareSerial.h` (public section, after `availableForWrite`)
- Modify: `~/Development/teensy-cores/imxrt1176/HardwareSerial.cpp`

The class already carries the Teensy 4 two-segment ring (`rx_buffer_storage_`, `rx_buffer_total_size_`, and the same for TX — used at `HardwareSerial.cpp:185,216,236,271,296`); only the setters are missing.

- [ ] **Step 1: Declare**

In `HardwareSerial.h`, after the line `virtual int availableForWrite(void);` add:

```cpp
	// Extend the receive / transmit ring with caller-owned storage (the
	// Teensy 4 idiom).  The built-in 64-byte ring suits a console, not a
	// bursty peer with no flow control: an HCI Extended Inquiry Result event
	// is 257 bytes and LPUART2 cannot ask the card to wait.  May be called
	// before or after begin(); the ring is emptied either way.  Pass nullptr
	// to drop back to the built-in ring.
	void addMemoryForRead(void *buffer, size_t length);
	void addMemoryForWrite(void *buffer, size_t length);
```

- [ ] **Step 2: Define**

In `HardwareSerial.cpp`, after the definition of `availableForWrite` (ends near line 134) add:

```cpp
void HardwareSerialIMXRT::addMemoryForRead(void *buffer, size_t length)
{
	__disable_irq();
	rx_buffer_storage_ = (BUFTYPE *)buffer;
	rx_buffer_total_size_ = buffer ? rx_buffer_size_ + length : rx_buffer_size_;
	rx_buffer_head_ = 0;
	rx_buffer_tail_ = 0;
	__enable_irq();
}

void HardwareSerialIMXRT::addMemoryForWrite(void *buffer, size_t length)
{
	__disable_irq();
	tx_buffer_storage_ = (BUFTYPE *)buffer;
	tx_buffer_total_size_ = buffer ? tx_buffer_size_ + length : tx_buffer_size_;
	tx_buffer_head_ = 0;
	tx_buffer_tail_ = 0;
	__enable_irq();
}
```

- [ ] **Step 3: Prove the existing serial example still builds and passes its gate**

```bash
cd examples/serial/serial_test && rm -rf build && cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake && cmake --build build 2>&1 | tail -1 && ./run_qemu.sh 2>&1 | tail -1
```

Expected: the gate's PASS line. (The new methods are exercised by Task 8's build and Task 9's gate.)

- [ ] **Step 4: Commit (core repo)**

```bash
git -C ~/Development/teensy-cores add imxrt1176/HardwareSerial.h imxrt1176/HardwareSerial.cpp
git -C ~/Development/teensy-cores commit -m "imxrt1176: HardwareSerial addMemoryForRead/Write (Teensy 4 idiom)

The two-segment ring was already there; only the setters were missing.  The
HCI transport on Serial2 needs a 1 KB RX ring: Extended Inquiry Result events
are 257 bytes and LPUART2 has no flow control on this board.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 5: `M2Radio/hci/H4Parser` — H4 byte stream → packets (TDD, host)

**Files:**
- Create: `~/Development/M2Radio/hci/H4Parser.h`
- Create: `~/Development/M2Radio/hci/H4Parser.cpp`
- Create: `~/Development/M2Radio/hci/test/h4parser_test.cpp`
- Create: `~/Development/M2Radio/hci/test/run.sh`

- [ ] **Step 1: Check the library repo is clean**

```bash
git -C ~/Development/M2Radio status --short && git -C ~/Development/M2Radio log --oneline -1
```

Expected: clean, HEAD `300d32b` (the SHA pinned in `evkb.cmake:119`).

- [ ] **Step 2: Write the failing test**

`~/Development/M2Radio/hci/test/h4parser_test.cpp`:

```cpp
// Host unit tests for H4Parser.  Fixture bytes are the Core 5.2 Vol 4 Part E
// encodings; the Command Complete for Reset (04 0E 04 01 03 0C 00) is also
// what m2_sdio_probe's B0 bracket reads off the card.
#include "H4Parser.h"
#include <stdio.h>
#include <string.h>

static int g_fails = 0, g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

struct Sink {
    int packets = 0, faults = 0;
    uint8_t lastType = 0; uint8_t lastFault = 0; uint8_t lastFaultByte = 0;
    size_t lastLen = 0; uint8_t last[H4Parser::MAX_PACKET];
    static void onPacket(void *ctx, uint8_t type, const uint8_t *pkt, size_t len) {
        Sink *s = (Sink *)ctx; s->packets++; s->lastType = type; s->lastLen = len; memcpy(s->last, pkt, len);
    }
    static void onFault(void *ctx, uint8_t fault, uint8_t byte) {
        Sink *s = (Sink *)ctx; s->faults++; s->lastFault = fault; s->lastFaultByte = byte;
    }
};

static const uint8_t RESET_CC[] = { 0x04, 0x0E, 0x04, 0x01, 0x03, 0x0C, 0x00 };

int main() {
    {   // 1. one Command Complete, fed whole
        H4Parser p; Sink s; p.setCallbacks(Sink::onPacket, Sink::onFault, &s);
        p.feed(RESET_CC, sizeof RESET_CC);
        CHECK(s.packets == 1); CHECK(s.faults == 0);
        CHECK(s.lastType == H4Parser::EVENT);
        CHECK(s.lastLen == 6);
        CHECK(memcmp(s.last, RESET_CC + 1, 6) == 0);
        CHECK(p.idle());
    }
    {   // 2. byte-at-a-time delivery is identical
        H4Parser p; Sink s; p.setCallbacks(Sink::onPacket, Sink::onFault, &s);
        for (size_t i = 0; i < sizeof RESET_CC; i++) { p.feed(RESET_CC[i]); CHECK(s.packets == (i + 1 == sizeof RESET_CC ? 1 : 0)); }
        CHECK(s.lastLen == 6);
    }
    {   // 3. two packets back to back in one feed
        H4Parser p; Sink s; p.setCallbacks(Sink::onPacket, Sink::onFault, &s);
        uint8_t two[sizeof RESET_CC * 2]; memcpy(two, RESET_CC, sizeof RESET_CC); memcpy(two + sizeof RESET_CC, RESET_CC, sizeof RESET_CC);
        p.feed(two, sizeof two);
        CHECK(s.packets == 2); CHECK(p.packets() == 2);
    }
    {   // 4. an event with zero parameters completes at the header
        H4Parser p; Sink s; p.setCallbacks(Sink::onPacket, Sink::onFault, &s);
        const uint8_t ev[] = { 0x04, 0x10, 0x00 };            // Hardware Error with no params (illegal but framable)
        p.feed(ev, sizeof ev);
        CHECK(s.packets == 1); CHECK(s.lastLen == 2);
    }
    {   // 5. a bad type byte is a fault, then the stream recovers on the next good packet
        H4Parser p; Sink s; p.setCallbacks(Sink::onPacket, Sink::onFault, &s);
        p.feed(0xFF);
        CHECK(s.faults == 1); CHECK(s.lastFault == H4Parser::BAD_TYPE); CHECK(s.lastFaultByte == 0xFF);
        CHECK(p.idle());
        p.feed(RESET_CC, sizeof RESET_CC);
        CHECK(s.packets == 1); CHECK(p.faults() == 1);
    }
    {   // 6. ACL packet: handle 0x0001, 3 data bytes
        H4Parser p; Sink s; p.setCallbacks(Sink::onPacket, Sink::onFault, &s);
        const uint8_t acl[] = { 0x02, 0x01, 0x00, 0x03, 0x00, 0xAA, 0xBB, 0xCC };
        p.feed(acl, sizeof acl);
        CHECK(s.packets == 1); CHECK(s.lastType == H4Parser::ACL); CHECK(s.lastLen == 7);
        CHECK(s.last[4] == 0xAA && s.last[6] == 0xCC);
    }
    {   // 7. ACL length above the plausibility bound is a fault, not a 64 KB wait
        H4Parser p; Sink s; p.setCallbacks(Sink::onPacket, Sink::onFault, &s);
        p.setAclMax(1021);
        const uint8_t acl[] = { 0x02, 0x01, 0x00, 0xFE, 0x03 };   // 1022 > 1021
        p.feed(acl, sizeof acl);
        CHECK(s.faults == 1); CHECK(s.lastFault == H4Parser::BAD_LENGTH); CHECK(p.idle());
        const uint8_t ok[] = { 0x02, 0x01, 0x00, 0xFD, 0x03 };    // 1021 is allowed
        p.feed(ok, sizeof ok);
        CHECK(s.faults == 1); CHECK(!p.idle());
    }
    {   // 8. setAclMax cannot exceed the buffer
        H4Parser p; p.setAclMax(0xFFFF);
        Sink s; p.setCallbacks(Sink::onPacket, Sink::onFault, &s);
        const uint8_t acl[] = { 0x02, 0x01, 0x00, 0x05, 0x04 };   // 1029 > MAX_PACKET - 4
        p.feed(acl, sizeof acl);
        CHECK(s.faults == 1);
    }
    {   // 9. a 255-byte event (Remote Name Request Complete) fits
        H4Parser p; Sink s; p.setCallbacks(Sink::onPacket, Sink::onFault, &s);
        uint8_t ev[3 + 255] = { 0x04, 0x07, 0xFF };
        p.feed(ev, sizeof ev);
        CHECK(s.packets == 1); CHECK(s.lastLen == 257);
    }
    printf("h4parser_test: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
```

`~/Development/M2Radio/hci/test/run.sh`:

```sh
#!/bin/sh
# Host-side unit tests for the HCI layer.  H4Parser, Hci and HciEvents are
# pure C++ with no Arduino dependency, so they compile with the host compiler.
# Usage: ./hci/test/run.sh   (from the M2Radio root, or anywhere)
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
CXX=${CXX:-c++}
for t in h4parser_test hci_test hcievents_test; do
    [ -f "$DIR/$t.cpp" ] || continue
    $CXX -std=c++11 -Wall -Wextra -Werror -I"$DIR/.." "$DIR/$t.cpp" \
        "$DIR/../H4Parser.cpp" "$DIR/../Hci.cpp" "$DIR/../HciEvents.cpp" -o "$OUT/$t"
    "$OUT/$t"
done
echo "HCI-HOST-TESTS: PASS"
```

Until Tasks 6 and 7 exist, make `run.sh` tolerate their absence: the `[ -f … ] || continue` skips missing tests, but the compile line names `Hci.cpp`/`HciEvents.cpp`. For this task only, create empty placeholders so the link line resolves:

```bash
chmod +x ~/Development/M2Radio/hci/test/run.sh && : > ~/Development/M2Radio/hci/Hci.cpp && : > ~/Development/M2Radio/hci/HciEvents.cpp
```

(Tasks 6 and 7 overwrite them with real content.)

- [ ] **Step 3: Run the test to verify it fails**

```bash
~/Development/M2Radio/hci/test/run.sh
```

Expected: compile error `H4Parser.h: No such file or directory`.

- [ ] **Step 4: Write the parser**

`~/Development/M2Radio/hci/H4Parser.h`:

```cpp
// H4Parser -- reassembles HCI packets from the H4 (UART) byte stream on the
// HOST side of the link.  Pure state machine: no I/O, no Arduino, so it is
// unit-tested on the host (hci/test/).
//
// A host receives two packet types (Core 5.2 Vol 4 Part A 2):
//   0x04 HCI Event : [type][event_code][param_len][params ...]
//   0x02 ACL data  : [type][handle lo][handle hi][len lo][len hi][data ...]
// (loopback-mode echoes arrive as Loopback Command EVENTS, not as command
// packets; SCO is unused on this board.)  Any other type byte is a framing
// fault.  H4 has no sync marker, so a lost byte desyncs the stream for good.
// This class recovers its OWN state immediately -- fault() has already reset
// it to WAIT_TYPE by the time the callback runs -- but that is not enough on a
// real link: the next bytes are still mid-packet garbage.  The LINK recovery
// policy (discard until the line has been idle a while) belongs to the owner,
// which the fault callback exists to tell.
//
// MIT.  Clean-room from the specification.
#pragma once
#include <stdint.h>
#include <stddef.h>

class H4Parser {
public:
    enum Type  : uint8_t { ACL = 0x02, EVENT = 0x04 };
    enum Fault : uint8_t { BAD_TYPE = 1, BAD_LENGTH = 2 };

    // pkt EXCLUDES the H4 type byte: for EVENT it is [code][plen][params],
    // for ACL it is [handle lo][handle hi][len lo][len hi][data].
    typedef void (*PacketFn)(void *ctx, uint8_t type, const uint8_t *pkt, size_t len);
    typedef void (*FaultFn)(void *ctx, uint8_t fault, uint8_t byte);

    static const size_t   EVT_HDR = 2, ACL_HDR = 4;
    static const uint16_t ACL_MAX_DEFAULT = 1024;        // plausibility bound until Read_Buffer_Size
    static const size_t   MAX_PACKET = ACL_HDR + 1024;   // the largest packet the buffer can hold

    H4Parser();
    void setCallbacks(PacketFn onPacket, FaultFn onFault, void *ctx);
    void setAclMax(uint16_t max);                        // clamped to MAX_PACKET - ACL_HDR
    void feed(uint8_t b);
    void feed(const uint8_t *p, size_t n);
    void reset();                                        // back to waiting for a type byte
    bool     idle()    const { return m_state == WAIT_TYPE; }
    uint32_t packets() const { return m_packets; }
    uint32_t faults()  const { return m_faults; }

private:
    enum State : uint8_t { WAIT_TYPE, HEADER, PAYLOAD };
    void emit();
    void fault(uint8_t f, uint8_t b);

    State    m_state;
    uint8_t  m_type;
    size_t   m_len, m_need, m_hdrLen;
    uint16_t m_aclMax;
    uint32_t m_packets, m_faults;
    uint8_t  m_buf[MAX_PACKET];
    PacketFn m_onPacket; FaultFn m_onFault; void *m_ctx;
};
```

`~/Development/M2Radio/hci/H4Parser.cpp`:

```cpp
#include "H4Parser.h"

H4Parser::H4Parser()
    : m_aclMax(ACL_MAX_DEFAULT), m_packets(0), m_faults(0),
      m_onPacket(nullptr), m_onFault(nullptr), m_ctx(nullptr) {
    reset();
}

void H4Parser::setCallbacks(PacketFn onPacket, FaultFn onFault, void *ctx) {
    m_onPacket = onPacket; m_onFault = onFault; m_ctx = ctx;
}

void H4Parser::setAclMax(uint16_t max) {
    const uint16_t cap = (uint16_t)(MAX_PACKET - ACL_HDR);
    m_aclMax = max > cap ? cap : max;
}

void H4Parser::reset() {
    m_state = WAIT_TYPE; m_type = 0; m_len = 0; m_need = 0; m_hdrLen = 0;
}

void H4Parser::feed(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) feed(p[i]);
}

void H4Parser::fault(uint8_t f, uint8_t b) {
    m_faults++;
    reset();
    if (m_onFault) m_onFault(m_ctx, f, b);
}

void H4Parser::emit() {
    m_packets++;
    if (m_onPacket) m_onPacket(m_ctx, m_type, m_buf, m_len);
    reset();
}

void H4Parser::feed(uint8_t b) {
    switch (m_state) {
    case WAIT_TYPE:
        if (b == EVENT)    { m_type = b; m_hdrLen = EVT_HDR; m_state = HEADER; }
        else if (b == ACL) { m_type = b; m_hdrLen = ACL_HDR; m_state = HEADER; }
        else fault(BAD_TYPE, b);
        return;
    case HEADER:
        m_buf[m_len++] = b;
        if (m_len < m_hdrLen) return;
        if (m_type == EVENT) {
            m_need = EVT_HDR + m_buf[1];
        } else {
            uint16_t dlen = (uint16_t)(m_buf[2] | (m_buf[3] << 8));
            if (dlen > m_aclMax) { fault(BAD_LENGTH, b); return; }
            m_need = ACL_HDR + dlen;
        }
        if (m_len == m_need) emit(); else m_state = PAYLOAD;
        return;
    case PAYLOAD:
        m_buf[m_len++] = b;
        if (m_len == m_need) emit();
        return;
    }
}
```

- [ ] **Step 5: Run the test to verify it passes**

```bash
~/Development/M2Radio/hci/test/run.sh
```

Expected: `h4parser_test: N checks, 0 failures` then `HCI-HOST-TESTS: PASS`.

- [ ] **Step 6: Commit (M2Radio)**

```bash
cd ~/Development/M2Radio && git add hci/H4Parser.h hci/H4Parser.cpp hci/Hci.cpp hci/HciEvents.cpp hci/test/h4parser_test.cpp hci/test/run.sh && git commit -m "hci: H4Parser -- H4 byte stream to packets, host-tested

First file of the Bluetooth layer (BT-1 of the M.2 programme).  Pure state
machine; bad type byte and implausible ACL length are faults reported to the
owner, which decides the resync policy.  Hci.cpp/HciEvents.cpp are empty
placeholders so the host test link line resolves until the next two commits.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 6: `M2Radio/hci/Hci` — command queue, reply matching, timeouts, resync (TDD, host)

**Files:**
- Create: `~/Development/M2Radio/hci/HciIo.h`
- Create: `~/Development/M2Radio/hci/Hci.h`
- Overwrite: `~/Development/M2Radio/hci/Hci.cpp`
- Create: `~/Development/M2Radio/hci/test/hci_test.cpp`

- [ ] **Step 1: Write the failing test**

`~/Development/M2Radio/hci/test/hci_test.cpp`:

```cpp
// Host unit tests for Hci against a scripted HciIo.  Every failure path the
// [hci] gate later exercises against the Python peer is pinned here first:
// timeout, framing (garbage then reply), credit starvation, late reply.
#include "Hci.h"
#include <stdio.h>
#include <string.h>
#include <vector>
#include <deque>
#include <functional>

static int g_fails = 0, g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

struct FakeIo : HciIo {
    std::vector<uint8_t> tx;          // everything the host wrote
    std::deque<uint8_t>  rx;          // what the controller will deliver
    uint32_t now = 0;
    int writes = 0;
    std::function<void(FakeIo &, const std::vector<uint8_t> &)> onWrite;
    size_t write(const uint8_t *p, size_t n) override {
        std::vector<uint8_t> pkt(p, p + n);
        tx.insert(tx.end(), p, p + n);
        writes++;
        if (onWrite) onWrite(*this, pkt);
        return n;
    }
    int available() override { return (int)rx.size(); }
    int read() override { if (rx.empty()) return -1; uint8_t b = rx.front(); rx.pop_front(); return b; }
    uint32_t nowMs() override { return now; }
    void deliver(std::initializer_list<uint8_t> b) { rx.insert(rx.end(), b); }
};

static FakeIo *g_io = nullptr;
static void idle10() { g_io->now += 10; }

static const uint8_t RESET_CMD[] = { 0x01, 0x03, 0x0C, 0x00 };

int main() {
    {   // 1. Reset answered: OK, the H4 command bytes are right, one credit remains
        FakeIo io; g_io = &io; Hci hci(io); hci.begin();
        io.onWrite = [](FakeIo &f, const std::vector<uint8_t> &) { f.deliver({0x04, 0x0E, 0x04, 0x01, 0x03, 0x0C, 0x00}); };
        Hci::Reply r;
        CHECK(hci.run(0x0C03, nullptr, 0, &r, 500, idle10) == Hci::OK);
        CHECK(io.tx.size() == 4 && memcmp(io.tx.data(), RESET_CMD, 4) == 0);
        CHECK(r.status == 0); CHECK(r.len == 0); CHECK(!r.statusEvent);
        CHECK(hci.ncmd() == 1); CHECK(hci.timeouts() == 0); CHECK(hci.lastError() == Hci::OK);
    }
    {   // 2. Command with parameters is framed [01][op lo][op hi][plen][params]
        FakeIo io; g_io = &io; Hci hci(io); hci.begin();
        io.onWrite = [](FakeIo &f, const std::vector<uint8_t> &) { f.deliver({0x04, 0x0F, 0x04, 0x00, 0x01, 0x01, 0x04}); };
        const uint8_t p[5] = { 0x33, 0x8B, 0x9E, 0x08, 0x00 };
        Hci::Reply r;
        CHECK(hci.run(0x0401, p, 5, &r, 500, idle10) == Hci::OK);
        const uint8_t want[] = { 0x01, 0x01, 0x04, 0x05, 0x33, 0x8B, 0x9E, 0x08, 0x00 };
        CHECK(io.tx.size() == sizeof want && memcmp(io.tx.data(), want, sizeof want) == 0);
        CHECK(r.statusEvent); CHECK(r.status == 0);
    }
    {   // 3. No reply: TIMEOUT after the deadline, counted, named
        FakeIo io; g_io = &io; Hci hci(io); hci.begin();
        Hci::Reply r;
        uint32_t t0 = io.now;
        CHECK(hci.run(0x0C03, nullptr, 0, &r, 500, idle10) == Hci::TIMEOUT);
        CHECK(io.now - t0 >= 500 && io.now - t0 < 600);
        CHECK(hci.timeouts() == 1); CHECK(r.status == 0xFF);
        CHECK(strcmp(Hci::errorName(Hci::TIMEOUT), "no_response") == 0);
    }
    {   // 4. Garbage then the reply in one burst: FRAMING now, and the NEXT command
        //    waits for the 50 ms quiet before it is sent, then succeeds.
        FakeIo io; g_io = &io; Hci hci(io); hci.begin();
        io.onWrite = [](FakeIo &f, const std::vector<uint8_t> &) {
            if (f.writes == 1) f.deliver({0xFF, 0xFF, 0xFF, 0x04, 0x0E, 0x04, 0x01, 0x03, 0x0C, 0x00});
            else               f.deliver({0x04, 0x0E, 0x04, 0x01, 0x03, 0x0C, 0x00});
        };
        Hci::Reply r;
        CHECK(hci.run(0x0C03, nullptr, 0, &r, 500, idle10) == Hci::FRAMING);
        CHECK(hci.ncmd() == 1);   // the killed command's credit came back; without
                                  // this the link deadlocks -- the reply that would
                                  // have restored it was in the discarded burst
        CHECK(hci.framing() == 1);
        uint32_t t1 = io.now;
        CHECK(hci.run(0x0C03, nullptr, 0, &r, 500, idle10) == Hci::OK);
        CHECK(io.writes == 2);
        CHECK(io.now - t1 >= Hci::IDLE_RESYNC_MS);        // it waited for the line to go quiet
        CHECK(hci.timeouts() == 0);
    }
    {   // 5. A line that never goes quiet fails the waiting command as FRAMING, not TIMEOUT
        FakeIo io; g_io = &io; Hci hci(io); hci.begin();
        io.deliver({0xFF});
        hci.service();                                      // fault -> resync
        CHECK(hci.framing() == 1);
        // keep one garbage byte arriving every 10 ms
        Hci::Reply r;
        static FakeIo *babble = nullptr; babble = &io;
        struct L { static void idle() { babble->now += 10; babble->deliver({0xFF}); } };
        CHECK(hci.run(0x0C03, nullptr, 0, &r, 200, L::idle) == Hci::FRAMING);
        CHECK(io.writes == 0);                              // never dispatched
        CHECK(hci.framing() == 1);                          // discarded bytes are not new faults
    }
    {   // 6. Credit starvation: Reset answered with ncmd=0, the next command starves
        FakeIo io; g_io = &io; Hci hci(io); hci.begin();
        io.onWrite = [](FakeIo &f, const std::vector<uint8_t> &) { f.deliver({0x04, 0x0E, 0x04, 0x00, 0x03, 0x0C, 0x00}); };
        Hci::Reply r;
        CHECK(hci.run(0x0C03, nullptr, 0, &r, 500, idle10) == Hci::OK);
        CHECK(hci.ncmd() == 0);
        CHECK(hci.run(0x1001, nullptr, 0, &r, 300, idle10) == Hci::NCMD_STARVED);
        CHECK(hci.starved() == 1); CHECK(io.writes == 1);
        CHECK(strcmp(Hci::errorName(Hci::NCMD_STARVED), "ncmd_starved") == 0);
    }
    {   // 7. A reply to a command already given up on is counted as late and restores the credit
        FakeIo io; g_io = &io; Hci hci(io); hci.begin();
        Hci::Reply r;
        CHECK(hci.run(0x0C03, nullptr, 0, &r, 100, idle10) == Hci::TIMEOUT);
        CHECK(hci.ncmd() == 0);
        io.deliver({0x04, 0x0E, 0x04, 0x01, 0x03, 0x0C, 0x00});
        hci.service();
        CHECK(hci.late() == 1); CHECK(hci.ncmd() == 1);
    }
    {   // 8. Read_Local_Version return parameters land in the reply after the status byte
        FakeIo io; g_io = &io; Hci hci(io); hci.begin();
        io.onWrite = [](FakeIo &f, const std::vector<uint8_t> &) {
            f.deliver({0x04, 0x0E, 0x0C, 0x01, 0x01, 0x10, 0x00, 0x0B, 0xEF, 0xBE, 0x0B, 0x34, 0x12, 0xFE, 0xCA});
        };
        Hci::Reply r;
        CHECK(hci.run(0x1001, nullptr, 0, &r, 500, idle10) == Hci::OK);
        CHECK(r.len == 8);
        CHECK(r.params[0] == 0x0B);
        CHECK((r.params[1] | (r.params[2] << 8)) == 0xBEEF);
        CHECK((r.params[4] | (r.params[5] << 8)) == 0x1234);
    }
    {   // 9. Non-zero status is STATUS, with the status byte exposed
        FakeIo io; g_io = &io; Hci hci(io); hci.begin();
        io.onWrite = [](FakeIo &f, const std::vector<uint8_t> &) { f.deliver({0x04, 0x0E, 0x04, 0x01, 0x03, 0x0C, 0x01}); };
        Hci::Reply r;
        CHECK(hci.run(0x0C03, nullptr, 0, &r, 500, idle10) == Hci::STATUS);
        CHECK(r.status == 0x01);
    }
    {   // 10. Asynchronous events reach the callback; Command Complete/Status do not
        FakeIo io; g_io = &io; Hci hci(io); hci.begin();
        struct Ev { int n = 0; uint8_t code = 0; uint8_t len = 0; uint8_t p0 = 0xEE;
                    static void fn(void *c, uint8_t code, const uint8_t *p, uint8_t len) { Ev *e = (Ev *)c; e->n++; e->code = code; e->len = len; e->p0 = len ? p[0] : 0xEE; } } ev;
        hci.onEvent(Ev::fn, &ev);
        io.deliver({0x04, 0x01, 0x01, 0x00});                // Inquiry Complete, status 0
        io.deliver({0x04, 0x0E, 0x03, 0x01, 0x00, 0x00});    // Command Complete for NOP: credit only
        hci.service();
        CHECK(ev.n == 1); CHECK(ev.code == 0x01); CHECK(ev.len == 1); CHECK(ev.p0 == 0);
        CHECK(hci.events() == 1); CHECK(hci.late() == 0);
    }
    {   // 11. ACL data reaches the ACL callback with the handle masked to 12 bits
        FakeIo io; g_io = &io; Hci hci(io); hci.begin();
        struct A { uint16_t h = 0; uint16_t len = 0; uint8_t d0 = 0;
                   static void fn(void *c, uint16_t h, const uint8_t *d, uint16_t len) { A *a = (A *)c; a->h = h; a->len = len; a->d0 = d[0]; } } a;
        hci.onAcl(A::fn, &a);
        io.deliver({0x02, 0x01, 0x20, 0x02, 0x00, 0xAA, 0xBB});   // handle 0x0001 with PB flags 0x2
        hci.service();
        CHECK(a.h == 0x0001); CHECK(a.len == 2); CHECK(a.d0 == 0xAA);
    }
    {   // 12. run() refuses to overlap
        FakeIo io; g_io = &io; Hci hci(io); hci.begin();
        Hci::Reply r;
        CHECK(hci.submit(0x0C03, nullptr, 0, nullptr, nullptr) == Hci::OK);
        CHECK(hci.run(0x1001, nullptr, 0, &r, 100, idle10) == Hci::BUSY);
    }
    printf("hci_test: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
~/Development/M2Radio/hci/test/run.sh
```

Expected: compile error `Hci.h: No such file or directory`.

- [ ] **Step 3: Write the interface and the class**

`~/Development/M2Radio/hci/HciIo.h`:

```cpp
// HciIo -- the platform glue the HCI layer needs: raw bytes each way and a
// millisecond clock.  HciTransport implements it over Serial2; the host unit
// tests implement it with a scripted fake.  MIT.
#pragma once
#include <stdint.h>
#include <stddef.h>

struct HciIo {
    virtual ~HciIo() {}
    virtual size_t   write(const uint8_t *p, size_t n) = 0;
    virtual int      available() = 0;
    virtual int      read() = 0;               // -1 when nothing is waiting
    virtual uint32_t nowMs() = 0;
};
```

`~/Development/M2Radio/hci/Hci.h`:

```cpp
// Hci -- the host side of an HCI link over H4 (Core 5.2 Vol 4 Part E).
//
// A small command queue that honours Num_HCI_Command_Packets, matches
// Command Complete / Command Status to the command in flight by opcode, times
// commands out, and hands everything else (asynchronous events, ACL data) to
// callbacks for the layers above.  Never blocks: service() is one bounded
// pass, and run() is a convenience for probes that loops service() itself.
//
// Every way a command can fail has a NAME and a COUNTER (the WiFiClass
// lastError() idiom), because H4 has no flow control on this board and no
// sync marker: a lost byte desyncs the stream for good.  The parser's fault
// starts a RESYNC -- bytes are discarded until the line has been idle for
// IDLE_RESYNC_MS -- and the command in flight fails as FRAMING, not TIMEOUT.
//
// Fixed pools, no heap.  MIT, clean-room from the specification.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "H4Parser.h"
#include "HciIo.h"

class Hci {
public:
    enum Error : uint8_t {
        OK = 0,
        TIMEOUT,        // no Command Complete / Command Status within the deadline
        FRAMING,        // a framing fault while in flight, or the line never went quiet
        NCMD_STARVED,   // the controller never granted a command credit
        QUEUE_FULL,     // submit() with QUEUE_DEPTH commands already queued
        STATUS,         // answered, with a non-zero status (see Reply::status)
        BUSY,           // run() while another command is queued or in flight
    };
    static const char *errorName(Error e);

    struct Reply {
        uint8_t status;        // Command Complete: return parameter 0.  Command Status: its status byte.
        bool    statusEvent;   // answered by Command Status (0x0F) rather than Command Complete (0x0E)
        uint8_t len;           // bytes in params (return parameters AFTER the status byte)
        uint8_t params[254];
    };
    typedef void (*DoneFn)(void *ctx, Error e, const Reply *reply);   // reply is null on failure
    typedef void (*EventFn)(void *ctx, uint8_t code, const uint8_t *params, uint8_t len);
    typedef void (*AclFn)(void *ctx, uint16_t handle, const uint8_t *data, uint16_t len);

    static const uint8_t  QUEUE_DEPTH    = 4;
    static const uint32_t IDLE_RESYNC_MS = 50;

    explicit Hci(HciIo &io);
    void begin();                        // reset state; one credit (Vol 4 Part E 4.4)
    void service();                      // one bounded pass; never blocks

    // Queue a command.  `done` fires from service() when it completes or fails.
    // A queued command not dispatched within the timeout fails: NCMD_STARVED
    // when the credit count is 0, FRAMING when the line never went quiet.
    Error submit(uint16_t opcode, const uint8_t *params, uint8_t plen, DoneFn done, void *ctx);
    // Probe helper: submit, then loop service() (calling `idle` between passes,
    // e.g. a delay(1)) until the command completes or fails.  Refuses (BUSY) if
    // anything is queued or in flight.
    Error run(uint16_t opcode, const uint8_t *params, uint8_t plen, Reply *reply,
              uint32_t timeoutMs, void (*idle)() = nullptr);

    void onEvent(EventFn fn, void *ctx) { m_onEvent = fn; m_eventCtx = ctx; }
    void onAcl(AclFn fn, void *ctx)     { m_onAcl = fn; m_aclCtx = ctx; }
    void setAclMax(uint16_t max)        { m_parser.setAclMax(max); }   // from Read_Buffer_Size
    void setCommandTimeout(uint32_t ms) { m_timeoutMs = ms; }          // for submit(); run() takes its own

    bool     busy()      const { return m_inflight || m_qCount != 0; }
    uint8_t  ncmd()      const { return m_ncmd; }
    uint32_t timeouts()  const { return m_timeouts; }
    uint32_t framing()   const { return m_framing; }      // parser faults seen
    uint32_t starved()   const { return m_starved; }
    uint32_t queueFull() const { return m_queueFull; }
    uint32_t late()      const { return m_late; }         // replies to commands already given up on
    uint32_t events()    const { return m_events; }       // asynchronous events delivered
    Error    lastError() const { return m_lastError; }

private:
    struct Cmd { uint16_t opcode; uint8_t plen; uint8_t params[255]; DoneFn done; void *ctx; uint32_t queuedAt; };
    static void packetThunk(void *ctx, uint8_t type, const uint8_t *pkt, size_t len);
    static void faultThunk(void *ctx, uint8_t fault, uint8_t byte);
    void onPacket(uint8_t type, const uint8_t *pkt, size_t len);
    void onFault();
    void dispatch();
    void finish(Error e, const Reply *r);

    HciIo   &m_io;
    H4Parser m_parser;
    Cmd      m_q[QUEUE_DEPTH];
    uint8_t  m_qHead, m_qCount;
    bool     m_inflight; uint16_t m_inflightOpcode; uint32_t m_sentAt;
    uint8_t  m_ncmd;
    uint32_t m_timeoutMs;
    bool     m_resync; uint32_t m_lastByteAt;
    Reply    m_scratch;
    uint32_t m_timeouts, m_framing, m_starved, m_queueFull, m_late, m_events;
    Error    m_lastError;
    EventFn  m_onEvent; void *m_eventCtx;
    AclFn    m_onAcl;   void *m_aclCtx;
};
```

`~/Development/M2Radio/hci/Hci.cpp`:

```cpp
#include "Hci.h"
#include <string.h>

const char *Hci::errorName(Error e) {
    switch (e) {
        case OK:           return "ok";
        case TIMEOUT:      return "no_response";
        case FRAMING:      return "framing";
        case NCMD_STARVED: return "ncmd_starved";
        case QUEUE_FULL:   return "queue_full";
        case STATUS:       return "status";
        case BUSY:         return "busy";
    }
    return "unknown";
}

Hci::Hci(HciIo &io) : m_io(io) { begin(); }

void Hci::begin() {
    m_parser.reset();
    m_parser.setCallbacks(packetThunk, faultThunk, this);
    m_qHead = 0; m_qCount = 0;
    m_inflight = false; m_inflightOpcode = 0; m_sentAt = 0;
    m_ncmd = 1;                       // Vol 4 Part E 4.4: one command before any reply
    m_timeoutMs = 1000;
    m_resync = false; m_lastByteAt = 0;
    m_timeouts = m_framing = m_starved = m_queueFull = m_late = m_events = 0;
    m_lastError = OK;
    m_onEvent = nullptr; m_eventCtx = nullptr;
    m_onAcl = nullptr; m_aclCtx = nullptr;
}

void Hci::packetThunk(void *ctx, uint8_t type, const uint8_t *pkt, size_t len) {
    ((Hci *)ctx)->onPacket(type, pkt, len);
}
void Hci::faultThunk(void *ctx, uint8_t, uint8_t) { ((Hci *)ctx)->onFault(); }

Hci::Error Hci::submit(uint16_t opcode, const uint8_t *params, uint8_t plen, DoneFn done, void *ctx) {
    if (m_qCount >= QUEUE_DEPTH) { m_queueFull++; m_lastError = QUEUE_FULL; return QUEUE_FULL; }
    Cmd &c = m_q[(m_qHead + m_qCount) % QUEUE_DEPTH];
    c.opcode = opcode; c.plen = plen;
    if (plen) memcpy(c.params, params, plen);
    c.done = done; c.ctx = ctx; c.queuedAt = m_io.nowMs();
    m_qCount++;
    dispatch();
    return OK;
}

// Send the head command if a credit is available and the line is in sync;
// otherwise age it, and fail it by name when it has waited a full timeout.
void Hci::dispatch() {
    if (m_inflight || m_qCount == 0) return;
    uint32_t now = m_io.nowMs();
    Cmd &c = m_q[m_qHead];
    if (m_ncmd > 0 && !m_resync) {
        uint8_t hdr[4] = { 0x01, (uint8_t)(c.opcode & 0xFF), (uint8_t)(c.opcode >> 8), c.plen };
        m_io.write(hdr, 4);
        if (c.plen) m_io.write(c.params, c.plen);
        m_inflight = true; m_inflightOpcode = c.opcode; m_sentAt = now;
        m_ncmd--;
        return;
    }
    if (now - c.queuedAt >= m_timeoutMs) {
        if (m_ncmd == 0) { m_starved++; finish(NCMD_STARVED, nullptr); }
        else             { finish(FRAMING, nullptr); }          // the line never went quiet
    }
}

void Hci::service() {
    uint32_t now = m_io.nowMs();
    while (m_io.available() > 0) {
        int b = m_io.read();
        if (b < 0) break;
        m_lastByteAt = now;
        if (m_resync) continue;                  // discard until the line has been idle
        m_parser.feed((uint8_t)b);
    }
    if (m_resync && (now - m_lastByteAt) >= IDLE_RESYNC_MS) { m_resync = false; m_parser.reset(); }
    if (m_inflight && (now - m_sentAt) >= m_timeoutMs) { m_timeouts++; finish(TIMEOUT, nullptr); }
    dispatch();
}

// Complete the HEAD command (in flight or still waiting) and pop it.
void Hci::finish(Error e, const Reply *r) {
    DoneFn done = m_q[m_qHead].done; void *ctx = m_q[m_qHead].ctx;
    m_qHead = (uint8_t)((m_qHead + 1) % QUEUE_DEPTH); m_qCount--;
    m_inflight = false;
    if (e != OK) m_lastError = e;
    if (done) done(ctx, e, r);
}

void Hci::onPacket(uint8_t type, const uint8_t *pkt, size_t len) {
    if (type == H4Parser::ACL) {
        if (len < 4) return;
        uint16_t handle = (uint16_t)((pkt[0] | (pkt[1] << 8)) & 0x0FFF);
        uint16_t dlen   = (uint16_t)(pkt[2] | (pkt[3] << 8));
        if (m_onAcl) m_onAcl(m_aclCtx, handle, pkt + 4, dlen);
        return;
    }
    if (len < 2) return;
    uint8_t code = pkt[0], plen = pkt[1];
    const uint8_t *p = pkt + 2;
    if (code == 0x0E && plen >= 3) {                             // Command Complete
        m_ncmd = p[0];
        uint16_t opcode = (uint16_t)(p[1] | (p[2] << 8));
        if (m_inflight && opcode == m_inflightOpcode) {
            m_scratch.statusEvent = false;
            m_scratch.status = plen >= 4 ? p[3] : 0;
            m_scratch.len    = plen >= 4 ? (uint8_t)(plen - 4) : 0;
            if (m_scratch.len) memcpy(m_scratch.params, p + 4, m_scratch.len);
            finish(m_scratch.status == 0 ? OK : STATUS, &m_scratch);
        } else if (opcode != 0x0000) {                           // 0x0000 = NOP: credit only
            m_late++;
        }
        return;
    }
    if (code == 0x0F && plen >= 4) {                             // Command Status
        uint8_t status = p[0]; m_ncmd = p[1];
        uint16_t opcode = (uint16_t)(p[2] | (p[3] << 8));
        if (m_inflight && opcode == m_inflightOpcode) {
            m_scratch.statusEvent = true; m_scratch.status = status; m_scratch.len = 0;
            finish(status == 0 ? OK : STATUS, &m_scratch);
        } else if (opcode != 0x0000) {
            m_late++;
        }
        return;
    }
    m_events++;
    if (m_onEvent) m_onEvent(m_eventCtx, code, p, plen);
}

void Hci::onFault() {
    m_framing++;
    m_resync = true; m_lastByteAt = m_io.nowMs();
    if (m_inflight) {
        // Give back the credit dispatch() spent on the command we are about to
        // kill.  m_ncmd is assigned ABSOLUTELY from each reply, so if the reply
        // that carried the credit is one of the bytes the resync discards, the
        // count is stuck low with nothing able to raise it: no credit means no
        // command, and no command means no reply.  One garbage burst would wedge
        // the link for good -- and on this board it is a garbage burst that is
        // expected, LPUART2 having no usable flow control.
        // This restores only what we spent (it cannot fire unless we sent), so it
        // cannot invent a credit that was never ours.  It CAN leave us one too
        // many if the lost reply was going to grant zero; the next reply
        // reassigns the true count, and over by one self-corrects where a
        // deadlock does not.  The nothing-in-flight case needs no help: a
        // controller freeing a buffer sends a NOP (opcode 0x0000) Command
        // Complete carrying the credit, which onPacket() already accepts.
        m_ncmd++;
        finish(FRAMING, nullptr);
    }
}

namespace {
struct RunCtx { Hci::Reply *reply; bool done; Hci::Error err; };
void runDone(void *ctx, Hci::Error e, const Hci::Reply *r) {
    RunCtx *c = (RunCtx *)ctx;
    if (r && c->reply) *c->reply = *r;
    c->err = e; c->done = true;
}
}

Hci::Error Hci::run(uint16_t opcode, const uint8_t *params, uint8_t plen, Reply *reply,
                    uint32_t timeoutMs, void (*idle)()) {
    if (busy()) { m_lastError = BUSY; return BUSY; }
    if (reply) { reply->status = 0xFF; reply->statusEvent = false; reply->len = 0; }
    RunCtx c = { reply, false, OK };
    uint32_t saved = m_timeoutMs; m_timeoutMs = timeoutMs;
    Error e = submit(opcode, params, plen, runDone, &c);
    if (e != OK) { m_timeoutMs = saved; return e; }
    while (!c.done) { service(); if (!c.done && idle) idle(); }
    m_timeoutMs = saved;
    return c.err;
}
```

- [ ] **Step 4: Run the tests**

```bash
~/Development/M2Radio/hci/test/run.sh
```

Expected: `h4parser_test: … 0 failures`, `hci_test: … 0 failures`, `HCI-HOST-TESTS: PASS`. If case 4's second `run()` fails as TIMEOUT rather than OK, `dispatch()` sent while `m_resync` was still set — re-check the `!m_resync` guard.

- [ ] **Step 5: Commit (M2Radio)**

```bash
cd ~/Development/M2Radio && git add hci/HciIo.h hci/Hci.h hci/Hci.cpp hci/test/hci_test.cpp && git commit -m "hci: Hci -- command queue, reply matching, timeouts, resync; every exit named

Honours Num_HCI_Command_Packets, matches Command Complete/Status by opcode,
fails the in-flight command as FRAMING (not TIMEOUT) on a parser fault and
discards until the line has been idle 50 ms -- the only resync an H4 link
with no flow control has.  Twelve host cases pin the failure paths the [hci]
gate will drive against the Python peer.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 7: `M2Radio/hci/HciEvents` — Inquiry Result and Remote Name parsers (TDD, host)

**Files:**
- Create: `~/Development/M2Radio/hci/HciEvents.h`
- Overwrite: `~/Development/M2Radio/hci/HciEvents.cpp`
- Create: `~/Development/M2Radio/hci/test/hcievents_test.cpp`

The Inquiry Result event (Vol 4 Part E 7.7.2) is **field-major**: with N responses it carries all N `BD_ADDR`s, then all N `Page_Scan_Repetition_Mode`s, then N×2 reserved bytes, then all N `Class_Of_Device`s, then all N `Clock_Offset`s — not N structs. A struct-major parser reads the second device's address out of the first device's class bytes. This is the bug the host test exists to catch.

- [ ] **Step 1: Write the failing test**

`~/Development/M2Radio/hci/test/hcievents_test.cpp`:

```cpp
#include "HciEvents.h"
#include <stdio.h>
#include <string.h>

static int g_fails = 0, g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

int main() {
    {   // Inquiry Result with TWO responses, field-major layout (Vol 4 Part E 7.7.2)
        const uint8_t p[] = {
            0x02,                                            // Num_Responses
            0x01, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA,              // BD_ADDR[0]  (LE on the wire -> AA:BB:CC:DD:EE:01)
            0x02, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA,              // BD_ADDR[1]
            0x01, 0x01,                                      // Page_Scan_Repetition_Mode[0..1]
            0x00, 0x00, 0x00, 0x00,                          // Reserved[0..1]
            0x04, 0x04, 0x24,                                // Class_Of_Device[0] = 0x240404
            0x08, 0x04, 0x24,                                // Class_Of_Device[1] = 0x240408
            0x34, 0x12,                                      // Clock_Offset[0] = 0x1234
            0x78, 0x56,                                      // Clock_Offset[1] = 0x5678
        };
        CHECK(hciInquiryResultCount(p, sizeof p) == 2);
        HciInquiryResult r;
        CHECK(hciParseInquiryResult(p, sizeof p, 0, &r));
        CHECK(r.bd[5] == 0xAA && r.bd[0] == 0x01); CHECK(r.psrm == 1); CHECK(r.cod == 0x240404); CHECK(r.clockOffset == 0x1234);
        CHECK(hciParseInquiryResult(p, sizeof p, 1, &r));
        CHECK(r.bd[0] == 0x02); CHECK(r.cod == 0x240408); CHECK(r.clockOffset == 0x5678);
        CHECK(!hciParseInquiryResult(p, sizeof p, 2, &r));                  // out of range
        CHECK(hciInquiryResultCount(p, sizeof p - 1) == 0);                // truncated -> none
    }
    {   // Remote Name Request Complete: status, BD_ADDR, 248-byte NUL-padded name
        uint8_t p[1 + 6 + 248]; memset(p, 0, sizeof p);
        p[0] = 0x00; const uint8_t bd[6] = { 0x01, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA }; memcpy(p + 1, bd, 6);
        memcpy(p + 7, "FAKE-HEADSET-01", 15);
        HciRemoteName n;
        CHECK(hciParseRemoteNameComplete(p, sizeof p, &n));
        CHECK(n.status == 0); CHECK(memcmp(n.bd, bd, 6) == 0); CHECK(strcmp(n.name, "FAKE-HEADSET-01") == 0);
        CHECK(!hciParseRemoteNameComplete(p, 6, &n));                       // too short
        // a name that fills all 248 bytes is still terminated
        memset(p + 7, 'x', 248);
        CHECK(hciParseRemoteNameComplete(p, sizeof p, &n)); CHECK(strlen(n.name) == 248);
    }
    {   // BD_ADDR formatting: MSB first, the usual representation
        const uint8_t bd[6] = { 0x66, 0x55, 0x44, 0x33, 0x22, 0x11 };
        char s[18]; hciFormatBd(bd, s);
        CHECK(strcmp(s, "11:22:33:44:55:66") == 0);
    }
    printf("hcievents_test: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
~/Development/M2Radio/hci/test/run.sh
```

Expected: compile error `HciEvents.h: No such file or directory`.

- [ ] **Step 3: Write the parsers**

`~/Development/M2Radio/hci/HciEvents.h`:

```cpp
// HciEvents -- pure parsers for the HCI events BT-1 consumes.  No I/O, no
// Arduino; host-tested.  Byte layouts from Core 5.2 Vol 4 Part E 7.7.
// MIT, clean-room.
#pragma once
#include <stdint.h>
#include <stddef.h>

struct HciInquiryResult {
    uint8_t  bd[6];          // as on the wire (little-endian); hciFormatBd prints it MSB-first
    uint8_t  psrm;           // Page_Scan_Repetition_Mode
    uint32_t cod;            // Class_Of_Device, 24 bits
    uint16_t clockOffset;
};

struct HciRemoteName {
    uint8_t status;
    uint8_t bd[6];
    char    name[249];       // 248 bytes max plus a terminator we add
};

// Inquiry Result (event 0x02) is FIELD-MAJOR: all BD_ADDRs, then all PSRMs,
// then reserved, then all CoDs, then all clock offsets.  Returns the number
// of complete responses the parameters actually hold (0 when truncated).
uint8_t hciInquiryResultCount(const uint8_t *params, uint8_t len);
bool    hciParseInquiryResult(const uint8_t *params, uint8_t len, uint8_t idx, HciInquiryResult *out);

// Remote Name Request Complete (event 0x07): status(1) bd(6) name(248).
bool    hciParseRemoteNameComplete(const uint8_t *params, uint8_t len, HciRemoteName *out);

// "11:22:33:44:55:66" into a caller buffer of at least 18 bytes.
void    hciFormatBd(const uint8_t bd[6], char out[18]);
```

`~/Development/M2Radio/hci/HciEvents.cpp`:

```cpp
#include "HciEvents.h"
#include <string.h>

uint8_t hciInquiryResultCount(const uint8_t *params, uint8_t len) {
    if (len < 1) return 0;
    uint8_t n = params[0];
    return (size_t)1 + (size_t)n * 14 <= len ? n : 0;
}

bool hciParseInquiryResult(const uint8_t *params, uint8_t len, uint8_t idx, HciInquiryResult *out) {
    uint8_t n = hciInquiryResultCount(params, len);
    if (idx >= n) return false;
    const uint8_t *bd   = params + 1 + 6 * idx;
    const uint8_t *psrm = params + 1 + 6 * n + idx;
    const uint8_t *cod  = params + 1 + 9 * n + 3 * idx;      // after 6n bd + n psrm + 2n reserved
    const uint8_t *clk  = params + 1 + 12 * n + 2 * idx;
    memcpy(out->bd, bd, 6);
    out->psrm = *psrm;
    out->cod = (uint32_t)cod[0] | ((uint32_t)cod[1] << 8) | ((uint32_t)cod[2] << 16);
    out->clockOffset = (uint16_t)(clk[0] | (clk[1] << 8));
    return true;
}

bool hciParseRemoteNameComplete(const uint8_t *params, uint8_t len, HciRemoteName *out) {
    if (len < 7) return false;
    out->status = params[0];
    memcpy(out->bd, params + 1, 6);
    size_t n = len - 7; if (n > 248) n = 248;
    memcpy(out->name, params + 7, n);
    out->name[n] = 0;
    return true;
}

void hciFormatBd(const uint8_t bd[6], char out[18]) {
    static const char hex[] = "0123456789ABCDEF";
    char *o = out;
    for (int i = 5; i >= 0; i--) {
        *o++ = hex[bd[i] >> 4]; *o++ = hex[bd[i] & 0xF];
        if (i) *o++ = ':';
    }
    *o = 0;
}
```

- [ ] **Step 4: Run the tests**

```bash
~/Development/M2Radio/hci/test/run.sh
```

Expected: all three test binaries report `0 failures`, then `HCI-HOST-TESTS: PASS`.

- [ ] **Step 5: Commit (M2Radio)**

```bash
cd ~/Development/M2Radio && git add hci/HciEvents.h hci/HciEvents.cpp hci/test/hcievents_test.cpp && git commit -m "hci: HciEvents -- Inquiry Result (field-major) and Remote Name parsers

Inquiry Result carries N responses as N arrays per field, not N structs; a
struct-major reader gets the second device's address from the first device's
class bytes.  Pinned by a two-response host fixture.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 8: `M2Radio/hci/HciTransport` + `HciPump` (Arduino side)

**Files:**
- Create: `~/Development/M2Radio/hci/HciTransport.h`, `HciTransport.cpp`
- Create: `~/Development/M2Radio/hci/HciPump.h`, `HciPump.cpp`

These have no host test (they are the platform glue); Task 9's build compiles them and Task 10's gate runs them.

- [ ] **Step 1: `HciTransport`**

`~/Development/M2Radio/hci/HciTransport.h`:

```cpp
// HciTransport -- HciIo over a core HardwareSerialIMXRT port (Serial2 = LPUART2
// = the M.2 socket's BT HCI UART on the MIMXRT1170-EVKB).  Adds a 1 KB RX
// ring on top of the core's 64-byte one: an Extended Inquiry Result event is
// 257 bytes and LPUART2 has no flow control on this board (RTS is the gigabit
// PHY's reset line), so the ring is the only slack there is.
// MIT.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "HciIo.h"

class HardwareSerialIMXRT;

class HciTransport : public HciIo {
public:
    static const size_t RX_EXTRA = 1024;
    explicit HciTransport(HardwareSerialIMXRT &port) : m_port(port) {}
    void begin(uint32_t baud);
    void end();
    size_t   write(const uint8_t *p, size_t n) override;
    int      available() override;
    int      read() override;
    uint32_t nowMs() override;
private:
    HardwareSerialIMXRT &m_port;
    uint8_t m_rxExtra[RX_EXTRA];
};
```

`~/Development/M2Radio/hci/HciTransport.cpp`:

```cpp
#include "HciTransport.h"
#include <Arduino.h>
#include <HardwareSerial.h>

void HciTransport::begin(uint32_t baud) {
    m_port.begin(baud);
    m_port.addMemoryForRead(m_rxExtra, sizeof m_rxExtra);
}
void HciTransport::end() { m_port.end(); }
size_t   HciTransport::write(const uint8_t *p, size_t n) { return m_port.write(p, n); }
int      HciTransport::available() { return m_port.available(); }
int      HciTransport::read()      { return m_port.read(); }
uint32_t HciTransport::nowMs()     { return millis(); }
```

- [ ] **Step 2: `HciPump`**

`~/Development/M2Radio/hci/HciPump.h`:

```cpp
// HciPump -- one bounded Hci::service() per yield(), from a yield-attached
// EventResponder.  The same shape as the Wi-Fi facade's pump
// (WiFiClass::serviceEvent), so the two coexist and every delay() services
// both.  Single instance, like the facade.  MIT.
#pragma once
#include <stdint.h>
#include <Arduino.h>
#include <EventResponder.h>

class Hci;

class HciPump {
public:
    void attach(Hci &hci);
    void detach();
    bool     attached() const { return m_attached; }
    uint32_t passes()   const { return m_passes; }
private:
    static void serviceEvent(EventResponderRef ref);
    static HciPump *s_self;
    EventResponder m_responder;
    Hci     *m_hci = nullptr;
    bool     m_attached = false;
    uint32_t m_passes = 0;
};
```

`~/Development/M2Radio/hci/HciPump.cpp`:

```cpp
#include "HciPump.h"
#include "Hci.h"

HciPump *HciPump::s_self = nullptr;

void HciPump::attach(Hci &hci) {
    if (m_attached) return;
    s_self = this; m_hci = &hci;
    m_responder.attach(serviceEvent);
    m_responder.triggerEvent();
    m_attached = true;
}

void HciPump::detach() {
    if (!m_attached) return;
    // clearEvent() BEFORE detach(): detach leaves _triggered set and a later
    // attach()+triggerEvent() would be a silent no-op (see WiFiClass::setAutoService).
    (void)m_responder.clearEvent();
    m_responder.detach();
    m_attached = false; m_hci = nullptr; s_self = nullptr;
}

void HciPump::serviceEvent(EventResponderRef ref) {
    if (s_self && s_self->m_hci) { s_self->m_hci->service(); s_self->m_passes++; }
    ref.triggerEvent();            // re-queue: one bounded pass per yield(), forever
}
```

- [ ] **Step 3: Commit (M2Radio)**

```bash
cd ~/Development/M2Radio && git add hci/HciTransport.h hci/HciTransport.cpp hci/HciPump.h hci/HciPump.cpp && git commit -m "hci: HciTransport over Serial2 (1 KB RX ring) and HciPump on a yield EventResponder

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 9: `examples/networking/m2_hci_probe` — the B1 + B2 sketch

**Files:**
- Create: `examples/networking/m2_hci_probe/CMakeLists.txt`
- Create: `examples/networking/m2_hci_probe/m2_hci_probe.cpp`

- [ ] **Step 1: CMake**

`examples/networking/m2_hci_probe/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.24)
project(m2_hci_probe)

if(NOT DEFINED TEENSY_VERSION)
    set(TEENSY_VERSION 117 CACHE STRING "")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

# sdio + iw416 for the board preamble and the combo blob download that brings
# the Bluetooth block up; hci for the transport.  lwip/arduino are NOT imported:
# this example never touches the Wi-Fi data path.
import_evkb_library(M2Radio sdio iw416 hci)

# --- IW416 firmware blob (NOT vendored) -------------------------------------
# Same rule as every other m2_* example: NXP ship the blob under
# LA_OPT_NXP_Software_License -- redistributable BINARY, not open source -- so
# it MUST NOT be committed.  Point this at a local MCUXpresso SDK copy:
#
#   cmake -B build -DM2RADIO_IW416_FW=/path/to/fw_bin/inc/IW416/sduartIW416_wlan_bt.bin.inc
#
# The Bluetooth firmware rides the COMBO image (sd-uart: sdio-wlan +
# uart-bt); the Wi-Fi-only sdIW416_wlan.bin would leave the BT block silent.
# Without a blob the example still builds, reports the download as skipped,
# and still runs the HCI sequence -- which is what both QEMU gates want.
set(M2RADIO_IW416_FW "" CACHE FILEPATH "IW416 firmware .bin.inc from an NXP SDK")
if(M2RADIO_IW416_FW AND EXISTS "${M2RADIO_IW416_FW}")
    message(STATUS "IW416 firmware: ${M2RADIO_IW416_FW}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/iw416_fw.cpp"
"#include <stdint.h>\n"
"// Generated at configure time from an NXP SDK copy. Never committed.\n"
"extern const uint8_t iw416_fw[];\n"
"extern const uint32_t iw416_fw_len;\n"
"// .progmem keeps this in FLASH: the imxrt1176 linker folds .rodata* into\n"
"// .data (DTCM), which a 400 KB array would overflow.\n"
"const uint8_t iw416_fw[] __attribute__((section(\".progmem\"), used)) = {\n#include \"${M2RADIO_IW416_FW}\"\n};\n"
"const uint32_t iw416_fw_len = sizeof(iw416_fw);\n")
    set(M2_FW_SRC "${CMAKE_CURRENT_BINARY_DIR}/iw416_fw.cpp")
    add_definitions(-DHAVE_IW416_FW=1)
else()
    message(STATUS "IW416 firmware: not supplied -- download will be skipped")
    set(M2_FW_SRC "")
endif()

teensy_add_executable(m2_hci_probe m2_hci_probe.cpp ${M2_FW_SRC})
teensy_target_link_libraries(m2_hci_probe cores M2Radio)
target_include_directories(m2_hci_probe.elf PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(m2_hci_probe.elf stdc++)
```

- [ ] **Step 2: The sketch**

`examples/networking/m2_hci_probe/m2_hci_probe.cpp`:

```cpp
// m2_hci_probe -- BT-1 of the M.2 Bluetooth programme: does the IW416 answer
// HCI, and what does it say it is?
//
// Sequence: board preamble -> SDIO enumerate -> combo blob download (if
// supplied) -> HCI over Serial2 at 115200:
//   B1  Reset, Read_Local_Version_Information, Read_BD_ADDR, Read_Buffer_Size
//   B2  Inquiry (GIAC, 10.24 s) then Remote_Name_Request per result
// then a 1 Hz heartbeat carrying the transport's counters.
//
// Every value printed is a value RECEIVED; this firmware contains none of the
// numbers its gates assert (the fake controller's manufacturer 0x1234, the
// card's real one).  Every failure is printed BY NAME with the counters, so a
// transcript reads as an accounting rather than a hope.
//
// Spec: docs/superpowers/specs/2026-08-23-m2-bluetooth-a2dp-programme-design.md §4
#include <Arduino.h>
#include <HardwareSerial.h>
#include <string.h>
#include <SdioHost.h>
#include <SdioFunc.h>
#include <Iw416.h>
#include <Hci.h>
#include <HciEvents.h>
#include <HciTransport.h>
#include <HciPump.h>

static SdioHost sdio;
static SdioFunc func(sdio);
static Iw416 iw416(sdio, func);
static HciTransport hciIo(Serial2);
static Hci hci(hciIo);
static HciPump pump;

#if defined(HAVE_IW416_FW)
extern const uint8_t  iw416_fw[];
extern const uint32_t iw416_fw_len;
#endif

static SdioHost::Status s_sdioSt = SdioHost::CMD5_NO_RESPONSE;
static SdioHost::Status s_iwSt   = SdioHost::CMD_TIMEOUT;
static SdioHost::Status s_fwSt   = SdioHost::CMD_TIMEOUT;
static bool       s_card  = false;            // firmware confirmed running on the card
static Hci::Error s_hciSt = Hci::TIMEOUT;     // outcome of the Reset step

// Same spelling as every other m2_* example.
static const char *statusName(SdioHost::Status s) {
    switch (s) {
        case SdioHost::OK:               return "ok";
        case SdioHost::NO_IO_FUNCTION:   return "no-io-function";
        case SdioHost::CMD_TIMEOUT:      return "cmd-timeout";
        case SdioHost::CMD_CRC:          return "cmd-crc";
        case SdioHost::CLOCK_UNSTABLE:   return "clock-unstable";
        case SdioHost::BAD_CIS:          return "bad-cis";
        case SdioHost::CMD5_NO_RESPONSE: return "cmd5-no-response";
        case SdioHost::INIT_CLK_STUCK:   return "init-clk-stuck";
    }
    return "unknown";
}

// --- board preamble -- copied from m2_uap_probe (and WiFi.cpp); keep in step --
// Release SDIO_RST (GPIO_AD_16 = GPIO9.15) then WL_RST/PDn (GPIO_AD_31 =
// GPIO9.30, reaching PDn via the hand-bridged R404), with the 1 s ROM-boot
// wait PDn requires.  Without it the card stays in power-down.
#define M2_SDIO_RST_MUX (*(volatile uint32_t *)0x400E814Cu)   // GPIO_AD_16
#define M2_WL_RST_MUX   (*(volatile uint32_t *)0x400E8188u)   // GPIO_AD_31
#define M2_SDIO_RST_BIT 15
#define M2_WL_RST_BIT   30

static void m2ReleaseWifiReset() {
    M2_SDIO_RST_MUX = 0x10u | 0xAu;                 // SION | ALT10 = GPIO9_IO15
    M2_WL_RST_MUX   = 0x10u | 0xAu;                 // SION | ALT10 = GPIO9_IO30
    GPIO9_GDIR |= (1u << M2_SDIO_RST_BIT) | (1u << M2_WL_RST_BIT);
    GPIO9_DR_CLEAR = (1u << M2_SDIO_RST_BIT) | (1u << M2_WL_RST_BIT);
    delay(10);
    GPIO9_DR_SET = (1u << M2_SDIO_RST_BIT);         // SDIO_RST high
    delay(100);
    GPIO9_DR_SET = (1u << M2_WL_RST_BIT);           // then WL_RST / PDn high
    delay(1000);                                    // PDn exit needs ROM boot time
}

// --- HCI opcodes and event codes (Core 5.2 Vol 4 Part E 7.x) ------------------
static const uint16_t OP_RESET            = 0x0C03;
static const uint16_t OP_READ_LOCAL_VER   = 0x1001;
static const uint16_t OP_READ_BUFFER_SIZE = 0x1005;
static const uint16_t OP_READ_BD_ADDR     = 0x1009;
static const uint16_t OP_INQUIRY          = 0x0401;
static const uint16_t OP_REMOTE_NAME_REQ  = 0x0419;
static const uint8_t  EV_INQUIRY_COMPLETE = 0x01;
static const uint8_t  EV_INQUIRY_RESULT   = 0x02;
static const uint8_t  EV_REMOTE_NAME_DONE = 0x07;

static void printHex8(uint8_t v)   { if (v < 0x10) Serial1.print('0'); Serial1.print(v, HEX); }
static void printHex16(uint16_t v) { printHex8((uint8_t)(v >> 8)); printHex8((uint8_t)v); }
static void printHex24(uint32_t v) { printHex8((uint8_t)(v >> 16)); printHex16((uint16_t)v); }
static void printBd(const uint8_t *bd) { char s[18]; hciFormatBd(bd, s); Serial1.print(s); }

static void printCounters() {
    Serial1.print(" timeouts="); Serial1.print(hci.timeouts());
    Serial1.print(" framing=");  Serial1.print(hci.framing());
    Serial1.print(" starved=");  Serial1.print(hci.starved());
    Serial1.print(" qfull=");    Serial1.print(hci.queueFull());
    Serial1.print(" late=");     Serial1.print(hci.late());
}
static void printFail(const char *what, Hci::Error e, const Hci::Reply &r, const char *alt) {
    Serial1.print(what); Serial1.print("=fail reason=");
    Serial1.print(e == Hci::OK ? alt : Hci::errorName(e));
    Serial1.print(" status=0x"); printHex8(r.status);
    printCounters(); Serial1.println();
}
static void idleMs() { delay(1); }

// --- B2 bookkeeping: filled by the event callback, which the pump runs from
// inside delay() while the probe waits -------------------------------------------
struct Found { HciInquiryResult r; bool named; HciRemoteName name; };
static Found         s_found[8];
static uint8_t       s_foundN = 0;
static volatile bool s_inqDone = false;
static uint8_t       s_inqStatus = 0xFF;
static volatile bool s_nameDone = false;

static void onEvent(void *, uint8_t code, const uint8_t *p, uint8_t len) {
    if (code == EV_INQUIRY_RESULT) {
        uint8_t n = hciInquiryResultCount(p, len);
        for (uint8_t i = 0; i < n && s_foundN < 8; i++) {
            Found &f = s_found[s_foundN];
            if (!hciParseInquiryResult(p, len, i, &f.r)) break;
            f.named = false; s_foundN++;
            Serial1.print("inq: bd="); printBd(f.r.bd);
            Serial1.print(" cod=0x"); printHex24(f.r.cod);
            Serial1.print(" psrm="); Serial1.print(f.r.psrm);
            Serial1.print(" clk=0x"); printHex16(f.r.clockOffset);
            Serial1.println();
        }
        if (n == 0) { Serial1.print("inq: malformed len="); Serial1.println(len); }
    } else if (code == EV_INQUIRY_COMPLETE && len >= 1) {
        s_inqStatus = p[0]; s_inqDone = true;
    } else if (code == EV_REMOTE_NAME_DONE) {
        HciRemoteName nm;
        if (hciParseRemoteNameComplete(p, len, &nm)) {
            for (uint8_t i = 0; i < s_foundN; i++)
                if (memcmp(s_found[i].r.bd, nm.bd, 6) == 0) { s_found[i].name = nm; s_found[i].named = true; }
        }
        s_nameDone = true;
    } else {
        Serial1.print("hci_event: code=0x"); printHex8(code); Serial1.print(" len="); Serial1.println(len);
    }
}

// --- B1: identity ---------------------------------------------------------------
static void probeIdentity() {
    Hci::Reply r;
    Hci::Error e = hci.run(OP_READ_LOCAL_VER, nullptr, 0, &r, 1000, idleMs);
    if (e == Hci::OK && r.len >= 8) {
        // Return params after status: HCI_Version(1) HCI_Revision(2)
        // LMP_Version(1) Manufacturer_Name(2) LMP_Subversion(2)
        Serial1.print("hci_version: hci_ver="); Serial1.print(r.params[0]);
        Serial1.print(" hci_rev=0x");     printHex16((uint16_t)(r.params[1] | (r.params[2] << 8)));
        Serial1.print(" lmp_ver=");       Serial1.print(r.params[3]);
        Serial1.print(" manufacturer=0x"); printHex16((uint16_t)(r.params[4] | (r.params[5] << 8)));
        Serial1.print(" lmp_subver=0x");  printHex16((uint16_t)(r.params[6] | (r.params[7] << 8)));
        Serial1.println();
    } else printFail("hci_version", e, r, "short_reply");

    e = hci.run(OP_READ_BD_ADDR, nullptr, 0, &r, 1000, idleMs);
    if (e == Hci::OK && r.len >= 6) { Serial1.print("bd_addr="); printBd(r.params); Serial1.println(); }
    else printFail("bd_addr", e, r, "short_reply");

    e = hci.run(OP_READ_BUFFER_SIZE, nullptr, 0, &r, 1000, idleMs);
    if (e == Hci::OK && r.len >= 7) {
        // ACL_Data_Packet_Length(2) Synchronous_Data_Packet_Length(1)
        // Total_Num_ACL_Data_Packets(2) Total_Num_Synchronous_Data_Packets(2)
        uint16_t aclLen = (uint16_t)(r.params[0] | (r.params[1] << 8));
        Serial1.print("hci_buffer: acl_len="); Serial1.print(aclLen);
        Serial1.print(" acl_num="); Serial1.print(r.params[3] | (r.params[4] << 8));
        Serial1.print(" sco_len="); Serial1.print(r.params[2]);
        Serial1.print(" sco_num="); Serial1.println(r.params[5] | (r.params[6] << 8));
        hci.setAclMax(aclLen);           // the parser's plausibility bound becomes the card's word
    } else printFail("hci_buffer", e, r, "short_reply");
}

// --- B2: who is in the room -------------------------------------------------------
static void probeInquiry() {
    hci.onEvent(onEvent, nullptr);
    s_foundN = 0; s_inqDone = false; s_inqStatus = 0xFF;
    // LAP = GIAC 0x9E8B33 little-endian, Inquiry_Length 0x08 = 10.24 s, Num_Responses 0 = unlimited
    const uint8_t params[5] = { 0x33, 0x8B, 0x9E, 0x08, 0x00 };
    Hci::Reply r;
    Hci::Error e = hci.run(OP_INQUIRY, params, sizeof params, &r, 1000, idleMs);
    if (e != Hci::OK || !r.statusEvent) { printFail("inquiry", e, r, "not_command_status"); return; }
    Serial1.println("inquiry=started");
    uint32_t t0 = millis();
    while (!s_inqDone && millis() - t0 < 12000) delay(10);     // events arrive via the pump
    Serial1.print("inquiry_complete: status=0x"); printHex8(s_inqDone ? s_inqStatus : 0xFF);
    Serial1.print(" n="); Serial1.print(s_foundN);
    if (!s_inqDone) Serial1.print(" timeout=1");
    Serial1.println();

    for (uint8_t i = 0; i < s_foundN; i++) {
        // Remote_Name_Request: BD_ADDR(6) Page_Scan_Repetition_Mode(1) Reserved(1) Clock_Offset(2, bit 15 = valid)
        uint8_t p[10];
        memcpy(p, s_found[i].r.bd, 6);
        p[6] = s_found[i].r.psrm; p[7] = 0;
        p[8] = (uint8_t)(s_found[i].r.clockOffset & 0xFF);
        p[9] = (uint8_t)((s_found[i].r.clockOffset >> 8) | 0x80);
        s_nameDone = false;
        e = hci.run(OP_REMOTE_NAME_REQ, p, sizeof p, &r, 1000, idleMs);
        t0 = millis();
        while (e == Hci::OK && !s_nameDone && millis() - t0 < 5000) delay(10);
        Serial1.print("inq_name: bd="); printBd(s_found[i].r.bd);
        if (e != Hci::OK)         { Serial1.print(" fail reason="); Serial1.println(Hci::errorName(e)); continue; }
        if (!s_found[i].named)    { Serial1.println(" fail reason=no_name_event"); continue; }
        Serial1.print(" status=0x"); printHex8(s_found[i].name.status);
        Serial1.print(" name=\""); Serial1.print(s_found[i].name.name); Serial1.println("\"");
    }
}

void setup() {
    Serial1.begin(115200);
    delay(50);
    Serial1.println("RT1176 M.2 HCI probe up");

    m2ReleaseWifiReset();
    Serial1.println("m2_wifi_reset=released");
    sdio.useIoVoltage1V8(true);
    s_sdioSt = sdio.begin();
    Serial1.print("sdio_begin="); Serial1.println(statusName(s_sdioSt));
    if (s_sdioSt == SdioHost::OK) {
        s_iwSt = iw416.begin();
        Serial1.print("iw416_begin="); Serial1.print(statusName(s_iwSt));
        Serial1.print(" fw_status=0x"); Serial1.println(iw416.fwStatus(), HEX);
        if (s_iwSt == SdioHost::OK) {
#if defined(HAVE_IW416_FW)
            s_fwSt = iw416.downloadFirmware(iw416_fw, iw416_fw_len);
            Serial1.print("fw_download="); Serial1.println(statusName(s_fwSt));
#else
            s_fwSt = (iw416.fwStatus() == Iw416::FIRMWARE_READY) ? SdioHost::OK : SdioHost::CMD_TIMEOUT;
            Serial1.print("fw_download=skipped (no blob supplied) preboot=");
            Serial1.println(s_fwSt == SdioHost::OK ? 1 : 0);
#endif
        }
    }
    s_card = (s_fwSt == SdioHost::OK);
    Serial1.print("card="); Serial1.println(s_card ? 1 : 0);

    // The HCI sequence runs WHATEVER the SDIO outcome: on silicon the BT block
    // only answers after the combo download (B0), and the card-absent gate
    // wants the timeout path by name; the [hci] gate's fake controller answers
    // regardless of SDIO.  NXP waits 100 ms + up to 260 ms here.
    delay(400);
    hciIo.begin(115200);
    hci.begin();
    pump.attach(hci);
    Serial1.println("serial2=up_115200");

    // Reset: up to 10 attempts, because silicon needs an unknown settle after
    // the download (B0 measures it).  In QEMU the [hci] gate's `-serial
    // unix:...,server` holds the guest until the peer is connected, so there
    // attempts>1 is a driver finding, not a timing one.  attempts= is printed
    // so neither is hidden.
    Hci::Reply r;
    uint8_t attempts = 0;
    for (attempts = 1; attempts <= 10; attempts++) {
        s_hciSt = hci.run(OP_RESET, nullptr, 0, &r, 500, idleMs);
        if (s_hciSt == Hci::OK) break;
    }
    if (s_hciSt == Hci::OK) {
        Serial1.print("hci_reset=ok attempts="); Serial1.print(attempts);
        printCounters(); Serial1.println();
        probeIdentity();
        probeInquiry();
    } else if (s_hciSt == Hci::TIMEOUT) {
        Serial1.print("hci_reset=timeout reason=no_response attempts=10");
        printCounters(); Serial1.println();
    } else {
        Serial1.print("hci_reset=fail reason="); Serial1.print(Hci::errorName(s_hciSt));
        Serial1.print(" attempts=10"); printCounters(); Serial1.println();
    }
    Serial1.println("hci_probe_done");
}

void loop() {
    static uint32_t n = 0;
    // Heartbeat: proves the image is still running after the probe rather than
    // having wedged in it, and carries the transport's accounting.
    Serial1.print("hb card="); Serial1.print(s_card ? 1 : 0);
    Serial1.print(" hci="); Serial1.print(s_hciSt == Hci::OK ? "ok" : Hci::errorName(s_hciSt));
    Serial1.print(" n="); Serial1.print(n++);
    Serial1.print(" pump="); Serial1.print(pump.passes());
    printCounters(); Serial1.println();
    delay(1000);
}
```

- [ ] **Step 3: Build**

```bash
cd examples/networking/m2_hci_probe && cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake 2>&1 | grep -i "m2radio\|hci\|firmware" ; cmake --build build 2>&1 | tail -3
```

Expected: the configure log shows `import_arduino_library(M2Radio … sdio iw416 hci)` and `IW416 firmware: not supplied`; the build ends with `m2_hci_probe.elf`. A `No such file: EventResponder.h` means the core include path is missing from `HciPump.h`'s compile — it is not, the core dir is on `teensy_flags`; check the error text before changing anything.

- [ ] **Step 4: Commit (this repo)**

```bash
git add examples/networking/m2_hci_probe/CMakeLists.txt examples/networking/m2_hci_probe/m2_hci_probe.cpp
git commit -m "m2_hci_probe: B1 identity + B2 inquiry over the new M2Radio hci/ layer

Reset (10 attempts, counted) -> Read_Local_Version -> Read_BD_ADDR ->
Read_Buffer_Size -> Inquiry -> Remote_Name_Request per result, every field
printed as received and every failure named with the transport's counters.
Runs the HCI sequence whatever SDIO said, so the card-absent and the fake-
controller gates share one ELF.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 10: The card-absent gate and its transcript

**Files:**
- Create: `examples/networking/m2_hci_probe/run_qemu.sh`
- Create: `examples/networking/m2_hci_probe/transcript_qemu.txt`

- [ ] **Step 1: Write the gate**

`examples/networking/m2_hci_probe/run_qemu.sh`:

```sh
#!/bin/sh
# run_qemu.sh -- the CARD-ABSENT gate for m2_hci_probe (BT-1).
#
# WHAT THIS PROVES
#   With no second -serial, qemu2's LPUART2 has no chardev: the firmware can
#   transmit and will never receive a byte -- exactly the "nothing answered"
#   case on silicon.  The gate asserts that the probe
#     * reaches its Reset step and times out BY NAME (hci_reset=timeout
#       reason=no_response) after all 10 attempts, with timeouts=10;
#     * prints NOTHING it could only know from a reply (no hci_version,
#       no bd_addr, no inq:) -- the fallback must not invent identity;
#     * reaches hci_probe_done and keeps heartbeating afterwards.
#   The reason code plus the LATER heartbeat are the positive tokens: "no
#   identity printed" is also what a dead image produces.
#
# WHAT THIS DOES NOT PROVE
#   Anything about the IW416.  Every claim about the card lives in
#   transcript_hw_evkb.txt.  The bidirectional transport is gated by
#   run_qemu_hci.sh against a fake controller.
#
# DEMONSTRATED RED (2026-08-XX): <quote the Task 11 Step 6 result here>
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
[ "$(gate_board)" = rt1176 ] || {
    echo "FAIL: this gate is rt1176-only (EVKB_BOARD=$(gate_board)) -- the M.2 socket is on the MIMXRT1170-EVKB"; exit 1; }
ELF="$DIR/$(gate_build_dir)/m2_hci_probe.elf"
OUT=$(gate_capture_path "$DIR" serial.uart)
DBG=$(gate_capture_path "$DIR" serial.dbg)
rm -f "$OUT" "$DBG"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none $(gate_console "$OUT") \
    -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
# The probe spends ~1.1 s in the preamble, the SDIO timeouts, 0.4 s settle and
# 10 x 0.5 s Reset attempts before the first heartbeat.  Wait for the SECOND
# heartbeat: it is the last line this gate parses.
for _ in $(seq 1 120); do
    [ -f "$OUT" ] && grep -q "^hb card=0 hci=no_response n=1 " "$OUT" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"
grep -q "RT1176 M.2 HCI probe up" "$OUT" || { echo "FAIL: banner missing"; exit 1; }
grep -q "^sdio_begin=cmd5-no-response" "$OUT" || {
    echo "FAIL: expected the cmd5-no-response SDIO fallback (a plain SD card ignores CMD5)"; exit 1; }
grep -q "^card=0[[:space:]]*$" "$OUT" || { echo "FAIL: card= line missing or not 0"; exit 1; }
grep -q "^serial2=up_115200[[:space:]]*$" "$OUT" || { echo "FAIL: Serial2 never came up"; exit 1; }
grep -q "^hci_reset=timeout reason=no_response attempts=10 timeouts=10 framing=0 starved=0 qfull=0 late=0[[:space:]]*$" "$OUT" || {
    echo "FAIL: expected the Reset timeout BY NAME with all ten attempts counted"; exit 1; }
# The fallback must not claim what it cannot have read.
for T in "^hci_version" "^bd_addr=" "^hci_buffer" "^inquiry=started" "^inq:" "^inq_name:"; do
    if grep -q "$T" "$OUT"; then echo "FAIL: reported '$T' with nothing on LPUART2"; exit 1; fi
done
grep -q "^hci_probe_done[[:space:]]*$" "$OUT" || { echo "FAIL: probe never completed"; exit 1; }
grep -q "^hb card=0 hci=no_response n=1 " "$OUT" || { echo "FAIL: no heartbeat after the probe"; exit 1; }
echo "PASS: HCI probe reached the no_response fallback cleanly and kept running"
```

- [ ] **Step 2: Run it**

```bash
cd examples/networking/m2_hci_probe && chmod +x run_qemu.sh && ./run_qemu.sh 2>&1 | tail -25
```

Expected: the capture ends with `hci_probe_done`, `hb card=0 hci=no_response n=0 …`, `hb … n=1 …`, then the PASS line. Note the wall time (≈ 10 s); if it exceeds 30 s, look at `build/serial.dbg` for guest errors before touching the gate.

- [ ] **Step 3: Commit the capture as the transcript**

```bash
cp build/serial.uart transcript_qemu.txt && git add run_qemu.sh transcript_qemu.txt && git commit -m "m2_hci_probe: card-absent gate (Reset times out by name, then heartbeats)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 11: `hci_peer.py` and the `[hci]` gate — bidirectional, four phases, demonstrated RED

**Files:**
- Create: `examples/networking/m2_hci_probe/hci_peer.py`
- Create: `examples/networking/m2_hci_probe/run_qemu_hci.sh`
- Create: `examples/networking/m2_hci_probe/transcript_qemu_hci.txt`
- Modify: `examples/networking/m2_hci_probe/run_qemu.sh` (the DEMONSTRATED RED line)

- [ ] **Step 1: The fake controller**

`examples/networking/m2_hci_probe/hci_peer.py`:

```python
#!/usr/bin/env python3
"""Fake HCI controller for the m2_hci_probe [hci] gate.

Speaks H4 over the UNIX socket qemu2 exposes as LPUART2
(-serial unix:PATH,server -- slot 1 = LPUART2; QEMU waits for us before booting the guest).  Every value it returns
is one the firmware cannot invent, so a matching line in the UART capture is
proof the bytes made the round trip.

Phases (argv[1]):
  full        answer everything; inject two inquiry results and their names
  drop-reset  never answer Reset              -> firmware must time out BY NAME
  garbage     3 bytes of 0xFF before the first Reset reply, same burst
                                              -> attempt 1 fails as framing, attempt 2 succeeds
  starve      answer with Num_HCI_Command_Packets=0 -> every later command starves
Exit 0 when the phase's last expected opcode was seen.  Prints PEER-* lines.
"""
import socket, struct, sys, time

MANUFACTURER, HCI_REV, LMP_SUBVER, HCI_VER, LMP_VER = 0x1234, 0xBEEF, 0xCAFE, 0x0B, 0x0B
BD_ADDR = bytes.fromhex("665544332211")          # little-endian on the wire -> prints 11:22:33:44:55:66
ACL_LEN, SCO_LEN, ACL_NUM, SCO_NUM = 1021, 64, 8, 0
DEVICES = [(bytes.fromhex("01EEDDCCBBAA"), 0x240404, b"FAKE-HEADSET-01"),   # prints AA:BB:CC:DD:EE:01
           (bytes.fromhex("02EEDDCCBBAA"), 0x240404, b"FAKE-HEADSET-02")]

OP_RESET, OP_READ_LOCAL_VER, OP_READ_BUFFER_SIZE, OP_READ_BD_ADDR = 0x0C03, 0x1001, 0x1005, 0x1009
OP_INQUIRY, OP_REMOTE_NAME_REQ = 0x0401, 0x0419
LAST_OPCODE = {"full": OP_REMOTE_NAME_REQ, "drop-reset": OP_RESET, "garbage": OP_REMOTE_NAME_REQ, "starve": OP_RESET}

def connect(path):
    deadline = time.time() + 20
    while time.time() < deadline:
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.connect(path); return s
        except OSError:
            time.sleep(0.2)
    print("ERROR: could not connect to %s" % path); sys.exit(2)

def event(code, params):               return bytes([0x04, code, len(params)]) + params
def cmd_complete(opcode, ret, ncmd=1): return event(0x0E, bytes([ncmd]) + struct.pack("<H", opcode) + ret)
def cmd_status(opcode, status=0, n=1): return event(0x0F, bytes([status, n]) + struct.pack("<H", opcode))

class Peer:
    def __init__(self, sock, phase):
        self.s, self.phase, self.buf, self.cmds, self.log = sock, phase, b"", [], []
        self.resets, self.pending = 0, []          # pending: (due, bytes)
    def send(self, b, delay=0.0): self.pending.append((time.time() + delay, b))
    def flush(self):
        now = time.time(); keep = []
        for due, b in self.pending:
            if due <= now: self.s.sendall(b)
            else: keep.append((due, b))
        self.pending = keep
    def handle(self, opcode, params):
        self.cmds.append(opcode)
        ncmd = 0 if self.phase == "starve" else 1
        if opcode == OP_RESET:
            self.resets += 1
            if self.phase == "drop-reset": return
            if self.phase == "garbage" and self.resets == 1:
                self.send(b"\xff\xff\xff" + cmd_complete(OP_RESET, b"\x00")); return
            self.send(cmd_complete(OP_RESET, b"\x00", ncmd))
        elif opcode == OP_READ_LOCAL_VER:
            self.send(cmd_complete(opcode, b"\x00" + bytes([HCI_VER]) + struct.pack("<H", HCI_REV)
                                   + bytes([LMP_VER]) + struct.pack("<HH", MANUFACTURER, LMP_SUBVER)))
        elif opcode == OP_READ_BD_ADDR:
            self.send(cmd_complete(opcode, b"\x00" + BD_ADDR))
        elif opcode == OP_READ_BUFFER_SIZE:
            self.send(cmd_complete(opcode, b"\x00" + struct.pack("<HBHH", ACL_LEN, SCO_LEN, ACL_NUM, SCO_NUM)))
        elif opcode == OP_INQUIRY:
            self.send(cmd_status(opcode))
            n = len(DEVICES)
            # Inquiry Result (7.7.2) is FIELD-MAJOR: all BD_ADDRs, all PSRMs, reserved, all CoDs, all clocks.
            body = (bytes([n]) + b"".join(d[0] for d in DEVICES) + bytes([1]) * n + b"\x00\x00" * n
                    + b"".join(struct.pack("<I", d[1])[:3] for d in DEVICES) + struct.pack("<H", 0x1234) * n)
            self.send(event(0x02, body), 0.2)
            self.send(event(0x01, b"\x00"), 0.5)                     # Inquiry Complete
        elif opcode == OP_REMOTE_NAME_REQ:
            self.send(cmd_status(opcode))
            bd = params[:6]
            for d in DEVICES:
                if d[0] == bd:
                    self.send(event(0x07, b"\x00" + bd + d[2].ljust(248, b"\x00")), 0.1); break
            else:
                self.send(event(0x07, b"\x04" + bd + b"\x00" * 248), 0.1)   # 0x04 = Page Timeout
        else:
            self.log.append("PEER-UNKNOWN-OPCODE 0x%04x" % opcode)
            self.send(cmd_complete(opcode, b"\x01"))                    # 0x01 = Unknown HCI Command
    def feed(self, data):
        self.buf += data
        while self.buf:
            if self.buf[0] != 0x01:                                     # only commands come from a host
                self.log.append("PEER-BAD-TYPE 0x%02x" % self.buf[0]); self.buf = self.buf[1:]; continue
            if len(self.buf) < 4: return
            opcode, plen = struct.unpack("<HB", self.buf[1:4])
            if len(self.buf) < 4 + plen: return
            params, self.buf = self.buf[4:4 + plen], self.buf[4 + plen:]
            self.handle(opcode, params)

if __name__ == "__main__":
    phase, path = sys.argv[1], sys.argv[2]
    if phase not in LAST_OPCODE: print("ERROR: unknown phase %s" % phase); sys.exit(2)
    sock = connect(path); sock.settimeout(0.05)
    print("PEER-CONNECTED phase=%s" % phase)
    peer = Peer(sock, phase)
    deadline, last_rx = time.time() + 45, time.time()
    while time.time() < deadline:
        try:
            d = sock.recv(4096)
            if not d: break
            peer.feed(d); last_rx = time.time()
        except socket.timeout:
            pass
        peer.flush()
        if LAST_OPCODE[phase] in peer.cmds and not peer.pending and time.time() - last_rx > 3.0:
            break
    for l in peer.log: print(l)
    print("PEER-DONE phase=%s cmds=%d resets=%d opcodes=%s"
          % (phase, len(peer.cmds), peer.resets, ",".join("%04x" % c for c in peer.cmds)))
    sys.exit(0 if LAST_OPCODE[phase] in peer.cmds else 1)
```

- [ ] **Step 2: The gate**

`examples/networking/m2_hci_probe/run_qemu_hci.sh`:

```sh
#!/bin/sh
# run_qemu_hci.sh -- the [hci] gate for m2_hci_probe (BT-1): the HCI transport
# is BIDIRECTIONAL, and its failure paths are named.
#
# WHAT THIS PROVES
#   qemu2 binds the second -serial to LPUART2 (hw/arm/fsl-imxrt1170.c:1110)
#   and its LPUART model receives from its chardev, so a UNIX socket puts
#   hci_peer.py -- a fake controller in this directory -- on the card's HCI
#   port with NO change to qemu2.  Four QEMU runs against four peer phases:
#     full        every B1 field and every B2 inquiry line carries the peer's
#                 value (manufacturer 0x1234, bd 11:22:33:44:55:66, two
#                 FAKE-HEADSET-* devices) -- values this firmware cannot invent;
#     drop-reset  no reply: the firmware times out BY NAME after 10 attempts;
#     garbage     3 x 0xFF before the first reply: attempt 1 fails as FRAMING
#                 (not timeout), attempt 2 succeeds after the 50 ms resync;
#     starve      Num_HCI_Command_Packets=0: every later command fails as
#                 ncmd_starved, by name, and the probe still completes.
#   The socket lives in /tmp: macOS caps sun_path at 104 bytes and this repo's
#   path alone can exceed it (the four mon.sock gates need /tmp/ev for that).
#   `server` WITHOUT `nowait`: QEMU holds the guest until the peer connects,
#   so the firmware's first Reset cannot be lost to an empty chardev and every
#   count below is strict -- attempts=2 in [garbage] means the driver, not
#   the timing.
#
# WHAT THIS DOES NOT PROVE
#   Anything about the IW416, or about baud (a chardev has none).  Silicon's
#   answers live in transcript_hw_evkb.txt.
#
# DEMONSTRATED RED (2026-08-XX): <quote the Step 6 results here>
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
[ "$(gate_board)" = rt1176 ] || {
    echo "FAIL: this gate is rt1176-only (EVKB_BOARD=$(gate_board)) -- the M.2 socket is on the MIMXRT1170-EVKB"; exit 1; }
ELF="$DIR/$(gate_build_dir)/m2_hci_probe.elf"

fail() { echo "FAIL: $*"; exit 1; }

# run_phase PHASE WAIT_REGEX -> capture in $OUT, peer output in $RES, peer rc in $PEER_RC
run_phase() {
    PHASE=$1; WAIT=$2
    OUT=$(gate_capture_path "$DIR" "hci_$PHASE.uart")
    DBG=$(gate_capture_path "$DIR" "hci_$PHASE.dbg")
    RES=$(gate_capture_path "$DIR" "hci_$PHASE.peer")
    rm -f "$OUT" "$DBG" "$RES"
    SOCK="/tmp/m2hci_$$_$PHASE.sock"; rm -f "$SOCK"; gate_tmp "$SOCK"
    "$QEMU" $(gate_qemu_machine) -kernel "$ELF" -display none \
        $(gate_console "$OUT") -serial unix:"$SOCK",server \
        -d guest_errors -D "$DBG" &
    P=$!; gate_pid $P
    PEER_RC=0
    python3 "$DIR/hci_peer.py" "$PHASE" "$SOCK" > "$RES" 2>&1 || PEER_RC=$?
    # Wait for the LAST line this phase parses, never the first interesting one.
    for _ in $(seq 1 120); do
        [ -f "$OUT" ] && grep -q "$WAIT" "$OUT" 2>/dev/null && break
        sleep 0.25
    done
    gate_reap $P
    gate_require_capture "$OUT" "phase $PHASE"
    echo "==== captured UART ($PHASE) ===="; cat "$OUT"
    echo "==== peer ($PHASE) ===="; cat "$RES"
    grep -q "RT1176 M.2 HCI probe up" "$OUT" || fail "[$PHASE] banner missing"
    if grep -q "^hci_reset=timeout" "$OUT" && [ "$PHASE" != drop-reset ]; then
        fail "[$PHASE] the card-absent fallback ran -- the peer never reached LPUART2"
    fi
}

# --- full ---------------------------------------------------------------------
run_phase full '^hb card=0 hci=ok n=1 '
grep -q "^hci_reset=ok attempts=" "$OUT" || fail "[full] no hci_reset=ok"
grep -q "^hci_version: hci_ver=11 hci_rev=0xBEEF lmp_ver=11 manufacturer=0x1234 lmp_subver=0xCAFE[[:space:]]*$" "$OUT" \
    || fail "[full] hci_version does not carry the peer's values"
grep -q "^bd_addr=11:22:33:44:55:66[[:space:]]*$" "$OUT" || fail "[full] bd_addr does not carry the peer's value"
grep -q "^hci_buffer: acl_len=1021 acl_num=8 sco_len=64 sco_num=0[[:space:]]*$" "$OUT" || fail "[full] hci_buffer wrong"
grep -q "^inquiry=started[[:space:]]*$" "$OUT" || fail "[full] inquiry did not start"
grep -q "^inq: bd=AA:BB:CC:DD:EE:01 cod=0x240404 psrm=1 clk=0x1234[[:space:]]*$" "$OUT" || fail "[full] inquiry result 1 wrong (field-major parse?)"
grep -q "^inq: bd=AA:BB:CC:DD:EE:02 cod=0x240404 psrm=1 clk=0x1234[[:space:]]*$" "$OUT" || fail "[full] inquiry result 2 wrong (field-major parse?)"
grep -q "^inquiry_complete: status=0x00 n=2[[:space:]]*$" "$OUT" || fail "[full] inquiry_complete wrong"
grep -q '^inq_name: bd=AA:BB:CC:DD:EE:01 status=0x00 name="FAKE-HEADSET-01"$' "$OUT" || fail "[full] remote name 1 wrong"
grep -q '^inq_name: bd=AA:BB:CC:DD:EE:02 status=0x00 name="FAKE-HEADSET-02"$' "$OUT" || fail "[full] remote name 2 wrong"
grep -q "^hci_probe_done[[:space:]]*$" "$OUT" || fail "[full] probe never completed"
grep -q "^hb card=0 hci=ok n=1 pump=[0-9]* timeouts=0 framing=0 starved=0 qfull=0 late=0[[:space:]]*$" "$OUT" \
    || fail "[full] heartbeat counters not all zero"
[ "$PEER_RC" -eq 0 ] || fail "[full] peer exited $PEER_RC"
grep -q "^PEER-DONE phase=full cmds=7 " "$RES" || fail "[full] peer did not see exactly the seven commands (Reset, 3 identity, Inquiry, 2 names)"

# --- drop-reset ---------------------------------------------------------------
run_phase drop-reset '^hb card=0 hci=no_response n=1 '
grep -q "^hci_reset=timeout reason=no_response attempts=10 timeouts=10 framing=0 starved=0 qfull=0 late=0[[:space:]]*$" "$OUT" \
    || fail "[drop-reset] Reset must time out BY NAME after ten counted attempts"
if grep -q "^hci_version" "$OUT"; then fail "[drop-reset] identity printed with no Reset"; fi
grep -q "^hci_probe_done[[:space:]]*$" "$OUT" || fail "[drop-reset] probe never completed"
grep -q "^PEER-DONE phase=drop-reset cmds=10 resets=10 " "$RES" || fail "[drop-reset] peer did not see ten Resets"

# --- garbage ------------------------------------------------------------------
run_phase garbage '^hb card=0 hci=ok n=1 '
grep -q "^hci_reset=ok attempts=2 timeouts=0 framing=1 starved=0 qfull=0 late=0[[:space:]]*$" "$OUT" \
    || fail "[garbage] attempt 1 must fail as FRAMING (not timeout) and attempt 2 must succeed"
grep -q "^hci_version: .*manufacturer=0x1234 " "$OUT" || fail "[garbage] the run did not recover to a full identity"
grep -q '^inq_name: bd=AA:BB:CC:DD:EE:02 status=0x00 name="FAKE-HEADSET-02"$' "$OUT" || fail "[garbage] inquiry did not complete after recovery"
grep -q "^hb card=0 hci=ok n=1 pump=[0-9]* timeouts=0 framing=1 starved=0 qfull=0 late=0[[:space:]]*$" "$OUT" \
    || fail "[garbage] heartbeat counters wrong"
[ "$PEER_RC" -eq 0 ] || fail "[garbage] peer exited $PEER_RC"

# --- starve -------------------------------------------------------------------
run_phase starve '^hb card=0 hci=ok n=1 '
grep -q "^hci_reset=ok attempts=1 " "$OUT" || fail "[starve] Reset itself must succeed"
for W in hci_version bd_addr hci_buffer inquiry; do
    grep -q "^$W=fail reason=ncmd_starved " "$OUT" || fail "[starve] $W must fail as ncmd_starved, by name"
done
grep -q "^hci_probe_done[[:space:]]*$" "$OUT" || fail "[starve] probe never completed"
grep -q "^hb card=0 hci=ok n=1 pump=[0-9]* timeouts=0 framing=0 starved=4 qfull=0 late=0[[:space:]]*$" "$OUT" \
    || fail "[starve] expected starved=4 and nothing else"
grep -q "^PEER-DONE phase=starve cmds=1 " "$RES" || fail "[starve] the firmware must not have SENT anything after Reset"

echo "PASS: HCI transport is bidirectional against the fake controller, and timeout/framing/starvation fail by name"
```

- [ ] **Step 3: Run it**

```bash
cd examples/networking/m2_hci_probe && chmod +x run_qemu_hci.sh hci_peer.py && ./run_qemu_hci.sh 2>&1 | tail -30
```

Expected: four captured-UART blocks, each followed by its peer block, then the PASS line. Typical wall time 45–60 s (four QEMU boots plus the peer's 3 s quiet rule each). Diagnosis table:

| Symptom | Likely cause |
|---|---|
| `[full] the card-absent fallback ran` | the peer never connected: check `build/hci_full.peer` for `ERROR: could not connect`; `ls -la /tmp/m2hci_*` for the socket; `qemu-system-arm -chardev help` lists `socket`. (With `server` and no `nowait` QEMU cannot have booted the guest before the connect, so a fallback capture here means the peer connected and then the bytes did not flow.) |
| `hci_reset=ok attempts=3` in `[garbage]` | `Hci::dispatch()` sent while `m_resync` was set — Task 6 case 4 should have caught it; re-run the host tests |
| `inq:` lines with wrong second address | struct-major parse — Task 7's test should fail too |
| `starved=4` missing, `timeouts` non-zero in `[starve]` | `dispatch()` aged the queued command as TIMEOUT rather than NCMD_STARVED |

- [ ] **Step 4: Save the `full` capture as the transcript**

```bash
cp build/hci_full.uart transcript_qemu_hci.txt
```

- [ ] **Step 5: Demonstrate RED — change what the fake says**

Temporarily edit `hci_peer.py`: `MANUFACTURER, … = 0x1234` → `0x1235`. Run the gate:

```bash
./run_qemu_hci.sh 2>&1 | grep "^FAIL"
```

Expected: `FAIL: [full] hci_version does not carry the peer's values`. Restore `0x1234`.

- [ ] **Step 6: Demonstrate RED — break the driver's opcode match**

Temporarily edit `~/Development/M2Radio/hci/Hci.cpp`, in `onPacket`'s Command Complete branch, change `if (m_inflight && opcode == m_inflightOpcode)` to `if (m_inflight && opcode == (uint16_t)(m_inflightOpcode ^ 1))`. Rebuild the example and run BOTH gates:

```bash
cmake --build build 2>&1 | tail -1 && ./run_qemu.sh 2>&1 | tail -1; ./run_qemu_hci.sh 2>&1 | grep "^FAIL"
```

Expected: the card-absent gate still PASSes (nothing to match), and the `[hci]` gate fails `[full] no hci_reset=ok` (every reply is now "late", Reset times out). That asymmetry is the point: only `[hci]` can see this bug. Restore the line, rebuild, re-run both gates to green.

- [ ] **Step 7: Quote both demonstrations in the gate headers**

Replace the `DEMONSTRATED RED (2026-08-XX): <…>` line in `run_qemu_hci.sh` with the date and two sentences: the manufacturer mutation failed `[full] hci_version …`; the opcode-match mutation failed `[full] no hci_reset=ok` while `run_qemu.sh` stayed green. In `run_qemu.sh`, replace its placeholder line with: the opcode-match mutation left this gate GREEN (it has nothing to match), which is why the `[hci]` variant exists.

- [ ] **Step 8: Commit**

```bash
git add run_qemu.sh run_qemu_hci.sh hci_peer.py transcript_qemu_hci.txt
git commit -m "m2_hci_probe[hci]: bidirectional HCI against a fake controller on LPUART2, four phases

qemu2 binds -serial #2 to LPUART2 and its LPUART model receives from the
chardev, so a UNIX socket plus hci_peer.py gates the transport with no model
change.  Phases: full (every field is the peer's, including two field-major
inquiry results and their names), drop-reset (timeout by name), garbage
(attempt 1 FRAMING, attempt 2 OK after the 50 ms resync), starve (ncmd=0 ->
every later command ncmd_starved).  Demonstrated RED twice; see the header.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 12: Vacuity coverage for the new pair

**Files:**
- Modify: `tools/gate-vacuity.test.sh` (append a case 5 after case 4's `fi`)

- [ ] **Step 1: Add the cases**

Append after the end of case 4 (the `fi` that closes the `uap_src` block), before the final summary/exit lines of the script:

```sh
# --- 5. the m2_hci_probe pair (BT-1) ----------------------------------------
# run_qemu.sh asserts the card-ABSENT fallback (LPUART2 on a null chardev:
# Reset times out BY NAME and the image heartbeats afterwards); run_qemu_hci.sh
# asserts a BIDIRECTIONAL transport against hci_peer.py.  The [hci] gate is
# worthless if it passes the fallback capture -- the two share an ELF, and
# dropping the second -serial is all it takes to get the fallback -- so the
# fallback transcript must FAIL it, by name.  (Under the fake qemu the peer
# cannot connect and exits 2; the gate checks the capture BEFORE the peer's
# exit code, so the named failure is the capture's, as it must be.)
hci_rel="examples/networking/m2_hci_probe"
hci_absent="$EVKB/$hci_rel/transcript_qemu.txt"
if [ ! -f "$hci_absent" ]; then
    echo "SKIP: absent_capture_fails_hci_gate (no transcript)"
else
    run_gate "$hci_rel" "run_qemu_hci.sh" "$hci_absent"; rc=$?
    result=0
    [ "$rc" -ne 0 ] || result=1                                          # must not pass
    echo "$OUT_TEXT" | grep -q "the card-absent fallback ran" || result=1   # and name it
    report "absent_capture_fails_hci_gate" $result

    # A dead QEMU must fail the fallback gate BY NAME, like every other runner.
    run_gate "$hci_rel" "run_qemu.sh"; rc=$?
    result=0
    [ "$rc" -ne 0 ] || result=1
    echo "$OUT_TEXT" | grep -q "FAIL: no UART capture" || result=1
    report "dead_qemu_named_m2_hci_probe" $result

    # Over-correction guard: the committed fallback transcript still passes its own gate.
    run_gate "$hci_rel" "run_qemu.sh" "$hci_absent"; rc=$?
    [ "$rc" -eq 0 ] && result=0 || result=1
    report "green_still_passes_m2_hci_probe" $result

    rm -f "$EVKB/$hci_rel"/build/hci_*.uart "$EVKB/$hci_rel"/build/hci_*.peer \
          "$EVKB/$hci_rel"/build/hci_*.dbg "$EVKB/$hci_rel"/build/serial.uart "$EVKB/$hci_rel"/build/serial.dbg
fi
```

- [ ] **Step 2: Run the whole vacuity suite**

```bash
sh tools/gate-vacuity.test.sh 2>&1 | grep -E "^(PASS|FAIL|SKIP):" 
```

Expected: every pre-existing case `PASS:` as before, plus `PASS: absent_capture_fails_hci_gate`, `PASS: dead_qemu_named_m2_hci_probe`, `PASS: green_still_passes_m2_hci_probe`. The new first case takes ~50 s (the peer's 20 s connect timeout plus the gate's poll ceiling on a capture that never shows the awaited line).

- [ ] **Step 3: Commit**

```bash
git add tools/gate-vacuity.test.sh
git commit -m "gate-vacuity: the m2_hci_probe pair -- absent capture fails [hci] by name

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 13: Sweep re-measure, docs, push, pins

**Files:**
- Modify: `CLAUDE.md` (gate count paragraph)
- Modify: `~/Development/M2Radio/README.md`
- Modify: `evkb.cmake:110,119` (pins)

- [ ] **Step 1: Count, then run the sweep**

```bash
./tools/run-all-qemu-gates.sh -l | tail -3
```

Expected: the listing includes `rt1176:networking/m2_hci_probe` and `rt1176:networking/m2_hci_probe[hci]`, and ends `(121 gate(s))`. Then (from `/tmp/ev` — create the symlink if it is absent, `ln -s "$PWD" /tmp/ev`):

```bash
cd /tmp/ev && ./tools/run-all-qemu-gates.sh 2>&1 | tail -5
```

Expected: `gates: 121 passed`, exit 0 — or `120 passed, 1 failed` with the one failure being `rt1176:dualcore/cm4_audio_test` (re-run it idle before believing it). Any other red is a regression from this work: read the gate NAME.

- [ ] **Step 2: Record the count in `CLAUDE.md`**

Three exact edits plus one paragraph:

1. `The sweep covers **119 gates** — the merge of` → `The sweep covers **121 gates** — the merge of`
2. `That arithmetic is CHECKED against the runner rather than trusted: \`-l\` reports\n119.` → `… reports\n121.`
3. `The target is **119 passed, 0 failed, 0 SKIP**, or\n**118 passed, 1 failed, 0 SKIP** when the nondeterministic dual-core gate is red.` → `**121 passed, 0 failed, 0 SKIP**, or\n**120 passed, 1 failed, 0 SKIP** …`
4. Insert, directly after the sentence ending `\`-l\` reports\n121.` and before `The M.2 line's own chain, kept because each step says what the gate is for:`, this paragraph (fill in the real date and numbers):

```markdown
BT-1 added TWO on the new `networking/m2_hci_probe` — the first Bluetooth gates
in the tree. `run_qemu.sh` asserts the card-ABSENT fallback (LPUART2 on a null
chardev: `HCI_Reset` times out BY NAME after ten counted attempts and the image
heartbeats afterwards), and `[hci]` puts a Python fake controller on LPUART2
through `-serial unix:…,server` (QEMU holds the guest until the peer connects)
— qemu2 binds the second `-serial` to LPUART2 and its LPUART model receives
from the chardev, so it needs NO model change — and runs
four phases: every B1 field and both field-major inquiry results carry the
peer's values; a dropped Reset times out by name; a garbage burst fails attempt
1 as FRAMING and attempt 2 succeeds after the 50 ms resync; `ncmd=0` starves
every later command by name. DEMONSTRATED RED twice (the peer's manufacturer
changed; the driver's opcode match broken — which left the card-absent gate
GREEN, the reason the variant exists). ★ The socket lives in `/tmp`, not the
example directory: the `sun_path` cap that bites the four `mon.sock` gates
would bite it too. 119 before them;

✅ **Measured 2026-08-XX: 121 passed, 0 failed, 0 SKIP** (`gates: 121 passed`,
exit 0), on the BT-1 HCI transport, run via `/tmp/ev`,
`rt1176:dualcore/cm4_audio_test` included and green.
```

- [ ] **Step 3: Document `hci/` in the M2Radio README**

Append to `~/Development/M2Radio/README.md`, before the `## Licence` heading:

```markdown
## `hci/` — the Bluetooth HCI transport (BT-1)

`import_evkb_library(M2Radio hci)` gives a sketch the host side of an HCI link
over `Serial2` (LPUART2 = the M.2 socket's BT UART): H4 framing, a command queue
that honours `Num_HCI_Command_Packets`, Command Complete/Status matching with
timeouts, and callbacks for asynchronous events and ACL data. `sdio`/`iw416`
are still needed to bring the card up — the Bluetooth firmware rides the combo
blob downloaded over SDIO — but `hci/` never compiles the Wi-Fi data path.

```cpp
static HciTransport io(Serial2);  static Hci hci(io);  static HciPump pump;
io.begin(115200); hci.begin(); pump.attach(hci);       // one service() per yield()
Hci::Reply r;
Hci::Error e = hci.run(0x0C03 /*Reset*/, nullptr, 0, &r, 500, [](){ delay(1); });
```

**Every exit is named** — `Hci::errorName()` gives `no_response`, `framing`,
`ncmd_starved`, `queue_full`, `status`, `busy` — and counted, because H4 has
no sync marker and LPUART2 has no flow control on this board: a lost byte
desyncs the stream for good, the parser's fault starts a 50 ms idle resync,
and the command in flight fails as `framing`, not `timeout`. `H4Parser`,
`Hci` and `HciEvents` are pure C++ with host unit tests (`hci/test/run.sh`).

Example: `networking/m2_hci_probe` in the rt1176-evkb repo (card-absent gate,
a `[hci]` gate against `hci_peer.py`, and the silicon transcript).
```

- [ ] **Step 4: Push the two sibling repos and bump the pins**

**Pushing publishes to GitHub — confirm with the user first if that has not already been authorised for this work.** Then:

```bash
git -C ~/Development/M2Radio push origin master && git -C ~/Development/M2Radio rev-parse HEAD
git -C ~/Development/teensy-cores push origin HEAD && git -C ~/Development/teensy-cores rev-parse HEAD
```

Put the two SHAs into `evkb.cmake`: line 110 (`teensy_declare_library(cores …`) gets the cores SHA; line 119 (`teensy_declare_library(M2Radio …`) gets the M2Radio SHA, and extend its trailing comment with `BT-1: hci/ (H4Parser, Hci, HciEvents, HciTransport, HciPump).`

- [ ] **Step 5: Prove the pins fetch ("fresh user" mode)**

```bash
cd examples/networking/m2_hci_probe && rm -rf build-fetch && cmake -B build-fetch -DEVKB_FORCE_FETCH=ON -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake 2>&1 | grep -i "fetch\|M2Radio\|cores" | head -5 && cmake --build build-fetch 2>&1 | tail -1 && rm -rf build-fetch
```

Expected: both libraries fetched at the new SHAs and the ELF links. A `FATAL_ERROR` here means the pin is unreachable (unpushed, or a rewritten history) — and would make both new gates SKIP on a clean machine.

- [ ] **Step 6: Commit**

```bash
git add CLAUDE.md evkb.cmake
git commit -m "docs+pins: BT-1 HCI transport -- sweep 121, M2Radio hci/ and cores addMemoryForRead pinned

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
git -C ~/Development/M2Radio add README.md && git -C ~/Development/M2Radio commit -m "readme: document hci/

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>" && git -C ~/Development/M2Radio push origin master
```

(The README commit lands after the pin; it is docs-only, so the pin need not move for it. Bump it anyway if anything else in M2Radio changes later.)

---

### Task 14: B1 + B2 on silicon (needs the board, and a Bluetooth device in range)

**Files:**
- Create: `examples/networking/m2_hci_probe/transcript_hw_evkb.txt`
- Possibly modify: `docs/superpowers/specs/2026-08-23-m2-bluetooth-a2dp-programme-design.md` (risk rows)

- [ ] **Step 1: Put a discoverable Bluetooth device in range**

Power the bench headphones/speaker and put them in pairing mode (discoverable), or make a phone discoverable (Android: open the Bluetooth settings page; iOS is not discoverable for classic inquiry — use the headphones).

- [ ] **Step 2: Build with the combo blob**

```bash
cd examples/networking/m2_hci_probe && rm -rf build-hw && cmake -B build-hw -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake -DM2RADIO_IW416_FW=$HOME/Development/mcuxsdk-ws/mcuxsdk/components/conn_fwloader/fw_bin/inc/IW416/sduartIW416_wlan_bt.bin.inc && cmake --build build-hw 2>&1 | tail -1
```

- [ ] **Step 3: Flash VCOM-free, attach the console, reset**

```bash
pkill LinkServer; pkill redlinkserv; pkill crt_emu_cm_redlink; sleep 1; LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load examples/networking/m2_hci_probe/build-hw/m2_hci_probe.elf 2>&1 | tail -2
```

Second terminal:

```bash
python3 tools/rt1170-console.py /dev/cu.usbmodem5DQ2DDHVWO5EI3 115200 | tee /tmp/bt1.uart
```

First terminal:

```bash
LinkServer run MIMXRT1176:MIMXRT1170-EVKB examples/networking/m2_hci_probe/build-hw/m2_hci_probe.elf > /dev/null 2>&1 &
```

Read for 60 s (the inquiry alone is 10.24 s), stop the reader before any further LinkServer command.

- [ ] **Step 4: Read the result**

```bash
grep -n "^fw_download=\|^card=\|^hci_reset=\|^hci_version\|^bd_addr\|^hci_buffer\|^inquiry\|^inq\|^hb card=1" /tmp/bt1.uart | head -30
```

What each line settles:
- `hci_reset=ok attempts=N` — N is the settle the card needs after the download (each attempt is 500 ms). If N > 1, that number goes into the transcript as a measurement and into B3's plan as the post-download wait.
- `hci_version: … manufacturer=0x0025` (NXP) or `0x0048` (Marvell) — either is a number this firmware cannot invent. Record `lmp_ver` (0x0B = 5.2) and `lmp_subver` — the latter identifies the BT firmware build.
- `bd_addr=…` — compare with the label on the u-blox card, if it carries one.
- `hci_buffer: acl_len=… acl_num=…` — **B5 sizes its segmentation from this.**
- `inq: …` + `inq_name: … name="…"` — the headphones' real name and address off the air. This is BT-2's `Create_Connection` target: record it.
- `hb card=1 hci=ok … framing=0` at 1 Hz — a non-zero `framing` on silicon at 115200 with nothing else on the bus is a finding (a baud-rate error or a level-shifter glitch), not noise to ignore.

If `hci_reset=timeout`, go back to the B0 transcript (Task 3): if B0's post-download bracket matched and this does not, the difference is the 400 ms settle or the ten-attempt cadence — bracket it with the B0 probe again before changing the driver.

- [ ] **Step 5: Write the transcript**

Create `transcript_hw_evkb.txt` with: date, board/card/bridge state, the build command (blob path), the full capture from `RT1176 M.2 HCI probe up` through the third heartbeat, and a reading that states — with the lines quoted — which manufacturer code came back, the BD_ADDR, the buffer sizes, and what was found in the room. Nothing stated without a line to show for it.

- [ ] **Step 6: Update the spec's risk rows if silicon said so**

In the spec's §4 "Risks", the first two rows (BT-only download path; Marvell vs NXP manufacturer) are now answered. Replace each with a one-line statement of the silicon result and the transcript path.

- [ ] **Step 7: Commit**

```bash
git add examples/networking/m2_hci_probe/transcript_hw_evkb.txt docs/superpowers/specs/2026-08-23-m2-bluetooth-a2dp-programme-design.md
git commit -m "m2_hci_probe: silicon -- the IW416 answers HCI; identity and inquiry off the wire

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

(Amend the subject with the manufacturer code and the attempt count once known.)

---

## After this plan

B3 (baud switch + eDMA RX + loss accounting) gets its own plan, written against Task 14's `attempts=` and `acl_len=` numbers. BT-2 (ACL connect, pairing, L2CAP, SDP) gets its own spec, written against the headphones' address and the `hci_buffer` line.
