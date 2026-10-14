# Validación — Crear CLI mínima

Fecha local: **2026-10-12**, America/Mexico_City. Implementación verificada en
macOS ARM64. Esta entrega cubre la CLI con el algoritmo disponible,
**BruteForce**. HNSW no está implementado; sus criterios funcionales se mantienen
pendientes y no se presentan como aprobados.

La ampliación posterior de formato, tiempo de consulta y datos reales se registra
en [Validación de tabla, tiempos y datos reales](cli_output_validation.md).
Las ejecuciones y tiempos siguientes corresponden a la entrega inicial de CLI.

HNSW se habilitó posteriormente tras verificar el núcleo contra la tarjeta
correspondiente. El estado actual y sus evidencias están en
[Auditoría e integración de HNSW](hnsw_validation.md); los estados «pendiente»
de este informe describen la entrega original.

## Alcance implementado

- `search`, `--dataset`, `--query`, `--algorithm`, `--k`, `--help` y selección
  opcional de métrica con `--metric`.
- Coseno por defecto y euclidiana al cuadrado opcional; `K > 0` en CLI y
  aplicación, con `min(K,N)` resultados. Se preserva el contrato de `QueryEngine`
  y del algoritmo, que siguen admitiendo `k == 0` para sus consumidores directos.
- Lectura estricta de CSV, IDs de 32 bits incluyendo 0 y máximo, dimensiones,
  componentes finitos, duplicados y consultas de una sola fila.
- Streaming de filas a un store local, sin colección paralela de embeddings.
  Un error descarta la carga y no imprime resultados parciales.
- Presentación de resultados, stdout/stderr separados y códigos 0, 2, 3 y 4.
- Separación entre parser/runner de CLI, `SearchService`, lector CSV interno,
  `QueryEngine`, almacenamiento y búsqueda.
- CMake, ejemplos pequeños, generador reproducible y documentación de uso.

La CLI anterior sin argumentos se sustituyó por ayuda y búsqueda desde archivos.
El ranking del ejemplo anterior se conserva en `integration.dense_retrieval`.
No se añadieron persistencia, HNSW, SIMD, índices ficticios, benchmarks ni
dependencias externas.

## Resultado de las ejecuciones

| Configuración | Resultado | Evidencia |
|---|---|---|
| Debug, AppleClang 17, `-Wall -Wextra -Wpedantic` | Build correcto; **10/10** CTest; salida 0 | [Registro Debug](validation/cli_debug.log) |
| Release, AppleClang 17, mismas advertencias | Build correcto; **10/10** CTest; salida 0 | [Registro Release](validation/cli_release.log) |
| Debug + ASan/UBSan, Clang 23.1.3 | Build correcto; **10/10** CTest; salida 0; sin diagnósticos de sanitizers en las rutas evaluadas | [Registro sanitizers](validation/cli_sanitizers.log) |
| Release, `BUILD_TESTING=OFF` | Build correcto; CTest enumera 0 tests; búsqueda del ejemplo correcta, salida 0 | Ver comandos y resultado reproducible abajo |
| Formato y diff | `clang-format --dry-run --Werror` y `git diff --check`, salida 0 | Archivos C++ modificados/nuevos formateados |

Las tres suites se ejecutaron de forma independiente en directorios nuevos y
terminaron con 3.22 s, 4.61 s y 8.00 s de tiempo total CTest, respectivamente.
Se ejecutaron concurrentemente; incluyen preparación, múltiples procesos y
comprobaciones. **No son medidas de latencia ni una comparación de rendimiento.**

En el primer pase, una prueba de integración detectó que un directorio se
reportaba como dataset vacío. Se corrigió la validación de archivos regulares
y se ejecutaron las suites completas que se conservan arriba.

## Correspondencia con el checklist de la tarjeta

| Prueba de la tarjeta | Cobertura y estado |
|---|---|
| T1 — Ayuda | Aprobado: ayuda global, `search --help` y sin argumentos, salida 0 |
| T2 — Argumentos | Aprobado para fuerza bruta, ambas métricas, K y rutas con espacios; argumentos incompletos, desconocidos, repetidos y fuera de rango rechazados |
| T3 — Carga | Aprobado: cuatro vectores 2D del ejemplo, asociación ID/embedding, LF/CRLF, líneas vacías, espacios y notación científica |
| T4 — BruteForce | Aprobado: IDs 10 y 40 a distancias 0 y aproximadamente 0.2; tolerancia estricta menor que `1e-5` |
| T5 — Búsqueda HNSW | **Pendiente**; la prueba actual solo confirma el rechazo explícito sin fallback, código 2 y antes de lectura |
| T6 — Comparación de algoritmos | **Pendiente**; se valida selección de métricas con rankings diferentes, sin presentarla como comparación ANN/exacta |
| T7 — K | Aprobado: 1, 5, 10, 20, valor predeterminado y truncamiento; el servicio también comprueba máximo `size_t` |
| T8 — Entradas inválidas | Aprobado para todos los casos inválidos definidos en la tarjeta, además de valores no finitos, ID fuera de rango, query vacía/múltiple, norma cero y desbordamiento de distancia |
| T9 — MVP | **Aprobado con BruteForce:** 1,000 × 128, semilla 42, diez consultas Top-10 distintas mediante diez ejecuciones reales de CLI. La variante HNSW queda **pendiente** |
| T10 — Otro integrante | Guía y datos reproducibles disponibles; ejecución por otro integrante y Windows **pendientes**, no simuladas |
| T11 — CTest y sanitizers | Aprobado: las diez pruebas pasan en las tres configuraciones descritas |

