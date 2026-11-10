# Concurrency

A `Collection` can be searched by any number of threads while one thread changes it with
`add`, `remove`, `compact` or `save`. Writers are serialized among themselves.

## Protocol

Two locks and one lock-free structure:

- **Writer mutex.** Serializes `add`, `remove`, `compact` and `save`.
- **State lock** (`std::shared_mutex`). Searches hold it shared. A writer holds it exclusively
  only to *publish*: append rows, set tombstones, commit a BM25 segment, update the
  document catalog, swap compacted components. Publishing is bookkeeping proportional to
  the batch, never index construction.
- **HNSW graph.** New nodes are linked *after* publishing, while searches run. Readers load
  adjacency words with relaxed `std::atomic_ref` loads, and writers store them atomically
  under per-node stripe locks.
  - A node becomes reachable only through the backlinks written last, so its own lists are
    complete before any search can reach it.
  - The entry point and top level live in one atomic word (release/acquire), so a search
    always sees a matching pair.

Expensive work happens with no lock that blocks searches:

- building the BM25 segment of an `add`;
- computing corpus-wide scoring data before a delete;
- the whole rebuild inside `compact`, which only takes the exclusive lock to swap the new
  components in.

### What a search observes

- **Whole writes.** Each publish is seen entirely or not at all: an `add`'s rows, BM25
  segment and catalog entries, a `remove`'s tombstones, a `compact`'s swap.
- **Graph links.** The exception is the nodes an `add` links into the HNSW graph, which
  become reachable one by one.
  - While they are being linked, a hybrid search can already return a new document through
    BM25, with no dense rank.
  - A vector search may not find a new vector until its links are written.
- **Batches.** A batch search (`all_queries`) holds the shared lock for the whole batch and
  sees one consistent state.

### What does not change

With no writer, a search takes plain loads through exactly the code path that existed
before concurrency support. The only addition is an uncontended shared lock per call.

## Evidence

**ThreadSanitizer.** `integration.concurrent_collection` runs four reader threads (hybrid
queries, some filtered) against one writer that adds, deletes, replaces, saves and
compacts.

- The test checks that every result is a record that existed and that filters hold.
- After the writer stops, the graph must validate, and the compacted collection must
  answer exactly like a fresh build.
- Under ThreadSanitizer (`cmake --preset tsan`, also in CI) it reports no race.
- To check that the test can catch one, searches were made to ignore the concurrent-reads
  flag (plain loads during linking). ThreadSanitizer flagged a data race on every run.

**Cost**, measured with `benchmarks/concurrent_benchmark.cpp`: 200K × 64 clustered vectors,
HNSW M = 16, ef_search = 64, 10 threads, Apple M4 Pro, three runs.

| Measurement | Result |
|---|---|
| Shared lock around `HnswIndex::search` | +0.5 %, +1.9 %, +2.5 % (noise) |
| `Collection::search`, no writer, vs raw `HnswIndex::search` | −3.3 %, −0.5 %, −1.5 % |
| Lock-free read path alone (used only while a writer links) | −8.8 %, −1.1 %, −3.4 % |
| Writer throughput, one thread, searches running | 62–65 K inserts/s |
| Read QPS per reader while the writer inserts | about 42 % lower |

The drop during writes is not lock waiting: readers waited 46 ms in total over a 2.5 s
run. It comes from sharing memory with an active writer:

- every insertion rewrites about 40 neighbor lists that readers traverse;
- the writer competes for the shared cache (the vectors and graph, 77 MB, exceed L2).

Moving the writer-hot members (`successful_insertions_`, `entry_mutex_`) onto their own
128-byte cache lines removed the false-sharing part of the drop (124 K → 142 K reader QPS).
Batching backlink updates could reduce it further; that is future work.
