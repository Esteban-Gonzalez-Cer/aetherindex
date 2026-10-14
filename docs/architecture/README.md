# Arquitectura de AetherIndex

AetherIndex es un monolito modular en C++20 con dependencias
unidireccionales. VectorStore, distancias escalares, fuerza bruta, HNSW y la
aplicación/CLI están implementados; los demás módulos se incorporarán
progresivamente.

## Estructura actual

```text
aetherindex/
├── CMakeLists.txt
├── README.md
├── .gitignore
├── .clang-format
├── .clang-tidy
├── cmake/
├── include/
│   └── aether/
│       ├── core/
│       ├── storage/
│       ├── distance/
│       ├── ann/
│       │   ├── brute_force/
│       │   └── hnsw/
│       │       ├── hnsw_config.hpp
│       │       ├── hnsw_node.hpp
│       │       └── hnsw_index.hpp
│       ├── lexical/
│       ├── hybrid/
│       ├── persistence/
│       └── application/
├── src/
│   ├── core/
│   ├── storage/
│   ├── distance/
│   ├── ann/
│   │   ├── brute_force/
│   │   └── hnsw/
│   │       └── hnsw_index.cpp
│   ├── lexical/
│   ├── hybrid/
│   ├── persistence/
│   ├── application/
│   └── api/
│       └── grpc/
├── apps/
│   ├── cli/
│   │   ├── main.cpp
│   │   ├── cli_parser.hpp / .cpp
│   │   └── cli_runner.hpp / .cpp
│   └── server/
├── tests/
│   ├── unit/
│   ├── integration/
│   └── e2e/
├── benchmarks/
│   ├── distance/
│   ├── hnsw/
│   ├── lexical/
│   └── system/
├── datasets/
│   └── README.md
├── scripts/
└── docs/
    ├── architecture/
    │   └── README.md
    ├── algorithms/
    ├── benchmarks/
    └── decisions/
```

Las carpetas vacías contienen `.gitkeep` para que Git pueda conservarlas.
Eliminar ese marcador cuando la carpeta contenga archivos propios del módulo.
Los headers públicos están en `include/aether/`; las implementaciones y los
headers internos están en `src/`. Por ejemplo:
`include/aether/storage/vector_store.hpp` y `src/storage/vector_store.cpp`.

## Responsabilidades y dependencias previstas

| Módulo | Responsabilidad | Puede depender de |
|---|---|---|
| `core` | Identificadores, tipos, configuración y errores comunes | Biblioteca estándar |
| `storage` | `VectorStore` para embeddings; `DocumentStore` para texto y metadata | `core` |
| `distance` | Métricas escalares y futuros kernels SIMD | `core` |
| `ann/brute_force` | Búsqueda exacta y referencia para evaluar HNSW | `core`, `storage`, `distance` |
| `ann/hnsw` | Grafo, niveles, inserción, búsqueda y selección de vecinos | `core`, `storage`, `distance` |
| `lexical` | Tokenizer, índice invertido y BM25 | `core`, `storage` |
| `hybrid` | Fusión de rankings mediante RRF | `core` |
| `persistence` | Serialización y futuros adaptadores de almacenamiento con `mmap` | Estructuras que serializa, sin dependencias inversas |
| `application` | Orquestación mediante `IndexEngine` y `QueryEngine` | Recuperación densa, léxica, híbrida y stores |
| `apps/cli` | Argumentos, validación de entrada y presentación de resultados | `application` |
| `src/api/grpc` | Mapeo de solicitudes y respuestas gRPC | `application` |
| `apps/server` | Arranque y composición del servidor futuro | Adaptador gRPC y `application` |

`VectorStore` es propietario de los embeddings. HNSW almacena identificadores
y topología, sin duplicar los vectores ni conservar spans. El llamador mantiene
el store vivo durante toda la vida del índice. Se permiten nuevas inserciones
en el store entre operaciones del índice; las lecturas y escrituras nunca se
solapan. La aplicación coordinará la relación
entre `DocumentId` y `VectorId` para que los rankings densos y léxicos puedan
fusionarse por documento.

