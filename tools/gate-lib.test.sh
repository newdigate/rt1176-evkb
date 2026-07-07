#!/bin/sh
# Unit tests for gate-lib.sh. No QEMU needed — uses `sleep` as a process stand-in.
# Usage: sh evkb/tools/gate-lib.test.sh   (prints PASS:/FAIL: per case; exits 1 on any FAIL)
set -u
LIB="$(cd "$(dirname "$0")" && pwd)/gate-lib.sh"
FAILED=0
report() { # <name> <0-pass|1-fail>
    if [ "$2" -eq 0 ]; then echo "PASS: $1"; else echo "FAIL: $1"; FAILED=1; fi
}

# gate_tmp registers files; gate_cleanup removes them.
test_tmp() {
    ( . "$LIB"
      f1=$(mktemp); f2=$(mktemp)
      gate_tmp "$f1" "$f2"
      gate_cleanup
      [ ! -e "$f1" ] && [ ! -e "$f2" ] )
}
test_tmp; report test_tmp $?

# gate_pid registers a process; gate_cleanup kills it via the EXPLICIT-kill path.
# The process is spawned detached (child of a transient `sh -c`, reparented to init)
# so it is NOT in this shell's `jobs -p` table — only `kill $GATE_PIDS` can reap it.
# This gives the gate_pid path genuine, mutation-catching coverage.
test_pid() {
    ( . "$LIB"
      p=$(sh -c 'sleep 60 >/dev/null 2>&1 & echo $!')
      gate_pid "$p"
      gate_cleanup
      sleep 1
      ! kill -0 "$p" 2>/dev/null )
}
test_pid; report test_pid $?

# Untracked background jobs are still reaped via the jobs -p backstop.
test_jobs_backstop() {
    ( . "$LIB"
      sleep 60 & p=$!          # started but NOT registered with gate_pid
      gate_cleanup
      sleep 1
      ! kill -0 $p 2>/dev/null )
}
test_jobs_backstop; report test_jobs_backstop $?

# Shared driver for the three signal-trap cases. A gate_init'd fixture (re-exec
# bypassed via GATE_GUARDED=1 so we test the trap directly) with a DETACHED tracked
# child + a registered temp is sent <signal>; we assert the child was reaped via the
# explicit-kill path, the temp removed, and the process exited with <expected_rc>
# (so a swapped/wrong exit literal in a handler is caught, not just missing cleanup).
# A separate `sleep 60 & wait` keeps the fixture alive until the signal.
_signal_trap_case() { # <signal> <expected_rc>
    _sig=$1; _erc=$2
    tf=$(mktemp); pf=$(mktemp); fx=$(mktemp)
    cat > "$fx" <<EOF
#!/bin/sh
GATE_GUARDED=1; . "$LIB"
gate_init
cp=\$(sh -c 'sleep 60 >/dev/null 2>&1 & echo \$!')
echo \$cp > "$pf"
gate_pid \$cp
gate_tmp "$tf"
: > "$fx.ready"
sleep 60 & wait
EOF
    sh "$fx" >/dev/null 2>&1 &
    gpid=$!
    i=0; while [ $i -lt 50 ] && [ ! -f "$fx.ready" ]; do sleep 0.1; i=$((i+1)); done
    child=$(cat "$pf" 2>/dev/null)
    kill -"$_sig" "$gpid" 2>/dev/null
    wait "$gpid" 2>/dev/null; rc=$?
    sleep 1
    result=0
    [ -e "$tf" ] && result=1                                          # temp not removed
    { [ -n "$child" ] && kill -0 "$child" 2>/dev/null; } && result=1  # child not reaped
    [ "$rc" -eq "$_erc" ] || result=1                                # wrong/absent code
    rm -f "$fx" "$fx.ready" "$pf" "$tf"
    return $result
}
test_trap_term() { _signal_trap_case TERM 143; }
test_trap_term; report test_trap_term $?

# All four traps are installed with the right handlers. This catches a dropped OR
# mis-wired per-signal trap (INT/TERM/HUP) that a behavioral background test can't:
# INT is SIG_IGN'd in a non-interactive shell's background child (so it can't be
# delivered to the fixture at all), and a deleted per-signal trap is masked at
# runtime by the always-present EXIT trap plus the default 128+signo death codes
# (INT 130 / TERM 143 / HUP 129). Introspecting the trap table sidesteps both and
# is fully deterministic (no signal delivery, no timing).
test_traps_installed() {
    fx=$(mktemp); td=$(mktemp)
    cat > "$fx" <<EOF
#!/bin/sh
GATE_GUARDED=1; . "$LIB"
gate_init
trap -p > "$td" 2>/dev/null || trap > "$td"
EOF
    sh "$fx" >/dev/null 2>&1
    result=0
    grep INT  "$td" | grep -q "exit 130"     || result=1
    grep TERM "$td" | grep -q "exit 143"     || result=1
    grep HUP  "$td" | grep -q "exit 129"     || result=1
    grep EXIT "$td" | grep -q "gate_cleanup" || result=1
    rm -f "$fx" "$td"
    return $result
}
test_traps_installed; report test_traps_installed $?

# gate_cleanup must be idempotent: the first fire clears the registries, so a
# second fire is a no-op. Proven by recreating the temp at the same path after the
# first cleanup and asserting the second cleanup does NOT remove it (it would if
# the GATE_TMPS reset were missing).
test_idempotent() {
    ( . "$LIB"
      f=$(mktemp); gate_tmp "$f"
      gate_cleanup
      : > "$f"
      gate_cleanup
      if [ -e "$f" ]; then rm -f "$f"; return 0; else return 1; fi )
}
test_idempotent; report test_idempotent $?

# The EXIT trap preserves the script's own exit code. NOTE: on this machine's
# /bin/sh (bash 3.2) an EXIT-trap `return N` never overrides the pre-trap exit
# status (only an explicit `exit N` in the trap would), so `return $_rc` is
# defensive cross-shell insurance. This case verifies the observable contract —
# the gate's exit code survives teardown — and catches an errant `exit` in EXIT.
test_exit_code() {
    fx=$(mktemp)
    cat > "$fx" <<EOF
#!/bin/sh
GATE_GUARDED=1; . "$LIB"
gate_init
exit 7
EOF
    sh "$fx"; rc=$?
    rm -f "$fx"
    [ "$rc" -eq 7 ]
}
test_exit_code; report test_exit_code $?

# gate_init WITHOUT GATE_GUARDED re-execs under gtimeout: a wedged runner is
# reaped at GATE_TIMEOUT, its temp removed, and it exits non-zero.
test_hang_backstop() {
    tf=$(mktemp)
    fx=$(mktemp)
    cat > "$fx" <<EOF
#!/bin/sh
. "$LIB"
GATE_TIMEOUT=2 gate_init
gate_tmp "$tf"
sleep 60 & wait
EOF
    chmod +x "$fx"          # gate_init re-execs "$0"; real gates are +x, so must this be
    start=$(date +%s)
    gtimeout 20 "$fx" > "$fx.out" 2>&1; rc=$?
    end=$(date +%s)
    elapsed=$((end - start))
    result=0
    [ "$rc" -eq 0 ] && result=1
    [ "$elapsed" -gt 12 ] && result=1
    [ -e "$tf" ] && result=1
    rm -f "$fx" "$fx.out" "$tf"
    return $result
}
test_hang_backstop; report test_hang_backstop $?

exit $FAILED
