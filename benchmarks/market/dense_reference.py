#!/usr/bin/env python3
"""Exact dense retrieval for prepared BEIR datasets (requires .venv-bench numpy).

Brute-force inner product over the unit-normalized bge vectors (= cosine), float32,
blocked matrix products. Writes PREFIX.run (top --depth per query, score = cosine) and
PREFIX_gt.ivecs (top-100 document ordinals per query), the ground truth for running
aether_ann_compare (AetherIndex, hnswlib, FAISS HNSW) on the BEIR corpora.
"""

import argparse
from pathlib import Path


def read_fvecs(path):
    import numpy as np

    raw = np.fromfile(path, dtype=np.float32)
    dimension = raw[:1].view(np.int32)[0]
    return raw.reshape(-1, dimension + 1)[:, 1:]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data", type=Path, required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--depth", type=int, default=101)
    args = parser.parse_args()
    import json
    import numpy as np

    corpus = read_fvecs(args.data / "corpus.fvecs")
    queries = read_fvecs(args.data / "queries.fvecs")
    doc_ids = (args.data / "doc_ids.txt").read_text(encoding="utf-8").split("\n")
    query_ids = [json.loads(l)["query_id"] for l in (args.data / "queries.jsonl").open(encoding="utf-8")]
    keep = max(args.depth, 100)
    top = np.empty((len(queries), keep), dtype=np.int64)
    top_scores = np.empty((len(queries), keep), dtype=np.float32)
    for start in range(0, len(queries), 256):
        scores = queries[start:start + 256] @ corpus.T
        part = np.argpartition(-scores, keep - 1, axis=1)[:, :keep]
        part_scores = np.take_along_axis(scores, part, axis=1)
        order = np.lexsort((part, -part_scores), axis=1)  # score desc, ordinal asc
        top[start:start + 256] = np.take_along_axis(part, order, axis=1)
        top_scores[start:start + 256] = np.take_along_axis(part_scores, order, axis=1)
    with open(f"{args.out}.run", "w", encoding="utf-8") as run:
        for q, qid in enumerate(query_ids):
            for rank in range(args.depth):
                run.write(f"{qid} Q0 {doc_ids[top[q, rank]]} {rank + 1} {float(top_scores[q, rank])!r} exact\n")
    gt = np.empty((len(queries), 101), dtype=np.int32)
    gt[:, 0] = 100
    gt[:, 1:] = top[:, :100]
    gt.tofile(f"{args.out}_gt.ivecs")
    print(f"{args.data.name}: exact top-{args.depth} for {len(queries)} queries")


if __name__ == "__main__":
    main()
