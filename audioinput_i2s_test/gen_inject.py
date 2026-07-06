#!/usr/bin/env python3
# Task 4 integration gate: generate a long, loud, right-channel-carrying sine
# for AudioInputI2S -> AudioAnalyzePeak to detect within the firmware's 500ms
# polling window.
#
# Unlike sai_rx_test's short "one block per stage" injection (each stage there
# permanently drains a fixed slice of the one-shot rx_ring), this gate just
# needs the peak detector to see *some* non-silent block(s) while the firmware
# polls, so we emit a much longer buffer (2400 frames = 25 back-to-back
# 96-frame periods) and the run script loop-feeds it into the injector fifo so
# the SAI model's rx_ring keeps getting refilled for as long as QEMU runs --
# no risk of draining dry mid-poll. 2400 frames/feed is still far under the
# QEMU SAI model's 16384-*sample* rx_ring (imxrt_sai.h IMXRT_SAI_TX_SAMPLES =
# 8192 stereo frames), so imxrt_sai_rx_can_receive() never stalls a single feed.
#
# Right channel (mic channel, per input_i2s.cpp's isr: even samples -> left,
# odd -> right) carries the full-amplitude sine; left carries a scaled-down
# copy purely so a left-channel check (if ever needed) also sees a non-zero
# signal. Amplitude 0x6000 (~49% FS) comfortably clears the peak.read() >
# 0.02f gate threshold (~0.02 * 32767 = 655) on the right channel.
import sys, struct, math
PI_C = 3.14159265358979
out = open(sys.argv[1], "wb")
for i in range(96 * 25):
    v = int(0x6000 * math.sin(2.0 * PI_C * (i % 48) / 48.0))
    v = ((v + 0x8000) & 0xFFFF) - 0x8000
    left = int(v / 2)
    right = v
    out.write(struct.pack("<hh", left, right))
out.close()
