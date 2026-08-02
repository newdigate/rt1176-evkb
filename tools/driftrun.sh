#!/usr/bin/env bash
# Locked-bias drift sweep against the XMOS MC200 (UAC1 async glitch hunt).
#
# For each bias point: rebuild usb_audio_graph_test with the rate trim locked
# (-DBIAS_LOCKED_PPM), reflash the EVKB, reboot the device under
# `xrun --xscope-file`, and record the device's own OUT-FIFO fill probe for
# DWELL seconds. tools/vcdfill.py fits the fill drift per point.
#
# The point of locking per flash (rather than sweeping at runtime) is
# provenance: one firmware = one bias = one VCD, no correlating two timelines.
# The fit of measured drift (in ppm equivalent) against bias must come out at
# slope 1.000, because the trim is calibrated arithmetic -- anything else
# means the probe or the analysis is broken, not the device. The zero
# crossing is the device converter's true rate relative to host nominal.
#
# Usage: driftrun.sh <outdir> [dwell_s] [bias list...]
set -euo pipefail

OUT="${1:?usage: driftrun.sh <outdir> [dwell_s] [bias ppm...]}"
DWELL="${2:-120}"
shift $(( $# >= 2 ? 2 : 1 ))
POINTS=("${@:--250 -150 -83 0 150 250}")
[ ${#POINTS[@]} -eq 1 ] && POINTS=(${POINTS[0]})   # split default string

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXDIR="$HERE/../examples/usb/usb_audio_graph_test"
XMOS_APP="$HOME/Development/xmos/sw_usb_audio/app_usb_aud_xk_216_mc"
XE="$XMOS_APP/bin/1AMi2o2xxxxxx/app_usb_aud_xk_216_mc_1AMi2o2xxxxxx.xe"
LINKSERVER="${LINKSERVER:-/Applications/LinkServer_26.6.137/LinkServer}"
DEVICE="MIMXRT1176:MIMXRT1170-EVKB"
PORT="${RT1170_PORT:-/dev/cu.usbmodem5DQ2DDHVWO5EI3}"
XTC="/Applications/XMOS_XTC_15.3.1"

mkdir -p "$OUT"
[ -f "$XE" ] || { echo "device image missing: $XE"; exit 1; }

# VCOM discipline, verbatim from rt1170-flash.sh: a reader holding the VCOM
# during a LinkServer op re-enumerates the port mid-flash and has kernel
# panicked this Mac three times. Kill readers, then confirm the port is free.
kill_vcom_readers() {
  local pids sig
  for sig in TERM KILL; do
    pids="$( { pgrep -f 'rt1170-console\.py' || true; lsof -t -- "$PORT" 2>/dev/null || true; } | sort -un )"
    [ -z "$pids" ] && return 0
    kill -"$sig" $pids 2>/dev/null || true
    for _ in 1 2 3 4 5 6 7 8 9 10; do
      sleep 0.2
      pids="$( { pgrep -f 'rt1170-console\.py' || true; lsof -t -- "$PORT" 2>/dev/null || true; } | sort -un )"
      [ -z "$pids" ] && return 0
    done
  done
  echo "ERROR: could not free $PORT — aborting to avoid a kernel panic." >&2
  return 1
}

kill_probe_daemons() {
  pkill LinkServer 2>/dev/null || true
  pkill redlinkserv 2>/dev/null || true
  pkill crt_emu_cm_redlink 2>/dev/null || true
  sleep 1
}

cleanup() {
  pkill -f 'xrun --xscope-file' 2>/dev/null || true
  kill_vcom_readers || true
  kill_probe_daemons
}
trap cleanup EXIT

# xrun needs the XTC environment; SetEnv.sh must be sourced from its own dir,
# and it reads LD_LIBRARY_PATH unguarded, so relax nounset around it.
pushd "$XTC" >/dev/null; set +u; source SetEnv.sh; set -u; popd >/dev/null

for BIAS in "${POINTS[@]}"; do
  TAG="bias${BIAS}"
  echo "=== point $TAG: build ==="
  cmake -S "$EXDIR" -B "$EXDIR/build" \
        -DCMAKE_TOOLCHAIN_FILE="$EXDIR/toolchain/rt1170-evkb.toolchain.cmake" \
        -DBIAS_LOCKED_PPM="$BIAS" >/dev/null
  cmake --build "$EXDIR/build" >/dev/null

  echo "=== point $TAG: flash host ==="
  pkill -f 'xrun --xscope-file' 2>/dev/null || true
  kill_vcom_readers
  kill_probe_daemons
  # `LinkServer run` = load + reset + free-run; killing it afterwards leaves
  # the board running. There is no standalone reset subcommand in 26.6.137.
  "$LINKSERVER" run "$DEVICE" "$EXDIR/build/usb_audio_graph_test.elf" \
      > "$OUT/$TAG.linkserver.log" 2>&1 &
  LS_PID=$!
  # LinkServer run prints three INFO lines then goes silent while it erases,
  # programs and resets -- there is NO progress marker to watch (calibrated
  # 2026-08-02: board reset observed ~23 s after launch, log still 3 lines at
  # 120 s). The console cannot be attached to confirm either, because a VCOM
  # reader during programming kernel-panics this Mac. So: a generous fixed
  # wait, then the per-point console check below fails loudly on a bad flash.
  sleep 90
  kill $LS_PID 2>/dev/null || true
  kill_probe_daemons

  echo "=== point $TAG: reboot device under xscope ==="
  ( cd "$XMOS_APP" && xrun --xscope-file "$OUT/$TAG" --id 0 "$XE" \
      > "$OUT/$TAG.xrun.log" 2>&1 ) &
  sleep 8   # device boot + enumerate + stream start

  python3 "$HERE/rt1170-console.py" "$PORT" 115200 > "$OUT/$TAG.console.log" 2>&1 &

  echo "=== point $TAG: dwell ${DWELL}s ==="
  sleep "$DWELL"

  kill_vcom_readers
  pkill -f 'xrun --xscope-file' 2>/dev/null || true
  sleep 1

  # The heartbeat must confirm the locked bias actually ran; a stale flash
  # would poison the whole fit. And the device must actually have attached
  # and streamed -- the MC200 wedges if its xscope collector disappears, so
  # audio=none for a whole dwell means the xrun reboot did not take.
  # Fail loudly, not quietly.
  if ! grep -q "bias=${BIAS}ppm\|bias=+${BIAS}ppm" "$OUT/$TAG.console.log"; then
    echo "ERROR: $TAG console never showed bias=${BIAS}ppm — stale firmware?" >&2
    tail -3 "$OUT/$TAG.console.log" >&2 || true
    exit 1
  fi
  if ! grep -q "pkts/s=" "$OUT/$TAG.console.log"; then
    echo "ERROR: $TAG host never streamed — device did not attach?" >&2
    tail -3 "$OUT/$TAG.console.log" >&2 || true
    exit 1
  fi
  python3 "$HERE/vcdfill.py" "$OUT/$TAG.vcd" --json > "$OUT/$TAG.analysis.json"
  python3 - "$OUT/$TAG.analysis.json" "$BIAS" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))
print(f"  bias {sys.argv[2]:>5} ppm -> drift {d.get('weighted_ppm_equiv','?'):>8} ppm  "
      f"dur {d.get('duration_s','?')}s  overflow {d.get('overflow_final',0)} "
      f"dryout {d.get('dryout_final',0)} missing {d.get('xscope_missing_marks',0)}")
EOF
done

echo "=== fit ==="
python3 - "$OUT" <<'EOF'
import json, glob, re, sys
rows = []
for p in glob.glob(sys.argv[1] + "/bias*.analysis.json"):
    b = int(re.search(r"bias(-?\d+)\.analysis", p).group(1))
    d = json.load(open(p))
    if "weighted_ppm_equiv" in d:
        rows.append((b, d["weighted_ppm_equiv"]))
rows.sort()
n = len(rows)
if n < 2:
    sys.exit("need >=2 points")
sx = sum(r[0] for r in rows) / n
sy = sum(r[1] for r in rows) / n
sxx = sum((r[0] - sx) ** 2 for r in rows)
sxy = sum((r[0] - sx) * (r[1] - sy) for r in rows)
slope = sxy / sxx
icept = sy - slope * sx
resid = [r[1] - (slope * r[0] + icept) for r in rows]
rms = (sum(e * e for e in resid) / n) ** 0.5
print("bias(ppm) drift(ppm)  residual")
for (b, d), e in zip(rows, resid):
    print(f"{b:9d} {d:10.1f} {e:9.2f}")
print(f"\nslope = {slope:.4f}   (must be 1.000 — calibrated arithmetic)")
print(f"device true rate = {-icept/slope:+.1f} ppm relative to host nominal")
print(f"residual rms = {rms:.2f} ppm")
EOF
