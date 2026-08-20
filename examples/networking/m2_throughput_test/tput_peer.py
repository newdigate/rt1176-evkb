#!/usr/bin/env python3
"""Mac-side peer for examples/networking/m2_throughput_test (W11).

The board runs four PASSIVE services; this script sequences the tests and is
the AUTHORITATIVE measurement: every mbps figure below comes from this side's
own byte counts and wall clock.  The board's serial `tput:` lines are the
cross-check (what the board thinks it saw), never the measurement.

  tcp-rx <ip>   Mac blasts TCP  -> board :5001 sink      (board RX direction)
  tcp-tx <ip>   board :5002 blasts TCP -> Mac receives   (board TX direction)
  udp-rx <ip>   Mac blasts UDP  -> board :5003 counts    (board RX + loss)
  udp-tx <ip>   "TPUT GO 10" -> board blasts UDP -> Mac  (board TX + loss)
  all    <ip>   the four in order with 2 s gaps, then summary lines:
                TPUT <test> mbps=<x.xx> [loss=<y.y>%]
  ab     <ip>   W16 A/B: tcp-rx twice on ONE association, once with the
                pre-W16 transport and once with the register port +
                aggregation, reporting COMMANDS PER FRAME (the verdict) with
                Mbps as context only.  Needs the W16 board image.

Python 3 stdlib only.
"""
import argparse
import socket
import struct
import sys
import time

TCP_RX_PORT, TCP_TX_PORT, UDP_PORT = 5001, 5002, 5003
UDP_PAYLOAD = 1400
DURATION = 10.0            # seconds of blasting per test


def mbps(nbytes, secs):
    return (nbytes * 8.0 / 1e6 / secs) if secs > 0 else 0.0


def tcp_rx(ip):
    # Mac -> board sink.  The clock runs from just before the first byte is
    # handed to the socket until sendall() of the last chunk returns
    # (send-completion).  That includes kernel buffering at both ends, which
    # is why the board's own `tput: tcp_rx bytes=/ms=` line is kept as the
    # cross-check: it counts what actually arrived, on its own clock.
    chunk = b"\xa5" * 65536
    s = socket.create_connection((ip, TCP_RX_PORT), timeout=10)
    sent = 0
    t0 = time.monotonic()
    while time.monotonic() - t0 < DURATION:
        s.sendall(chunk)
        sent += len(chunk)
    t1 = time.monotonic()
    s.shutdown(socket.SHUT_WR)         # FIN: board prints its result + closes
    s.settimeout(20)
    try:
        while s.recv(4096):            # drain until the board closes
            pass
    except socket.timeout:
        print("tcp-rx: WARN board never closed (no FIN back)")
    s.close()
    secs = t1 - t0
    r = {"test": "tcp-rx", "mbps": mbps(sent, secs)}
    print("tcp-rx: sent=%d bytes in %.2f s -> %.2f Mbps" % (sent, secs, r["mbps"]))
    return r


def tcp_tx(ip):
    # Board :5002 blasts for ~10 s then closes; count every received byte.
    # Clock runs from the first byte received to EOF.
    s = socket.create_connection((ip, TCP_TX_PORT), timeout=10)
    s.settimeout(20)
    got, t0, t1 = 0, None, None
    try:
        while True:
            b = s.recv(65536)
            if not b:                  # board closed: run complete
                break
            if t0 is None:
                t0 = time.monotonic()
            got += len(b)
            t1 = time.monotonic()
    except socket.timeout:
        print("tcp-tx: WARN timed out waiting for EOF")
    s.close()
    secs = (t1 - t0) if (t0 is not None and t1 is not None) else 0.0
    r = {"test": "tcp-tx", "mbps": mbps(got, secs)}
    print("tcp-tx: received=%d bytes in %.2f s -> %.2f Mbps" % (got, secs, r["mbps"]))
    return r


