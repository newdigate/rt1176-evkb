#!/usr/bin/env python3
"""Mac-side peer for examples/networking/wifi_server_test.

The board runs a WiFiServer echo on :5010; this script is the AUTHORITATIVE
side of the measurement -- it counts its own bytes and compares them itself.
The board's `alive= ... sess=/bytes= evict= stall= refuse=` heartbeat is the
CROSS-CHECK, not the measurement: firmware cannot fake bytes that made a full
round trip through lwip and the radio.

  echo <ip>        3 sequential connections, byte-exact echo each
  concurrent <ip>  2 connections open AT THE SAME TIME with data staged on
                   both before either is read -- the pool holds two live
                   conns at once
  fill <ip>        4 idle connections (they never send, so the sketch's
                   available()-based loop never claims them), then a 5th
                   connect + echo that can only succeed through the pool's
                   EVICTION valve -- and this side then CONFIRMS a peer was
                   evicted, rather than inferring it (see ★ SELF-VERIFYING).
                   Prints `fill evicted_peers=N`.  The board's evict= counter
                   is the cross-check (read the caveat before expecting 1).
  all <ip>         the three in order; prints WIFISRV <test> PASS/FAIL lines
  soak <ip> <minutes>
                   NEW-8: sustained MIXED load until the deadline -- repeats
                   [echo x3, concurrent, fill] cycles, because the shapes'
                   transitions are where the interesting failures live, with
                   a 60 s quiet window every 20 cycles so the board's `pcb
                   tw=` line can be watched DRAINING as well as saturating.
                   Every soak payload is EXACTLY SOAK_SIZE (40) bytes and
                   unique, so the board's final `sess=N/B` must satisfy
                   B == N x 40 TO THE BYTE against this side's own counts --
                   an accounting that closes is the assertion; a counter that
                   merely climbs is not.  Prints a progress line per minute
                   (a ratio cannot distinguish a constant from a rate; the
                   intermediate points are what can).  THE PEER IS
                   AUTHORITATIVE: compare the board's sess= DELTA across the
                   soak, never its absolute (it may carry pre-soak history).
Exit status is 0 only if every test selected passed.  For soak: 0 iff at
least one exchange completed and NO byte ever came back wrong or short
(connect failures and eviction counts are REPORTED, not failed on -- under
deliberate pcb pressure a refused connect is data, not a defect).
Python 3 stdlib only.

★ SELF-VERIFYING `fill`, because the obvious version is not.  "The 5th echo
worked" does NOT imply anything was evicted: a server with more slots, or one
that simply never fills, passes that test identically -- measured, against a
stand-in whose only difference was a larger capacity.  The proof is available
because eviction is tcp_abort() (WiFiConnPool.cpp), which puts an RST on the
wire: the evicted peer can SEE it.  So after the 5th echo this script selects
the four idlers for a couple of seconds and counts how many the board dropped.
It asserts >= 1, not == 1, and prints the count -- on a real link a lost RST
should degrade to a diagnosable number, not to a flaky hard FAIL.

★ PAYLOADS MUST FIT ONE TCP SEGMENT, and every message here is ~40 bytes so
they do.  The board echoes ONE available() snapshot and then closes, so a
payload split across two segments gets a partial echo followed by a FIN --
which on a bench reads as intermittent packet loss rather than as the
one-shot contract working exactly as designed.  Enlarge these messages and
that is what you will be debugging.

★ THE BOARD IS A ONE-SHOT SERVER, and this script is written to that contract.
wifi_server_test.cpp takes a transient `WiFiClient` from server.available()
each loop() pass; a WiFiClient is a refcounted handle and the connection is
CLOSED when the last one dies, so the board closes after echoing.  That is why
no test here sends a second message down a connection it already used -- it
would be answered by a RST, and the failure would look like packet loss rather
than like the design it is.  (wifi_client_test.cpp keeps its handle in a
static and holds one session open; that is the other half of the API.)

★ SOAK MODE DEMONSTRATED both ways before being trusted (2026-08-26), against
a localhost one-shot simulator implementing this server's contract (4-slot
pool, RST-eviction of the oldest silent conn on a 5th accept):
  - clean sim:      exch=6 bytes=240 mismatch=0, accounting closes 6 x 40,
                    evicted=1 observed -- exit 0.
  - corrupting sim (last byte flipped in every echo): mismatch=6 exch=0,
    "accounting will NOT close" -- exit 1.  A soak that cannot fail on a
    corrupt echo would be decoration.

★ CAVEAT on `fill`, worth reading before you disbelieve a counter: a peer that
merely CLOSES an unclaimed connection does not free its pool slot -- lwip
reports the FIN, the slot goes PEER_CLOSED, and only the stall valve reaps it
(WiFiConnPool.cpp connPoll).  So the idlers this test leaves behind still
occupy slots for a while afterwards.  Note WHERE that clock starts, because
the intuitive answer is wrong: connRecv's FIN branch returns BEFORE it touches
lastActivityMs, so connPoll measures 30-40 s from the ACCEPT, not from your
close.  The slots therefore come back SOONER than "40 s after I hung up"
suggests -- safe in that direction, but do not use your close as the anchor.
Running `fill` (or `all`) twice inside that window still PASSES -- the
leftovers are unclaimed, so the evictor takes them too -- but evict= will then
be larger than 1.  Compare the DELTA across the run, never the absolute value.
"""
import select
import socket
import sys
import time

