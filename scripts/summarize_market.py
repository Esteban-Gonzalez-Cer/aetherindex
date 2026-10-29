#!/usr/bin/env python3
"""Builds every table of the market comparison from the raw campaign outputs.

Reads evaluation/reports/market/ (written by benchmarks/market/run_market_campaign.py and
run_servers.py) and writes evaluation/reports/market/summary.md, quality.csv,
ann_targets.csv and, when matplotlib is available (run with .venv-bench/bin/python),
SVG Pareto plots under evaluation/reports/market/plots/. Deterministic: the same raw
files always give the same tables (the paired randomization test uses a fixed seed).
"""

import csv
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
MARKET = ROOT / "evaluation/reports/market"
sys.path.insert(0, str(ROOT / "benchmarks/market"))
from evaluate import evaluate, randomization_test, read_qrels, read_run  # noqa: E402

BEIR = ["scifact", "nfcorpus", "fiqa", "arguana", "trec-covid", "quora"]
# Anserini BEIR regressions, bm25-flat, nDCG@10 (Lucene, k1=0.9, b=0.4, Porter).
PUBLISHED_LUCENE = {"scifact": 0.6789, "nfcorpus": 0.3218, "fiqa": 0.2361, "arguana": 0.3970,
                    "trec-covid": 0.5947, "quora": 0.7886}
QUALITY_RUNS = [
    # (key, label, family, path relative to beir/<dataset> or servers/beir/<dataset>)
    ("lucene", "Lucene BM25 (Porter)", "BM25", "lucene-bm25-porter"),
    ("aether-bm25", "AetherIndex BM25 english-v1", "BM25", "aether-bm25-english"),
    ("aether-bm25-freq", "AetherIndex BM25 english-v1, frecuencia", "BM25", "aether-bm25-english-freq"),
    ("bm25s-matched", "bm25s (tokens english-v1)", "BM25", "bm25s-matched"),
    ("bm25s", "bm25s (por defecto)", "BM25", "bm25s-default"),
    ("qdrant-bm25", "Qdrant sparse BM25", "BM25", "servers:qdrant-bm25"),
    ("opensearch-bm25", "OpenSearch BM25", "BM25", "servers:opensearch-bm25"),
    ("lucene-plain", "Lucene sin stemming/stopwords", "BM25 sin stemming", "lucene-bm25-none-keepstop"),
    ("aether-bm25-ascii", "AetherIndex BM25 ascii-tech-v1", "BM25 sin stemming", "aether-bm25-ascii"),
    ("lucene-k1.2", "Lucene BM25 k1=1.2 b=0.75", "BM25 k1=1.2", "lucene-bm25-porter-k1.2"),
    ("aether-bm25-k1.2", "AetherIndex BM25 k1=1.2 b=0.75", "BM25 k1=1.2", "aether-bm25-english-k1.2"),
    ("aether-bm25-freq-k1.2", "AetherIndex BM25 k1=1.2 b=0.75, frecuencia", "BM25 k1=1.2",
     "aether-bm25-english-freq-k1.2"),
    ("exact", "Exacto (fuerza bruta)", "Denso", "exact-dense"),
    ("aether-dense", "AetherIndex HNSW FP32", "Denso", "aether-dense-hnsw"),
    ("aether-sq8", "AetherIndex HNSW SQ8", "Denso", "aether-dense-sq8"),
    ("qdrant-dense", "Qdrant HNSW", "Denso", "servers:qdrant-dense"),
    ("opensearch-dense", "OpenSearch k-NN", "Denso", "servers:opensearch-dense"),
    ("aether-hybrid", "AetherIndex híbrido RRF", "Híbrido", "aether-hybrid"),
    ("aether-hybrid-freq", "AetherIndex híbrido RRF, frecuencia", "Híbrido", "aether-hybrid-freq"),
    ("reference-hybrid", "Referencia: Lucene + exacto + RRF", "Híbrido", "reference-hybrid"),
    ("qdrant-hybrid", "Qdrant híbrido RRF", "Híbrido", "servers:qdrant-hybrid"),
    ("opensearch-hybrid", "OpenSearch híbrido RRF", "Híbrido", "servers:opensearch-hybrid"),
]
COMPARISONS = [("aether-bm25", "lucene"), ("aether-bm25-freq", "lucene"),
               ("aether-bm25-freq", "aether-bm25"), ("aether-bm25-freq-k1.2", "lucene-k1.2"),
               ("aether-bm25-ascii", "lucene-plain"),
               ("aether-hybrid", "reference-hybrid"), ("aether-hybrid-freq", "reference-hybrid"),
               ("aether-hybrid", "aether-bm25"), ("aether-hybrid", "exact"),
               ("aether-dense", "exact")]


