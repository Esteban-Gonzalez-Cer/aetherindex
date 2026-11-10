# Comparación con el mercado: tablas generadas

Generado por `scripts/summarize_market.py` a partir de los archivos crudos de esta carpeta. No editar a mano.

Commit `1bb7845a74cc39f614dcfa153e339dfd98ae96d1` (árbol con cambios: True), Apple M4 Pro, Apple clang version 17.0.0 (clang-1700.6.3.2).

## Analizador english-v1 frente a Lucene (SciFact)

Coincidencia de tokens: 99.9933% (838,072 de 838,128); documentos idénticos: 5,149 de 5,183.

## Controles

- Fusión RRF: 6 de 6 datasets reproducen exactamente el run híbrido de AetherIndex al fusionar sus propios runs BM25 y densos (`fuse_rrf.py --check`); con ponderación por frecuencia, 6 de 6; RRF ponderado, 6 de 6; fusión convexa, 6 de 6.

### Control: evaluador propio frente a trec_eval (nDCG@10 / Recall@100)

| Dataset | Run | Propio | trec_eval | 
|---|---|---|---|
| scifact | lucene-bm25-porter | 0.6789 / 0.9253 | 0.6789 / 0.9253 |
| scifact | aether-bm25-english | 0.6795 / 0.9253 | 0.6795 / 0.9253 |
| scifact | aether-bm25-english-freq | 0.6777 / 0.9253 | 0.6777 / 0.9253 |
| scifact | aether-hybrid | 0.7303 / 0.9683 | 0.7303 / 0.9683 |
| nfcorpus | lucene-bm25-porter | 0.3206 / 0.2456 | 0.3206 / 0.2456 |
| nfcorpus | aether-bm25-english | 0.3207 / 0.2478 | 0.3207 / 0.2478 |
| nfcorpus | aether-bm25-english-freq | 0.3202 / 0.2456 | 0.3202 / 0.2456 |
| nfcorpus | aether-hybrid | 0.3620 / 0.3139 | 0.3620 / 0.3139 |
| fiqa | lucene-bm25-porter | 0.2361 / 0.5395 | 0.2361 / 0.5395 |
| fiqa | aether-bm25-english | 0.2378 / 0.5354 | 0.2378 / 0.5354 |
| fiqa | aether-bm25-english-freq | 0.2353 / 0.5400 | 0.2353 / 0.5400 |
| fiqa | aether-hybrid | 0.3569 / 0.6855 | 0.3569 / 0.6855 |
| arguana | lucene-bm25-porter | 0.3970 / 0.9324 | 0.3970 / 0.9324 |
| arguana | aether-bm25-english | 0.3178 / 0.8357 | 0.3178 / 0.8357 |
| arguana | aether-bm25-english-freq | 0.4003 / 0.9324 | 0.4003 / 0.9324 |
| arguana | aether-hybrid | 0.4831 / 0.9844 | 0.4831 / 0.9844 |
| trec-covid | lucene-bm25-porter | 0.5935 / 0.1091 | 0.5935 / 0.1091 |
| trec-covid | aether-bm25-english | 0.5964 / 0.1085 | 0.5964 / 0.1085 |
| trec-covid | aether-bm25-english-freq | 0.5960 / 0.1091 | 0.5960 / 0.1091 |
| trec-covid | aether-hybrid | 0.8026 / 0.1337 | 0.8026 / 0.1337 |
| quora | lucene-bm25-porter | 0.7884 / 0.9733 | 0.7884 / 0.9733 |
| quora | aether-bm25-english | 0.7893 / 0.9736 | 0.7893 / 0.9736 |
| quora | aether-bm25-english-freq | 0.7885 / 0.9733 | 0.7885 / 0.9733 |
| quora | aether-hybrid | 0.8662 / 0.9955 | 0.8662 / 0.9955 |

Diferencia máxima: 0.00005 (trec_eval imprime 4 decimales).

## Calidad en BEIR

### nDCG@10

