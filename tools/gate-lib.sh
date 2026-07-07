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
