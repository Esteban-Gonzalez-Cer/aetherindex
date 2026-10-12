# Arquitectura de AetherIndex

AetherIndex seguirá un monolito modular en C++20 con dependencias
unidireccionales. Esta estructura prepara los módulos descritos en el diseño;
su implementación se realizará progresivamente.

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
│   ├── lexical/
│   ├── hybrid/
│   ├── persistence/
│   ├── application/
│   └── api/
│       └── grpc/
├── apps/
│   ├── cli/
│   │   └── main.cpp
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
Los headers públicos irán en `include/aether/`; las implementaciones y los
headers internos irán en `src/`. Por ejemplo:
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

`VectorStore` es propietario de los embeddings. HNSW almacenará identificadores
y topología, sin duplicar los vectores. La aplicación coordinará la relación
entre `DocumentId` y `VectorId` para que los rankings densos y léxicos puedan
fusionarse por documento.

Los algoritmos no dependerán de CLI, gRPC ni persistencia, ni imprimirán resultados
o terminarán el proceso. SIMD quedará encapsulado en `distance`; los algoritmos
accederán al almacenamiento mediante contratos, sin llamar directamente a `mmap`.
No se permitirán dependencias circulares.

## Archivos previstos por módulo

Estos nombres son una guía para las siguientes implementaciones. Ya existen
`vector_store.cpp`, `scalar_distance.cpp`, `brute_force_index.cpp`,
`query_engine.cpp` y `cmake/Sanitizers.cmake`; el resto sigue reservado. No se crean archivos fuente vacíos.

| Carpeta | Archivos previstos |
|---|---|
| `cmake` | `CompilerWarnings.cmake`, `Sanitizers.cmake`, `BuildOptions.cmake` |
| `src/core` | `config.cpp`, `error.cpp`; los tipos actuales se definen en headers |
| `src/storage` | `vector_store.cpp`, `document_store.cpp` |
| `src/distance` | `scalar_distance.cpp`; futuros kernels SIMD separados |
| `src/ann/brute_force` | `brute_force_index.cpp` |
| `src/ann/hnsw` | `hnsw_index.cpp`, `hnsw_node.cpp`, `search_layer.cpp`, `neighbor_selection.cpp` |
| `src/lexical` | `tokenizer.cpp`, `inverted_index.cpp`, `bm25.cpp` |
| `src/hybrid` | `rrf.cpp` |
| `src/persistence` | `serializer.cpp`, `mmap_storage.cpp` |
| `src/application` | `index_engine.cpp`, `query_engine.cpp` |
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
- `aether_application`: `QueryEngine`, enlazado con fuerza bruta.
- `aetherindex`: CLI, enlazada únicamente con la capa de aplicación.
- `aether_sanitizers`: opciones de instrumentación, activas solo con
  `-DAETHER_ENABLE_SANITIZERS=ON` en compiladores GCC/Clang compatibles.

Cada store tiene dimensión y métrica fijas. La métrica se comparte a través de
core, por lo que storage no depende de distance. El llamador posee el store;
ni `QueryEngine` ni el algoritmo mantienen copias de embeddings. La CLI utiliza
explícitamente euclidiana al cuadrado para conservar la salida del ejemplo.

Los headers implementados bajo `include/` son `aether/core/types.hpp`,
`aether/core/search_result.hpp`, `aether/storage/vector_store.hpp`,
`aether/distance/distance.hpp`, `aether/ann/brute_force/brute_force_index.hpp` y
`aether/application/query_engine.hpp`. Las APIs anteriores basadas en
`VectorEntry` se sustituyeron, sin adaptadores de compatibilidad.

CTest registra siete pruebas: almacenamiento, rollback ante fallas de asignación,
distancias, fuerza bruta, integración storage/distancias, búsqueda mediante
QueryEngine y salida completa de la CLI. Se pueden desactivar con
`-DBUILD_TESTING=OFF`. HNSW, recuperación léxica, RRF, persistencia, gRPC y
benchmarks siguen pendientes.

Consultar [VectorStore y distancias](../algorithms/vector_store_and_distances.md),
[Fuerza bruta](../algorithms/brute_force.md) y
[Evidencia de validación](vector_store_validation.md).

La configuración existente de formato y análisis estático se conserva. Los
artefactos de compilación se generan en `build/` y están excluidos mediante
`.gitignore`; los archivos ya versionados allí conservan su estado en Git.
