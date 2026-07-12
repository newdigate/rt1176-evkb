# RT1176 Ethernet milestone 2 — lwIP TCP/IP stack (ping + DHCP + UDP + TCP) — Design

**Status:** approved (design), ready for implementation plan
**Date:** 2026-07-11

## Goal

Bring up the **lwIP 2.2.1** TCP/IP stack (bare-metal `NO_SYS`) on top of the HW-verified RT1176
ENET foundation so the board runs a **real IP stack**: lwIP auto-answers ARP + ICMP (answer a ping
*through the stack*), pulls a **DHCP** lease, and echoes **UDP** and **TCP**. lwIP binds to the
existing core L2 driver `cores/imxrt1176/enet.c` (the four functions `enet_init`,
`enet_send_frame`, `enet_read_frame`, `enet_phy_link_up`) through a **small, hand-rolled netif
port** — NOT the SDK's `fsl_enet`-coupled `ethernetif` glue.

This is **milestone 2** of the Ethernet arc (v1 = MAC + PHY + a hand-rolled ARP/ICMP responder, done
+ HW-verified). The demo here uses lwIP's **raw API** (`raw`/`udp`/`tcp` PCBs) in the gate sketch;
the Arduino `Ethernet` socket classes (`EthernetClient`/`Server`/`UDP`) that *wrap* lwIP are
**milestone 3** — out of scope.

**Success:** four green QEMU gates (ping / DHCP / UDP echo / TCP echo) + HW verification (the board
pulls a real DHCP lease from a router, answers a ping, and echoes UDP + TCP from a LAN host).

## Why this shape (exploration findings)

Two parallel read-only recon passes over the SDK lwIP middleware, the netif↔MAC seam, the library/
placement precedent, and QEMU SLIRP established the design. Highlights (with citations):