| Sistema | scifact | nfcorpus | fiqa | arguana | trec-covid | quora | Media |
|---|---:|---:|---:|---:|---:|---:|---:|
| **BM25** | | | | | | | |
| Lucene BM25 (Porter) | 0.6789 | 0.3206 | 0.2361 | 0.3970 | 0.5935 | 0.7884 | 0.5024 |
| AetherIndex BM25 english-v1 | 0.6795 | 0.3207 | 0.2378 | 0.3178 | 0.5964 | 0.7893 | 0.4903 |
| AetherIndex BM25 english-v1, frecuencia | 0.6777 | 0.3202 | 0.2353 | 0.4003 | 0.5960 | 0.7885 | 0.5030 |
| bm25s (tokens english-v1) | 0.6777 | 0.3202 | 0.2353 | 0.4003 | 0.5960 | 0.7885 | 0.5030 |
| bm25s (por defecto) | 0.6762 | 0.3177 | 0.2385 | 0.4080 | 0.5887 | 0.7870 | 0.5027 |
| Qdrant sparse BM25 | 0.6795 | 0.3207 | 0.2378 | 0.3180 | 0.5964 | 0.7891 | 0.4903 |
| OpenSearch BM25 | 0.6789 | 0.3206 | 0.2361 | 0.3970 | 0.5935 | 0.7884 | 0.5024 |
| **BM25 sin stemming** | | | | | | | |
| Lucene sin stemming/stopwords | 0.6651 | 0.3049 | 0.2320 | 0.3813 | 0.5823 | 0.7269 | 0.4821 |
| AetherIndex BM25 ascii-tech-v1 | 0.6435 | 0.2935 | 0.2238 | 0.3244 | 0.5387 | 0.7244 | 0.4581 |
| AetherIndex BM25 english-plain-v1, frecuencia | 0.6622 | 0.3051 | 0.2324 | 0.3845 | 0.5832 | 0.7270 | 0.4824 |
| **BM25 k1=1.2** | | | | | | | |
| Lucene BM25 k1=1.2 b=0.75 | 0.6826 | 0.3209 | 0.2536 | 0.4780 | 0.6153 | 0.8077 | 0.5263 |
| AetherIndex BM25 k1=1.2 b=0.75 | 0.6870 | 0.3219 | 0.2509 | 0.4062 | 0.6123 | 0.8088 | 0.5145 |
| AetherIndex BM25 k1=1.2 b=0.75, frecuencia | 0.6830 | 0.3212 | 0.2523 | 0.4823 | 0.6139 | 0.8078 | 0.5268 |
| **Denso** | | | | | | | |
| Exacto (fuerza bruta) | 0.7127 | 0.3430 | 0.4035 | 0.6035 | 0.7578 | 0.8864 | 0.6178 |
| AetherIndex HNSW FP32 | 0.7127 | 0.3430 | 0.4014 | 0.6035 | 0.7578 | 0.8861 | 0.6174 |
| AetherIndex HNSW SQ8 | 0.7127 | 0.3424 | 0.4014 | 0.6035 | 0.7578 | 0.8861 | 0.6173 |
| Qdrant HNSW | 0.7127 | 0.3430 | 0.3989 | 0.6035 | 0.7557 | 0.8863 | 0.6167 |
| OpenSearch k-NN | 0.7127 | 0.3425 | 0.4005 | 0.6035 | 0.7578 | 0.8855 | 0.6171 |
| **Híbrido** | | | | | | | |
| AetherIndex híbrido RRF | 0.7303 | 0.3620 | 0.3569 | 0.4831 | 0.8026 | 0.8662 | 0.6002 |
| AetherIndex híbrido RRF, frecuencia | 0.7292 | 0.3612 | 0.3586 | 0.5372 | 0.7994 | 0.8662 | 0.6086 |
| AetherIndex RRF ponderado (global, dev) | 0.7239 | 0.3531 | 0.4080 | 0.6025 | 0.8007 | 0.8886 | 0.6295 |
| AetherIndex convexo (global, dev) | 0.7318 | 0.3603 | 0.4137 | 0.6005 | 0.7861 | 0.8955 | 0.6313 |
| AetherIndex RRF ponderado (por dataset, dev) | 0.7265 | 0.3578 | 0.4101 | — | — | 0.8896 | — |
| AetherIndex convexo (por dataset, dev) | 0.7318 | 0.3641 | 0.4153 | — | — | 0.8976 | — |
| Referencia: Lucene + exacto + RRF | 0.7273 | 0.3618 | 0.3609 | 0.5354 | 0.8009 | 0.8663 | 0.6088 |
| Qdrant híbrido RRF | 0.7302 | 0.3626 | 0.3545 | 0.4800 | 0.8007 | 0.8669 | 0.5991 |
| OpenSearch híbrido RRF | 0.7286 | 0.3607 | 0.3588 | 0.5349 | 0.7986 | 0.8652 | 0.6078 |

### Recall@100

| Sistema | scifact | nfcorpus | fiqa | arguana | trec-covid | quora | Media |
|---|---:|---:|---:|---:|---:|---:|---:|
| **BM25** | | | | | | | |
| Lucene BM25 (Porter) | 0.9253 | 0.2456 | 0.5395 | 0.9324 | 0.1091 | 0.9733 | 0.6209 |
| AetherIndex BM25 english-v1 | 0.9253 | 0.2478 | 0.5354 | 0.8357 | 0.1085 | 0.9736 | 0.6044 |
| AetherIndex BM25 english-v1, frecuencia | 0.9253 | 0.2456 | 0.5400 | 0.9324 | 0.1091 | 0.9733 | 0.6210 |
| bm25s (tokens english-v1) | 0.9253 | 0.2456 | 0.5400 | 0.9324 | 0.1091 | 0.9734 | 0.6210 |
| bm25s (por defecto) | 0.9127 | 0.2457 | 0.5382 | 0.9381 | 0.1075 | 0.9744 | 0.6194 |
| Qdrant sparse BM25 | 0.9253 | 0.2478 | 0.5354 | 0.8357 | 0.1085 | 0.9735 | 0.6044 |
| OpenSearch BM25 | 0.9253 | 0.2457 | 0.5395 | 0.9324 | 0.1091 | 0.9734 | 0.6209 |
| **BM25 sin stemming** | | | | | | | |
| Lucene sin stemming/stopwords | 0.8852 | 0.2368 | 0.4923 | 0.9075 | 0.0967 | 0.9400 | 0.5931 |
| AetherIndex BM25 ascii-tech-v1 | 0.8659 | 0.2258 | 0.4862 | 0.8179 | 0.0853 | 0.9396 | 0.5701 |
| AetherIndex BM25 english-plain-v1, frecuencia | 0.8852 | 0.2365 | 0.4934 | 0.9104 | 0.0975 | 0.9400 | 0.5938 |
| **BM25 k1=1.2** | | | | | | | |
| Lucene BM25 k1=1.2 b=0.75 | 0.9282 | 0.2463 | 0.5489 | 0.9587 | 0.1119 | 0.9771 | 0.6285 |
| AetherIndex BM25 k1=1.2 b=0.75 | 0.9282 | 0.2482 | 0.5539 | 0.9154 | 0.1125 | 0.9779 | 0.6227 |
| AetherIndex BM25 k1=1.2 b=0.75, frecuencia | 0.9282 | 0.2464 | 0.5525 | 0.9595 | 0.1122 | 0.9771 | 0.6293 |
| **Denso** | | | | | | | |
| Exacto (fuerza bruta) | 0.9417 | 0.3110 | 0.6963 | 0.9879 | 0.1333 | 0.9953 | 0.6776 |
| AetherIndex HNSW FP32 | 0.9417 | 0.3107 | 0.6877 | 0.9879 | 0.1343 | 0.9949 | 0.6762 |
| AetherIndex HNSW SQ8 | 0.9417 | 0.3116 | 0.6888 | 0.9879 | 0.1345 | 0.9949 | 0.6766 |
| Qdrant HNSW | 0.9417 | 0.3110 | 0.6862 | 0.9879 | 0.1333 | 0.9951 | 0.6759 |
| OpenSearch k-NN | 0.9450 | 0.3124 | 0.6904 | 0.9886 | 0.1343 | 0.9943 | 0.6775 |
| **Híbrido** | | | | | | | |
| AetherIndex híbrido RRF | 0.9683 | 0.3139 | 0.6855 | 0.9844 | 0.1337 | 0.9955 | 0.6802 |
| AetherIndex híbrido RRF, frecuencia | 0.9683 | 0.3126 | 0.6886 | 0.9865 | 0.1339 | 0.9955 | 0.6809 |
| AetherIndex RRF ponderado (global, dev) | 0.9450 | 0.3109 | 0.6885 | 0.9879 | 0.1344 | 0.9950 | 0.6769 |
| AetherIndex convexo (global, dev) | 0.9667 | 0.3150 | 0.6858 | 0.9893 | 0.1350 | 0.9954 | 0.6812 |
| AetherIndex RRF ponderado (por dataset, dev) | 0.9450 | 0.3112 | 0.6885 | — | — | 0.9950 | — |
| AetherIndex convexo (por dataset, dev) | 0.9667 | 0.3111 | 0.6945 | — | — | 0.9958 | — |
| Referencia: Lucene + exacto + RRF | 0.9683 | 0.3113 | 0.6895 | 0.9872 | 0.1331 | 0.9954 | 0.6808 |
| Qdrant híbrido RRF | 0.9683 | 0.3127 | 0.6855 | 0.9836 | 0.1334 | 0.9954 | 0.6798 |
| OpenSearch híbrido RRF | 0.9683 | 0.3116 | 0.6892 | 0.9872 | 0.1337 | 0.9955 | 0.6809 |

