# HNSW con FlatGraph y selección diversificada

El índice conserva la jerarquía probabilística y las búsquedas aproximadas de
HNSW. La topología usa arenas contiguas; la política predeterminada es el
Algoritmo 4 de selección de vecinos. Los embeddings pertenecen exclusivamente
a `VectorStore`. No hay persistencia, actualizaciones, eliminación, generación
de embeddings ni concurrencia.

## Organización y dependencias

| Archivo | Responsabilidad |
|---|---|
| `include/aether/ann/hnsw/flat_graph.hpp` | Arena de slots [grado, vecinos] de stride fijo |
| `hnsw_config.hpp` en el mismo módulo | Parámetros y política de selección |
| `hnsw_node.hpp` | Valor diagnóstico escalar, sin contenedores propietarios |
| `hnsw_index.hpp` | API, buffers globales y diagnóstico de memoria/estructura |
| `src/ann/hnsw/hnsw_index.cpp` | Inserción, navegación, poda y diversificación |

`aether_hnsw` depende de core/storage y enlaza distance privadamente. No depende
de fuerza bruta, CLI ni aplicación. Las pruebas usan fuerza bruta como oráculo.
La CLI accede mediante `SearchService` → `QueryEngine` → índice.

## Layout y propiedad

Las aristas usan uint32 internos: posiciones densas de inserción en VectorStore.
La traducción externa se hace con `ids()[position]`. Un ID externo UINT32_MAX
funciona igual que cero sin reservar una matriz por el valor numérico del ID.

- Layer 0: un arena con N slots; cada slot contiene 1 contador + M0 vecinos.
- Superiores: un segundo arena con slots de 1 contador + M vecinos. Cada nodo
  promovido tiene sus slots superiores consecutivos.
- Metadatos: un uint8 de nivel máximo por posición (255 significa no indexado)
  y un uint32 con su primer slot superior.
- Punto de entrada, nivel máximo, contador de inserciones y generador aleatorio:
  campos globales. No hay mapa ni vector de vecinos propietario por nodo.

Los dos arenas están alineados a 64 bytes. Sus slots **no** se alinean por separado:
con M0=32 un slot mide 132 bytes. Los embeddings sí comienzan cada uno en una
dirección alineada a 64 bytes. Los vecinos se leen como `span<const InternalId>`
sin copias. Antes de evaluar un vecino se solicita prefetch del siguiente embedding
mediante builtin GCC/Clang, intrínseco MSVC x86 o fallback portable.

El índice reserva para el tamaño conocido del store. `reserve(N)` también permite
anticipar una carga incremental. Las capas superiores reservan aproximadamente
1.2×N/(M−1)+32 slots; esto es headroom estocástico, no un límite de nivel ni una
cuota de bytes universal para cualquier semilla o patrón de crecimiento.

`memory_usage()` desglosa capacidades realmente asignadas, metadatos y sizeof
el objeto, incluido el RNG. Excluye embeddings y espacios temporales de búsqueda
e inserción. No debe confundirse con RSS. Para CA-2.2 se usa el límite aclarado
**14,500,000 bytes** para el grafo completo, y 13,200,000 bytes solo para Layer 0,
con N=100,000, M=16, M0=32. El benchmark reporta también el delta RSS del proceso,
que incluye efectos del asignador y no permite aislar exactamente un objeto.

## API

```cpp
HnswIndex index(store, config);
for (VectorId id : store.ids()) index.insert(id);
QueryEngine engine;
auto result = engine.search(index, query, 10);
auto wider = index.search(query, 10, 100); // mismo grafo, otro efSearch
```

`find_node(id)` devuelve `optional<HnswNode>` por valor, con id, internal_id y
max_level. Sustituye el antiguo puntero a un nodo propietario de subvectores.
`neighbors(id, level)` devuelve vecinos internos; `external_id(internal)` los
traduce. Los spans deben descartarse antes de insertar/reservar. El índice no
se copia ni mueve y el store debe vivir más tiempo. Se pueden añadir embeddings
e indexar sus IDs entre operaciones; no se solapan lecturas y escrituras.

## Parámetros

