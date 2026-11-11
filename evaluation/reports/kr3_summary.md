# OKR 3 - resumen generado

Generado por `scripts/summarize_kr3.py` a partir de los CSV de este directorio. No editar a mano.

## KR3.2 / KR3.3 / KR3.7 - Recall@10 vs latencia (M=16, efConstruction=100)

| Dataset | Vectores | efSearch | Consultas | Recall@10 | p50 ms | p95 ms | p99 ms | Build s |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| SIFT 10K (L2) | 10,000 | 10 | 1000 | 0.9239 | 0.007 | 0.010 | 0.043 | 1.0 |
| SIFT 10K (L2) | 10,000 | 25 | 1000 | 0.9828 | 0.013 | 0.016 | 0.021 | 1.0 |
| SIFT 10K (L2) | 10,000 | 50 | 1000 | 0.9969 | 0.022 | 0.027 | 0.030 | 1.0 |
| SIFT 10K (L2) | 10,000 | 100 | 1000 | 0.9997 | 0.037 | 0.046 | 0.050 | 1.0 |
| SIFT 10K (L2) | 10,000 | 200 | 1000 | 1.0000 | 0.064 | 0.077 | 0.084 | 1.0 |
| SIFT 10K (L2) | 10,000 | 400 | 1000 | 1.0000 | 0.108 | 0.127 | 0.139 | 1.0 |
| SIFT 100K (L2) | 100,000 | 10 | 1000 | 0.8412 | 0.013 | 0.018 | 0.021 | 15.1 |
| SIFT 100K (L2) | 100,000 | 25 | 1000 | 0.9459 | 0.024 | 0.031 | 0.036 | 15.1 |
| SIFT 100K (L2) | 100,000 | 50 | 1000 | 0.9847 | 0.043 | 0.057 | 0.071 | 15.1 |
| SIFT 100K (L2) | 100,000 | 100 | 1000 | 0.9968 | 0.074 | 0.094 | 0.105 | 15.1 |
| SIFT 100K (L2) | 100,000 | 200 | 1000 | 0.9998 | 0.130 | 0.170 | 0.195 | 15.1 |
| SIFT 100K (L2) | 100,000 | 400 | 1000 | 1.0000 | 0.236 | 0.305 | 0.335 | 15.1 |
| SIFT 1M (L2) | 1,000,000 | 10 | 1000 | 0.7406 | 0.020 | 0.028 | 0.033 | 27.5 |
| SIFT 1M (L2) | 1,000,000 | 25 | 1000 | 0.8835 | 0.038 | 0.050 | 0.066 | 27.5 |
| SIFT 1M (L2) | 1,000,000 | 50 | 1000 | 0.9505 | 0.064 | 0.076 | 0.086 | 27.5 |
| SIFT 1M (L2) | 1,000,000 | 100 | 1000 | 0.9854 | 0.113 | 0.133 | 0.139 | 27.5 |
| SIFT 1M (L2) | 1,000,000 | 200 | 1000 | 0.9969 | 0.209 | 0.254 | 0.304 | 27.5 |
| SIFT 1M (L2) | 1,000,000 | 400 | 1000 | 0.9995 | 0.373 | 0.453 | 0.490 | 27.5 |
| DBpedia 9K (cosine) | 9,000 | 10 | 1000 | 0.8817 | 0.058 | 0.084 | 0.104 | 6.8 |
| DBpedia 9K (cosine) | 9,000 | 25 | 1000 | 0.9621 | 0.127 | 0.171 | 0.187 | 6.9 |
| DBpedia 9K (cosine) | 9,000 | 50 | 1000 | 0.9886 | 0.186 | 0.248 | 0.278 | 6.7 |
| DBpedia 9K (cosine) | 9,000 | 100 | 1000 | 0.9972 | 0.350 | 0.495 | 1.014 | 6.9 |
| DBpedia 9K (cosine) | 9,000 | 200 | 1000 | 0.9990 | 0.605 | 0.828 | 0.926 | 7.7 |

### KR3.3 - menor efSearch con Recall@10 >= 0.95

| Dataset | Vectores | efSearch | Recall@10 | p50 ms | p95 ms | p99 ms | Estado |
|---|---:|---:|---:|---:|---:|---:|---|
| SIFT 10K (L2) | 10,000 | 25 | 0.9828 | 0.013 | 0.016 | 0.021 | PASS |
| SIFT 100K (L2) | 100,000 | 50 | 0.9847 | 0.043 | 0.057 | 0.071 | PASS |
| SIFT 1M (L2) | 1,000,000 | 50 | 0.9505 | 0.064 | 0.076 | 0.086 | PASS |
| DBpedia 9K (cosine) | 9,000 | 25 | 0.9621 | 0.127 | 0.171 | 0.187 | PASS |

## KR3.1 - M x efSearch (efConstruction=100) y efConstruction (M=16, efSearch=50)

