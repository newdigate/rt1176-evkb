# Continue M.2 Wi-Fi bring-up (W4: data path / first scan)

u-blox **M2-MAYA-W161** (NXP **IW416**/SD8978) on **MIMXRT1170-EVKB RevC3**,
repo `~/Development/rt1176-evkb-m2-maya-w161`, branch **`m2-phase0-serial2`**.
Driver in sibling `~/Development/M2Radio` (master, pushed, pinned in
`evkb.cmake` at `09f77d8`).

**Read first:** `docs/m2-evkb-revc3.md` and
`examples/networking/m2_sdio_probe/transcript_hw_evkb.txt` — the W3 section at
the end records the working command interface AND the four bugs it took.

## Where things stand

| Phase | State |
|---|---|
| W1 — SDIO enumerate | ✅ `manfid=0x2DF cardid=0x9158` |
| W2 — firmware download | ✅ `fw_download=ok fw_status=0xFEDC` (now validated on every exit path) |
| W3 — host commands | ✅ `hw_spec=ok mac=6C:1D:EB:91:0C:45 fw_release=0x9B105C15 hw_version=0x7201` |
| W4 — data path / scan | not started |

## What W3 settled — do not relearn

* The SD8978 **"new mode" init block is mandatory** and lives in
  `Iw416::begin()` (six writes, mirroring NXP `wlan_sdio_init_ioport()`).
* `HOST_INT_STATUS` (0x0C) is **clear-on-READ** (RSR=0xFF); never write it.
* Command replies publish length at **CMD_RD_LEN_0/1 (0xC0/0xC1)**, flagged by
  **CMD_PORT_UPLD (bit 6)**. `RD_LEN_P0` (0x18/0x19) is data-port only.
* `HIM_ENABLE` (0xC3) only **after** FIRMWARE_READY (`enableHostInt()`).
* **Never send a command before FIRMWARE_READY** — it kills the booting
  firmware (fw_status settles at 0xF00B, no interrupt ever fires again).
* **Match responses by `cmd | 0x8000`** (`waitCmdResp()`); the port also
  carries events, and the first packet is not necessarily the answer.

## W4 candidates, in rough order

1. **SCAN** (`HostCmd_CMD_802_11_SCAN` 0x0006) over the same command port —
   pure command traffic, no data path needed, and an SSID list is un-fakeable.
2. **Data path**: TX/RX over the DATA ports (WR_BITMAP/RD_BITMAP at 0x10–0x17,
   `RD_LEN_P0` finally earns its keep). Reference:
   `wifi-sdio.c` `wlan_process_int_status()` data-port half.
3. **Bluetooth**: `serial2_rx: total=10 last=0x47` — since R1901 was bridged
   the card's BT UART sends real bytes in response to HCI Reset. A proper HCI
   read on LPUART2 is now possible (remember: **no flow control** — RTS is the
   Ethernet PHY's reset).

## Unchanged constraints

* Blob at configure time via `-DM2RADIO_IW416_FW=...` (never commit it).
* J15 microSD slot stays EMPTY. Do not hold the VCOM while programming.
* QEMU gate asserts the module-absent path; never weaken it.
* After M2Radio changes: push, then bump the pin in `evkb.cmake` — an
  unpushed pin is the SynthUI SKIP-class failure.
