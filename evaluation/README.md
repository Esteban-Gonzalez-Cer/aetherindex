# Evaluación del Sprint 2

Reportes legibles por máquina de los KR del Sprint 2. Cada reporte se genera con
código del repositorio; no se editan a mano. El resumen consolidado, la decisión
de aceptación y los pendientes están en
[la revisión del Sprint 2](../docs/sprints/sprint_2_review.md).

## Reproducir

```sh
cmake -S . -B build-sprint2-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build-sprint2-release --parallel 4
ctest --test-dir build-sprint2-release --output-on-failure --output-junit ctest-junit.xml
cmake -S . -B build-sprint2-debug -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build-sprint2-debug --parallel 4
ctest --test-dir build-sprint2-debug --output-on-failure --output-junit ctest-junit.xml
python3 scripts/collect_sprint2_evidence.py --build release=build-sprint2-release \
    --build debug=build-sprint2-debug
```

Para agregar
sanitizers, configurar otro directorio con `-DAETHER_ENABLE_SANITIZERS=ON` y un
Clang/GCC compatible, y pasar `--build sanitizers=DIR`. En macOS con Clang de
Homebrew también hace falta `-DCMAKE_OSX_SYSROOT=macosx`; sin esa opción, sus
headers de libc++ piden `from_chars<float>`, que la libc++ del sistema no exporta,
y el enlace falla:

```sh
cmake -S . -B build-sprint2-sanitizers -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON \
    -DAETHER_ENABLE_SANITIZERS=ON -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ \
    -DCMAKE_OSX_SYSROOT=macosx
```

KR3 (rendimiento, compuerta separada; requiere `datasets/real/dbpedia/`, generado con
`scripts/prepare_real_dataset.py`):

```sh
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DAETHER_BUILD_BENCHMARKS=ON
cmake --build build-bench --target aether_hnsw_parameter_sweep --parallel 4
./build-bench/benchmarks/aether_hnsw_parameter_sweep evaluation/reports/kr3_dbpedia \
    --dbpedia datasets/real/dbpedia/vectors.csv
```

Sin DBpedia, omitir `--dbpedia ...` para usar la mezcla gaussiana sintética con semilla fija.
No ejecutar compilaciones u otras cargas en paralelo durante la medición.

## Archivos

| Archivo | KR | Contenido |
|---|---|---|
| `reports/kr1_3_bm25.csv` | KR1.3 | Por consulta: etiquetas, Top-10 BM25, rango del primer relevante, Hit@1/Hit@10 |
| `reports/kr1_3_summary.json` | KR1.3 | Hit Rate@10, Hit Rate@1, MRR@10, parámetros, PASS/FAIL |
| `reports/kr1_4_rrf.csv` | KR1.4 | Los 22 casos RRF obligatorios y su resultado |
| `reports/kr1_5_hybrid.csv` | KR1.5 | Por consulta híbrida: candidatos, unión, devueltos, error máximo de score frente al recálculo independiente, PASS/FAIL |
| `reports/kr1_5_summary.json` | KR1.5 | Consultas aprobadas y nota de alcance |
| `reports/kr1_7_acceptance.csv` | KR1.7 | Resultado de cada test CTest por perfil y filas por integrante (PENDING hasta su ejecución) |
| `reports/sprint2_okr1_summary.json` | Todos | Commit, entorno, compiladores, resultados KR |
| `reports/kr3_dbpedia/kr3_1_hnsw_parameters.csv` | KR3.1 | M ∈ {8,16,32} × efSearch ∈ {10,50,100}; efConstruction ∈ {50,100,200} |
| `reports/kr3_dbpedia/kr3_2_recall_latency.csv` | KR3.2 | efSearch ∈ {10,25,50,100,200}, Recall@10 y latencia |
| `reports/kr3_dbpedia/kr3_8_latency_percentiles.csv` | KR3.8 | p50/p95/p99 por configuración |
| `reports/kr3_dbpedia/raw_latencies.csv` | KR3 | Latencia y Recall@10 de cada consulta medida |
| `reports/kr3_dbpedia/workload.json` | KR3 | Dataset, división, semillas, warm-up, alcance del tiempo, compilador |

Para registrar una reproducción de KR1.7, cada integrante ejecuta el inicio rápido del
README en su máquina y cambia su fila `member-N` de `PENDING` a `PASS`/`FAIL`,
anotando el sistema operativo, el compilador y el commit en la revisión del Sprint.

## Optimización (post Sprint 2)

`evaluation/reports/optimization/<fase>/<workload>/` contiene, por configuración,
`*_summary.csv` (una fila por repetición), `*_results.csv` (Top-K de la primera repetición) y
`*_workload.json`, además de un `manifest.json` por fase con los hashes de las fuentes, el
binario, el commit y la máquina. Para reproducir:

```sh
cmake -S . -B build-opt-bench -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DAETHER_BUILD_BENCHMARKS=ON
cmake --build build-opt-bench --target aether_optimization_suite --parallel 4
python3 scripts/run_optimization_suite.py --phase NOMBRE --suite build-opt-bench/benchmarks/aether_optimization_suite --only vec_synth_100k_128
scripts/run_optimization_campaign.sh build-opt-bench/benchmarks/aether_optimization_suite BINARIO_BASE
python3 scripts/summarize_optimization.py
```

Los datasets públicos opcionales se preparan con `scripts/prepare_real_dataset.py
--output-dir datasets/real/dbpedia-100k --vectors 99980 --queries 20` (DBpedia 100K) y con
SIFT1M de TEXMEX (`ftp://ftp.irisa.fr/local/texmex/corpus/sift.tar.gz`, descomprimido en
`datasets/real/sift1m/`). Ambos quedan fuera de Git. Resultados e interpretación:
[informe de optimización](../docs/reports/OPTIMIZATION_ROADMAP.md).
