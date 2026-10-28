# CP-03 — Implementación y validación de Reciprocal Rank Fusion

**Fecha local:** 26 de octubre de 2026 · **Tarjeta:** Sprint 2 / CP-03 / KR1.4.

El módulo independiente `aether_hybrid` implementa la fusión de dos rankings
documentales mediante RRF. La matriz obligatoria aprueba **22/22 casos** en
Debug, Release y ASan/UBSan. La suite completa del proyecto aprueba **52/52** en
cada perfil. La integración de consultas y CLI híbridas pertenece a CP-04.

Esta evidencia valida el trabajo técnico local. La revisión de otro integrante
y la revisión/aprobación del Pull Request siguen pendientes para el cierre
formal de la tarjeta; no se acredita aquí una aprobación de equipo.

## 1. Implementación y límites arquitectónicos

| Actividad | Entrega y comprobación |
|---|---|
| C1 — Módulo y CMake | Headers en `include/aether/hybrid/`, implementación en `src/hybrid/rrf.cpp`, target estático `aether_hybrid`. Solo depende de `aether_core` y la biblioteca estándar. |
| C2 — Configuración | `RrfConfig::k_rrf` double, predeterminado 60. Se rechazan cero, negativos, NaN e infinitos antes de fusionar. |
| C3 — Entradas | Dos spans de DocumentId ordenados; se permiten vacíos y solapamiento entre listas. Duplicados internos y top_k=0 producen `std::invalid_argument`. |
| C4 — Matemática | Una contribución `1/(k_rrf + posición)` por lista donde aparece el documento; posiciones desde 1 y cálculo double. |
| C5 — Ranking | Unión única, score descendente, igualdad exacta resuelta por DocumentId ascendente y truncado posterior a top_k. |
| C6 — API | `RrfFusion::fuse(...) const` devuelve un vector propietario de los `RankedResult` compartidos de core. |
| C7 — Repetibilidad | Orden de acumulación definido y sort independiente del orden de la tabla hash. 50 ciclos repetidos del ejemplo y de un empate. |
| C8 — Contrato | Adaptación explícita VectorId→DocumentId en el fixture de aplicación; extracción de IDs léxicos sin alterar posiciones y consumo del resultado común. |
| C9 — Pruebas y documentación | 22 casos registrados individualmente, propiedades, contrato, guía de uso y evidencia esperado/obtenido en JSON. |

Se conserva el namespace real del proyecto, **`aetherindex::hybrid`**, en lugar
del namespace ilustrativo `aether::hybrid` del texto de la tarjeta.

RRF recibe IDs y posiciones. No calcula distancias ni BM25, no busca documentos,
no convierte VectorId, no verifica existencia en DocumentStore y no decide el
número de candidatos que solicita cada recuperador. La API distingue por
significado DocumentId de VectorId; ambos siguen siendo aliases uint32, por lo
que el compilador no detecta un cliente que omite la conversión documental.

Cada llamada usa contenedores locales, sin modificar ni conservar las vistas
de entrada. Las excepciones no publican una salida parcial ni alteran el
objeto. Se comprueba la finitud de las contribuciones y scores. La complejidad
esperada es `O(D + L + U log U)` y el espacio auxiliar `O(U)`, con U documentos
únicos. No se añade optimización Top-K, ponderación de ramas ni SIMD.

## 2. Verificación matemática

Entradas del ejemplo obligatorio: denso `[101, 102, 103]`, léxico
`[102, 104, 101]`, `k_rrf=60`, `top_k=10`.

| Posición | DocumentId | Referencia completa | Resultado double obtenido |
|---:|---:|---|---:|
| 1 | 102 | `1/62 + 1/61` | 0.03252247488101534 |
| 2 | 101 | `1/61 + 1/63` | 0.032266458495966696 |
| 3 | 104 | `1/62` | 0.016129032258064516 |
| 4 | 103 | `1/63` | 0.015873015873015872 |

Se exige **`abs(obtenido − esperado) <= 1e-12`** junto con el orden y las
propiedades aplicables. Los valores redondeados a diez decimales del documento
de la tarjeta sirven para presentación; las pruebas usan las fracciones
completas, porque ese redondeo puede superar la tolerancia solicitada.

