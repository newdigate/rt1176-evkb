#!/usr/bin/env bash
# Build, capture and judge one corpus case.
#
#   ./run_case.sh B_uac2_no_feedback
#
# ORDER MATTERS, and not the way the bench's other scripts do it. The host
# under test is flashed BEFORE the collector starts, which is the opposite of
# the usual collector-first rule -- and the corpus caught why on its first run.
#
# Collector-first leaves whatever firmware was already on the board streaming
# into the capture during the settle window. Every other bench script gets
# away with that because the resident firmware IS the thing under test. Here
# it is the PREVIOUS case's host, or master. Case B's first run recorded 36577
# feedback polls from a host that has no feedback reader compiled in at all:
# 36577 / 1000 polls/s = 36.6 s, exactly the settle window, during which
# master's 1000 polls/s build was still running. The judge then attributed the
# old host's behaviour to the new one and W1 fitted its flat, closed-loop
# stretch.
#
# Flashing first removes the possibility: `flash ... load` resets into the new
# image, which sits at `audio=none pkts/s=0` because no witness exists yet, and
# the only traffic the capture can contain is the host under test talking to
# the witness the collector itself launched.
#
# Other bench rules encoded here:
#   * the collector is ended with SIGINT, never kill -9. A -9'd xrun orphans
#     xgdbserver holding the XTAG, and the next run reports "device is in use";
#   * nothing holds the VCOM during a LinkServer operation -- with the console
#     attached, `flash ... load` dies mid-attempt and has kernel-panicked this
#     Mac (see the mac-kernel-panic memory). Flash-first makes the console safe
#     to hold for the whole capture, which is why each case now gets a console
#     log: case E's entire claim is that the host SAYS it is streaming while
#     transmitting nothing, and that half of the evidence is host-side;
#   * stale probe daemons are cleared first: pkill LinkServer alone leaves
#     redlinkserv and crt_emu_cm_redlink resident, which silently kills the
#     next few runs.
#
# The VCD is written to the archive OUTSIDE the repo (see README: a corpus of
# 20-minute HS captures is >100 MB even gzipped). What lands in git is the
# report this prints and checks.
set -u -o pipefail

CASE_ID="${1:?usage: run_case.sh <case-id> [extra-seconds]}"
EXTRA_S="${2:-0}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EVKB="$(cd "$HERE/../../.." && pwd)"
ARCHIVE="$HOME/Development/rt1170/uacv-captures/corpus"
IMAGES="$HOME/Development/rt1170/uacv-captures/images"
USBHOST="$HOME/Development/USBHost_t36"
LS=/Applications/LinkServer_26.6.137/LinkServer
XTAG=3LajHPG5

mkdir -p "$ARCHIVE" "$HERE/reports"

read -r COMMIT WITNESS DURATION < <(python3 - "$HERE/cases.json" "$CASE_ID" <<'PY'
import json, sys
cases = json.load(open(sys.argv[1]))["cases"]
c = next((x for x in cases if x["id"] == sys.argv[2]), None)
if c is None:
    sys.exit(f"no such case: {sys.argv[2]}")
print(c["commit"], c["witness_config"], c["duration_s"])
PY
) || exit 2

case "$WITNESS" in
  2AMi8o8xxxxxx) XE="$IMAGES/uac2_passive_b424b0e0.xe" ;;
  # The 16-bit build, and only that one: a 24-bit UAC1 witness makes
  # uac1_find_alt miss, the host never claims, and the capture looks exactly
  # like a host that never tried. That mis-read cost a published retraction.
  1AMi2o2xxxxxx) XE="$IMAGES/uac1_16bit_witness.xe" ;;
  *) echo "unknown witness config: $WITNESS" >&2; exit 2 ;;
esac
[ -f "$XE" ] || { echo "missing witness image: $XE" >&2; exit 2; }