PORT = 5010
TIMEOUT = 10


def _recv_exactly(sock, n):
    """Read n bytes, or return short on EOF/timeout. The board closes after it
    echoes, so a clean EOF right after the last byte is expected, not an error."""
    got = b""
    sock.settimeout(TIMEOUT)
    while len(got) < n:
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            break
        if not chunk:
            break
        got += chunk
    return got


def _echo_once(ip, tag):
    msg = f"WIFISRV {tag} {time.monotonic_ns()}".encode()
    with socket.create_connection((ip, PORT), timeout=TIMEOUT) as s:
        s.sendall(msg)
        got = _recv_exactly(s, len(msg))
    if got != msg:
        print(f"    sent {len(msg)}B {msg!r}", file=sys.stderr)
        print(f"    got  {len(got)}B {got!r}", file=sys.stderr)
    return got == msg


def t_echo(ip):
    ok = True
    for i in range(3):
        ok = _echo_once(ip, f"echo{i}") and ok      # run all 3, no short-circuit
    return ok


def t_concurrent(ip):
    """Two connections live at once, with bytes staged on BOTH before either is
    drained -- that is the part a sequential a/b/a/b round-trip would not prove,
    because the board could have finished with the first before the second ever
    existed."""
    socks = [socket.create_connection((ip, PORT), timeout=TIMEOUT)
             for _ in range(2)]
    msgs = [f"WIFISRV conc{i} {time.monotonic_ns()}".encode()
            for i in range(len(socks))]
    ok = True
    try:
        for s, m in zip(socks, msgs):
            s.sendall(m)                    # both staged before any read
        for s, m in zip(socks, msgs):
            got = _recv_exactly(s, len(m))
            if got != m:
                print(f"    sent {m!r}\n    got  {got!r}", file=sys.stderr)
            ok = (got == m) and ok
    finally:
        for s in socks:
            s.close()
    return ok


def _count_dropped(socks, seconds):
    """How many of these connections did the BOARD drop? Eviction is
    tcp_abort() (WiFiConnPool.cpp), i.e. an RST, which surfaces here as
    ConnectionResetError; a graceful close would surface as EOF. Either means
    the board let go of a connection this side never closed, so both count."""
    deadline = time.monotonic() + seconds
    pending, dropped = list(socks), 0
    while pending:
        left = deadline - time.monotonic()
        if left <= 0:
            break
        readable, _, _ = select.select(pending, [], [], left)
        if not readable:
            break                           # nothing more is coming
        for s in readable:
            pending.remove(s)               # each socket is judged once
            try:
                if s.recv(4096) == b"":
                    dropped += 1            # FIN
            except OSError:
                dropped += 1                # RST -- the eviction path
    return dropped


