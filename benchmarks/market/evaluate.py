#!/usr/bin/env python3
"""Relevance metrics for TREC run files, identical for every compared system.

Standard library only. Conventions (trec_eval / BEIR):
  * A query's own document id is removed from its results (BEIR ignore_identical_ids;
    matters on ArguAna and Quora), then the run is cut to --depth (100).
  * Results are ordered by score descending, ties by document id descending (trec_eval).
  * nDCG@10 uses linear gains (the qrels grade) and log2(rank + 1) discounts; the ideal
    ranking uses every judged document with grade > 0. Recall@100 counts grade > 0.
    MRR@10 is the reciprocal rank of the first grade > 0 document within 10.
  * Means are taken over every query in the qrels (trec_eval -c): a query without
    results scores 0.

  evaluate.py --qrels Q --run A.run [--run B.run ...] [--per-query OUT.csv] [--json OUT]
  evaluate.py --qrels Q --run A.run --run B.run --compare   (paired randomization test)
"""

import argparse
import json
import math
from pathlib import Path
import random

METRICS = ("ndcg@10", "recall@100", "mrr@10")


def read_qrels(path):
    qrels = {}
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        qid, _, docid, grade = line.split()
        qrels.setdefault(qid, {})[docid] = int(grade)
    return qrels


def read_run(path, depth=100):
    hits = {}
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        qid, _, docid, _, score, _ = line.split()
        if docid != qid:
            hits.setdefault(qid, []).append((float(score), docid))
    run = {}
    for qid, results in hits.items():
        results.sort(key=lambda item: (item[0], item[1]), reverse=True)
        seen, ranked = set(), []
        for _, docid in results:
            if docid not in seen:
                seen.add(docid)
                ranked.append(docid)
        run[qid] = ranked[:depth]
    return run


def query_metrics(ranked, judged):
    relevant = {d: g for d, g in judged.items() if g > 0}
    dcg = sum(relevant.get(d, 0) / math.log2(i + 2) for i, d in enumerate(ranked[:10]))
    ideal = sorted(relevant.values(), reverse=True)[:10]
    idcg = sum(g / math.log2(i + 2) for i, g in enumerate(ideal))
    recall = (sum(1 for d in ranked[:100] if d in relevant) / len(relevant)) if relevant else 0.0
    mrr = next((1.0 / (i + 1) for i, d in enumerate(ranked[:10]) if d in relevant), 0.0)
    return {"ndcg@10": dcg / idcg if idcg else 0.0, "recall@100": recall, "mrr@10": mrr}


def evaluate(qrels, run):
    per_query = {qid: query_metrics(run.get(qid, []), judged) for qid, judged in qrels.items()}
    means = {m: sum(v[m] for v in per_query.values()) / len(per_query) for m in METRICS}
    return means, per_query


def randomization_test(a, b, trials=10000, seed=7):
    """Two-sided paired randomization test on per-query differences (Smucker et al.)."""
    differences = [x - y for x, y in zip(a, b)]
    observed = abs(sum(differences))
    rng = random.Random(seed)
    extreme = 0
    for _ in range(trials):
        total = sum(d if rng.random() < 0.5 else -d for d in differences)
        extreme += abs(total) >= observed - 1e-12
    return (extreme + 1) / (trials + 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--qrels", required=True)
    parser.add_argument("--run", action="append", required=True)
    parser.add_argument("--depth", type=int, default=100)
    parser.add_argument("--per-query", type=Path)
    parser.add_argument("--json", type=Path)
    parser.add_argument("--compare", action="store_true")
    args = parser.parse_args()
    qrels = read_qrels(args.qrels)
    results = {}
    for path in args.run:
        means, per_query = evaluate(qrels, read_run(path, args.depth))
        results[path] = {"means": means, "per_query": per_query}
        print(path, " ".join(f"{m}={means[m]:.4f}" for m in METRICS))
    if args.compare:
        if len(args.run) != 2:
            raise SystemExit("--compare needs exactly two runs")
        a, b = (results[p]["per_query"] for p in args.run)
        qids = sorted(qrels)
        for metric in METRICS:
            p = randomization_test([a[q][metric] for q in qids], [b[q][metric] for q in qids])
            print(f"{metric}: p={p:.4f}")
    if args.per_query:
        with args.per_query.open("w", encoding="utf-8") as stream:
            stream.write("run,query_id," + ",".join(METRICS) + "\n")
            for path, result in results.items():
                for qid in sorted(result["per_query"]):
                    values = result["per_query"][qid]
                    stream.write(f"{Path(path).name},{qid}," +
                                 ",".join(f"{values[m]:.6f}" for m in METRICS) + "\n")
    if args.json:
        args.json.write_text(json.dumps({p: r["means"] for p, r in results.items()}, indent=2)
                             + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
