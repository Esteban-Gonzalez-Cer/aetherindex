# VectorStore, preparación y kernels FP32

## Propiedad y contrato

`VectorStore(dimension, metric=Cosine)` exige dimensión positiva y métrica
soportada. No se copia ni se mueve. Es propietario de un `AlignedVector<float>`,
un vector de IDs en orden de inserción y un mapa ID → posición densa.
Storage y distance dependen únicamente de core y de la biblioteca estándar.

`insert(id, span)` devuelve `Success`, `DuplicateId`, `DimensionMismatch` o
`InvalidEmbedding`. Admite cero y todo el rango uint32 de IDs externos.
Rechaza componentes no finitos. **Cosine normaliza L2 antes de almacenar y
rechaza norma < 1e-7**; SquaredEuclidean conserva exactamente las componentes
FP32 originales y permite ceros. Esta normalización sustituye deliberadamente
el contrato inicial de conservación de magnitud en stores Cosine.

Se valida antes de asignar. Se preparan los componentes en un buffer temporal
antes de cualquier reasignación del store, permitiendo insertar una vista del
mismo almacén. Si una asignación falla, se restauran las longitudes de datos
e IDs; el mapa se confirma al final. Se propaga la excepción con el contenido
lógico anterior, aunque las capacidades y direcciones pueden haber cambiado.
El span recibido nunca se modifica.

## Alineación y acceso público

La asignación usa `operator new(..., align_val_t{64})` y su delete correspondiente.
El stride físico es `(D+15)&~size_t(15)`, en floats. Cada fila empieza en un
múltiplo de 64 bytes y su padding contiene ceros. Por ejemplo, D=17 usa stride=32,
pero `dimension()` y los spans de `find()` siguen mostrando 17 componentes.

| API | Uso |
|---|---|
| `size()`, `dimension()`, `distance_kind()` | Configuración y cantidad lógica |
| `find(id)` | `optional<span<const float>>` de dimensión lógica; nullopt si no existe |
| `ids()` | IDs externos en orden de inserción, sin copia |
| `position_of(id)` | Traducción a posición densa; nullopt si no existe |
| `data_by_position(position)` | Puntero de lectura a una fila; exige position < size() |
| `padded_dimension()` | Longitud física que los kernels pueden leer, incluido padding |
| `reserve(count)` | Reserva para una carga cuyo tamaño se conoce |

HNSW usa posiciones densas en sus aristas y convierte a IDs externos al entregar
resultados. Nunca convierte directamente un ID externo en un offset. Los
punteros se obtienen de nuevo durante cada operación: no se guardan a través
de inserciones. El store debe vivir más tiempo que el índice.

Descartar todas las vistas y punteros antes de `insert()` o `reserve()`, incluso
si fallan. Destrucción también los invalida. Lecturas y escrituras no pueden
solaparse; esta versión no ofrece concurrencia.

## Normalización estable

La preparación encuentra la mayor componente absoluta y divide por ella antes
de acumular cuadrados. Ocho sumas parciales FP32/FMA evitan formar una norma
que desborde con componentes grandes o se anule con subnormales. Se calcula
una raíz durante preparación. La comparación del umbral evita multiplicar
una escala enorme por la norma escalada. No hay acumuladores double en
normalización ni en distancias de producción.

`PreparedQuery` posee un buffer alineado e inmutable; no se copia, asigna ni mueve. `QueryEngine` lo construye
una vez por consulta y llama a las entradas preparadas de los algoritmos.
Cosine requiere una consulta finita y no nula; el umbral 1e-7 se aplica a
**inserciones**, no a consultas ni al API general de distancia.

## Dos niveles de API de distancia

`compute_distance(span a, span b, metric)` acepta componentes crudas: valida,
prepara ambas entradas y llama al kernel. No altera los spans del llamador.
Se conserva para consumidores generales; no se llama por candidato en búsquedas.

Los kernels internos requieren iguales longitudes válidas, componentes finitas,
padding cero y, para Cosine, entradas unitarias:

- Cosine: `1.0f - clamp(dot(a,b), -1.0f, 1.0f)`, dentro de [0,2].
- SquaredEuclidean: suma de diferencias al cuadrado, sin raíz; un resultado
  no finito por desbordamiento de FP32 lanza `overflow_error`.

Las sumas son FP32/FMA con varias cadenas independientes. AVX2/FMA usa registros
de ocho floats, NEON/AArch64 de cuatro; otras plataformas usan fallback escalar
con `std::fma`. Las colas no leen fuera de los arrays. El archivo
`scalar_distance.cpp` contiene los kernels; `distance.cpp`, el API validado.
No hay raíces, divisiones ni conversiones float→double en el bucle de comparación.
La similitud se limita antes de restar para compensar redondeo.

Errores del API general: dimensiones diferentes o vacías, no finitos, métrica
inválida o norma cero Cosine → `invalid_argument`. Se aceptan normas no nulas
muy pequeñas en este API. La acumulación FP32 cambia los últimos bits frente
al baseline double: el ejemplo 0.2 se imprime `0.199999988`; nueve decimales
son presentación, no nueve decimales de precisión garantizada.

## Compilación y comprobación

```sh
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build-debug --parallel 4
ctest --test-dir build-debug --output-on-failure
```

`AETHER_NATIVE_SIMD=ON` permite compilar los kernels para la CPU de construcción
(`-march=native` en GCC/Clang, `/arch:AVX2` en MSVC x64). Es optativo: ese binario
puede no ejecutarse en otras CPUs. AArch64 tiene el kernel NEON sin esta opción.
No hay dispatch de instrucciones en tiempo de ejecución.

Los sanitizadores son optativos mediante `AETHER_ENABLE_SANITIZERS=ON`.
En el Mac validado se usa el Clang 23 instalado porque el runtime AppleClang 17
presentó previamente un bloqueo antes de main:

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON \
  -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ -DAETHER_ENABLE_SANITIZERS=ON
cmake --build build-asan --parallel 4
ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-asan --output-on-failure
```

La suite normal contiene 19 pruebas sin dependencias externas. Dos pruebas
reales opcionales usan los datos DBpedia preparados y Python/NumPy. Se cubren
padding y alineación, normalización contra una referencia independiente,
umbral de norma, entradas extremas, alias, rollback en asignaciones normales
y alineadas, y recuperación exacta de 1,000×128 en SquaredEuclidean. El protocolo
OKR v2 verifica las 128,000 componentes de la representación normalizada Cosine.
`unit.optimization_contracts` exige error absoluto coseno **estrictamente <1e-6**
frente a una referencia tradicional long double. Los casos iniciales T2.5
mantienen su tolerancia absoluta estricta <1e-5; no se deduce rendimiento de CTest.
