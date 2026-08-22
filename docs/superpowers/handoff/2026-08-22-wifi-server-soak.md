# Soak the Arduino `WiFiServer`

Your job this session: run `examples/networking/wifi_server_test` under sustained
load for **hours**, and either close it out with an accounting that balances or
come back with a named defect. Independent of the W19 CM4 brief — run in either
order.

The façade is *correct*. It has never been *soaked*. Those are different claims
and only one of them is currently backed by evidence.

## Where things stand

| | status |
|---|---|
| `WiFiServer` / `WiFiClient` / `WiFiConnPool` | Correct. `transcript_hw_evkb.txt`, silicon, 2026-08-21. |
| Total load ever applied | **6 sessions, 176 bytes, ~156 s.** That is the entire body of evidence. |
| Sweep | 119 passed, 0 failed, 0 SKIP is the target. Do not weaken a gate to get there. |
| M2Radio pin | `300d32b`, carries `arduino/` + the W17/W18 uAP work. |
| Bench RF | A U.FL antenna is **on order**. Until it lands the card associates only at ~1 cm / −51 dBm. |

★ **Do not start the soak before the antenna is fitted.** Every number below is
about resource behaviour under load, and a link that drops every few minutes
produces reconnect churn that will swamp the signal you are looking for. If you
want to spend the waiting time usefully, build the two instruments in §2 — they
are the real prerequisite and neither needs the radio.

## 1. The gap, stated precisely

A correctness run asks *does the mechanism work once*. A soak asks *does it
still work on the ten-thousandth time, and does anything accumulate*. Five
things are unreachable in 156 seconds, and they are the reason this session
exists:

**★ `MEMP_NUM_TCP_PCB` is 5. This is the headline risk and it is not obvious.**
Measured in `~/Development/lwip/port/lwipopts.h:29`. The pool has
`WIFI_MAX_CONNS = 4` slots, so four live connections leave **one** spare pcb —
and `wifi_server_test` is a *one-shot* server that closes a connection per
exchange, which makes the **board the active closer**, which means the **board**
holds the `TIME_WAIT`. At 2×`TCP_MSL` those linger ~120 s against a pool of 5.
lwip self-heals via `tcp_kill_timewait()` when `tcp_alloc` starves, so this will
not announce itself as an error — it degrades. **Exchange rate versus pcb
pressure is the single most valuable number this soak can produce**, and nothing
short of a soak can produce it.

**`refuse=` has never been non-zero.** `WiFiPool::acceptRefusals()` fires only
when all four slots are *claimed* — which `wifi_server_test` cannot do, because
it holds no handle across passes. It is untested code on a live path.

**`echo_short=` has never fired.** The 5 s TX no-progress bail. `wifi_peer.py`
always reads, so it cannot reach it.

**The reconnect SUCCESS path after a genuine link loss.** The transcript says so
in as many words. `begin()`-failed-then-retried is covered (attempts 9–11 of
`wifi_client_test`); a link that was **up** and then **went away** is not.

**DHCP lease renewal.** T1 is half the lease. A soak is the only thing that
crosses it.

## 2. Build these two instruments FIRST

Neither needs the radio, and the soak is much weaker without them.

### 2a. A health line at parity with `m2_uap_lwip`

`wifi_server_test` prints `alive= srv= sess= evict= stall= refuse=` — all
**monotonic**, which means you cannot apply the technique that made the uAP soak
conclusive. Add a second line, printed every 2 s, carrying only quantities that
must be **invariant**, reached through `WiFi.radio()`:

    iw416.rxStrandedRecovered()   iw416.rxDesyncRecovered()
    iw416.rxSplitMismatch()       iw416.rxDropped()
    iw416.seqMismatches()         iw416.psWakes()

★ **Then collapse every sample with `sort -u` and require ONE line out.** This
is the whole point: checking the final sample proves only that the counters were
clean *at the end*, while a single distinct signature proves none of them was
**ever** non-zero at **any** sample. That is what made the 28.8-minute uAP soak
worth trusting — see the SOAK section of
`examples/networking/m2_uap_lwip/transcript_hw_evkb.txt`.

Add lwip pressure to the same line: live pcb count and `TIME_WAIT` count, walked
from `tcp_active_pcbs` / `tcp_tw_pcbs`. Without these the pcb hypothesis above
stays a hypothesis.

### 2b. A soak mode for `wifi_peer.py`

Today it has `echo`, `concurrent`, `fill`, `all` — all one-shot. Add
`soak <ip> <minutes>`, and make it:

* cycle a **mix** (echo, concurrent, fill), not one shape on repeat — the
  interesting failures are at the transitions;
* count **its own** bytes and exchanges. **The peer is authoritative**; the
  board's `sess=` is the cross-check, never the measurement;
