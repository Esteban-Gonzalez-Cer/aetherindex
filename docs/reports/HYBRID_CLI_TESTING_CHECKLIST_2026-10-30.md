# Checklist de testing — QueryEngine híbrido y CLI

**Fecha local:** 30 de octubre de 2026 · **Resultado:** 11/11 puntos aprobados en el alcance definido.

Se verificó la implementación actual del árbol de trabajo, sin modificar código de producto ni los tests existentes. Se añadieron únicamente este informe y artefactos de auditoría. La versión actual **sí incorpora búsqueda híbrida**.

## Versión y alcance

- Base Git: `07f83c8f6030be6848423a143eff95d6e1bc4170`; rama `feature/market-benchmarks`.
- Había cambios locales previos, incluidos cambios en CLI, algoritmos y pruebas. Los resultados corresponden a ese árbol de trabajo, **no al commit limpio por sí solo**.
- La compilación nueva usa `build-testing-hybrid-2026-10-30`; no se reutilizó un ejecutable antiguo de `build-cli`.
- Modo validado: HNSW FP32/coseno, BM25 con k1=1.2/b=0.75 y RRF clásico sin pesos, k_rrf=60. Construcción y consultas con un hilo, semilla 42; candidate_k=50 y K final=10.
- Corpus funcional: 30 documentos y 24 consultas, con embeddings existentes de 128 dimensiones del perfil `aether-hash-trigram-v1`. Son rasgos léxico-morfológicos sintéticos; no se generaron nuevos embeddings durante esta comprobación.
- Se verificó que título/texto/tema de los documentos y texto/etiquetas de las consultas coinciden con el corpus textual original. Ambos motores ejecutados aquí reciben el mismo archivo `datasets/hybrid/corpus.jsonl`.

La CLI muestra tiempos y throughput en sus logs, pero esta ejecución **no es un benchmark de latencia ni demuestra calidad semántica o rendimiento industrial**. No valida las opciones adicionales de cuantización, filtros, fusión ponderada/convexa o paralelismo.

## Resultado de cada punto

| Punto | Comprobación | Estado | Evidencia observada | Comando y salida |
|---|---|---|---|---|
| 1.1 | Ejecutar una búsqueda densa independiente | APROBADO | HNSW/coseno, Q001, K=50: 30 candidatos. Las 24 consultas se contrastaron también por separado. | [log](evidence/2026-10-30-hybrid-checklist/1.1-dense.log) |
| 1.2 | Ejecutar una búsqueda léxica independiente | APROBADO | BM25, Q001, K=50: 26 coincidencias sobre 30 documentos indexados; también se contrastaron las 24 consultas. | [log](evidence/2026-10-30-hybrid-checklist/1.2-lexical.log) |
| 1.3 | Ejecutar una búsqueda híbrida | APROBADO | HNSW + BM25 + RRF reales, Q001, K=10; ejecución adicional de las 24 consultas. | [log](evidence/2026-10-30-hybrid-checklist/1.3-hybrid.log) |
| 1.4 | Verificar que ambos motores trabajan sobre el mismo corpus | APROBADO | Mismo archivo de 30 documentos, misma identidad; 30 vectores y 30 documentos en el índice léxico. | [log](evidence/2026-10-30-hybrid-checklist/1.4-1.8-corpus.log) |
| 1.5 | Confirmar que los identificadores de los rankings son compatibles | APROBADO | Asociación explícita y prueba adicional con IDs distintos: 9010→101, 7000→102, 42→103, 11→104. | [log](evidence/2026-10-30-hybrid-checklist/1.5-explicit-bindings.log) |
| 1.6 | Verificar que RRF recibe correctamente ambos rankings | APROBADO | Listas independientes coinciden con las expuestas por QueryEngine; 720 scores contrastados con la fórmula y fracciones exactas. | [log](evidence/2026-10-30-hybrid-checklist/1.4-1.8-corpus.log) |
| 1.7 | Confirmar que el Top-K final está ordenado | APROBADO | 240 filas Top-10: score descendente, desempate por DocumentId interno y prefijo correcto de la unión completa. | [log](evidence/2026-10-30-hybrid-checklist/1.4-1.8-corpus.log) |
| 1.8 | Comprobar que los documentos recuperados existen | APROBADO | Todo ID denso, léxico y fusionado resuelve a un documento del corpus; no hay IDs inventados ni duplicados. | [log](evidence/2026-10-30-hybrid-checklist/1.4-1.8-corpus.log) |
| 1.9 | Validar errores de dimensionalidad | APROBADO | Consulta de 127D frente a 128D y documento de 127D frente al resto de 128D: ambos rechazados con código 3 y stdout vacío. | [log](evidence/2026-10-30-hybrid-checklist/1.9-query-dimension.log) |
| 1.10 | Validar consultas con argumentos faltantes | APROBADO | Sin dataset/query/valor de K: código 2. Sin texto/embedding de consulta o selector obligatorio: código 3. Sin resultados parciales. | [log](evidence/2026-10-30-hybrid-checklist/1.10-missing-dataset.log) |
| 1.11 | Confirmar que la CLI no contiene implementaciones duplicadas de HNSW, BM25 o RRF | APROBADO | Revisión de fuentes, dependencias y símbolos definidos: la CLI delega en SearchService; cada algoritmo está en su biblioteca. | [log](evidence/2026-10-30-hybrid-checklist/1.11-architecture.log) |

