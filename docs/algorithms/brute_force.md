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

`QueryEngine::search` expone los mismos argumentos como entrada de la aplicación.
La CLI depende de `QueryEngine`; la búsqueda no contiene entrada/salida ni datos
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
| Demostración | `apps/cli/main.cpp` |

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

El store utiliza coseno por defecto. El ejemplo de CLI selecciona explícitamente
`SquaredEuclidean`, con IDs `1`, `2`, `4` y distancias `4.92`, `5.14`, `9.77`.
Su salida se comprueba completa mediante CTest. La CLI sigue siendo una
demostración en memoria, sin comandos de indexación ni lectura de archivos.

Ver [VectorStore y distancias](vector_store_and_distances.md) para compilación,
sanitizers, vida útil de las vistas y contrato numérico. `DocumentId`, HNSW,
BM25, RRF, persistencia y benchmarking permanecen pendientes.
