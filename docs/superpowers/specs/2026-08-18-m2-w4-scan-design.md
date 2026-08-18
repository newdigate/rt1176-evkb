# W4: first 802.11 scan over the IW416 command port — design

Continues the M.2 bring-up from `docs/superpowers/handoff/2026-08-18-w4-next-session.md`
(W3 complete: command port proven, `GET_HW_SPEC` returns the card's MAC).
Everything here rides the transport W3 verified; no data-path work.

## Goal and success criterion

Send `HostCmd_CMD_802_11_SCAN` (0x0006) and print the APs the card hears.
**Success is a list of real nearby SSIDs on the console** — strings that exist
nowhere in the firmware image, the same un-fakeable standard as W1's CIS ids
and W3's MAC.

## Approaches considered

1. **Legacy SCAN (0x0006), single command — CHOSEN.** One request, one
   response on the already-working command port. The response carries BSSID,
   RSSI and the beacon IEs inline. Smallest possible step from W3.
2. EXT_SCAN (0x0107): what NXP's current config uses, but results return as
   EVENT packets interleaved with data traffic — it wants the data path (W5)
   first. More machinery, no more proof.
3. Data path first, scan later: biggest step, and the scan would still be
   needed to prove RF works. Deferred; the scan is the cheaper first light.

## Command sequence

From NXP's `wlan_fw_init_cfg()` (wifi-sdio.c): after FUNC_INIT and
GET_HW_SPEC, the first RF-relevant command is `MAC_CONTROL` (0x0028). So:

    FUNC_INIT (0x00A9)          -- already sent by getHwSpec()
    GET_HW_SPEC (0x0003)        -- already sent; MAC arrives here
    MAC_CONTROL (0x0028)        -- body: u32 action = 0x0013
                                   (RX_ON 0x1 | TX_ON 0x2 | ETHERNETII 0x10)
    802_11_SCAN (0x0006)

If SCAN fails without further init, the `resp_result` instrumentation from W3
names the failure; add commands one at a time from `wlan_fw_init_cfg()`'s
order, never several at once.

## Scan request (mlan_fw.h layouts, all little-endian)

    HostCmd_DS_802_11_SCAN:
      u8  bss_mode        = 3   (HostCmd_BSS_MODE_ANY)
      u8  bssid[6]        = 0
      TLV ChanList (type 0x0101 = PROPRIETARY_TLV_BASE_ID+1):
        13 x ChanScanParamSet_t {
          u8  radio_type   = 0        (2.4 GHz)
          u8  chan_number  = 1..13
          u8  chan_scan_mode = 0      (active scan; all flag bits clear)
          u16 min_scan_time = 100
          u16 max_scan_time = 100     (MRVDRV_ACTIVE_SCAN_CHAN_TIME)
        }                              -> TLV len 13*7 = 91 bytes

Total body 7 + 4 + 91 = 102 bytes; fits one 256-byte block.

## Scan response (legacy format)

    body: u16 bss_descript_size, u8 number_of_sets, then per set:
      u16 beacon_size
      6   BSSID
      1   RSSI            (byte; RSSI in dBm = -value)
      8   timestamp
      2   beacon interval
      2   capability
      IEs (id,len,data): SSID = id 0, DS Param (channel) = id 3

Parsed straight off `wlan_interpret_bss_desc_with_ie()` (mlan_scan.c:1749ff).

## Driver additions (M2Radio Iw416)

* `macControl(uint32_t action)` — send + `waitCmdResp`.
* `struct ScanResult { uint8_t bssid[6]; uint8_t rssi; uint8_t channel; char ssid[33]; }`
* `scan(ScanResult *out, uint8_t maxOut, uint8_t *outCount)` — builds the
  request above, waits for the response with a **15 s** deadline (13 channels
  x 100 ms dwell plus firmware overhead exceeds the default 2 s), parses into
  `out`, keeps `number_of_sets` even when it exceeds `maxOut`.
* `readHostResp()`/`waitCmdResp()` grow a `timeoutMs` parameter
  (default 2000 — existing callers unchanged).
* Scan reply buffer: static 4 KB (a busy bench can exceed the 1 KB command
  buffer; `CMD_RD_LEN` publishes the true length and oversize reports
  BAD_CIS with the length as evidence rather than truncating silently).

## Probe report (m2_sdio_probe)

After `hw_spec=ok`:

    mac_ctrl=<status>
    scan=<status> num=<sets> (showing <=8)
    scan_ap0: bssid=XX:.. rssi=-NN ch=N ssid="..."

All under `#if HAVE_IW416_FW` (the no-blob build has no firmware to scan
with; W3 already fixed that guard once — keep it fixed).

## What does not change

* QEMU gate: module-absent path unreachable by any of this (scan only runs
  after `fw_download=ok`). Gate must stay green unmodified.
* Both repos' licence posture: NXP source remains a protocol reference;
  nothing vendored.
* After the M2Radio push, `evkb.cmake`'s pin is bumped in the same session —
  an unpushed pin is the SynthUI SKIP-class failure.

## Risks

* Scan may need more init than MAC_CONTROL — mitigated by one-command-at-a-
  time iteration with `resp_result`/`int_seen` evidence.
* A response larger than 4 KB on a dense RF bench — reported, not truncated;
  raise the buffer only if actually hit.
* RSSI sign convention differs across firmwares; the raw byte is printed
  alongside the interpreted dBm so the transcript preserves the evidence.
