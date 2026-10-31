#!/usr/bin/env python3
"""Embed prepared BEIR corpora and queries once, for every dense/hybrid system.

Requires the .venv-bench environment (torch + sentence-transformers). Writes, next to
each prepared dataset (datasets/market/beir/<name>/):

  corpus.fvecs   one row per document, in corpus.jsonl order (row N = DocumentId N)
  queries.fvecs  one row per query, in queries.jsonl order
  embedding.json model id and resolved revision, device, prefixes, dimension, hashes

fvecs is the TEXMEX layout (int32 dimension, then float32 components, per row).
Vectors are L2-normalized, so cosine, inner product and L2 rankings agree. Documents
are embedded as "title\\ntext" (the same string BM25 indexes); queries carry the BGE
retrieval instruction, as recommended for bge-*-en-v1.5.

--queries-only embeds only queries.jsonl (for the dev folders, whose corpus.fvecs links to
the test folder's) and records it in embedding.json.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import time

ROOT = Path(__file__).resolve().parents[2]
MODEL = "BAAI/bge-small-en-v1.5"
QUERY_PREFIX = "Represent this sentence for searching relevant passages: "
os.environ.setdefault("HF_HOME", str(ROOT / "datasets/market/hf-cache"))


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def write_fvecs(path, matrix):
    import numpy as np

    rows, dimension = matrix.shape
    out = np.empty((rows, dimension + 1), dtype=np.float32)
    out[:, 0] = np.array([dimension], dtype=np.int32).view(np.float32)[0]
    out[:, 1:] = matrix
    tmp = path.with_suffix(".tmp")
    out.tofile(tmp)
    tmp.replace(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--data", type=Path, default=ROOT / "datasets/market/beir")
    parser.add_argument("--batch-size", type=int, default=128)
    parser.add_argument("--queries-only", action="store_true")
    parser.add_argument("datasets", nargs="+")
    args = parser.parse_args()

    import numpy as np
    import torch
    from sentence_transformers import SentenceTransformer

    device = "mps" if torch.backends.mps.is_available() else "cpu"
    model = SentenceTransformer(MODEL, device=device)
    model.max_seq_length = 512
    revision = None
    try:
        from huggingface_hub import HfApi
        revision = HfApi().model_info(MODEL).sha
    except Exception as error:  # Offline: record that the revision is unknown.
        revision = f"unknown ({error.__class__.__name__})"
    for name in args.datasets:
        folder = args.data / name
        docs = [json.loads(line) for line in (folder / "corpus.jsonl").open(encoding="utf-8")]
        if [d["document_id"] for d in docs] != list(range(len(docs))):
            raise ValueError(f"{name}: document ids are not ordinals")
        queries = [json.loads(line) for line in (folder / "queries.jsonl").open(encoding="utf-8")]
        start = time.time()
        doc_vectors = None
        if not args.queries_only:
            doc_vectors = model.encode([f"{d['title']}\n{d['text']}" for d in docs],
                                       batch_size=args.batch_size, normalize_embeddings=True,
                                       convert_to_numpy=True, show_progress_bar=False)
        doc_seconds = time.time() - start
        query_vectors = model.encode([QUERY_PREFIX + q["query_text"] for q in queries],
                                     batch_size=args.batch_size, normalize_embeddings=True,
                                     convert_to_numpy=True, show_progress_bar=False)
        for matrix in (doc_vectors, query_vectors):
            if matrix is not None and not np.isfinite(matrix).all():
                raise ValueError(f"{name}: non-finite embedding")
        if doc_vectors is not None:
            write_fvecs(folder / "corpus.fvecs", doc_vectors.astype(np.float32))
        write_fvecs(folder / "queries.fvecs", query_vectors.astype(np.float32))
        info = {
            "model": MODEL, "revision": revision, "device": device,
            "torch": torch.__version__, "max_seq_length": model.max_seq_length,
            "dimension": int(query_vectors.shape[1]), "normalized": True,
            "queries_only": args.queries_only,
            "document_text": "title + '\\n' + text", "query_prefix": QUERY_PREFIX,
            "documents": len(docs), "queries": len(queries),
            "document_seconds": round(doc_seconds, 1),
            "corpus_fvecs_sha256": sha256(folder / "corpus.fvecs"),
            "queries_fvecs_sha256": sha256(folder / "queries.fvecs"),
        }
        (folder / "embedding.json").write_text(json.dumps(info, indent=2) + "\n", encoding="utf-8")
        print(f"{name}: {len(docs)} documents in {doc_seconds:.0f} s, {len(queries)} queries",
              flush=True)


if __name__ == "__main__":
    main()