def run_path(dataset, relative):
    if relative.startswith("servers:"):
        return MARKET / "servers/beir" / dataset / f"{relative[8:]}.run"
    return MARKET / "beir" / dataset / f"{relative}.run"


def load_json(path):
    return json.loads(path.read_text(encoding="utf-8")) if path.exists() else None


def fmt(value, digits=4):
    return "—" if value is None else f"{value:.{digits}f}"


def quality():
    rows, per_query, lines = [], {}, []
    for dataset in BEIR:
        qrels = read_qrels(ROOT / "datasets/market/beir" / dataset / "qrels.tsv")
        for key, label, family, relative in QUALITY_RUNS:
            path = run_path(dataset, relative)
            if not path.exists():
                continue
            means, queries = evaluate(qrels, read_run(path))
            per_query[(dataset, key)] = queries
            rows.append({"dataset": dataset, "system": key, "label": label, "family": family,
                         **{m: round(v, 6) for m, v in means.items()}})
    with (MARKET / "quality.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)
    value = {(r["dataset"], r["system"]): r for r in rows}
    for metric, title in (("ndcg@10", "nDCG@10"), ("recall@100", "Recall@100")):
        lines += [f"### {title}", "", "| Sistema | " + " | ".join(BEIR) + " | Media |",
                  "|---|" + "---:|" * (len(BEIR) + 1)]
        family = None
        for key, label, fam, _ in QUALITY_RUNS:
            cells = [value.get((d, key), {}).get(metric) for d in BEIR]
            if all(c is None for c in cells):
                continue
            if fam != family:
                lines.append(f"| **{fam}** |" + " |" * (len(BEIR) + 1))
                family = fam
            mean = sum(cells) / len(cells) if all(c is not None for c in cells) else None
            lines.append(f"| {label} | " + " | ".join(fmt(c) for c in cells) + f" | {fmt(mean)} |")
        lines.append("")
    lines += ["### Significancia (prueba de aleatorización pareada, nDCG@10, 10,000 permutaciones)",
              "", "| A vs B | " + " | ".join(BEIR) + " |", "|---|" + "---:|" * len(BEIR)]
    for a, b in COMPARISONS:
        cells = []
        for dataset in BEIR:
            if (dataset, a) not in per_query or (dataset, b) not in per_query:
                cells.append("—")
                continue
            qa, qb = per_query[(dataset, a)], per_query[(dataset, b)]
            qids = sorted(qa)
            diff = sum(qa[q]["ndcg@10"] - qb[q]["ndcg@10"] for q in qids) / len(qids)
            p = randomization_test([qa[q]["ndcg@10"] for q in qids], [qb[q]["ndcg@10"] for q in qids])
            cells.append(f"{diff:+.4f} (p={p:.3f}{', sig.' if p < 0.05 else ''})")
        label_a = next(l for k, l, _, _ in QUALITY_RUNS if k == a)
        label_b = next(l for k, l, _, _ in QUALITY_RUNS if k == b)
        lines.append(f"| {label_a} vs {label_b} | " + " | ".join(cells) + " |")
    lines += ["", "### Control: Lucene frente a su resultado publicado (nDCG@10)", "",
              "| Dataset | Publicado (Anserini bm25-flat) | Medido | Diferencia |", "|---|---:|---:|---:|"]
    for dataset in BEIR:
        measured = value.get((dataset, "lucene"), {}).get("ndcg@10")
        published = PUBLISHED_LUCENE[dataset]
        lines.append(f"| {dataset} | {published:.4f} | {fmt(measured)} | "
                     f"{'—' if measured is None else f'{measured - published:+.4f}'} |")
    return lines


def trec_eval_check():
    """Our evaluator against Anserini's bundled trec_eval on the Lucene and AetherIndex runs."""
    jar = ROOT / "tools/anserini/anserini-2.3.0-fatjar.jar"
    lines = ["### Control: evaluador propio frente a trec_eval (nDCG@10 / Recall@100)", "",
             "| Dataset | Run | Propio | trec_eval | ", "|---|---|---|---|"]
    worst = 0.0
    for dataset in BEIR:
        qrels_path = ROOT / "datasets/market/beir" / dataset / "qrels.tsv"
        qrels = read_qrels(qrels_path)
        for relative in ("lucene-bm25-porter", "aether-bm25-english", "aether-bm25-english-freq",
                         "aether-hybrid"):
            path = MARKET / "beir" / dataset / f"{relative}.run"
            if not path.exists() or not jar.exists():
                continue
            clean = path.with_suffix(".clean")
            clean.write_text("".join(l + "\n" for l in path.read_text().splitlines()
                                     if l.split()[0] != l.split()[2]), encoding="utf-8")
            output = subprocess.run(["java", "-cp", str(jar), "trec_eval", "-c", "-m", "ndcg_cut.10",
                                     "-m", "recall.100", str(qrels_path), str(clean)],
                                    capture_output=True, text=True).stdout
            clean.unlink()
            official = {l.split()[0]: float(l.split()[2]) for l in output.splitlines()
                        if len(l.split()) == 3 and l.split()[1] == "all"}
            means, _ = evaluate(qrels, read_run(path))
            ours = (means["ndcg@10"], means["recall@100"])
            theirs = (official.get("ndcg_cut_10"), official.get("recall_100"))
            if None not in theirs:
                worst = max(worst, abs(ours[0] - theirs[0]), abs(ours[1] - theirs[1]))
            lines.append(f"| {dataset} | {relative} | {ours[0]:.4f} / {ours[1]:.4f} | "
                         f"{fmt(theirs[0])} / {fmt(theirs[1])} |")
    lines += ["", f"Diferencia máxima: {worst:.5f} (trec_eval imprime 4 decimales).", ""]
    return lines


def latency():
    lines = ["### Latencia de consulta (k=10, un hilo) y throughput (10 hilos)", "",
             "Mediana de 5 repeticiones tras calentamiento. Servidores: medido desde el cliente "
             "(incluye HTTP/gRPC local); no comparable 1:1 con librerías en proceso.", "",
             "| Dataset | Sistema | p50 ms | p99 ms | QPS 10 hilos (IQR) |", "|---|---|---:|---:|---:|"]
    systems = [("aether-bm25-english", "AetherIndex BM25 (BMW)"),
               ("aether-bm25-english-exhaustive", "AetherIndex BM25 (exhaustivo)"),
               ("aether-bm25-english-freq", "AetherIndex BM25 frecuencia (BMW)"),
               ("aether-bm25-english-freq-exhaustive", "AetherIndex BM25 frecuencia (exhaustivo)"),
               ("lucene-bm25-porter", "Lucene BM25"), ("bm25s-default", "bm25s"),
               ("aether-dense-hnsw", "AetherIndex HNSW"), ("aether-dense-sq8", "AetherIndex HNSW SQ8"),
               ("aether-hybrid", "AetherIndex híbrido"),
               ("aether-hybrid-freq", "AetherIndex híbrido, frecuencia"),
               ("servers:qdrant-bm25", "Qdrant BM25 †"), ("servers:qdrant-dense", "Qdrant HNSW †"),
               ("servers:qdrant-hybrid", "Qdrant híbrido †"),
               ("servers:opensearch-bm25", "OpenSearch BM25 †"),
               ("servers:opensearch-dense", "OpenSearch k-NN †"),
               ("servers:opensearch-hybrid", "OpenSearch híbrido †")]
    for dataset in BEIR:
        for relative, label in systems:
            data = load_json(run_path(dataset, relative).with_suffix(".json"))
            if not data or "lat_p50_ms" not in data:
                continue
            lines.append(f"| {dataset} | {label} | {data['lat_p50_ms']:.3f} | {data['lat_p99_ms']:.3f} | "
                         f"{data['qps_batch']:,.0f} ({data['qps_batch_q1']:,.0f}–{data['qps_batch_q3']:,.0f}) |")
    lines += ["", "† Servidor en Docker, latencia observada por el cliente.", ""]
    return lines


def build_costs():
    lines = ["### Construcción del índice léxico (1 hilo) y tamaño", "",
             "| Dataset | AetherIndex BM25 s | Postings comprimidos MB | Lucene s | Índice Lucene MB | bm25s s | bm25s MB |",
             "|---|---:|---:|---:|---:|---:|---:|"]
    for dataset in BEIR:
        folder = MARKET / "beir" / dataset
        aether = load_json(folder / "aether-bm25-english.json") or {}
        lucene = load_json(folder / "lucene-index-porter.json") or {}
        bm25s = load_json(folder / "bm25s-default.json") or {}
        bm = aether.get("bm25", {})
        lines.append(
            f"| {dataset} | {fmt(bm.get('build_seconds_single_thread'), 2)} | "
            f"{fmt(bm.get('postings_compressed_bytes', 0) / 1e6 if bm else None, 1)} | "
            f"{fmt(lucene.get('wall_seconds'), 2)} | "
            f"{fmt(lucene.get('index_bytes', 0) / 1e6 if lucene else None, 1)} | "
            f"{fmt(bm25s.get('build_seconds_single_thread'), 2)} | "
            f"{fmt(bm25s.get('index_bytes', 0) / 1e6 if bm25s else None, 1)} |")
    lines += ["", "Lucene: tiempo de pared de `IndexCollection -threads 1 -optimize` (incluye arranque "
              "de la JVM y fusión a un segmento); tamaño en disco sin posiciones ni documentos. "
              "bm25s: solo `index()`, sin tokenizar.", ""]
    return lines


def read_csv(path):
    if not path.exists():
        return []
    with path.open(encoding="utf-8") as stream:
        return list(csv.DictReader(stream))


def ann():
    lines, targets = [], []
    labels = {"aether-hnsw": "AetherIndex HNSW", "hnswlib": "hnswlib 0.8.0",
              "hnswlib-fastmath": "hnswlib 0.8.0 (-ffast-math)", "faiss-hnsw": "FAISS HNSWFlat",
              "aether-sq8": "AetherIndex SQ8 + re-rank", "aether-sq8-lean": "AetherIndex SQ8 sin FP32",
              "faiss-hnsw-sq8": "FAISS HNSWSQ8", "aether-pq": "AetherIndex PQ + re-rank",
              "faiss-ivfpq-refine": "FAISS IVFPQ + refine", "faiss-flat": "FAISS Flat (exacto)"}
    servers = read_csv(MARKET / "servers/ann_servers.csv")
    for dataset in ("sift1m", "glove-100"):
        rows = read_csv(MARKET / "ann" / f"{dataset}.csv")
        if not rows:
            continue
        for row in rows:
            if row["system"] == "hnswlib" and row["variant"] == "fastmath":
                row["system"] = "hnswlib-fastmath"
        lines += [f"### {dataset}: mejor resultado con Recall@10 ≥ objetivo", "",
                  "Sobre toda la rejilla (M ∈ {16, 32}, efConstruction ∈ {100, 200}, barrido de ef). "
                  "QPS con 10 hilos; p50 con un hilo.", "",
                  "| Sistema | QPS @0.90 | QPS @0.95 | QPS @0.99 | p50 ms @0.95 | Recall máx. |",
                  "|---|---:|---:|---:|---:|---:|"]
        for system, label in labels.items():
            mine = [r for r in rows if r["system"] == system]
            if not mine:
                continue
            best = {}
            for target in (0.90, 0.95, 0.99):
                ok = [r for r in mine if float(r["recall10"]) >= target]
                best[target] = max((float(r["qps_batch"]) for r in ok), default=None)
                targets.append({"dataset": dataset, "system": system, "target": target,
                                "best_qps_batch": best[target],
                                "best_p50_ms": min((float(r["lat_p50_ms"]) for r in ok), default=None)})
            p50 = min((float(r["lat_p50_ms"]) for r in mine if float(r["recall10"]) >= 0.95), default=None)
            top = max(float(r["recall10"]) for r in mine)
            lines.append(f"| {label} | " + " | ".join(
                "—" if best[t] is None else f"{best[t]:,.0f}" for t in (0.90, 0.95, 0.99)) +
                f" | {fmt(p50, 3)} | {top:.4f} |")
        lines += ["", f"### {dataset}: construcción y memoria (M=16, efConstruction=200)", "",
                  "| Sistema | Construcción 10 hilos s | Construcción 1 hilo s | Memoria proceso MiB | Estructura MiB |",
                  "|---|---:|---:|---:|---:|"]
        single = {r["system"]: r for r in read_csv(MARKET / "ann" / f"{dataset}_build1.csv")}
        for system, label in labels.items():
            mine = [r for r in rows if r["system"] == system and r["M"] in ("16", "0") and
                    r["ef_construction"] in ("200", "0")] or [r for r in rows if r["system"] == system]
            if not mine:
                continue
            r = mine[0]
            one = single.get(system, {}).get("build_seconds") if system in single else None
            lines.append(f"| {label} | {float(r['build_seconds']):.1f} | {fmt(float(one) if one else None, 1)} | "
                         f"{float(r['memory_mib']):.0f} | {fmt(float(r['library_mib']) or None, 0)} |")
        mine_servers = [r for r in servers if r["dataset"] == dataset]
        if mine_servers:
            lines += ["", f"### {dataset}: servidores (cliente local, M=16, ef_construct=200)", "",
                      "| Servidor | ef | Recall@10 | p50 ms † | QPS 10 clientes † | Ingesta+índice s | Memoria contenedor MiB |",
                      "|---|---:|---:|---:|---:|---:|---:|"]
            for r in mine_servers:
                lines.append(f"| {r['system']} | {r['param']} | {float(r['recall10']):.4f} | "
                             f"{float(r['lat_p50_ms']):.3f} | {float(r['qps_batch']):,.0f} | "
                             f"{float(r['build_seconds']):.0f} | {fmt(float(r['memory_mib']) if r['memory_mib'] not in ('', 'None') else None, 0)} |")
        lines.append("")
    if targets:
        with (MARKET / "ann_targets.csv").open("w", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, fieldnames=list(targets[0].keys()))
            writer.writeheader()
            writer.writerows(targets)
    return lines


def beir_ann():
    rows = read_csv(MARKET / "beir-ann/beir_ann.csv")
    if not rows:
        return []
    lines = ["### HNSW sobre los vectores bge-small de BEIR (M=16, efConstruction=200)", "",
             "Recall@10 frente a la búsqueda exacta; QPS con 10 hilos.", "",
             "| Dataset | ef | AetherIndex R@10 / QPS | hnswlib R@10 / QPS | FAISS R@10 / QPS |",
             "|---|---:|---|---|---|"]
    for dataset in BEIR:
        for ef in ("32", "64", "128"):
            cells = []
            for system in ("aether-hnsw", "hnswlib", "faiss-hnsw"):
                r = next((r for r in rows if r["dataset"] == dataset and r["system"] == system
                          and r["param"] == ef), None)
                cells.append("—" if not r else f"{float(r['recall10']):.4f} / {float(r['qps_batch']):,.0f}")
            lines.append(f"| {dataset} | {ef} | " + " | ".join(cells) + " |")
    return lines + [""]


def plots():
    try:
        import matplotlib
        matplotlib.use("svg")
        import matplotlib.pyplot as plt
    except ImportError:
        return
    folder = MARKET / "plots"
    folder.mkdir(exist_ok=True)
    plt.rcParams["svg.hashsalt"] = "aetherindex"
    styles = {"aether-hnsw": ("#1f6feb", "-"), "hnswlib": ("#d1242f", "--"),
              "hnswlib-fastmath": ("#d1242f", ":"), "faiss-hnsw": ("#1a7f37", "-."),
              "aether-sq8": ("#8250df", "-"), "aether-sq8-lean": ("#8250df", ":"),
              "faiss-hnsw-sq8": ("#bf8700", "-."), "aether-pq": ("#0a3069", "-"),
              "faiss-ivfpq-refine": ("#57606a", "--")}
    for dataset in ("sift1m", "glove-100"):
        rows = read_csv(MARKET / "ann" / f"{dataset}.csv")
        if not rows:
            continue
        for metric, ylabel, name in (("qps_batch", "QPS (10 hilos)", "qps"),
                                     ("qps_single", "QPS (1 hilo)", "qps1")):
            fig, ax = plt.subplots(figsize=(7, 4.5))
            for system, (color, line) in styles.items():
                mine = [r for r in rows if (("hnswlib-fastmath" if r["system"] == "hnswlib" and
                                             r["variant"] == "fastmath" else r["system"]) == system)]
                if not mine:
                    continue
                points = sorted((float(r["recall10"]), float(r[metric])) for r in mine)
                frontier, best = [], 0.0
                for recall, qps in sorted(points, key=lambda p: -p[0]):
                    if qps > best:
                        frontier.append((recall, qps))
                        best = qps
                frontier.sort()
                ax.plot([p[0] for p in frontier], [p[1] for p in frontier], line, color=color,
                        marker="o", markersize=3, label=system)
            ax.set_yscale("log")
            ax.set_xlabel("Recall@10")
            ax.set_ylabel(ylabel)
            ax.set_title(f"{dataset}: frontera de Pareto (rejilla completa)")
            ax.grid(True, which="both", alpha=0.3)
            ax.legend(fontsize=8)
            fig.tight_layout()
            fig.savefig(folder / f"{dataset}_{name}.svg", metadata={"Date": None})
            plt.close(fig)


def main():
    lines = ["# Comparación con el mercado: tablas generadas", "",
             "Generado por `scripts/summarize_market.py` a partir de los archivos crudos de esta "
             "carpeta. No editar a mano.", ""]
    manifest = load_json(MARKET / "manifest.json")
    if manifest:
        lines += [f"Commit `{manifest['git_commit']}` (árbol con cambios: {manifest['git_dirty']}), "
                  f"{manifest['machine']['cpu']}, {manifest['compiler']}.", ""]
    parity = load_json(MARKET / "analyzer_parity.json")
    if parity:
        lines += ["## Analizador english-v1 frente a Lucene (SciFact)", "",
                  f"Coincidencia de tokens: {parity['agreement']:.4%} "
                  f"({parity['shared_tokens']:,} de {max(parity['reference_tokens'], parity['candidate_tokens']):,}); "
                  f"documentos idénticos: {parity['exact_documents']:,} de {parity['documents']:,}.", ""]
    done = (MARKET / "done.txt").read_text().split() if (MARKET / "done.txt").exists() else []
    checks = [d for d in done if d.startswith("parity/") and d.endswith("/check")]
    checks_freq = [d for d in done if d.startswith("parity/") and d.endswith("/check-freq")]
    lines += ["## Controles", "",
              f"- Fusión RRF: {len(checks)} de {len(BEIR)} datasets reproducen exactamente el run "
              "híbrido de AetherIndex al fusionar sus propios runs BM25 y densos (`fuse_rrf.py --check`); "
              f"con ponderación por frecuencia, {len(checks_freq)} de {len(BEIR)}.",
              ""]
    lines += trec_eval_check()
    lines += ["## Calidad en BEIR", ""] + quality() + [""]
    lines += ["## Coste en BEIR", ""] + latency() + build_costs()
    lines += ["## ANN", ""] + ann() + beir_ann()
    (MARKET / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    plots()
    print(f"Wrote {MARKET / 'summary.md'}")


if __name__ == "__main__":
    main()
