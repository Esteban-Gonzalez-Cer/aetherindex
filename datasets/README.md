# Datasets

Directorio reservado para datos de evaluación de búsqueda densa, léxica e híbrida.
`examples/` contiene un dataset sintético 2D de cuatro vectores y una consulta,
creados dentro del proyecto para demostrar la CLI. No provienen de un corpus
externo y no requieren descarga. Los IDs son 10, 20, 30 y 40; el CSV del dataset
contiene `ID,x,y` y la consulta contiene `x,y`, ambos sin encabezado.

Para el escenario MVP, `aether_cli_fixture_generator` (con `BUILD_TESTING=ON`)
genera 1,000 embeddings sintéticos de dimensión 128 y diez consultas con semilla
42, IDs `7*i+10` y representación float recuperable sin pérdida. El dataset se
genera en el directorio de compilación y no se versiona. Consultar
[las instrucciones de la CLI](../docs/cli.md).

Antes de incorporar un dataset, documentar su origen, licencia, instrucciones de
descarga, formato, dimensión de los embeddings y relación entre `DocumentId` y
`VectorId`. Mantener los archivos de gran tamaño fuera de Git y usar `scripts/`
para preparar los datos cuando sea necesario.

Los embeddings se recibirán previamente calculados; su generación no pertenece
al núcleo de AetherIndex.

El [caso real DBpedia](../docs/real_world_validation.md) prepara **10,000 artículos
× 1,024 dimensiones** y 20 consultas separadas, usando embeddings publicados por
Qdrant. Los CSV conservan el mismo esquema y se guardan en `real/dbpedia/`,
excluido de Git. `manifest.json` registra la revisión de la fuente, el mapeo de
IDs y los checksums; `articles.jsonl` y `queries.json` permiten relacionar cada
vector con su título y texto originales. La fuente y el estado de licencia están
documentados en la guía del caso real.
