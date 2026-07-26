#!/usr/bin/env bash
# run-all-qemu-gates.sh — run every QEMU gate under examples/ and summarise.
#
# Each example owns a ./run_qemu.sh that boots its image on the custom
# mimxrt1170-evk QEMU machine and asserts the expected UART tokens. This runs
# them all, prints one line per gate, and exits non-zero if any gate failed —
# so it drops straight into CI or a pre-push check.
#
# Things this gets right, per the repo's gate contract:
#
#  * Gates are executed DIRECTLY ("$gate"), never `sh run_qemu.sh`. Each gate
#    re-execs itself under gtimeout via tools/gate-lib.sh (gate_init), which
#    only works when the shebang runs. A non-executable gate is reported as an
#    error rather than silently downgraded to `sh`.
#  * GATE_GUARDED is cleared for each gate so that self-guard always re-arms.
#    (If this runner were itself invoked from inside a guarded gate, an
#    inherited GATE_GUARDED would disable every child's hang backstop.)
#  * GATE_TIMEOUT is exported so a wedged gate cannot hang the whole sweep.
#  * Gates do NOT build — they assume build/<name>.elf exists. A missing ELF is
#    reported as SKIP, not as a confusing gate failure.
#
# Serial by default ON PURPOSE: the gates are wall-clock timed (fixed `sleep`s
# while QEMU boots), so CPU contention from parallel runs can cause spurious
# failures. -j is available when you want speed and can tolerate that.
#
# Usage:
#   tools/run-all-qemu-gates.sh [options] [pattern...]
#
# Options:
#   -j N        run N gates in parallel (default 1 = serial; see note above)
#   -t SECS     per-gate timeout, exported as GATE_TIMEOUT (default 120)
#   -l          list the matching gates and exit
#   -x          fail fast: stop at the first failing gate
#   -q          quiet: only the summary and failing-gate output
#   -h          this help
#
# Patterns (optional) are matched as substrings against "<category>/<name>",
# e.g.  run-all-qemu-gates.sh dualcore        # just the dual-core gates
#       run-all-qemu-gates.sh wire spi        # anything matching wire OR spi
#
# Exit status: 0 = every selected gate passed (skips are not failures),
#              1 = at least one gate failed, 2 = usage/setup error.

set -uo pipefail   # deliberately NOT -e: a failing gate is data, not a crash.

REPO=$(cd "$(dirname "$0")/.." && pwd)
JOBS=1
GATE_TIMEOUT_SECS=120
LIST_ONLY=0
FAIL_FAST=0
QUIET=0
PATTERNS=""

usage() { sed -n '2,45p' "$0" | sed 's/^# \{0,1\}//'; }

while getopts ":j:t:lxqh" opt; do
    case "$opt" in
        j) JOBS=$OPTARG ;;
        t) GATE_TIMEOUT_SECS=$OPTARG ;;
        l) LIST_ONLY=1 ;;
        x) FAIL_FAST=1 ;;
        q) QUIET=1 ;;
        h) usage; exit 0 ;;
        :) echo "error: -$OPTARG needs an argument" >&2; exit 2 ;;
        \?) echo "error: unknown option -$OPTARG (try -h)" >&2; exit 2 ;;
    esac
done
shift $((OPTIND - 1))
for p in "$@"; do PATTERNS="$PATTERNS$p"$'\n'; done

case "$JOBS" in ''|*[!0-9]*|0) echo "error: -j needs a positive integer" >&2; exit 2 ;; esac
case "$GATE_TIMEOUT_SECS" in ''|*[!0-9]*|0) echo "error: -t needs a positive integer" >&2; exit 2 ;; esac

# --- colours (only when writing to a terminal) ------------------------------
if [ -t 1 ]; then
    C_PASS=$'\033[32m'; C_FAIL=$'\033[31m'; C_SKIP=$'\033[33m'
    C_DIM=$'\033[2m';   C_BOLD=$'\033[1m';  C_OFF=$'\033[0m'
else
    C_PASS=; C_FAIL=; C_SKIP=; C_DIM=; C_BOLD=; C_OFF=
fi

# --- prerequisites ----------------------------------------------------------
command -v gtimeout >/dev/null 2>&1 || {
    echo "error: gtimeout not found (brew install coreutils) — gate-lib.sh needs it" >&2; exit 2; }
