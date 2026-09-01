#!/bin/sh
# vglite-conformance-check.sh — diff a display/vglite_conformance SILICON
# transcript against the PRE-REGISTERED expectation, and fail on drift in
# EITHER direction.
#
# Usage: tools/vglite-conformance-check.sh <transcript> [expected]
#   default expected: examples/display/vglite_conformance/expected_silicon.txt
#   exit 0  every case matches (PASS line names the case count)
#   exit 1  DRIFT — the board and the expectation disagree
#   exit 2  usage error, an unreadable file, a malformed expectation, or a
#           transcript that is not a GPU run
#
# ★ WHY DRIFT IN *EITHER* DIRECTION IS A FAILURE. A quirk that silently
# DISAPPEARS after an SDK re-vendor or a pin bump matters as much as a new one
# appearing: it means the driver moved under us, and the safe-usage rules two
# shipping compositors (SynthUI's rotary knob and fader) are BUILT ON are no
# longer describing the machine. That is news, and news should not arrive as a
# green run. The expectation is a claim; this script is the only thing that
# keeps the claim honest across boots, SDK drops and library pins.
#
# ★ AND THE FIX FOR A RED IS NEVER `cp transcript expected`. See the
# expectation file's header: a verdict flipping to `ok` needs an explanation
# (driver change? our usage changed?) exactly as a re-goldened checksum does.
#
# House style follows tools/license-audit.sh and tools/gate-vacuity.test.sh:
# POSIX sh, no bashisms, a mktemp -d work dir with a cleanup trap, and named
# failures rather than bare exit codes.
set -eu

EVKB=$(cd "$(dirname "$0")/.." && pwd)
DEFAULT_EXPECTED="$EVKB/examples/display/vglite_conformance/expected_silicon.txt"