### Significancia (prueba de aleatorización pareada, nDCG@10, 10,000 permutaciones)

| A vs B | scifact | nfcorpus | fiqa | arguana | trec-covid | quora |
|---|---:|---:|---:|---:|---:|---:|
| AetherIndex BM25 english-v1 vs Lucene BM25 (Porter) | +0.0006 (p=0.854) | +0.0002 (p=0.856) | +0.0018 (p=0.458) | -0.0792 (p=0.000, sig.) | +0.0029 (p=0.438) | +0.0009 (p=0.087) |
| AetherIndex BM25 english-v1, frecuencia vs Lucene BM25 (Porter) | -0.0012 (p=0.200) | -0.0003 (p=0.599) | -0.0007 (p=0.544) | +0.0033 (p=0.001, sig.) | +0.0025 (p=0.461) | +0.0000 (p=0.123) |
| AetherIndex BM25 english-v1, frecuencia vs AetherIndex BM25 english-v1 | -0.0018 (p=0.550) | -0.0005 (p=1.000) | -0.0025 (p=0.243) | +0.0825 (p=0.000, sig.) | -0.0004 (p=0.756) | -0.0008 (p=0.098) |
| AetherIndex BM25 k1=1.2 b=0.75, frecuencia vs Lucene BM25 k1=1.2 b=0.75 | +0.0005 (p=0.055) | +0.0004 (p=0.656) | -0.0013 (p=0.451) | +0.0043 (p=0.002, sig.) | -0.0014 (p=0.376) | +0.0001 (p=0.188) |
| AetherIndex BM25 ascii-tech-v1 vs Lucene sin stemming/stopwords | -0.0216 (p=0.003, sig.) | -0.0114 (p=0.008, sig.) | -0.0081 (p=0.041, sig.) | -0.0570 (p=0.000, sig.) | -0.0436 (p=0.037, sig.) | -0.0025 (p=0.006, sig.) |
| AetherIndex BM25 english-plain-v1, frecuencia vs Lucene sin stemming/stopwords | -0.0030 (p=0.094) | +0.0002 (p=0.726) | +0.0005 (p=0.724) | +0.0032 (p=0.001, sig.) | +0.0009 (p=0.713) | +0.0000 (p=0.500) |
| AetherIndex híbrido RRF vs Referencia: Lucene + exacto + RRF | +0.0030 (p=0.144) | +0.0003 (p=0.809) | -0.0040 (p=0.136) | -0.0523 (p=0.000, sig.) | +0.0018 (p=0.723) | -0.0001 (p=0.718) |
| AetherIndex híbrido RRF, frecuencia vs Referencia: Lucene + exacto + RRF | +0.0018 (p=0.022, sig.) | -0.0006 (p=0.448) | -0.0023 (p=0.293) | +0.0019 (p=0.036, sig.) | -0.0015 (p=0.739) | -0.0001 (p=0.611) |
| AetherIndex híbrido RRF vs AetherIndex BM25 english-v1 | +0.0508 (p=0.000, sig.) | +0.0413 (p=0.000, sig.) | +0.1190 (p=0.000, sig.) | +0.1653 (p=0.000, sig.) | +0.2063 (p=0.000, sig.) | +0.0769 (p=0.000, sig.) |
| AetherIndex híbrido RRF vs Exacto (fuerza bruta) | +0.0176 (p=0.122) | +0.0190 (p=0.008, sig.) | -0.0466 (p=0.000, sig.) | -0.1204 (p=0.000, sig.) | +0.0448 (p=0.025, sig.) | -0.0202 (p=0.000, sig.) |
| AetherIndex convexo (global, dev) vs AetherIndex híbrido RRF, frecuencia | +0.0027 (p=0.717) | -0.0008 (p=0.857) | +0.0551 (p=0.000, sig.) | +0.0633 (p=0.000, sig.) | -0.0133 (p=0.350) | +0.0293 (p=0.000, sig.) |
| AetherIndex RRF ponderado (global, dev) vs AetherIndex híbrido RRF, frecuencia | -0.0053 (p=0.554) | -0.0081 (p=0.107) | +0.0494 (p=0.000, sig.) | +0.0652 (p=0.000, sig.) | +0.0013 (p=0.925) | +0.0224 (p=0.000, sig.) |
| AetherIndex convexo (global, dev) vs Exacto (fuerza bruta) | +0.0191 (p=0.024, sig.) | +0.0173 (p=0.000, sig.) | +0.0102 (p=0.048, sig.) | -0.0030 (p=0.411) | +0.0283 (p=0.007, sig.) | +0.0092 (p=0.000, sig.) |
| AetherIndex convexo (global, dev) vs Referencia: Lucene + exacto + RRF | +0.0045 (p=0.559) | -0.0014 (p=0.760) | +0.0528 (p=0.000, sig.) | +0.0651 (p=0.000, sig.) | -0.0148 (p=0.339) | +0.0292 (p=0.000, sig.) |
| AetherIndex HNSW FP32 vs Exacto (fuerza bruta) | +0.0000 (p=1.000) | +0.0000 (p=0.503) | -0.0021 (p=0.500) | +0.0000 (p=1.000) | +0.0000 (p=1.000) | -0.0002 (p=0.187) |

