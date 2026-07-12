# RT1176 Ethernet milestone 3 — Arduino `Ethernet` socket API — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Provide the familiar Arduino `Ethernet` socket API (`EthernetClient`, `EthernetServer`, `EthernetUDP`, `EthernetClass`) on top of the HW-verified lwIP-raw NO_SYS stack, so stock Arduino networking sketches compile and run on the MIMXRT1176-EVKB.

**Architecture:** A new library `~/Development/Ethernet` exposes the standard Arduino `Ethernet` public API over an internal *socket-glue* layer (`src/utility/socket_lwip.*`) that wraps lwIP raw PCBs. The stack stays async underneath (lwIP `tcp_*`/`udp_*`/`dns_*` callbacks); a thin synchronous facade sits at the Arduino edge. Single-threaded NO_SYS + a cooperative, reentrancy-guarded pump (`ethernetif_poll` + `sys_check_timeouts`) means callbacks fire only on one call path — no locks. DNS is enabled in the vendored lwIP (`LWIP_DNS=1`).

**Tech Stack:** C++ Arduino library; lwIP 2.2.1 raw API (`NO_SYS=1`, `LWIP_SOCKET=0`); the frozen `cores/imxrt1176/enet.c` L2 driver; teensy-cmake-macros + QEMU `imx.enet` (SLIRP) gate harness; pyserial VCOM on hardware.

**Reference (read, do not modify):** design spec `evkb/docs/superpowers/specs/2026-07-12-rt1176-ethernet-arduino-api-milestone3-design.md`; the milestone-2 gate `evkb/lwip_test/` (harness to clone); the abstract base classes in `evkb/cores/teensy4/{Client.h,Server.h,Udp.h,IPAddress.h,IPAddress.cpp}`.

---

## Repository boundaries & invariants (apply to EVERY task)

- **`cores/imxrt1176/enet.c` and the rest of `cores/imxrt1176/` are FROZEN** — do not modify the core. (The base classes are *vendored into the new library*, not added to the core.)
- **`~/Development/lwip`** gains **only** `LWIP_DNS` (Task 1). Commit there with `git -C ~/Development/lwip`.
- **`~/Development/Ethernet`** is a **NEW local repo** — `git init` in Task 2; **`git add -A` is acceptable there only**.
- **Gate files** (`evkb/ethernet_test/…`, this plan, HW results) commit to **`git -C ~/Development/rt1170/evkb`** — **NEVER `git add -A` in evkb** (shared tree); stage only the exact touched paths.
- Build every gate with the toolchain file; a **new-library import requires `rm -rf build`** first (the `import_arduino_library` flat-GLOB is cached by CMake).
- Gates run via **`./run_qemu_ethernet.sh <phase>`** (the leading `./`, not `sh` — `gate_init` re-execs `$0` under gtimeout).
- Commit to **`master`** in each repo. **Push only when the user asks** — no `git push` in any task.

**Canonical build command** (used verbatim in every task; `$T` = the gate dir):
```bash
T=~/Development/rt1170/evkb/ethernet_test
cd "$T" && cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake >/tmp/eth_cfg.log 2>&1 \
  && cmake --build build -j8 >/tmp/eth_build.log 2>&1 && echo BUILD_OK || { tail -40 /tmp/eth_build.log; echo BUILD_FAIL; }
```
After adding/removing any imported library or source file: `rm -rf "$T/build"` before the command above.

---

## File structure (created across the plan)

```
~/Development/lwip/port/lwipopts.h                  MODIFY: LWIP_DNS=1, MEMP_NUM_UDP_PCB 4->5   (Task 1)

~/Development/Ethernet/                              NEW library repo                              (Task 2+)
  library.properties                                fork-pattern metadata
  keywords.txt
  src/IPAddress.h  src/IPAddress.cpp                vendored verbatim from cores/teensy4
  src/Client.h  src/Server.h  src/Udp.h             vendored verbatim from cores/teensy4
  src/Ethernet.h  src/Ethernet.cpp                  EthernetClass: netif owner, begin/config     (Task 2)
  src/EthernetClient.h  src/EthernetClient.cpp      EthernetClient : Client                       (Task 3,5,6)
  src/EthernetServer.h  src/EthernetServer.cpp      EthernetServer : Server                       (Task 3)
  src/EthernetUdp.h  src/EthernetUdp.cpp            EthernetUDP : UDP                             (Task 4,6)
  src/utility/socket_lwip.h  src/utility/socket_lwip.cpp   the conn pool + pump + raw callbacks   (Task 2,3)

~/Development/rt1170/evkb/ethernet_test/            NEW gate (cloned from lwip_test)              (Task 2+)
  CMakeLists.txt
  toolchain/  (copied)
  ethernet_test.cpp                                 the single phase-agnostic sketch
  ethernet_peer.py                                  host peer (server/udp phases)
  guestfwd_echo.py                                  SLIRP -cmd echo for the client phase
  run_qemu_ethernet.sh
  HW-RESULTS.md                                     (Task 7)
```

---

## Task 1: DNS-enable the vendored lwIP (+ re-verify all 5 m2 gates)

**Files:**
- Modify: `~/Development/lwip/port/lwipopts.h:16` and `:26`

De-risk the one library change before building on it. `dns.c`/`dns.h` are **already** in the vendored copy (verified) — this task is only the option flip plus a full regression run of milestone 2.

- [ ] **Step 1: Confirm the m2 gates are green BEFORE the change (baseline)**

```bash
cd ~/Development/rt1170/evkb/lwip_test && rm -rf build
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake >/tmp/m2cfg.log 2>&1
cmake --build build -j8 >/tmp/m2build.log 2>&1 && echo BUILD_OK || { tail -30 /tmp/m2build.log; exit 1; }
for p in boot ping dhcp udp tcp; do ./run_qemu_lwip.sh $p 2>&1 | tail -1; done
```
Expected: five `PASS: lwip_test <phase>` lines. (If any fail pre-change, STOP — environment issue, not this task.)

- [ ] **Step 2: Flip `LWIP_DNS` on and give DNS its UDP pcb**

In `~/Development/lwip/port/lwipopts.h` change line 16:
```c
#define LWIP_DNS                     1
```
and line 26 (DNS opens one additional UDP pcb):
```c
#define MEMP_NUM_UDP_PCB             5
```

- [ ] **Step 3: Rebuild the m2 gate and re-run ALL FIVE phases (regression proof)**

```bash
cd ~/Development/rt1170/evkb/lwip_test && rm -rf build
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake >/tmp/m2cfg.log 2>&1
cmake --build build -j8 >/tmp/m2build.log 2>&1 && echo BUILD_OK || { tail -30 /tmp/m2build.log; exit 1; }
for p in boot ping dhcp udp tcp; do ./run_qemu_lwip.sh $p 2>&1 | tail -1; done
```
Expected: `BUILD_OK` then five `PASS: lwip_test <phase>` lines — DNS-on must not regress any m2 phase. (`dns.c` now compiles into the `lwip` library via the existing `src/core` glob.)

- [ ] **Step 4: Commit to the lwip repo**

