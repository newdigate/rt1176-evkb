#!/bin/sh
# build-bench-configs.sh — configure and BUILD every declared bench configuration.
#
# Gates do not build, and until NEW-45 nothing built a BENCH directory either:
# display/acid_box's M2_BT_OUT builds sat broken for two days in September 2026
# before an unrelated workstream happened to rebuild them.  An example declares
# its bench configurations in a `bench` sidecar, one per line, `<name> <flags>`:
#
#     # examples/display/acid_box/bench
#     bt      -DM2_BT_OUT=ON
#     bench   -DM2_BT_OUT=ON -DACIDBOX_LOOPSTAT=ON
#
# Usage: build-bench-configs.sh [-n] [<pattern>]
#   -n         also nm-diff each owned dir's ITCM symbol set and .text.itcm size
#              against the human's build-<name>, where one exists
#   <pattern>  substring of the example path, e.g. acid_box
#
# ★ THE TOOL BUILDS INTO DIRECTORIES IT OWNS -- build-benchcheck-<name> -- and
#   never touches a human's bench directory.  build-bench, -pre and -post carry
#   M2RADIO_IW416_BT_FW pointing at the real firmware image (131,840 bytes,
#   supplied as a .bin.inc C array); a tool
#   that reconfigured one from the declared flags alone would silently strip it
#   and the bench would run the 1 KB synthetic image instead.  That is the
#   2026-08-27 red inverted (there, a bench-configured dir made its own gate
#   fail).  build-bench-configs.test.sh asserts the tool never names one.
#
# ★ A configuration counts as built only if an .elf actually appeared.  "cmake
#   said 0" is not the same claim, and this tool exists precisely because a
#   build that silently does nothing reads as green.  Every "measured nothing"
#   path below is therefore a FAILURE, not a quiet pass: no pairs compared, no
#   symbols read, a pattern that selected nothing.
#
# Exit 0 = BENCH-BUILDS: PASS.  Run from anywhere.  NEVER concurrently with the
# QEMU sweep: CLAUDE.md records a sweep invalidated by a concurrent licence
# audit, and this is heavier than that.
set -e
# ★ set -f for the whole script: `set -- $line` below splits a sidecar line into
# words, and without it a flag containing * or ? would glob against the cwd.
# Nothing here wants pathname expansion.
set -f
NL='
'
REPO=$(cd "$(dirname "$0")/.." && pwd)
CMAKE=${BENCH_CMAKE:-cmake}          # the seam build-bench-configs.test.sh drives
TOOLBIN=${ARM_TOOLCHAIN_BIN:-/Applications/ARM_10/bin}
TOOLCHAIN="$REPO/toolchain/rt1170-evkb.toolchain.cmake"

# A here-doc, not sed on $0: a line-number range goes stale the moment the header
# is edited, and it had -- `-h` printed the sidecar example and no option list.
usage() {
    cat <<'USAGE'
Usage: build-bench-configs.sh [-n] [<pattern>]
  -n         also nm-diff each owned dir's ITCM symbol set and .text.itcm size
             against the human's build-<name>, where one exists
  <pattern>  substring of the example path, e.g. acid_box
USAGE
}

NMDIFF=0
PATTERN=""
while [ $# -gt 0 ]; do
    case "$1" in
        -n)        NMDIFF=1 ;;
        -h|--help) usage; exit 0 ;;
        # ★ Reject unknown options rather than treating them as a pattern: a
        # mistyped flag that silently becomes a filter matching nothing would
        # print PASS, which is the failure mode this whole tool is about.
        -*)        echo "error: unknown option '$1'" >&2; usage >&2; exit 2 ;;
        *)         [ -z "$PATTERN" ] || { echo "error: only one pattern allowed" >&2; exit 2; }
                   PATTERN=$1 ;;
    esac
    shift
done

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
: > "$WORK/fails"; n=0; found=0; selected=0

