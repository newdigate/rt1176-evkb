"""Shared verdict vocabulary.

Four levels, with a discipline attached to each:

  PASS  -- the check ran and the host satisfied it.
  FAIL  -- a violation. MUST carry a citation. Never invented from a
           threshold; if no clause can be cited, the finding is a WARN.
  WARN  -- behaviour the spec permits but which has a measurable cost. MUST
           carry consequence arithmetic, not an opinion.
  SKIP  -- the check could not run. MUST name the missing witness. A SKIP is
           never a quiet PASS; this is the single most likely way the tool
           lies.
"""
from dataclasses import dataclass, field

PASS = "PASS"
FAIL = "FAIL"
WARN = "WARN"
SKIP = "SKIP"
INVALID = "INVALID"

LEVELS = (PASS, FAIL, WARN, SKIP, INVALID)


@dataclass
class Verdict:
    rule_id: str
    level: str
    summary: str
    citation: str = ""
    consequence: str = ""
    missing_witness: str = ""
    evidence: dict = field(default_factory=dict)

    def __post_init__(self):
        if self.level not in LEVELS:
            raise ValueError(f"unknown level {self.level!r}")
        if self.level == FAIL and not self.citation:
            raise ValueError(f"{self.rule_id}: FAIL without a citation")
        if self.level == WARN and not self.consequence:
            raise ValueError(f"{self.rule_id}: WARN without consequence arithmetic")
        if self.level == SKIP and not self.missing_witness:
            raise ValueError(f"{self.rule_id}: SKIP without naming the missing witness")