```bash
git -C ~/Development/lwip add port/lwipopts.h
git -C ~/Development/lwip commit -m "feat(dns): enable LWIP_DNS for Ethernet milestone 3

LWIP_DNS=1 + MEMP_NUM_UDP_PCB 4->5 (DNS uses one UDP pcb). dns.c/dns.h
already vendored. All five m2 gates (boot/ping/dhcp/udp/tcp) re-verified
green after the flip.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 2: Scaffold the `Ethernet` library + `EthernetClass` + the gate (boot phase = compile+link)

**HIGHEST INTEGRATION RISK — the acceptance is that the vendored lwIP + the new library + the core COMPILE and LINK.** Give this task the closest verification.

**Files:**
- Create: `~/Development/Ethernet/library.properties`, `keywords.txt`
- Create (vendored): `~/Development/Ethernet/src/{IPAddress.h,IPAddress.cpp,Client.h,Server.h,Udp.h}`
- Create: `~/Development/Ethernet/src/utility/socket_lwip.h`, `src/utility/socket_lwip.cpp`
- Create: `~/Development/Ethernet/src/Ethernet.h`, `src/Ethernet.cpp`
- Create: `~/Development/rt1170/evkb/ethernet_test/{CMakeLists.txt,ethernet_test.cpp,run_qemu_ethernet.sh,ethernet_peer.py}` + copied `toolchain/`

- [ ] **Step 1: Create the library skeleton + vendor the base classes**

```bash
mkdir -p ~/Development/Ethernet/src/utility ~/Development/Ethernet/examples
cd ~/Development/Ethernet && git init -q
# Vendor the Arduino networking base classes from the sibling teensy4 core (the
# imxrt1176 core lacks them). Copy verbatim - do not edit.
CT=~/Development/rt1170/evkb/cores/teensy4
cp "$CT/IPAddress.h" "$CT/IPAddress.cpp" "$CT/Client.h" "$CT/Server.h" "$CT/Udp.h" src/
```

`library.properties`:
```
name=Ethernet
version=1.0
author=Arduino, newdigate
maintainer=newdigate
sentence=Arduino Ethernet socket API (EthernetClient/Server/UDP) over lwIP for the MIMXRT1176.
paragraph=Wraps the lwIP 2.2.1 raw API in the familiar Arduino Ethernet classes so stock networking sketches run on the NXP i.MX RT1176.
category=Communication
url=http://www.arduino.cc/en/Reference/Ethernet
architectures=*
```

`keywords.txt`:
```
#######################################
# Syntax Coloring Map Ethernet
#######################################
Ethernet	KEYWORD1
EthernetClient	KEYWORD1
EthernetServer	KEYWORD1
EthernetUDP	KEYWORD1
#######################################
begin	KEYWORD2
localIP	KEYWORD2
connect	KEYWORD2
available	KEYWORD2
read	KEYWORD2
write	KEYWORD2
stop	KEYWORD2
beginPacket	KEYWORD2
endPacket	KEYWORD2
parsePacket	KEYWORD2
remoteIP	KEYWORD2
remotePort	KEYWORD2
```

- [ ] **Step 2: Write the socket-glue header `src/utility/socket_lwip.h`**

```c
#ifndef ETHERNET_SOCKET_LWIP_H
#define ETHERNET_SOCKET_LWIP_H
#include <stdint.h>
#include "lwip/tcp.h"
#include "lwip/netif.h"

#define ETH_MAX_SOCK_NUM 4          /* within lwipopts MEMP_NUM_TCP_PCB=5 */

typedef enum { CONN_FREE=0, CONN_CONNECTING, CONN_ESTABLISHED, CONN_CLOSING, CONN_CLOSED } conn_state_t;

typedef struct {
    struct tcp_pcb *pcb;            /* NULL once lwIP frees it (err_cb) */
    struct pbuf    *rx_head;        /* received-but-unread pbuf chain (we hold it) */
    uint16_t        rx_off;         /* read offset into rx_head */
    volatile conn_state_t state;
    ip_addr_t       remote_ip;
    uint16_t        remote_port;
    uint16_t        accept_port;    /* listening port a server conn arrived on; 0 for outbound */
} tcp_conn_t;

#ifdef __cplusplus
extern "C" {
#endif
extern tcp_conn_t eth_conns[ETH_MAX_SOCK_NUM];
extern uint8_t    g_mac[6];         /* defined in the vendored lwip ethernetif.c */

struct netif *eth_netif(void);      /* the single shared netif (owned by EthernetClass) */
void  eth_pump(void);               /* reentrancy-guarded: ethernetif_poll + sys_check_timeouts */

int   eth_conn_alloc(void);         /* -> index in [0,ETH_MAX_SOCK_NUM) or -1 */
void  eth_conn_free(int i);         /* close pcb if any, free rx pbufs, mark FREE */
void  eth_conn_bind_callbacks(int i);   /* wire recv/sent/err/poll on eth_conns[i].pcb */
int   eth_conn_available(int i);        /* unread bytes in the rx chain */
int   eth_conn_read(int i, uint8_t *buf, int len);  /* drain + tcp_recved; -1 if none */
int   eth_conn_peek(int i);             /* first unread byte or -1 */

/* raw callbacks (exposed so EthernetServer's accept can share them) */
err_t eth_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err);
err_t eth_sent_cb(void *arg, struct tcp_pcb *pcb, uint16_t len);
void  eth_err_cb(void *arg, err_t err);
#ifdef __cplusplus
}
#endif
#endif
```

- [ ] **Step 3: Write `src/utility/socket_lwip.cpp` (pool, pump, callbacks, drain)**

```cpp
#include "socket_lwip.h"
#include "lwip/priv/tcp_priv.h"
#include "ethernetif.h"     /* ethernetif_poll */
#include "lwip/timeouts.h"

extern "C" { tcp_conn_t eth_conns[ETH_MAX_SOCK_NUM]; }

/* the single shared netif lives here; EthernetClass initializes it via eth_netif(). */
static struct netif s_netif;
struct netif *eth_netif(void) { return &s_netif; }

void eth_pump(void) {
    static bool in_pump = false;         /* reentrancy guard: write/connect spins call
                                            yield()->eth_pump(); never nest into lwIP. */
    if (in_pump) return;
    in_pump = true;
    ethernetif_poll(&s_netif);
    sys_check_timeouts();
    in_pump = false;
}

static int conn_index(struct tcp_pcb *pcb) {
    for (int i = 0; i < ETH_MAX_SOCK_NUM; i++)
        if (eth_conns[i].state != CONN_FREE && eth_conns[i].pcb == pcb) return i;
    return -1;
}

int eth_conn_alloc(void) {
    for (int i = 0; i < ETH_MAX_SOCK_NUM; i++)
        if (eth_conns[i].state == CONN_FREE) {
            eth_conns[i] = (tcp_conn_t){0};
            eth_conns[i].state = CONN_CONNECTING;   /* claimed; caller sets real state */
            return i;
        }
    return -1;
}

void eth_conn_free(int i) {
    if (i < 0) return;
    tcp_conn_t *c = &eth_conns[i];
    if (c->rx_head) { pbuf_free(c->rx_head); c->rx_head = nullptr; }
    if (c->pcb) {
        tcp_arg(c->pcb, nullptr);
        tcp_recv(c->pcb, nullptr); tcp_sent(c->pcb, nullptr); tcp_err(c->pcb, nullptr);
        if (tcp_close(c->pcb) != ERR_OK) tcp_abort(c->pcb);
        c->pcb = nullptr;
    }
    c->state = CONN_FREE;
}

err_t eth_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    int i = (int)(intptr_t)arg;
    tcp_conn_t *c = &eth_conns[i];
    if (p == nullptr) { c->state = CONN_CLOSING; return ERR_OK; }   /* peer FIN; keep rx_head */
    if (err != ERR_OK) { if (p) pbuf_free(p); return err; }
    if (c->rx_head) pbuf_cat(c->rx_head, p); else c->rx_head = p;   /* hold; recved on drain */
    return ERR_OK;
}

err_t eth_sent_cb(void *arg, struct tcp_pcb *pcb, uint16_t len) { (void)arg;(void)pcb;(void)len; return ERR_OK; }

void eth_err_cb(void *arg, err_t err) {
    int i = (int)(intptr_t)arg;                 /* lwIP has already freed the pcb */
    (void)err;
    if (i >= 0 && i < ETH_MAX_SOCK_NUM) { eth_conns[i].pcb = nullptr; eth_conns[i].state = CONN_CLOSED; }
}

void eth_conn_bind_callbacks(int i) {
    struct tcp_pcb *pcb = eth_conns[i].pcb;
    tcp_arg(pcb, (void *)(intptr_t)i);
    tcp_recv(pcb, eth_recv_cb);
    tcp_sent(pcb, eth_sent_cb);
    tcp_err(pcb, eth_err_cb);
}

