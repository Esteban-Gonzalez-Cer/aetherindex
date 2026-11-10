#!/usr/bin/env python3
"""Scoreboard for the objective in docs/OBJECTIVE.md: every benchmark cell, won or not.

A cell is (benchmark, dataset, metric). AetherIndex's shipped configuration is compared
with the best competitor configuration measured for that cell. Reads the raw campaign
outputs under evaluation/reports/market/ (the .run files and datasets/market/ qrels are
needed for quality cells) and writes scoreboard.md and scoreboard.csv next to them.
Deterministic: the paired randomization test uses a fixed seed.

Statuses:
  won       strictly better (quality: p < 0.05; cost: interquartile ranges do not overlap,
            where both were recorded)
  won~      strictly better, but inside noise (p >= 0.05, or overlapping IQR)
  tied      equal at the reported precision
  lost      worse (quality: p < 0.05; cost: IQR do not overlap, where recorded)
  lost~     worse, but inside noise
  ceiling   both sides reach the exact-search optimum, which nobody can exceed
  n/c       not comparable (network servers against an in-process library)
"""

import csv
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
MARKET = ROOT / "evaluation/reports/market"
sys.path.insert(0, str(ROOT / "benchmarks/market"))
from evaluate import evaluate, randomization_test, read_qrels, read_run  # noqa: E402

BEIR = ["scifact", "nfcorpus", "fiqa", "arguana", "trec-covid", "quora"]
QUALITY_METRICS = ["ndcg@10", "recall@100", "mrr@10"]
QUALITY_TOLERANCE = 5e-5  # Half a unit in the fourth decimal, the precision we report.

# Quality benchmarks: (name, our run, competitor runs, ceiling run or None).
# Our run is the configuration AetherIndex ships for that benchmark, never a per-dataset pick.
QUALITY_BENCHMARKS = [
    ("BM25 (k1=0.9, b=0.4)", "aether-bm25-english-freq",
     {"Lucene": "lucene-bm25-porter", "bm25s": "bm25s-default",
      "bm25s (english-v1 tokens)": "bm25s-matched", "Qdrant": "servers:qdrant-bm25",
      "OpenSearch": "servers:opensearch-bm25"}, None),
    ("BM25 (k1=1.2, b=0.75)", "aether-bm25-english-freq-k1.2",
     {"Lucene": "lucene-bm25-porter-k1.2"}, None),
    ("BM25 without stemming", "aether-bm25-plain",
     {"Lucene": "lucene-bm25-none-keepstop"}, None),
    ("Dense HNSW (bge-small)", "aether-dense-hnsw",
     {"Qdrant": "servers:qdrant-dense", "OpenSearch": "servers:opensearch-dense"}, "exact-dense"),
    ("Hybrid", "aether-hybrid-freq-convex",
     {"Lucene + exact + RRF": "reference-hybrid", "Qdrant": "servers:qdrant-hybrid",
      "OpenSearch": "servers:opensearch-hybrid"}, None),
]

ANN_OURS = {"aether-hnsw", "aether-sq8", "aether-sq8-lean", "aether-pq"}
ANN_LABELS = {"aether-hnsw": "AetherIndex HNSW", "aether-sq8": "AetherIndex SQ8 + re-rank",
              "aether-sq8-lean": "AetherIndex SQ8 without FP32", "aether-pq": "AetherIndex PQ + re-rank",
              "hnswlib": "hnswlib", "hnswlib-fastmath": "hnswlib -ffast-math",
              "faiss-hnsw": "FAISS HNSWFlat", "faiss-hnsw-sq8": "FAISS HNSWSQ8",
              "faiss-ivfpq-refine": "FAISS IVFPQ + refine", "faiss-flat": "FAISS Flat"}


def run_path(dataset, relative):
    if relative.startswith("servers:"):
        return MARKET / "servers/beir" / dataset / f"{relative[8:]}.run"
    return MARKET / "beir" / dataset / f"{relative}.run"


def load_json(path):
    return json.loads(path.read_text(encoding="utf-8")) if path.exists() else None


def read_csv(path):
    if not path.exists():
        return []
    with path.open(encoding="utf-8") as stream:
        return list(csv.DictReader(stream))


