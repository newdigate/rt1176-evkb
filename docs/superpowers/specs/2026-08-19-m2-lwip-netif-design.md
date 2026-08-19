# W9: lwip over the IW416 M.2 Wi-Fi link — design

Date: 2026-08-19.  Approved by the user in-session.
Context: the link is silicon-proven end to end (W8: WPA2 handshake 13 ms,
DHCP in 2 sends, encrypted pings, both SDIO rings healthy — see
`examples/networking/m2_sdio_probe/transcript_hw_evkb.txt` W8 RESULT).  This
design promotes it from a hand-rolled probe netlite to a real stack.

## Decision

Bridge the existing **lwip sibling** (pinned in `evkb.cmake`, BSD-3, already
gated via `examples/networking/lwip_test`) to the M2Radio IW416 driver with a
thin netif, rather than growing the probe's UDP-only netlite (rejected:
permanent bespoke-stack maintenance, no TCP/DNS) or porting a new stack
(rejected: lwip is already integrated, `NO_SYS=1`, DHCP+TCP enabled in
`port/lwipopts.h`).

## Components

### 1. `M2Radio/lwip/Iw416Netif.{h,cpp}` — the netif glue

The Wi-Fi sibling of the lwip repo's `port/ethernetif.c`, living in M2Radio
(it knows the driver; the lwip repo stays ENET-only and UNTOUCHED).

* `err_t iw416NetifInit(struct netif *)` — name `wl`, MTU 1500,
  `NETIF_FLAG_BROADCAST | ETHARP | LINK_UP` wiring, `linkoutput` =
  `low_level_output`, hwaddr from `Iw416::getHwSpec`'s MAC.  The `Iw416 *`
  handle rides in `netif->state`.
* `low_level_output`: `pbuf_copy_partial` the chain into a static frame
  buffer, `sendDataFrame()`.  lwip err codes: driver `OK` → `ERR_OK`,
  anything else → `ERR_IF` (lwip retries/timers handle the rest).
* `bool iw416NetifPoll(struct netif *)` — one service pass: drain
  `pollLink()` frames (each → `pbuf_alloc(PBUF_RAW, len, PBUF_POOL)`,
  copy, `netif->input`), map a link drop to `netif_set_link_down` and
  return false so the app can re-connect.  No RTOS, no IRQs: called from
  `loop()` alongside `sys_check_timeouts()`, exactly the `lwip_test`
  pattern.

### 2. `Iw416::connectStation(ssid, psk)` — driver convenience

The probe's proven bring-up promoted into the driver as one call:
scan → find SSID (loose match, exact scanned bytes for the PBKDF2 salt) →
`setPassphrase` → `associate` → `watchConnect`, with a bounded retry.
Open networks (empty psk) skip the passphrase step.  **No monitor phase** —
the NET_MONITOR-before-associate firmware erratum (W8) stands.

### 3. `examples/networking/m2_lwip_test/` — the example + gates

Boot: SDIO begin → fw download → init parity commands → `connectStation`
→ `netif_add` + `netif_set_default` + `dhcp_start` → main loop
(`iw416NetifPoll` + `sys_check_timeouts` + app).

App / hardware proof: a TCP echo client.  Every 2 s: connect to
`192.168.4.1:4712` (the ESP oracle), send `"M2LWIP hello <n>"`, verify the
echo BYTE-EXACT, close.  Console line once per ~3 s:
`lwip: ip=<dhcp ip> tcp=<attempts>/<echoes>/<ok|state> ring=T/R/n rx=<n>`.
DHCP comes from lwip's own client — the probe's netlite is not used here
(the probe example stays as-is, low-level evidence harness).

ESP oracle: the session sketch (`esp_ap/esp_ap.ino`) grows a
`WiFiServer(4712)` accept-echo-close loop beside the existing 1 Hz UDP
broadcast and station logging.  Same rig, still un-fakeable from the EVKB.

Gates (two-gate rule):
* QEMU (`run_qemu.sh`): no SDIO card model, so assert the no-IO-function
  fallback token plus the heartbeat, same shape as `m2_sdio_probe`'s gate.
  Sweep goes **95 → 96**; update the counts in `CLAUDE.md` per its rules.
* Hardware: `transcript_hw_evkb.txt` with lwip DHCP bind + N byte-verified
  TCP echoes, ESP log on the same timeline.
* Licence audit: lwip is BSD-3 and already in the GATES walk via
  `lwip_test`; the new example adds a normal GATES entry.

## Error handling

* Link drop (deauth/disassoc/LINK_LOST from `pollLink`) →
  `netif_set_link_down` + `dhcp_stop`; app re-runs `connectStation`, then
  `netif_set_link_up` + `dhcp_start`.  (lwip 2.x handles re-binding.)
* TCP connect/echo failure → count and print; next 2 s cycle retries.
  No panic paths: the example is a soak instrument as much as a demo.
* Oversized RX (> pbuf) → dropped by the glue, counted.

## Out of scope (tracked separately)

* The overnight firmware RX-death (soak + wifi_cli A/B running in this
  session; the example's `ring=`/`rx=` fields exist to correlate with it).
* Interrupt-driven SDIO service (DAT1) — future; polling matches lwip_test.
* Throughput tuning (TCP_MSS/window against 2048-byte TX buffers) — accept
  `port/lwipopts.h` defaults first; tune only if the echo proof needs it.
