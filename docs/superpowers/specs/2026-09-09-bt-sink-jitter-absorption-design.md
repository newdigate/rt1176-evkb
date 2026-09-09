# A2DP sink: absorb the phone's delivery jitter (NEW-42)

Design, brainstormed and approved 2026-09-09.  Follow-up to NEW-41 (`2026-09-08-bt-a2dp-sink-design.md`),
whose audible acceptance was met on the iPhone bench and whose `under=0 over=0` counter target was not.
Scope agreed: **A** (the jitter work) and **B** (the `Avrcp::unsupported()` miscount).  **C**, a runtime
pairing mode replacing the `M2_BT_FORGET_BONDS=ON` reflash, is deliberately NOT in this spec -- it is a
user-facing feature with its own trigger/timeout/feedback questions and deserves its own design and gate.

## 1. What the bench actually measured, and what it means

Source: `examples/audio/bt_sink_test/transcript_hw_evkb.txt`, run 3 (10-min window 12:38-12:48, music,
one pause/resume), and NEW-41's spec section 8.  Reported there: `under +103 / over +89` in paired bursts
of 11-17 every ~2 min, the servo hunting 25-96 ppm around ~65 with instantaneous `fill` 11-15 against
`TARGET` 8, and ~27 underruns while the ring first fills.

The issue's stated hypothesis was "a 35-45 ms delivery gap then a catch-up burst overflowing the ring --
jitter, not drift".  Re-reading the run's per-heartbeat deltas gives a sharper and materially different
account, in three facts:

1. **One RTP packet delivers EIGHT audio blocks at once.**  `frames/pkts` = 5913/742 = 7.97 throughout the
   run.  One SBC frame (16 blocks x 8 subbands) is exactly one Audio-library block, so a packet is 8 blocks
   = 23.2 ms of audio arriving in a single `onMedia()` call.  `RING 16` therefore holds exactly **two
   packets**, and `TARGET 8` is **one**.  The ring's operating point is a sawtooth of 8 blocks
   peak-to-peak, and the servo drives its MEAN to `TARGET`.

2. **The P-only servo's standing offset moved the operating point to the ceiling.**  The offset is
   `drift/kp` by construction; at the measured trim of ~90 ppm and `kp` = 40 ppm/block that is **2.3
   blocks**, so the mean parked at ~10.3 and the sawtooth peaked at ~14.3 with the ceiling at 16.  `servo.h`
   calls that offset "small, bounded and silent"; it is silent only in a ring with room, and this one has
   none.  The sampled `fill` of 11-14 is that sawtooth read at a random phase.

3. **Every dropped frame becomes an underrun, and the deficit is PERMANENT at the servo's timescale.**
   `pushFrame` drops the new frame when the ring is full; that audio can never be played, so over any
   window `under ~ over` by conservation, not by coincidence.  Worse: after a gap or a drop burst of G
   blocks the ring is G blocks short and nothing refills it -- the source keeps sending at its own rate,
   and only the servo's 72 s closed-loop trim claws the fill back.  A 45 ms gap costs 15 blocks and leaves
   the ring living on the edge for a minute.

The per-30 s deltas bear all three out:

```
dpkts=1292 dunder=2    dover=1    fill=14  trim=93       <- baseline, ring near the ceiling
dpkts=1292 dunder=36   dover=35   fill=13  trim=93       <- a drop burst: under == over
dpkts=1292 dunder=15   dover=15   fill=12  trim=55
dpkts=1292 dunder=17   dover=16   fill=12  trim=58
dpkts=1292 dunder=23   dover=21   fill=14  trim=93
dpkts=1291 dunder=0    dover=0    fill=5   trim=17       <- the run's OWN CONTROL: four consecutive
dpkts=1292 dunder=3    dover=1    fill=6   trim=57          windows with the servo parked LOW, and the
dpkts=1292 dunder=0    dover=0    fill=6   trim=23          errors simply stop
dpkts=1292 dunder=0    dover=0    fill=6   trim=17
```

Four consecutive 30 s windows at `fill` 5-6 ran `dunder=0 dover=0`, while every window at 12-14 shows
bursts.  That is a natural experiment already in the committed data, and it is the strongest single piece
of evidence here: **the binding constraint is headroom and recovery, not the loop.**

The residual is then `under - over` ~ 2-3 per 30 s with `over` 0-1: about one genuine missing-packet event
every 10-15 s, which is the "single underrun every ~10 s" the issue records as unidentified.  Naming it is
the instrument's job (section 3), not this spec's to guess.

## 2. The change

All of A is example-local (`examples/audio/bt_sink_test/`); only B touches M2Radio.