def cell(benchmark, dataset, metric, ours, best, competitor, status, note="", higher=True,
         unit=""):
    if ours is not None and best is not None:
        if unit == "nDCG" or metric in QUALITY_METRICS:
            margin = f"{ours - best:+.4f}"
        else:
            ratio = ours / best if higher else best / ours
            margin = f"{ratio:.2f}x"
    else:
        margin = ""
    return {"benchmark": benchmark, "dataset": dataset, "metric": metric, "ours": ours,
            "best_competitor": competitor, "theirs": best, "margin": margin, "status": status,
            "note": note}


def quality_cells():
    cells = []
    for dataset in BEIR:
        qrels_path = ROOT / "datasets/market/beir" / dataset / "qrels.tsv"
        if not qrels_path.exists():
            continue
        qrels = read_qrels(qrels_path)
        cache = {}

        def per_query(relative):
            if relative not in cache:
                path = run_path(dataset, relative)
                cache[relative] = evaluate(qrels, read_run(path)) if path.exists() else None
            return cache[relative]

        for name, ours_run, competitors, ceiling_run in QUALITY_BENCHMARKS:
            ours = per_query(ours_run)
            if not ours:
                continue
            ceiling = per_query(ceiling_run) if ceiling_run else None
            for metric in QUALITY_METRICS:
                measured = {label: per_query(run) for label, run in competitors.items()}
                measured = {label: r for label, r in measured.items() if r}
                if not measured:
                    continue
                label, theirs = max(measured.items(), key=lambda item: item[1][0][metric])
                mine, best = ours[0][metric], theirs[0][metric]
                server = competitors[label].startswith("servers:")
                note = "server measured over the network" if server else ""
                if ceiling and best >= ceiling[0][metric] - QUALITY_TOLERANCE and \
                        mine >= ceiling[0][metric] - QUALITY_TOLERANCE:
                    status = "ceiling"
                    note = f"exact search {ceiling[0][metric]:.4f}"
                elif abs(mine - best) < QUALITY_TOLERANCE:
                    status = "tied"
                else:
                    qids = sorted(ours[1])
                    p = randomization_test([ours[1][q][metric] for q in qids],
                                           [theirs[1][q][metric] for q in qids])
                    status = ("won" if mine > best else "lost") + ("" if p < 0.05 else "~")
                    note = f"p = {p:.3f}" + (f"; {note}" if note else "")
                cells.append(cell(name, dataset, metric, mine, best, label, status, note))
    return cells


def compare_cost(benchmark, dataset, metric, ours, competitors, higher, note=""):
    """ours/competitors: (value, q1, q3) with q1/q3 None when no IQR was recorded."""
    competitors = {k: v for k, v in competitors.items() if v and v[0] is not None}
    if ours is None or ours[0] is None or not competitors:
        return None
    pick = max if higher else min
    label, theirs = pick(competitors.items(), key=lambda item: item[1][0])
    better = ours[0] > theirs[0] if higher else ours[0] < theirs[0]
    if ours[0] == theirs[0]:
        status = "tied"
    else:
        overlap = None not in (ours[1], ours[2], theirs[1], theirs[2]) and \
            ours[1] <= theirs[2] and theirs[1] <= ours[2]
        status = ("won" if better else "lost") + ("~" if overlap else "")
        if overlap:
            note = (note + "; " if note else "") + "IQR overlap"
    return cell(benchmark, dataset, metric, ours[0], theirs[0], label, status, note, higher)


