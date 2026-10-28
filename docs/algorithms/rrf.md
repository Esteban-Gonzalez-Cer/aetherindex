# Reciprocal Rank Fusion — CP-03

## Alcance y dependencia

`aether_hybrid` implementa RRF en C++20 y solo depende de `aether_core` y la
biblioteca estándar. Su implementación está en `src/hybrid/rrf.cpp`; sus headers
públicos, en `include/aether/hybrid/`. El namespace es `aetherindex::hybrid`,
adaptado al namespace real del proyecto.

Recibe dos rankings documentales ya ordenados y devuelve resultados propios
`vector<RankedResult>`. No ejecuta HNSW/BM25, ni consulta un DocumentStore, ni
recibe puntuaciones originales, ni convierte VectorId en DocumentId.

La composición de búsquedas, asociación documental y CLI híbrida corresponden
a CP-04. CP-03 puede validarse con rankings conocidos sin requerir embeddings,
un corpus de relevancia ni una llamada a los recuperadores.

## Fórmula y parámetros

$$RRF(d)=\sum_{r:d\in r}\frac{1}{k_{rrf}+rank_r(d)}$$

Las posiciones empiezan en **1**. Un documento ausente de una lista aporta cero
por esa lista. Un documento presente en ambas suma dos contribuciones, pero
aparece una sola vez en la salida.

