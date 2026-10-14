# Auditoría e integración inicial de HNSW

Fechas locales: **2026-10-12–13**, America/Mexico_City. Referencia de requisitos:
tarjeta proporcionada por el usuario, «Sprint 1 — Implementación inicial de HNSW».

## Resultado de la revisión inicial

La versión recibida en el commit `7e7dac0` contenía el recorrido jerárquico,
dos colas de búsqueda, visitados e inserción incremental, pero **no satisfacía
la tarjeta ni estaba conectada al build del proyecto**. La revisión se
realizó antes de habilitar su consumo desde la CLI.

| Hallazgo inicial | Evidencia en la versión recibida | Corrección necesaria |
|---|---|---|
| HNSW no participa en CMake/CTest | No existe target HNSW; `tests/hnsw_test.cpp` solo imprime IDs y no está registrado | Biblioteca compilada y pruebas con aserciones registradas |
| Headers públicos dentro de `src/` | `HNSWIndex.h`, `HNSWNode.h`; el test incluye `../src/...` | API en `include/aether/ann/hnsw/`, implementación en `src/ann/hnsw/` |
| Matemática duplicada en float | `HNSWIndex.cpp:47–88` implementa ambas distancias | Utilizar exclusivamente `compute_distance`, con las validaciones y precisión existentes |
| Configuración incompleta | Tres parámetros posicionales, sin M0 ni semilla configurable; ef inválidos se corrigen silenciosamente | `HnswConfig`, rechazo explícito y semilla reproducible |
| Límite de Layer 0 incorrecto | Poda usa M en todas las capas | M0 en capa base, M en capas superiores |
| API de resultados incompatible | Búsqueda retorna `vector<VectorId>` | `vector<SearchResult>` con distancia |
| No hay evidencia estructural ni Recall | Faltan validador, pruebas 1,000 × 128, consultas independientes y comparación exacta | T1–T12 y Recall@10 promedio ≥ 0.85 en el protocolo definido |
| Fallo de inserción sin política | El nodo se publica antes de operaciones que pueden lanzar excepciones | Definir rechazo recuperable frente a fallo que exige reconstrucción |

Los cálculos locales de distancia pueden devolver NaN con embeddings coseno
finitos extremos o muy pequeños, y devolver infinito en euclidiana al cuadrado
en lugar de propagar `overflow_error`. Son incumplimientos del contrato del
módulo Distance, aunque el ejemplo pequeño imprima IDs aparentemente válidos.

La tarjeta permite selección de vecinos por cercanía como simplificación MVP
y enlaces asimétricos después de poda. **No se exige reciprocidad estricta** ni
se considera por sí sola un defecto. La separación en varios `.cpp` es sugerida;
lo obligatorio es conservar las responsabilidades y dependencias del módulo.

## Contrato y validación previstos

El propietario de embeddings sigue siendo `VectorStore`; HNSW conserva IDs,
niveles, vecinos y una referencia al store. No debe almacenar spans entre
operaciones ni copiar permanentemente embeddings. El store debe vivir más que
el índice, y lecturas y escrituras no se solapan.

La validación previa a CLI debe cubrir C1–C10 y T1–T12: configuración, primer nodo,
1000 niveles reproducibles, conexiones válidas, inserción 100+100, ranking de
referencia, K y entradas inválidas, distancias extremas, grafo válido y al menos
100 consultas independientes sobre 1000 × 128. Se registran Recall@10 y latencia
media para efSearch 10/30/50/100 sobre el mismo grafo. La aceptación exige promedio
≥ 0.85 con efSearch 50 en el protocolo del MVP, sin exigir monotonía por consulta.

El flujo de aplicación conservará:

```text
CLI → SearchService → QueryEngine → BruteForce
                                └→ HnswIndex → VectorStore / Distance
```

La construcción del grafo debe ocurrir antes de iniciar el temporizador de
consulta. La CLI conservará IDs enteros, distancias con nueve decimales y la tabla
con separadores. Cada ejecución carga y construye en memoria; no hay persistencia.

## Correcciones y verificación previa a CLI

