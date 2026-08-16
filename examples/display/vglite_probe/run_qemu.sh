#!/bin/sh
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
# Tools come from THIS checkout, derived from the gate's own location (a
# hardcoded path silently loads a different tree's gate-lib.sh from a worktree).
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/vglite_probe.elf"
# Run artifacts go through gate_capture_path, never "$DIR/<name>" -- see its
# comment in gate-lib.sh.
OUT=$(gate_capture_path "$DIR" vglite_probe.uart)
DBG=$(gate_capture_path "$DIR" vglite_probe.dbg)
rm -f "$OUT"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
# 12s: the RK055 bring-up margin lvgl_rk055_panel_test uses. The probe does no
# software rendering, so it needs no more.
sleep 12; gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"
grep -q "PANEL_OK" "$OUT" || { echo "FAIL: panel bring-up"; exit 1; }

# ★ WHAT THIS GATE PROVES, AND WHAT IT DOES NOT.
#
# QEMU has no GC355 model, so this asserts the GPU-ABSENT path: that the
# firmware ASKS whether a GPU is present, is told no, and takes its software
# path -- cleanly, without hanging. It does NOT prove anything renders on the
# GPU; that is silicon-only and lives in transcript_hw_evkb.txt. See
# docs/KNOWN-BROKEN-GATES.md.
#
# This is a real assertion, not a formality. Both of its failure modes were
# observed while building it:
#   - Without the chip-ID probe, vg_lite_init() SPINS on an absent GPU: the
#     firmware printed PANEL_OK and went silent.
#   - Without qemu2's GPU2D window stub, even READING the chip-ID register
#     raised an external abort and hung in an unhandled bus fault.
# Either regression makes this gate fail by timing out with no DONE token.
grep -qE "VGLITE_CHIP_ID=0x[0-9A-F]{8}\r?$" "$OUT" || \
    { echo "FAIL: no chip-ID probe result"; exit 1; }
grep -qE "VGLITE_INIT=ABSENT" "$OUT" || \
    { echo "FAIL: expected VGLITE_INIT=ABSENT under QEMU (no GC355 model)"; exit 1; }
# A bounded wait that gave up means the completion path is wrong even when the
# outcome looks right. Under QEMU no wait should ever run.
grep -qE "VGLITE_TIMEOUTS=0\r?$" "$OUT" || \
    { echo "FAIL: a bounded wait timed out"; exit 1; }
grep -q "VGLITE_PROBE_DONE" "$OUT" || { echo "FAIL: no completion token"; exit 1; }
echo "PASS: VGLite probe fallback verified"
