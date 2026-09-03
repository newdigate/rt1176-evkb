#!/bin/sh
# run_qemu_baud.sh -- the [baud] gate (BT-3 phase 0): the vendor set-baud
# SEQUENCE is right.  A chardev has no baud, so what the RATE does is
# silicon-only (transcript_hw_evkb.txt); this proves the driver sends 0xFC09
# with the right bytes, then re-baud()s and re-validates with a fresh Reset +
# identity -- against a peer that answers.
# ★ 2026-09-03, ON SILICON: the switch to 3 Mbaud WORKS -- but only because the
# host does NOT wait for the 0xFC09 Command Complete at the old rate.  The IW416
# acts on 0xFC09 by switching its OWN UART and returning the CC AT THE NEW RATE,
# so probeFastBaud() writes the command raw, lets rebaud()'s end() drain it at
# 115200, switches the host, and reads the CC + identity at 3 M
# (bt_baud_switch=ok rate=3000000, identity byte-identical either side).  The
# QEMU sequence is identical whether or not the host waits (a chardev has no
# baud), which is exactly why THIS gate could not catch the timing bug --
# transcript_hw_evkb.txt did.
#
# ★ OWNS ITS OWN BUILD DIRECTORY, build-baud/, with M2_BT_FAST_BAUD=ON and no
# firmware blobs (the gate build synthesises its BT image, same as every other
# m2_hci_probe gate -- see the CMakeLists comment on M2RADIO_IW416_BT_FW).
# build/ (the bench directory, real blobs, several M2_BT_* knobs ON) is never
# reconfigured or written to by this script.
#
# ★ cmds=12, NOT 9.  probeFastBaud() does not touch probeInquiry(), which
# unconditionally follows identity in setup() -- so the peer legitimately sees
# Reset, 3 identity, 0xFC09, Reset, 3 identity, Inquiry, 2 Remote_Name_Request
# = 12 commands, 2 resets.  Measured on the first real run of this gate, not
# assumed: an earlier draft asserted cmds=9 (identity-only) and it was WRONG
# against correct, unmodified firmware -- the failure is quoted at the
# assertion below.
#
# DEMONSTRATED RED (2026-09-02), twice, each reverted immediately after:
#   (a) byte order -- p[4] built big-endian instead of little-endian:
#         FAIL: [baud] the peer did not receive 0xFC09 with 921600 LE
#       (the peer decoded rate=1052160, i.e. 0x00 0E 10 00 reversed)
#   (b) sequence -- rebaud()/Reset moved AHEAD of the 0xFC09 command instead
#       of after it:
#         FAIL: [baud] wrong order: PEER-DONE phase=baud cmds=12 resets=2 opcodes=0c03,1001,1009,1005,0c03,fc09,1001,1009,1005,0401,0419,0419 baud=921600
#       (opcodes read ...,1005,0c03,fc09,1001,... -- the second Reset landed
#       BEFORE the vendor command instead of after it)
#   Confirmed green again after each revert.
#
# WHY THIS GATE BUILDS ITS OWN ELF
#   This is the tree's only self-building gate, deliberately.  The knob-ON
#   image must track the SOURCE, not a pre-built artifact somebody happened to
#   leave in build-baud/ -- a stale one would silently test old code, which is
#   exactly the "a green sweep is not evidence that a build dir can configure"
#   trap CLAUDE.md warns about for every other example's build directory.  The
#   sweep runner's own SKIP check (tools/run-all-qemu-gates.sh) only looks at
#   build/, never build-baud/, so it cannot protect this one -- the named FAIL
#   below is what a missing/broken toolchain produces here instead of a SKIP.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
[ "$(gate_board)" = rt1176 ] || { echo "FAIL: rt1176-only"; exit 1; }

# This gate owns its build: the knob must be ON here and OFF everywhere else.
# Under the vacuity harness (GATE_VACUITY=1, no ARM toolchain available) skip
# the (re)build once the ELF already exists -- it was built once by hand and
# every fixture run after that reuses it, same convention this tree already
# uses for "the sweep does not build, it only runs" (see CLAUDE.md's
# two-gate rule and gate-vacuity.test.sh's own note that it needs the covered
# examples pre-built).
BUILD_DIR="$DIR/build-baud"
ELF="$BUILD_DIR/m2_hci_probe.elf"

fail() { echo "FAIL: $*"; exit 1; }

if [ "${GATE_VACUITY:-}" = "1" ] && [ -x "$ELF" ]; then
    :
