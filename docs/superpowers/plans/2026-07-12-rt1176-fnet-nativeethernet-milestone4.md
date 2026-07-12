# RT1176 Ethernet Milestone 4 — FNET + NativeEthernet Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Arduino `Ethernet` socket API (EthernetClient/Server/UDP + EthernetClass, DHCP+static, DNS) on the MIMXRT1170-EVKB via FNET (Apache-2.0) + NativeEthernet (MIT) — 5 QEMU gates green, then HW-verified.

**Architecture:** Native FNET port (approved design, spec `docs/superpowers/specs/2026-07-12-rt1176-fnet-nativeethernet-milestone4-design.md`): keep FNET's generic `fnet_fec.c` FEC datapath (interrupt-driven RX), add a `FNET_CFG_CPU_MIMXRT1176` target, and transplant the HW-verified board bring-up from `cores/imxrt1176/enet.c` into self-contained board hooks in `fnet_mimxrt_eth.c`. NativeEthernet gets zero logic changes — only vendored Arduino base classes. `enet.c` and the core stay frozen.

**Tech Stack:** C/C++ Arduino-style bare-metal, CMake + teensy-cmake-macros (`import_arduino_library` flat globs), QEMU `qemu2` fork (`imx.enet`, SLIRP), LinkServer + pyserial for HW.

---

## Context for implementers (read first)

- **Repos:**
  - `~/Development/FNET` — FNET fork, branch `master`, `git add -A` OK. Apache-2.0. **ISO-8859/CRLF files — always use `grep -a`; preserve CRLF line endings when editing existing files** (new files may be plain LF).
  - `~/Development/NativeEthernet` — NativeEthernet fork, branch `master`, `git add -A` OK. MIT.
  - `~/Development/rt1170/evkb` — gate harness repo, branch `master`. **NEVER `git add -A`** (shared tree, nested `cores/` repo) — stage only the exact paths named in each task. `cores/imxrt1176/` is FROZEN this milestone.
