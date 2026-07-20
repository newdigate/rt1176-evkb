#!/usr/bin/env python3
import sys

def main():
    text = open(sys.argv[1], errors="replace").read() if len(sys.argv) > 1 else ""
    missing = []
    if "MSC_BLOCK_BEGIN" not in text: missing.append("MSC_BLOCK_BEGIN")
    # connect with a non-zero VID (guards a 0000:0000 phantom)
    connect_ok = any(
        line.startswith("MSC_CONNECT=") and line.split("=", 1)[1].split(":")[0] not in ("", "0000")
        for line in text.splitlines())
    if not connect_ok: missing.append("MSC_CONNECT=<nonzero vid>")
    if "MSC_CAP=" not in text: missing.append("MSC_CAP=")
    if "MSC_BLOCK_WRITE=PASS" not in text: missing.append("MSC_BLOCK_WRITE=PASS")
    if "MSC_BLOCK_READ=PASS" not in text: missing.append("MSC_BLOCK_READ=PASS")
    if missing:
        print("USB_MSC_BLOCK=FAIL missing: " + ", ".join(missing)); sys.exit(1)
    print("USB_MSC_BLOCK=PASS"); sys.exit(0)

if __name__ == "__main__":
    main()
