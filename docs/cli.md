# CLI mínima de AetherIndex

La CLI de Sprint 1 permite cargar embeddings proporcionados externamente y
ejecutar Top-K exacto con fuerza bruta o aproximado con HNSW. Toda la operación
ocurre en memoria dentro de un proceso; no se guardan índices ni datos entre
ejecuciones. No hay modo interactivo, modelos de embeddings, servidor ni
comparación automática de Recall@K dentro del comando de búsqueda. Los tests
de integración sí comparan HNSW contra fuerza bruta.

## Compilar y consultar

Desde la raíz del repositorio, con CMake 3.20+ y C++20:

```sh
cmake -S . -B build-cli -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build-cli --parallel 4
./build-cli/aetherindex --help
./build-cli/aetherindex search --dataset datasets/examples/vectors.csv --query datasets/examples/query.csv --algorithm brute-force --k 2
```

El ejemplo debe devolver los IDs 10 y 40, a distancias coseno 0 y aproximadamente
0.2, respectivamente. Las distancias se muestran siempre con **nueve decimales**:
`0.000000000` y `0.200000003`. Los IDs conservan su formato entero. La cantidad de
decimales es una decisión de presentación; la precisión del `float` no cambia y
`0.200000003` satisface `|resultado - 0.2| < 1e-5`.

Para euclidiana al cuadrado:

```sh
./build-cli/aetherindex search --dataset datasets/examples/vectors.csv --query datasets/examples/query.csv --algorithm brute-force --metric squared-euclidean --k 10
```

Este ejemplo devuelve cuatro vecinos, ya que `K > N`. El orden de los empates
queda sin especificar. Las métricas son `1 - cosine_similarity` y euclidiana al
cuadrado sin raíz; ambas ordenan de menor a mayor distancia.

En Visual Studio se utilizan los comandos del README y el ejecutable
`.\build\Debug\aetherindex.exe`; para Release, `.\build\Release\aetherindex.exe`.
Las pruebas de un generador de múltiples configuraciones requieren
`ctest --test-dir build -C Debug --output-on-failure`.

## Argumentos

| Argumento | Valores y comportamiento |
|---|---|
| `search` | Único comando de búsqueda |
| `--dataset FILE` | Ruta obligatoria del dataset |
| `--query FILE` | Ruta obligatoria del embedding de consulta |
| `--algorithm NAME` | Obligatorio: `brute-force` exacto o `hnsw` aproximado |
| `--k INTEGER` | Entero decimal positivo dentro del rango de `size_t`; por defecto 10 |
| `--metric NAME` | `cosine` (por defecto) o `squared-euclidean` |
| `--m INTEGER` | Solo HNSW: vecinos máximos en capas superiores, al menos 2; por defecto 16 |
| `--m0 INTEGER` | Solo HNSW: vecinos máximos en capa base, al menos M; por defecto 32 |
| `--ef-construction N` | Solo HNSW: amplitud de construcción, al menos M; por defecto 100 |
| `--ef-search N` | Solo HNSW: amplitud de consulta positiva; por defecto 50 |
| `--seed INTEGER` | Solo HNSW: semilla uint64, incluyendo cero; por defecto 42 |
| `--help` | Ayuda global o `search --help`; sin argumentos también muestra ayuda |

Las opciones con valor usan dos argumentos separados, por ejemplo `--k 10`;
no se admite `--k=10`. El orden de las opciones es libre. Las opciones
desconocidas o repetidas se rechazan. Las rutas con espacios deben ir entre
comillas. Para una ruta que comienza con `--`, utilizar una ruta absoluta o
el prefijo `./`. Los argumentos se validan antes de acceder a los archivos.

Para construir y consultar HNSW:

```sh
./build-cli/aetherindex search --dataset datasets/examples/vectors.csv --query datasets/examples/query.csv --algorithm hnsw --k 10
```

El comando carga el store, valida la consulta, construye un grafo con los IDs del
dataset y busca mediante `QueryEngine`. Devuelve `Algorithm: HNSW`, configuración,
tiempo de construcción y resultados ordenados. No usa fuerza bruta como fallback.

Ejemplo con parámetros explícitos:

```sh
./build-cli/aetherindex search --dataset datasets/examples/vectors.csv --query datasets/examples/query.csv --algorithm hnsw --k 2 --m 16 --m0 32 --ef-construction 100 --ef-search 100 --seed 42
```

