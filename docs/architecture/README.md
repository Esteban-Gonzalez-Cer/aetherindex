# Arquitectura de AetherIndex

AetherIndex es un monolito modular en C++20 con dependencias
unidireccionales. VectorStore, kernels FP32/SIMD, fuerza bruta, HNSW y la
aplicación/CLI y el módulo BM25 de CP-02 están implementados; los demás módulos se incorporarán
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
│       │       ├── flat_graph.hpp
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
| `distance` | API validada y kernels FP32/SIMD | `core` |
| `ann/brute_force` | Búsqueda exacta y referencia para evaluar HNSW | `core`, `storage`, `distance` |
| `ann/hnsw` | Grafo, niveles, inserción, búsqueda y selección de vecinos | `core`, `storage`, `distance` |
| `lexical` | Tokenizer, índice invertido, estadísticas y BM25 | `core`; CP-02 recibe texto, sin requerir `storage` |
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
`query_engine.cpp`, `search_service.cpp`, `csv_loader.cpp`, los tres archivos de
`src/lexical/` de CP-02 y
`cmake/Sanitizers.cmake`; el resto sigue reservado. No se crean archivos fuente vacíos.

| Carpeta | Archivos previstos |
|---|---|
| `cmake` | `CompilerWarnings.cmake`, `Sanitizers.cmake`, `BuildOptions.cmake` |
| `src/core` | `config.cpp`, `error.cpp`; los tipos actuales se definen en headers |
| `src/storage` | `vector_store.cpp`, `document_store.cpp` |
| `src/distance` | `distance.cpp` (validación) y `scalar_distance.cpp` (kernels FP32/SIMD) |
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
- **Optimización posterior al Sprint 1:** normalización, SIMD, Top-K acotado y FlatGraph/Algoritmo 4.
- **Posteriormente:** persistencia, `mmap`, concurrencia y gRPC.

`tests/unit` verificará componentes aislados; `tests/integration`, módulos
combinados; `tests/e2e`, flujos a través de interfaces externas. `benchmarks/`
medirá rendimiento por separado de la corrección. `docs/decisions/` registrará
las decisiones arquitectónicas y `scripts/` contendrá utilidades de desarrollo.

## Estado del build

CMake construye los siguientes targets:

- `aether_core`: `VectorId`, `DocumentId`, `EmbeddingValue`, `DistanceKind`,
  `SearchResult` y `RankedResult` (headers).
- `aether_storage`: biblioteca compilada de `VectorStore`, dependiente de core.
- `aether_lexical`: tokenizer ASCII/UTF-8 conservador, postings y BM25, dependiente
  únicamente de core y biblioteca estándar. No enlaza HNSW, CLI ni RRF.
- `aether_distance`: API general y kernels FP32/SIMD de coseno y euclidiana al cuadrado, dependiente de core.
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

Los contratos numéricos, de memoria y de búsqueda actuales están en
[VectorStore y distancias](../algorithms/vector_store_and_distances.md),
[Fuerza bruta](../algorithms/brute_force.md) y [HNSW](../algorithms/hnsw.md).
Core añade asignación alineada, normalización y `PreparedQuery`; ninguno de
estos headers depende de storage, algoritmos ni aplicación.

Cosine se normaliza al insertar (norma mínima 1e-7); el stride físico del store
se rellena a múltiplos de 16 floats sin cambiar la dimensión lógica. QueryEngine
prepara la consulta una vez. Los algoritmos llaman al kernel compartido y
resuelven filas con posiciones densas públicas, sin hashes por comparación.
BruteForce usa max-heap de capacidad K. HNSW guarda Layer 0 y superiores en
arenas alineados; solo cuatro buffers globales son propietarios de la topología,
con niveles uint8 y offsets uint32. Sus vecinos son posiciones internas, y
`ids()[internal]` recupera el identificador externo al presentar resultados.

La selección HNSW predeterminada es diversificada (Algoritmo 4), con extensión
opcional implementada y relleno de descartados. `--neighbor-selection simple`
permite comparar con Algoritmo 3. Una poda puede dejar enlaces unidireccionales.
Los invariantes no garantizan conectividad ni Recall: ambos se evalúan aparte.
Una excepción después de activar un nodo exige reconstruir el índice; errores
de búsqueda no lo invalidan y el store nunca se modifica desde HNSW.

CTest registra 19 pruebas de corrección sin dependencias externas. Los dos
casos reales optativos (`AETHER_REAL_DATA_DIR`) elevan la suite a 21. La opción
`AETHER_BUILD_BENCHMARKS=ON` compila drivers separados; con
`AETHER_REGISTER_PERFORMANCE_TESTS=ON` se registran umbrales de Top-K, memoria,
Recall agrupado y, si hay DBpedia, p99, además de inspección del ensamblador
en arquitecturas/compiladores compatibles. Estas mediciones no se deducen de
los segundos totales de CTest. El perfil PMU de caché necesita Linux y permisos
para `perf`; no es una dependencia del motor ni de la suite normal.

Ver [protocolo y reproducción](../benchmarks/OPTIMIZATION_VALIDATION.md) y
[validación inicial histórica](hnsw_validation.md). BM25, RRF, persistencia,
concurrencia y gRPC permanecen pendientes.
