#include "aether/ann/brute_force/brute_force_index.hpp"

#include <algorithm>

#include "aether/distance/distance.hpp"

namespace aetherindex {

std::vector<SearchResult> bruteForceSearch(const VectorStore& database,
                                           std::span<const EmbeddingValue> query, std::size_t k) {
    if (k == 0 || database.size() == 0) {
        return {};
    }

    std::vector<SearchResult> results;
    results.reserve(database.size());

    for (VectorId id : database.ids()) {
        const auto embedding = database.find(id);
        const float distance = compute_distance(query, *embedding, database.distance_kind());
        results.push_back({id, distance});
    }

    std::sort(results.begin(), results.end(),
              [](const SearchResult& a, const SearchResult& b) { return a.distance < b.distance; });

    if (results.size() > k) {
        results.resize(k);
    }

    return results;
}

}  // namespace aetherindex