Se sustituyeron los archivos importados por la API pública `HnswIndex`,
`HnswNode` y `HnswConfig` bajo `include/aether/ann/hnsw/`, con implementación en
`src/ann/hnsw/hnsw_index.cpp`. CMake compila `aether_hnsw`, dependiente de core,
storage y distance. Se centralizaron parámetros, semilla, límites por capa,
distancias compartidas y resultados `SearchResult`. No hay wrappers antiguos.
El [contrato completo](../algorithms/hnsw.md) describe propiedad, selección,
pruning, diagnóstico y política de reconstrucción tras un fallo de inserción.

La base anterior pasó **10/10** pruebas, pero ninguna construía HNSW. Después de
integrar el núcleo y **antes de habilitarlo en la CLI**, pasaron **15/15** en
Debug, con Recall@10 promedio **0.977 coseno / 0.972 euclidiana al cuadrado**.
Evidencias: [log previo a CLI](validation/hnsw_core_before_cli.log) y
[CSV por consulta](validation/hnsw_core_before_cli_recall.csv).

## Correspondencia con la tarjeta

| Requisitos | Evidencia |
|---|---|
| C1/C2, T1/T2/T10 | API pública, nodo sin embeddings, configuración validada, vacío/primer nodo y rango completo de IDs |
| C3, T3 | 1000 niveles/topologías reproducibles; 954 nodos de nivel 0, 44 de nivel 1 y 2 de nivel 2; grafo exclusivo de Layer 0 |
| C4/C5/C9, T6/T7 | Dos heaps, visitados, descenso jerárquico, resultados comunes y ordenados, K y consultas inválidas; referencias coseno 0/0.2/1/2 y euclidiana al cuadrado 25 |
| C6/C7/C10, T4 | Selección cercana comprobada con fixture controlado, poda M/M0, inspección independiente de todas las conexiones y validador de invariantes |
| C8, T5/T10 | Inserción 100+100 y reallocation del store, rechazos recuperables; fallo posterior a mutación exige reconstrucción |
| T8/T9/T11 | 1000 × 128, 100 consultas independientes por métrica, cuatro amplitudes sobre el mismo grafo, Recall y tiempos registrados |
| T12 | Suite completa con ASan/UBSan, sin diagnósticos en las rutas evaluadas |
| Integraciones 1–4 | Store/Distance públicos, referencia exacta, QueryEngine, servicio y pruebas de CLI |

## Ejecución final

Directorios nuevos, con `-Wall -Wextra -Wpedantic`:

| Configuración | Resultado | Evidencia |
|---|---|---|
| Debug, AppleClang 17 | Build correcto; **16/16 CTest**, salida 0 | [Debug](validation/hnsw_debug.log) |
| Release, AppleClang 17, datos reales habilitados | Build correcto; **18/18 CTest**, salida 0 | [Release](validation/hnsw_release.log) |
| Debug + ASan/UBSan, Homebrew Clang 23.1.3 | Build correcto; **16/16 CTest**, salida 0; sin diagnósticos de sanitizers | [Sanitizers](validation/hnsw_sanitizers.log) |
| Release, BUILD_TESTING=OFF | Build correcto, cero tests, CLI HNSW con K=100 devuelve los cuatro vecinos 2D correctos | Ejecución manual |
| Formato y diff | clang-format y git diff --check correctos | Fuentes C++ nuevas/modificadas |

Tiempos CTest: Debug **35.93 s**, Release **129.17 s**, sanitizers **116.04 s**.
Hubo ejecuciones concurrentes. Incluyen preparación y comprobaciones; no son
una comparación de rendimiento ni garantías de latencia de producción.
Directorios: `/private/tmp/aetherindex-hnsw-{debug,release,sanitizers,no-tests}-20261012`.

## Recall obligatorio

Dataset 1000 × 128, semilla de datos 42, consultas independientes 424242 y grafo
42; IDs `7*i+10`, en ese orden. Cien consultas por métrica, K=10, M=16, M0=32,
efConstruction=100. Componentes generados a partir de enteros `mt19937` para
evitar diferencias de distribuciones estándar. Ambos algoritmos usan el mismo
store y consulta. Se verifica que el corte exacto Top-10 no tenga empates.

| efSearch | Mean Recall@10 coseno | Mean Recall@10 euclidiana al cuadrado |
|---:|---:|---:|
| 10 | 0.676 | 0.733 |
| 30 | 0.924 | 0.928 |
| **50 (predeterminado)** | **0.977** | **0.972** |
| 100 | 0.998 | 0.993 |