def lexical_cost_cells():
    cells = []
    for dataset in BEIR:
        folder = MARKET / "beir" / dataset
        ours = load_json(folder / "aether-bm25-english-freq-maxscore.json")
        lucene = load_json(folder / "lucene-bm25-porter.json") or {}
        lucene_index = load_json(folder / "lucene-index-porter.json") or {}
        bm25s = load_json(folder / "bm25s-default.json") or {}
        if not ours:
            continue
        bm = ours.get("bm25", {})

        def iqr(data):
            return (data.get("qps_batch"), data.get("qps_batch_q1"), data.get("qps_batch_q3")) \
                if data else None

        def single(value):
            return (value, None, None) if value is not None else None

        rows = [
            ("BM25 latency p50, 1 thread (ms)", single(ours.get("lat_p50_ms")),
             {"Lucene": single(lucene.get("lat_p50_ms")), "bm25s": single(bm25s.get("lat_p50_ms"))},
             False, ""),
            ("BM25 throughput, 10 threads (QPS)", iqr(ours),
             {"Lucene": iqr(lucene), "bm25s": iqr(bm25s)}, True, ""),
            ("BM25 index build, 1 thread (s)", single(bm.get("build_seconds_single_thread")),
             {"Lucene": single(lucene_index.get("wall_seconds")),
              "bm25s": single(bm25s.get("build_seconds_single_thread"))}, False,
             "Lucene includes JVM start-up; bm25s excludes tokenization"),
            ("BM25 index size on disk (MB)",
             single(bm["index_bytes"] / 1e6 if "index_bytes" in bm else None),
             {"Lucene": single(lucene_index["index_bytes"] / 1e6 if "index_bytes" in lucene_index else None),
              "bm25s": single(bm25s["index_bytes"] / 1e6 if "index_bytes" in bm25s else None)},
             False, ""),
        ]
        for metric, mine, theirs, higher, note in rows:
            result = compare_cost("BM25 cost", dataset, metric, mine, theirs, higher, note)
            if result:
                cells.append(result)
    return cells


def server_cells():
    cells = []
    for dataset in BEIR:
        for name, ours_run, servers in (
                ("Dense HNSW cost", "aether-dense-hnsw",
                 ("servers:qdrant-dense", "servers:opensearch-dense")),
                ("Hybrid cost", "aether-hybrid-freq-convex",
                 ("servers:qdrant-hybrid", "servers:opensearch-hybrid"))):
            ours = load_json(run_path(dataset, ours_run).with_suffix(".json"))
            theirs = {s[8:]: load_json(run_path(dataset, s).with_suffix(".json")) for s in servers}
            theirs = {k: v for k, v in theirs.items() if v and "lat_p50_ms" in v}
            if not ours or not theirs:
                continue
            label, best = min(theirs.items(), key=lambda item: item[1]["lat_p50_ms"])
            cells.append(cell(name, dataset, "latency p50, 1 thread (ms)", ours["lat_p50_ms"],
                              best["lat_p50_ms"], label, "n/c",
                              "server latency includes the local network", higher=False))
    return cells


def best_at(rows, systems, target, column, higher=True):
    ok = [r for r in rows if r["system"] in systems and float(r["recall10"]) >= target]
    if not ok:
        return None, None
    pick = max if higher else min
    row = pick(ok, key=lambda r: float(r[column]))
    q1 = float(row["qps_batch_q1"]) if column == "qps_batch" and row.get("qps_batch_q1") else None
    q3 = float(row["qps_batch_q3"]) if column == "qps_batch" and row.get("qps_batch_q3") else None
    return (float(row[column]), q1, q3), ANN_LABELS.get(row["system"], row["system"])