Cada `.log` incluye el comando exacto, directorio, salida estándar, errores, código esperado y código real. Las salidas separadas también se conservan como `.stdout.txt` y `.stderr.txt`. Un código 2/3 **esperado** en una prueba negativa constituye aprobación del control, no un fallo de la validación.

## 1.1 — Búsqueda densa independiente

```sh
build-testing-hybrid-2026-10-30/aetherindex search --mode vector --algorithm hnsw --dataset datasets/hybrid/corpus.jsonl --query datasets/hybrid/queries.jsonl --query-id Q001 --k 50 --metric cosine --seed 42 --ef-search 50
```

Se devuelve una lista HNSW por distancia ascendente. Para Q001, los primeros documentos son D001, D004 y D015. Los 30 candidatos están disponibles para comparar con la rama densa que recibe RRF.

## 1.2 — Búsqueda léxica independiente

```sh
build-testing-hybrid-2026-10-30/aetherindex search --mode lexical --dataset datasets/hybrid/corpus.jsonl --query datasets/hybrid/queries.jsonl --query-id Q001 --k 50 --bm25-k1 1.2 --bm25-b 0.75
```

Los primeros documentos BM25 para Q001 son D001, D015 y D005. Que BM25 devuelva **26 coincidencias** mientras HNSW devuelve 30 candidatos no significa que utilicen corpus diferentes: ambos indexan los 30 documentos, pero BM25 no rellena resultados sin coincidencias.

## 1.3 — Búsqueda híbrida

```sh
build-testing-hybrid-2026-10-30/aetherindex search --mode hybrid --dataset datasets/hybrid/corpus.jsonl --query datasets/hybrid/queries.jsonl --query-id Q001 --k 10 --candidate-k 50 --rrf-k 60 --metric cosine --seed 42 --ef-search 50 --bm25-k1 1.2 --bm25-b 0.75
```

La salida acredita HNSW y BM25, 30 documentos, 128D, 30 candidatos densos, 26 léxicos, unión de 30 y 10 resultados finales. Primeros resultados:

| Rank | Documento | RRF mostrado | Rango denso | Rango léxico |
|---:|---|---:|---:|---:|
| 1 | D001 | 0.032786885246 | 1 | 1 |
| 2 | D015 | 0.032002048131 | 3 | 2 |
| 3 | D030 | 0.030776515152 | 4 | 6 |

Además se ejecutaron todas las consultas, en cada modo independiente y en híbrido:

```sh
build-testing-hybrid-2026-10-30/aetherindex search --mode vector --dataset datasets/hybrid/corpus.jsonl --query datasets/hybrid/queries.jsonl --all-queries --query-threads 1 --algorithm hnsw --k 50 --seed 42 --ef-search 50
```

[Log completo de las 24 consultas](evidence/2026-10-30-hybrid-checklist/1.1-dense-all.log)

