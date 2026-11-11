#!/usr/bin/env python3
"""bm25s runs for the market comparison (requires .venv-bench).

Two configurations, both with the Lucene BM25 variant and the same k1/b as the others:
  default  bm25s out of the box: its regex tokenizer, English stopwords, PyStemmer
           "english" (Snowball/Porter2) stemming, numpy backend.
  matched  token lists produced by AetherIndex english-v1 (aether_ir_compare tokens), so
           only bm25s scoring and retrieval differ from AetherIndex.
Protocol as in aether_ir_compare: a quality run of --depth results, single-thread
latency over the first --latency-queries queries at --latency-k after one warm-up pass,
and batch throughput with --threads over every query (bm25s n_threads). Latency includes
Python call overhead, which is part of using bm25s.
"""

import argparse
import json
from pathlib import Path
import subprocess
import tempfile
import time


def percentile(values, p):
    ordered = sorted(values)
    rank = max(int(-(-p * len(ordered) // 1)), 1)
    return ordered[rank - 1]


def median(values):
    return percentile(values, 0.5)


def english_v1_tokens(binary, lines):
    with tempfile.TemporaryDirectory() as folder:
        source, target = Path(folder) / "in.txt", Path(folder) / "out.txt"
        source.write_text("\n".join(lines) + "\n", encoding="utf-8")
        subprocess.run([binary, "tokens", "--input", str(source), "--out", str(target),
                        "--analyzer", "english-v1"], check=True)
        return [line.split() for line in target.read_text(encoding="utf-8").split("\n")[:len(lines)]]


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--data", type=Path, required=True)
    parser.add_argument("--out", required=True, help="Output prefix")
    parser.add_argument("--config", choices=["default", "matched"], required=True)
    parser.add_argument("--tokens-binary", default="build-market/benchmarks/market/aether_ir_compare")
    parser.add_argument("--k1", type=float, default=0.9)
    parser.add_argument("--b", type=float, default=0.4)
    parser.add_argument("--depth", type=int, default=101)
    parser.add_argument("--latency-k", type=int, default=10)
    parser.add_argument("--latency-queries", type=int, default=1000)
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--threads", type=int, default=10)
    args = parser.parse_args()

    import bm25s
    import numpy as np
    import Stemmer

    docs = [json.loads(line) for line in (args.data / "corpus.jsonl").open(encoding="utf-8")]
    doc_ids = (args.data / "doc_ids.txt").read_text(encoding="utf-8").split("\n")[:len(docs)]
    queries = [json.loads(line) for line in (args.data / "queries.jsonl").open(encoding="utf-8")]
    texts = [f"{d['title']}\n{d['text']}" for d in docs]
    query_texts = [q["query_text"] for q in queries]

    start = time.perf_counter()
    if args.config == "default":
        stemmer = Stemmer.Stemmer("english")
        corpus_tokens = bm25s.tokenize(texts, stopwords="en", stemmer=stemmer,
                                       show_progress=False)
        query_tokens = bm25s.tokenize(query_texts, stopwords="en", stemmer=stemmer,
                                      return_ids=False, show_progress=False)
    else:
        corpus_tokens = english_v1_tokens(args.tokens_binary, [" ".join(t.split()) for t in texts])
        query_tokens = english_v1_tokens(args.tokens_binary, query_texts)
    tokenize_seconds = time.perf_counter() - start
    start = time.perf_counter()
    retriever = bm25s.BM25(k1=args.k1, b=args.b, method="lucene")
    retriever.index(corpus_tokens, show_progress=False)
    index_seconds = time.perf_counter() - start
    vocabulary = retriever.vocab_dict
    # bm25s rejects queries without any indexed token; such queries get no results.
    known = [[t for t in tokens if t in vocabulary] for tokens in query_tokens]

    def retrieve(indices, k, threads=1):
        rows = [known[i] for i in indices]
        live = [j for j, tokens in enumerate(rows) if tokens]
        out = [([], []) for _ in rows]
        if live:
            kk = min(k, len(docs))
            ids, scores = retriever.retrieve([rows[j] for j in live], k=kk, n_threads=threads,
                                             show_progress=False)
            for slot, j in enumerate(live):
                keep = scores[slot] > 0  # bm25s pads with zero-score documents.
                out[j] = (ids[slot][keep], scores[slot][keep])
        return out

    results = retrieve(range(len(queries)), args.depth, args.threads)
    with open(f"{args.out}.run", "w", encoding="utf-8") as run:
        for query, (ids, scores) in zip(queries, results):
            for rank, (doc, score) in enumerate(zip(ids, scores), 1):
                run.write(f"{query['query_id']} Q0 {doc_ids[int(doc)]} {rank} {float(score)!r} bm25s\n")

    rows = min(args.latency_queries, len(queries))
    for q in range(rows):
        retrieve([q], args.latency_k)
    per_query = [[] for _ in range(rows)]
    p50, p95, p99, mean, qps1 = [], [], [], [], []
    for _ in range(args.repetitions):
        latencies = []
        for q in range(rows):
            start = time.perf_counter()
            retrieve([q], args.latency_k)
            latencies.append((time.perf_counter() - start) * 1000)
            per_query[q].append(latencies[-1])
        p50.append(percentile(latencies, 0.5))
        p95.append(percentile(latencies, 0.95))
        p99.append(percentile(latencies, 0.99))
        mean.append(sum(latencies) / rows)
        qps1.append(1000 * rows / sum(latencies))
    qps = []
    for rep in range(args.repetitions + 1):
        start = time.perf_counter()
        retrieve(range(len(queries)), args.latency_k, args.threads)
        if rep:
            qps.append(len(queries) / (time.perf_counter() - start))
    with open(f"{args.out}_latency.csv", "w", encoding="utf-8") as raw:
        raw.write("query_index,query_id,median_latency_ms\n")
        for q in range(rows):
            raw.write(f"{q},{queries[q]['query_id']},{median(per_query[q]):.9g}\n")
    scores = retriever.scores
    index_bytes = int(sum(np.asarray(scores[key]).nbytes for key in ("data", "indices", "indptr")))
    summary = {
        "system": f"bm25s-{args.config}", "bm25s": bm25s.__version__, "k1": args.k1,
        "b": args.b, "method": "lucene", "backend": "numpy",
        "documents": len(docs), "queries": len(queries), "depth": args.depth,
        "tokenize_seconds": tokenize_seconds, "build_seconds_single_thread": index_seconds,
        "index_bytes": index_bytes, "latency_k": args.latency_k, "latency_queries": rows,
        "repetitions": args.repetitions, "lat_p50_ms": median(p50), "lat_p95_ms": median(p95),
        "lat_p99_ms": median(p99), "lat_mean_ms": median(mean), "qps_single": median(qps1),
        "threads": args.threads, "qps_batch": median(qps),
        "qps_batch_q1": percentile(qps, 0.25), "qps_batch_q3": percentile(qps, 0.75),
    }
    Path(f"{args.out}.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(f"bm25s-{args.config} {args.data.name}: p50 {median(p50):.3f} ms, batch QPS {median(qps):.0f}")


if __name__ == "__main__":
    main()