def ann_cells():
    cells = []
    for dataset in ("sift1m", "glove-100"):
        rows = read_csv(MARKET / "ann" / f"{dataset}.csv")
        if not rows:
            continue
        for row in rows:
            if row["system"] == "hnswlib" and row["variant"] == "fastmath":
                row["system"] = "hnswlib-fastmath"
        theirs_systems = {r["system"] for r in rows} - ANN_OURS
        targets = [(0.90, "qps_batch", True, "throughput @R@10>=0.90, 10 threads (QPS)"),
                   (0.95, "qps_batch", True, "throughput @R@10>=0.95, 10 threads (QPS)"),
                   (0.99, "qps_batch", True, "throughput @R@10>=0.99, 10 threads (QPS)"),
                   (0.95, "qps_single", True, "throughput @R@10>=0.95, 1 thread (QPS)"),
                   (0.95, "lat_p50_ms", False, "latency p50 @R@10>=0.95, 1 thread (ms)")]
        for target, column, higher, metric in targets:
            mine, mine_label = best_at(rows, ANN_OURS, target, column, higher)
            best, label = best_at(rows, theirs_systems, target, column, higher)
            if mine is None and best is None:
                cells.append(cell("ANN", dataset, metric, None, None, "", "n/a",
                                  "no system reaches the target"))
                continue
            if mine is None:
                cells.append(cell("ANN", dataset, metric, None, best[0], label, "lost",
                                  "AetherIndex does not reach the target"))
                continue
            if best is None:
                cells.append(cell("ANN", dataset, metric, mine[0], None, "", "won",
                                  "no competitor reaches the target"))
                continue
            result = compare_cost("ANN", dataset, metric, mine, {label: best}, higher,
                                  f"ours: {mine_label}")
            cells.append(result)

        def build_row(system):
            mine = [r for r in rows if r["system"] == system and r["M"] == "16"
                    and r["ef_construction"] == "200"]
            return mine[0] if mine else None

        fp32 = ("hnswlib", "hnswlib-fastmath", "faiss-hnsw")
        ours_row = build_row("aether-hnsw")
        if ours_row:
            competitors = {ANN_LABELS[s]: (float(build_row(s)["build_seconds"]), None, None)
                           for s in fp32 if build_row(s)}
            cells.append(compare_cost("ANN", dataset, "HNSW build, 10 threads (s)",
                                      (float(ours_row["build_seconds"]), None, None),
                                      competitors, False))
            competitors = {ANN_LABELS[s]: (float(build_row(s)["library_mib"]), None, None)
                           for s in fp32 if build_row(s)}
            cells.append(compare_cost("ANN", dataset, "HNSW structure memory (MiB)",
                                      (float(ours_row["library_mib"]), None, None),
                                      competitors, False))
        lean, sq8 = build_row("aether-sq8-lean"), build_row("faiss-hnsw-sq8")
        if lean and sq8:
            cells.append(compare_cost("ANN", dataset, "SQ8 structure memory (MiB)",
                                      (float(lean["library_mib"]), None, None),
                                      {ANN_LABELS["faiss-hnsw-sq8"]: (float(sq8["library_mib"]), None, None)},
                                      False))
        single = {r["system"]: r for r in read_csv(MARKET / "ann" / f"{dataset}_build1.csv")}
        if "aether-hnsw" in single:
            competitors = {ANN_LABELS[s]: (float(single[s]["build_seconds"]), None, None)
                           for s in fp32 if s in single}
            cells.append(compare_cost("ANN", dataset, "HNSW build, 1 thread (s)",
                                      (float(single["aether-hnsw"]["build_seconds"]), None, None),
                                      competitors, False))
        for r in read_csv(MARKET / "servers/ann_servers.csv"):
            if r["dataset"] == dataset:
                cells.append(cell("ANN servers", dataset,
                                  f"{r['system']} ef={r['param']} latency p50 (ms)",
                                  None, float(r["lat_p50_ms"]), r["system"], "n/c",
                                  "no AetherIndex server", higher=False))
    return [c for c in cells if c]


def beir_ann_cells():
    rows = read_csv(MARKET / "beir-ann/beir_ann.csv")
    cells = []
    for dataset in BEIR:
        mine_rows = [r for r in rows if r["dataset"] == dataset]
        if not mine_rows:
            continue
        others = {r["system"] for r in mine_rows} - ANN_OURS
        for target in (0.95, 0.99):
            metric = f"throughput @R@10>={target:.2f}, 10 threads (QPS)"
            mine, _ = best_at(mine_rows, {"aether-hnsw"}, target, "qps_batch")
            best, label = best_at(mine_rows, others, target, "qps_batch")
            if mine is None and best is None:
                cells.append(cell("Dense HNSW cost (bge-small)", dataset, metric, None, None, "",
                                  "n/a", "no system reaches the target"))
            elif mine is None:
                cells.append(cell("Dense HNSW cost (bge-small)", dataset, metric, None, best[0],
                                  label, "lost", "AetherIndex does not reach the target"))
            elif best is None:
                cells.append(cell("Dense HNSW cost (bge-small)", dataset, metric, mine[0], None,
                                  "", "won", "no competitor reaches the target"))
            else:
                cells.append(compare_cost("Dense HNSW cost (bge-small)", dataset, metric, mine,
                                          {label: best}, True))
    return cells


