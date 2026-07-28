#!/bin/sh
# Negative tests for license-audit.sh — part 1 (the copyleft-header sweep and the
# binary-provenance check) and part 2 (the GATES drift check). A gate that only ever passes proves nothing, so every
# check here is shown FIRING on a throwaway repo built to trip it, and NOT firing
# on the licensed shapes the audit must keep tolerating.
#
# Part 1 is driven against a temp repo via the LICENSE_AUDIT_REPOS /
# LICENSE_AUDIT_PARTS hooks, and part 2 against a throwaway example tree via
# LICENSE_AUDIT_EVKB / LICENSE_AUDIT_GATES / LICENSE_AUDIT_GATES_EXEMPT — so no
# gate builds and no network are needed for any case here.
# Usage: sh evkb/tools/license-audit.test.sh   (prints PASS:/FAIL: per case)
set -u
AUDIT="$(cd "$(dirname "$0")" && pwd)/license-audit.sh"
FAILED=0
report() { # <name> <0-pass|1-fail>
    if [ "$2" -eq 0 ]; then echo "PASS: $1"; else echo "FAIL: $1"; FAILED=1; fi
}

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM HUP

# A throwaway git repo with a permissive root LICENSE and one MIT source file.
# Its root licence is deliberate: it is what a repo-root-to-leaf provenance walk
# would wrongly accept, and these tests pin that it does not.
new_repo() { # <name> -> prints path
    d="$WORK/$1"
    mkdir -p "$d"
    git -C "$d" init -q 2>/dev/null
    printf 'MIT License\n\nPermission is hereby granted, free of charge...\n' > "$d/LICENSE"
    printf '/* Copyright (c). MIT licensed: permission is hereby granted. */\nint ok(void){return 0;}\n' \
        > "$d/ok.c"
    git -C "$d" add -A 2>/dev/null
    printf '%s' "$d"
}

# 8 bytes of ar-archive header + NULs: a real binary, so `grep -I` skips it and
# only the structural check can see it at all.
mk_archive() { # <path>
    mkdir -p "$(dirname "$1")"
    printf '!<arch>\n\000\001\002\377\000' > "$1"
}

# Run part 1 only, against one repo. Prints the audit output; returns its status.
run_part1() { # <repo>
    LICENSE_AUDIT_PARTS=1 LICENSE_AUDIT_REPOS="$1" sh "$AUDIT" 2>&1
}

# --- control: a clean repo passes -------------------------------------------
# Without this, every case below could be passing for the wrong reason (an audit
# that fails on everything would "detect" all of them).
test_clean_repo_passes() {
    r=$(new_repo clean)
    out=$(run_part1 "$r"); rc=$?
    [ $rc -eq 0 ] && echo "$out" | grep -q 'LICENSE-AUDIT: PASS'
}
test_clean_repo_passes; report test_clean_repo_passes $?

# --- binary blind spot -------------------------------------------------------
# The motivating shape, reproduced exactly: LVGL's
# lvgl/libs/nema_gfx/lib/core/cortex_m33_revC/gcc/*.a — tracked archives with no
# licence text in their subtree, under a project whose ROOT is licensed.
test_binary_unlicensed_fires() {
    r=$(new_repo bin_unlicensed)
    mk_archive "$r/libs/nema_gfx/lib/core/cortex_m33_revC/gcc/libnema.a"
    git -C "$r" add -A 2>/dev/null
    out=$(run_part1 "$r"); rc=$?
    [ $rc -ne 0 ] \
      && echo "$out" | grep -q 'UNLICENSED BINARY' \
      && echo "$out" | grep -q 'libnema\.a' \
      && echo "$out" | grep -q 'LICENSE-AUDIT: FAIL'
}
test_binary_unlicensed_fires; report test_binary_unlicensed_fires $?

# All five extensions are covered, not just .a — each on its own repo so one
# missing extension cannot hide behind another's failure. Nested three deep so
# the root LICENSE is genuinely out of range (at vendor/blob.x it would be "one
# level up" and correctly accepted).
test_binary_all_extensions_fire() {
    result=0
    for ext in a o so dylib lib; do
        r=$(new_repo "bin_ext_$ext")
        mk_archive "$r/vendor/nema/lib/blob.$ext"
        git -C "$r" add -A 2>/dev/null
        out=$(run_part1 "$r")
        echo "$out" | grep -q "blob\.$ext" || { echo "  missed .$ext"; result=1; }
    done
    return $result
}
test_binary_all_extensions_fire; report test_binary_all_extensions_fire $?

