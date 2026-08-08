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

# --- board axis --------------------------------------------------------------
# A gate never names a QEMU machine. It asks here, and the answer comes from
# $EVKB_BOARD (default rt1176). That is what stops a gate booting the wrong
# machine: with the name in 81 scripts, adding a board meant 81 chances to get
# it wrong, and a gate that boots the wrong model can still PASS vacuously.

gate_board() { echo "${EVKB_BOARD:-rt1176}"; }

# Emit the -M/-global pair for the current board. Both machines expose a
# "boot-xip" bool (fsl-imxrt1170.c / fsl-imxrt1062.c DEFINE_PROP_BOOL), so only
# the SoC type name differs. Unknown boards EXIT rather than defaulting: a typo
# in EVKB_BOARD must be a loud failure, never a silent run on the other board.
gate_qemu_machine() {
    case "$(gate_board)" in
        rt1176) echo "-M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on" ;;
        rt1062) echo "-M mimxrt1060-evk -global fsl-imxrt1062.boot-ivt=on" ;;
        *) echo "gate-lib: unknown EVKB_BOARD '$(gate_board)'" >&2; exit 2 ;;
    esac
}

# Emit the -serial chain that lands FILE on the board's Serial1 UART.
#
# QEMU binds the Nth -serial to LPUART(N+1), so WHICH slot the capture file goes
# in depends on which LPUART the core calls Serial1 -- and the two cores differ:
# LPUART1 on imxrt1176 (HardwareSerial1.cpp, 0x4007C000), LPUART6 on the Teensy
# 4 core, which follows the Teensy pin-0/1 convention. A gate that hardcodes a
# bare `-serial file:` therefore captures NOTHING on rt1062 while the firmware
# prints happily to LPUART6 -- and an empty capture is indistinguishable from
# firmware that never ran. Board-dependent, so it lives here, not in 82 scripts.
gate_serial1() {   # gate_serial1 FILE
    case "$(gate_board)" in
        rt1176) echo "-serial file:$1" ;;
        rt1062) echo "-serial null -serial null -serial null -serial null -serial null -serial file:$1" ;;
        *) echo "gate-lib: unknown EVKB_BOARD '$(gate_board)'" >&2; exit 2 ;;
    esac
}

# Build directory for the current board. rt1176 keeps the plain "build" so the
# 81 pre-existing gates, the sweep runner's SKIP probe and every documented
# `cmake -B build` line are unaffected; new boards are suffixed.
gate_build_dir() {
    case "$(gate_board)" in
        rt1176) echo "build" ;;
        rt1062) echo "build-rt1062" ;;
        *) echo "gate-lib: unknown EVKB_BOARD '$(gate_board)'" >&2; exit 2 ;;
    esac
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
