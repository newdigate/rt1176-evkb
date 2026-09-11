#!/bin/sh
# Usage: tools/build-bench-configs.test.sh
#
# Negative tests for build-bench-configs.sh.  A tool that only ever reports PASS
# is indistinguishable from one that looks at nothing, so every check here is a
# case where the tool MUST fail, skip, or stay out of a directory.
#
# Drives the real tool against throwaway trees with a FAKE cmake (BENCH_CMAKE)
# and a FAKE arm-none-eabi-nm/size (ARM_TOOLCHAIN_BIN), so it needs no toolchain,
# no network and no gate builds -- the license-audit and gate-vacuity suites use
# the same technique.
set -e
REPO=$(cd "$(dirname "$0")/.." && pwd)
TOOL="$REPO/tools/build-bench-configs.sh"
fails=0
check() { # check <description> <expected-substring> <actual>
    case "$3" in *"$2"*) echo "PASS: $1" ;;
                 *) echo "FAIL: $1 (wanted '$2')"; fails=$((fails+1)) ;; esac
}
nocheck() { # nocheck <description> <forbidden-substring> <actual>
    case "$3" in *"$2"*) echo "FAIL: $1 (found '$2')"; fails=$((fails+1)) ;;
                 *) echo "PASS: $1" ;; esac
}
rc_is() { # rc_is <description> <expected-rc>
    [ "$rc" -eq "$2" ] && echo "PASS: $1" || { echo "FAIL: $1 (rc=$rc, wanted $2)"; fails=$((fails+1)); }
}

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# A fake cmake that logs its argv, fails for a configuration we mark, and
# otherwise EMULATES A REAL BUILD by dropping an .elf -- unless the configuration
# is marked MAKENOTHING, which models the tool's own historical bug (a missing -S
# configured the root project, which builds nothing and succeeds).
mkdir -p "$WORK/bin"
cat > "$WORK/bin/fakecmake" <<'FAKE'
#!/bin/sh
echo "$@" >> "$FAKE_LOG"
case "$*" in *BREAKME*) echo "ld: region \`ITCM' overflowed by 140 bytes" >&2; exit 1 ;; esac
case "$1" in
  --build) d=$2; ex=$(dirname "$d")
     [ -d "$d" ] && [ ! -f "$d/.makenothing" ] && : > "$d/fake.elf"
     # The tool wipes its owned dir before configuring (by design), so a test
     # cannot seed the dir directly -- the fake build plants what the fake nm
     # will read, exactly as a real build produces the symbols nm reads.
     [ -f "$ex/OWNED_SYMS" ] && cp "$ex/OWNED_SYMS" "$d/ITCM_SYMS"
     [ -f "$ex/OWNED_SIZE" ] && cp "$ex/OWNED_SIZE" "$d/ITCM_SIZE"
     : ;;
  *) nextb=0
     for a in "$@"; do
        if [ "$nextb" = 1 ]; then mkdir -p "$a"
           case "$*" in *MAKENOTHING*) : > "$a/.makenothing" ;; esac
           nextb=0
        fi
        [ "$a" = "-B" ] && nextb=1
     done ;;
esac
exit 0
FAKE
chmod +x "$WORK/bin/fakecmake"

# Fake nm/size.  Both read a sidecar file dropped beside the .elf so a test can
# say what each build "contains" -- ITCM_SYMS and ITCM_SIZE.  A missing tool is
# modelled by pointing ARM_TOOLCHAIN_BIN somewhere empty.
mkdir -p "$WORK/arm"
cat > "$WORK/arm/arm-none-eabi-nm" <<'FAKENM'
#!/bin/sh
elf=$3; [ -n "$elf" ] || elf=$2
d=$(dirname "$elf")
[ -f "$d/ITCM_SYMS" ] || exit 0
i=0
while IFS= read -r s; do
    printf '000%05x T %s\n' "$i" "$s"; i=$((i+1))
