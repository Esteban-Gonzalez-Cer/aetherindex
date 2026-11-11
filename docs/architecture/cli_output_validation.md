# Validación: tabla, tiempos y datos reales

> Evidencia histórica del baseline previo a las optimizaciones del 19-10-2026.
> Describe el código y las mediciones de esa versión; las cifras y salidas
> guardadas no se recalcularon. Para el contrato y resultados actuales, consultar
> [la validación de optimizaciones](../benchmarks/OPTIMIZATION_VALIDATION.md).

Fecha local: **2026-10-12**, America/Mexico_City, macOS ARM64.

Este informe registra la entrega de formato y datos reales anterior a la
habilitación de HNSW. Para el estado actual del algoritmo y la CLI consultar
[la auditoría y validación de HNSW](hnsw_validation.md).

## Cambios verificados

- Tabla ASCII con separadores verticales y horizontales entre vecinos.
- Rank e ID enteros; distancia en formato fijo con nueve decimales.
- Ancho calculado según los valores: ID máximo `4294967295` y distancia de
  orden `1e38` permanecen alineados, sin notación científica.
- `SearchResponse::query_time` usa milisegundos y un reloj monótono alrededor
  de `QueryEngine::search`, sin incluir carga CSV ni presentación.
- La prueba del servicio comprueba duración finita y no negativa; las pruebas
  de CLI comprueban formato y alcance del tiempo, sin imponer umbrales de latencia.
- Se conserva fuerza bruta, las dos métricas, el ranking y `min(K,N)`.

## Compilaciones y suite normal

Directorios nuevos, compilados con `-Wall -Wextra -Wpedantic`:

| Configuración | Resultado | Evidencia |
|---|---|---|
| Debug, AppleClang 17 | Build y **10/10** CTest correctos | [Log Debug](validation/cli_output_debug.log) |
| Release, AppleClang 17 | Build y **10/10** CTest correctos | [Log Release](validation/cli_output_release.log) |
| Debug + ASan/UBSan, Homebrew Clang 23.1.3 | Build y **10/10** CTest correctos, sin diagnósticos de sanitizers en las rutas evaluadas | [Log sanitizers](validation/cli_output_sanitizers.log) |
| Release, `BUILD_TESTING=OFF` | Build correcto, CTest enumera 0 tests, ejemplo Top-2 correcto | Ejecución manual descrita abajo |

Los tiempos totales CTest fueron 6.18 s, 5.63 s y 9.27 s respectivamente. Las
suites se ejecutaron concurrentemente: estos tiempos incluyen preparación y
aserciones, y no son una comparación de rendimiento del motor.

Directorios utilizados:

```text
/private/tmp/aetherindex-output-debug-20261012
/private/tmp/aetherindex-output-release-20261012
/private/tmp/aetherindex-output-sanitizers-20261012
/private/tmp/aetherindex-output-no-tests-20261012
```

La compilación sin tests ejecutó el ejemplo 2D con K=2 y devolvió `10` a
`0.000000000` y `40` a `0.200000003`, en la tabla con bordes y salida 0. Se
actualizó también el ejecutable del usuario en `build-cli/` y su suite pasó
**10/10**.

En el primer pase, la nueva prueba de distancias grandes dejaba un vector cero
en un fixture usado después con coseno. Se restauró el fixture antes de probar
errores de archivos; las tres suites completas anteriores incluyen la corrección.

## Caso real separado

El [escenario DBpedia](../real_world_validation.md) utiliza artículos y embeddings
publicados, consultas excluidas del corpus y un oráculo exhaustivo independiente.
Se registra como `e2e.cli_real_dataset` al configurar `AETHER_REAL_DATA_DIR`.
La suite normal conserva diez tests sin dependencias externas ni descargas.

Se ejecutó la suite ampliada en `build-cli-release/`, AppleClang 17, Release:
**11/11 CTest correctos**, incluidos los 40 procesos del caso real. El test de
datos reales tardó 87.76 s y la suite completa 89.77 s, con salida 0. El oráculo
utilizó Python 3.13.5 y NumPy 2.3.5.

- **40/40** consultas Top-100 correctas: 20 coseno y 20 euclidiana al cuadrado.
- **4,000** vecinos contrastados contra las distancias exhaustivas de NumPy
  float64 y el ranking redondeado a float32, con empates sin orden obligatorio.
- Error absoluto máximo: **`5.99890916941348e-08`**, menor que `1e-5`.
- Corpus: 10,000 × 1,024; consultas de 20 artículos distintos excluidos del
  catálogo, sin embeddings idénticos a los del corpus.
- Checksums de todos los archivos de entrada correctos; IDs válidos y únicos,
  posiciones consecutivas y distancias impresas con nueve decimales.

| Métrica | Mínimo de búsqueda (ms) | Mediana de búsqueda (ms) | Máximo de búsqueda (ms) | Mediana del proceso completo (ms) |
|---|---:|---:|---:|---:|
| Coseno | 9.242875 | 10.586855 | 12.073041 | 2143.201333 |
| Euclidiana al cuadrado | 8.798333 | 9.914167 | 11.293292 | 2162.851042 |

Son 20 observaciones locales por métrica, sin calentamiento ni repeticiones de
cada consulta. El proceso completo incluye cargar/validar el CSV de 140.0 MB,
crear el store, buscar, imprimir y liberar recursos. No se equipara su duración
con la búsqueda, ni se atribuye toda la diferencia a una fase no medida. No es
un benchmark de producción ni una comparación entre librerías.

Evidencia guardada: [registro CTest ampliado](validation/cli_output_real_release.log),
[informe independiente por consulta](validation/cli_real_dataset.json) y
[manifest de procedencia y checksums](validation/cli_real_dataset_manifest.json).

La preparación repetida con `--offline` conservó los SHA-256 de los **23 archivos**
preparados. Una consulta manual Top-10 también produjo la
[tabla real completa guardada](validation/cli_real_sample.txt), con los mismos
primeros vecinos del caso Top-100 y salida 0. El formato C++ y
`git diff --check` finalizaron correctamente.

HNSW sigue sin implementar. Esta entrega no valida HNSW, relevancia juzgada por
personas, capacidad concurrente ni una garantía de latencia de producción.
