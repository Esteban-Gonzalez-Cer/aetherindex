# Optimización posterior al Sprint 2 — resultados

**Fecha:** 28 de octubre de 2026 · **Rama:** `develop` (cambios sin commit sobre `4203fdf`)
**Máquina:** Apple M4 Pro (10 núcleos de rendimiento + 4 de eficiencia), 24 GiB, macOS 26.5,
AppleClang 17 en Release. **Datos crudos:**
[evaluation/reports/optimization](../../evaluation/reports/optimization) (`summary.csv`
consolida medianas; cada fase tiene `manifest.json` con hashes de fuentes y del binario).

## 1. Resumen

| Fase | Cambio | Meta del plan | Resultado | Estado |
|---|---|---|---|---|
| 1 | Formato binario `.avec` + `convert` | Carga ≥ 5× más rápida | **35× vs JSONL, 85–96× vs CSV** | Cumplida |
| 2 | Construcción HNSW paralela | ≥ 4× con 10 hilos, Recall ≥ secuencial − 0.005 | **6.0–7.4×**, Recall −0.001 a +0.001 | Cumplida |
| 2 | Consultas en lote | Escalado ~lineal | **6.2–8.0× con 10 hilos** (62–80% de eficiencia) | Parcial |
| 2 | Recuperadores concurrentes (híbrido) | Activar solo si mejora | 13% **más lento** → queda opcional y apagado | Cumplida (decisión) |
| 2 | Registro de visitados por hilo | Sin meta numérica | Neutral (±6%, dentro del ruido) | Informativo |
| 3 | SQ8 + reordenamiento exacto | Memoria 4× menor, Recall −0.01 como máximo | Códigos **3.9–4.0×** menores; Recall **−0.001**; huella de proceso con `--drop-fp32`: **−45% (SIFT1M), −68% (DBpedia)** | Cumplida |
| 4 | PQ 16× / 32× | ≥ 16× de compresión, curva por m | Códigos **15.3–16×** y **29–32×**; Recall 0.97–0.99 con ef=128 | Cumplida (con advertencia) |
| 5 | BM25 comprimido + WAND/BMW | Postings ≥ 2× menores; BMW ≥ 3× más rápido; 100% idéntico | **2.2× (100K) / 3.8× (1M)**; BMW **73× p50 y 63× p99** en 1M; resultados idénticos | Cumplida |
| 6 | Filtros por metadatos | Ruta exacta 100% correcta; Recall ≥ 0.95 con ≥ 10% de selectividad | Exacta = fuerza bruta filtrada; Recall **0.985–0.999** | Cumplida |

**Invariante:** la ruta por defecto no cambió. `e2e.cli_golden` compara 11 salidas de la
CLI byte a byte, y los Top-K del benchmark por defecto son idénticos al baseline en
synthetic 100K, SIFT1M, synthetic 1M y DBpedia 100K. Calidad: 70/70 tests en Release,
ASan/UBSan y TSan (antes de este trabajo eran 60), 0 warnings y 0 hallazgos de sanitizers.

## 2. Método

- Un driver (`benchmarks/optimization_suite.cpp`) y una matriz fija de workloads
  (`scripts/run_optimization_suite.py`, `scripts/run_optimization_campaign.sh`). Se
  ejecuta un workload a la vez, con 100 búsquedas de calentamiento y 1,000 consultas
  medidas. La verdad de referencia es BruteForce (o la oficial de SIFT1M). Los tiempos de
  carga, construcción y consulta se miden por separado.
- Datos:
  - Gaussiana sintética con semilla (10K, 100K y 1M × 128).
  - DBpedia 10K y 100K: embeddings reales de 1,024 dimensiones. Las filas con
    `fila % 10 == 9` se reservan como consultas.
  - SIFT1M: 1M × 128, distancia L2.
  - Texto Zipf sintético (100K y 1M documentos) para medir BM25 (rendimiento, no relevancia).
- Repeticiones: los workloads livianos se ejecutan 3 veces y se informa la mediana. Los
  pesados (1M filas, DBpedia 100K) se ejecutan 1 vez para acotar el tiempo de la campaña.
