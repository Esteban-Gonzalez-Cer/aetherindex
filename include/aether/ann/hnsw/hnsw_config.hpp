#pragma once

#include <cstddef>
#include <cstdint>

namespace aetherindex {

enum class NeighborSelection { Simple, Heuristic };

struct HnswConfig {
    std::size_t m = 16;
    std::size_t m_max_0 = 32;
    std::size_t ef_construction = 100;
    std::size_t ef_search = 50;
    std::uint64_t random_seed = 42;
    std::size_t max_level = 32;
    NeighborSelection neighbor_selection = NeighborSelection::Heuristic;
    bool extend_candidates = false;
    bool keep_pruned_connections = true;
};

// Throws invalid_argument instead of silently changing invalid parameters.
void validate_hnsw_config(const HnswConfig& config);

}  // namespace aetherindex
