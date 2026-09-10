#!/usr/bin/env python3
"""The sink gate's CONSOLE driver (NEW-46).

Connects to QEMU's LPUART1 socket -- the same bytes the gate's capture file receives, because the
chardev carries logfile= -- waits for UART milestones, and sends the console commands at the moments
the assertions need: `pair` while STREAMING (to be refused), then, after the peer's injected drop,
`pair`, `forget` and `status`.  It writes DRIVER-* lines to stdout; the gate greps them.

It also holds ONE ordering the gate depends on and cannot get anywhere else: after the drop it waits
for the sink's own `scan_enable=0x03` and logs `DRIVER-SAW` before typing, so the single
Write_Scan_Enable the peer sees in that region provably belongs to the DROP window and not to a
commanded one (reconcileScan() writes only on a change, so all three windows would otherwise
coalesce into it).  The gate asserts that DRIVER-SAW precedes every post-drop DRIVER-SENT.

★ EVERY exit this file can take prints a `DRIVER-*` line first, and the gate greps `^DRIVER-FAIL`
BEFORE any firmware-shaped assertion.  That pairing is the point: a driver that dies mid-run makes
the firmware assertions downstream of it unreachable, so without the DRIVER-FAIL grep the first
failure reads `[pair] the commanded windows never opened` -- a harness fault reported as a firmware
defect, which is the shape run_qemu.sh's header says this gate exists to prevent.
★ The converse, stated so `[pair] the console driver failed:` is not mis-read: a DRIVER-FAIL can also
be the FIRMWARE's fault, because every wait here is on a line the firmware owes.  A sink that never
opened a drop window, or never re-issued its scan write, times out HERE first.  The message always
names the token that never came, so read that rather than the wrapper -- and the peer's own
assertions in the gate say the same thing from the other side.

It never judges the firmware -- the gate does that from the capture and the peer's tally.  The one
verdict it DOES form is DRIVER-STATUS-OK/SLOW, and only because it is the one claim nothing else can
make: whether the heartbeat came from the command rather than from the 1 s timer.  Two independent
halves, both required:
  * STRUCTURAL, and load-immune: the `cmd=status` token -- which the timer never emits -- must be
    followed IMMEDIATELY by a COMPLETE heartbeat block: the `hb ` line and then EXACTLY the four
    lines that close it (bt_sink, bt_jit, bt_link, bt_hci) before `bt_avrcp`.  loop() prints those
    six back to back from one context and nothing else writes the console, so nothing can
    interleave; a timed block is never preceded by `cmd=status`.
    ★ The count is what makes the block COMPLETE, and it was measured rather than assumed: with the
    lazy `(?:[^\n]*\n)+?` this file carried until 2026-09-10, a commanded block TRUNCATED after its
    `hb ` line still matched -- by running on through the five HCI-settings lines that follow and
    borrowing the NEXT (timed) block's `bt_avrcp`, 13 lines instead of 7.  The docstring claimed a
    complete block and the pattern did not require one.
  * TIMED, on the DRIVER'S OWN WALL CLOCK: inside 0.9 s of the send.  That clock is the HOST's, not
    the guest's, which is the only reason a duration may be asserted here at all (CLAUDE.md: QEMU's
    guest clock makes every time MAGNITUDE a fiction -- a gate may assert interval COUNTS, never
    durations, and this is not the guest's reading).  The guest's own second is ~1.5 s of wall time
    on this vehicle (its millis() runs slow -- measured in Task 3's splice-timeout probes), so a
    block landing within 0.9 s wall cannot be the timer's.
    ★ THE 0.9 s BOUND WAS MEASURED IDLE ONLY -- 0.03-0.04 s over the runs to date, ~22x of headroom
    -- and NOT under load.  Stated because this tree has the precedent: NEW-42's `[jit]` band floor
    read a comfortable 144 idle and 118 under eight CPU spinners, and had to move 120 -> 90.  If this
    verdict ever reads SLOW in a sweep and OK idle, that headroom is where to look first, and the
    bound is a host-scheduling measurement to re-take, not a firmware finding.

Usage: sink_console.py <tcp-port> [budget-seconds]
"""
import re
import socket
import sys
import time


def die(msg):
    """Every abnormal exit goes through here, so every one of them is greppable by the gate."""
    print("DRIVER-FAIL %s" % msg)
    sys.stdout.flush()
    sys.exit(1)


if len(sys.argv) < 2:
    # Without this the missing-port case is an IndexError traceback on stdout, which the gate's
    # DRIVER-FAIL grep does not match and whose first line names neither this file's contract nor
    # the caller's mistake.
    print("DRIVER-FAIL usage: sink_console.py <tcp-port> [budget-seconds]")
    sys.exit(2)

HOST = "127.0.0.1"
PORT = int(sys.argv[1])
BUDGET = float(sys.argv[2]) if len(sys.argv) > 2 else 50.0
DEADLINE = time.time() + BUDGET
STATUS_BOUND = 0.9          # seconds of WALL time; see the module docstring for why this one is
                            # allowed, and for the measurement it rests on (idle only).
STATUS_WAIT = 5.0           # ... and the sub-budget the wait for it gets, so a broken `status` fails FAST
                            # rather than burning the whole budget and leaving QEMU to gtimeout.

