# Revisión y corrección de BM25 en la CLI

**Fecha local:** 28 de octubre de 2026 · **Base:** `a97f4ec`.

BM25 ya tenía una biblioteca funcional, pero la CLI no podía ejecutarla. Se
implementó el recorrido léxico completo CLI → SearchService → QueryEngine →
Bm25Index, con importación del corpus textual actual, selección de consulta,
identidad documental, configuración y presentación de scores. Se corrigieron
además errores reproducibles del recorrido JSONL compartido.

## Hallazgos y correcciones

| Hallazgo | Evidencia antes del cambio | Corrección |
|---|---|---|
| **F1 · P1 — BM25 no estaba conectado a la CLI.** | `--algorithm bm25` devolvía código 2, “Unknown algorithm”. El único servicio disponible seguía cargando VectorStore y exigía embeddings. | Modo léxico y selección `bm25`, sobrecarga textual de QueryEngine, construcción y búsqueda reales con Bm25Index. No requiere embeddings. |
| **F2 · P1 — El lector no interpretaba el contrato del corpus.** | Buscaba `id`/`query` y no `document_id`/`query_text`; faltaba una ruta documental de carga. El corpus usa IDs D001…D030 y consultas Q001…Q024. | Lectura de los campos actuales, aliases controlados, validación de tipos, identidades únicas y selección de consulta. |
| **F3 · P1 — JSON inválido podía convertirse en datos válidos incorrectos.** | El vector `[1oops,0]` se aceptaba y la búsqueda terminaba con código 0 por conversión parcial mediante `stof`. El extractor de strings tampoco decodificaba escapes JSON. | Parser privado de aplicación con consumo completo, UTF-8, escapes/Unicode, validación de gramática y claves duplicadas. Componentes vectoriales completos y finitos. |
| **F4 · P2 — Se ignoraban las consultas posteriores a la primera.** | Un archivo con Q1 y Q2 devolvía éxito consultando solo Q1, sin selección ni aviso. | `--query-id` explícito para archivos con varias consultas. IDs inexistentes/duplicados y archivos sin registros producen error de datos. |
| **F5 · P2 — Se clasificaban datos vectoriales inválidos como fallo interno.** | Una consulta JSONL de dimensión incorrecta llegaba al motor y devolvía código 4. | Validación de dimensión y consulta coseno cero antes de construir HNSW; código 3 y diagnóstico de entrada. |
| **F6 · P2 — Se construía un índice léxico que nunca se consultaba.** | La ruta JSONL vectorial preparaba un InvertedIndex aunque devolvía solo vecinos de fuerza bruta/HNSW. | Se elimina ese trabajo del recorrido vectorial; la indexación BM25 se realiza únicamente en modo léxico. |

Las reproducciones con comandos, datos y salida están en
[baseline-reproductions.json](evidence/2026-10-28-bm25-cli/baseline-reproductions.json).
Los tres errores de parsing/selección/dimensión ahora devuelven código 3 sin
resultados parciales:
[fixed-reproductions.json](evidence/2026-10-28-bm25-cli/fixed-reproductions.json).
La búsqueda vectorial con embeddings nulos sigue rechazándose: el nuevo modo
léxico permite consultar los textos sin fabricar vectores.

## Reglas funcionales incorporadas

- `--mode lexical` usa BM25. `--algorithm bm25` lo selecciona si no se indicó un
  modo. Los comandos vectoriales previos mantienen fuerza bruta/HNSW.
- Una consulta se proporciona por archivo o por `--query-text`, nunca ambos.
  En JSONL se selecciona por identidad; no se busca texto de varias consultas
  como una sola consulta ni se omiten registros silenciosamente.
- Se indexa `title + "\n" + text`, sin boosts. No se modifica el algoritmo BM25
  ni su perfil de tokenización CP-02.
- Los IDs de origen se conservan en la respuesta. Los uint32 canónicos
  mantienen su valor; los IDs opacos tienen una asociación explícita con IDs
  internos libres, determinística para el mismo corpus y ajena al orden de filas.
- La tabla léxica usa `Document ID` y `BM25 Score`, con score descendente y
  nueve decimales. Los bytes de IDs que podrían alterar la tabla se escapan
  únicamente en presentación. La respuesta del servicio conserva el original.
- K es un máximo; no se agregan documentos sin coincidencias. Consultas vacías,
  solo separadores u OOV producen éxito con cero resultados. Un archivo vacío
  o sin el texto requerido produce error de datos.
- Se validan `k1 > 0` y `b ∈ [0,1]`, ambos finitos. Las opciones vectoriales se
  rechazan en modo léxico, y las BM25 se rechazan en modo vectorial.
- Los campos de relevancia esperada no determinan los resultados. Cambiar
  `relevant_document_ids` deja intacto el ranking.
- Se separan tiempos de construcción y consulta. Los textos de alcance ahora
  hablan de carga de entrada, para describir correctamente CSV y JSONL.

No se añade lógica de scoring ni parsing documental a la CLI. El parser JSON
es interno a `application`; los tipos comunes y los módulos lexical/hybrid no
adquieren dependencias de archivos ni de presentación. No se añadió una
dependencia de terceros.

## Validación ejecutada

