# W16 handoff: break the ~14 Mbps ceiling (multiport aggregation)

u-blox **M2-MAYA-W161** (IW416/SD8978) on **MIMXRT1170-EVKB RevC3**, repo
`~/Development/rt1176-evkb-m2-maya-w161`, branch **`m2-phase0-serial2`**.
Pins: **M2Radio `1e15f0b`**, **lwip `c6b2548`**; QEMU model needs
**qemu2 ≥ `2ed9314631`** (gitlab.com/Newdigate/qemu-rt1170).

**Read before forming any theory:**
`examples/networking/m2_throughput_test/transcript_hw_evkb.txt` — the full
W11→W15 record, including six conclusions of mine that were wrong and the
measurement that killed each. Several obvious-sounding theories about this
driver are already dead in there; re-deriving them costs days.

## The goal, and the number that defines it

Throughput is **3–14 Mbps**, and it is **bus-command-bound, not air-bound**.
Measured (W11, silicon, driver counters):

* **one frame per CMD53** — no aggregation at all;
* ~10 SDIO bus commands per frame at **~0.1 ms each**;
* ~840 frames/s sustained ≈ 1.2 ms/frame ≈ the observed Mbps at ~1.4 KB.

The fix is mlan-style **multiport aggregation (MPA)**: batch N frames into a
single CMD53 across consecutive ring slots, both directions. That attacks the
per-frame command count directly, which is the only quantity that has ever
moved this number.

## Why now (the prerequisite is finally in place)

W11 tried to cut bus cost with a passive bitmap cache and **regressed
throughput 2.5×**. The cause is the single most important thing to carry into
this work:

> The read-per-frame pattern was **accidentally pacing the host** to the
> firmware's batchy credit-free cadence. Removing it produced
> burst-then-dry-wait against `delay(1)` granularity — ~5 ms/frame.

The conclusion recorded then was that amortising bus work needs a real
completion signal or deep queueing, not a cache. W15 delivered the completion
signal: **DAT1 interrupt-driven service, silicon-validated** (idle bus cost
1070 → 88 CMD52/s, 12×). So aggregation now has something to pace against.

Also new since W11: **the QEMU model implements the rings and the lazy batched
TX credit free** (qemu2 `2ed9314631`), so aggregation correctness can be
developed and gated in emulation *before* silicon — which W11 could not do.

## Shape of the work

1. **Read the reference first.** `mcuxsdk .../wifi_nxp/` — `mlan_sdio.c`'s
   MPA aggregation buffers (`mpa_tx`/`mpa_rx`), `calculate_sdio_write_params`,
   and how NXP decides to flush an aggregation buffer (size limit, port
   discontinuity, timeout). Model the **firmware's contract**, not our
   driver's convenience.
2. **TX aggregation** — accumulate frames into one buffer while ring ports
   stay contiguous and credits allow; flush on discontinuity/limit/idle.
   Ring-order invariants must hold exactly as today.
3. **RX aggregation** — a single CMD53 spanning several occupied slots, split
   host-side. Note the firmware's per-slot `RD_LEN` (0x18 + (port<<1)) is what
   tells you the split points.
4. **Gate it in QEMU first**, then silicon.

## Conventions this tree enforces (violate these and the work is worthless)

* **Two-gate rule.** A QEMU pass is necessary, never sufficient. **Silicon
  wins.** Never weaken a gate or the model to make a divergence disappear —
  document it.
* **Every regression gate must be DEMONSTRATED to fail** against a
  deliberately broken driver, and the failure output kept. A gate never shown
  to fail is decoration. (`m2_rx_demo`'s `[ring]`/`[stranded]` headers show
  the format.)
* **Model the contract, not the driver.** W15 phase 3 is the cautionary tale:
  the driver forgot the SDIO card-side interrupt enable (CCCR 0x04), the QEMU
  model — written from the same reading — omitted the same gate, they agreed,
  emulation was green and silicon was dead. When model and silicon disagree,
  **fix the model first until emulation reproduces the silicon failure against
  the unfixed driver**, then fix the driver. Keep the red run in the record.
