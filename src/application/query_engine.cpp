#include "aether/application/query_engine.hpp"

#include "aether/ann/brute_force/brute_force_index.hpp"
#include "aether/ann/hnsw/hnsw_index.hpp"

namespace aetherindex {

std::vector<SearchResult> QueryEngine::search(const VectorStore& database,
                                              std::span<const EmbeddingValue> query,
                                              std::size_t k) const {
    if (k == 0 || database.size() == 0) return {};
    const PreparedQuery prepared(query, database.dimension(), database.distance_kind());
    return bruteForceSearchPrepared(database, prepared, k);
}

std::vector<SearchResult> QueryEngine::search(const HnswIndex& index,
                                              std::span<const EmbeddingValue> query,
                                              std::size_t k) const {
    const PreparedQuery prepared(query, index.dimension(), index.distance_kind());
    return index.search_prepared(prepared, k, index.config().ef_search);
}

}  // namespace aetherindex
