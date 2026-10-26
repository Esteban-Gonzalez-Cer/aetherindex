#pragma once

#include <cstddef>
#include <cstdint>

#include "aether/core/types.hpp"

namespace aetherindex {

// Diagnostic value only; no per-node owning containers. The index stores
// topology in FlatGraph and packed level/offset arrays, not HnswNode objects.
struct HnswNode {
    VectorId id;
    std::uint32_t internal_id;
    std::size_t max_level;
};

}  // namespace aetherindex