done < "$d/ITCM_SYMS"
# an absolute symbol inside the ITCM window, which must be filtered out
printf '00051800 A _flashimagelen\n'
FAKENM
cat > "$WORK/arm/arm-none-eabi-size" <<'FAKESIZE'
#!/bin/sh
d=$(dirname "$2"); s=252336
[ -f "$d/ITCM_SIZE" ] && s=$(cat "$d/ITCM_SIZE")
echo "section size addr"
echo ".text.itcm $s 0"
FAKESIZE
chmod +x "$WORK/arm/arm-none-eabi-nm" "$WORK/arm/arm-none-eabi-size"

mktree() { # mktree <name> ; echoes a REPO-shaped root with a toolchain file
    root="$WORK/$1"; mkdir -p "$root/tools" "$root/toolchain" "$root/examples/display/acid_box"
    cp "$TOOL" "$root/tools/"; : > "$root/toolchain/rt1170-evkb.toolchain.cmake"; echo "$root"
}

# ★ BENCH_CMAKE and ARM_TOOLCHAIN_BIN must be EXPORTED, not merely assigned.  The
# tool is a separate process; `FOO=1 out=$(cmd)` is a simple command with no
# command word, so POSIX applies both as ordinary shell assignments and FOO never
# reaches the child.
export BENCH_CMAKE="$WORK/bin/fakecmake"
export ARM_TOOLCHAIN_BIN="$WORK/arm"
run_tool() { # run_tool <root> <logfile> [args...] ; sets $out and $rc
    _r=$1; _l=$2; shift 2
    FAKE_LOG="$WORK/$_l"; export FAKE_LOG; : > "$FAKE_LOG"
    out=$("$_r/tools/build-bench-configs.sh" "$@" 2>&1) && rc=0 || rc=1
}

# 1. A configuration that does not build must fail BY NAME, and the run must be non-zero.
root=$(mktree fail)
printf 'bt        -DM2_BT_OUT=ON\nbroken    -DBREAKME=ON\n' > "$root/examples/display/acid_box/bench"
run_tool "$root" log1
check "failing config is named"      "display/acid_box[broken]" "$out"
check "failing config reports FAIL"  "BENCH-BUILDS: FAIL"       "$out"
rc_is "broken config exits non-zero" 1
nocheck "a good config is not blamed" "display/acid_box[bt] FAILED" "$out"
# ★ The operator must be shown the line that NAMES the fault, not make's tail.
check "failure names the fault"      "overflowed by 140 bytes"  "$out"

# 2. All-good must PASS, exit 0, and build BOTH declared configurations.
root=$(mktree ok)
printf '# a comment\nbt        -DM2_BT_OUT=ON\nloopstat  -DM2_BT_OUT=ON -DACIDBOX_LOOPSTAT=ON\n' \
    > "$root/examples/display/acid_box/bench"
run_tool "$root" log2
check "clean run passes" "BENCH-BUILDS: PASS" "$out"
check "config count"     "2 configuration(s)" "$out"
rc_is "clean run exits zero" 0
log=$(cat "$WORK/log2")
check "flags reach cmake (bt)"       "-DM2_BT_OUT=ON"        "$log"
check "flags reach cmake (loopstat)" "-DACIDBOX_LOOPSTAT=ON" "$log"
# ★ THE SOURCE DIRECTORY MUST REACH CMAKE.  Without -S, cmake takes the source dir
#   from the CWD; since 912c8d1 the repo root holds a CMakeLists.txt --
#   project(rt1170_evkb_root NONE) with an empty `all` target -- so a run from the
#   repo root configured THE ROOT PROJECT into the example's build dir, built
#   nothing, exited 0 and reported OK.  Measured on the real tree 2026-09-11, and
#   no other arm could see it: they all inspect -B and -D only.  Checked HERE,
#   where $log and $root still refer to the same tree.
check "source dir reaches cmake" "-S $root/examples/display/acid_box" "$log"

