#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "aether/application/search_service.hpp"

namespace aetherindex {

struct QueryRecord;

// Query-time settings that may differ from the ones a saved collection was built with.
struct OpenOptions {
    bool verify_checksums = true;  // Check every section's CRC-32C while loading.
    std::optional<std::size_t> ef_search;
    std::optional<lexical::Bm25Strategy> bm25_strategy;  // Among the compressed strategies.
    std::optional<lexical::QueryTermWeighting> bm25_query_weighting;
};

// Sizes and timings of a collection, for reports and `index info`.
struct CollectionStats {
    std::size_t vectors = 0;
    std::size_t documents = 0;
    std::size_t dimension = 0;
    std::chrono::duration<double, std::milli> index_build_time{};
    std::chrono::duration<double, std::milli> lexical_build_time{};
    std::chrono::duration<double, std::milli> quantization_time{};
    std::size_t fp32_bytes = 0;
    std::size_t code_bytes = 0;
    bool fp32_released = false;
    // Set by open(): reading and validating the snapshot.
    std::chrono::duration<double, std::milli> load_time{};
    std::uint64_t snapshot_sequence = 0;
    std::size_t snapshot_bytes = 0;
};

// The data and indexes of one search configuration: vectors, documents, and the HNSW,
// BM25 and quantized indexes its mode needs. Built from dataset files, or opened from a
// saved snapshot directory.
//
// A request has build-time fields (dataset, mode, algorithm, metric, HNSW and BM25 shape,
// quantization) and query-time fields (queries, k, ef_search, fusion, filters, re-rank,
// batching). A collection fixes the build-time fields; search() reads only the query-time
// fields of the request it is given.
//
// Snapshot directory layout:
//   DIR/CURRENT                      name of the current snapshot, replaced atomically
//   DIR/snapshot-NNNNNNNNNNNNNNNNNNNN/
//       manifest.json                configuration, statistics and file list (written last)
//       vectors.sec hnsw.sec codes.sec bm25.sec documents.sec   (those the mode needs)
class Collection {
   public:
    ~Collection();
    Collection(const Collection&) = delete;
    Collection& operator=(const Collection&) = delete;

    // Reads the dataset files named by a validated request. No index is built yet, so
    // query files can be checked against the data before the expensive part.
    static std::unique_ptr<Collection> load(const SearchRequest& request);
    // Builds every index the configuration needs. Call once, after load().
    void build_indexes();

    // Writes the built collection as a new snapshot of `directory` (created if absent),
    // then points CURRENT at it and removes older snapshots. A crash at any point leaves
    // either the previous snapshot or the new one current, never a partial one.
    void save(const std::filesystem::path& directory) const;
    static std::unique_ptr<Collection> open(const std::filesystem::path& directory,
                                            const OpenOptions& options = {});
    // True if `directory` holds a saved collection (a CURRENT file).
    static bool is_collection(const std::filesystem::path& directory);

    // The request with its build-time fields replaced by this collection's.
    SearchRequest merged_request(const SearchRequest& request) const;
    // Reads and validates the request's queries against this collection's data.
    std::vector<QueryRecord> load_queries(const SearchRequest& request) const;
    // Runs the queries. Throws InputDataError for an unknown filter field.
    SearchResponse search(const SearchRequest& request, std::vector<QueryRecord>& queries) const;

    // Build-time configuration (the build-time fields of a SearchRequest).
    const SearchRequest& configuration() const noexcept;
    const CollectionStats& stats() const noexcept;
    // Structural graph check plus counts, for `index verify`; true when consistent.
    bool validate(std::string& report) const;

    // ----- Mutations ------------------------------------------------------------------
    // Visible to search() as soon as they return; persisted by the next save(). Each one
    // validates its whole input first, so an input error changes nothing.

    struct AddResult {
        std::size_t added = 0;
        std::size_t replaced = 0;
    };
    // Adds the records of `dataset` (and `vectors`, an .avec file, for hybrid indexes built
    // with --vectors), in the format the collection was built from. An ID that already
    // exists is an input error, unless `replace` is set: then the record replaces it and
    // keeps its DocumentId and VectorId (an update). New opaque IDs get the smallest
    // DocumentIds not in use, in source-ID order.
    AddResult add(const std::filesystem::path& dataset, const std::filesystem::path& vectors,
                  bool replace);
    // Deletes records by source ID (document_id, or the numeric VectorId of CSV/.avec
    // vectors). An unknown or repeated ID is an input error.
    std::size_t remove(std::span<const std::string> ids);
    // Drops deleted data for good: the vectors are rewritten without the deleted rows, the
    // HNSW graph and quantized codes are rebuilt over them, and BM25 is merged into one
    // segment. The result answers exactly like a collection built from the live records
    // with the same IDs. Indexes built with --drop-fp32 keep their vector tombstones.
    void compact();
    // Deleted records still held (vectors and BM25 documents) until compact().
    std::size_t deleted_count() const;

   private:
    Collection();
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace aetherindex
