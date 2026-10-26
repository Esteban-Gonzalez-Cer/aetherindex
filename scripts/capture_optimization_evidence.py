#!/usr/bin/env python3
"""Collect completed optimization checks with source/artifact fingerprints.

This copies evidence; it does not run tests or infer success from elapsed time.
Large generated cluster arrays stay in the build, identified by size and SHA256.
"""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import re
import shutil
import subprocess
from datetime import datetime, timezone


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for block in iter(lambda: file.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def copy_file(source, destination):
    if not source.is_file():
        raise FileNotFoundError(f"Required evidence is missing: {source}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def cache_options(path):
    selected = {}
    for line in path.read_text().splitlines():
        if line.startswith(("AETHER_", "BUILD_TESTING:", "CMAKE_BUILD_TYPE:", "CMAKE_CXX_COMPILER:", "CMAKE_CXX_FLAGS")):
            if "=" in line:
                key, value = line.split("=", 1)
                selected[key] = value
    return selected


def test_summary(path):
    text = path.read_text()
    # CTest 4.4 omits the zero-failures clause; older versions include it.
    matches = list(re.finditer(r"(\d+)% tests passed(?:, (\d+) tests failed)? out of (\d+)", text))
    match = matches[-1] if matches else None
    if not match:
        raise ValueError(f"No completed CTest result: {path}")
    if match.group(1) != "100" or int(match.group(2) or 0) != 0:
        raise ValueError(f"CTest did not pass completely: {path}")
    return {"tests": int(match.group(3)), "failed": 0}


def collect(args):
    root = args.root.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    runs = {}
    for name, build, log in (
        ("debug", args.debug, "ctest-debug.log"),
        ("release", args.release, "ctest-final.log"),
        ("sanitizers_initial", args.sanitizers, "ctest-sanitizers.log"),
    ):
        build = build.resolve()
        runs[name] = test_summary(build / log)
        copy_file(build / log, output / name / log)
        runs[name]["cmake_options"] = cache_options(build / "CMakeCache.txt")
    rerun = args.sanitizers.resolve() / "ctest-sanitizers-packed.log"
    copy_file(args.sanitizers.resolve() / "LastTest-initial.log", output / "sanitizers_initial" / "LastTest.log")
    if rerun.exists():
        runs["sanitizers_affected_rerun"] = test_summary(rerun)
        copy_file(rerun, output / "sanitizers_final" / rerun.name)
    final_sanitizers = args.sanitizers.resolve() / "ctest-sanitizers-final.log"
    if final_sanitizers.exists():
        runs["sanitizers_complete_final"] = test_summary(final_sanitizers)
        copy_file(final_sanitizers, output / "sanitizers_final" / final_sanitizers.name)
    for name, build in (("debug", args.debug), ("release", args.release), ("sanitizers_final", args.sanitizers)):
        build = build.resolve()
        full_log = (build / "Testing/Temporary/LastTest.log" if name == "sanitizers_final" and final_sanitizers.exists()
                    else build / ("LastTest-packed.log" if name == "sanitizers_final" else "LastTest-full.log"))
        copy_file(full_log, output / name / "LastTest.log")
        closure_log = build / "ctest-query-lifetime.log"
        runs[name + "_query_lifetime"] = test_summary(closure_log)
        copy_file(closure_log, output / name / closure_log.name)
        copy_file(build / "CMakeFiles/aether_distance.dir/flags.make", output / name / "distance-flags.txt")
        copy_file(build / "CMakeFiles/aetherindex.dir/link.txt", output / name / "cli-link.txt")

    release = args.release.resolve()
    copy_file(release / "query-immutability-proof.json", output / "release" / "query-immutability-proof.json")
    for mode in ("topk", "memory", "clustered", "dbpedia", "assembly"):
        source = release / "benchmarks/evidence" / mode
        for file in source.iterdir():
            if file.suffix in (".json", ".csv", ".s"):
                copy_file(file, output / "benchmarks" / mode / file.name)
    initial = release / "initial-byte-visited"
    if initial.exists():
        for file in initial.rglob("*"):
            if file.is_file():
                copy_file(file, output / "initial-byte-visited" / file.relative_to(initial))
    intermediate = release / "packed-memory-intermediate"
    if intermediate.exists():
        for file in intermediate.iterdir():
            if file.is_file():
                copy_file(file, output / "packed-memory-intermediate" / file.name)

    for name in ("hnsw-recall-Release.csv", "hnsw-real-data-Release.csv", "real-data-Release.json"):
        copy_file(release / "tests" / name, output / "release" / name)
    for name in ("protocol.json", "kr1_1_vectorstore_report.csv", "kr1_2_hnsw_functional_report.csv",
                 "kr1_6_hnsw_recall_report.csv", "graph_checkpoints.csv", "query_results.csv",
                 "insertions.csv", "invalid_inputs.csv"):
        source = release / "tests/okr1-Release" / name
        if source.is_file():
            copy_file(source, output / "release/okr1" / name)

    copy_file(root / "benchmarks/optimization_protocol.json", output / "optimization_protocol.json")
    copy_file(root / "build-optimization-baseline/baseline_manifest.json", output / "baseline_manifest.json")
    for name in ("build.log", "cli-help.txt", "cli-search.txt"):
        copy_file(args.no_tests.resolve() / name, output / "no-tests" / name)

    sources = {}
    for directory in ("include", "src", "apps", "cmake", "tests", "benchmarks", "scripts"):
        for path in (root / directory).rglob("*"):
            if path.is_file() and path.suffix in (".hpp", ".cpp", ".cmake", ".py", ".json", ".txt"):
                sources[str(path.relative_to(root))] = sha256(path)
    sources["CMakeLists.txt"] = sha256(root / "CMakeLists.txt")
    binaries = {str(path.relative_to(root)): sha256(path) for path in (
        release / "aetherindex", release / "benchmarks/aether_optimization_benchmark",
        release / "benchmarks/aether_dbpedia_benchmark", args.sanitizers.resolve() / "aetherindex",
        args.no_tests.resolve() / "aetherindex", root / "build-optimization-baseline/build/aether_dbpedia_benchmark")}
    large_fixtures = {}
    for name in ("cluster_vectors.f32", "cluster_queries.f32"):
        path = release / "benchmarks/evidence/clustered" / name
        large_fixtures[name] = {"sha256": sha256(path), "bytes": path.stat().st_size,
                                "local_path": str(path), "format": "native-endian IEEE-754 FP32 row-major"}
    manifest = {"captured_utc": datetime.now(timezone.utc).isoformat(), "system": platform.platform(),
                "machine": platform.machine(), "python": platform.python_version(),
                "base_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
                "uncommitted_changes_included": True, "test_runs": runs,
                "source_sha256": dict(sorted(sources.items())), "binary_sha256": binaries,
                "large_fixture_fingerprints": large_fixtures,
                "dbpedia_manifest_sha256": sha256(root / "datasets/real/dbpedia/manifest.json"),
                "limitations": ["CA-2.3 PMU profiling not measured on macOS",
                                "Graph owned capacity is not process RSS; preserve all observed values"]}
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n")
    fingerprints = {str(path.relative_to(output)): sha256(path) for path in output.rglob("*")
                    if path.is_file() and path.name != "artifact_sha256.json"}
    (output / "artifact_sha256.json").write_text(json.dumps(dict(sorted(fingerprints.items())), indent=2) + "\n")
    print(json.dumps({"output": str(output), "artifacts": len(fingerprints), "test_runs": runs}, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--debug", type=Path, default=Path("build-optimization-debug"))
    parser.add_argument("--release", type=Path, default=Path("build-optimization-release"))
    parser.add_argument("--sanitizers", type=Path, default=Path("build-optimization-sanitizers"))
    parser.add_argument("--no-tests", type=Path, default=Path("build-optimization-no-tests"))
    parser.add_argument("--output", type=Path, default=Path("docs/reports/evidence/2026-10-19-optimization"))
    collect(parser.parse_args())
