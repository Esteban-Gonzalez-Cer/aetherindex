#!/usr/bin/env python3
"""Build, run the full CTest suite, and collect Sprint 1 OKR1 evidence.

Requires Python 3.10+, CMake and an already configured build directory.
Uses only the Python standard library; CTest itself has no Python dependency.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import platform
import shutil
import subprocess
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def sources() -> dict[str, str]:
    paths = [ROOT / "CMakeLists.txt"]
    for folder in ("cmake", "include", "src", "apps", "tests", "scripts"):
        paths.extend(path for path in (ROOT / folder).rglob("*") if path.is_file()
                     and path.suffix in (".hpp", ".h", ".cpp", ".cmake", ".txt", ".py"))
    return {str(path.relative_to(ROOT)): sha256(path) for path in sorted(paths)}


def git(*args: str) -> str:
    return subprocess.check_output(["git", *args], cwd=ROOT, text=True).strip()


def run(command: list[str], output: Path) -> None:
    print("Running: " + " ".join(command), flush=True)
    with output.open("w", encoding="utf-8") as log:
        log.write("Command: " + " ".join(command) + "\n")
        log.flush()
        result = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
    print(output.read_text(encoding="utf-8"), flush=True)
    if result.returncode:
        raise RuntimeError(f"Command failed ({result.returncode}); see {output}")


def rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--config", choices=("Debug", "Release"), default="Release")
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    build = args.build_dir.resolve()
    output = args.output_dir.resolve()
    if not (build / "CMakeCache.txt").is_file():
        parser.error("Configure the build directory with CMake first")
    if build == output or build in output.parents:
        parser.error("Keep collected evidence outside the build directory")
    output.mkdir(parents=True, exist_ok=True)
    started = datetime.now(timezone.utc).isoformat()
    source_hashes = sources()
    commit = git("rev-parse", "HEAD")
    working_tree = git("status", "--short")
    run(["cmake", "--build", str(build), "--config", args.config, "--parallel", str(args.jobs)],
        output / "build.log")
    run(["ctest", "--test-dir", str(build), "-C", args.config, "--output-on-failure",
         "--no-tests=error", "--parallel", str(args.jobs)], output / "ctest.log")
    if source_hashes != sources() or commit != git("rev-parse", "HEAD"):
        raise RuntimeError("Sources/HEAD changed during validation; collected evidence is not valid")

    evidence = build / "tests" / f"okr1-{args.config}"
    storage = rows(evidence / "kr1_1_vectorstore_report.csv")
    functional = rows(evidence / "kr1_2_hnsw_functional_report.csv")
    recall = rows(evidence / "kr1_6_hnsw_recall_report.csv")
    if len(storage) != 1 or len(functional) != 1 or len(recall) != 100:
        raise RuntimeError("Incomplete OKR1 reports")
    for report in (storage, functional, recall):
        if any(row["status"] != "Cumplido" for row in report):
            raise RuntimeError("An OKR1 acceptance indicator failed")
    total_matches = 0
    for row in recall:
        exact, approximate = row["exact_ids"].split(), row["hnsw_ids"].split()
        matches = len(set(exact) & set(approximate))
        if len(set(exact)) != 10 or len(set(approximate)) != 10 or matches != int(row["matches"]):
            raise RuntimeError("Recall report contains invalid neighbor sets/matches")
        if abs(float(row["recall_at_10"]) - matches / 10) > 1e-12:
            raise RuntimeError("Per-query recall is inconsistent")
        total_matches += matches
    mean_recall = total_matches / 1000
    if mean_recall < 0.85 or any(abs(float(row["mean_recall_at_10"]) - mean_recall) > 1e-12
                                for row in recall):
        raise RuntimeError("Mean recall is inconsistent or below acceptance threshold")

    artifact_hashes = {}
    for path in sorted(evidence.iterdir()):
        if path.is_file():
            shutil.copy2(path, output / path.name)
            artifact_hashes[path.name] = sha256(path)
    shutil.copy2(build / "Testing" / "Temporary" / "LastTest.log", output / "LastTest.log")
    shutil.copy2(build / "CMakeCache.txt", output / "CMakeCache.txt")
    fixture_hashes = {str(path.relative_to(evidence)): sha256(path)
                      for path in sorted((evidence / "fixtures").rglob("*.csv"))}
    metadata = {
        "started_utc": started,
        "finished_utc": datetime.now(timezone.utc).isoformat(),
        "base_commit": commit,
        "working_tree_at_start": working_tree,
        "source_sha256": source_hashes,
        "fixture_sha256": fixture_hashes,
        "artifact_sha256": artifact_hashes,
        "build_directory": str(build),
        "configuration": args.config,
        "platform": platform.platform(),
        "machine": platform.machine(),
        "python": platform.python_version(),
        "cmake": subprocess.check_output(["cmake", "--version"], text=True).splitlines()[0],
        "all_ctest_tests_passed": True,
        "fulfilled_planned_krs": 3,
        "planned_krs": 3,
        "fulfillment_percent": 100,
        "mean_recall_at_10": mean_recall,
        "total_matches": total_matches,
        "human_sprint_review": "Pending; not established by automated tests",
    }
    (output / "run_metadata.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    print(f"Evidence: {output}; planned KRs=3/3; meanRecall@10={mean_recall:.3f}")


if __name__ == "__main__":
    main()