usage() {
    cat >&2 <<USAGE
usage: $0 <transcript> [expected]
  <transcript>  a display/vglite_conformance capture from SILICON
                (must contain a \`vgc_engine=gpu\` line)
  [expected]    default: $DEFAULT_EXPECTED
USAGE
    exit 2
}

[ $# -ge 1 ] && [ $# -le 2 ] || usage
TRANSCRIPT=$1
EXPECTED=${2:-$DEFAULT_EXPECTED}
[ -r "$TRANSCRIPT" ] || { echo "FAIL: cannot read transcript: $TRANSCRIPT" >&2; exit 2; }
[ -r "$EXPECTED" ]   || { echo "FAIL: cannot read expectation: $EXPECTED" >&2; exit 2; }

WORK=$(mktemp -d "${TMPDIR:-/tmp}/vgc-check.XXXXXX") || exit 2
trap 'rm -rf "$WORK"' EXIT INT TERM HUP

# ---------------------------------------------------------------------------
# 1. Parse and VALIDATE the expectation.
#
# The validation is not pedantry: every rule here is one that, unenforced,
# lets the expectation quietly stop expecting something. A malformed file is a
# config error (exit 2), never a pass and never a drift — "the expectation was
# unreadable" must not be spellable as "the board matched".
# ---------------------------------------------------------------------------
awk -v casesf="$WORK/exp.cases" -v pairsf="$WORK/exp.pairs" '
function die(msg) { printf "CONFIG ERROR: %s line %d: %s\n", FILENAME, FNR, msg > "/dev/stderr"; err = 1 }
function isort(a, n,   i, j, t) {
    for (i = 2; i <= n; i++) { t = a[i]; j = i - 1
        while (j >= 1 && a[j] > t) { a[j + 1] = a[j]; j-- }
        a[j + 1] = t }
}
BEGIN { err = 0; ncase = 0 }
{
    raw = $0; sub(/\r$/, "", raw)
    body = raw; comment = ""
    h = index(body, "#")
    if (h > 0) { comment = substr(body, h + 1); body = substr(body, 1, h - 1) }
    gsub(/^[ \t]+/, "", body); gsub(/[ \t]+$/, "", body)
    if (body == "") next
    n = split(body, f, /[ \t]+/)

    if (f[1] == "pair") {
        # pair <name> <id>=<verdict>[,<id>=<verdict>...] meaning=<slug>
        if (n != 4) { die("pair line must be: pair <name> <id>=<verdict>[,...] meaning=<slug>"); next }
        pname = f[2]
        if (f[4] !~ /^meaning=[^ \t]+$/) { die("pair tuple must carry meaning=<slug>"); next }
        mean = substr(f[4], 9)
        m = split(f[3], parts, ",")
        bad = 0
        for (i = 1; i <= m; i++) {
            if (parts[i] !~ /^[^=,]+=(ok|broken)$/) { die("bad tuple member \"" parts[i] "\" (want <id>=ok or <id>=broken)"); bad = 1; break }
            k = index(parts[i], "=")
            tid[i] = substr(parts[i], 1, k - 1)
            tvd[substr(parts[i], 1, k - 1)] = substr(parts[i], k + 1)
        }
        if (bad) next
        isort(tid, m)
        canon = ""; ids = ""
        for (i = 1; i <= m; i++) {
            canon = canon (i > 1 ? "," : "") tid[i] "=" tvd[tid[i]]
            ids   = ids   (i > 1 ? "," : "") tid[i]
        }
        if ((pname SUBSEP canon) in tupseen) { die("pair " pname " lists the same joint outcome twice"); next }
        tupseen[pname SUBSEP canon] = 1
        if ((pname SUBSEP mean) in meanseen) { die("pair " pname " reuses meaning=" mean " — each admissible outcome must MEAN something distinct"); next }
        meanseen[pname SUBSEP mean] = 1
        pn[pname]++
        ptup[pname, pn[pname]] = canon
        pids[pname, pn[pname]] = ids
        pmean[pname, pn[pname]] = mean
        next
    }

    if (n != 3) { die("case line must be: <id> <ok|broken|pair:NAME> <same|differs>"); next }
    id = f[1]; v = f[2]; r = f[3]
    if (v != "ok" && v != "broken" && v !~ /^pair:[^ \t]+$/) { die("bad verdict \"" v "\" (want ok, broken or pair:NAME)"); next }
    # ★ `unstable` is a THIRD repeat expectation, and it is NOT an escape
    # hatch -- it records a MEASURED fact that same|differs cannot express: a
    # case observed BOTH ways across boots. Pinning `same` reds half the future
    # runs and pinning `differs` reds the other half; either trains a reader to
    # ignore this checker, which is worse than the drift it would report.
    # It is admissible ONLY on the repeat field, never on a pixel verdict -- an
    # intermittently WRONG PICTURE is a defect, not a recorded property -- and
    # like `broken` it must carry a written reason. The actual reading is
    # still PRINTED for each run (see the UNSTABLE line below), so nothing
    # is hidden.
    if (r != "same" && r != "differs" && r != "unstable") { die("bad repeat \"" r "\" (want same, differs or unstable)"); next }
    if (r == "unstable" && comment !~ /[^ \t]/) {
        die("an `unstable` repeat line must carry a non-empty # reason"); next
    }
    if (id in cverd) { die("duplicate case id " id); next }
    # ★ A `broken` or `pair:` cell must SAY WHY it is expected. Without this a
    # new quirk can be pinned into the file by whoever pasted it there, with no
    # record of what was believed at the time — which is the rubber stamp this
    # whole pair of files exists to prevent.
    if ((v == "broken" || v ~ /^pair:/) && comment !~ /[^ \t]/) {
        die("a `" v "` line must carry a non-empty # reason"); next
    }
    ncase++; order[ncase] = id; cverd[id] = v; crep[id] = r
    if (v ~ /^pair:/) {
        pname = substr(v, 6)
        if (!(pname in memn)) { pseq[++npair] = pname }
        memn[pname]++
        mem[pname, memn[pname]] = id
    }
}
END {
    if (ncase == 0) { printf "CONFIG ERROR: %s: no case lines\n", FILENAME > "/dev/stderr"; err = 1 }

    # ---- pair validation: the anti-escape-hatch rules -----------------------
    for (i = 1; i <= npair; i++) {
        p = pseq[i]
        k = memn[p]
        if (k < 2) { printf "CONFIG ERROR: pair %s has %d member(s); a pair is an experiment with at least two arms\n", p, k > "/dev/stderr"; err = 1; continue }
        if (!(p in pn)) { printf "CONFIG ERROR: pair %s has members but no `pair %s ...` admissible-outcome lines\n", p, p > "/dev/stderr"; err = 1; continue }
        for (j = 1; j <= k; j++) ms[j] = mem[p, j]
        isort(ms, k)
        want = ""
        for (j = 1; j <= k; j++) want = want (j > 1 ? "," : "") ms[j]
        # Rule: every tuple assigns a verdict to EXACTLY the members, no more
        # and no fewer — a member cannot be dropped from the experiment while
        # the pair keeps passing.
        for (t = 1; t <= pn[p]; t++) {
            if (pids[p, t] != want) {
                printf "CONFIG ERROR: pair %s outcome %d covers {%s} but its members are {%s}\n", p, t, pids[p, t], want > "/dev/stderr"
                err = 1
            }
        }
        # Rule: at least two admissible outcomes (else it is just a verdict
        # written the long way) and STRICTLY FEWER than all 2^k combinations
        # (else it admits everything and is an escape hatch, not an
        # expectation).
        pow = 1; for (j = 0; j < k; j++) pow *= 2
        if (pn[p] < 2) { printf "CONFIG ERROR: pair %s admits only %d outcome; write it as a plain verdict instead\n", p, pn[p] > "/dev/stderr"; err = 1 }
        if (pn[p] >= pow) { printf "CONFIG ERROR: pair %s admits all %d combinations of its %d members — that is an escape hatch, not an expectation\n", p, pow, k > "/dev/stderr"; err = 1 }
    }
    # A `pair NAME ...` line whose NAME no case claims is dead weight that
    # reads as coverage.
    for (p in pn) {
        if (!(p in memn)) { printf "CONFIG ERROR: pair %s has admissible outcomes but no case line says pair:%s\n", p, p > "/dev/stderr"; err = 1 }
    }
    if (err) exit 2

    for (i = 1; i <= ncase; i++) { id = order[i]; printf "%s\t%s\t%s\n", id, cverd[id], crep[id] > casesf }
    for (i = 1; i <= npair; i++) {
        p = pseq[i]
        for (t = 1; t <= pn[p]; t++) printf "%s\t%s\t%s\n", p, ptup[p, t], pmean[p, t] > pairsf
    }
}' "$EXPECTED" || { echo "FAIL: $EXPECTED is not a usable expectation (see CONFIG ERROR above)" >&2; exit 2; }
touch "$WORK/exp.pairs"

# ---------------------------------------------------------------------------
# 2. REFUSE a transcript that is not a GPU run, BY NAME.
#
# QEMU has no GC355 model, so a QEMU capture reports vgc_engine=absent and
# every case pixel=skip. Diffing that would produce 13 "drift" lines for a
# reason that is not drift, and the real defect — the wrong file was handed to
# the checker — would be buried in them. The engine line is a prefix match:
# the silicon line continues `vgc_engine=gpu target=128x128 fmt=... tess=...`.
# ---------------------------------------------------------------------------
# ([[:space:]]|$) rather than a bare $ or a hand-spelled \r: it accepts the
# space that precedes `target=` on silicon, accepts a CR from a CRLF capture,
# and still refuses a longer token such as `vgc_engine=gpuX`.
if ! grep -aqE '^vgc_engine=gpu([[:space:]]|$)' "$TRANSCRIPT"; then
    ENGINE=$(grep -aE '^vgc_engine=' "$TRANSCRIPT" | head -1 | tr -d '\r' || true)
    echo "FAIL: not a GPU run — refusing to diff $TRANSCRIPT"
    if [ -n "$ENGINE" ]; then
        echo "  the transcript says: $ENGINE"
    else
        echo "  the transcript has no vgc_engine= line at all"
    fi
    echo "  expected_silicon.txt describes what the GC355 does; a QEMU capture is"
    echo "  all-skip and would differ from every line for a reason that is NOT drift."
    echo "  Hand this checker the silicon capture (transcript_hw_evkb.txt)."
    exit 2
fi

# ---------------------------------------------------------------------------
# 3. Extract <id> <pixel> <repeat> from the transcript's case lines.
#
# ★ FIELDS ARE READ BY NAME, NOT BY POSITION. The case line is
#   vgc case=<id> api=<a> api2=<a2> pixel=<p> detail=<d> repeat=<r>
# and the api2= field between api= and pixel= is exactly what a positional or
# naive regex misses — that mistake was already made once in this work. The
# detail field is space-free by contract (print_case_line's sanitise_detail
# enforces it), which is what keeps this split safe.
# ---------------------------------------------------------------------------
awk '
/^vgc case=/ {
    line = $0; sub(/\r$/, "", line)
    id = ""; pixel = ""; rep = ""
    n = split(line, f, /[ \t]+/)
    for (i = 1; i <= n; i++) {
        if      (f[i] ~ /^case=/)   id    = substr(f[i], 6)
        else if (f[i] ~ /^pixel=/)  pixel = substr(f[i], 7)
        else if (f[i] ~ /^repeat=/) rep   = substr(f[i], 8)
    }
    if (id == "" || pixel == "" || rep == "") {
        printf "FAIL: malformed case line %d: %s\n", FNR, line > "/dev/stderr"; err = 1; next
    }
    if (id in seen) { printf "FAIL: case %s appears twice in the transcript\n", id > "/dev/stderr"; err = 1; next }
    seen[id] = 1
    printf "%s\t%s\t%s\n", id, pixel, rep
}
END { if (err) exit 2 }' "$TRANSCRIPT" > "$WORK/obs.cases" || \
    { echo "FAIL: $TRANSCRIPT does not parse as a conformance capture" >&2; exit 2; }

if [ ! -s "$WORK/obs.cases" ]; then
    echo "FAIL: $TRANSCRIPT contains no 'vgc case=' lines (truncated capture?)" >&2
    exit 2
fi

# ---------------------------------------------------------------------------
# 4. Compare. Every disagreement is printed; the exit status is decided after
#    all of them, so one red cell never hides the next.
# ---------------------------------------------------------------------------
RC=0
awk -v casesf="$WORK/exp.cases" -v pairsf="$WORK/exp.pairs" -v obsf="$WORK/obs.cases" '
function isort(a, n,   i, j, t) {
    for (i = 2; i <= n; i++) { t = a[i]; j = i - 1
        while (j >= 1 && a[j] > t) { a[j + 1] = a[j]; j-- }
        a[j + 1] = t }
}
BEGIN {
    drift = 0
    while ((getline l < casesf) > 0) {
        split(l, f, "\t"); id = f[1]
        eorder[++ne] = id; everd[id] = f[2]; erep[id] = f[3]
        if (f[2] ~ /^pair:/) {
            p = substr(f[2], 6)
            if (!(p in memn)) pseq[++npair] = p
            memn[p]++; mem[p, memn[p]] = id
        }
    }
    close(casesf)
    while ((getline l < pairsf) > 0) {
        split(l, f, "\t"); p = f[1]
        pn[p]++; ptup[p, pn[p]] = f[2]; pmean[p, pn[p]] = f[3]
    }
    close(pairsf)
    while ((getline l < obsf) > 0) {
        split(l, f, "\t"); id = f[1]
        oorder[++no] = id; overd[id] = f[2]; orep[id] = f[3]
    }
    close(obsf)

    # ---- the matrix itself must not have moved --------------------------
    # A case present in one file and not the other is checked FIRST and on
    # its own: a matrix that quietly shrinks passes every per-case
    # comparison vacuously, the same way a SKIP hides in a gate count.
    for (i = 1; i <= ne; i++) {
        id = eorder[i]
        if (!(id in overd)) { printf "DRIFT  %-34s expected %-8s but the transcript has NO SUCH CASE\n", id, everd[id]; drift++ }
    }
    for (i = 1; i <= no; i++) {
        id = oorder[i]
        if (!(id in everd)) { printf "DRIFT  %-34s the transcript reports %s but the expectation has NO SUCH CASE\n", id, overd[id]; drift++ }
    }

    # ---- per-case pixel verdict and repeat ------------------------------
    for (i = 1; i <= ne; i++) {
        id = eorder[i]
        if (!(id in overd)) continue
        if (everd[id] !~ /^pair:/ && overd[id] != everd[id]) {
            printf "DRIFT  %-34s pixel: expected %-6s got %s\n", id, everd[id], overd[id]; drift++
        }
        # repeat is pinned per case even for pair members: an admissible
        # range of PIXEL outcomes never makes nondeterminism admissible.
        if (erep[id] == "unstable") {
            # Admissible either way -- but say which, every run. An `unstable`
            # cell that quietly stopped varying is itself worth noticing, and a
            # reader must never have to guess what this boot actually did.
            printf "UNSTABLE %-33s repeat: %s (expected unstable -- both readings admissible)\n", id, orep[id]
        } else if (orep[id] != erep[id]) {
            printf "DRIFT  %-34s repeat: expected %-7s got %s\n", id, erep[id], orep[id]; drift++
        }
    }

    # ---- joint (pair) outcomes ------------------------------------------
    for (i = 1; i <= npair; i++) {
        p = pseq[i]
        k = memn[p]
        missing = 0
        for (j = 1; j <= k; j++) { ms[j] = mem[p, j]; if (!(mem[p, j] in overd)) missing = 1 }
        if (missing) continue     # already reported as a moved matrix above
        isort(ms, k)
        got = ""
        for (j = 1; j <= k; j++) got = got (j > 1 ? "," : "") ms[j] "=" overd[ms[j]]
        okj = 0
        for (t = 1; t <= pn[p]; t++) if (ptup[p, t] == got) { okj = 1; hit = pmean[p, t] }
        if (okj) {
            printf "PAIR   %-34s %s  => %s\n", p, got, hit
        } else {
            printf "DRIFT  pair %-29s got %s — NOT an admissible joint outcome\n", p, got
            for (t = 1; t <= pn[p]; t++) printf "         admissible: %s  => %s\n", ptup[p, t], pmean[p, t]
            drift++
        }
    }
    exit (drift > 0 ? 1 : 0)
}' || RC=$?

CASES=$(wc -l < "$WORK/obs.cases" | tr -d ' ')

if [ "$RC" -eq 0 ]; then
    echo "PASS: $CASES cases match $EXPECTED"
    exit 0
fi

# ---------------------------------------------------------------------------
# 5. Drift. Say what each shape MEANS, because the wrong reaction to a red here
#    is cheap and permanent.
# ---------------------------------------------------------------------------
cat <<'GUIDE'

VGLITE-CONFORMANCE: DRIFT — the board and the pre-registered expectation disagree.

Work out WHICH of the two changed before touching either file:
  * a verdict expected ok, got broken
        A NEW QUIRK, or a regression in OUR usage (did the case's path
        encoding, arena use or predicate change?). If it is a control
        (single-contour-rect, two-draws-ring, self-intersecting), treat it as
        invalidating the rows that depend on it, not as one bad cell.
  * a verdict expected broken, got ok
        The DRIVER MOVED UNDER US — an SDK re-vendor, a Series/CHIPID switch,
        a vg_lite_options.h flag — or our usage no longer asks the old
        question. This is NOT merely good news: the safe-usage workarounds two
        shipping compositors carry were built on that quirk, and they may now
        be unnecessary, or the wrong shape. Find the WHY, write it into the
        line's reason, and only then move the verdict.
  * repeat expected `unstable`
        The case is KNOWN to vary between boots and both readings are
        admissible; the UNSTABLE line above says which this run gave. That is
        a recorded property, not a pass. It never applies to a pixel verdict.
  * repeat same -> differs
        NONDETERMINISM, a finding in its own right even when the pixels are
        right. This tree has seen 7 boots produce 7 checksums. Do not average
        it away; re-boot and record how it varies.
  * a case appearing or vanishing
        The MATRIX moved. examples/display/vglite_conformance/run_qemu.sh's id
        list, the case table in vgc_cases_path.cpp and expected_silicon.txt
        must all agree — if only two of the three do, one of them is now
        measuring less than the others believe.
  * a pair's joint outcome is not admissible
        Read the admissible list printed above with its meanings. Landing
        outside it means the experiment's premises moved, which is a bigger
        finding than either arm alone.

DO NOT paste the transcript over the expectation. That is re-goldening a
checksum because the picture looked fine: the drift disappears and so does the
finding. Every change to expected_silicon.txt carries a reason — see its
header.
GUIDE
exit 1
