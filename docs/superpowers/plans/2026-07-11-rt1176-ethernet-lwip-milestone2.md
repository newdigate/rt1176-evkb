# RT1176 lwIP milestone 2 (ping + DHCP + UDP + TCP) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bring up the lwIP 2.2.1 TCP/IP stack (bare-metal `NO_SYS`) on the HW-verified RT1176 ENET foundation, so the board answers a ping through the real stack, pulls a DHCP lease, and echoes UDP + TCP.

**Architecture:** Vendor lwIP 2.2.1 as a local library (`~/Development/lwip`); bind it to the *frozen* core L2 driver `cores/imxrt1176/enet.c` via a small hand-rolled netif port (`port/ethernetif.c` + `lwipopts.h` + `cc.h`), `sys_now()`=`millis()`, copying glue. A single gate ELF (DHCP-with-static-fallback) is exercised by four phases: **ping** on our socket+peer path, **DHCP/UDP/TCP** on QEMU SLIRP (`-nic user` + `hostfwd`). The Arduino socket API is milestone 3.

**Tech Stack:** lwIP 2.2.1 raw API (`NO_SYS`), C/C++ (ARM GCC 10.2.1), QEMU `mimxrt1170-evk` `imx.enet` model + SLIRP, Python 3 gate peer, LinkServer + pyserial (HW).

**Spec:** `evkb/docs/superpowers/specs/2026-07-11-rt1176-ethernet-lwip-milestone2-design.md`

**Repos (commit to `master`; push ONLY when the user asks):**
- lwIP library: `~/Development/lwip` (**new local repo** — `git init` in Task 1; matches how `~/Development/{Audio,SPI,SdFat,FNET}` are local repos).
- Gate: `git -C ~/Development/rt1170/evkb` (local; `git add` ONLY the `lwip_test/` files each task touches, never `git add -A` — the tree is shared).
- Core `enet.c` is **FROZEN** — this milestone makes NO changes to `cores/imxrt1176/`.

**Global cautions:**
- Build the gate with `cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake`; run gates as `./run_qemu_lwip.sh <phase>` (NOT `sh` — `gate_init` execs `$0`).
- Adding the `lwip` library (new sources) triggers the CMake `file(GLOB)` trap: `rm -rf build` reconfigure in the gate dir.
- `import_arduino_library` is a **flat, non-recursive** GLOB — every source subdir must be enumerated, and only the copied files in each are compiled (so the selective vendor in Task 1 matters).
- lwIP contrib files (BSD-licensed) are copied verbatim where noted — keep their headers.

---

## File Structure

