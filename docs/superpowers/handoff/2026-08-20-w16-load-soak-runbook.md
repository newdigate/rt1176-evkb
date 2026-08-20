# W16 load soak: the one measurement aggregation still owes

Everything else in W16 is done and committed. This is the single open item, and
it is small — it needs a peer on the bench AP's LAN and about twenty minutes.

## Where things stand

| | status |
|---|---|
| Multiport register port | **VALIDATED ON SILICON.** `mpregs=1`, ~11 bus commands per service pass → ~1. |
| RX aggregation | Proven in QEMU (`[rxaggr]`: 8 frames, 1 CMD53), **not on silicon**. |
| TX aggregation | Proven in QEMU (`[txaggr]`: 6 frames, 1 CMD53, card split them back onto 6 slots). On silicon, seen **once**, incidentally: `aggr:0/2` during a reconnect burst. |
| Sweep | 105 passed, 0 failed, 0 SKIP. |
| Licence audit | PASS. |
| Pushed | M2Radio `e34b3d9`, qemu2 `7e17eff5d3`, `evkb.cmake` pin bumped. |

**Why aggregation is unmeasured and that is not a defect.** A sparse link never
holds two occupied ring slots at once, so `aggr:0/0` is the correct reading —
aggregation can only help when frames arrive faster than the host services
them. Every silicon line so far has been sparse.

## The rig, and the one thing that blocks it

* Board: MIMXRT1170-EVKB, MCU-Link VCOM `/dev/cu.usbmodem5DQ2DDHVWO5EI3`.
* Bench AP: ESP8266 `ESP8266TEST`, console on `/dev/cu.usbserial-0001`. The
  board associates and takes `192.168.4.100`.
* **Blocked on:** this Mac has ONE Wi-Fi interface (`en0`), so joining
  `ESP8266TEST` drops it off the house network for the duration. The house AP's
  board-side credentials were rotated in W15 and are dead, so there is no
  both-on-one-network option.

## Runbook

1. Join `ESP8266TEST` on this Mac. Confirm with `ipconfig getifaddr en0` →
   expect `192.168.4.x`.
2. Board should already be flashed and associated; if not:
   ```bash
   pkill -f rt1170-console.py; pkill LinkServer; pkill redlinkserv; pkill crt_emu_cm_redlink
   ```
   then `LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load build/m2_throughput_test.elf`
   **VCOM-free**, then attach `tools/rt1170-console.py` and reset.
3. Baseline arm, register port ON (the default):
   ```bash
   python3 examples/networking/m2_throughput_test/tput_peer.py all 192.168.4.100
   ```
4. Read the board's `tput:` line for the same window and record
   `regs`, `rxtx`, `aggr`, `total`, and the health set.

## What to assert, and what NOT to

**The verdict rests on the counter mechanism, never on Mbps.** 2.4 GHz variance
is 2×–4× run to run (measured 2.95–14.8 Mbps on identical builds), so an Mbps
table is a floor, not a benchmark. That rule has already produced two wrong
conclusions in this file's history.

* **`aggr:` must go non-zero, and by a lot.** It counts slots carried by CMD53s
  that carried more than one. Under a tcp-rx blast the RX ring should routinely
  hold a run of occupied slots.
* **Commands per frame is the number.** `total` (busCommands) divided by frames
  delivered in the SAME window. W11's ceiling was ~10 per frame; the register
  port alone should already have taken ~7 of those out, and aggregation should
  cut the remaining data CMD53s below one per frame.
  ★ **Normalise per frame or per blast, always.** Never compare absolute
  counters across runs of different length — that single error produced two
  wrong conclusions in one day in W12.
* **Health counters must be no worse:** `stranded`, `resyncs`, `notready`,
  `drainerr`, and W16's new `split` (a batch packet whose SDIOPkt size
  disagreed with the slot length the card published — expected 0; non-zero
  means the block-padding inference is wrong at one end).
* **`rxRingResyncs` is high by nature** (~50–230 per 10 s blast) and
  `notready=0` proves those resyncs then read real packets. Normal operation,
  not damage. Misread as thrash twice already.

## The controlled A/B, if you want one

`Iw416::useRegisterPort(false)` forces the pre-W16 CMD52 transport on a card
whose register port works, so both arms run in ONE firmware life. Use it rather
than comparing across builds — W12's lesson is that an A/B whose arms differ in
a second respect measures the difference nobody intended.

## Two things to watch for

* **A wiped build cache is indistinguishable from a dead card.** No blob/creds
  → the Wi-Fi path compiles out → the same `alive=` heartbeat as card-absent.
  Check `grep -E "^M2RADIO_(IW416_FW|WIFI)" build/CMakeCache.txt` before
  blaming hardware, and never `cmake -B`/`rm -rf` an existing example build dir.
* **`mpregs=` in every status line and in the freeze dump.** A link that fell
  back to CMD52 would otherwise be diagnosed as an aggregation bug. `mpregs=1`
  is the healthy reading; `mpregs=0` with `rej=` says the register port
  returned a uniform snapshot and the driver walked away from it.

## If aggregation does NOT engage under load

The likely causes, in order:

1. The host is servicing faster than frames arrive, so runs are length 1. Check
   `rxtx` against frames: if data CMD53s ≈ frames, look at whether
   `serviceLink` is being called often enough to drain per frame.
2. The batch stops at the first slot because `RD_LEN` reads 0 there — that is
   the W13 positive check doing its job, and `notready` will be non-zero.
3. The ring wraps at slot 31 mid-run: by design a batch never wraps, so a run
   spanning the top is split. Costs at most one extra CMD53 per lap.

None of those are failures of the encoding. A failure of the *encoding* looks
like missing or garbled frames, or `split` non-zero — not like a low `aggr`.
