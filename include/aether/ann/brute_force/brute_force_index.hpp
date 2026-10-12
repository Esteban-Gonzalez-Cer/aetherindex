#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "aether/core/search_result.hpp"
#include "aether/storage/vector_store.hpp"

namespace aetherindex {

// Returns at most k neighbors, sorted by ascending distance using the store's metric.
// k == 0 or an empty database returns no results without checking dimensions.
// Otherwise, invalid queries propagate the distance module's exceptions.
// The relative order of equal-distance neighbors is unspecified.
std::vector<SearchResult> bruteForceSearch(const VectorStore& database,
                                           std::span<const EmbeddingValue> query, std::size_t k);

}  // namespace aetherindex