```sh
build-testing-hybrid-2026-10-30/aetherindex search --mode lexical --dataset datasets/hybrid/corpus.jsonl --query datasets/hybrid/queries.jsonl --all-queries --query-threads 1 --k 50 --bm25-k1 1.2 --bm25-b 0.75
```

[Log completo de las 24 consultas](evidence/2026-10-30-hybrid-checklist/1.2-lexical-all.log)

```sh
build-testing-hybrid-2026-10-30/aetherindex search --mode hybrid --dataset datasets/hybrid/corpus.jsonl --query datasets/hybrid/queries.jsonl --all-queries --query-threads 1 --k 10 --candidate-k 50 --rrf-k 60 --seed 42 --ef-search 50 --bm25-k1 1.2 --bm25-b 0.75
```

[Log completo de las 24 consultas](evidence/2026-10-30-hybrid-checklist/1.3-hybrid-all.log)

## 1.4 a 1.8 — Corpus, identidades, rankings de entrada, orden y existencia

Se compiló una sonda de auditoría con los headers públicos y las mismas bibliotecas que el test de integración existente. Ejecuta HNSW, BM25 y QueryEngine reales; utiliza una referencia aparte para comprobar RRF. No forma parte de la CLI ni del producto.

[Fuente de la sonda](evidence/2026-10-30-hybrid-checklist/hybrid_checklist_probe.cpp) · [comando exacto de compilación](evidence/2026-10-30-hybrid-checklist/00-probe-build.log)

```sh
<repo>/build-testing-hybrid-2026-10-30/hybrid_checklist_probe corpus datasets/hybrid/corpus.jsonl datasets/hybrid/queries.jsonl
```

La sonda comprueba, para las 24 consultas:

- Igualdad del conjunto de documentos indexados, no únicamente de sus cantidades.
- Presencia de todos los documentos en HNSW/VectorStore y BM25, y asociación única VectorId→DocumentId→ID de origen.
- Coincidencia exacta de IDs, orden y valores entre las consultas independientes y los rankings expuestos por el QueryEngine híbrido. Se revisó también la llamada de QueryEngine al módulo de fusión en el código.
- Unión correcta, sin IDs inventados/repetidos; score RRF según posiciones desde 1.
- Orden descendente y desempate por DocumentId interno en igualdad exacta; Top-10 como prefijo de la unión ordenada.
- Existencia de cada documento de las ramas y del resultado final.

La traza JSON completa queda en [1.4-1.8-corpus.stdout.txt](evidence/2026-10-30-hybrid-checklist/1.4-1.8-corpus.stdout.txt). Incluye los bindings, rankings densos/léxicos y scores completos de la unión.

### 1.5 — Caso adicional de IDs numéricamente distintos

```sh
<repo>/build-testing-hybrid-2026-10-30/hybrid_checklist_probe bindings
```

Se indexaron cuatro documentos y cuatro vectores con las asociaciones 9010→101, 7000→102, 42→103 y 11→104. La salida documental fue [102,101,104,103]. Se comprobaron también el rechazo de una asociación ausente (`logic_error`) y de dos vectores asociados al mismo documento dentro del ranking (`invalid_argument`).

### Verificación independiente de las salidas y de RRF

```sh
python3 docs/reports/evidence/2026-10-30-hybrid-checklist/verify_checklist.py
```

Se verificaron **720 filas de la unión** y **240 filas Top-10 de la CLI**. El error máximo de los scores completos frente a fracciones exactas fue **4.7124842370744e-18**, inferior a la tolerancia absoluta 1e-12.

La presentación híbrida usa 12 decimales: su error máximo de redondeo observado fue **4.98547e-13** (cota de redondeo 0.5×10^-12). Las distancias y scores BM25 se comparan por separado con sus nueve decimales. La tolerancia de prueba no altera el comparador de ordenamiento del motor.

[Resumen verificable](evidence/2026-10-30-hybrid-checklist/verification-summary.json) · [Script de comprobación](evidence/2026-10-30-hybrid-checklist/verify_checklist.py)

## 1.9 — Errores de dimensionalidad

