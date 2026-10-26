#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "aether/core/prepared_query.hpp"
#include "aether/core/search_result.hpp"
#include "aether/storage/vector_store.hpp"

namespace aetherindex {

// Returns at most k neighbors, sorted by ascending distance using the store's metric.
// k == 0 or an empty database returns no results without checking dimensions.
// Otherwise, invalid queries propagate the distance module's exceptions.
// Equal distances are ordered by external ID.
std::vector<SearchResult> bruteForceSearch(const VectorStore& database,
                                           std::span<const EmbeddingValue> query, std::size_t k);

// Application/internal entry point; the query was normalized once at entry.
std::vector<SearchResult> bruteForceSearchPrepared(const VectorStore& database,
                                                   const PreparedQuery& query, std::size_t k);

}  // namespace aetherindex
