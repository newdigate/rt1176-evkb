#!/bin/sh
# pxp_rotate_probe -- Phase 0 of the acid_box landscape design
# (docs/superpowers/specs/2026-09-14-acid-box-landscape-design.md, section 4).
#
# WHAT THIS GATE PROVES: the PXP rotates a 1280x720 XRGB8888 canvas into a
# 720x1280 buffer correctly, at the origin (`full`) AND at pointer-offset
# sub-rectangles on the 16-px grid, with nothing written outside the target
# (a whole-buffer sentinel predicate, on the firmware side).  It is the
# regression pin for the present path acid_box's landscape build stands on.
#
# WHAT IT CANNOT PROVE: timing.  Phase B runs after crc_done and is not
# waited for -- QEMU time is a fiction; transcript_hw_evkb.txt has the numbers.
#
# VACUITY GUARDS COME FIRST.  Every pin below is satisfied VACUOUSLY by an
# empty or truncated case table, so the case_begin count, the case count and
# the tally line are asserted before any sum is looked at.  How a HUNG op
# actually presents, checked against the driver: a PXP timeout returns
# PXP_ERR_TIMEOUT, prints pixel=skip and fails the tally; an op that never
# returns never reaches crc_done, which fails first.  The counts are what catch
# a case ADDED to the firmware's table without a pin here (12 -> 13).
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/pxp_rotate_probe.elf"
OUT=$(gate_capture_path "$DIR" pxp_rotate_probe.uart)
DBG=$(gate_capture_path "$DIR" pxp_rotate_probe.dbg)
rm -f "$OUT"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none $(gate_console "$OUT") -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
# Wait for the LAST line this gate parses (crc_done), never an earlier token
# -- the m2_rx_demo mid-line-reap lesson.  `kill -0` leaves the loop as soon as
# QEMU has died, so an early exit costs 0.25 s instead of the full 50 s.
for _ in $(seq 1 200); do
    kill -0 $P 2>/dev/null || break
    [ -f "$OUT" ] && grep -q "^crc_done" "$OUT" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"

fail() { echo "FAIL: $1"; exit 1; }
grep -q "pxp_rotate_probe up" "$OUT" || fail "banner missing"
grep -q "^PXP_BEGIN=PASS"     "$OUT" || fail "PXP begin"
grep -q "^crc_done"           "$OUT" || fail "case table never finished (crc_done missing) -- an op that never returned presents exactly like this"

# --- vacuity guards ---------------------------------------------------------
NB=$(grep -c "^case_begin=" "$OUT" || true)
NC=$(grep -c "^case="       "$OUT" || true)
[ "$NB" -eq 12 ] || fail "expected 12 case_begin lines, got $NB"
[ "$NC" -eq 12 ] || fail "expected 12 case lines, got $NC"
grep -qE "^cases=12 ok=11 broken=0\r?$" "$OUT" || fail "tally line missing or not cases=12 ok=11 broken=0"
# No GRADED case may be broken; offgrid is an observation and is excluded by name.
if grep "pixel=broken" "$OUT" | grep -qv "^case=offgrid "; then
    fail "a graded case reported pixel=broken"
fi

# --- the pins ---------------------------------------------------------------
# FNV-1a over the whole 720x1280 destination after each op.  These are
# REGRESSION pins, not correctness claims: correctness is the firmware-side
# pixel= predicate above.  Recorded from the first QEMU run after every graded
# case read pixel=ok; on a mismatch work out which of {probe geometry, PXP
# driver, QEMU model} moved -- never paste in whatever the run printed.
# ★ EVERY SUM ENDS IN C5, and that is FNV-1a, not a bug: the low k bits of the
# hash depend only on the low k bits of each input byte, and the offset basis
# ends in 0xC5.  The pins carry ~24 bits; do not chase the shared suffix.
for want in \
  "case=full api=ok pixel=ok sum=0xDF61D3C5" \
  "case=corner-tl api=ok pixel=ok sum=0xD087F7C5" \
  "case=corner-tr api=ok pixel=ok sum=0x0FFA55C5" \
  "case=corner-bl api=ok pixel=ok sum=0xBD3661C5" \
  "case=corner-br api=ok pixel=ok sum=0x22537BC5" \
  "case=knob api=ok pixel=ok sum=0x07E8D7C5" \
  "case=cell api=ok pixel=ok sum=0x040B7FC5" \
  "case=strip-top api=ok pixel=ok sum=0xC5B563C5" \
  "case=strip-left api=ok pixel=ok sum=0x3A4C71C5" \
  "case=centre api=ok pixel=ok sum=0x8CB3A5C5" \
  "case=tall api=ok pixel=ok sum=0x19431BC5"; do
    grep -qF "$want" "$OUT" || fail "missing/wrong: $want"
done
# P5 is an observation: the line must exist with a legal shape, any verdict.
grep -qE "^case=offgrid api=(ok|err[0-9]+) pixel=(ok|broken|skip) sum=0x[0-9A-F]{8}\r?$" "$OUT" \
    || fail "offgrid observation line missing"

echo "PASS: pxp_rotate_probe -- 11 graded rotations pixel-exact, offgrid observed"