| Campo | Valor predeterminado | Condición |
|---|---:|---|
| m / m_max_0 | 16 / 32 | m≥2, M0≥M, ambos representables como grado uint32 |
| ef_construction / ef_search | 100 / 50 | efConstruction≥M, efSearch>0 |
| random_seed | 42 | uint64, incluido cero |
| max_level | 32 | 0..32 |
| neighbor_selection | Heuristic | Heuristic (Alg.4) o Simple (Alg.3) |
| extend_candidates | false | Añade vecinos de un salto del conjunto original C |
| keep_pruned_connections | true | Rellena cupos con candidatos descartados |

La CLI añade `--neighbor-selection heuristic|simple` para comparaciones controladas.
La extensión y el relleno se configuran mediante la API C++.

El nivel se muestrea con mt19937_64 y `floor(-log(U)/log(M))`, con el mismo mapeo
explícito de 52 bits que el baseline y un máximo de 32. Ese cálculo double es
muestreo de niveles, no acumulación de distancias. Se conserva la semilla entre
inserciones; la reproducibilidad exige iguales datos, orden, opciones y entorno.

## Inserción y Algoritmo 4

1. Verifica existencia en el store y ausencia en el índice; asigna slots antes
   de activar el nodo. Niveles ausentes permiten indexar posiciones en otro orden.
2. Desciende desde el punto de entrada con ef=1 por encima del nuevo nivel.
3. En cada capa compartida ejecuta SEARCH-LAYER con efConstruction y selecciona
   candidatos ordenados por distancia al nodo base, con desempate por ID externo.
4. Algoritmo 4 acepta e solo si no existe un vecino seleccionado r con
   `distance(e,r) < distance(e,base)`. Conserva descartados en orden de distancia
   para rellenar si la opción está activada. La extensión, si está activada,
   une los vecinos de un salto de C y elimina duplicados antes de seleccionar.
5. Conecta el nuevo nodo y propone enlaces recíprocos. Si una lista está llena,
   puntúa M_max+1 candidatos en un buffer temporal y poda antes de escribir;
   nunca escribe un slot con grado mayor que su capacidad.
6. Actualiza entrada/nivel máximo si procede.

Se conserva la política del proyecto de seleccionar hasta M0 conexiones iniciales
en Layer 0 y M en superiores. Esta variante se mantiene igual en las ablaciones
Simple/Heuristic. La poda puede producir aristas de un solo sentido.

## Búsqueda y errores

SEARCH-LAYER usa min-heap de candidatos, max-heap de resultados y un bitmap denso
de visitados temporal de un bit por posición y recorrido. No asigna un hash/objeto por nodo visitado.
QueryEngine prepara la consulta una sola vez; distancias entre candidatos y
consultas, o entre nodos en la diversificación, llaman al mismo kernel FP32.

La consulta baja con ef=1 y explora Layer 0 con `min(size,max(k,efSearch))`.
Devuelve hasta min(k,size) nodos alcanzables ordenados por (distancia, ID).
K=0 o ef=0 se rechazan; la consulta se valida incluso en un índice vacío.
No hay fallback exhaustivo ni garantía universal de Recall.

Errores de precondición conservan el grafo. Una excepción después de activar
el nodo marca el índice inutilizable: hay que reconstruirlo; no se afirma rollback
transaccional de HNSW. Allocation failures antes de activarlo pueden cambiar
capacidades pero conservan los nodos existentes. Errores de búsqueda no invalidan
el índice y nunca modifican el store.

`validate_graph()` verifica referencias, presencia de embeddings, niveles,
capas, duplicados, autoenlaces, grados, entrada y cantidad. Estas invariantes
no demuestran conectividad ni exactitud: se evalúan mediante consultas aparte.

## Evidencia y reproducción

Las 19 pruebas normales incluyen invariantes, crecimiento incremental, IDs
extremos, opciones de Algoritmo 4, casos de diversidad conocidos, reproducción
de topología, fallos de asignación y Recall con las dos métricas. El caso real
opcional compara DBpedia con fuerza bruta. Los umbrales de memoria, Recall
agrupado, Top-K y p99 se registran como CTest optativos de rendimiento.

Ver [validación de optimizaciones](../benchmarks/OPTIMIZATION_VALIDATION.md).
El protocolo exige en CA-2.4 un incremento **absoluto ≥0.05**, con N=10,000,
D=128 y efSearch=30; no una mejora relativa del 5%. CA-2.3 necesita contadores
PMU de Linux: no se infiere una reducción LLC del tiempo de ejecución en macOS.

Base algorítmica: [Malkov y Yashunin, Algorithms 1–5](https://arxiv.org/abs/1603.09320).
