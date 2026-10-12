
#ifndef AETHERINDEX_HNSW_NODE_H
#define AETHERINDEX_HNSW_NODE_H

#include "aether/storage/vector_store.hpp"

#include <vector>

namespace aetherindex {

struct HNSWNode {
    VectorId vector_id;

    // neighbors[0]: conexiones del nivel base.
    // neighbors[1]: conexiones del nivel 1, etc.
    std::vector<std::vector<VectorId>> neighbors;

    HNSWNode(VectorId id, int max_level)
        : vector_id(id),
          neighbors(static_cast<std::size_t>(max_level) + 1) {}
};

}  // namespace aetherindex

#endif
