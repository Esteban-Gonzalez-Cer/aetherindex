
#ifndef AETHERINDEX_HNSW_NODE_H
#define AETHERINDEX_HNSW_NODE_H

#include "aether/storage/vector_store.hpp"

#include <vector>

namespace aetherindex {

struct HNSWNode {
    VectorId vector_id;
    std::vector<VectorId> neighbors;

    explicit HNSWNode(VectorId id)
        : vector_id(id) {}
};

}  // namespace aetherindex

#endif
