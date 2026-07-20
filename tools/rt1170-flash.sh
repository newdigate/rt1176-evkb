#!/usr/bin/env bash
# Flash an image to the MIMXRT1170-EVKB via LinkServer, then open the live console.
#
# Usage:  rt1170-flash.sh [path/to/image.elf|.hex]
# Default image: the prebuilt Zephyr hello_world.
#
# Why LinkServer (not pyOCD): pyOCD's RT1170 FlexSPI programming drops SWD at
# "board uninit" and leaves the external NOR stuck. LinkServer is reliable.
set -euo pipefail

LINKSERVER="${LINKSERVER:-/Applications/LinkServer_26.6.137/LinkServer}"
DEVICE="MIMXRT1176:MIMXRT1170-EVKB"
PORT="${RT1170_PORT:-/dev/cu.usbmodem5DQ2DDHVWO5EI3}"
PY="${PY:-/usr/local/Caskroom/miniconda/base/bin/python3}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMG="${1:-$HOME/Development/zephyr/projects/zepherproject/build-hello/zephyr/zephyr.elf}"

[ -x "$LINKSERVER" ] || { echo "LinkServer not found at $LINKSERVER (set \$LINKSERVER)"; exit 1; }
[ -f "$IMG" ]        || { echo "Image not found: $IMG"; exit 1; }

echo "==> Flashing $IMG"
"$LINKSERVER" flash "$DEVICE" load "$IMG" --erase-all
echo "==> Flash OK."
echo "==> Opening console. Press SW4/RESET on the board to boot and see output."
echo
exec "$PY" "$HERE/rt1170-console.py" "$PORT" 115200
