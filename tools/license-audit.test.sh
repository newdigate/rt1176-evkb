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

# =============================================================================
# Part 2 — the dual-licensed EMPTY-object check
# =============================================================================
# An ALLOW-listed copyleft source is excused only because it compiles to NOTHING,
# so "that object defines no symbols" is the entire justification for the
# allowlist. These cases pin the symbol walk that proves it.
#
# Unlike everything above, these need the ARM toolchain: the check shells out to
# arm-none-eabi-ar/nm, so there is no way to exercise it without an archive those
# two can actually read. Faking their output would test the fake. The toolchain
# path is read back out of the audit rather than restated, so the two cannot
# drift apart.
TOOLBIN=$(sed -n 's/^TOOL=//p' "$AUDIT" | head -1)

# An archive with two members whose names COLLIDE by suffix: Stream.cpp.obj is a
# tail of AudioStream.cpp.obj. That is the real shape from cores/teensy4 under
# EVKB_BOARD=rt1062, and it is what an unanchored member match gets wrong.
#
# AudioStream is deliberately added FIRST and given 6 symbols. nm lists members
# in archive order, so with an unanchored match the colliding member's symbols
# arrive first and fill the report's `head -5` on their own — which is exactly
# how this surfaced: a DUAL-LICENSED SOURCE NOT EMPTY report for Stream.cpp that
# listed nothing but AudioStream's symbols.
mk_obj_archive() { # <build-dir> <tag> <empty|full>
    o="$WORK/objs_$2"; mkdir -p "$o"
    printf 'typedef int evkb_no_symbols_t;\n' > "$o/empty.cpp"
    printf 'int stream_timed_read(void){return 3;}\n' > "$o/stream.cpp"
    : > "$o/audio.cpp"
    i=0
    while [ $i -lt 6 ]; do
        printf 'int audiostream_update_%d(void){return %d;}\n' "$i" "$i" >> "$o/audio.cpp"
        i=$((i + 1))
    done
    "$TOOLBIN/arm-none-eabi-gcc" -c "$o/audio.cpp" -o "$o/AudioStream.cpp.obj" || return 1
    case "$3" in
        empty) "$TOOLBIN/arm-none-eabi-gcc" -c "$o/empty.cpp"  -o "$o/Stream.cpp.obj" ;;
        full)  "$TOOLBIN/arm-none-eabi-gcc" -c "$o/stream.cpp" -o "$o/Stream.cpp.obj" ;;
    esac || return 1
    rm -f "$1"/lib*.a
    "$TOOLBIN/arm-none-eabi-ar" rcs "$1/libfake.o.a" \
        "$o/AudioStream.cpp.obj" "$o/Stream.cpp.obj" || return 1
}

# new_tree plus one ALLOW-listed copyleft source in the depfile and an archive
# carrying the colliding pair. Only Stream.cpp is copyleft-headered: AudioStream
# exists solely as an archive member, which is the realistic case — an archive
# holds every TU, and only the copyleft ones are ever symbol-checked.
new_sym_tree() { # <name> <empty|full> -> prints path
    t=$(new_tree "$1" sym_test)
    d="$t/examples/display/sym_test"
    mkdir -p "$t/cores/teensy4"
    printf '/*\n * GNU\n * Lesser General Public\n * License\n */\nclass Stream;\n' \
        > "$t/cores/teensy4/Stream.cpp"
    printf '%s \\\n' "$t/cores/teensy4/Stream.cpp" >> "$d/build/stub.obj.d"
    mk_obj_archive "$d/build" "$1" "$2" || return 1
    printf '%s' "$t"
}

SYMGATE="examples/display/sym_test:sym_test"

# --- control: a NON-empty dual-licensed object is still caught ----------------
# The anchoring fix narrows what the walk matches, and the cheapest way to get
# that wrong is to narrow it to nothing — which would make every case below pass
# while the check silently protected nothing. This pins that it still fires.
test_dual_nonempty_fires() {
    t=$(new_sym_tree dualfull full) || return 1
    out=$(run_part2 "$t" "$SYMGATE"); rc=$?
    [ $rc -ne 0 ] \
        && echo "$out" | grep -q 'DUAL-LICENSED SOURCE NOT EMPTY.*Stream\.cpp' \
        && echo "$out" | grep -q 'LICENSE-AUDIT: FAIL'
}
test_dual_nonempty_fires; report test_dual_nonempty_fires $?

# --- the false positive: an empty object beside a colliding sibling -----------
# Stream.cpp.obj genuinely defines NOTHING, so the allowlist holds and the audit
# must pass. With the unanchored match it inherited AudioStream.cpp.obj's six
# symbols and was flagged anyway — a clean file failing its own gate, the failure
# mode that erodes trust in an audit fastest.
test_dual_empty_not_flagged_despite_collision() {
    t=$(new_sym_tree dualempty empty) || return 1
    out=$(run_part2 "$t" "$SYMGATE"); rc=$?
    [ $rc -eq 0 ] \
        && ! echo "$out" | grep -q 'DUAL-LICENSED SOURCE NOT EMPTY' \
        && echo "$out" | grep -q 'LICENSE-AUDIT: PASS'
}
test_dual_empty_not_flagged_despite_collision; report \
    test_dual_empty_not_flagged_despite_collision $?

