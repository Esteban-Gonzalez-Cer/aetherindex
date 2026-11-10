#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

#include "aether/ann/hnsw/hnsw_index.hpp"
#include "aether/lexical/bm25.hpp"
#include "aether/quantization/quantized_vectors.hpp"
#include "aether/storage/vector_store.hpp"

namespace aetherindex {

// Component snapshots. Each component is one section file (see io/binary_io.hpp) that
// records its in-memory state exactly, so a loaded component answers every query
// bit-identically to the one that was saved, and saving the same state twice gives the
// same bytes. Saving writes a temporary file and renames it into place after syncing it.
//
// Loading always validates the structure (sizes, offsets, graph references, block layout)
// before anything can index memory with it. verify_checksums additionally checks the
// payload CRC-32C, one pass over the bytes. Errors throw io::IoError naming the file.
namespace snapshot {

enum class Section : std::uint32_t {
    Vectors = 1,
    Hnsw = 2,
    Bm25 = 3,
    Sq8 = 4,
    Pq = 5,
    Documents = 6,
};

void save(const VectorStore& store, const std::filesystem::path& path);
std::unique_ptr<VectorStore> load_vector_store(const std::filesystem::path& path,
                                               bool verify_checksums);

// The store must be the one the index was built on (same IDs in the same positions);
// load checks the sizes and the stored ID checksum. ef_search replaces the saved default.
void save(const HnswIndex& index, const std::filesystem::path& path);
std::unique_ptr<HnswIndex> load_hnsw(const std::filesystem::path& path, const VectorStore& store,
                                     bool verify_checksums,
                                     std::optional<std::size_t> ef_search = std::nullopt);

// Query-time settings may differ from the saved ones: the strategy (among the compressed
// strategies, which share one layout) and the query term weighting. Everything that
// shapes the index (k1, b, analyzer, compressed or exhaustive) is fixed at build time.
struct Bm25QueryOverrides {
    std::optional<lexical::Bm25Strategy> strategy;
    std::optional<lexical::QueryTermWeighting> query_weighting;
};
void save(const lexical::Bm25Index& index, const std::filesystem::path& path);
std::unique_ptr<lexical::Bm25Index> load_bm25(const std::filesystem::path& path,
                                              bool verify_checksums,
                                              const Bm25QueryOverrides& overrides = {});

// SQ8 or PQ codes; the section kind records which.
void save(const QuantizedVectors& codes, const std::filesystem::path& path);
std::unique_ptr<QuantizedVectors> load_quantized(const std::filesystem::path& path,
                                                 bool verify_checksums);

}  // namespace snapshot
}  // namespace aetherindex
