#!/usr/bin/env python3
"""Count sample-level discontinuities in a recording of a device playing a tone.

    analogglitch.py capture.wav "label"

The device-side validator watches the USB wire; this watches what leaves the
DAC. A block correction -- the device inserting silence when its buffer runs
dry, or dropping packets when it overflows -- is a STEP in the played tone's
phase, and that is what this measures: narrowband-filter the fundamental, take
the analytic signal, remove the best-fit linear phase ramp, and look for steps
in what is left.

Why phase rather than clicks: it is calibrated. A step's size in SAMPLES is
directly comparable to the device's correction quantum (1024 B = 32 audio
frames at 8ch x 4B), so the instrument reports the same physical quantity the
decoupler counts, not an arbitrary "event".

READ THIS BEFORE TRUSTING A NUMBER FROM IT. The predecessor of this tool
measured ~4 events/s and nearly all of it was the measurement chain, not the
device -- an afternoon of conclusions was drawn from that before anyone ran a
control. ALWAYS record a known-good configuration first and confirm the floor
is far below the effect you are looking for. Measured 2026-08-05 with xrec in
hog mode: 0 events in 119 s, phase noise 0.021-0.41 millisamples rms, against
an open-loop host's 13 events at ~29 samples each. That headroom is what makes
the number meaningful; a shared-mode capture chain would not have it.

The first and last EDGE seconds are excluded: the analytic signal's ramp-up at
the boundaries is not data.
"""
import struct, sys
import numpy as np
def read_wav(path):
    b = open(path,'rb').read()
    assert b[:4]==b'RIFF' and b[8:12]==b'WAVE'
    i, fmt, data = 12, None, None
    while i+8 <= len(b):
        cid, sz = b[i:i+4], struct.unpack('<I', b[i+4:i+8])[0]
        body = b[i+8:i+8+sz]
        if cid==b'fmt ':
            tag, ch, sr, _, _, bits = struct.unpack('<HHIIHH', body[:16])
            sub = struct.unpack('<H', body[24:26])[0] if tag==0xFFFE and sz>=26 else tag
            fmt = (sub, ch, sr, bits)
        elif cid==b'data':
            data = body
        i += 8 + sz + (sz & 1)
    sub, ch, sr, bits = fmt
    dt = {(3,32):'<f4',(1,16):'<i2',(1,32):'<i4'}[(sub,bits)]
    x = np.frombuffer(data, dtype=dt).reshape(-1, ch).astype(np.float64)
    if dt=='<i2': x/=32768.0
    if dt=='<i4': x/=2147483648.0
    return x, sr
EDGE = 0.5   # seconds at each end where the analytic-signal ramp-up lives

def analyse(path, label):
    x, sr = read_wav(path); v = x[:,0]; n = len(v)
    X = np.fft.rfft(v); f = np.fft.rfftfreq(n, 1/sr)
    lo = np.searchsorted(f,200); k = np.argmax(np.abs(X)[lo:np.searchsorted(f,800)])+lo
    f0 = f[k]
    Y = np.zeros_like(X); Y[np.abs(f-f0) < 30.0] = X[np.abs(f-f0) < 30.0]
    Z = np.zeros(n, dtype=complex); Z[:len(Y)] = Y*2; Z[0]=Y[0]
    ph = np.unwrap(np.angle(np.fft.ifft(Z))); t = np.arange(n)
    A = np.vstack([t, np.ones(n)]).T
    coef,_,_,_ = np.linalg.lstsq(A, ph, rcond=None)
    d = np.diff(ph - A@coef)
    e0, e1 = int(EDGE*sr), n-1-int(EDGE*sr)
    interior = d[e0:e1]
    sd = np.std(interior)
    idx = np.where(np.abs(interior) > 8*sd)[0] + e0
    ev=[]
    for i in idx:
        if ev and i-ev[-1][-1] < sr//100: ev[-1].append(i)
        else: ev.append([i])
    sps = sr/(2*np.pi*f0); dur = (e1-e0)/sr
    print(f"=== {label} ===")
    print(f"  {dur:.1f} s interior, f0 {f0:.4f} Hz, floor {sd*sps*1000:.3f} millisamples rms")
    print(f"  EVENTS: {len(ev)}   rate {len(ev)/dur:.4f}/s" + (f"   (one per {dur/len(ev):.1f} s)" if ev else ""))
    for e in ev[:15]:
        print(f"     t={e[0]/sr:8.3f}s  jump {abs(d[e].sum())*sps:8.3f} samples")
    return len(ev), dur


