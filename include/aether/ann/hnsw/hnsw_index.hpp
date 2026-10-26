#pragma once

#include <cstddef>
#include <optional>
#include <random>
#include <span>
#include <utility>
#include <vector>

#include "aether/ann/hnsw/flat_graph.hpp"
#include "aether/ann/hnsw/hnsw_config.hpp"
#include "aether/ann/hnsw/hnsw_node.hpp"
#include "aether/core/prepared_query.hpp"
#include "aether/core/search_result.hpp"
#include "aether/distance/kernels.hpp"
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

// Reserved graph payload and object size; allocator bookkeeping, embeddings
// and per-operation workspaces are outside this accounting. This is not RSS.
struct GraphMemoryUsage {
    std::size_t layer0_bytes = 0;
    std::size_t upper_bytes = 0;
    std::size_t metadata_bytes = 0;
    std::size_t object_bytes = 0;
    std::size_t upper_slots = 0;
    std::size_t allocation_blocks = 0;
    std::size_t total_bytes() const noexcept {
        return layer0_bytes + upper_bytes + metadata_bytes + object_bytes;
    }
};

// The caller must keep store alive longer than the index. Append to the store
// only between index operations; reads and writes must never overlap. No spans
// are cached. Neighbor spans must be discarded before insertion/reserve.
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
    std::vector<SearchResult> search_prepared(const PreparedQuery& query, std::size_t k,
                                              std::size_t ef_search) const;

    std::size_t size() const noexcept;
    bool contains(VectorId id) const;
    const HnswConfig& config() const noexcept;
    std::optional<VectorId> entry_point() const noexcept;
    std::optional<std::size_t> max_level() const noexcept;
    std::optional<HnswNode> find_node(VectorId id) const;
    // Read-only topology view. External IDs may be sparse or include UINT32_MAX;
    // translate a neighbor with external_id(), never use an external ID as offset.
    std::span<const InternalId> neighbors(VectorId id, std::size_t level) const;
    VectorId external_id(InternalId id) const;
    GraphMemoryUsage memory_usage() const noexcept;
    void reserve(std::size_t vector_capacity);
    std::size_t dimension() const noexcept { return store_.dimension(); }
    DistanceKind distance_kind() const noexcept { return store_.distance_kind(); }
    HnswGraphValidation validate_graph() const;

   private:
    struct Candidate {
        float distance;
        VectorId id;
        InternalId node;
        bool operator<(const Candidate& other) const noexcept {
            return distance < other.distance || (distance == other.distance && id < other.id);
        }
        bool operator>(const Candidate& other) const noexcept { return other < *this; }
    };

    const VectorStore& store_;
    const HnswConfig config_;
    FlatGraphLayer layer0_;
    FlatGraphLayer upper_;
    std::vector<std::uint8_t> levels_;
    std::vector<std::uint32_t> upper_offsets_;
    std::optional<InternalId> entry_point_;
    std::size_t max_level_ = 0;
    std::size_t successful_insertions_ = 0;
    bool usable_ = true;
    std::mt19937_64 random_engine_;
    detail::DistanceKernel distance_;
    static constexpr std::uint8_t absent_level = 255;

    void ensure_usable() const;
    std::size_t random_level();
    std::size_t connection_limit(std::size_t level) const noexcept;
    float distance_to(const float* query, InternalId id) const;
    std::span<const InternalId> neighbors_internal(InternalId id, std::size_t level) const noexcept;
    void set_neighbors(InternalId id, std::size_t level, std::span<const InternalId> neighbors);
    std::vector<Candidate> search_layer(const float* query, InternalId entry_point, std::size_t ef,
                                        std::size_t level) const;
    std::vector<InternalId> select_neighbors(InternalId id, std::vector<Candidate> candidates,
                                             std::size_t limit, std::size_t level) const;
    void add_link(InternalId a, InternalId b, std::size_t level);
};

}  // namespace aetherindex
