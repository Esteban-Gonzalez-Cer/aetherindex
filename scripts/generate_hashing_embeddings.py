#!/usr/bin/env python3
"""Derive the hybrid demo/evaluation fixture from the CP-01 JSONL corpus.

AetherIndex never computes embeddings. This script runs *outside* the engine and
attaches deterministic, dependency-free "feature hashing" embeddings to the
checked-in corpus (datasets/corpus.jsonl) and queries (datasets/queries.jsonl):

  * text = title + "\\n" + text for documents, query_text for queries;
  * NFC normalization and Unicode case folding, words = runs of letters/digits;
  * features = each word plus the character trigrams of " word ";
  * each feature is hashed with BLAKE2b into one of DIMENSION buckets with a
    hashed sign; counts use sublinear 1 + log(tf) weights; vectors are L2-normalized.

This is a lexical-morphology representation, NOT a neural semantic model. It is
used only so the dense channel (HNSW) has a real, reproducible, label-free input
for integration and contract tests. Relevance labels are copied unchanged and are
never read to build any vector. Replace these vectors with model embeddings
(same JSONL fields) when the team generates them.

Usage, from the repository root:
    python3 scripts/generate_hashing_embeddings.py            # writes datasets/hybrid/
    python3 scripts/generate_hashing_embeddings.py --check    # verifies files are current
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import sys
import unicodedata
from collections import Counter
from pathlib import Path

PROFILE = "aether-hash-trigram-v1"
DIMENSION = 128
WORD = re.compile(r"[^\W_]+", re.UNICODE)


def features(text: str) -> Counter[str]:
    folded = unicodedata.normalize("NFC", unicodedata.normalize("NFC", text).casefold())
    counts: Counter[str] = Counter()
    for word in WORD.findall(folded):
        counts["w:" + word] += 1
        padded = f" {word} "
        for i in range(len(padded) - 2):
            counts["c:" + padded[i : i + 3]] += 1
    return counts


def embed(text: str) -> list[float]:
    vector = [0.0] * DIMENSION
    for feature, tf in features(text).items():
        digest = hashlib.blake2b(feature.encode("utf-8"), digest_size=8).digest()
        value = int.from_bytes(digest, "little")
        sign = 1.0 if (value >> 63) & 1 == 0 else -1.0
        vector[value % DIMENSION] += sign * (1.0 + math.log(tf))
    norm = math.sqrt(sum(v * v for v in vector))
    if norm == 0.0:
        raise ValueError(f"text produced no features: {text!r}")
    # Nine significant digits round-trip through FP32 parsing in the engine.
    return [float(f"{v / norm:.9g}") for v in vector]


def read_jsonl(path: Path) -> list[dict]:
    with path.open(encoding="utf-8") as handle:
        return [json.loads(line) for line in handle if line.strip()]


def render(rows: list[dict]) -> str:
    return "".join(json.dumps(row, ensure_ascii=False, separators=(",", ":")) + "\n" for row in rows)


def build(source: Path) -> dict[str, str]:
    documents = []
    for doc in read_jsonl(source / "corpus.jsonl"):
        documents.append(
            {
                "document_id": doc["document_id"],
                "title": doc.get("title", ""),
                "text": doc["text"],
                "topic": doc.get("topic", ""),
                # Filterable copy of the source topic (no invented metadata).
                "metadata": {"topic": doc.get("topic", "")},
                "embedding_profile": PROFILE,
                "embedding": embed(doc.get("title", "") + "\n" + doc["text"]),
            }
        )
    queries = []
    for query in read_jsonl(source / "queries.jsonl"):
        queries.append(
            {
                "query_id": query["query_id"],
                "query_text": query["query_text"],
                "relevant_document_ids": query["relevant_document_ids"],
                "embedding_profile": PROFILE,
                "embedding": embed(query["query_text"]),
            }
        )
    files = {"corpus.jsonl": render(documents), "queries.jsonl": render(queries)}
    sources = {name: hashlib.sha256((source / name).read_bytes()).hexdigest()
               for name in ("corpus.jsonl", "queries.jsonl")}
    manifest = {
        "profile": PROFILE,
        "dimension": DIMENSION,
        "metric": "cosine",
        "generator": "scripts/generate_hashing_embeddings.py",
        "semantic_model": False,
        "documents": len(documents),
        "queries": len(queries),
        "source_sha256": sources,
        "output_sha256": {name: hashlib.sha256(text.encode("utf-8")).hexdigest()
                          for name, text in files.items()},
    }
    files["manifest.json"] = json.dumps(manifest, indent=2, ensure_ascii=False) + "\n"
    return files


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--source", type=Path, default=root / "datasets")
    parser.add_argument("--output", type=Path, default=root / "datasets" / "hybrid")
    parser.add_argument("--check", action="store_true", help="fail if outputs are stale")
    args = parser.parse_args()
    files = build(args.source)
    if args.check:
        stale = [name for name, text in files.items()
                 if not (args.output / name).is_file()
                 or (args.output / name).read_text(encoding="utf-8") != text]
        if stale:
            print("Stale hybrid fixture files: " + ", ".join(stale), file=sys.stderr)
            return 1
        print("Hybrid fixture is current.")
        return 0
    args.output.mkdir(parents=True, exist_ok=True)
    for name, text in files.items():
        (args.output / name).write_text(text, encoding="utf-8", newline="\n")
    print(f"Wrote {len(files)} files to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
