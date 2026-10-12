
#ifndef AETHERINDEX_HNSW_INDEX_H
#define AETHERINDEX_HNSW_INDEX_H

#include "HNSWNode.h"

#include <cstddef>
#include <unordered_map>
#include <vector>

class HNSWIndex {
public:
    explicit HNSWIndex(std::size_t M = 8);

    // Registra un vector y lo incorpora al grafo.
    void insert(
        std::size_t id,
        const std::vector<float>& vector
    );

    // Devuelve los k identificadores más cercanos
    // encontrados mediante el grafo.
    std::vector<std::size_t> search(
        const std::vector<float>& query,
        std::size_t k
    ) const;

    std::size_t size() const;

private:
    std::size_t M;

    std::unordered_map<std::size_t, HNSWNode> nodes;

    // Almacenamiento temporal para este primer prototipo.
    // Después lo conectaremos a tu VectorStore.
    std::unordered_map<
        std::size_t,
        std::vector<float>
    > vectors;

    std::size_t entryPoint;
    bool hasEntryPoint;

    float distance(
        const std::vector<float>& a,
        const std::vector<float>& b
    ) const;

    void connect(
        std::size_t a,
        std::size_t b
    );

    void pruneNeighbors(std::size_t id);
};

#endif