Se registran **800** búsquedas medidas: cuatro amplitudes × 100 consultas × dos
métricas. El umbral **≥ 0.85** se cumple con los parámetros predeterminados.
No se exige monotonía por consulta ni un límite de tiempo, ni se presenta ese
umbral como garantía universal. [CSV Release](validation/hnsw_recall_release.csv).

## Caso real adicional

DBpedia: **10,000 × 1,024**, coseno, 20 artículos de consulta excluidos del
corpus. Un único grafo M=16/M0=32, efConstruction=100, semilla 42, 10,000 nodos,
nivel máximo 4, válido antes y después.

| efSearch | Mean Recall@10 | Tiempo medio observado de consulta (ms) |
|---:|---:|---:|
| 50 | 0.985 | 1.088425 |
| 100 | 0.995 | 1.760913 |

Tiempo medio exacto observado: **10.108467 ms**. Construcción: **24735.628416 ms**;
carga del fixture de prueba: **2857.476750 ms**. Son observaciones locales
individuales, sin protocolo completo de benchmark. Se verificaron **400 vecinos
HNSW** por ID, distancia, orden y unicidad. [CSV real](validation/hnsw_real_recall.csv).
La prueba exacta NumPy existente también pasó sus 40 consultas y 4000 vecinos.

La ejecución final del comando real en `build-cli-release/` devolvió los mismos
10 IDs y distancias impresas de la referencia exacta guardada para `query_00`:
[salida completa](validation/hnsw_cli_real_sample.txt), salida 0. Midió
**23282.933792 ms** de construcción y **1.579292 ms** de consulta, separados de
la carga CSV. Los ejecutables y targets de tests locales Debug/Release se
actualizaron para poder repetir los comandos.

## Reproducir

Desde la raíz del proyecto:

```sh
cmake -S . -B build-cli -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build-cli --parallel 4
ctest --test-dir build-cli --output-on-failure

cmake -S . -B build-cli-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DAETHER_REAL_DATA_DIR="$PWD/datasets/real/dbpedia"
cmake --build build-cli-release --parallel 4
ctest --test-dir build-cli-release --output-on-failure
./build-cli-release/aetherindex search --dataset datasets/real/dbpedia/vectors.csv --query datasets/real/dbpedia/queries/query_00.csv --algorithm hnsw --k 10 --ef-search 100 --seed 42

cmake -S . -B build-hnsw-sanitizers -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DAETHER_ENABLE_SANITIZERS=ON -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++
cmake --build build-hnsw-sanitizers --parallel 4
ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ctest --test-dir build-hnsw-sanitizers --output-on-failure

cmake -S . -B build-hnsw-no-tests -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build-hnsw-no-tests --parallel 4
ctest --test-dir build-hnsw-no-tests -N
./build-hnsw-no-tests/aetherindex search --dataset datasets/examples/vectors.csv --query datasets/examples/query.csv --algorithm hnsw --k 100
```

Los datos reales ya están disponibles localmente. Para otro equipo ver
[la guía del caso real](../real_world_validation.md). NumPy es necesario solo
para el test exacto opcional; el motor y los 16 normales usan la biblioteca
estándar. Homebrew Clang evita el problema de inicialización ASan de AppleClang
documentado en tarjetas anteriores.

## Límites y aceptación externa

La selección por cercanía y los enlaces asimétricos tras poda son decisiones
permitidas por la tarjeta. Un grafo estructuralmente válido con parámetros
dispersos puede tener peor recall o devolver menos de K; no hay respaldo exacto
oculto. No se agregaron SIMD, concurrencia, persistencia, actualizaciones,
eliminación, BM25, RRF ni gRPC.

Los artefactos Windows `.obj`/`.exe` recibidos en la raíz son previos a esta
integración y no participan en CMake. Se conservan; los builds actuales generan
archivos en sus directorios de compilación.

Los criterios técnicos automatizados evaluados se cumplen. Revisión por otro
integrante humano, aprobación/integración de PR y ejecución por otro integrante
o Windows siguen pendientes. No se creó un PR ni se marca la tarjeta Done
automáticamente por pasar los tests.