Los algoritmos no dependerán de CLI, gRPC ni persistencia, ni imprimirán resultados
o terminarán el proceso. SIMD quedará encapsulado en `distance`; los algoritmos
accederán al almacenamiento mediante contratos, sin llamar directamente a `mmap`.
No se permitirán dependencias circulares.

## Archivos previstos por módulo

Estos nombres son una guía para las siguientes implementaciones. Ya existen
`vector_store.cpp`, `scalar_distance.cpp`, `brute_force_index.cpp`, `hnsw_index.cpp`,
`query_engine.cpp`, `search_service.cpp`, `csv_loader.cpp` y
`cmake/Sanitizers.cmake`; el resto sigue reservado. No se crean archivos fuente vacíos.

| Carpeta | Archivos previstos |
|---|---|
| `cmake` | `CompilerWarnings.cmake`, `Sanitizers.cmake`, `BuildOptions.cmake` |
| `src/core` | `config.cpp`, `error.cpp`; los tipos actuales se definen en headers |
| `src/storage` | `vector_store.cpp`, `document_store.cpp` |
| `src/distance` | `scalar_distance.cpp`; futuros kernels SIMD separados |
| `src/ann/brute_force` | `brute_force_index.cpp` |
| `src/ann/hnsw` | `hnsw_index.cpp`; búsqueda por capa y selección separadas como helpers en ese archivo |
| `src/lexical` | `tokenizer.cpp`, `inverted_index.cpp`, `bm25.cpp` |
| `src/hybrid` | `rrf.cpp` |
| `src/persistence` | `serializer.cpp`, `mmap_storage.cpp` |
| `src/application` | `query_engine.cpp`, `search_service.cpp`, lector interno `csv_loader.hpp/.cpp`; futuro `index_engine.cpp` |
| `apps/server` | `main.cpp` cuando se implemente gRPC |

## Evolución

- **Sprint 1:** tipos comunes, `VectorStore`, distancia escalar, fuerza bruta,
  HNSW y aplicación/CLI básica.
- **Sprint 2:** `DocumentStore`, tokenizer, índice invertido, BM25, RRF y
  consultas híbridas.
- **Posteriormente:** SIMD, persistencia, `mmap`, concurrencia y gRPC.

`tests/unit` verificará componentes aislados; `tests/integration`, módulos
combinados; `tests/e2e`, flujos a través de interfaces externas. `benchmarks/`
medirá rendimiento por separado de la corrección. `docs/decisions/` registrará
las decisiones arquitectónicas y `scripts/` contendrá utilidades de desarrollo.

## Estado del build

CMake construye los siguientes targets:

- `aether_core`: `VectorId`, `EmbeddingValue`, `DistanceKind` y `SearchResult` (headers).
- `aether_storage`: biblioteca compilada de `VectorStore`, dependiente de core.
- `aether_distance`: biblioteca escalar de coseno y euclidiana al cuadrado, dependiente de core.
- `aether_brute_force`: búsqueda exacta, enlazada con storage y distance.
- `aether_hnsw`: grafo e índice aproximado; depende de core/storage y enlaza distance de forma privada.
- `aether_application`: `QueryEngine`, `SearchService` y lector CSV interno; enlaza fuerza bruta y HNSW de forma privada.
- `aether_cli`: interpretación de argumentos y presentación, dependiente de aplicación.
- `aetherindex`: punto de entrada, enlazado con `aether_cli`.
- `aether_sanitizers`: opciones de instrumentación, activas solo con
  `-DAETHER_ENABLE_SANITIZERS=ON` en compiladores GCC/Clang compatibles.

Cada store tiene dimensión y métrica fijas. La métrica se comparte a través de
core, por lo que storage no depende de distance. El llamador posee el store;
ni `QueryEngine` ni los algoritmos mantienen copias de embeddings. `QueryEngine`
admite un store para fuerza bruta o un `HnswIndex` ya preparado para búsqueda
aproximada. `SearchService` valida la solicitud y configuración antes de abrir
archivos, crea el store local, procesa una fila CSV por vez y valida la consulta.
Para HNSW construye el grafo recorriendo los IDs del store antes de llamar a
`QueryEngine`. El grafo y el store se destruyen al terminar la operación.

