# Búsqueda BM25 desde la CLI

**Actualización:** 28 de octubre de 2026. BM25 está conectado a SearchService y
QueryEngine. Se conserva la búsqueda vectorial de la [CLI general](cli.md).
La búsqueda híbrida HNSW+BM25+RRF está disponible desde la misma fecha: ver
[CLI híbrida](cli_hybrid.md).

## Compilar y ejecutar

Desde la raíz del proyecto:

```sh
cmake -S . -B build-cli -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build-cli --parallel 4
./build-cli/aetherindex search --algorithm bm25 --dataset datasets/corpus.jsonl --query datasets/queries.jsonl --query-id Q002 --k 5
```

El corpus incluido tiene 30 documentos y 24 consultas. Sus embeddings pendientes
no impiden la búsqueda léxica. Para escribir tu propia consulta:

```sh
./build-cli/aetherindex search --mode lexical --dataset datasets/corpus.jsonl --query-text "BM25 frecuencia longitud documento" --k 5
```

En Visual Studio, utilizar `.\build-cli\Debug\aetherindex.exe` y agregar
`--config Debug` al build y `-C Debug` a CTest. Esta revisión se ejecutó en macOS;
no se acredita aquí una ejecución de Windows/Linux.

## Opciones y reglas

| Opción | Contrato |
|---|---|
| `--mode lexical` | Ejecuta BM25; no necesita `--algorithm`. |
| `--algorithm bm25` | Selecciona modo léxico cuando no se especificó un modo. |
| `--dataset FILE` | Corpus documental JSONL obligatorio. |
| `--query FILE` | Archivo JSONL de consultas. Excluyente con `--query-text`. |
| `--query-id ID` | Selección exacta; obligatoria con varias consultas. Funciona también en consultas vectoriales JSONL. |
| `--query-text TEXT` | Texto literal, incluyendo cadena vacía. Para texto que empieza con `--`, utilizar un archivo JSONL. |
| `--k N` | Entero positivo, default 10; se devuelven hasta K coincidencias, sin rellenar. |
| `--bm25-k1 X` | Finito y >0; default 1.2. |
| `--bm25-b X` | Finito en [0,1]; default 0.75. |

Las combinaciones de modo/algoritmo contradictorias se rechazan antes de abrir
archivos. `--metric` y los parámetros HNSW no aplican a BM25; los parámetros BM25
no aplican al modo vectorial. Los argumentos siguen usando nombre y valor
separados, sin sintaxis `--opcion=valor`.

Un archivo con una sola consulta no necesita selector. Con varias consultas se
requiere `--query-id`; el identificador elegido se imprime en la respuesta. Un
selector inexistente o duplicado es error de datos. Nunca se selecciona
silenciosamente la primera consulta.

Una consulta vacía, solo puntuación o sin términos conocidos devuelve cero
resultados y éxito. Un archivo sin registros de consulta es error de datos. K
mayor que el número de coincidencias devuelve únicamente las disponibles.

## Documentos y consultas JSONL

Un objeto por línea no vacía, UTF-8 válido, con LF o CRLF. Los documentos pueden
usar este formato:

```json
{"document_id":"D001","title":"Motor E47","text":"Revisar la temperatura.","embedding":null}
```

`document_id` es un string no vacío o entero uint32, obligatorio y único. El
alias anterior `id` sigue aceptándose; si aparecen ambos deben coincidir. `text`
es string obligatorio y puede estar vacío; `title` es string opcional. BM25
indexa **`title + "\n" + text`**, sin pesos especiales ni repeticiones artificiales
del título. El perfil de análisis se mantiene en
[ascii-tech-v1 de CP-02](algorithms/bm25.md): no añade stemming, sinónimos ni
normalización Unicode completa.

Las consultas usan:

```json
{"query_id":"Q001","query_text":"motor E47","relevant_document_ids":["D001"]}
```

Se aceptan también `id` para identidad y `query`/`text` para texto si no
contradicen los campos principales. La consulta seleccionada debe tener texto.
Las etiquetas `relevant_document_ids` **no participan en la recuperación**;
son referencias para una evaluación independiente.

