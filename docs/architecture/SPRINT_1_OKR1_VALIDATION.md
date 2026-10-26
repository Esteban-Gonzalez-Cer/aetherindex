# Sprint 1 — Validación de indicadores del OKR 1

> Evidencia histórica del baseline previo a las optimizaciones del 19-10-2026.
> Describe el código y las mediciones de esa versión; las cifras y salidas
> guardadas no se recalcularon. Para el contrato y resultados actuales, consultar
> [la validación de optimizaciones](../benchmarks/OPTIMIZATION_VALIDATION.md).

Fecha: **14 de octubre de 2026**. Protocolo de aceptación: **v1**.

**Los criterios técnicos de KR1.1, KR1.2 y KR1.6 se cumplen en el escenario de
aceptación del Sprint 1.** La recuperación de embeddings fue del 100 %, HNSW
respondió correctamente las 100 consultas y el Mean Recall@10 fue **0.990**,
superior al mínimo de 0.85. La Sprint Review humana continúa pendiente.

## Alcance y versión evaluada

Esta evaluación ejecuta el plan proporcionado para **KR1.1, KR1.2 y KR1.6**.
KR1.3, KR1.4, KR1.5 y KR1.7 permanecen fuera de esta aceptación, asignados al
Sprint 2. La CLI permite demostrar el recorrido completo; su funcionamiento
no se contabiliza como aceptación de KR1.7. Tampoco se acredita BM25, RRF,
recuperación híbrida ni el OKR 1 completo.

La implementación de producción evaluada corresponde al commit
`031adae19951d95ede349eaeaf295dd8d4a611d2`. El checkout estaba limpio al inicio.
Se añadieron localmente dos pruebas y un recolector de evidencias, sin modificar
`src/`, `include/` ni `apps/`. Estos instrumentos y este informe **todavía no
forman parte de un nuevo commit**. Cada ejecución conserva en
`run_metadata.json` el commit base, el estado del checkout y los SHA-256 de las
fuentes, incluidos los instrumentos locales. El recolector comprueba que las
fuentes y HEAD no cambien durante la ejecución.

## Matriz final de indicadores

Los CSV de **Release** son la evidencia principal; Debug y ASan/UBSan repiten
el mismo protocolo con archivos y rankings equivalentes.

| KR / indicador | Meta | Resultado medido | Estado |
|---|---|---|---|
| KR1.1A: embeddings almacenados | ≥1,000 | 1,000; 0 inserciones fallidas | Cumplido |
| KR1.1B: recuperación por ID | 100 % | 1,000/1,000; dimensión 128 | Cumplido |
| KR1.1C: embeddings alterados | 0 | 0; 128,000 componentes FP32 comparados exactamente | Cumplido |
| KR1.2A: nodos indexados | ≥1,000 | 1,000 | Cumplido |
| KR1.2B: consultas Top-10 válidas | 100/100 | 100/100, cada una con 10 IDs únicos y distancias ordenadas y finitas | Cumplido |
| KR1.2C: violaciones estructurales | 0 | 0 en los tres puntos de control y en el recorrido independiente | Cumplido |
| KR1.2D: errores de ejecución | 0 | 0 en las 100 consultas | Cumplido |
| KR1.6: Mean Recall@10 | ≥0.85, Q≥100 | **0.990**, Q=100; **990/1,000** coincidencias | Cumplido |

Porcentaje de cumplimiento del alcance planificado: **3/3 × 100 = 100 %**.
Este porcentaje corresponde exclusivamente a los tres KR evaluados, no al
OKR completo ni al cierre administrativo del Sprint.

## Datos y configuración congelada

| Parámetro | Valor oficial |
|---|---|
| Dataset común a los tres KR | 1,000 embeddings × 128 componentes |
| Consultas | 100 vectores distintos, ausentes del dataset |
| Tipo | IEEE-754 FP32 (`float` de 32 bits) |
| Métrica | Cosine: `1 − cosine_similarity` |
| Normalización previa | Ninguna; todos los vectores son finitos y no nulos |
| IDs | `7 × fila + 10`, únicos, con saltos; filas 0…999 |
| Semilla del dataset | 42 |
| Semilla de consultas de aceptación | **20261014** |
| Semilla del grafo | 42 |
| M / M0 | 16 / 32 |
| efConstruction / efSearch | 100 / 50 |
| K / límite de nivel | 10 / 32 |
| Ajustes después de observar la aceptación | Ninguno |

La semilla 424242 se conserva en la prueba de desarrollo existente
`integration.hnsw_recall`. Para esta aceptación se eligió una semilla nueva
**antes de ejecutar las mediciones**, manteniendo los parámetros anteriores.
Las consultas de aceptación no se utilizaron para ajustar la configuración.

