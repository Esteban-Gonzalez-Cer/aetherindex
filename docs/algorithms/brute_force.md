# Fuerza bruta con Top-K acotado

`bruteForceSearch(store, query, k)` sigue siendo la referencia exhaustiva: evalúa
los N embeddings con la métrica del store y devuelve los K mejores. No aproxima
ni omite candidatos. El cambio de selección reduce el espacio auxiliar de O(N)
a O(K), y el costo de ordenación de O(N log N) a O(N log K).

## Flujo

1. Si K=0 o el store está vacío, devuelve vacío antes de validar la consulta.
2. `QueryEngine` construye un `PreparedQuery`: valida, alinea, rellena con ceros
   y normaliza una sola vez si la métrica es Cosine.
3. `bruteForceSearchPrepared` recorre posiciones densas públicas del store.
   Lee `ids()[position]` y `data_by_position(position)`, sin hashes por distancia,
   copias de embeddings ni acceso a datos privados.
4. Un max-heap de pares `(distancia FP32, ID externo)` retiene como máximo
   `min(K,N)` candidatos. Su raíz es el peor resultado retenido.
5. Solo reemplaza la raíz si llega un candidato mejor. Extrae el heap de atrás
   hacia delante para devolver orden ascendente. Los empates se resuelven por ID.

El trabajo total es O(ND + N log K), con O(K) resultados temporales y O(D)
para preparar la consulta. Si K es cercano a N, la ventaja del heap disminuye.
La búsqueda directa también prepara la consulta una sola vez; la entrada
preparada evita repetir esa preparación cuando llama `QueryEngine`.

## Distancias y errores

Cosine usa producto punto de vectores unitarios; SquaredEuclidean conserva
magnitudes y suma diferencias al cuadrado. Ambos comparten los kernels de
`distance`, con acumulación FP32 y SIMD cuando la arquitectura lo permite.
Dimensiones incompatibles, consultas no finitas o norma cero Cosine producen
`invalid_argument`. Un resultado squared no representable produce
`overflow_error`, sin entregar un ranking parcial. El almacenamiento no cambia.

La firma pública continúa recibiendo `const VectorStore&`, `span<const float>`
y `size_t k`; los IDs son uint32, incluyendo cero y UINT32_MAX. No se admiten
lecturas simultáneas con inserciones o reservas de memoria.

## Verificación

`unit.optimization_contracts` compara el resultado completo con un oráculo de
ordenación total usando el mismo kernel: dos métricas, K pequeño/grande y
empates en el corte. Esto comprueba la selección exacta bajo el cálculo FP32.
La paridad matemática frente al coseno tradicional se comprueba por separado.

El benchmark optativo de CA-1.3 usa N=100,000, D=128 y K=10: ordenación total y
heap tienen idéntico kernel y preparación. Conserva muestras, alterna el orden
de ejecución y mide únicamente la búsqueda. No atribuye mejoras de SIMD o
normalización al cambio de Top-K. Ver [protocolo](../../benchmarks/optimization_protocol.json)
y [reproducción de benchmarks](../benchmarks/OPTIMIZATION_VALIDATION.md).
