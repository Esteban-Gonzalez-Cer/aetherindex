
#ifndef AETHERINDEX_HNSW_NODE_H
#define AETHERINDEX_HNSW_NODE_H

#include <cstddef>
#include <vector>

struct HNSWNode {
    std::size_t vectorId;

    // Conexiones con otros nodos del grafo.
    std::vector<std::size_t> neighbors;

    explicit HNSWNode(std::size_t id)
        : vectorId(id) {}
};

#endif