IFS=$NL
# ★ PRUNE BY DIRECTORY, NOT BY PATH SUBSTRING.  `-not -path '*/build*'` matches the
# WHOLE path, so a checkout living under any directory whose name starts with
# "build" -- ~/buildfarm/evkb, ~/builds/evkb -- prunes EVERYTHING, finds no
# sidecars and prints BENCH-BUILDS: PASS.  Measured 2026-09-11.  That is this
# tool's own disease (green while measuring nothing) in the tool itself, and no
# test arm could see it: every arm builds its throwaway tree under a path with no
# "build" component.  This is the idiom the sibling runner already uses
# (run-all-qemu-gates.sh) -- prune DIRECTORIES named build*, then print files.
for sidecar in $(find "$REPO/examples" -type d -name 'build*' -prune -o -type f -name bench -print | sort); do
    IFS=$NL
    dir=$(dirname "$sidecar"); rel=${dir#"$REPO"/}
    found=$((found+1))
    case "$rel" in *"$PATTERN"*) ;; *) continue ;; esac
    selected=$((selected+1))
    # tr -d '\r' so a CRLF sidecar does not leak a carriage return into the last
    # flag -- the sibling `boards` parser strips it for the same reason.
    body=$(grep -v '^[[:space:]]*#' "$sidecar" | tr -d '\r' | grep -v '^[[:space:]]*$' || true)
    if [ -z "$body" ]; then
        echo "error: $rel/bench declares no configuration (empty after stripping comments)"
        echo "$rel/bench" >> "$WORK/fails"; continue
    fi
    # A here-doc feeds the loop on stdin so it runs in THIS shell, not a pipeline
    # subshell -- otherwise every failure recorded below would be discarded.
    while IFS= read -r line; do
        [ -n "$line" ] || continue
        # ★ Split on whitespace into positional parameters rather than with
        # ${line%% *}: that idiom yields an EMPTY name for an indented line and
        # then builds `build-benchcheck-` while handing cmake the real name as a
        # bare positional argument -- measured, and it reported OK.  This form
        # also takes tabs as separators, which the sibling `boards` parser does.
        IFS=' 	'; set -- $line; IFS=$NL
        name=$1; shift
        if [ $# -eq 0 ]; then
            echo "error: $rel/bench: '$name' declares no cmake flags"
            echo "$rel[$name]" >> "$WORK/fails"; continue
        fi
        n=$((n+1))
        bdir="$dir/build-benchcheck-$name"
        printf '%-44s ' "$rel[$name]"
        # ★★ ALWAYS CONFIGURE FROM SCRATCH.  A CMake cache RETAINS every -D ever
        # passed, so a flag REMOVED from a `bench` line would stay in effect in
        # this directory forever and the tool would be measuring a configuration
        # nobody declared -- the one thing it exists to rule out.  Same trap
        # CLAUDE.md records: "editing a set(... CACHE ...) DEFAULT does not change
        # an existing build directory ... Check the symbol size, not the source."
        # Measured 2026-09-11: a one-off -DACIDBOX_ITCM_MIN_HEADROOM=65536, passed
        # once to demonstrate the headroom assert going red, survived in the cache
        # and kept the build FAILED on an otherwise unmodified tree.
        rm -rf "$bdir"
        # ★ -S IS LOAD-BEARING.  Without it cmake takes the source directory from
        # the CWD, and since 912c8d1 the repo root holds a CMakeLists.txt --
        # project(rt1170_evkb_root NONE), whose `all` target is empty -- so a run
        # from the repo root CONFIGURES THE ROOT PROJECT into this example's build
        # dir, builds nothing, exits 0 and reports OK.  Measured 2026-09-11.
        # Before 912c8d1 the same omission was LOUD, which is why it survived
        # review: the failure mode is newer than the idiom.
        if "$CMAKE" -S "$dir" -B "$bdir" -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" "$@" > "$WORK/out" 2>&1 \
           && "$CMAKE" --build "$bdir" >> "$WORK/out" 2>&1; then
            if [ -n "$(find "$bdir" -maxdepth 1 -name '*.elf' -print -quit)" ]; then
                echo OK
            else
                echo FAILED
                echo "    cmake exited 0 but produced no .elf -- the build claimed success and made nothing"
                echo "$rel[$name]" >> "$WORK/fails"
            fi
        else
            echo FAILED
            # Show the lines that NAME the fault, not the last line.  make's
            # generic "Error 2" sits nine lines below ld's actual diagnosis, and
            # CMake's own failures say "CMake Error at", which matches neither
            # ld: nor error: -- measured on the headroom-assert RED demo.
            _why=$(grep -E 'ld:|error:|CMake Error' "$WORK/out" | head -3)
            [ -n "$_why" ] || _why=$(tail -3 "$WORK/out")
            printf '%s\n' "$_why" | sed 's/^/    /'
            echo "$rel[$name]" >> "$WORK/fails"
        fi
    done <<EOF
$body
EOF
done
IFS=$NL

# ★ A pattern that selects nothing is NOT the same claim as "nothing is declared
# anywhere", and only the second deserves a pass.  A typo'd pattern printing
# BENCH-BUILDS: PASS is the exact disease this tool treats.
if [ -n "$PATTERN" ] && [ "$selected" -eq 0 ] && [ "$found" -gt 0 ]; then
    echo "error: pattern '$PATTERN' selected none of the $found example(s) with a bench sidecar"
    echo "pattern:$PATTERN" >> "$WORK/fails"
fi

# ITCM is 0x00000000-0x000FFFFF; flash is 0x30000000+, RAM 0x20000000+.  Absolute
# symbols (nm type A) carry a VALUE, not an address -- _flashimagelen's value is
# the image length and drifts into this window once an image reaches 1 MB, which
# would read as a spurious symbol-set difference pointing at the wrong cause.
itcm_syms() {
    "$TOOLBIN/arm-none-eabi-nm" --defined-only "$1" \
      | awk '$2 != "A" && $2 != "a" && $1 ~ /^000[0-9a-f]{5}$/ {print $3}' | sort
}
itcm_size() { "$TOOLBIN/arm-none-eabi-size" -A "$1" | awk '/^\.text\.itcm/ {print $2}'; }

if [ "$NMDIFF" -eq 1 ]; then
    pairs=0
    for owned in $(find "$REPO/examples" -maxdepth 3 -type d -name 'build-benchcheck-*' | sort); do
        rel_o=${owned#"$REPO"/}
        # ★ Honour the pattern here too.  Without this the loop globs EVERY owned
        # directory in the tree, including leftovers from an earlier run of a
        # different example, so a scoped invocation could go red for a
        # configuration it was told not to touch -- measured 2026-09-11.
        case "$rel_o" in *"$PATTERN"*) ;; *) continue ;; esac
        # NOTE: pairs are discovered by glob, not from the declared set, so a
        # RENAMED or DELETED bench entry leaves a gitignored build-benchcheck-<old>
        # that keeps being diffed against a stale build-<old>.  The DIFFERS text
        # names staleness as a cause; delete the orphan when a name changes.
        human=$(printf '%s' "$owned" | sed 's/build-benchcheck-/build-/')
        [ -d "$human" ] || continue
        a=$(find "$owned" -maxdepth 1 -name '*.elf' | head -1)
        b=$(find "$human" -maxdepth 1 -name '*.elf' | head -1)
        [ -n "$a" ] && [ -n "$b" ] || continue
        pairs=$((pairs+1))
        itcm_syms "$a" > "$WORK/owned.syms" 2>/dev/null || true
        itcm_syms "$b" > "$WORK/human.syms" 2>/dev/null || true
        # ★ Two empty files compare EQUAL.  A missing arm-none-eabi-nm, an
        # unreadable ELF or a stripped image would otherwise print nm-diff OK
        # having read nothing at all -- measured with ARM_TOOLCHAIN_BIN pointed
        # at a nonexistent directory, which reported OK and PASS.
        if [ ! -s "$WORK/owned.syms" ] || [ ! -s "$WORK/human.syms" ]; then
            echo "nm-diff BROKEN: read no ITCM symbols from ${owned##*/} or ${human##*/}"
            echo "  (is $TOOLBIN/arm-none-eabi-nm present and are both ELFs readable?)"
            echo "${owned##*/}:nm-unreadable" >> "$WORK/fails"; continue
        fi
        sa=$(itcm_size "$a"); sb=$(itcm_size "$b")
        # ★ Size as well as names.  CLAUDE.md's own precedent for this check
        # (2026-09-08, the acid_box ITCM wildcard swap) is "an IDENTICAL ITCM
        # symbol set AND .text.itcm size, nm-diffed, not eyeballed" -- names
        # alone would accept two builds whose ITCM footprints differ.
        if cmp -s "$WORK/owned.syms" "$WORK/human.syms" && [ -n "$sa" ] && [ "$sa" = "$sb" ]; then
            echo "nm-diff OK: ${owned##*/} == ${human##*/} (ITCM symbol set + .text.itcm $sa)"
        else
            echo "nm-diff DIFFERS: ${owned##*/} vs ${human##*/} (.text.itcm $sa vs $sb)"
            echo "  EITHER the proxy is not equivalent, OR the hand-configured ${human##*/} is stale:"
            echo "  the tool wipes its own directory every run and never touches that one."
            diff "$WORK/human.syms" "$WORK/owned.syms" | head -20 | sed 's/^/    /'
            echo "${owned##*/}:nm" >> "$WORK/fails"
        fi
    done
    echo "nm-diff: $pairs pair(s) compared"
    # ★ Zero pairs must be SAID, not merely implied -- silence read as success is
    # what this tool is for.  But it must not FAIL: a machine that has never
    # benched this example has no hand-made build-<name> to compare against, and
    # every declared configuration may still have built perfectly.  Making it
    # fatal made the root `bench_check` target red for everyone but the one bench
    # machine, which is a guard that gets switched off.  An unreadable pair is a
    # different claim and is still fatal, above (nm-diff BROKEN).
    if [ "$pairs" -eq 0 ]; then
        echo "  no hand-made build-<name> found beside any build-benchcheck-*, so -n verified nothing"
        echo "  (normal on a machine that has never benched these examples)"
    fi
fi

echo "bench: $n configuration(s)"
if [ -s "$WORK/fails" ]; then
    echo "BENCH-BUILDS: FAIL"; sed 's/^/  /' "$WORK/fails"; exit 1
fi
echo "BENCH-BUILDS: PASS"
