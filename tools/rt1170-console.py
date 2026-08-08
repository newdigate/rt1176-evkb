#!/usr/bin/env python3
"""Live serial console for an EVKB debug VCOM.

Streams the board's UART at 115200 to your terminal. Ctrl-C to quit.
Survives power-cycles: if the port disappears (USB re-enumerates on reset),
it waits and reconnects, so you can leave it running and tap RESET.

Works for both boards in this tree, but note they differ:
  MIMXRT1170-EVKB  MCU-Link VCOM;  POR button is SW4.
  MIMXRT1060-EVKB  DAPLink VCOM;   it has NO SW4 -- check the board's own
                   silkscreen for its reset button before hunting for one.
                   Firmware must print to Serial6 (LPUART1) to reach this
                   VCOM; Serial1 is LPUART6 and only reaches header pins
                   D0/D1. See CLAUDE.md.

Usage: rt1170-console.py [PORT] [BAUD]
Defaults: /dev/cu.usbmodem5DQ2DDHVWO5EI3  115200  (the 1170's MCU-Link)
"""
import sys, time
try:
    import serial
except ImportError:
    sys.exit("pyserial not installed: pip install pyserial")

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbmodem5DQ2DDHVWO5EI3"
BAUD = int(sys.argv[2]) if len(sys.argv) > 2 else 115200

print(f"[console] {PORT} @ {BAUD} — Ctrl-C to quit. Press the board's RESET "
      f"(SW4 on the 1170-EVKB; the 1060-EVKB has no SW4) to see the boot banner.\n",
      file=sys.stderr)
try:
    while True:
        try:
            ser = serial.Serial(PORT, BAUD, timeout=0.2)
        except (OSError, serial.SerialException):
            time.sleep(0.5)          # port not present (e.g. mid power-cycle); retry
            continue
        try:
            while True:
                data = ser.read(256)
                if data:
                    sys.stdout.buffer.write(data)
                    sys.stdout.buffer.flush()
        except (OSError, serial.SerialException):
            try: ser.close()
            except Exception: pass
            print("\n[console] port dropped, reconnecting…", file=sys.stderr)
            time.sleep(0.5)
except KeyboardInterrupt:
    print("\n[console] bye", file=sys.stderr)