# 3. ★ THE LOAD-BEARING ARM.  build-bench and its -pre/-post siblings carry
#    M2RADIO_IW416_BT_FW pointing at a real 131,840-byte blob; a tool that
#    reconfigured one from the declared flags alone would silently strip it.  The
#    tool must only ever name directories it owns.  Both patterns are
#    PATH-ANCHORED: "/build-bench" would be satisfied by the legitimate
#    "/build-benchcheck-..." and prove nothing, and an unanchored " build-bt"
#    would also be satisfied by a tool that named no directory at all.
nocheck "never configures build-bt"     "/build-bt"     "$log"
nocheck "never configures build-bench-" "/build-bench-" "$log"
check   "owns build-benchcheck-bt"       "build-benchcheck-bt"       "$log"
check   "owns build-benchcheck-loopstat" "build-benchcheck-loopstat" "$log"

# 4. A sidecar that is empty after comments is an error, not a silent pass -- the
#    same rule the `boards` parser applies, and for the same reason: a declaration
#    nobody reads hides in a count.
root=$(mktree empty)
printf '# nothing here\n\n' > "$root/examples/display/acid_box/bench"
run_tool "$root" log4
check "empty sidecar is an error" "declares no configuration" "$out"
rc_is "empty sidecar exits non-zero" 1

# 5. A declared name with no flags is an error -- it would configure a DEFAULT
#    build under a bench name, which links fine and proves nothing.
root=$(mktree noflags)
printf 'bt\n' > "$root/examples/display/acid_box/bench"
run_tool "$root" log5
check "name with no flags is an error" "no cmake flags" "$out"
rc_is "flagless declaration exits non-zero" 1

# 6. No sidecar anywhere is a clean no-op, not a failure -- most examples have none.
root=$(mktree none)
run_tool "$root" log6
check "no sidecars passes"   "BENCH-BUILDS: PASS"  "$out"
check "no sidecars counts 0" "0 configuration(s)"  "$out"
rc_is "no sidecars exits zero" 0

# 7. A pattern argument selects a subset.
root=$(mktree pattern)
mkdir -p "$root/examples/audio/bt_tone_test"
printf 'bt  -DM2_BT_OUT=ON\n'    > "$root/examples/display/acid_box/bench"
printf 'soak  -DM2_BT_SOAK=ON\n' > "$root/examples/audio/bt_tone_test/bench"
run_tool "$root" log7 acid_box
check   "pattern selects"        "display/acid_box[bt]" "$out"
nocheck "pattern excludes other" "bt_tone_test"         "$out"

# 8. ★ A pattern that selects NOTHING is not the same claim as "nothing declared",
#    and only the second deserves a pass.  A typo'd pattern printing PASS is the
#    exact disease this tool treats.
run_tool "$root" log8 typo_nonexistent
check "unmatched pattern is an error" "selected none of the" "$out"
rc_is "unmatched pattern exits non-zero" 1

# 9. ★ An unknown option must be rejected, not swallowed as a pattern -- a
#    mistyped flag becoming a filter that matches nothing would print PASS.
run_tool "$root" log9 -z
check "unknown option rejected" "unknown option" "$out"
rc_is "unknown option exits non-zero" 1

# 10. ★ A cmake that exits 0 having produced no .elf must be a FAILURE, not an OK.
#     This catches the class rather than the instance -- -S could be right and the
#     build still make nothing.
root=$(mktree noelf)
printf 'ghost  -DMAKENOTHING=ON\n' > "$root/examples/display/acid_box/bench"
run_tool "$root" log10
check "no .elf is a failure" "BENCH-BUILDS: FAIL" "$out"
check "no .elf says why"     "cmake exited 0 but produced no .elf" "$out"
rc_is "no .elf exits non-zero" 1