### Control: Lucene frente a su resultado publicado (nDCG@10)

| Dataset | Publicado (Anserini bm25-flat) | Medido | Diferencia |
|---|---:|---:|---:|
| scifact | 0.6789 | 0.6789 | +0.0000 |
| nfcorpus | 0.3218 | 0.3206 | -0.0012 |
| fiqa | 0.2361 | 0.2361 | -0.0000 |
| arguana | 0.3970 | 0.3970 | -0.0000 |
| trec-covid | 0.5947 | 0.5935 | -0.0012 |
| quora | 0.7886 | 0.7884 | -0.0002 |

### Ajuste de la fusión en dev (nunca en test)

tuned on dev (SciFact: train) judgments only; test never read. Rejilla 0.0–1.0 en pasos de 0.05; 101 candidatos por recuperador.

| Dataset | Consultas dev | nDCG@10 dev RRF | Denso | BM25 | Peso denso W (nDCG@10) | alpha convexo (nDCG@10) |
|---|---:|---:|---:|---:|---:|---:|
| scifact | 809 | 0.7492 | 0.7469 | 0.6971 | 0.75 (0.7598) | 0.65 (0.7706) |
| nfcorpus | 324 | 0.3402 | 0.3276 | 0.2876 | 0.75 (0.3426) | 0.50 (0.3405) |
| fiqa | 500 | 0.3638 | 0.4079 | 0.2576 | 0.95 (0.4120) | 0.75 (0.4174) |
| quora | 5,000 | 0.8641 | 0.8834 | 0.7888 | 0.90 (0.8864) | 0.75 (0.8951) |
| **Global** (media) | | 0.5793 | | | 0.85 (0.5991) | 0.65 (0.6052) |

Los valores globales se aplican a los 6 datasets de prueba (ArguAna y TREC-COVID no tienen dev); los valores por dataset, solo a los 4 con dev.

## Coste en BEIR

### Latencia de consulta (k=10, un hilo) y throughput (10 hilos)

Mediana de 5 repeticiones tras calentamiento. Servidores: medido desde el cliente (incluye HTTP/gRPC local); no comparable 1:1 con librerías en proceso.

