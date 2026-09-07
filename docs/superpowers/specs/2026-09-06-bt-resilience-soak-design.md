# M2Radio BT: unattended connection-resilience soak (NEW-34 piece 5)

**Status:** IMPLEMENTED and QEMU-swept 2026-09-07 — the `bt_tone_test[soak]` gate (132nd) green, sweep 132/132/0, vacuity 42/42, `LICENSE-AUDIT: PASS`, M2Radio `580f435` pushed+pinned, fresh-user verified by running `[soak]` on the fetched ELF. Review-driven corrections are folded into §2/§3 below. The 2–4 h SILICON soak (plan Task 7) is PENDING the bench.
**Issue:** NEW-34 "M2Radio BT: reconnect known devices + soak-test connection
resilience (range loss/recovery)", piece 5 of 5 — the "soak-test connection
resilience" half of the title.
**Depends on:** piece 1 (bond table + stored-key reconnect), piece 2 (the
non-blocking link lifecycle + `BtSession` auto-reconnect + `bt_link` stats),
piece 4 (the `bt_cred` credit-leak instrument). All three are SOFTWARE-DONE and
pinned; their pending SILICON claims are settled by THIS soak. **Does NOT depend
on piece 3** (the Shokz self-power-off): the soak sink is the ESP32
`EVKB-SINK`, which stays connected indefinitely and has no power-off.

## 1. Goal and shape

The NEW-8 analog for Bluetooth: a long **unattended** run that asserts the A2DP
link survives **repeated drops and reconnects** with a **flat health signature**
— no memory drift, no credit leak, no slot/handle/bond leak, every reconnect
succeeding — over hundreds of cycles. NEW-8 ran 240 min with a 2 s health line
and an all-zero health signature as the verdict, driven by a valve mechanism and
a peer; piece 5 has the same shape with the piece-2/4 fields as the signature.

Nearly all instrumentation already exists: piece 2's `bt_link` (links, lost,
reason, reconnect_ms), piece 4's `bt_cred` (sent, returned, credmin, starves,
starve_max_ms, clamp), and the heap / stack-floor figures. Piece 5 adds a soak
DRIVER, ONE structural-baseline accessor (`L2cap::freeSlots()` — `BondTable::count()`
already existed; a planning correction), one QEMU gate, and a bench protocol.

**Two brainstorm decisions:**
1. **Drops are SELF-INDUCED by the firmware** (not ESP32-induced, not manual).
   An unattended multi-hour soak needs an automatable drop source; a human
   carrying a headset out of range for hours is not a soak. The firmware forces
   the drop and lets `BtSession` reconnect, hammering the whole piece-1/2
   machinery hundreds of times. RF-timing realism (a peer that is genuinely
   unreachable for a while) is NOT reproduced here — it stays the manual
   range-loss / power-cycle bench matrix already planned under pieces 1/2.
2. **A QEMU accelerated-soak gate exists** (sweep 131 → 132) asserting the
   STRUCTURAL leak invariants over many rapid cycles — the class of bug a soak
   exists to catch (a slot never freed, a handle not reclaimed, a bond
   duplicated, credits not resetting), invisible to the piece-2 `[lifecycle]`
   gate's three legs but fatal over hundreds. Heap-over-hours and real timing
   remain honest SILICON claims; the gate does not pretend to them.

## 2. The soak driver (`M2_BT_SOAK`, bt_tone_test)

An opt-in build (default OFF; the default image and every existing gate stay
byte-identical) wrapping `bt_tone_test` with a small self-drive loop on top of
`BtSession` — ~30 lines of EXAMPLE code, no new library machinery.

- **Inducing a drop that exercises the AUTOMATIC path.** The driver must NOT
  call `session.disconnect()` / `resume()` — that is the MANUAL hook and tests
  the wrong path. Instead, every `M2_BT_SOAK_PERIOD_MS` (default 15000) while
  STREAMING, it issues a raw **HCI_Disconnect** (0x0406, reason 0x13) on the
  live handle. The Disconnection_Complete arrives as a LOSS, `BtSession` sees
  the stream drop, records it (`lost`, `reason`), goes WAITING and
  **auto-reconnects** — page, stored-key auth, L2CAP, AVDTP, resume streaming —
  exactly the resilience path a range loss triggers.
