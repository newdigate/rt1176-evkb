#!/usr/bin/env python3
# Assert the SAI1 TX tap carries the non-silent sine AudioOutputI2S transmitted.
import sys, struct
path = sys.argv[1]
data = open(path, "rb").read()
n = len(data) // 2
if n == 0:
    print("STAGE_TONE=FAIL (empty tap)"); sys.exit(1)
samples = struct.unpack("<%dh" % n, data[:n*2])
peak = max(abs(s) for s in samples)
# amplitude 0.5 full-scale -> ~16384; accept a wide band (QEMU FIFO/timing).
ok = peak > 4000
print("info tap_peak=%d (%.3f fs)" % (peak, peak/32767.0))
print("STAGE_TONE=PASS" if ok else "STAGE_TONE=FAIL")
sys.exit(0 if ok else 1)
