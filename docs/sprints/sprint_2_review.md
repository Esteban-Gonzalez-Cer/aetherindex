# Revisión del Sprint 2 y Definition of Done

**Fecha:** 28 de octubre de 2026 · **Rama:** `develop` · **Base:** `4203fdf` con cambios
sin commit (incluye el trabajo previo de BM25 en la CLI y esta revisión).
**Entorno verificado:** macOS 26.5 ARM64, AppleClang 17.0.0 (Debug/Release) y
Homebrew Clang 23.1.3 (ASan/UBSan). Datos y logs:
[evaluation/reports](../../evaluation/reports) y
[evidencia de builds](../reports/evidence/2026-10-28-sprint2).

## 1. Veredicto

**Sprint terminado. El MVP híbrido todavía no está aceptado (falta KR1.7).**
Todos los criterios automatizables pasan: build limpio, la suite completa en
Debug, Release y sanitizers, KR1.3, KR1.4, KR1.5 y las regresiones del Sprint 1.
Quedan pendientes los requisitos que solo el equipo puede cerrar: las
reproducciones 4/4 de KR1.7, la revisión y el merge del PR, y el commit final
evaluado. Cuando se cierren y se vuelva a ejecutar el colector de evidencia,
el resultado pasa a **"MVP híbrido aceptado, OKR 3 parcialmente completo"**:
KR3.1, KR3.2 y KR3.8 tienen evidencia, pero se reportan como una compuerta separada.

## 2. Hallazgos de la revisión y correcciones

| # | Severidad | Hallazgo antes de esta revisión | Corrección |
|---|---|---|---|
| H1 | Bloqueante | **No existía búsqueda híbrida.** QueryEngine, SearchService y la CLI solo tenían los modos vectorial y léxico; RRF estaba aislado. CP-04/KR1.5 no se podían cumplir. | `QueryEngine::search(HnswIndex, VectorDocumentBinding, Bm25Index, RrfFusion, HybridQuery)`; `SearchMode::Hybrid` en el servicio; `--mode hybrid`, `--candidate-k` y `--rrf-k` en la CLI. Ver [CLI híbrida](../cli_hybrid.md). |
| H2 | Bloqueante | **Se rompía la invariante `DocumentId_dense = DocumentId_sparse`.** El JSONL vectorial insertaba cada embedding con `VectorId` = número de fila, mientras BM25 usaba el DocumentId del documento (p. ej., `101` o un ID asignado a `D002`). Fusionar ambos rankings habría combinado documentos distintos. | Cada embedding JSONL se guarda con `VectorId = DocumentId`, y se agregó una asociación explícita, `DocumentRecord::vector_id` → `VectorDocumentBinding`. El motor rechaza VectorIds sin asociación y las asociaciones no inyectivas. La tabla vectorial JSONL ahora muestra `Document ID`. Tests: `integration.hybrid_retrieval`. |
| H3 | Alta | **Las etiquetas de relevancia no se leían.** `QueryRecord::relevant_doc_ids` quedaba siempre vacío, así que no se podía evaluar con el corpus CP-01. | Se lee `relevant_document_ids` (strings/uint32, sin duplicados). La recuperación sigue sin usar las etiquetas. |
| H4 | Alta | No había una evaluación formal de KR1.3 sobre CP-01: CP-02 solo medía su fixture sintético y la dejaba para CP-05. | `acceptance.kr1_3_bm25`: Hit Rate@10, Hit@1 y MRR, reporte CSV, y falla con menos del 90 %. |
| H5 | Alta | No había evaluación de KR1.5 ni 20 consultas híbridas. | `acceptance.kr1_5_hybrid` recalcula cada fusión de forma independiente. `e2e.cli_hybrid` ejecuta las 24 consultas por el binario. |
| H6 | Alta | El corpus CP-01 no tiene embeddings (`PENDIENTE_GENERAR_CON_MODELO`), así que no había datos versionados para ejecutar el modo híbrido. | `scripts/generate_hashing_embeddings.py` genera fuera del motor `datasets/hybrid/` con embeddings *hashing* reproducibles. **No son semánticos**; el equipo debe reemplazarlos por embeddings de un modelo. |
| H7 | Alta | No había un driver para KR3.1/3.2/3.8. El benchmark existente fija efSearch=50 y repite 20 consultas. | `benchmarks/hnsw_parameter_sweep.cpp`: barrido de M, efConstruction y efSearch con 1,000 consultas independientes, BruteForce como verdad de referencia, warm-up, p50/p95/p99 y latencias crudas. |
| H8 | Alta (reproducibilidad) | **Había artefactos de compilación versionados.** `build/CMakeCache.txt` contiene rutas absolutas de este Mac: en otro clon, `cmake -S . -B build` (el comando del README) falla con *"CMakeCache.txt directory is different"*. Además estaban versionados `*.obj` y `hnsw_test.exe`. | Se retiraron del índice con `git rm --cached` (los archivos locales se conservan) y se añadieron `*.obj`, `*.o` y `*.exe` a `.gitignore`. |
| H9 | Media | El README solo documentaba Windows en detalle. Faltaban las plataformas soportadas, las limitaciones y el uso de los tres modos, y varios documentos decían que CP-04 estaba pendiente. | Inicio rápido del Sprint 2 en el README, [CLI híbrida](../cli_hybrid.md), [dataset híbrido](../../datasets/hybrid/README.md) y [evaluación](../../evaluation/README.md); se actualizaron las afirmaciones obsoletas. |
| H10 | Media | No había un paquete de evidencia legible por máquina para los KR. | `evaluation/reports/*.csv/json` y `scripts/collect_sprint2_evidence.py`, que registra el commit, si el árbol tiene cambios, los compiladores y el resultado de cada test por perfil. |

