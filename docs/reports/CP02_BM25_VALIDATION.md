# CP-02 — Implementación y validación de BM25

**Fecha local:** 25 de octubre de 2026 · **Sprint:** 2

## Resultado y estado de la tarjeta

Se implementó la biblioteca independiente `aether_lexical` en C++20: tokenizer,
índice invertido, estadísticas, fórmula BM25 y ranking Top-K documental. El
namespace se adaptó a `aetherindex::lexical`, manteniendo el namespace existente
del proyecto. Los tipos comunes nuevos son `DocumentId` uint32 y `RankedResult`
con score double; `SearchResult` vectorial conserva su contrato de distancia.

La implementación y sus comprobaciones locales están completas. El **cierre
formal de la tarjeta** conserva pendientes: evaluación sobre CP-01 cuando exista
y revisión/aprobación del PR por otro integrante. No se certifica todavía KR1.3
ni la meta oficial Hit Rate@10≥90% de CP-05.

El usuario confirmó durante esta tarea que **CP-01 aún no existe**. Por acuerdo,
se entregó un fixture de regresión separado y un evaluador/adaptador preparado
para recibir posteriormente ese corpus. No se sustituyeron etiquetas oficiales
por resultados del propio algoritmo.

## Implementación entregada

| Actividad | Resultado |
|---|---|
| C1 | Target `aether_lexical`, enlazado solo con core y biblioteca estándar; no depende de HNSW, CLI ni RRF. |
| C2 | `Tokenizer` `ascii-tech-v1`: lowercase ASCII, dígitos, identificadores internos con `_`/`-`, `C++`, UTF-8 validado/preservado. |
| C3 | `InvertedIndex`: TF por documento, una entrada por documento/término, postings ordenados por DocumentId, vistas const. |
| C4 | N, longitudes uint32, total de tokens uint64, DF y avgdl; vacíos incluidos en N/media. |
| C5 | IDF con log1p, contribución BM25 y acumulación double; configuración y finitud validadas. |
| C6 | Consulta con términos ordenados/únicos, candidatos por postings, suma por documento, score descendente e ID ascendente, truncamiento K. |
| C7 | COLLECTING/READY; rechazo de duplicados, K=0, operaciones fuera de estado y UTF-8 inválido; publicación transaccional de build. |
| C8 | `vector<RankedResult>` consumible por posición/rango; prueba de contrato sin RRF ni HNSW. |
| C9 | Ocho checks CTest nuevos, oráculo independiente y fallos de asignación aislados. |
| C10 | [Guía técnica, API, ejemplos y limitaciones](../algorithms/bm25.md), actualización de arquitectura y README. |

La política conservadora y el tratamiento de vacíos siguen la especificación
CP-02 y actualizan las propuestas más amplias del análisis D-01. No se incorporó
una dependencia Unicode, ni normalización NFC, ni case folding no ASCII. Tampoco
se implementaron RRF, QueryEngine híbrido, comandos textuales de la CLI,
DocumentStore, generación de embeddings o persistencia.

## Compilaciones y pruebas ejecutadas

Todas las configuraciones finales usaron directorios nuevos, Ninja, C++20,
`AETHER_BUILD_BENCHMARKS=OFF` y SDK macOS seleccionado con CMake. El entorno de
ejecución fue macOS 26.5.2 ARM64. No se usó la duración de CTest como latencia
del buscador.

| Perfil | Compilador | Resultado |
|---|---|---|
| Debug limpio | AppleClang 17.0.0, clang-1700.6.3.2 | **27/27**: 19 checks previos y 8 de CP-02. |
| Release limpio | AppleClang 17.0.0 | **27/27**, sin warnings en el log de build. |
| Debug ASan+UBSan limpio | Homebrew Clang 23.1.3 | **27/27**, sin diagnósticos de sanitizadores en las rutas evaluadas. |
| `BUILD_TESTING=OFF`, solo lexical | AppleClang 17, Release | Build correcto; las órdenes Ninja no compilan ni enlazan HNSW, aplicación o CLI. |
| `BUILD_TESTING=OFF`, proyecto completo | AppleClang 17, Release | Build correcto. |
| Consumidor independiente | Solo headers públicos y `libaether_lexical.a` | El ejemplo documentado compiló y ejecutó correctamente, sin enlazar otras bibliotecas del motor. |
| Compilación estricta lexical | `-Wall -Wextra -Wpedantic -Werror` | Los tres archivos de implementación aprobaron. |
| Adaptador de corpus optativo | Fixture sintético como entrada externa | CTest externo aprobado; no se presentó como evaluación real de CP-01. |
| Directorio de corpus inexistente | Configuración negativa de CMake | Rechazo esperado con diagnóstico del archivo ausente. |

No se ejecutaron los tests optativos vectoriales sobre DBpedia ni los benchmarks
de rendimiento en esta tarjeta. Tampoco se validó ejecución Windows/Linux. El
target MSVC configura `/utf-8`, pero esa configuración requiere su comprobación
en la plataforma correspondiente.

## Cobertura de T1–T12

