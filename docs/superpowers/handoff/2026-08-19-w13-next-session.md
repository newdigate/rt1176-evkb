# Continue M.2 Wi-Fi (W13: close the two W12 residuals; then DAT1 / throughput)

u-blox **M2-MAYA-W161** (IW416/SD8978) on **MIMXRT1170-EVKB RevC3**, repo
`~/Development/rt1176-evkb-m2-maya-w161`, branch **`m2-phase0-serial2`**.
Siblings pushed + pinned: **M2Radio `84c2520`**, **lwip `c6b2548`**.

**Read first:** `examples/networking/m2_throughput_test/transcript_hw_evkb.txt`
— the whole W11+W12 story, including three of my wrong conclusions and why
each was wrong. That file is the reference, not this handoff.

## What W12 settled

**Fault #5 is fixed, and it was never a firmware bug — it was ours.**
`HOST_INT_STATUS` is clear-on-read; `serviceLink()` drained only on its own
observation of `HOST_INT_UP_LD`, while four other paths (above all
`readHostResp()`'s 1 ms command-wait poll) consumed and discarded that bit.
The firmware never re-raises for an already-pending upload, so the frame
was stranded forever. Fix = sticky `m_intPending` accumulated at every read
site + a ring safety net. Measured: **300+ blasts across three soaks on one
firmware life each, zero freezes**, vs RX dead within 1-3 blasts before.

**Power save is NOT implicated in anything** (W11 said it was; that was a
confounded A/B — the arms differed in boot-freshness, not just PS). PS
should stay ON everywhere: it fixes the W10 idle erratum and costs nothing
under load. `-DM2_PS_OFF=1` exists on `m2_throughput_test` as a reusable
A/B arm; never ship it.

## The two OPEN residuals — W13's first job

1. **An unidentified layer-1 loss path.** `stranded>0` with `drainerr=0`
   on every soak (3-7 per ~100 blasts). My "these are the drain's
   by-design error-exit clears" hypothesis was REFUTED by `drainerr=0`.
   Something still loses an upload interrupt that the sticky accumulator
   does not cover; the net recovers it within ~64 ms, so the link stays
   healthy and the bug is invisible without the counter.
   **Best lead:** the review's item M5 — `readHostResp()` branches on its
   OWN fresh read rather than on `m_intPending | fresh`, so a
   `CMD_PORT_UPLD` shelved by `readDataPacket()` is recorded but never
   consumed. Same shape as the fixed bug, on the command port, where
   there is no bitmap to re-derive it from. Not live on today's call
   graph — but "not live" is exactly what was said about the data-port
   version before it cost two days.
2. **The desync variant is uncovered.** The net fires only when the
   firmware's pending upload sits at our own ring slot. If an interrupt is
   lost AND the ring positions disagree, nothing recovers it and
   `stranded` still reads 0. Closing it needs a POSITIVE check (RD_LEN
   non-zero at the target slot), not a bitmap bit — see the resync note
   below for why a bitmap bit is not trustworthy on its own.

## Numbers you will misread if you don't read this

* **`rxRingResyncs` is high by nature: ~50-230 per 10 s blast, before and
  after the fix.** I twice drew wrong conclusions from it by comparing
  ABSOLUTE counts across runs of different length (339 from a 1.5-blast
  run vs 6150 from a 110-blast run). Always normalize per blast.
* Throughput is 3-14 Mbps depending entirely on 2.4 GHz conditions. The
  W11 "5-10 Mbps" table is a floor, not a benchmark. Bus cost per frame
  (~10 SDIO commands at ~0.1 ms) is the real ceiling.

## Also open (ranked)

* **DAT1 interrupt-driven SDIO** — still the biggest structural win: kills
  the ~1 kHz idle CMD52 poll AND is the correct base for any bitmap
  amortization (W11's cache attempt regressed 2.5x precisely because the
  read-per-frame pattern was accidentally pacing the host).
* Deep TX queueing / MPA aggregation for throughput beyond ~14 Mbps.
* waitCmdResp clear-hole hardening (W10 note, never observed).

## ⚠ Credential exposure — still open, still needs the user

The W6 probe transcript committed the user's **live home Wi-Fi password**
and an **iPhone hotspot password** to the public repo. Redacted at HEAD
(W11, commit 5622b68) but **still in pushed history** — redaction does not
remove it. **Rotation at the router and phone is the only real fix.** The
user was told 2026-08-19; ask again before starting W13. A history scrub
(`git filter-repo` + force-push of a published branch) is destructive and
needs explicit instruction.

## Bench traps that cost time this session

* **A wiped build cache is indistinguishable from a dead card.** A review
  subagent ran `rm -rf build` before reconfiguring (to avoid stale-cache
  passes) and wiped `M2RADIO_IW416_FW` / `M2RADIO_WIFI_*`. The example
  then compiles its Wi-Fi path OUT and prints the same `alive=` heartbeat
  it prints with no card. I misdiagnosed this as wedged hardware and had
  the user power-cycle for nothing. **`m2_sdio_probe` re-emits its
  bring-up report periodically — flash it and read `sdio_begin=` /
  `cardid=` before blaming hardware.** Check the build cache first:
  `grep -E "^M2RADIO_(IW416_FW|WIFI)" build/CMakeCache.txt`.
* My A/B "proving" it was hardware was invalid: both arms had been wiped
  by the same subagent, so they shared the defect and differed only in the
  variable I was testing.
* The reproducer scripts live in session scratch
  (`fault5_cycles.sh`, `fault5_multi.sh`, `fault5_hunt.sh`). `fault5_hunt.sh`
  now has a VACUITY GUARD: it once reported "no freeze in 25 min / 0
  blasts (link healthy throughout)" when every boot had failed to join.
  A green result from a test that never ran — the exact class this repo's
  gate-vacuity tests exist to catch. Rebuild these into `tools/` if they
  are worth keeping.
* Flash `LinkServer flash … load` VCOM-FREE (kill `read_port.py` first);
  sweep from `/tmp/ev`; audit with `LICENSE_AUDIT_EVKB=$(pwd)`.
