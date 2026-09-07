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
