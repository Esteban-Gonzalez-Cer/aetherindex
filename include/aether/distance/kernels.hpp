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

// Scalar references with the same contract: one sequential accumulator, vectorization
// disabled. They exist to validate and time the SIMD kernels above (KR3.5/KR3.6) under
// the same compiler and flags; the search path never uses them.
float cosine_unit_kernel_scalar(const float* a, const float* b, std::size_t length);
float squared_euclidean_kernel_scalar(const float* a, const float* b, std::size_t length);
DistanceKernel scalar_distance_kernel(DistanceKind metric);

}  // namespace aetherindex::detail