else
    mkdir -p "$BUILD_DIR"
    CONFIGURE_RC=0
    cmake -S "$DIR" -B "$BUILD_DIR" -DCMAKE_TOOLCHAIN_FILE="$EVKB/toolchain/rt1170-evkb.toolchain.cmake" \
          -DM2_BT_FAST_BAUD=ON >"$BUILD_DIR/configure.log" 2>&1 || CONFIGURE_RC=$?
    BUILD_RC=0
    if [ "$CONFIGURE_RC" -eq 0 ]; then
        cmake --build "$BUILD_DIR" >"$BUILD_DIR/build.log" 2>&1 || BUILD_RC=$?
    fi
    if [ "$CONFIGURE_RC" -ne 0 ] || [ "$BUILD_RC" -ne 0 ]; then
        fail "build-baud/ did not build -- see build-baud/configure.log / build.log"
    fi
fi

run_phase() {   # run_phase PHASE WAIT_REGEX -> $OUT $RES $PEER_RC
    PHASE=$1; WAIT=$2
    OUT="$BUILD_DIR/baud_$PHASE.uart"; DBG="$BUILD_DIR/baud_$PHASE.dbg"; RES="$BUILD_DIR/baud_$PHASE.peer"
    rm -f "$OUT" "$DBG" "$RES"
    SOCK="/tmp/m2baud_$$.sock"; rm -f "$SOCK"; gate_tmp "$SOCK"
    "$QEMU" $(gate_qemu_machine) -kernel "$ELF" -display none $(gate_console "$OUT") \
        -serial unix:"$SOCK",server -d guest_errors -D "$DBG" &
    P=$!; gate_pid $P; PEER_RC=0
    python3 "$DIR/hci_peer.py" "$PHASE" "$SOCK" > "$RES" 2>&1 || PEER_RC=$?
    for _ in $(seq 1 120); do [ -f "$OUT" ] && grep -q "$WAIT" "$OUT" 2>/dev/null && break; sleep 0.25; done
    gate_reap $P
    gate_require_capture "$OUT" "phase $PHASE"
    echo "==== captured UART ($PHASE) ===="; cat "$OUT"
    echo "==== peer ($PHASE) ===="; cat "$RES"
}

run_phase baud '^hb card=0 btfw=no_start_indication hci=ok n=1 '
grep -q "^hci_reset=ok attempts=" "$OUT"                          || fail "[baud] no first Reset"
grep -q "^bt_baud_switch=ok rate=921600[[:space:]]*$" "$OUT"       || fail "[baud] the switch did not report ok at 921600"
[ "$(grep -c '^hci_version: hci_ver=11 hci_rev=0xBEEF' "$OUT")" -eq 2 ] || fail "[baud] identity must be read TWICE: before and after the switch"
grep -q "^PEER-SETBAUD rate=921600[[:space:]]*$" "$RES"            || fail "[baud] the peer did not receive 0xFC09 with 921600 LE"
# 12, not 9: probeFastBaud() does not (and must not) suppress the B2 inquiry
# that unconditionally follows identity in setup() -- Reset, 3 identity,
# 0xFC09, Reset, 3 identity, Inquiry, 2 Remote_Name_Request.  Measured, not
# assumed: an earlier draft of this gate asserted cmds=9 and failed here on
# the FIRST real run, against unmodified (correct) firmware --
#   FAIL: [baud] expected Reset, 3 identity, 0xFC09, Reset, 3 identity = 9 commands, 2 resets
#   (actual: cmds=12 resets=2 opcodes=...,1005,0401,0419,0419)
# resets stays 2: neither Inquiry nor Remote_Name_Request is a Reset.
grep -q "^PEER-DONE phase=baud cmds=12 resets=2 " "$RES"           || fail "[baud] expected Reset, 3 identity, 0xFC09, Reset, 3 identity, Inquiry, 2 names = 12 commands, 2 resets"
# The set-baud must come AFTER the first identity and BEFORE the second Reset.
grep "^PEER-DONE" "$RES" | grep -q "opcodes=0c03,1001,1009,1005,fc09,0c03,1001,1009,1005,0401,0419,0419" || fail "[baud] wrong order: $(grep '^PEER-DONE' "$RES")"
[ "$PEER_RC" -eq 0 ] || fail "[baud] peer exited $PEER_RC"
echo "PASS: the vendor set-baud sequence is right (0xFC09 uint32 LE after identity, Reset + identity re-run); the RATE itself is silicon-only (3 Mbaud verified on the bench 2026-09-03 -- host switches WITHOUT waiting at the old rate)"