# NOT "no binaries": licence text beside the blob is provenance, and is accepted.
# This is the LiberationSans-Regular.ttf / freetype-LICENSE.txt shape, including
# the prefixed filename (LiberationSans-LICENSE.txt) rather than a bare LICENSE.
test_binary_licensed_adjacent_passes() {
    r=$(new_repo bin_adjacent)
    mk_archive "$r/vendor/nema/libnema.a"
    printf 'NemaGFX commercial licence text.\n' > "$r/vendor/nema/NemaGFX-LICENSE.txt"
    git -C "$r" add -A 2>/dev/null
    out=$(run_part1 "$r"); rc=$?
    [ $rc -eq 0 ] && echo "$out" | grep -qv 'UNLICENSED BINARY'
}
test_binary_licensed_adjacent_passes; report test_binary_licensed_adjacent_passes $?

# One level up also counts — the common vendor layout (lib/ under a licensed
# component directory). Spelling variants LICENCE/COPYING must count too.
test_binary_licensed_parent_passes() {
    result=0
    for name in LICENCE COPYING LICENSE.txt; do
        r=$(new_repo "bin_parent_$name")
        mk_archive "$r/vendor/nema/lib/libnema.a"
        printf 'licence text\n' > "$r/vendor/nema/$name"
        git -C "$r" add -A 2>/dev/null
        out=$(run_part1 "$r")
        echo "$out" | grep -q 'UNLICENSED BINARY' && { echo "  rejected $name"; result=1; }
    done
    return $result
}
test_binary_licensed_parent_passes; report test_binary_licensed_parent_passes $?

# Two levels up does NOT count, and the repo root emphatically does not: that
# leniency is what would have let lvgl/libs/nema_gfx/ through under
# lvgl/LICENCE.txt. Pins the depth limit so widening it cannot pass silently.
test_binary_grandparent_still_fires() {
    r=$(new_repo bin_grandparent)
    mk_archive "$r/vendor/nema/lib/core/libnema.a"
    printf 'licence text\n' > "$r/vendor/nema/LICENSE"
    git -C "$r" add -A 2>/dev/null
    out=$(run_part1 "$r"); rc=$?
    [ $rc -ne 0 ] && echo "$out" | grep -q 'UNLICENSED BINARY'
}
test_binary_grandparent_still_fires; report test_binary_grandparent_still_fires $?

# COPYRIGHTS.md is an attribution index, not a licence grant — it must not be
# mistaken for provenance (lvgl/ ships exactly this file).
test_binary_copyrights_md_not_licence() {
    r=$(new_repo bin_copyrights)
    mk_archive "$r/vendor/nema/libnema.a"
    printf 'Attribution index, not a grant.\n' > "$r/vendor/nema/COPYRIGHTS.md"
    git -C "$r" add -A 2>/dev/null
    out=$(run_part1 "$r"); rc=$?
    [ $rc -ne 0 ] && echo "$out" | grep -q 'UNLICENSED BINARY'
}
test_binary_copyrights_md_not_licence; report test_binary_copyrights_md_not_licence $?

# Scope is git-TRACKED files: build output is untracked by design and must not
# trip the gate, or no example directory could ever be swept.
test_binary_untracked_ignored() {
    r=$(new_repo bin_untracked)
    mk_archive "$r/build/libcores.a"        # never `git add`ed
    out=$(run_part1 "$r"); rc=$?
    [ $rc -eq 0 ] && echo "$out" | grep -qv 'UNLICENSED BINARY'
}
test_binary_untracked_ignored; report test_binary_untracked_ignored $?

# A non-repo directory (e.g. ~/Development/SD) cannot be checked for tracked-ness.
# It must say so out loud rather than skip in silence — a silent gap in a
# provenance gate is the bug class this whole change exists to remove.
test_non_repo_reports_note() {
    d="$WORK/plain_dir"
    mkdir -p "$d"
    printf 'int ok(void){return 0;}\n' > "$d/ok.c"
    out=$(run_part1 "$d"); rc=$?
    [ $rc -eq 0 ] && echo "$out" | grep -q 'not a git repo'
}
test_non_repo_reports_note; report test_non_repo_reports_note $?

