#!/usr/bin/env python3
"""Independently recompute acceptance metrics from retained CSV/JSON evidence."""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import re
import statistics


def check(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_json(path):
    return json.loads(path.read_text())


def rows(path):
    with path.open(newline="") as file:
        return list(csv.DictReader(file))


def close(a, b):
    check(math.isclose(a, b, rel_tol=1e-12, abs_tol=1e-12), f"Metric mismatch: {a} vs {b}")


def analyze(root, source_root):
    artifact_hashes = read_json(root / "artifact_sha256.json")
    for name, expected in artifact_hashes.items():
        check(digest(root / name) == expected, f"Artifact fingerprint mismatch: {name}")
    manifest = read_json(root / "manifest.json")
    for run in manifest["test_runs"].values():
        check(run["failed"] == 0, "A recorded CTest run failed.")
    check(manifest["test_runs"]["sanitizers_complete_final"]["tests"] == 21,
          "The final complete sanitizer suite must finish before acceptance is reported.")
    sanitizer_log = (root / "sanitizers_final/LastTest.log").read_text()
    check(not re.search(r"ERROR: AddressSanitizer|runtime error:|WARNING: (?:AddressSanitizer|UndefinedBehaviorSanitizer)", sanitizer_log),
          "Sanitizer diagnostics found.")
    if source_root:
        for name, expected in manifest["source_sha256"].items():
            check(digest(source_root / name) == expected, f"Current source differs from measured evidence: {name}")

    data = rows(root / "benchmarks/topk/topk_samples.csv")
    check(len(data) == 50 and len({(x['cycle'], x['query_id']) for x in data}) == 50,
          "TopK requires all 50 paired samples.")
    sort_mean = statistics.mean(float(x["full_sort_ms"]) for x in data)
    heap_mean = statistics.mean(float(x["max_heap_ms"]) for x in data)
    topk_reduction = 1 - heap_mean / sort_mean
    topk = read_json(root / "benchmarks/topk/topk.json")
    close(sort_mean, topk["full_sort_mean_ms"]); close(heap_mean, topk["heap_mean_ms"])
    close(topk_reduction, topk["relative_time_reduction"])

    memory = read_json(root / "benchmarks/memory/memory.json")
    total = sum(memory[key] for key in ("layer0_allocated_bytes", "upper_allocated_bytes", "metadata_allocated_bytes", "object_bytes"))
    check(total == memory["graph_allocated_bytes"] and memory["layer0_allocated_bytes"] == 13200000,
          "Graph memory accounting is inconsistent.")
    check(memory["global_buffer_blocks"] == 4, "Graph must own four global buffers.")
    rss_observations = [{"run": "final", "delta_bytes": memory["rss_delta_bytes"]}]
    for name, path in (("byte_visited_initial", "initial-byte-visited/memory/memory.json"),
                       ("packed_visited_intermediate", "packed-memory-intermediate/memory.json")):
        previous = read_json(root / path)
        rss_observations.append({"run": name, "delta_bytes": previous["rss_delta_bytes"]})

    data = rows(root / "benchmarks/clustered/clustered_queries.csv")
    check(len(data) == 1000 and {int(x["query_id"]) for x in data} == set(range(1000)),
          "Clustered evidence must contain each held-out query exactly once.")
    simple, heuristic = [], []
    for row in data:
        ids = {key: list(map(int, row[key].split())) for key in ("exact_ids", "simple_ids", "heuristic_ids")}
        for values in ids.values():
            check(len(values) == len(set(values)) == 10, "A clustered result is incomplete or duplicated.")
            check(all(10 <= x <= 70003 and (x - 10) % 7 == 0 for x in values), "Unknown fixture ID.")
        a = len(set(ids["exact_ids"]) & set(ids["simple_ids"])) / 10
        b = len(set(ids["exact_ids"]) & set(ids["heuristic_ids"])) / 10
        close(a, float(row["simple_recall"])); close(b, float(row["heuristic_recall"]))
        simple.append(a); heuristic.append(b)
    recall_gain = statistics.mean(heuristic) - statistics.mean(simple)
    cluster = read_json(root / "benchmarks/clustered/clustered.json")
    close(statistics.mean(simple), cluster["simple_mean_recall"])
    close(statistics.mean(heuristic), cluster["heuristic_mean_recall"])
    close(recall_gain, cluster["absolute_recall_gain"])

    data = rows(root / "benchmarks/dbpedia/dbpedia_samples.csv")
    check(len(data) == 1000 and all(int(x["query_id"]) == i % 20 for i, x in enumerate(data)),
          "DBpedia must have 1000 samples cycling 20 distinct queries.")
    times = sorted(float(x["query_ms"]) for x in data)
    check(all(math.isfinite(x) and x >= 0 for x in times), "Invalid query timing.")
    p99 = times[math.ceil(.99 * len(times)) - 1]
    real = read_json(root / "benchmarks/dbpedia/dbpedia.json")
    close(p99, real["p99_query_ms"])
    close(statistics.mean(float(x["recall_at_10"]) for x in data), real["mean_recall_at_10"])

    assembly = read_json(root / "benchmarks/assembly/assembly.json")
    check(assembly["packed_fp32_fma_count"] > 0 and not assembly["float_to_double_instructions"]
          and not assembly["sqrt_divide_instructions"], "Distance assembly contract failed.")
    parity_log = (root / "release/LastTest.log").read_text()
    match = re.search(r"CA-1.2: (\d+) comparisons; max absolute error=([\deE.+-]+)", parity_log)
    check(match is not None, "Missing independent numeric parity check.")
    numeric_error = float(match.group(2))

    fixture_checks = {}
    for name, info in manifest["large_fixture_fingerprints"].items():
        local = Path(info["local_path"])
        if local.exists():
            check(local.stat().st_size == info["bytes"] and digest(local) == info["sha256"], "Generated fixture fingerprint changed.")
            initial = read_json(root / "initial-byte-visited/fixture_sha256.json")[name]
            check(initial["sha256"] == info["sha256"], "Cluster fixture changed between implementation revisions.")
            fixture_checks[name] = "identical_to_initial_frozen_fixture"
        else:
            fixture_checks[name] = "local_large_fixture_unavailable_hash_retained"

    results = {
        "CA-1.1": {"pass": not assembly["sqrt_divide_instructions"]},
        "CA-1.2": {"pass": numeric_error < 1e-6, "comparisons": int(match.group(1)), "max_absolute_error_printed": numeric_error},
        "CA-1.3": {"pass": topk_reduction > .60, "relative_time_reduction": topk_reduction},
        "CA-1.4": {"pass": assembly["packed_fp32_fma_count"] > 0 and not assembly["float_to_double_instructions"]},
        "CA-2.1": {"pass": memory["global_buffer_blocks"] == 4, "persistent_global_buffers": 4},
        "CA-2.2": {"structural_budget_pass": total <= 14500000, "owned_bytes": total,
                   "rss_observations": rss_observations, "rss_bound_demonstrated_consistently": all(x["delta_bytes"] <= 14500000 for x in rss_observations),
                   "scope": "Owned capacity plus object differs from process RSS; no stable process RSS bound claimed."},
        "CA-2.3": {"status": "not_measured", "reason": "Linux perf PMU measurements unavailable on this macOS host"},
        "CA-2.4": {"pass": recall_gain >= .05, "absolute_recall_gain": recall_gain},
        "CA-2.5": {"pass": p99 < 1.5, "p99_ms": p99, "distinct_queries": 20, "measured_searches": 1000},
        "CA-2.6": {"pass": True, "final_complete_sanitizer_tests": 21, "diagnostics": 0},
    }
    for key, value in results.items():
        if "pass" in value:
            check(value["pass"], f"Acceptance threshold unmet: {key}")
    check(results["CA-2.2"]["structural_budget_pass"], "Graph structural budget exceeded.")
    summary = {"criteria": results, "verified_artifacts": len(artifact_hashes), "fixture_checks": fixture_checks,
               "all_step2_criteria_approved": False,
               "remaining": ["CA-2.3 real Linux LLC counters", "No consistent raw process RSS bound <=14.50 MB established"],
               "team_review_and_pr_approval_separate": True}
    target = root / "validation_summary.json"
    target.write_text(json.dumps(summary, indent=2) + "\n")
    artifact_hashes[target.name] = digest(target)
    (root / "artifact_sha256.json").write_text(json.dumps(dict(sorted(artifact_hashes.items())), indent=2) + "\n")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence", type=Path, default=Path("docs/reports/evidence/2026-10-19-optimization"))
    parser.add_argument("--verify-current-source", type=Path)
    args = parser.parse_args()
    analyze(args.evidence.resolve(), args.verify_current_source.resolve() if args.verify_current_source else None)
