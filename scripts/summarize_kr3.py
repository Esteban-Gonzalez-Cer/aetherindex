#!/usr/bin/env python3
"""OKR 3 summary: builds the KR3.1-KR3.8 tables and the Recall@10 vs latency chart.

Reads the CSVs written by aether_hnsw_parameter_sweep (evaluation/reports/kr3_dbpedia and
evaluation/reports/kr3_sift/{10k,100k,1m}) and aether_distance_kernel_benchmark
(evaluation/reports/kr3_kernels/*). Writes evaluation/reports/kr3_summary.md and
docs/reports/assets/kr3_recall_latency.{svg,png}. Needs matplotlib only for the chart.

    python3 scripts/summarize_kr3.py
"""
import csv
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REPORTS = ROOT / "evaluation" / "reports"
ASSETS = ROOT / "docs" / "reports" / "assets"

# (label, directory) in fixed order; the chart assigns categorical slots in this order.
SWEEPS = [
    ("SIFT 10K (L2)", REPORTS / "kr3_sift" / "10k"),
    ("SIFT 100K (L2)", REPORTS / "kr3_sift" / "100k"),
    ("SIFT 1M (L2)", REPORTS / "kr3_sift" / "1m"),
    ("DBpedia 9K (cosine)", REPORTS / "kr3_dbpedia"),
]
SERIES_COLORS = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100"]  # validated slots 1-4


def read(path):
    with open(path, newline="") as handle:
        return list(csv.DictReader(handle))


def fmt(value, digits=3):
    return f"{float(value):.{digits}f}"