* send **fixed-size** payloads, so `sess=N/B` must satisfy `B == N × size`
  **exactly**. An accounting that closes to the byte is the assertion; a counter
  that merely climbs is not;
* print a progress line per minute. You will want intermediate points — see the
  ratio trap in §5.

## 3. Rig and runbook

* Board: MIMXRT1170-EVKB, MCU-Link VCOM `/dev/cu.usbmodem*`, 115200.
* Bench AP: ESP8266 `ESP8266TEST` (or the ESP32-C6 in `tools/esp32c6-benchap/` —
  same SSID/PSK/subnet, nothing changes). Board takes `192.168.4.x`; server
  listens on **5010**.
* ★ **This Mac has ONE Wi-Fi interface.** Joining `ESP8266TEST` drops it off the
  house network for the whole soak. Plan for that before you start a 4-hour run.
* ★ **Semaphore with the user before flashing** — another session uses this board.

```bash
pkill -f rt1170-console.py; pkill LinkServer; pkill redlinkserv; pkill crt_emu_cm_redlink
```

Flash **VCOM-free**, then attach the reader, then reset. Capture everything:

```bash
python3 tools/rt1170-console.py /dev/cu.usbmodem* 115200 | tee /tmp/wifi_srv_soak.log
```

## 4. What to assert — and what NOT to

**Assert the accounting, never the throughput.** 2.4 GHz variance is 2×–4×
run-to-run on identical builds; that rule has already produced two wrong
conclusions in this repo's history.

| assertion | why it is the right one |
|---|---|
| `sess=N/B` with `B == N × size` | Un-fakeable. Every byte made a full round trip and the peer counted it independently. |
| ONE distinct health signature | Nothing was **ever** dirty, not merely clean at the end. |
| `refuse=0` **or** a refusal you can explain | A non-zero here is either the untested path finally firing or slot starvation. Both are results; an unexplained one is not. |
| `evict=` / `stall=` track offered load | Both valves are *supposed* to fire under `fill`. Flat is as suspicious as runaway. |
| heartbeat never stops | `alive=` gapping is the wedge everything else exists to catch. |

★ **Normalise per exchange or per minute. Always.** Never compare absolute
counters between runs of different length — that exact error produced two wrong
conclusions in one day in W12.

## 5. Traps, all previously paid for

★ **A ratio cannot distinguish a constant from a rate.** The uAP soak read
`dhcp_disc=31 / dhcp_req=30` at one moment as "~3% ongoing DHCP loss". It was a
**constant 1**, arriving at the first exchange and never moving across 46
cycles. One extra DISCOVER at boot is not a defect; 3% recurring loss would be.
It took a *second point* to tell them apart, and the second point was free
because the soak was already running. **Take intermediate readings.**

★ **A wiped build cache is indistinguishable from a dead card.** No blob/creds →
the Wi-Fi path compiles out → the same `alive=` heartbeat as card-absent. Check
`grep -E "^M2RADIO_(IW416_FW|WIFI)" build/CMakeCache.txt` before blaming
hardware, and **never `cmake -B` or `rm -rf` an existing example build dir.**

★ **A build dir is secret-bearing.** `M2RADIO_WIFI_PSK` is a CMake CACHE entry,
so the PSK sits verbatim in `build/CMakeCache.txt` regardless of the generated-
header indirection. Never attach one to a report. (`build*/` is gitignored, so
it cannot be committed by accident.) Credentials never enter the repo — a live
PSK was once committed here and is still in the pushed history.

★ **J15 (microSD) must be EMPTY.** `WiFi.begin()` switches the shared SDIO bus
to 1.8 V.

★ **The MCU-Link debug port drops after repeated `LinkServer run`/kill cycles**
and needs a physical repower. The board usually stays alive — **check the VCOM
first**, because a dead probe and a dead board look identical from the debugger.

★ **Scan presence is reliable; scan ABSENCE is not.** A single `system_profiler`
scan false-negatived an SSID that was demonstrably on the air. Absence means
nothing until it repeats — four consecutive scans, per the uAP transcript.

## 6. What to produce

Append a `SOAK` section to
`examples/networking/wifi_server_test/transcript_hw_evkb.txt`, following the
format of the uAP soak: duration, cycles, the health signature, and the
accounting written out so it visibly balances. State the RF conditions — a
transcript that hides its bench is worth less than one that states it.

If something breaks, **send the log anyway**. `lastfail=`, `srv=`, and
`evict=`/`stall=`/`refuse=` exist so a failed run is diagnosable. A real failure
is worth more than a clean rerun.

## 7. Explicitly NOT in scope

`WiFiHttpServer` is designed and queued — a real HTTP layer on top of
`WiFiServer`, with `using WiFiTcpServer = WiFiServer;` as an alias for sketches
that want the explicit pairing. **Do not build it in this session.** Soak the
thing that exists and is proven; promoting an unsoaked API is how a bad
interface becomes permanent.
