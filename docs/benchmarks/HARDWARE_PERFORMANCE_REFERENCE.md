# Análisis de condiciones de rendimiento y hardware disponible

**Tarjeta:** Analizar condiciones de rendimiento y hardware disponible

**Proyecto:** AetherIndex · **Revisión local:** 21 de octubre de 2026

**Referencia:** `REF-M4PRO-A17-RELEASE-NATIVE-20261021`

## 1. Resultado del análisis

El equipo local permite establecer una referencia **ARM64 nativa con NEON/FMA**, usando un solo hilo de búsqueda, C++20 y compilación Release. La referencia principal se fija con AppleClang 17 y sanitizadores desactivados; Homebrew Clang se trata como otro perfil, con resultados separados.

La compilación limpia de referencia terminó y pasó **19 de 19 pruebas de corrección**. Se confirmó la presencia de instrucciones SIMD en el objeto de distancias de esa compilación y se ejecutaron casos conocidos con tres variantes de compilación.

**Esta tarjeta no publica nuevas latencias HNSW ni una mejora porcentual SIMD.** La sesión tuvo actividad de memoria y no se preparó como una sesión de rendimiento en reposo. Se establece el inventario, la configuración reproducible y las condiciones que deberán cumplir las siguientes mediciones.

Dos conclusiones necesarias para interpretar futuros resultados:

- En ARM64, `AETHER_NATIVE_SIMD=OFF` **también utiliza NEON**. Comparar ON contra OFF no representa una comparación SIMD contra escalar.
- El tiempo de consulta sobre un índice preparado, el tiempo de construir HNSW y la duración completa del comando CLI son métricas distintas.

La [evidencia de entorno](hardware-reference/2026-10-21/environment.json), las [pruebas de compilador/kernel](hardware-reference/2026-10-21/compiler-probes.json) y el [log de CTest](hardware-reference/2026-10-21/ctest-release.log) acompañan este análisis.

## 2. Hardware y herramientas observados

### 2.1 Equipo de referencia

| Propiedad | Valor observado | Consecuencia |
|---|---|---|
| Modelo | MacBook Pro, identificador `Mac16,7` | Referencia local de portátil; registrar cambios de equipo. |
| Procesador | Apple M4 Pro | Arquitectura ARM64; no es una plataforma de ejecución AVX2. |
| Núcleos | 14 físicos y 14 lógicos; 10 de rendimiento y 4 de eficiencia | Una consulta actual usa un hilo; tener 14 núcleos no implica que esa consulta use los 14. |
| Memoria física | 25,769,803,776 bytes = 24 GiB | Registrar memoria disponible, compresión y swap en cada sesión; la capacidad instalada no expresa su disponibilidad instantánea. |
| ISA SIMD expuesta | `hw.optional.neon = 1` | Los kernels ARM64 pueden utilizar NEON; se verificó también su ejecución. |
| Línea de caché reportada | 128 bytes | El alineamiento de 64 bytes del store es un contrato diferente; no equivale a afirmar que la línea del M4 mide 64 bytes. |
| Tamaño de página | 16,384 bytes | Importa al interpretar estadísticas de memoria; no asumir páginas de 4 KiB. |
| Sistema | macOS 26.5.2, build `25F84`; Darwin 25.5.0 | Registrar versión y build del sistema en cada comparación. |
| Arquitectura del ejecutable nuevo | Mach-O `arm64` | Se validó ejecución nativa del binario ARM64; no se utilizó un ejecutable x86 traducido para esta referencia. |
| GPU/Neural Engine | No utilizados por los módulos actuales | Sus capacidades no se incluyen como aceleración de HNSW ni de distancias. |