def sweep_tables(lines):
    available = [(label, path) for label, path in SWEEPS if (path / "kr3_2_recall_latency.csv").exists()]
    lines += ["## KR3.2 / KR3.3 / KR3.7 - Recall@10 vs latencia (M=16, efConstruction=100)", ""]
    lines += ["| Dataset | Vectores | efSearch | Consultas | Recall@10 | p50 ms | p95 ms | p99 ms | Build s |",
              "|---|---:|---:|---:|---:|---:|---:|---:|---:|"]
    best = []
    for label, path in available:
        workload = json.loads((path / "workload.json").read_text())
        rows = read(path / "kr3_2_recall_latency.csv")
        for row in rows:
            lines.append(f"| {label} | {workload['vectors']:,} | {row['ef_search']} | {row['queries']} | "
                         f"{fmt(row['mean_recall_at_10'], 4)} | {fmt(row['p50_ms'])} | {fmt(row['p95_ms'])} | "
                         f"{fmt(row['p99_ms'])} | {float(row['build_ms']) / 1000:.1f} |")
        passing = [r for r in rows if float(r["mean_recall_at_10"]) >= 0.95]
        first = min(passing, key=lambda r: int(r["ef_search"])) if passing else None
        best.append((label, workload, first))
    lines += ["", "### KR3.3 - menor efSearch con Recall@10 >= 0.95", "",
              "| Dataset | Vectores | efSearch | Recall@10 | p50 ms | p95 ms | p99 ms | Estado |",
              "|---|---:|---:|---:|---:|---:|---:|---|"]
    for label, workload, row in best:
        if row is None:
            lines.append(f"| {label} | {workload['vectors']:,} | - | - | - | - | - | NO |")
        else:
            lines.append(f"| {label} | {workload['vectors']:,} | {row['ef_search']} | "
                         f"{fmt(row['mean_recall_at_10'], 4)} | {fmt(row['p50_ms'])} | {fmt(row['p95_ms'])} | "
                         f"{fmt(row['p99_ms'])} | PASS |")
    lines.append("")

    lines += ["## KR3.1 - M x efSearch (efConstruction=100) y efConstruction (M=16, efSearch=50)", ""]
    lines += ["| Dataset | Grupo | M | efC | efSearch | Recall@10 | p50 ms | p95 ms | p99 ms | Build s |",
              "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for label, path in available:
        file = path / "kr3_1_hnsw_parameters.csv"
        for row in read(file) if file.exists() else []:
            lines.append(f"| {label} | {row['group']} | {row['m']} | {row['ef_construction']} | {row['ef_search']} | "
                         f"{fmt(row['mean_recall_at_10'], 4)} | {fmt(row['p50_ms'])} | {fmt(row['p95_ms'])} | "
                         f"{fmt(row['p99_ms'])} | {float(row['build_ms']) / 1000:.1f} |")
    lines.append("")
    return available


def kernel_tables(lines):
    runs = sorted(p for p in (REPORTS / "kr3_kernels").iterdir() if (p / "kr3_6_simd_speedup.csv").exists())
    lines += ["## KR3.4 / KR3.6 - kernels de distancia, escalar vs SIMD", ""]
    for run in runs:
        env = json.loads((run / "environment.json").read_text())
        lines.append(f"`{run.name}`: SIMD = {env['simd_backend']}; {env['executions_per_kernel']:,} "
                     f"ejecuciones por kernel tras {env['warmup_repetitions']} repeticiones de warm-up; {env['compiler']}.")
    lines += ["", "| Datos | Métrica | Dim | Escalar ns/llamada | SIMD ns/llamada | Reducción | Speedup | >= 20 % |",
              "|---|---|---:|---:|---:|---:|---:|---|"]
    for run in runs:
        for row in read(run / "kr3_6_simd_speedup.csv"):
            lines.append(f"| {run.name} | {row['metric']} | {row['dimension']} | {fmt(row['scalar_mean_ns'], 2)} | "
                         f"{fmt(row['simd_mean_ns'], 2)} | {fmt(row['mean_reduction_pct'], 1)} % | "
                         f"{fmt(row['speedup'], 2)}x | {'PASS' if row['meets_kr3_6'] == 'true' else 'NO'} |")
    lines += ["", "## KR3.5 - validación numérica SIMD vs escalar (mismos datos)", "",
              "| Datos | Métrica | Dim | Pares | máx abs SIMD-escalar | máx rel | máx abs SIMD-long double | error/cota | Estado |",
              "|---|---|---:|---:|---:|---:|---:|---:|---|"]
    for run in runs:
        for row in read(run / "kr3_5_numeric_validation.csv"):
            lines.append(f"| {run.name} | {row['metric']} | {row['dimension']} | {int(row['pairs']):,} | "
                         f"{float(row['max_abs_simd_vs_scalar']):.3g} | {float(row['max_rel_simd_vs_scalar']):.3g} | "
                         f"{float(row['max_abs_simd_vs_longdouble']):.3g} | {fmt(row['max_error_over_bound'])} | "
                         f"{'PASS' if row['pass'] == 'true' else 'NO'} |")
    lines.append("")


def chart(available):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib not available: chart skipped")
        return
    plt.rcParams.update({"font.size": 10, "axes.edgecolor": "#c3c2b7", "axes.labelcolor": "#3d3d3a",
                         "xtick.color": "#5e5d59", "ytick.color": "#5e5d59"})
    figure, axes = plt.subplots(1, 2, figsize=(11, 4.4), sharey=True)
    for axis, column, title in ((axes[0], "p50_ms", "Recall@10 vs latencia p50"),
                                (axes[1], "p95_ms", "Recall@10 vs latencia p95")):
        for (label, path), color in zip(available, SERIES_COLORS):
            rows = read(path / "kr3_2_recall_latency.csv")
            x = [float(r[column]) for r in rows]
            y = [float(r["mean_recall_at_10"]) for r in rows]
            axis.plot(x, y, color=color, linewidth=2, marker="o", markersize=6,
                      markeredgecolor="white", markeredgewidth=1.5, label=label)
        axis.axhline(0.95, color="#8c8b84", linewidth=1, linestyle="--")
        axis.text(0.0, 0.951, " objetivo KR3.3 = 0.95", fontsize=8,
                  color="#5e5d59", transform=axis.get_yaxis_transform(), va="bottom")
        axis.set_xscale("log")
        axis.set_xlabel(f"{column.replace('_ms', '')} por consulta (ms, escala log)")
        axis.set_title(title, loc="left", fontsize=11, color="#1f1e1d")
        axis.grid(True, color="#ecebe6", linewidth=0.8)
        axis.spines[["top", "right"]].set_visible(False)
    axes[0].set_ylabel("Recall@10")
    axes[1].legend(loc="lower right", frameon=False, fontsize=9)
    figure.suptitle("HNSW M=16, efConstruction=100; puntos de izquierda a derecha: efSearch 10, 25, 50, 100, 200 (y 400 en SIFT); 1,000 consultas por punto, 1 hilo",
                    x=0.01, ha="left", fontsize=9, color="#5e5d59")
    figure.tight_layout()
    ASSETS.mkdir(parents=True, exist_ok=True)
    for suffix in ("svg", "png"):
        figure.savefig(ASSETS / f"kr3_recall_latency.{suffix}", dpi=160, facecolor="white")
    print(f"Wrote {ASSETS / 'kr3_recall_latency.svg'}")


def main():
    lines = ["# OKR 3 - resumen generado", "",
             "Generado por `scripts/summarize_kr3.py` a partir de los CSV de este directorio. "
             "No editar a mano.", ""]
    available = sweep_tables(lines)
    kernel_tables(lines)
    (REPORTS / "kr3_summary.md").write_text("\n".join(lines))
    print(f"Wrote {REPORTS / 'kr3_summary.md'}")
    chart(available)


if __name__ == "__main__":
    main()