def udp_rx(ip):
    # Mac blasts seq'd datagrams, then asks the board how many arrived.
    # loss% = 1 - board_rx/sent.  mbps is the DELIVERED rate: board-counted
    # datagrams x payload over this side's wall clock.
    #
    # Protocol: an explicit acked "TPUT RESET" zeroes the board's counters
    # BEFORE any data is in flight (so there is no reset-vs-late-tail race),
    # and "TPUT STATS?" is purely idempotent -- a lost STATS reply just means
    # the retry reads the same counters, never a phantom 100%-loss run.
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(1.0)
    reset_ok = False
    for _ in range(3):
        try:
            s.sendto(b"TPUT RESET", (ip, UDP_PORT))
            while True:
                b, _ = s.recvfrom(2048)
                if b.startswith(b"TPUT RESETOK"):
                    reset_ok = True
                    break
        except socket.timeout:
            continue
        break
    if not reset_ok:
        print("udp-rx: WARN no RESETOK from board; counters may be stale")
    payload = bytearray(b"\xa5" * UDP_PAYLOAD)
    sent = 0
    t0 = time.monotonic()
    while time.monotonic() - t0 < DURATION:
        payload[0:4] = struct.pack(">I", sent)
        try:
            s.sendto(payload, (ip, UDP_PORT))
            sent += 1
        except OSError:                # local ENOBUFS: brief back-off
            time.sleep(0.001)
    secs = time.monotonic() - t0
    rx = hi = None
    board_ms = 0
    for _ in range(3):                 # STATS? is itself a datagram: retry
        try:
            s.sendto(b"TPUT STATS?", (ip, UDP_PORT))
            while True:
                b, _ = s.recvfrom(2048)
                if b.startswith(b"TPUT STATS "):
                    kv = dict(f.split(b"=", 1) for f in b.split()[2:])
                    rx, hi = int(kv[b"rx"]), int(kv[b"hi"])
                    board_ms = int(kv.get(b"ms", b"0"))
                    break
        except (socket.timeout, ValueError, KeyError, IndexError):
            continue
        break
    s.close()
    loss = (1.0 - rx / sent) * 100.0 if (rx is not None and sent) else None
    r = {"test": "udp-rx",
         "mbps": mbps((rx or 0) * UDP_PAYLOAD, secs),
         "loss": loss}
    if rx is None:
        print("udp-rx: sent=%d in %.2f s; WARN no STATS reply from board" % (sent, secs))
    else:
        # board_ms is the board's own first->last datagram span: its rate is
        # the cross-check on ours, not the reported figure.
        xcheck = (" (board: %.2f Mbps over %d ms)"
                  % (mbps(rx * UDP_PAYLOAD, board_ms / 1000.0), board_ms)
                  if board_ms else "")
        print("udp-rx: sent=%d board-rx=%d hi=%d in %.2f s -> %.2f Mbps delivered, loss=%.1f%%%s"
              % (sent, rx, hi, secs, r["mbps"], loss, xcheck))
    return r


def udp_tx(ip):
    # Ask the board to blast at us for 10 s; count what lands.  loss% =
    # 1 - received/board_tx (board_tx from its TPUT DONE report).
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("", 0))                    # one ephemeral socket for the run
    try:                               # bigger buffer = fewer local drops
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 << 20)
    except OSError:
        pass
    s.sendto(b"TPUT GO 10", (ip, UDP_PORT))
    s.settimeout(0.5)
    got_dg, got_bytes, board_tx = 0, 0, None
    t0 = t1 = None
    t_end = time.monotonic() + 12.0    # 10 s blast + slack for DONE
    while time.monotonic() < t_end:
        try:
            b, _ = s.recvfrom(4096)
        except socket.timeout:
            continue
        if b.startswith(b"TPUT DONE "):
            try:
                board_tx = int(b.split(b"=", 1)[1])
            except (IndexError, ValueError):
                pass
            break                      # blast is over
        if b.startswith(b"TPUT "):
            continue                   # stray control traffic
        now = time.monotonic()
        if t0 is None:
            t0 = now
        t1 = now
        got_dg += 1
        got_bytes += len(b)
    s.close()
    secs = (t1 - t0) if (t0 is not None and t1 is not None) else 0.0
    loss = (1.0 - got_dg / board_tx) * 100.0 if board_tx else None
    r = {"test": "udp-tx", "mbps": mbps(got_bytes, secs), "loss": loss}
    if board_tx is None:
        print("udp-tx: received=%d datagrams; WARN no TPUT DONE from board" % got_dg)
    else:
        print("udp-tx: board-tx=%d received=%d in %.2f s -> %.2f Mbps, loss=%.1f%%"
              % (board_tx, got_dg, secs, r["mbps"], loss))
    return r


TESTS = (("tcp-rx", tcp_rx), ("tcp-tx", tcp_tx),
         ("udp-rx", udp_rx), ("udp-tx", udp_tx))


