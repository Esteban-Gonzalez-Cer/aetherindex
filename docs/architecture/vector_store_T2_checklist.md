# Resultados del checklist T2.1–T2.6

> Evidencia histórica del baseline anterior al 19-10-2026. La normalización,
> los kernels y el grafo actuales se validan en
> [optimización vectorial y FlatGraph](../benchmarks/OPTIMIZATION_VALIDATION.md).

Se ejecutaron los escenarios de las dos capturas proporcionadas. Los seis
criterios están cubiertos por las pruebas existentes de almacenamiento y
distancias, reforzadas con dos comprobaciones: ID inexistente en un store
poblado y dimensión de cada embedding del dataset recuperado.

## Resultado por criterio

| Criterio | Comprobación realizada | Resultado |
|---|---|---|
| T2.1 — Inserción de vectores | Se insertaron tres vectores 3D con IDs conocidos, incluyendo 0 y el máximo uint32_t. `size()` devolvió 3, igual al número insertado. | Aprobado |
| T2.2 — Recuperación por ID | Se recuperaron los IDs del dataset y se compararon dimensión y todos los componentes con los originales. Las vistas básicas también mantienen dimensión 3. | Aprobado |
| T2.3 — ID inexistente | `contains()` devolvió false y `find()` devolvió nullopt tanto en un store vacío como con ID 12345 en un store poblado. Sin crash ni diagnósticos ASan/UBSan en las rutas evaluadas. | Aprobado |
| T2.4 — Dimensiones incompatibles | Un vector 2D y uno vacío se rechazaron con DimensionMismatch en un store 3D. Se conservaron tamaño, IDs y datos originales; una inserción válida posterior funcionó. | Aprobado |
| T2.5 — Exactitud de distancia | Se verificaron distancias coseno 0, 1 y 2, y euclidiana al cuadrado 25, con tolerancia absoluta 1e-5; además, simetría y comparación con referencia long double. | Aprobado |
| T2.6 — Dataset para MVP | Se insertaron y recuperaron 1,000 embeddings de dimensión 128, semilla 42 e IDs no consecutivos. Los 128,000 componentes coincidieron exactamente, sin IDs faltantes ni pérdida de datos. | Aprobado |

La línea duplicada «Consultar el número de elementos almacenados» en T2.1
corresponde a la misma comprobación de `size()`.

## Ejecución completa

- **Debug:** 7/7 pruebas aprobadas; compilación AppleClang 17 con
  `-Wall -Wextra -Wpedantic`.
- **AddressSanitizer + UndefinedBehaviorSanitizer:** 7/7 pruebas aprobadas con
  Clang 23.1.3, usando `halt_on_error=1` y sin errores detectados.
- **Código de salida de ambas ejecuciones:** 0.
- **Dato emitido por la prueba:**
  `1000/1000 embeddings of dimension 128 recovered exactly (seed 42).`

Los seis renglones del checklist son escenarios; no se presentan como seis
nuevos tests CTest independientes. También se ejecutaron las pruebas de rollback,
fuerza bruta, integración de módulos y salida de CLI dentro de la suite completa.

## Evidencia conservada

- [Registro CTest Debug](validation/vector_store_T2_debug.log).
- [Registro CTest ASan/UBSan](validation/vector_store_T2_sanitizers.log).
- Almacenamiento: `tests/unit/vector_store_test.cpp`, funciones
  `checkBasicStorage`, `checkRejections` y `checkThousandEmbeddings`.
- Distancias: `tests/unit/distance_test.cpp`, funciones `checkKnownDistances`
  y `checkPropertiesAndReference`.

## Reproducir las pruebas

Desde la raíz del proyecto:

```sh
cmake -S . -B build-t2-debug -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build-t2-debug --parallel 4
ctest --test-dir build-t2-debug --output-on-failure -V

cmake -S . -B build-t2-sanitizers -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DAETHER_ENABLE_SANITIZERS=ON
cmake --build build-t2-sanitizers --parallel 4
ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ctest --test-dir build-t2-sanitizers --output-on-failure -V
```

Las ejecuciones registradas utilizaron carpetas nuevas en `/private/tmp/`;
los comandos anteriores crean carpetas locales equivalentes. Clang 23 se
selecciona explícitamente para evitar el bloqueo de inicio del runtime ASan de
AppleClang observado anteriormente en este Mac.

Los resultados validan las rutas probadas; no demuestran ausencia absoluta de
problemas de memoria ni rendimiento a gran escala. El checklist de la aplicación
mostrada en las capturas no se modificó.
