#!/bin/sh
set -e
DIR=$(cd "$(dirname "$0")" && pwd); OUT=$(mktemp -d); trap 'rm -rf "$OUT"' EXIT
cat > "$OUT/in.log" <<'EOF'
noise line, ignored
acl_trace dir=out h=0x0001 t=1000 hex=02 00 01 00 0A 01 02 00 02 00
acl_trace dir=in h=0x0001 t=2000 hex=02 00 01 00 0B 01 06 00 02 00 00 00
EOF
python3 "$DIR/acl-trace-to-btsnoop.py" "$OUT/in.log" "$OUT/out.btsnoop"
python3 - "$OUT/out.btsnoop" <<'PY'
import sys, struct
d = open(sys.argv[1], "rb").read()
assert d[:8] == b"btsnoop\x00", "bad magic"
ver, dl = struct.unpack(">II", d[8:16]); assert ver == 1 and dl == 1001, (ver, dl)
off = 16; n = 0
while off < len(d):
    ol, il, fl, drops, ts = struct.unpack(">IIIIq", d[off:off+24]); off += 24
    acl = d[off:off+il]; off += il; n += 1
    assert acl[0] == 0x02, "H4 type must be ACL (0x02)"
    hf = acl[1] | (acl[2] << 8); assert (hf & 0x0FFF) == 0x0001, "handle"
    aclLen = acl[3] | (acl[4] << 8); assert aclLen == len(acl) - 5, "ACL length field"
    if n == 1: assert fl == 0, "first record is sent (dir=out -> flag 0)"
    if n == 2: assert fl == 1, "second record is received (dir=in -> flag 1)"
assert n == 2, ("record count", n)
print("BTSNOOP-CONVERTER-TEST: PASS")
PY