- **lwIP 2.2.1 is the only lwIP present** — the NXP SDK's `~/Development/mcuxsdk-ws/mcuxsdk/
  middleware/lwip/` (`src/include/lwip/init.h`: 2.2.1-dev). No fresh lwIP, no QNEthernet.
  `NativeEthernet` wraps **FNET** behind a W5100 shim (wrong stack); `FNET` is a whole alternative
  stack (our `enet.c` was already lifted from its `fnet_fec.c`). So **vendor the SDK's lwIP `src/`**.

- **The netif↔MAC seam is tiny and exact — and the SDK glue is the wrong thing to retarget.** The
  SDK splits the port into generic `port/ethernetif.c` (netif wiring + the RX pump) and platform
  `port/enet_ethernetif_kinetis.c` (the `low_level_*` that call `fsl_enet`). The platform file is
  **entangled** with `fsl_enet.h`, the `fsl_phy` component (`ethernetif_config_t` phyHandle/ops/
  resource), zero-copy `pbuf_alloced_custom` RX callbacks, FreeRTOS, a GPIO adapter, and multicast
  hash filters — gutting it is *more* work than a clean hand-roll. The exact SDK-MAC call sites (so
  we know what our four functions replace): `ENET_Init` (`enet_ethernetif_kinetis.c:521`),
  `ENET_SendFrame` (`:597/:614`), `ENET_GetRxFrame` (`:707`); the NO_SYS RX pump is
  `fetch_received_pkts` (`ethernetif.c:201`, the `#else` at `:289`). **Decision: hand-roll a
  ~120-line `ethernetif.c` from lwIP's own contrib skeleton** (`middleware/lwip/contrib/examples/
  ethernetif/ethernetif.c:184-275`), binding directly to `enet.c`.

- **Bare-metal is simple here.** The SDK `lwip_ping/bm` config (`prj.conf`) is `NO_SYS=1`,
  `LWIP_NETCONN=0`/`LWIP_SOCKET=0`, `LWIP_TCP/UDP/ICMP/RAW/DHCP=1`, `MEM_SIZE=22528`, `TCP_MSS=1460`.
  The NO_SYS timer (`sys_now()`) just needs a millisecond counter — **our core already has a 1 ms
  SysTick + `millis()`**, so `sys_now(){ return millis(); }` and we need **no** `sys_arch.c`/`.h`
  (lwIP includes `arch/sys_arch.h` only when `NO_SYS==0`). The main loop is
  `{ ethernetif_poll(&netif); sys_check_timeouts(); }` — our `enet_read_frame` is already polled, so
  it maps 1:1 onto `fetch_received_pkts`' `while(1)` drain.

- **The copying glue keeps memory simple.** low_level_output = `pbuf_copy_partial` into a flat scratch
  buffer → `enet_send_frame` (which already memcpys into its OCRAM `tx_buf` + pads runts).
  low_level_input = `enet_read_frame` into a scratch → `pbuf_alloc(PBUF_POOL)` + `pbuf_take`. So
  **lwIP's pools live in ordinary BSS (DTCM); only `enet.c`'s descriptor rings need OCRAM** — no
  `DMAMEM` concern for lwIP itself.

- **lwIP is a library, not core (unambiguous).** The lean core holds only Arduino base classes +
  per-peripheral drivers (incl. `enet.c`, the L2 MAC); every big stack (Audio nodes, `USBHost_t36`,
  `FNET`, `SPI`, `Wire`, `SdFat`) is a separate library under `~/Development/`. `import_arduino_
  library` (`teensy-cmake-macros/CMakeLists.include.txt:282`) is a **flat, non-recursive** GLOB, so a
  vendored `lwip/src/**` must **enumerate each source subdir** — exactly like `sd_wav_play_test`
  imports SdFat's 9 subdirs (`sd_wav_play_test/CMakeLists.txt:34-36`).

- **QEMU SLIRP does the hard servers for us.** This qemu2 (11.0.50) has SLIRP built in
  (`build/config-host.h` `CONFIG_SLIRP`); `-nic user,model=imx.enet` gives a **real built-in DHCP
  server** (net 10.0.2.0/24, gw/host 10.0.2.2, first lease 10.0.2.15, DNS 10.0.2.3 —
  `net/slirp.c:445-449`) plus UDP/TCP NAT + `hostfwd`/`guestfwd`. **But SLIRP cannot inject an
  unsolicited inbound ICMP echo** to the guest (ICMP isn't port-forwardable) → the *ping* gate stays
  on our socket + Python-peer path (proven in v1), while DHCP/UDP/TCP ride SLIRP.

## Scope

**In scope:** the vendored `lwip` library (`src/` stack, imported by the gate); the port layer
(`port/lwipopts.h`, `port/arch/cc.h`, `port/ethernetif.c` + `sys_now`); a lwIP raw-API demo in the
gate sketch (netif bring-up with DHCP-and-static-fallback, a UDP echo server, a TCP echo server);
one `evkb/lwip_test/` gate with four phases (ping / dhcp / udp / tcp); HW verification.

**Explicitly deferred (YAGNI):** the Arduino `Ethernet` socket API (`EthernetClient/Server/UDP`) —
**milestone 3**; lwIP's `netconn`/`socket` APIs (`LWIP_NETCONN=0`/`LWIP_SOCKET=0` — raw API only, no
RTOS); IPv6; the SDK's `ethernetif`/`enet_ethernetif_kinetis` glue (we hand-roll); FreeRTOS; zero-copy
RX (`pbuf_alloced_custom`); MAC multicast/IGMP filtering; DNS/HTTP/mDNS apps; retargeting the SDK
`fsl_phy` component (our `enet_phy_link_up` replaces it). No change to `enet.c` (the L2 driver is
frozen for this milestone) or the QEMU model (SLIRP is already present).

## Decisions (resolved during brainstorming)

- **Vendor lwIP 2.2.1** from the SDK as a local library `~/Development/lwip` (copy `src/`), matching
  how every other dependency is a local repo under `~/Development/`.
- **Hand-roll a clean ~120-line `ethernetif.c`** from lwIP's contrib skeleton, NOT retarget the SDK's
  `fsl_enet`-coupled glue.
- **`NO_SYS=1`, raw API only**; `sys_now()` = `millis()`; no `sys_arch.c`.
- **Copying glue** (pbuf ↔ flat scratch buffer) — lwIP pools in DTCM, rings in OCRAM.
- **One ELF, DHCP-with-static-fallback** — serves both gate topologies (static fallback for the
  socket+peer ping phase; a real lease on the SLIRP phases).
- **Hybrid gate** — socket+peer for ping (SLIRP can't inbound-ICMP); `-nic user`+`hostfwd` for
  DHCP/UDP/TCP (SLIRP is a real DHCP server + TCP/UDP peer).
- **lwIP as a library the gate imports** (enumerated subdirs); the core is untouched.
- **Demo = lwIP raw API in the sketch**; Arduino socket classes are milestone 3.

## Architecture — components

### 1. `~/Development/lwip` — the vendored stack (library)
Copy the SDK's `middleware/lwip/src/` → `~/Development/lwip/src/` — **selectively**, because
`import_arduino_library` is a *flat* GLOB of each named subdir: keep `core/*.c`, `core/ipv4/*.c`
(incl. `dhcp.c`/`etharp.c`/`icmp.c`/`ip4.c`), **only `netif/ethernet.c`**, and `include/`; **OMIT**
`netif/{slipif,bridgeif,bridgeif_fdb,lowpan6*,zepif}.c`, `src/api/`, `src/apps/`, and any `ppp/` — or
the glob of `src/netif` would drag in stack variants we don't build (SLIP/bridge/6LoWPAN). Imported
by the gate:
```cmake
import_arduino_library(lwip $ENV{HOME}/Development/lwip
    src/include src/core src/core/ipv4 src/netif port port/arch)
```
(each subdir contributing sources and/or an `-I` path; `src/api` is NOT listed — no netconn/sockets).

### 2. `~/Development/lwip/port/lwipopts.h` (~40 lines)
NO_SYS bare-metal config from the SDK `lwip_ping/bm/prj.conf` + `template/lwipopts.h`:
`NO_SYS=1`, `LWIP_NETCONN=0`, `LWIP_SOCKET=0`, `SYS_LIGHTWEIGHT_PROT=0`, `LWIP_SINGLE_NETIF=1`,
`LWIP_ICMP=1`, `LWIP_RAW=1`, `LWIP_UDP=1`, `LWIP_TCP=1`, `LWIP_DHCP=1` (ARP implicit via the netif
`ETHARP` flag), `MEM_ALIGNMENT=4`, `MEM_SIZE=22528`, `MEMP_NUM_PBUF=15`, `PBUF_POOL_SIZE`≥8,
`TCP_MSS=1460`, `TCP_SND_BUF=6*TCP_MSS`, `TCP_WND=2*TCP_MSS`, `LWIP_STATS=0`, `ETH_PAD_SIZE=0`.

### 3. `~/Development/lwip/port/arch/cc.h`
The SDK `port/arch/cc.h` near-verbatim — pure compiler abstraction: `LWIP_PLATFORM_DIAG`/`ASSERT`
(route to `Serial1`), byte-order = little-endian (confirm the RT1176 toolchain sets
`BYTE_ORDER=LITTLE_ENDIAN`). No `sys_arch.h`.

### 4. `~/Development/lwip/port/ethernetif.c` (~120 lines, hand-rolled)
Five functions binding lwIP to `enet.c` (static scratch `uint8_t txbuf[1536]`, `rxbuf[1536]`; NO_SYS
single-threaded → one of each is safe):
- `err_t ethernetif_init(struct netif *netif)` — `netif->output = etharp_output`;
  `netif->linkoutput = low_level_output`; `hwaddr_len=6` + memcpy `hwaddr`; `mtu=1500`;
  `flags = NETIF_FLAG_BROADCAST|NETIF_FLAG_ETHARP|NETIF_FLAG_LINK_UP`; `enet_init(netif->hwaddr)`.
- `static err_t low_level_output(netif, p)` — `pbuf_copy_partial(p, txbuf, p->tot_len, 0)`;
  `return enet_send_frame(txbuf, p->tot_len) == 0 ? ERR_OK : ERR_IF`.
- `static struct pbuf *low_level_input(netif)` — `enet_read_frame(rxbuf, &len)`; on 1:
  `p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL); if (p) pbuf_take(p, rxbuf, len); return p`; else NULL.
- `void ethernetif_poll(netif)` — `while ((p = low_level_input(netif))) if (netif->input(p,netif) !=
  ERR_OK) pbuf_free(p);`
- `u32_t sys_now(void) { return millis(); }`.

### 5. The demo app (gate sketch `evkb/lwip_test/lwip_test.cpp`)
lwIP raw API. Bring-up (DHCP-with-static-fallback):
```
lwip_init(); enet_phy_link_up(3000);
netif_add(&netif, IP4_ADDR_ANY, ..., NULL, ethernetif_init, ethernet_input);
netif_set_default(&netif); netif_set_up(&netif); dhcp_start(&netif);
udp_echo_init(); tcp_echo_init();     /* servers up regardless of phase */
loop { ethernetif_poll(&netif); sys_check_timeouts();
       if (dhcp_supplied_address(&netif)) print once "DHCP_OK ip=<addr>";
       if (!leased && millis()-t0 > 5000) { dhcp_stop(&netif); netif_set_addr(&netif, static...); } }