# 11. ★ A STALE OWNED DIRECTORY MUST BE WIPED BEFORE CONFIGURING.  A CMake cache
#     retains every -D ever passed, so a flag removed from a `bench` line would
#     stay in effect forever and the tool would measure a configuration nobody
#     declared.  Measured on the real tree 2026-09-11: a one-off
#     -DACIDBOX_ITCM_MIN_HEADROOM=65536 survived into later runs and kept the
#     build red on a clean tree.
root=$(mktree stale)
printf 'bt  -DM2_BT_OUT=ON\n' > "$root/examples/display/acid_box/bench"
mkdir -p "$root/examples/display/acid_box/build-benchcheck-bt"
: > "$root/examples/display/acid_box/build-benchcheck-bt/STALE-CACHE-MARKER"
run_tool "$root" log11
[ -e "$root/examples/display/acid_box/build-benchcheck-bt/STALE-CACHE-MARKER" ] \
  && { echo "FAIL: stale owned dir was NOT wiped before configure"; fails=$((fails+1)); } \
  || echo "PASS: stale owned dir wiped before configure"
check "still builds after the wipe" "BENCH-BUILDS: PASS" "$out"

# 12. ★ AN INDENTED SIDECAR LINE MUST NOT BECOME AN UNNAMED CONFIGURATION.
#     `name=${line%% *}` yields an EMPTY name for a leading-space line, then
#     builds `build-benchcheck-` and hands cmake the real name as a bare
#     positional argument -- measured, and it reported OK.  Tabs must separate too.
root=$(mktree indent)
printf '   bt\t-DM2_BT_OUT=ON\n' > "$root/examples/display/acid_box/bench"
run_tool "$root" log12
check   "indented line keeps its name" "display/acid_box[bt]" "$out"
nocheck "no unnamed configuration"     "acid_box[] "          "$out"
check   "tab separates name from flags" "-DM2_BT_OUT=ON" "$(cat "$WORK/log12")"
nocheck "no build-benchcheck- dir"     "build-benchcheck- "   "$(cat "$WORK/log12")"

# --- the -n nm-diff path -------------------------------------------------------
# Every arm below drives -n, which until 2026-09-11 had NO coverage at all -- and
# it is the path that certifies the tool's central soundness claim, that an owned
# directory is a valid ITCM proxy for the hand-made one.
nmtree() { # nmtree <name> <owned-syms> <owned-size> <human-syms> <human-size>
    root=$(mktree "$1"); ex="$root/examples/display/acid_box"
    printf 'bt  -DM2_BT_OUT=ON\n' > "$ex/bench"
    # what the fake BUILD will plant in the owned dir (it survives the tool's wipe)
    printf '%s\n' "$2" > "$ex/OWNED_SYMS"; printf '%s\n' "$3" > "$ex/OWNED_SIZE"
    # the hand-made directory, which the tool must never touch
    mkdir -p "$ex/build-bt"; : > "$ex/build-bt/acid_box.elf"
    printf '%s\n' "$4" > "$ex/build-bt/ITCM_SYMS"; printf '%s\n' "$5" > "$ex/build-bt/ITCM_SIZE"
    echo "$root"
}

