# UAC validator state block — wire format

This is the **normative contract** between the `lib_xua` device-side observer
and the `tools/uacvalidate` judge. The observer computes no verdicts: it
reduces, and everything here is a reduction. Meaning lives here; the Python
`wireformat.py` implements this document, not the other way round.

Probe ids 0–3 (`out_underflow`, `out_overflow`, `out_fifo_fill`,
`out_dryout`) keep their existing meaning and behaviour — host gates and
committed transcripts depend on them. New ids are **append-only**.

## The block

37 words, little-endian uint32, emitted at 100 Hz (one per ten OUT packets,
riding the divider that already exists for the fill probe).

| Word | Name | Meaning |
|---|---|---|
| 0 | `magic` | `0x55414356` (`'UACV'`) |
| 1 | `version` | `1` |
| 2 | `pkt_count` | OUT packets accepted |
| 3 | `pkt_short_discarded` | packets shorter than one audio frame |
| 4 | `pkt_not_multiple` | packets not a whole number of frames |
| 5 | `or_acc` | OR of every sample word |
| 6 | `and_acc` | AND of every sample word |
| 7 | `nonsilent_frames` | frames where any channel was non-zero |
| 8 | `fb_poll_count` | feedback IN completions |
| 9 | `fb_value` | current `fb_clocks[0]` — see *Undecided* below |
| 10 | `alt_out` | current `g_curStreamAlt_Out` |
| 11 | `alt_transitions` | count of alt changes |
| 12 | `class_req_bitmap` | bit0 clock SET_CUR, bit1 clock GET_CUR, bit2 SET_INTERFACE |
| 13 | `host_active` | 0 or 1 |
| 14 | `pat_err_count` | cooperative mode only |
| 15 | `pat_resync_count` | cooperative mode only — **not** the same as word 36 |
| 16 | `pat_first_err_idx` | cooperative mode only |
| 17 | `pat_first_expected` | cooperative mode only |
| 18 | `pat_first_actual` | cooperative mode only |
| 19 | `size_hist_overflow` | packets whose size claimed no histogram slot |
| 20–27 | `size_hist_size[0..7]` | observed packet size in bytes, 0 = slot unused |
| 28–35 | `size_hist_count[0..7]` | count for the matching slot |
| 36 | `pat_sync_count` | times the cooperative pattern achieved lock |

All counters are free-running uint32 and may wrap. **The reader does not
unwrap**; the judge does, via `delta32`. A sample counter at 8 ch × 44.1 kHz
wraps in about 3.4 hours, inside soak durations this bench already runs.

### `pat_resync_count` vs `pat_sync_count`

Two different events; **do not implement them as one increment.** A *resync*
is giving up and hunting again — it fires after 8 consecutive mismatches. A
*sync* is a successful lock — it fires only when the seed value is matched. A
host whose playback path is not bit-exact accumulates resyncs while its sync
count barely moves. Collapse them and R7 loses its ability to tell "never
locked" from "locked once, then lost samples", and the judge goes back to
accusing conformant hosts of dropping packets.

The judge reads `pat_sync_count` **absolutely**, unlike every other counter,
because a capture opened mid-stream shows a constant value whose delta is zero.

### Emission rule the observer must obey

**Every emission must write at least one word.** If a block is identical to
its predecessor, emit word 0 alone as a heartbeat. Otherwise the timestamp
carries nothing, the reader drops it, and a wholly stationary block set — which
is exactly what a **stalled host** looks like — becomes indistinguishable from
a dead observer or a capture gap.

The first emission must write all 37 words. The reader refuses a capture whose
first timestamp is incomplete, rather than zero-filling: a partial block is a
different wire format, not a partial reading of this one.

**The observer writes all 37 words on every emission.** Do not delta-encode on
the device: it makes each block self-contained, satisfies the heartbeat rule
trivially, and keeps the repeated-word-index grouping exact, at a bandwidth
cost already accounted for below. Delta encoding exists only in the judge's
*synthetic* generator, where it is a property being tested rather than a
transport optimisation — which is why the heartbeat rule is written in general
terms above rather than as "always write 37".

## Emission format: SCALAR — decided by measurement, 2026-08-04

37 scalar probes at ids 4..40, one per word, named `uacv_w00` … `uacv_w36`.
The reader maps signals **by name, never by probe id**.

Two candidate formats were spiked on hardware (XTAG-3 `3LajHPG5`, MC200 running
`2AMi8o8xxxxxx`, two 12-second captures). The record form lost.

### What the spike measured

**1. `xscope_bytes()` data lands in `$comment` blocks, not as value changes.**