def _ctrl(ip, req, expect, tries=4):
    """Send a UDP control line and return the board's reply, or None."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(1.5)
    try:
        for _ in range(tries):
            try:
                s.sendto(req.encode(), (ip, UDP_PORT))
                while True:
                    b, _a = s.recvfrom(2048)
                    if b.startswith(expect.encode()):
                        return b.decode(errors="replace").strip()
            except socket.timeout:
                continue
    finally:
        s.close()
    return None


def _local_addrs():
    """Every IPv4 address this host answers to, so `ab` can spot self-talk."""
    out = set()
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            out.add(info[4][0])
    except OSError:
        pass
    # The address actually used to reach the board's subnet.
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("192.168.4.1", 9))
        out.add(s.getsockname()[0])
        s.close()
    except OSError:
        pass
    return out


def _reachable(ip):
    """Is the board answering at all, and does it know the W16 commands?

    Split deliberately.  `TPUT STATS?` exists in EVERY image this example has
    ever had, so it separates three failures a single probe would conflate:
    nothing there, an old image, and a working board.  The first version of
    this script reported "is it the W16 image?" for all three, and sent a real
    diagnosis (a DHCP address collision) chasing a firmware ghost.
    """
    if ip in _local_addrs():
        return ("self", "%s is one of THIS machine's own addresses" % ip)
    if _ctrl(ip, "TPUT STATS?", "TPUT STATS", tries=3) is None:
        return ("unreachable", "no reply to TPUT STATS? at %s" % ip)
    if _ctrl(ip, "TPUT BUS?", "TPUT BUS", tries=3) is None:
        return ("old", "board answers STATS but not BUS? -- pre-W16 image")
    return ("ok", "")


def _bus(ip):
    """One snapshot of the board's cumulative bus counters, as a dict."""
    line = _ctrl(ip, "TPUT BUS?", "TPUT BUS")
    if line is None:
        return None
    out = {}
    for tok in line.split()[2:]:
        k, _, v = tok.partition("=")
        try:
            out[k] = int(v)
        except ValueError:
            pass
    return out