# The heartbeat block `status` must produce, spelled as a pattern: `cmd=status`, the `hb ` line, then
# EXACTLY four more lines (bt_sink, bt_jit, bt_link, bt_hci) before `bt_avrcp` closes it.  printHeartbeat()
# emits all six unconditionally -- no branch inside it prints or skips a line -- so the count is exact and a
# block that grows a line here should fail LOUDLY (a DRIVER-FAIL naming this wait) rather than silently
# start matching across two blocks again.
STATUS_BLOCK = r"^cmd=status\nhb [^\n]*\n(?:[^\n]*\n){4}bt_avrcp [^\n]*\n"

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
    die("could not connect to the console socket")


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

    ★ What the region therefore INCLUDES: a line already partially received when the mark was taken,
    and every byte still sitting unread in the kernel's receive buffer.  So a mark does not mean "sent
    after this instant" -- it means "not yet complete at this instant".  Every token waited on under a
    mark here is sound on that basis: four are printed ONLY by the command handler
    (`pairing=refused`, `pairing=on reason=cmd`, `bonds_forgotten=`, `cmd=status`), and the other two
    -- `pairing=on reason=drop` and `scan_enable=0x03` -- are once-per-run edges whose only earlier
    occurrence is at boot, far behind any mark this file takes.  A wait on `hb ` or `bt_avrcp `, which
    the firmware emits on a timer, would NOT be sound under a mark; none is taken.
    """
    return buf.rfind(b"\n") + 1


def wait_for(s, pat, what, start=0, budget=None):
    """Block until a line matching pat has been RECEIVED at or after `start`.

    Two distinct failures, reported apart, because they mean different things to whoever reads the
    log: EOF is QEMU gone (killed by qrun, or dead), a timeout is QEMU alive and not saying it.
    """
    global buf
    t_begin = time.time()
    end = DEADLINE if budget is None else min(DEADLINE, t_begin + budget)
    while time.time() < end:
        if re.search(pat, text(start), re.M):
            return
        try:
            b = s.recv(4096)
        except socket.timeout:
            continue
        except OSError as e:
            die("console socket error while waiting for %s: %s" % (what, e))
        if not b:
            die("console socket closed by QEMU while waiting for %s (after %.1fs of the %.1fs "
                "left to this wait)" % (what, time.time() - t_begin, end - t_begin))
        buf += b
    die("timed out waiting for %s (after %.1fs)" % (what, time.time() - t_begin))


def send(s, line, note):
    # sendall(), not send(): a short write silently drops the tail of the command, and a command the
    # firmware never fully received fails downstream as a firmware defect.  (The same reason
    # serial/serial_test_rx/qemu_rx_driver.py uses it.)  Guarded so a QEMU that has already died
    # gives a DRIVER-FAIL line rather than a traceback the gate's grep cannot see.
    try:
        s.sendall((line + "\n").encode())
    except OSError as e:
        die("could not send `%s` (%s): %s" % (line, note, e))
    print("DRIVER-SENT %s %s" % (line, note))
    sys.stdout.flush()


s = connect()
print("DRIVER-CONNECTED")
sys.stdout.flush()

# 1. While STREAMING: `pair` must be REFUSED.  This is also the proof that the whole command path --
#    socket, LPUART1 RX, serialEvent1(), the parser, runCommand() -- works at all, so it comes first.
wait_for(s, r"^streaming by=incoming", "STREAMING")
# A settle margin, and NOT a synchronisation: BtSinkSession assigns m_state = STREAMING before it
# calls the stream callback that prints this line (BtSinkSession.cpp, the CONNECTING -> STREAMING
# transition), so `pair` is refused link_up with or without the wait -- removing it does not change
# the assertion.  It exists to keep the command clear of the bring-up burst the sink prints right
# after, so the refusal reads on its own in the capture.
time.sleep(0.5)
m0 = mark()
send(s, "pair", "while-streaming")
wait_for(s, r"^pairing=refused reason=link_up$", "the refusal", m0)

# 2. The peer drops the link once it has streamed every packet and set the volume; the sink opens an
#    automatic DROP window.  Only then do the commanded ones mean anything -- with a link up they are
#    all refused.
md = mark()                                   # a FRESH mark, not m0: see mark()'s note -- every wait
                                              # here takes the mark that immediately precedes it, and
                                              # this was the one place that re-used an older one.
wait_for(s, r"^pairing=on reason=drop", "the drop window", md)
# ★ THEN WAIT FOR THE SINK'S OWN SCAN WRITE BEFORE TYPING ANYTHING, and say so in the log.  This is what
# makes the gate's `PEER-SCAN-ENABLE 0x03` attributable to the DROP window rather than to "some window":
# BtLink::reconcileScan() writes only when the composed value CHANGES, so the drop window's 0x03 and the two
# commanded windows' coalesce into ONE controller write, and whichever window came first owns it.  Sending
# `pair` the instant the drop line appears is what made the commanded window first in the pre-review capture
# (measured: `pairing=on reason=cmd` twice, THEN `scan_enable=0x03`).  Waiting here inverts that for good.
# The mark is `md`, not a fresh one: `scan_enable=0x03` can arrive in the same recv() as the drop line, and
# the only earlier one in the run is at boot -- far behind `md`, which was taken while streaming.
wait_for(s, r"^scan_enable=0x03$", "the drop window's own scan write", md)
print("DRIVER-SAW scan_enable=0x03 after-drop")
sys.stdout.flush()
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
wait_for(s, STATUS_BLOCK, "the commanded heartbeat block", m3, STATUS_WAIT)
dt = time.time() - t0
print("DRIVER-STATUS-%s block after cmd=status in %.2fs (bound %.2fs, host clock)"
      % ("OK" if dt < STATUS_BOUND else "SLOW", dt, STATUS_BOUND))
print("DRIVER-DONE")
sys.stdout.flush()