Las opciones HNSW se rechazan con fuerza bruta en lugar de ignorarse. Se rechazan
valores con signo, fracciones, desbordamientos, parámetros repetidos y
configuraciones incompatibles antes de abrir los archivos. La consulta utiliza
`min(N, max(efSearch, K))`; una semilla y orden de inserción iguales producen la
misma construcción. Los empates HNSW se resuelven por ID. El índice se reconstruye
en cada proceso y puede devolver menos de K elementos si su grafo no permite
alcanzarlos. Ver [el contrato del algoritmo](algorithms/hnsw.md).

## Formato CSV

- Dataset: una fila `VectorId,x1,...,xD` por embedding, sin encabezado.
- Consulta: exactamente una fila no vacía `x1,...,xD`, sin identificador.
- Dimensión positiva, inferida de la primera fila del dataset; debe coincidir
  en todas las filas y en la consulta.
- IDs: dígitos decimales, de 0 a 4294967295, únicos; no se admiten signos.
- Componentes: `float` finitos en notación decimal o científica, con signo
  opcional y punto decimal, independientemente del locale del sistema.
- Se permiten espacios/tabulaciones alrededor de campos, líneas vacías y LF/CRLF.
- Se rechazan encabezados, comillas, comentarios, BOM, valores NaN/infinito,
  campos vacíos, comas finales y registros inválidos. No se omiten filas inválidas.
- Coseno rechaza embeddings y consultas completamente cero; euclidiana al
  cuadrado los acepta. No se normalizan ni cambian los datos proporcionados.
- Las rutas deben identificar archivos regulares legibles. No hay stdin ni
  separadores regionales distintos de coma y punto decimal.

La carga procesa una fila a la vez mediante `VectorStore::insert()`, sin una
segunda copia permanente del dataset. Si una fila falla, se descarta el store
local y no se presentan resultados parciales.

## Resultados y errores

Una búsqueda correcta presenta algoritmo, métrica, cantidad de vectores,
dimensión, K solicitado, cantidad devuelta, tiempo de consulta y una tabla ASCII
con `Rank`, `Vector ID`, `Distance`. Tiene bordes verticales y una línea horizontal
antes y después del encabezado y después de cada vecino. Las columnas amplían su
ancho según el contenido, incluso para el ID máximo y distancias muy grandes.

```text
+------+-----------+-------------+
| Rank | Vector ID | Distance    |
+------+-----------+-------------+
|    1 |        10 | 0.000000000 |
+------+-----------+-------------+
|    2 |        40 | 0.200000003 |
+------+-----------+-------------+
```

`Query time (ms)` usa seis decimales y mide tiempo de pared con un reloj monótono
alrededor de `QueryEngine::search`: cálculo de distancias, ordenamiento completo
y selección de vecinos. Excluye lectura/validación de CSV, inserción en el store,
formato y escritura de resultados. La salida también declara ese alcance. No es
el tiempo total del proceso ni una garantía de latencia; una sola medición varía
con la máquina, configuración y carga del sistema. Los decimales mostrados no
garantizan esa precisión de medición.

Con HNSW, `Index build time (ms)` mide por separado la construcción del grafo
después de cargar los CSV. Se excluye por completo de `Query time (ms)`. La salida
incluye M, M0, efConstruction, efSearch y semilla para repetir la configuración.
Los tiempos no incluyen persistencia; cada proceso construye nuevamente el índice.

El ranking del motor se conserva sin volver a ordenar o calcular distancias en
la CLI. La salida normal y la ayuda van a `stdout`; los errores van a `stderr`.

| Código | Significado | Ejemplos |
|---|---|---|
| 0 | Éxito o ayuda | Búsqueda, `--help`, sin argumentos |
| 2 | Argumentos o configuración inválidos | Falta `--dataset`, K cero, M menor que 2, efConstruction menor que M |
| 3 | Error de lectura o validación | Archivo inexistente/vacío, CSV inválido, ID duplicado, dimensión incompatible |
| 4 | Error de búsqueda o interno | Distancia euclidiana al cuadrado fuera del rango de float, fallo de asignación |

Los errores de registros indican ruta, número de línea y, para valores
numéricos inválidos, columna. Un resultado que excede el rango de `float` no se
convierte silenciosamente en infinito.

## Pruebas reproducibles

```sh
ctest --test-dir build-cli --output-on-failure
ctest --test-dir build-cli -R 'cli|search_service' --output-on-failure -V
```

