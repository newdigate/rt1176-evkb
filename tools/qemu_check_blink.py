#!/usr/bin/env python3
"""Boot an ELF in the mimxrt1170-evk QEMU machine and check memory via QMP.

Modes:
  --symbol S --expect V   : read symbol S (from ELF) once, assert == V
  --symbol S --advances   : assert symbol S's value increases over --seconds
  --addr A --bit B --toggles : assert the bit at A goes both 0 and 1 over --seconds
"""
import argparse, subprocess, json, socket, time, os, re, sys
QEMU = os.path.expanduser("~/Development/qemu2/build/qemu-system-arm")
NM = "/Applications/ARM_10/bin/arm-none-eabi-nm"

def sym_addr(elf, sym):
    for line in subprocess.check_output([NM, elf]).decode().splitlines():
        p = line.split()
        if len(p) == 3 and p[2] == sym:
            return int(p[0], 16)
    raise SystemExit(f"symbol {sym} not found in {elf}")

def qmp_send(sock, cmd):
    """Send a QMP command and collect the response, retrying until we get it."""
    sock.sendall((json.dumps(cmd) + "\n").encode())
    # Read with a generous timeout; response may come in multiple chunks
    sock.settimeout(3.0)
    buf = b""
    try:
        while True:
            chunk = sock.recv(65536)
            if not chunk:
                break
            buf += chunk
            # A complete JSON response ends with a newline; accept partial too
            try:
                json.loads(buf.decode())
                break
            except json.JSONDecodeError:
                continue
    except socket.timeout:
        pass
    finally:
        sock.settimeout(None)
    return buf.decode()

def qmp_drain(sock, timeout=0.5):
    """Drain any pending data from the socket (e.g. the QMP greeting)."""
    sock.settimeout(timeout)
    buf = b""
    try:
        while True:
            chunk = sock.recv(65536)
            if not chunk:
                break
            buf += chunk
    except socket.timeout:
        pass
    finally:
        sock.settimeout(None)
    return buf.decode()

def read_word(sock, addr):
    r = qmp_send(sock, {"execute": "human-monitor-command",
                        "arguments": {"command-line": f"xp/1wx 0x{addr:08x}"}})
    m = re.search(r":\s*0x([0-9a-fA-F]+)", r)
    return int(m.group(1), 16) if m else None

def connect_qmp(port, retries=20, delay=0.3):
    """Try to connect to QMP, retrying while QEMU is starting up."""
    for attempt in range(retries):
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=2.0)
            s.settimeout(None)
            return s
        except (ConnectionRefusedError, OSError):
            if attempt < retries - 1:
                time.sleep(delay)
    raise SystemExit(f"Could not connect to QMP on port {port} after {retries} attempts")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("--symbol"); ap.add_argument("--expect")
    ap.add_argument("--advances", action="store_true")
    ap.add_argument("--toggles", action="store_true")
    ap.add_argument("--addr"); ap.add_argument("--bit", type=int)
    ap.add_argument("--seconds", type=float, default=2.0)
    a = ap.parse_args()

    port = 55123
    proc = subprocess.Popen(
        [QEMU, "-M", "mimxrt1170-evk", "-global",
         "fsl-imxrt1170.boot-xip=on", "-nographic", "-serial", "null",
         "-kernel", a.elf, "-qmp", f"tcp:127.0.0.1:{port},server,nowait"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    try:
        # Connect with retry loop — QEMU may not be ready immediately
        s = connect_qmp(port)

        # QMP greeting: drain it (contains server capabilities banner)
        greeting = qmp_drain(s, timeout=2.0)
        if not greeting:
            raise SystemExit("No QMP greeting received")

        # Negotiate capabilities
        resp = qmp_send(s, {"execute": "qmp_capabilities"})
        if '"return"' not in resp:
            raise SystemExit(f"qmp_capabilities failed: {resp!r}")

        addr = int(a.addr, 0) if a.addr else sym_addr(a.elf, a.symbol)
        seen = set(); first = last = None; end = time.time() + a.seconds

        while time.time() < end:
            v = read_word(s, addr)
            if v is not None:
                if a.toggles or a.bit is not None:
                    seen.add((v >> a.bit) & 1)
                if first is None:
                    first = v
                last = v
            time.sleep(0.05)

        # Infer mode: --bit without --toggles still means toggles mode
        use_toggles = a.toggles or (a.bit is not None and not a.advances and a.expect is None)

        if use_toggles:
            ok = 0 in seen and 1 in seen
            print(f"bits seen: {sorted(seen)}")
        elif a.advances:
            ok = first is not None and last > first
            print(f"first={first} last={last}")
        elif a.expect is not None:
            ok = last == int(a.expect, 0)
            print(f"value=0x{last:x} expect={a.expect}")
        else:
            # Default: just report the value; pass if we read anything
            ok = last is not None
            print(f"value=0x{last:x}" if last is not None else "value=<none>")

        print("PASS" if ok else "FAIL")
        sys.exit(0 if ok else 1)

    finally:
        proc.terminate()
        proc.wait(timeout=5)

if __name__ == "__main__":
    main()