| Prueba | Evidencia | Estado |
|---|---|---|
| T1 | `unit.tokenizer`: los casos obligatorios, repetibilidad, puntuación, secuencias multibyte y propiedad del resultado. | Aprobada. |
| T2 | `unit.inverted_index`: TF exacto, DF, unicidad, vistas const, ausencia, IDs cero/máximo y no secuenciales. | Aprobada. |
| T3 | `unit.bm25_statistics`: N=3, longitudes 3/2/1, total=6, DF y avgdl=2; vacíos y mixtos. | Aprobada. |
| T4 | `unit.bm25_math`: IDF de referencia, entradas inválidas y N/DF grandes. | Aprobada. |
| T5 | Referencias BM25 individuales y búsqueda pública, con tolerancia double absoluta estricta <1e-6. | Aprobada. |
| T6 | Suma de contribuciones y eliminación de repeticiones de consulta, sin repetir documentos. | Aprobada. |
| T7 | Top-1/2/10, orden, desempates, prefijos y K máximo de size_t. | Aprobada. |
| T8 | Consulta vacía, solo separadores y OOV; ningún documento artificial de relleno. | Aprobada. |
| T9 | Buscar antes de build, insertar después, segundo build, duplicados y recuperación posterior. | Aprobada. |
| T10 | Parámetros finitos/válidos, K=0, corpus vacío/todos vacíos, UTF-8 inválido y extremos numéricos. | Aprobada. |
| T11 | Consumidor de la API obtiene IDs únicos ordenados y rangos desde 1. | Contrato aprobado; no es integración con el algoritmo RRF. |
| T12 | Fixture técnico de regresión y reportes completos de 20 consultas; adaptador listo. | **Parcial respecto a la tarjeta original:** ejecución sobre CP-01 pendiente de ese corpus. |

El oráculo adicional de búsqueda recorre todos los arrays originales de tokens
de 120 documentos determinísticos y compara 50 consultas, calculando por su
cuenta DF, longitudes y fórmula. No reutiliza el tokenizer, postings, scorer
ni estadísticas del módulo para construir los valores de referencia.

Las pruebas de asignación fallan cada punto de la operación de manera aislada.
Se comprobaron **14 puntos de fallo de inserción, 15 de build y 6 de búsqueda**
en cada perfil. Después de un build fallido no hay READY parcial, se pueden
agregar documentos y reintentar; tras una búsqueda fallida, el ranking y los
spans originales siguen válidos.

## Evidencia matemática independiente

Para el corpus `101: motor motor e47`, `102: motor temperatura`, `103: bomba`,
con N=3, avgdl=2, k1=1.2 y b=0.75:

| Consulta | Documento | Score esperado aproximado |
|---|---:|---:|
| e47 | 101 | 0.8142733421 |
| motor | 101 | 0.5665797174 |
| motor | 102 | 0.4700036292 |
| motor e47 | 101 | 1.3808530596 |
| motor e47 | 102 | 0.4700036292 |

Además de los tests C++, se ejecutó el evaluador con ese corpus y se contrastaron
los cinco resultados con aritmética Decimal independiente a precisión 60.
Para `e47` se obtuvo `0.81427334212294267`; el error absoluto respecto al cálculo
independiente fue aproximadamente **1.34e-16**, menor que **1e-6**. El resto de
los resultados también aprobó la tolerancia estricta. Valores completos en
[numerical-reference.json](evidence/2026-10-25-cp02/numerical-reference.json).

## Evaluación preliminar, sin certificación de calidad

El fixture propio de CP-02 tiene **100 documentos sintéticos**, 325 términos
distintos, 2,182 tokens y avgdl=21.82. Hay 20 consultas etiquetadas manualmente
antes de ejecutar la búsqueda; cada una identifica un documento esperado y
utiliza códigos técnicos presentes literalmente en el objetivo.

Los tres perfiles recuperaron el documento esperado en **20/20 Top-10**. El
fixture es deliberadamente fácil y está destinado a regresión de carga/ranking;
ese 100% no acredita relevancia sobre un corpus representativo ni cumplimiento
de KR1.3. Los Top-10, posiciones y misses se conservan sin deducir etiquetas
desde scores. Cuando exista CP-01, sus documentos y consultas se entregarán al
mismo evaluador; la meta oficial ≥18/20 se revisará en CP-05.

## Archivos de evidencia

- [Manifiesto de código, compiladores, configuración y resultados](evidence/2026-10-25-cp02/manifest.json): hashes de 79 entradas fuente/fixtures, commit base y aviso de cambios sin commit.
- [Órdenes y retornos de los builds/checks](evidence/2026-10-25-cp02/runs.json) y [comprobaciones adicionales](evidence/2026-10-25-cp02/extra-runs.json).
- [CTest Debug](evidence/2026-10-25-cp02/debug-ctest.log), [Release](evidence/2026-10-25-cp02/release-ctest.log), [ASan/UBSan](evidence/2026-10-25-cp02/sanitizers-ctest.log) y [opciones de runtime](evidence/2026-10-25-cp02/sanitizer-environment.json).
- [Órdenes del target lexical aislado](evidence/2026-10-25-cp02/standalone-lexical-dependencies.log) y [compilación estricta](evidence/2026-10-25-cp02/strict-compile.json).
- [Resumen de regresión Debug](evidence/2026-10-25-cp02/debug/summary.json), [posiciones de los documentos esperados](evidence/2026-10-25-cp02/debug/queries.csv) y [rankings Top-10](evidence/2026-10-25-cp02/debug/rankings.csv); también se conservan los reportes Release y sanitizados.

No se creó ni aprobó un PR, ni se solicitó revisión a terceros desde esta tarea.
La revisión de equipo y los indicadores de producto conservan sus requisitos
separados de estas comprobaciones locales de implementación.