| | now | after |
|---|---|---|
| `RING` | 16 (2 packets, 46 ms) | **32** (4 packets, 93 ms) |
| `TARGET` | 8 (1 packet, 23 ms) | **16 provisional** (46 ms) -- SIZED BY THE CONTROL ARM, section 4 |
| START | `update()` pops from an empty ring | **pre-fill**: silence, uncounted, until `fill == TARGET`, then `servo_recentre()` |
| ring dry mid-stream | one 2.9 ms tick, margin never restored | **re-prime**: ONE counted underrun, silence to `TARGET`, `servo_recentre()`, `reprimes++` |
| `servo.h` | -- | **UNCHANGED** (section 6 records what was considered and deferred) |

Sizing, from section 1's three facts: the ring must hold one packet of burst above the operating point
(8), the largest gap we intend to survive below it (~16 blocks = 46 ms), and the servo's standing offset
(+-2.5 blocks at +-100 ppm).  `RING 32 / TARGET 16` gives 16 blocks of headroom above and 16 below, so an
arriving packet can never find fewer than 8 free slots at the operating point.  `over = 0` therefore
becomes a claim about the DESIGN rather than about the phone, which is why section 5 makes it a hard
criterion.

Cost: the ring is `RING * 2 ch * 128 * 2 B`, so `btin` grows 10,692 -> ~18,884 B in `.bss` (DTCM);
`.bss` 36,828 -> ~45,020 of a 512 KB region.  Not a constraint.

`bt_sink_test.cpp` already derives the DelayReport from `TARGET`
(`TARGET * AUDIO_BLOCK_SAMPLES * 10000 / 44100`), so it becomes 464 (46.4 ms) with no code change --
inside the peer's 1..20000 check, and the gate greps the line's presence, not its value.  The phone uses
that figure for lip sync, so reporting the real depth is what keeps the added latency honest.

### Pre-fill and re-prime, precisely

* `begin()` sets `m_priming = true`.  While priming, `update()` transmits silence, counts NO underrun and
  steps no servo.  When `fill >= TARGET` it clears the flag, calls `servo_recentre()` and pops normally.
  Nothing buffered is discarded: the first block played is the first block decoded.
* Mid-stream, when a live un-held stream finds the ring empty, `update()` counts **one** underrun,
  increments `reprimes`, and re-enters priming.  Subsequent silent blocks are not counted -- one dropout is
  one underrun, so `under` stays a count of EVENTS rather than of silent blocks, which is what makes the
  section 5 bound `under <= reprimes + 2` meaningful.
* `hold()` (AVDTP SUSPEND) is unchanged and takes precedence: a held stream neither counts nor primes.  A
  RESUME finds the ring as SUSPEND left it and primes only if it is empty.
* `end()` clears priming with the rest of the state; the trim still holds.

## 3. The instrument

One new heartbeat line, printed beside `bt_sink`:

```
bt_jit gapmax_ms=N g30=N g50=N g80=N g120=N gbig=N fillmin=N fillmax=N trimlo=N trimhi=N overev=N reprimes=N prime_ms=N
```

* `gap*` -- inter-packet interval measured in `onMedia()` with `micros()`, bucketed <=30 / <=50 / <=80 /
  <=120 / >120 ms against a 23.2 ms nominal period, plus the running max.  This turns "a 35-45 ms delivery
  gap" from an inference into a distribution, and it is the distribution -- not this spec's estimate --
  that sizes `TARGET`.
* `fillmin` / `fillmax` -- sampled in `update()`, because the SAI ISR is the only place that sees EVERY
  block.  `bt_sink fill=` is a single random-phase sample of an 8-block sawtooth, which is exactly why run
  3's 11-14 never revealed how close to 16 the ring got.
* `trimlo` / `trimhi` -- the "is the trim settled" question as a number rather than an adjective.
* `overev` -- overrun EVENTS: a run of consecutive drops counts once, so a burst of 8 lost frames reads as
  one failure of headroom rather than eight independent ones.
* `reprimes`, `prime_ms` -- how often the buffer had to rebuild, and how long the one at START took.

**All cumulative since `begin()`, not per-heartbeat-window.**  Deliberate: the committed transcript samples
every 30th heartbeat, so a per-window max would be invisible in 29 of 30 windows -- and an extreme that is
never printed reads exactly like an extreme that never happened.

Cost is two compares per block in the ISR and one `micros()` per packet in main context.

## 4. Verification

### 4.1 Host tests -- every change RED-pinned before it is trusted

`examples/audio/bt_sink_test/tests/run.sh` (the real node source, the real `SbcDecoder`, ASan/UBSan under
clang) gains:

* **pre-fill** -- after `begin()`, `update()` emits silence and counts no underrun until `fill == TARGET`;
  the first popped block is the first DECODED block.  RED against: prime removed (the start-up burst
  returns); a prime that discards what it buffered (wrong first block).