El generador reproducible es `hnsw_test::embeddings` en
[`tests/hnsw_test_helpers.hpp`](../../tests/hnsw_test_helpers.hpp), identificado
por SHA-256 en los metadatos. Usa `std::mt19937(seed)` y, para cada componente,
`float(int(random() % 2000001) - 1000000) / 1000000.0f`; corrige un eventual
vector completamente nulo asignando 1 a su primer componente. No depende de
las distribuciones aleatorias particulares de una biblioteca estándar.

La prueba conserva en cada build el dataset y 100 consultas como CSV con
`max_digits10`, suficiente para recuperar exactamente FP32. Los 101 SHA-256
quedan guardados en `run_metadata.json`. Los CSV grandes se regeneran mediante
el código; las evidencias versionables conservan sus hashes. Este dataset
sintético mide corrección algorítmica y recall frente a la referencia exacta.
La evaluación real de DBpedia documentada anteriormente es complementaria y
no se volvió a ejecutar ni se contabilizó en estos resultados.

## Qué se verificó

### KR1.1 — Almacenamiento e integridad

Se registraron individualmente las 1,000 inserciones en `insertions.csv`. Se
recuperaron todos los IDs, verificando tamaño, dimensión, orden de inserción y
los **128,000 componentes originales por igualdad exacta FP32**. La comparación
se repitió después de las consultas y de cada rechazo para detectar cambios
indebidos en los vectores ya almacenados.

`invalid_inputs.csv` contiene ocho comprobaciones: ID duplicado, dimensión
corta, dimensión larga, NaN, +infinito, −infinito, vector cero en Cosine e ID
inexistente. Los siete intentos de inserción fueron rechazados con el estado
esperado. El ID inexistente devolvió un `optional` vacío. El tamaño permaneció
en 1,000 y los vectores existentes permanecieron intactos.

Las pruebas unitarias anteriores también verifican alias de un embedding del
mismo store, vistas contiguas de solo lectura, el rango completo de IDs y
rollback ante fallos de asignación. Se reutilizan; no se duplicó su
implementación en la aceptación.

### KR1.2 — HNSW funcional e incremental

Se utilizó **una misma instancia** de VectorStore y HnswIndex: se insertaron los
primeros 500 embeddings y nodos, se realizaron cinco búsquedas, y después se
añadieron otros 500. Se conservaron los 500 IDs y niveles iniciales; no se
reconstruyó el índice. Las conexiones de nodos existentes pueden cambiar al
añadir vecinos, como corresponde a la inserción incremental.

Se ejecutó `validate_graph()` y un recorrido independiente de todos los nodos
y capas después de 500 inserciones, después de 1,000 y después de las consultas.
Se comprobaron presencia en capa 0, referencias existentes, pertenencia a la
capa, ausencia de bucles y vecinos duplicados, límites M/M0, embeddings
disponibles, conteo, entry point y nivel máximo. **Cero violaciones**; el nivel
máximo observado fue **2**, y el entry point pertenece a ese nivel.

Las 100 consultas definitivas pasaron por `QueryEngine`: cada resultado tiene
10 IDs existentes y únicos, distancias finitas en orden ascendente, y valores
exactamente iguales a `compute_distance` sobre el embedding recuperado. Los
rankings y distancias están en `query_results.csv`.

Los casos límite se cubren con las pruebas existentes: índice vacío, singleton,
K=0 con error controlado en HNSW, K>N, efSearch<K, inserciones duplicadas o IDs
ausentes, dimensión incompatible, vectores no finitos, configuración inválida,
grafos pequeños y comportamiento ante errores. El diagnóstico estructural
por sí solo no establece recall; KR1.6 lo mide por separado.

### KR1.6 — Recall frente a fuerza bruta

Para cada consulta, `QueryEngine` ejecutó BruteForce sobre el mismo VectorStore
y recuperó el ranking completo de 1,000 vectores. En el evaluador se ordena por
**(distancia FP32, ID)** y se conservan los primeros 10. Así se define un
desempate determinista sin modificar el contrato público de BruteForce.
Se observaron **cero empates entre las posiciones 10 y 11**.

La referencia calcula 100 × 1,000 = **100,000 distancias**, cifra derivada de la
traversía exhaustiva del algoritmo. No es un contador instrumentado de todas
las llamadas del proceso: las comprobaciones recalculan distancias adicionales.
La corrección de BruteForce y Distance se verifica también con casos pequeños
de resultado conocido. Su tolerancia es estrictamente
`abs(double(resultado) − double(esperado)) < 1e-5`; el recall compara conjuntos
de IDs y no aplica tolerancia de punto flotante.

