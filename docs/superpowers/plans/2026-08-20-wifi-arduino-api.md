# Arduino WiFi Façade Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** An Arduino-style `WiFi` / `WiFiClient` / `WiFiServer` over the M2Radio + lwip NO_SYS stack, so a sketch can do `WiFi.begin(ssid, psk); WiFiClient c; c.connect(ip, 80);` on the MIMXRT1170-EVKB M.2 IW416.

**Architecture:** New `arduino/` subdir in the M2Radio sibling repo (`~/Development/M2Radio`), imported via `import_evkb_library(M2Radio sdio iw416 lwip arduino)`. Clean-room MIT `Client`/`Server`/`IPAddress` base classes copied from the Ethernet sibling. A `WiFiClass` singleton owns bring-up (board preamble → SDIO → firmware → lwip netif → `connectStation` → DHCP) and a yield()-driven service pump (EventResponder). TCP connections live in a fixed 4-slot pool; `WiFiClient` is a refcounted handle; `tcp_arg` only ever points at a pool slot. Two new examples with three QEMU gates; sweep baseline 108 → 111.

**Tech Stack:** ARM GCC 10 bare-metal C++, lwip raw API (`NO_SYS=1`), imxrt1176 core (`EventResponder`, `yield()`), CMake per-example builds, QEMU gates via `tools/gate-lib.sh`, qemu2 IW416 model (`m2-wifi=on`, `fw-preboot=on`) for the `[wifi]` gate.

**Spec:** `docs/superpowers/specs/2026-08-20-wifi-arduino-api-design.md` — read it first; it records the whys.

---

## Execution status (updated as tasks land)

| Task | State | Notes |
|---|---|---|
| 1 Baseline | ✅ | Caught two blockers: M2Radio being edited live, and no build dirs in the worktree. |
| 2 Base classes | ✅ | `9a3ddf8` + `1454ff4`. Both reviews passed; replayed onto upstream `c7d0510`. |
| 3 Skeleton + card-absent gate | ✅ | M2Radio `b766a41`, EVKB `d232551`. Review found the gate header overclaimed ("card-absent" is unprovable — the same 255 appears with the card present and no blob); narrowed to "clean-failure". Added `sdio()` accessor + SSID/PSK length rejection. |
| 4 Full begin() + `[wifi]` gate | ✅ | M2Radio `3cac1bd`, EVKB `709fb3b`. Review found a **critical** silent pump death (`EventResponder::detach()` leaves `_triggered` set, so `setAutoService(false)`→`(true)` never re-arms), single-shot auto-reconnect, and DHCP-timeout limbo. `DriverCmd` RAII guard now covers every command-port call. |
| 5 Guard-interaction re-run | ✅ | No new code. Both assertions verified in situ; both gates green. Subsumed by Task 4's reviews, which traced every guard path. |
| 6–15 | pending | |

### Findings that change later tasks

- **Task 6 must not undo the `DriverCmd` guard.** Adding `m_pool.service()` to `servicePass()` *outside* the `if (m_lwipUp)` block is what makes the missing bring-up guard a live, silicon-only bug. The guard is now in place — keep it.
- **QEMU cannot observe a dead service pump.** The `[wifi]` gate passes green with the pump working and with it dead (measured). Anything that depends on the pump actually running is silicon-only evidence.
- **Two Task-4 fixes are unverifiable in QEMU** — single-shot reconnect and DHCP-timeout limbo both need an association the zero-BSS model cannot provide. They are reasoned, not measured, and must be exercised in Task 14's silicon transcript.
- **`m_status` must not double as control state.** Two bugs in two review
  rounds came from it carrying both diagnosis and "please retry" (single-shot
  reconnect; then `disconnect()` silently failing to cancel auto-reconnect).
  Task 4 now separates them with `m_wantReconnect`. Later tasks that add state
  should keep diagnosis and control apart rather than overloading a field.
- **Silicon must exercise what QEMU cannot.** Task 14's transcript has to cover
  four Task-4 fixes that no gate here can reach, all needing a real
  association: single-shot reconnect, DHCP-timeout limbo, `disconnect()`
  cancelling auto-reconnect, and the reconnect throttle actually spacing
  attempts (it measured attempt *starts*, so a 15-45 s failing attempt left no
  gap at all).
- **The licence audit cannot go green in this worktree** until the examples are built (~99 `MISSING BUILD`). Part 1 (copyleft) and the GATES drift check are clean throughout. Same root cause as the Task 13 sweep question.

---

## Ground rules (apply to every task)

