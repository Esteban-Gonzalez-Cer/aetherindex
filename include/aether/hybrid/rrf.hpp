#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "aether/core/search_result.hpp"
#include "aether/hybrid/rrf_config.hpp"

namespace aetherindex::hybrid {

// Fuses already ordered, document-level rankings. It does not retrieve documents,
// compare original scores, check document existence or map VectorId to DocumentId.
class RrfFusion {
   public:
    explicit RrfFusion(RrfConfig config = {});

    // Inputs run from best to worst, with unique DocumentIds within each list.
    // Shared IDs across lists accumulate two contributions, using ranks from 1.
    // Empty lists are valid. top_k=0 or an internal duplicate anywhere in either
    // list throws invalid_argument, including duplicates beyond the requested K.
    // Returns min(top_k, union size) unique results: score descending, then ID
    // ascending on exact ties. No approximate-equality comparator is used.
    // Reads non-owning spans only during this call; never modifies or retains
    // input storage. Allocation failures propagate without changing this object
    // or inputs. A nonrepresentable score throws overflow_error.
    [[nodiscard]] std::vector<RankedResult> fuse(std::span<const DocumentId> dense_ranking,
                                                 std::span<const DocumentId> lexical_ranking,
                                                 std::size_t top_k) const;

    [[nodiscard]] RrfConfig config() const noexcept;

   private:
    RrfConfig config_;
};

}  // namespace aetherindex::hybrid
