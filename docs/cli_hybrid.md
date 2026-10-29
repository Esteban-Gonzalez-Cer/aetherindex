# Búsqueda híbrida HNSW + BM25 + RRF desde la CLI (CP-04)

**Actualización:** 28 de octubre de 2026. La CLI ejecuta el recorrido completo
CLI → SearchService → QueryEngine → {HNSW, BM25} → RRF → resultados documentales,
en un único proceso y sin persistencia. Se conservan los modos
[vectorial](cli.md) y [léxico](cli_bm25.md).

## Ejecutar

```sh
cmake -S . -B build-cli -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build-cli --parallel 4
./build-cli/aetherindex search --mode hybrid --dataset datasets/hybrid/corpus.jsonl \
    --query datasets/hybrid/queries.jsonl --query-id Q002 --k 5
```

En Visual Studio: `.\build-cli\Release\aetherindex.exe` con el mismo comando.

## Flujo implementado

```text
CLI (--mode hybrid)
 └─ SearchService
     ├─ carga JSONL: DocumentRecord(título, texto, embedding) + consulta(texto, embedding)
     ├─ VectorStore (VectorId = DocumentId) → HnswIndex
     ├─ Bm25Index sobre título + "\n" + texto
     └─ QueryEngine::search(HnswIndex, VectorDocumentBinding, Bm25Index, RrfFusion, HybridQuery)
          ├─ HNSW.search(embedding, candidate_k) → VectorId → DocumentId (binding explícito)
          ├─ BM25.search(texto, candidate_k)
          └─ RRF.fuse(denso, léxico, K)
 └─ CLI: tabla Rank | Document ID | RRF Score | Dense Rank | Lexical Rank
```

- `QueryEngine` solo orquesta; no duplica HNSW, BM25 ni RRF.
- La asociación `VectorDocumentBinding` es obligatoria: un VectorId sin documento
  produce `logic_error` (defecto bloqueante), y una asociación no inyectiva se
  detecta porque RRF rechaza DocumentId repetidos en un ranking.
- Cada ranking se conserva en su orden: HNSW por distancia creciente y BM25 por
  score decreciente. Las columnas `Dense Rank` y `Lexical Rank` permiten verificar
  cada score RRF: `1/(k_rrf + rank_denso) + 1/(k_rrf + rank_léxico)`; `-` indica
  que el documento no estaba entre los candidatos de ese recuperador.
- Si BM25 no encuentra coincidencias, RRF recibe una lista léxica vacía y conserva
  los candidatos densos; la salida lo indica explícitamente.
- K mayor que la unión de candidatos devuelve solo los documentos disponibles.

## Opciones

| Opción | Contrato |
|---|---|
| `--mode hybrid` | Obligatoria. El canal denso es siempre HNSW; `--algorithm hnsw` se acepta y otros algoritmos se rechazan. |
| `--dataset FILE` | JSONL de documentos con `document_id`, `text`, `title` opcional y `embedding` no nulo. |
| `--query FILE` | JSONL de consultas con `query_text` **y** `embedding`. `--query-text` se rechaza: falta el embedding. |
| `--query-id ID` | Obligatoria si el archivo tiene varias consultas. |
| `--k N` | Top-K final (default 10). |
| `--candidate-k N` | Candidatos pedidos a cada recuperador, ≥ K. Default `max(K, 50)`. |
| `--rrf-k X` | Constante RRF finita y positiva; default 60. |
| `--vectors FILE.avec` | Embeddings binarios de los documentos (de `convert`); el JSONL puede omitir `embedding`. Debe haber exactamente un vector por DocumentId. |
| `--metric`, `--m`, `--m0`, `--ef-construction`, `--ef-search`, `--seed`, `--neighbor-selection` | Configuración HNSW, como en modo vectorial. |
| `--bm25-k1`, `--bm25-b` | Configuración BM25, como en modo léxico. |

`--candidate-k` y `--rrf-k` se rechazan fuera del modo híbrido. Códigos de salida:
0 éxito, 2 argumentos/configuración, 3 datos de entrada (archivo inexistente,
JSON inválido, embedding nulo o de dimensión incorrecta, consulta sin texto) y 4
error interno. Los errores no imprimen resultados parciales.

## Identidad documental

`document_id` uint32 canónico se conserva; los IDs opacos (`D001`) reciben un
DocumentId determinista independiente del orden de las filas. El embedding de cada
documento se almacena con `VectorId = DocumentId`, y la asociación se pasa
explícitamente a QueryEngine. Así, `DocumentId_dense = DocumentId_sparse` antes de
la fusión. La prueba `integration.hybrid_retrieval` también usa VectorIds distintos
de los DocumentIds (7, 8, 9 → 101, 102, 103) para demostrar que el motor no asume
igualdad numérica.

## Tiempos

`Dense index build time` (HNSW) y `Lexical index build time` (BM25) se miden por
separado. `Query time` cubre HNSW + BM25 + RRF dentro de QueryEngine y excluye
carga de archivos, construcción y presentación.

## Datos de ejemplo y límites

`datasets/hybrid/` deriva del corpus CP-01 con embeddings *hashing* de trigramas
generados fuera del motor (ver [su README](../datasets/hybrid/README.md)). Sirven
para verificar la integración; **no** son un modelo semántico y no demuestran
beneficio de relevancia del modo híbrido. Para embeddings de un modelo, basta con
reemplazar el campo `embedding` de documentos y consultas manteniendo el formato.

## Pruebas

```sh
ctest --test-dir build-cli -L hybrid --output-on-failure
```

- `integration.hybrid_retrieval`: caso matemático de referencia a través del motor,
  binding con IDs distintos, sin coincidencias léxicas, K mayor que los documentos,
  dimensión inválida, archivos inexistentes/mal formados, embedding nulo, parámetros
  inválidos y repetibilidad después de errores.
- `e2e.cli_hybrid`: las 24 consultas del corpus por el ejecutable, tabla ordenada y
  sin duplicados, parámetros explícitos y códigos de error.
- `acceptance.kr1_5_hybrid`: KR1.5; recalcula de forma independiente cada fusión.
