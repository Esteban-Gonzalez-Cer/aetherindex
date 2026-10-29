#!/usr/bin/env python3
"""Consolidate optimization campaign results into one table.

    python3 scripts/summarize_optimization.py

Reads evaluation/reports/optimization/<phase>/<workload>/*_summary.csv, takes the median
of each metric over repetitions, and writes evaluation/reports/optimization/summary.csv
plus a Markdown table (summary.md). Numbers are copied, never adjusted.
"""

from __future__ import annotations

import csv
import statistics
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BASE = ROOT / "evaluation" / "reports" / "optimization"

METRICS = {
    "vectors": ["vectors", "dimension", "build_threads", "build_ms", "graph_bytes",
                "vector_bytes", "code_bytes", "code_build_ms", "recall_at_k", "p50_ms",
                "p95_ms", "p99_ms", "qps", "batch_qps", "filter_matches",
                "exact_filter_p50_ms"],
    "bm25": ["documents", "build_ms", "rss_mib", "p50_ms", "p95_ms", "p99_ms", "qps",
             "raw_posting_bytes", "compressed_posting_bytes"],
    "load": ["vectors", "dimension", "file_bytes", "load_ms", "mib_per_s"],
}


def median(values: list[str]) -> str:
    numbers = []
    for value in values:
        try:
            numbers.append(float(value))
        except ValueError:
            return values[0] if values else ""
    return f"{statistics.median(numbers):.6g}" if numbers else ""


def main() -> int:
    rows = []
    for phase_dir in sorted(p for p in BASE.iterdir() if p.is_dir()):
        for workload_dir in sorted(p for p in phase_dir.iterdir() if p.is_dir()):
            for summary in workload_dir.glob("*_summary.csv"):
                mode = summary.name.split("_summary")[0]
                with summary.open() as handle:
                    records = list(csv.DictReader(handle))
                if not records:
                    continue
                groups: dict[str, list[dict]] = {}
                key = "format" if mode == "load" else "channel" if mode == "hybrid" else None
                for record in records:
                    groups.setdefault(record.get(key, "") if key else "", []).append(record)
                for group, items in groups.items():
                    metrics = METRICS.get(mode) or [k for k in items[0] if k not in ("rep", key)]
                    row = {"phase": phase_dir.name, "workload": workload_dir.name,
                           "mode": mode, "variant": group, "repetitions": len(items)}
                    for metric in metrics:
                        if metric in items[0]:
                            row[metric] = median([item[metric] for item in items])
                    rows.append(row)
    columns = ["phase", "workload", "mode", "variant", "repetitions"]
    for row in rows:
        columns += [c for c in row if c not in columns]
    with (BASE / "summary.csv").open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=columns)
        writer.writeheader()
        writer.writerows(rows)
    lines = ["| " + " | ".join(columns) + " |", "|" + "---|" * len(columns)]
    for row in rows:
        lines.append("| " + " | ".join(str(row.get(c, "")) for c in columns) + " |")
    (BASE / "summary.md").write_text("\n".join(lines) + "\n")
    print(f"{len(rows)} rows -> {BASE / 'summary.csv'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
