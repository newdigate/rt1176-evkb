#!/usr/bin/env python3
import sys, struct, math
PI_C = 3.14159265358979
out = open(sys.argv[1], "wb")
for i in range(96):
    v = int(0x6000 * math.sin(2.0 * PI_C * (i % 48) / 48.0))
    v = ((v + 0x8000) & 0xFFFF) - 0x8000
    out.write(struct.pack("<hh", v, int(v / 2)))
out.close()
