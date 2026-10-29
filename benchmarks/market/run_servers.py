#!/usr/bin/env python3
"""Qdrant and OpenSearch runs for the market comparison (requires .venv-bench and the
service from docker-compose.yml running on 127.0.0.1).

  run_servers.py --server qdrant|opensearch --task ann --dataset sift1m|glove-100
  run_servers.py --server qdrant|opensearch --task beir --dataset scifact|...

ANN: HNSW with M=16, ef_construction=200 (same build grid point as the in-process
systems), sweep of ef at query time, Recall@10 against the official ground truth.
BEIR (same analysis and BM25 k1=0.9 b=0.4 as the other systems, same bge vectors):
  qdrant      sparse BM25 vectors (english-v1 tokens; document weights
              tf*(k1+1)/(tf+k1*(1-b+b*dl/avgdl)), IDF modifier applied by Qdrant with
              the Lucene formula), dense HNSW, hybrid = prefetch both + RRF (k=60).
  opensearch  'english' analyzer (Lucene EnglishAnalyzer) with a BM25 similarity, k-NN
              HNSW (faiss engine, inner product), hybrid query with the RRF
              score-ranker processor (rank_constant 60).
Quality runs use depth 101 (hybrid: 101 per retriever, like aether_ir_compare). Latency is
client-observed (HTTP/gRPC on localhost included), single client, first --latency-queries
queries at k=10 (hybrid: 50 per retriever) after a warm-up pass; throughput uses
--threads concurrent clients. Memory is the container's usage from `docker stats` after
indexing, merging and warm-up.
"""

import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import math
from pathlib import Path
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "evaluation/reports/market/servers"
IR = ROOT / "build-market/benchmarks/market/aether_ir_compare"
ANN_DATA = {
    "sift1m": ("datasets/real/sift1m/sift/sift_base.fvecs", "datasets/real/sift1m/sift/sift_query.fvecs",
               "datasets/real/sift1m/sift/sift_groundtruth.ivecs", "l2"),
    "glove-100": ("datasets/market/ann/glove-100/base.fvecs", "datasets/market/ann/glove-100/queries.fvecs",
                  "datasets/market/ann/glove-100/groundtruth.ivecs", "cosine"),
}
CONTAINERS = {"qdrant": "market-qdrant-1", "opensearch": "market-opensearch-1"}
K1, B = 0.9, 0.4


def read_vecs(path, dtype):
    import numpy as np

    raw = np.fromfile(ROOT / path, dtype=dtype)
    dimension = raw[:1].view(np.int32)[0]
    return raw.reshape(-1, dimension + 1)[:, 1:]


def percentile(values, p):
    ordered = sorted(values)
    return ordered[max(math.ceil(p * len(ordered)), 1) - 1]


def container_memory_mib(server):
    usage = subprocess.run(["docker", "stats", "--no-stream", "--format", "{{.MemUsage}}",
                            CONTAINERS[server]], capture_output=True, text=True).stdout
    value = usage.split("/")[0].strip()
    units = {"KiB": 1 / 1024, "MiB": 1, "GiB": 1024, "B": 1 / 1048576}
    for unit, factor in units.items():
        if value.endswith(unit):
            return float(value[:-len(unit)]) * factor
    return None


def image_digest(server):
    image = subprocess.run(["docker", "inspect", "--format", "{{.Image}}", CONTAINERS[server]],
                           capture_output=True, text=True).stdout.strip()
    return image


def timing(search, count, latency_queries, repetitions, threads):
    """search(i) runs query i. Returns latency stats (ms) and batch QPS medians."""
    rows = min(latency_queries, count)
    for q in range(rows):
        search(q)
    p50, p95, p99, mean = [], [], [], []
    for _ in range(repetitions):
        latencies = []
        for q in range(rows):
            start = time.perf_counter()
            search(q)
            latencies.append((time.perf_counter() - start) * 1000)
        p50.append(percentile(latencies, 0.5))
        p95.append(percentile(latencies, 0.95))
        p99.append(percentile(latencies, 0.99))
        mean.append(sum(latencies) / rows)
    qps = []
    with ThreadPoolExecutor(threads) as pool:
        for rep in range(repetitions + 1):
            start = time.perf_counter()
            list(pool.map(search, range(count)))
            if rep:
                qps.append(count / (time.perf_counter() - start))
    return {"latency_queries": rows, "repetitions": repetitions,
            "lat_p50_ms": percentile(p50, 0.5), "lat_p95_ms": percentile(p95, 0.5),
            "lat_p99_ms": percentile(p99, 0.5), "lat_mean_ms": percentile(mean, 0.5),
            "threads": threads, "qps_batch": percentile(qps, 0.5),
            "qps_batch_q1": percentile(qps, 0.25), "qps_batch_q3": percentile(qps, 0.75)}