Las compilaciones incluyen los cambios del árbol de trabajo sobre `a97f4ec`;
el commit base solo no los contiene. Los
[hashes de fuentes y corpus](evidence/2026-10-28-bm25-cli/source-sha256.json)
identifican lo validado. No se creó commit ni Pull Request.

| Perfil | Resultado | Log |
|---|---:|---|
| Debug, compilación limpia | **56/56** | [CTest](evidence/2026-10-28-bm25-cli/debug-ctest.log) |
| Release, directorio nuevo | **56/56** | [CTest](evidence/2026-10-28-bm25-cli/release-ctest.log) |
| Debug ASan/UBSan, directorio nuevo | **56/56** | [CTest](evidence/2026-10-28-bm25-cli/sanitizers-ctest.log) |
| Release, BUILD_TESTING=OFF | Compila; cero tests registrados; consulta BM25 Q002 aprobada | [Build](evidence/2026-10-28-bm25-cli/no-tests-build.log), [consulta](evidence/2026-10-28-bm25-cli/no-tests-smoke.log) |

Se ejecutó en macOS ARM64, con AppleClang 17.0.0 en Debug/Release y Homebrew
Clang 23.1.3 para ASan/UBSan. Los comandos y versiones completos están en
[runs.json](evidence/2026-10-28-bm25-cli/runs.json) y
[environment.json](evidence/2026-10-28-bm25-cli/environment.json).
No se detectaron errores de memoria o comportamiento indefinido en las rutas
ejercitadas. Las once unidades C++ nuevas/modificadas aprobaron
`-Wall -Wextra -Wpedantic -Werror`:
[comando y resultado](evidence/2026-10-28-bm25-cli/strict-warnings.json).

Las 56 comprobaciones comprenden las 52 previas y cuatro nuevas:

| Prueba | Qué comprueba |
|---|---|
| `unit.json_value` | Gramática completa del valor, consumo total, UTF-8, escapes, pares sustitutos, claves duplicadas y límite exacto de 64 contenedores. |
| `integration.lexical_service` | Scores de referencia independientes, parámetros, títulos, IDs numéricos/opacos, duplicados, orden de filas, vacíos/OOV, selección, etiquetas y errores. |
| `e2e.cli_bm25` | Ejecutable real: argumentos, streams, códigos, bordes y columnas, nueve decimales, texto escapado, IDs con controles y las 24 consultas del corpus. |
| `integration.bm25_cli_corpus` | Las 24 consultas del corpus frente a un cálculo separado de TF/DF, longitudes, IDF y fórmula BM25; candidatos, IDs, unicidad, orden y prefijo Top-K. |

Se ampliaron también `unit.cli_parser` y
`integration.jsonl_service_compatibility`. La suite existente verifica que
fuerza bruta/HNSW, ambos tipos de distancia y RRF continúan funcionando.

### Precisión interna y formato de salida

Se compararon scores completos contra la fórmula independiente, con
**tolerancia absoluta 1e-12**. El error máximo en los tres perfiles fue
**3.552713678800501e-15**:
[resumen Debug](evidence/2026-10-28-bm25-cli/debug/summary.json),
[scores esperado/obtenido](evidence/2026-10-28-bm25-cli/debug/scores.tsv).

La CLI muestra nueve decimales, por lo que su redondeo se verifica por separado
con cota `0.5 × 10^-9`. Se comprobaron las **238 filas mostradas por las 24
consultas**, en cada perfil, contra los scores completos y sus IDs/posiciones.
El máximo error de presentación fue **4.996412172886266e-10**. No se exige
1e-12 a un número ya redondeado a nueve decimales:
[verificación de salida](evidence/2026-10-28-bm25-cli/cli-output-verification.json).

Las salidas completas quedan en
[debug/cli](evidence/2026-10-28-bm25-cli/debug/cli/Q002.txt),
[release/cli](evidence/2026-10-28-bm25-cli/release/cli/Q002.txt) y
[sanitizers/cli](evidence/2026-10-28-bm25-cli/sanitizers/cli/Q002.txt).

## Ejecución manual

```sh
cmake -S . -B build-cli -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build-cli --parallel 4
./build-cli/aetherindex search --algorithm bm25 --dataset datasets/corpus.jsonl --query datasets/queries.jsonl --query-id Q002 --k 5
```

También puede usarse el build verificado de esta revisión:

```sh
./build-bm25-cli-review/aetherindex search --mode lexical --dataset datasets/corpus.jsonl --query-text "BM25 frecuencia longitud documento" --k 5
ctest --test-dir build-bm25-cli-review -L bm25-cli --output-on-failure
```

La [guía de CLI BM25](../cli_bm25.md) contiene contratos, formatos, opciones y
la tabla esperada de Q002.

## Alcance de la conclusión

BM25 funciona de extremo a extremo por la CLI para el corpus JSONL actual y
los casos probados. Las 24 consultas se utilizan para comprobar cálculo y
cableado; no se afirma que el corpus técnico pequeño represente un entorno
industrial ni que sus etiquetas acrediten una meta de relevancia. Los tiempos
de CTest no son latencias de consulta ni una prueba de rendimiento.

La composición híbrida con RRF, generación de embeddings, persistencia,
normalización Unicode completa y validación de ejecución Windows/Linux no se
añaden en esta revisión.
