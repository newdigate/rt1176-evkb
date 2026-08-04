# UAC host conformance validator — design

Date: 2026-08-04
Status: design approved, not yet implemented

## The argument

A USB audio device is the only party that sees ground truth about its host.
This week's UAC1/UAC2 bring-up on the MIMXRT1170-EVKB proved it by
measurement: an analogue capture of the device's line out "ruled out" rate
mismatch, wrongly, because the capture chain's own event floor was
~17 events/s while the real phenomenon ran at 0.005–0.06 events/s. Four
device-side xscope probes found the same phenomenon immediately, because a
counter in the decoupler has no floor.

Six real host defects were found on this bench, each costing hours:

1. Feedback endpoint never read → unbounded drift → periodic block corrections.
2. Feedback read, but the servo chased the raw dithered report instead of its
   mean → +4.8 ppm residual (a buffer excursion every ~45 min).
3. Isochronous audio streamed to a device sitting in alt 0.
4. Device claimed, class configuration sequence never completed.
5. Subslot packing silently plausible — right-justified 24-in-4 sounds fine,
   quietly.
6. Packet sizing ignoring the fractional-sample rule.

Every one of them was visible from the device. None of them was reported by
the device, because the device had no way to say so. This tool gives it one.

Evidence for all of the above: `examples/usb/usb_audio_graph_test/
transcript_hw_evkb.txt`.

## Decisions

Settled in brainstorming, recorded here as the frame for everything below.

| Decision | Choice |
|---|---|
| Reporting channel | xscope. **XTAG required and attached.** Bench equipment, not a shippable product. |
| Where spec knowledge lives | Split at **reduction vs judgement**: the device reduces, the host judges. |
| Host cooperation | Both modes; **passive is the default**. |
| Scope | **OUT direction only, both classes** — UAC1/FS and UAC2/HS. |
| What defines conformant | **Spec-literal.** A FAIL always cites a clause. |
| Verdict levels | **Three** — PASS / FAIL / WARN — plus SKIP and unjudged metrics. |
| Home | `evkb` now, with boundaries drawn so extraction to its own repo is a `git mv`. |

The reduction/judgement split is the load-bearing one. The constraint is on
**data reduction** — 8 ch × 24-bit × 44.1 kHz cannot be streamed out for
host-side checking. Deciding that a byte-lane mask means "right-justified" is
a comparison against a constant and can happen anywhere. Putting only the
reduction on-device buys the bandwidth win while keeping every rule,
threshold, clause citation and report format in MIT-licensed Python that
changes without a reflash — and keeps rules re-runnable against archived
captures.

## Architecture

| Component | Home | Job |
|---|---|---|
| **Observer** | `lib_xua` branch `instrumentation/uac-host-validator`, cut from `instrumentation/decouple-xscope-probes` at `26d13bab` | Observe and reduce. No verdicts, no thresholds, no clause text. |
| **Capture** | unchanged — `xrun --xscope-file` | The VCD is both wire format and archive. |
| **Judge** | `evkb/tools/uacvalidate/` (Python) | VCD + run manifest → report. `vcdfill.py` becomes a module inside it. |

### Boundaries

- **Observer never judges.** This is what keeps rule iteration out of the
  XMOS-licensed repo and off the reflash path.
- **Judge knows nothing about the RT1176.** Input is a VCD plus a manifest;
  output is a report. No `evkb.cmake` coupling, no import from the example
  tree. `git mv tools/uacvalidate` must later be the entire move.
- **Probe ids are append-only.** Ids 0–3 keep their current meaning and
  behaviour; existing host gates and transcripts depend on them. New probes
  start at 4.
- **Two images, one judge.** `1AMi2o2` (UAC1/FS) and `2AMi8o8` (UAC2/HS).

### Licence firewall

`evkb` gains the spec, the Python judge, its tests, run manifests,
transcripts, and **one regenerated patch file** representing the whole
observer branch — superseding `lib_xua-decouple-instrumentation.patch`. The
commit-by-commit reasoning stays on the branch in `~/Development/xmos/lib_xua`.
No XMOS source enters `evkb`.

## Findings that shape the observer

