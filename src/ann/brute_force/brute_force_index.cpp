#include "aether/ann/brute_force/brute_force_index.hpp"

#include <algorithm>
#include <queue>
#include <stdexcept>
#include <utility>

#include "aether/distance/kernels.hpp"

namespace aetherindex {

std::vector<SearchResult> bruteForceSearch(const VectorStore& database,
                                           std::span<const EmbeddingValue> query, std::size_t k) {
    if (k == 0 || database.size() == 0) {
        return {};
    }

    return bruteForceSearchPrepared(
        database, PreparedQuery(query, database.dimension(), database.distance_kind()), k);
}

std::vector<SearchResult> bruteForceSearchPrepared(const VectorStore& database,
                                                   const PreparedQuery& query, std::size_t k) {
    if (k == 0 || database.size() == 0) return {};
    if (query.dimension() != database.dimension() ||
        query.distance_kind() != database.distance_kind()) {
        throw std::invalid_argument("Prepared query does not match VectorStore.");
    }
    k = std::min(k, database.size());
    using Candidate = std::pair<float, VectorId>;
    std::vector<Candidate> buffer;
    buffer.reserve(k);
    std::priority_queue<Candidate> best(std::less<Candidate>{}, std::move(buffer));
    const auto distance = detail::distance_kernel(database.distance_kind());
    const auto ids = database.ids();
    for (std::size_t position = 0; position < ids.size(); ++position) {
        const Candidate candidate{distance(query.data(), database.data_by_position(position),
                                           database.padded_dimension()),
                                  ids[position]};
        if (best.size() < k) {
            best.push(candidate);
        } else if (candidate < best.top()) {
            best.pop();
            best.push(candidate);
        }
    }
    std::vector<SearchResult> results(best.size());
    for (std::size_t i = results.size(); i > 0; --i) {
        results[i - 1] = {best.top().second, best.top().first};
        best.pop();
    }
    return results;
}

}  // namespace aetherindex
