#!/usr/bin/env python3
"""Screenshot the RT1176's panel framebuffer over SWD, with no firmware changes.

    tools/rt1170-screenshot.py out.png
    tools/rt1170-screenshot.py out.png --addr 0x80200040 --width 720 --height 1280
    tools/rt1170-screenshot.py a.png --keep-raw a.raw     # keep the raw dump too

WHY THIS WORKS, since the obvious reading is that it should not. The MIPI-DSI
panel is effectively write-only for CONTENT: DSI has a read channel, but it
reads panel REGISTERS, not stored pixels, and these panels keep no readable
frame. So this does not read the display at all -- it reads OUR framebuffer,
which lives in SDRAM and is plain memory. The firmware already hands that same
pointer to lvgl_sum_feed(); this just reads it from the other side of the debug
probe.

Consequences worth knowing:
  - No firmware change, and it works on an ALREADY-RUNNING image.
  - The frame must be STATIC while dumping. A scene that renders once and then
    only services timers (as the vglite/knob examples do) is fine; an animating
    one will tear. Use --halt for those.
  - It is ~3.6 MB over SWD, so expect a few seconds.

★ VERIFY THE ADDRESS RATHER THAN TRUSTING THIS DEFAULT. 0x80200040 is what
Display.framebuffer() returned on this bench, but it is allocation-dependent.
The examples print it (VGLITE_TGT fb=...); read it from the capture when it
matters. A wrong address does not error -- it screenshots whatever else is
there, which is a convincing-looking wrong answer.
"""
import argparse
import os
import subprocess
import sys
import time

LINKSERVER = os.environ.get(
    "LINKSERVER", "/Applications/LinkServer_26.6.137/LinkServer")
GDB = os.environ.get("ARM_GDB", "/Applications/ARM_10/bin/arm-none-eabi-gdb")
DEVICE = os.environ.get("EVKB_DEVICE", "MIMXRT1176:MIMXRT1170-EVKB")
PORT = 3333


