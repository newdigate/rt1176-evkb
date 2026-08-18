# Continue M.2 Wi-Fi bring-up (W8: fix the RADIO, then the data path lights up)

u-blox **M2-MAYA-W161** (NXP **IW416**/SD8978) on **MIMXRT1170-EVKB RevC3**,
repo `~/Development/rt1176-evkb-m2-maya-w161`, branch **`m2-phase0-serial2`**.
Driver in sibling `~/Development/M2Radio` (master, pushed, pinned in
`evkb.cmake` at **`17fc353`**).

**Read first:** `examples/networking/m2_sdio_probe/transcript_hw_evkb.txt` —
the **"W7 RESULT"** section at the very end. It corrects W6.

## The one-line status

**All W7 software is built and exercised; the blocker is physical RF.** The
module hears an ESP8266 AP *on the same bench* at **-84 dBm** (should be
-20…-40). Management frames at 1 Mbps survive that; EAPOL and data do not.

## ACTION REQUIRED before any more software (hardware, user-only)

**Inspect the M.2 card's antenna.** The MAYA-W161 module routes RF through
MHF4/U.FL connectors — check that a pigtail/antenna is attached AND seated on
the right port (ANT0), and whether this card variant even ships an antenna.
`wlan-set-antcfg 1 / 2 / 0xFFFF` (fw antenna select) made no difference
(-81…-84 dBm on all), so it is NOT fixable from software.

Fastest re-verify once touched: flash NXP's
`~/Development/mcuxsdk-ws/mcuxsdk/build_wifi_m2/wifi_cli_cm7.elf`, then
`wlan-scan` over the VCOM — the bench ESP (or any adjacent AP) must read
better than ~-45 dBm. Then `wlan-add esp ssid ESP8266TEST` + `wlan-connect
esp` must reach "Connected" with an IP — that proves radio + DHCP end-to-end
against a reference stack in two minutes. `scratchpad/drive_wifi_cli.py`
(session scratch, recreate from transcript if gone) automates it.

## What W7 established (all silicon-measured, 2026-08-18)

1. **The ~10 s WPA2 drop was never an idle timeout.** On an OPEN AP the link
   holds indefinitely (69+ service passes, zero traffic needed). On WPA2 the
   AP deauths reason 15 after ~11.6 s because **the 4-way handshake never
   completes** — reason 15 means literally what it says.
2. **W6's "WPA2 CONNECTION WORKS" is corrected.** The ESP's
   `onSoftAPModeStationConnected` fires at **association**, not after the
   handshake (proven: it fired while the handshake provably died). W6 really
   proved: association + correct PMK derivation. The full connect never
   happened. Related: **this firmware DOES emit EVENT_PORT_RELEASE (0x2B)** —
   seen on every open-AP connect. It never came on WPA2 because the handshake
   never finished.
3. **Data path dead both ways while RF-starved**: every `sendDataFrame`
   is CMD53-accepted but the card never frees the WR_BITMAP port (16 sends →
   bitmap 0x0 → TX dead); AP-side 1 Hz UDP broadcasts never reach the host.
   Host init parity with NXP (RECONFIGURE_TX_BUFF/11N_CFG/AMSDU/RTS_CTS —
   all accepted) changed nothing; TX framing is byte-identical to NXP's.
4. **NXP's own wifi_cli fails identically on this hardware** (authenticates,
   never gets a DHCP lease) — the M2Radio host driver is exonerated.

## What's already in place for the moment RF works

* Driver (`M2Radio/iw416/Iw416.{h,cpp}`): `sendDataFrame()` (TxPD framing),
  `pollLink()` (RX + event service, LINK_LOST/deauth/disassoc → drop),
  `reconfigureTxBuffers()`, `set11nCfg()`, `amsduAggrCtrl()`.
* Probe (`m2_sdio_probe.cpp`): complete IPv4 mini-stack — DHCP client with
  retry (DISCOVER/REQUEST every 3 s), ARP responder, ICMP echo request every
  2 s once bound + echo responder. Console line:
  `net: dhcp=… ip=… server=… ping=tx/rx … wr_bitmap=… cfg=ok/ok/ok`.
* Expected behaviour with a working antenna, no code changes: associate →
  (WPA2: handshake completes, watch for 0x2B) → DHCP binds to 192.168.4.x →
  pings answered → `wr_bitmap` stays near 0xFFFF → link holds.

## W8 plan

1. User fixes/attaches the antenna.
2. Re-verify RF with wifi_cli (`wlan-scan` RSSI sanity, reference connect).
3. Re-run the probe against the OPEN ESP AP → expect `dhcp=bound`, pings.
4. Flip ESP to WPA2 (same sketch, PSK in `ap_creds.h` from the probe build's
   CMakeCache) → expect handshake completion (0x2B on WPA2 for the first
   time), then DHCP + hold. That completes W7's original milestone.
5. Then W8 proper: promote the probe's netlite into the driver or wire up a
   real stack (lwip port?) — decide then.

## The test rig (state as left)

* ESP8266 (`/dev/cu.usbserial-0001`): **OPEN** AP "ESP8266TEST" ch6,
  IP 192.168.4.1, broadcasting a 1 Hz UDP datagram to 192.168.4.255:4711
  (RX-path probe), logging `STA CONNECTED/DISCONNECTED` + `stations=N`.
  Sketch in session scratch `esp_ap/esp_ap.ino`; to restore WPA2, regenerate
  `ap_creds.h` with the PSK from the probe build's gitignored CMakeCache
  (`M2RADIO_WIFI_PSK`) — **never commit it**.
* EVKB: left running `m2_sdio_probe` (W7 build, creds compiled in).
* Dual serial reader: session scratch `dual_read.py` (both ports, one
  timeline). Board VCOM `/dev/cu.usbmodem5DQ2DDHVWO5EI3` @115200.

## Operational notes (unchanged + new)

* Plain `LinkServer flash … load`, never `--erase-all`; VCOM-free while
  programming; `pkill LinkServer redlinkserv crt_emu_cm_redlink` first.
* `LinkServer run` = load + reset + free-run; background it, wait ~15 s,
  then attach readers.
* The QEMU gate (`./run_qemu.sh`) stays green throughout — it asserts the
  no-IO-function fallback and none of the W7 code runs in QEMU.
