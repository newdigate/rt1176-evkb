#!/usr/bin/env python3
import sys

def main():
    text = open(sys.argv[1], errors="replace").read() if len(sys.argv) > 1 else ""
    missing = []
    for m in ("MSC_FS_BEGIN", "MSC_MOUNT=", "MSC_FS_WRITE=PASS",
              "MSC_FS_READ=PASS", "MSC_FS_DIR=PASS"):
        if m not in text:
            missing.append(m)
    if missing:
        print("USB_MSC_FS=FAIL missing: " + ", ".join(missing)); sys.exit(1)
    print("USB_MSC_FS=PASS"); sys.exit(0)

if __name__ == "__main__":
    main()