- **The cycle contract.** Each cycle: force the drop, `cycles++`, wait for
  re-STREAMING within `M2_BT_SOAK_RECONNECT_BOUND_MS` (default 30000); a cycle
  that re-streams increments `reconnects`, one that does not increments
  `fails` (and the driver keeps going — a soak records failures, it
  does not stop on the first). The tone resuming on the ESP32 after each drop
  is the audible, un-fakeable proof.
- **The health signature**, printed every 2 s (NEW-8 cadence) as a `bt_soak`
  line beside the existing `bt_link` / `bt_cred` / `bt_mem` lines: the soak
  counters `cycles`, `reconnects`, `fails`, `reconnect_ms_max`, and
  the structural baselines the soak watches for a leak — `l2_free` (L2CAP free
  channel-slot count), `handle` (the live ACL handle), `bonds` (bond-table
  count), plus `heap`/`stack_free_min`. One read-only accessor is added:
  `L2cap::freeSlots()` (`BondTable::count()` already existed).
- **The slot-leak witness is sampled at LOSS, not only at STREAMING entry**
  (review finding, 2026-09-06). `A2dpSource` calls `m_l2.begin()` unconditionally
  on every attempt, which zeroes the channel table, so `freeSlots()` at a
  STREAMING entry is always a fresh table and can never show a skipped teardown.
  `soakOnLoss()` samples it from the stream-lost callback — which fires AFTER
  `A2dpSource::tick()`'s `m_avdtp.reset(); m_l2.reset()` — so a correct teardown
  reads `MAX_CHANNELS` (5) and a skipped one reads 2: `l2_free_loss_min`. The
  STREAMING-entry baseline (`l2_free_base`, measured 2: AVDTP signalling + media
  + the outbound SDP channel held) and its re-stream minimum stay as the honest
  relative figure `l2_leak`, with `n/a` sentinels until sampled; the gate also
  pins the absolute base.
- **`retryNow()` is a build option** (`M2_BT_SOAK_RETRY_NOW`, default ON): the
  QEMU gate uses it for budget; the SILICON soak should run it OFF to exercise
  the session's own retry policy (and then needs a ~60 s reconnect bound).
- **`heap=` is vacuous on this vehicle** — bt_tone_test never touches newlib's
  arena (`mallinfo().uordblks == 0`), so the gate asserts `heap=0` EXACTLY as a
  nothing-ever-allocated tripwire; the memory-over-hours claim rests on
  `stack_free_min`, `l2_free*` and `bonds` (acid_box, which allocates via LVGL,
  is where `heap` means something).
