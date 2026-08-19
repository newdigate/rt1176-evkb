# lwip over the IW416 M.2 Wi-Fi Link — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bridge the pinned lwip sibling to the M2Radio IW416 driver with a thin netif, proven by a new example that DHCPs via lwip and completes byte-exact TCP echoes against the ESP8266 oracle.

**Architecture:** A sink-based `Iw416::serviceLink()` in the driver delivers every RX frame per pass (the existing `pollLink()` is reimplemented on top of it, unchanged for callers); `M2Radio/lwip/Iw416Netif.{h,cpp}` adapts that to a lwip netif exactly like the lwip repo's `port/ethernetif.c` does for ENET; a new example `examples/networking/m2_lwip_test` runs the `NO_SYS=1` poll loop with a raw-API TCP echo client. Spec: `docs/superpowers/specs/2026-08-19-m2-lwip-netif-design.md`.

**Tech Stack:** M2Radio (sibling repo, C++), lwip 2.x raw API (sibling repo, pinned, `NO_SYS=1`), teensy-cmake-macros build, QEMU gate + EVKB silicon verification, ESP8266 oracle (arduino-cli).

**Verification model:** No host unit-test framework exists for driver code in this tree; the failing-test role is played by (a) compile checks, (b) the example's QEMU gate (device-absent fallback), and (c) silicon transcripts. Every task ends re-running the affected QEMU gate.

**Background caveat:** a firmware RX-death erratum (sparse-traffic idle, stochastic ~14–44 min) is under separate investigation. The TCP echo hardware proof completes within minutes of boot; if RX freezes during a long session, that is the KNOWN erratum, not this feature — check `ring=`/`rx=` counters before blaming the netif.

---

### Task 1: `Iw416::serviceLink()` — sink-based drain (driver)

The netif needs EVERY frame; `pollLink()` copies only the first frame per pass and counts later ones as dropped. Extract the drain into a sink-based method and reimplement `pollLink()` on it so probe behaviour is unchanged.

**Files:**
- Modify: `~/Development/M2Radio/iw416/Iw416.h` (after the `pollLink` declaration, ~line 330)
- Modify: `~/Development/M2Radio/iw416/Iw416.cpp` (replace the `pollLink` definition, ~line 1120)

- [ ] **Step 1: Add the declaration to Iw416.h**

Insert directly after the `pollLink(...)` declaration:

```cpp
    // Stack-facing service pass (W9).  One HOST_INT_STATUS read; EVERY
    // pending data frame is unwrapped (RxPD) and handed to `sink`; command
    // -port events are recorded and a DEAUTH/DISASSOC/LINK_LOST sets
    // *dropped.  Returns OK if any frame or a drop was seen, CMD_TIMEOUT for
    // a quiet pass (not an error), else the bus error.  pollLink() is this
    // with a copy-first-frame sink.
    typedef void (*FrameSink)(void *ctx, const uint8_t *frame, uint16_t len);
    SdioHost::Status serviceLink(FrameSink sink, void *ctx, bool *dropped,
                                 uint32_t waitMs);
```

- [ ] **Step 2: Reimplement pollLink on serviceLink in Iw416.cpp**

Replace the whole existing `Iw416::pollLink(...)` definition with:

