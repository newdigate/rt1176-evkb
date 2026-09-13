#!/usr/bin/env bash
# Flash an image to the MIMXRT1170-EVKB via LinkServer, then open the live console.
#
# Usage:  rt1170-flash.sh [path/to/image.elf|.hex]
# Default image: the prebuilt Zephyr hello_world.
#
# Why LinkServer (not pyOCD): pyOCD's RT1170 FlexSPI programming drops SWD at
# "board uninit" and leaves the external NOR stuck. LinkServer is reliable.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENV_FILE="${RT1170_ENV_FILE:-$HERE/../.env}"
if [ -f "$ENV_FILE" ]; then
  # shellcheck disable=SC1090
  set -a; source "$ENV_FILE"; set +a
fi

# Auto-discovery if variables are not set
if [ -z "${LINKSERVER:-}" ]; then
  for cand in /Applications/LinkServer_*/LinkServer; do
    if [ -x "$cand" ]; then LINKSERVER="$cand"; break; fi
  done
  LINKSERVER="${LINKSERVER:-$(command -v LinkServer 2>/dev/null || true)}"
fi

if [ -z "${RT1170_PORT:-}" ]; then
  for cand in /dev/cu.usbmodem*; do
    if [ -e "$cand" ]; then RT1170_PORT="$cand"; break; fi
  done
fi

DEVICE="${EVKB_DEVICE:-${DEVICE:-MIMXRT1176:MIMXRT1170-EVKB}}"
PORT="${RT1170_PORT:-/dev/cu.usbmodem5DQ2DDHVWO5EI3}"
PY="${PY:-/usr/local/Caskroom/miniconda/base/bin/python3}"
[ -x "$PY" ] || PY="$(command -v python3 2>/dev/null || true)"

if [ "${1:-}" = "--env-check" ]; then
  echo "LINKSERVER=$LINKSERVER"
  echo "RT1170_PORT=$PORT"
  echo "EVKB_DEVICE=$DEVICE"
  echo "PY=$PY"
  exit 0
fi

IMG="${1:-$HOME/Development/zephyr/projects/zepherproject/build-hello/zephyr/zephyr.elf}"

[ -x "$LINKSERVER" ] || { echo "LinkServer not found at $LINKSERVER (set \$LINKSERVER or configure in .env)"; exit 1; }
[ -f "$IMG" ]        || { echo "Image not found: $IMG"; exit 1; }

# ★ A reader holding the VCOM while LinkServer programs the board doesn't just
# fail the load (DAP status 131): the VCOM re-enumerates mid-flash, and macOS's
# IOSerialFamily can hit a use-after-free tearing down the open tty — a full
# kernel panic (three identical panics, 2026-07-28..31, all with python3.12 on
# the port). The console's reconnect loop reopens the port every 0.5 s, so an
# lsof check alone is a race — kill the reader *processes*, then confirm the
# port is free.
kill_vcom_readers() {
  local pids sig
  for sig in TERM KILL; do
    pids="$( { pgrep -f 'rt1170-console\.py' || true; lsof -t -- "$PORT" 2>/dev/null || true; } | sort -un )"
    [ -z "$pids" ] && return 0
    echo "==> VCOM reader(s) on $PORT (pid $(echo $pids | tr '\n' ' ')) — sending SIG$sig"
    kill -"$sig" $pids 2>/dev/null || true
    for _ in 1 2 3 4 5 6 7 8 9 10; do
      sleep 0.2
      pids="$( { pgrep -f 'rt1170-console\.py' || true; lsof -t -- "$PORT" 2>/dev/null || true; } | sort -un )"
      [ -z "$pids" ] && return 0
    done
  done
  echo "ERROR: could not free $PORT (pid $(echo $pids | tr '\n' ' ')) — aborting flash to avoid a kernel panic." >&2
  return 1
}
kill_vcom_readers

echo "==> Flashing $IMG"
"$LINKSERVER" flash "$DEVICE" load "$IMG" --erase-all
echo "==> Flash OK."
echo "==> Opening console. Press SW4/RESET on the board to boot and see output."
echo
exec "$PY" "$HERE/rt1170-console.py" "$PORT" 115200