No se cambió ningún algoritmo de HNSW, BM25 ni RRF; la revisión los encontró
correctos y sus pruebas siguen pasando sin cambios.

## 3. Construction & Testing (CP)

| Tarjeta | Estado | Evidencia |
|---|---|---|
| CP-01 Corpus y consultas | **Parcial.** Hay 30 documentos, 24 consultas etiquetadas e IDs compartidos. Los embeddings son el *hashing* de esta revisión, no de un modelo. El corpus es pequeño. | `datasets/`, `datasets/hybrid/` |
| CP-02 BM25 | **Verificado.** | `ctest -L cp02` (8 tests), KR1.3 |
| CP-03 RRF | **Verificado.** | `ctest -L kr1_4` (22 casos) + propiedades + contrato |
| CP-04 QueryEngine + CLI | **Verificado** con esta revisión. | `integration.hybrid_retrieval`, `e2e.cli_hybrid`, `unit.cli_parser` |
| CP-05 Validación híbrida | **Verificado** (KR1.3, KR1.4 y KR1.5). | `ctest -L sprint2`, `evaluation/reports/` |
| CP-06 Benchmarks HNSW | **Verificado** como compuerta separada. | `evaluation/reports/kr3_dbpedia/` |
| CP-07 Aceptación y documentación | **Pendiente:** reproducción 4/4, PR revisado y commit final. | `kr1_7_acceptance.csv` |

## 4. OKR 1 — resultados

| KR | Métrica | Umbral | Resultado | Estado |
|---|---|---|---|---|
| KR1.3 BM25 | Hit Rate@10 (algún documento relevante etiquetado en el Top-10) | ≥ 90 %, ≥ 20 consultas | **23/24 = 95.83 %**; Hit@1 = 83.3 %; MRR@10 = 0.883 | PASS ⚠ |
| KR1.4 RRF | Casos obligatorios | 100 % | **22/22** + propiedades + contrato; referencia `[102,101,104,103]` con `RRF(102)=1/62+1/61` y `RRF(101)=1/61+1/63` (error ≤ 1e-12) | PASS |
| KR1.5 Híbrido | Consultas end-to-end válidas | 20/20 | **24/24** por SearchService con recálculo independiente (máximo error de score 0) y **24/24** por el ejecutable | PASS |
| KR1.7 Reproducibilidad | 4/4 integrantes + 100 % de tests obligatorios | 4/4 y 100 % | Tests automáticos **100 %** en los 3 perfiles; integrantes **0/4 registrados** | PENDIENTE |

⚠ **KR1.3 es evidencia débil.** Con 30 documentos, el Top-10 cubre un tercio
del corpus. Hit@1 y MRR son señales más informativas. La consulta Q024 falla
(D029/D028 no aparecen en el Top-10) y se reporta tal cual, sin modificar
etiquetas ni consultas. Recomendación: ampliar CP-01 a varios cientos de
documentos con distractores, o juzgar consultas sobre DBpedia (10,000
artículos), antes de presentar KR1.3 como evidencia de relevancia.

**Alcance de KR1.5:** verifica la integración y el contrato de salida, no un
beneficio de relevancia. Con los embeddings *hashing*, el Hit Rate@10 híbrido
(informativo) es de 24/24.

### Regresión del Sprint 1

| KR previo | Resultado en esta revisión |
|---|---|
| KR1.1 | 1,000 embeddings insertados y recuperados, 0 alterados (`integration.sprint1_okr1`) |
| KR1.2 | 100/100 consultas Top-10 válidas, 0 violaciones del grafo |
| KR1.6 | Recall@10 medio = 0.987 ≥ 0.85 |

## 5. OKR 3 — compuerta de rendimiento separada

