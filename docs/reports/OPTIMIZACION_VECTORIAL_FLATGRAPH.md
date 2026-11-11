# AetherIndex: optimización vectorial, Top-K y FlatGraph

**Fecha:** 19 de octubre de 2026. **Alcance:** cambios implementados después del
baseline del Sprint 1. **Plataforma observada:** macOS 26.5.2, arm64, AppleClang 17
para Release/Debug y Clang 23.1.3 para ASan/UBSan. No se verificaron modelo exacto
de CPU ni RAM; no se presentan como datos medidos. No hubo compilación ni
runtime reales de Windows/MSVC o Linux en esta sesión.

Este informe distingue implementación, corrección y rendimiento. Las cifras
históricas del [informe de Sprint 1](INFORME_TECNICO_AETHERINDEX_SPRINT_1.md)
pertenecen al diseño anterior. El [protocolo](../../benchmarks/optimization_protocol.json)
fue fijado antes de medir y conserva explícitamente las dos aclaraciones del
usuario: presupuesto de **14.50 MB decimales para el grafo completo** y ganancia
de Recall **absoluta ≥0.05**, no relativa.

## 1. Cambios implementados

| Componente | Baseline | Implementación actual |
|---|---|---|
| VectorStore Cosine | Componentes originales y norma calculada por comparación | Normalización L2 al insertar; rechazo de norma <1e-7 |
| VectorStore SquaredEuclidean | FP32 original | FP32 original; conserva magnitudes y permite ceros |
| Layout de embeddings | Vector contiguo sin garantía de alineación por fila | Asignación alineada a 64 bytes; stride múltiplo de 16 floats y padding cero |
| Consulta | Validación/cálculo normativo repetido en distancias | PreparedQuery propietario, normalizado una vez al entrar a QueryEngine |
| Distancia de búsqueda | Sumas double, normas y raíces Cosine por candidato | FP32/FMA: producto punto unitario o diferencias al cuadrado; NEON, AVX2/FMA o fallback escalar |
| Top-K exhaustivo | Ordenar N resultados y truncar | Max-heap de capacidad K; desempate por ID |
| Topología HNSW | Mapa de nodos y vectores de vecinos por nodo/capa | Dos arenas globales, niveles uint8 y offsets uint32; cuatro buffers propietarios |
| Selección HNSW | Vecinos más cercanos (Algoritmo 3) | Diversificación (Algoritmo 4) predeterminada; Simple disponible para ablación |
| Navegación HNSW | Hash de visitados y lookup de embedding por ID externo | Bitmap temporal de un bit por posición, filas densas y prefetch del siguiente vecino |
| CLI | HNSW y BruteForce | Conserva integración de aplicación, tabla, IDs enteros, nueve decimales y tiempos; añade selección de política |

Se conservaron los módulos existentes: storage y distance dependen de core;
HNSW y BruteForce comparten distance y storage; aplicación coordina y la CLI
presenta. No se agregaron dependencias de Python/NumPy al ejecutable C++.

### Normalización y precisión

La preparación escala por la mayor componente absoluta antes de acumular
cuadrados en ocho sumas FP32/FMA. Esto maneja componentes finitas muy grandes
y consultas subnormales sin formar una norma que desborde o se anule. La raíz
y divisiones ocurren durante inserción/preparación, fuera de los bucles de
comparación. El API general `compute_distance` valida/prepara entradas crudas;
los algoritmos llaman directamente a los kernels de entradas preparadas.

Se limita la similitud a [-1,1] antes de calcular 1−dot. Un resultado squared
que desborda FP32 lanza overflow_error. El span recibido nunca se modifica.
Las vistas de almacenamiento son de lectura; hay que descartarlas antes de
insertar o reservar, aunque la operación falle. Se conserva rollback lógico de
VectorStore con fallos de asignación, incluidas asignaciones alineadas y alias.

**Cambio observable:** Cosine devuelve la representación normalizada en find().
La distancia matemática 0.2 se imprime ahora `0.199999988`, frente al antiguo
`0.200000003`. Ambos son redondeos FP32; nueve decimales son formato de salida,
no una garantía de nueve decimales correctos. Los casos T2.5 conservan su límite
estricto de error <1e-5 y se añade el criterio más exigente de coseno <1e-6.

### Selección diversificada

Algoritmo 4 ordena candidatos por distancia al nodo base. Rechaza/pospone e si
algún vecino elegido r satisface d(e,r) < d(e,base). Con keep_pruned_connections
rellena cupos con los descartados más próximos; con extend_candidates añade
los vecinos de un salto del conjunto original, con deduplicación. Ambas opciones
están implementadas y probadas; defaults false/true, respectivamente.