* **re-prime** -- drive the ring dry mid-stream: exactly ONE underrun, silence to `TARGET`,
  `reprimes == 1`, the servo filter recentred.  RED against: an underrun counted per silent block; a
  resume below `TARGET`.
* **headroom** -- three packets back to back (24 blocks) must not overrun at `RING 32` and MUST at 16.
  This is the arm that pins the claim of section 2 rather than the code that implements it.
* **instrument** -- a scripted arrival schedule with known inter-packet times must land in the right
  buckets with the right `gapmax_ms`; `fillmin`/`fillmax` must equal the true extremes of a scripted
  sequence; a burst of 8 drops must read `overev=1 over=8`.  RED against a bucket edge off by one and
  against extremes latched at their init values.
* `servo_test.c` is unchanged -- `servo.h` is unchanged.

M2Radio `bt/test/avrcp_test.cpp` gains: `SetAbsoluteVolume` and `GetCapabilities` increment `answered` and
never `unsupported`; an unknown PDU increments `unsupported`.  RED against today's code.

### 4.2 Gate 139, extended in place -- and what QEMU cannot say

`run_qemu.sh`'s own header already records that `update()` runs on QEMU's schedule rather than at
44100/128 Hz.  It follows that **every CONSUME-side instrument is exactly as fictional there as `under=`
is**: `reprimes`, `fillmin`, `fillmax`, `trimlo`, `trimhi` and `prime_ms` are SILICON claims and the gate
must not assert them.  With the peer pacing at 100 ms and a fictional consume clock the sink would
re-prime dozens of times per run; a gate asserting `reprimes=1` would be asserting a fiction, and a
fiction that happened to hold would be worse than no assertion.

What is real in QEMU is the ARRIVAL side, because it is driven entirely by the peer's pacing -- a number
the firmware has no way to know:

* the peer's `source` phase gains **one scripted ~400 ms pause** mid-stream (safe: the H4 desynchronisation
  documented in that gate's header came from pacing too FAST, not from pausing);
* the gate asserts the histogram reads it back -- the bulk of intervals in the 100 ms bucket, exactly one
  entry in `gbig`, `gapmax_ms` ~ 400.  Without the injected gap every interval is 100 ms and only one
  bucket is ever exercised, so the gap is what makes the instrument's bucket discrimination testable at
  all;
* `primed=1` -- the prime completed;
* unchanged and provably unaffected: `crc200` (computed at DECODE time in `pushFrame`, so a gap that drops
  no frame cannot move it), `over=0`, `bad=0`, the decoded-level band, the DelayReport, the AVRCP
  assertions.

Both new assertions DEMONSTRATED RED by name before they are trusted (a stubbed instrument gives the wrong
histogram; the injected gap removed gives `gbig=0`).  `tools/gate-vacuity.test.sh` gains negatives: a
capture carrying a perfect `bt_jit` line but no stream must still fail, and the card-absent shape must
still fail.

Gate count stays **139**.

### 4.3 The bench -- a controlled A/B, one session

Arms are CMake cache values (`BT_SINK_RING`, `BT_SINK_TARGET`, `BT_SINK_PREFILL`) with the gate always
building the defaults, so both arms come from one source tree.  Recipe per CLAUDE.md: a `build-bench` dir
with the real `uartIW416_bt.bin`, `M2_BT_UART_DNLD`/`WAKE_PULSE`/`RTS_FLOW=ON` `RTS_WATER=1`
`FAST_BAUD=ON 3000000`; kill any `rt1170-console.py` before `LinkServer flash ... load
build-bench/bt_sink_test.hex`, then `verify`; attach `tools/rt1170-console.py` before SW4; strip the
capture with `tr -d '\r\000'` before grepping.  `M2_BT_FORGET_BONDS=OFF` -- the iPhone is bonded.

1. **Control arm** (`16 / 8 / prefill off`) with the instruments: 10 min of iPhone music.  Two purposes,
   and the second is the important one -- it measures the phone's real gap distribution, AND it is the only
   thing that proves the instrument fires at all, since a clean reading on the change arm is equally
   consistent with a gap counter that never triggers.  Expected to reproduce run 3's counters.
2. **Size `TARGET`** from that histogram.  16 is an estimate from a 35-45 ms figure, not a constant; if the
   measured distribution says otherwise the change arm uses what the data says, and the spec records the
   number that was measured rather than the one that was guessed.
3. **Change arm**: 10 min of iPhone music against section 5.

### 4.4 Close-out

M2Radio pin bumped for B -> **every M2Radio-linking gate ELF rebuilt before the sweep** (the NEW-36
freshness discipline: an ELF built against the old library boots perfectly, so an unrebuilt sweep passes
VACUOUSLY) -> fresh-user `-DEVKB_FORCE_FETCH=ON` verified by RUNNING the sink gate against the
GitHub-fetched ELF -> full sweep, target 139/139/0 -> vacuity suite -> `LICENSE-AUDIT` AFTER the sweep,
never concurrently -> spec section 7 written from the capture, `transcript_hw_evkb.txt` extended,
`CLAUDE.md`, memory, Linear.

