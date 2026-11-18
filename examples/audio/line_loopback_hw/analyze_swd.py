#!/usr/bin/env python3
"""analyze_swd.py ELF [--user-script PY] [--save DIR] -- read line_loopback_hw's
recordings out of SDRAM over SWD and measure them on the host.

Attaches without halting (the sketch idles in loop() once rec_done = 1, so a
read cannot catch an I2C transfer), reads rec_tone_L/R and rec_quiet_L/R, then:

  level   peak amplitude from a least-squares sine fit, dBFS (sent -6.0)
  THD     H2..H10 power over the fundamental (Blackman-windowed FFT)
  SNR     fundamental over the 20 Hz - 20 kHz noise, harmonics and the
          other channel's tone excluded
  xtalk   the other channel's tone over the fundamental
  fit     residual after the sine fit; a dropped or repeated block would
          leave a step near the signal level
  floor   muted recording, wideband RMS

Needs pyOCD and numpy. On the RT1176, pyOCD's own target hooks install trap
code on attach; pass --user-script with a script that disables them (Nosh:
nosh-dev-board/tools/pyocd_notrap.py).
"""
import argparse, os, subprocess, sys
import numpy as np
from pyocd.core.helpers import ConnectHelper

FS = 44100.0
TONES = {'L': 1000.0, 'R': 1500.0}

def symbols(elf, nm):
    out = subprocess.run([nm, '-S', elf], capture_output=True, text=True, check=True).stdout
    syms = {}
    for line in out.splitlines():
        f = line.split()
        if len(f) == 4 and f[3].startswith('rec_'):
            syms[f[3]] = (int(f[0], 16), int(f[1], 16))
    return syms

def read_recordings(args):
    syms = symbols(args.elf, args.nm)
    opts = {'user_script': args.user_script} if args.user_script else {}
    sess = ConnectHelper.session_with_chosen_probe(target_override='mimxrt1170_cm7', connect_mode='attach',
                                                   frequency=4000000, options=opts)
    sess.open()
    t = sess.target
    if t.read32(syms['rec_done'][0]) != 1:
        sys.exit('rec_done is not 1: the sketch has not finished recording')
    rec = {}
    for name, (addr, size) in syms.items():
        if name != 'rec_done':
            rec[name] = np.frombuffer(bytes(t.read_memory_block8(addr, size)), dtype='<i2')
    return rec

def sine_fit(x, f0):
    n = np.arange(len(x)) / FS
    A = np.column_stack([np.sin(2 * np.pi * f0 * n), np.cos(2 * np.pi * f0 * n), np.ones(len(x))])
    c, *_ = np.linalg.lstsq(A, x, rcond=None)
    return np.hypot(c[0], c[1]), x - A @ c

def spectrum(x):
    w = np.blackman(len(x))
    P = np.abs(np.fft.rfft((x - x.mean()) * w)) ** 2
    return np.fft.rfftfreq(len(x), 1 / FS), P

def band(f, P, fc, hw=8):
    i = int(np.argmin(abs(f - fc)))
    return P[max(i - hw, 0):i + hw + 1].sum(), slice(max(i - hw, 0), i + hw + 1)

def measure(ch, x, other):
    f0 = TONES[ch]
    amp, resid = sine_fit(x, f0)
    f, P = spectrum(x)
    sig, _ = band(f, P, f0)
    harm = sum(band(f, P, k * f0)[0] for k in range(2, 11) if k * f0 < 20000)
    xt, _ = band(f, P, other)
    keep = (f >= 20) & (f <= 20000)
    noise_bins = keep.copy()
    for fc in [k * f0 for k in range(1, 11)] + [other]:
        noise_bins[band(f, P, fc)[1]] = False
    noise = P[noise_bins].sum() * keep.sum() / noise_bins.sum()
    blk = max(np.abs(resid[i:i + 1024]).max() for i in range(0, len(resid) - 1024, 1024))
    print(f"{ch} {f0:.0f} Hz: level {20*np.log10(amp):6.2f} dBFS  THD {10*np.log10(harm/sig):6.1f} dB  "
          f"SNR {10*np.log10(sig/noise):5.1f} dB  xtalk {10*np.log10(xt/sig):6.1f} dB  "
          f"fit residual {20*np.log10(np.sqrt(np.mean(resid**2))):6.1f} dBFS rms, "
          f"worst 1024-sample block {20*np.log10(blk):6.1f} dBFS")
    print('   ' + '  '.join(f"H{k} {10*np.log10(band(f, P, k*f0)[0]/sig):6.1f} dBc" for k in range(2, 6)))

def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('elf')
    ap.add_argument('--user-script', help='pyOCD user script (Nosh: nosh-dev-board/tools/pyocd_notrap.py)')
    ap.add_argument('--nm', default=os.path.join(os.environ.get('ARM_TOOLCHAIN_BIN', '/Applications/ARM_10/bin'),
                                                 'arm-none-eabi-nm'))
    ap.add_argument('--save', help='also write each recording as raw int16 to this directory')
    args = ap.parse_args()
    rec = read_recordings(args)
    if args.save:
        os.makedirs(args.save, exist_ok=True)
        for name, x in rec.items():
            x.tofile(os.path.join(args.save, name + '.s16'))
    fs = {k: v.astype(float) / 32768 for k, v in rec.items()}
    measure('L', fs['rec_tone_L'], TONES['R'])
    measure('R', fs['rec_tone_R'], TONES['L'])
    for ch in 'LR':
        q = fs['rec_quiet_' + ch]; q = q - q.mean()
        print(f"floor {ch}: {20*np.log10(np.sqrt(np.mean(q**2))):6.1f} dBFS rms, peak {np.abs(q).max()*32768:.0f} LSB")
    sys.stdout.flush()
    os._exit(0)   # skip pyOCD's session teardown, which would touch the core

if __name__ == '__main__':
    main()