| Dataset | Sistema | p50 ms | p99 ms | QPS 10 hilos (IQR) |
|---|---|---:|---:|---:|
| scifact | AetherIndex BM25 (BMW) | 0.055 | 0.306 | 106,306 (106,171–106,370) |
| scifact | AetherIndex BM25 (exhaustivo) | 0.245 | 0.431 | 27,205 (26,895–27,347) |
| scifact | AetherIndex BM25 frecuencia (BMW) | 0.056 | 0.312 | 105,993 (105,831–106,026) |
| scifact | AetherIndex BM25 frecuencia (exhaustivo) | 0.243 | 0.426 | 26,754 (26,690–27,071) |
| scifact | AetherIndex BM25 (MaxScore) | 0.022 | 0.054 | 291,309 (289,984–298,273) |
| scifact | AetherIndex BM25 frecuencia (MaxScore) | 0.021 | 0.050 | 282,431 (274,380–284,080) |
| scifact | Lucene BM25 | 0.069 | 0.151 | 42,976 (42,454–44,240) |
| scifact | bm25s | 0.117 | 0.170 | 11,625 (11,562–11,764) |
| scifact | AetherIndex HNSW | 0.080 | 0.104 | 108,904 (106,311–108,914) |
| scifact | AetherIndex HNSW SQ8 | 0.077 | 0.103 | 115,663 (115,274–116,437) |
| scifact | AetherIndex híbrido | 0.189 | 0.468 | 42,333 (41,937–43,450) |
| scifact | AetherIndex híbrido, frecuencia | 0.175 | 0.479 | 43,740 (43,618–43,804) |
| scifact | AetherIndex híbrido, frecuencia (MaxScore) | 0.189 | 0.372 | 41,825 (41,109–43,849) |
| scifact | AetherIndex RRF ponderado | 0.165 | 0.242 | 53,761 (53,623–53,975) |
| scifact | AetherIndex convexo | 0.167 | 0.248 | 51,092 (50,596–53,416) |
| scifact | Qdrant BM25 † | 0.719 | 1.368 | 6,238 (6,121–6,247) |
| scifact | Qdrant HNSW † | 0.582 | 1.138 | 5,909 (5,889–6,033) |
| scifact | Qdrant híbrido † | 1.221 | 2.034 | 4,799 (4,554–4,821) |
| scifact | OpenSearch BM25 † | 1.134 | 2.000 | 4,355 (4,323–4,363) |
| scifact | OpenSearch k-NN † | 1.363 | 2.767 | 2,976 (2,962–3,168) |
| scifact | OpenSearch híbrido † | 3.717 | 5.072 | 1,568 (1,561–1,661) |
| nfcorpus | AetherIndex BM25 (BMW) | 0.004 | 0.049 | 738,990 (720,915–756,146) |
| nfcorpus | AetherIndex BM25 (exhaustivo) | 0.016 | 0.219 | 139,097 (133,639–140,112) |
| nfcorpus | AetherIndex BM25 frecuencia (BMW) | 0.004 | 0.049 | 682,695 (521,424–736,533) |
| nfcorpus | AetherIndex BM25 frecuencia (exhaustivo) | 0.017 | 0.229 | 133,694 (133,219–135,674) |
| nfcorpus | AetherIndex BM25 (MaxScore) | 0.003 | 0.021 | 753,061 (710,476–902,444) |
| nfcorpus | AetherIndex BM25 frecuencia (MaxScore) | 0.003 | 0.021 | 868,280 (723,202–965,502) |
| nfcorpus | Lucene BM25 | 0.037 | 0.140 | 15,221 (15,187–15,242) |
| nfcorpus | bm25s | 0.081 | 0.116 | 22,336 (22,178–22,850) |
| nfcorpus | AetherIndex HNSW | 0.079 | 0.101 | 106,614 (105,263–109,643) |
| nfcorpus | AetherIndex HNSW SQ8 | 0.075 | 0.097 | 113,523 (113,479–113,729) |
| nfcorpus | AetherIndex híbrido | 0.101 | 0.175 | 81,678 (81,650–82,342) |
| nfcorpus | AetherIndex híbrido, frecuencia | 0.098 | 0.167 | 84,986 (84,651–85,048) |
| nfcorpus | AetherIndex híbrido, frecuencia (MaxScore) | 0.107 | 0.221 | 70,020 (69,939–71,983) |
| nfcorpus | AetherIndex RRF ponderado | 0.101 | 0.168 | 80,837 (79,839–80,866) |
| nfcorpus | AetherIndex convexo | 0.099 | 0.164 | 82,533 (82,035–83,115) |
| nfcorpus | Qdrant BM25 † | 0.654 | 1.290 | 6,549 (6,537–6,595) |
| nfcorpus | Qdrant HNSW † | 0.819 | 1.287 | 5,909 (5,792–6,101) |
| nfcorpus | Qdrant híbrido † | 0.779 | 1.537 | 4,933 (4,844–4,934) |
| nfcorpus | OpenSearch BM25 † | 0.810 | 1.683 | 5,168 (5,107–5,263) |
| nfcorpus | OpenSearch k-NN † | 1.313 | 2.753 | 3,293 (3,291–3,303) |
| nfcorpus | OpenSearch híbrido † | 3.157 | 4.617 | 1,869 (1,851–1,893) |
| fiqa | AetherIndex BM25 (BMW) | 0.446 | 2.458 | 14,463 (14,439–14,693) |
| fiqa | AetherIndex BM25 (exhaustivo) | 3.005 | 5.481 | 2,977 (2,951–3,010) |
| fiqa | AetherIndex BM25 frecuencia (BMW) | 0.495 | 2.476 | 14,871 (14,837–14,885) |
| fiqa | AetherIndex BM25 frecuencia (exhaustivo) | 3.013 | 5.484 | 2,998 (2,981–3,015) |
| fiqa | AetherIndex BM25 (MaxScore) | 0.119 | 0.395 | 65,237 (65,219–65,354) |
| fiqa | AetherIndex BM25 frecuencia (MaxScore) | 0.115 | 0.375 | 65,011 (64,765–65,054) |
| fiqa | Lucene BM25 | 0.153 | 0.404 | 52,744 (51,950–52,931) |
| fiqa | bm25s | 0.422 | 0.630 | 7,182 (7,181–7,187) |
| fiqa | AetherIndex HNSW | 0.272 | 0.451 | 34,839 (33,930–34,878) |
| fiqa | AetherIndex HNSW SQ8 | 0.178 | 0.293 | 54,437 (53,798–55,006) |
| fiqa | AetherIndex híbrido | 1.025 | 3.459 | 8,120 (8,117–8,126) |
| fiqa | AetherIndex híbrido, frecuencia | 0.965 | 3.590 | 8,148 (8,148–8,156) |
| fiqa | AetherIndex híbrido, frecuencia (MaxScore) | 0.773 | 1.539 | 10,250 (10,215–10,300) |
| fiqa | AetherIndex RRF ponderado | 0.515 | 1.182 | 17,184 (17,099–17,226) |
| fiqa | AetherIndex convexo | 0.519 | 1.173 | 16,895 (16,784–17,069) |
| fiqa | Qdrant BM25 † | 0.777 | 2.056 | 6,086 (6,071–6,095) |
| fiqa | Qdrant HNSW † | 1.194 | 2.081 | 4,124 (4,122–4,145) |
| fiqa | Qdrant híbrido † | 1.548 | 2.598 | 3,446 (3,418–3,452) |
| fiqa | OpenSearch BM25 † | 1.057 | 1.546 | 5,019 (4,903–5,042) |
| fiqa | OpenSearch k-NN † | 1.991 | 3.544 | 3,129 (3,037–3,135) |
| fiqa | OpenSearch híbrido † | 4.464 | 6.497 | 1,416 (1,321–1,474) |
| arguana | AetherIndex BM25 (BMW) | 7.853 | 41.941 | 881 (874–882) |
| arguana | AetherIndex BM25 (exhaustivo) | 1.242 | 1.820 | 7,129 (7,111–7,136) |
| arguana | AetherIndex BM25 frecuencia (BMW) | 7.273 | 41.980 | 938 (936–938) |
| arguana | AetherIndex BM25 frecuencia (exhaustivo) | 1.278 | 1.913 | 7,154 (7,021–7,170) |
| arguana | AetherIndex BM25 (MaxScore) | 0.315 | 0.645 | 26,529 (26,519–26,734) |
| arguana | AetherIndex BM25 frecuencia (MaxScore) | 0.284 | 0.612 | 29,823 (29,789–29,892) |
| arguana | Lucene BM25 | 0.521 | 1.192 | 15,598 (15,456–15,657) |
| arguana | bm25s | 0.297 | 0.600 | 1,321 (1,317–1,330) |
| arguana | AetherIndex HNSW | 0.079 | 0.112 | 112,999 (112,353–113,569) |
| arguana | AetherIndex HNSW SQ8 | 0.075 | 0.104 | 120,712 (120,663–121,705) |
| arguana | AetherIndex híbrido | 8.223 | 41.882 | 893 (891–893) |
| arguana | AetherIndex híbrido, frecuencia | 8.732 | 48.374 | 817 (817–817) |
| arguana | AetherIndex híbrido, frecuencia (MaxScore) | 0.787 | 1.416 | 10,479 (10,193–10,723) |
| arguana | AetherIndex RRF ponderado | 0.678 | 1.175 | 13,789 (13,657–13,813) |
| arguana | AetherIndex convexo | 0.657 | 1.126 | 13,873 (13,848–13,885) |
| arguana | Qdrant BM25 † | 1.910 | 2.809 | 5,235 (5,213–5,292) |
| arguana | Qdrant HNSW † | 0.894 | 1.516 | 5,641 (5,594–5,683) |
| arguana | Qdrant híbrido † | 2.268 | 4.209 | 3,885 (3,808–3,886) |
| arguana | OpenSearch BM25 † | 1.468 | 2.858 | 4,064 (3,993–4,095) |
| arguana | OpenSearch k-NN † | 2.178 | 4.675 | 3,259 (3,238–3,312) |
| arguana | OpenSearch híbrido † | 6.509 | 11.707 | 1,143 (1,139–1,144) |
| trec-covid | AetherIndex BM25 (BMW) | 1.210 | 10.772 | 3,185 (3,178–3,194) |
| trec-covid | AetherIndex BM25 (exhaustivo) | 10.453 | 17.590 | 775 (775–776) |
| trec-covid | AetherIndex BM25 frecuencia (BMW) | 1.200 | 10.409 | 3,254 (3,250–3,255) |
| trec-covid | AetherIndex BM25 frecuencia (exhaustivo) | 10.590 | 17.803 | 775 (771–775) |
| trec-covid | AetherIndex BM25 (MaxScore) | 0.292 | 1.117 | 21,029 (20,825–21,154) |
| trec-covid | AetherIndex BM25 frecuencia (MaxScore) | 0.278 | 1.051 | 21,641 (21,595–21,732) |
| trec-covid | Lucene BM25 | 0.363 | 1.059 | 15,654 (15,350–15,657) |
| trec-covid | bm25s | 1.277 | 1.847 | 3,503 (3,466–3,509) |
| trec-covid | AetherIndex HNSW | 0.307 | 0.498 | 22,732 (22,521–23,714) |
| trec-covid | AetherIndex HNSW SQ8 | 0.195 | 0.311 | 43,868 (43,537–43,966) |
| trec-covid | AetherIndex híbrido | 2.122 | 12.780 | 2,267 (2,265–2,272) |
| trec-covid | AetherIndex híbrido, frecuencia | 2.218 | 13.365 | 2,170 (2,161–2,176) |
| trec-covid | AetherIndex híbrido, frecuencia (MaxScore) | 1.287 | 3.397 | 5,587 (5,583–5,588) |
| trec-covid | AetherIndex RRF ponderado | 0.927 | 2.628 | 7,510 (7,314–7,553) |
| trec-covid | AetherIndex convexo | 0.928 | 2.614 | 7,566 (7,458–7,621) |
| trec-covid | Qdrant BM25 † | 0.613 | 0.720 | 5,789 (5,692–5,898) |
| trec-covid | Qdrant HNSW † | 1.170 | 1.455 | 3,351 (3,240–3,387) |
| trec-covid | Qdrant híbrido † | 1.722 | 1.960 | 2,753 (2,678–2,765) |
| trec-covid | OpenSearch BM25 † | 1.233 | 2.468 | 3,772 (3,599–3,944) |
| trec-covid | OpenSearch k-NN † | 1.309 | 2.697 | 2,710 (2,619–2,902) |
| trec-covid | OpenSearch híbrido † | 5.043 | 10.260 | 1,100 (1,041–1,149) |
| quora | AetherIndex BM25 (BMW) | 0.320 | 2.523 | 18,986 (18,965–19,021) |
| quora | AetherIndex BM25 (exhaustivo) | 23.982 | 44.121 | 345 (330–356) |
| quora | AetherIndex BM25 frecuencia (BMW) | 0.332 | 2.774 | 18,454 (18,065–18,492) |
| quora | AetherIndex BM25 frecuencia (exhaustivo) | 23.832 | 43.533 | 356 (356–356) |
| quora | AetherIndex BM25 (MaxScore) | 0.174 | 0.584 | 44,108 (43,965–44,152) |
| quora | AetherIndex BM25 frecuencia (MaxScore) | 0.165 | 0.810 | 42,893 (42,806–42,967) |
| quora | Lucene BM25 | 0.209 | 0.710 | 39,011 (38,556–39,053) |
| quora | bm25s | 2.493 | 4.024 | 3,060 (3,060–3,115) |
| quora | AetherIndex HNSW | 0.384 | 0.585 | 20,630 (20,629–20,676) |
| quora | AetherIndex HNSW SQ8 | 0.306 | 0.451 | 29,604 (29,425–29,706) |
| quora | AetherIndex híbrido | 0.997 | 3.966 | 7,259 (7,208–7,320) |
| quora | AetherIndex híbrido, frecuencia | 0.992 | 4.271 | 7,338 (7,335–7,338) |
| quora | AetherIndex híbrido, frecuencia (MaxScore) | 0.905 | 2.610 | 9,126 (9,006–9,267) |
| quora | AetherIndex RRF ponderado | 0.698 | 2.469 | 11,494 (11,037–11,551) |
| quora | AetherIndex convexo | 0.679 | 2.429 | 11,618 (11,520–11,655) |
| quora | Qdrant BM25 † | 0.903 | 1.465 | 5,920 (5,742–5,983) |
| quora | Qdrant HNSW † | 1.791 | 2.599 | 2,566 (2,525–2,579) |
| quora | Qdrant híbrido † | 2.420 | 3.015 | 2,214 (2,183–2,217) |
| quora | OpenSearch BM25 † | 0.996 | 1.697 | 5,039 (5,021–5,078) |
| quora | OpenSearch k-NN † | 2.384 | 3.945 | 3,005 (2,995–3,039) |
| quora | OpenSearch híbrido † | 3.875 | 7.171 | 1,743 (1,739–1,747) |