* Anchor every modelled behaviour to a cited silicon capture or NXP source;
  where nothing is measured, say so in a "where this could be lying" list
  (`hw/sd/iw416-sdio.c`'s header is the precedent).
* Driver work goes through implementer + review subagent loops; the reviewer
  has overridden me correctly more than once — take that seriously.

## Measurement discipline (this is where I kept going wrong)

* **Never compare absolute counters across runs of different length.** That
  single error produced two wrong conclusions in one day (W12: "resyncs 339 vs
  6150" compared a 1.5-blast run with a 110-blast run; normalised, the arm I
  "fixed" was the better one and I reverted my own change). **Normalise per
  frame or per blast, always.**
* **2.4 GHz variance is ~2×–4× run to run** (measured 2.95–14.8 Mbps on
  identical builds). Mbps tables are **floors, not benchmarks**. Any
  regression/improvement verdict must rest on the **counter mechanism**
  (commands per frame), not on Mbps deltas.
* Use the existing counters — they were built for exactly this:
  `cmd52PollsTx/Svc`, `cmd53Count/Bytes` (bytes are block-padded; wraps in
  ~2 h at full rate, consume as deltas), `cardInts`, and the health set
  `rxStrandedRecovered / rxDesyncRecovered / rxDrainErrors / rxSlotNotReady /
  rxRingResyncs`.
* **`rxRingResyncs` is high by nature** (~50–230 per 10 s blast) and
  `notready=0` proves those resyncs then read real packets. It is normal
  operation, not damage. I misread it as thrash twice.

## Invariants aggregation must not break

* **The ring safety net stays.** W13 established the firmware does **not**
  always raise `HOST_INT_UP_LD` for a queued upload (three host-side
  explanations each refuted by a counter). `rxStrandedRecovered()` climbing
  ~3/80 blasts is firmware behaviour, not a host bug — the net is the fix, and
  any redesign that removes or starves it re-opens the W12 fault class (RX
  dead until reflash).
  ★ The net triggers on **quiet service passes**. W15 nearly killed it
  silently by removing idle polling; the fix was to remove the *read*, not the
  *pass*. Aggregation changes the pass structure — check this explicitly.
* **IEEE PS stays ON** everywhere (W10 workaround for the fw idle RX-death
  erratum; PS was exonerated of every other charge in W12).
* **A set RD bitmap bit does not imply a waiting packet** — stale bits linger
  (~6100 such resyncs found no data). Any RX aggregation that decides how many
  slots to read from the bitmap alone will read garbage; use per-slot `RD_LEN`.

## Success criteria

1. Commands-per-frame down measurably (the un-fakeable number), with
   throughput up on silicon against the same peer.
2. Health counters no worse: `stranded`/`resyncs`/`notready`/`drainerr`.
3. QEMU gates green including `[ring]`/`[stranded]`/`[irq]`, each still
   demonstrated failing against its re-broken driver.
4. Silicon soak via `tools/wifi-soak.sh` — see below.

## Bench notes

* `tools/wifi-soak.sh` is the hardware soak harness (reflash per boot, blast,
  dump ring state on freeze). It refuses to run against a creds-less build and
  fails loudly if zero blasts ran — both guards were learned the hard way.
* **The house AP password was rotated**; those creds are dead. Use the ESP8266
  bench AP (`ESP8266TEST`, own throwaway PSK, on `/dev/cu.usbserial-0001`).
  For blast-level load you need a peer on that LAN — the Mac must join the
  bench AP (ask the user; it drops them off the house network).
* **A wiped build cache is indistinguishable from a dead card**: no blob/creds
  → the Wi-Fi path compiles out → the same `alive=` heartbeat as card-absent.
  This cost a pointless board power-cycle. Check
  `grep -E "^M2RADIO_(IW416_FW|WIFI)" build/CMakeCache.txt` and flash
  `m2_sdio_probe` (it re-emits `sdio_begin=`/`cardid=`) before blaming
  hardware. Never `cmake -B`/`rm -rf` an existing example build dir.
* Flash `LinkServer flash … load` **VCOM-free** (kill `read_port.py` first);
  sweep from `/tmp/ev` (sun_path limit); audit with `LICENSE_AUDIT_EVKB=$(pwd)`.
* Sweep baseline **102/0/0**. Read gate NAMES on a red, never just the count —
  I once piped a sweep through `tail -2` and discarded the one line that
  identified the failure.
