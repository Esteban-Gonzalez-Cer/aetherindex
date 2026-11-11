#!/bin/bash
# Full optimization measurement campaign (Release, one workload at a time). Run on an
# otherwise idle machine from the repository root:
#   scripts/run_optimization_campaign.sh CURRENT_SUITE BASELINE_SUITE
# BASELINE_SUITE is the same tree with the only default-path performance change (the
# thread-local visited table) reverted; it measures heavy workloads that were not run
# before the optimization phases. Results: evaluation/reports/optimization/<phase>/.
set -u
CURRENT=$1
BASELINE=$2
RUN="python3 scripts/run_optimization_suite.py"
HEAVY_VEC="vec_sift1m vec_dbpedia_100k vec_synth_1m_128"

# Phase 0: heavy baselines + a light check that the baseline binary reproduces Phase 0.
$RUN --phase phase0_baseline_heavy --suite "$BASELINE" --only $HEAVY_VEC bm25_zipf_1m
$RUN --phase phase0_baseline_check --suite "$BASELINE" --only vec_synth_100k_128 vec_dbpedia_10k

# Phase 1: binary load format.
$RUN --phase phase1_avec --suite "$CURRENT" --only load_synth_10k_1024 load_synth_100k_128

# Phase 2: default (visited table), parallel build + batch, concurrent retrievers.
$RUN --phase phase2_default --suite "$CURRENT" --only vec_synth_100k_128 vec_dbpedia_10k hybrid_zipf_100k_128 $HEAVY_VEC
$RUN --phase phase2_parallel --suite "$CURRENT" --only vec_synth_100k_128 vec_dbpedia_10k $HEAVY_VEC \
    --extra-args ALL="--build-threads 10 --query-threads 10"
$RUN --phase phase2_parallel_retrievers --suite "$CURRENT" --only hybrid_zipf_100k_128 \
    --extra-args ALL="--parallel-retrievers"

# Phase 3: SQ8 with exact re-rank, and with FP32 rows released.
$RUN --phase phase3_sq8 --suite "$CURRENT" --only vec_sift1m vec_dbpedia_100k \
    --extra-args ALL="--quantization sq8 --build-threads 10"
$RUN --phase phase3_sq8_drop --suite "$CURRENT" --only vec_sift1m vec_dbpedia_100k \
    --extra-args ALL="--quantization sq8 --drop-fp32 --build-threads 10"

# Phase 4: PQ (16x and 32x compression) with exact re-rank and a deeper candidate list.
$RUN --phase phase4_pq16x --suite "$CURRENT" --only vec_sift1m vec_dbpedia_100k \
    --extra-args ALL="--quantization pq --ef-search 128 --build-threads 10"
$RUN --phase phase4_pq32x --suite "$CURRENT" --only vec_sift1m vec_dbpedia_100k \
    --extra-args vec_sift1m="--quantization pq --pq-subspaces 16 --ef-search 128 --build-threads 10" \
    --extra-args vec_dbpedia_100k="--quantization pq --pq-subspaces 128 --ef-search 128 --build-threads 10"

# Phase 5: BM25 strategies (exhaustive baseline is phase0 bm25_zipf_*).
for strategy in taat wand bmw; do
    $RUN --phase phase5_$strategy --suite "$CURRENT" --only bm25_zipf_100k bm25_zipf_1m bm25_cp01 \
        --extra-args ALL="--bm25-strategy $strategy"
done
$RUN --phase phase5_hybrid_bmw --suite "$CURRENT" --only hybrid_zipf_100k_128 \
    --extra-args ALL="--bm25-strategy bmw"

# Phase 6: filtered dense search at 1%, 10% and 50% selectivity.
for percent in 1 10 50; do
    $RUN --phase phase6_filter$percent --suite "$CURRENT" --only vec_synth_100k_128 vec_dbpedia_100k \
        --extra-args ALL="--filter-percent $percent --build-threads 10"
done
echo "Campaign finished."