† Servidor en Docker, latencia observada por el cliente.

### Construcción del índice léxico (1 hilo) y tamaño

| Dataset | AetherIndex BM25 s | Postings comprimidos MB | Lucene s | Índice Lucene MB | bm25s s | bm25s MB |
|---|---:|---:|---:|---:|---:|---:|
| scifact | 0.15 | 1.6 | 0.84 | 1.1 | 0.12 | 4.2 |
| nfcorpus | 0.11 | 1.2 | 0.70 | 0.8 | 0.09 | 3.2 |
| fiqa | 0.92 | 7.9 | 1.80 | 6.9 | 0.99 | 29.5 |
| arguana | 0.19 | 1.8 | 0.82 | 1.7 | 0.17 | 5.9 |
| trec-covid | 3.78 | 28.0 | 5.56 | 26.5 | 3.75 | 104.6 |
| quora | 1.01 | 14.8 | 2.20 | 15.8 | 2.50 | 32.9 |

Lucene: tiempo de pared de `IndexCollection -threads 1 -optimize` (incluye arranque de la JVM y fusión a un segmento); tamaño en disco sin posiciones ni documentos. bm25s: solo `index()`, sin tokenizar.

## ANN

### sift1m: mejor resultado con Recall@10 ≥ objetivo

Sobre toda la rejilla (M ∈ {16, 32}, efConstruction ∈ {100, 200}, barrido de ef). QPS con 10 hilos; p50 con un hilo.

