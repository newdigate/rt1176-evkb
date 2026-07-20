#!/usr/bin/env python3
# QMP input injector for the RT1176 USB Host input gate.
#
# Task 5 (enumeration) only exercises control transfers on the async schedule.
# This injector drives the *periodic* schedule / interrupt-IN path: after the
# firmware has enumerated the HID device (printed KBD_CONNECT / MOUSE_CONNECT to
# the marker file), it feeds a synthetic keypress / mouse move into QEMU over
# QMP, which usb-kbd / usb-mouse turns into a HID report the firmware collects on
# its next interrupt poll -- printing KEY=<n> / MOUSE=<dx>,<dy>,<btn>.
#
# Routing (why no `device`/`head` is needed, even with -display none):
#   input-send-event with no `device` dispatches by event-type mask to the first
#   input handler that has NO console binding (ui/input.c qemu_input_find_handler).
#   usb-kbd/usb-mouse register their handler console-less (hid_init) unless a
#   `display=` qdev property is set -- which we don't set. Each gate run attaches
#   exactly one HID device, so a `key` event has a single admissible handler
#   (the keyboard, mask INPUT_EVENT_MASK_KEY) and a `rel` event a single one
#   (the mouse, mask BTN|REL). QEMU's input trace shows `con -1`, confirming the
#   event reaches the console-less USB HID handler rather than a display sink.
#
# Stdlib only (socket + json). All waits are bounded; failures print to stderr
# and set a non-zero exit code. The gate's check_markers.py is the source of
# truth for pass/fail -- this injector's job is to make KEY=/MOUSE= appear.
#
# Usage:
#   inject_qmp.py --sock PATH --marker FILE --kind {kbd,mouse}
#                 --connect KBD_CONNECT --result KEY=
#
# Exit codes: 0 result marker seen; 2 QMP connect/handshake failed;
#             3 connect marker never appeared; 4 result marker never appeared.

import argparse
import json
import socket
import sys
import time


def log(*a):
    print("[inject]", *a, file=sys.stderr, flush=True)


def marker_has(path, kw):
    try:
        with open(path, "r", errors="replace") as f:
            return kw in f.read()
    except OSError:
        return False


def wait_marker(path, kw, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if marker_has(path, kw):
            return True
        time.sleep(0.1)
    return marker_has(path, kw)


class Qmp:
    def __init__(self, sock_path, connect_timeout):
        self.sock = None
        self.rf = None
        deadline = time.monotonic() + connect_timeout
        last = None
        while time.monotonic() < deadline:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                s.connect(sock_path)
                self.sock = s
                break
            except OSError as e:
                last = e
                s.close()
                time.sleep(0.1)
        if self.sock is None:
            raise ConnectionError(f"could not connect to {sock_path}: {last}")
        self.sock.settimeout(10.0)
        self.rf = self.sock.makefile("r")

    def _recv(self):
        # Return the next command reply, skipping asynchronous QMP events.
        while True:
            line = self.rf.readline()
            if not line:
                raise ConnectionError("QMP connection closed")
            msg = json.loads(line)
            if "event" in msg:
                continue
            return msg

    def handshake(self):
        greeting = json.loads(self.rf.readline())
        if "QMP" not in greeting:
            log("warning: unexpected QMP greeting:", greeting)
        self._send({"execute": "qmp_capabilities"})
        reply = self._recv()
        if "return" not in reply:
            raise ConnectionError(f"qmp_capabilities failed: {reply}")

    def _send(self, obj):
        self.sock.sendall((json.dumps(obj) + "\n").encode())

    def send_event(self, events):
        self._send({"execute": "input-send-event", "arguments": {"events": events}})
        reply = self._recv()
        if "return" not in reply:
            raise RuntimeError(f"input-send-event failed: {reply}")

    def close(self):
        try:
            if self.rf:
                self.rf.close()
            if self.sock:
                self.sock.close()
        except OSError:
            pass


def key_event(down, code):
    return {"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": code}}}


def rel_event(axis, value):
    return {"type": "rel", "data": {"axis": axis, "value": value}}


def inject(qmp, kind, key):
    if kind == "kbd":
        # A full press+release, so the HID report returns to idle between
        # attempts (a stuck-down key would suppress a re-press).
        qmp.send_event([key_event(True, key)])
        qmp.send_event([key_event(False, key)])
    else:
        # A relative move is enough for MouseController.available(); dx/dy are
        # echoed back in the MOUSE= marker.
        qmp.send_event([rel_event("x", 20), rel_event("y", 10)])


def main():
    ap = argparse.ArgumentParser(description="QMP HID input injector for the USB host gate")
    ap.add_argument("--sock", required=True, help="QMP unix socket path")
    ap.add_argument("--marker", required=True, help="LPUART1 marker file to watch")
    ap.add_argument("--kind", required=True, choices=["kbd", "mouse"])
    ap.add_argument("--connect", required=True, help="enumeration marker to wait for, e.g. KBD_CONNECT")
    ap.add_argument("--result", required=True, help="input marker to prove, e.g. KEY=")
    ap.add_argument("--key", default="a", help="qcode to press for --kind kbd (default: a)")
    ap.add_argument("--connect-timeout", type=float, default=25.0)
    ap.add_argument("--result-timeout", type=float, default=15.0)
    ap.add_argument("--attempts", type=int, default=12)
    args = ap.parse_args()

    try:
        qmp = Qmp(args.sock, args.connect_timeout)
        qmp.handshake()
    except (OSError, ValueError, ConnectionError) as e:
        log(f"FAIL: QMP setup: {e}")
        return 2

    try:
        if not wait_marker(args.marker, args.connect, args.connect_timeout):
            log(f"FAIL: {args.connect} never appeared in {args.marker} "
                f"within {args.connect_timeout}s (device not enumerated)")
            return 3
        log(f"{args.connect} seen; injecting {args.kind} input")

        # Inject, then poll for the result; re-inject on each miss (a single
        # interrupt poll can race the report). Bail as soon as the marker shows.
        per_attempt = max(0.3, args.result_timeout / max(1, args.attempts))
        for i in range(args.attempts):
            if marker_has(args.marker, args.result):
                break
            try:
                inject(qmp, args.kind, args.key)
            except (OSError, RuntimeError, ConnectionError) as e:
                log(f"FAIL: injection error on attempt {i + 1}: {e}")
                return 2
            if wait_marker(args.marker, args.result, per_attempt):
                break

        if marker_has(args.marker, args.result):
            log(f"OK: {args.result} seen after injection")
            return 0
        log(f"FAIL: {args.result} never appeared in {args.marker} "
            f"after {args.attempts} injection attempts")
        return 4
    finally:
        qmp.close()


if __name__ == "__main__":
    sys.exit(main())
