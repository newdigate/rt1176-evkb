# Task 14 — silicon runbook for the Arduino WiFi façade

You are running this; I verify the transcripts against what the gates and the
instrumentation predict. Everything below is copy-pasteable.

**Before anything: semaphore with the other session.** This flashes the EVKB.

---

## 0. Two inputs I do not have, and must never have in the repo

| input | where it goes | notes |
|---|---|---|
| ESP bench AP passphrase | `-DM2RADIO_WIFI_PSK=` at configure time | rendered into a **gitignored** generated header in the build dir; CMake prints `(PSK supplied, not shown)` |
| IW416 firmware `.bin.inc` | `-DM2RADIO_IW416_FW=` at configure time | NXP-licensed, never vendored |

★ **A build dir is secret-bearing.** `M2RADIO_WIFI_PSK` is a CMake **CACHE**
entry, so the value sits verbatim in `build/CMakeCache.txt` regardless of the
generated-header indirection. Never attach a build dir to a bug report, never
commit one. (`build*/` is gitignored, so it cannot be committed by accident.)

Also: **J15 must be EMPTY.** `WiFi.begin()` switches the SDIO pads to 1.8 V and
J15 (microSD) is the same bus — a 3.3 V-only card there must not meet a 1.8 V rail.

The bench AP may be the ESP8266 or the newer ESP32-C6 (`tools/esp32c6-benchap/`).
Either is fine: the C6 deliberately keeps the same SSID, PSK and 192.168.4.x
subnet, so nothing below changes.

---

## 1. Configure and build both examples

Run from the **main checkout** (`~/Development/rt1176-evkb-m2-maya-w161`), which
now carries the merged branch.

```sh
cd ~/Development/rt1176-evkb-m2-maya-w161/examples/networking/wifi_client_test
cmake -B build \
  -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake \
  -DM2RADIO_IW416_FW=<path-to-.bin.inc> \
  -DM2RADIO_WIFI_SSID=ESP8266TEST \
  -DM2RADIO_WIFI_PSK=<psk>
cmake --build build
```

Same for `wifi_server_test`. Confirm CMake prints:
```
-- IW416 firmware: <path>
-- Wi-Fi target SSID: ESP8266TEST (PSK supplied, not shown)
```
If it says `IW416 firmware: not supplied` the blob path is wrong and the run
will stop at `wifi_status=255`.

---

## 2. Flash — VCOM must be FREE during the load

```sh
pkill LinkServer; pkill redlinkserv; pkill crt_emu_cm_redlink
LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load \
  ~/Development/rt1176-evkb-m2-maya-w161/examples/networking/wifi_client_test/build/wifi_client_test.elf
```

**Attach the console only after the load finishes.** Holding the VCOM during
programming gives `request to clear DAP error failed - status 131` /
`LOAD_EXIT=255`, and `pkill LinkServer` alone leaves `redlinkserv` resident.

```sh
python3 ~/Development/rt1176-evkb-m2-maya-w161/tools/rt1170-console.py /dev/cu.usbmodem* 115200 | tee /tmp/wifi_client_hw.log &
LinkServer run MIMXRT1176:MIMXRT1170-EVKB \
  ~/Development/rt1176-evkb-m2-maya-w161/examples/networking/wifi_client_test/build/wifi_client_test.elf &
```

---

## 3. `wifi_client_test` — what a PASS looks like

Let it run **≥ 90 s**. Expected shape:

```
RT1176 WiFi client test up
wifi_status=3
wifi_ip=192.168.4.<n>
wifi_rssi=-<nn>
srv_listen=1 err=0 (LISTEN_OK)
alive=1 tcp=1/1/0 lastfail=0 srv=1/0 ...
alive=2 tcp=2/2/0 lastfail=0 ...
```

**The load-bearing assertions** — these are what no QEMU gate can reach:

