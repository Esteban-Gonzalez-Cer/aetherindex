#pragma once

#include <cstddef>
#include <optional>
#include <random>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "aether/ann/hnsw/hnsw_config.hpp"
#include "aether/ann/hnsw/hnsw_node.hpp"
#include "aether/core/search_result.hpp"
#include "aether/storage/vector_store.hpp"

namespace aetherindex {

// These are structural invariants. A valid report does not establish global
// reachability or nearest-neighbor recall; evaluate those with real queries.
struct HnswGraphValidation {
    std::size_t nodes = 0;
    std::size_t invalid_references = 0;
    std::size_t missing_embeddings = 0;
    std::size_t invalid_levels = 0;
    std::size_t layer_violations = 0;
    std::size_t self_references = 0;
    std::size_t duplicate_edges = 0;
    std::size_t degree_violations = 0;
    std::size_t entry_point_violations = 0;
    std::size_t max_level_violations = 0;
    std::size_t insertion_count_violations = 0;
    bool usable = true;

    bool valid() const noexcept;
};

// The caller must keep store alive longer than the index. Append to the store
// only between index operations; reads and writes must never overlap. No spans
// are cached. Diagnostic node pointers are read-only and must not be retained
// across insertions or index destruction.
class HnswIndex {
   public:
    explicit HnswIndex(const VectorStore& store, HnswConfig config = {});

    HnswIndex(const HnswIndex&) = delete;
    HnswIndex& operator=(const HnswIndex&) = delete;
    HnswIndex(HnswIndex&&) = delete;
    HnswIndex& operator=(HnswIndex&&) = delete;

    // Missing/duplicate IDs are rejected without changing the graph. If an
    // exception occurs after topology mutation starts, the index is unusable:
    // discard/rebuild it. Subsequent insert/search calls throw logic_error.
    // VectorStore contents are never modified, and no rollback is claimed.
    void insert(VectorId id);

    // k and ef_search must be positive. Queries are validated even when the
    // index is empty.
    // Search-only errors do not invalidate the index. Results use the store's
    // distance metric, ordered by distance and then ID to resolve ties. Search
    // returns at most min(k, size()) nodes reachable through the graph.
    std::vector<SearchResult> search(std::span<const EmbeddingValue> query, std::size_t k) const;
    std::vector<SearchResult> search(std::span<const EmbeddingValue> query, std::size_t k,
                                     std::size_t ef_search) const;

    std::size_t size() const noexcept;
    bool contains(VectorId id) const;
    const HnswConfig& config() const noexcept;
    std::optional<VectorId> entry_point() const noexcept;
    std::optional<std::size_t> max_level() const noexcept;
    const HnswNode* find_node(VectorId id) const;
    HnswGraphValidation validate_graph() const;

   private:
    using Candidate = std::pair<float, VectorId>;

    const VectorStore& store_;
    const HnswConfig config_;
    std::unordered_map<VectorId, HnswNode> nodes_;
    std::optional<VectorId> entry_point_;
    std::size_t max_level_ = 0;
    std::size_t successful_insertions_ = 0;
    bool usable_ = true;
    std::mt19937_64 random_engine_;

    void ensure_usable() const;
    std::size_t random_level();
    std::size_t connection_limit(std::size_t level) const noexcept;
    float distance_to(std::span<const EmbeddingValue> query, VectorId id) const;
    std::vector<Candidate> search_layer(std::span<const EmbeddingValue> query,
                                        const std::vector<VectorId>& entry_points, std::size_t ef,
                                        std::size_t level) const;
    std::vector<VectorId> select_neighbors(VectorId id, std::vector<Candidate> candidates,
                                           std::size_t limit) const;
    void connect(VectorId a, VectorId b, std::size_t level);
    void prune_neighbors(VectorId id, std::size_t level);
};

}  // namespace aetherindex
