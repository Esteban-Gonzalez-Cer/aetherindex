"""Recheck saved CLI results, library traces, corpus identity and expected failures."""
from decimal import Decimal
from fractions import Fraction
import hashlib
import json
from pathlib import Path
import re


def load(path):
    return json.loads(path.read_text(), parse_float=Decimal)


def cli_rows(path):
    queries = {}
    current = None
    for line in path.read_text().splitlines():
        if line.startswith("Query ID: "):
            current = line.removeprefix("Query ID: ")
            assert current not in queries, "Duplicate query block"
            queries[current] = []
        if line.startswith("|") and current:
            cells = [cell.strip() for cell in line.split("|")[1:-1]]
            if cells and cells[0].isdigit():
                queries[current].append(cells)
    return queries


def main():
    folder = Path(__file__).resolve().parent
    root = folder.parents[3]
    probe = load(folder / "1.4-1.8-corpus.stdout.txt")
    assert probe["passed"] and probe["same_corpus"]
    assert probe["documents"] == probe["dense_vectors"] == probe["lexical_documents"] == 30
    assert probe["dimension"] == 128 and probe["queries_passed"] == 24
    docs = [json.loads(line) for line in (root / "datasets/hybrid/corpus.jsonl").read_text().splitlines() if line.strip()]
    original = [json.loads(line) for line in (root / "datasets/corpus.jsonl").read_text().splitlines() if line.strip()]
    originals = {doc["document_id"]: doc for doc in original}
    assert len(docs) == len(originals) == 30
    for doc in docs:
        for key in ("document_id", "title", "text", "topic"):
            assert doc[key] == originals[doc["document_id"]][key]
        assert len(doc["embedding"]) == 128
    queries = [json.loads(line) for line in (root / "datasets/hybrid/queries.jsonl").read_text().splitlines() if line.strip()]
    original_queries = {q["query_id"]: q for q in (json.loads(line) for line in (root / "datasets/queries.jsonl").read_text().splitlines() if line.strip())}
    assert len(queries) == len(original_queries) == 24
    for query in queries:
        for key in ("query_id", "query_text", "relevant_document_ids"):
            assert query[key] == original_queries[query["query_id"]][key]
        assert len(query["embedding"]) == 128
    known = {doc["document_id"] for doc in docs}
    binding = {row["document_id"]: row["source_id"] for row in probe["bindings"]}
    assert len(binding) == 30 and set(binding.values()) == known
    dense_cli = cli_rows(folder / "1.1-dense-all.stdout.txt")
    lexical_cli = cli_rows(folder / "1.2-lexical-all.stdout.txt")
    hybrid_cli = cli_rows(folder / "1.3-hybrid-all.stdout.txt")
    wanted_queries = [q["query_id"] for q in queries]
    assert list(dense_cli) == list(lexical_cli) == list(hybrid_cli) == wanted_queries
    maximum_full_error = Fraction(0)
    maximum_display_error = Fraction(0)
    total_fused, total_returned = 0, 0
    for result in probe["queries"]:
        qid = result["query_id"]
        dense, lexical = result["dense_independent_and_to_rrf"], result["lexical_independent_and_to_rrf"]
        fused = result["fused_union"]
        for output, reference, score_key in ((dense_cli[qid], dense, "distance"), (lexical_cli[qid], lexical, "score")):
            assert len(output) == len(reference)
            for index, (cells, row) in enumerate(zip(output, reference), 1):
                assert len(cells) == 3 and int(cells[0]) == row["rank"] == index
                assert cells[1] == row["source_id"] == binding[row["document_id"]]
                assert re.fullmatch(r"\d+\.\d{9}", cells[2])
                assert abs(Decimal(cells[2]) - row[score_key]) <= Decimal("0.0000000005001")
        dense_rank = {row["source_id"]: row["rank"] for row in dense}
        lexical_rank = {row["source_id"]: row["rank"] for row in lexical}
        union = set(dense_rank) | set(lexical_rank)
        assert len(fused) == len(union) == len({r["source_id"] for r in fused})
        assert {r["source_id"] for r in fused} == union and union <= known
        previous = None
        for row in fused:
            expected = sum((Fraction(1, 60 + ranks[row["source_id"]])
                            for ranks in (dense_rank, lexical_rank) if row["source_id"] in ranks), Fraction(0))
            error = abs(Fraction(row["score"]) - expected)
            assert error <= Fraction(1, 10**12)
            maximum_full_error = max(maximum_full_error, error)
            if previous:
                assert previous["score"] > row["score"] or (previous["score"] == row["score"] and previous["document_id"] < row["document_id"])
            previous = row
        assert len(hybrid_cli[qid]) == min(10, len(union))
        for cells, row in zip(hybrid_cli[qid], fused[:10]):
            assert len(cells) == 5 and int(cells[0]) == row["rank"]
            assert cells[1] == row["source_id"] and cells[1] in known
            assert re.fullmatch(r"\d+\.\d{12}", cells[2])
            for cell, ranks in ((cells[3], dense_rank), (cells[4], lexical_rank)):
                assert cell == str(ranks[cells[1]]) if cells[1] in ranks else cell == "-"
            error = abs(Fraction(Decimal(cells[2])) - Fraction(row["score"]))
            assert error <= Fraction(5001, 10**16)
            maximum_display_error = max(maximum_display_error, error)
        total_fused += len(fused)
        total_returned += len(hybrid_cli[qid])
    for name, batch in (("1.1-dense", dense_cli), ("1.2-lexical", lexical_cli), ("1.3-hybrid", hybrid_cli)):
        single = cli_rows(folder / (name + ".stdout.txt"))
        assert single == {"Q001": batch["Q001"]}, "Single-query and batch output disagree"
    commands = load(folder / "commands.json")
    assert all(entry["passed"] for entry in commands)
    for entry in commands:
        if entry["id"].startswith(("1.9-", "1.10-")):
            assert (folder / entry["stdout"]).read_text() == ""
            assert (folder / entry["stderr"]).read_text().strip()
    assert load(folder / "1.5-explicit-bindings.stdout.txt")["passed"]
    assert load(folder / "1.11-architecture.stdout.txt")["passed"]
    snapshot = load(folder / "snapshot-before.json")
    changed = [name for name, value in snapshot["sha256"].items()
               if hashlib.sha256((root / name).read_bytes()).hexdigest() != value]
    assert not changed, ("Sources changed during validation; rebuild required", changed)
    summary = {"passed": True, "checklist_passed": 11, "checklist_total": 11,
               "documents": 30, "dimensions": 128, "queries": 24,
               "full_fused_rows_verified": total_fused, "cli_top_k_rows_verified": total_returned,
               "maximum_full_precision_error_vs_exact_fractions": float(maximum_full_error),
               "full_precision_tolerance": 1e-12,
               "maximum_12_decimal_presentation_error": float(maximum_display_error),
               "same_document_text_and_query_labels_as_original_corpus": True,
               "source_and_input_hashes_unchanged": True,
               "scope": "Classic unweighted RRF, FP32 HNSW/cosine, BM25 defaults, functional integration on the hashing corpus; not a semantic or performance benchmark."}
    (folder / "verification-summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
