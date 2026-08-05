# The verification corpus

A conformance tool that has only ever been run against a healthy host has been
tested for *crashes*, not for *judgement*. It will happily report all-PASS
forever. The only way to know a rule fires is to show it a host that breaks
that rule — and the only trustworthy way to know it fired *correctly* is for
the host's defect to have been characterised beforehand, by a different
instrument.

`USBHost_t36`'s git history is that corpus. Each case is a commit whose defect
was measured and written down in
`examples/usb/usb_audio_graph_test/transcript_hw_evkb.txt` before this
validator existed. The tool is graded against ground truth rather than against
its own output.

## The one edit that destroys this directory

**Adjusting an expectation to make a case pass.**

Every expectation in `cases.json` traces to a number someone measured with a
different instrument. If a case mismatches, exactly one of these is true:

- the validator is wrong,
- the bench differs from the run that produced the ground truth,
- the case is mis-labelled (the commit does not contain the defect claimed).

Find out which. `check_case.py` prints that instruction on every mismatch
because the tempting fix — editing the expected level until the row goes green
— converts an oracle into a mirror, silently and permanently.

If a case genuinely cannot be staged, record `"skipped_reason"` with the
attempts made. An honest gap is worth more than a fabricated pass.

## Files

| File | What it is |
|---|---|
| `cases.json` | The cases: commit, witness image, manifest, expected verdicts, ground truth. |
| `check_case.py` | Asserts a report matches a case. Ignores rules the case does not mention. |
| `host/` | Minimal host firmware built at each historical commit (see below). |
| `reports/` | Committed JSON + rendered reports, one per case. The evidence. |
| `captures/CHECKSUMS` | sha256 of each archived VCD. |

## Why the captures are not in this repo

A UAC2/HS capture runs about 110 KB/s — a five-minute case is ~33 MB, and the
corpus is ten of them. Even gzipped (measured 4.4:1) that is well over 100 MB
of bench artefact in a public repo that deliberately keeps NXP PDFs out for
the same reason.

The VCDs live in `~/Development/rt1170/uacv-captures/corpus/` with their
sha256 in `captures/CHECKSUMS`. What is committed is the *reports* — which is
what `check_case.py` actually validates, so the corpus stays checkable from a
fresh clone even though the raw captures do not travel with it.

The trade is real and worth naming: a future rule cannot be re-run against
these captures from a clone, only from this bench. If that becomes important,
the fix is a release asset or an LFS store, not committing them here.

## Why `host/` exists instead of the example

`examples/usb/usb_audio_graph_test` cannot compile against these commits.
`feedbackRateMilliHz()`, `isUAC2()`, `controlState()`, `topology()`,
`lastConfig()` and `patternMode()` were all added after the oldest corpus
commit, and the example calls every one of them. Checking out
`99cd466~1` and building the current example produces a compile error, not a
historical host.

`host/` is the intersection: only driver API present at *every* corpus commit
(`format`, `ready`, `tone`, `beginStreaming`, `service`, `packetsSent`,
`alternateSetting`, `queued`, `underruns`, and the transport error counters).
It is deliberately dull — the validator reads the *device* side, so the host
firmware only has to stream and say whether it thinks it is streaming.