REAL_QEMU="${REAL_QEMU:-$HOME/Development/qemu2/build/qemu-system-arm}"
[ -x "$REAL_QEMU" ] || {
    echo "error: QEMU not found/executable at $REAL_QEMU" >&2
    echo "       build gitlab.com/Newdigate/qemu-rt1170, or set REAL_QEMU=<path>" >&2; exit 2; }

# --- gate discovery ---------------------------------------------------------
# "<category>/<name>\t<path>" per line, sorted; filtered by any patterns given.
matches() {                                   # $1 = "category/name"
    [ -z "$PATTERNS" ] && return 0
    _target=$1
    _found=1
    # Here-doc (not a pipe) so the loop runs in THIS shell and _found survives.
    while IFS= read -r pat; do
        [ -z "$pat" ] && continue
        case "$_target" in *"$pat"*) _found=0 ;; esac
    done <<EOF
$PATTERNS
EOF
    return $_found
}

GATES=""
NGATES=0
while IFS= read -r gate; do
    [ -n "$gate" ] || continue
    dir=$(dirname "$gate")
    id=${dir#"$REPO"/examples/}
    matches "$id" || continue
    GATES="$GATES$id	$gate"$'\n'
    NGATES=$((NGATES + 1))
done <<EOF
$(find "$REPO/examples" -name run_qemu.sh -type f | LC_ALL=C sort)
EOF

if [ "$NGATES" -eq 0 ]; then
    echo "no gates matched${PATTERNS:+ the given pattern(s)}" >&2
    exit 2
fi

if [ "$LIST_ONLY" -eq 1 ]; then
    printf '%s' "$GATES" | while IFS=$'\t' read -r id path; do
        [ -n "$id" ] && echo "$id"
    done
    echo "${C_DIM}($NGATES gate(s))${C_OFF}"
    exit 0
fi

# --- run one gate -----------------------------------------------------------
# Writes "<status> <seconds>" to $RESULT_DIR/<slug>.result and the gate's own
# stdout+stderr to <slug>.log. Status is PASS | FAIL | SKIP | ERROR.
run_gate() {
    _id=$1; _path=$2; _slug=$3
    _dir=$(dirname "$_path")
    _log="$RESULT_DIR/$_slug.log"
    _start=$(date +%s)

    if [ ! -x "$_path" ]; then
        # Never fall back to `sh`: gate_init's `exec gtimeout "$0"` needs the
        # shebang, and CLAUDE.md calls this out explicitly.
        echo "gate is not executable: $_path (chmod +x it)" > "$_log"
        echo "ERROR 0" > "$RESULT_DIR/$_slug.result"; return
    fi
    if ! ls "$_dir"/build/*.elf >/dev/null 2>&1; then
        echo "no build/*.elf in $_dir — build the example first:" > "$_log"
        echo "  cd $_dir && cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake && cmake --build build" >> "$_log"
        echo "SKIP 0" > "$RESULT_DIR/$_slug.result"; return
    fi

    # GATE_GUARDED cleared -> the gate arms its own gtimeout backstop.
    ( unset GATE_GUARDED
      export GATE_TIMEOUT="$GATE_TIMEOUT_SECS"
      cd "$_dir" && "$_path" ) >"$_log" 2>&1
    _rc=$?
    _elapsed=$(( $(date +%s) - _start ))

    if [ "$_rc" -eq 0 ]; then
        echo "PASS $_elapsed" > "$RESULT_DIR/$_slug.result"
    else
        echo "FAIL $_elapsed" > "$RESULT_DIR/$_slug.result"
        echo "[exit status $_rc]" >> "$_log"
    fi
}

RESULT_DIR=$(mktemp -d "${TMPDIR:-/tmp}/qemu-gates.XXXXXX") || exit 2
trap 'rm -rf "$RESULT_DIR"' EXIT INT TERM HUP

[ "$QUIET" -eq 1 ] || {
    printf '%sRunning %d QEMU gate(s)%s' "$C_BOLD" "$NGATES" "$C_OFF"
    [ "$JOBS" -gt 1 ] && printf ' %s(-j %d — timing-sensitive gates may flake)%s' "$C_DIM" "$JOBS" "$C_OFF"
    printf '  %stimeout %ss each%s\n\n' "$C_DIM" "$GATE_TIMEOUT_SECS" "$C_OFF"
}

# --- dispatch ---------------------------------------------------------------
report_one() {                                # $1 = id, $2 = slug
    read -r _st _secs < "$RESULT_DIR/$2.result"
    case "$_st" in
        PASS)  [ "$QUIET" -eq 1 ] || printf '%s  PASS%s  %-46s %s%ss%s\n' "$C_PASS" "$C_OFF" "$1" "$C_DIM" "$_secs" "$C_OFF" ;;
        SKIP)  [ "$QUIET" -eq 1 ] || printf '%s  SKIP%s  %-46s %s(not built)%s\n' "$C_SKIP" "$C_OFF" "$1" "$C_DIM" "$C_OFF" ;;
        ERROR) printf '%s ERROR%s  %-46s\n' "$C_FAIL" "$C_OFF" "$1" ;;
        *)     printf '%s  FAIL%s  %-46s %s%ss%s\n' "$C_FAIL" "$C_OFF" "$1" "$C_DIM" "$_secs" "$C_OFF" ;;
    esac
}

running=0
printf '%s' "$GATES" | {
while IFS=$'\t' read -r id path; do
    [ -n "$id" ] || continue
    slug=$(printf '%s' "$id" | tr '/' '_')
    echo "$id	$slug" >> "$RESULT_DIR/order"

    if [ "$JOBS" -eq 1 ]; then
        run_gate "$id" "$path" "$slug"
        report_one "$id" "$slug"
        if [ "$FAIL_FAST" -eq 1 ]; then
            read -r st _ < "$RESULT_DIR/$slug.result"
            case "$st" in FAIL|ERROR) echo "$id" > "$RESULT_DIR/stopped"; break ;; esac
        fi
    else
        run_gate "$id" "$path" "$slug" &
        running=$((running + 1))
        if [ "$running" -ge "$JOBS" ]; then wait -n 2>/dev/null || wait; running=$((running - 1)); fi
    fi
done
wait
}

# Parallel mode reports after the fact, in discovery order.
if [ "$JOBS" -gt 1 ]; then
    while IFS=$'\t' read -r id slug; do
        [ -f "$RESULT_DIR/$slug.result" ] && report_one "$id" "$slug"
    done < "$RESULT_DIR/order"
fi

# --- summary ----------------------------------------------------------------
pass=0; fail=0; skip=0; err=0; failed_ids=""
while IFS=$'\t' read -r id slug; do
    [ -f "$RESULT_DIR/$slug.result" ] || continue
    read -r st _ < "$RESULT_DIR/$slug.result"
    case "$st" in
        PASS)  pass=$((pass + 1)) ;;
        SKIP)  skip=$((skip + 1)) ;;
        ERROR) err=$((err + 1));  failed_ids="$failed_ids$id	$slug"$'\n' ;;
        *)     fail=$((fail + 1)); failed_ids="$failed_ids$id	$slug"$'\n' ;;
    esac
done < "$RESULT_DIR/order"

# Failing gates: show their captured output — that is where the FAIL: line is.
if [ -n "$failed_ids" ]; then
    printf '\n%s──── failing gate output ────%s\n' "$C_BOLD" "$C_OFF"
    printf '%s' "$failed_ids" | while IFS=$'\t' read -r id slug; do
        [ -n "$id" ] || continue
        printf '\n%s%s%s\n' "$C_FAIL$C_BOLD" "$id" "$C_OFF"
        sed 's/^/    /' "$RESULT_DIR/$slug.log" | tail -30
    done
fi

printf '\n%s' "$C_BOLD"
printf 'gates: %d passed' "$pass"
[ "$fail" -gt 0 ] && printf ', %d failed' "$fail"
[ "$err"  -gt 0 ] && printf ', %d error' "$err"
[ "$skip" -gt 0 ] && printf ', %d skipped (not built)' "$skip"
printf '%s\n' "$C_OFF"

[ -f "$RESULT_DIR/stopped" ] && printf '%sstopped early (-x) at %s%s\n' "$C_DIM" "$(cat "$RESULT_DIR/stopped")" "$C_OFF"

if [ $((fail + err)) -gt 0 ]; then exit 1; fi
exit 0
