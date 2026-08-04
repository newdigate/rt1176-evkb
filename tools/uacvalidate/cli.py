#!/usr/bin/env python3
"""uacvalidate -- judge a USB host's UAC conformance from a device-side capture.

Usage:
    ./cli.py capture.vcd manifest.json [--json]

The device reduced; this judges. Nothing here knows anything about any
particular host -- input is a VCD and a manifest, output is a report.

Exit codes are the interface: 0 clean, 1 the host failed a rule, 2 the capture
or the manifest cannot be judged. Every path out of main() returns one of
those three deliberately, including the error paths -- an uncaught exception
leaves Python exiting 1, and a CI gate reading exit 1 would record "this host
FAILED" for what was actually a truncated capture. That is the false
accusation the whole design is built to avoid, so a malformed capture is
caught here and reported as INVALID.
"""
import argparse
import json
import math
import os
import sys

# vcdfill lives one directory up: it predates the validator and is still used
# standalone by tools/driftrun.sh, so it stays where its other caller expects
# it rather than moving in here.
#
# Appended, not inserted at 0. tools/ is a flat bag of scripts that anyone may
# add to; putting it ahead of this package's own directory would mean a future
# tools/rules.py or tools/report.py silently shadowing the module of the same
# name here, and the judge would run somebody else's code without a word.
sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import metrics
import rules
import report
from manifest import Manifest, ManifestError
from wireformat import read_blocks, count_missing_marks, delta32, MAGIC, VERSION
from verdict import Verdict, INVALID
# fit_fill_slope, not fit_slope: vcdfill already had a public fit_slope(pts)
# returning (slope, r2, n) over a point list, and analyse() unpacks that
# 3-tuple. Redefining the name would have broken tools/driftrun.sh silently.
from vcdfill import parse as parse_fill, fit_fill_slope, probe_points

# lib_xua refills the OUT FIFO with OUT_BUFFER_PREFILL bytes after a dry-out,
# and drains to half on overflow. Either is one block correction. The value is
# the device's, not a tuning knob: it sets how much drift is absorbed before
# the glitch, and therefore the seconds-to-glitch arithmetic in W1.
DEFAULT_CORRECTION_QUANTUM_BYTES = 1024


def _emit(header, verdicts, as_json, metrics=()):
    """Print the verdicts and return the exit code.

    Honours --json on every path, including the capture-level INVALID ones. A
    machine-readable mode that silently reverts to prose exactly when the run
    was rejected would hide the rejections from whatever is parsing it.

    Metrics are printed but are not passed to exit_code, which takes verdicts
    only. An unjudged observation cannot fail a run by construction, not by
    convention.
    """
    if as_json:
        print(json.dumps({"header": header,
                          "verdicts": [v.__dict__ for v in verdicts],
                          "metrics": [list(m) for m in metrics]}, indent=2))
    else:
        print(report.render(header, verdicts, metrics))
    return report.exit_code(verdicts)