```cpp
SdioHost::Status Iw416::serviceLink(FrameSink sink, void *ctx, bool *dropped,
                                    uint32_t waitMs) {
    static uint8_t rx[SDIO_BLOCK_SIZE * 8];
    if (dropped) *dropped = false;
    bool gotFrame = false, gotDrop = false;
    for (uint32_t waited = 0; waited <= waitMs; waited++) {
        uint8_t st = 0;
        SdioHost::Status s = m_host.cmd52Read(1, HOST_INT_STATUS, &st);
        if (s != SdioHost::OK) return s;
        m_intSeen |= st;

        if (st & HOST_INT_UP_LD) {
            // Drain every upload queued at the ring position (W8: the 32
            // ports are a ring; the packets sit at consecutive slots).
            for (;;) {
                uint16_t pktSize = 0;
                SdioHost::Status rs = readRingPacket(rx, sizeof(rx), &pktSize, nullptr);
                if (rs == SdioHost::CMD_TIMEOUT) break;         // ring drained
                if (rs != SdioHost::OK) break;                  // dropped/bus error
                uint16_t pkttype = (uint16_t)(rx[2] | ((uint16_t)rx[3] << 8));
                if (pkttype != MLAN_TYPE_DATA) continue;
                m_rxDataCount++;
                // RxPD: rx_pkt_length at +2, rx_pkt_offset at +4; the 802.3
                // frame at INTF_HEADER_LEN + rx_pkt_offset.
                const uint8_t *rxpd = &rx[INTF_HEADER_LEN];
                uint16_t plen = (uint16_t)(rxpd[2] | ((uint16_t)rxpd[3] << 8));
                uint16_t poff = (uint16_t)(rxpd[4] | ((uint16_t)rxpd[5] << 8));
                if ((uint32_t)INTF_HEADER_LEN + poff + plen > pktSize) continue;
                gotFrame = true;
                if (sink) sink(ctx, &rx[INTF_HEADER_LEN + poff], plen);
            }
        }
        if (st & CMD_PORT_UPLD) {
            uint8_t lo = 0, hi = 0;
            m_host.cmd52Read(1, CMD_RD_LEN_0, &lo);
            m_host.cmd52Read(1, CMD_RD_LEN_1, &hi);
            uint16_t len = (uint16_t)(lo | ((uint16_t)hi << 8));
            if (len && len <= sizeof(rx)) {
                uint16_t blocks = (uint16_t)((len + SDIO_BLOCK_SIZE - 1) / SDIO_BLOCK_SIZE);
                if (m_host.cmd53Read(1, m_ioPort | CMD_PORT_SLCT, false, rx, SDIO_BLOCK_SIZE, blocks) == SdioHost::OK) {
                    uint16_t pkttype = (uint16_t)(rx[2] | ((uint16_t)rx[3] << 8));
                    if (pkttype == MLAN_TYPE_EVENT) {
                        uint32_t ev = (uint32_t)rx[INTF_HEADER_LEN] | ((uint32_t)rx[INTF_HEADER_LEN+1] << 8) |
                                      ((uint32_t)rx[INTF_HEADER_LEN+2] << 16) | ((uint32_t)rx[INTF_HEADER_LEN+3] << 24);
                        m_lastEvent = ev;
                        if (len >= INTF_HEADER_LEN + 8)
                            m_lastEventInfo = (uint32_t)rx[INTF_HEADER_LEN+4] | ((uint32_t)rx[INTF_HEADER_LEN+5] << 8) |
                                              ((uint32_t)rx[INTF_HEADER_LEN+6] << 16) | ((uint32_t)rx[INTF_HEADER_LEN+7] << 24);
                        if (ev == EVENT_DEAUTHENTICATED || ev == EVENT_DISASSOCIATED ||
                            ev == EVENT_LINK_LOST) {
                            gotDrop = true;
                            if (dropped) *dropped = true;
                        }
                    }
                }
            }
        }
        if (gotFrame || gotDrop) return SdioHost::OK;
        if (!(st & (HOST_INT_UP_LD | CMD_PORT_UPLD))) delay(1);
    }
    return SdioHost::CMD_TIMEOUT;      // a quiet poll, not an error
}

// pollLink keeps its historical contract for the probe: the FIRST frame is
// copied out, later frames in the same pass are drained and counted in
// rxDropped().
namespace {
struct PollLinkCtx {
    uint8_t  *buf;
    uint16_t  cap;
    uint16_t *lenOut;
    bool      got;
    Iw416    *self;
};
}
static void pollLinkSink(void *vctx, const uint8_t *frame, uint16_t len);

SdioHost::Status Iw416::pollLink(uint8_t *frameBuf, uint16_t bufCap, uint16_t *frameLen,
                                 bool *dropped, uint32_t waitMs) {
    if (frameLen) *frameLen = 0;
    PollLinkCtx ctx = { frameBuf, bufCap, frameLen, false, this };
    return serviceLink(pollLinkSink, &ctx, dropped, waitMs);
}

static void pollLinkSink(void *vctx, const uint8_t *frame, uint16_t len) {
    PollLinkCtx *c = (PollLinkCtx *)vctx;
    if (!c->got && c->buf && len <= c->cap) {
        memcpy(c->buf, frame, len);
        if (c->lenOut) *c->lenOut = len;
        c->got = true;
    } else {
        c->self->countRxDropped();
    }
}
```

Add next to the other public accessors in `Iw416.h` (the sink runs outside the class):

```cpp
    // For pollLink's compatibility sink only.
    void countRxDropped() { m_rxDropped++; }
```

- [ ] **Step 3: Build the probe against the modified driver**

```bash
cd ~/Development/rt1176-evkb-m2-maya-w161/examples/networking/m2_sdio_probe
cmake --build build
```
Expected: `[100%] Built target m2_sdio_probe_hex`, no warnings about Iw416.

- [ ] **Step 4: Run the probe's QEMU gate (no regression)**

```bash
cd ~/Development/rt1176-evkb-m2-maya-w161/examples/networking/m2_sdio_probe
./run_qemu.sh
```
Expected: `PASS: SDIO enumerate reached the cmd5-no-response fallback cleanly`

- [ ] **Step 5: Commit (M2Radio)**

```bash
cd ~/Development/M2Radio
git add iw416/Iw416.h iw416/Iw416.cpp
git commit -m "iw416: serviceLink() -- sink-based per-frame drain; pollLink now wraps it"
```

---

### Task 2: `Iw416::connectStation()` — one-call station bring-up (driver)

