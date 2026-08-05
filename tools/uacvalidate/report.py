"""Report rendering and exit-code policy.

Exit codes: 0 clean (warnings and skips do not fail a run), 1 at least one
FAIL, 2 the capture itself is unusable. Separating 2 from 1 matters: an
INVALID run says nothing about the host, and treating it as a failure of the
host would be a false accusation -- the worst output this tool can produce.
The INVALID check therefore comes first, so a capture that is unusable *and*
happens to contain a FAIL still reports 2: whatever the rules concluded was
computed from counters known to be lower bounds.

Warnings and skips exit 0 on purpose. A WARN is spec-permitted behaviour
carrying a measured cost, not a violation; a SKIP is a check that could not
run. Failing the run on either would blur the line that the citation
discipline in verdict.py exists to keep sharp.

Rendering prints every supporting field the verdict carries. verdict.py
refuses to construct an uncited FAIL, a consequence-free WARN or a witnessless
SKIP; a renderer that then dropped those fields would undo the whole
discipline, because to the reader of the page an omitted citation and an
absent one are the same thing.
"""
from verdict import PASS, FAIL, WARN, SKIP, INVALID


def exit_code(verdicts):
    """0 clean, 1 the host failed a rule, 2 the capture cannot be judged.

    INVALID is tested before FAIL deliberately -- see the module docstring.
    """
    if any(v.level == INVALID for v in verdicts):
        return 2
    if any(v.level == FAIL for v in verdicts):
        return 1
    return 0


def render(header, verdicts, metrics=()):
    """The report as text.

    `header` is read with .get throughout: the capture-level INVALID paths
    render before the header is fully populated, and a KeyError there would
    replace the reason the capture was rejected with a traceback.

    `metrics` is an ordered sequence of (label, value) strings printed below
    the rules and above the summary. They carry no level, no citation, and no
    weight in exit_code -- which does not even see them. That separation is
    the point: they are observations a reader interprets, not findings the
    tool is prepared to defend with a clause.
    """
    lines = []
    lines.append("UAC host conformance report")
    lines.append("=" * 60)
    lines.append(f"host under test : {header.get('host_note', '')}")
    lines.append(f"capture         : {header.get('vcd', '')}")
    lines.append(f"duration        : {header.get('duration_s', 0):.1f} s")
    lines.append(f"xscope missing  : {header.get('missing_marks', 0)}")
    # Phantom timestamp wraps the reader removed (wireformat.XSCOPE_WRAP_S).
    # Zero on a clean capture; non-zero says every time-derived figure below
    # is on REPAIRED time, and the raw file's own span disagrees with it.
    if header.get("wrap_repairs"):
        lines.append(f"wrap repairs    : {header['wrap_repairs']} "
                     f"phantom 42.95 s period(s) removed from the time base")
    lines.append(f"observer build  : {header.get('observer_sha', 'unknown')}")
    lines.append("")

    for v in verdicts:
        lines.append(f"[{v.level:<7}] {v.rule_id:<5} {v.summary}")
        if v.citation:
            lines.append(f"           cites: {v.citation}")
        if v.consequence:
            lines.append(f"           consequence: {v.consequence}")
        if v.missing_witness:
            lines.append(f"           missing: {v.missing_witness}")
        if v.evidence:
            ev = ", ".join(f"{k}={val}" for k, val in v.evidence.items())
            lines.append(f"           evidence: {ev}")
        lines.append("")

    if metrics:
        lines.append("metrics (INFO -- no verdict, no effect on exit code)")
        width = max(len(str(k)) for k, _ in metrics)
        for k, v in metrics:
            lines.append(f"  {str(k):<{width}} : {v}")
        lines.append("")

    counts = {lvl: sum(1 for v in verdicts if v.level == lvl)
              for lvl in (PASS, FAIL, WARN, SKIP, INVALID)}
    # PASS and FAIL are always printed, even at zero: "0 FAIL" is a claim a
    # reader can rely on, whereas an omitted FAIL count is an ambiguity
    # between "none" and "not reported". WARN and SKIP appear only when
    # non-zero, because their absence carries no such weight.
    summary = ", ".join(f"{counts[l]} {l}" for l in (PASS, FAIL, WARN, SKIP)
                        if counts[l] or l in (PASS, FAIL))
    if counts[INVALID]:
        summary += f", {counts[INVALID]} INVALID"
    lines.append("-" * 60)
    lines.append(summary)
    return "\n".join(lines)