def fmt(value):
    if value is None:
        return "—"
    if abs(value) >= 1000:
        return f"{value:,.0f}"
    if abs(value) >= 10:
        return f"{value:.1f}"
    return f"{value:.4f}" if abs(value) < 1 else f"{value:.3f}"


def main():
    cells = quality_cells() + lexical_cost_cells() + beir_ann_cells() + ann_cells() + server_cells()
    with (MARKET / "scoreboard.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(cells[0].keys()))
        writer.writeheader()
        writer.writerows(cells)

    counts = {}
    for c in cells:
        counts[c["status"]] = counts.get(c["status"], 0) + 1
    scored = [c for c in cells if c["status"] not in ("n/c", "n/a")]
    lost = sum(1 for c in scored if c["status"] in ("lost", "lost~"))
    won = sum(1 for c in scored if c["status"] in ("won", "won~"))
    manifest = load_json(MARKET / "manifest.json") or {}
    lines = ["# Scoreboard", "",
             "Generated by `scripts/scoreboard.py` from the raw campaign results in this folder. "
             "Do not edit by hand. The rule is in [docs/OBJECTIVE.md](../../../docs/OBJECTIVE.md).", ""]
    if manifest:
        lines += [f"Campaign: commit `{manifest.get('git_commit', '?')}`, "
                  f"{manifest.get('machine', {}).get('cpu', '?')}, {manifest.get('compiler', '?')}.", ""]
    lines += [f"**{won} of {len(scored)} comparable cells won** "
              f"({counts.get('won', 0)} clearly, {counts.get('won~', 0)} inside noise); "
              f"{lost} lost ({counts.get('lost~', 0)} of them inside noise).", "",
              "| Status | Cells |", "|---|---:|"]
    for status in ("won", "won~", "tied", "ceiling", "lost~", "lost", "n/a", "n/c"):
        lines.append(f"| {status} | {counts.get(status, 0)} |")
    lines += ["", "`~` marks a result inside noise: p ≥ 0.05, or overlapping interquartile ranges. "
              "`ceiling`: both sides match exact search, which cannot be exceeded. `n/c`: not "
              "comparable, a network server against an in-process library.", ""]

    open_cells = [c for c in cells if c["status"] in ("lost", "lost~", "tied")]
    lines += ["## Open cells", "", "Every comparable cell not yet won, by dataset.",
              "", "| Benchmark | Dataset | Metric | AetherIndex | Best competitor | Theirs | Margin | Status | Note |",
              "|---|---|---|---:|---|---:|---:|---|---|"]
    for c in open_cells:
        lines.append(f"| {c['benchmark']} | {c['dataset']} | {c['metric']} | {fmt(c['ours'])} | "
                     f"{c['best_competitor']} | {fmt(c['theirs'])} | {c['margin']} | {c['status']} | "
                     f"{c['note']} |")
    benchmarks = []
    for c in cells:
        if c["benchmark"] not in benchmarks:
            benchmarks.append(c["benchmark"])
    lines += ["", "## All cells", ""]
    for benchmark in benchmarks:
        lines += [f"### {benchmark}", "",
                  "| Dataset | Metric | AetherIndex | Best competitor | Theirs | Margin | Status | Note |",
                  "|---|---|---:|---|---:|---:|---|---|"]
        for c in cells:
            if c["benchmark"] == benchmark:
                lines.append(f"| {c['dataset']} | {c['metric']} | {fmt(c['ours'])} | "
                             f"{c['best_competitor']} | {fmt(c['theirs'])} | {c['margin']} | "
                             f"{c['status']} | {c['note']} |")
        lines.append("")
    (MARKET / "scoreboard.md").write_text("\n".join(lines), encoding="utf-8")
    print(f"Wrote {MARKET / 'scoreboard.md'}: {won} of {len(scored)} comparable cells won; "
          + ", ".join(f"{k} {v}" for k, v in sorted(counts.items())))


if __name__ == "__main__":
    main()