- **The verdict shape** (NEW-8's): the invariant fields — `heap`,
  `stack_free_min`, `l2_free`, `bonds`, credit health — stay FLAT at baseline
  while the cycle counters climb in lockstep (`reconnects == cycles`) and the
  error counters — `fails`, `starves`, ncmd starvation, `clamp` —
  stay at ZERO for the whole run. One healthy signature, repeated, is the pass.

## 3. The QEMU accelerated-soak gate (`bt_tone_test[soak]`, the 132nd)

A `-DM2_BT_SOAK=ON -DM2_BT_SOAK_PERIOD_MS=2000 -DM2_BT_TARGET_NAME=FAKE-HEADSET-01`
build against a new `soak` phase in the fake peer (`hci_peer.py`): the peer
accepts the connect and streams, then on each firmware-forced disconnect handles
the teardown and re-accepts the re-page on a FRESH handle, for `N` cycles
(N = 10 — the 15–20 first written here did not fit: 10 cycles measure ~49 s
against the peer's 55 s deadline; N > ~12 will not) inside qrun's 60 s. The gate waits for a `soak_done` line.

Assertions — the structural leak invariants, each demonstrated RED by name:
- **Every cycle reconnects.** `cycles == reconnects == N`, `reconnect_fails == 0`,
  each reaching STREAMING — PEER-counted (`create_conns == N`,
  `disconnects == N`), so a stalled reconnect fails the tally, not a UART line.
  RED (demonstrated): the forced disconnect sent on a STALE handle — the peer
  refuses it, the link never drops, `reconnects=1 fails=9`.
- **No slot leak.** `l2_free` returns to its baseline after every cycle; a
  channel never freed makes it decline monotonically. RED (via the LOSS-time witness, §2 — the STREAMING-entry count cannot see it): the
  `m_l2.reset()` in the loss branch removed → `l2_free_loss_min=2`.
- **No handle leak.** Fresh handle per link; the firmware never writes ACL on a
  dead handle — the `PEER-ACL-BAD-HANDLE` tripwire reused from `[lifecycle]`.
- **No bond churn.** `bonds` stays 1 (the fake headset) across all cycles. (The planned always-insert RED is UNREACHABLE — see the dropped-RED bullet below.)
- **Credit stats RESET per cycle** (a planning correction: piece 4 established
  that QEMU credit DYNAMICS are timing noise, so `credmin` recovery is a SILICON
  claim, not gated). Gated: `bt_cred sent=` must FALL once per reconnect (the
  per-attempt `resetCreditStats()`) and stay under a run-wide ceiling; `clamp=0`
  and HCI `starved=0` (deterministic against this peer). RED: `resetCreditStats()`
  removed (`max sent=1297`).
- **Loss-time teardown witness.** `l2_free_loss_min=5` (§2). RED: the
  `m_l2.reset()` in `A2dpSource::tick()`'s loss branch removed → reads 2.
- **Media clean on every link.** The peer validates RTP/SBC per link and counts
  a link streamed only when clean (`badmedia=0`, threshold 20 packets).
- **Stragglers bounded, not assumed away.** A real controller keeps the ACL
  usable until it REPORTS Disconnection_Complete, and BtLink sets LINK_LOST only
  on that event — so the ~3–4 media packets the host legitimately sends in that
  50 ms window are COUNTED (`stale_acl`, `stale_max`) rather than logged as
  `PEER-ACL-UNKNOWN-CID`. (The first end-to-end run showed exactly 3 per drop
  with the peer tearing its channel table down at the Disconnect COMMAND — a
  peer-model artefact, fixed by deferring the teardown to the next page, as the
  lifecycle phase already does.) Gate bound `stale_max <= 8` (measured 4; a
  host that ignores the event streams the whole cycle, 100+).
- **One documented GATE GAP and one dropped RED.** The app-level `btout.end()`
  is redundant to everything the peer observes (the library tears down first) —
  the same gap `[lifecycle]` records. The planned bond-upsert RED (always-insert
  → `bonds=4`) was DROPPED as UNREACHABLE: `upsert()` has one call site (the
  Link_Key_Notification handler) and the peer notifies exactly one key per run,
  so the live bond-churn pin is the peer-counted `notified=1` equality
  (`links == key_ok + notified`), not `bonds=1` alone.

Vacuity gains the absent-capture and fewer-than-N-cycles negatives. The gate
proves the LIFECYCLE SURVIVES REPETITION deterministically; heap-over-hours,
reconnect latency and RF are silicon.

## 4. The silicon soak (bench, unattended)

The `M2_BT_SOAK` build at the realistic cadence (15 s) streaming to the ESP32
`EVKB-SINK`, unattended, **2–4 hours** (NEW-8 discipline) — at 15 s per cycle,
500–1000 real reconnect cycles. Console read by `tools/rt1170-console.py`, the
`bt_soak` line every 2 s as the record.

- **Verdict:** the flat health signature of §2 over the whole run.
- **What it proves that QEMU cannot:** memory flat over hours (no slow heap /
  stack drift); no ACCUMULATING credit leak over hundreds of REAL cycles (this
  IS piece 4's credit-leak soak, folded in — `credmin` / `starve_max_ms`
  watched run-long); the real `reconnect_ms` distribution; the machinery
  surviving real timing.
- **Un-fakeable corroboration:** the ESP32's own play state / received count
  tracks the firmware's cycle count; the tone is audible after reconnects.
- **This is the umbrella silicon validation for the programme.** Pieces 1
  (runs A–E), 2 (the reconnect machinery, R1–R5) and 4 (the credit soak) settle
  their pending silicon claims in this one run. The MANUAL range-loss and
  power-cycle matrix (a human moving the headset, RF-realistic) stays separate
  under pieces 1/2.
- **Bench discipline** (from prior sessions): flash with the VCOM detached
  (`LinkServer flash … load` → `verify`), free-run with SW4, then attach the
  reader; no unattended reset loops (the DAP wedge); a full board power-cycle
  clears a wedged `LinkServer run` connect.
- Evidence: `examples/audio/bt_tone_test/transcript_hw_evkb.txt` (a
  `SOAK` section: start/end signatures, the cycle tally, any anomaly lines).

## 5. Verification and acceptance

- **Gateable:** the `[soak]` gate (§3), its RED demonstrations, the vacuity
  negatives; a host test for `freeSlots()` (A4: begin → connect → CONFIG → OPEN →
  DISC_REQ/CLOSED → reuse → reset; a count-OPEN-as-reusable mutant that survived
  the first version is what added the CONFIG/OPEN legs), RED-pinned.
  Sweep 131 → 132, `LICENSE-AUDIT: PASS`, M2Radio pushed + pinned, fresh-user
  verified by RUNNING the `[soak]` gate against the fetched ELF, every
  bt-linking gate ELF rebuilt fresh (the NEW-36 freshness discipline).
- **Silicon acceptance:** one 2–4 h run with the flat signature end to end and
  `reconnects == cycles`, `reconnect_fails == 0`. That transcript closes piece 5
  AND the programme's silicon evidence for pieces 1/2/4.

## 6. Non-goals

No ESP32-induced drops (declined for the self-induced driver); no manual
RF-realistic range-loss matrix here (pieces 1/2 bench work); no acid_box soak as
a REQUIREMENT (bt_tone_test is the vehicle; acid_box `-DM2_BT_OUT=ON` is an
optional secondary witness); no piece-3 dependency; no new library machinery
beyond the one accessor — `BtSession` already does every reconnect. Piece 3
(Shokz AVRCP) closes separately on its own capture branch.

## 7. Files

M2Radio: `bt/L2cap.h` (`freeSlots()`), `bt/test/l2cap_test.cpp`. evkb:
`examples/audio/bt_tone_test/{bt_tone_test.cpp, CMakeLists.txt}` (the
`M2_BT_SOAK` driver + `bt_soak` line), `run_qemu_soak.sh` +
`transcript_qemu_soak.txt`, `examples/networking/m2_hci_probe/hci_peer.py` (the
`soak` phase), `tools/gate-vacuity.test.sh`, `evkb.cmake` pin, `CLAUDE.md`,
memory. The silicon transcript is a bench artifact.

## 8. The silicon soak, as run (2026-09-07)

**The sink changed, and the reason is measured.** The soak was started against the
ESP32 `EVKB-SINK` as §4 planned and hit two of its limitations inside the first
cycle: (a) after a HOST-initiated disconnect it stopped answering pages for
15+ min (no Connection_Complete / status 0x08, then 0x04) until reset — re-arming
its scan mode on disconnect (`tools/esp32-a2dp-sink`, now done) did not change
that; (b) **it forgets the link key on every disconnect** — a page that DID
connect was then refused with `Authentication_Complete 0x24` with no reset in
between (its own console proves it), so every cycle could only recover by erasing
the bond and re-pairing by PIN from a fresh inquiry, 1–2 min, always outside the
bound. A stored-key reconnect cannot be soaked against it. Two things came out
of that detour: `BtLink` now erases the bond on 0x24 as well as 0x05/0x06 (M2Radio
`9d3da4c`, host-tested, RED-pinned — a peer that forgot us presents as "LMP PDU
not allowed", and the old rung looped on the stale key every 10 s forever), and a
bench trap: **opening the ESP32's serial port resets it** (the auto-reset line,
regardless of the DTR/RTS flags) — hold the port open once for the whole session
rather than reading it repeatedly, or every read costs a bond.
**The soak therefore runs against the Shokz OpenMove** (SSP, stored key, the real
headset piece 3 characterised; `-DM2_BT_TARGET_NAME=Shokz`, no legacy PIN,
15 s period, `retry_now` OFF, 60 s bound; the ESP32 off). Started 13:55:57.
**Early shape (first ~25 min, 39 cycles):** every drop reconnects by page with the
stored key; p50 reconnect 12.0 s (the session's 10 s retry timer + ~2 s connect),
p90 38.8 s, worst 92.5 s; `l2_free_loss_min=5`, `l2_leak=0`, `bonds=1`, stack floor
flat, `heap=0` (this vehicle never allocates). **Finding:** ~1 in 5 reconnect
attempts reaches the ACL + stored-key auth and then **stalls in AVDTP** for the
15 s deadline (`a2dp=avdtp_failed`), our side disconnects and the next attempt
usually streams; two runs of three in a row crossed the 60 s bound and are the
soak's only counted failures. The AVRCP exchange completes even on those attempts.
The stage it stalls at is not in the soak log (the ACL trace is off by design) —
a short traced run to catch one is the follow-up. Final numbers in §8.1.

### 8.1 Final numbers (run stopped at 2 h 02 min)

13:55:57 → 15:57:58, 3641 `bt_soak` lines (7282 s), Shokz OpenMove, 15 s period,
`retry_now` OFF, 60 s bound. Log: `p5-soak3-final.log` (44609 lines); the
transcript's SOAK section carries the quoted lines.

**Last signature:** `bt_soak cycles=167 reconnects=146 fails=20
reconnect_ms_max=41431 l2_free=5 l2_free_base=2 l2_free_min=2 l2_free_loss_min=5
l2_leak=0 bonds=1 heap=0 stack_free_min=207104`; `bt_link links=168 lost=168`;
`bt_cred sent=960 returned=960 starves=0 starve_max_ms=0 clamp=0` on the last
link; `soak_drop_status` errors 0; every loss event reason 0x16 (ours) except one
0x13 (the headset's, the single `lost`).

**The structural signature is FLAT end to end** — the thing piece 5 exists to
watch: `l2_free_loss_min=5` on all 167 losses (the teardown ran every time),
`l2_leak=0`, one bond, `heap=0`, the stack floor unchanged over two hours,
`credmin=3` while streaming with `sent == returned` on every link and
`starves=0`/`clamp=0` (piece 4's leak fingerprint absent over 168 real links),
and 274 ACL links authenticated with the STORED key across the run (pieces 1/2's
silicon claims — a real headset accepting the stored key out of pairing mode,
both directions: 4 of the 274 were the headset paging us, accepted as slave with
its config adopted).

**The functional acceptance of §5 (`reconnects == cycles`, `fails == 0`) is NOT
MET**, and the reason is a single class with a TREND:

| window | attempts | ok | avdtp_failed | stall rate | soak_fail |
|---|---|---|---|---|---|
| 0–30 min | 66 | 51 | 15 | 23 % | 3 |
| 30–60 min | 68 | 47 | 21 | 31 % | 3 |
| 60–90 min | 67 | 37 | 29 | 43 % | 7 |
| 90–120 min | 69 | 30 | 39 | 57 % | 7 |

274 attempts: 168 streamed (median 0.3 s from `connect=ok` to `a2dp=ok`), 105
stalled in AVDTP to the full 15 s deadline (`a2dp=avdtp_failed`, then our
disconnect 0x16), 1 lost. Longest run of consecutive stalls: 9. Drop-to-stream
latency (n=169): p50 12.0 s, p90 65.7 s, max 240.7 s. In **90 of the 105 stalls
the headset opened AVCTP and completed the AVRCP exchange** on that same link, so
the ACL, L2CAP and the stored-key auth were healthy and the stall is inside AVDTP
signalling — which side, and at which step, the soak log cannot say (the ACL
trace is off by design). Three of the 168 streamed links (each adjacent to one of
the headset's inbound pages) stalled on MEDIA at packet 145 (`hw=64`, ~4000
drops) before the next drop.

**Verdict:** piece 5's own machinery holds — every host-side invariant it was
built to watch is flat over 167 real drop/reconnect cycles, and pieces 1/2/4's
pending silicon claims are settled by this run. The programme's reconnect is NOT
yet "flat": the AVDTP-stall rate GROWS over the run, from ~1 in 4 to more than 1
in 2, and nothing on the host side moves with it. Follow-up (bench, next
session): (a) a headset power-cycle CONTROL — restart the soak with the Shokz
power-cycled and the board NOT reset; if the rate returns to ~20 % the
accumulating state is in the headset; (b) a board-reset control for the converse;
(c) a short `M2_BT_ACL_TRACE` run (media skipped, so it is safe) to name the
AVDTP step a stall sits at. Until then piece 5 is *software done, silicon run,
acceptance open on the AVDTP-stall class*.

### 8.2 The two controls (2026-09-07, 16:03–17:33, same board, never reflashed)

Run as the follow-up §8.1 named, in the order that keeps each arm clean. The
board kept soaking untouched after §8.1's reader was stopped (cycles 167 → 186
by the time the first marker was set); the log is `p5-soak4-controls-final.log`
with markers at heartbeat 345 (headset power-cycle, 16:15:14) and 1760 (SW4
board reset, 17:02:40 — a full firmware re-download to the IW416, so the
CONTROLLER is reset too, and the bond reloaded from EEPROM, `bonds_boot=1`).

| arm | condition | minutes | attempts | ok | avdtp_failed | stall |
|---|---|---|---|---|---|---|
| pre-cycle | 2 h 10 min into the soak, nothing reset | 11 | 25 | 10 | 15 | **60 %** |
| headset power-cycled | board + controller untouched | 47 | 110 | 73 | 36 | **33 %** |
| board reset (SW4) | headset untouched, 47 min after its cycle | 30 | 70 | 37 | 33 | **47 %** |

Ten-minute windows after the headset cycle, running straight through the board
reset: 39, 43, 29, 33, 27 | 39, 41, 60 %. Every host invariant flat throughout
(`l2_free_loss_min=5`, `l2_leak=0`, `bonds=1`, stack floor 207104, credits
`1067 == 1067`, `starves=0`).

**Reading:** the headset power-cycle is the only intervention that moved the
rate (60 → 33 %), and it did not restore the run's opening 23 %; the board reset —
fresh host stack AND fresh controller firmware — moved nothing (47 % and the
windows climbing again, 27 → 39 → 41 → 60 % over the ninety minutes since the
headset's cycle, the same slope as §8.1's). So the accumulating state that makes
AVDTP stall is in the Shokz, it builds with the headset's uptime or connection
count, and nothing this host owns contributes. Two caveats: the arms are 25–110
attempts each, so a single window is noise and only the arm totals and the slope
are claims; and a soak at one forced drop per 15 s is itself an unusual stressor
for a consumer headset — the trend may be its response to that, not a defect a
user would meet. **What remains** is naming the AVDTP step a stall sits at (the
short `M2_BT_ACL_TRACE` run), which decides whether a host-side mitigation exists
(a different retry shape, a longer AVDTP deadline, an AVDTP ABORT before
disconnecting) or whether piece 5's acceptance should be restated as
"structural signature flat + reconnects within bound against a peer that is
itself healthy".
