# Datasets

Directorio reservado para datos de evaluación de búsqueda densa, léxica e híbrida.
No se incluyen datasets en este scaffold.

Antes de incorporar un dataset, documentar su origen, licencia, instrucciones de
descarga, formato, dimensión de los embeddings y relación entre `DocumentId` y
`VectorId`. Mantener los archivos de gran tamaño fuera de Git y usar `scripts/`
para preparar los datos cuando sea necesario.

Los embeddings se recibirán previamente calculados; su generación no pertenece
al núcleo de AetherIndex.
