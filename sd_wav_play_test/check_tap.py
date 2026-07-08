#!/usr/bin/env python3
# Sample-exact compare of the SAI1 TX tap against the source WAV.
#   check_tap.py <tap.raw> <src.wav> <mono|stereo>
# tap.raw = interleaved LE int16 (SAI1_TDR0 writes: L,R,L,R,...). AudioOutputI2S
# always emits stereo frames; a mono source is duplicated to both channels by
# AudioPlaySdWav, so tap L==R==wav. The played audio appears after leading
# boot/header-parse silence + a fixed pipeline latency -> find the offset (first
# frame that begins a K-run exactly matching the WAV start), then assert the
# whole WAV matches. No scaling exists in the play->TDR0 path, so the match is
# exact.
import sys, struct, wave
tap_path, wav_path, kind = sys.argv[1], sys.argv[2], sys.argv[3]

w = wave.open(wav_path, "rb")
ch, sw, nf = w.getnchannels(), w.getsampwidth(), w.getnframes()
assert sw == 2, "WAV must be 16-bit"
raw = w.readframes(nf)
if ch == 1:
    m = list(struct.unpack("<%dh" % nf, raw)); wavL, wavR = m, m
else:
    inter = struct.unpack("<%dh" % (nf*2), raw)
    wavL, wavR = list(inter[0::2]), list(inter[1::2])

tb = open(tap_path, "rb").read()
tn = len(tb)//2
tap = struct.unpack("<%dh" % tn, tb[:tn*2])
tapL, tapR = tap[0::2], tap[1::2]

K = 64
def run_ok(off):
    if off + len(wavL) > len(tapL): return False
    return all(tapL[off+i] == wavL[i] and tapR[off+i] == wavR[i]
               for i in range(min(K, len(wavL))))
offset = -1
for f in range(len(tapL) - K):
    if tapL[f] == wavL[0] and tapR[f] == wavR[0] and run_ok(f):
        offset = f; break
if offset < 0:
    print("WAV_SAMPLES_EXACT=FAIL (WAV start not found in tap; tap frames=%d)" % len(tapL))
    sys.exit(1)

# AudioOutputI2S emits audio in fixed AUDIO_BLOCK_SAMPLES (128-frame) blocks, so a
# WAV whose frame count isn't a multiple of 128 ends in a PARTIAL block. The node
# zero-pads that last block at EOF; for a MONO source it also drops the partial
# block's RIGHT channel -- stock AudioPlaySdWav::update() cleanup transmits the
# left block but guarded the mono right-dup transmit on `state < 8`, which was
# false by then (state == STATE_STOP). That mono-right EOF-tail bug is now FIXED
# in the node (play_sd_wav.cpp cleanup tests block_right == NULL), so we assert
# sample-EXACTness over the whole WAV on BOTH channels: the whole-block BODY plus
# both channels of the final partial-block tail. Every count is reported. The body
# is 68 full blocks here (8704 of 8820 frames); this catches any mid-stream
# scramble, a channel swap (stereo L!=R), or a wrong offset.
BLK = 128
nbody = (len(wavL) // BLK) * BLK
body_mism = sum(1 for i in range(nbody)
                if tapL[offset+i] != wavL[i] or tapR[offset+i] != wavR[i])
tailL_mism = sum(1 for i in range(nbody, len(wavL)) if tapL[offset+i] != wavL[i])
tailR_mism = sum(1 for i in range(nbody, len(wavL)) if tapR[offset+i] != wavR[i])
ok = (body_mism == 0 and tailL_mism == 0 and tailR_mism == 0)
print("info %s tap_offset=%d body=%d body_mism=%d tail=%d tailL_mism=%d tailR_mism=%d"
      % (kind, offset, nbody, body_mism, len(wavL) - nbody, tailL_mism, tailR_mism))
print("WAV_SAMPLES_EXACT=PASS" if ok else "WAV_SAMPLES_EXACT=FAIL")
sys.exit(0 if ok else 1)
