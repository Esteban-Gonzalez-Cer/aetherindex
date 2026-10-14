# HNSW: índice jerárquico en memoria

La implementación inicial de HNSW construye un grafo multicapa y devuelve
vecinos aproximados mediante navegación jerárquica. Utiliza los embeddings de
`VectorStore` y exclusivamente `compute_distance` del módulo distance. No
incluye SIMD, concurrencia, persistencia, actualizaciones ni eliminación.

## Archivos y dependencias

| Responsabilidad | Archivo |
|---|---|
| Configuración y validación compartida | `include/aether/ann/hnsw/hnsw_config.hpp` |
| Nodo: ID, nivel máximo y vecinos por capa | `include/aether/ann/hnsw/hnsw_node.hpp` |
| API del índice y diagnóstico del grafo | `include/aether/ann/hnsw/hnsw_index.hpp` |
| Niveles, búsqueda por capa, selección e inserción | `src/ann/hnsw/hnsw_index.cpp` |
| Pruebas unitarias | `tests/unit/hnsw_{node,insert,search,graph}_test.cpp` |
| Evaluación reproducible de Recall@10 | `tests/integration/hnsw_recall_test.cpp` |
| Evaluación opcional con embeddings reales | `tests/integration/hnsw_real_dataset_test.cpp` |

CMake compila `aether_hnsw`, con dependencias públicas `aether_core` y
`aether_storage`, y dependencia privada `aether_distance`. El módulo no depende
de fuerza bruta, aplicación, CLI, BM25, RRF ni persistencia. Las pruebas enlazan
fuerza bruta como referencia exacta. `aether_application` enlaza de forma
privada ambos algoritmos; la CLI consume la capa de aplicación.

## API y propiedad

```cpp
HnswIndex(const VectorStore& store, HnswConfig config = {});
void insert(VectorId id);
std::vector<SearchResult> search(std::span<const EmbeddingValue> query,
                                 std::size_t k) const;
std::vector<SearchResult> search(std::span<const EmbeddingValue> query,
                                 std::size_t k, std::size_t ef_search) const;
```

El índice no se copia ni se mueve. El llamador conserva la propiedad de
`VectorStore`, que debe vivir más tiempo que el índice. Cada nodo contiene
únicamente `id`, `max_level` y listas de IDs vecinos; no duplica embeddings ni
conserva vistas `std::span`. Los cálculos obtienen una vista nueva mediante
`store.find(id)` y utilizan `store.distance_kind()`, evitando métricas
contradictorias entre construcción y búsqueda.

Se pueden agregar embeddings al store entre operaciones del índice y después
indexar sus IDs. No se permite solapar lecturas y escrituras; deben descartarse
las vistas del store antes de insertar en este. Insertar en HNSW no modifica el
store.

```cpp
#include <array>
#include <stdexcept>

#include "aether/ann/hnsw/hnsw_index.hpp"
#include "aether/application/query_engine.hpp"

using namespace aetherindex;

int main() {
    VectorStore store(3, DistanceKind::Cosine);
    const std::array<float, 3> first{1.0f, 0.0f, 0.0f};
    const std::array<float, 3> second{0.8f, 0.6f, 0.0f};
    if (store.insert(10, first) != InsertStatus::Success ||
        store.insert(40, second) != InsertStatus::Success) {
        throw std::runtime_error("No se pudieron cargar los embeddings.");
    }

    HnswConfig config;
    HnswIndex index(store, config);
    for (VectorId id : store.ids()) {
        index.insert(id);
    }
    const QueryEngine engine;
    const auto neighbors = engine.search(index, first, 2);
    const auto wider_search = index.search(first, 2, 100);  // Mismo grafo.
    return neighbors.size() == 2 && wider_search.size() == 2 ? 0 : 1;
}
```

`QueryEngine` ofrece sobrecargas para un `VectorStore` (búsqueda exacta) y un
`HnswIndex` ya construido (búsqueda aproximada). Las respuestas usan la misma
estructura `SearchResult { id, distance }`.

## Configuración