Las pruebas se registran en CTest sin bibliotecas de testing ni dependencias
externas. `unit.cli_parser` valida opciones; `integration.search_service` prueba
carga y búsqueda usando el servicio; `e2e.cli_search` comprueba el ejecutable,
streams, códigos y errores; `e2e.cli_mvp` ejecuta diez procesos de búsqueda Top-10
sobre 1,000 embeddings de dimensión 128, con IDs no consecutivos y semilla 42.

La suite normal registra **16 tests**. Los cinco tests HNSW del núcleo cubren
nodos/niveles, inserción, búsqueda, invariantes y Recall@10 con 100 consultas
independientes por métrica. `e2e.cli_hnsw` ejecuta el algoritmo por la CLI sobre
1,000 × 128 con ambas métricas, parámetros y semilla reproducible. Sus consultas
de cableado son vectores del dataset; la aceptación de recall se evalúa con
consultas independientes en `integration.hnsw_recall`.

El generador sintético usa `mt19937` y un mapeo explícito de sus enteros para
producir los mismos componentes entre bibliotecas estándar. Guarda floats con
precisión suficiente para recuperarlos. Las diez consultas son embeddings
distintos del dataset, con un vecino exacto conocido; esto valida el flujo y
los IDs, sin presentarse como evaluación de relevancia o rendimiento.

Para generar el dataset y repetir una consulta manualmente:

```sh
./build-cli/tests/aether_cli_fixture_generator build-cli/manual-mvp
./build-cli/aetherindex search --dataset build-cli/manual-mvp/vectors.csv --query build-cli/manual-mvp/query_0.csv --algorithm brute-force --k 10
```

Los archivos son `vectors.csv` y `query_0.csv` hasta `query_9.csv`. El primer
vecino esperado de `query_i` es el ID `7 * 97 * i + 10`, con distancia coseno
menor que `1e-5`. El ejecutable auxiliar se construye con `BUILD_TESTING=ON`;
en Visual Studio está en `build/tests/Debug/`.

Para comprobar Release y una compilación sin tests:

```sh
cmake -S . -B build-cli-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build-cli-release --parallel 4
ctest --test-dir build-cli-release --output-on-failure
cmake -S . -B build-cli-no-tests -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build-cli-no-tests --parallel 4
./build-cli-no-tests/aetherindex --help
```

Sanitizers en GCC/Clang compatibles:

```sh
cmake -S . -B build-cli-sanitizers -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DAETHER_ENABLE_SANITIZERS=ON
cmake --build build-cli-sanitizers --parallel 4
ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ctest --test-dir build-cli-sanitizers --output-on-failure
```

En este Mac, el runtime ASan de AppleClang se bloqueaba antes de `main`.
La validación usa Clang de Homebrew agregando
`-DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++` al configurar una carpeta
nueva. Consultar [la evidencia de la tarjeta](architecture/cli_validation.md).
Los tiempos de CTest incluyen preparación y comprobaciones; no son benchmarks.

## Pruebas con artículos reales

El escenario ampliado usa 10,000 artículos DBpedia con embeddings reales de
dimensión 1,024 y 20 artículos de consulta excluidos del corpus. Conserva los
mismos formatos de dataset y consulta CSV. Consultar [el caso real reproducible](real_world_validation.md)
para preparar los datos, ejecutar una consulta y activar la comparación exhaustiva
independiente en CTest. La descarga es explícita; la suite normal sigue sin red
ni dependencias externas.

## Integración

```text
main → cli_parser / cli_runner → SearchService
                                  ├── lector CSV interno → VectorStore
                                  ├── construcción → HnswIndex
                                  └── QueryEngine → BruteForce / HNSW → Distance
```

`aether_cli` depende de `aether_application`; ningún módulo del motor depende
de la CLI. `SearchService` es reutilizable por otras interfaces: recibe rutas,
algoritmo, métrica y K; devuelve metadata y vecinos, sin imprimir ni terminar
el proceso. Posee el store únicamente durante la llamada. `QueryEngine` conserva
su contrato de búsqueda sobre un `const VectorStore&` y una consulta `span`.
La sobrecarga de `QueryEngine` recibe un `const HnswIndex&` ya construido.
`SearchService` coordina el store, el grafo y los tiempos; la CLI solo conoce
solicitudes, configuración y resultados públicos. Consultar
[la auditoría y validación de HNSW](architecture/hnsw_validation.md).