# --- clients -------------------------------------------------------------------------

def qdrant_client():
    from qdrant_client import QdrantClient

    return QdrantClient(host="127.0.0.1", port=6333, grpc_port=6334, prefer_grpc=True, timeout=3600)


def qdrant_wait(client, name, count):
    """Waits until every point is stored and optimization has settled. Returns Qdrant's
    indexed_vectors_count, an approximate figure: it sums dense and sparse vectors, skips
    empty sparse vectors, and omits dense vectors of segments below indexing_threshold,
    which Qdrant searches exhaustively. It is recorded for disclosure, not compared."""
    from qdrant_client.http.models import CollectionStatus

    previous, stable = None, 0
    while True:
        info = client.get_collection(name)
        indexed = info.indexed_vectors_count or 0
        settled = (info.points_count == count and info.status == CollectionStatus.GREEN
                   and str(info.optimizer_status).lower().endswith("ok"))
        stable = stable + 1 if settled and indexed == previous else 0
        if stable >= 3:
            return indexed
        previous = indexed
        time.sleep(2)


def opensearch_client():
    from opensearchpy import OpenSearch

    return OpenSearch(hosts=[{"host": "127.0.0.1", "port": 9200}], timeout=3600, http_compress=False,
                      pool_maxsize=32)


def opensearch_finish(client, index):
    client.indices.refresh(index=index)
    client.indices.forcemerge(index=index, max_num_segments=1, request_timeout=7200)
    client.indices.refresh(index=index)
    client.transport.perform_request("GET", f"/_plugins/_knn/warmup/{index}")


def opensearch_bulk(client, actions):
    from opensearchpy import helpers

    for ok, info in helpers.parallel_bulk(client, actions, thread_count=4, chunk_size=500,
                                          request_timeout=600):
        if not ok:
            raise RuntimeError(info)


# --- ANN -----------------------------------------------------------------------------

def run_ann(server, dataset, args):
    import numpy as np

    base_path, query_path, gt_path, metric = ANN_DATA[dataset]
    base = read_vecs(base_path, np.float32)
    queries = read_vecs(query_path, np.float32)
    truth = read_vecs(gt_path, np.int32)[:, :10]
    n, d = base.shape
    name = f"ann-{dataset}"
    start = time.perf_counter()
    if server == "qdrant":
        from qdrant_client.http import models as qm

        client = qdrant_client()
        client.delete_collection(name)
        client.create_collection(
            name, vectors_config=qm.VectorParams(size=d, distance=qm.Distance.EUCLID if metric == "l2"
                                                 else qm.Distance.DOT),
            hnsw_config=qm.HnswConfigDiff(m=16, ef_construct=200))
        client.upload_collection(name, vectors=base, ids=list(range(n)), batch_size=1024, parallel=4)
        indexed = qdrant_wait(client, name, n)
        client = qdrant_client()  # upload forks workers; query on a channel opened after it

        def search(q, ef):
            hits = client.query_points(name, query=queries[q].tolist(), limit=10,
                                       search_params=qm.SearchParams(hnsw_ef=ef)).points
            return [int(h.id) for h in hits]
    else:
        client = opensearch_client()
        if client.indices.exists(index=name):
            client.indices.delete(index=name)
        client.indices.create(index=name, body={
            "settings": {"index": {"knn": True, "number_of_shards": 1, "number_of_replicas": 0,
                                   "refresh_interval": "-1"}},
            "mappings": {"properties": {"v": {
                "type": "knn_vector", "dimension": d,
                "space_type": "l2" if metric == "l2" else "innerproduct",
                "method": {"name": "hnsw", "engine": "faiss",
                           "parameters": {"m": 16, "ef_construction": 200}}}}}})
        opensearch_bulk(client, ({"_index": name, "_id": str(i), "v": base[i].tolist()} for i in range(n)))
        opensearch_finish(client, name)
        indexed = None

        def search(q, ef):
            body = {"size": 10, "_source": False, "query": {"knn": {"v": {
                "vector": queries[q].tolist(), "k": 10, "method_parameters": {"ef_search": ef}}}}}
            return [int(h["_id"]) for h in client.search(index=name, body=body)["hits"]["hits"]]
    build_seconds = time.perf_counter() - start
    memory = container_memory_mib(server)
    rows = []
    for ef in [int(x) for x in args.params.split(",")]:
        with ThreadPoolExecutor(args.threads) as pool:
            results = list(pool.map(lambda q: search(q, ef), range(len(queries))))
        recall = sum(len(set(r) & set(truth[q].tolist())) for q, r in enumerate(results)) / (10 * len(queries))
        stats = timing(lambda q: search(q, ef), len(queries), args.latency_queries,
                       args.repetitions, args.threads)
        rows.append({"dataset": dataset, "system": server, "M": 16, "ef_construction": 200,
                     "param": ef, "recall10": recall, "build_seconds": build_seconds,
                     "memory_mib": memory, "qdrant_indexed_vectors": indexed, **stats})
        print(f"{server} {dataset} ef={ef} recall={recall:.4f} p50={stats['lat_p50_ms']:.3f} ms "
              f"qps={stats['qps_batch']:.0f}", flush=True)
    return rows


