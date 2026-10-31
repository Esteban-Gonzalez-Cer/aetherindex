#!/usr/bin/env python3
"""Reference fusion of two TREC runs (standard library only), the same arithmetic as
AetherIndex's hybrid::RrfFusion, so fusing AetherIndex's own component runs must
reproduce its hybrid run exactly (--check).

  RRF (default)    score(d) = sum over runs containing d of w / (k_rrf + rank), ranks from 1
                   in each run's file order; w = 1 for both runs, or with --dense-weight W,
                   W for the dense run and 1 - W for the lexical run.
  Convex (--alpha) score(d) = alpha * dense(d) + (1 - alpha) * lexical(d), each run's scores
                   min-max normalized over its own candidates (1 when all are equal; 0 for a
                   document missing from that run). The dense run's scores are AetherIndex's
                   1 - distance, as its run files print them.

Output order: score descending, then the document's ordinal (line number in doc_ids.txt)
ascending, AetherIndex's DocumentId tie rule. Each input run is cut to --candidates per
query first.
"""

import argparse
from pathlib import Path


def read_ranked(path, candidates):
    """qid -> [(docid, score)] in file (rank) order, cut to candidates."""
    runs = {}
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        qid, _, docid, rank, score, _ = line.split()
        runs.setdefault(qid, []).append((int(rank), docid, float(score)))
    return {q: [(d, s) for _, d, s in sorted(rows)][:candidates] for q, rows in runs.items()}


def fuse_query(dense, lexical, k_rrf=60.0, dense_weight=None, alpha=None):
    """Fused scores {docid: score} for one query's ranked (docid, score) lists."""
    scores = {}
    if alpha is None:
        weights = (1.0, 1.0) if dense_weight is None else (dense_weight, 1.0 - dense_weight)
        for ranking, weight in zip((dense, lexical), weights):
            for rank, (docid, _) in enumerate(ranking, 1):
                scores[docid] = scores.get(docid, 0.0) + weight / (k_rrf + rank)
        return scores
    for ranking, factor in ((dense, alpha), (lexical, 1.0 - alpha)):
        if not ranking:
            continue
        low = min(s for _, s in ranking)
        high = max(s for _, s in ranking)
        for docid, score in ranking:
            normalized = (score - low) / (high - low) if high > low else 1.0
            scores[docid] = scores.get(docid, 0.0) + factor * normalized
    return scores


def fuse(dense, lexical, ordinal, k_rrf, depth, dense_weight=None, alpha=None):
    fused = {}
    for qid in sorted(set(dense) | set(lexical)):
        scores = fuse_query(dense.get(qid, []), lexical.get(qid, []), k_rrf, dense_weight, alpha)
        fused[qid] = sorted(scores.items(), key=lambda item: (-item[1], ordinal[item[0]]))[:depth]
    return fused


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dense", required=True)
    parser.add_argument("--lexical", required=True)
    parser.add_argument("--doc-ids", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--k-rrf", type=float, default=60.0)
    parser.add_argument("--dense-weight", type=float, help="weighted RRF: dense weight W")
    parser.add_argument("--alpha", type=float, help="convex fusion instead of RRF")
    parser.add_argument("--candidates", type=int, default=101)
    parser.add_argument("--depth", type=int, default=101)
    parser.add_argument("--check", help="AetherIndex hybrid run that must match exactly")
    parser.add_argument("--tag", default="rrf")
    args = parser.parse_args()
    ids = Path(args.doc_ids).read_text(encoding="utf-8").split("\n")
    ordinal = {docid: i for i, docid in enumerate(ids) if docid}
    if args.alpha is not None and args.dense_weight is not None:
        parser.error("--alpha and --dense-weight are mutually exclusive")
    fused = fuse(read_ranked(args.dense, args.candidates), read_ranked(args.lexical, args.candidates),
                 ordinal, args.k_rrf, args.depth, args.dense_weight, args.alpha)
    with open(args.out, "w", encoding="utf-8") as out:
        for qid, rows in fused.items():
            for rank, (docid, score) in enumerate(rows, 1):
                out.write(f"{qid} Q0 {docid} {rank} {score!r} {args.tag}\n")
    if args.check:
        expected = {}
        for line in Path(args.check).read_text(encoding="utf-8").splitlines():
            qid, _, docid, _, score, _ = line.split()
            expected.setdefault(qid, []).append((docid, float(score)))
        mismatches = 0
        for qid in set(expected) | set(fused):
            a, b = expected.get(qid, []), fused.get(qid, [])
            if [d for d, _ in a] != [d for d, _ in b] or any(
                    abs(x - y) > 1e-12 for (_, x), (_, y) in zip(a, b)):
                mismatches += 1
        print(f"fusion check: {mismatches} of {len(fused)} queries differ")
        raise SystemExit(1 if mismatches else 0)


if __name__ == "__main__":
    main()
