#!/usr/bin/env python3
"""Prepare real article-recommendation inputs; Python standard library only.

Downloads existing embeddings, never calls a model or a paid inference API.
The dataset-viewer API serves main only, so every response must carry the pinned
X-Revision. A changed or truncated source is rejected instead of silently reused.
"""

import argparse
from array import array
import hashlib
import json
import math
from pathlib import Path
import ssl
import sys
import time
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode
from urllib.request import Request, urlopen


DATASET = "Qdrant/dbpedia-entities-openai3-text-embedding-3-small-1024-100K"
REVISION = "d49822cb2465295366349d414679b476997e88f5"
EMBEDDING_COLUMN = "text-embedding-3-small-1024-embedding"
DIMENSION = 1024
ROOT = Path(__file__).resolve().parents[1]
TLS_CONTEXT = ssl.create_default_context()
# Python.org macOS installations can lack a CA bundle. Use the system bundle
# with normal certificate verification; do not disable TLS validation.
if sys.platform == "darwin" and ssl.get_default_verify_paths().cafile is None:
    if Path("/etc/ssl/cert.pem").is_file():
        TLS_CONTEXT.load_verify_locations("/etc/ssl/cert.pem")


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def get_page(cache, offset, length, offline):
    destination = cache / f"rows_{offset}_{length}.json"
    if destination.exists():
        cached = json.loads(destination.read_text(encoding="utf-8"))
    else:
        if offline:
            raise ValueError(f"Missing cached source page: {destination}")
        parameters = urlencode(dict(dataset=DATASET, config="default", split="train",
                                    offset=offset, length=length))
        request = Request("https://datasets-server.huggingface.co/rows?" + parameters,
                          headers={"User-Agent": "AetherIndex-real-data-validation/1.0"})
        for attempt in range(4):
            try:
                # Stay below 30 new requests/minute on the public dataset API.
                time.sleep(2.1)
                with urlopen(request, timeout=45, context=TLS_CONTEXT) as response:
                    revision = response.headers.get("X-Revision")
                    if revision != REVISION:
                        raise ValueError(f"Unexpected source revision: {revision}; expected {REVISION}")
                    cached = {"revision": revision, "response": json.load(response)}
                temporary = destination.with_suffix(".part")
                temporary.write_text(json.dumps(cached, ensure_ascii=False), encoding="utf-8")
                temporary.replace(destination)
                break
            except (HTTPError, URLError, TimeoutError) as error:
                if isinstance(error, URLError) and isinstance(error.reason, ssl.SSLCertVerificationError):
                    raise
                if attempt == 3:
                    raise
                delay = 2 ** attempt
                if isinstance(error, HTTPError) and error.code == 429:
                    delay = min(60, float(error.headers.get("Retry-After", "60")))
                    print(f"Dataset API rate limit; retrying in {delay:g}s", flush=True)
                time.sleep(delay)
    if cached["revision"] != REVISION:
        raise ValueError(f"Cached revision mismatch: {destination}")
    rows = cached["response"]["rows"]
    if len(rows) != length:
        raise ValueError(f"Incomplete source page at offset {offset}")
    for index, item in enumerate(rows, start=offset):
        if item["row_idx"] != index or item.get("truncated_cells"):
            raise ValueError(f"Truncated or misordered source row {index}")
    return destination


def prepare(args):
    if args.vectors < 1 or args.queries < 1 or args.vectors + args.queries > 100000:
        raise ValueError("Positive vector/query counts must total at most 100,000 source rows.")
    output = args.output_dir.resolve()
    cache = output / "source-cache"
    cache.mkdir(parents=True, exist_ok=True)
    (output / "queries").mkdir(exist_ok=True)
    total = args.vectors + args.queries
    pages = [(offset, min(100, total - offset)) for offset in range(0, total, 100)]
    for count, (offset, length) in enumerate(pages, start=1):
        get_page(cache, offset, length, args.offline)
        if count % 10 == 0 or count == len(pages):
            print(f"Source pages ready: {count}/{len(pages)}", flush=True)

    seen_sources = set()
    dataset_fingerprints = set()
    queries = []
    source_pages = []
    with (output / "vectors.csv").open("w", encoding="utf-8", newline="\n") as vectors, \
            (output / "articles.jsonl").open("w", encoding="utf-8", newline="\n") as articles:
        for offset, length in pages:
            page = cache / f"rows_{offset}_{length}.json"
            source_pages.append({"offset": offset, "length": length, "sha256": sha256(page)})
            rows = json.loads(page.read_text(encoding="utf-8"))["response"]["rows"]
            for item in rows:
                source = item["row"]
                index = item["row_idx"]
                source_id = source["_id"]
                if source_id in seen_sources:
                    raise ValueError(f"Duplicate source article: {source_id}")
                seen_sources.add(source_id)
                embedding = array("f", source[EMBEDDING_COLUMN])
                if (len(embedding) != DIMENSION or not all(map(math.isfinite, embedding))
                        or not any(embedding)):
                    raise ValueError(f"Invalid real embedding at source row {index}")
                # Nine significant digits round-trip the float32 input components.
                # CLI output separately uses nine digits AFTER the decimal point.
                csv_embedding = ",".join(format(value, ".9g") for value in embedding) + "\n"
                fingerprint = hashlib.sha256(csv_embedding.encode("ascii")).hexdigest()
                metadata = {"source_row": index, "source_id": source_id,
                            "title": source["title"], "text": source["text"]}
                if index < args.vectors:
                    vector_id = 7 * index + 10
                    vectors.write(f"{vector_id}," + csv_embedding)
                    articles.write(json.dumps({"vector_id": vector_id, **metadata},
                                              ensure_ascii=False) + "\n")
                    dataset_fingerprints.add(fingerprint)
                else:
                    if fingerprint in dataset_fingerprints:
                        raise ValueError("A held-out query duplicates a stored embedding.")
                    query_index = index - args.vectors
                    relative = f"queries/query_{query_index:02d}.csv"
                    (output / relative).write_text(csv_embedding, encoding="ascii")
                    queries.append({"query_index": query_index, "file": relative, **metadata})
    (output / "queries.json").write_text(json.dumps(queries, indent=2, ensure_ascii=False) + "\n",
                                         encoding="utf-8")
    files = ["vectors.csv", "articles.jsonl", "queries.json"] + [q["file"] for q in queries]
    manifest = {
        "dataset": DATASET, "revision": REVISION, "split": "train", "config": "default",
        "source_url": "https://huggingface.co/datasets/" + DATASET,
        "embedding_model": "text-embedding-3-small", "embedding_column": EMBEDDING_COLUMN,
        "dimension": DIMENSION, "vectors": args.vectors, "queries": args.queries,
        "dataset_source_rows": [0, args.vectors - 1],
        "query_source_rows": [args.vectors, total - 1],
        "id_mapping": "VectorId = 7 * source_row + 10",
        "component_conversion": "Source float64 values rounded to float32; nine significant CSV digits",
        "use_case": "Content-based recommendation of related knowledge-base articles",
        "source_pages": source_pages,
        "sha256": {name: sha256(output / name) for name in files},
    }
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Prepared {args.vectors:,} real articles x {DIMENSION} dimensions and "
          f"{args.queries} held-out query articles in {output}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, default=ROOT / "datasets/real/dbpedia")
    parser.add_argument("--vectors", type=int, default=10000)
    parser.add_argument("--queries", type=int, default=20)
    parser.add_argument("--offline", action="store_true", help="Use existing source-cache only")
    args = parser.parse_args()
    try:
        prepare(args)
    except (OSError, ValueError, KeyError) as error:
        print(f"Real dataset preparation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