| token | must be | why it matters |
|---|---|---|
| `wifi_status=3` | exactly 3 (`WL_CONNECTED`) | associated **and** DHCP gave an address |
| `wifi_ip=192.168.4.x` | a real lease | the AP granted it; the firmware cannot invent it |
| `tcp=N/N/0` | **ok == tx, fail 0** | every echo came back **byte-exact** from the ESP oracle at `192.168.4.1:4712`. This is the un-fakeable core. |
| `lastfail=0` | 0 | no `WiFiClient::connect()` failure of any of the 7 kinds |
| heartbeat | never stops | the `alive=` counter must keep incrementing for the whole run |

**If `tcp=` shows fails**, `lastfail=` names which of the seven causes:
`1 NO_LINK, 2 NO_SLOT, 3 NO_PCB, 4 NO_ROUTE, 5 TIMED_OUT, 6 REFUSED, 7 DNS_FAILED`.
A non-zero `fail` with `lastfail=0` means the connect worked and the **echo
mismatched** — that is a data-path fault, not a connect fault.

---

## 4. `wifi_server_test` — needs the Mac on the bench AP

Flash it the same way. Note its `wifi_ip=`. Then **join the Mac to the bench
AP's network** and run the peer:

```sh
python3 ~/Development/rt1176-evkb-m2-maya-w161/examples/networking/wifi_server_test/wifi_peer.py all <board-ip>
```

Expected:
```
WIFISRV echo PASS
WIFISRV concurrent PASS
WIFISRV fill evicted_peers=1
WIFISRV fill PASS
```
exit 0. **The Mac side is the authoritative measurement**; the board's serial
line is the cross-check, never the measurement.

On the board's serial you should see `evict=` increment by **1** across the
`fill` test.

★ **Compare the DELTA, never the absolute.** `fill` is not idempotent inside
~40 s: a peer closing an *unclaimed* conn leaves the slot `PEER_CLOSED` holding
its pcb until the 30–40 s stall valve reaps it, and that valve measures from the
**accept**, not from your close. A repeat run still passes but `evict=` climbs.

### What each peer test proves

- **echo** — three sequential connections, byte-exact each. Basic accept + data.
- **concurrent** — two connections with bytes staged on **both before either is
  read**: two pool slots live simultaneously. (Fails on a 1-slot server.)
- **fill** — four idle connections, then a fifth that must still succeed. This
  is the **only** evidence for the §6 eviction valve. `evicted_peers=N` is the
  peer watching for the RST that `tcp_abort()` puts on the wire; it fails
  against a server that refuses instead of evicting.

---

## 5. The four things silicon is proving that nothing else can

QEMU's IW416 model returns **zero scan results by design**, so the board never
associates there. These have no automated coverage anywhere:

1. **Association + DHCP + real TCP** — everything past the scan.
2. **The service pump actually running.** Measured fact: the `[wifi]` gate is
   green with a working pump *and* with a dead one, because with no link
   `servicePass()` never reaches `iw416NetifPoll()`. A steadily climbing `tcp=`
   with `fail=0` over 90 s is the proof.
3. **`WiFiClient::connect()`'s `TIMED_OUT` and `REFUSED` arms** — both need a
   real peer.
4. **The success path of reconnect** — `connectAndDhcp()` clearing
   `m_wantReconnect` and returning `WL_CONNECTED` is the one branch the
   zero-BSS model can never execute.

Optional but valuable if you have time: pull the AP's power for ~10 s and
restore it, with `WiFi.setAutoReconnect(true)`. That exercises #4 plus the
DHCP-timeout path, which is otherwise reasoned-only.

---

## 6. What to send me

Both raw logs (`/tmp/wifi_client_hw.log`, the server run) and the
`wifi_peer.py` stdout. **Scan them for secrets first** — the PSK is never
printed by design, but check rather than assume.

I will save them as `transcript_hw_evkb.txt` in each example directory with a
dated header describing the bench, following the format of
`m2_lwip_test/transcript_hw_evkb.txt`, and verify each token above against what
the gates and instrumentation predict.

If something fails, send the log anyway — `lastfail=`, `srv=<up>/<err>`, and the
`evict=/stall=/refuse=` counters exist precisely so a failed run is diagnosable
rather than silent, and I would rather see the real failure than a clean rerun.
