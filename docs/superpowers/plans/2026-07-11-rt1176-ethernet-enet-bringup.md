# RT1176 Ethernet (10/100 ENET FEC + RTL8201 + ARP/ICMP ping) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bring up the 10/100 ENET (Freescale FEC) MAC + RTL8201 RMII PHY on the MIMXRT1176-EVKB CM7 and make the board answer a ping, gate-first in QEMU (reusing the `imx.enet` model) then HW-verified.

**Architecture:** A new bare-register core driver `cores/imxrt1176/enet.c` (MAC + PHY + MDIO + raw flat-buffer TX/RX), ported from FNET `fnet_fec` with the SDK `enet` example as ground truth for the 1176 pins/clock/reset. A hand-rolled ARP + ICMP-echo responder lives in the gate sketch (application logic, not the core; lwIP replaces it in milestone 2). One QEMU one-liner (`phy-num` 2→3) makes the emulated PHY answer at the RTL8201's real MDIO address. A single `evkb/enet_test/` gate accumulates TX/RX → link → ping tokens against a Python socket peer.

**Tech Stack:** C (Arduino/Teensyduino core, ARM GCC 10.2.1), QEMU `mimxrt1170-evk` machine (`imx.enet` FEC model), Python 3 (gate peer over QEMU socket netdev), LinkServer + pyserial (HW).

**Spec:** `evkb/docs/superpowers/specs/2026-07-11-rt1176-ethernet-enet-bringup-design.md`

**Repos (commit to `master` in each; push ONLY when the user asks):**
- Core: `git -C ~/Development/rt1170/evkb/cores/imxrt1176` (repo root is `evkb/cores`, a nested repo).
- Gate: `git -C ~/Development/rt1170/evkb` (local repo; working tree shared across sessions — `git add` ONLY the files each task touches, never `git add -A`).
- QEMU: `git -C ~/Development/qemu2`.

**Global cautions baked into the tasks below:**
- `imxrt1176.h` is AUTO-GENERATED — every register def goes in **both** `imxrt1176.h` AND `tools/gen_imxrt1176_h.py` (edit the generator, then regenerate).
- Adding the NEW core file `enet.c` triggers the CMake `file(GLOB)` trap (no `CONFIGURE_DEPENDS`): the gate dir needs `rm -rf build && cmake -B build -S . && cmake --build build`, not an incremental rebuild.
- SDK-first: read the cited SDK/FNET source before writing any register; never guess a value. Cross-check every offset/bit against `MIMXRT1176_cm7_COMMON.h` / `RT1170/periph/PERI_ENET.h`.
- HW is the final arbiter (Task 6). A green QEMU gate proves plumbing, not silicon.

---

## File Structure

**Create:**
- `cores/imxrt1176/enet.h` — the internal C API (init, raw TX/RX, MDIO, link).
- `cores/imxrt1176/enet.c` — MAC + PHY + MDIO + descriptor-ring driver.
- `evkb/enet_test/` — the gate: `enet_test.cpp` (sketch + ARP/ICMP responder), `run_qemu_enet.sh` (runner), `enet_peer.py` (Python wire peer), `CMakeLists.txt`, `toolchain/rt1170-evkb.toolchain.cmake`.
- `evkb/enet_test/HW-RESULTS.md` — hardware verification record (Task 6).

**Modify:**
- `cores/imxrt1176/tools/gen_imxrt1176_h.py` — add ENET to `WANTED` + a hand-authored ENET/GPR4/GPR28/clock block.
- `cores/imxrt1176/imxrt1176.h` — regenerated output (do not hand-edit; regenerate).
- `cores/imxrt1176/core_pins.h` — add `IRQ_ENET = 137` to `IRQ_NUMBER_t`.
- `qemu2/include/hw/arm/fsl-imxrt1170.h:173` — `FSL_IMXRT1170_ENET_PHY_NUM` 2 → 3.

**Shared constants (used across sketch + peer — keep identical):**
- Board MAC `ENET_MAC = 02:00:00:00:00:01` (locally-administered unicast); Board IP `ENET_IP = 192.168.100.50`.
- Peer MAC `02:00:00:00:00:02`; Peer IP `192.168.100.1`.
- Probe EtherType `0x88B5` (IEEE local experimental); ARP `0x0806`; IPv4 `0x0800`.

---

## Task 1: ENET register plumbing (core header + generator + IRQ)

**Files:**
- Modify: `cores/imxrt1176/tools/gen_imxrt1176_h.py`
- Modify (regenerated): `cores/imxrt1176/imxrt1176.h`
- Modify: `cores/imxrt1176/core_pins.h`

- [ ] **Step 1: Read ground truth first.** Read `RT1170/periph/PERI_ENET.h` (the `ENET_Type` struct + `ENET_*` bit masks) and `RT1170/periph/PERI_IOMUXC_GPR.h` (GPR4/GPR28 fields), and confirm `ENET_BASE == 0x40424000` + `ENET_IRQn == 137` in `MIMXRT1176_cm7_COMMON.h`. Read how `semc.c` + the generator emit a peripheral block (the `L += [...]` idiom and the `CCM_CLOCK_ROOTn_CONTROL`/`CCM_LPCGnn_DIRECT` defs) so the ENET block matches house style.

- [ ] **Step 2: Add ENET to the generator.** In `tools/gen_imxrt1176_h.py`: add `"ENET":"ENET"` to the `WANTED` dict (auto-emits `#define ENET_BASE 0x40424000u`). Then append a hand-authored block to `L` near the SEMC/USDHC blocks:

```python
L += [
    "/* ==== ENET 10/100 (Freescale FEC) @ 0x40424000, IRQ 137 ==== */",
    "/* Register offsets verified against RT1170/periph/PERI_ENET.h */",
    "#define ENET_EIR   (*(volatile uint32_t *)(ENET_BASE + 0x004u))",  # interrupt event
    "#define ENET_EIMR  (*(volatile uint32_t *)(ENET_BASE + 0x008u))",  # interrupt mask
    "#define ENET_RDAR  (*(volatile uint32_t *)(ENET_BASE + 0x010u))",  # RX descriptor active
    "#define ENET_TDAR  (*(volatile uint32_t *)(ENET_BASE + 0x014u))",  # TX descriptor active
    "#define ENET_ECR   (*(volatile uint32_t *)(ENET_BASE + 0x024u))",  # ethernet control
    "#define ENET_MMFR  (*(volatile uint32_t *)(ENET_BASE + 0x040u))",  # MII management frame
    "#define ENET_MSCR  (*(volatile uint32_t *)(ENET_BASE + 0x044u))",  # MII speed control
    "#define ENET_MIBC  (*(volatile uint32_t *)(ENET_BASE + 0x064u))",
    "#define ENET_RCR   (*(volatile uint32_t *)(ENET_BASE + 0x084u))",  # receive control
    "#define ENET_TCR   (*(volatile uint32_t *)(ENET_BASE + 0x0C4u))",  # transmit control
    "#define ENET_PALR  (*(volatile uint32_t *)(ENET_BASE + 0x0E4u))",  # phys addr low
    "#define ENET_PAUR  (*(volatile uint32_t *)(ENET_BASE + 0x0E8u))",  # phys addr high
    "#define ENET_OPD   (*(volatile uint32_t *)(ENET_BASE + 0x0ECu))",
    "#define ENET_TFWR  (*(volatile uint32_t *)(ENET_BASE + 0x144u))",
    "#define ENET_RDSR  (*(volatile uint32_t *)(ENET_BASE + 0x180u))",  # RX ring base
    "#define ENET_TDSR  (*(volatile uint32_t *)(ENET_BASE + 0x184u))",  # TX ring base
    "#define ENET_MRBR  (*(volatile uint32_t *)(ENET_BASE + 0x188u))",  # max RX buffer size
    "#define ENET_TACC  (*(volatile uint32_t *)(ENET_BASE + 0x1C0u))",
    "#define ENET_RACC  (*(volatile uint32_t *)(ENET_BASE + 0x1C4u))",
    "/* ECR bits */",
    "#define ENET_ECR_RESET   (1u<<0)",
    "#define ENET_ECR_ETHEREN (1u<<1)",
    "#define ENET_ECR_EN1588  (1u<<4)",
    "#define ENET_ECR_DBSWP   (1u<<8)",   # descriptor byte-swap: SET for little-endian M7
    "/* EIR/EIMR bits */",
    "#define ENET_EIR_MII (1u<<23)",
    "#define ENET_EIR_RXF (1u<<25)",
    "#define ENET_EIR_TXF (1u<<27)",
    "/* RDAR/TDAR active bit */",
    "#define ENET_RDAR_ACTIVE (1u<<24)",
    "#define ENET_TDAR_ACTIVE (1u<<24)",
    "/* MSCR MII_SPEED field: MDC = ref/((MII_SPEED+1)*2); HOLDTIME in [10:8] */",
    "#define ENET_MSCR_MII_SPEED(n) (((n)&0x3Fu)<<1)",
    "/* MMFR fields (clause-22) */",
    "#define ENET_MMFR_ST_01   (1u<<30)",
    "#define ENET_MMFR_OP_READ (2u<<28)",
    "#define ENET_MMFR_OP_WRITE (1u<<28)",
    "#define ENET_MMFR_TA_10   (2u<<16)",
    "#define ENET_MMFR_PA(a)   (((a)&0x1Fu)<<23)",
    "#define ENET_MMFR_RA(r)   (((r)&0x1Fu)<<18)",
    "/* Legacy 8-byte buffer-descriptor status bits (big-endian field in the BD) */",
    "#define ENET_TXBD_R  (1u<<15)",   # ready (owned by DMA)
    "#define ENET_TXBD_TO1 (1u<<14)",
    "#define ENET_TXBD_W  (1u<<13)",   # wrap (last BD in ring)
    "#define ENET_TXBD_TO2 (1u<<12)",
    "#define ENET_TXBD_L  (1u<<11)",   # last in frame
    "#define ENET_TXBD_TC (1u<<10)",   # transmit CRC
    "#define ENET_RXBD_E  (1u<<15)",   # empty (owned by DMA)
    "#define ENET_RXBD_W  (1u<<13)",   # wrap
    "#define ENET_RXBD_L  (1u<<11)",   # last in frame
    "/* IOMUXC_GPR ENET fields */",
    "#define IOMUXC_GPR_GPR4  (*(volatile uint32_t *)(0x400E4010u))",
    "#define IOMUXC_GPR_GPR4_ENET_REF_CLK_DIR (1u<<1)",
    "#define IOMUXC_GPR_GPR28 (*(volatile uint32_t *)(0x400E4070u))",
    "#define IOMUXC_GPR_GPR28_CACHE_ENET (1u<<7)",
    "/* ENET clock root 51 (kCLOCK_Root_Enet1): mux=4 SysPll1Div2, div=10 -> 50 MHz. */",
    "/* Confirm the exact CCM_CLOCK_ROOT51 + ENET LPCG gate # against the SDK before use. */",
]
```

- [ ] **Step 3: Regenerate the header.** Run: `cd ~/Development/rt1170/evkb/cores/imxrt1176 && python3 tools/gen_imxrt1176_h.py`. Expected: `imxrt1176.h` rewritten; `git diff imxrt1176.h` shows ONLY additions (the ENET/GPR block + `ENET_BASE`). Verify: `grep -c 'ENET_ECR\|ENET_MMFR\|IOMUXC_GPR_GPR4_ENET_REF_CLK_DIR' imxrt1176.h` prints ≥ 3.

- [ ] **Step 4: Add the IRQ number.** In `core_pins.h`, add `IRQ_ENET = 137,` to the `IRQ_NUMBER_t` enum (near the other IRQ constants; 137 < the existing max 156 so the RAM vector table needs no resize). Verify: `grep 'IRQ_ENET' core_pins.h` shows the line.

- [ ] **Step 5: Compile smoke-test (existing gate still builds).** The header change must not break the core. Run:
```sh
cd ~/Development/rt1170/evkb/usb_data_test && rm -rf build && cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake >/dev/null && cmake --build build 2>&1 | tail -3
```
Expected: builds to `build/usb_data_test.elf` with no errors.

- [ ] **Step 6: Commit (core repo).**
```sh
git -C ~/Development/rt1170/evkb/cores/imxrt1176 add tools/gen_imxrt1176_h.py imxrt1176.h core_pins.h
git -C ~/Development/rt1170/evkb/cores/imxrt1176 commit -m "enet: add ENET/GPR4/GPR28/clock register defs + IRQ_ENET=137"
```

---

## Task 2: Gate scaffold (`evkb/enet_test/`) — verify the socket-netdev harness

Bootstraps the test harness BEFORE the driver exists, so Task 3+ has a working gate to fail against. Verifies the `-nic socket` + Python-peer plumbing (the one thing no existing gate does).

