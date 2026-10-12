# VectorStore y distancias escalares

## Tipos y propiedad

`VectorId` es `std::uint32_t`; `EmbeddingValue` es `float` de 32 bits.
`DistanceKind` contiene `Cosine` y `SquaredEuclidean`. Estos conceptos viven en
core, evitando dependencias entre storage y distance.

`VectorStore(dimension, metric = DistanceKind::Cosine)` exige dimensión positiva
y una métrica soportada, o lanza `std::invalid_argument`. Ambas propiedades son
constantes durante la vida de la instancia. El store no se copia ni se mueve.

Es propietario de un arreglo contiguo `std::vector<float>`, un vector de IDs en
orden de inserción y un mapa ID → posición. Acepta todo el rango de `VectorId`.
`size()`, `dimension()`, `distance_kind()` e `ids()` son consultas `noexcept`.
`contains(id)` consulta existencia y `find(id)` devuelve
`std::optional<std::span<const float>>`: un ID inexistente produce `nullopt`.

`ids()` devuelve `std::span<const VectorId>`. Un consumidor puede recorrerlo y
obtener cada embedding con `find()`, sin acceder a estructuras privadas.

## Inserción y errores

`insert(id, embedding)` recibe una vista de solo lectura y devuelve:

| Estado | Significado |
|---|---|
| `Success` | El embedding y su ID se incorporaron |
| `DimensionMismatch` | La dimensión no coincide |
| `DuplicateId` | El ID ya existe; su embedding se conserva |
| `InvalidEmbedding` | Hay NaN/infinito, o un vector de norma cero en un store coseno |

La validación comprueba dimensión, ID duplicado y contenido, en ese orden.
Los stores euclidianos permiten vectores de ceros; ambos rechazan NaN, +∞ y −∞.
La comprobación de norma cero no usa epsilon: un componente finito subnormal
no nulo basta para que un embedding sea admisible para coseno.

Antes de ampliar el almacenamiento se crea una copia temporal del embedding.
Esto permite insertar directamente una vista obtenida del mismo store. Después
se añaden componentes e ID; el mapa se confirma al final. Si una asignación
falla, se restauran las longitudes anteriores y se propaga la excepción
(`std::bad_alloc` o errores de longitud), conservando todos los registros.
La copia temporal desaparece al terminar; no existe una segunda colección
permanente de embeddings.

## Vida útil de las vistas

Las vistas no poseen memoria. Descartar cualquier vista devuelta por `find()` o
`ids()` antes de insertar, aunque la inserción falle: una ampliación seguida de
rollback puede cambiar la dirección de los datos. Pasar una vista directamente
a `insert()` está permitido, pero no utilizarla después de esa llamada.

Destruir el store invalida todas sus vistas. Las lecturas requieren ausencia de
modificaciones simultáneas; esta versión es secuencial, sin garantías de
concurrencia. Los consumidores conservan IDs y adquieren vistas nuevas cuando
las necesitan.

## Distancias

```cpp
float compute_distance(std::span<const EmbeddingValue> a,
                       std::span<const EmbeddingValue> b,
                       DistanceKind metric);
```

- **Coseno:** `1 - dot(a,b)/(norm(a)*norm(b))`, en `[0, 2]`. Misma dirección da
  `0`, ortogonalidad `1`, dirección opuesta `2`. No normaliza ni modifica datos.
- **Euclidiana al cuadrado:** suma de diferencias al cuadrado, sin raíz cuadrada.
  Menores valores representan mayor proximidad en ambas opciones.

Se acumula con `double` y se devuelve `float`. La similitud coseno se limita a
`[-1, 1]` para compensar redondeo. Dimensiones vacías o diferentes, componentes
no finitos, métricas no soportadas y norma cero con coseno lanzan
`std::invalid_argument`. Un resultado euclidiano mayor que `float::max()` lanza
`std::overflow_error`; nunca se devuelve infinito por ese desbordamiento.
Resultados positivos menores que el rango representable de float pueden
redondearse a cero. Estas funciones son escalares y no implementan SIMD.

## Integración y futuro HNSW

```cpp
VectorStore store(3);  // Coseno por defecto.
const std::array<float, 3> a = {1.0f, 0.0f, 0.0f};
const std::array<float, 3> b = {0.0f, 1.0f, 0.0f};
// Comprobar los estados de inserción antes de continuar.
store.insert(10, a);
store.insert(20, b);
const auto lhs = store.find(10);
const auto rhs = store.find(20);
// Tras confirmar que ambas vistas existen:
const float distance = compute_distance(*lhs, *rhs, store.distance_kind()); // 1
```

`bruteForceSearch` y `QueryEngine::search` reciben `const VectorStore&`, una vista
de consulta y `k`. El store determina la métrica; no hay una segunda configuración
que pueda contradecirla. El llamador controla la vida del store.

Un futuro nodo HNSW puede guardar solo `VectorId`, consultar `find(id)` y enviar
esa vista a `compute_distance`. No conserva spans durante inserciones ni copia
embeddings dentro del grafo. La prueba de este patrón valida un contrato público;
no representa una implementación o prueba funcional de HNSW.

## Compilar y probar

Desde la raíz, con un compilador C++20:

```sh
cmake -S . -B build-local -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build-local --config Debug
ctest --test-dir build-local -C Debug --output-on-failure
```

En macOS/Linux con generador de configuración única: `./build-local/aetherindex`.
Con Visual Studio: `.\build-local\Debug\aetherindex.exe`.
Usar una carpeta nueva si cambia el compilador, generador o ubicación del proyecto.

Para activar AddressSanitizer y UndefinedBehaviorSanitizer con GCC/Clang compatibles:

```sh
cmake -S . -B build-sanitizers -DCMAKE_BUILD_TYPE=Debug -DAETHER_ENABLE_SANITIZERS=ON
cmake --build build-sanitizers --config Debug
ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-sanitizers --output-on-failure
```

La opción vale `OFF` por defecto. Un compilador no soportado produce un error de
configuración al solicitarla. En el entorno validado, el runtime de AppleClang 17
se bloqueó antes de `main`, incluso con un programa mínimo; la ejecución completa
con ASan/UBSan funcionó usando el Clang 23 ya instalado. Para reproducirla en este
Mac, utilizar una carpeta nueva y seleccionar explícitamente ese compilador:

```sh
cmake -S . -B build-llvm-sanitizers -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ -DCMAKE_BUILD_TYPE=Debug -DAETHER_ENABLE_SANITIZERS=ON
cmake --build build-llvm-sanitizers
ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-llvm-sanitizers --output-on-failure
```

No se cambia el compilador predeterminado del proyecto. Para compilar solo la aplicación:

```sh
cmake -S . -B build-app -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=OFF
cmake --build build-app --config Debug
```

CTest cubre siete ejecutables/escenarios sin dependencias externas. Incluye
recuperación exacta de 1,000 embeddings de dimensión 128 (semilla 42), inyección
aislada de fallas de asignación, cálculo independiente con long double, ranking
con ambas métricas y salida completa de la CLI. La tolerancia absoluta inicial
para las distancias de referencia es `1e-5`; el caso de gran magnitud usa una
comparación relativa explícita.

Ver [Evidencia de validación](../architecture/vector_store_validation.md).
La aprobación del equipo y la revisión de PR son pasos separados del resultado
de estas pruebas.
