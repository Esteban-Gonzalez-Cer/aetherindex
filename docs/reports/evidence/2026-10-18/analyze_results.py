#!/usr/bin/env python3
"""Recompute report summaries from the saved evidence; optionally render its figure.

Python standard library is sufficient for JSON/CSV audit. Matplotlib is optional
and used only for the publication figure, never for CTest or the search engine.
"""

import csv
import hashlib
import json
import re
import statistics
from collections import Counter, defaultdict
from pathlib import Path

EVIDENCE = Path(__file__).resolve().parent
ROOT = EVIDENCE.parents[3]


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load_csv(path):
    with path.open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream))


def aggregate(filename, separator):
    groups = defaultdict(list)
    for row in load_csv(EVIDENCE / "release" / filename):
        exact = row["exact_ids"].split(separator)
        approximate = row["hnsw_ids"].split(separator)
        assert len(exact) == len(approximate) == len(set(approximate)) == 10
        matches = len(set(exact) & set(approximate))
        assert abs(matches / 10 - float(row["recall_at_10"])) < 1e-12
        groups[(row.get("metric", "cosine"), int(row["ef_search"]))].append((row, matches))
    summaries = []
    for (metric, width), values in sorted(groups.items()):
        rows = [value[0] for value in values]
        total = sum(value[1] for value in values)
        times = [float(row.get("hnsw_query_ms", row.get("query_ms"))) for row in rows]
        item = {"metric": metric, "ef_search": width, "queries": len(rows), "matches": total,
                "mean_recall_at_10": total / (10 * len(rows)),
                "minimum_recall_at_10": min(value[1] for value in values) / 10,
                "observed_mean_query_ms": statistics.mean(times)}
        if "exact_query_ms" in rows[0]:
            item["observed_mean_exact_ms"] = statistics.mean(float(row["exact_query_ms"]) for row in rows)
        summaries.append(item)
    return summaries


def main():
    summary = {"evidence_date": "2026-10-18", "builds": {}}
    reference = None
    for label in ("debug", "release", "sanitizers"):
        folder = EVIDENCE / label
        metadata = json.loads((folder / "run_metadata.json").read_text())
        for filename, expected in metadata["source_sha256"].items():
            assert sha256(ROOT / filename) == expected, filename
        for filename, expected in metadata["artifact_sha256"].items():
            assert sha256(folder / filename) == expected, filename
        if reference is None:
            reference = metadata
        else:
            assert metadata["source_sha256"] == reference["source_sha256"]
            assert metadata["fixture_sha256"] == reference["fixture_sha256"]
        log = (folder / "ctest.log").read_text()
        passed = int(re.search(r"100% tests passed out of (\d+)", log)[1])
        seconds = float(re.search(r"Total Test time \(real\) =\s+([0-9.]+) sec", log)[1])
        assert not re.search(r"ERROR: AddressSanitizer|runtime error:|SUMMARY: AddressSanitizer",
                             (folder / "LastTest.log").read_text())
        recalls = load_csv(folder / "kr1_6_hnsw_recall_report.csv")
        matches = sum(len(set(row["exact_ids"].split()) & set(row["hnsw_ids"].split())) for row in recalls)
        assert len(recalls) == 100 and matches == 990
        summary["builds"][label] = {"passed_tests": passed, "ctest_seconds": seconds,
                                   "matches": matches, "mean_recall_at_10": matches / 1000}
    summary["base_commit"] = reference["base_commit"]
    summary["official_recall_histogram"] = dict(Counter(row["matches"] for row in recalls))
    summary["development_sweep"] = aggregate("development_recall.csv", " ")
    summary["dbpedia"] = aggregate("dbpedia_recall.csv", "|")
    oracle = json.loads((EVIDENCE / "release/numpy_oracle.json").read_text())
    assert oracle["passed_queries"] == 40 and oracle["checked_neighbors"] == 4000
    assert oracle["max_absolute_distance_error"] < 1e-5
    summary["oracle"] = {key: oracle[key] for key in
                         ("passed_queries", "checked_neighbors", "max_absolute_distance_error", "summary")}
    graph_log = (EVIDENCE / "release/LastTest.log").read_text()
    summary["dbpedia_build_ms"] = float(re.search(
        r"Graph validation: valid; nodes: 10000; max level: 4; build time \(ms\): ([0-9.]+)", graph_log)[1])
    summary["source_and_artifact_hashes_validated"] = True
    summary["identical_official_fixtures_across_builds"] = True
    (EVIDENCE / "results_summary.json").write_text(json.dumps(summary, indent=2) + "\n")

    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("Evidence audit passed; Matplotlib unavailable, figure generation skipped.")
        return
    figure, axes = plt.subplots(1, 2, figsize=(10.8, 4.2), layout="constrained")
    colors = {"cosine": "#1565a3", "squared-euclidean": "#a94f10"}
    for metric, label in (("cosine", "Cosine"), ("squared-euclidean", "Euclidiana al cuadrado")):
        points = [row for row in summary["development_sweep"] if row["metric"] == metric]
        axes[0].plot([row["ef_search"] for row in points], [row["mean_recall_at_10"] for row in points],
                     marker="o", label=label, color=colors[metric], linewidth=2)
    points = summary["dbpedia"]
    axes[1].plot([row["ef_search"] for row in points], [row["mean_recall_at_10"] for row in points],
                 marker="o", label="Cosine", color=colors["cosine"], linewidth=2)
    for row in points:
        axes[1].annotate(f"{row['mean_recall_at_10']:.3f}", (row["ef_search"], row["mean_recall_at_10"]),
                         xytext=(0, -18), textcoords="offset points", ha="center", fontsize=10)
    for axis, title, ticks in ((axes[0], "Desarrollo: N=1,000 · D=128 · Q=100", [10, 30, 50, 100]),
                               (axes[1], "DBpedia: N=10,000 · D=1,024 · Q=20", [50, 100])):
        axis.axhline(0.85, color="#67727e", linestyle="--", linewidth=1, label="Referencia: 0.85")
        axis.set(title=title, xlabel="efSearch", ylabel="Mean Recall@10", ylim=(0.60, 1.015), xticks=ticks)
        axis.spines[["top", "right"]].set_visible(False)
        axis.grid(axis="y", alpha=0.18)
        axis.legend(loc="lower right", fontsize=8, frameon=False)
    assets = EVIDENCE.parents[1] / "assets"
    assets.mkdir(exist_ok=True)
    figure.savefig(assets / "recall_vs_efsearch.svg")
    figure.savefig(assets / "recall_vs_efsearch.png", dpi=160)
    plt.close(figure)
    print(json.dumps({"builds": summary["builds"], "dbpedia": summary["dbpedia"],
                      "oracle_error": oracle["max_absolute_distance_error"]}, indent=2))


if __name__ == "__main__":
    main()
