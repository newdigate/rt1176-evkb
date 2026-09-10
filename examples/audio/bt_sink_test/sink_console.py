#!/usr/bin/env python3
"""The sink gate's CONSOLE driver (NEW-46).

Connects to QEMU's LPUART1 socket -- the same bytes the gate's capture file receives, because the
chardev carries logfile= -- waits for UART milestones, and sends the console commands at the moments
the assertions need: `pair` while STREAMING (to be refused), then, after the peer's injected drop,
`pair`, `forget` and `status`.  It writes DRIVER-* lines to stdout; the gate greps them.

It never judges the firmware -- the gate does that from the capture and the peer's tally.  The one
verdict it DOES form is DRIVER-STATUS-OK/SLOW, and only because it is the one claim nothing else can
make: whether the heartbeat came from the command rather than from the 1 s timer.  Two independent
halves, both required:
  * STRUCTURAL, and load-immune: the `cmd=status` token -- which the timer never emits -- must be
    followed IMMEDIATELY by the `hb ` line of a COMPLETE block (down to its closing `bt_avrcp`).
    loop() prints those back to back from one context and nothing else writes the console, so
    nothing can interleave; a timed block is never preceded by `cmd=status`.
  * TIMED, on the DRIVER'S OWN WALL CLOCK: inside 0.9 s of the send.  That clock is the HOST's, not
    the guest's, which is the only reason a duration may be asserted here at all (CLAUDE.md: QEMU's
    guest clock makes every time MAGNITUDE a fiction -- a gate may assert interval COUNTS, never
    durations, and this is not the guest's reading).  The guest's own second is ~1.5 s of wall time
    on this vehicle (its millis() runs slow -- measured in Task 3's splice-timeout probes), so a
    block landing within 0.9 s wall cannot be the timer's.

Usage: sink_console.py <tcp-port> [budget-seconds]
"""
import re
import socket
import sys
import time

HOST = "127.0.0.1"
PORT = int(sys.argv[1])
BUDGET = float(sys.argv[2]) if len(sys.argv) > 2 else 50.0
DEADLINE = time.time() + BUDGET
STATUS_BOUND = 0.9          # seconds of WALL time; see the module docstring for why this one is allowed
STATUS_WAIT = 5.0           # ... and the sub-budget the wait for it gets, so a broken `status` fails FAST
                            # rather than burning the whole budget and leaving QEMU to gtimeout.

buf = b""                   # every byte received, in order; `mark()` indexes into it


def connect():
    end = time.time() + 10
    while time.time() < end:
        try:
            s = socket.create_connection((HOST, PORT), timeout=2.0)
            s.settimeout(0.25)
            return s
        except OSError:
            time.sleep(0.2)
    print("DRIVER-FAIL could not connect to the console socket")
    sys.stdout.flush()
    sys.exit(1)


def text(start):
    # CONSOLE.println() emits CR LF; strip the CR so `$`-anchored patterns mean what they say -- the
    # same thing run_qemu.sh does to the capture, for the same reason.
    return buf[start:].decode("latin-1").replace("\r", "")


def mark():
    """Index of the start of the trailing (possibly empty) partial line in buf.

    ★ This is what makes a wait mean "arriving from HERE on".  wait_for() searching the WHOLE buffer
    matches lines received minutes ago, which for a token the firmware prints repeatedly -- `hb `,
    `bt_avrcp ` -- returns instantly and true regardless of what the command did.  Snapping to the
    last newline rather than to len(buf) keeps position 0 of the region a genuine line start, so `^`
    under re.M cannot match mid-line.
    """
    return buf.rfind(b"\n") + 1


def wait_for(s, pat, what, start=0, budget=None):
    """Block until a line matching pat has been RECEIVED at or after `start`.  Returns the match."""
    global buf
    end = DEADLINE if budget is None else min(DEADLINE, time.time() + budget)
    while time.time() < end:
        m = re.search(pat, text(start), re.M)
        if m:
            return m
        try:
            b = s.recv(4096)
        except socket.timeout:
            continue
        if not b:
            break
        buf += b
    print("DRIVER-FAIL timed out waiting for %s" % what)
    sys.stdout.flush()
    sys.exit(1)


def send(s, line, note):
    s.send((line + "\n").encode())
    print("DRIVER-SENT %s %s" % (line, note))
    sys.stdout.flush()


s = connect()
print("DRIVER-CONNECTED")
sys.stdout.flush()

# 1. While STREAMING: `pair` must be REFUSED.  This is also the proof that the whole command path --
#    socket, LPUART1 RX, serialEvent1(), the parser, runCommand() -- works at all, so it comes first.
wait_for(s, r"^streaming by=incoming", "STREAMING")
time.sleep(0.5)
m0 = mark()
send(s, "pair", "while-streaming")
wait_for(s, r"^pairing=refused reason=link_up$", "the refusal", m0)

# 2. The peer drops the link once it has streamed every packet and set the volume; the sink opens an
#    automatic DROP window.  Only then do the commanded ones mean anything -- with a link up they are
#    all refused.
wait_for(s, r"^pairing=on reason=drop", "the drop window", m0)
m1 = mark()
send(s, "pair", "after-drop")
wait_for(s, r"^pairing=on reason=cmd", "the commanded window", m1)

# 3. `forget` wipes the bond the peer's first pairing left, so the peer's re-page (~8 s after its
#    drop) finds NO stored key and must pair Just Works from scratch.  ★ Destructive on silicon; safe
#    here only because QEMU has no NVM behind the FlexSPI window (CLAUDE.md) -- the store this wipes
#    is a within-boot table.
m2 = mark()
send(s, "forget", "after-drop")
wait_for(s, r"^bonds_forgotten=1$", "the wipe", m2)

# 4. `status` prints a heartbeat NOW.  See the module docstring for both halves of the verdict.
m3 = mark()
t0 = time.time()
send(s, "status", "after-forget")
wait_for(s, r"^cmd=status\nhb (?:[^\n]*\n)+?bt_avrcp [^\n]*\n", "the commanded heartbeat block",
         m3, STATUS_WAIT)
dt = time.time() - t0
print("DRIVER-STATUS-%s block after cmd=status in %.2fs (bound %.2fs, host clock)"
      % ("OK" if dt < STATUS_BOUND else "SLOW", dt, STATUS_BOUND))
print("DRIVER-DONE")
sys.stdout.flush()