Hay dos comprobaciones independientes del cálculo: el oráculo C++ enumera la
unión y busca las posiciones en las listas originales; el
[verificador Python](evidence/2026-10-26-cp03/verify_matrix.py) vuelve a calcular
la matriz guardada con `fractions.Fraction`, sin reutilizar helpers ni scores
esperados del algoritmo C++. También valida las referencias guardadas.

En los tres perfiles se verificaron las 30 observaciones de la matriz,
incluyendo variantes de configuración y errores de contrato. El error absoluto
máximo observado frente a las fracciones exactas fue
**4.1988365943945e-18**, inferior a 1e-12. Los resultados están en
[independent-matrix-verification.json](evidence/2026-10-26-cp03/independent-matrix-verification.json).

El comparador de producción **no usa tolerancia**: el desempate por ID se
aplica a scores double exactamente iguales, manteniendo un orden estricto.

## 3. Matriz obligatoria de KR1.4

Cada JSON conserva entradas, configuración, Top-K, salida esperada y obtenida,
error esperado/obtenido y resultado final. La columna de evidencia enlaza el
perfil Debug; las carpetas `release/` y `sanitizers/` conservan la misma matriz
ejecutada con sus respectivos compiladores.

| Caso | Resultado esperado y obtenido | Debug / Release / sanitizers | Evidencia |
|---|---|---|---|
| RRF-01 | `[102,101,104,103]`, scores del ejemplo | PASS / PASS / PASS | [JSON](evidence/2026-10-26-cp03/debug/RRF-01.json) |
| RRF-02 | Rankings idénticos: `[101,102,103]`, dos contribuciones por documento | PASS / PASS / PASS | [JSON](evidence/2026-10-26-cp03/debug/RRF-02.json) |
| RRF-03 | Rankings invertidos: `[101,103,102]`; 101/103 empatan | PASS / PASS / PASS | [JSON](evidence/2026-10-26-cp03/debug/RRF-03.json) |
| RRF-04 | Disjuntos: `[101,103,102,104]`, unión completa | PASS / PASS / PASS | [JSON](evidence/2026-10-26-cp03/debug/RRF-04.json) |
| RRF-05 | Solo denso: `[101,102,103]` | PASS / PASS / PASS | [JSON](evidence/2026-10-26-cp03/debug/RRF-05.json) |
| RRF-06 | Solo léxico: `[102,104,101]` | PASS / PASS / PASS | [JSON](evidence/2026-10-26-cp03/debug/RRF-06.json) |
| RRF-07 | Ambos vacíos: `[]` | PASS / PASS / PASS | [JSON](evidence/2026-10-26-cp03/debug/RRF-07.json) |
| RRF-08 | Top-1: `[102]` | PASS / PASS / PASS | [JSON](evidence/2026-10-26-cp03/debug/RRF-08.json) |
| RRF-09 | Top-2: `[102,101]`; también prefijos Top-3/4 | PASS / PASS / PASS | [JSON](evidence/2026-10-26-cp03/debug/RRF-09.json) |
| RRF-10 | K mayor que U: cuatro documentos; también SIZE_MAX | PASS / PASS / PASS | [JSON](evidence/2026-10-26-cp03/debug/RRF-10.json) |
| RRF-11 | Duplicado denso: `std::invalid_argument` | PASS / PASS / PASS | [JSON](evidence/2026-10-26-cp03/debug/RRF-11.json) |
| RRF-12 | Duplicado léxico: `std::invalid_argument` | PASS / PASS / PASS | [JSON](evidence/2026-10-26-cp03/debug/RRF-12.json) |
| RRF-13 | Un documento en ambas: `[101]`, score `2/61` | PASS / PASS / PASS | [JSON](evidence/2026-10-26-cp03/debug/RRF-13.json) |
| RRF-14 | Empate: `[100,200]` con igual score | PASS / PASS / PASS | [JSON](evidence/2026-10-26-cp03/debug/RRF-14.json) |
| RRF-15 | k_rrf=1: score `1/2`; variante k_rrf=10: `1/11` | PASS / PASS / PASS | [JSON](evidence/2026-10-26-cp03/debug/RRF-15.json) |
| RRF-16 | k_rrf=100: score `1/101` | PASS / PASS / PASS | [JSON](evidence/2026-10-26-cp03/debug/RRF-16.json) |
| RRF-17 | k_rrf=0 y −0: `std::invalid_argument` | PASS / PASS / PASS | [JSON](evidence/2026-10-26-cp03/debug/RRF-17.json) |
| RRF-18 | k_rrf negativo: `std::invalid_argument` | PASS / PASS / PASS | [JSON](evidence/2026-10-26-cp03/debug/RRF-18.json) |
| RRF-19 | NaN, +∞ y −∞: `std::invalid_argument` | PASS / PASS / PASS | [JSON](evidence/2026-10-26-cp03/debug/RRF-19.json) |
| RRF-20 | top_k=0: `std::invalid_argument` | PASS / PASS / PASS | [JSON](evidence/2026-10-26-cp03/debug/RRF-20.json) |
| RRF-21 | 50 ciclos del ejemplo y del empate, mismos IDs y scores exactos | PASS / PASS / PASS | [JSON](evidence/2026-10-26-cp03/debug/RRF-21.json) |
| RRF-22 | Entradas intactas; salida `[200,427,0,91]` | PASS / PASS / PASS | [JSON](evidence/2026-10-26-cp03/debug/RRF-22.json) |

