#!/bin/sh
# gate-vacuity.test.sh — proves the gate runners FAIL when they should.
#
# Companion to license-audit.test.sh, and written for the same reason: a check
# that cannot be shown to fire is not a check. These cases pin the two defects
# swept out of all 68 runners on 2026-07-29, both of which had the property that
# the gate looked fine while proving nothing.
#
#   1. A dead QEMU must fail BY NAME. `kill $P` under `set -e` fails when QEMU
#      has already exited, errexit fires, and the gate dies before any assertion
#      runs -- exit 1 with no message. gate_reap's `|| true` plus
#      gate_require_capture turn that into a named failure.
#   2. A missing token is not proof of the good outcome. The cm4 wire gates
#      asserted "the CM4 took its IRQ" with `grep -q "^irqcnt=00000000" && fail`,
#      which scores a MISSING irqcnt as "not zero, therefore it fired" -- the
#      assertion passed most confidently in the one case it knew nothing about.
#
# Method: run REAL gate scripts against a fake qemu injected through tools/qrun's
# REAL_QEMU hook, so the run's output is whatever we choose. Fixtures are the
# gates' own committed transcript_qemu.txt -- version-controlled, so this test
# needs neither a prior gate run nor a working QEMU. It DOES need each covered
# example built, because the gates check for their ELF.
#
# Usage: sh tools/gate-vacuity.test.sh   (PASS:/FAIL: per case; exit 1 on any FAIL)
# Runtime ~40s: the dead-QEMU cases necessarily burn each gate's full poll
# ceiling, because "nothing ever arrives" is exactly what they are testing.
set -u
EVKB=$(cd "$(dirname "$0")/.." && pwd)
FAILED=0
report() { # <name> <0-pass|1-fail>
    if [ "$2" -eq 0 ]; then echo "PASS: $1"; else echo "FAIL: $1"; FAILED=1; fi
}

WORK=$(mktemp -d "${TMPDIR:-/tmp}/gate-vacuity.XXXXXX") || exit 2
trap 'rm -rf "$WORK"' EXIT INT TERM HUP

# Stand-in for qemu-system-arm. Writes $FAKE_CAPTURE to the `-serial file:`
# target if set, then LINGERS so the gate's reap has a live child; with it unset
# it produces nothing and exits at once, which is the dead-QEMU case.
#
# The linger is load-bearing for case 2: without it defect 1 aborts the gate
# before any assertion runs, masking defect 2 entirely. That interaction is why
# the vacuous assertions survived so long -- the failure that would have exposed
# them killed the script first.
cat > "$WORK/fake-qemu" <<'FAKE'
#!/bin/sh
target=""; prev=""
for a in "$@"; do
    case "$a" in file:*) [ "$prev" = "-serial" ] && target="${a#file:}" ;; esac
    prev="$a"
done
if [ -n "${FAKE_CAPTURE:-}" ]; then
    [ -n "$target" ] && cat "$FAKE_CAPTURE" > "$target"
    sleep 300      # qrun's gtimeout and the gate's own reap both bound this
fi
exit 0
FAKE
chmod +x "$WORK/fake-qemu"

# run_gate <example-rel-path> <gate-filename> [capture-fixture] -> rc, output in $OUT_TEXT
run_gate() {
    _dir="$EVKB/$1"; _gate="$2"; _cap="${3:-}"
    OUT_TEXT=$( cd "$_dir" && REAL_QEMU="$WORK/fake-qemu" FAKE_CAPTURE="$_cap" \
                GATE_TIMEOUT=120 QRUN_TIMEOUT=40 "./$_gate" 2>&1 )
    return $?
}

# --- 1. a run that produced nothing must fail, and SAY SO ------------------
# Three different runner shapes so this is not pinned to one gate's phrasing.
# Asserting the MESSAGE, not just a non-zero exit, is the point: without
# gate_require_capture these still exit non-zero, but blame the firmware
# ("banner missing") for a run that never happened.
for spec in "examples/serial/serial_test:run_qemu.sh" \
            "examples/dualcore/cm4_boot_test:run_qemu.sh" \
            "examples/display/rk055_panel_test:run_qemu.sh"; do
    rel=${spec%%:*}; gate=${spec##*:}
    name="dead_qemu_named_${rel##*/}"
    if [ ! -d "$EVKB/$rel" ]; then echo "SKIP: $name (no such example)"; continue; fi
    run_gate "$rel" "$gate"; rc=$?
    result=0
    [ "$rc" -ne 0 ] || result=1                                   # must not pass
    echo "$OUT_TEXT" | grep -q "FAIL: no UART capture" || result=1 # must name it
    report "$name" $result
done

# --- 2. a missing counter token must not read as "the IRQ fired" -----------
# Fixture is the gate's own committed transcript with the counter line deleted:
# a firmware that stopped reporting it, and for cm4_wire_dma_test precisely the
# RED-scaffold token timeout its own header describes.
for spec in "examples/dualcore/cm4_wire_int_master_test:irqcnt" \
            "examples/dualcore/cm4_wire_int_slave_test:irqcnt" \
            "examples/dualcore/cm4_wire_dma_test:dmairq"; do
    rel=${spec%%:*}; tok=${spec##*:}; short=${rel##*/}
    src="$EVKB/$rel/transcript_qemu.txt"
    if [ ! -f "$src" ]; then echo "SKIP: missing_${tok}_${short} (no transcript)"; continue; fi

    grep -v "^$tok=" "$src" > "$WORK/$short.no-$tok"
    run_gate "$rel" "run_qemu.sh" "$WORK/$short.no-$tok"; rc=$?
    result=0
    [ "$rc" -ne 0 ] || result=1                                # a vacuous PASS is the bug
    echo "$OUT_TEXT" | grep -q "$tok not reported" || result=1 # and it must name the token
    report "missing_${tok}_${short}" $result

    # Over-correction guard: the UNTOUCHED transcript must still pass, or the
    # case above would be satisfied by a gate that simply always fails.
    cp "$src" "$WORK/$short.green"
    run_gate "$rel" "run_qemu.sh" "$WORK/$short.green"; rc=$?
    [ "$rc" -eq 0 ] && result=0 || result=1
    report "green_still_passes_${short}" $result

    # The fabricated capture looks exactly like a real green run; leaving it in
    # the example directory would mislead the next reader. Gates rm -f it on
    # start, so this is tidiness, not correctness.
    rm -f "$EVKB/$rel"/*.uart
done

exit $FAILED