- **Baseline de los workloads pesados.** El árbol no tenía commit y no se conservó una
  copia de las fuentes previas. Por eso, el baseline pesado se midió con una copia del
  árbol actual a la que se le revirtió el único cambio de rendimiento en la ruta por
  defecto (el registro de visitados). Al medirlo otra vez sobre workloads livianos,
  reprodujo el Phase 0 original con una diferencia de ±10%, que corresponde a la deriva
  de la máquina entre sesiones (ver §5).

## 3. Resultados por fase

### Fase 1 — `.avec` (carga con validación e inserción en el VectorStore; mediana de 3)

| Workload | CSV | JSONL | `.avec` | Archivo `.avec` |
|---|---:|---:|---:|---:|
| 10K × 1024 | 2,158 ms | 792 ms | **22.5 ms** | 41 MB (vs 123 MB) |
| 100K × 128 | 2,734 ms | 1,104 ms | **32.0 ms** | 52 MB (vs 154 MB) |

Incluye la verificación CRC-32C y la normalización coseno. Los resultados de búsqueda son
idénticos a los de CSV/JSONL (`e2e.cli_avec`).

### Fase 2 — paralelismo (10 hilos vs 1 hilo, mismo binario)

| Workload | Construcción 1 → 10 hilos | Aceleración | Recall@10 1 → 10 hilos | Lote 1 → 10 hilos (q/s) |
|---|---:|---:|---|---:|
| synthetic 100K × 128 | 16.8 s → 2.26 s | 7.4× | 0.9675 → 0.9665 | 14,877 → 119,470 (8.0×) |
| DBpedia 10K × 1024 | 7.45 s → 1.02 s | 7.3× | 0.9886 → 0.9886 | 4,339 → 31,397 (7.2×) |
| SIFT1M | 247 s → 41.0 s | 6.0× | 0.9499 → 0.9501 | 9,156 → 56,910 (6.2×) |
| synthetic 1M × 128 | 329 s → 46.3 s | 7.1× | 0.7045 → 0.7058 | 7,287 → 56,587 (7.8×) |
| DBpedia 100K × 1024 | 110 s → 17.2 s | 6.4× | 0.9691 → 0.9691 | 2,857 → 18,580 (6.5×) |

- El grafo paralelo no es reproducible bit a bit (por diseño). Con `--build-threads 1` se
  obtiene exactamente el grafo secuencial.
- Una consulta individual sobre un grafo paralelo puede ser un poco más lenta (por
  ejemplo, DBpedia 100K: p50 0.363 → 0.379 ms) porque la topología cambia.
- Recuperadores concurrentes, híbrido Zipf 100K: p50 0.943 → 1.061 ms. Lanzar el hilo
  cuesta más que el solapamiento logrado, así que `--parallel-retrievers` queda apagado.
- Registro de visitados por hilo: sin ganancia consistente. Con SIFT1M la construcción
  bajó 3.5% y con synthetic 1M subió 6%. Una hipótesis plausible es que 4 bytes por nodo
  ocupan más caché que 1 bit, aunque eviten reservar y limpiar memoria en cada búsqueda.

### Fase 3 — SQ8 (construcción con 10 hilos, ef=50, reordenamiento de todos los candidatos)

| Workload | Recall FP32 → SQ8 → SQ8 sin FP32 | p50 FP32 → SQ8 | Vectores FP32 / códigos | Huella del proceso FP32 → SQ8 → SQ8 sin FP32 |
|---|---|---|---|---|
| SIFT1M | 0.9500 → 0.9489 → 0.9423 | 0.115 → 0.086 ms | 512 MB / 132 MB | 718 → 840 → **398 MiB** |
| DBpedia 100K | 0.9691 → 0.9690 → 0.9661 | 0.364 → 0.271 ms | 369 MB / 92 MB | 387 → 475 → **123 MiB** |

- Con el reordenamiento exacto, el Recall cae a lo sumo 0.0011 y la búsqueda es alrededor de 25%
  más rápida (p50), porque se recorren menos bytes.
- Si se conservan las filas FP32, la huella **crece** por los códigos. El ahorro de memoria
  real exige `--drop-fp32` (que implica `--rerank 0`), con una caída de Recall de 0.003 a
  0.008.