| Campo | Predeterminado | Contrato |
|---|---:|---|
| `m` | 16 | Al menos 2; límite de vecinos en capas superiores |
| `m_max_0` | 32 | Al menos `m`; límite de vecinos de Layer 0 |
| `ef_construction` | 100 | Al menos `m`; amplitud de búsqueda durante inserción |
| `ef_search` | 50 | Positivo; amplitud de búsqueda predeterminada |
| `random_seed` | 42 | Semilla `std::uint64_t` para construir el grafo |
| `max_level` | 32 | Entre 0 y 32; cero permite un grafo exclusivo de Layer 0 |

`validate_hnsw_config(config)` y el constructor rechazan parámetros inválidos
mediante `std::invalid_argument`; no los corrigen silenciosamente.

La generación usa `std::mt19937_64` y un mapeo explícito de enteros a `(0,1)`:
52 bits aleatorios se convierten en un entero de 1 a `2^52`, dividido entre
`2^52 + 1`. El nivel es `floor(-log(U)/log(m))`, limitado por `max_level`.
La misma semilla, configuración, orden de inserción y entorno producen la
misma construcción. Un nodo de nivel máximo L posee listas para todas las
capas de 0 a L.

## Construcción y búsqueda

El primer nodo se convierte en punto de entrada. Cada inserción posterior
desciende mediante búsquedas greedy de amplitud 1 por las capas superiores
que no comparte con el nuevo nodo. En las capas compartidas ejecuta
`search_layer` con `ef_construction`, selecciona vecinos y crea enlaces en
ambos sentidos. Si supera el nivel máximo existente, actualiza el punto de
entrada y el nivel superior del índice.

`search_layer` mantiene una cola de candidatos con menor distancia primero,
una cola de resultados con el peor candidato primero y un conjunto de IDs
visitados. Conserva como máximo `ef` resultados. Se detiene cuando el mejor
candidato pendiente es más lejano que el peor resultado conservado y el
conjunto ya tiene `ef` elementos.

La política de selección se implementa en un helper separado: candidatos más
cercanos primero, con el ID como desempate. La poda puntúa cada candidato una
vez y conserva como máximo `m_max_0` en la capa base o `m` en las demás. No se
implementa la heurística avanzada de diversidad espacial. Tras una poda pueden
permanecer enlaces de un solo sentido; la reciprocidad estricta no forma
parte del contrato del MVP.

La consulta desciende desde el punto de entrada por las capas superiores y
realiza la búsqueda amplia en Layer 0, con
`ef_effective = min(size(), max(ef_search, k))`. La sobrecarga con `ef_search`
permite comparar amplitudes sobre el mismo grafo, sin reconstruirlo ni cambiar
la configuración almacenada. La salida está ordenada por distancia ascendente
y después por ID, sin IDs repetidos, y contiene como máximo `min(k, size())`
nodos alcanzables a través del grafo. Un grafo poco conectado puede devolver
menos resultados. No existe un recorrido exhaustivo de respaldo ni fallback
a fuerza bruta.

## Validación y errores

`k` y la amplitud de búsqueda deben ser positivos. La consulta se valida
incluso si el índice está vacío: dimensión del store, componentes finitos y
norma no nula para coseno. Una consulta válida sobre un índice vacío devuelve
una colección vacía. Se conserva el contrato numérico de distance, incluido
`std::overflow_error` si la distancia euclidiana al cuadrado no cabe en un
`float` finito. Un error durante una búsqueda no modifica ni invalida el índice.

Insertar un ID inexistente o duplicado lanza `std::invalid_argument` antes de
modificar el grafo. Si una excepción de asignación, distancia u otra operación
ocurre después de empezar a modificar la topología, el índice se marca como
inutilizable. Las siguientes inserciones y búsquedas lanzan `std::logic_error`
con la indicación de reconstruirlo; `validate_graph().valid()` será falso.
Debe descartarse y reconstruirse el índice. El store conserva sus embeddings.
La inserción de HNSW no se presenta como transaccional.

Las consultas de diagnóstico `size()`, `contains(id)`, `config()`,
`entry_point()`, `max_level()` y `find_node(id)` permiten inspeccionar el estado
sin modificarlo. El punto de entrada y nivel máximo son opcionales cuando el
índice está vacío. Los punteros de solo lectura devueltos por `find_node` no
deben conservarse durante inserciones ni después de destruir el índice.

