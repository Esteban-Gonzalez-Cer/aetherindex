
#ifndef AETHERINDEX_BRUTE_FORCE_SEARCH_HPP
#define AETHERINDEX_BRUTE_FORCE_SEARCH_HPP

#include "storage/VectorEntry.hpp"

#include <vector>
#include <cstddef>

namespace aetherindex {

std::vector<SearchResult> bruteForceSearch(
    const std::vector<VectorEntry>& database,
    const std::vector<float>& query,
    std::size_t k
);

} // namespace aetherindex

#endif