`FusionCorrecta = 22 / 22 × 100 = 100%` en cada perfil. El denominador es la
matriz obligatoria, sin inflarlo con variantes, repeticiones o pruebas previas.
Esta métrica acredita corrección de la fusión, no relevancia documental.

Las actividades T1–T9 están cubiertas por esta matriz y propiedades. T10 se
comprueba en `integration.rrf_contract`: SearchResult preparados con VectorId
9010/7000/42 se asocian a DocumentId 101/102/103 fuera de RRF. Se verifica el
ranking final y que un cambio monótono de las distancias/scores originales no
afecte la fusión. Asociaciones ausentes o documentos duplicados tras convertir
se detectan. **Es una validación de contrato**, no una ejecución de QueryEngine
híbrido ni una consulta real HNSW+BM25.

`unit.rrf.properties` añade 200 pares de rankings generados con semilla 42 y
constantes 0.25/1/10/60/100: unión, scores, orden, Top-K como prefijo, simetría
entre canales e inmutabilidad. Incluye IDs cero/máximo, spans solapados,
configuraciones positivas extremas, duplicados posteriores a Top-1 y
reutilización del objeto tras un error. Estos checks adicionales no cambian
el denominador de KR1.4.

## 4. Entorno y ejecución reproducible

Base Git: `0d15fd0f66134cd371601ec0a13070c47e7dcfa7`, rama `feature/rrf`.
Las pruebas incluyen los cambios de esta entrega en el árbol de trabajo;
el commit base por sí solo no contiene RRF. Los
[SHA-256 de fuentes](evidence/2026-10-26-cp03/source-sha256.json) identifican el
código validado. No se creó un commit ni un Pull Request.

Entorno observado: macOS 26.5.2 (25F84), ARM64, SDK macOS 26.2, C++20,
CMake 4.4.4 y Ninja 1.13.2. Se usó AppleClang 17.0.0 para Debug/Release y
Homebrew Clang 23.1.3 para ASan/UBSan. Los flags reales del módulo y hashes de
biblioteca/binario/configuración figuran en
[suite-summary.json](evidence/2026-10-26-cp03/suite-summary.json).

| Perfil limpio | Pruebas aprobadas | CP-03 | Tiempo de la suite completa | Log |
|---|---:|---:|---:|---|
| Debug | 52/52 | 24/24 | 38.91 s | [CTest](evidence/2026-10-26-cp03/debug-ctest.log) |
| Release | 52/52 | 24/24 | 8.20 s | [CTest](evidence/2026-10-26-cp03/release-ctest.log) |
| Debug ASan/UBSan | 52/52 | 24/24 | 114.73 s | [CTest](evidence/2026-10-26-cp03/sanitizers-ctest.log) |
| Release, BUILD_TESTING=OFF | Compilación completa aprobada; cero tests registrados | Biblioteca aislada aprobada | No aplica | [Build](evidence/2026-10-26-cp03/no-tests-build.log) |