- Entrenar y codificar SQ8 tarda 0.12–0.18 s.
- **Corrección durante la medición.** Primero, `resident_size` no reflejaba la memoria
  liberada, así que se pasó a `phys_footprint`. Segundo, en macOS el bloque alineado de
  512 MB, una vez liberado, seguía dentro del proceso. Por eso, `AlignedAllocator` ahora
  mapea con `mmap`/`munmap` los búferes de 1 MiB o más (POSIX). Esa corrección es lo que
  hace visible el −45% / −68%.

### Fase 4 — PQ (ef=128, reordenamiento exacto, FP32 conservado)

| Workload | m (compresión) | Códigos | Recall@10 | p50 | Entrenamiento |
|---|---|---:|---:|---:|---:|
| SIFT1M | 32 (16×) | 32 MB | 0.9854 | 0.267 ms | 1.4 s |
| SIFT1M | 16 (32×) | 16 MB | 0.9706 | 0.186 ms | 1.5 s |
| DBpedia 100K | 256 (16×) | 24 MB | 0.9909 | 0.680 ms | 3.1 s |
| DBpedia 100K | 128 (32×) | 13 MB | 0.9888 | 0.444 ms | 3.1 s |

**Advertencia:** el Recall de PQ se midió con ef=128 y FP32 con ef=50, así que no es una
comparación directa. Un FP32 con ef=128 también tendría más Recall y más latencia (el
barrido KR3 en DBpedia 9K da 0.9972 con ef=100). Las cifras muestran que PQ con
reordenamiento recupera un Recall alto. **No** muestran que PQ supere a FP32. Tampoco se
midió PQ sin FP32: el ahorro de memoria de PQ requiere `--drop-fp32 --rerank 0`, y su Recall
aproximado es bastante menor (en la prueba de integración, entre 0.31 y 0.96 según m).

### Fase 5 — BM25 a escala (1,000 consultas Zipf de 2 a 5 términos, k=10)

| Corpus | Estrategia | p50 | p99 | Postings | Construcción |
|---|---|---:|---:|---:|---:|
| 100K docs | exhaustiva (original) | 0.856 ms | 7.90 ms | 67.9 MB | 1.23 s |
| 100K docs | taat | 0.065 ms | 0.70 ms | 30.6 MB | 1.54 s |
| 100K docs | wand | 0.027 ms | 0.32 ms | 30.6 MB | 1.47 s |
| 100K docs | **bmw** | **0.025 ms** | **0.30 ms** | 30.6 MB | 1.54 s |
| 1M docs | exhaustiva (original) | 13.1 ms | 107 ms | 637 MB | 19.7 s |
| 1M docs | taat | 0.657 ms | 6.97 ms | 167 MB | 21.7 s |
| 1M docs | wand | 0.168 ms | 2.37 ms | 167 MB | 21.1 s |
| 1M docs | **bmw** | **0.179 ms** | **1.71 ms** | 167 MB | 21.5 s |

- Los Top-10 (IDs y scores impresos con 17 dígitos) son idénticos byte a byte a los de la
  ruta exhaustiva en 100K y en 1M. `unit.bm25_strategies` además verifica 13,620
  comparaciones, incluidos empates masivos.
- Híbrido Zipf 100K con `bmw`: p50 0.943 → **0.078 ms** (12×) y p99 8.88 → **0.57 ms** (16×).
  El canal léxico dominaba la latencia híbrida.
- Costos: la construcción tarda entre 7% y 25% más, y en un corpus de juguete (CP-01, 30
  documentos) el formato comprimido ocupa **más** (32 KB vs 15 KB) por las tablas por
  documento. `exhaustive` sigue siendo el valor por defecto.

### Fase 6 — filtros (densos; construcción con 10 hilos, ef=50)

| Selectividad | Workload | Recall del grafo filtrado | p50 del grafo | p50 exacto | Estrategia que elige el producto |
|---|---|---:|---:|---:|---|
| 1% | synthetic 100K | 0.997 | 3.00 ms | 0.018 ms | exacta |
| 1% | DBpedia 100K | 0.999 | 12.5 ms | 0.080 ms | exacta |
| 10% | synthetic 100K | 0.999 | 0.20 ms | 0.149 ms | grafo |
| 10% | DBpedia 100K | 0.994 | 2.20 ms | 3.08 ms | grafo |
| 50% | synthetic 100K | 0.995 | 0.10 ms | 1.33 ms | grafo |
| 50% | DBpedia 100K | 0.985 | 0.62 ms | 6.25 ms | grafo |

