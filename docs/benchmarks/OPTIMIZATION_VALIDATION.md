# Validación de optimización vectorial y FlatGraph

Fecha del protocolo: 19 de octubre de 2026. El protocolo se fijó antes de medir:
[optimization_protocol.json](../../benchmarks/optimization_protocol.json).
Las mediciones y el estado individual de los criterios se documentan en
[el informe técnico de optimización](../reports/OPTIMIZACION_VECTORIAL_FLATGRAPH.md).
Las evidencias iniciales de Sprint 1 son históricas y se conservan sin sustituir
sus mediciones por resultados de otra versión.

## Contratos corregidos

- CA-2.2: **13,200,000 bytes** corresponden solo a Layer 0; el grafo completo,
  excluyendo embeddings, tiene límite **14,500,000 bytes decimales**. Metadatos,
  capacidades reservadas y objeto global deben incluirse; RSS se reporta aparte.
  Un delta RSS del proceso mayor a 14.50 MB no acredita una cota RSS de ese valor,
  aunque la suma de bytes propios del grafo sí cumpla el presupuesto.
- CA-2.4: diferencia absoluta de medias de Recall@10 **≥0.05** (cinco puntos
  porcentuales), con N=10,000, D=128, efSearch=30. Iguales semillas, datos,
  consultas, orden de inserción y parámetros, salvo la política Algoritmo 3/4.
- Cosine almacena vectores unitarios y rechaza norma <1e-7 al insertar. Esta
  versión cambia intencionadamente la conservación de magnitudes del baseline.
  SquaredEuclidean mantiene los valores originales. QueryEngine normaliza una
  vez; los kernels de comparación no calculan normas ni usan double.

## Compilar las comprobaciones

Desde la raíz del repositorio, usar carpetas nuevas para cambiar de compilador:

```sh
cmake -S . -B build-opt -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DAETHER_NATIVE_SIMD=ON -DAETHER_BUILD_BENCHMARKS=ON \
  -DAETHER_REGISTER_PERFORMANCE_TESTS=ON \
  -DAETHER_REAL_DATA_DIR="$PWD/datasets/real/dbpedia"
cmake --build build-opt --parallel 4
ctest --test-dir build-opt -E '^performance\.' --output-on-failure
ctest --test-dir build-opt -R '^performance\.' -V
```

La suite normal contiene 19 pruebas sin bibliotecas externas; con DBpedia se
registran dos adicionales. NumPy se requiere solo para el oráculo real.
Los cinco checks optativos son `performance.topk`, `.memory`, `.clustered`,
`.dbpedia` y `.assembly` (este último solo con GCC/Clang y SIMD soportado).
Los umbrales fallidos retornan código distinto de cero y conservan las muestras:
no se convierten automáticamente en aprobaciones por completar una medición.
No ejecutar otros builds, pruebas o procesos pesados durante benchmarks.

También se pueden medir por separado, sin imponer umbrales:

```sh
./build-opt/benchmarks/aether_optimization_benchmark topk build-opt/manual/topk
./build-opt/benchmarks/aether_optimization_benchmark memory build-opt/manual/memory
./build-opt/benchmarks/aether_optimization_benchmark clustered build-opt/manual/clustered
./build-opt/benchmarks/aether_dbpedia_benchmark datasets/real/dbpedia build-opt/manual/dbpedia
```

## Alcance de las mediciones

| Comprobación | Qué mide | Qué no acredita |
|---|---|---|
| Top-K | 50 muestras por variante, N100k D128 K10, idéntico kernel; orden alternado; media y mediana | Mejora universal para cualquier dimensión, K o CPU |
| Memoria | Capacidad real de los cuatro buffers y objeto; 100k nodos M16/M032; delta RSS adicional | Memoria total del proceso igual a la memoria de la topología |
| Clusters | 100 centros gaussianos, 100 puntos/centro, ruido stddev 0.015; 1000 consultas separadas | Garantía universal de +0.05 en cualquier dataset |
| DBpedia | 1000 búsquedas, un hilo, índice precargado, Top10 ef50; incluye preparación de consulta | 1000 consultas independientes, concurrencia, latencia fría o SLA de producción |
| Ensamblador | Kernel compilado -O3; FMA empaquetada y ausencia de float→double, raíz/división | Ejecución probada en CPUs no disponibles en esta máquina |

El fixture agrupado usa Box-Muller explícito y semillas 42/424242; no se ajusta
tras ver resultados. Se guardan los arrays FP32 generados y sus hashes para
identificar exactamente los bytes usados y detectar diferencias de libm entre
plataformas. El driver regenera el fixture; un hash diferente debe declararse
como otra evaluación, aunque se haya usado la misma semilla.
El corpus DBpedia se verifica contra `manifest.json`; son 10,000×1024 y 20
consultas fuera del corpus, repetidas 50 veces tras 100 búsquedas de calentamiento.
P99 se calcula por rango más cercano: muestra ordenada número 990 de 1000.
Construcción, carga CSV, recall y escritura de archivos quedan fuera del timer.
La CLI continúa reconstruyendo el índice por proceso y reporta construcción aparte.