Driver: `aether_hnsw_parameter_sweep`, Release (NDEBUG), AppleClang 17, un hilo.
Datos: DBpedia de 1,024 dimensiones, coseno. Las filas con `fila % 10 == 9` se
reservan como 1,000 consultas independientes y se indexan las 9,000 restantes.
La verdad de referencia es BruteForce exacto. Hay 100 búsquedas de warm-up por
configuración y el tiempo cubre solo `HnswIndex::search`. Son 16 configuraciones
y 16,000 búsquedas medidas, con latencias crudas en `raw_latencies.csv`.

| KR | Requisito | Resultado | Estado |
|---|---|---|---|
| KR3.1 | ≥ 3 valores de M y ≥ 3 de efSearch; efConstruction configurable | M ∈ {8,16,32} × efSearch ∈ {10,50,100}; efConstruction ∈ {50,100,200} (Recall@10 0.9845 → 0.9886 → 0.9907 con M=16, efS=50) | Evidencia completa |
| KR3.2 | ≥ 5 efSearch × 1,000 consultas | efSearch 10/25/50/100/200: Recall@10 0.882 / 0.962 / 0.989 / 0.997 / 0.999; p50 0.058 / 0.127 / 0.186 / 0.350 / 0.605 ms | Evidencia completa |
| KR3.8 | p50, p95 y p99 con ≥ 1,000 consultas | Los tres percentiles por configuración en `kr3_8_latency_percentiles.csv` | Evidencia completa |

Advertencias: hay una sola ejecución en una sola máquina. Algunos p99 muestran
*jitter*: por ejemplo, M=16/efS=100 tiene un p99 de 1.21 ms en KR3.1 y de 1.01 ms
en KR3.2, cuando efS=200 da 0.93 ms. El estado de carga del sistema no estaba
controlado. Los campos `dataset_seed` y `query_seed` de `workload.json` solo
aplican al modo sintético. El modo sintético (`--quick`/por defecto) saturó el
Recall en una prueba previa, por eso la evidencia oficial usa DBpedia. Estos
resultados **no** acreditan KR3.3–KR3.7 (Recall ≥ 0.95 como meta, SIMD ni
escalas de 1M). CP-06 figuraba como *Should Have*: el equipo debe decidir en la
Sprint Review si KR3 era un compromiso del Sprint.

## 6. Definition of Done — lista formal (13)

| # | Requisito | Estado |
|---|---|---|
| 1 | Build limpio con C++20 y CMake | ✅ Directorios nuevos en Debug, Release y sanitizers; 0 warnings |
| 2 | VectorStore, BruteForce y HNSW pasan la regresión del Sprint 1 | ✅ |
| 3 | BM25: tokenización, índice, estadísticas, scoring y Top-K | ✅ |
| 4 | RRF correcto, deduplicado y determinista | ✅ |
| 5 | QueryEngine integra HNSW, BM25 y RRF con identidad consistente | ✅ (H1, H2) |
| 6 | La CLI soporta los modos denso, léxico e híbrido con entradas documentadas | ✅ |
| 7 | KR1.3 ≥ 90 % | ✅ 95.83 % (⚠ corpus pequeño) |
| 8 | KR1.4 100 % | ✅ 22/22 |
| 9 | KR1.5 20/20 | ✅ 24/24 |
| 10 | KR1.7 4/4 y 100 % de tests | ⏳ Integrantes pendientes |
| 11 | Tests unitarios, de integración, de regresión y de errores, sin defectos bloqueantes | ✅ Suite completa; ASan/UBSan sin hallazgos |
| 12 | README, contratos, datasets, pruebas y limitaciones documentados | ✅ (Linux/Windows sin ejecución registrada) |
| 13 | Métricas, logs, configuración y commit evaluado registrados | ⏳ Registrados sobre `4203fdf` + cambios sin commit; falta el commit final |

**11/13 verificados; 2 pendientes de acción del equipo.**

Cobertura: el repositorio no define un umbral de cobertura (OKR 2), por lo que no se
evaluó. Un 100 % de tests aprobados no equivale a un 100 % de cobertura.

## 7. Pendientes para cerrar la aceptación

1. Hacer commit de los cambios en una rama, abrir el PR y obtener la revisión de otro integrante.
2. Sobre el commit final: ejecutar el procedimiento de [evaluation/README.md](../../evaluation/README.md)
   y `scripts/collect_sprint2_evidence.py`, y verificar que `working_tree_dirty=false`.
3. Que cada integrante (4/4) ejecute el inicio rápido del README en su máquina y
   registre su sistema operativo, compilador, commit y resultado en `kr1_7_acceptance.csv`.
   Idealmente, al menos una ejecución en Windows/MSVC y una en Linux.
4. Recomendado: generar embeddings de un modelo para CP-01 y ampliar el corpus
   (ver ⚠ de KR1.3).
5. Decidir si KR3.1, KR3.2 y KR3.8 eran un compromiso del Sprint o *Should Have*.
