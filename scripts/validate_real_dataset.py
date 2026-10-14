#!/usr/bin/env python3
"""Validate the actual CLI against an independent NumPy float64 exhaustive oracle.

No downloads are performed. Measured query times describe this local run;
they are not production latency guarantees or embedding-relevance judgments.
"""

import argparse
import hashlib
import json
import math
from pathlib import Path
import platform
import re
import statistics
import subprocess
import sys
import time

import numpy as np


ROW = re.compile(r"^\|\s+(\d+)\s+\|\s+(\d+)\s+\|\s+(\d+\.\d{9})\s+\|$", re.MULTILINE)
TIMER = re.compile(r"^Query time \(ms\): (\d+\.\d{6})$", re.MULTILINE)


def check(condition, message):
    if not condition:
        raise ValueError(message)


def validate(args):
    root = args.data_dir.resolve()
    manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
    for relative, expected_hash in manifest["sha256"].items():
        digest = hashlib.sha256()
        with (root / relative).open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
        check(digest.hexdigest() == expected_hash, f"Dataset checksum mismatch: {relative}")
    articles = [json.loads(line) for line in (root / "articles.jsonl").read_text(encoding="utf-8").splitlines()]
    queries = json.loads((root / "queries.json").read_text(encoding="utf-8"))
    # The engine's stored values are float32. All reference arithmetic is float64.
    loaded = np.loadtxt(root / "vectors.csv", delimiter=",", dtype=np.float64, ndmin=2)
    ids = loaded[:, 0].astype(np.uint32)
    data = loaded[:, 1:].astype(np.float32).astype(np.float64)
    check(data.shape == (manifest["vectors"], manifest["dimension"]), "Incorrect real dataset shape")
    check(len(articles) == len(data) and len(queries) == manifest["queries"], "Incorrect metadata count")
    check(len(set(map(int, ids))) == len(ids), "Duplicate vector IDs")
    check(np.array_equal(ids, [article["vector_id"] for article in articles]), "ID metadata mismatch")
    check(np.isfinite(data).all(), "Nonfinite dataset component")
    norms = np.linalg.norm(data, axis=1)
    check((norms > 0).all(), "Zero-norm dataset embedding")
    source_ids = {article["source_id"] for article in articles}
    index_by_id = {int(vector_id): index for index, vector_id in enumerate(ids)}
    count = min(args.k, len(ids))
    observations = []
    maximum_error = 0.0
    for metric in ("cosine", "squared-euclidean"):
        for query_info in queries:
            check(query_info["source_id"] not in source_ids, "Query article leaked into the corpus")
            query = np.loadtxt(root / query_info["file"], delimiter=",", dtype=np.float32).astype(np.float64)
            check(query.shape == (manifest["dimension"],), "Incorrect query shape")
            check(np.isfinite(query).all() and np.linalg.norm(query) > 0, "Invalid real query")
            check(not np.any(np.all(data == query, axis=1)), "Query embedding leaked into the corpus")
            if metric == "cosine":
                similarity = np.sum(data * query, axis=1) / (norms * np.linalg.norm(query))
                reference = 1.0 - np.clip(similarity, -1.0, 1.0)
            else:
                reference = np.sum(np.square(data - query), axis=1)
            rounded = reference.astype(np.float32)
            expected_ranks = np.sort(rounded)[:count]
            command = [str(args.cli.resolve()), "search", "--dataset", str(root / "vectors.csv"),
                       "--query", str(root / query_info["file"]), "--algorithm", "brute-force",
                       "--metric", metric, "--k", str(args.k)]
            start = time.perf_counter()
            process = subprocess.run(command, capture_output=True, text=True, timeout=120)
            process_ms = (time.perf_counter() - start) * 1000
            check(process.returncode == 0 and not process.stderr,
                  f"CLI failed for {metric}/{query_info['file']}: {process.stderr}")
            output = process.stdout
            for fragment in (f"Vectors indexed: {len(ids)}\n", f"Dimensions: {data.shape[1]}\n",
                             f"Results returned: {count}\n", f"Requested K: {args.k}\n",
                             "Algorithm: BruteForce\n", "Search completed successfully."):
                check(fragment in output, f"Missing output metadata: {fragment}")
            rows = ROW.findall(output)
            check(len(rows) == count, "Incorrect table count or decimal formatting")
            timer = TIMER.search(output)
            check(timer is not None, "Missing query timing")
            query_ms = float(timer.group(1))
            check(math.isfinite(query_ms) and query_ms >= 0, "Invalid query time")
            seen = set()
            results = []
            for position, (rank, id_text, distance_text) in enumerate(rows):
                vector_id, distance = int(id_text), float(distance_text)
                check(int(rank) == position + 1, "Incorrect result rank")
                check(vector_id in index_by_id and vector_id not in seen, "Missing or duplicate result ID")
                seen.add(vector_id)
                index = index_by_id[vector_id]
                error = abs(distance - float(reference[index]))
                check(math.isfinite(distance) and error < 1e-5, f"Distance error for ID {vector_id}: {error}")
                maximum_error = max(maximum_error, error)
                # Equality of the independently rounded float distances permits any tie order.
                check(rounded[index] == expected_ranks[position], f"Incorrect exact Top-K for ID {vector_id}")
                if position < 3:
                    results.append({"vector_id": vector_id, "title": articles[index]["title"],
                                    "source_id": articles[index]["source_id"], "distance": distance})
            observations.append({"metric": metric, "query": query_info["file"],
                                 "query_title": query_info["title"], "query_ms": query_ms,
                                 "whole_process_ms": process_ms, "first_three_results": results})
            print(f"PASS {metric:17s} {query_info['file']}: Top-{count}, query {query_ms:.6f} ms", flush=True)
    summary = {metric: {"queries": len(values), "median_query_ms": statistics.median(values),
                        "min_query_ms": min(values), "max_query_ms": max(values)}
               for metric in ("cosine", "squared-euclidean")
               for values in [[item["query_ms"] for item in observations if item["metric"] == metric]]}
    report = {"dataset": manifest["dataset"], "revision": manifest["revision"],
              "vectors": len(ids), "dimension": data.shape[1], "requested_k": args.k,
              "passed_queries": len(observations), "checked_neighbors": count * len(observations),
              "max_absolute_distance_error": maximum_error, "tolerance": "abs(actual - reference) < 1e-5",
              "oracle": "Independent NumPy float64 exhaustive distances; float32 ranking, ties unordered",
              "timing_scope": "QueryEngine search only; excludes CSV load, parsing and output",
              "timing_limit": "One observation per query/metric; local validation, not a production benchmark",
              "platform": platform.platform(), "machine": platform.machine(),
              "cli": str(args.cli.resolve()), "summary": summary, "observations": observations}
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(json.dumps({"passed_queries": len(observations), "checked_neighbors": report["checked_neighbors"],
                      "max_absolute_distance_error": maximum_error, "summary": summary}, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--data-dir", type=Path, required=True)
    parser.add_argument("--k", type=int, default=100)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    if args.k < 1:
        parser.error("--k must be positive")
    try:
        validate(args)
    except (OSError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
        print(f"Real dataset validation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
