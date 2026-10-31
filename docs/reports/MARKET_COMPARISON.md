# Comparación de AetherIndex con productos del mercado

**Fecha local:** 30 de octubre de 2026 · **Base:** `ff80080` con los cambios de la rama
`feature/market-benchmarks` (sin commit) · **Máquina:** Apple M4 Pro (10 núcleos de
rendimiento + 4 de eficiencia, 24 GB), macOS 26.5, Apple clang 17.

Este informe mide AetherIndex frente a hnswlib, FAISS, Lucene (Anserini), bm25s, Qdrant y
OpenSearch con datos públicos y un protocolo de equidad explícito. Incluye los resultados que
perdemos. Todas las tablas salen de
[`evaluation/reports/market/summary.md`](../../evaluation/reports/market/summary.md), que
`scripts/summarize_market.py` regenera a partir de los archivos crudos; los arneses y el modo
de reproducirlos están en [`benchmarks/market/`](../../benchmarks/market/README.md).

**Valores por defecto.** A raíz de estos resultados, desde el 30 de octubre de 2026 BM25 usa
por defecto la estrategia `maxscore` y pondera los términos repetidos de la consulta
(`frequency`), y el modo híbrido usa por defecto la fusión convexa con alpha = 0.65
(`--fusion rrf` recupera RRF clásico). Las mediciones de este informe nombran siempre la
configuración explícita; donde dice «por defecto» se refiere a los valores anteriores
(`exhaustive`, `unique` y RRF clásico).

## Resumen