**Files:**
- Modify: `~/Development/M2Radio/iw416/Iw416.h` (after the `associate` declaration block)
- Modify: `~/Development/M2Radio/iw416/Iw416.cpp` (append near the end)

- [ ] **Step 1: Add the declaration to Iw416.h**

```cpp
    // One-call station bring-up (W9): scan -> find `ssid` (exact byte match
    // against the beacon SSID) -> setPassphrase (skipped when psk is NULL or
    // empty -> open network) -> associate -> watchConnect, retrying the
    // associate up to `attempts` times on a rejection (CMD_CRC).  OK means
    // associated and settled (probe rule: no deauth in the watch window; a
    // port-release is not required here).  BAD_CIS = SSID not found in the
    // scan.  connectedAp() is valid after OK.
    SdioHost::Status connectStation(const char *ssid, const char *psk,
                                    uint8_t attempts = 3);
    const ScanResult &connectedAp() const { return m_connectedAp; }
```

And with the other private members:

```cpp
    ScanResult m_connectedAp = {};
```

- [ ] **Step 2: Implement in Iw416.cpp**

```cpp
SdioHost::Status Iw416::connectStation(const char *ssid, const char *psk,
                                       uint8_t attempts) {
    static ScanResult aps[12];
    uint8_t n = 0;
    SdioHost::Status s = scan(aps, 12, &n);
    if (s != SdioHost::OK) return s;
    int idx = -1;
    for (uint8_t i = 0; i < n; i++) {
        if (strcmp(aps[i].ssid, ssid) == 0) { idx = i; break; }
    }
    if (idx < 0) return SdioHost::BAD_CIS;         // SSID not in the scan
    m_connectedAp = aps[idx];

    if (psk && psk[0]) {
        // Use the SCANNED SSID bytes as the PBKDF2 salt (authoritative --
        // they are what the AP beacons), not the caller's spelling.
        s = setPassphrase(m_connectedAp.ssid, psk);
        if (s != SdioHost::OK) return s;
        delay(50);
    }
    for (uint8_t a = 0; a < attempts; a++) {
        s = associate(m_connectedAp);
        if (s != SdioHost::OK) continue;
        SdioHost::Status w = watchConnect(2500);
        // Probe rule: OK or a quiet TIMEOUT = up; CMD_CRC = rejected.
        if (w != SdioHost::CMD_CRC) return SdioHost::OK;
    }
    return SdioHost::CMD_CRC;
}
```

- [ ] **Step 3: Build the probe, run its QEMU gate**

```bash
cd ~/Development/rt1176-evkb-m2-maya-w161/examples/networking/m2_sdio_probe
cmake --build build && ./run_qemu.sh
```
Expected: build clean; `PASS: SDIO enumerate reached the cmd5-no-response fallback cleanly`

- [ ] **Step 4: Commit (M2Radio)**

```bash
cd ~/Development/M2Radio
git add iw416/Iw416.h iw416/Iw416.cpp
git commit -m "iw416: connectStation() -- scan/passphrase/associate/watch in one call"
```

---

### Task 3: netif glue — `M2Radio/lwip/Iw416Netif.{h,cpp}`

Compiled only when an example imports the `lwip` subdir of M2Radio alongside the lwip library (all imported include dirs accumulate on the shared `teensy_flags` INTERFACE target, so M2Radio sources see lwip headers). The build check happens in Task 4; this task writes the files.

**Files:**
- Create: `~/Development/M2Radio/lwip/Iw416Netif.h`
- Create: `~/Development/M2Radio/lwip/Iw416Netif.cpp`

- [ ] **Step 1: Write Iw416Netif.h**

```cpp
// lwip netif over the M2Radio IW416 driver (W9).  The Wi-Fi sibling of the
// lwip repo's port/ethernetif.c: same NO_SYS=1 poll-loop model, same MAC
// convention (the sketch fills the C-linkage g_mac[6] before netif_add).
//
// Usage (mirrors lwip_test.cpp):
//   netif_add(&nif, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4,
//             &iw416 /* state */, iw416NetifInit, ethernet_input);
//   ...
//   loop: if (!iw416NetifPoll(&nif)) { /* link dropped: reconnect, then
//            netif_set_link_up(&nif); } sys_check_timeouts();
#pragma once
#include "lwip/netif.h"
#include "lwip/err.h"

class Iw416;

// netif->state MUST be the Iw416* (netif_add's `state` argument).
err_t iw416NetifInit(struct netif *netif);

// One driver service pass: deliver every pending RX frame into lwip and
// record link events.  Returns false when the link dropped (the netif is
// marked link-down; the caller owns reconnect + netif_set_link_up).
bool iw416NetifPoll(struct netif *netif);
```

- [ ] **Step 2: Write Iw416Netif.cpp**