La respuesta contiene metadata, vecinos, `query_time` e `index_build_time`.
`query_time` mide únicamente la llamada a `QueryEngine` con `steady_clock`;
excluye carga CSV, construcción del grafo y presentación. `index_build_time`
mide la construcción HNSW por separado y vale cero con fuerza bruta. Cada
invocación de CLI reconstruye el índice. La CLI utiliza coseno por defecto y
admite euclidiana al cuadrado mediante `--metric squared-euclidean`.

HNSW se selecciona con `--algorithm hnsw`; sus parámetros de CLI son `--m`,
`--m0`, `--ef-construction`, `--ef-search` y `--seed`. Los defaults son
16, 32, 100, 50 y 42. La capa de presentación muestra IDs enteros, distancias
con nueve decimales y una tabla con bordes. La selección aproximada no tiene
fallback a fuerza bruta.

Los headers implementados bajo `include/` son `aether/core/types.hpp`,
`aether/core/search_result.hpp`, `aether/storage/vector_store.hpp`,
`aether/distance/distance.hpp`, `aether/ann/brute_force/brute_force_index.hpp`,
`aether/ann/hnsw/hnsw_config.hpp`, `aether/ann/hnsw/hnsw_node.hpp`,
`aether/ann/hnsw/hnsw_index.hpp`, `aether/application/query_engine.hpp` y
`aether/application/search_service.hpp`.
Los headers de CLI permanecen en `apps/cli/` y el lector CSV es interno a
aplicación. Las APIs anteriores basadas en
`VectorEntry` se sustituyeron, sin adaptadores de compatibilidad.

CTest registra 16 pruebas: almacenamiento, rollback ante fallas de asignación,
distancias, fuerza bruta, integración storage/distancias, búsqueda mediante
QueryEngine, parser CLI, servicio de búsqueda, flujo CLI y diez consultas MVP
Top-10 sobre 1,000 × 128, además de cuatro pruebas unitarias HNSW, Recall y flujo
CLI HNSW. Se pueden desactivar con `-DBUILD_TESTING=OFF`. Recuperación léxica,
RRF, persistencia, gRPC y un framework de benchmarks siguen pendientes. La CLI muestra
ayuda al ejecutarse sin argumentos; la demostración anterior fija en código se
sustituyó por archivos CSV y el comando `search`. Las pruebas de `QueryEngine`
conservan el ranking del ejemplo original.

Los escenarios optativos `e2e.cli_real_dataset` e
`integration.hnsw_real_dataset`, habilitados mediante `AETHER_REAL_DATA_DIR`,
elevan la suite a 18 pruebas. Utilizan 10,000 artículos DBpedia × 1,024 dimensiones y
20 consultas separadas. Comparan Top-100 de ambas métricas contra un cálculo
exhaustivo independiente en NumPy float64 para fuerza bruta y Recall@10 HNSW
contra fuerza bruta, con amplitudes 50 y 100 sobre el mismo grafo. Los scripts de preparación/evaluación
permanecen fuera del motor; no se agregan dependencias al binario C++ ni a los
16 tests predeterminados. Ver [el caso real](../real_world_validation.md).

La validación estructural de HNSW comprueba IDs, embeddings, niveles, capas,
grados, enlaces, punto de entrada y cantidad de inserciones. No demuestra por
sí misma conectividad ni Recall. La selección inicial conserva los vecinos más
cercanos, con desempate por ID; una poda puede dejar enlaces de un solo sentido.
Las consultas devuelven como máximo `min(k,N)` nodos alcanzables. Los errores de
precondición conservan el grafo. Una excepción después de comenzar una mutación
de inserción marca el índice como inutilizable y exige reconstruirlo; no se
afirma rollback transaccional. Los errores durante búsquedas no lo invalidan.

Consultar [VectorStore y distancias](../algorithms/vector_store_and_distances.md),
[Fuerza bruta](../algorithms/brute_force.md) y
[HNSW](../algorithms/hnsw.md),
[Evidencia de VectorStore](vector_store_validation.md),
[Uso de CLI](../cli.md), [Validación HNSW](hnsw_validation.md) y
[Validación histórica de CLI](cli_validation.md).

La configuración existente de formato y análisis estático se conserva. Los
artefactos de compilación se generan en `build/` y están excluidos mediante
`.gitignore`; los archivos ya versionados allí conservan su estado en Git.
