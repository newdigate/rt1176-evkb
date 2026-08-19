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


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name, _ in TESTS + (("all", None),):
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
        else:
            dict(TESTS)[args.cmd](args.ip)
    except OSError as e:
        print("ERROR: %s" % e, file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
