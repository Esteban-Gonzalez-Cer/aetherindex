#!/usr/bin/env python3
"""Convert downloaded BEIR datasets into the inputs every compared system reads.

Python standard library only. Run with `python3 -I` (the zips are downloaded data).
For each dataset under --downloads/<name>.zip it writes datasets/market/beir/<name>/:

  corpus.jsonl      AetherIndex documents: document_id = ordinal (0..N-1), title, text.
  doc_ids.txt       Ordinal -> BEIR document id (line N is ordinal N).
  queries.jsonl     AetherIndex queries (test split, only queries with judgments).
  topics.tsv        qid<TAB>text for Anserini and the Python systems.
  qrels.tsv         TREC qrels (qid 0 docid relevance) for the test split.
  anserini/docs.jsonl  Anserini JsonCollection: {"id", "contents": title + "\\n" + text}.
  manifest.json     Zip SHA-256, counts and output hashes.

Every system indexes the same "title\\ntext" string, which is also what the AetherIndex
CLI indexes (title + text, no field boosts).
"""

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[2]
DATASETS = ["scifact", "nfcorpus", "fiqa", "arguana", "trec-covid", "quora"]


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def member(archive, name, relative):
    """Open <name>/<relative> inside the zip, refusing unexpected paths."""
    wanted = f"{name}/{relative}"
    for info in archive.infolist():
        path = PurePosixPath(info.filename)
        if path.is_absolute() or ".." in path.parts:
            raise ValueError(f"Unsafe path in archive: {info.filename}")
        if info.filename == wanted:
            return archive.open(info)
    raise FileNotFoundError(f"{wanted} not found in archive")


def read_jsonl(stream):
    for line_number, raw in enumerate(stream, 1):
        line = raw.decode("utf-8").strip()
        if line:
            try:
                yield json.loads(line)
            except json.JSONDecodeError as error:
                raise ValueError(f"Invalid JSON on line {line_number}: {error}") from error


def prepare(name, downloads, output):
    zip_path = downloads / f"{name}.zip"
    out = output / name
    (out / "anserini").mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(zip_path) as archive:
        with member(archive, name, "qrels/test.tsv") as stream:
            qrels = {}
            for index, raw in enumerate(stream):
                fields = raw.decode("utf-8").rstrip("\n").split("\t")
                if index == 0 and fields[0] == "query-id":
                    continue
                qid, docid, score = fields[0], fields[1], int(fields[2])
                qrels.setdefault(qid, {})[docid] = score
        with member(archive, name, "queries.jsonl") as stream:
            queries = {str(q["_id"]): q["text"] for q in read_jsonl(stream)}
        doc_ids = []
        with member(archive, name, "corpus.jsonl") as stream, \
                (out / "corpus.jsonl").open("w", encoding="utf-8") as corpus, \
                (out / "anserini" / "docs.jsonl").open("w", encoding="utf-8") as anserini:
            for doc in read_jsonl(stream):
                beir_id, title, text = str(doc["_id"]), doc.get("title") or "", doc["text"] or ""
                ordinal = len(doc_ids)
                doc_ids.append(beir_id)
                corpus.write(json.dumps({"document_id": ordinal, "title": title, "text": text},
                                        ensure_ascii=False) + "\n")
                anserini.write(json.dumps({"id": beir_id, "contents": f"{title}\n{text}"},
                                          ensure_ascii=False) + "\n")
    if len(set(doc_ids)) != len(doc_ids):
        raise ValueError(f"{name}: duplicate BEIR document ids")
    known = set(doc_ids)
    test_ids = sorted(qid for qid in qrels if qid in queries)
    missing = sorted(set(qrels) - set(queries))
    if missing:
        raise ValueError(f"{name}: {len(missing)} judged queries have no text")
    (out / "doc_ids.txt").write_text("\n".join(doc_ids) + "\n", encoding="utf-8")
    with (out / "queries.jsonl").open("w", encoding="utf-8") as stream, \
            (out / "topics.tsv").open("w", encoding="utf-8") as topics:
        for qid in test_ids:
            text = " ".join(queries[qid].split())  # TSV-safe; whitespace only.
            relevant = sorted(d for d, s in qrels[qid].items() if s > 0 and d in known)
            stream.write(json.dumps({"query_id": qid, "query_text": text,
                                     "relevant_document_ids": relevant},
                                    ensure_ascii=False) + "\n")
            topics.write(f"{qid}\t{text}\n")
    with (out / "qrels.tsv").open("w", encoding="utf-8") as stream:
        for qid in test_ids:
            for docid, score in sorted(qrels[qid].items()):
                stream.write(f"{qid} 0 {docid} {score}\n")
    manifest = {
        "dataset": f"BEIR {name}",
        "source": f"https://public.ukp.informatik.tu-darmstadt.de/thakur/BEIR/datasets/{name}.zip",
        "zip_sha256": sha256(zip_path),
        "documents": len(doc_ids),
        "test_queries": len(test_ids),
        "judgments": sum(len(qrels[q]) for q in test_ids),
        "indexed_text": "title + '\\n' + text",
        "outputs": {p.name: sha256(p) for p in sorted(out.glob("*")) if p.is_file()
                    and p.name != "manifest.json"},
    }
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"{name}: {len(doc_ids)} documents, {len(test_ids)} test queries")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--downloads", type=Path,
                        default=ROOT / "datasets/market/downloads/beir")
    parser.add_argument("--output", type=Path, default=ROOT / "datasets/market/beir")
    parser.add_argument("datasets", nargs="*", default=DATASETS)
    args = parser.parse_args()
    for name in args.datasets:
        if name not in DATASETS:
            sys.exit(f"Unknown dataset {name}; choose from {', '.join(DATASETS)}")
        prepare(name, args.downloads, args.output)


if __name__ == "__main__":
    main()
