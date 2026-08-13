#!/usr/bin/env python3
# Assert what the SAI1 TX tap carries.
#
#   check_tap.py FILE                  -- non-silent: peak > 4000 (the tone)
#   check_tap.py --expect-silence FILE -- silent AT RATE: peak == 0 and the
#                                         tap grew past --min-bytes
#
# The silence mode is NOT the trivial inverse. An empty tap is also "not loud",
# and an empty tap means the graph never ran -- so the mode asserts a MINIMUM
# SIZE as well as a zero peak. That pair distinguishes "the graph is running
# and there is genuinely nothing to play" (usb_audio_capstone_test in QEMU,
# where the emulated usb-audio device is never even claimed) from "the graph is
# dead", which would otherwise look identical.
import sys, struct

args = sys.argv[1:]
expect_silence = False
min_bytes = 131072          # ~1 s of tap at the observed QEMU drain rate
if "--expect-silence" in args:
    expect_silence = True
    args.remove("--expect-silence")
if "--min-bytes" in args:
    i = args.index("--min-bytes")
    min_bytes = int(args[i + 1])
    del args[i:i + 2]
path = args[0]

data = open(path, "rb").read()
n = len(data) // 2
if n == 0:
    print("STAGE_TONE=FAIL (empty tap)"); sys.exit(1)
samples = struct.unpack("<%dh" % n, data[:n*2])
peak = max(abs(s) for s in samples)
print("info tap_peak=%d (%.3f fs) bytes=%d" % (peak, peak/32767.0, len(data)))

if expect_silence:
    if len(data) < min_bytes:
        print("STAGE_SILENCE=FAIL (tap only %d bytes, want >= %d -- graph not running "
              "at rate)" % (len(data), min_bytes))
        sys.exit(1)
    ok = peak == 0
    print("STAGE_SILENCE=PASS" if ok else
          "STAGE_SILENCE=FAIL (expected pure silence, saw peak=%d)" % peak)
    sys.exit(0 if ok else 1)

# amplitude 0.5 full-scale -> ~16384; accept a wide band (QEMU FIFO/timing).
ok = peak > 4000
print("STAGE_TONE=PASS" if ok else "STAGE_TONE=FAIL")
sys.exit(0 if ok else 1)
