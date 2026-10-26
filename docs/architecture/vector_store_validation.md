# Evidencia de validación: VectorStore y distancias

> Evidencia histórica del baseline anterior al 19-10-2026. La normalización,
> los kernels y el grafo actuales se validan en
> [optimización vectorial y FlatGraph](../benchmarks/OPTIMIZATION_VALIDATION.md).

Fecha de la sesión: **11 de octubre de 2026**, zona America/Mexico_City.
Entorno: macOS 26.5.2, ARM64; CMake; AppleClang 17.0.0 para el build normal y
Clang 23.1.3 instalado en Homebrew para ASan/UBSan.

## Resultados

| Verificación | Resultado |
|---|---|
| Build Debug con `-Wall -Wextra -Wpedantic` | Correcto |
| CTest Debug | 7/7 pruebas aprobadas |
| Dataset determinístico, semilla 42 | 1000/1000 embeddings de dimensión 128 recuperados exactamente |
| Rollback ante fallas de asignación | 18 puntos de falla verificados, con 0, 1, 2, 13 y 29 entradas previas |
| Coseno y euclidiana al cuadrado | Casos conocidos, propiedades, referencia long double y rango numérico aprobados |
| Integración con ambas métricas | Rankings distintos según la métrica configurada, como se esperaba |
| CLI | Salida completa del ejemplo conservada |
| ASan + UBSan con Clang 23.1.3 | 7/7 pruebas aprobadas; sin errores detectados en las rutas evaluadas |
| UBSan aislado con AppleClang 17 | 7/7 pruebas aprobadas |
| Build con `BUILD_TESTING=OFF` | Correcto; CLI ejecutada satisfactoriamente |

Los 18 puntos corresponden a 5, 4, 5, 2 y 2 fallas consecutivas antes de alcanzar
una inserción exitosa en cada escenario. Se verificaron tamaños, orden de IDs,
ausencia del ID rechazado, valores previos y capacidad de insertar nuevamente.
La inyección de fallas vive únicamente en su ejecutable de prueba.

## Comandos utilizados

Las carpetas de validación se crearon en `/private/tmp/` para no modificar los
artefactos previamente versionados en `build/`.

```sh
cmake -S . -B /private/tmp/aetherindex-vectorstore-debug-01a12e3a -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DCMAKE_CXX_FLAGS='-Wall -Wextra -Wpedantic'
cmake --build /private/tmp/aetherindex-vectorstore-debug-01a12e3a --parallel 4
ctest --test-dir /private/tmp/aetherindex-vectorstore-debug-01a12e3a --output-on-failure -V

cmake -S . -B /private/tmp/aetherindex-vectorstore-llvm-sanitizers-01a12e3a -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DAETHER_ENABLE_SANITIZERS=ON -DCMAKE_CXX_FLAGS='-Wall -Wextra -Wpedantic'
cmake --build /private/tmp/aetherindex-vectorstore-llvm-sanitizers-01a12e3a --parallel 4
ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ctest --test-dir /private/tmp/aetherindex-vectorstore-llvm-sanitizers-01a12e3a --output-on-failure --timeout 30

cmake -S . -B /private/tmp/aetherindex-vectorstore-no-tests-01a12e3a -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=OFF
cmake --build /private/tmp/aetherindex-vectorstore-no-tests-01a12e3a --parallel 4
/private/tmp/aetherindex-vectorstore-no-tests-01a12e3a/aetherindex
```

CTest conserva los registros completos en `Testing/Temporary/LastTest.log`
dentro de las carpetas de build. Estas rutas son temporales; los comandos
permiten reproducir la evidencia aunque el sistema limpie sus archivos.

## Incidencia del runtime de AppleClang

El build instrumentado con AppleClang 17 compiló, pero su runtime ASan se bloqueó
antes de `main`. La incidencia también se reprodujo con un programa mínimo y
fuera del sandbox. Una muestra de pila mostró inicialización recursiva del
allocator durante `InitializeShadowMemory`, terminando en `StaticSpinMutex`.
No se atribuye ese bloqueo al código de VectorStore.

Se completó la validación combinada con el Clang 23.1.3 ya instalado, sin
instalar dependencias ni cambiar el compilador predeterminado del proyecto.
Consultar [las instrucciones](../algorithms/vector_store_and_distances.md)
para seleccionar ese compilador en una carpeta de build independiente.

## Límites de la evidencia

Las pruebas del patrón ID → vista → distancia verifican el contrato para futuros
consumidores HNSW, no un grafo HNSW implementado. La prueba de 1,000 embeddings
valida funcionalidad, sin afirmar rendimiento a gran escala. Los sanitizers no
demuestran ausencia absoluta de errores de memoria.

No se ejecutaron builds en Windows o Linux. La revisión por el equipo y la
aprobación de un Pull Request permanecen como pasos externos pendientes para
cerrar la tarjeta en su gestor de tareas.

## Checklist T2.1–T2.6

La ejecución específica del checklist proporcionado por el usuario se documenta
en [Resultados T2.1–T2.6](vector_store_T2_checklist.md), con registros CTest
Debug y ASan/UBSan conservados dentro del repositorio.
