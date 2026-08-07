#!/bin/sh
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
# Tools come from THIS checkout, derived from the gate's own location. The old
# hardcoded ~/Development/rt1170/evkb/tools/... meant a worktree or a clone at
# any other path silently loaded a DIFFERENT tree's gate-lib.sh -- which surfaces
# as "gate_reap: command not found", or worse, as a gate quietly running against
# the wrong library.
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/lvgl_smoke_test.elf"; OUT="$DIR/lvgl_smoke.uart"
rm -f "$OUT"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/lvgl_smoke.dbg" &
P=$!; gate_pid $P; sleep 6; gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"
grep -q "LVGL_VERSION=9.4.0" "$OUT" || { echo "FAIL: version"; exit 1; }
grep -q "LVGL_INIT=PASS"     "$OUT" || { echo "FAIL: lv_init";  exit 1; }
# Proves lv_tick_set_cb(millis) is wired: LVGL's own tick must advance while we
# spin. Without the callback lv_tick_get() is frozen at 0 and every LVGL timer
# (and therefore every animation and refresh) silently never fires.
grep -q "LVGL_TICK=PASS"     "$OUT" || { echo "FAIL: tick";     exit 1; }
# The FNV-1a accumulator is the ONLY oracle Tasks 3/4 have (QEMU models no
# ILI9341). Proven here against the published "a" -> 0xE40C292C vector while a
# wrong answer is still visible: once a binding feeds it pixels, a defective
# accumulator would simply BECOME the golden value and the gate would be green
# and meaningless forever.
grep -q "LVGL_SUM_SELFTEST=PASS" "$OUT" || { echo "FAIL: sum selftest"; exit 1; }
# Everything above is setup() output. This is the only assertion that the loop()
# path runs -- and that lv_timer_handler() survives with no display registered.
grep -q "LVGL_LOOP="         "$OUT" || { echo "FAIL: loop";     exit 1; }
echo "PASS: LVGL init + tick rate + checksum oracle + loop verified"