- **Calidad léxica igual a Lucene en los 6 datasets.** Con el analizador opt-in
  `english-v1` (99.993 % de tokens idénticos a Lucene) y la ponderación `frequency` (ahora
  por defecto), el nDCG@10 medio de BM25 es 0.5030 frente a 0.5024 de Lucene, sin diferencia
  significativa en 5 datasets y +0.003 en ArguAna. Con la ponderación anterior, `unique`
  (cada término una vez), empatamos en 5 datasets y perdemos 0.079 en ArguAna, cuyas
  consultas son argumentos completos. Ver
  [Ponderación de la consulta](#ponderación-de-términos-repetidos-en-la-consulta).
- **HNSW más rápido que hnswlib y FAISS con la misma calidad, sin `-ffast-math`.** En
  SIFT1M, a Recall@10 ≥ 0.95, AetherIndex sirve 159.1 K QPS frente a 80.1 K de hnswlib
  compilado con `-ffast-math` (su mejor caso), 57.3 K de hnswlib estándar y 47.5 K de FAISS
  HNSWFlat. En GloVe-100 a 0.95 sirve 12.6 K frente a 9.0 K de hnswlib con `-ffast-math`,
  que antes nos superaba. También construimos con un hilo más rápido que ambos (SIFT1M:
  258 s frente a 352 s de FAISS y 366 s de hnswlib). La mejora viene de un perfilado: el
  coste estaba en los fallos de caché, no en el kernel de distancia. Ver
  [Perfilado de HNSW](#perfilado-de-hnsw-y-prefetch-por-lotes).
- **Búsqueda densa sin pérdida.** Sobre los embeddings bge-small de BEIR, HNSW de AetherIndex
  iguala la búsqueda exacta en nDCG@10 (media 0.6174 frente a 0.6178, sin diferencia
  significativa) y la cuantización SQ8 no cambia la calidad (0.6173).
- **Híbrido mejor que cada recuperador por separado.** La fusión reproduce exactamente una
  RRF de referencia sobre los mismos runs (6 de 6 datasets). Con RRF clásico iguala a la
  referencia Lucene + exacto + RRF (media 0.6086 frente a 0.6088) y a OpenSearch, pero queda
  por debajo de la búsqueda densa sola en FiQA, ArguAna y Quora. La fusión convexa, ahora
  la fusión por defecto, con un valor elegido solo en las particiones dev, sube la media a
  0.6313 y supera a la búsqueda densa exacta en 5 de 6 datasets (empata en ArguAna). Ver
  [Fusión ponderada](#fusión-ponderada-ajustada-en-dev).
- **Búsqueda densa en proceso de 4 a 11 veces más rápida que los servidores.** HNSW de
  AetherIndex responde en 0.08–0.38 ms frente a 0.58–2.38 ms de Qdrant y OpenSearch medidos
  desde el cliente. Esa diferencia incluye la red local y la serialización, así que no es
  una comparación de motores.
- **BM25 más rápido que Lucene, con los mismos resultados bit a bit.** Con la estrategia
  `maxscore`, los postings empaquetados por bits y dos cambios guiados por un perfilado,
  AetherIndex responde antes que Lucene con un hilo en los 6 datasets (FiQA 0.119 frente a
  0.165 ms, TREC-COVID 0.292 frente a 0.384 ms, Quora 0.174 frente a 0.214 ms) y da de 1.2 a
  1.7 veces su throughput con 10 hilos en FiQA, ArguAna, TREC-COVID y Quora. Con BMW la
  diferencia era de 1.7 a 14 veces a favor de Lucene. El índice ocupa entre 0.94 y 1.5 veces
  el de Lucene. Ver [Estrategia MaxScore](#estrategia-maxscore),
  [Postings empaquetados por bits](#postings-empaquetados-por-bits) y
  [Ventanas sin ordenación](#ventanas-sin-ordenación-y-desempaquetado-por-grupos).

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
| `unique` | +0.0006 (p = 0.85) | +0.0002 (p = 0.86) | +0.0018 (p = 0.46) | **−0.0792 (p < 0.001)** | +0.0029 (p = 0.44) | +0.0009 (p = 0.09) |
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
| SciFact | 0.055 ms · 106 K | **0.022 ms · 291 K** | 0.091 ms · 32 K | 0.125 ms · 10 K |
| NFCorpus | 0.004 ms · 739 K | **0.003 ms · 753 K** | 0.029 ms · 17 K | 0.086 ms · 21 K |
| FiQA | 0.446 ms · 14.5 K | **0.119 ms · 65.2 K** | 0.165 ms · 45.6 K | 0.458 ms · 6.9 K |
| ArguAna | 7.853 ms · 0.9 K | **0.315 ms · 26.5 K** | 0.541 ms · 15.4 K | 0.347 ms · 1.3 K |
| TREC-COVID | 1.210 ms · 3.2 K | **0.292 ms · 21.0 K** | 0.384 ms · 15.7 K | 1.399 ms · 3.4 K |
| Quora | 0.320 ms · 19.0 K | **0.174 ms · 44.1 K** | 0.214 ms · 37.8 K | 2.550 ms · 3.1 K |

Las columnas de AetherIndex usan la ponderación `unique` y los postings empaquetados por bits;
con `frequency` las cifras son prácticamente iguales (`summary.md`). AetherIndex y Lucene se
midieron en la misma sesión (octava pasada, ver [Limitaciones](#limitaciones)); bm25s es de la
primera. En SciFact y NFCorpus el throughput de Lucene en esta sesión es menor que en la
primera pasada (45 K y 76 K QPS); con esas cifras AetherIndex también da más.

Construcción con un hilo: AetherIndex indexa de 1.5 a 6.4 veces más rápido que Lucene
(TREC-COVID 3.9 s frente a 6.0 s; el tiempo de Lucene incluye el arranque de la JVM y la
fusión a un segmento) y a la par de bm25s. Los postings comprimidos de AetherIndex ocupan
de 0.94 a 1.5 veces lo que el índice de Lucene (TREC-COVID 28.0 MB frente a 26.5 MB; Quora
14.8 MB frente a 15.8 MB) y mucho menos que bm25s (104.6 MB en TREC-COVID).

## Resultados ANN

Mejor QPS (10 hilos) de cada sistema con Recall@10 ≥ objetivo, sobre toda la rejilla:

| Sistema | SIFT1M @0.90 | @0.95 | @0.99 | GloVe-100 @0.90 | @0.95 |
|---|---:|---:|---:|---:|---:|
| **AetherIndex HNSW** | **215.0 K** | **159.1 K** | **75.7 K** | **32.7 K** | **12.6 K** |
| hnswlib 0.8.0 | 93.2 K | 57.3 K | 29.2 K | 14.3 K | 7.8 K |
| hnswlib 0.8.0 (`-ffast-math`) | 127.1 K | 80.1 K | 41.2 K | 16.8 K | 9.0 K |
| FAISS HNSWFlat | 80.2 K | 47.5 K | 25.5 K | 9.8 K | 5.2 K |
| AetherIndex SQ8 + re-rank | 277.6 K | 163.8 K | 80.0 K | 19.3 K | 10.3 K |
| AetherIndex SQ8 sin FP32 | 238.5 K | 180.8 K | — | 24.3 K | — |
| FAISS HNSWSQ8 | 117.4 K | 67.3 K | — | 5.0 K | — |
| AetherIndex PQ + re-rank | 181.2 K | 114.9 K | 70.6 K | 20.4 K | — |
| FAISS IVFPQ + refine | 58.1 K | 33.2 K | — | — | — |

Todos los sistemas se midieron en la misma sesión (pasadas 6 y 7, ver
[Limitaciones](#limitaciones)). Frente a la primera pasada, hnswlib y FAISS quedan hasta un
10 % más lentos por la carga de fondo de la máquina; AetherIndex HNSW es de 1.4 a 1.7
veces más rápido por los cambios de la sección de perfilado.

Curvas completas: [SIFT1M](../../evaluation/reports/market/plots/sift1m_qps.svg),
[GloVe-100](../../evaluation/reports/market/plots/glove-100_qps.svg) (10 hilos) y sus
versiones con un hilo (`*_qps1.svg`). Ningún sistema alcanza 0.99 en GloVe-100 con esta
rejilla; el máximo de AetherIndex es 0.9859 y el de hnswlib 0.9799.

Construcción (M = 16, efConstruction = 200) y memoria:

| Sistema | SIFT1M 10 hilos | 1 hilo | GloVe 10 hilos | 1 hilo | Estructura SIFT1M |
|---|---:|---:|---:|---:|---:|
| AetherIndex HNSW | 34.5 s | **258 s** | 45.7 s | **355 s** | 624 MiB |
| hnswlib 0.8.0 | 43.9 s | 366 s | 56.1 s | 459 s | 626 MiB |
| hnswlib (`-ffast-math`) | **30.5 s** | — | **42.6 s** | — | 626 MiB |
| FAISS HNSWFlat | 56.3 s | 352 s | 82.2 s | 485 s | 626 MiB |
| AetherIndex SQ8 sin FP32 | 34.9 s | — | 47.8 s | — | 262 MiB |
| FAISS HNSWSQ8 | 46.8 s | — | 113.6 s | — | 260 MiB |

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

- Con la ponderación `unique`, el híbrido de AetherIndex no difiere significativamente de
  la referencia salvo en ArguAna (−0.052), donde hereda la pérdida léxica; Qdrant, que también
  cuenta cada término una vez, muestra lo mismo (0.4800). Con `frequency` la diferencia con la
  referencia es como mucho de 0.002 en cada dataset (ArguAna 0.5372 frente a 0.5354) y la
  fusión sigue reproduciendo exactamente la RRF de referencia (6 de 6 datasets).
- Con RRF clásico, el híbrido supera al denso exacto en NFCorpus y TREC-COVID (significativo;
  en SciFact la ventaja no lo es) y queda por debajo en FiQA, ArguAna y Quora: con un modelo
  denso fuerte, RRF con pesos iguales puede diluir la rama mejor. Es una propiedad de RRF,
  no de nuestra implementación, y la referencia y los servidores se comportan igual. La
  fusión ponderada de la sección siguiente lo corrige.
- En SciFact, NFCorpus y ArguAna, Qdrant no construye HNSW para los vectores densos porque la
  colección queda por debajo de su `indexing_threshold` por defecto, y busca de forma exacta.
  Se dejó así porque es su comportamiento estándar.

Sobre los mismos vectores bge-small, a igual ef, HNSW de AetherIndex obtiene recall igual o
mayor que hnswlib y FAISS en los 18 puntos medidos y de 1.3 a 2.4 veces el QPS del más rápido
de los dos. Por ejemplo, FiQA con ef = 64: 0.978 y 97 K QPS frente a 0.962 y 30 K de hnswlib
y 0.963 y 48 K de FAISS.

## Ponderación de términos repetidos en la consulta

La primera pasada de la campaña mostró la única pérdida de calidad léxica: −0.079 de
nDCG@10 frente a Lucene en ArguAna y −0.052 en el híbrido. La causa no era el analizador ni la
fórmula: AetherIndex contaba una vez cada término distinto de la consulta, mientras que Lucene
(vía Anserini) pondera cada término por sus repeticiones y bm25s suma los duplicados. Las
consultas de ArguAna son argumentos completos (mediana de 174 palabras), así que la
repetición pesa; en los demás datasets las consultas son cortas y no había diferencia.

Se añadió `Bm25Config::query_weighting` (`QueryTermWeighting::Unique`, entonces por
defecto, o `Frequency`) y la opción `--bm25-query-weighting unique|frequency` de la CLI. Con
`frequency`, la contribución de cada término se multiplica por sus apariciones en la consulta
analizada; las cuatro estrategias aplican el peso también a sus cotas superiores y siguen
devolviendo scores idénticos bit a bit (pruebas unitarias con consultas de hasta 25 términos
repetidos y k ∈ {1, 10, 100}). Al añadirla, el valor por defecto no cambiaba la salida: los
runs BM25 con `unique` de SciFact, ArguAna y Quora, en BMW y exhaustivo, son idénticos byte a
byte a los de la primera pasada. La ponderación no cambia la latencia (ArguAna BMW 7.47 ms
frente a 7.90 ms). Desde el 30 de octubre de 2026 `Frequency` es el valor por defecto; las
consultas sin términos repetidos dan exactamente los mismos resultados con ambos valores.

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

## Fusión ponderada ajustada en dev

RRF da el mismo peso a las dos listas. Con bge-small, la lista densa es mucho mejor que BM25
en FiQA, ArguAna y Quora, y la fusión con pesos iguales la diluye. Se añadieron dos fusiones.
Desde el 30 de octubre de 2026 la convexa con A = 0.65 es la fusión por defecto del modo
híbrido; `--fusion rrf` recupera RRF clásico:

- **RRF ponderado** (`--rrf-weight W`): `W/(60 + rango denso) + (1 - W)/(60 + rango léxico)`.
- **Fusión convexa** (`--alpha A`): `A · denso + (1 - A) · BM25`, con cada lista normalizada
  min-max sobre sus candidatos (similitud densa `1 - distancia`; un documento ausente de una
  lista aporta 0; una lista de scores iguales se normaliza a 1).

**Protocolo sin sobreajuste.** Los parámetros se eligieron solo con las particiones dev de
BEIR (SciFact no tiene dev y se usó su partición train; ArguAna y TREC-COVID no tienen
ninguna). `tune_fusion.py` fusiona fuera de línea los runs densos y BM25 de las consultas dev
con la misma aritmética que el motor, recorre W y A de 0 a 1 en pasos de 0.05 y elige el
valor que maximiza el nDCG@10 medio de dev en los cuatro datasets. Las consultas dev son
disjuntas de las de prueba (se comprueba al prepararlas) y los juicios de prueba no se leen
en el ajuste. Ese valor global se aplica a los 6 datasets de prueba; los valores por dataset
se aplican solo a los 4 con dev.

| Dataset (dev) | Consultas | RRF | Denso | BM25 | W (nDCG@10) | A (nDCG@10) |
|---|---:|---:|---:|---:|---:|---:|
| SciFact (train) | 809 | 0.7492 | 0.7469 | 0.6971 | 0.75 (0.7598) | 0.65 (0.7706) |
| NFCorpus | 324 | 0.3402 | 0.3276 | 0.2876 | 0.75 (0.3426) | 0.50 (0.3405) |
| FiQA | 500 | 0.3638 | 0.4079 | 0.2576 | 0.95 (0.4120) | 0.75 (0.4174) |
| Quora | 5,000 | 0.8641 | 0.8834 | 0.7888 | 0.90 (0.8864) | 0.75 (0.8951) |
| **Global** | | 0.5793 | | | **0.85** (0.5991) | **0.65** (0.6052) |

Resultados en prueba (nDCG@10; BM25 con ponderación por frecuencia; valores globales):

| Sistema | SciFact | NFCorpus | FiQA | ArguAna | TREC-COVID | Quora | Media |
|---|---:|---:|---:|---:|---:|---:|---:|
| Denso exacto | 0.7127 | 0.3430 | 0.4035 | 0.6035 | 0.7578 | 0.8864 | 0.6178 |
| AetherIndex RRF | 0.7292 | 0.3612 | 0.3586 | 0.5372 | 0.7994 | 0.8662 | 0.6086 |
| AetherIndex RRF ponderado (W = 0.85) | 0.7239 | 0.3531 | 0.4080 | 0.6025 | 0.8007 | 0.8886 | 0.6295 |
| **AetherIndex convexo (A = 0.65)** | **0.7318** | 0.3603 | **0.4137** | 0.6005 | 0.7861 | **0.8955** | **0.6313** |
| OpenSearch RRF | 0.7286 | 0.3607 | 0.3588 | 0.5349 | 0.7986 | 0.8652 | 0.6078 |
| Qdrant RRF | 0.7302 | 0.3626 | 0.3545 | 0.4800 | 0.8007 | 0.8669 | 0.5991 |

- La fusión convexa mejora a RRF de forma significativa en FiQA (+0.055), ArguAna (+0.063)
  y Quora (+0.029), sin diferencia significativa en los otros tres.
- Frente a la búsqueda densa exacta gana de forma significativa en SciFact, NFCorpus, FiQA,
  TREC-COVID y Quora, y empata en ArguAna (−0.003, p = 0.41). Con RRF clásico perdía en
  FiQA, ArguAna y Quora.
- Los valores por dataset apenas mejoran a los globales (convexo: NFCorpus 0.3641, FiQA
  0.4153, Quora 0.8976), así que un único valor global es suficiente.
- El motor reproduce exactamente la fusión de referencia de Python en los 6 datasets con
  ambas fusiones (runs deterministas con HNSW de un hilo).
- Recall@100 no cambia de forma apreciable (convexo 0.6812 frente a 0.6809 de RRF): la
  ganancia está en el orden de los primeros resultados.

## Postings empaquetados por bits

Con MaxScore, la diferencia que quedaba con Lucene estaba en el formato de los postings. Cada
posting se guardaba como varint (lectura byte a byte con un salto por byte) y cada término
tenía su propia estructura de 64 bytes con dos vectores, es decir, dos reservas de memoria.
Medido sobre los índices, la estructura por término pesaba más que los propios postings en
las colecciones pequeñas (SciFact: 1.85 MB frente a 0.79 MB de postings) y la mitad del
vocabulario aparece en un solo documento.

El nuevo formato:

- empaqueta cada bloque de 128 postings con un ancho fijo de bits para las diferencias de
  ordinal y otro para `tf - 1` (0 bits si todos los `tf` valen 1);
- decodifica con funciones especializadas por ancho, con una lectura de 8 bytes por valor y
  sin saltos;
- guarda todos los bloques y bytes en dos arreglos compartidos; cada término ocupa 12 bytes
  (frecuencia documental, primer bloque y cota) y su idf se recalcula con la misma función,
  así que da el mismo valor.

Los resultados no cambian: los runs BM25 de `bmw` y `maxscore` son idénticos byte a byte a los
anteriores en los 6 datasets y `quality.csv` (todas las configuraciones y métricas) no cambia.

| Dataset | Índice antes | Índice ahora | Lucene | `maxscore` p50 antes | ahora |
|---|---:|---:|---:|---:|---:|
| SciFact | 3.2 MB | 1.6 MB | 1.1 MB | 0.068 ms | 0.060 ms |
| NFCorpus | 2.4 MB | 1.2 MB | 0.8 MB | 0.006 ms | 0.006 ms |
| FiQA | 11.9 MB | 7.9 MB | 6.9 MB | 0.278 ms | 0.219 ms |
| ArguAna | 3.1 MB | 1.8 MB | 1.7 MB | 0.544 ms | 0.475 ms |
| TREC-COVID | 40.6 MB | 28.0 MB | 26.5 MB | 0.667 ms | 0.519 ms |
| Quora | 18.9 MB | 14.8 MB | 15.8 MB | 0.248 ms | 0.254 ms |

El throughput con 10 hilos de `maxscore` sube un 33 % en FiQA, 32 % en ArguAna y 29 % en
TREC-COVID. En Quora la latencia con un hilo queda igual dentro del ruido y el throughput sube
un 2 %.

## Ventanas sin ordenación y desempaquetado por grupos

Con `maxscore` y postings empaquetados, Lucene seguía respondiendo de 1.2 a 1.4 veces antes
en FiQA, TREC-COVID y Quora. Un perfilado de `maxscore` (`sample` de macOS, un hilo, consultas
de cada dataset en bucle) mostró dos costes evitables:

- **Ordenar los documentos de cada ventana (24–34 % del tiempo).** Los documentos tocados por
  los términos esenciales se guardaban en una lista y se ordenaban antes de consultar los no
  esenciales, cuyos cursores solo avanzan. Ahora se marcan en un mapa de 4,096 bits por
  ventana y se recorren en orden con `countr_zero`, sin ordenar.
- **Desempaquetar los bloques (13–18 %).** Con anchos de hasta 8 bits, ocho valores ocupan
  exactamente ese número de bytes, así que cada grupo de ocho sale de una sola lectura de 8
  bytes con desplazamientos constantes, en lugar de una lectura por valor.

Se probaron y descartaron, por no mejorar más allá del ruido, la decodificación perezosa de
las frecuencias (solo cuando el documento coincide) y comprobar la cota del bloque antes de
consultar un término no esencial. En el arnés de perfilado (p50 con un hilo, ponderación
`frequency`) los dos cambios llevan FiQA de 0.206 a 0.109 ms, TREC-COVID de 0.472 a 0.263 ms y
Quora de 0.210 a 0.154 ms.

Los resultados no cambian: en la octava pasada, los 72 runs BM25 de AetherIndex y Lucene son
idénticos byte a byte a los anteriores, y la prueba que compara todas las estrategias exige
scores idénticos bit a bit. Resultado en la campaña (`maxscore`, `unique`):

| Dataset | p50 antes | ahora | QPS 10 hilos antes | ahora | Lucene p50 · QPS |
|---|---:|---:|---:|---:|---:|
| SciFact | 0.060 ms | 0.022 ms | 127 K | 291 K | 0.091 ms · 32 K |
| NFCorpus | 0.006 ms | 0.003 ms | 520 K | 753 K | 0.029 ms · 17 K |
| FiQA | 0.219 ms | 0.119 ms | 35.6 K | 65.2 K | 0.165 ms · 45.6 K |
| ArguAna | 0.475 ms | 0.315 ms | 19.3 K | 26.5 K | 0.541 ms · 15.4 K |
| TREC-COVID | 0.519 ms | 0.292 ms | 12.2 K | 21.0 K | 0.384 ms · 15.7 K |
| Quora | 0.254 ms | 0.174 ms | 29.1 K | 44.1 K | 0.214 ms · 37.8 K |

Los dos cambios también aceleran BMW, que usa el mismo desempaquetado (TREC-COVID de 1.260 a
1.210 ms). Desde el 30 de octubre de 2026 `maxscore` es la estrategia por defecto.

Al hacerla la ruta por defecto, las pruebas de fallos de asignación (que ahora recorren las
cinco estrategias) encontraron dos defectos previos de las estrategias comprimidas, ya
corregidos: `build()` publicaba el índice antes de construir los bloques, así que un fallo
en ese paso dejaba un índice marcado como construido sin bloques, y un fallo a mitad de una
búsqueda `maxscore` o `taat` dejaba sucios los acumuladores por hilo para la siguiente. Ahora
los bloques se construyen antes de publicar nada y los acumuladores se limpian ante cualquier
excepción. Además, un corpus formado solo por documentos vacíos (longitud media 0) ya no
falla al construir los bloques.

## Perfilado de HNSW y prefetch por lotes

La primera pasada dejó dos huecos en ANN: hnswlib con `-ffast-math` era más rápido en
GloVe-100 a Recall@10 ≥ 0.95, y hnswlib y FAISS construían algo más rápido con un hilo. Antes
de cambiar nada se perfiló la búsqueda (`sample` de macOS, GloVe-100, ef = 256, 10 hilos):

- **El kernel de distancia no era la causa.** Los kernels de L2 y producto interno ya son
  explícitos (`src/distance/scalar_distance.cpp`: NEON con cuatro acumuladores de 4 floats y
  AVX2 + FMA con cuatro de 8) y suponían solo un ~20 % del tiempo. Un kernel más rápido no
  podía cerrar la diferencia.
- **El coste estaba en los fallos de caché.** El bucle de búsqueda ocupaba el 62 % del
  tiempo, casi todo esperando las filas de los vecinos. Solo se precargaba la primera línea
  de 64 bytes del vecino siguiente, y una fila de GloVe ocupa 400 bytes (7 líneas). Los
  accesores de `VectorStore` (no inline, en otra unidad de compilación) sumaban otro 10 % y
  el heap de candidatos un 8 %.

Cambios, sin `-ffast-math` y sin tocar los kernels:

1. Los accesores `ids()`, `padded_dimension()` y `data_by_position()` pasan a ser inline.
2. Cada lista de adyacencia se recorre en dos pasadas. La primera descarta los visitados y
   precarga todas las líneas de la fila y el ID de cada vecino nuevo, de modo que sus fallos
   de caché se solapan. La segunda calcula las distancias e inserta en el orden de
   adyacencia, así que las decisiones y los resultados son los mismos que con el bucle
   anterior. La construcción usa la misma búsqueda y también se beneficia.
3. La precarga sigue a lo que lee la distancia. Con SQ8 y PQ se precargan los códigos (128 y
   32 bytes por fila en SIFT1M) mediante `QuantizedQuery::prefetch`, no las filas FP32. La
   primera versión precargaba las filas FP32 también en ese camino y SQ8 y PQ con re-rank
   salían un ~25 % más lentos. Se detectó en la sexta pasada, se corrigió y esos seis pasos
   se volvieron a medir (séptima pasada).

Comparación intercalada con `HEAD` en la misma sesión (M = 16, efConstruction = 200, 10
hilos; QPS por lotes, p50 con un hilo; el recall coincide salvo variaciones de ±0.0002
debidas a la construcción paralela):

| Caso | `HEAD` | Con los cambios |
|---|---:|---:|
| SIFT1M, ef = 64 | 67 K QPS · p50 0.140 ms | 106 K QPS · p50 0.079 ms |
| SIFT1M, ef = 256 | 21.5 K QPS | 34.8 K QPS |
| GloVe-100, ef = 256 | 13 K QPS · p50 0.528 ms | 28 K QPS · p50 0.281 ms |
| GloVe-100, ef = 512 | 6.3 K QPS | 15.0 K QPS |
| SIFT1M SQ8 + re-rank, ef = 128 | 53.1 K QPS · p50 0.175 ms | 81.3 K QPS · p50 0.101 ms |
| SIFT1M PQ + re-rank, ef = 128 | 45.3 K QPS | 71.6 K QPS |
| GloVe-100 SQ8 + re-rank, ef = 128 | 43.9 K QPS | 55.5 K QPS |
| Construcción SIFT1M, 10 hilos | 44 s | 33 s |

En la campaña completa, AetherIndex HNSW pasa a superar a hnswlib con `-ffast-math` en todos
los objetivos de recall de los dos datasets y a construir con un hilo más rápido que hnswlib
y FAISS (ver [Resultados ANN](#resultados-ann)).

## Dónde perdemos y por qué

1. **Índice léxico algo más grande en colecciones pequeñas.** Ocupamos 1.5 veces el índice de
   Lucene en SciFact y NFCorpus y 1.06–1.14 veces en FiQA, ArguAna y TREC-COVID; en Quora
   ocupamos menos (0.94). En colecciones pequeñas pesan las cabeceras de bloque (16 bytes) y
   los arreglos por documento (12 bytes).
2. **Construcción paralela frente a hnswlib con `-ffast-math`.** Con 10 hilos, hnswlib
   compilado con `-ffast-math` construye SIFT1M en 30.5 s y GloVe-100 en 42.6 s, frente a
   34.5 s y 45.7 s de AetherIndex. Con las opciones estándar de hnswlib, y con un hilo,
   construimos más rápido. Los antiguos puntos de GloVe-100 a alto recall y de construcción
   con un hilo quedaron resueltos (ver
   [Perfilado de HNSW](#perfilado-de-hnsw-y-prefetch-por-lotes)).
3. **`ascii-tech-v1` en texto natural.** Sin stopwords ni stemming pierde frente a Lucene sin
   stemming en los 6 datasets; para texto en inglés debe usarse `english-v1`.

## Limitaciones

- Una sola máquina (Apple Silicon); los resultados x86 con AVX pueden diferir, sobre todo
  para hnswlib y FAISS, que tienen kernels x86 más maduros. El kernel AVX2 de AetherIndex se
  elige al compilar (`AETHER_NATIVE_SIMD=ON` o `-mavx2 -mfma`); no hay selección en tiempo de
  ejecución, así que un binario x86 genérico usa el camino escalar. No se pudo probar en esta
  máquina.
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
- La campaña tiene ocho pasadas:
  - **Primera** (`manifest-pass1.json`): todos los resultados salvo los de ponderación por
    frecuencia, MaxScore y postings empaquetados.
  - **Segunda** (`manifest-pass2.json`, binario con `QueryTermWeighting`): los runs `*-freq`
    y sus controles.
  - **Tercera** (`manifest-pass3.json`, binario con `MaxScore`): los runs `*-maxscore`.
  - **Cuarta** (`manifest-pass4.json`, postings empaquetados): todos los runs BM25 de
    AetherIndex.
  - **Quinta** (`manifest-pass5.json`): los runs dev y de prueba de la fusión ponderada.
  - **Sexta** (`manifest-pass6.json`, prefetch por lotes): todos los pasos ANN, de
    construcción con un hilo y HNSW sobre BEIR, para todos los sistemas.
  - **Séptima** (`manifest-pass7.json`, precarga de códigos): los seis pasos de SQ8 y PQ de
    AetherIndex.
  - **Octava** (`manifest.json`, ventanas sin ordenación y desempaquetado por grupos): todos
    los runs BM25 de AetherIndex y las tres configuraciones de búsqueda de Lucene, en la
    misma sesión. Los 72 runs son idénticos byte a byte a los anteriores.

  Antes de las pasadas segunda a cuarta se comprobó que el nuevo binario reproduce byte a
  byte los runs BM25 por defecto de la primera. Los runs híbridos de AetherIndex con RRF
  clásico no se repitieron en pasadas posteriores (su HNSW paralelo no es determinista y
  cambiaría las tablas de calidad), así que su latencia corresponde al formato varint y al
  HNSW anterior.
- Durante las pasadas sexta a octava la máquina tenía carga de fondo de macOS (indexado de
  almacenamiento, WebKit y otra aplicación; unos 3.4 de 14 núcleos ocupados). Afecta a todos
  los sistemas por igual porque se midieron en la misma sesión, pero los QPS absolutos son
  algo menores que en la primera pasada (hnswlib y FAISS, hasta un 10 %). Las comparaciones
  intercaladas con `HEAD` se hicieron en las mismas condiciones.
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
tarda unas 3 h 10 min (más unos 20 min de la segunda pasada, 3 de la tercera, 22 de la
cuarta, 20 de la quinta, 2 h 20 min de la sexta, 7 de la séptima y 22 de la octava) y los servidores unos
55 min.
