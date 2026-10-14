#include "aether/application/query_engine.hpp"

#include "aether/ann/brute_force/brute_force_index.hpp"
#include "aether/ann/hnsw/hnsw_index.hpp"

namespace aetherindex {

std::vector<SearchResult> QueryEngine::search(const VectorStore& database,
                                              std::span<const EmbeddingValue> query,
                                              std::size_t k) const {
    return bruteForceSearch(database, query, k);
}

std::vector<SearchResult> QueryEngine::search(const HnswIndex& index,
                                              std::span<const EmbeddingValue> query,
                                              std::size_t k) const {
    return index.search(query, k);
}

}  // namespace aetherindex