La misma política se aplica al podar listas llenas. Se puntúan M_max+1 candidatos
en un espacio temporal antes de escribir; el arena nunca recibe un grado que
sobrepase el slot. Se conserva la variante del proyecto de hasta M0 conexiones
iniciales en Layer 0, igual para ambas políticas comparadas. Las podas pueden
producir aristas unidireccionales; el grafo sigue siendo aproximado.

## 2. Presupuesto de memoria: bytes propios y RSS

Para N=100,000, M=16, M0=32 y semilla 42:

| Componente | Capacidad reservada (bytes) |
|---|---:|
| Layer 0: 100,000 × (32+1) × 4 | 13,200,000 |
| Arena superior: 8,032 slots × 17 × 4 | 546,176 |
| Niveles uint8 + offsets uint32: N × 5 | 500,000 |
| Objeto global, incluido generador aleatorio | 2,736 |
| **Total de capacidad propia + objeto** | **14,248,912** |

**14.248912 MB <14.50 MB**, con cuatro bloques propietarios. Se ocuparon 6,615
slots superiores, frente a la esperanza N/(M−1)≈6,666.67. La capacidad incluye
headroom estocástico no ocupado; no se presenta solo el tamaño lógico como si
fuera toda la memoria reservada. Excluye bookkeeping del asignador, embeddings
y espacios temporales de operaciones. Los buffers son alineados, pero un slot
Layer 0 de 132 bytes no empieza necesariamente en una línea de caché.

La última ejecución con bitmap compacto registró un delta RSS de
**11,304,960 bytes** (11.304960 MB). Una ejecución
anterior de la misma versión compacta dio **15,204,352 bytes**; la versión inicial
con un byte por visitado dio **15,876,096 bytes**. Se conservan las tres
observaciones. El último valor está por debajo de 14.50 MB; el anterior compacto
lo excede. Esta variación impide afirmar una cota estable de RSS bruto, y
muestra por qué RSS y capacidad propia del grafo son medidas distintas.
El delta incluye efectos de residencia de páginas del proceso, asignador,
código y retención de espacios temporales, además de la topología.
**La cota estructural se cumple; no se acredita una cota consistente de delta
RSS del proceso ≤14.50 MB seleccionando solo el último resultado favorable.**

La auditoría dinámica a N=1,000 intercepta new/delete ordinarios y alineados:
solo quedan cuatro asignaciones al terminar la construcción, y la suma de sus
bytes coincide con memory_usage(). Al destruir el índice se liberan las cuatro.
Los HnswNode diagnósticos son trivially_copyable y no poseen contenedores.
Los temporales de búsqueda/selección sí usan vectores por operación, no por nodo
persistente; el bitmap usa ceil(N/64) uint64, unos 12,504 bytes para N=100,000.

El presupuesto se valida para el protocolo y reserva declarados. No se promete
la misma capacidad para cualquier semilla extrema, reservas sobredimensionadas
o crecimiento incremental sin reserve. Los niveles máximos y offsets se validan.

## 3. Estado de los criterios

Los valores definitivos, muestras y logs están en
[evidence/2026-10-19-optimization](evidence/2026-10-19-optimization/).
El cuadro siguiente se calculó desde esos archivos, con el alcance indicado.

| Criterio | Resultado y alcance | Estado |
|---|---|---|
| CA-1.1: cero raíz/división en comparación | Inspección de los kernels -O3: ninguna; preparación separada | Cumple |
| CA-1.2: error coseno <1e-6 | 2,016 comparaciones, dimensiones 1–4,097, máximo observado ≈3.57628e-7 | Cumple en el conjunto probado |
| CA-1.3: reducción Top-K >60% | Media 6.386865 → 0.872127 ms; reducción 86.345%; 50 pares | Cumple localmente |
| CA-1.4: FMA empaquetada sin float→double | 8 instrucciones NEON fmla.4s; adicionalmente 16 FMA empaquetadas en ensamblador AVX2 compilado para x86, sin ejecución x86 | Cumple en ARM; compilación x86 verificada |
| CA-2.1: sin contenedores propietarios por nodo | Prueba estática y auditoría dinámica: cuatro buffers globales | Cumple |
| CA-2.2: grafo completo ≤14.50 MB | 14.248912 MB de capacidad propia + objeto; RSS final 11.304960 MB y otro run compacto 15.204352 MB | Presupuesto estructural cumplido; cota RSS no establecida |
| CA-2.3: ≥50% menos fallos LLC | Driver y control perf preparados; sin contadores Linux medidos | Pendiente |
| CA-2.4: ganancia absoluta ≥0.05 | Recall Alg.3=0.3909; Alg.4=0.9191; Δ=0.5282 (**+52.82 puntos porcentuales**) | Cumple en fixture congelado |
| CA-2.5: p99 <1.5 ms | p50=0.222291; p95=0.298333; p99=0.340083 ms; Recall=0.985; build=7404.482 ms aparte | Cumple localmente |
| CA-2.6: suite completa sin diagnósticos | Corrida final completa 21/21, incluidos los dos casos reales; cero diagnósticos ASan/UBSan | Cumple |