int eth_conn_available(int i) {
    tcp_conn_t *c = &eth_conns[i];
    if (!c->rx_head) return 0;
    return (int)(c->rx_head->tot_len - c->rx_off);
}

int eth_conn_peek(int i) {
    tcp_conn_t *c = &eth_conns[i];
    if (!c->rx_head || c->rx_off >= c->rx_head->tot_len) return -1;
    return pbuf_get_at(c->rx_head, c->rx_off);
}

int eth_conn_read(int i, uint8_t *buf, int len) {
    tcp_conn_t *c = &eth_conns[i];
    if (!c->rx_head) return -1;
    int got = 0;
    while (got < len && c->rx_head) {
        uint16_t avail = c->rx_head->tot_len - c->rx_off;
        if (avail == 0) break;
        uint16_t n = (uint16_t)((len - got) < avail ? (len - got) : avail);
        pbuf_copy_partial(c->rx_head, buf + got, n, c->rx_off);
        got += n; c->rx_off += n;
        /* free whole pbufs off the head as they are consumed, crediting the window */
        while (c->rx_head && c->rx_off >= c->rx_head->len) {
            c->rx_off -= c->rx_head->len;
            uint16_t seg = c->rx_head->len;
            struct pbuf *next = c->rx_head->next;
            if (next) pbuf_ref(next);
            pbuf_free(c->rx_head);
            c->rx_head = next;
            if (c->pcb) tcp_recved(c->pcb, seg);
        }
    }
    return got ? got : -1;
}
```
*(Note: `pbuf_get_at`, `pbuf_copy_partial`, `pbuf_ref`, `pbuf_cat` are lwIP pbuf.h helpers; `tcp_recved` credits the receive window as data is consumed.)*

- [ ] **Step 4: Write `src/Ethernet.h` (EthernetClass) + `src/Ethernet.cpp`**

`src/Ethernet.h`:
```cpp
#ifndef ethernet_h_
#define ethernet_h_
#include <Arduino.h>
#include "IPAddress.h"
#include "Client.h"
#include "Server.h"
#include "Udp.h"

enum EthernetLinkStatus { Unknown, LinkON, LinkOFF };
enum EthernetHardwareStatus { EthernetNoHardware, EthernetW5100, EthernetW5200, EthernetW5500, EthernetOther };

class EthernetClass {
public:
    int  begin(uint8_t *mac, unsigned long timeout = 60000, unsigned long responseTimeout = 4000);
    void begin(uint8_t *mac, IPAddress ip);
    void begin(uint8_t *mac, IPAddress ip, IPAddress dns);
    void begin(uint8_t *mac, IPAddress ip, IPAddress dns, IPAddress gateway);
    void begin(uint8_t *mac, IPAddress ip, IPAddress dns, IPAddress gateway, IPAddress subnet);
    int  maintain();
    EthernetLinkStatus linkStatus();
    EthernetHardwareStatus hardwareStatus();
    IPAddress localIP();
    IPAddress subnetMask();
    IPAddress gatewayIP();
    IPAddress dnsServerIP();
    void setDnsServerIP(const IPAddress dns);
    void MACAddress(uint8_t *mac_address);
    void setMACAddress(const uint8_t *mac_address);
    void loop();                       /* pump wrapper for sketches (optional) */
private:
    void netif_bringup(uint8_t *mac, const ip4_addr_t *ip, const ip4_addr_t *nm, const ip4_addr_t *gw);
    bool _inited = false;
};
extern EthernetClass Ethernet;
#include "EthernetClient.h"
#include "EthernetServer.h"
#include "EthernetUdp.h"
#endif
```

`src/Ethernet.cpp`:
```cpp
#include "Ethernet.h"
#include "utility/socket_lwip.h"
#include "lwip/init.h"
#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "lwip/netif.h"
#include "netif/ethernet.h"
#include "ethernetif.h"

extern "C" { uint8_t g_mac[6] = {0x02,0x00,0x00,0x00,0x00,0x01}; }   /* ethernetif.c extern-refs this */
tcp_conn_t eth_conns[ETH_MAX_SOCK_NUM];
extern "C" int enet_phy_link_up(uint32_t timeout_ms);   /* frozen core enet.c:487; 0 = poll once */
EthernetClass Ethernet;

static IPAddress from_ip4(const ip4_addr_t *a) {
    uint32_t v = a ? a->addr : 0;  /* network byte order */
    return IPAddress((v)&0xff,(v>>8)&0xff,(v>>16)&0xff,(v>>24)&0xff);
}
static void to_ip4(IPAddress in, ip4_addr_t *out) { IP4_ADDR(out, in[0], in[1], in[2], in[3]); }

void EthernetClass::netif_bringup(uint8_t *mac, const ip4_addr_t *ip, const ip4_addr_t *nm, const ip4_addr_t *gw) {
    for (int i=0;i<6;i++) g_mac[i]=mac[i];
    if (!_inited) { lwip_init(); _inited = true; }
    struct netif *n = eth_netif();
    netif_add(n, ip, nm, gw, NULL, ethernetif_init, ethernet_input);
    netif_set_default(n);
    netif_set_up(n);
}

int EthernetClass::begin(uint8_t *mac, unsigned long timeout, unsigned long responseTimeout) {
    (void)responseTimeout;
    ip4_addr_t any; ip4_addr_set_zero(&any);
    netif_bringup(mac, &any, &any, &any);
    dhcp_start(eth_netif());
    uint32_t t0 = millis();
    while (!dhcp_supplied_address(eth_netif())) {
        eth_pump();
        if (millis() - t0 > timeout) return 0;
    }
    return 1;
}

void EthernetClass::begin(uint8_t *mac, IPAddress ip) {
    begin(mac, ip, IPAddress(ip[0],ip[1],ip[2],1));                    /* dns = gw default */
}
void EthernetClass::begin(uint8_t *mac, IPAddress ip, IPAddress dns) {
    begin(mac, ip, dns, IPAddress(ip[0],ip[1],ip[2],1));
}
void EthernetClass::begin(uint8_t *mac, IPAddress ip, IPAddress dns, IPAddress gateway) {
    begin(mac, ip, dns, gateway, IPAddress(255,255,255,0));
}
void EthernetClass::begin(uint8_t *mac, IPAddress ip, IPAddress dns, IPAddress gateway, IPAddress subnet) {
    ip4_addr_t a,nm,gw; to_ip4(ip,&a); to_ip4(subnet,&nm); to_ip4(gateway,&gw);
    netif_bringup(mac, &a, &nm, &gw);
    setDnsServerIP(dns);
}

