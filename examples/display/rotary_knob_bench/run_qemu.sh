#!/bin/sh
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/rotary_knob_bench.elf"
OUT=$(gate_capture_path "$DIR" rotary_knob_bench.uart)
DBG=$(gate_capture_path "$DIR" rotary_knob_bench.dbg)
rm -f "$OUT"
# Phase A (all this gate asserts) lands ~5 s in; Phase B runs after crc_done
# and is deliberately NOT waited for (spec section 8). QRUN's default 60 s cap
# is ~12x margin over crc_done.
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
# Wait for the LAST line this gate parses (crc_done), never an earlier token
# -- the m2_rx_demo mid-line-reap lesson.
for _ in $(seq 1 200); do
    [ -f "$OUT" ] && grep -q "^crc_done " "$OUT" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"

grep -q "rotary_knob_bench up" "$OUT" || { echo "FAIL: banner missing"; exit 1; }
grep -q "PANEL_OK"             "$OUT" || { echo "FAIL: panel bring-up"; exit 1; }
grep -q "^gpu=absent"          "$OUT" || { echo "FAIL: QEMU must report gpu=absent"; exit 1; }

# TRIPWIRES FIRST: a fabricated result is worse than a missing one, so these
# outrank the golden checks.
#   1. No gpu cell may report ANY per-cell OUTCOME with no GPU present -- not
#      just a result. With no GPU the only legal disposition is st=gpu-absent,
#      decided before a single vg_lite_* call; a crc, a timing, a timeout or a
#      build failure all mean the cell was entered, and a fabricated FAILURE
#      would slip past a check that only looked for fabricated successes.
#   2. gpu_err= exists only on silicon gpu lines; in a QEMU capture ANY
#      occurrence means a sw line grew the token or a gpu cell ran.
# DEMONSTRATED RED 2026-08-27: appending
# 'cell=vector/gpu/notch st=ok crc=0xDEADBEEF init_us=1 rotor_bytes=0' to a
# passing capture failed tripwire 1 by name.
if grep -E "^(cell|time)=[a-z]+/gpu/" "$OUT" \
   | grep -qE "crc=|mfps_med=|st=timeout|st=vg-overflow"; then
    echo "FAIL: a GPU cell reported a result with no GPU present"; exit 1
fi
if grep -q "gpu_err=" "$OUT"; then
    echo "FAIL: gpu_err token in a QEMU capture (sw line grew it, or a gpu cell ran)"; exit 1
fi

# GOLDEN CHECKSUMS -- FNV-1a over the whole 720x1280 XRGB8888 framebuffer,
# one per sw cell, canonical angle 45 deg, recorded 2026-08-27 from repeated
# identical QEMU runs (Tasks 3-5 history) and independently reproduced by two
# reviewers. Silicon confirmation lands in transcript_hw_evkb.txt (Task 10).
# On a mismatch work out WHICH of {rk_geometry, LVGL pin, lv_conf, scene}
# moved; never paste in whatever the run printed.
# DEMONSTRATED RED 2026-08-27: a deliberately wrong vector/sw/notch golden
# failed by name below.
for want in \
  "cell=vector/sw/notch st=ok crc=0x41193045 " \
  "cell=vector/sw/facet st=ok crc=0xB7744585 " \
  "cell=bitmap/sw/notch st=ok crc=0xCD1F02C5 " \
  "cell=bitmap/sw/facet st=ok crc=0x3837A345 " \
  "cell=strip/sw/notch st=ok crc=0x6B428FC5 " \
  "cell=strip/sw/facet st=ok crc=0x572E8105 "; do
    grep -qF "$want" "$OUT" || { echo "FAIL: missing/wrong: $want"; exit 1; }
done

# Every gpu cell must be PRESENT with the honest negative -- an absent line is
# a silently skipped cell, the SKIP-hides-in-a-count hazard.
for c in vector bitmap strip; do
  for v in notch facet; do
    grep -q "^cell=$c/gpu/$v st=gpu-absent" "$OUT" || {
        echo "FAIL: gpu cell $c/$v did not report gpu-absent"; exit 1; }
  done
done

grep -qE "^crc_done cells=12 ok=6 gpu_absent=6 failed=0[[:space:]]*$" "$OUT" || {
    echo "FAIL: crc_done tally wrong or missing"; exit 1; }
echo "PASS: rotary_knob_bench Phase A verified (6 sw goldens, 6 honest gpu negatives)"
