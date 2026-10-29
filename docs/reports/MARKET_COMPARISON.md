# Comparación de AetherIndex con productos del mercado

**Fecha local:** 29 de octubre de 2026 · **Base:** `ff80080` con los cambios de la rama
`feature/market-benchmarks` (sin commit) · **Máquina:** Apple M4 Pro (10 núcleos de
rendimiento + 4 de eficiencia, 24 GB), macOS 26.5, Apple clang 17.

Este informe mide AetherIndex frente a hnswlib, FAISS, Lucene (Anserini), bm25s, Qdrant y
OpenSearch con datos públicos y un protocolo de equidad explícito. Incluye los resultados que
perdemos. Todas las tablas salen de
[`evaluation/reports/market/summary.md`](../../evaluation/reports/market/summary.md), que
`scripts/summarize_market.py` regenera a partir de los archivos crudos; los arneses y el modo
de reproducirlos están en [`benchmarks/market/`](../../benchmarks/market/README.md).

## Resumen

- **Calidad léxica igual a Lucene en los 6 datasets.** Con el analizador opt-in
  `english-v1` (99.993 % de tokens idénticos a Lucene) y la ponderación opt-in
  `--bm25-query-weighting frequency`, el nDCG@10 medio de BM25 es 0.5030 frente a 0.5024 de
  Lucene, sin diferencia significativa en 5 datasets y +0.003 en ArguAna. Con la ponderación
  por defecto (cada término una vez) empatamos en 5 datasets y perdemos 0.079 en ArguAna,
  cuyas consultas son argumentos completos. Ver
  [Ponderación de la consulta](#ponderación-de-términos-repetidos-en-la-consulta).
- **HNSW más rápido que hnswlib y FAISS con la misma calidad.** En SIFT1M, a Recall@10 ≥
  0.95, AetherIndex sirve 94.5 K QPS frente a 81.6 K de hnswlib compilado con
  `-ffast-math` (su mejor caso), 61.1 K de hnswlib estándar y 50.4 K de FAISS HNSWFlat.
  En GloVe-100 ganamos a 0.90 (20.6 K frente a 18.4 K) pero hnswlib con `-ffast-math` nos
  supera a 0.95 (10.0 K frente a 8.1 K).
- **Búsqueda densa sin pérdida.** Sobre los embeddings bge-small de BEIR, HNSW de AetherIndex
  iguala la búsqueda exacta en nDCG@10 (media 0.6174 frente a 0.6178, sin diferencia
  significativa) y la cuantización SQ8 no cambia la calidad (0.6173).
- **Híbrido RRF correcto y útil.** La fusión reproduce exactamente una RRF de referencia
  sobre los mismos runs (6 de 6 datasets). El híbrido mejora a BM25 en los 6 datasets
  (+0.04 a +0.21 de nDCG@10, todos significativos). Con ponderación por frecuencia iguala a
  la referencia Lucene + exacto + RRF (media 0.6086 frente a 0.6088) y a OpenSearch.
- **Búsqueda densa en proceso de 4 a 11 veces más rápida que los servidores.** HNSW de
  AetherIndex responde en 0.08–0.38 ms frente a 0.58–2.38 ms de Qdrant y OpenSearch medidos
  desde el cliente. Esa diferencia incluye la red local y la serialización, así que no es
  una comparación de motores.
- **BM25 con MaxScore: a la par de Lucene en consultas largas, todavía detrás en colecciones
  grandes.** La nueva estrategia `maxscore` reduce la latencia de BMW de 7.90 a 0.54 ms en
  ArguAna (Lucene 0.55 ms) y de 1.49 a 0.67 ms en TREC-COVID, con los mismos resultados bit a
  bit. Ganamos en SciFact y NFCorpus y empatamos en ArguAna; Lucene sigue siendo de 1.2 a 1.8
  veces más rápido con un hilo en FiQA, TREC-COVID y Quora. Ver
  [Estrategia MaxScore](#estrategia-maxscore).

## Protocolo de equidad

1. **Mismo entorno.** Misma máquina, mismos archivos de datos, mismo `k`, mismo código de
   métricas. Latencia con un hilo; throughput con 10 hilos (los núcleos de rendimiento).
   Cada sistema y configuración corre en un proceso nuevo y sin otra carga; los servidores
   corren de uno en uno.
2. **Cada sistema se ajusta, no solo el nuestro.** Curvas recall–QPS sobre la misma rejilla
   (M ∈ {16, 32}, efConstruction ∈ {100, 200}, ef de 10 a 800) y se compara el mejor punto
   de cada sistema a un recall objetivo. hnswlib 0.8.0 no tiene kernels SIMD para ARM, así
   que también se mide compilado con `-ffast-math`, que permite al compilador vectorizarlo.
3. **Misma capa de llamada.** hnswlib y FAISS corren dentro del mismo arnés C++ que
   AetherIndex (`ann_compare.cpp`): mismo compilador, mismas opciones, mismo temporizador.
   Lucene se mide dentro de la JVM tras calentamiento (`LuceneSearch.java`), con las
   consultas construidas como en Anserini. Los servidores incluyen la ida y vuelta por la
   red local y se presentan aparte, marcados con †.
4. **Análisis equivalente.** Mismos k1/b (0.9/0.4 de BEIR y 1.2/0.75). Lucene con Porter
   frente a `english-v1`, y Lucene sin stemming ni stopwords frente a `ascii-tech-v1`. bm25s
   por defecto y con exactamente los tokens de `english-v1`. Qdrant con vectores dispersos
   BM25 calculados con la misma fórmula y el modificador IDF. Mismos embeddings
   (bge-small-en-v1.5) para todos los sistemas densos.
5. **Rigor.** Cinco repeticiones (mediana e IQR). Recall contra el ground truth oficial;
   nDCG@10, Recall@100 y MRR@10 con un evaluador propio que coincide con `trec_eval` (máxima
   diferencia 0.00005, el redondeo de `trec_eval`). Prueba de aleatorización pareada por
   consulta con 10 000 permutaciones.
6. **Transparencia.** Versiones, digests de imágenes, hashes de datos y binarios, runs por
   consulta y logs en `evaluation/reports/market/` (manifiesto en `manifest.json`).

### Controles del pipeline

Antes de comparar se verificó que el pipeline mide bien:

| Control | Resultado |
|---|---|
| Lucene reproduce su resultado publicado de BEIR (±0.005) | Diferencias de 0.0000 a −0.0012 en los 6 datasets |
| Evaluador propio frente a `trec_eval` | Idénticos (diferencia máxima 0.00005) |
| FAISS Flat recall = 1 | 0.9994 en SIFT1M (empates de distancia en el ground truth) y 1.0000 en GloVe-100 |
| RRF de referencia en Python = híbrido de AetherIndex con las mismas entradas | 6 de 6 datasets idénticos |
| Tokens de `english-v1` = tokens de Lucene `EnglishAnalyzer` (SciFact) | 99.9933 % (838 072 de 838 128) |

Las 56 diferencias de tokens vienen de ® © ™ ↔ (Lucene los emite como tokens de emoji), del
guion suave y de caracteres U+FFFD presentes en el texto original.

## Entorno y versiones

| Componente | Versión |
|---|---|
| AetherIndex | `ff80080` + cambios de esta rama, Release, Apple clang 17 |
| hnswlib | 0.8.0 (`3f34296`), mismo compilador; variante adicional con `-ffast-math` |
| FAISS | 1.15.1 (Homebrew), OpenMP para lotes |
| Lucene | Anserini 2.3.0 (Lucene 10), OpenJDK 24, `-Xms4g -Xmx4g` |
| bm25s | 0.3.16 con PyStemmer 3.1.0 |
| Qdrant | 1.20.0 en Docker (10 CPU, 7 GB), cliente gRPC 1.20.0 |
| OpenSearch | 3.9.0 en Docker (10 CPU, 7 GB, heap 3 GB), k-NN con motor faiss |
| Embeddings | `BAAI/bge-small-en-v1.5` (384 dim, normalizados) en MPS |

Datos: SIFT1M (1 M × 128, L2), GloVe-100-angular de ann-benchmarks (1.18 M × 100,
normalizado), y BEIR test: SciFact (5 183 docs / 300 consultas), NFCorpus (3 633 / 323),
FiQA (57 638 / 648), ArguAna (8 674 / 1 406), TREC-COVID (171 332 / 50) y Quora
(522 931 / 10 000). Se indexa `título + "\n" + texto`.

## Resultados léxicos (BM25)

nDCG@10 con k1 = 0.9 y b = 0.4:

| Sistema | SciFact | NFCorpus | FiQA | ArguAna | TREC-COVID | Quora | Media |
|---|---:|---:|---:|---:|---:|---:|---:|
| Lucene BM25 (Porter) | 0.6789 | 0.3206 | 0.2361 | 0.3970 | 0.5935 | 0.7884 | 0.5024 |
| AetherIndex `english-v1` | 0.6795 | 0.3207 | 0.2378 | 0.3178 | 0.5964 | 0.7893 | 0.4903 |
| **AetherIndex `english-v1`, frecuencia** | **0.6777** | **0.3202** | **0.2353** | **0.4003** | **0.5960** | **0.7885** | **0.5030** |
| bm25s (tokens `english-v1`) | 0.6777 | 0.3202 | 0.2353 | 0.4003 | 0.5960 | 0.7885 | 0.5030 |
| bm25s (por defecto) | 0.6762 | 0.3177 | 0.2385 | 0.4080 | 0.5887 | 0.7870 | 0.5027 |
| Qdrant BM25 disperso | 0.6795 | 0.3207 | 0.2378 | 0.3180 | 0.5964 | 0.7891 | 0.4903 |
| OpenSearch BM25 | 0.6789 | 0.3206 | 0.2361 | 0.3970 | 0.5935 | 0.7884 | 0.5024 |

Diferencia con Lucene (prueba pareada, en el orden de la tabla):

| AetherIndex | SciFact | NFCorpus | FiQA | ArguAna | TREC-COVID | Quora |
|---|---:|---:|---:|---:|---:|---:|
| `unique` (por defecto) | +0.0006 (p = 0.85) | +0.0002 (p = 0.86) | +0.0018 (p = 0.46) | **−0.0792 (p < 0.001)** | +0.0029 (p = 0.44) | +0.0009 (p = 0.09) |
| `frequency` | −0.0012 (p = 0.20) | −0.0003 (p = 0.60) | −0.0007 (p = 0.54) | **+0.0033 (p = 0.001)** | +0.0025 (p = 0.46) | +0.0000 (p = 0.12) |

Dos controles cruzados confirman que la fórmula BM25 es la misma y que la única diferencia
es el peso de la consulta: Qdrant, que recibe cada término una vez, da exactamente los
números de `unique`, y bm25s con los mismos tokens, que suma los términos repetidos, da
exactamente los de `frequency` (nDCG@10 idéntico a 4 decimales en los 6 datasets).

Sin stemming, `ascii-tech-v1` queda por debajo de Lucene sin stemming ni stopwords en los 6
datasets (media 0.4581 frente a 0.4821, todas significativas): `ascii-tech-v1` está pensado
para identificadores técnicos y no elimina stopwords. Con k1 = 1.2 y b = 0.75 el patrón se
repite: con `unique` perdemos en ArguAna (0.4062 frente a 0.4780); con `frequency` la media es
0.5268 frente a 0.5263 de Lucene y ArguAna sube a 0.4823 (+0.004, p = 0.002).

### Coste léxico

Latencia p50 con un hilo y QPS con 10 hilos (k = 10):

| Dataset | AetherIndex BMW | AetherIndex MaxScore | Lucene | bm25s |
|---|---|---|---|---|
| SciFact | **0.064 ms** · 102 K | 0.068 ms · **113 K** | 0.077 ms · 45 K | 0.125 ms · 10 K |
| NFCorpus | **0.005 ms · 663 K** | 0.006 ms · 511 K | 0.024 ms · 76 K | 0.086 ms · 21 K |
| FiQA | 0.519 ms · 13.9 K | 0.278 ms · 26.8 K | **0.165 ms · 46.5 K** | 0.458 ms · 6.9 K |
| ArguAna | 7.901 ms · 0.9 K | 0.544 ms · 14.5 K | 0.553 ms · **15.4 K** | **0.347 ms** · 1.3 K |
| TREC-COVID | 1.487 ms · 3.0 K | 0.667 ms · 9.4 K | **0.375 ms · 14.1 K** | 1.399 ms · 3.4 K |
| Quora | 0.339 ms · 17.6 K | 0.248 ms · 28.4 K | **0.205 ms · 40.1 K** | 2.550 ms · 3.1 K |

Ambas columnas de AetherIndex usan la ponderación por defecto; con `frequency` las cifras son
prácticamente iguales (`summary.md`).

Construcción con un hilo: AetherIndex indexa de 1.5 a 6.4 veces más rápido que Lucene
(TREC-COVID 3.9 s frente a 6.0 s; el tiempo de Lucene incluye el arranque de la JVM y la
fusión a un segmento) y a la par de bm25s. Los postings comprimidos de AetherIndex ocupan
de 1.2 a 3.0 veces lo que el índice de Lucene (TREC-COVID 40.6 MB frente a 26.5 MB) y
menos que bm25s (104.6 MB).

## Resultados ANN

Mejor QPS (10 hilos) de cada sistema con Recall@10 ≥ objetivo, sobre toda la rejilla:

| Sistema | SIFT1M @0.90 | @0.95 | @0.99 | GloVe-100 @0.90 | @0.95 |
|---|---:|---:|---:|---:|---:|
| **AetherIndex HNSW** | **149.9 K** | **94.5 K** | **44.3 K** | **20.6 K** | 8.1 K |
| hnswlib 0.8.0 | 97.5 K | 61.1 K | 29.1 K | 14.8 K | 8.0 K |
| hnswlib 0.8.0 (`-ffast-math`) | 134.2 K | 81.6 K | 42.2 K | 18.4 K | **10.0 K** |
| FAISS HNSWFlat | 81.8 K | 50.4 K | 25.9 K | 10.0 K | 5.3 K |
| AetherIndex SQ8 + re-rank | 202.9 K | 121.2 K | 53.7 K | 16.9 K | 8.5 K |
| FAISS HNSWSQ8 | 119.2 K | 67.7 K | — | 5.1 K | — |
| AetherIndex PQ + re-rank | 129.1 K | 97.5 K | 45.9 K | 15.1 K | — |
| FAISS IVFPQ + refine | 58.2 K | 32.9 K | — | — | — |

Curvas completas: [SIFT1M](../../evaluation/reports/market/plots/sift1m_qps.svg),
[GloVe-100](../../evaluation/reports/market/plots/glove-100_qps.svg) (10 hilos) y sus
versiones con un hilo (`*_qps1.svg`). Ningún sistema alcanza 0.99 en GloVe-100 con esta
rejilla; el máximo de AetherIndex es 0.9859 y el de hnswlib 0.9798.

Construcción (M = 16, efConstruction = 200) y memoria:

| Sistema | SIFT1M 10 hilos | 1 hilo | GloVe 10 hilos | 1 hilo | Estructura SIFT1M |
|---|---:|---:|---:|---:|---:|
| AetherIndex HNSW | 42.2 s | 369 s | 62.6 s | 531 s | 624 MiB |
| hnswlib 0.8.0 | 41.1 s | 360 s | 53.3 s | 462 s | 626 MiB |
| hnswlib (`-ffast-math`) | 28.9 s | — | 39.8 s | — | 626 MiB |
| FAISS HNSWFlat | 53.9 s | 347 s | 75.9 s | 492 s | 626 MiB |
| AetherIndex SQ8 sin FP32 | 40.5 s | — | 58.8 s | — | 262 MiB |
| FAISS HNSWSQ8 | 46.5 s | — | 106.1 s | — | 260 MiB |

La memoria de las estructuras es equivalente. La memoria del proceso (`phys_footprint`)
también se publica en `summary.md`; en FAISS IVFPQ incluye 294 MiB de temporales de
entrenamiento que libmalloc de macOS conserva.

### Servidores (†)

Con M = 16 y ef_construct = 200, Qdrant alcanza Recall@10 de 0.9638 en SIFT1M con ef = 32
y ~5.7 K QPS desde 10 clientes; OpenSearch llega a 0.9157 con el mismo ef y ~5.0 K QPS. En
GloVe-100 con ef = 512, Qdrant da 0.9604 y OpenSearch 0.9322. A igual ef Qdrant obtiene más
recall que las librerías porque reparte la colección en varios segmentos y busca cada uno
con ese ef, así que su ef no equivale al de un solo grafo. La ingesta más el índice tarda
66 s (Qdrant) y 439 s (OpenSearch) en SIFT1M; los contenedores usan 3.4 GiB y 6.6 GiB.

## Resultados densos e híbridos en BEIR

nDCG@10 (HNSW M = 16, efConstruction = 200, ef = 128; híbrido RRF k = 60 con 50 candidatos
por rama):

| Sistema | SciFact | NFCorpus | FiQA | ArguAna | TREC-COVID | Quora | Media |
|---|---:|---:|---:|---:|---:|---:|---:|
| Denso exacto | 0.7127 | 0.3430 | 0.4035 | 0.6035 | 0.7578 | 0.8864 | 0.6178 |
| **AetherIndex HNSW** | 0.7127 | 0.3430 | 0.4014 | 0.6035 | 0.7578 | 0.8861 | 0.6174 |
| AetherIndex HNSW SQ8 | 0.7127 | 0.3424 | 0.4014 | 0.6035 | 0.7578 | 0.8861 | 0.6173 |
| Qdrant HNSW | 0.7127 | 0.3430 | 0.3989 | 0.6035 | 0.7557 | 0.8863 | 0.6167 |
| OpenSearch k-NN | 0.7127 | 0.3425 | 0.4005 | 0.6035 | 0.7578 | 0.8855 | 0.6171 |
| AetherIndex híbrido | 0.7303 | 0.3620 | 0.3569 | 0.4831 | 0.8026 | 0.8662 | 0.6002 |
| **AetherIndex híbrido, frecuencia** | 0.7292 | 0.3612 | 0.3586 | 0.5372 | 0.7994 | 0.8662 | 0.6086 |
| Referencia Lucene + exacto + RRF | 0.7273 | 0.3618 | 0.3609 | 0.5354 | 0.8009 | 0.8663 | 0.6088 |
| Qdrant híbrido | 0.7302 | 0.3626 | 0.3545 | 0.4800 | 0.8007 | 0.8669 | 0.5991 |
| OpenSearch híbrido | 0.7286 | 0.3607 | 0.3588 | 0.5349 | 0.7986 | 0.8652 | 0.6078 |

- Con la ponderación por defecto, el híbrido de AetherIndex no difiere significativamente de
  la referencia salvo en ArguAna (−0.052), donde hereda la pérdida léxica; Qdrant, que también
  cuenta cada término una vez, muestra lo mismo (0.4800). Con `frequency` la diferencia con la
  referencia es como mucho de 0.002 en cada dataset (ArguAna 0.5372 frente a 0.5354) y la
  fusión sigue reproduciendo exactamente la RRF de referencia (6 de 6 datasets).
- El híbrido supera al denso exacto en NFCorpus y TREC-COVID (significativo; en SciFact la
  ventaja no lo es) y queda por debajo en FiQA, ArguAna y Quora: con un modelo denso fuerte,
  RRF con pesos iguales puede diluir la rama mejor. Es una propiedad de RRF, no de nuestra implementación, y la referencia y los
  servidores se comportan igual.
- En SciFact, NFCorpus y ArguAna, Qdrant no construye HNSW para los vectores densos porque la
  colección queda por debajo de su `indexing_threshold` por defecto, y busca de forma exacta.
  Se dejó así porque es su comportamiento estándar.

Sobre los mismos vectores bge-small, a igual ef, HNSW de AetherIndex obtiene recall igual o
mayor que hnswlib y FAISS en todos los puntos medidos y más QPS en todos salvo uno (TREC-COVID
con ef = 64, donde FAISS empata en recall y es un 3 % más rápido). Por ejemplo, FiQA con
ef = 64: 0.978 y 66 K QPS frente a 0.962 y 43 K de hnswlib y 0.963 y 57 K de FAISS.

## Ponderación de términos repetidos en la consulta

La primera pasada de la campaña mostró la única pérdida de calidad léxica: −0.079 de
nDCG@10 frente a Lucene en ArguAna y −0.052 en el híbrido. La causa no era el analizador ni la
fórmula: AetherIndex contaba una vez cada término distinto de la consulta, mientras que Lucene
(vía Anserini) pondera cada término por sus repeticiones y bm25s suma los duplicados. Las
consultas de ArguAna son argumentos completos (mediana de 174 palabras), así que la
repetición pesa; en los demás datasets las consultas son cortas y no había diferencia.

Se añadió `Bm25Config::query_weighting` (`QueryTermWeighting::Unique` por defecto o
`Frequency`) y la opción `--bm25-query-weighting unique|frequency` de la CLI. Con
`frequency`, la contribución de cada término se multiplica por sus apariciones en la consulta
analizada; las cuatro estrategias aplican el peso también a sus cotas superiores y siguen
devolviendo scores idénticos bit a bit (pruebas unitarias con consultas de hasta 25 términos
repetidos y k ∈ {1, 10, 100}). El valor por defecto no cambia la salida: los runs BM25 por
defecto de SciFact, ArguAna y Quora, en BMW y exhaustivo, son idénticos byte a byte a los de
la primera pasada. La ponderación no cambia la latencia (ArguAna BMW 7.47 ms frente a 7.90 ms).

## Estrategia MaxScore

La primera versión de este informe mostraba que BlockMaxWand, la estrategia léxica más rápida
hasta entonces, era lenta con consultas largas: 7.9 ms en ArguAna, más que la búsqueda
exhaustiva (1.3 ms) y que la acumulación término a término (0.28 ms). WAND reordena todos los
cursores en cada paso; con 93 términos por consulta ese coste domina.

Se añadió `Bm25Strategy::MaxScore` (`--bm25-strategy maxscore`), la poda de disyunciones que
usa Lucene (Turtle y Flood, 1995), en ventanas de 4,096 ordinales como el
`MaxScoreBulkScorer` de Lucene:

- en cada ventana, la cota de cada término es la mayor cota de bloque que la solapa;
- los términos de menor cota cuya suma no alcanza el K-ésimo score son no esenciales: un
  documento que solo los contiene no puede entrar en el Top-K;
- los términos esenciales se acumulan en un búfer de la ventana, sin montículo;
- cada documento tocado consulta los no esenciales, de mayor a menor cota, solo mientras aún
  puede alcanzar el umbral;
- los documentos que sobreviven se puntúan sumando sus contribuciones en orden de término,
  igual que las demás estrategias, así que los scores son idénticos bit a bit.

Una primera versión sin ventanas (montículo de cursores y cotas globales) ya bajaba ArguAna a
1.0 ms; con ventanas y cotas de bloque baja a 0.48 ms en la medición rápida. Se probaron
ventanas de 512 a 8,192 documentos y 4,096 fue la mejor o casi la mejor en los cuatro datasets
grandes.

Verificación: `unit.bm25_strategies` compara 18,960 consultas/estrategias con la ruta
exhaustiva (incluye consultas de 20 a 150 términos, ambas ponderaciones, k de 1 a 1,000 y
empates masivos); los filtros, el análisis `english-v1` y la CLI en modo léxico e híbrido
también se prueban con `maxscore`. En la campaña, los runs BM25 de `maxscore` son idénticos
byte a byte a los de `bmw` en los 6 datasets y con las dos ponderaciones, y el híbrido con
`maxscore` es idéntico al run determinista de control (HNSW construido con un hilo).

El híbrido se beneficia igual: con `frequency`, ArguAna pasa de 8.73 a 0.79 ms y TREC-COVID de
2.22 a 1.29 ms.

## Dónde perdemos y por qué

1. **Ponderación por defecto en consultas largas.** Sin `--bm25-query-weighting frequency`,
   BM25 pierde 0.079 de nDCG@10 frente a Lucene en ArguAna (ver la sección anterior). El
   valor por defecto se mantiene para no cambiar resultados existentes.
2. **Latencia léxica en colecciones grandes.** Incluso con `maxscore`, Lucene responde de 1.2
   a 1.8 veces más rápido con un hilo en FiQA, TREC-COVID y Quora, y su throughput con 10
   hilos es de 1.4 a 1.7 veces mayor. La diferencia que queda está sobre todo en la
   decodificación de postings: Lucene empaqueta bloques de bits con SIMD y AetherIndex usa
   varint. En TREC-COVID, Qdrant (con la red incluida) todavía responde algo antes con un hilo
   (0.61 ms frente a 0.67 ms), aunque nuestro throughput ya es mayor (9.4 K frente a 5.8 K
   QPS). Con BMW, la estrategia recomendada antes, la diferencia con Lucene era de 1.7 a 14
   veces.
3. **Índice léxico más grande.** Nuestros postings comprimidos ocupan de 1.2 a 3.0 veces el
   índice de Lucene.
4. **GloVe-100 a alto recall.** hnswlib compilado con `-ffast-math` nos supera a Recall@10 ≥
   0.95 (10.0 K frente a 8.1 K QPS) y construye más rápido (39.8 s frente a 62.6 s). Con las
   opciones estándar de hnswlib empatamos a 0.95 y ganamos a 0.90.
5. **Construcción con un hilo.** FAISS (347 s) y hnswlib (360 s) construyen SIFT1M algo más
   rápido que AetherIndex (369 s); en GloVe-100 hnswlib es un 13 % más rápido.
6. **`ascii-tech-v1` en texto natural.** Sin stopwords ni stemming pierde frente a Lucene sin
   stemming en los 6 datasets; para texto en inglés debe usarse `english-v1`.

## Limitaciones

- Una sola máquina (Apple Silicon); los resultados x86 con AVX pueden diferir, sobre todo
  para hnswlib y FAISS, que tienen kernels x86 más maduros.
- AetherIndex no tiene modo servidor, así que no hay comparación de latencia de servidor a
  servidor. Las cifras de Qdrant y OpenSearch incluyen la red local de Docker.
- La latencia con un hilo de los servidores varía de forma apreciable entre ejecuciones (por
  ejemplo, Qdrant en SIFT1M con ef = 64: 0.88 ms en la ejecución abortada y 1.57 ms en la
  definitiva); el throughput es estable.
- Durante la ejecución se corrigieron dos fallos del arnés de servidores: la espera de
  indexación de Qdrant no terminaba en ArguAna (un documento sin términos dispersos) y el
  canal gRPC fallaba tras la subida en paralelo. Todos los resultados de Qdrant publicados
  provienen del código corregido, salvo GloVe-100, que terminó sin error con el código
  previo; el cambio solo afecta a qué canal se usa. El log de la ejecución abortada se
  conserva en `servers/servers-aborted-wait-bug.log`.
- La campaña tiene tres pasadas. La primera (`manifest-pass1.json`) produjo todos los
  resultados salvo los de ponderación por frecuencia y MaxScore; la segunda
  (`manifest-pass2.json`, binario con `QueryTermWeighting`) añadió los runs `*-freq` y sus
  controles; la tercera (`manifest.json`, binario con `MaxScore`) añadió los runs
  `*-maxscore`. Antes de cada pasada se comprobó que el nuevo binario reproduce byte a byte
  los runs por defecto de la primera.
- Qdrant no se volvió a medir con ponderación por frecuencia; sus vectores dispersos de
  consulta admiten pesos, así que el mismo cambio aplicaría allí. OpenSearch ya pondera por
  repetición (su consulta `match` repite la cláusula de cada término) y por eso coincide con
  Lucene.
- La igualdad bit a bit entre estrategias BM25 con `frequency` depende de que el compilador
  no fusione multiplicaciones y sumas entre sentencias (FMA). Se verificó con Apple clang y
  Homebrew Clang, cuya opción por defecto solo fusiona dentro de una expresión; no se
  verificó con GCC, que por defecto puede fusionar más (`-ffp-contract=fast`). Fijar
  `-ffp-contract=off` no es una solución, porque cambia los scores por defecto con k1 < 1.

## Reproducir

```sh
python3 -m venv .venv-bench && .venv-bench/bin/pip install -r benchmarks/market/requirements.txt
cmake -S . -B build-market -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DAETHER_BUILD_MARKET_BENCH=ON
cmake --build build-market --parallel 10
python3 benchmarks/market/run_market_campaign.py
.venv-bench/bin/python scripts/summarize_market.py
```

Los pasos de datos, Anserini y servidores están en
[`benchmarks/market/README.md`](../../benchmarks/market/README.md). La campaña en proceso
tarda unas 3 h 10 min (más unos 20 min de la segunda pasada y 3 de la tercera) y los servidores unos 55 min.