```sh
build-testing-hybrid-2026-10-30/aetherindex search --mode hybrid --dataset datasets/hybrid/corpus.jsonl --query docs/reports/evidence/2026-10-30-hybrid-checklist/fixtures/wrong-query-dimension.jsonl --query-id Q001
```

[log](evidence/2026-10-30-hybrid-checklist/1.9-query-dimension.log)

```sh
build-testing-hybrid-2026-10-30/aetherindex search --mode hybrid --dataset docs/reports/evidence/2026-10-30-hybrid-checklist/fixtures/wrong-corpus-dimension.jsonl --query datasets/hybrid/queries.jsonl --query-id Q001
```

[log](evidence/2026-10-30-hybrid-checklist/1.9-corpus-dimension.log)

Ambos comandos deben terminar con **código 3**, diagnóstico en stderr y stdout vacío. Se usaron copias del corpus/consulta en `fixtures/`; los archivos originales permanecen intactos.

## 1.10 — Argumentos y datos de consulta faltantes

**Caso `1.10-missing-dataset` — código esperado/obtenido: 2.**

```sh
build-testing-hybrid-2026-10-30/aetherindex search --mode hybrid --query datasets/hybrid/queries.jsonl --query-id Q001
```

[log](evidence/2026-10-30-hybrid-checklist/1.10-missing-dataset.log)

**Caso `1.10-missing-query` — código esperado/obtenido: 2.**

```sh
build-testing-hybrid-2026-10-30/aetherindex search --mode hybrid --dataset datasets/hybrid/corpus.jsonl
```

[log](evidence/2026-10-30-hybrid-checklist/1.10-missing-query.log)

**Caso `1.10-missing-option-value` — código esperado/obtenido: 2.**

```sh
build-testing-hybrid-2026-10-30/aetherindex search --mode hybrid --dataset datasets/hybrid/corpus.jsonl --query datasets/hybrid/queries.jsonl --query-id Q001 --k
```

[log](evidence/2026-10-30-hybrid-checklist/1.10-missing-option-value.log)

**Caso `1.10-missing-query-text` — código esperado/obtenido: 3.**

```sh
build-testing-hybrid-2026-10-30/aetherindex search --mode hybrid --dataset datasets/hybrid/corpus.jsonl --query docs/reports/evidence/2026-10-30-hybrid-checklist/fixtures/missing-query-text.jsonl --query-id Q001
```

[log](evidence/2026-10-30-hybrid-checklist/1.10-missing-query-text.log)

**Caso `1.10-missing-query-embedding` — código esperado/obtenido: 3.**

```sh
build-testing-hybrid-2026-10-30/aetherindex search --mode hybrid --dataset datasets/hybrid/corpus.jsonl --query docs/reports/evidence/2026-10-30-hybrid-checklist/fixtures/missing-query-embedding.jsonl --query-id Q001
```

[log](evidence/2026-10-30-hybrid-checklist/1.10-missing-query-embedding.log)

**Caso `1.10-missing-selector` — código esperado/obtenido: 3.**

```sh
build-testing-hybrid-2026-10-30/aetherindex search --mode hybrid --dataset datasets/hybrid/corpus.jsonl --query datasets/hybrid/queries.jsonl
```

[log](evidence/2026-10-30-hybrid-checklist/1.10-missing-selector.log)

Los tres primeros son errores de argumentos (2). Los tres últimos son errores de datos/selección (3). Todos fallaron de forma controlada, sin resultados parciales.

## 1.11 — Responsabilidad de la CLI

```sh
python3 docs/reports/evidence/2026-10-30-hybrid-checklist/audit_cli.py --build-dir build-testing-hybrid-2026-10-30
```

Se revisaron los tres `.cpp` y dos headers de `apps/cli`, sus dependencias y los símbolos definidos por las bibliotecas:

- CLI: argumentos, validación de opciones y formato de la respuesta; delega en SearchService.
- QueryEngine/application: ejecución y coordinación de los recuperadores y la fusión.
- `libaether_hnsw.a`: contiene las implementaciones HNSW.
- `libaether_lexical.a`: contiene BM25.
- `libaether_hybrid.a`: contiene RRF.