def ab(ip):
    """W16 A/B: the SAME blast, twice, on one association and one firmware life.

    Why this and not a comparison against the W11 baseline table: that table was
    measured against a house router with the board on 2.4 GHz and this Mac on
    the router's 5 GHz side -- two radios, no shared airtime.  The ESP8266 bench
    AP is ONE 2.4 GHz radio relaying both stations, so every byte crosses the
    air twice on the same channel.  A bench Mbps compared against that table
    measures the AP, not the driver.  Flipping the driver's switches between two
    blasts minutes apart removes the AP, the air and the firmware life from the
    comparison and leaves only the thing under test.

    THE VERDICT IS COMMANDS PER FRAME, NOT MBPS.  2.4 GHz variance is 2x-4x run
    to run on identical builds, so Mbps is reported as context and nothing else.
    Every counter is cumulative and per-firmware-life, so each arm consumes a
    DELTA -- comparing absolute counters across runs of different length is the
    error that produced two published wrong conclusions in W12.
    """
    why, detail = _reachable(ip)
    if why != "ok":
        print("ab: %s" % detail, file=sys.stderr)
        if why == "self":
            print("ab: an ESP8266 SoftAP hands out 192.168.4.100 FIRST -- after the AP\n"
                  "    reboots, its lease table is empty and whichever station asks\n"
                  "    first takes that address.  Check the board's own `tput: ip=`\n"
                  "    line on the serial console and use THAT, and confirm\n"
                  "    `ipconfig getifaddr en0` differs from it.", file=sys.stderr)
        elif why == "unreachable":
            print("ab: the board may still be re-associating -- after an AP reboot it\n"
                  "    waits up to 5 s, then scans (which can take 15 s) before DHCP.\n"
                  "    Watch the serial console for `link_reup` and a `tput: ip=`\n"
                  "    line, then re-run.", file=sys.stderr)
        elif why == "old":
            print("ab: reflash m2_throughput_test -- `TPUT MODE` and `TPUT BUS?` are\n"
                  "    W16 additions.", file=sys.stderr)
        return {"test": "ab", "failed": detail}

    arms = []
    for mode, label in ((0, "pre-W16 (CMD52, no batching, polled)"),
                        (1, "W16 (register port + aggregation, polled)"),
                        (2, "W16 + irq mode (DAT1 -- see isr= below)")):
        ok = _ctrl(ip, "TPUT MODE %d" % mode, "TPUT MODEOK")
        if ok is None:
            # _reachable() already proved the board answers BUS?, so this is a
            # lost reply rather than a missing feature.
            print("ab: no MODEOK for mode %d, though the board answered BUS? a"
                  " moment ago -- retrying once" % mode, file=sys.stderr)
            ok = _ctrl(ip, "TPUT MODE %d" % mode, "TPUT MODEOK", tries=6)
        if ok is None:
            print("ab: board never acked TPUT MODE %d" % mode, file=sys.stderr)
            return {"test": "ab", "failed": "no MODEOK"}
        print("ab: %s -> %s" % (label, ok))
        before = _bus(ip)
        if before is None:
            print("ab: no BUS reply", file=sys.stderr)
            return {"test": "ab", "failed": "no BUS"}
        time.sleep(1)
        res = tcp_rx(ip)
        time.sleep(2)                  # let the ring drain before sampling
        after = _bus(ip)
        if after is None:
            print("ab: no BUS reply after the blast", file=sys.stderr)
            return {"test": "ab", "failed": "no BUS"}
        d = {k: after.get(k, 0) - before.get(k, 0) for k in after}
        d["mbps"] = res["mbps"]
        d["label"] = label
        arms.append(d)
        time.sleep(2)

    print()
    print("=== W16 A/B, one association, one firmware life ===")
    print("%-38s %7s %8s %9s %7s %7s %6s %6s"
          % ("arm", "frames", "bus", "cmd/frame", "poll", "data", "slots", "Mbps"))
    for a in arms:
        f = a.get("frames", 0)
        b = a.get("total", 0)
        # The split that matters: what the SERVICE LOOP spent asking a quiet
        # card whether it had work, versus what the frames themselves cost.
        data = a.get("c53rx", 0) + a.get("c53tx", 0)
        poll = b - data
        print("%-38s %7d %8d %9.2f %7.2f %7.2f %6d %6.2f"
              % (a["label"], f, b, (b / f) if f else float("nan"),
                 (poll / f) if f else float("nan"),
                 (data / f) if f else float("nan"),
                 a.get("rxslots", 0), a["mbps"]))
    base = arms[0] if arms and arms[0].get("frames") else None
    if base:
        c0 = base["total"] / base["frames"]
        for a in arms[1:]:
            if a.get("frames"):
                c1 = a["total"] / a["frames"]
                if c1 > 0:
                    print("\n%-38s %.2f -> %.2f commands per frame  (%.1fx fewer)"
                          % (a["label"], c0, c1, c0 / c1))
    print("\nDAT1: isr = the ISR's own count, cardints = what serviceLink consumed.")
    print("      BOTH zero in an 'interrupt' arm means no interrupt was involved,")
    print("      whatever the command count did -- the win came from somewhere else.")
    for a in arms:
        print("  %-38s isr=%d cardints=%d"
              % (a["label"], a.get("isr", 0), a.get("cardints", 0)))
    print("\nhealth deltas (both arms must stay clean):")
    for a in arms:
        print("  %-34s stranded=%d resyncs=%d notready=%d drainerr=%d split=%d"
              % (a["label"], a.get("stranded", 0), a.get("resyncs", 0),
                 a.get("notready", 0), a.get("drainerr", 0), a.get("split", 0)))
    print("\nMbps is CONTEXT, not the verdict: 2.4 GHz varies 2x-4x run to run,")
    print("and an ESP8266 SoftAP relaying both stations is its own ceiling.")
    # Leave the board in the polled W16 mode rather than in whichever arm ran
    # last: interrupt mode is still DEFAULT OFF in the driver pending a load
    # soak, and a bench run must not quietly leave the board in a mode the
    # driver does not ship.
    _ctrl(ip, "TPUT MODE 1", "TPUT MODEOK")
    return {"test": "ab", "mbps": arms[-1]["mbps"]}


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name, _ in TESTS + (("all", None), ("ab", None)):
        p = sub.add_parser(name)
        p.add_argument("ip", help="board IP (from its `tput: ip=` status line)")
    args = ap.parse_args()

    try:
        if args.cmd == "all":
            results = []
            for i, (name, fn) in enumerate(TESTS):
                if i:
                    time.sleep(2)      # let the previous run's state settle
                try:                   # one dead service must not kill the rest
                    results.append(fn(args.ip))
                except OSError as e:
                    print("%s: ERROR %s" % (name, e), file=sys.stderr)
                    results.append({"test": name, "failed": str(e)})
            for r in results:
                if "failed" in r:
                    print("TPUT %s FAILED (%s)" % (r["test"], r["failed"]))
                    continue
                line = "TPUT %s mbps=%.2f" % (r["test"], r["mbps"])
                if r.get("loss") is not None:
                    line += " loss=%.1f%%" % r["loss"]
                print(line)
        elif args.cmd == "ab":
            ab(args.ip)
        else:
            dict(TESTS)[args.cmd](args.ip)
    except OSError as e:
        print("ERROR: %s" % e, file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