def _manifest_contradiction(first, last, man):
    """Message describing a manifest/trace disagreement, or "" if consistent.

    The test is divisibility. Every OUT packet is a whole number of audio
    frames, so every observed packet size must be a multiple of the declared
    frame. The device counts non-multiple packets itself, against the frame
    size it actually knows -- so when the device says zero and the declared
    frame does not divide the sizes on the wire, it is the declaration that is
    wrong, not the host.

    It is a partial net, deliberately so rather than by oversight: a declared
    frame that happens to divide the real one passes here (declaring 2
    channels for an 8-channel stream gives 8 B, which divides 32 B). Catching
    that would mean inferring the format from the trace, which is the thing
    the manifest exists to stop. The gcd is printed alongside so a reader can
    see the frame the trace implies and make the call.
    """
    sizes = last.sizes()
    if not sizes:
        return ""
    bad = sorted(s for s in sizes if s % man.frame_bytes)
    if not bad:
        return ""
    if delta32(first.pkt_not_multiple, last.pkt_not_multiple):
        # The device saw non-multiple packets too. Then the host really did
        # send them and this is R4b's FAIL to issue, not a manifest error.
        return ""
    implied = math.gcd(*sizes) if len(sizes) > 1 else next(iter(sizes))
    return (f"manifest contradicts the trace: it declares "
            f"{man.channels} channels x {man.subslot_bytes} B = "
            f"{man.frame_bytes} B per audio frame, but packet sizes {bad} are "
            f"not whole multiples of {man.frame_bytes} B, and the device "
            f"counted no non-multiple packets. The observed sizes imply a "
            f"frame of {implied} B. No verdict about the host can be trusted "
            f"until the two agree")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("vcd")
    ap.add_argument("manifest")
    ap.add_argument("--json", action="store_true", help="machine-readable output")
    ap.add_argument("--correction-quantum", type=int,
                    default=DEFAULT_CORRECTION_QUANTUM_BYTES,
                    help="device OUT_BUFFER_PREFILL in bytes")
    ap.add_argument("--observer-sha", default="unknown")
    args = ap.parse_args(argv)

    try:
        man = Manifest.load(args.manifest)
    except (ManifestError, OSError, ValueError) as e:
        # Not a host failure and not a capture failure -- the operator's
        # declaration is wrong or unreadable. 2 rather than 1, for the same
        # reason an unusable capture is 2: it says nothing about the host.
        print(f"uacvalidate: manifest unusable: {e}", file=sys.stderr)
        return 2

    header = {"vcd": args.vcd, "duration_s": 0.0, "missing_marks": 0,
              "observer_sha": args.observer_sha, "host_note": man.host_note}

    try:
        blocks = read_blocks(args.vcd)
    except (OSError, ValueError) as e:
        # read_blocks refuses a capture that does not declare or does not open
        # with the whole block. That refusal is the point -- carrying zeros
        # forward for absent words would hand the rules plausible-looking data
        # -- but it must arrive as INVALID, not as a traceback.
        return _emit(header, [Verdict("CAPTURE", INVALID,
                                      f"capture unreadable: {e}")], args.json)

    if not blocks:
        return _emit(header, [Verdict("CAPTURE", INVALID,
                                      "no validator state blocks found in the "
                                      "capture")], args.json)

    header["duration_s"] = blocks[-1][0] - blocks[0][0]

    # Probes 0-3 come through vcdfill: the fill trace is the drift instrument.
    # Missing marks are counted from the VCD directly rather than from
    # vcdfill's return value, which is keyed by VCD identifier and does not
    # surface the Missing_Data signal by name.
    #
    # The decoupler probes are optional cargo, so a capture whose probe
    # section is malformed still yields a judgeable state block: the fit is
    # abandoned and W1/W3 report SKIP with the missing witness they already
    # name. What must NOT happen is the exception escaping -- that exits 1,
    # and exit 1 means "this host failed a rule".
    try:
        fill = parse_fill(args.vcd)
    except (OSError, ValueError, KeyError, IndexError):
        fill = {}

    missing = count_missing_marks(args.vcd)
    header["missing_marks"] = missing

    # Two capture-level invalidations, both BEFORE any rule runs. Neither may
    # fall through: a rule verdict computed from these captures would be an
    # assertion about the host derived from data known to be untrustworthy.
    if missing:
        return _emit(header, [Verdict(
            "CAPTURE", INVALID,
            f"xscope dropped data ({missing} missing marks): every counter in "
            f"this report is a lower bound, so no verdict can be trusted")],
            args.json)

    _, first = blocks[0]
    _, last = blocks[-1]
    if last.magic != MAGIC:
        return _emit(header, [Verdict(
            "CAPTURE", INVALID,
            f"state block magic is 0x{last.magic:08X}, expected 0x{MAGIC:08X}: "
            f"the observer and this judge disagree about the wire format")],
            args.json)

    # MAGIC catches a different format; VERSION catches a revised one, which
    # is the more dangerous of the two because it still parses. A renumbered
    # block from a newer observer has the right magic and the wrong word
    # meanings, so every rule would read the wrong counter and report a
    # confident verdict about the wrong quantity.
    if last.version != VERSION:
        return _emit(header, [Verdict(
            "CAPTURE", INVALID,
            f"state block version is {last.version}, this judge understands "
            f"version {VERSION}: the observer's word layout may have been "
            f"revised, and the block would parse into the wrong fields "
            f"rather than fail")], args.json)

    # The spec's second report-invalidating condition: the manifest contradicts
    # the trace. A packet size that is not a whole number of the DECLARED audio
    # frame, while the device -- which knows the real frame size -- counted no
    # non-multiple packets, means the two sides disagree about the format. That
    # is an operator error, and running the rules on it produces cited FAILs
    # against a host that did nothing wrong.
    contradiction = _manifest_contradiction(first, last, man)
    if contradiction:
        return _emit(header, [Verdict("CAPTURE", INVALID, contradiction)],
                     args.json)

    verdicts = [rule(blocks, man) for rule in rules.ALL_RULES]

    try:
        slope = fit_fill_slope(fill)
    except (ValueError, TypeError, ZeroDivisionError):
        slope = None
    # R1 leads: whether the host reads feedback at all frames how W1 and W3
    # should be read. It takes the slope so its consequence can name a glitch
    # cadence rather than just saying "unbounded".
    verdicts.insert(0, rules.r1_feedback_polled(
        blocks, man, args.correction_quantum, slope))
    verdicts.append(rules.w1_residual_drift(blocks, man, slope,
                                            args.correction_quantum))
    verdicts.append(rules.w3_feedback_tracked(blocks, man, slope))

    return _emit(header, verdicts, args.json,
                 metrics.collect(blocks, man, fill, slope,
                                 probe_reader=probe_points))


if __name__ == "__main__":
    sys.exit(main())