- **Two working trees.** Library code goes in `~/Development/M2Radio` (its own git repo, on `master`); examples/gates/docs go in this repo's worktree (`$EVKB` below = the worktree root, branch `claude/arduino-wifi-m2-link-868770`). Commit each side in its own repo.
- **★ The worktree has NO build directories** (they are gitignored and live in the main checkout at `~/Development/rt1176-evkb-m2-maya-w161`, which is on branch `m2-phase0-serial2` and fully built). Two consequences: (a) every example this plan touches must be **configured fresh in the worktree** — the "never `cmake -B` an existing build dir" rule protects the MAIN checkout's dirs, which carry creds and fw-blob paths; a fresh worktree configure with no `-D`s is exactly what the QEMU gates want (none of them needs creds or a blob). (b) A full sweep in the worktree would SKIP ~98 gates; see Task 13, which resolves where the sweep runs.
- **★ `/tmp/ev` already exists and points at the MAIN checkout.** Never `ln -sf` onto it — that creates a link *inside* the target directory. Use `ln -sfn`, and put it back when done.
- **★ RESOLVED 2026-08-20 (mid-execution): the W16 session finished and pushed.** For Tasks 1–2 this plan ran against an insulated clone because `~/Development/M2Radio` had ~893 lines of uncommitted W16 aggregation work. That work is now committed and pushed (`c7d0510`), the tree is clean, and **Task 2's two commits have been replayed onto it** as `9a3ddf8` + `1454ff4`. The insulation is **retired**: work directly in `~/Development/M2Radio` from Task 3 onward, and use no `-DTEENSY_LIB_ROOT` override.

  Re-check `git -C ~/Development/M2Radio status --short` is clean at the start of each library task. If it goes dirty again, that session has restarted — stop and re-insulate (the clone recipe is in this plan's git history at `939be89`).
- **★ Upstream moved 50 commits during execution** and this branch has merged it (`f657d97`). Three consequences the original plan got wrong: the sweep baseline is **108, not 102** (so this phase targets **111**); the M2Radio pin in `evkb.cmake` is already **`c7d0510`**, not `1e15f0b`; and `m2_rx_demo` now owns **seven** gates (W16 added `[regfallback]`, `[rxaggr]`, `[txaggr]`), so Task 11 must re-run seven, not four.
- Only ADD files under `arduino/`; never touch `sdio/`, `iw416/`, `lwip/`. Existing examples do not import `arduino/`, so they cannot be affected.
- Run gates as `./run_qemu.sh`, never `sh run_qemu.sh`.
- Never `cmake -B` or `rm -rf` an EXISTING example's build dir (creds + fw-blob paths in their caches). The two NEW examples' build dirs are fresh and safe.
- Every gate script: `chmod +x` at creation.
- The board is shared with a soak-testing session: **Task 14 (silicon) requires the user's semaphore first.** Everything else is QEMU/host only.
- `AUDIT` shorthand: `cd $EVKB && LICENSE_AUDIT_EVKB=$(pwd) ./tools/license-audit.sh` — expect final line `LICENSE-AUDIT: PASS`.

## File map

M2Radio (create only):
```
arduino/Client.h  arduino/Server.h  arduino/IPAddress.h  arduino/IPAddress.cpp   (copies, Task 2)
arduino/WiFi.h        arduino/WiFi.cpp          (Tasks 3–5, 8)
arduino/WiFiConnPool.h arduino/WiFiConnPool.cpp (Task 6)
arduino/WiFiClient.h  arduino/WiFiClient.cpp    (Task 7)
arduino/WiFiServer.h  arduino/WiFiServer.cpp    (Task 9)
README.md             (modify, Task 12)
```
EVKB repo:
```
examples/networking/wifi_client_test/{CMakeLists.txt,wifi_client_test.cpp,run_qemu.sh,run_qemu_wifi.sh}   (Tasks 3,4)
examples/networking/wifi_server_test/{CMakeLists.txt,wifi_server_test.cpp,run_qemu.sh,wifi_peer.py}       (Task 10)
tools/license-audit.sh                (GATES entries, Tasks 3,10)
examples/networking/{m2_lwip_test,m2_sdio_probe,m2_throughput_test,m2_rx_demo}/*.cpp  (one comment line each, Task 11)
CLAUDE.md, docs/KNOWN-BROKEN-GATES.md (Task 13)
evkb.cmake                            (pin bump, Task 15)
```

---

### Task 1: Baseline

- [ ] **Step 1: Confirm the audit is green before anything is written**

Run: `AUDIT`
Expected: `LICENSE-AUDIT: PASS`. If not, STOP — the tree is dirty before we started; report to the user.

- [ ] **Step 2: Confirm M2Radio is clean and at the pin**

```bash
git -C ~/Development/M2Radio status --short && git -C ~/Development/M2Radio log --oneline -1
```
Expected: no output from status; log shows `1e15f0b`. If dirty, STOP and report (another session may be mid-work in the library).

### Task 2: Clean-room base classes into M2Radio/arduino/

**Files:** Create `~/Development/M2Radio/arduino/{Client.h,Server.h,IPAddress.h,IPAddress.cpp}`

- [ ] **Step 1: Copy the four files from the Ethernet sibling**

```bash
mkdir -p ~/Development/M2Radio/arduino
cp ~/Development/Ethernet/src/Client.h ~/Development/Ethernet/src/Server.h \
   ~/Development/Ethernet/src/IPAddress.h ~/Development/Ethernet/src/IPAddress.cpp \
   ~/Development/M2Radio/arduino/
```

- [ ] **Step 2: Add a provenance line to each copy**

At the very top of each of the four files (line 1, above the existing comment), insert:

```c
/* Copied verbatim from github.com/newdigate/Ethernet src/ (clean-room MIT,
 * see that repo). Per-library duplication is this tree's pattern (NativeEthernet
 * carries its own copy too); consequence: Ethernet and M2Radio's arduino/
 * cannot be imported into one sketch (duplicate class Client). */
```

- [ ] **Step 3: Verify no LGPL text came along**

```bash
grep -riE "GNU (Lesser|Library|General)" ~/Development/M2Radio/arduino/ || echo CLEAN
```
Expected: `CLEAN`.

- [ ] **Step 4: Commit (M2Radio)**

```bash
cd ~/Development/M2Radio && git add arduino/ && git commit -m "arduino: clean-room MIT Client/Server/IPAddress base classes (copied from newdigate/Ethernet)"
```

### Task 3: wifi_client_test skeleton + card-absent gate (TDD: gate first)

**Files:**
- Create: `$EVKB/examples/networking/wifi_client_test/run_qemu.sh`
- Create: `$EVKB/examples/networking/wifi_client_test/CMakeLists.txt`
- Create: `$EVKB/examples/networking/wifi_client_test/wifi_client_test.cpp`
- Create: `~/Development/M2Radio/arduino/WiFi.h`, `~/Development/M2Radio/arduino/WiFi.cpp`
- Modify: `$EVKB/tools/license-audit.sh` (GATES)

- [ ] **Step 1: Write the failing gate**

`run_qemu.sh` (then `chmod +x`):

```sh
#!/bin/sh
# run_qemu.sh — the CARD-ABSENT gate for wifi_client_test.
#
# WHAT THIS PROVES: with QEMU's default SD *memory* card (which ignores CMD5),
# WiFi.begin() must fail cleanly with WL_NO_SHIELD (255), never claim an IP,
# and leave the sketch's heartbeat running.  This is what a default sweep sees.
# WHAT THIS DOES NOT PROVE: anything about Wi-Fi.  Enumeration + scan live in
# run_qemu_wifi.sh; association/DHCP/TCP live on silicon only
# (transcript_hw_evkb.txt) — the QEMU model deliberately returns zero scan
# results, so no gate anywhere may assert association.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/wifi_client_test.elf"
OUT=$(gate_capture_path "$DIR" serial.uart)
rm -f "$OUT"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none $(gate_console "$OUT") \
    -d guest_errors -D "$(gate_capture_path "$DIR" serial.dbg)" &
P=$!; gate_pid $P
for _ in $(seq 1 60); do
    [ -f "$OUT" ] && grep -q "alive=2" "$OUT" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"
grep -q "RT1176 WiFi client test up" "$OUT" || { echo "FAIL: banner missing"; exit 1; }
grep -q "^wifi_status=255" "$OUT" || { echo "FAIL: expected WL_NO_SHIELD (255) with no card"; exit 1; }
grep -q "^alive=2" "$OUT" || { echo "FAIL: no heartbeat after begin()"; exit 1; }
if grep -q "^wifi_ip=" "$OUT"; then
    echo "FAIL: claimed an IP with no card present"; exit 1
fi
echo "PASS: WL_NO_SHIELD fallback, no IP claimed, heartbeat alive"
```

- [ ] **Step 2: Run it — must fail for want of the ELF**

```bash
cd $EVKB/examples/networking/wifi_client_test && ./run_qemu.sh
```
Expected: FAIL (no `build/wifi_client_test.elf`).

- [ ] **Step 3: Write CMakeLists.txt**

Copy of the m2_lwip_test pattern with `arduino` added to the import:

```cmake
cmake_minimum_required(VERSION 3.24)
project(wifi_client_test)

if(NOT DEFINED TEENSY_VERSION)
    set(TEENSY_VERSION 117 CACHE STRING "")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

import_evkb_library(lwip
    src/include src/core src/core/ipv4 src/netif port port/arch)
# sdio + iw416 + the lwip netif glue + the Arduino facade (WiFi/WiFiClient).
import_evkb_library(M2Radio sdio iw416 lwip arduino)

# --- IW416 firmware blob (NOT vendored) -- same rules as m2_lwip_test -------
set(M2RADIO_IW416_FW "" CACHE FILEPATH "IW416 firmware .bin.inc from an NXP SDK")
if(M2RADIO_IW416_FW AND EXISTS "${M2RADIO_IW416_FW}")
    message(STATUS "IW416 firmware: ${M2RADIO_IW416_FW}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/iw416_fw.cpp"
"#include <stdint.h>\n"
"// Generated at configure time from an NXP SDK copy. Never committed.\n"
"extern const uint8_t iw416_fw[];\n"
"extern const uint32_t iw416_fw_len;\n"
"const uint8_t iw416_fw[] __attribute__((section(\".progmem\"), used)) = {\n#include \"${M2RADIO_IW416_FW}\"\n};\n"
"const uint32_t iw416_fw_len = sizeof(iw416_fw);\n")
    set(M2_FW_SRC "${CMAKE_CURRENT_BINARY_DIR}/iw416_fw.cpp")
    add_definitions(-DHAVE_IW416_FW=1)
else()
    message(STATUS "IW416 firmware: not supplied -- download will be skipped")
    set(M2_FW_SRC "")
endif()

# --- Wi-Fi credentials (NEVER committed) -- same pattern as m2_lwip_test ----
set(M2RADIO_WIFI_SSID "" CACHE STRING "Target AP SSID")
set(M2RADIO_WIFI_PSK  "" CACHE STRING "Target AP WPA2 passphrase -- NEVER committed")
if(M2RADIO_WIFI_SSID AND M2RADIO_WIFI_PSK)
    message(STATUS "Wi-Fi target SSID: ${M2RADIO_WIFI_SSID} (PSK supplied, not shown)")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/wifi_creds.h"
"// Generated at configure time from -DM2RADIO_WIFI_*. NEVER committed.\n"
"#pragma once\n"
"#define M2_WIFI_SSID \"${M2RADIO_WIFI_SSID}\"\n"
"#define M2_WIFI_PSK  \"${M2RADIO_WIFI_PSK}\"\n")
    add_definitions(-DHAVE_WIFI_CREDS=1)
endif()

teensy_add_executable(wifi_client_test wifi_client_test.cpp ${M2_FW_SRC})
teensy_target_link_libraries(wifi_client_test cores M2Radio lwip)
target_include_directories(wifi_client_test.elf PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(wifi_client_test.elf stdc++)
```

- [ ] **Step 4: Write the sketch (echo loop arrives in Task 7)**

`wifi_client_test.cpp`:

```cpp
// Arduino WiFi facade proof: WiFi.begin() + WiFiClient echo against the ESP
// bench oracle (192.168.4.1:4712).
//
// QEMU proof (run_qemu.sh): the CARD-ABSENT path -- WL_NO_SHIELD, no IP, alive.
// QEMU proof (run_qemu_wifi.sh): enumeration + a REAL scan against the IW416
// model, which returns zero BSS by design -> WL_NO_SSID_AVAIL, honestly.
// Association/DHCP/TCP are silicon-only (transcript_hw_evkb.txt).
#include "Arduino.h"
#include "HardwareSerial.h"
#include "WiFi.h"
#include "WiFiClient.h"
#include <string.h>
#include <stdio.h>

#if defined(HAVE_WIFI_CREDS)
#include "wifi_creds.h"          // generated, gitignored -- never committed
#else
// Deliberately nonexistent SSID: NOT a credential.  The [wifi] gate uses this
// to drive a real scan that must honestly find nothing.
#define M2_WIFI_SSID "WIFI-GATE-NO-SUCH-AP"
#define M2_WIFI_PSK  nullptr
#endif

#if defined(HAVE_IW416_FW)
extern const uint8_t  iw416_fw[];
extern const uint32_t iw416_fw_len;
#endif

static uint32_t s_tx = 0, s_ok = 0, s_fail = 0;

void setup() {
    Serial1.begin(115200);
    delay(50);
    Serial1.println("RT1176 WiFi client test up");
#if defined(HAVE_IW416_FW)
    WiFi.setFirmware(iw416_fw, iw416_fw_len);
#endif
    int st = WiFi.begin(M2_WIFI_SSID, M2_WIFI_PSK);
    Serial1.print("wifi_status="); Serial1.println(st);
    if (st == WL_CONNECTED) {
        Serial1.print("wifi_ip=");   Serial1.println(WiFi.localIP());
        Serial1.print("wifi_rssi="); Serial1.println(WiFi.RSSI());
    }
}

void loop() {
    static uint32_t lastBeat = 0, beats = 0;
    if (millis() - lastBeat >= 1000) {
        lastBeat = millis();
        Serial1.print("alive="); Serial1.print(++beats);
        Serial1.print(" tcp="); Serial1.print(s_tx);
        Serial1.print('/');     Serial1.print(s_ok);
        Serial1.print('/');     Serial1.println(s_fail);
    }
}
```

- [ ] **Step 5: Write the minimal WiFi.h**

`~/Development/M2Radio/arduino/WiFi.h`:

```cpp
/* WiFi.h - Arduino-style station facade over the M2Radio IW416 + lwip stack.
 *
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Design doc: rt1176-evkb docs/superpowers/specs/2026-08-20-wifi-arduino-api-design.md
 * Two rules a caller must not defeat:
 *   - IEEE power save stays ON (the W10 idle-RX-death workaround).  There is
 *     deliberately no PS switch here; if you must, WiFi.radio().setIeeePs()
 *     puts you next to the erratum comment in the driver.
 *   - The link must be serviced continuously.  By default a yield()-driven
 *     EventResponder pump does it (every loop() pass and every delay() ms);
 *     WiFi.setAutoService(false) hands the cadence to your own WiFi.loop().
 */
#pragma once
#include <stdint.h>
#include "IPAddress.h"
#include "EventResponder.h"
#include "SdioHost.h"
#include "SdioFunc.h"
#include "Iw416.h"
#include "lwip/netif.h"

typedef enum {
    WL_IDLE_STATUS     = 0,
    WL_NO_SSID_AVAIL   = 1,
    WL_SCAN_COMPLETED  = 2,
    WL_CONNECTED       = 3,   // associated AND DHCP supplied an address
    WL_CONNECT_FAILED  = 4,
    WL_CONNECTION_LOST = 5,
    WL_DISCONNECTED    = 6,
    WL_NO_SHIELD       = 255, // no card / no function 1 / no firmware
} wl_status_t;

class WiFiClass {
public:
    WiFiClass() : m_func(m_sdio), m_iw416(m_sdio, m_func) {}

    // The IW416 firmware blob is NXP-licensed and never vendored; supply it
    // before begin() (examples wire the HAVE_IW416_FW configure-time pattern).
    void setFirmware(const uint8_t *fw, uint32_t len) { m_fw = fw; m_fwLen = len; }

    // Full bring-up; returns the resulting status() (WL_CONNECTED only when
    // localIP() is real).  Blocking, but pumps the stack while it waits.
    int begin(const char *ssid, const char *psk = nullptr,
              uint32_t timeoutMs = 30000, bool doBoardPreamble = true);
    void disconnect();

    uint8_t   status();
    IPAddress localIP()     { return ipFromNetif(0); }
    IPAddress subnetMask()  { return ipFromNetif(1); }
    IPAddress gatewayIP()   { return ipFromNetif(2); }
    IPAddress dnsServerIP();
    uint8_t  *macAddress(uint8_t *mac);
    const char *SSID() const { return m_ssid; }
    // SCAN-TIME RSSI of the AP we associated to -- the driver has no live-RSSI
    // command.  0 if never connected.
    int32_t   RSSI();
    int       hostByName(const char *host, IPAddress &out, uint32_t timeoutMs = 5000);

    // One bounded service pass; safe to call anywhere, any rate.
    void loop();
    void setAutoService(bool on);
    void setAutoReconnect(bool on) { m_autoReconnect = on; }

    // Escape hatches -- the facade is a floor, not a ceiling.
    Iw416        &radio() { return m_iw416; }
    struct netif *netif() { return &m_netif; }

    // Internal (WiFiClient/WiFiServer/pool); public for want of friends.
    bool lwipUp() const { return m_lwipUp; }
    bool linkUp() const { return m_linkUp; }
    void servicePass();
    bool pumpUntil(bool (*cond)(void *), void *ctx, uint32_t timeoutMs);

private:
    static void serviceEvent(EventResponderRef ref);
    bool bringUpCard(bool doBoardPreamble);
    int  connectAndDhcp(uint32_t timeoutMs);
    void maybeReconnect();
    void linkLost();
    IPAddress ipFromNetif(int which);

    SdioHost m_sdio;
    SdioFunc m_func;
    Iw416    m_iw416;
    struct netif m_netif;
    EventResponder m_responder;
    const uint8_t *m_fw = nullptr;
    uint32_t m_fwLen = 0;
    char m_ssid[33] = {0};
    char m_psk[64]  = {0};
    uint8_t m_status = WL_IDLE_STATUS;
    bool m_cardUp = false, m_lwipUp = false, m_linkUp = false;
    bool m_autoService = true, m_autoServiceAttached = false;
    bool m_autoReconnect = false;
    volatile bool m_inService = false;
    volatile bool m_inDriverCmd = false;   // serviceLink during a command-port
                                           // exchange steals the reply (Iw416.h)
    uint32_t m_lastReconnectMs = 0;
};

extern WiFiClass WiFi;
```

- [ ] **Step 6: Write the minimal WiFi.cpp (card-detect only; full begin lands in Task 4)**

`~/Development/M2Radio/arduino/WiFi.cpp`:

```cpp
/* WiFi.cpp - see WiFi.h.  MIT, (c) 2026 Nicholas Newdigate. */
#include "WiFi.h"
#include "Arduino.h"
#include "Iw416Netif.h"
#include "lwip/init.h"
#include "lwip/timeouts.h"
#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "netif/ethernet.h"
#include <string.h>

WiFiClass WiFi;

// Iw416Netif.cpp externs this; with the facade owning netif_add, the facade
// owns the definition too (sketches define nothing).
extern "C" { unsigned char g_mac[6] = {0}; }

// --- M.2 board bring-up preamble (moved here from the examples) -------------
// Release SDIO_RST (GPIO_AD_16 = GPIO9.15) then WL_RST/PDn (GPIO_AD_31 =
// GPIO9.30, reaching PDn via the hand-bridged R404), with the 1 s ROM-boot
// wait PDn requires, then the caller switches the SDIO pads to 1.8 V.
// Without this the card either stays in full power-down or is left in the
// PREVIOUS image's state and never answers CMD5 -- measured on silicon in W9:
// m2_lwip_test fell to the fallback path until the preamble was added.  An
// example without it is green in QEMU (no card either way) and dead on
// silicon, which is why it now lives in the library, on by default.
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

bool WiFiClass::bringUpCard(bool doBoardPreamble) {
    if (m_cardUp) return true;
    if (doBoardPreamble) m2ReleaseWifiReset();
    m_sdio.useIoVoltage1V8(true);
    if (m_sdio.begin() != SdioHost::OK) return false;
    if (m_iw416.begin() != SdioHost::OK) return false;
    if (m_iw416.fwStatus() == Iw416::FIRMWARE_READY) {
        // Already running: QEMU's fw-preboot model, or a warm card.
    } else if (m_fw != nullptr) {
        if (m_iw416.downloadFirmware(m_fw, m_fwLen) != SdioHost::OK) return false;
    } else {
        return false;                               // no firmware, none supplied
    }
    (void)m_iw416.refreshIoPort();
    delay(50);
    (void)m_iw416.enableHostInt();
    uint32_t fwRel = 0; uint16_t hwVer = 0;
    if (m_iw416.getHwSpec(g_mac, &fwRel, &hwVer) != SdioHost::OK) return false;
    (void)m_iw416.reconfigureTxBuffers(2048);
    (void)m_iw416.macControl(Iw416::MAC_RX_ON | Iw416::MAC_TX_ON |
                             Iw416::MAC_ETHERNETII | Iw416::MAC_RTS_CTS);
    (void)m_iw416.set11nCfg();
    (void)m_iw416.amsduAggrCtrl();
    m_cardUp = true;
    return true;
}

int WiFiClass::begin(const char *ssid, const char *psk,
                     uint32_t timeoutMs, bool doBoardPreamble) {
    (void)timeoutMs;
    strncpy(m_ssid, ssid ? ssid : "", sizeof(m_ssid) - 1);
    strncpy(m_psk,  psk  ? psk  : "", sizeof(m_psk)  - 1);
    if (!bringUpCard(doBoardPreamble)) { m_status = WL_NO_SHIELD; return m_status; }
    m_status = WL_IDLE_STATUS;          // Task 4 replaces this with the
    return m_status;                    // lwip + connectStation + DHCP path
}

void WiFiClass::disconnect() {}
uint8_t WiFiClass::status() { return m_status; }
IPAddress WiFiClass::ipFromNetif(int) { return IPAddress(); }
IPAddress WiFiClass::dnsServerIP() { return IPAddress(); }
uint8_t *WiFiClass::macAddress(uint8_t *mac) { memcpy(mac, g_mac, 6); return mac; }
int32_t WiFiClass::RSSI() { return 0; }
int WiFiClass::hostByName(const char *, IPAddress &, uint32_t) { return 0; }
void WiFiClass::loop() {}
void WiFiClass::setAutoService(bool on) { m_autoService = on; }
void WiFiClass::servicePass() {}
bool WiFiClass::pumpUntil(bool (*)(void *), void *, uint32_t) { return false; }
void WiFiClass::serviceEvent(EventResponderRef) {}
int WiFiClass::connectAndDhcp(uint32_t) { return WL_IDLE_STATUS; }
void WiFiClass::maybeReconnect() {}
void WiFiClass::linkLost() {}
```

Also create a placeholder include so the sketch's `#include "WiFiClient.h"` resolves (full class in Task 7) — `~/Development/M2Radio/arduino/WiFiClient.h`:

```cpp
/* WiFiClient.h - placeholder until the pool lands; see WiFi.h header. MIT. */
#pragma once
```

- [ ] **Step 7: Build**

```bash
cd $EVKB/examples/networking/wifi_client_test
cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake
cmake --build build
```
Expected: `wifi_client_test.elf` produced. Fix compile errors before moving on.

- [ ] **Step 8: Run the gate — PASS**

```bash
./run_qemu.sh
```
Expected: `PASS: WL_NO_SHIELD fallback, no IP claimed, heartbeat alive`.

- [ ] **Step 9: Add the GATES entry and re-run the audit**

In `$EVKB/tools/license-audit.sh`, in the `GATES=` list, after the line
`examples/networking/native_ethernet_test:native_ethernet_test \`, insert:

```
examples/networking/wifi_client_test:wifi_client_test \
```

Run: `AUDIT` → `LICENSE-AUDIT: PASS` (the drift check would have failed the audit without the entry — that is the test).

- [ ] **Step 10: Commit both repos**

```bash
cd ~/Development/M2Radio && git add arduino/ && git commit -m "arduino: WiFiClass skeleton -- card bring-up incl. the M.2 board preamble"
cd $EVKB && git add examples/networking/wifi_client_test tools/license-audit.sh && git commit -m "networking: wifi_client_test skeleton + card-absent gate (Arduino WiFi facade)"
```

### Task 4: Full begin() + the [wifi] enumeration/scan gate (TDD: gate first)

**Files:**
- Create: `$EVKB/examples/networking/wifi_client_test/run_qemu_wifi.sh`
- Modify: `~/Development/M2Radio/arduino/WiFi.cpp` (replace the Task-3 stubs)

- [ ] **Step 1: Write the failing gate**

`run_qemu_wifi.sh` (then `chmod +x`):

```sh
#!/bin/sh
# run_qemu_wifi.sh — the ENUMERATION + SCAN gate for wifi_client_test.
#
# WHAT THIS PROVES
#   Against the qemu2 IW416 card model (m2-wifi=on, fw-preboot=on so no NXP
#   blob is needed), WiFi.begin() walks real SDIO enumeration, real fn1 init,
#   GET_HW_SPEC, and issues a REAL 802_11_SCAN -- and when that scan returns
#   zero BSSes (the model returns none BY DESIGN: hw/sd/iw416-sdio.c refuses
#   to invent an AP), begin() reports WL_NO_SSID_AVAIL (1) honestly instead
#   of wedging or claiming a link.  The heartbeat after begin() proves the
#   blocking call returned and the image stays alive.
#
# WHAT THIS DOES *NOT* PROVE — read before trusting a green
#   * NO association, NO 4-way handshake, NO DHCP, NO TCP, NO WiFiClient/
#     WiFiServer data path.  Zero scan results means the connect path ends at
#     the scan; everything past it is silicon-only (transcript_hw_evkb.txt).
#   * NO firmware download (fw-preboot skips it; m2_lwip_test's silicon
#     transcript covers the download).
#   * The model is not silicon; see run_qemu_wifi.sh in m2_sdio_probe for the
#     full statement of where it could be lying.
# Requires qemu2 >= 2ed9314631 (gitlab.com/Newdigate/qemu-rt1170).  On stock
# QEMU this gate goes RED, not SKIP — documented in CLAUDE.md alongside the
# other model-dependent gates.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
[ "$(gate_board)" = rt1176 ] || {
    echo "FAIL: this gate is rt1176-only (EVKB_BOARD=$(gate_board)) — the M.2 socket and the"
    echo "      iw416-sdio model both live on the MIMXRT1170-EVKB machine"; exit 1; }
ELF="$DIR/$(gate_build_dir)/wifi_client_test.elf"
# Distinct basenames from run_qemu.sh's serial.uart ON PURPOSE (both gates run
# from this directory; each starts with rm -f).
OUT=$(gate_capture_path "$DIR" wifi.uart)
DBG=$(gate_capture_path "$DIR" wifi.dbg)
rm -f "$OUT" "$DBG"
"$QEMU" $(gate_qemu_machine) -machine m2-wifi=on \
    -global iw416-sdio.fw-preboot=on \
    -kernel "$ELF" -display none $(gate_console "$OUT") \
    -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
for _ in $(seq 1 240); do
    [ -f "$OUT" ] && grep -q "alive=3" "$OUT" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"
grep -q "RT1176 WiFi client test up" "$OUT" || { echo "FAIL: banner missing"; exit 1; }
# THE NEGATIVE THAT MAKES THE REST MEAN SOMETHING: status 255 here would mean
# the model was not attached and we are silently re-running the absent gate.
if grep -q "^wifi_status=255" "$OUT"; then
    echo "FAIL: WL_NO_SHIELD with the model attached — m2-wifi=on did not take"; exit 1
fi
grep -q "^wifi_status=1" "$OUT" || {
    echo "FAIL: expected WL_NO_SSID_AVAIL (1) from a real scan finding zero BSS"; exit 1; }
grep -q "^alive=3" "$OUT" || { echo "FAIL: no heartbeat after the failed connect"; exit 1; }
if grep -q "^wifi_ip=" "$OUT"; then
    echo "FAIL: claimed an IP the model cannot have granted"; exit 1
fi
echo "PASS: enumeration + real scan -> honest WL_NO_SSID_AVAIL; image alive"
```

- [ ] **Step 2: Run it — must fail against the Task-3 stub**

```bash
cd $EVKB/examples/networking/wifi_client_test && ./run_qemu_wifi.sh
```
Expected: FAIL — the stub `begin()` returns `WL_IDLE_STATUS` (0), so `wifi_status=1` is missing. This is the demonstrated-red for the facade's connect path.

- [ ] **Step 3: Implement the full begin() path in WiFi.cpp**

Replace the stub block (everything from `int WiFiClass::begin(` to the end of the file) with the following.

★ **Task 3's review added things to this region that must SURVIVE**: the
SSID/PSK length-rejection guard at the top of `begin()` (reproduced below --
do not drop it), and the `sdio()` accessor in `WiFi.h` (untouched by this
step). Diff your replacement against the current file rather than pasting
blind.

```cpp
int WiFiClass::begin(const char *ssid, const char *psk,
                     uint32_t timeoutMs, bool doBoardPreamble) {
    // ★ KEEP the length-rejection guard added by Task 3's review -- replacing
    // this function wholesale would silently revert it.  Reject rather than
    // truncate: a shortened SSID returns as "SSID not found" and a shortened
    // passphrase as a wrong key, both maximally confusing on a bench.
    if (ssid && strlen(ssid) > 32) { m_status = WL_CONNECT_FAILED; return m_status; }
    if (psk  && strlen(psk)  > 63) { m_status = WL_CONNECT_FAILED; return m_status; }
    strncpy(m_ssid, ssid ? ssid : "", sizeof(m_ssid) - 1);
    strncpy(m_psk,  psk  ? psk  : "", sizeof(m_psk)  - 1);
    if (m_linkUp) disconnect();
    if (!bringUpCard(doBoardPreamble)) { m_status = WL_NO_SHIELD; return m_status; }
    if (!m_lwipUp) {
        lwip_init();
        netif_add(&m_netif, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4,
                  &m_iw416, iw416NetifInit, ethernet_input);
        netif_set_default(&m_netif);
        netif_set_up(&m_netif);
        m_lwipUp = true;
    }
    int st = connectAndDhcp(timeoutMs);
    m_status = (uint8_t)st;
    if (m_autoService && !m_autoServiceAttached) {
        m_responder.attach(serviceEvent);
        m_responder.triggerEvent();
        m_autoServiceAttached = true;
    }
    return m_status;
}

static bool dhcpCond(void *nif) {
    return dhcp_supplied_address((struct netif *)nif) != 0;
}

int WiFiClass::connectAndDhcp(uint32_t timeoutMs) {
    uint32_t t0 = millis();
    m_inDriverCmd = true;
    SdioHost::Status c = m_iw416.connectStation(
        m_ssid, m_psk[0] ? m_psk : nullptr);
    m_inDriverCmd = false;
    if (c == SdioHost::BAD_CIS) return WL_NO_SSID_AVAIL;   // scan ran, SSID absent
    if (c != SdioHost::OK)      return WL_CONNECT_FAILED;  // assoc/handshake/bus
    m_linkUp = true;
    netif_set_link_up(&m_netif);
    dhcp_start(&m_netif);
    uint32_t spent = millis() - t0;
    uint32_t left  = (spent < timeoutMs) ? timeoutMs - spent : 1;
    if (!pumpUntil(dhcpCond, &m_netif, left)) {
        // Associated but no lease: report the failure rather than a half-truth.
        dhcp_stop(&m_netif);
        return WL_CONNECT_FAILED;
    }
    return WL_CONNECTED;
}

void WiFiClass::disconnect() {
    if (m_linkUp) {
        m_inDriverCmd = true;
        (void)m_iw416.deauthenticate(m_iw416.connectedAp().bssid);
        m_inDriverCmd = false;
        dhcp_stop(&m_netif);
        netif_set_link_down(&m_netif);
        m_linkUp = false;
    }
    m_status = WL_DISCONNECTED;
}

uint8_t WiFiClass::status() {
    maybeReconnect();
    return m_status;
}

IPAddress WiFiClass::ipFromNetif(int which) {
    if (!m_lwipUp) return IPAddress();
    const ip4_addr_t *a = (which == 1) ? netif_ip4_netmask(&m_netif)
                        : (which == 2) ? netif_ip4_gw(&m_netif)
                        : netif_ip4_addr(&m_netif);
    return IPAddress(ip4_addr_get_u32(a));
}

IPAddress WiFiClass::dnsServerIP() {
    const ip_addr_t *d = dns_getserver(0);
    return IPAddress(ip4_addr_get_u32(ip_2_ip4(d)));
}

uint8_t *WiFiClass::macAddress(uint8_t *mac) { memcpy(mac, g_mac, 6); return mac; }

int32_t WiFiClass::RSSI() {
    if (!m_linkUp) return 0;
    return -(int32_t)m_iw416.connectedAp().rssi;   // dBm = -raw (Iw416.h)
}

// --- the service pump --------------------------------------------------------
void WiFiClass::servicePass() {
    if (m_inService || m_inDriverCmd) return;   // both guards load-bearing:
    m_inService = true;                         // see WiFi.h + Iw416.h
    if (m_lwipUp) {
        if (m_linkUp && !iw416NetifPoll(&m_netif)) linkLost();
        sys_check_timeouts();
    }
    m_inService = false;
}

void WiFiClass::linkLost() {
    m_linkUp = false;
    dhcp_stop(&m_netif);
    netif_set_link_down(&m_netif);
    m_status = WL_CONNECTION_LOST;
    // Pool teardown arrives with the pool (Task 6).
}

bool WiFiClass::pumpUntil(bool (*cond)(void *), void *ctx, uint32_t timeoutMs) {
    uint32_t t0 = millis();
    while (!cond(ctx)) {
        servicePass();
        if (millis() - t0 >= timeoutMs) return false;
        delay(1);        // delay() yields -> auto-service also runs; harmless
    }
    return true;
}

void WiFiClass::serviceEvent(EventResponderRef ref) {
    WiFi.servicePass();
    ref.triggerEvent();      // re-queue: one bounded pass per yield(), forever
}

void WiFiClass::loop() {
    servicePass();
    maybeReconnect();        // sketch-called path only: the yield pump calls
}                            // servicePass() directly and can never scan

void WiFiClass::setAutoService(bool on) {
    m_autoService = on;
    if (!on && m_autoServiceAttached) { m_responder.detach(); m_autoServiceAttached = false; }
    if (on && !m_autoServiceAttached && m_lwipUp) {
        m_responder.attach(serviceEvent);
        m_responder.triggerEvent();
        m_autoServiceAttached = true;
    }
}

void WiFiClass::maybeReconnect() {
    if (!m_autoReconnect || m_linkUp || !m_cardUp || !m_lwipUp) return;
    if (m_status != WL_CONNECTION_LOST) return;
    if (millis() - m_lastReconnectMs < 5000) return;   // scan storms are 15 s
    m_lastReconnectMs = millis();
    m_status = (uint8_t)connectAndDhcp(30000);
}

int WiFiClass::hostByName(const char *, IPAddress &, uint32_t) { return 0; }  // Task 8
```

- [ ] **Step 4: Rebuild and run BOTH gates**

```bash
cd $EVKB/examples/networking/wifi_client_test && cmake --build build && ./run_qemu_wifi.sh && ./run_qemu.sh
```
Expected: both PASS. If `[wifi]` times out instead: check `wifi.dbg`, and confirm the local qemu2 binary is at `2ed9314631` (`git -C ~/Development/qemu2 log --oneline -1`).

- [ ] **Step 5: Commit both repos**

```bash
cd ~/Development/M2Radio && git add arduino/ && git commit -m "arduino: full WiFi.begin -- lwip netif, connectStation, DHCP pump, yield-driven auto-service"
cd $EVKB && git add examples/networking/wifi_client_test && git commit -m "networking: wifi_client_test[wifi] gate -- real enumeration+scan, honest WL_NO_SSID_AVAIL"
```

### Task 5: Guard-interaction sanity re-run

The `m_inDriverCmd` guard is the load-bearing safety (spec §3). No new code — verify the two orderings that could deadlock or starve:

- [ ] **Step 1: Re-read the interaction**

Confirm in `WiFi.cpp`: (a) `connectAndDhcp` sets `m_inDriverCmd` around `connectStation` — during which `connectStation`'s internal `delay()`s yield into `serviceEvent` → `servicePass` returns immediately on the guard; (b) `pumpUntil`'s `delay(1)` yields into `serviceEvent` → `servicePass` returns immediately on `m_inService` only if currently inside a pass (it is not — `pumpUntil` calls `servicePass()` synchronously, which completes before `delay`). State both in one comment above `servicePass()` if not already clear from the Task-4 text.

- [ ] **Step 2: Both gates again (unchanged code path, cheap insurance)**

```bash
cd $EVKB/examples/networking/wifi_client_test && ./run_qemu.sh && ./run_qemu_wifi.sh
```
Expected: both PASS.

### Task 6: The connection pool

**Files:** Create `~/Development/M2Radio/arduino/WiFiConnPool.h`, `~/Development/M2Radio/arduino/WiFiConnPool.cpp`; modify `WiFi.cpp` (`linkLost` calls `WiFiPool::abortAll`).

- [ ] **Step 1: Write WiFiConnPool.h**

```cpp
/* WiFiConnPool.h - fixed pool of TCP connection slots for the WiFi facade.
 *
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * THE ownership rule that makes the raw-API discipline structural: lwip's
 * tcp_arg points at a pool slot, NEVER at a WiFiClient.  WiFiClient is a
 * refcounted handle; destructing every handle cannot dangle anything lwip
 * holds.  4 slots against MEMP_NUM_TCP_PCB=5 -- the spare is TIME_WAIT
 * headroom (listeners draw from MEMP_NUM_TCP_PCB_LISTEN).
 */
#pragma once
#include <stdint.h>
#include "lwip/tcp.h"

static const uint8_t WIFI_MAX_CONNS = 4;

struct WiFiConn {
    enum State : uint8_t {
        FREE = 0,
        CONNECTING,      // client connect in flight
        ESTABLISHED,
        PEER_CLOSED,     // peer FIN'd or errored; rx chain still readable.
    };                   // pcb==nullptr => lwip side already gone.
    State    state = FREE;
    struct tcp_pcb *pcb = nullptr;
    struct pbuf *rxHead = nullptr;   // unconsumed RX chain (we own it)
    uint16_t rxOff = 0;              // read offset into rxHead's first pbuf
    uint8_t  refs = 0;               // WiFiClient handles attached
    bool     claimed = false;        // a sketch-visible handle ever existed
    uint16_t serverPort = 0;         // owning WiFiServer port; 0 = client conn
    uint32_t lastActivityMs = 0;
    volatile bool connectDone = false;
    volatile bool connectOk   = false;
};

namespace WiFiPool {
    WiFiConn *slot(uint8_t i);           // 0..WIFI_MAX_CONNS-1
    WiFiConn *alloc();                   // FREE slot or nullptr
    // alloc(); when full, evict the least-recently-active accepted-but-never-
    // claimed slot (refs==0 by definition -- the sketch never saw it).
    // Claimed connections are NEVER evicted.
    WiFiConn *allocEvicting();
    void addRef(WiFiConn *c);
    void release(WiFiConn *c);           // drop a handle; frees the slot when
                                         // refs==0 and the conn is dead
    // Clear EVERY callback BEFORE tcp_close; tcp_abort on close failure.
    // Returns what an in-callback caller must return to lwip (ERR_ABRT after
    // the abort path -- returning ERR_OK there leaves tcp_input on a freed
    // pcb; see m2_lwip_test.cpp closeEcho).
    err_t closeConn(WiFiConn *c);
    void abortAll();                     // link lost: no FIN possible
    void installCallbacks(WiFiConn *c, struct tcp_pcb *pcb);
    int  availableBytes(const WiFiConn *c);
    int  peekByte(const WiFiConn *c);
    int  consume(WiFiConn *c, uint8_t *buf, int len);  // + tcp_recved
    uint32_t evictions();                // silicon-visible safety-valve counter
}
```

- [ ] **Step 2: Write WiFiConnPool.cpp**

```cpp
/* WiFiConnPool.cpp - see WiFiConnPool.h. MIT, (c) 2026 Nicholas Newdigate. */
#include "WiFiConnPool.h"
#include "Arduino.h"
#include "lwip/pbuf.h"

static WiFiConn s_conns[WIFI_MAX_CONNS];
static uint32_t s_evictions = 0;

namespace WiFiPool {

WiFiConn *slot(uint8_t i) { return (i < WIFI_MAX_CONNS) ? &s_conns[i] : nullptr; }
uint32_t evictions() { return s_evictions; }

static void freeRx(WiFiConn *c) {
    if (c->rxHead) { pbuf_free(c->rxHead); c->rxHead = nullptr; }
    c->rxOff = 0;
}

static void toFree(WiFiConn *c) {
    freeRx(c);
    c->state = WiFiConn::FREE;
    c->pcb = nullptr;
    c->refs = 0;
    c->claimed = false;
    c->serverPort = 0;
    c->connectDone = c->connectOk = false;
}

WiFiConn *alloc() {
    for (uint8_t i = 0; i < WIFI_MAX_CONNS; i++)
        if (s_conns[i].state == WiFiConn::FREE) return &s_conns[i];
    return nullptr;
}

WiFiConn *allocEvicting() {
    WiFiConn *c = alloc();
    if (c) return c;
    WiFiConn *victim = nullptr;
    for (uint8_t i = 0; i < WIFI_MAX_CONNS; i++) {
        WiFiConn *s = &s_conns[i];
        if (s->claimed || s->serverPort == 0) continue;   // only unclaimed accepts
        if (!victim || (int32_t)(victim->lastActivityMs - s->lastActivityMs) > 0)
            victim = s;
    }
    if (!victim) return nullptr;
    s_evictions++;
    (void)closeConn(victim);     // clears callbacks first; not in a callback here
    toFree(victim);
    return victim;
}

void addRef(WiFiConn *c) { if (c) c->refs++; }

void release(WiFiConn *c) {
    if (!c || c->refs == 0) return;
    if (--c->refs == 0) {
        // Last handle gone.  A live conn the sketch abandoned gets closed --
        // Arduino clients don't linger after their last handle dies.
        if (c->pcb) (void)closeConn(c);
        toFree(c);
    }
}

err_t closeConn(WiFiConn *c) {
    struct tcp_pcb *pcb = c->pcb;
    c->pcb = nullptr;
    if (!pcb) return ERR_OK;
    tcp_arg(pcb, nullptr);
    tcp_recv(pcb, nullptr);
    tcp_sent(pcb, nullptr);
    tcp_err(pcb, nullptr);
    tcp_poll(pcb, nullptr, 0);
    if (tcp_close(pcb) != ERR_OK) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    return ERR_OK;
}

void abortAll() {
    for (uint8_t i = 0; i < WIFI_MAX_CONNS; i++) {
        WiFiConn *c = &s_conns[i];
        if (c->state == WiFiConn::FREE || !c->pcb) continue;
        struct tcp_pcb *pcb = c->pcb;
        c->pcb = nullptr;
        tcp_arg(pcb, nullptr); tcp_recv(pcb, nullptr); tcp_sent(pcb, nullptr);
        tcp_err(pcb, nullptr); tcp_poll(pcb, nullptr, 0);
        tcp_abort(pcb);                     // link is dead; no FIN possible
        c->state = WiFiConn::PEER_CLOSED;   // rx chain stays readable
        c->connectDone = true; c->connectOk = false;
        if (c->refs == 0) toFree(c);
    }
}

int availableBytes(const WiFiConn *c) {
    if (!c || !c->rxHead) return 0;
    return (int)c->rxHead->tot_len - (int)c->rxOff;
}

int peekByte(const WiFiConn *c) {
    if (availableBytes(c) <= 0) return -1;
    uint8_t b;
    if (pbuf_copy_partial(c->rxHead, &b, 1, c->rxOff) != 1) return -1;
    return b;
}

int consume(WiFiConn *c, uint8_t *buf, int len) {
    int avail = availableBytes(c);
    if (avail <= 0 || len <= 0) return 0;
    uint16_t n = (uint16_t)((len < avail) ? len : avail);
    uint16_t got = pbuf_copy_partial(c->rxHead, buf, n, c->rxOff);
    c->rxOff += got;
    // Free fully-consumed leading pbufs (ref-next-then-free-head; a plain
    // pbuf_free on the head would free the whole chain).
    while (c->rxHead && c->rxOff >= c->rxHead->len) {
        struct pbuf *h = c->rxHead;
        c->rxOff -= h->len;
        c->rxHead = h->next;
        if (c->rxHead) pbuf_ref(c->rxHead);
        pbuf_free(h);
    }
    if (c->pcb) tcp_recved(c->pcb, got);   // opens the window only as consumed
    c->lastActivityMs = millis();
    return got;
}

// --- lwip callbacks (arg is ALWAYS the slot) --------------------------------
static err_t connRecv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t) {
    WiFiConn *c = (WiFiConn *)arg;
    if (p == nullptr) {                    // peer FIN; keep pcb for our close
        c->state = WiFiConn::PEER_CLOSED;
        return ERR_OK;
    }
    if (c->rxHead) pbuf_cat(c->rxHead, p); else { c->rxHead = p; c->rxOff = 0; }
    c->lastActivityMs = millis();
    (void)pcb;
    return ERR_OK;                         // tcp_recved deferred to consume()
}

static void connErr(void *arg, err_t) {    // pcb ALREADY FREED by lwip:
    WiFiConn *c = (WiFiConn *)arg;         // reset state only, never tcp_*
    c->pcb = nullptr;
    c->state = WiFiConn::PEER_CLOSED;
    c->connectDone = true; c->connectOk = false;
    if (c->refs == 0) toFree(c);
}

static err_t connSent(void *arg, struct tcp_pcb *, u16_t) {
    ((WiFiConn *)arg)->lastActivityMs = millis();
    return ERR_OK;
}

// Stall safety valve: an accepted conn the sketch never picked up, idle past
// 30 s, is aborted (2 ticks/s * 60 = tcp_poll interval 60 => ~30 s).  Claimed
// conns get no poll -- an idle-but-held session is the sketch's business.
static err_t connPoll(void *arg, struct tcp_pcb *pcb) {
    WiFiConn *c = (WiFiConn *)arg;
    if (!c->claimed && millis() - c->lastActivityMs > 30000) {
        c->pcb = nullptr;
        tcp_arg(pcb, nullptr); tcp_recv(pcb, nullptr); tcp_sent(pcb, nullptr);
        tcp_err(pcb, nullptr); tcp_poll(pcb, nullptr, 0);
        tcp_abort(pcb);
        toFree(c);
        return ERR_ABRT;                   // in-callback abort contract
    }
    return ERR_OK;
}

void installCallbacks(WiFiConn *c, struct tcp_pcb *pcb) {
    c->pcb = pcb;
    c->lastActivityMs = millis();
    tcp_arg(pcb, c);
    tcp_recv(pcb, connRecv);
    tcp_err(pcb, connErr);
    tcp_sent(pcb, connSent);
    tcp_poll(pcb, connPoll, 60);
}

} // namespace WiFiPool
```

- [ ] **Step 3: Wire linkLost() to the pool**

In `WiFi.cpp`: add `#include "WiFiConnPool.h"` and replace the `// Pool teardown arrives with the pool (Task 6).` comment inside `linkLost()` with `WiFiPool::abortAll();`.

- [ ] **Step 4: Build + both gates**

```bash
cd $EVKB/examples/networking/wifi_client_test && cmake --build build && ./run_qemu.sh && ./run_qemu_wifi.sh
```
Expected: builds; both PASS.

- [ ] **Step 5: Commit (M2Radio)**

```bash
cd ~/Development/M2Radio && git add arduino/ && git commit -m "arduino: WiFiConnPool -- slots own pcbs, refcounted handles, unclaimed-only eviction"
```

### Task 7: WiFiClient + the echo loop in the sketch

**Files:** Replace `~/Development/M2Radio/arduino/WiFiClient.h`; create `~/Development/M2Radio/arduino/WiFiClient.cpp`; modify the sketch.

- [ ] **Step 1: Write WiFiClient.h (replacing the placeholder)**

```cpp
/* WiFiClient.h - Arduino Client over a WiFiConnPool slot.
 *
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * A refcounted HANDLE: copies share the connection (Arduino semantics), and
 * lwip only ever sees the pool slot, so destroying handles cannot dangle a
 * callback.  available()/read()/connected() run one service pass first, so
 * the classic `while (client.connected()) if (client.available())` sketch
 * loop services the link by construction.
 */
#pragma once
#include "Client.h"

struct WiFiConn;

class WiFiClient : public Client {
public:
    WiFiClient() : m_conn(nullptr) {}
    explicit WiFiClient(WiFiConn *conn);       // pool handoff (server accept)
    WiFiClient(const WiFiClient &other);
    WiFiClient &operator=(const WiFiClient &other);
    virtual ~WiFiClient();

    int connect(IPAddress ip, uint16_t port) override;
    int connect(const char *host, uint16_t port) override;
    size_t write(uint8_t b) override { return write(&b, 1); }
    size_t write(const uint8_t *buf, size_t size) override;
    int available() override;
    int read() override;
    int read(uint8_t *buf, size_t size) override;
    int peek() override;
    void flush() override;                     // drain lwip's send buffer
    void stop() override;
    uint8_t connected() override;
    operator bool() override { return m_conn != nullptr; }
    using Print::write;

private:
    void detach();
    WiFiConn *m_conn;
};
```

- [ ] **Step 2: Write WiFiClient.cpp**

```cpp
/* WiFiClient.cpp - see WiFiClient.h. MIT, (c) 2026 Nicholas Newdigate. */
#include "WiFiClient.h"
#include "WiFiConnPool.h"
#include "WiFi.h"
#include "Arduino.h"
#include "lwip/tcp.h"

WiFiClient::WiFiClient(WiFiConn *conn) : m_conn(conn) {
    if (m_conn) { WiFiPool::addRef(m_conn); m_conn->claimed = true; }
}
WiFiClient::WiFiClient(const WiFiClient &o) : Client(o), m_conn(o.m_conn) {
    if (m_conn) WiFiPool::addRef(m_conn);
}
WiFiClient &WiFiClient::operator=(const WiFiClient &o) {
    if (this == &o) return *this;
    detach();
    m_conn = o.m_conn;
    if (m_conn) WiFiPool::addRef(m_conn);
    return *this;
}
WiFiClient::~WiFiClient() { detach(); }
void WiFiClient::detach() {
    if (m_conn) { WiFiPool::release(m_conn); m_conn = nullptr; }
}

static err_t clientConnected(void *arg, struct tcp_pcb *, err_t) {
    WiFiConn *c = (WiFiConn *)arg;
    c->state = WiFiConn::ESTABLISHED;
    c->connectDone = true; c->connectOk = true;
    return ERR_OK;
}

static bool connectCond(void *arg) { return ((WiFiConn *)arg)->connectDone; }

int WiFiClient::connect(IPAddress ip, uint16_t port) {
    if (!WiFi.lwipUp() || !WiFi.linkUp()) return 0;
    stop();
    WiFiConn *c = WiFiPool::alloc();
    if (!c) return 0;
    struct tcp_pcb *pcb = tcp_new();
    if (!pcb) return 0;
    c->state = WiFiConn::CONNECTING;
    c->claimed = true;
    c->connectDone = c->connectOk = false;
    WiFiPool::installCallbacks(c, pcb);
    WiFiPool::addRef(c);
    m_conn = c;
    ip_addr_t dst;
    IP_ADDR4(&dst, ip[0], ip[1], ip[2], ip[3]);
    if (tcp_connect(pcb, &dst, port, clientConnected) != ERR_OK) {
        tcp_abort(pcb);                        // callbacks attached -> connErr
        detach();
        return 0;
    }
    if (!WiFi.pumpUntil(connectCond, c, 10000)) {
        if (c->pcb) tcp_abort(c->pcb);         // timeout -> connErr bookkeeping
        detach();
        return 0;
    }
    if (!c->connectOk) { detach(); return 0; }
    return 1;
}

int WiFiClient::connect(const char *host, uint16_t port) {
    IPAddress ip;
    if (!WiFi.hostByName(host, ip)) return 0;
    return connect(ip, port);
}

size_t WiFiClient::write(const uint8_t *buf, size_t size) {
    if (!m_conn || !m_conn->pcb || size == 0) return 0;
    size_t sent = 0;
    uint32_t t0 = millis();
    while (sent < size && m_conn->pcb) {
        u16_t room = tcp_sndbuf(m_conn->pcb);
        if (room == 0) {
            tcp_output(m_conn->pcb);
            if (millis() - t0 >= 5000) break;     // short write, bounded
            WiFi.servicePass();
            delay(1);
            continue;
        }
        u16_t n = (u16_t)((size - sent < room) ? size - sent : room);
        if (tcp_write(m_conn->pcb, buf + sent, n, TCP_WRITE_FLAG_COPY) != ERR_OK) {
            if (millis() - t0 >= 5000) break;
            WiFi.servicePass();
            delay(1);
            continue;
        }
        sent += n;
    }
    if (m_conn->pcb) tcp_output(m_conn->pcb);
    return sent;
}

int WiFiClient::available() {
    WiFi.servicePass();
    return WiFiPool::availableBytes(m_conn);
}
int WiFiClient::read() {
    uint8_t b;
    return (read(&b, 1) == 1) ? b : -1;
}
int WiFiClient::read(uint8_t *buf, size_t size) {
    WiFi.servicePass();
    if (!m_conn) return -1;
    int got = WiFiPool::consume(m_conn, buf, (int)size);
    if (got == 0 && m_conn->state == WiFiConn::PEER_CLOSED) return -1;
    return got;
}
int WiFiClient::peek() {
    WiFi.servicePass();
    return WiFiPool::peekByte(m_conn);
}

static bool drainedCond(void *arg) {
    WiFiConn *c = (WiFiConn *)arg;
    return !c->pcb || tcp_sndbuf(c->pcb) == TCP_SND_BUF;
}
void WiFiClient::flush() {
    if (!m_conn || !m_conn->pcb) return;
    tcp_output(m_conn->pcb);
    (void)WiFi.pumpUntil(drainedCond, m_conn, 5000);
}

void WiFiClient::stop() {
    if (!m_conn) return;
    flush();                                   // bounded drain, then close
    if (m_conn->pcb) (void)WiFiPool::closeConn(m_conn);  // not in a callback
    m_conn->state = WiFiConn::PEER_CLOSED;
    detach();
}

uint8_t WiFiClient::connected() {
    WiFi.servicePass();
    if (!m_conn) return 0;
    if (m_conn->state == WiFiConn::ESTABLISHED) return 1;
    // Arduino convention: still "connected" while unread data remains.
    if (m_conn->state == WiFiConn::PEER_CLOSED && WiFiPool::availableBytes(m_conn) > 0)
        return 1;
    return 0;
}
```

- [ ] **Step 3: Add the echo loop to the sketch**

In `wifi_client_test.cpp` `loop()`, insert BEFORE the heartbeat block:

```cpp
    static uint32_t lastKick = 0;
    if (WiFi.status() == WL_CONNECTED && millis() - lastKick >= 2000) {
        lastKick = millis();
        WiFiClient c;
        char msg[48];
        int n = snprintf(msg, sizeof(msg), "WIFI hello %lu", (unsigned long)s_tx);
        s_tx++;
        if (c.connect(IPAddress(192, 168, 4, 1), 4712)) {   // the ESP echo oracle
            c.write((const uint8_t *)msg, (size_t)n);
            char echo[48];
            int got = 0;
            uint32_t t0 = millis();
            while (got < n && millis() - t0 < 5000) {
                int r = c.read((uint8_t *)echo + got, (size_t)(n - got));
                if (r > 0) got += r;
                else delay(1);          // yield -> the auto-service pump runs
            }
            if (got == n && memcmp(echo, msg, (size_t)n) == 0) s_ok++; else s_fail++;
            c.stop();
        } else {
            s_fail++;
        }
    }
```

- [ ] **Step 4: Build + both gates (echo path is inert in QEMU — status is never WL_CONNECTED there)**

```bash
cd $EVKB/examples/networking/wifi_client_test && cmake --build build && ./run_qemu.sh && ./run_qemu_wifi.sh
```
Expected: both PASS.

- [ ] **Step 5: Commit both repos**

```bash
cd ~/Development/M2Radio && git add arduino/ && git commit -m "arduino: WiFiClient -- refcounted pool handle, pumping connect/write/flush/stop"
cd $EVKB && git add examples/networking/wifi_client_test && git commit -m "networking: wifi_client_test echo loop against the ESP oracle (silicon path)"
```

### Task 8: hostByName (DNS)

**Files:** Modify `~/Development/M2Radio/arduino/WiFi.cpp` (replace the Task-4 stub).

- [ ] **Step 1: Implement**

Replace `int WiFiClass::hostByName(const char *, IPAddress &, uint32_t) { return 0; }` with:

```cpp
struct DnsWait { volatile bool done; ip_addr_t addr; bool ok; };

static void dnsFound(const char *, const ip_addr_t *ipaddr, void *arg) {
    DnsWait *w = (DnsWait *)arg;
    if (ipaddr) { w->addr = *ipaddr; w->ok = true; }
    w->done = true;
}
static bool dnsCond(void *arg) { return ((DnsWait *)arg)->done; }

int WiFiClass::hostByName(const char *host, IPAddress &out, uint32_t timeoutMs) {
    if (!m_lwipUp || !m_linkUp) return 0;
    DnsWait w = { false, {}, false };
    ip_addr_t cached;
    err_t e = dns_gethostbyname(host, &cached, dnsFound, &w);
    if (e == ERR_OK) { out = IPAddress(ip4_addr_get_u32(ip_2_ip4(&cached))); return 1; }
    if (e != ERR_INPROGRESS) return 0;
    if (!pumpUntil(dnsCond, &w, timeoutMs) || !w.ok) return 0;
    out = IPAddress(ip4_addr_get_u32(ip_2_ip4(&w.addr)));
    return 1;
}
```

- [ ] **Step 2: Build + both gates**

```bash
cd $EVKB/examples/networking/wifi_client_test && cmake --build build && ./run_qemu.sh && ./run_qemu_wifi.sh
```
Expected: both PASS.

- [ ] **Step 3: Commit (M2Radio)**

```bash
cd ~/Development/M2Radio && git add arduino/ && git commit -m "arduino: WiFi.hostByName over lwip DNS (DHCP-supplied servers)"
```

### Task 9: WiFiServer

**Files:** Create `~/Development/M2Radio/arduino/WiFiServer.h`, `~/Development/M2Radio/arduino/WiFiServer.cpp`.

- [ ] **Step 1: Write WiFiServer.h**

```cpp
/* WiFiServer.h - Arduino Server over the WiFiConnPool.
 *
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Accept backpressure + the stall safety valve: with the pool full, an
 * incoming SYN evicts the least-recently-active accepted-but-NEVER-CLAIMED
 * conn (the sketch never saw it, so nothing dangles); if every slot is
 * claimed, the accept is refused (ERR_MEM).  Claimed conns are never touched
 * -- but note the flip side: a sketch holding 4 claimed dead conns starves
 * new accepts until it stop()s them.
 */
#pragma once
#include "Server.h"
#include "WiFiClient.h"

class WiFiServer : public Server {
public:
    explicit WiFiServer(uint16_t port) : m_port(port) {}
    // Safe to call with no link/lwip: records nothing, stays falsy, no wedge.
    void begin() override;
    WiFiClient available();              // a conn with data pending (claims it)
    WiFiClient accept();                 // any unclaimed accepted conn
    size_t write(uint8_t b) override { return write(&b, 1); }
    size_t write(const uint8_t *buf, size_t size) override;   // broadcast
    void end();
    operator bool() { return m_listen != nullptr; }
    using Print::write;

private:
    static err_t acceptCb(void *arg, struct tcp_pcb *newpcb, err_t err);
    uint16_t m_port;
    struct tcp_pcb *m_listen = nullptr;
};
```

- [ ] **Step 2: Write WiFiServer.cpp**

```cpp
/* WiFiServer.cpp - see WiFiServer.h. MIT, (c) 2026 Nicholas Newdigate. */
#include "WiFiServer.h"
#include "WiFiConnPool.h"
#include "WiFi.h"
#include "lwip/tcp.h"

err_t WiFiServer::acceptCb(void *arg, struct tcp_pcb *newpcb, err_t err) {
    WiFiServer *srv = (WiFiServer *)arg;
    if (err != ERR_OK || newpcb == nullptr) return ERR_VAL;
    WiFiConn *c = WiFiPool::allocEvicting();
    if (!c) return ERR_MEM;              // every slot claimed: refuse; lwip
                                         // aborts the new pcb on non-OK
    c->state = WiFiConn::ESTABLISHED;
    c->claimed = false;
    c->serverPort = srv->m_port;
    c->connectDone = true; c->connectOk = true;
    WiFiPool::installCallbacks(c, newpcb);
    return ERR_OK;
}

void WiFiServer::begin() {
    if (m_listen || !WiFi.lwipUp()) return;   // no-link guard: stays falsy
    struct tcp_pcb *pcb = tcp_new();
    if (!pcb) return;
    if (tcp_bind(pcb, IP4_ADDR_ANY, m_port) != ERR_OK) { tcp_abort(pcb); return; }
    struct tcp_pcb *l = tcp_listen(pcb);      // frees pcb on success only
    if (!l) { tcp_abort(pcb); return; }
    m_listen = l;
    tcp_arg(m_listen, this);
    tcp_accept(m_listen, acceptCb);
}

WiFiClient WiFiServer::available() {
    WiFi.servicePass();
    for (uint8_t i = 0; i < WIFI_MAX_CONNS; i++) {
        WiFiConn *c = WiFiPool::slot(i);
        if (c->state == WiFiConn::FREE || c->serverPort != m_port) continue;
        if (WiFiPool::availableBytes(c) > 0) return WiFiClient(c);  // claims
    }
    return WiFiClient();
}

WiFiClient WiFiServer::accept() {
    WiFi.servicePass();
    for (uint8_t i = 0; i < WIFI_MAX_CONNS; i++) {
        WiFiConn *c = WiFiPool::slot(i);
        if (c->state == WiFiConn::FREE || c->serverPort != m_port) continue;
        if (!c->claimed) return WiFiClient(c);                      // claims
    }
    return WiFiClient();
}

size_t WiFiServer::write(const uint8_t *buf, size_t size) {
    for (uint8_t i = 0; i < WIFI_MAX_CONNS; i++) {
        WiFiConn *c = WiFiPool::slot(i);
        if (c->serverPort != m_port || c->state != WiFiConn::ESTABLISHED || !c->pcb)
            continue;
        // Broadcast is best-effort per conn; a full sndbuf drops that conn's
        // copy rather than blocking the rest.
        if (tcp_write(c->pcb, buf, (u16_t)size, TCP_WRITE_FLAG_COPY) == ERR_OK)
            tcp_output(c->pcb);
    }
    return size;
}

void WiFiServer::end() {
    if (!m_listen) return;
    tcp_arg(m_listen, nullptr);
    tcp_accept(m_listen, nullptr);
    (void)tcp_close(m_listen);           // listen pcbs close synchronously
    m_listen = nullptr;
}
```

- [ ] **Step 3: Build wifi_client_test (compiles the new files) + both gates**

```bash
cd $EVKB/examples/networking/wifi_client_test && cmake --build build && ./run_qemu.sh && ./run_qemu_wifi.sh
```
Expected: builds; both PASS.

- [ ] **Step 4: Commit (M2Radio)**

```bash
cd ~/Development/M2Radio && git add arduino/ && git commit -m "arduino: WiFiServer -- listen/accept into the pool, unclaimed eviction valve, broadcast write"
```

### Task 10: wifi_server_test example + gate (TDD: gate first) + wifi_peer.py

**Files:**
- Create: `$EVKB/examples/networking/wifi_server_test/{run_qemu.sh,CMakeLists.txt,wifi_server_test.cpp,wifi_peer.py}`
- Modify: `$EVKB/tools/license-audit.sh` (GATES)

- [ ] **Step 1: Write the failing gate**

`run_qemu.sh` (then `chmod +x`):

```sh
#!/bin/sh
# run_qemu.sh — the CARD-ABSENT gate for wifi_server_test.
#
# WHAT THIS PROVES: with no card, WiFi.begin() fails WL_NO_SHIELD, and a
# WiFiServer.begin() on the dead stack is a clean no-op -- server_begin=
# ok_nolink, no wedge, heartbeat alive.  WHAT THIS DOES NOT PROVE: any
# accept/data path (QEMU cannot associate; the model returns zero scan
# results).  The server data path is silicon-only: transcript_hw_evkb.txt +
# wifi_peer.py (the Mac side is the authoritative measurement).
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/wifi_server_test.elf"
OUT=$(gate_capture_path "$DIR" serial.uart)
rm -f "$OUT"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none $(gate_console "$OUT") \
    -d guest_errors -D "$(gate_capture_path "$DIR" serial.dbg)" &
P=$!; gate_pid $P
for _ in $(seq 1 60); do
    [ -f "$OUT" ] && grep -q "alive=2" "$OUT" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"
grep -q "RT1176 WiFi server test up" "$OUT" || { echo "FAIL: banner missing"; exit 1; }
grep -q "^wifi_status=255" "$OUT" || { echo "FAIL: expected WL_NO_SHIELD (255) with no card"; exit 1; }
grep -q "^server_begin=ok_nolink" "$OUT" || {
    echo "FAIL: server.begin() on a dead stack must be a clean no-op"; exit 1; }
grep -q "^alive=2" "$OUT" || { echo "FAIL: no heartbeat -- server.begin() wedged?"; exit 1; }
echo "PASS: WL_NO_SHIELD fallback; server.begin() no-op'd cleanly; alive"
```

- [ ] **Step 2: Run it — FAIL (no ELF)**

```bash
cd $EVKB/examples/networking/wifi_server_test && ./run_qemu.sh
```

- [ ] **Step 3: CMakeLists.txt**

```cmake
cmake_minimum_required(VERSION 3.24)
project(wifi_server_test)

if(NOT DEFINED TEENSY_VERSION)
    set(TEENSY_VERSION 117 CACHE STRING "")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

import_evkb_library(lwip
    src/include src/core src/core/ipv4 src/netif port port/arch)
# sdio + iw416 + the lwip netif glue + the Arduino facade (WiFi/WiFiServer).
import_evkb_library(M2Radio sdio iw416 lwip arduino)

# --- IW416 firmware blob (NOT vendored) -- same rules as m2_lwip_test -------
set(M2RADIO_IW416_FW "" CACHE FILEPATH "IW416 firmware .bin.inc from an NXP SDK")
if(M2RADIO_IW416_FW AND EXISTS "${M2RADIO_IW416_FW}")
    message(STATUS "IW416 firmware: ${M2RADIO_IW416_FW}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/iw416_fw.cpp"
"#include <stdint.h>\n"
"// Generated at configure time from an NXP SDK copy. Never committed.\n"
"extern const uint8_t iw416_fw[];\n"
"extern const uint32_t iw416_fw_len;\n"
"const uint8_t iw416_fw[] __attribute__((section(\".progmem\"), used)) = {\n#include \"${M2RADIO_IW416_FW}\"\n};\n"
"const uint32_t iw416_fw_len = sizeof(iw416_fw);\n")
    set(M2_FW_SRC "${CMAKE_CURRENT_BINARY_DIR}/iw416_fw.cpp")
    add_definitions(-DHAVE_IW416_FW=1)
else()
    message(STATUS "IW416 firmware: not supplied -- download will be skipped")
    set(M2_FW_SRC "")
endif()

# --- Wi-Fi credentials (NEVER committed) -- same pattern as m2_lwip_test ----
set(M2RADIO_WIFI_SSID "" CACHE STRING "Target AP SSID")
set(M2RADIO_WIFI_PSK  "" CACHE STRING "Target AP WPA2 passphrase -- NEVER committed")
if(M2RADIO_WIFI_SSID AND M2RADIO_WIFI_PSK)
    message(STATUS "Wi-Fi target SSID: ${M2RADIO_WIFI_SSID} (PSK supplied, not shown)")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/wifi_creds.h"
"// Generated at configure time from -DM2RADIO_WIFI_*. NEVER committed.\n"
"#pragma once\n"
"#define M2_WIFI_SSID \"${M2RADIO_WIFI_SSID}\"\n"
"#define M2_WIFI_PSK  \"${M2RADIO_WIFI_PSK}\"\n")
    add_definitions(-DHAVE_WIFI_CREDS=1)
endif()

teensy_add_executable(wifi_server_test wifi_server_test.cpp ${M2_FW_SRC})
teensy_target_link_libraries(wifi_server_test cores M2Radio lwip)
target_include_directories(wifi_server_test.elf PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(wifi_server_test.elf stdc++)
```

- [ ] **Step 4: The sketch**

`wifi_server_test.cpp`:

```cpp
// Arduino WiFiServer proof: accept + echo, driven by wifi_peer.py on the Mac
// (the authoritative side).  QEMU (run_qemu.sh) proves only the card-absent
// fallback and that server.begin() on a dead stack is a clean no-op.
#include "Arduino.h"
#include "HardwareSerial.h"
#include "WiFi.h"
#include "WiFiClient.h"
#include "WiFiServer.h"
#include "WiFiConnPool.h"

#if defined(HAVE_WIFI_CREDS)
#include "wifi_creds.h"
#else
#define M2_WIFI_SSID "WIFI-GATE-NO-SUCH-AP"   // deliberately nonexistent
#define M2_WIFI_PSK  nullptr
#endif

#if defined(HAVE_IW416_FW)
extern const uint8_t  iw416_fw[];
extern const uint32_t iw416_fw_len;
#endif

static WiFiServer server(5010);

void setup() {
    Serial1.begin(115200);
    delay(50);
    Serial1.println("RT1176 WiFi server test up");
#if defined(HAVE_IW416_FW)
    WiFi.setFirmware(iw416_fw, iw416_fw_len);
#endif
    int st = WiFi.begin(M2_WIFI_SSID, M2_WIFI_PSK);
    Serial1.print("wifi_status="); Serial1.println(st);
    if (st == WL_CONNECTED) {
        Serial1.print("wifi_ip="); Serial1.println(WiFi.localIP());
    }
    server.begin();     // with no link this must be a clean, falsy no-op
    Serial1.print("server_begin=");
    Serial1.println(server ? "listening" : "ok_nolink");
}

void loop() {
    static uint32_t lastBeat = 0, beats = 0;
    if (server) {
        WiFiClient c = server.available();
        if (c) {
            uint8_t buf[256];
            int n;
            // Bounded per pass: echo what is there, then return to loop().
            while ((n = c.read(buf, sizeof(buf))) > 0) c.write(buf, (size_t)n);
        }
    }
    if (millis() - lastBeat >= 1000) {
        lastBeat = millis();
        Serial1.print("alive=");  Serial1.print(++beats);
        Serial1.print(" evict="); Serial1.println(WiFiPool::evictions());
    }
}
```

- [ ] **Step 5: Build + gate PASS**

```bash
cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake && cmake --build build && ./run_qemu.sh
```
Expected: `PASS: WL_NO_SHIELD fallback; server.begin() no-op'd cleanly; alive`.

- [ ] **Step 6: wifi_peer.py (the Mac-side authoritative peer for silicon)**

```python
#!/usr/bin/env python3
"""Mac-side peer for examples/networking/wifi_server_test.

The board runs WiFiServer echo on :5010; this script is the AUTHORITATIVE
check (its own byte counts; the board's serial lines are the cross-check).

  echo <ip>        3 sequential connections, byte-exact echo each
  concurrent <ip>  2 connections open at once, interleaved echos
  fill <ip>        4 idle connections (never send -> never claimed), then a
                   5th connect + echo MUST succeed via the eviction valve
                   (board heartbeat shows evict=1)
  all <ip>         the three in order; prints WIFISRV <test> PASS/FAIL lines
Python 3 stdlib only.
"""
import socket, sys, time

PORT = 5010

def _echo_once(ip, tag):
    msg = f"WIFISRV {tag} {time.monotonic_ns()}".encode()
    with socket.create_connection((ip, PORT), timeout=10) as s:
        s.sendall(msg)
        got = b""
        s.settimeout(10)
        while len(got) < len(msg):
            b = s.recv(4096)
            if not b:
                break
            got += b
    return got == msg

def t_echo(ip):
    return all(_echo_once(ip, f"echo{i}") for i in range(3))

def t_concurrent(ip):
    a = socket.create_connection((ip, PORT), timeout=10)
    b = socket.create_connection((ip, PORT), timeout=10)
    ok = True
    try:
        for i, s in enumerate((a, b, a, b)):
            msg = f"WIFISRV conc{i}".encode()
            s.sendall(msg)
            s.settimeout(10)
            got = b""
            while len(got) < len(msg):
                r = s.recv(4096)
                if not r:
                    break
                got += r
            ok = ok and got == msg
    finally:
        a.close(); b.close()
    return ok

def t_fill(ip):
    idlers = [socket.create_connection((ip, PORT), timeout=10) for _ in range(4)]
    try:
        time.sleep(1)                     # let the board's accepts land
        return _echo_once(ip, "evicted")  # 5th conn: needs the eviction valve
    finally:
        for s in idlers:
            s.close()

def main():
    if len(sys.argv) != 3 or sys.argv[1] not in ("echo", "concurrent", "fill", "all"):
        print(__doc__); sys.exit(2)
    test, ip = sys.argv[1], sys.argv[2]
    tests = {"echo": t_echo, "concurrent": t_concurrent, "fill": t_fill}
    names = list(tests) if test == "all" else [test]
    rc = 0
    for n in names:
        ok = tests[n](ip)
        print(f"WIFISRV {n} {'PASS' if ok else 'FAIL'}")
        rc |= 0 if ok else 1
        time.sleep(2)
    sys.exit(rc)

if __name__ == "__main__":
    main()
```

`chmod +x wifi_peer.py`.

- [ ] **Step 7: GATES entry + audit**

In `tools/license-audit.sh` GATES, directly after the `wifi_client_test` line added in Task 3, insert:

```
examples/networking/wifi_server_test:wifi_server_test \
```

Run: `AUDIT` → PASS.

- [ ] **Step 8: Commit (EVKB)**

```bash
cd $EVKB && git add examples/networking/wifi_server_test tools/license-audit.sh && git commit -m "networking: wifi_server_test -- WiFiServer echo example, card-absent gate, Mac peer script"
```

### Task 11: Preamble pointer comments in the four existing examples

**Files:** Modify one comment block in each of `examples/networking/{m2_lwip_test/m2_lwip_test.cpp, m2_sdio_probe/m2_sdio_probe.cpp, m2_throughput_test/m2_throughput_test.cpp, m2_rx_demo/m2_rx_demo.cpp}`.

- [ ] **Step 1: Add the pointer line**

In each file, find its `m2ReleaseWifiReset()` preamble comment (in m2_lwip_test it starts `// M.2 board bring-up preamble, from m2_sdio_probe`) and append one line to that comment block:

```c
// [2026-08-20] This preamble now ALSO lives in M2Radio arduino/WiFi.cpp
// (WiFi.begin() runs it by default); this copy stays for the non-facade path.
```

- [ ] **Step 2: Configure + build all four IN THE WORKTREE**

The worktree has no build dirs, so these are fresh configures. That is safe
and correct here — the "never reconfigure" rule protects the MAIN checkout's
dirs (creds + fw-blob paths in their caches); these gates need neither.

```bash
for d in m2_lwip_test m2_sdio_probe m2_throughput_test m2_rx_demo; do
  cd $EVKB/examples/networking/$d || exit 1
  cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake || exit 1
  cmake --build build || exit 1
done
```

- [ ] **Step 3: Re-run their 11 gates** (W16 added three m2_rx_demo variants)

```bash
cd $EVKB/examples/networking/m2_lwip_test && ./run_qemu.sh
cd $EVKB/examples/networking/m2_throughput_test && ./run_qemu.sh
cd $EVKB/examples/networking/m2_sdio_probe && ./run_qemu.sh && ./run_qemu_wifi.sh
cd $EVKB/examples/networking/m2_rx_demo && for g in run_qemu.sh run_qemu_ring.sh run_qemu_stranded.sh run_qemu_irq.sh run_qemu_regfallback.sh run_qemu_rxaggr.sh run_qemu_txaggr.sh; do ./$g || exit 1; done
```
Expected: all 11 PASS.

- [ ] **Step 4: Commit (EVKB)**

```bash
cd $EVKB && git add examples/networking/m2_*/ && git commit -m "networking: point the four m2_* preamble copies at their new library home"
```

### Task 12: M2Radio README section

**Files:** Modify `~/Development/M2Radio/README.md`.

- [ ] **Step 1: Append the arduino/ section**

```markdown
## `arduino/` — the Arduino WiFi facade

`import_evkb_library(M2Radio sdio iw416 lwip arduino)` (requires the lwip
library too) gives a sketch the classic surface:

    WiFi.setFirmware(fw, len);          // the NXP blob, configure-time supplied
    WiFi.begin(ssid, psk);              // preamble + SDIO + fw + DHCP, blocking
    WiFiClient c; c.connect(ip, 80);    // Arduino Client over a 4-slot pool
    WiFiServer s(80); s.begin();

Three things it will not let you do casually, on purpose:

* **Turn IEEE power save off.** PS-on is the W10 workaround for the firmware
  idle-RX-death erratum. If you must, `WiFi.radio().setIeeePs()` puts you next
  to the erratum comment.
* **Starve the link.** A yield()-driven pump services the stack from every
  `loop()` pass and every `delay()` millisecond (the W12/W13 safety net ticks
  on service passes). `WiFi.setAutoService(false)` + `WiFi.loop()` if you want
  the cadence yourself.
* **Mix with the Ethernet library.** `arduino/` carries its own clean-room MIT
  `Client`/`Server`/`IPAddress` base classes (copied from newdigate/Ethernet,
  same as NativeEthernet does), so importing both collides on `class Client`.

`WiFi.begin()` runs the M.2 board preamble (SDIO_RST/WL_RST + 1.8 V pads) by
default — the thing every pre-facade example had to open-code.
```

- [ ] **Step 2: Commit (M2Radio)**

```bash
cd ~/Development/M2Radio && git add README.md && git commit -m "readme: document the arduino/ WiFi facade"
```

### Task 13: Full sweep + repo docs

**Files:** Modify `$EVKB/CLAUDE.md`; check `$EVKB/docs/KNOWN-BROKEN-GATES.md`.

- [ ] **Step 0: Decide WHERE the sweep runs — the worktree cannot do it alone**

Discovery finds gates from the source tree, but the runner does not build:
a missing ELF is a SKIP. The worktree has build dirs only for the six
examples this plan touched, so a worktree sweep would report ~99 SKIPs — the
exact silent under-report CLAUDE.md warns about. The main checkout
(`~/Development/rt1176-evkb-m2-maya-w161`, branch `m2-phase0-serial2`) is
fully built but does not have this branch's two new examples.

**Escalate to the human with these two options** rather than guessing:
(a) build the ~99 remaining examples in the worktree, then sweep here; or
(b) merge/rebase this branch into the main checkout first and sweep there,
reusing its existing build dirs. Option (b) is far cheaper but reorders the
close-out. Do not fabricate a sweep result either way.

- [ ] **Step 1: Confirm discovery sees 111** (in whichever tree Step 0 chose)

```bash
cd $EVKB && ./tools/run-all-qemu-gates.sh -l | tail -3
```
Expected: the listing ends `(111 gate(s))`. If not 111, find the discovery discrepancy before sweeping.

- [ ] **Step 2: Sweep from the short path**

`/tmp/ev` already exists and points at the MAIN checkout — `ln -sf` onto an
existing symlink-to-a-directory creates the link *inside* it. Use `-n`, and
restore it afterwards if you repointed it.

```bash
ln -sfn <chosen-tree> /tmp/ev && ls -ld /tmp/ev
cd /tmp/ev && ./tools/run-all-qemu-gates.sh
```
Expected: `gates: 111 passed`, exit 0 (or 110/1 with ONLY `rt1176:dualcore/cm4_audio_test` red — re-run that one idle before accepting). Read gate NAMES on any other red.

- [ ] **Step 3: Update CLAUDE.md**

In the sweep paragraph: `108 gates` → `111 gates`, with the parenthetical history extended in the established style, e.g. prepend: `(110 before the Arduino WiFi facade added networking/wifi_server_test; 109 before it added networking/wifi_client_test's second gate — run_qemu_wifi.sh, enumeration + a real scan against the model's deliberate zero-BSS reply; 108 before wifi_client_test itself; …)`. Target line becomes `111 passed, 0 failed, 0 SKIP` / `110 passed, 1 failed`. Add the dated measured line above the previous one, per convention:

```
✅ Measured 2026-08-20: 111 passed, 0 failed, 0 SKIP (`gates: 111 passed`,
exit 0), on the Arduino WiFi facade close-out, run via `/tmp/ev`,
`rt1176:dualcore/cm4_audio_test` included and green.
```
(Adjust to what was actually measured — never write it before Step 2.)

In the model-dependent-gates block (`★ FIVE gates need the IW416 card model…`): FIVE → SIX, adding `networking/wifi_client_test[wifi]` to the list (same qemu2 `>= 2ed9314631` requirement, RED-not-SKIP on stock QEMU). The variant-suffix paragraph gains `wifi_client_test` as the second two-script example.

- [ ] **Step 4: KNOWN-BROKEN-GATES.md**

Read it; if it carries the model-dependent-gate list (it records exceptions by name), extend it the same way. If it doesn't mention them, leave it.

- [ ] **Step 5: Commit (EVKB)**

```bash
cd $EVKB && git add CLAUDE.md docs/KNOWN-BROKEN-GATES.md && git commit -m "docs: sweep baseline 111 -- wifi_client_test (2 gates) + wifi_server_test"
```

### Task 14: Silicon ⚠ REQUIRES THE BOARD — ask the user to semaphore first

**Files:** Create `examples/networking/wifi_client_test/transcript_hw_evkb.txt`, `examples/networking/wifi_server_test/transcript_hw_evkb.txt`.

- [ ] **Step 0: Ask the user for the board.** Another session soak-tests on it. Do not flash until they confirm. Also confirm the bench AP is up. It may now be the **ESP32-C6** (`tools/esp32c6-benchap/`, added upstream 2026-08-20) rather than the ESP8266 — it deliberately keeps the SAME SSID, PSK and 192.168.4.x subnet, so nothing in this plan changes either way; ask the user which is powered and get its PSK + the firmware `.bin.inc` path from the user (both are configure-time inputs, never committed).

- [ ] **Step 1: Configure + build both examples with fw + creds** (fresh build dirs — these are NEW examples; the never-reconfigure rule protects the OLD ones)

```bash
cd $EVKB/examples/networking/wifi_client_test
cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake \
  -DM2RADIO_IW416_FW=<path-from-user> -DM2RADIO_WIFI_SSID=ESP8266TEST -DM2RADIO_WIFI_PSK=<psk-from-user>
cmake --build build
```
Same for `wifi_server_test`. Note: reconfiguring these two dirs later without the `-D`s wipes the creds from the cache — the wiped-cache symptom looks exactly like a dead card.

- [ ] **Step 2: wifi_client_test on the board**

```bash
pkill LinkServer; pkill redlinkserv; pkill crt_emu_cm_redlink
LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load $EVKB/examples/networking/wifi_client_test/build/wifi_client_test.elf
# VCOM must be FREE during flash; attach the reader only afterwards:
python3 $EVKB/tools/rt1170-console.py /dev/cu.usbmodem* 115200 | tee /tmp/wifi_client_hw.log &
LinkServer run MIMXRT1176:MIMXRT1170-EVKB $EVKB/examples/networking/wifi_client_test/build/wifi_client_test.elf &
```
Watch for: `wifi_status=3`, `wifi_ip=192.168.4.x`, `wifi_rssi=-NN`, then `tcp=N/N/0` climbing with ok == tx. Let it run ≥ 60 s (≥ 25 echoes). Un-fakeable core: the echo bytes come back from the ESP, and the DHCP lease comes from the ESP.

- [ ] **Step 3: Save the client transcript**

Copy the log to `examples/networking/wifi_client_test/transcript_hw_evkb.txt` with a dated header describing bench setup (AP, oracle port, fw release line) — follow `m2_lwip_test/transcript_hw_evkb.txt`'s format. **Read the log for stray secrets before committing** (the PSK is never printed by design; verify).

- [ ] **Step 4: wifi_server_test on the board + the peer**

Flash/console the same way. Note the board's `wifi_ip=` line, join the Mac to `ESP8266TEST`, then:

```bash
python3 $EVKB/examples/networking/wifi_server_test/wifi_peer.py all <board-ip>
```
Expected: `WIFISRV echo PASS`, `WIFISRV concurrent PASS`, `WIFISRV fill PASS`, exit 0; board heartbeat shows `evict=1` after the fill test. Both sides recorded.

- [ ] **Step 5: Save the server transcript** (board serial + the peer's stdout in one file, labelled) → `examples/networking/wifi_server_test/transcript_hw_evkb.txt`.

- [ ] **Step 6: Commit (EVKB)**

```bash
cd $EVKB && git add examples/networking/wifi_*/transcript_hw_evkb.txt && git commit -m "test: silicon transcripts -- WiFi facade client echo + server accept/evict against the ESP bench"
```

### Task 15: Close-out — push, pin, fresh-user verify

- [ ] **Step 1: Push M2Radio** (the insulated clone is retired; work is already on `~/Development/M2Radio` master)

The other session may still have uncommitted work in `~/Development/M2Radio`.
A fast-forward only adds `arduino/`, so it cannot touch their five modified
files — but verify that rather than assume it.

```bash
git -C ~/Development/M2Radio status --short          # note their dirty files
git -C ~/Development/M2Radio fetch "$LIBROOT/M2Radio" master
git -C ~/Development/M2Radio merge --ff-only FETCH_HEAD
git -C ~/Development/M2Radio status --short          # same dirty files, unchanged
```
If the merge is not a fast-forward, STOP and escalate — that means they
committed to master meanwhile and the histories diverged.

```bash
git -C ~/Development/M2Radio push origin master && git -C ~/Development/M2Radio rev-parse HEAD
```
Note the new HEAD SHA (full 40 chars).

- [ ] **Step 2: Bump the pin**

In `$EVKB/evkb.cmake`, on the `teensy_declare_library(M2Radio …)` line, replace `c7d051068c55f9b8f33117939353c342b55195f8` with the new SHA, and update the trailing comment to `# subdir chosen by the importer: import_evkb_library(M2Radio sdio iw416 [lwip] [arduino])`.

- [ ] **Step 3: Fresh-user verify (the SKIP-hider check)**

A fresh-clone compile break presents as SKIP in a sweep, so check it directly — in a THROWAWAY build dir, forcing the pinned fetch:

```bash
cd $EVKB/examples/networking/wifi_client_test
cmake -B build-fetch -DEVKB_FORCE_FETCH=ON -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake
cmake --build build-fetch
rm -rf build-fetch
```
Expected: configure log shows M2Radio fetched at the new pin; build succeeds. (The creds-free build is exactly what a fresh user gets.)

- [ ] **Step 4: Final audit + the two wifi gates once more**

```bash
AUDIT
cd $EVKB/examples/networking/wifi_client_test && ./run_qemu.sh && ./run_qemu_wifi.sh
cd $EVKB/examples/networking/wifi_server_test && ./run_qemu.sh
```
Expected: PASS ×4.

- [ ] **Step 5: Commit (EVKB)**

```bash
cd $EVKB && git add evkb.cmake && git commit -m "build: bump M2Radio pin -- arduino/ WiFi facade (WiFi/WiFiClient/WiFiServer)"
```

---

## Deviations from the spec (recorded here, both deliberate)

* Spec §3 sketches a `m_pool.service()` step in the pump for "deferred-write
  retries and reaping". Write retries live inside `WiFiClient::write`'s own
  bounded pump and reaping lives in lwip's `tcp_poll` callback, so a separate
  pump step would be dead code — dropped (YAGNI).
* Spec §6's `CLOSING` state: `closeConn` either closes or aborts synchronously,
  so no conn can linger in a closing state — the state enum has no `CLOSING`.
  The "peer stops reading mid-stop()" case is bounded by `flush()`'s 5 s pump
  timeout instead, and with it goes the spec §7 parenthetical about asserting
  a CLOSING-drain abort from the peer script — there is no such state to
  assert; `flush()`'s bound is client-side and exercised by the client
  example's own stop() on every echo.

## Test-coverage honesty (say it, don't imply otherwise)

QEMU covers: card-absent fallbacks, enumeration, one real scan, honest
failure reporting, pump survival through a blocking begin(). QEMU does NOT
cover: association, DHCP, TCP, the pool, eviction, DNS — all silicon-only
(Task 14). The pool/client/server code paths are exercised in QEMU only as
far as "compiles, links, and the no-link guards hold". A future lwip loopback
netif (`LWIP_NETIF_LOOPBACK`) could close that gap in QEMU; it needs an
lwipopts change in the lwip repo and is deliberately out of scope here.
