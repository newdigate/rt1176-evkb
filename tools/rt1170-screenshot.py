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
  - Frames are coherent by construction: gdb's connect halts the core for the
    duration of the dump (allstop mode) and detach resumes it. --halt merely
    makes the halt explicit.
  - It is ~3.6 MB over SWD, so expect tens of seconds (~20 s measured).

★ The default --addr is AUTO: the tool reads the LCDIFv2 layer-0 descriptor
(CTRLDESCL4 = buffer ADDR at 0x4080820C, CTRLDESCL1 = W/H, CTRLDESCL3 =
PITCH) and dumps exactly what the hardware is scanning out. That is the
ground truth whatever the allocator did -- the first hardcoded default was
measured to be one build's address and WRONG for the sibling build of the
same example (the software build's frame came out shifted with a black
tail). An explicit --addr overrides it; then the CLI width/height/bpp are
trusted as given, with the same convincing-wrong-answer hazard the register
read exists to remove.
"""
import argparse
import os
import re
import subprocess
import sys
import time

LINKSERVER = os.environ.get(
    "LINKSERVER", "/Applications/LinkServer_26.6.137/LinkServer")
GDB = os.environ.get("ARM_GDB", "/Applications/ARM_10/bin/arm-none-eabi-gdb")
DEVICE = os.environ.get("EVKB_DEVICE", "MIMXRT1176:MIMXRT1170-EVKB")
PORT = 3333


def _kill_daemons():
    # All three, per CLAUDE.md: pkill LinkServer alone leaves redlinkserv /
    # crt_emu_cm_redlink resident, and those wedge later sessions.
    # ★ SIGKILL, not SIGTERM: a crt_emu_cm_redlink stub that never got a gdb
    # client (any wedged session leaves one) IGNORES SIGTERM while it waits,
    # keeps the internal stub port and the probe open, and starves every
    # subsequent server -- the stub then stalls at its banner and Target Ready
    # never appears. Measured 2026-08-17: two such stubs survived pkill -f and
    # needed -9.
    for pat in ("LinkServer", "redlinkserv", "crt_emu_cm_redlink"):
        subprocess.run(["pkill", "-9", "-f", pat], capture_output=True)
    for _ in range(10):
        alive = subprocess.run(["pgrep", "-f",
                                "LinkServer|redlinkserv|crt_emu_cm_redlink"],
                               capture_output=True)
        if alive.returncode != 0:
            return
        time.sleep(0.5)
    sys.stderr.write("screenshot: warning: probe daemons still resident\n")


def _dump_once(geom, raw_path, halt, log_path):
    """One server lifecycle: start gdbserver, wait via ITS LOG, dump, tear down."""
    _kill_daemons()
    time.sleep(1)
    log = open(log_path, "w")
    # --attach: no connect script (no M4 wake), no flash-driver VECTRESET --
    # the non-invasive mode a screenshot of a RUNNING image wants.
    srv = subprocess.Popen([LINKSERVER, "gdbserver", "--gdb-port", str(PORT),
                            "--semihost-port", "-1", "--attach", DEVICE],
                           stdout=log, stderr=subprocess.STDOUT)
    try:
        # ★ Wait by reading the server's LOG, never by probing the port.
        # A bare TCP open/close on the gdb port (nc -z, socket connect_ex)
        # wedges the session: the stub blocks in accept() until its FIRST
        # client and runs its whole init (probe, target connect, 'Target
        # Ready') inside that first session -- so a probe that connects and
        # vanishes both consumes the one first-client slot and leaves init
        # half-run against a dead socket. The next connection then times out.
        # That probe was this tool's original bug.
        # The only safe readiness signal is the LAUNCHER's listening line,
        # which is printed before any client; gdb itself must be the first
        # thing to touch the port, and remotetimeout must cover the ~10 s of
        # stub init that runs inside gdb's connect.
        deadline = time.time() + 45
        ready = False
        while time.time() < deadline:
            time.sleep(0.5)
            if srv.poll() is not None:
                break
            with open(log_path) as f:
                if "GDB server listening" in f.read():
                    ready = True
                    break
        if not ready:
            sys.stderr.write("screenshot: gdbserver never started listening; "
                             "log tail:\n" + open(log_path).read()[-2000:])
            return None
        time.sleep(1)
        cmds = ["-ex", "set confirm off",
                "-ex", "set pagination off",
                "-ex", "set remotetimeout 30",
                "-ex", "target remote 127.0.0.1:%d" % PORT,
                # The stub's device memory map has no 0x80000000 region --
                # SEMC SDRAM is board-level, not part of MIMXRT1176xxxxx --
                # and gdb refuses reads outside the advertised map with
                # "Cannot access memory". The map is advisory; turn the
                # refusal off. (The CM7 was happily using SDRAM the whole
                # time; only the debugger believed it did not exist.)
                "-ex", "set mem inaccessible-by-default off"]
        # gdb's connect halts the core (allstop mode), so the frame is coherent
        # by construction and detach resumes it. --halt just makes that
        # explicit; it is no longer the difference between torn and whole.
        if halt:
            cmds += ["-ex", "monitor halt"]
        if geom is None:
            # AUTO: the LCDIFv2 layer-0 descriptor is the ground truth for
            # what is on the panel. CTRLDESCL1 = HEIGHT<<16|WIDTH, CTRLDESCL3
            # low 16 = PITCH bytes, CTRLDESCL4 = buffer ADDR.
            cmds += ["-ex",
                     'printf "FBINFO addr=0x%08x w=%u h=%u pitch=%u\\n", '
                     '*(unsigned*)0x4080820C, '
                     '(*(unsigned*)0x40808200) & 0xFFFF, '
                     '((*(unsigned*)0x40808200) >> 16) & 0xFFFF, '
                     '(*(unsigned*)0x40808208) & 0xFFFF',
                     "-ex", "dump binary memory %s (*(unsigned*)0x4080820C) "
                     "((*(unsigned*)0x4080820C) + "
                     "((*(unsigned*)0x40808208) & 0xFFFF) * "
                     "(((*(unsigned*)0x40808200) >> 16) & 0xFFFF))" % raw_path]
        else:
            addr, nbytes = geom
            cmds += ["-ex", "dump binary memory %s 0x%X 0x%X"
                     % (raw_path, addr, addr + nbytes)]
        cmds += ["-ex", "detach", "-ex", "quit"]
        r = subprocess.run([GDB, "-batch"] + cmds,
                           capture_output=True, text=True, timeout=300)
        if geom is None:
            m = re.search(r"FBINFO addr=(0x[0-9a-fA-F]+) w=(\d+) h=(\d+) "
                          r"pitch=(\d+)", r.stdout)
            if not m:
                sys.stderr.write(r.stdout[-2000:] + r.stderr[-2000:] +
                                 "\nscreenshot: no FBINFO from target\n")
                return None
            info = {"addr": int(m.group(1), 16), "w": int(m.group(2)),
                    "h": int(m.group(3)), "pitch": int(m.group(4))}
            if info["addr"] == 0 or info["w"] == 0 or info["h"] == 0:
                sys.stderr.write("screenshot: LCDIFv2 layer 0 is not "
                                 "configured (%r) -- display never began?\n"
                                 % info)
                return None
            nbytes = info["pitch"] * info["h"]
        else:
            info = {"addr": addr, "w": None, "h": None, "pitch": None}
        if not os.path.exists(raw_path) or os.path.getsize(raw_path) != nbytes:
            sys.stderr.write(r.stdout[-2000:] + r.stderr[-2000:])
            got = os.path.getsize(raw_path) if os.path.exists(raw_path) else 0
            sys.stderr.write("\nscreenshot: dumped %d of %d bytes\n" % (got, nbytes))
            return None
        return info
    finally:
        log.close()
        srv.terminate()
        _kill_daemons()


def dump(geom, raw_path, halt):
    """Dump the frame via LinkServer's gdbserver.

    geom: None for auto-discovery from the LCDIFv2 registers, or an explicit
    (addr, nbytes) pair. Returns the discovery dict, or None on failure."""
    log_path = raw_path + ".gdbserver.log"
    for attempt in (1, 2):
        info = _dump_once(geom, raw_path, halt, log_path)
        if info is not None:
            if os.path.exists(log_path):
                os.unlink(log_path)
            return info
        if attempt == 1:
            sys.stderr.write("screenshot: retrying with a fresh server...\n")
            time.sleep(2)
    return None


def to_png(raw_path, png_path, w, h, order, stride):
    from PIL import Image
    data = open(raw_path, "rb").read()
    # LVGL's XRGB8888 is a little-endian uint32 0xAARRGGBB, so the bytes land
    # B,G,R,A -- i.e. Pillow's "BGRX". The flag exists because getting this
    # wrong yields a plausible image with red and blue swapped, which is easy
    # to look at and not notice (Phase 1 shipped exactly that mistake once,
    # in the driver's colour constants).
    mode = {"bgrx": "BGRX", "rgbx": "RGBX"}[order]
    img = Image.frombytes("RGB", (w, h), data, "raw", mode, stride)
    img.save(png_path)
    return img


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("png")
    ap.add_argument("--addr", default="auto",
                    help="framebuffer address, or 'auto' (default) to read the "
                         "LCDIFv2 layer-0 descriptor off the hardware")
    ap.add_argument("--width", type=int, default=720,
                    help="only used with an explicit --addr")
    ap.add_argument("--height", type=int, default=1280,
                    help="only used with an explicit --addr")
    ap.add_argument("--bpp", type=int, default=4,
                    help="only used with an explicit --addr")
    ap.add_argument("--order", choices=["bgrx", "rgbx"], default="bgrx")
    ap.add_argument("--halt", action="store_true",
                    help="halt the core while dumping (for animating scenes)")
    ap.add_argument("--keep-raw", metavar="PATH", default=None)
    a = ap.parse_args()

    raw = a.keep_raw or (a.png + ".raw")
    if a.addr == "auto":
        print("dumping the LCDIFv2 layer-0 scanout buffer (auto) ...")
        info = dump(None, raw, a.halt)
        if info is None:
            return 1
        w, h, stride = info["w"], info["h"], info["pitch"]
        print("FBINFO addr=0x%08X w=%d h=%d pitch=%d" %
              (info["addr"], w, h, stride))
    else:
        addr = int(a.addr, 0)
        w, h, stride = a.width, a.height, a.width * a.bpp
        nbytes = stride * h
        print("dumping 0x%X..0x%X (%d bytes) ..." % (addr, addr + nbytes, nbytes))
        if dump((addr, nbytes), raw, a.halt) is None:
            return 1

    img = to_png(raw, a.png, w, h, a.order, stride)
    if not a.keep_raw:
        os.unlink(raw)

    # ★ A sanity read, not decoration. A wrong --addr or --order still produces
    # a PNG, so print what the image actually contains and let the caller judge
    # whether it looks like a UI rather than like whatever else was in memory.
    colors = img.convert("RGB").getcolors(maxcolors=1 << 24) or []
    colors.sort(reverse=True)
    total = w * h
    print("wrote %s (%dx%d)" % (a.png, w, h))
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
# ★ STATUS 2026-08-17 (later the same day): WORKING. The earlier failure was
# diagnosed by experiment and the suspected cause (gdb's age) was WRONG.
#
# ROOT CAUSE, measured (and then corrected by a stack sample): LinkServer's
# launcher (python) listens on the public gdb port and proxies to the real
# stub (crt_emu_cm_redlink) on an internal port. The stub blocks in accept()
# -- seen directly with `sample` -- until its FIRST client connects, and only
# then runs its entire init: redlinkserv connect, probe open, target connect,
# ending in "Pc: (100) Target Ready". Everything follows from that:
#
#   - gdb as first client: init runs INSIDE gdb's connect, takes ~10 s, and
#     the session works -- provided remotetimeout covers it. gdb 10 is fine.
#   - A bare TCP open/close first (`nc -z`, socket connect_ex) consumes the
#     one first-client slot and leaves init half-run against a dead socket;
#     the NEXT connection times out inside connect(). This tool's own "wait
#     for the port" poll was exactly such a probe, so it poisoned every
#     session before gdb ever ran -- and the `nc -z` used to "confirm the
#     port is up" during diagnosis re-poisoned it on every retry. The
#     verification WAS the failure.
#   - Waiting for "Target Ready" BEFORE connecting is a deadlock: that line
#     is printed during the first session, not before it. (An earlier
#     revision of this note claimed it was pre-client; the "client" in that
#     experiment was the nc probe itself.)
#
# So: never health-check this port with a bare TCP connect, wait only for
# the launcher's "GDB server listening" line, then let gdb be the first and
# only toucher of the port, with remotetimeout >= 30.
#
# Also established:
#   - --attach skips the RT1170 connect script (no M4 wake/reset) and the
#     flash-driver VECTRESET that debug mode performs on connect, so it is
#     the right mode for photographing a running image.
#   - gdb's connect halts the core either way (allstop), so frames are
#     coherent by construction; detach resumes the target.
#   - A stub whose first client vanished ignores SIGTERM in accept() and
#     must be SIGKILLed, or it holds the probe and starves every later
#     session (see _kill_daemons).
#   - `LinkServer probe '#1' wiretimedreset 200` recovers a bench whose
#     half-initialised stub state outlived the daemons.
#
# The fallback that needs no probe at all (unused, kept for reference): have
# the firmware itself emit the framebuffer over the VCOM. A full frame is ~5
# minutes at 115200; a single 150x150 knob is 90 KB (~8 s).
