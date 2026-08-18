# Continue M.2 Wi-Fi bring-up (W7: keep the link alive + data path / DHCP)

u-blox **M2-MAYA-W161** (NXP **IW416**/SD8978) on **MIMXRT1170-EVKB RevC3**,
repo `~/Development/rt1176-evkb-m2-maya-w161`, branch **`m2-phase0-serial2`**.
Driver in sibling `~/Development/M2Radio` (master, pushed, pinned in
`evkb.cmake` at **`a32f9b3`**).

**Read first:** `examples/networking/m2_sdio_probe/transcript_hw_evkb.txt` —
the **"W6 RESULT: WPA2 CONNECTION WORKS"** section at the very end.

## Where things stand

| Phase | State |
|---|---|
| W1–W5 | ✅ enumerate, firmware, host cmds, scan, monitor RX |
| W6 — WPA2 connect | ✅ **DONE.** Associates, completes the 4-way handshake, **connects**. Independently confirmed by an AP-side oracle. |
| W7 — keep link + data path | ⬜ link takes a periodic deauth ~10 s after connect; no L3 traffic yet |

## W6 is finished — the card connects to Wi-Fi

The IW416 associates (`assoc_status=0`, real `cap_info=0x11`), the **embedded
supplicant completes the WPA2 4-way handshake**, and the card **connects**.
Proven by an **independent oracle**: a controlled **ESP8266 WPA2 SoftAP**
(sketch fires `onStationConnected` only *after* the handshake) reported
`AP: STA CONNECTED mac=6C:1D:EB:91:0C:45` + `stations=1` for the card, held
stably. This is the first Wi-Fi connection by this M.2 card on this board.

### Two HOST bugs were hiding it (both fixed)

1. **This firmware never emits `EVENT_PORT_RELEASE` (0x2B).** Connect success is
   signalled by `ASSOCIATE` returning `assoc_status==0`; the handshake runs
   internally (no EAPOL to the host). Waiting for a port release mis-scored a
   real connect as a timeout. The connect event actually seen post-assoc is
   `0x17` (EVENT_WMM_STATUS_CHANGE) at ~1 ms — benign, not the connect signal.
2. **Re-associating in a loop tore the link down.** The earlier "connects then
   drops after ~1–4 s" (iPhone) and the ~9.6 s connect/drop churn (ESP) were the
   probe re-associating every pass. Fixed model: **connect ONCE, then service
   the link** (`wpaServiceLink` in `m2_sdio_probe.cpp`) — drain events/data,
   watch for a real deauth, reconnect only on drop.

## The W7 open problem: link drops ~10 s after connect

With the connect-once/hold firmware the link is stable for ~**10 s**, then takes
a **real deauth** and the card reconnects cleanly (`reassocs` climbs):

* `last_event=0x8` (DEAUTHENTICATED), `event_info` low byte **`0x0F` = reason 15**
  (4-way-handshake timeout), sometimes **`0x06`** (class-2 frame from
  nonauthenticated STA).
* Connected duration is a very round **~10.0 s** every time → it is a **timer**,
  not jitter.

**Leading hypothesis:** the AP drops a station that sends **no L3 traffic**
(no DHCP / ARP / null-data keepalive). Real STAs DHCP immediately after connect,
which keeps them active. Our probe does nothing at L3 yet. This is exactly the
W7 data path — so building it both tests the hypothesis and is the milestone.

**Not yet ruled out:** a supplicant rekey / group-key maintenance gap (would
also present as reason 15). A cheap disambiguator: point the card at an **OPEN**
ESP AP (no encryption). If it then stays connected indefinitely → the drop is
WPA2-maintenance-specific; if it still drops at ~10 s → it's an idle/data-path
drop. (The probe already handles open APs: `rsnLen==0` → no RSN IE, auth Open.)

## W7 plan (suggested)

1. **Data-path TX (TxPD).** Send a frame down the data port: build a `TxPD`
   header + 802.3 frame, write over the WR_BITMAP port (mirror the RX path in
   `Iw416::diagConnect`/`watchConnect` which already reads RxPD + WR/RD bitmaps).
2. **DHCP DISCOVER** as the first real frame → get an IP from the AP
   (ESP AP is `192.168.4.1`, DHCP server active). A DHCP round-trip is the W7
   proof, and the traffic should also stop the ~10 s idle deauth.
3. If DHCP alone doesn't hold the link, add a periodic **null-data / ARP
   keepalive** in `wpaServiceLink`'s connected branch.

## The test rig (recreate it)

* **ESP8266 SoftAP** as controlled AP + oracle. Board on `/dev/cu.usbserial-0001`,
  flashed via `arduino-cli` (`esp8266:esp8266:generic`, core 3.1.2). Sketch was
  in session scratch `esp_ap/esp_ap.ino`: `WiFi.softAP(SSID, PSK, 6)`, WPA2-PSK,
  logs `onSoftAPModeStationConnected/Disconnected` + `stations=N` every 2 s.
  SSID **`ESP8266TEST`**, a throwaway **12-char** PSK (WPA2 needs ≥8) — **kept
  out of git**; recreate the sketch and rebuild the probe with matching creds.
  `onStationConnected` fires only after the 4-way handshake → un-fakeable proof.
* **Probe creds** are compiled in via
  `cmake -B build -DM2RADIO_WIFI_SSID="ESP8266TEST" -DM2RADIO_WIFI_PSK="<psk>"`
  → gitignored `build/wifi_creds.h` (`HAVE_WIFI_CREDS`). **Never commit the PSK.**
* **Read both serials at once** to correlate card state vs AP truth — the dual
  reader used this session lived at `/tmp/dual_read.py`.

## Operational gotchas learned this session (important)

* **Flashing: use plain `LinkServer flash … load`, NOT `--erase-all`.**
  `--erase-all` mass-erases the whole **64 MB** QSPI NOR and is slow; killing it
  mid-erase looks *exactly* like a hung/unreachable probe (stalls at "Selected
  probe", 0-byte crt_emu output). Plain `load` sector-erases only what it writes
  (~7 sectors, ~3 s). Diagnose "stuck flash" by reading the full crt_emu log
  (it reaches "Mass erasing Flash" then your timeout kills it) — the SWD connect
  itself is fine (`LinkServer probes` returns instantly).
* `LinkServer run` writes its detailed progress to a **temp `.out` file that it
  buffers until exit**, so `run`'s top-level log looks stuck at 3 lines while it
  is actually programming. Don't judge `run` progress by that log.
* VCOM-free while programming (unchanged rule). Board = MCU-Link VCOM
  `/dev/cu.usbmodem5DQ2DDHVWO5EI3` @115200.
* The card's setup() takes ~20–30 s (firmware download). It connects once in
  setup, then loop() holds/services the link.

## Files

* `~/Development/M2Radio/iw416/Iw416.{h,cpp}` — driver. New: `watchConnect()`
  (full-window event watcher + event log). `associate()`, `setPassphrase()`,
  `queryPmk()`, `buildAssocRsnIe()`, `scan()` all working.
* `examples/networking/m2_sdio_probe/m2_sdio_probe.cpp` — `wpaServiceLink()`
  connect-once/hold state machine; `wifi: connected=… assoc_status=…` report in
  loop().
* Security: user's own AP; **Wi-Fi PSKs must never be committed** — only via
  `-DM2RADIO_WIFI_PSK` into the gitignored build header.