CA-2.3 queda pendiente: esta máquina es macOS y no dispone de la medición PMU
Linux requerida. Se implementó un driver común para el baseline y la versión
actual, más un script perf con control/ack por FIFO. Compila contra ambas
versiones; su ejecución con contadores reales sigue pendiente. No se infiere
una reducción del 50% de LLC a partir de latencia o tiempos de CTest.

## 4. Protocolos y límites de interpretación

**Top-K:** N=100,000, D=128, K=10, diez consultas separadas, cinco ciclos y tres
pares de warmup. Se alterna el orden y se usa el mismo kernel, preparación y
almacenamiento. Solo difiere ordenar todo frente al heap de producción. Los
resultados coinciden exactamente por ID y distancia en cada muestra. La métrica
de aceptación es 1−mean(heap)/mean(fullsort); no se atribuyen a Top-K los ahorros
de normalización/SIMD.

**Clusters:** 100 centros gaussianos de 128 dimensiones y 100 puntos/centro,
ruido stddev 0.015, agrupados en inserción. Mil consultas independientes del
corpus (diez/centro), semillas de datos/consulta/grafo 42/424242/42. M16/M032,
efConstruction100 y efSearch30, idénticos en Algoritmos 3/4. Se congeló el fixture
antes de ver recall; solo se modifica la selección. Los CSV conservan cada
consulta y los IDs de referencia y de ambos índices. El resultado describe este
fixture y una semilla de grafo: no garantiza esa ganancia en todo dataset.
La construcción heurística cuesta más trabajo por comparar candidatos entre sí:
259.260 ms para Simple frente a 646.077 ms para Heuristic
en este run. La ganancia de recall se presenta junto con ese costo, no como una
mejora gratuita en todas las dimensiones.

**DBpedia:** corpus real 10,000×1024; veinte consultas fuera del corpus según el
manifiesto preparado. Tras cien búsquedas de warmup, se repiten cincuenta ciclos
(1,000 búsquedas), en un hilo y un único grafo. Top10, efSearch50, M16/M032,
efConstruction100, semilla42 y selección Heuristic. El timer incluye preparación
y QueryEngine; excluye CSV, construcción, escritura y cálculo de recall. P99 usa
nearest-rank, muestra 990/1000. Son veinte consultas distintas repetidas, no mil
consultas independientes ni una garantía de latencia fría o SLA de producción.

En la corrida final DBpedia, la media fue 0.226143 ms, el máximo
0.369958 ms y el Recall medio 0.985. En el primer run
la p99 fue 0.252292 ms; se conservan ambas mediciones y se usa la última,
0.340083 ms, en el cuadro. No se escoge el mejor tiempo observado.

La CLI sigue reconstruyendo el grafo en cada proceso. El tiempo de construcción
se reporta aparte de Query time; mejorar p99 de un índice reutilizado no elimina
ese costo en ejecuciones individuales de terminal.

**Oráculo y regresión:** cuarenta consultas CLI, dos métricas y Top100 por
consulta verificaron 4,000 vecinos contra NumPy float64 independiente. El máximo
error absoluto observado fue 2.234648108778714e-7, y los rangos Top100 cumplieron
el oráculo sin relajar su comprobación existente. El OKR v2 usa los mismos datos
y consultas congelados del Sprint 1 y verifica 128,000 componentes normalizadas;
obtuvo MeanRecall@10 0.987 en la evaluación Release inicial de esta optimización.
Es otro protocolo que el de clusters, no una contradicción ni una mejora universal.

## 5. Verificación y reproducibilidad

