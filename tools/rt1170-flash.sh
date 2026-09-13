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

LOCK_DIR="${RT1170_LOCK_DIR:-/tmp/rt1170}"
LOCK_FILE="$LOCK_DIR/evkb.lock"
SERIAL_PID_FILE="$LOCK_DIR/serial.pid"

unlock_board() {
  local spid=""
  if [ -f "$SERIAL_PID_FILE" ]; then
    spid="$(cat "$SERIAL_PID_FILE" 2>/dev/null || true)"
  fi
  if [ -n "$spid" ] && kill -0 "$spid" 2>/dev/null; then
    echo "==> Terminating background serial reader (PID: $spid)..."
    kill -TERM "$spid" 2>/dev/null || true
    for _ in {1..10}; do
      if ! kill -0 "$spid" 2>/dev/null; then break; fi
      sleep 0.2
    done
  fi
  rm -f "$LOCK_FILE" "$SERIAL_PID_FILE"
  echo "==> Board lock released and serial console stopped safely."
}

if [ "${1:-}" = "--unlock" ] || [ "${1:-}" = "--stop-console" ]; then
  unlock_board
  exit 0
fi

check_lock_and_conflicts() {
  mkdir -p "$LOCK_DIR"
  if [ -f "$LOCK_FILE" ]; then
    local lpid
    lpid="$(grep '^PID=' "$LOCK_FILE" 2>/dev/null | cut -d= -f2 || true)"
    if [ -n "$lpid" ] && kill -0 "$lpid" 2>/dev/null; then
      echo "ERROR: Board or serial port is currently busy!" >&2
      echo "Active lock held by PID $lpid:" >&2
      cat "$LOCK_FILE" >&2
      echo "To prevent a macOS IOSerialFamily kernel panic, aborting." >&2
      echo "Release the board with: tools/rt1170-flash.sh --unlock" >&2
      exit 1
    else
      rm -f "$LOCK_FILE"
    fi
  fi

  # Check active readers or active debug sessions (Option C: strict abort without auto-killing)
  local readers=""
  readers="$(lsof -t -- "$PORT" 2>/dev/null || true)"
  local console_pids=""
  console_pids="$(pgrep -f 'rt1170-console\.py' 2>/dev/null || true)"
  local active_sessions=""
  active_sessions="$(pgrep -f 'LinkServer[[:space:]]+(flash|run|gdbserver)|arm-none-eabi-gdb' 2>/dev/null || true)"

  local conflicting_pids=""
  conflicting_pids="$( { echo "$readers $console_pids $active_sessions" | tr ' ' '\n' | grep -v "^$$$" | grep -v "^$" || true; } | sort -un | tr '\n' ' ' )"
  if [ -n "${conflicting_pids// /}" ]; then
    echo "ERROR: Board or serial port is currently busy!" >&2
    echo "Conflicting process(es) detected (PID: $conflicting_pids):" >&2
    ps -p $conflicting_pids -o pid,comm,args >&2 2>/dev/null || true
    echo "To avoid a macOS kernel panic, aborting." >&2
    echo "Please terminate conflicting processes or run: tools/rt1170-flash.sh --unlock" >&2
    exit 1
  fi
}

if [ "${1:-}" = "--check-lock" ]; then
  check_lock_and_conflicts
  echo "LOCK_OK"
  exit 0
fi

check_board_power() {
  if [ ! -c "$PORT" ] && [ ! -e "$PORT" ]; then
    echo "ERROR: MCU-Link VCOM port $PORT not found." >&2
    echo "Please connect the EVKB debug USB cable (J17)." >&2
    exit 1
  fi

  local attempts=0
  while [ $attempts -lt 3 ]; do
    attempts=$((attempts + 1))
    local probe_out
    probe_out="$("$LINKSERVER" probe '#0' dapinfo 2>&1 || true)"
    if echo "$probe_out" | grep -q "Wire not connected"; then
      echo "==> WARNING: MCU-Link detected on USB, but RT1170 target is unpowered." >&2
      echo "==> Please switch ON the board power switch (SW1) or check the 5V DC barrel jack." >&2
      if [ -t 0 ]; then
        read -r -p "Press [Enter] to re-check power (attempt $attempts/3) or Ctrl-C to abort..."
      else
        sleep 0.5
      fi
    else
      return 0
    fi
  done
  echo "ERROR: Target unpowered after 3 checks. Aborting flash." >&2
  exit 1
}

if [ "${1:-}" = "--check-power" ]; then
  check_board_power
  echo "POWER_OK"
  exit 0
fi

flash_image() {
  local target_img="$1"
  local flash_log="$LOCK_DIR/flash.log"
  mkdir -p "$LOCK_DIR"
  echo "==> Flashing $DEVICE with $target_img"
  echo "==> Tail debugger output in another window with:"
  echo "    tail -f $flash_log"
  echo

  set +e
  "$LINKSERVER" flash "$DEVICE" load "$target_img" --erase-all > "$flash_log" 2>&1
  local rc=$?
  set -e

  if [ $rc -ne 0 ]; then
    if [[ "$target_img" == *.elf ]]; then
      local hex_alt="${target_img%.elf}.hex"
      if [ -f "$hex_alt" ] && grep -q -E "code -11|LOAD_EXIT=245" "$flash_log"; then
        echo "==> LinkServer ELF parser failed (exit -11). Falling back to .hex image: $hex_alt"
        "$LINKSERVER" flash "$DEVICE" load "$hex_alt" --erase-all >> "$flash_log" 2>&1
        echo "==> Flash OK (via HEX fallback)."
        return 0
      fi
    fi
    echo "ERROR: LinkServer flash failed (exit code $rc). Check $flash_log" >&2
    tail -n 20 "$flash_log" >&2
    exit $rc
  fi
  echo "==> Flash OK."
}

if [ "${1:-}" = "--test-flash" ]; then
  flash_image "${2:-}"
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
