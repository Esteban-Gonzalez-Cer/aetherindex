#pragma once

#include <span>

#include "aether/core/types.hpp"

namespace aetherindex {

// Smaller distances are better. Cosine returns 1 - similarity in [0, 2];
// SquaredEuclidean returns the sum of squared differences, without a root.
// Throws invalid_argument for unsupported metrics, empty/unequal dimensions,
// nonfinite coordinates, or a zero norm with cosine. Throws overflow_error
// when a squared Euclidean result cannot fit in a finite float.
float compute_distance(std::span<const EmbeddingValue> a, std::span<const EmbeddingValue> b,
                       DistanceKind metric);

}  // namespace aetherindex