VCD="$ARCHIVE/$CASE_ID"
MANIFEST="$HERE/reports/$CASE_ID.manifest.json"
python3 - "$HERE/cases.json" "$CASE_ID" "$MANIFEST" <<'PY'
import json, sys
cases = json.load(open(sys.argv[1]))["cases"]
c = next(x for x in cases if x["id"] == sys.argv[2])
json.dump(c["manifest"], open(sys.argv[3], "w"), indent=2)
PY

echo "=== $CASE_ID: $COMMIT, witness $WITNESS, ${DURATION}s ==="

echo "[$(date +%T)] checking out $COMMIT and building the corpus host"
git -C "$USBHOST" checkout -q --detach "$COMMIT" || exit 2
git -C "$USBHOST" log --oneline -1
rm -rf "$HERE/host/build-corpus"
( cd "$HERE/host" && cmake -B build-corpus \
    -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake >/dev/null 2>&1 \
  && cmake --build build-corpus >/dev/null 2>&1 ) \
  || { echo "BUILD FAILED at $COMMIT" >&2; git -C "$USBHOST" checkout -q master; exit 2; }
ELF="$HERE/host/build-corpus/corpus_host.elf"
echo "[$(date +%T)] built $(basename "$ELF")"

set +u; source /Applications/XMOS_XTC_15.3.1/SetEnv.sh >/dev/null 2>&1; set -u

# 1. Flash the host under test FIRST, with nothing holding the VCOM. This
#    also evicts the previous case's firmware, which is the whole point.
pkill LinkServer 2>/dev/null; pkill redlinkserv 2>/dev/null
pkill crt_emu_cm_redlink 2>/dev/null; sleep 2
echo "[$(date +%T)] flashing the host under test (no collector yet, VCOM free)"
"$LS" flash MIMXRT1176:MIMXRT1170-EVKB load "$ELF" > "$VCD.ls.log" 2>&1
if ! grep -q "restart on reset" "$VCD.ls.log"; then
    echo "FLASH FAILED -- refusing to capture; a capture taken now would" >&2
    echo "record the PREVIOUS case's host under this case's name." >&2
    tail -5 "$VCD.ls.log" >&2
    git -C "$USBHOST" checkout -q master
    exit 2
fi
pkill LinkServer 2>/dev/null; sleep 2

# 2. Console reader, BOUNDED. Holding the VCOM for a whole 300 s capture cost
#    an xscope missing mark on the first attempt -- this Mac serving both the
#    XTAG stream and the VCOM -- and one missing mark correctly invalidates the
#    entire report, because every counter in it becomes a lower bound. 30 s
#    covers the claim and the first heartbeats, which is all the host-side
#    evidence any case needs; the port is free for the rest of the capture.
CONSOLE_PORT="$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)"
WINDOW=$(( DURATION + EXTRA_S + 15 ))
CONSOLE_S=30
[ "$CONSOLE_S" -gt "$WINDOW" ] && CONSOLE_S="$WINDOW"
gtimeout "$CONSOLE_S" python3 "$EVKB/tools/rt1170-console.py" \
    "$CONSOLE_PORT" 115200 > "$VCD.console.log" 2>&1 &

# 3. The collector. The witness appears here and not before, so the host --
#    already up and idle at pkts/s=0 -- claims it within a second or two.
rm -f "$VCD.vcd"
echo "[$(date +%T)] collector up with the witness (${WINDOW}s)"
gtimeout -s INT "$WINDOW" xrun --adapter-id "$XTAG" --xscope-file "$VCD" "$XE" \
    > "$VCD.xrun.log" 2>&1 &
COLLECTOR=$!

wait "$COLLECTOR"
echo "[$(date +%T)] capture closed: $(stat -f%z "$VCD.vcd" 2>/dev/null || echo 0) bytes"
echo "[$(date +%T)] host said:"
grep -E "HEARTBEAT|ready|STREAM" "$VCD.console.log" 2>/dev/null | tail -3

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
