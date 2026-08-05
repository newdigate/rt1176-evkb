#!/usr/bin/env python3
"""Assert a validator report matches a corpus case's documented expectations.

Rules the case does not mention are ignored: a case pins the defect it was
chosen for, not the whole report, so an unrelated rule changing level does not
turn into spurious corpus churn.

Every mismatch is reported, not just the first. Each corpus case costs a build,
a flash and a multi-minute capture, so a checker that stopped early would spend
a bench run per defect discovered.
"""
import json
import sys


def check(case, report_json):
    by_id = {v["rule_id"]: v for v in report_json["verdicts"]}
    msgs = []
    ok = True

    for rid, expected in case["expect"].items():
        if rid not in by_id:
            # Reached when the report is a capture-level INVALID: no rule ran
            # at all. Saying so beats "expected FAIL, got None", because the
            # remedy is a new capture rather than a look at the host.
            ok = False
            msgs.append(f"{rid}: expected {expected}, but the rule did not run")
            continue
        got = by_id[rid]["level"]
        if got != expected:
            ok = False
            msgs.append(f"{rid}: expected {expected}, got {got}")

    for rid, fields in case.get("expect_evidence", {}).items():
        if rid not in by_id:
            ok = False
            msgs.append(f"{rid}: expected evidence but the rule did not run")
            continue
        ev = by_id[rid].get("evidence", {})
        for key, expected in fields.items():
            # Presence, then equality -- never truthiness. The corpus's
            # central evidence check is `feedback_polls == 0`, and a
            # truthiness test would make it unfireable.
            if key not in ev:
                ok = False
                msgs.append(f"{rid}.{key}: missing from evidence")
            elif ev[key] != expected:
                ok = False
                msgs.append(f"{rid}.{key}: expected {expected}, got {ev[key]}")

    return ok, msgs


def main():
    if len(sys.argv) != 4:
        print("usage: check_case.py cases.json <case-id> report.json",
              file=sys.stderr)
        return 2
    cases = json.load(open(sys.argv[1]))["cases"]
    case = next((c for c in cases if c["id"] == sys.argv[2]), None)
    if case is None:
        print(f"no such case: {sys.argv[2]}", file=sys.stderr)
        return 2
    ok, msgs = check(case, json.load(open(sys.argv[3])))
    for m in msgs:
        print(f"  MISMATCH {m}")
    print(f"{case['id']}: {'OK' if ok else 'MISMATCH'}")
    if not ok:
        print("\nDo NOT edit the expectation to make this pass. The "
              "expectation came from an independent measurement recorded in "
              "transcript_hw_evkb.txt. Either the validator is wrong, or the "
              "bench differs from the run that produced the ground truth -- "
              "find out which.")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
