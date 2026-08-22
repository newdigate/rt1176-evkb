#!/usr/bin/env python3
"""Read RT1176 firmware state over SWD -- symbols, memory, SCB registers.

    tools/rt1170-swdprobe.py --elf build/acid_box.elf -c "p systick_millis_count"
    tools/rt1170-swdprobe.py --elf build/x.elf --file probe.gdb
    tools/rt1170-swdprobe.py -c "x/1xw 0xE000EDF0"          # no ELF needed

The companion to rt1170-screenshot.py: that one dumps the framebuffer, this one
dumps STATE. Together they are a complete bench oracle with no serial console,
which is what the MCU-Link VCOM dying for two days made necessary.

The lifecycle is lifted from rt1170-screenshot.py, which earned it: SIGKILL all
three probe daemons, start `LinkServer gdbserver --attach`, wait for the
LAUNCHER'S LOG LINE (never TCP-probe the gdb port -- a bare connect consumes the
stub's single first-client slot and poisons the session), let gdb be the first
and only client, detach.

★ WHAT THIS MODE CAN AND CANNOT DO. `--attach` runs no connect script, so it
wakes no CM4 and resets nothing. The trade is that it is genuinely NON-INVASIVE
and therefore read-only:

  - MEMORY READS ARE LIVE AND TRUSTWORTHY. Two reads of a counter in one session
    will differ if the core is running. This is the whole value of the tool.
  - CORE REGISTER READS ARE NOT. On a running core the stub reports a FAKE STOP
    and hands gdb 0 for r0-r15, sp, lr, pc, xpsr, everything. Measured on a
    healthy image whose systick advanced 2 ms between two reads in that same
    session. NEVER quote `info registers` from this mode as evidence.
  - WRITES ARE SILENTLY DROPPED, including to debug registers. `set var x = 1`
    reads back unchanged; so does a DHCSR halt request.

★ HOW TO TELL "THE FIRMWARE FROZE" FROM "THE DEBUGGER HALTED IT". This
distinction cost a whole session once (see
examples/display/acid_box/transcript_hw_evkb.txt) because the two are
indistinguishable from a counter alone: a halted core's systick_millis_count is
frozen, and it SURVIVES `wiretimedreset`, because a core stuck in debug halt
never re-runs startup and the DTCM value simply persists. Every later read then
returns the identical number, which reads exactly like a reproducible firmware
freeze.

  Read DHCSR (0xE000EDF0) FIRST, every time:
      bit 17 S_HALT      = 1 -> the core is HALTED. A frozen counter proves
                                nothing. Say nothing about liveness.
      bit 24 S_RETIRE_ST = 1 -> instructions are retiring. The core is RUNNING,
                                and a counter read now is real.
      bit 19 S_LOCKUP    = 1 -> genuine ARM lockup, the only thing that earns
                                the word.
  0x01010001 is the healthy running value; 0x00030003 is halted-by-debugger.

  And when a counter IS frozen, confirm with a SECOND clock before believing it.
  systick_millis_count and an audio graph's sample counter agreeing to
  milliseconds over minutes is a much stronger statement than either alone.

★ FAULT STATE IS MEMORY, SO IT IS READABLE HERE EVEN WHEN REGISTERS ARE NOT:
  CFSR 0xE000ED28, HFSR 0xE000ED2C, DFSR 0xE000ED30, MMFAR 0xE000ED34,
  BFAR 0xE000ED38, ABFSR 0xE000EFA8, SHCSR 0xE000ED24, ICSR 0xE000ED04
  (VECTACTIVE in bits 8:0), VTOR 0xE000ED08.
  CFSR == HFSR == 0 means no fault has been taken, full stop.

  Note DEMCR (0xE000EDFC): any LinkServer connect script arms every vector
  catch (0x010007F0), and vector catch halts the core INSTEAD OF running its
  handler. So "my fault handler never ran" is the EXPECTED observation under a
  debug connect, with or without a fault -- read CFSR/HFSR rather than inferring
  from a breakpoint that never hit.
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


def _kill_daemons():
    # SIGKILL, not SIGTERM: a crt_emu_cm_redlink stub that never got a gdb
    # client ignores SIGTERM while it waits and keeps the probe open.
    # ★ But never kill one that is mid-FLASH-PROGRAM: doing that once left the
    # target unreachable at the wire level ("Wire not connected" on every
    # transfer, dapinfo included) while the MCU-Link still enumerated fine over
    # USB, and only a full board POWER CYCLE brought it back.
    for pat in ("LinkServer", "redlinkserv", "crt_emu_cm_redlink"):
        subprocess.run(["pkill", "-9", "-f", pat], capture_output=True)
    for _ in range(10):
        if subprocess.run(["pgrep", "-f",
                           "LinkServer|redlinkserv|crt_emu_cm_redlink"],
                          capture_output=True).returncode != 0:
            return
        time.sleep(0.5)
    sys.stderr.write("swdprobe: warning: probe daemons still resident\n")


def run(cmds, elf, log_path, timeout):
    _kill_daemons()
    time.sleep(1)
    log = open(log_path, "w")
    srv = subprocess.Popen([LINKSERVER, "gdbserver", "--gdb-port", str(PORT),
                            "--semihost-port", "-1", "--attach", DEVICE],
                           stdout=log, stderr=subprocess.STDOUT)
    try:
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
            sys.stderr.write("swdprobe: gdbserver never started listening; "
                             "log tail:\n" + open(log_path).read()[-2000:])
            return None
        time.sleep(1)
        argv = [GDB, "-batch", "-nx"]
        if elf:
            argv += [elf]
        argv += ["-ex", "set confirm off",
                 "-ex", "set pagination off",
                 # remotetimeout must cover the ~10 s of stub init that runs
                 # inside gdb's connect, since gdb is the first client.
                 "-ex", "set remotetimeout 30",
                 "-ex", "target remote 127.0.0.1:%d" % PORT,
                 # SEMC SDRAM at 0x80000000 is board-level and absent from the
                 # stub's device map; the map is advisory, so turn the refusal
                 # off or every framebuffer read fails.
                 "-ex", "set mem inaccessible-by-default off"]
        for c in cmds:
            argv += ["-ex", c]
        argv += ["-ex", "detach", "-ex", "quit"]
        return subprocess.run(argv, capture_output=True, text=True,
                              timeout=timeout)
    finally:
        log.close()
        srv.terminate()
        _kill_daemons()


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-c", "--command", action="append", default=[],
                    help="a gdb command; repeatable")
    ap.add_argument("--file", help="file of gdb commands, one per line")
    ap.add_argument("--elf", help="ELF for symbols (omit for raw addresses only)")
    ap.add_argument("--health", action="store_true",
                    help="prepend the standard DHCSR + fault-state block")
    ap.add_argument("--timeout", type=int, default=300)
    a = ap.parse_args()

    cmds = []
    if a.health:
        cmds += ["echo \\n== DHCSR (17 S_HALT / 19 S_LOCKUP / 24 S_RETIRE_ST) ==\\n",
                 "x/1xw 0xE000EDF0",
                 "echo == ICSR / VTOR ==\\n", "x/1xw 0xE000ED04", "x/1xw 0xE000ED08",
                 "echo == SHCSR CFSR HFSR DFSR MMFAR BFAR ==\\n",
                 "x/1xw 0xE000ED24", "x/1xw 0xE000ED28", "x/1xw 0xE000ED2C",
                 "x/1xw 0xE000ED30", "x/1xw 0xE000ED34", "x/1xw 0xE000ED38",
                 "echo == DEMCR (vector catch) ==\\n", "x/1xw 0xE000EDFC"]
    cmds += list(a.command)
    if a.file:
        for line in open(a.file):
            line = line.rstrip("\n")
            if line.strip() and not line.lstrip().startswith("#"):
                cmds.append(line)
    if not cmds:
        ap.error("nothing to do: pass --health, -c or --file")

    r = run(cmds, a.elf, "/tmp/rt1170-swdprobe.gdbserver.log", a.timeout)
    if r is None:
        sys.exit(2)
    sys.stdout.write(r.stdout)
    if r.stderr.strip():
        sys.stderr.write("--- gdb stderr ---\n" + r.stderr)
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
