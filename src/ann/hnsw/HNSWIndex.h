
#ifndef AETHERINDEX_HNSW_INDEX_H
#define AETHERINDEX_HNSW_INDEX_H

#include "HNSWNode.h"
#include "aether/storage/vector_store.hpp"

#include <cstddef>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aetherindex {

class HNSWIndex {
public:
    explicit HNSWIndex(
        const VectorStore& store,
        std::size_t M = 8,
        std::size_t efConstruction = 50,
        std::size_t efSearch = 20
    );

    // El vector ya debe existir en VectorStore.
    void insert(VectorId id);

    std::vector<VectorId> search(
        std::span<const EmbeddingValue> query,
        std::size_t k
    ) const;

    std::size_t size() const noexcept;

private:
    using Candidate = std::pair<float, VectorId>;

    const VectorStore& store_;

    std::size_t M_;
    std::size_t efConstruction_;
    std::size_t efSearch_;

    std::unordered_map<VectorId, HNSWNode> nodes_;

    VectorId entryPoint_{};
    bool hasEntryPoint_ = false;

    float distance(
        std::span<const EmbeddingValue> a,
        std::span<const EmbeddingValue> b
    ) const;

    // Explora un nivel del grafo desde uno o varios puntos iniciales.
    std::vector<Candidate> searchLayer(
        std::span<const EmbeddingValue> query,
        const std::vector<VectorId>& entryPoints,
        std::size_t ef
    ) const;

    void connect(VectorId a, VectorId b);

    void pruneNeighbors(VectorId id);
};

}  // namespace aetherindex

#endif