El modo léxico no requiere ni convierte embeddings. Los metadatos se ignoran
para scoring, pero el registro completo debe ser JSON válido. Se decodifican
comillas, barras, controles escapados y escapes Unicode válidos. Se rechazan
claves repetidas, aliases contradictorios, tipos incorrectos, IDs duplicados,
UTF-8/escapes inválidos, comas finales, contenido sobrante y anidamiento mayor
que 64 contenedores. Los errores de sintaxis indican archivo, línea y columna.
Un número parcial como `1oops` nunca se acepta como `1`.

Los identificadores numéricos canónicos uint32 se conservan dentro de BM25. Los
IDs opacos reciben IDs internos libres en orden del identificador de origen,
reservando previamente los numéricos. La asociación es reproducible para el
mismo corpus, independientemente del orden de sus filas, y se reconstruye en
cada invocación. La salida conserva el identificador de origen, por ejemplo
`D002`; no lo reemplaza con la posición de una fila.

Para mantener la tabla, los bytes no ASCII, barras inversas, delimitadores y
controles de los IDs se muestran como `\xHH`. El servicio conserva el valor
original; el escape se aplica únicamente a la presentación.

## Salida y tiempos

La consulta Q002 con K=5 produce:

```text
+------+-------------+-------------+
| Rank | Document ID | BM25 Score  |
+------+-------------+-------------+
|    1 |        D002 | 7.385718041 |
+------+-------------+-------------+
|    2 |        D019 | 3.490460462 |
+------+-------------+-------------+
|    3 |        D027 | 2.741042578 |
+------+-------------+-------------+
|    4 |        D009 | 2.652723532 |
+------+-------------+-------------+
|    5 |        D012 | 2.652723532 |
+------+-------------+-------------+
```

Un score mayor representa una mejor coincidencia léxica; no es una distancia
ni una probabilidad. El motor calcula en double y la tabla presenta nueve
decimales. Se conserva el orden del motor: score descendente y DocumentId
interno ascendente para igualdad exacta.

La respuesta incluye documentos indexados, consulta seleccionada, parámetros,
K solicitado, resultados devueltos y dos tiempos:

- Construcción: tokenización de documentos y construcción del índice léxico.
- Consulta: `QueryEngine::search`, incluyendo análisis de consulta, scoring,
  ordenamiento y Top-K. Excluye lectura de archivos, construcción, traducción
  de IDs y presentación.

Los tiempos se expresan en milisegundos. Cada proceso reconstruye el índice;
no hay persistencia ni generación de embeddings dentro de esta operación.

Se conservan los códigos: 0 éxito/ayuda, 2 argumentos/configuración, 3 datos de
entrada y 4 búsqueda/error interno. Los fallos escriben solo en stderr, sin
resultados parciales.

## Regresiones vectoriales y pruebas

En modo vectorial JSONL se exige un array `embedding` o `vector`, no ambos,
con floats finitos y dimensiones compatibles. Los embeddings nulos no se
reemplazan. Desde CP-04, cada embedding JSONL se almacena con VectorId igual a
su DocumentId, de modo que las salidas vectorial, BM25 e híbrida usan la misma
identidad documental y la tabla vectorial JSONL muestra `Document ID`. Dataset y consulta
vectoriales deben compartir formato CSV o JSONL. Las consultas coseno cero y
las dimensiones incorrectas se reportan como datos inválidos antes de construir
HNSW. Las opciones y columnas CSV existentes se conservan.

```sh
ctest --test-dir build-cli -L bm25-cli --output-on-failure
ctest --test-dir build-cli -R 'cli_parser|jsonl_service_compatibility' --output-on-failure
ctest --test-dir build-cli --output-on-failure
```

Las pruebas de la integración comprueban las 24 consultas contra un cálculo
independiente de scores, con tolerancia 1e-12, además del ejecutable real,
errores, IDs, tablas y ausencia de influencia de las etiquetas. Esto verifica
el funcionamiento; no afirma un objetivo de relevancia sobre datos de industria.

Ver [hallazgos y evidencia de la revisión](reports/BM25_CLI_REVIEW.md).
