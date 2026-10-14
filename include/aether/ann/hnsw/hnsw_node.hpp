#pragma once

#include <cstddef>
#include <vector>

#include "aether/core/types.hpp"

namespace aetherindex {

// The graph owns topology only. Embeddings remain owned by VectorStore.
struct HnswNode {
    VectorId id;
    std::size_t max_level;
    std::vector<std::vector<VectorId>> neighbors;

    HnswNode(VectorId node_id, std::size_t level)
        : id(node_id), max_level(level), neighbors(level + 1) {}
};

}  // namespace aetherindex