`validate_graph()` devuelve `HnswGraphValidation`, con el número de nodos,
estado de uso y contadores de referencias inválidas, embeddings ausentes,
niveles/listas incorrectos, conexiones entre capas incompatibles,
autorreferencias, vecinos duplicados, límites de grado, punto de entrada,
nivel máximo y discrepancias con el número de inserciones exitosas.
`valid()` exige cero infracciones y un índice utilizable.

La validez estructural no demuestra conectividad global ni calidad de búsqueda.
En particular, seleccionar solamente los vecinos más cercanos puede deteriorar
la conectividad con parámetros pequeños. La calidad se mide por separado
contra fuerza bruta, con consultas independientes.

## Aplicación, CLI y tiempos

`SearchService` valida `SearchRequest::hnsw_config` antes de abrir los archivos.
Después carga dataset y consulta, construye HNSW recorriendo los IDs del store
y llama a la sobrecarga correspondiente de `QueryEngine`. El grafo y el store
se destruyen al terminar la operación; no se guarda un índice persistente.

La duración `index_build_time` mide la construcción del grafo y se informa por
separado. `query_time` mide únicamente `QueryEngine::search` con `steady_clock`,
excluyendo carga CSV, construcción y presentación. Cada invocación de CLI con
HNSW vuelve a construir el índice.

```sh
./build-cli/aetherindex search \
  --dataset datasets/examples/vectors.csv \
  --query datasets/examples/query.csv \
  --algorithm hnsw --k 10 \
  --m 16 --m0 32 --ef-construction 100 --ef-search 50 --seed 42
```

Los parámetros HNSW de CLI son `--m`, `--m0`, `--ef-construction`, `--ef-search`
y `--seed`; `max_level` se configura solamente mediante la API C++.
La métrica se selecciona con `--metric cosine` o `--metric squared-euclidean`.
La tabla mantiene IDs enteros y distancias con nueve decimales.

## Protocolo de pruebas

La suite normal registra 16 pruebas, incluyendo cuatro unitarias HNSW,
evaluación de Recall y flujo HNSW de CLI. Se ejecuta sin red ni dependencias
externas. Las pruebas HNSW cubren configuración, nodos, niveles reproducibles,
inserción incremental, grafo, ambas métricas, consultas inválidas, ceros,
extremos finitos, desbordamiento y recuperación mediante reconstrucción.

El protocolo de aceptación utiliza 1,000 embeddings × 128 dimensiones,
dataset con semilla 42, 100 consultas independientes con semilla 424242 y
grafo con semilla 42, para cada métrica. Evalúa `ef_search` 10, 30, 50 y 100
sobre el mismo grafo. El umbral obligatorio es Recall@10 promedio ≥ 0.85 con
los valores predeterminados, `ef_search = 50`. Se verifica dimensión, validez,
unicidad, orden y distancia de cada resultado; la referencia exacta comprueba
que no hay empates ambiguos en el corte Top-10. Se conservan resultados de
cada consulta y parámetros en un CSV generado dentro de la carpeta del build.

La comprobación inicial del núcleo obtuvo Recall@10 de 0.977 para coseno y
0.972 para euclidiana al cuadrado. La evaluación real optativa utiliza 10,000
embeddings DBpedia × 1,024 dimensiones y 20 consultas separadas: la ejecución
Release inicial obtuvo 0.985 con amplitud 50 y 0.995 con amplitud 100. Estos
resultados describen esos protocolos; no garantizan el mismo Recall en otro
dataset ni convierten tiempos de CTest en benchmarks de rendimiento.

```sh
cmake -S . -B build-hnsw-debug -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build-hnsw-debug
ctest --test-dir build-hnsw-debug --output-on-failure
ctest --test-dir build-hnsw-debug -R hnsw -V
```

Con `AETHER_REAL_DATA_DIR` y los datos preparados se registran dos pruebas
adicionales, 18 en total: la referencia exhaustiva de CLI y la comparación
HNSW/fuerza bruta sobre un índice reutilizado. Ver el [caso real](../real_world_validation.md).
La evidencia final de compilaciones, CTest, sanitizers y criterios pendientes
se registra en [validación HNSW](../architecture/hnsw_validation.md).
La revisión del equipo y aprobación del PR son requisitos separados.
