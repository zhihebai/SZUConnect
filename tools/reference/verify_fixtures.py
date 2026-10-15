#!/usr/bin/env python3
"""Run every shared fixture against the executable Python references.

Exit status 0 only when:
  * every recovery scenario passes under the shared contract,
  * "legacy": "bug" cases fail under the comparison model, "preserved" cases
    pass under both, and "changed" cases pass under the shared contract
    (the comparison result is informational),
  * the SRun golden vectors, website-address cases, route-classification
    cases, proxy-environment cases, compatibility-diagnosis cases and status
    fixtures agree with their Python references (when those files exist).

Usage:  python3 tools/reference/verify_fixtures.py [--verbose]
"""
import json
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(HERE))

import recovery_reference  # noqa: E402

VERBOSE = "--verbose" in sys.argv


def say(line):
    print(line, flush=True)


def check_recovery():
    data = json.loads((ROOT / "shared/fixtures/auto-recovery-scenarios.json").read_text("utf-8"))
    problems = 0
    for scenario in data["scenarios"]:
        fixed = recovery_reference.run_scenario(scenario, legacy=False)
        legacy = recovery_reference.run_scenario(scenario, legacy=True)
        kind = scenario.get("legacy", "preserved")
        status_fixed = "PASS" if not fixed else "FAIL"
        if kind == "bug":
            status_legacy = "REPRODUCED" if legacy else "NOT-REPRODUCED"
            ok = (not fixed) and bool(legacy)
        elif kind == "preserved":
            status_legacy = "PASS" if not legacy else "REGRESSED"
            ok = not fixed and not legacy
        else:
            status_legacy = "n/a(changed)" if legacy else "PASS"
            ok = not fixed
        say(f"[recovery] {scenario['id']:7s} current={status_fixed:4s} comparison={status_legacy:14s} {scenario['title']}")
        if VERBOSE or not ok:
            for line in fixed:
                say(f"      current {line}")
            for line in legacy:
                say(f"      comparison {line}")
        elif kind == "bug":
            for line in legacy[:3]:
                say(f"      comparison reproduction: {line}")
        if not ok:
            problems += 1
    return problems, len(data["scenarios"])


def check_optional(name, module_name, function_name):
    path = ROOT / "shared/fixtures" / name
    if not path.exists() and (ROOT / "shared" / name).exists():
        path = ROOT / "shared" / name
    if not path.exists():
        say(f"[{name}] missing (skipped)")
        return 0, 0
    module = __import__(module_name)
    failures, total = getattr(module, function_name)(json.loads(path.read_text("utf-8")))
    for line in failures:
        say(f"[{name}] FAIL {line}")
    say(f"[{name}] {total - len(failures)}/{total} cases agree")
    return len(failures), total


def main():
    total_problems = 0
    total_cases = 0
    problems, cases = check_recovery()
    total_problems += problems
    total_cases += cases
    for name, module, function in (
        ("protocol-golden.json", "srun_reference", "verify_golden"),
        ("website-address-cases.json", "website_reference", "verify_cases"),
        ("route-classification-cases.json", "route_reference", "verify_cases"),
        ("proxy-environment-cases.json", "environment_reference", "verify_environment_cases"),
        ("compat-diagnosis-cases.json", "environment_reference", "verify_diagnosis_cases"),
        ("status-fixtures.json", "status_reference", "verify_cases"),
        ("rejection-classification-cases.json", "srun_reference", "verify_rejections"),
    ):
        problems, cases = check_optional(name, module, function)
        total_problems += problems
        total_cases += cases
    import memory_reference
    _total, _bad = memory_reference.verify(pathlib.Path(__file__).resolve().parents[2])
    total_problems += _bad
    total_cases += _total
    say(f"TOTAL cases={total_cases} problems={total_problems}")
    return 1 if total_problems else 0


if __name__ == "__main__":
    sys.exit(main())
