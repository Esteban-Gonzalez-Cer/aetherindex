# AetherIndex — Comandos para la demostración en macOS

> Evidencia histórica del baseline previo a las optimizaciones del 19-10-2026.
> Describe el código y las mediciones de esa versión; las cifras y salidas
> guardadas no se recalcularon. Para el contrato y resultados actuales, consultar
> [la validación de optimizaciones](../benchmarks/OPTIMIZATION_VALIDATION.md).

Guion explicado: [SPRINT_1_SHOWCASE.md](SPRINT_1_SHOWCASE.md).
Ejecutar desde la raíz del proyecto. Los bloques con la consulta 13 son
opcionales; el caso principal utiliza la consulta 0.

## Antes de presentar

```sh
cd /Users/estebangonzalezcervantes/Documents/aetherindex
cmake -S . -B build-showcase -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build-showcase --parallel 4
mkdir -p build-showcase/evidence
set -o pipefail
ctest --test-dir build-showcase -C Release -R sprint1_okr1 --output-on-failure
python3 scripts/showcase_results.py --query-index 0
```

Los embeddings reales deben estar preparados previamente. En este Mac ya lo
están. Para otra máquina, consultar la preparación del guion antes de presentar.

## 1. Ayuda y formato

```sh
./build-showcase/aetherindex --help
cat datasets/examples/vectors.csv
cat datasets/examples/query.csv
./build-showcase/aetherindex search --dataset datasets/examples/vectors.csv --query datasets/examples/query.csv --algorithm brute-force --metric cosine --k 100
```

Explicar: una fila por vector, ID separado de componentes, menor distancia
primero, nueve decimales, cuatro resultados porque solo existen cuatro vectores.

## 2. Artículos reales: consulta y referencia exacta

```sh
python3 scripts/showcase_results.py --query-index 0
./build-showcase/aetherindex search --dataset datasets/real/dbpedia/vectors.csv --query datasets/real/dbpedia/queries/query_00.csv --algorithm brute-force --metric cosine --k 10 | tee build-showcase/evidence/dbpedia-query00-brute-force.txt
python3 scripts/showcase_results.py --query-index 0 --results build-showcase/evidence/dbpedia-query00-brute-force.txt
```

Explicar: 10,000 artículos, 1,024 dimensiones, consulta excluida del catálogo,
recorrido completo y ordenamiento. Los títulos los añade el script externo.

## 3. HNSW y comparación

```sh
./build-showcase/aetherindex search --dataset datasets/real/dbpedia/vectors.csv --query datasets/real/dbpedia/queries/query_00.csv --algorithm hnsw --metric cosine --k 10 --m 16 --m0 32 --ef-construction 100 --ef-search 50 --seed 42 | tee build-showcase/evidence/dbpedia-query00-hnsw-50.txt
python3 scripts/showcase_results.py --query-index 0 --results build-showcase/evidence/dbpedia-query00-hnsw-50.txt --reference build-showcase/evidence/dbpedia-query00-brute-force.txt
```

Explicar el grafo mientras se construye. En el ensayo tardó unos 24 segundos el
proceso; la búsqueda dentro del grafo tardó aproximadamente 0.9 ms. La CLI
reconstruye el índice en cada invocación. Este caso recuperó 10/10 IDs exactos.

## 4. Opcional: una omisión y mayor exploración

```sh
python3 scripts/showcase_results.py --query-index 13
./build-showcase/aetherindex search --dataset datasets/real/dbpedia/vectors.csv --query datasets/real/dbpedia/queries/query_13.csv --algorithm brute-force --metric cosine --k 10 | tee build-showcase/evidence/dbpedia-query13-brute-force.txt
./build-showcase/aetherindex search --dataset datasets/real/dbpedia/vectors.csv --query datasets/real/dbpedia/queries/query_13.csv --algorithm hnsw --metric cosine --k 10 --m 16 --m0 32 --ef-construction 100 --ef-search 50 --seed 42 | tee build-showcase/evidence/dbpedia-query13-hnsw-50.txt
python3 scripts/showcase_results.py --query-index 13 --results build-showcase/evidence/dbpedia-query13-hnsw-50.txt --reference build-showcase/evidence/dbpedia-query13-brute-force.txt
./build-showcase/aetherindex search --dataset datasets/real/dbpedia/vectors.csv --query datasets/real/dbpedia/queries/query_13.csv --algorithm hnsw --metric cosine --k 10 --m 16 --m0 32 --ef-construction 100 --ef-search 100 --seed 42 | tee build-showcase/evidence/dbpedia-query13-hnsw-100.txt
python3 scripts/showcase_results.py --query-index 13 --results build-showcase/evidence/dbpedia-query13-hnsw-100.txt --reference build-showcase/evidence/dbpedia-query13-brute-force.txt
```

En el ensayo: 9/10 a efSearch=50 y 10/10 a efSearch=100. Explicar que K sigue en
10; la amplitud de exploración cambia. Recall mide coincidencia con vecinos
exactos, no relevancia humana. Los dos comandos reconstruyen el grafo.

## 5. Aceptación reproducible y cierre

```sh
ctest --test-dir build-showcase -C Release -R '^integration.sprint1_okr1$' --output-on-failure -V
```

Mostrar: 1,000 × 128 embeddings, 100 consultas, inserción 500 + 500, cero
violaciones y Mean Recall@10=0.990 frente a meta 0.85. Estos datos sintéticos
pertenecen al protocolo de aceptación, distinto del catálogo real de la demo.

Cerrar con almacenamiento + distancias + BruteForce + HNSW + CLI. La generación
de embeddings, respuestas de LLM, BM25, RRF y persistencia quedan fuera de la
versión mostrada. Los tiempos del ensayo son observaciones, no garantías.

## Respaldo anunciado como grabación del ensayo

```sh
cat docs/showcase/evidence/2026-10-16/dbpedia-query00-hnsw-50.txt
```

Indicar que es una salida guardada del 16 de octubre de 2026.