def dump(addr, nbytes, raw_path, halt):
    """Pull nbytes from the target into raw_path via LinkServer's gdbserver."""
    subprocess.run(["pkill", "-f", "LinkServer"], capture_output=True)
    time.sleep(1)
    srv = subprocess.Popen([LINKSERVER, "gdbserver", "--gdb-port", str(PORT), DEVICE],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        # Wait for the port rather than guessing: LinkServer takes a variable
        # time to attach, and a fixed sleep is how the first version of this
        # failed with a misleading "Cannot access memory".
        import socket
        for _ in range(60):
            time.sleep(0.5)
            with socket.socket() as sk:
                sk.settimeout(0.5)
                if sk.connect_ex(("127.0.0.1", PORT)) == 0:
                    break
        else:
            sys.stderr.write("screenshot: gdbserver never opened :%d\n" % PORT)
            return False
        cmds = ["-ex", "set confirm off",
                "-ex", "set pagination off",
                "-ex", "target remote :%d" % PORT]
        # Halting gives a coherent frame if the scene is animating; leaving the
        # target running is less invasive and fine for a static scene.
        if halt:
            cmds += ["-ex", "monitor halt"]
        cmds += ["-ex", "dump binary memory %s 0x%X 0x%X"
                 % (raw_path, addr, addr + nbytes)]
        if halt:
            cmds += ["-ex", "continue &"]
        cmds += ["-ex", "detach", "-ex", "quit"]
        r = subprocess.run([GDB, "-batch"] + cmds,
                           capture_output=True, text=True, timeout=180)
        if not os.path.exists(raw_path) or os.path.getsize(raw_path) != nbytes:
            sys.stderr.write(r.stdout[-2000:] + r.stderr[-2000:])
            got = os.path.getsize(raw_path) if os.path.exists(raw_path) else 0
            sys.stderr.write("\nscreenshot: dumped %d of %d bytes\n" % (got, nbytes))
            return False
        return True
    finally:
        srv.terminate()
        subprocess.run(["pkill", "-f", "LinkServer"], capture_output=True)


def to_png(raw_path, png_path, w, h, order):
    from PIL import Image
    data = open(raw_path, "rb").read()
    # LVGL's XRGB8888 is a little-endian uint32 0xAARRGGBB, so the bytes land
    # B,G,R,A -- i.e. Pillow's "BGRX". The flag exists because getting this
    # wrong yields a plausible image with red and blue swapped, which is easy
    # to look at and not notice (Phase 1 shipped exactly that mistake once,
    # in the driver's colour constants).
    mode = {"bgrx": "BGRX", "rgbx": "RGBX"}[order]
    img = Image.frombytes("RGB", (w, h), data, "raw", mode)
    img.save(png_path)
    return img


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("png")
    ap.add_argument("--addr", type=lambda s: int(s, 0), default=0x80200040,
                    help="framebuffer address (default 0x80200040 -- VERIFY IT)")
    ap.add_argument("--width", type=int, default=720)
    ap.add_argument("--height", type=int, default=1280)
    ap.add_argument("--bpp", type=int, default=4)
    ap.add_argument("--order", choices=["bgrx", "rgbx"], default="bgrx")
    ap.add_argument("--halt", action="store_true",
                    help="halt the core while dumping (for animating scenes)")
    ap.add_argument("--keep-raw", metavar="PATH", default=None)
    a = ap.parse_args()

    nbytes = a.width * a.height * a.bpp
    raw = a.keep_raw or (a.png + ".raw")
    print("dumping 0x%X..0x%X (%d bytes) ..." % (a.addr, a.addr + nbytes, nbytes))
    if not dump(a.addr, nbytes, raw, a.halt):
        return 1

    img = to_png(raw, a.png, a.width, a.height, a.order)
    if not a.keep_raw:
        os.unlink(raw)

    # ★ A sanity read, not decoration. A wrong --addr or --order still produces
    # a PNG, so print what the image actually contains and let the caller judge
    # whether it looks like a UI rather than like whatever else was in memory.
    colors = img.convert("RGB").getcolors(maxcolors=1 << 24) or []
    colors.sort(reverse=True)
    total = a.width * a.height
    print("wrote %s (%dx%d)" % (a.png, a.width, a.height))
    print("distinct colours: %d" % len(colors))
    for n, c in colors[:3]:
        print("   #%02X%02X%02X  %5.1f%%" % (c[0], c[1], c[2], 100.0 * n / total))
    if len(colors) == 1:
        print("★ ONE colour only -- a uniform frame. Wrong address, or nothing "
              "was drawn. Check the address the firmware printed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

# ─────────────────────────────────────────────────────────────────────────────
# ★ STATUS 2026-08-17: NOT WORKING YET. The approach is sound and the pieces
# are all present; the gdb-to-LinkServer handshake is what fails.
#
# Established:
#   - LinkServer gdbserver --gdb-port 3333 <device> starts, attaches the probe,
#     and logs "GDB server listening on port 3333 in debug mode (core cm7)".
#   - The port genuinely accepts TCP: `nc -z 127.0.0.1 3333` succeeds.
#   - arm-none-eabi-gdb then fails `target remote` with "Operation timed out",
#     identically for :3333, localhost:3333 and 127.0.0.1:3333, and with
#     `set remotetimeout 60` / `set tcp connect-timeout 60`. So it is not a
#     name-resolution or impatience problem: the socket opens and the remote
#     protocol handshake does not complete.
#
# Untried, in the order worth trying:
#   1. LinkServer's own console/"redlink" command interface, which may expose a
#      memory dump directly and skip gdb entirely -- likely the shortest path.
#   2. A newer gdb. ARM GCC 10's gdb is from 2020; LinkServer 26.6's stub may
#      speak a protocol feature it does not. `gdb-multiarch` or the toolchain
#      shipped inside LinkServer/MCUXpresso would test that in one command.
#   3. --semihost-port -1, in case semihosting setup blocks the stub.
#   4. pyocd, which this tree otherwise avoids for FLASHING (unreliable on this
#      board's FlexSPI NOR) -- but READING SDRAM is a different operation and
#      the objection may not carry over.
#
# The fallback that needs no probe at all: have the firmware itself emit the
# framebuffer. Over the 115200 VCOM a full 3.6 MB frame is ~5 minutes, but a
# single 150x150 knob is 90 KB (~8 s) and would be enough to diff a GPU render
# against a software one, which is the actual need here.