La ficha de Apple del [MacBook Pro de 16 pulgadas de 2024](https://support.apple.com/en-gb/121554) describe la configuración M4 Pro de 14 núcleos y un ancho de banda de memoria publicado de 273 GB/s. Ese ancho de banda es una **especificación del fabricante**, no una medición de AetherIndex ni una velocidad efectiva garantizada para accesos aleatorios al grafo.

No se ha medido la frecuencia sostenida del procesador, el ancho de banda efectivo, las tasas de aciertos de caché ni la latencia del almacenamiento. No se asignan valores de GHz o de LLC deducidos del nombre del chip.

### 2.2 Toolchains disponibles y comprobadas

| Elemento | Valor observado | Alcance de la comprobación |
|---|---|---|
| Compilador principal | Apple clang 17.0.0, `clang-1700.6.3.2` | Build completo Release y CTest. |
| Ruta principal | `/Library/Developer/CommandLineTools/usr/bin/clang++` | Fijada explícitamente al configurar el build nuevo. |
| Compilador alternativo | Homebrew Clang 23.1.3 | Compilación/ejecución de los casos de distancia y revisión de ensamblador; no se recompiló toda la suite con él en esta tarjeta. |
| Ruta alternativa | `/opt/homebrew/opt/llvm/bin/clang++` | Registrar la versión, porque la ruta Homebrew puede apuntar a una versión nueva tras una actualización. |
| Biblioteca estándar de cabeceras | `_LIBCPP_VERSION = 200100` con AppleClang y `230103` con Homebrew | Cambiar compilador también puede cambiar cabeceras y generación de código; no atribuir cualquier diferencia solamente a SIMD. |
| Biblioteca enlazada por la CLI | `/usr/lib/libc++.1.dylib` | Enlace observado mediante `otool -L`; se conserva en el manifiesto. |
| CMake | 4.4.4 | Versión usada para esta referencia. |
| Generador nuevo | Ninja 1.13.2 | Los builds históricos revisados usan Unix Makefiles; no se reutilizaron sus cachés. |
| Python de la sesión | 3.13.5 | El `/usr/bin/python3` del sistema es 3.9.6; las utilidades que requieren Python ≥3.10 deben usar el intérprete adecuado. |
| SDK | macOS 26.2, ruta resuelta `MacOSX26.2.sdk` | Es diferente de la versión del SO en ejecución. |
| Directorio de desarrollo activo | `/Library/Developer/CommandLineTools` | La disponibilidad de un stub `/usr/bin/xctrace` no implica que Instruments esté operativo. |

Al invocar directamente el AppleClang de Command Line Tools sin SDK explícito, las primeras pruebas no encontraron cabeceras como `<algorithm>`. Añadir `-isysroot` o definir `SDKROOT` resolvió esa invocación; CMake ya había añadido el SDK correctamente. La [documentación de `CMAKE_OSX_SYSROOT`](https://cmake.org/cmake/help/v4.4/variable/CMAKE_OSX_SYSROOT.html) explica esta selección. Conservar SDK y comando efectivo forma parte del entorno de referencia.

## 3. Perfiles de compilación

| Perfil | Uso | Configuración y estado |
|---|---|---|
| **R1: referencia de rendimiento ARM64** | Futuras mediciones locales comparables | AppleClang 17, Release, `AETHER_NATIVE_SIMD=ON`, sanitizadores OFF. Build y 19 pruebas comprobados. |
| **R2: ARM64 sin opciones nativas adicionales** | Aislar el efecto de `-march=native` | Mismo compilador, Release, `AETHER_NATIVE_SIMD=OFF`. Se comprobó el kernel; sigue siendo NEON. Para medir HNSW con R2 se necesita su build completo separado. |
| **R3: compilador alternativo** | Estudiar sensibilidad al compilador | Homebrew Clang 23.1.3, Release y NEON. Comprobado solo el kernel en esta tarjeta; rendimiento completo pendiente. |
| **D: diagnóstico** | Desarrollo y depuración | Debug, no mezclar sus tiempos con Release. |
| **S: sanitizadores** | Detectar errores de memoria/comportamiento indefinido | Build separado con `AETHER_ENABLE_SANITIZERS=ON`; no usar sus tiempos para aprobar latencia de R1. No se revalidaron runtimes de sanitizadores en esta tarjeta. |
| **X: x86-64** | Validación de AVX2/FMA y portabilidad | No hay ejecución x86 validada en este equipo; queda pendiente disponer de otra máquina. |

El perfil R1 nuevo utiliza:

```text
CMAKE_BUILD_TYPE=Release
CMAKE_CXX_STANDARD=20
CMAKE_CXX_COMPILER=/Library/Developer/CommandLineTools/usr/bin/clang++
CMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG
CMAKE_OSX_ARCHITECTURES=arm64
CMAKE_OSX_DEPLOYMENT_TARGET=26.5
AETHER_NATIVE_SIMD=ON
AETHER_ENABLE_SANITIZERS=OFF
AETHER_BUILD_BENCHMARKS=ON
AETHER_REGISTER_PERFORMANCE_TESTS=OFF
BUILD_TESTING=ON
```

El deployment target 26.5 fija **esta referencia local**; no establece un nuevo sistema operativo mínimo del producto. El SDK efectivo es 26.2. Se conservaron los comandos reales de compilación en el manifiesto; no basta con anotar únicamente «Release».

`-march=native` se aplica de forma privada a **`aether_distance`**, según el [CMake actual](../../CMakeLists.txt). No se aplica a todos los módulos. En esta referencia no se habilitaron LTO, PGO ni `-ffast-math`.

Para Ninja/Makefiles se debe fijar `CMAKE_BUILD_TYPE`; CMake documenta que su valor inicial puede quedar vacío. Los generadores multiconfiguración, como Visual Studio, seleccionan la configuración al construir y ejecutar las pruebas. Referencia: [`CMAKE_BUILD_TYPE`](https://cmake.org/cmake/help/v4.4/variable/CMAKE_BUILD_TYPE.html).

Los sanitizadores instrumentan el programa y alteran su ejecución; [Clang documenta el coste de ASan](https://clang.llvm.org/docs/AddressSanitizer.html). En el historial del proyecto hubo una incidencia de inicialización con AppleClang ASan y se utilizó Homebrew Clang para esas comprobaciones; no debe tratarse como una comprobación actual del runtime recién instalado. Consultar la [evidencia histórica de optimización](../reports/OPTIMIZACION_VECTORIAL_FLATGRAPH.md) y ejecutar la suite correspondiente cuando se vaya a certificar el perfil S.

## 4. Qué SIMD está implementado y qué se verificó

La selección actual está en [los kernels de distancia](../../src/distance/scalar_distance.cpp) y se decide **al compilar**:

| Condición de compilación | Rama del código actual |
|---|---|
| AVX2 y FMA habilitados; condición equivalente prevista para MSVC | Intrinsics AVX2/FMA FP32. |
| ARM64 (`__aarch64__` o `_M_ARM64`) | Intrinsics NEON/FMA FP32. |
| Resto | Bucle de acumulación FP32 con `std::fma`; el compilador aún puede autovectorizar. |

No existe un dispatcher general de capacidades de CPU en tiempo de ejecución ni una opción actual que fuerce una referencia puramente escalar en ARM64. Desactivar la autovectorización tampoco elimina intrinsics NEON escritos explícitamente.

NEON procesa cuatro valores FP32 por vector de 128 bits; el kernel actual utiliza cuatro acumuladores y avanza 16 componentes por iteración principal. Esto describe el trabajo por iteración, **no una aceleración garantizada de 4× o 16×**. Referencia del formato vectorial: [Arm, Coding for Neon](https://developer.arm.com/-/media/Arm%20Developer%20Community/PDF/Neon%20Programmers%20Guide/102159_0104_01_CodingForNeon.pdf?revision=a3235e10-ded9-4ac8-a415-badcafdd56a6).

### Evidencia nueva

Se compilaron y ejecutaron arrays de 128 componentes con dos vectores unitarios ortogonales. Los resultados esperados eran coseno-distancia `1` y distancia euclidiana al cuadrado `2`:

| Variante | Backend declarado en ejecución | Resultados | FMA vectorial en ensamblador auxiliar |
|---|---|---|---:|
| AppleClang, sin `-march=native` | `NEON/FMA FP32` | Correctos | 8 instrucciones estáticas |
| AppleClang, con `-march=native` | `NEON/FMA FP32` | Correctos | 8 instrucciones estáticas |
| Homebrew Clang, con `-march=native` | `NEON/FMA FP32` | Correctos | 8 instrucciones estáticas |

Además, el [objeto real del build R1](hardware-reference/2026-10-21/reference-object.json) contiene ocho instrucciones `fmla.4s` y dos `fmadd` escalares para las colas. El [desensamblado](hardware-reference/2026-10-21/reference-object.s) y los reportes de las tres variantes se conservaron. El contador admite las notaciones `fmla.4s v0` y `fmla v0.4s`; no confunde un cambio de sintaxis con ausencia de SIMD.

Estos recuentos son de instrucciones presentes en el código, no de instrucciones ejecutadas por consulta ni de utilización del procesador. La comprobación confirma selección, emisión de instrucciones y casos conocidos; no reemplaza las pruebas completas de distancias o una comparación de latencia.

### Condiciones de una futura afirmación de mejora SIMD

1. Añadir una referencia escalar controlada al benchmark de distancias, con semántica, precisión FP32 y preprocesamiento comparables. No cambiar a double y atribuir toda la diferencia a SIMD.
2. Identificar si la referencia puede autovectorizar; inspeccionar el código generado. Registrar el uso de FMA y el orden de reducción, que pueden introducir diferencias de redondeo.
3. Medir sobre los mismos bytes, dimensiones, alineamiento, métrica y número de comparaciones, fuera de carga/normalización de datos cuando se mida solo el kernel.
4. Conservar checksum de resultados para impedir que el compilador elimine el trabajo; analizar varias ejecuciones y alternar el orden de las variantes.
5. Publicar después la repercusión en HNSW por separado. Si cambia el redondeo y con ello la construcción del grafo, comprobar la topología/recall o declarar que se comparan configuraciones completas, sin atribuir todo el resultado al kernel.

En x86, `AETHER_NATIVE_SIMD=ON` solicita instrucciones del host con GCC/Clang; el CMake actual usa `/arch:AVX2` con MSVC x64. Los binarios deberán ejecutarse en CPUs compatibles con las instrucciones emitidas y comprobar FMA cuando lo utilice la rama seleccionada. GCC explica la diferencia entre [seleccionar ISA con `-march` y ajustar con `-mtune`](https://gcc.gnu.org/onlinedocs/gcc/x86-Options.html). Ni una compilación cruzada ni un ensamblador AVX2 acreditan ejecución o rendimiento en otra CPU.

`-ffast-math` queda fuera de la referencia porque permite suposiciones sobre NaN, infinitos y operaciones de coma flotante que afectan los contratos de validación del motor. No es un requisito para usar intrinsics FMA. Referencia: [Clang, comportamiento de punto flotante](https://clang.llvm.org/docs/UsersManual.html#controlling-floating-point-behavior).

## 5. Condiciones de ejecución que deben controlarse

| Condición | Cómo puede alterar el resultado | Regla para las mediciones |
|---|---|---|
| Núcleos P/E y planificador | Una consulta puede compartir CPU o cambiar de núcleo | Un hilo, proceso nativo, ejecución en primer plano y política/QoS registrados. No afirmar afinidad a un núcleo P si no se verificó. |
| Frecuencia, energía y temperatura | El rendimiento sostenido puede variar entre sesiones | Corriente conectada, modo de energía constante y evidencia antes/después. Permitir estabilización tras compilar. |
| Otros procesos | Competencia por CPU, caché, memoria e I/O | No medir mientras corren builds, tests, descargas o cargas pesadas. Registrar observaciones del sistema; no cambiar ni cerrar aplicaciones automáticamente. |
| Memoria y swap | Paginación y compresión pueden introducir pausas | Registrar deltas de swap/pageins y memoria; evitar actividad de swap durante la ventana de referencia. La memoria libre aislada y el swap usado acumulado no bastan para diagnosticar presión. |
| Cachés y conjunto de trabajo | Repetir consultas puede mantener calientes rutas, embeddings y páginas | Publicar si se mide caliente, proceso nuevo o condiciones frías controladas. Un proceso nuevo no garantiza caché fría del SO. |
| N, D, métrica y distribución | Cambian bytes leídos, comparaciones y navegación | Mismos archivos/checksums y representación FP32; indicar si los vectores están normalizados. |
| HNSW y calidad | Reducir `efSearch` puede reducir tiempo y recall | Congelar configuración y reportar recall respecto a fuerza bruta junto a latencia. |
| K solicitado | Afecta candidatos, ordenamiento y resultados | Mantener K idéntico al comparar. No comparar K=10 con K=100 como si fuera la misma carga. |
| Asignación de memoria | Reservas y workspaces pueden influir en las colas de latencia | Conservar la estrategia del código y declarar si las asignaciones dentro de `QueryEngine` permanecen en el timer. |
| Instrumentación | Debugger, sanitizadores y profilers añaden trabajo | Medir Release sin ellos; obtener trazas diagnósticas en sesiones separadas. |
| Paralelismo | Varias consultas compiten por recursos | La referencia usa una consulta a la vez. `--parallel 4` al compilar no configura hilos de búsqueda. |

### Estado observado durante esta tarjeta

- Alimentación de corriente; modo de bajo consumo reportado como desactivado.
- `ProcessInfo` devolvió estado térmico nominal (`0`). Es una instantánea: el SDK también documenta ese valor para estado desconocido/no soportado; no prueba temperatura, frecuencia fija ni ausencia de throttling durante otra ejecución.
- Una lectura de `vm.swapusage` mostró 7,367.06 unidades `M` usadas. Ese valor acumulado por sí solo no demuestra paginación activa.
- En una muestra de dos segundos de `vm_stat` se observaron **12 páginas de swap-in**, cero de swap-out, 514 pageins y un pageout. Con páginas de 16 KiB, los swap-ins equivalen a 196,608 bytes reportados en ese intervalo. Los contadores no atribuyen esos accesos a AetherIndex.
- Las medias de carga de la captura fueron aproximadamente `4.00 / 5.02 / 5.38`. No se certificó reposo del equipo ni se fijó afinidad.

Por ello, esta sesión se conserva como **inventario, diagnóstico y corrección**, sin iniciar un benchmark HNSW que se presentara como referencia controlada. No se cambiaron políticas de energía ni se cerraron procesos del usuario.

## 6. Datos, memoria y parámetros del entorno de referencia

### 6.1 Cargas disponibles

Se conserva el [protocolo de optimización existente](../../benchmarks/optimization_protocol.json), sin modificar sus fixtures ni sus umbrales históricos:

| Carga | Parámetros del driver actual | Uso y límite |
|---|---|---|
| DBpedia | 10,000 vectores × 1,024D; coseno; K=10; M=16; M0=32; `efConstruction=100`; `efSearch=50`; semilla 42; selección heurística | Referencia local de consulta HNSW sobre índice preparado. |
| Consultas DBpedia | 20 distintas, fuera del catálogo; 100 búsquedas de calentamiento y 1,000 medidas, repitiendo las 20 consultas 50 veces | Prueba repetible de caché caliente; no son 1,000 necesidades independientes ni tráfico de producción. |
| Top-K | 100,000 vectores × 128D; K=10; 50 muestras por variante | Comparación de ordenamiento completo frente a heap con el mismo kernel; no aísla SIMD. |
| Clusters | 10,000 vectores × 128D, 1,000 consultas, `efSearch=30` | Contrastar selección de vecinos y recall; no es el mismo workload que DBpedia. |
| Memoria del grafo | 100,000 nodos, embeddings de 8D, M=16 y M0=32 | Contabilidad de topología y RSS por separado; no mide latencia de un corpus de 128D/1,024D. |

El corpus real ya está preparado en el equipo. En esta tarjeta se conservó el hash de su manifiesto; no se revalidaron todos sus archivos ni se ejecutó su benchmark. Antes de una nueva medición debe comprobarse la integridad completa de los archivos declarados en el manifiesto.

El driver DBpedia tiene esos parámetros fijados en su código. Un barrido de `efSearch`, M o dimensiones requerirá otro protocolo/driver parametrizable; no se debe afirmar que el comando actual recibe parámetros que no implementa ni retocar el protocolo congelado después de observar un resultado.

Para el futuro microbenchmark SIMD se proponen dimensiones 128, 129, 768 y 1,024, con ambas métricas. La dimensión 129 sirve para estudiar padding/colas; este microbenchmark aislado todavía no está implementado.

### 6.2 Tamaño de trabajo y contabilidad de memoria

El store actual redondea D a múltiplos de 16 componentes FP32 y alinea su buffer a 64 bytes:

$$
D_{fisica}=16\left\lceil\frac{D}{16}\right\rceil,
\qquad bytes_{embeddings}=N\cdot D_{fisica}\cdot4
$$

| N y dimensión lógica | Dimensión física | Payload FP32 ocupado, sin capacidad extra |
|---|---:|---:|
| 1,000 × 128 | 128 | 512,000 bytes = 0.512 MB |
| 10,000 × 1,024 | 1,024 | 40,960,000 bytes = 40.96 MB |
| 100,000 × 128 | 128 | 51,200,000 bytes = 51.20 MB |
| 100,000 × 8, fixture de memoria | 16 | 6,400,000 bytes = 6.40 MB |

Son estimaciones del payload físico de embeddings, no del proceso. Añadir capacidad reservada, IDs, mapa de posiciones, topología, visitados, candidatos, consultas y buffers de carga. Para recuperación híbrida se añadirán texto, vocabulario, postings y asociaciones documentales descritos en el [análisis de requisitos híbridos](../architecture/HYBRID_RETRIEVAL_REQUIREMENTS.md).

Usar MB decimales (`10^6` bytes) y MiB (`2^20` bytes) con etiquetas explícitas. El límite histórico de 14.50 MB se refiere a la estructura completa del grafo para su fixture; no es un límite de embeddings, de RSS o del nuevo sistema híbrido.

Registrar RSS actual y pico con sus unidades y alcance. Envolver toda la CLI con una herramienta de memoria observa también carga y construcción; no aísla el grafo. Las herramientas de macOS y Linux no siempre presentan las mismas unidades para sus contadores: conservar los bytes normalizados y la fuente de la medida.

## 7. Protocolo propuesto para nuevas mediciones

### 7.1 Preparación de cada sesión

1. Identificar equipo, SO, compilador, biblioteca estándar, SDK y perfil de compilación. Una actualización de toolchain/SO crea una referencia nueva o exige revalidar la comparación.
2. Conservar commit **y hashes de los archivos usados**: este checkout contiene cambios sin commit, por lo que HEAD solo no identifica el código medido. Registrar hash del ejecutable y configuración CMake efectiva.
3. Usar directorios nuevos por compilador y perfil. Confirmar arquitectura del binario y backend; no reutilizar un cache CMake de otro compilador.
4. Completar las pruebas de corrección, detener sus procesos y esperar a que el equipo se estabilice antes de medir. No ejecutar benchmark junto al build ni con CTest en paralelo.
5. Conectar alimentación y conservar el mismo modo energético. Registrar estado térmico/energético y actividad de memoria antes y después; si no se puede observar, declararlo.
6. Observar al menos 30 segundos sin otras cargas intensivas como criterio operativo inicial; comprobar deltas de swap. Si hay actividad sostenida, aplazar la sesión controlada o etiquetarla como medición bajo carga.
7. Verificar los checksums del dataset y fijar orden de inserción, semillas, métrica, K y configuración HNSW. Para comparar velocidad, comparar también calidad; cambiar `efSearch` no equivale a optimizar código.

Estos controles son un procedimiento de laboratorio propuesto. No constituyen una medición de frecuencia constante ni garantizan eliminar toda variación del planificador.

### 7.2 Alcance de los timers

| Medición | Incluye | Excluye o se reporta por separado |
|---|---|---|
| Kernel aislado, futuro driver | Comparaciones numéricas sobre arrays preparados | Carga, validación/normalización, grafo y presentación. |
| Consulta HNSW del driver actual | `QueryEngine::search`, preparación de consulta y asignaciones dentro de la llamada | CSV, construcción, cálculo del oráculo, comprobación de recall y escritura de reportes. |
| Construcción HNSW | Crear el índice e insertar sus nodos según el driver | Carga CSV previa y consultas posteriores. |
| Proceso CLI completo | Inicio, carga, construcción, consulta y salida | Es otra métrica; no sustituye al `query_time` interno. |
| Híbrido futuro | Análisis léxico, BM25, HNSW, asociación y RRF | Desglosar etapas; indicar aparte la generación externa del embedding y el I/O. |

El driver DBpedia usa `std::chrono::steady_clock`, prepara el índice antes de medir y conserva los resultados para impedir que el trabajo desaparezca. El oráculo exacto y el calentamiento previo también influyen en el estado de caché; el resultado debe describirse como **caliente y precargado**.

### 7.3 Repeticiones y reporte

- Realizar al menos cinco ejecuciones completas independientes del driver por configuración, una a la vez. Reconstruir con los mismos parámetros dentro de cada ejecución, sin incluir ese tiempo en el timer de búsqueda.
- Guardar cada ejecución en una carpeta nueva, sus muestras crudas, estado del sistema, recall, configuración y hash del binario. No sobrescribir una corrida anómala; explicar interrupciones y reglas de exclusión predefinidas.
- Publicar media, p50, p95, p99, máximo y dispersión entre ejecuciones. Para las 1,000 muestras actuales, el rango más cercano usa posiciones 500, 950 y 990 para p50/p95/p99.
- Reportar **20 consultas distintas × 50 ciclos**, no 1,000 consultas independientes. Un intervalo estadístico que tratara esas repeticiones como 1,000 necesidades distintas exageraría la evidencia.
- Para comparar variantes, alternar su orden entre corridas y conservar la carga. Publicar diferencia absoluta y relativa: `speedup = tiempo_base / tiempo_variante`; `reducción = 1 - tiempo_variante / tiempo_base`.
- No mezclar directamente mediciones de CPUs, compiladores, datasets, flags o niveles de recall diferentes. Si se cambian varias condiciones, etiquetar la comparación completa.

Si se reportan consultas por segundo, medir consultas completadas divididas entre el tiempo total del workload definido. No calcular throughput como el inverso de p99 ni multiplicar por los 14 núcleos sin una implementación concurrente y una medición específicas.

Una mejora del kernel no se traslada íntegra a HNSW. Como ejemplo matemático, si el 60% del tiempo fuese distancia y esa parte acelerara 4×, el speedup total sería `1 / (0.40 + 0.60/4) ≈ 1.82×`. Es un ejemplo de reparto de tiempo, no una fracción medida en AetherIndex. Para conocer el reparto real se necesita un perfil.

No se establece un nuevo SLA en esta tarjeta. El umbral local histórico de DBpedia y otros criterios se conservan como aceptación del protocolo que los define; no se extrapolan a producción ni a otra máquina.

## 8. Reproducción y herramientas de observación

### 8.1 Build de referencia y corrección

Desde la raíz del repositorio, estos comandos reproducen las opciones efectivas. El SDK se explicita con su ruta versionada; en el build comprobado CMake había resuelto el mismo SDK mediante su ruta `MacOSX.sdk`:

```sh
cmake -S . -B build-perf-reference -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=/Library/Developer/CommandLineTools/usr/bin/clang++ \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_SYSROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX26.2.sdk \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=26.5 \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DAETHER_ENABLE_SANITIZERS=OFF \
  -DAETHER_NATIVE_SIMD=ON \
  -DAETHER_BUILD_BENCHMARKS=ON \
  -DAETHER_REGISTER_PERFORMANCE_TESTS=OFF \
  -DBUILD_TESTING=ON
cmake --build build-perf-reference --parallel 4
ctest --test-dir build-perf-reference --output-on-failure -j 1
```

La carpeta usada en la comprobación de esta tarjeta fue `build-hardware-reference-20261021`. No se activaron los tests opcionales del corpus real ni los umbrales de rendimiento; el resultado 19/19 corresponde a la suite normal. Su duración total incluye arranque de procesos, fixtures y validación, y no representa la latencia de consulta.

### 8.2 Medir cuando se cumplan las condiciones

Tras validar el corpus y estabilizar el equipo, la ejecución existente para una primera corrida es:

```sh
./build-perf-reference/benchmarks/aether_dbpedia_benchmark \
  datasets/real/dbpedia build-perf-reference/runs/dbpedia-01 heuristic
```

Repetir en directorios `dbpedia-02` a `dbpedia-05`, registrando condiciones por corrida. El driver produce `dbpedia_samples.csv` y `dbpedia.json`; la metadata completa de hardware/compilación debe conservarse como evidencia adicional, porque esos archivos no identifican por sí solos todo el entorno.

Para revisar ensamblador con el script existente en esta instalación:

```sh
SDKROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX26.2.sdk \
python3 scripts/verify_distance_assembly.py \
  --compiler /Library/Developer/CommandLineTools/usr/bin/clang++ \
  --source-root . --output build-perf-reference/assembly --native
```

Esta comprobación compila un ensamblador auxiliar; complementar con el objeto final, como se hizo en esta tarjeta. La presencia de FMA empaquetada no cuantifica speedup.

### 8.3 Disponibilidad de profilers

| Herramienta | Situación observada | Uso previsto |
|---|---|---|
| Activity Monitor, `vm_stat`, `pmset`, `sysctl` | Las lecturas necesarias pudieron obtenerse; algunas requieren permiso fuera del entorno restringido | Contexto de carga, memoria, energía y datos de hardware. |
| `otool` y revisión de ensamblador | Utilizados | Confirmar arquitectura, bibliotecas e instrucciones del build real. |
| Instruments/`xctrace` | `xcrun --find xctrace` falló con el directorio activo de Command Line Tools | Time Profiler y análisis de hilos quedarían para una sesión con Xcode/Instruments operativo. No se instaló ni cambió Xcode. |
| `powermetrics` | Existe el ejecutable; no se ejecutó | Diagnóstico opcional de energía/frecuencia según capacidades y permisos; no mezclar su coste con la referencia sin instrumentación. |
| Linux `perf`/PMU | No disponible en este entorno macOS | Validación futura de eventos en Linux real, con `perf list` y permisos comprobados. |

No hay nuevas mediciones de fallos de LLC ni prueba local de una reducción porcentual de esos fallos. El [procedimiento Linux existente](../../scripts/profile_hnsw_cache.py) requiere un host y PMU compatibles, y limita los contadores a la ventana de consultas. No reemplazar ese indicador por una reducción de tiempo o por presencia de SIMD.

## 9. Criterios de aceptación de esta tarjeta

| ID | Criterio de análisis | Resultado de esta entrega |
|---|---|---|
| AH-01 | Identificar CPU, arquitectura, núcleos, RAM y SO mediante lecturas del equipo | Documentado y acompañado de evidencia. |
| AH-02 | Registrar compilador, biblioteca estándar, SDK, CMake, flags y arquitectura del binario | Documentado para R1; R2/R3 con alcance limitado al kernel. |
| AH-03 | Definir perfiles separados para rendimiento, diagnóstico y seguridad de memoria | R1/R2/R3/D/S/X definidos; no se confunden sus validaciones. |
| AH-04 | Identificar backend SIMD real y límites de la comparación | NEON ejecutado y FMA en objeto real; no hay escalar forzado ni se ejecutó AVX2 en esta tarjeta. |
| AH-05 | Definir condiciones de carga, energía, memoria, caché e instrumentación | Procedimiento propuesto y observaciones de la sesión incluidos. |
| AH-06 | Fijar dataset, parámetros, timers, repeticiones y estadísticas de referencia | DBpedia y protocolo existentes identificados; nueva campaña todavía pendiente. |
| AH-07 | Validar que el perfil principal puede compilarse y ejecutar la suite normal | Build limpio y CTest **19/19**, sin tests opcionales reales ni performance. |
| AH-08 | Conservar trazabilidad sin presentar diagnóstico como benchmark | Manifiesto, hashes, ejemplos ejecutados, ensamblador y log conservados; ninguna nueva latencia atribuida a HNSW. |

Para cerrar el análisis, el equipo deberá adoptar o ajustar el perfil y procedimiento propuestos. Las campañas de latencia, la comparación escalar/SIMD, el profiling de caché, la validación x86 y los presupuestos de producción son trabajos posteriores con su propia evidencia.

## 10. Evidencias y alcance

- [Entorno, flags efectivos, versiones y hashes de 60 archivos](hardware-reference/2026-10-21/environment.json).
- [Lecturas seleccionadas de hardware y swap](hardware-reference/2026-10-21/sysctl.txt), sin números de serie ni identificadores personales del equipo.
- [Resultados de los tres perfiles de kernel](hardware-reference/2026-10-21/compiler-probes.json), [fuente del diagnóstico](hardware-reference/2026-10-21/kernel_probe.cpp) y sus ensambladores auxiliares.
- [Estado térmico/energético puntual](hardware-reference/2026-10-21/thermal-state.json) y [fuente de esa lectura](hardware-reference/2026-10-21/thermal_probe.mm).
- [Objeto final: resumen](hardware-reference/2026-10-21/reference-object.json) y [desensamblado](hardware-reference/2026-10-21/reference-object.s).
- [Alcance de verificación](hardware-reference/2026-10-21/verification.json) y [19 pruebas completadas](hardware-reference/2026-10-21/ctest-release.log).

Esta entrega añade análisis y evidencia de diagnóstico. No modifica algoritmos, contratos públicos, dependencias ni opciones de producción. Los directorios nuevos de compilación están ignorados por Git. Las fechas de las capturas JSON están en UTC y pueden corresponder al 22 de octubre; el directorio y la tarjeta usan el día local 21 de octubre de 2026.