| Configuración | Evidencia | Resultado |
|---|---|---|
| Debug, AppleClang 17 | Suite normal, carpeta creada para esta implementación | **19/19**, 35.15 s |
| Release -O3, SIMD nativo | 21 checks de corrección/datos reales + 5 checks optativos | **26/26**, 128.79 s |
| Debug ASan/UBSan, Clang 23 | Suite final completa con datos reales, CTest parallel=2; cada búsqueda sigue siendo secuencial | **21/21**, 417.81 s, cero diagnósticos |
| BUILD_TESTING=OFF | Build de aplicación, ayuda y consulta CLI HNSW sobre el ejemplo | Éxito, salida 0 |

Los tiempos totales anteriores describen la ejecución de pruebas: no son
latencias del motor. Las métricas de rendimiento del cuadro se obtuvieron en
Release, con sanitizadores desactivados y sin ejecutar otros checks pesados en
paralelo durante los benchmarks.

La suite completa instrumentada también pasó 21/21 antes de compactar visitados;
se conservaron esa corrida y las repeticiones focalizadas. La última corrida
completa se ejecutó después de los cambios finales. El cierre de PreparedQuery
como tipo no copiable/no movible se recompiló en las cuatro configuraciones y
se comprobó adicionalmente mediante static_asserts y dos tests por compilador.
Los hashes prueban que esa restricción de tipos no alteró los bytes de los tres
ejecutables Release ya medidos: CLI y ambos drivers de rendimiento.

El [resumen automático](evidence/2026-10-19-optimization/validation_summary.json)
recalcula Recall y percentiles desde los CSV, verifica huellas de artefactos y
fuentes, y confirma que los arrays del fixture agrupado no cambiaron entre las
revisiones. `artifact_sha256.json` permite verificar el paquete entregado.

Se conservó antes de modificar producción una copia del baseline y su manifiesto
SHA256. El commit base es 031adae19951d95ede349eaeaf295dd8d4a611d2; las pruebas y
documentos previos sin commit también se conservaron. Los nuevos cambios no
hacen commit ni publican un PR. El paquete de evidencia registra hashes de
fuentes/binaries, flags CMake, corpus y artefactos para identificar lo medido.
Los arrays grandes de clusters permanecen en el build local ignorado; se guardan
sus hashes, tamaños, semillas, protocolo y generador. El driver regenera el
fixture: en otra libm/plataforma debe compararse el hash antes de considerar
que se usaron los mismos bytes. No se promete identidad entre plataformas
solo por compartir una semilla.

Los comandos completos y opciones están en
[OPTIMIZATION_VALIDATION.md](../benchmarks/OPTIMIZATION_VALIDATION.md).
Se distinguen ejecución real ARM/NEON, inspección de ensamblador AVX2 compilado
para x86 y plataformas sin ejecución verificada. No se ejecutan benchmarks
instrumentados por sanitizadores para acreditar los umbrales de rendimiento.

## 6. Afirmaciones técnicas que requieren evidencia

Las arenas eliminan contenedores propietarios por nodo y permiten offsets O(1).
Esto no implica que cada acceso anterior fallara LLC, que cada fallo costara un
número fijo de ciclos, ni que el prefetch garantice acierto L1. Esas afirmaciones
requieren contadores específicos y conocimiento de CPU/carga.

Con la distribución HNSW de parámetro M=16, las apariciones esperadas por nodo
son 1+1/(M−1)≈1.0667, no cuatro capas por nodo como supuesto general. Además,
40 bytes de overhead ×106,667 listas serían aproximadamente **4.27 MB**, no
más de 45 MB. El costo real anterior incluye capacidades, hashes, nodos y
asignador: no se valida una cifra total de 60 MB sin medirla. El criterio nuevo
se comprueba sobre la estructura real y no sobre esa estimación del baseline.

## 7. Cierre técnico

La implementación solicitada queda integrada en la arquitectura y la CLI.
La aceptación total del Paso 2 requiere completar el perfilado LLC en Linux y
resolver/evaluar por separado cualquier exigencia de RSS bruto ≤14.50 MB.
El presupuesto estructural, la diversidad, los kernels, el Top-K y las
comprobaciones de corrección tienen evidencia específica. La revisión del equipo
y aprobación del PR siguen siendo requisitos separados; este informe no marca
la tarjeta Done.

Referencias primarias: [HNSW y Algoritmo 4, Malkov/Yashunin](https://arxiv.org/abs/1603.09320),
[intrínsecos NEON oficiales de Arm](https://arm-software.github.io/acle/neon_intrinsics/advsimd.html),
[header NEON ARM64 en MSVC](https://learn.microsoft.com/en-us/cpp/intrinsics/arm64-intrinsics),
[control de contadores en perf stat](https://man7.org/linux/man-pages/man1/perf-stat.1.html).