# --- MPL blind spot ----------------------------------------------------------
# MPL-2.0 is weak copyleft the old regex could not see. LVGL 9.4.0's
# src/libs/frogfs/ carried exactly this boilerplate.
test_mpl_fires() {
    r=$(new_repo mpl)
    printf '/* This Source Code Form is subject to the terms of the Mozilla Public\n * License, v. 2.0. */\nint f(void){return 1;}\n' \
        > "$r/frogfs.c"
    git -C "$r" add -A 2>/dev/null
    out=$(run_part1 "$r"); rc=$?
    [ $rc -ne 0 ] \
      && echo "$out" | grep -q 'COPYLEFT header, not allowlisted' \
      && echo "$out" | grep -q 'frogfs\.c'
}
test_mpl_fires; report test_mpl_fires $?

# The wrap tolerance must hold for MPL too: "Mozilla Public" / " * License" split
# across lines with star decoration is the form the boilerplate actually takes,
# and is the gap the SEP class exists to close.
test_mpl_wrapped_fires() {
    r=$(new_repo mpl_wrapped)
    printf '/*\n * Licensed under the Mozilla\n *   Public\n *   License Version 2.0\n */\nint f(void){return 1;}\n' \
        > "$r/wrapped.c"
    git -C "$r" add -A 2>/dev/null
    out=$(run_part1 "$r"); rc=$?
    [ $rc -ne 0 ] && echo "$out" | grep -q 'wrapped\.c'
}
test_mpl_wrapped_fires; report test_mpl_wrapped_fires $?

# --- regression: strong copyleft detection is not weakened -------------------
# Adding MPL to the pattern must not disturb GPL/LGPL matching, wrapped or plain.
test_gpl_still_fires() {
    result=0
    i=0
    for body in \
'/* Licensed under the GNU General Public License version 2. */' \
'/*\n * GNU\n * Lesser General Public\n * License\n */' \
'/* GNU Lesser General Public License v2.1 */'
    do
        i=$((i + 1))
        r=$(new_repo "gpl_$i")
        printf "$body\nint f(void){return 1;}\n" > "$r/gpl.c"
        git -C "$r" add -A 2>/dev/null
        out=$(run_part1 "$r")
        echo "$out" | grep -q 'gpl\.c' || { echo "  missed variant $i"; result=1; }
    done
    return $result
}
test_gpl_still_fires; report test_gpl_still_fires $?

# The sweep's existing exclusions must survive: a repo's own LICENSE/COPYING
# files and .md docs may name GPL/MPL without that being a finding.
test_licence_files_still_excluded() {
    r=$(new_repo excluded)
    printf 'GNU General Public License\n' > "$r/COPYING"
    printf 'Mozilla Public License 2.0\n' > "$r/NOTES.md"
    git -C "$r" add -A 2>/dev/null
    out=$(run_part1 "$r"); rc=$?
    [ $rc -eq 0 ] && echo "$out" | grep -q 'LICENSE-AUDIT: PASS'
}
test_licence_files_still_excluded; report test_licence_files_still_excluded $?

# =============================================================================
# Part 2 — the GATES drift check
# =============================================================================
# GATES is hand-maintained, and an example missing from it is invisible to the
# depfile walk. That gap recurred three times in one day (rk055_panel_test,
# lvgl_rpi_panel_test, then lvgl_smoke_test — which links LVGL, the tree's most
# licence-sensitive dependency), which is why the check exists. These cases pin
# that it actually fires, rather than being one more thing that always passes.
#
# The tree below is throwaway: an empty build/<target>.elf and no depfiles means
# the walk finds 0 dep paths and completes cleanly, so these cases exercise the
# drift logic alone with no compiler, no builds and no network.

