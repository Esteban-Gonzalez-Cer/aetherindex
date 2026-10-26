#pragma once

#include <cstdint>

namespace aetherindex {

using VectorId = std::uint32_t;
// Document identity is independent of VectorId and of dense index positions.
using DocumentId = std::uint32_t;
using EmbeddingValue = float;
static_assert(sizeof(EmbeddingValue) == 4, "Embeddings require 32-bit floats.");

enum class DistanceKind { Cosine, SquaredEuclidean };

}  // namespace aetherindex
