#!/usr/bin/env bash
# wifi-soak.sh -- HARDWARE soak harness for the M.2 Wi-Fi data path.
#
# WHY THIS EXISTS
# ---------------
# QEMU has no IW416 model: every m2_* QEMU gate asserts the CARD-ABSENT
# fallback, so not one line of the SDIO ring / interrupt logic is covered by
# the sweep.  The bugs that logic has produced -- W8's 32-port ring, W12's
# stranded uploads (RX dead until reflash) -- were all found by repeated
# blasting on silicon and would sail through a green sweep.  This script is
# that missing gate.  It is deliberately NOT wired into
# tools/run-all-qemu-gates.sh: it needs a board, a probe and an AP.
#
# WHAT IT DOES
#   Per boot: reflash (a fresh fw download -- the only thing that revives a
#   wedged card), wait for the DHCP-bound status line, then run N TCP-RX
#   blasts through the example's Mac-side peer, recording which blast in the
#   boot dies.  A freeze is followed by a linger so the board's own stall
#   valve fires and dumps rd_bitmap / ring position / counters into the log.
#
# READING THE RESULT (W12 signatures -- see the example's transcript)
#   FREEZE + rd_bitmap non-zero + c53 pinned -> uploads stranded (host bug).
#   stranded= > 0                            -> the driver's safety net had
#                                               to rescue an upload: an
#                                               interrupt was lost.  0 is the
#                                               only healthy value.
#   drainerr= tracks stranded 1:1            -> those strands are the drain's
#                                               by-design error-exit clears.
#   resyncs=                                 -> ALWAYS high (tens-hundreds per
#                                               blast).  NORMALISE PER BLAST
#                                               before comparing runs; comparing
#                                               absolute counts across runs of
#                                               different length produced two
#                                               wrong conclusions in W12.
#
# USAGE
#   tools/wifi-soak.sh [-e <example>] [-i <board-ip>] [-b <boots>]
#                      [-n <blasts-per-boot>] [-m <max-minutes>] [-o <outdir>]
#   Defaults: -e networking/m2_throughput_test -b 1 -n 0 (0 = until deadline)
#             -m 25
#   The example must already be CONFIGURED AND BUILT with a firmware blob and
#   credentials -- this script only builds nothing and flashes what is there.
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXAMPLE="networking/m2_throughput_test"
IP="192.168.1.102"
BOOTS=1
BLASTS=0
MAXMIN=25
OUTDIR=""
PORT="${WIFI_SOAK_PORT:-}"
LINKSERVER="${LINKSERVER:-/Applications/LinkServer_26.6.137/dist/LinkServer}"
TARGET="${WIFI_SOAK_TARGET:-MIMXRT1176:MIMXRT1170-EVKB}"

while getopts "e:i:b:n:m:o:p:h" o; do
  case "$o" in
    e) EXAMPLE=$OPTARG ;;  i) IP=$OPTARG ;;     b) BOOTS=$OPTARG ;;
    n) BLASTS=$OPTARG ;;   m) MAXMIN=$OPTARG ;; o) OUTDIR=$OPTARG ;;
    p) PORT=$OPTARG ;;
    h) sed -n '2,40p' "$0"; exit 0 ;;
    *) exit 2 ;;
  esac
done

EXDIR="$REPO/examples/$EXAMPLE"
NAME="$(basename "$EXAMPLE")"
ELF="$EXDIR/build/$NAME.elf"
PEER="$EXDIR/tput_peer.py"
[ -x "$LINKSERVER" ] || { echo "no LinkServer at $LINKSERVER (set \$LINKSERVER)"; exit 2; }
[ -f "$ELF" ]  || { echo "no ELF at $ELF -- configure+build the example first"; exit 2; }
[ -f "$PEER" ] || { echo "no peer script at $PEER"; exit 2; }
if [ -z "$PORT" ]; then
  PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
fi
[ -n "$PORT" ] || { echo "no /dev/cu.usbmodem* found (set \$WIFI_SOAK_PORT)"; exit 2; }
[ -n "$OUTDIR" ] || OUTDIR="$(mktemp -d "${TMPDIR:-/tmp}/wifi-soak.XXXXXX")"
mkdir -p "$OUTDIR"