int  EthernetClass::maintain() { eth_pump(); return 0; }    /* DHCP renew is automatic via timeouts */
EthernetLinkStatus EthernetClass::linkStatus() { return enet_phy_link_up(0) ? LinkON : LinkOFF; }
EthernetHardwareStatus EthernetClass::hardwareStatus() { return EthernetOther; }
IPAddress EthernetClass::localIP()    { return from_ip4(netif_ip4_addr(eth_netif())); }
IPAddress EthernetClass::subnetMask() { return from_ip4(netif_ip4_netmask(eth_netif())); }
IPAddress EthernetClass::gatewayIP()  { return from_ip4(netif_ip4_gw(eth_netif())); }
IPAddress EthernetClass::dnsServerIP(){ const ip_addr_t *d = dns_getserver(0); return from_ip4(ip_2_ip4(d)); }
void EthernetClass::setDnsServerIP(const IPAddress dns) { ip_addr_t d; IP_ADDR4(&d,dns[0],dns[1],dns[2],dns[3]); dns_setserver(0,&d); }
void EthernetClass::MACAddress(uint8_t *m) { for (int i=0;i<6;i++) m[i]=g_mac[i]; }
void EthernetClass::setMACAddress(const uint8_t *m) { for (int i=0;i<6;i++) g_mac[i]=m[i]; }
void EthernetClass::loop() { eth_pump(); }
```

- [ ] **Step 5: Stub the three class headers so the library links (real bodies land in Tasks 3–4)**

Create minimal compiling stubs now (they satisfy `#include "EthernetClient.h"` etc. from `Ethernet.h`; Task 3/4 replace the bodies). `src/EthernetClient.h`:
```cpp
#ifndef ethernetclient_h_
#define ethernetclient_h_
#include "Client.h"
class EthernetClient : public Client {
public:
    EthernetClient() : _sock(-1) {}
    explicit EthernetClient(int s) : _sock(s) {}
    int connect(IPAddress ip, uint16_t port) override;
    int connect(const char *host, uint16_t port) override;
    size_t write(uint8_t b) override;
    size_t write(const uint8_t *buf, size_t size) override;
    int available() override;
    int read() override;
    int read(uint8_t *buf, size_t size) override;
    int peek() override;
    void flush() override;
    void stop() override;
    uint8_t connected() override;
    operator bool() override;
    using Print::write;
    int _sock;
};
#endif
```
`src/EthernetServer.h`:
```cpp
#ifndef ethernetserver_h_
#define ethernetserver_h_
#include "Server.h"
class EthernetClient;
class EthernetServer : public Server {
public:
    explicit EthernetServer(uint16_t port) : _port(port), _listen(nullptr) {}
    void begin() override;
    EthernetClient available();
    EthernetClient accept();
    size_t write(uint8_t b) override;
    size_t write(const uint8_t *buf, size_t size) override;
    using Print::write;
    uint16_t _port;
    void *_listen;               /* struct tcp_pcb* (opaque here) */
};
#endif
```
`src/EthernetUdp.h`:
```cpp
#ifndef ethernetudp_h_
#define ethernetudp_h_
#include "Udp.h"
class EthernetUDP : public UDP {
public:
    EthernetUDP();
    uint8_t begin(uint16_t port) override;
    void stop() override;
    int beginPacket(IPAddress ip, uint16_t port) override;
    int beginPacket(const char *host, uint16_t port) override;
    int endPacket() override;
    size_t write(uint8_t b) override;
    size_t write(const uint8_t *buf, size_t size) override;
    int parsePacket() override;
    int available() override;
    int read() override;
    int read(unsigned char *buf, size_t len) override;
    int read(char *buf, size_t len) override;
    int peek() override;
    void flush() override;
    IPAddress remoteIP() override;
    uint16_t remotePort() override;
    using Print::write;
private:
    void *_pcb;                  /* struct udp_pcb* */
    struct udp_slot { void *p; ip_addr_t ip; uint16_t port; };
    static const int RXQ = 4;
    udp_slot _rxq[RXQ]; int _rxh, _rxt;
    void *_cur; uint16_t _cur_off;
    ip_addr_t _rip; uint16_t _rport;
    ip_addr_t _dip; uint16_t _dport;
    uint8_t _txbuf[1472]; uint16_t _txlen;
};
#endif
```
Provide temporary stub `.cpp` files that `return 0/-1/false` for every method **except** the ones Task 2 needs — but to keep Task 2 self-contained, implement each `.cpp` as trivial stubs now:

`src/EthernetClient.cpp`, `src/EthernetServer.cpp`, `src/EthernetUdp.cpp` — each method returns a null/zero/false default (e.g. `int EthernetClient::connect(IPAddress,uint16_t){return 0;}` … `EthernetUDP::EthernetUDP():_pcb(nullptr),_rxh(0),_rxt(0),_cur(nullptr),_cur_off(0),_txlen(0){}`). These make the library link for the boot gate; Tasks 3–6 replace the bodies.

- [ ] **Step 6: Create the gate `ethernet_test/` (clone the m2 harness)**

```bash
S=~/Development/rt1170/evkb/lwip_test ; D=~/Development/rt1170/evkb/ethernet_test
mkdir -p "$D" && cp -R "$S/toolchain" "$D/toolchain"
```
`ethernet_test/CMakeLists.txt`:
```cmake
cmake_minimum_required(VERSION 3.24)
project(ethernet_test)
set(TEENSY_VERSION 117 CACHE STRING "")
include(FetchContent)
FetchContent_Declare(teensy_cmake_macros SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../teensy-cmake-macros)
FetchContent_MakeAvailable(teensy_cmake_macros)
include(${teensy_cmake_macros_SOURCE_DIR}/CMakeLists.include.txt)
import_arduino_library(cores ${CMAKE_CURRENT_LIST_DIR}/../cores/imxrt1176)
import_arduino_library(lwip $ENV{HOME}/Development/lwip
    src/include src/core src/core/ipv4 src/netif port port/arch)
import_arduino_library(ethernet $ENV{HOME}/Development/Ethernet src src/utility)
teensy_add_executable(ethernet_test ethernet_test.cpp)
teensy_target_link_libraries(ethernet_test cores lwip ethernet)
target_link_libraries(ethernet_test.elf stdc++)
```

- [ ] **Step 7: Write the phase-agnostic sketch `ethernet_test/ethernet_test.cpp` (boot-capable form)**

The single sketch serves everything and self-drives the guest-initiated phases once DHCP is up. Task 2's boot acceptance only needs `begin()`+netif-up; keep the server/udp/client/dns bodies calling the (stubbed) classes — they compile now and become live in later tasks.
```cpp
#include "Arduino.h"
#include "HardwareSerial.h"
#include "Ethernet.h"

static uint8_t mac[6] = {0x02,0x00,0x00,0x00,0x00,0x01};
EthernetServer server(7);
EthernetUDP    udp;
static bool did_client = false, did_dns = false;

void setup() {
    Serial1.begin(115200); delay(50);
    Serial1.println("ETH_BOOT");
    int ok = Ethernet.begin(mac, 15000);          /* DHCP */
    IPAddress ip = Ethernet.localIP();
    Serial1.print("ETH_DHCP ok="); Serial1.print(ok);
    Serial1.print(" ip="); Serial1.println(ip);
    server.begin();
    udp.begin(7);
    Serial1.println("ETH_NETIF_UP");
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
static void try_client_once() {              /* fires ~immediately after DHCP, before the 6s peer */
    did_client = true;
    EthernetClient c;
    if (c.connect(IPAddress(10,0,2,100), 7)) {
        const char *tok = "ETHCLI-PROBE\n"; c.write((const uint8_t*)tok, 13);
        uint32_t t0 = millis(); char in[16]; int got = 0;
        while (got < 13 && millis()-t0 < 3000) { Ethernet.loop(); while (c.available() && got<13) in[got++]=c.read(); }
        Serial1.print("CLIENT_ECHO="); Serial1.println((got==13 && memcmp(in,tok,13)==0) ? "PASS":"FAIL");
        c.stop();
    } else Serial1.println("CLIENT_ECHO=FAIL");
}
static void try_dns_once() {
    did_dns = true;
    IPAddress r;
    /* connect() with a hostname exercises DNS; here we resolve via EthernetUDP::beginPacket(host) path.
       Task 6 wires real resolution; until then this prints DNS_FAIL harmlessly. */
    EthernetClient c;
    int ok = c.connect("example.com", 9);      /* discard port; DNS is the point */
    r = Ethernet.localIP();                     /* placeholder until Task 6 exposes resolved ip */
    Serial1.print("DNS_TRY ok="); Serial1.println(ok);
    if (ok) c.stop();
}

void loop() {
    Ethernet.loop();
    serve_tcp();
    serve_udp();
    if (Ethernet.localIP() != IPAddress(0,0,0,0)) {
        if (!did_client) try_client_once();
        if (!did_dns) try_dns_once();
    }
}
```
*(In Task 6 the `DNS_TRY`/`try_dns_once` is upgraded to print `DNS_OK ip=<addr>`.)*

- [ ] **Step 8: Write `run_qemu_ethernet.sh`, `ethernet_peer.py`, `guestfwd_echo.py`**

