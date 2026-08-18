# Continue M.2 Wi-Fi bring-up (W6: associate to an open AP)

u-blox **M2-MAYA-W161** (NXP **IW416**/SD8978) on **MIMXRT1170-EVKB RevC3**,
repo `~/Development/rt1176-evkb-m2-maya-w161`, branch **`m2-phase0-serial2`**.
Driver in sibling `~/Development/M2Radio` (master, pushed, pinned in
`evkb.cmake` at `14c93e3`).

**Read first:** `examples/networking/m2_sdio_probe/transcript_hw_evkb.txt`
(W3–W5) and `docs/superpowers/specs/2026-08-18-m2-w5-monitor-design.md`.

## Where things stand

| Phase | State |
|---|---|
| W1 — SDIO enumerate | ✅ `manfid=0x2DF cardid=0x9158` |
| W2 — firmware download | ✅ `fw_status=0xFEDC` |
| W3 — host commands | ✅ `hw_spec=ok mac=6C:1D:EB:91:0C:45` |
| W4 — scan | ✅ `scan=ok`, real SSID "OnestreamQJN7" |
| W5 — data-path RX (monitor) | ✅ `frames=16`, live beacon + Block Ack off the air |
| W6 — associate + bidirectional data | not started |

## What W5 built that W6 reuses

* `readDataPacket()` is the reusable data-port RX (UP_LD bit 0 → RD_BITMAP →
  RD_LEN_P\<n\> → CMD53-read at `ioport|n`). W6's RX side is done.
* SDIOPkt data framing + RxPD parse are proven (`rx_pkt_type`, `rx_pkt_offset`,
  snr/nf). Associated data frames are `PKT_TYPE_..._ETHERNET`, not 802DOT11 —
  check `mlan_fw.h` for the value and expect an 802.3/LLC payload, not a raw
  802.11 frame.
* `mon_dbg` counters make a frames=0 self-diagnosing — keep that pattern for
  the TX side (a WR_BITMAP that never frees a port is the TX analogue).

## W6: association (needs an OPEN AP the user sets up)

This is the step the W5 design deferred because it needs an external AP the
absent user must name. Sequence (NXP `wlan_cmd_802_11_associate`,
`mlan_sta_cmd.c`):

1. SCAN for the target SSID (W4 already does this) → capture its BSSID,
   channel, capability, and rate/RSN IEs from the scan entry.
2. **AUTHENTICATE** is folded into ASSOCIATE for open networks on this
   firmware (open-system auth); confirm whether a separate 0x0011 is needed.
3. **ASSOCIATE (0x0012)** — build from the scanned BSS: SSID TLV, PHY/rate
   TLVs, channel TLV, capability. `resp_result`/the assoc-result IE names a
   reject.
4. On success: **MAC_CONTROL** already on; data frames flow on the data ports.
   TX: check WR_BITMAP (0x14–0x17), CMD53-write MLAN_TYPE_DATA to `ioport|port`
   with a TxPD header. A DHCP DISCOVER or a broadcast ARP is a good first TX;
   the reply (or an ARP for our IP) captured via `readDataPacket` is the
   un-fakeable bidirectional proof.

**Scope carefully.** WPA2 is a large step (4-way handshake, EAPOL, PMK/PTK).
Do OPEN first: a phone hotspot set to "open"/no-password, or a spare router
with an open SSID. Get association + a DHCP round-trip on open before touching
WPA2. Confirm with the user which SSID to target and that it is open.

## Unchanged constraints

* Blob via `-DM2RADIO_IW416_FW=...`; J15 empty; VCOM free while programming.
* QEMU gate asserts the module-absent path; keep it green unmodified.
* After M2Radio changes: push, then bump the `evkb.cmake` pin.
