# W5: data-path RX via monitor mode — design

Continues the M.2 bring-up from `docs/superpowers/handoff/2026-08-18-w5-next-session.md`.
W1–W4 pass in a single boot (enumerate, firmware, host commands, scan).

## Scope decision — why monitor mode, not association

The handoff names three W5 paths and warns to scope carefully:

1. Full associated data path — needs AUTHENTICATE + ASSOCIATE + rate config +
   (for any real AP) WPA2. Large, and needs an AP the absent user must name.
2. RX-only after MAC_CONTROL — the firmware delivers nothing until associated,
   so there is nothing to receive.
3. **Monitor/promiscuous (`NET_MONITOR` 0x0102) — CHOSEN.** RX-only, no
   association, needs nothing external (it captures traffic already in the
   air), and it produces an un-fakeable result: real 802.11 frames whose
   BSSID/SSID match the "OnestreamQJN7" AP W4 already found. It also builds
   exactly the reusable data-port RX machinery
   (RD_BITMAP → RD_LEN_P\<n\> → CMD53-read → SDIOPkt/RxPD parse) the eventual
   associated path needs. Running autonomously, it is also the only W5
   milestone reachable with no user input.

Association is deferred to W6, where an OPEN test AP can be set up
deliberately.

## Success criterion

**Decoded 802.11 frames on the console captured live in monitor mode** —
each with frame-control, transmitter MAC, RSSI, and (for beacons) BSSID and
SSID. A beacon whose BSSID is `78:20:51:8F:19:5E` / SSID "OnestreamQJN7"
(what W4's scan reported) appearing in a raw capture is the un-fakeable proof.
If zero frames arrive in the window, that is reported as `frames=0`, not
dressed up as success.

## Firmware-feature risk, stated up front

`NET_MONITOR` is under `#if CONFIG_NET_MONITOR` in NXP's tree — it is a
firmware capability, and the `sduartIW416_wlan_bt.bin` blob may or may not
carry it. The `resp_result` from `waitCmdResp` is the oracle: a non-zero
result means the running firmware rejected the command, reported as
`monitor=cmd-fail result=0xNN` and the phase stops cleanly. No hang, no
guessing — the same discipline W3 established.

## Command: NET_MONITOR (0x0102), from mlan_misc.c `wlan_cmd_802_11_net_monitor`

Body (all LE), `HostCmd_DS_802_11_NET_MONITOR`:

    u16 action            = 1 (HostCmd_ACT_GEN_SET)
    u16 monitor_activity  = 1 (enable)      / 0 (disable)
    u16 filter_flags      = 0x07 (mgmt|ctrl|data — NXP's own example uses 7)
    ChanBandList TLV:
      u16 type  = TLV_TYPE_UAP_CHAN_BAND_CONFIG (0x012A)
      u16 len   = 2
      u8  radio_type  = 0 (2.4 GHz)
      u8  chan_number = 4        (where W4 heard the AP)
    Monitor filter TLV:
      u16 type  = TLV_TYPE_UAP_STA_MAC_ADDR_FILTER (0x0138)
      u16 len   = 1  (filter_num only; no MAC entries)
      u8  filter_num = 0

Reference caller values: `wlan-set-monitor-param 1 1 7 0 <chan>`.

## Data-port RX (reusable), from wifi-sdio.c `wlan_get_rd_port` / `wlan_read_rcv_packet`

* Poll `HOST_INT_STATUS` (0x0C, clear-on-READ) for `UP_LD` (bit 0) — the DATA
  port's upload flag, distinct from the command port's bit 6.
* Read `RD_BITMAP_L/U` (0x10/0x11) → 16-bit read bitmap (ports 0..15 for this
  part's low word; the probe only needs the low word).
* For the lowest set port bit `n`: length at `RD_LEN_P0 + (n<<1)` = 0x18 + 2n
  (L) / 0x19 + 2n (U), rounded up to whole 256-byte blocks.
* CMD53-read `blocks` at `ioport | n` (no CMD_PORT_SLCT — that selects the
  command port).

## Buffer layout of a received data packet

    [u16 sdio_size][u16 pkttype=0 MLAN_TYPE_DATA]     <- INTF_HEADER_LEN = 4
    RxPD @ offset 4:
       u8  bss_type; u8 bss_num;
       u16 rx_pkt_length; u16 rx_pkt_offset; u16 rx_pkt_type;
       u16 seq_num; u8 priority; u8 rx_rate; s8 snr; s8 nf; ...
    802.11 frame @ (4 + rx_pkt_offset), length rx_pkt_length

Monitor frames carry `rx_pkt_type == PKT_TYPE_802DOT11 (0x05)`.
RSSI (dBm) = `snr - nf` (NXP's `user_recv_monitor_data`). Invariant checked:
`4 + rx_pkt_offset + rx_pkt_length == sdio_size`.

## Driver additions (M2Radio Iw416)

* `netMonitor(bool enable, uint8_t channel)` — build + send NET_MONITOR,
  `waitCmdResp` matching `0x0102 | 0x8000`, surface `resp_result`.
* `struct MonitorFrame { uint16_t frameControl; uint8_t rssi; uint8_t channel;
  uint8_t ta[6]; uint8_t bssid[6]; char ssid[33]; uint16_t len; }`
* `readDataPacket(uint8_t *buf, uint16_t bufLen, uint16_t *outLen, uint8_t *port,
  uint16_t *rxType, uint32_t timeoutMs)` — the reusable RD_BITMAP/RD_LEN/CMD53
  read; `rxType` is the RxPD `rx_pkt_type`.
* `captureMonitor(MonitorFrame *out, uint8_t maxOut, uint8_t *count,
  uint32_t windowMs, uint8_t channel)` — enable monitor, poll data-port
  uploads for the window, parse each 802.11 frame (frame_control, addr2→ta;
  for beacon/probe-resp subtypes: addr3→bssid and SSID IE 0), disable monitor
  before returning. `framesSeen()` = total captured, even past `maxOut`.

Parsing bounds every read by the SDIOPkt size field, as W4's scan does — the
CMD53 read is block-padded and the last block's tail is garbage.

## Probe report (m2_sdio_probe), after the W4 scan block

    monitor=<status> result=0xNN frames=<n> (showing <=6)
    mon_fr0: fc=0x80 ta=XX:.. rssi=-NN ch=N bssid=XX:.. ssid="..."

All under `#if HAVE_IW416_FW`. `fc=0x80` is a beacon; management/data/control
subtypes all print their raw frame_control so the capture is legible even for
frames the parser does not decode further.

## What does not change

* QEMU gate: monitor only runs after `fw_download=ok`, unreachable by QEMU's
  module-absent path. Gate stays green unmodified.
* Licence posture: NXP source is a protocol reference; nothing vendored.
* After the M2Radio push, bump the `evkb.cmake` pin in the same session.

## Risks

* Firmware without CONFIG_NET_MONITOR — reported via `result`, not a hang.
* A quiet RF window (few beacons) — capture window is 3 s (a 2.4 GHz AP
  beacons ~10x/s, so channel 4 should yield several); `frames=0` is reported
  honestly and the window can be widened.
* Frame larger than the 2 KB data buffer — reported (BAD_CIS with the length),
  not truncated silently.
