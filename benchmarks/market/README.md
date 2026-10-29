# Comparación con el mercado

Arneses para medir AetherIndex contra hnswlib, FAISS, Lucene (Anserini), bm25s, Qdrant y
OpenSearch con los mismos datos, la misma máquina y el mismo protocolo. Resultados e
interpretación: [docs/reports/MARKET_COMPARISON.md](../../docs/reports/MARKET_COMPARISON.md).
Tablas generadas: `evaluation/reports/market/summary.md`.

## Protocolo de equidad

1. Misma máquina, mismos archivos, mismo `k`, mismo código de métricas y mismos hilos
   (1 para latencia, 10 núcleos de rendimiento para throughput). Cada sistema y
   configuración corre en un proceso nuevo, sin otra carga.
2. Se ajusta cada sistema, no solo AetherIndex: curvas recall–QPS sobre la misma rejilla
   (M ∈ {16, 32}, efConstruction ∈ {100, 200}, barrido de ef) y la configuración por
   defecto de cada uno.
3. hnswlib y FAISS corren dentro del mismo arnés C++ (`ann_compare.cpp`) que AetherIndex:
   mismo compilador y opciones, mismo temporizador, mismo `parallel_for` (FAISS usa su
   OpenMP para lotes). hnswlib 0.8.0 no tiene kernels SIMD para ARM, así que también se
   mide compilado con `-ffast-math` (su mejor caso). Lucene se mide dentro de la JVM tras
   calentamiento. Los servidores (Docker) incluyen la red local y van en tablas aparte.
4. Análisis equivalente: BM25 con k1/b iguales (0.9/0.4 de BEIR y 1.2/0.75); Lucene con
   y sin stemming frente a `english-v1` y `ascii-tech-v1`; bm25s por defecto y con los
   tokens de `english-v1`; Qdrant con BM25 disperso calculado con la misma fórmula.
   AetherIndex se mide con las dos ponderaciones de consulta (`unique` por defecto y
   `frequency`, que cuenta los términos repetidos como Lucene y bm25s) y con las estrategias
   `bmw` y `maxscore` (resultados idénticos, distinta velocidad).
5. Rigor: 5 repeticiones (mediana e IQR), recall contra el ground truth oficial, nDCG@10,
   Recall@100 y MRR@10 con un evaluador propio contrastado con `trec_eval`, y prueba de
   aleatorización pareada por consulta.
6. Transparencia: versiones, digests, hashes de datos, configuraciones y runs por
   consulta en `evaluation/reports/market/`; se reportan también los resultados perdidos.

## Reproducir

Requisitos (se instalan en carpetas que Git ignora): `.venv-bench/` con
`requirements.txt`, `brew install faiss`, Anserini 2.3.0 en `tools/anserini/`, Docker.

```sh
python3 -m venv .venv-bench && .venv-bench/bin/pip install -r benchmarks/market/requirements.txt
cmake -S . -B build-market -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DAETHER_BUILD_MARKET_BENCH=ON
cmake --build build-market --parallel 10
javac -cp tools/anserini/anserini-2.3.0-fatjar.jar -d build-market/java benchmarks/market/java/*.java

# Datos: BEIR (zips en datasets/market/downloads/beir), GloVe-100 (HDF5), SIFT1M.
python3 -I benchmarks/market/prepare_beir.py
.venv-bench/bin/python -I benchmarks/market/embed_beir.py scifact nfcorpus arguana fiqa trec-covid quora
.venv-bench/bin/python -I benchmarks/market/prepare_ann.py

# Campaña en proceso (reanudable) y servidores, uno a la vez.
python3 benchmarks/market/run_market_campaign.py
docker compose -f benchmarks/market/docker-compose.yml up -d qdrant
.venv-bench/bin/python benchmarks/market/run_servers.py --server qdrant --task beir --dataset scifact
.venv-bench/bin/python benchmarks/market/run_servers.py --server qdrant --task ann --dataset sift1m
docker compose -f benchmarks/market/docker-compose.yml stop qdrant   # y lo mismo con opensearch

.venv-bench/bin/python scripts/summarize_market.py
```

## Archivos

| Archivo | Función |
|---|---|
| `ann_compare.cpp` | ANN: AetherIndex (FP32, SQ8, PQ), hnswlib y FAISS (HNSW, HNSWSQ, IVFPQ, Flat) |
| `ir_compare.cpp` | BEIR: runs TREC de AetherIndex BM25/denso/híbrido, latencia y volcado de tokens |
| `java/LuceneSearch.java` | Lucene BM25 con el mismo protocolo de latencia (consultas como Anserini) |
| `java/LuceneTokens.java` | Tokens que indexa Anserini, para la paridad del analizador |
| `run_bm25s.py`, `run_servers.py` | bm25s; Qdrant y OpenSearch |
| `prepare_beir.py`, `embed_beir.py`, `prepare_ann.py` | Conversión de datos y embeddings bge-small |
| `dense_reference.py`, `fuse_rrf.py` | Búsqueda densa exacta y RRF de referencia |
| `evaluate.py`, `compare_tokens.py` | Métricas, significancia y paridad de tokens |
| `run_market_campaign.py` | Orquestación reanudable y manifiesto |