`ethernet_test/run_qemu_ethernet.sh`:
```sh
#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
export PHASE="${1:-${PHASE:-boot}}"
gate_init
ELF="$DIR/build/ethernet_test.elf"; VCOM="$DIR/vcom.uart"; DBG="$DIR/eth.dbg"; RES="$DIR/eth.result"
gate_tmp "$RES"; PORT=15600
rm -f "$VCOM" "$DBG" "$RES"
case "$PHASE" in
  server) NIC="-nic user,model=imx.enet,hostfwd=tcp::5555-:7" ;;
  udp)    NIC="-nic user,model=imx.enet,hostfwd=udp::5556-:7" ;;
  client) NIC="-nic user,model=imx.enet,guestfwd=tcp:10.0.2.100:7-cmd:python3 $DIR/guestfwd_echo.py" ;;
  dns)    NIC="-nic user,model=imx.enet" ;;
  *)      NIC="-nic user,model=imx.enet" ;;
esac
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$VCOM" $NIC -d guest_errors -D "$DBG" &
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
echo "PASS: ethernet_test $PHASE"
```

`ethernet_test/ethernet_peer.py` (server = the m2 tcp probe; udp = the m2 udp probe):
```python
#!/usr/bin/env python3
import socket, sys, time
def recvall(s, n):
    b=b""
    while len(b)<n:
        c=s.recv(n-len(b))
        if not c: raise EOFError
        b+=c
    return b
phase, host = sys.argv[1], sys.argv[2]
if phase == "server":
    hport=5555; time.sleep(6); s=None
    for a in range(5):
        try: s=socket.create_connection((host,hport),timeout=5); break
        except OSError: print("connect retry",a); time.sleep(1)
    if s is None: print("FAIL: no connect"); sys.exit(1)
    s.settimeout(6); msg=b"ETH-TCP-ECHO-PROBE"; s.sendall(msg)
    try: got=recvall(s,len(msg))
    except (EOFError,socket.timeout): print("FAIL: no echo"); sys.exit(1)
    print("TCP got=%r"%got); s.close(); sys.exit(0 if got==msg else 1)
if phase == "udp":
    hport=5556; time.sleep(6)
    s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); s.settimeout(5); msg=b"ETH-UDP-ECHO-PROBE"
    for a in range(4):
        s.sendto(msg,(host,hport))
        try:
            got,_=s.recvfrom(1024); print("UDP got=%r"%got); sys.exit(0 if got==msg else 1)
        except socket.timeout: print("udp retry",a)
    print("FAIL: no udp echo"); sys.exit(1)
print("skeleton phase",phase); sys.exit(0)
```

`ethernet_test/guestfwd_echo.py` (unbuffered byte echo for the client phase's SLIRP `-cmd`):
```python
#!/usr/bin/env python3
import os
while True:
    b = os.read(0, 4096)
    if not b: break
    os.write(1, b)
```

- [ ] **Step 9: Build and run the boot gate — verify it FAILS then PASSES**

```bash
T=~/Development/rt1170/evkb/ethernet_test; rm -rf "$T/build"
cd "$T" && cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake >/tmp/eth_cfg.log 2>&1 \
  && cmake --build build -j8 >/tmp/eth_build.log 2>&1 && echo BUILD_OK || { tail -50 /tmp/eth_build.log; echo BUILD_FAIL; }
./run_qemu_ethernet.sh boot 2>&1 | tail -20
```
Expected: `BUILD_OK`, then `PASS: ethernet_test boot` (VCOM shows `ETH_BOOT`, `ETH_DHCP ok=1 ip=10.0.2.15`, `ETH_NETIF_UP`). **This green boot gate is the compile+link acceptance** — the vendored lwIP + new Ethernet lib + core link and `Ethernet.begin()` runs. If the build fails, fix includes/link errors before proceeding (do not touch `enet.c`).

- [ ] **Step 10: Commit (both repos)**

```bash
cd ~/Development/Ethernet && git add -A && git commit -q -m "feat(ethernet): library scaffold, EthernetClass, socket-glue pool + pump

Vendored base classes (IPAddress/Client/Server/Udp) from cores/teensy4;
EthernetClass begin(DHCP+static)/config over the lwip netif; the tcp_conn
pool + reentrancy-guarded pump + raw callbacks; class stubs for Client/
Server/UDP (bodies land in later tasks).

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
git -C ~/Development/rt1170/evkb add ethernet_test/CMakeLists.txt ethernet_test/ethernet_test.cpp \
  ethernet_test/run_qemu_ethernet.sh ethernet_test/ethernet_peer.py ethernet_test/guestfwd_echo.py \
  ethernet_test/toolchain
git -C ~/Development/rt1170/evkb commit -q -m "test(ethernet-m3): gate scaffold + boot phase (compile+link acceptance)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 3: EthernetServer + the EthernetClient RX/TX bridge (server echo gate)

Implements the synchronous Client facade (first exercised end-to-end via accepted connections) and the server accept-pool.

**Files:**
- Modify: `~/Development/Ethernet/src/EthernetClient.cpp` (replace stubs with real bodies, except `connect` — Task 5)
- Modify: `~/Development/Ethernet/src/EthernetServer.cpp` (real bodies)
- Modify: `~/Development/Ethernet/src/utility/socket_lwip.cpp` (add `eth_accept` support if needed)

- [ ] **Step 1: Run the server gate to confirm it FAILS (stubs echo nothing)**

```bash
T=~/Development/rt1170/evkb/ethernet_test; cd "$T"
cmake --build build -j8 >/tmp/eth_build.log 2>&1 && ./run_qemu_ethernet.sh server 2>&1 | tail -8
```
Expected: `FAIL: peer rc=1` (the stub `server.available()` returns a false client; no echo).

- [ ] **Step 2: Implement `EthernetClient.cpp` (RX/TX bridge; `connect` stays stubbed until Task 5)**

```cpp
#include "Ethernet.h"
#include "EthernetClient.h"
#include "utility/socket_lwip.h"
#include "lwip/tcp.h"

int EthernetClient::connect(IPAddress ip, uint16_t port) { (void)ip;(void)port; return 0; } /* Task 5 */
int EthernetClient::connect(const char *host, uint16_t port) { (void)host;(void)port; return 0; } /* Task 6 */

size_t EthernetClient::write(uint8_t b) { return write(&b, 1); }

size_t EthernetClient::write(const uint8_t *buf, size_t size) {
    if (_sock < 0) return 0;
    tcp_conn_t *c = &eth_conns[_sock];
    size_t sent = 0; uint32_t t0 = millis();
    while (sent < size) {
        if (!c->pcb || c->state == CONN_CLOSED) break;
        uint16_t space = tcp_sndbuf(c->pcb);
        if (space == 0) {
            eth_pump();
            if (millis() - t0 > 4000) break;
            continue;
        }
        uint16_t n = (uint16_t)((size - sent) < space ? (size - sent) : space);
        if (tcp_write(c->pcb, buf + sent, n, TCP_WRITE_FLAG_COPY) != ERR_OK) { eth_pump(); continue; }
        sent += n; t0 = millis();
    }
    if (c->pcb) tcp_output(c->pcb);
    return sent;
}

int EthernetClient::available() { if (_sock < 0) return 0; eth_pump(); return eth_conn_available(_sock); }
int EthernetClient::read() { uint8_t b; return (read(&b,1) == 1) ? b : -1; }
int EthernetClient::read(uint8_t *buf, size_t size) { if (_sock < 0) return -1; eth_pump(); return eth_conn_read(_sock, buf, (int)size); }
int EthernetClient::peek() { if (_sock < 0) return -1; eth_pump(); return eth_conn_peek(_sock); }
void EthernetClient::flush() { if (_sock >= 0 && eth_conns[_sock].pcb) { tcp_output(eth_conns[_sock].pcb); eth_pump(); } }

void EthernetClient::stop() {
    if (_sock < 0) return;
    eth_conn_free(_sock);
    eth_pump();                     /* let the FIN go out */
    _sock = -1;
}

uint8_t EthernetClient::connected() {
    if (_sock < 0) return 0;
    eth_pump();
    tcp_conn_t *c = &eth_conns[_sock];
    if (c->state == CONN_ESTABLISHED) return 1;
    return eth_conn_available(_sock) > 0 ? 1 : 0;   /* data buffered past a peer FIN */
}

EthernetClient::operator bool() { return _sock >= 0 && eth_conns[_sock].state != CONN_FREE; }
```

- [ ] **Step 3: Implement `EthernetServer.cpp` (listen + accept-pool + available/accept/broadcast)**

```cpp
#include "Ethernet.h"
#include "EthernetServer.h"
#include "EthernetClient.h"
#include "utility/socket_lwip.h"
#include "lwip/tcp.h"

static err_t eth_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err) {
    if (err != ERR_OK || newpcb == nullptr) return ERR_VAL;
    uint16_t lport = (uint16_t)(intptr_t)arg;
    int i = eth_conn_alloc();
    if (i < 0) { tcp_abort(newpcb); return ERR_ABRT; }   /* pool full: refuse */
    tcp_conn_t *c = &eth_conns[i];
    c->pcb = newpcb; c->state = CONN_ESTABLISHED; c->accept_port = lport;
    c->remote_ip = newpcb->remote_ip; c->remote_port = newpcb->remote_port;
    eth_conn_bind_callbacks(i);
    return ERR_OK;
}