# --- BEIR ----------------------------------------------------------------------------

def english_tokens(lines):
    with tempfile.TemporaryDirectory() as folder:
        source, target = Path(folder) / "in.txt", Path(folder) / "out.txt"
        source.write_text("\n".join(lines) + "\n", encoding="utf-8")
        subprocess.run([IR, "tokens", "--input", source, "--out", target, "--analyzer",
                        "english-v1"], check=True)
        return [l.split() for l in target.read_text(encoding="utf-8").split("\n")[:len(lines)]]


def run_beir(server, dataset, args):
    import numpy as np

    data = ROOT / "datasets/market/beir" / dataset
    docs = [json.loads(l) for l in (data / "corpus.jsonl").open(encoding="utf-8")]
    doc_ids = (data / "doc_ids.txt").read_text(encoding="utf-8").split("\n")[:len(docs)]
    queries = [json.loads(l) for l in (data / "queries.jsonl").open(encoding="utf-8")]
    corpus_vectors = read_vecs(data / "corpus.fvecs", np.float32)
    query_vectors = read_vecs(data / "queries.fvecs", np.float32)
    texts = [f"{d['title']}\n{d['text']}" for d in docs]
    name = f"beir-{dataset}"
    start = time.perf_counter()
    if server == "qdrant":
        from qdrant_client.http import models as qm

        doc_tokens = english_tokens([" ".join(t.split()) for t in texts])
        query_tokens = english_tokens([q["query_text"] for q in queries])
        vocabulary = {t: i for i, t in enumerate(sorted({t for doc in doc_tokens for t in doc}))}
        avgdl = sum(len(t) for t in doc_tokens) / len(doc_tokens)

        def doc_sparse(tokens):
            counts = {}
            for t in tokens:
                counts[t] = counts.get(t, 0) + 1
            norm = K1 * (1 - B + B * len(tokens) / avgdl)
            items = sorted((vocabulary[t], c * (K1 + 1) / (c + norm)) for t, c in counts.items())
            return qm.SparseVector(indices=[i for i, _ in items], values=[v for _, v in items])

        def query_sparse(q):
            ids = sorted({vocabulary[t] for t in query_tokens[q] if t in vocabulary})
            return qm.SparseVector(indices=ids, values=[1.0] * len(ids))

        client = qdrant_client()
        client.delete_collection(name)
        client.create_collection(
            name, vectors_config={"dense": qm.VectorParams(size=corpus_vectors.shape[1],
                                                           distance=qm.Distance.DOT)},
            sparse_vectors_config={"bm25": qm.SparseVectorParams(modifier=qm.Modifier.IDF)},
            hnsw_config=qm.HnswConfigDiff(m=16, ef_construct=200))
        points = (qm.PointStruct(id=i, vector={"dense": corpus_vectors[i].tolist(),
                                               "bm25": doc_sparse(doc_tokens[i])})
                  for i in range(len(docs)))
        client.upload_points(name, points=points, batch_size=256, parallel=4)
        indexed = qdrant_wait(client, name, len(docs))
        client = qdrant_client()  # upload forks workers; query on a channel opened after it
        hnsw = qm.SearchParams(hnsw_ef=128)

        def lexical(q, k):
            sparse = query_sparse(q)
            if not sparse.indices:
                return []
            return [(int(p.id), p.score) for p in client.query_points(
                name, query=sparse, using="bm25", limit=k).points]

        def dense(q, k):
            return [(int(p.id), p.score) for p in client.query_points(
                name, query=query_vectors[q].tolist(), using="dense", limit=k,
                search_params=hnsw).points]

        def hybrid(q, k, candidates):
            prefetch = [qm.Prefetch(query=query_vectors[q].tolist(), using="dense", limit=candidates,
                                    params=hnsw)]
            sparse = query_sparse(q)
            if sparse.indices:
                prefetch.append(qm.Prefetch(query=sparse, using="bm25", limit=candidates))
            return [(int(p.id), p.score) for p in client.query_points(
                name, prefetch=prefetch, query=qm.RrfQuery(rrf=qm.Rrf(k=60)), limit=k).points]
        to_beir = lambda hit: doc_ids[hit]
    else:
        client = opensearch_client()
        if client.indices.exists(index=name):
            client.indices.delete(index=name)
        client.indices.create(index=name, body={
            "settings": {"index": {
                "knn": True, "number_of_shards": 1, "number_of_replicas": 0,
                "refresh_interval": "-1",
                "similarity": {"bm25_beir": {"type": "BM25", "k1": K1, "b": B}}}},
            "mappings": {"properties": {
                "contents": {"type": "text", "analyzer": "english", "similarity": "bm25_beir"},
                "embedding": {"type": "knn_vector", "dimension": int(corpus_vectors.shape[1]),
                              "space_type": "innerproduct",
                              "method": {"name": "hnsw", "engine": "faiss",
                                         "parameters": {"m": 16, "ef_construction": 200}}}}}})
        opensearch_bulk(client, ({"_index": name, "_id": doc_ids[i], "contents": texts[i],
                                  "embedding": corpus_vectors[i].tolist()} for i in range(len(docs))))
        opensearch_finish(client, name)
        indexed = None
        client.transport.perform_request("PUT", "/_search/pipeline/rrf-pipeline", body={
            "phase_results_processors": [{"score-ranker-processor": {
                "combination": {"technique": "rrf", "rank_constant": 60}}}]})

        def hits(body, pipeline=None):
            params = {"search_pipeline": pipeline} if pipeline else None
            result = client.search(index=name, body=body, params=params)
            return [(h["_id"], h["_score"]) for h in result["hits"]["hits"]]

        def knn(q, k):
            return {"knn": {"embedding": {"vector": query_vectors[q].tolist(), "k": k,
                                          "method_parameters": {"ef_search": 128}}}}

        def match(q):
            return {"match": {"contents": queries[q]["query_text"]}}

        def lexical(q, k):
            return hits({"size": k, "_source": False, "query": match(q)})

        def dense(q, k):
            return hits({"size": k, "_source": False, "query": knn(q, k)})

        def hybrid(q, k, candidates):
            return hits({"size": k, "_source": False, "query": {"hybrid": {
                "pagination_depth": candidates, "queries": [match(q), knn(q, candidates)]}}},
                pipeline="rrf-pipeline")
        to_beir = lambda hit: hit
    build_seconds = time.perf_counter() - start
    memory = container_memory_mib(server)
    folder = OUT / "beir" / dataset
    folder.mkdir(parents=True, exist_ok=True)
    summaries = {}
    modes = {"bm25": (lambda q, k: lexical(q, k)), "dense": (lambda q, k: dense(q, k)),
             "hybrid": (lambda q, k: hybrid(q, k, 101 if k > 10 else 50))}
    for mode, function in modes.items():
        prefix = folder / f"{server}-{mode}"
        with ThreadPoolExecutor(args.threads) as pool:
            results = list(pool.map(lambda q: function(q, 101), range(len(queries))))
        with open(f"{prefix}.run", "w", encoding="utf-8") as run:
            for query, ranking in zip(queries, results):
                for rank, (hit, score) in enumerate(ranking, 1):
                    run.write(f"{query['query_id']} Q0 {to_beir(hit)} {rank} {float(score)!r} {server}\n")
        stats = timing(lambda q: function(q, 10), len(queries), args.latency_queries,
                       args.repetitions, args.threads)
        summaries[mode] = stats
        print(f"{server} {dataset} {mode}: p50 {stats['lat_p50_ms']:.3f} ms, "
              f"QPS {stats['qps_batch']:.0f}", flush=True)
        Path(f"{prefix}.json").write_text(json.dumps({
            "system": f"{server}-{mode}", "dataset": dataset, "documents": len(docs),
            "queries": len(queries), "ingest_and_index_seconds": build_seconds,
            "container_memory_mib": memory, "image": image_digest(server), "k1": K1, "b": B,
            "hnsw": {"M": 16, "ef_construction": 200, "ef": 128},
            "qdrant_indexed_vectors": indexed, **stats}, indent=2) + "\n",
            encoding="utf-8")
    return summaries


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--server", choices=["qdrant", "opensearch"], required=True)
    parser.add_argument("--task", choices=["ann", "beir"], required=True)
    parser.add_argument("--dataset", required=True)
    parser.add_argument("--params", default="16,32,64,128,256,512")
    parser.add_argument("--latency-queries", type=int, default=1000)
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--threads", type=int, default=10)
    args = parser.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    if args.task == "ann":
        rows = run_ann(args.server, args.dataset, args)
        path = OUT / "ann_servers.csv"
        new = not path.exists()
        with path.open("a", encoding="utf-8") as out:
            if new:
                out.write(",".join(rows[0].keys()) + "\n")
            for row in rows:
                out.write(",".join(str(v) for v in row.values()) + "\n")
        (OUT / f"ann-{args.server}-{args.dataset}.json").write_text(json.dumps(
            {"image": image_digest(args.server), "rows": rows}, indent=2) + "\n", encoding="utf-8")
    else:
        run_beir(args.server, args.dataset, args)


if __name__ == "__main__":
    main()