def drift_from_glitches(path, label, rate_hz=44100.0):
    """Infer an open-loop rate error from the glitch pattern alone.

    A host that feeds a free-running device at the wrong rate fills or drains
    the device's buffer until it block-corrects. Two things are then visible
    in the recording and neither needs the device to be instrumented:

      * the SIZE of each phase step is the device's correction quantum, in
        samples -- it is whatever the device inserts or drops in one go;
      * the INTERVAL between steps is how long the accumulated error took to
        reach that quantum.

    So drift = quantum / interval samples per second, and the ppm follows by
    dividing by the sample rate. The SIGN says which way: a device being
    over-fed drops samples (phase jumps forward), under-fed it inserts
    (phase jumps back).

    This is the same drift-vs-trim measurement tools/driftrun.sh does, but
    driftrun needs the device's own xscope fill probe and so only works on
    the instrumented witness. This works on ANY device, because the evidence
    is in the audio rather than in the device.
    """
    x, sr = read_wav(path)
    v = x[:,0]; n = len(v)
    X = np.fft.rfft(v); f = np.fft.rfftfreq(n, 1/sr)
    lo = np.searchsorted(f,200); k = np.argmax(np.abs(X)[lo:np.searchsorted(f,800)])+lo
    f0 = f[k]
    Y = np.zeros_like(X); Y[np.abs(f-f0) < 30.0] = X[np.abs(f-f0) < 30.0]
    Z = np.zeros(n, dtype=complex); Z[:len(Y)] = Y*2; Z[0]=Y[0]
    ph = np.unwrap(np.angle(np.fft.ifft(Z))); t = np.arange(n)
    A = np.vstack([t, np.ones(n)]).T
    coef,_,_,_ = np.linalg.lstsq(A, ph, rcond=None)
    d = np.diff(ph - A@coef)
    e0, e1 = int(EDGE*sr), n-1-int(EDGE*sr)
    interior = d[e0:e1]; sd = np.std(interior)
    idx = np.where(np.abs(interior) > 8*sd)[0] + e0
    ev=[]
    for i in idx:
        if ev and i-ev[-1][-1] < sr//100: ev[-1].append(i)
        else: ev.append([i])
    sps = sr/(2*np.pi*f0); dur = (e1-e0)/sr
    print(f"=== {label} ===")
    print(f"  {dur:.1f} s interior, f0 {f0:.4f} Hz, floor {sd*sps*1000:.3f} millisamples rms")
    if len(ev) < 2:
        print(f"  EVENTS: {len(ev)} -- too few to infer a rate error.")
        print("  Either the host is already well trimmed, or the recording is too short.")
        return None
    steps = np.array([d[e].sum()*sps for e in ev])
    times = np.array([e[0]/sr for e in ev])
    gaps = np.diff(times)
    quantum = np.median(np.abs(steps))
    interval = np.median(gaps)
    signed = np.median(steps)
    drift_ppm = (quantum / interval) / rate_hz * 1e6 * (1.0 if signed > 0 else -1.0)
    print(f"  EVENTS: {len(ev)}, one per {interval:.2f} s (spread {gaps.std():.2f} s)")
    print(f"  step: {signed:+.2f} samples median (|q| {quantum:.2f}), "
          f"{'host OVER-feeding, device dropping' if signed > 0 else 'host UNDER-feeding, device inserting'}")
    print(f"  => open-loop drift {drift_ppm:+.1f} ppm")
    print(f"  => try -DBIAS_LOCKED_PPM={-drift_ppm:.0f}  (then re-measure; the sign")
    print(f"     convention is worth confirming empirically rather than trusted)")
    return drift_ppm

if __name__ == '__main__':
    if len(sys.argv) > 3 and sys.argv[3] == '--drift':
        drift_from_glitches(sys.argv[1], sys.argv[2])
    else:
        analyse(sys.argv[1], sys.argv[2])
