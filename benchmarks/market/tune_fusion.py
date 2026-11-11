#!/usr/bin/env python3
"""Tune hybrid fusion on BEIR dev splits only (standard library only).

For each dataset with dev (or train) judgments it reads AetherIndex's dense and BM25 runs
on the dev queries, fuses them offline with exactly the engine's arithmetic (fuse_rrf.py)
over a grid, and scores nDCG@10 with evaluate.py's conventions:

  weighted RRF   dense weight W in {0, 0.05, ..., 1} (lexical 1 - W), k_rrf = 60
  convex         alpha in {0, 0.05, ..., 1} on min-max normalized scores

It picks, per method, the best value for each dataset and one global value that maximizes
the mean dev nDCG@10 over all tuned datasets (ties go to the value closest to 0.5). Test
judgments are never read. Writes --out (JSON) and a CSV of the whole grid next to it.

  tune_fusion.py --dev-root DIR --runs DIR --out fusion_tuning.json [datasets...]
"""

import argparse
import csv
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))  # Also under python -I.
from evaluate import evaluate, read_qrels  # noqa: E402
from fuse_rrf import fuse_query, read_ranked  # noqa: E402

GRID = [round(i * 0.05, 2) for i in range(21)]
CANDIDATES = 101


def to_run(fused):
    """Fused scores -> evaluate.py run: own document dropped, score then id descending."""
    run = {}
    for qid, scores in fused.items():
        hits = sorted(((s, d) for d, s in scores.items() if d != qid), reverse=True)
        run[qid] = [d for _, d in hits][:100]
    return run


def score(qrels, dense, lexical, **params):
    fused = {q: fuse_query(dense.get(q, []), lexical.get(q, []), **params)
             for q in set(dense) | set(lexical)}
    return evaluate(qrels, to_run(fused))[0]["ndcg@10"]


def pick(values):
    """Best grid value; ties prefer the value closest to 0.5."""
    best = max(values.values())
    return min((v for v, s in values.items() if s == best), key=lambda v: (abs(v - 0.5), v))


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dev-root", type=Path, required=True)
    parser.add_argument("--runs", type=Path, required=True,
                        help="folder with <dataset>/dense.run and <dataset>/bm25.run (dev)")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("datasets", nargs="+")
    args = parser.parse_args()
    grid = {"weighted-rrf": {}, "convex": {}}
    baselines = {}
    for name in args.datasets:
        qrels = read_qrels(args.dev_root / name / "qrels.tsv")
        dense = read_ranked(args.runs / name / "dense.run", CANDIDATES)
        lexical = read_ranked(args.runs / name / "bm25.run", CANDIDATES)
        baselines[name] = {"rrf": score(qrels, dense, lexical),
                           "dense": score(qrels, dense, {}), "bm25": score(qrels, {}, lexical),
                           "queries": len(qrels)}
        grid["weighted-rrf"][name] = {w: score(qrels, dense, lexical, dense_weight=w) for w in GRID}
        grid["convex"][name] = {a: score(qrels, dense, lexical, alpha=a) for a in GRID}
        print(f"{name}: {len(qrels)} dev queries, RRF {baselines[name]['rrf']:.4f}", flush=True)
    result = {"protocol": "tuned on dev (SciFact: train) judgments only; test never read",
              "grid": GRID, "candidates": CANDIDATES, "k_rrf": 60.0, "baselines": baselines}
    for method, per_dataset in grid.items():
        mean = {v: sum(per_dataset[n][v] for n in args.datasets) / len(args.datasets) for v in GRID}
        result[method] = {
            "per_dataset": {n: {"value": pick(per_dataset[n]),
                                "dev_ndcg@10": per_dataset[n][pick(per_dataset[n])]}
                            for n in args.datasets},
            "global": {"value": pick(mean), "mean_dev_ndcg@10": mean[pick(mean)]},
        }
        print(f"{method}: global {pick(mean)} (mean dev nDCG@10 {mean[pick(mean)]:.4f})")
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    with args.out.with_suffix(".csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(["method", "dataset", "value", "dev_ndcg@10"])
        for method, per_dataset in grid.items():
            for name in args.datasets:
                for value in GRID:
                    writer.writerow([method, name, value, round(per_dataset[name][value], 6)])


if __name__ == "__main__":
    main()