# A fake evkb tree with <n> gate examples. Each gets a run_qemu.sh (what the
# drift check enumerates), a CMakeLists.txt with a project() (what the target
# name is checked against), a stub built ELF, and a depfile naming 120 real
# permissively-headered sources.
#
# The 120 is not padding: the walk has a "too few files checked" guard that
# fires below 100, so a gate that is listed but effectively auditing nothing
# gets caught. A fixture with no depfiles trips that guard and would make these
# cases pass for the wrong reason — it was the first thing these tests hit.
new_tree() { # <name> <example...> -> prints path
    t="$WORK/$1"; shift
    for ex in "$@"; do
        d="$t/examples/display/$ex"
        mkdir -p "$d/build/src"
        printf '#!/bin/sh\nexit 0\n' > "$d/run_qemu.sh"
        printf 'project(%s)\n' "$ex" > "$d/CMakeLists.txt"
        : > "$d/build/$ex.elf"
        i=0
        : > "$d/build/stub.obj.d"
        while [ $i -lt 120 ]; do
            f="$d/build/src/h$i.h"
            printf '/* MIT licensed: permission is hereby granted. */\n' > "$f"
            printf '%s \\\n' "$f" >> "$d/build/stub.obj.d"
            i=$((i + 1))
        done
    done
    printf '%s' "$t"
}

# Run part 2 only, against a fake tree and an explicit GATES list.
run_part2() { # <tree> <gates> [exempt]
    LICENSE_AUDIT_PARTS=2 LICENSE_AUDIT_EVKB="$1" LICENSE_AUDIT_GATES="$2" \
        LICENSE_AUDIT_GATES_EXEMPT="${3:-}" sh "$AUDIT" 2>&1
}

# --- control: a complete list passes ----------------------------------------
# Same reasoning as the part-1 control: without it, a drift check that fired on
# everything would "detect" all the cases below.
test_gates_complete_passes() {
    t=$(new_tree complete alpha_test beta_test)
    out=$(run_part2 "$t" "examples/display/alpha_test:alpha_test examples/display/beta_test:beta_test")
    rc=$?
    [ $rc -eq 0 ] \
        && ! echo "$out" | grep -q 'GATES DRIFT' \
        && echo "$out" | grep -q 'LICENSE-AUDIT: PASS'
}
test_gates_complete_passes; report test_gates_complete_passes $?

# --- the motivating shape: a gate exists but is not listed -------------------
# Exactly what lvgl_smoke_test was: a run_qemu.sh with no GATES entry, so Part 2
# silently never walked it.
test_gates_missing_entry_fires() {
    t=$(new_tree missing alpha_test beta_test)
    out=$(run_part2 "$t" "examples/display/alpha_test:alpha_test"); rc=$?
    [ $rc -ne 0 ] \
        && echo "$out" | grep -q 'GATES DRIFT.*beta_test' \
        && echo "$out" | grep -q 'LICENSE-AUDIT: FAIL'
}
test_gates_missing_entry_fires; report test_gates_missing_entry_fires $?

# --- a listed gate whose target name is wrong --------------------------------
# Part 2 looks for build/<target>.elf, so a wrong target name makes the gate
# report MISSING BUILD forever — auditing nothing while looking listed.
test_gates_wrong_target_fires() {
    t=$(new_tree wrongname alpha_test)
    out=$(run_part2 "$t" "examples/display/alpha_test:not_the_project"); rc=$?
    [ $rc -ne 0 ] && echo "$out" | grep -q "GATES MISMATCH.*not_the_project.*alpha_test"
}
test_gates_wrong_target_fires; report test_gates_wrong_target_fires $?

# --- the escape hatch works, and only when used ------------------------------
# An example that genuinely should not be audited goes in GATES_EXEMPT with a
# written reason. Pin that it suppresses the drift — otherwise the only way past
# a false positive is deleting the check, which is how these erode.
test_gates_exempt_suppresses() {
    t=$(new_tree exempt alpha_test beta_test)
    out=$(run_part2 "$t" "examples/display/alpha_test:alpha_test" \
                    "examples/display/beta_test"); rc=$?
    [ $rc -eq 0 ] \
        && ! echo "$out" | grep -q 'GATES DRIFT' \
        && echo "$out" | grep -q 'LICENSE-AUDIT: PASS'
}
test_gates_exempt_suppresses; report test_gates_exempt_suppresses $?

exit $FAILED