## Caché: CA-2.3 requiere Linux

El driver `dbpedia_benchmark.cpp` se puede compilar contra los headers/bibliotecas
anteriores con `AETHER_LEGACY_BASELINE`. En esta tarea se conserva una copia
pre-cambio en `build-optimization-baseline/source`, con manifiesto SHA256 y build
Release. Ese directorio local está ignorado; para otra máquina se necesita el
commit base y el driver actual. La producción al iniciar esta tarea coincidía
con el commit base `031adae19951d95ede349eaeaf295dd8d4a611d2`.

Ejemplo para crear un baseline reproducible, sin modificar el checkout actual:

```sh
mkdir -p build-perf-baseline/source
git archive 031adae19951d95ede349eaeaf295dd8d4a611d2 | tar -x -C build-perf-baseline/source
cmake -S build-perf-baseline/source -B build-perf-baseline/build \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build-perf-baseline/build --parallel 4
c++ -std=c++20 -O3 -DAETHER_LEGACY_BASELINE \
  -Ibuild-perf-baseline/source/include benchmarks/dbpedia_benchmark.cpp \
  build-perf-baseline/build/libaether_application.a \
  build-perf-baseline/build/libaether_brute_force.a \
  build-perf-baseline/build/libaether_hnsw.a \
  build-perf-baseline/build/libaether_storage.a \
  build-perf-baseline/build/libaether_distance.a \
  -o build-perf-baseline/build/aether_dbpedia_benchmark
python3 scripts/profile_hnsw_cache.py \
  --baseline build-perf-baseline/build/aether_dbpedia_benchmark \
  --optimized build-opt/benchmarks/aether_dbpedia_benchmark \
  --data-dir datasets/real/dbpedia --output build-opt/cache-evidence --cpu 0
```

En Linux con PMU permitida, el script usa `perf stat` con eventos
`L1-dcache-load-misses,LLC-load-misses`, inicia los contadores desactivados y los
habilita solo después de construir, validar y calentar el grafo. El driver envía
`enable`/`disable` y espera confirmación por FIFO. El script conserva los CSV
originales y calcula `1 - misses_optimized/misses_baseline`; exige ≥0.50.
Si el evento no está soportado, no hay permiso o el baseline da cero, no se
emite aprobación. Seleccionar un núcleo disponible; no se cambian permisos del
sistema automáticamente. Conviene repetir en condiciones controladas y conservar
todos los runs, no escoger el mejor. **No medido en este Mac: criterio pendiente.**

La comparación vieja/nueva incluye kernel, normalización, visitados, selección
y layout; no demuestra que solo FlatGraph causó una reducción de caché.
Para causalidad exclusiva del layout se requiere una ablación adicional.
La [documentación de perf stat](https://man7.org/linux/man-pages/man1/perf-stat.1.html)
describe el control por FIFO y los eventos de hardware; no sustituye la medición.

## Sanitizadores y compilación mínima

```sh
cmake -S . -B build-opt-asan -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON \
  -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ \
  -DAETHER_ENABLE_SANITIZERS=ON \
  -DAETHER_REAL_DATA_DIR="$PWD/datasets/real/dbpedia"
cmake --build build-opt-asan --parallel 4
ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build-opt-asan --parallel 2 --output-on-failure
cmake -S . -B build-opt-app -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build-opt-app --parallel 4
```

La ruta Clang corresponde al Mac verificado; en otros entornos GCC/Clang se
selecciona el compilador instalado. Windows/MSVC puede compilar el motor, pero
no se afirma validación real de ese entorno desde macOS.
La revisión del equipo y aprobación de PR son independientes de estas pruebas.

## Verificar el paquete de evidencia

Los artefactos del 19-10-2026 incluyen las muestras, logs completos, flags,
protocolos, hashes y los resultados iniciales conservados. Para recalcular
las métricas del paquete sin volver a medir:

```sh
python3 scripts/analyze_optimization_evidence.py --verify-current-source .
```

Esa opción exige que las fuentes actuales coincidan con la instantánea medida;
para auditar solo los archivos archivados se omite `--verify-current-source`.
El script verifica los hashes, recalcula la reducción media de Top-K, las mil
comparaciones de Recall agrupado y p99 desde los CSV, y conserva explícitamente
CA-2.3 como no medido. Los percentiles, incluido p50, usan nearest-rank.
Para recoger una nueva ejecución completa con las mismas carpetas de build se
usa `scripts/capture_optimization_evidence.py`; no ejecuta tests ni inventa su éxito.
