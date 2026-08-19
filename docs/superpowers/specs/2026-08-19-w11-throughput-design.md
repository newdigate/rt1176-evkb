# W11: Wi-Fi Throughput Measurement + Bottleneck Fix — Design

Date: 2026-08-19.  Follows W10 (IEEE PS workaround soak-proven; link stable).
Board: MIMXRT1170-EVKB RevC3 + u-blox M2-MAYA-W161 (IW416/SD8978), driver
sibling `~/Development/M2Radio` (pinned `c4f3940`).

## Goal

Measure the real TCP and UDP throughput of the M2Radio + lwip stack, both
directions, against the user's house AP with a peer whose numbers the EVKB
cannot fake — then instrument the driver, identify the top bottleneck, and
fix only that.  The DAT1-interrupt decision is made from this data, not
assumed.

## Context that shaped the design

* The link has never seen more than ~5 pkt/s (W9 echo cadence).  IW416 PHY
  is 802.11n; the 2.4 GHz association will cap around 72 Mbps HT20 —
  orders of magnitude above anything the current CMD52-polled data path
  can do, so the AP is not the ceiling that matters.
* House network measured from the Mac (2026-08-19): SSID
  `OnestreamQJN7_5G`, 802.11ac ch36/80 MHz, **WPA2 Personal** (no
  WPA3/PMF complication), Mac at 192.168.1.101/24, RSSI -34 dBm.  The
  2.4 GHz sibling SSID `OnestreamQJN7` is the EVKB's target (same PSK).
* The ESP8266 bench oracle is NOT used this phase: its own stack (~1-5
  Mbps) would be the thing measured.  The Mac is the peer, already on the
  house LAN; the EVKB joins the same LAN so no routing/NAT is involved.
* Mac application firewall is ENABLED — first inbound to the Python peer
  may raise an Allow prompt (user clicks Allow; noted in the run task).

## Credentials (hard rule)

The house PSK lives ONLY in the session scratchpad (`house_psk.txt`, line
1 = PSK; optional line 2 = SSID override, absent = `OnestreamQJN7`).  It
reaches the build as a CMake cache variable rendered into a generated,
gitignored `wifi_creds.h` — the exact `m2_lwip_test` mechanism.  It must
never appear in a committed file, a commit message, or configure output
(the CMakeLists prints "(PSK supplied, not shown)").

## Components

### 1. Example `examples/networking/m2_throughput_test/` (new, committed)

Board preamble (M.2 reset release + 1.8 V — the known silicon/QEMU
divergence trap), `connectStation()` to the house SSID (IEEE PS stays ON —
its behaviour under load is itself a measurement; `delay_to_ps=1000`
should hold the card awake during a blast), lwip netif + DHCP, then four
passive raw-API services the Mac sequences externally:

| Test    | EVKB role                          | Port | Authoritative count |
|---------|------------------------------------|------|---------------------|
| TCP RX  | sink: count+discard until FIN      | 5001 | Mac (bytes sent) + EVKB (bytes rx) |
| TCP TX  | source: blast 10 s on accept, close| 5002 | Mac (bytes received) |
| UDP RX  | sink: count seq'd datagrams        | 5003 | Mac sent vs EVKB stats reply |
| UDP TX  | source: blast on "GO" control      | 5003 | Mac (datagrams received) |

UDP framing: 1400-byte datagrams, first 4 bytes big-endian sequence
number.  Control datagrams on 5003 carry the ASCII magic `TPUT` —
`TPUT STATS?` (sink replies `TPUT STATS rx=<n> hi=<h>`), `TPUT GO <secs>`
(EVKB blasts seq datagrams back to the sender's addr:port, then
`TPUT DONE tx=<n>`).  Everything else on 5003 is test data.

Each test prints one serial line: `tput: <name> bytes=<n> ms=<m>
kbps=<k>` plus the standing `lwip:`-style status line with `ps=` counters
and the new driver bus counters (below).  The Mac side's numbers are the
ones the transcript reports; the EVKB's own lines are cross-checks.

lwip must not be the artificial bottleneck: `TCP_MSS` 1460, `TCP_WND` and
`TCP_SND_BUF` at least 8×MSS, pbuf pool and `MEMP_NUM_TCP_SEG` sized to
cover (exact values in the plan; RAM is plentiful).

QEMU gate: device-absent fallback, same shape as `m2_lwip_test`'s (no
card in QEMU either way — the gate proves clean fallback, nothing more).

### 2. Mac peer `tput_peer.py` (committed beside the example)

One script, four subcommands (`tcp-rx`, `tcp-tx`, `udp-rx`, `udp-tx`) or
a single `all <evkb-ip>` sequence.  It measures on the Mac side (bytes
and wall-clock around its own sockets) and prints a machine-readable
result line per test.  Contains no credentials; committed because it is
example tooling (like `run_qemu.sh`), MIT like the tree.

### 3. Driver bus instrumentation → one targeted fix (M2Radio)

Counters sufficient to attribute per-frame bus overhead, exposed via
accessors and printed by the example: CMD52 polls per TX frame and per
service pass, CMD53 transfer count/bytes, and byte-vs-block mode split.
After first measurement, fix ONLY the top attributed bottleneck
(candidates, in expected order: CMD53 byte-mode vs block-mode, CMD52
poll loops, single-frame-per-pass service).  Anything second-order rolls
to W12.  Driver changes go through the standing subagent review loops
and end in a push + pin bump.

## Success criteria

* A 4-cell Mbps table (TCP/UDP × TX/RX) measured by the Mac, in a dated
  section of the example's `transcript_hw_evkb.txt`, with PS counters
  shown healthy (or the PS-vs-load interaction documented as a finding).
* One bottleneck identified from counters; its fix reviewed, landed, and
  re-measured (delta in the transcript) — or a written decision that the
  first number is acceptable and why.
* Sweep 97/0/0 from `/tmp/ev`; licence audit PASS with the new GATES
  entry; CLAUDE.md gate count re-measured; M2Radio pushed + pin bumped if
  the driver changed; branch pushed.

## Risks / stop conditions

* **`OnestreamQJN7` (2.4 GHz) not visible to our scan** — if the house
  router turns out to be 5 GHz-only, STOP: 5 GHz scan support (SCAN TLV
  channel list + band config) is its own work item, not a blind patch.
  First silicon step is therefore a plain scan print.
* **PS thrash under load** — if `ps=` churns during a blast and caps
  throughput, that is a FINDING to record (and possibly the W11 fix),
  not a reason to quietly disable the W10 workaround.
* **Firewall** — Mac may prompt on first inbound; user clicks Allow.
* The erratum's sparse-traffic regime does not apply during blasts, but
  idle periods between tests stay PS-protected (PS is never turned off).