# The credential/blob cache is the difference between "no card" and "Wi-Fi
# compiled out", and the two are INDISTINGUISHABLE on the serial output --
# that ambiguity cost a pointless board power-cycle in W12.  Check up front.
CACHE="$EXDIR/build/CMakeCache.txt"
if [ -f "$CACHE" ]; then
  if ! grep -qE '^M2RADIO_IW416_FW:FILEPATH=.+' "$CACHE" ||
     ! grep -qE '^M2RADIO_WIFI_SSID:STRING=.+' "$CACHE"; then
    echo "REFUSING TO RUN: $NAME is built WITHOUT a firmware blob and/or"
    echo "credentials, so it compiles its Wi-Fi path out and prints the same"
    echo "alive= heartbeat as a card-absent board.  A soak of that image"
    echo "would be vacuous.  Reconfigure with -DM2RADIO_IW416_FW=... and"
    echo "-DM2RADIO_WIFI_SSID/-DM2RADIO_WIFI_PSK, then rebuild."
    exit 2
  fi
fi

reader_pid=""
kill_reader() { [ -n "$reader_pid" ] && kill "$reader_pid" 2>/dev/null; reader_pid=""; }
cleanup() { kill_reader; }
trap cleanup EXIT INT TERM

DEADLINE=$(( $(date +%s) + MAXMIN*60 ))
pass=0; froze=0; nojoin=0; total=0; boot=0

while [ "$boot" -lt "$BOOTS" ] || { [ "$BOOTS" -eq 0 ] && [ "$(date +%s)" -lt "$DEADLINE" ]; }; do
  boot=$((boot+1))
  [ "$(date +%s)" -lt "$DEADLINE" ] || break
  # VCOM must be free while programming (documented trap: holding it makes
  # LinkServer die with a DAP error mid-flash).
  kill_reader
  pkill LinkServer >/dev/null 2>&1; pkill redlinkserv >/dev/null 2>&1
  pkill crt_emu_cm_redlink >/dev/null 2>&1
  sleep 1
  "$LINKSERVER" flash "$TARGET" load "$ELF" >"$OUTDIR/flash_b${boot}.log" 2>&1
  LOG="$OUTDIR/boot_${boot}.txt"
  python3 "$REPO/tools/rt1170-console.py" "$PORT" 115200 > "$LOG" 2>/dev/null &
  reader_pid=$!
  # Boot + fw download + scan/assoc + DHCP.
  for _ in $(seq 1 40); do
    grep -q "ip=$IP" "$LOG" 2>/dev/null && break
    sleep 1
  done
  if ! grep -q "ip=$IP" "$LOG" 2>/dev/null; then
    echo "boot $boot: NOJOIN (see $LOG)"; nojoin=$((nojoin+1)); continue
  fi
  n=0
  while [ "$(date +%s)" -lt "$DEADLINE" ]; do
    [ "$BLASTS" -eq 0 ] || [ "$n" -lt "$BLASTS" ] || break
    n=$((n+1)); total=$((total+1))
    OUT=$(cd "$EXDIR" && timeout 40 python3 "$PEER" tcp-rx "$IP" 2>&1 | tail -1)
    if echo "$OUT" | grep -q "Mbps"; then
      pass=$((pass+1))
      echo "boot $boot blast $n (#$total): PASS $(echo "$OUT" | grep -o '[0-9.]* Mbps')"
    else
      froze=$((froze+1))
      echo "boot $boot blast $n (#$total): *** FREEZE *** after $((n-1)) clean this boot"
      sleep 36     # let the board's 30 s stall valve fire and dump
      grep -h "FREEZE (" "$LOG" | tail -1
      grep -h "tput: ip=" "$LOG" | tail -1
      break        # RX is dead until reset; further blasts are DOA, not trials
    fi
    sleep 3
  done
done
kill_reader

# VACUITY GUARD.  An earlier version of this harness reported "no freeze --
# link healthy throughout" after a run in which every boot failed to join and
# ZERO blasts executed: a green result from a test that never ran.  That is
# the same class the repo's gate-vacuity tests exist to catch, so it fails
# loudly here instead.
if [ "$total" -eq 0 ]; then
  echo "SOAK INVALID: 0 blasts ran (nojoin=$nojoin).  Nothing was tested."
  echo "  Logs: $OUTDIR"
  exit 2
fi
echo "SOAK: $pass passed, $froze froze, $nojoin boots failed to join ($total blasts)"
echo "  Logs: $OUTDIR"
[ "$froze" -eq 0 ] || exit 1