| Sistema | QPS @0.90 | QPS @0.95 | QPS @0.99 | p50 ms @0.95 | Recall máx. |
|---|---:|---:|---:|---:|---:|
| AetherIndex HNSW | 214,979 | 159,102 | 75,703 | 0.053 | 0.9994 |
| hnswlib 0.8.0 | 93,227 | 57,276 | 29,171 | 0.174 | 0.9993 |
| hnswlib 0.8.0 (-ffast-math) | 127,148 | 80,116 | 41,161 | 0.117 | 0.9993 |
| FAISS HNSWFlat | 80,171 | 47,456 | 25,461 | 0.139 | 0.9994 |
| AetherIndex SQ8 + re-rank | 306,607 | 178,791 | 78,524 | 0.045 | 0.9994 |
| AetherIndex SQ8 sin FP32 | 269,745 | 197,924 | — | 0.039 | 0.9889 |
| FAISS HNSWSQ8 | 117,410 | 67,313 | — | 0.148 | 0.9867 |
| AetherIndex PQ + re-rank | 181,207 | 114,899 | 70,552 | 0.068 | 0.9993 |
| FAISS IVFPQ + refine | 58,115 | 33,178 | — | 0.313 | 0.9869 |
| FAISS Flat (exacto) | 1,130 | 1,130 | 1,130 | 45.809 | 0.9994 |

### sift1m: construcción y memoria (M=16, efConstruction=200)

| Sistema | Construcción 10 hilos s | Construcción 1 hilo s | Memoria proceso MiB | Estructura MiB |
|---|---:|---:|---:|---:|
| AetherIndex HNSW | 34.5 | 258.4 | 702 | 624 |
| hnswlib 0.8.0 | 43.9 | 366.2 | 780 | 626 |
| hnswlib 0.8.0 (-ffast-math) | 30.5 | — | 779 | 626 |
| FAISS HNSWFlat | 56.3 | 351.8 | 664 | 626 |
| AetherIndex SQ8 + re-rank | 30.7 | — | 859 | 746 |
| AetherIndex SQ8 sin FP32 | 30.7 | — | 371 | 258 |
| FAISS HNSWSQ8 | 46.8 | — | 297 | 260 |
| AetherIndex PQ + re-rank | 34.6 | — | 736 | 655 |
| FAISS IVFPQ + refine | 8.6 | — | 838 | 527 |
| FAISS Flat (exacto) | 0.0 | — | 489 | 488 |

### sift1m: servidores (cliente local, M=16, ef_construct=200)

| Servidor | ef | Recall@10 | p50 ms † | QPS 10 clientes † | Ingesta+índice s | Memoria contenedor MiB |
|---|---:|---:|---:|---:|---:|---:|
| opensearch | 16 | 0.8166 | 1.203 | 5,001 | 439 | 6809 |
| opensearch | 32 | 0.9157 | 1.580 | 4,977 | 439 | 6809 |
| opensearch | 64 | 0.9704 | 1.423 | 4,919 | 439 | 6809 |
| opensearch | 128 | 0.9920 | 0.842 | 4,780 | 439 | 6809 |
| opensearch | 256 | 0.9981 | 1.162 | 4,342 | 439 | 6809 |
| opensearch | 512 | 0.9992 | 1.642 | 3,510 | 439 | 6809 |
| qdrant | 16 | 0.9008 | 0.736 | 5,996 | 66 | 3466 |
| qdrant | 32 | 0.9638 | 0.775 | 5,741 | 66 | 3466 |
| qdrant | 64 | 0.9893 | 1.574 | 5,184 | 66 | 3466 |
| qdrant | 128 | 0.9971 | 1.755 | 4,286 | 66 | 3466 |
| qdrant | 256 | 0.9991 | 1.391 | 3,242 | 66 | 3466 |
| qdrant | 512 | 0.9991 | 2.383 | 2,261 | 66 | 3466 |

### glove-100: mejor resultado con Recall@10 ≥ objetivo

Sobre toda la rejilla (M ∈ {16, 32}, efConstruction ∈ {100, 200}, barrido de ef). QPS con 10 hilos; p50 con un hilo.