Read from the source, not assumed:

- **Byte-lane justification is visible only inside the decoupler.**
  `decouple.xc:190` funnels every 24-in-4 channel count through a single
  `_send_sample_4()`. Downstream the evidence is destroyed — the 3-byte path
  computes `sample = unpackData & 0xffffff00` — and `UserBufferManagement`
  runs on the *other tile*, after unpacking. An app-level hook cannot see
  this. R3 is a `lib_xua`-only observation.
- **Packet length is already in hand** at `decouple.xc:1037`, read from the
  released XUD buffer before the FIFO write pointer advances.
- **`lib_xua` already carries defensive code for three host defects it never
  reports.** `decouple.xc:1040` silently discards packets shorter than one
  audio frame ("Ignore bad small packets"); `decouple.xc:631` handles a
  `datalength` that is not a multiple of the channel count, under a comment
  naming a bad driver; the mask above silently accepts any justification. A
  large part of this tool's job is making existing silent tolerance audible.
- **EP0, the decoupler and the EP buffer all sit on `XUA_XUD_TILE_NUM`**, so
  shared globals with single-writer discipline work — the pattern the existing
  patch already uses for `g_outDryoutCount`. AudioHub is on the other tile and
  is not involved.
- **Memory is not a constraint.** The built `2AMi8o8` image uses 29 KB on
  tile 0 and 52 KB on tile 1, of 256 KB each.

## Observations and reductions

OUT direction, both images, passive unless marked. Probes 0–3 untouched.

| Where | Reduction | Serves |
|---|---|---|
| `decouple.xc:1037` — packet released | packet count | arrival rate |
| same | size histogram: ≤8 distinct sizes with counts, plus a "more than 8 distinct" overflow counter | R4 |
| `decouple.xc:1040` else-branch | short-packet-discarded count | R4b (*silent today*) |
| `decouple.xc:631` tail path | datalength-not-a-multiple-of-frame count | R4b (*silent today*) |
| `_send_sample_4()` and the case-2 (16-bit) path | `orAcc \|= word`, `andAcc &= word` over every sample | R3 |
| same | non-silent frame count | vacuity guard |
| same, cooperative only | pattern-error count, resync count, first-error sample index, first expected/actual pair | R7 |
| `ep_buffer.xc`, feedback EP IN completion | feedback-poll count | R1 |
| same | the device's own current feedback value | W3 |
| EP0 | current OUT alt, alt-transition count | R2 |
| EP0 | class-request-arrived bitmap, host-active state | INFO |

Alt transitions carry no timestamp of their own: the state block is emitted at
100 Hz and xscope timestamps each emission, so a change is located to within
10 ms. That is ample for R2, which asks whether packets arrived during alt 0,
not exactly when the switch happened.

The class-request bitmap is one bit per request type of interest — clock
Source `SET_CUR`, clock Source `GET_CUR`, `SET_INTERFACE` — set on first
arrival and never cleared. It answers "did the host ever do this", which is
what defect #4 needs.

### The justification check

Two accumulators over every sample word answer R3 without knowing anything
about the content:

- `(orAcc & 0xFF) == 0` — the low byte was never non-zero across the whole
  run ⟹ left-justified, conformant.
- `(orAcc >> 24) == (andAcc >> 24)` — the top byte never varied ⟹
  right-justified with zero or sign fill.

Two ALU ops per sample: 0.7 M ops/s at 8 ch × 44.1 kHz, 3 M ops/s at 192 kHz,
against roughly 62 MIPS available to that core. Not a real-time risk.

Per-channel accumulators would be stronger and cost 8× the state. Deferred;
global first.

### The vacuity guard is not optional

If the host sends digital silence, `orAcc` is zero and R3 passes *for the
wrong reason* — declaring a right-justifying host conformant. The non-silent
frame count gates it: below **10,000 non-silent frames** (0.23 s at 44.1 kHz)
the judge reports `SKIP: insufficient signal`, never `PASS`. The threshold is
deliberately conservative — the cost of a false SKIP is one more run, the cost
of a false PASS is a wrong verdict about a host.

