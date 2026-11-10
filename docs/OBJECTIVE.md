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

## Progress

| Change | Cells affected |
|---|---|
| Baseline (scoreboard introduced) | 67 of 144 won; 28 clear losses |
| BM25 index size measured on disk for both sides. The earlier in-memory figure left out our term dictionary, so the Quora "win" was a loss (20.0 MB vs Lucene's 15.8). | Correction |
| Compact on-disk BM25 format (front-coded terms, bit-packed lengths and postings; everything else rebuilt on load) | Index size: 6 of 6 won, 0.46–0.84× Lucene |
| BM25 indexing: memoized stems, interned terms | Build time: 3 cells lost → won (SciFact by 0.6 %) |
| SQ8 without row padding and without per-row floats (−4 to −5 % SQ8 QPS) | SQ8 memory: 2 cells lost → won |
| `english-plain-v1`, the counterpart of Lucene with `-stemmer none -keepStopwords` (the old comparison used `ascii-tech-v1`, an analyzer for technical identifiers) | 14 clear losses → 3 won, 4 tied, 11 within noise either way |
| **Today** | **86 of 144 won; 6 clear losses** |

Re-measured cells come from a later campaign pass than the rest. The pass re-ran AetherIndex,
Lucene and bm25s together for the BM25 cost cells. For the ANN throughput cells only the
AetherIndex SQ8 runs were repeated, so those cells compare runs from different sessions;
they were won by more than 2× in both passes.

Open, with what is known:

- **ArguAna BM25 against bm25s (3 cells).** bm25s's default configuration uses its own
  stemmer and stopword list. With identical tokens (`bm25s (english-v1 tokens)`), the
  scores are identical to ours.
- **Parallel HNSW build against hnswlib `-ffast-math` (2 cells).** Removing per-node locks
  from the insertion traversal changed nothing (32.1 s vs 32.0 s on SIFT1M). The profile
  puts the time in the traversal loop and the distance kernel, not in locking.
- **GloVe-100 at Recall@10 ≥ 0.99.** No HNSW configuration on the shared grid reaches it,
  for any system; exact FAISS Flat does, at 2.7 K QPS.
