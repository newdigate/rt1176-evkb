#!/bin/sh
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
# Tools come from THIS checkout, derived from the gate's own location (a
# hardcoded path silently loads a different tree's gate-lib.sh from a worktree).
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/synthui_step_test.elf"
OUT=$(gate_capture_path "$DIR" synthui_step.uart)
DBG=$(gate_capture_path "$DIR" synthui_step.dbg)
rm -f "$OUT"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
# 14s: the RK055 bring-up margin (12s) plus one software render. This scene is
# a single frame, so it has less to do than the knob gate's five.
sleep 14; gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"
# Panel chain first: in DIRECT mode a framebuffer no display owns would still
# checksum perfectly.
grep -q "PANEL_OK"          "$OUT" || { echo "FAIL: panel bring-up"; exit 1; }
grep -q "LVGL_FLUSHED=PASS" "$OUT" || { echo "FAIL: no full refresh"; exit 1; }
# Flushed AREA of the first refresh -- 720*1280*4 at XRGB8888. The partial-
# repaint guard: a corner repaint and a scene edit both just change the sum.
# Value greps are ANCHORED (CR-tolerant \r?$) so 3686400 cannot pass via a
# hypothetical 36864000 and a golden must match the WHOLE value.
grep -qE "LVGL_BYTES=3686400\r?$" "$OUT" || { echo "FAIL: wrong byte count"; exit 1; }
# GOLDEN -- FNV-1a over the whole 720x1280 framebuffer. ONE sum is sufficient
# here and is NOT the acid-bass aggregate trap: the scene is a state MATRIX
# (8 column combinations x 2 rows = all sixteen reachable state combinations)
# rendered in a single frame, so no state can freeze without moving this sum.
#
# ★ THE FRAME BEHIND THIS GOLDEN WAS LOOKED AT, not merely reproduced. QEMU's
# monitor can hand over the scanout buffer, which makes the eye-check possible
# without hardware -- and this tree's rule is that a golden is never recorded
# for a frame nobody has seen (the knob pilot's arc clamp and the VGLite GPU
# frame were both perfectly reproducible AND visibly wrong):
#
#   qemu-system-arm -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on \
#       -kernel build/synthui_step_test.elf -display none -serial null \
#       -monitor unix:/tmp/mon.sock,server,nowait &
#   sleep 12
#   # LCDIFv2 layer-0 descriptor: ADDR 0x4080820c, W/H 0x40808200, PITCH ...208
#   printf 'xp/1wx 0x4080820c\n' | nc -U /tmp/mon.sock
#   # ★ QUOTE the filename -- unquoted, the monitor parses the path as an
#   #   expression and answers "invalid char 't' in expression".
#   printf 'pmemsave 0x80100040 0x384000 "/tmp/fb.raw"\n' | nc -U /tmp/mon.sock
#   python3 -c "from PIL import Image; \
#     Image.frombytes('RGB',(720,1280),open('/tmp/fb.raw','rb').read(), \
#                     'raw','BGRX',2880).save('/tmp/fb.png')"
#
# Verified 2026-08-17: the dump's own FNV-1a equals the value the firmware
# printed (0xCE619CE1), so the inspected image IS this golden's frame -- and
# all sixteen state combinations were confirmed distinct and correct in it.
# Silicon confirmation is still owed (capstone plan Task 8): QEMU renders in
# software, and the panel has its own failure modes this can never see.
# On a mismatch, work out WHICH of {SynthUI pin, LVGL pin, lv_conf.h, fonts,
# scene} changed; do NOT paste in whatever the board printed.
grep -qE "STEP_GRID_SUM=0xCE619CE1\r?$" "$OUT" || { echo "FAIL: grid checksum"; exit 1; }
# The all-zero framebuffer, asserted against BY NAME: 0x9BC99DC5 is the FNV of
# 3686400 zero bytes. A blank frame reporting a full-screen flush is a real
# failure mode this tree has met (vglite_lvgl_test), and it is otherwise
# indistinguishable from a golden that happens not to match.
grep -q "STEP_GRID_SUM=0x9BC99DC5" "$OUT" \
    && { echo "FAIL: framebuffer is all zeros (blank frame)"; exit 1; }
grep -q "SYNTHUI_STEP_DONE" "$OUT" || { echo "FAIL: no completion token"; exit 1; }
echo "PASS: SynthUI step widget render verified"
