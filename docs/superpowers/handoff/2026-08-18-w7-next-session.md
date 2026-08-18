# Continue M.2 Wi-Fi bring-up (W7: finish the WPA2 handshake + data)

u-blox **M2-MAYA-W161** (NXP **IW416**/SD8978) on **MIMXRT1170-EVKB RevC3**,
repo `~/Development/rt1176-evkb-m2-maya-w161`, branch **`m2-phase0-serial2`**.
Driver in sibling `~/Development/M2Radio` (master, pushed, pinned in
`evkb.cmake` at `3932272`).

**Read first:** `examples/networking/m2_sdio_probe/transcript_hw_evkb.txt`
(W6 section) and `docs/superpowers/specs/2026-08-18-m2-w6-associate-design.md`.

## Where things stand

| Phase | State |
|---|---|
| W1 — SDIO enumerate | ✅ |
| W2 — firmware download | ✅ |
| W3 — host commands | ✅ |
| W4 — scan (+ W6 security survey) | ✅ |
| W5 — data-path RX (monitor) | ✅ |
| W6 — WPA2 association | ✅ associates cleanly; ⚠️ 4-way handshake needs the correct PSK |

## The one open item: the WPA2 passphrase

W6 associates cleanly (`assoc_status=0`, real `cap_info=0x1411`) but the 4-way
handshake fails with **EVENT_DEAUTHENTICATED reason 15 (handshake timeout)**.
The RSN IE proves the AP is plain WPA2-PSK/CCMP with no PMF/SHA256, so the
association code is correct — reason 15 means the **PMK is wrong**, i.e. the
supplied passphrase (`-DM2RADIO_WIFI_PSK`) does not match OnestreamQJN7's key.

**First action:** confirm the correct WiFi password with the user, reconfigure
with it (`-DM2RADIO_WIFI_PSK="..."` from
`examples/networking/m2_sdio_probe/`), reflash, and look for
`connect=ok last_event=0x2b` (EVENT_PORT_RELEASE) instead of the reason-15
deauth. Nothing in the code should need to change for this — it is purely the
credential. (Small residual code risk if the correct password STILL fails
identically: verify the Passphrase TLV in `setPassphrase()` delivers the exact
bytes — but the balance of evidence is a wrong password value.)

## Then W7: data TX + DHCP (the bidirectional proof)

Once `connect=ok`:

* TX is the remaining primitive. TxPD layout is in `mlan_fw.h` (`_TxPD`):
  bss_type(1) bss_num(1) tx_pkt_length(2) tx_pkt_offset(2) tx_pkt_type(2)
  tx_control(4) priority(1) flags(1) ... then the 802.3 frame at
  tx_pkt_offset. Poll WR_BITMAP (0x14–0x17) for a free port, CMD53-write an
  MLAN_TYPE_DATA packet to `ioport|port`.
* RX is done: `readDataPacket()` (associated data frames are ethernet-typed,
  an 802.3/LLC payload behind the RxPD — not PKT_TYPE_802DOT11).
* Un-fakeable proof: a DHCP DISCOVER out, DHCP OFFER back (an IP from the AP),
  or a broadcast ARP and its reply, captured via `readDataPacket`.

## What is already built and reusable

* `setPassphrase()`, `associate()`, `deauthenticate()`, `waitForConnect()`
  (handshake outcome via EVENT_PORT_RELEASE / deauth reason).
* `readDataPacket()` — the data-port RX (from W5).
* Scan captures BSSID, channel, capability, RSN IE, rates, security.
* Credential plumbing: `-DM2RADIO_WIFI_SSID/PSK` → gitignored `wifi_creds.h`.

## Traps paid for in W6 (do not rediscover)

* Rapid reflash cycles trip the AP's deauth-flood protection → association
  returns `cap_info=0xFFFx` (internal/timeout). Rest the AP; it is not a code
  regression. `cap_info` disambiguates internal-error/timeout from a real AP
  status.
* `assoc_status=0` does NOT prove the PSK — open-system 802.11 auth associates
  before the handshake. Only EVENT_PORT_RELEASE proves the key.

## Unchanged constraints

* Blob + PSK via configure-time flags, never committed; J15 empty; VCOM free
  while programming. QEMU gate asserts module-absent; keep it green. After
  M2Radio changes: push, then bump the `evkb.cmake` pin.
