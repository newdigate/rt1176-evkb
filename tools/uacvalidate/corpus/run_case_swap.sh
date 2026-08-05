#!/usr/bin/env bash
# Build, capture and judge a corpus case whose defect needs the DEVICE to
# change underneath a running host.
#
#   ./run_case_swap.sh C_streams_in_alt0        # same witness both phases
#   ./run_case_swap.sh E_device_swap_wedge      # UAC1 -> UAC2 personality swap
#
# Cases C and E both trigger on a device that re-enumerates while the host is
# already streaming. There is one XTAG, so a second `xrun` cannot run beside
# the first -- the plan's "re-run xrun mid-capture" is not stageable as
# written.
#
# The fix is to put the capture on the SECOND xrun:
#
#   phase 1   xrun <witness A>, NO capture -> host claims and streams
#   (SIGINT ends phase 1; this resets the xcore, which IS the disconnect)
#   phase 2   xrun <witness B>, WITH capture -> device returns, host does not
#             reset, and whatever the host does about it is what gets recorded
#
# That records exactly the window the case is about -- the host's behaviour
# AFTER the device changed under it -- and needs only one XTAG. Phase 1 is
# deliberately uncaptured: nothing in either case's expectations concerns the
# healthy stream that precedes the event.
set -u -o pipefail

CASE_ID="${1:?usage: run_case_swap.sh <case-id>}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EVKB="$(cd "$HERE/../../.." && pwd)"
ARCHIVE="$HOME/Development/rt1170/uacv-captures/corpus"
IMAGES="$HOME/Development/rt1170/uacv-captures/images"
USBHOST="$HOME/Development/USBHost_t36"
LS=/Applications/LinkServer_26.6.137/LinkServer
XTAG=3LajHPG5

mkdir -p "$ARCHIVE" "$HERE/reports" "$HERE/captures"

read -r COMMIT WITNESS PHASE1 DURATION < <(python3 - "$HERE/cases.json" "$CASE_ID" <<'PY'
import json, sys
cases = json.load(open(sys.argv[1]))["cases"]
c = next((x for x in cases if x["id"] == sys.argv[2]), None)
if c is None:
    sys.exit(f"no such case: {sys.argv[2]}")
stage = c.get("stage", {})
print(c["commit"], c["witness_config"],
      stage.get("phase1_witness", c["witness_config"]), c["duration_s"])
PY
) || exit 2

image_for() {
  case "$1" in
    2AMi8o8xxxxxx) echo "$IMAGES/uac2_passive_b424b0e0.xe" ;;
    1AMi2o2xxxxxx) echo "$IMAGES/uac1_16bit_witness.xe" ;;
    *) echo "" ;;
  esac
}
XE1="$(image_for "$PHASE1")"; XE2="$(image_for "$WITNESS")"
[ -f "$XE1" ] && [ -f "$XE2" ] || { echo "missing witness image" >&2; exit 2; }

VCD="$ARCHIVE/$CASE_ID"
MANIFEST="$HERE/reports/$CASE_ID.manifest.json"
python3 - "$HERE/cases.json" "$CASE_ID" "$MANIFEST" <<'PY'
import json, sys
cases = json.load(open(sys.argv[1]))["cases"]
c = next(x for x in cases if x["id"] == sys.argv[2])
json.dump(c["manifest"], open(sys.argv[3], "w"), indent=2)
PY

echo "=== $CASE_ID: $COMMIT, phase1 $PHASE1 -> phase2 $WITNESS, ${DURATION}s ==="

git -C "$USBHOST" checkout -q --detach "$COMMIT" || exit 2
git -C "$USBHOST" log --oneline -1
rm -rf "$HERE/host/build-corpus"
( cd "$HERE/host" && cmake -B build-corpus \
    -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake >/dev/null 2>&1 \
  && cmake --build build-corpus >/dev/null 2>&1 ) \
  || { echo "BUILD FAILED at $COMMIT" >&2; git -C "$USBHOST" checkout -q master; exit 2; }
ELF="$HERE/host/build-corpus/corpus_host.elf"

set +u; source /Applications/XMOS_XTC_15.3.1/SetEnv.sh >/dev/null 2>&1; set -u

# Flash BEFORE any witness exists -- see the long note in run_case.sh. Here it
# matters twice over: this script's whole premise is that the host survives a
# device change unreset, so the host must be the one under test from the very
# first enumeration, not something inherited from the previous case.
pkill LinkServer 2>/dev/null; pkill redlinkserv 2>/dev/null
pkill crt_emu_cm_redlink 2>/dev/null; sleep 2
echo "[$(date +%T)] flashing the host under test (no witness up yet)"
"$LS" flash MIMXRT1176:MIMXRT1170-EVKB load "$ELF" > "$VCD.ls.log" 2>&1
if ! grep -q "restart on reset" "$VCD.ls.log"; then
    echo "FLASH FAILED -- refusing to capture" >&2
    tail -5 "$VCD.ls.log" >&2
    git -C "$USBHOST" checkout -q master
    exit 2
fi
pkill LinkServer 2>/dev/null; sleep 2

CONSOLE_PORT="$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)"
gtimeout 45 python3 "$EVKB/tools/rt1170-console.py" \
    "$CONSOLE_PORT" 115200 > "$VCD.console.log" 2>&1 &

echo "[$(date +%T)] phase 1: $PHASE1 up, no capture -- host claims and streams"
gtimeout -s INT 90 xrun --adapter-id "$XTAG" "$XE1" > "$VCD.phase1.log" 2>&1 &
P1=$!
sleep 75
echo "[$(date +%T)] host at end of phase 1:"
grep -E "HEARTBEAT" "$VCD.console.log" 2>/dev/null | tail -1

echo "[$(date +%T)] ending phase 1 -- the device disappears under the running host"
kill -INT "$P1" 2>/dev/null
wait "$P1" 2>/dev/null
sleep 3

rm -f "$VCD.vcd"
WINDOW=$(( DURATION + 20 ))
echo "[$(date +%T)] phase 2: $WITNESS up WITH capture (${WINDOW}s); host is NOT reset"
gtimeout -s INT "$WINDOW" xrun --adapter-id "$XTAG" --xscope-file "$VCD" "$XE2" \
    > "$VCD.xrun.log" 2>&1 &
P2=$!
wait "$P2"
echo "[$(date +%T)] host during phase 2 (the host-side half of the evidence):"
grep -E "HEARTBEAT" "$VCD.console.log" 2>/dev/null | tail -3
echo "[$(date +%T)] capture closed: $(stat -f%z "$VCD.vcd" 2>/dev/null || echo 0) bytes"

git -C "$USBHOST" checkout -q master
echo "[$(date +%T)] USBHost_t36 back on $(git -C "$USBHOST" rev-parse --abbrev-ref HEAD)"

OBS=$(git -C "$HOME/Development/xmos/lib_xua" rev-parse --short HEAD 2>/dev/null || echo unknown)
python3 "$EVKB/tools/uacvalidate/cli.py" "$VCD.vcd" "$MANIFEST" \
    --observer-sha "$OBS" --json > "$HERE/reports/$CASE_ID.json"
python3 "$EVKB/tools/uacvalidate/cli.py" "$VCD.vcd" "$MANIFEST" \
    --observer-sha "$OBS" | tee "$HERE/reports/$CASE_ID.txt"
shasum -a 256 "$VCD.vcd" >> "$HERE/captures/CHECKSUMS"

echo
python3 "$HERE/check_case.py" "$HERE/cases.json" "$CASE_ID" "$HERE/reports/$CASE_ID.json"
