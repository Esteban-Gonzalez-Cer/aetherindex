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
aetherindex index checkpoint --index idx
```

`add` and `delete` append to the index's write-ahead log, so they cost the size of the
change, not of the index (see [Write-ahead log](#write-ahead-log)). `compact` and
`checkpoint` write a new snapshot and empty the log. Every change is visible to the next
search.

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
├── wal.log                          operations since the snapshot (write-ahead log)
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

## Write-ahead log

Operations made after a snapshot live in `wal.log`.

The file is a 16-byte header (magic `AETHWAL1`, version, header CRC-32C) followed by records:

| Field | Bytes |
|---|---|
| payload size | 4 |
| CRC-32C of everything after it | 4 |
| operation sequence | 8 |
| operation (1 add, 2 delete, 3 compact) | 1 |
| payload | variable |

The payload of each operation:

- **add:** the `--replace` flag and the input files exactly as they were read.
- **delete:** the IDs.
- **compact:** nothing.

**When an operation is durable.** The record is appended after the operation succeeded in
memory, then flushed to stable storage (`F_FULLFSYNC` on Apple platforms). The command
returns after that.

- Each snapshot's manifest records the last operation it contains.
- With `--no-sync`, a record survives a crash of the process but not necessarily a power
  loss.
- Cost on Apple M4 Pro: about 4 ms per synced append, 0.02 ms without the flush.

**Opening an index** loads the snapshot and replays the records newer than it:

- Reading stops at the first record that is truncated, fails its checksum or breaks the
  sequence. A crash can only damage the end of the log, and that end was never
  acknowledged.
- The next writer truncates the file there before appending.
- `index info` reports how many operations were replayed and whether a damaged tail was
  ignored.

**A checkpoint** (`index checkpoint`, also part of `index compact`) saves a snapshot that
contains every logged operation, then empties the log. A crash between the two is harmless:
replay skips records the snapshot already holds.

**Recovery is tested by killing the process.** `e2e.cli_crash` kills the CLI
(`AETHER_CRASH_AT`, a test-only fault-injection hook) at five points:

- halfway through writing a record;
- after writing a record but before flushing it;
- in a checkpoint, before `CURRENT` moves;
- in a checkpoint, after `CURRENT` moves but before the log is emptied;
- in the checkpoint of a compaction.

After each crash the index must answer exactly like a crash-free index holding a prefix of
the operations that includes every acknowledged one. To check that the test can catch a
real bug, the writer was changed to append without first truncating the damaged tail; the
test failed. `unit.write_ahead_log` covers the reader's edge cases.

## Validation when loading

Loading never trusts a size or an offset before checking it:

- **Headers and files.** Header checksum, section kind and version, and that each file's
  size matches both its header and the manifest.
- **Vectors.** No duplicate live IDs, and the row count matches the ID count. Rows are
  stored without their in-memory padding, and IDs only when they are not 0..N-1.
- **HNSW.**
  - Every degree is at most `M_max` and every upper-layer offset is in range.
  - Every neighbor a traversal can follow names a node present at that layer, and the entry
    point, top level and node count agree. This is one pass over the edges.
  - The graph was built on the same vector IDs (an ID checksum).
  - The complete structural report (also duplicate edges and degree rules) runs in
    `index verify`.
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
