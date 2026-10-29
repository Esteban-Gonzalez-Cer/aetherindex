#!/usr/bin/env python3
"""Reference Reciprocal Rank Fusion of two TREC runs (standard library only).

score(d) = sum over runs containing d of 1 / (k_rrf + rank), ranks from 1 in each run's
file order (its own relevance order). Output order: score descending, then the
document's ordinal (line number in doc_ids.txt) ascending, which is AetherIndex's
DocumentId tie rule, so fusing AetherIndex's own component runs must reproduce its
hybrid run exactly (--check). Each input run is cut to --candidates per query first.
"""

import argparse
from pathlib import Path


def read_ranked(path, candidates):
    runs = {}
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        qid, _, docid, rank, score, _ = line.split()
        runs.setdefault(qid, []).append((int(rank), docid, score))
    return {q: [d for _, d, _ in sorted(rows)][:candidates] for q, rows in runs.items()}


def fuse(dense, lexical, ordinal, k_rrf, depth):
    fused = {}
    for qid in sorted(set(dense) | set(lexical)):
        scores = {}
        for ranking in (dense.get(qid, []), lexical.get(qid, [])):
            for rank, docid in enumerate(ranking, 1):
                scores[docid] = scores.get(docid, 0.0) + 1.0 / (k_rrf + rank)
        fused[qid] = sorted(scores.items(), key=lambda item: (-item[1], ordinal[item[0]]))[:depth]
    return fused


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dense", required=True)
    parser.add_argument("--lexical", required=True)
    parser.add_argument("--doc-ids", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--k-rrf", type=float, default=60.0)
    parser.add_argument("--candidates", type=int, default=101)
    parser.add_argument("--depth", type=int, default=101)
    parser.add_argument("--check", help="AetherIndex hybrid run that must match exactly")
    parser.add_argument("--tag", default="rrf")
    args = parser.parse_args()
    ids = Path(args.doc_ids).read_text(encoding="utf-8").split("\n")
    ordinal = {docid: i for i, docid in enumerate(ids) if docid}
    fused = fuse(read_ranked(args.dense, args.candidates), read_ranked(args.lexical, args.candidates),
                 ordinal, args.k_rrf, args.depth)
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
