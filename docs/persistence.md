# Persistence

An index is built once with `aetherindex index build` and searched any number of times with
`aetherindex search --index`. This document describes what is saved, the on-disk format,
and the guarantees.

```sh
aetherindex index build --out idx --mode hybrid --dataset datasets/hybrid/corpus.jsonl
aetherindex search --index idx --query datasets/hybrid/queries.jsonl --query-id Q002 --k 5
aetherindex index info --index idx
aetherindex index verify --index idx
```

## What a saved index answers

A search on a saved index returns **exactly** what a one-shot search with the same options
returns: same documents, same order, bit-identical scores and distances. The end-to-end test
`e2e.cli_index` checks this by replaying every golden CLI case through `index build` and
`search --index`.

A loaded component is the saved component, not a rebuilt one. The HNSW graph keeps its
random-generator state, so inserting into a loaded graph produces the same topology as
inserting into the original (`unit.snapshot` checks this). Saving a loaded component again
produces identical bytes.

## Adding, deleting and compacting

```sh
aetherindex index add --index idx --dataset more.jsonl            # new records
aetherindex index add --index idx --dataset fixes.jsonl --replace # updates by ID
aetherindex index delete --index idx --id D002 --id D019
aetherindex index compact --index idx
```

Each command opens the index, applies the change, and saves a new snapshot. Changes are
visible to the next search.

**Vectors.**

- A delete tombstones the row's position. Positions never move, so the HNSW graph and the
  quantized codes stay valid.
- HNSW still routes through a deleted node but never returns it, which is how hnswlib
  handles deletes. Exact search skips deleted rows.
- An update is a delete plus an insert under the same ID.
- New rows are inserted into the existing graph, and quantized codes encode them with the
  parameters already trained.

**BM25** works like Lucene:

- **Segments.** The index is a list of immutable compressed segments. An add indexes its
  batch as a new segment. When there are more than 8 segments, the two smallest merge.
- **Deletes.** A delete tombstones the document inside its segment. As in Lucene, corpus
  statistics (N, average length, document frequencies) keep counting a deleted document
  until its segment is merged or compacted.
- **Scoring.** Scores always use corpus-wide statistics summed over the segments, so every
  document scores exactly as an exhaustive BM25 over the same corpus. `unit.segmented_bm25`
  checks this bit for bit for every strategy, through adds, deletes, updates and merges.
- **Pruning.** MaxScore stays rank-safe across segments. Its bounds are computed at query
  time from each block's maximum term frequency and minimum document length.
- **Fast path.** An index with one segment and no deletes runs exactly the single-index code.

**Compaction** rewrites the vectors without the deleted rows, rebuilds the HNSW graph and
the quantized codes over them, and merges BM25 into one segment. The result answers
bit-identically to an index built from scratch over the live records with the same IDs.
`integration.collection_mutation` checks this in hybrid, lexical and vector modes.

Not supported: adding vectors to an index built with `--drop-fp32`, because the graph needs
FP32 rows to insert.

## Build options and query options

Options that shape an index are fixed when it is built:

- the dataset and its vectors;
- the mode and the algorithm;
- the metric;
- HNSW `M`, `M0`, `efConstruction`, seed and neighbor selection;
- BM25 `k1`, `b` and the analyzer;
- quantization;
- whether FP32 rows are kept.

`search --index` rejects them with an explanation.

Query options belong to the search. These are the queries, `k`, the fusion method and its
parameters, filters, the re-rank depth, and batching.

Three saved defaults can be overridden per search:

| Option | Allowed when |
|---|---|
| `--ef-search` | the index has an HNSW graph |
| `--bm25-strategy` | the index uses a compressed layout. All compressed strategies share it, so any of them works; `exhaustive` needs an index built with it. |
| `--bm25-query-weighting` | always |

## Directory layout

```text
idx/
├── CURRENT                          name of the current snapshot
└── snapshot-00000000000000000001/
    ├── manifest.json                configuration, statistics and file list (written last)
    ├── vectors.sec                  IDs and padded FP32 rows, exactly as in memory
    ├── hnsw.sec                     config, RNG state, levels, both adjacency layers
    ├── bm25.sec                     config, statistics, term dictionary, compressed blocks
    ├── codes.sec                    SQ8 or PQ codes (quantized indexes only)
    └── documents.sec                document identities and filter metadata
```

## Section files

Every `.sec` file has a 64-byte header followed by a payload.

| Offset | Field |
|---|---|
| 0 | magic `AETHSEC1` |
| 8 | format version (1) |
| 12 | section kind |
| 16 | payload offset (64) |
| 24 | payload size |
| 32 | payload CRC-32C |
| 36 | flags (0) |
| 60 | header CRC-32C |

The payload holds:

- little-endian scalars;
- length-prefixed strings;
- arrays, written as an element count followed by zero padding to the next 64-byte offset
  and then the raw elements.

The padding means every array is aligned in a memory-mapped file. Terms are written in
sorted order, so the same index always produces the same bytes.

CRC-32C uses the ARMv8 CRC instructions or SSE4.2 when available.

## Crash safety

Saving writes a new snapshot without touching the current one:

1. Each file is written to a temporary name, flushed to stable storage, and renamed into a
   staging directory. On Apple platforms "flushed" means `F_FULLFSYNC`, because plain
   `fsync` does not flush the drive cache there.
2. `manifest.json` is written last.
3. The staging directory is renamed to `snapshot-N` and the parent directory is synced.
4. `CURRENT` is replaced atomically by the same temporary-file-and-rename step.
5. Older snapshots are removed.

A crash at any step leaves either the previous snapshot or the new one current, never a
partial one.

## Validation when loading

Loading never trusts a size or an offset before checking it:

- **Headers and files.** Header checksum, section kind and version, and that each file's
  size matches both its header and the manifest.
- **Vectors.** No duplicate IDs, and the row count matches the ID count.
- **HNSW.**
  - Every degree is at most `M_max` and every upper-layer offset is in range.
  - The full structural check (`validate_graph`) passes: no invalid references, layer
    violations or wrong entry point.
  - The graph was built on the same vector IDs (an ID checksum).
- **BM25.** Every compressed block is decoded. Every read must stay inside the postings
  buffer and every document ordinal must be valid and ascending.
- **Quantized codes.** Sizes match the store, and every PQ code names an existing centroid.

The payload checksum is a single pass over the bytes and is on by default.
`search --index --no-verify` skips it. The structural checks above always run, because they
are what keeps a damaged file from causing out-of-bounds reads.

## Cost

SIFT1M (1 M × 128, squared Euclidean), HNSW `M=16`, `efConstruction=200`, 10 build
threads, Apple M4 Pro:

| | Time |
|---|---:|
| One-shot `search` (load the vectors, build the graph, query) | 37.2 s |
| `index build` (build 34.0 s, save 0.65 s) | 35.9 s once |
| `search --index`, checksums verified (load 334 ms) | **0.35 s** |
| `search --index --no-verify` (load 273 ms) | 0.29 s |

The snapshot is 627 MiB: 512 MiB of vectors (64-byte padded rows) plus the graph.

Loading copies each array out of the memory-mapped file into owned memory, so a loaded
index can keep accepting inserts. A zero-copy read-only mode, which would serve directly
from the mapping, is a possible future step.