def t_fill(ip):
    """4 silent connections fill the pool (WIFI_MAX_CONNS == 4); a 5th can only
    be accepted if the eviction valve drops the least-recently-active unclaimed
    one. The idlers must never send: sending would stage bytes, the board's
    available() would surface and CLAIM that conn, and a claimed conn is exempt
    from eviction -- the 5th would then be REFUSED (refuse= climbing) and this
    test would fail for the opposite reason.

    Two independent things must hold, and the second is the one that makes this
    test worth running: the 5th echo must work, AND a peer must actually have
    been evicted. Without the second, a server with more slots passes this
    identically -- see ★ SELF-VERIFYING at the top."""
    idlers = [socket.create_connection((ip, PORT), timeout=TIMEOUT)
              for _ in range(4)]
    try:
        time.sleep(1)                       # let the board's accepts land
        echoed = _echo_once(ip, "evicted")  # 5th conn: needs the valve
        dropped = _count_dropped(idlers, 2.0)
        print(f"    fill evicted_peers={dropped}", flush=True)
        if not dropped:
            print("    no idler was dropped -- the 5th connection was accepted "
                  "without the eviction valve firing", file=sys.stderr)
        return echoed and dropped >= 1
    finally:
        for s in idlers:
            s.close()


# --- NEW-8 soak -------------------------------------------------------------

SOAK_SIZE = 40          # bytes, every soak payload exactly; fits one segment
_soak_seq = 0


def _soak_msg(kind):
    """Unique fixed-size payload. Unique so a stale echo from an earlier
    exchange can never satisfy a later one; fixed-size so the board-side
    accounting must close as B == N * SOAK_SIZE exactly."""
    global _soak_seq
    _soak_seq += 1
    m = f"WIFISRV soak {_soak_seq:08d} {kind} ".encode()
    assert len(m) <= SOAK_SIZE, f"kind {kind!r} overflows SOAK_SIZE"
    return m + b"." * (SOAK_SIZE - len(m))


def _soak_echo(ip, kind, st):
    """One exchange. Three distinct outcomes, counted separately because they
    mean different things on the board: exch (clean, board counted 40 bytes),
    mismatch (board holds a session and SOME bytes -- the accounting will show
    exactly how many), connfail (no session on either side)."""
    msg = _soak_msg(kind)
    try:
        with socket.create_connection((ip, PORT), timeout=TIMEOUT) as s:
            s.sendall(msg)
            got = _recv_exactly(s, len(msg))
    except OSError as e:
        st["connfail"] += 1
        st["last_err"] = f"{kind}: {type(e).__name__}: {e}"
        return
    if got == msg:
        st["exch"] += 1
        st["bytes"] += len(msg)
    else:
        st["mismatch"] += 1
        what = "content mismatch" if len(got) == len(msg) else "short/absent echo"
        st["last_err"] = f"{kind}: sent {len(msg)}B got {len(got)}B ({what})"


def _soak_concurrent(ip, st):
    """Two live at once, both staged before either is drained -- the soak
    keeps exercising the two-slot case, not just the sequential one."""
    socks = []
    try:
        try:
            socks = [socket.create_connection((ip, PORT), timeout=TIMEOUT)
                     for _ in range(2)]
        except OSError as e:
            st["connfail"] += 1
            st["last_err"] = f"conc-connect: {type(e).__name__}: {e}"
            return
        msgs = [_soak_msg(f"c{i}") for i in range(len(socks))]
        for s, m in zip(socks, msgs):
            s.sendall(m)
        for s, m in zip(socks, msgs):
            got = _recv_exactly(s, len(m))
            if got == m:
                st["exch"] += 1
                st["bytes"] += len(m)
            else:
                st["mismatch"] += 1
                st["last_err"] = f"conc: sent {len(m)}B got {len(got)}B"
    finally:
        for s in socks:
            s.close()


def _soak_fill(ip, st):
    """4 silent idlers + 1 echo through the eviction valve, as t_fill -- but
    under soak a zero-drop fill is RECORDED (fill_nodrop), not failed:
    leftovers from the previous cycle mean the valve's candidate set varies,
    and the number to judge at the END is evicted vs fills, not each one."""
    try:
        idlers = [socket.create_connection((ip, PORT), timeout=TIMEOUT)
                  for _ in range(4)]
    except OSError as e:
        st["connfail"] += 1
        st["last_err"] = f"fill-connect: {type(e).__name__}: {e}"
        return
    try:
        time.sleep(1)
        _soak_echo(ip, "fill", st)
        d = _count_dropped(idlers, 2.0)
        st["evicted"] += d
        if d == 0:
            st["fill_nodrop"] += 1
    finally:
        for s in idlers:
            s.close()


