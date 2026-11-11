# Corpus de regresión BM25 de CP-02

100 documentos sintéticos en español, organizados en 20 familias técnicas con
un documento objetivo y cuatro distractores por familia. Las 20 consultas y
sus DocumentId esperados se escribieron antes de ejecutar el buscador.
Los IDs son no secuenciales; no son posiciones de arrays ni VectorId.

Este material prueba carga, tokenización de identificadores, asociaciones,
ranking y generación reproducible de reportes. Las consultas incluyen códigos
literales; el fixture es deliberadamente fácil y no representa demanda real.
No proviene de incidentes, equipos o registros de producción ni de una fuente
externa. Es material sintético del proyecto para pruebas, sin descarga.

**No es CP-01**, no certifica Hit Rate@10 del KR1.3 y no sustituye la evaluación
oficial CP-05. El usuario confirmó que CP-01 aún no existe el 25 de octubre de
2026. El evaluador admite otro corpus cuando se prepare.

Formato UTF-8 sin encabezado:

- `documents.tsv`: `document_id<TAB>text`.
- `queries.tsv`: `query_id<TAB>expected_document_id<TAB>query_text`.

Los campos no contienen tabuladores ni saltos de línea internos. DocumentId
es decimal uint32, sin signo. Cada consulta tiene un documento esperado.
Las etiquetas son explícitas, no se calculan desde el ranking BM25.