# 13. Matching symbol sets and sizes -> OK, and the pair is COUNTED.
root=$(nmtree nmok "alpha
beta" 252336 "alpha
beta" 252336)
run_tool "$root" log13 -n
check "nm-diff reports OK"        "nm-diff OK: build-benchcheck-bt == build-bt" "$out"
check "nm-diff counts its pairs"  "nm-diff: 1 pair(s) compared" "$out"
check "nm-diff OK passes overall" "BENCH-BUILDS: PASS" "$out"
rc_is "nm-diff OK exits zero" 0
# ★ The fake nm also emits an ABSOLUTE symbol (_flashimagelen) inside the ITCM
#   address window.  It appears on both sides, so this arm would pass either way;
#   arm 19 is what actually pins the filter.

# 14. Differing symbol sets -> DIFFERS, named, and the run fails.
root=$(nmtree nmdiff "alpha
gamma" 252336 "alpha
beta" 252336)
run_tool "$root" log14 -n
check "nm-diff reports DIFFERS"   "nm-diff DIFFERS" "$out"
check "DIFFERS names both causes" "OR the hand-configured" "$out"
check "DIFFERS fails the run"     "BENCH-BUILDS: FAIL" "$out"
rc_is "DIFFERS exits non-zero" 1

# 15. ★ Same symbol NAMES, different .text.itcm SIZE -> must still be DIFFERS.
#     CLAUDE.md's precedent for this check (2026-09-08, the acid_box ITCM wildcard
#     swap) is "an IDENTICAL ITCM symbol set AND .text.itcm size"; names alone
#     would accept two builds whose ITCM footprints differ.
root=$(nmtree nmsize "alpha
beta" 999999 "alpha
beta" 252336)
run_tool "$root" log15 -n
check "size difference is caught" "nm-diff DIFFERS" "$out"
check "size difference is shown"  "999999 vs 252336" "$out"
rc_is "size difference exits non-zero" 1

# 16. ★ A BROKEN nm MUST NOT READ AS OK.  itcm_syms is a pipeline, so its exit
#     status is sort's -- always 0 -- and two EMPTY files compare EQUAL.  Measured
#     2026-09-11 with ARM_TOOLCHAIN_BIN pointed at a nonexistent directory: the
#     tool printed nm-diff OK and BENCH-BUILDS: PASS having read nothing at all.
root=$(nmtree nmbroken "alpha" 252336 "alpha" 252336)
ARM_TOOLCHAIN_BIN="$WORK/no-such-toolchain" run_tool "$root" log16 -n
export ARM_TOOLCHAIN_BIN="$WORK/arm"
check   "broken nm is reported" "nm-diff BROKEN" "$out"
nocheck "broken nm is not OK"   "nm-diff OK"     "$out"
rc_is "broken nm exits non-zero" 1

# 17. ★ -n THAT COMPARES NO PAIRS MUST SAY SO -- silence read as success is the
#     disease this tool treats -- but must NOT FAIL.  A machine that has never
#     benched this example has no hand-made build-<name> to compare against, and
#     every declared configuration may still have built perfectly.  Making it fatal
#     made the root `bench_check` target red for everyone but one bench machine,
#     which is a guard that gets switched off.  An UNREADABLE pair is a different
#     claim and is still fatal -- arm 16.
root=$(mktree nmnopairs)
printf 'bt  -DM2_BT_OUT=ON\n' > "$root/examples/display/acid_box/bench"
run_tool "$root" log17 -n
check "no pairs counts zero"     "nm-diff: 0 pair(s) compared" "$out"
check "no pairs says why"        "verified nothing"            "$out"
rc_is "no pairs does not fail the run" 0

# 18. ★ -n MUST HONOUR THE PATTERN.  Without it the loop globs every owned dir in
#     the tree, so a scoped invocation could go red for a configuration it was told
#     not to touch -- measured 2026-09-11 against a leftover from an earlier run.
root=$(nmtree nmpattern "alpha" 252336 "alpha" 252336)
other="$root/examples/audio/bt_tone_test"
mkdir -p "$other/build-benchcheck-soak" "$other/build-soak"
printf 'soak  -DM2_BT_SOAK=ON\n' > "$other/bench"
: > "$other/build-benchcheck-soak/x.elf"; : > "$other/build-soak/x.elf"
printf 'zzz\n' > "$other/build-benchcheck-soak/ITCM_SYMS"; echo 1 > "$other/build-benchcheck-soak/ITCM_SIZE"
printf 'yyy\n' > "$other/build-soak/ITCM_SYMS";            echo 2 > "$other/build-soak/ITCM_SIZE"
run_tool "$root" log18 -n acid_box
nocheck "-n ignores dirs outside the pattern" "build-benchcheck-soak" "$out"
check   "-n still compares the selected pair" "nm-diff OK: build-benchcheck-bt" "$out"

# 19. ★ ABSOLUTE SYMBOLS MUST BE FILTERED.  nm type A carries a VALUE, not an
#     address: _flashimagelen's value is the image length, and it drifts into the
#     0x000xxxxx window as an image grows.  So the two sides must STRADDLE the
#     window -- one image under 1 MB (value 0x00051800, which the address regex
#     accepts) and one over (0x00151800, which it rejects) -- and then an unfiltered
#     nm puts the symbol in ONE set only and reports a spurious DIFFERS pointing at
#     entirely the wrong cause.
#     ★★ The first version of this arm gave the two sides DIFFERENT absolute VALUES
#     but both inside the window.  awk prints the symbol NAME, so both sets
#     contained _flashimagelen either way and the arm passed with the filter
#     removed -- vacuous, and only the mutation run exposed it.  An arm that cannot
#     fail is worse than no arm.
root=$(nmtree nmabs "alpha" 252336 "alpha" 252336)
cat > "$WORK/arm/arm-none-eabi-nm" <<'FAKENM2'
#!/bin/sh
elf=$3; [ -n "$elf" ] || elf=$2
d=$(dirname "$elf")
[ -f "$d/ITCM_SYMS" ] || exit 0
i=0
while IFS= read -r s; do printf '000%05x T %s\n' "$i" "$s"; i=$((i+1)); done < "$d/ITCM_SYMS"
# the owned build is small, so its _flashimagelen VALUE lands inside the ITCM
# address window; the human build is over 1 MB, so its value lands outside it
case "$d" in *benchcheck*) printf '00051800 A _flashimagelen\n' ;;
             *)            printf '00151800 A _flashimagelen\n' ;; esac
FAKENM2
chmod +x "$WORK/arm/arm-none-eabi-nm"
run_tool "$root" log19 -n
check "absolute symbols are filtered" "nm-diff OK: build-benchcheck-bt" "$out"
rc_is "absolute-symbol filter keeps the run green" 0

# 20. ★ DISCOVERY MUST PRUNE DIRECTORIES, NOT PATH SUBSTRINGS.  `-not -path
#     '*/build*'` matches the WHOLE path, so a checkout under ~/buildfarm or
#     ~/builds prunes EVERYTHING, finds no sidecars and prints PASS -- this tool's
#     own disease, in the tool itself.  Measured 2026-09-11.  No other arm can see
#     it: they all build their throwaway tree under a path with no "build"
#     component, so this arm deliberately puts one there.
root=$(mktree buildfarm)
printf 'bt  -DM2_BT_OUT=ON\n' > "$root/examples/display/acid_box/bench"
run_tool "$root" log20
check "sidecar found under a build* path" "display/acid_box[bt]" "$out"
check "and is actually counted"           "1 configuration(s)"   "$out"
# ...while a sidecar inside a build directory is still pruned, which is the point
# of pruning at all.
mkdir -p "$root/examples/display/acid_box/build-bt"
printf 'ghost  -DSHOULD_NOT_BE_SEEN=ON\n' > "$root/examples/display/acid_box/build-bt/bench"
run_tool "$root" log20b
nocheck "sidecar inside a build dir is pruned" "ghost" "$out"
check   "still exactly one configuration"      "1 configuration(s)" "$out"

# 21. ★ A CRLF SIDECAR MUST NOT LEAK A CARRIAGE RETURN INTO THE LAST FLAG.  The
#     sibling `boards` parser strips it for the same reason.  Without the strip
#     cmake receives -DM2_BT_OUT=ON\r, which is a different flag.
root=$(mktree crlf)
printf 'bt  -DM2_BT_OUT=ON\r\n' > "$root/examples/display/acid_box/bench"
run_tool "$root" log21
check   "CRLF sidecar builds"        "display/acid_box[bt]" "$out"
nocheck "no CR reaches cmake"        "$(printf 'ON\r')"    "$(cat "$WORK/log21")"
check   "the flag itself is intact"  "-DM2_BT_OUT=ON"       "$(cat "$WORK/log21")"

echo "-------------------------------------------------------------"
if [ "$fails" -eq 0 ]; then echo "build-bench-configs tests PASS"
else echo "$fails failure(s)"; exit 1; fi