**Create — the vendored library `~/Development/lwip/`:**
- `src/` — selective copy of the SDK lwIP stack (Task 1): `src/core/*.c`, `src/core/ipv4/*.c`, **only** `src/netif/ethernet.c`, `src/include/**`.
- `port/lwipopts.h` — the `NO_SYS` config.
- `port/arch/cc.h` — compiler abstraction (copy the SDK's).
- `port/ethernetif.c` + `port/ethernetif.h` — the netif↔`enet.c` glue (5 functions + `sys_now`).

**Create — the gate `~/Development/rt1170/evkb/lwip_test/`:**
- `lwip_test.cpp` — the demo sketch (netif bring-up + `udpecho_raw_init`/`tcpecho_raw_init`).
- `udpecho_raw.c`/`.h`, `tcpecho_raw.c`/`.h` — copied verbatim from lwIP contrib (the echo servers, bind port 7).
- `run_qemu_lwip.sh` — the phase-selected runner.
- `lwip_peer.py` — the gate peer (ARP/ICMP frame injector for `ping`; UDP/TCP socket client for `udp`/`tcp`).
- `CMakeLists.txt`, `toolchain/rt1170-evkb.toolchain.cmake` — cloned from `enet_test/`.
- `HW-RESULTS.md` — Task 7.

---

## Task 1: Vendor lwIP 2.2.1 + author the port layer

**Files:** Create the `~/Development/lwip/` tree (above).

- [ ] **Step 1: Selectively copy the lwIP stack.**
```sh
SDK=~/Development/mcuxsdk-ws/mcuxsdk/middleware/lwip
mkdir -p ~/Development/lwip/src/core/ipv4 ~/Development/lwip/src/netif ~/Development/lwip/port/arch
cp -R "$SDK/src/include" ~/Development/lwip/src/include
cp "$SDK/src/core/"*.c   ~/Development/lwip/src/core/
cp "$SDK/src/core/ipv4/"*.c ~/Development/lwip/src/core/ipv4/
cp "$SDK/src/netif/ethernet.c" ~/Development/lwip/src/netif/    # ONLY ethernet.c
cp "$SDK/port/arch/cc.h" ~/Development/lwip/port/arch/cc.h
```
Verify: `ls ~/Development/lwip/src/netif/` shows **only** `ethernet.c` (NOT slipif/bridgeif/lowpan6/zepif). `ls ~/Development/lwip/src/core/*.c` shows `tcp.c udp.c raw.c pbuf.c netif.c timeouts.c inet_chksum.c mem.c memp.c init.c ...`.

- [ ] **Step 2: Write `~/Development/lwip/port/lwipopts.h`** (minimal NO_SYS config; cross-check against `$SDK/template/lwipopts.h` for any macro your lwIP build errors on):
```c
#ifndef LWIPOPTS_H
#define LWIPOPTS_H
#define NO_SYS                       1
#define SYS_LIGHTWEIGHT_PROT         0
#define LWIP_NETCONN                 0
#define LWIP_SOCKET                  0
#define LWIP_IPV4                    1
#define LWIP_IPV6                    0
#define LWIP_ARP                     1
#define LWIP_ETHERNET                1
#define LWIP_ICMP                    1
#define LWIP_RAW                     1
#define LWIP_UDP                     1
#define LWIP_TCP                     1
#define LWIP_DHCP                    1
#define LWIP_DNS                     0
#define LWIP_IGMP                    0
#define LWIP_AUTOIP                  0
#define LWIP_SINGLE_NETIF            1
#define LWIP_NETIF_STATUS_CALLBACK   0
#define LWIP_NETIF_LINK_CALLBACK     0
#define LWIP_STATS                   0
#define MEM_ALIGNMENT                4
#define MEM_SIZE                     (24 * 1024)
#define MEMP_NUM_PBUF                16
#define MEMP_NUM_UDP_PCB             4
#define MEMP_NUM_TCP_PCB             5
#define MEMP_NUM_TCP_PCB_LISTEN      2
#define MEMP_NUM_TCP_SEG             16
#define PBUF_POOL_SIZE               16
#define TCP_MSS                      1460
#define TCP_SND_BUF                  (6 * TCP_MSS)
#define TCP_WND                      (2 * TCP_MSS)
#define ETH_PAD_SIZE                 0
#define LWIP_NETIF_TX_SINGLE_PBUF    1
#define CHECKSUM_GEN_IP              1
#define CHECKSUM_GEN_UDP             1
#define CHECKSUM_GEN_TCP             1
#define CHECKSUM_GEN_ICMP            1
#define CHECKSUM_CHECK_IP            1
#define CHECKSUM_CHECK_UDP           1
#define CHECKSUM_CHECK_TCP           1
#define CHECKSUM_CHECK_ICMP          1
#endif /* LWIPOPTS_H */
```

- [ ] **Step 3: Write `~/Development/lwip/port/ethernetif.h`:**
```c
#ifndef PORT_ETHERNETIF_H
#define PORT_ETHERNETIF_H
#include "lwip/err.h"
#include "lwip/netif.h"
#ifdef __cplusplus
extern "C" {
#endif
err_t ethernetif_init(struct netif *netif);   /* pass to netif_add */
void  ethernetif_poll(struct netif *netif);    /* call every main-loop iteration */
#ifdef __cplusplus
}
#endif
#endif
```

- [ ] **Step 4: Write `~/Development/lwip/port/ethernetif.c`** (the glue; adapted from `$SDK/contrib/examples/ethernetif/ethernetif.c`, binding to `enet.c`):
```c
#include "lwip/opt.h"
#include "lwip/def.h"
#include "lwip/mem.h"
#include "lwip/pbuf.h"
#include "lwip/etharp.h"
#include "netif/ethernet.h"
#include "ethernetif.h"
#include "enet.h"      /* the frozen core L2 driver */

extern "C" unsigned long millis(void);   /* Teensy core 1ms tick */

static uint8_t s_txbuf[1536];
static uint8_t s_rxbuf[1536];

static err_t low_level_output(struct netif *netif, struct pbuf *p)
{
    (void)netif;
    if (p->tot_len > sizeof(s_txbuf)) return ERR_IF;
    pbuf_copy_partial(p, s_txbuf, p->tot_len, 0);
    return (enet_send_frame(s_txbuf, (uint16_t)p->tot_len) == 0) ? ERR_OK : ERR_IF;
}

static struct pbuf *low_level_input(struct netif *netif)
{
    (void)netif;
    uint16_t len = 0;
    if (enet_read_frame(s_rxbuf, &len) != 1) return NULL;   /* 1=got, 0=none, -1=drop */
    struct pbuf *p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);
    if (p != NULL) pbuf_take(p, s_rxbuf, len);
    return p;    /* NULL on pool-exhaust: frame dropped, MAC BD already re-armed */
}

err_t ethernetif_init(struct netif *netif)
{
    netif->name[0] = 'e'; netif->name[1] = '0';
    netif->output     = etharp_output;
    netif->linkoutput = low_level_output;
    netif->mtu        = 1500;
    netif->hwaddr_len = ETH_HWADDR_LEN;
    /* MAC was placed in netif->hwaddr by netif_add's state/caller; init the MAC with it. */
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;
    enet_init(netif->hwaddr);
    return ERR_OK;
}

void ethernetif_poll(struct netif *netif)
{
    struct pbuf *p;
    while ((p = low_level_input(netif)) != NULL) {
        if (netif->input(p, netif) != ERR_OK) pbuf_free(p);
    }
}

extern "C" u32_t sys_now(void) { return (u32_t)millis(); }
```
**Note on the MAC address:** `netif_add(&netif, ip, mask, gw, STATE, ethernetif_init, ethernet_input)` — set the 6-byte MAC into `netif->hwaddr` *before* `enet_init` uses it. The clean way: the sketch memcpys the MAC into `netif.hwaddr` right after `netif_add` returns but before the first poll, OR `ethernetif_init` copies from a global. Simplest (used here): a global `extern uint8_t g_mac[6];` the sketch sets, and `ethernetif_init` does `memcpy(netif->hwaddr, g_mac, 6)` before `enet_init(netif->hwaddr)`. Add that (declare `g_mac` in `ethernetif.c` as `extern`, define + fill in the sketch).

- [ ] **Step 5: `git init` + commit the library.**
```sh
cd ~/Development/lwip && git init -q && printf 'build/\n' > .gitignore
git add -A && git commit -q -m "Vendor lwIP 2.2.1 (selective) + NO_SYS port for RT1176 enet.c"
```
(This is the one place `git add -A` is fine — it's a fresh dedicated repo, not the shared evkb tree.) Verify: `git -C ~/Development/lwip log --oneline` shows the commit; `git -C ~/Development/lwip status` clean.

---

## Task 2: Gate scaffold — the vendored lwIP + port must COMPILE and LINK

This is the highest-risk integration step: does ~10k LOC of lwIP + our hand-rolled port compile with ARM GCC 10.2.1 and link into an ELF, and does the netif come up? Acceptance = a booting ELF that prints `LWIP_NETIF_UP`.

**Files:** Create `evkb/lwip_test/{CMakeLists.txt,toolchain/,lwip_test.cpp,run_qemu_lwip.sh,lwip_peer.py}` + copy the echo servers.

- [ ] **Step 1: Clone the harness + copy the echo servers.**
```sh
mkdir -p ~/Development/rt1170/evkb/lwip_test/toolchain
cp ~/Development/rt1170/evkb/enet_test/toolchain/rt1170-evkb.toolchain.cmake ~/Development/rt1170/evkb/lwip_test/toolchain/
SDK=~/Development/mcuxsdk-ws/mcuxsdk/middleware/lwip
cp "$SDK/contrib/apps/udpecho_raw/udpecho_raw."[ch] ~/Development/rt1170/evkb/lwip_test/
cp "$SDK/contrib/apps/tcpecho_raw/tcpecho_raw."[ch] ~/Development/rt1170/evkb/lwip_test/
```

- [ ] **Step 2: `CMakeLists.txt`** (clone `enet_test`'s; add the lwIP import + the echo sources):
```cmake
cmake_minimum_required(VERSION 3.24)
project(lwip_test)
set(TEENSY_VERSION 117 CACHE STRING "")
include(FetchContent)
FetchContent_Declare(teensy_cmake_macros SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../teensy-cmake-macros)
FetchContent_MakeAvailable(teensy_cmake_macros)
include(${teensy_cmake_macros_SOURCE_DIR}/CMakeLists.include.txt)
import_arduino_library(cores ${CMAKE_CURRENT_LIST_DIR}/../cores/imxrt1176)
import_arduino_library(lwip $ENV{HOME}/Development/lwip
    src/include src/core src/core/ipv4 src/netif port port/arch)
teensy_add_executable(lwip_test lwip_test.cpp udpecho_raw.c tcpecho_raw.c)
teensy_target_link_libraries(lwip_test cores lwip)
target_link_libraries(lwip_test.elf stdc++)
```
(If `teensy_add_executable` rejects multiple sources, add the echo `.c` via `import_arduino_library(lwipapps ${CMAKE_CURRENT_LIST_DIR})` listing the gate dir — but try the multi-source form first.)

- [ ] **Step 3: Skeleton sketch `lwip_test.cpp`** (netif up with a static IP for now; prints `LWIP_NETIF_UP`):
```cpp
#include "Arduino.h"
#include "HardwareSerial.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"
#include "ethernetif.h"

uint8_t g_mac[6] = {0x02,0x00,0x00,0x00,0x00,0x01};   /* ethernetif_init copies this into netif->hwaddr */
static struct netif s_netif;

void setup() {
    Serial1.begin(115200); delay(50);
    Serial1.println("LWIP_BOOT");
    lwip_init();
    ip4_addr_t ip, mask, gw;
    IP4_ADDR(&ip, 192,168,1,50); IP4_ADDR(&mask, 255,255,255,0); IP4_ADDR(&gw, 192,168,1,1);
    netif_add(&s_netif, &ip, &mask, &gw, NULL, ethernetif_init, ethernet_input);
    netif_set_default(&s_netif);
    netif_set_up(&s_netif);
    Serial1.println("LWIP_NETIF_UP");
}
void loop() {
    ethernetif_poll(&s_netif);
    sys_check_timeouts();
}
```

- [ ] **Step 4: Runner `run_qemu_lwip.sh`** (phase-selected; `ping` = socket+peer, else SLIRP). Adapt from `enet_test/run_qemu_enet.sh`:
```sh
#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/lwip_test.elf"; VCOM="$DIR/vcom.uart"; DBG="$DIR/lwip.dbg"; RES="$DIR/lwip.result"
gate_tmp "$RES"; PORT=15600; PHASE="${1:-boot}"
rm -f "$VCOM" "$DBG" "$RES"
case "$PHASE" in
  ping)  NIC="-nic socket,listen=127.0.0.1:$PORT,model=imx.enet" ;;
  dhcp)  NIC="-nic user,model=imx.enet" ;;
  udp)   NIC="-nic user,model=imx.enet,hostfwd=udp::5556-:7" ;;
  tcp)   NIC="-nic user,model=imx.enet,hostfwd=tcp::5555-:7" ;;
  *)     NIC="-nic user,model=imx.enet" ;;
esac
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$VCOM" $NIC -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
if [ "$PHASE" = ping ] || [ "$PHASE" = udp ] || [ "$PHASE" = tcp ]; then
    python3 "$DIR/lwip_peer.py" "$PHASE" 127.0.0.1 "$PORT" > "$RES" 2>&1; RC=$?
else
    sleep 8; RC=0   # dhcp/boot: no peer, just let it run and grep VCOM
fi
sleep 1; kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== VCOM ===="; cat "$VCOM" 2>/dev/null; echo "==== peer ===="; cat "$RES" 2>/dev/null || true
grep -q "LWIP_NETIF_UP" "$VCOM" || { echo "FAIL: netif did not come up"; exit 1; }
[ $RC -eq 0 ] || { echo "FAIL: peer rc=$RC"; exit 1; }
echo "PASS: lwip_test $PHASE"
```
`chmod +x run_qemu_lwip.sh`. (Later tasks add per-phase `grep` checks before the final PASS.)

- [ ] **Step 5: Skeleton `lwip_peer.py`** (phase dispatcher; boot/ping use raw frames over the socket netdev, udp/tcp use SLIRP sockets):
```python
#!/usr/bin/env python3
import socket, sys, struct, time
def recvall(s,n):
    b=b""
    while len(b)<n:
        c=s.recv(n-len(b))
        if not c: raise EOFError
        b+=c
    return b
if __name__=="__main__":
    phase=sys.argv[1]
    if phase in ("ping",):      # socket netdev, raw ethernet frames (filled in Task 3)
        host,port=sys.argv[2],int(sys.argv[3]); print("peer ping (Task 3)"); sys.exit(0)
    sys.exit(0)
```

- [ ] **Step 6: Build (GLOB reconfigure) + run — verify COMPILE + LINK + netif up.**
```sh
cd ~/Development/rt1170/evkb/lwip_test && rm -rf build && cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake 2>&1 | tail -3 && cmake --build build 2>&1 | tail -20
./run_qemu_lwip.sh boot
```
Expected: **it compiles + links** (the integration milestone) → `build/lwip_test.elf`; the gate prints `LWIP_BOOT`, `LWIP_NETIF_UP`, `PASS`. **If compilation fails**, that's the real work of this task — resolve include paths (`port`/`port/arch` before `src/include`), missing `lwipopts.h` macros (add from `$SDK/template/lwipopts.h`), or C/C++ linkage (the `extern "C"` around `sys_now`/`millis`). Report the first error class if blocked.

- [ ] **Step 7: Commit (gate repo).**
```sh
git -C ~/Development/rt1170/evkb add lwip_test/CMakeLists.txt lwip_test/toolchain lwip_test/lwip_test.cpp lwip_test/run_qemu_lwip.sh lwip_test/lwip_peer.py lwip_test/udpecho_raw.c lwip_test/udpecho_raw.h lwip_test/tcpecho_raw.c lwip_test/tcpecho_raw.h
git -C ~/Development/rt1170/evkb commit -m "lwip_test: gate scaffold; lwIP+port compiles/links, netif up"
```

---

## Task 3: Ping gate — lwIP answers a ping through the real stack

**Files:** Modify `lwip_test/lwip_peer.py` (ping phase), `lwip_test/run_qemu_lwip.sh` (ping check). The sketch already brings the netif up with a static IP `192.168.1.50` — lwIP auto-answers ARP + ICMP; no sketch change needed beyond Task 2.

- [ ] **Step 1: Fill the ping phase in `lwip_peer.py`** — reuse `enet_test/enet_peer.py`'s ARP+ICMP frame injector over the socket netdev (4-byte-BE-length framing), targeting `192.168.1.50` (the static IP). Copy the `send_frame`/`recv_frame`/`cksum` helpers and the ARP-request + ICMP-echo build/assert from `enet_test/enet_peer.py`'s `ping` phase verbatim (board MAC `02:00:00:00:00:01`, board IP `192.168.1.50`, peer `02:..:02`/`192.168.1.1`). Assert: ARP reply (sender = board MAC/IP) + ICMP echo reply (type 0, valid cksum, payload echoed). Exit 0 on both.

- [ ] **Step 2: Wire the runner's ping check.** In `run_qemu_lwip.sh`, for `PHASE=ping`, the peer `RC` already gates it (peer asserts the echo). No VCOM token needed (lwIP auto-answers; the peer's exact-bytes assertion is the proof).

- [ ] **Step 3: Run — verify FAIL-first then PASS.**
```sh
cd ~/Development/rt1170/evkb/lwip_test && cmake --build build 2>&1 | tail -1
./run_qemu_lwip.sh ping
```
Expected: peer prints `ARP-REPLY ok=True` + `ICMP-REPLY ok=True type=0`; runner `PASS: lwip_test ping`. (Fail-first: if the `ethernetif` TX/RX glue is wrong — e.g. the pbuf copy or the `enet_read_frame` binding — the peer times out. This is the first exercise of the glue's data path; Task 2 only proved compile+link+init.)

- [ ] **Step 4: Commit.**
```sh
git -C ~/Development/rt1170/evkb add lwip_test/lwip_peer.py lwip_test/run_qemu_lwip.sh
git -C ~/Development/rt1170/evkb commit -m "lwip_test: Gate ping (lwIP auto ARP+ICMP answers a ping)"
```

---

## Task 4: DHCP gate — real lease from SLIRP + static fallback

**Files:** Modify `lwip_test/lwip_test.cpp` (DHCP-with-fallback), `run_qemu_lwip.sh` (dhcp check).

- [ ] **Step 1: Add DHCP-with-static-fallback to the sketch.** Replace the static-IP `netif_add` + `netif_set_up` with:
```cpp
    #include "lwip/dhcp.h"
    ...
    netif_add(&s_netif, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4, NULL,
              ethernetif_init, ethernet_input);   /* address via DHCP */
    netif_set_default(&s_netif); netif_set_up(&s_netif);
    dhcp_start(&s_netif);
    Serial1.println("LWIP_NETIF_UP");
```
And in `loop()`, after `sys_check_timeouts()`, add the lease report + fallback:
```cpp
    static bool leased=false, fell_back=false; static uint32_t t0=millis();
    if (!leased && dhcp_supplied_address(&s_netif)) {
        leased=true;
        Serial1.print("DHCP_OK ip="); Serial1.println(ip4addr_ntoa(netif_ip4_addr(&s_netif)));
    }
    if (!leased && !fell_back && (millis()-t0) > 5000) {   /* no DHCP server (ping phase) -> static */
        fell_back=true; dhcp_stop(&s_netif);
        ip4_addr_t ip,mask,gw; IP4_ADDR(&ip,192,168,1,50); IP4_ADDR(&mask,255,255,255,0); IP4_ADDR(&gw,192,168,1,1);
        netif_set_addr(&s_netif,&ip,&mask,&gw);
        Serial1.println("LWIP_STATIC_FALLBACK ip=192.168.1.50");
    }
```
(This preserves the Task-3 ping gate: on the socket+peer path there's no DHCP server, so after 5 s it falls back to `192.168.1.50` and the peer's ping still works.)

- [ ] **Step 2: Wire the runner's dhcp check.** In `run_qemu_lwip.sh`, for `PHASE=dhcp`, require `grep -q "DHCP_OK ip=10.0.2.15" "$VCOM"`. (The `dhcp` phase already runs `-nic user` with an 8 s sleep — enough for DORA.)

- [ ] **Step 3: Reconfigure not needed (sketch edit only), rebuild + run — verify.**
```sh
cd ~/Development/rt1170/evkb/lwip_test && cmake --build build 2>&1 | tail -1
./run_qemu_lwip.sh dhcp    # first smoke-test of -nic user + imx.enet
./run_qemu_lwip.sh ping    # regression: static fallback still answers the peer's ping
```
Expected: `dhcp` → VCOM `DHCP_OK ip=10.0.2.15`, `PASS`. `ping` → still `PASS` (fallback). **If `-nic user,model=imx.enet` errors at QEMU startup or no lease arrives:** run the raw QEMU line by hand to read stderr; the SLIRP+imx.enet pairing is unexercised (a QEMU reconciliation, not lwIP) — report the exact error.

- [ ] **Step 4: Commit.**
```sh
git -C ~/Development/rt1170/evkb add lwip_test/lwip_test.cpp lwip_test/run_qemu_lwip.sh
git -C ~/Development/rt1170/evkb commit -m "lwip_test: Gate DHCP (lease 10.0.2.15) + static fallback"
```

---

## Task 5: UDP echo gate

**Files:** Modify `lwip_test/lwip_test.cpp` (call `udpecho_raw_init`), `lwip_peer.py` (udp phase), `run_qemu_lwip.sh` (udp check). The echo server `udpecho_raw.c` (copied in Task 2, binds UDP port 7) is unmodified.

- [ ] **Step 1: Start the UDP echo server.** In `lwip_test.cpp` `setup()`, after `dhcp_start`, add:
```cpp
    extern "C" void udpecho_raw_init(void);
    udpecho_raw_init();     /* raw-API UDP echo on port 7 */
```

- [ ] **Step 2: Add the `udp` phase to `lwip_peer.py`** (SLIRP hostfwd — a normal UDP socket to 127.0.0.1:5556 → guest :7):
```python
    if phase == "udp":
        host, hport = sys.argv[2], 5556
        time.sleep(6)     # let DHCP lease + servers come up
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.settimeout(5)
        msg = b"LWIP-UDP-ECHO-PROBE"
        s.sendto(msg, (host, hport))
        try: got,_ = s.recvfrom(1024)
        except socket.timeout: print("FAIL: no UDP echo"); sys.exit(1)
        print("UDP got=%r" % got); sys.exit(0 if got == msg else 1)
```

- [ ] **Step 3: Wire the runner's udp check.** The `udp` phase already runs `-nic user,...,hostfwd=udp::5556-:7` + the peer. Peer `RC` gates it. (Optionally also `grep DHCP_OK`.)

- [ ] **Step 4: Rebuild + run.**
```sh
cd ~/Development/rt1170/evkb/lwip_test && cmake --build build 2>&1 | tail -1
./run_qemu_lwip.sh udp
```
Expected: peer `UDP got=b'LWIP-UDP-ECHO-PROBE'`, runner `PASS`. (Fail-first: before Step 1's `udpecho_raw_init`, nothing listens on :7 → SLIRP has no one to forward to → peer times out.)

- [ ] **Step 5: Commit.**
```sh
git -C ~/Development/rt1170/evkb add lwip_test/lwip_test.cpp lwip_test/lwip_peer.py lwip_test/run_qemu_lwip.sh
git -C ~/Development/rt1170/evkb commit -m "lwip_test: Gate UDP echo (raw-API server, SLIRP hostfwd)"
```

---

## Task 6: TCP echo gate

**Files:** Modify `lwip_test/lwip_test.cpp` (call `tcpecho_raw_init`), `lwip_peer.py` (tcp phase), `run_qemu_lwip.sh` (tcp check). `tcpecho_raw.c` (copied Task 2, binds TCP port 7) is unmodified.

- [ ] **Step 1: Start the TCP echo server.** In `lwip_test.cpp` `setup()`, after `udpecho_raw_init`, add:
```cpp
    extern "C" void tcpecho_raw_init(void);
    tcpecho_raw_init();     /* raw-API TCP echo on port 7 */
```

- [ ] **Step 2: Add the `tcp` phase to `lwip_peer.py`** (SLIRP hostfwd — a normal TCP socket to 127.0.0.1:5555 → guest :7):
```python
    if phase == "tcp":
        host, hport = sys.argv[2], 5555
        time.sleep(6)
        s = socket.create_connection((host, hport), timeout=6); s.settimeout(6)
        msg = b"LWIP-TCP-ECHO-PROBE"
        s.sendall(msg)
        try: got = recvall(s, len(msg))
        except (EOFError, socket.timeout): print("FAIL: no TCP echo"); sys.exit(1)
        print("TCP got=%r" % got); s.close(); sys.exit(0 if got == msg else 1)
```

- [ ] **Step 3: Rebuild + run + full regression.**
```sh
cd ~/Development/rt1170/evkb/lwip_test && cmake --build build 2>&1 | tail -1
for p in ping dhcp udp tcp; do echo "== $p =="; ./run_qemu_lwip.sh $p 2>&1 | tail -2; done
```
Expected: `tcp` → peer `TCP got=b'LWIP-TCP-ECHO-PROBE'`, `PASS`; all four phases PASS. (Fail-first: before Step 1, no TCP listener → connection refused/timeout.)

- [ ] **Step 4: Commit.**
```sh
git -C ~/Development/rt1170/evkb add lwip_test/lwip_test.cpp lwip_test/lwip_peer.py lwip_test/run_qemu_lwip.sh
git -C ~/Development/rt1170/evkb commit -m "lwip_test: Gate TCP echo (raw-API server, SLIRP hostfwd)"
```

---

## Task 7: Hardware verification

QEMU proved the stack + glue; silicon proves it over the real ENET + a real DHCP server. Interactive bench step (needs the board on a real RJ45 + a LAN host).

**Files:** Create `evkb/lwip_test/HW-RESULTS.md`.

- [ ] **Step 1: Pre-flight** (same as v1 ENET, per [[rt1176-ethernet-enet]]): cable in the **10/100 RJ45** (not 1G); **remove the SD card** (AD_32=MDC↔SD1_CD_B REVC); the LAN has a DHCP server (the router).

- [ ] **Step 2: Flash + capture the lease.**
```sh
pkill -9 -f LinkServer; pkill -9 -f redlinkserv
/Applications/LinkServer_26.6.137/LinkServer run MIMXRT1176:MIMXRT1170-EVKB ~/Development/rt1170/evkb/lwip_test/build/lwip_test.elf
```
Read VCOM `/dev/cu.usbmodem5DQ2DDHVWO5EI3` @115200 with pyserial (reader first). Expect `LWIP_NETIF_UP` then `DHCP_OK ip=<real router lease>` — **parse that IP** (dynamic; do not assume).

- [ ] **Step 3: Exercise the stack from a LAN host** (using the parsed lease `<IP>`):
```sh
ping -c 4 <IP>                                   # lwIP ICMP
echo LWIP-UDP | nc -u -w1 <IP> 7                 # UDP echo (port 7)
echo LWIP-TCP | nc -w2 <IP> 7                    # TCP echo (port 7)
```
Expect: ping replies; both `nc` commands echo the line back. **Success = DHCP lease + ping + UDP echo + TCP echo on real silicon.**

- [ ] **Step 4: Record + commit + update memory.** Write `evkb/lwip_test/HW-RESULTS.md` (the lease, the ping/UDP/TCP outputs, any QEMU-vs-silicon delta). Commit to evkb. Update the memory note `rt1176-ethernet-enet` (or a new `rt1176-lwip` note) with the milestone-2 result (lwIP stack HW-verified; the vendored-lib + hand-rolled-port pattern; DHCP/UDP/TCP work) linked from `MEMORY.md`.

---

## Self-Review

**Spec coverage:** vendored lwIP lib + selective copy → Task 1; the port (lwipopts/cc.h/ethernetif.c + sys_now) → Task 1; COMPILE+LINK integration → Task 2; ping (socket+peer) → Task 3; DHCP (SLIRP, static fallback) → Task 4; UDP echo → Task 5; TCP echo → Task 6; HW → Task 7. The `import_arduino_library` enumerated-subdirs + GLOB reconfigure, the `-nic user`+`imx.enet` smoke-test, the frozen `enet.c`, and the milestone-3 boundary (no Arduino socket API) all appear. Every spec section maps to a task. ✓

**Placeholder scan:** The `lwipopts.h`, `ethernetif.c`, the runner, the peer phases, and the sketch are given complete. The echo servers are copied verbatim from named SDK contrib paths (not placeholders). The "if compilation fails, resolve include paths / add lwipopts macros" guidance in Task 2 Step 6 is a real troubleshooting branch for the known integration risk, not a TODO. `HW-RESULTS.md` content is enumerated. ✓

**Type/name consistency:** `ethernetif_init`/`ethernetif_poll` (Task 1 header) are used verbatim in the sketch (Tasks 2–6). `g_mac`/`s_netif` consistent. `enet_send_frame`/`enet_read_frame` signatures match `enet.h` (the frozen driver). Echo ports: servers bind **7**; `hostfwd` maps host `5555`(tcp)/`5556`(udp) → guest `7`; the peer connects to `5555`/`5556` — consistent across runner + peer. Tokens `LWIP_BOOT`/`LWIP_NETIF_UP`/`DHCP_OK ip=`/`LWIP_STATIC_FALLBACK` match between the sketch prints and the runner greps. Board MAC `02:00:00:00:00:01` + IP `192.168.1.50` consistent between the sketch, the fallback, and the ping peer. ✓
