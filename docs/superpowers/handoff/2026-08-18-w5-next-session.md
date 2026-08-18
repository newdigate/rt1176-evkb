# Continue M.2 Wi-Fi bring-up (W5: data path)

u-blox **M2-MAYA-W161** (NXP **IW416**/SD8978) on **MIMXRT1170-EVKB RevC3**,
repo `~/Development/rt1176-evkb-m2-maya-w161`, branch **`m2-phase0-serial2`**.
Driver in sibling `~/Development/M2Radio` (master, pushed, pinned in
`evkb.cmake` at `146884b`).

**Read first:** `examples/networking/m2_sdio_probe/transcript_hw_evkb.txt`
(W3 + W4 sections) and `docs/superpowers/specs/2026-08-18-m2-w4-scan-design.md`.

## Where things stand

| Phase | State |
|---|---|
| W1 — SDIO enumerate | ✅ `manfid=0x2DF cardid=0x9158` |
| W2 — firmware download | ✅ `fw_status=0xFEDC`, READY-validated on every exit path |
| W3 — host commands | ✅ `hw_spec=ok mac=6C:1D:EB:91:0C:45` |
| W4 — first scan | ✅ `scan=ok num=3`, real SSID "OnestreamQJN7" ch 4 @ -74 |
| W5 — data path | not started |

## W4 facts worth keeping

* Command sequence that works, first try: FUNC_INIT → GET_HW_SPEC →
  MAC_CONTROL(0x0028, action 0x0013) → 802_11_SCAN (legacy 0x0006, ChanList
  TLV ch 1..13 active 100 ms).
* Scan response bounds come from the **SDIOPkt size field**, not CMD_RD_LEN —
  the CMD53 read is block-padded and the tail of the last block is garbage.
* `readHostResp`/`waitCmdResp` take `timeoutMs`; scan uses 15 s.
* The probe's `resp_*`/`cmd_rd_len` fields show the LAST command-port packet
  (after W4 that is the scan response), not a per-command record.

Everything W3 settled still applies (new-mode init block, clear-on-READ
status, CMD_RD_LEN for lengths, HIM_ENABLE only after READY, never command a
booting firmware, match responses by `cmd | 0x8000`).

## W5: the data path

Goal: TX/RX real frames over the DATA ports. Reference:
`wifi-sdio.c` `wlan_process_int_status()` (data half) and `wlan_send_sdio_packet`.

* RX: on `UP_LD_HOST_INT_STATUS` (bit 0), read RD_BITMAP (0x10–0x13), pick a
  port, length from `RD_LEN_P<n>` (0x18 + 2n for port n), CMD53-read at
  `ioport | port`. Packets carry the same SDIOPkt header (pkttype 0 = data,
  2 = ... check mlan defs; events use the CMD port on this family).
* TX: check WR_BITMAP (0x14–0x17), CMD53-write to `ioport | port`,
  MLAN_TYPE_DATA framing.
* A credible W5 proof without association: RX side only — after MAC_CONTROL
  the firmware may deliver nothing until associated, so the honest first
  milestone is association (0x0012 ASSOCIATE after AUTHENTICATE) or at
  minimum a monitor/promiscuous path. Scope W5 carefully before coding:
  association pulls in auth, rates, and (for any real network) WPA2, which
  is a large step — an OPEN test AP on a phone hotspot keeps it tractable.

## Unchanged constraints

* Blob via `-DM2RADIO_IW416_FW=...`; J15 empty; VCOM free while programming.
* QEMU gate asserts the module-absent path; keep it green unmodified.
* After M2Radio changes: push, then bump the `evkb.cmake` pin.
