# RT1176 Ethernet milestone 3 — Arduino `Ethernet` socket API (design)

**Date:** 2026-07-12
**Status:** approved design → ready for implementation plan
**Builds on:** milestone 1 (`cores/imxrt1176/enet.c` — FEC MAC + RTL8201 PHY, HW-verified) and milestone 2 (lwIP 2.2.1 `NO_SYS` raw stack at `~/Development/lwip`, ping+DHCP+UDP+TCP, HW-verified).

## Goal

Provide the familiar Arduino `Ethernet` socket API — `EthernetClient`, `EthernetServer`, `EthernetUDP`, and the `Ethernet` (`EthernetClass`) singleton — on top of the HW-verified lwIP raw (callback) API, so ordinary Arduino networking sketches (WebClient, echo servers, UDP tools, and libraries like PubSubClient) compile and run unchanged on the MIMXRT1176-EVKB.

## Architecture (summary)

A **new library `~/Development/Ethernet`** (its own git repo, alongside the SPI/Wire/SdFat forks) exposes the standard Arduino `Ethernet` public API and implements it over an internal *socket-glue* layer that wraps lwIP raw PCBs. The stack stays **async underneath** (lwIP's `tcp_*`/`udp_*`/`dns_*` callbacks) and presents a **thin synchronous facade** at the Arduino edge. Because the design is single-threaded `NO_SYS`, every lwIP callback fires only inside a cooperative **pump** (`ethernetif_poll` + `sys_check_timeouts`) that the library calls from its own blocking/poll methods and from the core `yield()` — so there are **no data races and no locks**.

**Tech stack:** C++ (Arduino library), lwIP 2.2.1 raw API (`NO_SYS=1`, `LWIP_SOCKET=0`), the frozen `enet.c` L2 driver, the teensy-cmake gate harness + QEMU `imx.enet` (SLIRP) + pyserial VCOM on hardware.

## Scope

**In (chosen: "core 3 classes + DNS"):**
- `EthernetClient` — `connect(IPAddress,port)`, `connect(const char* host,port)` (DNS), `write`/`print`, `available`, `read`, `peek`, `flush`, `stop`, `connected`, `operator bool`.
- `EthernetServer` — `begin`, `available`, `accept`, `write`/`print` (broadcast).
- `EthernetUDP` — `begin`, `beginPacket(IPAddress|host, port)`, `write`, `endPacket`, `parsePacket`, `available`, `read`, `peek`, `flush`, `stop`, `remoteIP`, `remotePort`.
- `EthernetClass` — `begin(mac[,timeout,responseTimeout])` (DHCP), `begin(mac, ip[, dns[, gateway[, subnet]]])` (static), `maintain`, `linkStatus`, `hardwareStatus`, `localIP`, `subnetMask`, `gatewayIP`, `dnsServerIP`, `setDnsServerIP`, `MACAddress`/`setMACAddress`.
- **DNS** — enable `LWIP_DNS=1` in the vendored lwip library so hostname resolution works (both `EthernetClient::connect(host,port)` and `EthernetUDP::beginPacket(host,port)`).

**Deferred (out of scope for m3):**
- UDP multicast (`beginMulticast`) — needs `LWIP_IGMP`.
- Optional async hooks (`onData`/`onConnect`/`connectAsync`) — the pure-sync facade is sufficient; can be added later as a superset without breaking the API.
- Timer-ISR / background pump — cooperative pump only; the ISR pump (with `SYS_ARCH_PROTECT`) is a documented future upgrade if background progress is ever required.

## Approach decision

**Chosen — A: fresh `~/Development/Ethernet` wrapping lwIP raw API.** Least code, keeps the HW-verified `NO_SYS` model, no FNET baggage, matches the fork pattern; we size the buffers and control the async→sync bridge precisely. Public API is copied from the standard Arduino `Ethernet` library (Paul Stoffregen, MIT) so sketches compile unchanged; only the private socket layer is ours.

**Rejected:**
- **B — retarget `~/Development/NativeEthernet`.** It is deeply FNET-coupled (`fnet_socket_t`, `fnet.h`, `DMAMEM socket_buf` arrays, an `IntervalTimer` poll, `teensyMAC`, `setStackHeap`); FNET sockets buffer internally while lwIP-raw does not, so the buffering is rewritten regardless — more net work, with dead scaffolding left behind.
- **C — enable lwIP sockets (`LWIP_SOCKET=1`/`LWIP_NETCONN=1`).** Requires `NO_SYS=0` + a threading layer (`sys_mbox`/`sys_sem`/`tcpip_thread`) — an RTOS-ish bring-up on bare metal, more RAM, and it abandons the `NO_SYS` design just HW-verified in m2.

## The async model (why sync facade over async core)

The Arduino contract is synchronous (`if (client.connect(host,80)) { client.write(...); while (client.available()) client.read(); }`), so surfacing async to the *sketch* would break compatibility with every stock example and library. Therefore: **async internally, synchronous adapter at the edge.** The adapter is genuinely thin — `read`/`available`/`peek`/`parsePacket` are non-blocking (they return whatever the async callbacks have accumulated); only `connect`, DNS resolve, `write`-when-send-buffer-full, `flush`, and `stop` ever wait, and each waits **cooperatively** (pumps the stack + hooks `yield()` + bounded by a timeout), so a well-written non-blocking sketch never truly blocks. A continuation/coroutine "async context" object would be over-engineering with no scheduler present.

## Component 1 — file structure & the connection pool

```
~/Development/Ethernet/
  library.properties                       name=Ethernet (fork-pattern metadata)
  keywords.txt
  src/Ethernet.h      / Ethernet.cpp       EthernetClass: netif owner, begin/config, the pump
  src/EthernetClient.h / .cpp              EthernetClient : Client
  src/EthernetServer.h / .cpp              EthernetServer : Server
  src/EthernetUdp.h    / .cpp              EthernetUDP : UDP
  src/utility/socket_lwip.h / .cpp         internal glue: the TCP-connection pool + raw callbacks
  examples/                                (optional demo sketches)
```

**Connection pool (sockindex model).** Standard Arduino Ethernet identifies a socket by a `uint8_t sockindex` into a fixed pool; copying an `EthernetClient` copies the index (copies share one connection), and `EthernetServer::available()` returns a client by index. We adopt this exactly:

```c
typedef enum { CONN_FREE=0, CONN_CONNECTING, CONN_ESTABLISHED, CONN_CLOSING, CONN_CLOSED } conn_state_t;
typedef struct {
    struct tcp_pcb *pcb;        /* NULL once lwIP has freed it (err_cb) */
    struct pbuf    *rx_head;    /* received-but-unread pbuf chain (we hold it) */
    uint16_t        rx_off;     /* read offset into rx_head */
    volatile conn_state_t state;
    ip_addr_t       remote_ip;
    uint16_t        remote_port;
    uint16_t        accept_port;/* listening port a server connection arrived on; 0 for outbound */
    err_t           last_err;
} tcp_conn_t;

#define MAX_SOCK_NUM 4          /* within lwipopts MEMP_NUM_TCP_PCB=5 */
tcp_conn_t conns[MAX_SOCK_NUM];
```

`EthernetClient`/`EthernetServer` hold indices into `conns[]`; `EthernetUDP` is independent (its own `udp_pcb`).

**The pump.** `NO_SYS` has no background thread, so lwIP advances only when the library calls `ethernet_loop()` = `ethernetif_poll(&s_netif) + sys_check_timeouts()`. Every blocking/poll method calls it, and it is hooked into the core `yield()` (already wired for EventResponder) so `delay()` pumps too. **The pump carries a reentrancy guard** (a static `in_pump` flag; `ethernet_loop()` returns immediately if already pumping) — a `write`/`connect` pump-spin calls `yield()`, which pumps, so without the guard a `yield()` reached from inside a pump would recurse into lwIP. The guard keeps every lwIP entry on a single, non-nested call path.

## Component 2 — EthernetClient (the async→sync bridge)

**Raw callbacks (all fire inside the pump → lock-free):**
- `recv_cb(arg,pcb,p,err)` — `p==NULL` ⇒ peer FIN ⇒ `state=CLOSING` (keep `rx_head` so buffered data stays readable). Else `pbuf_cat(rx_head,p)`. **Do not `tcp_recved` here** — the window is credited on drain (in `read`), which yields correct TCP flow control for free.
- `connected_cb(arg,pcb,err)` — `state=ESTABLISHED`.
- `err_cb(arg,err)` — lwIP has already freed the pcb ⇒ `pcb=NULL; state=CLOSED; last_err=err`.
- `sent_cb(arg,pcb,len)` — no-op beyond letting a blocked `write` re-check `tcp_sndbuf`.

**Methods:**
- `int connect(IPAddress ip, uint16_t port)` — allocate a FREE slot; `tcp_new`; `tcp_arg`; wire the 4 callbacks; `state=CONNECTING`; `tcp_connect(pcb,&ipaddr,port,connected_cb)`; pump-spin until `ESTABLISHED` (→1) or `CLOSED`/err/timeout (→0, free slot).
- `int connect(const char* host, uint16_t port)` **[DNS]** — `dns_gethostbyname(host,&addr,dns_found_cb,ctx)`: `ERR_OK` ⇒ resolved from cache ⇒ connect by IP; `ERR_INPROGRESS` ⇒ pump-spin until `dns_found_cb` delivers the address or timeout, then connect; other ⇒ 0.
- `size_t write(uint8_t)` / `size_t write(const uint8_t* buf, size_t len)` — loop: `n=min(left, tcp_sndbuf(pcb))`; if `n==0` pump-spin until `sent_cb` frees space (bounded); `tcp_write(pcb,ptr,n,TCP_WRITE_FLAG_COPY)`; advance; then `tcp_output(pcb)`. Return bytes accepted. `COPY` ⇒ caller buffer reusable immediately.
- `int available()` — pump; return `Σ pbuf tot_len − rx_off` over the rx chain.
- `int read()` / `int read(uint8_t* buf, size_t size)` — pump; copy from `rx_head+rx_off`; **as each pbuf empties, `tcp_recved(pcb, consumed)`** (credit window), `pbuf_free` and advance; return bytes or −1.
- `int peek()` — first unread byte, non-consuming.
- `void flush()` — `tcp_output(pcb)` (optionally pump until the send queue drains).
- `void stop()` — detach callbacks; `tcp_close(pcb)` (→`tcp_abort` on failure); free rx pbufs; pump so the FIN goes out; slot → FREE.
- `uint8_t connected()` — `ESTABLISHED`, **or** unread data remains (Arduino keeps `connected()` true past a peer FIN while bytes remain buffered).
- `operator bool()` — sockindex is valid / slot in use.

## Component 3 — EthernetServer + EthernetUDP

**EthernetServer** shares the same `conns[]` pool (an accepted connection *is* an `EthernetClient` by index). Holds `struct tcp_pcb *listen_pcb; uint16_t port;`.
- `begin()` — `tcp_new` → `tcp_bind(IP_ANY_TYPE,port)` → **`listen_pcb = tcp_listen(pcb)`** (reassign — `tcp_listen` frees the original and returns a smaller listen pcb) → `tcp_accept(listen_pcb, accept_cb)` → `tcp_arg(listen_pcb, this)`.
- `accept_cb(arg,newpcb,err)` — grab a FREE slot; **none free ⇒ `tcp_abort(newpcb)` + return `ERR_ABRT`** (refuse cleanly). Else init the slot like a connected client (wire the 4 callbacks, `state=ESTABLISHED`, `accept_port=port`, remote ip/port from `newpcb`); return `ERR_OK`.
- `available()` — pump; return the first server-owned slot (`accept_port==port`) with unread data as `EthernetClient(sockindex)`; else an invalid/false client.
- `accept()` — return each newly-accepted connection exactly once (ownership transfer). `write`/`print` broadcast to all this server's connected slots.

**EthernetUDP** is independent of the TCP pool — owns `udp_pcb *pcb`, a fixed-depth RX datagram ring, a TX assembly buffer, and current-packet parse state.
- `begin(port)` — `udp_new` → `udp_bind(IP_ANY_TYPE,port)` → `udp_recv(pcb, recv_cb, this)`.
- `recv_cb(arg,pcb,p,addr,port)` — enqueue `{pbuf*, src ip, src port}`; ring full ⇒ `pbuf_free(p)` (drop).
- `parsePacket()` — pump; free any partially-read current packet; dequeue the next; set `remoteIP`/`remotePort`; return its `tot_len` (else 0).
- `available()` — `cur ? cur->tot_len − cur_off : 0`. `read`/`peek` drain the current datagram's pbuf, freeing it when emptied.
- `beginPacket(IPAddress ip, uint16_t port)` and `beginPacket(const char* host, uint16_t port)` (DNS) — stash dest, reset the TX buffer. `write` appends (bounded ≈1472 B). `endPacket()` — wrap the TX buffer in a `PBUF_RAM` pbuf, `udp_sendto(pcb,txp,&dest,dport)`, free; return 1/0.
- `stop()` — `udp_remove(pcb)`; free queued/current/tx pbufs. `remoteIP`/`remotePort` from the current packet.

## Component 4 — EthernetClass + DNS/DHCP integration

`EthernetClass` (the `Ethernet` singleton) absorbs the milestone-2 sketch logic; it owns `struct netif s_netif` and the pump.
- `int begin(uint8_t* mac, unsigned long timeout=60000, unsigned long responseTimeout=4000)` — copy `mac`→`g_mac` (the `ethernetif.c` extern); `lwip_init()` (once-guard); `netif_add(&s_netif, ANY,ANY,ANY, NULL, ethernetif_init, ethernet_input)`; `netif_set_default`; `netif_set_up`; `dhcp_start`; pump-spin until `dhcp_supplied_address(&s_netif)` or `timeout` → 1/0.
- `void begin(mac, ip)` / `(mac, ip, dns)` / `(mac, ip, dns, gateway)` / `(mac, ip, dns, gateway, subnet)` — static config with Arduino defaulting (gateway = ip with last octet `.1`, subnet `255.255.255.0`, dns = gateway); `netif_add` with the address; `netif_set_up`; no DHCP; `dns_setserver(0, dns)` when a DNS is provided.
- `int maintain()` — pump (`sys_check_timeouts` auto-drives DHCP renew/rebind) + return `DHCP_CHECK_NONE` (0). A compatibility shim that also keeps the stack live.
- `EthernetLinkStatus linkStatus()` — `enet_phy_link_up()` → `LinkON`/`LinkOFF`. `hardwareStatus()` — returns a fixed non-`EthernetNoHardware` sentinel (no W5100).
- `localIP`/`subnetMask`/`gatewayIP` from `s_netif`; `dnsServerIP()`=`dns_getserver(0)`; `setDnsServerIP()`=`dns_setserver(0,…)`; `MACAddress`/`setMACAddress`.
- Internal `ethernet_loop()` = `ethernetif_poll(&s_netif)+sys_check_timeouts()`.

**DNS enablement (vendored `~/Development/lwip` only — `enet.c` stays FROZEN):**
- `port/lwipopts.h`: `LWIP_DNS=1`; `MEMP_NUM_UDP_PCB` `+1` (DNS uses a UDP pcb); default `DNS_TABLE_SIZE`/`DNS_MAX_SERVERS`.
- Confirm `src/core/dns.c` is present in the selective vendor copy (the earlier `core/*.c` sweep should include it; add it if missing).
- With `LWIP_DNS=1`, the DHCP client auto-populates the resolver from DHCP option 6, so `Ethernet.begin(mac)` yields a working resolver with no extra code.
- **Re-run all five m2 gates** (boot/ping/dhcp/udp/tcp) after this flag flip to prove no regression.

## Error handling (cross-cutting)

- Every pump-spin is **timeout-bounded** — `connect`, DNS, `write`-when-full, DHCP — so the API fails gracefully (returns 0/false) and never hangs.
- Pool exhaustion — `connect()` with no FREE slot returns 0; `accept_cb` with no slot refuses via `tcp_abort`.
- `err_cb` (RST/abort) nulls the freed pcb and marks the slot `CLOSED` — no use-after-free; later `write`→0, `connected()`→false once buffered data drains.
- pbuf-alloc failure — drop: TCP backpressures by withholding `tcp_recved`; a UDP drop is normal.

## Testing / gate strategy

Reuse the m2 hybrid harness → new **`~/Development/rt1170/evkb/ethernet_test/`** (cloned from `lwip_test`), importing cores + lwip (DNS-on) + the new `Ethernet` lib with enumerated subdirs (incl. `src` and `src/utility`). One ELF, phase-selected `-nic` + a host peer; gate-first TDD; run via `./run_qemu_ethernet.sh <phase>` (`./` not `sh`).

- **boot** — `Ethernet.begin()` + netif up. The **compile+link acceptance** (biggest integration risk: new lib + lwip + cores link). Prints `ETH_BEGIN_OK`.
- **server** — `EthernetServer` echo on port 7; `-nic user,model=imx.enet,hostfwd=tcp::5555-:7`; host python connects to `127.0.0.1:5555`, sends a token, asserts the echo. Exercises `accept_cb` → pool → `read`/`write` through the sync facade.
- **udp** — `EthernetUDP` echo on port 7; `hostfwd=udp::5556-:7`; host asserts echo (m2's udp path, now via the class).
- **dns** — sketch resolves a hostname; under `-nic user`, **SLIRP's built-in DNS at 10.0.2.3** (handed out by DHCP) resolves a real name → prints `DNS_OK ip=<addr>`, host asserts non-zero.
- **client** — `EthernetClient` connects **outbound** to a host:port, writes, reads the echo. **The one new harness unknown for m3** (outbound TCP from the guest): use SLIRP **`guestfwd`** (guest → `10.0.2.100:7` forwarded to a host echo listener), validated early in the plan (as m2 de-risked `-nic user+imx.enet`); fallback = the SLIRP gateway `10.0.2.2` to a host listener.

**Hardware verification (the arbiter)** on the real LAN (same bench as m1/m2: 10/100 RJ45, SD card out, Mac on Wi-Fi through the router):
- `Ethernet.begin(mac)` → real DHCP lease (VCOM `ETH_DHCP ip=<lease>`).
- `EthernetServer` echo → from the Mac `nc <lease> 7`.
- `EthernetUDP` echo → `nc -u <lease> 7`.
- `EthernetClient` → connects **out** from the board to a python echo server on the Mac (`<mac-ip>:7`); VCOM shows the echoed bytes.
- `DNS` → resolves a public hostname (e.g. `example.com`) via the router's DNS; VCOM `DNS_OK ip=<addr>`.
Write `ethernet_test/HW-RESULTS.md`.

## Guardrails & repo boundaries

- The core `cores/imxrt1176/enet.c` is **FROZEN** (no changes).
- The vendored `~/Development/lwip` gains **only** `LWIP_DNS` (lwipopts + `dns.c` if missing); re-run the 5 m2 gates after.
- New library `~/Development/Ethernet` — `git init`; `git add -A` is fine **there only**.
- Gate files commit to `git -C evkb` — **never `git add -A` in evkb** (shared tree; stage only touched files).
- Build with `-DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake`; a new-library import requires `rm -rf build` to defeat the `import_arduino_library` flat-GLOB reconfigure trap.
- Commit to `master` in each repo; **push only when the user asks**.

## Open risks

1. **Outbound-client gate mechanism** — SLIRP `guestfwd` to a host listener is the plan; validate it in an early task, keep the `10.0.2.2`-gateway fallback. (Bench HW is the ultimate arbiter for the client path.)
2. **`LWIP_DNS` regression** — flipping the flag changes the m2 library; the re-run of all 5 m2 gates is the guard.
3. **Pool sizing** — `MAX_SOCK_NUM=4` vs `MEMP_NUM_TCP_PCB=5`; if a gate needs more concurrent sockets, bump lwipopts (our library, allowed).
4. **pbuf lifetime in the RX-hold model** — the "hold the pbuf chain, `tcp_recved` on drain" path must free correctly on `stop`/`err` mid-stream (covered by the error-handling rules); code review should scrutinize it.