Se consultó HNSW con efSearch=50, sin barrido de parámetros en este protocolo.
Cada fila del CSV conserva el conjunto exacto, el aproximado, las coincidencias,
el recall individual, el promedio, la configuración y el estado global.

| Consultas | Coincidencias por consulta | Recall individual |
|---|---|---|
| 90 | 10/10 | 1.0 |
| 10 | 9/10 | 0.9 |

`Mean Recall@10 = (90 × 1.0 + 10 × 0.9) / 100 = 0.990`.
El criterio de aceptación se aplica al **promedio**; no exige ≥0.85 a cada
consulta individual. El menor recall observado fue 0.9.

### VI.1 — Demostración integrada

`e2e.sprint1_okr1` usa los CSV del protocolo y la consulta 0 en la CLI para ambos
algoritmos. Compara los diez IDs y las diez distancias impresas contra el ranking
registrado por el evaluador; valida metadatos, bordes y separadores por fila,
IDs enteros, nueve decimales y tiempo de consulta con su alcance. Ambos
comandos terminaron con código 0 y stderr vacío. Los dos Top-10 de esta consulta
coinciden; la conclusión de recall utiliza las 100 consultas, no solo este caso.

Salidas conservadas: [BruteForce](validation/okr1/release/cli_brute-force.txt)
y [HNSW](validation/okr1/release/cli_hnsw.txt).

## Builds y pruebas ejecutados

Entorno: **macOS 26.5.2 ARM64**, CMake **4.4.4**. Debug y Release usan
AppleClang **17.0.0.17000603**; ASan/UBSan usa Homebrew Clang **23.1.3** en otro
directorio. Todas las compilaciones parten de directorios nuevos para esta
validación. `AETHER_REAL_DATA_DIR` permanece desactivado.

| Configuración | CTest completo | Mean Recall@10 | Tiempo total CTest | Evidencia |
|---|---|---|---|---|
| Debug | 18/18 | 0.990 | 26.22 s | [Log](validation/okr1/debug/ctest.log) |
| Release | 18/18 | 0.990 | 6.76 s | [Log](validation/okr1/release/ctest.log) |
| Debug + ASan/UBSan | 18/18; sin diagnósticos ASan/UBSan | 0.990 | 87.52 s | [Log](validation/okr1/sanitizers/ctest.log) |

Se construyó además Release con `BUILD_TESTING=OFF`. Las dos pruebas nuevas
solo existen dentro de `tests/`; las bibliotecas mantienen sus dependencias.
Los 16 tests previos se ejecutaron completos, incluidos los casos conocidos de
distancia, fallos de asignación, dos métricas y la CLI.

Los sanitizadores se ejecutaron con `ASAN_OPTIONS=halt_on_error=1` y
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`. Los logs completos no
contienen diagnósticos de acceso inválido o comportamiento indefinido. Esta
evidencia corresponde a los caminos ejecutados, no demuestra ausencia de
errores en todos los usos posibles. El build sin pruebas también ejecutó
`--help` con código 0 y stderr vacío; su configuración y verificación están en
[`validation/okr1/no-tests/verification.json`](validation/okr1/no-tests/verification.json).

Los tres builds produjeron los mismos 101 hashes de fixtures y los mismos
rankings, coincidencias y distancias FP32. La comprobación final del paquete
está en [`evidence_integrity.json`](validation/okr1/evidence_integrity.json).

Las suites se ejecutaron con cuatro procesos CTest y hubo compilaciones de
otras configuraciones en paralelo. Estos tiempos y los tiempos por consulta
son **observaciones de esta validación, no benchmarks ni una garantía de QPS
o latencia en producción**. El tiempo de consulta de la CLI excluye CSV,
construcción del grafo y presentación. No se establece un objetivo de
rendimiento en estos tres KR.

## Entregables y trazabilidad

Los tres reportes solicitados están en:

- [kr1_1_vectorstore_report.csv](validation/okr1/release/kr1_1_vectorstore_report.csv)
- [kr1_2_hnsw_functional_report.csv](validation/okr1/release/kr1_2_hnsw_functional_report.csv)
- [kr1_6_hnsw_recall_report.csv](validation/okr1/release/kr1_6_hnsw_recall_report.csv)

En cada subdirectorio `validation/okr1/{debug,release,sanitizers}` se conservan
los reportes, `protocol.json`, registro de inserciones, entradas inválidas,
diagnósticos del grafo, rankings con distancias, demostración CLI, logs de
build/CTest, `LastTest.log`, `CMakeCache.txt` y `run_metadata.json` con hashes.
Los metadatos incluyen el commit evaluado y las modificaciones locales;
este documento no sustituye esas evidencias individuales.

| Requisito del plan | Prueba reutilizada o añadida |
|---|---|
| Almacenamiento, vistas, validaciones y dataset | `unit.vector_store`, `integration.sprint1_okr1` |
| Integridad ante fallos de asignación | `unit.vector_store_allocation` |
| Distancias conocidas, tolerancia e inválidos | `unit.distance`, `unit.brute_force`, `integration.storage_distance` |
| Nodos, niveles, inserción y casos límite HNSW | `unit.hnsw_node`, `unit.hnsw_insert`, `unit.hnsw_search`, `unit.hnsw_graph` |
| 500 + 500, mismo dataset y 100 consultas oficiales | `integration.sprint1_okr1` (nueva) |
| Recall de desarrollo en ambas métricas | `integration.hnsw_recall` |
| Capa de aplicación y selección de algoritmo | `integration.dense_retrieval`, `integration.search_service` |
| Argumentos y CLI | `unit.cli_parser`, `e2e.cli_search`, `e2e.cli_mvp`, `e2e.cli_hnsw` |
| Demostración CLI contra el ranking oficial | `e2e.sprint1_okr1` (nueva) |

## Reproducción

Desde la raíz del repositorio, para un generador de configuración única:

```sh
cmake -S . -B build-okr1-debug -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
python3 scripts/validate_sprint1_okr1.py --build-dir build-okr1-debug --config Debug --output-dir build-okr1-evidence/debug

