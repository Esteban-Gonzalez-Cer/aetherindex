# Caso real: recomendación de artículos relacionados

## Caso de uso y fuente

Una base de conocimiento puede sugerir artículos relacionados con el que está
leyendo una persona comparando sus embeddings. Es una aplicación de búsqueda
vectorial usada en recomendación de contenido, como describe
[Qdrant en su documentación de recomendaciones](https://qdrant.tech/articles/new-recommendation-api/).
Este caso adapta ese flujo a AetherIndex: el embedding de un artículo de consulta
busca los artículos más cercanos en un catálogo distinto.

Los datos provienen de
[Qdrant/DBpedia, text-embedding-3-small, 1,024 dimensiones, 100K](https://huggingface.co/datasets/Qdrant/dbpedia-entities-openai3-text-embedding-3-small-1024-100K).
Son descripciones y embeddings publicados de entidades reales, con título e ID
DBpedia. AetherIndex no genera embeddings ni llama a servicios de inferencia.
La preparación consume únicamente un fragmento mediante la
[API oficial de filas de Hugging Face](https://huggingface.co/docs/dataset-viewer/rows).

| Propiedad | Escenario predeterminado |
|---|---|
| Vectores del catálogo | 10,000 |
| Dimensión | 1,024 componentes float32 |
| Consultas | 20 artículos reales excluidos del catálogo |
| Filas del corpus en la fuente | 0–9,999 |
| Filas de consulta en la fuente | 10,000–10,019 |
| Revisión de la fuente | `d49822cb2465295366349d414679b476997e88f5` |
| ID interno | `VectorId = 7 * source_row + 10` |
| Vecinos solicitados | 100 |
| Métricas evaluadas | Coseno y euclidiana al cuadrado |

Las filas se eligen de forma determinista; no constituyen una muestra aleatoria
del catálogo. Los formatos siguen siendo `ID,x1,...,xD` para el dataset y
`x1,...,xD` para una consulta. No se rellenan con ceros ni se repiten los cuatro
vectores del ejemplo pequeño. Los componentes publicados se redondean a float32,
el tipo del motor, y se serializan con nueve dígitos significativos para permitir
su recuperación. Esto es independiente de los **nueve decimales** de la tabla.

## Preparar y ejecutar

Desde la raíz del repositorio, Python 3.10 o superior:

```sh
python3 scripts/prepare_real_dataset.py
cmake -S . -B build-cli-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build-cli-release --parallel 4
./build-cli-release/aetherindex search --dataset datasets/real/dbpedia/vectors.csv --query datasets/real/dbpedia/queries/query_00.csv --algorithm brute-force --k 100
```

La descarga inicial necesita red y ocupa unos cientos de MB entre las páginas
JSON originales y los archivos preparados. No descarga modelos ni requiere una
clave API. El script limita sus peticiones, conserva páginas válidas en caché y
se puede repetir si el proveedor limita temporalmente el acceso. Comprueba
`X-Revision` en cada respuesta y rechaza revisiones distintas, filas incompletas,
celdas truncadas, dimensiones inválidas, IDs originales duplicados y consultas
con un embedding idéntico a uno almacenado. No se desactiva la verificación TLS.

Para volver a preparar desde la caché sin utilizar red:

```sh
python3 scripts/prepare_real_dataset.py --offline
```

Archivos locales generados en `datasets/real/dbpedia/`:

- `vectors.csv`: el catálogo de embeddings, sin encabezado.
- `queries/query_00.csv` a `query_19.csv`: una consulta por archivo, sin ID.
- `articles.jsonl`: asociación entre `VectorId`, ID original, título y texto.
- `queries.json`: asociación entre cada archivo de consulta y el artículo real.
- `manifest.json`: procedencia, revisión, conversión, intervalos y SHA-256 de los
  archivos preparados y las páginas fuente cacheadas con su revisión recibida.
- `source-cache/`: páginas de la fuente para reutilizar la descarga.

La ficha de Qdrant no declara una licencia específica para estos embeddings.
Se conserva la atribución y los datos descargados se mantienen fuera de Git;
el repositorio contiene las instrucciones y scripts de preparación/evaluación.

## Validación independiente y CTest

La evaluación opcional requiere NumPy además de Python. Si no está instalado:

```sh
python3 -m pip install numpy
```

Activar el test con una ruta absoluta al dataset preparado:

```sh
cmake -S . -B build-cli-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DAETHER_REAL_DATA_DIR="$PWD/datasets/real/dbpedia"
cmake --build build-cli-release --parallel 4
ctest --test-dir build-cli-release --output-on-failure
ctest --test-dir build-cli-release -R '^e2e.cli_real_dataset$' --output-on-failure -V
```

El test adicional no descarga nada. La suite predeterminada conserva sus diez
pruebas sin dependencias externas; configurar `-DAETHER_REAL_DATA_DIR=` desactiva
el test opcional. También se puede ejecutar la evaluación directamente:

```sh
python3 scripts/validate_real_dataset.py --cli build-cli-release/aetherindex --data-dir datasets/real/dbpedia --k 100 --report build-cli-release/real-validation.json
```

El validador verifica los checksums, el tamaño y la dimensión del catálogo y la
separación entre corpus y consultas. Calcula todas las distancias con NumPy en
float64 a partir de los mismos componentes float32 del motor; no reutiliza
`compute_distance` ni el ordenamiento de AetherIndex. Ejecuta 20 consultas por
métrica, para 40 procesos de CLI, y comprueba los 100 vecinos de cada resultado:
IDs válidos y únicos, posiciones consecutivas, distancias con nueve decimales y
`abs(resultado - referencia) < 1e-5`. Exige el Top-K exacto usando las distancias
de referencia redondeadas a float32, permitiendo cualquier orden entre empates.
El informe guarda títulos e IDs de los primeros resultados para inspección humana.

## Interpretar los tiempos y el alcance

`Query time (ms)` mide exclusivamente la búsqueda en el store ya cargado, con
cálculo de distancias, ordenamiento completo y truncamiento a K. El informe
también registra tiempo del proceso entero; este incluye cargar y validar el
CSV de nuevo en cada ejecución. Por eso puede ser mucho mayor que el tiempo
de consulta mostrado en la tabla.

Se registra una observación por consulta y métrica, con mínimo, mediana y máximo
de esta ejecución local. Es evidencia de funcionamiento y medidas observadas,
sin calentamiento ni repeticiones suficientes para un benchmark de producción,
comparaciones entre sistemas o una garantía de latencia. El tiempo de CTest
también incluye cálculo de referencia, comprobaciones y creación de procesos.

Este caso utiliza contenido real y embeddings de dimensión habitual en búsqueda
semántica. Valida exactitud numérica y el flujo de recomendación por similitud;
no aporta juicios humanos de relevancia, pruebas de carga concurrente ni calidad
de respuestas RAG. El catálogo completo de la fuente contiene 100,000 artículos;
esta prueba usa 10,000. HNSW se incorporó después de verificar el núcleo contra
su tarjeta; la validación aproximada se describe a continuación.

## Resultado registrado de fuerza bruta

La ejecución local de 2026-10-12 pasó **11/11 CTest**, con **40/40** búsquedas y
**4,000 vecinos** contrastados. El mayor error absoluto fue `5.99890916941348e-08`,
menor que `1e-5`. La mediana de búsqueda en Release fue **10.586855 ms** para
coseno y **9.914167 ms** para euclidiana al cuadrado. La mediana del proceso entero
fue aproximadamente 2.14 s y 2.16 s, respectivamente; carga CSV y demás fases
quedan fuera del tiempo de búsqueda mostrado.

Por ejemplo, `query_00.csv` corresponde al artículo real **40th Flight Test Squadron**.
Sus primeros resultados por coseno fueron:

| Rank | Vector ID | Artículo original | Distancia |
|---|---:|---|---:|
| 1 | 33743 | No. 40 Squadron RAAF | 0.511461020 |
| 2 | 48471 | 453d Electronic Warfare Squadron | 0.539448142 |
| 3 | 18175 | 380th Air Expeditionary Wing | 0.552062094 |

Los títulos permiten inspeccionar el sentido del caso de recomendación. No se
presentan como juicios humanos de relevancia. Consultar
[la evidencia completa](architecture/cli_output_validation.md) para los logs,
el informe de las 40 consultas y el manifest reproducible.

## HNSW sobre el mismo catálogo

El mismo build Release admite ahora:

```sh
./build-cli-release/aetherindex search --dataset datasets/real/dbpedia/vectors.csv --query datasets/real/dbpedia/queries/query_00.csv --algorithm hnsw --k 10 --ef-search 100 --seed 42
ctest --test-dir build-cli-release -R '^integration.hnsw_real_dataset$' --output-on-failure -V
```

La prueba opcional utiliza los 10,000 vectores y las mismas 20 consultas
independientes, construye **un único grafo** y compara Top-10 con fuerza bruta
para efSearch 50 y 100. Comprueba IDs, distancias, orden, ausencia de duplicados
e invariantes antes y después. Guarda los IDs exactos y aproximados por consulta
en `tests/hnsw-real-data-Release.csv` dentro del build. El criterio adicional del
caso real es Recall@10 promedio ≥ 0.85 a efSearch 100. La aceptación principal de
la tarjeta sigue siendo el protocolo de 1,000 × 128 y 100 consultas por métrica.

La suite completa con `AETHER_REAL_DATA_DIR` registra **18 tests**: 16 normales,
la comprobación exacta con NumPy y esta comparación HNSW/BruteForce. No necesita
descargar nuevamente los datos. El test HNSW usa solo el núcleo C++ y biblioteca
estándar; NumPy corresponde al test exacto ya existente.

La CLI construye el grafo cada vez que se ejecuta. `Index build time (ms)`
permite observar ese costo separado de `Query time (ms)`. La comparación que
reutiliza el grafo corresponde al test, y no describe una persistencia o caché
que la CLI todavía no tiene. Consultar [la evidencia HNSW](architecture/hnsw_validation.md).
