#!/usr/bin/env python3
# 960 frames = ten back-to-back 96-frame blocks of the identical periodic
# (period-48) sine. Stage A polled-reads block 1 (frames 0-95); Stage A's
# read permanently drains QEMU's one-shot rx_ring (imxrt_sai.c rx_level), so
# each subsequent stage needs its own fresh block: Stage B's DMA capture
# drains block 2 (frames 96-191), and Stage C's full-duplex RX DMA needs
# several more blocks on top of that to keep rxBlockCount() advancing up to
# >=4. Since 96 is an exact multiple of the 48-sample period, every block is
# byte-identical -- all stages validate against the same expect_sine[] built
# from i in 0..95, no C++ expected-data change needed. 960 samples/channel is
# still well under the QEMU SAI model's 16384-sample rx_ring (imxrt_sai.h
# IMXRT_SAI_TX_SAMPLES), so imxrt_sai_rx_can_receive() never stalls the
# injector.
import sys, struct, math
PI_C = 3.14159265358979
out = open(sys.argv[1], "wb")
for i in range(96 * 10):
    v = int(0x6000 * math.sin(2.0 * PI_C * (i % 48) / 48.0))
    v = ((v + 0x8000) & 0xFFFF) - 0x8000
    out.write(struct.pack("<hh", v, int(v / 2)))
out.close()
