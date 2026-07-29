#!/bin/sh
# gate-lib.sh — shared lifecycle safety net for the evkb QEMU gate runners.
# Source it right after computing DIR, then call gate_init FIRST (before any
# side-effecting line: rm, mkfifo, python input-gen, launching QEMU).
# Spec: docs/superpowers/specs/2026-07-07-gate-runner-lifecycle-hardening-design.md
#
#   gate_init [timeout_secs]  arm hang backstop (gtimeout re-exec) + traps
#                             default 600s; also overridable via env GATE_TIMEOUT
#   gate_pid  PID...          register process(es) to reap on teardown
#   gate_tmp  FILE...         register temp file(s)/fifo(s) to rm on teardown
#   gate_reap PID...          end the QEMU run; safe if it already died
#   gate_require_capture F    refuse to grep a capture that isn't there

GATE_PIDS=""
GATE_TMPS=""

gate_pid() {
    for _p in "$@"; do
        [ -n "$_p" ] && GATE_PIDS="$GATE_PIDS $_p"
    done
}

gate_tmp() {
    for _f in "$@"; do
        [ -n "$_f" ] && GATE_TMPS="$GATE_TMPS $_f"
    done
}

# End the QEMU run a gate started. Use this instead of a bare `kill $P; wait $P`.
#
# The `|| true` on the KILL is the whole point, and it is load-bearing under the
# runners' `set -e`: if QEMU has already exited (bad image, instant crash, model
# assert) the kill fails, errexit fires, and the gate dies right there -- exit 1
# with NO message at all, before a single assertion runs. The gate still goes
# red, so it is never a false green, but the operator is told nothing about what
# broke. Every runner in this tree had that shape until it was swept out; see
# gate-lib.test.sh:test_reap_dead_process for the regression that pins it.
gate_reap() {
    for _p in "$@"; do
        [ -n "$_p" ] || continue
        kill "$_p" 2>/dev/null || true
        wait "$_p" 2>/dev/null || true
    done
}

# Assert a UART capture exists and is non-empty BEFORE anything greps it, and
# fail by NAME if it does not. Without this a QEMU that never opened the serial
# file surfaces as `cat: no such file` (points at nothing) or, worse, as the
# first token assertion reporting "banner missing" -- which misdiagnoses "the run
# never happened" as "the firmware regressed".
#
# `-s` (non-empty), NOT `-f`: tools/qrun creates the serial file before it execs
# QEMU, so mere existence proves qrun ran, not that QEMU emitted anything.
#
# Deliberately NOT reused for `-d guest_errors` .dbg logs: a HEALTHY run leaves
# those EMPTY, so `-s` would reject exactly the runs that should pass. A gate
# asserting a .dbg does NOT contain something wants `[ -f ... ]` and its own
# message -- do not "unify" the two checks.
gate_require_capture() {   # gate_require_capture FILE [WHAT]
    if [ -s "$1" ]; then return 0; fi
    echo "FAIL: no UART capture at $1${2:+ ($2)} -- QEMU produced no serial output (did it start?)"
    exit 1
}

gate_cleanup() {
    _rc=$?
    set +e   # trap-only: a dead registered PID must not abort us under the runner's
             # `set -e`. Intentionally not restored — gate_cleanup runs at end-of-life
             # (from a trap) or in a subshell (tests), so leaving errexit off is moot.
    [ -n "$GATE_PIDS" ] && kill $GATE_PIDS 2>/dev/null
    _bg=$(jobs -p 2>/dev/null)                      # backstop: any stray bg job of this shell
    [ -n "$_bg" ] && kill $_bg 2>/dev/null
    [ -n "$GATE_TMPS" ] && rm -f $GATE_TMPS
    GATE_PIDS=""                                     # idempotent: second fire is a no-op
    GATE_TMPS=""
    return $_rc                                      # preserve the script's exit code
}

gate_init() {
    if [ -z "${GATE_GUARDED:-}" ]; then
        export GATE_GUARDED=1
        # Runners take no CLI args, so re-exec "$0" with none. (Inside a function
        # "$@" is the function's own args, not the script's, so a runner's CLI args
        # can't be forwarded here anyway; a future arg-taking runner would capture
        # its "$@" before calling gate_init and forward it explicitly.)
        exec gtimeout --kill-after=10s "${1:-${GATE_TIMEOUT:-600}}" "$0"
    fi
    trap gate_cleanup EXIT
    trap 'gate_cleanup; exit 130' INT
    trap 'gate_cleanup; exit 143' TERM
    trap 'gate_cleanup; exit 129' HUP
}