| Sistema | QPS @0.90 | QPS @0.95 | QPS @0.99 | p50 ms @0.95 | Recall máx. |
|---|---:|---:|---:|---:|---:|
| AetherIndex HNSW | 32,691 | 12,550 | — | 0.642 | 0.9859 |
| hnswlib 0.8.0 | 14,346 | 7,842 | — | 1.193 | 0.9799 |
| hnswlib 0.8.0 (-ffast-math) | 16,778 | 9,028 | — | 0.975 | 0.9794 |
| FAISS HNSWFlat | 9,820 | 5,181 | — | 1.270 | 0.9785 |
| AetherIndex SQ8 + re-rank | 22,499 | 11,348 | — | 0.730 | 0.9536 |
| AetherIndex SQ8 sin FP32 | 24,858 | — | — | — | 0.9364 |
| FAISS HNSWSQ8 | 4,995 | — | — | — | 0.9257 |
| AetherIndex PQ + re-rank | 20,440 | — | — | — | 0.9432 |
| FAISS IVFPQ + refine | — | — | — | — | 0.8745 |
| FAISS Flat (exacto) | 2,706 | 2,706 | 2,706 | 40.654 | 1.0000 |

### glove-100: construcción y memoria (M=16, efConstruction=200)

| Sistema | Construcción 10 hilos s | Construcción 1 hilo s | Memoria proceso MiB | Estructura MiB |
|---|---:|---:|---:|---:|
| AetherIndex HNSW | 45.7 | 354.7 | 761 | 612 |
| hnswlib 0.8.0 | 56.1 | 459.1 | 791 | 615 |
| hnswlib 0.8.0 (-ffast-math) | 42.6 | — | 791 | 615 |
| FAISS HNSWFlat | 82.2 | 485.1 | 669 | 614 |
| AetherIndex SQ8 + re-rank | 43.1 | — | 919 | 725 |
| AetherIndex SQ8 sin FP32 | 42.3 | — | 413 | 274 |
| FAISS HNSWSQ8 | 113.6 | — | 332 | 276 |
| AetherIndex PQ + re-rank | 44.1 | — | 792 | 641 |
| FAISS IVFPQ + refine | 6.8 | — | 717 | 489 |
| FAISS Flat (exacto) | 0.0 | — | 452 | 451 |

### glove-100: servidores (cliente local, M=16, ef_construct=200)

| Servidor | ef | Recall@10 | p50 ms † | QPS 10 clientes † | Ingesta+índice s | Memoria contenedor MiB |
|---|---:|---:|---:|---:|---:|---:|
| qdrant | 16 | 0.6541 | 0.759 | 5,983 | 96 | 2256 |
| qdrant | 32 | 0.7556 | 0.792 | 5,720 | 96 | 2256 |
| qdrant | 64 | 0.8323 | 0.995 | 5,031 | 96 | 2256 |
| qdrant | 128 | 0.8893 | 1.127 | 3,991 | 96 | 2256 |
| qdrant | 256 | 0.9306 | 1.598 | 2,957 | 96 | 2256 |
| qdrant | 512 | 0.9604 | 2.286 | 1,915 | 96 | 2256 |
| opensearch | 16 | 0.5806 | 0.784 | 4,614 | 674 | 5550 |
| opensearch | 32 | 0.6924 | 1.836 | 4,595 | 674 | 5550 |
| opensearch | 64 | 0.7799 | 0.887 | 4,519 | 674 | 5550 |
| opensearch | 128 | 0.8446 | 2.127 | 4,384 | 674 | 5550 |
| opensearch | 256 | 0.8942 | 1.261 | 3,981 | 674 | 5550 |
| opensearch | 512 | 0.9322 | 1.912 | 3,211 | 674 | 5550 |

### HNSW sobre los vectores bge-small de BEIR (M=16, efConstruction=200)

Recall@10 frente a la búsqueda exacta; QPS con 10 hilos.

| Dataset | ef | AetherIndex R@10 / QPS | hnswlib R@10 / QPS | FAISS R@10 / QPS |
|---|---:|---|---|---|
| scifact | 32 | 0.9883 / 274,820 | 0.9720 / 108,779 | 0.9743 / 141,819 |
| scifact | 64 | 0.9953 / 187,324 | 0.9930 / 60,743 | 0.9920 / 86,266 |
| scifact | 128 | 0.9997 / 116,414 | 0.9993 / 39,262 | 0.9983 / 48,234 |
| nfcorpus | 32 | 0.9455 / 232,040 | 0.9077 / 112,939 | 0.9139 / 173,894 |
| nfcorpus | 64 | 0.9796 / 180,923 | 0.9594 / 66,019 | 0.9659 / 102,589 |
| nfcorpus | 128 | 0.9923 / 110,031 | 0.9839 / 41,571 | 0.9916 / 57,787 |
| fiqa | 32 | 0.9508 / 164,450 | 0.9136 / 61,012 | 0.9167 / 86,592 |
| fiqa | 64 | 0.9779 / 97,436 | 0.9625 / 29,626 | 0.9628 / 48,290 |
| fiqa | 128 | 0.9912 / 53,996 | 0.9850 / 20,225 | 0.9843 / 26,394 |
| arguana | 32 | 0.9992 / 346,836 | 0.9836 / 106,965 | 0.9969 / 171,362 |
| arguana | 64 | 0.9999 / 215,045 | 0.9932 / 56,025 | 0.9984 / 95,187 |
| arguana | 128 | 1.0000 / 112,097 | 0.9965 / 35,743 | 0.9986 / 45,931 |
| trec-covid | 32 | 0.9640 / 97,221 | 0.9560 / 50,899 | 0.9500 / 60,253 |
| trec-covid | 64 | 0.9880 / 66,797 | 0.9840 / 30,279 | 0.9880 / 38,155 |
| trec-covid | 128 | 1.0000 / 38,328 | 0.9960 / 16,547 | 0.9940 / 21,687 |
| quora | 32 | 0.9896 / 105,191 | 0.9786 / 44,069 | 0.9862 / 56,283 |
| quora | 64 | 0.9946 / 61,707 | 0.9895 / 25,923 | 0.9932 / 32,218 |
| quora | 128 | 0.9970 / 34,644 | 0.9944 / 13,965 | 0.9964 / 17,619 |

