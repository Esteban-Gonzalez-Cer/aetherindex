# Corpus híbrido derivado de CP-01

`corpus.jsonl` y `queries.jsonl` son los mismos 30 documentos y 24 consultas
etiquetadas de `datasets/corpus.jsonl` y `datasets/queries.jsonl`, con un campo
`embedding` de 128 dimensiones añadido **fuera del motor** por
`scripts/generate_hashing_embeddings.py`:

- Texto: `title + "\n" + text` para documentos y `query_text` para consultas.
- NFC + *case folding* Unicode; palabras = secuencias de letras/dígitos.
- Rasgos: cada palabra y los trigramas de `" palabra "`, asignados con BLAKE2b a
  128 posiciones con signo; pesos `1 + log(tf)`; normalización L2.
- Perfil `aether-hash-trigram-v1`; métrica coseno.

Las etiquetas `relevant_document_ids` se copian sin cambios y **no** se usan para
construir ningún vector. Es una representación léxico-morfológica reproducible,
sin descarga ni modelo; **no es un modelo semántico**. Su propósito es dar al
canal denso una entrada real y verificable para pruebas de integración (KR1.5),
no demostrar que la búsqueda híbrida mejore la relevancia.

`manifest.json` registra los SHA-256 de las fuentes y de los archivos generados.
Para regenerar o comprobar:

```sh
python3 scripts/generate_hashing_embeddings.py
python3 scripts/generate_hashing_embeddings.py --check
```

Cuando el equipo genere embeddings con un modelo, puede reemplazar el campo
`embedding` (misma dimensión en documentos y consultas) sin cambiar el formato.
