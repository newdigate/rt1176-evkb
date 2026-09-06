# M2Radio BT: A2DP credit-leak soak investigation (NEW-34 piece 4)

**Status:** IMPLEMENTED and QEMU-swept 2026-09-06 (instrument only, NO new gate; sweep effectively 131/131, LICENSE-AUDIT PASS, M2Radio 0c31ad5 pushed+pinned, fresh-user verified). The 30-min SILICON SOAK (both RF arms) + the flush-timeout/ESP32 escalation are PENDING the bench (plan Task 5).
**Issue:** NEW-34 "M2Radio BT: reconnect known devices + soak-test connection
resilience (range loss/recovery)", piece 4 of 5.
**Depends on:** piece 2 (the non-blocking link layer + `L2cap::creditsMin()`,
M2Radio `82e9172`). Feeds piece 5 (the full-system soak).

## 1. Goal and the reframe

Piece 4 was filed as "drops climbing ~14% over minutes — test FIRST whether a
lost Number_Of_Completed_Packets shrinks `L2cap::m_credits` (mimics air-link
starvation); log the credit floor." **That premise is largely already
resolved, and this piece is scoped accordingly.**

- The "~14% over minutes" was measured on the acid_box stream BEFORE the
  NEW-33/piece-2 **batching fix** (drain ~5 SBC frames per ACL packet →
  ~69 packets/s instead of ~344 against the 7-credit pool). The bench transcript
  now shows **drops=0 sustained over 6+ minutes**, and a diagnostic run found
  even the un-batched build holds drops=0 for 5 min under good RF. The root
  cause was credit CHURN plus RF transients, not a lost NCP.
- The credit-floor instrument already exists: piece 2 added
  `L2cap::creditsMin()` and bt_tone_test's heartbeat logs `credmin`.

So piece 4 is **investigation-first** (brainstorm decision): confirm or deny a
*residual* credit leak over a long soak, and build a fix ONLY if the soak shows
one. A clean soak legitimately closes the piece as "batching + instrument
suffice; the lost-NCP hypothesis is refuted for these conditions."

Depth (brainstorm decision): **host-side, escalate if needed.** Host-side
counters answer "is there still a problem" cheaply. They do NOT prove the
*mechanism* — a lost NCP and a not-yet-sent NCP are indistinguishable to the
firmware (both are "a credit that has not come back"); only the sink's
received-packet count versus the host's NCP-credit count can separate them.
That end-to-end reconciliation is deferred to the escalation (§6) and built only
if the host-side soak shows decay.

## 2. The instrument (`L2cap`, host-side)

Three cumulative counters plus one derived, all incremented in paths that
already run — no new hot-path work:

- **`pktsSent`** — ACL packets written in `service()` (one per credit consumed).
- **`creditsReturned`** — credits summed from Number_Of_Completed_Packets in
  `onEvent()`.
- **`starves`** — count of transitions into "credits == 0 while the TX queue is
  non-empty"; **`starveMaxMs`** — the longest continuous stretch spent at
  zero-with-work-pending (needs a millisecond clock reference — see §2.1).
- **`clampHits`** — count of times the NCP handler's `min(v, maxCredits)` clamp
  actually discards credit (the controller returned more than was outstanding —
  a double-count, the opposite failure, equally worth knowing).

Accessors: `pktsSent()`, `creditsReturned()`, `starves()`, `starveMaxMs()`,
`clampHits()`, plus a `resetCreditStats()` that zeros them (and `creditsMin`).

### 2.1 The leak signature (why these counters discriminate)

By construction `credits == maxCredits − (pktsSent − creditsReturned)`, so
`pktsSent − creditsReturned` is the outstanding-packet count and MUST stay
≤ `maxCredits` (7). Over a soak:

- **No leak:** `credmin` holds above a small floor; `starveMaxMs` stays in the
  low hundreds of ms (RF transients recover fast); `drops` flat; `clampHits`
  zero. Outstanding oscillates but never trends.
- **A leak (lost NCP):** `credmin` pinned at 0; `starveMaxMs` grows run-over-run
  (a lost NCP never recovers — this is the host-visible fingerprint that
  separates a lost NCP from ordinary backpressure, which recovers in ms);
  `drops` climb — even though `pktsSent − creditsReturned` still reads ≤ 7,
  because the firmware cannot count credits it never received.

`starveMaxMs` needs a time reference. `L2cap` currently takes no clock; add a
`tickClock(uint32_t nowMs)` the app calls each `service()` pass (or pass `now`
into `service()`), used ONLY to measure the zero-with-work stretch. Keep it
injected (host-testable), same discipline as BtLink's `tick(now)`.

## 3. Health line and soak protocol (silicon claim)

QEMU has no baud/RF pacing, so its credit dynamics are artifacts; this section
is a bench procedure whose evidence lives in a transcript, not a gate.

- **Health line.** Extend bt_tone_test's per-second heartbeat with five fields
  beside `credmin`: `sent`, `returned`, `starves`, `starve_max_ms`, `clamp`.
  That one line per second is the soak record. acid_box's `bt_hb` gets the same
  fields as a secondary witness under real synth + UI load.
