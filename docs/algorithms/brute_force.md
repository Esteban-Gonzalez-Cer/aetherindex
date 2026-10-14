# Fuerza bruta: integración y contrato

La búsqueda procede de `aetherindex-feature-fuerza-bruta` y ahora consume
`VectorStore` mediante sus interfaces públicas. La copia original en Downloads
permanece intacta. Se conserva el namespace `aetherindex` y el algoritmo de
recorrido completo, ordenación y truncamiento top-k.

## API actual

```cpp
std::vector<SearchResult> bruteForceSearch(
    const VectorStore& database,
    std::span<const EmbeddingValue> query,
    std::size_t k);
```

`QueryEngine::search` expone los mismos argumentos como entrada de la aplicación
y ofrece una sobrecarga separada para un índice HNSW ya construido.
La CLI llama a `SearchService`, que carga los archivos y consulta `QueryEngine`;
la búsqueda no contiene entrada/salida ni datos
de ejemplo. El algoritmo recorre `ids()`, obtiene cada vista con `find()` y llama
`compute_distance()` con `distance_kind()` del store. No copia embeddings.

Los identificadores son `VectorId` (`uint32_t`), incluyendo cero y su máximo.
`SearchResult::distance` es una distancia: valores menores se ordenan primero.
La dimensión y la métrica permanecen fijas durante la vida del store.

## Archivos

| Responsabilidad | Ubicación |
|---|---|
| API de búsqueda | `include/aether/ann/brute_force/brute_force_index.hpp` |
| Implementación | `src/ann/brute_force/brute_force_index.cpp` |
| Almacenamiento propietario | `include/aether/storage/vector_store.hpp` |
| Tipos e identificadores | `include/aether/core/types.hpp`, `search_result.hpp` |
| API de distancias | `include/aether/distance/distance.hpp` |
| Kernels escalares | `src/distance/scalar_distance.cpp` |
| Coordinación de carga y búsqueda | `include/aether/application/search_service.hpp` |
| Interfaz de terminal | `apps/cli/main.cpp`, `cli_parser.cpp`, `cli_runner.cpp` |

Las APIs anteriores basadas en `VectorEntry`, IDs `int` y
`squaredEuclideanDistance(vector, vector)` se reemplazaron, sin wrappers.

## Comportamiento

- Devuelve como máximo `k` resultados, ordenados por distancia ascendente.
- Si `k == 0` o el store está vacío, devuelve una lista vacía antes de validar
  la consulta, incluso si esta tiene dimensiones incorrectas.
- En una búsqueda efectiva, dimensiones incompatibles, consultas vacías o no
  finitas y consultas de norma cero con coseno producen `std::invalid_argument`.
- Los resultados euclidianos al cuadrado que exceden `float` producen
  `std::overflow_error`, sin devolver un ranking parcial.
- El orden relativo de los empates permanece sin especificar.
- La búsqueda no modifica el almacenamiento. No admite escrituras simultáneas.

Para `N` embeddings de dimensión `D`, el coste es `O(ND + N log N)` y el espacio
adicional es `O(N)`. No se incorpora una optimización top-k.

El store y la CLI utilizan coseno por defecto. La CLI admite también
`--metric squared-euclidean` y carga archivos CSV mediante `search`. El ejemplo
original de euclidiana al cuadrado, con IDs `1`, `2`, `4` y distancias
`4.92`, `5.14`, `9.77`, sigue validándose mediante `QueryEngine`.
La CLI ejecuta carga y búsqueda en memoria en un único comando, sin persistencia.
Ver [Uso de la CLI](../cli.md).

La implementación [HNSW](hnsw.md) comparte el store, la métrica y `SearchResult`,
y utiliza fuerza bruta como referencia exacta de Recall@10 en las pruebas. HNSW
recorre el grafo y puede omitir vecinos; nunca sustituye una consulta aproximada
por este recorrido exhaustivo. Su desempate por ID es explícito, mientras que
fuerza bruta mantiene el orden de empates sin especificar. El protocolo de
Recall evita empates ambiguos en el corte Top-10.

Ver [VectorStore y distancias](vector_store_and_distances.md) para compilación,
sanitizers, vida útil de las vistas y contrato numérico, y la
[validación HNSW](../architecture/hnsw_validation.md) para evidencias.
`DocumentId`, BM25, RRF, persistencia y un framework de benchmarks permanecen pendientes.