void EthernetServer::begin() {
    struct tcp_pcb *pcb = tcp_new();
    if (!pcb) return;
    if (tcp_bind(pcb, IP_ANY_TYPE, _port) != ERR_OK) { tcp_close(pcb); return; }
    struct tcp_pcb *lp = tcp_listen(pcb);               /* frees pcb, returns listen pcb */
    if (!lp) { tcp_close(pcb); return; }
    _listen = lp;
    tcp_arg(lp, (void *)(intptr_t)_port);
    tcp_accept(lp, eth_accept_cb);
}

EthernetClient EthernetServer::available() {
    eth_pump();
    for (int i = 0; i < ETH_MAX_SOCK_NUM; i++) {
        tcp_conn_t *c = &eth_conns[i];
        if (c->state != CONN_FREE && c->accept_port == _port && eth_conn_available(i) > 0)
            return EthernetClient(i);
    }
    return EthernetClient(-1);
}

EthernetClient EthernetServer::accept() {
    eth_pump();
    for (int i = 0; i < ETH_MAX_SOCK_NUM; i++) {
        tcp_conn_t *c = &eth_conns[i];
        if (c->state == CONN_ESTABLISHED && c->accept_port == _port) {
            c->accept_port = 0;            /* hand off once (ownership transfer) */
            return EthernetClient(i);
        }
    }
    return EthernetClient(-1);
}

size_t EthernetServer::write(uint8_t b) { return write(&b, 1); }
size_t EthernetServer::write(const uint8_t *buf, size_t size) {
    size_t n = 0;
    for (int i = 0; i < ETH_MAX_SOCK_NUM; i++)
        if (eth_conns[i].state != CONN_FREE && eth_conns[i].accept_port == _port) {
            EthernetClient c(i); n = c.write(buf, size);
        }
    return n;
}
```

- [ ] **Step 4: Build + run the server gate — verify it PASSES**

```bash
T=~/Development/rt1170/evkb/ethernet_test; cd "$T"
cmake --build build -j8 >/tmp/eth_build.log 2>&1 && echo BUILD_OK || { tail -40 /tmp/eth_build.log; exit 1; }
./run_qemu_ethernet.sh server 2>&1 | tail -6
# regression: boot must still pass
./run_qemu_ethernet.sh boot 2>&1 | tail -1
```
Expected: `PASS: ethernet_test server` (peer prints `TCP got=b'ETH-TCP-ECHO-PROBE'`) and `PASS: ethernet_test boot`.

- [ ] **Step 5: Commit**

```bash
cd ~/Development/Ethernet && git add -A && git commit -q -m "feat(ethernet): EthernetServer accept-pool + EthernetClient RX/TX bridge

Server begin/accept_cb(refuse-when-full)/available/accept/broadcast; client
read/write(sndbuf pump-spin)/available/peek/flush/stop/connected over the
tcp_conn pool. Server-echo gate green.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 4: EthernetUDP (udp echo gate)

**Files:**
- Modify: `~/Development/Ethernet/src/EthernetUdp.cpp` (real bodies)

- [ ] **Step 1: Run the udp gate to confirm it FAILS**

```bash
T=~/Development/rt1170/evkb/ethernet_test; cd "$T"
./run_qemu_ethernet.sh udp 2>&1 | tail -6
```
Expected: `FAIL: peer rc=1` (stub UDP echoes nothing).

- [ ] **Step 2: Implement `EthernetUdp.cpp`**

```cpp
#include "Ethernet.h"
#include "EthernetUdp.h"
#include "utility/socket_lwip.h"
#include "lwip/udp.h"

static void eth_udp_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                            const ip_addr_t *addr, u16_t port) {
    (void)pcb;
    EthernetUDP *self = (EthernetUDP *)arg;
    self->_enqueue(p, addr, port);
}

/* helper on the class (declare in EthernetUdp.h private: void _enqueue(...);) */
void EthernetUDP::_enqueue(void *p, const ip_addr_t *addr, uint16_t port) {
    int nt = (_rxt + 1) % RXQ;
    if (nt == _rxh) { pbuf_free((struct pbuf*)p); return; }   /* ring full: drop */
    _rxq[_rxt].p = p; _rxq[_rxt].ip = *addr; _rxq[_rxt].port = port; _rxt = nt;
}

EthernetUDP::EthernetUDP() : _pcb(nullptr), _rxh(0), _rxt(0), _cur(nullptr), _cur_off(0), _txlen(0) {}

uint8_t EthernetUDP::begin(uint16_t port) {
    struct udp_pcb *pcb = udp_new();
    if (!pcb) return 0;
    if (udp_bind(pcb, IP_ANY_TYPE, port) != ERR_OK) { udp_remove(pcb); return 0; }
    udp_recv(pcb, eth_udp_recv_cb, this);
    _pcb = pcb; return 1;
}

void EthernetUDP::stop() {
    if (_cur) { pbuf_free((struct pbuf*)_cur); _cur = nullptr; }
    while (_rxh != _rxt) { pbuf_free((struct pbuf*)_rxq[_rxh].p); _rxh = (_rxh+1)%RXQ; }
    if (_pcb) { udp_remove((struct udp_pcb*)_pcb); _pcb = nullptr; }
}

int EthernetUDP::parsePacket() {
    eth_pump();
    if (_cur) { pbuf_free((struct pbuf*)_cur); _cur = nullptr; _cur_off = 0; }
    if (_rxh == _rxt) return 0;
    _cur = _rxq[_rxh].p; _rip = _rxq[_rxh].ip; _rport = _rxq[_rxh].port;
    _rxh = (_rxh + 1) % RXQ; _cur_off = 0;
    return (int)((struct pbuf*)_cur)->tot_len;
}

int EthernetUDP::available() { return _cur ? (int)(((struct pbuf*)_cur)->tot_len - _cur_off) : 0; }

int EthernetUDP::read(unsigned char *buf, size_t len) {
    if (!_cur) return -1;
    struct pbuf *p = (struct pbuf*)_cur;
    uint16_t avail = p->tot_len - _cur_off; if (avail == 0) return -1;
    uint16_t n = (uint16_t)(len < avail ? len : avail);
    pbuf_copy_partial(p, buf, n, _cur_off); _cur_off += n;
    if (_cur_off >= p->tot_len) { pbuf_free(p); _cur = nullptr; _cur_off = 0; }
    return n;
}
int EthernetUDP::read(char *buf, size_t len) { return read((unsigned char*)buf, len); }
int EthernetUDP::read() { unsigned char b; return (read(&b,1)==1)? b : -1; }
int EthernetUDP::peek() { if(!_cur) return -1; struct pbuf*p=(struct pbuf*)_cur; if(_cur_off>=p->tot_len) return -1; return pbuf_get_at(p,_cur_off); }
void EthernetUDP::flush() { if (_cur) { pbuf_free((struct pbuf*)_cur); _cur=nullptr; _cur_off=0; } }

int EthernetUDP::beginPacket(IPAddress ip, uint16_t port) {
    IP_ADDR4(&_dip, ip[0],ip[1],ip[2],ip[3]); _dport = port; _txlen = 0; return 1;
}
int EthernetUDP::beginPacket(const char *host, uint16_t port) { (void)host;(void)port; return 0; } /* Task 6 */

size_t EthernetUDP::write(uint8_t b) { return write(&b, 1); }
size_t EthernetUDP::write(const uint8_t *buf, size_t size) {
    uint16_t room = sizeof(_txbuf) - _txlen; uint16_t n = (uint16_t)(size < room ? size : room);
    memcpy(_txbuf + _txlen, buf, n); _txlen += n; return n;
}
int EthernetUDP::endPacket() {
    if (!_pcb) return 0;
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, _txlen, PBUF_RAM);
    if (!p) return 0;
    pbuf_take(p, _txbuf, _txlen);
    err_t e = udp_sendto((struct udp_pcb*)_pcb, p, &_dip, _dport);
    pbuf_free(p); eth_pump();
    return e == ERR_OK ? 1 : 0;
}
IPAddress EthernetUDP::remoteIP() { uint32_t v = ip_2_ip4(&_rip)->addr; return IPAddress(v&0xff,(v>>8)&0xff,(v>>16)&0xff,(v>>24)&0xff); }
uint16_t EthernetUDP::remotePort() { return _rport; }
```
Add to `src/EthernetUdp.h` `private:` the declaration `public: void _enqueue(void *p, const ip_addr_t *addr, uint16_t port);` (public so the C callback can reach it), and `#include <string.h>`.

