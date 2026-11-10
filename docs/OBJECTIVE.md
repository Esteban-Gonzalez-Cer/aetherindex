# Objective

**AetherIndex aims to beat every competitor on every benchmark cell.**

Progress is tracked cell by cell in the
[scoreboard](../evaluation/reports/market/scoreboard.md), which is regenerated from the raw
campaign results by `scripts/scoreboard.py`. The scoreboard lists every cell, including the
ones still lost, so the claim can be checked rather than taken on trust.

## What a cell is

A cell is one (benchmark, dataset, metric). In each cell, the configuration AetherIndex
ships for that benchmark is compared with **the best competitor configuration measured for
that cell**. The comparison is deliberately strict:

- The competitor side takes the best of every system and configuration measured: Lucene,
  bm25s (default and with AetherIndex tokens), hnswlib (standard and `-ffast-math`), FAISS,
  Qdrant and OpenSearch, each tuned on the same grid.
- The AetherIndex side is one configuration per benchmark, the same on every dataset. It is
  never picked per dataset after looking at test results.
- For approximate search at a recall target, both sides take their best variant reaching
  the target (for AetherIndex: FP32, SQ8 or PQ).

## When a cell is won

| Kind | Cells | Won when |
|---|---|---|
| Quality | nDCG@10, Recall@100, MRR@10 on the BEIR test splits | Strictly higher. Parameters are chosen on dev splits only. |
| Cost | Latency, throughput, build time, index size, memory | Strictly better median of 5 repetitions in the same session. |

Every result carries its uncertainty. A quality difference with p ≥ 0.05 (paired
randomization test, 10,000 permutations) or a cost difference whose interquartile ranges
overlap is marked `~`: it counts toward the total, but the scoreboard reports it apart from
clear wins and losses.

Two kinds of cell are not counted as won or lost:

- **Ceiling.** When both sides already reach the exact-search optimum (for example, dense
  nDCG@10 equal to brute-force search), no system can do better. The cell is listed as
  `ceiling`.
- **Not comparable (`n/c`).** Latency of network servers (Qdrant, OpenSearch) against an
  in-process library. AetherIndex has no server mode, so these are shown for context only.

## Fairness rules that do not change

The [market comparison protocol](reports/MARKET_COMPARISON.md#protocolo-de-equidad) applies
to every run that feeds the scoreboard:

- Same machine, same data, same `k`, same evaluation code.
- Competitors are tuned on the same grid as AetherIndex.
- No tuning on test data, and no code paths specific to a benchmark or dataset.
- The pipeline is validated before it is trusted: Lucene must reproduce its published
  BEIR numbers, and the evaluator must match `trec_eval`.

## Known limits of the objective

- All results come from one Apple Silicon machine. x86 results are not measured.
- Some quality cells differ from Lucene or bm25s by less than 0.001 with the same BM25
  formula. Winning them takes ranking improvements that hold up on test data, and some
  may stay lost. The scoreboard will say so.
