#!/usr/bin/env python3
"""Collect the Sprint 2 / OKR 1 evidence package from CTest builds.

Run each build first with JUnit output, for example:

    ctest --test-dir build-sprint2-release --output-on-failure \
          --output-junit ctest-junit.xml

then, from the repository root:

    python3 scripts/collect_sprint2_evidence.py --build release=build-sprint2-release \
        --build debug=build-sprint2-debug --build sanitizers=build-sprint2-sanitizers

The script never re-runs or relabels tests. It copies the KR1.3/KR1.5 reports written
by acceptance.* tests, converts the 22 KR1.4 RRF case files to CSV, and builds a
KR1.7 table of every registered CTest result per profile plus the per-member
reproduction rows, which stay PENDING until a team member records their own run.
"""

from __future__ import annotations

import argparse
import csv
import json
import platform
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MEMBERS = ("member-1", "member-2", "member-3", "member-4")


def git(*args: str) -> str:
    return subprocess.run(["git", *args], cwd=ROOT, check=True, capture_output=True,
                          text=True).stdout.strip()


def cache_value(build: Path, key: str) -> str:
    for line in (build / "CMakeCache.txt").read_text(encoding="utf-8").splitlines():
        if line.startswith(key + ":"):
            return line.split("=", 1)[1]
    return ""


def junit_results(build: Path) -> list[tuple[str, str, float]]:
    path = build / "ctest-junit.xml"
    if not path.is_file():
        raise SystemExit(f"Missing {path}; run ctest with --output-junit ctest-junit.xml")
    results = []
    for case in ET.parse(path).getroot().iter("testcase"):
        status = case.get("status", "")
        if case.find("failure") is not None or case.find("error") is not None:
            status = "fail"
        elif case.find("skipped") is not None:
            status = "skipped"
        results.append((case.get("name", ""), "PASS" if status in ("run", "") else status.upper(),
                        float(case.get("time", "0"))))
    return results


def config_dir(build: Path, sub: str) -> Path:
    base = build / "tests" / sub
    folders = [p for p in base.iterdir() if p.is_dir()] if base.is_dir() else []
    if len(folders) != 1:
        raise SystemExit(f"Expected one configuration folder in {base}")
    return folders[0]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build", action="append", required=True,
                        help="profile=build_dir; the first profile supplies KR reports")
    parser.add_argument("--reports", type=Path, default=ROOT / "evaluation" / "reports")
    args = parser.parse_args()
    builds = []
    for item in args.build:
        profile, _, directory = item.partition("=")
        builds.append((profile, (ROOT / directory).resolve()))
    reports = args.reports
    reports.mkdir(parents=True, exist_ok=True)

    commit = git("rev-parse", "HEAD")
    dirty = bool(git("status", "--porcelain"))
    environment = {
        "collected_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "git_commit": commit,
        "working_tree_dirty": dirty,
        "note": "Dirty tree: results describe HEAD plus uncommitted changes; commit, then re-run "
                "to obtain the final evaluated revision." if dirty else "Clean tree at commit.",
        "os": platform.platform(),
        "machine": platform.machine(),
        "python": platform.python_version(),
        "profiles": {},
    }

    acceptance_rows = []
    for profile, build in builds:
        environment["profiles"][profile] = {
            "build_dir": build.name,
            "build_type": cache_value(build, "CMAKE_BUILD_TYPE"),
            "compiler": cache_value(build, "CMAKE_CXX_COMPILER"),
            "cxx_flags": cache_value(build, "CMAKE_CXX_FLAGS"),
            "sanitizers": cache_value(build, "AETHER_ENABLE_SANITIZERS") or "OFF",
        }
        for name, result, seconds in junit_results(build):
            acceptance_rows.append(("KR1.7", profile, name, result, f"{seconds:.3f}"))

    primary = builds[0][1]
    okr = config_dir(primary, "sprint2-okr1")
    for name in ("kr1_3_bm25.csv", "kr1_3_summary.json", "kr1_5_hybrid.csv", "kr1_5_summary.json"):
        shutil.copyfile(okr / name, reports / name)

    rrf_dir = config_dir(primary, "rrf-evidence")
    with (reports / "kr1_4_rrf.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(["case_id", "passed", "checks", "error", "evidence_file"])
        cases = sorted(rrf_dir.glob("RRF-*.json"))
        for path in cases:
            data = json.loads(path.read_text(encoding="utf-8"))
            writer.writerow([data["case_id"], "PASS" if data["passed"] else "FAIL",
                             len(data.get("checks", [])), data.get("error", ""), path.name])
    rrf_passed = sum(json.loads(p.read_text(encoding="utf-8"))["passed"] for p in cases)

    with (reports / "kr1_7_acceptance.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(["kr", "profile_or_member", "check", "result", "seconds"])
        writer.writerows(acceptance_rows)
        for member in MEMBERS:
            writer.writerow(["KR1.7", member, "clean build + ctest + hybrid demo (README)",
                             "PENDING", ""])

    profiles_green = {
        profile: all(row[3] == "PASS" for row in acceptance_rows if row[1] == profile)
        for profile, _ in builds
    }
    kr13 = json.loads((reports / "kr1_3_summary.json").read_text(encoding="utf-8"))
    kr15 = json.loads((reports / "kr1_5_summary.json").read_text(encoding="utf-8"))
    summary = {
        "environment": environment,
        "KR1.3": {"metric": "BM25 HitRate@10", "target": ">= 0.90 over >= 20 queries",
                  "actual": f'{kr13["successes"]}/{kr13["queries"]} = {kr13["hit_rate_at_10"]:.4f}',
                  "result": kr13["result"], "evidence": "kr1_3_bm25.csv"},
        "KR1.4": {"metric": "RRF mandatory case pass rate", "target": "100% (22 cases)",
                  "actual": f"{rrf_passed}/{len(cases)}",
                  "result": "PASS" if cases and rrf_passed == len(cases) == 22 else "FAIL",
                  "evidence": "kr1_4_rrf.csv"},
        "KR1.5": {"metric": "successful end-to-end hybrid queries", "target": "20/20 or more",
                  "actual": f'{kr15["passed"]}/{kr15["queries"]}', "result": kr15["result"],
                  "evidence": "kr1_5_hybrid.csv"},
        "KR1.7": {"metric": "mandatory tests per profile + 4/4 member reproductions",
                  "automated_profiles_green": profiles_green,
                  "members_reproduced": "0/4 recorded",
                  "result": "PENDING (requires each team member's own run)",
                  "evidence": "kr1_7_acceptance.csv"},
    }
    (reports / "sprint2_okr1_summary.json").write_text(
        json.dumps(summary, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(json.dumps({k: v.get("result") for k, v in summary.items() if k.startswith("KR")}))
    return 0 if all(profiles_green.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