- [ ] **Step 3: Build + run the udp gate — verify PASS + regression**

```bash
T=~/Development/rt1170/evkb/ethernet_test; cd "$T"
cmake --build build -j8 >/tmp/eth_build.log 2>&1 && echo BUILD_OK || { tail -40 /tmp/eth_build.log; exit 1; }
for p in boot server udp; do ./run_qemu_ethernet.sh $p 2>&1 | tail -1; done
```
Expected: `PASS` for boot, server, udp (peer prints `UDP got=b'ETH-UDP-ECHO-PROBE'`).

- [ ] **Step 4: Commit**

```bash
cd ~/Development/Ethernet && git add -A && git commit -q -m "feat(ethernet): EthernetUDP (rx datagram ring + tx assembly + sendto)

begin/recv_cb-enqueue/parsePacket/available/read/peek/beginPacket(IP)/write/
endPacket/remoteIP/remotePort/stop. UDP-echo gate green.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 5: Outbound EthernetClient::connect(IPAddress) (client gate — the new harness unknown)

Validate the SLIRP `guestfwd` mechanism (outbound TCP from the guest) — give this the same close verification as Task 2.

**Files:**
- Modify: `~/Development/Ethernet/src/EthernetClient.cpp` (`connect(IPAddress,port)` body)

- [ ] **Step 1: De-risk `guestfwd` FIRST — prove the mechanism carries bytes**

Confirm QEMU accepts the `-nic user,...,guestfwd=...-cmd:` form and the `-cmd` echoes, independent of the sketch:
```bash
chmod +x ~/Development/rt1170/evkb/ethernet_test/guestfwd_echo.py
T=~/Development/rt1170/evkb/ethernet_test
grep -q "guestfwd=tcp:10.0.2.100:7-cmd:python3 $T/guestfwd_echo.py" "$T/run_qemu_ethernet.sh" && echo "run-script client case present"
```
If QEMU later rejects `guestfwd` inside `-nic` (Step 4 shows a QEMU parse/`guest_errors` failure), FALLBACK: switch the client case to an explicit netdev, e.g. keep SLIRP but move guestfwd to a `-netdev user,id=n0,model=…`? — since `imx.enet` is the SoC MAC bound by `-nic`, the portable fallback is to have the sketch connect to the SLIRP gateway `10.0.2.2:80` while a host listener is bridged; document whichever form passes in `HW-RESULTS`/comments. Resolve the mechanism in THIS step before writing connect().

- [ ] **Step 2: Run the client gate to confirm it FAILS (connect stubbed → 0)**

```bash
cd ~/Development/rt1170/evkb/ethernet_test && ./run_qemu_ethernet.sh client 2>&1 | tail -8
```
Expected: `FAIL: no client echo` (VCOM shows `CLIENT_ECHO=FAIL`).

- [ ] **Step 3: Implement `connect(IPAddress, uint16_t)`**

Replace the Task-3 stub in `EthernetClient.cpp`:
```cpp
static err_t eth_connected_cb(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)pcb; int i = (int)(intptr_t)arg;
    if (err == ERR_OK) eth_conns[i].state = CONN_ESTABLISHED;
    return ERR_OK;
}

int EthernetClient::connect(IPAddress ip, uint16_t port) {
    int i = eth_conn_alloc();
    if (i < 0) return 0;
    struct tcp_pcb *pcb = tcp_new();
    if (!pcb) { eth_conns[i].state = CONN_FREE; return 0; }
    eth_conns[i].pcb = pcb; eth_conns[i].state = CONN_CONNECTING;
    eth_conn_bind_callbacks(i);
    ip_addr_t dst; IP_ADDR4(&dst, ip[0], ip[1], ip[2], ip[3]);
    if (tcp_connect(pcb, &dst, port, eth_connected_cb) != ERR_OK) { eth_conn_free(i); return 0; }
    _sock = i;
    uint32_t t0 = millis();
    while (eth_conns[i].state == CONN_CONNECTING) {
        eth_pump();
        if (millis() - t0 > 5000) { eth_conn_free(i); _sock = -1; return 0; }
    }
    if (eth_conns[i].state != CONN_ESTABLISHED) { eth_conn_free(i); _sock = -1; return 0; }
    return 1;
}
```
(The `eth_connected_cb` must be declared above `connect`; keep it `static` in this TU.)

- [ ] **Step 4: Build + run the client gate — verify PASS + full regression**

```bash
T=~/Development/rt1170/evkb/ethernet_test; cd "$T"
cmake --build build -j8 >/tmp/eth_build.log 2>&1 && echo BUILD_OK || { tail -40 /tmp/eth_build.log; exit 1; }
for p in boot server udp client; do ./run_qemu_ethernet.sh $p 2>&1 | tail -1; done
```
Expected: `PASS` for all four (client: VCOM shows `CLIENT_ECHO=PASS` — the guest connected outbound to `10.0.2.100:7`, the SLIRP `-cmd` echoed the token). If `guest_errors` shows QEMU rejected `guestfwd`, apply the Step-1 fallback and re-run.

- [ ] **Step 5: Commit**

```bash
cd ~/Development/Ethernet && git add -A && git commit -q -m "feat(ethernet): EthernetClient::connect(IPAddress) outbound (client gate green)

tcp_connect + connected_cb + bounded pump-spin. Client gate via SLIRP
guestfwd -cmd echo proves outbound TCP from the guest.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 6: DNS — connect(host) + beginPacket(host) (dns gate)

**Files:**
- Modify: `~/Development/Ethernet/src/EthernetClient.cpp` (`connect(const char*,port)`)
- Modify: `~/Development/Ethernet/src/EthernetUdp.cpp` (`beginPacket(const char*,port)`)
- Modify: `~/Development/Ethernet/src/utility/socket_lwip.{h,cpp}` (add `eth_resolve`)
- Modify: `~/Development/rt1170/evkb/ethernet_test/ethernet_test.cpp` (`try_dns_once` → print `DNS_OK ip=`)

- [ ] **Step 1: Run the dns gate to confirm it FAILS**

```bash
cd ~/Development/rt1170/evkb/ethernet_test && ./run_qemu_ethernet.sh dns 2>&1 | tail -6
```
Expected: `FAIL: no DNS resolve` (sketch prints only `DNS_TRY ok=0`).

