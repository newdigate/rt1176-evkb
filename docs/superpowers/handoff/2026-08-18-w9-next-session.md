# Continue M.2 Wi-Fi bring-up (W9: the link WORKS — promote it into a real stack)

u-blox **M2-MAYA-W161** (NXP **IW416**/SD8978) on **MIMXRT1170-EVKB RevC3**,
repo `~/Development/rt1176-evkb-m2-maya-w161`, branch **`m2-phase0-serial2`**.
Driver in sibling `~/Development/M2Radio` (master, pushed, pinned in
`evkb.cmake` at **`d978711`**).

**Read first:** `examples/networking/m2_sdio_probe/transcript_hw_evkb.txt` —
the **"W8 RESULT"** section at the very end.

## The one-line status

**WPA2 + DHCP + ping work end to end.** Handshake completes in 13 ms
(embedded supplicant, 0x2B), DHCP binds in 2 sends, pings flow encrypted,
zero drops, link holds. W6–W8's three faults are all closed:

1. **Antenna** (physical) — fixed by the user in W8; verified: scan RSSI
   -84 → -61 dBm, and NXP wifi_cli got the card's first-ever DHCP lease.
2. **32-port ring bug** (host driver) — fixed in M2Radio `d978711`. Both
   data-port directions are 32-slot RINGS (fw uploads to successive ports;
   frees TX ports in a batch at ring top). The old 16-bit lowest-set-bit
   driver starved TX at 16 sends and went blind to RX after the monitor
   phase consumed slots 0-15.
3. **NET_MONITOR firmware erratum** — running NET_MONITOR enable/disable
   before ASSOCIATE leaves managed RX-to-host delivery dead (A/B-proven,
   same driver). The probe's W5 monitor phase is now compiled OUT by
   default (`-DM2_WITH_MONITOR` restores it; never combine with data-path
   work). NXP's stack never exercises NET_MONITOR.

## W9 plan (the deferred "W8 proper")

Promote the probe's hand-rolled netlite (DHCP/ARP/ICMP in
`m2_sdio_probe.cpp`) into something an application can use. Decide:

* **Option A**: port lwip (NXP's stack uses it; licence is BSD — fine for
  the licence firewall, but check every file you vendor).
* **Option B**: grow the netlite into `M2Radio` as a small clean API
  (link + UDP + DHCP client is already written and silicon-proven).
* Either way: the driver surface it sits on (`sendDataFrame`/`pollLink`)
  is now trustworthy — 122+ frames RX, ring resyncs=0.

Also worth doing early in W9:

* **Interrupt-driven service.** Everything today polls HOST_INT_STATUS via
  CMD52. The card's DAT1 interrupt + the uSDHC's interrupt path would drop
  the poll loop.
* **Throughput check**: iperf-ish UDP blast both ways; the ring code has
  only seen ~2 pkt/s. Multi-packet-per-pass and TX while RX pending are
  exercised but not stressed.
* A second `wlan-set-antcfg` look is NOT needed — RSSI is believable now
  (-53 on our scan at ~1 m).

## Verify-first (2 minutes, before touching code)

Flash `examples/networking/m2_sdio_probe/build/m2_sdio_probe.elf` (W8
build: ring driver, monitor off, creds compiled in) with
`LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load <elf>` — **never plain
`LinkServer run` for loading: it programs at ~2 kB/s with no output (8+ min,
looks hung); `flash … load` does the same image in seconds and resets.**
Watch the VCOM: expect `dhcp=bound ip=192.168.4.x … ping=n/n …
ring=T/R/0` within ~15 s of boot, against the WPA2 ESP AP.

## The test rig (state as left)

* ESP8266 (`/dev/cu.usbserial-0001`): **WPA2-PSK** AP "ESP8266TEST" ch6,
  PSK = the probe build's `M2RADIO_WIFI_PSK` (gitignored CMakeCache —
  **never commit it**), IP 192.168.4.1, 1 Hz UDP broadcast to
  192.168.4.255:4711, logs `STA CONNECTED/DISCONNECTED` + 2 s heartbeat.
  Sketch recreated in session scratch `esp_ap/esp_ap.ino` (OPEN/WPA2 via
  `ap_creds.h` `AP_PSK` define); flash with
  `arduino-cli compile+upload --fqbn esp8266:esp8266:nodemcuv2`.
* EVKB: left running the W8 probe, bound at 192.168.4.100, pinging.
* Board VCOM `/dev/cu.usbmodem5DQ2DDHVWO5EI3` @115200 (pyserial only;
  macOS `cat` resets the port to 9600).

## Debug technique worth keeping (it found the ring)

NXP's `wifi_cli` rebuilt with `CONFIG_WIFI_IO_DEBUG 1` prints every SDIO
interrupt's rd/wr bitmaps and every TX's port number — a working reference
trace at register level. The flag is a plain C define; the copy that WINS is
the **board-level**
`examples/_boards/evkbmimxrt1170/wifi_examples/wifi_cli/wifi_config/wifi_config.h`
(the middleware and example copies are shadowed). Build to a SEPARATE dir
(`build_wifi_m2_dbg`) so the known-good `build_wifi_m2` binary stays
pristine, delete the wifi_nxp objects to force a recompile (ninja misses
the header edit), and **revert the edit after** (done this session; tree is
clean). Note the CLI's UART drops characters while IO debug is on — the
drive script types with 10 ms/char delay.

## Operational notes

* Plain `LinkServer flash … load`; VCOM-free while programming;
  `pkill LinkServer redlinkserv crt_emu_cm_redlink` first.
* The QEMU gate (`./run_qemu.sh`) stays green (asserts the no-IO-function
  fallback; none of the Wi-Fi code runs in QEMU). Sweep count unchanged.
* `dual_read.py` / `read_port.py` / `drive_wifi_cli.py` in session scratch;
  trivial to recreate (pyserial, timestamps, slow typing for wifi_cli).