```
- **UDP echo:** `udp_new` / `udp_bind(pcb, IP_ANY, 5556)` / `udp_recv(pcb, cb)`; cb `udp_sendto`s the
  received pbuf back to `addr:port`, prints `UDP_ECHO=PASS`.
- **TCP echo:** `tcp_new` / `tcp_bind(pcb, IP_ANY, 5555)` / `tcp_listen` / `tcp_accept(cb)`; accept-cb
  sets `tcp_recv(newpcb, rcb)`; rcb `tcp_write`+`tcp_output`s the data back, `tcp_recved`, prints
  `TCP_ECHO=PASS`.

## Data flow / behavior

- **TX:** app/stack → `netif->linkoutput` = `low_level_output` → `pbuf_copy_partial` → `enet_send_frame`
  → `enet.c` copies to OCRAM `tx_buf` + kicks `TDAR` → RMII → wire.
- **RX:** wire → RMII → `enet.c` RX ring → `enet_read_frame` → `low_level_input` → `pbuf_alloc`+`pbuf_take`
  → `ethernetif_poll` → `netif->input` = `ethernet_input` → lwIP (etharp/ip4 → icmp/udp/tcp).
- **Timers:** `sys_check_timeouts()` off `sys_now()`=`millis()` drives DHCP retransmit, ARP aging,
  TCP retransmit/keepalive.
- **ICMP:** automatic — once the netif has an IP and is up, `icmp_input` auto-replies to echo requests
  (no app code).

## Testing

**One gate `evkb/lwip_test/`** (clone `enet_test/`), **one ELF, `PHASE`-selected `-nic` + checker**
(mirrors `enet_test`'s structure + the `qrun`/`gate-lib.sh` lifecycle). Firmware always: netif up
(DHCP-fallback) + UDP/TCP echo servers + lwIP auto-ICMP — so one binary serves all phases.

- **`ping`** — `-nic socket,listen=127.0.0.1:$PORT,model=imx.enet` + `lwip_peer.py` (reuse v1's ARP+
  ICMP frame injector, targeting the static-fallback IP). Assert ARP reply + ICMP echo reply. Proves
  the netif↔enet.c glue + lwIP core.
- **`dhcp`** — `-nic user,model=imx.enet`. Board `dhcp_start` → lease `10.0.2.15`. Runner greps VCOM
  `DHCP_OK ip=10.0.2.15`. (Also the first smoke-test of `-nic user`+`imx.enet`.)
- **`udp`** — `-nic user,model=imx.enet,hostfwd=udp::5556-:5556`. `lwip_peer.py udp` sends a datagram
  to `127.0.0.1:5556` → SLIRP → board UDP echo → assert echoed bytes + VCOM `UDP_ECHO=PASS`.
- **`tcp`** — `-nic user,model=imx.enet,hostfwd=tcp::5555-:5555`. `lwip_peer.py tcp` connects
  `127.0.0.1:5555` → sends → board TCP echo → assert + VCOM `TCP_ECHO=PASS`.

**★ New library → from-scratch reconfigure** (the GLOB trap): the first `lwip_test` build needs
`rm -rf build && cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=…` so the enumerated lwIP subdirs are
globbed. Adding a lwIP `src` subdir later likewise needs a reconfigure.

**Hardware (final arbiter):** flash via LinkServer; read VCOM @115200. The board pulls a **real DHCP
lease from the router** → `DHCP_OK ip=<lease>` (dynamic — the HW steps parse the IP from VCOM, then
target it). From a LAN host: `ping <lease>` → replies; `nc -u <lease> 5556` → UDP echo; `nc <lease>
5555` → TCP echo. Same 10/100 RJ45 + no-SD-card + LAN-subnet prerequisites as the v1 ENET HW test
([[rt1176-ethernet-enet]]).

## Risks

- **lwIP memory sizing** — start with the SDK ping values (`MEM_SIZE=22528`, small `PBUF_POOL`); a
  gate that hangs under TCP load usually means pbuf/mem-pool exhaustion → bump `PBUF_POOL_SIZE`/
  `MEM_SIZE`. Tune empirically.
- **`-nic user`+`imx.enet` not yet exercised** in-tree (recon flag) — the `dhcp` gate de-risks it
  first; a wiring quirk there is a QEMU reconciliation (like the v1 `phy-num` fix), not lwIP.
- **DHCP is dynamic on HW** — never hard-code the lease; parse it from VCOM.
- **TCP complexity** — the echo server is tiny (`tcp_recv`→`tcp_write`+`tcp_output`+`tcp_recved`), but
  windowing/retransmit/`err`/`poll` callbacks + `tcp_close` on the peer must be handled; SLIRP is a
  real TCP peer so it exercises them honestly.
- **`sys_now`/timer base** — `sys_check_timeouts` MUST be called every loop or DHCP/TCP stall;
  `millis()` must be monotonic (it is). Confirm no `sys_arch.c` sneaks in (would double-define
  `sys_now`).
- **Header/include ordering** — the port `lwipopts.h` + `arch/cc.h` must be on the include path
  before lwIP's `src/include`; `import_arduino_library` adds each named subdir as `-I`, so list
  `port` and `port/arch` (order via `--start-group`, include order via the arg order).
- **Static-fallback vs DHCP race** — the fallback timer must not fire before a slow DHCP server
  answers (5 s is generous for SLIRP + a home router); make it cancel on `dhcp_supplied_address`.

## References

- **SDK lwIP (vendor source + reference glue):** `~/Development/mcuxsdk-ws/mcuxsdk/middleware/lwip/` —
  `src/` (the stack to copy), `src/include/lwip/init.h` (2.2.1), `port/ethernetif.c` (`ethernetif_init`
  :294, `fetch_received_pkts` :201, NO_SYS branch :289), `port/enet_ethernetif_kinetis.c` (the
  MAC-call sites to replace: `ENET_Init` :521, `ENET_SendFrame` :597/:614, `ENET_GetRxFrame` :707),
  `port/ethernetif_priv.h` (the `low_level_*`/`plat_init` contract), `port/arch/cc.h`, `template/
  lwipopts.h`, `contrib/examples/ethernetif/ethernetif.c:184-275` (the hand-roll skeleton),
  `contrib/apps/ping/ping.c`; the bm ping app + `prj.conf` under `mcuxsdk-examples/lwip_examples/
  lwip_ping/bm/`.
- **The foundation (frozen):** `cores/imxrt1176/enet.c` + `enet.h` (the four functions:
  `enet_init`/`enet_send_frame`/`enet_read_frame`/`enet_phy_link_up`), HW-verified in
  [[rt1176-ethernet-enet]].
- **Gate template + import mechanism:** `evkb/enet_test/` (the socket+peer + `qrun`/`gate-lib.sh` to
  clone; the `lwip_peer.py` reuses its ARP/ICMP frame logic for the ping phase),
  `evkb/sd_wav_play_test/CMakeLists.txt:34-36` (the enumerated-subdir `import_arduino_library`
  precedent), `teensy-cmake-macros/CMakeLists.include.txt:282` (the flat-GLOB import macro).
- **QEMU:** `~/Development/qemu2` (11.0.50, SLIRP built in) — `net/slirp.c:445-449` (10.0.2.x
  defaults + DHCP), `hostfwd`/`guestfwd`; the `imx.enet` model unchanged. Method / HW: gate-first TDD
  ([[rt1170-qemu]]), LinkServer + VCOM ([[rt1170-evkb-flashing]], [[macos-serial-capture]]), the
  lean-core-library precedent ([[rt1176-spi-library-move]], [[rt1176-wire-library-move]]).
