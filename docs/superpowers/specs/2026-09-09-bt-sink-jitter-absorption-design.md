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

**All cumulative for the RUN, not per-heartbeat-window.**  Deliberate: the committed transcript samples every
30th heartbeat, so a per-window max would be invisible in 29 of 30 windows -- and an extreme that is never
printed reads exactly like an extreme that never happened.

★ **Corrected during execution (2026-09-09).**  This section first said "cumulative since `begin()`", and that
is WRONG on this node: `begin()` runs on every stream START, so a reconnect would have printed `overev=0` beside
`over=89` -- an impossible pair -- and made a 30 s heartbeat delta jump backwards, which is exactly how section 1
read the bench's run 3.  The tallies and extremes are therefore LIFETIME, like the `m_over`/`m_under`/`m_pkts`
they are printed beside, and `begin()` resets only the two FLAGS whose staleness produces a WRONG rather than a
stale reading: `m_haveRx` (which would otherwise time the first packet of the new stream against the last packet
of the old one -- an interval of seconds, landing in `>120 ms` and pinning `gapMaxUs` at something that is not
jitter) and `m_inOverrun`.  CLAUDE.md already records this footgun class from L2cap's `l2frag`, which does reset
per attempt and needs a warning saying so; a second one was not worth having.

★ **A SUSPEND is not a delivery gap.**  `hold(false)` clears `m_haveRx` on the resume edge, so a deliberate pause
starts a fresh interval.  Without it a 30 s pause measured `gapmax_ms=30023` against `23` while streaming -- and
since the iPhone bench exercises pause/resume, and this distribution is what SIZES `TARGET`, the pause would have
corrupted the one number the task exists to produce.  The node already applies the same principle to the underrun
counter ("a suspended source is not an underrun").

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

What is real in QEMU is the ARRIVAL side -- but only its COUNTS, not its magnitudes, and that was measured
rather than assumed.

★ **The guest's clock makes every gap MAGNITUDE a fiction too (measured 2026-09-09, three runs).**  The fake
peer's own log proves it never pauses (`PEER-SOURCE-PROGRESS elapsed=5.2 / 10.2 / 15.2` for packets 50/100/150,
exactly linear at its 100 ms pace), yet the guest measures one ~3.14 s inter-arrival every run --
`gapmax_ms` 3142 / 3143 / 3142.  The guest's `millis()` runs slow against wall time (about 14 packets per guest
second against the peer's 10 per wall second) and `micros()` then takes a single step of the accumulated lag.
So `gbig` carries three or four entries with NO pause injected, and varies run to run.  This REFUTES two of the
three assertions this section originally planned -- "exactly one entry in `gbig`" and "`gapmax_ms` ~ 400" --
and it retires the idea of scripting a gap in the peer at all: a 400 ms pause cannot be distinguished from a
3.14 s artefact, so the gap would have bought nothing and would have changed a peer file three other gates
share.

What survives is what the peer's pacing determines and the clock cannot distort:

* **the interval COUNT** -- the five buckets must sum to exactly `pkts - 1` (149), because every accepted
  packet after the first records exactly one interval.  Measured 149 on all three runs.  A dead histogram
  sums to 0; a double-count sums to 298.
* **where the bulk lands** -- `g80 + g120` (the 50-120 ms band) must carry at least **90** of the 149, since
  the peer paces at 100 ms.  A stubbed `m_gap[0]++` puts all 149 in `g30` and scores 0.
  ★ **The band rather than `g120` alone, and the floor at 90 rather than 120: BOTH are measurements, and the
  second one corrected the first.**  `g120` alone reads 138 / 140 / 142 idle but 122 / 124 / 125 under eight CPU
  spinners -- a margin of two -- because the same guest-clock lag that invents the 3.14 s outlier makes an
  ordinary 100 ms interval measure SHORT.  Widening to the band was the first fix and was NOT ENOUGH: a review
  re-ran it under the same eight spinners and it went **RED 2 of 5**, reading 118 / 119 / 124 / 125 / 135
  against 144-145 idle.  The dominant sink under load is `gbig` (4 idle -> 14..26), which is deliberately
  OUTSIDE the band because the artefact lives there -- so the band absorbs the smaller half of the drift and
  not the larger.  A floor of 120 would have joined this tree's documented load-sensitivity class by
  construction, on a gate that has never been in it.  90 leaves 28 of headroom under the worst measured reading
  and is still exactly 0 against the stub.
  ★ **What it does NOT catch, stated so it is not read as stronger than it is:** a constant `m_gap[3]++` scores
  149 in the band and passes.  The band cannot separate 50-80 ms from 80-120 ms; the BUCKET EDGES are pinned on
  the host instead (below).
