#!/usr/bin/env python3
import sys, struct, math
tap = sys.argv[1]
data = open(tap, "rb").read()
got = list(struct.unpack("<%dh" % (len(data)//2), data))
exp = []
# Must match the firmware's truncated pi literal (i2s_audio_test.cpp build_sine(),
# 3.14159265358979) bit-for-bit -- using math.pi's extra precision flips sin() across
# the 0.5 boundary at the four "nice angle" frames (30 deg multiples), causing a
# spurious +/-1 LSB mismatch at tap indices 40/41/136/137.
PI_C = 3.14159265358979
for i in range(96):
    v = int(0x6000 * math.sin(2.0 * PI_C * (i % 48) / 48.0))
    v = ((v + 0x8000) & 0xFFFF) - 0x8000
    exp += [v, int(v/2)]   # int(v/2) truncates toward zero to match C (int16_t)(v/2)
if len(got) < len(exp):
    print("FAIL tap short: got %d want %d" % (len(got), len(exp))); sys.exit(1)
if got[:len(exp)] != exp:
    for i in range(len(exp)):
        if got[i] != exp[i]:
            print("FAIL at %d: got %d want %d" % (i, got[i], exp[i])); sys.exit(1)
print("PASS tap matches %d samples" % len(exp)); sys.exit(0)
