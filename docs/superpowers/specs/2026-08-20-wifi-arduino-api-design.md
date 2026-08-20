# Arduino-style WiFi / WiFiClient / WiFiServer for the M.2 link — design

2026-08-20.  Follows the handoff
`docs/superpowers/handoff/2026-08-20-wifi-arduino-api.md`.  Target: a normal
sketch can do `WiFi.begin(ssid, psk); WiFiClient c; c.connect(host, 80);
c.println(...)` on the MIMXRT1170-EVKB with the u-blox M2-MAYA-W161
(IW416/SD8978) in the M.2 socket, without knowing anything about SDIO, lwip
raw-API discipline, or GPIO9.15/GPIO9.30.

Decisions in this spec were made with the user in a brainstorming pass; each
records its why.

## 1. Where it lives

**A new `arduino/` subdirectory in the existing M2Radio sibling repo**
(`~/Development/M2Radio`, MIT), imported as:

    import_evkb_library(M2Radio sdio iw416 lwip arduino)

Why not the handoff's suggested new sibling repo: M2Radio already spans
importer-chosen layers (`sdio/`, `iw416/`, `lwip/`), `lwip/Iw416Netif.*`
already depends on the lwip library, and a new repo costs a new
`teensy_declare_library` pin, a separate push, and its own
`-DEVKB_FORCE_FETCH=ON` verification for zero structural gain.  One pin bump
at close-out covers everything.

Files:

    M2Radio/arduino/
      Client.h  Server.h  IPAddress.h  IPAddress.cpp   (copied, see §2)
      WiFi.h        WiFi.cpp          (WiFiClass + the WiFi singleton + pump)
      WiFiClient.h  WiFiClient.cpp    (handle onto the connection pool)
      WiFiServer.h  WiFiServer.cpp
      WiFiConnPool.h WiFiConnPool.cpp (slots, lwip callbacks, close/abort)

## 2. The licence landmine — resolved, not re-litigated

The handoff's three routes assumed the clean-room work was still to do.  It
is not: **`~/Development/Ethernet/src/` already carries clean-room MIT
`Client.h`, `Server.h`, `IPAddress.h`, `IPAddress.cpp`** — each headed
"Clean-room MIT implementation … not derived from the LGPL Arduino
{Client,Server}.h", `SPDX-License-Identifier: MIT`, Copyright (c) 2026
Nicholas Newdigate — compiled into a gated rt1176 image
(`examples/networking/ethernet_test`) under a green `license-audit.sh`.
NativeEthernet carries an independent copy of the same four files, so
**per-library duplication of these headers is the established, audited
pattern in this tree.**

Route 1 (full Arduino polymorphism) is therefore taken by **copying those
four files verbatim into `M2Radio/arduino/`**, provenance noted in each copy
("copied from newdigate/Ethernet src/, clean-room MIT, see that repo").
`teensy4/Client.h` / `teensy4/Server.h` (LGPL) are never touched, per
CLAUDE.md.

Accepted cost, to be documented in M2Radio's README: a sketch cannot import
both `Ethernet` and `M2Radio ... arduino` (duplicate `class Client`) —
exactly as `Ethernet` + `NativeEthernet` already cannot.

`Udp.h` is **not** copied: the Ethernet repo's copy carries an
Arduino-inherited header comment and no clean-room/SPDX marking, and
`WiFiUDP` is phase 2 anyway.  When phase 2 comes, `Udp.h`'s provenance must
be established first (survey it the way VGLite's `VENDORING.md` one-liner
does), not assumed from its neighbours.

**`LICENSE_AUDIT_EVKB=$(pwd) ./tools/license-audit.sh` runs before the first
façade file is written** (baseline green), after the base classes are copied,
and after each example lands.  The audit's GATES drift check will demand
entries for both new examples; they are added to `GATES` in
`tools/license-audit.sh` when the examples land.

## 3. Service model — who pumps the stack

Constraint (handoff): Arduino's API is blocking; this lwip is `NO_SYS=1` and
nothing may stall the loop; the W12/W13 ring safety net only ticks on driver
service passes; a façade that only services when the sketch reads is a façade
that strands frames.

**Chosen: automatic servicing from `yield()`, with an off-switch.**