Los diez casos CTest son `unit.vector_store`, `unit.vector_store_allocation`,
`unit.distance`, `unit.brute_force`, `integration.storage_distance`,
`integration.dense_retrieval`, `unit.cli_parser`, `integration.search_service`,
`e2e.cli_search` y `e2e.cli_mvp`.

`e2e.cli_mvp` verifica tamaño y dimensión reportados, diez filas de resultados
por consulta, rangos y pertenencia de IDs, ausencia de IDs repetidos, distancias
finitas en el rango coseno y orden ascendente. Cada consulta usa un embedding
distinto conocido y exige su ID como primer vecino con distancia menor que
`1e-5`. Es una comprobación funcional; no evalúa relevancia ni rendimiento.

También se corrigió el helper compartido `requireNear`: ahora exige
`abs(double(actual) - double(expected)) < 1e-5`, en lugar de `<=`, y rechaza
valores no finitos. Las pruebas anteriores de distancias pasan con ese criterio
estricto. La comprobación de gran magnitud conserva su tolerancia relativa
explícita.

## Reproducir la validación

Desde la raíz del repositorio, reemplazando las carpetas temporales si se desea:

```sh
cmake -S . -B /private/tmp/aetherindex-cli-debug-01a12e3a -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON '-DCMAKE_CXX_FLAGS=-Wall -Wextra -Wpedantic'
cmake --build /private/tmp/aetherindex-cli-debug-01a12e3a --parallel 4
ctest --test-dir /private/tmp/aetherindex-cli-debug-01a12e3a --output-on-failure -V

cmake -S . -B /private/tmp/aetherindex-cli-release-01a12e3a -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON '-DCMAKE_CXX_FLAGS=-Wall -Wextra -Wpedantic'
cmake --build /private/tmp/aetherindex-cli-release-01a12e3a --parallel 4
ctest --test-dir /private/tmp/aetherindex-cli-release-01a12e3a --output-on-failure -V

cmake -S . -B /private/tmp/aetherindex-cli-sanitizers-01a12e3a -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DAETHER_ENABLE_SANITIZERS=ON '-DCMAKE_CXX_FLAGS=-Wall -Wextra -Wpedantic'
cmake --build /private/tmp/aetherindex-cli-sanitizers-01a12e3a --parallel 4
ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ctest --test-dir /private/tmp/aetherindex-cli-sanitizers-01a12e3a --output-on-failure -V

cmake -S . -B /private/tmp/aetherindex-cli-no-tests-01a12e3a -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF '-DCMAKE_CXX_FLAGS=-Wall -Wextra -Wpedantic'
cmake --build /private/tmp/aetherindex-cli-no-tests-01a12e3a --parallel 4
ctest --test-dir /private/tmp/aetherindex-cli-no-tests-01a12e3a -N
/private/tmp/aetherindex-cli-no-tests-01a12e3a/aetherindex search --dataset datasets/examples/vectors.csv --query datasets/examples/query.csv --algorithm brute-force --k 2
```

La última búsqueda produce `Vectors indexed: 4`, `Dimensions: 2`, dos vecinos
y las filas `1 / 10 / 0` y `2 / 40 / 0.200000003`, con salida 0.

Clang de Homebrew se seleccionó explícitamente por el problema de inicialización
ASan de AppleClang observado en la tarjeta anterior. No se cambió el compilador
predeterminado ni se instaló software.

## Estado de aceptación

La parte de la CLI utilizable con el núcleo actual está implementada y probada.
La tarjeta completa **todavía no se marca Done**: quedan HNSW y su integración,
T5/T6 y la variante HNSW de T9, reproducibilidad por otro integrante y revisión /
aprobación de PR. No se crearon PRs ni se simuló una revisión de equipo.

Ver [la guía de uso](../cli.md), [arquitectura](README.md) y
[contrato de fuerza bruta](../algorithms/brute_force.md).
