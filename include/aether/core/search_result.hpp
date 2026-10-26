#pragma once

#include "aether/core/types.hpp"

namespace aetherindex {

// Distance uses the store's configured metric: smaller values are better.
struct SearchResult {
    VectorId id;
    float distance;
};

// Document ranking: larger scores are better, ties use document_id ascending.
// Each ID occurs once. Its vector position + 1 is the rank consumed by RRF.
struct RankedResult {
    DocumentId document_id;
    double score;
};

}  // namespace aetherindex