def t_soak(ip, minutes):
    st = dict(exch=0, bytes=0, connfail=0, mismatch=0, evicted=0,
              fill_nodrop=0, cycles=0, last_err="")
    t0 = time.monotonic()
    deadline = t0 + minutes * 60.0
    next_prog = t0 + 60.0

    def progress(force=False):
        nonlocal next_prog
        now = time.monotonic()
        if force or now >= next_prog:
            print(f"SOAK t={int((now - t0) // 60)}m cycles={st['cycles']} "
                  f"exch={st['exch']} bytes={st['bytes']} "
                  f"connfail={st['connfail']} mismatch={st['mismatch']} "
                  f"evicted={st['evicted']} fill_nodrop={st['fill_nodrop']}",
                  flush=True)
            while next_prog <= now:
                next_prog += 60.0

    while time.monotonic() < deadline:
        for i in range(3):
            _soak_echo(ip, f"e{i}", st)
            time.sleep(0.5)
        time.sleep(2)
        _soak_concurrent(ip, st)
        time.sleep(2)
        _soak_fill(ip, st)
        time.sleep(2)
        st["cycles"] += 1
        progress()
        if st["cycles"] % 20 == 0 and time.monotonic() < deadline:
            # Drain window: ~every 4 min of load, 60 s of silence.  The
            # board's tw= should fall over this window (TIME_WAIT expiry) --
            # saturation AND recovery are both part of the pcb measurement.
            print(f"SOAK quiet window 60s after cycle {st['cycles']}",
                  flush=True)
            time.sleep(60)
    dur = (time.monotonic() - t0) / 60.0
    sessions = st["exch"] + st["mismatch"]
    print(f"SOAK done minutes={dur:.1f} cycles={st['cycles']} "
          f"exch={st['exch']} bytes={st['bytes']} connfail={st['connfail']} "
          f"mismatch={st['mismatch']} evicted={st['evicted']} "
          f"fill_nodrop={st['fill_nodrop']}", flush=True)
    if st["mismatch"] == 0:
        print(f"SOAK expect board sess DELTA = +{sessions}/+{sessions * SOAK_SIZE} "
              f"(exactly, {sessions} x {SOAK_SIZE}B)", flush=True)
    else:
        print(f"SOAK accounting will NOT close: {st['mismatch']} mismatched "
              f"exchange(s), last: {st['last_err']}", flush=True)
    if st["last_err"] and st["mismatch"] == 0:
        print(f"SOAK last_nonfatal: {st['last_err']}", flush=True)
    return st["mismatch"] == 0 and st["exch"] > 0


# ORDER MATTERS for `all`, and only this dict expresses it: `fill` must run
# LAST. It leaves 3 unclaimed PEER_CLOSED slots behind (see the ★ CAVEAT), and
# ahead of `echo`/`concurrent` those still pass but make the board's evict=
# readings disagree with everything documented here -- the accepts in front of
# a near-full pool start evicting the leftovers. Reordering this dict silently
# changes what a bench sees on the serial line.
TESTS = {"echo": t_echo, "concurrent": t_concurrent, "fill": t_fill}


def main():
    if len(sys.argv) == 4 and sys.argv[1] == "soak":
        ok = t_soak(sys.argv[2], float(sys.argv[3]))
        print(f"WIFISRV soak {'PASS' if ok else 'FAIL'}", flush=True)
        sys.exit(0 if ok else 1)
    if len(sys.argv) != 3 or (sys.argv[1] not in TESTS and sys.argv[1] != "all"):
        print(__doc__)
        sys.exit(2)
    test, ip = sys.argv[1], sys.argv[2]
    names = list(TESTS) if test == "all" else [test]
    rc = 0
    for n in names:
        try:
            ok = TESTS[n](ip)
        except OSError as e:                # a refused/timed-out connect is a
            print(f"    {type(e).__name__}: {e}", file=sys.stderr)
            ok = False                      # test result, not a traceback
        print(f"WIFISRV {n} {'PASS' if ok else 'FAIL'}", flush=True)
        rc |= 0 if ok else 1
        time.sleep(2)
    sys.exit(rc)


if __name__ == "__main__":
    main()