This is the same failure `tools/gate-vacuity.test.sh` exists to catch, and it
is the single most likely way this tool lies. **Every reduction that can be
satisfied by an absence gets an explicit witness count, and a missing witness
is SKIP, never PASS.**

### Emission

All new state is emitted from the decoupler at its existing 100 Hz cadence.

**Preferred format:** a single atomic record via `xscope_bytes(4, …)`
carrying the whole state block. Atomicity matters because the judge reasons
across quantities — fill slope versus packet sizes versus feedback value —
and separately-timestamped probes must be aligned with a tolerance window,
which is a quiet source of error.

**Fallback format:** scalar probes at ids 4+, with timestamp-window alignment
in the judge. Workable, slightly worse.

`xscope_bytes()` exists in the XTC 15.3.1 headers. **Unverified: whether a
bytes probe lands usefully in the `--xscope-file` VCD**, which is what
`vcdfill.py` parses and every existing gate consumes. Resolved by the spike,
below.

### The run manifest

The operator declares per run: class, speed, channel count, subslot bytes,
sample rate, mode, and a free-text note about the host under test. The judge
reads expectations from this, **never from the trace**. Inference would let a
mis-set expectation silently reinterpret data — the failure that produced the
0.222-slope probe.

## Rules

`FAIL` always cites a clause. `WARN` always states consequence arithmetic.
`SKIP` always states the missing witness. Exit code is non-zero on any FAIL,
so this is gate-able like the QEMU runners.

| Id | Assertion | Level | Evidence | Citation |
|---|---|---|---|---|
| R1 | Host polled the feedback endpoint at least once while streaming | FAIL | fb-poll count, alt state | UAC2 §3.16.2.2 **verified**; USB 2.0 §5.12.4.2 `[UNVERIFIED]` |
| R2 | No packets arrived while the OUT interface was in alt 0 | FAIL | packet count vs alt state | UAC2 §3.16.2, §4.9.1 partial; USB 2.0 §9.4.10 `[UNVERIFIED]` |
| R3 | Samples left-justified in the subslot | FAIL | `orAcc`/`andAcc`, non-silent count | Audio Data Formats 2.0 §2.3.1 `[DOC MISSING]` |
| R4a | No packet exceeds `wMaxPacketSize` | FAIL | size histogram, manifest | USB 2.0 §5.6.3 `[UNVERIFIED]` |
| R4b | Every packet is a whole number of audio frames | FAIL | not-multiple count, short-discarded count | UAC2 frame structure `[UNVERIFIED]` |
| R7 | Sample continuity — no drops or duplications (*cooperative only*) | FAIL | pattern-error count, first-error index | weakest citation in the set — see below |

| Id | Observation | Level | Consequence stated |
|---|---|---|---|
| W1 | Residual rate error from fill slope | WARN | "+X ppm ⟹ this device block-corrects every T s", from the measured correction quantum |
| W2 | Packet sizes outside `{floor, ceil}` of the nominal frame count | WARN | extra FIFO wander ⟹ headroom consumed |
| W3 | Host's packet sizing does not track the device's reported feedback | WARN | the residual produced, and its consequence |

W3 makes defect #2 directly computable: the device emits its own feedback
value, so the judge compares what the device asked for against what the host
sent. The +4.8 ppm dither-chasing servo would have appeared here as a
first-class observation rather than an inference.

**Why packet lumpiness is W2 and not a FAIL.** A hard `wMaxPacketSize`
violation and a non-integral frame count are unambiguous. But lumpy sizing —
40/48/40/48 where 44/45 was expected — is not forbidden by anything citable:
with an async feedback loop the host is entitled to vary packet size. It is
bad engineering the spec permits, which is exactly what WARN is for. A badly
behaved host can therefore pass R4 outright; that is the accepted cost of a
spec-literal rule set.

**R7's honesty problem.** Isochronous transfers have no retry, and no clause
currently in hand says a host must not drop data. The strongest form of the
citation is the absence of any permission to drop, which is weak. R7 stays at
FAIL — a host that loses samples has failed at its only job — but its
citation must be settled once `usb_20.pdf` is on disk, and it may become a
WARN.

### Metrics (no verdict)

