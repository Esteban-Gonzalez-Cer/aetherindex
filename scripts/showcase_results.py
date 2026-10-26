#!/usr/bin/env python3
"""Show DBpedia article titles for an existing AetherIndex CLI result.

Presentation helper only: does not compute embeddings or execute vector search.
Uses the Python 3.10+ standard library. Run from the repository root.
"""

import argparse
import json
import re
import textwrap
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ROW = re.compile(r"^\|\s+(\d+)\s+\|\s+(\d+)\s+\|\s+(\d+\.\d{9})\s+\|$")


def read_result(path):
    text = path.read_text(encoding="utf-8")
    metadata = dict(line.split(": ", 1) for line in text.splitlines() if ": " in line)
    rows = [match.groups() for line in text.splitlines() if (match := ROW.fullmatch(line))]
    if not rows or "Search completed successfully." not in text:
        raise ValueError(f"No hay una respuesta CLI completa en {path}")
    if len(rows) != int(metadata["Results returned"]) or len({r[1] for r in rows}) != len(rows):
        raise ValueError(f"Cantidad de resultados o IDs duplicados inválidos en {path}")
    if [int(row[0]) for row in rows] != list(range(1, len(rows) + 1)):
        raise ValueError(f"Ranking no consecutivo en {path}")
    return metadata, rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-dir", type=Path, default=ROOT / "datasets/real/dbpedia")
    parser.add_argument("--query-index", type=int, default=0)
    parser.add_argument("--results", type=Path, help="Salida guardada del comando search")
    parser.add_argument("--reference", type=Path, help="Salida BruteForce de la misma consulta")
    args = parser.parse_args()
    try:
        manifest = json.loads((args.data_dir / "manifest.json").read_text(encoding="utf-8"))
        queries = json.loads((args.data_dir / "queries.json").read_text(encoding="utf-8"))
        query = next((q for q in queries if q["query_index"] == args.query_index), None)
        if query is None:
            raise ValueError("El índice de consulta no existe en queries.json")
        if args.reference and not args.results:
            raise ValueError("--reference necesita --results")
        print("Contexto de la demostración (metadata externa a la CLI)")
        print(f"Catálogo: {manifest['vectors']:,} artículos; dimensión: {manifest['dimension']}")
        print(f"Artículo de consulta: {query['title']}")
        print(f"Embedding de consulta: {query['file']}")
        print(textwrap.fill(query["text"], width=95))
        if not args.results:
            return
        metadata, rows = read_result(args.results)
        if int(metadata["Vectors indexed"]) != manifest["vectors"] or \
                int(metadata["Dimensions"]) != manifest["dimension"]:
            raise ValueError("Los resultados no corresponden al tamaño/dimensión del catálogo")
        with (args.data_dir / "articles.jsonl").open(encoding="utf-8") as stream:
            articles = {str(article["vector_id"]): article for line in stream
                        if (article := json.loads(line))}
        print(f"\nAlgoritmo: {metadata['Algorithm']}; métrica: {metadata['Distance metric']}")
        print(f"Tiempo de consulta registrado (ms): {metadata['Query time (ms)']}")
        print("Rank | Vector ID | Distancia   | Artículo recuperado")
        print("-----+-----------+-------------+----------------------------------------------------")
        for rank, vector_id, distance in rows:
            if vector_id not in articles:
                raise ValueError(f"No existe metadata para el ID {vector_id}")
            print(f"{rank:>4} | {vector_id:>9} | {distance} | {articles[vector_id]['title']}")
        if args.reference:
            reference, exact = read_result(args.reference)
            if reference["Algorithm"] != "BruteForce":
                raise ValueError("La referencia debe usar BruteForce")
            for field in ("Vectors indexed", "Dimensions", "Requested K", "Distance metric"):
                if reference[field] != metadata[field]:
                    raise ValueError(f"Referencia incompatible: {field}")
            k = int(reference["Requested K"])
            if len(exact) != k:
                raise ValueError("Esta comparación requiere un Top-K exacto completo (K <= N)")
            matches = len({row[1] for row in rows} & {row[1] for row in exact})
            print(f"\nRecall@{k} de ESTA consulta: {matches}/{k} = {matches / k:.3f}")
            print("Compara IDs con BruteForce; no mide relevancia humana ni respuestas de un LLM.")
    except (OSError, ValueError, KeyError) as error:
        parser.exit(1, f"Error de presentación: {error}\n")


if __name__ == "__main__":
    main()