- **Build a gate:** from the gate dir: `rm -rf build && cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake && cmake --build build`. Always `rm -rf build` after library/config changes (flat globs don't re-glob).
- **Run a gate:** `./run_qemu_native_ethernet.sh <phase>` (NOT `sh ...`). Phases: `boot server udp client dns`.
- **Key RT1176 facts (already verified — do not re-derive):** 10/100 ENET base `0x40424000`, IRQ **137** → vector **153**; RTL8201 PHY at MDIO addr **3**; RMII ref-clk = SysPll1Div2÷10 = 50 MHz (SysPll1 is OFF at boot, AI-protocol bring-up required — HW-only, QEMU no-op); bus clock 240 MHz → MDC divider (MII_SPEED) **47**; D-cache **off** in this core; DMA buffers must be OCRAM (`.dmabuffers` = the core's `DMAMEM` section) per ERR050396; `__IMXRT1176__` is defined by teensy-cmake-macros (`CMakeLists.include.txt:98`).
- **QEMU model:** `imx.enet` already at the right base/IRQ with `FSL_IMXRT1170_ENET_PHY_NUM=3` (patched in milestone 1). FNET's DBSWP-clear + software-byte-swap convention matches the model. QEMU has no CCM/IOMUXC/ANADIG models — clock/pinmux/PHY-reset writes are no-ops there; the FEC datapath + interrupts ARE modeled.
- **Reference driver:** `~/Development/rt1170/evkb/cores/imxrt1176/enet.c` is the HW-verified source of truth for every transplanted sequence. Read it before Task 4.

---

### Task 1: Vendor Arduino base classes into NativeEthernet

NativeEthernet expects `Client.h`, `Server.h`, `Udp.h`, `IPAddress` from the core; our core doesn't provide them (they live vendored inside the lwIP `~/Development/Ethernet` lib). Copy the same files in.

**Files:**
- Create: `~/Development/NativeEthernet/src/Client.h`, `Server.h`, `Udp.h`, `IPAddress.h`, `IPAddress.cpp` (byte-copies)
- Modify: `~/Development/NativeEthernet/library.json` (platforms), `~/Development/NativeEthernet/README.md` (RT1176 + license inventory)

- [ ] **Step 1: Copy the five files**

```bash
cd ~/Development/NativeEthernet/src
cp ~/Development/Ethernet/src/Client.h ~/Development/Ethernet/src/Server.h \
   ~/Development/Ethernet/src/Udp.h ~/Development/Ethernet/src/IPAddress.h \
   ~/Development/Ethernet/src/IPAddress.cpp .
```

- [ ] **Step 2: Verify they are byte-identical to the source of copy**

Run: `for f in Client.h Server.h Udp.h IPAddress.h IPAddress.cpp; do cmp ~/Development/NativeEthernet/src/$f ~/Development/Ethernet/src/$f && echo "$f OK"; done`
Expected: five `OK` lines.

- [ ] **Step 3: Widen library.json platforms**

In `~/Development/NativeEthernet/library.json` change the line `"platforms": "teensy"` to `"platforms": "*"`.

- [ ] **Step 4: Append an RT1176 + licensing section to README.md**

Append to `~/Development/NativeEthernet/README.md`:

```markdown

## i.MX RT1176 / NXP MIMXRT1170-EVKB

This fork runs on the RT1176 (MIMXRT1170-EVKB, 10/100 ENET + RTL8201 PHY) via the
RT1176 CPU target in the companion FNET fork (https://github.com/newdigate/FNET).
No board-specific code lives in this library; the Arduino base classes
(`Client.h`, `Server.h`, `Udp.h`, `IPAddress.{h,cpp}`) are vendored into `src/`
for cores that do not provide them.

## Licensing inventory

The project license is MIT (© 2020 Tino Hernandez, see LICENSE). Individual files
carry their own upstream headers, preserved unmodified:
- `NativeDns.{h,cpp}` — Apache License 2.0 (© 2009-2010 MCQN Ltd)
- `src/utility/NativeW5100.h` — GPLv2 / LGPLv2.1 (© 2018 Paul Stoffregen, © 2010 Cristian Maglie; constants-only header)
- Several sources © 2018 Paul Stoffregen / © 2008 Bjoern Hartmann (MIT-style)
- Vendored base classes keep their upstream headers
```

- [ ] **Step 5: Commit**

```bash
cd ~/Development/NativeEthernet && git add -A && git commit -m "feat(rt1176): vendor Arduino base classes (Client/Server/Udp/IPAddress); widen platforms; document RT1176 target + license inventory"
```

---

### Task 2: Gate harness scaffold (RED — build must fail on FNET CPU select)

Clone the milestone-3 harness shape into a new `native_ethernet_test/` gate. The build is EXPECTED TO FAIL because FNET has no RT1176 target yet — that failure is this task's "failing test".

**Files:**
- Create: `~/Development/rt1170/evkb/native_ethernet_test/CMakeLists.txt`
- Create: `~/Development/rt1170/evkb/native_ethernet_test/native_ethernet_test.cpp`
- Create: `~/Development/rt1170/evkb/native_ethernet_test/run_qemu_native_ethernet.sh` (executable)
- Copy: `ethernet_peer.py`, `guestfwd_echo.py`, `toolchain/` from `../ethernet_test/`

- [ ] **Step 1: Create the directory and copy shared pieces**

```bash
cd ~/Development/rt1170/evkb
mkdir -p native_ethernet_test
cp ethernet_test/ethernet_peer.py ethernet_test/guestfwd_echo.py native_ethernet_test/
cp -r ethernet_test/toolchain native_ethernet_test/
```

- [ ] **Step 2: Write `native_ethernet_test/CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.24)
project(native_ethernet_test)
set(TEENSY_VERSION 117 CACHE STRING "")
include(FetchContent)
FetchContent_Declare(teensy_cmake_macros SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../teensy-cmake-macros)
FetchContent_MakeAvailable(teensy_cmake_macros)
include(${teensy_cmake_macros_SOURCE_DIR}/CMakeLists.include.txt)
import_arduino_library(cores ${CMAKE_CURRENT_LIST_DIR}/../cores/imxrt1176)
# FNET: flat non-recursive globs — every source dir listed explicitly. All files
# are preprocessor-gated, so over-inclusion only costs compile time. third_party
# (mbedTLS) is deliberately NOT compiled (FNET_CFG_TLS=0 for __IMXRT1176__).
import_arduino_library(fnet $ENV{HOME}/Development/FNET/src
    stack
    service service/dhcp service/dns service/autoip service/link service/mdns service/ping
    port port/cpu port/cpu/mimxrt port/netif/fec)
import_arduino_library(nativeethernet $ENV{HOME}/Development/NativeEthernet src src/utility)
teensy_add_executable(native_ethernet_test native_ethernet_test.cpp)
teensy_target_link_libraries(native_ethernet_test cores fnet nativeethernet)
target_link_libraries(native_ethernet_test.elf stdc++)
```

- [ ] **Step 3: Write `native_ethernet_test/native_ethernet_test.cpp`**

Same phase markers as milestone 3 (`ETH_BOOT`, `ETH_DHCP ok=`, `ETH_NETIF_UP`, `CLIENT_ECHO=`, `DNS_OK ip=`), adapted to the NativeEthernet API (no `Ethernet.loop()` — the library pumps FNET from a 1 kHz IntervalTimer ISR). The client probe is adaptive: SLIRP lease (10.0.2.x) → guestfwd echo probe; real network → outbound HTTP GET (`HTTP_GET=` marker), matching the milestone-3 HW method.

```cpp
#include "Arduino.h"
#include "HardwareSerial.h"
#include "NativeEthernet.h"
#include "NativeDns.h"
#include <string.h>

static uint8_t mac[6] = {0x02,0x00,0x00,0x00,0x00,0x01};
EthernetServer server(7);
EthernetUDP    udp;
static bool did_client = false, did_dns = false;

void setup() {
    Serial1.begin(115200); delay(50);
    Serial1.println("ETH_BOOT");
    int ok = Ethernet.begin(mac, 15000);      /* DHCP; FNET pumped by IntervalTimer */
    IPAddress ip = Ethernet.localIP();
    Serial1.print("ETH_DHCP ok="); Serial1.print(ok);
    Serial1.print(" ip="); Serial1.println(ip);
    server.begin();
    udp.begin(7);
    Serial1.println("ETH_NETIF_UP");
}

static bool on_slirp() {                      /* QEMU SLIRP lease = 10.0.2.15 */
    IPAddress ip = Ethernet.localIP();
    return ip[0] == 10 && ip[1] == 0 && ip[2] == 2;
}

static void serve_tcp() {
    EthernetClient c = server.available();
    if (c) { while (c.available()) { uint8_t b = c.read(); c.write(b); } }
}

static void serve_udp() {
    int n = udp.parsePacket();
    if (n > 0) {
        static uint8_t buf[600]; int m = udp.read(buf, sizeof(buf));
        udp.beginPacket(udp.remoteIP(), udp.remotePort());
        udp.write(buf, m); udp.endPacket();
    }
}

static void try_client_once() {
    did_client = true;
    EthernetClient c;
    if (on_slirp()) {
        if (c.connect(IPAddress(10,0,2,100), 7)) {
            const char *tok = "ETHCLI-PROBE\n"; c.write((const uint8_t*)tok, 13);
            uint32_t t0 = millis(); char in[16]; int got = 0;
            while (got < 13 && millis()-t0 < 3000) { while (c.available() && got < 13) in[got++] = c.read(); }
            Serial1.print("CLIENT_ECHO="); Serial1.println((got==13 && memcmp(in,tok,13)==0) ? "PASS" : "FAIL");
            c.stop();
        } else Serial1.println("CLIENT_ECHO=FAIL");
    } else {
        /* Real network: outbound HTTP GET via router NAT (Mac firewall blocks
           inbound echo peers — same approach as milestone 3 HW verification). */
        if (c.connect("example.com", 80)) {
            c.print("GET / HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n");
            uint32_t t0 = millis(); char hdr[16]; int got = 0;
            while (got < 15 && millis()-t0 < 8000) { while (c.available() && got < 15) hdr[got++] = c.read(); }
            hdr[got] = 0;
            Serial1.print("HTTP_GET="); Serial1.println(strstr(hdr, "HTTP/1.1 200") ? "PASS" : "FAIL");
            c.stop();
        } else Serial1.println("HTTP_GET=FAIL");
    }
}

static void try_dns_once() {
    did_dns = true;
    DNSClient dns;
    IPAddress d = Ethernet.dnsServerIP();
    if (d == IPAddress(0,0,0,0)) d = IPAddress(10,0,2,3);   /* SLIRP DNS fallback */
    dns.begin(d);
    IPAddress rip;
    if (dns.getHostByName("example.com", rip, 6000) == 1) {
        Serial1.print("DNS_OK ip="); Serial1.println(rip);
    } else Serial1.println("DNS_FAIL");
}

void loop() {
    serve_tcp();
    serve_udp();
    if (Ethernet.localIP() != IPAddress(0,0,0,0)) {
        if (!did_client) try_client_once();
        if (!did_dns)    try_dns_once();
    }
}
```

- [ ] **Step 4: Write `native_ethernet_test/run_qemu_native_ethernet.sh`** (verbatim clone of the milestone-3 runner with names swapped — keep the `PHASE`-before-`gate_init` and single-quoted `-nic "$NICVAL"` conventions)

```sh
#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
export PHASE="${1:-${PHASE:-boot}}"
gate_init
ELF="$DIR/build/native_ethernet_test.elf"; VCOM="$DIR/vcom.uart"; DBG="$DIR/neth.dbg"; RES="$DIR/neth.result"
gate_tmp "$RES"; PORT=15600
rm -f "$VCOM" "$DBG" "$RES"
# Carry the -nic VALUE (no flag) so it can be passed as a single quoted arg;
# the client phase's guestfwd -cmd contains a space that must NOT word-split.
case "$PHASE" in
  server) NICVAL="user,model=imx.enet,hostfwd=tcp::5555-:7" ;;
  udp)    NICVAL="user,model=imx.enet,hostfwd=udp::5556-:7" ;;
  client) NICVAL="user,model=imx.enet,guestfwd=tcp:10.0.2.100:7-cmd:python3 $DIR/guestfwd_echo.py" ;;
  dns)    NICVAL="user,model=imx.enet" ;;
  *)      NICVAL="user,model=imx.enet" ;;
esac
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$VCOM" -nic "$NICVAL" -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
if [ "$PHASE" = server ] || [ "$PHASE" = udp ]; then
    RC=0; python3 "$DIR/ethernet_peer.py" "$PHASE" 127.0.0.1 "$PORT" > "$RES" 2>&1 || RC=$?
else
    sleep 12; RC=0
fi
sleep 1; kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== VCOM ===="; cat "$VCOM" 2>/dev/null; echo "==== peer ===="; cat "$RES" 2>/dev/null || true
grep -q "ETH_NETIF_UP" "$VCOM" || { echo "FAIL: netif did not come up"; exit 1; }
[ $RC -eq 0 ] || { echo "FAIL: peer rc=$RC"; exit 1; }
case "$PHASE" in
  client) grep -q "CLIENT_ECHO=PASS" "$VCOM" || { echo "FAIL: no client echo"; exit 1; } ;;
  dns)    grep -q "DNS_OK ip=" "$VCOM" || { echo "FAIL: no DNS resolve"; exit 1; } ;;
esac
echo "PASS: native_ethernet_test $PHASE"
```

Then: `chmod +x native_ethernet_test/run_qemu_native_ethernet.sh`

- [ ] **Step 5: Build — verify it FAILS on the FNET CPU select (the RED state)**

```bash
cd ~/Development/rt1170/evkb/native_ethernet_test
rm -rf build && cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake && cmake --build build
```

Expected: **BUILD FAILURE** originating in FNET/NativeEthernet, because `fnet_config.h` doesn't recognize `__IMXRT1176__` yet, so no CPU is selected and `port/cpu/fnet_cpu_config.h` is never included (`FNET_MIMXRT` stays 0, every port file compiles empty). The exact first error may vary — plausible forms: missing FNET CPU types/macros while compiling `stack/` or NativeEthernet, or `FNET_CPU_ETH0_IF`/`fnet_cpu_eth0_if` undeclared/unresolved. If it fails on missing `Client.h` instead, Task 1 wasn't completed. Record the exact error in the task report — Task 3 must flip precisely this failure to green.

- [ ] **Step 6: Commit the scaffold (exact paths only — never `git add -A` in evkb)**

```bash
cd ~/Development/rt1170/evkb
git add native_ethernet_test/CMakeLists.txt native_ethernet_test/native_ethernet_test.cpp \
        native_ethernet_test/run_qemu_native_ethernet.sh native_ethernet_test/ethernet_peer.py \
        native_ethernet_test/guestfwd_echo.py native_ethernet_test/toolchain
git commit -m "test(ethernet-m4): native_ethernet_test gate scaffold (FNET+NativeEthernet) — RED, no FNET RT1176 target yet"
```

---

### Task 3: FNET `FNET_CFG_CPU_MIMXRT1176` target (build green + boot gate green)

Seven config-level edits + NOTICE. After this task the ELF must build and the **boot** gate must pass in QEMU (board hooks are QEMU no-ops, so DHCP over SLIRP works before Task 4 — the hooks are HW-critical only).

**Files (all under `~/Development/FNET/`):**
- Modify: `src/fnet_config.h` (board→CPU select)
- Modify: `src/fnet_user_config.h` (TLS off for 1176)
- Modify: `src/port/cpu/fnet_cpu_config.h` (declare + include block)
- Create: `src/port/cpu/mimxrt/fnet_mimxrt1176_config.h`
- Modify: `src/port/cpu/mimxrt/fnet_mimxrt_config.h` (per-CPU ETH0 vector)
- Modify: `src/port/cpu/mimxrt/fnet_mimxrt.h` (per-CPU FEC base)
- Modify: `src/port/netif/fec/fnet_fec.h` (per-CPU FEC clock + reg-map guard)
- Modify: `src/port/netif/fec/fnet_fec.c` (MSCR formula guard)
- Create: `NOTICE`

- [ ] **Step 1: `src/fnet_config.h` — extend the board→CPU select**

Replace:
```c
#if defined(ARDUINO_TEENSY41) || defined(ARDUINO_MIMXRT1060_EVKB)
#define FNET_MIMXRT (1)
#define FNET_CFG_CPU_MIMXRT1062 (1)
#include "port/cpu/fnet_cpu_config.h"           /* Default platform configuration. */
#endif
```
with:
```c
#if defined(ARDUINO_TEENSY41) || defined(ARDUINO_MIMXRT1060_EVKB)
#define FNET_MIMXRT (1)
#define FNET_CFG_CPU_MIMXRT1062 (1)
#include "port/cpu/fnet_cpu_config.h"           /* Default platform configuration. */
#elif defined(__IMXRT1176__)
/* i.MX RT1176 (NXP MIMXRT1170-EVKB) — 10/100 ENET + RTL8201. */
#define FNET_MIMXRT (1)
#define FNET_CFG_CPU_MIMXRT1176 (1)
#include "port/cpu/fnet_cpu_config.h"           /* Default platform configuration. */
#endif
```

- [ ] **Step 2: `src/fnet_user_config.h` — force TLS off for the 1176 build**

`FNET_CFG_TLS` defaults to `(2)` (mbedTLS), and NativeEthernetClient's TLS methods reference `fnet_tls_*` whenever it is non-zero — with mbedTLS not compiled that is a guaranteed link failure. Directly ABOVE the existing block:
```c
#ifndef FNET_CFG_TLS
    #define FNET_CFG_TLS                (2)
#endif
```
insert:
```c
/* MIMXRT1176 (RT1170-EVKB) build: mbedTLS is not compiled — TLS off. */
#if defined(__IMXRT1176__)
    #define FNET_CFG_TLS                (0)
#endif
```

- [ ] **Step 3: `src/port/cpu/fnet_cpu_config.h` — declare the CPU + include block**

After the existing:
```c
#ifndef FNET_CFG_CPU_MIMXRT1062
    #define FNET_CFG_CPU_MIMXRT1062 (0)
#endif
```
(near line 126) add:
```c
#ifndef FNET_CFG_CPU_MIMXRT1176
    #define FNET_CFG_CPU_MIMXRT1176 (0)
#endif
```
After the existing `#if FNET_CFG_CPU_MIMXRT1062 ... #endif` include block (near line 318) add:
```c
#if FNET_CFG_CPU_MIMXRT1176
    #ifdef FNET_CPU_STR
        #error "More than one CPU selected FNET_CFG_CPU_XXXX"
    #endif

    #include "port/cpu/mimxrt/fnet_mimxrt1176_config.h"
    #define FNET_CPU_STR    "MIMXRT1176"
#endif
```

- [ ] **Step 4: Create `src/port/cpu/mimxrt/fnet_mimxrt1176_config.h`** (full file)

```c
/**************************************************************************
*
* Copyright 2026 by Nicholas Newdigate. FNET Community.
*
***************************************************************************
*
*  Licensed under the Apache License, Version 2.0 (the "License"); you may
*  not use this file except in compliance with the License.
*  You may obtain a copy of the License at
*
*  http://www.apache.org/licenses/LICENSE-2.0
*
*  Unless required by applicable law or agreed to in writing, software
*  distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
*  WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
*  See the License for the specific language governing permissions and
*  limitations under the License.
*
***************************************************************************
*
*  MIMXRT1176 (NXP MIMXRT1170-EVKB) specific configuration file.
*
***************************************************************************/

/************************************************************************
 * !!!DO NOT MODIFY THIS FILE!!!
 ************************************************************************/

#ifndef _FNET_MIMXRT1176_CONFIG_H_
#define _FNET_MIMXRT1176_CONFIG_H_

#define FNET_MIMXRT                                     (1)

/* CM7 core clock frequency: in Hz (OverDrive). */
#ifndef FNET_CFG_CPU_CLOCK_HZ
    #define FNET_CFG_CPU_CLOCK_HZ                       (996000000U)
#endif

/* Maximum number of incoming frames buffered by the Ethernet module. */
#ifndef FNET_CFG_CPU_ETH_RX_BUFS_MAX
    #define FNET_CFG_CPU_ETH_RX_BUFS_MAX                (4)
#endif

/* RT1170-EVKB 10/100 port: RTL8201 PHY at MDIO address 3
   (HW-verified milestone 1: PHYID 1C:C816). */
#ifndef FNET_CFG_CPU_ETH0_PHY_ADDR
    #define FNET_CFG_CPU_ETH0_PHY_ADDR                  (3)
#endif

/* No flash driver so far. */
#define FNET_CFG_CPU_FLASH                              (0)

/* ENET_1G / ENET_QOS are out of scope — single interface. */
#define FNET_CFG_CPU_ETH1                               (0)

/* FNET_CFG_TIMER_ALT=1 (application timer via millis()) — HW timers unused. */
#ifndef FNET_CFG_MIMXRT_TIMER_PIT
    #define FNET_CFG_MIMXRT_TIMER_PIT                   (0)
#endif
#ifndef FNET_CFG_MIMXRT_TIMER_QTMR
    #define FNET_CFG_MIMXRT_TIMER_QTMR                  (1)
#endif

/* HW checksum offload OFF: the QEMU imx.enet model does not implement TX
   checksum insertion — offloaded (zeroed) checksums would be dropped by SLIRP
   and the gates could never pass.  Software checksums everywhere; enabling
   offload on real HW is a later, HW-only experiment. */
#ifndef FNET_CFG_CPU_ETH_HW_TX_IP_CHECKSUM
    #define FNET_CFG_CPU_ETH_HW_TX_IP_CHECKSUM          (0)
#endif
#ifndef FNET_CFG_CPU_ETH_HW_TX_PROTOCOL_CHECKSUM
    #define FNET_CFG_CPU_ETH_HW_TX_PROTOCOL_CHECKSUM    (0)
#endif
#ifndef FNET_CFG_CPU_ETH_HW_RX_IP_CHECKSUM
    #define FNET_CFG_CPU_ETH_HW_RX_IP_CHECKSUM          (0)
#endif
#ifndef FNET_CFG_CPU_ETH_HW_RX_PROTOCOL_CHECKSUM
    #define FNET_CFG_CPU_ETH_HW_RX_PROTOCOL_CHECKSUM    (0)
#endif

/* Discard frames with MAC layer errors. */
#ifndef FNET_CFG_CPU_ETH_HW_RX_MAC_ERR
    #define FNET_CFG_CPU_ETH_HW_RX_MAC_ERR              (1)
#endif

/* ENET DMA buffer descriptors + buffers go to OCRAM via the core's DMAMEM
   section (.dmabuffers, zero-initialized by startup): the D-cache is OFF in
   this core (coherent without maintenance, which fnet_fec.c never performs)
   and erratum ERR050396 forbids ENET DMA into CM7 TCM regardless. */
#ifndef FNET_CFG_CPU_NONCACHEABLE_SECTION
    #define FNET_CFG_CPU_NONCACHEABLE_SECTION           ".dmabuffers"
#endif

#endif /* _FNET_MIMXRT1176_CONFIG_H_ */
```

- [ ] **Step 5: `src/port/cpu/mimxrt/fnet_mimxrt_config.h` — per-CPU ETH0 vector number**

Replace (near line 129):
```c
    #ifndef FNET_CFG_CPU_ETH0_VECTOR_NUMBER
        #define FNET_CFG_CPU_ETH0_VECTOR_NUMBER        (16+114/*irq*/)
    #endif
```
with:
```c
    #ifndef FNET_CFG_CPU_ETH0_VECTOR_NUMBER
        #if FNET_CFG_CPU_MIMXRT1176
            /* RT1176 10/100 ENET IRQ = 137. */
            #define FNET_CFG_CPU_ETH0_VECTOR_NUMBER    (16+137/*irq*/)
        #else
            #define FNET_CFG_CPU_ETH0_VECTOR_NUMBER    (16+114/*irq*/)
        #endif
    #endif
```

- [ ] **Step 6: `src/port/cpu/mimxrt/fnet_mimxrt.h` — per-CPU FEC base address**

Replace (near line 289):
```c
#define FNET_FEC0_BASE_ADDR                             (0x402D8000u)
```
with:
```c
#if FNET_CFG_CPU_MIMXRT1176
    /* RT1176 10/100 ENET (ENET_1G/ENET_QOS are separate IPs, not targeted). */
    #define FNET_FEC0_BASE_ADDR                         (0x40424000u)
#else
    #define FNET_FEC0_BASE_ADDR                         (0x402D8000u)
#endif
```

- [ ] **Step 7: `src/port/netif/fec/fnet_fec.h` — FEC clock source + register-map guard**

(a) Replace (near line 73):
```c
#if FNET_MIMXRT     /* i.MX-RT*/
    #define FNET_FEC_CLOCK_KHZ  FNET_CPU_CLOCK_KHZ
```
with:
```c
#if FNET_MIMXRT     /* i.MX-RT*/
    #if FNET_CFG_CPU_MIMXRT1176
        /* The MDC divider is clocked by the ENET module (bus) clock, not the
           CM7 core clock: RT1176 bus root = 240 MHz -> MII_SPEED 47
           (MDC = 2.5 MHz), matching the HW-verified milestone-1 value. */
        #define FNET_FEC_CLOCK_KHZ  (240000U)
    #else
        #define FNET_FEC_CLOCK_KHZ  FNET_CPU_CLOCK_KHZ
    #endif
```
(b) Replace (near line 194):
```c
#if FNET_CFG_CPU_MIMXRT1052 || FNET_CFG_CPU_MIMXRT1062
    volatile fnet_uint32_t reserved0;
#endif
```
with:
```c
#if FNET_CFG_CPU_MIMXRT1052 || FNET_CFG_CPU_MIMXRT1062 || FNET_CFG_CPU_MIMXRT1176
    volatile fnet_uint32_t reserved0;
#endif
```
(The RT1176 10/100 ENET is the same FEC IP: EIR at base+0x004 — verified against the generated `imxrt1176.h` register map.)

- [ ] **Step 8: `src/port/netif/fec/fnet_fec.c` — MSCR formula guard**

Replace (near line 375):
```c
#elif FNET_CFG_CPU_MIMXRT1052 || FNET_CFG_CPU_MIMXRT1062
```
with:
```c
#elif FNET_CFG_CPU_MIMXRT1052 || FNET_CFG_CPU_MIMXRT1062 || FNET_CFG_CPU_MIMXRT1176
```
Sanity: with `FNET_FEC_CLOCK_KHZ=240000` and `FNET_ETH_MII_CLOCK_KHZ=2500`, the formula `(240000 / (2*2500)) - 1 = 47` — the HW-proven divider.

- [ ] **Step 9: Create `~/Development/FNET/NOTICE`**

```
FNET TCP/IP stack — NOTICE

This product includes software developed as part of the FNET project
(Copyright 2005-2018 by Andrey Butok and the FNET Community), licensed
under the Apache License, Version 2.0 (see fnet_license.txt).

Modifications in this fork:
- 2026: i.MX RT1176 (NXP MIMXRT1170-EVKB) CPU target — FNET_CFG_CPU_MIMXRT1176
  (config plumbing, 10/100 ENET base/IRQ, RTL8201 PHY, board bring-up hooks
  transplanted from the HW-verified rt1170/evkb enet.c driver).
  Copyright 2026 Nicholas Newdigate, licensed under the Apache License 2.0.
- Earlier fork history: Teensy 4.1 / MIMXRT1060-EVKB Arduino packaging
  (vjmuzik/newdigate).
```

- [ ] **Step 10: Rebuild — expect SUCCESS**

```bash
cd ~/Development/rt1170/evkb/native_ethernet_test
rm -rf build && cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake && cmake --build build
```
Expected: `native_ethernet_test.elf` builds with no errors.

- [ ] **Step 11: Run the boot gate — expect GREEN**

Run: `./run_qemu_native_ethernet.sh boot`
Expected VCOM output includes: `ETH_BOOT`, `ETH_DHCP ok=1 ip=10.0.2.15`, `ETH_NETIF_UP`; script prints `PASS: native_ethernet_test boot`.
This exercises: FNET init, ISR install at vector 153, interrupt-driven RX in QEMU, DHCP over SLIRP. If it fails, use superpowers:systematic-debugging — likely suspects in order: ISR never fires (check `neth.dbg` for guest errors; check EIMR/EIR handling), FEC register-map mismatch, DHCP socket buffers.

- [ ] **Step 12: Commit FNET**

```bash
cd ~/Development/FNET && git add -A && git commit -m "feat(mimxrt1176): add FNET_CFG_CPU_MIMXRT1176 target (RT1170-EVKB 10/100 ENET)

Config plumbing only: __IMXRT1176__ board select, fnet_mimxrt1176_config.h
(PHY addr 3, checksum offload off, .dmabuffers DMA section), ENET base
0x40424000, vector 153, FEC clock 240 MHz bus -> MII_SPEED 47, reg-map +
MSCR guards extended. TLS forced off for this target. NOTICE added."
```

---

### Task 4: FNET RT1176 board hooks (transplanted bring-up; QEMU-neutral, HW-critical)

Add the self-contained RT1176 branch to `fnet_mimxrt_eth.c`: SysPll1 AI bring-up + ENET clock root + RMII pin mux + PHY reset, and the ANAR advertise in the phy-init hook. Logic-identical to `cores/imxrt1176/enet.c` (READ IT FIRST — it is the HW-proven truth). QEMU treats all of it as no-ops, so the verification here is (a) boot gate stays green, (b) a careful sequence-by-sequence diff against `enet.c`.

**Files:**
- Modify: `~/Development/FNET/src/port/cpu/mimxrt/fnet_mimxrt_eth.c`

- [ ] **Step 1: Add the 1176 register/helper block**

Insert directly after `#include "port/netif/fec/fnet_fec.h"` (line 32):

```c
#if FNET_CFG_CPU_MIMXRT1176
/*======================== i.MX RT1176 (NXP MIMXRT1170-EVKB) =================
 * Board bring-up for the 10/100 ENET (FEC @0x40424000) + RTL8201 PHY (MDIO
 * addr 3).  Transplanted logic-identical from the HW-verified rt1170/evkb
 * milestone-1 driver (cores/imxrt1176/enet.c — HW-proven: PHYID 1C:C816,
 * board answers ping), which itself follows the NXP SDK evkbmimxrt1170 enet
 * example (pin_mux.c / hardware_init.c / fsl_clock.c / fsl_anatop_ai.c).
 * One-off register addresses are carried locally so this port stays
 * self-contained (no SoC vendor header required).
 *
 * ★HW-CRITICAL: the boot ROM leaves SysPll1 powered down (SYS_PLL1_CTRL =
 * 0x4000).  MDIO + PHY link work without it, but the RMII 50 MHz ref-clock is
 * SysPll1Div2/10 — without the PLL the MAC moves ZERO frames.  All AI-protocol
 * polls are BOUNDED so QEMU (no analog-interface model) proceeds instead of
 * hanging; on silicon DONE/STABLE assert in ~us.
 *===========================================================================*/

/* Busy-wait provided by the Arduino core (delay.c). */
extern void delayMicroseconds(fnet_uint32_t usec);

/* ---- IOMUXC: RMII + MDIO pads (SW_MUX / SW_PAD) ---- */
#define FNET1176_MUX_AD_32        (*(volatile fnet_uint32_t *)0x400E818Cu) /* ENET_MDC   ALT3 */
#define FNET1176_MUX_AD_33        (*(volatile fnet_uint32_t *)0x400E8190u) /* ENET_MDIO  ALT3 */
#define FNET1176_MUX_DISP_B2_02   (*(volatile fnet_uint32_t *)0x400E821Cu) /* TXD0  ALT1 */
#define FNET1176_MUX_DISP_B2_03   (*(volatile fnet_uint32_t *)0x400E8220u) /* TXD1  ALT1 */
#define FNET1176_MUX_DISP_B2_04   (*(volatile fnet_uint32_t *)0x400E8224u) /* TX_EN ALT1 */
#define FNET1176_MUX_DISP_B2_05   (*(volatile fnet_uint32_t *)0x400E8228u) /* REF_CLK ALT2+SION */
#define FNET1176_MUX_DISP_B2_06   (*(volatile fnet_uint32_t *)0x400E822Cu) /* RXD0  ALT1+SION */
#define FNET1176_MUX_DISP_B2_07   (*(volatile fnet_uint32_t *)0x400E8230u) /* RXD1  ALT1+SION */
#define FNET1176_MUX_DISP_B2_08   (*(volatile fnet_uint32_t *)0x400E8234u) /* RX_EN ALT1 */
#define FNET1176_MUX_DISP_B2_09   (*(volatile fnet_uint32_t *)0x400E8238u) /* RX_ER ALT1 */
#define FNET1176_PAD_DISP_B2_02   (*(volatile fnet_uint32_t *)0x400E8460u)
#define FNET1176_PAD_DISP_B2_03   (*(volatile fnet_uint32_t *)0x400E8464u)
#define FNET1176_PAD_DISP_B2_04   (*(volatile fnet_uint32_t *)0x400E8468u)
#define FNET1176_PAD_DISP_B2_05   (*(volatile fnet_uint32_t *)0x400E846Cu)
#define FNET1176_PAD_DISP_B2_06   (*(volatile fnet_uint32_t *)0x400E8470u)
#define FNET1176_PAD_DISP_B2_07   (*(volatile fnet_uint32_t *)0x400E8474u)
#define FNET1176_PAD_DISP_B2_08   (*(volatile fnet_uint32_t *)0x400E8478u)
#define FNET1176_PAD_DISP_B2_09   (*(volatile fnet_uint32_t *)0x400E847Cu)
#define FNET1176_MUX_LPSR_12      (*(volatile fnet_uint32_t *)0x40C08030u) /* GPIO12_IO12 (PHY reset) */
#define FNET1176_PAD_LPSR_12      (*(volatile fnet_uint32_t *)0x40C08070u)
#define FNET1176_SION             0x10u

/* ---- IOMUXC SELECT_INPUT daisy regs (from fsl_iomuxc.h) ---- */
#define FNET1176_DAISY_MDIO       (*(volatile fnet_uint32_t *)0x400E84ACu)
#define FNET1176_DAISY_REF_CLK    (*(volatile fnet_uint32_t *)0x400E84A8u)
#define FNET1176_DAISY_RXD0       (*(volatile fnet_uint32_t *)0x400E84B0u)
#define FNET1176_DAISY_RXD1       (*(volatile fnet_uint32_t *)0x400E84B4u)
#define FNET1176_DAISY_RXEN       (*(volatile fnet_uint32_t *)0x400E84B8u)
#define FNET1176_DAISY_RXER       (*(volatile fnet_uint32_t *)0x400E84BCu)

/* ---- GPR + LPSR GPIO12 (PHY hardware reset line) ---- */
#define FNET1176_GPR4             (*(volatile fnet_uint32_t *)0x400E4010u)
#define FNET1176_GPR4_REF_CLK_DIR (1u << 1)
#define FNET1176_GPR28            (*(volatile fnet_uint32_t *)0x400E4070u)
#define FNET1176_GPR28_CACHE_ENET (1u << 7)
#define FNET1176_GPIO12_GDIR      (*(volatile fnet_uint32_t *)0x40C70004u)
#define FNET1176_GPIO12_DR_SET    (*(volatile fnet_uint32_t *)0x40C70084u)
#define FNET1176_GPIO12_DR_CLEAR  (*(volatile fnet_uint32_t *)0x40C70088u)

/* ---- CCM: ENET1 clock root (root 51) + LPCG112 gate ---- */
#define FNET1176_CCM_ROOT51       (*(volatile fnet_uint32_t *)0x40CC1980u)
#define FNET1176_CCM_ROOT_MUX(x)  (((fnet_uint32_t)(x) << 8) & 0x700u)
#define FNET1176_CCM_ROOT_DIV(x)  (((fnet_uint32_t)(x) << 0) & 0x0FFu)
#define FNET1176_CCM_LPCG112      (*(volatile fnet_uint32_t *)0x40CC6E00u)

/* ---- ANADIG / ANATOP-AI: SysPll1 (one overlapping block @0x40C84000) ---- */
#define FNET1176_SP1_CTRL    (*(volatile fnet_uint32_t *)0x40C842C0u)
#define  FNET1176_SP1_ENCLK  0x00002000u
#define  FNET1176_SP1_GATE   0x00004000u
#define  FNET1176_SP1_DIV2   0x02000000u
#define  FNET1176_SP1_DIV5   0x04000000u
#define  FNET1176_SP1_STABLE 0x20000000u
#define FNET1176_AI1G_CTRL   (*(volatile fnet_uint32_t *)0x40C84850u)
#define FNET1176_AI1G_WDATA  (*(volatile fnet_uint32_t *)0x40C84860u)
#define FNET1176_AI1G_RDATA  (*(volatile fnet_uint32_t *)0x40C84870u)
#define FNET1176_AILDO_CTRL  (*(volatile fnet_uint32_t *)0x40C84820u)
#define FNET1176_AILDO_WDAT  (*(volatile fnet_uint32_t *)0x40C84830u)
#define FNET1176_AILDO_RDAT  (*(volatile fnet_uint32_t *)0x40C84840u)
#define FNET1176_PMU_LDOPLL  (*(volatile fnet_uint32_t *)0x40C84500u)
#define FNET1176_PMU_REFCTL  (*(volatile fnet_uint32_t *)0x40C84570u)
#define  FNET1176_AI_ADDRM   0x000000FFu
#define  FNET1176_AI_RWB     0x00010000u
#define  FNET1176_AI1G_TOG   0x00000100u
#define  FNET1176_AI1G_DONE  0x00000200u
#define  FNET1176_PMU_TOG    0x00010000u
#define  FNET1176_PMU_VREF   0x00000010u
#define  FNET1176_AIR_C0     0x00u
#define  FNET1176_AIR_C0SET  0x04u
#define  FNET1176_AIR_C0CLR  0x08u
#define  FNET1176_AIR_C2     0x20u
#define  FNET1176_AIR_C3     0x30u
#define  FNET1176_AIR_LDO0   0x00u
#define  FNET1176_1G_HOLDR   0x00002000u
#define  FNET1176_1G_PWRUP   0x00004000u
#define  FNET1176_1G_EN      0x00008000u
#define  FNET1176_1G_BYP     0x00010000u
#define  FNET1176_1G_REGEN   0x00400000u
#define  FNET1176_1G_DIVM    0x0000007Fu
#define  FNET1176_LDO_LINR   0x00000001u
#define  FNET1176_LDO_LIMIT  0x00000004u
#define  FNET1176_LDO_1V0    0x00000100u

/* AI transport: 1G PLL interface (toggle + wait-done handshake, bounded). */
static void fnet1176_ai1g_write(fnet_uint32_t a, fnet_uint32_t d)
{
    fnet_uint32_t pre = FNET1176_AI1G_CTRL & FNET1176_AI1G_DONE, to = 100000u, t;
    FNET1176_AI1G_CTRL &= ~FNET1176_AI_RWB;                              /* write mode */
    t = FNET1176_AI1G_CTRL; t = (t & ~FNET1176_AI_ADDRM) | (a & FNET1176_AI_ADDRM); FNET1176_AI1G_CTRL = t;
    FNET1176_AI1G_WDATA = d;
    FNET1176_AI1G_CTRL ^= FNET1176_AI1G_TOG;                             /* kick */
    while (((FNET1176_AI1G_CTRL & FNET1176_AI1G_DONE) == pre) && --to) { }
}
static fnet_uint32_t fnet1176_ai1g_read(fnet_uint32_t a)
{
    fnet_uint32_t pre = FNET1176_AI1G_CTRL & FNET1176_AI1G_DONE, to = 100000u, t;
    t = FNET1176_AI1G_CTRL | FNET1176_AI_RWB; FNET1176_AI1G_CTRL = t;    /* read mode */
    t = FNET1176_AI1G_CTRL; t = (t & ~FNET1176_AI_ADDRM) | (a & FNET1176_AI_ADDRM); FNET1176_AI1G_CTRL = t;
    FNET1176_AI1G_CTRL ^= FNET1176_AI1G_TOG;
    while (((FNET1176_AI1G_CTRL & FNET1176_AI1G_DONE) == pre) && --to) { }
    return FNET1176_AI1G_RDATA;
}
/* AI transport: LDO interface (toggle PMU_LDO_PLL, no done-wait). */
static void fnet1176_aildo_write(fnet_uint32_t a, fnet_uint32_t d)
{
    fnet_uint32_t t;
    FNET1176_AILDO_CTRL &= ~FNET1176_AI_RWB;
    t = FNET1176_AILDO_CTRL; t = (t & ~FNET1176_AI_ADDRM) | (a & FNET1176_AI_ADDRM); FNET1176_AILDO_CTRL = t;
    FNET1176_AILDO_WDAT = d;
    FNET1176_PMU_LDOPLL ^= FNET1176_PMU_TOG;
}
static fnet_uint32_t fnet1176_aildo_read(fnet_uint32_t a)
{
    fnet_uint32_t t = FNET1176_AILDO_CTRL | FNET1176_AI_RWB; FNET1176_AILDO_CTRL = t;
    t = FNET1176_AILDO_CTRL; t = (t & ~FNET1176_AI_ADDRM) | (a & FNET1176_AI_ADDRM); FNET1176_AILDO_CTRL = t;
    FNET1176_PMU_LDOPLL ^= FNET1176_PMU_TOG;
    return FNET1176_AILDO_RDAT;
}
static void fnet1176_sys_pll1_init(void)
{
    fnet_uint32_t to, r;
    if (FNET1176_SP1_CTRL & FNET1176_SP1_STABLE) return;                 /* already locked */
    /* PLL LDO (1.0 V) enable + 100us soft-start (skip if already on). */
    if (fnet1176_aildo_read(FNET1176_AIR_LDO0) != (FNET1176_LDO_1V0 | FNET1176_LDO_LINR)) {
        fnet1176_aildo_write(FNET1176_AIR_LDO0, FNET1176_LDO_1V0 | FNET1176_LDO_LINR | FNET1176_LDO_LIMIT);
        delayMicroseconds(100);
        fnet1176_aildo_write(FNET1176_AIR_LDO0, FNET1176_LDO_1V0 | FNET1176_LDO_LINR);
        FNET1176_PMU_REFCTL |= FNET1176_PMU_VREF;
    }
    fnet1176_ai1g_write(FNET1176_AIR_C0SET, FNET1176_1G_BYP);            /* bypass on */
    FNET1176_SP1_CTRL |= FNET1176_SP1_ENCLK;                             /* sw enable clk */
    fnet1176_ai1g_write(FNET1176_AIR_C3, 0x0FFFFFFFu);                   /* denominator 2^28-1 */
    fnet1176_ai1g_write(FNET1176_AIR_C2, 178956970u);                    /* numerator 0x0AAAAAAA */
    r = fnet1176_ai1g_read(FNET1176_AIR_C0); r = (r & ~FNET1176_1G_DIVM) | (41u & FNET1176_1G_DIVM);
    fnet1176_ai1g_write(FNET1176_AIR_C0, r);                             /* loop divider 41 -> 1000 MHz */
    fnet1176_ai1g_write(FNET1176_AIR_C0SET, FNET1176_1G_REGEN);          /* PLL reg enable */
    delayMicroseconds(100);
    fnet1176_ai1g_write(FNET1176_AIR_C0SET, FNET1176_1G_PWRUP | FNET1176_1G_HOLDR); /* power up */
    fnet1176_ai1g_write(FNET1176_AIR_C0SET, FNET1176_1G_HOLDR);          /* toggle hold-ring-off */
    delayMicroseconds(225);
    fnet1176_ai1g_write(FNET1176_AIR_C0CLR, FNET1176_1G_HOLDR);
    to = 1000000u; while (((FNET1176_SP1_CTRL & FNET1176_SP1_STABLE) == 0u) && --to) { } /* wait lock */
    fnet1176_ai1g_write(FNET1176_AIR_C0SET, FNET1176_1G_EN);             /* enable clk out */
    FNET1176_SP1_CTRL &= ~FNET1176_SP1_GATE;                             /* ungate */
    FNET1176_SP1_CTRL |= FNET1176_SP1_DIV2;                              /* /2 tap (500 MHz -> ENET) */
    FNET1176_SP1_CTRL &= ~FNET1176_SP1_DIV5;
    fnet1176_ai1g_write(FNET1176_AIR_C0CLR, FNET1176_1G_BYP);            /* bypass off */
}
static void fnet1176_clock_init(void)
{
    fnet1176_sys_pll1_init();   /* SysPll1 1 GHz + /2 (500 MHz) = the ENET root source */
    /* ENET1 clock root (root 51) <- SysPll1Div2 (mux 4), divide-by-10 -> 50 MHz
       (SDK BOARD_InitModuleClock {.mux=4,.div=10}; ROOT DIV field = divider-1). */
    FNET1176_CCM_ROOT51 = FNET1176_CCM_ROOT_MUX(4) | FNET1176_CCM_ROOT_DIV(9);
    FNET1176_CCM_LPCG112 = 1u;  /* ungate the ENET peripheral clock */
}
static void fnet1176_pins_init(void)
{
    /* MDC / MDIO: ALT3, no SION; SDK leaves PAD_CTL at default (no write). */
    FNET1176_MUX_AD_32 = 3u;                  /* ENET_MDC  */
    FNET1176_MUX_AD_33 = 3u;                  /* ENET_MDIO */
    FNET1176_DAISY_MDIO = 1u;
    /* TXD0/TXD1/TX_EN: ALT1, no SION, PAD_CTL = 0x02. */
    FNET1176_MUX_DISP_B2_02 = 1u;  FNET1176_PAD_DISP_B2_02 = 0x02u;
    FNET1176_MUX_DISP_B2_03 = 1u;  FNET1176_PAD_DISP_B2_03 = 0x02u;
    FNET1176_MUX_DISP_B2_04 = 1u;  FNET1176_PAD_DISP_B2_04 = 0x02u;
    /* REF_CLK: ALT2 + SION, daisy=1, PAD_CTL = 0x03 (50 MHz RMII ref clock). */
    FNET1176_MUX_DISP_B2_05 = 2u | FNET1176_SION;
    FNET1176_DAISY_REF_CLK = 1u;
    FNET1176_PAD_DISP_B2_05 = 0x03u;
    /* RXD0/RXD1: ALT1 + SION, daisy=1, PAD_CTL = 0x06. */
    FNET1176_MUX_DISP_B2_06 = 1u | FNET1176_SION;  FNET1176_DAISY_RXD0 = 1u;  FNET1176_PAD_DISP_B2_06 = 0x06u;
    FNET1176_MUX_DISP_B2_07 = 1u | FNET1176_SION;  FNET1176_DAISY_RXD1 = 1u;  FNET1176_PAD_DISP_B2_07 = 0x06u;
    /* RX_EN/RX_ER: ALT1, no SION, daisy=1, PAD_CTL = 0x06. */
    FNET1176_MUX_DISP_B2_08 = 1u;  FNET1176_DAISY_RXEN = 1u;  FNET1176_PAD_DISP_B2_08 = 0x06u;
    FNET1176_MUX_DISP_B2_09 = 1u;  FNET1176_DAISY_RXER = 1u;  FNET1176_PAD_DISP_B2_09 = 0x06u;
    /* 50 MHz ENET_REF_CLK is an OUTPUT from the SoC to the external PHY. */
    FNET1176_GPR4 |= FNET1176_GPR4_REF_CLK_DIR;
    /* ERR050396: DMA buffers live in OCRAM; clear CACHE_ENET so ENET writes
       bypass the CM7-TCM sparse-write path. */
    FNET1176_GPR28 &= ~FNET1176_GPR28_CACHE_ENET;
}
static void fnet1176_phy_reset(void)
{
    /* RTL8201 hardware reset via GPIO12_IO12 (pad GPIO_LPSR_12, ALT 0xA);
       >=10 ms low, >=150 ms release — per SDK hardware_init.c. */
    FNET1176_MUX_LPSR_12 = 0xAu;
    FNET1176_PAD_LPSR_12 = 0x0Eu;
    FNET1176_GPIO12_GDIR |= (1u << 12);
    FNET1176_GPIO12_DR_CLEAR = (1u << 12);
    delayMicroseconds(10000);
    FNET1176_GPIO12_DR_SET = (1u << 12);
    delayMicroseconds(150000);
}
static void fnet1176_eth_io_init(void)
{
    fnet1176_clock_init();
    fnet1176_pins_init();
    fnet1176_phy_reset();
}
#endif /* FNET_CFG_CPU_MIMXRT1176 */
```

- [ ] **Step 2: Call the io-init from `fnet_mimxrt_eth_init()`**

Inside `fnet_mimxrt_eth_init`, directly after the `#if FNET_CFG_CPU_ETH_IO_INIT` line and BEFORE the existing `#if FNET_CFG_CPU_MIMXRT1052 || FNET_CFG_CPU_MIMXRT1062` line, insert:

```c
#if FNET_CFG_CPU_MIMXRT1176
    fnet1176_eth_io_init();
#endif
```

(The existing 1052/1062 block is untouched; with `FNET_CFG_CPU_MIMXRT1062=0` it compiles away.)

- [ ] **Step 3: Add the 1176 branch to `fnet_mimxrt_eth_phy_init()`**

Inside `fnet_mimxrt_eth_phy_init`, directly after the opening `{` and BEFORE the existing `#if FNET_CFG_CPU_MIMXRT1052 || FNET_CFG_CPU_MIMXRT1062` line, insert:

```c
#if FNET_CFG_CPU_MIMXRT1176
    /* RTL8201 @ MDIO addr 3: generic clause-22 only (no vendor registers —
       HW-verified in milestone 1; Zephyr drives this PHY generically too).
       Advertise the full 10/100 ability set here: the generic
       _fnet_eth_phy_init() has just soft-reset the PHY and restarts
       auto-negotiation right after this hook returns, so the ANAR write
       (0x01E1, same value as the HW-verified enet.c sequence) takes effect
       for the negotiation that brings the link up at 100BASE-TX full duplex. */
    _fnet_eth_phy_write(netif, FNET_ETH_MII_REG_ANAR,
                        (fnet_uint16_t)(FNET_ETH_MII_REG_ANAR_100_FULLDUPLEX |
                                        FNET_ETH_MII_REG_ANAR_100_HALFDUPLEX |
                                        FNET_ETH_MII_REG_ANAR_10_FULLDUPLEX  |
                                        FNET_ETH_MII_REG_ANAR_10_HALFDUPLEX  |
                                        FNET_ETH_MII_REG_ANAR_IEEE8023));
#endif
```

- [ ] **Step 4: Rebuild + boot gate — expect no regression**

```bash
cd ~/Development/rt1170/evkb/native_ethernet_test
rm -rf build && cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake && cmake --build build
./run_qemu_native_ethernet.sh boot
```
Expected: build clean; `PASS: native_ethernet_test boot`.

- [ ] **Step 5: Sequence diff review against enet.c**

Open `~/Development/rt1170/evkb/cores/imxrt1176/enet.c` side-by-side and verify, register-write by register-write, that `fnet1176_sys_pll1_init` == `enet_sys_pll1_init` (enet.c:176-205), `fnet1176_clock_init` == `enet_clock_init` (211-220, ROOT51 value `MUX(4)|DIV(9)`), `fnet1176_pins_init` == `enet_pins_init` (226-267, every mux/pad/daisy value), `fnet1176_phy_reset` == `enet_phy_reset` (273-283). Any divergence is a bug — fix to match enet.c. State in the task report that this diff was performed and what it found.

- [ ] **Step 6: Commit FNET**

```bash
cd ~/Development/FNET && git add -A && git commit -m "feat(mimxrt1176): RT1170-EVKB board hooks — SysPll1 AI bring-up, ENET root 51 50MHz, RMII pinmux, RTL8201 reset + ANAR advertise (transplanted from HW-verified enet.c)"
```

---

### Task 5: `server` gate green (TCP echo via EthernetServer)

**Files:** none expected; fixes (if any) land where the bug is.

- [ ] **Step 1: Run** `cd ~/Development/rt1170/evkb/native_ethernet_test && ./run_qemu_native_ethernet.sh server`
Expected: peer connects to `127.0.0.1:5555`, sends `ETH-TCP-ECHO-PROBE`, receives the echo; script prints `PASS: native_ethernet_test server`.
- [ ] **Step 2: If FAIL** — superpowers:systematic-debugging. Likely suspects: `EthernetServer::available()` accept path (`fnet_service_register` poll), socket buffer sizing, blocking-accept interplay with the IntervalTimer pump. Fix in the owning repo, rebuild (`rm -rf build` if FNET/NativeEthernet changed), re-run. Commit any fix in its repo with a message explaining the root cause.
- [ ] **Step 3: Re-run boot after any fix** (`./run_qemu_native_ethernet.sh boot`) — must still pass.

### Task 6: `udp` gate green (EthernetUDP echo)

- [ ] **Step 1: Run** `./run_qemu_native_ethernet.sh udp`
Expected: `PASS: native_ethernet_test udp` (peer sends `ETH-UDP-ECHO-PROBE` to `:5556`, gets it back).
- [ ] **Step 2: If FAIL** — systematic-debugging (suspects: `parsePacket`/`recvfrom` path, `beginPacket(remoteIP,remotePort)` addressing). Fix, commit in owning repo, re-run boot+server+udp.

### Task 7: `client` gate green (outbound EthernetClient through guestfwd)

- [ ] **Step 1: Run** `./run_qemu_native_ethernet.sh client`
Expected: VCOM shows `CLIENT_ECHO=PASS`; script prints `PASS: native_ethernet_test client`.
- [ ] **Step 2: If FAIL** — systematic-debugging (suspects: `connect()` non-blocking state machine + `SnSR` mapping, the guestfwd `-cmd` quoting — verify the `-nic` value is passed as ONE argument). Fix, commit, re-run boot..client.

### Task 8: `dns` gate green + full sweep

- [ ] **Step 1: Run** `./run_qemu_native_ethernet.sh dns`
Expected: VCOM shows `DNS_OK ip=…` (SLIRP DNS at 10.0.2.3); `PASS: native_ethernet_test dns`.
- [ ] **Step 2: If FAIL** — systematic-debugging (suspects: `fnet_dns_init` params, DNS server address plumbed from DHCP vs the 10.0.2.3 fallback, the `DNSClient` busy-wait needing the IntervalTimer pump).
- [ ] **Step 3: Full sweep — all five phases in one session**

```bash
for p in boot server udp client dns; do ./run_qemu_native_ethernet.sh $p || break; done
```
Expected: five `PASS:` lines.
- [ ] **Step 4: Commit any sketch/runner deltas to evkb (exact paths only)**

```bash
cd ~/Development/rt1170/evkb
git add native_ethernet_test/native_ethernet_test.cpp native_ethernet_test/run_qemu_native_ethernet.sh
git commit -m "test(ethernet-m4): 5 QEMU gates green (boot/server/udp/client/dns) for FNET+NativeEthernet"
```
(Skip the commit if nothing changed since Task 2 — check `git status` first.)

---

### Task 9: HW verification on the RT1170-EVKB (the arbiter)

**Precondition (physical):** board on USB debug + **10/100 RJ45** (not the 1G port) into the router; **SD card REMOVED** (AD_32 = MDC muxes with SD1_CD_B on REVC). Mac on the same LAN via Wi-Fi.

**Files:**
- Create: `~/Development/rt1170/evkb/native_ethernet_test/HW-RESULTS.md`

- [ ] **Step 1: Kill stale debug servers, start the VCOM reader BEFORE flashing**

```bash
pkill LinkServer; pkill redlinkserv; sleep 1
cd ~/Development/rt1170/evkb/native_ethernet_test
python3 - <<'EOF' > hw_vcom.log 2>&1 &
import serial, sys
s = serial.Serial('/dev/cu.usbmodem5DQ2DDHVWO5EI3', 115200, timeout=1)
while True:
    d = s.read(256)
    if d: sys.stdout.write(d.decode('utf-8','replace')); sys.stdout.flush()
EOF
```
(Reader first — `cat` resets baud; pyserial only. `hw_vcom.log` stays untracked, like the other gate artifacts.)

- [ ] **Step 2: Flash**

```bash
cd ~/Development/rt1170/evkb/native_ethernet_test
/Applications/LinkServer_*/LinkServer run MIMXRT1176:MIMXRT1170-EVKB build/native_ethernet_test.elf
```

- [ ] **Step 3: Read the VCOM log — expect the boot sequence on the real network**

Expected within ~30 s: `ETH_BOOT`, `ETH_DHCP ok=1 ip=192.168.1.x` (router lease), `ETH_NETIF_UP`, then `HTTP_GET=PASS` (adaptive client path — non-SLIRP lease) and `DNS_OK ip=…` (router DNS). Record the actual lease IP.

- [ ] **Step 4: From the Mac — ping, TCP echo, UDP echo**

```bash
BOARD=192.168.1.x   # from step 3
ping -c 5 $BOARD
python3 - "$BOARD" <<'EOF'
import socket, sys
b = sys.argv[1]
t = socket.create_connection((b, 7), timeout=5); t.sendall(b"HW-TCP-PROBE"); print("TCP:", t.recv(64)); t.close()
u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); u.settimeout(5)
u.sendto(b"HW-UDP-PROBE", (b, 7)); print("UDP:", u.recvfrom(64)[0])
EOF
```
Expected: 5/5 ping replies; `TCP: b'HW-TCP-PROBE'`; `UDP: b'HW-UDP-PROBE'`.

- [ ] **Step 5: Write `HW-RESULTS.md`** — record date, ELF, lease IP, ping stats, TCP/UDP echo transcripts, `HTTP_GET=PASS`, `DNS_OK ip=…` lines, and any anomalies. Follow the format of `ethernet_test/HW-RESULTS.md`.

- [ ] **Step 6: Commit (exact paths)**

```bash
cd ~/Development/rt1170/evkb
git add native_ethernet_test/HW-RESULTS.md
git commit -m "test(ethernet-m4): HW-verify FNET+NativeEthernet on MIMXRT1170-EVKB (DHCP lease, ping, TCP+UDP echo :7, DNS, outbound HTTP GET)"
```

- [ ] **Step 7: If any HW check fails** — superpowers:systematic-debugging with the milestone-1 playbook: MDIO dead → MSCR/MII_SPEED and pin mux; link up but zero frames → SysPll1 (read `SYS_PLL1_CTRL`, expect `0x62002000`-ish, and re-check GPR4 REF_CLK_DIR); RX counter alive but drops → endianness/BD layout (should be impossible — same convention). Do NOT modify `enet.c`.

---

### Task 10: Wrap-up

- [ ] **Step 1:** Re-run the full QEMU sweep one final time (all 5 phases) and paste the five PASS lines into the task report.
- [ ] **Step 2:** `git -C ~/Development/FNET log --oneline -5`, `git -C ~/Development/NativeEthernet log --oneline -3`, `git -C ~/Development/rt1170/evkb log --oneline -5` — confirm every commit from Tasks 1-9 is present, working trees clean (evkb: only the known pre-existing unrelated entries). **No pushes.**
- [ ] **Step 3 (coordinator, not subagent):** write the project memory note (`rt1176-fnet-nativeethernet.md`) + MEMORY.md index line.

---

## Plan self-review notes

- **Spec coverage:** driver strategy A (Tasks 3-4), NativeEthernet vendoring (Task 1), gate harness + 5 phases (Tasks 2, 5-8), HW verification + HW-RESULTS.md (Task 9), licensing (NOTICE Task 3, README inventory Task 1), checksum-offload-off + `.dmabuffers` + vector 153 + PHY 3 + MSCR 47 (Task 3), repo discipline (per-task commit blocks). Deferred items (multicast/mDNS/TLS/timestamps) require no tasks — TLS is explicitly forced off (Task 3 Step 2); mDNS service dirs compile but stay dormant.
- **Known risk register:** first interrupt-driven ENET RX (Task 3 Step 11 exercises it); NativeEthernet no-op FNET mutexes + ISR-context `fnet_poll` (Teensy-4.1-proven — do not "fix" preemptively); `EthernetMDNS MDNS` global lives in `NativeMdns.cpp` and is only linked if referenced (sketch doesn't).
- **Type consistency:** all `fnet1176_*` helpers defined in Task 4 Step 1 before use in Steps 2-3; sketch markers match runner greps (`ETH_NETIF_UP`, `CLIENT_ECHO=PASS`, `DNS_OK ip=`).