Fill envelope and slope; correction counts from probes 0–3; packet rate;
feedback poll rate and value; alt transitions with timestamps; the
class-request map and host-active history (defect #4, reported as INFO).

### Two things that invalidate a whole report

- **`xscope_missing_marks > 0`** — xscope dropped data, so every count is a
  lower bound. `INVALID`, not `PASS`.
- **Manifest contradiction** — declared format disagrees with what the trace
  implies. `INVALID`, both numbers printed.

### Report shape

Header: manifest echo, VCD path, duration, missing-marks count, and the
observer firmware git SHA. Then rules in id order, then metrics, then a
one-line summary. The firmware SHA matters: a report whose reductions came
from an unknown build is not evidence.

## Citation status

The two documents on disk do not contain the clauses this tool must cite.

- `docs/usb_20g.pdf` is **not** the USB 2.0 specification. `pdfinfo` reports
  `usb2tech_ovr.PDF`, 6 pages, Acrobat PDFWriter 4.0, October 1999 — a
  technology overview. §5.12.4.2 is not in it.
- `docs/Audio2_with_Errata_and_ECN_through_Apr_2_2025.pdf` is the correct
  Audio 2.0 class document, but it delegates both rules this tool needs.
  §3.16.2.2 refers feedback-pipe formatting to USB 2.0 §5.12.4.2 and §9.6.6 —
  confirming the pointer, while the normative text lives elsewhere. §3.16.2.3
  hands audio data formats to a separate document, which is where the
  left-justification rule lives.

**Two documents to fetch, both free from usb.org: `usb_20.pdf` and
`frmts20.pdf` (USB Audio Data Formats 2.0).** Until they are on disk, every
citation is written `[UNVERIFIED]` or `[DOC MISSING]` and no FAIL text states
a clause as fact. A validator that mis-cites is worse than one that does not
cite.

## Modes

**Passive (default).** Everything except R7. R7 reports `SKIP: passive mode`.
Nothing is required of the host beyond playing audio with signal in it.

**Cooperative.** The host plays a specified LFSR sequence. The device syncs on
first match, free-runs, and counts mismatches; after **8 consecutive
mismatches — one audio frame at 8 channels** — it resyncs, so one dropped
packet costs one error rather than an infinite stream.

The pattern can be a WAV file rather than host code, making cooperative mode
available on stock hosts — but it requires a bit-exact playback path, and any
OS volume control, mixing or resampling destroys it. **That precondition is
self-detecting**: if the pattern never syncs, the finding is "host playback
path is not bit-exact", a real result about the host. The report distinguishes
"never synced" from "synced then diverged".

Realistically, cooperative mode on a stock OS host is more trouble than it is
worth — bit-exact paths are fiddly on macOS and Windows, and a failed sync is
ambiguous between a dropping host and a misconfigured one. It is clean on a
host you control. Passive is expected to do nearly all the real work.

## Failure modes of the tool itself

| Failure | Handling |
|---|---|
| `xscope_missing_marks > 0` | Report `INVALID` |
| Manifest contradicts the trace | `INVALID`, both numbers printed |
| Probe-id drift | Observer firmware SHA in the header; append-only id rule |
| A reduction is silently wrong | Negative tests — see verification |
| Counter wrap | Judge treats all counters as monotonic-with-wrap |

**Counter wrap is real at these durations.** A 32-bit packet counter at
8,000/s wraps in ~6 days — irrelevant. A *sample* counter at 8 ch × 44.1 kHz
wraps in ~3.4 hours, and the longest soak so far was 31 minutes with obvious
appetite for longer. `orAcc`/`andAcc` are bitwise and cannot overflow.

**One category error to avoid.** The device's crystal sits at −85.7 ppm
against host nominal, and that is *not* a host defect — the entire point of
the feedback loop is that the host tracks the device. W1 is computed from the
fill slope, which is already the residual *after* the loop. The report must
never present the device's intrinsic offset as a finding about the host.

## Bench procedure

Ordered to sidestep the documented hot-reload race rather than recover from it.

1. `xrun --xscope-file <name> <witness>.xe` — load and start collecting **with
   the host not yet streaming**, so the device settles before any class
   request arrives and the startup race never occurs.
2. Bring up the host under test. If it was already streaming, reset it.
3. Run for the declared duration.
4. End the collector with **SIGINT, never `kill -9`** — `kill -9` wedges
   xscope until a reflash.
5. Run the judge against the VCD plus the manifest.
6. **Archive the VCD.**

Switching between the UAC1 and UAC2 images is a second `xrun` at step 1 with
the host restarted at step 2.

Standing bench rules that apply: never hold the VCOM during any LinkServer
operation when the RT1176 is the host under test; SIGINT resets the xcore and
drops the device off USB, which is normal end-of-run behaviour, not a finding.

### Archiving is a requirement, not hygiene

The entire argument for host-side judgement is that rules stay re-runnable
against past captures. That property is worth nothing if captures are
discarded — and no VCD from this week survives, so every soak behind
`transcript_hw_evkb.txt` is now unre-judgeable. Reduced state blocks at 100 Hz
are small, which is one more argument for reduction over raw per-packet probes.

## Verification

### The negative corpus already exists in git history

The hardest problem with a conformance tool is proving it finds defects rather
than merely producing output. The host driver's own history is a set of hosts
with documented known defects:

| Host under test | Known defect | Validator must report |
|---|---|---|
| before the feedback work | never reads the feedback EP | `R1 FAIL` |
| before the EMA | servo chases raw dither, +4.8 ppm | `W3 WARN`, residual ≈ +4.8 ppm |
| `090eadb~1` | device-swap wedge — healthy API, no descriptors rebuilt | `R2` / metrics show streaming with no packets |
| `d370e80~1` | streams into a device in alt 0 | `R2 FAIL` |

Each has ground truth recorded in `transcript_hw_evkb.txt` alongside the
measurement that established it. **A validator that cannot rediscover a defect
already characterised, from a commit still in the tree, has not earned trust.**
This is the acceptance criterion for v1.

### Deliberate defect injection

The host has always packed left-justified and sized packets correctly, so R3
and R4 have no historical negative. A `defects/` set supplies them: one-line
patches to the RT1176 driver that right-justify subslots, oversize a packet
past `wMaxPacketSize`, or emit a non-integral frame count — each paired with
its expected verdict. Same role as `license-audit.test.sh` and
`gate-vacuity.test.sh`: proving the checks fire rather than pass vacuously.

### Two gates, adapted

- **Judge gate (offline).** Pure Python, fixture-driven. Every rule gets a
  PASS fixture, a FAIL fixture and a SKIP fixture. The SKIP fixtures matter
  most — the silent-stream fixture that must make R3 report
  `SKIP: insufficient signal` rather than `PASS` guards the specific lie this
  tool is most likely to tell.
- **Observer gate (silicon).** Every reduction verified against un-fakeable
  stimulus on real hardware. Silicon still wins: a reduction that is correct
  in a fixture and wrong at 8,000 packets/s is exactly the class of bug the
  two-gate rule exists to catch.

## Plan shape

**Step 0 — the spike.** Register one `xscope_bytes` probe, emit a known
pattern, inspect the VCD. Resolves the emission format before a line of the
real observer is written. Both formats are recorded above; the spike picks one.

Then: observer reductions → judge rules → historical corpus → injected
defects → clean run on both images.

## Definition of done for v1

- The spike has resolved the emission format.
- The observer emits every reduction listed above; probes 0–3 unchanged and
  existing gates still green.
- The judge implements every rule listed above.
- The historical-commit corpus reproduces all four known defects.
- The injected-defect set fires R3 and R4.
- The silent-stream fixture reports SKIP, not PASS.
- A clean run of the current driver against both images produces an
  all-`PASS` report with W1 at the +0.1 to +0.16 ppm already measured.

## Deferred

- Live verdicts (device printing PASS/FAIL to the `xrun` console via
  `XSCOPE_IO_BASIC`). Recoverable later as a `--live` mode reusing the same
  Python rules rather than duplicating them on-device.
- IN-direction validation.
- Per-channel justification accumulators.
- Extraction to a standalone repo.
