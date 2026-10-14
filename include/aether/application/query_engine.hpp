#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "aether/core/search_result.hpp"
#include "aether/storage/vector_store.hpp"

namespace aetherindex {

class HnswIndex;

// Application entry point for exact search and a prepared approximate index.
// The caller owns the VectorStore; the engine neither stores nor copies embeddings.
class QueryEngine {
   public:
    std::vector<SearchResult> search(const VectorStore& database,
                                     std::span<const EmbeddingValue> query, std::size_t k) const;
    std::vector<SearchResult> search(const HnswIndex& index, std::span<const EmbeddingValue> query,
                                     std::size_t k) const;
};

}  // namespace aetherindex