Las 52 comprobaciones son 27 previas, 24 de CP-03 y una de compatibilidad
JSONL. Las 24 de CP-03 son la matriz de 22, propiedades y contrato. No se
activaron los datasets externos optativos ni los benchmarks. Los segundos
anteriores corresponden a la suite completa y **no miden latencia de consulta**.
La sonda opcional del nombre del procesador no estuvo disponible por permisos;
no se utiliza para formular una afirmación de rendimiento.

ASan/UBSan se ejecutaron con `halt_on_error=1` y UBSan con `print_stacktrace=1`.
No se detectaron errores en las rutas ejercitadas. La procedencia de ambos
compiladores y el entorno quedan en
[environment.json](evidence/2026-10-26-cp03/environment.json). Esta ejecución no
certifica Windows/Linux ni todos los posibles fallos de asignación.

También aprobaron `-Wall -Wextra -Wpedantic -Werror` para las seis unidades C++
nuevas/modificadas y la verificación de formato. Se compiló y ejecutó el
[ejemplo público](evidence/2026-10-26-cp03/standalone_consumer.cpp) enlazando
**solo `libaether_hybrid.a`**, sin bibliotecas HNSW/BM25/application:
[compilación](evidence/2026-10-26-cp03/standalone-consumer-compile.log),
[salida](evidence/2026-10-26-cp03/standalone-consumer-run.log) y
[dependencias de construcción](evidence/2026-10-26-cp03/standalone-hybrid-dependencies.log).

Los comandos exactos de configuración, compilación y CTest están en
[runs.json](evidence/2026-10-26-cp03/runs.json); las verificaciones adicionales,
en [source-checks.json](evidence/2026-10-26-cp03/source-checks.json).

Para repetir las pruebas de RRF desde la raíz:

```sh
cmake -S . -B build-rrf -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build-rrf --parallel 4
ctest --test-dir build-rrf -L cp03 --output-on-failure
ctest --test-dir build-rrf -L kr1_4 --output-on-failure
ctest --test-dir build-rrf --output-on-failure
```

Para recalcular las evidencias guardadas con fracciones exactas:

```sh
python3 docs/reports/evidence/2026-10-26-cp03/verify_matrix.py
```

## 5. Reparación necesaria de compatibilidad previa

El árbol inicial no configuraba: CMake hacía referencia a
`src/application/jsonl_loader.cpp`, mientras el archivo existente era
`json_loader.cpp`. El código JSONL también usaba un header ausente, el antiguo
VectorStore por defecto/`add_vector`, métodos de string sobre filesystem::path
y una API léxica incompatible. Se corrigieron esas referencias para poder
construir y comprobar todo el proyecto.

La ruta vectorial JSONL ahora conserva el algoritmo seleccionado y la métrica,
comprueba consultas/embeddings ausentes y carga la consulta antes del timer de
búsqueda. `integration.jsonl_service_compatibility` prueba ambos algoritmos y
métricas, así como corpus/consultas vacíos, embeddings ausentes y dimensiones
incompatibles. El detalle del alcance está en
[baseline-compatibility.json](evidence/2026-10-26-cp03/baseline-compatibility.json).

Se mantiene el parser manual JSONL existente: estas comprobaciones no lo
acreditan como parser JSON completo. El corpus con `embedding:null` requiere
embeddings reales para búsqueda vectorial; no se fabrican ni se evalúa su
relevancia en esta tarjeta. Estos arreglos no implementan CP-04.

## 6. Pendientes para las siguientes tarjetas y el cierre

- CP-04: orquestar recuperadores, asociación documental, presupuestos de
  candidatos, validación de corpus y CLI híbrida.
- Evaluación de relevancia: usar corpus/consultas etiquetados y medir las
  métricas acordadas; el 100% aritmético de KR1.4 no acredita KR1.5 ni una mejora
  de Hit Rate/Recall.
- Revisión por otro integrante y revisión/aprobación del Pull Request para el
  cierre formal de CP-03.

La [guía técnica de RRF](../algorithms/rrf.md) contiene el contrato público, la
fórmula, ejemplos de consumo y la responsabilidad del futuro adaptador CP-04.