# --- the evidence names the right file ---------------------------------------
# A true positive is only actionable if the symbols printed under a filename are
# that file's. Unanchored, the report for Stream.cpp listed AudioStream's symbols
# and sent the reader to debug a file that was not the problem, so this asserts
# both halves: Stream's symbol present, the colliding member's absent.
test_dual_evidence_attributed_correctly() {
    t=$(new_sym_tree dualattr full) || return 1
    out=$(run_part2 "$t" "$SYMGATE")
    echo "$out" | grep -q 'stream_timed_read' \
        && ! echo "$out" | grep -q 'audiostream_update_'
}
test_dual_evidence_attributed_correctly; report test_dual_evidence_attributed_correctly $?

# --- REPOS coverage: a dep path outside every swept root must fail -----------
# Part 1 sweeps only what REPOS names, and its `[ -d ] || continue` skips a
# missing repo silently — so firmware that compiled sources from a tree REPOS
# does not name would pass while the audit never looked at those files. The
# coverage check makes that omission LOUD, like the GATES drift check. The
# rogue file is MIT-headered on purpose: ONLY the coverage check may fire.
test_repos_coverage_fires() {
    t=$(new_tree rogue alpha_test)
    rogue="$WORK/rogue-src"; mkdir -p "$rogue"
    printf '/* MIT licensed: permission is hereby granted. */\n' > "$rogue/outside.h"
    printf '%s \\\n' "$rogue/outside.h" >> "$t/examples/display/alpha_test/build/stub.obj.d"
    out=$(LICENSE_AUDIT_PARTS=2 LICENSE_AUDIT_EVKB="$t" \
          LICENSE_AUDIT_GATES="examples/display/alpha_test:alpha_test" \
          LICENSE_AUDIT_REPOS="$t/no-such-repo" sh "$AUDIT" 2>&1); rc=$?
    [ $rc -ne 0 ] \
        && echo "$out" | grep -q "OUTSIDE SWEPT ROOTS.*alpha_test" \
        && echo "$out" | grep -q "rogue-src/outside.h" \
        && echo "$out" | grep -q 'LICENSE-AUDIT: FAIL'
}
test_repos_coverage_fires; report test_repos_coverage_fires $?

# --- control: the same shape under a REPOS entry passes ----------------------
# Without this, a coverage check that flagged every out-of-EVKB path would
# "detect" the case above while breaking every real gate that links a library.
test_repos_coverage_covered_passes() {
    t=$(new_tree covered alpha_test)
    repo="$WORK/covered-repo"; mkdir -p "$repo"
    printf '/* MIT licensed: permission is hereby granted. */\n' > "$repo/inside.h"
    printf '%s \\\n' "$repo/inside.h" >> "$t/examples/display/alpha_test/build/stub.obj.d"
    out=$(LICENSE_AUDIT_PARTS=2 LICENSE_AUDIT_EVKB="$t" \
          LICENSE_AUDIT_GATES="examples/display/alpha_test:alpha_test" \
          LICENSE_AUDIT_REPOS="$repo" sh "$AUDIT" 2>&1); rc=$?
    [ $rc -eq 0 ] \
        && ! echo "$out" | grep -q 'OUTSIDE SWEPT ROOTS' \
        && echo "$out" | grep -q 'LICENSE-AUDIT: PASS'
}
test_repos_coverage_covered_passes; report test_repos_coverage_covered_passes $?

# --- non-UTF-8 source blind spot ---------------------------------------------
# The motivating shape: SDK v7 VGLite shipped vg_lite_stroke.c in ISO-8859-1.
# grep -I classifies such a file as BINARY and skips it, so the copyleft sweep
# never reads it — MIT or GPL alike. The audit must therefore refuse
# non-UTF-8 source outright. The file below is the exact shape: perfectly
# ordinary C with one latin-1 byte (0xE9, 'é') in a comment.
test_non_utf8_source_fires() {
    r=$(new_repo nonutf8)
    printf '/* Copyright (c) R\351my. MIT: permission is hereby granted. */\nint x;\n' \
        > "$r/latin1.c"
    git -C "$r" add -A 2>/dev/null
    out=$(run_part1 "$r"); rc=$?
    [ $rc -ne 0 ] \
        && echo "$out" | grep -q 'NON-UTF-8 SOURCE' \
        && echo "$out" | grep -q 'latin1.c' \
        && echo "$out" | grep -q 'LICENSE-AUDIT: FAIL'
}
test_non_utf8_source_fires; report test_non_utf8_source_fires $?

exit $FAILED