Mechanism: `WiFiClass` owns an `EventResponder` attached with `attach()` (the
yield-dispatched flavour), triggered once in `begin()`; its handler calls
`WiFi.loop()` and re-triggers itself.  The `imxrt1176` core calls `yield()`
after every `loop()` iteration (`main.cpp`) and from every `delay()`
millisecond, so a sketch sitting in `delay(5000)` services the link ~5000
times and the safety net keeps ticking with zero cooperation from the sketch.
`EventResponder::runFromYield()` dispatches one responder per call under a
`runningFromYield` guard, and `yield()` itself has a re-entrancy latch, so
the driver's internal `delay(1)` in `serviceLink()` cannot recurse into the
pump.

    WiFiClass::loop():
      if (m_inService || m_inDriverCmd) return;   // both guards load-bearing
      m_inService = true;
      if (m_linkUp && !iw416NetifPoll(&m_netif)) { linkDownBookkeeping(); }
      sys_check_timeouts();
      m_pool.service();          // deferred-write retries, poll-based reaping
      m_inService = false;

* `m_inService` — re-entrancy latch (belt to yield()'s braces).
* `m_inDriverCmd` — held across every command-port driver call the façade
  makes (`connectStation`, `scan`, `deauthenticate`, `getHwSpec`, ...).
  Iw416.h documents that a `serviceLink()` pass concurrent with a
  command-port exchange steals or misparses the reply; this guard is what
  makes the yield pump safe.  Every façade entry point that calls into the
  driver's command path sets it.

**The pump never initiates scans, associates, or anything blocking.**  It
only ever does one bounded service pass.

Blocking façade calls (`begin`, `WiFiClient::connect`, `read` with data
pending, `stop` draining) wait by pumping: loop `{ WiFi.loop(); }` against
`millis()` timeouts — the same cadence `m2_lwip_test.cpp`'s loop proves out.
They never busy-spin without servicing.

Escape hatch: `WiFi.setAutoService(false)` detaches the responder;
`WiFi.loop()` is public and the sketch owns the cadence, like today's
examples.  `WiFi.radio()` (the `Iw416&`) and `WiFi.netif()` are exposed so
the façade is a floor, not a ceiling.

## 4. Non-negotiables, and where each lands

* **IEEE PS stays ON** (W10 idle-RX-death workaround).  `connectStation()` is
  called with its `psOn=true` default and the façade exposes **no** power-save
  switch.  Reaching PS requires `WiFi.radio().setIeeePs(...)` — deliberate,
  visible, and lands you in the driver where the erratum comment lives.
* **Service continues when the sketch is idle** — §3's whole design.
* **The M.2 board preamble moves into the library**: `WiFi.begin()` performs
  SDIO_RST (GPIO9.15) / WL_RST-PDn (GPIO9.30, via the hand-bridged R404)
  release with the 1 s ROM-boot wait, then `sdio.useIoVoltage1V8(true)`.
  The full lesson comment ("an example without this is green in QEMU and dead
  on silicon, measured W9") moves with the code.  The four existing m2_*
  examples are **not edited** (their build dirs carry creds + fw-blob paths
  and they are the record of five closed faults); each gets only a one-line
  comment added next to its own copy pointing at the library home.  A
  `begin(..., doBoardPreamble=false)` escape exists for a future board where
  the pins differ.
* **Credentials never enter the repo**: examples use the exact
  `m2_lwip_test/CMakeLists.txt` pattern — `-DM2RADIO_WIFI_SSID/_PSK` cache
  vars rendered into a gitignored generated header in the build dir, CMake
  prints "(PSK supplied, not shown)".  The firmware blob likewise
  (`-DM2RADIO_IW416_FW`).

## 5. WiFiClass contract

```cpp
class WiFiClass {
public:
  // Firmware must be supplied before begin() on silicon (the blob is
  // NXP-licensed and never in any repo).  Examples wire this from the
  // configure-time HAVE_IW416_FW pattern.
  void setFirmware(const uint8_t *fw, uint32_t len);

  // Full bring-up: board preamble -> sdio.begin -> iw416.begin ->
  // firmware download (skipped if fwStatus() already reads FIRMWARE_READY —
  // covers QEMU fw-preboot and never-reset warm cards) -> refreshIoPort/
  // enableHostInt/getHwSpec/reconfigureTxBuffers/macControl/set11nCfg/
  // amsduAggrCtrl -> lwip_init/netif_add/netif_set_default/netif_set_up ->
  // connectStation (psOn default) -> dhcp_start -> pump until DHCP supplies
  // an address or timeoutMs expires.  Returns the resulting status().
  int begin(const char *ssid, const char *psk = nullptr,
            uint32_t timeoutMs = 30000, bool doBoardPreamble = true);

  void disconnect();                  // deauth + dhcp_stop + link-down
  uint8_t status();                   // wl_status_t (Arduino/WiFiNINA values)
  IPAddress localIP(), subnetMask(), gatewayIP(), dnsServerIP();
  uint8_t *macAddress(uint8_t *mac);
  const char *SSID();                 // connected AP's SSID
  int32_t RSSI();                     // SCAN-TIME RSSI of connectedAp();
                                      // the driver has no live-RSSI command
  int hostByName(const char *host, IPAddress &out, uint32_t timeoutMs = 5000);

  void loop();                        // one bounded service pass (§3)
  void setAutoService(bool on);       // default true
  void setAutoReconnect(bool on);     // default FALSE, see below
  Iw416 &radio();  struct netif *netif();
};
extern WiFiClass WiFi;
```

Status values follow the Arduino `wl_status_t` convention: `WL_NO_SHIELD`
(no card answered CMD5 / no function 1 / no firmware and none supplied),
`WL_NO_SSID_AVAIL` (scan completed, SSID absent — `connectStation`'s
`BAD_CIS`), `WL_CONNECT_FAILED` (association/handshake failed),
`WL_CONNECTED` (associated **and** DHCP supplied an address — `localIP()` is
real when begin() says connected), `WL_CONNECTION_LOST` (was up, dropped),
`WL_IDLE_STATUS`, `WL_DISCONNECTED`.

**Auto-reconnect defaults OFF.**  On link loss (`iw416NetifPoll` false, or
LINK_LOST/deauth events) the pump does bookkeeping only: `dhcp_stop`,
`netif_set_link_down`, status -> `WL_CONNECTION_LOST`, and every pool
connection is aborted (their `tcp_err` path resets slot state).  The sketch
decides when to re-`begin()`.  With `setAutoReconnect(true)`, reconnect runs
from the **sketch-called** paths (`WiFi.loop()` when auto-service is off, or
a status()/connect() call), throttled to one attempt per 5 s as
`m2_lwip_test` does — **never from the yield pump**, so a 15 s scan can never
fire inside an unrelated `delay()`.

`hostByName` uses lwip DNS (`LWIP_DNS=1` already on; DHCP supplies the
server), pumping while the callback is pending.

lwip ownership: the façade owns `lwip_init()` + `netif_add` and defines the
`g_mac[6]` C-linkage array `Iw416Netif.cpp` externs (today each sketch
defines it; a façade sketch defines nothing).  Existing examples don't link
`arduino/`, so no duplicate-symbol risk.

## 6. Connection pool + WiFiClient + WiFiServer

**`WiFiClient` never owns a pcb.**  A fixed pool of 4 `WiFiConn` slots
(`MEMP_NUM_TCP_PCB` is 5; the spare is headroom for TIME_WAIT churn, which
rides that same pool — listeners draw from `MEMP_NUM_TCP_PCB_LISTEN`) holds:

    state (FREE/CONNECTING/ESTABLISHED/PEER_CLOSED/CLOSING), tcp_pcb*,
    RX pbuf chain head + read offset, unacked-read byte count,
    deferred-TX bookkeeping, refcount, lastActivityMs, claimedBySketch

`tcp_arg` points at the **slot**, never at a `WiFiClient`.  `WiFiClient` is a
refcounted handle (copy/assign/destruct manage the count), so
`WiFiServer::available()` returns by value with Arduino's shared-socket
semantics, and destructing any handle can never leave lwip holding a freed
pointer — the use-after-free class from the handoff is designed out
structurally.  A slot with refcount 0 in a terminal state returns to FREE.

Raw-API discipline (every rule from the handoff, made structural):

* One `closeConn(slot)` clears `tcp_arg/recv/sent/err/poll` **before**
  `tcp_close`; on close failure, `tcp_abort`.  It returns the `err_t` its
  caller must hand back to lwip — `ERR_ABRT` after the abort fallback —
  matching `m2_lwip_test.cpp`'s `closeEcho` contract.
* `tcp_err` fires with the pcb already freed: it resets slot state only,
  never calls `tcp_*`.
* RX: the recv callback appends to the slot's pbuf chain (`pbuf_cat`) and
  returns `ERR_OK`; `tcp_recved()` is called only as the sketch consumes
  bytes (`read`), so an unread client closes the TCP window and the peer
  stalls instead of pbufs piling up — correct NO_SYS flow control.  A cap on
  chain length (TCP_WND already bounds it) is asserted, not hoped.
  `pbuf_copy_partial` for all reads; chains are never assumed contiguous.
* TX: `tcp_write(..., TCP_WRITE_FLAG_COPY)` + `tcp_output`.  `ERR_MEM`
  defers: the pump retries from `pool.service()`; `write()` blocks pumping
  until accepted or a write timeout (default 5 s) expires, then returns the
  short count.  Bounded work per pass everywhere.
* `connect()` blocks pumping until the connected callback, `tcp_err`, or a
  timeout (default 10 s; on timeout, abort via the callbacks-still-attached
  path exactly like `tcpKick`'s 5 s abort).

`WiFiClient` surface (overrides of the clean-room `Client`):
`connect(ip,port)`, `connect(host,port)` (DNS first), `write(b)`,
`write(buf,len)`, `available()`, `read()`, `read(buf,len)`, `peek()`,
`flush()` (pump until lwip's send buffer drains or timeout), `stop()`
(graceful close, pump-drain with timeout, then abort), `connected()`
(true while ESTABLISHED or PEER_CLOSED-with-unread-data, the Arduino
convention), `operator bool()` (slot attached).

`WiFiServer` (clean-room `Server`): `begin()` = `tcp_new` + `tcp_bind` +
`tcp_listen` + `tcp_accept` into the pool (accepted slots marked
unclaimed); `available()` returns a client with data pending (claiming it);
`accept()` returns any unclaimed accepted client; `write()` broadcasts to
every ESTABLISHED pool connection owned by this server.  `end()` closes the
listener.  Accept backpressure: when the pool is full the accept callback
refuses (`ERR_MEM`).

**Stall safety valve** (handoff requires one; a peer that vanishes without
FIN/RST must not wedge the pool):

* Every pool slot gets `tcp_poll` at ~10 s cadence; a slot CLOSING longer
  than the drain timeout is aborted.
* When `accept` fires with the pool full **or** all slots stale: evict the
  least-recently-active **accepted-but-never-claimed** slot (the sketch
  never saw it, so nothing it holds dangles — refcount 0 by definition).
  Claimed connections are never evicted; an idle-but-held session is the
  sketch's business.

## 7. Examples + gates

Two examples, three gates.  Sweep baseline moves **102 -> 105**.
`wifi_client_test` owns two `run_qemu*.sh`, so its ids take the `[variant]`
suffix per the W14 rule; `wifi_server_test` owns one.

| gate id | QEMU config | asserts |
|---|---|---|
| `rt1176:networking/wifi_client_test` | stock (no model) | Card-absent: `WiFi.begin()` returns `WL_NO_SHIELD`, prints the fallback token, never claims an IP, heartbeat reaches `alive=2`.  Passes on stock QEMU like every plain m2_* gate. |
| `rt1176:networking/wifi_client_test[wifi]` | `-machine m2-wifi=on -global iw416-sdio.fw-preboot=on` | Real enumeration (CMD5 answered, fn1 up — the model's values, not the firmware's wishes), a **real scan** issued through `WiFi.begin()`, model returns zero BSS by design, façade reports `WL_NO_SSID_AVAIL` honestly, **and the heartbeat keeps ticking through and after the failed connect** — the pump survived a blocking begin(). |
| `rt1176:networking/wifi_server_test` | stock | Card-absent fallback; `server.begin()` with no link does not wedge; heartbeat. |

The `[wifi]` gate header must state what it does NOT prove, per the
`run_qemu_wifi.sh` convention: **no association is possible** (the model
deliberately returns zero scan results), so no DHCP, no TCP, no
WiFiClient/WiFiServer data path, no accept path is exercised in QEMU at all.
All of that is silicon-only.  Requires qemu2 `>= 2ed9314631`
(gitlab.com/Newdigate/qemu-rt1170); on stock QEMU it goes RED, not SKIP —
already-documented behaviour for the model-dependent gate class.

Both examples: rt1176-only (no `boards` sidecar), console `Serial1`,
gate scripts follow `gate-lib.sh` (`gate_init`, `gate_qemu_machine`,
`gate_console`, `gate_reap`, `gate_require_capture`), `./run_qemu.sh` direct
execution, never `sh run_qemu.sh`.

Silicon (two transcripts, un-fakeable assertions):

* `wifi_client_test` — ESP8266 bench AP `ESP8266TEST` (its own throwaway
  PSK): `WiFi.begin` -> DHCP address printed; `WiFiClient` echo against the
  ESP oracle `192.168.4.1:4712`, byte-exact `WIFI hello <n>` echoes counted
  `tx/ok/fail`; `hostByName` exercised if the bench AP offers DNS, skipped
  with a printed token if not.
* `wifi_server_test` — board runs `WiFiServer` on :5010; Mac-side
  `wifi_peer.py` (stdlib-only, modelled on `tput_peer.py`, the authoritative
  side) connects N times, sends known payloads, requires byte-exact responses
  and clean closes; one test holds a second connection open while the first
  transfers (pool concurrency); one fills the pool with connections it opens
  and never claims data on, then requires a further connect to still succeed
  — the board printing its unclaimed-eviction token proves the §6 safety
  valve on silicon.  (A true vanish-without-FIN needs a peer the bench can't
  fake from Python; the CLOSING-drain abort is asserted instead by a peer
  that stops reading mid-`stop()`.)

Bench discipline (handoff): the board is shared with a soak-testing session —
**semaphore with the user before flashing**.  Never `rm -rf`/`cmake -B` an
existing example build dir.  Flash VCOM-free, attach
`tools/rt1170-console.py` after.

## 8. Close-out

1. All M2Radio work committed on its master; **push to
   github.com/newdigate/M2Radio**.
2. Bump the M2Radio pin in `evkb.cmake` (currently `1e15f0b...`).
3. Verify fresh-user mode: configure one wifi example with
   `-DEVKB_FORCE_FETCH=ON` and build — a fresh-clone compile break presents
   as SKIP in the sweep and hides, so it is checked directly.
4. `GATES` entries for both examples in `tools/license-audit.sh`; audit
   green.
5. Full sweep from `/tmp/ev`: **105 passed, 0 failed, 0 SKIP** (or 104/1
   with the known nondeterministic `cm4_audio_test` red, re-run idle).
   Update CLAUDE.md's baseline paragraph and `docs/KNOWN-BROKEN-GATES.md`
   as the conventions require (the five-model-gate note grows to seven).
6. M2Radio README section for `arduino/`: the PS rule, the pump model, the
   Ethernet-coexistence limitation, the base-class provenance.

## 9. Out of scope (phase 2+)

`WiFiUDP` (needs the `Udp.h` provenance survey first), soft-AP, WPA3, scan
enumeration surface (`WiFi.scanNetworks()`), TLS, mDNS, auto-reconnect
default-on, `WiFiClientSecure`.  None of these blocks the minimum surface.

## Risks / open eyes

* The yield pump touches every sketch globally once `begin()` runs; the
  `m_inDriverCmd` guard is the load-bearing safety and gets a dedicated
  review point.  If an unforeseen interaction appears on silicon,
  `setAutoService(false)` is the immediate fallback and the examples still
  pass with an explicit `WiFi.loop()`.
* QEMU cannot cover the data path for this layer at all — the silicon
  transcripts ARE the evidence for everything past the scan.  Stated in gate
  headers.
* The eviction policy (unclaimed-only) means a sketch that holds 4 claimed
  dead connections can still starve accepts; `tcp_poll` reaping bounds this
  at the drain timeout.  Documented in WiFiServer's header.