- [ ] **Step 2: Add a synchronous resolver to the glue**

`socket_lwip.h` (add prototype inside the `extern "C"` block):
```c
int eth_resolve(const char *host, ip_addr_t *out, uint32_t timeout_ms);  /* 1 ok, 0 fail */
```
`socket_lwip.cpp` (append; include `#include "lwip/dns.h"`):
```cpp
struct dns_wait { volatile int done; volatile int ok; ip_addr_t addr; };
static void eth_dns_cb(const char *name, const ip_addr_t *ipaddr, void *arg) {
    (void)name; struct dns_wait *w = (struct dns_wait *)arg;
    if (ipaddr) { w->addr = *ipaddr; w->ok = 1; } else w->ok = 0;
    w->done = 1;
}
int eth_resolve(const char *host, ip_addr_t *out, uint32_t timeout_ms) {
    struct dns_wait w; w.done = 0; w.ok = 0;
    err_t e = dns_gethostbyname(host, &w.addr, eth_dns_cb, &w);
    if (e == ERR_OK) { *out = w.addr; return 1; }          /* cached */
    if (e != ERR_INPROGRESS) return 0;
    uint32_t t0 = millis();
    while (!w.done) { eth_pump(); if (millis() - t0 > timeout_ms) return 0; }
    if (w.ok) { *out = w.addr; return 1; }
    return 0;
}
```

- [ ] **Step 3: Wire the hostname overloads**

`EthernetClient.cpp`:
```cpp
int EthernetClient::connect(const char *host, uint16_t port) {
    ip_addr_t a;
    if (!eth_resolve(host, &a, 8000)) return 0;
    uint32_t v = ip_2_ip4(&a)->addr;
    return connect(IPAddress(v&0xff,(v>>8)&0xff,(v>>16)&0xff,(v>>24)&0xff), port);
}
```
`EthernetUdp.cpp`:
```cpp
int EthernetUDP::beginPacket(const char *host, uint16_t port) {
    ip_addr_t a;
    if (!eth_resolve(host, &a, 8000)) return 0;
    uint32_t v = ip_2_ip4(&a)->addr;
    return beginPacket(IPAddress(v&0xff,(v>>8)&0xff,(v>>16)&0xff,(v>>24)&0xff), port);
}
```

- [ ] **Step 4: Upgrade the sketch's `try_dns_once` to report the resolved IP**

Replace `try_dns_once()` in `ethernet_test.cpp` with a direct resolve so the gate sees a real address (SLIRP DNS at 10.0.2.3, handed via DHCP, resolves real names):
```cpp
#include "utility/socket_lwip.h"
static void try_dns_once() {
    did_dns = true;
    ip_addr_t a;
    if (eth_resolve("example.com", &a, 6000)) {
        uint32_t v = ip_2_ip4(&a)->addr;
        Serial1.print("DNS_OK ip=");
        Serial1.print(v&0xff); Serial1.print('.'); Serial1.print((v>>8)&0xff); Serial1.print('.');
        Serial1.print((v>>16)&0xff); Serial1.print('.'); Serial1.println((v>>24)&0xff);
    } else Serial1.println("DNS_FAIL");
}
```

- [ ] **Step 5: Build + run the dns gate — verify PASS + full regression**

```bash
T=~/Development/rt1170/evkb/ethernet_test; cd "$T"
cmake --build build -j8 >/tmp/eth_build.log 2>&1 && echo BUILD_OK || { tail -40 /tmp/eth_build.log; exit 1; }
for p in boot server udp client dns; do ./run_qemu_ethernet.sh $p 2>&1 | tail -1; done
```
Expected: `PASS` for all five (dns: VCOM shows `DNS_OK ip=<addr>`). *Note: the dns gate assumes the host running QEMU has working DNS (SLIRP forwards to the host resolver) — true on the dev Mac.*

- [ ] **Step 6: Commit (both repos)**

```bash
cd ~/Development/Ethernet && git add -A && git commit -q -m "feat(ethernet): DNS - connect(host)/beginPacket(host) via lwIP dns_gethostbyname

Synchronous eth_resolve (cached ERR_OK / ERR_INPROGRESS pump-spin on the
dns callback / timeout). dns gate green via SLIRP's 10.0.2.3 resolver.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
git -C ~/Development/rt1170/evkb add ethernet_test/ethernet_test.cpp
git -C ~/Development/rt1170/evkb commit -q -m "test(ethernet-m3): dns phase asserts DNS_OK ip= (sketch resolves example.com)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 7: Hardware verification (INTERACTIVE bench step — not subagent-executable)

Needs the physical board on the real LAN — the same bench as milestones 1 & 2: board's **10/100 RJ45 → router**, **SD card OUT** (AD_32=MDC muxes with SD1_CD_B), Mac on Wi-Fi through the same router. Drive from the Mac.

- [ ] **Step 1: Flash + capture the DHCP lease**

```bash
pkill -9 LinkServer redlinkserv 2>/dev/null
# start the resilient pyserial VCOM reader BEFORE the flash (DHCP_OK / ETH_DHCP prints once)
python3 <reader> /dev/cu.usbmodem5DQ2DDHVWO5EI3 80 /tmp/eth_hw.log &   # (reuse the m2 lwip_capture.py pattern, grep "ETH_DHCP ok=1 ip=")
/Applications/LinkServer_26.6.137/LinkServer run MIMXRT1176:MIMXRT1170-EVKB \
  ~/Development/rt1170/evkb/ethernet_test/build/ethernet_test.elf
```
Read the lease from VCOM: `ETH_DHCP ok=1 ip=<lease>` and `ETH_NETIF_UP`.

- [ ] **Step 2: Exercise all four paths from the Mac**

```bash
L=<lease>
ping -c 6 $L                                   # ICMP via lwIP
printf 'HWTCP\n' | nc -w3 $L 7                  # EthernetServer echo -> expect HWTCP
printf 'HWUDP\n' | nc -u -w3 $L 7               # EthernetUDP echo   -> expect HWUDP
# EthernetClient outbound: run a host echo, watch VCOM for the board's client probe
python3 -c "import socket;s=socket.socket();s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1);s.bind(('0.0.0.0',7));s.listen(1);\nwhile 1:\n c,_=s.accept();d=c.recv(64);c.sendall(d);c.close()" &
# (point the board's try_client_once at the Mac's LAN IP for the HW run, or confirm CLIENT_ECHO=PASS if reachable)
```
VCOM should show `CLIENT_ECHO=PASS` and `DNS_OK ip=<addr>` (board resolves `example.com` via the router's DNS).

- [ ] **Step 3: Write `ethernet_test/HW-RESULTS.md`** — record the lease, the ping stats, the TCP/UDP echo round-trips, the `CLIENT_ECHO`/`DNS_OK` VCOM lines, the topology, and the pre-flight checklist (mirror `enet_test/HW-RESULTS.md` and `lwip_test/HW-RESULTS.md`).

- [ ] **Step 4: Commit** the HW results to evkb (only that file):
```bash
git -C ~/Development/rt1170/evkb add ethernet_test/HW-RESULTS.md
git -C ~/Development/rt1170/evkb commit -q -m "test(ethernet-m3): HW-verify the Arduino Ethernet API on MIMXRT1176-EVKB

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Definition of done

- Six QEMU gates green: `boot`, `server`, `udp`, `client`, `dns` (+ the five m2 gates still green after the DNS flip).
- HW-verified: real DHCP lease, ping, `EthernetServer`/`EthernetUDP` echo, `EthernetClient` outbound, DNS resolve — recorded in `ethernet_test/HW-RESULTS.md`.
- `cores/imxrt1176/enet.c` unchanged. `~/Development/lwip` changed by only the `LWIP_DNS` flip. Three repos on `master`, nothing pushed.
- Update the `rt1176-ethernet-*` / add an `rt1176-ethernet-arduino-api` memory note after HW passes.
```