- **Vehicle and sink.** bt_tone_test is the PRIMARY soak — a fixed 1 kHz tone at
  the batched ~69 packets/s is the cleanest, most reproducible media rate, no UI
  noise. The sink is the **ESP32** (`EVKB-SINK`), which stays connected
  indefinitely; the Shokz self-powers-off after 1–2 min (piece 3) and cannot
  soak. acid_box (`-DM2_BT_OUT=ON`) against the ESP32 is the SECONDARY run.
- **RF arms.** Good RF at close range (baseline) and marginal RF by distance or
  an attenuator (the provocation — a lost NCP would hide behind backpressure, so
  the marginal arm is where a leak, if any, shows).
- **Duration/cadence.** 30+ minutes per arm, heartbeat once a second (NEW-8 soak
  discipline).
- **Verdict, stated up front:** CLEAN = `credmin` ≥ a small floor, `starve_max_ms`
  in the low hundreds, `drops` flat, `sent − returned` ≤ `maxCredits`, `clamp`
  zero. LEAK = `credmin` pinned at 0, `starve_max_ms` growing run-over-run,
  `drops` climbing. Clean closes the piece (§6); a leak escalates (§6).
- Evidence recorded in `examples/audio/bt_tone_test/transcript_hw_evkb.txt` and
  `examples/display/acid_box/transcript_hw_evkb_bt.txt`.

## 4. Proving the instrument (host test — the gateable surface)

A clean soak is trustworthy ONLY if the instrument provably fires when a leak
exists — the negative arm is the whole point. The accounting is pure,
deterministic logic, so its proof is a host test, not a QEMU gate.

`bt/test/l2cap_test.cpp` grows three scenarios:
1. **Normal flow.** Send N ACL packets, return NCPs for all N → assert
   `sent == returned == N`, credits back to `maxCredits`, `credmin` correct,
   `clamp == 0`, `starves == 0` (never starved).
2. **Withheld NCP (the load-bearing negative).** Send, withhold one NCP, keep
   the TX queue fed, advance the fake clock → assert `credits` stays down by
   one, `sent − returned` reflects the shortfall, and once the pool hits zero
   with work pending, `starves` increments and `starve_max_ms` grows with the
   clock.
3. **Over-return (clamp).** Return more credits than outstanding → assert
   `clampHits` increments and `credits` does not exceed `maxCredits`.

Each pin demonstrated RED by a mutant (counter not bumped, starve not detected,
clamp not counted), on a SCRATCH COPY per the piece-1/2 discipline.

**No new QEMU gate; the sweep count stays 131.** QEMU's `[media]` path drops
~53% as a timing artifact, so any `credmin`/`starve` value there is noise —
asserting it would be theatre. The new fields ride the heartbeat for the bench;
the `[media]` gate makes no claim about them. The instrument's *correctness* is
the host test's job; the instrument's *reading* is the silicon soak's. The
existing `[media]`/`[lifecycle]`/`[reconnect]` transcripts are unaffected (the
counters are additive and default-on, printed on a heartbeat those gates already
tolerate — confirm by re-running, and re-capture only if a gate greps the
heartbeat exactly; the piece-2 gates grep prefixes, so they should be
unaffected).

## 5. Files

M2Radio: `bt/L2cap.{h,cpp}` (the four counters + `starveMaxMs` clock hook +
`resetCreditStats`), `bt/test/l2cap_test.cpp` (three scenarios).
evkb: `examples/audio/bt_tone_test/bt_tone_test.cpp` (heartbeat fields + the
`service()` clock tick), `examples/display/acid_box/acid_box.cpp` (same fields
in `bt_hb`), `evkb.cmake` (M2Radio pin), `CLAUDE.md`, memory. The silicon
transcripts are bench artifacts.

## 6. Outcomes and escalation

- **Clean (likely):** merge the instrument as a permanent soak tool; record the
  negative finding ("no credit leak over 30 min on either RF arm; batching
  suffices; lost-NCP hypothesis refuted for these conditions"). No fix built.
- **Leak:** escalate to the end-to-end reconciliation deferred in the depth
  decision — add a received-packet counter to the ESP32 sink
  (`tools/esp32-a2dp-sink`) and compare against the host's `returned`. Sink
  received > host returned ⇒ NCPs lost in the host transport (lost-NCP
  confirmed). The fix is then the A2DP-standard **Write_Automatic_Flush_Timeout**
  (HCI 0x0C28): stale media packets auto-flush, which both bounds media latency
  under RF backpressure AND returns their credits (the controller reports
  flushed packets in its NCP), possibly with a defensive credit resync. The plan
  writes this escalation down as a contingency task; it is NOT built unless the
  soak shows a leak.
- **The documented gap either way:** the credit pool has no recovery path for a
  genuinely lost NCP (recovery is NCP-only). Piece 4 does not close that gap; it
  makes it measurable.
- **Hand-off to piece 5:** `credmin`/`starve`/`sent`/`returned` become health
  assertions in the NEW-8-style soak. Piece 4 is the credit-specific probe;
  piece 5 is the full-system soak that consumes it.

## 7. Non-goals

The flush-timeout implementation (0x0C28), the ESP32 received-counter, and any
change to the credit accounting are ALL contingent on the soak and explicitly
out of scope unless it shows a leak. No new QEMU gate. Pieces 3 (Shokz
AVRCP/power-off) and 5 (soak automation) are separate.
