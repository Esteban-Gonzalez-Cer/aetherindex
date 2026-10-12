#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "aether/core/search_result.hpp"
#include "aether/storage/vector_store.hpp"

namespace aetherindex {

// Application entry point for the current dense brute-force implementation.
// The caller owns the VectorStore; the engine neither stores nor copies embeddings.
class QueryEngine {
   public:
    std::vector<SearchResult> search(const VectorStore& database,
                                     std::span<const EmbeddingValue> query, std::size_t k) const;
};

}  // namespace aetherindex