```cpp
#include <string.h>
#include "Iw416Netif.h"
#include "lwip/opt.h"
#include "lwip/pbuf.h"
#include "lwip/etharp.h"
#include "netif/ethernet.h"
#include "Iw416.h"

extern "C" unsigned char g_mac[6];   // filled by the sketch (getHwSpec MAC)

#define IW416IF_MAX_FRAME 1536
static uint8_t s_txbuf[IW416IF_MAX_FRAME];

static err_t low_level_output(struct netif *netif, struct pbuf *p) {
    Iw416 *iw = (Iw416 *)netif->state;
    if (p->tot_len > IW416IF_MAX_FRAME) return ERR_IF;
    pbuf_copy_partial(p, s_txbuf, p->tot_len, 0);
    return (iw->sendDataFrame(s_txbuf, (uint16_t)p->tot_len) == SdioHost::OK)
               ? ERR_OK : ERR_IF;
}

err_t iw416NetifInit(struct netif *netif) {
    netif->name[0] = 'w'; netif->name[1] = 'l';
    netif->output     = etharp_output;
    netif->linkoutput = low_level_output;
    netif->mtu        = 1500;
    netif->hwaddr_len = ETH_HWADDR_LEN;
    memcpy(netif->hwaddr, g_mac, ETH_HWADDR_LEN);
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;
    return ERR_OK;
}

static void frameSink(void *vctx, const uint8_t *frame, uint16_t len) {
    struct netif *nif = (struct netif *)vctx;
    struct pbuf *p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);
    if (p == NULL) return;                 // pool exhausted: drop this frame
    pbuf_take(p, frame, len);
    if (nif->input(p, nif) != ERR_OK) pbuf_free(p);
}

bool iw416NetifPoll(struct netif *netif) {
    Iw416 *iw = (Iw416 *)netif->state;
    bool dropped = false;
    (void)iw->serviceLink(frameSink, netif, &dropped, 0);
    if (dropped) { netif_set_link_down(netif); return false; }
    return true;
}
```

- [ ] **Step 3: Commit after Task 4's build proves it compiles** (no commit yet — Task 4 Step 6 commits both repos)

---

### Task 4: the example — `examples/networking/m2_lwip_test/`

**Files:**
- Create: `examples/networking/m2_lwip_test/CMakeLists.txt`
- Create: `examples/networking/m2_lwip_test/m2_lwip_test.cpp`

- [ ] **Step 1: Write CMakeLists.txt**

```cmake
cmake_minimum_required(VERSION 3.24)
project(m2_lwip_test)

if(NOT DEFINED TEENSY_VERSION)
    set(TEENSY_VERSION 117 CACHE STRING "")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

# lwip exactly as lwip_test imports it (port/ carries lwipopts.h + arch/;
# port/ethernetif.c also compiles but is inert here -- nothing calls it).
import_evkb_library(lwip
    src/include src/core src/core/ipv4 src/netif port port/arch)
# M2Radio: sdio + iw416 as the probe, PLUS the lwip netif glue subdir.
import_evkb_library(M2Radio sdio iw416 lwip)

# --- IW416 firmware blob (NOT vendored) -- same rules as m2_sdio_probe ------
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

# --- Wi-Fi credentials (never committed; live only in CMakeCache) -----------
set(M2RADIO_WIFI_SSID "" CACHE STRING "Target AP SSID")
set(M2RADIO_WIFI_PSK  "" CACHE STRING "Target AP WPA2 passphrase")
if(M2RADIO_WIFI_SSID)
    add_definitions(-DHAVE_WIFI_CREDS=1
                    -DM2_WIFI_SSID="${M2RADIO_WIFI_SSID}"
                    -DM2_WIFI_PSK="${M2RADIO_WIFI_PSK}")
endif()

teensy_add_executable(m2_lwip_test m2_lwip_test.cpp ${M2_FW_SRC})
teensy_target_link_libraries(m2_lwip_test cores M2Radio lwip)
target_link_libraries(m2_lwip_test.elf stdc++)
```

- [ ] **Step 2: Write m2_lwip_test.cpp**

