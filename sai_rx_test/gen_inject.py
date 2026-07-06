#!/usr/bin/env python3
# 192 frames = two back-to-back 96-frame blocks of the identical periodic
# (period-48) sine: Stage A polled-reads block 1 (frames 0-95); Stage A's
# read permanently drains QEMU's one-shot rx_ring (imxrt_sai.c rx_level), so
# Stage B's DMA capture needs its own fresh block, which is block 2 (frames
# 96-191). Since 96 is an exact multiple of the 48-sample period, block 2 is
# byte-identical to block 1 -- both stages validate against the same
# expect_sine[] built from i in 0..95, no C++ expected-data change needed.
import sys, struct, math
PI_C = 3.14159265358979
out = open(sys.argv[1], "wb")
for i in range(192):
    v = int(0x6000 * math.sin(2.0 * PI_C * (i % 48) / 48.0))
    v = ((v + 0x8000) & 0xFFFF) - 0x8000
    out.write(struct.pack("<hh", v, int(v / 2)))
out.close()
