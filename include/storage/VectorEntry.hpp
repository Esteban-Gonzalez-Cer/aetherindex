
#ifndef AETHERINDEX_VECTOR_ENTRY_HPP
#define AETHERINDEX_VECTOR_ENTRY_HPP

#include <vector>
#include <cstddef>

namespace aetherindex {

struct VectorEntry {
    int id;
    std::vector<float> values;
};

struct SearchResult {
    int id;
    float distance;
};

} // namespace aetherindex

#endif