```cpp
// W9: lwip over the IW416 M.2 Wi-Fi link.
//
// Hardware proof: DHCP via lwip's own client, then a raw-API TCP echo client
// against the ESP8266 oracle (192.168.4.1:4712) -- send "M2LWIP hello <n>",
// require the echo byte-exact.  Console: `lwip: ip=... tcp=tx/ok/fail ...`.
//
// QEMU proof (run_qemu.sh): the DEVICE-ABSENT path -- QEMU's SD memory card
// ignores CMD5, so the correct outcome is the cmd5-no-response fallback,
// lwip_probe_done, and a heartbeat.  Green in QEMU says nothing about Wi-Fi.
//
// The W5 monitor phase is deliberately absent: NET_MONITOR before ASSOCIATE
// kills this firmware's managed RX-to-host delivery (W8 erratum).
#include "Arduino.h"
#include "HardwareSerial.h"
#include "SdioHost.h"
#include "SdioFunc.h"
#include "Iw416.h"
#include "Iw416Netif.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/timeouts.h"
#include "lwip/dhcp.h"
#include "lwip/tcp.h"
#include "netif/ethernet.h"

extern "C" { unsigned char g_mac[6] = {0}; }   // Iw416Netif.cpp + ethernetif.c extern this

#if defined(HAVE_IW416_FW)
extern const uint8_t  iw416_fw[];
extern const uint32_t iw416_fw_len;
#endif

static SdioHost sdio;
static SdioFunc func(sdio, 1);
static Iw416 iw416(sdio, func);
static struct netif s_netif;

static bool     s_linkUp = false, s_lwipUp = false;
static uint32_t s_reconnects = 0;

// --- TCP echo client (raw API) ----------------------------------------------
static struct tcp_pcb *s_pcb = nullptr;
static uint32_t s_tcpTx = 0, s_tcpOk = 0, s_tcpFail = 0;
static char     s_expect[48];
static uint16_t s_expectLen = 0;
static bool     s_busy = false;
static uint32_t s_started = 0, s_lastKick = 0;

static err_t echoRecv(void *, struct tcp_pcb *pcb, struct pbuf *p, err_t) {
    if (p == nullptr) {                        // remote closed before data
        tcp_close(pcb); s_pcb = nullptr; s_busy = false; s_tcpFail++;
        return ERR_OK;
    }
    bool ok = (p->tot_len == s_expectLen) &&
              (pbuf_memcmp(p, 0, s_expect, s_expectLen) == 0);
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    if (ok) s_tcpOk++; else s_tcpFail++;
    tcp_close(pcb); s_pcb = nullptr; s_busy = false;
    return ERR_OK;
}
static err_t echoConnected(void *, struct tcp_pcb *pcb, err_t) {
    tcp_write(pcb, s_expect, s_expectLen, TCP_WRITE_FLAG_COPY);
    tcp_output(pcb);
    return ERR_OK;
}
static void echoErr(void *, err_t) {           // pcb already freed by lwip
    s_pcb = nullptr; s_busy = false; s_tcpFail++;
}
static void tcpKick() {
    if (s_busy) {
        if (millis() - s_started > 5000 && s_pcb) tcp_abort(s_pcb);  // -> echoErr
        return;
    }
    if (millis() - s_lastKick < 2000) return;
    s_lastKick = millis();
    ip4_addr_t dst; IP4_ADDR(&dst, 192, 168, 4, 1);
    s_pcb = tcp_new();
    if (s_pcb == nullptr) return;
    s_expectLen = (uint16_t)snprintf(s_expect, sizeof(s_expect),
                                     "M2LWIP hello %lu", (unsigned long)s_tcpTx);
    tcp_arg(s_pcb, nullptr);
    tcp_err(s_pcb, echoErr);
    tcp_recv(s_pcb, echoRecv);
    s_busy = true; s_started = millis(); s_tcpTx++;
    if (tcp_connect(s_pcb, &dst, 4712, echoConnected) != ERR_OK)
        tcp_abort(s_pcb);                      // -> echoErr does the bookkeeping
}

// --- bring-up ----------------------------------------------------------------
static SdioHost::Status s_sdioSt = SdioHost::CMD_TIMEOUT;
static SdioHost::Status s_iwSt   = SdioHost::CMD_TIMEOUT;
static bool s_haveCard = false;

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

static bool wifiConnect() {
#if defined(HAVE_WIFI_CREDS)
    SdioHost::Status c = iw416.connectStation(M2_WIFI_SSID, M2_WIFI_PSK);
    Serial1.print("connect="); Serial1.print(statusName(c));
    Serial1.print(" last_event=0x"); Serial1.println(iw416.lastEvent(), HEX);
    return c == SdioHost::OK;
#else
    return false;
#endif
}

void setup() {
    Serial1.begin(115200);
    delay(50);
    Serial1.println("RT1176 M.2 lwip test up");

    s_sdioSt = sdio.begin();
    Serial1.print("sdio_begin="); Serial1.print(statusName(s_sdioSt));
    Serial1.print(" rc="); Serial1.println((int)s_sdioSt);
    Serial1.print("int_status=0x"); Serial1.println(sdio.lastIntStatus(), HEX);

#if defined(HAVE_IW416_FW)
    if (s_sdioSt == SdioHost::OK) {
        s_iwSt = iw416.begin();
        if (s_iwSt == SdioHost::OK &&
            iw416.downloadFirmware(iw416_fw, iw416_fw_len) == SdioHost::OK) {
            (void)iw416.refreshIoPort();
            delay(50);
            (void)iw416.enableHostInt();
            uint32_t fwRel = 0; uint16_t hwVer = 0;
            if (iw416.getHwSpec(g_mac, &fwRel, &hwVer) == SdioHost::OK) {
                (void)iw416.reconfigureTxBuffers(2048);
                (void)iw416.macControl(Iw416::MAC_RX_ON | Iw416::MAC_TX_ON |
                                       Iw416::MAC_ETHERNETII | Iw416::MAC_RTS_CTS);
                (void)iw416.set11nCfg();
                (void)iw416.amsduAggrCtrl();
                s_haveCard = true;
            }
        }
    }
#endif

    if (s_haveCard && wifiConnect()) {
        s_linkUp = true;
        lwip_init();
        netif_add(&s_netif, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4,
                  &iw416, iw416NetifInit, ethernet_input);
        netif_set_default(&s_netif);
        netif_set_up(&s_netif);
        dhcp_start(&s_netif);
        s_lwipUp = true;
        Serial1.println("lwip_netif_up");
    }
    Serial1.println("lwip_probe_done");
}

void loop() {
    static uint32_t lastBeat = 0, lastStat = 0, beats = 0;

    if (s_lwipUp) {
        if (s_linkUp && !iw416NetifPoll(&s_netif)) {
            s_linkUp = false;
            dhcp_stop(&s_netif);
            Serial1.println("link_down");
        }
        if (!s_linkUp && wifiConnect()) {
            s_linkUp = true;
            s_reconnects++;
            netif_set_link_up(&s_netif);
            dhcp_start(&s_netif);
            Serial1.println("link_reup");
        }
        sys_check_timeouts();
        if (s_linkUp && dhcp_supplied_address(&s_netif)) tcpKick();

        if (millis() - lastStat >= 3000) {
            lastStat = millis();
            Serial1.print("lwip: ip=");
            Serial1.print(ip4addr_ntoa(netif_ip4_addr(&s_netif)));
            Serial1.print(" tcp="); Serial1.print(s_tcpTx);
            Serial1.print('/');     Serial1.print(s_tcpOk);
            Serial1.print('/');     Serial1.print(s_tcpFail);
            Serial1.print(" rx=");  Serial1.print(iw416.rxDataCount());
            Serial1.print(" tx=");  Serial1.print(iw416.dataTxCount());
            Serial1.print(" ring="); Serial1.print(iw416.txPort());
            Serial1.print('/');      Serial1.print(iw416.rxPort());
            Serial1.print('/');      Serial1.print(iw416.rxRingResyncs());
            Serial1.print(" reconnects="); Serial1.println(s_reconnects);
        }
    } else {
        // Fallback (QEMU / no card / no fw): prove the image stays alive.
        if (millis() - lastBeat >= 1000) {
            lastBeat = millis();
            Serial1.print("alive="); Serial1.println(++beats);
        }
    }
}
```

