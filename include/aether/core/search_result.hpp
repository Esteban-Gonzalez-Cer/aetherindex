#pragma once

#include "aether/core/types.hpp"

namespace aetherindex {

// Distance uses the store's configured metric: smaller values are better.
struct SearchResult {
    VectorId id;
    float distance;
};

}  // namespace aetherindex