| Dataset | Grupo | M | efC | efSearch | Recall@10 | p50 ms | p95 ms | p99 ms | Build s |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| SIFT 10K (L2) | kr3_1 | 8 | 100 | 10 | 0.8554 | 0.005 | 0.007 | 0.008 | 0.5 |
| SIFT 10K (L2) | kr3_1 | 8 | 100 | 50 | 0.9906 | 0.018 | 0.021 | 0.022 | 0.5 |
| SIFT 10K (L2) | kr3_1 | 8 | 100 | 100 | 0.9988 | 0.031 | 0.035 | 0.037 | 0.5 |
| SIFT 10K (L2) | kr3_1 | 16 | 100 | 10 | 0.9239 | 0.007 | 0.009 | 0.011 | 1.0 |
| SIFT 10K (L2) | kr3_1 | 16 | 100 | 50 | 0.9969 | 0.023 | 0.028 | 0.030 | 1.0 |
| SIFT 10K (L2) | kr3_1 | 16 | 100 | 100 | 0.9997 | 0.037 | 0.046 | 0.051 | 1.0 |
| SIFT 10K (L2) | kr3_1 | 32 | 100 | 10 | 0.9677 | 0.011 | 0.014 | 0.021 | 2.9 |
| SIFT 10K (L2) | kr3_1 | 32 | 100 | 50 | 0.9990 | 0.030 | 0.038 | 0.076 | 2.9 |
| SIFT 10K (L2) | kr3_1 | 32 | 100 | 100 | 0.9999 | 0.048 | 0.060 | 0.067 | 2.9 |
| SIFT 10K (L2) | kr3_1_efc | 16 | 50 | 50 | 0.9954 | 0.022 | 0.027 | 0.030 | 0.8 |
| SIFT 10K (L2) | kr3_1_efc | 16 | 200 | 50 | 0.9978 | 0.023 | 0.028 | 0.037 | 1.3 |
| SIFT 100K (L2) | kr3_1 | 8 | 100 | 10 | 0.7379 | 0.009 | 0.014 | 0.020 | 7.6 |
| SIFT 100K (L2) | kr3_1 | 8 | 100 | 50 | 0.9548 | 0.031 | 0.041 | 0.049 | 7.6 |
| SIFT 100K (L2) | kr3_1 | 8 | 100 | 100 | 0.9860 | 0.057 | 0.083 | 0.137 | 7.6 |
| SIFT 100K (L2) | kr3_1 | 16 | 100 | 10 | 0.8412 | 0.013 | 0.018 | 0.024 | 15.1 |
| SIFT 100K (L2) | kr3_1 | 16 | 100 | 50 | 0.9847 | 0.043 | 0.055 | 0.069 | 15.1 |
| SIFT 100K (L2) | kr3_1 | 16 | 100 | 100 | 0.9968 | 0.076 | 0.099 | 0.123 | 15.1 |
| SIFT 100K (L2) | kr3_1 | 32 | 100 | 10 | 0.9087 | 0.018 | 0.026 | 0.031 | 37.8 |
| SIFT 100K (L2) | kr3_1 | 32 | 100 | 50 | 0.9954 | 0.059 | 0.079 | 0.090 | 37.8 |
| SIFT 100K (L2) | kr3_1 | 32 | 100 | 100 | 0.9987 | 0.097 | 0.129 | 0.140 | 37.8 |
| SIFT 100K (L2) | kr3_1_efc | 16 | 50 | 50 | 0.9802 | 0.039 | 0.052 | 0.059 | 11.0 |
| SIFT 100K (L2) | kr3_1_efc | 16 | 200 | 50 | 0.9871 | 0.041 | 0.052 | 0.056 | 18.8 |
| DBpedia 9K (cosine) | kr3_1 | 8 | 100 | 10 | 0.7715 | 0.052 | 0.097 | 0.165 | 3.0 |
| DBpedia 9K (cosine) | kr3_1 | 8 | 100 | 50 | 0.9599 | 0.147 | 0.204 | 0.349 | 2.8 |
| DBpedia 9K (cosine) | kr3_1 | 8 | 100 | 100 | 0.9849 | 0.220 | 0.290 | 0.338 | 2.8 |
| DBpedia 9K (cosine) | kr3_1 | 16 | 100 | 10 | 0.8817 | 0.065 | 0.100 | 0.124 | 7.1 |
| DBpedia 9K (cosine) | kr3_1 | 16 | 100 | 50 | 0.9886 | 0.220 | 0.301 | 0.339 | 7.3 |
| DBpedia 9K (cosine) | kr3_1 | 16 | 100 | 100 | 0.9972 | 0.433 | 0.693 | 1.210 | 7.2 |
| DBpedia 9K (cosine) | kr3_1 | 32 | 100 | 10 | 0.9402 | 0.115 | 0.184 | 0.223 | 21.7 |
| DBpedia 9K (cosine) | kr3_1 | 32 | 100 | 50 | 0.9956 | 0.378 | 0.648 | 1.194 | 21.6 |
| DBpedia 9K (cosine) | kr3_1 | 32 | 100 | 100 | 0.9990 | 0.463 | 0.648 | 0.737 | 21.5 |
| DBpedia 9K (cosine) | kr3_1_efc | 16 | 50 | 50 | 0.9845 | 0.170 | 0.236 | 0.263 | 5.3 |
| DBpedia 9K (cosine) | kr3_1_efc | 16 | 200 | 50 | 0.9907 | 0.203 | 0.268 | 0.302 | 8.8 |