- [ ] **Step 3: Configure and build (fallback flavour — no fw, no creds)**

```bash
cd ~/Development/rt1176-evkb-m2-maya-w161/examples/networking/m2_lwip_test
cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake
cmake --build build
```
Expected: clean configure (both libraries resolved locally) and `[100%] Built target m2_lwip_test_hex`. This also compile-proves Task 3's glue.

- [ ] **Step 4: Reconfigure with firmware + creds (the hardware flavour)**

```bash
cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake \
  -DM2RADIO_IW416_FW=$HOME/Development/mcuxsdk-ws/mcuxsdk/components/conn_fwloader/fw_bin/inc/IW416/sduartIW416_wlan_bt.bin.inc \
  -DM2RADIO_WIFI_SSID=ESP8266TEST -DM2RADIO_WIFI_PSK=esptest12345
cmake --build build
```
Expected: `IW416 firmware: ...bin.inc` in the configure log; clean build. (Creds live only in the gitignored CMakeCache; the PSK is the bench ESP's throwaway.)

- [ ] **Step 5: Verify nothing credential-shaped is staged**

```bash
cd ~/Development/rt1176-evkb-m2-maya-w161
git status --porcelain examples/networking/m2_lwip_test
```
Expected: only `CMakeLists.txt` and `m2_lwip_test.cpp` appear (build/ is ignored).

- [ ] **Step 6: Commit both repos**

```bash
cd ~/Development/M2Radio
git add lwip/Iw416Netif.h lwip/Iw416Netif.cpp
git commit -m "lwip: Iw416Netif -- netif glue over serviceLink (NO_SYS poll model)"
cd ~/Development/rt1176-evkb-m2-maya-w161
git add examples/networking/m2_lwip_test/CMakeLists.txt examples/networking/m2_lwip_test/m2_lwip_test.cpp
git commit -m "networking: m2_lwip_test -- lwip DHCP + TCP echo client over the IW416 link"
```

---

### Task 5: QEMU gate + audit registration

**Files:**
- Create: `examples/networking/m2_lwip_test/run_qemu.sh` (mode 755)
- Modify: `tools/license-audit.sh` (the GATES list, ~line 300, networking block)

- [ ] **Step 1: Write run_qemu.sh**

```sh
#!/bin/sh
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/m2_lwip_test.elf"
OUT=$(gate_capture_path "$DIR" serial.uart)
rm -f "$OUT"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none $(gate_console "$OUT") \
    -d guest_errors -D "$(gate_capture_path "$DIR" serial.dbg)" &
P=$!; gate_pid $P
for _ in $(seq 1 40); do
    [ -f "$OUT" ] && grep -q "alive=2" "$OUT" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"
grep -q "RT1176 M.2 lwip test up" "$OUT" || { echo "FAIL: banner missing"; exit 1; }
# This gate asserts the DEVICE-ABSENT path: QEMU attaches an SD *memory*
# card, which ignores CMD5, so the correct outcome is the clean fallback --
# NOT a Wi-Fi bring-up.  The Wi-Fi + lwip + TCP proof lives on silicon in
# transcript_hw_evkb.txt.
grep -q "^sdio_begin=cmd5-no-response" "$OUT" || {
    echo "FAIL: expected the cmd5-no-response fallback"; exit 1; }
grep -q "^int_status=0x" "$OUT" || { echo "FAIL: no raw evidence line"; exit 1; }
grep -q "^lwip_probe_done" "$OUT" || { echo "FAIL: probe never completed"; exit 1; }
grep -q "^alive=2" "$OUT" || { echo "FAIL: no heartbeat after the probe"; exit 1; }
# The fallback must not claim a Wi-Fi link it cannot have.
if grep -q "^lwip_netif_up" "$OUT"; then
    echo "FAIL: claimed a netif with no card present"; exit 1
fi
echo "PASS: reached the cmd5-no-response fallback; lwip never claimed a link"
```

```bash
chmod +x examples/networking/m2_lwip_test/run_qemu.sh
```

- [ ] **Step 2: Run the gate; commit its transcript**

```bash
cd ~/Development/rt1176-evkb-m2-maya-w161/examples/networking/m2_lwip_test
./run_qemu.sh | tee /tmp/m2_lwip_gate.out
```
Expected: `PASS: reached the cmd5-no-response fallback; lwip never claimed a link`

Save the captured UART section as the committed QEMU transcript:

```bash
cp "$(./run_qemu.sh >/dev/null 2>&1; ls -t .gate/serial.uart 2>/dev/null || echo build/serial.uart)" transcript_qemu.txt 2>/dev/null || true
```

(If `gate_capture_path` places the capture elsewhere, copy from the path printed in the gate output. The file to commit is the raw UART capture of a passing run.)

- [ ] **Step 3: Register in the licence audit's GATES list**

In `tools/license-audit.sh`, in the networking block of the `GATES` list (alphabetical, next to `examples/networking/lwip_test:lwip_test`), add:

```
examples/networking/m2_lwip_test:m2_lwip_test \
```

- [ ] **Step 4: Run the licence audit**

```bash
cd ~/Development/rt1176-evkb-m2-maya-w161
./tools/license-audit.sh
```
Expected: `LICENSE-AUDIT: PASS` (lwip is BSD-3; M2Radio is MIT; the new entry's depfile walk must appear in the output).

- [ ] **Step 5: Commit**

```bash
git add examples/networking/m2_lwip_test/run_qemu.sh examples/networking/m2_lwip_test/transcript_qemu.txt tools/license-audit.sh
git commit -m "networking: m2_lwip_test QEMU gate (device-absent fallback) + audit entry"
```

---

### Task 6: ESP oracle — TCP echo server (session scratch, not committed)

**Files:**
- Modify: `<scratchpad>/esp_ap/esp_ap.ino` (the session-scratch oracle sketch)

- [ ] **Step 1: Add the echo server to the sketch**

Add with the other globals:

```cpp
WiFiServer echoServer(4712);
```

In `setup()`, after `udp.begin(4711);`:

```cpp
  echoServer.begin();
  echoServer.setNoDelay(true);
```

In `loop()`, after the broadcast block:

```cpp
  WiFiClient c = echoServer.accept();
  if (c) {
    uint32_t t0 = millis();
    while (c.connected() && !c.available() && millis() - t0 < 500) delay(1);
    uint8_t buf[128];
    int n = c.read(buf, sizeof(buf));
    if (n > 0) { c.write(buf, n); c.flush(); }
    delay(5);
    c.stop();
    Serial.printf("AP: TCP echo %d bytes\n", n);
  }
```

- [ ] **Step 2: Compile and upload**

```bash
cd <scratchpad>
arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2 esp_ap
arduino-cli upload --fqbn esp8266:esp8266:nodemcuv2 -p /dev/cu.usbserial-0001 esp_ap
```
Expected: `Hash of data verified.` The AP restarts as WPA2 "ESP8266TEST" with the echo server listening. (`ap_creds.h` defines `AP_PSK "esptest12345"`; never commit it anywhere.)

---

### Task 7: silicon verification + transcript

- [ ] **Step 1: Flash the example**

```bash
pkill LinkServer; pkill redlinkserv; pkill crt_emu_cm_redlink; sleep 2
/Applications/LinkServer_26.6.137/LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load \
  ~/Development/rt1176-evkb-m2-maya-w161/examples/networking/m2_lwip_test/build/m2_lwip_test.elf
```
(Plain `flash … load` only — `LinkServer run` programs at ~2 kB/s silently.)

- [ ] **Step 2: Capture both consoles for 3 minutes**

```bash
cd <scratchpad> && python3 dual_read.py 180 | grep -a -vE "AP alive=" | tee m2_lwip_hw.txt
```
Expected within ~30 s of boot: `connect=ok`, `lwip_netif_up`, then `lwip: ip=192.168.4.x tcp=N/N/0 ...` with tcp ok-count tracking tx-count, `ring=` cycling, `reconnects=0`; on the ESP side `AP: TCP echo 15 bytes` lines. The pass criterion: **≥ 30 byte-exact echoes, fail count 0**, over 3 minutes.

- [ ] **Step 3: Write `transcript_hw_evkb.txt`**

Create `examples/networking/m2_lwip_test/transcript_hw_evkb.txt` with: date, rig description (ESP WPA2 AP + echo server), the captured console excerpts (boot, netif up, a representative window of `lwip:` lines and ESP `TCP echo` lines), and the verdict line stating the echo count. Follow the m2_sdio_probe transcript's voice. Note the RX-death erratum explicitly: long sessions are expected to hit it; that investigation is tracked separately.

- [ ] **Step 4: Commit**

```bash
cd ~/Development/rt1176-evkb-m2-maya-w161
git add examples/networking/m2_lwip_test/transcript_hw_evkb.txt
git commit -m "networking: m2_lwip_test silicon transcript -- lwip DHCP + byte-exact TCP echoes"
```

---

### Task 8: push, pin bump, sweep, docs

- [ ] **Step 1: Push M2Radio and bump the pin**

```bash
cd ~/Development/M2Radio && git push origin master && git rev-parse HEAD
```

Edit `evkb.cmake`'s M2Radio line: replace the old SHA with the printed one (full 40 chars).

- [ ] **Step 2: Full QEMU sweep from the short path**

The four USB-monitor gates die on `sun_path` > 104 bytes from this checkout's long directory name; the sweep MUST run via the short symlink:

```bash
ln -sfn ~/Development/rt1176-evkb-m2-maya-w161 /tmp/ev
cd /tmp/ev && ./tools/run-all-qemu-gates.sh
```
Expected: `gates: 96 passed` exit 0 (95 + the new gate), or 95 passed with ONLY `rt1176:dualcore/cm4_audio_test` red (the documented nondeterministic gate — re-run it idle before believing a red). **0 SKIP is load-bearing**; a SKIP means an unbuilt gate-owning example.

- [ ] **Step 3: Update CLAUDE.md gate counts**

In `CLAUDE.md`'s sweep paragraph: 95 → 96 (`96 passed, 0 failed, 0 SKIP`, or `95 passed, 1 failed` with the nondeterministic gate red), and extend the gate-history parenthetical: "(95 before W9's lwip bridge added `networking/m2_lwip_test`; …)". Record the measured sweep date.

- [ ] **Step 4: Update the W9 handoff / write the W10 handoff**

Note in `docs/superpowers/handoff/`: W9 stack milestone done (lwip netif, TCP echoes), RX-death erratum status per the soak findings, and what W10 should pick up.

- [ ] **Step 5: Commit**

```bash
cd ~/Development/rt1176-evkb-m2-maya-w161
git add evkb.cmake CLAUDE.md docs/superpowers/handoff/
git commit -m "build: bump M2Radio pin (serviceLink/connectStation/Iw416Netif); sweep 96 green"
```

---

## Self-review (done at write time)

- **Spec coverage:** netif glue (Task 3), connectStation (Task 2), example + TCP proof (Tasks 4, 7), gates + audit (Task 5), ESP oracle (Task 6), error handling (link_down/reconnect path in Task 4's loop) — all spec sections have tasks. The spec's "drain pollLink() frames" is implemented via the sink-based `serviceLink()` (Task 1) because pollLink's one-frame contract drops stack traffic; the spec is amended alongside this plan.
- **Placeholders:** none; every code step carries the full text.
- **Type consistency:** `serviceLink(FrameSink, void*, bool*, uint32_t)` matches between Tasks 1 and 3; `connectStation(const char*, const char*, uint8_t)` between Tasks 2 and 4; `statusName`'s cases were checked against `SdioHost.h`'s actual enum (OK, NO_IO_FUNCTION, CMD_TIMEOUT, CMD_CRC, CLOCK_UNSTABLE, BAD_CIS, CMD5_NO_RESPONSE, INIT_CLK_STUCK) and the printed `cmd5-no-response` token matches what both gates grep.
```