```
$comment
l16 a0a1a2a3a4a5a6a7a8a9aaabacadaeaf 4
$end
```

The payload is intact and the encoding is regular — `l<length> <hex bytes>
<probe id>` — so it *is* parseable. But it is inside `$comment`, which every
standard VCD parser skips by definition and every waveform viewer ignores. A
record-format capture would be a file in which the entire instrument is
invisible to every tool except one bespoke parser.

**2. `xrun` declares a `$var` for a probe that is never emitted.** The spike
registered `spike_silent` and never wrote it; the header still carried
`$var wire 32 5 spike_silent $end`.

This settles the risk flagged against the reader's completeness check. The
pattern words (14–18) stay zero through any passive-mode run, and the worry was
that `xrun` would omit them and the reader would refuse a good capture. **It
does not omit them.** The all-37-declared check is safe.

Confirmed a second time against the *real* observer rather than the spike:
`docs/uac-validator-idle-header.vcd` is a capture of the finished 41-probe
build with no host attached, and it declares all 37 `uacv_w*` signals while
containing **no value lines at all** — because with no host, no packet arrives,
so the fill-cadence emission never runs. Header completeness and emission are
independent, which is precisely the property the completeness check relies on.

**3. The VCD identifier *is* the probe id** — `0`, `1`, `2`, `3`, `4`, `5`,
`255` — not an arbitrary code. The judge's synthetic generator uses
`chr(33 + i)` instead, so identifiers differ between synthetic and real
captures. This is harmless **only because** the reader resolves by name; it is
also why `vcdfill.py`'s historical hard-coded identifier `"2"` had to stop
being authoritative, since `chr(50)` is `'2'` and would have resolved to state
word 17 in a synthetic capture.

**4. Consecutive `xscope_int()` calls get distinct timestamps ~18 ticks apart**
at the observed 10 ns timescale — about 180 ns per probe. A 37-word block
therefore spans roughly **6.7 µs**, against a 10 ms inter-block period: three
orders of magnitude of margin.

The spike emitted two back-to-back four-probe bursts separated by only 170 ns —
a worse case than anything a 100 Hz observer can produce, since the inter-burst
gap was indistinguishable from the intra-burst gap. The reader's
**repeated-word-index rule** (a word index appearing twice closes the group)
separated them correctly. That rule is exact and threshold-free; the 1 ms
window is a secondary net for the delta-encoded case where indices never repeat.

**5. Timescale is `10.000000 ns`, written in the split form** (`$timescale`,
value, `$end` on separate lines). `Missing_Data` is declared as
`$var wire 1 255 Missing_Data`, which is exactly the name `count_missing_marks`
matches.

### Why scalar wins

- The capture stays a **real VCD**. You can open it in a waveform viewer and
  watch `pkt_count` climb and `alt_out` step — genuine value for a bench
  instrument, and impossible with the record form.
- No bespoke comment parser, and nothing that a future standard tool will
  silently drop.
- The atomicity the record form would have bought is worth less than it looked:
  the grouping rule is now *measured* rather than assumed, and the margin is
  1000×.

### The cost, stated plainly

37 words × 100 Hz = 3,700 events/s, roughly 74 kB/s of VCD text, so a
30-minute soak is on the order of 130 MB. That is affordable but not free.

If `xscope_missing_marks` turns out non-zero on a real capture, the first lever
is to **drop the block cadence to 10 Hz** (every hundredth packet) while
leaving the fill probe at 100 Hz. The block carries counters, not a waveform,
and 100 ms resolution is ample for every rule that consumes it — R2 asks
whether packets arrived during alt 0, not precisely when the switch happened.
Do not instead widen the reader's tolerance; a dropped mark means the counts
are lower bounds and the report is `INVALID` by design.

## Undecided, deliberately

**`fb_value` (word 9) is published raw and rendered as hex.** Decoding it needs
the 10.14-vs-16.16 choice — 10.14 right-justified in three bytes at full speed,
16.16 in four at high speed — and `man.speed` is the manifest field that would
select between them. What is *not* known is where the device places the value
inside the published word: `ep_buffer.xc` shifts it differently at FS and HS,
and no capture yet exists to check a decode against. A confident wrong sample
rate printed beside a correct one is worse than hex, so this stays raw until
the first real capture settles it.

## Changing any of this

`magic` catches a *different* wire format. `version` catches a *revised* one,
where the words still parse but no longer mean what the judge thinks. If you
renumber, resize, or repurpose a word, **bump `version`** — the judge checks
both and reports `INVALID` on either mismatch, which exits 2 rather than 1
because a format disagreement says nothing about the host under test.