## 5. Acceptance

Silicon, change arm, a 10-min iPhone window with music:

* **`over = 0`.**  Hard.  With `RING 32 / TARGET 16` an arriving packet can never find fewer than 8 free
  slots at the operating point, so a drop means the design is wrong -- not that the phone misbehaved.
* **`under <= reprimes + 2`.**  Each genuine dropout costs exactly one counted underrun and one re-prime;
  anything beyond that is unexplained and must be explained rather than tuned away.
* **`fillmin >= TARGET/4` and `fillmax <= RING - 4`** -- real margin at both ends, measured by the ISR
  rather than sampled.  At the provisional `RING 32 / TARGET 16` that is `fillmin >= 4`, `fillmax <= 28`;
  the bounds move with `TARGET` if step 2 of section 4.3 resizes it, since they express margin, not
  absolute depth.
* **One `prime_ms` ~ `TARGET * 2.9 ms` at START and no start-up underrun burst** (run 3: ~27).  The START
  prime is NOT a re-prime and does not appear in `reprimes`, so the bound above counts only mid-stream
  rebuffers.
* **No audible change by ear**, and the pause/resume, range-loss and stored-key-reconnect behaviours of
  NEW-41 unchanged.
* **The trim's range REPORTED, not bounded** -- `trimlo`/`trimhi` feed section 6's decision and are not a
  pass/fail criterion in this spec.

Deliberately NOT `under = 0 over = 0` as NEW-41 wrote it: if the phone genuinely gaps past any sane buffer
then that target is unreachable, and the only way to "pass" it is more latency.  A bound that can only be
met by making the product worse is not a bound worth keeping.

If the control arm's histogram shows routine gaps beyond a sane buffer, the right outcome is to RECORD
that -- the phone's delivery is then the limit -- not to grow `TARGET` until the numbers look nice.

## 6. Considered and deferred

* **Servo option 2 -- control on windowed MIN fill** instead of instantaneous fill.  Classic jitter-buffer
  practice: the minimum over a window is the true headroom.  Deferred because an EMA of the sawtooth
  already converges to its mean and drives min and max symmetrically about `TARGET`, which is what a
  centred buffer wants.
* **Servo option 3 -- a deadband, output quantisation, or a PI form.**  Deferred, and the reasoning is
  worth keeping: the observed hunt is `trim = 40 * (EMA_fill - 8)` with the EMA swinging ~+-0.9 blocks, and
  at tau = 1 s the 8-block packet sawtooth is attenuated to ~+-0.09 blocks -- so that wobble is the DROP
  BURSTS and the gaps, not the sawtooth.  Removing the drops should remove most of it.  A deadband on a P
  controller widens the standing offset rather than removing it; only an integrator removes it, and
  `servo.h` records why an integrator on a link that stalls and resumes is a wind-up hazard.  `trimlo` /
  `trimhi` from the change arm is the measurement that decides whether anything is left to fix.
* **A fill histogram** beside the gap histogram.  `fillmin`/`fillmax` answer the headroom question; a
  distribution would answer a question nobody has asked yet.
* **Threshold re-prime** -- rebuffer only after the ring has been dry for more than N consecutive blocks,
  so a single missing packet stays a 2.9 ms tick and only a real dropout costs a `TARGET`-block silence.
  Better behaviour, one more tunable to justify; `reprimes` and `gapmax_ms` from the bench are what say
  whether it is worth adding.
* **A sample-domain ring** instead of a block ring, removing the 8-block arrival quantisation from the
  ring's accounting.  A redesign, not justified by the evidence: the block ring is fine once it is deep
  enough.
* **C, the runtime pairing mode.**  Out of scope by agreement; its own spec.

## 7. Silicon: the bench

Written from the capture after section 4.3 runs.  Both arms' `bt_jit` lines, the measured `TARGET`, the
acceptance verdict item by item, and anything refuted.

## 8. Files (expected)

evkb: `examples/audio/bt_sink_test/{AudioInputBluetooth.h,AudioInputBluetooth.cpp,bt_sink_test.cpp,
CMakeLists.txt,run_qemu.sh,tests/node_test.cpp,transcript_hw_evkb.txt,transcript_qemu.txt}`,
`examples/networking/m2_hci_probe/hci_peer.py` (the scripted gap in the `source` phase),
`tools/gate-vacuity.test.sh`, `evkb.cmake` (M2Radio pin), `CLAUDE.md`, memory, this spec.
M2Radio: `bt/Avrcp.{h,cpp}`, `bt/test/avrcp_test.cpp`.
