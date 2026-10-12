#include "aether/application/query_engine.hpp"

#include "aether/ann/brute_force/brute_force_index.hpp"

namespace aetherindex {

std::vector<SearchResult> QueryEngine::search(const VectorStore& database,
                                              std::span<const EmbeddingValue> query,
                                              std::size_t k) const {
    return bruteForceSearch(database, query, k);
}

}  // namespace aetherindex