* **`primed=1`** -- the START pre-fill completed.
* **`bt_avrcp notif=1 ans=1 unsup=0`** -- the source phase sends exactly two AV/C commands, one
  RegisterNotification and one SetAbsoluteVolume.  `unsup=0` is where the old firmware printed `unsup=1`, so
  this pins item B on the wire.  (`ans=1`, not 2: `GetCapabilities` belongs to the `[media]` phase, which is
  `bt_tone_test`'s peer, not this one.)

The BUCKET EDGES are not gated, deliberately: with the magnitudes distorted a gate cannot pin them honestly.
They are pinned exactly on the host instead, in `node_test` case 11, which drives boundary values through the
real `onMedia()` with an injectable `micros()` -- all four inclusive tops, each demonstrated RED.  That split
is the honest one: the gate proves the instrument RAN and bucketed at a pace only the peer knows; the host
suite proves it bucketed CORRECTLY.

Unchanged and provably unaffected: `crc200` (computed at DECODE time in `pushFrame`, so nothing about ring
depth, pre-fill or re-prime can move it), `over=0`, `bad=0`, the decoded-level band, the DelayReport (now 464,
derived from `TARGET`; the gate greps its presence, not its value).

`tools/gate-vacuity.test.sh` gains a negative: a capture carrying a well-formed `bt_jit` line whose buckets do
NOT account for every interval must still fail, by name.

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
* **`under <= dryTotal`.**  ~~`under <= reprimes + 2`~~ -- RETIRED by section 9, and the reason is worth
  keeping.  That bound assumed one counted underrun per dropout, which was true only while EVERY dry block
  re-primed.  With the threshold, a sub-threshold spell ticks one underrun PER BLOCK (NEW-41's semantics)
  and only a crossing spell books an event, so `under` counts blocks again and must be bounded by the
  blocks actually spent dry.  Holds on the gate's own fictional numbers as a sanity check: `drytot=1249`
  against `under=748`.
* **`fillmax <= RING - 4`** -- real margin above the operating point, measured by the ISR rather than
  sampled.  At `RING 32` that is `fillmax <= 28`; it moves with `RING`, since it expresses margin.
* ~~**`fillmin >= TARGET/4`**~~ -- **RETIRED as UNSOUND, 2026-09-10.**  `fillMin` samples the PRE-POP fill
  on the pop branch only, so approaching empty the sequence is ...fill=2 pop, fill=1 pop, then a dry block:
  `fillmin=1` is GUARANTEED on any run where the ring ever empties, and emptying is exactly what a dropout
  does.  The criterion is therefore equivalent to "no dropout ever happened" and never measured the
  habitual low-side margin this section claimed for it.  Measured 1 in every arm of RUN 5 and RUN 6.  The
  instrument that would measure it is the fill HISTOGRAM, which section 6 deferred on the grounds that
  `fillmin`/`fillmax` answered the question -- refuted for the low side.
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
* **Threshold re-prime** -- PROMOTED OUT OF THIS LIST BY RUN 5; see section 9.
* **A sample-domain ring** instead of a block ring, removing the 8-block arrival quantisation from the
  ring's accounting.  A redesign, not justified by the evidence: the block ring is fine once it is deep
  enough.
* **C, the runtime pairing mode.**  Out of scope by agreement; its own spec.

## 7. Silicon: the bench

**COMPLETE -- ACCEPTANCE MET at RUN 8 (2026-09-10).**  Five bench runs; the sections below are kept in the
order they happened, because three of them refuted something this spec asserted.  Final configuration:
`RING 40 / TARGET 16 / pre-fill / REPRIME_AFTER 16`.

| | control (RUN 4) | final (RUN 8) |
|---|---:|---:|
| `over` per min | 13.09 | **0** |
| `under` per min | 16.08 | 0.69 |
| `fillmax` | 15 = ceiling | 27, of 39 |
| `reprimes` | n/a | 0 |

Every criterion in section 5 as restated, plus section 10.1, is met.  The one exception across all four
change-arm runs is `prime_ms ~ 46 ms` (measured 87 / 101 / 136 / 89): the pre-fill waits for `TARGET`
blocks and the phone's first packets arrive slower than real time while its own pipeline fills, so the
wait runs about 2x the block count.  ~90 ms of silence before the first note; recorded, not chased.

**Originally written as PARTIAL after the control arm:**  Transcript: `examples/audio/bt_sink_test/
transcript_hw_evkb.txt`, RUN 4 (2026-09-09 evening).  Section 5's acceptance is therefore still OPEN.

### 7.1 Control arm (RING 16 / TARGET 8 / no pre-fill + the instrument) -- DONE

One continuous stream of 1585.5 s (26.4 min; the phone held the A2DP stream open past the music, so this is
2.6x the planned window -- counters normalised per minute).

| | measured |
|---|---|
| `pkts` / `frames` | 68325 / 546254 -> **7.995 frames per packet** |
| `under` / `over` | 425 / 346 = **16.1 / 13.1 per minute** (run 3: 10.3 / 8.9 -- same order, same PAIRED shape) |
| overrun EVENTS | 101, averaging 3.4 dropped frames each |
| ring extremes | `fillmin=1 fillmax=15` -- **rail to rail**, and 15 is `RING-1`, the ceiling |
| `seqgaps` / `bad` | 0 / 0 -- nothing was lost on the air |
| trim | -60 .. 105 ppm |
| `reprimes` / `primed` / `prime_ms` | 0 / 0 / 0 -- correct, the pre-fill is OFF in this arm |

**The control arm reproduces the defect it exists to reproduce**, and `fillmax=15` is section 1's overrun
mechanism caught in the act rather than inferred.  The instrument's bucket sum came to 68324 = `pkts - 1`
EXACTLY -- the same invariant the gate asserts, holding on silicon over 68k intervals.

### 7.2 The gap distribution, and the `TARGET` decision

| gap | count | share |
|---|---:|---:|
| <=30 ms | 67,307 | 98.512% |
| 30-50 ms | 988 | 1.446% |
| 50-80 ms | 27 | 0.040% |
| 80-120 ms | 2 | 0.003% |
| >120 ms | 0 | 0% |
| **worst** | **97 ms** | = 33.4 blocks |

**DECISION: keep `TARGET 16` (46.4 ms), `RING 32`.**  It rides out **99.958%** of intervals (68295 of 68324).
The worst gap is 97 ms -- BIGGER than the 35-45 ms this spec estimated from run 3's burst shape, so the
estimate was low -- but it occurred TWICE in 26 minutes.  Covering it would need `TARGET 34 / RING 50`, i.e.
99 ms of latency, to save two events; section 5 says explicitly to record such a tail rather than inflate
`TARGET` until the numbers look nice.  The 29 intervals over 50 ms become BOUNDED re-primes -- one counted
underrun each -- instead of underrun bursts, which is precisely what the re-prime exists for.

### 7.3 Prediction for the change arm, recorded BEFORE it runs

`over = 0` (hard); `reprimes` ~29 per 26 min with `under` ~= `reprimes`; `fillmin >= 4` and `fillmax <= 28`;
one `prime_ms` ~46 ms at START and no start-up underrun burst; no audible change.

### 7.4 A bench trap met on the way, and it is NOT a NEW-42 defect

`bonds_boot=0` -- the board's bond store was empty while the iPhone still held a stale link key.  SSP then ran
CORRECTLY to Just Works (`io_cap_req` -> `user_conf_req numeric=19466 -> accept`) and the PHONE failed it
(`pairing_complete: status=0x05`, Authentication Failure).  `BtLink`'s designed legacy-PIN fallback then wrote
`Write_Simple_Pairing_Mode = 0`, so **SSP was off for the rest of the session** and the next attempt went
straight to `pin_code_req` with no IO-capability exchange -- the passcode prompt iOS showed.  **One failed SSP
poisons every later attempt until the next boot**, because only PREPARE re-enables it.  Bench fix: forget the
device on the phone, press SW4, pair fresh -- Just Works, `status=0x00`, first try.  Worth a follow-up issue:
the fallback could re-issue `Write_Simple_Pairing_Mode = 1` when a PIN attempt fails, so a session recovers
without a reboot.

## 8. Files (expected)

evkb: `examples/audio/bt_sink_test/{AudioInputBluetooth.h,AudioInputBluetooth.cpp,bt_sink_test.cpp,
CMakeLists.txt,run_qemu.sh,tests/node_test.cpp,transcript_hw_evkb.txt,transcript_qemu.txt}`,
`examples/networking/m2_hci_probe/hci_peer.py` (the scripted gap in the `source` phase),
`tools/gate-vacuity.test.sh`, `evkb.cmake` (M2Radio pin), `CLAUDE.md`, memory, this spec.
M2Radio: `bt/Avrcp.{h,cpp}`, `bt/test/avrcp_test.cpp`.


## 9. Threshold re-prime (opened by RUN 5, 2026-09-10)

RUN 5 met neither `over = 0` nor `fillmax <= RING - 4`, and the trace named the mechanism: **a re-prime
refills the ring to `TARGET` from fresh packets, and THEN the backlog the source buffered during the gap
lands on top of it** -- `TARGET + backlog > RING - 1`, and the surplus is dropped.  Boot 2 went
`fillmax` 25 -> 31, `overev` 0 -> 3, `reprimes` 1 -> 2 inside one heartbeat window.

Not raising `RING` -- that out-runs the mechanism rather than removing it.  The chosen fix is section 6's
deferred **threshold re-prime**: rebuffer only after the ring has been dry for more than N consecutive
blocks.  Below N the ring simply ticks 2.9 ms underruns and the source's catch-up burst refills it by
itself, so the `TARGET` term never enters the sum.  The re-prime becomes what it should always have been --
a safety net for a source that does NOT catch up and would otherwise leave the ring permanently short
(section 1, fact 3) -- rather than a response to ordinary jitter.

### 9.1 N is measured, not chosen -- and the current code cannot measure it

Inference from the gap histogram puts every dry spell this phone produced at <= 25 blocks (the servo holds
fill at ~`TARGET`, so a gap of G blocks leaves the ring dry for about G - 16; the worst gap, 97 ms = 33
blocks, gives 17).  That is an inference, and `TARGET 16` was earned by measurement rather than inference,
so N will be too.

★ **The obstacle, and it is the whole reason this needs its own instrument: the re-prime TRUNCATES the
quantity to be measured.**  The ring goes dry for exactly one block and then enters priming, so any
counter that stops at "dry" reads 1 every time, forever, whatever the source did.

What must be counted instead is **consecutive `update()` calls on an EMPTY ring, irrespective of priming
state** -- during a re-prime the ring genuinely is empty until packets arrive, so counting through the
priming window recovers the natural dry spell exactly, with no change to current behaviour.  Three
exclusions, each for the same reason its sibling counter has it:

* **not while HELD** -- an AVDTP SUSPEND is not a dropout, the same principle that keeps it out of `under`;
* **not before the START prime has completed** (`m_primed` false) -- the START prime is a long empty
  stretch by construction (87/89 ms measured, ~30 blocks) and would dominate the histogram with an
  artefact of stream setup rather than of delivery;
* **not while `!m_live`.**

### 9.2 What ships in this increment

`dryMax` (longest run of consecutive empty blocks) plus a small dry-spell histogram bucketed at
<=4 / <=8 / <=16 / <=32 / >32 blocks, sampled when a spell ENDS, and `dryTotal`.  Cumulative for the run,
like every other tally on the line.  Additive: no behavioural change, so RUN 6 is directly comparable with
RUN 5, and the gate is untouched -- these are consume-side numbers and therefore silicon claims, exactly
as `under` and `reprimes` already are.

### 9.2a A limitation of the `m_primed` exclusion, found while building it

Gating on `m_primed` means the instrument reads **all zeros in the CONTROL arm** (`BT_SINK_PREFILL=0`),
because nothing ever latches `m_primed` there.  That is the specified exclusion behaving as specified, and
RUN 6 is a change-arm run by design, so it costs nothing here -- but it is precisely the "counter that
stopped measuring while looking healthy" shape this tree fears, so it is called out in the header, at the
print site and in the host case: `drymax=0 drytot=0` beside `primed=0` means NOT MEASURED, not "no dry
spells".  If a control-arm dry distribution is ever wanted, the arm-independent predicate is "after the
first successful pop", which excludes the same start-up stretch without depending on the pre-fill.

★ Also measured while building it, and it sharpens 9.1: a counter placed in the dry BRANCH does not read
1 as predicted -- it reads `dryMax=0`, never closing a spell at all, because with the pre-fill on the
block that would end the spell is itself a priming block.  Two independently written dry-branch mutants
gave the identical reading.  The truncation is worse than the spec assumed.

### 9.3 What RUN 6 decided -- MEASURED 2026-09-10, N = 16

RUN 6 (26.3 min, the change arm plus the instrument, no behavioural change): **three dry spells -- two of
<= 4 blocks, one of <= 16, longest 9 -- eleven dry blocks in total.**  `drymax=9 drytot=11 d4=2 d8=0 d16=1
d32=0 dbig=0`.

★ **The inference in 9.1 was wrong by about 3x, which is exactly why this run existed.**  It reasoned that
the servo holds fill at ~`TARGET`, so a gap of G blocks strands the ring for G - 16 and the worst spell
would be near 25.  Measured: 9.  The error is visible in the same heartbeat -- `fillmax=27`, so the
operating point PLUS the 8-block sawtooth keeps fill well above `TARGET` much of the time and a 22-block
gap is simply absorbed.  The ring empties only when a gap lands on a trough, which is rare and shallow.

★ **And `over = 0` held for the whole 26.3 min WITH three re-primes.**  So RUN 5's boot-2 overrun was a
re-prime coinciding with a large backlog, not something every re-prime causes.  The threshold removes a
mechanism, not a certainty -- worth stating so the next run's `over = 0` is not over-credited to it.

**`REPRIME_AFTER = 16` blocks (46.4 ms)**, `TARGET`-sized and clear above every spell this phone produced,
while still firing for a genuine stall.  Overridable as `BT_SINK_REPRIME_AFTER`, and pinned by value in
`node_test` case 16(g) -- because the rest of the suite is parameterised on the constant and was therefore
blind to it: mutating the default to 0 (i.e. reverting to the old always-re-prime behaviour) left every
case green except case 12(d).

**The trade to accept with open eyes, recorded before the run:** below the threshold `under` returns to
counting BLOCKS rather than EVENTS, so it will RISE -- predicted from RUN 5's 10 events to roughly 60-100
ticks over a comparable window, each an inaudible 2.9 ms -- while `over` should reach 0 and `reprimes`
fall to ~0 for this phone.  Section 5's `under <= reprimes + 2` therefore stops being the right bound and
must be restated as `over = 0` plus `under <= dryTotal`.  A criterion that no longer measures what it
claims gets rewritten, not reinterpreted -- as `fillmin >= TARGET/4` already had to be (section 7.5).


## 10. RING 32 -> 40 (opened by RUN 7, 2026-09-10)

RUN 7 met the criterion that matters -- **`over = 0` across 38.8 min and three boots, with
`reprimes = 0`** -- but boot 1 reached `fillmax = 30` against a ceiling of 31, on its one gap in the
80-120 ms band (`gapmax = 87 ms`).  A one-block margin.

**The threshold removed the AMPLIFIER, not the mechanism.**  A post-gap catch-up burst still lands on
whatever the ring already holds; what changed is that the ring is no longer refilled to `TARGET` first, so
the peak is `backlog` instead of `TARGET + backlog`.  That is why 30 < 31 held rather than overflowing --
and equally why a longer gap than this phone happened to produce would still overflow.

`RING 32 -> 40`, `TARGET` unchanged at 16 so **latency and the DelayReport do not move**: the backlog gets
23 blocks of headroom above the operating point instead of 15.  Cost is 8 KB of `.bss` (`btin`
0x4a20 -> 0x5a20, measured -- exactly 8 slots x 512 B) and nothing else.  Raising `RING` alone was rejected
in section 9 as a way to out-run the mechanism; it is the right move *after* the threshold has removed the
amplification, as headroom for the residual rather than as a substitute for the fix.

★ **A CMake trap met while making the change, worth knowing because the build lies convincingly.**
Editing the `set(BT_SINK_RING "32" CACHE STRING ...)` default does NOT change an existing build directory:
the cached value still reaches the compiler through `target_compile_definitions`, so the image rebuilds
happily at the OLD depth.  `btin` read 0x4a20 after the edit and only moved once each directory was
reconfigured with an explicit `-DBT_SINK_RING=40`.  Check the symbol size, not the source.

### 10.1 Acceptance for RUN 8

As section 5 (restated), plus `fillmax <= RING - 4 = 36` with real margin rather than one block.  `under`,
`drytot` and `reprimes` are expected to be indistinguishable from RUN 7 -- the ring's LOW side is untouched,
so if they move materially that is a finding, not a bonus.
