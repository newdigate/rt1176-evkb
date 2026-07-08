#!/usr/bin/env python3
# Generate a deterministic canonical 16-bit PCM 44.1 kHz WAV for the gate.
#   gen_wav.py mono   out.wav   -> mono, 440 Hz cosine
#   gen_wav.py stereo out.wav   -> stereo, L=440 Hz R=880 Hz (distinct -> a
#                                  channel swap fails the sample-exact check)
# cosine starts at +AMP (non-zero) so check_tap.py can unambiguously find the
# audio start against leading silence. python `wave` emits WAVE_FORMAT_PCM (tag
# 1, not EXTENSIBLE), exactly what AudioPlaySdWav requires.
import sys, math, wave, struct
kind, out = sys.argv[1], sys.argv[2]
RATE = 44100
N = int(RATE * 0.20)          # 200 ms -> ~8820 frames (short, fully captured)
AMP = 12000                   # safely inside int16, non-trivial amplitude
def cosine(freq):
    return [int(AMP * math.cos(2*math.pi*freq*i/RATE)) for i in range(N)]
w = wave.open(out, "wb")
w.setsampwidth(2); w.setframerate(RATE)
if kind == "mono":
    w.setnchannels(1)
    w.writeframes(b"".join(struct.pack("<h", s) for s in cosine(440)))
else:
    w.setnchannels(2)
    L, R = cosine(440), cosine(880)
    w.writeframes(b"".join(struct.pack("<hh", l, r) for l, r in zip(L, R)))
w.close()
print("wrote %s (%s, %d frames)" % (out, kind, N))