La CLI no define métodos de HnswIndex, Bm25Index o RrfFusion. Las fórmulas que aparecen dentro de strings de ayuda son documentación, no código que calcula rankings. Esta comprobación combina revisión de fuentes y evidencia de símbolos/dependencias; no se presenta como un detector universal de clones semánticos.

[Auditoría](evidence/2026-10-30-hybrid-checklist/1.11-architecture.log) · [Lugares de llamada](evidence/2026-10-30-hybrid-checklist/1.11-call-sites.log) · [Dependencias y comandos de construcción](evidence/2026-10-30-hybrid-checklist/1.11-build-dependencies.log)

## Compilación y CTest

Configuración y compilación Debug usadas:

```sh
cmake -S . -B build-testing-hybrid-2026-10-30 -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_OSX_SYSROOT=macosx -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DBUILD_TESTING=ON -DAETHER_BUILD_BENCHMARKS=OFF -DAETHER_BUILD_MARKET_BENCH=OFF
```
[log](evidence/2026-10-30-hybrid-checklist/00-configure.log)

```sh
cmake --build build-testing-hybrid-2026-10-30 --target aetherindex aether_hybrid_retrieval_tests aether_cli_parser_tests --parallel 4
```
[log](evidence/2026-10-30-hybrid-checklist/00-build.log)

Se ejecutaron las tres pruebas seleccionadas para este checklist, no toda la suite del proyecto:

```sh
ctest --test-dir build-testing-hybrid-2026-10-30 -R '^(integration.hybrid_retrieval|e2e.cli_hybrid|unit.cli_parser)$' --output-on-failure -V -j 1
```

**Resultado: 3/3 aprobadas:** `integration.hybrid_retrieval`, `e2e.cli_hybrid` y `unit.cli_parser`. El test e2e ejecutó 24 consultas híbridas por la CLI y casos positivos/negativos adicionales.

[Log Debug](evidence/2026-10-30-hybrid-checklist/00-ctest.log)

La misma selección se compiló y ejecutó con AddressSanitizer y UndefinedBehaviorSanitizer, usando Homebrew Clang:

```sh
cmake -S . -B build-testing-hybrid-2026-10-30-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ -DCMAKE_OSX_SYSROOT=macosx -DBUILD_TESTING=ON -DAETHER_ENABLE_SANITIZERS=ON -DAETHER_BUILD_BENCHMARKS=OFF -DAETHER_BUILD_MARKET_BENCH=OFF
```

```sh
cmake --build build-testing-hybrid-2026-10-30-asan --target aetherindex aether_hybrid_retrieval_tests aether_cli_parser_tests --parallel 4
```

```sh
env ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ctest --test-dir build-testing-hybrid-2026-10-30-asan -R '^(integration.hybrid_retrieval|e2e.cli_hybrid|unit.cli_parser)$' --output-on-failure -V -j 1
```

**Resultado: 3/3 aprobadas, sin errores detectados por ASan/UBSan en esas rutas.** [Log ASan/UBSan](evidence/2026-10-30-hybrid-checklist/00-asan-ctest.log)

La sonda detallada y las comparaciones de filas de este informe corresponden al build Debug; ASan/UBSan repite los escenarios de los tres tests existentes, incluidos errores de dimensión e identidad.

## Archivos y límites de la evidencia

- [Comandos, códigos y salidas — índice JSON](evidence/2026-10-30-hybrid-checklist/commands.json).
- [Versión y SHA-256 de 119 fuentes/entradas](evidence/2026-10-30-hybrid-checklist/snapshot-before.json). Se verificó que no cambiaron durante la validación.
- [Entorno, configuraciones y hashes de los binarios](evidence/2026-10-30-hybrid-checklist/environment.json).
- [Entradas híbridas exactas utilizadas](evidence/2026-10-30-hybrid-checklist/inputs/hybrid/corpus.jsonl) y [consultas](evidence/2026-10-30-hybrid-checklist/inputs/hybrid/queries.jsonl).
- Las pruebas cubren esta versión local en macOS ARM64 y el corpus funcional indicado. No certifican Windows/Linux, parámetros no ejercitados, datasets grandes ni una mejora de relevancia.
- El resultado permite marcar estos once controles técnicos como aprobados en este alcance; no sustituye una revisión de equipo ni una aprobación de PR.