**Files:**
- Create: `evkb/enet_test/CMakeLists.txt`, `evkb/enet_test/toolchain/rt1170-evkb.toolchain.cmake`, `evkb/enet_test/enet_test.cpp`, `evkb/enet_test/run_qemu_enet.sh`, `evkb/enet_test/enet_peer.py`

- [ ] **Step 1: Clone the CMake + toolchain from usb_data_test.**
```sh
mkdir -p ~/Development/rt1170/evkb/enet_test/toolchain
cp ~/Development/rt1170/evkb/usb_data_test/toolchain/rt1170-evkb.toolchain.cmake ~/Development/rt1170/evkb/enet_test/toolchain/
```
Create `evkb/enet_test/CMakeLists.txt` (usb_data_test's, renamed):
```cmake
cmake_minimum_required(VERSION 3.24)
project(enet_test)
set(TEENSY_VERSION 117 CACHE STRING "")
include(FetchContent)
FetchContent_Declare(teensy_cmake_macros SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../teensy-cmake-macros)
FetchContent_MakeAvailable(teensy_cmake_macros)
include(${teensy_cmake_macros_SOURCE_DIR}/CMakeLists.include.txt)
import_arduino_library(cores ${CMAKE_CURRENT_LIST_DIR}/../cores/imxrt1176)
teensy_add_executable(enet_test enet_test.cpp)
teensy_target_link_libraries(enet_test cores)
target_link_libraries(enet_test.elf stdc++)
```

- [ ] **Step 2: Skeleton sketch** `evkb/enet_test/enet_test.cpp` (prints a boot token; does NOT yet reference `enet.h`):
```cpp
#include "Arduino.h"
#include "HardwareSerial.h"

void setup() {
    Serial1.begin(115200);          // debug VCOM (LPUART1)
    delay(50);
    Serial1.println("ENET_BOOT");
}
void loop() {}
```

- [ ] **Step 3: Python wire peer** `evkb/enet_test/enet_peer.py` — connects to the QEMU socket netdev and proves the harness is live. QEMU's socket netdev frames packets as a 4-byte big-endian length + raw ethernet frame.
```python
#!/usr/bin/env python3
"""QEMU socket-netdev peer for the ENET gate.
Phase arg selects behavior; prints result lines, exits 0 on success."""
import socket, sys, time, struct

def connect(host, port):
    deadline = time.time() + 10
    while time.time() < deadline:
        try:
            return socket.create_connection((host, port), timeout=1)
        except OSError:
            time.sleep(0.2)
    print("ERROR: could not connect to enet socket"); sys.exit(2)

def send_frame(sock, frame):
    sock.sendall(struct.pack(">I", len(frame)) + frame)

def recvall(sock, n):
    buf = b""
    while len(buf) < n:
        c = sock.recv(n - len(buf))
        if not c: raise EOFError
        buf += c
    return buf

def recv_frame(sock, timeout=5):
    sock.settimeout(timeout)
    (n,) = struct.unpack(">I", recvall(sock, 4))
    return recvall(sock, n)

if __name__ == "__main__":
    host, port, phase = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    sock = connect(host, port)
    print("PEER-CONNECTED phase=%s" % phase)
    # Task 2: just confirm the socket is live, then exit 0.
    sys.exit(0)
```

- [ ] **Step 4: Runner** `evkb/enet_test/run_qemu_enet.sh` (adapted from usb_data_test; chardev → `-nic socket`):
```sh
#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/enet_test.elf"
VCOM="$DIR/vcom.uart"; DBG="$DIR/enet.dbg"; RES="$DIR/enet.result"
gate_tmp "$RES"
PORT=15556
PHASE="${1:-boot}"
rm -f "$VCOM" "$DBG" "$RES"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none \
    -serial file:"$VCOM" \
    -nic socket,listen=127.0.0.1:$PORT,model=imx.enet \
    -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
python3 "$DIR/enet_peer.py" 127.0.0.1 $PORT "$PHASE" > "$RES" 2>&1
RC=$?
sleep 1; kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== VCOM ===="; cat "$VCOM" 2>/dev/null || true
echo "==== peer ===="; cat "$RES"
[ $RC -eq 0 ] || { echo "FAIL: peer rc=$RC"; exit 1; }
grep -q "ENET_BOOT" "$VCOM" || { echo "FAIL: no ENET_BOOT"; exit 1; }
echo "PASS: enet_test harness live (boot + socket peer)"
chmod +x run_qemu_enet.sh
```
Then: `chmod +x ~/Development/rt1170/evkb/enet_test/run_qemu_enet.sh`.

- [ ] **Step 5: Build + run — verify the harness.**
```sh
cd ~/Development/rt1170/evkb/enet_test && rm -rf build && cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake >/dev/null && cmake --build build 2>&1 | tail -2
sh run_qemu_enet.sh boot
```
Expected: `ENET_BOOT` on VCOM, `PEER-CONNECTED phase=boot` from the peer, `PASS: enet_test harness live`. **If QEMU rejects `-nic socket,listen=...,model=imx.enet`**, this is the moment to reconcile the exact on-SoC-NIC binding syntax (per the spec's known open item) — try `-nic socket,connect=` with the peer as server, or `-netdev socket,id=n0,listen=... -net nic,netdev=n0,model=imx.enet`, until the peer connects and the guest boots. Record the working form.

- [ ] **Step 6: Commit (gate repo — only these files).**
```sh
git -C ~/Development/rt1170/evkb add enet_test/CMakeLists.txt enet_test/toolchain enet_test/enet_test.cpp enet_test/run_qemu_enet.sh enet_test/enet_peer.py
git -C ~/Development/rt1170/evkb commit -m "enet_test: gate scaffold (socket-netdev peer harness)"
```

---

## Task 3: `enet.c` MAC bring-up + Gate 1 (raw frame TX/RX)

**Files:**
- Create: `cores/imxrt1176/enet.h`, `cores/imxrt1176/enet.c`
- Modify: `evkb/enet_test/enet_test.cpp`, `evkb/enet_test/enet_peer.py`

- [ ] **Step 1: Write the API header** `cores/imxrt1176/enet.h`:
```c
#ifndef ENET_H
#define ENET_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void     enet_init(const uint8_t mac[6]);
int      enet_send_frame(const uint8_t *frame, uint16_t len); /* 0 ok, <0 timeout */
int      enet_read_frame(uint8_t *buf, uint16_t *len);        /* 1 got, 0 none, <0 err */
uint16_t enet_mdio_read(uint8_t phy, uint8_t reg);            /* implemented in Task 4 */
void     enet_mdio_write(uint8_t phy, uint8_t reg, uint16_t v);/* implemented in Task 4 */
int      enet_phy_link_up(uint32_t timeout_ms);               /* implemented in Task 4 */
#ifdef __cplusplus
}
#endif
#endif
```

- [ ] **Step 2: Write the Gate-1 sketch** (the failing test) — `enet_test.cpp`:
```cpp
#include "Arduino.h"
#include "HardwareSerial.h"
#include "enet.h"

static const uint8_t ENET_MAC[6] = {0x02,0x00,0x00,0x00,0x00,0x01};
static const uint8_t PROBE_TX[]  = {
    0x02,0x00,0x00,0x00,0x00,0x02,  /* dst = peer MAC */
    0x02,0x00,0x00,0x00,0x00,0x01,  /* src = board MAC */
    0x88,0xB5,                      /* experimental ethertype */
    'E','N','E','T','-','T','X','-','P','R','O','B','E'
};

void setup() {
    Serial1.begin(115200); delay(50);
    Serial1.println("ENET_BOOT");
    enet_init(ENET_MAC);
    Serial1.println("ENET_INIT_DONE");
}

void loop() {
    static uint32_t t0 = millis();
    /* TX: emit a probe the peer will assert on. */
    static bool sent = false;
    if (!sent && (millis() - t0) > 500) {
        int r = enet_send_frame(PROBE_TX, sizeof(PROBE_TX));
        Serial1.print("ENET_TX="); Serial1.println(r == 0 ? "PASS" : "FAIL");
        sent = true;
    }
    /* RX: read a peer-injected frame, check its payload. */
    uint8_t buf[1522]; uint16_t len = 0;
    if (enet_read_frame(buf, &len) == 1) {
        bool ok = (len >= 14) && (buf[12]==0x88 && buf[13]==0xB5)
                  && len >= 27 && memcmp(&buf[14], "ENET-RX-PROBE", 13) == 0;
        Serial1.print("ENET_RX="); Serial1.println(ok ? "PASS" : "FAIL");
    }
}
```

- [ ] **Step 3: Write `enet.c` MAC bring-up.** Port from FNET `~/Development/FNET/src/port/netif/fec/fnet_fec.c` (ring init `:201-281`, `fnet_fec_output :674`, `_fnet_fec_input :534`) — cut FNET's `netbuf`/`_fnet_eth_input` seams for flat `memcpy`. Pins/clock/GPR/reset from the SDK `enet/txrx_transfer/{cm7/hardware_init.c,pin_mux.c}`. **Verify every register bit against `PERI_ENET.h` + `fnet_fec.h` as you port.** Concrete structure:
```c
#include <stdint.h>
#include <string.h>
#include "imxrt1176.h"
#include "core_pins.h"   /* delayMicroseconds, GPIO12 defs */
#include "enet.h"

#define ENET_RXBD_NUM 4
#define ENET_TXBD_NUM 4
#define ENET_BUF_SZ   1536      /* >=1518, 64-aligned */

/* Legacy 8-byte FEC buffer descriptor. NOTE: status/length are BIG-ENDIAN in
   the BD; we set ECR.DBSWP so the FEC reads our little-endian words. Verify the
   struct + swap policy against fnet_fec.h fnet_fec_buf_desc_t. */
typedef struct __attribute__((packed)) {
    uint16_t status;   /* R/E,W,L,... */
    uint16_t length;
    uint32_t buf;      /* buffer pointer */
} enet_bd_t;

/* DMA-reachable memory: OCRAM via DMAMEM (never DTCM/ITCM) — ERR050396. */
DMAMEM __attribute__((aligned(64))) static enet_bd_t rx_bd[ENET_RXBD_NUM];
DMAMEM __attribute__((aligned(64))) static enet_bd_t tx_bd[ENET_TXBD_NUM];
DMAMEM __attribute__((aligned(64))) static uint8_t   rx_buf[ENET_RXBD_NUM][ENET_BUF_SZ];
DMAMEM __attribute__((aligned(64))) static uint8_t   tx_buf[ENET_TXBD_NUM][ENET_BUF_SZ];
static uint32_t rx_idx, tx_idx;

static inline uint16_t bswap16(uint16_t v){ return (uint16_t)((v>>8)|(v<<8)); }

static void enet_clock_init(void) {
    /* Route CCM_CLOCK_ROOT51 (Enet1) mux=4 (SysPll1Div2) div=10 -> 50 MHz, then
       ungate the ENET LPCG. Ensure SysPll1 Div2 is enabled — but FIRST read the
       SDK BOARD_InitModuleClock + check what SysPll1 already drives (do NOT blindly
       re-init: SYS_PLL2 lesson). Fill exact CCM_CLOCK_ROOT51_CONTROL/LPCG per SDK. */
    /* ... CCM writes ... */
}

static void enet_pins_init(void) {
    /* RMII mux (SDK pin_mux.c). MDC/MDIO default pad; SION=1 on REF_CLK/RXD0/RXD1.
       Write IOMUXC SW_MUX_CTL + SW_PAD_CTL for each:
         AD_32=ENET_MDC, AD_33=ENET_MDIO,
         DISP_B2_02..04 = TXD0/TXD1/TX_EN (pad 0x02),
         DISP_B2_05 = REF_CLK (SION=1, pad 0x03),
         DISP_B2_06/07 = RXD0/RXD1 (SION=1, pad 0x06),
         DISP_B2_08/09 = RX_EN/RX_ER (pad 0x06).
       Use the exact IOMUXC_GPIO_* mux macro values from the SDK. */
    IOMUXC_GPR_GPR4  |= IOMUXC_GPR_GPR4_ENET_REF_CLK_DIR;   /* drive 50 MHz out */
    IOMUXC_GPR_GPR28 &= ~IOMUXC_GPR_GPR28_CACHE_ENET;       /* ERR050396 */
}

static void enet_phy_reset(void) {
    /* GPIO12_IO12 (pad GPIO_LPSR_12 -> GPIO12_IO12 per SDK pin_mux.c). Output low,
       10 ms, high, 150 ms. Use the core GPIO12 GDIR/DR regs. QEMU no-op; HW real. */
    /* ... set mux, GPIO12 GDIR bit12=1, DR bit12=0; delayMicroseconds(10000);
           DR bit12=1; delayMicroseconds(150000); ... */
}

static void enet_ring_init(void) {
    for (int i=0;i<ENET_RXBD_NUM;i++){
        rx_bd[i].status = bswap16(ENET_RXBD_E | (i==ENET_RXBD_NUM-1?ENET_RXBD_W:0));
        rx_bd[i].length = 0;
        rx_bd[i].buf = (uint32_t)&rx_buf[i][0];
    }
    for (int i=0;i<ENET_TXBD_NUM;i++){
        rx_idx=tx_idx=0;
        tx_bd[i].status = bswap16(i==ENET_TXBD_NUM-1?ENET_TXBD_W:0);
        tx_bd[i].length = 0;
        tx_bd[i].buf = (uint32_t)&tx_buf[i][0];
    }
}

void enet_init(const uint8_t mac[6]) {
    enet_clock_init();
    ENET_ECR = ENET_ECR_RESET;               /* soft reset */
    while (ENET_ECR & ENET_ECR_RESET) {}
    enet_pins_init();
    enet_phy_reset();
    enet_ring_init();
    ENET_PALR = ((uint32_t)mac[0]<<24)|((uint32_t)mac[1]<<16)|((uint32_t)mac[2]<<8)|mac[3];
    ENET_PAUR = ((uint32_t)mac[4]<<24)|((uint32_t)mac[5]<<16);
    ENET_EIMR = 0;                            /* polled: no IRQ */
    ENET_MSCR = ENET_MSCR_MII_SPEED(/*n from bus_clk so MDC<=2.5MHz*/ 24); /* verify n */
    ENET_RCR  = /* MII_MODE|RMII_MODE|MAX_FL(1518)|FCE ... per PERI_ENET.h */ 0;
    ENET_TCR  = /* FDEN (full duplex) */ 0;
    ENET_RDSR = (uint32_t)&rx_bd[0];
    ENET_TDSR = (uint32_t)&tx_bd[0];
    ENET_MRBR = ENET_BUF_SZ;
    ENET_ECR  = ENET_ECR_ETHEREN | ENET_ECR_DBSWP;   /* enable, little-endian BDs */
    ENET_RDAR = ENET_RDAR_ACTIVE;             /* arm RX */
}

int enet_send_frame(const uint8_t *frame, uint16_t len) {
    if (len > ENET_BUF_SZ) return -1;
    enet_bd_t *bd = &tx_bd[tx_idx];
    uint32_t spin = 0;
    while (bswap16(bd->status) & ENET_TXBD_R) { if (++spin > 1000000u) return -2; }
    memcpy((void*)bd->buf, frame, len);
    if (len < 60) { memset((uint8_t*)bd->buf + len, 0, 60 - len); len = 60; } /* pad */
    bd->length = bswap16(len);
    uint16_t st = ENET_TXBD_R | ENET_TXBD_L | ENET_TXBD_TC;
    if (tx_idx == ENET_TXBD_NUM-1) st |= ENET_TXBD_W;
    bd->status = bswap16(st);
    __asm volatile("dmb":::"memory");
    ENET_TDAR = ENET_TDAR_ACTIVE;
    tx_idx = (tx_idx+1) % ENET_TXBD_NUM;
    return 0;
}

int enet_read_frame(uint8_t *out, uint16_t *outlen) {
    enet_bd_t *bd = &rx_bd[rx_idx];
    if (bswap16(bd->status) & ENET_RXBD_E) return 0;   /* still DMA-owned */
    uint16_t st = bswap16(bd->status);
    int rc = 1;
    uint16_t len = bswap16(bd->length);
    if (st & 0x0037 /* LG/NO/CR/OV/TR error bits — verify masks vs PERI_ENET.h */) rc = -1;
    else { if (len > ENET_BUF_SZ) len = ENET_BUF_SZ; memcpy(out, (void*)bd->buf, len); *outlen = len; }
    uint16_t ns = ENET_RXBD_E | (rx_idx==ENET_RXBD_NUM-1?ENET_RXBD_W:0);
    bd->status = bswap16(ns);
    __asm volatile("dmb":::"memory");
    ENET_RDAR = ENET_RDAR_ACTIVE;                      /* re-arm */
    rx_idx = (rx_idx+1) % ENET_RXBD_NUM;
    return rc;
}

/* enet_mdio_read/write + enet_phy_link_up: implemented in Task 4. */
```

- [ ] **Step 4: Extend the peer for Gate 1** — `enet_peer.py`, add a `phase == "mac"` branch in `__main__` before the boot `sys.exit(0)`:
```python
    if phase == "mac":
        time.sleep(1.0)  # let the guest reach enet_init
        # Inject an RX probe the guest asserts on (dst=board, src=peer, ethertype 0x88B5).
        rx = bytes.fromhex("020000000001" "020000000002") + b"\x88\xb5" + b"ENET-RX-PROBE"
        send_frame(sock, rx)
        # Receive the guest's TX probe and assert it.
        try:
            f = recv_frame(sock, timeout=6)
        except (EOFError, socket.timeout):
            print("FAIL: no TX frame from guest"); sys.exit(1)
        ok = f[12:14] == b"\x88\xb5" and f[14:14+13] == b"ENET-TX-PROBE"
        print("TX-FRAME=%r ok=%s" % (f[:32], ok))
        sys.exit(0 if ok else 1)
```
Update the runner's PASS check for the `mac` phase: after the existing boot checks, add `grep -q "ENET_RX=PASS" "$VCOM"` and `grep -q "ENET_TX=PASS" "$VCOM"` (guard both under `[ "$PHASE" = mac ]`).

- [ ] **Step 5: Reconfigure (GLOB trap) + run — verify FAIL then PASS.** Adding `enet.c` is a new core file:
```sh
cd ~/Development/rt1170/evkb/enet_test && rm -rf build && cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake >/dev/null && cmake --build build 2>&1 | tail -2
sh run_qemu_enet.sh mac
```
Expected once `enet.c` is complete: VCOM shows `ENET_INIT_DONE`, `ENET_TX=PASS`, `ENET_RX=PASS`; peer prints `TX-FRAME=... ok=True`; runner `PASS`. (Fail-first: with the `enet_clock_init`/`enet_pins_init`/`ENET_RCR`/`ENET_TCR`/`MSCR` bodies left as `0`/stubs, the rings won't move and the gate FAILS — implement the real register writes from the SDK/FNET until it passes.)

- [ ] **Step 6: Commit (two repos).**
```sh
git -C ~/Development/rt1170/evkb/cores/imxrt1176 add enet.h enet.c
git -C ~/Development/rt1170/evkb/cores/imxrt1176 commit -m "enet: MAC bring-up + raw flat-buffer TX/RX on DMAMEM rings"
git -C ~/Development/rt1170/evkb add enet_test/enet_test.cpp enet_test/enet_peer.py enet_test/run_qemu_enet.sh
git -C ~/Development/rt1170/evkb commit -m "enet_test: Gate 1 raw frame TX/RX"
```

---

## Task 4: MDIO + clause-22 PHY link + Gate 2 + QEMU phy-num fix

The fail-first here is the star: with QEMU's default `phy-num=2`, an MDIO read at the RTL8201's address 3 returns `0xffff` (link-bit set → false "up", PHYID garbage). Reading PHYID and asserting it ≠ `0xffff` fails; the QEMU 2→3 fix makes it pass.

**Files:**
- Modify: `cores/imxrt1176/enet.c` (add MDIO + link)
- Modify: `qemu2/include/hw/arm/fsl-imxrt1170.h:173`
- Modify: `evkb/enet_test/enet_test.cpp`

- [ ] **Step 1: Add MDIO + clause-22 link to `enet.c`** (port MDIO from FNET `_fnet_fec_phy_read/_write` `:925/:984`):
```c
uint16_t enet_mdio_read(uint8_t phy, uint8_t reg) {
    ENET_EIR = ENET_EIR_MII;                 /* clear */
    ENET_MMFR = ENET_MMFR_ST_01 | ENET_MMFR_OP_READ | ENET_MMFR_PA(phy)
              | ENET_MMFR_RA(reg) | ENET_MMFR_TA_10;
    uint32_t spin=0; while (!(ENET_EIR & ENET_EIR_MII)) { if (++spin>1000000u) return 0xffff; }
    ENET_EIR = ENET_EIR_MII;
    return (uint16_t)(ENET_MMFR & 0xFFFF);
}
void enet_mdio_write(uint8_t phy, uint8_t reg, uint16_t val) {
    ENET_EIR = ENET_EIR_MII;
    ENET_MMFR = ENET_MMFR_ST_01 | ENET_MMFR_OP_WRITE | ENET_MMFR_PA(phy)
              | ENET_MMFR_RA(reg) | ENET_MMFR_TA_10 | val;
    uint32_t spin=0; while (!(ENET_EIR & ENET_EIR_MII)) { if (++spin>1000000u) return; }
    ENET_EIR = ENET_EIR_MII;
}
/* Generic clause-22, PHY addr passed by caller. No vendor pages, no OUI check. */
int enet_phy_link_up(uint32_t timeout_ms) {
    const uint8_t A = 3;                      /* RTL8201 MDIO address */
    enet_mdio_write(A, 0, 0x8000);            /* BMCR soft reset */
    uint32_t s=0; while ((enet_mdio_read(A,0) & 0x8000) && ++s<100000u) {}
    enet_mdio_write(A, 4, 0x01E1);            /* ANAR: 100F/100H/10F/10H + 802.3 */
    enet_mdio_write(A, 0, 0x1200);            /* BMCR: autoneg enable + restart */
    uint32_t t0 = millis();
    do {
        (void)enet_mdio_read(A,1);            /* BMSR latches low: read twice */
        if (enet_mdio_read(A,1) & 0x0004) return 1;   /* link up */
        delay(5);
    } while ((millis()-t0) < timeout_ms);
    return 0;
}
```

- [ ] **Step 2: Add the Gate-2 phase to the sketch** (`enet_test.cpp`, in `setup()` after `ENET_INIT_DONE`):
```cpp
    uint16_t id1 = enet_mdio_read(3, 2), id2 = enet_mdio_read(3, 3);
    Serial1.print("ENET_PHYID="); Serial1.print(id1, HEX); Serial1.print(":"); Serial1.println(id2, HEX);
    int link = enet_phy_link_up(3000);
    Serial1.print("ENET_LINK="); Serial1.println(link ? "PASS" : "FAIL");
    /* False-pass guard: a real MDIO round-trip returns a real ID, not 0xffff/0x0000. */
    bool idok = (id1 != 0xFFFF && id1 != 0x0000);
    Serial1.print("ENET_PHYID_OK="); Serial1.println(idok ? "PASS" : "FAIL");
```

- [ ] **Step 3: Run against UNPATCHED QEMU — verify the false-pass FAILS.**
```sh
cd ~/Development/rt1170/evkb/enet_test && cmake --build build 2>&1 | tail -1
sh run_qemu_enet.sh mac
```
Expected: `ENET_PHYID=FFFF:FFFF`, `ENET_PHYID_OK=FAIL` (and `ENET_LINK` may spuriously read PASS off the `0xffff` link bit — exactly the trap). This is the fail-first.

- [ ] **Step 4: Apply the QEMU fix.** In `qemu2/include/hw/arm/fsl-imxrt1170.h:173`, change `#define FSL_IMXRT1170_ENET_PHY_NUM 2` → `3`. Rebuild:
```sh
cd ~/Development/qemu2/build && ninja qemu-system-arm 2>&1 | tail -2
```

- [ ] **Step 5: Re-run — verify PASS.**
```sh
cd ~/Development/rt1170/evkb/enet_test && sh run_qemu_enet.sh mac
```
Expected: `ENET_PHYID=7:C0D1` (QEMU's LAN9118 ID — non-`0xffff`), `ENET_PHYID_OK=PASS`, `ENET_LINK=PASS`. Add `grep -q "ENET_LINK=PASS"` and `grep -q "ENET_PHYID_OK=PASS"` to the runner's `mac`-phase checks.

- [ ] **Step 6: Commit (two repos).**
```sh
git -C ~/Development/qemu2 add include/hw/arm/fsl-imxrt1170.h
git -C ~/Development/qemu2 commit -m "imxrt1170: ENET PHY MDIO address 2->3 (match RTL8201 on EVKB)"
git -C ~/Development/rt1170/evkb/cores/imxrt1176 add enet.c
git -C ~/Development/rt1170/evkb/cores/imxrt1176 commit -m "enet: MDIO + generic clause-22 PHY link-up (addr 3)"
git -C ~/Development/rt1170/evkb add enet_test/enet_test.cpp enet_test/run_qemu_enet.sh
git -C ~/Development/rt1170/evkb commit -m "enet_test: Gate 2 PHY link + PHYID false-pass guard"
```

---

## Task 5: Hand-rolled ARP + ICMP-echo responder + Gate 3 (answer a ping)

The responder is application logic in the sketch (lwIP replaces it in milestone 2). The peer plays the pinging host.

**Files:**
- Modify: `evkb/enet_test/enet_test.cpp` (responder + poll loop)
- Modify: `evkb/enet_test/enet_peer.py` (ARP + ICMP request/assert)
- Modify: `evkb/enet_test/run_qemu_enet.sh` (ping-phase PASS checks)

- [ ] **Step 1: Add the responder to the sketch.** Static IP + helpers + `enet_poll()`:
```cpp
static const uint8_t ENET_IP[4] = {192,168,100,50};

static uint16_t inet_cksum(const uint8_t *p, int n) {
    uint32_t s = 0;
    for (int i=0;i+1<n;i+=2) s += (p[i]<<8)|p[i+1];
    if (n & 1) s += p[n-1]<<8;
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)~s;
}
static void handle_arp(uint8_t *f, uint16_t len) {
    if (len < 42) return;
    if (!(f[20]==0x00 && f[21]==0x01)) return;              /* oper=request */
    if (memcmp(&f[38], ENET_IP, 4) != 0) return;            /* target IP == us */
    uint8_t reply[60]; memset(reply,0,sizeof(reply));
    memcpy(&reply[0], &f[6], 6);                            /* dst = requester */
    memcpy(&reply[6], ENET_MAC, 6);                         /* src = us */
    reply[12]=0x08; reply[13]=0x06;
    memcpy(&reply[14], &f[14], 6);                          /* htype/ptype/hlen/plen */
    reply[20]=0x00; reply[21]=0x02;                        /* oper=reply */
    memcpy(&reply[22], ENET_MAC, 6); memcpy(&reply[28], ENET_IP, 4);   /* sender */
    memcpy(&reply[32], &f[22], 6);  memcpy(&reply[38], &f[28], 4);     /* target = requester */
    enet_send_frame(reply, 60);
    Serial1.println("ENET_ARP=PASS");
}
static void handle_ipv4(uint8_t *f, uint16_t len) {
    if (len < 34) return;
    uint8_t ihl = (f[14] & 0x0F) * 4;
    if (f[23] != 1) return;                                 /* proto ICMP */
    if (memcmp(&f[30], ENET_IP, 4) != 0) return;            /* dst IP == us */
    uint16_t icmp = 14 + ihl;
    if (f[icmp] != 8) return;                               /* echo request */
    uint8_t tmp[6];
    memcpy(tmp,&f[0],6); memcpy(&f[0],&f[6],6); memcpy(&f[6],tmp,6);   /* swap MAC */
    for (int i=0;i<4;i++){ uint8_t t=f[26+i]; f[26+i]=f[30+i]; f[30+i]=t; } /* swap IP */
    f[icmp] = 0;                                            /* echo reply */
    f[16]=0; f[17]=0; uint16_t ic = inet_cksum(&f[14], ihl); f[16]=ic>>8; f[17]=ic;  /* IP cksum */
    f[icmp+2]=0; f[icmp+3]=0;
    uint16_t cc = inet_cksum(&f[icmp], len - icmp); f[icmp+2]=cc>>8; f[icmp+3]=cc;    /* ICMP cksum */
    enet_send_frame(f, len);
    Serial1.println("ENET_PING=PASS");
}
static void enet_poll(void) {
    uint8_t buf[1522]; uint16_t len=0;
    while (enet_read_frame(buf,&len) == 1) {
        if (len < 14) continue;
        uint16_t et = (buf[12]<<8)|buf[13];
        if (et == 0x0806) handle_arp(buf,len);
        else if (et == 0x0800) handle_ipv4(buf,len);
    }
}
```
Replace `loop()` with `void loop() { enet_poll(); }` and drop the Gate-1 TX/RX probe code from `loop()` (keep the Gate-1/Gate-2 tokens in `setup()`).

- [ ] **Step 2: Add the ping phase to the peer.** `enet_peer.py`, `phase == "ping"` branch — craft ARP + ICMP with `struct`, assert the exact replies:
```python
    if phase == "ping":
        time.sleep(1.5)  # let the guest finish init + link
        BOARD_MAC = bytes.fromhex("020000000001"); PEER_MAC = bytes.fromhex("020000000002")
        BOARD_IP = bytes([192,168,100,50]); PEER_IP = bytes([192,168,100,1])
        def cksum(b):
            s=0
            for i in range(0,len(b)-1,2): s += (b[i]<<8)|b[i+1]
            if len(b)&1: s += b[-1]<<8
            while s>>16: s=(s&0xffff)+(s>>16)
            return (~s)&0xffff
        # 1) ARP request "who has BOARD_IP".
        arp = (BOARD_MAC.replace(BOARD_MAC, b"\xff"*6) + PEER_MAC + b"\x08\x06"
               + b"\x00\x01\x08\x00\x06\x04\x00\x01" + PEER_MAC + PEER_IP + b"\x00"*6 + BOARD_IP)
        send_frame(sock, arp)
        r = recv_frame(sock, 6)
        arp_ok = (r[12:14]==b"\x08\x06" and r[20:22]==b"\x00\x02"
                  and r[22:28]==BOARD_MAC and r[28:32]==BOARD_IP)
        print("ARP-REPLY ok=%s %r" % (arp_ok, r[:42]))
        # 2) ICMP echo request.
        ident, seq, data = 0x1234, 1, b"abcdefghij"
        icmp = struct.pack(">BBHHH", 8,0,0,ident,seq) + data
        icmp = icmp[:2] + struct.pack(">H", cksum(icmp)) + icmp[4:]
        ipv4 = struct.pack(">BBHHHBBH", 0x45,0,20+len(icmp),0,0,64,1,0) + PEER_IP + BOARD_IP
        ipv4 = ipv4[:10] + struct.pack(">H", cksum(ipv4)) + ipv4[12:]
        eth = BOARD_MAC + PEER_MAC + b"\x08\x00" + ipv4 + icmp
        send_frame(sock, eth)
        r = recv_frame(sock, 6)
        off = 14 + (r[14]&0x0F)*4
        icmp_ok = (r[12:14]==b"\x08\x00" and r[off]==0 and r[off+8:off+8+len(data)]==data
                   and cksum(r[off:off+8+len(data)])==0)
        print("ICMP-REPLY ok=%s type=%d" % (icmp_ok, r[off]))
        sys.exit(0 if (arp_ok and icmp_ok) else 1)
```

- [ ] **Step 3: Wire the runner's ping-phase checks.** In `run_qemu_enet.sh`, when `PHASE = ping`, require `grep -q "ENET_ARP=PASS" "$VCOM"` and `grep -q "ENET_PING=PASS" "$VCOM"` in addition to the peer `RC -eq 0`.

- [ ] **Step 4: Run — verify FAIL then PASS.**
```sh
cd ~/Development/rt1170/evkb/enet_test && cmake --build build 2>&1 | tail -1
sh run_qemu_enet.sh ping
```
Expected once complete: peer prints `ARP-REPLY ok=True` + `ICMP-REPLY ok=True type=0`; VCOM shows `ENET_ARP=PASS`, `ENET_PING=PASS`; runner `PASS`. (Fail-first: before Step 1's responder is added, the peer times out on `recv_frame` → `FAIL`.)

- [ ] **Step 5: Full-suite sanity — all three phases green.**
```sh
cd ~/Development/rt1170/evkb/enet_test && for p in mac ping; do echo "== $p =="; sh run_qemu_enet.sh $p | tail -3; done
```
Expected: both phases end in `PASS`.

- [ ] **Step 6: Commit (gate repo).**
```sh
git -C ~/Development/rt1170/evkb add enet_test/enet_test.cpp enet_test/enet_peer.py enet_test/run_qemu_enet.sh
git -C ~/Development/rt1170/evkb commit -m "enet_test: Gate 3 hand-rolled ARP + ICMP-echo responder (answers a ping)"
```

---

## Task 6: Hardware verification (the final arbiter)

QEMU proved the plumbing; silicon proves the PHY, RMII clocking, and the real ping. No QEMU gate here — a bench checklist recorded in `HW-RESULTS.md`.

**Files:**
- Create: `evkb/enet_test/HW-RESULTS.md`

- [ ] **Step 1: Pre-flight the board hazards.** Confirm the 10/100 RJ45 connector designator on THIS board (spec flags "J43 area") and the board revision. **Remove any SD card** before the first ENET test (the `AD_32`=ENET_MDC ↔ `SD1_CD_B` REVC conflict); if REVC, note whether `R1926/R136` are populated. Record findings.

- [ ] **Step 2: Flash.**
```sh
pkill -9 -f LinkServer; pkill -9 -f redlinkserv
/Applications/LinkServer_26.6.137/LinkServer run MIMXRT1176:MIMXRT1170-EVKB ~/Development/rt1170/evkb/enet_test/build/enet_test.elf
```
Start the pyserial reader on `/dev/cu.usbmodem5DQ2DDHVWO5EI3` @115200 BEFORE `run` (per the flashing/serial notes).

- [ ] **Step 3: Confirm link + real PHY.** Connect RJ45 to a host/switch. On VCOM expect `ENET_LINK=PASS` and — the key HW-vs-QEMU tell — `ENET_PHYID=1C:C816` (real Realtek RTL8201, **not** QEMU's `7:C0D1`). Watch the board's link LED go solid. If link fails where QEMU passed, check (in order) the `MSCR` MDC divider (MDC ≤ 2.5 MHz), `GPR4` ref-clk-out, and the RTL8201 reset timing.

- [ ] **Step 4: Ping from the host.** Put the host on the `192.168.100.0/24` subnet (e.g. `192.168.100.1`). Then:
```sh
ping -c 4 192.168.100.50
sudo tcpdump -i <iface> -e -n 'arp or icmp'   # in parallel
arp -n 192.168.100.50
```
Expected: ping replies; `tcpdump` shows the host's ARP-request → the board's ARP-reply (`02:00:00:00:00:01`) → echo-request → echo-reply; `arp -n` shows the board's MAC. **This is v1 success.** (Weaker pass if ICMP misbehaves but the ARP reply is on the wire.)

- [ ] **Step 5: Record + commit results.** Write `evkb/enet_test/HW-RESULTS.md` (board rev, connector, PHYID read, `ping` output, `tcpdump` snippet, any QEMU-vs-silicon deltas found). Then:
```sh
git -C ~/Development/rt1170/evkb add enet_test/HW-RESULTS.md
git -C ~/Development/rt1170/evkb commit -m "enet_test: hardware verification results (ping answered on silicon)"
```

- [ ] **Step 6: Update memory.** Write a new memory note `rt1176-ethernet-enet` capturing the hard-won specifics (the phy-num 2→3 QEMU fix, the DBSWP requirement, the SD↔MDC conflict outcome, any silicon-only bug QEMU masked, real RTL8201 PHYID), linked from `MEMORY.md`, following the sibling ENET-adjacent notes' style.

---

## Self-Review

**Spec coverage:** (1) register plumbing → Task 1; (2) `enet.c` MAC/pins/clock/reset/rings/raw TX-RX → Task 3; MDIO + clause-22 link → Task 4; (3) hand-rolled ARP/ICMP responder → Task 5; (4) QEMU `phy-num` 2→3 → Task 4; (5) one `enet_test/` gate accumulating TX/RX→link→ping tokens → Tasks 2–5; HW verification → Task 6. Deferred items (lwIP, sockets, DHCP, IRQ-driven, 1G/QOS) are absent by design. All spec sections map to a task. ✓

**Placeholder scan:** The `enet.c` clock/pins/RCR/TCR/MSCR bodies are intentionally shown as skeletons with exact register targets + SDK/FNET source pointers, because the project method forbids guessing register values — the executing subagent reads `PERI_ENET.h`/`fnet_fec.c`/the SDK and the gate objectively judges correctness. That is a deliberate port-from-source instruction, not a vague "implement later": every value needed (pin table, clock root 51, GPR bits, PHY reset timing, ANAR `0x01E1`, MMFR/BD bit macros) is provided, and the fail-first gate forces completion. The gate sketch, runner, and Python peer — the actual test artifacts — are complete and runnable. ✓

**Type consistency:** `enet_init`/`enet_send_frame`/`enet_read_frame`/`enet_mdio_read`/`enet_mdio_write`/`enet_phy_link_up` signatures match between `enet.h` (Task 3 Step 1) and every call site (sketch Tasks 3–5, `enet.c` Task 4). Shared constants (`ENET_MAC` `02:..:01`, `ENET_IP` `192.168.100.50`, peer `..:02`/`192.168.100.1`, ethertype `0x88B5`) are identical in the sketch and the peer. Token strings (`ENET_BOOT`, `ENET_INIT_DONE`, `ENET_TX=PASS`, `ENET_RX=PASS`, `ENET_PHYID=`, `ENET_LINK=PASS`, `ENET_PHYID_OK=PASS`, `ENET_ARP=PASS`, `ENET_PING=PASS`) match between the sketch prints and the runner greps. ✓
