#pragma once

#include <cstddef>

#include "aether/core/types.hpp"

namespace aetherindex::detail {

// Hot-path contract: equal lengths, finite components, zero padding, and unit
// vectors for cosine. Callers validate/normalize once, outside comparisons.
using DistanceKernel = float (*)(const float*, const float*, std::size_t);
float cosine_unit_kernel(const float* a, const float* b, std::size_t length);
float squared_euclidean_kernel(const float* a, const float* b, std::size_t length);
DistanceKernel distance_kernel(DistanceKind metric);
const char* distance_kernel_backend() noexcept;

}  // namespace aetherindex::detail