- El cambio a búsqueda exacta (< 5% o < 2,000 coincidencias) evita el peor caso del grafo.
- Con un 10% de coincidencias, el grafo filtrado en DBpedia es unas 6 veces más lento que
  la búsqueda sin filtro (p50 2.20 vs 0.36 ms), porque necesita recorrer más nodos.
- En synthetic, al 10% la búsqueda exacta fue más rápida que el grafo. El umbral podría
  ajustarse hacia ~10% según el dataset (ver §6).

## 4. Correcciones hechas durante el trabajo

- Concurrencia en la construcción paralela: la primera versión publicaba un nodo antes de
  completar sus listas de niveles inferiores (Recall −0.0075 con 14 hilos). Ahora cada
  nodo publica sus enlaces inversos después de escribir todos sus niveles (Recall 1.0 en la
  prueba).
- La compresión de postings no alcanzaba 2× (1.68×). Se agregó `tf=1` en un bit y cotas por
  bloque en float redondeado hacia arriba, y llegó a 2.13×.
- PQ con ef=64 daba Recall 0.90 tras reordenar. Se documentó que conviene un ef mayor, y la
  prueba usa 128.
- Medición de memoria: ver §3, Fase 3. Además, una repetición de mediciones falló sin que se
  notara (problema de shell) y se volvió a ejecutar. Las cifras de las Fases 1 y 2 y el
  baseline de DBpedia 100K provienen de esa repetición limpia.

## 5. Limitaciones de la evidencia

- Una sola máquina (macOS ARM). No hay resultados en x86, Linux ni Windows.
- Deriva de ±10% entre sesiones de medición: por ejemplo, el p50 del synthetic 100K por
  defecto fue de 0.060 a 0.068 ms en distintas corridas. Solo se comparan corridas cercanas
  en el tiempo, y los cambios menores al 10% se tratan como neutrales.
- Los workloads pesados tienen una sola repetición.
- Parte de la campaña coincidió con la descarga de DBpedia 100K (<2% de un núcleo). Los
  workloads que coincidieron con su escritura final de CSV se repitieron.
- El cambio de `AlignedAllocator` (mmap ≥ 1 MiB) entró después de la campaña. Se volvieron
  a medir solo las fases de memoria y cuantización (3 y 4) y las reejecuciones de §4; las
  demás cifras usan el asignador anterior. El cambio afecta la reserva de búferes grandes,
  no el recorrido de búsqueda.
- Los corpus Zipf miden rendimiento, no relevancia. La calidad de búsqueda contra
  productos de mercado pertenece a la siguiente etapa.

## 6. Recomendaciones

1. Hacer commit de este estado y repetir la campaña completa sobre un commit limpio para
   tener un baseline fijo antes de comparar contra hnswlib/FAISS/Lucene.
2. Considerar `bmw` como estrategia BM25 por defecto para corpus grandes: es idéntica en
   resultados y entre 34× y 73× más rápida. Para corpus pequeños conviene mantener
   `exhaustive`.
3. Para despliegues con poca memoria, usar SQ8 con `--drop-fp32`. Para mejor Recall con
   memoria acotada, usar SQ8 con reordenamiento (requiere conservar FP32).
4. Ajustar el umbral de filtrado exacto/grafo por dataset (5% fue conservador en synthetic).
5. La persistencia queda pendiente: cada proceso todavía reconstruye el grafo (17 s para
   DBpedia 100K con 10 hilos).

## 7. Reproducir

```sh
cmake -S . -B build-opt-bench -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DAETHER_BUILD_BENCHMARKS=ON
cmake --build build-opt-bench --target aether_optimization_suite --parallel 4
scripts/run_optimization_campaign.sh build-opt-bench/benchmarks/aether_optimization_suite BINARIO_BASE
python3 scripts/summarize_optimization.py
```

Pruebas: `ctest -L optimization` (avec, paralelismo, cuantización, BM25 a escala y
filtros), `ctest -R cli_golden` (invariante de salida) y los perfiles ASan/UBSan
(`-DAETHER_ENABLE_SANITIZERS=ON`) y TSan (`-DAETHER_ENABLE_TSAN=ON`) con Clang de Homebrew y
`-DCMAKE_OSX_SYSROOT=macosx`. Logs: [evidencia](evidence/2026-10-28-optimization).