cmake -S . -B build-okr1-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
python3 scripts/validate_sprint1_okr1.py --build-dir build-okr1-release --config Release --output-dir build-okr1-evidence/release

cmake -S . -B build-okr1-sanitizers -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DAETHER_ENABLE_SANITIZERS=ON -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++
ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 python3 scripts/validate_sprint1_okr1.py --build-dir build-okr1-sanitizers --config Debug --output-dir build-okr1-evidence/sanitizers
```

El recolector requiere Python 3.10+ y únicamente su biblioteca estándar. Primero
compila, luego ejecuta toda la suite CTest, comprueba los reportes y recalcula
las intersecciones de IDs en Python antes de recopilar evidencia. Si un comando
falla, devuelve error y no acredita una ejecución nueva. Para cada nueva
evaluación conviene utilizar un directorio de evidencia nuevo y revisar la
fecha y el resultado de sus logs, evitando confundir reportes anteriores con
una ejecución fallida.

Para ejecutar directamente las pruebas, sin el recolector ni Python:

```sh
cmake --build build-okr1-release --parallel 4
ctest --test-dir build-okr1-release -C Release --output-on-failure
```

Para repetir únicamente la aceptación y su demostración:

```sh
ctest --test-dir build-okr1-release -C Release -R sprint1_okr1 --output-on-failure -V
```

Los CSV regenerados quedan en `build-okr1-release/tests/okr1-Release/`:

```sh
./build-okr1-release/aetherindex search --dataset build-okr1-release/tests/okr1-Release/fixtures/vectors.csv --query build-okr1-release/tests/okr1-Release/fixtures/queries/query_0.csv --algorithm brute-force --metric cosine --k 10
./build-okr1-release/aetherindex search --dataset build-okr1-release/tests/okr1-Release/fixtures/vectors.csv --query build-okr1-release/tests/okr1-Release/fixtures/queries/query_0.csv --algorithm hnsw --metric cosine --k 10 --m 16 --m0 32 --ef-construction 100 --ef-search 50 --seed 42
```

Con Visual Studio en Windows, configurar sin `CMAKE_BUILD_TYPE`, compilar con
`--config Release` y ejecutar CTest con `-C Release`. El recolector también usa
`--config`; el ejecutable CLI estará en `build-okr1-release/Release/aetherindex.exe`.
La ejecución registrada en este informe se realizó en macOS; no acredita una
compilación o ejecución real en Windows.

## Cierre y revisión

- [x] Dataset y consultas reproducibles; configuración y semillas congeladas.
- [x] KR1.1: 1,000 embeddings recuperados sin alteración.
- [x] KR1.2: construcción, inserción incremental, grafo y 100 consultas válidas.
- [x] KR1.6: Mean Recall@10 ≥0.85 y detalle por consulta.
- [x] Demostración integrada y evidencias para revisión.
- [x] Suite completa en Debug, Release y ASan/UBSan; build sin tests.
- [ ] Revisión de resultados por el equipo durante la **Sprint Review**.

La evidencia técnica está preparada para la revisión. El cierre formal del
Sprint y las aprobaciones del equipo no se pueden deducir de las pruebas
automatizadas. Los KR diferidos conservan su alcance en Sprint 2.
