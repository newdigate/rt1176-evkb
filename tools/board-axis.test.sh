#!/bin/sh
# Proves the sweep runner honours a per-example `boards` sidecar.
set -e
REPO=$(cd "$(dirname "$0")/.." && pwd)
fails=0
check() { # check <description> <expected-substring> <actual>
    case "$3" in *"$2"*) ;; *) echo "FAIL: $1 (wanted '$2')"; fails=$((fails+1)) ;; esac
}

# serial_test has no `boards` file yet -> rt1176 only, listed with the prefix.
out=$("$REPO/tools/run-all-qemu-gates.sh" -l serial/serial_test)
check "default board prefix" "rt1176:serial/serial_test" "$out"
case "$out" in *rt1062*) echo "FAIL: rt1062 listed with no sidecar"; fails=$((fails+1));; esac

# Ids gained a prefix but matching is still a plain substring, so every pattern
# written before the board axis existed must select exactly what it always did.
out=$("$REPO/tools/run-all-qemu-gates.sh" -l usb)
check "pre-board pattern still selects" "rt1176:usb/" "$out"

# Result/log files are named for the id with '/' -> '_' (the board's ':' is
# kept). Two ids collapsing to one slug would make one gate silently overwrite
# another's result and the sweep would report the wrong thing -- so assert the
# id -> slug mapping is injective across the whole tree.
ids=$("$REPO/tools/run-all-qemu-gates.sh" -l | grep ':')
n=$(printf '%s\n' "$ids" | grep -c .)
u=$(printf '%s\n' "$ids" | tr '/' '_' | sort -u | grep -c .)
[ "$n" -eq "$u" ] || { echo "FAIL: slug collision ($n ids -> $u slugs)"; fails=$((fails+1)); }

[ "$fails" -eq 0 ] && echo "board-axis tests PASS" || { echo "$fails failure(s)"; exit 1; }