La fórmula y constante inicial 60 siguen el [artículo original de Cormack,
Clarke y Büttcher, SIGIR 2009](https://cormack.uwaterloo.ca/cormacksigir09-rrf.pdf).
En este módulo, `k_rrf` es double, configurable, finito y estrictamente positivo;
se permiten valores fraccionarios. No es el `top_k` de documentos finales.

Todos los cálculos son double. El constructor rechaza cero, negativos, NaN e
infinitos con `std::invalid_argument`. La configuración se copia y puede
inspeccionarse mediante `config()`, que devuelve otra copia.

## Uso desde C++20

```cpp
#include <array>
#include <iomanip>
#include <iostream>
#include "aether/hybrid/rrf.hpp"

int main() {
    using aetherindex::DocumentId;
    const aetherindex::hybrid::RrfFusion fusion({.k_rrf = 60.0});
    const std::array<DocumentId, 3> dense = {101, 102, 103};
    const std::array<DocumentId, 3> lexical = {102, 104, 101};
    const auto results = fusion.fuse(dense, lexical, 10);
    for (const auto& result : results) {
        std::cout << result.document_id << ' ' << std::fixed
                  << std::setprecision(10) << result.score << '\n';
    }
    return results.size() == 4 && results.front().document_id == 102 ? 0 : 1;
}
```

Salida del ejemplo:

```text
102 0.0325224749
101 0.0322664585
104 0.0161290323
103 0.0158730159
```

El consumidor enlaza únicamente la biblioteca:

```cmake
target_link_libraries(mi_consumidor PRIVATE aether_hybrid)
```

El algoritmo no imprime ni abre archivos. Los informes JSON pertenecen al
ejecutable de pruebas; no forman parte de la API de producción.

## Contrato de entrada y salida

```cpp
std::vector<aetherindex::RankedResult> fuse(
    std::span<const aetherindex::DocumentId> dense_ranking,
    std::span<const aetherindex::DocumentId> lexical_ranking,
    std::size_t top_k) const;
```

| Condición | Comportamiento |
|---|---|
| Rankings preparados | Mejor a peor: denso tras ordenar por distancia ascendente; léxico por score descendente. RRF conserva esas posiciones. |
| IDs compartidos entre listas | Válidos; una contribución por lista. |
| ID repetido dentro de una lista | `std::invalid_argument`, incluso si queda después del Top-K solicitado. |
| Ranking vacío | Permitido; se usa la otra lista. |
| Ambos vacíos | Salida vacía si top_k es válido. |
| top_k=0 | `std::invalid_argument`, también con listas vacías. |
| top_k mayor que la unión | Se devuelven todos los documentos disponibles; no se reserva memoria según top_k. |
| IDs cero, máximo uint32 o no secuenciales | Admitidos; no son posiciones internas de arrays. |
| Desempate | Score double descendente y DocumentId ascendente en igualdad exacta. |
| Memoria insuficiente | Se propaga la excepción de asignación; objeto y entradas no se modifican. |
| Contribución no representable/score no finito | `std::overflow_error`; no se publica un ranking parcial. |

No se utiliza epsilon en el comparador de ordenamiento. La tolerancia corresponde
a las comprobaciones matemáticas, no a la definición del orden estricto.

Las vistas de entrada se leen únicamente durante la llamada. No se conservan
punteros/spans ni se modifican sus elementos. Se admiten listas idénticas o
spans que se solapan, siempre que cada lista individual sea válida. La salida
es un vector propietario. No se añade un sistema de indexación, estado READY,
persistencia o ejecución concurrente en esta tarjeta.

## Integración documental para CP-04

La capa de aplicación deberá:

1. Obtener candidatos de cada recuperador, con su orden de relevancia válido.
2. Resolver explícitamente cada VectorId del ranking denso a un DocumentId.
3. Garantizar documentos únicos en cada lista antes de invocar RRF. Si hay
   fragmentos múltiples por documento, su agregación requiere otra política
   explícita; RRF no los elimina silenciosamente.
4. Extraer los DocumentId del resultado BM25 conservando el orden.
5. Llamar a `fuse()` y consumir el vector de `RankedResult` resultante. Aquí
   su campo score significa puntuación RRF, no distancia ni score BM25.

DocumentId y VectorId son aliases uint32 distintos por significado, pero no
tipos fuertes diferentes para el compilador. La API no puede detectar que un
cliente le pasó IDs vectoriales sin convertir; esa responsabilidad y la
existencia de los documentos pertenecen al adaptador de aplicación.

`candidate_k` de cada recuperador es independiente del `top_k` final y de
`k_rrf`. La propuesta de CP-03 usa 20 candidatos como punto de partida para
experimentar con Top-10, pero **RRF no solicita candidatos ni fija esa política**.
La calidad depende de las listas recibidas: un documento fuera de ambas no
puede aparecer tras la fusión.

`integration.rrf_contract` utiliza SearchResult y RankedResult preparados, una
tabla de asociación con IDs numéricamente distintos y un adaptador exclusivo
del test. Verifica conversión, orden, scores y consumo por rangos. No ejecuta
consultas HNSW/BM25 ni constituye una prueba de QueryEngine híbrido real.

## Algoritmo y complejidad

Se recorren primero los IDs densos y después los léxicos, en sus posiciones
originales. Un conjunto local por lista detecta duplicados; un mapa local
DocumentId→double acumula scores. Después se crea y ordena el vector completo
de candidatos únicos y se trunca a top_k.

Para D y L posiciones y U documentos distintos, el tiempo esperado es
`O(D + L + U log U)` y la memoria adicional `O(U)`. No hay normalización de scores
originales, ponderación de ramas, heap especializado, SIMD ni poda anticipada.

Con las mismas listas, configuración y entorno, la operación es reproducible:
el orden de suma es fijo y la iteración de la tabla hash no define el ranking
final. Esto no promete orden bit a bit entre toolchains o modos flotantes
diferentes. No se utilizan opciones fast-math en las comprobaciones de referencia.

## Ejecutar las pruebas

Desde la raíz del proyecto:

```sh
cmake -S . -B build-rrf -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build-rrf --parallel 4
ctest --test-dir build-rrf -L cp03 --output-on-failure
ctest --test-dir build-rrf -L kr1_4 --output-on-failure
ctest --test-dir build-rrf --output-on-failure
```

Hay **22 casos RRF-01 a RRF-22**, registrados individualmente en CTest, más
una prueba de propiedades y una prueba de contrato: 24 comprobaciones de CP-03.
`-L kr1_4` selecciona exclusivamente la matriz de 22 casos. Cada caso conserva
su esperado/obtenido y errores de contrato en
`build-rrf/tests/rrf-evidence/Debug/`, también cuando falla una comprobación.

Para construir solo el módulo sin tests ni otros motores:

```sh
cmake -S . -B build-rrf-library -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build-rrf-library --target aether_hybrid --parallel 4
```

En Visual Studio, añadir `--config Debug` a la construcción y `-C Debug` a
CTest. La ejecución de esta entrega se valida en macOS ARM64; no acredita una
ejecución Windows/Linux.

## Precisión y casos adicionales

Las referencias matemáticas usan fracciones completas, con error absoluto
permitido **≤1e-12**. Los números mostrados con diez decimales son de
presentación y **no** se utilizan como oráculo con esa tolerancia. Por ejemplo,
el valor de Doc 102 se comprueba contra `1/62 + 1/61`.

La prueba de propiedades añade 200 pares de listas determinísticos, con semilla
42 y constantes 0.25/1/10/60/100. Un oráculo independiente enumera la unión y
busca las posiciones en las listas originales; no usa el mapa acumulador ni
helpers del algoritmo. Se comprueban scores, unicidad, orden, prefijos Top-K,
intercambio de listas e inmutabilidad.

También se prueban constantes positivas extremas, IDs cero/máximo, spans
solapados, duplicados más allá de top_k y reutilización tras errores. RRF-21
comprueba 50 ciclos de repetición tanto del ejemplo nominal como de un empate,
incluyendo igualdad exacta de los scores generados en ese entorno.

La evidencia de ejecución y el alcance del indicador KR1.4 se documentan en
[el informe CP-03](../reports/CP03_RRF_VALIDATION.md).