## KR3.4 / KR3.6 - kernels de distancia, escalar vs SIMD

`sift1m`: SIMD = NEON/FMA FP32; 500,000 ejecuciones por kernel tras 5 repeticiones de warm-up; Apple LLVM 17.0.0 (clang-1700.6.3.2).
`synthetic`: SIMD = NEON/FMA FP32; 500,000 ejecuciones por kernel tras 5 repeticiones de warm-up; Apple LLVM 17.0.0 (clang-1700.6.3.2).

| Datos | Métrica | Dim | Escalar ns/llamada | SIMD ns/llamada | Reducción | Speedup | >= 20 % |
|---|---|---:|---:|---:|---:|---:|---|
| sift1m | cosine | 128 | 69.80 | 7.28 | 89.6 % | 9.59x | PASS |
| sift1m | squared_euclidean | 128 | 70.31 | 7.46 | 89.4 % | 9.43x | PASS |
| synthetic | cosine | 100 | 55.88 | 6.54 | 88.3 % | 8.54x | PASS |
| synthetic | squared_euclidean | 100 | 59.74 | 6.39 | 89.3 % | 9.35x | PASS |
| synthetic | cosine | 128 | 68.56 | 6.90 | 89.9 % | 9.94x | PASS |
| synthetic | squared_euclidean | 128 | 68.84 | 7.34 | 89.3 % | 9.37x | PASS |
| synthetic | cosine | 384 | 249.02 | 22.31 | 91.0 % | 11.16x | PASS |
| synthetic | squared_euclidean | 384 | 263.96 | 21.83 | 91.7 % | 12.09x | PASS |
| synthetic | cosine | 768 | 572.09 | 51.59 | 91.0 % | 11.09x | PASS |
| synthetic | squared_euclidean | 768 | 584.60 | 50.05 | 91.4 % | 11.68x | PASS |
| synthetic | cosine | 1024 | 800.96 | 66.26 | 91.7 % | 12.09x | PASS |
| synthetic | squared_euclidean | 1024 | 840.50 | 64.98 | 92.3 % | 12.94x | PASS |

## KR3.5 - validación numérica SIMD vs escalar (mismos datos)

| Datos | Métrica | Dim | Pares | máx abs SIMD-escalar | máx rel | máx abs SIMD-long double | error/cota | Estado |
|---|---|---:|---:|---:|---:|---:|---:|---|
| sift1m | cosine | 128 | 80,000 | 5.36e-07 | 3.31e-06 | 1.03e-07 | 0.037 | PASS |
| sift1m | squared_euclidean | 128 | 80,000 | 0 | 0 | 0 | 0.000 | PASS |
| synthetic | cosine | 100 | 80,000 | 2.38e-07 | 2.65e-07 | 9.34e-08 | 0.015 | PASS |
| synthetic | squared_euclidean | 100 | 80,000 | 0.000153 | 7.27e-07 | 4.33e-05 | 0.027 | PASS |
| synthetic | cosine | 128 | 80,000 | 2.38e-07 | 2.47e-07 | 9.34e-08 | 0.013 | PASS |
| synthetic | squared_euclidean | 128 | 80,000 | 0.000244 | 8.02e-07 | 4.9e-05 | 0.026 | PASS |
| synthetic | cosine | 384 | 80,000 | 1.79e-07 | 2.08e-07 | 7.91e-08 | 0.003 | PASS |
| synthetic | squared_euclidean | 384 | 80,000 | 0.000977 | 1.29e-06 | 0.000125 | 0.014 | PASS |
| synthetic | cosine | 768 | 80,000 | 2.38e-07 | 2.27e-07 | 8.14e-08 | 0.002 | PASS |
| synthetic | squared_euclidean | 768 | 80,000 | 0.00293 | 1.88e-06 | 0.000274 | 0.010 | PASS |
| synthetic | cosine | 1024 | 80,000 | 2.38e-07 | 2.15e-07 | 8e-08 | 0.002 | PASS |
| synthetic | squared_euclidean | 1024 | 80,000 | 0.00439 | 2.07e-06 | 0.000484 | 0.009 | PASS |
